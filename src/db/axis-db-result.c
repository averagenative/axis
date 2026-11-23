/*
 * axis-db-result.c - the rows returned by a statement
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "db/axis-db-result.h"
#include "db/axis-db-private.h"

#include <string.h>

struct _AxisDbResult
{
	PGresult *res;
};

AxisDbResult *
axis_db_result_new_take(PGresult *res)
{
	AxisDbResult *self;

	g_return_val_if_fail(res != NULL, NULL);

	self = g_new0(AxisDbResult, 1);
	self->res = res;

	return self;
}

void
axis_db_result_free(AxisDbResult *self)
{
	if (self == NULL)
		return;

	PQclear(self->res);
	g_free(self);
}

guint
axis_db_result_get_n_rows(AxisDbResult *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return (guint)PQntuples(self->res);
}

guint
axis_db_result_get_n_columns(AxisDbResult *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return (guint)PQnfields(self->res);
}

gint
axis_db_result_get_column_index(
	AxisDbResult *self,
	const gchar  *name
){
	gint n;
	gint i;

	g_return_val_if_fail(self != NULL, -1);
	g_return_val_if_fail(name != NULL, -1);

	/* Not PQfnumber(): it case-folds and unquotes, so "Name" would find
	 * a column called name. */
	n = PQnfields(self->res);
	for (i = 0; i < n; i++) {
		if (strcmp(PQfname(self->res, i), name) == 0)
			return i;
	}

	return -1;
}

static gboolean
in_range(
	AxisDbResult *self,
	guint         row,
	guint         column
){
	if (row >= (guint)PQntuples(self->res) || column >= (guint)PQnfields(self->res)) {
		g_critical("axis_db_result: row %u, column %u is out of range (%d rows, %d columns)",
			row, column, PQntuples(self->res), PQnfields(self->res));
		return FALSE;
	}

	return TRUE;
}

gboolean
axis_db_result_is_null(
	AxisDbResult *self,
	guint         row,
	guint         column
){
	g_return_val_if_fail(self != NULL, TRUE);

	if (!in_range(self, row, column))
		return TRUE;

	return PQgetisnull(self->res, (int)row, (int)column) ? TRUE : FALSE;
}

const gchar *
axis_db_result_get_text(
	AxisDbResult *self,
	guint         row,
	guint         column
){
	g_return_val_if_fail(self != NULL, NULL);

	if (!in_range(self, row, column))
		return NULL;
	if (PQgetisnull(self->res, (int)row, (int)column))
		return NULL;

	return PQgetvalue(self->res, (int)row, (int)column);
}

gboolean
axis_db_result_get_int64(
	AxisDbResult *self,
	guint         row,
	guint         column,
	gint64       *value
){
	const gchar *text;
	const gchar *p;
	gint64 parsed;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(value != NULL, FALSE);

	text = axis_db_result_get_text(self, row, column);
	if (text == NULL || *text == '\0')
		return FALSE;

	/* g_ascii_string_to_signed() would also take a leading '+'. */
	p = text;
	if (*p == '-')
		p++;
	if (*p == '\0')
		return FALSE;
	for (; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return FALSE;
	}

	if (!g_ascii_string_to_signed(text, 10, G_MININT64, G_MAXINT64, &parsed, NULL))
		return FALSE;

	*value = parsed;
	return TRUE;
}

gboolean
axis_db_result_get_boolean(
	AxisDbResult *self,
	guint         row,
	guint         column,
	gboolean     *value
){
	const gchar *text;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(value != NULL, FALSE);

	text = axis_db_result_get_text(self, row, column);
	if (text == NULL)
		return FALSE;

	if (strcmp(text, "t") == 0) {
		*value = TRUE;
		return TRUE;
	}
	if (strcmp(text, "f") == 0) {
		*value = FALSE;
		return TRUE;
	}

	return FALSE;
}

const gchar *
axis_db_result_get_command_tag(AxisDbResult *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return PQcmdStatus(self->res);
}

guint64
axis_db_result_get_affected_rows(AxisDbResult *self)
{
	const gchar *tuples;
	guint64 count;

	g_return_val_if_fail(self != NULL, 0);

	tuples = PQcmdTuples(self->res);
	if (tuples == NULL || *tuples == '\0')
		return 0;
	if (!g_ascii_string_to_unsigned(tuples, 10, 0, G_MAXUINT64, &count, NULL))
		return 0;

	return count;
}
