/*
 * test-crash.c - the report a fatal signal leaves in the log
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each case crashes a child (g_test_trap_subprocess) and reads its stderr.
 * Under AddressSanitizer the bad address is left out: ASan keeps SIGSEGV for
 * its own report.  abort() and a raised SIGILL are not ASan's, so those run
 * in every build.
 */

#include <glib.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "core/axis-crash.h"
#include "core/axis-log.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define UNDER_SANITIZER 1
#endif
#endif

#ifndef UNDER_SANITIZER
/* Read through a volatile, so that the compiler sees no constant address. */
static volatile gsize bad_address = 0x10;

static gpointer
fault_in_thread(gpointer data)
{
	volatile gint *bad = (volatile gint *)bad_address;

	*bad = 1;
	return NULL;
}
#endif

/* A bad address in a thread with a name: the signal, the address and its
 * cause, the thread, the build, then frames as files and offsets, the
 * first the faulting instruction; the child dies of the signal. */
static void
test_fault(void)
{
#ifdef UNDER_SANITIZER
	g_test_skip("the sanitizer keeps SIGSEGV for its own report");
#else
	if (g_test_subprocess()) {
		GThread *thread;

		axis_log_init(AXIS_LOG_LEVEL_INFO);
		axis_crash_report_install("axis-test", "test 0.0.0 (made-up)");
		thread = g_thread_new("crash-here", fault_in_thread, NULL);
		g_thread_join(thread);
		return;
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
	g_test_trap_assert_stderr("*Z ERROR axis-test: crashed: signal 11 (SIGSEGV) at address 0x10 (nothing is mapped "
		"there), in thread crash-here (id *); build test 0.0.0 (made-up). The stack of that thread follows*");
	g_test_trap_assert_stderr("*ERROR axis-test: crash: frame 0: *test-crash+0x* (the instruction that faulted)*");
	g_test_trap_assert_stderr("*ERROR axis-test: crash: frame 1: *");
	g_test_trap_assert_stderr("*ERROR axis-test: crash: end of the report.*");
#endif
}

/* abort(): a signal that was sent, by the process itself, in the main
 * thread; the child still dies of it. */
static void
test_abort(void)
{
	if (g_test_subprocess()) {
		axis_log_init(AXIS_LOG_LEVEL_INFO);
		axis_crash_report_install("axis-test", "test 0.0.0 (made-up)");
		abort();
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
	g_test_trap_assert_stderr("*ERROR axis-test: crashed: signal 6 (SIGABRT), sent by process *, in thread *");
	g_test_trap_assert_stderr("*crash: frame 0: * (where the signal arrived)*");
	g_test_trap_assert_stderr("*crash: end of the report.*");
	/* One report, though abort() may go on to a fault of its own. */
	g_test_trap_assert_stderr_unmatched("*crashed:*crashed:*");
}

/* In the JSON format every line of the report is an object of the log's
 * own shape (axis-log.c), at the error level, with the quotes of the build
 * escaped. */
static void
test_json(void)
{
	if (g_test_subprocess()) {
		axis_log_init(AXIS_LOG_LEVEL_INFO);
		axis_log_set_format(AXIS_LOG_FORMAT_JSON);
		axis_crash_report_install("axis-test", "test \"0.0.0\" (made-up)");
		raise(SIGILL);
		return;
	}
	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
	g_test_trap_assert_stderr("{\"time\":\"\?\?\?\?-\?\?-\?\?T\?\?:\?\?:\?\?Z\",\"level\":\"error\",\"domain\":\"axis-test\","
		"\"message\":\"crashed: signal 4 (SIGILL), sent by process *, in thread *; build test \\\"0.0.0\\\" "
		"(made-up). The stack of that thread follows, a frame a line, as a file and an offset in it\"}\n"
		"{\"time\":\"\?\?\?\?-\?\?-\?\?T\?\?:\?\?:\?\?Z\",\"level\":\"error\",\"domain\":\"axis-test\","
		"\"message\":\"crash: frame 0: *\"}\n*"
		"{\"time\":\"\?\?\?\?-\?\?-\?\?T\?\?:\?\?:\?\?Z\",\"level\":\"error\",\"domain\":\"axis-test\","
		"\"message\":\"crash: end of the report. *\"}\n");
	g_test_trap_assert_stderr_unmatched("*ERROR axis-test:*");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/crash/fault", test_fault);
	g_test_add_func("/crash/abort", test_abort);
	g_test_add_func("/crash/json", test_json);

	return g_test_run();
}
