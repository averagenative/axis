/*
 * axis-compat.c - the compat object: settings, the connection, the
 * connection test and the job entry points
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-private.h"

#include <string.h>

#include "compat/axis-compat-plex.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library-name.h"

G_DEFINE_QUARK(axis-compat-error-quark, axis_compat_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo settings[] = {
	{ AXIS_COMPAT_SETTING_ADDRESS, AXIS_SETTING_STRING, "Media server address (Plex)",
	  "The Plex Media Server to learn from, as http://host:port or https://host:port (for example "
	  "http://192.168.1.10:32400). Axis only ever sends read requests (GET) to this address and nowhere "
	  "else; leave it empty to switch the connection off.",
	  0, NULL, 0, AXIS_COMPAT_ADDRESS_MAX_BYTES, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_TOKEN, AXIS_SETTING_STRING, "Media server token (Plex)",
	  "The server's access token (X-Plex-Token). It is never shown again, never written to a log, and sent "
	  "only to the address above, as a request header. Over plain http it crosses the network unprotected: "
	  "use https where the server offers it. Clearing it switches the connection off.",
	  AXIS_SETTING_SECRET, NULL, 0, 255, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_UNTRUSTED_TLS, AXIS_SETTING_BOOLEAN, "Accept an untrusted certificate (Plex)",
	  "Accept an https certificate that does not verify (a self-signed one on a local address). Off by "
	  "default: certificates are verified, and a connection whose certificate is refused fails.",
	  0, "false", 0, 0, 0, 0, NULL },
	{ AXIS_COMPAT_JELLYFIN_ADDRESS, AXIS_SETTING_STRING, "Media server address (Jellyfin)",
	  "The Jellyfin server to learn from, as http://host:port or https://host:port (for example "
	  "http://192.168.1.11:8096). Axis sends read requests (GET) to this address and nowhere else, and one "
	  "other request: to rescan a folder after an import (\"Tell Jellyfin which folder changed\" below). Leave "
	  "it empty to switch the connection off. It can be used at the same time as Plex.",
	  0, NULL, 0, AXIS_COMPAT_ADDRESS_MAX_BYTES, 0, 0, NULL },
	{ AXIS_COMPAT_JELLYFIN_TOKEN, AXIS_SETTING_STRING, "Media server API key (Jellyfin)",
	  "An API key made under Dashboard, API Keys on the Jellyfin server. It is never shown again, never "
	  "written to a log, and sent only to the address above, as a request header. Over plain http it crosses "
	  "the network unprotected: use https where the server offers it. Clearing it switches the connection off.",
	  AXIS_SETTING_SECRET, NULL, 0, 255, 0, 0, NULL },
	{ AXIS_COMPAT_JELLYFIN_UNTRUSTED_TLS, AXIS_SETTING_BOOLEAN, "Accept an untrusted certificate (Jellyfin)",
	  "Accept an https certificate that does not verify (a self-signed one on a local address). Off by "
	  "default: certificates are verified, and a connection whose certificate is refused fails.",
	  0, "false", 0, 0, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_NOTIFY_PLEX, AXIS_SETTING_BOOLEAN, "Tell Plex which folder changed",
	  "After an import, an upgrade or a removal, ask Plex to scan the one folder that changed (a request to its "
	  "library section, limited to that folder), never the whole library. Used only while the Plex address and "
	  "token above are set, and only for folders the path map and Plex's library locations reach.",
	  0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN, AXIS_SETTING_BOOLEAN, "Tell Jellyfin which folder changed",
	  "After an import, an upgrade or a removal, ask Jellyfin to rescan the one folder that changed (its \"media "
	  "updated\" request, the one thing Axis ever writes to a media server). Used only while the Jellyfin address "
	  "and key above are set, and only for folders the path map reaches.",
	  0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_NOTIFY_QUIET, AXIS_SETTING_DURATION, "Wait before telling the media servers about a folder",
	  "A folder is sent to the media servers once nothing more has changed in it for this long, so that a "
	  "movie with its subtitles and extras is one request. A failed request is tried again a few times within "
	  "the hour and then dropped, and the page says so.",
	  0, "30s", 1, 3600, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_LOOKBACK, AXIS_SETTING_DURATION, "Count a device's plays from the last",
	  "A device counts toward a library's verdicts if it played that library within this period (devices "
	  "marked always-consider count regardless). It must not be longer than the time play history is kept.",
	  0, "180d", 86400, 3650 * 86400, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_RETENTION, AXIS_SETTING_DURATION, "Keep play history for",
	  "Observed sessions and imported plays older than this are removed.",
	  0, "400d", 7 * 86400, 3650 * 86400, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_ACCOUNT_NAMES, AXIS_SETTING_BOOLEAN, "Store the media servers' user names",
	  "Keep the media server's user names with observed sessions. Off by default: only an opaque account id "
	  "is stored. Names already stored are not removed when this is switched off.",
	  0, "false", 0, 0, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_POLL_INTERVAL, AXIS_SETTING_DURATION, "Look at what is playing every",
	  "How often the sessions in progress are looked at while something is playing.",
	  0, "30s", 30, 3600, 0, 0, NULL },
	{ AXIS_COMPAT_SETTING_IDLE_INTERVAL, AXIS_SETTING_DURATION, "Longest wait between looks while nothing plays",
	  "The longest the interval grows to while nothing is playing (it doubles with each empty look).",
	  0, "5m", 30, 86400, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_compat_get_setting_table(void)
{
	/* The addresses are checked wherever they are written. */
	axis_setting_set_validator(AXIS_COMPAT_SETTING_ADDRESS, axis_compat_address_validate);
	axis_setting_set_validator(AXIS_COMPAT_JELLYFIN_ADDRESS, axis_compat_address_validate);
	return settings;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisCompat *
axis_compat_new(
	AxisDbPool    *pool,
	AxisStorage   *storage,
	AxisSettings  *settings_store,
	AxisEstimator *estimator
){
	AxisCompat *self;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisCompat, 1);
	self->refs = 1;
	self->pool = pool;
	self->storage = storage != NULL ? axis_storage_ref(storage) : NULL;
	self->settings = settings_store != NULL ? axis_settings_ref(settings_store) : NULL;
	self->estimator = estimator != NULL ? axis_estimator_ref(estimator) : NULL;
	self->tuning.request_timeout_ms = AXIS_HTTP_DEFAULT_TIMEOUT_MS;
	self->tuning.max_body_bytes = AXIS_HTTP_DEFAULT_MAX_BYTES;
	self->tuning.max_session_rows = AXIS_COMPAT_MAX_SESSION_ROWS;
	return self;
}

AxisCompat *
axis_compat_ref(AxisCompat *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_compat_unref(AxisCompat *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	if (self->storage != NULL)
		axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	g_free(self);
}

void
axis_compat_set_tuning(
	AxisCompat             *self,
	const AxisCompatTuning *tuning
){
	if (tuning->request_timeout_ms != 0)
		self->tuning.request_timeout_ms = tuning->request_timeout_ms;
	if (tuning->max_body_bytes != 0)
		self->tuning.max_body_bytes = tuning->max_body_bytes;
	if (tuning->max_session_rows != 0)
		self->tuning.max_session_rows = tuning->max_session_rows;
}

void
axis_compat_set_enabled(
	AxisCompat *self,
	gboolean    enabled
){
	g_atomic_int_set(&self->enabled, enabled ? 1 : 0);
}

gboolean
axis_compat_is_enabled(AxisCompat *self)
{
	return self != NULL && g_atomic_int_get(&self->enabled) != 0;
}

AxisDbPool *
axis_compat_get_pool(AxisCompat *self)
{
	return self->pool;
}

AxisEstimator *
axis_compat_get_estimator(AxisCompat *self)
{
	return self->estimator;
}

/* ── Kinds and handles ──────────────────────────────────────────── */

guint
axis_compat_n_kinds(void)
{
	guint n = 0;

	while (axis_compat_kind_at(n) != NULL)
		n++;
	return n;
}

const gchar *
axis_compat_kind_name(guint index)
{
	const AxisCompatKind *kind = axis_compat_kind_at(index);

	return kind != NULL ? kind->name : NULL;
}

const gchar *
axis_compat_kind_label(const gchar *name)
{
	const AxisCompatKind *kind = axis_compat_kind_find(name);

	return kind != NULL ? kind->label : NULL;
}

gboolean
axis_compat_kind_has_history(const gchar *name)
{
	const AxisCompatKind *kind = axis_compat_kind_find(name);

	return kind != NULL && kind->has_history;
}

const gchar *
axis_compat_kind_product(const gchar *name)
{
	const AxisCompatKind *kind = axis_compat_kind_find(name);

	return kind != NULL ? kind->product : NULL;
}

gchar *
axis_compat_handle(
	const gchar *kind,
	const gchar *id
){
	if (kind == NULL || g_strcmp0(kind, AXIS_COMPAT_KIND_PLEX) == 0)
		return g_strdup(id);
	return g_strdup_printf("%s~%s", kind, id);
}

gboolean
axis_compat_handle_split(
	const gchar  *handle,
	gchar       **kind,
	gchar       **id
){
	const gchar *tilde;
	g_autofree gchar *name = NULL;
	const gchar *rest;

	if (handle == NULL)
		return FALSE;
	tilde = strchr(handle, '~');
	if (tilde == NULL) {
		name = g_strdup(AXIS_COMPAT_KIND_PLEX);
		rest = handle;
	} else {
		name = g_strndup(handle, (gsize)(tilde - handle));
		rest = tilde + 1;
	}
	if (axis_compat_kind_find(name) == NULL || !axis_compat_plex_id_valid(rest))
		return FALSE;
	if (kind != NULL)
		*kind = g_steal_pointer(&name);
	if (id != NULL)
		*id = g_strdup(rest);
	return TRUE;
}

/* ── The connection ─────────────────────────────────────────────── */

void
axis_compat_connection_clear(AxisCompatConnection *connection)
{
	if (connection == NULL)
		return;
	axis_compat_address_clear(&connection->address);
	/* The secret does not linger in freed memory. */
	if (connection->token != NULL)
		memset(connection->token, 0, strlen(connection->token));
	if (connection->auth_value != NULL)
		memset(connection->auth_value, 0, strlen(connection->auth_value));
	g_clear_pointer(&connection->token, g_free);
	g_clear_pointer(&connection->auth_value, g_free);
	connection->kind = NULL;
}

gboolean
axis_compat_connection_load(
	AxisCompat            *self,
	const AxisCompatKind  *kind,
	AxisCompatConnection  *connection,
	GError               **error
){
	g_autofree gchar *address = NULL;
	g_autofree gchar *problem = NULL;

	memset(connection, 0, sizeof(*connection));
	if (self->settings == NULL) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_CONFIGURED,
			"the media server connection is not set up");
		return FALSE;
	}
	address = axis_settings_get_string(self->settings, kind->address_setting);
	connection->token = axis_settings_get_string(self->settings, kind->token_setting);
	if (address == NULL || address[0] == '\0' || connection->token == NULL || connection->token[0] == '\0') {
		axis_compat_connection_clear(connection);
		g_set_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_CONFIGURED,
			"the %s server address and token are not both set", kind->label);
		return FALSE;
	}
	if (kind->secret_usable != NULL && !kind->secret_usable(connection->token)) {
		axis_compat_connection_clear(connection);
		g_set_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_CONFIGURED,
			"the %s server token has characters it cannot be sent with", kind->label);
		return FALSE;
	}
	if (!axis_compat_address_parse(address, &connection->address, &problem)) {
		axis_compat_connection_clear(connection);
		g_set_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_CONFIGURED,
			"the %s server address %s", kind->label, problem != NULL ? problem : "is not valid");
		return FALSE;
	}
	connection->kind = kind;
	connection->auth_value = g_strconcat(kind->auth_prefix != NULL ? kind->auth_prefix : "", connection->token,
		kind->auth_suffix != NULL ? kind->auth_suffix : "", NULL);
	connection->untrusted = axis_settings_get_boolean(self->settings, kind->untrusted_setting);
	return TRUE;
}

gboolean
axis_compat_is_configured_kind(
	AxisCompat  *self,
	const gchar *name
){
	const AxisCompatKind *kind = axis_compat_kind_find(name);
	AxisCompatConnection connection;
	gboolean ok;

	if (kind == NULL)
		return FALSE;
	ok = axis_compat_connection_load(self, kind, &connection, NULL);
	axis_compat_connection_clear(&connection);
	return ok;
}

gboolean
axis_compat_is_configured(AxisCompat *self)
{
	guint i;

	for (i = 0; axis_compat_kind_at(i) != NULL; i++) {
		if (axis_compat_is_configured_kind(self, axis_compat_kind_at(i)->name))
			return TRUE;
	}
	return FALSE;
}

gchar *
axis_compat_describe_address_kind(
	AxisCompat  *self,
	const gchar *name
){
	const AxisCompatKind *kind = axis_compat_kind_find(name);
	AxisCompatConnection connection;
	gchar *base = NULL;

	/* Cleared below on every path, including the ones that never load it. */
	memset(&connection, 0, sizeof(connection));
	if (kind != NULL && axis_compat_connection_load(self, kind, &connection, NULL))
		base = g_strdup(connection.address.base);
	axis_compat_connection_clear(&connection);
	return base;
}

gchar *
axis_compat_describe_address(AxisCompat *self)
{
	return axis_compat_describe_address_kind(self, AXIS_COMPAT_KIND_PLEX);
}

gboolean
axis_compat_fetch(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	const gchar                *path,
	const gchar *const         *headers,
	GCancellable               *cancellable,
	GBytes                    **body,
	GError                    **error
){
	AxisHttpConfig config;
	g_autoptr(GError) local = NULL;

	memset(&config, 0, sizeof(config));
	config.origin = &connection->address;
	config.auth_header = connection->kind->auth_header;
	config.auth_value = connection->auth_value;
	config.token = connection->token;
	config.accept_untrusted_certificate = connection->untrusted;
	config.timeout_ms = self->tuning.request_timeout_ms;
	config.max_bytes = self->tuning.max_body_bytes;
	config.cancellable = cancellable;
	/* Bound only with outbound.bind_scope = all (core/axis-outbound.h). */
	config.traffic = AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER;
	if (axis_http_get(&config, path, headers, body, &local))
		return TRUE;
	if (local->domain == AXIS_HTTP_ERROR && local->code == AXIS_HTTP_ERROR_CANCELLED)
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
	else
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_SERVER, local->message);
	return FALSE;
}

void
axis_compat_set_server_error(
	GError      **error,
	const gchar  *what,
	const gchar  *problem
){
	g_set_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_SERVER,
		"the media server's answer to %s could not be used: %s", what, problem != NULL ? problem : "it is not valid");
}

gboolean
axis_compat_fetch_identity(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	AxisCompatIdentity         *identity,
	GError                    **error
){
	memset(identity, 0, sizeof(*identity));
	return connection->kind->fetch_identity(self, connection, cancellable, identity, error);
}

void
axis_compat_test_result_clear(AxisCompatTestResult *result)
{
	if (result == NULL)
		return;
	g_clear_pointer(&result->name, g_free);
	g_clear_pointer(&result->version, g_free);
	g_clear_pointer(&result->message, g_free);
}

void
axis_compat_test_connection_kind(
	AxisCompat           *self,
	const gchar          *name,
	GCancellable         *cancellable,
	AxisCompatTestResult *result
){
	const AxisCompatKind *kind = axis_compat_kind_find(name);
	AxisCompatConnection connection;
	AxisCompatIdentity identity;
	g_autoptr(GError) error = NULL;

	memset(result, 0, sizeof(*result));
	if (kind == NULL) {
		result->message = g_strdup("that is not a kind of media server");
		return;
	}
	if (!axis_compat_connection_load(self, kind, &connection, &error)) {
		result->message = g_strdup(error->message);
		return;
	}
	if (!axis_compat_fetch_identity(self, &connection, cancellable, &identity, &error)) {
		result->message = axis_http_scrub(error->message, connection.token, 300);
	} else {
		result->ok = TRUE;
		result->name = identity.name != NULL ? g_strdup(identity.name) : NULL;
		result->version = g_strdup(identity.version);
		axis_compat_identity_clear(&identity);
	}
	axis_compat_connection_clear(&connection);
}

void
axis_compat_test_connection(
	AxisCompat           *self,
	GCancellable         *cancellable,
	AxisCompatTestResult *result
){
	axis_compat_test_connection_kind(self, AXIS_COMPAT_KIND_PLEX, cancellable, result);
}

/* ── Failures ───────────────────────────────────────────────────── */

void
axis_compat_note_failure(
	AxisCompat   *self,
	const gchar  *kind,
	const GError *error,
	gboolean      history
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *message = axis_library_text_clean(error != NULL ? error->message : "failed", 500);
	g_autofree gchar *interval = NULL;
	const gchar *params[4];
	AxisDbConn *conn;

	if (self->settings != NULL)
		interval = g_strdup_printf("%" G_GINT64_FORMAT, axis_settings_get_duration(self->settings,
			AXIS_COMPAT_SETTING_POLL_INTERVAL));
	else
		interval = g_strdup("30");
	params[0] = message;
	params[1] = interval;
	params[2] = kind;
	params[3] = NULL;
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL)
		return;
	axis_compat_ensure_state(conn, kind, NULL);
	if (history)
		axis_db_conn_exec(conn, "UPDATE compat_state SET history_error = $1::text WHERE server_kind = $3::text", 3,
			params, NULL, &local);
	else
		axis_db_conn_exec(conn,
			"UPDATE compat_state SET last_poll_at = now(), last_error = $1::text, failures = failures + 1,"
			" next_poll_at = now() + least($2::double precision * power(2, least(failures, 10)), "
			G_STRINGIFY(AXIS_COMPAT_FAILURE_BACKOFF_MAX_SECONDS) ") * interval '1 second'"
			" WHERE server_kind = $3::text",
			3, params, NULL, &local);
	axis_db_pool_release(self->pool, conn);
}

/* ── Jobs ───────────────────────────────────────────────────────── */

gboolean
axis_compat_run_poll_job(
	AxisCompat      *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	gboolean force = payload != NULL && json_object_get_boolean_member_with_default(payload, "force", FALSE);
	AxisCompatPollSummary summary;

	return axis_compat_poll(self, force, axis_job_context_get_cancellable(context), &summary, error);
}

gboolean
axis_compat_run_history_job(
	AxisCompat      *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	gboolean force = payload != NULL && json_object_get_boolean_member_with_default(payload, "force", FALSE);
	AxisCompatHistorySummary summary;

	return axis_compat_import_history(self, force, axis_job_context_get_cancellable(context), &summary, error);
}

static gboolean
enqueue(
	AxisCompat  *self,
	const gchar *kind,
	gint64      *job_id,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *dedupe = g_strconcat(kind, ":run", NULL);
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	json_object_set_boolean_member(payload, "force", TRUE);
	memset(&spec, 0, sizeof(spec));
	spec.kind = kind;
	spec.queue = AXIS_COMPAT_QUEUE;
	spec.payload = payload;
	spec.priority = AXIS_COMPAT_REQUEST_PRIORITY;
	spec.dedupe_key = dedupe;
	spec.max_attempts = 1;
	if (job_id != NULL)
		*job_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_enqueue(conn, &spec, job_id, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_compat_request_history(
	AxisCompat  *self,
	gint64      *job_id,
	GError     **error
){
	return enqueue(self, AXIS_COMPAT_HISTORY_KIND, job_id, error);
}

gboolean
axis_compat_request_poll(
	AxisCompat  *self,
	gint64      *job_id,
	GError     **error
){
	return enqueue(self, AXIS_COMPAT_POLL_KIND, job_id, error);
}
