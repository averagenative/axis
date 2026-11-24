/*
 * axis-conversion-private.h - shared by the files of the conversions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_CONVERSION_PRIVATE_H
#define AXIS_CONVERSION_PRIVATE_H

#include "transcode/axis-conversion.h"

#include <sys/stat.h>

#include "core/axis-size.h"
#include "storage/axis-volume-guard.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-estimate.h"

G_BEGIN_DECLS

struct _AxisConversions
{
	gint                       refs;	/* atomic */
	AxisDbPool                *pool;
	AxisSettings              *settings;	/* nullable */
	AxisStorage               *storage;
	AxisProbe                 *probe;
	AxisEstimator             *estimator;

	GMutex                     lock;
	AxisConversionVerdictFunc  verdict;	/* (lock) */
	gpointer                   verdict_data;
	AxisConversionTuning       tuning;	/* (lock) the strings owned */
	gchar                    **ffmpeg;	/* (lock) tuning's */
	gchar                    **ffprobe;	/* (lock) */
	gchar                     *preset;	/* (lock) */
	AxisConversionHook         hook;	/* (lock) */
	gpointer                   hook_data;

	AxisJobHold               *hold;	/* conversion work after a restart */
	gint64                     held_since_us;	/* (lock) real time, 0 when not held */
	void                     (*on_resume)(gpointer data);	/* (lock) */
	gpointer                   on_resume_data;
};

/* ── axis-conversion.c ──────────────────────────────────────────── */

gchar *
axis_conversion_int_text(gint64 value);

gint64
axis_conversion_setting_integer(
	AxisConversions *self,
	const gchar     *key,
	gint64           fallback
);

gdouble
axis_conversion_setting_decimal(
	AxisConversions *self,
	const gchar     *key,
	gdouble          fallback
);

gboolean
axis_conversion_setting_boolean(
	AxisConversions *self,
	const gchar     *key,
	gboolean         fallback
);

/* The tools' commands (the tuning's, else the settings'), ended by %NULL. */
gchar **
axis_conversion_ffmpeg(AxisConversions *self);

gchar **
axis_conversion_ffprobe(AxisConversions *self);

/* The machine's options a plan is built with (the strings are static or
 * the object's). */
void
axis_conversion_options(
	AxisConversions    *self,
	AxisConvertOptions *options
);

/* The options recorded with a conversion (its preset, a word of the table or
 * "medium", and its threads): the strings are static. */
void
axis_conversion_options_recorded(
	const gchar        *preset,
	guint               threads,
	AxisConvertOptions *options
);

AxisCompatVerdict
axis_conversion_verdict(
	AxisConversions *self,
	gint64           root_id,
	const gchar     *slug
);

AxisConversionHookVerdict
axis_conversion_hook(
	AxisConversions *self,
	const gchar     *step,
	gint64           conversion_id
);

guint
axis_conversion_tool_timeout(
	AxisConversions *self,
	gint64           duration_ms
);

/* ── The row ────────────────────────────────────────────────────── */

/**
 * AxisConversionRow:
 *
 * Everything recorded of a conversion, as the finish job and the tidy job
 * read it (axis_conversion_row_load()).
 */
typedef struct
{
	gint64   id;
	gint64   file_id;
	gint64   volume_id;
	gint64   root_id;
	gchar   *root_path;
	gchar   *rel_path;
	gchar   *new_rel_path;
	gchar   *container;
	gint64   revision_id;
	AxisConversionOutcome outcome;
	gchar   *state;
	gchar   *requested_by;
	gchar   *verdict;
	gint64   src_size;
	gint64   src_mtime_sec;
	gint     src_mtime_nsec;
	gint64   src_ctime_sec;
	gint     src_ctime_nsec;
	gint64   src_inode;
	gint64   src_nlink;
	gchar   *source_quality;
	gint64   job_id;
	gint64   finish_job_id;
	gint     attempt;
	gchar   *worker;
	gchar   *machine;
	gchar   *encoder;
	gchar   *args;
	gchar   *output_name;
	gint64   output_reported;
	gint64   output_inode;
	gint64   output_size;
	gint64   output_mtime_sec;
	gint     output_mtime_nsec;
	gint64   output_ctime_sec;
	gint     output_ctime_nsec;
	gchar   *step;
	gchar   *temp_name;
	gchar   *recycle_path;
	gint64   recycle_inode;
	gint64   recycle_size;
	gchar   *recycle_state;
	gchar   *trial_name;
	gchar   *trial_state;
	gchar   *checked_where;	/* "server", "worker", or NULL */
	gint64   verify_job_id;
	gchar   *verifier;
	gchar   *check_plan;	/* the plan's JSON as sent to the worker */
	gchar   *check_source_sample;
	gboolean handed;	/* the handed_* stamp is recorded */
	gint64   handed_inode;
	gint64   handed_size;
	gint64   handed_mtime_sec;
	gint     handed_mtime_nsec;
	gint64   handed_ctime_sec;
	gint     handed_ctime_nsec;
	gchar   *evidence;	/* the worker's measurements' JSON, or NULL */
	gchar   *checks_waiting;
	gboolean passed;	/* every check passed for the passed_* file */
	gint64   passed_inode;
	gint64   passed_size;
	gint64   passed_mtime_sec;
	gint     passed_mtime_nsec;
	gint64   passed_ctime_sec;
	gint     passed_ctime_nsec;
	gchar   *passed_sample;	/* the daemon's sample digest of it then */
	gchar   *passed_digest;	/* nullable: its whole SHA-256 (a worker's, bound) */
	gint     claims;	/* how many times the encode was handed out */
	gchar   *plan_caps;	/* nullable: the capabilities the plan was made with */
	gchar   *plan_preset;	/* nullable: the software preset it was made with */
	gint     plan_threads;	/* the software encoders' threads it was made with */
	gint     checks_version;	/* the checks' rules it was judged by (0: before versions) */
	gboolean convert_kept;	/* asked for although the file is kept (convert-kept-files) */
} AxisConversionRow;

void
axis_conversion_row_free(AxisConversionRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConversionRow, axis_conversion_row_free)

/* The row, or %NULL (and TRUE) for none; @for_update locks it (inside a
 * transaction). */
gboolean
axis_conversion_row_load(
	AxisDbConn          *conn,
	gint64               id,
	gboolean             for_update,
	AxisConversionRow  **row,
	GError             **error
);

/* The conversion whose encode job is @job (locked). */
gboolean
axis_conversion_row_by_job(
	AxisDbConn          *conn,
	gint64               job,
	AxisConversionRow  **row,
	GError             **error
);

/* The conversion whose check job is @job (locked). */
gboolean
axis_conversion_row_by_verify_job(
	AxisDbConn          *conn,
	gint64               job,
	AxisConversionRow  **row,
	GError             **error
);

/* Sets @state and (nullable) the result's check and text, and finished_at
 * when the state is final. */
gboolean
axis_conversion_set_state(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *state,
	gint          failed_check,
	const gchar  *text,
	GError      **error
);

/* Records check @number ("passed", "failed", "skipped") with @detail
 * (@conn %NULL: a connection of @pool for the statement). */
gboolean
axis_conversion_record_check_pool(
	AxisDbPool   *pool,
	gint64        id,
	guint         number,
	const gchar  *result,
	const gchar  *detail,
	GError      **error
);

gboolean
axis_conversion_record_check(
	AxisDbConn   *conn,
	gint64        id,
	guint         number,
	const gchar  *result,
	const gchar  *detail,
	GError      **error
);

/* Queues the finish job of @id (de-duplicated); its job id in *@job. */
gboolean
axis_conversion_queue_finish(
	AxisDbConn  *conn,
	gint64       id,
	gint64       delay_ms,
	gint64      *job,
	GError     **error
);

/* The path of the file at @rel_path (stored form) in the root @root_path, as
 * the volume's directories: the folder (relative to the volume's top,
 * normalised, "" for the top) and the name's bytes.  FALSE for a path that
 * cannot be decoded. */
gboolean
axis_conversion_split(
	const gchar  *root_path,
	const gchar  *rel_path,
	gchar       **folder,
	gchar       **name
);

/* ── axis-conversion-gates.c ────────────────────────────────────── */

/* What a file kept by hand or brought over is told, by a gate and by a run's
 * list: the same words. */
#define AXIS_CONVERSION_KEPT_TEXT \
	"Kept: Axis does not replace this file. Let Axis upgrade it again on its links page to allow a conversion."

/* What the same gate says of a kept file an administrator chose to convert
 * (convert-kept-files): it holds, and the mark is not touched. */
#define AXIS_CONVERSION_KEPT_CHOSEN_TEXT \
	"The file is kept, and converting kept files was chosen for this conversion: it is converted in place and stays " \
	"kept, so Axis still does not replace it with another release."

/**
 * AxisConversionFacts:
 *
 * What the gates are worked out from, read now.
 */
typedef struct
{
	AxisEstimateRows    *rows;	/* the file, with the estimator's facts of its probe */
	const AxisEstimateFile *file;	/* in @rows, or NULL */
	AxisMediaInfo       *full;	/* the probe's whole record (plans, checks), or NULL */
	AxisRootInfo        *root;	/* nullable */
	AxisVolumeInfo      *volume;	/* nullable */
	AxisProfileRevision *revision;	/* nullable */
	AxisEstimateTarget   target;
	gboolean             has_target;
	gboolean             pinned_downloads;
	gboolean             kept;	/* a link of the file is kept (by hand, or brought over) */
	gboolean             is_current;	/* the revision is its profile's current one */
	gchar               *source_quality;	/* nullable */
	gchar               *container;	/* "mkv", "mp4" */
	gchar               *new_rel_path;	/* stored form */
	gchar               *new_rel_taken;	/* nullable: why the new name is taken */
	gboolean             memory;	/* not worth it or converted with the revision */
	gchar               *memory_reason;
	gint64               other_in_flight;	/* another conversion of the file, or 0 */
} AxisConversionFacts;

void
axis_conversion_facts_clear(AxisConversionFacts *facts);

/**
 * AxisConversionInputs:
 *
 * What the gates need from other parts that read the database themselves
 * (the estimator's model, the player check's verdict): worked out before the
 * caller takes the connection the gates run on, so that a job never holds
 * two at once.
 */
typedef struct
{
	AxisEstimateModel *model;	/* nullable: the estimates cannot be worked out */
	AxisCompatVerdict  verdict;
} AxisConversionInputs;

void
axis_conversion_inputs_load(
	AxisConversions            *self,
	const AxisConversionFacts  *facts,
	AxisConversionInputs       *inputs
);

void
axis_conversion_inputs_clear(AxisConversionInputs *inputs);

gboolean
axis_conversion_facts_load(
	AxisConversions      *self,
	AxisDbConn           *conn,
	gint64                file_id,
	gint64                revision_id,
	gint64                exclude,
	AxisConversionFacts  *facts,
	GError              **error
);

/**
 * axis_conversion_gates:
 * @guard: (nullable): a write guard already held (verification), else one
 *   is taken and released here
 * @plan_machines: work out G8 for the registered workers' capabilities
 * @stamp: (nullable): the stamp recorded at request (G15), checked against
 *   the index (and, with @guard, the file)
 * @convert_kept: an administrator chose to convert the file although it is
 *   kept (the request's tick, or the run's choice; afterwards what the
 *   conversion's row recorded).  It lifts the gate "kept" and no other: the
 *   only place the kept mark is passed over.
 *
 * Every gate for @facts and @outcome, appended to @gates
 * (AxisConversionGate); the estimate and the encoder found, in @preview
 * when given.
 */
gboolean
axis_conversion_gates(
	AxisConversions             *self,
	AxisDbConn                  *conn,
	const AxisConversionFacts   *facts,
	const AxisConversionInputs  *inputs,
	AxisConversionOutcome        outcome,
	gboolean                     convert_kept,
	AxisVolumeGuard             *guard,
	gboolean                     plan_machines,
	const AxisConversionRow     *stamp,
	AxisConversionPreview       *preview,
	GPtrArray                   *gates,
	GError                     **error
);

/* The plan for @facts on @machine's capabilities (NULL: refusal set). */
AxisConvertPlan *
axis_conversion_plan(
	AxisConversions            *self,
	AxisDbConn                 *conn,
	const AxisConversionFacts  *facts,
	const gchar                *machine,
	AxisConvertRefusal         *refusal,
	GError                    **error
);

/* The plan for @facts made again from what @row recorded when the work was
 * handed out (the capabilities, the preset and the threads), so that a
 * setting or a worker's encoders changed since make no difference; a row of
 * before 0049 has none recorded: then as axis_conversion_plan() for its
 * machine.  NULL: refusal set (@error for a database failure). */
AxisConvertPlan *
axis_conversion_plan_recorded(
	AxisConversions            *self,
	AxisDbConn                 *conn,
	const AxisConversionFacts  *facts,
	const AxisConversionRow    *row,
	AxisConvertRefusal         *refusal,
	GError                    **error
);

/* The name of the output of @row's claim number @claims ("<id>-<n>.tmp"). */
gchar *
axis_conversion_output_name(
	gint64 id,
	gint   claims
);

/* ── axis-conversion-verify.c ───────────────────────────────────── */

/**
 * AxisConversionCheckMode:
 * @AXIS_CONVERSION_CHECK_ALL: the ten checks, the daemon measuring 5 to 8
 *   itself (with convert.check_threads)
 * @AXIS_CONVERSION_CHECK_HAND_OUT: checks 1 to 4, then what a worker needs
 *   to measure 5 to 8 (the plan, the original's digest, the output's stamp)
 * @AXIS_CONVERSION_CHECK_JUDGE: the ten checks, 5 to 8 judged from a
 *   worker's measurements after they were bound to the files
 * @AXIS_CONVERSION_CHECK_AGAIN: every check passed already (the row's
 *   passed_* file): checks 1 and 2, the output required to be that file,
 *   then 9 and 10; 3 to 8 are not judged again
 */
typedef enum
{
	AXIS_CONVERSION_CHECK_ALL,
	AXIS_CONVERSION_CHECK_HAND_OUT,
	AXIS_CONVERSION_CHECK_JUDGE,
	AXIS_CONVERSION_CHECK_AGAIN
} AxisConversionCheckMode;

/**
 * AxisConversionVerify:
 * @plan: (nullable): HAND_OUT: what the worker is to measure
 * @source_sample: (nullable): HAND_OUT: the original's sample digest, read
 *   now from the daemon's disk
 * @unbound: (nullable): JUDGE: why the worker's measurements are not of
 *   these files (they were not used; nothing was concluded of the file)
 * @not_judged: (nullable): why the converted file was not judged: the plan
 *   made again from the conversion's records is not the one the worker was
 *   given (no check of the file was made; it is not a rejection)
 * @not_passed: AGAIN: the output is not the file whose checks passed (it is
 *   to be checked from the start)
 * @output_sample: (nullable): ALL, JUDGE: the daemon's digest of samples of
 *   the output when every check passed
 * @output_digest: (nullable): JUDGE with the whole file confirmed: its
 *   SHA-256
 *
 * What the checks found, for the placement.
 */
typedef struct
{
	struct stat     output;	/* after settling */
	struct stat     source;
	guint64         picture_bytes;
	gdouble         picture_seconds;
	gboolean        worth_it_failed;
	AxisVerifyPlan *plan;
	gchar          *source_sample;
	gchar          *unbound;
	gchar          *not_judged;
	gboolean        not_passed;
	gchar          *output_sample;
	gchar          *output_digest;
} AxisConversionVerify;

void
axis_conversion_verify_clear(AxisConversionVerify *found);

/* Runs the checks of @mode (recorded as they run) on the output in @work;
 * FALSE with @failed the first check that failed, or with @failed 0 and
 * either @error (that concludes nothing), found->unbound (JUDGE: the
 * worker's measurements are not of these files), found->not_judged (the
 * plan cannot be made again as the worker was given it) or found->not_passed
 * (AGAIN: not the file whose checks passed).  @evidence is the worker's, for
 * JUDGE.  It takes a connection from the pool for each step and holds none
 * while a tool runs. */
gboolean
axis_conversion_verify(
	AxisConversions            *self,
	AxisConversionRow          *row,
	AxisVolumeGuard            *guard,
	gint                        work,
	AxisJobContext             *context,
	AxisConversionCheckMode     mode,
	const AxisVerifyEvidence   *evidence,
	AxisConversionVerify       *found,
	guint                      *failed,
	GError                    **error
);

/* ── axis-conversion-place.c ────────────────────────────────────── */

gboolean
axis_conversion_place(
	AxisConversions            *self,
	AxisConversionRow          *row,
	AxisVolumeGuard            *guard,
	gint                        work,
	const AxisConversionVerify *found,
	GError                    **error
);

/* A placement in flight: what the records and the disk say, finished or
 * undone (the recovery table of design.md). */
gboolean
axis_conversion_recover(
	AxisConversions    *self,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	GError            **error
);

/* Removes a done conversion's recycled original when its time has come
 * (now, with @now).  *@removed when it went. */
gboolean
axis_conversion_remove_original(
	AxisConversions    *self,
	AxisDbConn         *conn,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	gboolean           *removed,
	GError            **error
);

/* The work area of @guard's volume, made when missing (with @make); -1 and
 * @error otherwise. */
gint
axis_conversion_work_area(
	AxisVolumeGuard  *guard,
	gboolean          make,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_CONVERSION_PRIVATE_H */
