/*
 * axis-web-settings.h - the Settings and Modules pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_settings_get_module() with the module registry,
 * passing the AxisWebSettings as the module's user data; its pages then
 * exist once axis_web_new() has run.  The module is called "web", is
 * required, and declares one setting of its own:
 *
 *   web.session_idle  how long a session may go unused before the user has
 *                     to log in again (1 hour to 90 days, default 30 days),
 *                     applied to the session store as it changes
 *   web.default_theme the colour theme of logged-out pages and of users who
 *                     chose none (a choice among the built-in themes,
 *                     default "midnight"), applied as it changes
 *
 * Pages (administrators only; every POST needs the CSRF token):
 *
 *   GET  /admin/settings                 every declared setting of every
 *                                        module that is switched on, one
 *                                        form per module
 *   POST /admin/settings/:module         validates all of the module's
 *                                        submitted values and stores them
 *                                        only if every one is acceptable;
 *                                        a form shown in the setup guide
 *                                        comes back to it ("return", see
 *                                        web/axis-web-setup-path.h)
 *   POST /admin/settings/:module/clear   removes a secret's value (the
 *                                        form field "key")
 *   GET  /admin/modules                  the modules, their state, their
 *                                        dependencies and a switch
 *   POST /admin/modules/:id/enable       switches a module on or off
 *   POST /admin/modules/:id/disable      (refused for a required module and
 *                                        where a dependency forbids it)
 *
 * A secret setting is shown only as set or not set; a form never carries
 * its value, and a submitted secret appears in no log, message or page.
 * Database work happens on a worker thread; pages are built on the main
 * loop.  Each change is logged with the acting user and the key or module
 * id (never a value).
 */

#ifndef AXIS_WEB_SETTINGS_H
#define AXIS_WEB_SETTINGS_H

#include <glib.h>
#include <htmx-glib.h>

#include "core/axis-module.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

typedef struct _AxisWebSettings AxisWebSettings;

/**
 * axis_web_settings_new:
 * @settings: (transfer none): the store; it must outlive the result
 * @pool: (nullable): where module states are stored; must outlive the
 *   result.  %NULL (tests) keeps them in memory only.
 *
 * Follows "web.session_idle" in @settings.  Create on the main thread.
 *
 * Returns: (transfer full): the pages' state
 */
AxisWebSettings *
axis_web_settings_new(
	AxisSettings *settings,
	AxisDbPool   *pool
);

/**
 * axis_web_settings_sync:
 *
 * Applies the settings this module follows (the session idle limit, the default theme and the log level) now.
 * Call it once the store is loaded.
 */
void
axis_web_settings_sync(AxisWebSettings *self);

/**
 * axis_web_settings_stop:
 *
 * Refuses new work and waits (running the default main context) for the
 * database work in flight.  Call it once the server has stopped taking
 * requests and before the pool is freed.
 */
void
axis_web_settings_stop(AxisWebSettings *self);

/* Stops @self if needed, and frees it. */
void
axis_web_settings_free(AxisWebSettings *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebSettings, axis_web_settings_free)

/**
 * axis_web_settings_get_module:
 *
 * Returns: (transfer none): the module description, in static storage
 */
const AxisModuleInfo *
axis_web_settings_get_module(void);

/* ── One module's form, wherever it is shown ───────────────────── */

/**
 * AxisWebSettingsChoice:
 * @value: what is stored
 * @label: its name for people
 * @detail: (nullable): one line about it
 *
 * One value of a text setting offered as a choice.
 */
typedef struct
{
	const gchar *value;
	const gchar *label;
	const gchar *detail;
} AxisWebSettingsChoice;

/**
 * AxisWebSettingsForm:
 * @keys: (nullable) (array zero-terminated=1): the settings shown, in the
 *   table's order whatever their order here; %NULL for every one
 * @button: (nullable): the button's words; %NULL: "Save <module> settings"
 * @return_path: (nullable): an address of the setup guide
 *   (axis_web_setup_return_path()): saving goes back there
 * @choice_key: (nullable): a text setting shown as a list of @choices to
 *   pick one of, rather than a field to type it in (it is checked and
 *   stored as typed text is)
 * @choices: (array length=n_choices) (nullable)
 * @n_choices: how many
 * @reasons: (nullable): setting key -> why the value was refused
 * @texts: (nullable): setting key -> the text submitted, shown again
 *
 * How to draw a module's settings form.  Zeroed, it is the Settings page's.
 */
typedef struct
{
	const gchar *const          *keys;
	const gchar                 *button;
	const gchar                 *return_path;
	const gchar                 *choice_key;
	const AxisWebSettingsChoice *choices;
	guint                        n_choices;
	GHashTable                  *reasons;
	GHashTable                  *texts;
} AxisWebSettingsForm;

/**
 * axis_web_settings_form_new:
 * @settings: the store
 * @request: the request the page answers (for the CSRF field)
 * @module_id: the module whose settings they are
 * @module_name: its name, for the button
 * @table: its settings
 * @form: (nullable): what to show; %NULL for all of them
 *
 * The form the Settings page shows for a module, posting to
 * /admin/settings/@module_id.  A secret is never put in it: its field is
 * empty, and says whether a value is stored.
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_settings_form_new(
	AxisSettings              *settings,
	HtmxRequest               *request,
	const gchar               *module_id,
	const gchar               *module_name,
	const AxisSettingInfo     *table,
	const AxisWebSettingsForm *form
);

G_END_DECLS

#endif /* AXIS_WEB_SETTINGS_H */
