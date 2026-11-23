/*
 * axis-db-migrate-parse.c - migration file names and markers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure functions with no database behind them, kept apart so the fuzz
 * target can build them alone (tests/fuzz/fuzz-migration-name.c).
 */

#include "db/axis-db-migrate.h"

#include <string.h>

static gboolean
is_name_start(gchar c)
{
	return c >= 'a' && c <= 'z';
}

static gboolean
is_name_char(gchar c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

gboolean
axis_db_migration_parse_filename(
	const gchar  *filename,
	guint        *version,
	gchar       **name
){
	static const gchar suffix[] = ".sql";
	gsize suffix_len = sizeof(suffix) - 1;
	gsize len;
	gsize name_len;
	guint value = 0;
	gsize i;

	if (filename == NULL)
		return FALSE;

	/* "NNNN" "_" name ".sql" */
	len = strlen(filename);
	if (len < 4 + 1 + 1 + suffix_len)
		return FALSE;

	for (i = 0; i < 4; i++) {
		if (filename[i] < '0' || filename[i] > '9')
			return FALSE;
		value = value * 10 + (guint)(filename[i] - '0');
	}
	if (value < 1 || value > AXIS_DB_MIGRATION_MAX_VERSION)
		return FALSE;

	if (filename[4] != '_')
		return FALSE;

	if (memcmp(filename + len - suffix_len, suffix, suffix_len) != 0)
		return FALSE;

	name_len = len - 5 - suffix_len;
	if (name_len < 1 || name_len > AXIS_DB_MIGRATION_NAME_MAX_LEN)
		return FALSE;
	if (!is_name_start(filename[5]))
		return FALSE;
	for (i = 5; i < 5 + name_len; i++) {
		if (!is_name_char(filename[i]))
			return FALSE;
	}

	if (version != NULL)
		*version = value;
	if (name != NULL)
		*name = g_strndup(filename + 5, name_len);

	return TRUE;
}

gboolean
axis_db_migration_has_no_transaction_marker(const gchar *sql)
{
	static const gchar marker[] = AXIS_DB_NO_TRANSACTION_MARKER;
	gsize marker_len = sizeof(marker) - 1;
	const gchar *end;
	const gchar *p;

	if (sql == NULL)
		return FALSE;

	if (strncmp(sql, marker, marker_len) != 0)
		return FALSE;

	/* Only trailing blanks, then an optional '\r', then the end of the
	 * line or of the text. */
	end = strchr(sql, '\n');
	if (end == NULL)
		end = sql + strlen(sql);

	for (p = sql + marker_len; p < end; p++) {
		if (*p == ' ' || *p == '\t')
			continue;
		if (*p == '\r' && p + 1 == end)
			continue;
		return FALSE;
	}

	return TRUE;
}

gchar *
axis_db_migration_checksum(const gchar *sql)
{
	g_return_val_if_fail(sql != NULL, NULL);

	return g_compute_checksum_for_data(G_CHECKSUM_SHA256,
		(const guchar *)sql, strlen(sql));
}
