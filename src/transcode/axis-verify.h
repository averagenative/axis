/*
 * axis-verify.h - the measurements of a converted file's checks 5 to 8, and
 * the judges that decide those checks from them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The daemon's ten checks (transcode/axis-conversion-verify.c) decide
 * whether a converted file may take its original's place.  Four of them
 * need tool runs that read whole files and decode pictures: the full decode
 * (5), the picture's packet counts (6), the copied tracks' data (7, with the
 * output's hashes made by 5's reading) and the picture comparison (8).
 * Those measurements are made here, by one code path that the daemon runs
 * on its own disk and that axis-worker runs through its mount
 * (convert.verify, worker protocol 3), and they are judged here, by pure
 * functions that only the daemon calls: a worker reports figures, never a
 * verdict.  A check passes on exactly the same figures whoever measured
 * them, and says the same sentence.
 *
 * Every tool is ffmpeg or ffprobe run through the probe runner
 * (probe/axis-probe-run.h): its own process group, the lowest CPU priority
 * and the idle I/O class, an address-space limit, a CPU-time limit and the
 * plan's wall-clock limit; the files are descriptors 3 and 4, read only, and
 * only the fd protocol and the probe's container whitelist are allowed.
 * With a thread count, each input's decoder and the filters get that many
 * threads (the daemon's convert.check_threads); without one ffmpeg chooses
 * (as many as the machine has cores).
 *
 * In libaxis-core.a: GLib, GIO and the probe runner only.  Blocks.
 */

#ifndef AXIS_VERIFY_H
#define AXIS_VERIFY_H

#include <gio/gio.h>
#include <glib.h>

#include "probe/axis-probe-run.h"
#include "transcode/axis-convert.h"

G_BEGIN_DECLS

#define AXIS_VERIFY_MAX_POINTS      20
#define AXIS_VERIFY_MAX_TRACKS      512	/* AXIS_PROBE_MAX_STREAMS */
#define AXIS_VERIFY_SAID_MAX_CHARS  300
#define AXIS_VERIFY_MIN_TIMEOUT_MS  1000
#define AXIS_VERIFY_MAX_TIMEOUT_MS  604800000	/* a week */
#define AXIS_VERIFY_MAX_COUNT       G_GINT64_CONSTANT(4503599627370496)
#define AXIS_VERIFY_MAX_START_MS    G_GINT64_CONSTANT(31536000000)	/* a year */
#define AXIS_VERIFY_MAX_THREADS     64

/* ── The plan ───────────────────────────────────────────────────── */

/**
 * AxisVerifyTrack:
 * @source: the copied track's index in the original (ffprobe's)
 * @output: its position in the converted file
 * @aac: it is AAC, brought to one framing (aac_adtstoasc) before hashing
 */
typedef struct
{
	gint     source;
	guint    output;
	gboolean aac;
} AxisVerifyTrack;

/**
 * AxisVerifyPoint:
 * @start_ms: where the compared window starts in both files
 * @length_ms: how long it is
 */
typedef struct
{
	gint64 start_ms;
	gint64 length_ms;
} AxisVerifyPoint;

/**
 * AxisVerifyPlan:
 * @picture_source: the picture's index in the original
 * @picture_output: its position in the converted file
 * @n_tracks: how many tracks the plan copies as packets (attachments are
 *   compared by the daemon from the headers, not here)
 * @tracks: (array length=n_tracks): in the plan's order
 * @n_points: where the pictures are compared, 0 to AXIS_VERIFY_MAX_POINTS
 *   (0 when the movie's length is not known: check 8 then fails)
 * @timeout_ms: the wall-clock limit of each tool run
 *
 * What to measure: made by the daemon from its plan of the conversion, sent
 * to a worker as it is.
 */
typedef struct
{
	gint             picture_source;
	guint            picture_output;
	guint            n_tracks;
	AxisVerifyTrack *tracks;
	guint            n_points;
	AxisVerifyPoint  points[AXIS_VERIFY_MAX_POINTS];
	guint            timeout_ms;
} AxisVerifyPlan;

AxisVerifyPlan *
axis_verify_plan_new(void);

void
axis_verify_plan_free(AxisVerifyPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVerifyPlan, axis_verify_plan_free)

AxisVerifyPlan *
axis_verify_plan_copy(const AxisVerifyPlan *plan);

/* Appends a copied track (the plan's order is the order of calls). */
void
axis_verify_plan_add_track(
	AxisVerifyPlan *plan,
	gint            source,
	guint           output,
	gboolean        aac
);

/**
 * axis_verify_plan_make:
 * @plan: the conversion's plan (its first stream is the picture)
 * @duration_ms: the original's length, 0 or less when not known
 * @points: how many points to compare at (0: five)
 * @timeout_ms: each tool run's limit
 *
 * What checks 5 to 8 measure for @plan: its copied tracks (packets, not
 * attachments) in order, and @points windows of at most a second, evenly
 * spread, at the same instants the checks always used.  %NULL when @plan has
 * no streams.
 */
AxisVerifyPlan *
axis_verify_plan_make(
	const AxisConvertPlan *plan,
	gint64                 duration_ms,
	guint                  points,
	guint                  timeout_ms
);

/* Whether every field is within its bounds; @problem as the protocol's
 * readers say it. */
gboolean
axis_verify_plan_check(
	const AxisVerifyPlan  *plan,
	gchar                **problem
);

/* ── The evidence ───────────────────────────────────────────────── */

/**
 * AxisVerifyRun:
 * @status: how the tool's run ended
 * @code: its exit status, or the signal it died of
 * @error_bytes: how much it wrote on standard error (up to the runner's cap)
 * @said: (nullable): that, cleaned, at most AXIS_VERIFY_SAID_MAX_CHARS
 *   characters
 */
typedef struct
{
	AxisProbeRunStatus status;
	gint               code;
	guint64            error_bytes;
	gchar             *said;
} AxisVerifyRun;

/**
 * AxisVerifyCount:
 * @run: the ffprobe run
 * @read: its output was the packet list it writes
 * @count, @bytes: the picture's packets and their bytes, when @read
 */
typedef struct
{
	AxisVerifyRun run;
	gboolean      read;
	guint64       count;
	guint64       bytes;
} AxisVerifyCount;

/**
 * AxisVerifyPicture:
 * @run: the comparison's run
 * @read: its statistics were read and had frames to count
 * @lowest: the lowest of Y, U and V over the frames counted, when @read
 * @frames: how many frames were counted
 */
typedef struct
{
	AxisVerifyRun run;
	gboolean      read;
	gdouble       lowest;
	guint         frames;
} AxisVerifyPicture;

/**
 * AxisVerifyEvidence:
 * @output_digest, @output_sample, @source_sample: (nullable): a worker's
 *   digests of the converted file (whole, samples) and of the original's
 *   samples (storage/axis-file-check.h), which the daemon compares with its
 *   own; the daemon's own measurement leaves them unset
 * @decoded: check 5 was measured: @decode, and @output_hashes (the output's
 *   copied tracks' SHA-256 in the plan's order, %NULL when they could not
 *   be read)
 * @counted: check 6's packets were measured: @source_packets,
 *   @output_packets
 * @hashed: check 7's tracks were measured: @tracks, and @source_hashes
 *   (%NULL when they could not be read)
 * @n_points: how many points of check 8 were measured, in order
 */
typedef struct
{
	gchar             *output_digest;
	gchar             *output_sample;
	gchar             *source_sample;
	gboolean           decoded;
	AxisVerifyRun      decode;
	gchar            **output_hashes;
	gboolean           counted;
	AxisVerifyCount    source_packets;
	AxisVerifyCount    output_packets;
	gboolean           hashed;
	AxisVerifyRun      tracks;
	gchar            **source_hashes;
	guint              n_points;
	AxisVerifyPicture  points[AXIS_VERIFY_MAX_POINTS];
} AxisVerifyEvidence;

AxisVerifyEvidence *
axis_verify_evidence_new(void);

void
axis_verify_evidence_free(AxisVerifyEvidence *evidence);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVerifyEvidence, axis_verify_evidence_free)

void
axis_verify_run_clear(AxisVerifyRun *run);

/* Whether the run died of a signal nobody here sent it (as
 * axis_probe_run_crashed() says of the runner's result). */
gboolean
axis_verify_run_crashed(const AxisVerifyRun *run);

/* Whether a run's outcome is a measurement a worker may report: it ended by
 * itself or by the runner's limits (exit, a signal of the runner's limits,
 * a timeout, too much output); not a cancellation, a tool that could not
 * start or would not die, or a crash. */
gboolean
axis_verify_run_reportable(const AxisVerifyRun *run);

/* Whether every run the evidence holds is reportable, and it is complete
 * for @plan (every measurement made, every point). */
gboolean
axis_verify_evidence_complete(
	const AxisVerifyEvidence *evidence,
	const AxisVerifyPlan     *plan
);

/* Whether @evidence fits @plan: as many hashes as tracks where read, as many
 * points, figures within their bounds and within @output_size and
 * @source_size.  What the daemon requires of a worker's report. */
gboolean
axis_verify_evidence_fits(
	const AxisVerifyEvidence  *evidence,
	const AxisVerifyPlan      *plan,
	gint64                     output_size,
	gint64                     source_size,
	gchar                    **problem
);

/* ── Measuring ──────────────────────────────────────────────────── */

/**
 * AxisVerifyTools:
 * @ffmpeg, @ffprobe: the tools' commands, ended by %NULL
 * @scratch: the tools' working directory (a private directory)
 * @threads: each input's decoder threads and the filters' (0: ffmpeg's own
 *   choice)
 * @cancellable: (nullable): kills a tool when cancelled
 */
typedef struct
{
	const gchar *const *ffmpeg;
	const gchar *const *ffprobe;
	const gchar        *scratch;
	guint               threads;
	GCancellable       *cancellable;
} AxisVerifyTools;

/* The commands, ended by %NULL (tests, and the measurements below). */
gchar **
axis_verify_decode_argv(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan
);

gchar **
axis_verify_packets_argv(
	const AxisVerifyTools *tools,
	gint                   index
);

gchar **
axis_verify_tracks_argv(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan
);

gchar **
axis_verify_point_argv(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	guint                  k
);

/* Check 5's measurement: the full decode of @out_fd, its copied tracks
 * hashed in the same reading.  Fills @evidence's decode part. */
void
axis_verify_measure_decode(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   out_fd,
	AxisVerifyEvidence    *evidence
);

/* Check 6's: the picture's packets in both files. */
void
axis_verify_measure_packets(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   src_fd,
	gint                   out_fd,
	AxisVerifyEvidence    *evidence
);

/* Check 7's: the original's copied tracks hashed (nothing to run when the
 * plan copies none). */
void
axis_verify_measure_tracks(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   src_fd,
	AxisVerifyEvidence    *evidence
);

/* Check 8's point @k (the next one: @k must be evidence->n_points). */
void
axis_verify_measure_point(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	guint                  k,
	gint                   src_fd,
	gint                   out_fd,
	AxisVerifyEvidence    *evidence
);

/* ── Judging (the daemon) ───────────────────────────────────────── */

/* Check 5: a decode that ran to its end without an error.  *@detail is the
 * check's sentence. */
gboolean
axis_verify_judge_whole(
	const AxisVerifyEvidence  *evidence,
	gchar                    **detail
);

/* Check 6's packets: both counted, equal and not none.  On success
 * *@packets is the count; on failure *@detail is the check's sentence. */
gboolean
axis_verify_judge_packets(
	const AxisVerifyEvidence  *evidence,
	guint64                   *packets,
	gchar                    **detail
);

/* Check 7's tracks: every copied track's hash in the converted file equal
 * to the original's.  @plan is the conversion's, for the sentence (which
 * track, of which kind).  On failure *@detail is the check's sentence. */
gboolean
axis_verify_judge_tracks(
	const AxisVerifyEvidence  *evidence,
	const AxisConvertPlan     *plan,
	gchar                    **detail
);

/* Check 8: every point measured, its lowest similarity at least @floor_value.
 * *@detail is the check's sentence (with every point's figure). */
gboolean
axis_verify_judge_picture(
	const AxisVerifyEvidence  *evidence,
	guint                      n_points,
	gdouble                    floor_value,
	gchar                    **detail
);

/* Whether point @k's figures fail check 8: the daemon, measuring itself,
 * measures no further point after one that does. */
gboolean
axis_verify_point_fails(
	const AxisVerifyEvidence *evidence,
	guint                     k,
	gdouble                   floor_value
);

/* "ok", "exited", "signalled", "timeout", "too-much", "cancelled",
 * "spawn-failed", "unkillable", and back. */
const gchar *
axis_verify_status_to_string(AxisProbeRunStatus status);

gboolean
axis_verify_status_from_string(
	const gchar        *text,
	AxisProbeRunStatus *status
);

G_END_DECLS

#endif /* AXIS_VERIFY_H */
