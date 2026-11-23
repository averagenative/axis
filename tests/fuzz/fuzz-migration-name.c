/*
 * fuzz-migration-name.c - libFuzzer target for the migration file-name
 * parser and the no-transaction marker
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Built with clang -fsanitize=fuzzer,address,undefined by `make fuzz`.
 * Besides memory safety, it checks what the parsers promise: an accepted
 * name has a version in range and a name of the allowed characters, and
 * formatting them back reproduces the input exactly; the marker is found
 * only when the input starts with it.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "db/axis-db-migrate.h"

static void
check_filename(const gchar *text)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *again = NULL;
	guint version = 0;
	const gchar *p;

	if (!axis_db_migration_parse_filename(text, &version, &name))
		return;

	if (version < 1 || version > AXIS_DB_MIGRATION_MAX_VERSION)
		abort();
	if (name == NULL || *name == '\0' || strlen(name) > AXIS_DB_MIGRATION_NAME_MAX_LEN)
		abort();
	if (!(name[0] >= 'a' && name[0] <= 'z'))
		abort();
	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
			abort();
	}

	again = g_strdup_printf("%04u_%s.sql", version, name);
	if (strcmp(again, text) != 0)
		abort();
}

static void
check_marker(const gchar *text)
{
	if (axis_db_migration_has_no_transaction_marker(text) &&
	    !g_str_has_prefix(text, AXIS_DB_NO_TRANSACTION_MARKER))
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* The parsers take C strings; an embedded NUL ends the input. */
	g_autofree gchar *text = g_strndup((const gchar *)data, size);

	check_filename(text);
	check_marker(text);

	return 0;
}
