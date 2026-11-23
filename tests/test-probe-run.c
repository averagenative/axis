/*
 * test-probe-run.c - the subprocess runner: what the child inherits, how
 * it is limited, and that it is always stopped and reaped
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The child is tests/fixtures/probe/fake-tool.sh, which stands in for
 * ffprobe and misbehaves in the way each test asks for.  No database.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-storage.h"
#include "probe/axis-probe-run.h"

#define TOOL "tests/fixtures/probe/fake-tool.sh"

static gchar *
limit_of(
	const gchar *limits,
	const gchar *name
);

/* ── Helpers ────────────────────────────────────────────────────── */

typedef struct
{
	gchar *dir;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	fx->dir = axis_test_storage_dir_new();
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_storage_dir_free(fx->dir);
}

/* Runs the fake tool in @mode with @arg (may be NULL). */
static void
run_tool(
	AxisProbeRunResult *result,
	const gchar        *mode,
	const gchar        *arg,
	gint                fd,
	guint               timeout_ms,
	gsize               output_cap,
	GCancellable       *cancellable
){
	const gchar *argv[5];
	AxisProbeRunSpec spec;
	/* The child runs in "/": the script is named by its absolute path. */
	g_autofree gchar *tool = g_canonicalize_filename(TOOL, NULL);

	argv[0] = "/bin/sh";
	argv[1] = tool;
	argv[2] = mode;
	argv[3] = arg;
	argv[4] = NULL;
	memset(&spec, 0, sizeof(spec));
	spec.argv = argv;
	spec.fd = fd;
	spec.timeout_ms = timeout_ms;
	spec.output_cap = output_cap;
	spec.error_cap = 4096;
	spec.cancellable = cancellable;
	spec.memory_limit = G_GUINT64_CONSTANT(1073741824);
	axis_probe_run(&spec, result);
}

static gchar *
output_text(const AxisProbeRunResult *result)
{
	return g_strndup((const gchar *)result->output->data, result->output->len);
}

/* Whether a process is gone, or only a zombie waiting for its parent. */
static gboolean
gone_or_zombie(GPid pid)
{
	g_autofree gchar *path = g_strdup_printf("/proc/%d/stat", (int)pid);
	g_autofree gchar *text = NULL;
	const gchar *close_paren;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return TRUE;
	close_paren = strrchr(text, ')');
	/* 'Z' is a zombie; 'X' is a dead task the kernel is in the middle of
	 * releasing, which is what a zombie shows for an instant as it is
	 * reaped. */
	return close_paren != NULL && close_paren[1] == ' ' &&
	       (close_paren[2] == 'Z' || close_paren[2] == 'X');
}

static void
wait_until_gone(GPid pid)
{
	gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

	/* One look decides: looking again after the loop could catch the
	 * process in another state than the one that ended the loop. */
	for (;;) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;

		if (gone_or_zombie(pid))
			return;
		if (g_get_monotonic_time() < deadline) {
			g_usleep(10000);
			continue;
		}

		/* Say what is still there: its state and process group. */
		path = g_strdup_printf("/proc/%d/stat", (int)pid);
		g_file_get_contents(path, &text, NULL, NULL);
		g_error("process %d is still alive: %s", (int)pid, text != NULL ? text : "(gone just now)");
	}
}

/* The child has been reaped: it is not even a zombie. */
static void
assert_reaped(GPid pid)
{
	int status;

	g_assert_cmpint(waitpid(pid, &status, WNOHANG), ==, -1);
	g_assert_cmpint(errno, ==, ECHILD);
	g_assert_cmpint(kill(pid, 0), ==, -1);
	g_assert_cmpint(errno, ==, ESRCH);
}

/* Whether any process of group @pgid is alive (a zombie waiting for its
 * parent, init for an orphan, is not). */
static gboolean
group_has_live_member(GPid pgid)
{
	GDir *dir = g_dir_open("/proc", 0, NULL);
	const gchar *name;
	gboolean live = FALSE;

	g_assert_nonnull(dir);
	while (!live && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		g_auto(GStrv) fields = NULL;
		const gchar *close_paren;

		if (!g_ascii_isdigit(name[0]))
			continue;
		path = g_strdup_printf("/proc/%s/stat", name);
		if (!g_file_get_contents(path, &text, NULL, NULL))
			continue;
		close_paren = strrchr(text, ')');
		if (close_paren == NULL)
			continue;
		fields = g_strsplit(close_paren + 2, " ", 5);
		/* Neither a zombie nor a dead task being released ('X'). */
		if (g_strv_length(fields) >= 3 && fields[0][0] != 'Z' && fields[0][0] != 'X' &&
		    g_ascii_strtoll(fields[2], NULL, 10) == pgid)
			live = TRUE;
	}
	g_dir_close(dir);
	return live;
}

static void
assert_group_gone(GPid pid)
{
	gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

	/* No member of the child's process group is left running. */
	gboolean live;

	/* No member of the child's process group is left running.  One look
	 * decides, as in wait_until_gone(). */
	while ((live = group_has_live_member(pid)) && g_get_monotonic_time() < deadline)
		g_usleep(10000);
	g_assert_false(live);
}

/* ── What it runs ───────────────────────────────────────────────── */

static void
test_output_and_status(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;

	run_tool(&result, "echo", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	err = g_strndup((const gchar *)result.errors->data, result.errors->len);
	g_assert_cmpstr(out, ==, "out line\n");
	g_assert_cmpstr(err, ==, "err line\n");
	assert_reaped(result.pid);
	axis_probe_run_result_clear(&result);

	run_tool(&result, "exit3", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_EXITED);
	g_assert_cmpint(result.exit_status, ==, 3);
	g_assert_cmpuint(result.output->len, >, 0);
	assert_reaped(result.pid);
	axis_probe_run_result_clear(&result);

	run_tool(&result, "segv", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_SIGNALLED);
	g_assert_cmpint(result.exit_status, ==, SIGSEGV);
	assert_reaped(result.pid);
	axis_probe_run_result_clear(&result);
}

/* Spec (worker-protocol): a crash is a death by a signal nobody here sent,
 * other than the two the runner's own limits send; an exit with a status,
 * a timeout and a cancellation are not crashes. */
static void
test_crash_or_not(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *mode;
		gint         signal_number;
		gboolean     crash;
	} cases[] = {
		{ "segv", SIGSEGV, TRUE },
		{ "abrt", SIGABRT, TRUE },
		{ "kill", SIGKILL, TRUE },
		{ "xfsz", SIGXFSZ, FALSE },
		{ "xcpu", SIGXCPU, FALSE },
	};
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	AxisProbeRunResult result;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		run_tool(&result, cases[i].mode, NULL, -1, 10000, 65536, NULL);
		g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_SIGNALLED);
		g_assert_cmpint(result.exit_status, ==, cases[i].signal_number);
		if (axis_probe_run_crashed(&result) != cases[i].crash)
			g_error("%s: crashed is %d", cases[i].mode, axis_probe_run_crashed(&result));
		axis_probe_run_result_clear(&result);
	}
	g_assert_cmpstr(axis_probe_run_signal_name(SIGSEGV), ==, "SEGV");
	g_assert_cmpstr(axis_probe_run_signal_name(SIGKILL), ==, "KILL");
	g_assert_cmpstr(axis_probe_run_signal_name(0), ==, "unknown");

	/* Its own verdict, a timeout, a cancellation. */
	run_tool(&result, "exit3", NULL, -1, 10000, 65536, NULL);
	g_assert_false(axis_probe_run_crashed(&result));
	axis_probe_run_result_clear(&result);
	run_tool(&result, "hang", NULL, -1, 300, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_TIMEOUT);
	g_assert_false(axis_probe_run_crashed(&result));
	axis_probe_run_result_clear(&result);
	g_cancellable_cancel(cancel);
	run_tool(&result, "hang", NULL, -1, 10000, 65536, cancel);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_CANCELLED);
	g_assert_false(axis_probe_run_crashed(&result));
	axis_probe_run_result_clear(&result);
}

static void
test_big_output_below_the_cap(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;

	run_tool(&result, "big", NULL, -1, 30000, 4 * 1024 * 1024, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	g_assert_cmpuint(result.output->len, >, 1000000);
	axis_probe_run_result_clear(&result);
}

static void
test_spawn_failure(Fixture *fx, gconstpointer data)
{
	const gchar *argv[2];
	AxisProbeRunSpec spec;
	AxisProbeRunResult result;
	guint fds = axis_test_storage_count_fds();

	argv[0] = "/nonexistent/ffprobe";
	argv[1] = NULL;
	memset(&spec, 0, sizeof(spec));
	spec.argv = argv;
	spec.fd = -1;
	spec.timeout_ms = 5000;
	spec.output_cap = 1024;
	spec.error_cap = 1024;
	axis_probe_run(&spec, &result);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_SPAWN_FAILED);
	g_assert_nonnull(result.message);
	g_assert_cmpint(result.pid, ==, 0);
	axis_probe_run_result_clear(&result);
	g_assert_cmpuint(axis_test_storage_count_fds(), ==, fds);
}

/* ── What the child gets and does not get ───────────────────────── */

static void
test_descriptor_three(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fx->dir, "media.bin", NULL);
	AxisProbeRunResult result;
	g_autofree gchar *out = NULL;
	gint fd;

	axis_test_storage_write(path, "the file the tool may read");
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);

	run_tool(&result, "fd3", NULL, fd, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	g_assert_cmpstr(out, ==, "the file the tool may read");
	axis_probe_run_result_clear(&result);
	/* The caller's descriptor is still the caller's. */
	g_assert_cmpint(fcntl(fd, F_GETFD), >=, 0);
	close(fd);
}

/* A shell script in the fixture's directory, run with an input and an
 * output descriptor and a file size limit. */
static void
run_script(
	Fixture            *fx,
	AxisProbeRunResult *result,
	const gchar        *body,
	gint                in_fd,
	gint                out_fd,
	guint64             file_size_limit
){
	g_autofree gchar *path = g_build_filename(fx->dir, "script.sh", NULL);
	const gchar *argv[3];
	AxisProbeRunSpec spec;

	axis_test_storage_write(path, body);
	argv[0] = "/bin/sh";
	argv[1] = path;
	argv[2] = NULL;
	memset(&spec, 0, sizeof(spec));
	spec.argv = argv;
	spec.fd = in_fd;
	spec.output_fd = out_fd;
	spec.file_size_limit = file_size_limit;
	spec.timeout_ms = 10000;
	spec.output_cap = 65536;
	spec.error_cap = 4096;
	spec.memory_limit = G_GUINT64_CONSTANT(1073741824);
	axis_probe_run(&spec, result);
}

/* An output descriptor is the child's 4, writable up to the file size
 * limit the caller gives; without one the limit stays zero and nothing
 * reaches a file; 0 means no descriptor at all. */
static void
test_descriptor_four(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *in_path = g_build_filename(fx->dir, "in.bin", NULL);
	g_autofree gchar *out_path = g_build_filename(fx->dir, "out.bin", NULL);
	g_autofree gchar *content = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *value = NULL;
	AxisProbeRunResult result;
	gint in_fd;
	gint out_fd;

	axis_test_storage_write(in_path, "read through three, written through four");
	in_fd = open(in_path, O_RDONLY | O_CLOEXEC);
	out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	g_assert_cmpint(in_fd, >=, 0);
	g_assert_cmpint(out_fd, >=, 0);

	/* Read from 3, write to 4, under a limit of 1 MiB. */
	run_script(fx, &result, "cat <&3 >&4 || exit 9\ncat /proc/self/limits\n", in_fd, out_fd, 1048576);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	value = limit_of(out, "Max file size");
	g_assert_cmpstr(value, ==, "1048576 1048576");
	axis_probe_run_result_clear(&result);
	content = axis_test_storage_read(out_path);
	g_assert_cmpstr(content, ==, "read through three, written through four");
	/* The caller's descriptors are still the caller's. */
	g_assert_cmpint(fcntl(in_fd, F_GETFD), >=, 0);
	g_assert_cmpint(fcntl(out_fd, F_GETFD), >=, 0);

	/* Beyond the limit the write is refused (the child is stopped by
	 * SIGXFSZ or sees the error): no more than the limit lands. */
	g_assert_cmpint(ftruncate(out_fd, 0), ==, 0);
	g_assert_cmpint(lseek(out_fd, 0, SEEK_SET), ==, 0);
	run_script(fx, &result, "head -c 100000 /dev/zero >&4 && exit 0\nexit 7\n", -1, out_fd, 1000);
	g_assert_cmpint(result.status, !=, AXIS_PROBE_RUN_OK);
	axis_probe_run_result_clear(&result);
	{
		struct stat st;

		g_assert_cmpint(fstat(out_fd, &st), ==, 0);
		g_assert_cmpint(st.st_size, <=, 1000);
	}

	/* With no limit given, not a byte. */
	g_assert_cmpint(ftruncate(out_fd, 0), ==, 0);
	g_assert_cmpint(lseek(out_fd, 0, SEEK_SET), ==, 0);
	run_script(fx, &result, "printf x >&4 && exit 0\nexit 7\n", -1, out_fd, 0);
	g_assert_cmpint(result.status, !=, AXIS_PROBE_RUN_OK);
	axis_probe_run_result_clear(&result);
	{
		struct stat st;

		g_assert_cmpint(fstat(out_fd, &st), ==, 0);
		g_assert_cmpint(st.st_size, ==, 0);
	}

	/* 0 (a zeroed spec) is no descriptor: the child has no 4. */
	run_script(fx, &result, "if { true >&4; } 2>/dev/null; then echo open; else echo closed; fi\n", -1, 0, 0);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	g_free(out);
	out = output_text(&result);
	g_assert_cmpstr(out, ==, "closed\n");
	axis_probe_run_result_clear(&result);
	close(in_fd);
	close(out_fd);
}

static void
test_stdin_is_empty(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	g_autofree gchar *out = NULL;

	run_tool(&result, "stdin", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	g_assert_cmpstr(out, ==, "stdin was empty\n");
	axis_probe_run_result_clear(&result);
}

/* Spec (design): no inherited descriptors beyond what is needed.  A
 * descriptor open without close-on-exec in the parent is closed in the
 * child. */
static void
test_no_inherited_descriptors(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	g_autofree gchar *number = NULL;
	g_autofree gchar *out = NULL;
	gint leaked = open("/dev/null", O_RDONLY);	/* no O_CLOEXEC */

	g_assert_cmpint(leaked, >=, 0);
	number = g_strdup_printf("%d", leaked);
	run_tool(&result, "fdopen", number, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	g_assert_cmpstr(out, ==, "closed\n");
	axis_probe_run_result_clear(&result);
	close(leaked);
}

static void
test_environment_is_replaced(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	g_autofree gchar *out = NULL;

	g_setenv("AXIS_TEST_SECRET", "hunter2", TRUE);
	g_setenv("LD_PRELOAD", "/nonexistent.so", TRUE);
	run_tool(&result, "env", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	g_assert_null(strstr(out, "hunter2"));
	g_assert_null(strstr(out, "LD_PRELOAD"));
	g_assert_nonnull(strstr(out, "LC_ALL=C"));
	g_assert_null(strstr(out, "HOME="));
	axis_probe_run_result_clear(&result);

	/* What the caller names is added to the minimal one, and nothing else. */
	{
		static const gchar *const more[] = { "HOME=/nonexistent/made-up", "AXIS_TEST_NAMED=a value", NULL };
		const gchar *argv[2];
		AxisProbeRunSpec spec;
		g_autofree gchar *named = NULL;

		argv[0] = "/usr/bin/env";
		argv[1] = NULL;
		memset(&spec, 0, sizeof(spec));
		spec.argv = argv;
		spec.fd = -1;
		spec.timeout_ms = 10000;
		spec.output_cap = 65536;
		spec.error_cap = 4096;
		spec.environment = more;
		axis_probe_run(&spec, &result);
		g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
		named = output_text(&result);
		g_assert_nonnull(strstr(named, "HOME=/nonexistent/made-up\n"));
		g_assert_nonnull(strstr(named, "AXIS_TEST_NAMED=a value\n"));
		g_assert_nonnull(strstr(named, "LC_ALL=C"));
		g_assert_null(strstr(named, "hunter2"));
		g_assert_null(strstr(named, "LD_PRELOAD"));
		axis_probe_run_result_clear(&result);
	}
	g_unsetenv("AXIS_TEST_SECRET");
	g_unsetenv("LD_PRELOAD");
}

/* The soft and hard values of a line of /proc/PID/limits, "a b". */
static gchar *
limit_of(
	const gchar *limits,
	const gchar *name
){
	const gchar *line = strstr(limits, name);
	g_auto(GStrv) fields = NULL;
	g_autofree gchar *rest = NULL;
	g_autofree gchar *eol = NULL;
	const gchar *newline;

	g_assert_nonnull(line);
	line += strlen(name);
	newline = strchr(line, '\n');
	rest = newline != NULL ? g_strndup(line, (gsize)(newline - line)) : g_strdup(line);
	fields = g_strsplit_set(rest, " \t", -1);
	{
		GString *values = g_string_new(NULL);
		guint i;

		for (i = 0; fields[i] != NULL && values->len < 64; i++) {
			if (fields[i][0] == '\0')
				continue;
			if (g_ascii_isdigit(fields[i][0]) || strcmp(fields[i], "unlimited") == 0)
				g_string_append_printf(values, "%s%s", values->len > 0 ? " " : "", fields[i]);
		}
		return g_string_free(values, FALSE);
	}
}

/* Whether this platform keeps a limit on the address space where
 * /proc/PID/limits shows it.  Under user-mode emulation of another
 * architecture (an image built for arm64 on an amd64 machine) the emulator
 * takes that limit for itself and the kernel never sees it, so the file says
 * "unlimited"; on a real machine of either architecture it is kept.  A child
 * sets the limit on itself and reads its own file, with nothing allocated in
 * between. */
static gboolean
address_space_limit_is_kept(void)
{
	pid_t pid = fork();
	int status = 0;

	g_assert_cmpint(pid, >=, 0);
	if (pid == 0) {
		struct rlimit limit = { 1073741824, 1073741824 };
		char text[4096];
		char *line;
		char *end;
		ssize_t n;
		int fd;

		if (setrlimit(RLIMIT_AS, &limit) != 0)
			_exit(0);
		fd = open("/proc/self/limits", O_RDONLY);
		if (fd < 0)
			_exit(0);
		n = read(fd, text, sizeof text - 1);
		if (n <= 0)
			_exit(0);
		text[n] = '\0';
		line = strstr(text, "Max address space");
		if (line == NULL)
			_exit(0);
		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';
		_exit(strstr(line, "1073741824") == NULL ? 1 : 0);
	}
	g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
	return !(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

static void
test_limits_and_priority(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	g_autofree gchar *out = NULL;
	g_autofree gchar *pid_text = NULL;
	g_autofree gchar *value = NULL;
	const gchar *close_paren;
	gchar **fields;
	const gchar *pid_line;

	run_tool(&result, "limits", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	out = output_text(&result);
	value = limit_of(out, "Max address space");
	if (address_space_limit_is_kept())
		g_assert_cmpstr(value, ==, "1073741824 1073741824");
	else
		g_test_message("the address-space limit is not checked: this platform does not show one "
			"(user-mode emulation); it said: %s", value);
	g_free(value);
	/* 1, not 0: see child_setup() in src/probe/axis-probe-run.c. */
	value = limit_of(out, "Max core file size");
	g_assert_cmpstr(value, ==, "1 1");
	g_free(value);
	value = limit_of(out, "Max file size");
	g_assert_cmpstr(value, ==, "0 0");
	g_free(value);
	value = limit_of(out, "Max open files");
	g_assert_cmpstr(value, ==, "64 64");
	axis_probe_run_result_clear(&result);

	/* Its own process group, at the lowest CPU priority. */
	run_tool(&result, "stat", NULL, -1, 10000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	g_free(out);
	out = output_text(&result);
	pid_line = strstr(out, "pid=");
	g_assert_nonnull(pid_line);
	pid_text = g_strdup(pid_line + 4);
	g_strstrip(pid_text);
	close_paren = strrchr(out, ')');
	g_assert_nonnull(close_paren);
	fields = g_strsplit(close_paren + 2, " ", -1);
	g_assert_cmpstr(fields[2], ==, pid_text);	/* pgrp is its own pid */
	g_assert_cmpstr(fields[16], ==, "19");	/* nice */
	g_strfreev(fields);
	axis_probe_run_result_clear(&result);
}

/* ── Hung, runaway and cancelled children ───────────────────────── */

/* Spec: Probe tool hangs: killed, and no child process is left. */
static void
test_timeout_kills_the_group(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	AxisProbeRunResult result;
	gchar **lines;
	GPid background;
	GPid self;
	guint fds = axis_test_storage_count_fds();
	gint64 started = g_get_monotonic_time();

	run_tool(&result, "hang", NULL, -1, 500, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_TIMEOUT);
	g_assert_cmpint(result.exit_status, ==, SIGKILL);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 4 * G_USEC_PER_SEC);

	/* What it printed before it was stopped: its pid and its child's. */
	text = output_text(&result);
	lines = g_strsplit(text, "\n", -1);
	background = (GPid)g_ascii_strtoll(lines[0], NULL, 10);
	self = (GPid)g_ascii_strtoll(lines[1], NULL, 10);
	g_strfreev(lines);
	g_assert_cmpint(self, ==, result.pid);
	g_assert_cmpint(background, >, 1);

	assert_reaped(result.pid);
	assert_group_gone(result.pid);
	wait_until_gone(background);
	g_assert_cmpuint(axis_probe_run_get_unreaped(), ==, 0);
	axis_probe_run_result_clear(&result);
	g_assert_cmpuint(axis_test_storage_count_fds(), ==, fds);
}

/* Spec: Unexpected tool output (enormous): killed, memory bounded. */
static void
test_output_cap_kills(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	guint fds = axis_test_storage_count_fds();
	gint64 started = g_get_monotonic_time();

	run_tool(&result, "flood", NULL, -1, 30000, 100 * 1024, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_TOO_MUCH);
	g_assert_cmpuint(result.output->len, <=, 100 * 1024);
	/* Long before the timeout. */
	g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
	assert_reaped(result.pid);
	assert_group_gone(result.pid);
	axis_probe_run_result_clear(&result);
	g_assert_cmpuint(axis_test_storage_count_fds(), ==, fds);
}

/* A child that floods its error output does not keep the timeout from
 * firing, and only the first bytes are kept. */
static void
test_error_flood_is_bounded(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	gint64 started = g_get_monotonic_time();

	run_tool(&result, "flood-err", NULL, -1, 700, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_TIMEOUT);
	g_assert_cmpuint(result.errors->len, <=, 4096);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
	assert_group_gone(result.pid);
	axis_probe_run_result_clear(&result);
}

/* A child that closes its output and carries on is still timed out. */
static void
test_closed_output_is_still_timed_out(Fixture *fx, gconstpointer data)
{
	AxisProbeRunResult result;
	gint64 started = g_get_monotonic_time();

	run_tool(&result, "daemonize", NULL, -1, 500, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 4 * G_USEC_PER_SEC);
	assert_reaped(result.pid);
	assert_group_gone(result.pid);
	axis_probe_run_result_clear(&result);
}

/* A child that exits leaving another holding the output open does not
 * hold the run up, and the other does not survive. */
static void
test_leftover_child_is_killed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	AxisProbeRunResult result;
	GPid leftover;
	gint64 started = g_get_monotonic_time();

	run_tool(&result, "leave-child", NULL, -1, 30000, 65536, NULL);
	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
	text = output_text(&result);
	leftover = (GPid)g_ascii_strtoll(text, NULL, 10);
	g_assert_cmpint(leftover, >, 1);
	wait_until_gone(leftover);
	assert_group_gone(result.pid);
	axis_probe_run_result_clear(&result);
}

typedef struct
{
	GCancellable *cancellable;
} Canceller;

static gpointer
cancel_later(gpointer data)
{
	Canceller *canceller = data;

	g_usleep(300000);
	g_cancellable_cancel(canceller->cancellable);
	return NULL;
}

static void
test_cancellation(Fixture *fx, gconstpointer data)
{
	Canceller canceller;
	AxisProbeRunResult result;
	GThread *thread;
	gint64 started = g_get_monotonic_time();

	canceller.cancellable = g_cancellable_new();
	thread = g_thread_new("canceller", cancel_later, &canceller);
	run_tool(&result, "hang", NULL, -1, 60000, 65536, canceller.cancellable);
	g_thread_join(thread);

	g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
	assert_reaped(result.pid);
	assert_group_gone(result.pid);
	axis_probe_run_result_clear(&result);
	g_object_unref(canceller.cancellable);
}

/* Many runs at once do not disturb each other. */
static gpointer
run_many(gpointer data)
{
	guint i;

	for (i = 0; i < 5; i++) {
		AxisProbeRunResult result;

		run_tool(&result, "echo", NULL, -1, 10000, 65536, NULL);
		g_assert_cmpint(result.status, ==, AXIS_PROBE_RUN_OK);
		g_assert_cmpuint(result.output->len, ==, 9);
		axis_probe_run_result_clear(&result);
	}
	return NULL;
}

static void
test_concurrent_runs(Fixture *fx, gconstpointer data)
{
	GThread *threads[6];
	guint fds = axis_test_storage_count_fds();
	guint i;

	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		threads[i] = g_thread_new("run", run_many, NULL);
	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		g_thread_join(threads[i]);
	g_assert_cmpuint(axis_test_storage_count_fds(), ==, fds);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)
	ADD("/probe/run/output-and-status", test_output_and_status);
	ADD("/probe/run/crash-or-not", test_crash_or_not);
	ADD("/probe/run/big-output-below-the-cap", test_big_output_below_the_cap);
	ADD("/probe/run/spawn-failure", test_spawn_failure);
	ADD("/probe/run/descriptor-three", test_descriptor_three);
	ADD("/probe/run/descriptor-four", test_descriptor_four);
	ADD("/probe/run/stdin-is-empty", test_stdin_is_empty);
	ADD("/probe/run/no-inherited-descriptors", test_no_inherited_descriptors);
	ADD("/probe/run/environment-is-replaced", test_environment_is_replaced);
	ADD("/probe/run/limits-and-priority", test_limits_and_priority);
	ADD("/probe/run/timeout-kills-the-group", test_timeout_kills_the_group);
	ADD("/probe/run/output-cap-kills", test_output_cap_kills);
	ADD("/probe/run/error-flood-is-bounded", test_error_flood_is_bounded);
	ADD("/probe/run/closed-output-timed-out", test_closed_output_is_still_timed_out);
	ADD("/probe/run/leftover-child-is-killed", test_leftover_child_is_killed);
	ADD("/probe/run/cancellation", test_cancellation);
	ADD("/probe/run/concurrent-runs", test_concurrent_runs);
#undef ADD

	return g_test_run();
}
