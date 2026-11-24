/*
 * axis-worker-api.c - the worker API
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-worker-api.h"

#include <string.h>

#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "db/axis-db-error.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library-name.h"
#include "storage/axis-volume-marker.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-convert-caps.h"
#include "web/axis-web-auth-private.h"

#define DEFAULT_REAP_MS        250
#define SEEN_WRITE_INTERVAL_US (G_GINT64_CONSTANT(10) * G_USEC_PER_SEC)
#define REFUSED_NOTE_US        (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)
#define ADDRESS_MAX            64

/* A lease this daemon granted or renewed, on its monotonic clock. */
typedef struct
{
	gint64   job;
	gchar    token[AXIS_VOLUME_UUID_LEN + 1];
	gint64   worker;
	gint64   expires_us;
	guint    busy;		/* requests for it being handled */
	gboolean expiring;	/* being handed back */
} Lease;

/* What this daemon knows of a worker since it started. */
typedef struct
{
	gint64 worker;
	guint  max_jobs;	/* 0 until it registers */
	guint  claiming;
	gint64 seen_written_us;
	gint64 refused_noted_us;	/* a request of another version written down */
} Known;

struct _AxisWorkerApi
{
	AxisDbPool          *pool;
	AxisJobRegistry     *registry;
	AxisWorkerTokens    *tokens;
	AxisEstimator       *estimator;
	AxisWorkerApiTiming  timing;
	AxisWorkerConvertHooks convert;
	gpointer             convert_data;
	AxisWorkerRegisteredFunc registered;	/* (nullable) main loop */
	gpointer             registered_data;

	GMutex               lock;
	GHashTable          *leases;	/* (lock) gint64 job -> Lease */
	GHashTable          *known;	/* (lock) gint64 worker -> Known */

	guint                pending;	/* main loop */
	gboolean             stopping;	/* main loop */
	guint                reap_id;
	gboolean             reaping;	/* main loop */
	gboolean             paused;	/* main loop: no lease is handed back */
	AxisJobQueue        *fence_source;	/* (nullable) set before serving */
};

/* The daemon's lock, as its job queue has it now; NULL without one. */
static const AxisJobFence *
api_fence(
	AxisWorkerApi *self,
	AxisJobFence  *fence
){
	if (self->fence_source == NULL || !axis_job_queue_get_fence(self->fence_source, fence))
		return NULL;
	return fence;
}

typedef enum
{
	OP_REGISTER,
	OP_CLAIM,
	OP_HEARTBEAT,
	OP_COMPLETE,
	OP_FAIL
} Op;

typedef struct
{
	guint  status;
	gchar *body;
} Reply;

typedef struct
{
	AxisWorkerApi *api;
	Op             op;
	HtmxResponse  *deferred;
	GBytes        *body;
	gint64         worker;
	gchar         *name;
	gchar         *holder;	/* "worker:" + name */
	gchar         *machine;	/* "worker-" + name */
	gchar         *address;
	Reply          reply;
} Work;

/* ── Replies ────────────────────────────────────────────────────── */

static void
reply_set(
	Reply       *reply,
	guint        status,
	gchar       *body
){
	g_free(reply->body);
	reply->status = status;
	reply->body = body;
}

static void
reply_error(
	Reply       *reply,
	guint        status,
	const gchar *word,
	const gchar *message
){
	reply_set(reply, status, axis_worker_error_to_json(word, message));
}

static void
reply_malformed(
	Reply       *reply,
	const gchar *problem
){
	reply_error(reply, 400, "malformed", problem != NULL ? problem : "the request is malformed");
}

static void
reply_lease(Reply *reply)
{
	reply_error(reply, 409, "lease", "the job is not held under this lease by this worker now");
}

/* A database failure: logged here, never said to the worker. */
static void
reply_busy(
	Work         *work,
	const GError *error
){
	g_warning("worker API: %s from worker %s could not be handled: %s",
		work->op == OP_REGISTER ? "register" : work->op == OP_CLAIM ? "claim" : work->op == OP_HEARTBEAT ?
		"heartbeat" : work->op == OP_COMPLETE ? "complete" : "fail", work->name,
		error != NULL ? error->message : "?");
	reply_error(&work->reply, 503, "busy", "the daemon cannot do this now; try again");
}

static const gchar *
body_data(
	Work  *work,
	gsize *length
){
	*length = 0;
	if (work->body == NULL)
		return NULL;
	return g_bytes_get_data(work->body, length);
}

/* ── Leases ─────────────────────────────────────────────────────── */

typedef enum
{
	LEASE_HELD,	/* in memory, alive: now busy */
	LEASE_UNKNOWN,	/* not in memory: the database decides (adoption) */
	LEASE_REFUSED	/* another worker's or token, expired, or being handed back */
} LeaseCheck;

static LeaseCheck
lease_enter(
	AxisWorkerApi *self,
	gint64         job,
	const gchar   *token,
	gint64         worker
){
	gint64 now = g_get_monotonic_time();
	LeaseCheck check;
	Lease *lease;

	g_mutex_lock(&self->lock);
	lease = g_hash_table_lookup(self->leases, &job);
	if (lease == NULL) {
		check = LEASE_UNKNOWN;
	} else if (lease->worker != worker || strcmp(lease->token, token) != 0 || lease->expiring ||
	           now >= lease->expires_us) {
		check = LEASE_REFUSED;
	} else {
		lease->busy++;
		check = LEASE_HELD;
	}
	g_mutex_unlock(&self->lock);
	return check;
}

static Lease *
lease_find_locked(
	AxisWorkerApi *self,
	gint64         job,
	const gchar   *token
){
	Lease *lease = g_hash_table_lookup(self->leases, &job);

	return lease != NULL && strcmp(lease->token, token) == 0 ? lease : NULL;
}

/* The request is done with the lease, which stays as it is. */
static void
lease_leave(
	AxisWorkerApi *self,
	gint64         job,
	const gchar   *token
){
	Lease *lease;

	g_mutex_lock(&self->lock);
	lease = lease_find_locked(self, job, token);
	if (lease != NULL && lease->busy > 0)
		lease->busy--;
	g_mutex_unlock(&self->lock);
}

/* The lease is over (ended, handed back, or found lost). */
static void
lease_drop(
	AxisWorkerApi *self,
	gint64         job,
	const gchar   *token
){
	g_mutex_lock(&self->lock);
	if (lease_find_locked(self, job, token) != NULL)
		g_hash_table_remove(self->leases, &job);
	g_mutex_unlock(&self->lock);
}

/* Granted or renewed now: it lasts lease_ms from this moment.  Adopts a
 * lease not in memory. */
static void
lease_renew(
	AxisWorkerApi *self,
	gint64         job,
	const gchar   *token,
	gint64         worker,
	gboolean       entered
){
	gint64 expires = g_get_monotonic_time() + (gint64)self->timing.lease_ms * 1000;
	Lease *lease;

	g_mutex_lock(&self->lock);
	lease = lease_find_locked(self, job, token);
	if (lease == NULL) {
		gint64 *key = g_new(gint64, 1);

		lease = g_new0(Lease, 1);
		*key = job;
		lease->job = job;
		g_strlcpy(lease->token, token, sizeof(lease->token));
		lease->worker = worker;
		g_hash_table_replace(self->leases, key, lease);
	} else if (entered && lease->busy > 0) {
		lease->busy--;
	}
	lease->expires_us = expires;
	g_mutex_unlock(&self->lock);
}

static Known *
known_locked(
	AxisWorkerApi *self,
	gint64         worker
){
	Known *known = g_hash_table_lookup(self->known, &worker);

	if (known == NULL) {
		gint64 *key = g_new(gint64, 1);

		*key = worker;
		known = g_new0(Known, 1);
		known->worker = worker;
		g_hash_table_insert(self->known, key, known);
	}
	return known;
}

/* Writes down when the worker was last heard from, at most every ten
 * seconds. */
static void
touch(
	Work       *work,
	AxisDbConn *conn
){
	AxisWorkerApi *self = work->api;
	gint64 now = g_get_monotonic_time();
	g_autofree gchar *id = NULL;
	g_autofree gchar *address = NULL;
	const gchar *params[2];
	gboolean due;
	Known *known;

	g_mutex_lock(&self->lock);
	known = known_locked(self, work->worker);
	due = known->seen_written_us == 0 || now - known->seen_written_us >= SEEN_WRITE_INTERVAL_US;
	if (due)
		known->seen_written_us = now;
	g_mutex_unlock(&self->lock);
	if (!due)
		return;
	id = g_strdup_printf("%" G_GINT64_FORMAT, work->worker);
	address = axis_library_text_clean(work->address != NULL && work->address[0] != '\0' ? work->address : "?",
		ADDRESS_MAX);
	params[0] = id;
	params[1] = address;
	(void)axis_db_conn_exec(conn, "UPDATE worker SET last_seen_at = now(), last_address = $2::text"
		" WHERE id = $1::bigint", 2, params, NULL, NULL);
}

/* ── register ───────────────────────────────────────────────────── */

static void
op_register(Work *work)
{
	AxisWorkerApi *self = work->api;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisWorkerRegister) request = NULL;
	g_autoptr(AxisWorkerRegistered) answer = axis_worker_registered_new();
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(AxisDbResult) updated = NULL;
	g_autoptr(AxisDbResult) before = NULL;
	g_autoptr(GHashTable) known_volumes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autofree gchar *problem = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->worker);
	g_autofree gchar *max_jobs = NULL;
	g_autofree gchar *address = NULL;
	GString *uuids = g_string_new("{");
	const gchar *params[5];
	const gchar *data;
	AxisDbConn *conn;
	gint64 was_paused = 0;
	guint accepted = 0;
	guint without_identity = 0;
	gsize length;
	guint i;

	data = body_data(work, &length);
	request = axis_worker_register_parse(data, length, &problem);
	if (request == NULL) {
		g_string_free(uuids, TRUE);
		reply_malformed(&work->reply, problem);
		return;
	}
	for (i = 0; i < request->volumes->len; i++) {
		const AxisWorkerVolumeSeen *v = g_ptr_array_index(request->volumes, i);

		/* UUIDs were checked: hexadecimal digits and dashes only. */
		g_string_append_printf(uuids, "%s%s", i > 0 ? "," : "", v->uuid);
	}
	g_string_append_c(uuids, '}');

	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL) {
		g_string_free(uuids, TRUE);
		reply_busy(work, error);
		return;
	}
	params[0] = uuids->str;
	if (!axis_db_conn_exec(conn, "SELECT uuid::text, id FROM volume WHERE uuid = ANY ($1::uuid[])", 1, params, &found,
	                       &error))
		goto fail;
	for (i = 0; i < axis_db_result_get_n_rows(found); i++)
		g_hash_table_insert(known_volumes, g_strdup(axis_db_result_get_text(found, i, 0)),
			g_strdup(axis_db_result_get_text(found, i, 1)));

	max_jobs = g_strdup_printf("%u", request->max_jobs);
	address = axis_library_text_clean(work->address != NULL && work->address[0] != '\0' ? work->address : "?",
		ADDRESS_MAX);
	if (!axis_db_conn_begin(conn, &error))
		goto fail;
	params[0] = id;
	params[1] = request->worker_version;
	params[2] = request->caps->tool_version;
	params[3] = max_jobs;
	params[4] = address;
	if (!axis_db_conn_exec(conn, "SELECT coalesce(paused_job, 0) FROM worker WHERE id = $1::bigint FOR UPDATE", 1,
	        params, &before, &error))
		goto rollback;
	if (axis_db_result_get_n_rows(before) == 1)
		axis_db_result_get_int64(before, 0, 0, &was_paused);
	/* Registering clears a pause: a worker that was restarted has forgotten
	 * its crash; one that only registered again (its volumes changed, the
	 * daemon restarted) says so again at its next claim. */
	if (!axis_db_conn_exec(conn, "UPDATE worker SET registered_at = now(), worker_version = $2::text,"
	        " ffmpeg_version = $3::text, max_jobs = $4::integer, last_seen_at = now(), last_address = $5::text,"
	        " paused_at = NULL, paused_job = NULL, paused_reason = NULL,"
	        " refused_protocol = NULL, refused_protocol_at = NULL"
	        " WHERE id = $1::bigint AND revoked_at IS NULL RETURNING id", 5, params, &updated, &error))
		goto rollback;
	if (axis_db_result_get_n_rows(updated) != 1) {
		/* Revoked while the request was on its way. */
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		g_string_free(uuids, TRUE);
		reply_error(&work->reply, 401, "unauthorized", "the token is not a worker's, or it was revoked");
		return;
	}
	if (!axis_db_conn_exec(conn, "DELETE FROM worker_volume WHERE worker_id = $1::bigint", 1, params, NULL, &error))
		goto rollback;
	for (i = 0; i < request->volumes->len; i++) {
		const AxisWorkerVolumeSeen *v = g_ptr_array_index(request->volumes, i);
		const gchar *volume = g_hash_table_lookup(known_volumes, v->uuid);
		const gchar *row[4];

		if (volume == NULL) {
			axis_worker_registered_add_volume(answer, v->uuid, FALSE,
				"the daemon has no volume with this identifier");
			continue;
		}
		row[0] = id;
		row[1] = volume;
		row[2] = v->path;
		row[3] = v->keeps_identity ? "true" : "false";
		if (!axis_db_conn_exec(conn, "INSERT INTO worker_volume (worker_id, volume_id, path, keeps_identity)"
		        " VALUES ($1::bigint, $2::bigint, $3::text, $4::boolean)", 4, row, NULL, &error))
			goto rollback;
		axis_worker_registered_add_volume(answer, v->uuid, TRUE, NULL);
		accepted++;
		if (!v->keeps_identity)
			without_identity++;
	}
	if (!axis_db_conn_commit(conn, &error))
		goto rollback;

	/* Its capabilities, where the daemon's own are kept. */
	if (!axis_convert_capabilities_store(conn, work->machine, request->caps, &error))
		goto fail;
	axis_db_pool_release(self->pool, conn);
	g_string_free(uuids, TRUE);

	g_mutex_lock(&self->lock);
	known_locked(self, work->worker)->max_jobs = request->max_jobs;
	known_locked(self, work->worker)->seen_written_us = g_get_monotonic_time();
	g_mutex_unlock(&self->lock);

	g_message("audit: worker %s registered from %s: axis-worker %s, ffmpeg %s, %u of %u volumes accepted "
		"(%u through a mount that does not keep file identities), %u jobs at once", work->name, address,
		request->worker_version, request->caps->tool_version, accepted, request->volumes->len, without_identity,
		request->max_jobs);
	if (was_paused > 0)
		g_message("audit: worker %s registered again: the pause after its encoder crashed on job %" G_GINT64_FORMAT
			" is lifted (a worker that still holds it says so at its next claim)", work->name, was_paused);
	answer->worker = g_strdup(work->name);
	answer->lease_ms = self->timing.lease_ms;
	answer->heartbeat_ms = self->timing.heartbeat_ms;
	answer->poll_ms = self->timing.poll_ms;
	reply_set(&work->reply, 200, axis_worker_registered_to_json(answer));
	return;

rollback:
	axis_db_conn_rollback(conn, NULL);
fail:
	axis_db_pool_release(self->pool, conn);
	g_string_free(uuids, TRUE);
	reply_busy(work, error);
}

/* ── claim ──────────────────────────────────────────────────────── */

/* The JSON array of the volumes the worker reported proved just now that
 * it also registered.  A volume that has no marker yet is never among
 * them: a worker proves a disk by its marker, so it cannot have proved that
 * one (a marker it read there was written so recently that the daemon has
 * not recorded it yet: its next check does, and the volume is offered
 * then). */
static gboolean
claim_volumes(
	Work                   *work,
	AxisDbConn             *conn,
	const AxisWorkerClaim  *request,
	gchar                 **json,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->worker);
	GString *wanted = g_string_new("{");
	GString *out;
	const gchar *params[2];
	guint i;
	gboolean ok;

	for (i = 0; request->volumes != NULL && request->volumes[i] != NULL; i++)
		g_string_append_printf(wanted, "%s%s", i > 0 ? "," : "", request->volumes[i]);
	g_string_append_c(wanted, '}');
	params[0] = id;
	params[1] = wanted->str;
	ok = axis_db_conn_exec(conn, "SELECT v.uuid::text FROM worker_volume wv JOIN volume v ON v.id = wv.volume_id"
		" WHERE wv.worker_id = $1::bigint AND v.uuid = ANY ($2::uuid[]) AND v.identity = 'marker' ORDER BY 1", 2,
		params, &result, error);
	g_string_free(wanted, TRUE);
	if (!ok)
		return FALSE;
	out = g_string_new("[");
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_string_append_printf(out, "%s\"%s\"", i > 0 ? "," : "", axis_db_result_get_text(result, i, 0));
	g_string_append_c(out, ']');
	*json = g_string_free(out, FALSE);
	return TRUE;
}

/*
 * A worker whose encoder crashed says so in every claim (the job, and what
 * happened) until it is resumed: the first such claim pauses it, unless a
 * person resumed it from that very crash already.  Then, whatever the claim
 * said, *@paused is the daemon's sentence while the worker is held paused.
 */
static gboolean
claim_pause(
	Work                   *work,
	AxisDbConn             *conn,
	const AxisWorkerClaim  *request,
	gchar                 **paused,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->worker);
	const gchar *params[3];

	*paused = NULL;
	params[0] = id;
	if (request->paused_job > 0 && request->paused_reason != NULL) {
		g_autoptr(AxisDbResult) recorded = NULL;
		g_autofree gchar *job = g_strdup_printf("%" G_GINT64_FORMAT, request->paused_job);

		params[1] = job;
		params[2] = request->paused_reason;
		if (!axis_db_conn_exec(conn, "UPDATE worker SET paused_at = now(), paused_job = $2::bigint,"
		        " paused_reason = $3::text WHERE id = $1::bigint AND paused_at IS NULL"
		        " AND resumed_job IS DISTINCT FROM $2::bigint RETURNING id", 3, params, &recorded, error))
			return FALSE;
		if (axis_db_result_get_n_rows(recorded) == 1) {
			g_message("audit: worker %s paused: its encoder crashed on job %" G_GINT64_FORMAT " (%s); it takes no work "
				"until it is resumed on the Workers page or restarted", work->name, request->paused_job,
				request->paused_reason);
		} else {
			/* The claim that reports the crash a person resumed it from is
			 * answered "not paused" once, which the worker takes for the
			 * resume and forgets the crash: the resume is used up then, so
			 * that a later crash on the same job (a check job or a sample
			 * measurement is tried again under its id) pauses it again.  An
			 * answer lost on the way means it is paused again, and resumed
			 * once more: the safe side. */
			if (!axis_db_conn_exec(conn, "UPDATE worker SET resumed_job = NULL WHERE id = $1::bigint"
			        " AND paused_at IS NULL AND resumed_job = $2::bigint", 2, params, NULL, error))
				return FALSE;
		}
	}
	if (!axis_db_conn_exec(conn, "SELECT paused_job, paused_reason FROM worker WHERE id = $1::bigint"
	        " AND paused_at IS NOT NULL", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*paused = g_strdup_printf("Its encoder crashed on job %s (%s). It takes no work until it is resumed on the "
			"Workers page, or restarted.", axis_db_result_get_text(result, 0, 0),
			axis_db_result_get_text(result, 0, 1));
	return TRUE;
}

static void
op_claim(Work *work)
{
	AxisWorkerApi *self = work->api;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisWorkerClaim) request = NULL;
	g_autoptr(AxisJobClaim) claim = NULL;
	g_autoptr(GPtrArray) kinds = g_ptr_array_new();
	g_autofree gchar *problem = NULL;
	g_autofree gchar *volumes = NULL;
	g_autofree gchar *paused = NULL;
	AxisJobRemoteFilter filter;
	AxisJobFence fence;
	AxisWorkerJob job;
	GHashTableIter iter;
	gpointer value;
	const gchar *data;
	AxisDbConn *conn;
	gsize length;
	guint held = 0;
	guint max_jobs = 0;
	gboolean busy;
	guint i;

	data = body_data(work, &length);
	request = axis_worker_claim_parse(data, length, &problem);
	if (request == NULL) {
		reply_malformed(&work->reply, problem);
		return;
	}
	/* Only remote kinds, of modules that are on. */
	for (i = 0; request->kinds[i] != NULL; i++) {
		if (axis_job_registry_is_remote(self->registry, request->kinds[i]) &&
		    axis_job_registry_kind_claimable(self->registry, request->kinds[i]))
			g_ptr_array_add(kinds, request->kinds[i]);
	}
	g_ptr_array_add(kinds, NULL);

	g_mutex_lock(&self->lock);
	max_jobs = known_locked(self, work->worker)->max_jobs;
	g_mutex_unlock(&self->lock);
	if (max_jobs == 0) {
		reply_error(&work->reply, 409, "unregistered", "register first");
		return;
	}
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL) {
		reply_busy(work, error);
		return;
	}
	touch(work, conn);
	/* Every answer says whether the worker is held paused, busy or not: the
	 * worker takes "not paused" for a resume. */
	if (!claim_pause(work, conn, request, &paused, &error)) {
		axis_db_pool_release(self->pool, conn);
		reply_busy(work, error);
		return;
	}
	if (paused != NULL) {
		axis_db_pool_release(self->pool, conn);
		reply_set(&work->reply, 200, axis_worker_claimed_to_json(NULL, paused));
		return;
	}

	/* At most as many at once as it said it runs. */
	g_mutex_lock(&self->lock);
	max_jobs = known_locked(self, work->worker)->max_jobs;
	g_hash_table_iter_init(&iter, self->leases);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const Lease *lease = value;

		if (lease->worker == work->worker)
			held++;
	}
	busy = max_jobs == 0 || held + known_locked(self, work->worker)->claiming >= max_jobs || kinds->len == 1;
	if (!busy)
		known_locked(self, work->worker)->claiming++;
	g_mutex_unlock(&self->lock);
	if (busy) {
		axis_db_pool_release(self->pool, conn);
		reply_set(&work->reply, 200, axis_worker_claimed_to_json(NULL, NULL));
		return;
	}

	if (!claim_volumes(work, conn, request, &volumes, &error))
		goto release;
	filter.worker = work->name;
	filter.machine = work->machine;
	filter.volumes_json = volumes;
	/* Fenced by the daemon's lock: a daemon that lost it hands out nothing
	 * (its settings, read once, may be stale beside the holder's). */
	if (!axis_job_claim_remote_fenced(conn, AXIS_WORKER_QUEUE, (const gchar *const *)kinds->pdata, work->holder,
	                                  &filter, self->timing.lease_ms, api_fence(self, &fence), &claim, &error))
		goto release;

	memset(&job, 0, sizeof(job));
	if (claim != NULL) {
		job.id = claim->id;
		job.kind = claim->kind;
		job.lease_token = claim->lease_token;
		job.lease_ms = self->timing.lease_ms;
		job.heartbeat_ms = self->timing.heartbeat_ms;
		job.attempt = MIN(claim->attempt, (guint)AXIS_WORKER_MAX_ATTEMPT);
		if (strcmp(claim->kind, AXIS_WORKER_KIND_SAMPLE) == 0) {
			g_autoptr(GError) local = NULL;

			if (self->estimator == NULL ||
			    !axis_estimator_describe_remote_sample(claim->payload, &job.sample, &local)) {
				/* A job nobody can run: it fails now, for good. */
				g_warning("worker API: job %" G_GINT64_FORMAT " cannot be described to worker %s: %s", claim->id,
					work->name, local != NULL ? local->message : "no estimator");
				(void)axis_job_fail(conn, claim->id, claim->lease_token, "the job's plan could not be read",
					TRUE, 0, 0, NULL, NULL);
				g_clear_pointer(&claim, axis_job_claim_free);
			}
		} else if (strcmp(claim->kind, AXIS_WORKER_KIND_CONVERT) == 0) {
			g_autoptr(GError) local = NULL;
			g_autofree gchar *why = NULL;
			gboolean permanent = TRUE;

			if (self->convert.describe == NULL) {
				why = g_strdup("this daemon does not convert files");
			} else if (!self->convert.describe(self->convert_data, conn, claim, work->name, work->machine, &job.convert,
			                                   &why, &permanent, &local)) {
				/* Not the job's fault: it goes back as it was. */
				g_warning("worker API: job %" G_GINT64_FORMAT " cannot be described to worker %s now: %s", claim->id,
					work->name, local != NULL ? local->message : "?");
				(void)axis_job_hand_back(conn, claim->id, claim->lease_token, NULL, NULL);
				g_clear_pointer(&claim, axis_job_claim_free);
			}
			if (claim != NULL && job.convert == NULL) {
				g_message("worker API: job %" G_GINT64_FORMAT " is not given to worker %s: %s", claim->id, work->name,
					why != NULL ? why : "?");
				(void)axis_job_fail(conn, claim->id, claim->lease_token, why != NULL ? why : "it cannot be done", permanent,
					self->timing.backoff_base_ms != 0 ? self->timing.backoff_base_ms : AXIS_JOB_DEFAULT_BACKOFF_BASE_MS,
					self->timing.backoff_cap_ms != 0 ? self->timing.backoff_cap_ms : AXIS_JOB_DEFAULT_BACKOFF_CAP_MS,
					NULL, NULL);
				g_clear_pointer(&claim, axis_job_claim_free);
			}
		} else if (strcmp(claim->kind, AXIS_WORKER_KIND_VERIFY) == 0) {
			g_autoptr(GError) local = NULL;
			g_autofree gchar *why = NULL;
			gboolean permanent = TRUE;

			if (self->convert.describe_verify == NULL) {
				why = g_strdup("this daemon does not check converted files");
			} else if (!self->convert.describe_verify(self->convert_data, conn, claim, work->name, &job.verify, &why,
			                                          &permanent, &local)) {
				g_warning("worker API: job %" G_GINT64_FORMAT " cannot be described to worker %s now: %s", claim->id,
					work->name, local != NULL ? local->message : "?");
				(void)axis_job_hand_back(conn, claim->id, claim->lease_token, NULL, NULL);
				g_clear_pointer(&claim, axis_job_claim_free);
			}
			if (claim != NULL && job.verify == NULL) {
				g_message("worker API: job %" G_GINT64_FORMAT " is not given to worker %s: %s", claim->id, work->name,
					why != NULL ? why : "?");
				(void)axis_job_fail(conn, claim->id, claim->lease_token, why != NULL ? why : "it cannot be done", permanent,
					self->timing.backoff_base_ms != 0 ? self->timing.backoff_base_ms : AXIS_JOB_DEFAULT_BACKOFF_BASE_MS,
					self->timing.backoff_cap_ms != 0 ? self->timing.backoff_cap_ms : AXIS_JOB_DEFAULT_BACKOFF_CAP_MS,
					NULL, NULL);
				g_clear_pointer(&claim, axis_job_claim_free);
			}
		}
	}
	axis_db_pool_release(self->pool, conn);

	g_mutex_lock(&self->lock);
	known_locked(self, work->worker)->claiming--;
	g_mutex_unlock(&self->lock);
	if (claim == NULL) {
		reply_set(&work->reply, 200, axis_worker_claimed_to_json(NULL, NULL));
		return;
	}
	lease_renew(self, claim->id, claim->lease_token, work->worker, FALSE);
	g_message("worker %s claimed job %" G_GINT64_FORMAT " (%s), attempt %u", work->name, claim->id, claim->kind,
		claim->attempt);
	reply_set(&work->reply, 200, axis_worker_claimed_to_json(&job, NULL));
	axis_sample_description_free(job.sample);
	axis_convert_work_free(job.convert);
	axis_verify_work_free(job.verify);
	return;

release:
	axis_db_pool_release(self->pool, conn);
	g_mutex_lock(&self->lock);
	known_locked(self, work->worker)->claiming--;
	g_mutex_unlock(&self->lock);
	reply_busy(work, error);
}

/* ── heartbeat ──────────────────────────────────────────────────── */

/* Ends a job the worker holds, in one transaction that locks it first:
 * @how is 'f' (fail), 'b' (hand back) or 'c' (cancelled: fail, which ends
 * it as cancelled). */
static gboolean
end_held(
	Work          *work,
	AxisDbConn    *conn,
	gint64         job,
	const gchar   *token,
	gchar          how,
	const gchar   *message,
	gboolean       permanent,
	AxisJobState  *state,
	GError       **error
){
	AxisWorkerApi *self = work->api;
	guint base = self->timing.backoff_base_ms != 0 ? self->timing.backoff_base_ms : AXIS_JOB_DEFAULT_BACKOFF_BASE_MS;
	guint cap = self->timing.backoff_cap_ms != 0 ? self->timing.backoff_cap_ms : AXIS_JOB_DEFAULT_BACKOFF_CAP_MS;

	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!axis_job_lock_remote(conn, job, token, work->holder, NULL, NULL, error))
		goto rollback;
	if (how == 'b') {
		if (!axis_job_hand_back(conn, job, token, state, error))
			goto rollback;
	} else if (!axis_job_fail(conn, job, token, message, permanent, base, cap, state, error)) {
		goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	return TRUE;

rollback:
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

static void
op_heartbeat(Work *work)
{
	AxisWorkerApi *self = work->api;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisWorkerHeartbeat) request = NULL;
	g_autofree gchar *problem = NULL;
	AxisJobProgress progress;
	AxisWorkerBeat beat;
	LeaseCheck check;
	const gchar *data;
	AxisDbConn *conn;
	gboolean cancel = FALSE;
	gsize length;

	data = body_data(work, &length);
	request = axis_worker_heartbeat_parse(data, length, &problem);
	if (request == NULL) {
		reply_malformed(&work->reply, problem);
		return;
	}
	check = lease_enter(self, request->job, request->lease_token, work->worker);
	if (check == LEASE_REFUSED) {
		reply_lease(&work->reply);
		return;
	}
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL) {
		if (check == LEASE_HELD)
			lease_leave(self, request->job, request->lease_token);
		reply_busy(work, error);
		return;
	}
	touch(work, conn);
	progress.fraction = request->progress;
	progress.message = request->message;
	if (!axis_job_heartbeat_remote(conn, request->job, request->lease_token, work->holder, self->timing.lease_ms,
	                               request->has_progress ? &progress : NULL, &cancel, &error)) {
		axis_db_pool_release(self->pool, conn);
		if (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST)) {
			lease_drop(self, request->job, request->lease_token);
			reply_lease(&work->reply);
			return;
		}
		if (check == LEASE_HELD)
			lease_leave(self, request->job, request->lease_token);
		reply_busy(work, error);
		return;
	}
	memset(&beat, 0, sizeof(beat));
	if (cancel) {
		/* Cancelled: the job ends here, and the worker is told to stop. */
		g_clear_error(&error);
		if (!end_held(work, conn, request->job, request->lease_token, 'c', "cancelled while a worker ran it",
		              FALSE, NULL, &error) && !g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST))
			g_warning("worker API: cancelled job %" G_GINT64_FORMAT " could not be ended: %s", request->job,
				error->message);
		axis_db_pool_release(self->pool, conn);
		lease_drop(self, request->job, request->lease_token);
		g_message("worker %s: job %" G_GINT64_FORMAT " was cancelled; told to stop", work->name, request->job);
		beat.stop = TRUE;
		beat.reason = (gchar *)"cancelled";
		reply_set(&work->reply, 200, axis_worker_beat_to_json(&beat));
		return;
	}
	axis_db_pool_release(self->pool, conn);
	lease_renew(self, request->job, request->lease_token, work->worker, check == LEASE_HELD);
	beat.lease_ms = self->timing.lease_ms;
	reply_set(&work->reply, 200, axis_worker_beat_to_json(&beat));
}

/* ── complete ───────────────────────────────────────────────────── */

/* Whether a worker.capabilities job's payload is for @name (or anyone). */
static gboolean
capabilities_for(
	const gchar *payload,
	const gchar *name
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonObject *object;
	JsonObject *requires;
	const gchar *worker;

	if (payload == NULL || !json_parser_load_from_data(parser, payload, -1, NULL))
		return FALSE;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return FALSE;
	object = json_node_get_object(root);
	if (!json_object_has_member(object, "requires"))
		return TRUE;
	if (!JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "requires")))
		return FALSE;
	requires = json_object_get_object_member(object, "requires");
	worker = json_object_get_string_member_with_default(requires, "worker", NULL);
	return worker == NULL || g_strcmp0(worker, name) == 0;
}

static void
op_complete(Work *work)
{
	AxisWorkerApi *self = work->api;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisWorkerComplete) request = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *payload = NULL;
	AxisWorkerCompleted answer;
	LeaseCheck check;
	const gchar *data;
	AxisDbConn *conn;
	gsize length;

	data = body_data(work, &length);
	request = axis_worker_complete_parse(data, length, &problem);
	if (request == NULL) {
		reply_malformed(&work->reply, problem);
		return;
	}
	check = lease_enter(self, request->job, request->lease_token, work->worker);
	if (check == LEASE_REFUSED) {
		reply_lease(&work->reply);
		return;
	}
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL)
		goto busy;
	touch(work, conn);
	memset(&answer, 0, sizeof(answer));
	if (!axis_db_conn_begin(conn, &error))
		goto release;
	/* The job is the worker's now, and stays so until this ends. */
	if (!axis_job_lock_remote(conn, request->job, request->lease_token, work->holder, &kind, &payload, &error))
		goto rollback;
	if (g_strcmp0(kind, request->kind) != 0) {
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		if (check == LEASE_HELD)
			lease_leave(self, request->job, request->lease_token);
		reply_malformed(&work->reply, "the result is of another kind than the job");
		return;
	}
	if (request->convert != NULL) {
		if (self->convert.encoded == NULL) {
			g_set_error_literal(&error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID, "this daemon does not convert files");
			goto rollback;
		}
		if (!self->convert.encoded(self->convert_data, conn, request->job, request->convert, work->name, &error))
			goto rollback;
	} else if (request->verify != NULL) {
		if (self->convert.verified == NULL) {
			g_set_error_literal(&error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID, "this daemon does not check converted "
				"files");
			goto rollback;
		}
		if (!self->convert.verified(self->convert_data, conn, request->job, request->verify, work->name, &error)) {
			if (g_error_matches(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED)) {
				/* Measurements that do not fit the job: refused, nothing
				 * kept; the lease runs out and the job is tried again. */
				axis_db_conn_rollback(conn, NULL);
				axis_db_pool_release(self->pool, conn);
				if (check == LEASE_HELD)
					lease_leave(self, request->job, request->lease_token);
				g_warning("worker API: worker %s's measurements for job %" G_GINT64_FORMAT " are refused: %s",
					work->name, request->job, error->message);
				reply_malformed(&work->reply, error->message);
				return;
			}
			goto rollback;
		}
	} else if (request->sample != NULL) {
		if (self->estimator == NULL) {
			g_set_error_literal(&error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "no estimator");
			goto rollback;
		}
		if (!axis_estimator_record_remote_sample(self->estimator, conn, request->job, payload, request->sample,
		                                         work->name, &answer.recorded, &answer.refused, &error)) {
			if (g_error_matches(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_INVALID)) {
				axis_db_conn_rollback(conn, NULL);
				axis_db_pool_release(self->pool, conn);
				if (check == LEASE_HELD)
					lease_leave(self, request->job, request->lease_token);
				reply_malformed(&work->reply, error->message);
				return;
			}
			goto rollback;
		}
	} else if (!capabilities_for(payload, work->name)) {
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		lease_drop(self, request->job, request->lease_token);
		reply_lease(&work->reply);
		return;
	}
	if (!axis_job_succeed(conn, request->job, request->lease_token, &error))
		goto rollback;
	if (!axis_db_conn_commit(conn, &error))
		goto rollback;

	if (request->caps != NULL) {
		g_autoptr(GError) local = NULL;
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->worker);
		const gchar *params[2];

		params[0] = id;
		params[1] = request->caps->tool_version;
		if (!axis_convert_capabilities_store(conn, work->machine, request->caps, &local) ||
		    !axis_db_conn_exec(conn, "UPDATE worker SET ffmpeg_version = $2::text WHERE id = $1::bigint", 2, params,
		                       NULL, &local))
			g_warning("worker API: the capabilities worker %s measured could not be stored: %s", work->name,
				local->message);
	}
	axis_db_pool_release(self->pool, conn);
	lease_drop(self, request->job, request->lease_token);
	g_message("worker %s completed job %" G_GINT64_FORMAT " (%s): %u recorded, %u refused", work->name, request->job,
		request->kind, answer.recorded, answer.refused);
	reply_set(&work->reply, 200, axis_worker_completed_to_json(&answer));
	return;

rollback:
	axis_db_conn_rollback(conn, NULL);
release:
	axis_db_pool_release(self->pool, conn);
	if (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST)) {
		lease_drop(self, request->job, request->lease_token);
		reply_lease(&work->reply);
		return;
	}
busy:
	if (check == LEASE_HELD)
		lease_leave(self, request->job, request->lease_token);
	reply_busy(work, error);
}

/* ── fail ───────────────────────────────────────────────────────── */

static void
op_fail(Work *work)
{
	AxisWorkerApi *self = work->api;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisWorkerFail) request = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *message = NULL;
	AxisJobState state = AXIS_JOB_STATE_QUEUED;
	LeaseCheck check;
	const gchar *data;
	AxisDbConn *conn;
	gsize length;

	data = body_data(work, &length);
	request = axis_worker_fail_parse(data, length, &problem);
	if (request == NULL) {
		reply_malformed(&work->reply, problem);
		return;
	}
	check = lease_enter(self, request->job, request->lease_token, work->worker);
	if (check == LEASE_REFUSED) {
		reply_lease(&work->reply);
		return;
	}
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL) {
		if (check == LEASE_HELD)
			lease_leave(self, request->job, request->lease_token);
		reply_busy(work, error);
		return;
	}
	touch(work, conn);
	message = g_strdup_printf("worker %s: %s", work->name, request->error);
	if (!end_held(work, conn, request->job, request->lease_token, request->stopping ? 'b' : 'f', message,
	              !request->retry, &state, &error)) {
		axis_db_pool_release(self->pool, conn);
		if (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST)) {
			lease_drop(self, request->job, request->lease_token);
			reply_lease(&work->reply);
			return;
		}
		if (check == LEASE_HELD)
			lease_leave(self, request->job, request->lease_token);
		reply_busy(work, error);
		return;
	}
	axis_db_pool_release(self->pool, conn);
	lease_drop(self, request->job, request->lease_token);
	g_message("worker %s %s job %" G_GINT64_FORMAT ": now %s", work->name,
		request->stopping ? "gave back" : "failed", request->job, axis_job_state_to_string(state));
	reply_set(&work->reply, 200, axis_worker_failed_to_json(axis_job_state_to_string(state)));
}

/* ── Dispatch ───────────────────────────────────────────────────── */

static void
work_free(Work *work)
{
	g_clear_object(&work->deferred);
	if (work->body != NULL)
		g_bytes_unref(work->body);
	g_free(work->name);
	g_free(work->holder);
	g_free(work->machine);
	g_free(work->address);
	g_free(work->reply.body);
	g_free(work);
}

/*
 * A request of another protocol version is refused before it is read: in
 * its own version when this daemon can write that (an older axis-worker
 * reads the refusal and logs it), with the word every version knows, and a
 * sentence naming both versions.  The worker's row says so, for the Workers
 * page (written at most once a minute per worker).
 */
static gboolean
refuse_other_version(Work *work)
{
	AxisWorkerApi *self = work->api;
	g_autofree gchar *message = NULL;
	const gchar *data;
	gboolean note = FALSE;
	guint version = 0;
	gsize length;

	data = body_data(work, &length);
	if (!axis_worker_message_protocol(data, length, &version) || version == AXIS_WORKER_PROTOCOL)
		return FALSE;
	message = axis_worker_protocol_refusal(version);
	reply_set(&work->reply, 400, axis_worker_error_to_json_version(version, "malformed", message));

	g_mutex_lock(&self->lock);
	{
		Known *known = known_locked(self, work->worker);
		gint64 now = g_get_monotonic_time();

		if (known->refused_noted_us == 0 || now - known->refused_noted_us >= REFUSED_NOTE_US) {
			known->refused_noted_us = now;
			note = TRUE;
		}
	}
	g_mutex_unlock(&self->lock);
	if (note) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->worker);
		g_autofree gchar *seen = g_strdup_printf("%u", version);
		g_autofree gchar *address = axis_library_text_clean(work->address != NULL && work->address[0] != '\0' ?
			work->address : "?", ADDRESS_MAX);
		const gchar *params[3];
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, &error);

		g_message("worker API: worker %s speaks worker protocol %u and this daemon %u: its requests are refused "
			"until it runs the axis-worker of the daemon's version", work->name, version, AXIS_WORKER_PROTOCOL);
		params[0] = id;
		params[1] = seen;
		params[2] = address;
		if (conn != NULL) {
			if (!axis_db_conn_exec(conn, "UPDATE worker SET refused_protocol = $2::integer, refused_protocol_at = now(),"
			        " last_seen_at = now(), last_address = $3::text WHERE id = $1::bigint", 3, params, NULL, &error))
				g_warning("worker API: the protocol of worker %s could not be written down: %s", work->name,
					error->message);
			axis_db_pool_release(self->pool, conn);
		}
	}
	return TRUE;
}

static void
work_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Work *work = task_data;

	if (refuse_other_version(work)) {
		g_task_return_boolean(task, TRUE);
		return;
	}
	switch (work->op) {
	case OP_REGISTER:
		op_register(work);
		break;
	case OP_CLAIM:
		op_claim(work);
		break;
	case OP_HEARTBEAT:
		op_heartbeat(work);
		break;
	case OP_COMPLETE:
		op_complete(work);
		break;
	default:
		op_fail(work);
		break;
	}
	if (work->reply.body == NULL)
		reply_error(&work->reply, 500, "internal", "the request could not be answered");
	g_task_return_boolean(task, TRUE);
}

static void
work_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Work *work = user_data;
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, work->reply.status);
	htmx_response_set_content(response, work->reply.body);
	htmx_response_set_content_type(response, "application/json");
	if (work->reply.status == 503)
		htmx_response_add_header(response, "Retry-After", "5");
	if (work->op == OP_REGISTER && work->reply.status == 200 && work->api->registered != NULL &&
	    !work->api->stopping)
		work->api->registered(work->name, work->api->registered_data);
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	work->api->pending--;
	work_free(work);
}

static HtmxResponse *
dispatch(
	AxisWorkerApi *self,
	HtmxRequest   *request,
	Op             op
){
	const AxisAuthState *state = axis_web_auth_state(request);
	HtmxResponse *returned;
	GBytes *body;
	GTask *task;
	Work *work;

	if (state == NULL || state->worker_id == 0 || state->worker_name == NULL)
		return axis_web_auth_worker_error(401, "unauthorized", "a worker's token is required");
	if (self->stopping || self->pending >= AXIS_WORKER_API_MAX_PENDING) {
		HtmxResponse *busy = axis_web_auth_worker_error(503, "busy", "the daemon is busy; try again");

		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}

	work = g_new0(Work, 1);
	work->api = self;
	work->op = op;
	body = htmx_request_get_body_bytes(request);
	work->body = body != NULL ? g_bytes_ref(body) : NULL;
	work->worker = state->worker_id;
	work->name = g_strdup(state->worker_name);
	work->holder = axis_worker_holder(state->worker_name);
	work->machine = axis_worker_machine(state->worker_name);
	work->address = g_strdup(state->client_address);
	work->deferred = htmx_response_new_deferred();
	returned = g_object_ref(work->deferred);

	self->pending++;
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
	return returned;
}

static HtmxResponse *
handle_register(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return dispatch(user_data, request, OP_REGISTER);
}

static HtmxResponse *
handle_claim(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return dispatch(user_data, request, OP_CLAIM);
}

static HtmxResponse *
handle_heartbeat(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return dispatch(user_data, request, OP_HEARTBEAT);
}

static HtmxResponse *
handle_complete(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return dispatch(user_data, request, OP_COMPLETE);
}

static HtmxResponse *
handle_fail(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return dispatch(user_data, request, OP_FAIL);
}

/* ── The reaper ─────────────────────────────────────────────────── */

typedef struct
{
	AxisWorkerApi *api;
	GArray        *jobs;	/* gint64 */
	GPtrArray     *tokens;	/* gchar* */
} Reap;

static void
reap_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Reap *reap = task_data;
	AxisWorkerApi *self = reap->api;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, &error);
	AxisJobFence fence;
	const AxisJobFence *fenced = api_fence(self, &fence);
	guint i;

	/* Fenced: a daemon that lost its lock takes no job back (it would be
	 * the live job of a worker that talks to the one that holds it); the
	 * lease is kept and tried again. */
	for (i = 0; i < reap->jobs->len; i++) {
		gint64 job = g_array_index(reap->jobs, gint64, i);
		const gchar *token = g_ptr_array_index(reap->tokens, i);
		g_autoptr(GError) local = NULL;
		gboolean done = conn != NULL &&
			(axis_job_expire_remote_fenced(conn, job, token, fenced, NULL, &local) ||
			 g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST));
		Lease *lease;

		g_mutex_lock(&self->lock);
		lease = lease_find_locked(self, job, token);
		if (lease != NULL) {
			if (done)
				g_hash_table_remove(self->leases, &job);
			else
				lease->expiring = FALSE;	/* tried again at the next turn */
		}
		g_mutex_unlock(&self->lock);
		if (!done && g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_FENCED))
			g_debug("worker API: the expired lease of job %" G_GINT64_FORMAT " is kept: %s", job, local->message);
		else if (!done && local != NULL)
			g_warning("worker API: an expired lease of job %" G_GINT64_FORMAT " could not be handed back: %s", job,
				local->message);
	}
	if (conn != NULL)
		axis_db_pool_release(self->pool, conn);
	else if (error != NULL)
		g_warning("worker API: expired leases could not be handed back: %s", error->message);
	g_task_return_boolean(task, TRUE);
}

static void
reap_free(Reap *reap)
{
	g_array_unref(reap->jobs);
	g_ptr_array_unref(reap->tokens);
	g_free(reap);
}

static void
reap_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Reap *reap = user_data;

	reap->api->pending--;
	reap->api->reaping = FALSE;
	reap_free(reap);
}

static gboolean
on_reap(gpointer user_data)
{
	AxisWorkerApi *self = user_data;
	gint64 now = g_get_monotonic_time();
	GHashTableIter iter;
	gpointer value;
	Reap *reap;
	GTask *task;

	if (self->reaping || self->stopping || self->paused)
		return G_SOURCE_CONTINUE;
	reap = g_new0(Reap, 1);
	reap->api = self;
	reap->jobs = g_array_new(FALSE, FALSE, sizeof(gint64));
	reap->tokens = g_ptr_array_new_with_free_func(g_free);
	g_mutex_lock(&self->lock);
	g_hash_table_iter_init(&iter, self->leases);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Lease *lease = value;

		/* A request being handled for it keeps it: it decides. */
		if (lease->expiring || lease->busy > 0 || now < lease->expires_us)
			continue;
		lease->expiring = TRUE;
		g_array_append_val(reap->jobs, lease->job);
		g_ptr_array_add(reap->tokens, g_strdup(lease->token));
	}
	g_mutex_unlock(&self->lock);
	if (reap->jobs->len == 0) {
		reap_free(reap);
		return G_SOURCE_CONTINUE;
	}
	self->reaping = TRUE;
	self->pending++;
	task = g_task_new(NULL, NULL, reap_finished, reap);
	g_task_set_task_data(task, reap, NULL);
	g_task_run_in_thread(task, reap_thread);
	g_object_unref(task);
	return G_SOURCE_CONTINUE;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

gboolean
axis_worker_api_register_kinds(
	AxisJobRegistry     *registry,
	AxisModuleRegistry  *modules,
	GError             **error
){
	g_return_val_if_fail(registry != NULL, FALSE);

	if (!axis_job_registry_add_remote(registry, AXIS_WORKER_KIND_CAPABILITIES, AXIS_WORKER_QUEUE, 1,
	        "Measuring what a worker's encoders can do", AXIS_JOB_CLASS_WORK,
	        modules != NULL ? axis_module_registry_ref_switch(modules, AXIS_WORKERS_MODULE_ID) : NULL, error))
		return FALSE;
	if (!axis_job_registry_add_remote(registry, AXIS_WORKER_KIND_SAMPLE, AXIS_WORKER_QUEUE, 3,
	        "Converting samples on a worker to measure the savings", AXIS_JOB_CLASS_WORK,
	        modules != NULL ? axis_module_registry_ref_switch(modules, AXIS_ESTIMATE_MODULE_ID) : NULL, error))
		return FALSE;
	if (!axis_job_registry_add_remote(registry, AXIS_WORKER_KIND_CONVERT, AXIS_WORKER_QUEUE,
	        AXIS_CONVERSION_ENCODE_ATTEMPTS, "Converting a file on a worker", AXIS_JOB_CLASS_WORK,
	        modules != NULL ? axis_module_registry_ref_switch(modules, AXIS_CONVERSION_MODULE_ID) : NULL, error))
		return FALSE;
	if (!axis_job_registry_add_remote(registry, AXIS_WORKER_KIND_VERIFY, AXIS_WORKER_QUEUE,
	        AXIS_CONVERSION_VERIFY_ATTEMPTS, "Checking a converted file on a worker", AXIS_JOB_CLASS_WORK,
	        modules != NULL ? axis_module_registry_ref_switch(modules, AXIS_CONVERSION_MODULE_ID) : NULL, error))
		return FALSE;

	/* Each of these is made from its owner's state when it is asked for (a
	 * plan, a conversion's stage, a worker's name): its owner asks again,
	 * and the job retried as it was would follow what is no longer so. */
	return axis_job_registry_set_retry_elsewhere(registry, AXIS_WORKER_KIND_CAPABILITIES,
	        "Measure the worker's encoders again with \"Measure its encoders again\" on its page under "
		"Conversion workers.", error) &&
		axis_job_registry_set_retry_elsewhere(registry, AXIS_WORKER_KIND_SAMPLE,
		"A measurement on a worker follows the plan made when it was asked for: ask for a new one with "
		"\"Measure on a worker\" on the Conversion workers page.", error) &&
		axis_job_registry_set_retry_elsewhere(registry, AXIS_WORKER_KIND_CONVERT,
		"Axis starts this encode from its conversion: ask for the conversion again on the Conversions page.", error) &&
		axis_job_registry_set_retry_elsewhere(registry, AXIS_WORKER_KIND_VERIFY,
		"Axis starts these checks from their conversion: the conversion's page under Conversions says what it "
		"is waiting for.", error);
}

void
axis_worker_api_set_convert(
	AxisWorkerApi                *self,
	const AxisWorkerConvertHooks *hooks,
	gpointer                      data
){
	g_return_if_fail(self != NULL);

	if (hooks != NULL)
		self->convert = *hooks;
	else
		memset(&self->convert, 0, sizeof(self->convert));
	self->convert_data = data;
}

static void
fix_timing(AxisWorkerApiTiming *timing)
{
	timing->lease_ms = CLAMP(timing->lease_ms != 0 ? timing->lease_ms : AXIS_WORKER_API_DEFAULT_LEASE_MS,
		AXIS_WORKER_MIN_LEASE_MS, AXIS_WORKER_MAX_LEASE_MS);
	if (timing->heartbeat_ms == 0)
		timing->heartbeat_ms = timing->lease_ms / 4;
	timing->heartbeat_ms = CLAMP(timing->heartbeat_ms, AXIS_WORKER_MIN_HEARTBEAT_MS, timing->lease_ms / 3);
	timing->poll_ms = CLAMP(timing->poll_ms != 0 ? timing->poll_ms : AXIS_WORKER_API_DEFAULT_POLL_MS,
		AXIS_WORKER_MIN_POLL_MS, AXIS_WORKER_MAX_POLL_MS);
	if (timing->reap_ms == 0)
		timing->reap_ms = DEFAULT_REAP_MS;
}

AxisWorkerApi *
axis_worker_api_new(
	AxisDbPool       *pool,
	AxisJobRegistry  *registry,
	AxisWorkerTokens *tokens,
	AxisEstimator    *estimator
){
	AxisWorkerApi *self;

	g_return_val_if_fail(pool != NULL && registry != NULL && tokens != NULL, NULL);

	self = g_new0(AxisWorkerApi, 1);
	self->pool = pool;
	self->registry = axis_job_registry_ref(registry);
	self->tokens = tokens;
	self->estimator = estimator != NULL ? axis_estimator_ref(estimator) : NULL;
	g_mutex_init(&self->lock);
	self->leases = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	self->known = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	fix_timing(&self->timing);
	return self;
}

void
axis_worker_api_set_registered(
	AxisWorkerApi            *self,
	AxisWorkerRegisteredFunc  func,
	gpointer                  user_data
){
	g_return_if_fail(self != NULL);

	self->registered = func;
	self->registered_data = user_data;
}

void
axis_worker_api_set_timing(
	AxisWorkerApi             *self,
	const AxisWorkerApiTiming *timing
){
	g_return_if_fail(self != NULL && timing != NULL);

	self->timing = *timing;
	fix_timing(&self->timing);
}

void
axis_worker_api_get_timing(
	AxisWorkerApi       *self,
	AxisWorkerApiTiming *timing
){
	g_return_if_fail(self != NULL && timing != NULL);

	*timing = self->timing;
}

void
axis_worker_api_register_routes(
	AxisWorkerApi *self,
	AxisWeb       *web
){
	g_return_if_fail(self != NULL && web != NULL);

	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "register", AXIS_WEB_ROUTE_WORKER,
		handle_register, self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "claim", AXIS_WEB_ROUTE_WORKER,
		handle_claim, self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "heartbeat", AXIS_WEB_ROUTE_WORKER,
		handle_heartbeat, self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "complete", AXIS_WEB_ROUTE_WORKER,
		handle_complete, self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "fail", AXIS_WEB_ROUTE_WORKER,
		handle_fail, self);
}

void
axis_worker_api_start(AxisWorkerApi *self)
{
	g_return_if_fail(self != NULL);

	if (self->reap_id == 0)
		self->reap_id = g_timeout_add(self->timing.reap_ms, on_reap, self);
}

void
axis_worker_api_stop(AxisWorkerApi *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	if (self->reap_id != 0) {
		g_source_remove(self->reap_id);
		self->reap_id = 0;
	}
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_worker_api_set_fence(
	AxisWorkerApi *self,
	AxisJobQueue  *jobs
){
	g_return_if_fail(self != NULL);

	g_clear_object(&self->fence_source);
	if (jobs != NULL)
		self->fence_source = g_object_ref(jobs);
}

void
axis_worker_api_pause(
	AxisWorkerApi *self,
	gboolean       paused
){
	GHashTableIter iter;
	gpointer value;
	gint64 until;

	g_return_if_fail(self != NULL);

	if (paused || !self->paused) {
		self->paused = paused;
		return;
	}

	/* The workers could not reach this daemon while it was paused. */
	until = g_get_monotonic_time() + (gint64)self->timing.lease_ms * 1000;
	g_mutex_lock(&self->lock);
	g_hash_table_iter_init(&iter, self->leases);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Lease *lease = value;

		lease->expires_us = MAX(lease->expires_us, until);
	}
	g_mutex_unlock(&self->lock);
	self->paused = FALSE;
}

void
axis_worker_api_free(AxisWorkerApi *self)
{
	if (self == NULL)
		return;
	axis_worker_api_stop(self);
	g_hash_table_unref(self->leases);
	g_hash_table_unref(self->known);
	g_mutex_clear(&self->lock);
	axis_job_registry_unref(self->registry);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	g_clear_object(&self->fence_source);
	g_free(self);
}

guint
axis_worker_api_count_leases(AxisWorkerApi *self)
{
	guint n;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->lock);
	n = g_hash_table_size(self->leases);
	g_mutex_unlock(&self->lock);
	return n;
}
