/*
 * axis-settings.c - the typed settings store
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-settings.h"
#include "core/axis-error.h"
#include "core/axis-setting-units.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#define KEY_MAX 64

/* One value of any type; the fields that fit the type are used. */
typedef struct
{
	gboolean  b;
	gint64    i;	/* integer, duration (seconds), byte size (bytes) */
	gdouble   d;
	gchar    *s;	/* string and choice */
	gchar   **list;
} Value;

typedef struct
{
	const AxisSettingInfo *info;
	gchar                 *module_id;
	Value                 *def;
	Value                 *stored;		/* (cache lock) %NULL: not stored */
	Value                 *effective;	/* (cache lock) restart settings */
	gboolean               stored_invalid;	/* (cache lock) a row exists we cannot use */
} Entry;

typedef struct
{
	guint                    id;
	gchar                   *key;	/* %NULL: every setting */
	AxisSettingsChangedFunc  func;
	gpointer                 user_data;
} Subscription;

struct _AxisSettings
{
	gint         ref_count;	/* atomic */
	GMainContext *context;	/* where notifications are delivered */
	GHashTable  *entries;	/* key -> Entry*; fixed once loaded */
	gboolean     loaded;
	AxisDbPool  *pool;	/* nullable */

	GMutex       write_lock;	/* one write at a time, database and cache */
	GRWLock      cache_lock;	/* the values in Entry */

	GMutex       sub_lock;
	GArray      *subs;	/* (sub_lock) of Subscription */
	guint        next_sub;	/* (sub_lock) */
};

/* ── Values ─────────────────────────────────────────────────────── */

static Value *
value_new(AxisSettingType type)
{
	Value *value = g_new0(Value, 1);

	if (type == AXIS_SETTING_STRING || type == AXIS_SETTING_CHOICE)
		value->s = g_strdup("");
	if (type == AXIS_SETTING_STRING_LIST)
		value->list = g_new0(gchar *, 1);

	return value;
}

static void
value_free(Value *value)
{
	if (value == NULL)
		return;

	g_free(value->s);
	g_strfreev(value->list);
	g_free(value);
}

static Value *
value_copy(const Value *value)
{
	Value *copy = g_new0(Value, 1);

	copy->b = value->b;
	copy->i = value->i;
	copy->d = value->d;
	copy->s = g_strdup(value->s);
	copy->list = g_strdupv(value->list);

	return copy;
}

static gboolean
value_equal(
	AxisSettingType type,
	const Value    *a,
	const Value    *b
){
	switch (type) {
	case AXIS_SETTING_BOOLEAN:
		return a->b == b->b;
	case AXIS_SETTING_INTEGER:
	case AXIS_SETTING_DURATION:
	case AXIS_SETTING_BYTE_SIZE:
		return a->i == b->i;
	case AXIS_SETTING_DECIMAL:
		return a->d == b->d;
	case AXIS_SETTING_STRING:
	case AXIS_SETTING_CHOICE:
		return g_strcmp0(a->s, b->s) == 0;
	case AXIS_SETTING_STRING_LIST:
		return g_strv_equal((const gchar *const *)a->list, (const gchar *const *)b->list);
	}

	return FALSE;
}

static gchar *
format_decimal(gdouble d)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	g_ascii_formatd(buffer, sizeof(buffer), "%.15g", d);
	if (g_ascii_strtod(buffer, NULL) != d)
		g_ascii_formatd(buffer, sizeof(buffer), "%.17g", d);

	return g_strdup(buffer);
}

/* A limit written for a message. */
static gchar *
format_limit(
	const AxisSettingInfo *info,
	gint64                 n
){
	switch (info->type) {
	case AXIS_SETTING_DURATION:
		return axis_setting_format_duration(n);
	case AXIS_SETTING_BYTE_SIZE:
		return axis_setting_format_byte_size(n);
	default:
		return g_strdup_printf("%" G_GINT64_FORMAT, n);
	}
}

/* The value as the Settings page edits it. */
static gchar *
value_format(
	const AxisSettingInfo *info,
	const Value           *value
){
	switch (info->type) {
	case AXIS_SETTING_BOOLEAN:
		return g_strdup(value->b ? "true" : "false");
	case AXIS_SETTING_INTEGER:
		return g_strdup_printf("%" G_GINT64_FORMAT, value->i);
	case AXIS_SETTING_DECIMAL:
		return format_decimal(value->d);
	case AXIS_SETTING_STRING:
	case AXIS_SETTING_CHOICE:
		return g_strdup(value->s);
	case AXIS_SETTING_STRING_LIST:
		return g_strjoinv("\n", value->list);
	case AXIS_SETTING_DURATION:
		return axis_setting_format_duration(value->i);
	case AXIS_SETTING_BYTE_SIZE:
		return axis_setting_format_byte_size(value->i);
	}

	return g_strdup("");
}

/* The value as the JSON kept in the table. */
static gchar *
value_to_json(
	const AxisSettingInfo *info,
	const Value           *value
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) root = NULL;
	guint i;

	switch (info->type) {
	case AXIS_SETTING_BOOLEAN:
		return g_strdup(value->b ? "true" : "false");
	case AXIS_SETTING_INTEGER:
	case AXIS_SETTING_DURATION:
	case AXIS_SETTING_BYTE_SIZE:
		return g_strdup_printf("%" G_GINT64_FORMAT, value->i);
	case AXIS_SETTING_DECIMAL:
		return format_decimal(value->d);
	default:
		break;
	}

	if (info->type == AXIS_SETTING_STRING_LIST) {
		builder = json_builder_new();
		json_builder_begin_array(builder);
		for (i = 0; value->list[i] != NULL; i++)
			json_builder_add_string_value(builder, value->list[i]);
		json_builder_end_array(builder);
		root = json_builder_get_root(builder);
	} else {
		root = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(root, value->s);
	}
	generator = json_generator_new();
	json_generator_set_root(generator, root);

	return json_generator_to_data(generator, NULL);
}

/* ── Checking ───────────────────────────────────────────────────── */

static GMutex validators_lock;
static GHashTable *validators;	/* (validators_lock) key -> AxisSettingValidateFunc */

void
axis_setting_set_validator(
	const gchar             *key,
	AxisSettingValidateFunc  func
){
	g_return_if_fail(key != NULL);

	g_mutex_lock(&validators_lock);
	if (validators == NULL)
		validators = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (func != NULL)
		g_hash_table_insert(validators, g_strdup(key), (gpointer)func);
	else
		g_hash_table_remove(validators, key);
	g_mutex_unlock(&validators_lock);
}

static GMutex choice_labels_lock;
static GHashTable *choice_labels;	/* (choice_labels_lock) key -> static NULL-ended label array */

void
axis_setting_set_choice_labels(
	const gchar         *key,
	const gchar *const  *labels
){
	g_return_if_fail(key != NULL);

	g_mutex_lock(&choice_labels_lock);
	if (choice_labels == NULL)
		choice_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (labels != NULL)
		g_hash_table_insert(choice_labels, g_strdup(key), (gpointer)labels);
	else
		g_hash_table_remove(choice_labels, key);
	g_mutex_unlock(&choice_labels_lock);
}

const gchar *
axis_setting_choice_label(
	const AxisSettingInfo  *info,
	const gchar            *value
){
	const gchar *const *labels = NULL;
	guint i;

	g_mutex_lock(&choice_labels_lock);
	if (choice_labels != NULL)
		labels = g_hash_table_lookup(choice_labels, info->key);
	g_mutex_unlock(&choice_labels_lock);

	for (i = 0; labels != NULL && info->choices != NULL && info->choices[i] != NULL; i++) {
		if (strcmp(info->choices[i], value) == 0)
			return labels[i] != NULL ? labels[i] : value;
	}

	return value;
}

static AxisSettingValidateFunc
lookup_validator(const gchar *key)
{
	AxisSettingValidateFunc func = NULL;

	g_mutex_lock(&validators_lock);
	if (validators != NULL)
		func = (AxisSettingValidateFunc)g_hash_table_lookup(validators, key);
	g_mutex_unlock(&validators_lock);
	return func;
}

static gboolean
text_is_clean(
	const gchar *text,
	gsize        length
){
	const gchar *p;

	if (!g_utf8_validate(text, (gssize)length, NULL))
		return FALSE;

	for (p = text; (gsize)(p - text) < length; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			return FALSE;
	}

	return TRUE;
}

/*
 * Whether @value satisfies the declaration's limits.  On failure *@reason
 * is a sentence fragment; it never contains the value.
 */
static gboolean
value_check(
	const AxisSettingInfo *info,
	const Value           *value,
	gchar                **reason
){
	guint i;

	switch (info->type) {
	case AXIS_SETTING_BOOLEAN:
		return TRUE;

	case AXIS_SETTING_INTEGER:
	case AXIS_SETTING_DURATION:
	case AXIS_SETTING_BYTE_SIZE:
		/* 0 switches it off, whatever the limits (AXIS_SETTING_ZERO_OFF). */
		if (value->i == 0 && (info->flags & AXIS_SETTING_ZERO_OFF))
			return TRUE;
		if (value->i < info->minimum || (info->type != AXIS_SETTING_INTEGER && value->i < 0)) {
			g_autofree gchar *limit = format_limit(info, MAX(info->minimum,
				info->type == AXIS_SETTING_INTEGER ? G_MININT64 : 0));

			*reason = g_strdup_printf((info->flags & AXIS_SETTING_ZERO_OFF) ? "must be 0 or at least %s" :
				"must be at least %s", limit);
			return FALSE;
		}
		if (info->maximum != 0 && value->i > info->maximum) {
			g_autofree gchar *limit = format_limit(info, info->maximum);

			*reason = g_strdup_printf("must be at most %s", limit);
			return FALSE;
		}
		return TRUE;

	case AXIS_SETTING_DECIMAL:
		if (!isfinite(value->d)) {
			*reason = g_strdup("must be a number");
			return FALSE;
		}
		if (info->minimum_decimal != 0 || info->maximum_decimal != 0) {
			if (value->d < info->minimum_decimal) {
				g_autofree gchar *limit = format_decimal(info->minimum_decimal);

				*reason = g_strdup_printf("must be at least %s", limit);
				return FALSE;
			}
			if (value->d > info->maximum_decimal) {
				g_autofree gchar *limit = format_decimal(info->maximum_decimal);

				*reason = g_strdup_printf("must be at most %s", limit);
				return FALSE;
			}
		}
		return TRUE;

	case AXIS_SETTING_STRING: {
		gsize length = strlen(value->s);
		gint64 maximum = info->maximum != 0 ? info->maximum : AXIS_SETTING_DEFAULT_MAX_STRING;

		if (!text_is_clean(value->s, length)) {
			*reason = g_strdup("must be plain text without control characters");
			return FALSE;
		}
		if ((gint64)length < info->minimum) {
			*reason = info->minimum <= 1 ?
				g_strdup("must not be empty") :
				g_strdup_printf("must be at least %" G_GINT64_FORMAT " bytes long", info->minimum);
			return FALSE;
		}
		if ((gint64)length > maximum) {
			*reason = g_strdup_printf("must be at most %" G_GINT64_FORMAT " bytes long", maximum);
			return FALSE;
		}
		if (length > 0) {
			AxisSettingValidateFunc validate = lookup_validator(info->key);

			if (validate != NULL && !validate(value->s, reason))
				return FALSE;
		}
		return TRUE;
	}

	case AXIS_SETTING_CHOICE:
		for (i = 0; info->choices != NULL && info->choices[i] != NULL; i++) {
			if (strcmp(info->choices[i], value->s) == 0)
				return TRUE;
		}
		{
			g_autofree gchar *allowed = g_strjoinv(", ", (gchar **)info->choices);

			*reason = g_strdup_printf("must be one of: %s", allowed);
		}
		return FALSE;

	case AXIS_SETTING_STRING_LIST: {
		guint count = g_strv_length(value->list);
		gint64 maximum = info->maximum != 0 ? info->maximum : AXIS_SETTING_DEFAULT_MAX_ITEMS;

		if ((gint64)count < info->minimum) {
			*reason = g_strdup_printf("must have at least %" G_GINT64_FORMAT " item%s",
				info->minimum, info->minimum == 1 ? "" : "s");
			return FALSE;
		}
		if ((gint64)count > maximum) {
			*reason = g_strdup_printf("must have at most %" G_GINT64_FORMAT " items", maximum);
			return FALSE;
		}
		for (i = 0; i < count; i++) {
			gsize length = strlen(value->list[i]);

			if (length == 0 || length > AXIS_SETTING_MAX_ITEM_BYTES ||
			    !text_is_clean(value->list[i], length)) {
				*reason = g_strdup_printf("each line must be plain text of 1 to %d bytes",
					AXIS_SETTING_MAX_ITEM_BYTES);
				return FALSE;
			}
		}
		return TRUE;
	}
	}

	return FALSE;
}

/* ── Parsing what a person typed ────────────────────────────────── */

static gboolean
parse_whole_number(
	const gchar *text,
	gint64      *n
){
	gboolean negative = FALSE;
	guint digits = 0;
	gint64 value = 0;

	if (*text == '-') {
		negative = TRUE;
		text++;
	}
	for (; *text >= '0' && *text <= '9'; text++) {
		if (++digits > 18)
			return FALSE;
		value = value * 10 + (*text - '0');
	}
	if (digits == 0 || *text != '\0')
		return FALSE;

	*n = negative ? -value : value;
	return TRUE;
}

/* -?digits[.digits][e[+-]digits], the way JSON writes a number. */
static gboolean
parse_decimal_number(
	const gchar *text,
	gdouble     *d
){
	const gchar *p = text;
	guint digits;

	if (strlen(text) > 40)
		return FALSE;
	if (*p == '-')
		p++;
	for (digits = 0; g_ascii_isdigit(*p); p++)
		digits++;
	if (digits == 0)
		return FALSE;
	if (*p == '.') {
		p++;
		for (digits = 0; g_ascii_isdigit(*p); p++)
			digits++;
		if (digits == 0)
			return FALSE;
	}
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-')
			p++;
		for (digits = 0; g_ascii_isdigit(*p); p++)
			digits++;
		if (digits == 0)
			return FALSE;
	}
	if (*p != '\0')
		return FALSE;

	*d = g_ascii_strtod(text, NULL);
	return isfinite(*d);
}

static gchar **
split_lines(const gchar *text)
{
	GPtrArray *items = g_ptr_array_new();
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = g_strstrip(lines[i]);

		if (line[0] != '\0')
			g_ptr_array_add(items, g_strdup(line));
	}
	g_ptr_array_add(items, NULL);

	return (gchar **)g_ptr_array_free(items, FALSE);
}

/*
 * Parses @text as a value of @info's type and checks its limits.  Returns
 * %NULL with *@reason set when it is not acceptable.
 */
static Value *
value_parse(
	const AxisSettingInfo *info,
	const gchar           *text,
	gchar                **reason
){
	Value *value = value_new(info->type);
	g_autofree gchar *copy = g_strdup(text != NULL ? text : "");
	gchar *trimmed;

	switch (info->type) {
	case AXIS_SETTING_BOOLEAN:
		trimmed = g_ascii_strdown(g_strstrip(copy), -1);
		if (strcmp(trimmed, "true") == 0 || strcmp(trimmed, "on") == 0 ||
		    strcmp(trimmed, "yes") == 0 || strcmp(trimmed, "1") == 0) {
			value->b = TRUE;
		} else if (strcmp(trimmed, "false") == 0 || strcmp(trimmed, "off") == 0 ||
		           strcmp(trimmed, "no") == 0 || strcmp(trimmed, "0") == 0) {
			value->b = FALSE;
		} else {
			*reason = g_strdup("must be true or false");
			g_free(trimmed);
			goto refused;
		}
		g_free(trimmed);
		break;

	case AXIS_SETTING_INTEGER:
		if (!parse_whole_number(g_strstrip(copy), &value->i)) {
			*reason = g_strdup("must be a whole number");
			goto refused;
		}
		break;

	case AXIS_SETTING_DECIMAL:
		if (!parse_decimal_number(g_strstrip(copy), &value->d)) {
			*reason = g_strdup("must be a number such as 2 or 0.75");
			goto refused;
		}
		break;

	case AXIS_SETTING_STRING:
	case AXIS_SETTING_CHOICE:
		g_free(value->s);
		value->s = g_strdup(g_strstrip(copy));
		break;

	case AXIS_SETTING_STRING_LIST:
		g_strfreev(value->list);
		value->list = split_lines(copy);
		break;

	case AXIS_SETTING_DURATION:
		if (!axis_setting_parse_duration(copy, &value->i)) {
			*reason = g_strdup("must be a length of time such as 90s, 15m, 2h, 7d or 1h 30m");
			goto refused;
		}
		break;

	case AXIS_SETTING_BYTE_SIZE:
		if (!axis_setting_parse_byte_size(copy, &value->i)) {
			*reason = g_strdup("must be a size such as 512MiB or 10GiB");
			goto refused;
		}
		break;
	}

	if (!value_check(info, value, reason))
		goto refused;

	return value;

refused:
	value_free(value);
	return NULL;
}

/* A value read from the table's JSON; %NULL if it has the wrong shape or
 * breaks the limits. */
static Value *
value_from_json(
	const AxisSettingInfo *info,
	JsonNode              *node
){
	Value *value = value_new(info->type);
	g_autofree gchar *reason = NULL;
	guint i;

	if (node == NULL)
		goto refused;

	switch (info->type) {
	case AXIS_SETTING_BOOLEAN:
		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
			goto refused;
		value->b = json_node_get_boolean(node);
		break;

	case AXIS_SETTING_INTEGER:
	case AXIS_SETTING_DURATION:
	case AXIS_SETTING_BYTE_SIZE:
		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
			goto refused;
		value->i = json_node_get_int(node);
		break;

	case AXIS_SETTING_DECIMAL:
		if (!JSON_NODE_HOLDS_VALUE(node))
			goto refused;
		if (json_node_get_value_type(node) == G_TYPE_INT64)
			value->d = (gdouble)json_node_get_int(node);
		else if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
			value->d = json_node_get_double(node);
		else
			goto refused;
		break;

	case AXIS_SETTING_STRING:
	case AXIS_SETTING_CHOICE:
		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
			goto refused;
		g_free(value->s);
		value->s = g_strdup(json_node_get_string(node));
		break;

	case AXIS_SETTING_STRING_LIST: {
		JsonArray *array;
		GPtrArray *items;

		if (!JSON_NODE_HOLDS_ARRAY(node))
			goto refused;
		array = json_node_get_array(node);
		items = g_ptr_array_new();
		for (i = 0; i < json_array_get_length(array); i++) {
			JsonNode *item = json_array_get_element(array, i);

			if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING) {
				g_ptr_array_add(items, NULL);
				g_strfreev((gchar **)g_ptr_array_free(items, FALSE));
				goto refused;
			}
			g_ptr_array_add(items, g_strdup(json_node_get_string(item)));
		}
		g_ptr_array_add(items, NULL);
		g_strfreev(value->list);
		value->list = (gchar **)g_ptr_array_free(items, FALSE);
		break;
	}
	}

	if (!value_check(info, value, &reason))
		goto refused;

	return value;

refused:
	value_free(value);
	return NULL;
}

/* ── Declarations ───────────────────────────────────────────────── */

static gboolean
key_is_valid(const gchar *key)
{
	const gchar *p;
	const gchar *dot;

	if (key == NULL || strlen(key) > KEY_MAX)
		return FALSE;

	dot = strchr(key, '.');
	if (dot == NULL || dot == key || dot[1] == '\0')
		return FALSE;

	if (!g_ascii_islower(key[0]) || !g_ascii_islower(dot[1]))
		return FALSE;
	for (p = key; p < dot; p++) {
		if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '-')
			return FALSE;
	}
	for (p = dot + 1; *p != '\0'; p++) {
		if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '_')
			return FALSE;
	}

	return TRUE;
}

static void
entry_free(gpointer data)
{
	Entry *entry = data;

	g_free(entry->module_id);
	value_free(entry->def);
	value_free(entry->stored);
	value_free(entry->effective);
	g_free(entry);
}

/* Builds the entry for @info, or sets *@error. */
static Entry *
entry_declare(
	const gchar           *module_id,
	const AxisSettingInfo *info,
	GError               **error
){
	g_autofree gchar *reason = NULL;
	Value *def;
	Entry *entry;

	if (!key_is_valid(info->key)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"module '%s': '%s' is not a valid setting key (use <module>.<name>, "
			"lower-case, at most %d characters)", module_id,
			info->key != NULL ? info->key : "(null)", KEY_MAX);
		return NULL;
	}
	if (info->label == NULL || info->label[0] == '\0') {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' has no label", info->key);
		return NULL;
	}
	if (info->type < AXIS_SETTING_BOOLEAN || info->type > AXIS_SETTING_BYTE_SIZE) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' has an unknown type", info->key);
		return NULL;
	}
	if ((info->flags & AXIS_SETTING_SECRET) &&
	    (info->type != AXIS_SETTING_STRING || info->default_value != NULL)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' is secret, so it must be a string without a default", info->key);
		return NULL;
	}
	if ((info->flags & AXIS_SETTING_ZERO_OFF) && info->type != AXIS_SETTING_INTEGER &&
	    info->type != AXIS_SETTING_DURATION && info->type != AXIS_SETTING_BYTE_SIZE) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' allows 0 to switch it off, so it must be a number, a duration or a size",
			info->key);
		return NULL;
	}
	if (info->type == AXIS_SETTING_CHOICE &&
	    (info->choices == NULL || info->choices[0] == NULL)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' is a choice without choices", info->key);
		return NULL;
	}
	if ((info->type == AXIS_SETTING_DURATION || info->type == AXIS_SETTING_BYTE_SIZE) &&
	    info->minimum < 0) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' has a negative minimum", info->key);
		return NULL;
	}
	if (info->type != AXIS_SETTING_BOOLEAN && info->type != AXIS_SETTING_DECIMAL &&
	    info->type != AXIS_SETTING_CHOICE && info->maximum != 0 && info->maximum < info->minimum) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' has a maximum below its minimum", info->key);
		return NULL;
	}
	if (info->type == AXIS_SETTING_DECIMAL &&
	    (info->minimum_decimal != 0 || info->maximum_decimal != 0) &&
	    info->maximum_decimal < info->minimum_decimal) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s' has a maximum below its minimum", info->key);
		return NULL;
	}

	if (info->default_value != NULL) {
		def = value_parse(info, info->default_value, &reason);
	} else {
		def = value_new(info->type);
		if (!value_check(info, def, &reason)) {
			value_free(def);
			def = NULL;
		}
	}
	if (def == NULL) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"setting '%s': the default %s", info->key, reason);
		return NULL;
	}

	entry = g_new0(Entry, 1);
	entry->info = info;
	entry->module_id = g_strdup(module_id);
	entry->def = def;
	entry->effective = value_copy(def);

	return entry;
}

AxisSettings *
axis_settings_new(void)
{
	AxisSettings *self = g_new0(AxisSettings, 1);

	self->ref_count = 1;
	self->context = g_main_context_ref_thread_default();
	self->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
	g_mutex_init(&self->write_lock);
	g_rw_lock_init(&self->cache_lock);
	g_mutex_init(&self->sub_lock);
	self->subs = g_array_new(FALSE, FALSE, sizeof(Subscription));

	return self;
}

AxisSettings *
axis_settings_ref(AxisSettings *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_settings_unref(AxisSettings *self)
{
	guint i;

	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_hash_table_unref(self->entries);
	for (i = 0; i < self->subs->len; i++)
		g_free(g_array_index(self->subs, Subscription, i).key);
	g_array_free(self->subs, TRUE);
	g_main_context_unref(self->context);
	g_mutex_clear(&self->write_lock);
	g_rw_lock_clear(&self->cache_lock);
	g_mutex_clear(&self->sub_lock);
	g_free(self);
}

gboolean
axis_settings_add_table(
	AxisSettings          *self,
	const gchar           *module_id,
	const AxisSettingInfo *table,
	GError               **error
){
	g_autoptr(GPtrArray) built = g_ptr_array_new_with_free_func(entry_free);
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(module_id != NULL, FALSE);

	if (self->loaded) {
		g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_SETTING,
			"settings cannot be declared after they have been loaded");
		return FALSE;
	}

	for (i = 0; table != NULL && table[i].key != NULL; i++) {
		Entry *entry = entry_declare(module_id, &table[i], error);
		guint j;

		if (entry == NULL)
			return FALSE;
		g_ptr_array_add(built, entry);

		if (g_hash_table_contains(self->entries, table[i].key)) {
			Entry *first = g_hash_table_lookup(self->entries, table[i].key);

			g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
				"setting '%s' is declared twice (by modules '%s' and '%s')",
				table[i].key, first->module_id, module_id);
			return FALSE;
		}
		for (j = 0; j + 1 < built->len; j++) {
			if (strcmp(((Entry *)g_ptr_array_index(built, j))->info->key, table[i].key) == 0) {
				g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING,
					"setting '%s' is declared twice (by module '%s')", table[i].key,
					module_id);
				return FALSE;
			}
		}
	}

	for (i = 0; i < built->len; i++) {
		Entry *entry = g_ptr_array_index(built, i);

		g_hash_table_insert(self->entries, g_strdup(entry->info->key), entry);
	}
	/* The table owns them now. */
	g_ptr_array_set_free_func(built, NULL);

	return TRUE;
}

gboolean
axis_settings_add_modules(
	AxisSettings        *self,
	AxisModuleRegistry  *modules,
	GError             **error
){
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(modules != NULL, FALSE);

	for (i = 0; i < axis_module_registry_get_count(modules); i++) {
		const AxisSettingInfo *table = axis_module_registry_get_settings(modules, i);
		g_autoptr(GError) local = NULL;

		if (table != NULL &&
		    !axis_settings_add_table(self, axis_module_registry_get_id(modules, i), table,
		                             &local)) {
			g_propagate_prefixed_error(error, g_steal_pointer(&local), "module %s: ",
				axis_module_registry_get_id(modules, i));
			return FALSE;
		}
	}

	return TRUE;
}

const AxisSettingInfo *
axis_settings_lookup(
	AxisSettings  *self,
	const gchar   *key,
	const gchar  **module_id
){
	Entry *entry;

	g_return_val_if_fail(self != NULL, NULL);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL)
		return NULL;

	if (module_id != NULL)
		*module_id = entry->module_id;
	return entry->info;
}

/* ── Loading ────────────────────────────────────────────────────── */

static Value *
entry_current(const Entry *entry)
{
	return entry->stored != NULL ? entry->stored : entry->def;
}

/* What the getters return. */
static const Value *
entry_in_effect(const Entry *entry)
{
	if (entry->info->flags & AXIS_SETTING_RESTART)
		return entry->effective;

	return entry_current(entry);
}

gboolean
axis_settings_load(
	AxisSettings  *self,
	AxisDbPool    *pool,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(!self->loaded, FALSE);

	if (pool != NULL &&
	    !axis_db_pool_exec(pool, "SELECT key, value::text FROM setting ORDER BY key",
	                       0, NULL, &result, error))
		return FALSE;

	for (i = 0; result != NULL && i < axis_db_result_get_n_rows(result); i++) {
		const gchar *key = axis_db_result_get_text(result, i, 0);
		const gchar *text = axis_db_result_get_text(result, i, 1);
		Entry *entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
		g_autoptr(JsonParser) parser = NULL;
		Value *parsed = NULL;

		if (entry == NULL) {
			g_warning("the settings table holds '%s', which no module declares; "
				"leaving it unchanged", key != NULL ? key : "(null)");
			continue;
		}

		parser = json_parser_new();
		if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL))
			parsed = value_from_json(entry->info, json_parser_get_root(parser));
		if (parsed == NULL) {
			g_warning("the stored value of setting '%s' is not valid; using the default",
				key);
			entry->stored_invalid = TRUE;
			continue;
		}
		entry->stored = parsed;
	}

	g_hash_table_iter_init(&iter, self->entries);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Entry *entry = value;

		value_free(entry->effective);
		entry->effective = value_copy(entry_current(entry));
	}

	self->pool = pool;
	self->loaded = TRUE;
	return TRUE;
}

/* ── Reading ────────────────────────────────────────────────────── */

/* The entry for @key if it is declared with a type that @type_a or
 * @type_b says; else a critical naming @caller. */
static Entry *
typed_entry(
	AxisSettings    *self,
	const gchar     *key,
	AxisSettingType  type_a,
	AxisSettingType  type_b,
	const gchar     *caller
){
	Entry *entry;

	g_return_val_if_fail(self != NULL, NULL);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL) {
		g_critical("%s: '%s' is not a declared setting", caller, key != NULL ? key : "(null)");
		return NULL;
	}
	if (entry->info->type != type_a && entry->info->type != type_b) {
		g_critical("%s: setting '%s' has another type", caller, key);
		return NULL;
	}

	return entry;
}

gboolean
axis_settings_get_boolean(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry = typed_entry(self, key, AXIS_SETTING_BOOLEAN, AXIS_SETTING_BOOLEAN, G_STRFUNC);
	gboolean result;

	if (entry == NULL)
		return FALSE;

	g_rw_lock_reader_lock(&self->cache_lock);
	result = entry_in_effect(entry)->b;
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

static gint64
get_integer(
	AxisSettings    *self,
	const gchar     *key,
	AxisSettingType  type,
	const gchar     *caller
){
	Entry *entry = typed_entry(self, key, type, type, caller);
	gint64 result;

	if (entry == NULL)
		return 0;

	g_rw_lock_reader_lock(&self->cache_lock);
	result = entry_in_effect(entry)->i;
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

gint64
axis_settings_get_integer(AxisSettings *self, const gchar *key)
{
	return get_integer(self, key, AXIS_SETTING_INTEGER, G_STRFUNC);
}

gint64
axis_settings_get_duration(AxisSettings *self, const gchar *key)
{
	return get_integer(self, key, AXIS_SETTING_DURATION, G_STRFUNC);
}

gint64
axis_settings_get_byte_size(AxisSettings *self, const gchar *key)
{
	return get_integer(self, key, AXIS_SETTING_BYTE_SIZE, G_STRFUNC);
}

gdouble
axis_settings_get_decimal(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry = typed_entry(self, key, AXIS_SETTING_DECIMAL, AXIS_SETTING_DECIMAL, G_STRFUNC);
	gdouble result;

	if (entry == NULL)
		return 0;

	g_rw_lock_reader_lock(&self->cache_lock);
	result = entry_in_effect(entry)->d;
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

gchar *
axis_settings_get_string(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry = typed_entry(self, key, AXIS_SETTING_STRING, AXIS_SETTING_CHOICE, G_STRFUNC);
	gchar *result;

	if (entry == NULL)
		return g_strdup("");

	g_rw_lock_reader_lock(&self->cache_lock);
	result = g_strdup(entry_in_effect(entry)->s);
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

gchar **
axis_settings_get_string_list(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry = typed_entry(self, key, AXIS_SETTING_STRING_LIST, AXIS_SETTING_STRING_LIST,
		G_STRFUNC);
	gchar **result;

	if (entry == NULL)
		return g_new0(gchar *, 1);

	g_rw_lock_reader_lock(&self->cache_lock);
	result = g_strdupv(entry_in_effect(entry)->list);
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

gchar *
axis_settings_format(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry;
	gchar *result;

	g_return_val_if_fail(self != NULL, NULL);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL || (entry->info->flags & AXIS_SETTING_SECRET))
		return NULL;

	g_rw_lock_reader_lock(&self->cache_lock);
	result = value_format(entry->info, entry_current(entry));
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

gchar *
axis_settings_format_default(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry;

	g_return_val_if_fail(self != NULL, NULL);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL || (entry->info->flags & AXIS_SETTING_SECRET))
		return NULL;

	return value_format(entry->info, entry->def);
}

gboolean
axis_settings_is_set(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry;
	gboolean result;

	g_return_val_if_fail(self != NULL, FALSE);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL)
		return FALSE;

	g_rw_lock_reader_lock(&self->cache_lock);
	result = entry->stored != NULL;
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

gboolean
axis_settings_restart_pending(
	AxisSettings *self,
	const gchar  *key
){
	Entry *entry;
	gboolean result;

	g_return_val_if_fail(self != NULL, FALSE);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL || !(entry->info->flags & AXIS_SETTING_RESTART))
		return FALSE;

	g_rw_lock_reader_lock(&self->cache_lock);
	result = !value_equal(entry->info->type, entry_current(entry), entry->effective);
	g_rw_lock_reader_unlock(&self->cache_lock);

	return result;
}

/* ── Notification ───────────────────────────────────────────────── */

guint
axis_settings_subscribe(
	AxisSettings            *self,
	const gchar             *key,
	AxisSettingsChangedFunc  func,
	gpointer                 user_data
){
	Subscription sub;

	g_return_val_if_fail(self != NULL, 0);
	g_return_val_if_fail(func != NULL, 0);

	g_mutex_lock(&self->sub_lock);
	sub.id = ++self->next_sub;
	sub.key = g_strdup(key);
	sub.func = func;
	sub.user_data = user_data;
	g_array_append_val(self->subs, sub);
	g_mutex_unlock(&self->sub_lock);

	return sub.id;
}

void
axis_settings_unsubscribe(
	AxisSettings *self,
	guint         subscription_id
){
	guint i;

	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->sub_lock);
	for (i = 0; i < self->subs->len; i++) {
		if (g_array_index(self->subs, Subscription, i).id == subscription_id) {
			g_free(g_array_index(self->subs, Subscription, i).key);
			g_array_remove_index(self->subs, i);
			break;
		}
	}
	g_mutex_unlock(&self->sub_lock);
}

typedef struct
{
	AxisSettings *settings;
	gchar        *key;
} Delivery;

static void
delivery_free(gpointer data)
{
	Delivery *delivery = data;

	g_free(delivery->key);
	axis_settings_unref(delivery->settings);
	g_free(delivery);
}

/* Main loop: calls every subscriber still subscribed. */
static gboolean
deliver(gpointer data)
{
	Delivery *delivery = data;
	AxisSettings *self = delivery->settings;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(guint));
	guint i;

	g_mutex_lock(&self->sub_lock);
	for (i = 0; i < self->subs->len; i++) {
		const Subscription *sub = &g_array_index(self->subs, Subscription, i);

		if (sub->key == NULL || strcmp(sub->key, delivery->key) == 0)
			g_array_append_val(ids, sub->id);
	}
	g_mutex_unlock(&self->sub_lock);

	for (i = 0; i < ids->len; i++) {
		guint id = g_array_index(ids, guint, i);
		Subscription sub;
		gboolean found = FALSE;
		guint j;

		memset(&sub, 0, sizeof(sub));

		/* Unsubscribed by an earlier subscriber: not called. */
		g_mutex_lock(&self->sub_lock);
		for (j = 0; j < self->subs->len; j++) {
			const Subscription *candidate = &g_array_index(self->subs, Subscription, j);

			if (candidate->id == id) {
				sub = *candidate;
				found = TRUE;
				break;
			}
		}
		g_mutex_unlock(&self->sub_lock);

		if (found)
			sub.func(self, delivery->key, sub.user_data);
	}

	return G_SOURCE_REMOVE;
}

static void
notify(
	AxisSettings *self,
	const gchar  *key
){
	Delivery *delivery = g_new0(Delivery, 1);
	GSource *source = g_idle_source_new();

	delivery->settings = axis_settings_ref(self);
	delivery->key = g_strdup(key);

	/* Always through the main context, never called directly, even from
	 * the main thread: a subscriber never runs inside the write. */
	g_source_set_name(source, "axis-settings-changed");
	g_source_set_callback(source, deliver, delivery, delivery_free);
	g_source_attach(source, self->context);
	g_source_unref(source);
}

/* ── Writing ────────────────────────────────────────────────────── */

void
axis_setting_problem_free(AxisSettingProblem *problem)
{
	if (problem == NULL)
		return;

	g_free(problem->key);
	g_free(problem->reason);
	g_free(problem->message);
	g_free(problem);
}

static AxisSettingProblem *
problem_new(
	const gchar *key,
	const gchar *label,
	const gchar *reason
){
	AxisSettingProblem *problem = g_new0(AxisSettingProblem, 1);

	problem->key = g_strdup(key);
	problem->reason = g_strdup(reason);
	problem->message = label != NULL ?
		g_strdup_printf("%s (%s) %s", label, key, reason) :
		g_strdup_printf("%s %s", key, reason);

	return problem;
}

gboolean
axis_settings_check(
	AxisSettings        *self,
	const gchar         *key,
	const gchar         *text,
	AxisSettingProblem **problem
){
	Entry *entry;
	g_autofree gchar *reason = NULL;
	Value *value;

	g_return_val_if_fail(self != NULL, FALSE);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL) {
		if (problem != NULL)
			*problem = problem_new(key != NULL ? key : "(null)", NULL, "is not a setting");
		return FALSE;
	}

	value = value_parse(entry->info, text, &reason);
	if (value == NULL) {
		if (problem != NULL)
			*problem = problem_new(key, entry->info->label, reason);
		return FALSE;
	}

	value_free(value);
	return TRUE;
}

/* "{"a","b"}" as a text[] parameter. */
static gchar *
text_array(
	const gchar *const *items,
	guint               n
){
	GString *text = g_string_new("{");
	guint i;

	for (i = 0; i < n; i++) {
		const gchar *p;

		g_string_append(text, i > 0 ? ",\"" : "\"");
		for (p = items[i]; *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(text, '\\');
			g_string_append_c(text, *p);
		}
		g_string_append_c(text, '"');
	}
	g_string_append_c(text, '}');

	return g_string_free(text, FALSE);
}

static const gchar upsert_sql[] =
	"INSERT INTO setting (key, value)"
	" SELECT t.k, t.v::jsonb FROM unnest($1::text[], $2::text[]) AS t (k, v)"
	" ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now()";

gboolean
axis_settings_set_many(
	AxisSettings             *self,
	const AxisSettingChange  *changes,
	guint                     n,
	GStrv                    *changed,
	GPtrArray               **problems,
	GError                  **error
){
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_setting_problem_free);
	Entry **entries;
	Value **values;
	gboolean *write;
	guint writes = 0;
	gboolean secret = FALSE;
	gboolean ok = FALSE;
	guint i;
	guint j;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(n == 0 || changes != NULL, FALSE);

	if (changed != NULL)
		*changed = NULL;
	entries = g_new0(Entry *, n + 1);
	values = g_new0(Value *, n + 1);
	write = g_new0(gboolean, n + 1);

	g_mutex_lock(&self->write_lock);

	for (i = 0; i < n; i++) {
		g_autofree gchar *reason = NULL;

		entries[i] = changes[i].key != NULL ? g_hash_table_lookup(self->entries, changes[i].key) : NULL;
		if (entries[i] == NULL) {
			g_ptr_array_add(found, problem_new(changes[i].key != NULL ? changes[i].key : "(null)",
				NULL, "is not a setting"));
			continue;
		}
		for (j = 0; j < i; j++) {
			if (entries[j] == entries[i])
				break;
		}
		if (j < i) {
			g_ptr_array_add(found, problem_new(changes[i].key, entries[i]->info->label,
				"was given twice"));
			continue;
		}

		values[i] = value_parse(entries[i]->info, changes[i].text, &reason);
		if (values[i] == NULL)
			g_ptr_array_add(found, problem_new(changes[i].key, entries[i]->info->label, reason));
	}

	if (found->len > 0) {
		const AxisSettingProblem *first = g_ptr_array_index(found, 0);

		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING, "the settings were not saved: %s",
			first->message);
		if (problems != NULL)
			*problems = g_steal_pointer(&found);
		goto out;
	}

	g_rw_lock_reader_lock(&self->cache_lock);
	for (i = 0; i < n; i++) {
		write[i] = entries[i]->stored_invalid ||
		           !value_equal(entries[i]->info->type, values[i], entry_current(entries[i]));
		if (write[i]) {
			writes++;
			if (entries[i]->info->flags & AXIS_SETTING_SECRET)
				secret = TRUE;
		}
	}
	g_rw_lock_reader_unlock(&self->cache_lock);

	if (writes == 0) {
		ok = TRUE;
		goto out;
	}

	if (self->pool != NULL) {
		g_autoptr(GError) local = NULL;
		g_autofree const gchar **keys = g_new0(const gchar *, writes + 1);
		g_autofree gchar **json = g_new0(gchar *, writes + 1);
		g_autofree gchar *key_array = NULL;
		g_autofree gchar *value_array = NULL;
		const gchar *params[2];
		guint k = 0;

		for (i = 0; i < n; i++) {
			if (!write[i])
				continue;
			keys[k] = entries[i]->info->key;
			json[k] = value_to_json(entries[i]->info, values[i]);
			k++;
		}
		key_array = text_array(keys, writes);
		value_array = text_array((const gchar *const *)json, writes);
		params[0] = key_array;
		params[1] = value_array;

		ok = axis_db_pool_exec(self->pool, upsert_sql, 2, params, NULL, &local);
		for (k = 0; k < writes; k++)
			g_free(json[k]);
		if (!ok) {
			/* The server's message could quote a value. */
			if (secret)
				g_warning("could not store the settings in the database");
			else
				g_warning("could not store the settings in the database: %s", local->message);
			g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_FAILED,
				"the settings could not be stored in the database");
			goto out;
		}
	}

	g_rw_lock_writer_lock(&self->cache_lock);
	for (i = 0; i < n; i++) {
		if (!write[i])
			continue;
		value_free(entries[i]->stored);
		entries[i]->stored = g_steal_pointer(&values[i]);
		entries[i]->stored_invalid = FALSE;
	}
	g_rw_lock_writer_unlock(&self->cache_lock);
	ok = TRUE;

out:
	g_mutex_unlock(&self->write_lock);

	if (ok) {
		GPtrArray *keys = g_ptr_array_new();

		for (i = 0; i < n; i++) {
			if (!write[i])
				continue;
			g_ptr_array_add(keys, g_strdup(entries[i]->info->key));
			if (!(entries[i]->info->flags & AXIS_SETTING_RESTART))
				notify(self, entries[i]->info->key);
		}
		g_ptr_array_add(keys, NULL);
		if (changed != NULL)
			*changed = (GStrv)g_ptr_array_free(keys, FALSE);
		else
			g_strfreev((GStrv)g_ptr_array_free(keys, FALSE));
	}

	for (i = 0; i < n; i++)
		value_free(values[i]);
	g_free(values);
	g_free(entries);
	g_free(write);

	return ok;
}

gboolean
axis_settings_set(
	AxisSettings  *self,
	const gchar   *key,
	const gchar   *text,
	GError       **error
){
	AxisSettingChange change;

	change.key = key;
	change.text = text;
	return axis_settings_set_many(self, &change, 1, NULL, NULL, error);
}

gboolean
axis_settings_clear(
	AxisSettings  *self,
	const gchar   *key,
	GError       **error
){
	Entry *entry;
	gboolean had;

	g_return_val_if_fail(self != NULL, FALSE);

	entry = key != NULL ? g_hash_table_lookup(self->entries, key) : NULL;
	if (entry == NULL) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_SETTING, "%s is not a setting",
			key != NULL ? key : "(null)");
		return FALSE;
	}

	g_mutex_lock(&self->write_lock);

	if (self->pool != NULL) {
		g_autoptr(GError) local = NULL;
		const gchar *params[1];

		params[0] = key;
		if (!axis_db_pool_exec(self->pool, "DELETE FROM setting WHERE key = $1", 1, params,
		                       NULL, &local)) {
			g_warning("could not clear setting '%s' in the database: %s", key, local->message);
			g_mutex_unlock(&self->write_lock);
			g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_FAILED,
				"the setting could not be cleared in the database");
			return FALSE;
		}
	}

	g_rw_lock_writer_lock(&self->cache_lock);
	had = entry->stored != NULL || entry->stored_invalid;
	value_free(entry->stored);
	entry->stored = NULL;
	entry->stored_invalid = FALSE;
	g_rw_lock_writer_unlock(&self->cache_lock);

	g_mutex_unlock(&self->write_lock);

	if (had && !(entry->info->flags & AXIS_SETTING_RESTART))
		notify(self, key);
	return TRUE;
}
