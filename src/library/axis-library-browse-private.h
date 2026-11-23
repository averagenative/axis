/*
 * axis-library-browse-private.h - the statements of the Library pages, so
 * that a test can ask the database for their plans
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_LIBRARY_BROWSE_PRIVATE_H
#define AXIS_LIBRARY_BROWSE_PRIVATE_H

#include "library/axis-library-browse.h"

G_BEGIN_DECLS

#define AXIS_LIBRARY_DIR_COLUMNS \
	"id, root_id, coalesce(parent_id, 0), path, name, direct_files, direct_bytes," \
	" total_files, total_bytes, subdirs"

#define AXIS_LIBRARY_FILE_COLUMNS \
	"f.id, f.root_id, f.rel_path, f.kind, f.state, f.size, f.mtime_sec, f.pinned," \
	" m.status, m.container, m.video_codec, m.resolution_class, m.hdr_format, m.hdr_uncertain," \
	" m.audio_codec, m.duration_ms, m.bitrate"

/* A folder's subfolders in name order: $1 the folder, $2 the limit; and
 * after a row: $3 its name, $4 its id. */
#define AXIS_LIBRARY_SQL_DIRS_FIRST \
	"SELECT " AXIS_LIBRARY_DIR_COLUMNS " FROM library_dir WHERE parent_id = $1::bigint" \
	" ORDER BY name, id LIMIT $2::integer"

#define AXIS_LIBRARY_SQL_DIRS_AFTER \
	"SELECT " AXIS_LIBRARY_DIR_COLUMNS " FROM library_dir WHERE parent_id = $1::bigint" \
	" AND (name, id) > ($3::text COLLATE \"C\", $4::bigint)" \
	" ORDER BY name, id LIMIT $2::integer"

/* The same, of the subfolders whose name holds a text: its LIKE pattern is
 * $3 of the first page, $5 of a later one.  The same index, in the same
 * order: what does not match is passed over, among the folder's children
 * only. */
#define AXIS_LIBRARY_SQL_DIRS_NAMED_FIRST \
	"SELECT " AXIS_LIBRARY_DIR_COLUMNS " FROM library_dir WHERE parent_id = $1::bigint" \
	" AND name ILIKE $3::text" \
	" ORDER BY name, id LIMIT $2::integer"

#define AXIS_LIBRARY_SQL_DIRS_NAMED_AFTER \
	"SELECT " AXIS_LIBRARY_DIR_COLUMNS " FROM library_dir WHERE parent_id = $1::bigint" \
	" AND (name, id) > ($3::text COLLATE \"C\", $4::bigint) AND name ILIKE $5::text" \
	" ORDER BY name, id LIMIT $2::integer"

/* The files directly in a folder, the same way. */
#define AXIS_LIBRARY_SQL_FILES_FIRST \
	"SELECT " AXIS_LIBRARY_FILE_COLUMNS " FROM library_dir d" \
	" JOIN library_file f ON f.root_id = d.root_id AND f.dir_hash = d.path_hash" \
	" LEFT JOIN media_info m ON m.file_id = f.id" \
	" WHERE d.id = $1::bigint AND f.state = 'present'" \
	" ORDER BY f.base_name, f.id LIMIT $2::integer"

#define AXIS_LIBRARY_SQL_FILES_AFTER \
	"SELECT " AXIS_LIBRARY_FILE_COLUMNS " FROM library_dir d" \
	" JOIN library_file f ON f.root_id = d.root_id AND f.dir_hash = d.path_hash" \
	" LEFT JOIN media_info m ON m.file_id = f.id" \
	" WHERE d.id = $1::bigint AND f.state = 'present'" \
	" AND (f.base_name, f.id) > ($3::text COLLATE \"C\", $4::bigint)" \
	" ORDER BY f.base_name, f.id LIMIT $2::integer"

/**
 * axis_library_browse_list_sql:
 * @filter: the list
 * @key: (nullable): the sort key of the row the page starts after
 * @after: (nullable): that row's id (with @key)
 * @params: (element-type utf8): the parameters, appended: owned strings
 * @n_base: (out): how many of them the count statement uses
 * @count_sql: (out) (transfer full): the statement that counts
 *
 * Returns: (transfer full): the statement that lists one page
 */
gchar *
axis_library_browse_list_sql(
	const AxisLibraryFilter  *filter,
	const gchar              *key,
	const gchar              *after,
	GPtrArray                *params,
	guint                    *n_base,
	gchar                   **count_sql
);

/* The statement that reads a row's sort key by its id ($1). */
const gchar *
axis_library_browse_key_sql(AxisLibrarySort sort);

/**
 * axis_library_browse_scan_sql:
 * @filter: the list; its estimator conditions (target, outcome) are not
 *   SQL and are left to the caller
 * @extra: (nullable): one more condition over library_file f, media_info m
 *   and library_root r (always joined), constant text of the caller's
 * @key: (nullable): the sort key of the row to start after
 * @after: (nullable): that row's id
 * @limit: at most this many rows
 * @params: (element-type utf8): the parameters, appended: owned strings
 *
 * For a list whose rows the caller sifts (the estimator's): the ids and
 * sort keys (as text) of the rows after the cursor, in the list's order.
 *
 * Returns: (transfer full): the statement
 */
gchar *
axis_library_browse_scan_sql(
	const AxisLibraryFilter  *filter,
	const gchar              *extra,
	const gchar              *key,
	const gchar              *after,
	guint                     limit,
	GPtrArray                *params
);

/* The list lines of @ids, in that order (rows gone are left out); one
 * statement under the list's deadline, in a transaction of its own. */
gboolean
axis_library_browse_read_files(
	AxisDbConn    *conn,
	const gint64  *ids,
	guint          n_ids,
	GPtrArray    **files,
	GError       **error
);

G_END_DECLS

#endif /* AXIS_LIBRARY_BROWSE_PRIVATE_H */
