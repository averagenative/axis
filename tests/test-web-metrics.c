/*
 * test-web-metrics.c - the metrics endpoint's token check, its text, and
 * the throttle on wrong tokens (the last through the real shell on a
 * loopback port), without a database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-metrics.h"

#define TOKEN "0123456789abcdef0123456789abcdef"

/* Spec (deployment): Off by default -- and with a token, only that token. */
static void
test_authorized(void)
{
	g_autoptr(AxisWebMetrics) off = axis_web_metrics_new(NULL);
	g_autoptr(AxisWebMetrics) empty = axis_web_metrics_new("");
	g_autoptr(AxisWebMetrics) on = axis_web_metrics_new(TOKEN);

	g_assert_false(axis_web_metrics_is_enabled(off));
	g_assert_false(axis_web_metrics_is_enabled(empty));
	g_assert_true(axis_web_metrics_is_enabled(on));

	g_assert_false(axis_web_metrics_authorized(off, "Bearer " TOKEN));
	g_assert_false(axis_web_metrics_authorized(off, NULL));

	g_assert_true(axis_web_metrics_authorized(on, "Bearer " TOKEN));
	g_assert_true(axis_web_metrics_authorized(on, "bearer " TOKEN));
	g_assert_false(axis_web_metrics_authorized(on, NULL));
	g_assert_false(axis_web_metrics_authorized(on, ""));
	g_assert_false(axis_web_metrics_authorized(on, TOKEN));
	g_assert_false(axis_web_metrics_authorized(on, "Basic " TOKEN));
	g_assert_false(axis_web_metrics_authorized(on, "Bearer " TOKEN "0"));
	g_assert_false(axis_web_metrics_authorized(on, "Bearer 0123456789abcdef0123456789abcdee"));
	g_assert_false(axis_web_metrics_authorized(on, "Bearer  " TOKEN));
	g_assert_false(axis_web_metrics_authorized(on, "Bearer " TOKEN ", Bearer " TOKEN));
}

/* The text: counters by class, a cumulative histogram, and the database
 * reported down (with none of its series) when it cannot be asked. */
static void
test_render(void)
{
	g_autoptr(AxisWebMetrics) metrics = axis_web_metrics_new(TOKEN);
	g_autofree gchar *text = NULL;

	axis_web_metrics_record(metrics, 200, 1000);		/* 1 ms */
	axis_web_metrics_record(metrics, 204, 30000);		/* 30 ms */
	axis_web_metrics_record(metrics, 303, 4000);
	axis_web_metrics_record(metrics, 404, 20 * G_USEC_PER_SEC);	/* past the last bound */
	axis_web_metrics_record(metrics, 503, 2000);
	axis_web_metrics_record(metrics, 200, -1);		/* a stream */
	axis_web_metrics_record(metrics, 0, 1000);		/* no class */
	text = axis_web_metrics_render(metrics, NULL);

	g_assert_nonnull(strstr(text, "# TYPE axis_http_requests_total counter\n"));
	g_assert_nonnull(strstr(text, "axis_http_requests_total{code=\"2xx\"} 3\n"));
	g_assert_nonnull(strstr(text, "axis_http_requests_total{code=\"3xx\"} 1\n"));
	g_assert_nonnull(strstr(text, "axis_http_requests_total{code=\"4xx\"} 1\n"));
	g_assert_nonnull(strstr(text, "axis_http_requests_total{code=\"5xx\"} 1\n"));
	g_assert_nonnull(strstr(text, "axis_http_requests_total{code=\"other\"} 1\n"));
	g_assert_nonnull(strstr(text, "axis_http_request_duration_seconds_bucket{le=\"0.005\"} 4\n"));
	g_assert_nonnull(strstr(text, "axis_http_request_duration_seconds_bucket{le=\"0.05\"} 5\n"));
	g_assert_nonnull(strstr(text, "axis_http_request_duration_seconds_bucket{le=\"10\"} 5\n"));
	g_assert_nonnull(strstr(text, "axis_http_request_duration_seconds_sum 20.038000\n"));
	g_assert_nonnull(strstr(text, "axis_http_request_duration_seconds_bucket{le=\"+Inf\"} 6\n"));
	g_assert_nonnull(strstr(text, "axis_http_request_duration_seconds_count 6\n"));
	g_assert_nonnull(strstr(text, "axis_http_streams_total 1\n"));
	g_assert_nonnull(strstr(text, "axis_build_info{version=\""));
	g_assert_nonnull(strstr(text, "axis_database_up 0\n"));
	g_assert_null(strstr(text, "axis_jobs"));
	g_assert_null(strstr(text, TOKEN));
	/* Every line is a comment or "name[{labels}] value". */
	{
		g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
		guint i;

		for (i = 0; lines[i] != NULL; i++) {
			if (lines[i][0] == '\0' || lines[i][0] == '#')
				continue;
			g_assert_true(g_str_has_prefix(lines[i], "axis_"));
			g_assert_nonnull(strrchr(lines[i], ' '));
		}
	}
}

/* GET /metrics with @token as the bearer token (NULL: none). */
static guint
scrape(
	SoupSession  *session,
	guint16       port,
	const gchar  *token,
	gchar       **retry_after
){
	g_autofree gchar *bearer = token != NULL ? g_strconcat("Bearer ", token, NULL) : NULL;
	const gchar *const extra[] = { "Authorization", bearer, NULL };
	AxisTestReply reply = axis_test_web_request(session, port, "GET", "/metrics", NULL,
		token != NULL ? extra : NULL, NULL);
	guint status = reply.status;

	if (retry_after != NULL)
		*retry_after = g_strdup(axis_test_reply_header(&reply, "Retry-After"));
	axis_test_reply_clear(&reply);
	return status;
}

/*
 * Found by the stage 2 hunt of the web layer (W-8): wrong metrics tokens
 * were never throttled.  As at the worker API, an address gets ten, then
 * waits, and is answered 429 without its token being looked at.
 */
static void
test_wrong_tokens_throttled(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(HtmxServer) server = htmx_server_new();
	g_autoptr(AxisWebMetrics) metrics = axis_web_metrics_new(TOKEN);
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 20, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *retry = NULL;
	AxisWebAuth *auth;
	AxisWeb *web;
	guint16 port;
	guint i;

	axis_test_log_init();
	axis_test_log_reset();
	g_assert_true(htmx_server_listen_on(server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	web = axis_web_new(server, registry, auth);
	axis_web_metrics_register(metrics, web, NULL);
	g_assert_true(htmx_server_start(server, &error));
	g_assert_no_error(error);
	port = axis_test_web_port(server);

	g_assert_cmpuint(scrape(session, port, TOKEN, NULL), ==, 200);
	/* No token at all is not a guess, nor is another scheme, nor a bearer
	 * token that is not of the metrics token's form (a forward-auth
	 * proxy's): none of them counts. */
	g_assert_cmpuint(scrape(session, port, NULL, NULL), ==, 401);
	for (i = 0; i < 20; i++) {
		static const gchar *const basic[] = { "Authorization", "Basic YWxpY2U6c2VjcmV0", NULL };
		AxisTestReply reply = axis_test_web_request(session, port, "GET", "/metrics", NULL, basic, NULL);

		g_assert_cmpuint(reply.status, ==, 401);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(scrape(session, port, "short", NULL), ==, 401);

	for (i = 0; i < 11; i++)
		g_assert_cmpuint(scrape(session, port, "0123456789abcdef0123456789abcdee", NULL), ==, 401);

	/* The address waits now: its wrong tokens are refused 429 ... */
	g_assert_cmpuint(scrape(session, port, "0123456789abcdef0123456789abcdee", &retry), ==, 429);
	g_assert_nonnull(retry);
	g_assert_cmpint(g_ascii_strtoll(retry, NULL, 10), >=, 1);
	/* ... and the right one always works (found by the review of
	 * fix-web-hunt-findings: a scanner beside Prometheus locked it out). */
	g_assert_cmpuint(scrape(session, port, TOKEN, NULL), ==, 200);
	/* The wrong ones were logged once, not twelve times. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "metrics: *token that is not the metrics token*"), ==, 1);

	htmx_server_stop(server);
	axis_web_metrics_drain(metrics);
	axis_web_free(web);
	axis_web_auth_free(auth);
	axis_test_log_reset();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-metrics/authorized", test_authorized);
	g_test_add_func("/web-metrics/render", test_render);
	g_test_add_func("/web-metrics/wrong-tokens-throttled", test_wrong_tokens_throttled);

	return g_test_run();
}
