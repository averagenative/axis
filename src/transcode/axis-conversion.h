/*
 * axis-conversion.h - converting one file: the request and its gates, the
 * work a worker is given, the daemon's verification, the placement that
 * replaces the original, recovery, the record, and proving a revision by
 * hand
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is the code that can delete media files.  What it keeps true (the
 * whole of it is design.md of add-conversion-execution):
 *
 *  - Nothing is written while "storage.read_only" is on, the volume is
 *    offline or mounted read-only: every change goes through the volume
 *    write layer (storage/axis-volume-write.h) with a write guard.
 *  - The worker's word is never taken: the daemon checks the output itself
 *    (ten checks, axis-conversion-verify.c) before anything in the library
 *    changes, and a failed check removes the output and leaves the original.
 *    Where convert.checks_where sends them to a worker, the measurements of
 *    checks 5 to 8 are a worker's (convert.verify), bound to the files on
 *    the daemon's own disk and judged by the daemon (design.md of
 *    verify-on-worker); a worker never says a check passed.
 *  - Conversion work (encodes, the checks' measurements, sample
 *    measurements) is held after the daemon starts while
 *    convert.wait_after_start is on, until a person resumes it.
 *  - At every instant the library name holds the complete original or the
 *    complete verified output (an exchange of two names, or a new name
 *    published before the old one is retired); the original goes into the
 *    volume's recycle folder and is removed from it only after the
 *    replacement is recorded, after convert.keep_original.
 *  - Every step is recorded first or after, so that a daemon that stops at
 *    any point finishes or undoes the placement from its records; when the
 *    disk and the records disagree, nothing is removed.
 *  - Only one job ever acts on a conversion's files at a time: the finish
 *    job, de-duplicated per conversion (a second can neither be queued nor
 *    run while one is), on the "convert" queue; recovery is that job too.
 *    One conversion per file is in flight (a unique index).
 *
 * Every function blocks (database, filesystem, tools): call it from a
 * worker thread.
 */

#ifndef AXIS_CONVERSION_H
#define AXIS_CONVERSION_H

#include <glib.h>

#include "api/axis-worker-proto.h"
#include "compat/axis-compat-verdict.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-registry.h"
#include "probe/axis-probe.h"
#include "storage/axis-storage.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile-store.h"

G_BEGIN_DECLS

#define AXIS_CONVERSION_MODULE_ID       "conversions"

/* The daemon's jobs: the checks and the placement (and recovery) of one
 * conversion, de-duplicated on "convert.finish:<id>"; and the tidying of
 * the work areas, every 15 minutes. */
#define AXIS_CONVERSION_QUEUE           "convert"
#define AXIS_CONVERSION_FINISH_KIND     "convert.finish"
#define AXIS_CONVERSION_FINISH_KEY      "convert.finish:"
#define AXIS_CONVERSION_FINISH_ATTEMPTS 5
#define AXIS_CONVERSION_TIDY_KIND       "convert.tidy"
#define AXIS_CONVERSION_TIDY_SCHEDULE   "convert.tidy"
#define AXIS_CONVERSION_TIDY_INTERVAL_S 900
#define AXIS_CONVERSION_ENCODE_ATTEMPTS 3

/* A converted file's checks measured on a worker (convert.verify): how many
 * attempts, and how long it is offered first to a worker other than the one
 * that converted it, when there is one. */
#define AXIS_CONVERSION_VERIFY_ATTEMPTS 3
#define AXIS_CONVERSION_PREFER_OTHER_S  120

/* A worker counts as connected for the checks when heard from this
 * recently. */
#define AXIS_CONVERSION_WORKER_SEEN_S   60

/* Settings. */
#define AXIS_CONVERSION_SETTING_MIN_FREE         "convert.min_free"
#define AXIS_CONVERSION_SETTING_MIN_FREE_PERCENT "convert.min_free_percent"
#define AXIS_CONVERSION_SETTING_KEEP_ORIGINAL    "convert.keep_original"
#define AXIS_CONVERSION_SETTING_TRIAL_RETENTION  "convert.trial_retention"
#define AXIS_CONVERSION_SETTING_LEFTOVER_GRACE   "convert.leftover_grace"
#define AXIS_CONVERSION_SETTING_SIMILARITY       "convert.min_similarity"
#define AXIS_CONVERSION_SETTING_PROVE_TRIALS     "convert.prove_trials"
#define AXIS_CONVERSION_SETTING_OTHER_OWNER      "convert.allow_other_owner"
#define AXIS_CONVERSION_SETTING_NETWORK_FS       "convert.allow_network_fs"
#define AXIS_CONVERSION_SETTING_FFMPEG           "convert.ffmpeg_path"
#define AXIS_CONVERSION_SETTING_FFPROBE          "convert.ffprobe_path"
#define AXIS_CONVERSION_SETTING_PRESET           "convert.software_preset"
#define AXIS_CONVERSION_SETTING_CHECKS_WHERE     "convert.checks_where"
#define AXIS_CONVERSION_SETTING_WAIT_AFTER_START "convert.wait_after_start"
#define AXIS_CONVERSION_SETTING_CHECK_THREADS    "convert.check_threads"
#define AXIS_CONVERSION_SETTING_CONFIRM_WHOLE    "convert.confirm_whole_file"
#define AXIS_CONVERSION_SETTING_RUN_FAILURES     "convert.run_failures"

/**
 * AxisConversionChecksWhere:
 * @AXIS_CONVERSION_CHECKS_SERVER: "server": the daemon measures the checks
 *   itself (the default)
 * @AXIS_CONVERSION_CHECKS_WORKER: "worker": on a worker that can (connected,
 *   of the daemon's protocol, not paused, has proved the volume), else on
 *   this server
 * @AXIS_CONVERSION_CHECKS_WORKER_ONLY: "worker_only": only on a worker; the
 *   daemon never measures, and a conversion waits for one
 *
 * Where the measurements of a converted file's checks 5 to 8 are made; the
 * daemon decides every check in every mode.
 */
typedef enum
{
	AXIS_CONVERSION_CHECKS_SERVER,
	AXIS_CONVERSION_CHECKS_WORKER,
	AXIS_CONVERSION_CHECKS_WORKER_ONLY
} AxisConversionChecksWhere;

/* The checks. */
#define AXIS_CONVERSION_N_CHECKS        10

/*
 * The checks' rules.
 *
 * AXIS_CONVERSION_CHECKS_VERSION is the version of the rules of the checks of
 * the converted file itself (3 to 8) that this build judges by.  It is
 * recorded with each conversion they judged (conversion.checks_version); one
 * judged before versions were recorded counts as version 0.  Raise it with
 * every change to what one of those checks accepts.
 *
 * Raising it changes nothing that was judged before.  What does is a mark in
 * the build's list (marks[] in axis-conversion.c), which says which way a
 * check was wrong before the version the mark names, with the reason a person
 * reads on the pages.  For a change to check n made in new version V, add:
 *
 *  - AXIS_CONVERSION_MARK_STRICT (check n, before V) when the check was
 *    wrongly strict: it rejected files it should have passed (say, a fault
 *    that refused good files, now fixed).  Trials rejected by check n and
 *    judged before V stop counting against proving their revision: they count
 *    neither for nor against (the checks stop at the first failure, so what
 *    the later checks would have said of such a file is not known, and it is
 *    no pass), and their pages say so, with the reason.
 *  - AXIS_CONVERSION_MARK_LENIENT (check n, before V) when the check was too
 *    lenient: it passed files it should have rejected (say, a fault that let
 *    a damaged file through, now fixed).  Trials that passed every check and
 *    were judged before V stop counting towards proving their revision (their
 *    pages say so, with the reason), and a conversion whose checks all passed
 *    before V is checked again from the start when its finish job is tried
 *    again, never placed on that pass.
 *  - Both, when the change goes both ways (stricter for some files, more
 *    lenient for others).
 *  - Neither, when no file is judged otherwise (wording, speed): then do not
 *    raise the version either.
 *
 * A trial rejected for a reason that is not the profile's but that no mark
 * covers (or rejected before versions were recorded, when no mark covers its
 * check) is set aside by an administrator: axis_conversions_set_aside().  A
 * proof already recorded is not withdrawn by a later mark: it was a person's
 * decision, after playing the trial files.
 */
#define AXIS_CONVERSION_CHECKS_VERSION    1

typedef enum
{
	AXIS_CONVERSION_MARK_STRICT,
	AXIS_CONVERSION_MARK_LENIENT
} AxisConversionMarkKind;

/**
 * AxisConversionMark:
 * @check: the check, 3 to 8
 * @kind: which way it was wrong
 * @before: it was wrong for conversions judged by an earlier version (1 to
 *   AXIS_CONVERSION_CHECKS_VERSION)
 * @why: what was wrong, as the pages say it ("it refused untagged sources")
 */
typedef struct
{
	guint                  check;
	AxisConversionMarkKind kind;
	gint                   before;
	const gchar           *why;
} AxisConversionMark;

/* The build's marks (or a test's, axis_conversion_marks_set_for_test()). */
const AxisConversionMark *
axis_conversion_marks(guint *n_marks);

/* The strict mark under which a rejection by check @failed_check judged by
 * version @version (0 for before versions were recorded) counts no more, or
 * NULL when it counts. */
const AxisConversionMark *
axis_conversion_rejection_cleared(
	gint failed_check,
	gint version
);

/* The lenient mark under which a pass judged by @version counts no more, or
 * NULL when it counts. */
const AxisConversionMark *
axis_conversion_pass_voided(gint version);

/* SQL over a conversion aliased "c", built from the marks (free it): a trial
 * rejected by a check of the converted file that counts against proving its
 * revision (not set aside, not cleared by a strict mark). */
gchar *
axis_conversion_bad_trial_sql(void);

/* SQL over "c": a trial that passed every check and counts towards proving
 * (no lenient mark voids it). */
gchar *
axis_conversion_passed_trial_sql(void);

/* Tests only: @marks (@n_marks of them, kept by the caller) in place of the
 * build's, or the build's again with NULL. */
void
axis_conversion_marks_set_for_test(
	const AxisConversionMark *marks,
	guint                     n_marks
);

/* The temporary name of a placement is AXIS_VOLUME_TEMP_PREFIX and this
 * plus the conversion's id: a range of numbers the import pipeline (which
 * uses its rows' ids) never reaches. */
#define AXIS_CONVERSION_TEMP_BASE       G_GINT64_CONSTANT(1000000000000000000)

#define AXIS_CONVERSION_ERROR (axis_conversion_error_quark())

/**
 * AxisConversionError:
 * @AXIS_CONVERSION_ERROR_NOT_FOUND: no such file, revision or conversion
 * @AXIS_CONVERSION_ERROR_REFUSED: a gate refuses (the message says which)
 * @AXIS_CONVERSION_ERROR_BUSY: try again later (a volume away, a scan)
 * @AXIS_CONVERSION_ERROR_STATE: not in a state that allows it
 * @AXIS_CONVERSION_ERROR_FAILED: something else
 */
typedef enum
{
	AXIS_CONVERSION_ERROR_NOT_FOUND,
	AXIS_CONVERSION_ERROR_REFUSED,
	AXIS_CONVERSION_ERROR_BUSY,
	AXIS_CONVERSION_ERROR_STATE,
	AXIS_CONVERSION_ERROR_FAILED
} AxisConversionError;

GQuark
axis_conversion_error_quark(void);

typedef enum
{
	AXIS_CONVERSION_TRIAL,
	AXIS_CONVERSION_REPLACE
} AxisConversionOutcome;

/* "trial", "replace" */
const gchar *
axis_conversion_outcome_to_string(AxisConversionOutcome outcome);

gboolean
axis_conversion_outcome_from_string(
	const gchar           *text,
	AxisConversionOutcome *outcome
);

/* What a check is called ("It is the file that was asked for"), 1 to 10. */
const gchar *
axis_conversion_check_label(guint number);

/* A state for people ("Waiting for a worker", "Checking", "Done", ...). */
const gchar *
axis_conversion_state_label(const gchar *state);

/* The setting's word ("server", "worker", "worker_only"), and what it means
 * in plain words ("On this server", ...). */
const gchar *
axis_conversion_checks_where_to_string(AxisConversionChecksWhere where);

const gchar *
axis_conversion_checks_where_label(AxisConversionChecksWhere where);

/* What workers do in that mode, one sentence for the Workers page. */
const gchar *
axis_conversion_checks_where_sentence(AxisConversionChecksWhere where);

/* The kinds conversion work consists of, which the hold after a restart
 * holds (convert.encode, convert.verify, estimate.sample,
 * estimate.sample.remote), ended by %NULL. */
const gchar *const *
axis_conversion_held_kinds(void);

typedef struct _AxisConversions AxisConversions;

/**
 * axis_conversions_new:
 * @settings: (nullable): where the convert.* settings are read (and
 *   storage.read_only by the storage); %NULL: the defaults
 * @probe: the probe, asked to look at a replaced file again (referenced)
 * @estimator: the estimator: eligibility, estimates, calibration
 *   (referenced)
 */
AxisConversions *
axis_conversions_new(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	AxisStorage   *storage,
	AxisProbe     *probe,
	AxisEstimator *estimator
);

AxisConversions *
axis_conversions_ref(AxisConversions *self);

void
axis_conversions_unref(AxisConversions *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversions, axis_conversions_unref)

const AxisSettingInfo *
axis_conversions_get_setting_table(void);

AxisStorage *
axis_conversions_get_storage(AxisConversions *self);

/* Where checks run now (the setting; "server" without settings). */
AxisConversionChecksWhere
axis_conversions_checks_where(AxisConversions *self);

/* How long a replaced original is kept in the recycle folder
 * (convert.keep_original), in seconds. */
gint64
axis_conversions_keep_original(AxisConversions *self);

/* ── Held after a restart ───────────────────────────────────────── */

/* The hold of conversion work (transfer none): the job queue holds the
 * kinds of axis_conversion_held_kinds() by it (axisd/main.c). */
AxisJobHold *
axis_conversions_get_hold(AxisConversions *self);

/* At start-up: holds conversion work when convert.wait_after_start is on
 * (the default), from now. */
void
axis_conversions_hold_after_start(AxisConversions *self);

/* Whether conversion work is held, and since when (real time, µs). */
gboolean
axis_conversions_is_held(
	AxisConversions *self,
	gint64          *since_us
);

/* Called after a resume (any thread): the job queue is woken. */
void
axis_conversions_set_on_resume(
	AxisConversions *self,
	void           (*func)(gpointer data),
	gpointer         data
);

/* Releases the hold (@user, for the audit log) and queues the tidy job,
 * which queues the checks that waited.  *@was_held says whether it was. */
gboolean
axis_conversions_resume(
	AxisConversions  *self,
	const gchar      *user,
	gboolean         *was_held,
	GError          **error
);

/**
 * AxisConversionVerdictFunc:
 * @root_id: the file's root
 * @slug: the profile's slug (its current revision is what the verdict is
 *   for)
 *
 * The player check's verdict, worked out now.  Any thread.
 */
typedef AxisCompatVerdict (*AxisConversionVerdictFunc)(
	gpointer     data,
	gint64       root_id,
	const gchar *slug
);

/* Without one, every verdict is unknown (so nothing can be replaced).
 * Before use. */
void
axis_conversions_set_verdict(
	AxisConversions           *self,
	AxisConversionVerdictFunc  func,
	gpointer                   data
);

/**
 * AxisConversionTuning:
 * @ffmpeg, @ffprobe: (nullable): the tools' commands (tests); default the
 *   settings
 * @software_preset: (nullable): the software encoders' preset; default the
 *   setting
 * @threads: the software encoders' threads (0: theirs to choose)
 * @tool_timeout_ms: the limit of each check's tool run (0: ten minutes plus
 *   six times the movie's length)
 * @similarity_points: how many points check 7 compares (0: five)
 * @scan_wait_ms: how long a placement waits for a scan of its volume to
 *   end (0: ten minutes)
 */
typedef struct
{
	const gchar *const *ffmpeg;
	const gchar *const *ffprobe;
	const gchar        *software_preset;
	guint               threads;
	guint               tool_timeout_ms;
	guint               similarity_points;
	guint               scan_wait_ms;
} AxisConversionTuning;

/* Tests only, before use. */
void
axis_conversions_set_tuning(
	AxisConversions            *self,
	const AxisConversionTuning *tuning
);

typedef enum
{
	AXIS_CONVERSION_HOOK_CONTINUE,
	AXIS_CONVERSION_HOOK_FAIL,	/* the step fails, as an error would */
	AXIS_CONVERSION_HOOK_CRASH	/* stop here as a daemon that died: nothing is undone */
} AxisConversionHookVerdict;

/* Called before each step of the finish job (tests: fault injection);
 * @step is its name ("settle", "placing", "adopt", "link", "swap", ...). */
typedef AxisConversionHookVerdict (*AxisConversionHook)(
	const gchar *step,
	gint64       conversion_id,
	gpointer     data
);

void
axis_conversions_set_hook(
	AxisConversions    *self,
	AxisConversionHook  hook,
	gpointer            data
);

/* ── Requesting ─────────────────────────────────────────────────── */

/**
 * AxisConversionGate:
 * @id: a word ("read_only", "volume", ...)
 * @label: what it asks, for people
 * @applies: it applies to the outcome asked for (a gate that does not is
 *   shown and never refuses)
 * @passed: it holds now
 * @detail: (nullable): why not, or what it found
 */
typedef struct
{
	const gchar *id;
	const gchar *label;
	gboolean     applies;
	gboolean     passed;
	gchar       *detail;
} AxisConversionGate;

/**
 * AxisConversionPreview:
 *
 * What a request would do, as the confirmation shows it, with every gate
 * worked out now.
 */
typedef struct
{
	gint64                 file_id;
	gint64                 root_id;
	gint64                 volume_id;
	gchar                 *root_name;
	gchar                 *volume_name;
	gchar                 *rel_path;	/* stored form */
	gchar                 *new_rel_path;	/* stored form */
	gint64                 size;
	AxisProfileRevision   *revision;	/* nullable */
	AxisConversionOutcome  outcome;
	AxisCompatVerdict      verdict;
	gboolean               estimated;
	gint64                 estimate_low;
	gint64                 estimate_mid;
	gint64                 estimate_high;
	gchar                 *source_quality;	/* nullable */
	gchar                 *encoder;	/* nullable: the encoder a worker would use */
	gint64                 keep_seconds;	/* convert.keep_original */
	GPtrArray             *gates;	/* AxisConversionGate */
	gboolean               ok;	/* every gate that applies holds */
} AxisConversionPreview;

void
axis_conversion_preview_free(AxisConversionPreview *preview);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionPreview, axis_conversion_preview_free)

/* The first gate that applies and does not hold, or %NULL. */
const AxisConversionGate *
axis_conversion_preview_refusal(const AxisConversionPreview *preview);

/**
 * axis_conversions_preview:
 *
 * What a request of @file_id with @revision_id as @outcome would do, every
 * gate worked out now.  Changes nothing.  Fails with
 * %AXIS_CONVERSION_ERROR_NOT_FOUND for no such file or revision.
 */
gboolean
axis_conversions_preview(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	AxisConversionPreview **preview,
	GError                **error
);

/**
 * axis_conversions_request:
 * @confirmed: the administrator ticked that the original will be deleted
 *   (needed for a replacement)
 * @user: who asks
 * @conversion_id: (out): the new conversion
 *
 * Every gate again, then the record and the encode job.  Fails with
 * %AXIS_CONVERSION_ERROR_REFUSED (the first gate that refuses, in the
 * message) storing nothing.
 */
gboolean
axis_conversions_request(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *user,
	gint64                 *conversion_id,
	GError                **error
);

/**
 * axis_conversions_request_for_run:
 * @confirmed: the person who started the run ticked, on the run's page,
 *   that originals will be deleted (needed for a replacement)
 * @user: that person
 * @run_id, @entry_id: the conversion run and its entry that ask
 * @refused_gate: (out) (optional) (transfer full): the id of the gate that
 *   refused ("read_only", "volume", "space", "verdict", ...), %NULL when
 *   none did
 *
 * axis_conversions_request() exactly, the same gates in the same function,
 * with the conversion recorded as the run's.  A run adds no other way of
 * asking for a conversion.
 */
gboolean
axis_conversions_request_for_run(
	AxisConversions        *self,
	gint64                  file_id,
	gint64                  revision_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *user,
	gint64                  run_id,
	gint64                  entry_id,
	gint64                 *conversion_id,
	gchar                 **refused_gate,
	GError                **error
);

/* The current revision of every video profile that is not archived, as a
 * file's page offers them (AxisProfileRevision). */
gboolean
axis_conversions_offer(
	AxisConversions  *self,
	GPtrArray       **revisions,
	GError          **error
);

/* ── What the worker API calls ──────────────────────────────────── */

/**
 * axis_conversions_describe:
 * @claim: the convert.encode job a worker just claimed (it runs under the
 *   claim's lease)
 * @worker: the worker's name; @machine: its capabilities' machine
 * @work: (out): the work, when it can be done
 * @why_not: (out): when it cannot, why (a sentence); @permanent says
 *   whether the job ends for good (the conversion is then refused) or is
 *   tried again later
 *
 * The gates again, the plan for this worker's capabilities, the attempt
 * recorded.  FALSE only for a database failure.
 */
gboolean
axis_conversions_describe(
	AxisConversions     *self,
	AxisDbConn          *conn,
	const AxisJobClaim  *claim,
	const gchar         *worker,
	const gchar         *machine,
	AxisConvertWork    **work,
	gchar              **why_not,
	gboolean            *permanent,
	GError             **error
);

/**
 * axis_conversions_encoded:
 * @job: the encode job, locked in the caller's transaction
 * @result: what the worker reports
 *
 * Inside the transaction that ends the encode job: the output's name must be
 * the one the daemon chose for the attempt; the conversion goes on to be
 * checked (the finish job is queued in the same transaction), or is
 * rejected.
 */
gboolean
axis_conversions_encoded(
	AxisConversions          *self,
	AxisDbConn               *conn,
	gint64                    job,
	const AxisConvertResult  *result,
	const gchar              *worker,
	GError                  **error
);

/**
 * axis_conversions_describe_verify:
 * @claim: the convert.verify job a worker just claimed
 * @work: (out): what it is to measure, when it can
 * @why_not, @permanent: as for axis_conversions_describe()
 *
 * The conversion must still be being checked by this job; the description
 * is the one the daemon made when it handed the checks out (the original's
 * stamp and digest, the converted file's name and size, the plan).  FALSE
 * only for a database failure.
 */
gboolean
axis_conversions_describe_verify(
	AxisConversions     *self,
	AxisDbConn          *conn,
	const AxisJobClaim  *claim,
	const gchar         *worker,
	AxisVerifyWork     **work,
	gchar              **why_not,
	gboolean            *permanent,
	GError             **error
);

/**
 * axis_conversions_verified:
 * @job: the convert.verify job, locked in the caller's transaction
 * @result: what the worker measured
 *
 * Inside the transaction that ends the job: the measurements must fit the
 * plan the daemon made (else %AXIS_CONVERSION_ERROR_REFUSED: the worker's
 * report is refused and nothing is recorded); they are kept with the
 * conversion and its finish job is queued, which binds them to the files
 * and judges them.
 */
gboolean
axis_conversions_verified(
	AxisConversions         *self,
	AxisDbConn              *conn,
	gint64                   job,
	const AxisVerifyResult  *result,
	const gchar             *worker,
	GError                 **error
);

/* ── The jobs ───────────────────────────────────────────────────── */

gboolean
axis_conversions_run_finish(
	AxisConversions  *self,
	AxisJobContext   *context,
	GError          **error
);

gboolean
axis_conversions_run_tidy(
	AxisConversions  *self,
	AxisJobContext   *context,
	GError          **error
);

/**
 * axis_conversions_finish:
 * @context: (nullable): for cancellation and progress
 *
 * What the finish job does for one conversion: the checks and the
 * placement, or the recovery of a placement in flight.  Fails with
 * %AXIS_CONVERSION_ERROR_BUSY when it must be tried again later (nothing
 * concluded).
 */
gboolean
axis_conversions_finish(
	AxisConversions  *self,
	gint64            conversion_id,
	AxisJobContext   *context,
	GError          **error
);

/**
 * AxisConversionTidyReport:
 * @failed: conversions whose encode job ended without a result
 * @waiting: conversions whose attempt ended without a result (a lease that
 *   ran out, a job given back) and that wait for a worker again
 * @queued: finish jobs queued for conversions being checked or placed
 * @trials: trials removed after their time
 * @originals: recycled originals removed after their time
 * @leftovers: leftovers listed now
 * @removed: leftovers removed after the grace time
 */
typedef struct
{
	guint failed;
	guint waiting;
	guint queued;
	guint trials;
	guint originals;
	guint leftovers;
	guint removed;
} AxisConversionTidyReport;

gboolean
axis_conversions_tidy(
	AxisConversions           *self,
	AxisConversionTidyReport  *report,
	GError                   **error
);

/* Queues the tidy job now (at start-up, when a volume comes back). */
gboolean
axis_conversions_request_tidy(
	AxisConversions  *self,
	GError          **error
);

/* ── Actions ────────────────────────────────────────────────────── */

/* Stops a conversion that is queued or encoding (its job is cancelled);
 * %AXIS_CONVERSION_ERROR_STATE otherwise. */
gboolean
axis_conversions_cancel(
	AxisConversions  *self,
	gint64            conversion_id,
	const gchar      *user,
	GError          **error
);

/* A conversion run gives back its conversion @conversion_id of entry
 * @entry_id that waits for a worker (state queued: nothing was made of it):
 * in one transaction the conversion is cancelled, saying @text, and the entry
 * waits again in the list.  *@given is FALSE when the conversion is not
 * waiting for a worker any more (a worker took it meanwhile). */
gboolean
axis_conversions_give_back_for_run(
	AxisConversions  *self,
	gint64            conversion_id,
	gint64            entry_id,
	const gchar      *text,
	gboolean         *given,
	GError          **error
);

/* Removes a done trial's kept output now. */
gboolean
axis_conversions_remove_trial(
	AxisConversions  *self,
	gint64            conversion_id,
	const gchar      *user,
	GError          **error
);

/* ── Proving by hand ────────────────────────────────────────────── */

/**
 * AxisConversionProving:
 * @revision: the revision
 * @needed: how many passed trials proving needs (the setting)
 * @trials: (element-type AxisConversionRecord): its passed trials, one per
 *   file, newest first
 * @bad_trials: its trials rejected by a check of the output itself (3 to 8)
 *   that count against it (axis_conversion_bad_trial_sql())
 * @set_aside: its rejected trials an administrator set aside
 * @allowed: it may be proven now
 * @why_not: (nullable): why not
 */
typedef struct
{
	AxisProfileRevision *revision;
	guint                needed;
	GPtrArray           *trials;
	guint                bad_trials;
	guint                set_aside;
	gboolean             allowed;
	gchar               *why_not;
} AxisConversionProving;

void
axis_conversion_proving_free(AxisConversionProving *proving);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionProving, axis_conversion_proving_free)

gboolean
axis_conversions_proving(
	AxisConversions        *self,
	gint64                  revision_id,
	AxisConversionProving **proving,
	GError                **error
);

/**
 * axis_conversions_prove:
 * @trials: (array length=n_trials): the trials the confirmation named
 *
 * Marks the revision proven by @user, resting on @trials, which must be
 * exactly the passed trials axis_conversions_proving() lists now (the page
 * named them).  Fails with %AXIS_CONVERSION_ERROR_REFUSED otherwise.
 */
gboolean
axis_conversions_prove(
	AxisConversions  *self,
	gint64            revision_id,
	const gint64     *trials,
	guint             n_trials,
	const gchar      *user,
	GError          **error
);

/**
 * axis_conversions_set_aside:
 * @reason: why, in the administrator's words (1 to 500 characters once
 *   cleaned)
 *
 * Sets a trial rejected by a check of the converted file (3 to 8) aside, as
 * @user, saying @reason: from then on it counts neither for nor against
 * proving its revision, and its page says so.  Recorded and logged as an
 * audit line.  Fails with %AXIS_CONVERSION_ERROR_REFUSED for anything else
 * (not a trial, not rejected by such a check, set aside already, no reason).
 */
gboolean
axis_conversions_set_aside(
	AxisConversions  *self,
	gint64            conversion_id,
	const gchar      *user,
	const gchar      *reason,
	GError          **error
);

/* ── Reading ────────────────────────────────────────────────────── */

/**
 * AxisConversionRecord:
 *
 * A conversion as the pages show it.
 */
typedef struct
{
	gint64   id;
	gint64   file_id;
	gint64   volume_id;
	gint64   root_id;
	gchar   *root_name;
	gchar   *rel_path;
	gchar   *new_rel_path;
	gint64   revision_id;
	gchar   *profile_name;
	gchar   *profile_slug;
	gint     revision_number;
	gchar   *outcome;
	gchar   *state;
	gchar   *requested_by;
	gint64   requested_us;
	gchar   *verdict;
	gint64   src_size;
	gchar   *source_quality;
	gint64   estimate_bytes;
	gint64   job_id;
	gint64   finish_job_id;
	gint     attempt;
	gchar   *worker;
	gchar   *encoder;
	gchar   *output_name;
	gint64   output_size;
	gchar   *step;
	gchar   *recycle_path;
	gchar   *recycle_state;
	gint64   recycle_remove_us;
	gchar   *trial_name;
	gchar   *trial_state;
	gint64   trial_remove_us;
	gint     failed_check;
	gchar   *result_text;
	gint64   finished_us;
	gchar   *job_state;	/* the encode job's, nullable */
	gdouble  job_progress;
	gchar   *job_message;	/* nullable */
	gchar   *checked_where;	/* "server", "worker", nullable: not yet */
	gchar   *verifier;	/* the worker whose measurements were taken, nullable */
	gchar   *checks_waiting;	/* why the checks wait now, nullable */
	gchar   *verify_state;	/* the check job's, nullable */
	gdouble  verify_progress;
	gchar   *verify_message;	/* nullable */
	gchar   *verify_holder;	/* the worker running it now ("worker:name"), nullable */
	gint64   run_id;	/* the conversion run that asked for it, 0 for none */
	gint64   set_aside_us;	/* when an administrator set it aside, 0 for never */
	gchar   *set_aside_by;	/* nullable */
	gchar   *set_aside_reason;	/* nullable */
	gboolean counts_against;	/* a trial rejected by checks 3 to 8 that counts against proving */
	gint     checks_version;	/* the version of the checks' rules it was judged by, 0 for unknown */
	gboolean all_passed;	/* every check passed */
	const AxisConversionMark *cleared_by;	/* a rejection by checks 3 to 8 a strict mark clears, or NULL */
	const AxisConversionMark *voided_by;	/* a trial's pass a lenient mark voids, or NULL */
} AxisConversionRecord;

void
axis_conversion_record_free(AxisConversionRecord *record);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionRecord, axis_conversion_record_free)

/**
 * AxisConversionCheck:
 * @number: 1 to 10
 * @result: "passed", "failed", "skipped", or "pending" (not run yet)
 * @detail: what was found
 */
typedef struct
{
	guint  number;
	gchar *result;
	gchar *detail;
} AxisConversionCheck;

void
axis_conversion_check_free(AxisConversionCheck *check);

/* The newest @limit conversions (AxisConversionRecord). */
gboolean
axis_conversions_list(
	AxisConversions  *self,
	guint             limit,
	GPtrArray       **records,
	GError          **error
);

/* One conversion, with its ten checks (AxisConversionCheck, every number
 * once, "pending" for those not run).  Fails with
 * %AXIS_CONVERSION_ERROR_NOT_FOUND. */
gboolean
axis_conversions_get(
	AxisConversions        *self,
	gint64                  conversion_id,
	AxisConversionRecord  **record,
	GPtrArray             **checks,
	GError                **error
);

/* A file's conversions, newest first. */
gboolean
axis_conversions_for_file(
	AxisConversions  *self,
	gint64            file_id,
	GPtrArray       **records,
	GError          **error
);

/**
 * AxisConversionLeftover:
 * @ours: a name of Axis's (removed after the grace time); otherwise listed
 *   only
 */
typedef struct
{
	gint64    volume_id;
	gchar    *volume_name;
	gchar    *name;
	gint64    size;
	gboolean  ours;
	gint64    first_seen_us;
	gint64    remove_us;	/* when it will be removed, 0 for never */
} AxisConversionLeftover;

void
axis_conversion_leftover_free(AxisConversionLeftover *leftover);

gboolean
axis_conversions_leftovers(
	AxisConversions  *self,
	GPtrArray       **leftovers,
	GError          **error
);

/**
 * AxisConversionTrialRevision:
 *
 * A revision with trials, for the Conversions page: how many passed, how
 * many proving needs, and whether it is proven.
 */
typedef struct
{
	gint64   revision_id;
	gchar   *profile_name;
	gint     revision_number;
	guint    passed;
	guint    needed;
	guint    bad;	/* rejected by checks 3 to 8 and counting against it */
	guint    set_aside;	/* rejected trials set aside by an administrator */
	gboolean proven;
	gchar   *proof_method;
} AxisConversionTrialRevision;

void
axis_conversion_trial_revision_free(AxisConversionTrialRevision *row);

gboolean
axis_conversions_trial_revisions(
	AxisConversions  *self,
	GPtrArray       **rows,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_CONVERSION_H */
