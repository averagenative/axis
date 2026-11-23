/*
 * axis-worker-store.h - the worker table: creating a worker (and its
 * token, shown once), revoking one, listing them with what they reported,
 * and asking one to measure its encoders again
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every function blocks on the database: call it from a worker thread.
 */

#ifndef AXIS_WORKER_STORE_H
#define AXIS_WORKER_STORE_H

#include <glib.h>

#include "api/axis-worker-tokens.h"
#include "db/axis-db-pool.h"
#include "transcode/axis-convert.h"

G_BEGIN_DECLS

/* The machine a worker's capabilities are stored under (encoder_tool), and
 * the worker id its jobs are held under (job.worker_id). */
#define AXIS_WORKER_MACHINE_PREFIX "worker-"
#define AXIS_WORKER_HOLDER_PREFIX  "worker:"

#define AXIS_WORKER_STORE_ERROR (axis_worker_store_error_quark())

typedef enum
{
	AXIS_WORKER_STORE_ERROR_INVALID,	/* the name is not one a worker may have */
	AXIS_WORKER_STORE_ERROR_EXISTS,		/* a worker of that name exists (revoked ones too) */
	AXIS_WORKER_STORE_ERROR_NOT_FOUND
} AxisWorkerStoreError;

GQuark
axis_worker_store_error_quark(void);

/* "worker-" + name, "worker:" + name */
gchar *
axis_worker_machine(const gchar *name);

gchar *
axis_worker_holder(const gchar *name);

/**
 * axis_worker_store_create:
 * @name: as typed: trimmed and lower-cased, then it must pass
 *   axis_worker_name_is_valid()
 * @by: the administrator's name
 * @token: (out) (transfer full): the token, the one and only time it is
 *   known: free it with axis_secret_free()
 *
 * Makes a worker with a new random token, stores only the token's SHA-256,
 * and adds the worker to @tokens.
 */
gboolean
axis_worker_store_create(
	AxisDbPool        *pool,
	AxisWorkerTokens  *tokens,
	const gchar       *name,
	const gchar       *by,
	gchar            **token,
	gint64            *id,
	GError           **error
);

/**
 * axis_worker_store_revoke:
 * @revoked: (out) (optional): whether it was not revoked before
 *
 * Revokes the worker's token in @tokens first, so that its very next
 * request is refused, then in the database.  Its jobs keep their leases
 * until they lapse (it can no longer renew them), and are then handed
 * back.
 */
gboolean
axis_worker_store_revoke(
	AxisDbPool        *pool,
	AxisWorkerTokens  *tokens,
	const gchar       *name,
	const gchar       *by,
	gboolean          *revoked,
	GError           **error
);

/**
 * axis_worker_store_request_measure:
 *
 * Queues a worker.capabilities job that only that worker can claim (one at
 * a time per worker).
 */
gboolean
axis_worker_store_request_measure(
	AxisDbPool   *pool,
	const gchar  *name,
	gint64       *job_id,
	gboolean     *created,
	GError      **error
);

/**
 * axis_worker_store_resume:
 * @resumed: (out) (optional): whether it was paused
 *
 * Lifts the pause of a worker whose encoder crashed: it takes work again
 * at its next claim.  The crashed job's number is kept (resumed_job), so
 * that the worker's next report of that crash does not pause it again.
 */
gboolean
axis_worker_store_resume(
	AxisDbPool   *pool,
	const gchar  *name,
	const gchar  *by,
	gboolean     *resumed,
	GError      **error
);

typedef struct
{
	gchar    *volume_name;
	gchar    *uuid;
	gchar    *path;
	gint64    proved_us;
	gboolean  keeps_identity;	/* FALSE: a mount that does not keep file identities */
} AxisWorkerVolumeRow;

typedef struct
{
	gint64   id;
	gchar   *kind;
	gdouble  progress;	/* -1 when none */
	gchar   *message;
	gint64   started_us;
} AxisWorkerJobRow;

/**
 * AxisWorkerInfo:
 *
 * One worker as the Workers page shows it.  Times are microseconds since
 * 1970 (0 for never); @seen_ago_ms is how long ago its last request was,
 * by the database's clock (-1 for never).
 */
typedef struct
{
	gint64                   id;
	gchar                   *name;
	gint64                   created_us;
	gchar                   *created_by;
	gint64                   revoked_us;
	gchar                   *revoked_by;
	gint64                   last_seen_us;
	gint64                   seen_ago_ms;
	gchar                   *last_address;
	gint64                   registered_us;
	gchar                   *worker_version;
	gchar                   *ffmpeg_version;
	gint                     max_jobs;
	GPtrArray               *volumes;	/* AxisWorkerVolumeRow */
	GPtrArray               *jobs;		/* AxisWorkerJobRow, running */
	AxisEncoderCapabilities *caps;		/* nullable */
	gint64                   paused_us;	/* 0: not paused */
	gint64                   paused_job;
	gchar                   *paused_reason;
	gint                     refused_protocol;	/* -1: none refused */
	gint64                   refused_protocol_us;
} AxisWorkerInfo;

void
axis_worker_info_free(AxisWorkerInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerInfo, axis_worker_info_free)

/* Every worker, by name. */
gboolean
axis_worker_store_list(
	AxisDbPool  *pool,
	GPtrArray  **workers,
	GError     **error
);

G_END_DECLS

#endif /* AXIS_WORKER_STORE_H */
