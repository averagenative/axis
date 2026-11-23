/*
 * test-compat-notify.c - telling the media servers which folder changed
 * (add-library-notify): what is recorded and merged, what Plex and Jellyfin
 * are sent, what is not, a server that is down, and where the secrets go
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The fake servers are the one of axis-test-plex.h: one stands for Plex
 * (the standard libraries Films /data/movies, Series /data/tv and
 * /data/tv-extra, Music /data/music) and one for Jellyfin (the recorded
 * libraries /media/films and /media/music).  The library is the savings
 * library, whose Films (movies) and Shows (tv) roots are what is imported
 * into.
 *
 * Spec scenarios of client-compatibility covered here: Import, No matching
 * library, Any other write (with test-http.c), Server down, Film with
 * subtitles (with test-import.c), and the secrets staying with their server.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-compat.h"
#include "compat/axis-compat-notify.h"
#include "core/axis-http.h"

#define F_PLEX 1
#define F_JELLYFIN 2
#define F_MAP_PLEX 4
#define F_MAP_JELLYFIN 8

typedef struct
{
	AxisTestCompat *t;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	gint flags = GPOINTER_TO_INT(data);

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the media server is not the one*");
	fx->t = axis_test_compat_new(TRUE);
	if (fx->t == NULL)
		return;
	axis_test_compat_enable_jellyfin(fx->t);
	if ((flags & F_PLEX) != 0)
		axis_test_compat_connect(fx->t);
	if ((flags & F_JELLYFIN) != 0)
		axis_test_compat_connect_jellyfin(fx->t);
	if ((flags & F_MAP_PLEX) != 0)
		axis_test_compat_map_standard(fx->t);
	if ((flags & F_MAP_JELLYFIN) != 0)
		axis_test_compat_map_jellyfin_standard(fx->t);
	/* What the servers answer to being told. */
	axis_test_plex_route_full(fx->t->plex, "/library/sections/1/refresh", 200, NULL, NULL, NULL);
	axis_test_plex_route_full(fx->t->plex, "/library/sections/2/refresh", 200, NULL, NULL, NULL);
	axis_test_plex_route_full(fx->t->plex, "/library/sections/3/refresh", 200, NULL, NULL, NULL);
	axis_test_plex_route_full(fx->t->jellyfin, "/Library/Media/Updated", 204, NULL, NULL, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_compat_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static gchar *
folder_in(
	Fixture     *fx,
	const gchar *root,
	const gchar *name
){
	return g_build_filename(fx->t->s.disk_a, root, name, NULL);
}

static void
record(
	Fixture          *fx,
	const gchar      *folder,
	AxisCompatChange  change
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_notify_record(fx->t->lib->pool, fx->t->settings, folder, change, &error));
	g_assert_no_error(error);
}

static AxisCompatNotifySummary
run(
	Fixture  *fx,
	gboolean  force
){
	AxisCompatNotifySummary summary;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_notify_run(fx->t->compat, force, NULL, &summary, &error));
	g_assert_no_error(error);
	return summary;
}

static gchar *
one(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
one(
	Fixture     *fx,
	const gchar *format,
	...
){
	va_list ap;
	g_autofree gchar *sql = NULL;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->t->lib->db, sql);
}

/* A query's one answer, compared and freed. */
#define assert_one(fx, expected, ...) \
	G_STMT_START { \
		g_autofree gchar *got_ = one((fx), __VA_ARGS__); \
		g_assert_cmpstr(got_, ==, (expected)); \
	} G_STMT_END

#define assert_one_prefix(fx, prefix, ...) \
	G_STMT_START { \
		g_autofree gchar *got_ = one((fx), __VA_ARGS__); \
		g_assert_nonnull(got_); \
		if (!g_str_has_prefix(got_, (prefix))) \
			g_error("expected a prefix %s, got %s", (prefix), got_); \
	} G_STMT_END

/* The servers' libraries and devices, as the poll reads them. */
static void
collect(Fixture *fx)
{
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_no_error(error);
}

static void
due_now(Fixture *fx)
{
	axis_test_db_exec(fx->t->lib->db,
		"UPDATE compat_notify SET due_at = now() - interval '1 second' WHERE state = 'pending'");
}

/* How many requests of the fake @server were for a path that holds @part. */
static guint
requests_with(
	AxisTestPlex *server,
	const gchar  *part
){
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(server);
	guint n = 0;
	guint i;

	for (i = 0; i < requests->len; i++) {
		const gchar *text = g_ptr_array_index(requests, i);
		const gchar *line_end = strchr(text, '\n');
		g_autofree gchar *line = g_strndup(text, line_end != NULL ? (gsize)(line_end - text) : strlen(text));

		if (strstr(line, part) != NULL)
			n++;
	}
	return n;
}

/* Every line of the fake's requests: the first line of each. */
static gchar *
request_lines(AxisTestPlex *server)
{
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(server);
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < requests->len; i++) {
		const gchar *text = g_ptr_array_index(requests, i);
		const gchar *line_end = strchr(text, '\n');

		g_string_append_len(out, text, line_end != NULL ? (gssize)(line_end - text) : (gssize)strlen(text));
		g_string_append_c(out, '\n');
	}
	return g_string_free(out, FALSE);
}

/* Neither secret is in the notification table, the job table, a log line, or
 * the other server's requests: each reached only its own. */
static void
assert_secrets_stay(Fixture *fx)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_CRITICAL
	};
	g_autofree gchar *plex_log = g_strconcat("*", AXIS_TEST_COMPAT_TOKEN, "*", NULL);
	g_autofree gchar *jellyfin_log = g_strconcat("*", AXIS_TEST_COMPAT_JELLYFIN_KEY, "*", NULL);
	g_autofree gchar *on_plex = axis_test_plex_transcript(fx->t->plex);
	g_autofree gchar *on_jellyfin = axis_test_plex_transcript(fx->t->jellyfin);
	guint i;

	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify n", "to_jsonb(n)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"),
		==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify n",
		"to_jsonb(n)::text LIKE '%" AXIS_TEST_COMPAT_JELLYFIN_KEY "%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "job j", "to_jsonb(j)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "job j", "to_jsonb(j)::text LIKE '%" AXIS_TEST_COMPAT_JELLYFIN_KEY "%'"),
		==, 0);
	for (i = 0; i < G_N_ELEMENTS(levels); i++) {
		g_assert_cmpuint(axis_test_log_count(levels[i], plex_log), ==, 0);
		g_assert_cmpuint(axis_test_log_count(levels[i], jellyfin_log), ==, 0);
	}
	g_assert_null(strstr(on_plex, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	g_assert_null(strstr(on_jellyfin, AXIS_TEST_COMPAT_TOKEN));
}

/* ── Plex ───────────────────────────────────────────────────────── */

/* Import: the section whose location holds the folder, and the refresh
 * limited to the folder as Plex sees it, as a GET with the token in its
 * header; nothing before the folder has been quiet for its time. */
static void
test_plex_is_told_the_folder_of_its_section(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *movie = folder_in(fx, "Films", "Hearth (1995) {tmdb-6001}");
	g_autofree gchar *series = folder_in(fx, "Shows", "Harbour Lights (2024)");
	g_autofree gchar *lines = NULL;
	g_autofree gchar *token = NULL;
	AxisCompatNotifySummary summary;

	record(fx, movie, AXIS_COMPAT_CHANGE_CREATED);
	record(fx, series, AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending' AND server_kind = 'plex'"), ==, 2);

	/* Quiet first: nothing is due, nothing is asked. */
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 0);
	g_assert_cmpuint(requests_with(fx->t->plex, "/refresh"), ==, 0);

	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 2);
	g_assert_cmpuint(summary.unmapped, ==, 0);
	/* Each folder to the section that holds it, as the server names it. */
	lines = request_lines(fx->t->plex);
	g_assert_nonnull(strstr(lines, "GET /library/sections/1/refresh?path=%2Fdata%2Fmovies%2FHearth%20%281995%29%20%7Btmdb-6001%7D\n"));
	g_assert_nonnull(strstr(lines, "GET /library/sections/2/refresh?path=%2Fdata%2Ftv%2FHarbour%20Lights%20%282024%29\n"));
	g_assert_cmpuint(requests_with(fx->t->plex, "/refresh"), ==, 2);
	token = axis_test_plex_last_header(fx->t->plex, "/library/sections/1/refresh", "X-Plex-Token");
	g_assert_cmpstr(token, ==, AXIS_TEST_COMPAT_TOKEN);
	g_assert_false(axis_test_plex_saw_non_get(fx->t->plex));
	/* The record says what it asked for. */
	assert_one(fx, "sent|1|/data/movies/Hearth (1995) {tmdb-6001}",
		"SELECT state || '|' || attempts || '|' || sent_folder FROM compat_notify WHERE folder = '%s'", movie);
	/* Nothing asked a second time. */
	axis_test_plex_reset_requests(fx->t->plex);
	summary = run(fx, TRUE);
	g_assert_cmpuint(summary.sent, ==, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->plex), ==, 0);
	assert_secrets_stay(fx);
}

/* No matching library: nothing is sent to Plex, never a scan of every section,
 * and the root is shown as not mapped to a Plex library. */
static void
test_a_folder_no_plex_library_holds_is_not_sent(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *films = g_build_filename(fx->t->s.disk_a, "Films", NULL);
	g_autofree gchar *movie = folder_in(fx, "Films", "Hearth (1995)");
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GError) error = NULL;
	AxisCompatNotifySummary summary;
	guint i;
	gboolean found = FALSE;

	/* Axis's Films is /unlisted/films for Plex: no library of its holds that. */
	axis_test_compat_map(fx->t, "/unlisted/films", films);
	record(fx, movie, AXIS_COMPAT_CHANGE_CREATED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 0);
	g_assert_cmpuint(summary.unmapped, ==, 1);
	g_assert_cmpuint(requests_with(fx->t->plex, "/refresh"), ==, 0);
	g_assert_cmpuint(requests_with(fx->t->plex, "/library/sections/all"), ==, 0);
	assert_one_prefix(fx, "unmapped|Plex has no library whose folders hold /unlisted/films/Hearth (1995)",
		"SELECT state || '|' || note FROM compat_notify WHERE folder = '%s'", movie);
	/* Only the libraries were read. */
	g_assert_false(axis_test_plex_saw_non_get(fx->t->plex));

	g_assert_true(axis_compat_notify_roots(fx->t->compat, &roots, &error));
	g_assert_no_error(error);
	for (i = 0; i < roots->len; i++) {
		const AxisCompatNotifyRoot *r = g_ptr_array_index(roots, i);

		if (g_strcmp0(r->root_name, "Films") == 0) {
			found = TRUE;
			g_assert_cmpstr(r->server_kind, ==, "plex");
			g_assert_cmpint(r->reach, ==, 1);
			g_assert_cmpstr(r->server_path, ==, "/unlisted/films");
			g_assert_null(r->library);
		}
	}
	g_assert_true(found);
}

/* No path mapping at all reaches the folder: not sent either. */
static void
test_a_folder_the_path_map_does_not_reach_is_not_sent(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *movie = folder_in(fx, "Films", "Hearth (1995)");
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GError) error = NULL;
	AxisCompatNotifySummary summary;
	guint i;

	record(fx, movie, AXIS_COMPAT_CHANGE_CREATED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.unmapped, ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->plex), ==, 0);
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify");
	assert_one_prefix(fx, "No path mapping reaches this folder", "SELECT note FROM compat_notify");
	g_assert_true(axis_compat_notify_roots(fx->t->compat, &roots, &error));
	g_assert_cmpuint(roots->len, >=, 2);	/* Films and Shows, and any other root of movies or TV */
	for (i = 0; i < roots->len; i++)
		g_assert_cmpint(((const AxisCompatNotifyRoot *)g_ptr_array_index(roots, i))->reach, ==, 0);
}

/* A library holds the root as a whole, or a part of it: told. */
static void
test_the_page_lists_what_each_root_reaches(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GError) error = NULL;
	guint seen = 0;
	guint i;

	collect(fx);
	g_assert_true(axis_compat_notify_roots(fx->t->compat, &roots, &error));
	g_assert_no_error(error);
	for (i = 0; i < roots->len; i++) {
		const AxisCompatNotifyRoot *r = g_ptr_array_index(roots, i);

		if (g_strcmp0(r->server_kind, "plex") == 0 && g_strcmp0(r->root_name, "Films") == 0) {
			g_assert_cmpint(r->reach, ==, 2);
			g_assert_cmpstr(r->library, ==, "Films");
			g_assert_cmpstr(r->server_path, ==, "/data/movies");
			seen++;
		} else if (g_strcmp0(r->server_kind, "plex") == 0 && g_strcmp0(r->root_name, "Shows") == 0) {
			g_assert_cmpint(r->reach, ==, 2);
			g_assert_cmpstr(r->library, ==, "Series");
			seen++;
		} else if (g_strcmp0(r->server_kind, "jellyfin") == 0 && g_strcmp0(r->root_name, "Films") == 0) {
			g_assert_cmpint(r->reach, ==, 2);
			g_assert_cmpstr(r->server_path, ==, "/media/films");
			seen++;
		} else if (g_strcmp0(r->server_kind, "jellyfin") == 0 && g_strcmp0(r->root_name, "Shows") == 0) {
			/* Jellyfin has no map for it. */
			g_assert_cmpint(r->reach, ==, 0);
			seen++;
		}
	}
	g_assert_cmpuint(seen, ==, 4);
}

/* ── Jellyfin ───────────────────────────────────────────────────── */

/* One request for the batch, with the folders under Jellyfin's names and what
 * happened to each, and the key in its header: the one write there is. */
static void
test_jellyfin_gets_one_request_for_the_batch(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995) {tmdb-6001}");
	g_autofree gchar *alder = folder_in(fx, "Films", "Alder (1979)");
	g_autofree gchar *gone = folder_in(fx, "Films", "Gone \"Q\" (1999)");
	g_autoptr(GPtrArray) requests = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *type = NULL;
	AxisCompatNotifySummary summary;
	const gchar *text;

	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	record(fx, alder, AXIS_COMPAT_CHANGE_MODIFIED);
	record(fx, gone, AXIS_COMPAT_CHANGE_DELETED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 3);
	requests = axis_test_plex_requests(fx->t->jellyfin);
	g_assert_cmpuint(requests->len, ==, 1);
	text = g_ptr_array_index(requests, 0);
	g_assert_true(g_str_has_prefix(text, "POST /Library/Media/Updated\n"));
	g_assert_nonnull(strstr(text, "\n\n{\"Updates\":["
		"{\"Path\":\"/media/films/Hearth (1995) {tmdb-6001}\",\"UpdateType\":\"Created\"},"
		"{\"Path\":\"/media/films/Alder (1979)\",\"UpdateType\":\"Modified\"},"
		"{\"Path\":\"/media/films/Gone \\\"Q\\\" (1999)\",\"UpdateType\":\"Deleted\"}]}\n"));
	key = axis_test_plex_last_header(fx->t->jellyfin, "/Library/Media/Updated", "Authorization");
	g_assert_cmpstr(key, ==, "MediaBrowser Token=\"" AXIS_TEST_COMPAT_JELLYFIN_KEY "\"");
	type = axis_test_plex_last_header(fx->t->jellyfin, "/Library/Media/Updated", "Content-Type");
	g_assert_cmpstr(type, ==, "application/json");
	/* No other request with a body, ever. */
	g_assert_cmpuint(requests_with(fx->t->jellyfin, "POST"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'sent'"), ==, 3);
	assert_secrets_stay(fx);
}

/* More folders than one request may hold are sent as more than one. */
static void
test_a_big_batch_is_split_at_the_cap(Fixture *fx, gconstpointer data)
{
	AxisCompatNotifySummary summary;
	g_autoptr(GPtrArray) requests = NULL;
	guint i;

	for (i = 0; i < AXIS_HTTP_RESCAN_MAX_UPDATES + 8; i++) {
		g_autofree gchar *name = g_strdup_printf("Film %02u (2000)", i);
		g_autofree gchar *folder = folder_in(fx, "Films", name);

		record(fx, folder, AXIS_COMPAT_CHANGE_CREATED);
	}
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, AXIS_HTTP_RESCAN_MAX_UPDATES + 8);
	requests = axis_test_plex_requests(fx->t->jellyfin);
	g_assert_cmpuint(requests->len, ==, 2);
}

/* A server that is not set up, or switched off, is never told; both are
 * said. */
static void
test_a_server_that_is_off_or_not_set_up_is_not_told(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	g_autofree gchar *alder = folder_in(fx, "Films", "Alder (1979)");
	g_autoptr(GError) error = NULL;
	AxisCompatNotifySummary summary;

	/* Jellyfin is set up and on; Plex is not set up: it has no row. */
	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "server_kind = 'plex'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "server_kind = 'jellyfin' AND state = 'pending'"), ==, 1);

	/* Switched off afterwards: the waiting row is skipped, nothing is sent. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN, "false", &error));
	g_assert_no_error(error);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.skipped, ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->jellyfin), ==, 0);
	assert_one(fx, "skipped|Telling this server which folder changed is switched off.",
		"SELECT state || '|' || note FROM compat_notify");
	/* And while it is off nothing more is recorded. */
	record(fx, alder, AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending'"), ==, 0);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN, "true", &error));
	record(fx, alder, AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending'"), ==, 1);

	/* No longer set up when its turn comes: skipped, said. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, "", &error));
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.skipped, ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->jellyfin), ==, 0);
}

/* ── Merging ────────────────────────────────────────────────────── */

static void
test_what_happened_to_a_folder_merges(void)
{
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_CREATED, AXIS_COMPAT_CHANGE_MODIFIED), ==,
		AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_MODIFIED, AXIS_COMPAT_CHANGE_CREATED), ==,
		AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_MODIFIED, AXIS_COMPAT_CHANGE_MODIFIED), ==,
		AXIS_COMPAT_CHANGE_MODIFIED);
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_CREATED, AXIS_COMPAT_CHANGE_DELETED), ==,
		AXIS_COMPAT_CHANGE_DELETED);
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_MODIFIED, AXIS_COMPAT_CHANGE_DELETED), ==,
		AXIS_COMPAT_CHANGE_DELETED);
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_DELETED, AXIS_COMPAT_CHANGE_DELETED), ==,
		AXIS_COMPAT_CHANGE_DELETED);
	/* It is there again. */
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_DELETED, AXIS_COMPAT_CHANGE_CREATED), ==,
		AXIS_COMPAT_CHANGE_MODIFIED);
	g_assert_cmpint(axis_compat_change_merge(AXIS_COMPAT_CHANGE_DELETED, AXIS_COMPAT_CHANGE_MODIFIED), ==,
		AXIS_COMPAT_CHANGE_MODIFIED);
}

/* Film with subtitles: every event of one folder in the quiet period is one
 * row and so one request; the table merges as the function does. */
static void
test_a_film_and_its_extras_are_one_notification(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	g_autofree gchar *other = folder_in(fx, "Films", "Alder (1979)");
	AxisCompatNotifySummary summary;
	guint i;

	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	for (i = 0; i < 4; i++)
		record(fx, hearth, AXIS_COMPAT_CHANGE_MODIFIED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", NULL), ==, 2);	/* a row each for Plex and Jellyfin */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "events = 5 AND change = 'created'"), ==, 2);
	/* Another folder is another row, and created then deleted is deleted;
	 * deleted then created is modified. */
	record(fx, other, AXIS_COMPAT_CHANGE_CREATED);
	record(fx, other, AXIS_COMPAT_CHANGE_DELETED);
	assert_one(fx, "deleted", "SELECT change FROM compat_notify WHERE folder = '%s' AND server_kind = 'plex'", other);
	record(fx, other, AXIS_COMPAT_CHANGE_CREATED);
	assert_one(fx, "modified", "SELECT change FROM compat_notify WHERE folder = '%s' AND server_kind = 'plex'", other);

	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 4);
	/* The film once (Plex's refresh names one folder, so one request a
	 * folder: two), and Jellyfin once for the whole batch. */
	g_assert_cmpuint(requests_with(fx->t->plex, "/library/sections/1/refresh?path=%2Fdata%2Fmovies%2FHearth%20%281995%29"),
		==, 1);
	g_assert_cmpuint(requests_with(fx->t->plex, "/refresh"), ==, 2);
	g_assert_cmpuint(requests_with(fx->t->jellyfin, "POST"), ==, 1);
}

/* An event of a folder that has been sent is a new row; one that arrives
 * while the folder waits moves its time on, but never past the longest wait. */
static void
test_the_quiet_time_and_the_longest_wait(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");

	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	/* Due after the quiet time (30 seconds by default). */
	assert_one(fx, "t", "SELECT due_at > now() + interval '25 seconds' AND due_at <= now() + interval '31 seconds'"
		" FROM compat_notify");
	/* A folder that keeps changing is sent at the latest five minutes after
	 * its first change. */
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_notify SET first_at = now() - interval '10 minutes'");
	record(fx, hearth, AXIS_COMPAT_CHANGE_MODIFIED);
	assert_one(fx, "t", "SELECT due_at <= now() FROM compat_notify");
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", NULL), ==, 1);

	run(fx, FALSE);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'sent'"), ==, 1);
	/* A later change is a row of its own. */
	record(fx, hearth, AXIS_COMPAT_CHANGE_MODIFIED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", NULL), ==, 2);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending'"), ==, 1);
}

/* ── A server that is down ──────────────────────────────────────── */

/* The import goes on (it never waited for the server); the request is tried
 * again after one, two, four, eight and sixteen minutes, then dropped and
 * said; the server's own text and not its key. */
static void
test_a_server_that_is_down_costs_retries_and_then_a_drop(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	static const gint seconds[] = { 60, 120, 240, 480, 960 };
	AxisCompatNotifySummary summary;
	guint attempt;

	axis_test_plex_clear_routes(fx->t->jellyfin);
	axis_test_plex_route(fx->t->jellyfin, "/Library/Media/Updated", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	for (attempt = 0; attempt < G_N_ELEMENTS(seconds); attempt++) {
		g_autofree gchar *expected = g_strdup_printf("pending|%u|true", attempt + 1);

		summary = run(fx, TRUE);
		g_assert_cmpuint(summary.failed, ==, 1);
		g_assert_cmpuint(summary.dropped, ==, 0);
		/* The next try is this long from now, twice as long each time. */
		assert_one(fx, expected, "SELECT state || '|' || attempts || '|' || (due_at > now() + interval '%d seconds'"
			" - interval '5 seconds' AND due_at <= now() + interval '%d seconds') FROM compat_notify", seconds[attempt],
			seconds[attempt]);
		/* Not due yet: the job leaves it alone. */
		summary = run(fx, FALSE);
		g_assert_cmpuint(summary.failed + summary.sent + summary.dropped, ==, 0);
	}
	/* The sixth failure drops it. */
	summary = run(fx, TRUE);
	g_assert_cmpuint(summary.dropped, ==, 1);
	assert_one_prefix(fx, "dropped|6|Given up after 6 tries: ",
		"SELECT state || '|' || attempts || '|' || note FROM compat_notify");
	assert_one(fx, "t", "SELECT done_at IS NOT NULL FROM compat_notify");
	/* Nothing tries a dropped row again. */
	axis_test_plex_reset_requests(fx->t->jellyfin);
	summary = run(fx, TRUE);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->jellyfin), ==, 0);
	assert_secrets_stay(fx);
}

/* A server that comes back after a failure: the next try sends it. */
static void
test_a_server_that_comes_back_is_told(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	AxisCompatNotifySummary summary;

	axis_test_plex_clear_routes(fx->t->jellyfin);
	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	summary = run(fx, TRUE);
	g_assert_cmpuint(summary.failed, ==, 1);
	axis_test_plex_route_full(fx->t->jellyfin, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	summary = run(fx, TRUE);
	g_assert_cmpuint(summary.sent, ==, 1);
	assert_one(fx, "sent|2", "SELECT state || '|' || attempts FROM compat_notify");
}

/* A redirect from the server is not followed: the body is sent once. */
static void
test_a_redirecting_jellyfin_is_not_followed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(other);
	g_autofree gchar *elsewhere = g_strconcat(base, "/Library/Media/Updated", NULL);
	AxisCompatNotifySummary summary;

	axis_test_plex_route_full(other, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	axis_test_plex_clear_routes(fx->t->jellyfin);
	axis_test_plex_route(fx->t->jellyfin, "/Library/Media/Updated", -1, AXIS_TEST_PLEX_REDIRECT, 307, elsewhere, 0);
	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	summary = run(fx, TRUE);
	g_assert_cmpuint(summary.failed, ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	g_assert_cmpuint(requests_with(fx->t->jellyfin, "POST"), ==, 1);
	axis_test_plex_free(other);
}

/* Old finished rows go; waiting ones stay. */
static void
test_old_finished_rows_are_removed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	g_autofree gchar *alder = folder_in(fx, "Films", "Alder (1979)");
	AxisCompatNotifySummary summary;

	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	run(fx, TRUE);
	record(fx, alder, AXIS_COMPAT_CHANGE_CREATED);
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_notify SET done_at = now() - interval '40 days' WHERE state = 'sent'");
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.removed, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", NULL), ==, 1);
	assert_one(fx, "pending", "SELECT state FROM compat_notify");
}

/* The listing the page shows: newest first, with what was said. */
static void
test_the_list_for_the_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	g_autofree gchar *shows = folder_in(fx, "Shows", "Harbour Lights (2024)");
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	const AxisCompatNotifyInfo *newest;

	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	run(fx, TRUE);
	record(fx, shows, AXIS_COMPAT_CHANGE_CREATED);
	g_assert_true(axis_compat_notify_list(fx->t->compat, 10, &rows, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	newest = g_ptr_array_index(rows, 0);
	g_assert_cmpstr(newest->folder, ==, shows);
	g_assert_cmpstr(newest->state, ==, "pending");
	g_assert_cmpstr(newest->change, ==, "created");
	g_assert_cmpint(newest->done_us, ==, 0);
	g_assert_cmpstr(((const AxisCompatNotifyInfo *)g_ptr_array_index(rows, 1))->state, ==, "sent");
	g_assert_cmpstr(((const AxisCompatNotifyInfo *)g_ptr_array_index(rows, 1))->sent_folder, ==, "/media/films/Hearth (1995)");
}

/* A folder that is not a clean absolute path, or the root of the filesystem,
 * is ignored. */
static void
test_a_folder_that_is_not_a_folder_is_ignored(Fixture *fx, gconstpointer data)
{
	record(fx, "relative/folder", AXIS_COMPAT_CHANGE_CREATED);
	record(fx, "/a/../b", AXIS_COMPAT_CHANGE_CREATED);
	record(fx, "/", AXIS_COMPAT_CHANGE_CREATED);
	record(fx, "", AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", NULL), ==, 0);
}

/* The two servers at once: each told its own folder under its own names,
 * with its own secret. */
static void
test_both_servers_are_told_with_their_own_secrets(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hearth = folder_in(fx, "Films", "Hearth (1995)");
	AxisCompatNotifySummary summary;

	record(fx, hearth, AXIS_COMPAT_CHANGE_CREATED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", NULL), ==, 2);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 2);
	g_assert_cmpuint(requests_with(fx->t->plex, "/library/sections/1/refresh?path=%2Fdata%2Fmovies%2FHearth%20%281995%29"), ==, 1);
	g_assert_cmpuint(requests_with(fx->t->jellyfin, "POST /Library/Media/Updated"), ==, 1);
	{
		g_autoptr(GPtrArray) posted = axis_test_plex_requests(fx->t->jellyfin);

		g_assert_nonnull(strstr(g_ptr_array_index(posted, 0), "\"Path\":\"/media/films/Hearth (1995)\""));
	}
	assert_secrets_stay(fx);
	g_assert_false(axis_test_plex_saw_non_get(fx->t->plex));
}

/* ── Asked again (fix-first-import) ─────────────────────────────── */

/* The made-up film's folder in the Films root. */
#define LANTERN_FOLDER "Marrowfield Lanterns (2011) {tmdb-990120}"

static gint64
row_of(
	Fixture     *fx,
	const gchar *where
){
	g_autofree gchar *id = one(fx, "SELECT id FROM compat_notify WHERE %s ORDER BY id DESC LIMIT 1", where);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

/* Spec (client-compatibility): a request that no path mapping reached is
 * sent once a mapping that reaches it is added: the latest one per folder of
 * the last seven days waits again, its tries from nothing; older ones stay
 * as they are. */
static void
test_a_mapping_added_sends_the_recent_unmapped_requests(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *recent = folder_in(fx, "Films", LANTERN_FOLDER);
	g_autofree gchar *old = folder_in(fx, "Films", "Quillfeather Junction (2014)");
	g_autofree gchar *elsewhere = g_build_filename(fx->t->s.disk_a, "Elsewhere", "Bramblewick Tides (2010)", NULL);
	AxisCompatNotifySummary summary;
	gint64 latest;
	gint64 earlier;
	guint count = 99;

	/* No mapping: three folders, none sent; the recent one twice. */
	record(fx, recent, AXIS_COMPAT_CHANGE_CREATED);
	record(fx, old, AXIS_COMPAT_CHANGE_CREATED);
	record(fx, elsewhere, AXIS_COMPAT_CHANGE_CREATED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.unmapped, ==, 3);
	record(fx, recent, AXIS_COMPAT_CHANGE_MODIFIED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.unmapped, ==, 1);
	g_assert_cmpuint(requests_with(fx->t->jellyfin, "/Library/Media/Updated"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'unmapped'"), ==, 4);
	earlier = row_of(fx, "change = 'created' AND folder LIKE '%" LANTERN_FOLDER "'");
	latest = row_of(fx, "change = 'modified' AND folder LIKE '%" LANTERN_FOLDER "'");
	/* The old one changed more than a week ago. */
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_notify SET first_at = now() - interval '8 days',"
		" last_at = now() - interval '8 days', done_at = now() - interval '8 days'"
		" WHERE folder LIKE '%Quillfeather Junction (2014)'");

	/* The mapping of the Films root (and of Music) is added. */
	axis_test_compat_map_jellyfin_standard(fx->t);
	/* The latest request of the recent folder waits again, due now, from no
	 * tries; the earlier one of that folder, the old folder's and the one no
	 * mapping reaches still stay as they were. */
	assert_one(fx, "pending|0||true", "SELECT state || '|' || attempts || '|' || coalesce(note, '') || '|' ||"
		" (due_at <= now())::text FROM compat_notify WHERE id = %" G_GINT64_FORMAT, latest);
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify WHERE id = %" G_GINT64_FORMAT, earlier);
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify WHERE folder LIKE '%%Quillfeather Junction (2014)'");
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify WHERE folder LIKE '%%Bramblewick Tides (2010)'");

	/* The notify job sends it, under the server's name for the folder. */
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 1);
	g_assert_cmpuint(requests_with(fx->t->jellyfin, "/Library/Media/Updated"), ==, 1);
	assert_one(fx, "sent|/media/films/" LANTERN_FOLDER, "SELECT state || '|' || sent_folder FROM compat_notify WHERE id = %"
		G_GINT64_FORMAT, latest);
	{
		g_autofree gchar *lines = axis_test_plex_transcript(fx->t->jellyfin);

		g_assert_null(strstr(lines, "Quillfeather"));
		g_assert_null(strstr(lines, "Bramblewick"));
	}

	/* Looked at again, nothing more waits: the folder was told since its
	 * earlier request. */
	g_assert_true(axis_compat_notify_retry_unmapped(fx->t->compat, NULL, &count, NULL));
	g_assert_cmpuint(count, ==, 0);
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify WHERE id = %" G_GINT64_FORMAT, earlier);
	assert_secrets_stay(fx);
}

/* A request for the folder waiting already is the one that is sent: the
 * unmapped one stays, and nothing breaks the one waiting row per folder. */
static void
test_an_unmapped_request_with_one_waiting_stays(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *movie = folder_in(fx, "Films", LANTERN_FOLDER);
	AxisCompatNotifySummary summary;
	g_autoptr(GError) error = NULL;
	AxisCompatAskAgain outcome;
	gint64 unmapped;
	guint count = 99;

	record(fx, movie, AXIS_COMPAT_CHANGE_CREATED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.unmapped, ==, 1);
	unmapped = row_of(fx, "state = 'unmapped'");
	/* The folder changes again: a new request waits for its quiet time. */
	record(fx, movie, AXIS_COMPAT_CHANGE_MODIFIED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending'"), ==, 1);

	axis_test_compat_map_jellyfin_standard(fx->t);
	g_assert_true(axis_compat_notify_retry_unmapped(fx->t->compat, NULL, &count, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count, ==, 0);
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify WHERE id = %" G_GINT64_FORMAT, unmapped);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending'"), ==, 1);
	/* Asked again by hand: the waiting one is it. */
	g_assert_true(axis_compat_notify_ask_again(fx->t->compat, unmapped, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_COMPAT_ASK_AGAIN_ALREADY);
	assert_one(fx, "unmapped", "SELECT state FROM compat_notify WHERE id = %" G_GINT64_FORMAT, unmapped);
}

/* Spec (client-compatibility): "Ask again" sends a request that was not
 * sent: one no mapping reached, one whose tries ran out, one skipped; never
 * one that was sent or is waiting. */
static void
test_ask_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *movie = folder_in(fx, "Films", LANTERN_FOLDER);
	g_autofree gchar *other = folder_in(fx, "Films", "Ostrander Peaks (2018)");
	AxisCompatNotifySummary summary;
	AxisCompatAskAgain outcome;
	gint64 dropped;
	gint64 sent;
	guint i;

	/* The server is down for every try: dropped. */
	axis_test_plex_route_full(fx->t->jellyfin, "/Library/Media/Updated", 503, NULL, NULL, NULL);
	record(fx, movie, AXIS_COMPAT_CHANGE_CREATED);
	for (i = 0; i < AXIS_COMPAT_NOTIFY_MAX_ATTEMPTS; i++) {
		due_now(fx);
		summary = run(fx, FALSE);
	}
	g_assert_cmpuint(summary.dropped, ==, 1);
	dropped = row_of(fx, "state = 'dropped'");
	/* The server is back; another folder is sent. */
	axis_test_plex_route_full(fx->t->jellyfin, "/Library/Media/Updated", 204, NULL, NULL, NULL);
	record(fx, other, AXIS_COMPAT_CHANGE_CREATED);
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 1);
	sent = row_of(fx, "state = 'sent'");

	/* The dropped one waits again, from no tries, and is sent. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_compat_notify_ask_again(fx->t->compat, dropped, &outcome, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpint(outcome, ==, AXIS_COMPAT_ASK_AGAIN_WAITING);
	assert_one(fx, "pending|0|", "SELECT state || '|' || attempts || '|' || coalesce(note, '') FROM compat_notify"
		" WHERE id = %" G_GINT64_FORMAT, dropped);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.sent, ==, 1);
	assert_one(fx, "sent", "SELECT state FROM compat_notify WHERE id = %" G_GINT64_FORMAT, dropped);

	/* Sent, or waiting: nothing to ask again. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_compat_notify_ask_again(fx->t->compat, sent, &outcome, &error));
		g_assert_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID);
	}
	record(fx, other, AXIS_COMPAT_CHANGE_MODIFIED);
	{
		g_autoptr(GError) error = NULL;
		gint64 waiting = row_of(fx, "state = 'pending'");

		g_assert_false(axis_compat_notify_ask_again(fx->t->compat, waiting, &outcome, &error));
		g_assert_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID);
	}
	/* No such request. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_compat_notify_ask_again(fx->t->compat, 999999, &outcome, &error));
		g_assert_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND);
	}

	/* Skipped (telling Jellyfin switched off), then asked again with it on. */
	axis_test_db_exec(fx->t->lib->db, "DELETE FROM compat_notify WHERE state = 'pending'");
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN, "false", NULL));
	record(fx, movie, AXIS_COMPAT_CHANGE_MODIFIED);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_notify", "state = 'pending'"), ==, 0);
	axis_test_db_exec(fx->t->lib->db, "INSERT INTO compat_notify (server_kind, folder, change) VALUES ('jellyfin',"
		" '/made/up/Films/Ostrander Peaks (2018)', 'modified')");
	due_now(fx);
	summary = run(fx, FALSE);
	g_assert_cmpuint(summary.skipped, ==, 1);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN, "true", NULL));
	{
		g_autoptr(GError) error = NULL;
		gint64 skipped = row_of(fx, "state = 'skipped'");

		g_assert_true(axis_compat_notify_ask_again(fx->t->compat, skipped, &outcome, &error));
		g_assert_no_error(error);
		g_assert_cmpint(outcome, ==, AXIS_COMPAT_ASK_AGAIN_WAITING);
		/* /made/up is reached by no mapping: not sent, and it says so again. */
		summary = run(fx, FALSE);
		g_assert_cmpuint(summary.unmapped, ==, 1);
		assert_one_prefix(fx, "unmapped|No path mapping reaches this folder",
			"SELECT state || '|' || note FROM compat_notify WHERE id = %" G_GINT64_FORMAT, skipped);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(name, flags, func) \
	AXIS_TEST_ADD_DB_CASE("/compat-notify/" name, Fixture, GINT_TO_POINTER(flags), func)
	ADD("plex/section", F_PLEX | F_MAP_PLEX, test_plex_is_told_the_folder_of_its_section);
	ADD("plex/no-library", F_PLEX, test_a_folder_no_plex_library_holds_is_not_sent);
	ADD("plex/no-path-map", F_PLEX, test_a_folder_the_path_map_does_not_reach_is_not_sent);
	ADD("roots", F_PLEX | F_JELLYFIN | F_MAP_PLEX | F_MAP_JELLYFIN, test_the_page_lists_what_each_root_reaches);
	ADD("jellyfin/one-request", F_JELLYFIN | F_MAP_JELLYFIN, test_jellyfin_gets_one_request_for_the_batch);
	ADD("jellyfin/cap", F_JELLYFIN | F_MAP_JELLYFIN, test_a_big_batch_is_split_at_the_cap);
	ADD("jellyfin/redirect", F_JELLYFIN | F_MAP_JELLYFIN, test_a_redirecting_jellyfin_is_not_followed);
	ADD("off", F_JELLYFIN | F_MAP_JELLYFIN, test_a_server_that_is_off_or_not_set_up_is_not_told);
	ADD("batching", F_PLEX | F_JELLYFIN | F_MAP_PLEX | F_MAP_JELLYFIN, test_a_film_and_its_extras_are_one_notification);
	ADD("quiet", F_JELLYFIN | F_MAP_JELLYFIN, test_the_quiet_time_and_the_longest_wait);
	ADD("down", F_JELLYFIN | F_MAP_JELLYFIN, test_a_server_that_is_down_costs_retries_and_then_a_drop);
	ADD("back", F_JELLYFIN | F_MAP_JELLYFIN, test_a_server_that_comes_back_is_told);
	ADD("prune", F_JELLYFIN | F_MAP_JELLYFIN, test_old_finished_rows_are_removed);
	ADD("list", F_JELLYFIN | F_MAP_JELLYFIN, test_the_list_for_the_page);
	ADD("not-a-folder", F_JELLYFIN | F_MAP_JELLYFIN, test_a_folder_that_is_not_a_folder_is_ignored);
	ADD("both", F_PLEX | F_JELLYFIN | F_MAP_PLEX | F_MAP_JELLYFIN, test_both_servers_are_told_with_their_own_secrets);
	ADD("again/mapping-added", F_JELLYFIN, test_a_mapping_added_sends_the_recent_unmapped_requests);
	ADD("again/one-waiting", F_JELLYFIN, test_an_unmapped_request_with_one_waiting_stays);
	ADD("again/by-hand", F_JELLYFIN | F_MAP_JELLYFIN, test_ask_again);
#undef ADD
	g_test_add_func("/compat-notify/merge", test_what_happened_to_a_folder_merges);
	return g_test_run();
}
