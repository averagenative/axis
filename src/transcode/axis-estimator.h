/*
 * axis-estimator.h - the estimator with its database: the files' facts,
 * calibration, the Savings snapshots, the estimator's file lists, and the
 * optional sample measurement
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The decision and the size model are pure (axis-estimate.h); this object
 * reads what they need from the index and the probe's records, and keeps:
 *
 *  - Targets come from conversion profile revisions (axis-profile-store.h),
 *    each made by axis_profile_revision_target(): a revision whose stored
 *    definition does not validate gives none, so it is estimated for
 *    nothing.  Every figure names the revision it was computed for.
 *  - Per-file answers are computed when asked for (the file page, the file
 *    lists, a conversion) and never stored.  The one loader
 *    (axis_estimator_load_files()) reads the facts for every caller, so the
 *    report, the list and the file page decide from the same facts.
 *  - The Savings reports are snapshots, made by the estimate.refresh job
 *    after each Analysis refresh and on request, in one REPEATABLE READ
 *    transaction (every figure describes one moment), with the coverage of
 *    the Analysis reports.  A total's middle is the sum of its files'
 *    middles (whole bytes); its range combines them as axis-estimate.h
 *    says.  Hard links are pinned, so a file that would be converted is
 *    one path; the bytes of skipped files count hard links once, as the
 *    Analysis does.  Files on a volume that is away are counted from the
 *    index as it last was, and marked, so totals do not move while a disk
 *    is unplugged.
 *  - Calibration: estimate_sample rows (axis_estimator_record_sample()),
 *    read into the model when they change.
 *  - The sample measurement (axis-estimate-sample.c) reads media through a
 *    read guard, writes only to the encoder's pipe, and runs one at a time
 *    at the lowest priority on a queue of its own.
 *
 * Every function that touches the database or a file blocks: call it from
 * a worker thread.
 */

#ifndef AXIS_ESTIMATOR_H
#define AXIS_ESTIMATOR_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "api/axis-worker-proto.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "library/axis-library-browse.h"
#include "library/axis-library-filter.h"
#include "storage/axis-storage.h"
#include "transcode/axis-estimate.h"
#include "transcode/axis-profile-store.h"

G_BEGIN_DECLS

#define AXIS_ESTIMATE_MODULE_ID              "estimate"

/* The refresh and its check run on the "maintenance" queue, after the
 * Analysis refresh (the check waits for one to end): the queue exists, so
 * the pool needs no connection more. */
#define AXIS_ESTIMATE_REFRESH_KIND           "estimate.refresh"
#define AXIS_ESTIMATE_REFRESH_ATTEMPTS       1
#define AXIS_ESTIMATE_REQUEST_PRIORITY       10
#define AXIS_ESTIMATE_DUE_KIND               "estimate.due"
#define AXIS_ESTIMATE_DUE_SCHEDULE           "estimate.due"
#define AXIS_ESTIMATE_DUE_INTERVAL_SECONDS   60

/* The measurement runs for minutes or hours on a queue of its own (one
 * worker: one measurement at a time), so that it never holds up the
 * maintenance jobs; it needs one pool connection while it records. */
#define AXIS_ESTIMATE_SAMPLE_KIND            "estimate.sample"
#define AXIS_ESTIMATE_SAMPLE_QUEUE           "sample"
#define AXIS_ESTIMATE_SAMPLE_PRIORITY        (-100)

#define AXIS_ESTIMATE_SETTING_ENCODER        "estimate.encoder"
#define AXIS_ESTIMATE_SETTING_FFMPEG         "estimate.ffmpeg_path"
#define AXIS_ESTIMATE_SETTING_SAMPLE_FILES   "estimate.sample_files"
#define AXIS_ESTIMATE_SETTING_SAMPLE_SECONDS "estimate.sample_seconds"
#define AXIS_ESTIMATE_SETTING_SAMPLE_TIMEOUT "estimate.sample_timeout"

/* Bounded lists, per target and snapshot. */
#define AXIS_ESTIMATE_TOP_ITEMS              100

/* The estimator's file lists: rows read per batch, and how long a page and
 * its count may take (they evaluate every row they read). */
#define AXIS_ESTIMATE_LIST_BATCH             500
#define AXIS_ESTIMATE_LIST_DEADLINE_MS       5000
#define AXIS_ESTIMATE_COUNT_DEADLINE_MS      4000

/* Deadlines of the refresh: each statement, and the whole. */
#define AXIS_ESTIMATE_STATEMENT_TIMEOUT      "180s"
#define AXIS_ESTIMATE_DEADLINE_SECONDS       1800

#define AXIS_ESTIMATE_ERROR (axis_estimate_error_quark())

/**
 * AxisEstimateError:
 * @AXIS_ESTIMATE_ERROR_BUSY: a refresh (or a measurement) is running
 * @AXIS_ESTIMATE_ERROR_DEADLINE: it took longer than its deadline
 * @AXIS_ESTIMATE_ERROR_NOT_FOUND: no such file or target
 * @AXIS_ESTIMATE_ERROR_TOOL: ffmpeg is not there or cannot be started
 * @AXIS_ESTIMATE_ERROR_SCRATCH: the scratch directory is not safe to use
 * @AXIS_ESTIMATE_ERROR_FAILED: something else
 * @AXIS_ESTIMATE_ERROR_INVALID: a worker's result does not fit the job's
 *   plan (nothing of it is kept)
 */
typedef enum
{
	AXIS_ESTIMATE_ERROR_BUSY,
	AXIS_ESTIMATE_ERROR_DEADLINE,
	AXIS_ESTIMATE_ERROR_NOT_FOUND,
	AXIS_ESTIMATE_ERROR_TOOL,
	AXIS_ESTIMATE_ERROR_SCRATCH,
	AXIS_ESTIMATE_ERROR_FAILED,
	AXIS_ESTIMATE_ERROR_INVALID
} AxisEstimateError;

GQuark
axis_estimate_error_quark(void);

typedef struct _AxisEstimator AxisEstimator;

/**
 * axis_estimator_new:
 * @pool: the database; must outlive the result
 * @storage: the volumes (for the measurement's read guards); referenced
 * @settings: (nullable): where the settings are read (the module's and the
 *   Analysis "large for its class" thresholds); %NULL (tests) is the
 *   defaults
 */
AxisEstimator *
axis_estimator_new(
	AxisDbPool   *pool,
	AxisStorage  *storage,
	AxisSettings *settings
);

AxisEstimator *
axis_estimator_ref(AxisEstimator *self);

void
axis_estimator_unref(AxisEstimator *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisEstimator, axis_estimator_unref)

const AxisSettingInfo *
axis_estimator_get_setting_table(void);

/* Whether the "estimate" module is on: the file page and the file lists of
 * other modules only use the estimator while it is.  Any thread. */
void
axis_estimator_set_enabled(
	AxisEstimator *self,
	gboolean       enabled
);

gboolean
axis_estimator_is_enabled(AxisEstimator *self);

/* The built-in targets as the code table defines them (the database's
 * built-in revisions are written from the same table at start-up), with
 * the setting applied, in page order (AXIS_ESTIMATE_N_TARGETS of them). */
void
axis_estimator_get_targets(
	AxisEstimator      *self,
	AxisEstimateTarget  targets[AXIS_ESTIMATE_N_TARGETS]
);

/* One of them; FALSE for an id that is none. */
gboolean
axis_estimator_get_target(
	AxisEstimator      *self,
	const gchar        *id,
	AxisEstimateTarget *target
);

/**
 * axis_estimator_profile_target:
 * @slug: a profile
 * @number: its revision, 0 for the current one
 * @target: (out caller-allocates): the revision's target with the setting
 *   applied
 * @revision: (out) (optional) (transfer full): the revision
 *
 * Fails with %AXIS_ESTIMATE_ERROR_NOT_FOUND for no such revision, and with
 * %AXIS_ESTIMATE_ERROR_FAILED for one whose definition is not usable.
 */
gboolean
axis_estimator_profile_target(
	AxisEstimator        *self,
	const gchar          *slug,
	gint                  number,
	AxisEstimateTarget   *target,
	AxisProfileRevision **revision,
	GError              **error
);

/* The pool, for the pages that work with profiles beside the estimates. */
AxisDbPool *
axis_estimator_get_pool(AxisEstimator *self);

/* @built (axis_profile_to_target()) with the setting applied, as every
 * target the estimator uses is. */
void
axis_estimator_configure(
	AxisEstimator            *self,
	const AxisEstimateTarget *built,
	AxisEstimateTarget       *target
);

/* The configured targets of the current revisions of every profile that is
 * not archived (built-ins first), or with @scheduled only of those
 * refreshed on schedule; a revision that is not usable gives none
 * (AxisEstimateTarget). */
gboolean
axis_estimator_profile_targets(
	AxisEstimator  *self,
	gboolean        scheduled,
	GArray        **targets,
	GError        **error
);

/**
 * AxisEstimateCounts:
 * @scope: files read that the target is for
 * @converts: of them, would be converted
 * @now: their size now; @mid: estimated after (middles)
 * @skips: the others, by reason
 */
typedef struct
{
	gint64 scope;
	gint64 converts;
	gint64 now;
	gint64 mid;
	gint64 skips[AXIS_ESTIMATE_N_REASONS];
} AxisEstimateCounts;

/**
 * AxisEstimatePreview:
 * @before, @after: the counts of each target over the same files
 * @read: files read; @total: present video and audio files
 * @complete: every file was read before the deadline
 */
typedef struct
{
	AxisEstimateCounts before;
	AxisEstimateCounts after;
	gint64             read;
	gint64             total;
	gboolean           complete;
} AxisEstimatePreview;

/**
 * axis_estimator_preview:
 * @deadline_ms: how long it may read
 *
 * What an edit would do to the eligibility counts: both targets decided
 * for the files of the index (an album whole), as a refresh would, until
 * the deadline.  Stores nothing.
 */
gboolean
axis_estimator_preview(
	AxisEstimator            *self,
	const AxisEstimateTarget *before,
	const AxisEstimateTarget *after,
	guint                     deadline_ms,
	AxisEstimatePreview      *preview,
	GError                  **error
);

/**
 * axis_estimator_get_model:
 *
 * The model with every calibration sample, read again when a sample was
 * added since; shared, so a caller holds a reference
 * (axis_estimator_model_unref()).
 */
AxisEstimateModel *
axis_estimator_get_model(
	AxisEstimator  *self,
	GError        **error
);

void
axis_estimator_model_unref(AxisEstimateModel *model);

/* ── Loading facts ──────────────────────────────────────────────── */

/**
 * AxisEstimateRows:
 * @files: (element-type AxisEstimateFile): in the order asked for (rows
 *   that are gone are left out); each owns its facts until the rows are
 *   freed
 */
typedef struct
{
	GPtrArray *files;
	/* Private. */
	GPtrArray *owned;
	GStringChunk *strings;
} AxisEstimateRows;

void
axis_estimate_rows_free(AxisEstimateRows *rows);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisEstimateRows, axis_estimate_rows_free)

/**
 * axis_estimator_load_files:
 * @conn: a connection the caller holds
 * @ids: (array length=n_ids): library_file ids
 * @rows: (out) (transfer full): their facts
 *
 * The one loader: index facts (with the album unit and how many present
 * files it has, and whether the path counts a hard-linked file's bytes)
 * and the probe's record with its streams.
 */
gboolean
axis_estimator_load_files(
	AxisDbConn         *conn,
	const gint64       *ids,
	guint               n_ids,
	AxisEstimateRows  **rows,
	GError            **error
);

/**
 * axis_estimator_load_units:
 * @ids: (array length=n_ids): files whose unit has more than one file
 * @rows: (out) (transfer full): every present file of each of their units,
 *   by id (one statement for all of them)
 */
gboolean
axis_estimator_load_units(
	AxisDbConn         *conn,
	const gint64       *ids,
	guint               n_ids,
	AxisEstimateRows  **rows,
	GError            **error
);

/* ── One file ───────────────────────────────────────────────────── */

/**
 * AxisFileEstimate:
 * @target: the target (configured)
 * @result: the decision and the estimate
 * @state: the calibration of the cell the estimate came from
 * @samples: real results that cell has
 */
typedef struct
{
	AxisEstimateTarget   target;
	AxisEstimateResult   result;
	AxisCalibrationState state;
	guint                samples;
} AxisFileEstimate;

/**
 * axis_estimator_estimate_file:
 * @estimates: (out) (transfer full) (element-type AxisFileEstimate): one per
 *   target of the file's kind (video targets for a video file, audio
 *   targets for an audio file), in page order; empty for any other file
 *
 * What the file page shows.  An audio file is decided with the rest of its
 * album.  Fails with %AXIS_ESTIMATE_ERROR_NOT_FOUND for no such file.
 */
gboolean
axis_estimator_estimate_file(
	AxisEstimator  *self,
	gint64          file_id,
	GArray        **estimates,
	GError        **error
);

/* ── Calibration ────────────────────────────────────────────────── */

/**
 * axis_estimator_record_sample:
 * @conn: (nullable): a connection the caller holds, or %NULL for one from
 *   the pool
 * @origin: "measured" or "converted"
 * @job_id: the job that made it, or 0
 * @file_id: its file, or 0
 *
 * Stores a real result for the calibration.  Refuses (with
 * %AXIS_ESTIMATE_ERROR_FAILED) a sample the model would not accept.
 */
gboolean
axis_estimator_record_sample(
	AxisEstimator             *self,
	AxisDbConn                *conn,
	const AxisEstimateSample  *sample,
	const gchar               *origin,
	gint64                     job_id,
	gint64                     file_id,
	GError                   **error
);

/* ── Snapshots ──────────────────────────────────────────────────── */

/**
 * axis_estimator_refresh:
 * @trigger: "schedule", "request" or "direct"
 * @job_id: the job, or 0
 * @context: (nullable): for cancellation and progress
 * @snapshot_id: (out) (optional): the snapshot made
 *
 * Computes every target's figures into a new snapshot, in one transaction,
 * then applies the retention.  Nothing is stored when it fails.  Fails
 * with %AXIS_ESTIMATE_ERROR_BUSY while another refresh runs,
 * %AXIS_ESTIMATE_ERROR_DEADLINE, %AXIS_JOB_ERROR_CANCELLED or a database
 * error.
 */
gboolean
axis_estimator_refresh(
	AxisEstimator   *self,
	const gchar     *trigger,
	gint64           job_id,
	AxisJobContext  *context,
	gint64          *snapshot_id,
	GError         **error
);

/* What an administrator's "refresh now" does: the built-in and assigned
 * profiles (and any asked for). */
gboolean
axis_estimator_request_refresh(
	AxisEstimator  *self,
	gint64         *job_id,
	GError        **error
);

/* Asks for figures for the profile @slug's current revision too: the next
 * refresh (queued now) includes it.  Profiles that are neither built in
 * nor assigned are refreshed only so. */
gboolean
axis_estimator_request_profile_refresh(
	AxisEstimator  *self,
	const gchar    *slug,
	const gchar    *requested_by,
	gint64         *job_id,
	GError        **error
);

/**
 * axis_estimator_enqueue_due:
 *
 * What estimate.due does: enqueues a refresh when an Analysis snapshot,
 * a calibration sample, the setting or the profiles refreshed on schedule
 * (the built-in ones and those assigned to a root, at their current
 * revisions) are newer than the last Savings snapshot, or figures were
 * asked for a profile, no Analysis refresh is queued or running, and none
 * of its own is.
 */
gboolean
axis_estimator_enqueue_due(
	AxisEstimator  *self,
	gboolean       *enqueued,
	GError        **error
);

gboolean
axis_estimator_run_refresh_job(
	AxisEstimator   *self,
	AxisJobContext  *context,
	GError         **error
);

gboolean
axis_estimator_run_due_job(
	AxisEstimator   *self,
	AxisJobContext  *context,
	GError         **error
);

/**
 * AxisEstimateFact:
 * @target: the profile's slug (a snapshot has one revision of a profile)
 * @revision_id: the revision it was computed for
 * @revision: that revision's number
 */
typedef struct
{
	gchar      *target;
	gint64      revision_id;
	gint        revision;
	gchar      *report;
	gchar      *dim;
	gint64      paths;
	gint64      bytes;
	gint64      low;
	gint64      mid;
	gint64      high;
	JsonObject *extra;	/* never NULL */
} AxisEstimateFact;

/**
 * AxisEstimateSnapshot:
 * @coverage: what the snapshot could not see, in the Analysis' keys
 * @config: the targets as configured (by slug, each with its revision),
 *   the model version, the calibration
 * @facts: (element-type AxisEstimateFact): ordered by target, report, dim
 */
typedef struct
{
	gint64      id;
	gint64      taken_us;
	gint64      finished_us;
	gchar      *trigger;
	gint64      took_ms;
	JsonObject *coverage;
	JsonObject *config;
	GPtrArray  *facts;
} AxisEstimateSnapshot;

void
axis_estimate_snapshot_free(AxisEstimateSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisEstimateSnapshot, axis_estimate_snapshot_free)

/* The latest snapshot; with @target (a profile's slug), the latest that
 * has figures for that profile, with only its facts.  %NULL (and TRUE)
 * when there is none yet. */
gboolean
axis_estimator_load_snapshot(
	AxisEstimator          *self,
	const gchar            *target,
	AxisEstimateSnapshot  **snapshot,
	GError                **error
);

/* One fact (borrowed), or %NULL. */
const AxisEstimateFact *
axis_estimate_snapshot_fact(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target,
	const gchar                *report,
	const gchar                *dim
);

/* The facts of one target and report, in dim order (the array borrows). */
GPtrArray *
axis_estimate_snapshot_facts(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target,
	const gchar                *report
);

typedef struct
{
	gboolean  refreshing;
	gboolean  running;
	gint64    job_id;
	gdouble   progress;
	gchar    *message;
} AxisEstimateStatus;

void
axis_estimate_status_clear(AxisEstimateStatus *status);

gboolean
axis_estimator_get_status(
	AxisEstimator       *self,
	AxisEstimateStatus  *status,
	GError             **error
);

/* ── The estimator's file lists ─────────────────────────────────── */

/**
 * axis_estimator_list_files:
 * @filter: a parsed filter with a target (and maybe an outcome)
 * @list: (out) (transfer full): the page
 *
 * As axis_library_list_files(), for a filter whose target and outcome only
 * the estimator can evaluate: it reads the list's rows in order (narrowed
 * by SQL to the rows that could match), decides each with the estimator
 * (an audio file with its album), and keeps the ones that match, until a
 * page is full; the count does the same up to AXIS_LIBRARY_COUNT_CAP.
 * Both stop at their deadline: the page then fails with
 * %AXIS_DB_ERROR_TIMEOUT, the count says it is unknown.
 */
gboolean
axis_estimator_list_files(
	AxisEstimator            *self,
	const AxisLibraryFilter  *filter,
	AxisFileList            **list,
	GError                  **error
);

/* Whether a result is what a filter's outcome asks for (%NULL: any but
 * "not applicable"). */
gboolean
axis_estimate_outcome_matches(
	const gchar              *outcome,
	const AxisEstimateResult *result
);

/* ── The sample measurement (axis-estimate-sample.c) ────────────── */

/**
 * AxisSampleTuning:
 *
 * For tests: @tool_command is the program and the arguments before the
 * measurement's own (a fake tool); @scratch_parent where the scratch
 * directories go (else the daemon's temporary directory); @excerpt_hook is
 * called on the measuring thread after each excerpt, with the scratch
 * directory.
 */
typedef struct
{
	const gchar *const *tool_command;
	const gchar        *scratch_parent;
	guint               timeout_ms;
	void              (*excerpt_hook)(const gchar *scratch, gpointer data);
	gpointer            hook_data;
} AxisSampleTuning;

void
axis_estimator_set_sample_tuning(
	AxisEstimator          *self,
	const AxisSampleTuning *tuning
);

/* The directory the scratch directories are made in (created 0700 if
 * missing); refuses one that is a link, not ours, open to others, or on or
 * under a volume. */
gchar *
axis_estimator_scratch_parent(
	AxisEstimator  *self,
	GError        **error
);

/* Removes what an earlier run left in the scratch parent (a daemon that
 * was killed): only the measurement's own directories, never following a
 * link or leaving its filesystem.  The module's start hook calls it. */
void
axis_estimator_clean_scratch(AxisEstimator *self);

/**
 * axis_estimator_request_sample:
 * @target: a target id
 * @requested_by: the administrator's name, for the job's payload
 * @job_id: (out) (optional): the job (the one queued or running when there
 *   is one already)
 * @created: (out) (optional): whether it is a new one
 */
gboolean
axis_estimator_request_sample(
	AxisEstimator  *self,
	const gchar    *target,
	const gchar    *requested_by,
	gint64         *job_id,
	gboolean       *created,
	GError        **error
);

/* Cancels the queued or running measurement, if any. */
gboolean
axis_estimator_cancel_sample(
	AxisEstimator  *self,
	gboolean       *cancelled,
	GError        **error
);

/**
 * AxisSampleStatus:
 * @job_id: the latest measurement job, 0 for none
 * @state: its state ("queued", "running", "succeeded", ...)
 * @target: its target
 * @progress: 0 to 1, -1 unknown
 * @message: (nullable): where it is
 * @error: (nullable): why it failed
 * @started_us, @finished_us: 0 when not
 * @samples: calibration samples recorded by it
 */
typedef struct
{
	gint64  job_id;
	gchar  *state;
	gchar  *target;
	gdouble progress;
	gchar  *message;
	gchar  *error;
	gint64  started_us;
	gint64  finished_us;
	gint64  samples;
} AxisSampleStatus;

void
axis_sample_status_clear(AxisSampleStatus *status);

gboolean
axis_estimator_get_sample_status(
	AxisEstimator     *self,
	AxisSampleStatus  *status,
	GError           **error
);

/* Samples per target and origin, for the pages: target -> count. */
gboolean
axis_estimator_count_samples(
	AxisEstimator  *self,
	GHashTable    **counts,
	GError        **error
);

gboolean
axis_estimator_run_sample_job(
	AxisEstimator   *self,
	AxisJobContext  *context,
	GError         **error
);

/* ── The sample measurement on a worker ─────────────────────────── */

/**
 * axis_estimator_request_remote_sample:
 * @target: a profile's slug
 * @requested_by: (nullable): the administrator's name
 * @job_id: (out) (optional): the job (the measurement queued or running,
 *   the daemon's own or a worker's, when there is one already)
 * @created: (out) (optional): whether it is a new one
 *
 * Plans a measurement for a worker (AXIS_WORKER_KIND_SAMPLE): the files the
 * daemon's own measurement would choose, each with its stream, quality,
 * depth and excerpts, named by volume UUID and paths relative to the
 * volume; and what a worker must have shown to be offered it (the
 * encoder, 10 bits, the volumes).  The plan is kept in the job's payload.
 * Fails with %AXIS_ESTIMATE_ERROR_NOT_FOUND when no file would be
 * measured, and as axis_estimator_request_sample().
 */
gboolean
axis_estimator_request_remote_sample(
	AxisEstimator  *self,
	const gchar    *target,
	const gchar    *requested_by,
	gint64         *job_id,
	gboolean       *created,
	GError        **error
);

/* The description a claim's answer carries, from the job's payload. */
gboolean
axis_estimator_describe_remote_sample(
	const gchar            *payload,
	AxisSampleDescription **description,
	GError                **error
);

/**
 * axis_estimator_record_remote_sample:
 * @conn: a connection in the transaction that holds the job locked
 *   (axis_job_lock_remote())
 * @job_id: the job
 * @payload: its payload
 * @result: what the worker reported
 * @worker: (nullable): its name, for the log
 * @recorded: (out): samples recorded
 * @refused: (out): measured files not recorded
 *
 * Records a worker's measurement, trusting only its byte counts: each
 * measured file must still be, by the index, the very file the plan was
 * made of (present, not changed, same volume, size, time and inode) and be
 * planned the same way again (stream, quality, depth, excerpt length); the
 * seconds are the plan's; and the bitrate must be plausible (within a
 * factor of 20 of the model's prediction for a picture; for audio at most
 * twice the source's or four times the target's bitrate).  A file that
 * fails is not recorded and is counted in @refused.  Fails with
 * %AXIS_ESTIMATE_ERROR_INVALID, recording nothing, when the result does not
 * fit the plan (a file it does not have, another number of excerpts), with
 * %AXIS_ESTIMATE_ERROR_FAILED for a payload or profile that cannot be used,
 * and with the database's error.
 */
gboolean
axis_estimator_record_remote_sample(
	AxisEstimator           *self,
	AxisDbConn              *conn,
	gint64                   job_id,
	const gchar             *payload,
	const AxisSampleResult  *result,
	const gchar             *worker,
	guint                   *recorded,
	guint                   *refused,
	GError                 **error
);

G_END_DECLS

#endif /* AXIS_ESTIMATOR_H */
