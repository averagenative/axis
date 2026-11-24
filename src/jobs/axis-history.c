/*
 * axis-history.c - the history module: how long what happened to movies and
 * series, their grabs, the downloads and the imports are kept
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each table has one statement, which a run sends again and again: it takes
 * the oldest rows that ended before the setting's age, after where the last
 * one stopped (by the index on when they ended, 0082_history_retention.sql),
 * at most a batch of them, and deletes those of them that nothing needs.
 * The rows it looked at and kept are not looked at again until the sweep
 * has gone through every old row of the table and starts from the oldest
 * again, so that rows kept for good (a title's latest events, the grab of a
 * file in the library) never stand in front of rows that may go.
 *
 * What keeps a row comes from every reader of the tables (src/acquire,
 * src/import, src/download, src/modules/movies and tv, the pages), each
 * read for what it needs of old rows: when in doubt, a row is kept.  Each
 * statement says its own rules.
 * A row that another row names, by a foreign key or by a plain column, goes
 * only once that other row is gone: so a run deletes the events and the
 * imports first, then the grabs they named, then the downloads the grabs
 * and imports named.  An ON DELETE SET NULL is not taken as leave to delete:
 * no statement deletes a row while anything still names it.
 */

#include "jobs/axis-history.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-registry.h"

#define DAY_S (G_GINT64_CONSTANT(24) * 3600)

/* A download still at its client, or whose folder Axis still has to look
 * at (folder_removal), as dl. */
#define DOWNLOAD_HELD \
	"(dl.state IN ('sending', 'queued', 'downloading', 'paused', 'completed')" \
	" OR dl.folder_removal IN ('pending', 'waiting'))"

/* Ended longer ago than $1 seconds, after the cursor ($2 microseconds since
 * 1970 and the id $3; id 0: from the oldest). */
#define OLD_AND_AFTER(at, id) \
	" AND " at " < now() - $1::bigint * interval '1 second'" \
	" AND (" at ", " id ") > (CASE WHEN $3::bigint = 0 THEN '-infinity'::timestamptz" \
	"  ELSE timestamptz 'epoch' + $2::bigint * interval '1 microsecond' END, $3::bigint)"

/* At least $5 rows of the same title are newer (by id, as the pages order
 * them): the row is not one of its title's latest. */
#define NOT_LATEST(table, column, row) \
	"(SELECT count(*) FROM (SELECT 1 FROM " table " k WHERE k." column " = " row "." column \
	" AND k.id > " row ".id LIMIT $5::integer) n) >= $5::integer"

/* How many rows were looked at (c) and deleted (d), and the last looked at:
 * where the next statement goes on. */
#define RESULT \
	" SELECT (SELECT count(*) FROM c), (SELECT count(*) FROM d), l.us, l.id FROM (SELECT 1) one" \
	" LEFT JOIN LATERAL (SELECT (extract(epoch FROM c.at) * 1000000)::bigint AS us, c.id FROM c" \
	"  ORDER BY c.at DESC, c.id DESC LIMIT 1) l ON true"

/*
 * The events of a series.  Kept whatever their age:
 *  - "added" and "removed": when a series came and went, which nothing else
 *    records once it is gone;
 *  - what a person did (by_user): who changed, blocked, grabbed by hand;
 *  - the AXIS_HISTORY_EVENTS_KEPT latest of each series, so that its page
 *    (AXIS_TV_WEB_EVENTS_SHOWN) shows what it always did; for a series that
 *    was removed, the latest of its title.
 * Nothing names an event.  The first two are left out of the index the
 * statement reads, so that it never looks at them.
 */
static const gchar series_events_sql[] =
	"WITH c AS MATERIALIZED (SELECT e.id, e.created_at AS at FROM series_event e"
	" WHERE e.kind NOT IN ('added', 'removed') AND e.by_user IS NULL"
	OLD_AND_AFTER("e.created_at", "e.id")
	" ORDER BY e.created_at, e.id LIMIT $4::integer),"
	" d AS (DELETE FROM series_event e WHERE e.id = ANY (ARRAY(SELECT c.id FROM c))"
	" AND (e.series_id IS NULL OR " NOT_LATEST("series_event", "series_id", "e") ")"
	" AND (e.series_id IS NOT NULL OR e.title_id IS NULL OR (SELECT count(*) FROM (SELECT 1 FROM series_event k"
	"  WHERE k.title_id = e.title_id AND k.series_id IS NULL AND k.id > e.id LIMIT $5::integer) n) >= $5::integer)"
	" RETURNING 1)"
	RESULT;

/* The events of a movie, by the same rules: its page shows
 * AXIS_MOVIES_WEB_EVENTS_SHOWN, and the page of a movie that was removed
 * the latest of its title with no movie (axis_movies_store_title_events()). */
static const gchar movie_events_sql[] =
	"WITH c AS MATERIALIZED (SELECT e.id, e.created_at AS at FROM movie_event e"
	" WHERE e.kind NOT IN ('added', 'removed') AND e.by_user IS NULL"
	OLD_AND_AFTER("e.created_at", "e.id")
	" ORDER BY e.created_at, e.id LIMIT $4::integer),"
	" d AS (DELETE FROM movie_event e WHERE e.id = ANY (ARRAY(SELECT c.id FROM c))"
	" AND (e.movie_id IS NULL OR " NOT_LATEST("movie_event", "movie_id", "e") ")"
	" AND (e.movie_id IS NOT NULL OR e.title_id IS NULL OR (SELECT count(*) FROM (SELECT 1 FROM movie_event k"
	"  WHERE k.title_id = e.title_id AND k.movie_id IS NULL AND k.id > e.id LIMIT $5::integer) n) >= $5::integer)"
	" RETURNING 1)"
	RESULT;

/*
 * Imports that ended: imported or dismissed.  Waiting, running, blocked and
 * partly imported ones are open (a person retries a partial one), and never
 * looked at.  Kept whatever their age:
 *  - one that a file of the library came by (series_file, movie_file
 *    import_id): that it came by an import is itself a fact the pages and
 *    the TV episodes' "settled" read (axis-tv-store.c), which a NULL would
 *    change;
 *  - one with a file retired to the recycle folder (import_recycle), whose
 *    page says where that file is;
 *  - one whose download is still to be removed from its client, or failed
 *    to be (client_removal pending, failed), or is a torrent left seeding
 *    (kept): the cleanup, the folder removals and the Leftovers page count
 *    such a torrent's folder as in use by it;
 *  - one whose folder is still to be looked at (folder_removal pending,
 *    waiting);
 *  - one with a file still under its temporary name in the library (step
 *    temp or swapped), which the hourly cleanup settles or opens again, or
 *    whose download's copy is still to be deleted (source_state pending);
 *  - one whose download is still at its client, or whose download's folder
 *    is still to be looked at (the folder removal of a download reads
 *    whether an import of it was imported);
 *  - the latest import of each title (module, owner_id): what a movie's
 *    facts read as its own import;
 *  - one with a job queued or running for it.
 * Its files, its actions and its assignments are its own and go with it
 * (ON DELETE CASCADE).
 */
static const gchar imports_sql[] =
	"WITH c AS MATERIALIZED (SELECT i.id, i.finished_at AS at FROM import i"
	" WHERE i.state IN ('imported', 'dismissed')"
	OLD_AND_AFTER("i.finished_at", "i.id")
	" ORDER BY i.finished_at, i.id LIMIT $4::integer),"
	" d AS (DELETE FROM import i WHERE i.id = ANY (ARRAY(SELECT c.id FROM c))"
	" AND i.state IN ('imported', 'dismissed')"
	" AND (i.client_removal IS NULL OR i.client_removal NOT IN ('pending', 'failed', 'kept'))"
	" AND (i.folder_removal IS NULL OR i.folder_removal NOT IN ('pending', 'waiting'))"
	" AND NOT EXISTS (SELECT 1 FROM series_file sf WHERE sf.import_id = i.id)"
	" AND NOT EXISTS (SELECT 1 FROM movie_file mf WHERE mf.import_id = i.id)"
	" AND NOT EXISTS (SELECT 1 FROM import_recycle r WHERE r.import_id = i.id)"
	" AND NOT EXISTS (SELECT 1 FROM import_file f WHERE f.import_id = i.id AND f.temp_name IS NOT NULL"
	"  AND f.step IN ('temp', 'swapped'))"
	" AND NOT EXISTS (SELECT 1 FROM import_file f WHERE f.import_id = i.id AND f.source_state = 'pending')"
	" AND (i.download_id IS NULL OR NOT EXISTS (SELECT 1 FROM download dl WHERE dl.id = i.download_id"
	"  AND " DOWNLOAD_HELD "))"
	" AND (i.owner_id IS NULL OR EXISTS (SELECT 1 FROM import n WHERE n.module = i.module"
	"  AND n.owner_id = i.owner_id AND n.id > i.id))"
	" AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = 'import:' || i.id::text"
	"  AND j.state IN ('queued', 'running'))"
	" RETURNING 1)"
	RESULT;

/*
 * Grabs that are closed (an open one is never looked at).  Kept whatever
 * their age:
 *  - one whose download is still to be removed from its client, or failed
 *    to be (removal pending, failed): the follow-up asks again;
 *  - one on the blocklist (its entry names it), or named by an event;
 *  - one an import names, by the grab (import.grab_id, with the module) or
 *    by its download: an import's page, its block, the movie's and the
 *    episodes' facts find the grab by the download;
 *  - one whose download is still at its client, or whose download's folder
 *    is still to be looked at;
 *  - for a series, one whose import left episodes it was grabbed for
 *    without a file (AXIS_HISTORY_TV_LEFT_HOLES): its search reads it so as
 *    not to grab that release again;
 *  - the AXIS_HISTORY_GRABS_KEPT latest of each title.
 * The episodes a series' grab was for are its own and go with it.
 */
static const gchar series_grabs_sql[] =
	"WITH c AS MATERIALIZED (SELECT g.id, g.closed_at AS at FROM series_grab g"
	" WHERE g.closed_at IS NOT NULL"
	OLD_AND_AFTER("g.closed_at", "g.id")
	" ORDER BY g.closed_at, g.id LIMIT $4::integer),"
	" d AS (DELETE FROM series_grab g WHERE g.id = ANY (ARRAY(SELECT c.id FROM c))"
	" AND g.closed_at IS NOT NULL"
	" AND (g.removal IS NULL OR g.removal NOT IN ('pending', 'failed'))"
	" AND NOT (g.close_reason = 'imported' AND coalesce(g.close_text, '') LIKE '" AXIS_HISTORY_TV_LEFT_HOLES "%')"
	" AND NOT EXISTS (SELECT 1 FROM series_blocklist b WHERE b.grab_id = g.id)"
	" AND NOT EXISTS (SELECT 1 FROM series_event e WHERE e.grab_id = g.id)"
	" AND NOT EXISTS (SELECT 1 FROM import i WHERE i.module = 'tv' AND i.grab_id = g.id)"
	" AND (g.download_id IS NULL OR (NOT EXISTS (SELECT 1 FROM import i WHERE i.download_id = g.download_id)"
	"  AND NOT EXISTS (SELECT 1 FROM download dl WHERE dl.id = g.download_id AND " DOWNLOAD_HELD ")))"
	" AND " NOT_LATEST("series_grab", "series_id", "g")
	" RETURNING 1)"
	RESULT;

static const gchar movie_grabs_sql[] =
	"WITH c AS MATERIALIZED (SELECT g.id, g.closed_at AS at FROM movie_grab g"
	" WHERE g.closed_at IS NOT NULL"
	OLD_AND_AFTER("g.closed_at", "g.id")
	" ORDER BY g.closed_at, g.id LIMIT $4::integer),"
	" d AS (DELETE FROM movie_grab g WHERE g.id = ANY (ARRAY(SELECT c.id FROM c))"
	" AND g.closed_at IS NOT NULL"
	" AND (g.removal IS NULL OR g.removal NOT IN ('pending', 'failed'))"
	" AND NOT EXISTS (SELECT 1 FROM movie_blocklist b WHERE b.grab_id = g.id)"
	" AND NOT EXISTS (SELECT 1 FROM movie_event e WHERE e.grab_id = g.id)"
	" AND NOT EXISTS (SELECT 1 FROM import i WHERE i.module = 'movies' AND i.grab_id = g.id)"
	" AND (g.download_id IS NULL OR (NOT EXISTS (SELECT 1 FROM import i WHERE i.download_id = g.download_id)"
	"  AND NOT EXISTS (SELECT 1 FROM download dl WHERE dl.id = g.download_id AND " DOWNLOAD_HELD ")))"
	" AND " NOT_LATEST("movie_grab", "movie_id", "g")
	" RETURNING 1)"
	RESULT;

/*
 * Downloads that ended: failed, gone, removed or imported (one still at its
 * client is never looked at).  Kept whatever their age:
 *  - one a grab or an import names (download_id): the grab's and the
 *    import's pages and rules read it;
 *  - one whose folder is still to be looked at (folder_removal pending,
 *    waiting).
 */
static const gchar downloads_sql[] =
	"WITH c AS MATERIALIZED (SELECT dl.id, dl.finished_at AS at FROM download dl"
	" WHERE dl.state IN ('failed', 'gone', 'removed', 'imported') AND dl.finished_at IS NOT NULL"
	OLD_AND_AFTER("dl.finished_at", "dl.id")
	" ORDER BY dl.finished_at, dl.id LIMIT $4::integer),"
	" d AS (DELETE FROM download dl WHERE dl.id = ANY (ARRAY(SELECT c.id FROM c))"
	" AND dl.state IN ('failed', 'gone', 'removed', 'imported')"
	" AND (dl.folder_removal IS NULL OR dl.folder_removal NOT IN ('pending', 'waiting'))"
	" AND NOT EXISTS (SELECT 1 FROM series_grab g WHERE g.download_id = dl.id)"
	" AND NOT EXISTS (SELECT 1 FROM movie_grab g WHERE g.download_id = dl.id)"
	" AND NOT EXISTS (SELECT 1 FROM import i WHERE i.download_id = dl.id)"
	" RETURNING 1)"
	RESULT;

typedef struct
{
	const gchar *table;
	const gchar *sql;
	guint        rows;	/* looked at by one statement */
	guint        kept;	/* the latest of a title kept ($5); 0: none */
} Part;

static const Part parts[AXIS_HISTORY_N_PARTS] = {
	{ "series_event", series_events_sql, AXIS_HISTORY_BATCH_ROWS, AXIS_HISTORY_EVENTS_KEPT },
	{ "movie_event", movie_events_sql, AXIS_HISTORY_BATCH_ROWS, AXIS_HISTORY_EVENTS_KEPT },
	{ "import", imports_sql, AXIS_HISTORY_IMPORT_BATCH_ROWS, 0 },
	{ "series_grab", series_grabs_sql, AXIS_HISTORY_BATCH_ROWS, AXIS_HISTORY_GRABS_KEPT },
	{ "movie_grab", movie_grabs_sql, AXIS_HISTORY_BATCH_ROWS, AXIS_HISTORY_GRABS_KEPT },
	{ "download", downloads_sql, AXIS_HISTORY_BATCH_ROWS, 0 }
};

/* Where a table's sweep goes on: after the row (at_us, id); id 0: from the
 * oldest. */
typedef struct
{
	gint64 at_us;
	gint64 id;
} Cursor;

struct _AxisHistory
{
	AxisSettings *settings;	/* (nullable) */
	GMutex        lock;
	Cursor        cursor[AXIS_HISTORY_N_PARTS];	/* (lock) */
	guint         rows;	/* (lock) 0: each part's own */
	guint         statements;	/* (lock) 0: AXIS_HISTORY_BATCHES_PER_RUN */
};

AxisHistory *
axis_history_new(AxisSettings *settings)
{
	AxisHistory *self = g_new0(AxisHistory, 1);

	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	g_mutex_init(&self->lock);
	return self;
}

void
axis_history_free(AxisHistory *self)
{
	if (self == NULL)
		return;
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_history_set_batch(
	AxisHistory *self,
	guint        rows,
	guint        statements
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->rows = rows;
	self->statements = statements;
	g_mutex_unlock(&self->lock);
}

void
axis_history_read_keep(
	AxisHistory     *self,
	AxisHistoryKeep *keep
){
	g_return_if_fail(self != NULL && keep != NULL);

	if (self->settings == NULL) {
		keep->events_s = keep->grabs_s = keep->downloads_s = keep->imports_s =
			(gint64)AXIS_HISTORY_DEFAULT_DAYS * DAY_S;	/* a year */
		return;
	}
	keep->events_s = axis_settings_get_duration(self->settings, AXIS_HISTORY_SETTING_KEEP_EVENTS);
	keep->grabs_s = axis_settings_get_duration(self->settings, AXIS_HISTORY_SETTING_KEEP_GRABS);
	keep->downloads_s = axis_settings_get_duration(self->settings, AXIS_HISTORY_SETTING_KEEP_DOWNLOADS);
	keep->imports_s = axis_settings_get_duration(self->settings, AXIS_HISTORY_SETTING_KEEP_IMPORTS);
}

const gchar *
axis_history_part_name(AxisHistoryPart part)
{
	return (guint)part < AXIS_HISTORY_N_PARTS ? parts[part].table : "unknown";
}

static gint64
keep_of(
	const AxisHistoryKeep *keep,
	AxisHistoryPart        part
){
	switch (part) {
	case AXIS_HISTORY_SERIES_EVENTS:
	case AXIS_HISTORY_MOVIE_EVENTS:
		return keep->events_s;
	case AXIS_HISTORY_IMPORTS:
		return keep->imports_s;
	case AXIS_HISTORY_SERIES_GRABS:
	case AXIS_HISTORY_MOVIE_GRABS:
		return keep->grabs_s;
	case AXIS_HISTORY_DOWNLOADS:
		return keep->downloads_s;
	}
	return 0;
}

static gint64
int_at(
	AxisDbResult *result,
	guint         column
){
	gint64 value = 0;

	if (axis_db_result_get_n_rows(result) == 0 || axis_db_result_is_null(result, 0, column) ||
	    !axis_db_result_get_int64(result, 0, column, &value))
		return 0;
	return value;
}

/* One table's statements in this run. */
static gboolean
prune_part(
	AxisHistory        *self,
	AxisDbPool         *pool,
	AxisHistoryPart     part,
	gint64              keep_s,
	GCancellable       *cancellable,
	AxisHistoryReport  *report,
	GError            **error
){
	const Part *p = &parts[part];
	g_autofree gchar *keep = g_strdup_printf("%" G_GINT64_FORMAT, keep_s);
	g_autofree gchar *rows = NULL;
	g_autofree gchar *kept = g_strdup_printf("%u", p->kept);
	guint n_rows;
	guint statements;
	guint i;

	g_mutex_lock(&self->lock);
	n_rows = self->rows > 0 ? self->rows : p->rows;
	statements = self->statements > 0 ? self->statements : AXIS_HISTORY_BATCHES_PER_RUN;
	g_mutex_unlock(&self->lock);
	rows = g_strdup_printf("%u", n_rows);

	for (i = 0; i < statements; i++) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *at = NULL;
		g_autofree gchar *id = NULL;
		const gchar *params[5];
		Cursor cursor;
		gint64 examined;
		gint64 began;
		gdouble ms;

		if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
			return TRUE;
		g_mutex_lock(&self->lock);
		cursor = self->cursor[part];
		g_mutex_unlock(&self->lock);
		at = g_strdup_printf("%" G_GINT64_FORMAT, cursor.at_us);
		id = g_strdup_printf("%" G_GINT64_FORMAT, cursor.id);
		params[0] = keep;
		params[1] = at;
		params[2] = id;
		params[3] = rows;
		params[4] = kept;
		began = g_get_monotonic_time();
		if (!axis_db_pool_exec(pool, p->sql, p->kept > 0 ? 5 : 4, params, &result, error)) {
			g_prefix_error(error, "%s: ", p->table);
			return FALSE;
		}
		ms = (g_get_monotonic_time() - began) / 1000.0;
		examined = int_at(result, 0);
		report->statements[part]++;
		report->examined[part] += (guint64)MAX(examined, 0);
		report->deleted[part] += (guint64)MAX(int_at(result, 1), 0);
		report->longest_ms = MAX(report->longest_ms, ms);

		g_mutex_lock(&self->lock);
		if (examined < (gint64)n_rows) {
			/* Every old row has been looked at: the next run starts from
			 * the oldest again. */
			self->cursor[part].at_us = 0;
			self->cursor[part].id = 0;
		} else {
			self->cursor[part].at_us = int_at(result, 2);
			self->cursor[part].id = int_at(result, 3);
		}
		g_mutex_unlock(&self->lock);
		if (examined < (gint64)n_rows)
			return TRUE;
	}
	/* The bound: the rest waits for the next run. */
	report->more[part] = TRUE;
	return TRUE;
}

gboolean
axis_history_prune(
	AxisHistory            *self,
	AxisDbPool             *pool,
	const AxisHistoryKeep  *keep,
	GCancellable           *cancellable,
	AxisHistoryReport      *report_out,
	GError                **error
){
	AxisHistoryReport report;
	GError *first = NULL;
	guint i;

	g_return_val_if_fail(self != NULL && pool != NULL && keep != NULL, FALSE);

	memset(&report, 0, sizeof(report));
	for (i = 0; i < AXIS_HISTORY_N_PARTS; i++) {
		GError *local = NULL;
		gint64 keep_s = keep_of(keep, (AxisHistoryPart)i);

		/* 0: kept forever. */
		if (keep_s <= 0)
			continue;
		/* A cancelled run stops at once, with what it deleted kept. */
		if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
			break;
		if (prune_part(self, pool, (AxisHistoryPart)i, keep_s, cancellable, &report, &local))
			continue;
		/* An error in one table does not stop the others: the first is
		 * returned at the end, the later ones are logged. */
		if (first == NULL) {
			first = local;
		} else {
			g_warning("deleting old history: %s", local->message);
			g_error_free(local);
		}
	}
	if (report_out != NULL)
		*report_out = report;
	if (first != NULL) {
		g_propagate_error(error, first);
		return FALSE;
	}
	return TRUE;
}

/* ── The module ─────────────────────────────────────────────────── */

/* The ages are read once, at the start of the run, so that one run uses one
 * set of values. */
static gboolean
prune_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisHistory *self = user_data;
	AxisHistoryKeep keep;
	AxisHistoryReport report;
	guint64 events;
	guint64 grabs;
	gboolean more = FALSE;
	guint i;

	axis_history_read_keep(self, &keep);
	if (!axis_history_prune(self, axis_job_context_get_pool(context), &keep,
	        axis_job_context_get_cancellable(context), &report, error))
		return FALSE;

	for (i = 0; i < AXIS_HISTORY_N_PARTS; i++)
		more = more || report.more[i];
	events = report.deleted[AXIS_HISTORY_SERIES_EVENTS] + report.deleted[AXIS_HISTORY_MOVIE_EVENTS];
	grabs = report.deleted[AXIS_HISTORY_SERIES_GRABS] + report.deleted[AXIS_HISTORY_MOVIE_GRABS];
	if (events + grabs + report.deleted[AXIS_HISTORY_IMPORTS] + report.deleted[AXIS_HISTORY_DOWNLOADS] > 0 || more)
		g_info("deleted old history: %" G_GUINT64_FORMAT " event(s), %" G_GUINT64_FORMAT " import(s), %"
			G_GUINT64_FORMAT " grab(s), %" G_GUINT64_FORMAT " download(s); the longest statement took %.0f ms%s",
			events, report.deleted[AXIS_HISTORY_IMPORTS], grabs, report.deleted[AXIS_HISTORY_DOWNLOADS],
			report.longest_ms, more ? "; older rows are left for the next run" : "");
	return TRUE;
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_HISTORY_PRUNE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 3, prune_handler,
	  "Deleting old history of movies, series, downloads and imports", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_HISTORY_PRUNE_SCHEDULE, AXIS_HISTORY_PRUNE_KIND, NULL, AXIS_HISTORY_PRUNE_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const AxisSettingInfo setting_table[] = {
	{
		AXIS_HISTORY_SETTING_KEEP_EVENTS, AXIS_SETTING_DURATION,
		"Keep the history of what happened to movies and series for",
		"How long the events on a movie's or a series' page and on the History pages stay before the hourly "
		"clean-up deletes them. The 30 latest events of each movie and series, when a title was added or removed, "
		"and everything a person did are always kept. The default is a year (365d); 0 keeps them forever.",
		AXIS_SETTING_ZERO_OFF, "365d",
		(gint64)AXIS_HISTORY_MIN_DAYS * 24 * 3600, (gint64)AXIS_HISTORY_MAX_DAYS * 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_HISTORY_SETTING_KEEP_GRABS, AXIS_SETTING_DURATION,
		"Keep the record of each release grabbed for",
		"How long the record of a grab that ended (imported, failed, given up) stays before the hourly clean-up "
		"deletes it, counted from when it ended. A grab that an import, a blocklist entry or a kept event still "
		"names, or whose download is still at its client, is kept, and so are the 5 latest of each movie and "
		"series. The default is a year (365d); 0 keeps them forever.",
		AXIS_SETTING_ZERO_OFF, "365d",
		(gint64)AXIS_HISTORY_MIN_DAYS * 24 * 3600, (gint64)AXIS_HISTORY_MAX_DAYS * 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_HISTORY_SETTING_KEEP_DOWNLOADS, AXIS_SETTING_DURATION,
		"Keep finished downloads on the Downloads page for",
		"How long a download that finished, failed or was removed stays listed before the hourly clean-up deletes "
		"it, counted from when it ended. A download that a grab or an import still names, or whose folder is still "
		"to be cleaned up, is kept. The default is a year (365d); 0 keeps them forever.",
		AXIS_SETTING_ZERO_OFF, "365d",
		(gint64)AXIS_HISTORY_MIN_DAYS * 24 * 3600, (gint64)AXIS_HISTORY_MAX_DAYS * 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_HISTORY_SETTING_KEEP_IMPORTS, AXIS_SETTING_DURATION,
		"Keep finished imports on the Imports page for",
		"How long an import that was imported or dismissed stays, with its files and decisions, before the hourly "
		"clean-up deletes it, counted from when it ended. An import that a file in the library came by, that "
		"retired a file to the recycle folder, that has work left at its download client, or that is the latest of "
		"its movie or series is kept. The default is a year (365d); 0 keeps them forever.",
		AXIS_SETTING_ZERO_OFF, "365d",
		(gint64)AXIS_HISTORY_MIN_DAYS * 24 * 3600, (gint64)AXIS_HISTORY_MAX_DAYS * 24 * 3600, 0, 0, NULL
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

static const AxisModuleInfo module = {
	AXIS_HISTORY_MODULE_ID,
	"History",
	NULL,
	NULL,
	handlers,
	schedules,
	NULL,
	NULL,
	"Keeps the history of movies, series, downloads and imports for as long as the settings say, and deletes "
	"what is older.",
	setting_table,
	AXIS_MODULE_REQUIRED,
	NULL,
	NULL
};

const AxisModuleInfo *
axis_history_get_module(void)
{
	return &module;
}
