/*
 * axis-library-dirs.c - the folder totals of a root, brought up to date
 * when a complete scan ends
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The folders are not handed over by the walk: the database derives them
 * from the index, in the transaction that ends the scan.  That keeps the
 * walk's memory independent of the number of folders, and puts the totals
 * under the same rule as every other conclusion of a scan: they are written
 * only when the scan completed, and a transaction that fails or is rolled
 * back leaves the previous ones as they were.
 *
 * Three statements on a temporary table of the root's folders:
 *
 *   1. one pass over the root's present files counts them per directory
 *      (a hash aggregate on the database's side), a recursive query walks
 *      each directory up to the top to give every ancestor its totals;
 *   2. rows whose numbers changed are written (an unchanged folder is not
 *      written at all), and rows of folders that have none are deleted;
 *   3. the parent links of new rows are set.
 */

#include "library/axis-library-private.h"

const gchar axis_library_sql_dirs_build[] =
	"CREATE TEMP TABLE library_scan_dirs ON COMMIT DROP AS"
	" WITH RECURSIVE direct AS ("
	"   SELECT library_dir_of(rel_path) AS path, count(*) AS files, sum(size)::bigint AS bytes"
	"   FROM library_file WHERE root_id = $1::bigint AND state = 'present' GROUP BY 1"
	" ), up(anc, files, bytes) AS ("
	"   SELECT path, files, bytes FROM direct"
	"   UNION ALL SELECT ''::text, 0::bigint, 0::bigint"
	"   UNION ALL SELECT library_dir_of(anc), files, bytes FROM up WHERE anc <> ''"
	" ), totals AS ("
	"   SELECT anc AS path, sum(files)::bigint AS total_files, sum(bytes)::bigint AS total_bytes"
	"   FROM up GROUP BY anc"
	" ), base AS ("
	"   SELECT t.path, library_path_hash(t.path) AS path_hash,"
	"     CASE WHEN t.path = '' THEN ''::text ELSE library_base_name_of(t.path) END AS name,"
	"     CASE WHEN t.path = '' THEN NULL::bytea ELSE library_path_hash(library_dir_of(t.path)) END"
	"       AS parent_hash,"
	"     coalesce(d.files, 0)::bigint AS direct_files, coalesce(d.bytes, 0)::bigint AS direct_bytes,"
	"     t.total_files, t.total_bytes"
	"   FROM totals t LEFT JOIN direct d ON d.path = t.path"
	" )"
	" SELECT b.path, b.path_hash, b.name, b.parent_hash, b.direct_files, b.direct_bytes,"
	"   b.total_files, b.total_bytes, coalesce(s.n, 0)::bigint AS subdirs"
	" FROM base b LEFT JOIN (SELECT parent_hash, count(*) AS n FROM base"
	"   WHERE parent_hash IS NOT NULL GROUP BY parent_hash) s ON s.parent_hash = b.path_hash";

const gchar axis_library_sql_dirs_write[] =
	"INSERT INTO library_dir (root_id, path, name, direct_files, direct_bytes, total_files,"
	"   total_bytes, subdirs)"
	" SELECT $1::bigint, path, name, direct_files, direct_bytes, total_files, total_bytes, subdirs"
	" FROM library_scan_dirs"
	" ON CONFLICT (root_id, path_hash) DO UPDATE SET"
	"   direct_files = EXCLUDED.direct_files, direct_bytes = EXCLUDED.direct_bytes,"
	"   total_files = EXCLUDED.total_files, total_bytes = EXCLUDED.total_bytes,"
	"   subdirs = EXCLUDED.subdirs"
	" WHERE (library_dir.direct_files, library_dir.direct_bytes, library_dir.total_files,"
	"   library_dir.total_bytes, library_dir.subdirs) IS DISTINCT FROM"
	"   (EXCLUDED.direct_files, EXCLUDED.direct_bytes, EXCLUDED.total_files, EXCLUDED.total_bytes,"
	"    EXCLUDED.subdirs)";

const gchar axis_library_sql_dirs_delete[] =
	"DELETE FROM library_dir d WHERE d.root_id = $1::bigint"
	" AND NOT EXISTS (SELECT 1 FROM library_scan_dirs t WHERE t.path_hash = d.path_hash)";

const gchar axis_library_sql_dirs_parents[] =
	"UPDATE library_dir c SET parent_id = p.id"
	" FROM library_scan_dirs t JOIN library_dir p ON p.root_id = $1::bigint AND p.path_hash = t.parent_hash"
	" WHERE c.root_id = $1::bigint AND c.path_hash = t.path_hash AND c.parent_id IS DISTINCT FROM p.id";

gboolean
axis_library_index_update_dirs(
	AxisDbConn  *conn,
	gint64       root_id,
	GError     **error
){
	g_autofree gchar *root = axis_library_int_text(root_id);
	const gchar *params[1];

	params[0] = root;
	/*
	 * Every join below is of the whole root against the whole root, and
	 * the planner cannot know that: the temporary table has no statistics
	 * until it is analysed, and the rows of library_dir that were just
	 * written (or not yet counted, on a root's first scan) have none
	 * either.  Misled, it nests loops over hundreds of thousands of rows.
	 * So the nested loop is ruled out for these statements (and allowed
	 * again for the rest of the scan's last transaction).
	 */
	return axis_db_conn_exec(conn, "SET LOCAL enable_nestloop = off", 0, NULL, NULL, error) &&
	       axis_db_conn_exec(conn, "SET LOCAL work_mem = '" AXIS_LIBRARY_DIRS_WORK_MEM "'", 0, NULL, NULL,
	                         error) &&
	       axis_db_conn_exec(conn, axis_library_sql_dirs_build, 1, params, NULL, error) &&
	       axis_db_conn_exec(conn, "ANALYZE library_scan_dirs", 0, NULL, NULL, error) &&
	       axis_db_conn_exec(conn, axis_library_sql_dirs_write, 1, params, NULL, error) &&
	       axis_db_conn_exec(conn, axis_library_sql_dirs_delete, 1, params, NULL, error) &&
	       axis_db_conn_exec(conn, axis_library_sql_dirs_parents, 1, params, NULL, error) &&
	       axis_db_conn_exec(conn, "RESET enable_nestloop", 0, NULL, NULL, error) &&
	       axis_db_conn_exec(conn, "RESET work_mem", 0, NULL, NULL, error);
}
