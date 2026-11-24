/*
 * axis-setting.h - how a module declares a setting
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module lists its settings in a static table (AxisModuleInfo.settings).
 * The declaration is the one source for validation, the default and the
 * Settings page.  The store that holds the values is axis-settings.h.
 */

#ifndef AXIS_SETTING_H
#define AXIS_SETTING_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * AxisSettingType:
 * @AXIS_SETTING_BOOLEAN: true or false
 * @AXIS_SETTING_INTEGER: a whole number, within [minimum, maximum]
 * @AXIS_SETTING_DECIMAL: a number with a fractional part, within
 *   [minimum_decimal, maximum_decimal]
 * @AXIS_SETTING_STRING: text of minimum to maximum bytes (maximum 0 means
 *   %AXIS_SETTING_DEFAULT_MAX_STRING)
 * @AXIS_SETTING_CHOICE: one of @choices
 * @AXIS_SETTING_STRING_LIST: a list of strings, minimum to maximum items
 *   (maximum 0 means %AXIS_SETTING_DEFAULT_MAX_ITEMS), edited one per line
 * @AXIS_SETTING_DURATION: a time span, kept as whole seconds, within
 *   [minimum, maximum] seconds, edited with units ("90m", "1h 30m", "7d")
 * @AXIS_SETTING_BYTE_SIZE: an amount of storage, kept as bytes, within
 *   [minimum, maximum] bytes, edited with units ("512MiB", "10GiB")
 */
typedef enum
{
	AXIS_SETTING_BOOLEAN = 1,
	AXIS_SETTING_INTEGER,
	AXIS_SETTING_DECIMAL,
	AXIS_SETTING_STRING,
	AXIS_SETTING_CHOICE,
	AXIS_SETTING_STRING_LIST,
	AXIS_SETTING_DURATION,
	AXIS_SETTING_BYTE_SIZE
} AxisSettingType;

/**
 * AxisSettingFlags:
 * @AXIS_SETTING_SECRET: write-only: the value is never rendered, logged or
 *   put in an error or notification.  Only for %AXIS_SETTING_STRING.
 * @AXIS_SETTING_RESTART: a change is stored at once but the getters keep
 *   returning the value the daemon started with until it is restarted; the
 *   Settings page says so.  Few settings should need this.
 * @AXIS_SETTING_ZERO_OFF: 0 is allowed besides [minimum, maximum], and
 *   means that what the setting limits is switched off ("0: kept
 *   forever"); its help says so.  Only for %AXIS_SETTING_INTEGER,
 *   %AXIS_SETTING_DURATION and %AXIS_SETTING_BYTE_SIZE.
 */
typedef enum
{
	AXIS_SETTING_SECRET   = 1 << 0,
	AXIS_SETTING_RESTART  = 1 << 1,
	AXIS_SETTING_ZERO_OFF = 1 << 2
} AxisSettingFlags;

#define AXIS_SETTING_DEFAULT_MAX_STRING 4096
#define AXIS_SETTING_DEFAULT_MAX_ITEMS  256
#define AXIS_SETTING_MAX_ITEM_BYTES     1024

/**
 * AxisSettingInfo:
 * @key: "<name of the module>.<setting>": a module name or id in lower-case
 *   letters, digits and '-', a '.', then lower-case letters, digits and '_',
 *   starting with a letter; at most 64 characters.  Unique across modules.
 * @type: what kind of value it holds
 * @label: the name on the Settings page
 * @help: (nullable): one or two sentences under the field
 * @flags: #AxisSettingFlags
 * @default_value: (nullable): the default, written as a person would type
 *   it ("true", "42", "7d", "10GiB", one item per line for a list); %NULL
 *   is false, 0, an empty string or an empty list, whichever fits
 * @minimum: integer, duration (seconds) and byte size: the smallest value;
 *   string and list: the fewest bytes or items.  Write %G_MININT64 for
 *   integers that may be negative without a lower limit.
 * @maximum: integer, duration, byte size: the largest value, 0 meaning no
 *   upper limit; string and list: the most bytes or items, 0 meaning the
 *   default maximum above
 * @minimum_decimal: decimal: the smallest value
 * @maximum_decimal: decimal: the largest value; when both decimal limits
 *   are 0 there are none
 * @choices: choice: the allowed values, ended by %NULL
 *
 * The words the Settings page shows for each allowed value of a choice are
 * registered beside the table with axis_setting_set_choice_labels(); the
 * stored values stay @choices.
 *
 * The default must satisfy the declaration: a module whose does not is
 * refused when the settings are collected.
 */
typedef struct
{
	const gchar         *key;
	AxisSettingType      type;
	const gchar         *label;
	const gchar         *help;
	guint                flags;
	const gchar         *default_value;
	gint64               minimum;
	gint64               maximum;
	gdouble              minimum_decimal;
	gdouble              maximum_decimal;
	const gchar *const  *choices;
} AxisSettingInfo;

/**
 * AxisSettingValidateFunc:
 * @text: a non-empty value of a string setting that passed the limits of
 *   its declaration
 * @reason: (out): on refusal, a sentence fragment ("must be an http or
 *   https address") that does not contain the value
 *
 * A further check for one string setting, run wherever the value is checked
 * (on any thread): it must be a pure function.
 *
 * Returns: %TRUE if @text is acceptable
 */
typedef gboolean (*AxisSettingValidateFunc)(
	const gchar  *text,
	gchar       **reason
);

/**
 * axis_setting_set_validator:
 * @key: the string setting's key
 * @func: (nullable): the check; %NULL removes it
 *
 * Registers the check for @key.  Idempotent; a module calls it when it
 * hands out its setting table (a table has a fixed layout, so the check
 * lives beside it).
 */
void
axis_setting_set_validator(
	const gchar             *key,
	AxisSettingValidateFunc  func
);

/**
 * axis_setting_set_choice_labels:
 * @key: the choice setting's key
 * @labels: (nullable): one label for each of the setting's choices, in the
 *   same order, ended by %NULL (static); %NULL removes them
 *
 * Registers the words the Settings page shows for the allowed values of
 * @key, in place of the stored values themselves.  Idempotent; a module
 * calls it where it calls axis_setting_set_validator().
 */
void
axis_setting_set_choice_labels(
	const gchar         *key,
	const gchar *const  *labels
);

/**
 * axis_setting_choice_label:
 * @info: a choice setting
 * @value: one of its allowed values
 *
 * Returns: the label registered for @value, or @value itself when none is
 *   (valid for the life of the program, or as long as @value)
 */
const gchar *
axis_setting_choice_label(
	const AxisSettingInfo  *info,
	const gchar            *value
);

G_END_DECLS

#endif /* AXIS_SETTING_H */
