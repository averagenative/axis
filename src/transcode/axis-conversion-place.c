/*
 * axis-conversion-place.c - the finish job: the checks, then the trial's
 * kept file or the replacement of the original, and the recovery of a
 * placement in flight
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Placement is one loop that looks at the disk, decides from what it finds
 * and the conversion's record what the next step is, takes it through the
 * volume write layer, records it, and looks again: the first run and a
 * run after a crash at any point are the same code.  The rules it keeps:
 *
 *  - The library name holds the original or the complete verified new file
 *    at every instant: the new file reaches the original's folder under a
 *    temporary name, then either exchanges names with the original (which
 *    has a second name in the recycle folder first) or is published under
 *    its new name before the original is retired.
 *  - The new file is removed only while the original is verified under its
 *    library name; the recycled original is removed only once the new file
 *    is under the library name and the conversion is recorded done.
 *  - What the loop does not recognise (the library name holding neither
 *    file, an original that is gone) stops it with everything left where it
 *    is, for a person.
 *  - It holds the shared form of the scan's volume lock, so that no scan of
 *    the volume runs while the library changes and is recorded.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-conversion-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "library/axis-library.h"
#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-write.h"

#define DIR_MODE          0700
#define SCAN_WAIT_US      (G_GINT64_CONSTANT(600) * G_USEC_PER_SEC)
#define SCAN_RETRY_US     (G_GINT64_CONSTANT(2) * G_USEC_PER_SEC)
#define MAX_STEPS         12

/* ── The work area ──────────────────────────────────────────────── */

gint
axis_conversion_work_area(
	AxisVolumeGuard  *guard,
	gboolean          make,
	GError          **error
){
	struct stat st;
	gint fd;

	if (make)
		fd = axis_volume_write_mkdir(guard, axis_volume_guard_get_fd(guard), AXIS_VOLUME_WORK_DIR, DIR_MODE, NULL, NULL,
			error);
	else {
		fd = openat(axis_volume_guard_get_fd(guard), AXIS_VOLUME_WORK_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
			O_NOCTTY | O_CLOEXEC);
		if (fd < 0) {
			gint err = errno;

			if (err == ELOOP || err == ENOTDIR)
				g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
					"Something that is not a folder is where the work area should be.");
			else
				g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "The work area cannot be opened: %s",
					g_strerror(err));
			return -1;
		}
	}
	if (fd < 0)
		return -1;
	/* The daemon's, and writable by nobody else but its group. */
	if (fstat(fd, &st) != 0 || (guint64)st.st_dev != axis_volume_guard_get_dev(guard) || st.st_uid != geteuid() ||
	    (st.st_mode & 0002) != 0) {
		close(fd);
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
			"The work area is not the daemon's own folder, or anyone may write in it.");
		return -1;
	}
	return fd;
}

/* ── Steps ──────────────────────────────────────────────────────── */

typedef struct
{
	AxisConversions    *self;
	AxisConversionRow  *row;
	AxisVolumeGuard    *guard;
	AxisDbConn         *conn;	/* holds the scan lock */
	gint                work;
	gint                dir;
	gchar              *folder;	/* the original's folder, from the volume's top */
	gchar              *name;	/* the original's name (bytes) */
	gchar              *new_name;	/* the new file's name (bytes) */
	gboolean            same;	/* the new file takes the original's name */
	gchar             **where;	/* its place in the recycle folder */
} Place;

static void
place_clear(Place *p)
{
	if (p->dir >= 0)
		close(p->dir);
	g_free(p->folder);
	g_free(p->name);
	g_free(p->new_name);
	g_strfreev(p->where);
}

/* A test's hook: CONTINUE, or FAIL (an error, nothing done), or CRASH
 * (stop as a daemon that died). */
static gboolean
hook(
	Place        *p,
	const gchar  *step,
	GError      **error
){
	switch (axis_conversion_hook(p->self, step, p->row->id)) {
	case AXIS_CONVERSION_HOOK_FAIL:
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "The step %s failed (a test).", step);
		return FALSE;
	case AXIS_CONVERSION_HOOK_CRASH:
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "Stopped at %s (a test).", step);
		return FALSE;
	default:
		return TRUE;
	}
}

/* Before a change: the test's hook.  (The write layer proves the volume
 * again at the start of every operation; the loop proves it before it looks
 * at the folder, so that what it decides is decided from the volume.) */
static gboolean
before(
	Place        *p,
	const gchar  *step,
	GError      **error
){
	return hook(p, step, error);
}

static gboolean
set_step(
	Place        *p,
	const gchar  *step,
	GError      **error
){
	g_autofree gchar *id = axis_conversion_int_text(p->row->id);
	const gchar *params[2];

	params[0] = id;
	params[1] = step;
	if (!axis_db_conn_exec(p->conn, "UPDATE conversion SET step = $2, updated_at = now() WHERE id = $1::bigint", 2,
	        params, NULL, error))
		return FALSE;
	g_free(p->row->step);
	p->row->step = g_strdup(step);
	return TRUE;
}

static gboolean
look(
	gint         dir,
	const gchar *name,
	struct stat *st
){
	if (dir < 0 || name == NULL || fstatat(dir, name, st, AT_SYMLINK_NOFOLLOW) != 0) {
		memset(st, 0, sizeof(*st));
		return FALSE;
	}
	return TRUE;
}

static gboolean
is_inode(
	const struct stat *st,
	gint64             ino
){
	return ino > 0 && st->st_ino != 0 && S_ISREG(st->st_mode) && (gint64)st->st_ino == ino;
}

/* The original as it was at the request (its times untouched since: before
 * it was linked). */
static gboolean
original_unchanged(
	const AxisConversionRow *r,
	const struct stat       *st,
	gboolean                 linked
){
	if (!is_inode(st, r->src_inode) || (gint64)st->st_size != r->src_size ||
	    (gint64)st->st_mtim.tv_sec != r->src_mtime_sec || (gint)st->st_mtim.tv_nsec != r->src_mtime_nsec)
		return FALSE;
	if (linked)
		return st->st_nlink == 2;
	return st->st_nlink == 1 && (gint64)st->st_ctim.tv_sec == r->src_ctime_sec &&
		(gint)st->st_ctim.tv_nsec == r->src_ctime_nsec;
}

/* The new file as it was settled (its change time too while it is still in
 * the work area: a rename changes that). */
static gboolean
output_unchanged(
	const AxisConversionRow *r,
	const struct stat       *st,
	gboolean                 moved
){
	if (!is_inode(st, r->output_inode) || (gint64)st->st_size != r->output_size ||
	    (gint64)st->st_mtim.tv_sec != r->output_mtime_sec || (gint)st->st_mtim.tv_nsec != r->output_mtime_nsec ||
	    st->st_nlink != 1)
		return FALSE;
	return moved || ((gint64)st->st_ctim.tv_sec == r->output_ctime_sec && (gint)st->st_ctim.tv_nsec == r->output_ctime_nsec);
}

/* The stored form of a place in the recycle folder. */
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

/* The recorded place in the recycle folder, as components; NULL when there
 * is none or it cannot be read. */
static gchar **
recycle_parts(const gchar *stored)
{
	g_autoptr(GString) bytes = NULL;

	if (stored == NULL || !axis_library_name_decode(stored, &bytes) || memchr(bytes->str, '\0', bytes->len) != NULL)
		return NULL;
	return g_strsplit(bytes->str, "/", 70);
}

/* Whether the recorded recycle place holds the original (inode). */
static gboolean
recycle_holds_original(Place *p)
{
	g_auto(GStrv) parts = recycle_parts(p->row->recycle_path);
	struct stat st;
	gint dir;
	guint i;

	if (parts == NULL || parts[0] == NULL || parts[1] == NULL)
		return FALSE;
	dir = fcntl(axis_volume_guard_get_fd(p->guard), F_DUPFD_CLOEXEC, 0);
	for (i = 0; dir >= 0 && parts[i + 1] != NULL; i++) {
		gint next = openat(dir, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		close(dir);
		dir = next;
	}
	if (dir < 0)
		return FALSE;
	i = g_strv_length(parts) - 1;
	if (!look(dir, parts[i], &st)) {
		close(dir);
		return FALSE;
	}
	close(dir);
	return is_inode(&st, p->row->src_inode);
}

/* Where in the recycle folder the original goes: its root's path and its
 * folders below. */
static gchar **
recycle_where(const gchar *folder)
{
	if (folder == NULL || folder[0] == '\0')
		return g_new0(gchar *, 1);
	return g_strsplit(folder, "/", 140);
}

static gboolean
note_recycled(
	Place        *p,
	const gchar  *recycle_name,
	GError      **error
){
	g_autofree gchar *id = axis_conversion_int_text(p->row->id);
	g_autofree gchar *path = recycle_stored(p->where, recycle_name);
	g_autofree gchar *ino = axis_conversion_int_text(p->row->src_inode);
	g_autofree gchar *size = axis_conversion_int_text(p->row->src_size);
	const gchar *params[4];

	params[0] = id;
	params[1] = path;
	params[2] = ino;
	params[3] = size;
	if (!axis_db_conn_exec(p->conn, "UPDATE conversion SET recycle_path = $2, recycle_inode = $3::bigint,"
	        " recycle_size = $4::bigint, updated_at = now() WHERE id = $1::bigint", 4, params, NULL, error))
		return FALSE;
	g_free(p->row->recycle_path);
	p->row->recycle_path = g_steal_pointer(&path);
	p->row->recycle_inode = p->row->src_inode;
	p->row->recycle_size = p->row->src_size;
	return TRUE;
}

/* Gives up a placement while the original is under its name: the new file
 * goes (it is a copy of the original's content; the original is there), and
 * the original's name in the recycle folder (if it was given one) goes. */
static gboolean
abandon(
	Place        *p,
	const gchar  *state,
	const gchar  *why,
	GError      **error
){
	struct stat s;
	struct stat t;
	struct stat o;
	gboolean original;
	g_autofree gchar *text = NULL;

	/* What is decided here is decided from the volume as it is now: a disk
	 * that went away is a retry, never "the original is gone". */
	if (!axis_volume_guard_check(p->guard, error))
		return FALSE;
	look(p->dir, p->name, &s);
	look(p->dir, p->row->temp_name, &t);
	look(p->work, p->row->output_name, &o);
	/* The original's file under its own name (changed or not, it is not
	 * lost by removing the new file). */
	original = is_inode(&s, p->row->src_inode);
	if (!original) {
		/* Never remove the new file without the original under its name. */
		text = g_strdup_printf("%s The original is not under its name, so the new file was kept where it is: a "
			"person must look.", why);
		return axis_conversion_set_state(p->conn, p->row->id, "failed", 0, text, error);
	}
	if (is_inode(&t, p->row->output_inode)) {
		GError *local = NULL;

		if (!axis_volume_write_unlink_temp(p->guard, p->dir, p->row->temp_name, (guint64)p->row->output_inode, TRUE,
		        NULL, &local)) {
			g_propagate_error(error, local);
			return FALSE;
		}
	}
	if (is_inode(&o, p->row->output_inode)) {
		GError *local = NULL;

		if (!axis_volume_write_unlink_work(p->guard, p->work, p->row->output_name, (guint64)p->row->output_inode, NULL,
		        &local)) {
			g_propagate_error(error, local);
			return FALSE;
		}
	}
	/* The original's second name, given here, while it still has its own. */
	if (p->same && s.st_nlink >= 2 && recycle_holds_original(p)) {
		g_auto(GStrv) parts = recycle_parts(p->row->recycle_path);
		AxisVolumeRecycledOutcome outcome;
		GError *local = NULL;

		if (!axis_volume_write_remove_recycled(p->guard, (const gchar *const *)parts, (guint64)p->row->src_inode,
		        (guint64)p->row->src_size, &outcome, &local)) {
			g_propagate_error(error, local);
			return FALSE;
		}
	}
	text = g_strdup_printf("%s Nothing was replaced: the original is as it was.", why);
	if (!axis_conversion_set_state(p->conn, p->row->id, state, 0, text, error))
		return FALSE;
	{
		g_autofree gchar *id = axis_conversion_int_text(p->row->id);
		const gchar *params[1];

		params[0] = id;
		return axis_db_conn_exec(p->conn, "UPDATE conversion SET step = NULL, recycle_path = NULL WHERE id = $1::bigint",
			1, params, NULL, error);
	}
}

/* Stops with everything where it is: the records and the disk disagree. */
static gboolean
stuck(
	Place        *p,
	const gchar  *why,
	GError      **error
){
	g_autofree gchar *text = g_strdup_printf("%s Nothing was removed; a person must look (the conversion's page says "
		"where each file is).", why);

	g_warning("conversions: conversion %" G_GINT64_FORMAT " stopped: %s", p->row->id, why);
	return axis_conversion_set_state(p->conn, p->row->id, "failed", 0, text, error);
}

/* The original is in the recycle folder now but not recorded there (a
 * crash between the step and its record): found again by its inode. */
static gboolean
find_recycled(
	Place        *p,
	GError      **error
){
	g_autofree gchar *candidate = NULL;
	struct stat st;
	gint dir;
	guint i;
	guint n;

	if (p->row->recycle_path != NULL && recycle_holds_original(p))
		return TRUE;
	dir = fcntl(axis_volume_guard_get_fd(p->guard), F_DUPFD_CLOEXEC, 0);
	{
		gint next = dir >= 0 ? openat(dir, AXIS_VOLUME_RECYCLE_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;

		if (dir >= 0)
			close(dir);
		dir = next;
	}
	for (i = 0; dir >= 0 && p->where[i] != NULL; i++) {
		gint next = openat(dir, p->where[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		close(dir);
		dir = next;
	}
	if (dir < 0)
		return TRUE;
	for (n = 0; n < 1000; n++) {
		g_free(candidate);
		candidate = n == 0 ? g_strdup(p->name) : g_strdup_printf("%.*s.%u", (gint)MIN(strlen(p->name), 250),
			p->name, n);
		if (look(dir, candidate, &st) && is_inode(&st, p->row->src_inode)) {
			close(dir);
			return note_recycled(p, candidate, error);
		}
		if (n > 0 && !look(dir, candidate, &st))
			break;
	}
	close(dir);
	return TRUE;
}

/* ── P6: the record ─────────────────────────────────────────────── */

static gboolean
bookkeeping(
	Place        *p,
	const gchar  *note,
	GError      **error
){
	const AxisConversionRow *r = p->row;
	g_autofree gchar *id = axis_conversion_int_text(r->id);
	g_autofree gchar *file = axis_conversion_int_text(r->file_id);
	g_autofree gchar *root = axis_conversion_int_text(r->root_id);
	g_autofree gchar *keep = axis_conversion_int_text(axis_conversion_setting_integer(p->self,
		AXIS_CONVERSION_SETTING_KEEP_ORIGINAL, 0));
	g_autofree gchar *size = NULL;
	g_autofree gchar *ms = NULL;
	g_autofree gchar *mn = NULL;
	g_autofree gchar *cs = NULL;
	g_autofree gchar *cn = NULL;
	g_autofree gchar *ino = NULL;
	g_autofree gchar *src_ino = axis_conversion_int_text(r->src_inode);
	g_autofree gchar *revision = axis_conversion_int_text(r->revision_id);
	g_autofree gchar *text = NULL;
	g_autoptr(AxisDbResult) updated = NULL;
	const gchar *final_name = p->same ? p->name : p->new_name;
	const gchar *params[12];
	struct stat st;

	if (!hook(p, "record", error))
		return FALSE;
	if (!look(p->dir, final_name, &st) || !is_inode(&st, r->output_inode))
		return stuck(p, "The new file is not under its name at the end.", error);
	size = axis_conversion_int_text((gint64)st.st_size);
	ms = axis_conversion_int_text((gint64)st.st_mtim.tv_sec);
	mn = axis_conversion_int_text((gint64)st.st_mtim.tv_nsec);
	cs = axis_conversion_int_text((gint64)st.st_ctim.tv_sec);
	cn = axis_conversion_int_text((gint64)st.st_ctim.tv_nsec);
	ino = axis_conversion_int_text((gint64)st.st_ino);
	text = g_strdup_printf("Replaced: the new file is in place%s%s", note != NULL ? "; " : ".",
		note != NULL ? note : "");

	if (!axis_db_conn_begin(p->conn, error))
		return FALSE;
	params[0] = file;
	params[1] = root;
	params[2] = r->new_rel_path;
	params[3] = size;
	params[4] = ms;
	params[5] = mn;
	params[6] = cs;
	params[7] = cn;
	params[8] = ino;
	params[9] = src_ino;
	params[10] = axis_file_kind_to_string(axis_file_kind_from_name(final_name, strlen(final_name)));
	/* A row a scan made for the new name after a crash is the same file:
	 * it goes, and the file's own row (its id: the links of the movie or
	 * the episodes, its history) takes the new name. */
	if (!p->same && r->file_id > 0) {
		const gchar *more[4];

		more[0] = file;
		more[1] = root;
		more[2] = r->new_rel_path;
		more[3] = ino;
		if (!axis_db_conn_exec(p->conn, "DELETE FROM library_file WHERE root_id = $2::bigint AND rel_path = $3"
		        " AND id <> $1::bigint AND inode = $4::bigint", 4, more, NULL, error))
			goto rollback;
	}
	if (r->file_id > 0 &&
	    !axis_db_conn_exec(p->conn, "UPDATE library_file SET rel_path = $3, kind = $11, size = $4::bigint,"
	        " mtime_sec = $5::bigint, mtime_nsec = $6::integer, ctime_sec = $7::bigint, ctime_nsec = $8::integer,"
	        " inode = $9::bigint, nlink = 1, pinned = false, pinned_links = false, stat_racy = true, changed = true,"
	        " changed_generation = last_seen_generation, content_changed_at = now(),"
	        " moved_at = CASE WHEN rel_path <> $3 THEN now() ELSE moved_at END"
	        " WHERE id = $1::bigint AND root_id = $2::bigint AND (inode = $10::bigint OR inode = $9::bigint)"
	        " AND NOT pinned_downloads RETURNING id", 11, params, &updated, error))
		goto rollback;
	if (r->file_id > 0 && axis_db_result_get_n_rows(updated) != 1)
		g_message("conversions: conversion %" G_GINT64_FORMAT ": the index row of the file was not the one expected; "
			"the next scan settles it", r->id);
	{
		const gchar *more[4];

		more[0] = id;
		more[1] = keep;
		more[2] = text;
		more[3] = r->recycle_path;
		if (!axis_db_conn_exec(p->conn, "UPDATE conversion SET state = 'done', step = 'recorded', finished_at = now(),"
		        " updated_at = now(), result_text = left($3, 2000),"
		        " recycle_state = CASE WHEN $4::text IS NULL THEN 'gone' ELSE 'kept' END,"
		        " recycle_remove_after = CASE WHEN $4::text IS NULL THEN NULL ELSE now() + $2::bigint * interval '1 second' END"
		        " WHERE id = $1::bigint", 4, more, NULL, error))
			goto rollback;
	}
	if (r->file_id > 0) {
		const gchar *more[3];

		more[0] = file;
		more[1] = revision;
		more[2] = id;
		if (!axis_db_conn_exec(p->conn, "INSERT INTO conversion_memory (file_id, revision_id, reason, conversion_id)"
		        " VALUES ($1::bigint, $2::bigint, 'converted', $3::bigint) ON CONFLICT (file_id, revision_id) DO UPDATE"
		        " SET reason = 'converted', conversion_id = EXCLUDED.conversion_id, recorded_at = now()", 3, more, NULL,
		        error))
			goto rollback;
	}
	if (!axis_db_conn_commit(p->conn, error))
		goto rollback;
	g_free(p->row->state);
	p->row->state = g_strdup("done");
	g_free(p->row->recycle_state);
	p->row->recycle_state = g_strdup(r->recycle_path != NULL ? "kept" : "gone");
	g_message("audit: conversion %" G_GINT64_FORMAT " replaced file %" G_GINT64_FORMAT " (requested by %s)", r->id,
		r->file_id, r->requested_by);
	return TRUE;

rollback:
	axis_db_conn_rollback(p->conn, NULL);
	return FALSE;
}

/* ── The loop ───────────────────────────────────────────────────── */

typedef enum
{
	GO_ON,
	FINISHED,
	FAILED
} Next;

/* One step: what the disk and the record say, and the next thing done. */
static Next
step(
	Place   *p,
	GError **error
){
	const AxisConversionRow *r = p->row;
	struct stat s;
	struct stat n;
	struct stat t;
	struct stat o;
	gboolean has_s = look(p->dir, p->name, &s);
	gboolean has_n = p->same ? has_s : look(p->dir, p->new_name, &n);
	GError *local = NULL;

	if (p->same)
		n = s;
	look(p->dir, r->temp_name, &t);
	look(p->work, r->output_name, &o);

	if (p->same) {
		if (has_s && is_inode(&s, r->output_inode)) {
			/* Swapped: the temporary name is the original's second name. */
			if (is_inode(&t, r->src_inode)) {
				if (!before(p, "unlink", error))
					return FAILED;
				if (!axis_volume_write_unlink_temp(p->guard, p->dir, r->temp_name, (guint64)r->src_inode, FALSE, NULL,
				        &local)) {
					if (local->domain == AXIS_STORAGE_ERROR && local->code == AXIS_STORAGE_ERROR_NOT_CONTAINED) {
						g_clear_error(&local);
						return stuck(p, "The new file is in place, but the original's temporary name is its only "
							"name; it was kept beside the new file.", error) ? FINISHED : FAILED;
					}
					g_propagate_error(error, local);
					return FAILED;
				}
				if (!hook(p, "unlink-done", error))
					return FAILED;
			}
			if (!find_recycled(p, error) || !bookkeeping(p, r->recycle_path == NULL ? "the original was not found in "
			        "the recycle folder" : NULL, error))
				return FAILED;
			return FINISHED;
		}
		if (!has_s || !is_inode(&s, r->src_inode))
			return stuck(p, "The library name holds neither the original nor the new file.", error) ? FINISHED : FAILED;
		/* The original is there: nothing was exchanged yet.  Its second
		 * name, if it was given one and the record was lost, is found. */
		if (s.st_nlink >= 2 && r->recycle_path == NULL && !find_recycled(p, error))
			return FAILED;
		if (!original_unchanged(r, &s, s.st_nlink == 2 && recycle_holds_original(p)))
			return abandon(p, "rejected", "The original changed, or got another hard link, just before placement.",
				error) ? FINISHED : FAILED;
		if (is_inode(&t, r->output_inode)) {
			if (!output_unchanged(r, &t, TRUE))
				return abandon(p, "rejected", "The new file changed after it was checked.", error) ? FINISHED : FAILED;
			if (s.st_nlink == 1) {
				g_autofree gchar *recycled = NULL;

				if (!before(p, "link", error))
					return FAILED;
				if (!axis_volume_write_retire(p->guard, p->dir, p->name, (guint64)r->src_inode,
				        (const gchar *const *)p->where, DIR_MODE, AXIS_VOLUME_RETIRE_LINK, &recycled, &local)) {
					g_propagate_error(error, local);
					return FAILED;
				}
				if (!hook(p, "link-done", error) || !note_recycled(p, recycled, error) || !set_step(p, "linked", error))
					return FAILED;
				return GO_ON;
			}
			if (!before(p, "swap", error))
				return FAILED;
			if (!axis_volume_write_swap(p->guard, p->dir, r->temp_name, (guint64)r->output_inode, p->name,
			        (guint64)r->src_inode, &local)) {
				g_propagate_error(error, local);
				return FAILED;
			}
			if (!hook(p, "swap-done", error))
				return FAILED;
			return set_step(p, "swapped", error) ? GO_ON : FAILED;
		}
	} else {
		if (has_n && is_inode(&n, r->output_inode)) {
			/* Published.  The original goes into the recycle folder, while it
			 * is still the file that was checked: one changed since (a stop
			 * between the two steps can leave it under its name for days)
			 * keeps its name beside the new file, for a person. */
			if (has_s && is_inode(&s, r->src_inode)) {
				g_autofree gchar *recycled = NULL;

				if (!original_unchanged(r, &s, FALSE))
					return stuck(p, "The new file is in place under its new name, but the original changed after it was "
						"checked, so it was not retired: both are kept, the original under its name and the new file "
						"(made from the original as it was) under the new one.", error) ? FINISHED : FAILED;
				if (!before(p, "retire", error))
					return FAILED;
				if (!axis_volume_write_retire(p->guard, p->dir, p->name, (guint64)r->src_inode,
				        (const gchar *const *)p->where, DIR_MODE, AXIS_VOLUME_RETIRE_MOVE, &recycled, &local)) {
					g_propagate_error(error, local);
					return FAILED;
				}
				if (!hook(p, "retire-done", error) || !note_recycled(p, recycled, error) ||
				    !set_step(p, "retired", error))
					return FAILED;
				return GO_ON;
			}
			if (!find_recycled(p, error) || !bookkeeping(p, has_s ? "another file has the original's old name now; "
			        "it was left alone" : r->recycle_path == NULL ? "the original was not found in the recycle folder" :
			        NULL, error))
				return FAILED;
			return FINISHED;
		}
		if (!has_s || !is_inode(&s, r->src_inode))
			return stuck(p, "The original is not under its name, and the new file is not under the new one.", error) ?
				FINISHED : FAILED;
		if (has_n)
			return abandon(p, "rejected", "A file of the new name appeared before the new file was placed.", error) ?
				FINISHED : FAILED;
		if (!original_unchanged(r, &s, FALSE))
			return abandon(p, "rejected", "The original changed, or got another hard link, just before placement.",
				error) ? FINISHED : FAILED;
		if (is_inode(&t, r->output_inode)) {
			if (!output_unchanged(r, &t, TRUE))
				return abandon(p, "rejected", "The new file changed after it was checked.", error) ? FINISHED : FAILED;
			if (!before(p, "publish", error))
				return FAILED;
			if (!axis_volume_write_publish(p->guard, p->dir, r->temp_name, (guint64)r->output_inode, p->new_name,
			        &local)) {
				if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
					g_clear_error(&local);
					return abandon(p, "rejected", "A file of the new name appeared before the new file was placed.",
						error) ? FINISHED : FAILED;
				}
				g_propagate_error(error, local);
				return FAILED;
			}
			if (!hook(p, "publish-done", error))
				return FAILED;
			return set_step(p, "published", error) ? GO_ON : FAILED;
		}
	}

	/* Not in the folder yet: still in the work area, as it was checked. */
	if (!is_inode(&o, r->output_inode))
		return abandon(p, "failed", "The new file is gone from the work area.", error) ? FINISHED : FAILED;
	if (!output_unchanged(r, &o, FALSE))
		return abandon(p, "rejected", "The new file changed after it was checked.", error) ? FINISHED : FAILED;
	if (!before(p, "adopt", error))
		return FAILED;
	if (!axis_volume_write_adopt(p->guard, p->work, r->output_name, (guint64)r->output_inode, p->dir, r->temp_name,
	        &local)) {
		g_propagate_error(error, local);
		return FAILED;
	}
	if (!hook(p, "adopt-done", error))
		return FAILED;
	return set_step(p, "adopted", error) ? GO_ON : FAILED;
}

/* The scan's volume lock, shared, on @conn: waits while a scan runs. */
static gboolean
scan_lock(
	AxisConversions  *self,
	AxisDbConn       *conn,
	gint64            volume_id,
	GError          **error
){
	g_autofree gchar *volume = axis_conversion_int_text(volume_id);
	gint64 until;
	guint tuned;
	const gchar *params[1];

	g_mutex_lock(&self->lock);
	tuned = self->tuning.scan_wait_ms;
	g_mutex_unlock(&self->lock);
	until = g_get_monotonic_time() + (tuned != 0 ? (gint64)tuned * 1000 : SCAN_WAIT_US);
	params[0] = volume;
	for (;;) {
		g_autoptr(AxisDbResult) result = NULL;
		gboolean got = FALSE;

		if (!axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock_shared(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		        &result, error))
			return FALSE;
		axis_db_result_get_boolean(result, 0, 0, &got);
		if (got)
			return TRUE;
		if (g_get_monotonic_time() >= until) {
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
				"A scan of the volume is running; the placement waits for it to end.");
			return FALSE;
		}
		g_usleep(MIN(SCAN_RETRY_US, MAX(until - g_get_monotonic_time(), 1000)));
	}
}

static void
scan_unlock(
	AxisDbConn *conn,
	gint64      volume_id
){
	g_autofree gchar *volume = axis_conversion_int_text(volume_id);
	const gchar *params[1];

	params[0] = volume;
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_unlock_shared(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params, NULL,
	        NULL))
		g_debug("conversions: the scan lock could not be given back (the connection will be)");
}

static gboolean
place_open(
	Place        *p,
	GError      **error
){
	g_autofree gchar *new_folder = NULL;

	p->dir = -1;
	if (!axis_conversion_split(p->row->root_path, p->row->rel_path, &p->folder, &p->name) ||
	    !axis_conversion_split(p->row->root_path, p->row->new_rel_path, &new_folder, &p->new_name) ||
	    strcmp(p->folder, new_folder) != 0) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_FAILED,
			"The conversion's paths cannot be read.");
		return FALSE;
	}
	p->same = strcmp(p->name, p->new_name) == 0;
	p->where = recycle_where(p->folder);
	p->dir = axis_volume_guard_open_dir(p->guard, p->folder, error);
	return p->dir >= 0;
}

/* Runs the loop from wherever the record and the disk are. */
static gboolean
advance(
	AxisConversions    *self,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	gint                work,
	GError            **error
){
	Place p;
	gboolean ok = FALSE;
	gboolean locked = FALSE;
	guint i;

	memset(&p, 0, sizeof(p));
	p.self = self;
	p.row = row;
	p.guard = guard;
	p.work = work;
	p.dir = -1;
	p.conn = axis_db_pool_acquire(self->pool, error);
	if (p.conn == NULL)
		return FALSE;
	if (!scan_lock(self, p.conn, row->volume_id, error))
		goto out;
	locked = TRUE;
	if (!place_open(&p, error))
		goto out;
	if (g_strcmp0(row->state, "placing") != 0) {
		g_autofree gchar *id = axis_conversion_int_text(row->id);
		g_autofree gchar *temp = g_strdup_printf(AXIS_VOLUME_TEMP_PREFIX "%" G_GINT64_FORMAT,
			AXIS_CONVERSION_TEMP_BASE + row->id);
		const gchar *params[2];

		/* P1: from here recovery knows what was intended. */
		if (!hook(&p, "placing", error))
			goto out;
		params[0] = id;
		params[1] = temp;
		if (!axis_db_conn_exec(p.conn, "UPDATE conversion SET state = 'placing', step = 'placing', temp_name = $2,"
		        " updated_at = now() WHERE id = $1::bigint", 2, params, NULL, error))
			goto out;
		g_free(row->state);
		row->state = g_strdup("placing");
		g_free(row->temp_name);
		row->temp_name = g_steal_pointer(&temp);
		g_free(row->step);
		row->step = g_strdup("placing");
	}
	for (i = 0; i < MAX_STEPS; i++) {
		Next next;

		if (!axis_volume_guard_check(guard, error))
			goto out;
		next = step(&p, error);
		if (next == FAILED)
			goto out;
		if (next == FINISHED) {
			ok = TRUE;
			goto out;
		}
	}
	ok = stuck(&p, "The placement did not come to an end.", error);

out:
	if (locked)
		scan_unlock(p.conn, row->volume_id);
	axis_db_pool_release(self->pool, p.conn);
	place_clear(&p);
	return ok;
}

/* ── The recycled original ──────────────────────────────────────── */

gboolean
axis_conversion_remove_original(
	AxisConversions    *self,
	AxisDbConn         *conn,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	gboolean           *removed,
	GError            **error
){
	g_auto(GStrv) parts = recycle_parts(row->recycle_path);
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	AxisVolumeRecycledOutcome outcome = AXIS_VOLUME_RECYCLED_CHANGED;
	const gchar *params[2];
	const gchar *state;

	*removed = FALSE;
	if (parts == NULL || g_strcmp0(row->recycle_state, "kept") != 0 || g_strcmp0(row->state, "done") != 0)
		return TRUE;
	if (axis_conversion_hook(self, "remove-original", row->id) != AXIS_CONVERSION_HOOK_CONTINUE) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "Stopped before the original's "
			"removal (a test).");
		return FALSE;
	}
	/* (The write layer proves the volume first: "gone" is never concluded
	 * from a disk that went away.) */
	if (!axis_volume_write_remove_recycled(guard, (const gchar *const *)parts, (guint64)row->recycle_inode,
	        (guint64)row->recycle_size, &outcome, error))
		return FALSE;
	state = outcome == AXIS_VOLUME_RECYCLED_REMOVED ? "removed" : outcome == AXIS_VOLUME_RECYCLED_GONE ? "gone" :
		"changed";
	params[0] = id;
	params[1] = state;
	if (!axis_db_conn_exec(conn, "UPDATE conversion SET recycle_state = $2, recycle_removed_at = now(),"
	        " updated_at = now() WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	*removed = outcome == AXIS_VOLUME_RECYCLED_REMOVED;
	g_free(row->recycle_state);
	row->recycle_state = g_strdup(state);
	return TRUE;
}

static gboolean
remove_original_now(
	AxisConversions    *self,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	GError            **error
){
	AxisDbConn *conn;
	gboolean removed = FALSE;
	gboolean ok;

	if (axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_KEEP_ORIGINAL, 0) > 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_conversion_remove_original(self, conn, row, guard, &removed, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_conversion_recover(
	AxisConversions    *self,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	GError            **error
){
	g_autoptr(GError) local = NULL;
	gint work = axis_conversion_work_area(guard, FALSE, &local);
	gboolean ok;

	/* The work area may be gone with nothing in it to need: the loop
	 * decides from the folder first. */
	ok = advance(self, row, guard, work, error);
	if (work >= 0)
		close(work);
	if (ok && g_strcmp0(row->state, "done") == 0)
		ok = remove_original_now(self, row, guard, error);
	return ok;
}

gboolean
axis_conversion_place(
	AxisConversions            *self,
	AxisConversionRow          *row,
	AxisVolumeGuard            *guard,
	gint                        work,
	const AxisConversionVerify *found,
	GError                    **error
){
	gboolean ok;

	ok = advance(self, row, guard, work, error);
	if (ok && g_strcmp0(row->state, "done") == 0) {
		g_autoptr(GError) local = NULL;

		if (row->file_id > 0 && !axis_probe_request_file(self->probe, row->file_id, NULL, &local))
			g_message("conversions: the new file of conversion %" G_GINT64_FORMAT " is probed at the probe's next turn: "
				"%s", row->id, local->message);
		ok = remove_original_now(self, row, guard, error);
	}
	return ok;
}
