/*
 * test-history-retention.c - the history module: what its hourly job
 * deletes of old events, grabs, downloads and imports, what it never
 * deletes, the bounds of a run, and its settings
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test makes its own database with one series (one episode) and one
 * movie on a made-up disk, writes the rows it needs by SQL, runs one or more
 * runs of axis_history_prune() and reads what is left.  "Old" is 400 days
 * ago, beyond the default of 365 days; "recent" is 10 days ago.  Each rule
 * of what is kept has a case of its own; /history/<table>/old-one-goes shows
 * that the same rows without the rule do go.
 *
 * Every title here is made up.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "core/axis-error.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "core/axis-settings.h"
#include "jobs/axis-history.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "modules/tv/axis-tv-private.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-settings.h"

#define OLD    "now() - interval '400 days'"
#define RECENT "now() - interval '10 days'"

typedef struct
{
	AxisTestDb  *db;
	AxisDbPool  *pool;
	AxisHistory *history;
	gint64       volume_id;
	gint64       root_id;
	gint64       series_id;
	gint64       series_title;
	gint64       episode_id;
	gint64       movie_id;
	gint64       movie_title;
	guint        made;	/* rows made so far: names that differ */
} Fixture;

/* ── The fixture ────────────────────────────────────────────────── */

static gint64
qi(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gint64
qi(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *value = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	value = axis_test_db_query(fx->db, text);
	return value != NULL ? g_ascii_strtoll(value, NULL, 10) : -1;
}

static void
sql(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
sql(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	axis_test_db_exec(fx->db, text);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 4);
	fx->history = axis_history_new(NULL);

	fx->volume_id = qi(fx, "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at) VALUES"
		" ('5a5a5a5a-1111-4222-8333-444444444444', 'Made-up disk', '/srv/made-up-disk', false, 'offline', now())"
		" RETURNING id");
	fx->root_id = qi(fx, "INSERT INTO library_root (volume_id, rel_path, media_type, name) VALUES (%" G_GINT64_FORMAT
		", 'library', 'tv', 'Made-up library') RETURNING id", fx->volume_id);
	fx->series_title = qi(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id) VALUES"
		" ('series', 'Made-up Harbour Watch', 2020, 'ended', 'fixture', 'h1') RETURNING id");
	fx->series_id = qi(fx, "INSERT INTO series (title_id, quality_profile_id, root_id) SELECT %" G_GINT64_FORMAT ","
		" id, %" G_GINT64_FORMAT " FROM quality_profile WHERE slug = 'balanced' RETURNING id", fx->series_title,
		fx->root_id);
	fx->episode_id = qi(fx, "INSERT INTO series_episode (series_id, season_number, number, monitored) VALUES"
		" (%" G_GINT64_FORMAT ", 1, 1, true) RETURNING id", fx->series_id);
	fx->movie_title = qi(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id) VALUES"
		" ('film', 'Made-up Lantern Night', 2001, 'released', 'fixture', 'f1') RETURNING id");
	fx->movie_id = qi(fx, "INSERT INTO movie (title_id, quality_profile_id, root_id) SELECT %" G_GINT64_FORMAT ","
		" id, %" G_GINT64_FORMAT " FROM quality_profile WHERE slug = 'balanced' RETURNING id", fx->movie_title,
		fx->root_id);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_history_free(fx->history);
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	if (fx->db != NULL)
		axis_test_db_free(fx->db);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── Making rows ────────────────────────────────────────────────── */

/* An event of the series (@series: else of no series, of its title) or of
 * the movie, @when (SQL), @kind, by @user (nullable), naming @grab (0:
 * none). */
static gint64
add_event(
	Fixture     *fx,
	gboolean     movie,
	gboolean     managed,
	const gchar *kind,
	const gchar *user,
	gint64       grab,
	const gchar *when
){
	g_autofree gchar *by = user != NULL ? g_strdup_printf("'%s'", user) : g_strdup("NULL");
	g_autofree gchar *grab_text = grab > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, grab) : g_strdup("NULL");

	if (movie)
		return qi(fx, "INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, by_user, grab_id,"
			" created_at) VALUES (%s, %" G_GINT64_FORMAT ", 'Made-up Lantern Night (2001)', '%s', 'As the test"
			" made it.', %s, %s, %s) RETURNING id", managed ? "(SELECT max(id) FROM movie)" : "NULL",
			fx->movie_title, kind, by, grab_text, when);
	return qi(fx, "INSERT INTO series_event (series_id, title_id, title_label, kind, text, by_user, grab_id,"
		" created_at) VALUES (%s, %" G_GINT64_FORMAT ", 'Made-up Harbour Watch (2020)', '%s', 'As the test made"
		" it.', %s, %s, %s) RETURNING id", managed ? "(SELECT max(id) FROM series)" : "NULL", fx->series_title,
		kind, by, grab_text, when);
}

static void
add_events(
	Fixture     *fx,
	gboolean     movie,
	gboolean     managed,
	guint        n,
	const gchar *when
){
	guint i;

	for (i = 0; i < n; i++)
		add_event(fx, movie, managed, "searched", NULL, 0, when);
}

/* A download in @state, ended @when (SQL; NULL: not ended). */
static gint64
add_download(
	Fixture     *fx,
	const gchar *state,
	const gchar *when
){
	fx->made++;
	return qi(fx, "INSERT INTO download (client_name, client_ref, protocol, source, release_name, module, state,"
		" rel_path, finished_at) VALUES ('Made-up client', 'ref-%u', 'usenet', 'file',"
		" 'Made.Up.Harbour.Watch.S01E01.%u.1080p.WEB-DL-GRP', 'tv', '%s', 'downloads/made-up-%u', %s) RETURNING id",
		fx->made, fx->made, state, fx->made, when != NULL ? when : "NULL");
}

/* A grab of the series' one episode (or of the movie), closed @when (SQL;
 * NULL: open), of @download (0: none). */
static gint64
add_grab(
	Fixture     *fx,
	gboolean     movie,
	const gchar *when,
	gint64       download
){
	g_autofree gchar *download_text = download > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, download) :
		g_strdup("NULL");
	gint64 id;

	fx->made++;
	if (movie)
		return qi(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, closed_at,"
			" close_reason) VALUES (%" G_GINT64_FORMAT ", 'Made.Up.Lantern.Night.%u.1080p.WEB-DL-GRP', 'usenet',"
			" 'made-up-%u', %s, %s, %s) RETURNING id", fx->movie_id, fx->made, fx->made, download_text,
			when != NULL ? when : "NULL", when != NULL ? "'failed'" : "NULL");
	id = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, download_id, closed_at,"
		" close_reason, scope, episode_id) VALUES (%" G_GINT64_FORMAT ", 'Made.Up.Harbour.Watch.S01E01.%u.1080p.WEB-DL-GRP',"
		" 'usenet', 'made-up-%u', %s, %s, %s, 'episode', %" G_GINT64_FORMAT ") RETURNING id", fx->series_id,
		fx->made, fx->made, download_text, when != NULL ? when : "NULL", when != NULL ? "'failed'" : "NULL",
		fx->episode_id);
	sql(fx, "INSERT INTO series_grab_episode (grab_id, episode_id, open) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", %s)", id, fx->episode_id, when != NULL ? "false" : "true");
	return id;
}

/* An import for the movie (@module "movies") or the series ("tv"), in
 * @state, ended @when (SQL; NULL for the states that are not ended), with a
 * file, an action and an assignment of its own. */
static gint64
add_import(
	Fixture     *fx,
	const gchar *module,
	const gchar *state,
	const gchar *when,
	gint64       download,
	gint64       grab
){
	g_autofree gchar *download_text = download > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, download) :
		g_strdup("NULL");
	g_autofree gchar *grab_text = grab > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, grab) : g_strdup("NULL");
	gint64 id;

	fx->made++;
	id = qi(fx, "INSERT INTO import (module, owner_id, title_label, source_path, state, download_id, grab_id,"
		" finished_at) VALUES ('%s', %" G_GINT64_FORMAT ", 'Made-up title', 'downloads/made-up-%u', '%s', %s, %s, %s)"
		" RETURNING id", module, strcmp(module, "tv") == 0 ? fx->series_id : fx->movie_id, fx->made, state,
		download_text, grab_text, when != NULL ? when : "NULL");
	sql(fx, "INSERT INTO import_file (import_id, position, path, kind) VALUES (%" G_GINT64_FORMAT ", 0,"
		" 'made-up.mkv', 'video')", id);
	sql(fx, "INSERT INTO import_action (import_id, action, by_user) VALUES (%" G_GINT64_FORMAT ", 'dismiss',"
		" 'root')", id);
	sql(fx, "INSERT INTO import_assignment (import_id, position, path, by_user) VALUES (%" G_GINT64_FORMAT ", 0,"
		" 'made-up.mkv', 'root')", id);
	return id;
}

/* A file of the library, for the series or the movie, that came by
 * @import. */
static void
add_library_file(
	Fixture  *fx,
	gboolean  movie,
	gint64    import
){
	gint64 file;

	fx->made++;
	file = qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'made-up/%u.mkv', 'video', 1000, 1, 0, 1, 0, %u, 1, false, false, false, 1, 1, 1) RETURNING id",
		fx->root_id, fx->volume_id, fx->made, 7000 + fx->made);
	if (movie)
		sql(fx, "INSERT INTO movie_file (file_id, movie_id, import_id) VALUES (%" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")", file, fx->movie_id, import);
	else
		sql(fx, "INSERT INTO series_file (file_id, series_id, import_id) VALUES (%" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")", file, fx->series_id, import);
}

/* ── Running ────────────────────────────────────────────────────── */

static void
run(
	Fixture           *fx,
	AxisHistoryReport *report
){
	g_autoptr(GError) error = NULL;
	AxisHistoryKeep keep;

	axis_history_read_keep(fx->history, &keep);
	g_assert_true(axis_history_prune(fx->history, fx->pool, &keep, NULL, report, &error));
	g_assert_no_error(error);
}

static gboolean
exists(
	Fixture     *fx,
	const gchar *table,
	gint64       id
){
	return qi(fx, "SELECT count(*) FROM %s WHERE id = %" G_GINT64_FORMAT, table, id) == 1;
}

/* ── Events ─────────────────────────────────────────────────────── */

/* Beyond the latest 30 of the series, old events go; the latest 30 stay,
 * old or not. */
static void
test_events_old_ones_go(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 first = add_event(fx, FALSE, TRUE, "searched", NULL, 0, OLD);

	add_events(fx, FALSE, TRUE, 39, OLD);
	run(fx, &report);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_SERIES_EVENTS], ==, 10);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event"), ==, AXIS_HISTORY_EVENTS_KEPT);
	g_assert_false(exists(fx, "series_event", first));
	g_assert_cmpint(qi(fx, "SELECT min(id) FROM series_event"), ==, first + 10);

	/* The movie's the same way. */
	first = add_event(fx, TRUE, TRUE, "searched", NULL, 0, OLD);
	add_events(fx, TRUE, TRUE, 34, OLD);
	run(fx, &report);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_MOVIE_EVENTS], ==, 5);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event"), ==, AXIS_HISTORY_EVENTS_KEPT);
	g_assert_false(exists(fx, "movie_event", first));
}

/* A title's page never loses what it shows: its 30 latest stay. */
static void
test_events_latest_of_a_title_kept(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;

	add_events(fx, FALSE, TRUE, AXIS_HISTORY_EVENTS_KEPT, OLD);
	add_events(fx, TRUE, TRUE, AXIS_HISTORY_EVENTS_KEPT, OLD);
	run(fx, &report);
	g_assert_cmpuint(report.examined[AXIS_HISTORY_SERIES_EVENTS], ==, AXIS_HISTORY_EVENTS_KEPT);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_SERIES_EVENTS], ==, 0);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_MOVIE_EVENTS], ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event"), ==, AXIS_HISTORY_EVENTS_KEPT);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event"), ==, AXIS_HISTORY_EVENTS_KEPT);
}

/* When a title was added or removed is the only record of it. */
static void
test_events_added_and_removed_kept(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 added = add_event(fx, FALSE, TRUE, "added", NULL, 0, OLD);
	gint64 removed = add_event(fx, TRUE, FALSE, "removed", NULL, 0, OLD);
	gint64 movie_added = add_event(fx, TRUE, TRUE, "added", NULL, 0, OLD);

	add_events(fx, FALSE, TRUE, 40, OLD);
	add_events(fx, TRUE, FALSE, 40, OLD);
	add_events(fx, TRUE, TRUE, 40, OLD);
	run(fx, &report);
	g_assert_true(exists(fx, "series_event", added));
	g_assert_true(exists(fx, "movie_event", removed));
	g_assert_true(exists(fx, "movie_event", movie_added));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_SERIES_EVENTS], ==, 10);
}

/* What a person did is kept. */
static void
test_events_of_a_person_kept(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 series = add_event(fx, FALSE, TRUE, "changed", "root", 0, OLD);
	gint64 movie = add_event(fx, TRUE, TRUE, "blocklisted", "root", 0, OLD);

	add_events(fx, FALSE, TRUE, 40, OLD);
	add_events(fx, TRUE, TRUE, 40, OLD);
	run(fx, &report);
	g_assert_true(exists(fx, "series_event", series));
	g_assert_true(exists(fx, "movie_event", movie));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_MOVIE_EVENTS], ==, 10);
}

/* Younger than the setting: kept. */
static void
test_events_recent_kept(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;

	add_events(fx, FALSE, TRUE, 40, RECENT);
	add_events(fx, TRUE, TRUE, 40, RECENT);
	run(fx, &report);
	g_assert_cmpuint(report.examined[AXIS_HISTORY_SERIES_EVENTS], ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event"), ==, 40);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event"), ==, 40);
}

/* A title that is not managed any more keeps its 30 latest too, by its
 * title (the page of a removed movie reads those); an event of no title at
 * all has no floor. */
static void
test_events_of_a_removed_title(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 orphan;

	add_events(fx, FALSE, FALSE, 35, OLD);
	add_events(fx, TRUE, FALSE, 32, OLD);
	orphan = add_event(fx, FALSE, FALSE, "searched", NULL, 0, OLD);
	sql(fx, "UPDATE series_event SET title_id = NULL WHERE id = %" G_GINT64_FORMAT, orphan);
	run(fx, &report);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE title_id IS NOT NULL"), ==,
		AXIS_HISTORY_EVENTS_KEPT);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event"), ==, AXIS_HISTORY_EVENTS_KEPT);
	g_assert_false(exists(fx, "series_event", orphan));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_SERIES_EVENTS], ==, 6);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_MOVIE_EVENTS], ==, 2);
}

/* ── Grabs ──────────────────────────────────────────────────────── */

/* An old closed grab, older than its title's five latest, with nothing
 * naming it: it goes, with the episodes it was for.  Then its download,
 * which nothing names any more, goes in the same run. */
static void
test_grabs_old_one_goes(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 download = add_download(fx, "failed", OLD);
	gint64 series = add_grab(fx, FALSE, OLD, download);
	gint64 movie = add_grab(fx, TRUE, OLD, 0);
	guint i;

	for (i = 0; i < AXIS_HISTORY_GRABS_KEPT; i++) {
		add_grab(fx, FALSE, OLD, 0);
		add_grab(fx, TRUE, OLD, 0);
	}
	run(fx, &report);
	g_assert_false(exists(fx, "series_grab", series));
	g_assert_false(exists(fx, "movie_grab", movie));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab_episode WHERE grab_id = %" G_GINT64_FORMAT, series), ==,
		0);
	g_assert_false(exists(fx, "download", download));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_SERIES_GRABS], ==, 1);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_MOVIE_GRABS], ==, 1);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, AXIS_HISTORY_GRABS_KEPT);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab"), ==, AXIS_HISTORY_GRABS_KEPT);
}

/* One rule of what keeps a grab, applied to the oldest of six old grabs of
 * the series and of the movie (the oldest, which /history/grabs/old-one-goes
 * shows would go). */
typedef struct
{
	const gchar *name;
	void       (*apply)(Fixture *fx, gboolean movie, gint64 grab);
} GrabRule;

static void
grab_open(Fixture *fx, gboolean movie, gint64 grab)
{
	sql(fx, "UPDATE %s SET closed_at = NULL, close_reason = NULL WHERE id = %" G_GINT64_FORMAT,
		movie ? "movie_grab" : "series_grab", grab);
}

static void
grab_recent(Fixture *fx, gboolean movie, gint64 grab)
{
	sql(fx, "UPDATE %s SET closed_at = " RECENT " WHERE id = %" G_GINT64_FORMAT, movie ? "movie_grab" : "series_grab",
		grab);
}

static void
grab_removal_pending(Fixture *fx, gboolean movie, gint64 grab)
{
	sql(fx, "UPDATE %s SET removal = 'pending' WHERE id = %" G_GINT64_FORMAT, movie ? "movie_grab" : "series_grab",
		grab);
}

static void
grab_removal_failed(Fixture *fx, gboolean movie, gint64 grab)
{
	sql(fx, "UPDATE %s SET removal = 'failed' WHERE id = %" G_GINT64_FORMAT, movie ? "movie_grab" : "series_grab",
		grab);
}

static void
grab_blocked(Fixture *fx, gboolean movie, gint64 grab)
{
	if (movie)
		sql(fx, "INSERT INTO movie_blocklist (movie_id, key, name_key, release_name, reason, grab_id) VALUES (%"
			G_GINT64_FORMAT ", 'name:made-up', 'made-up', 'Made.Up.Release', 'failed', %" G_GINT64_FORMAT ")",
			fx->movie_id, grab);
	else
		sql(fx, "INSERT INTO series_blocklist (series_id, key, name_key, release_name, reason, grab_id) VALUES (%"
			G_GINT64_FORMAT ", 'name:made-up', 'made-up', 'Made.Up.Release', 'failed', %" G_GINT64_FORMAT ")",
			fx->series_id, grab);
}

static void
grab_named_by_event(Fixture *fx, gboolean movie, gint64 grab)
{
	add_event(fx, movie, TRUE, "grabbed", NULL, grab, RECENT);
}

static void
grab_named_by_import(Fixture *fx, gboolean movie, gint64 grab)
{
	add_import(fx, movie ? "movies" : "tv", "dismissed", RECENT, 0, grab);
}

static void
grab_download_imported(Fixture *fx, gboolean movie, gint64 grab)
{
	gint64 download = add_download(fx, "imported", OLD);

	sql(fx, "UPDATE %s SET download_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT,
		movie ? "movie_grab" : "series_grab", download, grab);
	add_import(fx, movie ? "movies" : "tv", "imported", RECENT, download, 0);
}

static void
grab_download_at_client(Fixture *fx, gboolean movie, gint64 grab)
{
	gint64 download = add_download(fx, "completed", NULL);

	sql(fx, "UPDATE %s SET download_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT,
		movie ? "movie_grab" : "series_grab", download, grab);
}

static void
grab_download_folder_pending(Fixture *fx, gboolean movie, gint64 grab)
{
	gint64 download = add_download(fx, "removed", OLD);

	sql(fx, "UPDATE download SET folder_removal = 'waiting' WHERE id = %" G_GINT64_FORMAT, download);
	sql(fx, "UPDATE %s SET download_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT,
		movie ? "movie_grab" : "series_grab", download, grab);
}

/* (A series' grab only; a movie's grab with the same record goes.) */
static void
grab_left_holes(Fixture *fx, gboolean movie, gint64 grab)
{
	sql(fx, "UPDATE %s SET close_reason = 'imported', close_text = '" AXIS_TV_GRAB_LEFT_HOLES " S01E02'"
		" WHERE id = %" G_GINT64_FORMAT, movie ? "movie_grab" : "series_grab", grab);
}

static const GrabRule grab_rules[] = {
	{ "open", grab_open },
	{ "recent", grab_recent },
	{ "removal-pending", grab_removal_pending },
	{ "removal-failed", grab_removal_failed },
	{ "on-the-blocklist", grab_blocked },
	{ "named-by-an-event", grab_named_by_event },
	{ "named-by-an-import", grab_named_by_import },
	{ "its-download-has-an-import", grab_download_imported },
	{ "its-download-at-its-client", grab_download_at_client },
	{ "its-download-folder-pending", grab_download_folder_pending },
	{ "left-episodes-without-a-file", grab_left_holes }
};

static void
test_grab_kept(Fixture *fx, gconstpointer data)
{
	const GrabRule *rule = data;
	AxisHistoryReport report;
	gboolean left_holes = rule->apply == grab_left_holes;
	gint64 series;
	gint64 movie;
	guint i;

	/* (Registered by g_test_add(): skipped here without a database.) */
	if (fx->db == NULL)
		return;
	series = add_grab(fx, FALSE, OLD, 0);
	movie = add_grab(fx, TRUE, OLD, 0);
	for (i = 0; i < AXIS_HISTORY_GRABS_KEPT; i++) {
		add_grab(fx, FALSE, OLD, 0);
		add_grab(fx, TRUE, OLD, 0);
	}
	rule->apply(fx, FALSE, series);
	/* A movie's grab has no such record: it goes. */
	if (!left_holes)
		rule->apply(fx, TRUE, movie);
	run(fx, &report);
	g_assert_cmpuint(report.statements[AXIS_HISTORY_SERIES_GRABS], >=, 1);
	g_assert_true(exists(fx, "series_grab", series));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab_episode WHERE grab_id = %" G_GINT64_FORMAT, series), ==,
		1);
	g_assert_true(exists(fx, "movie_grab", movie) != left_holes);
}

/* The five latest of a title stay, however old. */
static void
test_grabs_latest_kept(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	guint i;

	for (i = 0; i < AXIS_HISTORY_GRABS_KEPT; i++) {
		add_grab(fx, FALSE, OLD, 0);
		add_grab(fx, TRUE, OLD, 0);
	}
	run(fx, &report);
	g_assert_cmpuint(report.examined[AXIS_HISTORY_SERIES_GRABS], ==, AXIS_HISTORY_GRABS_KEPT);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, AXIS_HISTORY_GRABS_KEPT);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab"), ==, AXIS_HISTORY_GRABS_KEPT);
}

/* ── Downloads ──────────────────────────────────────────────────── */

static void
test_downloads_old_one_goes(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 failed = add_download(fx, "failed", OLD);
	gint64 gone = add_download(fx, "gone", OLD);
	gint64 removed = add_download(fx, "removed", OLD);
	gint64 imported = add_download(fx, "imported", OLD);

	sql(fx, "UPDATE download SET folder_removal = 'done' WHERE id = %" G_GINT64_FORMAT, removed);
	run(fx, &report);
	g_assert_false(exists(fx, "download", failed));
	g_assert_false(exists(fx, "download", gone));
	g_assert_false(exists(fx, "download", removed));
	g_assert_false(exists(fx, "download", imported));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 4);
}

typedef struct
{
	const gchar *name;
	void       (*apply)(Fixture *fx, gint64 download);
} DownloadRule;

static void
download_live(Fixture *fx, gint64 download)
{
	static const gchar *const live[] = { "sending", "queued", "downloading", "paused", "completed" };
	guint i;

	/* (The state alone: a download that was failed once and is at its
	 * client again keeps its old finished_at.) */
	sql(fx, "UPDATE download SET state = 'downloading' WHERE id = %" G_GINT64_FORMAT, download);
	for (i = 0; i < G_N_ELEMENTS(live); i++) {
		gint64 other = add_download(fx, "failed", OLD);

		sql(fx, "UPDATE download SET state = '%s' WHERE id = %" G_GINT64_FORMAT, live[i], other);
	}
}

static void
download_recent(Fixture *fx, gint64 download)
{
	sql(fx, "UPDATE download SET finished_at = " RECENT " WHERE id = %" G_GINT64_FORMAT, download);
}

static void
download_folder_pending(Fixture *fx, gint64 download)
{
	gint64 other = add_download(fx, "gone", OLD);

	sql(fx, "UPDATE download SET folder_removal = 'pending' WHERE id = %" G_GINT64_FORMAT, download);
	sql(fx, "UPDATE download SET folder_removal = 'waiting' WHERE id = %" G_GINT64_FORMAT, other);
}

static void
download_named_by_series_grab(Fixture *fx, gint64 download)
{
	add_grab(fx, FALSE, RECENT, download);
}

static void
download_named_by_movie_grab(Fixture *fx, gint64 download)
{
	add_grab(fx, TRUE, RECENT, download);
}

static void
download_named_by_import(Fixture *fx, gint64 download)
{
	add_import(fx, "movies", "imported", RECENT, download, 0);
}

static const DownloadRule download_rules[] = {
	{ "at-its-client", download_live },
	{ "recent", download_recent },
	{ "folder-pending", download_folder_pending },
	{ "named-by-a-series-grab", download_named_by_series_grab },
	{ "named-by-a-movie-grab", download_named_by_movie_grab },
	{ "named-by-an-import", download_named_by_import }
};

static void
test_download_kept(Fixture *fx, gconstpointer data)
{
	const DownloadRule *rule = data;
	AxisHistoryReport report;
	gint64 download;

	if (fx->db == NULL)
		return;
	download = add_download(fx, "imported", OLD);
	rule->apply(fx, download);
	run(fx, &report);
	g_assert_true(exists(fx, "download", download));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, qi(fx, "SELECT max(id) - min(id) + 1 FROM download"));
}

/* ── Imports ────────────────────────────────────────────────────── */

/* An old import that ended, nothing naming it and not its title's latest:
 * it goes with its files, actions and assignments; the latest stays. */
static void
test_imports_old_one_goes(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 old = add_import(fx, "movies", "dismissed", OLD, 0, 0);
	gint64 imported = add_import(fx, "tv", "imported", OLD, 0, 0);
	gint64 latest = add_import(fx, "movies", "imported", OLD, 0, 0);
	gint64 latest_tv = add_import(fx, "tv", "dismissed", OLD, 0, 0);
	gint64 manual;

	manual = add_import(fx, "movies", "dismissed", OLD, 0, 0);
	sql(fx, "UPDATE import SET owner_id = NULL WHERE id = %" G_GINT64_FORMAT, manual);
	run(fx, &report);
	g_assert_false(exists(fx, "import", old));
	g_assert_false(exists(fx, "import", imported));
	g_assert_false(exists(fx, "import", manual));
	g_assert_true(exists(fx, "import", latest));
	g_assert_true(exists(fx, "import", latest_tv));
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_IMPORTS], ==, 3);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_file"), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_action"), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_assignment"), ==, 2);
}

typedef struct
{
	const gchar *name;
	void       (*apply)(Fixture *fx, gint64 import);
	gboolean     latest;	/* no later import of the movie is made */
} ImportRule;

static void
import_open(Fixture *fx, gint64 import)
{
	static const gchar *const open[] = { "waiting", "running", "blocked", "partial" };
	guint i;

	/* A series may have several open at once (one running); a partial one
	 * has ended, and is open all the same (a person retries it). */
	sql(fx, "UPDATE import SET module = 'tv', owner_id = %" G_GINT64_FORMAT ", state = 'partial' WHERE id = %"
		G_GINT64_FORMAT, fx->series_id, import);
	for (i = 0; i < G_N_ELEMENTS(open); i++)
		add_import(fx, "tv", open[i], g_strcmp0(open[i], "partial") == 0 ? OLD : NULL, 0, 0);
}

static void
import_recent(Fixture *fx, gint64 import)
{
	sql(fx, "UPDATE import SET finished_at = " RECENT " WHERE id = %" G_GINT64_FORMAT, import);
}

static void
import_series_file(Fixture *fx, gint64 import)
{
	add_library_file(fx, FALSE, import);
}

static void
import_movie_file(Fixture *fx, gint64 import)
{
	add_library_file(fx, TRUE, import);
}

static void
import_recycled(Fixture *fx, gint64 import)
{
	sql(fx, "INSERT INTO import_recycle (import_id, volume_id, original_path, recycle_path, inode, size, how, state)"
		" VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'library/made-up.mkv', '.axis-recycle/library/made-up.mkv',"
		" 1, 1, 'moved', 'removed')", import, fx->volume_id);
}

static void
import_client_removal(Fixture *fx, gint64 import)
{
	static const gchar *const states[] = { "failed", "kept" };
	guint i;

	sql(fx, "UPDATE import SET client_removal = 'pending' WHERE id = %" G_GINT64_FORMAT, import);
	for (i = 0; i < G_N_ELEMENTS(states); i++) {
		gint64 other = add_import(fx, "movies", "imported", OLD, 0, 0);

		sql(fx, "UPDATE import SET client_removal = '%s' WHERE id = %" G_GINT64_FORMAT, states[i], other);
	}
}

static void
import_folder_removal(Fixture *fx, gint64 import)
{
	gint64 other = add_import(fx, "movies", "imported", OLD, 0, 0);

	sql(fx, "UPDATE import SET folder_removal = 'pending' WHERE id = %" G_GINT64_FORMAT, import);
	sql(fx, "UPDATE import SET folder_removal = 'waiting' WHERE id = %" G_GINT64_FORMAT, other);
}

static void
import_temporary(Fixture *fx, gint64 import)
{
	gint64 other = add_import(fx, "movies", "imported", OLD, 0, 0);

	sql(fx, "UPDATE import_file SET temp_name = '.axis-partial-7', step = 'temp' WHERE import_id = %" G_GINT64_FORMAT,
		import);
	sql(fx, "UPDATE import_file SET temp_name = '.axis-partial-8', step = 'swapped' WHERE import_id = %"
		G_GINT64_FORMAT, other);
}

static void
import_copy_to_delete(Fixture *fx, gint64 import)
{
	sql(fx, "UPDATE import_file SET source_state = 'pending' WHERE import_id = %" G_GINT64_FORMAT, import);
}

static void
import_download_at_client(Fixture *fx, gint64 import)
{
	gint64 download = add_download(fx, "completed", NULL);

	sql(fx, "UPDATE import SET download_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT, download, import);
}

static void
import_download_folder(Fixture *fx, gint64 import)
{
	gint64 download = add_download(fx, "removed", OLD);

	sql(fx, "UPDATE download SET folder_removal = 'pending' WHERE id = %" G_GINT64_FORMAT, download);
	sql(fx, "UPDATE import SET download_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT, download, import);
}

/* (Nothing to do: the import is its movie's latest, as no later one is
 * made.) */
static void
import_latest(Fixture *fx, gint64 import)
{
}

static void
import_job(Fixture *fx, gint64 import)
{
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) VALUES ('import.run', 'imports',"
		" 'queued', 3, 'import:%" G_GINT64_FORMAT "')", import);
}

static const ImportRule import_rules[] = {
	{ "open", import_open, FALSE },
	{ "recent", import_recent, FALSE },
	{ "a-series-file-came-by-it", import_series_file, FALSE },
	{ "a-movie-file-came-by-it", import_movie_file, FALSE },
	{ "it-retired-a-file", import_recycled, FALSE },
	{ "work-left-at-the-client", import_client_removal, FALSE },
	{ "its-folder-to-look-at", import_folder_removal, FALSE },
	{ "a-temporary-name-in-the-library", import_temporary, FALSE },
	{ "a-copy-to-delete", import_copy_to_delete, FALSE },
	{ "its-download-at-its-client", import_download_at_client, FALSE },
	{ "its-download-folder-pending", import_download_folder, FALSE },
	{ "the-latest-of-its-title", import_latest, TRUE },
	{ "a-job-for-it", import_job, FALSE }
};

/* The import under test (and what the rule makes), then a later one of the
 * same movie, so that none of them is the latest; nothing is deleted. */
static void
test_import_kept(Fixture *fx, gconstpointer data)
{
	const ImportRule *rule = data;
	AxisHistoryReport report;
	gint64 import;
	gint64 before;

	if (fx->db == NULL)
		return;
	import = add_import(fx, "movies", "dismissed", OLD, 0, 0);
	rule->apply(fx, import);
	if (!rule->latest)
		add_import(fx, "movies", "dismissed", RECENT, 0, 0);
	before = qi(fx, "SELECT count(*) FROM import");
	run(fx, &report);
	g_assert_cmpuint(report.statements[AXIS_HISTORY_IMPORTS], >=, 1);
	g_assert_true(exists(fx, "import", import));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT, import), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import"), ==, before);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_IMPORTS], ==, 0);
}

/* ── One run after another ──────────────────────────────────────── */

/* An import names its grab and its download, and the grab its download:
 * one run deletes the import, then the grab, then the download. */
static void
test_chain_in_one_run(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	gint64 download = add_download(fx, "imported", OLD);
	gint64 grab = add_grab(fx, FALSE, OLD, download);
	gint64 import = add_import(fx, "tv", "dismissed", OLD, download, grab);
	guint i;

	add_import(fx, "tv", "dismissed", OLD, 0, 0);
	for (i = 0; i < AXIS_HISTORY_GRABS_KEPT; i++)
		add_grab(fx, FALSE, OLD, 0);
	run(fx, &report);
	g_assert_false(exists(fx, "import", import));
	g_assert_false(exists(fx, "series_grab", grab));
	g_assert_false(exists(fx, "download", download));
}

/* A statement looks at a batch at most, and a run sends a few: the rest
 * waits for the next run, which goes on where the last one stopped. */
static void
test_batches_are_bounded(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	guint i;

	for (i = 0; i < 25; i++)
		add_download(fx, "failed", OLD);
	axis_history_set_batch(fx->history, 10, 2);

	run(fx, &report);
	g_assert_cmpuint(report.statements[AXIS_HISTORY_DOWNLOADS], ==, 2);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 20);
	g_assert_true(report.more[AXIS_HISTORY_DOWNLOADS]);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 5);

	run(fx, &report);
	g_assert_cmpuint(report.statements[AXIS_HISTORY_DOWNLOADS], ==, 1);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 5);
	g_assert_false(report.more[AXIS_HISTORY_DOWNLOADS]);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 0);
}

/* Rows kept for good at the old end never stand in front of rows that may
 * go: each run looks further, and the sweep starts again once it has seen
 * every old row. */
static void
test_kept_rows_do_not_block(Fixture *fx, gconstpointer data)
{
	AxisHistoryReport report;
	guint i;

	/* 30 old downloads that recent grabs name, then 5 that nothing names. */
	for (i = 0; i < 30; i++) {
		gint64 download = add_download(fx, "failed", "now() - interval '500 days'");

		add_grab(fx, TRUE, RECENT, download);
	}
	for (i = 0; i < 5; i++)
		add_download(fx, "failed", OLD);
	axis_history_set_batch(fx->history, 10, 1);

	for (i = 0; i < 3; i++) {
		run(fx, &report);
		g_assert_cmpuint(report.examined[AXIS_HISTORY_DOWNLOADS], ==, 10);
		g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 0);
		g_assert_true(report.more[AXIS_HISTORY_DOWNLOADS]);
	}
	run(fx, &report);
	g_assert_cmpuint(report.examined[AXIS_HISTORY_DOWNLOADS], ==, 5);
	g_assert_cmpuint(report.deleted[AXIS_HISTORY_DOWNLOADS], ==, 5);
	g_assert_false(report.more[AXIS_HISTORY_DOWNLOADS]);
	/* The sweep starts from the oldest again. */
	run(fx, &report);
	g_assert_cmpuint(report.examined[AXIS_HISTORY_DOWNLOADS], ==, 10);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 30);
}

/* 0 keeps everything: nothing is even looked at. */
static void
test_zero_keeps_everything(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisHistoryReport report;
	AxisHistoryKeep keep;
	guint i;

	add_events(fx, FALSE, TRUE, 40, OLD);
	add_import(fx, "movies", "dismissed", OLD, 0, 0);
	add_import(fx, "movies", "dismissed", OLD, 0, 0);
	add_download(fx, "failed", OLD);
	for (i = 0; i < AXIS_HISTORY_GRABS_KEPT + 1; i++)
		add_grab(fx, TRUE, OLD, 0);
	memset(&keep, 0, sizeof(keep));
	g_assert_true(axis_history_prune(fx->history, fx->pool, &keep, NULL, &report, &error));
	g_assert_no_error(error);
	for (i = 0; i < AXIS_HISTORY_N_PARTS; i++)
		g_assert_cmpuint(report.statements[i], ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event"), ==, 40);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import"), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab"), ==, AXIS_HISTORY_GRABS_KEPT + 1);

	/* One kind at 0, the others not: only that kind stays. */
	keep.events_s = 0;
	keep.imports_s = keep.downloads_s = keep.grabs_s = 30 * 86400;
	g_assert_true(axis_history_prune(fx->history, fx->pool, &keep, NULL, &report, &error));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event"), ==, 40);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab"), ==, AXIS_HISTORY_GRABS_KEPT);
}

/* The series' record of a release that left holes begins as the TV module
 * writes it. */
static void
test_left_holes_words(void)
{
	g_assert_cmpstr(AXIS_HISTORY_TV_LEFT_HOLES, ==, AXIS_TV_GRAB_LEFT_HOLES);
}

/* ── The job and its settings ───────────────────────────────────── */

/* The schedule runs it at once; the ages are the settings', read at the
 * start of each run; a setting at 0 keeps its kind. */
static void
test_job_follows_the_settings(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = axis_settings_new();
	g_autoptr(AxisModuleRegistry) modules = axis_module_registry_new();
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(AxisHistory) history = axis_history_new(settings);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_module_registry_register(modules, axis_history_get_module(), history, &error));
	g_assert_true(axis_settings_add_modules(settings, modules, &error));
	g_assert_true(axis_settings_load(settings, fx->pool, &error));
	g_assert_no_error(error);

	/* The defaults, and the limits: 30 days at least, or 0. */
	g_assert_cmpint(axis_settings_get_duration(settings, AXIS_HISTORY_SETTING_KEEP_EVENTS), ==, 365 * 86400);
	g_assert_cmpint(axis_settings_get_duration(settings, AXIS_HISTORY_SETTING_KEEP_GRABS), ==, 365 * 86400);
	g_assert_cmpint(axis_settings_get_duration(settings, AXIS_HISTORY_SETTING_KEEP_DOWNLOADS), ==, 365 * 86400);
	g_assert_cmpint(axis_settings_get_duration(settings, AXIS_HISTORY_SETTING_KEEP_IMPORTS), ==, 365 * 86400);
	g_assert_false(axis_settings_set(settings, AXIS_HISTORY_SETTING_KEEP_EVENTS, "29d", &error));
	g_assert_nonnull(strstr(error->message, "must be 0 or at least 30d"));
	g_clear_error(&error);
	g_assert_false(axis_settings_set(settings, AXIS_HISTORY_SETTING_KEEP_IMPORTS, "3651d", &error));
	g_clear_error(&error);
	g_assert_true(axis_settings_set(settings, AXIS_HISTORY_SETTING_KEEP_EVENTS, "40d", &error));
	g_assert_true(axis_settings_set(settings, AXIS_HISTORY_SETTING_KEEP_DOWNLOADS, "0", &error));
	g_assert_no_error(error);

	add_events(fx, FALSE, TRUE, 35, "now() - interval '50 days'");
	add_download(fx, "failed", OLD);

	registry = axis_job_registry_new();
	g_assert_true(axis_job_registry_add_modules(registry, modules, &error));
	queue = axis_job_queue_new(fx->pool, registry);
	axis_job_queue_set_poll_interval(queue, 100);
	axis_job_queue_set_schedule_interval(queue, 100);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	g_assert_true(axis_test_wait_query(fx->db, "SELECT count(*) FROM job WHERE kind = '" AXIS_HISTORY_PRUNE_KIND "'"
		" AND state = 'succeeded'", "1", AXIS_TEST_WAIT_MS));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event"), ==, AXIS_HISTORY_EVENTS_KEPT);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job_schedule WHERE name = '" AXIS_HISTORY_PRUNE_SCHEDULE "'"
		" AND enabled AND run_interval = interval '1 hour'"), ==, 1);

	/* A change applies to the next run, with no restart. */
	g_assert_true(axis_settings_set(settings, AXIS_HISTORY_SETTING_KEEP_DOWNLOADS, "30d", &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_queue_enqueue(queue, AXIS_HISTORY_PRUNE_KIND, NULL, NULL, &id, NULL, &error));
	g_assert_true(axis_test_wait_query(fx->db, "SELECT count(*) FROM job WHERE kind = '" AXIS_HISTORY_PRUNE_KIND "'"
		" AND state = 'succeeded'", "2", AXIS_TEST_WAIT_MS));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download"), ==, 0);

	axis_job_queue_stop(queue, 5000);
	g_assert_true(axis_job_queue_wait_threads(queue, AXIS_TEST_WAIT_MS));
}

/* ── The Settings page ──────────────────────────────────────────── */

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisHistory        *history;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	gchar              *admin;
} Page;

static void
page_setup(Page *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	fx->history = axis_history_new(fx->settings);
	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_history_get_module(), fx->history, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
}

static void
page_teardown(Page *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_history_free(fx->history);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_free(fx->admin);
	axis_test_log_reset();
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack);
}

static AxisTestReply
post_history(
	Page        *fx,
	const gchar *events,
	const gchar *grabs,
	const gchar *downloads,
	const gchar *imports
){
	AxisTestReply page = axis_test_web_request(fx->session, fx->port, "GET", "/admin/settings", fx->admin, NULL,
		NULL);
	g_autofree gchar *token = axis_test_web_csrf_token(page.body);
	g_autofree gchar *form = NULL;
	const gchar *extra[3];

	g_assert_nonnull(token);
	axis_test_reply_clear(&page);
	form = g_strdup_printf(AXIS_HISTORY_SETTING_KEEP_EVENTS "=%s&" AXIS_HISTORY_SETTING_KEEP_GRABS "=%s&"
		AXIS_HISTORY_SETTING_KEEP_DOWNLOADS "=%s&" AXIS_HISTORY_SETTING_KEEP_IMPORTS "=%s", events, grabs, downloads,
		imports);
	extra[0] = "X-CSRF-Token";
	extra[1] = token;
	extra[2] = NULL;
	return axis_test_web_request(fx->session, fx->port, "POST", "/admin/settings/" AXIS_HISTORY_MODULE_ID, fx->admin,
		extra, form);
}

/* The four settings are a card of their own on the Settings page, with
 * their defaults, and are saved there: 0 and a value within the limits are
 * taken, a value under 30 days is refused with the reason. */
static void
test_settings_page(Page *fx, gconstpointer data)
{
	AxisTestReply reply;

	if (fx->db == NULL)
		return;
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/settings", fx->admin, NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "History settings");
	assert_contains(reply.body, "action=\"/admin/settings/history\"");
	assert_contains(reply.body, "Keep the history of what happened to movies and series for");
	assert_contains(reply.body, "Keep the record of each release grabbed for");
	assert_contains(reply.body, "Keep finished downloads on the Downloads page for");
	assert_contains(reply.body, "Keep finished imports on the Imports page for");
	assert_contains(reply.body, "name=\"history.keep_events\"");
	assert_contains(reply.body, "value=\"365d\"");
	assert_contains(reply.body, "Default: 365d.");
	assert_contains(reply.body, "0: kept forever.");
	axis_test_reply_clear(&reply);

	reply = post_history(fx, "0", "60d", "365d", "400d");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_nonnull(strstr(axis_test_reply_header(&reply, "Location"), "done=saved"));
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_HISTORY_SETTING_KEEP_EVENTS), ==, 0);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_HISTORY_SETTING_KEEP_GRABS), ==, 60 * 86400);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_HISTORY_SETTING_KEEP_IMPORTS), ==, 400 * 86400);
	g_assert_true(axis_test_wait_query(fx->db, "SELECT value::text FROM setting WHERE key = 'history.keep_events'",
		"0", AXIS_TEST_WAIT_MS));

	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/settings", fx->admin, NULL, NULL);
	assert_contains(reply.body, "value=\"0s\"");
	assert_contains(reply.body, "value=\"60d\"");
	axis_test_reply_clear(&reply);

	/* Under the minimum: nothing is saved, and the field says why. */
	reply = post_history(fx, "10d", "60d", "365d", "400d");
	g_assert_cmpuint(reply.status, !=, 303);
	assert_contains(reply.body, "must be 0 or at least 30d");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_HISTORY_SETTING_KEEP_EVENTS), ==, 0);
}

/* ── Registration ───────────────────────────────────────────────── */

#define ADD(name, func) AXIS_TEST_ADD_DB_CASE("/history/" name, Fixture, NULL, func)

int
main(int argc, char **argv)
{
	guint i;

	g_test_init(&argc, &argv, NULL);

	ADD("events/old-ones-go", test_events_old_ones_go);
	ADD("events/kept/the-latest-of-a-title", test_events_latest_of_a_title_kept);
	ADD("events/kept/added-and-removed", test_events_added_and_removed_kept);
	ADD("events/kept/what-a-person-did", test_events_of_a_person_kept);
	ADD("events/kept/recent", test_events_recent_kept);
	ADD("events/of-a-removed-title", test_events_of_a_removed_title);

	ADD("grabs/old-one-goes", test_grabs_old_one_goes);
	ADD("grabs/kept/the-latest-of-a-title", test_grabs_latest_kept);
	for (i = 0; i < G_N_ELEMENTS(grab_rules); i++) {
		g_autofree gchar *path = g_strdup_printf("/history/grabs/kept/%s", grab_rules[i].name);

		g_test_add(path, Fixture, &grab_rules[i], fixture_setup, test_grab_kept, fixture_teardown);
	}

	ADD("downloads/old-one-goes", test_downloads_old_one_goes);
	for (i = 0; i < G_N_ELEMENTS(download_rules); i++) {
		g_autofree gchar *path = g_strdup_printf("/history/downloads/kept/%s", download_rules[i].name);

		g_test_add(path, Fixture, &download_rules[i], fixture_setup, test_download_kept, fixture_teardown);
	}

	ADD("imports/old-one-goes", test_imports_old_one_goes);
	for (i = 0; i < G_N_ELEMENTS(import_rules); i++) {
		g_autofree gchar *path = g_strdup_printf("/history/imports/kept/%s", import_rules[i].name);

		g_test_add(path, Fixture, &import_rules[i], fixture_setup, test_import_kept, fixture_teardown);
	}

	ADD("runs/a-chain-in-one-run", test_chain_in_one_run);
	ADD("runs/batches-are-bounded", test_batches_are_bounded);
	ADD("runs/kept-rows-do-not-block", test_kept_rows_do_not_block);
	ADD("runs/zero-keeps-everything", test_zero_keeps_everything);
	g_test_add_func("/history/left-holes-words", test_left_holes_words);
	ADD("job/follows-the-settings", test_job_follows_the_settings);
	g_test_add("/history/settings-page", Page, NULL, page_setup, test_settings_page, page_teardown);

	return g_test_run();
}
