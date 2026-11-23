/*
 * axis-import-folder.c - the leftover folder of a Usenet download after its
 * import
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A Usenet download is removed from its client once its import is recorded
 * (axis-import-cleanup.c).  SABnzbd's history delete with del_files=1 deletes
 * the files of a job that failed, not of one that completed: the job's entry
 * goes and its folder stays where it finished, with the film's file (now
 * only one name of the library's file, which keeps its own) and whatever
 * else SABnzbd left beside it, an .nfo, a sample, par2 files.  Axis is not a
 * seedbox and keeps one copy of a film, so it removes that folder itself,
 * whole: it is the client's own job folder under Axis's own category, and
 * nothing else's.  Only when:
 *
 *  - the import is imported and its client removal is done (a job that was
 *    already gone from SABnzbd is done too: SABnzbd's history delete answers
 *    the same either way); a client removal still to be made, or failed,
 *    keeps the folder waiting for it, and one the client never will make
 *    (kept) leaves the folder with the client;
 *  - the download is a Usenet one (a torrent's files are its client's
 *    business, by import.torrents_after);
 *  - every rule of axis_import_folder_judge() allows it, on the text alone:
 *    the client's path for the download, as it reported it when the
 *    download completed, is a full path with no "." or ".." part; it lies
 *    strictly inside the folder the client keeps Axis's category in (asked
 *    of the client now: never the category's folder itself, never outside
 *    it); the one path mapping that reaches it also holds that category
 *    folder; through it, it is the very place the download was recorded at
 *    and the import took its files from, on a known volume; it is not the
 *    volume's top or a folder of Axis's own; it is not inside a library root
 *    (a downloads root aside, which is where downloads are), and it is not
 *    a root and holds none; no other download Axis knows that is still at
 *    its client, and no import still open, is in it or around it;
 *  - what is at the reported place decides what goes (looked at through a
 *    read guard first, which storage read-only allows, so that a rule that
 *    refuses is known at once): a folder is the job's folder and goes; a
 *    regular file (SABnzbd reports a job of one file by the file, seen on a
 *    real server) means the job's folder is the first part beneath the
 *    category's folder on the way to it, and that goes, at whatever depth
 *    the file is; a file straight in the category's folder goes alone, and
 *    the category's folder never does.  The rules of the place (Axis's own
 *    folders, the library roots, other downloads and open imports) apply to
 *    what goes;
 *  - and then, through a write guard of the volume: the way to it passes no
 *    symbolic link and no other filesystem; it is a folder (or, alone, a
 *    regular file), not a link; a file the import took is still at its place
 *    in it (the reported place, then the file's recorded path) with the same
 *    inode, so it is still what was imported from, not something new at the
 *    same place; and axis_volume_write_remove_folder() finds no other
 *    filesystem inside it and nothing beyond its bounds.
 *
 * A rule that refuses is final: the folder is left, and the import says
 * which rule.  Storage read-only (the default), the volume offline or
 * mounted read-only, or the client not answering, is not a refusal: the
 * folder waits and the hourly cleanup looks again.
 *
 * A torrent is removed with its data by its client after its import (unless
 * import.torrents_after is "seed"), and nothing of it is Axis's to delete.
 * But a client may no longer have it when asked (a plugin that removes
 * finished torrents and leaves their files, a person, another program): its
 * client removal is then "gone" (axis-import-cleanup.c), and what it left is
 * looked at here, under the same rules, with the client's folder for Axis's
 * label or category as the category's folder (Deluge's Label plugin's "Move
 * completed to" of the label, qBittorrent's category save path; each only
 * when no folder for every download and no other label or category shares
 * it).  When the client has no folder of Axis's own, a torrent's folder is
 * never deleted: a torrent names its own folder, and in a folder shared with
 * other downloads (another manager's, a cross-seed of the same files) one of
 * that name is not told from another; the page says why and where it is.
 * Its one file (a torrent of one file) is deleted alone, by name and inode,
 * as a job's file straight in the category's folder is, and only while the
 * library still has the file the import placed of it.  With the setting on
 * "seed" nothing of a torrent is deleted.
 *
 * A Usenet job reported by a file inside its folder is imported from that
 * folder (axis-import-run.c, so that a subtitle beside the file is taken):
 * the import's source is then the job's folder, which is what goes, and the
 * import's files are found in it relative to it.
 *
 * A download removed from its client with its data outside an import (a
 * person giving up on it, a failure put on the blocklist) has no import to
 * look after what its client kept; axis_downloads_remove() records it
 * (download.folder_removal) and the hourly cleanup looks at it here, under
 * the same rules, with the inode of what was at the reported place when it
 * completed (download.location_inode) standing in for an imported file, and
 * more strictly: without a location, that inode, or a folder of Axis's own
 * (Usenet or torrent), nothing is deleted and the download says where its
 * files are.
 *
 * A removal is two halves, called one after the other: folder_decide()
 * (everything above up to the looks through the read guard and what the
 * clients have now; it deletes nothing) and folder_delete() (the write
 * guard, the checks again through it, the removal).  "Delete this download"
 * on an import that waits for a person (axis-import-delete.c) deletes
 * through these two and nothing else, as a download removed outside an
 * import; its confirmation runs the first half alone and writes nothing
 * (axis_import_download_folder_look()), so that it shows the decision the
 * deletion makes.  Two things are its own.  The setting that leaves torrents
 * seeding is set aside for a download a person asked to be deleted, whose
 * import was not imported (the setting is about what Axis does by itself
 * after an import); no other rule is.  And a torrent its client still has
 * is deleted by its client, never by Axis: its place is judged here first,
 * by the rules of the text without the one about a folder of Axis's own
 * (AxisImportFolderFacts.client_deletes), the client must say now that the
 * torrent is at the recorded place and that place must be named as the
 * torrent is (never the client's download folder itself), and the volume
 * must be writable, before the client is asked.  What the client says is
 * asked before the place is looked at, and nothing at the recorded place is
 * a refusal for such a torrent, never "gone": the client, asked to remove it
 * with its data, deletes it wherever it is (in a library folder, when it was
 * moved there in the client).
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
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-write.h"

/* Imports looked at per run. */
#define BATCH 100

/* Files of an import compared with what is in the folder, at most. */
#define IDENTITY_FILES 50

/* ── The rules (pure) ───────────────────────────────────────────── */

static AxisImportFolderRule
refuse(
	gchar                **why,
	AxisImportFolderRule   rule,
	gchar                 *text
){
	*why = text;
	return rule;
}

/* Whether @path is the volume's top or lies in a folder of Axis's own at the
 * top of the volume (".axis-recycle", ".axis-work", the marker). */
static gboolean
axis_own(const gchar *path)
{
	gsize first;

	if (path == NULL || path[0] == '\0')
		return TRUE;
	first = strcspn(path, "/");
	return first >= 6 && strncmp(path, ".axis-", 6) == 0;
}

AxisImportJobPlace
axis_import_job_folder(
	const gchar  *category_folder,
	const gchar  *client_path,
	const gchar  *mapped_path,
	gchar       **job_client,
	gchar       **job_rel,
	gchar       **inner
){
	const gchar *rest;
	const gchar *slash;
	const gchar *below;
	gsize mapped_n;
	gsize below_n;

	if (job_client != NULL)
		*job_client = NULL;
	if (job_rel != NULL)
		*job_rel = NULL;
	if (inner != NULL)
		*inner = NULL;
	if (category_folder == NULL || client_path == NULL || mapped_path == NULL ||
	    strcmp(category_folder, client_path) == 0 || !axis_storage_path_contains(category_folder, client_path))
		return AXIS_IMPORT_JOB_UNTOLD;
	rest = client_path + strlen(category_folder) + 1;
	slash = strchr(rest, '/');
	if (slash == NULL)
		return AXIS_IMPORT_JOB_NONE;
	/* The mapping that reaches the file holds the category's folder, so on
	 * the volume the job's folder is the file's place without the parts
	 * below it. */
	below = slash + 1;
	mapped_n = strlen(mapped_path);
	below_n = strlen(below);
	if (below_n == 0 || mapped_n <= below_n + 1 || !g_str_has_suffix(mapped_path, below) ||
	    mapped_path[mapped_n - below_n - 1] != '/')
		return AXIS_IMPORT_JOB_UNTOLD;
	if (job_client != NULL)
		*job_client = g_strndup(client_path, (gsize)(slash - client_path));
	if (job_rel != NULL)
		*job_rel = g_strndup(mapped_path, mapped_n - below_n - 1);
	if (inner != NULL)
		*inner = g_strdup(below);
	return AXIS_IMPORT_JOB_FOLDER;
}

AxisImportFolderRule
axis_import_folder_judge(
	const AxisImportFolderFacts  *facts,
	AxisImportFolderShape         shape,
	AxisImportFolderTarget       *target,
	gchar                       **why
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *job_client = NULL;
	g_autofree gchar *job_rel = NULL;
	g_autofree gchar *inner = NULL;
	AxisImportJobPlace place = AXIS_IMPORT_JOB_UNTOLD;
	AxisImportFolderTarget chosen;
	gboolean from_job_folder = FALSE;
	guint i;

	*why = NULL;
	memset(target, 0, sizeof(*target));
	/* "..", ".", a relative path: refused, never resolved. */
	if (facts->client_path == NULL || !axis_storage_path_normalize(facts->client_path, TRUE, &path, NULL))
		return refuse(why, AXIS_IMPORT_FOLDER_BAD_PATH, g_strdup("the path its client reported for it is not a full "
			"path, or has a '.' or '..' part (Axis never resolves one)"));
	if (facts->category_folder == NULL && !facts->torrent)
		return refuse(why, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, g_strdup("where its client keeps Axis's category "
			"is not known"));
	if (facts->category_folder != NULL) {
		if (strcmp(facts->category_folder, path) == 0)
			return refuse(why, AXIS_IMPORT_FOLDER_IS_CATEGORY, g_strdup_printf("it is the folder of Axis's category "
				"at the client (%s) itself, not a job's own folder: the client put the job's files straight into it",
				facts->category_folder));
		if (!axis_storage_path_contains(facts->category_folder, path))
			return refuse(why, AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY, g_strdup_printf("its client reported it at %s, "
				"which is not inside the folder of Axis's category at the client (%s)", path,
				facts->category_folder));
		if (facts->map_prefix == NULL || !axis_storage_path_contains(facts->map_prefix, facts->category_folder))
			return refuse(why, AXIS_IMPORT_FOLDER_MAP_INSIDE, g_strdup_printf("the path mapping that reaches it (%s) "
				"begins inside the folder of Axis's category, so on the volume the job's folder cannot be told from "
				"the category's", facts->map_prefix != NULL ? facts->map_prefix : "none"));
	}
	if (facts->mapped_path == NULL || facts->recorded_path == NULL || facts->source_path == NULL ||
	    facts->mapped_volume <= 0 || facts->mapped_volume != facts->recorded_volume ||
	    facts->mapped_volume != facts->source_volume || strcmp(facts->mapped_path, facts->recorded_path) != 0)
		return refuse(why, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, g_strdup("through the client's path mapping it is "
			"now another place than the one its files were imported from"));
	if (facts->category_folder != NULL && shape == AXIS_IMPORT_FOLDER_SHAPE_FILE)
		place = axis_import_job_folder(facts->category_folder, path, facts->mapped_path, &job_client, &job_rel, &inner);
	/* The import took its files from the reported place, or (a file reported
	 * inside a job's folder) from that job's folder. */
	if (strcmp(facts->mapped_path, facts->source_path) != 0) {
		if (place != AXIS_IMPORT_JOB_FOLDER || strcmp(job_rel, facts->source_path) != 0)
			return refuse(why, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, g_strdup("through the client's path mapping it "
				"is now another place than the one its files were imported from"));
		from_job_folder = TRUE;
	}

	/* What goes. */
	memset(&chosen, 0, sizeof(chosen));
	if (facts->client_deletes) {
		/* A torrent its client still has, which the client is to delete: the
		 * reported place itself, whatever it is.  The rules of the place
		 * below are asked of it before the client is. */
		chosen.file = shape == AXIS_IMPORT_FOLDER_SHAPE_FILE;
		chosen.path = g_strdup(facts->mapped_path);
		chosen.client_path = g_strdup(path);
		chosen.inner = g_strdup("");
	} else if (facts->category_folder == NULL) {
		/* A torrent whose client keeps Axis's torrents in no folder of their
		 * own: its one file, alone, by its inode; its folder never, for in a
		 * folder shared with other downloads a folder the torrent names
		 * cannot be told from another's of that name. */
		if (shape == AXIS_IMPORT_FOLDER_SHAPE_FOLDER)
			return refuse(why, AXIS_IMPORT_FOLDER_NO_OWN_FOLDER, g_strdup_printf("its client keeps Axis's torrents in "
				"no folder of their own (%s), and in a folder shared with other downloads a torrent's folder, which the "
				"torrent itself names, cannot be told from another of that name, so Axis does not delete it on that "
				"evidence. It is at %s at the client and can be deleted there by hand. Giving Axis's label or category "
				"a folder of its own at the client (in Deluge, the label's \"Move completed to\" in the Label plugin; "
				"in qBittorrent, the category's save path) lets Axis delete such folders",
				facts->no_folder_why != NULL ? facts->no_folder_why : "it says of none", path));
		chosen.file = TRUE;
		chosen.loose = TRUE;
		chosen.path = g_strdup(facts->mapped_path);
		chosen.client_path = g_strdup(path);
		chosen.inner = g_strdup("");
	} else if (shape == AXIS_IMPORT_FOLDER_SHAPE_FOLDER) {
		/* The reported folder, the job's own. */
		chosen.path = g_strdup(facts->mapped_path);
		chosen.client_path = g_strdup(path);
		chosen.inner = g_strdup("");
	} else if (place == AXIS_IMPORT_JOB_NONE) {
		/* A file straight in the category's folder: that file alone. */
		chosen.file = TRUE;
		chosen.path = g_strdup(facts->mapped_path);
		chosen.client_path = g_strdup(path);
		chosen.inner = g_strdup("");
	} else if (place == AXIS_IMPORT_JOB_FOLDER) {
		/* A file inside a job's folder (at any depth): that folder, the
		 * first part beneath the category's.  The import's files are
		 * recorded relative to its source: the reported file, or the job's
		 * folder when the import took that. */
		chosen.path = g_steal_pointer(&job_rel);
		chosen.client_path = g_steal_pointer(&job_client);
		chosen.inner = from_job_folder ? g_strdup("") : g_steal_pointer(&inner);
	} else {
		return refuse(why, AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM, g_strdup("on the volume its job's folder cannot be "
			"told from the file's place"));
	}

	/* The rules of the place, for what goes. */
	if (axis_own(chosen.path)) {
		axis_import_folder_target_clear(&chosen);
		return refuse(why, AXIS_IMPORT_FOLDER_AXIS_OWN, g_strdup("it is the top of its volume, or a folder of "
			"Axis's own there"));
	}
	for (i = 0; facts->roots != NULL && i < facts->roots->len; i++) {
		const AxisImportFolderRoot *root = g_ptr_array_index(facts->roots, i);

		if (root->rel_path != NULL && strcmp(root->rel_path, chosen.path) == 0) {
			axis_import_folder_target_clear(&chosen);
			return refuse(why, AXIS_IMPORT_FOLDER_HOLDS_ROOT, g_strdup_printf("it is the library root %s itself",
				root->name));
		}
	}
	for (i = 0; facts->roots != NULL && i < facts->roots->len; i++) {
		const AxisImportFolderRoot *root = g_ptr_array_index(facts->roots, i);

		if (root->rel_path != NULL && !root->downloads && axis_storage_path_contains(root->rel_path, chosen.path)) {
			axis_import_folder_target_clear(&chosen);
			return refuse(why, AXIS_IMPORT_FOLDER_IN_LIBRARY, g_strdup_printf("it is inside the library root %s",
				root->name));
		}
	}
	for (i = 0; facts->roots != NULL && i < facts->roots->len; i++) {
		const AxisImportFolderRoot *root = g_ptr_array_index(facts->roots, i);

		if (root->rel_path != NULL && axis_storage_path_contains(chosen.path, root->rel_path)) {
			axis_import_folder_target_clear(&chosen);
			return refuse(why, AXIS_IMPORT_FOLDER_HOLDS_ROOT, g_strdup_printf("it holds the library root %s",
				root->name));
		}
	}
	for (i = 0; facts->others != NULL && i < facts->others->len; i++) {
		const gchar *other = g_ptr_array_index(facts->others, i);

		if (other != NULL && (axis_storage_path_contains(chosen.path, other) ||
		    axis_storage_path_contains(other, chosen.path))) {
			axis_import_folder_target_clear(&chosen);
			return refuse(why, AXIS_IMPORT_FOLDER_SHARED, g_strdup("another download Axis knows that is still at "
				"its client, or an import still open, is in it or around it"));
		}
	}
	*target = chosen;
	return AXIS_IMPORT_FOLDER_OK;
}

void
axis_import_folder_target_clear(AxisImportFolderTarget *target)
{
	if (target == NULL)
		return;
	g_clear_pointer(&target->path, g_free);
	g_clear_pointer(&target->client_path, g_free);
	g_clear_pointer(&target->inner, g_free);
	target->file = FALSE;
	target->loose = FALSE;
}

/* ── Recording ──────────────────────────────────────────────────── */

/* Whose leftover it is: an import's (import.folder_removal), or a download's
 * that was removed from its client with its data outside an import
 * (download.folder_removal). */
typedef struct
{
	gchar *state;	/* "left", "waiting", "gone", "done"; NULL: nothing was said */
	gchar *text;
} Said;

typedef struct
{
	gint64   id;
	gboolean download;
	/* (nullable) A look that writes nothing (a confirmation page; the
	 * judgement before a client is asked): what would be recorded is kept
	 * here instead. */
	Said    *dry;
} Subject;

static gboolean
record(
	AxisImport     *self,
	const Subject  *who,
	const gchar    *state,
	const gchar    *text,
	GError        **error
){
	g_autofree gchar *id = axis_import_int_text(who->id);
	const gchar *params[3];

	if (who->dry != NULL) {
		g_free(who->dry->state);
		g_free(who->dry->text);
		who->dry->state = g_strdup(state);
		who->dry->text = g_strdup(text);
		return TRUE;
	}
	params[0] = id;
	params[1] = state;
	params[2] = text;
	if (who->download)
		return axis_db_pool_exec(self->pool, "UPDATE download SET folder_removal = $2, folder_removal_text ="
			" left($3, 1000), updated_at = now() WHERE id = $1::bigint AND folder_removal IN ('pending', 'waiting')",
			3, params, NULL, error);
	return axis_db_pool_exec(self->pool, "UPDATE import SET folder_removal = $2, folder_removal_text = left($3, 1000),"
		" updated_at = now() WHERE id = $1::bigint AND folder_removal IN ('pending', 'waiting')", 3, params, NULL,
		error);
}

static gboolean
left_as(
	AxisImport     *self,
	const Subject  *who,
	gboolean        file,
	const gchar    *why,
	GError        **error
){
	g_autofree gchar *text = g_strdup_printf("Its %s was left where it is: %s.", file ? "file" : "folder", why);

	/* A look that writes nothing: the rule alone, for the page's sentence. */
	if (who->dry != NULL)
		return record(self, who, "left", why, error);
	return record(self, who, "left", text, error);
}

static gboolean
left(
	AxisImport     *self,
	const Subject  *who,
	const gchar    *why,
	GError        **error
){
	return left_as(self, who, FALSE, why, error);
}

static gboolean
waits(
	AxisImport     *self,
	const Subject  *who,
	gboolean        torrent,
	const gchar    *why,
	GError        **error
){
	g_autofree gchar *text = torrent ?
		g_strdup_printf("What the torrent left at the client is still there, and is deleted once it can be: %s It is "
			"tried again every hour.", why) :
		g_strdup_printf("Its folder at the client is still there, and is deleted once it can be: %s It is tried again "
			"every hour.", why);

	if (who->dry != NULL)
		return record(self, who, "waiting", why, error);
	return record(self, who, "waiting", text, error);
}

/* ── Looking at the folder ──────────────────────────────────────── */

/* Splits a stored path at its last '/': its folder ("" for none) and name. */
static void
split_last(
	const gchar  *path,
	gchar       **parent,
	gchar       **last
){
	const gchar *slash = strrchr(path, '/');

	*parent = slash != NULL ? g_strndup(path, (gsize)(slash - path)) : g_strdup("");
	*last = g_strdup(slash != NULL ? slash + 1 : path);
}

typedef enum
{
	SEEN_FOLDER,
	SEEN_FILE,
	SEEN_ABSENT,
	SEEN_REFUSED,	/* a link, another filesystem, not a file or folder: *why */
	SEEN_WAIT	/* it cannot be looked at now: *why */
} Seen;

/* What is at @rel on the guard's volume, reached without a link or another
 * filesystem, never following one at its place. */
static Seen
look_at(
	AxisVolumeGuard  *guard,
	const gchar      *rel,
	guint64          *ino,
	gchar           **why
){
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	g_autoptr(GError) local = NULL;
	struct stat st;
	gint pfd;
	gint rc;

	*ino = 0;
	*why = NULL;
	split_last(rel, &parent, &last);
	pfd = axis_volume_guard_open_dir(guard, parent, &local);
	if (pfd < 0) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
		    g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
			return SEEN_ABSENT;
		if (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_NOT_CONTAINED ||
		    local->code == AXIS_STORAGE_ERROR_INVALID)) {
			*why = g_strdup("the way to it passes a symbolic link or another filesystem, which Axis never follows");
			return SEEN_REFUSED;
		}
		*why = g_strdup(local->message);
		return SEEN_WAIT;
	}
	rc = fstatat(pfd, last, &st, AT_SYMLINK_NOFOLLOW);
	if (rc != 0) {
		gint err = errno;

		close(pfd);
		if (err == ENOENT)
			return SEEN_ABSENT;
		*why = g_strdup_printf("it cannot be looked at (%s).", g_strerror(err));
		return SEEN_WAIT;
	}
	close(pfd);
	if (S_ISLNK(st.st_mode)) {
		*why = g_strdup("it is a symbolic link, which Axis never follows");
		return SEEN_REFUSED;
	}
	if ((guint64)st.st_dev != axis_volume_guard_get_dev(guard)) {
		*why = g_strdup("it is another filesystem than its volume");
		return SEEN_REFUSED;
	}
	*ino = (guint64)st.st_ino;
	if (S_ISDIR(st.st_mode))
		return SEEN_FOLDER;
	if (S_ISREG(st.st_mode))
		return SEEN_FILE;
	*why = g_strdup("it is not a file or a folder");
	return SEEN_REFUSED;
}

/* Whether @dir still holds a file the import took, at its place, a regular
 * file with the same inode.  The import's files are recorded relative to
 * its source (the reported place): within @dir that is @inner, then the
 * recorded path ("" for a source that is the file itself).  -1 when the
 * import recorded none to compare with. */
static gint
still_imported_from(
	AxisImport  *self,
	gint64       import_id,
	gint         dir,
	const gchar *inner,
	guint64      dev,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *limit = axis_import_int_text(IDENTITY_FILES);
	const gchar *params[2];
	guint i;

	params[0] = id;
	params[1] = limit;
	if (!axis_db_pool_exec(self->pool, "SELECT path, inode FROM import_file WHERE import_id = $1::bigint"
	        " AND kind = 'video' AND accepted AND role IN ('film', 'episode') AND inode IS NOT NULL"
	        " ORDER BY position LIMIT $2::integer", 2, params, &rows, error))
		return -2;
	if (axis_db_result_get_n_rows(rows) == 0)
		return -1;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(GString) bytes = NULL;
		g_autofree gchar *place = NULL;
		g_auto(GStrv) parts = NULL;
		struct stat st;
		gint64 inode = 0;
		gint current;
		guint k;

		axis_db_result_get_int64(rows, i, 1, &inode);
		if (!axis_library_name_decode(axis_db_result_get_text(rows, i, 0), &bytes) ||
		    memchr(bytes->str, '\0', bytes->len) != NULL)
			continue;
		if (inner != NULL && inner[0] != '\0')
			place = bytes->len > 0 ? g_strconcat(inner, "/", bytes->str, NULL) : g_strdup(inner);
		else
			place = g_strdup(bytes->str);
		if (place[0] == '\0')
			continue;
		parts = g_strsplit(place, "/", 70);
		current = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		for (k = 0; current >= 0 && parts[k] != NULL && parts[k + 1] != NULL; k++) {
			gint next = openat(current, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

			close(current);
			current = next;
			if (current >= 0 && (fstat(current, &st) != 0 || (guint64)st.st_dev != dev)) {
				close(current);
				current = -1;
			}
		}
		if (current < 0 || parts[k] == NULL)
			continue;
		if (fstatat(current, parts[k], &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
		    (guint64)st.st_ino == (guint64)inode && (guint64)st.st_dev == dev) {
			close(current);
			return 1;
		}
		close(current);
	}
	return 0;
}

/*
 * Whether the library still holds what the import placed of the download's
 * one file (its source being that file, recorded at ""): the file's row
 * placed, with its folder, name and inode recorded, and at that place in the
 * import's root a regular file with that inode, reached without a link or
 * another filesystem.  1 yes; 0 no (*@where: the library's place, for the
 * page); -1 nothing placed was recorded; -3 it cannot be looked at now
 * (*@why); -2 with @error for the database.
 */
static gint
library_copy_present(
	AxisImport  *self,
	gint64       import_id,
	gchar      **where,
	gchar      **why,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(GString) dir_bytes = NULL;
	g_autoptr(GString) name_bytes = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *stored = NULL;
	g_auto(GStrv) parts = NULL;
	const gchar *params[1];
	struct stat st;
	gint64 inode = 0;
	gint64 volume = 0;
	guint64 dev;
	gint current;
	guint k;

	*where = NULL;
	*why = NULL;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT f.dest_dir, f.dest_name, f.dest_inode, r.volume_id, r.rel_path, r.name"
	        " FROM import_file f JOIN import i ON i.id = f.import_id JOIN library_root r ON r.id = i.root_id"
	        " WHERE f.import_id = $1::bigint AND f.path = '' AND f.accepted AND f.dest_dir IS NOT NULL"
	        " AND f.dest_name IS NOT NULL AND f.dest_inode IS NOT NULL ORDER BY f.position LIMIT 1", 1, params, &rows,
	        error))
		return -2;
	if (axis_db_result_get_n_rows(rows) == 0)
		return -1;
	axis_db_result_get_int64(rows, 0, 2, &inode);
	axis_db_result_get_int64(rows, 0, 3, &volume);
	stored = g_strconcat(axis_db_result_get_text(rows, 0, 0), "/", axis_db_result_get_text(rows, 0, 1), NULL);
	{
		g_autofree gchar *shown = axis_library_name_display(stored);

		*where = g_strdup_printf("%s, in %s", shown, axis_db_result_get_text(rows, 0, 5));
	}
	if (!axis_library_name_decode(axis_db_result_get_text(rows, 0, 0), &dir_bytes) ||
	    memchr(dir_bytes->str, '\0', dir_bytes->len) != NULL ||
	    !axis_library_name_decode(axis_db_result_get_text(rows, 0, 1), &name_bytes) ||
	    memchr(name_bytes->str, '\0', name_bytes->len) != NULL || name_bytes->len == 0)
		return 0;
	reader = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, &local);
	if (reader == NULL) {
		*why = g_strdup_printf("the library's volume cannot be looked at, to see that the import's file is still "
			"there (%s).", local->message);
		return -3;
	}
	dev = axis_volume_guard_get_dev(reader);
	current = axis_volume_guard_open_dir(reader, axis_db_result_get_text(rows, 0, 4), NULL);
	parts = g_strsplit(dir_bytes->str, "/", 140);
	for (k = 0; current >= 0 && parts[k] != NULL; k++) {
		gint next;

		if (parts[k][0] == '\0')
			continue;
		next = openat(current, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		close(current);
		current = next;
		if (current >= 0 && (fstat(current, &st) != 0 || (guint64)st.st_dev != dev)) {
			close(current);
			current = -1;
		}
	}
	if (current < 0)
		return 0;
	if (fstatat(current, name_bytes->str, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
	    (guint64)st.st_ino == (guint64)inode && (guint64)st.st_dev == dev) {
		close(current);
		return 1;
	}
	close(current);
	return 0;
}

/* Whether what is at @inner within @dir ("" for @dir itself: a folder the
 * client reported) has @inode on @dev, reached without a link or another
 * filesystem; a file at a deeper place must be a regular file.  1 or 0. */
static gint
inode_at(
	gint         dir,
	const gchar *inner,
	guint64      dev,
	guint64      inode
){
	g_auto(GStrv) parts = NULL;
	struct stat st;
	gint current;
	guint k;

	if (inode == 0)
		return 0;
	if (inner == NULL || inner[0] == '\0')
		return fstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && (guint64)st.st_dev == dev &&
			(guint64)st.st_ino == inode ? 1 : 0;
	parts = g_strsplit(inner, "/", 70);
	current = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	for (k = 0; current >= 0 && parts[k] != NULL && parts[k + 1] != NULL; k++) {
		gint next = openat(current, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		close(current);
		current = next;
		if (current >= 0 && (fstat(current, &st) != 0 || (guint64)st.st_dev != dev)) {
			close(current);
			current = -1;
		}
	}
	if (current < 0 || parts[k] == NULL) {
		if (current >= 0)
			close(current);
		return 0;
	}
	if (fstatat(current, parts[k], &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
	    (guint64)st.st_dev == dev && (guint64)st.st_ino == inode) {
		close(current);
		return 1;
	}
	close(current);
	return 0;
}

/* ── One import ─────────────────────────────────────────────────── */

typedef struct
{
	gint64  import_id;
	gint64  source_volume;
	gchar  *source_path;
	gchar  *client_removal;
	gint64  download_id;
	gchar  *protocol;
	gint64  client_id;
	gchar  *client_path;
	gint64  recorded_volume;
	gchar  *recorded_path;
	gboolean torrent;
	/* A download removed outside an import (no import_id): its leftover is
	 * told by the inode of its place recorded when it completed. */
	Subject  who;
	guint64  location_inode;
	gchar   *release_name;
	/* The import moved the download's files (import.mode move or
	 * move_copy): they are not in the folder any more, and what tells that it
	 * is the folder imported from is moved_identity()'s. */
	gboolean moved;
	guint64  source_inode;
	/* A person asked for this download to be deleted, on the page of its
	 * import, which was not imported ("Delete this download": the import is
	 * dismissed with the reason download_deleted).  The one rule set aside for
	 * it is the import setting that leaves torrents seeding, which is about
	 * what Axis does by itself after an import. */
	gboolean asked;
	/* A torrent its client still has, which the client is to be asked to
	 * remove with its data: its place is judged first (the rules of the text
	 * with AxisImportFolderFacts.client_deletes, the look through the read
	 * guard, what the clients have in or around it), and the client must say
	 * the torrent is at the recorded place now.  Only ever looked at dry
	 * (Subject.dry): Axis deletes nothing of it itself. */
	gboolean at_client;
	gchar   *client_ref;
} Candidate;

static void
candidate_clear(Candidate *c)
{
	g_free(c->client_ref);
	g_free(c->source_path);
	g_free(c->client_removal);
	g_free(c->protocol);
	g_free(c->client_path);
	g_free(c->recorded_path);
	g_free(c->release_name);
}

/* ── After a move: is it still the folder imported from? ────────── */

/*
 * What may go with a folder whose files the import moved, and what leaves it
 * where it is.  A file of at least the feature size (a quarter of the
 * smallest video the import moved, or FEATURE_MIN_BYTES whatever that was) is
 * not the debris of a download (an .nfo, a .sfv, the par2 volumes, proof
 * pictures: a set of par2 volumes is a few per cent of a release, each far
 * smaller), so it leaves the folder unless the import classified it as what
 * may go (a sample, an extra it does not import):
 *
 *  - a video the import looked at and did not take (not the title, too short
 *    to be it and not named as an extra, one a person did not choose, an
 *    episode not wanted): another download's or a second feature;
 *  - a file that is not media at all (a disc image, an archive): the content
 *    itself, in a form Axis does not import;
 *  - anything the import did not record (it appeared after the walk).
 *
 * Smaller files go with the folder, and so do those the import took (they
 * are not in it any more).
 */
#define FEATURE_MIN_BYTES ((guint64)1024 * 1024 * 1024)

typedef enum
{
	SURVEY_TAKEN = 1,	/* the import took it */
	SURVEY_MAY_GO,		/* classified as a sample or an extra not imported */
	SURVEY_LOOKED		/* recorded, and not taken */
} SurveyClass;

typedef struct
{
	GHashTable *accounted;	/* "<path>\n<inode>" of every file the import recorded: its SurveyClass */
	guint64     dev;
	guint64     threshold;
	guint       seen;
	gchar      *found;	/* a file of feature size that does not go with the folder */
	guint64     found_size;
	gboolean    found_video;
	SurveyClass found_class;	/* 0: not recorded */
	gboolean    too_big;
} Survey;

/* Looks through the folder @dir (its place in the import's source is
 * @prefix, "" for the source's top), bounded as the removal is and never
 * following a link: %FALSE when it found what blocks (survey->found or
 * too_big). */
static gboolean
survey_folder(
	Survey      *survey,
	gint         dir,
	const gchar *prefix,
	guint        depth
){
	gint copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR *stream = copy >= 0 ? fdopendir(copy) : NULL;
	g_autoptr(GPtrArray) folders = g_ptr_array_new_with_free_func(g_free);
	struct dirent *entry;
	gboolean ok = TRUE;
	guint i;

	if (stream == NULL) {
		if (copy >= 0)
			close(copy);
		return TRUE;
	}
	while (ok && (entry = readdir(stream)) != NULL) {
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (++survey->seen > AXIS_VOLUME_FOLDER_ENTRIES) {
			survey->too_big = TRUE;
			ok = FALSE;
			break;
		}
		if (fstatat(dirfd(stream), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if ((guint64)st.st_dev == survey->dev)
				g_ptr_array_add(folders, g_strdup(entry->d_name));
			continue;
		}
		if (!S_ISREG(st.st_mode) || (guint64)st.st_size < survey->threshold)
			continue;
		{
			GString *stored = g_string_new(prefix);
			g_autofree gchar *key = NULL;
			SurveyClass class;

			if (stored->len > 0)
				g_string_append_c(stored, '/');
			axis_library_name_encode(stored, entry->d_name, strlen(entry->d_name));
			key = g_strdup_printf("%s\n%" G_GUINT64_FORMAT, stored->str, (guint64)st.st_ino);
			class = (SurveyClass)GPOINTER_TO_INT(g_hash_table_lookup(survey->accounted, key));
			if (class != SURVEY_TAKEN && class != SURVEY_MAY_GO) {
				survey->found = axis_library_name_display(stored->str);
				survey->found_size = (guint64)st.st_size;
				survey->found_video = axis_import_file_kind(entry->d_name, strlen(entry->d_name)) ==
					AXIS_IMPORT_FILE_VIDEO;
				survey->found_class = class;
				ok = FALSE;
			}
			g_string_free(stored, TRUE);
		}
	}
	closedir(stream);
	for (i = 0; ok && i < folders->len; i++) {
		const gchar *name = g_ptr_array_index(folders, i);
		GString *stored;
		gint child;

		if (depth + 1 > AXIS_VOLUME_FOLDER_DEPTH) {
			survey->too_big = TRUE;
			return FALSE;
		}
		child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		if (child < 0)
			continue;
		stored = g_string_new(prefix);
		if (stored->len > 0)
			g_string_append_c(stored, '/');
		axis_library_name_encode(stored, name, strlen(name));
		ok = survey_folder(survey, child, stored->str, depth + 1);
		g_string_free(stored, TRUE);
		close(child);
	}
	return ok;
}

/*
 * The identity check of a folder whose files the import moved, in place of
 * still_imported_from() (the files it took are not in it any more, so one
 * still there cannot be what tells it is the folder imported from).  All of
 * these must hold:
 *
 *  - the folder is the one the import walked: its inode is the one recorded
 *    then (import.source_inode), before anything was renamed out of it;
 *  - every video the import moved is in the library where the import put it,
 *    a regular file with the inode recorded (for a rename, the very inode the
 *    download's file had), reached without a link;
 *  - nothing in the folder, at any depth within the removal's bounds, is a
 *    video of feature size (FEATURE_MIN_BYTES, or a quarter of the smallest
 *    video moved) that the import does not account for by its place and
 *    inode: such a file is not Axis's to delete (another download, a file of
 *    a person's).
 *
 * The rules of the place (inside the client's folder for Axis, no root, no
 * other download or open import in or around it) were applied before.  1 it
 * is; 0 it is not (*@why); -1 nothing was recorded to tell (*@why); -2 with
 * @error for the database; -3 it cannot be looked at now (*@why).
 */
static gint
moved_identity(
	AxisImport       *self,
	const Candidate  *c,
	gint              dir,
	guint64           dev,
	gchar           **why,
	GError          **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) accounted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *id = axis_import_int_text(c->import_id);
	const gchar *params[1];
	struct stat st;
	Survey survey;
	guint64 smallest = 0;
	guint moved = 0;
	guint i;

	*why = NULL;
	if (c->source_inode == 0) {
		*why = g_strdup("Axis recorded nothing of the folder itself when it imported from it, so it cannot tell that it "
			"is still the folder imported from");
		return -1;
	}
	if (fstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || (guint64)st.st_ino != c->source_inode ||
	    (guint64)st.st_dev != dev) {
		*why = g_strdup("it is not the folder the import walked (another folder has its name now)");
		return 0;
	}
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT f.path, f.inode, f.role, f.accepted, f.kind, f.size, f.source_state,"
	        " f.dest_dir, f.dest_name, f.dest_inode, r.volume_id, r.rel_path,"
	        " (f.reasons @> '[{\"c\": \"sample\"}]' OR f.reasons @> '[{\"c\": \"extra\"}]' OR f.role = 'extra'), f.note"
	        " FROM import_file f JOIN import i ON i.id = f.import_id LEFT JOIN library_root r ON r.id = i.root_id"
	        " WHERE f.import_id = $1::bigint ORDER BY f.position", 1, params, &rows, error))
		return -2;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *path = axis_db_result_get_text(rows, i, 0);
		const gchar *role = axis_db_result_get_text(rows, i, 2);
		const gchar *state = axis_db_result_get_text(rows, i, 6);
		gboolean taken = role != NULL && g_strcmp0(axis_db_result_get_text(rows, i, 3), "t") == 0 &&
			(g_strcmp0(state, "moved") == 0 || g_strcmp0(state, "deleted") == 0 || g_strcmp0(state, "gone") == 0);
		gint64 inode = 0;
		gint64 size = 0;

		axis_db_result_get_int64(rows, i, 1, &inode);
		axis_db_result_get_int64(rows, i, 5, &size);
		/* A download's file a move across volumes kept (its copy in the
		 * library is not as recorded, or it is not the file copied): it may
		 * be the one copy left, and goes with nothing. */
		if (g_strcmp0(state, "kept") == 0 || g_strcmp0(state, "pending") == 0) {
			g_autofree gchar *shown = path != NULL ? axis_library_name_display(path) : g_strdup("?");
			const gchar *why_kept = axis_db_result_get_text(rows, i, 13);

			*why = g_strdup_printf("the import kept the download's file %s%s%s%s", path != NULL && path[0] != '\0' ?
				shown : "(the download itself)", why_kept != NULL ? " (" : "", why_kept != NULL ? why_kept : "",
				why_kept != NULL ? ")" : ", which it has not deleted yet");
			return 0;
		}
		if (path != NULL && inode > 0)
			g_hash_table_insert(accounted, g_strdup_printf("%s\n%" G_GUINT64_FORMAT, path, (guint64)inode),
				GINT_TO_POINTER(taken ? SURVEY_TAKEN : g_strcmp0(axis_db_result_get_text(rows, i, 12), "t") == 0 ?
				SURVEY_MAY_GO : SURVEY_LOOKED));
		if (g_strcmp0(axis_db_result_get_text(rows, i, 4), "video") != 0 || g_strcmp0(axis_db_result_get_text(rows, i,
		        3), "t") != 0 || (g_strcmp0(role, "film") != 0 && g_strcmp0(role, "episode") != 0) ||
		    (g_strcmp0(state, "moved") != 0 && g_strcmp0(state, "deleted") != 0 && g_strcmp0(state, "gone") != 0))
			continue;
		/* In the library, as the import put it. */
		{
			g_autoptr(AxisVolumeGuard) reader = NULL;
			g_autoptr(GString) dir_bytes = NULL;
			g_autoptr(GString) name_bytes = NULL;
			g_auto(GStrv) parts = NULL;
			gint64 volume = 0;
			gint64 dest_inode = 0;
			guint64 root_dev;
			gint current;
			guint k;
			gboolean there = FALSE;

			axis_db_result_get_int64(rows, i, 9, &dest_inode);
			axis_db_result_get_int64(rows, i, 10, &volume);
			if (dest_inode <= 0 || axis_db_result_get_text(rows, i, 7) == NULL ||
			    !axis_library_name_decode(axis_db_result_get_text(rows, i, 7), &dir_bytes) ||
			    !axis_library_name_decode(axis_db_result_get_text(rows, i, 8), &name_bytes) || name_bytes->len == 0 ||
			    memchr(dir_bytes->str, '\0', dir_bytes->len) != NULL ||
			    memchr(name_bytes->str, '\0', name_bytes->len) != NULL) {
				*why = g_strdup("Axis did not record where in the library a file it moved went");
				return -1;
			}
			reader = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
			if (reader == NULL) {
				*why = g_strdup("the library's volume cannot be looked at, to see that the files the import moved are "
					"in place.");
				return -3;
			}
			root_dev = axis_volume_guard_get_dev(reader);
			current = axis_volume_guard_open_dir(reader, axis_db_result_get_text(rows, i, 11) != NULL ?
				axis_db_result_get_text(rows, i, 11) : "", NULL);
			parts = g_strsplit(dir_bytes->str, "/", 140);
			for (k = 0; current >= 0 && parts[k] != NULL; k++) {
				gint next;

				if (parts[k][0] == '\0')
					continue;
				next = openat(current, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
				close(current);
				current = next;
				if (current >= 0 && (fstat(current, &st) != 0 || (guint64)st.st_dev != root_dev)) {
					close(current);
					current = -1;
				}
			}
			if (current >= 0) {
				there = fstatat(current, name_bytes->str, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
					(guint64)st.st_ino == (guint64)dest_inode && (guint64)st.st_dev == root_dev &&
					(g_strcmp0(state, "moved") != 0 || dest_inode == inode);
				close(current);
			}
			if (!there) {
				g_autofree gchar *stored = g_strconcat(axis_db_result_get_text(rows, i, 7), "/",
					axis_db_result_get_text(rows, i, 8), NULL);
				g_autofree gchar *shown = axis_library_name_display(stored);

				*why = g_strdup_printf("the file the import moved into the library (%s) is not there as it was placed, "
					"so Axis does not take this for the folder it imported from", shown);
				return 0;
			}
		}
		moved++;
		if (smallest == 0 || (guint64)size < smallest)
			smallest = (guint64)MAX(size, 0);
	}
	if (moved == 0) {
		*why = g_strdup("Axis recorded no video it moved out of it, to tell that it is still the folder imported from");
		return -1;
	}
	/* Nothing in it of feature size that Axis does not account for. */
	memset(&survey, 0, sizeof(survey));
	survey.accounted = accounted;
	survey.dev = dev;
	survey.threshold = MAX(MIN(smallest / 4, FEATURE_MIN_BYTES), (guint64)1);
	if (!survey_folder(&survey, dir, "", 0)) {
		if (survey.too_big) {
			*why = g_strdup_printf("it holds more than %u files and folders, or folders more than %u deep, which no "
				"download's folder does", (guint)AXIS_VOLUME_FOLDER_ENTRIES, (guint)AXIS_VOLUME_FOLDER_DEPTH);
		} else {
			g_autofree gchar *size_text = axis_size_format(survey.found_size);

			if (survey.found_class == SURVEY_LOOKED)
				*why = g_strdup_printf("it holds %s (%s), a %s the import looked at and did not take, which may be "
					"another download's, a second feature or a person's: only what the import took, its samples and "
					"small files like an .nfo go with the folder", survey.found, size_text, survey.found_video ? "video" :
					"file that is not media (a disc image or an archive?)");
			else if (survey.found_video)
				*why = g_strdup_printf("it holds a video the import did not take and Axis knows nothing of (%s, %s), "
					"which may be another download's or a person's", survey.found, size_text);
			else
				*why = g_strdup_printf("it holds a large file that is not media, which the import did not take and "
					"Axis knows nothing of (%s, %s: a disc image or an archive?), which may be another download's or a "
					"person's", survey.found, size_text);
		}
		g_free(survey.found);
		return 0;
	}
	return 1;
}

static gchar *
text_at(
	AxisDbResult *rows,
	guint         row,
	guint         column
){
	return g_strdup(axis_db_result_get_text(rows, row, column));
}

static gint64
int_at(
	AxisDbResult *rows,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (axis_db_result_is_null(rows, row, column) || !axis_db_result_get_int64(rows, row, column, &value))
		return 0;
	return value;
}

/* The roots of @volume and the places of other downloads and open imports
 * on it, for the rules. */
static gboolean
neighbours(
	AxisImport  *self,
	const Candidate *c,
	gint64       volume,
	AxisDbResult **roots,
	AxisDbResult **others,
	GError     **error
){
	g_autofree gchar *v = axis_import_int_text(volume);
	g_autofree gchar *d = axis_import_int_text(c->download_id);
	g_autofree gchar *i = axis_import_int_text(c->import_id);
	const gchar *params[3];

	params[0] = v;
	params[1] = d;
	params[2] = i;
	if (!axis_db_pool_exec(self->pool, "SELECT rel_path, media_type = 'downloads', name FROM library_root"
	        " WHERE volume_id = $1::bigint ORDER BY id", 1, params, roots, error))
		return FALSE;
	/* Torrents left at their clients (seeding, or moved by a person to
	 * another category or label: whatever their downloads' state says now) are
	 * still there too. */
	return axis_db_pool_exec(self->pool,
		"SELECT rel_path FROM download d WHERE volume_id = $1::bigint AND id <> $2::bigint AND rel_path IS NOT NULL"
		" AND (state IN ('sending', 'queued', 'downloading', 'paused', 'completed') OR (protocol = 'torrent'"
		"  AND EXISTS (SELECT 1 FROM import j WHERE j.download_id = d.id"
		"  AND j.client_removal = 'kept')))"
		" UNION ALL SELECT source_path FROM import WHERE source_volume_id = $1::bigint AND id <> $3::bigint"
		" AND state IN ('waiting', 'running', 'blocked', 'partial')", 3, params, others, error);
}

/* What a client lists now, asked once per run of the folder step. */
typedef struct
{
	GPtrArray *items;	/* NULL when it could not be asked */
	guint      unreadable;
	gchar     *unreadable_why;	/* which entry, and why */
	gchar     *failure;
} ClientNow;

static void
client_now_free(gpointer data)
{
	ClientNow *now = data;

	g_clear_pointer(&now->items, g_ptr_array_unref);
	g_free(now->unreadable_why);
	g_free(now->failure);
	g_free(now);
}

static const ClientNow *
client_now(
	AxisImport *self,
	GHashTable *cache,
	gint64      client_id
){
	ClientNow *now = g_hash_table_lookup(cache, &client_id);

	if (now == NULL) {
		gint64 *key = g_new(gint64, 1);
		GError *local = NULL;

		*key = client_id;
		now = g_new0(ClientNow, 1);
		if (!axis_downloads_client_items_full(self->downloads, client_id, &now->items, &now->unreadable,
		        &now->unreadable_why, &local)) {
			now->failure = g_strdup(local->message);
			g_error_free(local);
		}
		g_hash_table_insert(cache, key, now);
	}
	return now;
}

/*
 * Where the downloads its client has now are, on @volume, into @places (owned
 * strings): what it lists in every category or label and state, by the place it
 * says (a torrent in progress is in its save folder already), mapped through
 * its path mappings; not the import's own download.  One it keeps loose in
 * the client's folder for Axis itself (@category, the client's own path; a
 * torrent without a folder of its own) is named in *@loose.  What it lists
 * without a place, and the downloads Axis follows at it that it does not list
 * and that have no place recorded (one being sent), are by their names in
 * @names: nothing of those names is taken for what an import left.  When what
 * the client has is not all known (it cannot be asked, its list is not whole
 * or holds what Axis could not read, or its mappings put a download in its
 * folder for Axis somewhere else), *@unknown says why.  %FALSE with @error
 * for the database.
 */
static gboolean
client_places(
	AxisImport       *self,
	GHashTable       *cache,
	const Candidate  *c,
	gint64            volume,
	const gchar      *category,
	const gchar      *area,
	GPtrArray        *places,
	GPtrArray        *names,
	gchar           **loose,
	gchar           **unknown,
	GError          **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *client = axis_import_int_text(c->client_id);
	g_autofree gchar *download = axis_import_int_text(c->download_id);
	const ClientNow *now = client_now(self, cache, c->client_id);
	GPtrArray *items = now->items;
	const gchar *own_ref = NULL;
	const gchar *params[2];
	guint i;

	*loose = NULL;
	*unknown = NULL;
	if (now->failure != NULL) {
		*unknown = g_strdup_printf("its client could not be asked which downloads it has now (%s).", now->failure);
		return TRUE;
	}
	if (now->unreadable > 0) {
		*unknown = g_strdup_printf("its client has something Axis cannot read (%s), so a download of it may be in or "
			"around this folder.", now->unreadable_why != NULL ? now->unreadable_why : "its list was not whole");
		return TRUE;
	}
	params[0] = client;
	params[1] = download;
	if (!axis_db_pool_exec(self->pool, "SELECT client_ref, release_name, rel_path IS NOT NULL, id = $2::bigint"
	        " FROM download WHERE client_id = $1::bigint AND (id = $2::bigint OR state IN ('sending', 'queued',"
	        " 'downloading', 'paused', 'completed'))", 2, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		if (g_strcmp0(axis_db_result_get_text(rows, i, 3), "t") == 0)
			own_ref = axis_db_result_get_text(rows, i, 0);
	}
	for (i = 0; i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);
		const gchar *place = item->place != NULL ? item->place : item->path;
		g_autofree gchar *normal = NULL;
		g_autofree gchar *rel = NULL;
		g_autofree gchar *problem = NULL;
		gint64 mapped_volume = 0;

		if (own_ref != NULL && g_strcmp0(item->client_ref, own_ref) == 0)
			continue;
		/* By its name exactly as the client gives it, when it says no
		 * place; what Axis cannot read exactly tells nothing. */
		if (place == NULL) {
			if (item->own_name != NULL)
				g_ptr_array_add(names, g_strdup(item->own_name));
			else if (*unknown == NULL)
				*unknown = g_strdup_printf("its client has a download (%s) whose name Axis cannot read exactly and whose "
					"place it does not say, so it may be in or around this folder.", item->name);
			continue;
		}
		if (!axis_storage_path_normalize_exact(place, TRUE, &normal, NULL)) {
			if (*unknown == NULL)
				*unknown = g_strdup_printf("its client has a download (%s) whose place Axis cannot read exactly, so it "
					"may be in or around this folder.", item->name);
			continue;
		}
		/* Its files loose in the client's folder for Axis itself. */
		if (category != NULL && axis_storage_path_contains(normal, category)) {
			if (*loose == NULL)
				*loose = g_strdup(item->name);
			continue;
		}
		if (!axis_downloads_map_client_path(self->downloads, c->client_id, normal, &mapped_volume, &rel, NULL,
		        &problem, error) || mapped_volume != volume) {
			if (error != NULL && *error != NULL)
				return FALSE;
			if (category != NULL && axis_storage_path_contains(category, normal) && *unknown == NULL)
				*unknown = g_strdup_printf("its client has %s in its folder for Axis, and its path mappings put it "
					"somewhere else, so Axis cannot tell where it is.", item->name);
			continue;
		}
		g_ptr_array_add(places, g_steal_pointer(&rel));
	}
	/* Followed by Axis, not listed by the client, no place recorded. */
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *ref = axis_db_result_get_text(rows, i, 0);
		gboolean listed = FALSE;
		guint k;

		if (g_strcmp0(axis_db_result_get_text(rows, i, 2), "t") == 0 ||
		    g_strcmp0(axis_db_result_get_text(rows, i, 3), "t") == 0)
			continue;
		for (k = 0; ref != NULL && k < items->len; k++) {
			if (g_strcmp0(((const AxisDownloadItem *)g_ptr_array_index(items, k))->client_ref, ref) == 0)
				listed = TRUE;
		}
		if (!listed && axis_db_result_get_text(rows, i, 1) != NULL)
			g_ptr_array_add(names, g_strdup(axis_db_result_get_text(rows, i, 1)));
	}
	/* What the other clients whose path mappings reach the client's folder for
	 * Axis (or, without one, what would go) have in it or around it. */
	{
		AxisImportOthers others;

		if (!axis_import_others(self, c->client_id, volume, area, &others, error)) {
			axis_import_others_clear(&others);
			return FALSE;
		}
		for (i = 0; i < others.places->len; i++)
			g_ptr_array_add(places, g_strdup(g_ptr_array_index(others.places, i)));
		for (i = 0; i < others.names->len; i++)
			g_ptr_array_add(names, g_strdup(g_ptr_array_index(others.names, i)));
		for (i = 0; i < others.unplaced->len; i++)
			g_ptr_array_add(names, g_strdup(g_ptr_array_index(others.unplaced, i)));
		if (others.loose->len > 0 && *loose == NULL)
			*loose = g_strdup(g_ptr_array_index(others.loose, 0));
		if (others.shared != NULL && *unknown == NULL)
			*unknown = g_strdup_printf("%s.", others.shared);
		else if (others.undetermined != NULL && *unknown == NULL)
			*unknown = g_strdup_printf("%s.", others.undetermined);
		else if (others.unknown != NULL && *unknown == NULL)
			*unknown = g_strdup_printf("%s.", others.unknown);
		axis_import_others_clear(&others);
	}
	return TRUE;
}

/*
 * What folder_decide() found may go, for folder_delete(): the volume and what
 * is removed there.  When @go is %FALSE, what became of the folder is said
 * already (left, waiting, gone: recorded, or kept in Subject.dry).
 */
typedef struct
{
	gboolean                go;
	gint64                  volume;
	AxisImportFolderTarget  target;
	guint64                 seen_inode;	/* of what is at the reported place; 0 for nothing there */
	/* For a torrent its client is to delete (Candidate.at_client), whatever
	 * @go says.  @client_agrees: the client said, at this look, that the
	 * torrent is at the place recorded when it completed, in a folder or file
	 * named as the torrent is.  @absent: nothing is at the recorded place.
	 * @elsewhere: (nullable) where the client says the torrent is now, when
	 * that is not the recorded place. */
	gboolean                client_agrees;
	gboolean                absent;
	gchar                  *elsewhere;
} Decision;

#define DECISION_INIT { FALSE, 0, { FALSE, NULL, NULL, NULL, FALSE }, 0, FALSE, FALSE, NULL }

static void
decision_clear(Decision *decision)
{
	axis_import_folder_target_clear(&decision->target);
	g_clear_pointer(&decision->elsewhere, g_free);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Decision, decision_clear)

/*
 * The first half of a leftover's removal: everything that decides, up to and
 * including the looks through the read guard and what the clients have now.
 * It deletes nothing.  The confirmation of "Delete this download" calls this
 * half alone (dry), so that it shows the decision the deletion makes.
 */
static gboolean
folder_decide(
	AxisImport               *self,
	GHashTable               *cache,
	const Candidate          *c,
	AxisImportCleanupReport  *report,
	Decision                 *decision,
	GError                  **error
){
	g_autoptr(AxisDbResult) roots_rows = NULL;
	g_autoptr(AxisDbResult) other_rows = NULL;
	g_autoptr(GPtrArray) roots = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) others = g_ptr_array_new();
	g_autoptr(GPtrArray) item_places = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) item_names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) local = NULL;
	g_autofree gchar *category = NULL;
	g_autofree gchar *mapped = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *why = NULL;
	g_auto(AxisImportFolderTarget) target = { FALSE, NULL, NULL, NULL, FALSE };
	g_autofree gchar *no_folder_why = NULL;
	AxisImportFolderFacts facts;
	gint64 volume = 0;
	guint i;

	decision->go = FALSE;
	if (c->download_id == 0)
		return left(self, &c->who, "Axis no longer has the record of its download", error);
	if (c->torrent && !c->at_client) {
		AxisImportSettings settings;
		gboolean seeding;

		/* A torrent's files go with its client, removed with its data; Axis
		 * deletes what is left itself only when the client no longer has it,
		 * or let go of it before the import moved its files. */
		if (g_strcmp0(c->client_removal, "gone") != 0 && !(c->moved && g_strcmp0(c->client_removal, "done") == 0))
			return left(self, &c->who, "a torrent's files go with its client, as the import settings say; Axis "
				"deletes them itself only when the client no longer has the torrent", error);
		axis_import_settings_read(self, &settings);
		/* The setting is about what Axis does by itself after an import: a
		 * deletion a person asked for, of a download that was not imported,
		 * sets it aside (and nothing else). */
		seeding = !settings.torrents_remove && !c->asked;
		axis_import_settings_clear(&settings);
		if (seeding)
			return left(self, &c->who, "the import settings leave torrents seeding (\"After a torrent is "
				"imported\" is seed), so Axis deletes none of a torrent's files", error);
	} else if (c->torrent) {
		/* Its client deletes it: judged below, before the client is asked. */
	} else if (g_strcmp0(c->protocol, "usenet") != 0) {
		return left(self, &c->who, "Axis does not know what kind of download it was", error);
	}
	if (g_strcmp0(c->client_removal, "kept") == 0)
		return left(self, &c->who, "its client did not remove the job, so the folder is still the client's",
			error);
	if (c->client_id == 0)
		return left(self, &c->who, "its client is no longer set up in Axis, so where the client keeps Axis's "
			"downloads cannot be asked", error);
	/* A download removed outside an import: nothing is deleted unless the
	 * client said where its files were when it completed and Axis recorded
	 * what was there; the page says where they may be. */
	if (c->who.download && (c->client_path == NULL || c->recorded_path == NULL || c->recorded_volume == 0 ||
	    (c->location_inode == 0 && !c->at_client)))
		return left(self, &c->who, c->client_path == NULL ? "its client never said where its files were (it had not "
			"finished, as far as Axis saw, when it was removed): if it finished at the client, they are in the "
			"client's folder for Axis's category, under the download's name, and can be deleted there by hand" :
			"Axis recorded nothing of what was at the place its client reported when it completed, so it cannot tell "
			"what is there now from something else of that name; it can be deleted at the client by hand", error);
	if (c->client_path == NULL || c->recorded_path == NULL || c->recorded_volume == 0)
		return left(self, &c->who, "its client never said where its files were", error);

	/* Where the client keeps Axis's category (or label), as it says now. */
	if (!axis_downloads_category_folder(self->downloads, c->client_id, &category, &local)) {
		g_autofree gchar *text = NULL;

		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND)) {
			/* Outside an import, never without a folder of Axis's own (a
			 * torrent its client is to delete itself aside: that folder is
			 * what tells Axis's own deletions apart, and Axis makes none). */
			if ((!c->torrent || c->who.download) && !c->at_client) {
				g_autofree gchar *shown = NULL;

				if (c->who.download)
					axis_storage_path_normalize(c->client_path, TRUE, &shown, NULL);
				text = g_strdup_printf("the client keeps Axis's %s in no folder Axis may delete in (%s)%s%s%s",
					c->torrent ? "torrents" : "category", local->message, shown != NULL ? "; it is at " : "",
					shown != NULL ? shown : "", shown != NULL ? " at the client and can be deleted there by hand" : "");
				return left(self, &c->who, text, error);
			}
			/* A torrent with no folder of Axis's own: judged as such. */
			no_folder_why = g_strdup(local->message);
			g_clear_error(&local);
		} else {
			text = g_strdup_printf("its client could not be asked where it keeps Axis's downloads (%s).",
				local->message);
			return waits(self, &c->who, c->torrent, text, error);
		}
	}
	/* Its path, through the client's mapping as it is now. */
	if (!axis_downloads_map_client_path(self->downloads, c->client_id, c->client_path, &volume, &mapped, &prefix,
	        &problem, &local)) {
		g_autofree gchar *text = NULL;

		if (local != NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		text = g_strdup_printf("the path its client reported for it no longer maps onto a volume (%s)", problem);
		return left(self, &c->who, text, error);
	}
	if (!neighbours(self, c, volume, &roots_rows, &other_rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(roots_rows); i++) {
		AxisImportFolderRoot *root = g_new0(AxisImportFolderRoot, 1);

		root->rel_path = axis_db_result_get_text(roots_rows, i, 0);
		root->downloads = g_strcmp0(axis_db_result_get_text(roots_rows, i, 1), "t") == 0;
		root->name = axis_db_result_get_text(roots_rows, i, 2);
		g_ptr_array_add(roots, root);
	}
	for (i = 0; i < axis_db_result_get_n_rows(other_rows); i++)
		g_ptr_array_add(others, (gpointer)axis_db_result_get_text(other_rows, i, 0));
	memset(&facts, 0, sizeof(facts));
	facts.client_path = c->client_path;
	facts.category_folder = category;
	facts.map_prefix = prefix;
	facts.mapped_volume = volume;
	facts.mapped_path = mapped;
	facts.recorded_volume = c->recorded_volume;
	facts.recorded_path = c->recorded_path;
	facts.source_volume = c->source_volume;
	facts.source_path = c->source_path;
	facts.roots = roots;
	facts.others = others;
	facts.torrent = c->torrent;
	facts.no_folder_why = no_folder_why;
	facts.client_deletes = c->at_client;

	/* A torrent its client is to delete: only when the client says, now, that
	 * it is at the place recorded when it completed (a person, or the client,
	 * may have moved it since: into a library folder, for one).  Asked before
	 * the place is looked at, so that nothing found there, or not found there
	 * ("gone"), is said of a torrent the client has somewhere else: a client
	 * asked to remove a torrent with its data deletes it wherever it is.  No
	 * caller but the look of "Delete this download" sets at_client. */
	if (c->at_client) {
		const ClientNow *now = client_now(self, cache, c->client_id);
		g_autofree gchar *recorded = NULL;
		const AxisDownloadItem *own = NULL;

		for (i = 0; now->items != NULL && c->client_ref != NULL && i < now->items->len; i++) {
			const AxisDownloadItem *item = g_ptr_array_index(now->items, i);

			if (g_strcmp0(item->client_ref, c->client_ref) == 0)
				own = item;
		}
		if (own == NULL)
			return waits(self, &c->who, TRUE, "its client does not list the torrent now, so Axis cannot ask it where "
				"the torrent's files are.", error);
		if (!axis_storage_path_normalize(c->client_path, TRUE, &recorded, NULL))
			return left(self, &c->who, "the path its client reported for it is not a full path", error);
		{
			const gchar *said = own->path != NULL ? own->path : own->place;
			g_autofree gchar *normal = NULL;

			if (said == NULL || !axis_storage_path_normalize_exact(said, TRUE, &normal, NULL) ||
			    strcmp(normal, recorded) != 0) {
				g_autofree gchar *text = g_strdup_printf("the torrent is no longer where Axis recorded it: its client "
					"says its files are at %s now, which is not the place recorded when it completed (%s). It was "
					"probably moved in the client. Axis will not delete it, and does not ask the client to: remove it in "
					"the client, or dismiss the import", said != NULL ? said : "no place it names", recorded);

				decision->elsewhere = g_strdup(said != NULL ? said : "");
				return left(self, &c->who, text, error);
			}
			/* Never the client's own download location itself: a torrent with
			 * no folder of its own (several files put straight into the
			 * client's download folder) is reported at that folder, and the
			 * client would be asked about a place that holds other things.
			 * The torrent's own folder or file is named as the torrent is. */
			{
				g_autofree gchar *base = g_path_get_basename(normal);

				if (own->own_name == NULL || strcmp(base, own->own_name) != 0) {
					g_autofree gchar *text = g_strdup_printf("the place its client reports (%s) is not named as the "
						"torrent is, so it may be the client's download folder itself and not the torrent's own folder "
						"or file; it can be removed at the client by hand", normal);

					return left(self, &c->who, text, error);
				}
			}
		}
		decision->client_agrees = TRUE;
	}

	/* What is at the reported place, looked at through a read guard (which
	 * storage read-only allows): a folder, or a file (as SABnzbd reports a
	 * job of one file). */
	{
		g_autoptr(AxisVolumeGuard) reader = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ,
			&local);
		g_autofree gchar *seen_why = NULL;
		AxisImportFolderShape shape;
		guint64 ino = 0;
		Seen seen;

		if (reader == NULL) {
			if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
				return left(self, &c->who, "its volume is no longer known", error);
			return waits(self, &c->who, c->torrent, local->message, error);
		}
		seen = look_at(reader, mapped, &ino, &seen_why);
		if (seen == SEEN_REFUSED)
			return left(self, &c->who, seen_why, error);
		if (seen == SEEN_WAIT)
			return waits(self, &c->who, c->torrent, seen_why, error);
		decision->seen_inode = seen == SEEN_ABSENT ? 0 : ino;
		/* Nothing there, and its client still lists the torrent: it is
		 * somewhere, and not where Axis recorded it, whatever place the client
		 * names (its files were moved at the client, or outside it).  Never
		 * "gone", which lets the import's download be deleted: the client,
		 * asked to remove it with its data, would delete it where it is. */
		if (seen == SEEN_ABSENT && c->at_client) {
			decision->absent = TRUE;
			return left(self, &c->who, "the torrent is no longer where Axis recorded it: nothing is at that place, "
				"and its client still lists the torrent. It was probably moved in the client. Axis will not delete it, "
				"and does not ask the client to: remove it in the client, or dismiss the import", error);
		}
		/* Nothing there: is its job's folder gone too? */
		shape = seen == SEEN_FOLDER ? AXIS_IMPORT_FOLDER_SHAPE_FOLDER : AXIS_IMPORT_FOLDER_SHAPE_FILE;
		if (axis_import_folder_judge(&facts, shape, &target, &why) != AXIS_IMPORT_FOLDER_OK)
			return left(self, &c->who, why, error);
		if (seen == SEEN_ABSENT) {
			g_autofree gchar *again_why = NULL;

			if (!target.file)
				seen = look_at(reader, target.path, &ino, &again_why);
			if (target.file || seen == SEEN_ABSENT) {
				if (report != NULL)
					report->gone++;
				return record(self, &c->who, "gone", c->moved ? "It was already gone from the client's folder: "
					"nothing of it is left there (what the import took was moved into the library)." : target.file ?
					"Its file at the client was already gone." : "Its folder at the client was already gone.", error);
			}
			if (seen == SEEN_WAIT)
				return waits(self, &c->who, c->torrent, again_why, error);
			/* The reported file was moved out of its job's folder: the folder is
			 * looked at below, by what a move leaves to tell it. */
			if (!(c->moved && seen == SEEN_FOLDER))
				return left(self, &c->who, seen == SEEN_REFUSED ? again_why : "the file Axis imported from its "
					"folder is not in it any more, so it may not be the folder imported from", error);
		}
	}

	/* What the client has now, in progress too, as it says (asked once a run):
	 * a download of it in or around what would go keeps it; one whose place
	 * it does not say keeps a folder of its name; one it keeps loose in its
	 * folder for Axis could be a file there, not a job's own folder (whose
	 * identity the rules below prove: the inode recorded when the import
	 * walked it, the import's own files in it or where they went); and when
	 * what it has is not all known, nothing goes until it is. */
	{
		g_autofree gchar *loose = NULL;
		g_autofree gchar *unknown = NULL;
		g_autofree gchar *base = g_path_get_basename(target.path);
		g_autofree gchar *area = NULL;

		/* The client's folder for Axis on the volume (what the other clients
		 * are asked about), or, without one, what would go. */
		if (category != NULL) {
			g_autofree gchar *area_problem = NULL;
			gint64 area_volume = 0;

			if (!axis_downloads_map_client_path(self->downloads, c->client_id, category, &area_volume, &area, NULL,
			        &area_problem, error) && error != NULL && *error != NULL)
				return FALSE;
			if (area != NULL && area_volume != volume)
				g_clear_pointer(&area, g_free);
		}
		if (area == NULL)
			area = g_strdup(target.path);
		if (!client_places(self, cache, c, volume, category, area, item_places, item_names, &loose, &unknown, error))
			return FALSE;
		if (unknown != NULL)
			return waits(self, &c->who, c->torrent, unknown, error);
		for (i = 0; i < item_places->len; i++) {
			const gchar *other = g_ptr_array_index(item_places, i);

			if (axis_storage_path_contains(target.path, other) || axis_storage_path_contains(other, target.path))
				return left(self, &c->who, "another download Axis knows that is still at its client, or an import "
					"still open, is in it or around it", error);
		}
		for (i = 0; i < item_names->len; i++) {
			if (g_strcmp0(g_ptr_array_index(item_names, i), base) == 0)
				return left(self, &c->who, "a download still at its client, whose place it does not say yet, has this "
					"folder's name and may be in it", error);
		}
		if (loose != NULL && target.file) {
			g_autofree gchar *text = g_strdup_printf("its client keeps %s loose in its folder for Axis (with no folder "
				"of its own), and this file may be one of its files: Axis looks again once that download is done there.",
				loose);

			return waits(self, &c->who, c->torrent, text, error);
		}
	}

	decision->go = TRUE;
	decision->volume = volume;
	decision->target = target;
	memset(&target, 0, sizeof(target));
	return TRUE;
}

/*
 * The second half: through a write guard of the volume, the checks made
 * again, and the removal.  What goes is @decision's, from folder_decide().
 */
static gboolean
folder_delete(
	AxisImport               *self,
	const Candidate          *c,
	const Decision           *decision,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	const AxisImportFolderTarget target = decision->target;
	const gint64 volume = decision->volume;
	AxisVolumeFolderOutcome outcome;
	struct stat st;
	guint removed = 0;
	gint pfd;
	gint dir;
	gint same;

	/* Never for a torrent its client is to delete: Axis deletes nothing of
	 * it. */
	if (c->at_client)
		return TRUE;
	/* The volume, for writing: not while storage is read-only, nor while it
	 * is offline or mounted read-only. */
	guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_WRITE, &local);
	if (guard == NULL) {
		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
			return left(self, &c->who, "its volume is no longer known", error);
		return waits(self, &c->who, c->torrent, local->message, error);
	}
	/* Through it, again: what goes, and that it is what was imported from. */
	split_last(target.path, &parent, &last);
	pfd = axis_volume_guard_open_dir(guard, parent, &local);
	if (pfd < 0) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
		    g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
			if (report != NULL)
				report->gone++;
			return record(self, &c->who, "gone", target.file ? "Its file at the client was already gone." :
				"Its folder at the client was already gone.", error);
		}
		if (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_NOT_CONTAINED ||
		    local->code == AXIS_STORAGE_ERROR_INVALID))
			return left_as(self, &c->who, target.file, "the way to it passes a symbolic link or another "
				"filesystem, which Axis never follows", error);
		return waits(self, &c->who, c->torrent, local->message, error);
	}
	if (fstatat(pfd, last, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		gint err = errno;

		close(pfd);
		if (err == ENOENT) {
			if (report != NULL)
				report->gone++;
			return record(self, &c->who, "gone", target.file ? "Its file at the client was already gone." :
				"Its folder at the client was already gone.", error);
		}
		{
			g_autofree gchar *text = g_strdup_printf("it cannot be looked at (%s).", g_strerror(err));

			return waits(self, &c->who, c->torrent, text, error);
		}
	}
	if (S_ISLNK(st.st_mode)) {
		close(pfd);
		return left_as(self, &c->who, target.file, "it is a symbolic link, which Axis never follows", error);
	}
	if (target.file ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)) {
		close(pfd);
		return left_as(self, &c->who, target.file, "what is at its place changed while Axis looked at it", error);
	}
	if ((guint64)st.st_dev != axis_volume_guard_get_dev(guard)) {
		close(pfd);
		return left_as(self, &c->who, target.file, "it is another filesystem than its volume", error);
	}
	/* Still what was imported from: a file it gave is there, the same.  For
	 * a download removed outside an import: what the client reported is
	 * still the very file or folder that was there when it completed. */
	if (c->moved && target.file) {
		/* The import moved that file: what is at its place now is something
		 * else, not Axis's. */
		close(pfd);
		return left_as(self, &c->who, TRUE, "a file is at the place of the one the import moved into the library, and "
			"it is not that file (the download's file of a move across volumes kept, or a new one)", error);
	} else if (c->who.download && target.file) {
		same = (guint64)st.st_ino == c->location_inode ? 1 : 0;
	} else if (target.file) {
		same = still_imported_from(self, c->import_id, pfd, last, axis_volume_guard_get_dev(guard), error);
	} else {
		dir = openat(pfd, last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		if (dir < 0) {
			g_autofree gchar *text = g_strdup_printf("it cannot be opened (%s).", g_strerror(errno));

			close(pfd);
			return waits(self, &c->who, c->torrent, text, error);
		}
		if (c->moved) {
			g_autofree gchar *moved_why = NULL;

			/* The files it took are not in it: what tells it is the folder
			 * imported from is moved_identity()'s. */
			if (target.inner != NULL && target.inner[0] != '\0') {
				close(dir);
				close(pfd);
				return left(self, &c->who, "the import took the one file its client reported, not the job's folder "
					"around it, so Axis recorded nothing of that folder to tell it is still the one imported from",
					error);
			}
			same = moved_identity(self, c, dir, axis_volume_guard_get_dev(guard), &moved_why, error);
			close(dir);
			if (same == -3) {
				close(pfd);
				return waits(self, &c->who, c->torrent, moved_why, error);
			}
			if (same == -1 || same == 0) {
				close(pfd);
				return left(self, &c->who, moved_why, error);
			}
		} else if (c->who.download) {
			same = inode_at(dir, target.inner, axis_volume_guard_get_dev(guard), c->location_inode);
			close(dir);
		} else {
			same = still_imported_from(self, c->import_id, dir, target.inner, axis_volume_guard_get_dev(guard), error);
			close(dir);
		}
	}
	if (same == -2) {
		close(pfd);
		return FALSE;
	}
	if (same == -1) {
		close(pfd);
		return left_as(self, &c->who, target.file, "Axis recorded no file of it to tell that it is still what it "
			"imported from", error);
	}
	if (same == 0 && c->who.download) {
		close(pfd);
		return left_as(self, &c->who, target.file, "what is at its place is not what the client reported there when "
			"the download completed (another file or folder has its name now)", error);
	}
	if (same == 0) {
		close(pfd);
		return left_as(self, &c->who, target.file, target.file ? "the file there is not the one Axis imported "
			"(another file has its name now)" : "the file Axis imported from it is not in it any more, so it may not "
			"be the folder imported from", error);
	}

	if (target.file) {
		AxisVolumeRecycledOutcome gone;

		/* A torrent's file in a folder that is not Axis's own: only while the
		 * library still has what the import made of it. */
		if (target.loose) {
			g_autofree gchar *where = NULL;
			g_autofree gchar *copy_why = NULL;
			gint present = library_copy_present(self, c->import_id, &where, &copy_why, error);

			if (present == -2) {
				close(pfd);
				return FALSE;
			}
			if (present == -3) {
				close(pfd);
				return waits(self, &c->who, c->torrent, copy_why, error);
			}
			if (present <= 0) {
				g_autofree gchar *text = present < 0 ?
					g_strdup("Axis recorded no file it placed in the library from it, so it may be the one copy left") :
					g_strdup_printf("the file the import placed in the library (%s) is not there any more, so the "
						"download's file, which may be the one copy left, is kept", where);

				close(pfd);
				return left_as(self, &c->who, TRUE, text, error);
			}
		}
		if (!axis_volume_write_remove_file(guard, pfd, last, (guint64)st.st_ino, &gone, &local)) {
			close(pfd);
			return waits(self, &c->who, c->torrent, local->message, error);
		}
		close(pfd);
		if (gone == AXIS_VOLUME_RECYCLED_REMOVED) {
			g_autofree gchar *text = target.loose ?
				g_strdup_printf("Its file at the client (%s) was deleted: the torrent's one file, the very one imported, "
					"whose copy in the library is in place. Its client keeps Axis's torrents in no folder of their own, "
					"so nothing else there was touched.", target.client_path) :
				g_strdup_printf("Its file at the client (%s), which the client put straight into the folder of Axis's "
					"%s, was deleted; that folder stays.", target.client_path, c->torrent ? "label or category" :
					"category");

			if (report != NULL)
				report->folders++;
			return record(self, &c->who, "done", text, error);
		}
		if (gone == AXIS_VOLUME_RECYCLED_GONE) {
			if (report != NULL)
				report->gone++;
			return record(self, &c->who, "gone", "Its file at the client was already gone.", error);
		}
		return left_as(self, &c->who, TRUE, "what is at its place changed while Axis looked at it", error);
	}

	if (!axis_volume_write_remove_folder(guard, pfd, last, (guint64)st.st_ino, &outcome, &removed, &local)) {
		g_autofree gchar *text = NULL;

		close(pfd);
		if (removed > 0)
			text = g_strdup_printf("it was partly deleted (%u files and folders) and then stopped: %s", removed,
				local->message);
		return waits(self, &c->who, c->torrent, text != NULL ? text : local->message, error);
	}
	close(pfd);
	switch (outcome) {
	case AXIS_VOLUME_FOLDER_REMOVED: {
		g_autofree gchar *text = c->moved ?
			g_strdup_printf("Its folder at the client (%s) was deleted, with the %u files and folders left in it that "
				"the import did not take (an .nfo, a sample and the like); what it took had been moved into the "
				"library.", target.client_path, removed) :
			g_strdup_printf("Its folder at the client (%s) was deleted, with the %u files and folders the %s left in "
				"it.", target.client_path, removed, c->torrent ? "torrent" : "client");

		if (report != NULL)
			report->folders++;
		return record(self, &c->who, "done", text, error);
	}
	case AXIS_VOLUME_FOLDER_GONE:
		if (report != NULL)
			report->gone++;
		return record(self, &c->who, "gone", "Its folder at the client was already gone.", error);
	case AXIS_VOLUME_FOLDER_NOT_FOLDER:
		return left(self, &c->who, "what is at its place is not a folder (a symbolic link is never followed)",
			error);
	case AXIS_VOLUME_FOLDER_OTHER_FS:
		return left(self, &c->who, "another filesystem is mounted in it", error);
	case AXIS_VOLUME_FOLDER_TOO_BIG: {
		g_autofree gchar *text = g_strdup_printf("it holds more than %u files and folders, or folders more than %u "
			"deep, which no download's folder does", (guint)AXIS_VOLUME_FOLDER_ENTRIES, (guint)AXIS_VOLUME_FOLDER_DEPTH);

		return left(self, &c->who, text, error);
	}
	default:
		return left(self, &c->who, "what is at its place changed while Axis looked at it", error);
	}
}

/* A leftover's removal: the decision, then the deletion. */
static gboolean
one_folder(
	AxisImport               *self,
	GHashTable               *cache,
	const Candidate          *c,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_auto(Decision) decision = DECISION_INIT;

	if (!folder_decide(self, cache, c, report, &decision, error))
		return FALSE;
	if (!decision.go)
		return TRUE;
	return folder_delete(self, c, &decision, report, error);
}

/* ── A look that deletes nothing ("Delete this download") ───────── */

void
axis_import_folder_look_clear(AxisImportFolderLook *look)
{
	if (look == NULL)
		return;
	g_clear_pointer(&look->state, g_free);
	g_clear_pointer(&look->text, g_free);
	g_clear_pointer(&look->path, g_free);
	g_clear_pointer(&look->client_path, g_free);
	g_clear_pointer(&look->download_state, g_free);
	g_clear_pointer(&look->client_ref, g_free);
}

static void
look_says(
	AxisImportFolderLook *look,
	const gchar          *state,
	const gchar          *text
){
	g_free(look->state);
	g_free(look->text);
	look->state = g_strdup(state);
	look->text = g_strdup(text);
}

typedef struct
{
	guint64  dev;
	guint    seen;
	guint    files;
	guint64  bytes;
	gboolean too_big;
	gboolean other_fs;
} Measure;

/* Counts what is in @dir, bounded as the removal is, never following a link
 * and never entering another filesystem: %FALSE when it stopped. */
static gboolean
measure_folder(
	Measure *m,
	gint     dir,
	guint    depth
){
	gint copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR *stream = copy >= 0 ? fdopendir(copy) : NULL;
	g_autoptr(GPtrArray) folders = g_ptr_array_new_with_free_func(g_free);
	struct dirent *entry;
	gboolean ok = TRUE;
	guint i;

	if (stream == NULL) {
		if (copy >= 0)
			close(copy);
		return TRUE;
	}
	while (ok && (entry = readdir(stream)) != NULL) {
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (++m->seen > AXIS_VOLUME_FOLDER_ENTRIES) {
			m->too_big = TRUE;
			ok = FALSE;
			break;
		}
		if (fstatat(dirfd(stream), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if ((guint64)st.st_dev != m->dev) {
				m->other_fs = TRUE;
				ok = FALSE;
			} else {
				g_ptr_array_add(folders, g_strdup(entry->d_name));
			}
			continue;
		}
		if (S_ISREG(st.st_mode)) {
			m->files++;
			m->bytes += (guint64)MAX(st.st_size, 0);
		}
	}
	closedir(stream);
	for (i = 0; ok && i < folders->len; i++) {
		gint child;

		if (depth + 1 > AXIS_VOLUME_FOLDER_DEPTH) {
			m->too_big = TRUE;
			return FALSE;
		}
		child = openat(dir, g_ptr_array_index(folders, i), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		if (child < 0)
			continue;
		ok = measure_folder(m, child, depth + 1);
		close(child);
	}
	return ok;
}

/*
 * What would go, looked at through a read guard: that it is still what the
 * client reported when the download completed (the inode recorded then, as
 * folder_delete() asks again through the write guard), and how many files
 * and bytes it holds, within the removal's bounds.  What refuses, or cannot
 * be told now, is said in @look.
 */
static void
folder_measure(
	AxisImport            *self,
	const Candidate       *c,
	const Decision        *decision,
	AxisImportFolderLook  *look
){
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	struct stat st;
	guint64 dev;
	gint pfd;
	gint dir;

	reader = axis_volume_guard_acquire(self->storage, decision->volume, AXIS_VOLUME_INTENT_READ, &local);
	if (reader == NULL) {
		look_says(look, "waiting", local->message);
		return;
	}
	dev = axis_volume_guard_get_dev(reader);
	split_last(decision->target.path, &parent, &last);
	pfd = axis_volume_guard_open_dir(reader, parent, &local);
	if (pfd < 0) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
		    g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
			look_says(look, "gone", "Nothing is at its place any more.");
		else if (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_NOT_CONTAINED ||
		    local->code == AXIS_STORAGE_ERROR_INVALID))
			look_says(look, "left", "the way to it passes a symbolic link or another filesystem, which Axis never "
				"follows");
		else
			look_says(look, "waiting", local->message);
		return;
	}
	if (fstatat(pfd, last, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		gint err = errno;

		close(pfd);
		if (err == ENOENT)
			look_says(look, "gone", "Nothing is at its place any more.");
		else
			look_says(look, "waiting", g_strerror(err));
		return;
	}
	if (S_ISLNK(st.st_mode)) {
		close(pfd);
		look_says(look, "left", "it is a symbolic link, which Axis never follows");
		return;
	}
	if ((decision->target.file ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)) || (guint64)st.st_dev != dev) {
		close(pfd);
		look_says(look, "left", (guint64)st.st_dev != dev ? "it is another filesystem than its volume" :
			"what is at its place changed while Axis looked at it");
		return;
	}
	if (decision->target.file) {
		close(pfd);
		if (c->location_inode != 0 && (guint64)st.st_ino != c->location_inode) {
			look_says(look, "left", "what is at its place is not what the client reported there when the download "
				"completed (another file or folder has its name now)");
			return;
		}
		look->n_files = 1;
		look->bytes = (guint64)MAX(st.st_size, 0);
		return;
	}
	dir = openat(pfd, last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	close(pfd);
	if (dir < 0) {
		look_says(look, "waiting", g_strerror(errno));
		return;
	}
	if (c->location_inode != 0 && inode_at(dir, decision->target.inner, dev, c->location_inode) != 1) {
		close(dir);
		look_says(look, "left", "what is at its place is not what the client reported there when the download "
			"completed (another file or folder has its name now)");
		return;
	}
	{
		Measure m;

		memset(&m, 0, sizeof(m));
		m.dev = dev;
		if (!measure_folder(&m, dir, 0)) {
			g_autofree gchar *text = m.other_fs ? g_strdup("another filesystem is mounted in it") :
				g_strdup_printf("it holds more than %u files and folders, or folders more than %u deep, which no "
					"download's folder does", (guint)AXIS_VOLUME_FOLDER_ENTRIES, (guint)AXIS_VOLUME_FOLDER_DEPTH);

			close(dir);
			look_says(look, "left", text);
			return;
		}
		look->n_files = m.files;
		look->bytes = m.bytes;
	}
	close(dir);
}

gboolean
axis_import_download_folder_look(
	AxisImport            *self,
	gint64                 download_id,
	gint64                 import_id,
	AxisImportFolderLook  *look,
	GError               **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, client_now_free);
	g_auto(Decision) decision = DECISION_INIT;
	g_autofree gchar *id = axis_import_int_text(download_id);
	const gchar *params[1];
	const ClientNow *now;
	gboolean listed = FALSE;
	gboolean ok;
	Candidate c;
	Said said = { NULL, NULL };
	guint i;

	memset(look, 0, sizeof(*look));
	params[0] = id;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT d.id, d.volume_id, d.rel_path, d.state, d.protocol, d.client_id, d.client_path, d.location_inode,"
	        " d.release_name, d.client_ref FROM download d WHERE d.id = $1::bigint", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0) {
		look_says(look, "left", "Axis no longer has the record of its download");
		return TRUE;
	}
	memset(&c, 0, sizeof(c));
	c.download_id = int_at(rows, 0, 0);
	c.source_volume = int_at(rows, 0, 1);
	c.source_path = text_at(rows, 0, 2);
	c.protocol = text_at(rows, 0, 4);
	c.torrent = g_strcmp0(c.protocol, "torrent") == 0;
	c.client_removal = g_strdup(c.torrent ? "gone" : "done");
	c.client_id = int_at(rows, 0, 5);
	c.client_path = text_at(rows, 0, 6);
	c.recorded_volume = c.source_volume;
	c.recorded_path = g_strdup(c.source_path);
	c.location_inode = (guint64)int_at(rows, 0, 7);
	c.release_name = text_at(rows, 0, 8);
	c.client_ref = text_at(rows, 0, 9);
	/* The import that is being looked at is still open: it is not "another
	 * open import" in the download's place. */
	c.import_id = import_id;
	c.who.id = c.download_id;
	c.who.download = TRUE;
	c.who.dry = &said;
	c.asked = TRUE;
	look->torrent = c.torrent;
	look->volume = c.source_volume;
	look->path = g_strdup(c.source_path);
	look->client_path = g_strdup(c.client_path);
	look->download_state = text_at(rows, 0, 3);
	look->client_ref = g_strdup(c.client_ref);

	/* Whether its client has it now, asked of the client. */
	if (c.client_id == 0) {
		look_says(look, "left", "its client is no longer set up in Axis, so where the client keeps Axis's downloads "
			"cannot be asked");
		candidate_clear(&c);
		return TRUE;
	}
	now = client_now(self, cache, c.client_id);
	if (now->failure != NULL) {
		g_autofree gchar *text = g_strdup_printf("its client could not be asked which downloads it has now (%s)",
			now->failure);

		look_says(look, "waiting", text);
		candidate_clear(&c);
		return TRUE;
	}
	for (i = 0; c.client_ref != NULL && i < now->items->len; i++) {
		if (g_strcmp0(((const AxisDownloadItem *)g_ptr_array_index(now->items, i))->client_ref, c.client_ref) == 0)
			listed = TRUE;
	}
	if (listed && (g_strcmp0(look->download_state, "removed") == 0 || g_strcmp0(look->download_state, "gone") == 0)) {
		look_says(look, "left", "Axis recorded it as gone from its client, and the client lists it now: Axis deletes "
			"nothing under a download its client has. It can be removed at the client by hand");
		candidate_clear(&c);
		return TRUE;
	}
	look->at_client = listed;
	look->client_deletes = listed && c.torrent;
	c.at_client = look->client_deletes;

	ok = folder_decide(self, cache, &c, NULL, &decision, error);
	look->client_agrees = ok && decision.client_agrees;
	if (ok && !decision.go) {
		look_says(look, said.state != NULL ? said.state : "left", said.text != NULL ? said.text :
			"Axis's rules do not let it be deleted");
		/* A torrent that is not where it was recorded: the place the client
		 * names now, or none, never the recorded one as where its files are. */
		if (decision.elsewhere != NULL || decision.absent) {
			g_free(look->client_path);
			look->client_path = decision.elsewhere != NULL && decision.elsewhere[0] != '\0' ?
				g_strdup(decision.elsewhere) : NULL;
		}
	} else if (ok) {
		g_free(look->path);
		g_free(look->client_path);
		look->volume = decision.volume;
		look->path = g_strdup(decision.target.path);
		look->client_path = g_strdup(decision.target.client_path);
		look->file = decision.target.file;
		look->inode = decision.seen_inode;
		folder_measure(self, &c, &decision, look);
		/* It went from its place while Axis looked, and its client lists the
		 * torrent: as when nothing was there at the first look. */
		if (c.at_client && g_strcmp0(look->state, "gone") == 0) {
			look_says(look, "left", "the torrent is no longer where Axis recorded it: nothing is at that place, and "
				"its client still lists the torrent. It was probably moved in the client. Axis will not delete it, and "
				"does not ask the client to: remove it in the client, or dismiss the import");
			g_clear_pointer(&look->client_path, g_free);
		}
	}
	/* Never while nothing may be written, whoever deletes: a disk Axis cannot
	 * write is a disk it does not delete on, or ask a client to. */
	if (ok && (look->state == NULL || strcmp(look->state, "gone") == 0)) {
		g_autoptr(AxisVolumeGuard) writer = NULL;
		g_autoptr(GError) local = NULL;

		writer = axis_volume_guard_acquire(self->storage, look->volume, AXIS_VOLUME_INTENT_WRITE, &local);
		if (writer == NULL)
			look_says(look, g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND) ? "left" :
				"waiting", g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND) ?
				"its volume is no longer known" : local->message);
	}
	g_free(said.state);
	g_free(said.text);
	candidate_clear(&c);
	return ok;
}

gboolean
axis_import_folder_removals(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, client_now_free);
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *limit = axis_import_int_text(BATCH);
	const gchar *params[2];
	guint i;

	params[0] = id;
	params[1] = limit;
	/* A client removal still to make, or failed, keeps the folder pending; a
	 * torrent's is pending only once its client said it is gone. */
	if (!axis_db_pool_exec(self->pool,
	        "SELECT i.id, i.source_volume_id, i.source_path, i.client_removal, d.id, d.protocol, d.client_id,"
	        " d.client_path, d.volume_id, d.rel_path, i.mode IN ('move', 'move_copy'), i.source_inode"
	        " FROM import i LEFT JOIN download d ON d.id = i.download_id"
	        " WHERE i.state = 'imported' AND i.folder_removal IN ('pending', 'waiting')"
	        " AND coalesce(i.client_removal, '') NOT IN ('pending', 'failed')"
	        /* A move across volumes deletes the download's files it copied
	         * first (axis_import_moved_sources()). */
	        " AND NOT EXISTS (SELECT 1 FROM import_file f WHERE f.import_id = i.id AND f.source_state = 'pending')"
	        " AND ($1::bigint = 0 OR i.id = $1::bigint) ORDER BY i.id LIMIT $2::integer", 2, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		Candidate c;
		gboolean ok;

		memset(&c, 0, sizeof(c));
		c.import_id = int_at(rows, i, 0);
		c.source_volume = int_at(rows, i, 1);
		c.source_path = text_at(rows, i, 2);
		c.client_removal = text_at(rows, i, 3);
		c.download_id = int_at(rows, i, 4);
		c.protocol = text_at(rows, i, 5);
		c.client_id = int_at(rows, i, 6);
		c.client_path = text_at(rows, i, 7);
		c.recorded_volume = int_at(rows, i, 8);
		c.recorded_path = text_at(rows, i, 9);
		c.torrent = g_strcmp0(c.protocol, "torrent") == 0;
		c.moved = g_strcmp0(axis_db_result_get_text(rows, i, 10), "t") == 0;
		c.source_inode = (guint64)int_at(rows, i, 11);
		c.who.id = c.import_id;
		c.who.download = FALSE;
		ok = one_folder(self, cache, &c, report, error);
		candidate_clear(&c);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_import_download_folder_removals(
	AxisImport               *self,
	gint64                    download_id,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, client_now_free);
	g_autofree gchar *limit = axis_import_int_text(BATCH);
	g_autofree gchar *only = axis_import_int_text(download_id);
	const gchar *params[2];
	guint i;

	params[0] = limit;
	params[1] = only;
	/* Removed from its client with its data outside an import, or gone from
	 * it (axis_downloads_remove()); an imported one's leftover is its
	 * import's.  The last column: a person asked for it to be deleted on the
	 * page of its import (Candidate.asked). */
	if (!axis_db_pool_exec(self->pool,
	        "SELECT d.id, d.volume_id, d.rel_path, d.state, d.protocol, d.client_id, d.client_path, d.location_inode,"
	        " d.release_name, EXISTS (SELECT 1 FROM import j WHERE j.download_id = d.id AND j.state = 'dismissed'"
	        " AND j.reason = 'download_deleted')"
	        " FROM download d WHERE d.folder_removal IN ('pending', 'waiting')"
	        " AND d.state IN ('removed', 'gone')"
	        " AND NOT EXISTS (SELECT 1 FROM import i WHERE i.download_id = d.id AND i.state = 'imported')"
	        " AND ($2::bigint = 0 OR d.id = $2::bigint)"
	        " ORDER BY d.id LIMIT $1::integer", 2, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		Candidate c;
		gboolean ok;

		memset(&c, 0, sizeof(c));
		c.download_id = int_at(rows, i, 0);
		c.source_volume = int_at(rows, i, 1);
		c.source_path = text_at(rows, i, 2);
		c.protocol = text_at(rows, i, 4);
		c.torrent = g_strcmp0(c.protocol, "torrent") == 0;
		/* A torrent's leftover is looked at only when its client no longer
		 * had it; a Usenet job's when its client removed it. */
		c.client_removal = g_strdup(c.torrent ? "gone" : "done");
		c.client_id = int_at(rows, i, 5);
		c.client_path = text_at(rows, i, 6);
		c.recorded_volume = c.source_volume;
		c.recorded_path = g_strdup(c.source_path);
		c.location_inode = (guint64)int_at(rows, i, 7);
		c.release_name = text_at(rows, i, 8);
		c.asked = g_strcmp0(axis_db_result_get_text(rows, i, 9), "t") == 0;
		c.who.id = c.download_id;
		c.who.download = TRUE;
		ok = one_folder(self, cache, &c, report, error);
		candidate_clear(&c);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}
