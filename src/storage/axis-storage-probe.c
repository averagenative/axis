/*
 * axis-storage-probe.c - filesystem work with a deadline, on a bounded set
 * of helper threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One thread per call, made when the call starts and gone when its
 * function returns, so nothing idles and a stuck thread is simply a call
 * that has not returned.  The helper's lock guards the counts and every
 * call's done and abandoned flags; one condition, broadcast whenever a
 * call finishes, wakes the waiters.
 */

#include "storage/axis-storage-probe.h"

G_DEFINE_QUARK(axis-storage-probe-error-quark, axis_storage_probe_error)

struct _AxisStorageProbe
{
	gint        ref_count;		/* atomic */
	guint       max_threads;
	GMutex      lock;
	GCond       finished;
	guint       running;		/* (lock) */
	guint       abandoned;		/* (lock) */
	GHashTable *abandoned_keys;	/* (lock) key -> number of abandoned calls */
};

struct _AxisStorageProbeCall
{
	gint                  ref_count;	/* atomic: the caller's and the thread's */
	AxisStorageProbe     *probe;
	gchar                *key;
	AxisStorageProbeFunc  func;
	gpointer              data;
	GDestroyNotify        free_data;
	gboolean              done;		/* (probe lock) */
	gboolean              abandoned;	/* (probe lock) */
};

AxisStorageProbe *
axis_storage_probe_new(guint max_threads)
{
	AxisStorageProbe *self;

	g_return_val_if_fail(max_threads > 0, NULL);

	self = g_new0(AxisStorageProbe, 1);
	self->ref_count = 1;
	self->max_threads = max_threads;
	g_mutex_init(&self->lock);
	g_cond_init(&self->finished);
	self->abandoned_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	return self;
}

static AxisStorageProbe *
probe_ref(AxisStorageProbe *self)
{
	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_storage_probe_unref(AxisStorageProbe *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_hash_table_unref(self->abandoned_keys);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->finished);
	g_free(self);
}

static void
call_free(AxisStorageProbeCall *call)
{
	if (call->free_data != NULL)
		call->free_data(call->data);
	g_free(call->key);
	axis_storage_probe_unref(call->probe);
	g_free(call);
}

void
axis_storage_probe_call_unref(AxisStorageProbeCall *call)
{
	if (call == NULL || !g_atomic_int_dec_and_test(&call->ref_count))
		return;

	call_free(call);
}

gpointer
axis_storage_probe_call_get_data(AxisStorageProbeCall *call)
{
	g_return_val_if_fail(call != NULL, NULL);

	return call->data;
}

static gpointer
call_thread(gpointer user_data)
{
	AxisStorageProbeCall *call = user_data;
	AxisStorageProbe *probe = probe_ref(call->probe);
	gboolean last;

	call->func(call->data);

	g_mutex_lock(&probe->lock);
	call->done = TRUE;
	probe->running--;
	if (call->abandoned) {
		guint n = GPOINTER_TO_UINT(g_hash_table_lookup(probe->abandoned_keys, call->key));

		probe->abandoned--;
		if (n <= 1)
			g_hash_table_remove(probe->abandoned_keys, call->key);
		else
			g_hash_table_insert(probe->abandoned_keys, g_strdup(call->key), GUINT_TO_POINTER(n - 1));
	}
	/* This thread's reference goes before the waiter is told, so that a
	 * caller who waited for the call drops the last one itself: what the
	 * call holds (a descriptor on the volume's directory, say) is then
	 * released by the time the caller's unref returns, and not a moment
	 * later on this thread while the caller already unmounts.  The call is
	 * not touched after the unlock unless it was abandoned, when nobody
	 * else holds it. */
	last = g_atomic_int_dec_and_test(&call->ref_count);
	g_cond_broadcast(&probe->finished);
	g_mutex_unlock(&probe->lock);

	if (last)
		call_free(call);
	/* Its own reference: the lock above outlives the call's. */
	axis_storage_probe_unref(probe);
	return NULL;
}

AxisStorageProbeCall *
axis_storage_probe_start(
	AxisStorageProbe      *self,
	const gchar           *key,
	AxisStorageProbeFunc   func,
	gpointer               data,
	GDestroyNotify         free_data,
	GError               **error
){
	AxisStorageProbeCall *call;
	GError *local = NULL;
	GThread *thread;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);
	g_return_val_if_fail(func != NULL, NULL);

	g_mutex_lock(&self->lock);
	if (g_hash_table_contains(self->abandoned_keys, key)) {
		g_mutex_unlock(&self->lock);
		if (free_data != NULL)
			free_data(data);
		g_set_error_literal(error, AXIS_STORAGE_PROBE_ERROR, AXIS_STORAGE_PROBE_ERROR_STUCK,
			"an earlier check has not finished after its time limit; the filesystem is "
			"not responding");
		return NULL;
	}
	if (self->running >= self->max_threads) {
		guint running = self->running;

		g_mutex_unlock(&self->lock);
		if (free_data != NULL)
			free_data(data);
		g_set_error(error, AXIS_STORAGE_PROBE_ERROR, AXIS_STORAGE_PROBE_ERROR_FULL,
			"%u filesystem checks are still running or stuck; not starting another", running);
		return NULL;
	}
	self->running++;
	g_mutex_unlock(&self->lock);

	call = g_new0(AxisStorageProbeCall, 1);
	call->ref_count = 2;
	call->probe = probe_ref(self);
	call->key = g_strdup(key);
	call->func = func;
	call->data = data;
	call->free_data = free_data;

	thread = g_thread_try_new("axis-storage", call_thread, call, &local);
	if (thread == NULL) {
		g_mutex_lock(&self->lock);
		self->running--;
		g_mutex_unlock(&self->lock);
		g_set_error(error, AXIS_STORAGE_PROBE_ERROR, AXIS_STORAGE_PROBE_ERROR_THREAD,
			"cannot start a filesystem check: %s", local->message);
		g_error_free(local);
		/* Both references: the thread never took its own. */
		call->ref_count = 1;
		axis_storage_probe_call_unref(call);
		return NULL;
	}
	g_thread_unref(thread);

	return call;
}

gboolean
axis_storage_probe_wait(
	AxisStorageProbeCall *call,
	gint64                deadline_us
){
	AxisStorageProbe *probe;
	gboolean done;

	g_return_val_if_fail(call != NULL, FALSE);

	probe = call->probe;
	g_mutex_lock(&probe->lock);
	while (!call->done) {
		if (!g_cond_wait_until(&probe->finished, &probe->lock, deadline_us) && !call->done) {
			if (!call->abandoned) {
				guint n = GPOINTER_TO_UINT(g_hash_table_lookup(probe->abandoned_keys, call->key));

				call->abandoned = TRUE;
				probe->abandoned++;
				g_hash_table_insert(probe->abandoned_keys, g_strdup(call->key),
					GUINT_TO_POINTER(n + 1));
			}
			break;
		}
	}
	done = call->done;
	g_mutex_unlock(&probe->lock);

	return done;
}

guint
axis_storage_probe_get_running(AxisStorageProbe *self)
{
	guint n;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->lock);
	n = self->running;
	g_mutex_unlock(&self->lock);

	return n;
}

guint
axis_storage_probe_get_abandoned(AxisStorageProbe *self)
{
	guint n;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->lock);
	n = self->abandoned;
	g_mutex_unlock(&self->lock);

	return n;
}
