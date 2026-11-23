/*
 * axis-analysis-refresh.c - the refresh: every report computed from the
 * index into one snapshot
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One REPEATABLE READ transaction, so that every figure describes the same
 * moment and readers never block the scanner or the probe.  The files
 * of the index that the reports need are first copied into a temporary
 * table of narrow rows (analysis_work: one row per present path, with the
 * probe's summary, and whether the path is the one that counts its bytes),
 * so that each report is one pass over a table that fits in memory.  The
 * statements are constants, or assembled from constant pieces chosen by
 * enumerations; every value reaches the database as a parameter.
 *
 * What is done in SQL stays in SQL.  Only what needs the name normaliser
 * (same-title duplicates, series and albums split across volumes) is read
 * into C, in batches by file id, and its results written back as bounded
 * lists.
 */

#include "library/axis-analysis-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "jobs/axis-job-error.h"
#include "library/axis-analysis-name.h"
#include "library/axis-analysis-rank.h"
#include "library/axis-analysis-rules.h"

#define SNAPSHOT_STAGES 40
#define BATCH_ROWS      "20000"
#define BATCH_ROWS_N    20000

typedef struct
{
	AxisAnalysis   *self;
	AxisDbConn     *conn;
	AxisJobContext *context;
	gint64          started_us;
	gint64          snapshot;
	gchar          *snapshot_text;
	guint           stage;
	gint64          milli[AXIS_ANALYSIS_N_LARGE_CODECS];
	gchar          *milli_text[AXIS_ANALYSIS_N_LARGE_CODECS];
	guint64         name_rows;
	gboolean        names_truncated;
} Refresh;

/* ── Running statements ─────────────────────────────────────────── */

static gboolean
checkpoint(
	Refresh *rf,
	GError **error
){
	if (rf->context != NULL && axis_job_context_is_cancelled(rf->context)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED,
			"the refresh was stopped");
		return FALSE;
	}
	if (g_get_monotonic_time() - rf->started_us > (gint64)AXIS_ANALYSIS_DEADLINE_SECONDS * G_USEC_PER_SEC) {
		g_set_error_literal(error, AXIS_ANALYSIS_ERROR, AXIS_ANALYSIS_ERROR_DEADLINE,
			"the refresh took longer than its deadline");
		return FALSE;
	}
	return TRUE;
}

static void
announce(
	Refresh     *rf,
	const gchar *name
){
	AxisAnalysisTuning tuning;

	rf->stage++;
	if (rf->context != NULL)
		axis_job_context_set_progress(rf->context, MIN(0.99, (gdouble)rf->stage / SNAPSHOT_STAGES), name);
	g_mutex_lock(&rf->self->lock);
	tuning = rf->self->tuning;
	g_mutex_unlock(&rf->self->lock);
	if (tuning.after_stage != NULL)
		tuning.after_stage(name, tuning.data);
}

/* One statement of the refresh with the parameters it names, $1 on. */
static gboolean
run_raw(
	Refresh             *rf,
	const gchar         *name,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	AxisDbResult       **result,
	GError             **error
){
	if (!checkpoint(rf, error))
		return FALSE;
	if (!axis_db_conn_exec(rf->conn, sql, n_params, params, result, error))
		return FALSE;
	announce(rf, name);
	return TRUE;
}

/* A statement whose $1 is the snapshot, and whose $2 on come from @extra. */
static gboolean
run(
	Refresh             *rf,
	const gchar         *name,
	const gchar         *sql,
	guint                n_extra,
	const gchar *const  *extra,
	AxisDbResult       **result,
	GError             **error
){
	const gchar *params[16];
	guint i;

	g_return_val_if_fail(n_extra < 15, FALSE);
	params[0] = rf->snapshot_text;
	for (i = 0; i < n_extra; i++)
		params[1 + i] = extra[i];
	return run_raw(rf, name, sql, 1 + n_extra, params, result, error);
}

/* A statement without parameters. */
static gboolean
run_plain(
	Refresh      *rf,
	const gchar  *name,
	const gchar  *sql,
	GError      **error
){
	return run_raw(rf, name, sql, 0, NULL, NULL, error);
}

/* A statement whose only parameter is the snapshot. */
static gboolean
run_snap(
	Refresh      *rf,
	const gchar  *name,
	const gchar  *sql,
	GError      **error
){
	return run(rf, name, sql, 0, NULL, NULL, error);
}

/* Inserts one fact, with its extra as JSON text. */
static gboolean
insert_fact(
	Refresh      *rf,
	const gchar  *report,
	const gchar  *dim1,
	gint64        paths,
	gint64        bytes,
	const gchar  *extra,
	GError      **error
){
	g_autofree gchar *paths_text = g_strdup_printf("%" G_GINT64_FORMAT, paths);
	g_autofree gchar *bytes_text = g_strdup_printf("%" G_GINT64_FORMAT, bytes);
	const gchar *params[6];

	if (!checkpoint(rf, error))
		return FALSE;
	params[0] = rf->snapshot_text;
	params[1] = report;
	params[2] = dim1;
	params[3] = paths_text;
	params[4] = bytes_text;
	params[5] = extra;
	return axis_db_conn_exec(rf->conn,
		"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)"
		" VALUES ($1::bigint, $2::text, $3::text, $4::bigint, $5::bigint, $6::jsonb)",
		6, params, NULL, error);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

static gboolean
bool_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── The work table ─────────────────────────────────────────────── */

/*
 * One row per present path.  "counted" is true for the one path of a hard
 * linked file that counts its bytes (among the paths of one volume and
 * inode that have more than one link: the lowest id outside a downloads
 * root, so that a film imported by hard link from a download counts as a
 * film, not as a download; else the lowest id) and for every path with one
 * link.
 * "usable" is whether its volume can be read now: files of a volume that
 * cannot are in every total, from the index as it last was.
 */
#define SQL_WORK \
	"CREATE TEMP TABLE analysis_work ON COMMIT DROP AS" \
	" SELECT f.id, f.root_id, f.volume_id, r.media_type, f.kind, f.size, f.pinned, f.changed," \
	"  (h.rep IS NULL OR h.rep = f.id) AS counted," \
	"  v.state IN ('online', 'read_only') AS usable," \
	"  m.status AS probe_status, m.container, m.video_codec AS vcodec, m.resolution_class AS res," \
	"  m.hdr_format AS hdr, coalesce(m.hdr_uncertain, false) AS hdr_uncertain, m.audio_codec AS acodec," \
	"  m.bits_per_pixel AS bpp, coalesce(m.bits_per_pixel_estimated, false) AS bpp_est," \
	"  m.duration_ms, m.bitrate, m.audio_lossless AS lossless" \
	" FROM library_file f" \
	" JOIN library_root r ON r.id = f.root_id" \
	" JOIN volume v ON v.id = f.volume_id" \
	" LEFT JOIN media_info m ON m.file_id = f.id" \
	" LEFT JOIN (SELECT h1.volume_id, h1.inode," \
	"   (array_agg(h1.id ORDER BY (h2.media_type = 'downloads'), h1.id))[1] AS rep" \
	"   FROM library_file h1 JOIN library_root h2 ON h2.id = h1.root_id" \
	"   WHERE h1.state = 'present' AND h1.nlink > 1 GROUP BY h1.volume_id, h1.inode) h" \
	"   ON f.nlink > 1 AND h.volume_id = f.volume_id AND h.inode = f.inode" \
	" WHERE f.state = 'present'"

/* ── Space ──────────────────────────────────────────────────────── */

#define SQL_OVERALL \
	"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'overall', '', count(*), coalesce(sum(size) FILTER (WHERE counted), 0)," \
	"  jsonb_build_object('linked_paths', count(*) FILTER (WHERE NOT counted)," \
	"   'offline_paths', count(*) FILTER (WHERE NOT usable)," \
	"   'offline_bytes', coalesce(sum(size) FILTER (WHERE counted AND NOT usable), 0))" \
	" FROM analysis_work"

#define SQL_SPACE_VOLUME \
	"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'space_volume', v.id::text, coalesce(a.paths, 0), coalesce(a.bytes, 0)," \
	"  jsonb_build_object('name', v.name, 'state', v.state," \
	"   'usable', v.state IN ('online', 'read_only')," \
	"   'total_bytes', v.total_bytes, 'free_bytes', v.free_bytes," \
	"   'space_checked_us', (extract(epoch FROM v.space_checked_at) * 1000000)::bigint," \
	"   'linked_paths', coalesce(a.linked, 0))" \
	" FROM volume v LEFT JOIN (SELECT volume_id, count(*) AS paths," \
	"   coalesce(sum(size) FILTER (WHERE counted), 0) AS bytes, count(*) FILTER (WHERE NOT counted) AS linked" \
	"   FROM analysis_work GROUP BY volume_id) a ON a.volume_id = v.id"

#define SQL_SPACE_ROOT \
	"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'space_root', r.id::text, coalesce(a.paths, 0), coalesce(a.bytes, 0)," \
	"  jsonb_build_object('name', r.name, 'media_type', r.media_type, 'volume_id', r.volume_id," \
	"   'linked_paths', coalesce(a.linked, 0), 'complete_scan', coalesce(st.complete_generation, 0) > 0," \
	"   'last_complete_us', (extract(epoch FROM c.finished_at) * 1000000)::bigint)" \
	" FROM library_root r" \
	" LEFT JOIN library_root_state st ON st.root_id = r.id" \
	" LEFT JOIN library_scan c ON c.id = st.last_complete_scan_id" \
	" LEFT JOIN (SELECT root_id, count(*) AS paths, coalesce(sum(size) FILTER (WHERE counted), 0) AS bytes," \
	"   count(*) FILTER (WHERE NOT counted) AS linked FROM analysis_work GROUP BY root_id) a ON a.root_id = r.id"

/* A breakdown by a column of the work table.  The report and the column
 * are constants of this file. */
static const struct
{
	const gchar *report;
	const gchar *column;
	const gchar *label;
} breakdowns[] = {
	{ "space_type",     "media_type", "category" },
	{ "space_kind",     "kind",       "file kind" },
	{ "comp_vcodec",    "vcodec",     "video codec" },
	{ "comp_res",       "res",        "resolution class" },
	{ "comp_hdr",       "hdr",        "HDR format" },
	{ "comp_container", "container",  "container" },
	{ "comp_acodec",    "acodec",     "audio codec" },
	{ NULL, NULL, NULL }
};

static gchar *
breakdown_sql(
	const gchar *report,
	const gchar *column
){
	return g_strdup_printf(
		"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)"
		" SELECT $1::bigint, '%s', %s, count(*), coalesce(sum(size) FILTER (WHERE counted), 0),"
		"  jsonb_build_object('linked_paths', count(*) FILTER (WHERE NOT counted),"
		"   'offline_paths', count(*) FILTER (WHERE NOT usable))"
		" FROM analysis_work WHERE %s IS NOT NULL AND octet_length(%s) <= 200 GROUP BY %s",
		report, column, column, column, column);
}

/* ── Bit rates ──────────────────────────────────────────────────── */

/* The histogram's buckets as a table: bucket k holds bits per pixel in
 * [lo, hi) thousandths, the last one open.  A figure and the list it links
 * to compare "bits_per_pixel * 1000" with the same two numbers. */
#define BUCKETS_VALUES \
	"(VALUES (0, 0, 50), (1, 50, 100), (2, 100, 150), (3, 150, 200), (4, 200, 250), (5, 250, 300)," \
	" (6, 300, 350), (7, 350, 400), (8, 400, 450), (9, 450, 500), (10, 500, 1000000000000))"

G_STATIC_ASSERT(AXIS_ANALYSIS_BPP_BUCKETS == 11);
G_STATIC_ASSERT(AXIS_ANALYSIS_BPP_BUCKET == 50);

#define SQL_BPP \
	"WITH cells AS (" \
	" SELECT w.res, w.vcodec, b.k, count(*) AS n, coalesce(sum(w.size) FILTER (WHERE w.counted), 0) AS bytes," \
	"  count(*) FILTER (WHERE w.bpp_est) AS est" \
	" FROM analysis_work w JOIN " BUCKETS_VALUES " b(k, lo, hi)" \
	"   ON w.bpp * 1000 >= b.lo AND w.bpp * 1000 < b.hi" \
	" WHERE w.bpp IS NOT NULL AND w.res IS NOT NULL AND w.vcodec IS NOT NULL AND octet_length(w.vcodec) <= 200" \
	" GROUP BY w.res, w.vcodec, b.k)," \
	" pct AS (" \
	" SELECT res, vcodec, percentile_cont(0.5) WITHIN GROUP (ORDER BY bpp) AS median," \
	"  percentile_cont(0.95) WITHIN GROUP (ORDER BY bpp) AS p95" \
	" FROM analysis_work WHERE bpp IS NOT NULL AND res IS NOT NULL AND vcodec IS NOT NULL" \
	"  AND octet_length(vcodec) <= 200 GROUP BY res, vcodec)" \
	" INSERT INTO analysis_fact (snapshot_id, report, dim1, dim2, paths, bytes, extra)" \
	" SELECT $1::bigint, 'bpp', c.res, c.vcodec, sum(c.n), sum(c.bytes)," \
	"  jsonb_build_object('buckets', jsonb_agg(jsonb_build_array(c.k, c.n, c.bytes) ORDER BY c.k)," \
	"   'estimated', sum(c.est), 'median', p.median, 'p95', p.p95)" \
	" FROM cells c JOIN pct p ON p.res = c.res AND p.vcodec = c.vcodec GROUP BY c.res, c.vcodec, p.median, p.p95"

/* $2 to $5: the thresholds, thousandths, in the order of the codecs. */
#define THRESHOLDS_VALUES \
	"(VALUES ('h264', $2::float8), ('hevc', $3::float8), ('av1', $4::float8), ('vp9', $5::float8))"

#define SQL_LARGE \
	"INSERT INTO analysis_fact (snapshot_id, report, dim1, dim2, paths, bytes, extra)" \
	" SELECT $1::bigint, 'large', w.res, w.vcodec, count(*), coalesce(sum(w.size) FILTER (WHERE w.counted), 0)," \
	"  jsonb_build_object('threshold_milli', t.th::bigint, 'estimated', count(*) FILTER (WHERE w.bpp_est)," \
	"   'offline_paths', count(*) FILTER (WHERE NOT w.usable))" \
	" FROM analysis_work w JOIN " THRESHOLDS_VALUES " t(codec, th) ON t.codec = w.vcodec" \
	" WHERE w.bpp IS NOT NULL AND w.res IS NOT NULL AND w.bpp * 1000 >= t.th" \
	" GROUP BY w.res, w.vcodec, t.th"

#define SQL_LARGE_LIST \
	"WITH big AS (" \
	" SELECT w.id, w.size, w.res, w.vcodec, w.bpp, w.root_id, w.volume_id, w.usable, w.counted" \
	" FROM analysis_work w JOIN " THRESHOLDS_VALUES " t(codec, th) ON t.codec = w.vcodec" \
	" WHERE w.bpp IS NOT NULL AND w.res IS NOT NULL AND w.bpp * 1000 >= t.th AND w.counted)" \
	" INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'large_list', '', (SELECT count(*) FROM big)," \
	"  (SELECT coalesce(sum(size), 0) FROM big)," \
	"  jsonb_build_object('items', coalesce((SELECT jsonb_agg(jsonb_build_object('id', t.id, 'size', t.size," \
	"    'path', f.rel_path, 'root_id', t.root_id, 'volume_id', t.volume_id, 'bpp', t.bpp, 'codec', t.vcodec," \
	"    'res', t.res, 'usable', t.usable) ORDER BY t.size DESC, t.id DESC)" \
	"   FROM (SELECT * FROM big ORDER BY size DESC, id DESC LIMIT 100) t JOIN library_file f ON f.id = t.id)," \
	"   '[]'::jsonb))"

/* ── Largest ────────────────────────────────────────────────────── */

#define SQL_LARGEST_FILES \
	"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'largest_files', '', (SELECT count(*) FROM analysis_work WHERE counted)," \
	"  (SELECT coalesce(sum(size), 0) FROM analysis_work WHERE counted)," \
	"  jsonb_build_object('items', coalesce((SELECT jsonb_agg(jsonb_build_object('id', t.id, 'size', t.size," \
	"    'path', f.rel_path, 'root_id', t.root_id, 'volume_id', t.volume_id, 'nlink', f.nlink," \
	"    'usable', t.usable, 'kind', t.kind) ORDER BY t.size DESC, t.id DESC)" \
	"   FROM (SELECT id, size, root_id, volume_id, usable, kind FROM analysis_work WHERE counted" \
	"     ORDER BY size DESC, id DESC LIMIT 100) t JOIN library_file f ON f.id = t.id), '[]'::jsonb))"

/* Folders directly below a root's top: a film, a show, an artist.  Their
 * totals are those of the Library pages (every path counted). */
#define SQL_LARGEST_DIRS \
	"INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'largest_dirs', ''," \
	"  (SELECT count(*) FROM library_dir d JOIN library_dir p ON p.id = d.parent_id AND p.path = '')," \
	"  (SELECT coalesce(sum(d.total_bytes), 0) FROM library_dir d JOIN library_dir p ON p.id = d.parent_id" \
	"    AND p.path = '')," \
	"  jsonb_build_object('items', coalesce((SELECT jsonb_agg(jsonb_build_object('id', t.id, 'root_id', t.root_id," \
	"    'name', t.name, 'files', t.total_files, 'bytes', t.total_bytes, 'volume_id', r.volume_id," \
	"    'usable', v.state IN ('online', 'read_only')) ORDER BY t.total_bytes DESC, t.id DESC)" \
	"   FROM (SELECT d.id, d.root_id, d.name, d.total_files, d.total_bytes FROM library_dir d" \
	"     JOIN library_dir p ON p.id = d.parent_id AND p.path = ''" \
	"     ORDER BY d.total_bytes DESC, d.id DESC LIMIT 100) t" \
	"   JOIN library_root r ON r.id = t.root_id JOIN volume v ON v.id = r.volume_id), '[]'::jsonb))"

/* ── Duplicates: the same content ───────────────────────────────── */

/*
 * Files (one path per hard linked file) with the same size, the same
 * duration and the same stream layout.  Two paths of one hard linked file
 * are one candidate, so they are never a pair.
 */
#define SQL_DUP_CONTENT \
	"WITH cand AS (" \
	" SELECT id, size, duration_ms FROM analysis_work" \
	" WHERE counted AND probe_status = 'ok' AND kind IN ('video', 'audio') AND duration_ms > 0 AND size > 0)," \
	" pairs AS (SELECT size, duration_ms FROM cand GROUP BY size, duration_ms HAVING count(*) > 1)," \
	" sig AS (" \
	" SELECT c.id, c.size, c.duration_ms," \
	"  string_agg(concat_ws(':', s.kind, s.codec, s.width, s.height, s.channels, s.sample_rate), ','" \
	"    ORDER BY s.idx) AS layout" \
	" FROM cand c JOIN pairs p ON p.size = c.size AND p.duration_ms = c.duration_ms" \
	" JOIN media_stream s ON s.file_id = c.id GROUP BY c.id, c.size, c.duration_ms)," \
	" grp AS (" \
	" SELECT size, duration_ms, layout, count(*) AS n, array_agg(id ORDER BY id) AS ids" \
	" FROM sig GROUP BY size, duration_ms, layout HAVING count(*) > 1)," \
	" ranked AS (" \
	" SELECT g.*, row_number() OVER (ORDER BY (g.n - 1) * g.size DESC, g.size DESC, g.ids[1]) AS rn," \
	"  count(*) OVER () AS total_groups, sum((g.n - 1) * g.size) OVER () AS total_wasted FROM grp g)," \
	" top AS (SELECT * FROM ranked WHERE rn <= 200)" \
	" INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)" \
	" SELECT $1::bigint, 'dup_content', '', coalesce(max(t.total_groups), 0)::bigint," \
	"  coalesce(max(t.total_wasted), 0)::bigint," \
	"  jsonb_build_object('items', coalesce(jsonb_agg(jsonb_build_object(" \
	"   'size', t.size, 'duration_ms', t.duration_ms, 'layout', left(t.layout, 400), 'copies', t.n," \
	"   'wasted', (t.n - 1) * t.size," \
	"   'members', (SELECT jsonb_agg(jsonb_build_object('id', f.id, 'root_id', f.root_id," \
	"      'volume_id', f.volume_id, 'path', f.rel_path, 'size', f.size, 'probed', true, 'res', w.res," \
	"      'hdr', w.hdr, 'bpp', w.bpp, 'acodec', w.acodec, 'lossless', w.lossless, 'pinned', f.pinned," \
	"      'usable', w.usable) ORDER BY f.id)" \
	"     FROM unnest(t.ids[1:24]) AS u(id) JOIN library_file f ON f.id = u.id JOIN analysis_work w ON w.id = f.id)" \
	"  ) ORDER BY t.rn), '[]'::jsonb))" \
	" FROM top t"

/* ── Leftovers ──────────────────────────────────────────────────── */

static gchar *
leftover_sql(
	const gchar *id,
	const gchar *condition,
	gboolean     items
){
	return g_strdup_printf(
		"WITH hits AS MATERIALIZED ("
		" SELECT w.id, w.size, w.counted, w.usable, w.root_id, w.volume_id, f.rel_path"
		" FROM analysis_work w JOIN library_file f ON f.id = w.id JOIN library_root r ON r.id = f.root_id"
		" WHERE %s)"
		" INSERT INTO analysis_fact (snapshot_id, report, dim1, paths, bytes, extra)"
		" SELECT $1::bigint, 'leftover', '%s', count(*), coalesce(sum(size) FILTER (WHERE counted), 0),"
		"  jsonb_build_object('linked_paths', count(*) FILTER (WHERE NOT counted),"
		"   'offline_paths', count(*) FILTER (WHERE NOT usable)%s)"
		" FROM hits",
		condition, id,
		items ?
		", 'items', coalesce((SELECT jsonb_agg(jsonb_build_object('id', t.id, 'size', t.size,"
		"   'path', t.rel_path, 'root_id', t.root_id, 'volume_id', t.volume_id, 'usable', t.usable)"
		"   ORDER BY t.size DESC, t.id DESC)"
		"  FROM (SELECT * FROM hits WHERE counted ORDER BY size DESC, id DESC LIMIT 50) t), '[]'::jsonb)" :
		"");
}

/* ── Coverage ───────────────────────────────────────────────────── */

#define SQL_COVERAGE \
	"UPDATE analysis_snapshot s SET paths = o.paths, bytes = o.bytes," \
	" volumes_total = vt.total, volumes_used = vt.used," \
	" coverage = jsonb_build_object(" \
	"  'present_paths', o.paths, 'media_paths', o.media, 'not_probed', o.not_probed," \
	"  'probe_failed', o.failed, 'probe_timeout', o.timeout, 'hdr_uncertain', o.uncertain," \
	"  'probe_stale', o.stale, 'pinned_paths', o.pinned, 'offline_paths', o.offline_paths," \
	"  'offline_bytes', o.offline_bytes, 'no_bpp', o.no_bpp, 'unassessed_codec', o.unassessed," \
	"  'suspected_paths', ab.suspected_paths, 'suspected_bytes', ab.suspected_bytes," \
	"  'missing_paths', ab.missing_paths, 'missing_bytes', ab.missing_bytes," \
	"  'roots', (SELECT count(*) FROM library_root)," \
	"  'roots_unscanned', (SELECT coalesce(jsonb_agg(jsonb_build_object('id', r.id, 'name', r.name)" \
	"     ORDER BY r.id), '[]'::jsonb) FROM library_root r" \
	"     LEFT JOIN library_root_state st ON st.root_id = r.id WHERE coalesce(st.complete_generation, 0) = 0)," \
	"  'volumes_unavailable', (SELECT coalesce(jsonb_agg(jsonb_build_object('id', v.id, 'name', v.name," \
	"     'state', v.state, 'paths', coalesce(a.paths, 0), 'bytes', coalesce(a.bytes, 0)) ORDER BY v.id)," \
	"     '[]'::jsonb) FROM volume v LEFT JOIN (SELECT volume_id, count(*) AS paths," \
	"     coalesce(sum(size) FILTER (WHERE counted), 0) AS bytes FROM analysis_work GROUP BY volume_id) a" \
	"     ON a.volume_id = v.id WHERE v.state NOT IN ('online', 'read_only')))" \
	" FROM (SELECT count(*) AS paths, coalesce(sum(size) FILTER (WHERE counted), 0) AS bytes," \
	"   count(*) FILTER (WHERE kind IN ('video', 'audio')) AS media," \
	"   count(*) FILTER (WHERE kind IN ('video', 'audio') AND probe_status IS NULL) AS not_probed," \
	"   count(*) FILTER (WHERE probe_status = 'failed') AS failed," \
	"   count(*) FILTER (WHERE probe_status = 'timeout') AS timeout," \
	"   count(*) FILTER (WHERE hdr_uncertain) AS uncertain," \
	"   count(*) FILTER (WHERE changed AND probe_status = 'ok') AS stale," \
	"   count(*) FILTER (WHERE pinned) AS pinned," \
	"   count(*) FILTER (WHERE NOT usable) AS offline_paths," \
	"   coalesce(sum(size) FILTER (WHERE counted AND NOT usable), 0) AS offline_bytes," \
	"   count(*) FILTER (WHERE probe_status = 'ok' AND vcodec IS NOT NULL AND bpp IS NULL) AS no_bpp," \
	"   count(*) FILTER (WHERE bpp IS NOT NULL AND vcodec NOT IN ('h264', 'hevc', 'av1', 'vp9')) AS unassessed" \
	"  FROM analysis_work) o," \
	" (SELECT count(*) FILTER (WHERE state = 'suspected') AS suspected_paths," \
	"   coalesce(sum(size) FILTER (WHERE state = 'suspected'), 0) AS suspected_bytes," \
	"   count(*) FILTER (WHERE state = 'missing') AS missing_paths," \
	"   coalesce(sum(size) FILTER (WHERE state = 'missing'), 0) AS missing_bytes" \
	"  FROM library_file WHERE state <> 'present') ab," \
	" (SELECT sum(total_bytes) AS total, sum(total_bytes - free_bytes) AS used FROM volume" \
	"  WHERE total_bytes IS NOT NULL AND free_bytes IS NOT NULL) vt" \
	" WHERE s.id = $1::bigint"

/* ── JSON ───────────────────────────────────────────────────────── */

static gchar *
builder_text(JsonBuilder *builder)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();
	JsonNode *root = json_builder_get_root(builder);
	gchar *text;

	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);
	json_node_unref(root);
	return text;
}

static void
add_int(
	JsonBuilder *builder,
	const gchar *name,
	gint64       value
){
	json_builder_set_member_name(builder, name);
	json_builder_add_int_value(builder, value);
}

static void
add_string(
	JsonBuilder *builder,
	const gchar *name,
	const gchar *value
){
	json_builder_set_member_name(builder, name);
	if (value != NULL)
		json_builder_add_string_value(builder, value);
	else
		json_builder_add_null_value(builder);
}

static void
add_bool(
	JsonBuilder *builder,
	const gchar *name,
	gboolean     value
){
	json_builder_set_member_name(builder, name);
	json_builder_add_boolean_value(builder, value);
}

/* ── Names: same-title duplicates ───────────────────────────────── */

typedef struct
{
	AxisAnalysisCopy copy;
	gint64           root_id;
	gint64           volume_id;
	gboolean         pinned;
	gboolean         usable;
	gchar           *res;
	gchar           *hdr;
	gchar           *acodec;
	gchar           *path;
} Member;

static void
member_free(gpointer data)
{
	Member *member = data;

	g_free(member->res);
	g_free(member->hdr);
	g_free(member->acodec);
	g_free(member->path);
	g_free(member);
}

typedef struct
{
	gchar     *key;
	GPtrArray *members;	/* Member *, best first once ranked */
	GArray    *ids;
	gint64     reclaimable;
} Group;

static void
group_free(gpointer data)
{
	Group *group = data;

	g_free(group->key);
	g_ptr_array_unref(group->members);
	g_array_unref(group->ids);
	g_free(group);
}

static void
add_to_groups(
	GHashTable *groups,
	gchar      *key,
	gint64      id
){
	GArray *ids = g_hash_table_lookup(groups, key);

	if (ids != NULL) {
		g_array_append_val(ids, id);
		g_free(key);
		return;
	}
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_array_append_val(ids, id);
	g_hash_table_insert(groups, key, ids);
}

/* Reads one kind of name in batches by file id into @groups; returns how
 * many names were readable. */
typedef enum
{
	NAMES_FILM,
	NAMES_EPISODE,
	NAMES_TRACK
} NamesKind;

static gboolean
read_names(
	Refresh      *rf,
	NamesKind     kind,
	GHashTable   *groups,
	gint64       *considered,
	GError      **error
){
	g_autofree gchar *excluded = g_strdup_printf("(%s OR %s)", axis_leftover_condition("sample"),
		axis_leftover_condition("extras"));
	g_autofree gchar *sql = NULL;
	gint64 after = 0;
	const gchar *name = kind == NAMES_FILM ? "reading movie names" :
		kind == NAMES_EPISODE ? "reading episode names" : "reading track tags";

	*considered = 0;
	switch (kind) {
	case NAMES_FILM:
		sql = g_strdup_printf(
			"SELECT w.id, f.rel_path, '', '' FROM analysis_work w JOIN library_file f ON f.id = w.id"
			" JOIN library_root r ON r.id = w.root_id"
			" WHERE w.counted AND w.kind = 'video' AND w.media_type = 'movies' AND w.id > $1::bigint"
			" AND f.rel_path ~ '(19|20)[0-9]{2}' AND NOT %s ORDER BY w.id LIMIT " BATCH_ROWS, excluded);
		break;
	case NAMES_EPISODE:
		sql = g_strdup_printf(
			"SELECT w.id, f.rel_path, '', '' FROM analysis_work w JOIN library_file f ON f.id = w.id"
			" JOIN library_root r ON r.id = w.root_id"
			" WHERE w.counted AND w.kind = 'video' AND w.media_type = 'tv' AND w.id > $1::bigint"
			" AND f.rel_path ~* '[0-9]x[0-9][0-9]|s[0-9]+e[0-9]+' AND NOT %s ORDER BY w.id LIMIT " BATCH_ROWS,
			excluded);
		break;
	case NAMES_TRACK:
	default:
		sql = g_strdup(
			"SELECT w.id, m.tag_artist, m.tag_album, m.tag_title FROM analysis_work w"
			" JOIN media_info m ON m.file_id = w.id"
			" WHERE w.counted AND w.kind = 'audio' AND w.media_type = 'music' AND w.id > $1::bigint"
			" AND m.tag_artist IS NOT NULL AND m.tag_album IS NOT NULL AND m.tag_title IS NOT NULL"
			" ORDER BY w.id LIMIT " BATCH_ROWS);
		break;
	}

	for (;;) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *after_text = g_strdup_printf("%" G_GINT64_FORMAT, after);
		const gchar *extra[1];
		guint n;
		guint i;

		extra[0] = after_text;
		if (!run_raw(rf, name, sql, 1, extra, &result, error))
			return FALSE;
		n = axis_db_result_get_n_rows(result);
		for (i = 0; i < n; i++) {
			gint64 id = int_at(result, i, 0, 0);
			gchar *key;

			after = id;
			if (kind == NAMES_FILM)
				key = axis_analysis_film_key(axis_db_result_get_text(result, i, 1));
			else if (kind == NAMES_EPISODE)
				key = axis_analysis_episode_key(axis_db_result_get_text(result, i, 1));
			else
				key = axis_analysis_track_key(axis_db_result_get_text(result, i, 1),
					axis_db_result_get_text(result, i, 2), axis_db_result_get_text(result, i, 3));
			if (key == NULL)
				continue;
			(*considered)++;
			add_to_groups(groups, key, id);
		}
		rf->name_rows += n;
		if (n < BATCH_ROWS_N)
			break;
		if (rf->name_rows >= AXIS_ANALYSIS_NAME_MAX_ROWS) {
			rf->names_truncated = TRUE;
			break;
		}
	}
	return TRUE;
}

static gint
compare_members(
	gconstpointer a,
	gconstpointer b
){
	const Member *x = *(Member *const *)a;
	const Member *y = *(Member *const *)b;

	return axis_analysis_copy_compare(&x->copy, &y->copy);
}

static gint
compare_groups(
	gconstpointer a,
	gconstpointer b
){
	const Group *x = *(Group *const *)a;
	const Group *y = *(Group *const *)b;

	if (x->reclaimable != y->reclaimable)
		return x->reclaimable > y->reclaimable ? -1 : 1;
	return strcmp(x->key, y->key);
}

/* Fills the members of @list's groups from the work table, @chunk ids at a
 * time. */
static gboolean
load_members(
	Refresh      *rf,
	GPtrArray    *list,
	GHashTable   *by_id,
	GError      **error
){
	GArray *all = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint i;
	guint at = 0;
	gboolean ok = TRUE;

	for (i = 0; i < list->len; i++) {
		const Group *group = g_ptr_array_index(list, i);

		g_array_append_vals(all, group->ids->data, group->ids->len);
	}
	while (ok && at < all->len) {
		g_autoptr(AxisDbResult) result = NULL;
		GString *text = g_string_new("{");
		g_autofree gchar *array = NULL;
		const gchar *extra[1];
		guint end = MIN(at + 2000, all->len);
		guint k;
		guint n;

		for (k = at; k < end; k++)
			g_string_append_printf(text, "%s%" G_GINT64_FORMAT, k > at ? "," : "",
				g_array_index(all, gint64, k));
		g_string_append_c(text, '}');
		array = g_string_free(text, FALSE);
		extra[0] = array;
		ok = run_raw(rf, "ranking copies",
			"SELECT id, root_id, volume_id, size, probe_status = 'ok', res, hdr, hdr_uncertain, lossless,"
			" bpp, bitrate, pinned, usable, acodec FROM analysis_work WHERE id = ANY($1::bigint[])",
			1, extra, &result, error);
		if (!ok)
			break;
		n = axis_db_result_get_n_rows(result);
		for (k = 0; k < n; k++) {
			Member *member = g_new0(Member, 1);
			gboolean lossless = FALSE;
			gdouble bpp = -1;
			const gchar *text_bpp = axis_db_result_get_text(result, k, 9);

			member->copy.id = int_at(result, k, 0, 0);
			member->root_id = int_at(result, k, 1, 0);
			member->volume_id = int_at(result, k, 2, 0);
			member->copy.size = int_at(result, k, 3, 0);
			member->copy.probed = bool_at(result, k, 4);
			member->res = g_strdup(axis_db_result_get_text(result, k, 5));
			member->hdr = g_strdup(axis_db_result_get_text(result, k, 6));
			member->copy.resolution = member->res;
			member->copy.hdr = member->hdr;
			member->copy.hdr_uncertain = bool_at(result, k, 7);
			member->copy.lossless = axis_db_result_is_null(result, k, 8) ? -1 :
				(axis_db_result_get_boolean(result, k, 8, &lossless) && lossless ? 1 : 0);
			if (text_bpp != NULL)
				bpp = g_ascii_strtod(text_bpp, NULL);
			member->copy.bits_per_pixel = bpp;
			member->copy.bitrate = int_at(result, k, 10, -1);
			member->pinned = bool_at(result, k, 11);
			member->usable = bool_at(result, k, 12);
			member->acodec = g_strdup(axis_db_result_get_text(result, k, 13));
			g_hash_table_insert(by_id, &member->copy.id, member);
		}
		at = end;
	}
	g_array_unref(all);
	return ok;
}

static gboolean
load_paths(
	Refresh      *rf,
	GPtrArray    *shown,
	GError      **error
){
	GHashTable *by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(AxisDbResult) result = NULL;
	GString *text = g_string_new("{");
	g_autofree gchar *array = NULL;
	const gchar *extra[1];
	gboolean first = TRUE;
	gboolean ok;
	guint i;
	guint k;

	for (i = 0; i < shown->len; i++) {
		const Group *group = g_ptr_array_index(shown, i);

		for (k = 0; k < group->members->len && k < AXIS_ANALYSIS_GROUP_MEMBERS; k++) {
			Member *member = g_ptr_array_index(group->members, k);

			g_hash_table_insert(by_id, &member->copy.id, member);
			g_string_append_printf(text, "%s%" G_GINT64_FORMAT, first ? "" : ",", member->copy.id);
			first = FALSE;
		}
	}
	g_string_append_c(text, '}');
	array = g_string_free(text, FALSE);
	extra[0] = array;
	ok = run_raw(rf, "reading paths", "SELECT id, rel_path FROM library_file WHERE id = ANY($1::bigint[])", 1,
		extra, &result, error);
	if (ok) {
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			gint64 id = int_at(result, i, 0, 0);
			Member *member = g_hash_table_lookup(by_id, &id);

			if (member != NULL) {
				g_free(member->path);
				member->path = g_strdup(axis_db_result_get_text(result, i, 1));
			}
		}
	}
	g_hash_table_unref(by_id);
	return ok;
}

static void
add_group_item(
	JsonBuilder *builder,
	const Group *group
){
	g_auto(GStrv) parts = g_strsplit(group->key, "|", -1);
	guint i;

	json_builder_begin_object(builder);
	add_string(builder, "kind", parts[0]);
	json_builder_set_member_name(builder, "parts");
	json_builder_begin_array(builder);
	for (i = 1; parts[i] != NULL; i++)
		json_builder_add_string_value(builder, parts[i]);
	json_builder_end_array(builder);
	add_int(builder, "copies", group->members->len);
	add_int(builder, "reclaimable", group->reclaimable);
	json_builder_set_member_name(builder, "members");
	json_builder_begin_array(builder);
	for (i = 0; i < group->members->len && i < AXIS_ANALYSIS_GROUP_MEMBERS; i++) {
		const Member *member = g_ptr_array_index(group->members, i);

		json_builder_begin_object(builder);
		add_int(builder, "id", member->copy.id);
		add_int(builder, "root_id", member->root_id);
		add_int(builder, "volume_id", member->volume_id);
		add_string(builder, "path", member->path != NULL ? member->path : "");
		add_int(builder, "size", member->copy.size);
		add_string(builder, "res", member->res);
		add_string(builder, "hdr", member->hdr);
		add_string(builder, "acodec", member->acodec);
		json_builder_set_member_name(builder, "lossless");
		if (member->copy.lossless >= 0)
			json_builder_add_boolean_value(builder, member->copy.lossless > 0);
		else
			json_builder_add_null_value(builder);
		json_builder_set_member_name(builder, "bpp");
		if (member->copy.bits_per_pixel >= 0)
			json_builder_add_double_value(builder, member->copy.bits_per_pixel);
		else
			json_builder_add_null_value(builder);
		add_bool(builder, "probed", member->copy.probed);
		add_bool(builder, "pinned", member->pinned);
		add_bool(builder, "usable", member->usable);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
}

static gboolean
dup_title(
	Refresh  *rf,
	GError  **error
){
	GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_array_unref);
	GPtrArray *list = g_ptr_array_new_with_free_func(group_free);
	GHashTable *by_id = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, member_free);
	g_autoptr(AxisDbResult) counts = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *extra = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 considered[3] = { 0, 0, 0 };
	gint64 total_reclaimable = 0;
	gint64 total_groups = 0;
	guint shown_count;
	guint i;
	gboolean ok = FALSE;

	if (!read_names(rf, NAMES_FILM, groups, &considered[0], error) ||
	    !read_names(rf, NAMES_EPISODE, groups, &considered[1], error) ||
	    !read_names(rf, NAMES_TRACK, groups, &considered[2], error))
		goto out;

	g_hash_table_iter_init(&iter, groups);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GArray *ids = value;

		if (ids->len >= 2) {
			Group *group = g_new0(Group, 1);

			group->key = g_strdup(key);
			group->ids = g_array_ref(ids);
			group->members = g_ptr_array_new();
			g_ptr_array_add(list, group);
		}
	}
	if (!load_members(rf, list, by_id, error))
		goto out;

	/* Members from the work table; a name read a moment ago is in it. */
	for (i = 0; i < list->len; i++) {
		Group *group = g_ptr_array_index(list, i);
		guint k;

		for (k = 0; k < group->ids->len; k++) {
			gint64 id = g_array_index(group->ids, gint64, k);
			Member *member = g_hash_table_lookup(by_id, &id);

			if (member != NULL)
				g_ptr_array_add(group->members, member);
		}
		g_ptr_array_sort(group->members, compare_members);
		group->reclaimable = 0;
		for (k = 1; k < group->members->len; k++)
			group->reclaimable += ((Member *)g_ptr_array_index(group->members, k))->copy.size;
	}
	/* A group the work table could not complete is no group. */
	for (i = 0; i < list->len;) {
		Group *group = g_ptr_array_index(list, i);

		if (group->members->len < 2)
			g_ptr_array_remove_index(list, i);
		else
			i++;
	}
	g_ptr_array_sort(list, compare_groups);
	total_groups = list->len;
	for (i = 0; i < list->len; i++)
		total_reclaimable += ((Group *)g_ptr_array_index(list, i))->reclaimable;

	shown_count = MIN(list->len, (guint)AXIS_ANALYSIS_GROUPS);
	{
		g_autoptr(GPtrArray) shown = g_ptr_array_new();

		for (i = 0; i < shown_count; i++)
			g_ptr_array_add(shown, g_ptr_array_index(list, i));
		if (shown->len > 0 && !load_paths(rf, shown, error))
			goto out;
	}

	if (!run_raw(rf, "counting names",
	        "SELECT count(*) FILTER (WHERE media_type = 'movies' AND kind = 'video'),"
	        " count(*) FILTER (WHERE media_type = 'tv' AND kind = 'video'),"
	        " count(*) FILTER (WHERE media_type = 'music' AND kind = 'audio') FROM analysis_work WHERE counted",
	        0, NULL, &counts, error))
		goto out;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "considered");
	json_builder_begin_object(builder);
	add_int(builder, "film", considered[0]);
	add_int(builder, "episode", considered[1]);
	add_int(builder, "track", considered[2]);
	json_builder_end_object(builder);
	json_builder_set_member_name(builder, "candidates");
	json_builder_begin_object(builder);
	add_int(builder, "film", int_at(counts, 0, 0, 0));
	add_int(builder, "episode", int_at(counts, 0, 1, 0));
	add_int(builder, "track", int_at(counts, 0, 2, 0));
	json_builder_end_object(builder);
	add_bool(builder, "truncated", rf->names_truncated);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);
	for (i = 0; i < shown_count; i++)
		add_group_item(builder, g_ptr_array_index(list, i));
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	extra = builder_text(builder);

	ok = insert_fact(rf, "dup_title", "", total_groups, total_reclaimable, extra, error);

out:
	g_hash_table_unref(groups);
	g_hash_table_unref(by_id);
	g_ptr_array_unref(list);
	return ok;
}

/* ── Names: split across volumes ────────────────────────────────── */

typedef struct
{
	gint64  root_id;
	gint64  volume_id;
	gchar  *folder;	/* stored form, relative to the root */
	gint64  paths;
	gint64  bytes;
	gboolean usable;
	gint64  dir_id;	/* 0 when the folder has no row */
} Part;

static void
part_free(gpointer data)
{
	Part *part = data;

	g_free(part->folder);
	g_free(part);
}

typedef struct
{
	gchar     *name;
	GPtrArray *parts;	/* Part * */
	gint64     paths;
	gint64     bytes;
	gint64     moved;	/* bytes not on the volume that holds the most */
	guint      volumes;
} Split;

static void
split_free(gpointer data)
{
	Split *split = data;

	g_free(split->name);
	g_ptr_array_unref(split->parts);
	g_free(split);
}

static gint
compare_parts(
	gconstpointer a,
	gconstpointer b
){
	const Part *x = *(Part *const *)a;
	const Part *y = *(Part *const *)b;

	if (x->bytes != y->bytes)
		return x->bytes > y->bytes ? -1 : 1;
	return x->volume_id < y->volume_id ? -1 : x->volume_id > y->volume_id;
}

static gint
compare_splits(
	gconstpointer a,
	gconstpointer b
){
	const Split *x = *(Split *const *)a;
	const Split *y = *(Split *const *)b;

	if (x->moved != y->moved)
		return x->moved > y->moved ? -1 : 1;
	return strcmp(x->name, y->name);
}

/*
 * Series: the folders directly below TV roots.  Albums: the folders two
 * levels below music roots (artist and album).  Both groups of folders are
 * keyed by their normalised names; a key whose folders lie on more than one
 * volume is split.  Every file under a folder belongs to it (the file list
 * of its "under" filter).
 */
static gboolean
split_report(
	Refresh      *rf,
	gboolean      albums,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) skipped = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *extra = NULL;
	GHashTable *keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, split_free);
	GPtrArray *list = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gint64 total_bytes = 0;
	gint64 not_compared = 0;
	guint shown;
	guint i;
	gboolean ok = FALSE;

	if (albums) {
		if (!run_raw(rf, "albums across volumes",
		        "SELECT g.root_id, g.volume_id, g.seg1, g.seg2, g.n, g.bytes, g.usable, d.id FROM ("
		        " SELECT w.root_id, w.volume_id, split_part(f.rel_path, '/', 1) AS seg1,"
		        "  split_part(f.rel_path, '/', 2) AS seg2, count(*) AS n,"
		        "  coalesce(sum(w.size) FILTER (WHERE w.counted), 0) AS bytes, bool_and(w.usable) AS usable"
		        " FROM analysis_work w JOIN library_file f ON f.id = w.id"
		        " WHERE w.media_type = 'music' AND length(f.rel_path) - length(replace(f.rel_path, '/', '')) >= 2"
		        " GROUP BY 1, 2, 3, 4) g"
		        " LEFT JOIN library_dir d ON d.root_id = g.root_id"
		        "  AND d.path_hash = library_path_hash(g.seg1 || '/' || g.seg2)",
		        0, NULL, &result, error))
			goto out;
		if (!run_raw(rf, "albums not compared",
		        "SELECT count(*) FROM analysis_work w JOIN library_file f ON f.id = w.id"
		        " WHERE w.media_type = 'music' AND length(f.rel_path) - length(replace(f.rel_path, '/', '')) < 2",
		        0, NULL, &skipped, error))
			goto out;
	} else {
		if (!run_raw(rf, "series across volumes",
		        "SELECT g.root_id, g.volume_id, g.seg1, '', g.n, g.bytes, g.usable, d.id FROM ("
		        " SELECT w.root_id, w.volume_id, split_part(f.rel_path, '/', 1) AS seg1, count(*) AS n,"
		        "  coalesce(sum(w.size) FILTER (WHERE w.counted), 0) AS bytes, bool_and(w.usable) AS usable"
		        " FROM analysis_work w JOIN library_file f ON f.id = w.id"
		        " WHERE w.media_type = 'tv' AND position('/' IN f.rel_path) > 0 GROUP BY 1, 2, 3) g"
		        " LEFT JOIN library_dir d ON d.root_id = g.root_id AND d.path_hash = library_path_hash(g.seg1)",
		        0, NULL, &result, error))
			goto out;
		if (!run_raw(rf, "series not compared",
		        "SELECT count(*) FROM analysis_work w JOIN library_file f ON f.id = w.id"
		        " WHERE w.media_type = 'tv' AND position('/' IN f.rel_path) = 0",
		        0, NULL, &skipped, error))
			goto out;
	}
	not_compared = int_at(skipped, 0, 0, 0);

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *seg1 = axis_db_result_get_text(result, i, 2);
		const gchar *seg2 = axis_db_result_get_text(result, i, 3);
		gchar *name_key = albums ? axis_analysis_album_key(seg1, seg2) : axis_analysis_show_key(seg1);
		Split *split;
		Part *part;

		if (name_key == NULL) {
			not_compared += int_at(result, i, 4, 0);
			continue;
		}
		split = g_hash_table_lookup(keys, name_key);
		if (split == NULL) {
			split = g_new0(Split, 1);
			split->name = albums ? g_strdup_printf("%s / %s", seg1, seg2) : g_strdup(seg1);
			split->parts = g_ptr_array_new_with_free_func(part_free);
			g_hash_table_insert(keys, name_key, split);
		} else {
			g_free(name_key);
		}
		part = g_new0(Part, 1);
		part->root_id = int_at(result, i, 0, 0);
		part->volume_id = int_at(result, i, 1, 0);
		part->folder = albums ? g_strdup_printf("%s/%s", seg1, seg2) : g_strdup(seg1);
		part->paths = int_at(result, i, 4, 0);
		part->bytes = int_at(result, i, 5, 0);
		part->usable = bool_at(result, i, 6);
		part->dir_id = int_at(result, i, 7, 0);
		g_ptr_array_add(split->parts, part);
	}

	g_hash_table_iter_init(&iter, keys);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Split *split = value;
		g_autoptr(GHashTable) volumes = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
		GHashTableIter by_volume;
		gpointer vkey;
		gpointer vvalue;
		gint64 largest = 0;

		g_ptr_array_sort(split->parts, compare_parts);
		for (i = 0; i < split->parts->len; i++) {
			const Part *part = g_ptr_array_index(split->parts, i);
			gint64 *sum = g_hash_table_lookup(volumes, &part->volume_id);

			if (sum == NULL) {
				gint64 *volume = g_new(gint64, 1);

				*volume = part->volume_id;
				sum = g_new0(gint64, 1);
				g_hash_table_insert(volumes, volume, sum);
			}
			*sum += part->bytes;
			split->paths += part->paths;
			split->bytes += part->bytes;
		}
		split->volumes = g_hash_table_size(volumes);
		g_hash_table_iter_init(&by_volume, volumes);
		while (g_hash_table_iter_next(&by_volume, &vkey, &vvalue))
			largest = MAX(largest, *(gint64 *)vvalue);
		split->moved = split->bytes - largest;
		if (split->volumes >= 2)
			g_ptr_array_add(list, split);
	}
	g_ptr_array_sort(list, compare_splits);
	for (i = 0; i < list->len; i++)
		total_bytes += ((Split *)g_ptr_array_index(list, i))->bytes;

	shown = MIN(list->len, (guint)AXIS_ANALYSIS_GROUPS);
	json_builder_begin_object(builder);
	add_int(builder, "not_compared_paths", not_compared);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);
	for (i = 0; i < shown; i++) {
		const Split *split = g_ptr_array_index(list, i);
		guint k;

		json_builder_begin_object(builder);
		add_string(builder, "name", split->name);
		add_int(builder, "volumes", split->volumes);
		add_int(builder, "paths", split->paths);
		add_int(builder, "bytes", split->bytes);
		add_int(builder, "moved", split->moved);
		json_builder_set_member_name(builder, "parts");
		json_builder_begin_array(builder);
		for (k = 0; k < split->parts->len; k++) {
			const Part *part = g_ptr_array_index(split->parts, k);

			json_builder_begin_object(builder);
			add_int(builder, "root_id", part->root_id);
			add_int(builder, "volume_id", part->volume_id);
			add_string(builder, "folder", part->folder);
			add_int(builder, "paths", part->paths);
			add_int(builder, "bytes", part->bytes);
			add_int(builder, "dir_id", part->dir_id);
			add_bool(builder, "usable", part->usable);
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	extra = builder_text(builder);

	ok = insert_fact(rf, "split", albums ? "album" : "series", list->len, total_bytes, extra, error);

out:
	g_hash_table_unref(keys);
	g_ptr_array_unref(list);
	return ok;
}

/* ── Retention ──────────────────────────────────────────────────── */

/*
 * The last snapshot of each UTC day is kept for analysis.keep_daily, every
 * other for analysis.keep_other, and the newest always.  Done after the
 * snapshot is committed, in a transaction of its own: a failure here loses
 * nothing.
 */
static void
apply_retention(
	AxisAnalysis *self,
	gint64        newest
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *daily = g_strdup_printf("%" G_GINT64_FORMAT, axis_analysis_get_duration(self,
		AXIS_ANALYSIS_SETTING_KEEP_DAILY, AXIS_ANALYSIS_DEFAULT_KEEP_DAILY));
	g_autofree gchar *other = g_strdup_printf("%" G_GINT64_FORMAT, axis_analysis_get_duration(self,
		AXIS_ANALYSIS_SETTING_KEEP_OTHER, AXIS_ANALYSIS_DEFAULT_KEEP_OTHER));
	g_autofree gchar *keep = g_strdup_printf("%" G_GINT64_FORMAT, newest);
	const gchar *params[3];

	params[0] = keep;
	params[1] = daily;
	params[2] = other;
	if (!axis_db_pool_exec(self->pool,
	        "DELETE FROM analysis_snapshot s WHERE s.id <> $1::bigint AND ("
	        " s.taken_at < now() - make_interval(secs => $2::double precision)"
	        " OR (s.taken_at < now() - make_interval(secs => $3::double precision)"
	        "   AND EXISTS (SELECT 1 FROM analysis_snapshot t WHERE t.id <> s.id"
	        "     AND t.taken_at > s.taken_at"
	        "     AND date_trunc('day', t.taken_at AT TIME ZONE 'UTC') = date_trunc('day', s.taken_at AT TIME ZONE 'UTC'))))",
	        3, params, &result, &error))
		g_warning("analysis: cannot apply the retention of snapshots: %s", error->message);
}

/* ── The refresh ────────────────────────────────────────────────── */

static gchar *
config_text(const Refresh *rf)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint i;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "large_bpp_milli");
	json_builder_begin_object(builder);
	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++)
		add_int(builder, axis_analysis_large_codecs[i], rf->milli[i]);
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	return builder_text(builder);
}

static gboolean
refresh_in_transaction(
	Refresh      *rf,
	const gchar  *trigger,
	gint64        job_id,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *config = config_text(rf);
	g_autofree gchar *job_text = job_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, job_id) : NULL;
	const gchar *params[6];
	const gchar *large[4];
	const AxisLeftoverRule *rules = axis_leftover_rules();
	gboolean locked = FALSE;
	guint i;

	if (!axis_db_conn_begin(rf->conn, error))
		return FALSE;
	if (!axis_db_conn_exec(rf->conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", 0, NULL, NULL, error) ||
	    !axis_db_conn_exec(rf->conn, "SET LOCAL statement_timeout = '" AXIS_ANALYSIS_STATEMENT_TIMEOUT "'", 0,
	        NULL, NULL, error) ||
	    !axis_db_conn_exec(rf->conn, "SET LOCAL idle_in_transaction_session_timeout = '30min'", 0, NULL, NULL,
	        error) ||
	    !axis_db_conn_exec(rf->conn, "SET LOCAL work_mem = '128MB'", 0, NULL, NULL, error))
		return FALSE;

	/* One refresh at a time. */
	if (!axis_db_conn_exec(rf->conn, "SELECT pg_try_advisory_xact_lock(hashtext('axis.analysis.refresh'), 0)",
	        0, NULL, &result, error))
		return FALSE;
	locked = bool_at(result, 0, 0);
	g_clear_pointer(&result, axis_db_result_free);
	if (!locked) {
		g_set_error_literal(error, AXIS_ANALYSIS_ERROR, AXIS_ANALYSIS_ERROR_BUSY,
			"another refresh is running");
		return FALSE;
	}

	params[0] = trigger;
	params[1] = job_text;
	params[2] = config;
	if (!axis_db_conn_exec(rf->conn,
	        "INSERT INTO analysis_snapshot (taken_at, trigger, job_id, took_ms, paths, bytes, coverage, config)"
	        " VALUES (clock_timestamp(), $1::text, $2::bigint, 0, 0, 0, '{}'::jsonb, $3::jsonb) RETURNING id",
	        3, params, &result, error))
		return FALSE;
	rf->snapshot = int_at(result, 0, 0, 0);
	rf->snapshot_text = g_strdup_printf("%" G_GINT64_FORMAT, rf->snapshot);
	g_clear_pointer(&result, axis_db_result_free);

	if (!run_plain(rf, "reading the index", SQL_WORK, error) ||
	    !run_plain(rf, "indexing", "CREATE UNIQUE INDEX analysis_work_id ON analysis_work (id)", error) ||
	    !run_plain(rf, "analysing", "ANALYZE analysis_work", error))
		return FALSE;

	/* Space. */
	if (!run_snap(rf, "totals", SQL_OVERALL, error) ||
	    !run_snap(rf, "space by volume", SQL_SPACE_VOLUME, error) ||
	    !run_snap(rf, "space by root", SQL_SPACE_ROOT, error))
		return FALSE;
	for (i = 0; breakdowns[i].report != NULL; i++) {
		g_autofree gchar *sql = breakdown_sql(breakdowns[i].report, breakdowns[i].column);

		if (!run_snap(rf, breakdowns[i].label, sql, error))
			return FALSE;
	}

	/* Bit rates. */
	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++)
		large[i] = rf->milli_text[i];
	if (!run_snap(rf, "bit rates", SQL_BPP, error) ||
	    !run(rf, "large for its class", SQL_LARGE, 4, large, NULL, error) ||
	    !run(rf, "the largest for their class", SQL_LARGE_LIST, 4, large, NULL, error))
		return FALSE;

	/* Largest. */
	if (!run_snap(rf, "largest files", SQL_LARGEST_FILES, error) ||
	    !run_snap(rf, "largest folders", SQL_LARGEST_DIRS, error))
		return FALSE;

	/* Duplicates and splits. */
	if (!run_snap(rf, "duplicates by content", SQL_DUP_CONTENT, error) ||
	    !dup_title(rf, error) ||
	    !split_report(rf, FALSE, error) ||
	    !split_report(rf, TRUE, error))
		return FALSE;

	/* Leftovers. */
	for (i = 0; rules[i].id != NULL; i++) {
		g_autofree gchar *sql = leftover_sql(rules[i].id, rules[i].condition, TRUE);

		if (!run_snap(rf, rules[i].label, sql, error))
			return FALSE;
	}
	{
		g_autofree gchar *sql = leftover_sql("any", axis_leftover_condition("any"), FALSE);

		if (!run_snap(rf, "all leftovers", sql, error))
			return FALSE;
	}

	/* What could not be seen, and the headline figures. */
	if (!run_snap(rf, "coverage", SQL_COVERAGE, error))
		return FALSE;
	{
		g_autofree gchar *took = g_strdup_printf("%" G_GINT64_FORMAT,
			(g_get_monotonic_time() - rf->started_us) / 1000);
		const gchar *finish[1];

		finish[0] = took;
		if (!run(rf, "finishing",
		        "UPDATE analysis_snapshot SET took_ms = LEAST($2::bigint, 2000000000),"
		        " finished_at = clock_timestamp() WHERE id = $1::bigint", 1, finish, NULL, error))
			return FALSE;
	}
	return axis_db_conn_commit(rf->conn, error);
}

gboolean
axis_analysis_refresh_run(
	AxisAnalysis    *self,
	const gchar     *trigger,
	gint64           job_id,
	AxisJobContext  *context,
	gint64          *snapshot_id,
	GError         **error
){
	Refresh rf;
	gboolean ok;
	guint i;

	if (snapshot_id != NULL)
		*snapshot_id = 0;
	if (g_strcmp0(trigger, "schedule") != 0 && g_strcmp0(trigger, "request") != 0 &&
	    g_strcmp0(trigger, "direct") != 0) {
		g_set_error_literal(error, AXIS_ANALYSIS_ERROR, AXIS_ANALYSIS_ERROR_FAILED,
			"the trigger is not one of schedule, request and direct");
		return FALSE;
	}

	memset(&rf, 0, sizeof(rf));
	rf.self = self;
	rf.context = context;
	rf.started_us = g_get_monotonic_time();
	axis_analysis_get_thresholds(self, rf.milli);
	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++)
		rf.milli_text[i] = g_strdup_printf("%" G_GINT64_FORMAT, rf.milli[i]);

	rf.conn = axis_db_pool_acquire(self->pool, error);
	if (rf.conn == NULL) {
		for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++)
			g_free(rf.milli_text[i]);
		return FALSE;
	}
	ok = refresh_in_transaction(&rf, trigger, job_id, error);
	if (!ok)
		axis_db_conn_rollback(rf.conn, NULL);
	axis_db_pool_release(self->pool, rf.conn);

	if (ok) {
		if (snapshot_id != NULL)
			*snapshot_id = rf.snapshot;
		apply_retention(self, rf.snapshot);
	}
	g_free(rf.snapshot_text);
	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++)
		g_free(rf.milli_text[i]);
	return ok;
}
