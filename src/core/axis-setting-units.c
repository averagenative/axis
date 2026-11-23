/*
 * axis-setting-units.c - durations and byte sizes as people type them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-setting-units.h"

#include <string.h>

/* More digits than this cannot fit in 63 bits. */
#define MAX_DIGITS 18

static const gchar *
skip_blanks(const gchar *p)
{
	while (*p == ' ' || *p == '\t')
		p++;

	return p;
}

/* Reads 1 to MAX_DIGITS digits at *@p; moves *@p past them. */
static gboolean
read_number(
	const gchar **p,
	gint64       *value
){
	const gchar *q = *p;
	gint64 n = 0;
	guint digits = 0;

	while (*q >= '0' && *q <= '9') {
		if (++digits > MAX_DIGITS)
			return FALSE;
		n = n * 10 + (*q - '0');
		q++;
	}
	if (digits == 0)
		return FALSE;

	*value = n;
	*p = q;
	return TRUE;
}

/* @total += @n * @unit, unless that overflows. */
static gboolean
add_scaled(
	gint64 *total,
	gint64  n,
	gint64  unit
){
	if (n > G_MAXINT64 / unit)
		return FALSE;
	if (n * unit > G_MAXINT64 - *total)
		return FALSE;

	*total += n * unit;
	return TRUE;
}

/* ── Durations ──────────────────────────────────────────────────── */

static gint64
duration_unit(gchar c)
{
	switch (g_ascii_tolower(c)) {
	case 's': return 1;
	case 'm': return 60;
	case 'h': return 3600;
	case 'd': return 86400;
	case 'w': return 7 * 86400;
	default:  return 0;
	}
}

gboolean
axis_setting_parse_duration(
	const gchar *text,
	gint64      *seconds
){
	const gchar *p;
	gint64 total = 0;
	guint seen = 0;
	guint components = 0;

	if (text == NULL)
		return FALSE;

	p = skip_blanks(text);
	for (;;) {
		gint64 n;
		gint64 unit;
		guint bit;

		if (!read_number(&p, &n))
			return FALSE;

		unit = duration_unit(*p);
		if (unit == 0) {
			/* A bare number is seconds, but only on its own. */
			if (components != 0 || *skip_blanks(p) != '\0')
				return FALSE;
			unit = 1;
		} else {
			bit = 1u << (g_ascii_tolower(*p) - 'a');
			if (seen & bit)
				return FALSE;
			seen |= bit;
			p++;
		}

		if (!add_scaled(&total, n, unit))
			return FALSE;
		components++;

		p = skip_blanks(p);
		if (*p == '\0')
			break;
	}

	if (seconds != NULL)
		*seconds = total;
	return TRUE;
}

gchar *
axis_setting_format_duration(gint64 seconds)
{
	static const struct
	{
		gint64 unit;
		gchar  letter;
	} units[] = {
		{ 86400, 'd' }, { 3600, 'h' }, { 60, 'm' }, { 1, 's' }
	};
	GString *text;
	guint i;

	if (seconds <= 0)
		return g_strdup("0s");

	text = g_string_new(NULL);
	for (i = 0; i < G_N_ELEMENTS(units); i++) {
		gint64 n = seconds / units[i].unit;

		if (n == 0)
			continue;
		seconds -= n * units[i].unit;
		g_string_append_printf(text, "%s%" G_GINT64_FORMAT "%c", text->len > 0 ? " " : "",
			n, units[i].letter);
	}

	return g_string_free(text, FALSE);
}

/* ── Byte sizes ─────────────────────────────────────────────────── */

/* The power of 1024 a unit name stands for, or -1. */
static gint
size_shift(const gchar *name)
{
	static const gchar letters[] = "bkmgtp";
	gchar lower[4];
	guint length = (guint)strlen(name);
	gchar *found;
	guint i;

	if (length == 0)
		return 0;
	if (length > 3)
		return -1;

	for (i = 0; i < length; i++)
		lower[i] = g_ascii_tolower(name[i]);
	lower[length] = '\0';

	found = strchr(letters, lower[0]);
	if (found == NULL)
		return -1;
	/* "b" alone; else the letter alone, with "b" or with "ib". */
	if (lower[0] == 'b')
		return length == 1 ? 0 : -1;
	if (length == 2 && lower[1] != 'b')
		return -1;
	if (length == 3 && (lower[1] != 'i' || lower[2] != 'b'))
		return -1;

	return (gint)(found - letters);
}

gboolean
axis_setting_parse_byte_size(
	const gchar *text,
	gint64      *bytes
){
	const gchar *p;
	gchar name[4];
	guint length = 0;
	gint64 n;
	gint shift;
	gint64 total = 0;

	if (text == NULL)
		return FALSE;

	p = skip_blanks(text);
	if (!read_number(&p, &n))
		return FALSE;
	p = skip_blanks(p);

	while (g_ascii_isalpha(*p)) {
		if (length >= 3)
			return FALSE;
		name[length++] = *p++;
	}
	name[length] = '\0';
	if (*skip_blanks(p) != '\0')
		return FALSE;

	shift = size_shift(name);
	if (shift < 0)
		return FALSE;
	if (!add_scaled(&total, n, G_GINT64_CONSTANT(1) << (10 * shift)))
		return FALSE;

	if (bytes != NULL)
		*bytes = total;
	return TRUE;
}

gchar *
axis_setting_format_byte_size(gint64 bytes)
{
	static const gchar *const names[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	gint shift;

	if (bytes <= 0)
		return g_strdup("0B");

	for (shift = 5; shift > 0; shift--) {
		if (bytes % (G_GINT64_CONSTANT(1) << (10 * shift)) == 0)
			break;
	}

	return g_strdup_printf("%" G_GINT64_FORMAT "%s", bytes >> (10 * shift), names[shift]);
}
