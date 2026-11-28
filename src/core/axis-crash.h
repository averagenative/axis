/*
 * axis-crash.h - a report in the log when a program dies of a fatal signal
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_CRASH_H
#define AXIS_CRASH_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * axis_crash_report_install:
 * @domain: the log domain the report's lines carry ("axis", "axis-worker",
 *   "axis-portal"); kept, so a string that lives as long as the process
 * @build: the version and commit ("0.1.0 (d3fc47a)"); kept likewise
 *
 * From now on, when the process gets SIGSEGV, SIGBUS, SIGILL, SIGFPE or
 * SIGABRT, it writes to stderr, in the log's format (text or JSON, as
 * axis_log_set_format() last chose): a line that names the signal, the
 * address, the thread and the build; a line for each frame of the stack of
 * the thread that got it, as the file of the frame's code and the offset in
 * that file; and a line that says how to turn those offsets into names.
 * The signal then ends the process as it would have without this; a
 * signal sent to a PID 1, which would drop it, ends it too (status 128 and
 * the signal's number).
 *
 * Call it once, early in main(), after axis_log_init().  Nothing in the
 * handler allocates or takes a lock; a thread whose own stack overflowed
 * cannot run it (only the main thread's handler has a stack of its own),
 * and such a crash leaves no report.
 */
void
axis_crash_report_install(
	const gchar *domain,
	const gchar *build
);

G_END_DECLS

#endif /* AXIS_CRASH_H */
