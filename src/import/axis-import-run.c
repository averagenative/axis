/*
 * axis-import-run.c - the import job: from a completed download to a file
 * in the library, step by step, recorded so that a run that dies is carried
 * on by the next
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The steps, and what is on disk and in import_file after each:
 *
 *   gate      storage writable, the title's root known, both volumes there;
 *             nothing written yet (else the import waits, saying why)
 *   decide    the walk, the probe, every file's decision (import_file rows
 *             replaced); a blocked import stops here, having written nothing
 *   folder    the title's folder exists (made now, or already)
 *   temp      import_file: dest_dir, dest_name, temp_name, temp_inode and
 *             step 'temp' are recorded BEFORE the temporary name is made;
 *             then it is linked or copied (and verified)
 *   publish   the final name, never replacing one (an upgrade to the same
 *             name: the old file is linked into the recycle folder first,
 *             then the names are swapped); step 'published' after
 *   link      one transaction: the file in the library index, the title's
 *             link to it, the import's mode and quality
 *   sidecars  subtitles and extras, the same way (a failure there is noted,
 *             never undoes the film)
 *   retire    an upgrade: the old files and their subtitles renamed into the
 *             recycle folder of their volume (import_recycle row 'moving'
 *             before, 'kept' after, in the transaction that drops the old
 *             file from the index)
 *   finish    one transaction: imported, the download imported, the recycled
 *             files' retention started, the title's history
 *   after     a Usenet download removed from its client (retried by the
 *             cleanup when it fails)
 *
 * A run that finds steps recorded looks at the disk first (recover()): a
 * temporary name whose final name already leads to its inode was published;
 * a temporary name left behind is removed (when another name of its file
 * exists, or it is a copy whose source is still there); a recycle row left
 * 'moving' is settled by where its inode is.  Then it carries on.
 *
 * A failure before publishing removes the temporary name and the folder
 * made for it (when empty): the library is as it was.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "import/axis-import-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "compat/axis-compat-notify.h"
#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-write.h"

/* Retries of a failed placement are this far apart. */
#define PLACE_RETRY_S   60
/* A failed probe is tried again after this many seconds times the attempt. */
#define PROBE_RETRY_S   300

typedef struct
{
	gint     parent;	/* a dup, owned */
	gchar   *name;
	guint64  ino;
} MadeDir;

typedef struct
{
	AxisImport            *self;
	AxisJobContext        *context;
	AxisImportReport      *report;
	gint64                 import_id;
	AxisImportSettings     settings;
	gboolean               crashed;

	/* The import's row. */
	gchar                 *module;
	gchar                 *state;
	gchar                 *kind;
	gint64                 download_id;
	gint64                 owner_id;
	gint64                 source_volume_id;
	gchar                 *source_path;
	gchar                 *release_name;
	AxisDownloadProtocol   protocol;
	gboolean               protocol_known;
	gint                   chosen;
	gchar                **overrides;
	gchar                 *override_by;
	gchar                 *requested_by;
	gint                   attempts;
	gboolean               was_blocked;
	/* Assigned by hand (add-force-grab-and-assign, slice B): what a person
	 * said the files are (AxisImportAssigned; NULL: nothing was assigned),
	 * who said it, and whether the import was opened to wait for that. */
	GPtrArray             *assigned;
	gchar                 *assigned_by;
	gboolean               assign_first;

	/* What it is for and where it goes. */
	const AxisImportOwner *owner;
	AxisImportTarget      *target;
	AxisRootInfo          *root;
	AxisVolumeGuard       *dest;
	AxisVolumeGuard       *source;	/* == dest on one volume */
	gboolean               same_volume;

	/* What was found and decided. */
	GPtrArray             *found;
	AxisImportVerdict      verdict;
	AxisRelease           *grab;
	gchar                 *mode;	/* "hardlink", "copy" */
	gchar                 *mode_text;

	/* Placing. */
	gint                   root_fd;
	gint                   film_fd;	/* the title's folder */
	gchar                 *folder;	/* its name (bytes, UTF-8) */
	gchar                 *dest_dir;	/* its stored form, relative to the root */
	gchar                 *stem;	/* the film's file without its extension */
	GArray                *made;	/* MadeDir */
	gint64                 film_row;	/* import_file.id of the film */
	gchar                 *film_name;	/* its final name */
	guint64                film_inode;
	gboolean               film_upgrade;	/* recorded by an earlier run */
	gint64                 film_library_id;
	GString               *notes;	/* what went wrong but did not stop it */
	guint                  recycled;

	/* File by file (an owner with match()). */
	AxisImportUnitsVerdict units;
	GHashTable            *kept;	/* paths an earlier run took */

	/* A move (import.take_files): a torrent's client has let go of it, as
	 * recorded in client_removal ("done" or "gone"; NULL: not asked). */
	gchar                 *released;
	/* An earlier run asked the client to let go of the torrent (recorded
	 * before it asked: import.release_asked_at). */
	gboolean               asked_before;
	/* Which client and which torrent Axis asked (import.release_client_id,
	 * release_ref): asked about again by its hash when the download's own
	 * record can no longer be used. */
	gint64                 release_client_id;
	gchar                 *release_ref;
	/* A file of the import may be in one place only, and that place is not
	 * where the import can take it on from (a temporary name that may be
	 * the file's only one, a folder replaced, a published file no longer
	 * at its name): it is noted, its row keeps it recorded, and the import
	 * stops for a person, with @stranded_code (NULL: "moved_file_kept"). */
	gboolean               stranded;
	const gchar           *stranded_code;
	/* A strand that time will not clear (every one but a folder or a name
	 * that cannot be read now, which the import looks at again a few times
	 * before a person is asked). */
	gboolean               strand_lasting;
	/* A folder this run made could not be flushed to the disk in its
	 * parent: a move across volumes keeps the download's files. */
	gboolean               folders_unsynced;
	/* The torrent's client has it in another category or label than Axis's
	 * (a person moved it there): it is left at its client, and its files are
	 * hardlinked or copied, never moved (client_removal "kept"). */
	gboolean               not_ours;
	/* No import: a duplicate a person removes (axis_import_retire_duplicate()).
	 * What it was a file of and who removes it, for its recycle row. */
	const gchar           *dup_label;
	const gchar           *dup_user;
} Run;

/* The block of a run stranded (Run.stranded). */
#define STRANDED_CODE(run) ((run)->stranded_code != NULL ? (run)->stranded_code : "moved_file_kept")

/* Whether @mode moves the download's file (it is not left where it was). */
static gboolean
mode_moves(const gchar *mode)
{
	return g_strcmp0(mode, "move") == 0 || g_strcmp0(mode, "move_copy") == 0;
}

/* Whether @mode writes a copy (the download's file being read, not
 * renamed or linked). */
static gboolean
mode_copies(const gchar *mode)
{
	return g_strcmp0(mode, "copy") == 0 || g_strcmp0(mode, "move_copy") == 0;
}

static void
made_dir_clear(gpointer data)
{
	MadeDir *dir = data;

	if (dir->parent >= 0)
		close(dir->parent);
	g_free(dir->name);
}

static void
run_clear(Run *run)
{
	g_free(run->module);
	g_free(run->state);
	g_free(run->kind);
	g_free(run->source_path);
	g_free(run->release_name);
	g_strfreev(run->overrides);
	g_free(run->override_by);
	g_free(run->requested_by);
	g_clear_pointer(&run->assigned, g_ptr_array_unref);
	g_free(run->assigned_by);
	axis_import_target_free(run->target);
	axis_root_info_free(run->root);
	if (run->source != NULL && run->source != run->dest)
		axis_volume_guard_release(run->source);
	axis_volume_guard_release(run->dest);
	g_clear_pointer(&run->found, g_ptr_array_unref);
	axis_import_verdict_clear(&run->verdict);
	axis_release_free(run->grab);
	g_free(run->mode);
	g_free(run->mode_text);
	if (run->root_fd >= 0)
		close(run->root_fd);
	if (run->film_fd >= 0)
		close(run->film_fd);
	g_free(run->folder);
	g_free(run->dest_dir);
	g_free(run->stem);
	if (run->made != NULL)
		g_array_unref(run->made);
	g_free(run->film_name);
	if (run->notes != NULL)
		g_string_free(run->notes, TRUE);
	axis_import_units_verdict_clear(&run->units);
	g_clear_pointer(&run->kept, g_hash_table_unref);
	g_free(run->released);
	g_free(run->release_ref);
	axis_import_settings_clear(&run->settings);
}

static void
note(
	Run         *run,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
note(
	Run         *run,
	const gchar *format,
	...
){
	va_list ap;

	if (run->notes->len > 0)
		g_string_append_c(run->notes, ' ');
	va_start(ap, format);
	g_string_append_vprintf(run->notes, format, ap);
	va_end(ap);
}

/* The run is stranded (Run.stranded), for @code (NULL: the default block);
 * @transient: by an error that time may clear (a folder that cannot be read
 * now), so that the import may look again a few times before a person is
 * asked. */
static void
strand(
	Run         *run,
	const gchar *code,
	gboolean     transient
){
	run->stranded = TRUE;
	if (run->stranded_code == NULL && code != NULL)
		run->stranded_code = code;
	if (!transient)
		run->strand_lasting = TRUE;
}

/* Errors of a folder or a name that may well be gone in a minute: an I/O
 * error, no file descriptor left, access refused (a person fixing the
 * permissions, a mount coming back). */
static gboolean
errno_transient(gint err)
{
	return err == EIO || err == EMFILE || err == ENFILE || err == EACCES || err == EBUSY || err == ENOMEM;
}

static gboolean
error_transient(const GError *error)
{
	return error != NULL && error->domain == G_IO_ERROR && (error->code == G_IO_ERROR_PERMISSION_DENIED ||
		error->code == G_IO_ERROR_TOO_MANY_OPEN_FILES || error->code == G_IO_ERROR_BUSY ||
		error->code == G_IO_ERROR_FAILED);
}

/* The hook at @step; %TRUE to carry on.  A crash stops everything. */
static gboolean
step(
	Run          *run,
	const gchar  *name,
	gint64        file_id,
	GError      **error
){
	switch (axis_import_hook(run->self, name, run->import_id, file_id)) {
	case AXIS_IMPORT_HOOK_FAIL:
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "A failure was injected at the step \"%s\".", name);
		return FALSE;
	case AXIS_IMPORT_HOOK_CRASH:
		run->crashed = TRUE;
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The run was stopped at the step \"%s\", as a daemon "
			"that died there would be.", name);
		return FALSE;
	default:
		return TRUE;
	}
}

static gboolean
cancelled(
	Run     *run,
	GError **error
){
	if (run->context != NULL && axis_job_context_is_cancelled(run->context)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "stopped");
		return TRUE;
	}
	return FALSE;
}

/* Whether placing a file failed because the job was told to stop (the
 * daemon stopping, the job cancelled): not a failure of the placement, so
 * nothing is counted against the import; the run ends there, and the job
 * is given back. */
static gboolean
stopped_by_job(
	Run          *run,
	const GError *failure
){
	return g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_CANCELLED) && run->context != NULL &&
		axis_job_context_is_cancelled(run->context);
}

/* ── Reading the row ────────────────────────────────────────────── */

static gchar **
codes_from_array(const gchar *text)
{
	g_autofree gchar *inner = NULL;
	gsize n = text != NULL ? strlen(text) : 0;

	if (n < 2 || text[0] != '{' || text[n - 1] != '}')
		return g_new0(gchar *, 1);
	inner = g_strndup(text + 1, n - 2);
	if (inner[0] == '\0')
		return g_new0(gchar *, 1);
	return g_strsplit(inner, ",", 32);
}

static gboolean
load(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	const gchar *params[1];
	const gchar *text;

	params[0] = id;
	if (!axis_db_pool_exec(run->self->pool, "SELECT module, state, kind, download_id, owner_id, source_volume_id,"
	        " source_path, release_name, protocol, chosen_position, override_codes::text, override_by, requested_by,"
	        " attempts, CASE WHEN client_removal IN ('done', 'gone') THEN client_removal END,"
	        " release_asked_at IS NOT NULL, release_client_id, release_ref, assign_first FROM import"
	        " WHERE id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		return FALSE;
	}
	run->module = g_strdup(axis_db_result_get_text(row, 0, 0));
	run->state = g_strdup(axis_db_result_get_text(row, 0, 1));
	run->kind = g_strdup(axis_db_result_get_text(row, 0, 2));
	axis_db_result_get_int64(row, 0, 3, &run->download_id);
	axis_db_result_get_int64(row, 0, 4, &run->owner_id);
	axis_db_result_get_int64(row, 0, 5, &run->source_volume_id);
	run->source_path = g_strdup(axis_db_result_get_text(row, 0, 6));
	run->release_name = g_strdup(axis_db_result_get_text(row, 0, 7));
	text = axis_db_result_get_text(row, 0, 8);
	run->protocol_known = text != NULL && axis_download_protocol_from_string(text, &run->protocol);
	run->chosen = -1;
	if (!axis_db_result_is_null(row, 0, 9)) {
		gint64 chosen = -1;

		axis_db_result_get_int64(row, 0, 9, &chosen);
		run->chosen = (gint)chosen;
	}
	run->overrides = codes_from_array(axis_db_result_get_text(row, 0, 10));
	run->override_by = g_strdup(axis_db_result_get_text(row, 0, 11));
	run->requested_by = g_strdup(axis_db_result_get_text(row, 0, 12));
	{
		gint64 attempts = 0;

		axis_db_result_get_int64(row, 0, 13, &attempts);
		run->attempts = (gint)attempts;
	}
	/* A torrent's client let go of it before its files were moved (an
	 * earlier run of this import asked). */
	run->released = g_strdup(axis_db_result_get_text(row, 0, 14));
	/* An earlier run asked the client to let go of it. */
	run->asked_before = g_strcmp0(axis_db_result_get_text(row, 0, 15), "t") == 0;
	axis_db_result_get_int64(row, 0, 16, &run->release_client_id);
	run->release_ref = g_strdup(axis_db_result_get_text(row, 0, 17));
	axis_db_result_get_boolean(row, 0, 18, &run->assign_first);
	/* What a person assigned by hand, if they did. */
	return axis_import_assigned_load(run->self, NULL, run->import_id, &run->assigned, &run->assigned_by, error);
}

/* Nothing is assigned by hand yet, and the import was opened to be assigned
 * by hand, or its owner holds it although the title is known (a series whose
 * numbering changed since the grab: its files' names would be read against
 * the wrong one).  Its files are read and recorded, so that a person can
 * say what each is, and nothing is imported by the names. */
static gboolean
waits_for_assignment(const Run *run)
{
	return (run->assign_first || (run->target != NULL && run->target->hold != NULL)) && run->assigned == NULL;
}

static const gchar assign_first_text[] = "This import was opened to be assigned by hand: Axis read the files and "
	"imported nothing. Select \"Assign by hand\" to say what each file is.";

static gboolean block_one(Run *run, const gchar *code, gboolean overridable, const gchar *text, GError **error);

/* The block of an import that waits_for_assignment(). */
static gboolean
block_waiting(
	Run     *run,
	GError **error
){
	if (run->target != NULL && run->target->hold != NULL)
		return block_one(run, "no_title", FALSE, run->target->hold, error);
	return block_one(run, "assign", FALSE, assign_first_text, error);
}

/* Whether the download's other files are left where they are once the
 * import ends: always when its files were assigned by hand.  The person
 * chose some files and left the rest alone, so nothing of the rest is
 * deleted by the import: the download is not removed from its client with
 * its files, and its folder is not deleted.  (They clear it themselves, on
 * the Leftovers page.) */
static gboolean
leaves_files(const Run *run)
{
	return run->assigned != NULL;
}

/* Nothing of what was read is taken (the import waits for the assignment):
 * its rows say so. */
static void
nothing_accepted(Run *run)
{
	guint i;

	for (i = 0; i < run->found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(run->found, i);
		guint k;

		if (!f->accepted)
			continue;
		f->accepted = FALSE;
		if (g_strcmp0(f->role, "film") != 0)
			f->role = NULL;
		/* (What it would have been imported as is not said: it was not.) */
		for (k = f->reasons != NULL ? f->reasons->len : 0; k > 0; k--) {
			if (g_strcmp0(((const AxisImportReason *)g_ptr_array_index(f->reasons, k - 1))->code, "episode") == 0)
				g_ptr_array_remove_index(f->reasons, k - 1);
		}
		axis_import_found_reason(f, "waits", FALSE, "Read, and not imported: the import waits for a person to say "
			"what its files are.");
	}
}

/* ── States ─────────────────────────────────────────────────────── */

/* A note in the title's history, in its own transaction. */
static void
history(
	Run         *run,
	const gchar *kind,
	const gchar *text
){
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;

	if (run->owner == NULL || run->target == NULL || run->owner->info->note == NULL)
		return;
	conn = axis_db_pool_acquire(run->self->pool, &error);
	if (conn == NULL ||
	    !run->owner->info->note(run->owner->data, conn, run->target, kind, text, NULL, &error))
		g_warning("import %" G_GINT64_FORMAT ": its history could not be written: %s", run->import_id,
			error != NULL ? error->message : "no connection");
	if (conn != NULL)
		axis_db_pool_release(run->self->pool, conn);
}

/* The import waits, for @code, and is looked at again after @after_s. */
static gboolean
wait_for(
	Run          *run,
	const gchar  *code,
	const gchar  *text,
	gint64        after_s,
	gboolean      count_attempt,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *after = axis_import_int_text(MAX(after_s, 0));
	g_autofree gchar *cut = axis_import_text_cut(text, 2000);
	const gchar *params[5];

	params[0] = id;
	params[1] = code;
	params[2] = cut;
	params[3] = after;
	params[4] = count_attempt ? "true" : "false";
	if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET state = 'waiting', reason = $2, reason_text = $3,"
	        " blockers = '[]', retry_after = now() + $4::integer * interval '1 second',"
	        " attempts = attempts + CASE WHEN $5::boolean THEN 1 ELSE 0 END, updated_at = now()"
	        " WHERE id = $1::bigint RETURNING (SELECT i.reason FROM import i WHERE i.id = $1::bigint)", 5, params,
	        &result, error))
		return FALSE;
	/* Its history learns of the first wait of each kind. */
	if (axis_db_result_get_n_rows(result) > 0 && g_strcmp0(axis_db_result_get_text(result, 0, 0), code) != 0)
		history(run, "import_waiting", text);
	if (run->report != NULL) {
		run->report->state = "waiting";
		run->report->reason = g_intern_string(code);
	}
	return TRUE;
}

/* The import is blocked, with every reason. */
static gboolean
block_with(
	Run              *run,
	const gchar      *code,
	const gchar      *summary,
	const GPtrArray  *blockers,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *cut = axis_import_text_cut(summary, 2000);
	g_autofree gchar *json = axis_import_reasons_to_json(blockers);
	const gchar *params[4];

	params[0] = id;
	params[1] = code;
	params[2] = cut;
	params[3] = json;
	if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET state = 'blocked', reason = $2, reason_text = $3,"
	        " blockers = $4::jsonb, retry_after = NULL, updated_at = now() WHERE id = $1::bigint"
	        " RETURNING (SELECT i.state FROM import i WHERE i.id = $1::bigint)", 4, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0 && g_strcmp0(axis_db_result_get_text(result, 0, 0), "blocked") != 0) {
		g_autofree gchar *text = g_strdup_printf("Its import is blocked: %s", cut);
		gboolean anyway = FALSE;
		guint i;

		history(run, "import_blocked", text);
		/* No file in the download it can take, and nothing a person can
		 * import anyway: an owner's grab by hand lets go of what it claimed.
		 * What a person can put right (a volume, a root, a name) keeps it. */
		for (i = 0; blockers != NULL && i < blockers->len; i++)
			anyway = anyway || ((const AxisImportReason *)g_ptr_array_index(blockers, i))->overridable;
		if (!anyway && (g_strcmp0(code, "nothing_wanted") == 0 || g_strcmp0(code, "no_video") == 0 ||
		        g_strcmp0(code, "archive") == 0) &&
		    run->owner != NULL && run->target != NULL && run->owner->info->stuck != NULL) {
			g_autoptr(GError) local = NULL;
			AxisDbConn *conn = axis_db_pool_acquire(run->self->pool, &local);

			if (conn == NULL || !run->owner->info->stuck(run->owner->data, conn, run->target, code, cut, &local))
				g_warning("import %" G_GINT64_FORMAT ": its grab could not be let go: %s", run->import_id,
					local != NULL ? local->message : "no connection");
			if (conn != NULL)
				axis_db_pool_release(run->self->pool, conn);
		}
	}
	if (run->report != NULL) {
		run->report->state = "blocked";
		run->report->reason = g_intern_string(code);
	}
	return TRUE;
}

/* Blocked for one reason. */
static gboolean
block_one(
	Run          *run,
	const gchar  *code,
	gboolean      overridable,
	const gchar  *text,
	GError      **error
){
	g_autoptr(GPtrArray) blockers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_reason_free);

	g_ptr_array_add(blockers, axis_import_reason_new(code, text, overridable));
	return block_with(run, code, text, blockers, error);
}

/* ── Recording what was found ───────────────────────────────────── */

static const gchar *
kind_name(const AxisImportFound *f)
{
	switch (f->type) {
	case AXIS_IMPORT_FOUND_LINK:
		return "link";
	case AXIS_IMPORT_FOUND_SPECIAL:
	case AXIS_IMPORT_FOUND_FOLDER:
		return "special";
	default:
		break;
	}
	switch (f->kind) {
	case AXIS_IMPORT_FILE_VIDEO:
		return "video";
	case AXIS_IMPORT_FILE_SUBTITLE:
		return "subtitle";
	case AXIS_IMPORT_FILE_ARCHIVE:
		return "archive";
	default:
		return "other";
	}
}

/* The decisions replace what an earlier run recorded (recover() made sure
 * no temporary name of those is left). */
static gboolean
store_found(
	Run     *run,
	GError **error
){
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	AxisDbConn *conn = axis_db_pool_acquire(run->self->pool, error);
	const gchar *params[13];
	gboolean ok;
	guint i;

	if (conn == NULL)
		return FALSE;
	params[0] = id;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "DELETE FROM import_file WHERE import_id = $1::bigint", 1, params, NULL, error);
	for (i = 0; ok && i < run->found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(run->found, i);
		g_autoptr(AxisDbResult) inserted = NULL;
		g_autofree gchar *position = axis_import_int_text(f->position);
		g_autofree gchar *size = f->type == AXIS_IMPORT_FOUND_FILE ? axis_import_int_text((gint64)f->size) : NULL;
		g_autofree gchar *inode = f->type == AXIS_IMPORT_FOUND_FILE ? axis_import_int_text((gint64)f->inode) : NULL;
		g_autofree gchar *reasons = axis_import_reasons_to_json(f->reasons);
		g_autofree gchar *duration = f->probe_ok ? axis_import_int_text(f->duration_ms) : NULL;
		g_autofree gchar *width = f->probe_ok ? axis_import_int_text(f->width) : NULL;
		g_autofree gchar *height = f->probe_ok ? axis_import_int_text(f->height) : NULL;
		g_autofree gchar *codec = axis_import_text_cut(f->video_codec, 60);
		g_autofree gchar *probe = axis_import_text_cut(f->probe_text, 1000);

		params[1] = position;
		params[2] = f->path;
		params[3] = kind_name(f);
		params[4] = size;
		params[5] = inode;
		params[6] = f->role;
		params[7] = f->accepted ? "true" : "false";
		params[8] = reasons;
		params[9] = duration;
		params[10] = width;
		params[11] = height;
		params[12] = codec;
		ok = axis_db_conn_exec(conn, "INSERT INTO import_file (import_id, position, path, kind, size, inode, role,"
			" accepted, reasons, duration_ms, width, height, video_codec) VALUES ($1::bigint, $2::integer, $3, $4,"
			" $5::bigint, $6::bigint, $7, $8::boolean, $9::jsonb, $10::bigint, $11::integer, $12::integer, $13)"
			" RETURNING id", 13, params, &inserted, error);
		if (ok && probe != NULL) {
			const gchar *more[2];

			more[0] = axis_db_result_get_text(inserted, 0, 0);
			more[1] = probe;
			ok = axis_db_conn_exec(conn, "UPDATE import_file SET probe_text = $2 WHERE id = $1::bigint", 2, more, NULL,
				error);
		}
		/* The row's id names the file's temporary name. */
		if (ok)
			axis_db_result_get_int64(inserted, 0, 0, &f->row_id);
		if (ok && f == run->verdict.film)
			run->film_row = f->row_id;
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	return ok;
}

/* ── Opening folders of a volume ────────────────────────────────── */

/* The bytes of a stored path's parts. */
static gchar **
stored_parts(const gchar *stored)
{
	g_autoptr(GString) bytes = NULL;

	if (stored == NULL || stored[0] == '\0')
		return g_new0(gchar *, 1);
	if (!axis_library_name_decode(stored, &bytes) || memchr(bytes->str, '\0', bytes->len) != NULL)
		return NULL;
	return g_strsplit(bytes->str, "/", AXIS_LIBRARY_DEPTH_MAX + 1);
}

/* The folder @parts below @top (not consumed), no link, the same device.
 * Fails with %G_IO_ERROR_NOT_FOUND only when a folder on the way is not
 * there (ENOENT); with %AXIS_STORAGE_ERROR_NOT_CONTAINED for a link or
 * another filesystem or something that is not a folder on the way; and with
 * the system's error otherwise (it cannot be opened now: EACCES, EIO,
 * EMFILE), which tells nothing of what is there. */
static gint
open_below(
	gint                top,
	gchar             **parts,
	guint               n,
	guint64             dev,
	GError            **error
){
	gint current = openat(top, ".", O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
	guint i;

	if (current < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot open a folder: %s", g_strerror(errno));
		return -1;
	}
	for (i = 0; i < n && parts[i] != NULL; i++) {
		struct stat st;
		gint next = openat(current, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		close(current);
		if (next < 0 || fstat(next, &st) != 0 || (guint64)st.st_dev != dev) {
			gint err = next < 0 ? errno : EXDEV;

			if (next >= 0)
				close(next);
			if (err == ENOENT)
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "A folder on the way is not there (%s).",
					g_strerror(err));
			else if (err == ELOOP || err == ENOTDIR || err == EXDEV)
				g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
					"A folder on the way is not there as it was (%s).", g_strerror(err));
			else
				g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "A folder on the way cannot be opened now "
					"(%s).", g_strerror(err));
			return -1;
		}
		current = next;
	}
	return current;
}

/* ── Recovering from a run that died ────────────────────────────── */

typedef struct
{
	gint64   id;
	gchar   *role;
	gchar   *path;
	gchar   *dest_dir;
	gchar   *dest_name;
	gchar   *temp_name;
	gint64   temp_inode;
	gchar   *step;
	gchar   *mode;
	gint64   dest_inode;
	gint64   replaced_inode;
	gint64   source_inode;
	gint64   library_file_id;
	gint64   dest_dir_inode;	/* the folder's, when the temporary name was made; 0: not recorded */
	gboolean verified;		/* a copy, read back and found the same (its checksum recorded) */
} Placed;

static void
placed_free(gpointer data)
{
	Placed *p = data;

	g_free(p->role);
	g_free(p->path);
	g_free(p->dest_dir);
	g_free(p->dest_name);
	g_free(p->temp_name);
	g_free(p->step);
	g_free(p->mode);
	g_free(p);
}

static GPtrArray *
load_placed(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	GPtrArray *placed;
	const gchar *params[1];
	guint i;

	params[0] = id;
	if (!axis_db_pool_exec(run->self->pool, "SELECT id, role, path, dest_dir, dest_name, temp_name, temp_inode, step,"
	        " mode, dest_inode, replaced_inode, inode, library_file_id, dest_dir_inode, checksum IS NOT NULL"
	        " FROM import_file WHERE import_id = $1::bigint AND role IS NOT NULL ORDER BY role <> 'film', position", 1,
	        params, &rows, error))
		return NULL;
	placed = g_ptr_array_new_with_free_func(placed_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		Placed *p = g_new0(Placed, 1);

		axis_db_result_get_int64(rows, i, 0, &p->id);
		p->role = g_strdup(axis_db_result_get_text(rows, i, 1));
		p->path = g_strdup(axis_db_result_get_text(rows, i, 2));
		p->dest_dir = g_strdup(axis_db_result_get_text(rows, i, 3));
		p->dest_name = g_strdup(axis_db_result_get_text(rows, i, 4));
		p->temp_name = g_strdup(axis_db_result_get_text(rows, i, 5));
		axis_db_result_get_int64(rows, i, 6, &p->temp_inode);
		p->step = g_strdup(axis_db_result_get_text(rows, i, 7));
		p->mode = g_strdup(axis_db_result_get_text(rows, i, 8));
		axis_db_result_get_int64(rows, i, 9, &p->dest_inode);
		axis_db_result_get_int64(rows, i, 10, &p->replaced_inode);
		axis_db_result_get_int64(rows, i, 11, &p->source_inode);
		axis_db_result_get_int64(rows, i, 12, &p->library_file_id);
		axis_db_result_get_int64(rows, i, 13, &p->dest_dir_inode);
		p->verified = g_strcmp0(axis_db_result_get_text(rows, i, 14), "t") == 0;
		g_ptr_array_add(placed, p);
	}
	return placed;
}

static gboolean
set_step(
	Run          *run,
	gint64        row,
	const gchar  *step_name,
	gint64        dest_inode,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(row);
	g_autofree gchar *inode = dest_inode > 0 ? axis_import_int_text(dest_inode) : NULL;
	const gchar *params[3];

	params[0] = id;
	params[1] = step_name;
	params[2] = inode;
	if (step_name == NULL)
		return axis_db_pool_exec(run->self->pool, "UPDATE import_file SET step = NULL, temp_inode = NULL,"
			" replaced_inode = NULL WHERE id = $1::bigint", 1, params, NULL, error);
	/* A moved file is out of the download once it has its final name (a
	 * copy that moves is, once the download's file is deleted). */
	return axis_db_pool_exec(run->self->pool, "UPDATE import_file SET step = $2, dest_inode = coalesce($3::bigint,"
		" dest_inode), source_state = CASE WHEN $2 = 'temp' THEN source_state WHEN mode = 'move' THEN 'moved'"
		" WHEN mode = 'move_copy' THEN coalesce(source_state, 'pending') ELSE source_state END"
		" WHERE id = $1::bigint", 3, params, NULL, error);
}

/* The folder of the download a row's file was in, reached through the
 * source's guard without a link, and the file's name there (raw bytes); -1
 * when that folder cannot be reached now. */
static gint
open_source_of(
	Run          *run,
	const gchar  *path,
	gchar       **name
){
	g_autoptr(GString) bytes = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	g_auto(GStrv) parts = NULL;
	const gchar *slash;
	gint top;
	gint dir;
	guint n;

	*name = NULL;
	if (path == NULL || run->source == NULL || run->source_path == NULL || run->source_path[0] == '\0' ||
	    !axis_library_name_decode(path, &bytes) || memchr(bytes->str, '\0', bytes->len) != NULL)
		return -1;
	slash = strrchr(run->source_path, '/');
	parent = slash != NULL ? g_strndup(run->source_path, (gsize)(slash - run->source_path)) : g_strdup("");
	last = g_strdup(slash != NULL ? slash + 1 : run->source_path);
	if (bytes->len == 0) {
		/* The download was that one file. */
		dir = axis_volume_guard_open_dir(run->source, parent, NULL);
		if (dir >= 0)
			*name = g_steal_pointer(&last);
		return dir;
	}
	parts = g_strsplit(bytes->str, "/", AXIS_LIBRARY_DEPTH_MAX + 1);
	n = g_strv_length(parts);
	if (n == 0 || parts[n - 1][0] == '\0')
		return -1;
	top = axis_volume_guard_open_dir(run->source, run->source_path, NULL);
	if (top < 0)
		return -1;
	dir = open_below(top, parts, n - 1, axis_volume_guard_get_dev(run->source), NULL);
	close(top);
	if (dir >= 0)
		*name = g_strdup(parts[n - 1]);
	return dir;
}

static gboolean
storage_gone(const GError *error)
{
	return error != NULL && error->domain == AXIS_STORAGE_ERROR && (error->code == AXIS_STORAGE_ERROR_UNAVAILABLE ||
		error->code == AXIS_STORAGE_ERROR_READ_ONLY);
}

/*
 * A file an earlier run placed under its temporary name (@p's temp_inode) in
 * @dir and did not publish, given its final name now, never replacing one;
 * for an upgrade to the same name (the old file has its name in the recycle
 * folder already) the two names change places.  *@published; else *@why
 * says why not (a volume gone or read-only is an error).
 */
static gboolean
publish_forward(
	Run       *run,
	Placed    *p,
	gint       dir,
	gboolean  *published,
	gchar    **why,
	GError   **error
){
	g_autoptr(GString) final = NULL;
	guint64 ino = (guint64)p->temp_inode;
	GError *local = NULL;
	gboolean forward;

	*published = FALSE;
	*why = NULL;
	if (!axis_library_name_decode(p->dest_name, &final) || memchr(final->str, '\0', final->len) != NULL) {
		*why = g_strdup("its recorded name in the library is not valid");
		return TRUE;
	}
	if (p->replaced_inode > 0) {
		forward = axis_volume_write_swap(run->dest, dir, p->temp_name, ino, final->str, (guint64)p->replaced_inode,
			&local);
		if (forward) {
			GError *undo = NULL;

			if (!set_step(run, p->id, "swapped", (gint64)ino, error))
				return FALSE;
			if (!axis_volume_write_unlink_temp(run->dest, dir, p->temp_name, (guint64)p->replaced_inode, FALSE, NULL,
			        &undo)) {
				note(run, "The old file's temporary name stays beside the new one: %s", undo->message);
				g_error_free(undo);
			}
		}
	} else {
		forward = axis_volume_write_publish(run->dest, dir, p->temp_name, ino, final->str, &local);
	}
	if (forward) {
		*published = TRUE;
		return TRUE;
	}
	if (storage_gone(local)) {
		g_propagate_error(error, local);
		return FALSE;
	}
	*why = g_strdup(local->message);
	g_error_free(local);
	return TRUE;
}

/*
 * A file an earlier run moved out of the download (renamed to its temporary
 * name in the library) and did not publish: the run died between the two
 * renames, or could not undo the first.  It goes back to its download name
 * when it can (the import then goes on as if the move had not been made);
 * else forward to its final name, never replacing one (the move is
 * completed; *@published); else it is kept under its temporary name, noted,
 * and the import stops for a person (run->stranded).  Nothing is lost
 * either way: the file has one of its names at every moment.
 */
static gboolean
settle_moved(
	Run       *run,
	Placed    *p,
	gint       dir,
	gboolean  *published,
	GError   **error
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *not_named = NULL;
	guint64 ino = (guint64)p->temp_inode;
	GError *local = NULL;
	gboolean forward = FALSE;
	gint src;

	*published = FALSE;
	src = open_source_of(run, p->path, &name);
	if (src >= 0) {
		gboolean back = axis_volume_write_move_back(run->dest, dir, p->temp_name, ino, src, name, &local);

		close(src);
		if (back)
			return TRUE;
		if (storage_gone(local)) {
			g_propagate_error(error, local);
			return FALSE;
		}
		why = g_strdup_printf("it could not be put back in the download (%s)", local->message);
		g_clear_error(&local);
	} else {
		why = g_strdup("its folder in the download is not there any more");
	}
	if (!publish_forward(run, p, dir, &forward, &not_named, error))
		return FALSE;
	if (forward) {
		*published = TRUE;
		return TRUE;
	}
	note(run, "A file this import moved out of the download is kept in the library's folder under the temporary "
		"name %s: %s, nor given its name in the library (%s). Nothing of it was lost; put it where it belongs by hand, "
		"or make room for its name and retry the import.", p->temp_name, why, not_named);
	strand(run, NULL, FALSE);
	return TRUE;
}

/* Whether the source of a copy is still there (so its temporary copy can
 * go without losing anything). */
static gboolean
source_still_there(
	Run         *run,
	const gchar *path,
	gint64       inode
){
	AxisImportFound probe;
	g_autoptr(GString) bytes = NULL;
	gint dir = -1;
	const gchar *slash;
	gboolean there;

	if (path == NULL || run->source == NULL || !axis_library_name_decode(path, &bytes))
		return FALSE;
	memset(&probe, 0, sizeof(probe));
	slash = strrchr(bytes->str, '/');
	probe.path = (gchar *)path;
	probe.dir = slash != NULL ? g_strndup(bytes->str, (gsize)(slash - bytes->str)) : g_strdup("");
	probe.name = g_strdup(slash != NULL ? slash + 1 : bytes->str);
	if (path[0] == '\0') {
		g_autofree gchar *last = g_path_get_basename(run->source_path);

		g_free(probe.name);
		probe.name = g_strdup(last);
	}
	probe.inode = (guint64)inode;
	there = axis_import_open_found(run->source, run->source_path, &probe, &dir, NULL, NULL);
	if (dir >= 0)
		close(dir);
	g_free(probe.dir);
	g_free(probe.name);
	return there;
}

/* The folder a placed row's file is in, opened from the root. */
static gint
open_dest_dir(
	Run          *run,
	const gchar  *dest_dir,
	GError      **error
){
	g_auto(GStrv) parts = stored_parts(dest_dir);

	if (parts == NULL) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "A recorded folder is not valid.");
		return -1;
	}
	return open_below(run->root_fd, parts, g_strv_length(parts), axis_volume_guard_get_dev(run->dest), error);
}

/* Settles the recycle rows an earlier run left 'moving'. */
static gboolean recover_recycled(Run *run, GError **error);

/* Whether the download's own name of row @p's file leads, looked at now, to
 * the very file the row moved (its inode recorded before the move): the move
 * was never made, or the file is back. */
static gboolean
source_holds(
	Run          *run,
	const Placed *p
){
	g_autofree gchar *name = NULL;
	struct stat st;
	gboolean held;
	gint dir;

	if (p->temp_inode <= 0)
		return FALSE;
	dir = open_source_of(run, p->path, &name);
	if (dir < 0)
		return FALSE;
	held = fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
		(gint64)st.st_ino == p->temp_inode;
	close(dir);
	return held;
}

/* A file an earlier run moved out of the download that cannot be found now
 * where it must be (its folder in the library cannot be opened, or its
 * temporary name is not there and the download's name does not lead to it):
 * its row keeps it recorded, and the import stops for a person, saying where
 * it should be (run->stranded); @transient: by an error time may clear, so
 * that it looks again a few times first. */
static void
lost_track(
	Run          *run,
	const Placed *p,
	const gchar  *why,
	gboolean      transient
){
	g_autofree gchar *shown = axis_library_name_display(p->dest_dir);

	note(run, "A file this import %s should be in %s's folder %s, under the temporary name %s, but %s. Axis keeps the "
		"record of it and looks for it again when the import is retried: put the folder back if it was moved, or put "
		"the file where it belongs by hand.", g_strcmp0(p->mode, "move") == 0 ? "moved out of the download" :
		mode_copies(p->mode) ? "copied, whose download's file is not in the download any more," :
		"linked, whose download's name no longer leads to it,", run->root->name, shown,
		p->temp_name != NULL ? p->temp_name : "(none)", why);
	strand(run, NULL, transient);
}

/* Whether the download still has row @p's file under its own name, looked
 * at now: for a move or a link, its name leads to the very file; for a copy,
 * to the file copied.  When it does not, what the row placed under its
 * temporary name may be the file's only copy. */
static gboolean
download_holds(
	Run          *run,
	const Placed *p
){
	if (mode_copies(p->mode))
		return source_still_there(run, p->path, p->source_inode);
	return source_holds(run, p);
}

/* The row forgets its step: nothing of it is where it was placed (or what
 * is there is not its only copy), in the row and in what recover() looks at
 * afterwards. */
static gboolean
forget_step(
	Run     *run,
	Placed  *p,
	GError **error
){
	if (!set_step(run, p->id, NULL, 0, error))
		return FALSE;
	g_clear_pointer(&p->step, g_free);
	return TRUE;
}

/* A file under its temporary name that may be its only copy, and could not
 * be given its name: kept, its row keeps it recorded, and the import stops
 * for a person (run->stranded); a retry gives it its name. */
static void
kept_temp(
	Run          *run,
	const Placed *p,
	const gchar  *why
){
	g_autofree gchar *shown = axis_library_name_display(p->dest_dir);

	note(run, "A file of this import is kept in %s's folder %s under the temporary name %s, and may be its only copy: "
		"%s. Nothing of it was lost; make room for its name (or put it where it belongs by hand) and retry the import.",
		run->root->name, shown, p->temp_name, why);
	strand(run, NULL, FALSE);
}

/* The folder at row @p's recorded place is not the folder the file was put
 * in (another inode: renamed or moved away, and another made with its name):
 * the file, under @name, is in that other folder; the import stops for a
 * person. */
static void
folder_replaced(
	Run          *run,
	const Placed *p,
	const gchar  *name
){
	g_autofree gchar *shown = axis_library_name_display(p->dest_dir);

	note(run, "%s's folder %s is not the folder an earlier run of this import put a file in: that folder was renamed or "
		"moved away, and another was made with its name since. The file is in that other folder, under the name %s. "
		"Axis keeps the record of it: put that folder back, or put the file where it belongs by hand, then retry the "
		"import.", run->root->name, shown, name);
	strand(run, "folder_replaced", FALSE);
}

/* The film's file, given its name by an earlier run, is not under that name
 * now (@found says what is): the import stops for a person, naming it. */
static void
film_not_at_name(
	Run          *run,
	const Placed *p,
	const gchar  *found
){
	g_autofree gchar *shown = axis_library_name_display(p->dest_dir);
	g_autofree gchar *name = axis_library_name_display(p->dest_name);

	note(run, "An earlier run of this import gave the movie's file its name %s in %s's folder %s, and %s. Axis keeps the "
		"record of it: put the file back under that name and retry the import, or dismiss the import (what is in the "
		"library stays).", name, run->root->name, shown, found);
	strand(run, "film_file_changed", FALSE);
}

/* The temporary name of a file the import moved, removed only beside
 * another name of the very file, looked at now: its final name when it was
 * given it (@published), else the download's own name.  A link count is not
 * taken for one (axis_volume_write_unlink_temp_beside()). */
static gboolean
unlink_moved_temp(
	Run           *run,
	const Placed  *p,
	gint           dir,
	gboolean       published,
	guint64        expected,
	GError       **error
){
	g_autoptr(GString) final = NULL;
	g_autofree gchar *name = NULL;
	gboolean ok;
	gint src;

	if (published && axis_library_name_decode(p->dest_name, &final) && memchr(final->str, '\0', final->len) == NULL)
		return axis_volume_write_unlink_temp_beside(run->dest, dir, p->temp_name, expected, dir, final->str, NULL,
			error);
	src = open_source_of(run, p->path, &name);
	if (src < 0) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED, "It may be the one name of "
			"a file moved out of the download: no other name of it is known to lead to it.");
		return FALSE;
	}
	ok = axis_volume_write_unlink_temp_beside(run->dest, dir, p->temp_name, expected, src, name, NULL, error);
	close(src);
	return ok;
}

/* The download's file of a move across volumes is kept, with @why: it is
 * never deleted (axis_import_moved_sources() only deletes a pending one). */
static gboolean
keep_source(
	Run          *run,
	gint64        row,
	const gchar  *why,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(row);
	const gchar *params[2];

	params[0] = id;
	params[1] = why;
	return axis_db_pool_exec(run->self->pool, "UPDATE import_file SET source_state = 'kept', note = left($2, 1000)"
		" WHERE id = $1::bigint AND mode = 'move_copy' AND coalesce(source_state, 'pending') = 'pending'", 2, params,
		NULL, error);
}

#define NOT_FLUSHED "The download's file was kept: the library's folder could not be flushed to the disk after the " \
	"verified copy got its name there, so the copy is not known to be on the disk, and the download's file may be " \
	"the one copy that is."
#define FOLDER_NOT_FLUSHED "The download's file was kept: a folder that holds the verified copy in the library could " \
	"not be flushed to the disk, so the copy is not known to be on the disk, and the download's file may be the one " \
	"copy that is."

/* The folders that hold @dest_dir's folder in the library, from the root
 * down, flushed to the disk, so that its own entry is on it (the folder
 * itself is flushed when a file gets its name in it).  %FALSE when one could
 * not be, or cannot be opened. */
static gboolean
ancestors_flushed(
	Run         *run,
	const gchar *dest_dir
){
	g_auto(GStrv) parts = stored_parts(dest_dir);
	gboolean ok = TRUE;
	gint current;
	guint n;
	guint i;

	if (parts == NULL)
		return FALSE;
	n = g_strv_length(parts);
	current = axis_volume_guard_open_dir(run->dest, run->root->rel_path, NULL);
	if (current < 0)
		return FALSE;
	for (i = 0; ok && i < n; i++) {
		gint next;

		ok = axis_volume_write_flush_dir(run->dest, current);
		if (!ok || i + 1 == n)
			break;
		next = open_below(current, &parts[i], 1, axis_volume_guard_get_dev(run->dest), NULL);
		close(current);
		current = next;
		if (current < 0)
			ok = FALSE;
	}
	if (current >= 0)
		close(current);
	return ok;
}

/*
 * What an earlier run left: every placed row is looked at on disk.  Sets
 * *@film_published when the film's file has its final name.
 */
static gboolean
recover(
	Run       *run,
	gboolean  *film_published,
	GError   **error
){
	g_autoptr(GPtrArray) placed = load_placed(run, error);
	guint i;

	*film_published = FALSE;
	if (placed == NULL)
		return FALSE;
	for (i = 0; i < placed->len; i++) {
		Placed *p = g_ptr_array_index(placed, i);
		struct stat st;
		gboolean published = FALSE;
		gboolean moved = g_strcmp0(p->mode, "move") == 0;
		gboolean copies = mode_copies(p->mode);
		gboolean at_temp = g_strcmp0(p->step, "temp") == 0;
		gboolean film = g_strcmp0(p->role, "film") == 0;
		gboolean flushed = TRUE;
		gboolean other_folder = FALSE;
		g_autofree gchar *found = NULL;
		gint64 final_ino = 0;
		gint dir;
		GError *local = NULL;

		if (p->step == NULL || p->dest_name == NULL || p->dest_dir == NULL)
			continue;
		dir = open_dest_dir(run, p->dest_dir, &local);
		if (dir < 0) {
			if (local->domain == AXIS_STORAGE_ERROR && local->code != AXIS_STORAGE_ERROR_NOT_CONTAINED) {
				g_propagate_error(error, local);
				return FALSE;
			}
			/* A moved file already linked (in the library's index) is left as
			 * it is: its folder is the library scan's to look at now, and the
			 * run goes on from what is recorded. */
			if (moved && p->library_file_id > 0) {
				g_error_free(local);
				continue;
			}
			/* A file moved out of the download and not linked yet may be in
			 * that folder under its temporary name: it is forgotten only when
			 * the download's name leads to it again (the move was never
			 * made).  So with a copy or a link made there whose download's
			 * name no longer leads to the file: it may be its only copy. */
			if ((moved && g_strcmp0(p->step, "swapped") != 0 && !source_holds(run, p)) ||
			    (!moved && at_temp && p->temp_inode > 0 && !download_holds(run, p))) {
				g_autofree gchar *why = g_strdup_printf("that folder cannot be opened now (%s)", local->message);
				gboolean transient = error_transient(local);

				g_error_free(local);
				lost_track(run, p, why, transient);
				continue;
			}
			g_error_free(local);
			/* The film given its name keeps its record: the run stops for a
			 * person, saying its folder cannot be opened (film_folder_gone). */
			if (film && !at_temp)
				continue;
			/* No folder: no temporary name in it that is a file's only one
			 * (a link's or a copy's file is still the download's). */
			if (!forget_step(run, p, error))
				return FALSE;
			continue;
		}
		{
			g_autoptr(GString) name = NULL;

			if (!axis_library_name_decode(p->dest_name, &name) || memchr(name->str, '\0', name->len) != NULL) {
				found = g_strdup("that name is not valid any more");
			} else if (fstatat(dir, name->str, &st, AT_SYMLINK_NOFOLLOW) != 0) {
				found = errno == ENOENT ? g_strdup("nothing has that name now") :
					g_strdup_printf("that name cannot be looked at now (%s)", g_strerror(errno));
			} else if (!S_ISREG(st.st_mode)) {
				found = g_strdup("that name is not a file's now");
			} else {
				gint64 expected = at_temp ? p->temp_inode : p->dest_inode;

				published = expected > 0 && (gint64)st.st_ino == expected;
				if (published)
					final_ino = (gint64)st.st_ino;
				else
					found = g_strdup("that name is another file's now");
			}
		}
		/* The very folder the temporary name was made in (its inode,
		 * recorded then): one renamed or moved away, and another made with
		 * its name, is another folder, and what the earlier run put there is
		 * in the first. */
		if (!published && p->dest_dir_inode > 0 && fstat(dir, &st) == 0 && (gint64)st.st_ino != p->dest_dir_inode)
			other_folder = TRUE;
		if (other_folder) {
			close(dir);
			/* Nothing made there yet (a copy not begun), or the download has
			 * the file still: nothing of it is only there. */
			if (at_temp && ((copies && p->temp_inode <= 0) || download_holds(run, p))) {
				if (!moved && !(copies && p->temp_inode <= 0)) {
					g_autofree gchar *shown = axis_library_name_display(p->dest_dir);

					note(run, "A temporary name an earlier run made (%s) may be left in the folder that had the name %s "
						"then (it was renamed or moved away since): it is not a file's only name.", p->temp_name, shown);
				}
				if (!forget_step(run, p, error))
					return FALSE;
				continue;
			}
			{
				g_autofree gchar *shown = axis_library_name_display(p->dest_name);

				folder_replaced(run, p, at_temp && p->temp_name != NULL ? p->temp_name : shown);
			}
			continue;
		}
		/* A file moved out of the download and not published: back, or on
		 * to its name, never lost (settle_moved()). */
		if (!published && moved && g_strcmp0(p->step, "swapped") != 0 && p->temp_name != NULL && p->temp_inode > 0 &&
		    fstatat(dir, p->temp_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
		    (gint64)st.st_ino == p->temp_inode) {
			gboolean forward = FALSE;
			gboolean was_stranded = run->stranded;

			if (!settle_moved(run, p, dir, &forward, error)) {
				close(dir);
				return FALSE;
			}
			close(dir);
			/* Kept under its temporary name: its row keeps the step. */
			if (run->stranded && !was_stranded)
				continue;
			if (forward) {
				published = TRUE;
				final_ino = p->temp_inode;
			}
			if (published) {
				if (!set_step(run, p->id, "published", final_ino, error))
					return FALSE;
				g_free(p->step);
				p->step = g_strdup("published");
				if (film)
					*film_published = TRUE;
			} else if (!forget_step(run, p, error)) {
				return FALSE;
			}
			continue;
		}
		/* The film given its name by an earlier run is not under it now
		 * (another file has it, or nothing): the import stops for a person,
		 * saying what is there; its row keeps the record. */
		if (!published && film && !at_temp) {
			close(dir);
			film_not_at_name(run, p, found != NULL ? found : "it is not there now");
			continue;
		}
		/* Not given its name, its temporary name not there (or another
		 * file's), and the download's name not leading to it: the run died
		 * before the rename and something else deleted the download's file, or
		 * something deleted the file after it: it is in neither place, and the
		 * step goes (the walk then tells what is there, truthfully).  Only a
		 * temporary name that cannot be looked at now (not "not there") keeps
		 * the record and stops the import; so with a copy or a link made. */
		if (!published && at_temp && (moved || p->temp_inode > 0) && p->temp_name != NULL && !download_holds(run, p) &&
		    fstatat(dir, p->temp_name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno != ENOENT) {
			gint err = errno;
			g_autofree gchar *why = g_strdup_printf("its temporary name cannot be looked at now (%s)", g_strerror(err));

			close(dir);
			lost_track(run, p, why, errno_transient(err));
			continue;
		}
		/* A copy or a link under its temporary name, not given its name,
		 * whose download's name no longer leads to the file (something
		 * deleted it since): the temporary name may be the file's only one.
		 * A copy read back and found the same (its checksum recorded), or a
		 * link (the very file), is given its name now; else it stays, its
		 * row keeps the step, and the import stops for a person. */
		if (!published && !moved && at_temp && p->temp_name != NULL && p->temp_inode > 0 &&
		    fstatat(dir, p->temp_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
		    (gint64)st.st_ino == p->temp_inode && !download_holds(run, p)) {
			g_autofree gchar *why = NULL;
			gboolean forward = FALSE;

			if (copies && !p->verified) {
				close(dir);
				kept_temp(run, p, "the copy was not yet read back and found the same as the download's file, and that "
					"file is not in the download any more, so Axis cannot tell that the copy is whole");
				continue;
			}
			if (!publish_forward(run, p, dir, &forward, &why, error)) {
				close(dir);
				return FALSE;
			}
			if (!forward) {
				g_autofree gchar *text = g_strdup_printf("its download's file is not in the download any more, and it "
					"could not be given its name in the library (%s)", why);

				close(dir);
				kept_temp(run, p, text);
				continue;
			}
			published = TRUE;
			final_ino = p->temp_inode;
		}
		/* A temporary name left: removed when nothing is lost by it. */
		if (p->temp_name != NULL && fstatat(dir, p->temp_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
			gboolean last_ok = FALSE;
			gboolean replaced = g_strcmp0(p->step, "swapped") == 0 || (published && p->replaced_inode > 0);
			guint64 expected = (guint64)MAX(p->temp_inode, 0);
			gboolean ok;

			if (replaced)
				expected = (guint64)p->replaced_inode;
			else if (mode_copies(p->mode))
				last_ok = source_still_there(run, p->path, p->source_inode);
			/* An upgrade's old file has its name in the recycle folder; a moved
			 * file's temporary name goes only beside a name of its own. */
			if (moved && !replaced)
				ok = unlink_moved_temp(run, p, dir, published, expected, &local);
			else
				ok = axis_volume_write_unlink_temp(run->dest, dir, p->temp_name, expected, last_ok, NULL, &local);
			if (!ok) {
				if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED)) {
					note(run, "A temporary file an earlier run left (%s) was kept: %s", p->temp_name, local->message);
					g_error_free(local);
				} else {
					close(dir);
					g_propagate_error(error, local);
					return FALSE;
				}
			}
		}
		/* A copy given its name by an earlier run: its folder flushed now,
		 * before the download's file may be deleted. */
		if (published && g_strcmp0(p->mode, "move_copy") == 0)
			flushed = axis_volume_write_flush_dir(run->dest, dir);
		close(dir);
		if (published) {
			const gchar *kept = !flushed ? NOT_FLUSHED : g_strcmp0(p->mode, "move_copy") == 0 &&
				!ancestors_flushed(run, p->dest_dir) ? FOLDER_NOT_FLUSHED : NULL;

			if (!set_step(run, p->id, "published", final_ino, error))
				return FALSE;
			g_free(p->step);
			p->step = g_strdup("published");
			if (kept != NULL && !keep_source(run, p->id, kept, error))
				return FALSE;
			if (film)
				*film_published = TRUE;
		} else if (!forget_step(run, p, error)) {
			/* (Its temporary name is gone, or is not its file's: what may be a
			 * file's only name kept its step above.) */
			return FALSE;
		}
	}
	/* The film published, then the earlier run died: carry on from there
	 * (the steps as they are now, a step forgotten above being no more). */
	if (!*film_published) {
		for (i = 0; i < placed->len; i++) {
			Placed *p = g_ptr_array_index(placed, i);

			if (g_strcmp0(p->role, "film") == 0 && (g_strcmp0(p->step, "published") == 0 ||
			    g_strcmp0(p->step, "swapped") == 0))
				*film_published = TRUE;
		}
	}
	return recover_recycled(run, error);
}

/* ── What a run taken up again sees ─────────────────────────────── */

/* The kind of folder a file is in, from the folders of its place in the
 * download, as the walk tells it (a plain folder inside an extras folder is
 * still extras). */
static AxisImportFolder
folder_of_dir(
	const gchar  *dir,
	const gchar **extra
){
	g_auto(GStrv) parts = dir != NULL && dir[0] != '\0' ? g_strsplit(dir, "/", AXIS_LIBRARY_DEPTH_MAX + 1) :
		g_new0(gchar *, 1);
	AxisImportFolder folder = AXIS_IMPORT_FOLDER_PLAIN;
	guint i;

	*extra = NULL;
	for (i = 0; parts[i] != NULL; i++) {
		const gchar *this_extra = NULL;
		AxisImportFolder kind = axis_import_folder_kind(parts[i], strlen(parts[i]), &this_extra);

		if (kind == AXIS_IMPORT_FOLDER_PLAIN && folder != AXIS_IMPORT_FOLDER_PLAIN)
			continue;
		folder = kind;
		*extra = this_extra;
	}
	return folder;
}

static gint
compare_position(
	gconstpointer a,
	gconstpointer b
){
	const AxisImportFound *x = *(const AxisImportFound *const *)a;
	const AxisImportFound *y = *(const AxisImportFound *const *)b;

	return x->position < y->position ? -1 : x->position > y->position;
}

/*
 * A run taken up again, after files of the import were moved out of the
 * download: the walk no longer finds what was moved, so those files are
 * added from their rows, as the walk found them then (place, size, inode,
 * what the probe said), and every file found takes the position and row of
 * its first walk.  The decisions made again on that see the download as it
 * was when the import decided; with @recorded, every file of a row takes the
 * role and decision recorded then, which stand.
 */
static gboolean
align_found(
	Run       *run,
	gboolean   recorded,
	GError   **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autoptr(GHashTable) by_path = g_hash_table_new(g_str_hash, g_str_equal);
	const gchar *params[1];
	gint next = 0;
	guint i;

	if (run->found == NULL)
		run->found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
	params[0] = id;
	if (!axis_db_pool_exec(run->self->pool, "SELECT id, position, path, size, inode, duration_ms, width, height,"
	        " video_codec, role, accepted, mode = 'move' AND source_state = 'moved' FROM import_file"
	        " WHERE import_id = $1::bigint ORDER BY position", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 position = 0;

		axis_db_result_get_int64(rows, i, 1, &position);
		next = MAX(next, (gint)position + 1);
		if (axis_db_result_get_text(rows, i, 2) != NULL)
			g_hash_table_insert(by_path, (gpointer)axis_db_result_get_text(rows, i, 2), GUINT_TO_POINTER(i + 1));
	}
	/* What the walk found: its first walk's position and row, or a place of
	 * its own after them (it appeared since, and is not taken).  Something
	 * new at the place of a file that was moved is not this import's at
	 * all, and is left out. */
	for (i = run->found->len; i > 0; i--) {
		AxisImportFound *f = g_ptr_array_index(run->found, i - 1);
		guint row = GPOINTER_TO_UINT(g_hash_table_lookup(by_path, f->path));
		gint64 value = 0;

		if (row == 0) {
			f->position = next++;
			f->row_id = 0;
			continue;
		}
		axis_db_result_get_int64(rows, row - 1, 4, &value);
		if (g_strcmp0(axis_db_result_get_text(rows, row - 1, 11), "t") == 0 && (guint64)value != f->inode) {
			g_ptr_array_remove_index(run->found, i - 1);
			continue;
		}
		axis_db_result_get_int64(rows, row - 1, 1, &value);
		f->position = (gint)value;
		axis_db_result_get_int64(rows, row - 1, 0, &f->row_id);
		g_hash_table_replace(by_path, (gpointer)axis_db_result_get_text(rows, row - 1, 2), GUINT_TO_POINTER(0));
	}
	/* What was moved, and is not in the download any more. */
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *path = axis_db_result_get_text(rows, i, 2);
		g_autoptr(GString) bytes = NULL;
		AxisImportFound *f;
		const gchar *slash;
		gint64 value = 0;

		if (path == NULL || GPOINTER_TO_UINT(g_hash_table_lookup(by_path, path)) == 0 ||
		    g_strcmp0(axis_db_result_get_text(rows, i, 11), "t") != 0 || !axis_library_name_decode(path, &bytes) ||
		    memchr(bytes->str, '\0', bytes->len) != NULL)
			continue;
		f = g_new0(AxisImportFound, 1);
		axis_db_result_get_int64(rows, i, 0, &f->row_id);
		axis_db_result_get_int64(rows, i, 1, &value);
		f->position = (gint)value;
		f->type = AXIS_IMPORT_FOUND_FILE;
		f->path = g_strdup(path);
		slash = strrchr(bytes->str, '/');
		f->dir = slash != NULL ? g_strndup(bytes->str, (gsize)(slash - bytes->str)) : g_strdup("");
		if (bytes->len > 0) {
			f->name = g_strdup(slash != NULL ? slash + 1 : bytes->str);
		} else {
			/* The download was that one file. */
			const gchar *last = strrchr(run->source_path, '/');

			f->name = g_strdup(last != NULL ? last + 1 : run->source_path);
		}
		f->kind = axis_import_file_kind(f->name, strlen(f->name));
		f->folder = folder_of_dir(f->dir, &f->extra_folder);
		axis_db_result_get_int64(rows, i, 3, &value);
		f->size = (guint64)MAX(value, 0);
		value = 0;
		axis_db_result_get_int64(rows, i, 4, &value);
		f->inode = (guint64)MAX(value, 0);
		if (!axis_db_result_is_null(rows, i, 5)) {
			gint64 number = 0;

			f->probed = TRUE;
			f->probe_ok = TRUE;
			axis_db_result_get_int64(rows, i, 5, &f->duration_ms);
			axis_db_result_get_int64(rows, i, 6, &number);
			f->width = (gint)number;
			number = 0;
			axis_db_result_get_int64(rows, i, 7, &number);
			f->height = (gint)number;
			f->video_streams = 1;
			f->video_codec = g_strdup(axis_db_result_get_text(rows, i, 8));
		}
		g_ptr_array_add(run->found, f);
	}
	g_ptr_array_sort(run->found, compare_position);
	if (!recorded)
		return TRUE;
	/* The decisions recorded stand. */
	for (i = 0; i < run->found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(run->found, i);
		guint r;

		if (f->row_id <= 0)
			continue;
		for (r = 0; r < axis_db_result_get_n_rows(rows); r++) {
			gint64 row = 0;

			axis_db_result_get_int64(rows, r, 0, &row);
			if (row != f->row_id)
				continue;
			f->role = g_intern_string(axis_db_result_get_text(rows, r, 9));
			f->accepted = g_strcmp0(axis_db_result_get_text(rows, r, 10), "t") == 0;
			break;
		}
	}
	return TRUE;
}

/* ── Placing one file ───────────────────────────────────────────── */

typedef struct
{
	Run     *run;
	gint64   row;
	guint64  size;
	gboolean recorded;
	gboolean verifying;
} CopyStep;

static gboolean
copy_step(
	gpointer   data,
	guint64    ino,
	guint64    written,
	GError   **error
){
	CopyStep *cs = data;
	Run *run = cs->run;

	if (cs->recorded && !cs->verifying && written == cs->size) {
		/* Written and flushed, about to be read back. */
		cs->verifying = TRUE;
		return step(run, "verifying", cs->row, error);
	}
	if (!cs->recorded) {
		g_autofree gchar *id = axis_import_int_text(cs->row);
		g_autofree gchar *inode = axis_import_int_text((gint64)ino);
		const gchar *params[2];

		params[0] = id;
		params[1] = inode;
		if (!axis_db_pool_exec(run->self->pool, "UPDATE import_file SET temp_inode = $2::bigint WHERE id = $1::bigint",
		        2, params, NULL, error))
			return FALSE;
		cs->recorded = TRUE;
		return step(run, "copying", cs->row, error);
	}
	if (cancelled(run, error))
		return FALSE;
	(void)written;
	return TRUE;
}

/*
 * The film's (or a sidecar's) file placed under its temporary name in @dir
 * and given @final_name.  @replaced_inode: for an upgrade to the same name,
 * the old file there (already linked into the recycle folder).  *@inode: the
 * placed file's.  On failure the temporary name is gone again (unless the
 * run crashed).
 */
static gboolean
place_one(
	Run              *run,
	AxisImportFound  *f,
	gint64            row,
	gint              dir,
	const gchar      *dest_dir,
	const gchar      *final_name,
	guint64           replaced_inode,
	guint64          *inode,
	gchar           **checksum,
	GError          **error
){
	g_autofree gchar *temp = g_strdup_printf(AXIS_VOLUME_TEMP_PREFIX "%" G_GINT64_FORMAT, row);
	g_autofree gchar *id = axis_import_int_text(row);
	g_autofree gchar *stored = NULL;
	g_autofree gchar *src_inode = axis_import_int_text((gint64)f->inode);
	g_autofree gchar *replaced = replaced_inode > 0 ? axis_import_int_text((gint64)replaced_inode) : NULL;
	gboolean copy = mode_copies(run->mode);
	gboolean move = g_strcmp0(run->mode, "move") == 0;
	gint src_dir = -1;
	gint src_fd = -1;
	guint64 temp_ino = 0;
	gboolean made = FALSE;
	gboolean swapped = FALSE;
	gboolean synced = TRUE;
	AxisVolumeSourceId copied;
	GError *local = NULL;

	*inode = 0;
	*checksum = NULL;
	memset(&copied, 0, sizeof(copied));
	{
		GString *s = g_string_new(NULL);

		axis_library_name_encode(s, final_name, strlen(final_name));
		stored = g_string_free(s, FALSE);
	}
	/* Recorded before anything is made: a run that dies after it knows
	 * what to look for, and in which very folder (its inode). */
	{
		struct stat folder_st;
		g_autofree gchar *folder_ino = fstat(dir, &folder_st) == 0 ? axis_import_int_text((gint64)folder_st.st_ino) :
			NULL;
		const gchar *more[8];

		more[0] = id;
		more[1] = dest_dir;
		more[2] = stored;
		more[3] = temp;
		more[4] = copy ? NULL : src_inode;
		more[5] = run->mode;
		more[6] = replaced;
		more[7] = folder_ino;
		if (!axis_db_pool_exec(run->self->pool, "UPDATE import_file SET dest_dir = $2, dest_name = $3, temp_name = $4,"
		        " temp_inode = $5::bigint, mode = $6, replaced_inode = $7::bigint, dest_dir_inode = $8::bigint,"
		        " checksum = NULL, step = 'temp' WHERE id = $1::bigint", 8, more, NULL, error))
			return FALSE;
	}
	if (!step(run, "temp", row, &local))
		goto fail;
	if (!axis_import_open_found(run->source, run->source_path, f, &src_dir, copy ? &src_fd : NULL, &local))
		goto fail;
	if (copy) {
		CopyStep cs = { run, row, f->size, FALSE, FALSE };

		if (!axis_volume_write_copy_temp(run->dest, src_fd, src_dir, f->name, f->size, dir, temp, copy_step, &cs,
		        &temp_ino, &copied, checksum, &local))
			goto fail;
		/* The copy is under its temporary name from here on: a failure now
		 * removes it again (made), as any other. */
		made = TRUE;
		/* What the download's file was when it was copied, for its deletion
		 * after a move across volumes: the very file, untouched since.  The
		 * times are those the copy took on the descriptor before its first
		 * byte was read (and found again after its last), never a look now:
		 * a change made while the copy was flushed and read back would be
		 * recorded as what was copied, and the changed file deleted.  The
		 * inode and the size recorded by the walk are the copy's too
		 * (axis_import_open_found() and the copy checked them). */
		/* And that it was verified (its checksum): a run taken up again may
		 * give a verified copy its name when it is the only copy left. */
		{
			g_autofree gchar *mtime = copied.ino == f->inode ? g_strdup_printf("%" G_GINT64_FORMAT, copied.mtime_ns) :
				NULL;
			g_autofree gchar *ctime = mtime != NULL ? g_strdup_printf("%" G_GINT64_FORMAT, copied.ctime_ns) : NULL;
			const gchar *more[4];

			more[0] = id;
			more[1] = mtime;
			more[2] = ctime;
			more[3] = *checksum;
			if (!axis_db_pool_exec(run->self->pool, "UPDATE import_file SET source_mtime_ns = $2::bigint,"
			        " source_ctime_ns = $3::bigint, checksum = $4 WHERE id = $1::bigint", 4, more, NULL, &local))
				goto fail;
		}
	} else if (move) {
		/* One rename: from here on the file is in the library's folder,
		 * under its temporary name, and no longer in the download. */
		if (!axis_volume_write_move_temp(run->dest, src_dir, f->name, f->inode, dir, temp, &local))
			goto fail;
		temp_ino = f->inode;
	} else {
		if (!axis_volume_write_link_temp(run->dest, src_dir, f->name, f->inode, dir, temp, &local))
			goto fail;
		temp_ino = f->inode;
	}
	made = TRUE;
	/* (The file is under its temporary name, however it got there.) */
	if (!step(run, "copied", row, &local))
		goto fail;
	if (replaced_inode > 0) {
		/* The old file has its name in the recycle folder already: the two
		 * names change places at once, then the temporary name (the old
		 * file's second name now) goes. */
		if (!axis_volume_write_swap_synced(run->dest, dir, temp, temp_ino, final_name, replaced_inode, &synced, &local))
			goto fail;
		swapped = TRUE;
		if (!set_step(run, row, "swapped", (gint64)temp_ino, &local))
			goto fail;
		if (!axis_volume_write_unlink_temp(run->dest, dir, temp, replaced_inode, FALSE, NULL, &local)) {
			note(run, "The old file's temporary name stays beside the new one: %s", local->message);
			g_clear_error(&local);
		}
	} else if (!axis_volume_write_publish_synced(run->dest, dir, temp, temp_ino, final_name, &synced, &local)) {
		goto fail;
	}
	if (src_fd >= 0)
		close(src_fd);
	if (src_dir >= 0)
		close(src_dir);
	{
		const gchar *more[4];
		g_autofree gchar *ino_text = axis_import_int_text((gint64)temp_ino);
		const gchar *kept = NOT_FLUSHED;

		/* A copy that moves: its download's file is deleted later, once the
		 * import is recorded, only if the folder that names the copy is on
		 * the disk, and the folders that hold it (one this run made among
		 * them); else it is kept, saying why. */
		if (synced && g_strcmp0(run->mode, "move_copy") == 0 &&
		    (run->folders_unsynced || !ancestors_flushed(run, dest_dir))) {
			synced = FALSE;
			kept = FOLDER_NOT_FLUSHED;
		}
		more[0] = id;
		more[1] = ino_text;
		more[2] = synced ? "true" : "false";
		more[3] = kept;
		if (!axis_db_pool_exec(run->self->pool, "UPDATE import_file SET step = 'published', dest_inode = $2::bigint,"
		        " temp_inode = $2::bigint, source_state = CASE mode WHEN 'move' THEN 'moved' WHEN 'move_copy' THEN"
		        " CASE WHEN $3::boolean THEN 'pending' ELSE 'kept' END ELSE source_state END,"
		        " note = CASE WHEN mode = 'move_copy' AND NOT $3::boolean THEN $4 ELSE note END"
		        " WHERE id = $1::bigint", 4, more, NULL, error))
			return FALSE;
		if (*checksum != NULL) {
			more[1] = *checksum;
			if (!axis_db_pool_exec(run->self->pool, "UPDATE import_file SET checksum = $2 WHERE id = $1::bigint", 2,
			        more, NULL, error))
				return FALSE;
		}
	}
	*inode = temp_ino;
	return step(run, "published", row, error);

fail:
	if (src_fd >= 0)
		close(src_fd);
	/* A crash leaves everything for the next run; so does a failure after
	 * the swap (the new file has its name; the next run finds it). */
	if (run->crashed || swapped) {
		if (src_dir >= 0)
			close(src_dir);
		g_propagate_error(error, local);
		return FALSE;
	}
	/* Undone: the temporary name goes (a link's file keeps the download's
	 * name; a copy's source is still there), or, for a move, the file gets
	 * its download name back; the row forgets the step.  A link or a copy
	 * that failed removed its own temporary name already, and a name it
	 * found taken is not Axis's to remove.  A moved file that cannot be put
	 * back keeps its temporary name and the step, for the next run's
	 * recover() (settle_moved()): it is never left without a name. */
	if (made && move) {
		GError *undo = NULL;

		if (!axis_volume_write_move_back(run->dest, dir, temp, temp_ino, src_dir, f->name, &undo)) {
			g_autofree gchar *shown = axis_library_name_display(dest_dir);

			g_warning("import %" G_GINT64_FORMAT ": a moved file could not be put back in the download, and keeps its "
				"temporary name for the next run: %s", run->import_id, undo->message);
			/* Kept, recorded (the row keeps the step): the import does not
			 * end as if it had not been there (run->stranded). */
			note(run, "%s was moved out of the download and could be neither given its name in the library (%s) nor "
				"put back (%s): it is kept in %s's folder %s under the temporary name %s. Nothing of it was lost; make "
				"room for its name (or put it where it belongs by hand) and retry the import.", f->name,
				local != NULL ? local->message : "?", undo->message, run->root->name, shown, temp);
			strand(run, NULL, FALSE);
			g_error_free(undo);
			if (src_dir >= 0)
				close(src_dir);
			g_free(*checksum);
			*checksum = NULL;
			g_propagate_error(error, local);
			return FALSE;
		}
	} else if (move) {
		/* A move that failed between its link and the removal of the
		 * download's name (a filesystem without RENAME_NOREPLACE) left a
		 * second name: it goes, but only while the download's own name,
		 * looked at now, leads to the file (a link count would count a name
		 * outside as well). */
		GError *undo = NULL;
		struct stat st;

		if (!axis_volume_write_unlink_temp_beside(run->dest, dir, temp, f->inode, src_dir, f->name, NULL, &undo)) {
			if (fstatat(dir, temp, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
			    (guint64)st.st_ino == f->inode) {
				g_autofree gchar *shown = axis_library_name_display(dest_dir);

				/* The file has its temporary name, and its download name does
				 * not lead to it: kept, recorded. */
				note(run, "%s was moved out of the download, and the move could not be completed or undone (%s): it is "
					"kept in %s's folder %s under the temporary name %s. Nothing of it was lost; retry the import.",
					f->name, undo->message, run->root->name, shown, temp);
				strand(run, NULL, FALSE);
				g_error_free(undo);
				if (src_dir >= 0)
					close(src_dir);
				g_free(*checksum);
				*checksum = NULL;
				g_propagate_error(error, local);
				return FALSE;
			}
			g_debug("import %" G_GINT64_FORMAT ": no second name of a file that was not moved to remove: %s",
				run->import_id, undo->message);
			g_error_free(undo);
		}
	} else if (made) {
		GError *undo = NULL;
		gboolean last_ok = copy && source_still_there(run, f->path, (gint64)f->inode);
		guint64 kept_ino = copy ? temp_ino : f->inode;
		struct stat st;

		if (!axis_volume_write_unlink_temp(run->dest, dir, temp, kept_ino, last_ok, NULL, &undo)) {
			/* Kept because it may be the file's only name (a copy whose
			 * download's file something else deleted while it was copied, a
			 * link whose download's name went): the row keeps its step and the
			 * import stops for a person, naming it; a retry gives it its name. */
			if (fstatat(dir, temp, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
			    (guint64)st.st_ino == kept_ino && (copy ? last_ok : st.st_nlink >= 2)) {
				/* Not removed now (storage went read-only, say), and nothing is
				 * lost by it: the step stays, so that the next run removes it. */
				g_warning("import %" G_GINT64_FORMAT ": a temporary file could not be removed, and is left for the "
					"next run: %s", run->import_id, undo->message);
				g_error_free(undo);
				if (src_dir >= 0)
					close(src_dir);
				g_free(*checksum);
				*checksum = NULL;
				g_propagate_error(error, local);
				return FALSE;
			}
			if (fstatat(dir, temp, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
			    (guint64)st.st_ino == kept_ino) {
				g_autofree gchar *shown = axis_library_name_display(dest_dir);

				note(run, "%s could not be given its name in the library (%s), and its download's file is not there any "
					"more: %s is kept in %s's folder %s under the temporary name %s, and may be the only copy. Nothing of "
					"it was lost; make room for its name (or put it where it belongs by hand) and retry the import.",
					f->name, local != NULL ? local->message : "?", copy ? "its verified copy" : "it", run->root->name, shown,
					temp);
				strand(run, NULL, FALSE);
				g_error_free(undo);
				if (src_dir >= 0)
					close(src_dir);
				g_free(*checksum);
				*checksum = NULL;
				g_propagate_error(error, local);
				return FALSE;
			}
			/* Not there, or not the file placed: nothing of it to keep. */
			g_debug("import %" G_GINT64_FORMAT ": no temporary file of its own to remove: %s", run->import_id,
				undo->message);
			g_error_free(undo);
		}
	}
	if (src_dir >= 0)
		close(src_dir);
	/* (What may be a file's only name kept the step above.) */
	if (!set_step(run, row, NULL, 0, NULL))
		g_warning("import %" G_GINT64_FORMAT ": a step could not be cleared", run->import_id);
	g_free(*checksum);
	*checksum = NULL;
	g_propagate_error(error, local);
	return FALSE;
}

/* The folder @name in @parent, made when needed (and remembered, to be
 * removed again if the run fails and it is still empty). */
static gint
folder(
	Run          *run,
	gint          parent,
	const gchar  *name,
	GError      **error
){
	gboolean created = FALSE;
	gboolean synced = TRUE;
	guint64 ino = 0;
	gint fd = axis_volume_write_mkdir_synced(run->dest, parent, name, run->settings.dir_mode, &created, &ino, &synced,
		error);

	if (fd >= 0 && created && !synced)
		run->folders_unsynced = TRUE;
	if (fd >= 0 && created) {
		MadeDir made;

		made.parent = fcntl(parent, F_DUPFD_CLOEXEC, 0);
		made.name = g_strdup(name);
		made.ino = ino;
		g_array_append_val(run->made, made);
	}
	return fd;
}

static void
undo_folders(Run *run)
{
	guint i;

	if (run->crashed || run->made == NULL || run->dest == NULL)
		return;
	for (i = run->made->len; i > 0; i--) {
		MadeDir *made = &g_array_index(run->made, MadeDir, i - 1);

		if (made->parent >= 0)
			axis_volume_write_rmdir_created(run->dest, made->parent, made->name, made->ino, NULL);
	}
	g_array_set_size(run->made, 0);
}

/* ── The index ──────────────────────────────────────────────────── */

/* A placed file in the library index (inserted, or the row of its path
 * brought up to date: an upgrade to the same name).  Its generations fit
 * the root's scans so that a scan running now does not take it for gone,
 * and the probe takes it as soon as the root has had a complete scan. */
static gboolean
index_file(
	Run          *run,
	AxisDbConn   *conn,
	gint          dir,
	const gchar  *name,
	const gchar  *rel_stored,
	gint64       *file_id,
	GError      **error
){
	g_autoptr(AxisDbResult) gen = NULL;
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *root = axis_import_int_text(run->root->id);
	g_autofree gchar *volume = axis_import_int_text(run->root->volume_id);
	g_autofree gchar *size = NULL;
	g_autofree gchar *ms = NULL;
	g_autofree gchar *mn = NULL;
	g_autofree gchar *cs = NULL;
	g_autofree gchar *cn = NULL;
	g_autofree gchar *ino = NULL;
	g_autofree gchar *links = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *last = NULL;
	gint64 generation = 0;
	gint64 complete = 0;
	gint64 f;
	gint64 l;
	struct stat st;
	const gchar *params[16];
	gboolean racy;

	*file_id = 0;
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "A placed file is not there to be indexed.");
		return FALSE;
	}
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT coalesce(s.generation, 0), coalesce(s.complete_generation, 0)"
	        " FROM library_root r LEFT JOIN library_root_state s ON s.root_id = r.id WHERE r.id = $1::bigint", 1, params,
	        &gen, error))
		return FALSE;
	if (axis_db_result_get_n_rows(gen) > 0) {
		axis_db_result_get_int64(gen, 0, 0, &generation);
		axis_db_result_get_int64(gen, 0, 1, &complete);
	}
	f = complete >= 1 ? complete : MAX(generation, 1);
	l = MAX(generation, f);
	racy = g_get_real_time() / G_USEC_PER_SEC - (gint64)st.st_mtim.tv_sec < 2;
	size = axis_import_int_text((gint64)st.st_size);
	ms = axis_import_int_text((gint64)st.st_mtim.tv_sec);
	mn = axis_import_int_text((gint64)st.st_mtim.tv_nsec);
	cs = axis_import_int_text((gint64)st.st_ctim.tv_sec);
	cn = axis_import_int_text((gint64)st.st_ctim.tv_nsec);
	ino = axis_import_int_text((gint64)st.st_ino);
	links = axis_import_int_text((gint64)st.st_nlink);
	first = axis_import_int_text(f);
	last = axis_import_int_text(l);
	params[0] = root;
	params[1] = volume;
	params[2] = rel_stored;
	params[3] = axis_library_name_is_invalid(rel_stored) ? "true" : "false";
	params[4] = axis_file_kind_to_string(axis_file_kind_from_name(name, strlen(name)));
	params[5] = size;
	params[6] = ms;
	params[7] = mn;
	params[8] = cs;
	params[9] = cn;
	params[10] = ino;
	params[11] = links;
	params[12] = st.st_nlink > 1 ? "true" : "false";
	params[13] = racy ? "true" : "false";
	params[14] = first;
	params[15] = last;
	if (!axis_db_conn_exec(conn, "INSERT INTO library_file AS f (root_id, volume_id, rel_path, name_invalid_utf8, kind,"
	        " size, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, link_target, pinned, pinned_links,"
	        " pinned_downloads, stat_racy, first_seen_generation, last_seen_generation, changed, changed_generation)"
	        " VALUES ($1::bigint, $2::bigint, $3, $4::boolean, $5, $6::bigint, $7::bigint, $8::integer, $9::bigint,"
	        "  $10::integer, $11::bigint, $12::bigint, NULL, $13::boolean, $13::boolean, false, $14::boolean,"
	        "  $15::bigint, $16::bigint, true, $15::bigint)"
	        " ON CONFLICT (root_id, path_hash) DO UPDATE SET kind = EXCLUDED.kind, size = EXCLUDED.size,"
	        "  mtime_sec = EXCLUDED.mtime_sec, mtime_nsec = EXCLUDED.mtime_nsec, ctime_sec = EXCLUDED.ctime_sec,"
	        "  ctime_nsec = EXCLUDED.ctime_nsec, inode = EXCLUDED.inode, nlink = EXCLUDED.nlink, link_target = NULL,"
	        "  pinned = EXCLUDED.pinned OR f.pinned_downloads, pinned_links = EXCLUDED.pinned_links,"
	        "  stat_racy = EXCLUDED.stat_racy, state = 'present', suspected_at = NULL, suspected_scan_id = NULL,"
	        "  suspected_boot_id = NULL, suspected_boottime_us = NULL, missing_at = NULL, changed = true,"
	        "  changed_generation = greatest(f.first_seen_generation, EXCLUDED.first_seen_generation),"
	        "  last_seen_generation = greatest(f.last_seen_generation, EXCLUDED.last_seen_generation,"
	        "   f.first_seen_generation, EXCLUDED.first_seen_generation),"
	        "  content_changed_at = now()"
	        " RETURNING id", 16, params, &row, error))
		return FALSE;
	return axis_db_result_get_int64(row, 0, 0, file_id);
}

/* ── The film ───────────────────────────────────────────────────── */

static gchar *
stored_of(const gchar *raw)
{
	GString *s = g_string_new(NULL);

	axis_library_name_encode(s, raw, strlen(raw));
	return g_string_free(s, FALSE);
}

/* The existing file an upgrade to the same name replaces, if one does. */
static const AxisImportExisting *
same_name_existing(
	Run         *run,
	const gchar *rel_stored
){
	guint i;

	for (i = 0; run->verdict.replace != NULL && i < run->verdict.replace->len; i++) {
		const AxisImportExisting *e = g_ptr_array_index(run->verdict.replace, i);

		if (e->root_id == run->root->id && g_strcmp0(e->rel_path, rel_stored) == 0)
			return e;
	}
	return NULL;
}

static gchar **
recycle_where(
	const gchar *root_path,
	const gchar *rel_stored
){
	g_autoptr(GPtrArray) where = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) root_parts = root_path != NULL && root_path[0] != '\0' ? g_strsplit(root_path, "/", -1) :
		g_new0(gchar *, 1);
	g_auto(GStrv) rel_parts = stored_parts(rel_stored);
	guint i;

	if (rel_parts == NULL)
		return NULL;
	for (i = 0; root_parts[i] != NULL; i++)
		g_ptr_array_add(where, g_strdup(root_parts[i]));
	/* The folders of the file, not its own name. */
	for (i = 0; rel_parts[i] != NULL && rel_parts[i + 1] != NULL; i++)
		g_ptr_array_add(where, g_strdup(rel_parts[i]));
	g_ptr_array_add(where, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&where), FALSE);
}

static gchar *
recycle_stored(
	gchar       **where,
	const gchar  *name
){
	GString *s = g_string_new(AXIS_VOLUME_RECYCLE_DIR);
	guint i;

	for (i = 0; where[i] != NULL; i++) {
		g_string_append_c(s, '/');
		axis_library_name_encode(s, where[i], strlen(where[i]));
	}
	g_string_append_c(s, '/');
	axis_library_name_encode(s, name, strlen(name));
	return g_string_free(s, FALSE);
}

static gchar *
original_stored(
	const gchar *root_path,
	const gchar *rel_stored
){
	return root_path != NULL && root_path[0] != '\0' ? g_strconcat(root_path, "/", rel_stored, NULL) :
		g_strdup(rel_stored);
}

/* A recycle row, written before the file is moved. */
static gboolean
recycle_row(
	Run          *run,
	gint64        volume_id,
	const gchar  *original,
	gint64        inode,
	gint64        size,
	const gchar  *how,
	gint64       *row,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	/* No import: a duplicate a person removed (axis_import_retire_duplicate()). */
	g_autofree gchar *id = run->import_id > 0 ? axis_import_int_text(run->import_id) : NULL;
	g_autofree gchar *volume = axis_import_int_text(volume_id);
	g_autofree gchar *ino = axis_import_int_text(inode);
	g_autofree gchar *bytes = axis_import_int_text(size);
	const gchar *params[8];

	params[0] = id;
	params[1] = volume;
	params[2] = original;
	params[3] = ino;
	params[4] = bytes;
	params[5] = how;
	/* Only a duplicate removed by hand says what it was and who removed it. */
	params[6] = run->dup_label != NULL && run->dup_label[0] != '\0' ? run->dup_label : NULL;
	params[7] = run->dup_user != NULL && run->dup_user[0] != '\0' ? run->dup_user : NULL;
	if (!axis_db_pool_exec(run->self->pool, "INSERT INTO import_recycle (import_id, volume_id, original_path, inode,"
	        " size, how, label, by_user) VALUES ($1::bigint, $2::bigint, $3, $4::bigint, $5::bigint, $6, left($7, 500),"
	        " left($8, 200)) RETURNING id", 8, params, &result, error))
		return FALSE;
	return axis_db_result_get_int64(result, 0, 0, row);
}

static gboolean
recycle_kept(
	Run          *run,
	AxisDbConn   *conn,
	gint64        row,
	const gchar  *recycle_path,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(row);
	g_autofree gchar *retention = axis_import_int_text(run->settings.retention_s);
	const gchar *params[3];

	params[0] = id;
	params[1] = recycle_path;
	params[2] = retention;
	/* An import's files get their time when it is recorded complete
	 * (finish()); a row with no import (a duplicate a person removed) has no
	 * such moment, so its retention counts from now. */
	return axis_db_conn_exec(conn, "UPDATE import_recycle SET state = 'kept', recycle_path = $2,"
		" remove_after = CASE WHEN import_id IS NULL THEN now() + $3::bigint * interval '1 second' ELSE remove_after END"
		" WHERE id = $1::bigint", 3, params, NULL, error);
}

static gboolean
recycle_forget(
	Run          *run,
	gint64        row,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(row);
	const gchar *params[1];

	params[0] = id;
	return axis_db_pool_exec(run->self->pool, "DELETE FROM import_recycle WHERE id = $1::bigint AND state = 'moving'",
		1, params, NULL, error);
}

/* Places the film's file; on failure the library is as it was. */
static gboolean
place_film(
	Run                   *run,
	const AxisImportName  *name,
	GError               **error
){
	AxisImportFound *film = run->verdict.film;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *checksum = NULL;
	const AxisImportExisting *same;
	guint64 inode = 0;
	gint64 recycle = 0;
	g_autofree gchar *recycle_name = NULL;
	GError *local = NULL;

	run->root_fd = axis_volume_guard_open_dir(run->dest, run->root->rel_path, error);
	if (run->root_fd < 0)
		return FALSE;
	run->folder = g_strdup(name->folder);
	run->dest_dir = stored_of(name->folder);
	run->stem = g_strdup(name->stem);
	run->film_name = g_strdup(name->file);
	run->film_fd = folder(run, run->root_fd, name->folder, &local);
	if (run->film_fd < 0)
		goto fail;
	if (!step(run, "folder", run->film_row, &local))
		goto fail;
	{
		g_autofree gchar *file_stored = stored_of(name->file);

		rel = g_strconcat(run->dest_dir, "/", file_stored, NULL);
	}
	same = same_name_existing(run, rel);
	if (same != NULL) {
		/* An upgrade to the same name: the old file gets its name in the
		 * recycle folder first, so that it is never without one. */
		g_auto(GStrv) where = recycle_where(run->root->rel_path, rel);
		g_autofree gchar *original = original_stored(run->root->rel_path, rel);
		AxisDbConn *conn;
		gboolean ok;

		if (where == NULL || !recycle_row(run, run->root->volume_id, original, same->inode, same->size, "linked",
		        &recycle, &local))
			goto fail;
		if (!axis_volume_write_retire(run->dest, run->film_fd, name->file, (guint64)same->inode,
		        (const gchar *const *)where, run->settings.dir_mode, AXIS_VOLUME_RETIRE_LINK, &recycle_name, &local)) {
			recycle_forget(run, recycle, NULL);
			goto fail;
		}
		conn = axis_db_pool_acquire(run->self->pool, &local);
		ok = conn != NULL;
		if (ok) {
			g_autofree gchar *path = recycle_stored(where, recycle_name);

			ok = recycle_kept(run, conn, recycle, path, &local);
			axis_db_pool_release(run->self->pool, conn);
		}
		if (!ok)
			goto fail;
		run->recycled++;
	}
	if (!place_one(run, film, run->film_row, run->film_fd, run->dest_dir, name->file,
	        same != NULL ? (guint64)same->inode : 0, &inode, &checksum, &local))
		goto fail;
	run->film_inode = inode;
	if (run->report != NULL)
		run->report->placed++;
	return TRUE;

fail:
	if (!run->crashed) {
		/* The old file's extra name in the recycle folder is not needed
		 * when the swap did not happen: it stays (it is a second name of a
		 * file the library still names) and is removed after the
		 * retention like any recycled file. */
		undo_folders(run);
	}
	g_propagate_error(error, local);
	return FALSE;
}

/* ── Recording the film ─────────────────────────────────────────── */

static gboolean
link_film(
	Run     *run,
	GError **error
){
	g_autofree gchar *file_stored = stored_of(run->film_name);
	g_autofree gchar *rel = g_strconcat(run->dest_dir, "/", file_stored, NULL);
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *row = axis_import_int_text(run->film_row);
	g_autofree gchar *library_id = NULL;
	g_autofree gchar *root = axis_import_int_text(run->root->id);
	g_autofree gchar *volume = axis_import_int_text(run->root->volume_id);
	AxisImportLinked linked;
	const gchar *params[10];
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) &&
		index_file(run, conn, run->film_fd, run->film_name, rel, &run->film_library_id, error);
	if (ok) {
		library_id = axis_import_int_text(run->film_library_id);
		memset(&linked, 0, sizeof(linked));
		linked.import_id = run->import_id;
		linked.library_file_id = run->film_library_id;
		linked.quality = run->verdict.quality;
		linked.quality_text = run->verdict.quality_text;
		linked.release_name = run->target->grab_release != NULL ? run->target->grab_release : run->release_name;
		linked.mode = run->mode;
		linked.upgrade = run->verdict.upgrade;
		linked.user = run->override_by != NULL ? run->override_by : run->requested_by;
		/* Assigned by hand: the link is that person's, and kept. */
		if (run->assigned != NULL) {
			linked.hand_how = "assigned";
			linked.hand_by = run->assigned_by;
		}
		ok = run->owner->info->link(run->owner->data, conn, run->target, &linked, error);
	}
	params[0] = id;
	params[1] = run->mode;
	params[2] = run->mode_text;
	params[3] = axis_quality_name(run->verdict.quality);
	params[4] = run->verdict.quality_text;
	params[5] = run->verdict.upgrade ? "true" : "false";
	params[6] = run->dest_dir;
	params[7] = root;
	params[8] = volume;
	params[9] = NULL;
	ok = ok && axis_db_conn_exec(conn, "UPDATE import SET mode = $2, mode_text = $3, quality = $4, quality_text = $5,"
		" upgrade = $6::boolean, dest_dir = $7, root_id = $8::bigint, dest_volume_id = $9::bigint, updated_at = now()"
		" WHERE id = $1::bigint", 9, params, NULL, error);
	params[0] = row;
	params[1] = library_id;
	ok = ok && axis_db_conn_exec(conn, "UPDATE import_file SET library_file_id = $2::bigint WHERE id = $1::bigint", 2,
		params, NULL, error);
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	if (!ok)
		return FALSE;
	/* Its probe, now (the probe takes it once the root has had a complete
	 * scan). */
	{
		g_autoptr(GError) local = NULL;

		if (!axis_probe_request_file(run->self->probe, run->film_library_id, NULL, &local))
			g_debug("import %" G_GINT64_FORMAT ": the probe of the new file was not queued: %s", run->import_id,
				local->message);
	}
	return step(run, "linked", run->film_row, error);
}

/* ── Subtitles and extras ───────────────────────────────────────── */

static gboolean
name_taken(
	GPtrArray   *names,
	const gchar *name
){
	guint i;

	for (i = 0; i < names->len; i++) {
		if (strcmp(g_ptr_array_index(names, i), name) == 0)
			return TRUE;
	}
	return FALSE;
}

/* The sidecars, each by itself: one that cannot be placed is noted. */
static gboolean
place_sidecars(
	Run     *run,
	GError **error
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) placed = load_placed(run, error);
	guint i;

	if (placed == NULL)
		return FALSE;
	g_ptr_array_add(names, g_strdup(run->film_name));
	for (i = 0; run->found != NULL && i < run->found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(run->found, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *checksum = NULL;
		g_autofree gchar *dest_dir = NULL;
		gint64 row = 0;
		gint dir = -1;
		guint64 inode = 0;
		gboolean was_stranded;
		GError *local = NULL;
		guint k;

		if (f == run->verdict.film || f->role == NULL || !f->accepted || g_strcmp0(f->role, "film") == 0)
			continue;
		row = f->row_id;
		for (k = 0; k < placed->len; k++) {
			Placed *p = g_ptr_array_index(placed, k);

			if (p->id == row && g_strcmp0(p->step, "published") == 0)
				row = 0;
		}
		if (row <= 0)
			continue;
		if (g_strcmp0(f->role, "subtitle") == 0) {
			AxisImportSubtitle subtitle;
			guint number = 0;

			axis_import_subtitle_read(f->name, strlen(f->name), &subtitle);
			do {
				g_free(name);
				name = axis_import_subtitle_name(run->stem, &subtitle, number);
				number = number == 0 ? 2 : number + 1;
			} while (name_taken(names, name) && number < 100);
			dir = fcntl(run->film_fd, F_DUPFD_CLOEXEC, 0);
			dest_dir = g_strdup(run->dest_dir);
		} else {
			name = g_strdup(f->dest_name);
			if (f->dest_subdir != NULL) {
				g_autofree gchar *sub = stored_of(f->dest_subdir);

				dir = folder(run, run->film_fd, f->dest_subdir, &local);
				dest_dir = g_strconcat(run->dest_dir, "/", sub, NULL);
			} else {
				dir = fcntl(run->film_fd, F_DUPFD_CLOEXEC, 0);
				dest_dir = g_strdup(run->dest_dir);
			}
		}
		if (name == NULL || dir < 0) {
			note(run, "%s could not be placed: %s", f->name, local != NULL ? local->message : "no name");
			g_clear_error(&local);
			if (dir >= 0)
				close(dir);
			continue;
		}
		g_ptr_array_add(names, g_strdup(name));
		was_stranded = run->stranded;
		if (!place_one(run, f, row, dir, dest_dir, name, 0, &inode, &checksum, &local)) {
			close(dir);
			if (run->crashed) {
				g_propagate_error(error, local);
				return FALSE;
			}
			if (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_UNAVAILABLE ||
			    local->code == AXIS_STORAGE_ERROR_READ_ONLY)) {
				g_propagate_error(error, local);
				return FALSE;
			}
			/* Kept under its temporary name (noted, and the import stops for
			 * it): not a file left in the download. */
			if (!was_stranded && run->stranded) {
				g_error_free(local);
				continue;
			}
			note(run, "%s was not placed: %s", f->name, local->message);
			{
				g_autofree gchar *id = axis_import_int_text(row);
				g_autofree gchar *text = axis_import_text_cut(local->message, 1000);
				const gchar *params[2];

				params[0] = id;
				params[1] = text;
				axis_db_pool_exec(run->self->pool, "UPDATE import_file SET note = $2 WHERE id = $1::bigint", 2, params,
					NULL, NULL);
			}
			g_error_free(local);
			continue;
		}
		if (run->report != NULL)
			run->report->placed++;
		{
			g_autofree gchar *file_stored = stored_of(name);
			g_autofree gchar *rel = g_strconcat(dest_dir, "/", file_stored, NULL);
			g_autofree gchar *id = axis_import_int_text(row);
			g_autofree gchar *library_id = NULL;
			AxisDbConn *conn = axis_db_pool_acquire(run->self->pool, &local);
			gint64 file_id = 0;
			gboolean ok = conn != NULL && index_file(run, conn, dir, name, rel, &file_id, &local);

			if (ok) {
				const gchar *params[2];

				library_id = axis_import_int_text(file_id);
				params[0] = id;
				params[1] = library_id;
				ok = axis_db_conn_exec(conn, "UPDATE import_file SET library_file_id = $2::bigint WHERE id = $1::bigint",
					2, params, NULL, &local);
			}
			if (conn != NULL)
				axis_db_pool_release(run->self->pool, conn);
			if (!ok) {
				note(run, "%s was placed but not indexed: %s", name, local != NULL ? local->message : "?");
				g_clear_error(&local);
			}
		}
		close(dir);
	}
	return TRUE;
}

/* ── Retiring what an upgrade replaces ──────────────────────────── */

/* The first half of a retirement: the recycle row, then the file moved into
 * its volume's recycle folder.  %FALSE (with @error) when it could not be
 * moved: it stays, and the row is forgotten.  *@row is the recycle row and
 * *@path where the file is now (stored form). */
static gboolean
retire_move(
	Run              *run,
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *root_path,
	gint64            volume_id,
	const gchar      *rel_stored,
	const gchar      *name,
	gint64            inode,
	gint64            size,
	gint64           *row,
	gchar           **path,
	GError          **error
){
	g_auto(GStrv) where = recycle_where(root_path, rel_stored);
	g_autofree gchar *original = original_stored(root_path, rel_stored);
	g_autofree gchar *recycle_name = NULL;

	*row = 0;
	*path = NULL;
	if (where == NULL) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "A recorded path is not valid.");
		return FALSE;
	}
	if (!recycle_row(run, volume_id, original, inode, size, "moved", row, error))
		return FALSE;
	if (!axis_volume_write_retire(guard, dir, name, (guint64)inode, (const gchar *const *)where,
	        run->settings.dir_mode, AXIS_VOLUME_RETIRE_MOVE, &recycle_name, error)) {
		recycle_forget(run, *row, NULL);
		return FALSE;
	}
	*path = recycle_stored(where, recycle_name);
	return TRUE;
}

/* The second half: the recycle row marked kept at @path and the file's row
 * dropped from the index, in one transaction on a connection of its own. */
static gboolean
retire_record(
	Run          *run,
	gint64        row,
	const gchar  *path,
	gint64        root_id,
	const gchar  *rel_stored,
	gint64        inode,
	gint64        library_file_id,
	GError      **error
){
	g_autofree gchar *file = library_file_id > 0 ? axis_import_int_text(library_file_id) : NULL;
	g_autofree gchar *ino = axis_import_int_text(inode);
	g_autofree gchar *root = axis_import_int_text(root_id);
	AxisDbConn *conn;
	const gchar *params[3];
	gboolean ok;

	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = file;
	params[1] = ino;
	params[2] = root;
	ok = axis_db_conn_begin(conn, error) && recycle_kept(run, conn, row, path, error);
	/* Out of the index (its link to the title goes with it); by its path
	 * when it was not recorded by id (a subtitle). */
	if (ok && file != NULL)
		ok = axis_db_conn_exec(conn, "DELETE FROM library_file WHERE id = $1::bigint AND inode = $2::bigint", 2, params,
			NULL, error);
	else if (ok) {
		params[0] = rel_stored;
		ok = axis_db_conn_exec(conn, "DELETE FROM library_file WHERE root_id = $3::bigint"
			" AND path_hash = library_path_hash($1) AND rel_path = $1 AND inode = $2::bigint", 3, params, NULL, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	return ok;
}

/* Moves one file of the library into its volume's recycle folder and drops
 * it from the index.  %FALSE (with @error) when it could not: it stays. */
static gboolean
retire_file(
	Run              *run,
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *root_path,
	gint64            root_id,
	gint64            volume_id,
	const gchar      *rel_stored,
	const gchar      *name,
	gint64            inode,
	gint64            size,
	gint64            library_file_id,
	GError          **error
){
	g_autofree gchar *path = NULL;
	gint64 row = 0;
	gboolean ok;

	if (!retire_move(run, guard, dir, root_path, volume_id, rel_stored, name, inode, size, &row, &path, error))
		return FALSE;
	ok = retire_record(run, row, path, root_id, rel_stored, inode, library_file_id, error);
	if (ok) {
		run->recycled++;
		if (run->report != NULL)
			run->report->recycled++;
	}
	return ok;
}

/*
 * A duplicate a person removed on the Duplicates page (slice D of
 * add-force-grab-and-assign): retired by retire_file(), as an upgrade's old
 * file is, with no import behind it.  It is looked at on disk first: the
 * regular file the library records (inode and size), with one name only (a
 * second name means something else still uses it, whatever the last scan
 * said).  Its subtitles are not touched.
 */
/* The two statements of a moved duplicate's record, on @conn in its
 * transaction: the recycle row kept at @path, its retention counted from
 * now, and the file's row out of the index (its link goes with it).  A
 * recycle row that is no longer there (the rename took so long that the
 * cleanup took the row for one cut short and forgot it) is written again:
 * without a row nothing would list the file, and nothing would ever delete
 * it from the recycle folder. */
static gboolean
duplicate_statements(
	AxisDbConn                *conn,
	gint64                     row,
	const gchar               *path,
	const AxisImportExisting  *file,
	const gchar               *label,
	const gchar               *user,
	gint64                     retention_s,
	GError                   **error
){
	g_autoptr(AxisDbResult) kept = NULL;
	g_autofree gchar *id = axis_import_int_text(row);
	g_autofree gchar *retention = axis_import_int_text(retention_s);
	g_autofree gchar *volume = axis_import_int_text(file->volume_id);
	g_autofree gchar *original = original_stored(file->root_path, file->rel_path);
	g_autofree gchar *ino = axis_import_int_text(file->inode);
	g_autofree gchar *bytes = axis_import_int_text(file->size);
	g_autofree gchar *library = axis_import_int_text(file->library_file_id);
	const gchar *params[8];

	params[0] = id;
	params[1] = path;
	params[2] = retention;
	if (!axis_db_conn_exec(conn, "UPDATE import_recycle SET state = 'kept', recycle_path = $2,"
	        " remove_after = now() + $3::bigint * interval '1 second'"
	        " WHERE id = $1::bigint AND state IN ('moving', 'kept') RETURNING id", 3, params, &kept, error))
		return FALSE;
	if (axis_db_result_get_n_rows(kept) == 0) {
		params[0] = volume;
		params[1] = original;
		params[2] = path;
		params[3] = ino;
		params[4] = bytes;
		params[5] = retention;
		params[6] = label != NULL && label[0] != '\0' ? label : "a duplicate";
		params[7] = user != NULL && user[0] != '\0' ? user : NULL;
		g_warning("duplicates: the recycle row of a file that was moved was gone when the move ended; it is "
			"written again");
		if (!axis_db_conn_exec(conn, "INSERT INTO import_recycle (volume_id, original_path, recycle_path, inode, size,"
		        " how, state, remove_after, label, by_user) VALUES ($1::bigint, $2, $3, $4::bigint, $5::bigint, 'moved',"
		        " 'kept', now() + $6::bigint * interval '1 second', left($7, 500), left($8, 200))", 8, params, NULL,
		        error))
			return FALSE;
	}
	params[0] = library;
	params[1] = ino;
	return axis_db_conn_exec(conn, "DELETE FROM library_file WHERE id = $1::bigint AND inode = $2::bigint", 2, params,
		NULL, error);
}

/*
 * The record of a duplicate that was moved, tried once.  On @conn (the
 * transaction that holds the title and the files' links, locked: only that
 * connection can delete the link without waiting on the lock), under a
 * savepoint, so that a record that fails leaves the transaction usable; the
 * record is then committed with the title's transaction.  With no @conn (the
 * title's transaction is over): on a connection of its own, committed at
 * once.  The test hook's step "duplicate_moved" fails it as a lost database
 * would.
 */
gboolean
axis_import_record_duplicate(
	AxisImport                *self,
	AxisDbConn                *conn,
	gint64                     recycle_row,
	const gchar               *recycle_path,
	const AxisImportExisting  *file,
	const gchar               *label,
	const gchar               *user,
	GError                   **error
){
	AxisImportSettings settings;
	AxisDbConn *own;
	gint64 retention;
	gboolean ok;

	g_return_val_if_fail(self != NULL && file != NULL && recycle_path != NULL, FALSE);

	if (axis_import_hook(self, "duplicate_moved", 0, file->library_file_id) != AXIS_IMPORT_HOOK_GO) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the test's hook failed the record");
		return FALSE;
	}
	axis_import_settings_read(self, &settings);
	retention = settings.retention_s;
	axis_import_settings_clear(&settings);
	if (conn != NULL) {
		if (!axis_db_conn_exec(conn, "SAVEPOINT axis_duplicate_record", 0, NULL, NULL, error))
			return FALSE;
		ok = duplicate_statements(conn, recycle_row, recycle_path, file, label, user, retention, error) &&
			axis_db_conn_exec(conn, "RELEASE SAVEPOINT axis_duplicate_record", 0, NULL, NULL, error);
		if (!ok)
			axis_db_conn_exec(conn, "ROLLBACK TO SAVEPOINT axis_duplicate_record", 0, NULL, NULL, NULL);
		return ok;
	}
	own = axis_db_pool_acquire(self->pool, error);
	if (own == NULL)
		return FALSE;
	ok = axis_db_conn_begin(own, error) &&
		duplicate_statements(own, recycle_row, recycle_path, file, label, user, retention, error) &&
		axis_db_conn_commit(own, error);
	axis_db_conn_rollback(own, NULL);
	axis_db_pool_release(self->pool, own);
	return ok;
}

gboolean
axis_import_retire_duplicate(
	AxisImport                *self,
	AxisDbConn                *conn,
	const AxisImportExisting  *file,
	const gchar               *label,
	const gchar               *user,
	gint64                    *recycle_row,
	gchar                    **recycle_path,
	gboolean                  *moved,
	GError                   **error
){
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *path = NULL;
	AxisVolumeGuard *guard;
	const gchar *last;
	struct stat st;
	Run run;
	gint64 row = 0;
	guint n;
	gint root;
	gint dir = -1;
	gboolean ok;

	g_return_val_if_fail(self != NULL && file != NULL, FALSE);

	if (moved != NULL)
		*moved = FALSE;
	if (recycle_path != NULL)
		*recycle_path = NULL;
	if (recycle_row != NULL)
		*recycle_row = 0;
	parts = stored_parts(file->rel_path);
	if (parts == NULL || (n = g_strv_length(parts)) == 0 || file->library_file_id <= 0 || file->inode <= 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "A recorded path is not valid.");
		return FALSE;
	}
	last = parts[n - 1];
	memset(&run, 0, sizeof(run));
	run.self = self;
	run.dup_label = label;
	run.dup_user = user;
	axis_import_settings_read(self, &run.settings);
	guard = axis_volume_guard_acquire(self->storage, file->volume_id, AXIS_VOLUME_INTENT_WRITE, error);
	if (guard == NULL) {
		axis_import_settings_clear(&run.settings);
		return FALSE;
	}
	root = axis_volume_guard_open_dir(guard, file->root_path, error);
	if (root >= 0) {
		dir = open_below(root, parts, n - 1, axis_volume_guard_get_dev(guard), error);
		close(root);
	}
	ok = dir >= 0;
	if (ok && (fstatat(dir, last, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
	    (gint64)st.st_ino != file->inode || (gint64)st.st_size != file->size)) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED, "The file on disk is not the one "
			"the library records (it is gone, or another file has its name): it was left alone. Scan the library, then "
			"look again.");
		ok = FALSE;
	}
	if (ok && st.st_nlink != 1) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED, "The file has another name on "
			"its disk (a hard link, as a download that still seeds has): it was left alone.");
		ok = FALSE;
	}
	ok = ok && retire_move(&run, guard, dir, file->root_path, file->volume_id, file->rel_path, last, file->inode,
		file->size, &row, &path, error);
	if (dir >= 0)
		close(dir);
	axis_volume_guard_release(guard);
	if (ok) {
		/* The file is in the recycle folder.  Its record is tried twice, on
		 * the title's connection (see axis_import_record_duplicate()); when
		 * both fail it is said where the file is.  The caller tries once more
		 * on a connection of its own when the title's transaction turns out
		 * to be lost, and the cleanup finishes whatever record is still
		 * missing (axis_import_recover_duplicates()). */
		g_autoptr(GError) first = NULL;
		g_autoptr(GError) second = NULL;

		if (moved != NULL)
			*moved = TRUE;
		if (recycle_path != NULL)
			*recycle_path = g_strdup(path);
		if (recycle_row != NULL)
			*recycle_row = row;
		ok = axis_import_record_duplicate(self, conn, row, path, file, label, user, &first) ||
			axis_import_record_duplicate(self, conn, row, path, file, label, user, &second);
		if (!ok) {
			g_autofree gchar *shown = axis_library_name_display(path);

			g_warning("duplicates: a file was moved into the recycle folder (%s), but its records could not be "
				"updated: %s; then: %s", shown, first != NULL ? first->message : "?",
				second != NULL ? second->message : "?");
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "It was moved into the recycle folder of its "
				"disk (%s), but the database could not be reached to record that (%s). It is no longer at its place, "
				"though the library still lists it and its link remains for now: the clean-up job finishes the record "
				"within the hour, and deletes the file after the retention time counted from then.", shown,
				second != NULL ? second->message : "unknown");
		}
	}
	axis_import_settings_clear(&run.settings);
	return ok;
}

/*
 * The recycle rows with no import that are still 'moving' after
 * @older_than_s: a duplicate removed by hand whose record was cut short.  As
 * recover_recycled() does for an import, by where the recorded inode is now:
 * in the recycle folder with no other name (it is not at its place then),
 * the record is finished; at its place, the row is forgotten; anything else
 * is left for the next time.  Nothing is moved.
 */
gboolean
axis_import_recover_duplicates(
	AxisImport               *self,
	gint64                    older_than_s,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *age = axis_import_int_text(MAX(older_than_s, 0));
	const gchar *params[1];
	Run run;
	gboolean ok = TRUE;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	params[0] = age;
	/* Only a duplicate's rows (they have a label): a row whose import was
	 * deleted has no import either (ON DELETE SET NULL), and is not ours. */
	if (!axis_db_pool_exec(self->pool, "SELECT id, volume_id, original_path, inode, size FROM import_recycle"
	        " WHERE import_id IS NULL AND label IS NOT NULL AND state = 'moving'"
	        " AND retired_at <= now() - $1::bigint * interval '1 second' ORDER BY id LIMIT 500", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	memset(&run, 0, sizeof(run));
	run.self = self;
	axis_import_settings_read(self, &run.settings);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *original = axis_db_result_get_text(rows, i, 2);
		g_auto(GStrv) parts = stored_parts(original);
		g_auto(GStrv) where = NULL;
		g_autoptr(AxisVolumeGuard) guard = NULL;
		g_autofree gchar *path = NULL;
		gint64 row = 0;
		gint64 volume = 0;
		gint64 inode = 0;
		gint64 size = 0;
		gboolean recycled = FALSE;
		gboolean in_place = FALSE;
		struct stat st;
		guint n;
		guint k;
		gint dir;

		axis_db_result_get_int64(rows, i, 0, &row);
		axis_db_result_get_int64(rows, i, 1, &volume);
		axis_db_result_get_int64(rows, i, 3, &inode);
		axis_db_result_get_int64(rows, i, 4, &size);
		if (parts == NULL || (n = g_strv_length(parts)) == 0 || inode <= 0)
			continue;
		/* A volume that cannot be read now tells nothing: next time. */
		guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
		if (guard == NULL)
			continue;
		/* In the recycle folder, under its name or one with a number: the
		 * regular file recorded, and its only name. */
		where = g_new0(gchar *, n + 1);
		where[0] = g_strdup(AXIS_VOLUME_RECYCLE_DIR);
		for (k = 0; k + 1 < n; k++)
			where[k + 1] = g_strdup(parts[k]);
		dir = open_below(axis_volume_guard_get_fd(guard), where, n, axis_volume_guard_get_dev(guard), NULL);
		for (k = 0; dir >= 0 && k < 1000 && !recycled; k++) {
			g_autofree gchar *candidate = k == 0 ? g_strdup(parts[n - 1]) : g_strdup_printf("%.*s.%u",
				(gint)MIN(strlen(parts[n - 1]), (gsize)240), parts[n - 1], k);

			if (fstatat(dir, candidate, &st, AT_SYMLINK_NOFOLLOW) != 0) {
				if (k > 0)
					break;
				continue;
			}
			if (S_ISREG(st.st_mode) && (gint64)st.st_ino == inode && (gint64)st.st_size == size && st.st_nlink == 1) {
				g_auto(GStrv) inside = g_new0(gchar *, n);
				guint m;

				for (m = 0; m + 1 < n; m++)
					inside[m] = g_strdup(parts[m]);
				path = recycle_stored(inside, candidate);
				recycled = TRUE;
			}
		}
		if (dir >= 0)
			close(dir);
		if (!recycled) {
			/* Still at its place: the very file, looked at now. */
			dir = open_below(axis_volume_guard_get_fd(guard), parts, n - 1, axis_volume_guard_get_dev(guard), NULL);
			in_place = dir >= 0 && fstatat(dir, parts[n - 1], &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
				(gint64)st.st_ino == inode;
			if (dir >= 0)
				close(dir);
		}
		/* Looked at again through the guard: the volume is still the one. */
		if (!axis_volume_guard_check(guard, NULL))
			continue;
		if (recycled) {
			g_autoptr(AxisDbResult) kept = NULL;
			g_autofree gchar *id = axis_import_int_text(row);
			g_autofree gchar *retention = axis_import_int_text(run.settings.retention_s);
			g_autofree gchar *vol = axis_import_int_text(volume);
			g_autofree gchar *ino = axis_import_int_text(inode);
			g_autofree gchar *shown = axis_library_name_display(path);
			AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
			const gchar *mark[3];
			const gchar *more[3];
			gboolean ours = FALSE;

			if (conn == NULL) {
				ok = FALSE;
				break;
			}
			mark[0] = id;
			mark[1] = path;
			mark[2] = retention;
			more[0] = vol;
			more[1] = ino;
			more[2] = original;
			/* The row kept, only while it is still moving (a removal that was
			 * merely slow may have finished it meanwhile, and then the rest
			 * is its own), and the file's row out of the index, its link with
			 * it: the file is not at that place. */
			ok = axis_db_conn_begin(conn, error) &&
				axis_db_conn_exec(conn, "UPDATE import_recycle SET state = 'kept', recycle_path = $2,"
					" remove_after = now() + $3::bigint * interval '1 second'"
					" WHERE id = $1::bigint AND state = 'moving' RETURNING id", 3, mark, &kept, error);
			ours = ok && axis_db_result_get_n_rows(kept) > 0;
			if (ours)
				ok = axis_db_conn_exec(conn, "DELETE FROM library_file lf USING library_root r WHERE r.id = lf.root_id"
					" AND lf.volume_id = $1::bigint AND lf.inode = $2::bigint"
					" AND CASE WHEN r.rel_path = '' THEN lf.rel_path ELSE r.rel_path || '/' || lf.rel_path END = $3", 3,
					more, NULL, error) &&
				axis_db_conn_commit(conn, error);
			axis_db_conn_rollback(conn, NULL);
			axis_db_pool_release(self->pool, conn);
			if (ok && ours) {
				g_message("import cleanup: a duplicate removed by hand was in the recycle folder (%s) with its record "
					"unfinished; the record is finished now", shown);
				if (report != NULL)
					report->recovered++;
			}
		} else if (in_place) {
			ok = recycle_forget(&run, row, error);
			if (ok && report != NULL)
				report->recovered++;
		}
	}
	axis_import_settings_clear(&run.settings);
	return ok;
}

/* Whether @file is on its disk now as the library records it: a regular file
 * with that inode and size, reached through a read guard without a link.
 * %FALSE for anything else, and when it cannot be looked at. */
gboolean
axis_import_library_file_seen(
	AxisImport                *self,
	const AxisImportExisting  *file
){
	g_auto(GStrv) parts = stored_parts(file->rel_path);
	AxisVolumeGuard *guard;
	struct stat st;
	guint n;
	gint root;
	gint dir = -1;
	gboolean seen;

	if (parts == NULL || (n = g_strv_length(parts)) == 0)
		return FALSE;
	guard = axis_volume_guard_acquire(self->storage, file->volume_id, AXIS_VOLUME_INTENT_READ, NULL);
	if (guard == NULL)
		return FALSE;
	root = axis_volume_guard_open_dir(guard, file->root_path, NULL);
	if (root >= 0) {
		dir = open_below(root, parts, n - 1, axis_volume_guard_get_dev(guard), NULL);
		close(root);
	}
	seen = dir >= 0 && fstatat(dir, parts[n - 1], &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
		(gint64)st.st_ino == file->inode && (gint64)st.st_size == file->size;
	if (dir >= 0)
		close(dir);
	axis_volume_guard_release(guard);
	return seen;
}

/* The inodes of the files this import has placed (a run carried on after
 * one that died may have placed subtitles already: they are not the old
 * file's). */
static GHashTable *
placed_inodes(Run *run)
{
	g_autoptr(GPtrArray) placed = load_placed(run, NULL);
	GHashTable *inodes = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;

	for (i = 0; placed != NULL && i < placed->len; i++) {
		Placed *p = g_ptr_array_index(placed, i);

		if (p->dest_inode > 0 && (g_strcmp0(p->step, "published") == 0 || g_strcmp0(p->step, "swapped") == 0))
			g_hash_table_add(inodes, g_memdup2(&p->dest_inode, sizeof(p->dest_inode)));
	}
	if (run->film_inode > 0) {
		gint64 film = (gint64)run->film_inode;

		g_hash_table_add(inodes, g_memdup2(&film, sizeof(film)));
	}
	return inodes;
}

/* The subtitles of a file being retired: "<its stem>.*" beside it, but none
 * this import placed. */
static void
retire_sidecars(
	Run              *run,
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *root_path,
	gint64            root_id,
	gint64            volume_id,
	const gchar      *dir_stored,
	const gchar      *stem,
	GHashTable       *ours
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) others = g_ptr_array_new_with_free_func(g_free);
	gint copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
	DIR *listing = copy >= 0 ? fdopendir(copy) : NULL;
	struct dirent *entry;
	g_autofree gchar *prefix = g_strconcat(stem, ".", NULL);
	guint i;

	if (listing == NULL) {
		if (copy >= 0)
			close(copy);
		return;
	}
	while ((entry = readdir(listing)) != NULL) {
		AxisImportFileKind kind = axis_import_file_kind(entry->d_name, strlen(entry->d_name));

		if (!g_str_has_prefix(entry->d_name, prefix))
			continue;
		if (kind == AXIS_IMPORT_FILE_SUBTITLE && names->len < 64) {
			g_ptr_array_add(names, g_strdup(entry->d_name));
		} else if (kind == AXIS_IMPORT_FILE_VIDEO && others->len < 64) {
			/* Another video whose name begins with the old file's: the
			 * subtitles of its own name are its, not the old file's.  (The
			 * old file itself, and a new one of its name, have its stem.) */
			g_autofree gchar *ext = axis_import_extension(entry->d_name, strlen(entry->d_name));
			gchar *other_stem = ext != NULL ? g_strndup(entry->d_name, strlen(entry->d_name) - strlen(ext)) :
				g_strdup(entry->d_name);

			if (strlen(other_stem) > strlen(stem))
				g_ptr_array_add(others, other_stem);
			else
				g_free(other_stem);
		}
	}
	closedir(listing);
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		g_autofree gchar *name_stored = stored_of(name);
		g_autofree gchar *rel = dir_stored[0] != '\0' ? g_strconcat(dir_stored, "/", name_stored, NULL) :
			g_strdup(name_stored);
		g_autoptr(GError) local = NULL;
		struct stat st;
		gint64 ino;
		gboolean theirs = FALSE;
		guint k;

		for (k = 0; k < others->len; k++) {
			g_autofree gchar *other_prefix = g_strconcat(g_ptr_array_index(others, k), ".", NULL);

			if (g_str_has_prefix(name, other_prefix))
				theirs = TRUE;
		}
		if (theirs)
			continue;
		if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
			continue;
		ino = (gint64)st.st_ino;
		if (g_hash_table_contains(ours, &ino))
			continue;
		if (!retire_file(run, guard, dir, root_path, root_id, volume_id, rel, name, (gint64)st.st_ino,
		        (gint64)st.st_size, 0, &local))
			note(run, "A subtitle of the old file (%s) could not be retired, and stays: %s", name, local->message);
	}
}

/* The files of @replace (AxisImportExisting) retired, but not the one that
 * is @new_library_id (an upgrade to the same name: swapped already). */
static gboolean
retire_list(
	Run        *run,
	GPtrArray  *replace,
	gint64      new_library_id,
	GError    **error
){
	g_autoptr(GHashTable) ours = placed_inodes(run);
	guint i;

	for (i = 0; replace != NULL && i < replace->len; i++) {
		const AxisImportExisting *e = g_ptr_array_index(replace, i);
		g_auto(GStrv) parts = stored_parts(e->rel_path);
		g_autoptr(AxisVolumeGuard) other = NULL;
		g_autoptr(GError) local = NULL;
		g_autofree gchar *dir_stored = NULL;
		g_autofree gchar *stem = NULL;
		AxisVolumeGuard *guard = run->dest;
		const gchar *last;
		guint n;
		gint root = -1;
		gint dir = -1;
		gboolean same = e->library_file_id == new_library_id;

		if (parts == NULL || (n = g_strv_length(parts)) == 0)
			continue;
		last = parts[n - 1];
		{
			const gchar *slash = strrchr(e->rel_path, '/');

			dir_stored = slash != NULL ? g_strndup(e->rel_path, (gsize)(slash - e->rel_path)) : g_strdup("");
		}
		{
			g_autofree gchar *ext = axis_import_extension(last, strlen(last));

			stem = ext != NULL ? g_strndup(last, strlen(last) - strlen(ext)) : g_strdup(last);
		}
		if (e->volume_id != run->root->volume_id) {
			other = axis_volume_guard_acquire(run->self->storage, e->volume_id, AXIS_VOLUME_INTENT_WRITE, &local);
			if (other == NULL) {
				note(run, "The old file stays in the library beside the new one: its disk cannot be written now (%s).",
					local->message);
				continue;
			}
			guard = other;
		}
		root = axis_volume_guard_open_dir(guard, e->root_path, &local);
		if (root >= 0)
			dir = open_below(root, parts, n - 1, axis_volume_guard_get_dev(guard), &local);
		if (root >= 0)
			close(root);
		if (dir < 0) {
			note(run, "The old file stays in the library beside the new one: its folder cannot be opened (%s).",
				local->message);
			continue;
		}
		/* Its subtitles first: the new file's come after. */
		retire_sidecars(run, guard, dir, e->root_path, e->root_id, e->volume_id, dir_stored, stem, ours);
		if (!same && !retire_file(run, guard, dir, e->root_path, e->root_id, e->volume_id, e->rel_path, last, e->inode,
		        e->size, e->library_file_id, &local)) {
			if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE) && guard == run->dest) {
				close(dir);
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			{
				g_autofree gchar *shown = axis_library_name_display(e->rel_path);

				note(run, "The old file %s could not be moved into the recycle folder, so both it and the new one are "
					"in the library: %s", shown, local->message);
			}
		}
		close(dir);
	}
	return step(run, "retired", 0, error);
}

static gboolean
retire_old(
	Run     *run,
	GError **error
){
	return retire_list(run, run->verdict.replace, run->film_library_id, error);
}

/* A recycle row left 'moving' by a run that died: where is its inode? */
static gboolean
recover_recycled(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	const gchar *params[1];
	guint i;

	params[0] = id;
	if (!axis_db_pool_exec(run->self->pool, "SELECT id, volume_id, original_path, inode, how FROM import_recycle"
	        " WHERE import_id = $1::bigint AND state = 'moving'", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 row = 0;
		gint64 volume = 0;
		gint64 inode = 0;
		const gchar *original = axis_db_result_get_text(rows, i, 2);
		g_auto(GStrv) parts = stored_parts(original);
		g_autoptr(AxisVolumeGuard) other = NULL;
		AxisVolumeGuard *guard = run->dest;
		gchar **where;
		guint n;
		gint dir;
		guint k;
		gboolean found = FALSE;

		axis_db_result_get_int64(rows, i, 0, &row);
		axis_db_result_get_int64(rows, i, 1, &volume);
		axis_db_result_get_int64(rows, i, 3, &inode);
		if (parts == NULL || (n = g_strv_length(parts)) == 0)
			continue;
		if (volume != run->root->volume_id) {
			other = axis_volume_guard_acquire(run->self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
			if (other == NULL)
				continue;
			guard = other;
		}
		/* In the recycle folder, under its name or one with a number. */
		where = g_new0(gchar *, n + 1);
		where[0] = g_strdup(AXIS_VOLUME_RECYCLE_DIR);
		for (k = 0; k + 1 < n; k++)
			where[k + 1] = g_strdup(parts[k]);
		dir = open_below(axis_volume_guard_get_fd(guard), where, n, axis_volume_guard_get_dev(guard), NULL);
		if (dir >= 0) {
			for (k = 0; k < 1000 && !found; k++) {
				g_autofree gchar *candidate = k == 0 ? g_strdup(parts[n - 1]) : g_strdup_printf("%.*s.%u",
					(gint)MIN(strlen(parts[n - 1]), (gsize)240), parts[n - 1], k);
				struct stat st;

				if (fstatat(dir, candidate, &st, AT_SYMLINK_NOFOLLOW) != 0) {
					if (k > 0)
						break;
					continue;
				}
				if (S_ISREG(st.st_mode) && (gint64)st.st_ino == inode) {
					g_auto(GStrv) inside = g_new0(gchar *, n);
					g_autofree gchar *path = NULL;
					AxisDbConn *conn;
					guint m;

					for (m = 0; m + 1 < n; m++)
						inside[m] = g_strdup(parts[m]);
					path = recycle_stored(inside, candidate);
					conn = axis_db_pool_acquire(run->self->pool, error);
					if (conn == NULL) {
						close(dir);
						g_strfreev(where);
						return FALSE;
					}
					if (!recycle_kept(run, conn, row, path, error)) {
						axis_db_pool_release(run->self->pool, conn);
						close(dir);
						g_strfreev(where);
						return FALSE;
					}
					axis_db_pool_release(run->self->pool, conn);
					found = TRUE;
				}
			}
			close(dir);
		}
		g_strfreev(where);
		/* Not moved: the row goes, and the step is made again. */
		if (!found && !recycle_forget(run, row, error))
			return FALSE;
	}
	return TRUE;
}

/* ── Finishing ──────────────────────────────────────────────────── */

/* Records that the title's folder changed, for each media server that is set
 * up: they scan that folder (the compat module's job, a little later).  The
 * import is complete and recorded by now, and a media server never fails or
 * holds up one: a failure here is logged, and the server finds the file by
 * its own scan in time. */
static void
notify_servers(Run *run)
{
	g_autoptr(GError) local = NULL;

	if (!axis_compat_notify_record_import(run->self->pool, run->self->settings, run->import_id, &local))
		g_warning("import %" G_GINT64_FORMAT ": the media servers could not be told which folder changed (they find "
			"it when they scan): %s", run->import_id, local != NULL ? local->message : "unknown");
}

/* The import's row was not 'running' when its end was to be recorded: its
 * state was changed while the run placed its files (it should not be: the
 * sweep and the run each end an import only in a statement that checks for
 * the other).  Said, loudly, and the run fails; what it placed stays. */
static gboolean
not_running(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	const gchar *params[1];
	const gchar *state = NULL;

	params[0] = id;
	if (axis_db_pool_exec(run->self->pool, "SELECT state || coalesce('/' || reason, '') FROM import"
	        " WHERE id = $1::bigint", 1, params, &row, NULL) && axis_db_result_get_n_rows(row) > 0)
		state = axis_db_result_get_text(row, 0, 0);
	g_warning("import %" G_GINT64_FORMAT ": its files were placed, but it is no longer running (%s): it is not "
		"recorded as imported", run->import_id, state != NULL ? state : "gone");
	g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "The import is no longer running (it is %s now): "
		"something changed it while its files were placed, so it is not recorded as imported. What was placed is in "
		"the library.", state != NULL ? state : "gone");
	return FALSE;
}

static gboolean
finish(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) done = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *retention = axis_import_int_text(run->settings.retention_s);
	g_autofree gchar *text = NULL;
	g_autofree gchar *shown = NULL;
	/* A torrent whose client let go of it before its files were moved keeps
	 * what was recorded then. */
	/* Files a person left alone when they assigned the others by hand stay
	 * where they are: the download is not removed from its client, and its
	 * folder is not deleted. */
	gboolean leaves = leaves_files(run);
	const gchar *removal = run->not_ours ? "kept" : run->released != NULL ? run->released : run->download_id > 0 &&
		run->protocol_known ? (!leaves && (run->protocol == AXIS_DOWNLOAD_USENET || run->settings.torrents_remove) ?
		"pending" : "kept") : NULL;
	/* What SABnzbd keeps of a Usenet job after removing it, and what a torrent
	 * whose files were moved left (axis-import-folder.c); never a torrent that
	 * is not Axis's any more. */
	const gchar *folder = !leaves && !run->not_ours && run->download_id > 0 && run->protocol_known &&
		(run->protocol == AXIS_DOWNLOAD_USENET || (run->released != NULL && mode_moves(run->mode))) ? "pending" : NULL;
	const gchar *params[5];
	gboolean marked = FALSE;
	AxisDbConn *conn;
	gboolean ok;

	{
		g_autofree gchar *file_stored = stored_of(run->film_name);
		g_autofree gchar *rel = g_strconcat(run->dest_dir, "/", file_stored, NULL);

		shown = axis_library_name_display(rel);
	}
	text = g_strdup_printf("%s %s, in %s, by %s.%s%s%s", run->verdict.upgrade ? "Upgraded to" : "Imported as", shown,
		run->root->name, run->mode_text != NULL ? run->mode_text : run->mode,
		run->recycled > 0 ? " The file it replaced is in the recycle folder of its disk." : "",
		run->notes->len > 0 ? " " : "", run->notes->str);
	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	params[1] = removal;
	params[2] = run->notes->len > 0 ? run->notes->str : NULL;
	params[3] = folder;
	params[4] = run->released != NULL || run->not_ours ? "true" : "false";
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "UPDATE import SET state = 'imported', finished_at = now(), updated_at = now(),"
			" reason = NULL, reason_text = left($3, 2000), blockers = '[]', retry_after = NULL,"
			" client_removal = CASE WHEN $5::boolean THEN client_removal ELSE $2 END,"
			" folder_removal = $4, folder_removal_text = NULL"
			" WHERE id = $1::bigint AND state = 'running' RETURNING id", 5, params, &done, error);
	/* Its state changed while it ran (something ended it meanwhile): never
	 * recorded as imported as if nothing had (the files placed stay). */
	if (ok && axis_db_result_get_n_rows(done) == 0)
		ok = not_running(run, error);
	if (ok) {
		const gchar *more[2];

		more[0] = id;
		more[1] = retention;
		ok = axis_db_conn_exec(conn, "UPDATE import_recycle SET remove_after = now() + $2::integer * interval"
			" '1 second' WHERE import_id = $1::bigint AND state = 'kept' AND remove_after IS NULL", 2, more, NULL,
			error);
	}
	if (ok && axis_db_result_get_n_rows(done) > 0) {
		if (run->download_id > 0)
			ok = axis_downloads_mark_imported(conn, run->download_id, &marked, error);
		if (ok && run->owner->info->note != NULL)
			ok = run->owner->info->note(run->owner->data, conn, run->target, run->verdict.upgrade ? "upgraded" :
				"imported", text, NULL, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	if (!ok)
		return FALSE;
	if (marked)
		axis_downloads_notify_changed(run->self->downloads, run->download_id);
	notify_servers(run);
	if (run->report != NULL) {
		run->report->state = "imported";
		run->report->reason = NULL;
	}
	return step(run, "recorded", 0, error);
}

/* ── The gate and the decision ──────────────────────────────────── */

/* The volumes, through their guards; %TRUE with *@go %FALSE when the import
 * waits (the reason recorded). */
static gboolean
take_guards(
	Run       *run,
	gboolean  *go,
	GError   **error
){
	g_autoptr(AxisVolumeInfo) volume = NULL;
	GError *local = NULL;

	*go = FALSE;
	volume = axis_storage_get_volume(run->self->storage, run->root->volume_id);
	run->dest = axis_volume_guard_acquire(run->self->storage, run->root->volume_id, AXIS_VOLUME_INTENT_WRITE, &local);
	if (run->dest == NULL) {
		g_autofree gchar *text = NULL;
		const gchar *name = volume != NULL ? volume->name : "the destination's disk";
		gboolean ok;

		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY) &&
		    axis_storage_is_read_only_mode(run->self->storage)) {
			g_error_free(local);
			return wait_for(run, "read_only", "Read-only storage is on (Settings, Storage; storage.read_only), and "
				"Axis writes nothing to any disk while it is. Turn it off to let imports into the library.", 60, FALSE,
				error);
		}
		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY)) {
			text = g_strdup_printf("Disk %s, where %s goes, is mounted read-only: nothing is written to it until it "
				"is mounted read-write.", name, run->root->name);
			ok = wait_for(run, "volume_read_only", text, 60, FALSE, error);
		} else if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND)) {
			text = g_strdup_printf("The destination's disk is no longer known.");
			ok = block_one(run, "no_volume", FALSE, text, error);
		} else {
			text = g_strdup_printf("Disk %s, where %s goes, is offline: %s", name, run->root->name, local->message);
			ok = wait_for(run, "volume_offline", text, 60, FALSE, error);
		}
		g_error_free(local);
		return ok;
	}
	run->same_volume = run->source_volume_id == run->root->volume_id;
	if (run->same_volume) {
		run->source = run->dest;
	} else {
		run->source = axis_volume_guard_acquire(run->self->storage, run->source_volume_id, AXIS_VOLUME_INTENT_READ,
			&local);
		if (run->source == NULL) {
			g_autoptr(AxisVolumeInfo) source = axis_storage_get_volume(run->self->storage, run->source_volume_id);
			g_autofree gchar *text = g_strdup_printf("The download's disk, %s, is offline: %s", source != NULL ?
				source->name : "a disk", local->message);
			gboolean known = !g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND);

			g_error_free(local);
			return known ? wait_for(run, "source_offline", text, 60, FALSE, error) :
				block_one(run, "no_source", FALSE, "The download's place is not on a disk Axis knows.", error);
		}
	}
	*go = TRUE;
	return TRUE;
}

/*
 * A Usenet job of one file is reported by that file inside the job's folder
 * (SABnzbd: <category>/<job>/<file>, seen on a real server).  So that the
 * import takes what is beside it (a subtitle) as it would had the client
 * reported the folder, its source becomes the job's folder, by the one
 * definition the leftover folder's removal uses (axis_import_job_folder():
 * the first part beneath the folder the client keeps Axis's category in, as
 * it says now), and only when all of this holds: nothing of the import has
 * been placed yet; its source is still the place the download was recorded
 * at, a regular file reached without a link; the client's path for it maps,
 * through a mapping that holds the category's folder too, to that very
 * place; the job's folder is a folder reached without a link, and neither
 * inside the title's root nor holding it.  Otherwise (a file straight in the
 * category's folder, a client that keeps the category in no folder of its
 * own, or one that cannot be asked) the source stays the file, as before.
 */
static gboolean
widen_source(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *download = axis_import_int_text(run->download_id);
	g_autofree gchar *client_path = NULL;
	g_autofree gchar *category = NULL;
	g_autofree gchar *mapped = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *job_rel = NULL;
	g_autofree gchar *parent = NULL;
	const gchar *params[3];
	const gchar *slash;
	struct stat st;
	gint64 client_id = 0;
	gint64 volume = 0;
	gint fd;

	if (run->download_id <= 0 || !run->protocol_known || run->protocol != AXIS_DOWNLOAD_USENET ||
	    run->source == NULL || run->source_path == NULL || run->source_path[0] == '\0')
		return TRUE;
	params[0] = id;
	params[1] = download;
	if (!axis_db_pool_exec(run->self->pool, "SELECT d.client_id, d.client_path, d.volume_id, d.rel_path,"
	        " EXISTS (SELECT 1 FROM import_file f WHERE f.import_id = $1::bigint AND (f.step IS NOT NULL"
	        "  OR f.temp_name IS NOT NULL OR f.dest_inode IS NOT NULL OR f.library_file_id IS NOT NULL))"
	        " FROM download d WHERE d.id = $2::bigint", 2, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0 || axis_db_result_is_null(rows, 0, 0) ||
	    axis_db_result_is_null(rows, 0, 2) || g_strcmp0(axis_db_result_get_text(rows, 0, 4), "f") != 0)
		return TRUE;
	axis_db_result_get_int64(rows, 0, 0, &client_id);
	axis_db_result_get_int64(rows, 0, 2, &volume);
	/* Still the recorded place, never widened before. */
	if (volume != run->source_volume_id || g_strcmp0(axis_db_result_get_text(rows, 0, 3), run->source_path) != 0 ||
	    axis_db_result_get_text(rows, 0, 1) == NULL ||
	    !axis_storage_path_normalize(axis_db_result_get_text(rows, 0, 1), TRUE, &client_path, NULL))
		return TRUE;
	/* A regular file at that place, reached without a link. */
	slash = strrchr(run->source_path, '/');
	parent = slash != NULL ? g_strndup(run->source_path, (gsize)(slash - run->source_path)) : g_strdup("");
	fd = axis_volume_guard_open_dir(run->source, parent, NULL);
	if (fd < 0)
		return TRUE;
	if (fstatat(fd, slash != NULL ? slash + 1 : run->source_path, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
	    !S_ISREG(st.st_mode)) {
		close(fd);
		return TRUE;
	}
	close(fd);
	/* Where the client keeps Axis's category, as it says now. */
	if (!axis_downloads_category_folder(run->self->downloads, client_id, &category, &local)) {
		if (!g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) &&
		    !g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) &&
		    !g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
			note(run, "Its client could not say where it keeps Axis's category (%s), so only the file it reported "
				"was looked at, not the job's folder around it.", local->message);
		return TRUE;
	}
	if (!axis_downloads_map_client_path(run->self->downloads, client_id, client_path, &volume, &mapped, &prefix,
	        &problem, error)) {
		if (error != NULL && *error != NULL)
			return FALSE;
		return TRUE;
	}
	if (volume != run->source_volume_id || g_strcmp0(mapped, run->source_path) != 0 || prefix == NULL ||
	    !axis_storage_path_contains(prefix, category))
		return TRUE;
	if (axis_import_job_folder(category, client_path, mapped, NULL, &job_rel, NULL) != AXIS_IMPORT_JOB_FOLDER)
		return TRUE;
	/* Never inside the title's root, nor holding it. */
	if (run->root != NULL && run->source_volume_id == run->root->volume_id &&
	    (axis_storage_path_contains(run->root->rel_path, job_rel) ||
	     axis_storage_path_contains(job_rel, run->root->rel_path)))
		return TRUE;
	/* A folder, reached without a link. */
	fd = axis_volume_guard_open_dir(run->source, job_rel, NULL);
	if (fd < 0)
		return TRUE;
	close(fd);
	params[1] = job_rel;
	params[2] = run->source_path;
	g_clear_pointer(&rows, axis_db_result_free);
	if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET source_path = $2, source_inode = NULL, updated_at = now()"
	        " WHERE id = $1::bigint AND source_path = $3 AND state IN ('waiting', 'running') RETURNING id", 3, params,
	        &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	g_free(run->source_path);
	run->source_path = g_steal_pointer(&job_rel);
	return TRUE;
}

/*
 * Whether this import moves the download's files (import.take_files
 * "move"): a Usenet download's, and a torrent's when torrents are removed
 * after their import and its client can be asked to let go of it (or did
 * already, for an earlier run).  Never a manual import of a folder with no
 * download (whose files they are is not known), nor a download whose
 * protocol is not known.  *@why: when the setting moves but this import
 * does not, the reason, for the page.
 */
static gboolean
moves(
	Run       *run,
	gboolean  *moving,
	gchar    **why,
	GError   **error
){
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) local = NULL;

	*moving = FALSE;
	*why = NULL;
	if (!run->settings.move)
		return TRUE;
	/* A torrent its client let go of for an earlier run is moved, its
	 * download's record gone since notwithstanding (its client is asked about
	 * it by its hash first: release_torrent()). */
	if (run->released != NULL && run->protocol_known && run->protocol == AXIS_DOWNLOAD_TORRENT &&
	    run->settings.torrents_remove) {
		*moving = TRUE;
		return TRUE;
	}
	if (run->download_id <= 0 || !run->protocol_known) {
		*why = g_strdup("files imported by hand from a folder are never moved: they stay where they are");
		return TRUE;
	}
	if (run->protocol == AXIS_DOWNLOAD_USENET) {
		*moving = TRUE;
		return TRUE;
	}
	if (!run->settings.torrents_remove) {
		*why = g_strdup("the torrent is left seeding, as \"After a torrent is imported\" says");
		return TRUE;
	}
	if (run->released != NULL) {
		*moving = TRUE;
		return TRUE;
	}
	if (!axis_downloads_get(run->self->downloads, run->download_id, &info, &local)) {
		if (!g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		*why = g_strdup("Axis no longer has the record of the download, so its client cannot be asked to let go of "
			"the torrent");
		return TRUE;
	}
	if (info->client_id == 0 || info->client_ref == NULL) {
		*why = g_strdup("its client is no longer set up in Axis, so it cannot be asked to let go of the torrent");
		return TRUE;
	}
	*moving = TRUE;
	return TRUE;
}

/*
 * Before a torrent's files are moved: its client lets go of it.  The torrent
 * is removed from its client without its data, and the client is asked
 * again until it says itself that it no longer has it
 * (axis_downloads_release()); only then is anything moved, so that the
 * client never finds its files gone while it holds the torrent.  What the
 * client answered is recorded (client_removal "done", or "gone" when it had
 * no such torrent already).  Every run that moves files asks again, an
 * earlier release notwithstanding: a torrent added to its client again since
 * (by a person, an automation) is let go of again before its files are moved
 * (a torrent still gone costs one question).  When Axis first asked is
 * recorded before it asks, so that the next run, after one that died between
 * the client's removal and its record, does not say that something else
 * removed it.  A client that cannot be asked, or still has the torrent, makes
 * the import wait: nothing is moved.
 */
/* What the import page says of the client after Axis went on without asking
 * it (client_removal_text, said once), and in the import's own notes. */
static gboolean
went_on(
	Run          *run,
	const gchar  *text,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	const gchar *params[2];

	params[0] = id;
	params[1] = text;
	note(run, "%s", text);
	return axis_db_pool_exec(run->self->pool, "UPDATE import SET client_removal_text = left(CASE WHEN"
		" client_removal_text IS NULL OR client_removal_text = '' THEN $2 ELSE client_removal_text || ' ' || $2 END,"
		" 1000), updated_at = now() WHERE id = $1::bigint AND position($2 IN coalesce(client_removal_text, '')) = 0", 2,
		params, NULL, error);
}

/*
 * The torrent's client has it in another category or label than Axis's (a
 * person moved it there, to keep it): it is theirs now.  Nothing more is asked
 * of the client, nothing of it is removed or deleted, and its files are taken
 * without moving them: hardlinked on one volume, copied with verification
 * across volumes, the download's files left where they are (client_removal
 * "kept", saying why).
 */
static gboolean
not_ours(
	Run          *run,
	const gchar  *why,
	gboolean     *go,
	GError      **error
){
	g_autoptr(AxisVolumeInfo) dest = axis_storage_get_volume(run->self->storage, run->root->volume_id);
	g_autoptr(AxisVolumeInfo) source = axis_storage_get_volume(run->self->storage, run->source_volume_id);
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *text = g_strdup_printf("The torrent is not Axis's any more: %s. Axis left it at its client, "
		"untouched, and took its files without moving them (a hardlink, or a verified copy across disks); nothing of "
		"it is removed or deleted.", why);
	const gchar *params[2];

	g_free(run->mode);
	g_free(run->mode_text);
	if (run->same_volume) {
		run->mode = g_strdup("hardlink");
		run->mode_text = g_strdup_printf("a hardlink on %s: the download's file is untouched and takes no more room "
			"(the torrent is not Axis's any more: a person moved it to another category or label at its client, so its "
			"files were not moved)", dest != NULL ? dest->name : "its disk");
	} else {
		run->mode = g_strdup("copy");
		run->mode_text = g_strdup_printf("a verified copy from %s to %s; the download's file stays (the torrent is not "
			"Axis's any more: a person moved it to another category or label at its client, so its files were not "
			"moved)", source != NULL ? source->name : "another disk", dest != NULL ? dest->name : "its disk");
	}
	run->not_ours = TRUE;
	note(run, "%s", text);
	params[0] = id;
	params[1] = text;
	if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET client_removal = 'kept', client_removal_text ="
	        " left($2, 1000), updated_at = now() WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	*go = TRUE;
	return TRUE;
}

typedef enum
{
	CLIENT_HAS,		/* the client lists the torrent */
	CLIENT_HAS_NOT,		/* its whole list of Axis's category, without it */
	CLIENT_CANNOT_TELL,	/* not reached, or its list not all read */
	CLIENT_NOT_IN_AXIS,	/* no such client in Axis any more */
	CLIENT_ELSEWHERE	/* it has it in another category or label */
} ClientHas;

/* Whether client @client_id has the torrent @ref now, asked by its hash
 * among the torrents of Axis's category there.  *@why: for
 * %CLIENT_CANNOT_TELL, why. */
static gboolean
client_has_torrent(
	Run          *run,
	gint64        client_id,
	const gchar  *ref,
	ClientHas    *has,
	gchar       **why,
	GError      **error
){
	g_autoptr(GPtrArray) items = NULL;
	GError *local = NULL;
	guint unreadable = 0;
	guint i;

	*has = CLIENT_CANNOT_TELL;
	*why = NULL;
	/* By its hash in every category, where the kind can: one a person moved
	 * to another category is not Axis's (and its files are not moved). */
	{
		g_autofree gchar *category = NULL;
		gboolean found = FALSE;
		gboolean ours = FALSE;

		if (axis_downloads_find_torrent(run->self->downloads, client_id, ref, &found, &ours, &category, &local)) {
			*has = !found ? CLIENT_HAS_NOT : ours ? CLIENT_HAS : CLIENT_ELSEWHERE;
			if (*has == CLIENT_ELSEWHERE)
				*why = g_strdup_printf("its client has it in the category or label \"%s\" now, not in Axis's: a person "
					"moved it there", category != NULL ? category : "");
			return TRUE;
		}
		if (local->domain == AXIS_DB_ERROR) {
			g_propagate_error(error, local);
			return FALSE;
		}
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND)) {
			*has = CLIENT_NOT_IN_AXIS;
			g_error_free(local);
			return TRUE;
		}
		if (!g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID)) {
			*why = g_strdup_printf("its client could not be asked (%s)", local->message);
			g_error_free(local);
			return TRUE;
		}
		/* A kind that cannot look a torrent up: by its list of Axis's
		 * category, as before. */
		g_clear_error(&local);
	}
	if (!axis_downloads_client_items(run->self->downloads, client_id, &items, &unreadable, &local)) {
		if (local->domain == AXIS_DB_ERROR) {
			g_propagate_error(error, local);
			return FALSE;
		}
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
			*has = CLIENT_NOT_IN_AXIS;
		else
			*why = g_strdup_printf("its client could not be asked (%s)", local->message);
		g_error_free(local);
		return TRUE;
	}
	/* The list holds every category: one in another than Axis's is not
	 * Axis's. */
	for (i = 0; i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);

		if ((item->client_ref != NULL && g_ascii_strcasecmp(item->client_ref, ref) == 0) ||
		    (item->info_hash != NULL && g_ascii_strcasecmp(item->info_hash, ref) == 0)) {
			g_autoptr(AxisDownloadClientInfo) info = NULL;

			if (!axis_downloads_get_client(run->self->downloads, client_id, &info, &local)) {
				if (local->domain == AXIS_DB_ERROR) {
					g_propagate_error(error, local);
					return FALSE;
				}
				*why = g_strdup_printf("its client could not be asked (%s)", local->message);
				g_error_free(local);
				return TRUE;
			}
			if (item->category != NULL && info->category != NULL &&
			    g_ascii_strcasecmp(item->category, info->category) != 0) {
				*has = CLIENT_ELSEWHERE;
				*why = g_strdup_printf("its client has it in the category or label \"%s\" now, not in Axis's: a person "
					"moved it there", item->category);
				return TRUE;
			}
			*has = CLIENT_HAS;
			return TRUE;
		}
	}
	if (unreadable > 0) {
		*why = g_strdup("its client's list of torrents could not all be read");
		return TRUE;
	}
	*has = CLIENT_HAS_NOT;
	return TRUE;
}

/* A torrent let go of by an earlier run (recorded) that its client cannot be
 * asked to let go of again now: the files are moved when nothing holds the
 * torrent; else the import waits, a few times, then a person decides. */
static gboolean
release_again_failed(
	Run       *run,
	GError    *failure,
	gboolean  *go,
	GError   **error
){
	g_autofree gchar *text = NULL;
	gboolean refused = g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_autofree gchar *cannot = NULL;

	if (axis_import_overridden(run->overrides, "client_release")) {
		*go = TRUE;
		return TRUE;
	}
	/* The download's record cannot be used to ask its client any more (it
	 * is gone, or not a completed torrent any more: a person removed it on
	 * the Downloads page, its client lost it, or the client was removed from
	 * Axis): the client, when it is still in Axis, is asked by the torrent's
	 * hash; the files are moved only when it does not list it. */
	if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) ||
	    g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID)) {
		g_autoptr(AxisDownloadInfo) info = NULL;
		g_autofree gchar *why = NULL;
		g_autofree gchar *cause = g_strdup_printf("the download's record in Axis can no longer be used to ask its "
			"client (%s)", failure->message);
		gint64 client_id = run->release_client_id;
		const gchar *ref = run->release_ref;
		ClientHas has;

		if (run->download_id > 0 && axis_downloads_get(run->self->downloads, run->download_id, &info, NULL) &&
		    info->client_id > 0 && info->client_ref != NULL) {
			client_id = info->client_id;
			ref = info->client_ref;
		}
		if (client_id <= 0 || ref == NULL) {
			text = g_strdup_printf("Axis moved the torrent's files without asking its client again: %s, and which "
				"client had it was not recorded.", cause);
			if (!went_on(run, text, error))
				return FALSE;
			*go = TRUE;
			return TRUE;
		}
		if (!client_has_torrent(run, client_id, ref, &has, &why, error))
			return FALSE;
		switch (has) {
		case CLIENT_NOT_IN_AXIS:
			text = g_strdup_printf("Axis moved the torrent's files without asking its client again: %s, and that "
				"client is no longer set up in Axis.", cause);
			if (!went_on(run, text, error))
				return FALSE;
			*go = TRUE;
			return TRUE;
		case CLIENT_HAS_NOT:
			text = g_strdup_printf("Axis moved the torrent's files without asking its client to let go of it again: "
				"%s; its client, asked by the torrent's hash, does not have it.", cause);
			if (!went_on(run, text, error))
				return FALSE;
			*go = TRUE;
			return TRUE;
		case CLIENT_HAS:
			text = g_strdup_printf("Its client let go of the torrent before (recorded), and has it again: asked by the "
				"torrent's hash, it lists it. Axis does not ask it to let go this time, as %s. Nothing was moved. Remove "
				"the torrent there (keeping its files), then retry; or import anyway.", cause);
			return block_one(run, "client_release", TRUE, text, error);
		case CLIENT_ELSEWHERE:
			/* At its client again, in a category of a person's: theirs. */
			return not_ours(run, why, go, error);
		case CLIENT_CANNOT_TELL:
		default:
			cannot = g_strdup_printf("%s, and whether its client has the torrent cannot be told now: %s", cause, why);
			break;
		}
	}
	/* One that exists and cannot be reached, or has the torrent again:
	 * waits, a few times, then a person decides. */
	if (cannot == NULL)
		cannot = g_strdup(failure->message);
	if (run->attempts + 1 < AXIS_IMPORT_PLACE_ATTEMPTS) {
		text = g_strdup_printf("Its client let go of the torrent before (recorded), and is asked again before its files "
			"are moved; it could not be now (%s): nothing was moved, and the import tries again (%d of %d tries).", cannot,
			run->attempts + 1, AXIS_IMPORT_PLACE_ATTEMPTS);
		return wait_for(run, "client_release", text, refused ? 600 : 60, TRUE, error);
	}
	text = g_strdup_printf("Its client let go of the torrent before (recorded), and could not be asked again before its "
		"files are moved, %d times (%s). Nothing was moved. If the torrent is not at its client any more, import anyway; "
		"if it is (added again), remove it there first.", AXIS_IMPORT_PLACE_ATTEMPTS, cannot);
	return block_one(run, "client_release", TRUE, text, error);
}

/* A torrent whose client could not be made to let go of it: nothing is
 * moved.  A cause that time may clear (a client not reached, one that still
 * lists the torrent) is tried again a few times; one it will not (the
 * download's record cannot be used to ask) blocks at once; then a person
 * decides: remove the torrent at its client, or import anyway. */
static gboolean
release_failed(
	Run       *run,
	GError    *failure,
	gboolean  *go,
	GError   **error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *cause = NULL;
	gboolean refused = g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	gboolean lasting = g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) ||
		g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);

	if (axis_import_overridden(run->overrides, "client_release")) {
		text = g_strdup_printf("A person chose to import anyway: the torrent's client had not let go of it (%s), and "
			"its files were moved all the same.", failure->message);
		if (!went_on(run, text, error))
			return FALSE;
		*go = TRUE;
		return TRUE;
	}
	if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID))
		cause = g_strdup_printf("Axis's record of the download no longer lets it ask the client (%s): the download "
			"was removed on the Downloads page, say, or its client lost it", failure->message);
	else if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
		cause = g_strdup_printf("its client can no longer be asked (%s): the download's record, or its client, is "
			"gone from Axis", failure->message);
	else if (refused)
		cause = g_strdup_printf("its client did not confirm that it let go of the torrent (it refused the removal, "
			"without its files, or still has the torrent after it): %s", failure->message);
	else
		cause = g_strdup_printf("its client could not be asked (%s)", failure->message);
	if (!lasting && run->attempts + 1 < AXIS_IMPORT_PLACE_ATTEMPTS) {
		text = g_strdup_printf("Its files are moved into the library only once its client has let go of the torrent, "
			"and %s: nothing was moved, and the import tries again (%d of %d tries).", cause, run->attempts + 1,
			AXIS_IMPORT_PLACE_ATTEMPTS);
		return wait_for(run, "client_release", text, refused ? 600 : 60, TRUE, error);
	}
	if (lasting)
		text = g_strdup_printf("Its files are moved into the library only once its client has let go of the torrent, "
			"and %s. Nothing was moved. If the torrent is not at its client any more, import anyway; if it is, remove "
			"it there (keeping its files), then retry.", cause);
	else
		text = g_strdup_printf("Its files are moved into the library only once its client has let go of the torrent, "
			"and %s, %d times. Nothing was moved. If the torrent is not at its client any more, import anyway; if it "
			"is, remove it there (keeping its files), then retry.", cause, AXIS_IMPORT_PLACE_ATTEMPTS);
	return block_one(run, "client_release", TRUE, text, error);
}

static gboolean
release_torrent(
	Run       *run,
	gboolean  *go,
	GError   **error
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	const gchar *params[3];
	gboolean was_gone = FALSE;

	*go = FALSE;
	if (!mode_moves(run->mode) || !run->protocol_known || run->protocol != AXIS_DOWNLOAD_TORRENT) {
		*go = TRUE;
		return TRUE;
	}
	params[0] = id;
	if (run->released == NULL) {
		g_autoptr(AxisDownloadInfo) info = NULL;
		g_autofree gchar *client = NULL;
		const gchar *more[3];

		/* When, and which client and torrent: recorded before it asks. */
		if (run->download_id > 0 && axis_downloads_get(run->self->downloads, run->download_id, &info, NULL) &&
		    info->client_id > 0 && info->client_ref != NULL) {
			client = axis_import_int_text(info->client_id);
			run->release_client_id = info->client_id;
			g_free(run->release_ref);
			run->release_ref = g_strdup(info->client_ref);
		}
		more[0] = id;
		more[1] = client;
		more[2] = client != NULL ? info->client_ref : NULL;
		if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET release_asked_at = coalesce(release_asked_at, now()),"
		        " release_client_id = coalesce($2::bigint, release_client_id), release_ref = coalesce($3, release_ref)"
		        " WHERE id = $1::bigint", 3, more, NULL, error))
			return FALSE;
	}
	if (!step(run, "release", 0, error))
		return FALSE;
	if (!axis_downloads_release(run->self->downloads, run->download_id, &was_gone, &local)) {
		if (local->domain == AXIS_DB_ERROR) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		/* In another category or label now: a person's, left alone. */
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS))
			return not_ours(run, local->message, go, error);
		if (run->released != NULL)
			return release_again_failed(run, local, go, error);
		return release_failed(run, local, go, error);
	}
	/* Let go of before and still gone: what was recorded stands. */
	if (run->released != NULL && was_gone) {
		*go = TRUE;
		return TRUE;
	}
	if (run->released != NULL) {
		params[1] = "done";
		params[2] = "The torrent was removed from its client without its files before they were moved into the "
			"library; it had been added to its client again after Axis's first release of it, and was removed again, "
			"without its files, the client saying it no longer has it.";
	} else if (was_gone && run->asked_before) {
		params[1] = "gone";
		params[2] = "Axis had asked its client to let go of it before the import was interrupted, and the client "
			"now says it does not have the torrent (whether because Axis asked or because of something else cannot be "
			"told), so its client deleted none of its files; they were then moved into the library.";
	} else if (was_gone) {
		params[1] = "gone";
		params[2] = "The torrent was already gone from its client (something else removed it), so its client "
			"deleted none of its files; they were then moved into the library.";
	} else {
		params[1] = "done";
		params[2] = "The torrent was removed from its client without its files, the client saying it no longer has "
			"it, before its files were moved into the library.";
	}
	if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET client_removal = $2, client_removal_text = $3,"
	        " updated_at = now() WHERE id = $1::bigint", 3, params, NULL, error))
		return FALSE;
	g_free(run->released);
	run->released = g_strdup(params[1]);
	if (!step(run, "released", 0, error))
		return FALSE;
	*go = TRUE;
	return TRUE;
}

/* Hardlink or copy, and why; %TRUE with *@go %FALSE when it cannot be
 * placed now (blocked or waiting, recorded). */
static gboolean
choose_mode(
	Run       *run,
	gboolean  *go,
	GError   **error
){
	AxisImportFound *film = run->verdict.film;
	g_autoptr(AxisVolumeInfo) dest = axis_storage_get_volume(run->self->storage, run->root->volume_id);
	g_autoptr(AxisVolumeInfo) source = axis_storage_get_volume(run->self->storage, run->source_volume_id);
	/* (File by file there is no film: every file taken counts.) */
	guint64 need = film != NULL ? film->size : 0;
	struct statvfs vfs;
	guint i;

	g_autofree gchar *keep_why = NULL;
	gboolean moving;

	*go = FALSE;
	if (!moves(run, &moving, &keep_why, error))
		return FALSE;
	if (run->same_volume && moving) {
		run->mode = g_strdup("move");
		run->mode_text = g_strdup_printf("a move on %s: the download's file was renamed into the library, so nothing "
			"of it is left in the download's folder", dest != NULL ? dest->name : "its disk");
		*go = TRUE;
		return TRUE;
	}
	if (run->same_volume) {
		run->mode = g_strdup("hardlink");
		run->mode_text = g_strdup_printf("a hardlink on %s: the download's file is untouched and takes no more "
			"room%s%s%s", dest != NULL ? dest->name : "its disk", keep_why != NULL ? " (" : "",
			keep_why != NULL ? keep_why : "", keep_why != NULL ? ")" : "");
		*go = TRUE;
		return TRUE;
	}
	if (run->settings.hardlink_only) {
		g_autofree gchar *text = g_strdup_printf("The download is on %s and %s is on %s: a hardlink cannot cross "
			"disks, and \"Download and library on different disks\" (Settings, Imports) says to block, not copy.",
			source != NULL ? source->name : "another disk", run->root->name, dest != NULL ? dest->name : "another");

		return block_one(run, "hardlink_only", FALSE, text, error);
	}
	/* Room for everything that is copied, and the floor besides. */
	for (i = 0; i < run->found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(run->found, i);

		if (f != film && f->accepted && f->role != NULL)
			need += f->size;
	}
	if (fstatvfs(axis_volume_guard_get_fd(run->dest), &vfs) == 0) {
		guint64 available = (guint64)vfs.f_bavail * (guint64)vfs.f_frsize;

		if (available < need + (guint64)run->settings.free_floor) {
			g_autofree gchar *needed = axis_size_format(need);
			g_autofree gchar *has = axis_size_format(available);
			g_autofree gchar *floor = axis_size_format((guint64)run->settings.free_floor);
			g_autofree gchar *text = g_strdup_printf("%s has %s free; the copy needs %s, and %s more must stay "
				"free (\"Free space to keep when copying\", Settings, Imports).", dest != NULL ? dest->name :
				"The destination", has, needed, floor);

			return wait_for(run, "no_space", text, 600, FALSE, error);
		}
	}
	if (moving) {
		run->mode = g_strdup("move_copy");
		run->mode_text = g_strdup_printf("a move from %s to %s: a verified copy, and the download's file deleted once "
			"the import was recorded (different disks: a rename cannot cross them)", source != NULL ? source->name :
			"another disk", dest != NULL ? dest->name : "its disk");
		*go = TRUE;
		return TRUE;
	}
	run->mode = g_strdup("copy");
	run->mode_text = g_strdup_printf("a verified copy from %s to %s (different disks: a hardlink cannot cross them)%s%s%s",
		source != NULL ? source->name : "another disk", dest != NULL ? dest->name : "its disk",
		keep_why != NULL ? "; the download's file stays (" : "", keep_why != NULL ? keep_why : "",
		keep_why != NULL ? ")" : "");
	*go = TRUE;
	return TRUE;
}

static gboolean
name_film(
	Run             *run,
	AxisImportName  *name,
	GError         **error
){
	AxisImportFound *film = run->verdict.film;
	g_autofree gchar *ext = axis_import_extension(film->name, strlen(film->name));
	AxisImportNaming naming;
	const gchar *problem = NULL;

	memset(&naming, 0, sizeof(naming));
	naming.rename = run->settings.rename;
	naming.layout = run->settings.layout;
	naming.format = run->settings.format;
	naming.title = run->target->title;
	naming.year = run->target->year;
	naming.tmdb_id = run->target->tmdb_id;
	naming.imdb_id = run->target->imdb_id;
	naming.release = run->grab;
	naming.file = run->verdict.file_release;
	naming.file_name = film->name;
	naming.extension = ext;
	naming.quality = run->verdict.quality;
	naming.proper = run->verdict.proper;
	naming.video_codec = film->video_codec;
	naming.hdr = film->hdr;
	naming.audio_codec = film->audio_codec;
	naming.audio_profile = film->audio_profile;
	naming.audio_channels = film->audio_channels;
	if (!axis_import_names_for(&naming, name, &problem)) {
		g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "The file cannot be named: %s.",
			problem != NULL ? problem : "no reason");
		return FALSE;
	}
	return TRUE;
}

/* After a run that published the film and died: what it decided, again. */
static gboolean
reload_film(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autoptr(GString) name = NULL;
	const gchar *params[1];
	const gchar *quality;

	params[0] = id;
	if (!axis_db_pool_exec(run->self->pool, "SELECT f.id, f.dest_dir, f.dest_name, f.dest_inode, f.mode, i.mode_text,"
	        " i.quality, i.quality_text, i.upgrade FROM import_file f JOIN import i ON i.id = f.import_id"
	        " WHERE f.import_id = $1::bigint AND f.role = 'film' AND f.step IN ('published', 'swapped') LIMIT 1", 1,
	        params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0 ||
	    !axis_library_name_decode(axis_db_result_get_text(row, 0, 2), &name)) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "The movie's placed file is not "
			"recorded as it should be.");
		return FALSE;
	}
	axis_db_result_get_int64(row, 0, 0, &run->film_row);
	run->dest_dir = g_strdup(axis_db_result_get_text(row, 0, 1));
	run->film_name = g_strdup(name->str);
	{
		gint64 inode = 0;

		axis_db_result_get_int64(row, 0, 3, &inode);
		run->film_inode = (guint64)inode;
	}
	run->mode = g_strdup(axis_db_result_get_text(row, 0, 4));
	run->mode_text = g_strdup(axis_db_result_get_text(row, 0, 5));
	quality = axis_db_result_get_text(row, 0, 6);
	if (quality == NULL || !axis_quality_from_name(quality, &run->verdict.quality))
		run->verdict.quality = AXIS_QUALITY_UNKNOWN;
	run->verdict.quality_text = g_strdup(axis_db_result_get_text(row, 0, 7));
	run->film_upgrade = g_strcmp0(axis_db_result_get_text(row, 0, 8), "t") == 0;
	{
		g_autofree gchar *ext = axis_import_extension(run->film_name, strlen(run->film_name));

		run->stem = ext != NULL ? g_strndup(run->film_name, strlen(run->film_name) - strlen(ext)) :
			g_strdup(run->film_name);
	}
	run->film_fd = open_dest_dir(run, run->dest_dir, error);
	return run->film_fd >= 0;
}

/* A failure while placing: tried again a few times, then a person asks. */
static gboolean
placing_failed(
	Run          *run,
	GError       *failure,
	GError      **error
){
	g_autofree gchar *text = NULL;
	gboolean ok;

	if (g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
		text = g_strdup_printf("A file named as the movie's would be is already in its folder, and it is not the "
			"movie's file Axis knows: Axis does not replace it. Move it away, or import by hand. (%s)", failure->message);
		ok = block_one(run, "name_taken", FALSE, text, error);
	} else if (g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		text = g_strdup_printf("The new file has the old one's name, and its filesystem cannot swap two names at "
			"once, which an upgrade to the same name needs: %s", failure->message);
		ok = block_one(run, "cannot_swap", FALSE, text, error);
	} else if (failure->domain == AXIS_STORAGE_ERROR && (failure->code == AXIS_STORAGE_ERROR_UNAVAILABLE ||
	           failure->code == AXIS_STORAGE_ERROR_READ_ONLY)) {
		text = g_strdup_printf("A disk went away or became read-only while the file was placed (nothing was "
			"left): %s", failure->message);
		ok = wait_for(run, failure->code == AXIS_STORAGE_ERROR_READ_ONLY ? "volume_read_only" : "volume_offline", text,
			60, FALSE, error);
	} else if (run->attempts + 1 < AXIS_IMPORT_PLACE_ATTEMPTS) {
		text = g_strdup_printf("Placing the file failed and nothing was left: %s Axis tries again.", failure->message);
		ok = wait_for(run, "place_failed", text, PLACE_RETRY_S, TRUE, error);
	} else {
		text = g_strdup_printf("Placing the file failed %d times, and nothing was left: %s", run->attempts + 1,
			failure->message);
		ok = block_one(run, "place_failed", FALSE, text, error);
	}
	g_error_free(failure);
	return ok;
}

/* ── Imports file by file ───────────────────────────────────────── */

/*
 * An owner that answers match() (a series): every video file of the
 * download is imported for itself, with what the owner says it holds (its
 * episodes), into its own folder (the series' folder, the season's folder)
 * under its own name, decided against the files of its own episodes.  Each
 * goes through the very steps a film's file does (place_one(): the
 * temporary dot-name recorded first, the link or the verified copy, the
 * publish that never replaces, the swap for an upgrade to the same name),
 * is linked in its own transaction, retires the files it replaces, and
 * takes its own subtitles.
 *
 * A file that fails is noted on its row and the next is placed: what was
 * imported stays, recorded, and the failed file's episodes are left as they
 * were.  It is tried again with the import a few times (place_failed), then
 * the import ends 'partial' (partly imported), with the reason on the file
 * and "retry" to take it up again.  A volume that goes away or becomes
 * read-only stops the run (the import waits).  A run taken up again (after
 * a crash, a wait or a retry) keeps the rows an earlier run recorded:
 * recover() has settled their files on disk, a file linked already is done,
 * one published but not linked is linked now, and only the others are
 * placed: each file is imported once.
 */

static gboolean read_source(Run *run, gboolean *go, gboolean *tool_missing, GError **error);

/* The files of @plan's unit an earlier run of it may retire: with the file
 * assigned by hand, only those the person ticked for it (a file that is on
 * the episode now and was not ticked stays).  A new list; its files are
 * borrowed. */
static GPtrArray *
retirable(
	Run            *run,
	AxisImportPlan *plan
){
	GPtrArray *out = g_ptr_array_new();
	guint i;
	guint a;
	guint k;

	for (i = 0; plan->unit->existing != NULL && i < plan->unit->existing->len; i++) {
		gpointer e = g_ptr_array_index(plan->unit->existing, i);
		gint64 id = ((const AxisImportExisting *)e)->library_file_id;
		gboolean ticked = !plan->unit->by_hand;

		for (a = 0; !ticked && run->assigned != NULL && a < run->assigned->len; a++) {
			const AxisImportAssigned *hand = g_ptr_array_index(run->assigned, a);

			if (hand->position != plan->found->position || g_strcmp0(hand->path, plan->found->path) != 0)
				continue;
			for (k = 0; hand->replaces != NULL && k < hand->replaces->len; k++)
				ticked = ticked || g_array_index(hand->replaces, gint64, k) == id;
		}
		if (ticked)
			g_ptr_array_add(out, e);
	}
	return out;
}

static Placed *
placed_by_id(
	GPtrArray *placed,
	gint64     row
){
	guint i;

	for (i = 0; placed != NULL && i < placed->len; i++) {
		Placed *p = g_ptr_array_index(placed, i);

		if (p->id == row)
			return p;
	}
	return NULL;
}

static gboolean
note_row(
	Run          *run,
	gint64        row,
	const gchar  *text
){
	g_autofree gchar *id = axis_import_int_text(row);
	g_autofree gchar *cut = axis_import_text_cut(text, 1000);
	const gchar *params[2];

	params[0] = id;
	params[1] = cut;
	return axis_db_pool_exec(run->self->pool, "UPDATE import_file SET note = $2 WHERE id = $1::bigint", 2, params,
		NULL, NULL);
}

/* What the files are decided to be; %TRUE with *@go %FALSE when the import
 * waits or is blocked (recorded). */
static gboolean
decide_units(
	Run       *run,
	gboolean  *go,
	GError   **error
){
	g_autoptr(GPtrArray) placed = NULL;
	AxisImportExpect expect;
	gboolean tool_missing = FALSE;
	gboolean resume = FALSE;
	guint i;

	*go = FALSE;
	if (!read_source(run, go, &tool_missing, error))
		return FALSE;
	if (!*go)
		return TRUE;
	*go = FALSE;
	/* Taken up again: what an earlier run recorded stands. */
	placed = load_placed(run, error);
	if (placed == NULL)
		return FALSE;
	run->kept = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < placed->len; i++) {
		Placed *p = g_ptr_array_index(placed, i);

		if (p->step != NULL || p->library_file_id > 0)
			resume = TRUE;
		/* Kept is what a run really placed: with files assigned by hand, a
		 * file given the role and not placed is decided again, by its row. */
		if (g_strcmp0(p->role, "episode") == 0 && p->path != NULL && (run->assigned == NULL || p->library_file_id > 0))
			g_hash_table_add(run->kept, g_strdup(p->path));
	}
	if (!resume)
		g_hash_table_remove_all(run->kept);
	/* Taken up again after files were moved out of the download: those are
	 * taken from their rows, and every file keeps its first walk's place. */
	if (resume && !align_found(run, FALSE, error))
		return FALSE;
	memset(&expect, 0, sizeof(expect));
	expect.target = run->target;
	expect.owner = run->owner->info;
	expect.owner_data = run->owner->data;
	expect.settings = &run->settings;
	expect.grab = run->grab;
	expect.grab_size = -1;
	expect.protocol = run->protocol_known ? run->protocol : AXIS_DOWNLOAD_TORRENT;
	expect.chosen = -1;
	expect.overrides = run->overrides;
	expect.probe_tool_missing = tool_missing;
	expect.now_us = g_get_real_time();
	expect.kept = resume ? run->kept : NULL;
	expect.assigned = run->assigned;
	axis_import_decide_units(&expect, run->found, resume ? AXIS_IMPORT_PROBE_ATTEMPTS : run->attempts, &run->units);
	if (!resume) {
		if (waits_for_assignment(run))
			nothing_accepted(run);
		if (!store_found(run, error))
			return FALSE;
	} else {
		g_autofree gchar *id = axis_import_int_text(run->import_id);
		const gchar *params[1];

		/* The rows stand; what this run takes that they did not say is
		 * written onto them. */
		params[0] = id;
		{
			g_autoptr(AxisDbResult) all = NULL;

			if (!axis_db_pool_exec(run->self->pool, "SELECT id, path, role FROM import_file WHERE import_id = $1::bigint",
			        1, params, &all, error))
				return FALSE;
			for (i = 0; i < run->found->len; i++) {
				AxisImportFound *f = g_ptr_array_index(run->found, i);
				guint r;

				for (r = 0; r < axis_db_result_get_n_rows(all); r++) {
					if (g_strcmp0(axis_db_result_get_text(all, r, 1), f->path) != 0)
						continue;
					axis_db_result_get_int64(all, r, 0, &f->row_id);
					if (f->role != NULL && g_strcmp0(axis_db_result_get_text(all, r, 2), f->role) != 0) {
						g_autofree gchar *row = axis_import_int_text(f->row_id);
						g_autofree gchar *reasons = axis_import_reasons_to_json(f->reasons);
						const gchar *more[4];

						more[0] = row;
						more[1] = f->role;
						more[2] = f->accepted ? "true" : "false";
						more[3] = reasons;
						if (!axis_db_pool_exec(run->self->pool, "UPDATE import_file SET role = $2, accepted = $3::boolean,"
						        " reasons = $4::jsonb WHERE id = $1::bigint AND library_file_id IS NULL", 4, more, NULL,
						        error))
							return FALSE;
					}
				}
			}
		}
	}
	if (!step(run, "decided", 0, error))
		return FALSE;
	if (waits_for_assignment(run))
		return block_waiting(run, error);
	if (run->units.blocked)
		return block_with(run, run->units.code, run->units.summary, run->units.blockers, error);
	if (run->units.retry_probe)
		return wait_for(run, run->units.code, run->units.summary, (gint64)PROBE_RETRY_S * (run->attempts + 1), TRUE,
			error);
	*go = TRUE;
	return TRUE;
}

static gboolean
name_unit(
	Run             *run,
	AxisImportPlan  *plan,
	AxisImportName  *name,
	GError         **error
){
	AxisImportFound *f = plan->found;
	g_autofree gchar *ext = axis_import_extension(f->name, strlen(f->name));
	AxisImportNaming naming;
	const gchar *problem = NULL;

	memset(&naming, 0, sizeof(naming));
	naming.rename = run->settings.rename;
	naming.layout = run->settings.layout;
	naming.format = run->settings.format;
	naming.title = run->target->title;
	naming.year = run->target->year;
	naming.tmdb_id = run->target->tmdb_id;
	naming.imdb_id = run->target->imdb_id;
	/* The file's own fields name it (a pack's release name is the pack's). */
	naming.release = plan->fields;
	naming.file = plan->file_release;
	naming.file_name = f->name;
	naming.extension = ext;
	naming.quality = plan->quality;
	naming.proper = plan->proper;
	naming.video_codec = f->video_codec;
	naming.hdr = f->hdr;
	naming.audio_codec = f->audio_codec;
	naming.audio_profile = f->audio_profile;
	naming.audio_channels = f->audio_channels;
	naming.episode = TRUE;
	naming.tvdb_id = run->target->tvdb_id;
	naming.season = plan->unit->season;
	naming.episodes = plan->unit->episodes;
	naming.n_episodes = plan->unit->n_episodes;
	naming.episode_title = plan->unit->episode_title;
	naming.air_date = plan->unit->air_date;
	if (!axis_import_names_for(&naming, name, &problem)) {
		g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID, "The file cannot be named: %s.",
			problem != NULL ? problem : "no reason");
		return FALSE;
	}
	return TRUE;
}

/* The existing file an upgrade to the same name replaces, if one does. */
static const AxisImportExisting *
same_name_in(
	GPtrArray   *replace,
	gint64       root_id,
	const gchar *rel_stored
){
	guint i;

	for (i = 0; replace != NULL && i < replace->len; i++) {
		const AxisImportExisting *e = g_ptr_array_index(replace, i);

		if (e->root_id == root_id && g_strcmp0(e->rel_path, rel_stored) == 0)
			return e;
	}
	return NULL;
}

/* One file placed in the season's folder (made when needed); *@dir: that
 * folder, open; *@dest_dir: its stored form.  On failure the library is as
 * it was for this file. */
static gboolean
place_unit(
	Run                   *run,
	AxisImportPlan        *plan,
	const AxisImportName  *name,
	gint                  *dir,
	gchar                **dest_dir,
	guint64               *inode,
	GError               **error
){
	AxisImportFound *f = plan->found;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *recycle_name = NULL;
	const AxisImportExisting *same;
	gint series_fd = -1;
	gint season_fd = -1;
	gint64 recycle = 0;
	GError *local = NULL;

	*dir = -1;
	*dest_dir = NULL;
	*inode = 0;
	series_fd = folder(run, run->root_fd, name->folder, &local);
	if (series_fd < 0)
		goto fail;
	season_fd = folder(run, series_fd, name->season_folder, &local);
	close(series_fd);
	if (season_fd < 0)
		goto fail;
	{
		g_autofree gchar *series_stored = stored_of(name->folder);
		g_autofree gchar *season_stored = stored_of(name->season_folder);
		g_autofree gchar *file_stored = stored_of(name->file);

		*dest_dir = g_strconcat(series_stored, "/", season_stored, NULL);
		rel = g_strconcat(*dest_dir, "/", file_stored, NULL);
	}
	if (!step(run, "folder", f->row_id, &local))
		goto fail;
	same = same_name_in(plan->unit->existing, run->root->id, rel);
	if (same != NULL) {
		/* An upgrade to the same name: the old file gets its name in the
		 * recycle folder first, so that it is never without one. */
		g_auto(GStrv) where = recycle_where(run->root->rel_path, rel);
		g_autofree gchar *original = original_stored(run->root->rel_path, rel);
		AxisDbConn *conn;
		gboolean ok;

		if (where == NULL || !recycle_row(run, run->root->volume_id, original, same->inode, same->size, "linked",
		        &recycle, &local))
			goto fail;
		if (!axis_volume_write_retire(run->dest, season_fd, name->file, (guint64)same->inode,
		        (const gchar *const *)where, run->settings.dir_mode, AXIS_VOLUME_RETIRE_LINK, &recycle_name, &local)) {
			recycle_forget(run, recycle, NULL);
			goto fail;
		}
		conn = axis_db_pool_acquire(run->self->pool, &local);
		ok = conn != NULL;
		if (ok) {
			g_autofree gchar *path = recycle_stored(where, recycle_name);

			ok = recycle_kept(run, conn, recycle, path, &local);
			axis_db_pool_release(run->self->pool, conn);
		}
		if (!ok)
			goto fail;
		run->recycled++;
	}
	if (!place_one(run, f, f->row_id, season_fd, *dest_dir, name->file, same != NULL ? (guint64)same->inode : 0,
	        inode, &checksum, &local))
		goto fail;
	if (run->report != NULL)
		run->report->placed++;
	*dir = season_fd;
	return TRUE;

fail:
	if (season_fd >= 0)
		close(season_fd);
	g_clear_pointer(dest_dir, g_free);
	if (!run->crashed)
		undo_folders(run);
	g_propagate_error(error, local);
	return FALSE;
}

/* One placed file recorded: the index, the owner's links to its episodes,
 * the import's mode and folder. */
static gboolean
link_unit(
	Run             *run,
	AxisImportPlan  *plan,
	gint             dir,
	const gchar     *dest_dir,
	const gchar     *file_name,
	gint64          *library_id,
	GError         **error
){
	g_autofree gchar *file_stored = stored_of(file_name);
	g_autofree gchar *rel = g_strconcat(dest_dir, "/", file_stored, NULL);
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *row = axis_import_int_text(plan->found->row_id);
	g_autofree gchar *library = NULL;
	g_autofree gchar *root = axis_import_int_text(run->root->id);
	g_autofree gchar *volume = axis_import_int_text(run->root->volume_id);
	g_autofree gchar *series_dir = NULL;
	AxisImportLinked linked;
	const gchar *params[9];
	AxisDbConn *conn;
	gboolean ok;

	{
		const gchar *slash = strchr(dest_dir, '/');

		series_dir = slash != NULL ? g_strndup(dest_dir, (gsize)(slash - dest_dir)) : g_strdup(dest_dir);
	}
	*library_id = 0;
	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && index_file(run, conn, dir, file_name, rel, library_id, error);
	if (ok) {
		library = axis_import_int_text(*library_id);
		memset(&linked, 0, sizeof(linked));
		linked.import_id = run->import_id;
		linked.library_file_id = *library_id;
		linked.quality = plan->quality;
		linked.quality_text = plan->quality_text;
		linked.release_name = run->target->grab_release != NULL ? run->target->grab_release : run->release_name;
		linked.mode = run->mode;
		linked.upgrade = plan->upgrade;
		linked.user = run->override_by != NULL ? run->override_by : run->requested_by;
		linked.unit = plan->unit;
		/* Assigned by hand: the link is that person's, and kept. */
		if (plan->assigned) {
			linked.hand_how = "assigned";
			linked.hand_by = run->assigned_by;
		}
		ok = run->owner->info->link(run->owner->data, conn, run->target, &linked, error);
	}
	params[0] = id;
	params[1] = run->mode;
	params[2] = run->mode_text;
	params[3] = axis_quality_name(plan->quality);
	params[4] = plan->quality_text;
	params[5] = plan->upgrade ? "true" : "false";
	params[6] = series_dir;
	params[7] = root;
	params[8] = volume;
	/* The import's own quality is its first file's; upgrade when any is. */
	ok = ok && axis_db_conn_exec(conn, "UPDATE import SET mode = $2, mode_text = $3, quality = coalesce(quality, $4),"
		" quality_text = coalesce(quality_text, $5), upgrade = upgrade OR $6::boolean, dest_dir = $7,"
		" root_id = $8::bigint, dest_volume_id = $9::bigint, updated_at = now() WHERE id = $1::bigint", 9, params, NULL,
		error);
	params[0] = row;
	params[1] = library;
	ok = ok && axis_db_conn_exec(conn, "UPDATE import_file SET library_file_id = $2::bigint WHERE id = $1::bigint", 2,
		params, NULL, error);
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	if (!ok)
		return FALSE;
	{
		g_autoptr(GError) local = NULL;

		if (!axis_probe_request_file(run->self->probe, *library_id, NULL, &local))
			g_debug("import %" G_GINT64_FORMAT ": the probe of the new file was not queued: %s", run->import_id,
				local->message);
	}
	return step(run, "linked", plan->found->row_id, error);
}

/* The subtitles of one placed file, beside it, each by itself. */
static gboolean
place_unit_subtitles(
	Run             *run,
	AxisImportPlan  *plan,
	gint             dir,
	const gchar     *dest_dir,
	const gchar     *file_name,
	GPtrArray       *placed,
	GError         **error
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *ext = axis_import_extension(file_name, strlen(file_name));
	g_autofree gchar *stem = ext != NULL ? g_strndup(file_name, strlen(file_name) - strlen(ext)) :
		g_strdup(file_name);
	guint i;

	g_ptr_array_add(names, g_strdup(file_name));
	for (i = 0; i < run->found->len; i++) {
		AxisImportFound *f = g_ptr_array_index(run->found, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *checksum = NULL;
		AxisImportSubtitle subtitle;
		Placed *p;
		guint number = 0;
		guint64 inode = 0;
		gboolean was_stranded;
		gint copy;
		GError *local = NULL;

		if (g_strcmp0(f->role, "subtitle") != 0 || !f->accepted || f->unit_of != plan->found->position ||
		    f->row_id <= 0)
			continue;
		p = placed_by_id(placed, f->row_id);
		if (p != NULL && (g_strcmp0(p->step, "published") == 0 || p->library_file_id > 0))
			continue;
		axis_import_subtitle_read(f->name, strlen(f->name), &subtitle);
		do {
			g_free(name);
			name = axis_import_subtitle_name(stem, &subtitle, number);
			number = number == 0 ? 2 : number + 1;
		} while (name_taken(names, name) && number < 100);
		g_ptr_array_add(names, g_strdup(name));
		copy = fcntl(dir, F_DUPFD_CLOEXEC, 0);
		if (copy < 0)
			continue;
		was_stranded = run->stranded;
		if (!place_one(run, f, f->row_id, copy, dest_dir, name, 0, &inode, &checksum, &local)) {
			close(copy);
			if (run->crashed || (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_UNAVAILABLE ||
			    local->code == AXIS_STORAGE_ERROR_READ_ONLY))) {
				g_propagate_error(error, local);
				return FALSE;
			}
			/* Kept under its temporary name: noted, and the import stops for
			 * it (run_units()). */
			if (!was_stranded && run->stranded) {
				g_error_free(local);
				continue;
			}
			note(run, "%s was not placed: %s", f->name, local->message);
			note_row(run, f->row_id, local->message);
			g_error_free(local);
			continue;
		}
		if (run->report != NULL)
			run->report->placed++;
		{
			g_autofree gchar *file_stored = stored_of(name);
			g_autofree gchar *rel = g_strconcat(dest_dir, "/", file_stored, NULL);
			g_autofree gchar *row = axis_import_int_text(f->row_id);
			g_autofree gchar *library = NULL;
			AxisDbConn *conn = axis_db_pool_acquire(run->self->pool, &local);
			gint64 file_id = 0;
			gboolean ok = conn != NULL && index_file(run, conn, copy, name, rel, &file_id, &local);

			if (ok) {
				const gchar *params[2];

				library = axis_import_int_text(file_id);
				params[0] = row;
				params[1] = library;
				ok = axis_db_conn_exec(conn, "UPDATE import_file SET library_file_id = $2::bigint WHERE id = $1::bigint",
					2, params, NULL, &local);
			}
			if (conn != NULL)
				axis_db_pool_release(run->self->pool, conn);
			if (!ok) {
				note(run, "%s was placed but not indexed: %s", name, local != NULL ? local->message : "?");
				g_clear_error(&local);
			}
		}
		close(copy);
	}
	return TRUE;
}

/* The import's end, file by file: every file it was to import is in the
 * library (imported), or not (partly imported, with the reasons). */
static gboolean
finish_units(
	Run          *run,
	guint         done,
	const gchar  *failures,
	GError      **error
){
	g_autoptr(AxisDbResult) finished = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *retention = axis_import_int_text(run->settings.retention_s);
	g_autofree gchar *text = NULL;
	gboolean complete = failures == NULL || failures[0] == '\0';
	guint total = run->units.plans->len;
	/* Files a person left alone when they assigned the others by hand stay
	 * where they are: the download is not removed from its client, and its
	 * folder is not deleted. */
	gboolean leaves = leaves_files(run);
	const gchar *removal = run->not_ours ? "kept" : run->released != NULL ? run->released : complete &&
		run->download_id > 0 && run->protocol_known ? (!leaves && (run->protocol == AXIS_DOWNLOAD_USENET ||
		run->settings.torrents_remove) ? "pending" : "kept") : NULL;
	/* What SABnzbd keeps of a Usenet job after removing it, and what a torrent
	 * whose files were moved left (axis-import-folder.c); never a torrent that
	 * is not Axis's any more. */
	const gchar *folder = !leaves && !run->not_ours && complete && run->download_id > 0 && run->protocol_known &&
		(run->protocol == AXIS_DOWNLOAD_USENET || (run->released != NULL && mode_moves(run->mode))) ? "pending" : NULL;
	const gchar *params[7];
	gboolean marked = FALSE;
	AxisDbConn *conn;
	gboolean ok;

	if (complete)
		text = g_strdup_printf("Imported %u %s into %s, by %s.%s%s%s%s", total, total == 1 ? "file" : "files",
			run->root->name, run->mode_text != NULL ? run->mode_text : run->mode, run->recycled > 0 ?
			" The files they replaced are in the recycle folder of their disk." : "",
			run->units.left == 0 ? "" : run->assigned != NULL ? " The files were assigned by hand; those that were not "
			"assigned were left in the download." : " Files that hold no wanted episode were left in the download, each "
			"with its reason.", run->notes->len > 0 ? " " : "", run->notes->str);
	else
		text = g_strdup_printf("Partly imported: %u of %u files are in the library; %s Retry the import once that is "
			"put right.%s%s", done, total, failures, run->notes->len > 0 ? " " : "", run->notes->str);
	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	params[1] = complete ? "imported" : "partial";
	params[2] = removal;
	params[3] = complete ? (run->notes->len > 0 ? run->notes->str : NULL) : text;
	params[4] = complete ? NULL : "partly_imported";
	params[5] = folder;
	params[6] = run->released != NULL || run->not_ours ? "true" : "false";
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "UPDATE import SET state = $2, finished_at = now(), updated_at = now(), reason = $5,"
			" reason_text = left($4, 2000), blockers = '[]', retry_after = NULL,"
			" client_removal = CASE WHEN $7::boolean THEN client_removal ELSE $3 END,"
			" folder_removal = $6, folder_removal_text = NULL"
			" WHERE id = $1::bigint AND state = 'running' RETURNING id", 7, params, &finished, error);
	if (ok && axis_db_result_get_n_rows(finished) == 0)
		ok = not_running(run, error);
	if (ok) {
		const gchar *more[2];

		more[0] = id;
		more[1] = retention;
		ok = axis_db_conn_exec(conn, "UPDATE import_recycle SET remove_after = now() + $2::integer * interval"
			" '1 second' WHERE import_id = $1::bigint AND state = 'kept' AND remove_after IS NULL", 2, more, NULL,
			error);
	}
	if (ok && axis_db_result_get_n_rows(finished) > 0) {
		/* Only a download whose every wanted file is in the library is done
		 * with (and, Usenet, removed from its client). */
		if (complete && run->download_id > 0)
			ok = axis_downloads_mark_imported(conn, run->download_id, &marked, error);
		if (ok && run->owner->info->note != NULL)
			ok = run->owner->info->note(run->owner->data, conn, run->target, complete ? "imported" : "import_blocked",
				text, NULL, error);
		if (ok && run->owner->info->finished != NULL)
			ok = run->owner->info->finished(run->owner->data, conn, run->target, complete, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	if (!ok)
		return FALSE;
	if (marked)
		axis_downloads_notify_changed(run->self->downloads, run->download_id);
	if (done > 0)
		notify_servers(run);
	if (run->report != NULL) {
		run->report->state = complete ? "imported" : "partial";
		run->report->reason = complete ? NULL : "partly_imported";
	}
	return step(run, "recorded", 0, error);
}

/* The file-by-file import, from the decision on (the gate passed, the
 * guards taken, recover() done). */
static gboolean
run_units(
	Run     *run,
	GError **error
){
	g_autoptr(GPtrArray) placed = NULL;
	g_autoptr(GString) failures = g_string_new(NULL);
	gboolean go = FALSE;
	guint done = 0;
	guint failed = 0;
	guint i;
	GError *local = NULL;

	if (!decide_units(run, &go, error))
		return FALSE;
	if (!go)
		return TRUE;
	if (!choose_mode(run, &go, error))
		return FALSE;
	if (!go)
		return TRUE;
	if (!release_torrent(run, &go, error))
		return FALSE;
	if (!go)
		return TRUE;
	placed = load_placed(run, error);
	if (placed == NULL)
		return FALSE;
	close(run->root_fd);
	run->root_fd = axis_volume_guard_open_dir(run->dest, run->root->rel_path, error);
	if (run->root_fd < 0)
		return FALSE;
	for (i = 0; i < run->units.plans->len; i++) {
		AxisImportPlan *plan = g_ptr_array_index(run->units.plans, i);
		Placed *p = placed_by_id(placed, plan->found->row_id);
		g_autofree gchar *dest_dir = NULL;
		g_autofree gchar *file_name = NULL;
		gint64 library_id = 0;
		guint64 inode = 0;
		gint dir = -1;

		if (cancelled(run, error))
			return FALSE;
		if (plan->found->row_id <= 0) {
			note(run, "%s appeared after the import began and was left.", plan->found->name);
			continue;
		}
		if (p != NULL && p->library_file_id > 0 && p->dest_dir != NULL && p->dest_name != NULL) {
			/* Done by an earlier run: what it replaced is retired, its
			 * subtitles placed, if that run did not get so far. */
			g_autoptr(GString) name = NULL;
			g_autoptr(GPtrArray) retire = retirable(run, plan);

			dir = open_dest_dir(run, p->dest_dir, &local);
			if (dir >= 0 && axis_library_name_decode(p->dest_name, &name)) {
				if (!retire_list(run, retire, p->library_file_id, error) ||
				    !place_unit_subtitles(run, plan, dir, p->dest_dir, name->str, placed, error)) {
					close(dir);
					return FALSE;
				}
			}
			if (dir >= 0)
				close(dir);
			g_clear_error(&local);
			done++;
			continue;
		}
		if (p != NULL && g_strcmp0(p->step, "published") == 0 && p->dest_dir != NULL && p->dest_name != NULL) {
			/* Published by an earlier run that died before linking it. */
			g_autoptr(GString) name = NULL;

			dir = open_dest_dir(run, p->dest_dir, &local);
			if (dir < 0 || !axis_library_name_decode(p->dest_name, &name)) {
				if (dir >= 0)
					close(dir);
				g_propagate_error(error, local != NULL ? local : g_error_new_literal(AXIS_IMPORT_ERROR,
					AXIS_IMPORT_ERROR_INVALID, "A placed file is not recorded as it should be."));
				return FALSE;
			}
			dest_dir = g_strdup(p->dest_dir);
			file_name = g_strdup(name->str);
		} else {
			AxisImportName name;

			memset(&name, 0, sizeof(name));
			if (!name_unit(run, plan, &name, &local)) {
				note_row(run, plan->found->row_id, local->message);
				g_string_append_printf(failures, "%s%s: %s", failures->len > 0 ? " " : "", plan->found->name,
					local->message);
				g_clear_error(&local);
				failed++;
				continue;
			}
			if (!place_unit(run, plan, &name, &dir, &dest_dir, &inode, &local)) {
				axis_import_name_clear(&name);
				if (run->crashed || stopped_by_job(run, local)) {
					g_propagate_error(error, local);
					return FALSE;
				}
				if (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_UNAVAILABLE ||
				    local->code == AXIS_STORAGE_ERROR_READ_ONLY)) {
					/* The volume went away: the import waits; what is placed
					 * stays and the next run carries on. */
					return placing_failed(run, local, error);
				}
				{
					g_autofree gchar *why = g_error_matches(local, G_IO_ERROR, G_IO_ERROR_EXISTS) ?
						g_strdup_printf("a file named as this one would be is already in its folder, and it is not "
							"one Axis knows: Axis does not replace it (%s)", local->message) : g_strdup(local->message);

					note_row(run, plan->found->row_id, why);
					g_string_append_printf(failures, "%s%s (%s) was not placed: %s", failures->len > 0 ? " " : "",
						plan->found->name, plan->unit->label, why);
				}
				g_clear_error(&local);
				failed++;
				continue;
			}
			file_name = g_strdup(name.file);
			axis_import_name_clear(&name);
		}
		/* In place: the rest only adds to the library. */
		if (!link_unit(run, plan, dir, dest_dir, file_name, &library_id, error) ||
		    (plan->upgrade && !retire_list(run, plan->unit->existing, library_id, error)) ||
		    !place_unit_subtitles(run, plan, dir, dest_dir, file_name, placed, error)) {
			close(dir);
			return FALSE;
		}
		close(dir);
		note_row(run, plan->found->row_id, NULL);
		done++;
	}
	/* A file kept under its temporary name (it could be neither given its
	 * name nor put back): a person looks, the import naming it; a retry
	 * carries on from what is recorded. */
	if (run->stranded)
		return block_one(run, STRANDED_CODE(run), FALSE, run->notes->str, error);
	if (failed > 0 && run->attempts + 1 < AXIS_IMPORT_PLACE_ATTEMPTS) {
		g_autofree gchar *text = g_strdup_printf("%u of %u files are in the library; %u could not be placed and are "
			"tried again: %s", done, run->units.plans->len, failed, failures->str);

		return wait_for(run, "place_failed", text, PLACE_RETRY_S, TRUE, error);
	}
	return finish_units(run, done, failures->str, error);
}

/* ── The run ────────────────────────────────────────────────────── */

static gboolean
set_owner(
	Run     *run,
	gboolean *go,
	GError  **error
){
	g_autoptr(AxisDbResult) other = NULL;
	g_autofree gchar *id = axis_import_int_text(run->import_id);
	g_autofree gchar *owner = axis_import_int_text(run->target->owner_id);
	g_autofree gchar *grab = run->target->grab_id > 0 ? axis_import_int_text(run->target->grab_id) : NULL;
	g_autofree gchar *title = run->target->title_id > 0 ? axis_import_int_text(run->target->title_id) : NULL;
	g_autofree gchar *label = axis_import_text_cut(run->target->label != NULL ? run->target->label : "Untitled", 600);
	g_autofree gchar *root = run->target->root_id > 0 ? axis_import_int_text(run->target->root_id) : NULL;
	const gchar *params[6];

	*go = FALSE;
	params[0] = id;
	params[1] = run->module;
	params[2] = owner;
	/* A film's imports one at a time, from the first to the last; an owner
	 * that imports file by file (a series: each download its own episodes)
	 * only places one at a time, so that one blocked download does not hold
	 * up the others (0028_series.sql has the index for it). */
	if (!axis_db_pool_exec(run->self->pool, run->owner->info->match != NULL ?
	        "SELECT id FROM import WHERE module = $2 AND owner_id = $3::bigint AND state = 'running'"
	        " AND id <> $1::bigint" :
	        "SELECT id FROM import WHERE module = $2 AND owner_id = $3::bigint"
	        " AND state IN ('waiting', 'running', 'blocked') AND id <> $1::bigint", 3, params, &other, error))
		return FALSE;
	if (axis_db_result_get_n_rows(other) > 0)
		return wait_for(run, "busy", "Another import of the same title is open; this one waits for it.", 60, FALSE,
			error);
	params[1] = owner;
	params[2] = grab;
	params[3] = title;
	params[4] = label;
	params[5] = root;
	{
		g_autoptr(AxisDbResult) updated = NULL;
		GError *local = NULL;

		/* Only while it is still open as it was read: the hourly sweep may
		 * have ended it meanwhile (its files gone). */
		if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET owner_id = $2::bigint,"
		        " grab_id = coalesce($3::bigint, grab_id), title_id = coalesce($4::bigint, title_id), title_label = $5,"
		        " root_id = $6::bigint, state = 'running', updated_at = now() WHERE id = $1::bigint"
		        " AND state IN ('waiting', 'running') RETURNING id", 6, params, &updated, &local)) {
			/* File by file: another run began placing between the look and
			 * this (a film's run fails here as it always has). */
			if (run->owner->info->match != NULL && g_error_matches(local, AXIS_DB_ERROR,
			        AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
				g_error_free(local);
				return wait_for(run, "busy", "Another import of the same title is open; this one waits for it.", 60,
					FALSE, error);
			}
			g_propagate_error(error, local);
			return FALSE;
		}
		if (axis_db_result_get_n_rows(updated) == 0) {
			/* Ended meanwhile: nothing more is done. */
			if (run->report != NULL)
				run->report->state = "dismissed";
			return TRUE;
		}
	}
	run->owner_id = run->target->owner_id;
	*go = TRUE;
	return TRUE;
}

/* The walk and the probe of the source; %TRUE with *@go %FALSE when the
 * import waits or is blocked (recorded). */
static gboolean
read_source(
	Run       *run,
	gboolean  *go,
	gboolean  *tool_missing,
	GError   **error
){
	g_autofree gchar *problem = NULL;
	gboolean is_file = FALSE;
	GError *local = NULL;

	*go = FALSE;
	*tool_missing = FALSE;
	if (!axis_import_walk(run->source, run->source_path, &run->found, &is_file, &problem, &local)) {
		if (local->domain == AXIS_STORAGE_ERROR) {
			g_autofree gchar *text = g_strdup_printf("The download's disk went away while it was read: %s",
				local->message);

			g_error_free(local);
			return wait_for(run, "source_offline", text, 60, FALSE, error);
		}
		g_propagate_error(error, local);
		return FALSE;
	}
	if (problem != NULL) {
		gint absent = axis_import_source_absent(run->source, run->source_path, NULL);
		gint placed = absent == 1 ? axis_import_placed_count(run->self, run->import_id, error) : 0;

		if (placed < 0)
			return FALSE;
		/* Deleted or moved by something else before anything of it was
		 * imported: a final state, saying so, and its title wanted again
		 * (the follow-up closes its grab). */
		if (absent == 1 && placed == 0 && run->download_id > 0) {
			gboolean done = FALSE;

			if (!axis_import_files_gone(run->self, run->import_id, FALSE, &done, error))
				return FALSE;
			if (run->report != NULL) {
				run->report->state = "dismissed";
				run->report->reason = "files_gone";
			}
			return TRUE;
		}
		/* Everything of it the import took was moved out already (a run taken
		 * up again): the rest is in its rows. */
		if (absent == 1 && placed > 0) {
			run->found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
		} else {
			return block_one(run, "source_problem", FALSE, problem, error);
		}
	} else {
		/* Seen, and what the folder imported from is (for its removal after a
		 * move, when the files are not in it any more). */
		guint64 inode = 0;

		if (axis_import_source_absent(run->source, run->source_path, &inode) == 0 && inode > 0) {
			g_autofree gchar *id = axis_import_int_text(run->import_id);
			g_autofree gchar *ino = axis_import_int_text((gint64)inode);
			const gchar *params[2];

			params[0] = id;
			params[1] = ino;
			if (!axis_db_pool_exec(run->self->pool, "UPDATE import SET seen_at = now(),"
			        " source_inode = coalesce(source_inode, $2::bigint) WHERE id = $1::bigint", 2, params, NULL, error))
				return FALSE;
		}
	}
	if (!axis_import_probe_found(run->self->probe, run->source, run->source_path, run->found, &local)) {
		if (g_error_matches(local, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL)) {
			*tool_missing = TRUE;
			g_clear_error(&local);
		} else if (local->domain == AXIS_STORAGE_ERROR || g_error_matches(local, AXIS_PROBE_ERROR,
		           AXIS_PROBE_ERROR_UNAVAILABLE)) {
			g_autofree gchar *text = g_strdup_printf("The download could not be read: %s", local->message);

			g_error_free(local);
			return wait_for(run, "source_offline", text, 60, FALSE, error);
		} else {
			g_propagate_error(error, local);
			return FALSE;
		}
	}
	*go = TRUE;
	return TRUE;
}

static gboolean
decide(
	Run       *run,
	gboolean  *go,
	GError   **error
){
	AxisImportExpect expect;
	gboolean tool_missing = FALSE;

	*go = FALSE;
	if (!read_source(run, go, &tool_missing, error))
		return FALSE;
	if (!*go)
		return TRUE;
	*go = FALSE;
	memset(&expect, 0, sizeof(expect));
	expect.target = run->target;
	expect.owner = run->owner->info;
	expect.owner_data = run->owner->data;
	expect.settings = &run->settings;
	expect.grab = run->grab;
	expect.grab_size = -1;
	expect.protocol = run->protocol_known ? run->protocol : AXIS_DOWNLOAD_TORRENT;
	expect.chosen = run->chosen;
	expect.overrides = run->overrides;
	expect.probe_tool_missing = tool_missing;
	expect.now_us = g_get_real_time();
	expect.assigned = run->assigned;
	axis_import_decide(&expect, run->found, run->attempts, &run->verdict);
	if (waits_for_assignment(run))
		nothing_accepted(run);
	if (!store_found(run, error))
		return FALSE;
	if (!step(run, "decided", 0, error))
		return FALSE;
	if (waits_for_assignment(run))
		return block_waiting(run, error);
	if (run->verdict.blocked)
		return block_with(run, run->verdict.code, run->verdict.summary, run->verdict.blockers, error);
	if (run->verdict.retry_probe)
		return wait_for(run, run->verdict.code, run->verdict.summary, (gint64)PROBE_RETRY_S * (run->attempts + 1), TRUE,
			error);
	*go = TRUE;
	return TRUE;
}

gboolean
axis_import_run(
	AxisImport        *self,
	gint64             import_id,
	AxisJobContext    *context,
	AxisImportReport  *report,
	GError           **error
){
	Run run;
	g_autofree gchar *problem = NULL;
	gboolean film_published = FALSE;
	gboolean go = FALSE;
	gboolean ok = FALSE;
	GError *local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	memset(&run, 0, sizeof(run));
	run.self = self;
	run.context = context;
	run.report = report;
	run.import_id = import_id;
	run.root_fd = -1;
	run.film_fd = -1;
	run.made = g_array_new(FALSE, TRUE, sizeof(MadeDir));
	g_array_set_clear_func(run.made, made_dir_clear);
	run.notes = g_string_new(NULL);
	if (report != NULL)
		memset(report, 0, sizeof(*report));
	axis_import_settings_read(self, &run.settings);

	if (!load(&run, error))
		goto out;
	if (report != NULL)
		report->state = g_intern_string(run.state);
	/* Done, dismissed, or for a person to decide. */
	if (g_strcmp0(run.state, "waiting") != 0 && g_strcmp0(run.state, "running") != 0) {
		ok = TRUE;
		goto out;
	}

	/* Who it is for. */
	run.owner = axis_import_find_owner(self, run.module);
	if (run.owner == NULL) {
		ok = block_one(&run, "no_owner", FALSE, "Nothing in this version of Axis imports for what this download is "
			"for.", error);
		goto out;
	}
	{
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);

		if (conn == NULL)
			goto out;
		ok = run.owner->info->resolve(run.owner->data, conn, run.download_id, run.owner_id, &run.target, &problem,
			error);
		axis_db_pool_release(self->pool, conn);
		if (!ok)
			goto out;
		/* From here on every way out says how it went. */
		ok = FALSE;
	}
	if (run.target == NULL) {
		ok = block_one(&run, "no_title", FALSE, problem != NULL ? problem : "The title this download was for is not "
			"managed any more.", error);
		goto out;
	}
	/* (One its owner holds although the title is known, a series whose
	 * numbering changed since the grab, goes on until its files are read and
	 * recorded, and is blocked then: see waits_for_assignment().) */
	if (run.target->grab_release != NULL)
		run.grab = axis_release_parse(run.target->grab_release);
	else if (run.release_name != NULL)
		run.grab = axis_release_parse(run.release_name);

	/* Nothing at all is written while storage is read-only. */
	if (axis_storage_is_read_only_mode(self->storage)) {
		ok = wait_for(&run, "read_only", "Read-only storage is on (Settings, Storage; storage.read_only), and Axis "
			"writes nothing to any disk while it is. Turn it off to let imports into the library.", 60, FALSE, error);
		goto out;
	}
	if (run.target->root_id <= 0 || !axis_storage_get_root(self->storage, run.target->root_id, &run.root, NULL)) {
		ok = block_one(&run, "no_root", FALSE, "The title has no destination root: choose one on its page.", error);
		goto out;
	}
	if (run.source_volume_id <= 0 || run.source_path == NULL) {
		ok = block_one(&run, "no_source", FALSE, "The download's place is not on a disk Axis knows.", error);
		goto out;
	}
	/* A download inside the root (or holding it) is never imported into
	 * it. */
	if (run.source_volume_id == run.root->volume_id &&
	    (axis_storage_path_contains(run.root->rel_path, run.source_path) ||
	     axis_storage_path_contains(run.source_path, run.root->rel_path))) {
		ok = block_one(&run, "inside_root", FALSE, "The download is inside the title's library root (or holds it): "
			"Axis does not import a folder of the library into itself.", error);
		goto out;
	}
	if (!set_owner(&run, &go, error))
		goto out;
	if (!go) {
		ok = TRUE;
		goto out;
	}
	if (!take_guards(&run, &go, error))
		goto out;
	if (!go) {
		ok = TRUE;
		goto out;
	}
	/* A Usenet job reported by its file: imported from the job's folder. */
	if (!widen_source(&run, error))
		goto out;
	run.root_fd = axis_volume_guard_open_dir(run.dest, run.root->rel_path, &local);
	if (run.root_fd < 0) {
		g_autofree gchar *text = g_strdup_printf("The title's root cannot be opened: %s", local->message);

		g_error_free(local);
		ok = block_one(&run, "no_root", FALSE, text, error);
		goto out;
	}
	if (!recover(&run, &film_published, error))
		goto out;
	/* A file an earlier run moved could be neither put back nor given its
	 * name: a person looks (its row keeps it recorded). */
	if (run.stranded) {
		/* Only a folder or a name that cannot be read now (an I/O error,
		 * access refused, no descriptor left): looked at again a few times
		 * first. */
		if (!run.strand_lasting && run.attempts + 1 < AXIS_IMPORT_PLACE_ATTEMPTS) {
			g_autofree gchar *text = g_strdup_printf("%s Axis looks again in a minute (%d of %d tries).", run.notes->str,
				run.attempts + 1, AXIS_IMPORT_PLACE_ATTEMPTS);

			ok = wait_for(&run, "file_unreadable", text, PLACE_RETRY_S, TRUE, error);
			goto out;
		}
		ok = block_one(&run, STRANDED_CODE(&run), FALSE, run.notes->str, error);
		goto out;
	}

	/* File by file (a series): its own path from here on. */
	if (run.owner->info->match != NULL) {
		if (!run_units(&run, error))
			goto out;
		if (report == NULL || g_strcmp0(report->state, "imported") == 0) {
			AxisImportCleanupReport removals;

			memset(&removals, 0, sizeof(removals));
			if (run.download_id > 0 && !axis_import_client_removals(self, import_id, &removals, &local)) {
				g_debug("import %" G_GINT64_FORMAT ": the client removal is left for the cleanup: %s", import_id,
					local->message);
				g_clear_error(&local);
			}
		}
		if (run.settings.retention_s == 0 && run.recycled > 0 && !axis_import_cleanup_due(self, import_id, &local)) {
			g_debug("import %" G_GINT64_FORMAT ": the recycled files are left for the cleanup: %s", import_id,
				local->message);
			g_clear_error(&local);
		}
		ok = TRUE;
		goto out;
	}

	if (film_published) {
		GError *reload = NULL;

		if (!reload_film(&run, &reload)) {
			g_autofree gchar *text = NULL;

			/* Its folder in the library cannot be opened now (renamed, or
			 * unreadable): a person looks; nothing is forgotten. */
			if (reload->domain == AXIS_DB_ERROR || reload->domain == AXIS_IMPORT_ERROR || storage_gone(reload) ||
			    run.dest_dir == NULL) {
				g_propagate_error(error, reload);
				goto out;
			}
			{
				g_autofree gchar *shown = axis_library_name_display(run.dest_dir);

				text = g_strdup_printf("The movie's file was given its name by an earlier run of this import, in %s's "
					"folder %s, and that folder cannot be opened now (%s). Put it back if it was moved, then retry the "
					"import; or dismiss it (what is in the library stays).", run.root->name, shown, reload->message);
			}
			/* One that cannot be read now (an I/O error, access refused):
			 * looked at again a few times first. */
			if (error_transient(reload) && run.attempts + 1 < AXIS_IMPORT_PLACE_ATTEMPTS) {
				g_autofree gchar *more = g_strdup_printf("%s Axis looks again in a minute (%d of %d tries).", text,
					run.attempts + 1, AXIS_IMPORT_PLACE_ATTEMPTS);

				g_error_free(reload);
				ok = wait_for(&run, "file_unreadable", more, PLACE_RETRY_S, TRUE, error);
				goto out;
			}
			g_error_free(reload);
			ok = block_one(&run, "film_folder_gone", FALSE, text, error);
			goto out;
		}
		/* What it decided about the rest, again (the walk is read only).
		 * After a move the download no longer holds what was moved: those
		 * files are taken from their rows, and the decisions recorded
		 * stand (align_found()). */
		{
			g_autofree gchar *walk_problem = NULL;
			gboolean is_file = FALSE;
			gboolean moved = mode_moves(run.mode);
			AxisImportExpect expect;

			if (!axis_import_walk(run.source, run.source_path, &run.found, &is_file, &walk_problem, NULL) ||
			    run.found == NULL) {
				g_clear_pointer(&run.found, g_ptr_array_unref);
				run.found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
			}
			if (moved && !align_found(&run, FALSE, error))
				goto out;
			if (run.found->len > 0) {
				AxisQuality quality = run.verdict.quality;
				g_autofree gchar *quality_text = g_steal_pointer(&run.verdict.quality_text);

				memset(&expect, 0, sizeof(expect));
				expect.target = run.target;
				expect.owner = run.owner->info;
				expect.owner_data = run.owner->data;
				expect.settings = &run.settings;
				expect.grab = run.grab;
				expect.protocol = run.protocol_known ? run.protocol : AXIS_DOWNLOAD_TORRENT;
				expect.chosen = run.chosen;
				expect.overrides = run.overrides;
				expect.assigned = run.assigned;
				expect.probe_tool_missing = TRUE;
				axis_import_verdict_clear(&run.verdict);
				axis_import_decide(&expect, run.found, AXIS_IMPORT_PROBE_ATTEMPTS, &run.verdict);
				run.verdict.quality = quality;
				g_free(run.verdict.quality_text);
				run.verdict.quality_text = g_steal_pointer(&quality_text);
				if (moved && !align_found(&run, TRUE, error))
					goto out;
				/* Rows ids of the sidecars, by position. */
				{
					g_autoptr(GPtrArray) placed = load_placed(&run, NULL);
					guint i;
					guint k;

					for (i = 0; placed != NULL && i < placed->len; i++) {
						Placed *p = g_ptr_array_index(placed, i);

						for (k = 0; k < run.found->len; k++) {
							AxisImportFound *f = g_ptr_array_index(run.found, k);

							if (g_strcmp0(f->path, p->path) == 0)
								f->row_id = p->id;
						}
					}
				}
			} else {
				g_clear_pointer(&run.found, g_ptr_array_unref);
				run.found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
			}
			if (run.verdict.replace == NULL)
				run.verdict.replace = g_ptr_array_new();
			g_ptr_array_set_size(run.verdict.replace, 0);
			{
				guint i;

				/* What it replaces: the title's files, but not the new one
				 * (linked already when the earlier run died after that). */
				for (i = 0; i < run.target->existing->len; i++) {
					AxisImportExisting *e = g_ptr_array_index(run.target->existing, i);

					if ((guint64)e->inode != run.film_inode)
						g_ptr_array_add(run.verdict.replace, e);
				}
			}
			run.verdict.upgrade = run.film_upgrade || run.verdict.replace->len > 0;
		}
	} else {
		AxisImportName name;

		if (!decide(&run, &go, error))
			goto out;
		if (!go) {
			ok = TRUE;
			goto out;
		}
		if (!choose_mode(&run, &go, error))
			goto out;
		if (!go) {
			ok = TRUE;
			goto out;
		}
		memset(&name, 0, sizeof(name));
		if (!name_film(&run, &name, &local)) {
			ok = block_one(&run, "bad_name", FALSE, local->message, error);
			g_error_free(local);
			goto out;
		}
		/* A torrent's client lets go of it before anything of it is
		 * moved. */
		if (!release_torrent(&run, &go, error)) {
			axis_import_name_clear(&name);
			goto out;
		}
		if (!go) {
			axis_import_name_clear(&name);
			ok = TRUE;
			goto out;
		}
		close(run.root_fd);
		run.root_fd = -1;
		if (!place_film(&run, &name, &local)) {
			axis_import_name_clear(&name);
			if (run.crashed || stopped_by_job(&run, local)) {
				g_propagate_error(error, local);
				goto out;
			}
			/* The film kept under its temporary name: a person looks. */
			if (run.stranded) {
				g_error_free(local);
				ok = block_one(&run, STRANDED_CODE(&run), FALSE, run.notes->str, error);
				goto out;
			}
			ok = placing_failed(&run, local, error);
			goto out;
		}
		axis_import_name_clear(&name);
	}
	if (cancelled(&run, error))
		goto out;
	/* The film's file is in place: the rest only adds to the library. */
	if (!link_film(&run, error))
		goto out;
	/* An upgrade retires what it replaces (with their subtitles), then the
	 * new file's subtitles and extras are placed. */
	if (run.verdict.upgrade && !retire_old(&run, error))
		goto out;
	/* A run taken up again after the film was given its name: the torrent is
	 * asked again (it may be at its client again) before the sidecars are
	 * moved, as before any file is. */
	if (film_published) {
		if (!release_torrent(&run, &go, error))
			goto out;
		if (!go) {
			ok = TRUE;
			goto out;
		}
	}
	if (!place_sidecars(&run, error))
		goto out;
	/* A sidecar kept under its temporary name: not imported silently; the
	 * import stops for a person, naming it, and a retry places it. */
	if (run.stranded) {
		ok = block_one(&run, STRANDED_CODE(&run), FALSE, run.notes->str, error);
		goto out;
	}
	if (!finish(&run, error))
		goto out;
	/* After: a Usenet download goes from its client; with no retention the
	 * recycled files go at once. */
	if (run.download_id > 0) {
		AxisImportCleanupReport removals;

		memset(&removals, 0, sizeof(removals));
		if (!axis_import_client_removals(self, import_id, &removals, &local)) {
			g_debug("import %" G_GINT64_FORMAT ": the client removal is left for the cleanup: %s", import_id,
				local->message);
			g_clear_error(&local);
		}
	}
	if (run.settings.retention_s == 0 && run.recycled > 0 && !axis_import_cleanup_due(self, import_id, &local)) {
		g_debug("import %" G_GINT64_FORMAT ": the recycled files are left for the cleanup: %s", import_id,
			local->message);
		g_clear_error(&local);
	}
	ok = TRUE;

out:
	if (!ok && !run.crashed && error != NULL && *error != NULL && run.state != NULL &&
	    g_strcmp0(run.state, "imported") != 0)
		undo_folders(&run);
	run_clear(&run);
	return ok;
}
