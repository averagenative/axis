/*
 * axis-size.c - sizes in words: the one way the interface writes them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-size.h"

#include <string.h>

#define MAX_UNIT 6
#define MAX_DECIMALS 6

/* The number and its unit are joined by a no-break space (U+00A0), as
 * g_format_size() does, so that a sentence never ends a line between them
 * ("bytes" is a word, which g_format_size() set after a plain space, and so
 * does this). */
#define NBSP "\302\240"

static const gchar *const unit_names[MAX_UNIT + 1] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };

/* @bytes divided by 1024^@unit. */
static gdouble
scaled(
	guint64 bytes,
	guint   unit
){
	gdouble value = (gdouble)bytes;
	guint i;

	for (i = 0; i < unit; i++)
		value /= 1024.0;

	return value;
}

/* "%.<decimals>f" of @value with the zeros at the end dropped, and the
 * point with them when nothing is left after it. */
static gchar *
number_text(
	gdouble value,
	guint   decimals
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	gchar format[8];
	gsize length;

	g_snprintf(format, sizeof(format), "%%.%uf", MIN(decimals, MAX_DECIMALS));
	g_ascii_formatd(buffer, sizeof(buffer), format, value);
	if (strchr(buffer, '.') != NULL) {
		length = strlen(buffer);
		while (length > 0 && buffer[length - 1] == '0')
			buffer[--length] = '\0';
		if (length > 0 && buffer[length - 1] == '.')
			buffer[length - 1] = '\0';
	}

	return g_strdup(buffer);
}

guint
axis_size_unit_of(guint64 bytes)
{
	guint unit = 0;

	while (unit < MAX_UNIT && scaled(bytes, unit) >= 1024.0)
		unit++;
	/* 1023.96 KiB is written "1024 KiB" at one decimal: it is 1 MiB. */
	if (unit < MAX_UNIT && unit > 0 && scaled(bytes, unit) >= 1023.95)
		unit++;

	return unit;
}

gchar *
axis_size_number_as(
	guint64 bytes,
	guint   unit,
	guint   decimals
){
	unit = MIN(unit, MAX_UNIT);
	if (unit == 0)
		return g_strdup_printf("%" G_GUINT64_FORMAT, bytes);

	return number_text(scaled(bytes, unit), decimals);
}

gchar *
axis_size_format_as(
	guint64 bytes,
	guint   unit,
	guint   decimals
){
	g_autofree gchar *number = NULL;

	unit = MIN(unit, MAX_UNIT);
	if (unit == 0)
		return g_strdup_printf("%" G_GUINT64_FORMAT " %s", bytes, bytes == 1 ? "byte" : "bytes");
	number = number_text(scaled(bytes, unit), decimals);

	return g_strdup_printf("%s" NBSP "%s", number, unit_names[unit]);
}

gchar *
axis_size_format(guint64 bytes)
{
	return axis_size_format_as(bytes, axis_size_unit_of(bytes), 1);
}
