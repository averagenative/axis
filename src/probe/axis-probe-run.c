/*
 * axis-probe-run.c - running the probe tool as a contained subprocess
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probe/axis-probe-run.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "library/axis-library-name.h"

/* The child's descriptors 3 (to read) and 4 (to write). */
#define CHILD_FD 3
#define CHILD_OUTPUT_FD 4

/* How long a killed child is waited for before it is given to a reaper
 * thread, and how long after its exit the pipes are drained. */
#define REAP_WAIT_MS   5000
#define DRAIN_AFTER_EXIT_MS 2000

#define READ_CHUNK 65536

#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif
#define IOPRIO_CLASS_IDLE_VALUE (3 << 13)

static gint started_total;	/* atomic */
static gint unreaped_total;	/* atomic */

typedef struct
{
	guint64 memory_limit;
	guint   cpu_seconds;
	guint64 file_size_limit;
	pid_t   parent;
} ChildLimits;

/* ── The child, between fork and exec ───────────────────────────── */

static void
set_limit(
	int     resource,
	guint64 value
){
	struct rlimit limit;

	limit.rlim_cur = (rlim_t)value;
	limit.rlim_max = (rlim_t)value;
	(void)setrlimit(resource, &limit);
}

/* Only calls that are safe between fork and exec. */
static void
child_setup(gpointer data)
{
	const ChildLimits *limits = data;

	/* A tool never outlives the process that runs it: if that process is
	 * killed (SIGKILL leaves it no say), the tool is killed too.  The
	 * signal follows the thread that forked, which waits in
	 * axis_probe_run() until the tool has exited.  If the parent died
	 * before this, the tool does not start. */
	(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (getppid() != limits->parent)
		_exit(127);
	(void)setpgid(0, 0);
	(void)setpriority(PRIO_PROCESS, 0, 19);
	(void)syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE_VALUE);

	/*
	 * No core dump of a tool that crashes on a hostile file: its memory
	 * holds pieces of the user's media, and a crash is something Axis
	 * records itself.  The limit is 1, not 0: where the system's crash
	 * handler is a pipe (systemd-coredump, the usual case), the kernel
	 * ignores a limit of 0 and runs the handler anyway, but treats 1 as
	 * "do not dump".  Where cores are plain files, 1 byte is as good as
	 * none.
	 */
	set_limit(RLIMIT_CORE, 1);
	set_limit(RLIMIT_FSIZE, limits->file_size_limit);
	set_limit(RLIMIT_NOFILE, 64);
	if (limits->memory_limit > 0)
		set_limit(RLIMIT_AS, limits->memory_limit);
	set_limit(RLIMIT_CPU, limits->cpu_seconds);
}

/* ── Waiting ────────────────────────────────────────────────────── */

typedef struct
{
	GPid pid;
} Reaper;

static gpointer
reaper_thread(gpointer data)
{
	Reaper *reaper = data;
	int status;

	while (waitpid(reaper->pid, &status, 0) < 0 && errno == EINTR)
		;
	g_atomic_int_add(&unreaped_total, -1);
	g_free(reaper);
	return NULL;
}

static void
kill_group(GPid pid)
{
	(void)kill(-pid, SIGKILL);
	(void)kill(pid, SIGKILL);
}

/* Whether the child has exited, without reaping it. */
static gboolean
has_exited(GPid pid)
{
	siginfo_t info;

	memset(&info, 0, sizeof(info));
	if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0)
		return errno == ECHILD;
	return info.si_pid != 0;
}

/* Reaps the child, waiting up to REAP_WAIT_MS; FALSE if it would not die. */
static gboolean
reap(
	GPid  pid,
	int  *wstatus
){
	gint64 deadline = g_get_monotonic_time() + REAP_WAIT_MS * 1000;

	for (;;) {
		pid_t r = waitpid(pid, wstatus, WNOHANG);

		if (r == pid)
			return TRUE;
		if (r < 0 && errno != EINTR) {
			/* Somebody else reaped it: its status is not known, which
			 * is neither an exit nor a signal. */
			*wstatus = -1;
			return TRUE;
		}
		if (g_get_monotonic_time() >= deadline)
			return FALSE;
		g_usleep(2000);
	}
}

/* Reads what is there, at most a few chunks (a child writing without end
 * must not keep the loop from looking at the clock); returns FALSE at the
 * end of the stream or on an error that ends it. */
static gboolean
drain(
	gint        *fd,
	GByteArray  *into,
	guint64     *count,
	gsize        cap,
	gboolean     is_output,
	gboolean     discard,
	gboolean    *too_much,
	guint8      *chunk,
	void       (*piece)(const guint8 *data, gsize length, gpointer user_data),
	gpointer     piece_data
){
	guint turns;

	for (turns = 0; turns < 16; turns++) {
		gssize n = read(*fd, chunk, READ_CHUNK);

		if (n > 0) {
			guint64 had = discard ? *count : into->len;
			gsize room = cap > had ? (gsize)(cap - had) : 0;

			if ((gsize)n > room) {
				/* Beyond the cap: standard output is a breach, standard
				 * error is simply dropped. */
				if (is_output)
					*too_much = TRUE;
				n = (gssize)room;
			}
			if (n > 0 && !discard)
				g_byte_array_append(into, chunk, (guint)n);
			if (n > 0 && piece != NULL)
				piece(chunk, (gsize)n, piece_data);
			if (count != NULL)
				*count += (guint64)n;
			if (*too_much)
				return TRUE;
			continue;
		}
		if (n == 0) {
			close(*fd);
			*fd = -1;
			return FALSE;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return TRUE;
		close(*fd);
		*fd = -1;
		return FALSE;
	}
	return TRUE;
}

static const gchar *const child_env[] = {
	"PATH=/usr/local/bin:/usr/bin:/bin",
	"LC_ALL=C",
	"AV_LOG_FORCE_NOCOLOR=1",
	NULL
};

void
axis_probe_run(
	const AxisProbeRunSpec *spec,
	AxisProbeRunResult     *result
){
	ChildLimits limits;
	GError *error = NULL;
	gint source_fds[2];
	gint target_fds[2];
	guint n_fds = 0;
	gint out_fd = -1;
	gint err_fd = -1;
	GPid pid = 0;
	gint64 started;
	gint64 deadline;
	gint64 exit_seen = 0;
	gboolean exited = FALSE;
	gboolean too_much = FALSE;
	AxisProbeRunStatus forced = AXIS_PROBE_RUN_OK;
	GPollFD cancel_poll;
	gboolean has_cancel_poll = FALSE;
	guint8 *chunk;
	int wstatus = 0;
	gboolean reaped;
	g_autofree const gchar **env = NULL;
	guint n_env = 0;
	guint e;

	g_return_if_fail(spec != NULL && spec->argv != NULL && spec->argv[0] != NULL);
	g_return_if_fail(result != NULL);

	memset(result, 0, sizeof(*result));
	result->output = g_byte_array_new();
	result->errors = g_byte_array_new();

	limits.memory_limit = spec->memory_limit;
	limits.cpu_seconds = spec->cpu_seconds > 0 ? spec->cpu_seconds : spec->timeout_ms / 1000 + 10;
	limits.file_size_limit = spec->file_size_limit;
	limits.parent = getpid();

	if (spec->fd >= 0) {
		source_fds[n_fds] = spec->fd;
		target_fds[n_fds] = CHILD_FD;
		n_fds++;
	}
	/* 0 to 2 are the standard streams, never an output: 0 is "none". */
	if (spec->output_fd > 2) {
		source_fds[n_fds] = spec->output_fd;
		target_fds[n_fds] = CHILD_OUTPUT_FD;
		n_fds++;
	}
	/* The minimal environment, then what the caller names. */
	for (e = 0; spec->environment != NULL && spec->environment[e] != NULL; e++)
		;
	env = g_new0(const gchar *, G_N_ELEMENTS(child_env) + e);
	for (e = 0; child_env[e] != NULL; e++)
		env[n_env++] = child_env[e];
	for (e = 0; spec->environment != NULL && spec->environment[e] != NULL; e++)
		env[n_env++] = spec->environment[e];
	started = g_get_monotonic_time();

	if (!g_spawn_async_with_pipes_and_fds(spec->working_directory != NULL ? spec->working_directory : "/",
	                                      (const gchar * const *)spec->argv, env,
	                                      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES |
	                                      G_SPAWN_STDIN_FROM_DEV_NULL,
	                                      child_setup, &limits, -1, -1, -1,
	                                      n_fds > 0 ? source_fds : NULL,
	                                      n_fds > 0 ? target_fds : NULL,
	                                      n_fds, &pid, NULL, &out_fd, &err_fd, &error)) {
		result->status = AXIS_PROBE_RUN_SPAWN_FAILED;
		result->message = axis_library_text_clean(error->message, 200);
		g_error_free(error);
		return;
	}
	result->pid = pid;
	g_atomic_int_inc(&started_total);

	(void)fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL) | O_NONBLOCK);
	(void)fcntl(err_fd, F_SETFL, fcntl(err_fd, F_GETFL) | O_NONBLOCK);
	if (spec->cancellable != NULL)
		has_cancel_poll = g_cancellable_make_pollfd(spec->cancellable, &cancel_poll);

	chunk = g_malloc(READ_CHUNK);
	deadline = started + (gint64)MAX(spec->timeout_ms, 100u) * 1000;

	for (;;) {
		GPollFD fds[3];
		guint n = 0;
		guint out_slot = 0;
		guint err_slot = 0;
		guint cancel_slot = 0;
		gint64 now = g_get_monotonic_time();
		gint wait_ms;

		if (!exited && has_exited(pid)) {
			exited = TRUE;
			exit_seen = now;
			/* Anything it left behind goes with it, so that the pipes
			 * close. */
			kill_group(pid);
		}
		if (too_much) {
			forced = AXIS_PROBE_RUN_TOO_MUCH;
			break;
		}
		if (!exited && now >= deadline) {
			forced = AXIS_PROBE_RUN_TIMEOUT;
			break;
		}
		if (!exited && spec->cancellable != NULL && g_cancellable_is_cancelled(spec->cancellable)) {
			forced = AXIS_PROBE_RUN_CANCELLED;
			break;
		}
		if (exited && out_fd < 0 && err_fd < 0)
			break;
		if (exited && now - exit_seen > DRAIN_AFTER_EXIT_MS * 1000)
			break;

		if (out_fd >= 0) {
			out_slot = n;
			fds[n].fd = out_fd;
			fds[n].events = G_IO_IN | G_IO_HUP | G_IO_ERR;
			fds[n].revents = 0;
			n++;
		}
		if (err_fd >= 0) {
			err_slot = n;
			fds[n].fd = err_fd;
			fds[n].events = G_IO_IN | G_IO_HUP | G_IO_ERR;
			fds[n].revents = 0;
			n++;
		}
		if (has_cancel_poll) {
			cancel_slot = n;
			fds[n] = cancel_poll;
			fds[n].revents = 0;
			n++;
		}

		if (out_fd < 0 && err_fd < 0)
			wait_ms = 3;
		else if (exited)
			wait_ms = 20;
		else
			wait_ms = (gint)MIN((deadline - now) / 1000 + 1, (gint64)100);
		if (wait_ms < 1)
			wait_ms = 1;
		g_poll(fds, n, wait_ms);

		if (out_fd >= 0 && (out_slot < n) && fds[out_slot].revents != 0)
			drain(&out_fd, result->output, &result->output_bytes, spec->output_cap, TRUE, spec->discard_output,
				&too_much, chunk, spec->output_func, spec->output_data);
		if (err_fd >= 0 && (err_slot < n) && fds[err_slot].revents != 0)
			drain(&err_fd, result->errors, NULL, spec->error_cap, FALSE, FALSE, &too_much, chunk, NULL, NULL);
		(void)cancel_slot;
	}

	/* Whatever way out of the loop: the group goes, once more, while the
	 * leader is still unreaped (so its process group id cannot have been
	 * handed to anything else), then the leader is reaped. */
	kill_group(pid);
	if (out_fd >= 0)
		close(out_fd);
	if (err_fd >= 0)
		close(err_fd);
	out_fd = err_fd = -1;
	reaped = reap(pid, &wstatus);
	if (!reaped) {
		Reaper *reaper = g_new0(Reaper, 1);

		reaper->pid = pid;
		g_atomic_int_inc(&unreaped_total);
		g_thread_unref(g_thread_new("axis-probe-reap", reaper_thread, reaper));
		forced = AXIS_PROBE_RUN_UNKILLABLE;
	}

	if (has_cancel_poll)
		g_cancellable_release_fd(spec->cancellable);
	g_free(chunk);

	result->elapsed_ms = (g_get_monotonic_time() - started) / 1000;
	if (forced != AXIS_PROBE_RUN_OK) {
		result->status = forced;
		result->exit_status = reaped && WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0;
	} else if (WIFEXITED(wstatus)) {
		result->exit_status = WEXITSTATUS(wstatus);
		result->status = result->exit_status == 0 ? AXIS_PROBE_RUN_OK : AXIS_PROBE_RUN_EXITED;
	} else if (WIFSIGNALED(wstatus)) {
		result->exit_status = WTERMSIG(wstatus);
		result->status = AXIS_PROBE_RUN_SIGNALLED;
	} else {
		result->status = AXIS_PROBE_RUN_EXITED;
		result->exit_status = -1;
	}
}

void
axis_probe_run_result_clear(AxisProbeRunResult *result)
{
	if (result == NULL)
		return;

	if (result->output != NULL)
		g_byte_array_unref(result->output);
	if (result->errors != NULL)
		g_byte_array_unref(result->errors);
	g_free(result->message);
	memset(result, 0, sizeof(*result));
}

guint64
axis_probe_run_get_started(void)
{
	return (guint64)(guint)g_atomic_int_get(&started_total);
}

guint
axis_probe_run_get_unreaped(void)
{
	return (guint)g_atomic_int_get(&unreaped_total);
}

const gchar *
axis_probe_run_status_describe(AxisProbeRunStatus status)
{
	switch (status) {
	case AXIS_PROBE_RUN_OK:
		return "finished";
	case AXIS_PROBE_RUN_EXITED:
		return "failed";
	case AXIS_PROBE_RUN_SIGNALLED:
		return "crashed";
	case AXIS_PROBE_RUN_TIMEOUT:
		return "did not finish in time and was stopped";
	case AXIS_PROBE_RUN_TOO_MUCH:
		return "wrote more output than is accepted and was stopped";
	case AXIS_PROBE_RUN_CANCELLED:
		return "was stopped";
	case AXIS_PROBE_RUN_SPAWN_FAILED:
		return "could not be started";
	case AXIS_PROBE_RUN_UNKILLABLE:
		return "could not be stopped";
	default:
		return "failed";
	}
}

gboolean
axis_probe_run_crashed(const AxisProbeRunResult *result)
{
	g_return_val_if_fail(result != NULL, FALSE);

	return result->status == AXIS_PROBE_RUN_SIGNALLED && result->exit_status != SIGXFSZ &&
		result->exit_status != SIGXCPU;
}

const gchar *
axis_probe_run_signal_name(gint signal_number)
{
	switch (signal_number) {
	case SIGHUP:
		return "HUP";
	case SIGINT:
		return "INT";
	case SIGQUIT:
		return "QUIT";
	case SIGILL:
		return "ILL";
	case SIGTRAP:
		return "TRAP";
	case SIGABRT:
		return "ABRT";
	case SIGBUS:
		return "BUS";
	case SIGFPE:
		return "FPE";
	case SIGKILL:
		return "KILL";
	case SIGSEGV:
		return "SEGV";
	case SIGPIPE:
		return "PIPE";
	case SIGALRM:
		return "ALRM";
	case SIGTERM:
		return "TERM";
	case SIGXCPU:
		return "XCPU";
	case SIGXFSZ:
		return "XFSZ";
	case SIGSYS:
		return "SYS";
	default:
		return "unknown";
	}
}
