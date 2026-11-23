/*
 * axis-estimate.h - whether a file would be converted to a target, and how
 * large it would be after
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is the decision the conversion changes will act on before they
 * convert a file and delete its original, so the rules are strict:
 *
 *  - Eligibility is a pure function of the probe's facts (AxisMediaInfo,
 *    as the probe stores it), the index's facts (AxisEstimateIndex) and the
 *    target (axis-estimate-target.h).  It reads no clock, no file and no
 *    database.  A file is eligible only when every fact that matters is
 *    known and allows it; a missing, unknown or conflicting fact makes it
 *    not eligible with a reason, never eligible by default.
 *  - "Not eligible" comes with exactly one reason, the first that applies in
 *    the fixed order of #AxisEstimateReason.  The order is part of the
 *    specification (openspec/specs/space-estimate).
 *  - Audio targets convert albums as units: the files of one album folder
 *    (axis_estimate_unit_key()) are eligible together or not at all.  An
 *    evaluation that is not given the whole unit (fewer files than the
 *    index counted in it) makes every file of it not eligible.
 *  - A size estimate is three figures, low <= middle <= high, finite, never
 *    negative, and the high figure is at most the file's size: a
 *    conversion keeps the original when the output is not smaller, so a
 *    file never ends larger than it is.  (The picture alone is also never
 *    estimated above 0.95 of the source picture.)  Whether the saving is
 *    enough is decided on the uncapped middle.
 *  - Monotonic: a higher quality level never estimates smaller, and at
 *    equal source bits per pixel a larger picture never estimates smaller.
 *
 * The model (priors and calibration) is axis-estimate-model.c; its numbers
 * are priors, stated as such, until real samples correct them.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-estimate.c).
 */

#ifndef AXIS_ESTIMATE_H
#define AXIS_ESTIMATE_H

#include <glib.h>

#include "probe/axis-probe-model.h"
#include "storage/axis-storage.h"
#include "transcode/axis-estimate-target.h"

G_BEGIN_DECLS

/* Bump when the model or the rules change: the Savings pages say which
 * version made a snapshot. */
#define AXIS_ESTIMATE_MODEL_VERSION 1

/**
 * AxisEstimateReason:
 *
 * Why a file is not converted to a target, in the fixed order in which
 * they are tried: a file gets the first that applies.
 */
typedef enum
{
	AXIS_ESTIMATE_ELIGIBLE = 0,
	AXIS_ESTIMATE_NOT_APPLICABLE,	/* not the kind of file or library the target is for */
	AXIS_ESTIMATE_NOT_PRESENT,	/* the last complete scan did not find it */
	AXIS_ESTIMATE_PINNED,		/* hard-linked, or under a downloads root */
	AXIS_ESTIMATE_NOT_PROBED,
	AXIS_ESTIMATE_PROBE_FAILED,	/* failed or timed out */
	AXIS_ESTIMATE_PROBE_STALE,	/* changed since, or an older probe version */
	AXIS_ESTIMATE_HDR_UNCERTAIN,	/* uncertain or unclassified HDR */
	AXIS_ESTIMATE_DOLBY_VISION,
	AXIS_ESTIMATE_HDR10PLUS,	/* until verified */
	AXIS_ESTIMATE_HDR_TO_SDR,	/* HDR, and the target is SDR only */
	AXIS_ESTIMATE_INTERLACED,	/* until handled */
	AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN,
	AXIS_ESTIMATE_LAYOUT,		/* container or streams the target cannot carry */
	AXIS_ESTIMATE_CODEC_UNKNOWN,	/* a source codec the estimator has no figures for */
	AXIS_ESTIMATE_LOSSY_SOURCE,	/* lossy to lossy, not allowed by the target */
	AXIS_ESTIMATE_FACTS_MISSING,	/* duration, picture size, frame rate or bitrate unknown */
	AXIS_ESTIMATE_FACTS_INCONSISTENT,	/* the probe's bitrates do not fit the file's size */
	AXIS_ESTIMATE_BETTER_CODEC,	/* already in a codec that saves more than the target's */
	AXIS_ESTIMATE_ALREADY_EFFICIENT,	/* the target's codec, at or below the threshold */
	AXIS_ESTIMATE_ALBUM,		/* another file of its album is not eligible */
	AXIS_ESTIMATE_WOULD_NOT_SHRINK,	/* the estimated saving is below the minimum */
	AXIS_ESTIMATE_OFFLINE,		/* eligible, but its volume is away now */
	AXIS_ESTIMATE_N_REASONS
} AxisEstimateReason;

/* "eligible", "not_applicable", ... as stored and in file list filters */
const gchar *
axis_estimate_reason_to_string(AxisEstimateReason reason);

gboolean
axis_estimate_reason_from_string(
	const gchar        *text,
	AxisEstimateReason *reason
);

/* "Pinned", "Probe failed", ... */
const gchar *
axis_estimate_reason_label(AxisEstimateReason reason);

/* One or two sentences: what the reason means and what would change it. */
const gchar *
axis_estimate_reason_help(AxisEstimateReason reason);

/* The ids of every reason but "eligible", in their order; ended by %NULL. */
const gchar *const *
axis_estimate_reason_ids(void);

/**
 * AxisEstimateIndex:
 *
 * What the index knows about a file (library_file, its root and volume).
 * The strings are borrowed.
 *
 * @kind: "video", "audio", ... (library_file.kind)
 * @root_type: what its root holds
 * @state: "present", "suspected" or "missing"
 * @pinned: library_file.pinned
 * @changed: the scanner saw new content since the probe
 * @volume_usable: its volume is online (or read-only) now
 * @counted: the path that counts the bytes of a hard-linked file (as in the
 *   Analysis); only aggregation reads it
 * @unit: the album unit key (axis_estimate_unit_key()), %NULL when the file
 *   is a unit of its own
 * @unit_files: how many present files the index has in that unit
 */
typedef struct
{
	gint64         file_id;
	gint64         root_id;
	gint64         volume_id;
	const gchar   *kind;
	AxisMediaType  root_type;
	const gchar   *state;
	gint64         size;
	gint64         nlink;
	gboolean       pinned;
	gboolean       changed;
	gint64         mtime_sec;
	gint           mtime_nsec;
	gint64         ctime_sec;
	gint           ctime_nsec;
	gint64         inode;
	gboolean       volume_usable;
	gboolean       counted;
	const gchar   *rel_path;
	const gchar   *unit;
	guint          unit_files;
} AxisEstimateIndex;

/**
 * AxisEstimateFile:
 * @index: the index's facts
 * @info: (nullable): the probe's record with its streams; %NULL when the
 *   file was never probed
 */
typedef struct
{
	AxisEstimateIndex    index;
	const AxisMediaInfo *info;
} AxisEstimateFile;

/* ── The model ──────────────────────────────────────────────────── */

typedef struct _AxisEstimateModel AxisEstimateModel;

/**
 * AxisEstimateSample:
 *
 * One real result (estimate_sample): the facts the model predicts from and
 * the output bitrate that was seen.  @part is "video" or "audio".
 *
 * A sample lands in the cell of its codec, encoder family, quality and
 * picture size (or audio class), whatever profile revision measured it: two
 * revisions whose output-affecting part is the same for that cell share it.
 * What the prediction needs beyond the facts (the output's bit depth, the
 * nominal audio bitrate, whether audio was reduced to CD quality) is given
 * with @has_output_facts; without, it is read from the built-in target
 * named @target (samples made before profiles carried it).
 */
typedef struct
{
	const gchar     *target;
	gint64           revision_id;
	gboolean         has_output_facts;
	gint             out_bit_depth;
	gint             nominal_bitrate;
	gboolean         reduce_to_cd;
	AxisTargetCodec  codec;
	AxisEncoderFamily family;
	gint             quality;
	const gchar     *part;
	const gchar     *source_codec;
	gint             width;
	gint             height;
	gdouble          frame_rate;
	gint64           source_bitrate;
	gint             sample_rate;
	gint             channels;
	gint             bit_depth;
	gdouble          seconds;
	gint64           output_bitrate;
} AxisEstimateSample;

/* Whether a sample's facts are within the ranges the store accepts. */
gboolean
axis_estimate_sample_is_valid(const AxisEstimateSample *sample);

/* The priors, uncalibrated.  Reference counted: a finished model is only
 * read, so threads may share it. */
AxisEstimateModel *
axis_estimate_model_new(void);

AxisEstimateModel *
axis_estimate_model_ref(AxisEstimateModel *model);

/* Drops a reference (the last frees it). */
void
axis_estimate_model_free(AxisEstimateModel *model);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisEstimateModel, axis_estimate_model_free)

/**
 * axis_estimate_model_add_sample:
 *
 * Adds a real result to the calibration (a sample that is not valid, or
 * that names a cell the model does not have, is ignored and FALSE is
 * returned).  Call axis_estimate_model_finish() after the last one.
 */
gboolean
axis_estimate_model_add_sample(
	AxisEstimateModel        *model,
	const AxisEstimateSample *sample
);

/* Works out the calibrated table.  Without it the model is the priors. */
void
axis_estimate_model_finish(AxisEstimateModel *model);

/* Samples at which a cell's prior is replaced (blended before). */
#define AXIS_ESTIMATE_CALIBRATED_SAMPLES 20

typedef enum
{
	AXIS_CALIBRATION_NONE,		/* no sample */
	AXIS_CALIBRATION_PARTLY,	/* some, fewer than AXIS_ESTIMATE_CALIBRATED_SAMPLES */
	AXIS_CALIBRATION_FULL
} AxisCalibrationState;

/* "uncalibrated", "partly calibrated", "calibrated" */
const gchar *
axis_calibration_state_label(AxisCalibrationState state);

/* "none", "partly", "full" */
const gchar *
axis_calibration_state_to_string(AxisCalibrationState state);

/* How many cells the model has; a cell is what a file's estimate comes
 * from (a prior and its calibration). */
guint
axis_estimate_model_n_cells(const AxisEstimateModel *model);

/**
 * AxisEstimateCellInfo:
 * @name: "hevc/software/q4/1080p", "opus/software/q4/transcode", ...
 * @samples: real results it has
 * @state: what they make of it
 * @factor: the calibrated correction to the prior's middle (1 for none)
 * @group: cells whose errors are taken as one (all uncalibrated cells of a
 *   codec and encoder family share one: their priors come from the same
 *   reasoning)
 */
typedef struct
{
	const gchar          *name;
	guint                 samples;
	AxisCalibrationState  state;
	gdouble               factor;
	guint                 group;
} AxisEstimateCellInfo;

void
axis_estimate_model_cell_info(
	const AxisEstimateModel *model,
	guint                    cell,
	AxisEstimateCellInfo    *info
);

/* The state of a set of cells: none if no cell has a sample, full if every
 * one is calibrated, partly otherwise. */
AxisCalibrationState
axis_estimate_model_state_of(
	const AxisEstimateModel *model,
	const guint             *cells,
	guint                    n_cells
);

/* The cells @target can use (at its quality, every picture size), for the
 * state of a target with no files yet. */
guint
axis_estimate_model_target_cells(
	const AxisEstimateModel  *model,
	const AxisEstimateTarget *target,
	guint                    *cells,
	guint                     max
);

/* ── Deciding ───────────────────────────────────────────────────── */

/**
 * AxisEstimateResult:
 * @reason: eligible, or why not
 * @album_reason: for %AXIS_ESTIMATE_ALBUM, the reason of the file that
 *   made the album ineligible
 * @album_file: and that file (0 when the album was not given whole)
 * @sized: whether @low, @mid and @high hold an estimate (eligible, offline,
 *   album and would-not-shrink files have one when it could be made)
 * @now: the file's size
 * @low, @mid, @high: the estimated size after, whole bytes
 * @cell: the model cell the estimate came from
 * @quality: the quality level used (after the target's 4K rule)
 */
typedef struct
{
	AxisEstimateReason reason;
	AxisEstimateReason album_reason;
	gint64             album_file;
	gboolean           sized;
	gint64             now;
	gint64             low;
	gint64             mid;
	gint64             high;
	guint              cell;
	gint               quality;
} AxisEstimateResult;

/**
 * axis_estimate_evaluate:
 * @files: (array length=n_files): one unit: a file alone, or the files of
 *   one album folder for an audio target
 * @target: the target
 * @model: the model (priors and calibration)
 * @results: (array length=n_files) (out caller-allocates): the decision for
 *   each file, in the same order
 *
 * The one decision the estimates, the plans and the conversions use.
 */
void
axis_estimate_evaluate(
	const AxisEstimateFile *const *files,
	guint                          n_files,
	const AxisEstimateTarget      *target,
	const AxisEstimateModel       *model,
	AxisEstimateResult            *results
);

/* For one file: axis_estimate_evaluate() on a unit of one.  For an audio
 * file of an album of several files this is never eligible (reason album):
 * an album is decided whole. */
AxisEstimateReason
axis_estimate_eligibility(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *result
);

/* Whether a result counts as "would be converted" in the reports: eligible,
 * or eligible on a volume that is away (counted from the index as it last
 * was, so totals do not move while a disk is unplugged). */
gboolean
axis_estimate_result_converts(const AxisEstimateResult *result);

/**
 * axis_estimate_unit_key:
 * @kind: library_file.kind
 * @root_type: the root's media type
 * @rel_path: the file's path in its root, stored form
 *
 * The album unit a file belongs to: for an audio file in a music or
 * audiobooks root, the folder two levels below the root (artist and album,
 * author and book: the album of the Analysis pages) when the file is at
 * least that deep, else the folder it is in ("" for the root's top); %NULL
 * for any other file (a unit of its own).  AXIS_ESTIMATE_SQL_UNIT computes
 * the same in SQL.
 *
 * Returns: (transfer full) (nullable): the key
 */
gchar *
axis_estimate_unit_key(
	const gchar   *kind,
	AxisMediaType  root_type,
	const gchar   *rel_path
);

/* The same, over library_file f and library_root r. */
#define AXIS_ESTIMATE_SQL_UNIT \
	"(CASE WHEN f.kind = 'audio' AND r.media_type IN ('music', 'audiobooks') THEN" \
	" CASE WHEN f.rel_path ~ '^[^/]+/[^/]+/' THEN substring(f.rel_path FROM '^[^/]+/[^/]+')" \
	" ELSE library_dir_of(f.rel_path) END END)"

/* ── Aggregates ─────────────────────────────────────────────────── */

/**
 * AxisEstimateTotal:
 *
 * The sum of the estimates of the files that would be converted, with a
 * range: middles add up; within one correlation group (a calibrated cell,
 * or every uncalibrated cell of a codec and encoder family) the deviations
 * add up as if all wrong together, and different groups combine as
 * independent.  Add files with axis_estimate_total_add(); read with
 * axis_estimate_total_get().
 */
typedef struct _AxisEstimateTotal AxisEstimateTotal;

AxisEstimateTotal *
axis_estimate_total_new(void);

void
axis_estimate_total_free(AxisEstimateTotal *total);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisEstimateTotal, axis_estimate_total_free)

void
axis_estimate_total_add(
	AxisEstimateTotal       *total,
	const AxisEstimateModel *model,
	const AxisEstimateResult *result
);

/**
 * axis_estimate_total_get:
 * @files: (out) (optional): how many
 * @now: (out) (optional): their size now
 * @low: (out) (optional): the low end of the range of their size after
 * @mid: (out) (optional): the sum of the middles
 * @high: (out) (optional): the high end
 */
void
axis_estimate_total_get(
	const AxisEstimateTotal *total,
	gint64                  *files,
	gint64                  *now,
	gint64                  *low,
	gint64                  *mid,
	gint64                  *high
);

/* The cells the files came from (for the calibration state of a figure). */
const GArray *
axis_estimate_total_cells(const AxisEstimateTotal *total);

G_END_DECLS

#endif /* AXIS_ESTIMATE_H */
