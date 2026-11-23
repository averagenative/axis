/*
 * axis-db-result.h - the rows returned by a statement
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Values arrive in PostgreSQL's text format.  A result is independent of
 * the connection that produced it: it may be kept and read after the
 * connection has been released, from any one thread at a time.
 *
 * Row and column numbers start at 0.  Passing one that is out of range is
 * a programming error: it logs a critical and returns the documented
 * failure value.
 */

#ifndef AXIS_DB_RESULT_H
#define AXIS_DB_RESULT_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AxisDbResult AxisDbResult;

void
axis_db_result_free(AxisDbResult *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDbResult, axis_db_result_free)

/**
 * axis_db_result_get_n_rows:
 *
 * Returns: the number of rows; 0 for a statement that returns none
 */
guint
axis_db_result_get_n_rows(AxisDbResult *self);

/**
 * axis_db_result_get_n_columns:
 *
 * Returns: the number of columns; 0 for a statement that returns none
 */
guint
axis_db_result_get_n_columns(AxisDbResult *self);

/**
 * axis_db_result_get_column_index:
 * @self: an #AxisDbResult
 * @name: a column name, compared exactly (case matters, no quoting)
 *
 * Returns: the index of the first column called @name, or -1 if there is
 *   none
 */
gint
axis_db_result_get_column_index(
	AxisDbResult *self,
	const gchar  *name
);

/**
 * axis_db_result_is_null:
 *
 * Returns: %TRUE if the value is SQL NULL (an empty string is not)
 */
gboolean
axis_db_result_is_null(
	AxisDbResult *self,
	guint         row,
	guint         column
);

/**
 * axis_db_result_get_text:
 *
 * Returns: (transfer none) (nullable): the value in text format, owned by
 *   @self; %NULL if it is SQL NULL, "" if it is an empty string
 */
const gchar *
axis_db_result_get_text(
	AxisDbResult *self,
	guint         row,
	guint         column
);

/**
 * axis_db_result_get_int64:
 * @self: an #AxisDbResult
 * @row: the row
 * @column: the column
 * @value: (out): the value; untouched on failure
 *
 * Parses the value as a base-10 integer: an optional '-', digits only,
 * nothing else, within the range of #gint64.  The text form of
 * smallint, integer and bigint always qualifies.
 *
 * Returns: %TRUE on success; %FALSE if the value is NULL or not such an
 *   integer
 */
gboolean
axis_db_result_get_int64(
	AxisDbResult *self,
	guint         row,
	guint         column,
	gint64       *value
);

/**
 * axis_db_result_get_boolean:
 * @self: an #AxisDbResult
 * @row: the row
 * @column: the column
 * @value: (out): the value; untouched on failure
 *
 * Reads a boolean in PostgreSQL's text format: exactly "t" or "f".
 *
 * Returns: %TRUE on success; %FALSE if the value is NULL or anything else
 */
gboolean
axis_db_result_get_boolean(
	AxisDbResult *self,
	guint         row,
	guint         column,
	gboolean     *value
);

/**
 * axis_db_result_get_affected_rows:
 *
 * Returns: the number of rows an INSERT, UPDATE, DELETE, MERGE, SELECT,
 *   MOVE, FETCH or COPY touched (as in the command tag); 0 for any other
 *   statement
 */
guint64
axis_db_result_get_affected_rows(AxisDbResult *self);

G_END_DECLS

#endif /* AXIS_DB_RESULT_H */
