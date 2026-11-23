/*
 * axis-daemon-lock.c - one daemon per database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axisd/axis-daemon-lock.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>

#include "axis-version.h"
#include "jobs/axis-job-lifecycle.h"

/* Waiting for the handlers after a loss, in slices this long. */
#define HANDLER_SLICE_MS 200

struct _AxisDaemonLock
{
	AxisDbLock          *lock;		/* the start thread's, then the watcher's */
	gchar                instance[AXIS_JOB_INSTANCE_LEN + 1];
	gint                 held;		/* atomic */
	gint                 backend_pid;	/* atomic */

	GMutex               mutex;
	gboolean             stop;		/* (mutex) */
	gchar               *taken_by;	/* (mutex) */

	AxisDaemonLockHooks  hooks;
	gpointer             data;
	gint                 wake[2];
	GThread             *thread;
};

AxisDaemonLock *
axis_daemon_lock_new(AxisDbPool *pool)
{
	AxisDaemonLock *self;
	g_autofree gchar *host = NULL;
	g_autofree gchar *name = NULL;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisDaemonLock, 1);
	g_snprintf(self->instance, sizeof(self->instance), "%08x%08x", g_random_int(), g_random_int());
	/* The instance first: the server keeps 63 bytes of a name, and the
	 * lock recognises an earlier session of its own by the whole name. */
	host = g_strdup(g_get_host_name());
	g_strdelimit(host, "/", '-');
	name = g_strdup_printf("axisd %s %s pid %d on %s", self->instance, AXIS_VERSION, (int)getpid(), host);
	self->lock = axis_db_lock_new(pool, AXIS_DB_DAEMON_LOCK_KEY, name);
	g_mutex_init(&self->mutex);
	self->wake[0] = -1;
	self->wake[1] = -1;

	return self;
}

void
axis_daemon_lock_free(AxisDaemonLock *self)
{
	if (self == NULL)
		return;

	axis_daemon_lock_stop_watching(self);
	axis_db_lock_free(self->lock);
	if (self->wake[0] >= 0)
		close(self->wake[0]);
	if (self->wake[1] >= 0)
		close(self->wake[1]);
	g_free(self->taken_by);
	g_mutex_clear(&self->mutex);
	g_free(self);
}

const gchar *
axis_daemon_lock_get_instance(AxisDaemonLock *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->instance;
}

const gchar *
axis_daemon_lock_get_name(AxisDaemonLock *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return axis_db_lock_get_name(self->lock);
}

gint64
axis_daemon_lock_get_key(AxisDaemonLock *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return axis_db_lock_get_key(self->lock);
}

gboolean
axis_daemon_lock_is_held(AxisDaemonLock *self)
{
	g_return_val_if_fail(self != NULL, FALSE);

	return g_atomic_int_get(&self->held);
}

gint
axis_daemon_lock_get_backend_pid(AxisDaemonLock *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_atomic_int_get(&self->backend_pid);
}

const gchar *
axis_daemon_lock_get_backend_start(AxisDaemonLock *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return axis_db_lock_get_backend_start(self->lock);
}

const gchar *
axis_daemon_lock_get_taken_by(AxisDaemonLock *self)
{
	const gchar *taken_by;

	g_return_val_if_fail(self != NULL, NULL);

	/* Set once, before hooks.taken(), and never changed after. */
	g_mutex_lock(&self->mutex);
	taken_by = self->taken_by;
	g_mutex_unlock(&self->mutex);

	return taken_by;
}

/* One attempt, bounded by AXIS_DAEMON_LOCK_TRY_DEADLINE_MS. */
static AxisDbLockResult
try_once(
	AxisDaemonLock  *self,
	gchar          **holder,
	GError         **error
){
	g_autoptr(AxisDbLockHolder) found = NULL;
	AxisDbLockResult result;

	if (holder != NULL)
		*holder = NULL;
	result = axis_db_lock_try(self->lock,
		g_get_monotonic_time() + (gint64)AXIS_DAEMON_LOCK_TRY_DEADLINE_MS * 1000, &found, error);
	switch (result) {
	case AXIS_DB_LOCK_TAKEN:
		g_atomic_int_set(&self->backend_pid, axis_db_lock_get_backend_pid(self->lock));
		g_atomic_int_set(&self->held, TRUE);
		break;
	case AXIS_DB_LOCK_BUSY:
		if (holder != NULL)
			*holder = axis_db_lock_holder_describe(found);
		break;
	case AXIS_DB_LOCK_ERROR:
	default:
		break;
	}

	return result;
}

AxisDbLockResult
axis_daemon_lock_try(
	AxisDaemonLock  *self,
	gchar          **holder,
	GError         **error
){
	g_return_val_if_fail(self != NULL, AXIS_DB_LOCK_ERROR);

	return try_once(self, holder, error);
}

/*
 * The watcher
 */

static gboolean
watch_stopping(AxisDaemonLock *self)
{
	gboolean stop;

	g_mutex_lock(&self->mutex);
	stop = self->stop;
	g_mutex_unlock(&self->mutex);

	return stop;
}

static void
drain_wake(AxisDaemonLock *self)
{
	gchar buffer[16];

	while (read(self->wake[0], buffer, sizeof(buffer)) > 0)
		;
}

/* Sleeps @ms unless woken; the lock is not held, so only the pipe and the
 * clock are watched. */
static void
watch_sleep(
	AxisDaemonLock *self,
	guint           ms
){
	if (axis_db_lock_wait(self->lock, self->wake[0], g_get_monotonic_time() + (gint64)ms * 1000, NULL) ==
	    AXIS_DB_LOCK_WOKEN)
		drain_wake(self);
}

/*
 * The lock is gone.  Stops the daemon's work, waits for its handlers (or
 * ends the process), then takes the lock again.  Returns TRUE when it holds
 * the lock again; FALSE when the watcher must end (a stop, or another
 * session holds it).
 */
static gboolean
handle_loss(
	AxisDaemonLock *self,
	const GError   *cause
){
	gint64 lost_at = g_get_monotonic_time();
	gint64 grace_end = lost_at + (gint64)AXIS_DAEMON_LOCK_STOP_GRACE_MS * 1000;
	guint delay_ms = 500;
	gboolean warned = FALSE;

	g_atomic_int_set(&self->held, FALSE);
	g_atomic_int_set(&self->backend_pid, 0);
	g_warning("lost the database lock: %s. This daemon runs no job and serves only the probes until it holds "
		"the lock again, because another daemon may take the database meanwhile",
		cause != NULL ? cause->message : "no reason given");
	self->hooks.lost(self->data);

	/* No handler may go on as if this daemon were alone: they get the grace
	 * period, and the process ends if one is still running then.  A stop
	 * that comes meanwhile does not shorten this. */
	while (!self->hooks.wait_handlers(self->data, HANDLER_SLICE_MS)) {
		if (g_get_monotonic_time() >= grace_end) {
			g_warning("a job handler did not stop within %d s after the database lock was lost; ending the "
				"process at once, so that it cannot run beside a daemon that takes the database "
				"(exit status %d)", AXIS_DAEMON_LOCK_STOP_GRACE_MS / 1000, AXIS_DAEMON_LOCK_EXIT_STUCK);
			_exit(AXIS_DAEMON_LOCK_EXIT_STUCK);
		}
	}
	g_message("database lock: no job handler runs any more (%" G_GINT64_FORMAT " ms after the loss); taking the "
		"lock again", (g_get_monotonic_time() - lost_at) / 1000);

	for (;;) {
		g_autofree gchar *holder = NULL;
		g_autoptr(GError) error = NULL;
		AxisDbLockResult result;

		if (watch_stopping(self))
			return FALSE;

		result = try_once(self, &holder, &error);
		if (result == AXIS_DB_LOCK_TAKEN) {
			/* Held again for the shutdown that began meanwhile, which
			 * releases it once the job queue has stopped: nothing resumes. */
			if (watch_stopping(self))
				return FALSE;
			g_message("holds the database lock again, %" G_GINT64_FORMAT " ms after it was lost; resuming",
				(g_get_monotonic_time() - lost_at) / 1000);
			self->hooks.regained(self->data, axis_daemon_lock_get_backend_pid(self),
				axis_daemon_lock_get_backend_start(self));
			return TRUE;
		}
		if (result == AXIS_DB_LOCK_BUSY && watch_stopping(self))
			return FALSE;
		if (result == AXIS_DB_LOCK_BUSY) {
			g_mutex_lock(&self->mutex);
			g_free(self->taken_by);
			self->taken_by = g_strdup(holder);
			g_mutex_unlock(&self->mutex);
			g_warning("another session took the database lock while this daemon had lost it: %s; this daemon "
				"stops, so that it starts afresh and waits for that one", holder);
			self->hooks.taken(self->data);
			return FALSE;
		}

		/* Our own earlier session (its name) is not another daemon: it
		 * holds the lock only until the server ends it. */
		if (!warned)
			g_warning("cannot take the database lock again: %s; retrying (no job runs meanwhile)",
				result == AXIS_DB_LOCK_OURS ? "an earlier connection of this daemon still holds it" :
				error != NULL ? error->message : "no reason given");
		warned = TRUE;
		watch_sleep(self, delay_ms);
		delay_ms = MIN(delay_ms * 2, AXIS_DAEMON_LOCK_RETAKE_MAX_MS);
	}
}

/*
 * The watcher.  @confirmed is when the latest question that the server
 * answered yes was sent: the lock was held at some moment after it.  The
 * lock is lost when the connection closes, when a question is answered no,
 * or when @confirmed is AXIS_DAEMON_LOCK_SILENCE_MS old.  Every wait ends
 * by then.
 */
static gpointer
watch_main(gpointer data)
{
	AxisDaemonLock *self = data;
	const gint64 silence_us = (gint64)AXIS_DAEMON_LOCK_SILENCE_MS * 1000;
	gint64 confirmed = g_get_monotonic_time();	/* the lock was just taken */
	gint64 next_send = confirmed + (gint64)AXIS_DAEMON_LOCK_CHECK_MS * 1000;
	gboolean slow = FALSE;		/* a silence is being reported */
	gboolean vouched = FALSE;	/* another connection said it is held */

	while (!watch_stopping(self)) {
		g_autoptr(GError) error = NULL;
		gint64 now = g_get_monotonic_time();
		gint64 lost_at = confirmed + silence_us;
		gint64 sent = 0;
		gboolean lost = FALSE;
		AxisDbLockState state;

		if (now < next_send && now < lost_at) {
			AxisDbLockWait wait = axis_db_lock_wait(self->lock, self->wake[0], MIN(next_send, lost_at), &error);

			if (wait == AXIS_DB_LOCK_WOKEN)
				drain_wake(self);
			if (wait != AXIS_DB_LOCK_LOST)
				continue;
			lost = TRUE;
		} else if (now >= lost_at) {
			g_set_error(&error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT,
				"the server has not confirmed it for %d s, on the lock's connection or another",
				AXIS_DAEMON_LOCK_SILENCE_MS / 1000);
			axis_db_lock_release(self->lock);
			lost = TRUE;
		} else {
			/* Time for a check, or the answer to the one in flight. */
			next_send = now + (gint64)AXIS_DAEMON_LOCK_CHECK_MS * 1000;
			state = axis_db_lock_check(self->lock,
				MIN(now + (gint64)AXIS_DAEMON_LOCK_CHECK_DEADLINE_MS * 1000, lost_at), &sent, &error);
			if (state == AXIS_DB_LOCK_HELD) {
				confirmed = MAX(confirmed, sent);
				if (slow)
					g_message("database lock: the lock's connection answers again");
				slow = FALSE;
				vouched = FALSE;
				continue;
			}
			if (state == AXIS_DB_LOCK_GONE) {
				lost = TRUE;
			} else {
				/* No answer proves nothing: ask on another connection. */
				if (!slow)
					g_warning("database lock: the lock's connection did not answer within %d s (%s); asking "
						"the server on another connection whether this daemon still holds the lock",
						AXIS_DAEMON_LOCK_CHECK_DEADLINE_MS / 1000,
						error != NULL ? error->message : "the server is slow, or the network is gone");
				slow = TRUE;
				g_clear_error(&error);
				now = g_get_monotonic_time();
				if (now >= lost_at)
					continue;
				state = axis_db_lock_verify(self->lock,
					MIN(now + (gint64)AXIS_DAEMON_LOCK_VERIFY_DEADLINE_MS * 1000, lost_at), &sent, &error);
				if (state == AXIS_DB_LOCK_HELD) {
					confirmed = MAX(confirmed, sent);
					if (!vouched)
						g_message("database lock: the server confirms on another connection that this daemon "
							"still holds the lock; its own connection is slow");
					vouched = TRUE;
					continue;
				}
				if (state == AXIS_DB_LOCK_SILENT) {
					g_debug("database lock: no answer on another connection either: %s", error->message);
					continue;
				}
				axis_db_lock_release(self->lock);
				lost = TRUE;
			}
		}

		if (lost) {
			if (!handle_loss(self, error))
				break;
			confirmed = g_get_monotonic_time();
			next_send = confirmed + (gint64)AXIS_DAEMON_LOCK_CHECK_MS * 1000;
			slow = FALSE;
			vouched = FALSE;
		}
	}

	return NULL;
}

void
axis_daemon_lock_watch(
	AxisDaemonLock            *self,
	const AxisDaemonLockHooks *hooks,
	gpointer                   data
){
	g_autoptr(GError) error = NULL;

	g_return_if_fail(self != NULL && hooks != NULL);
	g_return_if_fail(self->thread == NULL);

	if (self->wake[0] < 0 &&
	    (!g_unix_open_pipe(self->wake, O_CLOEXEC, &error) ||
	     !g_unix_set_fd_nonblocking(self->wake[0], TRUE, &error) ||
	     !g_unix_set_fd_nonblocking(self->wake[1], TRUE, &error))) {
		/* Without a pipe the watcher still works; a stop then waits for
		 * its next check. */
		g_warning("database lock: cannot make the watcher's wake pipe: %s", error->message);
		if (self->wake[0] >= 0)
			close(self->wake[0]);
		if (self->wake[1] >= 0)
			close(self->wake[1]);
		self->wake[0] = -1;
		self->wake[1] = -1;
	}

	self->hooks = *hooks;
	self->data = data;
	g_mutex_lock(&self->mutex);
	self->stop = FALSE;
	g_mutex_unlock(&self->mutex);
	self->thread = g_thread_new("axis-db-lock", watch_main, self);
}

void
axis_daemon_lock_stop_watching(AxisDaemonLock *self)
{
	g_return_if_fail(self != NULL);

	if (self->thread == NULL)
		return;

	g_mutex_lock(&self->mutex);
	self->stop = TRUE;
	g_mutex_unlock(&self->mutex);
	if (self->wake[1] >= 0 && write(self->wake[1], "x", 1) < 0 && errno != EAGAIN)
		g_debug("database lock: cannot wake the watcher: %s", g_strerror(errno));

	g_thread_join(self->thread);
	self->thread = NULL;
}
