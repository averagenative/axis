/*
 * axis-crash.c - a report in the log when a program dies of a fatal signal
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The handler runs in whichever thread got the signal, with the process in
 * an unknown state, so it calls only what is safe there or close to it:
 * write(), open(), read(), close(), clock_gettime(), prctl(), syscall(),
 * sigaction(), raise(), nanosleep() and backtrace().  backtrace() is called
 * once at install, so that libgcc_s is loaded before it is needed; with the
 * GCC and glibc the image is built with, the unwinder finds a frame's unwind
 * data through _dl_find_object(), which takes no lock.  Each frame's file
 * and offset come from /proc/self/maps, read in pieces into static buffers:
 * dladdr() and backtrace_symbols_fd() take the loader's lock, and a handler
 * that waited on a lock its own thread held would hang the process instead
 * of ending it (a container's restart policy restarts a program that died,
 * not one that hangs).  The buffers are static; one thread at a time writes
 * a report (crash_owner).
 *
 * After the report the handler puts the signal's default action back.  A
 * fault (a bad address, an illegal instruction, a division by zero) then
 * happens again at the same instruction once the handler returns, and ends
 * the process.  A signal that was sent (abort(), raise(), kill) is sent
 * again.  A process that is PID 1 in a container drops a sent signal whose
 * action is the default, so a PID 1 ends itself instead, with 128 and the
 * signal's number as its status, as a shell reports a death by that
 * signal.  The log must not say "crashed" of a
 * process that goes on.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "core/axis-crash.h"
#include "core/axis-log.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_FRAMES   64
#define PATH_LEN     256
#define MESSAGE_LEN  1024

typedef struct
{
	unsigned long address;
	unsigned long offset;	/* in the file; when found */
	gboolean      found;
	char          path[PATH_LEN];	/* "" for memory that is no file's */
} Frame;

typedef struct
{
	char  *data;
	gsize  len;
	gsize  cap;
} Buf;

static const gchar *crash_domain = "axis";
static const gchar *crash_build = "";
static gint crash_owner;	/* the id of the thread writing a report; 0 for none */

static Frame frames[MAX_FRAMES + 1];
static void *returns[MAX_FRAMES];
static char maps_chunk[8192];
static char message[MESSAGE_LEN];
static char line[2 * MESSAGE_LEN];
static char alt_stack[65536];

static const int fatal_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

/* ── Writing without allocating ─────────────────────────────────── */

static void
put_char(
	Buf  *b,
	char  c
){
	if (b->len + 1 < b->cap) {
		b->data[b->len++] = c;
		b->data[b->len] = '\0';
	}
}

static void
put(
	Buf        *b,
	const char *s
){
	for (; s != NULL && *s != '\0'; s++)
		put_char(b, *s);
}

static void
put_number(
	Buf           *b,
	unsigned long  value,
	unsigned       base,
	unsigned       min_digits
){
	char digits[24];
	unsigned n = 0;

	do {
		digits[n++] = "0123456789abcdef"[value % base];
		value /= base;
	} while (value != 0 && n < sizeof(digits));
	while (n < min_digits && n < sizeof(digits))
		digits[n++] = '0';
	while (n > 0)
		put_char(b, digits[--n]);
}

static void
put_signed(
	Buf  *b,
	long  value
){
	if (value < 0) {
		put_char(b, '-');
		put_number(b, (unsigned long)(-(value + 1)) + 1, 10, 1);
	} else {
		put_number(b, (unsigned long)value, 10, 1);
	}
}

static void
put_hex(
	Buf           *b,
	unsigned long  value
){
	put(b, "0x");
	put_number(b, value, 16, 1);
}

/* The time as axis-log.c writes it: 2026-11-27T17:54:05Z. */
static void
put_time(Buf *b)
{
	struct timespec now;
	long days;
	long secs;
	long z;
	long era;
	unsigned long doe;
	unsigned long yoe;
	unsigned long doy;
	unsigned long mp;
	unsigned long month;
	unsigned long day;
	long year;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
		put(b, "-");
		return;
	}
	days = (long)(now.tv_sec / 86400);
	secs = (long)(now.tv_sec % 86400);
	/* The civil date of a day count (Howard Hinnant's days_to_civil). */
	z = days + 719468;
	era = z / 146097;
	doe = (unsigned long)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	year = (long)yoe + era * 400 + (month <= 2 ? 1 : 0);

	put_number(b, (unsigned long)year, 10, 4);
	put_char(b, '-');
	put_number(b, month, 10, 2);
	put_char(b, '-');
	put_number(b, day, 10, 2);
	put_char(b, 'T');
	put_number(b, (unsigned long)(secs / 3600), 10, 2);
	put_char(b, ':');
	put_number(b, (unsigned long)(secs / 60 % 60), 10, 2);
	put_char(b, ':');
	put_number(b, (unsigned long)(secs % 60), 10, 2);
	put_char(b, 'Z');
}

/* @s in a JSON string: quotes, backslashes and control characters escaped;
 * a byte outside ASCII becomes '?', since it may not be UTF-8. */
static void
put_json(
	Buf        *b,
	const char *s
){
	for (; s != NULL && *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			put_char(b, '\\');
			put_char(b, (char)c);
		} else if (c < 0x20 || c == 0x7f) {
			put(b, "\\u00");
			put_number(b, c, 16, 2);
		} else if (c >= 0x80) {
			put_char(b, '?');
		} else {
			put_char(b, (char)c);
		}
	}
}

static void
write_all(
	const char *data,
	gsize       len
){
	while (len > 0) {
		ssize_t n = write(STDERR_FILENO, data, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		data += n;
		len -= (gsize)n;
	}
}

/* One line of the log, at the error level, in the log's format. */
static void
emit(const char *text)
{
	Buf b;
	const char *p;

	b.data = line;
	b.len = 0;
	b.cap = sizeof(line) - 1;	/* room for the newline */
	line[0] = '\0';
	if (axis_log_get_format() == AXIS_LOG_FORMAT_JSON) {
		put(&b, "{\"time\":\"");
		put_time(&b);
		put(&b, "\",\"level\":\"error\",\"domain\":\"");
		put_json(&b, crash_domain);
		put(&b, "\",\"message\":\"");
		put_json(&b, text);
		put(&b, "\"}");
	} else {
		put_time(&b);
		put(&b, " ERROR ");
		put(&b, crash_domain);
		put(&b, ": ");
		for (p = text; *p != '\0'; p++)
			put_char(&b, ((unsigned char)*p < 0x20 || *p == 0x7f) ? ' ' : *p);
	}
	line[b.len++] = '\n';
	write_all(line, b.len);
}

/* ── The report ─────────────────────────────────────────────────── */

static const char *
signal_name(int sig)
{
	switch (sig) {
	case SIGSEGV:
		return "SIGSEGV";
	case SIGBUS:
		return "SIGBUS";
	case SIGILL:
		return "SIGILL";
	case SIGFPE:
		return "SIGFPE";
	case SIGABRT:
		return "SIGABRT";
	default:
		return "a fatal signal";
	}
}

static const char *
fault_cause(
	int sig,
	int code
){
	if (sig == SIGSEGV && code == SEGV_MAPERR)
		return "nothing is mapped there";
	if (sig == SIGSEGV && code == SEGV_ACCERR)
		return "that access is not allowed there";
	if (sig == SIGBUS)
		return "the memory there cannot be read or written";
	if (sig == SIGILL)
		return "an illegal instruction";
	if (sig == SIGFPE)
		return "an arithmetic fault, such as a division by zero";
	return "a fault";
}

/* The instruction the signal arrived at, from the thread's saved state. */
static unsigned long
context_pc(void *context)
{
	const ucontext_t *uc = context;

	if (uc == NULL)
		return 0;
#if defined(__x86_64__)
	return (unsigned long)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
	return (unsigned long)uc->uc_mcontext.pc;
#else
	return 0;
#endif
}

static gboolean
parse_hex(
	const char    **p,
	unsigned long  *out
){
	unsigned long value = 0;
	guint digits = 0;

	for (;; (*p)++) {
		char c = **p;
		unsigned d;

		if (c >= '0' && c <= '9')
			d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (unsigned)(c - 'A' + 10);
		else
			break;
		value = value * 16 + d;
		digits++;
	}
	*out = value;
	return digits > 0;
}

/* One line of /proc/self/maps ("start-end perms offset dev inode path"):
 * every frame it holds gets its file and offset. */
static void
match_row(
	const char *row,
	guint       n
){
	const char *p = row;
	unsigned long start;
	unsigned long end;
	unsigned long offset;
	guint field;
	guint i;

	if (!parse_hex(&p, &start) || *p++ != '-' || !parse_hex(&p, &end) || *p++ != ' ')
		return;
	while (*p != ' ' && *p != '\0')
		p++;
	if (*p++ != ' ' || !parse_hex(&p, &offset) || *p != ' ')
		return;
	for (field = 0; field < 2; field++) {
		while (*p == ' ')
			p++;
		while (*p != ' ' && *p != '\0')
			p++;
	}
	while (*p == ' ')
		p++;

	for (i = 0; i < n; i++) {
		Frame *f = &frames[i];
		gsize k;

		if (f->found || f->address < start || f->address >= end)
			continue;
		f->found = TRUE;
		f->offset = f->address - start + offset;
		for (k = 0; p[k] != '\0' && k + 1 < sizeof(f->path); k++)
			f->path[k] = p[k];
		f->path[k] = '\0';
	}
}

static void
locate_frames(guint n)
{
	char row[PATH_LEN + 128];
	gsize row_len = 0;
	ssize_t got;
	gint fd;

	fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	for (;;) {
		ssize_t i;

		got = read(fd, maps_chunk, sizeof(maps_chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		for (i = 0; i < got; i++) {
			if (maps_chunk[i] == '\n') {
				row[row_len] = '\0';
				match_row(row, n);
				row_len = 0;
			} else if (row_len + 1 < sizeof(row)) {
				/* A longer row keeps its start: the path is cut short. */
				row[row_len++] = maps_chunk[i];
			}
		}
	}
	close(fd);
}

static void
report(
	int        sig,
	siginfo_t *info,
	void      *context,
	gint       thread_id
){
	char thread[17];
	gboolean fault = info != NULL && info->si_code > 0 && sig != SIGABRT;
	unsigned long pc = context_pc(context);
	Buf m;
	gint n;
	gint i;
	guint count = 0;

	memset(thread, 0, sizeof(thread));
	if (prctl(PR_GET_NAME, (unsigned long)thread, 0UL, 0UL, 0UL) != 0)
		thread[0] = '\0';
	thread[16] = '\0';

	m.data = message;
	m.len = 0;
	m.cap = sizeof(message);
	message[0] = '\0';
	put(&m, "crashed: signal ");
	put_signed(&m, sig);
	put(&m, " (");
	put(&m, signal_name(sig));
	put(&m, ")");
	if (fault) {
		put(&m, " at address ");
		put_hex(&m, (unsigned long)info->si_addr);
		put(&m, " (");
		put(&m, fault_cause(sig, info->si_code));
		put(&m, ")");
	} else if (info != NULL) {
		put(&m, ", sent by process ");
		put_signed(&m, (long)info->si_pid);
	}
	put(&m, ", in thread ");
	put(&m, thread[0] != '\0' ? thread : "(no name)");
	put(&m, " (id ");
	put_signed(&m, thread_id);
	put(&m, "); build ");
	put(&m, crash_build);
	put(&m, ". The stack of that thread follows, a frame a line, as a file and an offset in it");
	emit(message);

	memset(frames, 0, sizeof(frames));
	if (pc != 0)
		frames[count++].address = pc;
	n = backtrace(returns, MAX_FRAMES);
	for (i = 0; i < n; i++)
		frames[count++].address = (unsigned long)returns[i];
	locate_frames(count);

	for (i = 0; i < (gint)count; i++) {
		const Frame *f = &frames[i];

		m.len = 0;
		message[0] = '\0';
		put(&m, "crash: frame ");
		put_signed(&m, i);
		put(&m, ": ");
		if (f->found && f->path[0] != '\0') {
			put(&m, f->path);
			put(&m, "+");
			put_hex(&m, f->offset);
		} else {
			put_hex(&m, f->address);
			put(&m, f->found ? " (memory that is no file's)" : " (in nothing mapped)");
		}
		if (i == 0 && pc != 0)
			put(&m, fault ? " (the instruction that faulted)" : " (where the signal arrived)");
		emit(message);
	}

	emit("crash: end of the report. With the build's debug files (make container-debug), "
		"eu-addr2line -f -i -e PROGRAM.debug OFFSET names the function and line of each frame in that program "
		"(deploy/README.md, \"When a program crashes\")");
}

static void
on_fatal(
	int        sig,
	siginfo_t *info,
	void      *context
){
	gint saved = errno;
	gint me = (gint)syscall(SYS_gettid);
	struct sigaction dfl;

	if (g_atomic_int_compare_and_exchange(&crash_owner, 0, me)) {
		report(sig, info, context, me);
	} else if (g_atomic_int_get(&crash_owner) != me) {
		/* Another thread is writing its report, and ends the process after
		 * it: this one waits for that, a while. */
		struct timespec wait;

		wait.tv_sec = 5;
		wait.tv_nsec = 0;
		while (nanosleep(&wait, &wait) != 0 && errno == EINTR)
			;
	}
	/* (The same thread again: a fault in its own report, or abort()'s last
	 * resort after a SIGABRT that a PID 1 dropped.  No second report.) */

	memset(&dfl, 0, sizeof(dfl));
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	(void)sigaction(sig, &dfl, NULL);
	if (info == NULL || info->si_code <= 0) {
		/* Blocked while this handler runs, and delivered when it returns;
		 * a PID 1 would queue it and then drop it. */
		if (getpid() == 1)
			_exit(128 + sig);
		(void)raise(sig);
	}
	errno = saved;
}

void
axis_crash_report_install(
	const gchar *domain,
	const gchar *build
){
	struct sigaction action;
	stack_t stack;
	void *probe[1];
	guint i;

	crash_domain = domain != NULL ? domain : "axis";
	crash_build = build != NULL ? build : "";
	(void)backtrace(probe, 1);

	stack.ss_sp = alt_stack;
	stack.ss_size = sizeof(alt_stack);
	stack.ss_flags = 0;
	(void)sigaltstack(&stack, NULL);

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = on_fatal;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (i = 0; i < G_N_ELEMENTS(fatal_signals); i++)
		(void)sigaction(fatal_signals[i], &action, NULL);
}
