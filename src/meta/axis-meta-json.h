/*
 * axis-meta-json.h - reading the fields of a provider's JSON answer into the
 * metadata model's cleaned values, for every provider's parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each reader takes what an answer says only in the form it should be in,
 * and leaves anything else out (%NULL or 0): a field missing, null, of the
 * wrong type, out of range or not clean text.  GLib only (and the GLib-only
 * JSON reader of probe/axis-probe-json.h), no I/O: the fuzz targets of the
 * providers' parsers read through it.
 */

#ifndef AXIS_META_JSON_H
#define AXIS_META_JSON_H

#include <glib.h>

#include "meta/axis-meta-provider.h"
#include "probe/axis-probe-json.h"

G_BEGIN_DECLS

/* Fails with %AXIS_META_ERROR_UNREADABLE: "<label>'s answer cannot be read:
 * <why>".  Returns: %FALSE */
gboolean
axis_meta_json_unreadable(
	const gchar  *label,
	GError      **error,
	const gchar  *why
);

/**
 * axis_meta_json_parse:
 * @label: the provider's name for people, for the message
 *
 * Returns: (transfer full) (nullable): the answer, when it is JSON whose
 *   root is an object; otherwise %NULL with %AXIS_META_ERROR_UNREADABLE
 */
AxisJsonDoc *
axis_meta_json_parse(
	const gchar  *label,
	const gchar  *data,
	gsize         length,
	GError      **error
);

/* A string member, cleaned (axis_meta_clean_text()) and cut to @max_chars. */
gchar *
axis_meta_json_text(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
);

/* A whole number member within [@min, @max], else 0.  Only a number counts:
 * a string that holds one is not. */
gint
axis_meta_json_int(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              min,
	gint64              max
);

/* "YYYY-MM-DD", alone or as the start of a date and time ("...T00:00:00Z"),
 * when it is a real date within the years believed; else %NULL. */
gchar *
axis_meta_json_date_of(const gchar *text);

/* A date member, as axis_meta_json_date_of(). */
gchar *
axis_meta_json_date(
	const AxisJsonNode *object,
	const gchar        *name
);

/* The year of a date "YYYY-...", when it is one believed; else 0. */
gint
axis_meta_json_year_of(const gchar *date);

G_END_DECLS

#endif /* AXIS_META_JSON_H */
