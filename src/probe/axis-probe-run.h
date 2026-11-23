/*
 * axis-probe-run.h - running the probe tool as a contained subprocess
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ffprobe reads files an attacker may have made, so it is run as a child
 * that cannot do much and cannot be waited on forever:
 *
 *  - The command is an argv array and is never given to a shell.  The
 *    program is executed by the path given (nothing is searched for).
 *  - The environment is replaced by a minimal one (PATH, LC_ALL=C, no
 *    colour) and what the caller adds by name; nothing the daemon inherited
 *    (LD_PRELOAD, tokens, the database URL) reaches the child.
 *  - Standard input is /dev/null.  Standard output and error are pipes
 *    read here, with caps: output beyond the cap kills the child and the
 *    run reports it (memory is bounded by the caps).  Every other
 *    descriptor is closed in the child except the ones the caller gives:
 *    one to read, which the child sees as descriptor 3, and optionally one
 *    to write (an encoder's output file, opened by the caller), which it
 *    sees as descriptor 4.
 *  - The child is in a process group of its own, at the lowest CPU
 *    priority and idle I/O priority (the latter takes effect only under
 *    some I/O schedulers), with limits on address space, descriptors,
 *    core size and file size and a CPU limit as a last resort.  The file
 *    size limit is zero unless the caller names one (it then writes no
 *    file: a write to a file fails, only the pipes take data); a caller
 *    that gives an output descriptor gives the most it may write.  Its
 *    working directory is "/" unless the caller names one (the sample
 *    measurement gives ffmpeg a scratch directory, so that anything it
 *    might create lands there).
 *  - Standard output can be counted instead of kept (an encoder's output,
 *    whose size is all that matters); the cap then bounds the count.
 *  - A wall-clock timeout kills the child's whole group with SIGKILL; so
 *    does cancellation and an output cap breach.  After a kill, and after a
 *    normal exit, the group is killed once more before the child is
 *    reaped, so no descendant outlives the run.  The child is always
 *    reaped: one that does not die even after SIGKILL (stuck in the kernel
 *    on a failing disk) is handed to a thread that waits for it and the
 *    run reports %AXIS_PROBE_RUN_UNKILLABLE.  No descriptor is left open.
 *
 * What this does not do: it is not a sandbox.  The child can still open any
 * file the daemon's user can read, make network connections, and fork; the
 * probe layer restricts what ffprobe is asked to open (formats and
 * protocols), and a memory-safety bug in ffprobe is limited to its own
 * process and these limits.  Landlock or seccomp would add to this.
 *
 * Blocks: call from a worker thread.
 */

#ifndef AXIS_PROBE_RUN_H
#define AXIS_PROBE_RUN_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
	AXIS_PROBE_RUN_OK,		/* exited with status 0 */
	AXIS_PROBE_RUN_EXITED,		/* exited with another status */
	AXIS_PROBE_RUN_SIGNALLED,	/* died of a signal it was not sent by us */
	AXIS_PROBE_RUN_TIMEOUT,		/* killed: it did not finish in time */
	AXIS_PROBE_RUN_TOO_MUCH,	/* killed: it wrote too much */
	AXIS_PROBE_RUN_CANCELLED,	/* killed: the cancellable fired */
	AXIS_PROBE_RUN_SPAWN_FAILED,	/* the program could not be started */
	AXIS_PROBE_RUN_UNKILLABLE	/* survived SIGKILL; a thread waits for it */
} AxisProbeRunStatus;

/**
 * AxisProbeRunSpec:
 * @argv: the command, ended by %NULL; argv[0] is the program's path
 * @fd: an open descriptor for the child to read, as its descriptor 3, or
 *   -1 (the caller keeps ownership)
 * @timeout_ms: wall-clock limit; at least 100
 * @output_cap: the most standard output to accept, in bytes
 * @error_cap: the most standard error to keep (more is read and dropped)
 * @cancellable: (nullable): kills the child when cancelled
 * @memory_limit: address space limit in bytes, 0 for none
 * @discard_output: count standard output, do not keep it (@output_cap
 *   still applies, to the count)
 * @working_directory: (nullable): the child's working directory; %NULL is
 *   "/"
 * @cpu_seconds: the CPU time limit; 0 is the wall-clock limit plus ten
 *   seconds (enough for one thread; a tool that runs several needs more)
 * @output_fd: an open descriptor for the child to write, as its descriptor
 *   4 (the caller keeps ownership), or 0 for none: descriptors 0 to 2 are
 *   never passed, so 0 (what a zeroed spec holds) means none
 * @file_size_limit: the largest file the child may write, in bytes (its
 *   RLIMIT_FSIZE); 0 (the default) lets it write none
 * @output_func: (nullable): given each piece of standard output as it is
 *   read, while the child runs (a tool's progress lines), on the calling
 *   thread; only what is within @output_cap.  It must not block for long:
 *   the clock and the cancellable are looked at between pieces
 * @output_data: for @output_func
 * @environment: (nullable): more of the child's environment, "NAME=value"
 *   entries ended by %NULL, after the minimal one (a home directory of the
 *   caller's making, a proxy): the caller's own words, never something the
 *   daemon inherited.  Unlike the command, it is not in the process list
 */
typedef struct
{
	const gchar *const *argv;
	gint                fd;
	guint               timeout_ms;
	gsize               output_cap;
	gsize               error_cap;
	GCancellable       *cancellable;
	guint64             memory_limit;
	gboolean            discard_output;
	const gchar        *working_directory;
	guint               cpu_seconds;
	gint                output_fd;
	guint64             file_size_limit;
	void              (*output_func)(const guint8 *data, gsize length, gpointer user_data);
	gpointer            output_data;
	const gchar *const *environment;
} AxisProbeRunSpec;

typedef struct
{
	AxisProbeRunStatus  status;
	gint                exit_status;	/* the exit code, or the signal number */
	GByteArray         *output;	/* standard output (not NUL-terminated); empty when discarded */
	guint64             output_bytes;	/* how much standard output there was (up to the cap) */
	GByteArray         *errors;	/* standard error, the first error_cap bytes */
	gint64              elapsed_ms;
	GPid                pid;	/* 0 when none was started */
	gchar              *message;	/* spawn failures */
} AxisProbeRunResult;

/* Runs the command to its end; fills @result (always). */
void
axis_probe_run(
	const AxisProbeRunSpec *spec,
	AxisProbeRunResult     *result
);

void
axis_probe_run_result_clear(AxisProbeRunResult *result);

/* How many children have been started in this process (tests). */
guint64
axis_probe_run_get_started(void);

/* Children that survived SIGKILL and have not been reaped yet. */
guint
axis_probe_run_get_unreaped(void);

/* A sentence for a status ("it did not finish in time", ...). */
const gchar *
axis_probe_run_status_describe(AxisProbeRunStatus status);

/*
 * Whether the tool crashed: it died of a signal nobody here sent it, other
 * than the two its own limits send (SIGXFSZ when it would write past the
 * file size limit, which for an encoder means an output larger than its
 * source: the input's doing; SIGXCPU at the CPU limit, a timeout).  A
 * timeout, a cancellation and an output cap breach are kills of ours, and
 * an exit with a status, however bad, is the tool's own verdict on its
 * input: none of those is a crash.
 */
gboolean
axis_probe_run_crashed(const AxisProbeRunResult *result);

/* The signal's name without "SIG" ("SEGV"), or "unknown". */
const gchar *
axis_probe_run_signal_name(gint signal_number);

G_END_DECLS

#endif /* AXIS_PROBE_RUN_H */
