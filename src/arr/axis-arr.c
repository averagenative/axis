/*
 * axis-arr.c - the bring-over layer: its lifetime, the connections and
 * their secret, and the test of a connection
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The secrets table is read in one place, axis_arr_store_source_secret(), for a
 * request; a listing knows only whether a key is set.  A database error's
 * message can quote a failing row, so a key is checked before it is written
 * and the error of writing it is replaced by fixed words.
 */

#include "arr/axis-arr-private.h"

#include <string.h>

#include "arr/axis-arr-assumed.h"
#include "db/axis-db-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "storage/axis-storage-path.h"

G_DEFINE_QUARK(axis-arr-error-quark, axis_arr_error)

/* ── Results ────────────────────────────────────────────────────── */

gchar *
axis_arr_text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return g_strdup(axis_db_result_get_text(result, row, column));
}

gint64
axis_arr_int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
	return value;
}

gboolean
axis_arr_bool_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

void
axis_arr_secret_free(gchar *secret)
{
	volatile gchar *p = secret;

	if (secret == NULL)
		return;
	while (*p != '\0')
		*p++ = '\0';
	g_free(secret);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisArr *
axis_arr_new(
	AxisDbPool *pool,
	AxisMeta   *meta
){
	AxisArr *self;

	g_return_val_if_fail(pool != NULL && meta != NULL, NULL);

	self = g_new0(AxisArr, 1);
	self->ref_count = 1;
	self->pool = pool;
	self->meta = axis_meta_ref(meta);
	self->cancel = g_cancellable_new();
	g_mutex_init(&self->lock);
	return self;
}

AxisArr *
axis_arr_ref(AxisArr *self)
{
	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_arr_unref(AxisArr *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;
	g_object_unref(self->cancel);
	g_mutex_clear(&self->lock);
	axis_meta_unref(self->meta);
	g_clear_pointer(&self->movies, axis_movies_unref);
	g_clear_pointer(&self->tv, axis_tv_unref);
	g_free(self);
}

void
axis_arr_set_movies(
	AxisArr    *self,
	AxisMovies *movies
){
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->movies, axis_movies_unref);
	self->movies = movies != NULL ? axis_movies_ref(movies) : NULL;
}

void
axis_arr_set_tv(
	AxisArr *self,
	AxisTv  *tv
){
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->tv, axis_tv_unref);
	self->tv = tv != NULL ? axis_tv_ref(tv) : NULL;
}

void
axis_arr_stop(AxisArr *self)
{
	g_return_if_fail(self != NULL);

	g_cancellable_cancel(self->cancel);
}

AxisDbPool *
axis_arr_get_pool(AxisArr *self)
{
	return self->pool;
}

void
axis_arr_set_tuning(
	AxisArr             *self,
	const AxisArrTuning *tuning
){
	g_mutex_lock(&self->lock);
	if (tuning != NULL)
		self->tuning = *tuning;
	else
		memset(&self->tuning, 0, sizeof(self->tuning));
	g_mutex_unlock(&self->lock);
}

void
axis_arr_get_tuning(
	AxisArr       *self,
	AxisArrTuning *tuning
){
	g_mutex_lock(&self->lock);
	*tuning = self->tuning;
	g_mutex_unlock(&self->lock);
	if (tuning->request_timeout_ms == 0)
		tuning->request_timeout_ms = AXIS_ARR_REQUEST_TIMEOUT_MS;
	if (tuning->list_timeout_ms == 0)
		tuning->list_timeout_ms = AXIS_ARR_LIST_TIMEOUT_MS;
	if (tuning->list_max_bytes == 0)
		tuning->list_max_bytes = AXIS_ARR_LIST_MAX_BYTES;
	if (tuning->page_size == 0 || tuning->page_size > AXIS_ARR_MAX_PAGE_RECORDS)
		tuning->page_size = AXIS_ARR_PAGE_SIZE;
	if (tuning->lookup_wait_ms == 0)
		tuning->lookup_wait_ms = AXIS_ARR_LOOKUP_WAIT_MS;
	if (tuning->lookup_give_up == 0)
		tuning->lookup_give_up = AXIS_ARR_LOOKUP_GIVE_UP;
	if (tuning->lookup_max == 0)
		tuning->lookup_max = AXIS_ARR_LOOKUP_MAX;
	if (tuning->queue_deadline_ms == 0)
		tuning->queue_deadline_ms = AXIS_ARR_QUEUE_DEADLINE_MS;
	if (tuning->provider == NULL)
		tuning->provider = "tmdb";
	if (tuning->requests_per_second == 0)
		tuning->requests_per_second = AXIS_ARR_REQUESTS_PER_SECOND;
	if (tuning->series_max_bytes == 0)
		tuning->series_max_bytes = AXIS_ARR_SERIES_MAX_BYTES;
	if (tuning->series_timeout_ms == 0)
		tuning->series_timeout_ms = AXIS_ARR_SERIES_TIMEOUT_MS;
	if (tuning->series_read_max == 0)
		tuning->series_read_max = AXIS_ARR_SERIES_READ_MAX;
	if (tuning->series_total_bytes == 0)
		tuning->series_total_bytes = AXIS_ARR_SERIES_TOTAL_BYTES;
	if (tuning->series_deadline_ms == 0)
		tuning->series_deadline_ms = AXIS_ARR_SERIES_DEADLINE_MS;
	if (tuning->series_retry_ms == 0)
		tuning->series_retry_ms = AXIS_ARR_SERIES_RETRY_MS;
}

gboolean
axis_arr_series_ready(AxisArr *self)
{
	const AxisMetaProviderInfo *info;

	g_return_val_if_fail(self != NULL, FALSE);

	/* The provider of the numbering a series from Sonarr follows: offered
	 * only while its key is set. */
	info = axis_meta_provider_find(self->meta, AXIS_ARR_SERIES_PROVIDER);
	return info != NULL && axis_meta_provider_offered(self->meta, info);
}

/* ── Connections ────────────────────────────────────────────────── */

void
axis_arr_source_free(AxisArrSource *source)
{
	if (source == NULL)
		return;
	g_free(source->name);
	g_free(source->origin);
	g_free(source->path_prefix);
	g_free(source->made_by);
	g_free(source->version);
	g_free(source->mappings);
	g_free(source);
}

static const struct
{
	AxisArrTest  test;
	const gchar *word;
} test_words[] = {
	{ AXIS_ARR_TEST_OK, "ok" },
	{ AXIS_ARR_TEST_REFUSED, "refused" },
	{ AXIS_ARR_TEST_NOT_APP, "not_app" },
	{ AXIS_ARR_TEST_NO_ANSWER, "no_answer" },
	{ AXIS_ARR_TEST_CERTIFICATE, "certificate" }
};

static AxisArrTest
test_from_word(const gchar *word)
{
	guint i;

	for (i = 0; word != NULL && i < G_N_ELEMENTS(test_words); i++) {
		if (strcmp(word, test_words[i].word) == 0)
			return test_words[i].test;
	}
	return AXIS_ARR_TEST_NONE;
}

static const gchar *
test_to_word(AxisArrTest test)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(test_words); i++) {
		if (test_words[i].test == test)
			return test_words[i].word;
	}
	return NULL;
}

gchar *
axis_arr_test_sentence(
	AxisArrApp   app,
	AxisArrTest  result,
	const gchar *version
){
	const gchar *label = axis_arr_app_label(app);

	switch (result) {
	case AXIS_ARR_TEST_OK:
		return version != NULL ? g_strdup_printf("%s %s answered.", label, version) :
			g_strdup_printf("%s answered.", label);
	case AXIS_ARR_TEST_REFUSED:
		return g_strdup_printf("%s refused the API key.", label);
	case AXIS_ARR_TEST_NOT_APP:
		return g_strdup_printf("This address answers, but not as %s.", label);
	case AXIS_ARR_TEST_NO_ANSWER:
		return g_strdup("No answer from this address.");
	case AXIS_ARR_TEST_CERTIFICATE:
		return g_strdup_printf("%s's certificate was not accepted.", label);
	default:
		return NULL;
	}
}

/* Never the secret: only whether one is set. */
#define SOURCE_COLUMNS \
	"SELECT a.id, a.app, a.name, a.origin, a.path_prefix, a.accept_untrusted, (s.source_id IS NOT NULL)," \
	" a.made_by, a.last_test, a.version, " AXIS_ARR_US("a.tested_at") ", " AXIS_ARR_US("a.created_at") "," \
	" a.mappings::text"
#define SOURCE_FROM " FROM arr_source a LEFT JOIN arr_source_secret s ON s.source_id = a.id"

static const gchar *const source_sql =
	SOURCE_COLUMNS SOURCE_FROM
	" WHERE ($1::bigint = 0 OR a.id = $1::bigint)"
	" ORDER BY lower(a.name), a.id";

/* The one statement that reads the key: the connection and its key in one
 * row, so that the key is the one of the address read with it. */
static const gchar *const source_secret_sql =
	SOURCE_COLUMNS ", s.secret" SOURCE_FROM
	" WHERE a.id = $1::bigint";

/* @secret: (nullable): where the key is put as well, from one statement
 * with the connection (%NULL when none is stored). */
static gboolean
load_sources(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **sources,
	gchar      **secret,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];
	guint i;

	params[0] = id_text;
	if (secret != NULL)
		*secret = NULL;
	if (!axis_db_conn_exec(conn, secret != NULL ? source_secret_sql : source_sql, 1, params, &result, error))
		return FALSE;
	if (secret != NULL && axis_db_result_get_n_rows(result) > 0 && axis_arr_bool_at(result, 0, 6))
		*secret = axis_arr_text_at(result, 0, 13);
	*sources = g_ptr_array_new_with_free_func((GDestroyNotify)axis_arr_source_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisArrSource *source = g_new0(AxisArrSource, 1);

		source->id = axis_arr_int_at(result, i, 0, 0);
		if (!axis_arr_app_from_string(axis_db_result_get_text(result, i, 1), &source->app))
			source->app = AXIS_ARR_RADARR;
		source->name = axis_arr_text_at(result, i, 2);
		source->origin = axis_arr_text_at(result, i, 3);
		source->path_prefix = axis_arr_text_at(result, i, 4);
		source->accept_untrusted = axis_arr_bool_at(result, i, 5);
		source->secret_set = axis_arr_bool_at(result, i, 6);
		source->made_by = axis_arr_text_at(result, i, 7);
		source->last_test = test_from_word(axis_db_result_get_text(result, i, 8));
		source->version = axis_arr_text_at(result, i, 9);
		source->tested_us = axis_arr_int_at(result, i, 10, 0);
		source->created_us = axis_arr_int_at(result, i, 11, 0);
		source->mappings = axis_arr_text_at(result, i, 12);
		g_ptr_array_add(*sources, source);
	}
	return TRUE;
}

gboolean
axis_arr_store_source_secret(
	AxisDbConn     *conn,
	gint64          id,
	AxisArrSource **source,
	gchar         **secret,
	GError        **error
){
	g_autoptr(GPtrArray) sources = NULL;

	*source = NULL;
	if (secret != NULL)
		*secret = NULL;
	if (id <= 0 || !load_sources(conn, id, &sources, secret, error)) {
		if (id <= 0)
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "There is no such connection.");
		return FALSE;
	}
	if (sources->len == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "There is no such connection.");
		return FALSE;
	}
	*source = g_ptr_array_steal_index(sources, 0);
	return TRUE;
}

gboolean
axis_arr_store_source(
	AxisDbConn     *conn,
	gint64          id,
	AxisArrSource **source,
	GError        **error
){
	return axis_arr_store_source_secret(conn, id, source, NULL, error);
}

gboolean
axis_arr_list_sources(
	AxisArr    *self,
	GPtrArray **sources,
	GError    **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	*sources = NULL;
	if (conn == NULL)
		return FALSE;
	ok = load_sources(conn, 0, sources, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_arr_get_source(
	AxisArr        *self,
	gint64          id,
	AxisArrSource **source,
	GError        **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	*source = NULL;
	if (conn == NULL)
		return FALSE;
	ok = axis_arr_store_source(conn, id, source, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gboolean
refuse(
	GError      **error,
	const gchar  *sentence
){
	g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, sentence);
	return FALSE;
}

/* A key as the apps make them, loosely: printable ASCII without a space,
 * so that it can go in a header as it is. */
static gboolean
key_valid(const gchar *key)
{
	const gchar *p;

	if (key[0] == '\0' || strlen(key) > AXIS_ARR_KEY_MAX_CHARS)
		return FALSE;
	for (p = key; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_arr_save_source(
	AxisArr                  *self,
	gint64                    id,
	const AxisArrSourceSpec  *spec,
	gint64                   *saved,
	GError                  **error
){
	g_autoptr(AxisDbResult) current = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *origin = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *why = NULL;
	const gchar *secret = spec->secret != NULL && spec->secret[0] != '\0' ? spec->secret : NULL;
	const gchar *problem = NULL;
	const gchar *params[7];
	AxisHttpOrigin parsed;
	AxisArrApp app;
	AxisDbConn *conn;
	gboolean moved = FALSE;
	gint64 row = id;

	if (saved != NULL)
		*saved = 0;
	if (!axis_arr_app_from_string(spec->app, &app))
		return refuse(error, "Choose the app: Radarr or Sonarr.");
	if (!axis_storage_label_normalize(spec->name, &name, NULL))
		return refuse(error, "The name must be 1 to 64 characters.");
	if (!axis_arr_address_split(spec->address, &origin, &prefix, &problem))
		return refuse(error, problem);
	if (!axis_http_origin_parse(origin, &parsed, &why))
		return refuse(error, "The address must be http:// or https://, a host and an optional port.");
	axis_http_origin_clear(&parsed);
	if (secret != NULL && !key_valid(secret))
		return refuse(error, "The API key must be 1 to 1024 characters without spaces.");

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	if (id <= 0 && secret == NULL) {
		axis_db_pool_release(self->pool, conn);
		return refuse(error, "The API key is missing.");
	}
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	if (id > 0) {
		/* The row is held from here to the commit: the address it has now is
		 * the one the change is measured against, and no other change of it
		 * slips between. */
		params[0] = id_text;
		if (!axis_db_conn_exec(conn, "SELECT origin, path_prefix FROM arr_source WHERE id = $1::bigint FOR UPDATE", 1,
		        params, &current, &local))
			goto failed;
		if (axis_db_result_get_n_rows(current) == 0) {
			axis_db_conn_rollback(conn, NULL);
			axis_db_pool_release(self->pool, conn);
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "There is no such connection.");
			return FALSE;
		}
		moved = g_strcmp0(axis_db_result_get_text(current, 0, 0), origin) != 0 ||
			g_strcmp0(axis_db_result_get_text(current, 0, 1), prefix) != 0;
		/* A stored key was stored for one address, and goes to no other. */
		if (moved && secret == NULL) {
			axis_db_conn_rollback(conn, NULL);
			axis_db_pool_release(self->pool, conn);
			return refuse(error, "Type the API key again: the address changed, and a stored key is only ever sent to "
				"the address it was stored for.");
		}
	}
	params[0] = axis_arr_app_to_string(app);
	params[1] = name;
	params[2] = origin;
	params[3] = prefix;
	params[4] = spec->accept_untrusted ? "true" : "false";
	if (id > 0) {
		/* What a test found is of the address, the certificate rule and the
		 * key it was made with: a change of any of them forgets it. */
		params[5] = secret != NULL ? "true" : "false";
		params[6] = id_text;
		if (!axis_db_conn_exec(conn,
		        "UPDATE arr_source SET name = $2, origin = $3, path_prefix = $4, accept_untrusted = $5::boolean,"
		        " last_test = CASE WHEN origin = $3 AND path_prefix = $4 AND accept_untrusted = $5::boolean"
		        "   AND NOT $6::boolean THEN last_test END,"
		        " version = CASE WHEN origin = $3 AND path_prefix = $4 THEN version END,"
		        " updated_at = now() WHERE id = $7::bigint AND app = $1", 7, params, &result, &local))
			goto failed;
		/* Gone meanwhile, or of another app: no key is written for it.  A
		 * connection keeps the app it was made for, so a key typed for a
		 * Sonarr never lands on a Radarr's address, nor the reverse. */
		if (axis_db_result_get_affected_rows(result) == 0) {
			axis_db_conn_rollback(conn, NULL);
			axis_db_pool_release(self->pool, conn);
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "There is no such connection.");
			return FALSE;
		}
	} else {
		params[5] = spec->user != NULL && spec->user[0] != '\0' ? spec->user : NULL;
		if (!axis_db_conn_exec(conn,
		        "INSERT INTO arr_source (app, name, origin, path_prefix, accept_untrusted, made_by)"
		        " VALUES ($1, $2, $3, $4, $5::boolean, $6) RETURNING id", 6, params, &result, &local))
			goto failed;
		row = axis_arr_int_at(result, 0, 0, 0);
	}
	if (secret != NULL) {
		g_autofree gchar *row_text = g_strdup_printf("%" G_GINT64_FORMAT, row);
		const gchar *secret_params[2];
		g_autoptr(GError) hidden = NULL;

		secret_params[0] = row_text;
		secret_params[1] = secret;
		/* The error of this statement could quote the row: it is not kept. */
		if (!axis_db_conn_exec(conn,
		        "INSERT INTO arr_source_secret (source_id, secret) VALUES ($1::bigint, $2)"
		        " ON CONFLICT (source_id) DO UPDATE SET secret = EXCLUDED.secret, set_at = now()", 2, secret_params,
		        NULL, &hidden)) {
			axis_db_conn_rollback(conn, NULL);
			axis_db_pool_release(self->pool, conn);
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED, "The API key could not be stored.");
			return FALSE;
		}
	}
	if (!axis_db_conn_commit(conn, &local))
		goto failed;
	axis_db_pool_release(self->pool, conn);
	if (saved != NULL)
		*saved = row;
	return TRUE;

failed:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION))
		return refuse(error, "Another connection already has this name.");
	g_propagate_error(error, g_steal_pointer(&local));
	return FALSE;
}

gboolean
axis_arr_delete_source(
	AxisArr  *self,
	gint64    id,
	GError  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) live = NULL;
	g_autoptr(GError) ignored = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_autofree gchar *key = g_strdup_printf(AXIS_ARR_READ_KEY_PREFIX "%" G_GINT64_FORMAT, id);
	g_autofree gchar *run_key = g_strdup_printf(AXIS_ARR_APPLY_KEY_PREFIX "%" G_GINT64_FORMAT, id);
	const gchar *params[2];
	AxisDbConn *conn;
	guint i;

	params[0] = id_text;
	/* Its key, its plans and its list of what it brought go with it (ON
	 * DELETE CASCADE).  What it brought stays: the movies, their links with
	 * their marks, and their holds. */
	if (!axis_db_pool_exec(self->pool, "DELETE FROM arr_source WHERE id = $1::bigint", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "There is no such connection.");
		return FALSE;
	}
	/* A reading of it under way is stopped: it has nothing left to write
	 * to, and its job would otherwise go on asking the app. */
	params[0] = key;
	params[1] = run_key;
	conn = axis_db_pool_acquire(self->pool, &ignored);
	if (conn == NULL)
		return TRUE;
	/* A run of it too: it ends with the movie it is at. */
	if (axis_db_conn_exec(conn, "SELECT id FROM job WHERE dedupe_key IN ($1, $2) AND state IN ('queued', 'running')", 2,
	        params, &live, &ignored)) {
		for (i = 0; i < axis_db_result_get_n_rows(live); i++) {
			g_autoptr(GError) failed = NULL;

			if (!axis_job_cancel(conn, axis_arr_int_at(live, i, 0, 0), NULL, &failed))
				g_debug("bring over: a reading could not be stopped: %s", failed->message);
		}
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

gboolean
axis_arr_store_test(
	AxisArr      *self,
	gint64        id,
	const gchar  *origin,
	const gchar  *path_prefix,
	AxisArrTest   result,
	const gchar  *version,
	GError      **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[5];

	params[0] = id_text;
	params[1] = test_to_word(result);
	params[2] = result == AXIS_ARR_TEST_OK ? version : NULL;
	params[3] = origin;
	params[4] = path_prefix;
	/* Only of the address it was made at: one changed since has not been
	 * tested by this. */
	return axis_db_pool_exec(self->pool,
		"UPDATE arr_source SET last_test = $2, tested_at = now(),"
		" version = CASE WHEN $2 = 'ok' THEN coalesce($3, version) ELSE version END"
		" WHERE id = $1::bigint AND origin = $4 AND path_prefix = $5",
		5, params, NULL, error);
}

gboolean
axis_arr_test_source(
	AxisArr      *self,
	gint64        id,
	AxisArrTest  *result,
	GError      **error
){
	g_autoptr(AxisArrSource) source = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *version = NULL;
	AxisArrClient *client;
	AxisDbConn *conn;
	gchar *secret = NULL;
	gboolean ok;

	*result = AXIS_ARR_TEST_NONE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_arr_store_source_secret(conn, id, &source, &secret, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_arr_secret_free(secret);
		return FALSE;
	}
	client = axis_arr_client_new(self, source, secret, self->cancel, error);
	axis_arr_secret_free(secret);
	if (client == NULL)
		return FALSE;
	if (axis_arr_client_status(client, &version, &local))
		*result = AXIS_ARR_TEST_OK;
	else
		*result = axis_arr_test_of_error(local);
	axis_arr_client_free(client);
	return axis_arr_store_test(self, id, source->origin, source->path_prefix, *result, version, error);
}
