/*
 * test-meta-unit.c - the metadata layer's parts that need no database: the
 * model's checks, the image header sniffer, the rate limiter, the fixture
 * provider's parser and the freshness rules
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (metadata): Not an image (types and dimensions read from
 * the header, a type that lies refused), Malformed answer (an unreadable
 * answer is a failure, nothing partial), Rate limit and Told to wait (the
 * bucket's arithmetic, the pause, the back-off), and the freshness rules.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-meta-fixture.h"
#include "axis-test-meta-fixture-parse.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-image.h"
#include "meta/axis-meta-limit.h"
#include "meta/axis-meta-model.h"

/* ── The model ──────────────────────────────────────────────────── */

static void
test_identifiers(void)
{
	static const struct
	{
		const gchar *scheme;
		const gchar *value;
		gboolean     ok;
	} table[] = {
		{ "imdb", "tt0600003", TRUE },
		{ "imdb", "tt12345678", TRUE },
		{ "imdb", "tt123456", FALSE },
		{ "imdb", "nm0000206", FALSE },
		{ "imdb", "tt0600003 ", FALSE },
		{ "tmdb", "6003", TRUE },
		{ "tmdb", "0603", FALSE },
		{ "tmdb", "-6003", FALSE },
		{ "tmdb", "1234567890123", FALSE },
		{ "tvdb", "350001", TRUE },
		{ "fixture", "101", TRUE },
		{ "fixture", "a-b", FALSE },
		{ "fixture", "", FALSE },
		{ "x", "1", FALSE },
		{ "Imdb", "tt0600003", FALSE },
		{ "imdb", NULL, FALSE },
		{ NULL, "1", FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (axis_meta_id_valid(table[i].scheme, table[i].value) != table[i].ok)
			g_error("identifier case %u: %s %s", i, table[i].scheme, table[i].value);
	}
}

static void
test_dates_refs_and_codes(void)
{
	g_assert_true(axis_meta_date_valid("1999-03-31"));
	g_assert_true(axis_meta_date_valid("2024-02-29"));
	g_assert_false(axis_meta_date_valid("2023-02-29"));
	g_assert_false(axis_meta_date_valid("1999-13-01"));
	g_assert_false(axis_meta_date_valid("1999-3-31"));
	g_assert_false(axis_meta_date_valid("1800-01-01"));
	g_assert_false(axis_meta_date_valid("1999-03-31T00"));
	g_assert_false(axis_meta_date_valid(NULL));

	g_assert_true(axis_meta_image_ref_valid("/p101.png"));
	g_assert_true(axis_meta_image_ref_valid("/a/b_c-d.jpg"));
	g_assert_false(axis_meta_image_ref_valid("p101.png"));
	g_assert_false(axis_meta_image_ref_valid("/"));
	g_assert_false(axis_meta_image_ref_valid("/a//b.png"));
	g_assert_false(axis_meta_image_ref_valid("/../etc/passwd"));
	g_assert_false(axis_meta_image_ref_valid("/a/./b.png"));
	g_assert_false(axis_meta_image_ref_valid("/a/"));
	g_assert_false(axis_meta_image_ref_valid("/a?b.png"));
	g_assert_false(axis_meta_image_ref_valid("/a%2fb.png"));
	g_assert_false(axis_meta_image_ref_valid("https://elsewhere/x.png"));

	g_assert_true(axis_meta_country_valid("DE"));
	g_assert_false(axis_meta_country_valid("de"));
	g_assert_true(axis_meta_language_valid("en"));
	g_assert_true(axis_meta_language_valid("deu"));
	g_assert_false(axis_meta_language_valid("EN"));
	g_assert_true(axis_meta_provider_name_valid("fixture"));
	g_assert_true(axis_meta_provider_name_valid("tmdb"));
	g_assert_false(axis_meta_provider_name_valid("fixture-b"));
	g_assert_false(axis_meta_provider_name_valid("12abc"));
	g_assert_false(axis_meta_provider_name_valid("abcdefghijklmnopq"));
}

static void
test_clean_text(void)
{
	g_autofree gchar *a = axis_meta_clean_text("  The\tMazework \n\n Reloaded  ", 100);
	g_autofree gchar *b = axis_meta_clean_text("\x01\x02 \x1b[31m", 100);
	g_autofree gchar *c = axis_meta_clean_text("abcdef", 3);
	g_autofree gchar *d = axis_meta_clean_text("bad \xff byte", 100);
	g_autofree gchar *e = axis_meta_clean_text("right\xe2\x80\xaeleft", 100);

	g_assert_cmpstr(a, ==, "The Mazework Reloaded");
	g_assert_cmpstr(b, ==, "[31m");
	g_assert_cmpstr(c, ==, "abc");
	g_assert_true(g_utf8_validate(d, -1, NULL));
	g_assert_null(strchr(d, '\xff'));
	/* A direction override is not kept. */
	g_assert_null(strstr(e, "\xe2\x80\xae"));
	g_assert_null(axis_meta_clean_text("   ", 10));
	g_assert_null(axis_meta_clean_text(NULL, 10));
}

static void
test_query_key(void)
{
	g_autofree gchar *a = axis_meta_query_key("The  Mazework");
	g_autofree gchar *b = axis_meta_query_key("the mazework ");
	g_autofree gchar *c = axis_meta_query_key("\xef\xbc\xb4\xef\xbd\x88\xef\xbd\x85 mazework");	/* full-width "The" */
	g_autofree gchar *long_text = g_strnfill(5000, 'x');
	g_autofree gchar *d = axis_meta_query_key(long_text);
	g_autofree gchar *sent = axis_meta_query_clean(long_text);
	g_autofree gchar *e = axis_meta_query_key("\xef\xb7\xba");	/* expands to 18 characters */

	g_assert_cmpstr(a, ==, b);
	g_assert_cmpstr(a, ==, c);
	g_assert_cmpuint(strlen(d), <=, 300);
	g_assert_cmpuint(g_utf8_strlen(sent, -1), ==, AXIS_META_QUERY_MAX_CHARS);
	g_assert_nonnull(e);
	g_assert_true(g_utf8_validate(e, -1, NULL));
	g_assert_null(axis_meta_query_key(" \t "));
}

static AxisMetaTitle *
good_title(void)
{
	AxisMetaTitle *title = axis_meta_title_new(AXIS_META_KIND_FILM);

	title->provider = g_strdup("fixture");
	title->provider_id = g_strdup("101");
	title->title = g_strdup("The Mazework");
	g_assert_true(axis_meta_title_add_id(title, "fixture", "101"));
	g_assert_true(axis_meta_title_add_id(title, "imdb", "tt0600003"));
	g_assert_false(axis_meta_title_add_id(title, "imdb", "tt0680000"));
	g_assert_false(axis_meta_title_add_id(title, "imdb", "bad"));
	return title;
}

static void
test_title_check(void)
{
	g_autoptr(AxisMetaTitle) title = good_title();
	g_autofree gchar *problem = NULL;

	g_assert_true(axis_meta_title_check(title, "fixture", &problem));
	g_assert_false(axis_meta_title_check(title, "other", NULL));

	g_free(title->title);
	title->title = g_strdup(" padded ");
	g_assert_false(axis_meta_title_check(title, "fixture", &problem));
	g_clear_pointer(&problem, g_free);
	g_free(title->title);
	title->title = g_strdup("The Mazework");

	title->year = 1500;
	g_assert_false(axis_meta_title_check(title, "fixture", NULL));
	title->year = 1999;
	title->poster_ref = g_strdup("/../x");
	g_assert_false(axis_meta_title_check(title, "fixture", NULL));
	g_clear_pointer(&title->poster_ref, g_free);
	title->release_date = g_strdup("1999-02-30");
	g_assert_false(axis_meta_title_check(title, "fixture", NULL));
	g_clear_pointer(&title->release_date, g_free);
	title->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	g_assert_false(axis_meta_title_check(title, "fixture", NULL));	/* a film has no seasons */
	g_clear_pointer(&title->seasons, g_ptr_array_unref);
	g_free(title->provider_id);
	title->provider_id = g_strdup("102");
	g_assert_false(axis_meta_title_check(title, "fixture", NULL));	/* not its own id */
}

/* ── The image sniffer ──────────────────────────────────────────── */

static void
test_sniff_accepted(void)
{
	g_autoptr(GBytes) png = axis_test_meta_png();
	g_autoptr(GBytes) jpeg = axis_test_meta_jpeg(500, 750, 2000);
	g_autoptr(GBytes) webp = axis_test_meta_webp(342, 513, 600);
	AxisMetaImageInfo info;
	gsize n;
	const guint8 *data;

	data = g_bytes_get_data(png, &n);
	g_assert_true(axis_meta_image_sniff(data, n, &info));
	g_assert_cmpint(info.type, ==, AXIS_META_IMAGE_PNG);
	g_assert_cmpuint(info.width, ==, 180);
	g_assert_cmpuint(info.height, ==, 180);

	data = g_bytes_get_data(jpeg, &n);
	g_assert_true(axis_meta_image_sniff(data, n, &info));
	g_assert_cmpint(info.type, ==, AXIS_META_IMAGE_JPEG);
	g_assert_cmpuint(info.width, ==, 500);
	g_assert_cmpuint(info.height, ==, 750);

	data = g_bytes_get_data(webp, &n);
	g_assert_true(axis_meta_image_sniff(data, n, &info));
	g_assert_cmpint(info.type, ==, AXIS_META_IMAGE_WEBP);
	g_assert_cmpuint(info.width, ==, 342);
	g_assert_cmpuint(info.height, ==, 513);
}

/* A copy of @bytes, and its length (order of evaluation matters). */
static guint8 *
copy_of(
	GBytes *bytes,
	gsize  *length
){
	gconstpointer data = g_bytes_get_data(bytes, length);

	return g_memdup2(data, *length);
}

static void
test_sniff_refused(void)
{
	static const gchar html[] = "<!DOCTYPE html><html><script>alert(1)</script></html>";
	static const guint8 png_sig_only[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	g_autoptr(GBytes) png = axis_test_meta_png();
	g_autoptr(GBytes) jpeg = axis_test_meta_jpeg(500, 750, 2000);
	g_autoptr(GBytes) webp = axis_test_meta_webp(342, 513, 600);
	AxisMetaImageInfo info;
	guint8 *copy;
	gsize n;

	g_assert_false(axis_meta_image_sniff((const guint8 *)html, sizeof(html) - 1, &info));
	g_assert_cmpint(info.type, ==, AXIS_META_IMAGE_NONE);
	g_assert_false(axis_meta_image_sniff(png_sig_only, sizeof(png_sig_only), &info));
	g_assert_false(axis_meta_image_sniff(NULL, 0, &info));

	/* A PNG whose IHDR says width 0. */
	copy = copy_of(png, &n);
	memset(copy + 16, 0, 4);
	g_assert_false(axis_meta_image_sniff(copy, n, &info));
	g_free(copy);
	/* A PNG with a colour type it cannot have. */
	copy = copy_of(png, &n);
	copy[25] = 5;
	g_assert_false(axis_meta_image_sniff(copy, n, &info));
	g_free(copy);

	/* A JPEG whose frame header is cut off, and one with a scan first. */
	g_assert_false(axis_meta_image_sniff(g_bytes_get_data(jpeg, NULL), 28, &info));
	copy = copy_of(jpeg, &n);
	copy[21] = 0xDA;
	g_assert_false(axis_meta_image_sniff(copy, n, &info));
	g_free(copy);
	/* Zero height. */
	copy = copy_of(jpeg, &n);
	copy[25] = 0;
	copy[26] = 0;
	g_assert_false(axis_meta_image_sniff(copy, n, &info));
	g_free(copy);

	/* A WebP whose RIFF says it is longer than it is. */
	copy = copy_of(webp, &n);
	g_assert_false(axis_meta_image_sniff(copy, n - 1, &info));
	copy[20] = 0x2E;	/* not the VP8L signature */
	g_assert_false(axis_meta_image_sniff(copy, n, &info));
	g_free(copy);

	g_assert_cmpint(axis_meta_image_type_from_mime("image/jpg"), ==, AXIS_META_IMAGE_JPEG);
	g_assert_cmpint(axis_meta_image_type_from_mime("IMAGE/PNG"), ==, AXIS_META_IMAGE_PNG);
	g_assert_cmpint(axis_meta_image_type_from_mime("image/svg+xml"), ==, AXIS_META_IMAGE_NONE);
	g_assert_cmpint(axis_meta_image_type_from_mime("text/html"), ==, AXIS_META_IMAGE_NONE);
}

/* ── The limiter ────────────────────────────────────────────────── */

static void
test_gcra(void)
{
	gint64 tat = 0;
	const gint64 interval = 200000;	/* 5 a second */

	/* A burst of two at once, then one every interval. */
	g_assert_cmpint(axis_meta_gcra(&tat, 1000000, interval, 2, TRUE), ==, 1000000);
	g_assert_cmpint(axis_meta_gcra(&tat, 1000000, interval, 2, TRUE), ==, 1000000);
	g_assert_cmpint(axis_meta_gcra(&tat, 1000000, interval, 2, TRUE), ==, 1200000);
	g_assert_cmpint(axis_meta_gcra(&tat, 1200000, interval, 2, TRUE), ==, 1200000);
	g_assert_cmpint(axis_meta_gcra(&tat, 1200000, interval, 2, FALSE), ==, 1400000);
	/* After a quiet spell the burst is back, never more. */
	g_assert_cmpint(axis_meta_gcra(&tat, 9000000, interval, 2, TRUE), ==, 9000000);
	g_assert_cmpint(axis_meta_gcra(&tat, 9000000, interval, 2, TRUE), ==, 9000000);
	g_assert_cmpint(axis_meta_gcra(&tat, 9000000, interval, 2, TRUE), ==, 9200000);
}

static void
test_limiter_rate(void)
{
	AxisMetaLimiter *limiter = axis_meta_limiter_new(20.0, 1);
	gint64 start = g_get_monotonic_time();
	guint i;

	for (i = 0; i < 10; i++)
		g_assert_cmpint(axis_meta_limiter_acquire(limiter, G_MAXINT64, NULL, NULL), ==, AXIS_META_LIMIT_OK);
	/* Ten at 20 a second with no burst: nine intervals of 50 ms. */
	g_assert_cmpint(g_get_monotonic_time() - start, >=, 440000);
	/* With a deadline that comes first, no wait. */
	g_assert_cmpint(axis_meta_limiter_acquire(limiter, g_get_monotonic_time(), NULL, NULL), ==,
		AXIS_META_LIMIT_DEADLINE);
	axis_meta_limiter_free(limiter);
}

static void
test_limiter_pause_and_backoff(void)
{
	AxisMetaLimiter *limiter = axis_meta_limiter_new(50.0, 5);
	AxisMetaLimitStatus status;
	gint64 until = 0;
	gint64 start;

	axis_meta_limiter_pause(limiter, 1, "asked to wait");
	start = g_get_monotonic_time();
	/* Refused at once when the pause outlasts the deadline. */
	g_assert_cmpint(axis_meta_limiter_acquire(limiter, start + 200000, NULL, &until), ==, AXIS_META_LIMIT_PAUSED);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 100000);
	g_assert_cmpint(until, >, g_get_real_time());
	/* Waited for when the deadline allows. */
	g_assert_cmpint(axis_meta_limiter_acquire(limiter, start + 3 * G_USEC_PER_SEC, NULL, NULL), ==,
		AXIS_META_LIMIT_OK);
	g_assert_cmpint(g_get_monotonic_time() - start, >=, 900000);

	/* One failure is not a back-off; two are; an answer ends it. */
	axis_meta_limiter_failed(limiter, "first");
	memset(&status, 0, sizeof(status));
	axis_meta_limiter_get_status(limiter, &status);
	g_assert_cmpuint(status.failures, ==, 1);
	g_assert_cmpint(status.backoff_until_us, ==, 0);
	axis_meta_limit_status_clear(&status);
	axis_meta_limiter_failed(limiter, "second");
	axis_meta_limiter_get_status(limiter, &status);
	g_assert_cmpint(status.backoff_until_us, >, g_get_real_time());
	g_assert_cmpstr(status.last_error, ==, "second");
	axis_meta_limit_status_clear(&status);
	g_assert_cmpint(axis_meta_limiter_acquire(limiter, g_get_monotonic_time() + 100000, NULL, NULL), ==,
		AXIS_META_LIMIT_PAUSED);
	axis_meta_limiter_answered(limiter);
	g_assert_cmpint(axis_meta_limiter_acquire(limiter, g_get_monotonic_time() + 100000, NULL, NULL), ==,
		AXIS_META_LIMIT_OK);
	axis_meta_limiter_free(limiter);
}

typedef struct
{
	GCancellable *cancellable;
} Canceller;

static gpointer
cancel_soon(gpointer data)
{
	Canceller *c = data;

	g_usleep(100000);
	g_cancellable_cancel(c->cancellable);
	return NULL;
}

static void
test_limiter_cancel(void)
{
	AxisMetaLimiter *limiter = axis_meta_limiter_new(1.0, 1);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Canceller c = { cancellable };
	GThread *thread;
	gint64 start;

	axis_meta_limiter_pause(limiter, 30, NULL);
	thread = g_thread_new("cancel", cancel_soon, &c);
	start = g_get_monotonic_time();
	g_assert_cmpint(axis_meta_limiter_acquire(limiter, G_MAXINT64, cancellable, NULL), ==, AXIS_META_LIMIT_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 2 * G_USEC_PER_SEC);
	g_thread_join(thread);
	axis_meta_limiter_free(limiter);
}

/* ── The fixture provider's parser ──────────────────────────────── */

static gchar *
read_fixture(
	const gchar *name,
	gsize       *length
){
	g_autofree gchar *path = g_build_filename("tests/fixtures/meta", name, NULL);
	gchar *data = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(path, &data, length, &error))
		g_error("%s", error->message);
	return data;
}

static void
test_fixture_results(void)
{
	gsize length = 0;
	g_autofree gchar *data = read_fixture("search-film.json", &length);
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	g_assert_true(axis_test_meta_fixture_parse_results("fixture", AXIS_META_KIND_FILM, data, length, &titles, &error));
	/* The entry without a title, the series and the string are left out. */
	g_assert_cmpuint(titles->len, ==, 3);
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);
		g_autofree gchar *problem = NULL;

		if (!axis_meta_title_check(title, "fixture", &problem))
			g_error("result %u fails its check: %s", i, problem);
		g_assert_null(title->genres);
		g_assert_null(title->alt_titles);
	}
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(titles, 2))->provider_id, ==, "103");
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(titles, 0), "imdb"), ==, "tt0600003");
}

static void
test_fixture_title_and_season(void)
{
	gsize length = 0;
	g_autofree gchar *film = read_fixture("title-film-101.json", &length);
	g_autofree gchar *series = NULL;
	g_autofree gchar *season = NULL;
	g_autoptr(AxisMetaTitle) title = NULL;
	g_autoptr(AxisMetaTitle) show = NULL;
	g_autoptr(AxisMetaSeason) two = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_test_meta_fixture_parse_title("fixture", AXIS_META_KIND_FILM, film, length, &title, &error));
	g_assert_true(axis_meta_title_check(title, "fixture", NULL));
	g_assert_cmpuint(title->genres->len, ==, 2);
	g_assert_cmpuint(title->alt_titles->len, ==, 2);
	g_assert_cmpint(title->status, ==, AXIS_META_STATUS_RELEASED);
	g_assert_cmpstr(title->release_date, ==, "1999-03-31");
	/* Asked for a series, the film is not one. */
	g_assert_false(axis_test_meta_fixture_parse_title("fixture", AXIS_META_KIND_SERIES, film, length, &show, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
	g_clear_error(&error);

	series = read_fixture("title-series-201.json", &length);
	g_assert_true(axis_test_meta_fixture_parse_title("fixture", AXIS_META_KIND_SERIES, series, length, &show, &error));
	g_assert_true(axis_meta_title_check(show, "fixture", NULL));
	g_assert_cmpuint(show->seasons->len, ==, 2);

	season = read_fixture("season-201-2.json", &length);
	g_assert_true(axis_test_meta_fixture_parse_season(2, season, length, &two, &error));
	g_assert_true(axis_meta_season_check(two, NULL));
	g_assert_cmpuint(two->episodes->len, ==, 2);
	g_clear_pointer(&two, axis_meta_season_free);
	g_assert_false(axis_test_meta_fixture_parse_season(3, season, length, &two, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
}

static void
test_fixture_malformed(void)
{
	static const gchar *const bad[] = {
		"", "{not json", "[]", "{\"results\": 3}", "{\"results\": [", "null", "\"text\"", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GPtrArray) titles = NULL;
		g_autoptr(AxisMetaTitle) title = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_test_meta_fixture_parse_results("fixture", AXIS_META_KIND_FILM, bad[i], strlen(bad[i]),
			&titles, &error));
		g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
		g_assert_null(titles);
		g_clear_error(&error);
		g_assert_false(axis_test_meta_fixture_parse_title("fixture", AXIS_META_KIND_FILM, bad[i], strlen(bad[i]),
			&title, &error));
		g_assert_null(title);
	}
}

/* ── Freshness ──────────────────────────────────────────────────── */

static void
test_freshness_rules(void)
{
	AxisMetaFreshness f;
	g_autoptr(AxisMetaTitle) film = axis_meta_title_new(AXIS_META_KIND_FILM);
	g_autoptr(AxisMetaTitle) series = axis_meta_title_new(AXIS_META_KIND_SERIES);

	f.search = 1;
	f.unreleased = 2;
	f.released = 3;
	f.ended = 4;
	f.current_season = 5;
	f.negative = 6;
	f.viewed = 7;

	film->status = AXIS_META_STATUS_RELEASED;
	g_assert_cmpint(axis_meta_title_period(film, &f, "2026-11-13"), ==, 3);
	film->status = AXIS_META_STATUS_UPCOMING;
	g_assert_cmpint(axis_meta_title_period(film, &f, "2026-11-13"), ==, 2);
	film->status = AXIS_META_STATUS_UNKNOWN;
	g_assert_cmpint(axis_meta_title_period(film, &f, "2026-11-13"), ==, 2);
	film->release_date = g_strdup("2027-01-01");
	g_assert_cmpint(axis_meta_title_period(film, &f, "2026-11-13"), ==, 2);
	g_free(film->release_date);
	film->release_date = g_strdup("2026-11-13");
	g_assert_cmpint(axis_meta_title_period(film, &f, "2026-11-13"), ==, 3);

	series->status = AXIS_META_STATUS_CONTINUING;
	g_assert_cmpint(axis_meta_title_period(series, &f, "2026-11-13"), ==, 2);
	series->status = AXIS_META_STATUS_UNKNOWN;
	g_assert_cmpint(axis_meta_title_period(series, &f, "2026-11-13"), ==, 2);
	series->status = AXIS_META_STATUS_ENDED;
	g_assert_cmpint(axis_meta_title_period(series, &f, "2026-11-13"), ==, 4);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/meta-unit/model/identifiers", test_identifiers);
	g_test_add_func("/meta-unit/model/dates-refs-codes", test_dates_refs_and_codes);
	g_test_add_func("/meta-unit/model/clean-text", test_clean_text);
	g_test_add_func("/meta-unit/model/query-key", test_query_key);
	g_test_add_func("/meta-unit/model/title-check", test_title_check);
	g_test_add_func("/meta-unit/image/accepted", test_sniff_accepted);
	g_test_add_func("/meta-unit/image/refused", test_sniff_refused);
	g_test_add_func("/meta-unit/limit/gcra", test_gcra);
	g_test_add_func("/meta-unit/limit/rate", test_limiter_rate);
	g_test_add_func("/meta-unit/limit/pause-and-backoff", test_limiter_pause_and_backoff);
	g_test_add_func("/meta-unit/limit/cancel", test_limiter_cancel);
	g_test_add_func("/meta-unit/fixture/results", test_fixture_results);
	g_test_add_func("/meta-unit/fixture/title-and-season", test_fixture_title_and_season);
	g_test_add_func("/meta-unit/fixture/malformed", test_fixture_malformed);
	g_test_add_func("/meta-unit/freshness", test_freshness_rules);
	return g_test_run();
}
