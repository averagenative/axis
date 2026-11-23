/*
 * axis-worker-store.c - the worker table
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-worker-store.h"

#include <string.h>

#include "api/axis-worker-proto.h"
#include "db/axis-db-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "transcode/axis-convert-caps.h"
#include "web/axis-password.h"

G_DEFINE_QUARK(axis-worker-store-error-quark, axis_worker_store_error)

gchar *
axis_worker_machine(const gchar *name)
{
	return g_strconcat(AXIS_WORKER_MACHINE_PREFIX, name, NULL);
}

gchar *
axis_worker_holder(const gchar *name)
{
	return g_strconcat(AXIS_WORKER_HOLDER_PREFIX, name, NULL);
}

static gchar *
normalise_name(const gchar *input)
{
	g_autofree gchar *trimmed = NULL;
	gchar *lower;

	if (input == NULL || strlen(input) > 4 * AXIS_WORKER_NAME_MAX)
		return NULL;
	trimmed = g_strstrip(g_strdup(input));
	lower = g_ascii_strdown(trimmed, -1);
	if (!axis_worker_name_is_valid(lower)) {
		g_free(lower);
		return NULL;
	}
	return lower;
}

gboolean
axis_worker_store_create(
	AxisDbPool        *pool,
	AxisWorkerTokens  *tokens,
	const gchar       *input,
	const gchar       *by,
	gchar            **token_out,
	gint64            *id_out,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *name = normalise_name(input);
	g_autofree gchar *hash = NULL;
	const gchar *params[3];
	gchar *token;
	gint64 id = 0;

	*token_out = NULL;
	if (id_out != NULL)
		*id_out = 0;
	if (name == NULL) {
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_INVALID,
			"A worker's name is 1 to 48 lower-case letters, digits and '-', starting with a letter.");
		return FALSE;
	}
	token = axis_random_token();
	hash = axis_token_hash(token);
	params[0] = name;
	params[1] = hash;
	params[2] = by;
	if (!axis_db_pool_exec(pool, "INSERT INTO worker (name, token_hash, created_by) VALUES ($1::text, $2::text, $3::text)"
	        " RETURNING id", 3, params, &result, &local)) {
		axis_secret_free(token);
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_EXISTS,
				"There is a worker of that name already (a revoked worker keeps its name).");
			return FALSE;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (axis_db_result_get_n_rows(result) != 1 || !axis_db_result_get_int64(result, 0, 0, &id)) {
		axis_secret_free(token);
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND,
			"The worker could not be read back.");
		return FALSE;
	}
	axis_worker_tokens_put(tokens, id, name, hash, FALSE);
	g_message("audit: worker %s created by %s", name, by != NULL ? by : "?");
	*token_out = token;
	if (id_out != NULL)
		*id_out = id;
	return TRUE;
}

gboolean
axis_worker_store_revoke(
	AxisDbPool        *pool,
	AxisWorkerTokens  *tokens,
	const gchar       *name,
	const gchar       *by,
	gboolean          *revoked,
	GError           **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];
	gint64 id = 0;

	if (revoked != NULL)
		*revoked = FALSE;
	if (!axis_worker_name_is_valid(name)) {
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND, "There is no such worker.");
		return FALSE;
	}
	params[0] = name;
	params[1] = by;
	if (!axis_db_pool_exec(pool, "SELECT id FROM worker WHERE name = $1::text", 1, params, &found, error))
		return FALSE;
	if (axis_db_result_get_n_rows(found) != 1 || !axis_db_result_get_int64(found, 0, 0, &id)) {
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND, "There is no such worker.");
		return FALSE;
	}
	/* In memory first: the next request with the token is refused even if
	 * writing it down fails. */
	axis_worker_tokens_revoke(tokens, id);
	if (!axis_db_pool_exec(pool, "UPDATE worker SET revoked_at = now(), revoked_by = coalesce($2::text, '?')"
	        " WHERE name = $1::text AND revoked_at IS NULL RETURNING id", 2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1) {
		g_message("audit: worker %s revoked by %s", name, by != NULL ? by : "?");
		if (revoked != NULL)
			*revoked = TRUE;
	}
	return TRUE;
}

gboolean
axis_worker_store_request_measure(
	AxisDbPool   *pool,
	const gchar  *name,
	gint64       *job_id,
	gboolean     *created,
	GError      **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *dedupe = NULL;
	JsonObject *requires = json_object_new();
	const gchar *params[1];
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	if (!axis_worker_name_is_valid(name)) {
		json_object_unref(requires);
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND, "There is no such worker.");
		return FALSE;
	}
	params[0] = name;
	if (!axis_db_pool_exec(pool, "SELECT id FROM worker WHERE name = $1::text AND revoked_at IS NULL", 1, params,
	        &found, error)) {
		json_object_unref(requires);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(found) != 1) {
		json_object_unref(requires);
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND,
			"There is no such worker, or it is revoked.");
		return FALSE;
	}
	json_object_set_string_member(requires, "worker", name);
	json_object_set_object_member(payload, "requires", requires);
	dedupe = g_strconcat(AXIS_WORKER_KIND_CAPABILITIES ":", name, NULL);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_WORKER_KIND_CAPABILITIES;
	spec.queue = AXIS_WORKER_QUEUE;
	spec.payload = payload;
	spec.dedupe_key = dedupe;
	spec.max_attempts = 1;
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_enqueue(conn, &spec, job_id, created, error);
	axis_db_pool_release(pool, conn);
	return ok;
}

gboolean
axis_worker_store_resume(
	AxisDbPool   *pool,
	const gchar  *name,
	const gchar  *by,
	gboolean     *resumed,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	if (resumed != NULL)
		*resumed = FALSE;
	if (!axis_worker_name_is_valid(name)) {
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND, "There is no such worker.");
		return FALSE;
	}
	params[0] = name;
	if (!axis_db_pool_exec(pool, "WITH found AS (SELECT id, paused_job FROM worker WHERE name = $1::text"
	        " AND revoked_at IS NULL FOR UPDATE)"
	        " UPDATE worker w SET resumed_job = coalesce(f.paused_job, w.resumed_job), paused_at = NULL,"
	        " paused_job = NULL, paused_reason = NULL FROM found f WHERE w.id = f.id"
	        " RETURNING f.paused_job IS NOT NULL, coalesce(f.paused_job, 0)", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error_literal(error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND,
			"There is no such worker, or it is revoked.");
		return FALSE;
	}
	{
		gboolean was = FALSE;
		gint64 job = 0;

		axis_db_result_get_boolean(result, 0, 0, &was);
		axis_db_result_get_int64(result, 0, 1, &job);
		if (was)
			g_message("audit: worker %s resumed by %s after its encoder crashed on job %" G_GINT64_FORMAT, name,
				by != NULL ? by : "?", job);
		if (resumed != NULL)
			*resumed = was;
	}
	return TRUE;
}

static void
volume_row_free(gpointer data)
{
	AxisWorkerVolumeRow *row = data;

	g_free(row->volume_name);
	g_free(row->uuid);
	g_free(row->path);
	g_free(row);
}

static void
job_row_free(gpointer data)
{
	AxisWorkerJobRow *row = data;

	g_free(row->kind);
	g_free(row->message);
	g_free(row);
}

void
axis_worker_info_free(AxisWorkerInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->name);
	g_free(info->created_by);
	g_free(info->revoked_by);
	g_free(info->last_address);
	g_free(info->worker_version);
	g_free(info->ffmpeg_version);
	g_free(info->paused_reason);
	if (info->volumes != NULL)
		g_ptr_array_unref(info->volumes);
	if (info->jobs != NULL)
		g_ptr_array_unref(info->jobs);
	axis_encoder_capabilities_free(info->caps);
	g_free(info);
}

#define MICROS(column) "coalesce((extract(epoch FROM " column ") * 1000000)::bigint, 0)"

static gint64
get_int(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_is_null(result, row, column))
		axis_db_result_get_int64(result, row, column, &value);
	return value;
}

static gchar *
get_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : g_strdup(axis_db_result_get_text(result, row, column));
}

gboolean
axis_worker_store_list(
	AxisDbPool  *pool,
	GPtrArray  **workers_out,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisDbResult) volumes = NULL;
	g_autoptr(AxisDbResult) jobs = NULL;
	g_autoptr(GPtrArray) workers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_worker_info_free);
	AxisDbConn *conn;
	gboolean ok = FALSE;
	guint i;
	guint j;

	*workers_out = NULL;
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_exec(conn,
	        "SELECT id, name, " MICROS("created_at") ", created_by, " MICROS("revoked_at") ", revoked_by, "
	        MICROS("last_seen_at") ", coalesce((extract(epoch FROM now() - last_seen_at) * 1000)::bigint, -1),"
	        " last_address, " MICROS("registered_at") ", worker_version, ffmpeg_version, coalesce(max_jobs, 0),"
	        " " MICROS("paused_at") ", coalesce(paused_job, 0), paused_reason, coalesce(refused_protocol, -1),"
	        " " MICROS("refused_protocol_at")
	        " FROM worker ORDER BY name", 0, NULL, &rows, error) ||
	    !axis_db_conn_exec(conn,
	        "SELECT wv.worker_id, v.name, v.uuid::text, wv.path, " MICROS("wv.proved_at") ", wv.keeps_identity"
	        " FROM worker_volume wv JOIN volume v ON v.id = wv.volume_id ORDER BY v.name", 0, NULL, &volumes, error) ||
	    !axis_db_conn_exec(conn,
	        "SELECT w.id, j.id, j.kind, coalesce(j.progress, -1)::text, j.progress_message, " MICROS("j.started_at")
	        " FROM job j JOIN worker w ON j.worker_id = '" AXIS_WORKER_HOLDER_PREFIX "' || w.name"
	        " WHERE j.state = 'running' ORDER BY j.id", 0, NULL, &jobs, error))
		goto out;

	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisWorkerInfo *info = g_new0(AxisWorkerInfo, 1);
		g_autofree gchar *machine = NULL;

		info->id = get_int(rows, i, 0);
		info->name = get_text(rows, i, 1);
		info->created_us = get_int(rows, i, 2);
		info->created_by = get_text(rows, i, 3);
		info->revoked_us = get_int(rows, i, 4);
		info->revoked_by = get_text(rows, i, 5);
		info->last_seen_us = get_int(rows, i, 6);
		info->seen_ago_ms = axis_db_result_is_null(rows, i, 7) ? -1 : get_int(rows, i, 7);
		info->last_address = get_text(rows, i, 8);
		info->registered_us = get_int(rows, i, 9);
		info->worker_version = get_text(rows, i, 10);
		info->ffmpeg_version = get_text(rows, i, 11);
		info->max_jobs = (gint)get_int(rows, i, 12);
		info->paused_us = get_int(rows, i, 13);
		info->paused_job = get_int(rows, i, 14);
		info->paused_reason = get_text(rows, i, 15);
		info->refused_protocol = (gint)get_int(rows, i, 16);
		info->refused_protocol_us = get_int(rows, i, 17);
		info->volumes = g_ptr_array_new_with_free_func(volume_row_free);
		info->jobs = g_ptr_array_new_with_free_func(job_row_free);
		for (j = 0; j < axis_db_result_get_n_rows(volumes); j++) {
			AxisWorkerVolumeRow *v;

			if (get_int(volumes, j, 0) != info->id)
				continue;
			v = g_new0(AxisWorkerVolumeRow, 1);
			v->volume_name = get_text(volumes, j, 1);
			v->uuid = get_text(volumes, j, 2);
			v->path = get_text(volumes, j, 3);
			v->proved_us = get_int(volumes, j, 4);
			v->keeps_identity = TRUE;
			axis_db_result_get_boolean(volumes, j, 5, &v->keeps_identity);
			g_ptr_array_add(info->volumes, v);
		}
		for (j = 0; j < axis_db_result_get_n_rows(jobs); j++) {
			AxisWorkerJobRow *job;

			if (get_int(jobs, j, 0) != info->id)
				continue;
			job = g_new0(AxisWorkerJobRow, 1);
			job->id = get_int(jobs, j, 1);
			job->kind = get_text(jobs, j, 2);
			job->progress = g_ascii_strtod(axis_db_result_get_text(jobs, j, 3), NULL);
			job->message = get_text(jobs, j, 4);
			job->started_us = get_int(jobs, j, 5);
			g_ptr_array_add(info->jobs, job);
		}
		machine = axis_worker_machine(info->name);
		if (!axis_convert_capabilities_load(conn, machine, &info->caps, error)) {
			axis_worker_info_free(info);
			goto out;
		}
		g_ptr_array_add(workers, info);
	}
	ok = TRUE;
	*workers_out = g_steal_pointer(&workers);

out:
	axis_db_pool_release(pool, conn);
	return ok;
}
