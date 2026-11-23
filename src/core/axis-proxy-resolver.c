/*
 * axis-proxy-resolver.c - keeps the addresses of trusted proxy names current
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-proxy-resolver.h"

#define MAX_LOOKED_UP 64

struct _AxisProxyResolver
{
	gint            refs;		/* atomic: the owner's, and one for each lookup under way */
	AxisProxyList  *list;
	GMainContext   *context;
	GCancellable   *cancellable;
	GSource        *timer;
	GMutex          lock;		/* guards the next two */
	gint64          last_round;	/* when the last round was begun or asked for; 0 for never */
	gboolean        stopped;
	guint           pending;		/* lookups under way; main thread only */
};

typedef struct
{
	AxisProxyResolver *resolver;	/* a reference */
	gchar             *name;
} Lookup;

static void
resolver_unref(AxisProxyResolver *self)
{
	if (!g_atomic_int_dec_and_test(&self->refs))
		return;

	g_clear_object(&self->cancellable);
	g_main_context_unref(self->context);
	g_mutex_clear(&self->lock);
	g_free(self);
}

static gboolean
is_stopped(AxisProxyResolver *self)
{
	gboolean stopped;

	g_mutex_lock(&self->lock);
	stopped = self->stopped;
	g_mutex_unlock(&self->lock);
	return stopped;
}

/* The addresses worth trusting out of a lookup's answer: not the
 * unspecified address (0.0.0.0, ::), which names no peer, nor a multicast
 * one, which no peer connects from. */
static guint
collect_addresses(
	GList         *found,
	AxisIpAddress *out,
	guint          max
){
	guint n = 0;
	GList *l;

	for (l = found; l != NULL && n < max; l = l->next) {
		GInetAddress *inet = l->data;
		g_autofree gchar *text = NULL;

		if (g_inet_address_get_is_any(inet) || g_inet_address_get_is_multicast(inet))
			continue;
		text = g_inet_address_to_string(inet);
		if (axis_ip_address_parse(text, &out[n]))
			n++;
	}

	return n;
}

static void
lookup_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Lookup *lookup = user_data;
	AxisProxyResolver *self = lookup->resolver;
	g_autoptr(GError) error = NULL;
	GList *found = g_resolver_lookup_by_name_finish(G_RESOLVER(source), result, &error);

	if (self->pending > 0)
		self->pending--;

	if (!is_stopped(self)) {
		AxisIpAddress addresses[MAX_LOOKED_UP];
		guint n = found != NULL ? collect_addresses(found, addresses, MAX_LOOKED_UP) : 0;
		g_autofree gchar *change = NULL;
		gboolean changed;

		changed = axis_proxy_list_name_result(self->list, lookup->name, n > 0 ? addresses : NULL, n,
			error != NULL ? error->message : NULL, &change);
		if (changed && change != NULL) {
			if (n > 0)
				g_info("%s", change);
			else
				g_warning("%s", change);
		}
	}

	g_list_free_full(found, g_object_unref);
	g_free(lookup->name);
	g_free(lookup);
	resolver_unref(self);
}

/* Asks for every name; main thread. */
static void
start_round(AxisProxyResolver *self)
{
	g_auto(GStrv) names = NULL;
	g_autoptr(GResolver) dns = NULL;
	guint i;

	if (is_stopped(self) || self->pending > 0)
		return;
	dns = g_resolver_get_default();

	g_mutex_lock(&self->lock);
	self->last_round = axis_proxy_list_now(self->list);
	g_mutex_unlock(&self->lock);

	names = axis_proxy_list_get_names(self->list);
	g_main_context_push_thread_default(self->context);
	for (i = 0; names[i] != NULL; i++) {
		Lookup *lookup = g_new0(Lookup, 1);

		g_atomic_int_inc(&self->refs);
		lookup->resolver = self;
		lookup->name = g_strdup(names[i]);
		self->pending++;
		g_resolver_lookup_by_name_async(dns, names[i], self->cancellable, lookup_done, lookup);
	}
	g_main_context_pop_thread_default(self->context);
}

static gboolean
on_timer(gpointer data)
{
	start_round(data);
	return G_SOURCE_CONTINUE;
}

static gboolean
poke_idle(gpointer data)
{
	start_round(data);
	return G_SOURCE_REMOVE;
}

AxisProxyResolver *
axis_proxy_resolver_new(AxisProxyList *list)
{
	AxisProxyResolver *self;

	g_return_val_if_fail(list != NULL, NULL);

	self = g_new0(AxisProxyResolver, 1);
	self->refs = 1;
	self->list = list;
	self->context = g_main_context_ref_thread_default();
	self->cancellable = g_cancellable_new();
	g_mutex_init(&self->lock);

	return self;
}

void
axis_proxy_resolver_start(AxisProxyResolver *self)
{
	g_return_if_fail(self != NULL);

	if (self->timer != NULL || is_stopped(self))
		return;

	self->timer = g_timeout_source_new_seconds(AXIS_PROXY_RESOLVE_PERIOD_S);
	g_source_set_callback(self->timer, on_timer, self, NULL);
	g_source_attach(self->timer, self->context);
	start_round(self);
}

void
axis_proxy_resolver_poke(AxisProxyResolver *self)
{
	gint64 now;
	GSource *idle;

	if (self == NULL)
		return;

	now = axis_proxy_list_now(self->list);
	g_mutex_lock(&self->lock);
	if (self->stopped || (self->last_round != 0 && now - self->last_round < AXIS_PROXY_RESOLVE_MIN_INTERVAL_US)) {
		g_mutex_unlock(&self->lock);
		return;
	}
	/* Reserved here, so that a burst of requests asks once. */
	self->last_round = now;
	g_mutex_unlock(&self->lock);

	g_atomic_int_inc(&self->refs);
	idle = g_idle_source_new();
	g_source_set_callback(idle, poke_idle, self, (GDestroyNotify)resolver_unref);
	g_source_attach(idle, self->context);
	g_source_unref(idle);
}

void
axis_proxy_resolver_stop(AxisProxyResolver *self)
{
	if (self == NULL)
		return;

	g_mutex_lock(&self->lock);
	self->stopped = TRUE;
	g_mutex_unlock(&self->lock);

	if (self->timer != NULL) {
		g_source_destroy(self->timer);
		g_source_unref(self->timer);
		self->timer = NULL;
	}
	g_cancellable_cancel(self->cancellable);
}

void
axis_proxy_resolver_free(AxisProxyResolver *self)
{
	if (self == NULL)
		return;

	axis_proxy_resolver_stop(self);
	resolver_unref(self);
}

gboolean
axis_proxy_resolver_is_busy(AxisProxyResolver *self)
{
	return self->pending > 0;
}
