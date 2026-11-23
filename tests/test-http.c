/*
 * test-http.c - the daemon's outbound HTTP client (core/axis-http.h)
 * against a fake server on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Moved with the client from compat/ (it was test-compat-http.c); the cases
 * are the same.  Spec scenarios of client-compatibility:
 *   Token stays secret (sent as a header only, to the configured host only)
 *   Redirect to another host refused (and the token is not sent there)
 *   Bad responses are survivable (oversized, never-ending, slow, silent)
 * and the rest of the client's promises: GET only, a same-host redirect is
 * followed a few times at most, proxy environment variables are ignored,
 * certificates are verified unless the administrator says otherwise, and
 * no message holds the token.  Added with the move: what the server said
 * beside the body (status, Retry-After, content type), and the names a
 * caller gives the server and its secret in the messages.
 *
 * Added with the download clients (downloaders: Client secrets stay with
 * their client; Redirect): POST only from a download client's session, and
 * never from a media server's or a metadata provider's connection (a
 * session of their purpose refuses it, sending nothing, and no file of
 * theirs names the download purpose); a POST is never followed on any
 * redirect; session cookies are kept in memory, sent back to their origin
 * only, and blanked out of messages; whether a failed request was sent.
 *
 * Added with fix-prowlarr-live (indexers: An indexer's download redirect is
 * followed once, with nothing of Axis's; Inside the network): only a
 * release's fetch follows one redirect off the origin, to https, to a public
 * address checked where it is connected to (the tests allow loopback only
 * through a hook the daemon never names, and the refusals are tested with it
 * off), with the method, path, Host and user agent and nothing else, against
 * the system's authorities, not again, within the size cap; the address is
 * in no message; a Location of a long magnet link is read.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-plex.h"
#include "core/axis-http.h"
#include "core/axis-http-cookie.h"

#define TOKEN "secret-token-0123456789abcdef"

typedef struct
{
	AxisTestPlex   *plex;
	AxisHttpOrigin  origin;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = NULL;
	g_autofree gchar *problem = NULL;

	fx->plex = axis_test_plex_new();
	g_assert_nonnull(fx->plex);
	base = axis_test_plex_base(fx->plex);
	g_assert_true(axis_http_origin_parse(base, &fx->origin, &problem));
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_http_origin_clear(&fx->origin);
	axis_test_plex_free(fx->plex);
}

static AxisHttpConfig
config_for(const Fixture *fx)
{
	AxisHttpConfig config;

	memset(&config, 0, sizeof(config));
	config.origin = &fx->origin;
	config.auth_header = "X-Plex-Token";
	config.auth_value = TOKEN;
	config.token = TOKEN;
	config.timeout_ms = 5000;
	return config;
}

static void
assert_error(
	GError       *error,
	AxisHttpError code
){
	g_assert_nonnull(error);
	g_assert_cmpint(error->domain, ==, AXIS_HTTP_ERROR);
	g_assert_cmpint(error->code, ==, (gint)code);
	/* No message ever holds the token. */
	g_assert_null(strstr(error->message, TOKEN));
}

/* Spec: Token stays secret */
static void
test_get_sends_the_token_as_a_header_only(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *header = NULL;
	gsize length;

	axis_test_plex_standard(fx->plex);
	g_assert_true(axis_http_get(&config, "/identity", NULL, &body, &error));
	g_assert_no_error(error);
	g_assert_nonnull(g_bytes_get_data(body, &length));
	g_assert_nonnull(strstr(g_bytes_get_data(body, NULL), "machineIdentifier"));

	header = axis_test_plex_last_header(fx->plex, "/identity", "X-Plex-Token");
	g_assert_cmpstr(header, ==, TOKEN);
	transcript = axis_test_plex_transcript(fx->plex);
	/* The first line of a request is its method and URL: no token there. */
	g_assert_true(g_str_has_prefix(transcript, "GET /identity\n"));
	g_assert_false(axis_test_plex_saw_non_get(fx->plex));
	/* The media servers' user agent and accept header, as before the move. */
	g_clear_pointer(&header, g_free);
	header = axis_test_plex_last_header(fx->plex, "/identity", "User-Agent");
	g_assert_cmpstr(header, ==, "axisd");
	g_clear_pointer(&header, g_free);
	header = axis_test_plex_last_header(fx->plex, "/identity", "Accept");
	g_assert_cmpstr(header, ==, "application/json");
}

/* Spec: Token stays secret (only GET, ever) */
static void
test_only_get_is_sent(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *headers[] = { "X-Plex-Container-Start", "0", NULL };

	axis_test_plex_standard(fx->plex);
	g_assert_true(axis_http_get(&config, "/devices", NULL, &body, &error));
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_true(axis_http_get(&config, "/status/sessions/history/all?sort=viewedAt:desc", headers, &body,
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 2);
	g_assert_false(axis_test_plex_saw_non_get(fx->plex));
}

/* Spec: Redirect to another host refused */
static void
test_redirect_to_another_port_is_refused_and_nothing_is_sent_there(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(other);
	g_autofree gchar *location = g_strconcat(base, "/identity", NULL);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;

	axis_test_plex_standard(other);
	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_REDIRECT, 302, location, 0);
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_null(body);
	/* The other listener saw nothing at all: not the request, not the token. */
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	transcript = axis_test_plex_transcript(other);
	g_assert_null(strstr(transcript, TOKEN));
	axis_test_plex_free(other);
}

/* A refused redirect reports where it pointed, as the server wrote it (for a
 * caller that reads a magnet link and never requests it); nothing is sent
 * there, and no message holds it. */
static void
test_a_refused_redirect_reports_its_location(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	AxisHttpResponse response;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	static const gchar magnet[] = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Hearth&tr=x%3Ay";

	axis_test_plex_route(fx->plex, "/magnet", -1, AXIS_TEST_PLEX_REDIRECT, 302, magnet, 0);
	g_assert_false(axis_http_get_full(&config, "/magnet", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(response.status, ==, 302);
	g_assert_cmpstr(response.location, ==, magnet);
	g_assert_null(strstr(error->message, "magnet"));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 1);
	axis_http_response_clear(&response);
	g_assert_null(response.location);

	/* A location with a space or a control character is not reported, and a
	 * redirect that is followed leaves none. */
	axis_test_plex_route(fx->plex, "/spaced", -1, AXIS_TEST_PLEX_REDIRECT, 302, "magnet:?xt=a b", 0);
	g_clear_error(&error);
	g_assert_false(axis_http_get_full(&config, "/spaced", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_null(response.location);
	axis_http_response_clear(&response);
	axis_test_plex_route(fx->plex, "/start", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/identity", 0);
	axis_test_plex_standard(fx->plex);
	g_assert_true(axis_http_get_full(&config, "/start", NULL, &body, &response, &error));
	g_assert_null(response.location);
	axis_http_response_clear(&response);
}

/* Spec: Redirect to another host refused (a different name for the same machine is another host) */
static void
test_redirect_to_another_host_name_is_refused(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autofree gchar *location = g_strdup_printf("http://localhost:%u/identity", axis_test_plex_port(fx->plex));
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_REDIRECT, 302, location, 0);
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 1);
}

static void
test_redirect_to_another_scheme_is_refused(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autofree gchar *location = g_strdup_printf("https://127.0.0.1:%u/identity", axis_test_plex_port(fx->plex));
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_REDIRECT, 302, location, 0);
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
}

/* A redirect to the same host is followed, a few times at most. */
static void
test_redirect_to_the_same_host_is_followed(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *header = NULL;

	axis_test_plex_standard(fx->plex);
	axis_test_plex_route(fx->plex, "/old", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/identity", 0);
	g_assert_true(axis_http_get(&config, "/old", NULL, &body, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(g_bytes_get_data(body, NULL), "machineIdentifier"));
	/* The token went to the same server on the second hop too. */
	header = axis_test_plex_last_header(fx->plex, "/identity", "X-Plex-Token");
	g_assert_cmpstr(header, ==, TOKEN);
}

static void
test_redirect_loop_ends(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route(fx->plex, "/loop", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/loop", 0);
	g_assert_false(axis_http_get(&config, "/loop", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/loop"), ==, AXIS_HTTP_MAX_REDIRECTS + 1);
}

/* Spec: Bad responses are survivable */
static void
test_oversized_answer_is_refused(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	config.max_bytes = 100 * 1024;
	axis_test_plex_route(fx->plex, "/big", -1, AXIS_TEST_PLEX_OVERSIZE, 200, NULL, 1024 * 1024);
	g_assert_false(axis_http_get(&config, "/big", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_TOO_LARGE);
	g_assert_null(body);
}

static void
test_never_ending_answer_is_cut_off(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();

	config.max_bytes = 200 * 1024;
	axis_test_plex_route(fx->plex, "/forever", -1, AXIS_TEST_PLEX_ENDLESS, 200, NULL, 0);
	g_assert_false(axis_http_get(&config, "/forever", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_TOO_LARGE);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 4 * G_USEC_PER_SEC);
}

static void
test_slow_drip_hits_the_deadline(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	gint64 took;

	config.timeout_ms = 700;
	axis_test_plex_route(fx->plex, "/drip", -1, AXIS_TEST_PLEX_DRIP, 200, NULL, 100);
	g_assert_false(axis_http_get(&config, "/drip", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_TIMEOUT);
	took = g_get_monotonic_time() - start;
	g_assert_cmpint(took, >=, 600 * 1000);
	g_assert_cmpint(took, <, 4 * G_USEC_PER_SEC);
}

static void
test_silent_server_hits_the_deadline(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();

	config.timeout_ms = 600;
	axis_test_plex_route(fx->plex, "/hang", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	g_assert_false(axis_http_get(&config, "/hang", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 4 * G_USEC_PER_SEC);
}

static void
test_a_refused_token_says_so_without_the_token(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	g_assert_nonnull(strstr(error->message, "401"));
	g_assert_nonnull(strstr(error->message, "token was refused"));
	g_assert_nonnull(strstr(error->message, "the media server"));
}

static void
test_a_server_error_is_a_status_error(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_NORMAL, 500, "{\"error\":\"" TOKEN "\"}", 0);
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	/* A response body is never put in a message, even one that echoes the token. */
	g_assert_null(strstr(error->message, "error"));
}

static void
test_unreachable_server_is_a_connect_error(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	AxisTestPlex *gone = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(gone);
	AxisHttpOrigin origin;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	/* A port that was open a moment ago and is closed now. */
	g_assert_true(axis_http_origin_parse(base, &origin, NULL));
	axis_test_plex_free(gone);
	config.origin = &origin;
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONNECT);
	axis_http_origin_clear(&origin);
}

typedef struct
{
	GCancellable *cancellable;
} Canceller;

static gpointer
cancel_later(gpointer data)
{
	Canceller *canceller = data;

	g_usleep(300 * 1000);
	g_cancellable_cancel(canceller->cancellable);
	return NULL;
}

static void
test_cancellation_ends_the_request(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Canceller canceller = { cancellable };
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	GThread *thread;
	gint64 start = g_get_monotonic_time();

	config.cancellable = cancellable;
	config.timeout_ms = 10000;
	axis_test_plex_route(fx->plex, "/hang", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	thread = g_thread_new("canceller", cancel_later, &canceller);
	g_assert_false(axis_http_get(&config, "/hang", NULL, &body, &error));
	g_thread_join(thread);
	assert_error(error, AXIS_HTTP_ERROR_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_USEC_PER_SEC);
}

static void
test_proxy_environment_is_ignored(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	/* A proxy that is not there: if it were used the request would fail. */
	g_setenv("http_proxy", "http://127.0.0.1:1", TRUE);
	g_setenv("HTTP_PROXY", "http://127.0.0.1:1", TRUE);
	g_setenv("all_proxy", "http://127.0.0.1:1", TRUE);
	g_unsetenv("no_proxy");
	g_unsetenv("NO_PROXY");
	axis_test_plex_standard(fx->plex);
	g_assert_true(axis_http_get(&config, "/identity", NULL, &body, &error));
	g_assert_no_error(error);
	g_unsetenv("http_proxy");
	g_unsetenv("HTTP_PROXY");
	g_unsetenv("all_proxy");
}

static void
test_a_path_that_is_not_a_path_is_refused(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_http_get(&config, "http://127.0.0.1:9/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_false(axis_http_get(&config, "/with space", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);
}

/* Added with the move: a caller names the server and its secret, and
 * chooses the user agent and the accept header. */
static void
test_names_user_agent_and_accept_are_the_callers(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *agent = NULL;
	g_autofree gchar *accept = NULL;

	config.peer = "the metadata provider";
	config.secret_name = "key";
	config.user_agent = "Axis/9.9.9";
	config.accept = "image/png";
	axis_test_plex_standard(fx->plex);
	g_assert_true(axis_http_get(&config, "/identity", NULL, &body, &error));
	agent = axis_test_plex_last_header(fx->plex, "/identity", "User-Agent");
	accept = axis_test_plex_last_header(fx->plex, "/identity", "Accept");
	g_assert_cmpstr(agent, ==, "Axis/9.9.9");
	g_assert_cmpstr(accept, ==, "image/png");

	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_STATUS, 403, NULL, 403);
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	g_assert_cmpstr(error->message, ==, "the metadata provider answered 403: the key was refused");
}

/* Added with the move: what the server said beside the body. */
static void
test_the_response_is_reported(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;
	const gchar *const wait[] = { "Retry-After", "7", NULL };

	axis_test_plex_standard(fx->plex);
	g_assert_true(axis_http_get_full(&config, "/identity", NULL, &body, &response, &error));
	g_assert_cmpuint(response.status, ==, 200);
	g_assert_cmpint(response.retry_after_s, ==, -1);
	g_assert_cmpstr(response.content_type, ==, "application/json");
	axis_http_response_clear(&response);

	axis_test_plex_route_full(fx->plex, "/busy", 429, "text/plain", NULL, wait);
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_false(axis_http_get_full(&config, "/busy", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	g_assert_cmpuint(response.status, ==, 429);
	g_assert_cmpint(response.retry_after_s, ==, 7);
	g_assert_null(response.content_type);
	axis_http_response_clear(&response);
}

static void
test_ratelimit_reset_forms(void)
{
	const gint64 now = 1800000000;

	g_assert_cmpint(axis_http_parse_ratelimit_reset("1800000600", now), ==, 600);
	g_assert_cmpint(axis_http_parse_ratelimit_reset(" 1800000600 ", now), ==, 600);
	/* Already past: no wait, not a negative one. */
	g_assert_cmpint(axis_http_parse_ratelimit_reset("1799999000", now), ==, 0);
	g_assert_cmpint(axis_http_parse_ratelimit_reset("0", now), ==, 0);
	/* Far away, or too long to be a time: bounded. */
	g_assert_cmpint(axis_http_parse_ratelimit_reset("1900000000", now), ==, AXIS_HTTP_MAX_RETRY_AFTER_S);
	g_assert_cmpint(axis_http_parse_ratelimit_reset("99999999999999999999", now), ==, AXIS_HTTP_MAX_RETRY_AFTER_S);
	g_assert_cmpint(axis_http_parse_ratelimit_reset("-5", now), ==, -1);
	g_assert_cmpint(axis_http_parse_ratelimit_reset("12s", now), ==, -1);
	g_assert_cmpint(axis_http_parse_ratelimit_reset("", now), ==, -1);
	g_assert_cmpint(axis_http_parse_ratelimit_reset("999999999999999999999", now), ==, -1);
	g_assert_cmpint(axis_http_parse_ratelimit_reset(NULL, now), ==, -1);
}

static void
test_retry_after_forms(void)
{
	/* 2026-11-13 12:00:00 UTC */
	const gint64 now = 1794571200;

	g_assert_cmpint(axis_http_parse_retry_after("120", now), ==, 120);
	g_assert_cmpint(axis_http_parse_retry_after(" 0 ", now), ==, 0);
	g_assert_cmpint(axis_http_parse_retry_after("999999999999999999999", now), ==, AXIS_HTTP_MAX_RETRY_AFTER_S);
	g_assert_cmpint(axis_http_parse_retry_after("Fri, 13 Nov 2026 12:01:30 GMT", now), ==, 90);
	g_assert_cmpint(axis_http_parse_retry_after("Fri, 13 Nov 2026 11:00:00 GMT", now), ==, 0);
	g_assert_cmpint(axis_http_parse_retry_after("-5", now), ==, -1);
	g_assert_cmpint(axis_http_parse_retry_after("12s", now), ==, -1);
	g_assert_cmpint(axis_http_parse_retry_after("soon", now), ==, -1);
	g_assert_cmpint(axis_http_parse_retry_after("", now), ==, -1);
	g_assert_cmpint(axis_http_parse_retry_after(NULL, now), ==, -1);
}

static void
test_scrub_blanks_the_token(void)
{
	g_autofree gchar *text = axis_http_scrub("failed for " TOKEN " twice " TOKEN "\n", TOKEN, 100);

	g_assert_null(strstr(text, TOKEN));
	g_assert_nonnull(strstr(text, "***"));
}

/* TLS: a certificate that does not verify is refused unless allowed. */
static void
test_untrusted_certificate(void)
{
	AxisTestPlex *tls = axis_test_plex_new_tls("tests/fixtures/compat/tls/cert.pem", "tests/fixtures/compat/tls/key.pem");
	g_autofree gchar *base = NULL;
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	if (tls == NULL) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	base = axis_test_plex_base(tls);
	axis_test_plex_standard(tls);
	g_assert_true(axis_http_origin_parse(base, &origin, NULL));
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	config.auth_header = "X-Plex-Token";
	config.auth_value = TOKEN;
	config.token = TOKEN;
	config.timeout_ms = 5000;

	g_assert_false(axis_http_get(&config, "/identity", NULL, &body, &error));
	assert_error(error, AXIS_HTTP_ERROR_TLS);
	/* Nothing was sent: the handshake failed before any request. */
	g_assert_cmpuint(axis_test_plex_n_requests(tls), ==, 0);
	g_clear_error(&error);

	config.accept_untrusted_certificate = TRUE;
	g_assert_true(axis_http_get(&config, "/identity", NULL, &body, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(g_bytes_get_data(body, NULL), "machineIdentifier"));
	axis_http_origin_clear(&origin);
	axis_test_plex_free(tls);
}

/* ── A release's fetch: the one redirect to an indexer's site ────── */

#define INDEXER_KEY "the-indexer-own-key-4c19ab72"

/* Which addresses are public, of every range the followed redirect refuses. */
static void
test_public_addresses(void)
{
	static const struct
	{
		const gchar *address;
		gboolean     public;
	} cases[] = {
		{ "8.8.8.8", TRUE }, { "1.1.1.1", TRUE }, { "93.184.216.34", TRUE }, { "172.15.255.255", TRUE },
		{ "172.32.0.1", TRUE }, { "100.63.255.255", TRUE }, { "100.128.0.1", TRUE }, { "223.255.255.255", TRUE },
		{ "0.0.0.0", FALSE }, { "0.1.2.3", FALSE }, { "10.0.0.1", FALSE }, { "10.255.255.255", FALSE },
		{ "100.64.0.1", FALSE }, { "127.0.0.1", FALSE }, { "127.255.0.9", FALSE }, { "169.254.169.254", FALSE },
		{ "172.16.0.1", FALSE }, { "172.31.255.255", FALSE }, { "192.0.0.8", FALSE }, { "192.0.2.1", FALSE },
		{ "192.88.99.1", FALSE }, { "192.168.1.1", FALSE }, { "198.18.0.1", FALSE }, { "198.19.255.255", FALSE },
		{ "198.51.100.7", FALSE }, { "203.0.113.9", FALSE }, { "224.0.0.1", FALSE }, { "239.255.255.250", FALSE },
		{ "240.0.0.1", FALSE }, { "255.255.255.255", FALSE },
		{ "2606:4700:4700::1111", TRUE }, { "2a00:1450:4001::200e", TRUE }, { "::ffff:8.8.8.8", TRUE },
		{ "64:ff9b::808:808", TRUE }, { "2002:808:808::1", TRUE },
		{ "::", FALSE }, { "::1", FALSE }, { "::ffff:127.0.0.1", FALSE }, { "::ffff:10.0.0.1", FALSE },
		{ "::ffff:192.168.0.1", FALSE }, { "::ffff:169.254.169.254", FALSE }, { "::ffff:0.0.0.0", FALSE },
		{ "::127.0.0.1", FALSE }, { "::8.8.8.8", FALSE }, { "64:ff9b::a00:1", FALSE }, { "64:ff9b::7f00:1", FALSE },
		{ "2002:7f00:1::1", FALSE }, { "2002:a00:1::1", FALSE }, { "fe80::1", FALSE }, { "febf::1", FALSE },
		{ "fec0::1", FALSE }, { "fc00::1", FALSE }, { "fd12:3456:789a::1", FALSE }, { "ff02::1", FALSE },
		{ "ff05::2", FALSE }, { "2001:db8::1", FALSE }, { "2001::1", FALSE }, { "100::1", FALSE },
		{ "4000::1", FALSE }, { "1::1", FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GInetAddress) address = g_inet_address_new_from_string(cases[i].address);

		g_assert_nonnull(address);
		if (axis_http_address_is_public(address) != cases[i].public)
			g_error("%s: expected %s", cases[i].address, cases[i].public ? "public" : "not public");
	}
}

typedef struct
{
	AxisTestPlex   *plex;	/* the indexer source, plain http */
	AxisHttpOrigin  origin;
	AxisTestPlex   *site;	/* the indexer's site, TLS with the test authority */
	GTlsDatabase   *database;
} Offsite;

#define RELEASE_BYTES "<?xml version=\"1.0\"?><nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\"></nzb>"

static gboolean
offsite_setup(Offsite *os)
{
	g_autofree gchar *base = NULL;
	g_autofree gchar *anchors = g_canonicalize_filename("tests/fixtures/compat/tls/cert.pem", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) release = g_bytes_new_static(RELEASE_BYTES, strlen(RELEASE_BYTES));

	memset(os, 0, sizeof(*os));
	os->site = axis_test_plex_new_tls("tests/fixtures/compat/tls/cert.pem", "tests/fixtures/compat/tls/key.pem");
	if (os->site == NULL)
		return FALSE;
	os->database = g_tls_file_database_new(anchors, &error);
	g_assert_no_error(error);
	os->plex = axis_test_plex_new();
	base = axis_test_plex_base(os->plex);
	g_assert_true(axis_http_origin_parse(base, &os->origin, NULL));
	axis_test_plex_route_full(os->site, "/file", 200, "application/x-nzb", release, NULL);
	return TRUE;
}

static void
offsite_teardown(Offsite *os)
{
	axis_http_set_offsite_testing(FALSE, NULL);
	axis_http_origin_clear(&os->origin);
	axis_test_plex_free(os->plex);
	axis_test_plex_free(os->site);
	g_clear_object(&os->database);
}

static AxisHttpConfig
offsite_config(const Offsite *os)
{
	AxisHttpConfig config;

	memset(&config, 0, sizeof(config));
	config.origin = &os->origin;
	config.auth_header = "X-Api-Key";
	config.auth_value = TOKEN;
	config.token = TOKEN;
	config.timeout_ms = 5000;
	config.peer = "the indexer source";
	config.secret_name = "key";
	config.user_agent = "Axis/test";
	config.accept = "application/x-nzb";
	return config;
}

/* The source redirects "/release" to @location. */
static void
redirect_to(
	Offsite     *os,
	const gchar *location
){
	axis_test_plex_clear_routes(os->plex);
	axis_test_plex_route(os->plex, "/release", -1, AXIS_TEST_PLEX_REDIRECT, 301, location, 0);
}

static gchar *
site_address(
	Offsite     *os,
	const gchar *host,
	const gchar *path
){
	return g_strdup_printf("https://%s:%u%s?t=get&apikey=" INDEXER_KEY, host, axis_test_plex_port(os->site), path);
}

/* No message holds the source's key or the indexer's address or key. */
static void
assert_offsite_clean(
	const GError *error,
	const gchar  *location
){
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, TOKEN));
	g_assert_null(strstr(error->message, INDEXER_KEY));
	g_assert_null(strstr(error->message, "apikey"));
	if (location != NULL)
		g_assert_null(strstr(error->message, location));
}

/*
 * Spec (indexers): An indexer's download redirect is followed once, with
 * nothing of Axis's (Followed).  Only axis_http_fetch_release() follows it;
 * the request there carries the method, the path and query, Host and the
 * user agent, and nothing of the source's or the caller's.
 */
static void
test_offsite_followed(void)
{
	Offsite os;
	AxisHttpConfig config;
	AxisHttpResponse response;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *location = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	const gchar *headers[] = { "X-Extra", "from-the-caller", NULL };
	gchar **lines;
	guint i;

	if (!offsite_setup(&os)) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	config = offsite_config(&os);
	config.accept_untrusted_certificate = TRUE;
	location = site_address(&os, "127.0.0.1", "/file");
	redirect_to(&os, location);
	axis_http_set_offsite_testing(TRUE, os.database);

	/* Any other request refuses it, with nothing sent there. */
	g_assert_false(axis_http_get_full(&config, "/release", headers, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_false(response.offsite);
	axis_http_response_clear(&response);
	g_assert_cmpuint(axis_test_plex_n_requests(os.site), ==, 0);
	g_clear_error(&error);

	g_assert_true(axis_http_fetch_release(&config, "/release", headers, &body, &response, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(body, NULL), g_bytes_get_size(body), RELEASE_BYTES, strlen(RELEASE_BYTES));
	g_assert_true(response.offsite);
	g_assert_cmpuint(response.status, ==, 200);
	g_assert_cmpstr(response.content_type, ==, "application/x-nzb");
	/* Where it pointed is not kept anywhere the caller gets. */
	g_assert_null(response.location);
	axis_http_response_clear(&response);

	/* One request at the site: the path and query, and only Host and the
	 * user agent (and what the library adds about the connection itself). */
	requests = axis_test_plex_requests(os.site);
	g_assert_cmpuint(requests->len, ==, 1);
	lines = g_strsplit(g_ptr_array_index(requests, 0), "\n", -1);
	g_assert_cmpstr(lines[0], ==, "GET /file?t=get&apikey=" INDEXER_KEY);
	for (i = 1; lines[i] != NULL && lines[i][0] != '\0'; i++) {
		g_autofree gchar *name = g_strndup(lines[i], strcspn(lines[i], ":"));

		if (g_ascii_strcasecmp(name, "Host") != 0 && g_ascii_strcasecmp(name, "User-Agent") != 0 &&
		    g_ascii_strcasecmp(name, "Connection") != 0)
			g_error("the followed request carried the header %s", lines[i]);
		if (g_ascii_strcasecmp(name, "User-Agent") == 0)
			g_assert_cmpstr(lines[i] + strlen(name) + 2, ==, "Axis/test");
	}
	g_strfreev(lines);
	g_assert_null(strstr(g_ptr_array_index(requests, 0), TOKEN));
	g_assert_null(strstr(g_ptr_array_index(requests, 0), "from-the-caller"));
	/* The source was asked once by each call, with its key, as ever. */
	g_assert_cmpuint(axis_test_plex_n_requests(os.plex), ==, 2);
	offsite_teardown(&os);
}

/*
 * Spec (indexers): Inside the network.  With the tests' allowance off, a
 * redirect to loopback (by address or by a name that resolves to it) is
 * refused, as is every other local range, an address that is not https and
 * one with credentials: nothing reaches the site, and nothing names it.
 */
static void
test_offsite_refused(void)
{
	static const gchar *const local[] = {
		"10.0.0.1", "172.16.0.1", "192.168.1.1", "169.254.169.254", "100.64.0.1", "0.0.0.0", "224.0.0.1",
		"[::ffff:127.0.0.1]", "[::ffff:10.0.0.1]", "[::ffff:192.168.0.1]", "[::]", "[::1]", "[fe80::1]", "[fc00::1]",
		"[fd00::1]", "[ff02::1]", "[64:ff9b::a00:1]", "[2002:a00:1::1]", NULL
	};
	Offsite os;
	AxisHttpConfig config;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	if (!offsite_setup(&os)) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	config = offsite_config(&os);
	/* The authority is the tests', but loopback is not allowed. */
	axis_http_set_offsite_testing(FALSE, os.database);
	{
		const gchar *hosts[] = { "127.0.0.1", "localhost", NULL };

		for (i = 0; hosts[i] != NULL; i++) {
			g_autofree gchar *location = site_address(&os, hosts[i], "/file");

			redirect_to(&os, location);
			g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, NULL, &error));
			assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
			assert_offsite_clean(error, location);
			g_assert_nonnull(strstr(error->message, "not at a public address"));
			g_clear_error(&error);
		}
	}
	for (i = 0; local[i] != NULL; i++) {
		g_autofree gchar *location = g_strdup_printf("https://%s/file?apikey=" INDEXER_KEY, local[i]);

		redirect_to(&os, location);
		g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, NULL, &error));
		/* Refused where it is connected to; an IPv6 address this machine
		 * cannot even open a socket for fails before that, as harmlessly. */
		if (!g_error_matches(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_REDIRECT) &&
		    !(local[i][0] == '[' && g_error_matches(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONNECT)))
			g_error("%s: %s", local[i], error->message);
		assert_offsite_clean(error, location);
		g_clear_error(&error);
	}
	/* Not https, or with a user: refused before anything is looked up. */
	axis_http_set_offsite_testing(TRUE, os.database);
	{
		g_autofree gchar *plain = g_strdup_printf("http://127.0.0.1:%u/file?apikey=" INDEXER_KEY,
			axis_test_plex_port(os.site));
		g_autofree gchar *user = g_strdup_printf("https://someone:pw@127.0.0.1:%u/file?apikey=" INDEXER_KEY,
			axis_test_plex_port(os.site));

		redirect_to(&os, plain);
		g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, NULL, &error));
		assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
		assert_offsite_clean(error, plain);
		g_clear_error(&error);
		redirect_to(&os, user);
		g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, NULL, &error));
		assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
		g_clear_error(&error);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(os.site), ==, 0);
	offsite_teardown(&os);
}

/* Spec (indexers): followed once: a second redirect, an oversized answer and
 * a certificate the system does not trust are refused; what the site
 * answered is reported as the site's. */
static void
test_offsite_limits(void)
{
	Offsite os;
	AxisHttpConfig config;
	AxisHttpResponse response;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *location = NULL;
	static const gchar magnet[] = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=X";

	if (!offsite_setup(&os)) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	config = offsite_config(&os);
	axis_http_set_offsite_testing(TRUE, os.database);

	/* A second redirect, even on the site itself, is not followed. */
	again = site_address(&os, "127.0.0.1", "/file");
	axis_test_plex_route(os.site, "/again", -1, AXIS_TEST_PLEX_REDIRECT, 302, again, 0);
	location = site_address(&os, "127.0.0.1", "/again");
	redirect_to(&os, location);
	g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	assert_offsite_clean(error, location);
	g_assert_true(response.offsite);
	axis_http_response_clear(&response);
	g_assert_cmpuint(axis_test_plex_count(os.site, "/again"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(os.site, "/file"), ==, 0);
	g_clear_error(&error);

	/* A magnet link the site redirects to is reported, never requested. */
	axis_test_plex_route(os.site, "/magnet", -1, AXIS_TEST_PLEX_REDIRECT, 302, magnet, 0);
	g_clear_pointer(&location, g_free);
	location = site_address(&os, "127.0.0.1", "/magnet");
	redirect_to(&os, location);
	g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpstr(response.location, ==, magnet);
	axis_http_response_clear(&response);
	g_clear_error(&error);

	/* Larger than the cap. */
	axis_test_plex_route(os.site, "/huge", -1, AXIS_TEST_PLEX_OVERSIZE, 200, NULL, 64 * 1024);
	g_clear_pointer(&location, g_free);
	location = site_address(&os, "127.0.0.1", "/huge");
	redirect_to(&os, location);
	config.max_bytes = 16 * 1024;
	g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_TOO_LARGE);
	assert_offsite_clean(error, location);
	axis_http_response_clear(&response);
	g_clear_error(&error);
	config.max_bytes = 0;

	/* The site's own refusal is its own, not the source refusing its key. */
	axis_test_plex_route(os.site, "/refused", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	g_clear_pointer(&location, g_free);
	location = site_address(&os, "127.0.0.1", "/refused");
	redirect_to(&os, location);
	g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	g_assert_true(response.offsite);
	g_assert_cmpuint(response.status, ==, 401);
	g_assert_null(strstr(error->message, "was refused"));
	assert_offsite_clean(error, location);
	axis_http_response_clear(&response);
	g_clear_error(&error);

	/* A certificate only the system's authorities would have to vouch for:
	 * refused, whatever the source's own setting says, with nothing sent. */
	axis_http_set_offsite_testing(TRUE, NULL);
	config.accept_untrusted_certificate = TRUE;
	axis_test_plex_reset_requests(os.site);
	g_clear_pointer(&location, g_free);
	location = site_address(&os, "127.0.0.1", "/file");
	redirect_to(&os, location);
	g_assert_false(axis_http_fetch_release(&config, "/release", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_TLS);
	assert_offsite_clean(error, location);
	g_assert_null(strstr(error->message, "allow that in the settings"));
	axis_http_response_clear(&response);
	g_assert_cmpuint(axis_test_plex_n_requests(os.site), ==, 0);
	offsite_teardown(&os);
}

/* A Location of about 1.1 kB (a magnet link with its trackers, as Prowlarr's
 * are) is read; one beyond the cap is not. */
static void
test_long_locations(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	AxisHttpResponse response;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	GString *magnet = g_string_new("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Hearth");
	guint i;

	for (i = 0; magnet->len < 1100; i++)
		g_string_append_printf(magnet, "&tr=udp%%3a%%2f%%2ftracker%u.test%%3a1337%%2fannounce", i);
	axis_test_plex_route(fx->plex, "/magnet", -1, AXIS_TEST_PLEX_REDIRECT, 301, magnet->str, 0);
	g_assert_false(axis_http_get_full(&config, "/magnet", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpstr(response.location, ==, magnet->str);
	axis_http_response_clear(&response);
	g_clear_error(&error);

	while (magnet->len <= AXIS_HTTP_MAX_LOCATION_BYTES)
		g_string_append(magnet, "&tr=udp%3a%2f%2fmore.test%3a1%2fa");
	axis_test_plex_route(fx->plex, "/longer", -1, AXIS_TEST_PLEX_REDIRECT, 301, magnet->str, 0);
	g_assert_false(axis_http_get_full(&config, "/longer", NULL, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_null(response.location);
	axis_http_response_clear(&response);
	g_string_free(magnet, TRUE);
}

/* ── POST, sessions and cookies (download clients) ──────────────── */

#define SESSION_COOKIE "fixture-session-7f3a9c1e5b2d"

static AxisHttpConfig
download_config_for(const Fixture *fx)
{
	AxisHttpConfig config = config_for(fx);

	config.auth_header = "X-Api-Key";
	config.peer = "the download client";
	config.secret_name = "key";
	return config;
}

static GBytes *
text_bytes(const gchar *text)
{
	return g_bytes_new(text, strlen(text));
}

static const gchar *const login_cookie[] = { "Set-Cookie", "sid=" SESSION_COOKIE "; Path=/", NULL };
static const gchar *const login_cookies[] = {
	"Set-Cookie", "sid=" SESSION_COOKIE "; Path=/json; HttpOnly",
	"Set-Cookie", "bad name=x",
	NULL
};
static const gchar *const logout_cookie[] = { "Set-Cookie", "sid=gone; Max-Age=0", NULL };

/* A media server's or metadata provider's connection cannot POST: a
 * session of their purpose refuses before anything is sent. */
static void
test_post_refused_for_a_read_session(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_READ);
	g_autoptr(GBytes) payload = text_bytes("{\"x\":1}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;

	axis_test_plex_standard(fx->plex);
	g_assert_cmpint(axis_http_session_get_purpose(session), ==, AXIS_HTTP_PURPOSE_READ);
	g_assert_false(axis_http_session_post(session, "/identity", "application/json", payload, NULL, &body,
		&response, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_assert_false(response.sent);
	g_assert_null(body);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);
	g_clear_error(&error);

	/* GET works as it does without a session, and keeps no cookie. */
	axis_test_plex_route_full(fx->plex, "/login", 200, "application/json", payload, login_cookie);
	g_assert_true(axis_http_session_get(session, "/login", NULL, &body, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_http_session_n_cookies(session), ==, 0);
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_true(axis_http_session_get(session, "/identity", NULL, &body, NULL, &error));
	g_assert_null(axis_test_plex_last_header(fx->plex, "/identity", "Cookie"));
	g_assert_false(axis_test_plex_saw_non_get(fx->plex));
}

static void
test_post_from_a_download_session(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = download_config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	g_autoptr(GBytes) payload = text_bytes("{\"name\":\"Some.Film.2020\"}");
	g_autoptr(GBytes) answer = text_bytes("{\"id\":\"one\"}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *key = NULL;
	AxisHttpResponse response;

	axis_test_plex_route_full(fx->plex, "/api/add", 200, "application/json", answer, NULL);
	g_assert_true(axis_http_session_post(session, "/api/add", "application/json", payload, NULL, &body,
		&response, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(response.status, ==, 200);
	g_assert_true(response.sent);
	axis_http_response_clear(&response);
	g_assert_cmpmem(g_bytes_get_data(body, NULL), g_bytes_get_size(body), "{\"id\":\"one\"}", 12);
	requests = axis_test_plex_requests(fx->plex);
	g_assert_cmpuint(requests->len, ==, 1);
	g_assert_true(g_str_has_prefix(g_ptr_array_index(requests, 0), "POST /api/add\n"));
	g_assert_nonnull(strstr(g_ptr_array_index(requests, 0), "\n\n{\"name\":\"Some.Film.2020\"}\n"));
	type = axis_test_plex_last_header(fx->plex, "/api/add", "Content-Type");
	g_assert_cmpstr(type, ==, "application/json");
	key = axis_test_plex_last_header(fx->plex, "/api/add", "X-Api-Key");
	g_assert_cmpstr(key, ==, TOKEN);
}

/* Spec (downloaders): Redirect - nothing is sent to the other address, and
 * a POST is not sent again even to its own origin. */
static void
test_post_is_never_followed_on_a_redirect(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = download_config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(other);
	g_autofree gchar *elsewhere = g_strconcat(base, "/api/add", NULL);
	g_autoptr(GBytes) payload = text_bytes("{\"password\":\"" TOKEN "\"}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;

	axis_test_plex_standard(other);
	axis_test_plex_route(fx->plex, "/api/add", -1, AXIS_TEST_PLEX_REDIRECT, 307, elsewhere, 0);
	g_assert_false(axis_http_session_post(session, "/api/add", "application/json", payload, NULL, &body, NULL,
		&error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_null(body);
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	transcript = axis_test_plex_transcript(other);
	g_assert_null(strstr(transcript, TOKEN));
	g_clear_error(&error);

	/* To its own origin: still not followed. */
	axis_test_plex_route(fx->plex, "/api/old", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/api/new", 0);
	axis_test_plex_route(fx->plex, "/api/new", -1, AXIS_TEST_PLEX_NORMAL, 200, "{}", 0);
	g_assert_false(axis_http_session_post(session, "/api/old", "application/json", payload, NULL, &body, NULL,
		&error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/api/old"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/api/new"), ==, 0);
	axis_test_plex_free(other);
}

/* ── The one write to a media server (add-library-notify) ───────── */

#define RESCAN_BODY_ONE \
	"{\"Updates\":[{\"Path\":\"/data/movies/Hearth (1995) {tmdb-6001}\",\"UpdateType\":\"Created\"}]}"

static AxisHttpConfig
jellyfin_config_for(const Fixture *fx)
{
	AxisHttpConfig config = config_for(fx);

	config.auth_header = "Authorization";
	config.auth_value = "MediaBrowser Token=\"" TOKEN "\"";
	return config;
}

/* The request as it arrives: exactly the pinned method and path, the body
 * built from the fixed shape (and nothing more), the key in its header as for
 * a read, the content type, no query, no other header of ours. */
static void
test_rescan_request_is_the_pinned_one(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = jellyfin_config_for(fx);
	AxisHttpRescanUpdate one[] = { { "/data/movies/Hearth (1995) {tmdb-6001}", AXIS_HTTP_RESCAN_CREATED } };
	AxisHttpRescanUpdate two[] = {
		{ "/data/tv/Show \"Q\" \\ (2020)", AXIS_HTTP_RESCAN_MODIFIED },
		{ "/data/tv/Caf\xc3\xa9", AXIS_HTTP_RESCAN_DELETED }
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	g_autofree gchar *value = NULL;
	AxisHttpResponse response;
	const gchar *text;

	axis_test_plex_route_full(fx->plex, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	g_assert_true(axis_http_post_rescan(&config, one, 1, &response, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(response.status, ==, 204);
	g_assert_true(response.sent);
	axis_http_response_clear(&response);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 1);
	requests = axis_test_plex_requests(fx->plex);
	text = g_ptr_array_index(requests, 0);
	g_assert_true(g_str_has_prefix(text, "POST /Library/Media/Updated\n"));
	g_assert_nonnull(strstr(text, "\n\n" RESCAN_BODY_ONE "\n"));
	value = axis_test_plex_last_header(fx->plex, "/Library/Media/Updated", "Content-Type");
	g_assert_cmpstr(value, ==, "application/json");
	g_clear_pointer(&value, g_free);
	value = axis_test_plex_last_header(fx->plex, "/Library/Media/Updated", "Authorization");
	g_assert_cmpstr(value, ==, "MediaBrowser Token=\"" TOKEN "\"");
	g_clear_pointer(&value, g_free);
	value = axis_test_plex_last_header(fx->plex, "/Library/Media/Updated", "User-Agent");
	g_assert_cmpstr(value, ==, "axisd");
	/* The key is in a header, not in the request line and not in the body. */
	g_assert_nonnull(strstr(text, TOKEN));
	g_assert_true(strstr(text, TOKEN) < strstr(text, "\n\n"));
	g_assert_null(strstr(strstr(text, "\n\n"), TOKEN));

	/* Two folders: one request, the body escaped and in the fixed shape. */
	axis_test_plex_reset_requests(fx->plex);
	g_assert_true(axis_http_post_rescan(&config, two, 2, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 1);
	g_clear_pointer(&requests, g_ptr_array_unref);
	requests = axis_test_plex_requests(fx->plex);
	g_assert_nonnull(strstr(g_ptr_array_index(requests, 0), "\n\n{\"Updates\":[{\"Path\":\"/data/tv/Show \\\"Q\\\" \\\\ "
		"(2020)\",\"UpdateType\":\"Modified\"},{\"Path\":\"/data/tv/Caf\xc3\xa9\",\"UpdateType\":\"Deleted\"}]}\n"));
}

/* An update out of shape, or too many, is refused before anything is sent. */
static void
test_rescan_body_shape_is_enforced(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = jellyfin_config_for(fx);
	AxisHttpRescanUpdate many[AXIS_HTTP_RESCAN_MAX_UPDATES + 1];
	g_autofree gchar *fill = g_strnfill(AXIS_HTTP_RESCAN_MAX_PATH_BYTES, 'x');
	g_autofree gchar *long_path = g_strconcat("/", fill, NULL);	/* one byte over the cap */
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;
	guint i;
	static const gchar *const bad[] = {
		"relative/path", "", "/with\nnewline", "/with\ttab", "/bell\a", "/del\x7f", "/invalid\xff" "utf8", NULL
	};

	axis_test_plex_route_full(fx->plex, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	for (i = 0; bad[i] != NULL; i++) {
		AxisHttpRescanUpdate update = { bad[i], AXIS_HTTP_RESCAN_CREATED };

		g_assert_false(axis_http_post_rescan(&config, &update, 1, &response, &error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_assert_false(response.sent);
		g_clear_error(&error);
	}
	{
		AxisHttpRescanUpdate update = { long_path, AXIS_HTTP_RESCAN_CREATED };

		g_assert_false(axis_http_post_rescan(&config, &update, 1, NULL, &error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_clear_error(&error);
	}
	{
		AxisHttpRescanUpdate update = { NULL, AXIS_HTTP_RESCAN_CREATED };

		g_assert_false(axis_http_post_rescan(&config, &update, 1, NULL, &error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_clear_error(&error);
	}
	{
		AxisHttpRescanUpdate update = { "/ok", (AxisHttpRescanType)7 };

		g_assert_false(axis_http_post_rescan(&config, &update, 1, NULL, &error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_clear_error(&error);
	}
	for (i = 0; i < G_N_ELEMENTS(many); i++) {
		many[i].path = "/data/movies/x";
		many[i].type = AXIS_HTTP_RESCAN_MODIFIED;
	}
	g_assert_false(axis_http_post_rescan(&config, many, AXIS_HTTP_RESCAN_MAX_UPDATES + 1, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_false(axis_http_post_rescan(&config, many, 0, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);

	/* The cap itself is allowed, and so is the longest path. */
	g_assert_true(axis_http_post_rescan(&config, many, AXIS_HTTP_RESCAN_MAX_UPDATES, NULL, &error));
	g_assert_no_error(error);
	{
		/* Exactly the cap. */
		g_autofree gchar *path = g_strconcat("/", fill + 1, NULL);
		AxisHttpRescanUpdate update = { path, AXIS_HTTP_RESCAN_CREATED };

		g_assert_cmpuint(strlen(path), ==, AXIS_HTTP_RESCAN_MAX_PATH_BYTES);
		g_assert_true(axis_http_post_rescan(&config, &update, 1, NULL, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 2);
}

/* The body is sent once, to the origin it was meant for: a redirect, to
 * another server or to this one, is not followed. */
static void
test_rescan_is_never_followed_on_a_redirect(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = jellyfin_config_for(fx);
	AxisHttpRescanUpdate one[] = { { "/data/movies/Hearth", AXIS_HTTP_RESCAN_CREATED } };
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(other);
	g_autofree gchar *elsewhere = g_strconcat(base, "/Library/Media/Updated", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;

	axis_test_plex_route_full(other, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	axis_test_plex_route(fx->plex, "/Library/Media/Updated", -1, AXIS_TEST_PLEX_REDIRECT, 307, elsewhere, 0);
	g_assert_false(axis_http_post_rescan(&config, one, 1, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	transcript = axis_test_plex_transcript(other);
	g_assert_null(strstr(transcript, TOKEN));
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/Library/Media/Updated"), ==, 1);
	g_clear_error(&error);

	/* To its own origin: still not sent again. */
	axis_test_plex_clear_routes(fx->plex);
	axis_test_plex_reset_requests(fx->plex);
	axis_test_plex_route(fx->plex, "/Library/Media/Updated", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/elsewhere", 0);
	axis_test_plex_route_full(fx->plex, "/elsewhere", 204, NULL, NULL, NULL);
	g_assert_false(axis_http_post_rescan(&config, one, 1, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/Library/Media/Updated"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/elsewhere"), ==, 0);
	axis_test_plex_free(other);
}

/* Spec: the only write to a media server is the rescan request.  The gate
 * every request passes is data: exactly this method and this path. */
static void
test_no_other_write_is_permitted(void)
{
	static const gchar *const methods[] = {
		"GET", "HEAD", "PUT", "DELETE", "PATCH", "OPTIONS", "post", "Post", "POST ", " POST", "", NULL
	};
	static const gchar *const paths[] = {
		"/Library/Media/Updated/", "/library/media/updated", "/Library/Media/Updated?x=1", "/Library/Media",
		"/Library/Refresh", "/Library/Media/Updated/x", "//Library/Media/Updated", "/Items/1", "/Sessions/Playing",
		"/Library/VirtualFolders", "/identity", "/library/sections/1/refresh", "/Users/1/Delete", "", "Library/Media/Updated",
		"/Library/Media/Updated#", "/Library/Media/Updated ", NULL
	};
	guint m;
	guint p;

	g_assert_true(axis_http_rescan_permitted("POST", "/Library/Media/Updated"));
	g_assert_cmpstr(AXIS_HTTP_RESCAN_METHOD, ==, "POST");
	g_assert_cmpstr(AXIS_HTTP_RESCAN_PATH, ==, "/Library/Media/Updated");
	g_assert_false(axis_http_rescan_permitted(NULL, "/Library/Media/Updated"));
	g_assert_false(axis_http_rescan_permitted("POST", NULL));
	for (m = 0; methods[m] != NULL; m++)
		g_assert_false(axis_http_rescan_permitted(methods[m], "/Library/Media/Updated"));
	for (p = 0; paths[p] != NULL; p++) {
		for (m = 0; methods[m] != NULL; m++)
			g_assert_false(axis_http_rescan_permitted(methods[m], paths[p]));
		g_assert_false(axis_http_rescan_permitted("POST", paths[p]));
	}
}

/* At run time, on a media-server connection (a session of the read purpose,
 * the only kind of session that is not a download client's), a POST of any
 * path, the pinned one included, is refused before anything is sent: the
 * pinned request has its own function and no session. */
static void
test_media_server_post_is_refused_before_it_is_sent(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = jellyfin_config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_READ);
	g_autoptr(GBytes) payload = text_bytes(RESCAN_BODY_ONE);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	static const gchar *const paths[] = {
		"/Library/Media/Updated", "/Library/Refresh", "/Items/1/Refresh", "/Library/VirtualFolders", "/identity", NULL
	};
	AxisHttpResponse response;
	guint i;

	axis_test_plex_route_full(fx->plex, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	for (i = 0; paths[i] != NULL; i++) {
		g_assert_false(axis_http_session_post(session, paths[i], "application/json", payload, NULL, &body, &response,
			&error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_assert_false(response.sent);
		g_assert_null(body);
		g_clear_error(&error);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(fx->plex));
}

/* ── The one write to a metadata provider (add-metadata-tvdb) ───── */

/* Obviously fake: a test key and PIN, never a real one. */
#define LOGIN_KEY "fake-tvdb-key-for-tests-0000"
#define LOGIN_PIN "FAKEPIN0000"

static AxisHttpConfig
login_config_for(const Fixture *fx)
{
	AxisHttpConfig config = config_for(fx);

	config.auth_header = NULL;
	config.auth_value = NULL;
	config.token = NULL;
	config.peer = "TheTVDB";
	config.secret_name = "key";
	return config;
}

/* The request as it arrives: exactly the pinned method and path, the body of
 * the key (and the PIN when there is one) and nothing more, no credential
 * header, no query. */
static void
test_login_request_is_the_pinned_one(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = login_config_for(fx);
	g_autoptr(GBytes) answer = text_bytes("{\"status\":\"success\",\"data\":{\"token\":\"fake.token.value\"}}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	g_autofree gchar *value = NULL;
	AxisHttpResponse response;
	const gchar *text;

	axis_test_plex_route_full(fx->plex, "/v4/login", 200, "application/json", answer, NULL);
	g_assert_true(axis_http_post_login(&config, LOGIN_KEY, NULL, &body, &response, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(response.status, ==, 200);
	axis_http_response_clear(&response);
	g_assert_cmpmem(g_bytes_get_data(body, NULL), g_bytes_get_size(body), g_bytes_get_data(answer, NULL),
		g_bytes_get_size(answer));
	requests = axis_test_plex_requests(fx->plex);
	g_assert_cmpuint(requests->len, ==, 1);
	text = g_ptr_array_index(requests, 0);
	g_assert_true(g_str_has_prefix(text, "POST /v4/login\n"));
	g_assert_nonnull(strstr(text, "\n\n{\"apikey\":\"" LOGIN_KEY "\"}\n"));
	/* The key is in the body only: not in the request line or a header. */
	g_assert_true(strstr(text, LOGIN_KEY) > strstr(text, "\n\n"));
	g_assert_null(strstr(text, "Authorization"));
	g_assert_null(strstr(text, "pin"));
	value = axis_test_plex_last_header(fx->plex, "/v4/login", "Content-Type");
	g_assert_cmpstr(value, ==, "application/json");

	/* With a PIN, the PIN too. */
	axis_test_plex_reset_requests(fx->plex);
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_true(axis_http_post_login(&config, LOGIN_KEY, LOGIN_PIN, &body, NULL, &error));
	g_clear_pointer(&requests, g_ptr_array_unref);
	requests = axis_test_plex_requests(fx->plex);
	g_assert_nonnull(strstr(g_ptr_array_index(requests, 0),
		"\n\n{\"apikey\":\"" LOGIN_KEY "\",\"pin\":\"" LOGIN_PIN "\"}\n"));
	/* An empty PIN is none. */
	axis_test_plex_reset_requests(fx->plex);
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_true(axis_http_post_login(&config, LOGIN_KEY, "", &body, NULL, &error));
	g_clear_pointer(&requests, g_ptr_array_unref);
	requests = axis_test_plex_requests(fx->plex);
	g_assert_null(strstr(g_ptr_array_index(requests, 0), "pin"));
}

/* A key or PIN out of shape, or a credential header, is refused before
 * anything is sent, and no message holds what was refused. */
static void
test_login_body_shape_is_enforced(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad[] = {
		"", "two words", "quote\"d", "back\\slash", "tab\there", "new\nline", "caf\xc3\xa9", "del\x7f", NULL
	};
	AxisHttpConfig config = login_config_for(fx);
	AxisHttpConfig with_header = config_for(fx);
	g_autofree gchar *fill = g_strnfill(AXIS_HTTP_LOGIN_MAX_SECRET_BYTES + 1, 'k');
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;
	guint i;

	axis_test_plex_route(fx->plex, "/v4/login", -1, AXIS_TEST_PLEX_NORMAL, 200, "{}", 0);
	for (i = 0; bad[i] != NULL; i++) {
		g_assert_false(axis_http_post_login(&config, bad[i], NULL, &body, &response, &error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_assert_false(response.sent);
		if (bad[i][0] != '\0')
			g_assert_null(strstr(error->message, bad[i]));
		g_clear_error(&error);
		if (bad[i][0] == '\0')
			continue;
		g_assert_false(axis_http_post_login(&config, LOGIN_KEY, bad[i], &body, NULL, &error));
		assert_error(error, AXIS_HTTP_ERROR_CONFIG);
		g_assert_null(strstr(error->message, LOGIN_KEY));
		g_clear_error(&error);
	}
	g_assert_false(axis_http_post_login(&config, NULL, NULL, &body, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_false(axis_http_post_login(&config, fill, NULL, &body, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_clear_error(&error);
	/* The login carries no credential header of any kind. */
	g_assert_false(axis_http_post_login(&with_header, LOGIN_KEY, NULL, &body, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);
	g_assert_null(body);
}

/* The body is sent once, to the origin it was meant for; a refusal and an
 * error never hold the key or the PIN, whatever the answer echoes. */
static void
test_login_redirect_and_failures(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = login_config_for(fx);
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(other);
	g_autofree gchar *elsewhere = g_strconcat(base, "/v4/login", NULL);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;

	axis_test_plex_route(fx->plex, "/v4/login", -1, AXIS_TEST_PLEX_REDIRECT, 307, elsewhere, 0);
	g_assert_false(axis_http_post_login(&config, LOGIN_KEY, LOGIN_PIN, &body, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_REDIRECT);
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->plex, "/v4/login"), ==, 1);
	g_clear_error(&error);

	axis_test_plex_clear_routes(fx->plex);
	axis_test_plex_route(fx->plex, "/v4/login", -1, AXIS_TEST_PLEX_NORMAL, 401,
		"{\"status\":\"failure\",\"message\":\"refused " LOGIN_KEY " " LOGIN_PIN "\"}", 0);
	g_assert_false(axis_http_post_login(&config, LOGIN_KEY, LOGIN_PIN, &body, &response, &error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	g_assert_cmpuint(response.status, ==, 401);
	g_assert_null(strstr(error->message, LOGIN_KEY));
	g_assert_null(strstr(error->message, LOGIN_PIN));
	axis_http_response_clear(&response);
	axis_test_plex_free(other);
}

/* The gate of the login is data: exactly this method and this path. */
static void
test_login_is_the_only_provider_write(void)
{
	static const gchar *const methods[] = { "GET", "PUT", "DELETE", "PATCH", "post", "POST ", "", NULL };
	static const gchar *const paths[] = {
		"/v4/login/", "/V4/login", "/v4/login?x=1", "/v4", "/login", "/v4/user", "//v4/login", "/v4/login#",
		"v4/login", "/Library/Media/Updated", "", NULL
	};
	guint m;
	guint p;

	g_assert_true(axis_http_login_permitted("POST", "/v4/login"));
	g_assert_cmpstr(AXIS_HTTP_LOGIN_METHOD, ==, "POST");
	g_assert_cmpstr(AXIS_HTTP_LOGIN_PATH, ==, "/v4/login");
	g_assert_false(axis_http_login_permitted(NULL, "/v4/login"));
	g_assert_false(axis_http_login_permitted("POST", NULL));
	/* Each gate opens for its own request only. */
	g_assert_false(axis_http_rescan_permitted("POST", "/v4/login"));
	g_assert_false(axis_http_login_permitted("POST", "/Library/Media/Updated"));
	for (m = 0; methods[m] != NULL; m++)
		g_assert_false(axis_http_login_permitted(methods[m], "/v4/login"));
	for (p = 0; paths[p] != NULL; p++)
		g_assert_false(axis_http_login_permitted("POST", paths[p]));
}

/* A read session cannot send the login's path as a POST either. */
static void
test_login_path_refused_for_a_read_session(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = login_config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_READ);
	g_autoptr(GBytes) payload = text_bytes("{\"apikey\":\"" LOGIN_KEY "\"}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route(fx->plex, "/v4/login", -1, AXIS_TEST_PLEX_NORMAL, 200, "{}", 0);
	g_assert_false(axis_http_session_post(session, "/v4/login", "application/json", payload, NULL, &body, NULL,
		&error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);
}

/* Session cookies: kept from the origin's answers, sent back to it, never
 * to another server, never in a message; a Max-Age of 0 removes one. */
static void
test_session_cookies_stay_with_their_origin(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = download_config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *other_base = axis_test_plex_base(other);
	AxisHttpOrigin other_origin;
	AxisHttpConfig other_config;
	g_autoptr(AxisHttpSession) other_session = NULL;
	g_autoptr(GBytes) payload = text_bytes("{}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *transcript = NULL;

	axis_test_plex_route_full(fx->plex, "/api/login", 200, "application/json", payload, login_cookies);
	axis_test_plex_route(fx->plex, "/api/list", -1, AXIS_TEST_PLEX_NORMAL, 200, "{}", 0);
	axis_test_plex_route(fx->plex, "/api/fail", -1, AXIS_TEST_PLEX_NORMAL, 500, SESSION_COOKIE, 0);
	g_assert_true(axis_http_session_post(session, "/api/login", "application/json", payload, NULL, &body, NULL,
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_http_session_n_cookies(session), ==, 1);
	g_clear_pointer(&body, g_bytes_unref);
	g_assert_true(axis_http_session_post(session, "/api/list", "application/json", payload, NULL, &body, NULL,
		&error));
	cookie = axis_test_plex_last_header(fx->plex, "/api/list", "Cookie");
	g_assert_cmpstr(cookie, ==, "sid=" SESSION_COOKIE);
	g_clear_pointer(&body, g_bytes_unref);

	/* An error message never holds it. */
	g_assert_false(axis_http_session_post(session, "/api/fail", "application/json", payload, NULL, &body, NULL,
		&error));
	assert_error(error, AXIS_HTTP_ERROR_STATUS);
	g_assert_null(strstr(error->message, SESSION_COOKIE));
	g_clear_error(&error);

	/* Another origin is another session: it carries nothing of this one. */
	g_assert_true(axis_http_origin_parse(other_base, &other_origin, NULL));
	other_config = download_config_for(fx);
	other_config.origin = &other_origin;
	other_config.auth_value = "another-key";
	other_config.token = "another-key";
	other_session = axis_http_session_new(&other_config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	axis_test_plex_route(other, "/api/list", -1, AXIS_TEST_PLEX_NORMAL, 200, "{}", 0);
	g_assert_true(axis_http_session_post(other_session, "/api/list", "application/json", payload, NULL, &body,
		NULL, &error));
	transcript = axis_test_plex_transcript(other);
	g_assert_null(strstr(transcript, SESSION_COOKIE));
	g_assert_null(strstr(transcript, TOKEN));
	g_clear_pointer(&body, g_bytes_unref);

	/* The origin ends the session. */
	axis_test_plex_route_full(fx->plex, "/api/logout", 200, "application/json", payload, logout_cookie);
	g_assert_true(axis_http_session_post(session, "/api/logout", "application/json", payload, NULL, &body, NULL,
		&error));
	g_assert_cmpuint(axis_http_session_n_cookies(session), ==, 0);
	axis_http_origin_clear(&other_origin);
	axis_test_plex_free(other);
}

/* Whether a request that failed may have arrived. */
static void
test_sent_says_whether_a_request_went_out(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = download_config_for(fx);
	AxisTestPlex *gone = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(gone);
	AxisHttpOrigin origin;
	g_autoptr(AxisHttpSession) closed = NULL;
	g_autoptr(AxisHttpSession) hanging = NULL;
	g_autoptr(GBytes) payload = text_bytes("{}");
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;

	g_assert_true(axis_http_origin_parse(base, &origin, NULL));
	axis_test_plex_free(gone);
	config.origin = &origin;
	closed = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	g_assert_false(axis_http_session_post(closed, "/api/add", "application/json", payload, NULL, &body, &response,
		&error));
	assert_error(error, AXIS_HTTP_ERROR_CONNECT);
	g_assert_false(response.sent);
	axis_http_response_clear(&response);
	g_clear_error(&error);

	config = download_config_for(fx);
	config.timeout_ms = 500;
	hanging = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	axis_test_plex_route(fx->plex, "/api/add", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	g_assert_false(axis_http_session_post(hanging, "/api/add", "application/json", payload, NULL, &body, &response,
		&error));
	assert_error(error, AXIS_HTTP_ERROR_TIMEOUT);
	g_assert_true(response.sent);
	axis_http_response_clear(&response);
	axis_http_origin_clear(&origin);
}

static void
test_post_body_limit(Fixture *fx, gconstpointer data)
{
	AxisHttpConfig config = download_config_for(fx);
	g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);
	gsize n = AXIS_HTTP_MAX_REQUEST_BYTES + 1;
	g_autoptr(GBytes) payload = g_bytes_new_take(g_malloc0(n), n);
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_http_session_post(session, "/api/add", "application/octet-stream", payload, NULL, &body,
		NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_TOO_LARGE);
	g_clear_error(&error);
	g_assert_false(axis_http_session_post(session, "/api/add", "not a type", payload, NULL, &body, NULL, &error));
	assert_error(error, AXIS_HTTP_ERROR_CONFIG);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->plex), ==, 0);
}

static void
test_cookie_jar(void)
{
	g_autoptr(AxisHttpCookies) jar = axis_http_cookies_new();
	g_autofree gchar *header = NULL;
	g_autofree gchar *scrubbed = NULL;
	guint i;

	g_assert_true(axis_http_cookies_take(jar, "_session_id=abcdef0123456789; Expires=Fri, 13 Nov 2026 12:00:00 GMT"));
	g_assert_true(axis_http_cookies_take(jar, " other = \"quoted-value\" ; Secure"));
	g_assert_false(axis_http_cookies_take(jar, "no-equals"));
	g_assert_false(axis_http_cookies_take(jar, "bad name=x"));
	g_assert_false(axis_http_cookies_take(jar, "name=has space"));
	g_assert_false(axis_http_cookies_take(jar, "name=has\"quote"));
	g_assert_false(axis_http_cookies_take(jar, "=nameless"));
	g_assert_false(axis_http_cookies_take(jar, NULL));
	g_assert_cmpuint(axis_http_cookies_count(jar), ==, 2);
	header = axis_http_cookies_header(jar);
	g_assert_cmpstr(header, ==, "_session_id=abcdef0123456789; other=quoted-value");
	scrubbed = axis_http_cookies_scrub(jar, "failed with abcdef0123456789 and quoted-value");
	g_assert_cmpstr(scrubbed, ==, "failed with *** and ***");

	/* Replaced, then removed. */
	g_assert_true(axis_http_cookies_take(jar, "other=second"));
	g_assert_cmpuint(axis_http_cookies_count(jar), ==, 2);
	g_assert_true(axis_http_cookies_take(jar, "other=x; Max-Age=0"));
	g_assert_true(axis_http_cookies_take(jar, "_session_id=; Path=/"));
	g_assert_cmpuint(axis_http_cookies_count(jar), ==, 0);
	g_assert_false(axis_http_cookies_take(jar, "never=set; max-age=-1"));

	/* Bounded. */
	for (i = 0; i < AXIS_HTTP_MAX_COOKIES + 4; i++) {
		g_autofree gchar *one = g_strdup_printf("c%u=v%u", i, i);

		axis_http_cookies_take(jar, one);
	}
	g_assert_cmpuint(axis_http_cookies_count(jar), ==, AXIS_HTTP_MAX_COOKIES);
	axis_http_cookies_clear(jar);
	g_assert_null(axis_http_cookies_header(jar));
}

static void
test_scrub_queries(void)
{
	g_autofree gchar *text = axis_http_scrub_queries("GET http://h:8080/api?apikey=s3cret&mode=queue failed (x)");
	g_autofree gchar *plain = axis_http_scrub_queries("no address here");

	g_assert_cmpstr(text, ==, "GET http://h:8080/api?*** failed (x)");
	g_assert_cmpstr(plain, ==, "no address here");
}

static void
test_tls_error_classification(void)
{
	GError *bad = g_error_new_literal(G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE, "bad");
	GError *need = g_error_new_literal(G_TLS_ERROR, G_TLS_ERROR_CERTIFICATE_REQUIRED, "need");
	GError *hand = g_error_new_literal(G_TLS_ERROR, G_TLS_ERROR_HANDSHAKE, "Peer failed to perform TLS handshake");
	GError *nottls = g_error_new_literal(G_TLS_ERROR, G_TLS_ERROR_NOT_TLS, "not tls");
	GError *eof = g_error_new_literal(G_TLS_ERROR, G_TLS_ERROR_EOF, "eof");
	GError *io = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "failed");

	g_assert_true(axis_http_tls_error_is_certificate(bad));
	g_assert_true(axis_http_tls_error_is_certificate(need));
	g_assert_false(axis_http_tls_error_is_certificate(hand));
	g_assert_false(axis_http_tls_error_is_certificate(nottls));
	g_assert_false(axis_http_tls_error_is_certificate(eof));
	g_assert_false(axis_http_tls_error_is_certificate(io));
	g_assert_false(axis_http_tls_error_is_certificate(NULL));
	g_error_free(bad);
	g_error_free(need);
	g_error_free(hand);
	g_error_free(nottls);
	g_error_free(eof);
	g_error_free(io);
}

/* Reads @path (relative to the project root, where tests run) whole. */
static gchar *
read_source(const gchar *path)
{
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static void
scan_sources(
	const gchar *dir,
	GPtrArray   *found
){
	g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
	const gchar *name;

	g_assert_nonnull(handle);
	while ((name = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			scan_sources(path, found);
		else if (g_str_has_suffix(name, ".c") || g_str_has_suffix(name, ".h"))
			g_ptr_array_add(found, g_steal_pointer(&path));
	}
}

/* No media-server or metadata code can POST: only download/ (and the
 * client itself) names the download purpose or the POST function, and
 * compat/ and meta/ use no session at all. */
static void
test_only_download_code_may_post(void)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	guint named = 0;
	guint i;

	scan_sources("src", files);
	g_assert_cmpuint(files->len, >, 100);
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = read_source(path);
		gboolean own = g_str_has_prefix(path, "src/download/") || strcmp(path, "src/core/axis-http.c") == 0 ||
			strcmp(path, "src/core/axis-http.h") == 0;

		if (strstr(text, "AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT") != NULL ||
		    strstr(text, "axis_http_session_post") != NULL) {
			if (!own)
				g_error("%s names the download clients' POST; only src/download/ may", path);
			named++;
		}
		if ((g_str_has_prefix(path, "src/compat/") || g_str_has_prefix(path, "src/meta/")) &&
		    strstr(text, "axis_http_session_") != NULL)
			g_error("%s uses an HTTP session; media servers and metadata providers use axis_http_get()", path);
	}
	g_assert_cmpuint(named, >=, 3);
}

static guint
count_of(
	const gchar *text,
	const gchar *needle
);

/*
 * add-library-notify: the media-server connection's one write.  Outside the
 * client itself, the pinned request is made by exactly one call in the whole
 * of src/, in the Jellyfin notification file of src/compat/; its data (the
 * method, the path, the gate) is named nowhere else; and the client has
 * exactly three requests that are not a GET: the download clients' POST in
 * a session, and the pinned one.
 */
static void
test_one_rescan_call_site(void)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *client = read_source("src/core/axis-http.c");
	guint calls = 0;
	guint i;

	scan_sources("src", files);
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = read_source(path);
		gboolean own = strcmp(path, "src/core/axis-http.c") == 0 || strcmp(path, "src/core/axis-http.h") == 0;

		if (own)
			continue;
		if (strstr(text, "AXIS_HTTP_RESCAN_METHOD") != NULL || strstr(text, "AXIS_HTTP_RESCAN_PATH") != NULL ||
		    strstr(text, "axis_http_rescan_permitted") != NULL)
			g_error("%s names the pinned request's data; only the client may", path);
		if (strstr(text, "axis_http_post_rescan") != NULL) {
			if (strcmp(path, "src/compat/axis-compat-notify-jellyfin.c") != 0)
				g_error("%s asks a media server to rescan; only the Jellyfin notification may", path);
			g_assert_cmpuint(count_of(text, "axis_http_post_rescan"), ==, 1);
			calls += count_of(text, "axis_http_post_rescan(");
		}
		/* Nothing in src/compat/ or src/meta/ builds a request of its own. */
		if ((g_str_has_prefix(path, "src/compat/") || g_str_has_prefix(path, "src/meta/")) &&
		    (strstr(text, "SOUP_METHOD_") != NULL || strstr(text, "soup_message_new") != NULL))
			g_error("%s makes a request without the client", path);
	}
	g_assert_cmpuint(calls, ==, 1);
	/* In the client: one request with the pinned method, one with a
	 * download client's POST, and no other method named in a call. */
	g_assert_cmpuint(count_of(client, "rescan_request.method, rescan_request.path"), ==, 1);
	g_assert_cmpuint(count_of(client, "SOUP_METHOD_POST, path, content_type"), ==, 1);
	g_assert_cmpuint(count_of(client, "SOUP_METHOD_GET, path, NULL, NULL"), ==, 3);
	g_assert_cmpuint(count_of(client, "login_request.method, login_request.path"), ==, 1);
	g_assert_null(strstr(client, "SOUP_METHOD_PUT"));
	g_assert_null(strstr(client, "SOUP_METHOD_DELETE"));
	g_assert_null(strstr(client, "SOUP_METHOD_PATCH"));
}

/*
 * add-metadata-tvdb: the metadata providers' one write.  Outside the client,
 * the pinned login is made by exactly one call in the whole of src/, in the
 * metadata layer's requests (src/meta/axis-meta-fetch.c, which reads the key
 * from the settings itself: no provider sees it), and its data is named
 * nowhere else.
 */
static void
test_one_login_call_site(void)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	guint calls = 0;
	guint i;

	scan_sources("src", files);
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = read_source(path);

		if (strcmp(path, "src/core/axis-http.c") == 0 || strcmp(path, "src/core/axis-http.h") == 0)
			continue;
		if (strstr(text, "AXIS_HTTP_LOGIN_METHOD") != NULL || strstr(text, "AXIS_HTTP_LOGIN_PATH") != NULL ||
		    strstr(text, "axis_http_login_permitted") != NULL)
			g_error("%s names the pinned login's data; only the client may", path);
		if (strstr(text, "axis_http_post_login") != NULL) {
			if (strcmp(path, "src/meta/axis-meta-fetch.c") != 0)
				g_error("%s logs in to a provider; only the metadata layer's requests may", path);
			g_assert_cmpuint(count_of(text, "axis_http_post_login"), ==, 1);
			calls += count_of(text, "axis_http_post_login(");
		}
	}
	g_assert_cmpuint(calls, ==, 1);
}

/* The text of the function of @text that holds @at: from the line that names
 * the function (a name at the start of a line followed by "(") before @at,
 * to the next line that is "}". */
static gchar *
enclosing_function(
	const gchar *text,
	const gchar *at
){
	const gchar *start = at;
	const gchar *end;

	while (start > text && !(start[-1] == '\n' && g_ascii_isalpha(start[0]) && strchr(start, '(') != NULL &&
	       strchr(start, '(') < strchr(start, '\n')))
		start--;
	end = strstr(at, "\n}\n");
	g_assert_nonnull(end);
	return g_strndup(start, (gsize)(end - start));
}

/* How often @needle is in @text. */
static guint
count_of(
	const gchar *text,
	const gchar *needle
){
	guint n = 0;
	const gchar *p = text;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/*
 * The one redirect followed off an origin is reachable from a release's fetch
 * and nothing else: only the indexer layer's connection calls
 * axis_http_fetch_release(), from the one function that makes its requests,
 * for a release only; only the indexer kinds' fetch calls
 * axis_indexer_conn_fetch_release(); and nothing in the daemon names the
 * tests' allowance for loopback.
 */
static void
test_offsite_only_from_the_release_fetch(void)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	guint fetchers = 0;
	guint kinds = 0;
	guint i;

	scan_sources("src", files);
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = read_source(path);
		gboolean client = strcmp(path, "src/core/axis-http.c") == 0 || strcmp(path, "src/core/axis-http.h") == 0;

		if (strstr(text, "axis_http_set_offsite_testing") != NULL && !client)
			g_error("%s names the tests' allowance for the followed redirect; nothing in the daemon may", path);
		if (strstr(text, "axis_http_fetch_release") != NULL && !client) {
			const gchar *at = strstr(text, "axis_http_fetch_release(");
			g_autofree gchar *function = NULL;

			if (strcmp(path, "src/indexer/axis-indexer-conn.c") != 0)
				g_error("%s calls axis_http_fetch_release(); only the indexer layer's release fetch may", path);
			g_assert_cmpuint(count_of(text, "axis_http_fetch_release("), ==, 1);
			function = enclosing_function(text, at);
			/* In do_get(), for a release with following allowed. */
			g_assert_true(g_str_has_prefix(function, "do_get("));
			g_assert_nonnull(strstr(function, "if (release && conn->follow_redirects)"));
			fetchers++;
		}
		if (strstr(text, "axis_indexer_conn_fetch_release") != NULL && strcmp(path, "src/indexer/axis-indexer-conn.c") != 0 &&
		    strcmp(path, "src/indexer/axis-indexer-kind.h") != 0) {
			const gchar *at = strstr(text, "axis_indexer_conn_fetch_release(");
			g_autofree gchar *function = NULL;

			if (!g_str_has_prefix(path, "src/indexer/"))
				g_error("%s fetches a release outside the indexer layer", path);
			g_assert_nonnull(at);
			g_assert_cmpuint(count_of(text, "axis_indexer_conn_fetch_release("), ==, 1);
			function = enclosing_function(text, at);
			/* Only a kind's fetch of a release. */
			if (strstr(function, "_fetch(") == NULL || strstr(function, "AxisIndexerFetchRequest") == NULL)
				g_error("%s calls axis_indexer_conn_fetch_release() outside a kind's fetch", path);
			kinds++;
		}
	}
	g_assert_cmpuint(fetchers, ==, 1);
	g_assert_cmpuint(kinds, >=, 1);
	{
		/* And within the connection, the release flag is set by
		 * axis_indexer_conn_fetch_release() alone. */
		g_autofree gchar *conn = read_source("src/indexer/axis-indexer-conn.c");
		g_autofree gchar *setter = NULL;

		g_assert_cmpuint(count_of(conn, ", TRUE, body, reply, error)"), ==, 1);
		setter = enclosing_function(conn, strstr(conn, ", TRUE, body, reply, error)"));
		g_assert_true(g_str_has_prefix(setter, "axis_indexer_conn_fetch_release("));
	}
}

/* The TLS backend (glib-networking's gnutls module) leaks an error object
 * in its handshake thread when a handshake is refused, which is exactly
 * what the test above provokes; the leak is the library's, not ours. */
const char *
__lsan_default_suppressions(void);

const char *
__lsan_default_suppressions(void)
{
	return "leak:libgiognutls.so\n";
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(name, func) g_test_add("/http/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)
	ADD("get/token-header-only", test_get_sends_the_token_as_a_header_only);
	ADD("get/only-get", test_only_get_is_sent);
	ADD("redirect/another-port", test_redirect_to_another_port_is_refused_and_nothing_is_sent_there);
	ADD("redirect/reports-location", test_a_refused_redirect_reports_its_location);
	ADD("redirect/another-host-name", test_redirect_to_another_host_name_is_refused);
	ADD("redirect/another-scheme", test_redirect_to_another_scheme_is_refused);
	ADD("redirect/same-host", test_redirect_to_the_same_host_is_followed);
	ADD("redirect/loop", test_redirect_loop_ends);
	ADD("bad/oversized", test_oversized_answer_is_refused);
	ADD("bad/never-ending", test_never_ending_answer_is_cut_off);
	ADD("bad/slow-drip", test_slow_drip_hits_the_deadline);
	ADD("bad/silent", test_silent_server_hits_the_deadline);
	ADD("bad/token-refused", test_a_refused_token_says_so_without_the_token);
	ADD("bad/server-error", test_a_server_error_is_a_status_error);
	ADD("bad/unreachable", test_unreachable_server_is_a_connect_error);
	ADD("cancel", test_cancellation_ends_the_request);
	ADD("proxy-ignored", test_proxy_environment_is_ignored);
	ADD("path-refused", test_a_path_that_is_not_a_path_is_refused);
	ADD("names", test_names_user_agent_and_accept_are_the_callers);
	ADD("response", test_the_response_is_reported);
	ADD("post/refused-for-read-session", test_post_refused_for_a_read_session);
	ADD("post/download-session", test_post_from_a_download_session);
	ADD("post/redirect-not-followed", test_post_is_never_followed_on_a_redirect);
	ADD("rescan/pinned-request", test_rescan_request_is_the_pinned_one);
	ADD("rescan/body-shape", test_rescan_body_shape_is_enforced);
	ADD("rescan/redirect-not-followed", test_rescan_is_never_followed_on_a_redirect);
	ADD("rescan/media-server-post-refused", test_media_server_post_is_refused_before_it_is_sent);
	ADD("login/pinned-request", test_login_request_is_the_pinned_one);
	ADD("login/body-shape", test_login_body_shape_is_enforced);
	ADD("login/redirect-and-failures", test_login_redirect_and_failures);
	ADD("login/read-session-refused", test_login_path_refused_for_a_read_session);
	ADD("post/cookies-stay-with-origin", test_session_cookies_stay_with_their_origin);
	ADD("post/sent", test_sent_says_whether_a_request_went_out);
	ADD("post/body-limit", test_post_body_limit);
	ADD("redirect/long-location", test_long_locations);
#undef ADD
	g_test_add_func("/http/retry-after", test_retry_after_forms);
	g_test_add_func("/http/ratelimit-reset", test_ratelimit_reset_forms);
	g_test_add_func("/http/scrub", test_scrub_blanks_the_token);
	g_test_add_func("/http/scrub-queries", test_scrub_queries);
	g_test_add_func("/http/tls-error-classification", test_tls_error_classification);
	g_test_add_func("/http/cookie-jar", test_cookie_jar);
	g_test_add_func("/http/only-download-code-may-post", test_only_download_code_may_post);
	g_test_add_func("/http/rescan/no-other-write-permitted", test_no_other_write_is_permitted);
	g_test_add_func("/http/rescan/one-call-site", test_one_rescan_call_site);
	g_test_add_func("/http/login/no-other-write-permitted", test_login_is_the_only_provider_write);
	g_test_add_func("/http/login/one-call-site", test_one_login_call_site);
	g_test_add_func("/http/offsite/only-from-the-release-fetch", test_offsite_only_from_the_release_fetch);
	g_test_add_func("/http/offsite/public-addresses", test_public_addresses);
	g_test_add_func("/http/offsite/followed", test_offsite_followed);
	g_test_add_func("/http/offsite/refused", test_offsite_refused);
	g_test_add_func("/http/offsite/limits", test_offsite_limits);
	g_test_add_func("/http/tls/untrusted", test_untrusted_certificate);
	return g_test_run();
}
