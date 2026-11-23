/*
 * axis-test-jobs.c - test support for the job queue: waiting without sleeping
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-jobs.h"
#include "db/axis-db-migrate.h"

#include <stdarg.h>

AxisDbPool *
axis_test_jobs_pool(
	AxisTestDb *db,
	guint       size
){
	g_autoptr(GError) error = NULL;
	AxisDbPool *pool;

	pool = axis_db_pool_new(axis_test_db_get_url(db), size, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pool);
	/* The error first: a migration that fails says which statement and why. */
	if (!axis_db_migrate(pool, &error))
		g_assert_no_error(error);

	return pool;
}

static gboolean
on_tick(gpointer data)
{
	return G_SOURCE_CONTINUE;
}

gboolean
axis_test_pump_until(
	AxisTestCondition  done,
	gpointer           data,
	guint              timeout_ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	guint tick;
	gboolean ok;

	/* Wakes the blocking iteration below at least every 10 ms. */
	tick = g_timeout_add(10, on_tick, NULL);
	for (;;) {
		ok = done(data);
		if (ok || g_get_monotonic_time() >= deadline)
			break;
		g_main_context_iteration(NULL, TRUE);
	}
	g_source_remove(tick);

	/* Whatever is ready now, too. */
	while (g_main_context_iteration(NULL, FALSE))
		;

	return ok;
}

static gboolean
never(gpointer data)
{
	return FALSE;
}

void
axis_test_pump_for(guint ms)
{
	axis_test_pump_until(never, NULL, ms);
}

typedef struct
{
	AxisTestDb  *db;
	const gchar *sql;
	const gchar *expected;
} QueryWait;

static gboolean
query_matches(gpointer data)
{
	QueryWait *wait = data;
	g_autofree gchar *value = axis_test_db_query(wait->db, wait->sql);

	return g_strcmp0(value, wait->expected) == 0;
}

gboolean
axis_test_wait_query(
	AxisTestDb  *db,
	const gchar *sql,
	const gchar *expected,
	guint        timeout_ms
){
	QueryWait wait;
	gboolean ok;

	wait.db = db;
	wait.sql = sql;
	wait.expected = expected;
	ok = axis_test_pump_until(query_matches, &wait, timeout_ms);
	if (!ok) {
		g_autofree gchar *value = axis_test_db_query(db, sql);

		g_test_message("waited %u ms for \"%s\" to be %s; it is %s",
			timeout_ms, sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
	}

	return ok;
}

gchar *
axis_test_db_queryf(
	AxisTestDb  *db,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);

	return axis_test_db_query(db, sql);
}
