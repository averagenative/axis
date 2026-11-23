/*
 * test-meta-tvdb.c - the TheTVDB provider (add-metadata-tvdb, V1): its
 * answers read into the model (from hand-written fixtures in
 * tests/fixtures/tvdb, in the shape TheTVDB's API version 4 documents and
 * src/meta/axis-meta-tvdb-assumed.h assumes), and the provider run through
 * the metadata layer against a stand-in TheTVDB on loopback that logs in,
 * hands out tokens and refuses them, with a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (metadata): A film; An image on another host; No key;
 * Nothing kept; Expired token; Key refused; Testing the key; and of Every
 * episode of an order, that every page is read (the rest, stored apart from
 * TMDb's seasons, is V2's: tests/test-meta-sources.c).
 *
 * No request ever goes to TheTVDB: the fixtures are written by hand, not
 * recorded, and prove that Axis reads that shape, not that TheTVDB answers
 * in it today.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-plex.h"
#include "axis-test-storage.h"
#include "axis-test-tvdb.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb-assumed.h"
#include "meta/axis-meta-tvdb-parse.h"
#include "meta/axis-meta-tvdb.h"

/* Obviously fake: a test key and PIN, never a real one. */
#define KEY AXIS_TEST_TVDB_KEY
#define OTHER_KEY AXIS_TEST_TVDB_OTHER_KEY
#define PIN AXIS_TEST_TVDB_PIN
#define TOKEN_PREFIX AXIS_TEST_TVDB_TOKEN_PREFIX
#define WAIT 5000
#define DIR "tests/fixtures/tvdb/"

/* ── Parsing, without a network ─────────────────────────────────── */

static gchar *
read_fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(DIR, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

static void
assert_checked(GPtrArray *titles)
{
	guint i;

	for (i = 0; i < titles->len; i++) {
		g_autofree gchar *problem = NULL;

		if (!axis_meta_title_check(g_ptr_array_index(titles, i), "tvdb", &problem))
			g_error("result %u does not pass the check: %s", i, problem);
	}
}

static void
test_parse_search(void)
{
	g_autofree gchar *text = read_fixture("search-series.json");
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GPtrArray) french = NULL;
	g_autoptr(GError) error = NULL;
	const AxisMetaTitle *one;
	const AxisMetaTitle *two;
	const AxisMetaTitle *four;

	g_assert_true(axis_meta_tvdb_parse_search(text, strlen(text), "eng", &titles, &error));
	g_assert_no_error(error);
	/* The film, the one without an id and the repeat are left out. */
	g_assert_cmpuint(titles->len, ==, 3);
	assert_checked(titles);
	one = g_ptr_array_index(titles, 0);
	g_assert_cmpstr(one->provider, ==, "tvdb");
	g_assert_cmpstr(one->provider_id, ==, "81001");
	g_assert_cmpint(one->kind, ==, AXIS_META_KIND_SERIES);
	g_assert_false(one->detail);
	g_assert_cmpstr(one->title, ==, "Wobbly Lighthouse");
	g_assert_cmpstr(one->original_title, ==, "Wobbly Lighthouse");
	g_assert_cmpstr(one->overview, ==, "A lighthouse that leans with the tide.");
	g_assert_cmpint(one->year, ==, 2023);
	g_assert_cmpstr(one->first_air_date, ==, "2023-09-01");
	g_assert_cmpstr(one->original_language, ==, "eng");
	g_assert_cmpstr(one->poster_ref, ==, "/banners/posters/81001-1.jpg");
	/* A brief record claims nothing about other schemes. */
	g_assert_cmpuint(one->ids->len, ==, 1);
	g_assert_null(axis_meta_title_get_id(one, "imdb"));
	g_assert_null(one->genres);
	g_assert_null(one->alt_titles);
	g_assert_null(one->seasons);
	/* Spec: An image on another host: not kept. */
	two = g_ptr_array_index(titles, 1);
	g_assert_cmpstr(two->provider_id, ==, "81002");
	g_assert_null(two->poster_ref);
	g_assert_cmpint(two->year, ==, 2025);
	/* An id as a number, a year as a number, a path that climbs out. */
	four = g_ptr_array_index(titles, 2);
	g_assert_cmpstr(four->provider_id, ==, "81004");
	g_assert_cmpint(four->year, ==, 2024);
	g_assert_null(four->poster_ref);

	/* The name in the language asked for. */
	g_assert_true(axis_meta_tvdb_parse_search(text, strlen(text), "fra", &french, &error));
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(french, 0))->title, ==, "Le Phare Bancal");
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(french, 0))->original_title, ==, "Wobbly Lighthouse");
}

static void
test_parse_series(void)
{
	g_autofree gchar *text = read_fixture("series-81001.json");
	g_autoptr(AxisMetaTitle) title = NULL;
	g_autoptr(AxisMetaTitle) french = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *problem = NULL;
	const AxisMetaSeason *season;
	guint i;

	g_assert_true(axis_meta_tvdb_parse_series(text, strlen(text), "eng", &title, &error));
	g_assert_no_error(error);
	g_assert_true(axis_meta_title_check(title, "tvdb", &problem));
	g_assert_null(problem);
	g_assert_true(title->detail);
	g_assert_cmpstr(title->title, ==, "Wobbly Lighthouse");
	g_assert_cmpstr(title->overview, ==, "A lighthouse that leans with the tide.");
	g_assert_cmpint(title->year, ==, 2023);
	g_assert_cmpstr(title->first_air_date, ==, "2023-09-01");
	g_assert_cmpstr(title->last_air_date, ==, "2024-06-15");
	g_assert_cmpint(title->status, ==, AXIS_META_STATUS_CONTINUING);
	g_assert_cmpint(title->runtime, ==, 24);
	g_assert_cmpstr(title->original_language, ==, "eng");
	g_assert_cmpstr(title->poster_ref, ==, "/banners/posters/81001-1.jpg");
	g_assert_null(title->backdrop_ref);
	/* Genres once each. */
	g_assert_cmpuint(title->genres->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(title->genres, 0), ==, "Children");
	/* Remote ids: IMDb's and TMDb's by their assumed names; another site's
	 * is not a scheme Axis knows. */
	g_assert_cmpuint(title->ids->len, ==, 3);
	g_assert_cmpstr(axis_meta_title_get_id(title, "tvdb"), ==, "81001");
	g_assert_cmpstr(axis_meta_title_get_id(title, "imdb"), ==, "tt9000081");
	g_assert_cmpstr(axis_meta_title_get_id(title, "tmdb"), ==, "7051");
	/* Other names: the alias once, the French name; the name shown is not
	 * one of them, and an alias of no language keeps none. */
	g_assert_cmpuint(title->alt_titles->len, ==, 3);
	for (i = 0; i < title->alt_titles->len; i++)
		g_assert_cmpstr(((AxisMetaAltTitle *)g_ptr_array_index(title->alt_titles, i))->title, !=, "Wobbly Lighthouse");
	/* The orders it has (the absolute one and an unknown are no source). */
	g_assert_nonnull(title->sources);
	g_assert_cmpuint(title->sources->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(title->sources, 0), ==, "tvdb");
	g_assert_cmpstr(g_ptr_array_index(title->sources, 1), ==, "tvdb-dvd");
	/* Its aired order's seasons, without episodes. */
	g_assert_cmpuint(title->seasons->len, ==, 3);
	season = g_ptr_array_index(title->seasons, 2);
	g_assert_cmpint(season->number, ==, 2);
	g_assert_cmpstr(season->name, ==, "Season 2");
	g_assert_null(season->episodes);

	g_assert_true(axis_meta_tvdb_parse_series(text, strlen(text), "fra", &french, &error));
	g_assert_cmpstr(french->title, ==, "Le Phare Bancal");
	g_assert_cmpstr(french->original_title, ==, "Wobbly Lighthouse");
	g_assert_true(axis_meta_title_check(french, "tvdb", NULL));
}

/* Spec: Every episode of an order: the pages add up, read whole, each place
 * and each TheTVDB id once. */
static void
test_parse_episodes(void)
{
	g_autoptr(GPtrArray) seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	g_autoptr(GError) error = NULL;
	const AxisMetaSeason *season;
	const AxisMetaEpisode *episode;
	gboolean more = FALSE;
	guint page;
	guint i;

	for (page = 0; page < 3; page++) {
		g_autofree gchar *name = g_strdup_printf("episodes-81001-official-%u.json", page);
		g_autofree gchar *text = read_fixture(name);

		g_assert_true(axis_meta_tvdb_parse_episodes(text, strlen(text), seasons, &more, &error));
		g_assert_no_error(error);
		g_assert_true(more == (page < 2));
	}
	g_assert_cmpuint(seasons->len, ==, 3);
	for (i = 0; i < seasons->len; i++) {
		g_autofree gchar *problem = NULL;

		season = g_ptr_array_index(seasons, i);
		g_assert_cmpint(season->number, ==, (gint)i);
		if (!axis_meta_season_check(season, &problem))
			g_error("season %d: %s", season->number, problem);
	}
	season = g_ptr_array_index(seasons, 1);
	g_assert_cmpuint(season->episodes->len, ==, 5);
	g_assert_cmpint(season->episode_count, ==, 5);
	episode = g_ptr_array_index(season->episodes, 3);
	g_assert_cmpint(episode->number, ==, 4);
	g_assert_cmpstr(episode->name, ==, "The Fog Bell");
	g_assert_cmpstr(episode->air_date, ==, "2024-02-10");
	g_assert_cmpstr(episode->provider_id, ==, "8100104");
	g_assert_cmpint(episode->absolute_number, ==, 4);
	/* The repeat of S01E02 (another id, the same place) is not read. */
	episode = g_ptr_array_index(season->episodes, 1);
	g_assert_cmpstr(episode->name, ==, "Keeper of Gulls");
	/* Season 2: three half-hour episodes; the one whose id was read before
	 * (S01E01's, at S02E09), the one without a number and the one whose
	 * season is text are not. */
	season = g_ptr_array_index(seasons, 2);
	g_assert_cmpuint(season->episodes->len, ==, 3);
	episode = g_ptr_array_index(season->episodes, 2);
	g_assert_cmpint(episode->number, ==, 3);
	g_assert_cmpstr(episode->name, ==, "Storm Glass");
	g_assert_cmpint(episode->runtime, ==, 24);
	/* The specials. */
	season = g_ptr_array_index(seasons, 0);
	g_assert_cmpuint(season->episodes->len, ==, 1);
	g_assert_cmpint(((AxisMetaEpisode *)g_ptr_array_index(season->episodes, 0))->absolute_number, ==, 0);

	/* A page that cannot be read adds nothing. */
	{
		g_autofree gchar *failure = read_fixture("failure.json");
		GError *local = NULL;

		g_assert_false(axis_meta_tvdb_parse_episodes(failure, strlen(failure), seasons, &more, &local));
		g_assert_error(local, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
		g_assert_false(more);
		g_error_free(local);
		g_assert_cmpuint(seasons->len, ==, 3);
	}
}

static void
test_parse_remote_and_orders(void)
{
	g_autofree gchar *remote = read_fixture("remoteid-imdb.json");
	g_autofree gchar *types = read_fixture("seasons-types.json");
	g_autofree gchar *login = read_fixture("login.json");
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL;

	g_assert_true(axis_meta_tvdb_parse_remote(remote, strlen(remote), "imdb", "tt9000081", "eng", &titles, &error));
	g_assert_cmpuint(titles->len, ==, 1);
	assert_checked(titles);
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(titles, 0), "imdb"), ==, "tt9000081");
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(titles, 0))->poster_ref, ==, "/banners/posters/81001-1.jpg");

	g_assert_true(axis_meta_tvdb_parse_orders(types, strlen(types), &names, &error));
	g_assert_cmpuint(names->len, ==, 4);
	g_assert_cmpstr(g_ptr_array_index(names, 0), ==, "Aired Order");

	g_assert_true(axis_meta_tvdb_parse_login(login, strlen(login), &token, &error));
	g_assert_cmpstr(token, ==, "fake.tvdb-token-one.not-real");
}

static void
assert_unreadable(
	gboolean  ok,
	GError   *error
){
	g_assert_false(ok);
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
	g_error_free(error);
}

static void
test_parse_refused(void)
{
	static const gchar *const not_answers[] = {
		"", "not json", "[]", "null", "{}", "{\"data\": []}", "{\"status\": \"failure\", \"data\": []}",
		"{\"status\": \"success\"}", "{\"status\": \"success\", \"data\": {}}", "{\"status\": 1, \"data\": []}"
	};
	static const gchar *const not_logins[] = {
		"{\"status\": \"success\", \"data\": {}}", "{\"status\": \"success\", \"data\": {\"token\": \"\"}}",
		"{\"status\": \"success\", \"data\": {\"token\": \"two words\"}}",
		"{\"status\": \"success\", \"data\": {\"token\": 5}}",
		"{\"status\": \"success\", \"data\": {\"token\": \"line\\nbreak\"}}",
		"{\"status\": \"failure\", \"data\": {\"token\": \"x\"}}"
	};
	static const gchar *const not_series[] = {
		"{\"status\": \"success\", \"data\": {\"name\": \"x\"}}",
		"{\"status\": \"success\", \"data\": {\"id\": \"81001x\", \"name\": \"x\"}}",
		"{\"status\": \"success\", \"data\": {\"id\": 0, \"name\": \"x\"}}",
		"{\"status\": \"success\", \"data\": {\"id\": 81001}}",
		"{\"status\": \"success\", \"data\": {\"id\": 81001, \"name\": \"\"}}",
		"{\"status\": \"success\", \"data\": [1]}"
	};
	g_autofree gchar *failure = read_fixture("failure.json");
	GPtrArray *titles = NULL;
	AxisMetaTitle *title = NULL;
	gchar *token = NULL;
	GError *error = NULL;
	gboolean ok;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(not_answers); i++) {
		error = NULL;
		ok = axis_meta_tvdb_parse_search(not_answers[i], strlen(not_answers[i]), "eng", &titles, &error);
		assert_unreadable(ok, error);
		g_assert_null(titles);
		error = NULL;
		ok = axis_meta_tvdb_parse_series(not_answers[i], strlen(not_answers[i]), "eng", &title, &error);
		assert_unreadable(ok, error);
		g_assert_null(title);
	}
	for (i = 0; i < G_N_ELEMENTS(not_logins); i++) {
		error = NULL;
		ok = axis_meta_tvdb_parse_login(not_logins[i], strlen(not_logins[i]), &token, &error);
		assert_unreadable(ok, error);
		g_assert_null(token);
	}
	for (i = 0; i < G_N_ELEMENTS(not_series); i++) {
		error = NULL;
		ok = axis_meta_tvdb_parse_series(not_series[i], strlen(not_series[i]), "eng", &title, &error);
		assert_unreadable(ok, error);
		g_assert_null(title);
	}
	error = NULL;
	ok = axis_meta_tvdb_parse_remote(failure, strlen(failure), "imdb", "tt9000081", "eng", &titles, &error);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tvdb_parse_orders(failure, strlen(failure), &titles, &error);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tvdb_parse_search(NULL, 0, "eng", &titles, &error);
	assert_unreadable(ok, error);
}

/* Lists far beyond any real answer are cut, and what remains is checked. */
static void
test_parse_huge(void)
{
	g_autoptr(GString) text = g_string_new("{\"status\": \"success\", \"data\": [");
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GPtrArray) seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	g_autoptr(GError) error = NULL;
	gboolean more = FALSE;
	guint i;

	for (i = 1; i <= 120; i++)
		g_string_append_printf(text, "%s{\"tvdb_id\": \"%u\", \"name\": \"Made-up %u\"}", i > 1 ? "," : "", i, i);
	g_string_append(text, "]}");
	g_assert_true(axis_meta_tvdb_parse_search(text->str, text->len, "eng", &titles, &error));
	g_assert_cmpuint(titles->len, ==, AXIS_META_MAX_RESULTS);
	assert_checked(titles);

	g_string_assign(text, "{\"status\": \"success\", \"data\": {\"episodes\": [");
	for (i = 0; i < 4000; i++)
		g_string_append_printf(text, "%s{\"id\": %u, \"seasonNumber\": %u, \"number\": %u}", i > 0 ? "," : "",
			i + 1, i % 250, i / 250 + 1);
	g_string_append(text, "]}}");
	g_assert_true(axis_meta_tvdb_parse_episodes(text->str, text->len, seasons, &more, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(seasons->len, ==, AXIS_META_MAX_SEASONS);
	for (i = 0; i < seasons->len; i++)
		g_assert_true(axis_meta_season_check(g_ptr_array_index(seasons, i), NULL));
}

/* Spec: An image on another host: only TheTVDB's one image origin. */
static void
test_image_refs(void)
{
	static const gchar *const bad[] = {
		NULL, "", "/banners/x.jpg", "http://artworks.thetvdb.com/banners/x.jpg",
		"https://artworks.thetvdb.com.example/banners/x.jpg", "https://images.example/banners/x.jpg",
		"https://artworks.thetvdb.com@images.example/banners/x.jpg", "https://artworks.thetvdb.com:8443/banners/x.jpg",
		"https://artworks.thetvdb.com/banners/x.jpg?size=1", "https://artworks.thetvdb.com/banners/x.jpg#f",
		"https://artworks.thetvdb.com/banners/../x.jpg", "https://artworks.thetvdb.com//x.jpg",
		"https://artworks.thetvdb.com/banners/x.gif", "https://artworks.thetvdb.com/banners/.jpg",
		"https://artworks.thetvdb.com/banners/x y.jpg", "https://artworks.thetvdb.com/",
		"https://ARTWORKS.thetvdb.com/banners/x.jpg"
	};
	g_autofree gchar *good = axis_meta_tvdb_image_ref("https://artworks.thetvdb.com/banners/posters/81001-1.jpg");
	g_autofree gchar *path = NULL;
	guint i;

	g_assert_cmpstr(AXIS_META_TVDB_IMAGE_ORIGIN, ==, "https://artworks.thetvdb.com");
	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autofree gchar *ref = axis_meta_tvdb_image_ref(bad[i]);

		if (ref != NULL)
			g_error("%s was taken as an image: %s", bad[i] != NULL ? bad[i] : "(null)", ref);
	}
	g_assert_cmpstr(good, ==, "/banners/posters/81001-1.jpg");
	/* No thumbnail form is assumed: every size is the image itself. */
	path = axis_meta_tvdb_image_path(good, AXIS_META_ART_POSTER_SMALL);
	g_assert_cmpstr(path, ==, good);
	g_clear_pointer(&path, g_free);
	g_assert_null(axis_meta_tvdb_image_path("/banners/../x.jpg", AXIS_META_ART_POSTER));
	g_assert_null(axis_meta_tvdb_image_path("/x.gif", AXIS_META_ART_POSTER));
	g_assert_null(axis_meta_tvdb_image_path(good, (AxisMetaArtSize)3));
}

static void
test_forms(void)
{
	static const gchar *const secrets[] = { KEY, PIN, "x", "a-b_c.d~e!", NULL };
	static const gchar *const not_secrets[] = { "", "two words", "quote\"d", "back\\slash", "caf\xc3\xa9", "tab\t",
		NULL };
	g_autofree gchar *long_one = g_strnfill(256, 'k');
	guint i;

	for (i = 0; secrets[i] != NULL; i++) {
		g_autofree gchar *reason = NULL;

		g_assert_true(axis_meta_tvdb_secret_valid(secrets[i], &reason));
		g_assert_null(reason);
	}
	for (i = 0; not_secrets[i] != NULL; i++) {
		g_autofree gchar *reason = NULL;

		g_assert_false(axis_meta_tvdb_secret_valid(not_secrets[i], &reason));
		g_assert_nonnull(reason);
		if (not_secrets[i][0] != '\0')
			g_assert_null(strstr(reason, not_secrets[i]));
	}
	{
		g_autofree gchar *reason = NULL;

		g_assert_false(axis_meta_tvdb_secret_valid(long_one, &reason));
	}
	g_assert_true(axis_meta_tvdb_language_valid("eng"));
	g_assert_true(axis_meta_tvdb_language_valid("deu"));
	g_assert_false(axis_meta_tvdb_language_valid("en"));
	g_assert_false(axis_meta_tvdb_language_valid("ENG"));
	g_assert_false(axis_meta_tvdb_language_valid("en-US"));
	g_assert_false(axis_meta_tvdb_language_valid(NULL));

	/* The orders and the sources Axis keeps them under. */
	g_assert_cmpstr(axis_meta_tvdb_source_of_order("official"), ==, "tvdb");
	g_assert_cmpstr(axis_meta_tvdb_source_of_order("dvd"), ==, "tvdb-dvd");
	g_assert_null(axis_meta_tvdb_source_of_order("absolute"));
	g_assert_null(axis_meta_tvdb_source_of_order("default"));
	g_assert_cmpstr(axis_meta_tvdb_order_of_source("tvdb"), ==, "official");
	g_assert_cmpstr(axis_meta_tvdb_order_of_source("tvdb-alttwo"), ==, "alttwo");
	g_assert_null(axis_meta_tvdb_order_of_source("tmdb"));
	for (i = 0; axis_meta_tvdb_sources()[i] != NULL; i++)
		g_assert_true(axis_meta_source_valid(axis_meta_tvdb_sources()[i]));
	g_assert_cmpuint(i, ==, 6);
	g_assert_true(axis_meta_source_valid("tmdb"));
	g_assert_false(axis_meta_source_valid("tvdb-"));
	g_assert_false(axis_meta_source_valid("tvdb-dvd2"));
	g_assert_false(axis_meta_source_valid("-dvd"));
	g_assert_false(axis_meta_source_valid("Tvdb"));
	g_assert_false(axis_meta_source_valid(NULL));
}

/* What the provider declares: its fixed origins, series only, secondary, its
 * login and the notice with its link. */
static void
test_declaration(void)
{
	const AxisMetaProviderInfo *info = axis_meta_tvdb_get_info();

	g_assert_cmpstr(info->name, ==, "tvdb");
	g_assert_cmpstr(info->label, ==, "TheTVDB");
	g_assert_cmpstr(info->api_origin, ==, "https://api4.thetvdb.com");
	g_assert_cmpstr(info->image_origin, ==, AXIS_META_TVDB_IMAGE_ORIGIN);
	g_assert_cmpuint(info->kinds, ==, AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES));
	g_assert_true(info->secondary);
	g_assert_nonnull(info->login);
	g_assert_nonnull(info->order);
	g_assert_nonnull(info->test);
	g_assert_cmpstr(info->pin_setting, ==, AXIS_META_TVDB_SETTING_PIN);
	g_assert_cmpstr(info->key_header, ==, "Authorization");
	g_assert_cmpstr(info->key_prefix, ==, "Bearer ");
	g_assert_cmpfloat(info->default_rate, ==, 4.0);
	g_assert_cmpuint(info->burst, ==, 2);
	g_assert_cmpstr(info->attribution, ==,
		"Metadata provided by TheTVDB. Please consider adding missing information or subscribing.");
	g_assert_cmpstr(info->attribution_url, ==, "https://thetvdb.com/subscribe");
	g_assert_true(info->sources == axis_meta_tvdb_sources());
	g_assert_null(info->list);
}

/* ── Through the metadata layer, against a stand-in TheTVDB ──────── */

/* Marks a test whose fixture sets no key, and one with the key but no title
 * of TMDb's stored. */
static const gchar no_key[] = "no key";
static const gchar bare[] = "bare";

typedef struct
{
	AxisTestDb                 *db;
	AxisDbPool                 *pool;
	AxisSettings               *settings;
	gchar                      *dir;
	gchar                      *data_path;
	AxisDataDir                *data;
	AxisMeta                   *meta;
	AxisTestTvdb               *tvdb;	/* the stand-in (tests/axis-test-tvdb.h) */
	const AxisMetaProviderInfo *info;	/* the real one, pointed at it */
	AxisTestPlex               *api;
	AxisTestPlex               *images;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->data_path = g_build_filename(fx->dir, "data", NULL);
	fx->data = axis_data_dir_open(fx->data_path, &error);
	g_assert_no_error(error);
	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);

	fx->tvdb = axis_test_tvdb_new();
	fx->info = axis_test_tvdb_info(fx->tvdb);
	fx->api = axis_test_tvdb_api(fx->tvdb);
	fx->images = axis_test_tvdb_images(fx->tvdb);
	g_assert_true(axis_meta_add_provider(fx->meta, fx->info, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	if (data == NULL || data == bare)
		g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, KEY, &error));
	g_assert_no_error(error);
	/* The series as TMDb's record made it, naming TheTVDB 81001: TheTVDB adds
	 * to a series Axis holds, never makes one (until a series can follow
	 * it, add-metadata-tvdb V3). */
	if (data == NULL) {
		axis_test_db_exec(fx->db, "INSERT INTO meta_title (kind, title, provider, provider_id, year) VALUES"
			" ('series', 'Wobbly Lighthouse', 'tmdb', '7051', 2023)");
		axis_test_db_exec(fx->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series',"
			" s, v FROM meta_title, (VALUES ('tmdb', '7051'), ('tvdb', '81001')) AS u (s, v)");
	}
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->meta != NULL) {
		axis_meta_stop(fx->meta);
		axis_meta_unref(fx->meta);
	}
	if (fx->settings != NULL)
		axis_settings_unref(fx->settings);
	axis_test_tvdb_free(fx->tvdb);
	if (fx->data != NULL)
		axis_data_dir_unref(fx->data);
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_free(fx->data_path);
	axis_test_storage_dir_free(fx->dir);
	axis_test_log_reset();
	while (g_main_context_iteration(NULL, FALSE))
		;
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

static guint
logins(Fixture *fx)
{
	return axis_test_tvdb_logins(fx->tvdb);
}

static gint64
query_int(
	Fixture     *fx,
	const gchar *sql
){
	g_autofree gchar *text = axis_test_db_query(fx->db, sql);

	return text != NULL ? g_ascii_strtoll(text, NULL, 10) : -1;
}

static void
search_ok(
	Fixture              *fx,
	const gchar          *text,
	AxisMetaSearchAnswer *answer
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_search(fx->meta, "tvdb", AXIS_META_KIND_SERIES, text, 0, WAIT, answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer->titles);
}

/* Every request: the login a POST of the key in the body and nothing else;
 * every other a GET with the token only as a bearer header; no key, PIN or
 * token in any request line, no header but the fixed ones. */
static void
assert_requests_plain(
	AxisTestPlex *plex,
	const gchar  *pin
){
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(plex);
	guint i;

	g_assert_cmpuint(requests->len, >, 0);
	for (i = 0; i < requests->len; i++) {
		const gchar *text = g_ptr_array_index(requests, i);
		g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
		const gchar *body = strstr(text, "\n\n");
		gboolean login = g_str_has_prefix(lines[0], "POST /v4/login");
		gboolean bearer = FALSE;
		guint j;

		g_assert_true(login || g_str_has_prefix(lines[0], "GET /v4/"));
		g_assert_null(strstr(lines[0], KEY));
		g_assert_null(strstr(lines[0], PIN));
		g_assert_null(strstr(lines[0], TOKEN_PREFIX));
		g_assert_null(strstr(lines[0], "apikey"));
		for (j = 1; lines[j] != NULL && lines[j][0] != '\0'; j++) {
			const gchar *colon = strchr(lines[j], ':');
			g_autofree gchar *name = g_strndup(lines[j], (gsize)(colon - lines[j]));
			const gchar *value = colon + 2;

			g_assert_null(strstr(lines[j], KEY));
			g_assert_null(strstr(lines[j], PIN));
			if (g_ascii_strcasecmp(name, "Host") == 0 || g_ascii_strcasecmp(name, "Accept") == 0 ||
			    g_ascii_strcasecmp(name, "Connection") == 0 || g_ascii_strcasecmp(name, "User-Agent") == 0 ||
			    (login && (g_ascii_strcasecmp(name, "Content-Type") == 0 ||
			               g_ascii_strcasecmp(name, "Content-Length") == 0)))
				continue;
			if (!login && g_ascii_strcasecmp(name, "Authorization") == 0) {
				g_assert_true(g_str_has_prefix(value, "Bearer " TOKEN_PREFIX));
				bearer = TRUE;
				continue;
			}
			g_error("an unexpected header was sent: %s", lines[j]);
		}
		if (login) {
			g_autofree gchar *expected = pin != NULL ?
				g_strdup_printf("\n\n{\"apikey\":\"%s\",\"pin\":\"%s\"}", KEY, pin) :
				g_strdup_printf("\n\n{\"apikey\":\"%s\"}", KEY);

			g_assert_nonnull(body);
			g_assert_true(g_str_has_prefix(body, expected));
		} else {
			g_assert_true(bearer);
			g_assert_true(body == NULL || strstr(body, KEY) == NULL);
		}
	}
}

/* A search logs in once and asks with the token. */
static void
test_search_logs_in(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaSearchAnswer second;
	g_autofree gchar *transcript = NULL;
	AxisMetaProviderStatus status;

	search_ok(fx, "Wobbly Lighthouse", &answer);
	g_assert_false(answer.saved);
	/* Only the result Axis holds is kept: TheTVDB adds to it. */
	g_assert_cmpuint(answer.titles->len, ==, 1);
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(answer.titles, 0))->title, ==, "Wobbly Lighthouse");
	axis_meta_search_answer_clear(&answer);
	search_ok(fx, "Wobbly Lighthouse Tales", &second);
	axis_meta_search_answer_clear(&second);

	/* One login for both: the token is kept in memory. */
	g_assert_cmpuint(logins(fx), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/login"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/search"), ==, 2);
	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript, "GET /v4/search?query=Wobbly%20Lighthouse&type=series&limit=50&offset=0\n"));
	assert_requests_plain(fx->api, NULL);
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_true(status.logs_in);
	g_assert_true(status.has_token);
	g_assert_false(status.key_refused);
	axis_meta_provider_status_clear(&status);
}

/* The PIN goes with the key when one is set, in the login's body only. */
static void
test_pin_in_the_login(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	g_autoptr(GError) error = NULL;
	const AxisSettingInfo *info = axis_settings_lookup(fx->settings, AXIS_META_TVDB_SETTING_PIN, NULL);

	g_assert_nonnull(info);
	g_assert_true((info->flags & AXIS_SETTING_SECRET) != 0);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_PIN, PIN, &error));
	g_assert_no_error(error);
	search_ok(fx, "Wobbly Lighthouse", &answer);
	g_assert_false(answer.saved);
	axis_meta_search_answer_clear(&answer);
	assert_requests_plain(fx->api, PIN);
	/* A key or PIN no login can carry is refused when it is saved. */
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TVDB_SETTING_PIN, "two words", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TVDB_SETTING_KEY, "quote\"d", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TVDB_SETTING_LANGUAGE, "en", NULL));
	g_assert_true(axis_settings_check(fx->settings, AXIS_META_TVDB_SETTING_LANGUAGE, "fra", NULL));
}

/* Spec: Expired token: a 401 to a token that worked brings one login and one
 * more try, and the answer is used. */
static void
test_expired_token(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;
	AxisMetaProviderStatus status;

	search_ok(fx, "Wobbly Lighthouse", &answer);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(logins(fx), ==, 1);

	axis_test_tvdb_expire(fx->tvdb, 1);
	axis_test_plex_reset_requests(fx->api);
	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_no_error(error);
	g_assert_false(title.saved);
	g_assert_nonnull(title.title);
	g_assert_cmpstr(title.title->title, ==, "Wobbly Lighthouse");
	axis_meta_title_answer_clear(&title);
	/* The refused try, one login, the try again. */
	g_assert_cmpuint(logins(fx), ==, 2);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 3);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/series/81001/extended"), ==, 2);
	/* The first 401 is not counted as a failure of the provider. */
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_cmpuint(status.limit.failures, ==, 0);
	axis_meta_provider_status_clear(&status);

	/* Once per request: a token refused twice is a failure, with no third
	 * try and no second login. */
	axis_test_tvdb_expire(fx->tvdb, 2);
	axis_test_plex_reset_requests(fx->api);
	search_ok(fx, "another made-up name", &answer);
	g_assert_true(answer.saved);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(logins(fx), ==, 3);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/search"), ==, 2);
}

/* A token older than its time is renewed before it is sent. */
static void
test_token_renewed(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	tuning.token_renew_s = 1;
	axis_meta_set_tuning(fx->meta, &tuning);
	search_ok(fx, "Wobbly Lighthouse", &answer);
	axis_meta_search_answer_clear(&answer);
	g_usleep(1100 * 1000);
	search_ok(fx, "Wobbly Lighthouse Tales", &answer);
	g_assert_false(answer.saved);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(logins(fx), ==, 2);
	/* Never a 401 on the way: the old token was not sent again. */
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/search"), ==, 2);
}

/* Spec: Key refused: nothing more is sent until the key or the PIN changes. */
static void
test_key_refused(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaProviderStatus status;
	g_autoptr(GError) error = NULL;
	guint sent;

	axis_test_tvdb_refuse(fx->tvdb, TRUE);
	search_ok(fx, "Wobbly Lighthouse", &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "refused the key"));
	g_assert_null(strstr(answer.problem, KEY));
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 1);
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_true(status.key_refused);
	g_assert_false(status.has_token);
	axis_meta_provider_status_clear(&status);

	/* The provider would take it now, but nothing is sent at all. */
	axis_test_tvdb_refuse(fx->tvdb, FALSE);
	search_ok(fx, "another made-up name", &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "until the key or the PIN is changed"));
	axis_meta_search_answer_clear(&answer);
	{
		g_autofree gchar *summary = NULL;
		GError *local = NULL;

		g_assert_false(axis_meta_test_key(fx->meta, "tvdb", NULL, &summary, &local));
		g_assert_nonnull(local);
		g_error_free(local);
		g_assert_nonnull(strstr(summary, "refused the key"));
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 1);

	/* A PIN changes what is sent: tried again. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_PIN, PIN, &error));
	search_ok(fx, "a third made-up name", &answer);
	g_assert_false(answer.saved);
	axis_meta_search_answer_clear(&answer);
	sent = axis_test_plex_n_requests(fx->api);
	g_assert_cmpuint(sent, ==, 3);

	/* And another key. */
	axis_test_tvdb_refuse(fx->tvdb, TRUE);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, OTHER_KEY, &error));
	search_ok(fx, "a fourth made-up name", &answer);
	g_assert_true(answer.saved);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, sent + 1);
	axis_test_tvdb_refuse(fx->tvdb, FALSE);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, KEY, &error));
	search_ok(fx, "a fifth made-up name", &answer);
	g_assert_false(answer.saved);
	axis_meta_search_answer_clear(&answer);
}

/* Spec: No key: nothing is sent, and TheTVDB is offered nowhere. */
static void
test_no_key(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;

	g_assert_false(axis_settings_is_set(fx->settings, AXIS_META_TVDB_SETTING_KEY));
	g_assert_false(axis_meta_provider_offered(fx->meta, fx->info));
	search_ok(fx, "Wobbly Lighthouse", &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "no key is set"));
	axis_meta_search_answer_clear(&answer);
	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_true(title.saved);
	axis_meta_title_answer_clear(&title);
	g_assert_false(axis_meta_test_key(fx->meta, "tvdb", NULL, &summary, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NO_KEY);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->images), ==, 0);

	g_clear_error(&error);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, KEY, &error));
	g_assert_true(axis_meta_provider_offered(fx->meta, fx->info));
}

/* Spec: A film: invalid, and nothing is sent. */
static void
test_a_film(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_meta_search(fx->meta, "tvdb", AXIS_META_KIND_FILM, "Wobbly Lighthouse", 0, WAIT, &answer,
		&error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_FILM, "81001", WAIT, &title, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_meta_find(fx->meta, "tvdb", AXIS_META_KIND_FILM, "imdb", "tt9000081", WAIT, &answer, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 0);
}

/* A series' record through the layer: stored with its identifiers. */
static void
test_series_record(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer title;
	AxisMetaSearchAnswer find;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_no_error(error);
	g_assert_nonnull(title.title);
	/* TMDb's record stays the title's; TheTVDB adds its ids, its other names
	 * and its aired order's seasons. */
	g_assert_cmpstr(title.title->provider, ==, "tmdb");
	g_assert_cmpstr(axis_meta_title_get_id(title.title, "imdb"), ==, "tt9000081");
	g_assert_cmpstr(axis_meta_title_get_id(title.title, "tmdb"), ==, "7051");
	g_assert_cmpuint(title.title->alt_titles->len, ==, 3);
	axis_meta_title_answer_clear(&title);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season WHERE source = 'tvdb'"), ==, 3);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE provider = 'tvdb'"), ==, 0);

	/* Found by its IMDb id: the same one record. */
	g_assert_true(axis_meta_find(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "imdb", "tt9000081", WAIT, &find, &error));
	g_assert_cmpuint(find.titles->len, ==, 1);
	/* (The record is TMDb's: its own id is TMDb's.) */
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(find.titles, 0))->provider_id, ==, "7051");
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(find.titles, 0), "tvdb"), ==, "81001");
	axis_meta_search_answer_clear(&find);
	/* By its own id, the series itself. */
	g_assert_true(axis_meta_find(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "tvdb", "81001", WAIT, &find, &error));
	g_assert_cmpuint(find.titles->len, ==, 1);
	axis_meta_search_answer_clear(&find);
	/* Not by a TMDb id (assumed 4): said, and not asked. */
	axis_test_plex_reset_requests(fx->api);
	g_assert_true(axis_meta_find(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "tmdb", "7051", WAIT, &find, &error));
	g_assert_true(find.saved);
	g_assert_nonnull(strstr(find.problem, "cannot look a series up"));
	axis_meta_search_answer_clear(&find);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/search/remoteid/7051"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);
}

/* Spec: Every episode of an order: every page is read, each once, by its
 * number (the address an answer names is never asked). */
static void
test_every_page(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer title;
	AxisMetaSeasonAnswer season;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	const AxisMetaEpisode *episode;

	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_true(axis_meta_get_source_season(fx->meta, title.title->id, "tvdb", 2, WAIT, &season, &error));
	g_assert_no_error(error);
	g_assert_nonnull(season.season);
	g_assert_cmpuint(season.season->episodes->len, ==, 3);
	episode = g_ptr_array_index(season.season->episodes, 2);
	g_assert_cmpstr(episode->name, ==, "Storm Glass");
	g_assert_cmpint(episode->runtime, ==, 24);
	g_assert_cmpstr(episode->air_date, ==, "2024-05-18");
	axis_meta_season_answer_clear(&season);
	axis_meta_title_answer_clear(&title);
	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript, "GET /v4/series/81001/episodes/official/eng?page=0\n"));
	g_assert_nonnull(strstr(transcript, "GET /v4/series/81001/episodes/official/eng?page=1\n"));
	g_assert_nonnull(strstr(transcript, "GET /v4/series/81001/episodes/official/eng?page=2\n"));
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/series/81001/episodes/official/eng"), ==, 3);
	assert_requests_plain(fx->api, NULL);
}

/* Spec: An image on another host: not kept, and nothing is fetched there;
 * the image origin gets no credential of any kind. */
static void
test_images(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *images = NULL;

	axis_test_tvdb_set_record(fx->tvdb, "https://images.example/banners/posters/81001-1.jpg", NULL);
	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_null(title.title->poster_ref);
	axis_meta_title_answer_clear(&title);
	g_assert_null(axis_meta_artwork_find(fx->meta, "tvdb", "https://images.example/banners/posters/81001-1.jpg",
		AXIS_META_ART_POSTER, TRUE));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->images), ==, 0);

	/* The image on TheTVDB's own origin is fetched, without the token. */
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "tvdb", "/banners/posters/81001-1.jpg", AXIS_META_ART_POSTER,
		&hash, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_plex_count(fx->images, "/banners/posters/81001-1.jpg"), ==, 1);
	images = axis_test_plex_transcript(fx->images);
	g_assert_null(strstr(images, "Authorization"));
	g_assert_null(strstr(images, TOKEN_PREFIX));
	g_assert_null(strstr(images, KEY));
}

/* Spec: Testing the key: a login and the list of orders, nothing about any
 * title or the library; what came back is kept for the page. */
static void
test_testing_the_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	AxisMetaProviderStatus status;
	AxisMetaSearchAnswer answer;

	/* A token held is not what is tested: the key is, by a new login. */
	search_ok(fx, "Wobbly Lighthouse", &answer);
	axis_meta_search_answer_clear(&answer);
	axis_test_plex_reset_requests(fx->api);
	g_assert_true(axis_meta_test_key(fx->meta, "tvdb", NULL, &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(summary, ==, "TheTVDB accepted the key and lists 4 episode orders: Aired Order, DVD Order, "
		"Absolute Order, Alternate Order.");
	requests = axis_test_plex_requests(fx->api);
	g_assert_cmpuint(requests->len, ==, 2);
	g_assert_true(g_str_has_prefix(g_ptr_array_index(requests, 0), "POST /v4/login\n"));
	g_assert_true(g_str_has_prefix(g_ptr_array_index(requests, 1), "GET /v4/seasons/types\n"));
	g_assert_cmpuint(logins(fx), ==, 2);
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_true(status.test_ok);
	g_assert_cmpstr(status.test_text, ==, summary);
	g_assert_cmpint(status.test_us, >, 0);
	axis_meta_provider_status_clear(&status);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);

	/* A provider without a test. */
	g_clear_error(&error);
	g_clear_pointer(&summary, g_free);
	g_assert_false(axis_meta_test_key(fx->meta, "tmdb", NULL, &summary, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID);
}

/* Spec: Nothing kept: neither the key nor the token in a stored row, a log
 * line, a stored job or any requested address. */
static void
test_nothing_kept(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer title;
	AxisMetaSeasonAnswer season;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *rows = NULL;
	static const gchar *const tables[] = {
		"meta_title", "meta_external_id", "meta_alt_title", "meta_season", "meta_episode", "meta_search",
		"meta_artwork", "job", "job_schedule", NULL
	};
	guint i;

	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_PIN, PIN, &error));
	search_ok(fx, "Wobbly Lighthouse", &answer);
	axis_meta_search_answer_clear(&answer);
	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_true(axis_meta_get_source_season(fx->meta, title.title->id, "tvdb", 1, WAIT, &season, &error));
	axis_meta_season_answer_clear(&season);
	axis_meta_title_answer_clear(&title);
	g_assert_true(axis_meta_test_key(fx->meta, "tvdb", NULL, &summary, &error));
	axis_test_tvdb_expire(fx->tvdb, 2);
	search_ok(fx, "a made-up name that fails", &answer);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(logins(fx), >=, 3);

	for (i = 0; tables[i] != NULL; i++) {
		g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM %s r WHERE to_jsonb(r)::text LIKE '%%%s%%'"
			" OR to_jsonb(r)::text LIKE '%%%s%%' OR to_jsonb(r)::text LIKE '%%%s%%'", tables[i], KEY, PIN,
			TOKEN_PREFIX);

		if (query_int(fx, sql) != 0)
			g_error("the key, the PIN or a token is in %s", tables[i]);
	}
	/* The settings hold the key and the PIN as they hold TMDb's token (a
	 * secret setting), and never a token. */
	rows = axis_test_db_query(fx->db, "SELECT count(*) FROM setting s WHERE to_jsonb(s)::text LIKE '%"
		TOKEN_PREFIX "%'");
	g_assert_cmpstr(rows, ==, "0");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" KEY "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" PIN "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" TOKEN_PREFIX "*"), ==, 0);
	assert_requests_plain(fx->api, PIN);
}

/* Review of V1/V2, blocker 1: until a series can follow TheTVDB, no route
 * makes a title of TheTVDB's alone: a search, a lookup and a find by either
 * id about a series Axis does not hold keep nothing. */
static void
test_no_tvdb_only_titles(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;

	search_ok(fx, "Wobbly Lighthouse", &answer);
	g_assert_cmpuint(answer.titles->len, ==, 0);
	axis_meta_search_answer_clear(&answer);
	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &title, &error));
	g_assert_null(title.title);
	axis_meta_title_answer_clear(&title);
	g_assert_true(axis_meta_find(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "imdb", "tt9000081", WAIT, &answer, &error));
	g_assert_cmpuint(answer.titles->len, ==, 0);
	axis_meta_search_answer_clear(&answer);
	g_assert_true(axis_meta_find(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "tvdb", "81001", WAIT, &answer, &error));
	g_assert_cmpuint(answer.titles->len, ==, 0);
	axis_meta_search_answer_clear(&answer);
	g_assert_no_error(error);
	/* TheTVDB was asked: it is what it said that was not kept. */
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/search"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season"), ==, 0);
}

/* Review of V1/V2, fault 4: a login that does not answer holds the others
 * no longer than its own deadline, and they fail as TheTVDB not answering
 * instead of logging in again after it. */
typedef struct
{
	Fixture *fx;
	gint64   took_us;
	gboolean saved;
} Asker;

static gpointer
ask_in_thread(gpointer data)
{
	Asker *asker = data;
	AxisMetaSearchAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();

	g_assert_true(axis_meta_search(asker->fx->meta, "tvdb", AXIS_META_KIND_SERIES, "a made-up waiter", 0, 15000,
		&answer, &error));
	asker->took_us = g_get_monotonic_time() - start;
	asker->saved = answer.saved;
	axis_meta_search_answer_clear(&answer);
	return NULL;
}

static void
test_login_waiters_bounded(Fixture *fx, gconstpointer data)
{
	AxisMetaTuning tuning;
	AxisMetaSearchAnswer answer;
	g_autoptr(GError) error = NULL;
	Asker waiter;
	GThread *thread;
	gint64 start;

	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	tuning.background_ms = 20000;
	axis_meta_set_tuning(fx->meta, &tuning);
	axis_test_plex_route(fx->api, "/v4/login", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	memset(&waiter, 0, sizeof(waiter));
	waiter.fx = fx;
	start = g_get_monotonic_time();
	thread = g_thread_new("tvdb-waiter", ask_in_thread, &waiter);
	g_usleep(200 * 1000);
	g_assert_true(axis_meta_search(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "a made-up first", 0, 15000, &answer,
		&error));
	g_assert_true(answer.saved);
	axis_meta_search_answer_clear(&answer);
	g_thread_join(thread);
	g_assert_true(waiter.saved);
	/* One login's deadline for both, not one after the other. */
	g_assert_cmpint(g_get_monotonic_time() - start, <, 2500 * 1000);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/login"), ==, 1);
}

/* Rate limiting and back-off as TMDb's: a 429 pauses the provider. */
static void
test_told_to_wait(Fixture *fx, gconstpointer data)
{
	const gchar *const wait[] = { "Retry-After", "30", NULL };
	AxisMetaSearchAnswer answer;
	AxisMetaProviderStatus status;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) empty = g_bytes_new_static("{}", 2);

	axis_test_plex_route_full(fx->api, "/v4/search", 429, "application/json", empty, wait);
	search_ok(fx, "Wobbly Lighthouse", &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "wait"));
	axis_meta_search_answer_clear(&answer);
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_cmpint(status.limit.paused_until_us, >, g_get_real_time());
	g_assert_cmpfloat(status.limit.rate, ==, 4.0);
	axis_meta_provider_status_clear(&status);
	/* Not asked again while it waits. */
	g_assert_true(axis_meta_search(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "another made-up name", 0, 500, &answer,
		&error));
	g_assert_true(answer.saved);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/v4/search"), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/meta/tvdb/parse/search", test_parse_search);
	g_test_add_func("/meta/tvdb/parse/series", test_parse_series);
	g_test_add_func("/meta/tvdb/parse/episodes", test_parse_episodes);
	g_test_add_func("/meta/tvdb/parse/remote-and-orders", test_parse_remote_and_orders);
	g_test_add_func("/meta/tvdb/parse/refused", test_parse_refused);
	g_test_add_func("/meta/tvdb/parse/huge", test_parse_huge);
	g_test_add_func("/meta/tvdb/forms/image-refs", test_image_refs);
	g_test_add_func("/meta/tvdb/forms/secrets-languages-orders", test_forms);
	g_test_add_func("/meta/tvdb/declaration", test_declaration);

	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/login/search", Fixture, NULL, test_search_logs_in);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/login/pin", Fixture, NULL, test_pin_in_the_login);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/login/expired-token", Fixture, NULL, test_expired_token);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/login/renewed", Fixture, NULL, test_token_renewed);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/login/key-refused", Fixture, NULL, test_key_refused);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/no-key", Fixture, no_key, test_no_key);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/a-film", Fixture, NULL, test_a_film);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/series/record", Fixture, NULL, test_series_record);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/order/every-page", Fixture, NULL, test_every_page);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/images/one-origin", Fixture, NULL, test_images);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/test-key", Fixture, NULL, test_testing_the_key);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/nothing-kept", Fixture, NULL, test_nothing_kept);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/limit/told-to-wait", Fixture, NULL, test_told_to_wait);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/no-tvdb-only-titles", Fixture, bare, test_no_tvdb_only_titles);
	AXIS_TEST_ADD_DB_CASE("/meta/tvdb/login/waiters-bounded", Fixture, NULL, test_login_waiters_bounded);
	return g_test_run();
}
