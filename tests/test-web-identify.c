/*
 * test-web-identify.c - Media management -> Identify, a root's review, a
 * unit's page with its candidates and search, the bulk add, and the film's
 * page with its identified file, with the real shell on an ephemeral
 * loopback port, a real database and a real volume (a directory of the
 * scratch directory)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-library-identification 3.1 to 3.3, 4.9 and 4.10: the pages
 * render for an administrator and a viewer; only administrators see the
 * forms, which carry the session's CSRF token, and only they may post;
 * every action is recorded with the user; names from disk (markup, bytes
 * that are not UTF-8) are shown escaped.
 *
 * With AXIS_TEST_IDENTIFY_PAGES set to a directory, the pages are saved
 * there for looking at in a browser.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-identify.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-library.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gchar                *films_dir;
	gint64                volume_id;
	gint64                root_id;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisMovies           *movies;
	AxisIdentify         *identify;
	AxisWebIdentify      *identify_ui;
	AxisWebHub           *hub;
	AxisWebStorage       *storage_ui;
	AxisProbe            *probe;
	AxisWebLibrary       *library_ui;
	AxisWebProbe         *probe_ui;
	AxisWebBrowser       *browser_ui;
	AxisModuleRegistry   *registry;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;
	gchar                *viewer;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIdentifyTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->films_dir = g_build_filename(fx->disk, "films", NULL);
	while (g_main_context_iteration(NULL, FALSE))
		;
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, axis_test_identify_provider, NULL);
	axis_test_identify_posters(fx->fa);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
	fx->identify_ui = axis_web_identify_new(fx->identify, fx->quality);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_movies_set_identify(fx->movies, fx->identify);
	fx->hub = axis_web_hub_new(NULL);
	fx->storage_ui = axis_web_storage_new(fx->t->storage, fx->hub);
	/* The Library's pages, whose films root lists each film folder with the
	 * film it is. */
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->library_ui = axis_web_library_new(fx->t->library, fx->t->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->t->storage);
	fx->browser_ui = axis_web_browser_new(fx->t->library, fx->probe, fx->t->storage, fx->hub);
	axis_web_browser_set_folder_titles(fx->browser_ui, AXIS_MEDIA_MOVIES, AXIS_IDENTIFY_MODULE_ID,
		axis_web_identify_folder_titles, fx->identify);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(), fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_library_get_module(), fx->library_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_probe_get_module(), fx->probe_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_browser_get_module(), fx->browser_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_identify_get_module(), fx->identify_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), fx->movies, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", "50", &error));
	g_assert_no_error(error);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_stop(fx->web);
	axis_web_browser_stop(fx->browser_ui);
	axis_web_probe_stop(fx->probe_ui);
	axis_web_library_stop(fx->library_ui);
	axis_movies_stop(fx->movies);
	axis_web_identify_stop(fx->identify_ui);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_browser_free(fx->browser_ui);
	axis_web_probe_free(fx->probe_ui);
	axis_web_library_free(fx->library_ui);
	axis_probe_unref(fx->probe);
	axis_web_storage_free(fx->storage_ui);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_identify_free(fx->identify_ui);
	axis_identify_unref(fx->identify);
	axis_movies_unref(fx->movies);
	axis_web_quality_free(fx->quality_ui);
	axis_quality_store_unref(fx->quality);
	axis_web_downloads_free(fx->downloads_ui);
	axis_downloads_unref(fx->downloads);
	axis_web_indexers_free(fx->indexers_ui);
	axis_indexers_unref(fx->indexers);
	axis_web_meta_free(fx->meta_ui);
	axis_meta_unref(fx->meta);
	axis_web_settings_free(fx->web_settings);
	axis_test_meta_fixture_free(fx->fa);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->films_dir);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *token_from,
	const gchar *path,
	const gchar *form
){
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;

	if (token_from != NULL) {
		AxisTestReply opened = get_as(fx, cookie, token_from);

		token = axis_test_web_csrf_token(opened.body);
		axis_test_reply_clear(&opened);
	}
	escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain == NULL || strstr(plain, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);
	g_autoptr(GRegex) handler = g_regex_new("<[a-zA-Z][^>]*\\son[a-z]+=", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	g_assert_false(g_regex_match(handler, body, 0, NULL));
	assert_lacks(body, "javascript:");
}

/* Every form that posts carries the session's CSRF field; returns how many
 * post to the Identify pages (the header's logout form is not counted). */
static guint
forms_with_token(const gchar *body)
{
	const gchar *p = body;
	guint forms = 0;

	while ((p = strstr(p, "<form")) != NULL) {
		const gchar *end = strstr(p, "</form>");

		g_assert_nonnull(end);
		if (g_strstr_len(p, end - p, "method=\"post\"") != NULL) {
			g_assert_nonnull(g_strstr_len(p, end - p, "name=\"_csrf\""));
			if (g_strstr_len(p, end - p, "action=\"/identify") != NULL)
				forms++;
		}
		p = end;
	}
	return forms;
}

static guint
occurrences(
	const gchar *body,
	const gchar *needle
){
	const gchar *p = body;
	guint n = 0;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_IDENTIFY_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->t->db, sql);
}

static void
put(
	Fixture     *fx,
	const gchar *rel,
	gsize        size
){
	g_autofree gchar *path = g_build_filename(fx->films_dir, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *content = g_strnfill(MAX(size, 1), 'x');

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, content, (gssize)size, NULL));
}

static void
probed(
	Fixture     *fx,
	const gchar *rel,
	const gchar *resolution,
	gint         minutes
){
	axis_test_probe_video(fx->t, fx->root_id, rel, "matroska", "h264", resolution, "sdr", "0.1",
		(gint64)minutes * 60000, "aac", FALSE);
}

static gint64
unit_id(
	Fixture     *fx,
	const gchar *rel
){
	g_autofree gchar *id = q(fx, "SELECT id FROM library_identity WHERE rel_path = '%s'", rel);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

/* A messy root, scanned, probed, identified; every poster stored. */
static void
scene(Fixture *fx)
{
	g_autoptr(AxisScanResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIdentifyReport report;
	const AxisTestFilm *films = axis_test_identify_films();
	g_autofree gchar *bad_dir = g_build_filename(fx->films_dir, "Bad\xff\xfe" "Name (2001)", NULL);
	guint i;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.x264-GRP.mkv", 4000);
	put(fx, "Rokan (1998) {tmdb-6002}/Rokan.mkv", 3000);
	put(fx, "Dunmore (2021)/Dunmore.2021.1080p.WEB-DL.DDP5.1.H.264-GRP.mkv", 3000);
	put(fx, "Dunmore (2021)/Dunmore.2021.2160p.WEB-DL.DDP5.1.HDR.H.265-GRP.mkv", 5000);
	put(fx, "Solvane/Solvane.mkv", 2500);
	put(fx, "The Inlander (1999)/The.Inlander.1999.1080p.BluRay.mkv", 2600);
	put(fx, "Some Home Video (2003)/clip.mkv", 900);
	put(fx, "<img src=x onerror=alert(1)> & \"q\" (2001)/x<b>.mkv", 1000);
	put(fx, "Rowan.Marsh.Collection.2004.1080p.BluRay.DTS-HD.MA.5.1.x264-AVeryLongGroupName/"
		"Colliery.2004.1080p.BluRay.DTS-HD.MA.5.1.x264-AVeryLongGroupName.mkv", 2900);
	g_assert_cmpint(g_mkdir_with_parents(bad_dir, 0755), ==, 0);
	axis_test_write_named(bad_dir, "Bad\xff.mkv", "xxxx");
	result = axis_test_library_scan_expect(fx->t, fx->root_id, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	probed(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.x264-GRP.mkv", "1080p", 170);
	probed(fx, "Rokan (1998) {tmdb-6002}/Rokan.mkv", "1080p", 122);
	probed(fx, "Dunmore (2021)/Dunmore.2021.1080p.WEB-DL.DDP5.1.H.264-GRP.mkv", "1080p", 155);
	probed(fx, "Dunmore (2021)/Dunmore.2021.2160p.WEB-DL.DDP5.1.HDR.H.265-GRP.mkv", "2160p", 155);
	probed(fx, "Solvane/Solvane.mkv", "1080p", 99);
	probed(fx, "The Inlander (1999)/The.Inlander.1999.1080p.BluRay.mkv", "1080p", 60);
	for (i = 0; i < 5; i++) {
		g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
		g_assert_no_error(error);
		if (report.pending == 0)
			break;
	}
	/* The posters stored, as browsing would have left them. */
	for (i = 0; films[i].id != NULL; i++) {
		g_autofree gchar *ref = g_strdup_printf("/p%s.png", films[i].id);
		g_autofree gchar *hash = NULL;
		g_autoptr(GError) art = NULL;

		axis_meta_artwork_fetch(fx->meta, "fixture", ref, AXIS_META_ART_POSTER_SMALL, &hash, &art);
	}
}

/* ── The review flow (fix-identify-review-flow) ─────────────────── */

static AxisTestReply
get_htmx(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	const gchar *extra[3] = { "HX-Request", "true", NULL };

	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, extra, NULL);
}

/* A made-up film title, with its poster "/p<provider_id>.png". */
static gchar *
made_up_title(
	Fixture     *fx,
	const gchar *kind,
	const gchar *title,
	gint         year,
	gint         runtime,
	const gchar *provider_id
){
	g_autofree gchar *minutes = runtime > 0 ? g_strdup_printf("%d", runtime) : g_strdup("NULL");

	return q(fx, "INSERT INTO meta_title (kind, title, year, runtime_minutes, provider, provider_id, poster_ref)"
		" VALUES ('%s', '%s', %d, %s, 'fixture', '%s', '/p%s.png') RETURNING id", kind, title, year, minutes,
		provider_id, provider_id);
}

/* Puts the unit of @rel in a list: @state and @confidence, with @candidates
 * (a JSON array of meta_title ids, best first). */
static void
held_as(
	Fixture     *fx,
	const gchar *rel,
	const gchar *state,
	const gchar *confidence,
	const gchar *candidates
){
	g_autofree gchar *done = q(fx, "UPDATE library_identity SET state = '%s', confidence = '%s', title_id = NULL,"
		" linked_how = NULL, doubt = NULL, candidates = '%s' WHERE rel_path = '%s' RETURNING id", state, confidence,
		candidates, rel);

	g_assert_nonnull(done);
}

/*
 * Made-up folders, identified (the provider knows none of them, so each is
 * held as unknown), then put in the lists by hand.  Likely, in the order of
 * their paths: Amber Field, Glass Wren and Paper Lantern, each with made-up
 * candidates (Paper Lantern two of one name a year apart, Amber Field a
 * series of three seasons beside its film).  Unknown: Northern Static, Quiet
 * Harbour and Velvet Signal, each with one candidate.  Linked and now
 * uncertain: Willow Gate.  Only Paper Lantern's 2004 poster is on the image
 * host; nothing is kept yet.
 */
static void
review_scene(Fixture *fx)
{
	static const gchar *const folders[] = {
		"Amber Field (2010)", "Glass Wren (2003)", "Paper Lantern (2004)", "Northern Static (2015)",
		"Quiet Harbour (1999)", "Velvet Signal (2008)", "Willow Gate (2011)", NULL
	};
	g_autoptr(AxisScanResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) png = axis_test_meta_png();
	g_autofree gchar *amber = NULL;
	g_autofree gchar *coast = NULL;
	g_autofree gchar *glass = NULL;
	g_autofree gchar *paper = NULL;
	g_autofree gchar *paper_later = NULL;
	g_autofree gchar *north = NULL;
	g_autofree gchar *quiet = NULL;
	g_autofree gchar *velvet = NULL;
	g_autofree gchar *willow = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *doubted = NULL;
	AxisIdentifyReport report;
	guint i;

	for (i = 0; folders[i] != NULL; i++) {
		g_autofree gchar *rel = g_strdup_printf("%s/movie.mkv", folders[i]);

		put(fx, rel, 2000);
	}
	result = axis_test_library_scan_expect(fx->t, fx->root_id, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	for (i = 0; i < 5; i++) {
		g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
		g_assert_no_error(error);
		if (report.pending == 0)
			break;
	}
	amber = made_up_title(fx, "film", "Amber Field", 2010, 97, "9500");
	coast = made_up_title(fx, "series", "Amber Field", 2012, 45, "9510");
	glass = made_up_title(fx, "film", "Glass Wren", 2003, 101, "9520");
	paper = made_up_title(fx, "film", "Paper Lantern", 2004, 104, "9501");
	paper_later = made_up_title(fx, "film", "Paper Lantern", 2005, 88, "9599");
	north = made_up_title(fx, "film", "Northern Static", 2015, 92, "9530");
	quiet = made_up_title(fx, "film", "Quiet Harbour", 1999, 110, "9540");
	velvet = made_up_title(fx, "film", "Velvet Signal", 2008, 95, "9550");
	willow = made_up_title(fx, "film", "Willow Gate", 2011, 99, "9560");
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_season (title_id, number) SELECT id, n FROM meta_title,"
		" generate_series(0, 3) AS n WHERE provider_id = '9510'");

	json = g_strdup_printf("[{\"id\": %s}, {\"id\": %s}]", amber, coast);
	held_as(fx, "Amber Field (2010)", "review", "likely", json);
	g_free(json);
	json = g_strdup_printf("[{\"id\": %s}]", glass);
	held_as(fx, "Glass Wren (2003)", "review", "likely", json);
	g_free(json);
	json = g_strdup_printf("[{\"id\": %s}, {\"id\": %s}]", paper, paper_later);
	held_as(fx, "Paper Lantern (2004)", "review", "likely", json);
	g_free(json);
	json = g_strdup_printf("[{\"id\": %s}]", north);
	held_as(fx, "Northern Static (2015)", "review", "unknown", json);
	g_free(json);
	json = g_strdup_printf("[{\"id\": %s}]", quiet);
	held_as(fx, "Quiet Harbour (1999)", "review", "unknown", json);
	g_free(json);
	json = g_strdup_printf("[{\"id\": %s}]", velvet);
	held_as(fx, "Velvet Signal (2008)", "review", "unknown", json);
	doubted = q(fx, "UPDATE library_identity SET state = 'linked', confidence = 'likely', linked_how = 'auto',"
		" title_id = %s, doubt = 'It was linked by itself under earlier rules; looked at again, it is now uncertain'"
		" WHERE rel_path = 'Willow Gate (2011)' RETURNING id", willow);
	g_assert_nonnull(doubted);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/p9501.png", 200, "image/png", png, NULL);
}

/* The value of attribute @name of the first element at or after @marker,
 * with the "&amp;" htmx-glib writes there undone. */
static gchar *
attribute_after(
	const gchar *body,
	const gchar *marker,
	const gchar *name
){
	g_autofree gchar *open = g_strdup_printf("%s=\"", name);
	g_autofree gchar *raw = NULL;
	g_auto(GStrv) parts = NULL;
	const gchar *at = strstr(body, marker);
	const gchar *start;
	const gchar *end;

	if (at == NULL)
		g_error("expected to find '%s' in:\n%s", marker, body);
	start = strstr(at, open);
	g_assert_nonnull(start);
	start += strlen(open);
	end = strchr(start, '"');
	g_assert_nonnull(end);
	raw = g_strndup(start, (gsize)(end - start));
	parts = g_strsplit(raw, "&amp;", -1);
	return g_strjoinv("&", parts);
}

typedef struct
{
	Fixture     *fx;
	const gchar *ref;
	gboolean     kept;
} PosterWait;

static gboolean
poster_settled(gpointer data)
{
	const PosterWait *wait = data;
	g_autofree gchar *n = q(wait->fx, "SELECT count(*) FROM meta_artwork WHERE source_ref = '%s' AND"
		" (sha256 IS NOT NULL) = %s", wait->ref, wait->kept ? "true" : "false");

	return g_strcmp0(n, "1") == 0;
}

/* Waits until the small poster @ref is kept (@kept) or refused. */
static gboolean
wait_for_poster(
	Fixture     *fx,
	const gchar *ref,
	gboolean     kept
){
	PosterWait wait = { fx, ref, kept };

	return axis_test_pump_until(poster_settled, &wait, AXIS_TEST_WAIT_MS);
}

static gboolean
never(gpointer data)
{
	return FALSE;
}

/*
 * fix-identify-review-flow, defect 1: a candidate's poster on a review card
 * is fetched the first time its placeholder asks for it (nothing else asks
 * for a candidate's), kept, and shown on the next ask and on the card from
 * then on; the image host is asked once.  A poster the image host does not
 * have ends as the titled placeholder that asks nothing more.  An address
 * no stored title shows is never fetched.  Made-up titles; the image host
 * is the provider fixture's stand-in.
 */
static void
test_candidate_posters(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *card = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *art = NULL;
	g_autofree gchar *rows = NULL;
	gchar *tries;
	AxisTestReply reply;

	review_scene(fx);
	root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);
	reply = get_as(fx, fx->admin, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "src=\"/art/");
	card = g_strdup_printf("id=\"likely-%" G_GINT64_FORMAT "\"", unit_id(fx, "Paper Lantern (2004)"));
	first = attribute_after(reply.body, card, "hx-get");
	g_assert_cmpstr(first, ==, "/search/poster?provider=fixture&ref=%2Fp9501.png&size=card&try=0&t=Paper%20Lantern");
	save_page("identify-root-placeholders.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The first ask fetches it, in the background... */
	reply = get_htmx(fx, fx->admin, first);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "poster-none");
	assert_contains(reply.body, "try=1");
	axis_test_reply_clear(&reply);
	g_assert_true(wait_for_poster(fx, "/p9501.png", TRUE));
	g_assert_cmpuint(axis_test_plex_count(fx->fa->images, "/img/w185/p9501.png"), ==, 1);

	/* ...the next one shows it, from Axis, and asks no more... */
	again = g_strdup(first);
	tries = strstr(again, "try=0");
	g_assert_nonnull(tries);
	tries[4] = '1';
	hash = q(fx, "SELECT sha256 FROM meta_artwork WHERE source_ref = '/p9501.png'");
	art = g_strdup_printf("src=\"/art/%s\"", hash);
	reply = get_htmx(fx, fx->admin, again);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, art);
	assert_lacks(reply.body, "hx-get");
	axis_test_reply_clear(&reply);
	/* ...and the card has it from then on, without asking. */
	reply = get_as(fx, fx->admin, root);
	assert_contains(reply.body, art);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->images, "/img/w185/p9501.png"), ==, 1);

	/* The other Paper Lantern's poster is not on the image host: asked
	 * once, refused, and its placeholder asks nothing more. */
	reply = get_htmx(fx, fx->admin, "/search/poster?provider=fixture&ref=%2Fp9599.png&size=card&try=0&t=Paper+Lantern");
	assert_contains(reply.body, "try=1");
	axis_test_reply_clear(&reply);
	g_assert_true(wait_for_poster(fx, "/p9599.png", FALSE));
	reply = get_htmx(fx, fx->admin, "/search/poster?provider=fixture&ref=%2Fp9599.png&size=card&try=1&t=Paper+Lantern");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<span class=\"poster-title\">Paper Lantern</span>");
	assert_lacks(reply.body, "hx-get");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->images, "/img/w185/p9599.png"), ==, 1);

	/* An address no stored title shows: looked up, never fetched. */
	reply = get_htmx(fx, fx->viewer, "/search/poster?provider=fixture&ref=%2Fp9998.png&size=card&try=0&t=Nothing");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "poster-none");
	axis_test_reply_clear(&reply);
	axis_test_pump_until(never, NULL, 500);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->images, "/img/w185/p9998.png"), ==, 0);
	rows = q(fx, "SELECT count(*) FROM meta_artwork WHERE source_ref = '/p9998.png'");
	g_assert_cmpstr(rows, ==, "0");
}

/* The page's one notice of an action is directly before the element @id
 * (nothing but it after the card before), and says @words. */
static void
assert_notice_before(
	const gchar *body,
	const gchar *id,
	const gchar *words
){
	g_autofree gchar *attr = g_strdup_printf("id=\"%s\"", id);
	g_autofree gchar *between = NULL;
	const gchar *target = strstr(body, attr);
	const gchar *notice;

	if (target == NULL)
		g_error("expected to find '%s' in:\n%s", attr, body);
	g_assert_cmpuint(occurrences(body, "identify-notice\""), ==, 1);
	notice = g_strrstr_len(body, target - body, "identify-notice\"");
	if (notice == NULL)
		g_error("expected the notice before '%s' in:\n%s", attr, body);
	between = g_strndup(notice, (gsize)(target - notice));
	g_assert_null(strstr(between, "identify-row\""));
	g_assert_null(strstr(between, "<tr id="));
	assert_contains(between, words);
}

/* The page's one notice is under the heading of the list's card @id, before
 * any card of it. */
static void
assert_notice_under(
	const gchar *body,
	const gchar *id,
	const gchar *words
){
	g_autofree gchar *attr = g_strdup_printf("id=\"%s\"", id);
	g_autofree gchar *after = NULL;
	const gchar *list = strstr(body, attr);
	const gchar *notice;
	const gchar *end;

	if (list == NULL)
		g_error("expected to find '%s' in:\n%s", attr, body);
	g_assert_cmpuint(occurrences(body, "identify-notice\""), ==, 1);
	notice = strstr(list, "identify-notice\"");
	end = strstr(list, "</section>");
	g_assert_nonnull(notice);
	g_assert_nonnull(end);
	g_assert_true(notice < end);
	after = g_strndup(list, (gsize)(notice - list));
	g_assert_null(strstr(after, "identify-row\""));
	assert_contains(notice, words);
}

/* Posts @form as an administrator to unit @rel's actions (the token from
 * @from) and returns where it sends the browser. */
static gchar *
act_on(
	Fixture     *fx,
	const gchar *from,
	const gchar *rel,
	const gchar *form
){
	g_autofree gchar *act = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "/act", unit_id(fx, rel));
	AxisTestReply reply = post_as(fx, fx->admin, from, act, form);
	gchar *location;

	g_assert_cmpuint(reply.status, ==, 303);
	location = g_strdup(axis_test_reply_header(&reply, "Location"));
	axis_test_reply_clear(&reply);
	g_assert_nonnull(location);
	return location;
}

/* "action=choose&title=<the made-up title @provider_id>&<rest>". */
static gchar *
choose_form(
	Fixture     *fx,
	const gchar *provider_id,
	const gchar *rest
){
	g_autofree gchar *title = q(fx, "SELECT id FROM meta_title WHERE provider_id = '%s'", provider_id);

	g_assert_nonnull(title);
	return g_strdup_printf("action=choose&title=%s&%s", title, rest);
}

/* The page @location shows (its fragment never reaches the server). */
static AxisTestReply
follow(
	Fixture     *fx,
	const gchar *location
){
	g_autofree gchar *path = g_strndup(location, strcspn(location, "#"));

	return get_as(fx, fx->admin, path);
}

/*
 * fix-identify-review-flow, defect 2: after an action on a card, the person
 * lands at the card that followed it in the same list (the one before it
 * when it was the last, the list's heading when the list is empty), by the
 * redirect's fragment, with the notice there; the notice's Undo comes back
 * to the unit's own card.  The likely list on the root's page; the unknown
 * list on its own page, kept through the post and the redirect; a unit's
 * page reached from that list; "Linked, now uncertain"; Undo in "Linked
 * lately".  And the facts beside each candidate.
 */
static void
test_review_flow(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *expect = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *form = NULL;
	gint64 amber;
	gint64 glass;
	gint64 paper;
	gint64 north;
	gint64 quiet;
	gint64 velvet;
	gint64 willow;
	AxisTestReply reply;

	review_scene(fx);
	root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);
	amber = unit_id(fx, "Amber Field (2010)");
	glass = unit_id(fx, "Glass Wren (2003)");
	paper = unit_id(fx, "Paper Lantern (2004)");
	north = unit_id(fx, "Northern Static (2015)");
	quiet = unit_id(fx, "Quiet Harbour (1999)");
	velvet = unit_id(fx, "Velvet Signal (2008)");
	willow = unit_id(fx, "Willow Gate (2011)");

	/* Each card has its id; its forms say the page and the list; beside
	 * each candidate, what tells it apart. */
	reply = get_as(fx, fx->admin, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "id=\"list-likely\"");
	id = g_strdup_printf("id=\"likely-%" G_GINT64_FORMAT "\"", glass);
	assert_contains(reply.body, id);
	g_free(id);
	id = g_strdup_printf("id=\"unknown-%" G_GINT64_FORMAT "\"", quiet);
	assert_contains(reply.body, id);
	g_free(id);
	id = g_strdup_printf("id=\"doubt-%" G_GINT64_FORMAT "\"", willow);
	assert_contains(reply.body, id);
	assert_contains(reply.body, "name=\"list\" value=\"likely\"");
	assert_contains(reply.body, "<p class=\"identify-pick-facts\">Runs 1 h 44 min</p>");
	assert_contains(reply.body, "<p class=\"identify-pick-facts\">Runs 1 h 28 min</p>");
	assert_contains(reply.body, "<p class=\"identify-pick-facts\">3 seasons, 45 min an episode</p>");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	save_page("identify-root-flow.html", reply.body);
	axis_test_reply_clear(&reply);

	/* Confirm in the middle of the list: the card that followed. */
	location = act_on(fx, root, "Glass Wren (2003)", "action=confirm&back=root&list=likely");
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=confirm&at=likely-%" G_GINT64_FORMAT "#likely-%"
		G_GINT64_FORMAT, root, glass, paper, paper);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	g_assert_cmpuint(reply.status, ==, 200);
	g_free(id);
	id = g_strdup_printf("likely-%" G_GINT64_FORMAT, paper);
	assert_notice_before(reply.body, id, "<span>Linked </span><bdi>Glass Wren (2003)</bdi><span> to </span>"
		"<bdi>Glass Wren (2003)</bdi>");
	/* Its Undo comes back to the unit's own card. */
	assert_contains(reply.body, "name=\"land\" value=\"self\"");
	assert_lacks(reply.body, "Confirmed: it is linked to the title.");
	assert_csp_clean(reply.body);
	save_page("identify-root-after-confirm.html", reply.body);
	axis_test_reply_clear(&reply);
	g_free(location);
	location = act_on(fx, root, "Glass Wren (2003)", "action=undo&back=root&list=likely&land=self");
	g_free(expect);
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=undo&at=likely-%" G_GINT64_FORMAT "#likely-%"
		G_GINT64_FORMAT, root, glass, glass, glass);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	g_free(id);
	id = g_strdup_printf("likely-%" G_GINT64_FORMAT, glass);
	assert_notice_before(reply.body, id, "<span>Undone: </span><bdi>Glass Wren (2003)</bdi>");
	axis_test_reply_clear(&reply);

	/* The last card of the list: the one before it. */
	g_free(location);
	location = act_on(fx, root, "Paper Lantern (2004)", "action=confirm&back=root&list=likely");
	g_free(expect);
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=confirm&at=likely-%" G_GINT64_FORMAT "#likely-%"
		G_GINT64_FORMAT, root, paper, glass, glass);
	g_assert_cmpstr(location, ==, expect);
	g_free(location);
	location = act_on(fx, root, "Glass Wren (2003)", "action=confirm&back=root&list=likely");
	g_free(expect);
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=confirm&at=likely-%" G_GINT64_FORMAT "#likely-%"
		G_GINT64_FORMAT, root, glass, amber, amber);
	g_assert_cmpstr(location, ==, expect);
	/* The list empty: its heading, with the notice under it. */
	g_free(location);
	location = act_on(fx, root, "Amber Field (2010)", "action=confirm&back=root&list=likely");
	g_free(expect);
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=confirm&at=list-likely#list-likely", root, amber);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	assert_notice_under(reply.body, "list-likely", "<span>Linked </span><bdi>Amber Field (2010)</bdi>");
	assert_contains(reply.body, "Nothing likely waits for a person.");
	axis_test_reply_clear(&reply);

	/* The unknown list on its own page: "This movie" stays in that list,
	 * at the next card. */
	list = g_strdup_printf("%s?list=unknown", root);
	reply = get_as(fx, fx->admin, list);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "name=\"back\" value=\"list\"");
	assert_contains(reply.body, "name=\"list\" value=\"unknown\"");
	g_free(id);
	id = g_strdup_printf("href=\"/identify/units/%" G_GINT64_FORMAT "?back=list&amp;list=unknown\"", quiet);
	assert_contains(reply.body, id);
	axis_test_reply_clear(&reply);
	form = choose_form(fx, "9530", "back=list&list=unknown");
	g_free(location);
	location = act_on(fx, list, "Northern Static (2015)", form);
	g_free(expect);
	expect = g_strdup_printf("%s?list=unknown&unit=%" G_GINT64_FORMAT "&done=choose&at=unknown-%" G_GINT64_FORMAT
		"#unknown-%" G_GINT64_FORMAT, root, north, quiet, quiet);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<h2>Unknown: search or mark</h2>");
	assert_lacks(reply.body, "<h2>Likely: confirm or choose</h2>");
	g_free(id);
	id = g_strdup_printf("unknown-%" G_GINT64_FORMAT, quiet);
	assert_notice_before(reply.body, id, "<span>Linked </span><bdi>Northern Static (2015)</bdi><span> to </span>"
		"<bdi>Northern Static (2015)</bdi>");
	save_page("identify-unknown-after-choose.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A unit's page opened from that list: its actions come back to the
	 * list, at the next card. */
	g_free(list);
	list = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "?back=list&list=unknown", quiet);
	reply = get_as(fx, fx->admin, list);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "name=\"back\" value=\"list\"");
	assert_contains(reply.body, "name=\"list\" value=\"unknown\"");
	assert_contains(reply.body, "Back to the list");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = choose_form(fx, "9540", "back=list&list=unknown");
	g_free(location);
	location = act_on(fx, list, "Quiet Harbour (1999)", form);
	g_free(expect);
	expect = g_strdup_printf("%s?list=unknown&unit=%" G_GINT64_FORMAT "&done=choose&at=unknown-%" G_GINT64_FORMAT
		"#unknown-%" G_GINT64_FORMAT, root, quiet, velvet, velvet);
	g_assert_cmpstr(location, ==, expect);
	/* The last of the list, marked: its heading. */
	g_free(location);
	location = act_on(fx, root, "Velvet Signal (2008)", "action=not_film&back=list&list=unknown");
	g_free(expect);
	expect = g_strdup_printf("%s?list=unknown&unit=%" G_GINT64_FORMAT "&done=not_film&at=list-unknown#list-unknown",
		root, velvet);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	assert_notice_under(reply.body, "list-unknown", "<span>Marked </span><bdi>Velvet Signal (2008)</bdi>");
	axis_test_reply_clear(&reply);

	/* Linked, now uncertain: the link kept, the list empty, its heading on
	 * the root's page. */
	g_free(form);
	form = choose_form(fx, "9560", "back=root&list=doubt");
	g_free(location);
	location = act_on(fx, root, "Willow Gate (2011)", form);
	g_free(expect);
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=choose&at=list-doubt#list-doubt", root, willow);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	assert_notice_under(reply.body, "list-doubt", "<span>Linked </span><bdi>Willow Gate (2011)</bdi>");
	assert_contains(reply.body, "Nothing is uncertain now.");
	axis_test_reply_clear(&reply);

	/* Undo in Linked lately (the latest first): the row after it, with the
	 * notice in a row of its own before it. */
	g_free(location);
	location = act_on(fx, root, "Willow Gate (2011)", "action=undo&back=root&list=linked");
	g_free(expect);
	expect = g_strdup_printf("%s?unit=%" G_GINT64_FORMAT "&done=undo&at=linked-%" G_GINT64_FORMAT "#linked-%"
		G_GINT64_FORMAT, root, willow, quiet, quiet);
	g_assert_cmpstr(location, ==, expect);
	reply = follow(fx, location);
	g_free(id);
	id = g_strdup_printf("linked-%" G_GINT64_FORMAT, quiet);
	assert_notice_before(reply.body, id, "<span>Undone: </span><bdi>Willow Gate (2011)</bdi>");
	assert_contains(reply.body, "identify-notice-row");
	axis_test_web_assert_tables(reply.body);
	save_page("identify-root-after-undo.html", reply.body);
	axis_test_reply_clear(&reply);
}

/* ── The pages ──────────────────────────────────────────────────── */

static void
test_pages(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *search = NULL;
	g_autofree gchar *bad = NULL;
	AxisTestReply reply;

	scene(fx);
	root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);

	/* add-attention-notices, slice N2: what waits in Identify is a notice of
	 * the page "Needs attention", which names no folder; and a link Axis is
	 * no longer sure of is another. */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, " found in the library wait");
	assert_contains(reply.body, " for a person to say which title ");
	assert_contains(reply.body, "<a href=\"/identify\">Open Identify</a>");
	assert_lacks(reply.body, "no longer look");
	assert_lacks(reply.body, "Axis could not read");
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET doubt = 'As the test made it.' WHERE id = (SELECT min(id)"
		" FROM library_identity WHERE state = 'linked')");
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	assert_contains(reply.body, "1 movie or series folder that Axis linked to a title by itself no longer looks like "
		"that title.");
	assert_lacks(reply.body, "Axis could not read");
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET doubt = NULL");

	/* Every root. */
	reply = get_as(fx, fx->admin, "/identify");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Identify<");
	assert_contains(reply.body, "films");
	assert_contains(reply.body, "linked by Axis (sure)");
	assert_contains(reply.body, "likely, to review");
	assert_contains(reply.body, "Nothing on disk is renamed");
	assert_csp_clean(reply.body);
	save_page("identify.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A root: its figures, the bulk add, the review lists. */
	reply = get_as(fx, fx->admin, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Progress in this library folder");
	assert_contains(reply.body, "Manage every identified movie");
	assert_contains(reply.body, "each is listed as On disk, not managed by Axis");
	assert_contains(reply.body, "Likely: confirm or choose");
	assert_contains(reply.body, "Unknown: search or mark");
	assert_contains(reply.body, "Linked lately");
	assert_contains(reply.body, "<bdi>Solvane</bdi>");
	assert_contains(reply.body, ">1972<");
	assert_contains(reply.body, ">Confirm this movie<");
	assert_contains(reply.body, ">Choose this movie<");
	assert_contains(reply.body, "Mark as not a movie");
	assert_contains(reply.body, "Search for the movie");
	assert_contains(reply.body, "Identify new and changed files now");
	assert_contains(reply.body, "identify-picks");
	assert_contains(reply.body, "identify-row");
	/* Names from disk, escaped: markup, and bytes that are not UTF-8. */
	assert_contains(reply.body, "&lt;img src=x onerror=alert(1)&gt;");
	assert_lacks(reply.body, "<img src=x");
	assert_lacks(reply.body, "<b>.mkv");
	assert_csp_clean(reply.body);
	g_assert_cmpuint(forms_with_token(reply.body), >, 5);
	axis_test_web_assert_tables(reply.body);
	save_page("identify-root.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A viewer sees the same, with no form. */
	reply = get_as(fx, fx->viewer, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">1972<");
	g_assert_cmpuint(forms_with_token(reply.body), ==, 0);
	assert_lacks(reply.body, "Search for the movie");
	assert_lacks(reply.body, "Manage every identified movie");
	axis_test_reply_clear(&reply);

	/* One unit: its evidence, candidates, files and a search. */
	unit = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, unit_id(fx, "Solvane"));
	reply = get_as(fx, fx->admin, unit);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "About this folder or file");
	assert_contains(reply.body, "Possible matches");
	assert_contains(reply.body, "Search for the movie");
	assert_contains(reply.body, ">2002<");
	/* A file Axis did not match: linked by hand, for an administrator. */
	assert_contains(reply.body, "Link to a movie or episode by hand");
	assert_contains(reply.body, "/link\"");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	save_page("identify-unit.html", reply.body);
	g_assert_cmpuint(occurrences(reply.body, "identify-picks"), ==, 1);
	axis_test_reply_clear(&reply);
	/* The root's "Search for the movie": the unit's page, searching for what
	 * its names read as; a viewer's page searches nothing. */
	{
		g_autofree gchar *find = g_strdup_printf("%s?find=1", unit);

		reply = get_as(fx, fx->admin, find);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_cmpuint(occurrences(reply.body, "identify-picks"), ==, 2);
		assert_contains(reply.body, "value=\"Solvane\"");
		assert_csp_clean(reply.body);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->viewer, find);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_cmpuint(occurrences(reply.body, "identify-picks"), ==, 1);
		assert_lacks(reply.body, "Link to a movie or episode by hand");
		axis_test_reply_clear(&reply);
	}
	search =g_strdup_printf("%s?q=Insmere&year=2002", unit);
	reply = get_as(fx, fx->admin, search);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Insmere");
	axis_test_reply_clear(&reply);
	/* A viewer cannot search through the provider from here. */
	reply = get_as(fx, fx->viewer, search);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Insmere");
	axis_test_reply_clear(&reply);

	/* A name that is not UTF-8, on its unit's page: shown escaped. */
	bad = q(fx, "SELECT id FROM library_identity WHERE rel_path LIKE 'Bad%%'");
	g_assert_nonnull(bad);
	{
		g_autofree gchar *path = g_strdup_printf("/identify/units/%s", bad);

		reply = get_as(fx, fx->admin, path);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_true(g_utf8_validate(reply.body, -1, NULL));
		assert_contains(reply.body, "not valid UTF-8");
		axis_test_reply_clear(&reply);
	}

	/* An identified film not managed yet: its title page shows it as in the
	 * library and not managed, with its file in the movies' own card (the
	 * identification card steps aside), and the form that manages it. */
	{
		g_autofree gchar *hearth = q(fx, "SELECT title_id FROM library_identity WHERE rel_path = 'Hearth (1995)'");
		g_autofree gchar *path = g_strdup_printf("/title/%s", hearth);

		reply = get_as(fx, fx->admin, path);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "film-mark-in_library");
		assert_contains(reply.body, "On disk, not managed by Axis");
		assert_contains(reply.body, "<h2>Files of this movie</h2>");
		assert_contains(reply.body, "Hearth.1995.1080p.BluRay.x264-GRP.mkv");
		assert_contains(reply.body, "Manage this movie");
		assert_lacks(reply.body, "Folders and files on disk");
		assert_lacks(reply.body, "Its file is already on disk");
		assert_csp_clean(reply.body);
		save_page("film-on-disk.html", reply.body);
		axis_test_reply_clear(&reply);
	}
	/* Movies lists every identified film, and says how many wait for a
	 * person, with the way to the Identify page. */
	reply = get_as(fx, fx->viewer, "/films");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "film-mark-in_library");
	assert_contains(reply.body, "not listed yet: identification was not sure which movie");
	assert_contains(reply.body, "<a href=\"/identify\">Review the waiting folders on the Identify page</a>");
	assert_lacks(reply.body, "Identify is switched off");
	assert_csp_clean(reply.body);
	save_page("films-identified.html", reply.body);
	axis_test_reply_clear(&reply);
	/* With Identify switched off, the list says so instead of linking to it;
	 * with Movies switched off, the identification card stands in on the
	 * title's page. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *hearth = q(fx, "SELECT title_id FROM library_identity WHERE rel_path = 'Hearth (1995)'");
		g_autofree gchar *path = g_strdup_printf("/title/%s", hearth);

		g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_IDENTIFY_MODULE_ID, FALSE, &error));
		g_assert_no_error(error);
		reply = get_as(fx, fx->viewer, "/films");
		assert_contains(reply.body, "Identify is switched off: an administrator can switch it on under Modules.");
		assert_lacks(reply.body, "href=\"/identify\"");
		axis_test_reply_clear(&reply);
		g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_IDENTIFY_MODULE_ID, TRUE, &error));
		g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_MOVIES_MODULE_ID, FALSE, &error));
		g_assert_no_error(error);
		reply = get_as(fx, fx->admin, path);
		assert_contains(reply.body, "Folders and files on disk");
		assert_lacks(reply.body, "Manage this movie");
		axis_test_reply_clear(&reply);
		g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_MOVIES_MODULE_ID, TRUE, &error));
		g_assert_no_error(error);
	}
	reply = get_as(fx, fx->admin, "/identify/units/999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/* ── The actions ────────────────────────────────────────────────── */

static void
test_actions(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *act = NULL;
	g_autofree gchar *add = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *hearth = NULL;
	AxisTestReply reply;

	scene(fx);
	root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);
	unit = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, unit_id(fx, "Solvane"));
	act = g_strdup_printf("%s/act", unit);

	/* Not for a viewer, and not without the token. */
	reply = post_as(fx, fx->viewer, unit, act, "action=confirm&back=root");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, act, "action=confirm&back=root");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	count = q(fx, "SELECT count(*) FROM library_identity_action");
	g_assert_cmpstr(count, ==, "0");

	/* Confirm, as an administrator: linked, recorded with the user. */
	reply = post_as(fx, fx->admin, unit, act, "action=confirm&back=root");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=confirm");
	axis_test_reply_clear(&reply);
	g_free(count);
	count = q(fx, "SELECT state || ' ' || linked_how || ' ' || decided_by FROM library_identity WHERE rel_path ="
		" 'Solvane'");
	g_assert_cmpstr(count, ==, "linked confirmed root");
	g_free(count);
	count = q(fx, "SELECT action || ' ' || by_user FROM library_identity_action");
	g_assert_cmpstr(count, ==, "confirm root");
	{
		g_autofree gchar *after = g_strdup_printf("%s?done=confirm", root);

		reply = get_as(fx, fx->admin, after);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Confirmed: it is linked to the title.");
		save_page("identify-root-after.html", reply.body);
		axis_test_reply_clear(&reply);
	}

	/* Choose, from a search. */
	{
		g_autofree gchar *insider = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "/act",
			unit_id(fx, "The Inlander (1999)"));
		g_autofree gchar *title = q(fx, "SELECT id FROM meta_title WHERE title = 'The Inlander'");
		g_autofree gchar *form = g_strdup_printf("action=choose&title=%s&back=unit", title);

		reply = post_as(fx, fx->admin, unit, insider, form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, unit, insider, "action=choose&title=999999&back=unit");
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
	}
	/* Not a movie; then undo it. */
	{
		g_autofree gchar *home = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "/act",
			unit_id(fx, "Some Home Video (2003)"));

		reply = post_as(fx, fx->admin, unit, home, "action=not_film&back=root");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, unit, home, "action=not_film&back=root");
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, unit, home, "action=undo&back=root");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
	reply = post_as(fx, fx->admin, unit, act, "action=nonsense");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* Adding every identified film: not for a viewer; then as the
	 * administrator, unmonitored; nothing searched. */
	add = g_strdup_printf("%s/add", root);
	reply = post_as(fx, fx->viewer, root, add, "monitored=no");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, root, add, "monitored=maybe");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, root, add, "monitored=no");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=added");
	axis_test_reply_clear(&reply);
	g_free(count);
	count = q(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'");
	g_assert_cmpstr(count, ==, "0");
	g_free(count);
	count = q(fx, "SELECT count(*) FROM movie WHERE monitored");
	g_assert_cmpstr(count, ==, "0");
	g_free(count);
	count = q(fx, "SELECT action || ' ' || by_user FROM library_identity_action ORDER BY id DESC LIMIT 1");
	g_assert_cmpstr(count, ==, "add_all root");

	/* The film's page shows its identified files, the best marked. */
	hearth = q(fx, "SELECT title_id FROM library_identity WHERE rel_path = 'Dunmore (2021)'");
	{
		g_autofree gchar *path = g_strdup_printf("/title/%s", hearth);

		reply = get_as(fx, fx->admin, path);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Files of this movie");
		assert_contains(reply.body, ">Identified<");
		assert_contains(reply.body, "The copy its upgrades are judged by.");
		assert_contains(reply.body, "WEB-DL 2160p");
		assert_lacks(reply.body, "Folders and files on disk");
		assert_csp_clean(reply.body);
		save_page("film-identified.html", reply.body);
		axis_test_reply_clear(&reply);
	}
	/* Look again: a run is queued. */
	{
		g_autofree gchar *run = g_strdup_printf("%s/run", root);

		reply = post_as(fx, fx->admin, root, run, NULL);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_free(count);
		count = q(fx, "SELECT count(*) FROM job WHERE kind = 'identify.root' AND state = 'queued'");
		g_assert_cmpstr(count, ==, "1");
	}
}

/* ── The Library's folders ─────────────────────────────────────── */

/* The view links name @label's view as the current one. */
static void
assert_view_shown(
	const gchar *body,
	const gchar *label
){
	g_autofree gchar *pattern = g_strdup_printf("<a class=\"current\" aria-current=\"true\""
		" href=\"[^\"]*view=[a-z]+[^\"]*\">%s</a>", label);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);

	g_assert_nonnull(regex);
	if (!g_regex_match(regex, body, 0, NULL))
		g_error("expected the view %s to be shown as current in:\n%s", label, body);
}

static AxisTestReply
get_folder(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *query
){
	g_autofree gchar *id = q(fx, "SELECT id FROM library_dir WHERE root_id = %" G_GINT64_FORMAT " AND path = '%s'",
		fx->root_id, path);
	g_autofree gchar *address = NULL;

	g_assert_nonnull(id);
	address = g_strdup_printf("/library/folder/%s%s", id, query != NULL ? query : "");
	return get_as(fx, cookie, address);
}

/*
 * Spec (web-shell): Identified folder; Not identified.  The films root's
 * folders, in each of the three views: an identified one shows its film's
 * poster and title beside its own name, one not identified says so and
 * links to its unit; the name filter; the view kept per account; a folder's
 * own page says what it holds; with Identify switched off, the folders by
 * name alone.
 */
static void
test_library_folders(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *top = NULL;
	g_autofree gchar *hearth = NULL;
	g_autofree gchar *hearth_ref = NULL;
	g_autofree gchar *poster = NULL;
	g_autofree gchar *art = NULL;
	g_autofree gchar *film_link = NULL;
	g_autofree gchar *unit_link = NULL;
	g_autofree gchar *solvane = NULL;
	g_autofree gchar *address = NULL;
	AxisTestReply reply;

	scene(fx);
	/* Accounts the views are kept for (the auth here has no database). */
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (id, username, password_hash, role) OVERRIDING SYSTEM VALUE"
		" VALUES (1, 'root', '$argon2id$test', 'admin'), (2, 'viewer', '$argon2id$test', 'user')");
	top = g_strdup_printf("/library/root/%" G_GINT64_FORMAT, fx->root_id);
	hearth = q(fx, "SELECT title_id FROM library_identity WHERE rel_path = 'Hearth (1995)' AND state = 'linked'");
	g_assert_nonnull(hearth);
	hearth_ref = q(fx, "SELECT poster_ref FROM meta_title WHERE id = %s", hearth);
	g_assert_nonnull(hearth_ref);
	poster = axis_meta_artwork_find(fx->meta, "fixture", hearth_ref, AXIS_META_ART_POSTER_SMALL, FALSE);
	g_assert_nonnull(poster);
	art = g_strdup_printf("src=\"/art/%s\"", poster);
	film_link = g_strdup_printf("href=\"/title/%s\"", hearth);
	unit_link = g_strdup_printf("href=\"/identify/units/%" G_GINT64_FORMAT "\"", unit_id(fx, "Solvane"));
	solvane = q(fx, "SELECT state FROM library_identity WHERE rel_path = 'Solvane'");
	g_assert_cmpstr(solvane, !=, "linked");

	/* Posters, the first time: the film's poster and name, the folder's
	 * name and figures under it; Solvane not identified, with its unit. */
	reply = get_as(fx, fx->viewer, top);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Filter the folders by name");
	assert_contains(reply.body, "title-grid");
	assert_contains(reply.body, art);
	assert_contains(reply.body, ">Hearth</a>");
	assert_contains(reply.body, "Hearth (1995) \xc2\xb7 1 file, ");
	assert_contains(reply.body, unit_link);
	assert_contains(reply.body, ">Not identified</a>");
	assert_view_shown(reply.body, "Posters");
	/* Names from disk are text. */
	assert_contains(reply.body, "&lt;img src=x onerror=alert(1)&gt;");
	assert_lacks(reply.body, "<img src=x");
	g_assert_true(g_utf8_validate(reply.body, -1, NULL));
	assert_csp_clean(reply.body);
	save_page("library-films-posters.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The overview: the same, one dense line each. */
	address = g_strdup_printf("%s?view=overview", top);
	reply = get_as(fx, fx->viewer, address);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "title-rows");
	assert_contains(reply.body, art);
	assert_contains(reply.body, "Hearth (1995) \xc2\xb7 1 file, ");
	assert_contains(reply.body, unit_link);
	assert_view_shown(reply.body, "Overview");
	assert_csp_clean(reply.body);
	save_page("library-films-overview.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The table: as it was, with the film beside each folder's name. */
	g_free(address);
	address = g_strdup_printf("%s?view=table", top);
	reply = get_as(fx, fx->viewer, address);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Movie</th>");
	assert_contains(reply.body, film_link);
	assert_contains(reply.body, "<bdi>Hearth (1995)</bdi></a>");
	assert_contains(reply.body, unit_link);
	assert_lacks(reply.body, "title-grid");
	axis_test_web_assert_tables(reply.body);
	assert_csp_clean(reply.body);
	save_page("library-films-table.html", reply.body);
	axis_test_reply_clear(&reply);

	/* Kept for the viewer, who chose it; not for the administrator. */
	reply = get_as(fx, fx->viewer, top);
	assert_view_shown(reply.body, "Table");
	assert_lacks(reply.body, "title-grid");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, top);
	assert_view_shown(reply.body, "Posters");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *kept = q(fx, "SELECT string_agg(user_id || ':' || list || ':' || view, ',' ORDER BY user_id)"
			" FROM user_list_view");

		g_assert_cmpstr(kept, ==, "2:library-films:table");
	}

	/* The name filter keeps the view; what does not match is gone. */
	g_free(address);
	address = g_strdup_printf("%s?q=hearth", top);
	reply = get_as(fx, fx->viewer, address);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "value=\"hearth\"");
	assert_contains(reply.body, film_link);
	assert_lacks(reply.body, unit_link);
	assert_contains(reply.body, ">Clear the name filter</a>");
	assert_view_shown(reply.body, "Table");
	axis_test_reply_clear(&reply);
	g_free(address);
	address = g_strdup_printf("%s?q=nothing%%20like%%20it", top);
	reply = get_as(fx, fx->viewer, address);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "No subfolder here has a name that holds &quot;nothing like it&quot;.");
	axis_test_reply_clear(&reply);

	/* A folder's own page says what it holds. */
	reply = get_folder(fx, fx->viewer, "Hearth (1995)", NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Movie<");
	assert_contains(reply.body, film_link);
	axis_test_reply_clear(&reply);
	reply = get_folder(fx, fx->viewer, "Solvane", NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, unit_link);
	axis_test_reply_clear(&reply);

	/* A folder a person said is not a film says that. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'not_film', title_id = NULL, linked_how = NULL"
		" WHERE rel_path = 'Some Home Video (2003)'");
	reply = get_as(fx, fx->admin, top);
	assert_contains(reply.body, ">Not a movie</a>");
	axis_test_reply_clear(&reply);

	/* Identify switched off: the folders by name alone, still filtered. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_IDENTIFY_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->admin, top);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Filter the folders by name");
	assert_contains(reply.body, "<bdi>Hearth (1995)</bdi>");
	assert_lacks(reply.body, "Not identified");
	assert_lacks(reply.body, film_link);
	assert_lacks(reply.body, "title-grid");
	assert_lacks(reply.body, ">Posters</a>");
	axis_test_reply_clear(&reply);
}

/*
 * fix-identify-hunt-findings, item C: a folder waiting for the metadata
 * provider (a full record its decision needs was not answered) is shown
 * apart, saying so and what settles it, and Movies counts it as waiting.
 */
static void
test_waiting(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *list = NULL;
	AxisTestReply reply;

	scene(fx);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'waiting', confidence = 'unknown',"
		" title_id = NULL, linked_how = NULL, retry_at = now() + interval '15 minutes', tries = 1,"
		" reasons = '[{\"c\": \"provider_waiting\", \"t\": \"the details provider did not answer for the full record"
		" of a title (Fixture answered 500), which the decision needs\"}]' WHERE rel_path = 'Some Home Video (2003)'");
	root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);
	reply = get_as(fx, fx->admin, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Waiting for the details provider");
	assert_contains(reply.body, "Fixture answered 500");
	assert_contains(reply.body, "Axis looks at it again by itself once it answers");
	assert_contains(reply.body, "waiting for the details provider");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	list = g_strdup_printf("%s?list=waiting", root);
	reply = get_as(fx, fx->admin, list);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Waiting for the details provider");
	assert_contains(reply.body, "Some Home Video (2003)");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/films");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "wait");
	assert_contains(reply.body, "for TMDb, which did not answer");
	axis_test_reply_clear(&reply);
}

/*
 * The review of fix-identify-hunt-findings, item 4: a link a later look did
 * not confirm stands, listed apart for a person with what it is linked to
 * and why it is uncertain now; an administrator keeps it there.  Made-up
 * title.
 */
static void
test_doubt(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *act = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *row = NULL;
	AxisTestReply reply;

	scene(fx);
	title = q(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('film', 'Glass Wren', 2003,"
		" 'fixture', '9301') RETURNING id");
	id = q(fx, "UPDATE library_identity SET state = 'linked', confidence = 'likely', linked_how = 'auto',"
		" title_id = %s, doubt = 'It was linked by itself under earlier rules; looked at again, it is now uncertain"
		" (runtime 44 minutes off: 60 minutes against 104), so the link stands until a person decides'"
		" WHERE rel_path = 'Some Home Video (2003)' RETURNING id", title);
	root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);
	reply = get_as(fx, fx->admin, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Linked, now uncertain");
	assert_contains(reply.body, "linked, now uncertain");
	assert_contains(reply.body, ">Linked, now uncertain<");
	assert_contains(reply.body, "Linked to Glass Wren");
	assert_contains(reply.body, "It was linked by itself under earlier rules; looked at again, it is now uncertain"
		" (runtime 44 minutes off: 60 minutes against 104)");
	assert_contains(reply.body, "Keep this link");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	list = g_strdup_printf("%s?list=doubt", root);
	reply = get_as(fx, fx->admin, list);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Some Home Video (2003)");
	assert_contains(reply.body, "Keep this link");
	axis_test_reply_clear(&reply);
	unit = g_strdup_printf("/identify/units/%s", id);
	reply = get_as(fx, fx->admin, unit);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "now uncertain");
	assert_contains(reply.body, "Keep this link");
	axis_test_reply_clear(&reply);
	/* Not for a viewer to see the action. */
	reply = get_as(fx, fx->viewer, unit);
	assert_lacks(reply.body, "Keep this link");
	axis_test_reply_clear(&reply);

	/* Kept: the person's choice now, listed no more. */
	act = g_strdup_printf("%s/act", unit);
	form = g_strdup_printf("action=choose&title=%s&back=root", title);
	reply = post_as(fx, fx->admin, root, act, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	row = q(fx, "SELECT state || ' ' || linked_how || ' ' || held || ' ' || (doubt IS NULL) || ' ' ||"
		" (title_id = %s) FROM library_identity WHERE id = %s", title, id);
	g_assert_cmpstr(row, ==, "linked chosen true true true");
	reply = get_as(fx, fx->admin, root);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "now uncertain");
	axis_test_reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/web/identify/pages", test_pages);
	ADD("/web/identify/actions", test_actions);
	ADD("/web/identify/library-folders", test_library_folders);
	ADD("/web/identify/waiting", test_waiting);
	ADD("/web/identify/doubt", test_doubt);
	ADD("/web/identify/candidate-posters", test_candidate_posters);
	ADD("/web/identify/review-flow", test_review_flow);
#undef ADD
	return g_test_run();
}
