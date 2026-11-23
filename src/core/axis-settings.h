/*
 * axis-settings.h - the typed settings store
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything an administrator can change after start-up lives in the
 * "setting" table, one row per key with a JSON value.  Modules declare
 * their settings (AxisModuleInfo.settings); this store holds the declared
 * keys' values in memory and is the only code that validates and writes
 * them.
 *
 *  - A key without a stored value reads as its declared default.  A row
 *    the daemon has no declaration for is left alone and named once in a
 *    warning at load.  A stored value that no longer satisfies its
 *    declaration is not used: the default is, and one warning names the
 *    key (never the value).
 *  - Getters work from any thread and return copies.  A write validates,
 *    stores, updates the cache and then tells the subscribers, on the main
 *    loop.  A handler that needs a consistent view of several settings
 *    over a long job reads them once, at its start.
 *  - A setting declared %AXIS_SETTING_RESTART is stored at once, but its
 *    getter keeps returning the value the daemon loaded until the next
 *    start, and no notification is sent.
 *  - A setting declared %AXIS_SETTING_SECRET is write-only as far as the
 *    outside is concerned: no function here puts its value in a message or
 *    a notification.  Code that needs it (an API client) reads it with the
 *    getter.  It is kept in the table as it is: the database credentials
 *    sit in axis.conf beside it, so encrypting it with a key on the same
 *    host would protect nothing.
 *
 * Create the store on the main thread (it remembers that thread's main
 * context for notifications).  Writes block on the database: call them
 * from a worker thread, never from the main loop while it serves.
 */

#ifndef AXIS_SETTINGS_H
#define AXIS_SETTINGS_H

#include <glib.h>

#include "core/axis-module.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

typedef struct _AxisSettings AxisSettings;

/**
 * AxisSettingProblem:
 * @key: the setting
 * @reason: what is wrong, a short sentence fragment ("must be at least 1h")
 * @message: the whole message, naming the setting
 *
 * One refused value of a submission.
 */
typedef struct
{
	gchar *key;
	gchar *reason;
	gchar *message;
} AxisSettingProblem;

void
axis_setting_problem_free(AxisSettingProblem *problem);

/**
 * AxisSettingChange:
 * @key: the setting
 * @text: the new value as a person would type it (see
 *   %AXIS_SETTING_STRING_LIST for lists)
 */
typedef struct
{
	const gchar *key;
	const gchar *text;
} AxisSettingChange;

/**
 * AxisSettingsChangedFunc:
 * @settings: the store
 * @key: the setting that changed (its value is not passed on: read it)
 * @user_data: (closure): as given to axis_settings_subscribe()
 *
 * Called on the main loop after a successful write or clear.
 */
typedef void (*AxisSettingsChangedFunc)(
	AxisSettings *settings,
	const gchar  *key,
	gpointer      user_data
);

/**
 * axis_settings_new:
 *
 * An empty store: declare settings with axis_settings_add_modules() and
 * then axis_settings_load() the values.  Reference counted.
 *
 * Returns: (transfer full): the store
 */
AxisSettings *
axis_settings_new(void);

AxisSettings *
axis_settings_ref(AxisSettings *self);

void
axis_settings_unref(AxisSettings *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisSettings, axis_settings_unref)

/**
 * axis_settings_add_table:
 * @self: the store
 * @module_id: the module declaring them (for messages and the page)
 * @table: settings ended by an entry whose key is %NULL
 * @error: (nullable): return location for an error
 *
 * Declares settings.  Fails with %AXIS_ERROR_SETTING, adding none of
 * @table, when a key is malformed or already declared (the message names
 * it and the modules), a type, limit, choice or default is wrong, a secret
 * is not a string or has a default, or the store has been loaded.  @table
 * must outlive the store.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_settings_add_table(
	AxisSettings          *self,
	const gchar           *module_id,
	const AxisSettingInfo *table,
	GError               **error
);

/**
 * axis_settings_add_modules:
 *
 * Declares the settings of every module of @modules, as
 * axis_settings_add_table(), with the module's id prefixed to a failure.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_settings_add_modules(
	AxisSettings        *self,
	AxisModuleRegistry  *modules,
	GError             **error
);

/**
 * axis_settings_load:
 * @self: the store
 * @pool: (nullable): the database, which must outlive the store; %NULL
 *   keeps every value in memory only (for tests)
 * @error: (nullable): return location for an error
 *
 * Reads the table.  Blocks: call it before the main loop runs.  Rows for
 * undeclared keys are kept and named in one warning each; stored values
 * that do not satisfy their declaration are reported in one warning each
 * and read as the default.  Call it once.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_settings_load(
	AxisSettings  *self,
	AxisDbPool    *pool,
	GError       **error
);

/**
 * axis_settings_lookup:
 * @module_id: (out) (optional) (transfer none): the declaring module's id
 *
 * Returns: (transfer none) (nullable): the declaration of @key, or %NULL
 */
const AxisSettingInfo *
axis_settings_lookup(
	AxisSettings  *self,
	const gchar   *key,
	const gchar  **module_id
);

/* ── Reading ───────────────────────────────────────────────────── */

/*
 * The value in effect.  The getter must match the declared type (a
 * critical and a zero value otherwise); %AXIS_SETTING_CHOICE is read as a
 * string.
 */

gboolean
axis_settings_get_boolean(AxisSettings *self, const gchar *key);

gint64
axis_settings_get_integer(AxisSettings *self, const gchar *key);

gdouble
axis_settings_get_decimal(AxisSettings *self, const gchar *key);

/* Returns: (transfer full): the text */
gchar *
axis_settings_get_string(AxisSettings *self, const gchar *key);

/* Returns: (transfer full): the items, ended by %NULL; free with g_strfreev() */
gchar **
axis_settings_get_string_list(AxisSettings *self, const gchar *key);

/* Returns: the length in seconds */
gint64
axis_settings_get_duration(AxisSettings *self, const gchar *key);

/* Returns: the size in bytes */
gint64
axis_settings_get_byte_size(AxisSettings *self, const gchar *key);

/**
 * axis_settings_format:
 *
 * Returns: (transfer full) (nullable): the value that will be in effect
 *   (after a restart, for a restart setting) written as the Settings page
 *   edits it, or %NULL for a secret or an unknown key
 */
gchar *
axis_settings_format(AxisSettings *self, const gchar *key);

/**
 * axis_settings_format_default:
 *
 * Returns: (transfer full) (nullable): the default written like
 *   axis_settings_format(), or %NULL for a secret or an unknown key
 */
gchar *
axis_settings_format_default(AxisSettings *self, const gchar *key);

/**
 * axis_settings_is_set:
 *
 * Returns: %TRUE if a valid value is stored for @key (for a secret: that
 *   it is set)
 */
gboolean
axis_settings_is_set(AxisSettings *self, const gchar *key);

/**
 * axis_settings_restart_pending:
 *
 * Returns: %TRUE if @key is a restart setting whose stored value differs
 *   from the one in effect
 */
gboolean
axis_settings_restart_pending(AxisSettings *self, const gchar *key);

/* ── Writing ───────────────────────────────────────────────────── */

/**
 * axis_settings_check:
 * @self: the store
 * @key: a declared setting
 * @text: a value as typed
 * @problem: (out) (optional) (transfer full): why not, when it fails
 *
 * Whether @text would be accepted, changing nothing.
 *
 * Returns: %TRUE if it satisfies the declaration
 */
gboolean
axis_settings_check(
	AxisSettings        *self,
	const gchar         *key,
	const gchar         *text,
	AxisSettingProblem **problem
);

/**
 * axis_settings_set_many:
 * @self: the store
 * @changes: (array length=n): the new values
 * @n: how many
 * @changed: (out) (optional) (transfer full): the keys whose stored value
 *   was actually changed, ended by %NULL; free with g_strfreev()
 * @problems: (out) (optional) (transfer full) (element-type
 *   AxisSettingProblem): on a validation failure, one entry per refused
 *   value, a #GPtrArray whose entries are freed by the array
 * @error: (nullable): return location for an error
 *
 * Validates every value, and only if all are acceptable stores them in one
 * statement, updates the cache and notifies the subscribers of the keys
 * that changed (a value equal to the one in use is not written).  Nothing
 * is stored when any value is refused.  Blocks.
 *
 * Fails with %AXIS_ERROR_SETTING when a value or key is refused (the
 * message names the first setting and holds no value), and with a database
 * error or %AXIS_ERROR_FAILED when storing fails (the message holds no
 * value); the cache is unchanged then.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_settings_set_many(
	AxisSettings             *self,
	const AxisSettingChange  *changes,
	guint                     n,
	GStrv                    *changed,
	GPtrArray               **problems,
	GError                  **error
);

/* One value: axis_settings_set_many() with a single change. */
gboolean
axis_settings_set(
	AxisSettings  *self,
	const gchar   *key,
	const gchar   *text,
	GError       **error
);

/**
 * axis_settings_clear:
 *
 * Removes the stored value of @key, so that it reads as its default (the
 * way to un-set a secret).  Blocks.  Succeeds if nothing was stored.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_settings_clear(
	AxisSettings  *self,
	const gchar   *key,
	GError       **error
);

/* ── Notification ──────────────────────────────────────────────── */

/**
 * axis_settings_subscribe:
 * @key: (nullable): the setting to follow, or %NULL for every setting
 *
 * Returns: an id for axis_settings_unsubscribe()
 */
guint
axis_settings_subscribe(
	AxisSettings            *self,
	const gchar             *key,
	AxisSettingsChangedFunc  func,
	gpointer                 user_data
);

/* Safe to call from the subscriber itself.  After it returns, @func is not
 * called again. */
void
axis_settings_unsubscribe(
	AxisSettings *self,
	guint         subscription_id
);

G_END_DECLS

#endif /* AXIS_SETTINGS_H */
