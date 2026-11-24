/*
 * axis-conversion-gates.c - the gates of a conversion, the request, and what
 * the worker API calls at claim and at complete
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The gates are worked out from facts read now, never from what a form or a
 * worker says: at request (the confirmation shows them; the request
 * evaluates them again), when a worker claims the encode (with the plan made
 * for that worker's capabilities), and as check 9 just before placement.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-conversion-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "jobs/axis-job-error.h"
#include "library/axis-library-name.h"
#include "storage/axis-file-check.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-storage-fs.h"
#include "storage/axis-storage-open.h"
#include "storage/axis-volume-write.h"
#include "transcode/axis-convert-caps.h"

/* What gate 5 asks. */
#define FILE_GATE "The file is a present, unpinned video of a movies, TV series or other library"

/* The filesystems a replacement trusts its renames on. */
static const gchar *const local_filesystems[] = {
	"ext4", "xfs", "btrfs", "zfs", "f2fs", "tmpfs", "bcachefs", NULL
};

/* ── Facts ──────────────────────────────────────────────────────── */

void
axis_conversion_facts_clear(AxisConversionFacts *facts)
{
	g_clear_pointer(&facts->rows, axis_estimate_rows_free);
	facts->file = NULL;
	g_clear_pointer(&facts->full, axis_media_info_free);
	g_clear_pointer(&facts->root, axis_root_info_free);
	g_clear_pointer(&facts->volume, axis_volume_info_free);
	g_clear_pointer(&facts->revision, axis_profile_revision_unref);
	g_clear_pointer(&facts->source_quality, g_free);
	g_clear_pointer(&facts->container, g_free);
	g_clear_pointer(&facts->new_rel_path, g_free);
	g_clear_pointer(&facts->new_rel_taken, g_free);
	g_clear_pointer(&facts->memory_reason, g_free);
}

/* @rel_path (stored form) with the extension of @container: unchanged when
 * it has it already (".mkv"; ".mp4" or ".m4v"). */
static gchar *
with_extension(
	const gchar *rel_path,
	const gchar *container
){
	g_autoptr(GString) bytes = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *lower = NULL;
	const gchar *slash = strrchr(rel_path, '/');
	const gchar *stored_base = slash != NULL ? slash + 1 : rel_path;
	const gchar *dot;
	GString *out;

	if (!axis_library_name_decode(stored_base, &bytes) || memchr(bytes->str, '\0', bytes->len) != NULL)
		return NULL;
	lower = g_ascii_strdown(bytes->str, -1);
	if (strcmp(container, "mkv") == 0 ? g_str_has_suffix(lower, ".mkv") :
	    (g_str_has_suffix(lower, ".mp4") || g_str_has_suffix(lower, ".m4v")))
		return g_strdup(rel_path);
	dot = strrchr(bytes->str, '.');
	base = dot != NULL && dot != bytes->str ? g_strndup(bytes->str, (gsize)(dot - bytes->str)) : g_strdup(bytes->str);
	out = g_string_new(NULL);
	if (slash != NULL)
		g_string_append_len(out, rel_path, (gssize)(slash - rel_path + 1));
	{
		g_autofree gchar *name = g_strdup_printf("%s.%s", base, container);

		axis_library_name_encode(out, name, strlen(name));
	}
	return g_string_free(out, FALSE);
}

gboolean
axis_conversion_facts_load(
	AxisConversions      *self,
	AxisDbConn           *conn,
	gint64                file_id,
	gint64                revision_id,
	gint64                exclude,
	AxisConversionFacts  *facts,
	GError              **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *file = axis_conversion_int_text(file_id);
	g_autofree gchar *revision = axis_conversion_int_text(revision_id);
	g_autofree gchar *other = axis_conversion_int_text(exclude);
	const gchar *params[3];

	memset(facts, 0, sizeof(*facts));
	if (!axis_estimator_load_files(conn, &file_id, 1, &facts->rows, error))
		return FALSE;
	facts->file = facts->rows->files->len == 1 ? g_ptr_array_index(facts->rows->files, 0) : NULL;
	if (!axis_profile_store_revision_by_id(conn, revision_id, &facts->revision, error))
		return FALSE;
	if (facts->revision != NULL && facts->revision->usable) {
		AxisEstimateTarget built;

		if (axis_profile_revision_target(facts->revision, &built)) {
			axis_estimator_configure(self->estimator, &built, &facts->target);
			facts->has_target = TRUE;
		}
		facts->is_current = facts->revision->current;
		if (facts->revision->kind == AXIS_PROFILE_KIND_VIDEO)
			facts->container = g_strdup(facts->revision->def.container == AXIS_CONTAINER_MP4 ? "mp4" : "mkv");
	}
	if (facts->file == NULL)
		return TRUE;
	/* The estimator's facts decide eligibility; a plan is made from the
	 * probe's whole record (pixel format, colour, HDR10 values, titles and
	 * flags of every stream), the same row. */
	if (facts->file->info != NULL && !axis_probe_load_info(conn, file_id, &facts->full, error))
		return FALSE;
	if (facts->full != NULL && (g_strcmp0(facts->full->status, "ok") != 0 ||
	    facts->full->probe_version != facts->file->info->probe_version))
		g_clear_pointer(&facts->full, axis_media_info_free);
	axis_storage_get_root(self->storage, facts->file->index.root_id, &facts->root, NULL);
	facts->volume = axis_storage_get_volume(self->storage, facts->file->index.volume_id);

	params[0] = file;
	params[1] = revision;
	params[2] = other;
	if (!axis_db_conn_exec(conn, "SELECT f.pinned_downloads,"
	        " coalesce((SELECT c.source_quality FROM conversion c WHERE c.file_id = f.id AND c.state = 'done'"
	        "   AND c.outcome = 'replace' ORDER BY c.id DESC LIMIT 1),"
	        "  (SELECT m.quality FROM movie_file m WHERE m.file_id = f.id),"
	        "  (SELECT s.quality FROM series_file s WHERE s.file_id = f.id)),"
	        " (SELECT reason FROM conversion_memory m WHERE m.file_id = f.id AND m.revision_id = $2::bigint),"
	        " coalesce((SELECT c.id FROM conversion c WHERE c.file_id = f.id AND c.id <> $3::bigint"
	        "   AND c.state IN ('queued', 'encoding', 'verifying', 'placing') LIMIT 1), 0),"
	        " EXISTS (SELECT 1 FROM movie_file m WHERE m.file_id = f.id AND m.kept)"
	        "  OR EXISTS (SELECT 1 FROM series_file s WHERE s.file_id = f.id AND s.kept)"
	        " FROM library_file f WHERE f.id = $1::bigint", 3, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1) {
		gboolean pinned = FALSE;

		axis_db_result_get_boolean(result, 0, 0, &pinned);
		facts->pinned_downloads = pinned;
		if (!axis_db_result_is_null(result, 0, 1))
			facts->source_quality = g_strdup(axis_db_result_get_text(result, 0, 1));
		if (!axis_db_result_is_null(result, 0, 2)) {
			facts->memory = TRUE;
			facts->memory_reason = g_strdup(axis_db_result_get_text(result, 0, 2));
		}
		axis_db_result_get_int64(result, 0, 3, &facts->other_in_flight);
		axis_db_result_get_boolean(result, 0, 4, &facts->kept);
	}
	if (facts->container != NULL) {
		facts->new_rel_path = with_extension(facts->file->index.rel_path, facts->container);
		if (facts->new_rel_path != NULL && strcmp(facts->new_rel_path, facts->file->index.rel_path) != 0) {
			g_autoptr(AxisDbResult) taken = NULL;
			g_autofree gchar *root = axis_conversion_int_text(facts->file->index.root_id);
			const gchar *more[2];

			more[0] = root;
			more[1] = facts->new_rel_path;
			if (!axis_db_conn_exec(conn, "SELECT state FROM library_file WHERE root_id = $1::bigint AND rel_path = $2",
			        2, more, &taken, error))
				return FALSE;
			if (axis_db_result_get_n_rows(taken) > 0)
				facts->new_rel_taken = g_strdup("The index has a file of the new name already.");
		}
	}
	return TRUE;
}

/* ── What other parts say ───────────────────────────────────────── */

void
axis_conversion_inputs_load(
	AxisConversions            *self,
	const AxisConversionFacts  *facts,
	AxisConversionInputs       *inputs
){
	g_autoptr(GError) local = NULL;

	memset(inputs, 0, sizeof(*inputs));
	inputs->verdict = AXIS_VERDICT_UNKNOWN;
	inputs->model = axis_estimator_get_model(self->estimator, &local);
	if (inputs->model == NULL)
		g_message("conversions: the estimates cannot be worked out: %s", local != NULL ? local->message : "?");
	if (facts->file != NULL && facts->revision != NULL)
		inputs->verdict = axis_conversion_verdict(self, facts->file->index.root_id, facts->revision->slug);
}

void
axis_conversion_inputs_clear(AxisConversionInputs *inputs)
{
	if (inputs->model != NULL)
		axis_estimator_model_unref(inputs->model);
	inputs->model = NULL;
}

/* ── The gates ──────────────────────────────────────────────────── */

static void
gate_free(gpointer data)
{
	AxisConversionGate *gate = data;

	g_free(gate->detail);
	g_free(gate);
}

static void
add_gate(
	GPtrArray   *gates,
	const gchar *id,
	const gchar *label,
	gboolean     applies,
	gboolean     passed,
	gchar       *detail
){
	AxisConversionGate *gate = g_new0(AxisConversionGate, 1);

	gate->id = id;
	gate->label = label;
	gate->applies = applies;
	gate->passed = passed;
	gate->detail = detail;
	g_ptr_array_add(gates, gate);
}

const AxisConversionGate *
axis_conversion_preview_refusal(const AxisConversionPreview *preview)
{
	guint i;

	for (i = 0; preview->gates != NULL && i < preview->gates->len; i++) {
		const AxisConversionGate *gate = g_ptr_array_index(preview->gates, i);

		if (gate->applies && !gate->passed)
			return gate;
	}
	return NULL;
}

void
axis_conversion_preview_free(AxisConversionPreview *preview)
{
	if (preview == NULL)
		return;
	g_free(preview->root_name);
	g_free(preview->volume_name);
	g_free(preview->rel_path);
	g_free(preview->new_rel_path);
	if (preview->revision != NULL)
		axis_profile_revision_unref(preview->revision);
	g_free(preview->source_quality);
	g_free(preview->encoder);
	if (preview->gates != NULL)
		g_ptr_array_unref(preview->gates);
	g_free(preview);
}

/* The plan for @facts with @caps and @options (NULL: refusal set). */
static AxisConvertPlan *
plan_with(
	const AxisConversionFacts      *facts,
	const AxisEncoderCapabilities  *caps,
	const AxisConvertOptions       *options,
	AxisConvertRefusal             *refusal
){
	AxisConvertPlan *plan = NULL;

	*refusal = AXIS_CONVERT_REFUSE_NOT_MEASURED;
	if (facts->file == NULL || facts->full == NULL || facts->revision == NULL || !facts->revision->usable) {
		*refusal = AXIS_CONVERT_REFUSE_NOT_PROBED;
		return NULL;
	}
	if (caps == NULL)
		return NULL;
	if (!axis_convert_plan_choose(&facts->revision->def, facts->full, facts->file->index.size, caps, options, &plan,
	                              refusal, NULL))
		return NULL;
	return plan;
}

AxisConvertPlan *
axis_conversion_plan(
	AxisConversions            *self,
	AxisDbConn                 *conn,
	const AxisConversionFacts  *facts,
	const gchar                *machine,
	AxisConvertRefusal         *refusal,
	GError                    **error
){
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	AxisConvertOptions options;

	*refusal = AXIS_CONVERT_REFUSE_NOT_MEASURED;
	if (facts->file == NULL || facts->full == NULL || facts->revision == NULL || !facts->revision->usable) {
		*refusal = AXIS_CONVERT_REFUSE_NOT_PROBED;
		return NULL;
	}
	if (!axis_convert_capabilities_load(conn, machine, &caps, error))
		return NULL;
	axis_conversion_options(self, &options);
	return plan_with(facts, caps, &options, refusal);
}

AxisConvertPlan *
axis_conversion_plan_recorded(
	AxisConversions            *self,
	AxisDbConn                 *conn,
	const AxisConversionFacts  *facts,
	const AxisConversionRow    *row,
	AxisConvertRefusal         *refusal,
	GError                    **error
){
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	g_autofree gchar *problem = NULL;
	AxisConvertOptions options;

	/* A conversion handed out before 0049 recorded nothing of this. */
	if (row->plan_caps == NULL || row->plan_preset == NULL)
		return axis_conversion_plan(self, conn, facts, row->machine, refusal, error);
	caps = axis_worker_tool_parse(row->plan_caps, strlen(row->plan_caps), &problem);
	if (caps == NULL) {
		*refusal = AXIS_CONVERT_REFUSE_NOT_MEASURED;
		g_message("conversions: the capabilities recorded with conversion %" G_GINT64_FORMAT " cannot be read: %s",
			row->id, problem != NULL ? problem : "?");
		return NULL;
	}
	axis_conversion_options_recorded(row->plan_preset, (guint)MAX(row->plan_threads, 0), &options);
	return plan_with(facts, caps, &options, refusal);
}

gchar *
axis_conversion_output_name(
	gint64 id,
	gint   claims
){
	return g_strdup_printf("%" G_GINT64_FORMAT "-%d.tmp", id, MAX(claims, 1));
}

static gboolean
in_list(
	const gchar *const *list,
	const gchar        *word
){
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		if (g_strcmp0(list[i], word) == 0)
			return TRUE;
	}
	return FALSE;
}

gboolean
axis_conversion_gates(
	AxisConversions             *self,
	AxisDbConn                  *conn,
	const AxisConversionFacts   *facts,
	const AxisConversionInputs  *inputs,
	AxisConversionOutcome        outcome,
	gboolean                     convert_kept,
	AxisVolumeGuard             *held,
	gboolean                     plan_machines,
	const AxisConversionRow     *stamp,
	AxisConversionPreview       *preview,
	GPtrArray                   *gates,
	GError                     **error
){
	g_autoptr(AxisVolumeGuard) own = NULL;
	AxisVolumeGuard *guard = held;
	const AxisEstimateFile *file = facts->file;
	gboolean replace = outcome == AXIS_CONVERSION_REPLACE;
	gboolean video_root = facts->root != NULL && (facts->root->media_type == AXIS_MEDIA_MOVIES ||
		facts->root->media_type == AXIS_MEDIA_TV || facts->root->media_type == AXIS_MEDIA_OTHER);
	AxisEstimateResult result;
	gboolean eligible = FALSE;
	gint64 high = -1;
	g_autofree gchar *encoder = NULL;

	memset(&result, 0, sizeof(result));

	/* G2 */
	add_gate(gates, "read_only", "Storage may be written (Read-only storage is off)", TRUE,
		!axis_storage_is_read_only_mode(self->storage),
		axis_storage_is_read_only_mode(self->storage) ? g_strdup("Read-only storage is on: nothing may be written. "
			"An administrator can switch it off in Settings.") : NULL);

	/* G3 */
	if (guard == NULL && file != NULL) {
		GError *local = NULL;

		own = axis_volume_guard_acquire(self->storage, file->index.volume_id, AXIS_VOLUME_INTENT_WRITE, &local);
		guard = own;
		add_gate(gates, "volume", "The disk is online and mounted read-write", TRUE, own != NULL,
			own == NULL ? g_strdup(local->message) : NULL);
		g_clear_error(&local);
	} else {
		add_gate(gates, "volume", "The disk is online and mounted read-write", TRUE, guard != NULL,
			guard == NULL ? g_strdup("The file is not in the index.") : NULL);
	}

	/* G4 */
	{
		const gchar *type = NULL;
		struct statfs fs;

		if (guard != NULL && fstatfs(axis_volume_guard_get_fd(guard), &fs) == 0)
			type = axis_storage_fs_type_name((guint64)fs.f_type);
		else if (facts->volume != NULL)
			type = facts->volume->fs_type;
		if (type != NULL && in_list(local_filesystems, type))
			add_gate(gates, "filesystem", "The disk's filesystem renames files atomically", replace, TRUE, NULL);
		else if (axis_conversion_setting_boolean(self, AXIS_CONVERSION_SETTING_NETWORK_FS, FALSE))
			add_gate(gates, "filesystem", "The disk's filesystem renames files atomically", replace, TRUE,
				g_strdup_printf("Its filesystem is %s; the setting \"Replace files on network filesystems\" accepts it.", type != NULL ? type : "unknown"));
		else
			add_gate(gates, "filesystem", "The disk's filesystem renames files atomically", replace, FALSE,
				g_strdup_printf("Its filesystem is %s, not one of the local filesystems a replacement trusts "
					"(Settings, \"Replace files on network filesystems\").", type != NULL ? type : "unknown"));
	}

	/* G5 */
	if (file == NULL)
		add_gate(gates, "file", FILE_GATE, TRUE, FALSE,
			g_strdup("There is no such file in the index."));
	else if (g_strcmp0(file->index.state, "present") != 0)
		add_gate(gates, "file", FILE_GATE, TRUE, FALSE,
			g_strdup("The last complete scan did not find it."));
	else if (g_strcmp0(file->index.kind, "video") != 0 || !video_root)
		add_gate(gates, "file", FILE_GATE, TRUE, FALSE,
			g_strdup("It is not a video file in a movies, TV series or other library."));
	else if (file->index.pinned || file->index.nlink != 1 || facts->pinned_downloads)
		add_gate(gates, "file", FILE_GATE, TRUE, FALSE,
			g_strdup("It is pinned: it has other hard links (a download client may be seeding it) or is under a "
				"downloads folder."));
	else
		add_gate(gates, "file", FILE_GATE, TRUE, TRUE,
			NULL);

	/* G6 */
	if (facts->revision == NULL)
		add_gate(gates, "revision", "The profile version is usable", TRUE, FALSE,
			g_strdup("There is no such profile version."));
	else if (!facts->revision->usable || !facts->has_target)
		add_gate(gates, "revision", "The profile version is usable", TRUE, FALSE,
			g_strdup(facts->revision->unusable != NULL ? facts->revision->unusable : "Its definition is not usable."));
	else if (facts->revision->kind != AXIS_PROFILE_KIND_VIDEO)
		add_gate(gates, "revision", "The profile version is usable", TRUE, FALSE,
			g_strdup("It is an audio profile; Axis does not convert audio files yet."));
	else
		add_gate(gates, "revision", "The profile version is usable", TRUE, TRUE, NULL);

	/* G7 */
	if (file != NULL && facts->has_target) {
		if (inputs->model == NULL) {
			add_gate(gates, "eligible", "The file is eligible for the profile now", TRUE, FALSE,
				g_strdup("The estimates cannot be worked out just now."));
		} else {
			AxisEstimateReason reason = axis_estimate_eligibility(file, &facts->target, inputs->model, &result);

			eligible = reason == AXIS_ESTIMATE_ELIGIBLE;
			add_gate(gates, "eligible", "The file is eligible for the profile now", TRUE, eligible,
				eligible ? NULL : g_strdup_printf("%s: %s", axis_estimate_reason_label(reason),
					axis_estimate_reason_help(reason)));
			if (result.sized)
				high = result.high;
			if (preview != NULL && result.sized) {
				preview->estimated = TRUE;
				preview->estimate_low = result.low;
				preview->estimate_mid = result.mid;
				preview->estimate_high = result.high;
			}
		}
	} else {
		add_gate(gates, "eligible", "The file is eligible for the profile now", TRUE, FALSE,
			g_strdup("There is nothing to decide it from."));
	}

	/* G8 (at request: some registered worker's capabilities) */
	if (plan_machines) {
		g_autoptr(AxisDbResult) workers = NULL;
		AxisConvertRefusal first = AXIS_CONVERT_OK;
		guint i;

		if (!axis_db_conn_exec(conn, "SELECT name FROM worker WHERE revoked_at IS NULL AND registered_at IS NOT NULL"
		        " ORDER BY last_seen_at DESC NULLS LAST, id LIMIT 50", 0, NULL, &workers, error))
			return FALSE;
		for (i = 0; encoder == NULL && i < axis_db_result_get_n_rows(workers); i++) {
			g_autofree gchar *machine = axis_worker_machine(axis_db_result_get_text(workers, i, 0));
			g_autoptr(AxisConvertPlan) plan = NULL;
			AxisConvertRefusal refusal = AXIS_CONVERT_OK;

			plan = axis_conversion_plan(self, conn, facts, machine, &refusal, error);
			if (plan == NULL && error != NULL && *error != NULL)
				return FALSE;
			if (plan != NULL)
				encoder = g_strdup(plan->encoder->name);
			else if (first == AXIS_CONVERT_OK)
				first = refusal;
		}
		if (encoder != NULL)
			add_gate(gates, "plan", "A worker can make this conversion", TRUE, TRUE,
				g_strdup_printf("With %s.", encoder));
		else if (axis_db_result_get_n_rows(workers) == 0)
			add_gate(gates, "plan", "A worker can make this conversion", TRUE, FALSE,
				g_strdup("There is no registered conversion worker (Conversion workers)."));
		else
			add_gate(gates, "plan", "A worker can make this conversion", TRUE, FALSE,
				g_strdup_printf("No registered worker can: %s", axis_convert_refusal_describe(first)));
		if (preview != NULL) {
			g_free(preview->encoder);
			preview->encoder = g_strdup(encoder);
		}
	}

	/* G9 */
	{
		gboolean proven = facts->revision != NULL && axis_profile_revision_is_unlocked(facts->revision);

		const gchar *how = !proven ? NULL : g_strcmp0(facts->revision->proof_method, "trials") == 0 ?
			"Proven by hand from passed trials." : g_strcmp0(facts->revision->proof_method, "carried") == 0 ?
			"Proven: an earlier version was, and this one changed nothing that needs proving again." :
			"Proven on the household's devices.";

		add_gate(gates, "proven", "The profile version is proven", replace, proven,
			proven ? g_strdup(how) :
			g_strdup("It is locked: a version is proven by hand from passed trials on the Conversions page. "
				"Trials are allowed with a locked version."));
	}

	/* G10 */
	{
		AxisCompatVerdict verdict = inputs->verdict;
		gboolean ok;

		if (preview != NULL)
			preview->verdict = verdict;
		ok = verdict == AXIS_VERDICT_SAFE && facts->is_current;
		add_gate(gates, "verdict", "The playback verdict for this library folder is safe", replace, ok,
			!facts->is_current ? g_strdup("The playback verdict is for a profile's current version only, and "
				"this is not the current one.") :
			verdict == AXIS_VERDICT_SAFE ? NULL :
			g_strdup_printf("The verdict is %s; a replacement needs safe.",
				axis_compat_verdict_to_string(verdict)));
	}

	/* G11 */
	add_gate(gates, "in_flight", "No other conversion of the file is in flight", TRUE, facts->other_in_flight == 0,
		facts->other_in_flight != 0 ? g_strdup_printf("Conversion %" G_GINT64_FORMAT " of it is in flight.",
			facts->other_in_flight) : NULL);

	/* G11b: a file a person keeps by hand, or one brought over from Radarr or
	 * Sonarr, is not replaced by a conversion until a person lets it go (the
	 * keep is read now, so lifting it makes the file eligible again).  An
	 * administrator may choose to convert kept files (convert-kept-files):
	 * @convert_kept lifts this gate, and this gate only; the mark is read
	 * here and written nowhere, so the converted file is as kept as its
	 * original was. */
	add_gate(gates, "kept", "The file is not kept by hand or brought over", replace, !facts->kept || convert_kept,
		!facts->kept ? NULL :
		g_strdup(convert_kept ? AXIS_CONVERSION_KEPT_CHOSEN_TEXT : AXIS_CONVERSION_KEPT_TEXT));

	/* G12 */
	if (guard != NULL) {
		struct statvfs vfs;

		if (fstatvfs(axis_volume_guard_get_fd(guard), &vfs) != 0) {
			add_gate(gates, "space", "Enough free space stays on the disk", TRUE, FALSE,
				g_strdup("The disk's free space cannot be read."));
		} else {
			guint64 total = (guint64)vfs.f_blocks * (guint64)vfs.f_frsize;
			guint64 available = (guint64)vfs.f_bavail * (guint64)vfs.f_frsize;
			guint64 floor_bytes = (guint64)MAX(axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_MIN_FREE,
				G_GINT64_CONSTANT(10) * 1024 * 1024 * 1024), 0);
			guint64 share = total / 100 * (guint64)CLAMP(axis_conversion_setting_integer(self,
				AXIS_CONVERSION_SETTING_MIN_FREE_PERCENT, 5), 0, 50);
			/* At verification the output is on the volume already; before, it
			 * may grow to the estimate's high figure (to the source's size
			 * when there is no estimate). */
			guint64 need = (stamp != NULL && stamp->output_reported > 0 ? 0 : (guint64)(high >= 0 ? high :
				file != NULL ? file->index.size : 0)) + MAX(floor_bytes, share);
			g_autofree gchar *left = axis_size_format(available);
			g_autofree gchar *needed = axis_size_format(need);

			add_gate(gates, "space", "Enough free space stays on the disk", TRUE, available >= need,
				g_strdup_printf("%s free; %s needed (the output and the free-space floor).", left, needed));
		}
	} else {
		add_gate(gates, "space", "Enough free space stays on the disk", TRUE, FALSE,
			g_strdup("The disk cannot be looked at."));
	}

	/* G13 */
	add_gate(gates, "memory", "The file is not remembered as not worth it, or as converted, with this version", TRUE,
		!facts->memory, !facts->memory ? NULL : g_strcmp0(facts->memory_reason, "converted") == 0 ?
		g_strdup("It was converted with this version already.") :
		g_strdup("An earlier conversion with this version was not smaller by the profile's minimum saving."));

	/* G14 */
	if (replace && facts->new_rel_path != NULL && file != NULL &&
	    strcmp(facts->new_rel_path, file->index.rel_path) != 0) {
		g_autofree gchar *taken = facts->new_rel_taken != NULL ? g_strdup(facts->new_rel_taken) : NULL;

		if (taken == NULL && guard != NULL && facts->root != NULL) {
			g_autofree gchar *folder = NULL;
			g_autofree gchar *name = NULL;

			if (axis_conversion_split(facts->root->rel_path, facts->new_rel_path, &folder, &name)) {
				g_autoptr(GError) local = NULL;
				gint dir = axis_volume_guard_open_dir(guard, folder, &local);
				struct stat st;

				if (dir >= 0) {
					if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
						taken = g_strdup("A file of the new name is in the folder already.");
					close(dir);
				} else {
					/* Not known to be free: refused. */
					taken = g_strdup_printf("The file's folder cannot be looked at: %s", local->message);
				}
			} else {
				taken = g_strdup("The new name cannot be made.");
			}
		}
		{
			/* Decided before the text is handed over: the order in which a
			 * call's arguments are worked out is not C's to promise. */
			gboolean free_name = taken == NULL;

			add_gate(gates, "new_name", "The new name (the profile's container) is free", TRUE, free_name,
				g_steal_pointer(&taken));
		}
	} else {
		add_gate(gates, "new_name", "The new name (the profile's container) is free", FALSE, TRUE,
			g_strdup("The file keeps its name."));
	}

	/* G15 */
	if (stamp != NULL) {
		gboolean same = file != NULL && file->index.size == stamp->src_size &&
			file->index.mtime_sec == stamp->src_mtime_sec && file->index.mtime_nsec == stamp->src_mtime_nsec &&
			file->index.inode == stamp->src_inode && file->index.nlink == stamp->src_nlink;

		add_gate(gates, "stamp", "The file is the one the conversion was requested for", TRUE, same,
			same ? NULL : g_strdup("Its size, time, inode or links are not what they were at the request."));
	}
	return TRUE;
}

/* ── Requesting ─────────────────────────────────────────────────── */

static gboolean
build_preview(
	AxisConversions            *self,
	AxisDbConn                 *conn,
	const AxisConversionFacts  *facts,
	const AxisConversionInputs *inputs,
	AxisConversionOutcome       outcome,
	gboolean                    convert_kept,
	AxisConversionPreview     **out,
	GError                    **error
){
	g_autoptr(AxisConversionPreview) preview = g_new0(AxisConversionPreview, 1);

	preview->outcome = outcome;
	preview->kept = facts->kept;
	preview->convert_kept = convert_kept;
	preview->gates = g_ptr_array_new_with_free_func(gate_free);
	preview->keep_seconds = axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_KEEP_ORIGINAL, 0);
	if (facts->file != NULL) {
		preview->file_id = facts->file->index.file_id;
		preview->root_id = facts->file->index.root_id;
		preview->volume_id = facts->file->index.volume_id;
		preview->rel_path = g_strdup(facts->file->index.rel_path);
		preview->size = facts->file->index.size;
	}
	preview->new_rel_path = g_strdup(facts->new_rel_path);
	preview->root_name = facts->root != NULL ? g_strdup(facts->root->name) : NULL;
	preview->volume_name = facts->volume != NULL ? g_strdup(facts->volume->name) : NULL;
	preview->revision = facts->revision != NULL ? axis_profile_revision_ref(facts->revision) : NULL;
	preview->source_quality = g_strdup(facts->source_quality);
	if (!axis_conversion_gates(self, conn, facts, inputs, outcome, convert_kept, NULL, TRUE, NULL, preview,
	        preview->gates, error))
		return FALSE;
	preview->ok = axis_conversion_preview_refusal(preview) == NULL;
	*out = g_steal_pointer(&preview);
	return TRUE;
}

gboolean
axis_conversions_preview(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	AxisConversionPreview **preview,
	GError                **error
){
	return axis_conversions_preview_full(self, file_id, revision_id, outcome, FALSE, preview, error);
}

gboolean
axis_conversions_preview_full(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                convert_kept,
	AxisConversionPreview **preview,
	GError                **error
){
	AxisConversionFacts facts;
	AxisConversionInputs inputs;
	AxisDbConn *conn;
	gboolean ok;

	*preview = NULL;
	memset(&inputs, 0, sizeof(inputs));
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_conversion_facts_load(self, conn, file_id, revision_id, 0, &facts, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && facts.file == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such file.");
		ok = FALSE;
	} else if (ok && facts.revision == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND,
			"There is no such profile version.");
		ok = FALSE;
	}
	if (ok) {
		axis_conversion_inputs_load(self, &facts, &inputs);
		conn = axis_db_pool_acquire(self->pool, error);
		ok = conn != NULL && build_preview(self, conn, &facts, &inputs, outcome, convert_kept, preview, error);
		if (conn != NULL)
			axis_db_pool_release(self->pool, conn);
	}
	axis_conversion_inputs_clear(&inputs);
	axis_conversion_facts_clear(&facts);
	return ok;
}

/* The probe's HDR class of the source, as recorded with the request:
 * "sdr" only when the probe is certain of it; anything uncertain or not
 * classified is "unclassified". */
static const gchar *
source_hdr_class(const AxisEstimateFile *file)
{
	const gchar *name;

	if (file == NULL || file->info == NULL || file->info->hdr_uncertain)
		return "unclassified";
	name = axis_hdr_format_to_string(file->info->hdr_format);
	return name != NULL ? name : "unclassified";
}

/*
 * The one request: a person's from the confirmation page, or a run's for
 * the person who started it (@run_id, @entry_id; 0 for none), with exactly
 * the same gates.  *@refused_gate (when given) is the id of the gate that
 * refused, when one did.  @convert_kept_by is the person who chose to
 * convert kept files (the person's tick, or the run's choice), NULL for no
 * choice.  What is recorded with the conversion is whether the choice
 * covered the file: chosen, and the file kept now.  The claim and check 10
 * read the gate "kept" with that, so a file that becomes kept after the
 * request is refused there as without the choice.
 */
static gboolean
request(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *convert_kept_by,
	const gchar            *user,
	gint64                  run_id,
	gint64                  entry_id,
	gint64                 *conversion_id,
	gchar                 **refused_gate,
	GError                **error
){
	g_autoptr(AxisConversionPreview) preview = NULL;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(GError) local = NULL;
	AxisConversionFacts facts;
	AxisConversionInputs inputs;
	const AxisConversionGate *refusal;
	const AxisEstimateFile *file;
	struct stat now;
	AxisDbConn *conn;
	gboolean convert_kept = convert_kept_by != NULL && convert_kept_by[0] != '\0';
	gboolean covered = FALSE;
	gboolean ok = FALSE;

	*conversion_id = 0;
	if (refused_gate != NULL)
		*refused_gate = NULL;
	memset(&inputs, 0, sizeof(inputs));
	memset(&facts, 0, sizeof(facts));
	if (user == NULL || user[0] == '\0') {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "A conversion needs a person.");
		return FALSE;
	}
	if (outcome == AXIS_CONVERSION_REPLACE && !confirmed) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
			"A replacement deletes the original: it needs the confirmation ticked.");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_conversion_facts_load(self, conn, file_id, revision_id, 0, &facts, error))
		goto out;
	if (facts.file == NULL || facts.revision == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND,
			facts.file == NULL ? "There is no such file." : "There is no such profile version.");
		goto out;
	}
	/* What other parts say, with no connection held here. */
	axis_db_pool_release(self->pool, conn);
	axis_conversion_inputs_load(self, &facts, &inputs);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		goto out;
	if (!build_preview(self, conn, &facts, &inputs, outcome, convert_kept, &preview, error))
		goto out;
	refusal = axis_conversion_preview_refusal(preview);
	if (refusal != NULL) {
		if (refused_gate != NULL)
			*refused_gate = g_strdup(refusal->id);
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "%s: %s", refusal->label,
			refusal->detail != NULL ? refusal->detail : "it does not hold.");
		goto out;
	}

	/* The work area, made now if it is not there, and the file itself as
	 * it is now: that is the stamp every later step compares with. */
	{
		g_autoptr(AxisVolumeGuard) guard = axis_volume_guard_acquire(self->storage, facts.file->index.volume_id,
			AXIS_VOLUME_INTENT_WRITE, &local);
		gint work = guard != NULL ? axis_conversion_work_area(guard, TRUE, &local) : -1;
		AxisStorageOpenStatus opened;
		gint saved = 0;
		gint fd = -1;

		if (work < 0) {
			g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
				"The disk's work area cannot be used: %s", local->message);
			goto out;
		}
		close(work);
		opened = axis_storage_open_file_beneath(axis_volume_guard_get_fd(guard),
			facts.root != NULL ? facts.root->rel_path : "", facts.file->index.rel_path, &fd, &now, &saved);
		if (opened != AXIS_STORAGE_OPEN_OK) {
			g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "The file cannot be opened: %s",
				axis_storage_open_status_describe(opened));
			goto out;
		}
		close(fd);
		if ((gint64)now.st_size != facts.file->index.size || (gint64)now.st_mtim.tv_sec != facts.file->index.mtime_sec ||
		    (gint)now.st_mtim.tv_nsec != facts.file->index.mtime_nsec || (gint64)now.st_ino != facts.file->index.inode ||
		    now.st_nlink != 1) {
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
				"The file on disk is not what the index says (it changed since the last scan, or has another hard "
				"link now): scan its library and look again.");
			goto out;
		}
	}

	file = facts.file;
	/* The choice is recorded for a file it covers: one that is kept now.  A
	 * file that is not kept is asked for as without it, so that the mark set
	 * on it later is read at the claim and at check 10 as it would be. */
	covered = convert_kept && facts.kept;
	if (!axis_db_conn_begin(conn, error))
		goto out;
	{
		g_autofree gchar *file_text = axis_conversion_int_text(file->index.file_id);
		g_autofree gchar *volume = axis_conversion_int_text(file->index.volume_id);
		g_autofree gchar *root = axis_conversion_int_text(file->index.root_id);
		g_autofree gchar *revision = axis_conversion_int_text(revision_id);
		g_autofree gchar *size = axis_conversion_int_text((gint64)now.st_size);
		g_autofree gchar *ms = axis_conversion_int_text((gint64)now.st_mtim.tv_sec);
		g_autofree gchar *mn = axis_conversion_int_text((gint64)now.st_mtim.tv_nsec);
		g_autofree gchar *cs = axis_conversion_int_text((gint64)now.st_ctim.tv_sec);
		g_autofree gchar *cn = axis_conversion_int_text((gint64)now.st_ctim.tv_nsec);
		g_autofree gchar *ino = axis_conversion_int_text((gint64)now.st_ino);
		g_autofree gchar *links = axis_conversion_int_text((gint64)now.st_nlink);
		g_autofree gchar *estimate = preview->estimated ? axis_conversion_int_text(preview->estimate_mid) : NULL;
		g_autofree gchar *run = run_id > 0 ? axis_conversion_int_text(run_id) : NULL;
		g_autofree gchar *entry = entry_id > 0 ? axis_conversion_int_text(entry_id) : NULL;
		const gchar *params[23];

		params[0] = file_text;
		params[1] = volume;
		params[2] = root;
		params[3] = facts.root != NULL ? facts.root->rel_path : "";
		params[4] = file->index.rel_path;
		params[5] = facts.new_rel_path != NULL ? facts.new_rel_path : file->index.rel_path;
		params[6] = facts.container;
		params[7] = revision;
		params[8] = axis_conversion_outcome_to_string(outcome);
		params[9] = user;
		params[10] = axis_compat_verdict_to_string(preview->verdict);
		params[11] = size;
		params[12] = ms;
		params[13] = mn;
		params[14] = cs;
		params[15] = cn;
		params[16] = ino;
		params[17] = links;
		params[18] = facts.source_quality;
		params[19] = source_hdr_class(file);
		params[20] = run;
		params[21] = entry;
		params[22] = covered ? "true" : "false";
		if (!axis_db_conn_exec(conn, "INSERT INTO conversion (file_id, volume_id, root_id, root_path, rel_path,"
		        " new_rel_path, container, revision_id, outcome, requested_by, verdict, src_size, src_mtime_sec,"
		        " src_mtime_nsec, src_ctime_sec, src_ctime_nsec, src_inode, src_nlink, source_quality, source_hdr,"
		        " run_id, run_entry_id, convert_kept)"
		        " VALUES ($1::bigint, $2::bigint, $3::bigint, $4, $5, $6, $7, $8::bigint, $9, $10, $11, $12::bigint,"
		        "  $13::bigint, $14::integer, $15::bigint, $16::integer, $17::bigint, $18::bigint, $19, $20,"
		        "  $21::bigint, $22::bigint, $23::boolean) RETURNING id", 23,
		        params, &inserted, error))
			goto rollback;
		axis_db_result_get_int64(inserted, 0, 0, conversion_id);
		if (estimate != NULL) {
			g_autofree gchar *id = axis_conversion_int_text(*conversion_id);
			const gchar *more[2];

			more[0] = id;
			more[1] = estimate;
			if (!axis_db_conn_exec(conn, "UPDATE conversion SET estimate_bytes = $2::bigint WHERE id = $1::bigint", 2,
			        more, NULL, error))
				goto rollback;
		}
	}
	/* The encode job: for a worker that has proved the volume. */
	{
		g_autoptr(JsonObject) requires = json_object_new();
		g_autoptr(JsonArray) volumes = json_array_new();
		g_autofree gchar *key = g_strdup_printf(AXIS_WORKER_KIND_CONVERT ":%" G_GINT64_FORMAT, *conversion_id);
		g_autofree gchar *id = axis_conversion_int_text(*conversion_id);
		g_autofree gchar *job_text = NULL;
		const AxisConvertEncoder *encoder = preview->encoder != NULL ? axis_convert_encoder_named(preview->encoder) :
			NULL;
		const gchar *more[2];
		AxisJobSpec spec;
		gint64 job = 0;

		payload = json_object_new();
		json_array_add_string_element(volumes, facts.volume != NULL ? facts.volume->uuid : "");
		json_object_set_array_member(requires, "volumes", json_array_ref(volumes));
		if (encoder != NULL) {
			json_object_set_string_member(requires, "encoder", encoder->name);
			json_object_set_boolean_member(requires, "ten_bit", encoder->ten_bit &&
				(facts.revision->def.bit_depth == 10 || (facts.file->info != NULL &&
				 (facts.file->info->hdr_format == AXIS_HDR_HDR10 || facts.file->info->hdr_format == AXIS_HDR_HLG))));
		}
		json_object_set_int_member(payload, "conversion", *conversion_id);
		json_object_set_object_member(payload, "requires", json_object_ref(requires));
		memset(&spec, 0, sizeof(spec));
		spec.kind = AXIS_WORKER_KIND_CONVERT;
		spec.queue = AXIS_WORKER_QUEUE;
		spec.payload = payload;
		spec.dedupe_key = key;
		spec.max_attempts = AXIS_CONVERSION_ENCODE_ATTEMPTS;
		if (!axis_job_enqueue(conn, &spec, &job, NULL, error))
			goto rollback;
		job_text = axis_conversion_int_text(job);
		more[0] = id;
		more[1] = job_text;
		if (!axis_db_conn_exec(conn, "UPDATE conversion SET job_id = $2::bigint WHERE id = $1::bigint", 2, more, NULL,
		        error))
			goto rollback;
	}
	ok = axis_db_conn_commit(conn, error);
	if (!ok)
		goto rollback;
	if (run_id > 0 && covered)
		g_message("audit: %s requested conversion %" G_GINT64_FORMAT " (%s) of file %" G_GINT64_FORMAT " with revision %"
			G_GINT64_FORMAT " through conversion run %" G_GINT64_FORMAT "; the file is kept, and %s chose to convert "
			"the files Axis keeps in this run", user, *conversion_id, axis_conversion_outcome_to_string(outcome), file_id,
			revision_id, run_id, convert_kept_by);
	else if (run_id > 0)
		g_message("audit: %s requested conversion %" G_GINT64_FORMAT " (%s) of file %" G_GINT64_FORMAT " with revision %"
			G_GINT64_FORMAT " through conversion run %" G_GINT64_FORMAT, user, *conversion_id,
			axis_conversion_outcome_to_string(outcome), file_id, revision_id, run_id);
	else
		g_message("audit: %s requested conversion %" G_GINT64_FORMAT " (%s) of file %" G_GINT64_FORMAT " with revision %"
			G_GINT64_FORMAT "%s", user, *conversion_id, axis_conversion_outcome_to_string(outcome), file_id, revision_id,
			covered ? "; the file is kept, and they chose to convert it" : "");
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
	/* The unique index: another request of the file won the race. */
	if (error != NULL && *error != NULL && strstr((*error)->message, "conversion_in_flight") != NULL) {
		g_clear_error(error);
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
			"Another conversion of the file is in flight.");
	}
	*conversion_id = 0;
	ok = FALSE;
out:
	if (conn != NULL)
		axis_db_pool_release(self->pool, conn);
	axis_conversion_inputs_clear(&inputs);
	axis_conversion_facts_clear(&facts);
	return ok;
}

gboolean
axis_conversions_request(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *user,
	gint64                 *conversion_id,
	GError                **error
){
	return request(self, file_id, revision_id, outcome, confirmed, NULL, user, 0, 0, conversion_id, NULL, error);
}

gboolean
axis_conversions_request_full(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	gboolean                convert_kept,
	const gchar            *user,
	gint64                 *conversion_id,
	GError                **error
){
	return request(self, file_id, revision_id, outcome, confirmed, convert_kept ? user : NULL, user, 0, 0,
		conversion_id, NULL, error);
}

gboolean
axis_conversions_request_for_run(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *convert_kept_by,
	const gchar            *user,
	gint64                  run_id,
	gint64                  entry_id,
	gint64                 *conversion_id,
	gchar                 **refused_gate,
	GError                **error
){
	g_return_val_if_fail(run_id > 0 && entry_id > 0, FALSE);

	return request(self, file_id, revision_id, outcome, confirmed, convert_kept_by, user, run_id, entry_id,
		conversion_id, refused_gate, error);
}

gboolean
axis_conversions_offer(
	AxisConversions  *self,
	GPtrArray       **out,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GPtrArray *list;
	guint i;

	*out = NULL;
	if (!axis_db_pool_exec(self->pool, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
	        " WHERE p.current_revision_id = r.id AND p.kind = 'video' AND NOT p.archived"
	        " ORDER BY p.built_in DESC, p.id", 0, NULL, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_revision_unref);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisProfileRevision *revision = axis_profile_revision_from_row(result, i, 0);

		if (revision != NULL && revision->usable)
			g_ptr_array_add(list, revision);
		else if (revision != NULL)
			axis_profile_revision_unref(revision);
	}
	*out = list;
	return TRUE;
}

/* ── The claim ──────────────────────────────────────────────────── */

/*
 * The source as the daemon sees it when it hands the work out: opened
 * beneath the volume (a read guard), it must still have the whole stamp
 * recorded at the request (what check 2 will compare: size, both times to
 * the nanosecond, inode, one link), and its samples are read for the
 * worker to compare its own view with (storage/axis-file-check.h).  The
 * gates compared the index with the stamp; this compares the disk.
 * FALSE with *@permanent when the file is not the one requested any more;
 * FALSE without it when it cannot be looked at now.
 */
static gboolean
source_sample(
	AxisConversions          *self,
	const AxisConversionRow  *row,
	gchar                   **digest,
	gchar                   **why,
	gboolean                 *permanent
){
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	AxisStorageOpenStatus opened;
	struct stat st;
	gint saved = 0;
	gint err = 0;
	gint fd = -1;

	*digest = NULL;
	*why = NULL;
	*permanent = FALSE;
	guard = axis_volume_guard_acquire(self->storage, row->volume_id, AXIS_VOLUME_INTENT_READ, &local);
	if (guard == NULL) {
		*why = g_strdup_printf("Not given to a worker now: the disk cannot be read: %s", local->message);
		return FALSE;
	}
	opened = axis_storage_open_file_beneath(axis_volume_guard_get_fd(guard), row->root_path, row->rel_path, &fd, &st,
		&saved);
	if (opened == AXIS_STORAGE_OPEN_FAILED) {
		*why = g_strdup_printf("Not given to a worker now: the file cannot be opened: %s", g_strerror(saved));
		return FALSE;
	}
	if (opened != AXIS_STORAGE_OPEN_OK) {
		*why = g_strdup_printf("Refused when a worker took it: the file cannot be opened: %s.",
			axis_storage_open_status_describe(opened));
		*permanent = TRUE;
		return FALSE;
	}
	if ((gint64)st.st_size != row->src_size || (gint64)st.st_mtim.tv_sec != row->src_mtime_sec ||
	    (gint)st.st_mtim.tv_nsec != row->src_mtime_nsec || (gint64)st.st_ctim.tv_sec != row->src_ctime_sec ||
	    (gint)st.st_ctim.tv_nsec != row->src_ctime_nsec || (gint64)st.st_ino != row->src_inode || st.st_nlink != 1) {
		close(fd);
		*why = g_strdup("Refused when a worker took it: the file on disk is not the one the conversion was requested "
			"for (its size, times, inode or links changed since).");
		*permanent = TRUE;
		return FALSE;
	}
	if (!axis_file_sample_digest(fd, row->src_size, digest, &err)) {
		close(fd);
		*why = g_strdup_printf("Not given to a worker now: the file cannot be read: %s", g_strerror(err));
		return FALSE;
	}
	close(fd);
	return TRUE;
}

/* The JSON array of @args. */
static gchar *
args_json(gchar **args)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_array(builder);
	for (i = 0; args[i] != NULL; i++)
		json_builder_add_string_value(builder, args[i]);
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

gboolean
axis_conversions_describe(
	AxisConversions     *self,
	AxisDbConn          *conn,
	const AxisJobClaim  *claim,
	const gchar         *worker,
	const gchar         *machine,
	AxisConvertWork    **work_out,
	gchar              **why_not,
	gboolean            *permanent,
	GError             **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autoptr(GPtrArray) gates = g_ptr_array_new_with_free_func(gate_free);
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(AxisConvertWork) work = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *sample = NULL;
	g_autofree gchar *plan_caps = NULL;
	g_autofree gchar *plan_preset = NULL;
	g_autofree gchar *plan_threads = NULL;
	AxisConversionFacts facts;
	AxisConversionInputs inputs;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	gboolean ok = FALSE;
	gint64 max_index = -1;
	guint i;

	*work_out = NULL;
	*why_not = NULL;
	*permanent = FALSE;
	memset(&facts, 0, sizeof(facts));
	memset(&inputs, 0, sizeof(inputs));
	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!axis_conversion_row_by_job(conn, claim->id, &row, error))
		goto rollback;
	if (row == NULL || (g_strcmp0(row->state, "queued") != 0 && g_strcmp0(row->state, "encoding") != 0)) {
		*why_not = g_strdup("the conversion is not waiting for a worker");
		*permanent = TRUE;
		ok = axis_db_conn_commit(conn, error);
		goto out;
	}
	if (row->file_id <= 0 || !axis_conversion_facts_load(self, conn, row->file_id, row->revision_id, row->id, &facts,
	                                                     error)) {
		if (row->file_id > 0)
			goto rollback;
	}
	axis_conversion_inputs_load(self, &facts, &inputs);
	if (!axis_conversion_gates(self, conn, &facts, &inputs, row->outcome, row->convert_kept, NULL, FALSE, row, NULL,
	        gates, error))
		goto rollback;
	for (i = 0; i < gates->len; i++) {
		const AxisConversionGate *gate = g_ptr_array_index(gates, i);

		if (gate->applies && !gate->passed) {
			g_autofree gchar *text = g_strdup_printf("Refused when a worker took it: %s: %s", gate->label,
				gate->detail != NULL ? gate->detail : "it does not hold.");

			/* A volume that is away, a scan, read-only for now: later.
			 * Anything else ends the conversion. */
			if (g_strcmp0(gate->id, "volume") == 0 || g_strcmp0(gate->id, "read_only") == 0 ||
			    g_strcmp0(gate->id, "space") == 0) {
				*why_not = g_strdup(text);
			} else {
				*why_not = g_strdup(text);
				*permanent = TRUE;
				if (!axis_conversion_set_state(conn, row->id, "refused", 0, text, error))
					goto rollback;
			}
			ok = axis_db_conn_commit(conn, error);
			goto out;
		}
	}
	/* The source on disk, and the samples the worker compares its view
	 * with. */
	{
		g_autofree gchar *text = NULL;
		gboolean gone = FALSE;

		if (!source_sample(self, row, &sample, &text, &gone)) {
			*why_not = g_strdup(text);
			*permanent = gone;
			if (gone && !axis_conversion_set_state(conn, row->id, "refused", 0, text, error))
				goto rollback;
			ok = axis_db_conn_commit(conn, error);
			goto out;
		}
	}
	/* The plan, and what it is made from besides the file and the revision
	 * (this worker's measured encoders, the preset and threads now): kept,
	 * so that the checks make the same plan again whatever changes meanwhile. */
	{
		AxisConvertOptions options;

		if (!axis_convert_capabilities_load(conn, machine, &caps, error))
			goto rollback;
		axis_conversion_options(self, &options);
		plan = plan_with(&facts, caps, &options, &refusal);
		if (plan != NULL) {
			plan_caps = axis_worker_tool_to_json(caps);
			plan_preset = g_strdup(options.software_preset);
			plan_threads = axis_conversion_int_text((gint64)options.threads);
		}
	}
	if (plan == NULL) {
		*why_not = g_strdup_printf("this worker cannot make the plan: %s", axis_convert_refusal_describe(refusal));
		ok = axis_db_conn_commit(conn, error);
		goto out;
	}
	/* A name of its own for every claim: a job given back uses no attempt,
	 * and a name an earlier claim left in the work area never blocks this
	 * one. */
	if (row->claims >= 999) {
		g_autofree gchar *text = g_strdup("Refused when a worker took it: it was handed out 999 times.");

		*why_not = g_strdup(text);
		*permanent = TRUE;
		if (!axis_conversion_set_state(conn, row->id, "failed", 0, text, error))
			goto rollback;
		ok = axis_db_conn_commit(conn, error);
		goto out;
	}
	for (i = 0; i < facts.full->streams->len; i++)
		max_index = MAX(max_index, ((const AxisMediaStream *)g_ptr_array_index(facts.full->streams, i))->index);
	output = axis_conversion_output_name(row->id, row->claims + 1);
	args = args_json(plan->args);
	{
		g_autofree gchar *id = axis_conversion_int_text(row->id);
		g_autofree gchar *attempt = axis_conversion_int_text(MAX(claim->attempt, 1));
		g_autofree gchar *claims = axis_conversion_int_text(row->claims + 1);
		g_autofree gchar *clean_worker = axis_library_text_clean(worker, 64);
		const gchar *made = axis_hdr_format_to_string(plan->hdr_format);
		const gchar *params[12];

		params[0] = id;
		params[1] = attempt;
		params[2] = clean_worker;
		params[3] = machine;
		params[4] = plan->encoder->name;
		params[5] = args;
		params[6] = output;
		/* The class of the picture the plan makes: with the source's class
		 * recorded at the request, what makes a replaced file known SDR
		 * (conversion_known_sdr, 0046). */
		params[7] = made != NULL ? made : "unclassified";
		params[8] = claims;
		params[9] = plan_caps;
		params[10] = plan_preset;
		params[11] = plan_threads;
		if (!axis_db_conn_exec(conn, "UPDATE conversion SET state = 'encoding', attempt = $2::integer, worker = $3,"
		        " machine = left($4, 80), encoder = $5, args = $6, output_name = $7, made_hdr = $8, claims = $9::integer,"
		        " plan_caps = $10, plan_preset = $11, plan_threads = $12::integer, updated_at = now(),"
		        " result_text = NULL WHERE id = $1::bigint", 12, params, NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;

	work = g_new0(AxisConvertWork, 1);
	work->volume = g_strdup(facts.volume != NULL ? facts.volume->uuid : "");
	work->root = g_strdup(row->root_path);
	work->path = g_strdup(row->rel_path);
	work->size = row->src_size;
	work->mtime_sec = row->src_mtime_sec;
	work->mtime_nsec = row->src_mtime_nsec;
	work->inode = row->src_inode;
	work->source_sample = g_steal_pointer(&sample);
	work->output = g_steal_pointer(&output);
	work->args = g_strdupv(plan->args);
	work->source_streams = (guint)MAX(max_index + 1, 1);
	work->duration_ms = MAX(plan->duration_ms, 0);
	work->timeout_ms = (guint)CLAMP(600000 + 50 * MAX(plan->duration_ms, 0), AXIS_CONVERT_MIN_TIMEOUT_MS,
		(gint64)AXIS_CONVERT_MAX_TIMEOUT_MS);
	{
		g_autofree gchar *problem = NULL;

		if (!axis_convert_work_check(work, &problem)) {
			g_autofree gchar *text = g_strdup_printf("The work cannot be described: %s", problem);
			GError *local = NULL;

			*why_not = g_strdup(text);
			*permanent = TRUE;
			if (!axis_conversion_set_state(conn, row->id, "failed", 0, text, &local))
				g_clear_error(&local);
			ok = TRUE;
			goto out;
		}
	}
	*work_out = g_steal_pointer(&work);
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
	ok = FALSE;
out:
	axis_conversion_inputs_clear(&inputs);
	axis_conversion_facts_clear(&facts);
	return ok;
}

/* ── The result ─────────────────────────────────────────────────── */

gboolean
axis_conversions_encoded(
	AxisConversions          *self,
	AxisDbConn               *conn,
	gint64                    job,
	const AxisConvertResult  *result,
	const gchar              *worker,
	GError                  **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *size = NULL;
	const gchar *params[2];

	if (!axis_conversion_row_by_job(conn, job, &row, error))
		return FALSE;
	if (row == NULL || g_strcmp0(row->state, "encoding") != 0) {
		g_message("conversions: worker %s reported an encode of job %" G_GINT64_FORMAT " that is not being encoded; "
			"what it made is left to the tidy job", worker, job);
		return TRUE;
	}
	if (g_strcmp0(result->output, row->output_name) != 0) {
		/* Check 1, at once: not the file that was asked for.  Both names
		 * said; the worker's cleaned and cut short before it is shown. */
		g_autofree gchar *given = result->output != NULL ? axis_library_text_clean(result->output, 80) : NULL;
		g_autofree gchar *said = g_strdup_printf("The worker reported a file of another name (%s%s%s) than the one it "
			"was given (\"%s\"); nothing it named is touched.", given != NULL ? "\"" : "", given != NULL ? given : "none",
			given != NULL ? "\"" : "", row->output_name != NULL ? row->output_name : "none");

		if (!axis_conversion_record_check(conn, row->id, 1, "failed", said, error) ||
		    !axis_conversion_set_state(conn, row->id, "rejected", 1, "Rejected by check 1, \"The converted file is the "
		        "one asked for\": the worker reported another file; the original is as it was.", error))
			return FALSE;
		g_warning("conversions: worker %s reported another output name for conversion %" G_GINT64_FORMAT
			"; rejected", worker, row->id);
		return TRUE;
	}
	id = axis_conversion_int_text(row->id);
	size = axis_conversion_int_text(result->size);
	params[0] = id;
	params[1] = size;
	if (!axis_db_conn_exec(conn, "UPDATE conversion SET state = 'verifying', output_reported = $2::bigint,"
	        " updated_at = now() WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	return axis_conversion_queue_finish(conn, row->id, 0, NULL, error);
}

/* ── The checks on a worker ─────────────────────────────────────── */

gboolean
axis_conversions_describe_verify(
	AxisConversions     *self,
	AxisDbConn          *conn,
	const AxisJobClaim  *claim,
	const gchar         *worker,
	AxisVerifyWork     **work_out,
	gchar              **why_not,
	gboolean            *permanent,
	GError             **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autoptr(AxisVerifyWork) work = NULL;
	g_autoptr(AxisDbResult) uuid = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *volume = NULL;
	g_autofree gchar *id = NULL;
	const gchar *params[1];

	*work_out = NULL;
	*why_not = NULL;
	*permanent = TRUE;
	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!axis_conversion_row_by_verify_job(conn, claim->id, &row, error))
		goto rollback;
	if (row == NULL || g_strcmp0(row->state, "verifying") != 0 || row->evidence != NULL ||
	    g_strcmp0(row->checked_where, "worker") != 0) {
		*why_not = g_strdup("the conversion is not waiting for its checks to be measured");
		return axis_db_conn_commit(conn, error);
	}
	if (!row->handed || row->check_plan == NULL || row->check_source_sample == NULL || row->output_name == NULL) {
		*why_not = g_strdup("the conversion's checks were not handed out");
		return axis_db_conn_commit(conn, error);
	}
	volume = axis_conversion_int_text(row->volume_id);
	params[0] = volume;
	if (!axis_db_conn_exec(conn, "SELECT uuid::text FROM volume WHERE id = $1::bigint", 1, params, &uuid, error))
		goto rollback;
	if (axis_db_result_get_n_rows(uuid) != 1) {
		*why_not = g_strdup("the disk is no longer known to Axis");
		return axis_db_conn_commit(conn, error);
	}
	work = g_new0(AxisVerifyWork, 1);
	work->volume = g_strdup(axis_db_result_get_text(uuid, 0, 0));
	work->root = g_strdup(row->root_path);
	work->path = g_strdup(row->rel_path);
	work->size = row->src_size;
	work->mtime_sec = row->src_mtime_sec;
	work->mtime_nsec = row->src_mtime_nsec;
	work->inode = row->src_inode;
	work->source_sample = g_strdup(row->check_source_sample);
	work->output = g_strdup(row->output_name);
	work->output_size = row->handed_size;
	work->plan = axis_verify_plan_parse(row->check_plan, strlen(row->check_plan), &problem);
	if (work->plan == NULL || !axis_verify_work_check(work, &problem)) {
		*why_not = g_strdup_printf("the checks cannot be described: %s", problem != NULL ? problem : "?");
		return axis_db_conn_commit(conn, error);
	}
	/* Being measured: the page shows the job's worker and progress. */
	id = axis_conversion_int_text(row->id);
	params[0] = id;
	if (!axis_db_conn_exec(conn, "UPDATE conversion SET checks_waiting = NULL, updated_at = now() WHERE id = $1::bigint",
	        1, params, NULL, error))
		goto rollback;
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("conversions: worker %s measures the checks of conversion %" G_GINT64_FORMAT " (job %" G_GINT64_FORMAT
		")", worker, row->id, claim->id);
	*permanent = FALSE;
	*work_out = g_steal_pointer(&work);
	return TRUE;

rollback:
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

gboolean
axis_conversions_verified(
	AxisConversions         *self,
	AxisDbConn              *conn,
	gint64                   job,
	const AxisVerifyResult  *result,
	const gchar             *worker,
	GError                 **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autoptr(AxisVerifyPlan) plan = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *clean_worker = NULL;
	const gchar *params[3];

	if (!axis_conversion_row_by_verify_job(conn, job, &row, error))
		return FALSE;
	if (row == NULL || g_strcmp0(row->state, "verifying") != 0 || row->evidence != NULL ||
	    g_strcmp0(row->checked_where, "worker") != 0 || row->check_plan == NULL) {
		g_message("conversions: worker %s reported checks of job %" G_GINT64_FORMAT " that no conversion waits for; "
			"they are not used", worker, job);
		return TRUE;
	}
	plan = axis_verify_plan_parse(row->check_plan, strlen(row->check_plan), &problem);
	if (plan == NULL) {
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_FAILED,
			"the plan of the checks kept with conversion %" G_GINT64_FORMAT " cannot be read: %s", row->id, problem);
		return FALSE;
	}
	/* Of the file it was given, and fitting the plan it was given: anything
	 * else is refused, and nothing of it is kept. */
	if (result == NULL || result->evidence == NULL || g_strcmp0(result->output, row->output_name) != 0 ||
	    result->output_size != row->handed_size) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
			"the measurements are of another file than the one the job described");
		return FALSE;
	}
	if (!axis_verify_evidence_fits(result->evidence, plan, row->handed_size, row->src_size, &problem)) {
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
			"the measurements do not fit the plan of the checks: %s", problem);
		return FALSE;
	}
	json = axis_verify_result_to_json(result);
	if (json == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
			"the measurements are not ones a worker may report");
		return FALSE;
	}
	id = axis_conversion_int_text(row->id);
	clean_worker = axis_worker_name_is_valid(worker) ? g_strdup(worker) : NULL;
	params[0] = id;
	params[1] = json;
	params[2] = clean_worker;
	if (!axis_db_conn_exec(conn, "UPDATE conversion SET evidence = $2, evidence_at = now(), verifier = $3,"
	        " checks_waiting = NULL, updated_at = now() WHERE id = $1::bigint", 3, params, NULL, error))
		return FALSE;
	g_message("conversions: worker %s measured the checks of conversion %" G_GINT64_FORMAT "; they are judged next",
		worker, row->id);
	return axis_conversion_queue_finish(conn, row->id, 0, NULL, error);
}
