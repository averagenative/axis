/*
 * axis-module.c - module registry
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-module.h"
#include "core/axis-error.h"

#include <string.h>

#define MODULE_ID_MAX 32

typedef struct
{
	gchar                     *id;
	gchar                     *name;
	gboolean                 (*start)(gpointer user_data, GError **error);
	void                     (*stop)(gpointer user_data);
	const AxisJobHandlerInfo  *job_handlers;
	const AxisJobScheduleInfo *job_schedules;
	const gchar *const        *job_retry_elsewhere;
	void                     (*register_routes)(AxisWeb *web, gpointer user_data);
	const AxisNavEntry        *nav_entries;
	gchar                     *description;
	const AxisSettingInfo     *settings;
	guint                      flags;
	gchar                    **depends_on;	/* %NULL-terminated, never %NULL */
	gpointer                   user_data;
	gboolean                   started;
	AxisModuleSwitch          *sw;		/* the on/off flag */
} Module;

struct _AxisModuleSwitch
{
	gint ref_count;	/* atomic */
	gint on;	/* atomic */
};

static AxisModuleSwitch *
switch_new(gboolean on)
{
	AxisModuleSwitch *sw = g_new0(AxisModuleSwitch, 1);

	sw->ref_count = 1;
	sw->on = on ? 1 : 0;
	return sw;
}

void
axis_module_switch_unref(AxisModuleSwitch *self)
{
	if (self != NULL && g_atomic_int_dec_and_test(&self->ref_count))
		g_free(self);
}

gboolean
axis_module_switch_is_on(AxisModuleSwitch *self)
{
	g_return_val_if_fail(self != NULL, FALSE);

	return g_atomic_int_get(&self->on) != 0;
}

typedef struct
{
	guint                id;
	AxisModuleStateFunc  func;
	gpointer             user_data;
} Listener;

struct _AxisModuleRegistry
{
	GPtrArray *modules;	/* of Module*, in registration order */
	GArray    *listeners;	/* of Listener */
	guint      next_listener;
	gboolean   started;
};

static gboolean module_is_on(const Module *module);

static void
module_free(Module *module)
{
	g_free(module->id);
	g_free(module->name);
	g_free(module->description);
	g_strfreev(module->depends_on);
	axis_module_switch_unref(module->sw);
	g_free(module);
}

AxisModuleRegistry *
axis_module_registry_new(void)
{
	AxisModuleRegistry *self = g_new0(AxisModuleRegistry, 1);

	self->modules = g_ptr_array_new_with_free_func((GDestroyNotify)module_free);
	self->listeners = g_array_new(FALSE, FALSE, sizeof(Listener));
	return self;
}

void
axis_module_registry_free(AxisModuleRegistry *self)
{
	if (self == NULL)
		return;

	axis_module_registry_stop_all(self);
	g_ptr_array_free(self->modules, TRUE);
	g_array_free(self->listeners, TRUE);
	g_free(self);
}

/* Lower-case letters, digits and '-', starting with a letter, 1..32. */
static gboolean
id_is_valid(const gchar *id)
{
	const gchar *p;

	if (id == NULL || id[0] < 'a' || id[0] > 'z')
		return FALSE;
	if (strlen(id) > MODULE_ID_MAX)
		return FALSE;

	for (p = id; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
			return FALSE;
	}

	return TRUE;
}

static Module *
find_module(
	AxisModuleRegistry *self,
	const gchar        *id
){
	guint i;

	for (i = 0; i < self->modules->len; i++) {
		Module *module = g_ptr_array_index(self->modules, i);

		if (strcmp(module->id, id) == 0)
			return module;
	}

	return NULL;
}

gboolean
axis_module_registry_register(
	AxisModuleRegistry    *self,
	const AxisModuleInfo  *info,
	gpointer               user_data,
	GError               **error
){
	Module *module;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(info != NULL, FALSE);

	if (self->started) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
			"cannot register module '%s': the registry has already been started",
			info->id != NULL ? info->id : "(null)");
		return FALSE;
	}

	if (!id_is_valid(info->id)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
			"invalid module id '%s' (use lower-case letters, digits and '-', "
			"starting with a letter, at most %d characters)",
			info->id != NULL ? info->id : "(null)", MODULE_ID_MAX);
		return FALSE;
	}

	if (find_module(self, info->id) != NULL) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
			"module '%s' is already registered", info->id);
		return FALSE;
	}

	if ((info->flags & AXIS_MODULE_REQUIRED) && (info->flags & AXIS_MODULE_DISABLED_BY_DEFAULT)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
			"module '%s' is required, so it cannot be off by default", info->id);
		return FALSE;
	}

	for (i = 0; info->depends_on != NULL && info->depends_on[i] != NULL; i++) {
		const gchar *dependency = info->depends_on[i];
		Module *target;

		if (strcmp(dependency, info->id) == 0) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
				"module '%s' depends on itself", info->id);
			return FALSE;
		}

		target = find_module(self, dependency);
		if (target == NULL) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
				"module '%s' depends on '%s', which is not registered "
				"(register a module's dependencies before it; a dependency that "
				"depends back on it is a cycle)", info->id, dependency);
			return FALSE;
		}
		if ((info->flags & AXIS_MODULE_REQUIRED) && !(target->flags & AXIS_MODULE_REQUIRED)) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
				"module '%s' is required but depends on '%s', which is not", info->id,
				dependency);
			return FALSE;
		}
	}

	module = g_new0(Module, 1);
	module->id = g_strdup(info->id);
	module->name = g_strdup(info->name != NULL ? info->name : info->id);
	module->start = info->start;
	module->stop = info->stop;
	module->job_handlers = info->job_handlers;
	module->job_schedules = info->job_schedules;
	module->job_retry_elsewhere = info->job_retry_elsewhere;
	module->register_routes = info->register_routes;
	module->nav_entries = info->nav_entries;
	module->description = g_strdup(info->description);
	module->settings = info->settings;
	module->flags = info->flags;
	module->depends_on = g_new0(gchar *, 1);
	for (i = 0; info->depends_on != NULL && info->depends_on[i] != NULL; i++) {
		module->depends_on = g_renew(gchar *, module->depends_on, i + 2);
		module->depends_on[i] = g_strdup(info->depends_on[i]);
		module->depends_on[i + 1] = NULL;
	}
	module->user_data = user_data;
	module->sw = switch_new(!(info->flags & AXIS_MODULE_DISABLED_BY_DEFAULT));
	g_ptr_array_add(self->modules, module);

	return TRUE;
}

gboolean
axis_module_registry_contains(
	AxisModuleRegistry *self,
	const gchar        *id
){
	g_return_val_if_fail(self != NULL, FALSE);

	return id != NULL && find_module(self, id) != NULL;
}

guint
axis_module_registry_get_count(AxisModuleRegistry *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->modules->len;
}

static Module *
module_at(
	AxisModuleRegistry *self,
	guint               index
){
	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(index < self->modules->len, NULL);

	return g_ptr_array_index(self->modules, index);
}

const gchar *
axis_module_registry_get_id(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->id : NULL;
}

const gchar *
axis_module_registry_get_name(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->name : NULL;
}

gpointer
axis_module_registry_get_user_data(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->user_data : NULL;
}

const AxisJobHandlerInfo *
axis_module_registry_get_job_handlers(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->job_handlers : NULL;
}

const AxisJobScheduleInfo *
axis_module_registry_get_job_schedules(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->job_schedules : NULL;
}

const gchar *const *
axis_module_registry_get_job_retry_elsewhere(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->job_retry_elsewhere : NULL;
}

const AxisNavEntry *
axis_module_registry_get_nav_entries(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->nav_entries : NULL;
}

gint
axis_nav_entry_compare(
	gconstpointer a,
	gconstpointer b
){
	const AxisNavEntry *x = a;
	const AxisNavEntry *y = b;

	if (x->order != y->order)
		return x->order < y->order ? -1 : 1;
	return strcmp(x->label, y->label);
}

GArray *
axis_module_registry_collect_nav(
	AxisModuleRegistry *self,
	AxisNavGroup        group,
	gboolean            admin
){
	GArray *entries = g_array_new(FALSE, FALSE, sizeof(AxisNavEntry));
	guint i;

	g_return_val_if_fail(self != NULL, entries);

	for (i = 0; i < self->modules->len; i++) {
		Module *module = g_ptr_array_index(self->modules, i);
		const AxisNavEntry *entry;

		/* A module that is switched off has no link on any page. */
		if (!module_is_on(module))
			continue;

		for (entry = module->nav_entries; entry != NULL && entry->label != NULL; entry++) {
			if (entry->group != group || (entry->admin_only && !admin))
				continue;
			g_array_append_val(entries, *entry);
		}
	}

	g_array_sort(entries, axis_nav_entry_compare);
	return entries;
}

void
axis_module_registry_register_routes(
	AxisModuleRegistry *self,
	guint               index,
	AxisWeb            *web
){
	Module *module = module_at(self, index);

	if (module != NULL && module->register_routes != NULL)
		module->register_routes(web, module->user_data);
}

const gchar *
axis_module_registry_get_description(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->description : NULL;
}

const AxisSettingInfo *
axis_module_registry_get_settings(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL ? module->settings : NULL;
}

const gchar *const *
axis_module_registry_get_depends_on(
	AxisModuleRegistry *self,
	guint               index
){
	static const gchar *const none[] = { NULL };
	Module *module = module_at(self, index);

	return module != NULL ? (const gchar *const *)module->depends_on : none;
}

gboolean
axis_module_registry_is_required(
	AxisModuleRegistry *self,
	guint               index
){
	Module *module = module_at(self, index);

	return module != NULL && (module->flags & AXIS_MODULE_REQUIRED) != 0;
}

gint
axis_module_registry_find(
	AxisModuleRegistry *self,
	const gchar        *id
){
	guint i;

	g_return_val_if_fail(self != NULL, -1);

	for (i = 0; id != NULL && i < self->modules->len; i++) {
		Module *module = g_ptr_array_index(self->modules, i);

		if (strcmp(module->id, id) == 0)
			return (gint)i;
	}

	return -1;
}

/* ── Switching modules on and off ───────────────────────────────── */

static gboolean
module_is_on(const Module *module)
{
	return axis_module_switch_is_on(module->sw);
}

static gboolean
depends_on(
	const Module *module,
	const gchar  *id
){
	guint i;

	for (i = 0; module->depends_on[i] != NULL; i++) {
		if (strcmp(module->depends_on[i], id) == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
axis_module_registry_is_enabled(
	AxisModuleRegistry *self,
	const gchar        *id
){
	Module *module;

	g_return_val_if_fail(self != NULL, FALSE);

	module = id != NULL ? find_module(self, id) : NULL;
	return module != NULL && module_is_on(module);
}

AxisModuleSwitch *
axis_module_registry_ref_switch(
	AxisModuleRegistry *self,
	const gchar        *id
){
	Module *module;

	g_return_val_if_fail(self != NULL, NULL);

	module = id != NULL ? find_module(self, id) : NULL;
	if (module == NULL)
		return NULL;

	g_atomic_int_inc(&module->sw->ref_count);
	return module->sw;
}

gboolean
axis_module_registry_check_enabled(
	AxisModuleRegistry  *self,
	const gchar         *id,
	gboolean             enabled,
	GError             **error
){
	Module *module;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	module = id != NULL ? find_module(self, id) : NULL;
	if (module == NULL) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE, "there is no module '%s'",
			id != NULL ? id : "(null)");
		return FALSE;
	}

	if (!enabled && (module->flags & AXIS_MODULE_REQUIRED)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
			"%s is required and cannot be switched off", module->name);
		return FALSE;
	}
	if (module_is_on(module) == enabled)
		return TRUE;

	if (enabled) {
		for (i = 0; module->depends_on[i] != NULL; i++) {
			Module *dependency = find_module(self, module->depends_on[i]);

			if (dependency != NULL && !module_is_on(dependency)) {
				g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
					"%s needs %s, which is switched off", module->name, dependency->name);
				return FALSE;
			}
		}
	} else {
		GString *names = NULL;

		for (i = 0; i < self->modules->len; i++) {
			Module *other = g_ptr_array_index(self->modules, i);

			if (!module_is_on(other) || !depends_on(other, module->id))
				continue;
			if (names == NULL)
				names = g_string_new(NULL);
			else
				g_string_append(names, ", ");
			g_string_append(names, other->name);
		}
		if (names != NULL) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
				"%s cannot be switched off while %s %s switched on and need%s it",
				module->name, names->str, strchr(names->str, ',') != NULL ? "are" : "is",
				strchr(names->str, ',') != NULL ? "" : "s");
			g_string_free(names, TRUE);
			return FALSE;
		}
	}

	return TRUE;
}

static void
notify(
	AxisModuleRegistry *self,
	const gchar        *id,
	gboolean            enabled
){
	GArray *copy = g_array_sized_new(FALSE, FALSE, sizeof(Listener), self->listeners->len);
	guint i;

	g_array_append_vals(copy, self->listeners->data, self->listeners->len);
	for (i = 0; i < copy->len; i++) {
		const Listener *listener = &g_array_index(copy, Listener, i);

		listener->func(self, id, enabled, listener->user_data);
	}
	g_array_free(copy, TRUE);
}

gboolean
axis_module_registry_set_enabled(
	AxisModuleRegistry  *self,
	const gchar         *id,
	gboolean             enabled,
	GError             **error
){
	Module *module;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!axis_module_registry_check_enabled(self, id, enabled, error))
		return FALSE;

	module = find_module(self, id);
	if (module_is_on(module) == enabled)
		return TRUE;

	if (enabled) {
		if (self->started && module->start != NULL) {
			g_autoptr(GError) local = NULL;

			if (!module->start(module->user_data, &local)) {
				if (local == NULL)
					g_set_error_literal(&local, AXIS_ERROR, AXIS_ERROR_MODULE,
						"failed to start");
				g_propagate_prefixed_error(error, g_steal_pointer(&local), "%s: ", module->id);
				return FALSE;
			}
		}
		if (self->started)
			module->started = TRUE;
		g_atomic_int_set(&module->sw->on, 1);
	} else {
		g_atomic_int_set(&module->sw->on, 0);
		if (module->started) {
			module->started = FALSE;
			if (module->stop != NULL)
				module->stop(module->user_data);
		}
	}

	notify(self, module->id, enabled);
	return TRUE;
}

/* A module that is on while a dependency is off is switched off.  Modules
 * come after their dependencies, so one pass is enough. */
static void
settle(AxisModuleRegistry *self)
{
	guint i;

	for (i = 0; i < self->modules->len; i++) {
		Module *module = g_ptr_array_index(self->modules, i);
		guint j;

		if (!module_is_on(module))
			continue;

		for (j = 0; module->depends_on[j] != NULL; j++) {
			Module *dependency = find_module(self, module->depends_on[j]);

			if (dependency != NULL && !module_is_on(dependency)) {
				g_warning("module '%s' is switched off because '%s', which it needs, is off",
					module->id, dependency->id);
				g_atomic_int_set(&module->sw->on, 0);
				break;
			}
		}
	}
}

void
axis_module_registry_load_states(
	AxisModuleRegistry  *self,
	const gchar *const  *ids,
	const gboolean      *states,
	guint                n
){
	guint i;

	g_return_if_fail(self != NULL);
	g_return_if_fail(!self->started);

	for (i = 0; i < n; i++) {
		Module *module = find_module(self, ids[i]);

		if (module == NULL) {
			g_warning("a stored module state is for '%s', which is not registered; keeping it",
				ids[i]);
			continue;
		}
		if (!states[i] && (module->flags & AXIS_MODULE_REQUIRED)) {
			g_warning("module '%s' is required; ignoring the stored state that switches it off",
				module->id);
			continue;
		}
		g_atomic_int_set(&module->sw->on, states[i] ? 1 : 0);
	}

	settle(self);
}

guint
axis_module_registry_add_listener(
	AxisModuleRegistry  *self,
	AxisModuleStateFunc  func,
	gpointer             user_data
){
	Listener listener;

	g_return_val_if_fail(self != NULL, 0);
	g_return_val_if_fail(func != NULL, 0);

	listener.id = ++self->next_listener;
	listener.func = func;
	listener.user_data = user_data;
	g_array_append_val(self->listeners, listener);

	return listener.id;
}

void
axis_module_registry_remove_listener(
	AxisModuleRegistry *self,
	guint               listener_id
){
	guint i;

	g_return_if_fail(self != NULL);

	for (i = 0; i < self->listeners->len; i++) {
		if (g_array_index(self->listeners, Listener, i).id == listener_id) {
			g_array_remove_index(self->listeners, i);
			return;
		}
	}
}

/* Stop every module whose start succeeded, newest first. */
static void
stop_started(AxisModuleRegistry *self)
{
	guint i;

	for (i = self->modules->len; i > 0; i--) {
		Module *module = g_ptr_array_index(self->modules, i - 1);

		if (!module->started)
			continue;

		module->started = FALSE;
		if (module->stop != NULL)
			module->stop(module->user_data);
	}
}

gboolean
axis_module_registry_start_all(
	AxisModuleRegistry  *self,
	GError             **error
){
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->started) {
		g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_MODULE,
			"the module registry has already been started");
		return FALSE;
	}

	settle(self);

	for (i = 0; i < self->modules->len; i++) {
		Module *module = g_ptr_array_index(self->modules, i);

		/* A module that is off is started when it is switched on. */
		if (!module_is_on(module))
			continue;

		if (module->start != NULL) {
			g_autoptr(GError) local = NULL;

			if (!module->start(module->user_data, &local)) {
				if (local == NULL)
					g_set_error_literal(&local, AXIS_ERROR, AXIS_ERROR_MODULE,
						"failed to start");

				stop_started(self);
				g_propagate_prefixed_error(error, g_steal_pointer(&local),
					"%s: ", module->id);
				return FALSE;
			}
		}

		module->started = TRUE;
	}

	self->started = TRUE;
	return TRUE;
}

void
axis_module_registry_stop_all(AxisModuleRegistry *self)
{
	g_return_if_fail(self != NULL);

	if (!self->started)
		return;

	self->started = FALSE;
	stop_started(self);
}
