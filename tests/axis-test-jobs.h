/*
 * axis-test-jobs.h - test support for the job queue: waiting without sleeping
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tests that wait for something to happen pump the default main context
 * (so "job-event" signals are delivered meanwhile) and re-check their
 * condition at least every 10 ms, up to a timeout; none sleeps for a fixed
 * time where a condition can be awaited.  Every wait is bounded.
 */

#ifndef AXIS_TEST_JOBS_H
#define AXIS_TEST_JOBS_H

#include <glib.h>

#include "axis-test-db.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

/* Generous bounds, so a loaded machine does not fail a test that works. */
#define AXIS_TEST_WAIT_MS 10000

/* A pool of @size on the test database, with every migration applied. */
AxisDbPool *
axis_test_jobs_pool(
	AxisTestDb *db,
	guint       size
);

typedef gboolean (*AxisTestCondition)(gpointer data);

/* Pumps the default main context until @done returns TRUE (checked at
 * least every 10 ms, never while the context is held) or @timeout_ms
 * passes.  Returns the last value of @done. */
gboolean
axis_test_pump_until(
	AxisTestCondition  done,
	gpointer           data,
	guint              timeout_ms
);

/* Pumps the default main context for @ms: for asserting that something
 * does not happen within a time. */
void
axis_test_pump_for(guint ms);

/* Pumps until the first column of the first row of @sql (run on the test
 * helper's own connection) equals @expected (NULL for no row or SQL NULL),
 * or @timeout_ms passes.  Returns whether it did. */
gboolean
axis_test_wait_query(
	AxisTestDb  *db,
	const gchar *sql,
	const gchar *expected,
	guint        timeout_ms
);

/* printf-style axis_test_db_query(). */
gchar *
axis_test_db_queryf(
	AxisTestDb  *db,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

G_END_DECLS

#endif /* AXIS_TEST_JOBS_H */
