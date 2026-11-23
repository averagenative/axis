/*
 * axis-worker-proto.h - the worker API's messages, read strictly and
 * written exactly, by both sides
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every request is a POST of one JSON object to AXIS_WORKER_API_PREFIX
 * followed by its name (register, claim, heartbeat, complete, fail), and
 * every answer is one JSON object.  Each side treats what the other sends
 * as outside input: the daemon parses requests from a machine that may be
 * faulty or hostile, and the worker parses answers from a daemon it may
 * have been pointed at by mistake.  So the readers here, for every message
 * of both directions:
 *
 *  - take at most AXIS_WORKER_MAX_REQUEST_BYTES (requests) or
 *    AXIS_WORKER_MAX_RESPONSE_BYTES (answers) of strict JSON (RFC 8259,
 *    through probe/axis-probe-json.h, which bounds depth, values and
 *    string lengths);
 *  - refuse a member they do not know, a member given twice, and a value
 *    of another JSON type than the one named (a number in a string is not
 *    a number; 1.0 is a whole number);
 *  - check every number against its range, every text against its length
 *    and its alphabet, every list against its length, and every value
 *    against the others where they depend on each other;
 *  - say why they refused in a short sentence that never quotes the
 *    input.
 *
 * What the parsers hand out has passed all of that, and every free text in
 * it is cleaned (valid UTF-8, no control or direction characters).  The
 * writers produce exactly what the readers accept.  GLib only, no I/O:
 * tests/fuzz/fuzz-worker-request.c and fuzz-worker-response.c read
 * arbitrary bytes through every reader.
 *
 * The bounds, request by request, are in design.md of the
 * add-worker-protocol change and in the parsers' comments below.
 *
 * Versions.  Every message carries "protocol", and a reader takes only its
 * own version: since unknown members are refused, any new member is a
 * change both sides must make together.  Protocol 2 (add-remote-worker-
 * mounts) added: per volume of register, whether the worker's mount keeps
 * file identities; in a conversion's description, the digest of samples of
 * the source as the daemon read it; in claim, a worker's report that it is
 * paused after its encoder crashed, and in claim's answer, whether the
 * daemon holds it paused.  Protocol 3 (verify-on-worker) added the kind
 * convert.verify: a converted file's checks measured on a worker, its
 * description and its evidence.  An error answer has the same shape in every
 * version, so the worker reads one of any version up to its own (to show
 * what an older daemon said), and the daemon answers a request of another
 * version in that version when it can write it (axis_worker_error_to_json_
 * version()), with the word "malformed", which every version knows.
 */

#ifndef AXIS_WORKER_PROTO_H
#define AXIS_WORKER_PROTO_H

#include <glib.h>

#include "transcode/axis-convert.h"
#include "transcode/axis-estimate-target.h"
#include "transcode/axis-verify.h"

G_BEGIN_DECLS

#define AXIS_WORKER_PROTOCOL            3
#define AXIS_WORKER_API_PREFIX          "/api/v1/worker/"

/* The oldest version whose error answers this side reads. */
#define AXIS_WORKER_PROTOCOL_OLDEST     1

/* The kept capability measurement's own format number: a file, not a
 * message, unchanged since protocol 1 (so that a new axis-worker does not
 * measure again only because the protocol moved). */
#define AXIS_WORKER_TOOL_FORMAT         1

/* The largest request body the daemon reads, and the largest answer the
 * worker reads. */
#define AXIS_WORKER_MAX_REQUEST_BYTES   (256 * 1024)
#define AXIS_WORKER_MAX_RESPONSE_BYTES  (1024 * 1024)

/* The kinds of remote work, and the queue they are claimed from. */
#define AXIS_WORKER_KIND_CAPABILITIES   "worker.capabilities"
#define AXIS_WORKER_KIND_SAMPLE         "estimate.sample.remote"
#define AXIS_WORKER_KIND_CONVERT        "convert.encode"
#define AXIS_WORKER_KIND_VERIFY         "convert.verify"
#define AXIS_WORKER_QUEUE               "remote"

/* Names and texts. */
#define AXIS_WORKER_NAME_MAX            48	/* [a-z][a-z0-9-]*, the worker's name */
#define AXIS_WORKER_KIND_MAX            63	/* [a-z][a-z0-9._-]* */
#define AXIS_WORKER_VERSION_MAX         32	/* [0-9A-Za-z.+~_-]: axis-worker's version */
#define AXIS_WORKER_TOOL_VERSION_MAX    63	/* the same alphabet: ffmpeg's version word */
#define AXIS_WORKER_MESSAGE_MAX_CHARS   200	/* a progress message */
#define AXIS_WORKER_ERROR_MAX_CHARS     500	/* a failure's text */
#define AXIS_WORKER_REASON_MAX_CHARS    480	/* an encoder's reason */
#define AXIS_WORKER_ANSWER_MAX_CHARS    200	/* an error answer's message */
#define AXIS_WORKER_PAUSE_MAX_CHARS     200	/* why a worker says it is paused */
#define AXIS_WORKER_PAUSED_MAX_CHARS    480	/* why the daemon holds it paused */

/* Lists. */
#define AXIS_WORKER_MAX_VOLUMES         64
#define AXIS_WORKER_MAX_KINDS           16
#define AXIS_WORKER_MAX_JOBS            16

/* Numbers. */
#define AXIS_WORKER_MAX_ID              G_GINT64_CONSTANT(9007199254740991)
#define AXIS_WORKER_MIN_LEASE_MS        1000
#define AXIS_WORKER_MAX_LEASE_MS        3600000
#define AXIS_WORKER_MIN_HEARTBEAT_MS    100
#define AXIS_WORKER_MIN_POLL_MS         100
#define AXIS_WORKER_MAX_POLL_MS         600000
#define AXIS_WORKER_MAX_ATTEMPT         100
#define AXIS_WORKER_MAX_TIME_S          G_GINT64_CONSTANT(253402300799)

/* The sample measurement's description and result. */
#define AXIS_SAMPLE_MAX_FILES           128
#define AXIS_SAMPLE_MAX_EXCERPTS        3
#define AXIS_SAMPLE_MIN_EXCERPT_MS      500
#define AXIS_SAMPLE_MAX_EXCERPT_MS      120000
#define AXIS_SAMPLE_MAX_START_MS        G_GINT64_CONSTANT(1000000000)
#define AXIS_SAMPLE_MIN_TIMEOUT_MS      1000
#define AXIS_SAMPLE_MAX_TIMEOUT_MS      21600000
#define AXIS_SAMPLE_MAX_FILE_BYTES      G_GINT64_CONSTANT(4503599627370496)	/* 4 PiB */
#define AXIS_SAMPLE_MAX_STREAM          1023
#define AXIS_SAMPLE_MAX_AUDIO_BITRATE   1000000
#define AXIS_SAMPLE_MAX_CHANNELS        64
#define AXIS_SAMPLE_MAX_SAMPLE_RATE     768000
#define AXIS_SAMPLE_MAX_SAMPLE_BITS     64

/* A conversion's description. */
#define AXIS_CONVERT_MAX_ARGS           2048
#define AXIS_CONVERT_MAX_ARG_BYTES      512
#define AXIS_CONVERT_MIN_TIMEOUT_MS     60000
#define AXIS_CONVERT_MAX_TIMEOUT_MS     604800000	/* a week */
#define AXIS_CONVERT_MAX_DURATION_MS    G_GINT64_CONSTANT(31536000000)	/* a year */
#define AXIS_CONVERT_MAX_INODE          G_MAXINT64

/* ── Texts both sides check ─────────────────────────────────────── */

/* [a-z][a-z0-9-]{0,47} */
gboolean
axis_worker_name_is_valid(const gchar *name);

/* [a-z][a-z0-9._-]{0,62} */
gboolean
axis_worker_kind_is_valid(const gchar *kind);

/* 1 to @max of [0-9A-Za-z.+~_-] */
gboolean
axis_worker_version_is_valid(
	const gchar *version,
	gsize        max
);

/* 64 lower-case hexadecimal digits */
gboolean
axis_worker_digest_is_valid(const gchar *digest);

/* A lower-case UUID in the 8-4-4-4-12 layout, not all zeros (a volume's,
 * or a lease token). */
gboolean
axis_worker_uuid_is_valid(const gchar *uuid);

/* ── Capabilities (register, and worker.capabilities' result) ──── */

/*
 * {"measured_at": seconds, "aac": bool, "mov_text": bool, "subrip": bool,
 *  "encoders": [{"name": one of the table's six, "measured": bool,
 *                "usable": bool, "ten_bit": bool, "hdr10": bool,
 *                "reason": null | 1..480 characters}]}
 *
 * At most one entry per encoder; an encoder not listed is not measured.
 * usable needs measured, ten_bit needs usable and an encoder the table asks
 * 10 bits of, hdr10 needs ten_bit.  The tool's version and digest come
 * from the enclosing message (ffmpeg_version, ffmpeg_digest).
 */

/*
 * A measurement on its own, {"protocol": 1, "ffmpeg_version": ...,
 * "ffmpeg_digest": ..., "capabilities": {...}}: what axis-worker keeps in
 * its state directory, so that it measures again only when its ffmpeg
 * changes.
 */
gchar *
axis_worker_tool_to_json(const AxisEncoderCapabilities *caps);

AxisEncoderCapabilities *
axis_worker_tool_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* ── register ───────────────────────────────────────────────────── */

/**
 * AxisWorkerVolumeSeen:
 * @uuid: the UUID the worker read from the marker
 * @path: where it read it: an absolute, normalised path of at most
 *   AXIS_STORAGE_PATH_MAX bytes (shown to administrators; never used to
 *   open anything by the daemon)
 * @keeps_identity: whether the worker's mount of it shows the server's
 *   inode numbers and times to the nanosecond (its configuration says so:
 *   FALSE for a path listed in volumes_without_file_identity).  Shown to
 *   administrators; the daemon checks nothing less for it.
 */
typedef struct
{
	gchar    *uuid;
	gchar    *path;
	gboolean  keeps_identity;
} AxisWorkerVolumeSeen;

/**
 * AxisWorkerRegister:
 * @worker_version: axis-worker's version
 * @max_jobs: how many jobs it runs at once, 1 to AXIS_WORKER_MAX_JOBS
 * @caps: what its ffmpeg was shown to do (tool_version and tool_digest
 *   set)
 * @volumes: (element-type AxisWorkerVolumeSeen): 0 to
 *   AXIS_WORKER_MAX_VOLUMES, distinct UUIDs
 *
 * {"protocol": 2, "worker_version": ..., "max_jobs": n,
 *  "ffmpeg_version": ..., "ffmpeg_digest": ..., "capabilities": {...},
 *  "volumes": [{"uuid": ..., "path": ..., "keeps_file_identity": bool}]}
 */
typedef struct
{
	gchar                   *worker_version;
	guint                    max_jobs;
	AxisEncoderCapabilities *caps;
	GPtrArray               *volumes;
} AxisWorkerRegister;

AxisWorkerRegister *
axis_worker_register_new(void);

void
axis_worker_register_free(AxisWorkerRegister *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerRegister, axis_worker_register_free)

gchar *
axis_worker_register_to_json(const AxisWorkerRegister *self);

AxisWorkerRegister *
axis_worker_register_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisWorkerVolumeAnswer:
 * @uuid: as reported
 * @accepted: the daemon has a volume of that UUID
 * @reason: (nullable): why not, a short sentence
 */
typedef struct
{
	gchar    *uuid;
	gboolean  accepted;
	gchar    *reason;
} AxisWorkerVolumeAnswer;

/**
 * AxisWorkerRegistered:
 * @worker: the worker's name, as the daemon knows it
 * @lease_ms: how long a lease lasts from the moment it was granted or
 *   renewed, AXIS_WORKER_MIN_LEASE_MS to AXIS_WORKER_MAX_LEASE_MS
 * @heartbeat_ms: how often to heartbeat: AXIS_WORKER_MIN_HEARTBEAT_MS to
 *   a third of @lease_ms
 * @poll_ms: how long to wait after a claim that found nothing
 * @volumes: (element-type AxisWorkerVolumeAnswer): one per volume reported
 *
 * {"protocol": 2, "worker": ..., "lease_ms": n, "heartbeat_ms": n,
 *  "poll_ms": n, "volumes": [{"uuid": ..., "accepted": bool,
 *  "reason": null | text}]}
 */
typedef struct
{
	gchar     *worker;
	guint      lease_ms;
	guint      heartbeat_ms;
	guint      poll_ms;
	GPtrArray *volumes;
} AxisWorkerRegistered;

AxisWorkerRegistered *
axis_worker_registered_new(void);

void
axis_worker_registered_free(AxisWorkerRegistered *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerRegistered, axis_worker_registered_free)

/* Adds a volume to the answer (copies). */
void
axis_worker_registered_add_volume(
	AxisWorkerRegistered *self,
	const gchar          *uuid,
	gboolean              accepted,
	const gchar          *reason
);

gchar *
axis_worker_registered_to_json(const AxisWorkerRegistered *self);

AxisWorkerRegistered *
axis_worker_registered_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* ── claim ──────────────────────────────────────────────────────── */

/**
 * AxisWorkerClaim:
 * @kinds: the kinds of work the worker can run: 1 to
 *   AXIS_WORKER_MAX_KINDS distinct kind names
 * @volumes: the volumes it has proved just now: 0 to
 *   AXIS_WORKER_MAX_VOLUMES distinct UUIDs
 * @paused_job: 0, or the job whose encoder crashed on this worker: it is
 *   paused since, takes no work, and says so in every claim until a claim's
 *   answer says the daemon no longer holds it paused (a person resumed it)
 * @paused_reason: (nullable): with @paused_job, what happened, 1 to
 *   AXIS_WORKER_PAUSE_MAX_CHARS characters
 *
 * {"protocol": 2, "kinds": [...], "volumes": [...]} and, while paused,
 * "paused": {"job": n, "reason": text}
 */
typedef struct
{
	gchar  **kinds;
	gchar  **volumes;
	gint64   paused_job;
	gchar   *paused_reason;
} AxisWorkerClaim;

void
axis_worker_claim_free(AxisWorkerClaim *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerClaim, axis_worker_claim_free)

gchar *
axis_worker_claim_to_json(const AxisWorkerClaim *self);

AxisWorkerClaim *
axis_worker_claim_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisSampleFilePlan:
 * @index: its place in the description (0 for the first)
 * @volume: the volume's UUID
 * @root: the root's directory on the volume, a normalised relative path
 *   ("" for the volume's top)
 * @path: the file's path relative to the root, in the index's stored form
 * @size: what the file must measure, 0 to AXIS_SAMPLE_MAX_FILE_BYTES
 * @mtime_sec, @mtime_nsec: its modification time
 * @stream: the stream to encode, 0 to AXIS_SAMPLE_MAX_STREAM
 * @quality: the quality level, AXIS_QUALITY_MIN to AXIS_QUALITY_MAX
 * @bit_depth: of the picture: 8 or 10 (8 for H.264); 0 for audio
 * @channels, @sample_rate, @sample_bits: of an audio stream (0 for a
 *   picture, or not known)
 * @n_excerpts: 1 to AXIS_SAMPLE_MAX_EXCERPTS
 * @start_ms: where each excerpt starts, 0 to AXIS_SAMPLE_MAX_START_MS
 * @length_ms: how long each is, AXIS_SAMPLE_MIN_EXCERPT_MS to
 *   AXIS_SAMPLE_MAX_EXCERPT_MS
 */
typedef struct
{
	guint   index;
	gchar  *volume;
	gchar  *root;
	gchar  *path;
	gint64  size;
	gint64  mtime_sec;
	gint    mtime_nsec;
	gint    stream;
	gint    quality;
	gint    bit_depth;
	gint    channels;
	gint    sample_rate;
	gint    sample_bits;
	guint   n_excerpts;
	gint64  start_ms[AXIS_SAMPLE_MAX_EXCERPTS];
	gint64  length_ms;
} AxisSampleFilePlan;

/**
 * AxisSampleDescription:
 * @media: video or audio; @codec must be of it
 * @codec: the output codec
 * @family: the encoder family
 * @target_quality: the target's own level (FLAC's compression level)
 * @audio_bitrate, @audio_bitrate_mono: bit/s, 0 to
 *   AXIS_SAMPLE_MAX_AUDIO_BITRATE
 * @audio_quality: an encoder quality (MP3 V0 is 0) or -1, up to 9
 * @reduce_to_cd: FLAC: reduce what is above 16 bits or 48 kHz
 * @excerpt_timeout_ms: each excerpt's limit
 * @files: (element-type AxisSampleFilePlan): 1 to AXIS_SAMPLE_MAX_FILES,
 *   in index order
 *
 * The "description" of an estimate.sample.remote job:
 * {"media": "video"|"audio", "codec": ..., "family": ...,
 *  "target_quality": n, "audio_bitrate": n, "audio_bitrate_mono": n,
 *  "audio_quality": n, "reduce_to_cd": bool, "excerpt_timeout_ms": n,
 *  "files": [{"index": n, "volume": uuid, "root": ..., "path": ...,
 *    "size": n, "mtime_sec": n, "mtime_nsec": n, "stream": n,
 *    "quality": n, "bit_depth": n, "channels": n, "sample_rate": n,
 *    "sample_bits": n, "length_ms": n, "starts_ms": [n, ...]}]}
 * A path is never absolute: every file is a volume's UUID and paths
 * relative to it.
 */
typedef struct
{
	AxisTargetMedia    media;
	AxisTargetCodec    codec;
	AxisEncoderFamily  family;
	gint               target_quality;
	gint               audio_bitrate;
	gint               audio_bitrate_mono;
	gint               audio_quality;
	gboolean           reduce_to_cd;
	guint              excerpt_timeout_ms;
	GPtrArray         *files;
} AxisSampleDescription;

AxisSampleDescription *
axis_sample_description_new(void);

void
axis_sample_description_free(AxisSampleDescription *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisSampleDescription, axis_sample_description_free)

/* Appends a copy of @file (its index is set to its place). */
void
axis_sample_description_add_file(
	AxisSampleDescription    *self,
	const AxisSampleFilePlan *file
);

/* Whether every field is within its bounds and they agree (what a parser
 * would accept); @problem as for the parsers. */
gboolean
axis_sample_description_check(
	const AxisSampleDescription  *self,
	gchar                       **problem
);

/* The description as the JSON object a claim's answer carries, and back:
 * the daemon keeps it in the job's payload so that what it plans is what it
 * sends and what it checks results against. */
gchar *
axis_sample_description_to_json(const AxisSampleDescription *self);

AxisSampleDescription *
axis_sample_description_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/*
 * What the measurement's command (axis_estimate_sample_argv()) needs, from
 * a checked description and one of its files: @target and @stream are
 * zeroed and then filled with the description's enumerations and numbers
 * only.
 */
void
axis_sample_description_to_target(
	const AxisSampleDescription *self,
	const AxisSampleFilePlan    *file,
	AxisEstimateTarget          *target,
	AxisMediaStream             *stream
);

/* ── A conversion (convert.encode) ──────────────────────────────── */

/* Whether @name is an attempt's output name in a work area:
 * "<1 to 19 digits>-<1 to 3 digits>.tmp", no leading zero in either
 * number. */
gboolean
axis_convert_output_name_valid(const gchar *name);

/**
 * AxisConvertWork:
 * @volume: the volume's UUID
 * @root: the root's directory on the volume, a normalised relative path
 * @path: the file's path relative to the root, in the index's stored form
 * @size, @mtime_sec, @mtime_nsec, @inode: what the source must be (@inode
 *   as the index stores it, the same bits as st_ino)
 * @source_sample: the digest of samples of the source's content
 *   (storage/axis-file-check.h), read by the daemon from its own view of the
 *   disk when it handed the work out: 64 lower-case hexadecimal digits
 * @output: the name the output must have in the volume's work area
 *   (axis_convert_output_name_valid())
 * @args: ffmpeg's arguments, without the program, ended by %NULL: 1 to
 *   AXIS_CONVERT_MAX_ARGS arguments of 1 to AXIS_CONVERT_MAX_ARG_BYTES
 *   bytes, valid UTF-8 without control characters (the worker also checks
 *   them with axis_convert_args_valid())
 * @source_streams: how many streams the source has (1 to
 *   AXIS_PROBE_MAX_STREAMS; their indices are 0 to this less one)
 * @duration_ms: the source's duration, for progress; 0 when not known
 * @timeout_ms: the encode's time limit
 *
 * The "description" of a convert.encode job:
 * {"volume": uuid, "root": ..., "path": ..., "size": n, "mtime_sec": n,
 *  "mtime_nsec": n, "inode": n, "source_sample": hex, "output": name,
 *  "args": [...], "source_streams": n, "duration_ms": n, "timeout_ms": n}
 */
typedef struct
{
	gchar   *volume;
	gchar   *root;
	gchar   *path;
	gint64   size;
	gint64   mtime_sec;
	gint     mtime_nsec;
	gint64   inode;
	gchar   *source_sample;
	gchar   *output;
	gchar  **args;
	guint    source_streams;
	gint64   duration_ms;
	guint    timeout_ms;
} AxisConvertWork;

void
axis_convert_work_free(AxisConvertWork *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConvertWork, axis_convert_work_free)

/* Whether every field is within its bounds (what the parser accepts). */
gboolean
axis_convert_work_check(
	const AxisConvertWork  *self,
	gchar                 **problem
);

/**
 * AxisConvertResult:
 * @output: the output's name, as given
 * @size: its size, 1 to AXIS_SAMPLE_MAX_FILE_BYTES
 *
 * A convert.encode job's result: {"output": name, "size": n}
 */
typedef struct
{
	gchar  *output;
	gint64  size;
} AxisConvertResult;

void
axis_convert_result_free(AxisConvertResult *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConvertResult, axis_convert_result_free)

/* ── A converted file's checks (convert.verify) ─────────────────── */

/*
 * The plan of what to measure (transcode/axis-verify.h), as the description
 * carries it and the daemon keeps it with the conversion:
 * {"picture": {"source": n, "output": n},
 *  "tracks": [{"source": n, "output": n, "aac": bool}, ...],
 *  "points": [{"start_ms": n, "length_ms": n}, ...], "timeout_ms": n}
 * 0 to AXIS_VERIFY_MAX_TRACKS tracks (distinct output positions), 0 to
 * AXIS_VERIFY_MAX_POINTS points (axis_verify_plan_check()).
 */
gchar *
axis_verify_plan_to_json(const AxisVerifyPlan *plan);

AxisVerifyPlan *
axis_verify_plan_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisVerifyWork:
 * @volume, @root, @path, @size, @mtime_sec, @mtime_nsec, @inode,
 *   @source_sample: the original, as for a conversion's description
 * @output: the converted file's name in the volume's work area
 *   (axis_convert_output_name_valid())
 * @output_size: its size as the daemon recorded it, 1 to
 *   AXIS_SAMPLE_MAX_FILE_BYTES
 * @plan: what to measure
 *
 * The "description" of a convert.verify job:
 * {"volume": uuid, "root": ..., "path": ..., "size": n, "mtime_sec": n,
 *  "mtime_nsec": n, "inode": n, "source_sample": hex, "output": name,
 *  "output_size": n, "plan": {...}}
 */
typedef struct
{
	gchar          *volume;
	gchar          *root;
	gchar          *path;
	gint64          size;
	gint64          mtime_sec;
	gint            mtime_nsec;
	gint64          inode;
	gchar          *source_sample;
	gchar          *output;
	gint64          output_size;
	AxisVerifyPlan *plan;
} AxisVerifyWork;

void
axis_verify_work_free(AxisVerifyWork *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVerifyWork, axis_verify_work_free)

gboolean
axis_verify_work_check(
	const AxisVerifyWork  *self,
	gchar                **problem
);

/**
 * AxisVerifyResult:
 * @output, @output_size: the converted file measured, as described
 * @evidence: the measurements, complete (axis_verify_evidence_complete()),
 *   with the three digests set
 *
 * A convert.verify job's result:
 * {"output": name, "output_size": n, "output_digest": hex,
 *  "output_sample": hex, "source_sample": hex,
 *  "decode": {"run": RUN, "hashes": [hex, ...] | null},
 *  "packets": {"source": COUNT, "output": COUNT},
 *  "tracks": {"run": RUN, "hashes": [hex, ...] | null},
 *  "points": [{"run": RUN, "lowest": x | null, "frames": n}, ...]}
 * RUN = {"status": "ok" | "exited" | "signalled" | "timeout" | "too-much",
 *        "code": n, "error_bytes": n, "said": null | text}
 * COUNT = {"run": RUN, "count": n | null, "bytes": n | null}
 * A run that was cancelled, could not start or crashed is not a
 * measurement and cannot be written here.  No verdict.
 */
typedef struct
{
	gchar              *output;
	gint64              output_size;
	AxisVerifyEvidence *evidence;
} AxisVerifyResult;

void
axis_verify_result_free(AxisVerifyResult *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVerifyResult, axis_verify_result_free)

/* The result as the daemon keeps it with the conversion, and back (as a
 * complete request carries it). */
gchar *
axis_verify_result_to_json(const AxisVerifyResult *self);

AxisVerifyResult *
axis_verify_result_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisWorkerJob:
 * @id: the job, 1 to AXIS_WORKER_MAX_ID
 * @kind: one of the remote kinds
 * @lease_token: the lease, a UUID
 * @lease_ms, @heartbeat_ms: as in AxisWorkerRegistered
 * @attempt: which attempt this is, 1 to AXIS_WORKER_MAX_ATTEMPT
 * @sample: (nullable): for AXIS_WORKER_KIND_SAMPLE, its description
 * @convert: (nullable): for AXIS_WORKER_KIND_CONVERT, its description; the
 *   capabilities kind's description is {}
 * @verify: (nullable): for AXIS_WORKER_KIND_VERIFY, its description
 *
 * The answer to claim: {"protocol": 2, "job": null, "paused": null} or
 * {"protocol": 2, "job": {"id": n, "kind": ..., "lease_token": ...,
 *  "lease_ms": n, "heartbeat_ms": n, "attempt": n, "description": {...}},
 *  "paused": null}; while the daemon holds the worker paused, "job" is null
 *  and "paused" says why (1 to AXIS_WORKER_PAUSED_MAX_CHARS characters).
 */
typedef struct
{
	gint64                 id;
	gchar                 *kind;
	gchar                 *lease_token;
	guint                  lease_ms;
	guint                  heartbeat_ms;
	guint                  attempt;
	AxisSampleDescription *sample;
	AxisConvertWork       *convert;
	AxisVerifyWork        *verify;
} AxisWorkerJob;

void
axis_worker_job_free(AxisWorkerJob *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerJob, axis_worker_job_free)

/* @job: (nullable): %NULL for "nothing to do"; @paused: (nullable): why the
 * daemon holds the worker paused (then @job must be %NULL) */
gchar *
axis_worker_claimed_to_json(
	const AxisWorkerJob *job,
	const gchar         *paused
);

/* *@job is %NULL (and TRUE is returned) for "nothing to do"; *@paused
 * (optional) is the daemon's reason when it holds the worker paused, else
 * %NULL. */
gboolean
axis_worker_claimed_parse(
	const gchar    *data,
	gsize           length,
	AxisWorkerJob **job,
	gchar         **paused,
	gchar         **problem
);

/* ── heartbeat ──────────────────────────────────────────────────── */

/**
 * AxisWorkerHeartbeat:
 * @job: the job
 * @lease_token: its lease
 * @has_progress: whether @progress is given
 * @progress: 0 to 1
 * @message: (nullable): where it is, at most AXIS_WORKER_MESSAGE_MAX_CHARS
 *
 * {"protocol": 2, "job": n, "lease_token": ..., "progress": x,
 *  "message": text}; progress and message are optional
 */
typedef struct
{
	gint64    job;
	gchar    *lease_token;
	gboolean  has_progress;
	gdouble   progress;
	gchar    *message;
} AxisWorkerHeartbeat;

void
axis_worker_heartbeat_free(AxisWorkerHeartbeat *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerHeartbeat, axis_worker_heartbeat_free)

gchar *
axis_worker_heartbeat_to_json(const AxisWorkerHeartbeat *self);

AxisWorkerHeartbeat *
axis_worker_heartbeat_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisWorkerBeat:
 * @stop: stop the job now
 * @lease_ms: with "continue", the renewed lease
 * @reason: (nullable): with "stop": "cancelled" or "lease"
 *
 * {"protocol": 2, "action": "continue", "lease_ms": n} or
 * {"protocol": 2, "action": "stop", "reason": "cancelled"|"lease"}
 */
typedef struct
{
	gboolean  stop;
	guint     lease_ms;
	gchar    *reason;
} AxisWorkerBeat;

void
axis_worker_beat_free(AxisWorkerBeat *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerBeat, axis_worker_beat_free)

gchar *
axis_worker_beat_to_json(const AxisWorkerBeat *self);

AxisWorkerBeat *
axis_worker_beat_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* ── complete ───────────────────────────────────────────────────── */

/* Why a file of a measurement was not measured: one of these words. */
#define AXIS_SAMPLE_SKIP_CHANGED            "changed"
#define AXIS_SAMPLE_SKIP_MISSING            "missing"
#define AXIS_SAMPLE_SKIP_NOT_CONTAINED      "not-contained"
#define AXIS_SAMPLE_SKIP_UNREADABLE         "unreadable"
#define AXIS_SAMPLE_SKIP_ENCODER_FAILED     "encoder-failed"
#define AXIS_SAMPLE_SKIP_VOLUME             "volume-unavailable"
#define AXIS_SAMPLE_SKIP_PATH_REFUSED       "path-refused"

gboolean
axis_sample_skip_reason_is_valid(const gchar *reason);

/**
 * AxisSampleFileResult:
 * @index: the file's index in the description
 * @measured: whether its excerpts were encoded
 * @n_bytes: measured: one count per excerpt of the description
 * @bytes: what each excerpt's encoder wrote, 0 to AXIS_SAMPLE_OUTPUT_CAP
 * @reason: (nullable): not measured: one of the AXIS_SAMPLE_SKIP_* words
 */
typedef struct
{
	guint     index;
	gboolean  measured;
	guint     n_bytes;
	guint64   bytes[AXIS_SAMPLE_MAX_EXCERPTS];
	gchar    *reason;
} AxisSampleFileResult;

/**
 * AxisSampleResult:
 * @files: (element-type AxisSampleFileResult): 0 to
 *   AXIS_SAMPLE_MAX_FILES, each index at most once
 *
 * {"files": [{"index": n, "bytes": [n, ...]} |
 *            {"index": n, "skipped": word}]}
 */
typedef struct
{
	GPtrArray *files;
} AxisSampleResult;

AxisSampleResult *
axis_sample_result_new(void);

void
axis_sample_result_free(AxisSampleResult *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisSampleResult, axis_sample_result_free)

/* Appends a measured file (@n_bytes counts) or, with @reason, a skipped
 * one. */
void
axis_sample_result_add(
	AxisSampleResult *self,
	guint             index,
	const guint64    *bytes,
	guint             n_bytes,
	const gchar      *reason
);

/**
 * AxisWorkerComplete:
 * @job: the job
 * @lease_token: its lease
 * @kind: the job's kind, which decides what @result is
 * @sample: (nullable): for AXIS_WORKER_KIND_SAMPLE
 * @caps: (nullable): for AXIS_WORKER_KIND_CAPABILITIES (tool_version and
 *   tool_digest set)
 * @convert: (nullable): for AXIS_WORKER_KIND_CONVERT
 * @verify: (nullable): for AXIS_WORKER_KIND_VERIFY
 *
 * {"protocol": 3, "job": n, "lease_token": ..., "kind": ...,
 *  "result": {...}}, where the capabilities result is
 *  {"ffmpeg_version": ..., "ffmpeg_digest": ..., "capabilities": {...}}
 */
typedef struct
{
	gint64                   job;
	gchar                   *lease_token;
	gchar                   *kind;
	AxisSampleResult        *sample;
	AxisEncoderCapabilities *caps;
	AxisConvertResult       *convert;
	AxisVerifyResult        *verify;
} AxisWorkerComplete;

void
axis_worker_complete_free(AxisWorkerComplete *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerComplete, axis_worker_complete_free)

gchar *
axis_worker_complete_to_json(const AxisWorkerComplete *self);

AxisWorkerComplete *
axis_worker_complete_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisWorkerCompleted:
 * @recorded: results the daemon kept
 * @refused: results it did not (implausible, or about a file that changed)
 *
 * {"protocol": 2, "recorded": n, "refused": n}
 */
typedef struct
{
	guint recorded;
	guint refused;
} AxisWorkerCompleted;

gchar *
axis_worker_completed_to_json(const AxisWorkerCompleted *self);

gboolean
axis_worker_completed_parse(
	const gchar          *data,
	gsize                 length,
	AxisWorkerCompleted  *out,
	gchar               **problem
);

/* ── fail ───────────────────────────────────────────────────────── */

/**
 * AxisWorkerFail:
 * @job: the job
 * @lease_token: its lease
 * @error: what went wrong, 1 to AXIS_WORKER_ERROR_MAX_CHARS
 * @retry: whether trying again makes sense
 * @stopping: the worker is stopping: the job did not fail, it is given
 *   back (implies @retry)
 *
 * {"protocol": 2, "job": n, "lease_token": ..., "error": text,
 *  "retry": bool, "stopping": bool}
 */
typedef struct
{
	gint64    job;
	gchar    *lease_token;
	gchar    *error;
	gboolean  retry;
	gboolean  stopping;
} AxisWorkerFail;

void
axis_worker_fail_free(AxisWorkerFail *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerFail, axis_worker_fail_free)

gchar *
axis_worker_fail_to_json(const AxisWorkerFail *self);

AxisWorkerFail *
axis_worker_fail_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* {"protocol": 2, "state": "queued"|"failed"|"cancelled"} */
gchar *
axis_worker_failed_to_json(const gchar *state);

gboolean
axis_worker_failed_parse(
	const gchar  *data,
	gsize         length,
	gchar       **state,
	gchar       **problem
);

/* ── Errors ─────────────────────────────────────────────────────── */

/*
 * Every refusal: {"protocol": 2, "error": word, "message": text}, the word
 * one of "malformed", "unauthorized", "throttled", "forbidden",
 * "not-found", "method", "content-type", "too-large", "lease",
 * "unregistered" (claim before register, since the daemon started),
 * "busy", "internal".
 */
gchar *
axis_worker_error_to_json(
	const gchar *word,
	const gchar *message
);

/* The same in @version (AXIS_WORKER_PROTOCOL_OLDEST to AXIS_WORKER_PROTOCOL;
 * another is written as this one's): the answer to a request of that
 * version, so that the program that sent it can read why it was refused. */
gchar *
axis_worker_error_to_json_version(
	guint        version,
	const gchar *word,
	const gchar *message
);

/* Reads an error answer of any version from AXIS_WORKER_PROTOCOL_OLDEST to
 * this one; *@version (optional) is the one it was written in. */
gboolean
axis_worker_error_parse(
	const gchar  *data,
	gsize         length,
	gchar       **word,
	gchar       **message,
	guint        *version,
	gchar       **problem
);

/**
 * axis_worker_message_protocol:
 *
 * The "protocol" a message says it is in, read before anything else of it
 * (strict JSON within the request bound; a whole number from 0 to 1000 as a
 * member of the top object): what the daemon looks at first, to answer a
 * worker of another version in a way it understands.  FALSE when the body
 * says no version.
 */
gboolean
axis_worker_message_protocol(
	const gchar *data,
	gsize        length,
	guint       *version
);

/* The sentence for a request of @theirs refused by a daemon of this
 * version (it names both, and what to do). */
gchar *
axis_worker_protocol_refusal(guint theirs);

G_END_DECLS

#endif /* AXIS_WORKER_PROTO_H */
