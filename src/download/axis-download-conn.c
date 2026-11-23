/*
 * axis-download-conn.c - a connection to one configured download client
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The only place a client's secret is read (through the store) and handed
 * to anything: the HTTP session made for that client's own origin, which
 * sends it only there and blanks it out of every message.
 */

#include "download/axis-download-private.h"

#include <string.h>

#include "axis-version.h"

#define USER_AGENT "Axis/" AXIS_VERSION

/* ── Errors ─────────────────────────────────────────────────────── */

/* The outbound client's error as the layer's; its message (already free
 * of the secret, the cookies and any query) is kept. */
static void
map_error(
	GError  *http,
	GError **error
){
	AxisDownloadError code = AXIS_DOWNLOAD_ERROR_FAILED;

	if (http->domain == AXIS_HTTP_ERROR) {
		switch (http->code) {
		case AXIS_HTTP_ERROR_CONNECT:
		case AXIS_HTTP_ERROR_TIMEOUT:
		case AXIS_HTTP_ERROR_TLS:
			code = AXIS_DOWNLOAD_ERROR_UNREACHABLE;
			break;
		case AXIS_HTTP_ERROR_TOO_LARGE:
			code = AXIS_DOWNLOAD_ERROR_UNREADABLE;
			break;
		case AXIS_HTTP_ERROR_CANCELLED:
			code = AXIS_DOWNLOAD_ERROR_CANCELLED;
			break;
		case AXIS_HTTP_ERROR_CONFIG:
			code = AXIS_DOWNLOAD_ERROR_INVALID;
			break;
		default:
			code = AXIS_DOWNLOAD_ERROR_FAILED;
			break;
		}
	}
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, (gint)code, http->message);
}

static gboolean
finish(
	AxisDownloadConn  *conn,
	gboolean           ok,
	AxisHttpResponse  *response,
	GError            *local,
	GError           **error
){
	conn->last_status = response->status;
	g_strlcpy(conn->last_refusal, response->refusal != NULL ? response->refusal : "", sizeof(conn->last_refusal));
	if (response->sent)
		conn->sent = TRUE;
	axis_http_response_clear(response);
	if (ok)
		return TRUE;
	/* A 401 or 403 is the secret refused, whatever the kind. */
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS) &&
	    (conn->last_status == 401 || conn->last_status == 403))
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH, local->message);
	else
		map_error(local, error);
	g_error_free(local);
	return FALSE;
}

/* @path under the client's base path: the configured prefix and nothing
 * else (it comes from the administrator, was checked when it was saved, and
 * is never changed by an answer).  A kind asks for "/api"; the client
 * served under "/sabnzbd" is asked for "/sabnzbd/api". */
static gchar *
prefixed(
	AxisDownloadConn *conn,
	const gchar      *path
){
	if (conn->base_path == NULL || conn->base_path[0] == '\0')
		return g_strdup(path);
	return g_strconcat(conn->base_path, path, NULL);
}

gboolean
axis_download_conn_post(
	AxisDownloadConn    *conn,
	const gchar         *path,
	const gchar         *content_type,
	GBytes              *body,
	const gchar *const  *headers,
	GBytes             **answer,
	GError             **error
){
	AxisHttpResponse response;
	g_autofree gchar *full = NULL;
	GError *local = NULL;
	gboolean ok;

	g_return_val_if_fail(conn != NULL && answer != NULL, FALSE);

	if (conn->session == NULL) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "a built-in client is asked nothing");
		return FALSE;
	}
	full = prefixed(conn, path);
	ok = axis_http_session_post(conn->session, full, content_type, body, headers, answer, &response, &local);
	return finish(conn, ok, &response, local, error);
}

gboolean
axis_download_conn_get(
	AxisDownloadConn    *conn,
	const gchar         *path,
	const gchar *const  *headers,
	GBytes             **answer,
	GError             **error
){
	AxisHttpResponse response;
	g_autofree gchar *full = NULL;
	GError *local = NULL;
	gboolean ok;

	g_return_val_if_fail(conn != NULL && answer != NULL, FALSE);

	if (conn->session == NULL) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "a built-in client is asked nothing");
		return FALSE;
	}
	full = prefixed(conn, path);
	ok = axis_http_session_get(conn->session, full, headers, answer, &response, &local);
	return finish(conn, ok, &response, local, error);
}

const gchar *
axis_download_conn_get_secret(AxisDownloadConn *conn)
{
	return conn->secret != NULL && conn->secret[0] != '\0' ? conn->secret : NULL;
}

const gchar *
axis_download_conn_get_username(AxisDownloadConn *conn)
{
	return conn->username != NULL && conn->username[0] != '\0' ? conn->username : NULL;
}

const gchar *
axis_download_conn_get_address(AxisDownloadConn *conn)
{
	return conn->address;
}

guint
axis_download_conn_get_last_status(AxisDownloadConn *conn)
{
	return conn->last_status;
}

const gchar *
axis_download_conn_get_last_refusal(AxisDownloadConn *conn)
{
	return conn->last_refusal;
}

gboolean
axis_download_conn_has_session(AxisDownloadConn *conn)
{
	return axis_http_session_n_cookies(conn->session) > 0;
}

void
axis_download_conn_forget_session(AxisDownloadConn *conn)
{
	axis_http_session_forget_cookies(conn->session);
}

const gchar *
axis_download_conn_get_label(AxisDownloadConn *conn)
{
	return conn->label;
}

void
axis_download_conn_set_list_partial(AxisDownloadConn *conn)
{
	conn->list_partial = TRUE;
}

const gchar *
axis_download_conn_get_category(AxisDownloadConn *conn)
{
	return conn->category;
}

/* ── With a client ──────────────────────────────────────────────── */

static gboolean
load_target(
	AxisDownloads       *self,
	gint64               id,
	AxisDownloadTarget  *target,
	GError             **error
){
	g_autoptr(GPtrArray) clients = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gint64 set_us = 0;
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_download_store_clients(conn, id, &clients, error) &&
		(clients->len == 0 || axis_download_store_secret(conn, id, &target->secret, &set_us, error));
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (clients->len == 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND, "there is no such download client");
		return FALSE;
	}
	target->info = g_ptr_array_steal_index(clients, 0);
	target->stamp = MAX(target->info->updated_us, set_us);
	return TRUE;
}

static AxisHttpSession *
session_for(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	GError                    **error
){
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	AxisHttpSession *session;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *secret_name = g_ascii_strdown(kind->secret_label, -1);

	if (!axis_http_origin_parse(target->info->address, &origin, &problem)) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "the client's address %s",
			problem != NULL ? problem : "cannot be used");
		return NULL;
	}
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	/* The kind places the secret itself (a header, a login body); the
	 * session only needs to know it, to blank it out of messages. */
	config.token = target->secret;
	config.accept_untrusted_certificate = target->info->accept_untrusted;
	config.timeout_ms = self->tuning.request_timeout_ms != 0 ? self->tuning.request_timeout_ms :
		AXIS_DOWNLOAD_REQUEST_TIMEOUT_MS;
	config.max_bytes = AXIS_DOWNLOAD_MAX_ANSWER_BYTES;
	config.peer = "the download client";
	config.secret_name = secret_name;
	config.user_agent = USER_AGENT;
	config.accept = "application/json";
	/* Bound only with outbound.bind_scope = all (core/axis-outbound.h). */
	config.traffic = AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT;
	session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	axis_http_origin_clear(&origin);
	return session;
}

static void
on_cancel(
	GCancellable *source,
	gpointer      user_data
){
	g_cancellable_cancel(G_CANCELLABLE(user_data));
}

gboolean
axis_download_with_client(
	AxisDownloads         *self,
	gint64                 id,
	GCancellable          *cancellable,
	AxisDownloadCallFunc   func,
	gpointer               data,
	gboolean              *sent,
	GError               **error
){
	AxisDownloadTarget target;
	const AxisDownloadKindInfo *kind;
	AxisDownloadSlot *state;
	AxisDownloadConn conn;
	g_autoptr(GCancellable) call_cancel = NULL;
	gulong own_handler = 0;
	gulong caller_handler = 0;
	gboolean ok;

	if (sent != NULL)
		*sent = FALSE;
	if (g_atomic_int_get(&self->stopped)) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_CANCELLED, "the daemon is stopping");
		return FALSE;
	}
	memset(&target, 0, sizeof(target));
	if (!load_target(self, id, &target, error))
		return FALSE;
	kind = axis_downloads_find_kind(self, target.info->kind);
	if (kind == NULL) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"this version of Axis has no download client kind called %s", target.info->kind);
		axis_download_target_clear(&target);
		return FALSE;
	}

	state = axis_download_slot_get(self, id);
	g_mutex_lock(&state->call);
	/* A built-in kind is not behind an address: it has no session, and its
	 * connection makes no request. */
	if (!kind->builtin && (state->session == NULL || state->session_stamp != target.stamp)) {
		g_clear_pointer(&state->session, axis_http_session_free);
		state->session = session_for(self, &target, kind, error);
		state->session_stamp = target.stamp;
	}
	if (!kind->builtin && state->session == NULL) {
		g_mutex_unlock(&state->call);
		axis_download_slot_unref(state);
		axis_download_target_clear(&target);
		return FALSE;
	}

	/* Either the daemon stopping or the caller (a job) cancels the call. */
	call_cancel = g_cancellable_new();
	own_handler = g_cancellable_connect(self->cancel, G_CALLBACK(on_cancel), call_cancel, NULL);
	if (cancellable != NULL)
		caller_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancel), call_cancel, NULL);
	if (state->session != NULL)
		axis_http_session_set_cancellable(state->session, call_cancel);

	memset(&conn, 0, sizeof(conn));
	conn.owner = self;
	conn.kind = kind;
	conn.session = state->session;
	conn.secret = target.secret;
	conn.label = target.info->name;
	conn.base_path = target.info->base_path;
	conn.address = target.info->address;
	conn.username = target.info->username;
	conn.category = target.info->category;
	ok = func(self, &target, kind, &conn, data, error);
	if (sent != NULL)
		*sent = conn.sent;

	if (state->session != NULL)
		axis_http_session_set_cancellable(state->session, NULL);
	g_cancellable_disconnect(self->cancel, own_handler);
	if (cancellable != NULL)
		g_cancellable_disconnect(cancellable, caller_handler);
	g_mutex_unlock(&state->call);
	axis_download_slot_unref(state);
	axis_download_target_clear(&target);
	return ok;
}
