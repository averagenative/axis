/*
 * axis-compat-verdicts.c - the verdict per target and library, with the
 * devices behind it, and the recommendation
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-report.h"

#include <string.h>

#include "compat/axis-compat-private.h"
#include "library/axis-library-name.h"

static const gchar *
text_or_null(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : axis_db_result_get_text(result, row, column);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_is_null(result, row, column))
		axis_db_result_get_int64(result, row, column, &value);
	return value;
}

void
axis_compat_device_verdict_free(AxisCompatDeviceVerdict *verdict)
{
	if (verdict == NULL)
		return;
	g_free(verdict->server_kind);
	g_free(verdict->client_id);
	g_free(verdict->name);
	g_free(verdict->detail);
	g_free(verdict);
}

static void
target_verdict_free(AxisCompatTargetVerdict *tv)
{
	if (tv == NULL)
		return;
	if (tv->devices != NULL)
		g_ptr_array_unref(tv->devices);
	g_free(tv);
}

static void
library_report_free(AxisCompatLibraryReport *library)
{
	if (library == NULL)
		return;
	g_free(library->server_kind);
	g_free(library->key);
	g_free(library->title);
	g_free(library->kind);
	if (library->root_ids != NULL)
		g_array_unref(library->root_ids);
	if (library->root_names != NULL)
		g_ptr_array_unref(library->root_names);
	if (library->folders != NULL)
		g_ptr_array_unref(library->folders);
	if (library->unmapped_folders != NULL)
		g_ptr_array_unref(library->unmapped_folders);
	if (library->suggestions != NULL)
		g_ptr_array_unref(library->suggestions);
	if (library->targets != NULL)
		g_ptr_array_unref(library->targets);
	g_free(library);
}

void
axis_compat_report_free(AxisCompatReport *report)
{
	if (report == NULL)
		return;
	axis_compat_coverage_clear(&report->coverage);
	if (report->libraries != NULL)
		g_ptr_array_unref(report->libraries);
	g_free(report);
}

/* ── Combinations a target needs ────────────────────────────────── */

typedef struct
{
	AxisCompatCombo combo[2];
	guint           n;
	gchar          *owned[4];
	guint           n_owned;
} Needs;

static void
needs_clear(Needs *needs)
{
	guint i;

	for (i = 0; i < needs->n_owned; i++)
		g_free(needs->owned[i]);
	needs->n_owned = 0;
}

static const gchar *
keep(
	Needs *needs,
	gchar *text
){
	if (text != NULL)
		needs->owned[needs->n_owned++] = text;
	return text;
}

/* What the target needs a device to play, for a library with these facts. */
static void
needs_for(
	Needs                    *needs,
	const AxisEstimateTarget *target,
	const AxisCompatLibraryReport *library
){
	const gchar *codec = axis_target_codec_to_string(target->codec);
	const gchar *container = axis_target_container_to_string(target->container);

	memset(needs, 0, sizeof(*needs));
	if (target->media == AXIS_TARGET_AUDIO) {
		needs->combo[0].kind = AXIS_COMPAT_STREAM_AUDIO;
		needs->combo[0].codec = codec;
		needs->combo[0].hdr = -1;
		needs->combo[0].container = container;
		needs->n = 1;
		return;
	}
	if (library->has_sdr || !(library->has_hdr && target->hdr_bit_depth > 0)) {
		AxisCompatCombo *c = &needs->combo[needs->n++];

		c->kind = AXIS_COMPAT_STREAM_VIDEO;
		c->codec = codec;
		c->profile = keep(needs, target->profile != NULL ? g_ascii_strdown(target->profile, -1) : NULL);
		c->bit_depth = target->bit_depth;
		c->res_class = library->res_class;
		c->hdr = 0;
		c->container = container;
	}
	if (library->has_hdr && target->hdr_bit_depth > 0) {
		AxisCompatCombo *c = &needs->combo[needs->n++];

		c->kind = AXIS_COMPAT_STREAM_VIDEO;
		c->codec = codec;
		/* An HDR picture of a codec that has a ten-bit profile uses it. */
		c->profile = target->hdr_bit_depth > target->bit_depth && target->codec == AXIS_TCODEC_HEVC ?
			"main 10" : keep(needs, target->profile != NULL ? g_ascii_strdown(target->profile, -1) : NULL);
		c->bit_depth = target->hdr_bit_depth;
		c->res_class = library->res_class;
		c->hdr = 1;
		c->container = container;
	}
}

/* ── Describing evidence ────────────────────────────────────────── */

gchar *
axis_compat_describe_row(const AxisCompatObsRow *row)
{
	const AxisCompatCombo *c = &row->obs.combo;
	GString *out = g_string_new(c->codec);

	if (c->profile != NULL)
		g_string_append_printf(out, " %s", c->profile);
	if (c->bit_depth > 0)
		g_string_append_printf(out, " %d-bit", c->bit_depth);
	if (c->res_class != NULL)
		g_string_append_printf(out, " %s", c->res_class);
	if (c->hdr == 1)
		g_string_append(out, " HDR");
	if (row->channels > 0)
		g_string_append_printf(out, " %d ch", row->channels);
	if (c->container != NULL)
		g_string_append_printf(out, " in %s", c->container);
	return g_string_free(out, FALSE);
}

static gchar *
describe_verdict(
	AxisCompatVerdict      verdict,
	AxisCompatWhy          why,
	const AxisCompatObsRow *row
){
	g_autofree gchar *what = row != NULL ? axis_compat_describe_row(row) : NULL;
	g_autofree gchar *remote = NULL;

	if (row != NULL && row->obs.remote_plays > 0)
		remote = g_strdup_printf(", %u of them remote", row->obs.remote_plays);
	if (row != NULL && verdict == AXIS_VERDICT_SAFE) {
		return g_strdup_printf("%s %s (%u play%s%s)", row->obs.effect == AXIS_EFFECT_PASSTHROUGH ?
			"Seen passing through" : "Seen playing directly", what, row->obs.plays, row->obs.plays == 1 ? "" : "s",
			remote != NULL ? remote : "");
	}
	if (row != NULL && verdict == AXIS_VERDICT_RISKY && row->obs.property != AXIS_PROPERTY_NONE) {
		return g_strdup_printf("The server converted %s because the device did not take %s (%u play%s%s)", what,
			axis_compat_property_label(row->obs.property), row->obs.plays, row->obs.plays == 1 ? "" : "s",
			remote != NULL ? remote : "");
	}
	if (row != NULL && verdict == AXIS_VERDICT_RISKY) {
		return g_strdup_printf("The server converted %s%s%s (%u play%s%s)", what,
			row->out_codec != NULL ? " to " : "", row->out_codec != NULL ? row->out_codec : "", row->obs.plays,
			row->obs.plays == 1 ? "" : "s", remote != NULL ? remote : "");
	}
	return g_strdup(axis_compat_why_label(why));
}

/* ── Libraries ──────────────────────────────────────────────────── */

typedef struct
{
	GHashTable *by_device;	/* axis_compat_device_key() -> GPtrArray of AxisCompatObsRow */
	GHashTable *maps;	/* kind -> GPtrArray of AxisCompatPathEntry */
	GPtrArray  *roots;
	GHashTable *root_names;	/* root id (gint64 *) -> name */
	AxisEstimateSnapshot *snapshot;
	GArray     *targets;	/* AxisEstimateTarget: every profile's current revision */
} Context;

static gint
rank_verdict(AxisCompatVerdict v)
{
	return v == AXIS_VERDICT_RISKY ? 0 : (v == AXIS_VERDICT_UNKNOWN ? 1 : 2);
}

static gint
compare_device_verdicts(
	gconstpointer a,
	gconstpointer b
){
	const AxisCompatDeviceVerdict *x = *(AxisCompatDeviceVerdict *const *)a;
	const AxisCompatDeviceVerdict *y = *(AxisCompatDeviceVerdict *const *)b;
	gint r = rank_verdict(x->verdict) - rank_verdict(y->verdict);

	if (r != 0)
		return r;
	r = g_strcmp0(x->name, y->name);
	if (r != 0)
		return r;
	r = g_strcmp0(x->client_id, y->client_id);
	return r != 0 ? r : g_strcmp0(x->server_kind, y->server_kind);
}

static gboolean
library_facts(
	AxisDbConn              *conn,
	AxisCompatLibraryReport *library,
	GError                 **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GString *ids = g_string_new("{");
	g_autofree gchar *literal = NULL;
	const gchar *params[1];
	gint64 files;
	gint64 probed;
	gint64 rank;
	gint64 odd;
	gint64 hdr;
	gint64 sdr;
	gint64 uncertain;
	gboolean complete;
	guint i;

	for (i = 0; i < library->root_ids->len; i++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(library->root_ids, gint64, i));
	g_string_append_c(ids, '}');
	literal = g_string_free(ids, FALSE);
	params[0] = literal;
	/* A file whose probe is uncertain of its HDR class but that is what Axis
	 * made from a source the probe was certain was SDR, with a plan that
	 * kept SDR (conversion_known_sdr, 0046_conversion_runs.sql), is SDR: the
	 * probe cannot tell an untagged 10-bit picture from Dolby Vision profile
	 * 5, but Axis knows what it wrote.  Without this a root's first
	 * replacement by a 10-bit profile would make the root "possibly HDR" and
	 * the verdict unknown, refusing every later replacement.  A file Axis did
	 * not make keeps the probe's caution. */
	if (!axis_db_conn_exec(conn,
	        "SELECT count(*), count(*) FILTER (WHERE m.status = 'ok' AND m.video_codec IS NOT NULL),"
	        " coalesce(max(CASE m.resolution_class WHEN 'sd' THEN 1 WHEN '720p' THEN 2 WHEN '1080p' THEN 3"
	        "  WHEN '2160p' THEN 4 ELSE 0 END) FILTER (WHERE m.status = 'ok'), 0),"
	        " count(*) FILTER (WHERE m.status = 'ok' AND m.video_codec IS NOT NULL"
	        "  AND (m.resolution_class IS NULL OR m.resolution_class = 'other')),"
	        " count(*) FILTER (WHERE m.status = 'ok' AND m.hdr_format IN ('hdr10', 'hdr10plus', 'hlg', 'dolby_vision')),"
	        " count(*) FILTER (WHERE m.status = 'ok' AND ((m.hdr_format = 'sdr' AND NOT m.hdr_uncertain)"
	        "  OR (m.video_codec IS NOT NULL AND (m.hdr_uncertain OR m.hdr_format IS NULL) AND k.file_id IS NOT NULL))),"
	        " count(*) FILTER (WHERE m.status = 'ok' AND m.video_codec IS NOT NULL"
	        "  AND (m.hdr_uncertain OR m.hdr_format IS NULL) AND k.file_id IS NULL)"
	        " FROM library_file f LEFT JOIN media_info m ON m.file_id = f.id"
	        " LEFT JOIN conversion_known_sdr k ON k.file_id = f.id"
	        " WHERE f.root_id = ANY($1::bigint[]) AND f.kind = 'video' AND f.state <> 'missing'",
	        1, params, &result, error))
		return FALSE;
	files = int_at(result, 0, 0);
	probed = int_at(result, 0, 1);
	rank = int_at(result, 0, 2);
	odd = int_at(result, 0, 3);
	hdr = int_at(result, 0, 4);
	sdr = int_at(result, 0, 5);
	uncertain = int_at(result, 0, 6);
	/* Anything not known is taken as the hardest case. */
	complete = files > 0 && files == probed;
	library->probed = complete;
	library->res_class = !complete || odd > 0 || rank <= 0 ? "2160p" :
		(rank == 1 ? "sd" : (rank == 2 ? "720p" : (rank == 3 ? "1080p" : "2160p")));
	library->has_hdr = !complete || hdr > 0 || uncertain > 0;
	library->has_sdr = !complete || sdr > 0 || uncertain > 0;
	return TRUE;
}

/**
 * library_devices:
 * @libraries: (element-type AxisCompatLibraryReport): the library and the
 *   libraries of the other kinds of server that map to a root it maps to
 *
 * The devices that count toward a library: those that played it, or a
 * library of another server that maps to the same root, within the window
 * (a root is judged by every device that plays it, whichever server it
 * plays it from); those pinned; and, for a server whose sessions do not
 * name their library, those whose session's library could not be told.
 */
static gboolean
library_devices(
	AxisDbConn   *conn,
	GPtrArray    *libraries,
	const gchar  *lookback,
	GPtrArray   **devices,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kinds = NULL;
	g_autofree gchar *keys = NULL;
	g_autofree gchar *blind = NULL;
	g_autoptr(GPtrArray) kind_list = g_ptr_array_new();
	g_autoptr(GPtrArray) key_list = g_ptr_array_new();
	g_autoptr(GPtrArray) blind_list = g_ptr_array_new();
	const gchar *params[4];
	guint rows;
	guint i;

	for (i = 0; i < libraries->len; i++) {
		const AxisCompatLibraryReport *l = g_ptr_array_index(libraries, i);
		const AxisCompatKind *kind = axis_compat_kind_find(l->server_kind);
		guint j;
		gboolean seen = FALSE;

		g_ptr_array_add(kind_list, l->server_kind);
		g_ptr_array_add(key_list, l->key);
		for (j = 0; kind != NULL && kind->library_from_path && j < blind_list->len; j++)
			seen = seen || g_strcmp0(g_ptr_array_index(blind_list, j), l->server_kind) == 0;
		if (kind != NULL && kind->library_from_path && !seen)
			g_ptr_array_add(blind_list, l->server_kind);
	}
	kinds = axis_compat_array_literal(kind_list);
	keys = axis_compat_array_literal(key_list);
	blind = axis_compat_array_literal(blind_list);
	params[0] = kinds;
	params[1] = keys;
	params[2] = lookback;
	params[3] = blind;
	*devices = NULL;
	/* Played within the window, or always considered. */
	if (!axis_db_conn_exec(conn,
	        "WITH libs AS (SELECT kind, key FROM unnest($1::text[], $2::text[]) AS u(kind, key)),"
	        " p AS (SELECT pl.server_kind, pl.device_id, pl.viewed_at AS t FROM compat_play pl"
	        "  WHERE EXISTS (SELECT 1 FROM libs WHERE libs.kind = pl.server_kind AND libs.key = pl.library_key)"
	        "  AND pl.viewed_at >= now() - $3::double precision * interval '1 second'"
	        " UNION ALL SELECT s.server_kind, s.device_id, s.last_seen FROM compat_session s"
	        "  WHERE (EXISTS (SELECT 1 FROM libs WHERE libs.kind = s.server_kind AND libs.key = s.library_key)"
	        "   OR (s.library_key IS NULL AND s.server_kind = ANY($4::text[])))"
	        "  AND s.last_seen >= now() - $3::double precision * interval '1 second')"
	        " SELECT d.client_id, d.name, d.pinned, count(p.t), coalesce((extract(epoch FROM max(p.t)) * 1000000)::bigint, 0),"
	        " d.server_kind FROM compat_device d"
	        " LEFT JOIN p ON p.device_id = d.client_id AND p.server_kind = d.server_kind"
	        " WHERE d.pinned OR p.device_id IS NOT NULL GROUP BY d.server_kind, d.client_id, d.name, d.pinned"
	        " ORDER BY d.name, d.client_id, d.server_kind",
	        4, params, &result, error))
		return FALSE;
	rows = axis_db_result_get_n_rows(result);
	*devices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_verdict_free);
	for (i = 0; i < rows; i++) {
		AxisCompatDeviceVerdict *d = g_new0(AxisCompatDeviceVerdict, 1);
		gboolean pinned = FALSE;

		d->client_id = g_strdup(axis_db_result_get_text(result, i, 0));
		d->name = g_strdup(text_or_null(result, i, 1));
		axis_db_result_get_boolean(result, i, 2, &pinned);
		d->pinned = pinned;
		d->plays = (guint)int_at(result, i, 3);
		d->last_play_us = int_at(result, i, 4);
		d->server_kind = g_strdup(axis_db_result_get_text(result, i, 5));
		g_ptr_array_add(*devices, d);
	}
	return TRUE;
}

/* The device's verdict for a target: the worst over what the target needs. */
static void
judge_device(
	const Needs             *needs,
	GPtrArray               *rows,
	AxisCompatDeviceVerdict *out
){
	AxisCompatObservation *obs = NULL;
	AxisCompatVerdict worst = AXIS_VERDICT_SAFE;
	AxisCompatWhy worst_why = AXIS_WHY_DIRECT;
	const AxisCompatObsRow *worst_row = NULL;
	guint n = rows != NULL ? rows->len : 0;
	guint i;
	guint c;
	gboolean first = TRUE;

	obs = g_new0(AxisCompatObservation, n + 1);
	for (i = 0; i < n; i++) {
		const AxisCompatObsRow *row = g_ptr_array_index(rows, i);

		obs[i] = row->obs;
		out->local_evidence += row->obs.plays - row->obs.remote_plays;
		out->remote_evidence += row->obs.remote_plays;
	}
	for (c = 0; c < needs->n; c++) {
		AxisCompatWhy why = AXIS_WHY_NO_EVIDENCE;
		gint deciding = -1;
		AxisCompatVerdict v = axis_compat_device_verdict(&needs->combo[c], obs, n, &why, &deciding);

		if (first || rank_verdict(v) < rank_verdict(worst)) {
			worst = v;
			worst_why = why;
			worst_row = deciding >= 0 ? g_ptr_array_index(rows, (guint)deciding) : NULL;
		}
		first = FALSE;
	}
	out->verdict = worst;
	out->why = worst_why;
	out->detail = describe_verdict(worst, worst_why, worst_row);
	g_free(obs);
}

/* The estimated saving of a target in a library: the figures of its roots
 * the target is for; -1 without estimates. */
static gint64
library_saving(
	const Context                 *ctx,
	const AxisEstimateTarget      *target,
	const AxisCompatLibraryReport *library
){
	gint64 total = 0;
	guint i;

	if (ctx->snapshot == NULL)
		return -1;
	for (i = 0; i < library->root_ids->len; i++) {
		gint64 id = g_array_index(library->root_ids, gint64, i);
		g_autofree gchar *dim = g_strdup_printf("%" G_GINT64_FORMAT, id);
		const AxisCompatRootRef *root = NULL;
		const AxisEstimateFact *fact;
		AxisMediaType type;
		guint j;

		for (j = 0; j < ctx->roots->len; j++) {
			const AxisCompatRootRef *r = g_ptr_array_index(ctx->roots, j);

			if (r->id == id)
				root = r;
		}
		if (root == NULL || !axis_media_type_from_string(root->media_type, &type) ||
		    (target->root_types & AXIS_TARGET_ROOT(type)) == 0)
			continue;
		fact = axis_estimate_snapshot_fact(ctx->snapshot, target->id, "root", dim);
		if (fact != NULL && fact->bytes > fact->mid)
			total += fact->bytes - fact->mid;
	}
	return total;
}

static gboolean
target_applies(
	const Context                 *ctx,
	const AxisEstimateTarget      *target,
	const AxisCompatLibraryReport *library
){
	guint i;

	for (i = 0; i < library->root_ids->len; i++) {
		gint64 id = g_array_index(library->root_ids, gint64, i);
		guint j;

		for (j = 0; j < ctx->roots->len; j++) {
			const AxisCompatRootRef *r = g_ptr_array_index(ctx->roots, j);
			AxisMediaType type;

			if (r->id == id && axis_media_type_from_string(r->media_type, &type) &&
			    (target->root_types & AXIS_TARGET_ROOT(type)) != 0)
				return TRUE;
		}
	}
	return FALSE;
}

static gboolean
has_video_root(
	const Context                 *ctx,
	const AxisCompatLibraryReport *library
){
	guint i;

	for (i = 0; i < ctx->targets->len; i++) {
		const AxisEstimateTarget *target = &g_array_index(ctx->targets, AxisEstimateTarget, i);

		if (target->media == AXIS_TARGET_VIDEO && target_applies(ctx, target, library))
			return TRUE;
	}
	return FALSE;
}

static gboolean
roots_overlap(
	const AxisCompatLibraryReport *a,
	const AxisCompatLibraryReport *b
){
	guint i;
	guint j;

	if (a->root_ids == NULL || b->root_ids == NULL)
		return FALSE;
	for (i = 0; i < a->root_ids->len; i++) {
		for (j = 0; j < b->root_ids->len; j++) {
			if (g_array_index(a->root_ids, gint64, i) == g_array_index(b->root_ids, gint64, j))
				return TRUE;
		}
	}
	return FALSE;
}

static gboolean
judge_library(
	AxisDbConn              *conn,
	const Context           *ctx,
	const gchar             *lookback,
	GPtrArray               *all,
	AxisCompatLibraryReport *library,
	GError                 **error
){
	const AxisEstimateTarget *targets = (const AxisEstimateTarget *)ctx->targets->data;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) served = g_ptr_array_new();
	g_autofree AxisCompatChoice *choices = g_new0(AxisCompatChoice, MAX(ctx->targets->len, 1));
	guint applicable = 0;
	guint t;
	guint i;

	library->res_class = "2160p";
	library->has_sdr = TRUE;
	library->has_hdr = TRUE;
	library->targets = g_ptr_array_new_with_free_func((GDestroyNotify)target_verdict_free);
	library->recommended = library->best = library->next = -1;
	if (library->state != AXIS_MAP_MAPPED)
		return TRUE;
	if (has_video_root(ctx, library) && !library_facts(conn, library, error))
		return FALSE;
	/* A root another kind of server also serves is judged by the devices
	 * of both. */
	g_ptr_array_add(served, library);
	for (i = 0; i < all->len; i++) {
		AxisCompatLibraryReport *other = g_ptr_array_index(all, i);

		if (other != library && other->state == AXIS_MAP_MAPPED &&
		    g_strcmp0(other->server_kind, library->server_kind) != 0 && roots_overlap(library, other))
			g_ptr_array_add(served, other);
	}
	if (!library_devices(conn, served, lookback, &devices, error))
		return FALSE;
	library->n_devices = devices->len;
	for (t = 0; t < ctx->targets->len; t++) {
		AxisCompatTargetVerdict *tv;
		Needs needs;
		guint d;

		if (!target_applies(ctx, &targets[t], library))
			continue;
		tv = g_new0(AxisCompatTargetVerdict, 1);
		tv->target = targets[t];
		tv->devices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_verdict_free);
		tv->saving = library_saving(ctx, &targets[t], library);
		needs_for(&needs, &targets[t], library);
		for (d = 0; d < devices->len; d++) {
			const AxisCompatDeviceVerdict *src = g_ptr_array_index(devices, d);
			AxisCompatDeviceVerdict *dv = g_new0(AxisCompatDeviceVerdict, 1);

			g_autofree gchar *device_key = axis_compat_device_key(src->server_kind, src->client_id);

			dv->server_kind = g_strdup(src->server_kind);
			dv->client_id = g_strdup(src->client_id);
			dv->name = g_strdup(src->name);
			dv->pinned = src->pinned;
			dv->plays = src->plays;
			dv->last_play_us = src->last_play_us;
			judge_device(&needs, g_hash_table_lookup(ctx->by_device, device_key), dv);
			switch (dv->verdict) {
			case AXIS_VERDICT_SAFE:
				tv->n_safe++;
				break;
			case AXIS_VERDICT_RISKY:
				tv->n_risky++;
				break;
			default:
				tv->n_unknown++;
				break;
			}
			g_ptr_array_add(tv->devices, dv);
		}
		needs_clear(&needs);
		g_ptr_array_sort(tv->devices, compare_device_verdicts);
		/* No device at all, or any that is not safe, is not safe. */
		tv->verdict = tv->devices->len == 0 ? AXIS_VERDICT_UNKNOWN :
			(tv->n_risky > 0 ? AXIS_VERDICT_RISKY : (tv->n_unknown > 0 ? AXIS_VERDICT_UNKNOWN : AXIS_VERDICT_SAFE));
		choices[applicable].verdict = tv->verdict;
		choices[applicable].saving = tv->saving;
		applicable++;
		g_ptr_array_add(library->targets, tv);
	}
	library->recommended = axis_compat_recommend(choices, applicable, &library->best, &library->next);
	return TRUE;
}

/* The path map of each kind of server. */
static gboolean
load_maps(
	AxisDbConn  *conn,
	GHashTable **maps,
	GError     **error
){
	guint i;

	*maps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	for (i = 0; axis_compat_kind_at(i) != NULL; i++) {
		const gchar *kind = axis_compat_kind_at(i)->name;
		GPtrArray *entries = NULL;

		if (!axis_compat_load_path_map(conn, kind, &entries, error))
			return FALSE;
		g_hash_table_insert(*maps, g_strdup(kind), entries);
	}
	return TRUE;
}

gboolean
axis_compat_compute_report(
	AxisCompat         *self,
	AxisCompatReport  **report,
	GError            **error
){
	g_autoptr(AxisDbResult) libraries = NULL;
	g_autoptr(AxisDbResult) names = NULL;
	g_autofree gchar *lookback = g_strdup_printf("%" G_GINT64_FORMAT, self->settings != NULL ?
		axis_settings_get_duration(self->settings, AXIS_COMPAT_SETTING_LOOKBACK) : (gint64)180 * 86400);
	AxisCompatReport *rep = g_new0(AxisCompatReport, 1);
	Context ctx;
	AxisDbConn *conn;
	AxisCompatLibraryReport *current = NULL;
	GPtrArray *folders = NULL;
	gboolean ok = FALSE;
	guint rows;
	guint i;

	*report = NULL;
	memset(&ctx, 0, sizeof(ctx));
	rep->libraries = g_ptr_array_new_with_free_func((GDestroyNotify)library_report_free);
	rep->lookback_seconds = g_ascii_strtoll(lookback, NULL, 10);
	if (!axis_compat_load_coverage(self, &rep->coverage, error))
		goto out;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		goto out;
	ctx.by_device = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	ctx.root_names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	if (!axis_db_conn_begin(conn, error) ||
	    !axis_db_conn_exec(conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", 0, NULL, NULL, error) ||
	    !load_maps(conn, &ctx.maps, error) || !axis_compat_load_roots(conn, &ctx.roots, error) ||
	    !axis_compat_load_observations(conn, NULL, NULL, ctx.by_device, error) ||
	    !axis_db_conn_exec(conn, "SELECT id, name FROM library_root", 0, NULL, &names, error) ||
	    !axis_db_conn_exec(conn,
	        "SELECT l.key, l.kind, l.title, f.folder, l.server_kind FROM compat_library l"
	        " LEFT JOIN LATERAL unnest(l.folders) WITH ORDINALITY AS f(folder, n) ON true"
	        " ORDER BY l.title NULLS LAST, l.key, l.server_kind, f.n",
	        0, NULL, &libraries, error))
		goto out_conn;
	rows = axis_db_result_get_n_rows(names);
	for (i = 0; i < rows; i++) {
		gint64 *id = g_new(gint64, 1);

		*id = int_at(names, i, 0);
		g_hash_table_insert(ctx.root_names, id, g_strdup(axis_db_result_get_text(names, i, 1)));
	}
	if (self->estimator != NULL) {
		g_autoptr(GError) local = NULL;

		if (!axis_estimator_load_snapshot(self->estimator, NULL, &ctx.snapshot, &local))
			g_message("compat: the savings estimates could not be read: %s", local->message);
		else if (ctx.snapshot != NULL) {
			rep->have_estimates = TRUE;
			rep->estimates_us = ctx.snapshot->taken_us;
		}
	}
	/* Every profile is judged (a profile whose definition is not usable
	 * gives no target, so no verdict); without the estimator, the built-ins
	 * as the code has them. */
	if (self->estimator != NULL) {
		g_autoptr(GError) local = NULL;

		if (!axis_estimator_profile_targets(self->estimator, FALSE, &ctx.targets, &local)) {
			g_propagate_error(error, g_steal_pointer(&local));
			goto out_conn;
		}
	} else {
		ctx.targets = g_array_new(FALSE, TRUE, sizeof(AxisEstimateTarget));
		g_array_append_vals(ctx.targets, axis_estimate_builtin_targets(), AXIS_ESTIMATE_N_TARGETS);
	}

	rows = axis_db_result_get_n_rows(libraries);
	for (i = 0; i <= rows; i++) {
		const gchar *key = i < rows ? axis_db_result_get_text(libraries, i, 0) : NULL;
		const gchar *kind_name = i < rows ? axis_db_result_get_text(libraries, i, 4) : NULL;

		if (current != NULL && (key == NULL || g_strcmp0(key, current->key) != 0 ||
		    g_strcmp0(kind_name, current->server_kind) != 0)) {
			AxisCompatMapping mapping;
			GPtrArray *map = g_hash_table_lookup(ctx.maps, current->server_kind);
			guint k;

			axis_compat_map_library(folders, map, ctx.roots, &mapping);
			current->folders = g_ptr_array_new_with_free_func(g_free);
			for (k = 0; k < folders->len; k++)
				g_ptr_array_add(current->folders, g_strdup(g_ptr_array_index(folders, k)));
			current->state = mapping.state;
			current->root_ids = g_steal_pointer(&mapping.root_ids);
			current->unmapped_folders = g_steal_pointer(&mapping.unmapped_folders);
			current->suggestions = g_steal_pointer(&mapping.suggestions);
			current->root_names = g_ptr_array_new_with_free_func(g_free);
			for (k = 0; k < current->root_ids->len; k++) {
				const gchar *name = g_hash_table_lookup(ctx.root_names, &g_array_index(current->root_ids, gint64, k));

				g_ptr_array_add(current->root_names, g_strdup(name != NULL ? name : "?"));
			}
			g_ptr_array_add(rep->libraries, current);
			g_ptr_array_unref(folders);
			current = NULL;
			folders = NULL;
		}
		if (key == NULL)
			break;
		if (current == NULL) {
			current = g_new0(AxisCompatLibraryReport, 1);
			current->server_kind = g_strdup(kind_name);
			current->key = g_strdup(key);
			current->kind = g_strdup(axis_db_result_get_text(libraries, i, 1));
			current->title = g_strdup(text_or_null(libraries, i, 2));
			folders = g_ptr_array_new_with_free_func(g_free);
		}
		if (!axis_db_result_is_null(libraries, i, 3))
			g_ptr_array_add(folders, g_strdup(axis_db_result_get_text(libraries, i, 3)));
	}
	/* Every library is mapped before any is judged: a root's verdict
	 * looks at every server's library that maps to it. */
	for (i = 0; i < rep->libraries->len; i++) {
		if (!judge_library(conn, &ctx, lookback, rep->libraries, g_ptr_array_index(rep->libraries, i), error))
			goto out_conn;
	}
	ok = TRUE;
out_conn:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
out:
	if (ctx.by_device != NULL)
		g_hash_table_unref(ctx.by_device);
	if (ctx.root_names != NULL)
		g_hash_table_unref(ctx.root_names);
	g_clear_pointer(&ctx.maps, g_hash_table_unref);
	g_clear_pointer(&ctx.roots, g_ptr_array_unref);
	g_clear_pointer(&ctx.targets, g_array_unref);
	if (ctx.snapshot != NULL)
		axis_estimate_snapshot_free(ctx.snapshot);
	if (current != NULL)
		library_report_free(current);
	if (!ok)
		axis_compat_report_free(rep);
	else
		*report = rep;
	return ok;
}

void
axis_compat_report_target_counts(
	const AxisCompatReport *report,
	const gchar            *target_id,
	guint                  *safe,
	guint                  *risky,
	guint                  *unknown
){
	guint i;

	*safe = *risky = *unknown = 0;
	for (i = 0; report != NULL && i < report->libraries->len; i++) {
		const AxisCompatLibraryReport *library = g_ptr_array_index(report->libraries, i);
		guint t;

		for (t = 0; t < library->targets->len; t++) {
			const AxisCompatTargetVerdict *tv = g_ptr_array_index(library->targets, t);

			if (strcmp(tv->target.id, target_id) != 0)
				continue;
			switch (tv->verdict) {
			case AXIS_VERDICT_SAFE:
				(*safe)++;
				break;
			case AXIS_VERDICT_RISKY:
				(*risky)++;
				break;
			default:
				(*unknown)++;
				break;
			}
		}
	}
}

AxisCompatVerdict
axis_compat_report_root_verdict(
	const AxisCompatReport *report,
	gint64                  root_id,
	const gchar            *target_id,
	guint                  *libraries
){
	AxisCompatVerdict worst = AXIS_VERDICT_SAFE;
	guint seen = 0;
	guint i;

	for (i = 0; report != NULL && target_id != NULL && i < report->libraries->len; i++) {
		const AxisCompatLibraryReport *library = g_ptr_array_index(report->libraries, i);
		gboolean maps = FALSE;
		guint t;

		for (t = 0; library->root_ids != NULL && t < library->root_ids->len; t++) {
			if (g_array_index(library->root_ids, gint64, t) == root_id)
				maps = TRUE;
		}
		if (!maps || library->targets == NULL)
			continue;
		for (t = 0; t < library->targets->len; t++) {
			const AxisCompatTargetVerdict *tv = g_ptr_array_index(library->targets, t);

			if (strcmp(tv->target.id, target_id) != 0)
				continue;
			worst = axis_compat_verdict_worse(worst, tv->verdict);
			seen++;
		}
	}
	if (libraries != NULL)
		*libraries = seen;
	/* Nothing seen is unknown: never safe by default. */
	return seen > 0 ? worst : AXIS_VERDICT_UNKNOWN;
}

const AxisCompatLibraryReport *
axis_compat_report_find_library(
	const AxisCompatReport *report,
	const gchar            *key
){
	guint i;

	for (i = 0; report != NULL && key != NULL && i < report->libraries->len; i++) {
		const AxisCompatLibraryReport *library = g_ptr_array_index(report->libraries, i);
		g_autofree gchar *handle = axis_compat_handle(library->server_kind, library->key);

		if (strcmp(handle, key) == 0)
			return library;
	}
	return NULL;
}
