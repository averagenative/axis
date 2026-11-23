/*
 * axis-convert-caps.h - what this machine's ffmpeg can really do: measured
 * by test encodes, stored per ffmpeg build, measured again when the build
 * changes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * For each encoder of the table that `ffmpeg -encoders` lists, the check
 * makes plans with the builder itself (axis_convert_plan_build(), for a
 * profile of that codec and family) for one-second synthetic sources it
 * makes in a scratch directory, runs them as a conversion would run (the
 * source as descriptor 3, the output as descriptor 4, under the runner's
 * limits), reads each output with the probe's own calls and mapper
 * (axis_probe_describe_fd()) and compares it with the plan
 * (axis_convert_plan_check_output()):
 *
 *  - usable: an 8-bit BT.709 source comes out exactly as planned;
 *  - 10-bit (HEVC): a 10-bit HLG source (BT.2020, arib-std-b67) does;
 *  - HDR10 (HEVC): a 10-bit PQ source with mastering display and content
 *    light values (made with libx265; without it nothing can be shown, and
 *    no encoder is marked as carrying HDR10) comes back with the very same
 *    values, in the bitstream.
 *
 * Each step needs the one before.  Anything else (the encoder missing, the
 * run failing, a refusal, any difference) is "not capable", with the reason.
 * Nothing is written outside the scratch directory, which the caller makes
 * private and removes.
 *
 * The result is stored by machine and ffmpeg build (the SHA-256 of what
 * `ffmpeg -version` prints): axis_convert_capabilities_ensure() reads it
 * and measures again first when the build is not the one stored.  It does
 * not notice a changed driver or a GPU taken out: measure again for that.
 *
 * Blocks: call from a worker thread.
 */

#ifndef AXIS_CONVERT_CAPS_H
#define AXIS_CONVERT_CAPS_H

#include <gio/gio.h>
#include <glib.h>

#include "db/axis-db-pool.h"
#include "transcode/axis-convert.h"

G_BEGIN_DECLS

#define AXIS_CONVERT_ERROR (axis_convert_error_quark())

typedef enum
{
	AXIS_CONVERT_ERROR_TOOL,	/* ffmpeg or ffprobe cannot be run, or is not ffmpeg */
	AXIS_CONVERT_ERROR_SCRATCH,	/* the scratch directory cannot be used */
	AXIS_CONVERT_ERROR_CANCELLED
} AxisConvertError;

GQuark
axis_convert_error_quark(void);

/* The machine the daemon runs on (workers will have names of their own). */
#define AXIS_CONVERT_LOCAL_MACHINE "local"

/* The test picture, and what a test run may take and write. */
#define AXIS_CONVERT_CHECK_WIDTH       640
#define AXIS_CONVERT_CHECK_HEIGHT      360
#define AXIS_CONVERT_CHECK_TIMEOUT_MS  120000
#define AXIS_CONVERT_CHECK_FILE_LIMIT  (G_GUINT64_CONSTANT(256) * 1024 * 1024)
#define AXIS_CONVERT_CHECK_MEMORY      (G_GUINT64_CONSTANT(8) * 1024 * 1024 * 1024)

/**
 * AxisCapabilityTool:
 * @ffmpeg: ffmpeg, and any arguments before its own, ended by %NULL
 * @ffprobe: ffprobe likewise
 * @scratch: a private directory for the test files, made (and removed) by
 *   the caller; the check removes what it made in it
 * @timeout_ms: for each run; 0 is %AXIS_CONVERT_CHECK_TIMEOUT_MS
 * @cancellable: (nullable): stops the run in progress and the check
 * @width, @height: the test picture; 0 is 640x360
 * @threads: the software encoders' threads; 0 is 2
 * @record: (nullable): appended a line per run (the arguments, separated
 *   by spaces): for tests
 * @skip_families: AXIS_CONVERT_FAMILY_BIT() of each encoder family whose
 *   encoders are not to be run at all: they are left not measured, with
 *   the reason.  0 (what the daemon uses) runs every family.  For tests on
 *   a machine whose GPU is shared with other work.
 */
typedef struct
{
	const gchar *const *ffmpeg;
	const gchar *const *ffprobe;
	const gchar        *scratch;
	guint               timeout_ms;
	GCancellable       *cancellable;
	guint               width;
	guint               height;
	guint               threads;
	GString            *record;
	guint               skip_families;
} AxisCapabilityTool;

#define AXIS_CONVERT_FAMILY_BIT(family) (1u << (guint)(family))

/* The reason of an encoder whose family was skipped. */
#define AXIS_CONVERT_SKIPPED_REASON "not tried: the check was told to leave this family's hardware alone"

/**
 * axis_convert_tool_identity:
 * @version: (out) (transfer full): "8.1.3"
 * @digest: (out) (transfer full): the SHA-256 of `ffmpeg -version`'s output
 *
 * Fails with %AXIS_CONVERT_ERROR_TOOL when ffmpeg cannot be run or does not
 * say it is ffmpeg.
 */
gboolean
axis_convert_tool_identity(
	const AxisCapabilityTool  *tool,
	gchar                    **version,
	gchar                    **digest,
	GError                   **error
);

/* Measures every encoder of the table (see above).  Fails only when the
 * tool cannot be run at all, the scratch directory cannot be used, or the
 * check is cancelled; an encoder that does not work is a result. */
AxisEncoderCapabilities *
axis_convert_capabilities_measure(
	const AxisCapabilityTool  *tool,
	GError                   **error
);

/**
 * axis_convert_check_bitstream:
 * @plan: an HEVC plan
 * @output_fd: the output it produced (read from its start)
 *
 * Whether the picture's own bitstream carries the plan's HDR10 static
 * metadata, exactly.  A decoder of ffmpeg 7.1 or 8.1 hands a container's
 * mastering display and light data to the frames it decodes, so a probe of
 * the output cannot tell the bitstream's values from the container's;
 * here the picture is remuxed (stream copy, into the scratch directory) to
 * a raw HEVC stream, which has no container, and that is probed.  Nothing
 * to check (no values planned, or not HEVC) passes.
 *
 * Returns: (transfer full) (nullable): %NULL when it does, else why not
 */
gchar *
axis_convert_check_bitstream(
	const AxisCapabilityTool *tool,
	const AxisConvertPlan    *plan,
	gint                      output_fd
);

/* ── The store (encoder_tool, encoder_capability: migration 0016) ── */

/* The stored capabilities of @machine; *@caps is %NULL when there are none. */
gboolean
axis_convert_capabilities_load(
	AxisDbConn               *conn,
	const gchar              *machine,
	AxisEncoderCapabilities **caps,
	GError                  **error
);

/* Replaces what is stored for @machine with @caps, in one transaction. */
gboolean
axis_convert_capabilities_store(
	AxisDbConn                    *conn,
	const gchar                   *machine,
	const AxisEncoderCapabilities *caps,
	GError                       **error
);

/**
 * axis_convert_capabilities_ensure:
 * @measured: (out) (optional): whether a measurement was made now
 *
 * The capabilities of this machine's ffmpeg as it is now: what is stored
 * when it is for the same build (by digest), else a new measurement, which
 * is stored.  Plans are only to be built from what this returns.
 */
gboolean
axis_convert_capabilities_ensure(
	AxisDbPool                *pool,
	const gchar               *machine,
	const AxisCapabilityTool  *tool,
	AxisEncoderCapabilities  **caps,
	gboolean                  *measured,
	GError                   **error
);

G_END_DECLS

#endif /* AXIS_CONVERT_CAPS_H */
