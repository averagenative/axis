/*
 * axis-test-resolver.c - a GResolver that answers from a table, for tests
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-resolver.h"

#include <string.h>

#define AXIS_TYPE_TEST_RESOLVER (axis_test_resolver_get_type())

typedef struct
{
	GResolver   parent_instance;
	GHashTable *answers;	/* name -> NULL-terminated strv, or NULL for a failure */
	GHashTable *counts;	/* name -> GUINT_TO_POINTER(asked) */
	GList      *held;		/* of GTask */
	gboolean    hold;
} AxisTestResolver;

typedef GResolverClass AxisTestResolverClass;

GType axis_test_resolver_get_type(void);
G_DEFINE_TYPE(AxisTestResolver, axis_test_resolver, G_TYPE_RESOLVER)

static GList *
answer_for(
	AxisTestResolver *self,
	const gchar      *name,
	GError          **error
){
	gpointer found = NULL;
	GList *list = NULL;
	gchar **strv;
	guint i;

	if (!g_hash_table_lookup_extended(self->answers, name, NULL, &found) || found == NULL) {
		g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "no such host: %s", name);
		return NULL;
	}
	strv = found;
	for (i = 0; strv[i] != NULL; i++)
		list = g_list_append(list, g_inet_address_new_from_string(strv[i]));
	return list;
}

static gboolean
complete_idle(gpointer data)
{
	GTask *task = data;
	AxisTestResolver *self = g_task_get_source_object(task);
	const gchar *name = g_task_get_task_data(task);
	GError *error = NULL;
	GList *list = answer_for(self, name, &error);

	if (error != NULL)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, list, (GDestroyNotify)g_resolver_free_addresses);
	g_object_unref(task);
	return G_SOURCE_REMOVE;
}

static void
lookup_async(
	GResolver           *resolver,
	const gchar         *name,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AxisTestResolver *self = (AxisTestResolver *)resolver;
	GTask *task = g_task_new(resolver, cancellable, callback, user_data);
	gpointer count = g_hash_table_lookup(self->counts, name);

	g_hash_table_replace(self->counts, g_strdup(name), GUINT_TO_POINTER(GPOINTER_TO_UINT(count) + 1));
	g_task_set_task_data(task, g_strdup(name), g_free);
	if (self->hold)
		self->held = g_list_append(self->held, task);
	else
		g_idle_add(complete_idle, task);
}

static void
lookup_with_flags_async(
	GResolver              *resolver,
	const gchar            *name,
	GResolverNameLookupFlags flags,
	GCancellable           *cancellable,
	GAsyncReadyCallback     callback,
	gpointer                user_data
){
	lookup_async(resolver, name, cancellable, callback, user_data);
}

static GList *
lookup_finish(
	GResolver     *resolver,
	GAsyncResult  *result,
	GError       **error
){
	return g_task_propagate_pointer(G_TASK(result), error);
}

static GList *
lookup_sync(
	GResolver     *resolver,
	const gchar   *name,
	GCancellable  *cancellable,
	GError       **error
){
	return answer_for((AxisTestResolver *)resolver, name, error);
}

static void
axis_test_resolver_finalize(GObject *object)
{
	AxisTestResolver *self = (AxisTestResolver *)object;

	g_list_free_full(self->held, g_object_unref);
	g_hash_table_unref(self->answers);
	g_hash_table_unref(self->counts);
	G_OBJECT_CLASS(axis_test_resolver_parent_class)->finalize(object);
}

static void
axis_test_resolver_class_init(AxisTestResolverClass *klass)
{
	GResolverClass *resolver_class = G_RESOLVER_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = axis_test_resolver_finalize;
	resolver_class->lookup_by_name = lookup_sync;
	resolver_class->lookup_by_name_async = lookup_async;
	resolver_class->lookup_by_name_finish = lookup_finish;
	resolver_class->lookup_by_name_with_flags_async = lookup_with_flags_async;
	resolver_class->lookup_by_name_with_flags_finish = lookup_finish;
}

static void
axis_test_resolver_init(AxisTestResolver *self)
{
	self->answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
	self->counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GResolver *
axis_test_resolver_new(void)
{
	return g_object_new(AXIS_TYPE_TEST_RESOLVER, NULL);
}

void
axis_test_resolver_set(
	GResolver          *resolver,
	const gchar        *name,
	const gchar *const *addresses
){
	AxisTestResolver *self = (AxisTestResolver *)resolver;

	g_hash_table_replace(self->answers, g_strdup(name), addresses != NULL ? g_strdupv((gchar **)addresses) : NULL);
}

void
axis_test_resolver_hold(
	GResolver *resolver,
	gboolean   hold
){
	((AxisTestResolver *)resolver)->hold = hold;
}

void
axis_test_resolver_release(GResolver *resolver)
{
	AxisTestResolver *self = (AxisTestResolver *)resolver;
	GList *l;

	self->hold = FALSE;
	for (l = self->held; l != NULL; l = l->next)
		g_idle_add(complete_idle, l->data);	/* the idle owns the list's reference */
	g_list_free(self->held);
	self->held = NULL;
}

guint
axis_test_resolver_count(
	GResolver   *resolver,
	const gchar *name
){
	return GPOINTER_TO_UINT(g_hash_table_lookup(((AxisTestResolver *)resolver)->counts, name));
}

void
axis_test_resolver_drain(void)
{
	guint guard = 0;

	while (g_main_context_iteration(NULL, FALSE) && ++guard < 1000)
		;
}
