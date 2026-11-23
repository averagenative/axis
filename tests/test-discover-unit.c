/*
 * test-discover-unit.c - TMDb's lists and genres read into the metadata
 * model, their paths, and the checks a list's answer passes before anything
 * of it is stored; no database, no network
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (add-discover, metadata): A list not offered (the paths);
 * An unreadable answer; Coming soon in the region (the path's rules); No
 * vote, no rating.  Tasks 1.1, 1.3 and 1.4.
 */

#include <glib.h>
#include <string.h>

#include "meta/axis-meta-model.h"
#include "meta/axis-meta-provider.h"
#include "meta/axis-meta-tmdb-parse.h"

#define DIR "tests/fixtures/discover/"

static gchar *
read_fixture(
	const gchar *name,
	gsize       *length
){
	g_autofree gchar *path = g_strconcat(DIR, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, length, &error));
	g_assert_no_error(error);
	return data;
}

static AxisMetaListPage *
parse_fixture(
	AxisMetaKind  kind,
	const gchar  *name,
	gboolean      adult
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *data = NULL;
	AxisMetaListPage *page = NULL;
	gsize length = 0;

	data = read_fixture(name, &length);
	g_assert_true(axis_meta_tmdb_parse_list(kind, data, length, adult, &page, &error));
	g_assert_no_error(error);
	g_assert_nonnull(page);
	return page;
}

static const AxisMetaListEntry *
entry_of(
	const AxisMetaListPage *page,
	const gchar            *id
){
	guint i;

	for (i = 0; i < page->entries->len; i++) {
		const AxisMetaListEntry *entry = g_ptr_array_index(page->entries, i);

		if (strcmp(entry->title->provider_id, id) == 0)
			return entry;
	}
	return NULL;
}

/* ── Lists ──────────────────────────────────────────────────────── */

static void
test_parse_popular_movies(void)
{
	g_autoptr(AxisMetaListPage) page = parse_fixture(AXIS_META_KIND_FILM, "movie-popular.json", FALSE);
	const AxisMetaListEntry *orchard;
	const AxisMetaListEntry *lanterns;
	const AxisMetaListEntry *signal;
	g_autofree gchar *problem = NULL;

	/* Six in the answer, one of them adult. */
	g_assert_cmpuint(page->entries->len, ==, 5);
	g_assert_cmpuint(page->total_pages, ==, 2);
	g_assert_null(entry_of(page, "9006"));
	orchard = g_ptr_array_index(page->entries, 0);
	g_assert_cmpstr(orchard->title->provider_id, ==, "9001");
	g_assert_cmpstr(orchard->title->title, ==, "The Brass Orchard");
	g_assert_cmpint(orchard->title->year, ==, 2024);
	g_assert_cmpstr(orchard->title->poster_ref, ==, "/p9001.jpg");
	g_assert_false(orchard->title->detail);
	/* 7.36 is 74 tenths; the votes are the count. */
	g_assert_cmpint(orchard->rating, ==, 74);
	g_assert_cmpint(orchard->votes, ==, 1204);
	g_assert_cmpuint(orchard->genre_ids->len, ==, 2);
	g_assert_cmpint(g_array_index(orchard->genre_ids, gint, 0), ==, 18);
	g_assert_cmpint(g_array_index(orchard->genre_ids, gint, 1), ==, 9648);
	/* A genre listed twice is kept once. */
	lanterns = entry_of(page, "9002");
	g_assert_nonnull(lanterns);
	g_assert_cmpint(lanterns->rating, ==, 62);
	g_assert_cmpuint(lanterns->genre_ids->len, ==, 2);
	/* Nobody voted: no rating, whatever the average says. */
	signal = entry_of(page, "9005");
	g_assert_nonnull(signal);
	g_assert_cmpint(signal->rating, ==, -1);
	g_assert_cmpint(signal->votes, ==, 0);
	g_assert_cmpint(entry_of(page, "9004")->rating, ==, 69);
	g_assert_true(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", &problem));
	g_assert_null(problem);
}

static void
test_parse_adult(void)
{
	g_autoptr(AxisMetaListPage) page = parse_fixture(AXIS_META_KIND_FILM, "movie-popular.json", TRUE);

	g_assert_cmpuint(page->entries->len, ==, 6);
	g_assert_nonnull(entry_of(page, "9006"));
}

static void
test_parse_other_kinds_dropped(void)
{
	g_autoptr(AxisMetaListPage) movies = parse_fixture(AXIS_META_KIND_FILM, "trending-movie-week.json", FALSE);
	g_autoptr(AxisMetaListPage) series = parse_fixture(AXIS_META_KIND_SERIES, "trending-tv-week.json", FALSE);
	g_autoptr(AxisMetaListPage) popular = parse_fixture(AXIS_META_KIND_SERIES, "tv-popular.json", FALSE);
	const AxisMetaListEntry *archive;

	g_assert_cmpuint(movies->entries->len, ==, 2);
	g_assert_null(entry_of(movies, "9501"));
	g_assert_cmpuint(series->entries->len, ==, 1);
	g_assert_null(entry_of(series, "9010"));
	archive = entry_of(popular, "9601");
	g_assert_nonnull(archive);
	g_assert_cmpint(archive->title->kind, ==, AXIS_META_KIND_SERIES);
	g_assert_cmpstr(archive->title->title, ==, "The Tidewater Archive");
	g_assert_cmpint(archive->title->year, ==, 2024);
	g_assert_cmpint(archive->rating, ==, 82);
}

static void
test_parse_wrong_types(void)
{
	g_autoptr(AxisMetaListPage) page = parse_fixture(AXIS_META_KIND_FILM, "list-wrong-types.json", FALSE);
	const AxisMetaListEntry *typed;
	const AxisMetaListEntry *ranged;
	const AxisMetaListEntry *ten;
	g_autofree gchar *problem = NULL;

	/* An id as text and an entry without a name are no titles. */
	g_assert_cmpuint(page->entries->len, ==, 3);
	g_assert_cmpuint(page->total_pages, ==, 0);
	typed = entry_of(page, "9033");
	g_assert_nonnull(typed);
	g_assert_cmpint(typed->title->year, ==, 0);
	g_assert_null(typed->title->poster_ref);
	g_assert_null(typed->title->original_language);
	g_assert_cmpuint(typed->genre_ids->len, ==, 0);
	g_assert_cmpint(typed->rating, ==, -1);
	g_assert_cmpint(typed->votes, ==, 0);
	ranged = entry_of(page, "9034");
	g_assert_nonnull(ranged);
	g_assert_cmpuint(ranged->genre_ids->len, ==, 2);
	g_assert_cmpint(g_array_index(ranged->genre_ids, gint, 0), ==, 18);
	g_assert_cmpint(g_array_index(ranged->genre_ids, gint, 1), ==, 35);
	/* An average above ten is no rating. */
	g_assert_cmpint(ranged->rating, ==, -1);
	ten = entry_of(page, "9035");
	g_assert_cmpint(ten->rating, ==, 100);
	g_assert_true(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", &problem));
}

static void
test_parse_refused(void)
{
	static const gchar *const bodies[] = { "", "[1, 2]", "{\"results\": {}}", "{\"page\": 1}", "not json", NULL };
	g_autofree gchar *data = NULL;
	g_autoptr(GError) error = NULL;
	AxisMetaListPage *page = NULL;
	GPtrArray *genres = NULL;
	gsize length = 0;
	guint i;

	for (i = 0; bodies[i] != NULL; i++) {
		g_clear_error(&error);
		g_assert_false(axis_meta_tmdb_parse_list(AXIS_META_KIND_FILM, bodies[i], strlen(bodies[i]), FALSE, &page,
			&error));
		g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
		g_assert_null(page);
	}
	data = read_fixture("list-no-results.json", &length);
	g_clear_error(&error);
	g_assert_false(axis_meta_tmdb_parse_list(AXIS_META_KIND_SERIES, data, length, FALSE, &page, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
	g_clear_error(&error);
	g_assert_false(axis_meta_tmdb_parse_genres("[]", 2, &genres, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
	g_assert_null(genres);
	g_clear_error(&error);
	g_assert_false(axis_meta_tmdb_parse_genres("{\"genres\": 3}", 13, &genres, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
}

/* Fifty results at most, whatever the answer holds; pages beyond TMDb's own
 * limit are not believed. */
static void
test_parse_huge(void)
{
	g_autoptr(GString) json = g_string_new("{\"total_pages\": 90000, \"results\": [");
	g_autoptr(AxisMetaListPage) page = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	for (i = 0; i < 400; i++)
		g_string_append_printf(json, "%s{\"id\": %u, \"title\": \"Generated %u\", \"vote_count\": 3,"
			" \"vote_average\": 5.5, \"genre_ids\": [%u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u,"
			" %u, %u, %u, %u, %u, %u, %u, %u, %u, %u]}", i > 0 ? "," : "", 100 + i, i,
			1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25);
	g_string_append(json, "]}");
	g_assert_true(axis_meta_tmdb_parse_list(AXIS_META_KIND_FILM, json->str, json->len, FALSE, &page, &error));
	g_assert_cmpuint(page->entries->len, ==, AXIS_META_MAX_RESULTS);
	g_assert_cmpuint(page->total_pages, ==, AXIS_META_LIST_MAX_PAGE);
	g_assert_cmpuint(((AxisMetaListEntry *)g_ptr_array_index(page->entries, 0))->genre_ids->len, ==,
		AXIS_META_MAX_GENRES);
	g_assert_cmpint(((AxisMetaListEntry *)g_ptr_array_index(page->entries, 0))->rating, ==, 55);
	g_assert_true(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
}

static void
test_parse_genres(void)
{
	g_autofree gchar *data = NULL;
	g_autoptr(GPtrArray) genres = NULL;
	g_autoptr(GError) error = NULL;
	const AxisMetaGenre *genre;
	gsize length = 0;
	guint i;

	data = read_fixture("genre-movie-list.json", &length);
	g_assert_true(axis_meta_tmdb_parse_genres(data, length, &genres, &error));
	g_assert_no_error(error);
	/* Sixteen in the answer: a number twice, a number 0 and an empty name. */
	g_assert_cmpuint(genres->len, ==, 13);
	genre = g_ptr_array_index(genres, 0);
	g_assert_cmpint(genre->id, ==, 28);
	g_assert_cmpstr(genre->name, ==, "Action");
	for (i = 0; i < genres->len; i++) {
		genre = g_ptr_array_index(genres, i);
		if (genre->id == 53)
			g_assert_cmpstr(genre->name, ==, "Thriller");
	}
	g_assert_true(axis_meta_genres_check(genres, NULL));
}

/* ── The paths ──────────────────────────────────────────────────── */

static void
test_paths(void)
{
	static const struct
	{
		AxisMetaKind  kind;
		AxisMetaList  list;
		const gchar  *id;
		const gchar  *path;
		gboolean      regional;
	} cases[] = {
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_POPULAR, NULL, "/3/movie/popular", TRUE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_TRENDING, NULL, "/3/trending/movie/week", FALSE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_NOW, NULL, "/3/movie/now_playing", TRUE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_UPCOMING, NULL, "/3/movie/upcoming", TRUE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_TOP_RATED, NULL, "/3/movie/top_rated", TRUE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_RECOMMENDED, "9001", "/3/movie/9001/recommendations", FALSE },
		{ AXIS_META_KIND_SERIES, AXIS_META_LIST_POPULAR, NULL, "/3/tv/popular", FALSE },
		{ AXIS_META_KIND_SERIES, AXIS_META_LIST_TRENDING, NULL, "/3/trending/tv/week", FALSE },
		{ AXIS_META_KIND_SERIES, AXIS_META_LIST_NOW, NULL, "/3/tv/on_the_air", FALSE },
		{ AXIS_META_KIND_SERIES, AXIS_META_LIST_UPCOMING, NULL, NULL, FALSE },
		{ AXIS_META_KIND_SERIES, AXIS_META_LIST_TOP_RATED, NULL, "/3/tv/top_rated", FALSE },
		{ AXIS_META_KIND_SERIES, AXIS_META_LIST_RECOMMENDED, "9601", "/3/tv/9601/recommendations", FALSE },
		/* Recommendations are of one title, and only they are. */
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_RECOMMENDED, NULL, NULL, FALSE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_POPULAR, "9001", NULL, TRUE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_RECOMMENDED, "../9001", NULL, FALSE },
		{ AXIS_META_KIND_FILM, AXIS_META_LIST_RECOMMENDED, "0123", NULL, FALSE },
		{ AXIS_META_KIND_FILM, (AxisMetaList)17, NULL, NULL, FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *path = axis_meta_tmdb_list_path(cases[i].kind, cases[i].list, cases[i].id);

		g_assert_cmpstr(path, ==, cases[i].path);
		if (cases[i].path != NULL)
			g_assert_cmpint(axis_meta_tmdb_list_regional(cases[i].kind, cases[i].list), ==, cases[i].regional);
	}
	/* What the provider declares matches the paths it has. */
	for (i = 0; i < AXIS_META_N_LISTS; i++) {
		g_autofree gchar *film = axis_meta_tmdb_list_path(AXIS_META_KIND_FILM, (AxisMetaList)i,
			i == AXIS_META_LIST_RECOMMENDED ? "1" : NULL);
		g_autofree gchar *series = axis_meta_tmdb_list_path(AXIS_META_KIND_SERIES, (AxisMetaList)i,
			i == AXIS_META_LIST_RECOMMENDED ? "1" : NULL);

		g_assert_cmpint(film != NULL, ==, (AXIS_META_TMDB_FILM_LISTS & AXIS_META_LIST_BIT(i)) != 0);
		g_assert_cmpint(series != NULL, ==, (AXIS_META_TMDB_SERIES_LISTS & AXIS_META_LIST_BIT(i)) != 0);
	}
}

static void
test_list_names(void)
{
	AxisMetaList list;
	guint i;

	for (i = 0; i < AXIS_META_N_LISTS; i++) {
		const gchar *name = axis_meta_list_to_string((AxisMetaList)i);

		g_assert_nonnull(name);
		g_assert_true(axis_meta_list_from_string(name, &list));
		g_assert_cmpint(list, ==, i);
	}
	g_assert_null(axis_meta_list_to_string((AxisMetaList)AXIS_META_N_LISTS));
	g_assert_false(axis_meta_list_from_string("Popular", &list));
	g_assert_false(axis_meta_list_from_string(NULL, &list));
	g_assert_false(axis_meta_list_from_string("", &list));
}

/* ── The checks ─────────────────────────────────────────────────── */

static AxisMetaListEntry *
entry_new(
	AxisMetaKind  kind,
	const gchar  *id
){
	AxisMetaTitle *title = axis_meta_title_new(kind);

	title->provider = g_strdup("tmdb");
	title->provider_id = g_strdup(id);
	title->title = g_strdup("A made-up title");
	axis_meta_title_add_id(title, "tmdb", id);
	return axis_meta_list_entry_new(title);
}

static void
test_page_check(void)
{
	g_autoptr(AxisMetaListPage) page = axis_meta_list_page_new();
	AxisMetaListEntry *first = entry_new(AXIS_META_KIND_FILM, "11");
	AxisMetaListEntry *second = entry_new(AXIS_META_KIND_FILM, "12");
	g_autofree gchar *problem = NULL;
	gint genre = 18;
	guint i;

	g_ptr_array_add(page->entries, first);
	g_ptr_array_add(page->entries, second);
	g_assert_true(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", &problem));
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_SERIES, "tmdb", &problem));
	g_clear_pointer(&problem, g_free);
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "other", &problem));
	g_clear_pointer(&problem, g_free);

	second->rating = 101;
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", &problem));
	g_assert_nonnull(strstr(problem, "rating"));
	g_clear_pointer(&problem, g_free);
	second->rating = -1;
	second->votes = -2;
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
	second->votes = 0;
	g_array_append_val(second->genre_ids, genre);
	g_array_append_val(second->genre_ids, genre);
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
	g_array_set_size(second->genre_ids, 0);
	for (i = 0; i <= AXIS_META_MAX_GENRES; i++) {
		gint value = (gint)i + 1;

		g_array_append_val(second->genre_ids, value);
	}
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
	g_array_set_size(second->genre_ids, 0);
	/* A full record is not a list's entry. */
	second->title->detail = TRUE;
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
	second->title->detail = FALSE;
	/* The same title twice. */
	g_ptr_array_add(page->entries, entry_new(AXIS_META_KIND_FILM, "11"));
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", &problem));
	g_assert_nonnull(strstr(problem, "twice"));
	g_ptr_array_remove_index(page->entries, 2);
	page->total_pages = AXIS_META_LIST_MAX_PAGE + 1;
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
	page->total_pages = 1;
	for (i = 0; i < AXIS_META_MAX_RESULTS; i++) {
		g_autofree gchar *id = g_strdup_printf("%u", 100 + i);

		g_ptr_array_add(page->entries, entry_new(AXIS_META_KIND_FILM, id));
	}
	g_assert_false(axis_meta_list_page_check(page, AXIS_META_KIND_FILM, "tmdb", NULL));
}

static void
test_genres_check(void)
{
	g_autoptr(GPtrArray) genres = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_genre_free);
	AxisMetaGenre *genre = g_new0(AxisMetaGenre, 1);
	AxisMetaGenre *twice = g_new0(AxisMetaGenre, 1);

	genre->id = 18;
	genre->name = g_strdup("Drama");
	g_ptr_array_add(genres, genre);
	g_assert_true(axis_meta_genres_check(genres, NULL));
	twice->id = 18;
	twice->name = g_strdup("Drama again");
	g_ptr_array_add(genres, twice);
	g_assert_false(axis_meta_genres_check(genres, NULL));
	twice->id = 19;
	g_free(twice->name);
	twice->name = g_strdup(" not clean ");
	g_assert_false(axis_meta_genres_check(genres, NULL));
	g_free(twice->name);
	twice->name = g_strdup("History");
	twice->id = 0;
	g_assert_false(axis_meta_genres_check(genres, NULL));
	g_assert_false(axis_meta_genres_check(NULL, NULL));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/discover/tmdb/popular-movies", test_parse_popular_movies);
	g_test_add_func("/discover/tmdb/adult", test_parse_adult);
	g_test_add_func("/discover/tmdb/other-kinds-dropped", test_parse_other_kinds_dropped);
	g_test_add_func("/discover/tmdb/wrong-types", test_parse_wrong_types);
	g_test_add_func("/discover/tmdb/refused", test_parse_refused);
	g_test_add_func("/discover/tmdb/huge", test_parse_huge);
	g_test_add_func("/discover/tmdb/genres", test_parse_genres);
	g_test_add_func("/discover/tmdb/paths", test_paths);
	g_test_add_func("/discover/model/list-names", test_list_names);
	g_test_add_func("/discover/model/page-check", test_page_check);
	g_test_add_func("/discover/model/genres-check", test_genres_check);
	return g_test_run();
}
