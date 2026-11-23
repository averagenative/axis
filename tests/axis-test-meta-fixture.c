/*
 * axis-test-meta-fixture.c - test support: the "fixture" metadata provider
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-meta-fixture.h"

#include <string.h>

#include "axis-test-meta-fixture-parse.h"

static gchar *
escape(const gchar *text)
{
	return g_uri_escape_string(text, NULL, FALSE);
}

static gboolean
get(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	GError        **error
){
	return axis_meta_fetch_get(fetch, path, body, error);
}

static gboolean
fixture_search(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *text,
	gint                         year,
	guint                        page,
	GPtrArray                  **results,
	GError                     **error
){
	g_autofree gchar *q = escape(text);
	g_autofree gchar *with_page = page > 1 ? g_strdup_printf("&page=%u", page) : g_strdup("");
	g_autofree gchar *path = year != 0 ?
		g_strdup_printf("/search/%s?q=%s&year=%d%s", axis_meta_kind_to_string(kind), q, year, with_page) :
		g_strdup_printf("/search/%s?q=%s%s", axis_meta_kind_to_string(kind), q, with_page);
	g_autoptr(GBytes) body = NULL;
	gsize length = 0;
	const gchar *data;

	if (!get(fetch, path, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	return axis_test_meta_fixture_parse_results(info->name, kind, data, length, results, error);
}

static gboolean
fixture_title(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *id,
	AxisMetaTitle              **title,
	GError                     **error
){
	g_autofree gchar *escaped = escape(id);
	g_autofree gchar *path = g_strdup_printf("/title/%s/%s", axis_meta_kind_to_string(kind), escaped);
	g_autoptr(GBytes) body = NULL;
	gsize length = 0;
	const gchar *data;

	if (!get(fetch, path, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	return axis_test_meta_fixture_parse_title(info->name, kind, data, length, title, error);
}

static gboolean
fixture_season(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	const gchar                 *series_id,
	gint                         number,
	AxisMetaSeason             **season,
	GError                     **error
){
	g_autofree gchar *escaped = escape(series_id);
	g_autofree gchar *path = g_strdup_printf("/title/series/%s/season/%d", escaped, number);
	g_autoptr(GBytes) body = NULL;
	gsize length = 0;
	const gchar *data;

	if (!get(fetch, path, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	return axis_test_meta_fixture_parse_season(number, data, length, season, error);
}

static gboolean
fixture_find(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *scheme,
	const gchar                 *value,
	GPtrArray                  **results,
	GError                     **error
){
	g_autofree gchar *s = escape(scheme);
	g_autofree gchar *v = escape(value);
	g_autofree gchar *path = g_strdup_printf("/find/%s?scheme=%s&value=%s", axis_meta_kind_to_string(kind), s, v);
	g_autoptr(GBytes) body = NULL;
	gsize length = 0;
	const gchar *data;

	if (!get(fetch, path, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	return axis_test_meta_fixture_parse_results(info->name, kind, data, length, results, error);
}

static gchar *
fixture_image_path(
	const AxisMetaProviderInfo *info,
	const gchar                *ref,
	AxisMetaArtSize             size
){
	static const gchar *const widths[AXIS_META_ART_N_SIZES] = { "w185", "w500", "w1280" };

	if ((guint)size >= AXIS_META_ART_N_SIZES || !axis_meta_image_ref_valid(ref))
		return NULL;
	return g_strdup_printf("/img/%s%s", widths[size], ref);
}

AxisTestMetaFixture *
axis_test_meta_fixture_new(
	const gchar *name,
	gdouble      rate,
	guint        burst
){
	AxisTestMetaFixture *self = g_new0(AxisTestMetaFixture, 1);

	self->api = axis_test_plex_new();
	self->images = axis_test_plex_new();
	g_assert_nonnull(self->api);
	g_assert_nonnull(self->images);
	self->name = g_strdup(name);
	self->label = g_strcmp0(name, "fixture") == 0 ? g_strdup("Fixture") : g_strdup_printf("Fixture %s", name);
	self->api_origin = axis_test_plex_base(self->api);
	self->image_origin = axis_test_plex_base(self->images);
	self->info.name = self->name;
	self->info.label = self->label;
	self->info.kinds = AXIS_META_KIND_BIT(AXIS_META_KIND_FILM) | AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES);
	self->info.attribution = "Test data from the fixture provider, which is not a real service.";
	self->info.api_origin = self->api_origin;
	self->info.image_origin = self->image_origin;
	self->info.default_rate = rate;
	self->info.burst = burst;
	self->info.key_header = AXIS_TEST_META_KEY_HEADER;
	self->info.key_help = "Any text: the fixture server does not check it.";
	self->info.search = fixture_search;
	self->info.title = fixture_title;
	self->info.season = fixture_season;
	self->info.find_by_external = fixture_find;
	self->info.image_path = fixture_image_path;
	return self;
}

void
axis_test_meta_fixture_free(AxisTestMetaFixture *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->api);
	axis_test_plex_free(self->images);
	g_free(self->name);
	g_free(self->label);
	g_free(self->api_origin);
	g_free(self->image_origin);
	g_free(self);
}

/* ── Images ─────────────────────────────────────────────────────── */

GBytes *
axis_test_meta_png(void)
{
	gchar *data = NULL;
	gsize length = 0;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents("data/web/icons/apple-touch-icon.png", &data, &length, &error))
		g_error("cannot read the PNG for the tests: %s", error->message);
	return g_bytes_new_take(data, length);
}

GBytes *
axis_test_meta_jpeg(
	guint width,
	guint height,
	gsize length
){
	/* SOI, APP0 (JFIF), SOF0 for a 3-component image, then filler. */
	static const guint8 head[] = {
		0xFF, 0xD8,
		0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
		0xFF, 0xC0, 0x00, 0x11, 0x08
	};
	guint8 *data;
	gsize n = MAX(length, sizeof(head) + 15 + 2);

	data = g_malloc0(n);
	memcpy(data, head, sizeof(head));
	data[sizeof(head) + 0] = (guint8)(height >> 8);
	data[sizeof(head) + 1] = (guint8)height;
	data[sizeof(head) + 2] = (guint8)(width >> 8);
	data[sizeof(head) + 3] = (guint8)width;
	data[sizeof(head) + 4] = 3;
	/* three components of three bytes */
	data[sizeof(head) + 5] = 1;
	data[sizeof(head) + 8] = 2;
	data[sizeof(head) + 11] = 3;
	data[n - 2] = 0xFF;
	data[n - 1] = 0xD9;
	return g_bytes_new_take(data, n);
}

GBytes *
axis_test_meta_webp(
	guint width,
	guint height,
	gsize length
){
	gsize n = MAX(length, (gsize)30);
	guint8 *data = g_malloc0(n);
	guint32 riff = (guint32)(n - 8);
	guint32 chunk = (guint32)(n - 20);
	guint32 bits = ((width - 1) & 0x3FFF) | (((height - 1) & 0x3FFF) << 14);

	memcpy(data, "RIFF", 4);
	data[4] = (guint8)riff;
	data[5] = (guint8)(riff >> 8);
	data[6] = (guint8)(riff >> 16);
	data[7] = (guint8)(riff >> 24);
	memcpy(data + 8, "WEBPVP8L", 8);
	data[16] = (guint8)chunk;
	data[17] = (guint8)(chunk >> 8);
	data[18] = (guint8)(chunk >> 16);
	data[19] = (guint8)(chunk >> 24);
	data[20] = 0x2F;
	data[21] = (guint8)bits;
	data[22] = (guint8)(bits >> 8);
	data[23] = (guint8)(bits >> 16);
	data[24] = (guint8)(bits >> 24);
	return g_bytes_new_take(data, n);
}

void
axis_test_meta_fixture_standard(AxisTestMetaFixture *self)
{
	static const struct
	{
		const gchar *path;
		const gchar *file;
	} routes[] = {
		{ "/search/film", "tests/fixtures/meta/search-film.json" },
		{ "/search/series", "tests/fixtures/meta/search-series.json" },
		{ "/title/film/101", "tests/fixtures/meta/title-film-101.json" },
		{ "/title/film/102", "tests/fixtures/meta/title-film-102.json" },
		{ "/title/series/201", "tests/fixtures/meta/title-series-201.json" },
		{ "/title/series/201/season/1", "tests/fixtures/meta/season-201-1.json" },
		{ "/title/series/201/season/2", "tests/fixtures/meta/season-201-2.json" },
		{ "/find/film", "tests/fixtures/meta/find-film.json" },
		{ NULL, NULL }
	};
	const gchar *const image_paths[] = { "/img/w185/p101.png", "/img/w185/p102.png", "/img/w500/p101.png",
		"/img/w1280/b101.png", "/img/w185/p201.png", NULL };
	g_autoptr(GBytes) png = axis_test_meta_png();
	guint i;

	for (i = 0; routes[i].path != NULL; i++)
		axis_test_plex_route_file(self->api, routes[i].path, -1, routes[i].file);
	for (i = 0; image_paths[i] != NULL; i++)
		axis_test_plex_route_full(self->images, image_paths[i], 200, "image/png", png, NULL);
}
