/*
 * axis-library-index.c - what a scan writes: batches of what it saw, and
 * at the end of a complete scan, what it concludes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every statement here works on the stored forms of paths
 * (axis-library-name.h), passed as JSON so that any name survives the trip
 * exactly.  A batch is one transaction; so is the conclusion.  Nothing is
 * kept in memory across batches but counters: moves and absences are
 * worked out by the database.
 */

#include "library/axis-library-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* The first characters of a path the prefix index holds. */
#define PREFIX_CHARS 600

void
axis_scan_state_clear(AxisScanState *state)
{
	g_free(state->started_at);
	g_free(state->boot_id);
	g_strfreev(state->patterns);
	memset(state, 0, sizeof(*state));
}

gchar *
axis_library_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static gint64
column_int64(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

/* ── The volume's lock ──────────────────────────────────────────── */

gboolean
axis_library_index_lock(
	AxisDbConn  *conn,
	gint64       volume_id,
	gboolean    *got,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_library_int_text(volume_id);
	const gchar *params[1];
	gboolean value = FALSE;

	*got = FALSE;
	if (volume_id <= 0 || volume_id > G_MAXINT32) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_FAILED,
			"volume %" G_GINT64_FORMAT " cannot be locked", volume_id);
		return FALSE;
	}

	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1,
	                       params, &result, error))
		return FALSE;
	if (axis_db_result_get_boolean(result, 0, 0, &value))
		*got = value;
	return TRUE;
}

void
axis_library_index_unlock(
	AxisDbConn *conn,
	gint64      volume_id
){
	g_autofree gchar *id = axis_library_int_text(volume_id);
	const gchar *params[1];
	g_autoptr(GError) error = NULL;

	params[0] = id;
	/* A connection that broke has lost the lock with its session. */
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_unlock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1,
	                       params, NULL, &error))
		g_debug("library: unlocking volume %" G_GINT64_FORMAT ": %s", volume_id, error->message);
}

/* ── Starting ───────────────────────────────────────────────────── */

static gchar **
patterns_from_json(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	GPtrArray *patterns = g_ptr_array_new();
	JsonNode *root;

	if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL)) {
		root = json_parser_get_root(parser);
		if (root != NULL && JSON_NODE_HOLDS_ARRAY(root)) {
			JsonArray *array = json_node_get_array(root);
			guint i;

			for (i = 0; i < json_array_get_length(array); i++) {
				JsonNode *node = json_array_get_element(array, i);

				if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
					g_ptr_array_add(patterns, g_strdup(json_node_get_string(node)));
			}
		}
	}
	g_ptr_array_add(patterns, NULL);

	return (gchar **)g_ptr_array_free(patterns, FALSE);
}

gboolean
axis_library_index_begin(
	AxisDbConn     *conn,
	AxisScanState  *state,
	const gchar    *trigger,
	gint64          job_id,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *root = axis_library_int_text(state->root_id);
	g_autofree gchar *volume = axis_library_int_text(state->volume_id);
	g_autofree gchar *generation = NULL;
	g_autofree gchar *job = job_id > 0 ? axis_library_int_text(job_id) : NULL;
	g_autofree gchar *boottime = axis_library_int_text(state->boottime_us);
	g_autofree gchar *previous = NULL;
	g_autofree gchar *scan = NULL;
	const gchar *params[7];

	if (!axis_db_conn_begin(conn, error))
		return FALSE;

	params[0] = root;
	if (!axis_db_conn_exec(conn, "INSERT INTO library_root_state (root_id) VALUES ($1::bigint)"
	                       " ON CONFLICT (root_id) DO NOTHING", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn,
	                       "SELECT s.complete_generation, array_to_json(s.ignore_patterns)::text,"
	                       " coalesce((SELECT c.files FROM library_scan c"
	                       "   WHERE c.id = s.last_complete_scan_id), 0)"
	                       " FROM library_root_state s WHERE s.root_id = $1::bigint FOR UPDATE",
	                       1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND,
			"root %" G_GINT64_FORMAT " is gone", state->root_id);
		goto fail;
	}
	state->complete_generation = column_int64(result, 0, 0);
	g_strfreev(state->patterns);
	state->patterns = patterns_from_json(axis_db_result_get_text(result, 0, 1));
	state->previous_files = column_int64(result, 0, 2);
	g_clear_pointer(&result, axis_db_result_free);

	/* The caller holds the volume's lock: whatever of the volume is still
	 * "running" was left by a scan that died. */
	params[0] = volume;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE library_scan SET status = 'incomplete', reason = 'interrupted',"
	                       " message = 'The scan stopped without finishing (the daemon was stopped or"
	                       " killed while it ran).', finished_at = now()"
	                       " WHERE status = 'running' AND root_id IN"
	                       "   (SELECT id FROM library_root WHERE volume_id = $1::bigint)",
	                       1, params, NULL, error))
		goto fail;

	/* A request made before now is answered by this scan. */
	params[0] = root;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE library_root_state SET generation = generation + 1,"
	                       " scan_requested_at = CASE WHEN scan_requested_at <= now() THEN NULL"
	                       "   ELSE scan_requested_at END,"
	                       " updated_at = now()"
	                       " WHERE root_id = $1::bigint RETURNING generation",
	                       1, params, &result, error))
		goto fail;
	state->generation = column_int64(result, 0, 0);
	g_clear_pointer(&result, axis_db_result_free);

	generation = axis_library_int_text(state->generation);
	previous = axis_library_int_text(state->previous_files);
	params[0] = root;
	params[1] = generation;
	params[2] = job;
	params[3] = trigger;
	params[4] = state->boot_id;
	params[5] = boottime;
	params[6] = previous;
	if (!axis_db_conn_exec(conn,
	                       "INSERT INTO library_scan (root_id, generation, job_id, trigger, boot_id,"
	                       " started_boottime_us, previous_files)"
	                       " VALUES ($1::bigint, $2::bigint, $3::bigint, $4, $5, $6::bigint, $7::bigint)"
	                       " RETURNING id",
	                       7, params, &result, error))
		goto fail;
	state->scan_id = column_int64(result, 0, 0);
	g_clear_pointer(&result, axis_db_result_free);

	scan = axis_library_int_text(state->scan_id);
	params[0] = root;
	params[1] = scan;
	if (!axis_db_conn_exec(conn, "UPDATE library_root_state SET last_scan_id = $2::bigint"
	                       " WHERE root_id = $1::bigint", 2, params, NULL, error) ||
	    !axis_db_conn_commit(conn, error))
		goto fail;

	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

/* ── Batches ────────────────────────────────────────────────────── */

static void
append_bool(
	GString     *json,
	const gchar *name,
	gboolean     value
){
	g_string_append_printf(json, ",\"%s\":%s", name, value ? "true" : "false");
}

static void
append_int(
	GString     *json,
	const gchar *name,
	gint64       value
){
	g_string_append_printf(json, ",\"%s\":%" G_GINT64_FORMAT, name, value);
}

static gchar *
entries_json(GArray *entries)
{
	GString *json = g_string_sized_new(entries->len * 200 + 2);
	guint i;

	g_string_append_c(json, '[');
	for (i = 0; i < entries->len; i++) {
		const AxisWalkEntry *entry = &g_array_index(entries, AxisWalkEntry, i);

		if (i > 0)
			g_string_append_c(json, ',');
		g_string_append(json, "{\"p\":");
		axis_library_json_append_string(json, entry->path);
		append_bool(json, "u", entry->invalid);
		g_string_append(json, ",\"k\":");
		axis_library_json_append_string(json, axis_file_kind_to_string(entry->kind));
		append_int(json, "s", entry->size);
		append_int(json, "ms", entry->mtime_sec);
		append_int(json, "mn", entry->mtime_nsec);
		append_int(json, "cs", entry->ctime_sec);
		append_int(json, "cn", entry->ctime_nsec);
		/* The same 64 bits, signed. */
		append_int(json, "i", (gint64)entry->inode);
		append_int(json, "l", (gint64)MIN(entry->nlink, (guint64)G_MAXINT64));
		append_bool(json, "r", entry->racy);
		g_string_append(json, ",\"t\":");
		if (entry->target != NULL)
			axis_library_json_append_string(json, entry->target);
		else
			g_string_append(json, "null");
		g_string_append_c(json, '}');
	}
	g_string_append_c(json, ']');

	return g_string_free(json, FALSE);
}

/*
 * A file's row: inserted when its path is new, else brought up to date.
 * Whatever is seen is present again.  Its content counts as changed when
 * anything that tells content apart differs, or the last look was racy.
 */
#define DIFFERS \
	"(f.stat_racy OR f.kind <> EXCLUDED.kind OR f.size <> EXCLUDED.size" \
	" OR f.mtime_sec <> EXCLUDED.mtime_sec OR f.mtime_nsec <> EXCLUDED.mtime_nsec" \
	" OR f.ctime_sec <> EXCLUDED.ctime_sec OR f.ctime_nsec <> EXCLUDED.ctime_nsec" \
	" OR f.inode <> EXCLUDED.inode OR f.link_target IS DISTINCT FROM EXCLUDED.link_target)"

static const gchar upsert_sql[] =
	"WITH e AS ("
	"  SELECT DISTINCT ON (x.p) x.p, x.u, x.k, x.s, x.ms, x.mn, x.cs, x.cn, x.i, x.l, x.r, x.t"
	"  FROM jsonb_to_recordset($4::jsonb) AS x(p text, u boolean, k text, s bigint, ms bigint,"
	"    mn integer, cs bigint, cn integer, i bigint, l bigint, r boolean, t text)"
	"  ORDER BY x.p"
	"), prev AS ("
	"  SELECT f.rel_path, f.state FROM library_file f JOIN e"
	"    ON f.path_hash = library_path_hash(e.p) AND f.rel_path = e.p"
	"  WHERE f.root_id = $1::bigint"
	"), up AS ("
	"  INSERT INTO library_file AS f (root_id, volume_id, rel_path, name_invalid_utf8, kind, size,"
	"    mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, link_target, pinned,"
	"    pinned_links, pinned_downloads, stat_racy, first_seen_generation, last_seen_generation,"
	"    changed, changed_generation)"
	"  SELECT $1::bigint, $2::bigint, e.p, e.u, e.k, e.s, e.ms, e.mn, e.cs, e.cn, e.i, e.l, e.t,"
	"    e.l > 1 OR $5::boolean, e.l > 1, $5::boolean, e.r, $3::bigint, $3::bigint, true, $3::bigint"
	"  FROM e"
	"  ON CONFLICT (root_id, path_hash) DO UPDATE SET"
	"    last_seen_generation = EXCLUDED.last_seen_generation,"
	"    changed = f.changed OR " DIFFERS ","
	"    changed_generation = CASE WHEN " DIFFERS " THEN EXCLUDED.last_seen_generation"
	"      ELSE f.changed_generation END,"
	"    content_changed_at = CASE WHEN " DIFFERS " THEN now() ELSE f.content_changed_at END,"
	"    name_invalid_utf8 = EXCLUDED.name_invalid_utf8,"
	"    kind = EXCLUDED.kind,"
	"    size = EXCLUDED.size,"
	"    mtime_sec = EXCLUDED.mtime_sec,"
	"    mtime_nsec = EXCLUDED.mtime_nsec,"
	"    ctime_sec = EXCLUDED.ctime_sec,"
	"    ctime_nsec = EXCLUDED.ctime_nsec,"
	"    inode = EXCLUDED.inode,"
	"    nlink = EXCLUDED.nlink,"
	"    link_target = EXCLUDED.link_target,"
	"    pinned = EXCLUDED.pinned,"
	"    pinned_links = EXCLUDED.pinned_links,"
	"    pinned_downloads = EXCLUDED.pinned_downloads,"
	"    stat_racy = EXCLUDED.stat_racy,"
	"    state = 'present',"
	"    suspected_at = NULL,"
	"    suspected_scan_id = NULL,"
	"    suspected_boot_id = NULL,"
	"    suspected_boottime_us = NULL,"
	"    missing_at = NULL"
	"  RETURNING f.rel_path, f.changed_generation = $3::bigint AS changed_now"
	")"
	" SELECT count(*) FILTER (WHERE prev.rel_path IS NULL),"
	"   count(*) FILTER (WHERE prev.rel_path IS NOT NULL AND up.changed_now),"
	"   count(*) FILTER (WHERE prev.state <> 'present')"
	" FROM up LEFT JOIN prev ON prev.rel_path = up.rel_path";

static gboolean
write_entries(
	AxisDbConn     *conn,
	AxisScanState  *state,
	GArray         *entries,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *root = axis_library_int_text(state->root_id);
	g_autofree gchar *volume = axis_library_int_text(state->volume_id);
	g_autofree gchar *generation = axis_library_int_text(state->generation);
	g_autofree gchar *json = entries_json(entries);
	const gchar *params[5];

	params[0] = root;
	params[1] = volume;
	params[2] = generation;
	params[3] = json;
	params[4] = state->downloads ? "true" : "false";
	if (!axis_db_conn_exec(conn, upsert_sql, 5, params, &result, error))
		return FALSE;

	state->new_files += (guint64)column_int64(result, 0, 0);
	state->changed_files += (guint64)column_int64(result, 0, 1);
	state->reappeared += (guint64)column_int64(result, 0, 2);
	return TRUE;
}

/*
 * Marks rows at or under paths that were not looked at: the path itself
 * by its hash, what is under it by a range over the prefix index (exact
 * for prefixes shorter than the index holds, an equality on the first
 * PREFIX_CHARS otherwise), always rechecked in full.
 */
static gboolean
write_unexamined(
	AxisDbConn     *conn,
	AxisScanState  *state,
	GPtrArray      *paths,
	GError        **error
){
	g_autoptr(GString) exact = g_string_new("[");
	g_autoptr(GString) range = g_string_new("[");
	g_autoptr(GString) longer = g_string_new("[");
	g_autofree gchar *root = axis_library_int_text(state->root_id);
	g_autofree gchar *generation = axis_library_int_text(state->generation);
	gboolean any_range = FALSE;
	gboolean any_long = FALSE;
	const gchar *params[3];
	guint n_exact = 0;
	guint i;

	for (i = 0; i < paths->len; i++) {
		const gchar *path = g_ptr_array_index(paths, i);
		g_autofree gchar *prefix = NULL;

		if (path[0] == '\0')
			continue;
		prefix = g_strconcat(path, "/", NULL);

		g_string_append(exact, n_exact++ > 0 ? ",{\"p\":" : "{\"p\":");
		axis_library_json_append_string(exact, path);
		g_string_append_c(exact, '}');

		if (g_utf8_strlen(prefix, -1) < PREFIX_CHARS) {
			/* Every path that starts with "D/" lies in ["D/", "D0"). */
			g_autofree gchar *high = g_strconcat(path, "0", NULL);

			g_string_append(range, any_range ? ",{\"pre\":" : "{\"pre\":");
			axis_library_json_append_string(range, prefix);
			g_string_append(range, ",\"hi\":");
			axis_library_json_append_string(range, high);
			g_string_append_c(range, '}');
			any_range = TRUE;
		} else {
			const gchar *end = g_utf8_offset_to_pointer(prefix, PREFIX_CHARS);
			g_autofree gchar *head = g_strndup(prefix, (gsize)(end - prefix));

			g_string_append(longer, any_long ? ",{\"pre\":" : "{\"pre\":");
			axis_library_json_append_string(longer, prefix);
			g_string_append(longer, ",\"lo\":");
			axis_library_json_append_string(longer, head);
			g_string_append_c(longer, '}');
			any_long = TRUE;
		}
	}
	g_string_append_c(exact, ']');
	g_string_append_c(range, ']');
	g_string_append_c(longer, ']');

	params[0] = root;
	params[1] = generation;
	if (n_exact > 0) {
		params[2] = exact->str;
		if (!axis_db_conn_exec(conn,
		                       "UPDATE library_file f SET unexamined_generation = $2::bigint"
		                       " FROM jsonb_to_recordset($3::jsonb) AS d(p text)"
		                       " WHERE f.root_id = $1::bigint AND f.path_hash = library_path_hash(d.p)"
		                       "   AND f.rel_path = d.p",
		                       3, params, NULL, error))
			return FALSE;
	}
	if (any_range) {
		params[2] = range->str;
		if (!axis_db_conn_exec(conn,
		                       "UPDATE library_file f SET unexamined_generation = $2::bigint"
		                       " FROM jsonb_to_recordset($3::jsonb) AS d(pre text, hi text)"
		                       " WHERE f.root_id = $1::bigint"
		                       "   AND left(f.rel_path, 600) >= d.pre AND left(f.rel_path, 600) < d.hi"
		                       "   AND starts_with(f.rel_path, d.pre)",
		                       3, params, NULL, error))
			return FALSE;
	}
	if (any_long) {
		params[2] = longer->str;
		if (!axis_db_conn_exec(conn,
		                       "UPDATE library_file f SET unexamined_generation = $2::bigint"
		                       " FROM jsonb_to_recordset($3::jsonb) AS d(pre text, lo text)"
		                       " WHERE f.root_id = $1::bigint AND left(f.rel_path, 600) = d.lo"
		                       "   AND starts_with(f.rel_path, d.pre)",
		                       3, params, NULL, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
write_findings(
	AxisDbConn     *conn,
	AxisScanState  *state,
	GPtrArray      *findings,
	GError        **error
){
	g_autoptr(GString) json = g_string_new("[");
	g_autofree gchar *scan = axis_library_int_text(state->scan_id);
	const gchar *params[2];
	guint i;

	for (i = 0; i < findings->len; i++) {
		const AxisWalkFinding *finding = g_ptr_array_index(findings, i);
		g_autofree gchar *detail = axis_library_text_clean(finding->detail, AXIS_LIBRARY_DETAIL_CHARS);

		if (i > 0)
			g_string_append_c(json, ',');
		g_string_append_printf(json, "{\"n\":%u,\"k\":", state->findings + i + 1);
		axis_library_json_append_string(json, axis_walk_finding_kind_to_string(finding->kind));
		g_string_append(json, ",\"p\":");
		axis_library_json_append_string(json, finding->path);
		g_string_append(json, ",\"d\":");
		if (detail != NULL)
			axis_library_json_append_string(json, detail);
		else
			g_string_append(json, "null");
		g_string_append_c(json, '}');
	}
	g_string_append_c(json, ']');

	params[0] = scan;
	params[1] = json->str;
	if (!axis_db_conn_exec(conn,
	                       "INSERT INTO library_scan_finding (scan_id, seq, kind, rel_path, detail)"
	                       " SELECT $1::bigint, x.n, x.k, x.p, x.d"
	                       " FROM jsonb_to_recordset($2::jsonb) AS x(n integer, k text, p text, d text)",
	                       2, params, NULL, error))
		return FALSE;

	state->findings += findings->len;
	return TRUE;
}

static gchar *
kinds_json(const AxisWalkTotals *totals)
{
	GString *json = g_string_new("{");
	guint i;

	for (i = 0; i < AXIS_FILE_KIND_COUNT; i++) {
		if (i > 0)
			g_string_append_c(json, ',');
		axis_library_json_append_string(json, axis_file_kind_to_string((AxisFileKind)i));
		g_string_append_printf(json, ":{\"files\":%" G_GUINT64_FORMAT ",\"bytes\":%" G_GUINT64_FORMAT "}",
			totals->kind_files[i], totals->kind_bytes[i]);
	}
	g_string_append_c(json, '}');

	return g_string_free(json, FALSE);
}

#define N_COUNTERS 20

/* The scan's counters as parameters $2 to $21 of COUNTERS_SET. */
#define COUNTERS_SET \
	" dirs = $2::bigint, files = $3::bigint, bytes = $4::bigint, symlinks = $5::bigint," \
	" special = $6::bigint, ignored = $7::bigint, partials = $8::bigint, other_fs = $9::bigint," \
	" too_deep = $10::bigint, too_long = $11::bigint, unreadable = $12::bigint," \
	" io_errors = $13::bigint, vanished = $14::bigint, replaced = $15::bigint," \
	" findings = $16::integer, new_files = $17::bigint, changed_files = $18::bigint," \
	" reappeared = $19::bigint, kinds = $20::jsonb, previous_files = $21::bigint"

typedef struct
{
	gchar *text[N_COUNTERS + 1];
} Counters;

static void
counters_fill(
	Counters             *counters,
	const AxisScanState  *state,
	const AxisWalkTotals *totals,
	const gchar         **params
){
	guint64 values[] = {
		totals->dirs, totals->files, totals->bytes, totals->symlinks, totals->special,
		totals->ignored, totals->partials, totals->other_fs, totals->too_deep, totals->too_long,
		totals->unreadable, totals->io_errors, totals->vanished, totals->replaced, totals->findings,
		state->new_files, state->changed_files, state->reappeared
	};
	guint i;

	G_STATIC_ASSERT(G_N_ELEMENTS(values) == N_COUNTERS - 2);
	for (i = 0; i < G_N_ELEMENTS(values); i++)
		counters->text[i] = g_strdup_printf("%" G_GUINT64_FORMAT, MIN(values[i], (guint64)G_MAXINT64));
	/* findings is an integer column. */
	g_free(counters->text[14]);
	counters->text[14] = g_strdup_printf("%u", (guint)MIN(totals->findings, (guint64)G_MAXINT32));
	counters->text[N_COUNTERS - 2] = kinds_json(totals);
	counters->text[N_COUNTERS - 1] = axis_library_int_text(state->previous_files);
	counters->text[N_COUNTERS] = NULL;
	for (i = 0; i < N_COUNTERS; i++)
		params[i] = counters->text[i];
}

static void
counters_clear(Counters *counters)
{
	guint i;

	for (i = 0; i < N_COUNTERS; i++)
		g_free(counters->text[i]);
}

static gboolean
write_counters(
	AxisDbConn            *conn,
	AxisScanState         *state,
	const AxisWalkTotals  *totals,
	GError               **error
){
	g_autofree gchar *scan = axis_library_int_text(state->scan_id);
	const gchar *params[N_COUNTERS + 1];
	Counters counters;
	gboolean ok;

	params[0] = scan;
	counters_fill(&counters, state, totals, params + 1);
	ok = axis_db_conn_exec(conn, "UPDATE library_scan SET" COUNTERS_SET " WHERE id = $1::bigint",
		N_COUNTERS + 1, params, NULL, error);
	counters_clear(&counters);

	return ok;
}

gboolean
axis_library_index_write(
	AxisDbConn             *conn,
	AxisScanState          *state,
	AxisWalkBatch          *batch,
	const AxisWalkTotals   *totals,
	AxisLibraryCommitHook   hook,
	gpointer                hook_data,
	GError                **error
){
	guint findings_before = state->findings;
	guint64 new_before = state->new_files;
	guint64 changed_before = state->changed_files;
	guint64 reappeared_before = state->reappeared;

	if (!axis_db_conn_begin(conn, error))
		return FALSE;

	if ((batch->entries->len > 0 && !write_entries(conn, state, batch->entries, error)) ||
	    (batch->unexamined->len > 0 && !write_unexamined(conn, state, batch->unexamined, error)) ||
	    (batch->findings->len > 0 && !write_findings(conn, state, batch->findings, error)) ||
	    !write_counters(conn, state, totals, error))
		goto fail;

	state->batches++;
	if (hook != NULL)
		hook("batch", state->batches, hook_data);
	if (!axis_db_conn_commit(conn, error))
		goto fail;

	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	/* What was not committed is not counted. */
	state->findings = findings_before;
	state->new_files = new_before;
	state->changed_files = changed_before;
	state->reappeared = reappeared_before;
	return FALSE;
}

gboolean
axis_library_index_has_present(
	AxisDbConn  *conn,
	gint64       root_id,
	gboolean    *present,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *root = axis_library_int_text(root_id);
	const gchar *params[1];

	*present = FALSE;
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT EXISTS (SELECT 1 FROM library_file WHERE root_id = $1::bigint"
	                       " AND state = 'present')", 1, params, &result, error))
		return FALSE;
	return axis_db_result_get_boolean(result, 0, 0, present);
}

/* ── Ending ─────────────────────────────────────────────────────── */

gboolean
axis_library_index_end(
	AxisDbConn            *conn,
	AxisScanState         *state,
	AxisScanStatus         status,
	AxisScanReason         reason,
	const gchar           *message,
	const AxisWalkTotals  *totals,
	GError               **error
){
	g_autofree gchar *scan = axis_library_int_text(state->scan_id);
	g_autofree gchar *clean = axis_library_text_clean(message, AXIS_LIBRARY_MESSAGE_CHARS);
	const gchar *params[N_COUNTERS + 4];
	Counters counters;
	gboolean ok;

	g_return_val_if_fail(status == AXIS_SCAN_INCOMPLETE || status == AXIS_SCAN_UNAVAILABLE, FALSE);
	g_return_val_if_fail(reason != AXIS_SCAN_REASON_NONE, FALSE);

	params[0] = scan;
	counters_fill(&counters, state, totals, params + 1);
	params[N_COUNTERS + 1] = axis_scan_status_to_string(status);
	params[N_COUNTERS + 2] = axis_scan_reason_to_string(reason);
	params[N_COUNTERS + 3] = clean;
	ok = axis_db_conn_exec(conn,
	                       "UPDATE library_scan SET" COUNTERS_SET ","
	                       " status = $22, reason = $23, message = $24,"
	                       " walk_finished_at = coalesce(walk_finished_at, now()), finished_at = now()"
	                       " WHERE id = $1::bigint AND status = 'running'",
	                       N_COUNTERS + 4, params, NULL, error);
	counters_clear(&counters);

	return ok && axis_library_index_prune_scans(conn, state->root_id, error);
}

gboolean
axis_library_index_prune_scans(
	AxisDbConn  *conn,
	gint64       root_id,
	GError     **error
){
	g_autofree gchar *root = axis_library_int_text(root_id);
	g_autofree gchar *keep = g_strdup_printf("%d", AXIS_LIBRARY_KEEP_SCANS);
	const gchar *params[2];

	params[0] = root;
	params[1] = keep;
	return axis_db_conn_exec(conn,
	                         "DELETE FROM library_scan WHERE root_id = $1::bigint AND status <> 'running'"
	                         " AND id NOT IN (SELECT id FROM library_scan WHERE root_id = $1::bigint"
	                         "   ORDER BY id DESC LIMIT $2::integer)"
	                         " AND id IS DISTINCT FROM (SELECT last_complete_scan_id FROM library_root_state"
	                         "   WHERE root_id = $1::bigint)",
	                         2, params, NULL, error);
}

/*
 * Moves: a row first seen since the root's last complete scan (and seen
 * now) and a row not seen now (and not under anything unexamined) are one
 * file when they are each other's only match: by inode, size and
 * modification time, or else, for a non-empty file, by size and
 * modification time alone.  The new row's facts go into the old row,
 * which keeps its identity; the new row is deleted.
 */
static const gchar moves_sql[] =
	"CREATE TEMP TABLE library_scan_moves ON COMMIT DROP AS"
	" WITH fresh AS ("
	"   SELECT id, inode, size, mtime_sec, mtime_nsec, kind FROM library_file"
	"   WHERE root_id = $1::bigint AND first_seen_generation > $3::bigint"
	"     AND last_seen_generation = $2::bigint AND kind <> 'symlink'"
	" ), gone AS ("
	"   SELECT id, inode, size, mtime_sec, mtime_nsec, kind FROM library_file"
	"   WHERE root_id = $1::bigint AND last_seen_generation < $2::bigint"
	"     AND coalesce(unexamined_generation, 0) < $2::bigint AND kind <> 'symlink'"
	" ), by_inode AS ("
	"   SELECT g.id AS old_id, n.id AS new_id,"
	"     count(*) OVER (PARTITION BY g.id) AS per_old, count(*) OVER (PARTITION BY n.id) AS per_new"
	"   FROM gone g JOIN fresh n ON n.inode = g.inode AND n.size = g.size"
	"     AND n.mtime_sec = g.mtime_sec AND n.mtime_nsec = g.mtime_nsec AND n.kind = g.kind"
	" ), tier1 AS ("
	"   SELECT old_id, new_id FROM by_inode WHERE per_old = 1 AND per_new = 1"
	" ), by_stat AS ("
	"   SELECT g.id AS old_id, n.id AS new_id,"
	"     count(*) OVER (PARTITION BY g.id) AS per_old, count(*) OVER (PARTITION BY n.id) AS per_new"
	"   FROM gone g JOIN fresh n ON n.size = g.size AND n.mtime_sec = g.mtime_sec"
	"     AND n.mtime_nsec = g.mtime_nsec AND n.kind = g.kind AND n.inode <> g.inode"
	"   WHERE g.size > 0 AND g.id NOT IN (SELECT old_id FROM tier1)"
	"     AND n.id NOT IN (SELECT new_id FROM tier1)"
	" ), pairs AS ("
	"   SELECT old_id, new_id, 1 AS tier FROM tier1"
	"   UNION ALL"
	"   SELECT old_id, new_id, 2 AS tier FROM by_stat WHERE per_old = 1 AND per_new = 1"
	" )"
	" SELECT p.old_id, p.tier, n.id AS new_id, n.first_seen_generation AS new_seen,"
	"   n.rel_path, n.name_invalid_utf8, n.kind, n.size,"
	"   n.mtime_sec, n.mtime_nsec, n.ctime_sec, n.ctime_nsec, n.inode, n.nlink, n.link_target,"
	"   n.pinned, n.pinned_links, n.pinned_downloads, n.stat_racy"
	" FROM pairs p JOIN library_file n ON n.id = p.new_id";

static const gchar apply_moves_sql[] =
	"UPDATE library_file o SET"
	" rel_path = m.rel_path,"
	" name_invalid_utf8 = m.name_invalid_utf8,"
	" kind = m.kind,"
	" size = m.size,"
	" mtime_sec = m.mtime_sec,"
	" mtime_nsec = m.mtime_nsec,"
	" ctime_sec = m.ctime_sec,"
	" ctime_nsec = m.ctime_nsec,"
	" inode = m.inode,"
	" nlink = m.nlink,"
	" link_target = m.link_target,"
	" pinned = m.pinned,"
	" pinned_links = m.pinned_links,"
	" pinned_downloads = m.pinned_downloads,"
	" stat_racy = m.stat_racy,"
	" state = 'present',"
	" suspected_at = NULL,"
	" suspected_scan_id = NULL,"
	" suspected_boot_id = NULL,"
	" suspected_boottime_us = NULL,"
	" missing_at = NULL,"
	" last_seen_generation = $2::bigint,"
	/* Same inode, size and time: the same content, unless the old look was
	 * racy.  Matched by size and time alone: look again. */
	" changed = o.changed OR m.tier = 2 OR o.stat_racy,"
	" changed_generation = CASE WHEN m.tier = 2 OR o.stat_racy THEN $2::bigint"
	"   ELSE o.changed_generation END,"
	" content_changed_at = CASE WHEN m.tier = 2 OR o.stat_racy THEN now() ELSE o.content_changed_at END,"
	" moved_at = now()"
	" FROM library_scan_moves m WHERE o.id = m.old_id AND o.root_id = $1::bigint";

gboolean
axis_library_index_complete(
	AxisDbConn              *conn,
	AxisScanState           *state,
	const AxisWalkTotals    *totals,
	AxisScanResult          *result,
	AxisLibraryCommitCheck   check,
	gpointer                 check_data,
	AxisLibraryCommitHook    hook,
	gpointer                 hook_data,
	GError                 **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *root = axis_library_int_text(state->root_id);
	g_autofree gchar *generation = axis_library_int_text(state->generation);
	g_autofree gchar *complete = axis_library_int_text(state->complete_generation);
	g_autofree gchar *scan = axis_library_int_text(state->scan_id);
	g_autofree gchar *boottime = NULL;
	g_autofree gchar *confirm = g_strdup_printf("%d", AXIS_LIBRARY_CONFIRM_SECONDS);
	g_autofree gchar *retention = g_strdup_printf("%d", AXIS_LIBRARY_RETENTION_DAYS);
	g_autofree gchar *moved = NULL;
	g_autofree gchar *suspected = NULL;
	g_autofree gchar *confirmed = NULL;
	g_autofree gchar *removed = NULL;
	g_autofree gchar *present_after = NULL;
	g_autofree gchar *suspected_after = NULL;
	g_autofree gchar *missing_after = NULL;
	g_autofree gchar *pinned_after = NULL;
	const gchar *params[N_COUNTERS + 9];
	guint64 new_before = state->new_files;
	Counters counters;
	gboolean ok;

	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!axis_db_conn_exec(conn, "SET LOCAL statement_timeout = '" AXIS_LIBRARY_FINISH_TIMEOUT "'", 0,
	                       NULL, NULL, error))
		goto fail;

	/* Still this scan's root, at this generation. */
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT generation FROM library_root_state WHERE root_id = $1::bigint"
	                       " FOR UPDATE", 1, params, &rows, error))
		goto fail;
	if (axis_db_result_get_n_rows(rows) != 1 || column_int64(rows, 0, 0) != state->generation) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_BUSY,
			"root %" G_GINT64_FORMAT " was scanned again meanwhile", state->root_id);
		goto fail;
	}
	g_clear_pointer(&rows, axis_db_result_free);

	/* Moves first: a moved file is neither gone nor new. */
	params[0] = root;
	params[1] = generation;
	params[2] = complete;
	if (!axis_db_conn_exec(conn, moves_sql, 3, params, &rows, error))
		goto fail;
	result->moved = axis_db_result_get_affected_rows(rows);
	g_clear_pointer(&rows, axis_db_result_free);
	if (result->moved > 0) {
		/* A move found by this scan was counted as a new path when it was
		 * seen; it is not a new file. */
		if (!axis_db_conn_exec(conn, "SELECT count(*) FROM library_scan_moves"
		                       " WHERE new_seen = $1::bigint", 1, params + 1, &rows, error))
			goto fail;
		state->new_files -= MIN(state->new_files, (guint64)column_int64(rows, 0, 0));
		g_clear_pointer(&rows, axis_db_result_free);
		if (!axis_db_conn_exec(conn, "DELETE FROM library_file WHERE id IN"
		                       " (SELECT new_id FROM library_scan_moves)", 0, NULL, NULL, error) ||
		    !axis_db_conn_exec(conn, apply_moves_sql, 2, params, NULL, error))
			goto fail;
	}

	/* Absent from this complete scan: suspected. */
	boottime = axis_library_int_text(axis_library_boottime_us());
	params[0] = root;
	params[1] = generation;
	params[2] = scan;
	params[3] = state->boot_id;
	params[4] = boottime;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE library_file f SET state = 'suspected', suspected_at = now(),"
	                       " suspected_scan_id = $3::bigint, suspected_boot_id = $4,"
	                       " suspected_boottime_us = $5::bigint"
	                       " WHERE f.root_id = $1::bigint AND f.state = 'present'"
	                       "   AND f.last_seen_generation < $2::bigint"
	                       "   AND coalesce(f.unexamined_generation, 0) < $2::bigint",
	                       5, params, &rows, error))
		goto fail;
	result->suspected = axis_db_result_get_affected_rows(rows);
	g_clear_pointer(&rows, axis_db_result_free);

	/* Suspected by an earlier complete scan, and absent again from this
	 * one, which started at least the confirmation time after it: by the
	 * database's clock, and by the boot clock when both ran in this boot. */
	params[3] = confirm;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE library_file f SET state = 'missing', missing_at = now()"
	                       " FROM library_scan s"
	                       " WHERE s.id = $3::bigint AND f.root_id = $1::bigint AND f.state = 'suspected'"
	                       "   AND f.last_seen_generation < $2::bigint"
	                       "   AND coalesce(f.unexamined_generation, 0) < $2::bigint"
	                       "   AND f.suspected_scan_id <> $3::bigint"
	                       "   AND s.started_at >= f.suspected_at + make_interval(secs => $4::integer)"
	                       "   AND (f.suspected_boot_id IS NULL OR s.boot_id IS NULL"
	                       "     OR f.suspected_boot_id <> s.boot_id OR f.suspected_boottime_us IS NULL"
	                       "     OR s.started_boottime_us >= f.suspected_boottime_us"
	                       "       + $4::bigint * 1000000)",
	                       4, params, &rows, error))
		goto fail;
	result->confirmed = axis_db_result_get_affected_rows(rows);
	g_clear_pointer(&rows, axis_db_result_free);

	/* Missing for the retention time, and absent from this scan too. */
	params[2] = retention;
	if (!axis_db_conn_exec(conn,
	                       "DELETE FROM library_file f"
	                       " WHERE f.root_id = $1::bigint AND f.state = 'missing'"
	                       "   AND f.last_seen_generation < $2::bigint"
	                       "   AND coalesce(f.unexamined_generation, 0) < $2::bigint"
	                       "   AND now() >= f.missing_at + make_interval(days => $3::integer)",
	                       3, params, &rows, error))
		goto fail;
	result->removed = axis_db_result_get_affected_rows(rows);
	g_clear_pointer(&rows, axis_db_result_free);

	/* The folder totals the Library pages show: in this transaction, after
	 * the conclusions above, so that they always describe a complete scan
	 * and an incomplete one leaves them alone. */
	if (!axis_library_index_update_dirs(conn, state->root_id, error))
		goto fail;

	params[0] = root;
	if (!axis_db_conn_exec(conn,
	                       "SELECT count(*) FILTER (WHERE state = 'present'),"
	                       " count(*) FILTER (WHERE state = 'suspected'),"
	                       " count(*) FILTER (WHERE state = 'missing'),"
	                       " count(*) FILTER (WHERE state = 'present' AND pinned)"
	                       " FROM library_file WHERE root_id = $1::bigint",
	                       1, params, &rows, error))
		goto fail;
	result->present_after = (guint64)column_int64(rows, 0, 0);
	result->suspected_after = (guint64)column_int64(rows, 0, 1);
	result->missing_after = (guint64)column_int64(rows, 0, 2);
	result->pinned_after = (guint64)column_int64(rows, 0, 3);
	g_clear_pointer(&rows, axis_db_result_free);

	params[0] = root;
	params[1] = generation;
	params[2] = scan;
	if (!axis_db_conn_exec(conn, "UPDATE library_root_state SET complete_generation = $2::bigint,"
	                       " last_complete_scan_id = $3::bigint, updated_at = now()"
	                       " WHERE root_id = $1::bigint", 3, params, NULL, error))
		goto fail;

	moved = g_strdup_printf("%" G_GUINT64_FORMAT, result->moved);
	suspected = g_strdup_printf("%" G_GUINT64_FORMAT, result->suspected);
	confirmed = g_strdup_printf("%" G_GUINT64_FORMAT, result->confirmed);
	removed = g_strdup_printf("%" G_GUINT64_FORMAT, result->removed);
	present_after = g_strdup_printf("%" G_GUINT64_FORMAT, result->present_after);
	suspected_after = g_strdup_printf("%" G_GUINT64_FORMAT, result->suspected_after);
	missing_after = g_strdup_printf("%" G_GUINT64_FORMAT, result->missing_after);
	pinned_after = g_strdup_printf("%" G_GUINT64_FORMAT, result->pinned_after);
	params[0] = scan;
	counters_fill(&counters, state, totals, params + 1);
	params[N_COUNTERS + 1] = moved;
	params[N_COUNTERS + 2] = suspected;
	params[N_COUNTERS + 3] = confirmed;
	params[N_COUNTERS + 4] = removed;
	params[N_COUNTERS + 5] = present_after;
	params[N_COUNTERS + 6] = suspected_after;
	params[N_COUNTERS + 7] = missing_after;
	params[N_COUNTERS + 8] = pinned_after;
	ok = axis_db_conn_exec(conn,
	                       "UPDATE library_scan SET" COUNTERS_SET ","
	                       " moved = $22::bigint, suspected = $23::bigint, confirmed = $24::bigint,"
	                       " removed = $25::bigint, present_after = $26::bigint,"
	                       " suspected_after = $27::bigint, missing_after = $28::bigint,"
	                       " pinned_after = $29::bigint, status = 'complete',"
	                       " walk_finished_at = coalesce(walk_finished_at, now()), finished_at = now()"
	                       " WHERE id = $1::bigint AND status = 'running'",
	                       N_COUNTERS + 9, params, &rows, error);
	counters_clear(&counters);
	if (!ok)
		goto fail;
	if (axis_db_result_get_affected_rows(rows) != 1) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_FAILED,
			"scan %" G_GINT64_FORMAT " is no longer running", state->scan_id);
		goto fail;
	}
	g_clear_pointer(&rows, axis_db_result_free);

	if (!axis_library_index_prune_scans(conn, state->root_id, error))
		goto fail;

	if (hook != NULL)
		hook("finish", 0, hook_data);
	/* The last word before anything is committed: is the volume still the
	 * one that was walked? */
	if (check != NULL && !check(check_data, error))
		goto fail;
	if (!axis_db_conn_commit(conn, error))
		goto fail;

	result->new_files = state->new_files;
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	state->new_files = new_before;
	result->moved = result->suspected = result->confirmed = result->removed = 0;
	result->present_after = result->suspected_after = result->missing_after = result->pinned_after = 0;
	return FALSE;
}
