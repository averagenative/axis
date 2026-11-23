/*
 * axis-module.h - module registry
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Core features and media types (movies, TV, music, books) register here.
 * The registry starts them in registration order and stops them in
 * reverse.  This struct will grow (migrations) in later changes; always
 * initialise it with designated-style zeroing (static storage or memset)
 * so new fields default to NULL.
 */

#ifndef AXIS_MODULE_H
#define AXIS_MODULE_H

#include <glib.h>

#include "core/axis-setting.h"

G_BEGIN_DECLS

/* Defined in jobs/axis-job-registry.h; the registry only keeps pointers. */
typedef struct _AxisJobHandlerInfo  AxisJobHandlerInfo;
typedef struct _AxisJobScheduleInfo AxisJobScheduleInfo;

/* Defined in web/axis-web.h. */
typedef struct _AxisWeb AxisWeb;

/**
 * AxisNavGroup:
 * @AXIS_NAV_GROUP_SYSTEM: the installation: settings, users, modules.
 *   Zero, so an entry that names no group lands in the least prominent
 *   place.
 * @AXIS_NAV_GROUP_MANAGEMENT: looking after what is there: the library,
 *   its analysis, storage, conversion
 * @AXIS_NAV_GROUP_ACTIVITY: what is happening: jobs, later downloads and
 *   imports
 * @AXIS_NAV_GROUP_MEDIA: the media themselves, one section per media type
 *   (Movies, TV, ...)
 *
 * The primary tab a page belongs to.  The order of the tabs is fixed by
 * the web shell (the dashboard, media, activity, media management,
 * system), not by these values.
 */
typedef enum
{
	AXIS_NAV_GROUP_SYSTEM = 0,
	AXIS_NAV_GROUP_MANAGEMENT,
	AXIS_NAV_GROUP_ACTIVITY,
	AXIS_NAV_GROUP_MEDIA
} AxisNavGroup;

/* How many groups there are (one more than the largest value). */
#define AXIS_NAV_GROUP_COUNT 4

/**
 * AxisNavEntry:
 * @label: the text of the link
 * @path: where it goes, an absolute path such as "/movies"
 * @order: within its group, entries are listed by ascending @order, then
 *   by @label
 * @group: the primary tab the page belongs to; an entry that does not
 *   say is in %AXIS_NAV_GROUP_SYSTEM
 * @admin_only: shown to administrators only.  This hides the link; the
 *   route itself must still refuse anyone else (AXIS_WEB_ROUTE_ADMIN).
 * @section: (nullable): in %AXIS_NAV_GROUP_MEDIA, the media type the page
 *   is of ("Movies", "TV"): the tab's second row lists the types, in the
 *   order of their first page, and a third row the open type's pages.  An
 *   entry with none is a type of its own, named by @label.
 * @run: entries that belong together share a run; where the run changes
 *   along a row, a thin divider separates them (0 for all, normally)
 *
 * One page of the navigation.  A table of these, ended by an entry whose
 * @label is %NULL, is a module's contribution to it.  New fields are added
 * at the end, so a table that does not set them has none.
 */
typedef struct
{
	const gchar  *label;
	const gchar  *path;
	gint          order;
	AxisNavGroup  group;
	gboolean      admin_only;
	const gchar  *section;
	gint          run;
} AxisNavEntry;

/**
 * AxisModuleInfo:
 * @id: unique identifier: lower-case letters, digits and '-', starting
 *   with a letter, at most 32 characters
 * @name: human-readable name
 * @start: (nullable): called by axis_module_registry_start_all()
 * @stop: (nullable): called by axis_module_registry_stop_all(), only for
 *   modules whose start succeeded (or that have no start)
 * @job_handlers: (nullable): the job kinds this module runs, an array
 *   ended by an entry whose kind is %NULL; %NULL for none.  The handlers
 *   get the module's user data.
 * @job_schedules: (nullable): the module's default schedules, an array
 *   ended by an entry whose name is %NULL; %NULL for none
 * @register_routes: (nullable): called once by axis_web_new() with the
 *   web shell and the module's user data, to add the module's pages with
 *   axis_web_get() and friends
 * @nav_entries: (nullable): the module's navigation entries, an array
 *   ended by an entry whose label is %NULL; %NULL for none
 * @description: (nullable): one sentence for the Modules page
 * @settings: (nullable): the module's settings, an array ended by an entry
 *   whose key is %NULL; %NULL for none (see core/axis-settings.h)
 * @flags: #AxisModuleFlags; 0 is a module that can be switched off and is
 *   on by default
 * @depends_on: (nullable): the ids of the modules this one needs, ended by
 *   %NULL.  Each must be registered before this one (the registry starts
 *   modules in registration order, so a dependency is started first).
 * @job_retry_elsewhere: (nullable): pairs of a kind of @job_handlers and
 *   one plain sentence saying where a person asks for its work again, for
 *   a kind whose jobs its owner tries again rather than the Jobs page's
 *   Retry (axis_job_registry_set_retry_elsewhere()), ended by %NULL
 *
 * The registry copies @id, @name, @description and the ids in
 * @depends_on; the struct need not outlive registration.  The tables are
 * not copied: they must outlive the registry (static tables, normally).
 * axis_job_registry_add_modules(), the settings store and the web shell
 * read them.  New fields are added at the end: a description that does
 * not set them (static storage or memset) has no settings or
 * dependencies, is not required and is on by default.
 *
 * A module that is switched off (see axis_module_registry_set_enabled())
 * is not started, has no navigation entry, its routes answer 404, its
 * schedules do not fire and its queued jobs wait.  Its start hook is
 * called when it is switched on, its stop hook when it is switched off.
 */
typedef struct _AxisModuleInfo
{
	const gchar               *id;
	const gchar               *name;
	gboolean                 (*start)(gpointer user_data, GError **error);
	void                     (*stop)(gpointer user_data);
	const AxisJobHandlerInfo  *job_handlers;
	const AxisJobScheduleInfo *job_schedules;
	void                     (*register_routes)(AxisWeb *web, gpointer user_data);
	const AxisNavEntry        *nav_entries;
	const gchar               *description;
	const AxisSettingInfo     *settings;
	guint                      flags;
	const gchar *const        *depends_on;
	const gchar *const        *job_retry_elsewhere;
} AxisModuleInfo;

/**
 * AxisModuleFlags:
 * @AXIS_MODULE_REQUIRED: the daemon cannot do without it: it has no switch
 *   on the Modules page and is never switched off.  It may only depend on
 *   required modules.
 * @AXIS_MODULE_DISABLED_BY_DEFAULT: off until an administrator switches it
 *   on (a stored choice overrides this)
 */
typedef enum
{
	AXIS_MODULE_REQUIRED            = 1 << 0,
	AXIS_MODULE_DISABLED_BY_DEFAULT = 1 << 1
} AxisModuleFlags;

typedef struct _AxisModuleRegistry AxisModuleRegistry;

AxisModuleRegistry *
axis_module_registry_new(void);

/**
 * axis_module_registry_free:
 *
 * Stops any started modules first (as axis_module_registry_stop_all()).
 */
void
axis_module_registry_free(AxisModuleRegistry *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisModuleRegistry, axis_module_registry_free)

/**
 * axis_module_registry_register:
 * @self: the registry
 * @info: the module description
 * @user_data: (nullable): passed to @info's start and stop
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_ERROR_MODULE, leaving the registry unchanged, when
 * @info->id is missing or malformed, is already registered (message names
 * the id), when the registry has already been started, when @info depends
 * on itself or on a module not registered yet (the message names both
 * modules), or when a required module is off by default or depends on one
 * that is not required.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_module_registry_register(
	AxisModuleRegistry    *self,
	const AxisModuleInfo  *info,
	gpointer               user_data,
	GError               **error
);

/**
 * axis_module_registry_contains:
 *
 * Returns: %TRUE if a module with @id is registered
 */
gboolean
axis_module_registry_contains(
	AxisModuleRegistry *self,
	const gchar        *id
);

guint
axis_module_registry_get_count(AxisModuleRegistry *self);

/**
 * axis_module_registry_get_id:
 * @self: the registry
 * @index: 0 to axis_module_registry_get_count() - 1, in registration order
 *
 * Returns: (transfer none) (nullable): the module's id, or %NULL (and a
 *   critical) for an index out of range
 */
const gchar *
axis_module_registry_get_id(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_name:
 *
 * Returns: (transfer none) (nullable): the human-readable name of the
 *   module at @index (its id when it gave none), or %NULL (and a
 *   critical) for an index out of range
 */
const gchar *
axis_module_registry_get_name(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_user_data:
 *
 * Returns: (transfer none) (nullable): the user data the module at @index
 *   was registered with
 */
gpointer
axis_module_registry_get_user_data(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_job_handlers:
 *
 * Returns: (transfer none) (nullable): the job handler table of the module
 *   at @index, or %NULL if it has none
 */
const AxisJobHandlerInfo *
axis_module_registry_get_job_handlers(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_job_schedules:
 *
 * Returns: (transfer none) (nullable): the default schedule table of the
 *   module at @index, or %NULL if it has none
 */
const AxisJobScheduleInfo *
axis_module_registry_get_job_schedules(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_job_retry_elsewhere:
 *
 * Returns: (transfer none) (nullable): the pairs of kind and sentence of
 *   the module at @index (AxisModuleInfo.job_retry_elsewhere), or %NULL
 */
const gchar *const *
axis_module_registry_get_job_retry_elsewhere(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_nav_entries:
 *
 * Returns: (transfer none) (nullable): the navigation table of the module
 *   at @index, or %NULL if it has none
 */
const AxisNavEntry *
axis_module_registry_get_nav_entries(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_collect_nav:
 * @self: the registry
 * @group: the group to collect
 * @admin: whether the user is an administrator (else entries marked
 *   admin_only are left out)
 *
 * The navigation entries of @group from every module that is switched
 * on, ordered by order and then label.  The entries are copies of the
 * modules' own (their strings are the modules' static ones).
 *
 * Returns: (transfer full) (element-type AxisNavEntry): the entries
 */
GArray *
axis_module_registry_collect_nav(
	AxisModuleRegistry *self,
	AxisNavGroup        group,
	gboolean            admin
);

/**
 * axis_nav_entry_compare:
 *
 * The order of entries within a group: by order, then by label.
 */
gint
axis_nav_entry_compare(
	gconstpointer a,
	gconstpointer b
);

/**
 * axis_module_registry_register_routes:
 * @self: the registry
 * @index: the module
 * @web: passed to the module's register_routes
 *
 * Calls the module's register_routes with its user data, if it has one.
 */
void
axis_module_registry_register_routes(
	AxisModuleRegistry *self,
	guint               index,
	AxisWeb            *web
);

/**
 * axis_module_registry_get_description:
 *
 * Returns: (transfer none) (nullable): the module's description, or %NULL
 */
const gchar *
axis_module_registry_get_description(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_settings:
 *
 * Returns: (transfer none) (nullable): the setting table of the module at
 *   @index, or %NULL if it has none
 */
const AxisSettingInfo *
axis_module_registry_get_settings(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_get_depends_on:
 *
 * Returns: (transfer none): the ids the module at @index depends on, ended
 *   by %NULL (empty for none); owned by the registry
 */
const gchar *const *
axis_module_registry_get_depends_on(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_is_required:
 *
 * Returns: %TRUE if the module at @index has %AXIS_MODULE_REQUIRED
 */
gboolean
axis_module_registry_is_required(
	AxisModuleRegistry *self,
	guint               index
);

/**
 * axis_module_registry_find:
 *
 * Returns: the index of the module called @id, or -1
 */
gint
axis_module_registry_find(
	AxisModuleRegistry *self,
	const gchar        *id
);

/**
 * axis_module_registry_is_enabled:
 * @self: the registry
 * @id: a module id
 *
 * Callable from any thread, at any time after registration.  The answer is
 * a single flag read atomically: it does not wait for a switch in progress.
 *
 * Returns: %TRUE if the module is registered and switched on
 */
gboolean
axis_module_registry_is_enabled(
	AxisModuleRegistry *self,
	const gchar        *id
);

/**
 * AxisModuleSwitch:
 *
 * A module's on/off flag, reference counted so that code that must keep
 * asking (the job queue's threads) can hold it without holding the
 * registry.  It keeps its last value if the registry goes away.
 */
typedef struct _AxisModuleSwitch AxisModuleSwitch;

/**
 * axis_module_registry_ref_switch:
 *
 * Returns: (transfer full) (nullable): the switch of the module called
 *   @id, or %NULL if there is none
 */
AxisModuleSwitch *
axis_module_registry_ref_switch(
	AxisModuleRegistry *self,
	const gchar        *id
);

/* Any thread. */
gboolean
axis_module_switch_is_on(AxisModuleSwitch *self);

void
axis_module_switch_unref(AxisModuleSwitch *self);

/**
 * axis_module_registry_check_enabled:
 * @self: the registry
 * @id: a module id
 * @enabled: the state to check
 * @error: (nullable): return location for an error
 *
 * Whether switching @id to @enabled is allowed, changing nothing.  Fails
 * with %AXIS_ERROR_MODULE when @id is not registered, when it is required
 * and @enabled is %FALSE, when @enabled is %TRUE and a module it depends
 * on is off (the message names the module and the dependency), or when
 * @enabled is %FALSE and a module that is on depends on it (the message
 * names the dependents).  Switching to the state a module is in always
 * succeeds.
 *
 * Returns: %TRUE if the switch is allowed
 */
gboolean
axis_module_registry_check_enabled(
	AxisModuleRegistry  *self,
	const gchar         *id,
	gboolean             enabled,
	GError             **error
);

/**
 * axis_module_registry_set_enabled:
 * @self: the registry
 * @id: a module id
 * @enabled: the new state
 * @error: (nullable): return location for an error
 *
 * Switches a module on or off, as axis_module_registry_check_enabled()
 * allows.  Main loop thread only.  Switching on a module of a started
 * registry calls its start hook first, and the module stays off if that
 * fails (the error is the hook's, with the module id prefixed); switching
 * one off calls its stop hook after the flag changes.  Before the registry
 * is started no hook is called.  Listeners are told afterwards.  This does
 * not store anything: see core/axis-module-state.h.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_module_registry_set_enabled(
	AxisModuleRegistry  *self,
	const gchar         *id,
	gboolean             enabled,
	GError             **error
);

/**
 * axis_module_registry_load_states:
 * @self: the registry
 * @ids: (array length=n) (element-type utf8): module ids
 * @states: (array length=n): whether each is on
 * @n: how many
 *
 * Sets the starting state from what was stored, before the registry is
 * started (otherwise a critical).  An id that is not registered is
 * ignored with one warning; a required module cannot be switched off; a
 * module whose dependency ends up off is switched off too (a warning names
 * both).  Nothing is stored and no hook or listener is called.
 */
void
axis_module_registry_load_states(
	AxisModuleRegistry  *self,
	const gchar *const  *ids,
	const gboolean      *states,
	guint                n
);

/**
 * AxisModuleStateFunc:
 * @registry: the registry
 * @id: the module that changed
 * @enabled: its new state
 * @user_data: (closure): as given when adding the listener
 *
 * Called by axis_module_registry_set_enabled() on the thread that called
 * it, after the change and after the module's hook.
 */
typedef void (*AxisModuleStateFunc)(
	AxisModuleRegistry *registry,
	const gchar        *id,
	gboolean            enabled,
	gpointer            user_data
);

/**
 * axis_module_registry_add_listener:
 *
 * Returns: an id for axis_module_registry_remove_listener()
 */
guint
axis_module_registry_add_listener(
	AxisModuleRegistry  *self,
	AxisModuleStateFunc  func,
	gpointer             user_data
);

void
axis_module_registry_remove_listener(
	AxisModuleRegistry *self,
	guint               listener_id
);

/**
 * axis_module_registry_start_all:
 *
 * Starts every module that is switched on, in registration order (a module
 * that is on while a dependency is off is first switched off, with a
 * warning).  If one fails, the modules
 * already started are stopped in reverse order, the registry is left
 * not-started, and the module's error is returned with its id prefixed to
 * the message.  Calling this on a started registry is an error.
 *
 * Returns: %TRUE if every module started
 */
gboolean
axis_module_registry_start_all(
	AxisModuleRegistry  *self,
	GError             **error
);

/**
 * axis_module_registry_stop_all:
 *
 * Stops started modules in reverse registration order.  Does nothing if
 * the registry is not started, so it is safe to call twice.
 */
void
axis_module_registry_stop_all(AxisModuleRegistry *self);

G_END_DECLS

#endif /* AXIS_MODULE_H */
