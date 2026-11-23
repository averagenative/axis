/*
 * axis-library-browse.h - what the Library pages read: the roots' overview,
 * a folder's contents, and the filtered, sorted, paged list of files
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The promises these queries keep:
 *
 *  - No query does work proportional to the library.  A folder reads its
 *    children by an index on (parent, name) and its files by an index on
 *    (root, directory, name); the flat list walks the index of its sort,
 *    or a filter's index, and stops after one page; a count stops at
 *    AXIS_LIBRARY_COUNT_CAP.  Every statement runs under a deadline
 *    (AXIS_LIBRARY_LIST_TIMEOUT), and one that does not make it fails with
 *    %AXIS_DB_ERROR_TIMEOUT instead of holding a connection.
 *  - Paging is by keyset, never OFFSET: a page starts after a row id, and
 *    the row's sort key is read from the database (never from the URL), so
 *    rows added or removed between two pages neither repeat nor skip any
 *    row of the list.  A cursor whose row has gone starts the list again
 *    (the result says so).
 *  - Values only ever reach the database as parameters.  The text a name
 *    filter looks for is escaped for LIKE and cut to its length cap by the
 *    parser.
 *  - Folder totals are those of the last complete scan (library_dir), the
 *    files listed are the index as it is now.
 *
 * Every function blocks on the database: call it from a worker thread.
 */

#ifndef AXIS_LIBRARY_BROWSE_H
#define AXIS_LIBRARY_BROWSE_H

#include <glib.h>

#include "library/axis-library.h"
#include "library/axis-library-filter.h"

G_BEGIN_DECLS

/* Rows on a page, and counted before "more than". */
#define AXIS_LIBRARY_PAGE_SIZE       100
#define AXIS_LIBRARY_COUNT_CAP       10000

/* Deadlines of the statements of a page, and of its count. */
#define AXIS_LIBRARY_LIST_TIMEOUT    "5s"
#define AXIS_LIBRARY_COUNT_TIMEOUT   "2s"

/* ── The overview ───────────────────────────────────────────────── */

/**
 * AxisRootOverview:
 * @root_id: the root
 * @dir_id: its top folder's row, 0 until a scan has completed
 * @files: present files, as of the last complete scan
 * @bytes: their size
 * @last_complete_us: when the last complete scan finished, 0 for none
 * @last_complete_scan_id: that scan, 0 for none
 * @last_status: the latest scan's status (valid when @has_last)
 * @last_started_us: when it started
 * @running: a scan of the root is running now
 * @run_files: files the running scan has met so far
 * @run_dirs: folders it has met
 * @run_previous: files the root had before it, to judge its progress
 * @requested_us: a scan was requested and not yet started, or 0
 */
typedef struct
{
	gint64          root_id;
	gint64          dir_id;
	guint64         files;
	guint64         bytes;
	gint64          last_complete_us;
	gint64          last_complete_scan_id;
	gboolean        has_last;
	AxisScanStatus  last_status;
	gint64          last_started_us;
	gboolean        running;
	guint64         run_files;
	guint64         run_dirs;
	gint64          run_previous;
	gint64          requested_us;
} AxisRootOverview;

/** All roots, by id. */
gboolean
axis_library_get_overview(
	AxisLibrary  *self,
	GPtrArray   **roots,	/* (element-type AxisRootOverview) */
	GError      **error
);

/* ── Folders and files ──────────────────────────────────────────── */

typedef struct
{
	gint64   id;
	gint64   root_id;
	gint64   parent_id;	/* 0 for a root's top */
	gchar   *path;		/* stored form; "" for the top */
	gchar   *name;		/* stored form; "" for the top */
	guint64  direct_files;
	guint64  direct_bytes;
	guint64  total_files;
	guint64  total_bytes;
	guint64  subdirs;
} AxisDirRecord;

void
axis_dir_record_free(AxisDirRecord *dir);

/**
 * AxisBrowseFile:
 *
 * One line of a list.  Probe fields are unset (%NULL, -1) for a file
 * that has no media_info row.
 */
typedef struct
{
	gint64     id;
	gint64     root_id;
	gchar     *rel_path;	/* stored form */
	gchar     *kind;
	gchar     *state;
	gint64     size;
	gint64     mtime_us;
	gboolean   pinned;
	gchar     *probe_status;	/* NULL: no media_info row */
	gchar     *container;
	gchar     *video_codec;
	gchar     *resolution;
	gchar     *hdr;
	gboolean   hdr_uncertain;
	gchar     *audio_codec;
	gint64     duration_ms;	/* -1: unknown */
	gint64     bitrate;	/* -1: unknown */
} AxisBrowseFile;

void
axis_browse_file_free(AxisBrowseFile *file);

/**
 * AxisFolderView:
 * @dir: the folder
 * @crumbs: (element-type AxisDirRecord): from the root's top to @dir
 *   (itself included), ids and names only
 * @subdirs: (element-type AxisDirRecord): up to a page of subfolders in
 *   name order
 * @next_dir: the cursor for the next page of subfolders, 0 for none
 * @files: (element-type AxisBrowseFile): up to a page of the files directly
 *   in the folder, in name order
 * @next_file: the cursor for the next page of files, 0 for none
 * @dir_cursor_gone: the subfolder cursor named a row that is gone
 * @file_cursor_gone: likewise for files
 * @scan_id: the last complete scan of the root, 0 for none
 * @scan_us: when it finished
 */
typedef struct
{
	AxisDirRecord *dir;
	GPtrArray     *crumbs;
	GPtrArray     *subdirs;
	gint64         next_dir;
	GPtrArray     *files;
	gint64         next_file;
	gboolean       dir_cursor_gone;
	gboolean       file_cursor_gone;
	gint64         scan_id;
	gint64         scan_us;
} AxisFolderView;

void
axis_folder_view_free(AxisFolderView *view);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisFolderView, axis_folder_view_free)

/**
 * axis_library_get_folder:
 * @self: the library
 * @dir_id: a folder's row id
 * @after_dir: the last subfolder of the previous page, 0 for the first
 * @after_file: the last file of the previous page, 0 for the first
 * @view: (out) (transfer full): the folder
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_LIBRARY_ERROR_NOT_FOUND for no such folder.
 */
gboolean
axis_library_get_folder(
	AxisLibrary      *self,
	gint64            dir_id,
	gint64            after_dir,
	gint64            after_file,
	AxisFolderView  **view,
	GError          **error
);

/**
 * axis_library_get_folder_named:
 * @name: (nullable): only the subfolders whose name holds this text
 *   (ignoring the case of ASCII letters; at most
 *   AXIS_LIBRARY_FOLDER_NAME_MAX bytes of UTF-8), %NULL or "" for all
 *
 * As axis_library_get_folder(), with the subfolders filtered by name: the
 * pages walk the same index, among the folder's own children.  The files
 * are not filtered.
 */
#define AXIS_LIBRARY_FOLDER_NAME_MAX 200

gboolean
axis_library_get_folder_named(
	AxisLibrary      *self,
	gint64            dir_id,
	const gchar      *name,
	gint64            after_dir,
	gint64            after_file,
	AxisFolderView  **view,
	GError          **error
);

/**
 * axis_library_get_dir:
 *
 * One folder's record. Fails with %AXIS_LIBRARY_ERROR_NOT_FOUND.
 */
gboolean
axis_library_get_dir(
	AxisLibrary     *self,
	gint64           dir_id,
	AxisDirRecord  **dir,
	GError         **error
);

/**
 * axis_library_find_root_dir:
 * @dir_id: (out): the row of the root's top folder, 0 if no scan of the
 *   root has completed yet
 */
gboolean
axis_library_find_root_dir(
	AxisLibrary  *self,
	gint64        root_id,
	gint64       *dir_id,
	GError      **error
);

/**
 * AxisFileList:
 * @files: (element-type AxisBrowseFile): up to a page
 * @next: the cursor (last row's id) for the next page, 0 for none
 * @count: how many files match, counted up to AXIS_LIBRARY_COUNT_CAP
 * @count_capped: there are more than @count
 * @count_unknown: the count did not finish in time
 * @cursor_gone: the filter's cursor row has gone; the list starts again
 */
typedef struct
{
	GPtrArray *files;
	gint64     next;
	guint64    count;
	gboolean   count_capped;
	gboolean   count_unknown;
	gboolean   cursor_gone;
} AxisFileList;

void
axis_file_list_free(AxisFileList *list);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisFileList, axis_file_list_free)

/**
 * axis_library_list_files:
 * @self: the library
 * @filter: what to list (a parsed filter)
 * @list: (out) (transfer full): the page
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_DB_ERROR_TIMEOUT when the page did not come in time (the
 * filter is too broad for the sort; the caller says so).
 */
gboolean
axis_library_list_files(
	AxisLibrary              *self,
	const AxisLibraryFilter  *filter,
	AxisFileList            **list,
	GError                  **error
);

/**
 * axis_library_latest_scan:
 * @self: the library
 * @root_id: a root, or 0 for any
 * @scan_id: (out): the highest id among the roots' last complete scans
 *
 * What the "list changed" notice compares against.
 */
gboolean
axis_library_latest_scan(
	AxisLibrary  *self,
	gint64        root_id,
	gint64       *scan_id,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_LIBRARY_BROWSE_H */
