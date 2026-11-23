/*
 * axis-conversion-run.h - conversion runs: a suggested list of a library's
 * files, the largest estimated saving first and each series kept together,
 * worked through by asking for ordinary single conversions, one at a time
 * by default
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What a run keeps true (design.md of add-conversion-runs):
 *
 *  - A run adds no way of replacing a file and weakens no gate: every file
 *    it works on is asked for through axis_conversions_request_for_run(),
 *    which is axis_conversions_request() with the run recorded, the same
 *    gates in the same function, and becomes an ordinary conversion with
 *    its ten checks, its placement and its page.  A replacement is asked
 *    for only once a person confirmed on the run's page that originals will
 *    be deleted, with a revision that is proven.
 *  - The list is built by the estimator's own loader and decision
 *    (axis_estimator_load_files()'s columns, axis_estimate_eligibility()):
 *    there is no second estimator.  A file not in the list has exactly one
 *    reason, and can be looked at.
 *  - The list is built by a job, in one transaction, a batch at a time;
 *    every page of it reads a bounded number of rows in a bounded number of
 *    statements.
 *  - A series is a unit: its episodes are asked for one after another, in
 *    season and episode order, before the next unit; units are ranked by
 *    their total estimated saving.  A movie (or any file that is no
 *    series') is a unit of its own.
 *  - The run asks for nothing while conversion work is held after a
 *    restart, storage is read-only, no suitable worker is connected, or it
 *    is outside its hours; it pauses itself, and says why, when files keep
 *    failing, an encoder crashed, the player check stops saying safe, its
 *    revision cannot replace any more, or (with trials) enough trials passed
 *    for a person to prove the revision.  An offline volume, or one below
 *    its free-space floor, freezes its files: the run goes on with others.
 *
 * Every function blocks (database): call it from a worker thread.
 */

#ifndef AXIS_CONVERSION_RUN_H
#define AXIS_CONVERSION_RUN_H

#include <glib.h>

#include "jobs/axis-job-context.h"
#include "transcode/axis-conversion.h"

G_BEGIN_DECLS

/* The jobs: the list of a run (on the maintenance queue, like the
 * estimate's refresh), and the step that settles what finished and asks
 * for the next files, every minute and whenever a run is started or
 * resumed (de-duplicated). */
#define AXIS_CONVERSION_RUN_LIST_KIND        "convert.run.list"
#define AXIS_CONVERSION_RUN_STEP_KIND        "convert.run.step"
#define AXIS_CONVERSION_RUN_STEP_SCHEDULE    "convert.run.step"
#define AXIS_CONVERSION_RUN_STEP_INTERVAL_S  60

/* The most files a run asks for at once (a run's setting, default 1, and
 * never more than the suitable workers can take). */
#define AXIS_CONVERSION_RUN_MAX_AT_ONCE      8

/* Refusals in a row (files the gates refused when the run asked for them)
 * after which a run pauses itself: they cost no encode, but a condition
 * every file shares should not mark the whole list skipped. */
#define AXIS_CONVERSION_RUN_REFUSALS         20

/* Pages: units and entries of a page, files not in the list of a page. */
#define AXIS_CONVERSION_RUN_PAGE             50
#define AXIS_CONVERSION_RUN_ENTRY_PAGE       100

typedef enum
{
	AXIS_CONVERSION_RUN_MOVIES,
	AXIS_CONVERSION_RUN_TV,
	AXIS_CONVERSION_RUN_ROOTS,
	AXIS_CONVERSION_RUN_SERIES
} AxisConversionRunScope;

/* "movies", "tv", "roots", "series" */
const gchar *
axis_conversion_run_scope_to_string(AxisConversionRunScope scope);

gboolean
axis_conversion_run_scope_from_string(
	const gchar            *text,
	AxisConversionRunScope *scope
);

/* A run's state for people ("Being listed", "Ready to start", "Running",
 * ...). */
const gchar *
axis_conversion_run_state_label(const gchar *state);

/* Why a file is not in a run's list ("Already efficient", "Remembered as not
 * worth it", ...), from a stored reason: the estimator's reasons and the
 * run's own ("not_worth_it", "converted", "in_flight", "kept"). */
const gchar *
axis_conversion_run_reason_label(const gchar *reason);

const gchar *
axis_conversion_run_reason_help(const gchar *reason);

/* A time of day, minutes after midnight, as "22:00". */
gchar *
axis_conversion_run_clock_text(gint minute);

typedef struct _AxisConversionRuns AxisConversionRuns;

/**
 * axis_conversion_runs_new:
 * @conversions: the conversions (referenced): the request, the settings,
 *   the storage, the estimator
 */
AxisConversionRuns *
axis_conversion_runs_new(AxisConversions *conversions);

AxisConversionRuns *
axis_conversion_runs_ref(AxisConversionRuns *self);

void
axis_conversion_runs_unref(AxisConversionRuns *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionRuns, axis_conversion_runs_unref)

AxisConversions *
axis_conversion_runs_get_conversions(AxisConversionRuns *self);

/* Tests: the time the step and the pages take as now (real time, µs); 0
 * for the clock. */
void
axis_conversion_runs_set_now(
	AxisConversionRuns *self,
	gint64              now_us
);

/* ── Making a run ───────────────────────────────────────────────── */

/**
 * axis_conversion_runs_create:
 * @root_ids: (array length=n_roots): for %AXIS_CONVERSION_RUN_ROOTS
 * @title_id: for %AXIS_CONVERSION_RUN_SERIES, the series
 * @revision_id: a video profile's revision that is usable
 * @user: the administrator
 * @run_id: (out): the new run, being listed
 *
 * Stores the run and queues the job that lists it, always (a later run
 * beside a started one skips what that one holds).  Fails with
 * %AXIS_CONVERSION_ERROR_REFUSED (the message says why) for a scope or
 * revision it cannot be made of.
 */
gboolean
axis_conversion_runs_create(
	AxisConversionRuns     *self,
	AxisConversionRunScope  scope,
	const gint64           *root_ids,
	guint                   n_roots,
	gint64                  title_id,
	gint64                  revision_id,
	const gchar            *user,
	gint64                 *run_id,
	GError                **error
);

/* As axis_conversion_runs_create(), for a person's click: given @created, a
 * run of the same selection (the same roots in any order) with the same
 * revision that has not started yet (being listed, or ready) is that run:
 * *@run_id is it, nothing is stored and *@created is %FALSE (a second
 * click makes no second run).  Without @created, as
 * axis_conversion_runs_create(). */
gboolean
axis_conversion_runs_create_full(
	AxisConversionRuns     *self,
	AxisConversionRunScope  scope,
	const gint64           *root_ids,
	guint                   n_roots,
	gint64                  title_id,
	gint64                  revision_id,
	const gchar            *user,
	gint64                 *run_id,
	gboolean               *created,
	GError                **error
);

/**
 * axis_conversion_runs_build_list:
 * @context: (nullable): for cancellation and progress
 *
 * What the list job does: every video file of the scope decided by the
 * estimator for the run's revision, the eligible ones (and those eligible
 * on a volume that is away now) listed as entries in units, ranked, the
 * others kept with their reason; in one transaction; the run becomes ready.
 * A run not being listed any more is left as it is.
 */
gboolean
axis_conversion_runs_build_list(
	AxisConversionRuns  *self,
	gint64               run_id,
	AxisJobContext      *context,
	GError             **error
);

gboolean
axis_conversion_runs_run_list_job(
	AxisConversionRuns  *self,
	AxisJobContext      *context,
	GError             **error
);

/* ── What a person does ─────────────────────────────────────────── */

/**
 * AxisConversionRunLimits:
 * @at_once: files asked for at once, 1 to AXIS_CONVERSION_RUN_MAX_AT_ONCE
 * @cap_files: only the first units holding this many files (0: no cap)
 * @cap_bytes: only the first units estimated to save this much (0: none)
 * @cap_seconds: stop asking this long after the start (0: no budget)
 * @window_start, @window_end: the hours it may ask in, minutes after
 *   midnight local time (both -1: any time)
 *
 * A cap keeps a series whole: the list ends after the unit in which it is
 * reached.
 */
typedef struct
{
	gint   at_once;
	gint64 cap_files;
	gint64 cap_bytes;
	gint64 cap_seconds;
	gint   window_start;
	gint   window_end;
} AxisConversionRunLimits;

gboolean
axis_conversion_runs_set_limits(
	AxisConversionRuns            *self,
	gint64                         run_id,
	const AxisConversionRunLimits *limits,
	const gchar                   *user,
	GError                       **error
);

/**
 * axis_conversion_runs_start:
 * @outcome: trials, or replacing originals (only with a proven revision
 *   that is its profile's current one)
 * @confirmed: for a replacement, the person ticked that originals will be
 *   deleted
 *
 * A ready run starts; no other run may be running.  Fails with
 * %AXIS_CONVERSION_ERROR_REFUSED or %AXIS_CONVERSION_ERROR_STATE.
 */
gboolean
axis_conversion_runs_start(
	AxisConversionRuns     *self,
	gint64                  run_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *user,
	GError                **error
);

/* Running -> paused by @user: nothing more is asked for; what is in flight
 * finishes. */
gboolean
axis_conversion_runs_pause(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *user,
	GError             **error
);

/* Paused -> running (no other run may be running); the counts of failures
 * and refusals in a row start again. */
gboolean
axis_conversion_runs_resume(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *user,
	GError             **error
);

/* Ready, running or paused -> cancelled: what waits for a worker or is
 * being converted is cancelled (its output removed as any cancelled
 * conversion's); what is being checked or put in place finishes. */
gboolean
axis_conversion_runs_cancel(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *user,
	GError             **error
);

/**
 * axis_conversion_runs_replace:
 * @confirmed: the person ticked that originals will be deleted
 *
 * From trials to replacing originals, without building the list again: the
 * revision must be proven now and its profile's current one.  The files
 * converted as trials are asked for again as replacements (a trial's file
 * is never put in a library).  A run paused because its trials could prove
 * the revision runs again.
 */
gboolean
axis_conversion_runs_replace(
	AxisConversionRuns  *self,
	gint64               run_id,
	gboolean             confirmed,
	const gchar         *user,
	GError             **error
);

/* Takes a unit (every file of it not asked for yet) out of the list. */
gboolean
axis_conversion_runs_remove_unit(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 position,
	const gchar         *user,
	GError             **error
);

/* Takes one file not asked for yet out of the list. */
gboolean
axis_conversion_runs_remove_entry(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint64               entry_id,
	const gchar         *user,
	GError             **error
);

/* Queues the step now (de-duplicated). */
gboolean
axis_conversion_runs_request_step(
	AxisConversionRuns  *self,
	GError             **error
);

/* ── The step ───────────────────────────────────────────────────── */

/**
 * AxisConversionRunStepReport:
 * @settled: entries whose conversion ended, recorded
 * @asked: conversions asked for
 * @skipped: entries skipped (refused when asked, gone)
 */
typedef struct
{
	guint settled;
	guint asked;
	guint skipped;
} AxisConversionRunStepReport;

/**
 * axis_conversion_runs_step:
 * @report: (nullable): what it did
 *
 * What the step job does: records what ended of every run's files in
 * flight, then for the running run works out whether it must wait or pause,
 * and asks for the next files of the list (as many as it may at once).
 */
gboolean
axis_conversion_runs_step(
	AxisConversionRuns           *self,
	AxisConversionRunStepReport  *report,
	GError                      **error
);

gboolean
axis_conversion_runs_run_step_job(
	AxisConversionRuns  *self,
	AxisJobContext      *context,
	GError             **error
);

/* ── Reading ────────────────────────────────────────────────────── */

/**
 * AxisConversionRunWait:
 *
 * A volume whose files wait.
 */
typedef struct
{
	gint64  volume_id;
	gchar  *volume_name;
	gchar  *code;	/* "offline", "full" */
	gchar  *text;
	gint64  since_us;
} AxisConversionRunWait;

void
axis_conversion_run_wait_free(AxisConversionRunWait *wait);

/**
 * AxisConversionRunInfo:
 *
 * A run as its page shows it: what was asked, its state and why, the list's
 * totals (units and files not removed; within the caps), and what became of
 * its files.
 */
typedef struct
{
	gint64     id;
	gint64     revision_id;
	gchar     *profile_name;
	gchar     *profile_slug;
	gint       revision_number;
	gboolean   proven;
	gboolean   current;
	gchar     *scope;
	gchar     *scope_text;
	gchar     *state;
	gchar     *outcome;
	gint       at_once;
	gint64     cap_files;
	gint64     cap_bytes;
	gint64     cap_seconds;
	gint       cap_unit;	/* -1: none */
	gint       window_start;	/* -1: none */
	gint       window_end;
	gchar     *created_by;
	gint64     created_us;
	gchar     *started_by;
	gint64     started_us;
	gchar     *confirmed_by;
	gint64     confirmed_us;
	gint64     finished_us;
	gchar     *finish_text;
	gchar     *list_text;
	gint64     listed_us;
	gchar     *list_job_state;
	gdouble    list_progress;
	gchar     *list_message;
	gint64     scope_files;
	gchar     *pause_code;
	gchar     *pause_text;
	gchar     *paused_by;
	gint64     paused_us;
	gchar     *wait_code;
	gchar     *wait_text;
	gint64     wait_us;
	gint       failed_in_row;
	gint       refused_in_row;
	/* The list: every unit and file not removed, and those within the caps. */
	gint64     units;
	gint64     files;
	gint64     bytes;
	gint64     mid;
	gint64     cap_units_in;
	gint64     cap_files_in;
	gint64     cap_bytes_in;
	gint64     cap_mid_in;
	/* Its files by what became of them. */
	gint64     waiting;
	gint64     asked;
	gint64     done;
	gint64     skipped;
	gint64     failed;
	gint64     removed;
	gint64     units_done;
	/* The files done: their size before, the estimate after, the real size
	 * after; the trials among them. */
	gint64     done_before;
	gint64     done_estimate;
	gint64     done_after;
	gint64     done_trials;
	/* The revision's trials: passed (one per file) and how many proving by
	 * hand needs; trials rejected by a check of the converted file. */
	gint64     passed_trials;
	gint64     needed_trials;
	gint64     bad_trials;
	/* The revision's finished conversions with an estimate: files, the
	 * estimate after and the real size after (conversion_estimate_seen). */
	gint64     seen_files;
	gint64     seen_estimated;
	gint64     seen_actual;
	GPtrArray *waits;	/* AxisConversionRunWait */
} AxisConversionRunInfo;

void
axis_conversion_run_info_free(AxisConversionRunInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionRunInfo, axis_conversion_run_info_free)

/* The newest @limit runs, without their totals (only the run's row and its
 * files' counts by state, in one statement). */
gboolean
axis_conversion_runs_list(
	AxisConversionRuns  *self,
	guint                limit,
	GPtrArray          **runs,
	GError             **error
);

/* One run with everything its page shows; %AXIS_CONVERSION_ERROR_NOT_FOUND. */
gboolean
axis_conversion_runs_get(
	AxisConversionRuns     *self,
	gint64                  run_id,
	AxisConversionRunInfo **info,
	GError                **error
);

/*
 * The runs that wait for a person, the newest first (six at most), for the
 * page "Needs attention": each row is the run's id, its state ("paused" or
 * "ready"), its pause code ("" when it has none; "person" when a person
 * paused it), the second it was paused (0 when it was not) and the second
 * its list was finished (when it became ready to start).  One statement
 * over the few rows of conversion_run, every wait bounded by @timeout_ms.
 * Any thread.
 */
gboolean
axis_conversion_runs_waiting(
	AxisConversionRuns  *self,
	guint                timeout_ms,
	AxisDbResult       **result,
	GError             **error
);

/* The run that is running, else the newest paused one, for the dashboard;
 * %NULL (and TRUE) when there is none. */
gboolean
axis_conversion_runs_active(
	AxisConversionRuns     *self,
	AxisConversionRunInfo **info,
	GError                **error
);

/**
 * AxisConversionRunUnit:
 *
 * A unit of the list with what became of its files.  @title is the movie's
 * or series' title when the unit is one (with @year), else %NULL and
 * @rel_path (stored form) names it.
 */
typedef struct
{
	gint     position;
	gchar   *kind;
	gint64   title_id;
	gchar   *title;
	gint     year;
	gchar   *root_name;
	gchar   *rel_path;
	gint     files;
	gint64   bytes;
	gint64   mid;
	gboolean removed;
	gboolean beyond_cap;
	gint     done;
	gint     skipped;
	gint     failed;
	gint     asked;
	gint     waiting;
	gint64   done_after;
	gint64   done_before;
} AxisConversionRunUnit;

void
axis_conversion_run_unit_free(AxisConversionRunUnit *unit);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionRunUnit, axis_conversion_run_unit_free)

/* A page of units after @after (a position, 0 for the first page), at most
 * @limit (AxisConversionRunUnit); *@more when there are others after. */
gboolean
axis_conversion_runs_units(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 after,
	guint                limit,
	GPtrArray          **units,
	gboolean            *more,
	GError             **error
);

/* One unit; %AXIS_CONVERSION_ERROR_NOT_FOUND. */
gboolean
axis_conversion_runs_unit(
	AxisConversionRuns     *self,
	gint64                  run_id,
	gint                    position,
	AxisConversionRunUnit **unit,
	GError                **error
);

/**
 * AxisConversionRunEntry:
 *
 * A file of the list.
 */
typedef struct
{
	gint64  id;
	gint    position;
	gint    unit_position;
	gint64  file_id;
	gint64  volume_id;
	gchar  *volume_name;
	gchar  *rel_path;
	gint    season_number;	/* -1: none */
	gint    episode_number;
	gint64  size;
	gint64  low;
	gint64  mid;
	gint64  high;
	gchar  *state;
	gint64  conversion_id;
	gchar  *conversion_state;
	gint64  trial_id;
	gchar  *outcome;
	gint64  result_size;	/* -1: none */
	gchar  *reason_code;
	gchar  *reason_text;
	gint64  settled_us;
} AxisConversionRunEntry;

void
axis_conversion_run_entry_free(AxisConversionRunEntry *entry);

/* The files of a unit after the entry position @after (0 for the first),
 * at most @limit (AxisConversionRunEntry); *@more when there are others. */
gboolean
axis_conversion_runs_entries(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 unit_position,
	gint                 after,
	guint                limit,
	GPtrArray          **entries,
	gboolean            *more,
	GError             **error
);

/* The files in flight, and the @limit settled last (newest first). */
gboolean
axis_conversion_runs_in_flight(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray          **entries,
	GError             **error
);

gboolean
axis_conversion_runs_recent(
	AxisConversionRuns  *self,
	gint64               run_id,
	guint                limit,
	GPtrArray          **entries,
	GError             **error
);

/**
 * AxisConversionRunReason:
 *
 * How many files of the scope are not in the list for one reason.
 */
typedef struct
{
	gchar  *reason;
	gint64  files;
	gint64  bytes;
} AxisConversionRunReason;

void
axis_conversion_run_reason_free(AxisConversionRunReason *reason);

/* By how many files, most first. */
gboolean
axis_conversion_runs_reasons(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray          **reasons,
	GError             **error
);

/**
 * AxisConversionRunExcluded:
 *
 * A file not in the list.
 */
typedef struct
{
	gint64  file_id;
	gint64  size;
	gchar  *root_name;
	gchar  *rel_path;
	gchar  *state;	/* the file's index state now, nullable when it is gone */
} AxisConversionRunExcluded;

void
axis_conversion_run_excluded_free(AxisConversionRunExcluded *file);

/* The files not in the list for @reason, largest first, after (@after_size,
 * @after_file) (both 0 for the first page), at most @limit
 * (AxisConversionRunExcluded); *@more when there are others. */
gboolean
axis_conversion_runs_excluded(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *reason,
	gint64               after_size,
	gint64               after_file,
	guint                limit,
	GPtrArray          **files,
	gboolean            *more,
	GError             **error
);

/**
 * AxisConversionRunSeries:
 *
 * A series identification linked to a folder of a TV root, for the choice
 * of a run's scope.
 */
typedef struct
{
	gint64  title_id;
	gchar  *title;
	gint    year;	/* 0: not known */
} AxisConversionRunSeries;

void
axis_conversion_run_series_free(AxisConversionRunSeries *series);

/* The series on disk, by title, at most @limit (AxisConversionRunSeries). */
gboolean
axis_conversion_runs_series_on_disk(
	AxisConversionRuns  *self,
	guint                limit,
	GPtrArray          **series,
	GError             **error
);

G_END_DECLS

#endif /* AXIS_CONVERSION_RUN_H */
