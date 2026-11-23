/*
 * axis-analysis.h - reports about the library, computed from the index
 * into snapshots
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An AxisAnalysis answers what none of the other pages do: where the space
 * went, what the library is made of, what is duplicated, what is scattered
 * across disks.  Decisions about converting and deleting files will be made
 * from these figures, so a figure that is wrong, or silently computed from
 * partial data, is a harm.  The promises (data/migrations/0009_library_analysis.sql
 * has the storage side):
 *
 *  - Every figure is traceable to the files behind it: it equals the length
 *    of the file list it links to (axis-library-filter.h), because both are
 *    derived from the same conditions.
 *  - Every report says what it could not see: the snapshot records files not
 *    probed, probes that failed or timed out, files whose probe is out of
 *    date, probes whose HDR class is uncertain, roots never completely
 *    scanned, volumes offline, and files the last complete scan did not
 *    find.
 *  - Only present files are counted.  Suspected and missing files are
 *    reported on their own coverage lines.  Files on an offline volume
 *    stay in every total, from the index as it last was, and are marked, so
 *    totals do not move when a disk disconnects.
 *  - Hard links: two paths on one volume with the same inode (and more than
 *    one link) are one file.  Its size is counted once in every byte total,
 *    attributed to the path with the lowest file id outside a downloads
 *    root (else the lowest), wherever the other path is (another root of
 *    the same volume included); the other path
 *    still counts in "paths", which is the length of the file list.  They
 *    are never listed as duplicates of each other.  Paths on different
 *    volumes are different files.
 *  - Duplicates are "likely".  They show their evidence, change nothing,
 *    and propose nothing.  Two tiers: the same size, duration and stream
 *    layout; and the same title by the normaliser of axis-analysis-name.h.
 *  - Nothing here opens a media file or holds a volume guard: it reads the
 *    index (one REPEATABLE READ transaction, so all figures describe one
 *    moment; readers do not block the scanner or the probe) and writes its
 *    own tables.  Only one refresh runs at a time.  Every statement runs
 *    under a deadline, and the refresh as a whole under another.
 *
 * Every function that touches the database blocks: call it from a worker
 * thread.
 */

#ifndef AXIS_ANALYSIS_H
#define AXIS_ANALYSIS_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"

G_BEGIN_DECLS

#define AXIS_ANALYSIS_MODULE_ID           "analysis"

/* The refresh job and the check that decides when one is due.  Both run on
 * the "maintenance" queue (an existing one, so the pool needs no extra
 * connection): a refresh takes seconds on a large library and minutes on a
 * huge one, during which the other maintenance jobs (the scan and probe
 * schedulers, job retention) wait their turn. */
#define AXIS_ANALYSIS_REFRESH_KIND        "analysis.refresh"
#define AXIS_ANALYSIS_REFRESH_ATTEMPTS    1
#define AXIS_ANALYSIS_REQUEST_PRIORITY    10
#define AXIS_ANALYSIS_DUE_KIND            "analysis.due"
#define AXIS_ANALYSIS_DUE_SCHEDULE        "analysis.due"
#define AXIS_ANALYSIS_DUE_INTERVAL_SECONDS 300

/* Without a snapshot for this long, one is made even if nothing changed
 * (free space moves, and the growth chart needs a point per day). */
#define AXIS_ANALYSIS_DAILY_SECONDS       (24 * 3600)

#define AXIS_ANALYSIS_SETTING_LARGE_H264  "analysis.large_bpp_h264"
#define AXIS_ANALYSIS_SETTING_LARGE_HEVC  "analysis.large_bpp_hevc"
#define AXIS_ANALYSIS_SETTING_LARGE_AV1   "analysis.large_bpp_av1"
#define AXIS_ANALYSIS_SETTING_LARGE_VP9   "analysis.large_bpp_vp9"
#define AXIS_ANALYSIS_SETTING_MIN_INTERVAL "analysis.min_interval"
#define AXIS_ANALYSIS_SETTING_SETTLE      "analysis.settle"
#define AXIS_ANALYSIS_SETTING_KEEP_DAILY  "analysis.keep_daily"
#define AXIS_ANALYSIS_SETTING_KEEP_OTHER  "analysis.keep_other"

/* The codecs that have a "large for its class" threshold, in the order of
 * the settings above. */
#define AXIS_ANALYSIS_N_LARGE_CODECS      4

extern const gchar *const axis_analysis_large_codecs[AXIS_ANALYSIS_N_LARGE_CODECS];

/* Bounded lists, per snapshot. */
#define AXIS_ANALYSIS_LIST_ITEMS          100
#define AXIS_ANALYSIS_LEFTOVER_ITEMS      50
#define AXIS_ANALYSIS_GROUPS              200
#define AXIS_ANALYSIS_GROUP_MEMBERS       24

/* The bits-per-pixel histogram: buckets of this many thousandths, the last
 * one open at the top. */
#define AXIS_ANALYSIS_BPP_BUCKET          50
#define AXIS_ANALYSIS_BPP_BUCKETS         11

/* Deadlines: each statement, and the whole refresh. */
#define AXIS_ANALYSIS_STATEMENT_TIMEOUT   "180s"
#define AXIS_ANALYSIS_DEADLINE_SECONDS    1800

/* Rows of names the same-title tier reads at most (it says so when a
 * library has more). */
#define AXIS_ANALYSIS_NAME_MAX_ROWS       2000000

#define AXIS_ANALYSIS_ERROR (axis_analysis_error_quark())

/**
 * AxisAnalysisError:
 * @AXIS_ANALYSIS_ERROR_BUSY: another refresh is running
 * @AXIS_ANALYSIS_ERROR_DEADLINE: the refresh took longer than its deadline
 * @AXIS_ANALYSIS_ERROR_FAILED: something else
 */
typedef enum
{
	AXIS_ANALYSIS_ERROR_BUSY,
	AXIS_ANALYSIS_ERROR_DEADLINE,
	AXIS_ANALYSIS_ERROR_FAILED
} AxisAnalysisError;

GQuark
axis_analysis_error_quark(void);

typedef struct _AxisAnalysis AxisAnalysis;

/**
 * AxisAnalysisTuning:
 *
 * For tests: @after_stage is called on the refreshing thread after each
 * stage, inside the refresh's transaction, with the stage's name.
 */
typedef struct
{
	void     (*after_stage)(const gchar *stage, gpointer data);
	gpointer   data;
} AxisAnalysisTuning;

/**
 * axis_analysis_new:
 * @pool: the database; must outlive the result
 * @settings: (nullable): where the thresholds and times are read; %NULL
 *   (tests) is the defaults
 */
AxisAnalysis *
axis_analysis_new(
	AxisDbPool   *pool,
	AxisSettings *settings
);

AxisAnalysis *
axis_analysis_ref(AxisAnalysis *self);

void
axis_analysis_unref(AxisAnalysis *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAnalysis, axis_analysis_unref)

/* The module's settings, to declare in its AxisModuleInfo or add to a
 * test's store. */
const AxisSettingInfo *
axis_analysis_get_setting_table(void);

/* Tests only, before any refresh. */
void
axis_analysis_set_tuning(
	AxisAnalysis             *self,
	const AxisAnalysisTuning *tuning
);

/* ── Refreshing ─────────────────────────────────────────────────── */

/**
 * axis_analysis_refresh:
 * @self: the analysis
 * @trigger: "schedule", "request" or "direct"
 * @job_id: the job running the refresh, or 0
 * @context: (nullable): the job, for cancellation and progress
 * @snapshot_id: (out) (optional): the snapshot made
 * @error: (nullable): return location for an error
 *
 * Computes every report into a new snapshot, in one transaction, and
 * applies the retention afterwards.  Nothing is stored when it fails.
 * Fails with %AXIS_ANALYSIS_ERROR_BUSY while another refresh runs,
 * %AXIS_ANALYSIS_ERROR_DEADLINE, %AXIS_JOB_ERROR_CANCELLED (when @context
 * says to stop) or a database error.
 *
 * Returns: %TRUE if a snapshot was stored
 */
gboolean
axis_analysis_refresh(
	AxisAnalysis    *self,
	const gchar     *trigger,
	gint64           job_id,
	AxisJobContext  *context,
	gint64          *snapshot_id,
	GError         **error
);

/**
 * axis_analysis_request_refresh:
 * @job_id: (out) (optional): the job (a new one, or the one queued or
 *   running)
 *
 * What an administrator's "refresh now" does: enqueues the refresh job.
 */
gboolean
axis_analysis_request_refresh(
	AxisAnalysis  *self,
	gint64        *job_id,
	GError       **error
);

/**
 * axis_analysis_enqueue_due:
 * @enqueued: (out) (optional): whether a refresh was enqueued
 *
 * What the "analysis.due" job does: enqueues a refresh when scans and probes
 * have settled (no scan running, no probe queued or running, the last of
 * them ended at least analysis.settle ago) and something changed since the
 * last snapshot, at most once per analysis.min_interval; or when the last
 * snapshot is a day old, so that the growth chart has a point for every
 * day.  Nothing is enqueued while no root exists, nor before the first scan
 * or probe has ended.
 */
gboolean
axis_analysis_enqueue_due(
	AxisAnalysis  *self,
	gboolean      *enqueued,
	GError       **error
);

/* The handlers of AXIS_ANALYSIS_REFRESH_KIND and AXIS_ANALYSIS_DUE_KIND. */
gboolean
axis_analysis_run_refresh_job(
	AxisAnalysis    *self,
	AxisJobContext  *context,
	GError         **error
);

gboolean
axis_analysis_run_due_job(
	AxisAnalysis    *self,
	AxisJobContext  *context,
	GError         **error
);

/* ── Reading ────────────────────────────────────────────────────── */

typedef struct
{
	gchar      *dim1;
	gchar      *dim2;
	gint64      paths;
	gint64      bytes;
	JsonObject *extra;	/* never NULL */
} AxisAnalysisFact;

/**
 * AxisAnalysisSnapshot:
 * @id: the snapshot
 * @taken_us: the moment the figures describe, microseconds since 1970
 * @finished_us: when the refresh ended
 * @trigger: "schedule", "request" or "direct"
 * @took_ms: how long the refresh took
 * @paths: present paths in all
 * @bytes: their size, hard links once
 * @volumes_total: capacity of the volumes as last measured, -1 if unknown
 * @volumes_used: used space of the volumes, -1 if unknown
 * @coverage: what the snapshot could not see
 * @config: the thresholds it used (thousandths of a bit per pixel)
 * @facts: report name -> GPtrArray of #AxisAnalysisFact, in dimension order
 */
typedef struct
{
	gint64      id;
	gint64      taken_us;
	gint64      finished_us;
	gchar      *trigger;
	gint64      took_ms;
	gint64      paths;
	gint64      bytes;
	gint64      volumes_total;
	gint64      volumes_used;
	JsonObject *coverage;
	JsonObject *config;
	GHashTable *facts;
} AxisAnalysisSnapshot;

void
axis_analysis_snapshot_free(AxisAnalysisSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAnalysisSnapshot, axis_analysis_snapshot_free)

/* The facts of one report (borrowed; %NULL when there are none). */
const GPtrArray *
axis_analysis_snapshot_facts(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report
);

/* One fact (borrowed), or %NULL. */
const AxisAnalysisFact *
axis_analysis_snapshot_fact(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report,
	const gchar                *dim1,
	const gchar                *dim2
);

/**
 * axis_analysis_load_snapshot:
 * @snapshot_id: the snapshot, or 0 for the latest
 * @reports: (nullable): the reports to load (the facts of the others are
 *   not read), ended by %NULL; %NULL for all
 * @snapshot: (out) (transfer full): the snapshot, or %NULL when there is
 *   none yet (still %TRUE)
 */
gboolean
axis_analysis_load_snapshot(
	AxisAnalysis          *self,
	gint64                 snapshot_id,
	const gchar *const    *reports,
	AxisAnalysisSnapshot **snapshot,
	GError               **error
);

/** One point of the growth chart. */
typedef struct
{
	gint64 id;
	gint64 taken_us;
	gint64 paths;
	gint64 bytes;
	gint64 volumes_total;
	gint64 volumes_used;
} AxisAnalysisPoint;

/**
 * axis_analysis_load_history:
 * @limit: at most this many of the newest snapshots
 * @points: (out) (transfer full) (element-type AxisAnalysisPoint): oldest
 *   first
 */
gboolean
axis_analysis_load_history(
	AxisAnalysis  *self,
	guint          limit,
	GPtrArray    **points,
	GError       **error
);

/**
 * AxisAnalysisStatus:
 * @refreshing: a refresh is queued or running
 * @running: it is running
 * @job_id: its job
 * @progress: 0 to 1, -1 when unknown
 * @message: (nullable): where it is
 */
typedef struct
{
	gboolean  refreshing;
	gboolean  running;
	gint64    job_id;
	gdouble   progress;
	gchar    *message;
} AxisAnalysisStatus;

void
axis_analysis_status_clear(AxisAnalysisStatus *status);

gboolean
axis_analysis_get_status(
	AxisAnalysis        *self,
	AxisAnalysisStatus  *status,
	GError             **error
);

/* A fact's extra member, 0 / "" when missing or of another type. */
gint64
axis_analysis_extra_int(
	JsonObject  *extra,
	const gchar *member
);

const gchar *
axis_analysis_extra_string(
	JsonObject  *extra,
	const gchar *member
);

G_END_DECLS

#endif /* AXIS_ANALYSIS_H */
