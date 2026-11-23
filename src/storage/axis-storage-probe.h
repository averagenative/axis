/*
 * axis-storage-probe.h - filesystem work with a deadline, on a bounded set
 * of helper threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A filesystem that stops answering (a stale network mount, a dying USB
 * disk) blocks any thread that touches it, in the kernel, where it cannot
 * be interrupted.  So every touch of a volume runs here: a call runs its
 * function on a new helper thread and the caller waits for it until a
 * deadline.  A call that misses its deadline is abandoned: the caller
 * moves on (and reports the volume offline), while the helper thread
 * stays blocked until the kernel lets it go, then frees the call's data,
 * closing any descriptor the function opened.
 *
 * What a hang can cost is bounded twice:
 *
 *  - at most @max_threads calls run at once, abandoned ones included; a
 *    call beyond that fails at once;
 *  - a call whose key (a volume) still has an abandoned call running fails
 *    at once, without a thread: one hung disk holds at most one thread,
 *    however often it is checked.
 *
 * The refusals have their own error codes (below) so that a caller can
 * tell "this volume's filesystem is stuck" from "the helper is full of
 * other work"; both mean the volume cannot be looked at now.  The
 * function must only touch its data; the data must own everything it
 * holds (a descriptor is dup()ed in, never borrowed).
 *
 * Thread-safe throughout.
 */

#ifndef AXIS_STORAGE_PROBE_H
#define AXIS_STORAGE_PROBE_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_STORAGE_PROBE_ERROR (axis_storage_probe_error_quark())

/**
 * AxisStorageProbeError:
 * @AXIS_STORAGE_PROBE_ERROR_STUCK: the key has an abandoned call that is
 *   still running: its filesystem is not answering
 * @AXIS_STORAGE_PROBE_ERROR_FULL: @max_threads calls are running; nothing
 *   is known about this key
 * @AXIS_STORAGE_PROBE_ERROR_THREAD: no thread could be made
 */
typedef enum
{
	AXIS_STORAGE_PROBE_ERROR_STUCK,
	AXIS_STORAGE_PROBE_ERROR_FULL,
	AXIS_STORAGE_PROBE_ERROR_THREAD
} AxisStorageProbeError;

GQuark
axis_storage_probe_error_quark(void);

typedef struct _AxisStorageProbe     AxisStorageProbe;
typedef struct _AxisStorageProbeCall AxisStorageProbeCall;

typedef void (*AxisStorageProbeFunc)(gpointer data);

/* @max_threads: 1 or more */
AxisStorageProbe *
axis_storage_probe_new(guint max_threads);

/* Drops the caller's reference.  Calls still running keep the helper
 * alive until they finish; this never waits for them. */
void
axis_storage_probe_unref(AxisStorageProbe *self);

/**
 * axis_storage_probe_start:
 * @self: the helper
 * @key: what the call is about (a volume), for the per-key bound
 * @func: runs on the helper thread
 * @data: for @func; owned by the call from now on
 * @free_data: (nullable): frees @data once @func has returned and the
 *   caller is done with it, on whichever thread is last
 * @error: (nullable): return location for an error
 *
 * Fails (freeing @data at once) with %AXIS_STORAGE_PROBE_ERROR_STUCK when
 * @key has an abandoned call still running, %AXIS_STORAGE_PROBE_ERROR_FULL
 * when @max_threads calls are running, and
 * %AXIS_STORAGE_PROBE_ERROR_THREAD when no thread could be made.
 *
 * Returns: (transfer full) (nullable): the call, for
 *   axis_storage_probe_wait() and then axis_storage_probe_call_unref()
 */
AxisStorageProbeCall *
axis_storage_probe_start(
	AxisStorageProbe      *self,
	const gchar           *key,
	AxisStorageProbeFunc   func,
	gpointer               data,
	GDestroyNotify         free_data,
	GError               **error
);

/**
 * axis_storage_probe_wait:
 * @call: a call
 * @deadline_us: until when, on the monotonic clock
 *
 * Waits for the call's function to return.  When the deadline passes
 * first, the call is abandoned (see above) and its data must not be read.
 *
 * Returns: %TRUE if the function returned in time; its data may be read
 *   until axis_storage_probe_call_unref()
 */
gboolean
axis_storage_probe_wait(
	AxisStorageProbeCall *call,
	gint64                deadline_us
);

/* Returns: (transfer none): the call's data */
gpointer
axis_storage_probe_call_get_data(AxisStorageProbeCall *call);

void
axis_storage_probe_call_unref(AxisStorageProbeCall *call);

/* Calls whose function has not returned yet, abandoned ones included. */
guint
axis_storage_probe_get_running(AxisStorageProbe *self);

/* Calls that missed their deadline and have not returned yet. */
guint
axis_storage_probe_get_abandoned(AxisStorageProbe *self);

G_END_DECLS

#endif /* AXIS_STORAGE_PROBE_H */
