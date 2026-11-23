/*
 * axis-arr-client.c - the requests to a person's Radarr or Sonarr: GET only,
 * the key in its header, to the connection's own origin
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The only place a connection's key is handed to anything: the HTTP
 * sessions made for that connection's origin, which send it only there, in
 * the header the app takes it in, and blank it out of every message.  The
 * sessions are of purpose %AXIS_HTTP_PURPOSE_READ: asked to send anything
 * but a GET they fail without sending.
 *
 * Every error made here is fixed words that name the app: what the app
 * answered, and what the HTTP library said, are never part of one, so a
 * page that shows the error cannot be used to read another service on the
 * network.
 */

#include "arr/axis-arr-private.h"

#include <string.h>

#include "arr/axis-arr-assumed.h"
#include "axis-version.h"

#define USER_AGENT "Axis/" AXIS_VERSION

struct _AxisArrClient
{
	AxisHttpSession *small;
	AxisHttpSession *large;
	AxisHttpSession *series;	/* one series' episodes or files; Sonarr only, else NULL */
	gchar           *prefix;
	AxisArrApp       app;
	gsize            list_max_bytes;
	gsize            series_max_bytes;
};

gchar *
axis_arr_queue_path(
	AxisArrApp app,
	guint      page,
	guint      page_size
){
	return g_strdup_printf(app == AXIS_ARR_SONARR ? AXIS_ARR_QUEUE_PATH_SONARR : AXIS_ARR_QUEUE_PATH, page, page_size);
}

AxisArrClient *
axis_arr_client_new(
	AxisArr              *self,
	const AxisArrSource  *source,
	const gchar          *secret,
	GCancellable         *cancellable,
	GError              **error
){
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	AxisArrClient *client;
	AxisArrTuning tuning;
	g_autofree gchar *problem = NULL;

	if (secret == NULL || secret[0] == '\0') {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "No API key is stored for this connection.");
		return NULL;
	}
	if (!axis_http_origin_parse(source->origin, &origin, &problem)) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "The connection's address cannot be used.");
		return NULL;
	}
	axis_arr_get_tuning(self, &tuning);
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	config.auth_header = AXIS_ARR_KEY_HEADER;
	config.auth_value = secret;
	config.token = secret;
	config.accept_untrusted_certificate = source->accept_untrusted;
	config.timeout_ms = tuning.request_timeout_ms;
	config.max_bytes = AXIS_ARR_ANSWER_MAX_BYTES;
	config.cancellable = cancellable;
	config.peer = axis_arr_app_label(source->app);
	config.secret_name = "API key";
	config.user_agent = USER_AGENT;
	config.accept = "application/json";
	/* A local service, as a download client is: bound only with
	 * outbound.bind_scope = all (core/axis-outbound.h). */
	config.traffic = AXIS_OUTBOUND_TRAFFIC_ARR;

	client = g_new0(AxisArrClient, 1);
	client->app = source->app;
	client->prefix = g_strdup(source->path_prefix != NULL ? source->path_prefix : "");
	client->list_max_bytes = tuning.list_max_bytes;
	client->small = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_READ);
	config.timeout_ms = tuning.list_timeout_ms;
	config.max_bytes = tuning.list_max_bytes;
	client->large = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_READ);
	if (source->app == AXIS_ARR_SONARR) {
		config.timeout_ms = tuning.series_timeout_ms;
		config.max_bytes = tuning.series_max_bytes;
		client->series_max_bytes = tuning.series_max_bytes;
		client->series = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_READ);
	}
	axis_http_origin_clear(&origin);
	return client;
}

void
axis_arr_client_free(AxisArrClient *client)
{
	if (client == NULL)
		return;
	g_clear_pointer(&client->small, axis_http_session_free);
	g_clear_pointer(&client->large, axis_http_session_free);
	g_clear_pointer(&client->series, axis_http_session_free);
	g_free(client->prefix);
	g_free(client);
}

AxisHttpSession *
axis_arr_client_peek_session(AxisArrClient *client)
{
	return client->small;
}

/* The outbound client's failure in this layer's fixed words. */
static void
set_error(
	AxisArrClient  *client,
	AxisArrGet      kind,
	const GError   *http,
	guint           status,
	GError        **error
){
	const gchar *app = axis_arr_app_label(client->app);
	gint code = http->domain == AXIS_HTTP_ERROR ? http->code : AXIS_HTTP_ERROR_FAILED;

	switch (code) {
	case AXIS_HTTP_ERROR_CONNECT:
	case AXIS_HTTP_ERROR_TIMEOUT:
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_ANSWER, "%s did not answer.", app);
		break;
	case AXIS_HTTP_ERROR_TLS:
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_CERTIFICATE, "%s's certificate was not accepted.", app);
		break;
	case AXIS_HTTP_ERROR_TOO_LARGE:
		if (kind == AXIS_ARR_GET_SERIES)
			g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_TOO_LARGE, "%s's library is too large for this version of "
				"Axis to migrate (its answer for one series is over %u MB).", app,
				(guint)(client->series_max_bytes / (1024 * 1024)));
		else
			g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_TOO_LARGE, "%s's library is too large for this version of Axis to migrate "
				"(its answer is over %u MB).", app, (guint)(client->list_max_bytes / (1024 * 1024)));
		break;
	case AXIS_HTTP_ERROR_CANCELLED:
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_CANCELLED, "The request was stopped.");
		break;
	case AXIS_HTTP_ERROR_CONFIG:
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "The connection's address cannot be used.");
		break;
	case AXIS_HTTP_ERROR_STATUS:
		if (status == AXIS_ARR_STATUS_KEY_REFUSED || status == AXIS_ARR_STATUS_KEY_REFUSED2) {
			g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_REFUSED, "%s refused the API key.", app);
			break;
		}
		/* An app that errs (a restart, a busy moment) has not answered: one
		 * list of one series is asked for again. */
		if (kind == AXIS_ARR_GET_SERIES && status >= 500) {
			g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_ANSWER, "%s did not answer.", app);
			break;
		}
		/* fall through */
	default:
		/* Another status, a redirect elsewhere, anything else. */
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_APP, "This address answers, but not as %s.", app);
		break;
	}
}

gboolean
axis_arr_client_get(
	AxisArrClient  *client,
	const gchar    *path,
	AxisArrGet      kind,
	GBytes        **body,
	GError        **error
){
	g_autofree gchar *full = g_strconcat(client->prefix, path, NULL);
	AxisHttpSession *session = kind == AXIS_ARR_GET_LIBRARY ? client->large :
		kind == AXIS_ARR_GET_SERIES && client->series != NULL ? client->series : client->small;
	AxisHttpResponse response;
	GError *local = NULL;

	*body = NULL;
	if (axis_http_session_get(session, full, NULL, body, &response, &local)) {
		axis_http_response_clear(&response);
		return TRUE;
	}
	set_error(client, kind, local, response.status, error);
	g_error_free(local);
	axis_http_response_clear(&response);
	g_clear_pointer(body, g_bytes_unref);
	return FALSE;
}

gboolean
axis_arr_client_status(
	AxisArrClient  *client,
	gchar         **version,
	GError        **error
){
	g_autoptr(GBytes) body = NULL;
	const gchar *data;
	gsize length = 0;

	if (version != NULL)
		*version = NULL;
	if (!axis_arr_client_get(client, AXIS_ARR_STATUS_PATH, AXIS_ARR_GET_SMALL, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	if (!axis_arr_parse_status(data, length, client->app, version)) {
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_APP, "This address answers, but not as %s.",
			axis_arr_app_label(client->app));
		return FALSE;
	}
	return TRUE;
}

AxisArrTest
axis_arr_test_of_error(const GError *error)
{
	if (error == NULL || error->domain != AXIS_ARR_ERROR)
		return AXIS_ARR_TEST_NO_ANSWER;
	switch (error->code) {
	case AXIS_ARR_ERROR_REFUSED:
		return AXIS_ARR_TEST_REFUSED;
	case AXIS_ARR_ERROR_CERTIFICATE:
		return AXIS_ARR_TEST_CERTIFICATE;
	case AXIS_ARR_ERROR_NOT_APP:
	case AXIS_ARR_ERROR_TOO_LARGE:
	case AXIS_ARR_ERROR_UNREADABLE:
		return AXIS_ARR_TEST_NOT_APP;
	default:
		return AXIS_ARR_TEST_NO_ANSWER;
	}
}
