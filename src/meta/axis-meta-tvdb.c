/*
 * axis-meta-tvdb.c - TheTVDB (API version 4) as a metadata provider
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-tvdb.h"

#include <string.h>

#include "meta/axis-meta-tvdb-assumed.h"
#include "meta/axis-meta-tvdb-parse.h"

#define DEFAULT_LANGUAGE "eng"

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo tvdb_settings[] = {
	{ AXIS_META_TVDB_SETTING_PIN, AXIS_SETTING_STRING, "TheTVDB subscriber PIN",
	  "Only for a user-supported key, which asks each user for their TheTVDB subscription's PIN; a key you "
	  "registered for your own installation needs none, so leave this empty. Sent only with the key, in the body of "
	  "TheTVDB's login request, and never shown again.", AXIS_SETTING_SECRET, NULL, 0, 255, 0, 0, NULL },
	{ AXIS_META_TVDB_SETTING_LANGUAGE, AXIS_SETTING_STRING, "Language (TheTVDB)",
	  "The language series' names, summaries and episode titles are asked for, as TheTVDB's three-letter code "
	  "such as eng, deu or fra. Other names are kept whatever it is.", 0, DEFAULT_LANGUAGE, 3, 3, 0, 0, NULL }
};

static gboolean
secret_validate(
	const gchar  *text,
	gchar       **reason
){
	return axis_meta_tvdb_secret_valid(text, reason);
}

static gboolean
language_validate(
	const gchar  *text,
	gchar       **reason
){
	if (axis_meta_tvdb_language_valid(text))
		return TRUE;
	*reason = g_strdup("must be three lower-case letters, such as eng or deu");
	return FALSE;
}

static void
prepare_settings(const AxisMetaProviderInfo *self)
{
	axis_setting_set_validator(AXIS_META_TVDB_SETTING_KEY, secret_validate);
	axis_setting_set_validator(AXIS_META_TVDB_SETTING_PIN, secret_validate);
	axis_setting_set_validator(AXIS_META_TVDB_SETTING_LANGUAGE, language_validate);
}

static gchar *
language_setting(AxisMetaFetch *fetch)
{
	gchar *value = axis_meta_fetch_get_setting(fetch, AXIS_META_TVDB_SETTING_LANGUAGE);

	if (!axis_meta_tvdb_language_valid(value)) {
		g_free(value);
		return g_strdup(DEFAULT_LANGUAGE);
	}
	return value;
}

/* ── Requests ───────────────────────────────────────────────────── */

static gboolean
get(
	AxisMetaFetch  *fetch,
	const gchar    *path,
	GBytes        **body,
	const gchar   **data,
	gsize          *length,
	GError        **error
){
	if (!axis_meta_fetch_get(fetch, path, body, error))
		return FALSE;
	*data = g_bytes_get_data(*body, length);
	return TRUE;
}

static gboolean
series_only(
	AxisMetaKind   kind,
	GError       **error
){
	if (kind == AXIS_META_KIND_SERIES)
		return TRUE;
	g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "TheTVDB is asked about series only");
	return FALSE;
}

static gboolean
tvdb_login(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	gchar                      **token,
	GError                     **error
){
	g_autoptr(GBytes) body = NULL;
	const gchar *data;
	gsize length = 0;

	*token = NULL;
	if (!axis_meta_fetch_login(fetch, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	return axis_meta_tvdb_parse_login(data, length, token, error);
}

static gboolean
tvdb_search(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *text,
	gint                         year,
	guint                        page,
	GPtrArray                  **results,
	GError                     **error
){
	g_autofree gchar *query = g_uri_escape_string(text, NULL, FALSE);
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *path = NULL;
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	if (!series_only(kind, error))
		return FALSE;
	path = year != 0 ?
		g_strdup_printf("/v4/search?query=%s&type=series&limit=%u&offset=%u&year=%d", query, AXIS_META_MAX_RESULTS,
			(CLAMP(page, 1, 2) - 1) * AXIS_META_MAX_RESULTS, year) :
		g_strdup_printf("/v4/search?query=%s&type=series&limit=%u&offset=%u", query, AXIS_META_MAX_RESULTS,
			(CLAMP(page, 1, 2) - 1) * AXIS_META_MAX_RESULTS);
	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tvdb_parse_search(data, length, language, results, error);
}

/* The extended record of the series @id. */
static gboolean
extended(
	AxisMetaFetch   *fetch,
	const gchar     *id,
	AxisMetaTitle  **title,
	GError         **error
){
	g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *path = g_strdup_printf("/v4/series/%s/extended?meta=translations&short=true", escaped);
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tvdb_parse_series(data, length, language, title, error);
}

static gboolean
tvdb_title(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *id,
	AxisMetaTitle              **title,
	GError                     **error
){
	*title = NULL;
	if (!series_only(kind, error))
		return FALSE;
	return extended(fetch, id, title, error);
}

static gboolean
tvdb_order(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	const gchar                 *series_id,
	const gchar                 *source,
	GPtrArray                  **seasons,
	GError                     **error
){
	const gchar *order = axis_meta_tvdb_order_of_source(source);
	g_autofree gchar *escaped = g_uri_escape_string(series_id, NULL, FALSE);
	g_autofree gchar *language = language_setting(fetch);
	g_autoptr(GPtrArray) read = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	gboolean more = TRUE;
	guint page;

	*seasons = NULL;
	if (order == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "TheTVDB has no such episode order");
		return FALSE;
	}
	/* Every page, each asked for by its number: an address in an answer is
	 * never followed (assumed 5). */
	for (page = 0; more && page < AXIS_META_TVDB_EPISODE_PAGES; page++) {
		g_autofree gchar *path = g_strdup_printf(AXIS_META_TVDB_EPISODES_PATH, escaped, order, language, page);
		g_autoptr(GBytes) body = NULL;
		const gchar *data = NULL;
		gsize length = 0;

		if (!get(fetch, path, &body, &data, &length, error) ||
		    !axis_meta_tvdb_parse_episodes(data, length, read, &more, error))
			return FALSE;
	}
	if (more) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE,
			"TheTVDB lists the episodes of this order on more than %d pages; Axis reads that many at most",
			AXIS_META_TVDB_EPISODE_PAGES);
		return FALSE;
	}
	*seasons = g_steal_pointer(&read);
	return TRUE;
}

/* A season of the aired order, for a series whose record is TheTVDB's: the
 * whole order is read, and the season asked for kept. */
static gboolean
tvdb_season(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	const gchar                 *series_id,
	gint                         number,
	AxisMetaSeason             **season,
	GError                     **error
){
	g_autoptr(GPtrArray) seasons = NULL;
	guint i;

	*season = NULL;
	if (!tvdb_order(info, fetch, series_id, AXIS_META_TVDB_NAME, &seasons, error))
		return FALSE;
	for (i = 0; i < seasons->len; i++) {
		AxisMetaSeason *one = g_ptr_array_index(seasons, i);

		if (one->number == number) {
			*season = g_ptr_array_steal_index(seasons, i);
			return TRUE;
		}
	}
	g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "TheTVDB has no season %d of this series", number);
	return FALSE;
}

static gboolean
tvdb_find(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *scheme,
	const gchar                 *value,
	GPtrArray                  **results,
	GError                     **error
){
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *language = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	*results = NULL;
	if (!series_only(kind, error))
		return FALSE;
	if (strcmp(scheme, AXIS_META_TVDB_NAME) == 0) {
		/* Its own identifier: the series itself, or no such series. */
		g_autoptr(AxisMetaTitle) title = NULL;
		g_autoptr(GError) local = NULL;

		*results = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
		if (!extended(fetch, value, &title, &local)) {
			if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND))
				return TRUE;
			g_clear_pointer(results, g_ptr_array_unref);
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_ptr_array_add(*results, g_steal_pointer(&title));
		return TRUE;
	}
	if (strcmp(scheme, "imdb") != 0 && !(AXIS_META_TVDB_REMOTEID_TAKES_TMDB && strcmp(scheme, "tmdb") == 0)) {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "TheTVDB cannot look a series up by a %s identifier",
			scheme);
		return FALSE;
	}
	escaped = g_uri_escape_string(value, NULL, FALSE);
	language = language_setting(fetch);
	path = g_strdup_printf("/v4/search/remoteid/%s", escaped);
	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tvdb_parse_remote(data, length, scheme, value, language, results, error);
}

static gboolean
tvdb_test(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	gchar                      **summary,
	GError                     **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GPtrArray) names = NULL;
	const gchar *data = NULL;
	gsize length = 0;
	GString *text;
	guint i;

	*summary = NULL;
	if (!get(fetch, "/v4/seasons/types", &body, &data, &length, error) ||
	    !axis_meta_tvdb_parse_orders(data, length, &names, error))
		return FALSE;
	text = g_string_new("TheTVDB accepted the key");
	if (names->len == 0) {
		g_string_append(text, " and lists no episode orders.");
	} else {
		g_string_append_printf(text, " and lists %u episode %s: ", names->len, names->len == 1 ? "order" : "orders");
		for (i = 0; i < names->len; i++)
			g_string_append_printf(text, "%s%s", i > 0 ? ", " : "", (const gchar *)g_ptr_array_index(names, i));
		g_string_append_c(text, '.');
	}
	*summary = g_string_free(text, FALSE);
	return TRUE;
}

static gchar *
tvdb_image_path(
	const AxisMetaProviderInfo *info,
	const gchar                *ref,
	AxisMetaArtSize             size
){
	return axis_meta_tvdb_image_path(ref, size);
}

/* ── The provider ───────────────────────────────────────────────── */

static const AxisMetaProviderInfo tvdb_info = {
	AXIS_META_TVDB_NAME,
	"TheTVDB",
	AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES),
	AXIS_META_TVDB_ATTRIBUTION,
	AXIS_META_TVDB_API_ORIGIN,
	AXIS_META_TVDB_IMAGE_ORIGIN,
	AXIS_META_TVDB_RATE,
	AXIS_META_TVDB_BURST,
	"Authorization",
	"Bearer ",
	"The API key of your own TheTVDB account (see thetvdb.com/api-information): a free key for a project such as "
	"your own installation asks that TheTVDB's notice is shown, as Axis does. Each administrator registers their "
	"own; Axis ships none.",
	tvdb_search,
	tvdb_title,
	tvdb_season,
	tvdb_find,
	tvdb_image_path,
	tvdb_settings,
	G_N_ELEMENTS(tvdb_settings),
	prepare_settings,
	NULL,
	NULL,
	NULL,
	0,
	0,
	TRUE,
	AXIS_META_TVDB_ATTRIBUTION_URL,
	AXIS_META_TVDB_SETTING_PIN,
	tvdb_login,
	tvdb_order,
	NULL,
	tvdb_test
};

const AxisMetaProviderInfo *
axis_meta_tvdb_get_info(void)
{
	static AxisMetaProviderInfo info;
	static gsize once = 0;

	/* The sources are the parser's table, which a static initialiser cannot
	 * name. */
	if (g_once_init_enter(&once)) {
		info = tvdb_info;
		info.sources = axis_meta_tvdb_sources();
		g_once_init_leave(&once, 1);
	}
	return &info;
}
