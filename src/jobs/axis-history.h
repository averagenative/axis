/*
 * axis-history.h - the history module: how long what happened to movies and
 * series, their grabs, the downloads and the imports are kept
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The "history" module registers one job kind and its default schedule:
 *
 *   history.prune (queue "maintenance"), every hour: deletes what ended
 *   longer ago than its setting, in bounded batches.  Each statement looks
 *   at most at a fixed number of the oldest rows of one table (found through
 *   an index on when they ended, after where the last statement stopped)
 *   and deletes those of them that nothing needs; each is its own short
 *   transaction; a run makes at most a fixed number of them a table, and
 *   the rest waits for the next run.  A backlog (after an upgrade) drains
 *   over several runs, never in one long lock.
 *
 * The settings, durations from 30 days to 10 years, or 0 for "kept for
 * ever" (365 days by default):
 *
 *   history.keep_events     the events of movies and series (created_at)
 *   history.keep_grabs      grabs that are closed (closed_at)
 *   history.keep_downloads  downloads that ended (finished_at)
 *   history.keep_imports    imports that ended, imported or dismissed
 *                           (finished_at)
 *
 * What is never deleted, whatever its age, is in axis-history.c beside each
 * statement: what another row names, what is open, live or pending, what a
 * blocklist, a file in the library or an attention line needs, and a floor
 * of each title's latest events and grabs.  Pass an #AxisHistory as the
 * module's user data.
 *
 * The module is required: it cannot be switched off.
 */

#ifndef AXIS_HISTORY_H
#define AXIS_HISTORY_H

#include <gio/gio.h>

#include "core/axis-module.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

#define AXIS_HISTORY_MODULE_ID              "history"
#define AXIS_HISTORY_PRUNE_KIND             "history.prune"
#define AXIS_HISTORY_PRUNE_SCHEDULE         "history.prune"
#define AXIS_HISTORY_PRUNE_INTERVAL_SECONDS 3600
#define AXIS_HISTORY_SETTING_KEEP_EVENTS    "history.keep_events"
#define AXIS_HISTORY_SETTING_KEEP_GRABS     "history.keep_grabs"
#define AXIS_HISTORY_SETTING_KEEP_DOWNLOADS "history.keep_downloads"
#define AXIS_HISTORY_SETTING_KEEP_IMPORTS   "history.keep_imports"

/* The settings' limits, in days (0, kept forever, besides). */
#define AXIS_HISTORY_MIN_DAYS     30
#define AXIS_HISTORY_MAX_DAYS     3650
#define AXIS_HISTORY_DEFAULT_DAYS 365

/* The rows one statement looks at, at most: as many parents may go in it.
 * An import takes its files with it (at most AXIS_IMPORT_MAX_FILES each),
 * so fewer of those. */
#define AXIS_HISTORY_BATCH_ROWS        1000
#define AXIS_HISTORY_IMPORT_BATCH_ROWS 250
/* The statements of one table in one run, at most: an hourly run deletes up
 * to 50,000 rows of a table in a few seconds, and the rest of a backlog
 * waits for the next. */
#define AXIS_HISTORY_BATCHES_PER_RUN   50

/* Kept whatever their age: the latest events of each movie and series (as
 * many as a title's page shows, and more), and the latest grabs. */
#define AXIS_HISTORY_EVENTS_KEPT 30
#define AXIS_HISTORY_GRABS_KEPT  5

/* How the record of a series' grab begins when its import left episodes it
 * was grabbed for without a file (AXIS_TV_GRAB_LEFT_HOLES): a search reads
 * those grabs so as not to grab the release again. */
#define AXIS_HISTORY_TV_LEFT_HOLES "Imported, with no file for:"

/**
 * AxisHistoryPart:
 *
 * The tables a run deletes from, in the order it does: a row goes only
 * after what names it, so the events and the imports go before the grabs
 * they name, and those before their downloads.
 */
typedef enum
{
	AXIS_HISTORY_SERIES_EVENTS = 0,
	AXIS_HISTORY_MOVIE_EVENTS,
	AXIS_HISTORY_IMPORTS,
	AXIS_HISTORY_SERIES_GRABS,
	AXIS_HISTORY_MOVIE_GRABS,
	AXIS_HISTORY_DOWNLOADS
} AxisHistoryPart;

#define AXIS_HISTORY_N_PARTS 6

/**
 * AxisHistoryKeep:
 *
 * How long each kind is kept, in seconds; 0 keeps it forever.
 */
typedef struct
{
	gint64 events_s;
	gint64 grabs_s;
	gint64 downloads_s;
	gint64 imports_s;
} AxisHistoryKeep;

/**
 * AxisHistoryReport:
 * @examined: the rows each table's statements looked at
 * @deleted: the rows deleted from each table (its own: an import's files,
 *   actions and assignments and a grab's episodes go with it uncounted)
 * @statements: the statements each table took
 * @more: the run stopped at its bound with old rows still to look at: the
 *   next run goes on from there
 * @longest_ms: the longest of the statements, as the daemon waited for it
 */
typedef struct
{
	guint64  examined[AXIS_HISTORY_N_PARTS];
	guint64  deleted[AXIS_HISTORY_N_PARTS];
	guint    statements[AXIS_HISTORY_N_PARTS];
	gboolean more[AXIS_HISTORY_N_PARTS];
	gdouble  longest_ms;
} AxisHistoryReport;

typedef struct _AxisHistory AxisHistory;

/**
 * axis_history_new:
 * @settings: (nullable): the settings store the job reads the ages from;
 *   without one the defaults apply
 *
 * Returns: (transfer full): the module's state: where each table's sweep
 *   goes on (kept in memory: after a restart a sweep starts from the
 *   oldest rows again)
 */
AxisHistory *
axis_history_new(AxisSettings *settings);

void
axis_history_free(AxisHistory *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisHistory, axis_history_free)

/**
 * axis_history_set_batch:
 * @self: the module's state
 * @rows: the rows one statement looks at (0: the tables' own)
 * @statements: the statements of a table in a run (0:
 *   %AXIS_HISTORY_BATCHES_PER_RUN)
 *
 * For tests, which show the bounds with a handful of rows.
 */
void
axis_history_set_batch(
	AxisHistory *self,
	guint        rows,
	guint        statements
);

/**
 * axis_history_read_keep:
 *
 * The ages the settings say now (the defaults without a settings store).
 */
void
axis_history_read_keep(
	AxisHistory     *self,
	AxisHistoryKeep *keep
);

/**
 * axis_history_prune:
 * @self: the module's state
 * @pool: the database
 * @keep: how long each kind is kept
 * @cancellable: (nullable): checked between statements; a run that is
 *   cancelled stops there, with what it deleted kept
 * @report: (out caller-allocates) (optional): what the run did
 * @error: (nullable): return location for an error
 *
 * One run: each table in the order of #AxisHistoryPart, its statements
 * until one finds no more old rows or the run's bound is reached.  Each
 * statement is a transaction of its own.
 *
 * Returns: %TRUE on success (also when cancelled)
 */
gboolean
axis_history_prune(
	AxisHistory            *self,
	AxisDbPool             *pool,
	const AxisHistoryKeep  *keep,
	GCancellable           *cancellable,
	AxisHistoryReport      *report,
	GError                **error
);

/**
 * axis_history_part_name:
 *
 * Returns: (transfer none): the table a part deletes from ("series_event")
 */
const gchar *
axis_history_part_name(AxisHistoryPart part);

/**
 * axis_history_get_module:
 *
 * The module to register with an #AxisHistory as its user data.
 *
 * Returns: (transfer none): the module description, in static storage
 */
const AxisModuleInfo *
axis_history_get_module(void);

G_END_DECLS

#endif /* AXIS_HISTORY_H */
