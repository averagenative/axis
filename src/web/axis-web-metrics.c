/*
 * axis-web-metrics.c - the optional Prometheus endpoint, /metrics
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-metrics.h"

#include <string.h>

#include "axis-version.h"
#include "core/axis-proxy.h"
#include "web/axis-login-throttle.h"
#include "web/axis-password.h"

/* Wrong tokens: an address gets this many before it waits, as at the worker
 * API.  The token is at least 24 characters, so the throttle is not what
 * protects it; it keeps a guesser from trying without end. */
#define TOKEN_FREE_FAILURES  10
#define TOKEN_BASE_US        (G_GINT64_CONSTANT(2) * G_USEC_PER_SEC)
#define TOKEN_MAX_US         (G_GINT64_CONSTANT(15) * 60 * G_USEC_PER_SEC)
#define TOKEN_FORGET_US      (G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC)
#define TOKEN_MAX_KEYS       10000

/* Seconds; the last bucket, +Inf, is the count. */
static const gdouble bucket_bounds[AXIS_WEB_METRICS_N_BUCKETS] = {
	0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
};

/* What the main loop counts; copied for a scrape. */
typedef struct
{
	guint64 by_class[6];				/* [1] 1xx .. [5] 5xx; [0] other */
	guint64 buckets[AXIS_WEB_METRICS_N_BUCKETS];	/* not cumulative */
	guint64 timed;
	gint64  timed_us;
	guint64 streams;
} Counters;

struct _AxisWebMetrics
{
	gchar      *token;	/* NULL: off; wiped when freed */
	Counters    counters;	/* main loop only */
	AxisLoginThrottle *wrong;	/* wrong tokens, per client address; main loop only */
	guint       wrong_since;	/* wrong tokens not logged yet */
	gint64      wrong_logged_us;	/* when they last were */
	AxisDbPool *pool;	/* borrowed, set by axis_web_metrics_register() */
	GMutex      lock;
	GCond       idle;
	guint       in_flight;	/* (lock) scrapes reading the database */
};

AxisWebMetrics *
axis_web_metrics_new(const gchar *token)
{
	AxisWebMetrics *self = g_new0(AxisWebMetrics, 1);

	self->token = token != NULL && *token != '\0' ? g_strdup(token) : NULL;
	self->wrong = axis_login_throttle_new(TOKEN_FREE_FAILURES, TOKEN_BASE_US, TOKEN_MAX_US, TOKEN_FORGET_US,
		TOKEN_MAX_KEYS);
	g_mutex_init(&self->lock);
	g_cond_init(&self->idle);

	return self;
}

void
axis_web_metrics_drain(AxisWebMetrics *self)
{
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	while (self->in_flight > 0)
		g_cond_wait(&self->idle, &self->lock);
	g_mutex_unlock(&self->lock);
}

void
axis_web_metrics_free(AxisWebMetrics *self)
{
	if (self == NULL)
		return;

	axis_web_metrics_drain(self);
	if (self->token != NULL) {
		memset(self->token, 0, strlen(self->token));
		g_free(self->token);
	}
	axis_login_throttle_free(self->wrong);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->idle);
	g_free(self);
}

gboolean
axis_web_metrics_is_enabled(AxisWebMetrics *self)
{
	return self != NULL && self->token != NULL;
}

/* ── Counting ───────────────────────────────────────────────────── */

void
axis_web_metrics_record(
	AxisWebMetrics *self,
	guint           status,
	gint64          duration_us
){
	guint klass = status / 100;
	gdouble seconds;
	guint i;

	g_return_if_fail(self != NULL);

	self->counters.by_class[klass >= 1 && klass <= 5 ? klass : 0]++;
	if (duration_us < 0) {
		self->counters.streams++;
		return;
	}

	seconds = (gdouble)duration_us / G_USEC_PER_SEC;
	for (i = 0; i < AXIS_WEB_METRICS_N_BUCKETS; i++) {
		if (seconds <= bucket_bounds[i]) {
			self->counters.buckets[i]++;
			break;
		}
	}
	self->counters.timed++;
	self->counters.timed_us += duration_us;
}

typedef struct
{
	AxisWebMetrics *self;
	gint64          start;
} Timing;

static void
on_answered(
	HtmxResponse *deferred,
	HtmxResponse *response,
	gpointer      user_data
){
	Timing *timing = user_data;

	axis_web_metrics_record(timing->self, htmx_response_get_status(response),
		g_get_monotonic_time() - timing->start);
}

static void
timing_middleware(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	AxisWebMetrics *self = user_data;
	gint64 start = g_get_monotonic_time();
	HtmxResponse *response;

	next(context, next_data);

	response = htmx_context_get_response(context);
	if (response == NULL)
		return;
	if (htmx_response_is_streaming(response)) {
		axis_web_metrics_record(self, htmx_response_get_status(response), -1);
	} else if (htmx_response_is_deferred(response)) {
		Timing *timing = g_new0(Timing, 1);

		timing->self = self;
		timing->start = start;
		/* Runs at once if the answer is already there. */
		htmx_response_on_complete(response, on_answered, timing, g_free);
	} else {
		axis_web_metrics_record(self, htmx_response_get_status(response), g_get_monotonic_time() - start);
	}
}

void
axis_web_metrics_install_timing(
	AxisWebMetrics *self,
	HtmxServer     *server
){
	g_return_if_fail(self != NULL);

	if (axis_web_metrics_is_enabled(self))
		htmx_server_use(server, timing_middleware, self, NULL);
}

/* ── The text ───────────────────────────────────────────────────── */

/* A label value as the text format wants it. */
static void
append_label_value(
	GString     *out,
	const gchar *value
){
	const gchar *p;

	for (p = value != NULL ? value : ""; *p != '\0'; p++) {
		if (*p == '\\')
			g_string_append(out, "\\\\");
		else if (*p == '"')
			g_string_append(out, "\\\"");
		else if (*p == '\n')
			g_string_append(out, "\\n");
		else if ((guchar)*p < 0x20 || (guchar)*p == 0x7f)
			g_string_append_c(out, '_');
		else
			g_string_append_c(out, *p);
	}
}

static void
append_help(
	GString     *out,
	const gchar *name,
	const gchar *type,
	const gchar *help
){
	g_string_append_printf(out, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

/* The milliseconds left until @deadline, at least 1. */
static guint
left_ms(gint64 deadline)
{
	gint64 left = (deadline - g_get_monotonic_time()) / 1000;

	return left > 0 ? (guint)left : 1;
}

/*
 * One gauge per row of @sql, whose last column is the count and whose
 * other columns are the values of @labels in order.  Returns FALSE (and
 * writes nothing) when the database does not answer in time.
 */
static gboolean
append_rows(
	GString            *out,
	AxisDbPool         *pool,
	gint64              deadline,
	const gchar        *name,
	const gchar        *help,
	const gchar        *sql,
	const gchar *const *labels
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	guint n_labels = g_strv_length((gchar **)labels);
	guint row;

	if (!axis_db_pool_query_bounded(pool, sql, left_ms(deadline), &result, &error)) {
		g_debug("metrics: %s: %s", name, error->message);
		return FALSE;
	}

	append_help(out, name, "gauge", help);
	for (row = 0; row < axis_db_result_get_n_rows(result); row++) {
		gint64 count = 0;
		guint i;

		if (!axis_db_result_get_int64(result, row, n_labels, &count))
			continue;
		g_string_append(out, name);
		g_string_append_c(out, '{');
		for (i = 0; i < n_labels; i++) {
			g_string_append_printf(out, "%s%s=\"", i > 0 ? "," : "", labels[i]);
			append_label_value(out, axis_db_result_get_text(result, row, i));
			g_string_append_c(out, '"');
		}
		g_string_append_printf(out, "} %" G_GINT64_FORMAT "\n", count);
	}

	return TRUE;
}

static gchar *
render(
	const Counters *counters,
	AxisDbPool     *pool
){
	static const gchar *const class_names[6] = { "other", "1xx", "2xx", "3xx", "4xx", "5xx" };
	static const gchar *const job_labels[] = { "queue", "state", NULL };
	static const gchar *const state_labels[] = { "state", NULL };
	GString *out = g_string_new(NULL);
	gint64 deadline = g_get_monotonic_time() + (gint64)AXIS_WEB_METRICS_QUERY_TIMEOUT_MS * 1000;
	gboolean database_up = pool != NULL;
	guint64 cumulative = 0;
	gchar number[G_ASCII_DTOSTR_BUF_SIZE];
	guint i;

	append_help(out, "axis_build_info", "gauge", "The version of this Axis; always 1.");
	g_string_append(out, "axis_build_info{version=\"");
	append_label_value(out, AXIS_VERSION);
	g_string_append(out, "\",revision=\"");
	append_label_value(out, AXIS_GIT_SHA);
	g_string_append(out, "\"} 1\n");

	append_help(out, "axis_http_requests_total", "counter",
		"HTTP requests answered since the daemon started, by status class.");
	for (i = 0; i < G_N_ELEMENTS(class_names); i++)
		g_string_append_printf(out, "axis_http_requests_total{code=\"%s\"} %" G_GUINT64_FORMAT "\n",
			class_names[i], counters->by_class[i]);

	append_help(out, "axis_http_request_duration_seconds", "histogram",
		"Time from a request reaching the web shell to its answer (event streams are not timed).");
	for (i = 0; i < AXIS_WEB_METRICS_N_BUCKETS; i++) {
		cumulative += counters->buckets[i];
		g_ascii_formatd(number, sizeof(number), "%g", bucket_bounds[i]);
		g_string_append_printf(out, "axis_http_request_duration_seconds_bucket{le=\"%s\"} %" G_GUINT64_FORMAT "\n",
			number, cumulative);
	}
	g_string_append_printf(out, "axis_http_request_duration_seconds_bucket{le=\"+Inf\"} %" G_GUINT64_FORMAT "\n",
		counters->timed);
	g_ascii_formatd(number, sizeof(number), "%.6f", (gdouble)counters->timed_us / G_USEC_PER_SEC);
	g_string_append_printf(out, "axis_http_request_duration_seconds_sum %s\n", number);
	g_string_append_printf(out, "axis_http_request_duration_seconds_count %" G_GUINT64_FORMAT "\n", counters->timed);

	append_help(out, "axis_http_streams_total", "counter",
		"Event streams opened since the daemon started (live pages).");
	g_string_append_printf(out, "axis_http_streams_total %" G_GUINT64_FORMAT "\n", counters->streams);

	if (database_up) {
		database_up = append_rows(out, pool, deadline, "axis_jobs",
				"Jobs waiting or running now, by queue and state.",
				"SELECT queue, state::text, count(*) FROM job WHERE state IN ('queued', 'running') "
				"GROUP BY 1, 2 ORDER BY 1, 2", job_labels) &&
			append_rows(out, pool, deadline, "axis_downloads",
				"Downloads Axis has sent to a download client, by state.",
				"SELECT state, count(*) FROM download GROUP BY 1 ORDER BY 1", state_labels) &&
			append_rows(out, pool, deadline, "axis_imports",
				"Completed downloads taken (or to be taken) into the library, by state.",
				"SELECT state, count(*) FROM import GROUP BY 1 ORDER BY 1", state_labels);
	}
	append_help(out, "axis_database_up", "gauge",
		"Whether the database answered this scrape's questions in time (1) or not (0).");
	g_string_append_printf(out, "axis_database_up %d\n", database_up ? 1 : 0);

	return g_string_free(out, FALSE);
}

gchar *
axis_web_metrics_render(
	AxisWebMetrics *self,
	AxisDbPool     *pool
){
	g_return_val_if_fail(self != NULL, NULL);

	return render(&self->counters, pool);
}

/* ── The route ──────────────────────────────────────────────────── */

gboolean
axis_web_metrics_authorized(
	AxisWebMetrics *self,
	const gchar    *authorization
){
	if (!axis_web_metrics_is_enabled(self) || authorization == NULL)
		return FALSE;
	if (g_ascii_strncasecmp(authorization, "Bearer ", 7) != 0)
		return FALSE;

	return axis_secret_equal(authorization + 7, self->token);
}

/* The Authorization header of @request, as a list (two join with a comma,
 * which makes no token). */
static const gchar *
authorization_of(HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);
	SoupMessageHeaders *headers = message != NULL ? soup_server_message_get_request_headers(message) : NULL;

	return headers != NULL ? soup_message_headers_get_list(headers, "Authorization") : NULL;
}

/* Whether @authorization is "Bearer " and something of the metrics token's
 * form (printable ASCII without a space, as long as a token may be): a
 * guess at the token.  Anything else (Basic, a forward-auth proxy's own
 * long bearer token) is not about the metrics at all. */
static gboolean
token_shaped(const gchar *authorization)
{
	const gchar *p;
	gsize len;

	if (authorization == NULL || g_ascii_strncasecmp(authorization, "Bearer ", 7) != 0)
		return FALSE;
	len = strlen(authorization + 7);
	if (len < AXIS_WEB_METRICS_TOKEN_MIN || len > AXIS_WEB_METRICS_TOKEN_MAX)
		return FALSE;
	for (p = authorization + 7; *p != '\0'; p++) {
		if (*p <= ' ' || *p > '~')
			return FALSE;
	}
	return TRUE;
}

/* Says, at most once a minute, how many wrong tokens came since it last
 * said so, and from where the last one did. */
static void
note_wrong(
	AxisWebMetrics *self,
	const gchar    *client
){
	gint64 now = g_get_monotonic_time();

	self->wrong_since++;
	if (self->wrong_logged_us != 0 && now - self->wrong_logged_us < 60 * G_USEC_PER_SEC)
		return;
	g_message("metrics: %u request(s) carried a token that is not the metrics token, the last from %s (more are "
		"counted, not logged, for a minute)", self->wrong_since, client);
	self->wrong_logged_us = now;
	self->wrong_since = 0;
}

AxisWebMetricsToken
axis_web_metrics_check(
	AxisWebMetrics *self,
	HtmxRequest    *request,
	gint64         *wait_us
){
	const gchar *authorization = authorization_of(request);
	g_autofree gchar *client = axis_proxy_address_key(axis_web_request_get_client_address(request));
	gint64 now = g_get_monotonic_time();
	gint64 wait;

	if (wait_us != NULL)
		*wait_us = 0;
	if (!axis_web_metrics_is_enabled(self) || !token_shaped(authorization))
		return AXIS_WEB_METRICS_TOKEN_NONE;

	/* The right token always works, however many wrong ones came from the
	 * same address (a scanner beside Prometheus must not lock it out).  The
	 * token is long and random: the throttle keeps a guesser from trying
	 * without end and from filling the log, it is not what protects it. */
	if (axis_web_metrics_authorized(self, authorization))
		return AXIS_WEB_METRICS_TOKEN_GOOD;

	wait = axis_login_throttle_check(self->wrong, client, now);
	if (wait > 0) {
		if (wait_us != NULL)
			*wait_us = wait;
		return AXIS_WEB_METRICS_TOKEN_THROTTLED;
	}
	axis_login_throttle_fail(self->wrong, client, now);
	note_wrong(self, client);
	return AXIS_WEB_METRICS_TOKEN_WRONG;
}

typedef struct
{
	AxisWebMetrics *self;
	Counters        counters;
} Scrape;

/* Runs on a worker thread: reads the database and the copied counters. */
static HtmxResponse *
scrape_run(
	GCancellable *cancellable,
	gpointer      user_data
){
	Scrape *scrape = user_data;
	AxisWebMetrics *self = scrape->self;
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *text = render(&scrape->counters, self->pool);

	htmx_response_set_content(response, text);
	htmx_response_set_content_type(response, "text/plain; version=0.0.4; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "no-store");

	/* The last use of @self: after this, shutdown may free it. */
	g_mutex_lock(&self->lock);
	if (--self->in_flight == 0)
		g_cond_broadcast(&self->idle);
	g_mutex_unlock(&self->lock);

	return response;
}

static HtmxResponse *
handle_metrics(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMetrics *self = user_data;
	gint64 wait = 0;
	AxisWebMetricsToken token = axis_web_metrics_check(self, request, &wait);
	Scrape *scrape;

	if (token != AXIS_WEB_METRICS_TOKEN_GOOD) {
		HtmxResponse *refused = htmx_response_new();

		htmx_response_set_content(refused, "");
		htmx_response_set_content_type(refused, "text/plain; charset=utf-8");
		htmx_response_add_header(refused, "Cache-Control", "no-store");
		if (token == AXIS_WEB_METRICS_TOKEN_THROTTLED) {
			g_autofree gchar *seconds = g_strdup_printf("%" G_GINT64_FORMAT,
				MAX((wait + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1));

			htmx_response_set_status(refused, 429);
			htmx_response_add_header(refused, "Retry-After", seconds);
		} else {
			htmx_response_set_status(refused, 401);
			htmx_response_add_header(refused, "WWW-Authenticate", "Bearer");
		}
		return refused;
	}

	scrape = g_new0(Scrape, 1);
	scrape->self = self;
	scrape->counters = self->counters;
	g_mutex_lock(&self->lock);
	self->in_flight++;
	g_mutex_unlock(&self->lock);

	return htmx_response_new_threaded(scrape_run, scrape, g_free);
}

void
axis_web_metrics_register(
	AxisWebMetrics *self,
	AxisWeb        *web,
	AxisDbPool     *pool
){
	g_return_if_fail(self != NULL);

	if (!axis_web_metrics_is_enabled(self))
		return;

	self->pool = pool;
	axis_web_route_full(web, HTMX_METHOD_GET, "/metrics", AXIS_WEB_ROUTE_PUBLIC, handle_metrics, self);
	g_info("metrics: /metrics is on, for requests that carry the metrics token");
}
