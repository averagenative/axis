/*
 * axis-portal-form.c - how axis-portal reads what a browser sends: a path, a
 * query, a posted form
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-portal-form.h"

#include <string.h>

#include "api/axis-portal-proto.h"

/* ── Paths ──────────────────────────────────────────────────────── */

static const gchar *const patterns[] = {
	"/", "/login", "/logout", "/search", "/title/<id>", "/title/<id>/request", "/requests",
	"/requests/<id>/withdraw", "/art/<id>", AXIS_PORTAL_STYLE_PATH, "/healthz", NULL
};

const gchar *const *
axis_portal_route_patterns(void)
{
	return patterns;
}

/* The title's number at @text, which ends at @end. */
static gboolean
title_number(
	const gchar *text,
	const gchar *end,
	gint64      *id
){
	g_autofree gchar *digits = g_strndup(text, (gsize)(end - text));

	return axis_portal_form_number(digits, 1, AXIS_PORTAL_MAX_ID, id);
}

AxisPortalRoute
axis_portal_route_parse(
	const gchar  *path,
	gint64       *id,
	gchar       **request_id
){
	static const struct
	{
		const gchar     *path;
		AxisPortalRoute  route;
	} fixed[] = {
		{ "/", AXIS_PORTAL_ROUTE_HOME },
		{ "/login", AXIS_PORTAL_ROUTE_LOGIN },
		{ "/logout", AXIS_PORTAL_ROUTE_LOGOUT },
		{ "/search", AXIS_PORTAL_ROUTE_SEARCH },
		{ "/requests", AXIS_PORTAL_ROUTE_REQUESTS },
		{ AXIS_PORTAL_STYLE_PATH, AXIS_PORTAL_ROUTE_STYLE },
		{ "/healthz", AXIS_PORTAL_ROUTE_HEALTH }
	};
	const gchar *rest;
	const gchar *slash;
	gsize length;
	guint i;

	*id = 0;
	*request_id = NULL;
	if (path == NULL)
		return AXIS_PORTAL_ROUTE_NONE;
	length = strlen(path);
	if (length == 0 || length > AXIS_PORTAL_PATH_MAX)
		return AXIS_PORTAL_ROUTE_NONE;
	for (i = 0; i < G_N_ELEMENTS(fixed); i++) {
		if (strcmp(path, fixed[i].path) == 0)
			return fixed[i].route;
	}

	if (g_str_has_prefix(path, "/art/")) {
		rest = path + strlen("/art/");
		return title_number(rest, rest + strlen(rest), id) ? AXIS_PORTAL_ROUTE_ART : AXIS_PORTAL_ROUTE_NONE;
	}
	if (g_str_has_prefix(path, "/title/")) {
		rest = path + strlen("/title/");
		slash = strchr(rest, '/');
		if (slash == NULL)
			return title_number(rest, rest + strlen(rest), id) ? AXIS_PORTAL_ROUTE_TITLE : AXIS_PORTAL_ROUTE_NONE;
		if (strcmp(slash, "/request") == 0 && title_number(rest, slash, id))
			return AXIS_PORTAL_ROUTE_ASK;
		*id = 0;
		return AXIS_PORTAL_ROUTE_NONE;
	}
	if (g_str_has_prefix(path, "/requests/")) {
		g_autofree gchar *candidate = NULL;

		rest = path + strlen("/requests/");
		slash = strchr(rest, '/');
		if (slash == NULL || strcmp(slash, "/withdraw") != 0)
			return AXIS_PORTAL_ROUTE_NONE;
		candidate = g_strndup(rest, (gsize)(slash - rest));
		if (!axis_portal_wire_is_request_id(candidate))
			return AXIS_PORTAL_ROUTE_NONE;
		*request_id = g_steal_pointer(&candidate);
		return AXIS_PORTAL_ROUTE_WITHDRAW;
	}
	return AXIS_PORTAL_ROUTE_NONE;
}

/* ── Forms ──────────────────────────────────────────────────────── */

typedef struct
{
	gchar *name;
	gchar *value;
} Field;

struct _AxisPortalForm
{
	GArray *fields;		/* Field */
};

static void
wipe(gchar *text)
{
	volatile gchar *p = text;

	if (text == NULL)
		return;
	while (*p != '\0')
		*p++ = '\0';
	g_free(text);
}

void
axis_portal_form_free(AxisPortalForm *form)
{
	guint i;

	if (form == NULL)
		return;
	for (i = 0; i < form->fields->len; i++) {
		Field *field = &g_array_index(form->fields, Field, i);

		g_free(field->name);
		wipe(field->value);
	}
	g_array_unref(form->fields);
	g_free(form);
}

/* Decodes the @length bytes at @data: '+' is a space, "%XX" a byte.  %NULL
 * for a '%' that is not followed by two hexadecimal digits, for a NUL, for
 * a result longer than @max_bytes or one that is not valid UTF-8. */
static gchar *
decode(
	const gchar *data,
	gsize        length,
	gsize        max_bytes
){
	GString *out = g_string_sized_new(length);
	gsize i;

	for (i = 0; i < length; i++) {
		guchar c = (guchar)data[i];

		if (c == '+') {
			c = ' ';
		} else if (c == '%') {
			if (i + 2 >= length || !g_ascii_isxdigit(data[i + 1]) || !g_ascii_isxdigit(data[i + 2]))
				goto refuse;
			c = (guchar)(g_ascii_xdigit_value(data[i + 1]) * 16 + g_ascii_xdigit_value(data[i + 2]));
			i += 2;
		}
		if (c == '\0' || out->len >= max_bytes)
			goto refuse;
		g_string_append_c(out, (gchar)c);
	}
	if (!g_utf8_validate(out->str, (gssize)out->len, NULL))
		goto refuse;
	return g_string_free(out, FALSE);

refuse:
	wipe(g_string_free(out, FALSE));
	return NULL;
}

static gboolean
name_ok(const gchar *name)
{
	gsize length = strlen(name);
	gsize i;

	if (length == 0 || length > AXIS_PORTAL_FORM_NAME_MAX)
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_islower(name[i]) && name[i] != '_')
			return FALSE;
	}
	return TRUE;
}

AxisPortalForm *
axis_portal_form_parse(
	const gchar *data,
	gsize        length,
	gsize        max_bytes
){
	g_autoptr(AxisPortalForm) form = g_new0(AxisPortalForm, 1);
	gsize start = 0;

	form->fields = g_array_new(FALSE, TRUE, sizeof(Field));
	if (data == NULL || length == 0)
		return g_steal_pointer(&form);
	if (length > max_bytes)
		return NULL;

	while (start < length) {
		const gchar *pair = data + start;
		const gchar *amp = memchr(pair, '&', length - start);
		gsize pair_len = amp != NULL ? (gsize)(amp - pair) : length - start;
		const gchar *equals = memchr(pair, '=', pair_len);
		Field field;

		/* Every pair has a name and an '='; an empty pair is not one. */
		if (equals == NULL || form->fields->len >= AXIS_PORTAL_FORM_MAX_FIELDS)
			return NULL;
		field.name = decode(pair, (gsize)(equals - pair), AXIS_PORTAL_FORM_NAME_MAX);
		if (field.name == NULL || !name_ok(field.name)) {
			g_free(field.name);
			return NULL;
		}
		field.value = decode(equals + 1, pair_len - (gsize)(equals - pair) - 1, AXIS_PORTAL_FORM_VALUE_MAX);
		if (field.value == NULL) {
			g_free(field.name);
			return NULL;
		}
		g_array_append_val(form->fields, field);
		start += pair_len + 1;
		/* Nothing after a last '&'. */
		if (amp != NULL && start >= length)
			return NULL;
	}
	return g_steal_pointer(&form);
}

guint
axis_portal_form_count(
	const AxisPortalForm *form,
	const gchar          *name
){
	guint count = 0;
	guint i;

	if (form == NULL || name == NULL)
		return 0;
	for (i = 0; i < form->fields->len; i++) {
		if (strcmp(g_array_index(form->fields, Field, i).name, name) == 0)
			count++;
	}
	return count;
}

const gchar *
axis_portal_form_get(
	const AxisPortalForm *form,
	const gchar          *name
){
	const gchar *found = NULL;
	guint i;

	if (form == NULL || name == NULL)
		return NULL;
	for (i = 0; i < form->fields->len; i++) {
		const Field *field = &g_array_index(form->fields, Field, i);

		if (strcmp(field->name, name) != 0)
			continue;
		/* Twice is no value. */
		if (found != NULL)
			return NULL;
		found = field->value;
	}
	return found;
}

gboolean
axis_portal_form_only(
	const AxisPortalForm *form,
	const gchar *const   *names
){
	guint i;

	if (form == NULL)
		return FALSE;
	for (i = 0; i < form->fields->len; i++) {
		if (!g_strv_contains(names, g_array_index(form->fields, Field, i).name))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_portal_form_number(
	const gchar *text,
	gint64       minimum,
	gint64       maximum,
	gint64      *value
){
	gint64 v = 0;
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	/* At most 18 digits: no overflow. */
	if (length == 0 || length > 18 || (length > 1 && text[0] == '0'))
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
		v = v * 10 + (text[i] - '0');
	}
	if (v < minimum || v > maximum)
		return FALSE;
	*value = v;
	return TRUE;
}

gboolean
axis_portal_form_kind(
	const gchar    *text,
	AxisPortalKind *kind
){
	if (g_strcmp0(text, "movie") == 0)
		*kind = AXIS_PORTAL_KIND_IS_MOVIE;
	else if (g_strcmp0(text, "series") == 0)
		*kind = AXIS_PORTAL_KIND_IS_SERIES;
	else
		return FALSE;
	return TRUE;
}

gboolean
axis_portal_form_seasons(
	const AxisPortalForm *form,
	AxisPortalAsk        *ask,
	gint                 *picked,
	guint                *n_picked
){
	const gchar *what = axis_portal_form_get(form, "what");
	guint i;
	guint j;

	*n_picked = 0;
	if (g_strcmp0(what, "all") == 0) {
		*ask = AXIS_PORTAL_ASK_ALL;
		return TRUE;
	}
	if (g_strcmp0(what, "first") == 0) {
		*ask = AXIS_PORTAL_ASK_FIRST;
		return TRUE;
	}
	if (g_strcmp0(what, "latest") == 0) {
		*ask = AXIS_PORTAL_ASK_LATEST;
		return TRUE;
	}
	if (g_strcmp0(what, "picked") != 0)
		return FALSE;
	*ask = AXIS_PORTAL_ASK_PICKED;
	for (i = 0; i < form->fields->len; i++) {
		const Field *field = &g_array_index(form->fields, Field, i);
		gint64 number = 0;

		if (strcmp(field->name, "season") != 0)
			continue;
		if (*n_picked >= AXIS_PORTAL_MAX_SEASONS ||
		    !axis_portal_form_number(field->value, 0, AXIS_PORTAL_MAX_SEASON_NUMBER, &number))
			return FALSE;
		for (j = 0; j < *n_picked; j++) {
			if (picked[j] == (gint)number)
				return FALSE;
		}
		picked[(*n_picked)++] = (gint)number;
	}
	return *n_picked > 0;
}
