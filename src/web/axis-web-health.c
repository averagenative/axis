/*
 * axis-web-health.c - the probes: /livez, /readyz and /healthz
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-health.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#include "axis-version.h"
#include "core/axis-proxy.h"

typedef struct _Question Question;

/* A probe waiting for its question's answer. */
typedef struct
{
	HtmxResponse *deferred;
	HtmxRequest  *request;	/* a reference, for axis_web_health_mark() */
	gboolean      full;	/* /healthz with the version */
} Waiter;

struct _Question
{
	AxisWebHealth    *health;
	AxisWebHealthAsk  ask;
	gboolean          answered;	/* there is an answer */
	gboolean          ok;
	gint64            answered_us;	/* monotonic */
	gboolean          asking;	/* a thread is asking (main loop) */
	GPtrArray        *waiters;	/* of Waiter* */
	guint             asked;
};

struct _AxisWebHealth
{
	AxisWebHealthHooks hooks;
	gint64             reuse_us;
	Question           ping;
	Question           current;
	GMainContext      *context;
	GMutex             lock;
	GCond              idle;
	guint              in_flight;	/* (lock) questions whose thread runs */
};

/* What the thread found, for the main loop. */
typedef struct
{
	Question *question;
	gboolean  ok;
	gchar    *error;
} Answer;

/* ── Answers ────────────────────────────────────────────────────── */

HtmxResponse *
axis_web_health_probe_response(
	guint        status,
	const gchar *word
){
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *body = g_strdup_printf("{\"status\":\"%s\"}", word);

	htmx_response_set_status(response, status);
	htmx_response_set_content(response, body);
	htmx_response_set_content_type(response, "application/json");
	htmx_response_add_header(response, "Cache-Control", "no-store");

	return response;
}

/* /healthz: the status word, and to someone who may know more the version
 * and the database's word. */
static HtmxResponse *
health_response(
	gboolean ok,
	gboolean full
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *body = NULL;
	HtmxResponse *response;

	if (!full)
		return axis_web_health_probe_response(ok ? 200 : 503, ok ? "ok" : "degraded");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, ok ? "ok" : "degraded");
	json_builder_set_member_name(builder, "version");
	json_builder_add_string_value(builder, AXIS_VERSION);
	json_builder_set_member_name(builder, "database");
	json_builder_add_string_value(builder, ok ? "ok" : "unreachable");
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	generator = json_generator_new();
	json_generator_set_root(generator, root);
	body = json_generator_to_data(generator, NULL);

	response = htmx_response_new();
	htmx_response_set_status(response, ok ? 200 : 503);
	htmx_response_set_content(response, body);
	htmx_response_set_content_type(response, "application/json");
	htmx_response_add_header(response, "Cache-Control", "no-store");
	return response;
}

static HtmxResponse *
answer_for(
	Question *question,
	gboolean  full
){
	if (question == &question->health->ping)
		return health_response(question->ok, full);

	return question->ok ? axis_web_health_probe_response(200, "ready") :
		axis_web_health_probe_response(503, "not ready");
}

/* ── Questions ──────────────────────────────────────────────────── */

static void
waiter_free(Waiter *waiter)
{
	g_object_unref(waiter->deferred);
	g_object_unref(waiter->request);
	g_free(waiter);
}

/* On the main loop: the answer, to everyone who waited for it. */
static void
on_answered(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Answer *answer = g_task_propagate_pointer(G_TASK(result), NULL);
	Question *question = answer->question;
	g_autoptr(GPtrArray) waiters = question->waiters;
	gboolean was = question->ok;
	gboolean had = question->answered;
	guint i;

	question->asking = FALSE;
	question->answered = TRUE;
	question->ok = answer->ok;
	question->answered_us = g_get_monotonic_time();
	question->waiters = g_ptr_array_new_with_free_func((GDestroyNotify)waiter_free);

	/* Changes are logged, not every probe. */
	if (question == &question->health->ping && (!had || was != answer->ok)) {
		if (answer->ok && had)
			g_message("health: the database is reachable again");
		else if (!answer->ok)
			g_warning("health: the database is unreachable: %s", answer->error);
	} else if (question == &question->health->current && !answer->ok) {
		g_debug("readiness: not ready: %s", answer->error);
	}

	for (i = 0; i < waiters->len; i++) {
		Waiter *waiter = g_ptr_array_index(waiters, i);
		g_autoptr(HtmxResponse) response = answer_for(question, waiter->full);

		axis_web_health_mark(waiter->request, response);
		htmx_response_complete(waiter->deferred, response);
	}

	g_free(answer->error);
	g_free(answer);
}

/* On a thread of its own. */
static void
ask_run(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Question *question = task_data;
	AxisWebHealth *health = question->health;
	g_autoptr(GError) error = NULL;
	Answer *answer = g_new0(Answer, 1);

	answer->question = question;
	answer->ok = question->ask(health->hooks.data, &error);
	if (!answer->ok)
		answer->error = g_strdup(error != NULL ? error->message : "no reason given");

	/* The last use of what the question asks: after this, shutdown may
	 * free it. */
	g_mutex_lock(&health->lock);
	if (--health->in_flight == 0)
		g_cond_broadcast(&health->idle);
	g_mutex_unlock(&health->lock);

	g_task_return_pointer(task, answer, NULL);
}

/*
 * The answer to @question for one more request: at once when a recent one
 * is there, else a deferred response that the next answer completes (and
 * a thread to ask, unless one is asking already).
 */
static HtmxResponse *
ask(
	Question    *question,
	HtmxRequest *request,
	gboolean     full
){
	AxisWebHealth *health = question->health;
	Waiter *waiter;
	GTask *task;

	if (question->answered && g_get_monotonic_time() - question->answered_us < health->reuse_us) {
		HtmxResponse *response = answer_for(question, full);

		axis_web_health_mark(request, response);
		return response;
	}

	waiter = g_new0(Waiter, 1);
	waiter->deferred = htmx_response_new_deferred();
	waiter->request = g_object_ref(request);
	waiter->full = full;
	g_ptr_array_add(question->waiters, waiter);
	if (question->asking)
		return g_object_ref(waiter->deferred);

	question->asking = TRUE;
	question->asked++;
	g_mutex_lock(&health->lock);
	health->in_flight++;
	g_mutex_unlock(&health->lock);
	task = g_task_new(NULL, NULL, on_answered, NULL);
	g_task_set_task_data(task, question, NULL);
	g_task_run_in_thread(task, ask_run);
	g_object_unref(task);

	return g_object_ref(waiter->deferred);
}

void
axis_web_health_mark(
	HtmxRequest  *request,
	HtmxResponse *response
){
	SoupServerMessage *message = htmx_request_get_message(request);
	const gchar *peer = message != NULL ? soup_server_message_get_remote_host(message) : NULL;
	AxisIpAddress address;
	g_autofree gchar *pid = NULL;

	/* The connection's own peer, not what a proxy says: only a process on
	 * this machine (which can list processes anyway) is told. */
	if (peer == NULL || !axis_ip_address_parse(peer, &address) || !axis_ip_address_is_loopback(&address))
		return;
	pid = g_strdup_printf("%d", (gint)getpid());
	htmx_response_add_header(response, AXIS_WEB_HEALTH_PROCESS_HEADER, pid);
}

/* ── Routes ─────────────────────────────────────────────────────── */

/* Whether @request may see the version: a session, or the metrics token. */
static gboolean
may_know_more(
	AxisWebHealth *self,
	HtmxRequest   *request
){
	if (axis_web_request_get_username(request) != NULL)
		return TRUE;

	return self->hooks.metrics != NULL &&
	       axis_web_metrics_check(self->hooks.metrics, request, NULL) == AXIS_WEB_METRICS_TOKEN_GOOD;
}

static HtmxResponse *
handle_healthz(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	AxisWebHealth *self = user_data;

	return ask(&self->ping, request, may_know_more(self, request));
}

static HtmxResponse *
handle_livez(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	HtmxResponse *response = axis_web_health_probe_response(200, "alive");

	axis_web_health_mark(request, response);
	return response;
}

static HtmxResponse *
handle_readyz(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	AxisWebHealth *self = user_data;

	/* Known without asking the database. */
	if (self->hooks.held != NULL && !self->hooks.held(self->hooks.data)) {
		HtmxResponse *response = axis_web_health_probe_response(503, "database lock lost");

		axis_web_health_mark(request, response);
		return response;
	}

	return ask(&self->current, request, FALSE);
}

void
axis_web_health_register(
	AxisWebHealth *self,
	AxisWeb       *web
){
	g_return_if_fail(self != NULL);

	axis_web_route_full(web, HTMX_METHOD_GET, "/healthz", AXIS_WEB_ROUTE_PUBLIC, handle_healthz, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/livez", AXIS_WEB_ROUTE_PUBLIC, handle_livez, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/readyz", AXIS_WEB_ROUTE_PUBLIC, handle_readyz, self);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static void
question_init(
	Question         *question,
	AxisWebHealth    *health,
	AxisWebHealthAsk  ask_func
){
	question->health = health;
	question->ask = ask_func;
	question->waiters = g_ptr_array_new_with_free_func((GDestroyNotify)waiter_free);
}

AxisWebHealth *
axis_web_health_new(const AxisWebHealthHooks *hooks)
{
	AxisWebHealth *self;

	g_return_val_if_fail(hooks != NULL && hooks->ping != NULL && hooks->current != NULL, NULL);

	self = g_new0(AxisWebHealth, 1);
	self->hooks = *hooks;
	self->reuse_us = (gint64)(hooks->reuse_ms > 0 ? hooks->reuse_ms : AXIS_WEB_HEALTH_REUSE_MS) * 1000;
	self->context = g_main_context_ref_thread_default();
	g_mutex_init(&self->lock);
	g_cond_init(&self->idle);
	question_init(&self->ping, self, hooks->ping);
	question_init(&self->current, self, hooks->current);

	return self;
}

void
axis_web_health_drain(AxisWebHealth *self)
{
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	while (self->in_flight > 0)
		g_cond_wait(&self->idle, &self->lock);
	g_mutex_unlock(&self->lock);
}

void
axis_web_health_free(AxisWebHealth *self)
{
	if (self == NULL)
		return;

	axis_web_health_drain(self);
	/* The answers are on their way to the main loop: deliver them. */
	while (self->ping.asking || self->current.asking)
		g_main_context_iteration(self->context, TRUE);

	g_ptr_array_unref(self->ping.waiters);
	g_ptr_array_unref(self->current.waiters);
	g_main_context_unref(self->context);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->idle);
	g_free(self);
}

guint
axis_web_health_count_pings(AxisWebHealth *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->ping.asked;
}

guint
axis_web_health_count_checks(AxisWebHealth *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->current.asked;
}
