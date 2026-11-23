/*
 * axis-acquire-search.c - searching for what a module wants and grabbing a
 * release once: the search-and-grab job's work, and an administrator's
 * grab of a listed release
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One search, for one target:
 *
 *  1. Nothing is asked when it cannot lead anywhere: the module's load()
 *     says it is in flight or not available yet; its profile is unusable,
 *     its destination offline, no download client usable, no indexer
 *     source.  Each of those is the search's outcome (the module's pages say
 *     it).
 *  2. The module's prepare() gives the indexer request and the titles the
 *     names are matched against; every enabled indexer source is asked.
 *  3. Each release name is read (release/axis-release.h) and matched against
 *     those titles: anything but a match to the first is rejected by a
 *     fixed rule.  The module's judge() then says what the release is for
 *     (an identifier of another film, the episodes it holds).  The
 *     blocklist rejects what failed before.
 *  4. The profile decides every release (quality/axis-quality.h), against
 *     the file on disk it would replace (the want's, or with decide_each the
 *     release's own), and ranks the acceptable ones.
 *  5. Every release is stored with its verdict, reasons and rank.
 *  6. The best acceptable release whose protocol has a usable client is
 *     claimed, fetched and sent (grab_release(), below); a release that
 *     cannot be fetched or that the client refuses is put on the blocklist
 *     and the next is tried, up to AXIS_ACQUIRE_MAX_TRIES.
 *
 * The claim (claim()) is the grab-once rule: in one transaction the owner's
 * row is locked, the module's claim_check() reads what is in flight under
 * that lock, and the grab is written (with the module's own rows, claimed())
 * only when nothing it is for is; the module's unique indexes refuse a
 * second open grab besides.  Only then is the release fetched and sent,
 * outside the transaction (a job holds one connection at a time, and none
 * while it waits on the network).
 */

#include "acquire/axis-acquire-private.h"

#include <string.h>

#include "compat/axis-compat.h"
#include "compat/axis-compat-report.h"
#include "quality/axis-quality-store.h"

static void
considered_free(AxisAcquireConsidered *c)
{
	axis_release_free(c->parsed);
	g_free(c->match_text);
	g_free(c->name_key);
	g_free(c->note);
	g_free(c->reject_text);
	if (c->data_free != NULL && c->data != NULL)
		c->data_free(c->data);
	axis_quality_verdict_free(c->own_verdict);
	g_free(c);
}

/* A history line written when the search is finished, after its own. */
typedef struct
{
	gchar  *kind;
	gchar  *text;
	gint64  grab_id;
} Deferred;

static void
deferred_free(Deferred *d)
{
	g_free(d->kind);
	g_free(d->text);
	g_free(d);
}

/* Everything one search holds. */
typedef struct
{
	AxisAcquire           *self;
	AxisAcquireWant       *want;
	const gchar           *cause;
	const gchar           *user;
	GCancellable          *cancellable;
	gint64                 now_us;
	AxisQualityProfile     profile;
	GPtrArray             *blocklist;	/* AxisAcquireBlockKey */
	GPtrArray             *titles;		/* AxisMetaTitle, borrowed from the want */
	AxisIndexerRequest     request;
	guint                  n_compared;
	GPtrArray             *results;		/* AxisIndexerSearchResult */
	GPtrArray             *considered;	/* AxisAcquireConsidered */
	AxisQualityDecision   *decision;	/* (nullable) one decision for them all */
	guint                  n_accepted;	/* by the profile (blocklisted or not) */
	GString               *how;
	gboolean               how_begun;	/* "Asked ...: " was just written */
	GString               *source_errors;
	guint                  n_sources;
	gboolean               usenet_usable;
	gboolean               torrent_usable;
	gint64                 search_id;
	GPtrArray             *deferred;	/* Deferred */
	gchar                 *phrase_no_match;
	gchar                 *phrase_ambiguous;
} Search;

static void
search_clear(Search *s)
{
	axis_acquire_want_free(s->want);
	g_clear_pointer(&s->blocklist, g_ptr_array_unref);
	g_clear_pointer(&s->titles, g_ptr_array_unref);
	g_clear_pointer(&s->results, g_ptr_array_unref);
	g_clear_pointer(&s->considered, g_ptr_array_unref);
	axis_quality_decision_free(s->decision);
	if (s->how != NULL)
		g_string_free(s->how, TRUE);
	if (s->source_errors != NULL)
		g_string_free(s->source_errors, TRUE);
	g_clear_pointer(&s->deferred, g_ptr_array_unref);
	g_free(s->phrase_no_match);
	g_free(s->phrase_ambiguous);
}

static const gchar *
noun(const AxisAcquire *self)
{
	return self->info->noun != NULL ? self->info->noun : "title";
}

/* "film's", "series'" */
static gchar *
possessive(const AxisAcquire *self)
{
	const gchar *n = noun(self);

	return g_strdup_printf("%s%s", n, g_str_has_suffix(n, "s") ? "'" : "'s");
}

static gboolean
protocol_usable(
	const Search         *s,
	AxisDownloadProtocol  protocol
){
	return protocol == AXIS_DOWNLOAD_USENET ? s->usenet_usable : s->torrent_usable;
}

/* ── The household's players, for the players preference ────────── */

typedef struct
{
	GPtrArray          *infos;		/* AxisCompatDeviceInfo */
	GPtrArray          *observations;	/* GArray of AxisCompatObservation */
	GArray             *devices;		/* AxisQualityDevice */
	AxisQualityPlayers  players;
} Players;

static void
players_clear(Players *p)
{
	g_clear_pointer(&p->infos, g_ptr_array_unref);
	g_clear_pointer(&p->observations, g_ptr_array_unref);
	g_clear_pointer(&p->devices, g_array_unref);
}

/* As the Quality page reads them (web/axis-web-quality-pages.c): every
 * device seen playing in the look-back window, or pinned. */
static void
players_read(
	AxisAcquire *self,
	Players     *p
){
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	guint k;

	p->infos = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_info_free);
	p->observations = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
	p->devices = g_array_new(FALSE, TRUE, sizeof(AxisQualityDevice));
	if (self->compat == NULL || !axis_compat_is_enabled(self->compat))
		return;
	if (!axis_compat_list_devices(self->compat, &list, &error)) {
		g_message("%s: the household's players could not be read: %s", self->info->module, error->message);
		return;
	}
	for (i = 0; i < list->len && i < 64; i++) {
		const AxisCompatDeviceInfo *d = g_ptr_array_index(list, i);
		g_autofree gchar *handle = NULL;
		AxisCompatDeviceInfo *info = NULL;
		AxisQualityDevice device;
		GArray *obs;

		if (d->plays_in_window == 0 && !d->pinned)
			continue;
		handle = axis_compat_handle(d->server_kind, d->client_id);
		if (handle == NULL || !axis_compat_get_device(self->compat, handle, &info, NULL) || info == NULL)
			continue;
		g_ptr_array_add(p->infos, info);
		obs = g_array_new(FALSE, TRUE, sizeof(AxisCompatObservation));
		for (k = 0; info->rows != NULL && k < info->rows->len; k++) {
			const AxisCompatObsRow *row = g_ptr_array_index(info->rows, k);

			g_array_append_val(obs, row->obs);
		}
		g_ptr_array_add(p->observations, obs);
		device.obs = (const AxisCompatObservation *)obs->data;
		device.n_obs = obs->len;
		g_array_append_val(p->devices, device);
	}
}

/* ── Closing and recording a grab ───────────────────────────────── */

static gboolean
close_grab(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        grab_id,
	const gchar  *reason,
	const gchar  *text,
	gint64        download_id,
	GError      **error
){
	g_autofree gchar *id = axis_acquire_int_text(grab_id);
	g_autofree gchar *download = download_id > 0 ? axis_acquire_int_text(download_id) : NULL;
	g_autofree gchar *cut = axis_acquire_text_cut(text, 1000);
	g_autofree gchar *sql = axis_acquire_sql(self, "UPDATE %G SET closed_at = now(), close_reason = $2,"
		" close_text = $3, download_id = coalesce($4::bigint, download_id) WHERE id = $1::bigint AND closed_at IS NULL");
	const gchar *params[4];

	params[0] = id;
	params[1] = reason;
	params[2] = cut;
	params[3] = download;
	return axis_db_conn_exec(conn, sql, 4, params, NULL, error);
}

/* What grab_release() is given. */
typedef struct
{
	AxisAcquireGrabSpec  spec;
	GPtrArray           *deferred;	/* (nullable) Deferred: a search's own grabs */
	AxisAcquireWant     *want;
	GCancellable        *cancellable;
} GrabSpec;

static void
run_claim_hook(
	AxisAcquire *self,
	gint64       owner_id
){
	AxisAcquireClaimHook hook;
	gpointer data;

	g_mutex_lock(&self->hook_lock);
	hook = self->claim_hook;
	data = self->claim_hook_data;
	g_mutex_unlock(&self->hook_lock);
	if (hook != NULL)
		hook(owner_id, data);
}

static void
run_replace_hook(
	AxisAcquire *self,
	gint64       owner_id
){
	AxisAcquireClaimHook hook;
	gpointer data;

	g_mutex_lock(&self->hook_lock);
	hook = self->replace_hook;
	data = self->replace_hook_data;
	g_mutex_unlock(&self->hook_lock);
	if (hook != NULL)
		hook(owner_id, data);
}

/* THE CLAIM: the in-flight check and the grab's record, in one transaction
 * with the owner's row locked.  *@result is SENT when the claim was written
 * (the caller fetches and sends next). */
static gboolean
claim(
	AxisAcquire            *self,
	const GrabSpec         *g,
	gint64                 *grab_id,
	AxisAcquireGrabResult  *result,
	GError                **error
){
	const AxisAcquireGrabSpec *spec = &g->spec;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(g->want->owner_id);
	g_autofree gchar *search = spec->search_id > 0 ? axis_acquire_int_text(spec->search_id) : NULL;
	g_autofree gchar *candidate = spec->candidate_id > 0 ? axis_acquire_int_text(spec->candidate_id) : NULL;
	g_autofree gchar *source = axis_acquire_int_text(spec->source_id);
	g_autofree gchar *size = spec->size_bytes >= 0 ? axis_acquire_int_text(spec->size_bytes) : NULL;
	g_autofree gchar *indexer_id = axis_acquire_int_text(spec->indexer_id);
	g_autofree gchar *why = axis_acquire_text_cut(spec->why, 2000);
	g_autofree gchar *hash = spec->info_hash != NULL && axis_download_hash_valid(spec->info_hash) ?
		g_strdup(spec->info_hash) : NULL;
	g_autofree gchar *sql = axis_acquire_sql(self, "INSERT INTO %G (%O, search_id, candidate_id, release_name,"
		" protocol, source_id, indexer, indexer_id, size_bytes, quality, name_key, info_hash, why, manual,"
		" grabbed_by, set_aside, replaces_files, numbering_ok, forced, bound) VALUES ($1::bigint, $2::bigint,"
		" $3::bigint, $4, $5, $6::bigint, $7, $8::bigint, $9::bigint, $10, $11, $12, $13, $14::boolean, $15, $16,"
		" coalesce($17::jsonb, '[]'), $18::boolean, coalesce($19::jsonb, '[]'), $20::boolean) RETURNING id");
	const gchar *params[20];
	AxisDbConn *conn;
	gboolean found = FALSE;
	gboolean gone = FALSE;
	gboolean in_flight = FALSE;
	gboolean ok;

	*result = AXIS_ACQUIRE_GRAB_IN_FLIGHT;
	run_claim_hook(self, g->want->owner_id);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_acquire_store_lock(self, conn, g->want->owner_id, &found, error);
	if (ok && !found) {
		*result = AXIS_ACQUIRE_GRAB_GONE;
		goto done;
	}
	ok = ok && self->info->claim_check(self->data, conn, g->want, spec, &gone, &in_flight, error);
	if (ok && gone) {
		*result = AXIS_ACQUIRE_GRAB_GONE;
		goto done;
	}
	if (ok && in_flight) {
		*result = AXIS_ACQUIRE_GRAB_IN_FLIGHT;
		goto done;
	}
	params[0] = owner;
	params[1] = search;
	params[2] = candidate;
	params[3] = spec->name;
	params[4] = axis_download_protocol_to_string(spec->protocol);
	params[5] = source;
	params[6] = spec->indexer;
	params[7] = indexer_id;
	params[8] = size;
	params[9] = spec->quality;
	params[10] = spec->name_key;
	params[11] = hash;
	params[12] = why;
	params[13] = spec->manual ? "true" : "false";
	params[14] = spec->user;
	/* What a person set aside and ticked: only a person's grab has any. */
	params[15] = spec->manual ? spec->set_aside : NULL;
	params[16] = spec->manual ? spec->replaces_files : NULL;
	params[17] = spec->manual && spec->numbering_ok ? "true" : "false";
	/* By force: the fixed rules set aside, and the grab bound to what the
	 * person named. */
	params[18] = spec->manual ? spec->forced : NULL;
	params[19] = spec->manual && spec->bound ? "true" : "false";
	if (ok && !axis_db_conn_exec(conn, sql, 20, params, &inserted, &local)) {
		/* The database's own guard: an open grab exists after all. */
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			*result = AXIS_ACQUIRE_GRAB_IN_FLIGHT;
			goto done;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		ok = FALSE;
	}
	if (ok)
		axis_db_result_get_int64(inserted, 0, 0, grab_id);
	if (ok && self->info->claimed != NULL && !self->info->claimed(self->data, conn, g->want, spec, *grab_id, &local)) {
		/* The module's own guard (an item it is for has an open grab). */
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			*result = AXIS_ACQUIRE_GRAB_IN_FLIGHT;
			*grab_id = 0;
			goto done;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		ok = FALSE;
	}
	if (ok) {
		ok = axis_db_conn_commit(conn, error);
		if (ok)
			*result = AXIS_ACQUIRE_GRAB_SENT;
	}
done:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* After the claim: what became of it, in one transaction. */
static const gchar *
quality_label(const gchar *name)
{
	AxisQuality q;

	if (name != NULL && axis_quality_from_name(name, &q))
		return axis_quality_label(q);
	return "unknown quality";
}

/* @block: (nullable) the blocklist's reason, when the release goes on it. */
static gboolean
record_outcome(
	AxisAcquire           *self,
	const GrabSpec        *g,
	gint64                 grab_id,
	AxisAcquireGrabResult  result,
	gint64                 download_id,
	const gchar           *hash,
	const gchar           *problem,
	const gchar           *block,
	gboolean               uncertain,
	GError               **error
){
	const AxisAcquireGrabSpec *spec = &g->spec;
	g_autofree gchar *grab = axis_acquire_int_text(grab_id);
	g_autofree gchar *download = download_id > 0 ? axis_acquire_int_text(download_id) : NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autofree gchar *size = spec->size_bytes > 0 ? axis_quality_format_size(spec->size_bytes) : NULL;
	g_autofree gchar *text = NULL;
	const gchar *kind;
	AxisDbConn *conn;
	const gchar *params[3];
	gboolean ok;

	if (result == AXIS_ACQUIRE_GRAB_SENT && download_id > 0)
		axis_downloads_get(self->downloads, download_id, &info, NULL);
	if (result == AXIS_ACQUIRE_GRAB_SENT) {
		kind = "grabbed";
		text = g_strdup_printf("%s %s (%s%s%s%s%s) and sent it to %s%s.%s%s", spec->manual && spec->forced != NULL ?
			"Grabbed by force" : spec->manual ? "Grabbed by hand" : "Grabbed", spec->name, quality_label(spec->quality), size != NULL ? ", " : "", size != NULL ? size : "",
			spec->indexer != NULL ? ", from " : "", spec->indexer != NULL ? spec->indexer : "",
			info != NULL ? info->client_name : "a download client",
			uncertain ? "; whether the client took it is not known yet, and it is not sent again" : "",
			spec->why != NULL ? " " : "", spec->why != NULL ? spec->why : "");
	} else {
		kind = "not_sent";
		text = g_strdup_printf("%s was not sent: %s%s", spec->name, problem != NULL ? problem : "it could not be had",
			block != NULL ? " It is on the blocklist now." : "");
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error);
	if (result == AXIS_ACQUIRE_GRAB_SENT) {
		g_autofree gchar *sql = axis_acquire_sql(self, "UPDATE %G SET download_id = $2::bigint, sent_at = now(),"
			" info_hash = coalesce($3, info_hash) WHERE id = $1::bigint");

		params[0] = grab;
		params[1] = download;
		params[2] = hash != NULL && axis_download_hash_valid(hash) ? hash : NULL;
		ok = ok && axis_db_conn_exec(conn, sql, 3, params, NULL, error);
		if (ok && !spec->manual && g->want->pace_ids->len > 0) {
			/* A grab starts the pacing over. */
			g_autoptr(GString) rows = g_string_new("{");
			g_autofree gchar *floor = axis_acquire_int_text(axis_acquire_setting_duration(self,
				self->info->setting_floor, 6 * 3600));
			g_autofree gchar *pace_sql = axis_acquire_sql(self, "UPDATE %P SET search_floor_s = $2::integer"
				" WHERE id = ANY ($1::bigint[])");
			const gchar *pace[2];
			guint i;

			for (i = 0; i < g->want->pace_ids->len; i++)
				g_string_append_printf(rows, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
					g_array_index(g->want->pace_ids, gint64, i));
			g_string_append_c(rows, '}');
			pace[0] = rows->str;
			pace[1] = floor;
			ok = axis_db_conn_exec(conn, pace_sql, 2, pace, NULL, error);
		}
	} else {
		ok = ok && close_grab(self, conn, grab_id, "not_sent", problem, download_id, error);
		if (ok && block != NULL)
			ok = axis_acquire_store_block(self, conn, g->want->owner_id, spec->name_key,
				hash != NULL ? hash : spec->info_hash, spec->name, spec->indexer, block, problem, grab_id, spec->user,
				error);
	}
	/* A search's own grabs go in the history after the search itself. */
	if (ok && g->deferred != NULL) {
		Deferred *d = g_new0(Deferred, 1);

		d->kind = g_strdup(kind);
		d->text = g_steal_pointer(&text);
		d->grab_id = grab_id;
		g_ptr_array_add(g->deferred, d);
	} else if (ok) {
		ok = self->info->event(self->data, conn, g->want->owner_id, g->want, kind, spec->user, spec->search_id,
			grab_id, text, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gchar *
sentence(
	const gchar *prefix,
	const gchar *message
){
	return g_strdup_printf("%s%s%s", prefix, message != NULL ? message : "",
		message != NULL && g_str_has_suffix(message, ".") ? "" : ".");
}

/**
 * grab_release:
 * @problem: (out) (transfer full): why not, when not sent
 *
 * Claims, fetches and sends one release.  Returns %FALSE only for a
 * database error or cancellation.
 */
static gboolean
grab_release(
	AxisAcquire            *self,
	const GrabSpec         *g,
	AxisAcquireGrabResult  *result,
	gint64                 *grab_id_out,
	gchar                 **problem,
	GError                **error
){
	const AxisAcquireGrabSpec *spec = &g->spec;
	g_autoptr(GError) failure = NULL;
	AxisIndexerFetched fetched;
	AxisDownloadRequest request;
	gint64 grab_id = 0;
	gint64 download_id = 0;
	const gchar *block = NULL;
	gboolean sent;

	*problem = NULL;
	if (!claim(self, g, &grab_id, result, error))
		return FALSE;
	if (*result != AXIS_ACQUIRE_GRAB_SENT) {
		*problem = *result == AXIS_ACQUIRE_GRAB_GONE ? g_strdup_printf("the %s was removed meanwhile", noun(self)) :
			g_strdup_printf("another grab of the %s is in flight", noun(self));
		return TRUE;
	}
	if (grab_id_out != NULL)
		*grab_id_out = grab_id;

	memset(&fetched, 0, sizeof(fetched));
	if (!axis_indexers_fetch(self->indexers, spec->release_id, g->cancellable, &fetched, &failure)) {
		if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
			record_outcome(self, g, grab_id, AXIS_ACQUIRE_GRAB_SOURCE_ERROR, 0, NULL, "the search was stopped", NULL,
				FALSE, NULL);
			g_propagate_error(error, g_steal_pointer(&failure));
			return FALSE;
		}
		if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND)) {
			*result = AXIS_ACQUIRE_GRAB_BAD_RELEASE;
			*problem = g_strdup("its search has expired: search again.");
		} else if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED) ||
		           g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE) ||
		           g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT)) {
			*result = AXIS_ACQUIRE_GRAB_BAD_RELEASE;
			block = "unfetchable";
			*problem = sentence("the indexer would not give it: ", failure->message);
		} else {
			*result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
			*problem = sentence("the indexer could not be fetched from: ", failure->message);
		}
		return record_outcome(self, g, grab_id, *result, 0, NULL, *problem, block, FALSE, error);
	}

	memset(&request, 0, sizeof(request));
	request.client_id = 0;
	request.protocol = fetched.protocol;
	request.file = fetched.file;
	request.magnet = fetched.magnet;
	request.release_name = spec->name;
	request.title_id = g->want->title_id;
	request.title_label = g->want->label;
	request.module = self->info->module;
	sent = axis_downloads_add(self->downloads, &request, &download_id, &failure);
	if (sent || g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN)) {
		/* Uncertain counts as sent: the downloads layer settles it, and the
		 * grab stays in flight until it has. */
		gboolean ok = record_outcome(self, g, grab_id, AXIS_ACQUIRE_GRAB_SENT, download_id, fetched.info_hash, NULL,
			NULL, !sent, error);

		axis_indexer_fetched_clear(&fetched);
		*result = AXIS_ACQUIRE_GRAB_SENT;
		return ok;
	}
	axis_indexer_fetched_clear(&fetched);
	if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_CANCELLED)) {
		record_outcome(self, g, grab_id, AXIS_ACQUIRE_GRAB_CLIENT_ERROR, download_id, NULL, "the search was stopped",
			NULL, FALSE, NULL);
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT)) {
		*result = AXIS_ACQUIRE_GRAB_NO_CLIENT;
		*problem = sentence("no download client takes it: ", failure->message);
	} else if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS)) {
		*result = AXIS_ACQUIRE_GRAB_BAD_RELEASE;
		*problem = sentence("the client has it in progress already, for something else: ", failure->message);
	} else if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
	           g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED)) {
		*result = AXIS_ACQUIRE_GRAB_BAD_RELEASE;
		block = "refused";
		*problem = sentence("the download client refused it: ", failure->message);
	} else {
		*result = AXIS_ACQUIRE_GRAB_CLIENT_ERROR;
		*problem = sentence("the download client could not take it: ", failure->message);
	}
	return record_outcome(self, g, grab_id, *result, download_id, NULL, *problem, block, FALSE, error);
}

/* ── Reading and deciding ───────────────────────────────────────── */

/*
 * What tells a release found by one request of a search from the same release
 * found by another (the requests are kept apart by the indexer layer, each
 * with its own stored releases): the key by which the layer itself knows one
 * release at several indexers (a torrent's info-hash; a Usenet release's exact
 * name and size), else where it came from, its name and its size.
 */
static gchar *
once_key(
	const AxisIndexerRelease *release,
	gint64                    source_id
){
	gchar *group = axis_indexer_release_group_key(release);
	g_autofree gchar *name = NULL;

	if (group != NULL)
		return group;
	name = axis_acquire_name_key(source_id, release->indexer_id, release->indexer_name, release->name);
	return g_strdup_printf("n|%s|%" G_GINT64_FORMAT, name != NULL ? name : "", release->size_bytes);
}

static void
read_releases(Search *s)
{
	g_autoptr(GHashTable) once = s->results->len > 1 ? g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL) :
		NULL;
	g_autofree guint *share = g_new0(guint, MAX(s->results->len, 1));
	guint left = AXIS_INDEXER_MAX_RESULTS;
	guint i;
	guint k;

	/* How many releases of each request are read, of the most a search
	 * considers.  One request: all of them up to that, as always.  Several:
	 * each an equal share first, so that a first request that returns the
	 * most there can be does not leave the others unread, and what the shares
	 * leave over goes to the requests in their order. */
	for (i = 0; i < s->results->len; i++) {
		const AxisIndexerSearchResult *result = g_ptr_array_index(s->results, i);

		share[i] = MIN(result->releases->len, AXIS_INDEXER_MAX_RESULTS / s->results->len);
		left -= share[i];
	}
	for (i = 0; i < s->results->len; i++) {
		const AxisIndexerSearchResult *result = g_ptr_array_index(s->results, i);
		guint more = MIN(result->releases->len - share[i], left);

		share[i] += more;
		left -= more;
	}
	s->n_compared = s->titles->len;
	s->considered = g_ptr_array_new_with_free_func((GDestroyNotify)considered_free);
	for (i = 0; i < s->results->len; i++) {
		const AxisIndexerSearchResult *result = g_ptr_array_index(s->results, i);
		guint taken = 0;

		for (k = 0; k < result->releases->len && taken < share[i] &&
		     s->considered->len < AXIS_INDEXER_MAX_RESULTS; k++) {
			const AxisIndexerRelease *release = g_ptr_array_index(result->releases, k);
			AxisAcquireConsidered *c;

			/* Several requests (an anime series' search): a release that a
			 * later one returns again is listed once, as the first found it. */
			if (once != NULL) {
				gchar *key = once_key(release, release->source_id != 0 ? release->source_id : result->source_id);
				guint first = GPOINTER_TO_UINT(g_hash_table_lookup(once, key));

				/* (Only what an earlier request found: two results of one
				 * request that look alike are both listed, as they always
				 * were.) */
				if (first != 0 && first != i + 1) {
					g_free(key);
					continue;
				}
				if (first == 0)
					g_hash_table_insert(once, key, GUINT_TO_POINTER(i + 1));
				else
					g_free(key);
			}
			c = g_new0(AxisAcquireConsidered, 1);
			c->release = release;
			/* A merged search's releases each say where they came from. */
			c->source_id = release->source_id != 0 ? release->source_id : result->source_id;
			c->matched = -1;
			c->parsed = axis_release_parse(release->name);
			c->match = axis_release_match(c->parsed, (const AxisMetaTitle *const *)s->titles->pdata, s->titles->len,
				&c->matched, &c->match_text);
			/* A match to another title is not what was asked for. */
			if (c->match == AXIS_RELEASE_MATCH_YES && c->matched != 0) {
				g_autofree gchar *said = c->match_text;

				c->match = AXIS_RELEASE_MATCH_NONE;
				c->match_text = g_strdup_printf("it is another %s: it %s", noun(s->self),
					said != NULL ? said : "matches another title");
			} else if (c->match == AXIS_RELEASE_MATCH_YES && s->self->info->judge != NULL) {
				s->self->info->judge(s->self->data, s->want, c);
			} else if (c->match == AXIS_RELEASE_MATCH_NONE && c->matched < 0 && s->self->info->unmatched != NULL) {
				/* None of the titles by its own reading: the module's own
				 * fixed rule, if it has one (an anime series alone). */
				s->self->info->unmatched(s->self->data, s->want, c);
			}
			c->name_key = axis_acquire_name_key(c->source_id, release->indexer_id, release->indexer_name,
				release->name);
			{
				const AxisAcquireBlockKey *block = axis_acquire_blocked_by(s->blocklist, c->name_key,
					release->info_hash);

				c->blocklisted = block != NULL;
				c->block_reason = block != NULL ? block->reason : NULL;
				c->block_by = block != NULL ? block->created_by : NULL;
			}
			g_ptr_array_add(s->considered, c);
			taken++;
		}
	}
}

static void
candidate_input(
	const AxisAcquireConsidered *c,
	AxisQualityCandidate        *input
){
	input->release = c->parsed;
	input->name = c->release->name;
	input->match = c->match;
	input->match_reason = c->match_text;
	input->size_bytes = c->release->size_bytes;
	input->protocol = c->release->protocol;
	input->seeders = c->release->seeders;
	input->published_us = c->release->published_us;
}

/* The accepted best first by the profile's keys (whose last key is the
 * position: equals keep their order). */
static gint
compare_rank(
	gconstpointer a,
	gconstpointer b
){
	const AxisAcquireConsidered *x = *(const AxisAcquireConsidered *const *)a;
	const AxisAcquireConsidered *y = *(const AxisAcquireConsidered *const *)b;

	return axis_quality_keys_compare(&x->verdict->keys, &y->verdict->keys, NULL);
}

/* What every decision about a release shares, whatever the profile: the
 * languages, protocol and retention settings, the time, and the household's
 * players (read once, when a profile asks for them). */
typedef struct
{
	gchar                         **languages;
	AxisQualityProtocolPreference   protocol;
	gint                            retention_days;
	gint64                          now_us;
	Players                         players;
	gboolean                        players_read;
} Around;

static void
around_init(
	AxisAcquire *self,
	gint64       now_us,
	Around      *a
){
	g_autofree gchar *protocol = axis_acquire_setting_string(self, self->info->setting_protocol, "any");

	memset(a, 0, sizeof(*a));
	a->languages = axis_acquire_setting_languages(self);
	a->protocol = g_strcmp0(protocol, "usenet") == 0 ? AXIS_QUALITY_PROTOCOL_USENET :
		g_strcmp0(protocol, "torrent") == 0 ? AXIS_QUALITY_PROTOCOL_TORRENT : AXIS_QUALITY_PROTOCOL_ANY;
	a->retention_days = (gint)axis_acquire_setting_integer(self, self->info->setting_retention, 0);
	a->now_us = now_us;
}

static void
around_clear(Around *a)
{
	g_strfreev(a->languages);
	players_clear(&a->players);
}

/* The context of one decision under @profile: @title's languages are set
 * here. */
static void
around_context(
	Around                   *a,
	AxisAcquire              *self,
	const AxisQualityProfile *profile,
	AxisQualityTitle         *title,
	const AxisQualityOnDisk  *disk,
	AxisQualityContext       *context
){
	title->languages = (const gchar *const *)a->languages;
	memset(context, 0, sizeof(*context));
	context->profile = profile;
	context->title = title;
	context->disk = disk;
	context->protocol = a->protocol;
	context->retention_days = a->retention_days;
	context->now_us = a->now_us;
	if (profile->players > 0 && !a->players_read) {
		players_read(self, &a->players);
		a->players.players.devices = (const AxisQualityDevice *)a->players.devices->data;
		a->players.players.n_devices = a->players.devices->len;
		a->players_read = TRUE;
	}
	if (profile->players > 0 && a->players.devices != NULL && a->players.devices->len > 0) {
		context->players = axis_quality_players_verdict;
		context->players_data = &a->players.players;
	}
}

static void
decide(Search *s)
{
	g_autofree AxisQualityCandidate *input = g_new0(AxisQualityCandidate, MAX(s->considered->len, 1));
	AxisQualityContext context;
	AxisQualityTitle title;
	Around around;
	guint rank = 0;
	guint i;

	around_init(s->self, s->now_us, &around);
	for (i = 0; i < s->considered->len; i++)
		candidate_input(g_ptr_array_index(s->considered, i), &input[i]);
	memset(&title, 0, sizeof(title));
	title.noun = noun(s->self);
	title.runtime_minutes = s->want->runtime_minutes;
	/* With a file in the library a release must be an upgrade over it, by
	 * the same rules its import applies. */
	around_context(&around, s->self, &s->profile, &title, s->want->on_disk ? &s->want->disk : NULL, &context);
	if (!s->self->info->decide_each) {
		s->decision = axis_quality_decide(&context, input, s->considered->len);
		s->n_accepted = s->decision->n_accepted;
		/* The profile's order, without what is on the blocklist or what the
		 * module does not want. */
		for (i = 0; i < s->considered->len; i++) {
			guint index = s->decision->order[i];
			AxisAcquireConsidered *c = g_ptr_array_index(s->considered, index);

			c->verdict = g_ptr_array_index(s->decision->verdicts, index);
			c->accepted = c->verdict->accepted && !c->blocklisted && !c->rejected;
			if (c->accepted)
				c->rank = ++rank;
		}
	} else {
		g_autoptr(GPtrArray) accepted = g_ptr_array_new();

		/* Each release against its own file on disk and its own runtime. */
		for (i = 0; i < s->considered->len; i++) {
			AxisAcquireConsidered *c = g_ptr_array_index(s->considered, i);
			AxisQualityContext own = context;
			AxisQualityTitle own_title = title;

			own_title.runtime_minutes = c->runtime_minutes;
			if (c->noun != NULL)
				own_title.noun = c->noun;
			own.title = &own_title;
			own.disk = c->disk;
			c->own_verdict = axis_quality_decide_one(&own, &input[i]);
			c->own_verdict->keys.index = i;
			c->verdict = c->own_verdict;
			if (c->verdict->accepted)
				s->n_accepted++;
			c->accepted = c->verdict->accepted && !c->blocklisted && !c->rejected;
			if (c->accepted)
				g_ptr_array_add(accepted, c);
		}
		g_ptr_array_sort(accepted, compare_rank);
		for (i = 0; i < accepted->len; i++)
			((AxisAcquireConsidered *)g_ptr_array_index(accepted, i))->rank = i + 1;
	}
	around_clear(&around);
}

static const gchar *
release_quality(const AxisAcquireConsidered *c)
{
	return axis_quality_name(c->verdict != NULL ? c->verdict->quality : axis_quality_of_release(c->parsed));
}

static GPtrArray *
reasons_of(
	const Search                *s,
	const AxisAcquireConsidered *c
){
	GPtrArray *reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_reason_free);
	guint i;

	if (c->blocklisted) {
		g_autofree gchar *whose = possessive(s->self);
		g_autofree gchar *why = axis_acquire_block_words(c->block_reason, c->block_by);
		g_autofree gchar *text = g_strdup_printf("It is on this %s blocklist: %s.", whose, why);

		g_ptr_array_add(reasons, axis_acquire_reason_new("blocklist", "blocklisted", text));
	}
	if (c->rejected)
		g_ptr_array_add(reasons, axis_acquire_reason_new("wanted", c->reject_code != NULL ? c->reject_code :
			"not_wanted", c->reject_text != NULL ? c->reject_text : "It is not wanted."));
	for (i = 0; c->verdict != NULL && i < c->verdict->reasons->len; i++) {
		const AxisQualityReason *r = g_ptr_array_index(c->verdict->reasons, i);

		g_ptr_array_add(reasons, axis_acquire_reason_new(r->kind == AXIS_QUALITY_REASON_SAFETY ? "safety" :
			r->kind == AXIS_QUALITY_REASON_UPGRADE ? "upgrade" : "profile", r->code, r->text));
	}
	return reasons;
}

/* ── Storing the search ─────────────────────────────────────────── */

static gboolean
store_search(
	Search      *s,
	AxisDbConn  *conn,
	const gchar *outcome,
	const gchar *outcome_text,
	GError     **error
){
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(s->want->owner_id);
	g_autofree gchar *sources = axis_acquire_int_text(s->n_sources);
	g_autofree gchar *found = axis_acquire_int_text(s->considered != NULL ? s->considered->len : 0);
	g_autofree gchar *compared = axis_acquire_int_text(s->n_compared);
	g_autofree gchar *how = s->how != NULL && s->how->len > 0 ? axis_acquire_text_cut(s->how->str, 1000) : NULL;
	g_autofree gchar *text = axis_acquire_text_cut(outcome_text, 2000);
	g_autofree gchar *sql = axis_acquire_sql(s->self, "INSERT INTO %S (%O, cause, requested_by, how, n_sources,"
		" n_found, n_compared, outcome, outcome_text, finished_at) VALUES ($1::bigint, $2, $3, $4, $5::integer,"
		" $6::integer, $7::integer, $8, $9, CASE WHEN $8::text IS NULL THEN NULL ELSE now() END) RETURNING id");
	const gchar *params[9];

	params[0] = owner;
	params[1] = s->cause;
	params[2] = s->user;
	params[3] = how;
	params[4] = sources;
	params[5] = found;
	params[6] = compared;
	params[7] = outcome;
	params[8] = text;
	if (!axis_db_conn_exec(conn, sql, 9, params, &inserted, error))
		return FALSE;
	axis_db_result_get_int64(inserted, 0, 0, &s->search_id);
	if (s->self->info->search_stored != NULL &&
	    !s->self->info->search_stored(s->self->data, conn, s->want, s->search_id, error))
		return FALSE;
	return TRUE;
}

static gboolean
store_candidates(
	Search      *s,
	AxisDbConn  *conn,
	GError     **error
){
	g_autofree gchar *search = axis_acquire_int_text(s->search_id);
	g_autofree gchar *sql = axis_acquire_sql(s->self, "INSERT INTO %C (search_id, position, release_id, source_id,"
		" name, protocol, size_bytes, published_at, seeders, indexer, indexer_id, info_hash, name_key, quality, match,"
		" match_text, accepted, safety, blocklisted, rank, score, reasons) VALUES ($1::bigint, $2::integer,"
		" $3::bigint, $4::bigint, $5, $6, $7::bigint, to_timestamp($8::bigint / 1000000.0), $9::integer, $10,"
		" $11::bigint, $12, $13, $14, $15, $16, $17::boolean, $18::boolean, $19::boolean, $20::integer,"
		" $21::integer, $22::jsonb) RETURNING id");
	g_autofree gchar *count_sql = axis_acquire_sql(s->self, "UPDATE %S SET n_accepted = $2::integer"
		" WHERE id = $1::bigint");
	guint accepted = 0;
	guint i;

	for (i = 0; i < s->considered->len; i++) {
		AxisAcquireConsidered *c = g_ptr_array_index(s->considered, i);
		const AxisIndexerRelease *r = c->release;
		g_autoptr(GPtrArray) reasons = reasons_of(s, c);
		g_autoptr(AxisDbResult) inserted = NULL;
		g_autofree gchar *position = axis_acquire_int_text(i);
		g_autofree gchar *release = axis_acquire_int_text(r->id);
		g_autofree gchar *source = axis_acquire_int_text(c->source_id);
		g_autofree gchar *size = r->size_bytes >= 0 ? axis_acquire_int_text(r->size_bytes) : NULL;
		g_autofree gchar *published = r->published_us > 0 ? axis_acquire_int_text(r->published_us) : NULL;
		g_autofree gchar *seeders = r->seeders >= 0 ? axis_acquire_int_text(r->seeders) : NULL;
		g_autofree gchar *indexer_id = axis_acquire_int_text(r->indexer_id);
		g_autofree gchar *rank = axis_acquire_int_text(c->rank);
		g_autofree gchar *score = axis_acquire_int_text(c->verdict != NULL ? c->verdict->score : 0);
		g_autofree gchar *json = axis_acquire_reasons_to_json(reasons);
		g_autofree gchar *match_text = axis_acquire_text_cut(c->match_text, 1000);
		g_autofree gchar *indexer = axis_acquire_text_cut(r->indexer_name, 100);
		const gchar *params[22];

		params[0] = search;
		params[1] = position;
		params[2] = release;
		params[3] = source;
		params[4] = r->name;
		params[5] = axis_download_protocol_to_string(r->protocol);
		params[6] = size;
		params[7] = published;
		params[8] = seeders;
		params[9] = indexer;
		params[10] = indexer_id;
		params[11] = r->info_hash != NULL && axis_download_hash_valid(r->info_hash) ? r->info_hash : NULL;
		params[12] = c->name_key;
		params[13] = release_quality(c);
		params[14] = c->match == AXIS_RELEASE_MATCH_YES ? "match" :
			c->match == AXIS_RELEASE_MATCH_AMBIGUOUS ? "ambiguous" : "none";
		params[15] = match_text;
		params[16] = c->accepted ? "true" : "false";
		params[17] = c->verdict != NULL && c->verdict->safety ? "true" : "false";
		params[18] = c->blocklisted ? "true" : "false";
		params[19] = rank;
		params[20] = score;
		params[21] = json;
		if (!axis_db_conn_exec(conn, sql, 22, params, &inserted, error))
			return FALSE;
		axis_db_result_get_int64(inserted, 0, 0, &c->candidate_id);
		if (s->self->info->candidate_stored != NULL &&
		    !s->self->info->candidate_stored(s->self->data, conn, s->want, c, c->candidate_id, error))
			return FALSE;
		if (c->accepted)
			accepted++;
	}
	{
		g_autofree gchar *n = axis_acquire_int_text(accepted);
		const gchar *params[2];

		params[0] = search;
		params[1] = n;
		if (!axis_db_conn_exec(conn, count_sql, 2, params, NULL, error))
			return FALSE;
	}
	return TRUE;
}

/* The search's end: its outcome, the notes of its candidates, the pacing
 * and the history, in one transaction. */
static gboolean
finish_search(
	Search      *s,
	const gchar *outcome,
	const gchar *outcome_text,
	gint64       grab_id,
	GError     **error
){
	g_autofree gchar *search = axis_acquire_int_text(s->search_id);
	g_autofree gchar *text = axis_acquire_text_cut(outcome_text, 2000);
	g_autofree gchar *finish_sql = axis_acquire_sql(s->self, "UPDATE %S SET finished_at = now(), outcome = $2,"
		" outcome_text = $3 WHERE id = $1::bigint");
	g_autofree gchar *note_sql = axis_acquire_sql(s->self, "UPDATE %C SET note = $2 WHERE id = $1::bigint");
	AxisDbConn *conn = axis_db_pool_acquire(s->self->pool, error);
	const gchar *params[3];
	gboolean ok;
	guint i;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error);
	if (ok && s->search_id == 0) {
		ok = store_search(s, conn, outcome, text, error);
	} else if (ok) {
		params[0] = search;
		params[1] = outcome;
		params[2] = text;
		ok = axis_db_conn_exec(conn, finish_sql, 3, params, NULL, error);
	}
	for (i = 0; ok && s->considered != NULL && i < s->considered->len; i++) {
		const AxisAcquireConsidered *c = g_ptr_array_index(s->considered, i);
		g_autofree gchar *id = NULL;
		g_autofree gchar *note = NULL;

		if (c->note == NULL || c->candidate_id == 0)
			continue;
		id = axis_acquire_int_text(c->candidate_id);
		note = axis_acquire_text_cut(c->note, 1000);
		params[0] = id;
		params[1] = note;
		ok = axis_db_conn_exec(conn, note_sql, 2, params, NULL, error);
	}
	/* A person's look moves no schedule. */
	if (g_strcmp0(s->cause, "list") != 0)
		ok = ok && axis_acquire_pace(s->self, conn, s->want->pace_ids, outcome, error);
	ok = ok && axis_acquire_eventf(s->self, conn, s->want->owner_id, s->want, "searched", s->user, s->search_id, grab_id,
		error, "%s%s%s%s", g_strcmp0(s->cause, "scheduled") == 0 ? "Scheduled search. " :
		g_strcmp0(s->cause, "retry") == 0 ? "Search for the next best. " : g_strcmp0(s->cause, "added") == 0 ?
		"Search on adding. " : g_strcmp0(s->cause, "profile") == 0 ? "Search after the quality profile changed. " :
		g_strcmp0(s->cause, "list") == 0 ? "Manual search. " :
		"", s->how != NULL && s->how->len > 0 ? s->how->str : "",
		s->how != NULL && s->how->len > 0 ? ". " : "", text != NULL ? text : outcome);
	for (i = 0; ok && s->deferred != NULL && i < s->deferred->len; i++) {
		const Deferred *d = g_ptr_array_index(s->deferred, i);

		ok = s->self->info->event(s->self->data, conn, s->want->owner_id, s->want, d->kind, s->user, s->search_id,
			d->grab_id, d->text, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(s->self->pool, conn);
	return ok;
}

/* ── Summaries ──────────────────────────────────────────────────── */

static const gchar *
code_phrase(
	const Search *s,
	const gchar  *code
){
	static const struct
	{
		const gchar *code;
		const gchar *phrase;
	} phrases[] = {
		{ "no_title", "no readable title" },
		{ "sample", "a sample" },
		{ "password", "password-protected" },
		{ "archive", "an archive" },
		{ "executable", "a program" },
		{ "post_subject", "one part of a Usenet post" },
		{ "unwanted", "a recording or pre-release copy" },
		{ "not_allowed", "a quality the profile does not allow" },
		{ "unknown_quality", "unknown quality" },
		{ "too_large", "too large" },
		{ "too_small", "too small" },
		{ "over_limit", "over the size limit" },
		{ "rule_require", "without what the profile requires" },
		{ "rule_reject", "with what the profile rejects" },
		{ "language", "not in a wanted language" },
		{ "retention", "older than the Usenet retention" },
		{ "profile_invalid", "an unusable profile" },
		{ "upgrades_off", "not wanted over the file on disk" },
		{ "cutoff", "not wanted over the file on disk" },
		{ "not_better", "not better than the file on disk" },
		{ "blocklisted", "on the blocklist" },
		{ NULL, NULL }
	};
	guint i;

	if (g_strcmp0(code, "no_match") == 0)
		return s->phrase_no_match;
	if (g_strcmp0(code, "ambiguous") == 0)
		return s->phrase_ambiguous;
	for (i = 0; code != NULL && phrases[i].code != NULL; i++) {
		if (strcmp(phrases[i].code, code) == 0)
			return phrases[i].phrase;
	}
	return "other reasons";
}

/* "not this film (3), too large (2)": the first reason of each rejected one. */
static gchar *
rejection_summary(const Search *s)
{
	g_autoptr(GHashTable) counts = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GPtrArray) order = g_ptr_array_new();
	g_autoptr(GString) text = g_string_new(NULL);
	guint i;

	for (i = 0; i < s->considered->len; i++) {
		const AxisAcquireConsidered *c = g_ptr_array_index(s->considered, i);
		const gchar *phrase;
		guint n;

		if (c->accepted)
			continue;
		if (c->blocklisted)
			phrase = code_phrase(s, "blocklisted");
		else if (c->reject_phrase != NULL && (c->rejected || c->match != AXIS_RELEASE_MATCH_YES))
			phrase = c->reject_phrase;
		else if (c->rejected)
			phrase = "not wanted";
		else if (c->verdict != NULL && c->verdict->reasons->len > 0)
			phrase = code_phrase(s, ((const AxisQualityReason *)g_ptr_array_index(c->verdict->reasons, 0))->code);
		else
			phrase = code_phrase(s, NULL);
		n = GPOINTER_TO_UINT(g_hash_table_lookup(counts, phrase));
		if (n == 0)
			g_ptr_array_add(order, (gpointer)phrase);
		g_hash_table_insert(counts, (gpointer)phrase, GUINT_TO_POINTER(n + 1));
	}
	for (i = 0; i < order->len; i++)
		g_string_append_printf(text, "%s%s (%u)", i > 0 ? ", " : "", (const gchar *)g_ptr_array_index(order, i),
			GPOINTER_TO_UINT(g_hash_table_lookup(counts, g_ptr_array_index(order, i))));
	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* ── The search ─────────────────────────────────────────────────── */

static gboolean
skip(
	AxisAcquire              *self,
	const AxisAcquireWant    *want,
	const gchar              *cause,
	const gchar              *user,
	const gchar              *why,
	AxisAcquireSearchReport  *report,
	GError                  **error
){
	AxisDbConn *conn;
	gboolean ok;

	report->skipped = why;
	/* Asked for by a person, on adding or by a new profile: the history says
	 * why nothing came of it. */
	if (g_strcmp0(cause, "manual") != 0 && g_strcmp0(cause, "added") != 0 && g_strcmp0(cause, "profile") != 0 &&
	    g_strcmp0(cause, "list") != 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_acquire_eventf(self, conn, want->owner_id, want, "skipped", user, 0, 0, error, "Not searched for: %s.",
		why);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* The searches' summary for the history: "Prowlarr by IMDb identifier". */
static void
note_how(
	Search                      *s,
	const AxisIndexerSearchPart *part
){
	if (s->how->len > 0 && !s->how_begun)
		g_string_append(s->how, "; ");
	s->how_begun = FALSE;
	g_string_append_printf(s->how, "%s by %s%s%s: %u %s", part->name, axis_indexer_mode_label(part->mode),
		part->fell_back ? " (the identifier found nothing)" : "", part->cached ? ", from a search a moment ago" : "",
		part->n_releases, part->n_releases == 1 ? "release" : "releases");
}

/* The same for an indexer that had nothing for this kind of search and was
 * not asked: "EZTV not asked, it has no movies". */
static void
note_not_asked(
	Search                      *s,
	const AxisIndexerSearchPart *part
){
	if (s->how->len > 0 && !s->how_begun)
		g_string_append(s->how, "; ");
	s->how_begun = FALSE;
	g_string_append_printf(s->how, "%s not asked, %s", part->name, part->not_asked);
}

/* With several requests, each one's share of the summary begins with what
 * was asked: "Asked by season 6 episode 12: ...". */
static void
note_asked(
	Search      *s,
	const gchar *label
){
	g_string_append_printf(s->how, "%sAsked %s: ", s->how->len > 0 ? ". " : "", label);
	s->how_begun = TRUE;
}

/*
 * One more request of the search (an anime series': the words of its name and
 * an absolute number, or its name in the anime category), made as the first
 * is: every enabled indexer, each within its own limits and holds, the answer
 * kept and reused as any search's.  What it cannot be asked of costs the
 * search nothing: the first request's answer stands.
 */
static gboolean
ask_more(
	Search                *s,
	const AxisAcquireAsk  *ask,
	GError               **error
){
	AxisIndexerRequest request = ask->request;
	AxisIndexerSearchResult *result = NULL;
	g_autoptr(GError) local = NULL;
	guint i;

	request.automatic = s->request.automatic;
	note_asked(s, ask->label);
	if (!axis_indexers_search(s->self->indexers, 0, &request, s->cancellable, &result, &local)) {
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		s->how_begun = FALSE;
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID)) {
			/* (One indexer alone, which takes no words: it says so itself.) */
			g_string_append(s->how, "not asked, no indexer here can be searched by these words");
		} else {
			g_string_append(s->how, "no indexer answered");
			if (strstr(s->source_errors->str, local->message) == NULL)
				g_string_append_printf(s->source_errors, "%s%s", s->source_errors->len > 0 ? "; " : "", local->message);
		}
		return TRUE;
	}
	for (i = 0; result->parts != NULL && i < result->parts->len; i++) {
		const AxisIndexerSearchPart *part = g_ptr_array_index(result->parts, i);

		if (part->asked && part->problem == NULL) {
			note_how(s, part);
		} else if (part->not_asked != NULL) {
			note_not_asked(s, part);
		} else if (part->problem != NULL) {
			g_autofree gchar *said = g_strdup_printf("%s: %s", part->name, part->problem);

			if (strstr(s->source_errors->str, said) == NULL)
				g_string_append_printf(s->source_errors, "%s%s", s->source_errors->len > 0 ? "; " : "", said);
		}
	}
	if (s->how_begun) {
		s->how_begun = FALSE;
		g_string_append(s->how, "no indexer answered");
	}
	g_ptr_array_add(s->results, result);
	return TRUE;
}

/* Every enabled indexer and source at once (the layer asks them each within
 * its own limits and merges what they found); an automatic search leaves
 * out an indexer near its daily allowance. */
static gboolean
ask_indexers(
	Search  *s,
	GError **error
){
	AxisIndexerSearchResult *result = NULL;
	g_autoptr(GError) local = NULL;
	gboolean more;
	guint i;

	s->results = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_search_result_free);
	s->how = g_string_new(NULL);
	s->source_errors = g_string_new(NULL);
	/* A person's search (Search now, a manual search) asks every indexer. */
	s->request.automatic = g_strcmp0(s->cause, "manual") != 0 && g_strcmp0(s->cause, "list") != 0;
	more = s->want->asks != NULL && s->want->asks->len > 0;
	if (s->want->ask_label != NULL)
		note_asked(s, s->want->ask_label);
	if (!axis_indexers_search(s->self->indexers, 0, &s->request, s->cancellable, &result, &local)) {
		g_autoptr(GPtrArray) sources = NULL;

		if (s->how_begun) {
			s->how_begun = FALSE;
			g_string_append(s->how, "no indexer answered");
		}
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE))
			return TRUE;
		/* None answered: how many were there, and what they said. */
		if (!axis_indexers_list_sources(s->self->indexers, &sources, error))
			return FALSE;
		for (i = 0; i < sources->len; i++) {
			const AxisIndexerSourceInfo *source = g_ptr_array_index(sources, i);

			if (source->enabled)
				s->n_sources++;
		}
		s->n_sources = MAX(s->n_sources, 1);
		g_string_append(s->source_errors, local->message);
		return TRUE;
	}
	for (i = 0; result->parts != NULL && i < result->parts->len; i++) {
		const AxisIndexerSearchPart *part = g_ptr_array_index(result->parts, i);

		s->n_sources++;
		if (part->asked && part->problem == NULL)
			note_how(s, part);
		else if (part->not_asked != NULL)
			note_not_asked(s, part);
		else if (part->problem != NULL)
			g_string_append_printf(s->source_errors, "%s%s: %s", s->source_errors->len > 0 ? "; " : "", part->name,
				part->problem);
	}
	if (s->how_begun) {
		s->how_begun = FALSE;
		g_string_append(s->how, "no indexer answered");
	}
	g_ptr_array_add(s->results, result);
	/* What else the module asks for (none, for every search but an anime
	 * series'): after the first request, and only when it could be made. */
	for (i = 0; more && i < s->want->asks->len; i++) {
		if (g_cancellable_set_error_if_cancelled(s->cancellable, error))
			return FALSE;
		if (!ask_more(s, g_ptr_array_index(s->want->asks, i), error))
			return FALSE;
	}
	return TRUE;
}

static gchar *
why_chosen(
	const Search                *s,
	const AxisAcquireConsidered *best,
	guint                        n_accepted
){
	const AxisAcquireConsidered *next = NULL;
	g_autofree gchar *versus = NULL;
	g_autofree gchar *matches = g_strdup_printf("it matches the %s", noun(s->self));
	guint i;

	for (i = 0; i < s->considered->len && next == NULL; i++) {
		const AxisAcquireConsidered *c = g_ptr_array_index(s->considered, i);

		if (c->accepted && c->rank == best->rank + 1)
			next = c;
	}
	if (next == NULL)
		return g_strdup_printf("It is the one acceptable release of %u found; %s.", s->considered->len,
			best->match_text != NULL ? best->match_text : matches);
	versus = axis_quality_explain_rank(&s->profile, best->verdict, next->verdict);
	return g_strdup_printf("Ranked %u of %u acceptable, above the next for %s; %s.", best->rank, n_accepted, versus,
		best->match_text != NULL ? best->match_text : matches);
}

static gint
compare_order(
	gconstpointer a,
	gconstpointer b
){
	const AxisAcquireConsidered *x = *(const AxisAcquireConsidered *const *)a;
	const AxisAcquireConsidered *y = *(const AxisAcquireConsidered *const *)b;

	return x->rank < y->rank ? -1 : x->rank > y->rank;
}

/* Tries the acceptable releases best first.  Sets *@outcome (static) and
 * *@text. */
static gboolean
try_grabs(
	Search        *s,
	const gchar  **outcome,
	gchar        **text,
	gint64        *grab_id,
	guint         *tries,
	GError       **error
){
	g_autoptr(GPtrArray) order = g_ptr_array_new();
	g_autoptr(GString) failures = g_string_new(NULL);
	gboolean no_client = FALSE;
	guint i;

	for (i = 0; i < s->considered->len; i++) {
		AxisAcquireConsidered *c = g_ptr_array_index(s->considered, i);

		if (c->accepted)
			g_ptr_array_add(order, c);
	}
	/* By rank (stable: ranks are distinct). */
	g_ptr_array_sort(order, compare_order);
	for (i = 0; i < order->len; i++) {
		AxisAcquireConsidered *c = g_ptr_array_index(order, i);
		g_autofree gchar *why = NULL;
		g_autofree gchar *problem = NULL;
		GrabSpec g;
		AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;

		if (!protocol_usable(s, c->release->protocol)) {
			c->note = g_strdup_printf("Passed over: no usable download client takes %s releases.",
				axis_download_protocol_label(c->release->protocol));
			no_client = TRUE;
			continue;
		}
		if (*tries >= AXIS_ACQUIRE_MAX_TRIES) {
			c->note = g_strdup("Not tried: the search stopped after too many releases that could not be had.");
			continue;
		}
		if (g_cancellable_set_error_if_cancelled(s->cancellable, error))
			return FALSE;
		(*tries)++;
		why = why_chosen(s, c, s->n_accepted);
		memset(&g, 0, sizeof(g));
		g.deferred = s->deferred;
		g.want = s->want;
		g.cancellable = s->cancellable;
		g.spec.considered = c;
		g.spec.search_id = s->search_id;
		g.spec.candidate_id = c->candidate_id;
		g.spec.release_id = c->release->id;
		g.spec.source_id = c->source_id;
		g.spec.name = c->release->name;
		g.spec.protocol = c->release->protocol;
		g.spec.size_bytes = c->release->size_bytes;
		g.spec.indexer = c->release->indexer_name;
		g.spec.indexer_id = c->release->indexer_id;
		g.spec.quality = axis_quality_name(c->verdict->quality);
		g.spec.name_key = c->name_key;
		g.spec.info_hash = c->release->info_hash;
		g.spec.why = why;
		g.spec.manual = FALSE;
		g.spec.user = s->user;
		if (!grab_release(s->self, &g, &result, grab_id, &problem, error))
			return FALSE;
		switch (result) {
		case AXIS_ACQUIRE_GRAB_SENT:
			c->note = g_strdup("Grabbed.");
			*outcome = "grabbed";
			if (c->rank == 1 && order->len == 1)
				*text = g_strdup_printf("Grabbed %s, the one acceptable release of %u found.", c->release->name,
					s->considered->len);
			else
				*text = g_strdup_printf("Grabbed %s, ranked %u of %u acceptable releases of %u found.",
					c->release->name, c->rank, order->len, s->considered->len);
			return TRUE;
		case AXIS_ACQUIRE_GRAB_IN_FLIGHT:
		case AXIS_ACQUIRE_GRAB_GONE:
			c->note = g_strdup_printf("Not grabbed: %s.", problem);
			*outcome = "in_flight";
			*text = g_strdup_printf("Nothing was grabbed: %s.", problem);
			return TRUE;
		case AXIS_ACQUIRE_GRAB_NO_CLIENT:
			c->note = g_strdup_printf("Not sent: %s", problem);
			if (c->release->protocol == AXIS_DOWNLOAD_USENET)
				s->usenet_usable = FALSE;
			else
				s->torrent_usable = FALSE;
			no_client = TRUE;
			break;
		case AXIS_ACQUIRE_GRAB_BAD_RELEASE:
			c->note = g_strdup_printf("Not sent: %s", problem);
			g_string_append_printf(failures, "%s%s: %s", failures->len > 0 ? "; " : "", c->release->name, problem);
			break;
		case AXIS_ACQUIRE_GRAB_CLIENT_ERROR:
			c->note = g_strdup_printf("Not sent: %s", problem);
			*outcome = "client_error";
			*text = g_strdup_printf("%s was not sent: %s Nothing else is tried until the client works again.",
				c->release->name, problem);
			return TRUE;
		default:
			c->note = g_strdup_printf("Not sent: %s", problem);
			*outcome = "indexer_error";
			*text = g_strdup_printf("%s could not be fetched: %s", c->release->name, problem);
			return TRUE;
		}
	}
	if (*tries > 0) {
		*outcome = "exhausted";
		*text = g_strdup_printf("Every acceptable release that was tried could not be had (%s).%s", failures->str,
			no_client ? " The others are of a kind no usable download client takes." : "");
	} else {
		gboolean torrents = FALSE;
		gboolean usenet = FALSE;

		for (i = 0; i < order->len; i++) {
			const AxisAcquireConsidered *c = g_ptr_array_index(order, i);

			if (c->release->protocol == AXIS_DOWNLOAD_TORRENT)
				torrents = TRUE;
			else
				usenet = TRUE;
		}
		*outcome = "no_client";
		*text = g_strdup_printf("%u acceptable %s found, but no usable download client takes %s: set up or switch "
			"on a client for %s. Nothing was sent.", order->len, order->len == 1 ? "release was" : "releases were",
			order->len == 1 ? "it" : "them", torrents && usenet ? "torrents or Usenet" : torrents ? "torrents" :
			"Usenet");
	}
	return TRUE;
}

gboolean
axis_acquire_run_search(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	const gchar              *cause,
	const gchar              *user,
	GCancellable             *cancellable,
	AxisAcquireSearchReport  *report_out,
	GError                  **error
){
	Search s;
	AxisAcquireSearchReport report;
	AxisAcquireTarget own = *target;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	const gchar *outcome = NULL;
	const gchar *skipped = NULL;
	AxisDbConn *conn;
	gint64 grab_id = 0;
	gboolean ok = TRUE;
	gboolean list = g_strcmp0(cause, "list") == 0;
	guint blocked_accepted = 0;
	guint i;

	/* A manual search is a person's look: whatever is wanted now. */
	if (list)
		own.by_hand = TRUE;
	target = &own;
	memset(&s, 0, sizeof(s));
	memset(&report, 0, sizeof(report));
	s.self = self;
	s.cause = cause;
	s.user = user;
	s.cancellable = cancellable;
	s.now_us = g_get_real_time();
	s.deferred = g_ptr_array_new_with_free_func((GDestroyNotify)deferred_free);
	s.phrase_no_match = g_strdup_printf("not this %s", noun(self));
	s.phrase_ambiguous = g_strdup_printf("not certainly this %s", noun(self));

	/* 1. What can be decided without asking anyone. */
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		search_clear(&s);
		return FALSE;
	}
	ok = self->info->load(self->data, conn, target, &s.want, &skipped, error);
	if (ok && s.want != NULL)
		ok = axis_acquire_store_block_keys(self, conn, s.want->owner_id, &s.blocklist, error) &&
			(self->info->read_disk == NULL || self->info->read_disk(self->data, conn, s.want, error));
	if (ok && s.want != NULL && s.want->profile_usable && !axis_quality_store_load(conn, s.want->profile_id,
	        &s.profile, &local))
		s.want->profile_usable = FALSE;
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		goto out;
	if (s.want == NULL) {
		report.skipped = skipped != NULL ? skipped : "it was removed";
		goto out;
	}
	if (skipped != NULL) {
		ok = skip(self, s.want, cause, user, skipped, &report, error);
		goto out;
	}
	if (!s.want->profile_usable) {
		outcome = "profile_unusable";
		text = g_strdup_printf("Its quality profile, %s, cannot be used: %s", s.want->profile_name,
			local != NULL ? local->message : "its definition does not validate.");
		goto finish;
	}
	if (s.want->root_id == 0) {
		outcome = "root_offline";
		text = g_strdup("It has no destination root (the one it had was removed): nothing was searched for or "
			"sent. Choose another on its page.");
		goto finish;
	}
	/* The storage's own view of the volume is the freshest. */
	volume = axis_storage_get_volume(self->storage, s.want->volume_id);
	if (volume == NULL || !axis_volume_state_is_usable(volume->state)) {
		outcome = "root_offline";
		text = g_strdup_printf("Its destination, %s, is on %s, which is not online: nothing was searched for or "
			"sent.", s.want->root_name != NULL ? s.want->root_name : "its root",
			volume != NULL ? volume->name : s.want->volume_name != NULL ? s.want->volume_name : "a volume");
		goto finish;
	}
	if (!axis_acquire_read_clients(self, &s.usenet_usable, &s.torrent_usable, error)) {
		ok = FALSE;
		goto out;
	}
	if (!s.usenet_usable && !s.torrent_usable) {
		outcome = "no_client";
		text = g_strdup("No download client is set up and switched on (or every one refused its secret): nothing was "
			"searched for or sent. Add one under Download clients.");
		goto finish;
	}

	/* 2. The record of what is wanted, and every title of its name. */
	axis_indexer_request_init(&s.request, AXIS_INDEXER_QUERY_FILM);
	if (!self->info->prepare(self->data, s.want, cancellable, &s.request, &s.titles, &problem, error)) {
		ok = FALSE;
		goto out;
	}
	if (problem != NULL || s.titles == NULL || s.titles->len == 0) {
		outcome = "metadata_missing";
		text = g_steal_pointer(&problem);
		if (text == NULL)
			text = g_strdup("Its record could not be read from the metadata store.");
		goto finish;
	}
	if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
		ok = FALSE;
		goto out;
	}

	/* 3. The indexers. */
	if (!ask_indexers(&s, error)) {
		ok = FALSE;
		goto out;
	}
	if (s.n_sources == 0) {
		outcome = "no_indexer";
		text = g_strdup("No indexer is set up and switched on: nothing was searched for. Add one under "
			"Indexers.");
		goto finish;
	}
	if (s.results->len == 0) {
		outcome = "indexer_error";
		text = g_strdup_printf("The indexers could not be searched: %s.", s.source_errors->str);
		goto finish;
	}

	/* 4. Read, match, judge, blocklist, decide, rank. */
	read_releases(&s);
	decide(&s);
	for (i = 0; i < s.considered->len; i++) {
		const AxisAcquireConsidered *c = g_ptr_array_index(s.considered, i);

		if ((c->blocklisted || c->rejected) && c->verdict->accepted)
			blocked_accepted++;
	}

	/* 5. Every release, with its verdict. */
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		ok = FALSE;
		goto out;
	}
	ok = axis_db_conn_begin(conn, error) && store_search(&s, conn, NULL, NULL, error) &&
		store_candidates(&s, conn, error) && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		goto out;
	report.search_id = s.search_id;

	/* 6. The best that can be had; a manual search stops here, and a person
	 * chooses. */
	if (list) {
		guint acceptable = 0;

		for (i = 0; i < s.considered->len; i++)
			acceptable += ((const AxisAcquireConsidered *)g_ptr_array_index(s.considered, i))->accepted ? 1 : 0;
		outcome = "listed";
		if (s.considered->len == 0)
			text = g_strdup("No release was found. A manual search grabs nothing by itself.");
		else
			text = g_strdup_printf("%u %s found, %u acceptable under the profile %s. Nothing is grabbed by itself: "
				"an administrator chooses one.", s.considered->len, s.considered->len == 1 ? "release was" :
				"releases were", acceptable, s.want->profile_name);
	} else if (s.considered->len == 0) {
		outcome = "nothing_found";
		text = g_strdup("No release was found.");
	} else if (s.n_accepted - MIN(blocked_accepted, s.n_accepted) == 0) {
		guint blocked = 0;
		guint by_person = 0;

		for (i = 0; i < s.considered->len; i++) {
			const AxisAcquireConsidered *c = g_ptr_array_index(s.considered, i);

			if (c->blocklisted && c->verdict->accepted) {
				blocked++;
				if (g_strcmp0(c->block_reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0)
					by_person++;
			}
		}
		summary = rejection_summary(&s);
		if (blocked > 0 && by_person > 0) {
			/* Not all of them failed: a person put some there. */
			outcome = "exhausted";
			text = g_strdup_printf("Every acceptable release is on its blocklist: %u %s, %u of them put there by a "
				"person. Of the %u found: %s. Take a release off the blocklist, or wait for new releases.", blocked,
				blocked == 1 ? "is" : "are", by_person, s.considered->len, summary);
		} else if (blocked > 0) {
			outcome = "exhausted";
			text = g_strdup_printf("Every acceptable release has failed before: %u %s on its blocklist. Of the %u "
				"found: %s. Clear the blocklist, or wait for new releases.", blocked, blocked == 1 ? "is" : "are",
				s.considered->len, summary);
		} else if (s.want->on_disk && s.decision != NULL) {
			/* In the library already: nothing found is wanted over it. */
			outcome = "none_acceptable";
			text = g_strdup_printf("%u %s found, and with the profile %s none is wanted over the file in the "
				"library, %s: %s.", s.considered->len, s.considered->len == 1 ? "release was" : "releases were",
				s.want->profile_name, s.decision->disk_text != NULL ? s.decision->disk_text : "of unknown quality",
				summary);
		} else {
			outcome = "none_acceptable";
			text = g_strdup_printf("%u %s found and the profile %s rejected %s: %s.", s.considered->len,
				s.considered->len == 1 ? "release was" : "releases were", s.want->profile_name,
				s.considered->len == 1 ? "it" : "them all", summary);
		}
	} else if (!try_grabs(&s, &outcome, &text, &grab_id, &report.tries, error)) {
		ok = FALSE;
		goto out;
	}
	if (s.source_errors->len > 0) {
		g_autofree gchar *more = g_strdup_printf("%s (Not every indexer answered: %s.)", text, s.source_errors->str);

		g_free(text);
		text = g_steal_pointer(&more);
	}

finish:
	report.searched = TRUE;
	report.outcome = outcome;
	report.grab_id = grab_id;
	ok = finish_search(&s, outcome, text, grab_id, error);
	report.search_id = s.search_id;
out:
	search_clear(&s);
	if (report_out != NULL)
		*report_out = report;
	return ok;
}

/* ── An administrator's grab ────────────────────────────────────── */

/* The release of the owner's newest open grab (%NULL: none), for the words
 * of a replace that a search's claim came before. */
static gboolean
open_release(
	AxisAcquire  *self,
	gint64        owner_id,
	gchar       **release,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT release_name FROM %G WHERE %O = $1::bigint"
		" AND closed_at IS NULL ORDER BY id DESC LIMIT 1");
	const gchar *params[1];

	*release = NULL;
	params[0] = owner;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) > 0)
		*release = g_strdup(axis_db_result_get_text(row, 0, 0));
	return TRUE;
}

gboolean
axis_acquire_grab(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	const gchar              *user,
	gint64                   *grab_id,
	AxisAcquireGrabResult    *result,
	gchar                   **problem,
	GError                  **error
){
	AxisAcquireHow how;

	memset(&how, 0, sizeof(how));
	return axis_acquire_grab_stored(self, target, candidate_id, user, &how, grab_id, result, problem, error);
}

gboolean
axis_acquire_grab_stored(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	const gchar              *user,
	const AxisAcquireHow     *how,
	gint64                   *grab_id,
	AxisAcquireGrabResult    *result,
	gchar                   **problem,
	GError                  **error
){
	g_autoptr(AxisAcquireWant) want = NULL;
	g_autoptr(GPtrArray) blocklist = NULL;
	g_autoptr(AxisIndexerRelease) release = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisAcquireHandPreview) preview = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *replaces = NULL;
	g_autofree gchar *forced = NULL;
	AxisAcquireTarget own = *target;
	AxisAcquireCandidate *candidate = NULL;
	const gchar *skipped = NULL;
	gboolean usenet = FALSE;
	gboolean torrent = FALSE;
	guint n_replaced = 0;
	guint n_forced = 0;
	gboolean kept_ticked = FALSE;
	gint64 sent_id = 0;
	const AxisAcquireReplaceTicks *asked = NULL;
	const AxisAcquireStanding *replacing = NULL;
	gboolean flying = FALSE;
	AxisDbConn *conn;
	GrabSpec g;
	gboolean ok;
	guint i;

	*result = AXIS_ACQUIRE_GRAB_NOT_FOUND;
	*problem = NULL;
	if (how->by_hand)
		own.by_hand = TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = self->info->load(self->data, conn, &own, &want, &skipped, error) &&
		(want == NULL || axis_acquire_store_candidate(self, conn, want->owner_id, candidate_id, &candidate, error)) &&
		(want == NULL || axis_acquire_store_block_keys(self, conn, want->owner_id, &blocklist, error));
	/* What the module refuses by hand, whether it would replace a better
	 * file, and whether it is numbered otherwise than the data (which only
	 * Grab this one takes, after its warning). */
	if (ok && want != NULL && candidate != NULL && (!candidate->safety || how->force != NULL)) {
		preview = g_new0(AxisAcquireHandPreview, 1);
		preview->force = how->force != NULL;
		ok = axis_acquire_preview_fill(self, conn, want, candidate, preview, error);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		goto out;
	if (want == NULL || candidate == NULL) {
		*result = want == NULL ? AXIS_ACQUIRE_GRAB_NOT_FOUND : AXIS_ACQUIRE_GRAB_NO_CANDIDATE;
		goto out;
	}
	/* A file a person keeps by hand is replaced only through a confirmation
	 * that names it and carries its tick: never by one click (Grab, Grab it
	 * anyway), whatever the profile says. */
	if (!how->by_hand && how->force == NULL && skipped != NULL && strstr(skipped, "its file is kept") != NULL) {
		/* The module's own words say how it is kept: by hand, or as it was
		 * brought over from another app. */
		*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
		*problem = g_strdup_printf("%s, so no single click replaces it. Use Manual search: it asks you to confirm "
			"replacing that file. Nothing was sent", strstr(skipped, "kept by hand") != NULL ?
			"its file in the library is kept by hand" : skipped);
		goto out;
	}
	/* The same for what the release itself would replace (an episode's kept
	 * file under a pack). */
	for (i = 0; !how->by_hand && how->force == NULL && preview != NULL && preview->replaces != NULL &&
	     i < preview->replaces->len; i++) {
		const AxisAcquireReplaced *r = g_ptr_array_index(preview->replaces, i);

		g_autofree gchar *kept_words = NULL;

		if (r->kept_by == NULL)
			continue;
		kept_words = axis_acquire_replaced_kept_words(r);
		*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
		*problem = g_strdup_printf("%s in the library is %s, so no single click replaces it. Use "
			"Manual search: it asks you to confirm replacing that file. Nothing was sent", r->label != NULL ? r->label :
			"a file", kept_words);
		goto out;
	}
	if (how->force != NULL) {
		/* By force: only the fixed rules about its name are set aside, and
		 * only a release that has one is taken this way. */
		g_autofree gchar *kept = NULL;

		if (!candidate->safety) {
			*result = AXIS_ACQUIRE_GRAB_REFUSED;
			*problem = g_strdup("No fixed rule rules it out: use Grab this one.");
			goto out;
		}
		if (!axis_acquire_force_allowed(candidate, &kept)) {
			*result = AXIS_ACQUIRE_GRAB_REFUSED;
			*problem = g_steal_pointer(&kept);
			goto out;
		}
	} else if (candidate->safety) {
		g_autoptr(GString) reasons = g_string_new(NULL);

		for (i = 0; i < candidate->reasons->len; i++) {
			const AxisAcquireReason *r = g_ptr_array_index(candidate->reasons, i);

			if (g_strcmp0(r->kind, "safety") == 0)
				g_string_append_printf(reasons, "%s%s", reasons->len > 0 ? " " : "", r->text);
		}
		*result = AXIS_ACQUIRE_GRAB_REFUSED;
		*problem = reasons->len > 0 ? g_strdup(reasons->str) : NULL;
		goto out;
	}
	if (axis_acquire_blocked(blocklist, candidate->name_key, candidate->info_hash)) {
		*result = AXIS_ACQUIRE_GRAB_BLOCKLISTED;
		goto out;
	}
	/* The module's refusals are Grab this one's; Grab's are the claim's. */
	if (!how->by_hand && preview != NULL) {
		g_clear_pointer(&preview->refused, g_free);
		preview->in_flight = FALSE;
	}
	flying = want->in_flight || (preview != NULL && preview->in_flight);
	asked = how->force != NULL ? how->force->in_flight : how->ticks != NULL ? how->ticks->in_flight : NULL;
	if (asked != NULL && asked->grab_id <= 0)
		asked = NULL;
	/* A replace of the download in flight was asked for (the confirmation
	 * offered it): what stands in the way is read again, here, and must be
	 * what the page showed.  Another grab, a download that has arrived, an
	 * import, nothing in the way any more: nothing is written, and the page is
	 * shown again.  Then its ticks.  Every later check of the grab runs before
	 * anything is ended. */
	if (how->by_hand && asked != NULL) {
		const AxisAcquireStanding *s = preview != NULL && preview->replaceable && preview->standing != NULL &&
			preview->standing->len == 1 ? g_ptr_array_index(preview->standing, 0) : NULL;

		if (s == NULL || s->grab_id != asked->grab_id || asked->digest == NULL || preview->replace_digest == NULL ||
		    strcmp(asked->digest, preview->replace_digest) != 0) {
			*result = AXIS_ACQUIRE_GRAB_CHANGED;
			*problem = g_strdup_printf("%s Nothing was ended and nothing was sent", preview != NULL &&
				preview->standing_text != NULL ? preview->standing_text : "the download in flight is not what this page "
				"showed any more.");
			goto out;
		}
		if (!asked->ok) {
			*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
			*problem = g_strdup_printf("%s is downloading: tick the box to replace the download in flight with this "
				"release. Nothing was ended and nothing was sent", s->release != NULL ? s->release : "a release");
			goto out;
		}
		if (s->others != NULL && !asked->others) {
			*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
			*problem = g_strdup_printf("the download in flight also holds %s, which this release does not: tick the "
				"box that says they stop downloading too. Nothing was ended and nothing was sent", s->others);
			goto out;
		}
		replacing = s;
	}
	/* By force, a confirmation is only shown for what the module does not
	 * refuse: refused now (an episode named went in flight, the numbering
	 * changed, an import opened), what the page showed is no longer so. */
	if (replacing == NULL && how->force != NULL && !want->in_flight && preview != NULL &&
	    (preview->refused != NULL || preview->in_flight)) {
		*result = AXIS_ACQUIRE_GRAB_CHANGED;
		*problem = g_strdup(preview->refused != NULL ? preview->refused : preview->standing_text);
		goto out;
	}
	if (replacing == NULL && (flying || (preview != NULL && preview->refused != NULL))) {
		*result = AXIS_ACQUIRE_GRAB_IN_FLIGHT;
		*problem = preview != NULL ? g_strdup(preview->refused != NULL ? preview->refused : preview->standing_text) :
			NULL;
		goto out;
	}
	/* By force: what the confirmation showed must be what is so now (the
	 * claim's own checks guard what changes after this), the person must
	 * have ticked having checked the release, and every file it would
	 * replace needs its own tick, whether or not the release is better. */
	if (how->force != NULL) {
		g_autofree gchar *now = axis_acquire_force_digest(candidate, preview, want);

		/* Bound to nothing: the person named no part of the owner. */
		if (preview->unnamed) {
			*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
			*problem = g_strdup_printf("%s Nothing was sent", preview->pick_help != NULL ? preview->pick_help :
				"say what this release is for first.");
			goto out;
		}
		if (how->force->digest == NULL || strcmp(now, how->force->digest) != 0) {
			*result = AXIS_ACQUIRE_GRAB_CHANGED;
			*problem = g_strdup("what this page showed has changed since it was opened. Nothing was sent");
			goto out;
		}
		if (!how->force->checked) {
			*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
			*problem = g_strdup("tick the box that says you have checked that this release is what you say it is. "
				"Nothing was sent");
			goto out;
		}
		for (i = 0; i < preview->replaces->len; i++) {
			const AxisAcquireReplaced *r = g_ptr_array_index(preview->replaces, i);
			gboolean ticked = FALSE;
			guint k;

			for (k = 0; how->force->replace != NULL && k < how->force->replace->len; k++)
				ticked = ticked || g_array_index(how->force->replace, gint64, k) == r->file_id;
			if (!ticked) {
				*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
				*problem = g_strdup_printf("it would replace %s in the library, which was not ticked: tick the box to "
					"replace it. Nothing was sent", r->label != NULL ? r->label : "a file");
				goto out;
			}
		}
	}
	/* Numbered otherwise than the data: never one click. */
	if (!how->by_hand && preview != NULL && preview->numbering != NULL) {
		*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
		*problem = g_strdup("it numbers its episodes differently from the data: only Grab this one, from a manual "
			"search, takes it, after a warning. Nothing was sent");
		goto out;
	}
	/* By hand, a better file is replaced only when the person ticked it,
	 * and a release numbered otherwise than the data taken only so too. */
	if (how->by_hand && how->force == NULL && preview != NULL && preview->needs_tick &&
	    (how->ticks == NULL || !how->ticks->replace_better)) {
		*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
		*problem = g_strdup("it would replace a file in the library that it is not better than: tick the box to "
			"replace it. Nothing was sent");
		goto out;
	}
	/* By hand, a file kept by hand is replaced only with the tick that names
	 * it, though the release be better: "Grab this one" asks for it on its
	 * confirmation instead of the import refusing it later. */
	for (i = 0; how->by_hand && how->force == NULL && preview != NULL && i < preview->replaces->len; i++) {
		const AxisAcquireReplaced *r = g_ptr_array_index(preview->replaces, i);
		gboolean ticked = FALSE;
		guint k;

		if (r->kept_by == NULL)
			continue;
		for (k = 0; how->ticks != NULL && how->ticks->replace != NULL && k < how->ticks->replace->len; k++)
			ticked = ticked || g_array_index(how->ticks->replace, gint64, k) == r->file_id;
		if (!ticked) {
			g_autofree gchar *kept_words = axis_acquire_replaced_kept_words(r);

			*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
			*problem = g_strdup_printf("%s in the library is %s: tick the box that names it to replace "
				"it. Nothing was sent", r->label != NULL ? r->label : "a file", kept_words);
			goto out;
		}
	}
	if (how->by_hand && how->force == NULL && preview != NULL && preview->numbering != NULL &&
	    (how->ticks == NULL || !how->ticks->numbering)) {
		*result = AXIS_ACQUIRE_GRAB_NEEDS_TICK;
		*problem = g_strdup("it numbers its episodes differently from the data: tick the box to take it all the "
			"same. Nothing was sent");
		goto out;
	}
	if (want->root_id == 0 ||
	    (volume = axis_storage_get_volume(self->storage, want->volume_id)) == NULL ||
	    !axis_volume_state_is_usable(volume->state)) {
		*result = AXIS_ACQUIRE_GRAB_OFFLINE;
		goto out;
	}
	if (!axis_indexers_get_release(self->indexers, candidate->release_id, &release, &local)) {
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND)) {
			*result = AXIS_ACQUIRE_GRAB_EXPIRED;
		} else {
			g_propagate_error(error, g_steal_pointer(&local));
			ok = FALSE;
		}
		goto out;
	}
	if (!axis_acquire_read_clients(self, &usenet, &torrent, error)) {
		ok = FALSE;
		goto out;
	}
	if (!(candidate->protocol == AXIS_DOWNLOAD_USENET ? usenet : torrent)) {
		*result = AXIS_ACQUIRE_GRAB_NO_CLIENT;
		*problem = g_strdup_printf("no usable download client takes %s releases",
			axis_download_protocol_label(candidate->protocol));
		goto out;
	}

	if (how->force != NULL) {
		/* Every file it replaces was ticked: each is recorded, the ones the
		 * release is better than too. */
		replaces = axis_acquire_replaces_json(preview, TRUE, &n_replaced);
		forced = axis_acquire_forced_json(candidate, how->force->one_file, &n_forced);
	} else if (how->by_hand && preview != NULL) {
		/* The files it is not better than, and every file kept by hand: each
		 * was ticked, or the grab was refused above. */
		replaces = axis_acquire_replaces_json(preview, FALSE, &n_replaced);
		for (i = 0; i < preview->replaces->len; i++)
			kept_ticked = kept_ticked || ((const AxisAcquireReplaced *)g_ptr_array_index(preview->replaces,
				i))->kept_by != NULL;
	}
	if (how->why != NULL) {
		why = g_strdup(how->why);
	} else if (how->force != NULL) {
		g_autofree gchar *rules = axis_acquire_safety_words(candidate);
		g_autofree gchar *this_one = g_strdup_printf("this %s", noun(self));

		/* What was recorded, and nothing else. */
		why = g_strdup_printf("Chosen by %s by force from a manual search, for %s and nothing else. Set aside: %s%s "
			"The quality profile %s is set aside too. Its import does not judge the name again%s.",
			user != NULL ? user : "an administrator", preview->bound_text != NULL ? preview->bound_text : this_one,
			rules, g_str_has_suffix(rules, ".") ? "" : ".",
			want->profile_name != NULL ? want->profile_name : "", n_replaced == 0 ? "" : n_replaced == 1 ?
			", and replaces the file ticked" : ", and replaces the files ticked");
	} else if (how->by_hand)
		/* What was recorded, and nothing else. */
		why = g_strdup_printf("Chosen by %s by hand from a manual search, the quality profile %s set aside%s%s%s.",
			user != NULL ? user : "an administrator", want->profile_name != NULL ? want->profile_name : "",
			n_replaced > 0 ? "; replacing " : "", n_replaced == 0 ? "" : kept_ticked ?
			"what was ticked (a file kept by hand, or one it is not better than)" : n_replaced == 1 ?
			"a file it is not better than, as ticked" : "files it is not better than, as ticked",
			preview != NULL && preview->numbering != NULL ? "; numbered differently from the data, as ticked" : "");
	else
		why = candidate->accepted ? g_strdup_printf("Chosen by %s; the profile ranked it %u.", user != NULL ? user :
			"an administrator", candidate->rank) :
			g_strdup_printf("Chosen by %s although the profile rejected it.", user != NULL ? user :
			"an administrator");
	memset(&g, 0, sizeof(g));
	g.want = want;
	g.spec.considered = NULL;
	g.spec.search_id = candidate->search_id;
	g.spec.candidate_id = candidate->id;
	g.spec.release_id = candidate->release_id;
	g.spec.source_id = candidate->source_id;
	g.spec.name = candidate->name;
	g.spec.protocol = candidate->protocol;
	g.spec.size_bytes = candidate->size_bytes;
	g.spec.indexer = candidate->indexer;
	g.spec.indexer_id = candidate->indexer_id;
	g.spec.quality = candidate->quality;
	g.spec.name_key = candidate->name_key;
	g.spec.info_hash = candidate->info_hash;
	g.spec.why = why;
	g.spec.manual = TRUE;
	/* The profile is set aside by Grab this one, and by Grab it anyway on a
	 * release it rejected; an accepted Grab keeps it.  The tick is recorded
	 * for the very files it was given for. */
	g.spec.set_aside = how->by_hand ? "hand" : candidate->accepted ? NULL : "anyway";
	g.spec.replaces_files = replaces;
	g.spec.numbering_ok = how->by_hand && how->force == NULL && preview != NULL && preview->numbering != NULL;
	g.spec.forced = forced;
	g.spec.bound = how->force != NULL;
	g.spec.user = user;
	/* The replace: every check above has passed, so the download in flight is
	 * ended now, and only when nothing of it is left at its client is the
	 * release claimed and sent.  (The claim comes after: it needs the old grab
	 * closed.  What cannot be checked before is the fetch from the indexer and
	 * the client's answer.) */
	if (replacing != NULL) {
		g_autofree gchar *old_release = g_strdup(replacing->release != NULL ? replacing->release : "the download");
		g_autofree gchar *with = NULL;
		g_autofree gchar *stuck = NULL;
		AxisAcquirePlace place = AXIS_ACQUIRE_PLACE_UNKNOWN;
		gint64 old_grab = replacing->grab_id;
		gboolean ended = FALSE;
		gboolean gone = FALSE;

		/* Where it is now, before anything is ended: a place Axis does not let
		 * the client delete, and a client that cannot be asked where it is,
		 * both leave the download in flight as it was.  (The removal judges
		 * the place itself each time it asks, for what changes after this
		 * look.) */
		if (!axis_acquire_removal_place(self, replacing->download_id, &place, &stuck, error)) {
			ok = FALSE;
			goto out;
		}
		if (place == AXIS_ACQUIRE_PLACE_REFUSED) {
			*result = AXIS_ACQUIRE_GRAB_REPLACE_STUCK;
			*problem = g_strdup_printf("%s is not replaced. Axis does not ask the client to delete it: %s Nothing was "
				"ended and nothing was sent. Remove it in the client, or give up on it on the Downloads page",
				old_release, stuck != NULL ? stuck : "where it is now cannot be judged.");
			goto out;
		}
		if (place == AXIS_ACQUIRE_PLACE_UNKNOWN) {
			*result = AXIS_ACQUIRE_GRAB_REPLACE_STUCK;
			*problem = g_strdup_printf("%s is not replaced. Its client could not be asked where it is: %s%s Nothing was "
				"ended and nothing was sent. Try again when the client answers", old_release,
				stuck != NULL ? stuck : "no reason was given.", stuck != NULL && !g_str_has_suffix(stuck, ".") ? "." : "");
			goto out;
		}
		g_clear_pointer(&stuck, g_free);
		run_replace_hook(self, want->owner_id);
		if (!axis_acquire_replace_end(self, want->owner_id, replacing, candidate->name, user, &ended, &gone, &stuck,
		        error)) {
			ok = FALSE;
			goto out;
		}
		/* Not taken: it arrived, or was dealt with, between the look above and
		 * the owner's lock.  Nothing was ended. */
		if (!ended) {
			*result = AXIS_ACQUIRE_GRAB_CHANGED;
			*problem = g_strdup_printf("%s is not downloading any more: it has arrived, or was dealt with meanwhile. "
				"Nothing was ended and nothing was sent", old_release);
			goto out;
		}
		if (!gone) {
			*result = AXIS_ACQUIRE_GRAB_REPLACE_STUCK;
			*problem = g_strdup_printf("%s was ended, but it is still at its client. %s%s Nothing was sent. Axis "
				"keeps asking the client; nothing else is grabbed for the %s until it is gone. Then grab this release "
				"again from the manual search, or leave it to the next search", old_release,
				stuck != NULL ? stuck : "The client could not remove it.",
				stuck != NULL && !g_str_has_suffix(stuck, ".") ? "." : "", noun(self));
			goto out;
		}
		with = g_strdup_printf("%s It is sent in place of %s, which was downloading.", why, old_release);
		g.spec.why = with;
		ok = grab_release(self, &g, result, &sent_id, problem, error);
		if (ok && *result == AXIS_ACQUIRE_GRAB_IN_FLIGHT) {
			/* A search claimed in the moment between the removal and this
			 * claim: one download, the search's. */
			g_autofree gchar *other = NULL;

			/* (An owner with parts has other open grabs, of other parts: the
			 * one that came first is not named then.) */
			if (self->info->standing == NULL)
				ok = open_release(self, want->owner_id, &other, error);
			g_free(*problem);
			*problem = g_strdup_printf("%s was removed, and a search grabbed %s for the %s in the same moment. Nothing "
				"else was sent. You can replace that one from the manual search", old_release,
				other != NULL ? other : "another release", noun(self));
		} else if (ok && *result != AXIS_ACQUIRE_GRAB_SENT) {
			g_autofree gchar *reason = g_steal_pointer(problem);

			*problem = g_strdup_printf("the download in flight (%s) was removed, but this release could not be sent: "
				"%s%s Nothing is downloading for the %s now. A scheduled search takes it up again if it is monitored; "
				"otherwise search for it yourself", old_release, reason != NULL ? reason : "it could not be had",
				reason != NULL && g_str_has_suffix(reason, ".") ? "" : ".", noun(self));
		} else if (!ok && error != NULL && *error != NULL) {
			/* The claim itself failed (the module's check refused with its own
			 * words, or the database did): the old download is gone all the
			 * same, and the page says both. */
			g_prefix_error(error, "the download in flight (%s) was removed from its client, and this release was not "
				"sent. Nothing is downloading for the %s now. Why it was not sent: ", old_release, noun(self));
		}
		if (ok && *result == AXIS_ACQUIRE_GRAB_SENT && how->grabbed != NULL)
			how->grabbed->replaced_grab = old_grab;
	} else {
		ok = grab_release(self, &g, result, &sent_id, problem, error);
	}
	if (grab_id != NULL)
		*grab_id = sent_id;
	if (ok && *result == AXIS_ACQUIRE_GRAB_SENT && how->grabbed != NULL) {
		how->grabbed->grab_id = sent_id;
		how->grabbed->n_replaced = n_replaced;
		how->grabbed->numbering = g.spec.numbering_ok;
		how->grabbed->n_forced = n_forced;
	}
out:
	axis_acquire_candidate_free(candidate);
	return ok;
}

/* ── Another profile, tried on what a search found ──────────────── */

void
axis_acquire_profile_try_free(AxisAcquireProfileTry *attempt)
{
	if (attempt == NULL)
		return;
	g_clear_pointer(&attempt->accepted, g_array_unref);
	g_free(attempt->reason);
	g_free(attempt);
}

/* A stored release as a search held it: the indexer's release (its fields
 * borrowed from @c), read again and matched as it was then. */
static AxisAcquireConsidered *
considered_of(
	AxisAcquire                *self,
	AxisAcquireWant            *want,
	const GPtrArray            *blocklist,
	const AxisAcquireCandidate *candidate,
	AxisIndexerRelease         *release
){
	AxisAcquireConsidered *c = g_new0(AxisAcquireConsidered, 1);

	memset(release, 0, sizeof(*release));
	release->id = candidate->release_id;
	release->name = candidate->name;
	release->size_bytes = candidate->size_bytes;
	release->protocol = candidate->protocol;
	release->indexer_id = candidate->indexer_id;
	release->indexer_name = candidate->indexer;
	release->published_us = candidate->published_us;
	release->seeders = candidate->seeders;
	release->leechers = -1;
	release->grabs = -1;
	release->info_hash = candidate->info_hash;
	release->source_id = candidate->source_id;
	c->release = release;
	c->source_id = candidate->source_id;
	c->matched = candidate->match == AXIS_RELEASE_MATCH_YES ? 0 : -1;
	c->parsed = axis_release_parse(candidate->name);
	/* The matcher's verdict as the search stored it (a fixed rule: no
	 * profile changes it); what the module wants is read again, now. */
	c->match = candidate->match;
	c->match_text = g_strdup(candidate->match_text);
	if (c->match == AXIS_RELEASE_MATCH_YES && self->info->judge != NULL)
		self->info->judge(self->data, want, c);
	c->name_key = g_strdup(candidate->name_key);
	{
		const AxisAcquireBlockKey *block = axis_acquire_blocked_by(blocklist, candidate->name_key,
			candidate->info_hash);

		c->blocklisted = block != NULL;
		c->block_reason = block != NULL ? block->reason : NULL;
		c->block_by = block != NULL ? block->created_by : NULL;
	}
	return c;
}

gboolean
axis_acquire_try_profiles(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	const GPtrArray          *candidates,
	const GArray             *profile_ids,
	GPtrArray               **tries,
	GError                  **error
){
	g_autoptr(AxisAcquireWant) want = NULL;
	g_autoptr(GPtrArray) blocklist = NULL;
	g_autoptr(GPtrArray) considered = g_ptr_array_new_with_free_func((GDestroyNotify)considered_free);
	g_autofree AxisIndexerRelease *releases = g_new0(AxisIndexerRelease, MAX(candidates->len, 1));
	g_autofree AxisQualityProfile *profiles = g_new0(AxisQualityProfile, MAX(profile_ids->len, 1));
	g_autofree gboolean *loaded = g_new0(gboolean, MAX(profile_ids->len, 1));
	const gchar *skipped = NULL;
	AxisDbConn *conn;
	Around around;
	gboolean ok;
	guint i;
	guint k;

	*tries = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* What the owner wants now, as a search would load it; whether it may be
	 * searched now does not matter here. */
	ok = self->info->load(self->data, conn, target, &want, &skipped, error);
	if (ok && want == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", skipped != NULL ? skipped : "it was removed");
		ok = FALSE;
	}
	ok = ok && axis_acquire_store_block_keys(self, conn, want->owner_id, &blocklist, error) &&
		(self->info->read_disk == NULL || self->info->read_disk(self->data, conn, want, error));
	for (i = 0; ok && i < profile_ids->len; i++) {
		g_autoptr(GError) local = NULL;

		loaded[i] = axis_quality_store_load(conn, g_array_index(profile_ids, gint64, i), &profiles[i], &local) &&
			axis_quality_profile_check(&profiles[i], NULL);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;

	for (i = 0; i < candidates->len; i++)
		g_ptr_array_add(considered, considered_of(self, want, blocklist, g_ptr_array_index(candidates, i),
			&releases[i]));
	around_init(self, g_get_real_time(), &around);
	*tries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_profile_try_free);
	for (k = 0; k < profile_ids->len; k++) {
		AxisAcquireProfileTry *attempt = g_new0(AxisAcquireProfileTry, 1);

		attempt->profile_id = g_array_index(profile_ids, gint64, k);
		attempt->usable = loaded[k];
		attempt->accepted = g_array_new(FALSE, TRUE, sizeof(gboolean));
		for (i = 0; i < considered->len; i++) {
			AxisAcquireConsidered *c = g_ptr_array_index(considered, i);
			g_autoptr(AxisQualityVerdict) verdict = NULL;
			AxisQualityCandidate input;
			AxisQualityContext context;
			AxisQualityTitle title;
			gboolean accepted = FALSE;

			if (!loaded[k]) {
				g_array_append_val(attempt->accepted, accepted);
				if (attempt->reason == NULL)
					attempt->reason = g_strdup("The profile cannot be used: its definition does not validate.");
				continue;
			}
			/* As decide() does, each with its own file and runtime when the
			 * module decides each release so. */
			memset(&title, 0, sizeof(title));
			title.noun = self->info->decide_each && c->noun != NULL ? c->noun : noun(self);
			title.runtime_minutes = self->info->decide_each ? c->runtime_minutes : want->runtime_minutes;
			around_context(&around, self, &profiles[k], &title, self->info->decide_each ? c->disk :
				want->on_disk ? &want->disk : NULL, &context);
			candidate_input(c, &input);
			verdict = axis_quality_decide_one(&context, &input);
			accepted = verdict != NULL && verdict->accepted && !c->blocklisted && !c->rejected;
			g_array_append_val(attempt->accepted, accepted);
			if (accepted) {
				attempt->n_accepted++;
			} else if (attempt->reason == NULL) {
				if (c->blocklisted) {
					g_autofree gchar *why = axis_acquire_block_words(c->block_reason, c->block_by);

					attempt->reason = g_strdup_printf("It is on the blocklist: %s.", why);
				}
				else if (c->rejected && c->reject_text != NULL)
					attempt->reason = g_strdup(c->reject_text);
				else if (verdict != NULL && verdict->reasons->len > 0)
					attempt->reason = g_strdup(((const AxisQualityReason *)
						g_ptr_array_index(verdict->reasons, 0))->text);
			}
		}
		g_ptr_array_add(*tries, attempt);
	}
	around_clear(&around);
	return TRUE;
}
