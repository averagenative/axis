/*
 * axis-volume-marker-parse.c - the marker's one accepted form
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GLib only, so that the fuzz target can be built from this file alone.
 * The canonical marker has a fixed length, so parsing is a length check,
 * byte comparisons of the fixed parts and a check of each character of the
 * two variable fields: constant time and memory whatever is on the disk.
 */

#include "storage/axis-volume-marker.h"

#include <string.h>

G_DEFINE_QUARK(axis-volume-marker-error-quark, axis_volume_marker_error)

#define LINE_MAGIC   "axis-volume 1\n"
#define MAGIC_PREFIX "axis-volume "
#define UUID_PREFIX  "uuid "
#define TIME_PREFIX  "created "
#define NOTE_LINE    "note " AXIS_VOLUME_MARKER_NOTE "\n"
#define TIME_LEN     20	/* 2026-11-12T10:15:30Z */

#define CANONICAL_LEN (sizeof(LINE_MAGIC) - 1 + sizeof(UUID_PREFIX) - 1 + AXIS_VOLUME_UUID_LEN + 1 + \
                       sizeof(TIME_PREFIX) - 1 + TIME_LEN + 1 + sizeof(NOTE_LINE) - 1)

G_STATIC_ASSERT(CANONICAL_LEN < AXIS_VOLUME_MARKER_MAX_BYTES);

/* ── UUIDs ──────────────────────────────────────────────────────── */

static gboolean
is_lower_hex(gchar c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* @text is exactly AXIS_VOLUME_UUID_LEN bytes long (not necessarily
 * terminated). */
static gboolean
uuid_bytes_valid(const gchar *text)
{
	gboolean nonzero = FALSE;
	guint i;

	for (i = 0; i < AXIS_VOLUME_UUID_LEN; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (text[i] != '-')
				return FALSE;
			continue;
		}
		if (!is_lower_hex(text[i]))
			return FALSE;
		if (text[i] != '0')
			nonzero = TRUE;
	}

	return nonzero;
}

gboolean
axis_volume_uuid_is_valid(const gchar *text)
{
	guint i;

	if (text == NULL)
		return FALSE;
	/* Bounded: no strlen over arbitrary input. */
	for (i = 0; i < AXIS_VOLUME_UUID_LEN; i++) {
		if (text[i] == '\0')
			return FALSE;
	}

	return text[AXIS_VOLUME_UUID_LEN] == '\0' && uuid_bytes_valid(text);
}

/* ── Times ──────────────────────────────────────────────────────── */

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
 * algorithm); @year is at least 1. */
static gint64
days_from_civil(
	gint64 year,
	guint  month,
	guint  day
){
	gint64 era;
	guint year_of_era;
	guint day_of_year;
	guint day_of_era;

	if (month <= 2)
		year--;
	era = year / 400;
	year_of_era = (guint)(year - era * 400);
	day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	return era * 146097 + (gint64)day_of_era - 719468;
}

/* The inverse, for @days of 0 or more. */
static void
civil_from_days(
	gint64  days,
	gint64 *year,
	guint  *month,
	guint  *day
){
	gint64 era;
	guint day_of_era;
	guint year_of_era;
	guint day_of_year;
	guint shifted_month;

	days += 719468;
	era = days / 146097;
	day_of_era = (guint)(days - era * 146097);
	year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	shifted_month = (5 * day_of_year + 2) / 153;

	*day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
	*month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
	*year = (gint64)year_of_era + era * 400 + (*month <= 2 ? 1 : 0);
}

static guint
days_in_month(
	gint64 year,
	guint  month
){
	static const guint days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	gboolean leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	return month == 2 && leap ? 29 : days[month - 1];
}

/* @count decimal digits at @text, or -1. */
static gint64
digits(
	const gchar *text,
	guint        count
){
	gint64 value = 0;
	guint i;

	for (i = 0; i < count; i++) {
		if (text[i] < '0' || text[i] > '9')
			return -1;
		value = value * 10 + (text[i] - '0');
	}

	return value;
}

/* "YYYY-MM-DDTHH:MM:SSZ", exactly TIME_LEN bytes, 1970 to 9999. */
static gboolean
parse_time(
	const gchar *text,
	gint64      *seconds
){
	gint64 year;
	gint64 month;
	gint64 day;
	gint64 hour;
	gint64 minute;
	gint64 second;

	if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
	    text[16] != ':' || text[19] != 'Z')
		return FALSE;

	year = digits(text, 4);
	month = digits(text + 5, 2);
	day = digits(text + 8, 2);
	hour = digits(text + 11, 2);
	minute = digits(text + 14, 2);
	second = digits(text + 17, 2);
	if (year < 1970 || month < 1 || month > 12 || day < 1 ||
	    day > (gint64)days_in_month(year, (guint)month) || hour < 0 || hour > 23 ||
	    minute < 0 || minute > 59 || second < 0 || second > 59)
		return FALSE;

	*seconds = days_from_civil(year, (guint)month, (guint)day) * 86400 + hour * 3600 +
		minute * 60 + second;
	return TRUE;
}

/* Writes TIME_LEN bytes (and a NUL) to @out. */
static gboolean
format_time(
	gint64  seconds,
	gchar  *out
){
	gint64 year;
	guint month;
	guint day;
	gint64 rest;

	if (seconds < 0 || seconds > AXIS_VOLUME_MARKER_MAX_TIME)
		return FALSE;

	civil_from_days(seconds / 86400, &year, &month, &day);
	rest = seconds % 86400;
	g_snprintf(out, TIME_LEN + 1, "%04d-%02u-%02uT%02d:%02d:%02dZ", (gint)year, month, day,
		(gint)(rest / 3600), (gint)(rest / 60 % 60), (gint)(rest % 60));

	return TRUE;
}

/* ── The marker ─────────────────────────────────────────────────── */

/* "axis-volume N\n" with N a later version than 1. */
static gboolean
is_later_version(
	const guint8 *data,
	gsize         length
){
	gsize prefix = sizeof(MAGIC_PREFIX) - 1;
	gsize i;
	guint64 version = 0;

	if (length <= prefix || memcmp(data, MAGIC_PREFIX, prefix) != 0)
		return FALSE;
	if (data[prefix] < '1' || data[prefix] > '9')
		return FALSE;

	for (i = prefix; i < length && i < prefix + 9; i++) {
		if (data[i] == '\n')
			return version > 1;
		if (data[i] < '0' || data[i] > '9')
			return FALSE;
		version = version * 10 + (guint64)(data[i] - '0');
	}

	return FALSE;
}

static gboolean
invalid(GError **error)
{
	g_set_error_literal(error, AXIS_VOLUME_MARKER_ERROR, AXIS_VOLUME_MARKER_ERROR_INVALID,
		"the file is not a valid Axis volume marker");
	return FALSE;
}

gboolean
axis_volume_marker_parse(
	const guint8      *data,
	gsize              length,
	AxisVolumeMarker  *marker,
	GError           **error
){
	const gchar *text = (const gchar *)data;
	gsize at = 0;
	AxisVolumeMarker result;

	g_return_val_if_fail(data != NULL || length == 0, FALSE);
	g_return_val_if_fail(marker != NULL, FALSE);

	if (length > AXIS_VOLUME_MARKER_MAX_BYTES) {
		g_set_error(error, AXIS_VOLUME_MARKER_ERROR, AXIS_VOLUME_MARKER_ERROR_TOO_LARGE,
			"the marker is larger than %d bytes", AXIS_VOLUME_MARKER_MAX_BYTES);
		return FALSE;
	}

	if (is_later_version(data, length)) {
		g_set_error_literal(error, AXIS_VOLUME_MARKER_ERROR, AXIS_VOLUME_MARKER_ERROR_VERSION,
			"the marker was written by a later version of Axis");
		return FALSE;
	}

	if (length != CANONICAL_LEN)
		return invalid(error);

	if (memcmp(text + at, LINE_MAGIC, sizeof(LINE_MAGIC) - 1) != 0)
		return invalid(error);
	at += sizeof(LINE_MAGIC) - 1;

	if (memcmp(text + at, UUID_PREFIX, sizeof(UUID_PREFIX) - 1) != 0)
		return invalid(error);
	at += sizeof(UUID_PREFIX) - 1;
	if (!uuid_bytes_valid(text + at) || text[at + AXIS_VOLUME_UUID_LEN] != '\n')
		return invalid(error);
	memcpy(result.uuid, text + at, AXIS_VOLUME_UUID_LEN);
	result.uuid[AXIS_VOLUME_UUID_LEN] = '\0';
	at += AXIS_VOLUME_UUID_LEN + 1;

	if (memcmp(text + at, TIME_PREFIX, sizeof(TIME_PREFIX) - 1) != 0)
		return invalid(error);
	at += sizeof(TIME_PREFIX) - 1;
	if (!parse_time(text + at, &result.created) || text[at + TIME_LEN] != '\n' ||
	    result.created > AXIS_VOLUME_MARKER_MAX_TIME)
		return invalid(error);
	at += TIME_LEN + 1;

	if (memcmp(text + at, NOTE_LINE, sizeof(NOTE_LINE) - 1) != 0)
		return invalid(error);
	at += sizeof(NOTE_LINE) - 1;

	if (at != length)
		return invalid(error);

	*marker = result;
	return TRUE;
}

gsize
axis_volume_marker_format(
	const AxisVolumeMarker *marker,
	gchar                  *buffer
){
	gchar time_text[TIME_LEN + 1];
	gint written;

	g_return_val_if_fail(marker != NULL, 0);
	g_return_val_if_fail(buffer != NULL, 0);
	g_return_val_if_fail(axis_volume_uuid_is_valid(marker->uuid), 0);
	g_return_val_if_fail(format_time(marker->created, time_text), 0);

	written = g_snprintf(buffer, AXIS_VOLUME_MARKER_MAX_BYTES, "%s%s%s\n%s%s\n%s", LINE_MAGIC,
		UUID_PREFIX, marker->uuid, TIME_PREFIX, time_text, NOTE_LINE);
	g_return_val_if_fail(written == (gint)CANONICAL_LEN, 0);

	return (gsize)written;
}
