/*
 * axis-meta-tmdb.c - TMDb (The Movie Database) as a metadata provider
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-tmdb.h"

#include <string.h>

#include "meta/axis-meta-tmdb-parse.h"

#define DEFAULT_LANGUAGE "en-US"
#define DEFAULT_REGION   "US"

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo tmdb_settings[] = {
	{ AXIS_META_TMDB_SETTING_LANGUAGE, AXIS_SETTING_STRING, "Language (TMDb)",
	  "The language titles and summaries are asked for, such as en-US or de-DE. Original and alternative titles are "
	  "kept whatever it is.", 0, DEFAULT_LANGUAGE, 2, 6, 0, 0, NULL },
	{ AXIS_META_TMDB_SETTING_REGION, AXIS_SETTING_STRING, "Region (TMDb)",
	  "The country whose release dates a movie's date is taken from (the earliest cinema, digital or disc release "
	  "there), as two capital letters such as US or GB.", 0, DEFAULT_REGION, 2, 2, 0, 0, NULL },
	{ AXIS_META_TMDB_SETTING_ADULT, AXIS_SETTING_BOOLEAN, "Include adult titles in searches (TMDb)",
	  "Whether search results include titles TMDb marks as adult.", 0, "false", 0, 0, 0, 0, NULL }
};

static gboolean
key_validate(
	const gchar  *text,
	gchar       **reason
){
	return axis_meta_tmdb_key_valid(text, reason);
}

static gboolean
language_validate(
	const gchar  *text,
	gchar       **reason
){
	if (axis_meta_tmdb_language_valid(text))
		return TRUE;
	*reason = g_strdup("must be a language code such as en-US, de-DE or fr");
	return FALSE;
}

static gboolean
region_validate(
	const gchar  *text,
	gchar       **reason
){
	if (axis_meta_tmdb_region_valid(text))
		return TRUE;
	*reason = g_strdup("must be two capital letters, such as US or GB");
	return FALSE;
}

static void
prepare_settings(const AxisMetaProviderInfo *self)
{
	axis_setting_set_validator(AXIS_META_TMDB_SETTING_KEY, key_validate);
	axis_setting_set_validator(AXIS_META_TMDB_SETTING_LANGUAGE, language_validate);
	axis_setting_set_validator(AXIS_META_TMDB_SETTING_REGION, region_validate);
}

/* The language setting as it is sent, or the default when it is not set or
 * not usable. */
static gchar *
language_setting(AxisMetaFetch *fetch)
{
	gchar *value = axis_meta_fetch_get_setting(fetch, AXIS_META_TMDB_SETTING_LANGUAGE);

	if (!axis_meta_tmdb_language_valid(value)) {
		g_free(value);
		return g_strdup(DEFAULT_LANGUAGE);
	}
	return value;
}

static gchar *
region_setting(AxisMetaFetch *fetch)
{
	gchar *value = axis_meta_fetch_get_setting(fetch, AXIS_META_TMDB_SETTING_REGION);

	if (!axis_meta_tmdb_region_valid(value)) {
		g_free(value);
		return g_strdup(DEFAULT_REGION);
	}
	return value;
}

static gboolean
adult_setting(AxisMetaFetch *fetch)
{
	g_autofree gchar *value = axis_meta_fetch_get_setting(fetch, AXIS_META_TMDB_SETTING_ADULT);

	return g_strcmp0(value, "true") == 0;
}

/* ── Requests ───────────────────────────────────────────────────── */

static const gchar *
media_of(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_FILM ? "movie" : "tv";
}

/* Asks for @path and hands the answer to the caller as text. */
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
tmdb_search(
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
	GString *url = g_string_new(NULL);
	const gchar *data = NULL;
	gboolean adult = adult_setting(fetch);
	gsize length = 0;

	/*
	 * No "region": TMDb documents it on a search as a filter of the release
	 * date shown, so with it an answer's release_date is the region's date
	 * (its first release only where the region has none), and a film first
	 * released abroad came back years after its first release
	 * (fix-identify-regional-years).  Without it the answer gives the
	 * primary release date, the year the full record gives too.  The region
	 * stays where its dates are wanted: a film's full record (tmdb_title())
	 * and the lists of what is on in its cinemas (tmdb_list()).
	 */
	g_string_append_printf(url, "/3/search/%s?query=%s&include_adult=%s&language=%s&page=%u", media_of(kind), query,
		adult ? "true" : "false", language, CLAMP(page, 1, 2));
	if (kind == AXIS_META_KIND_FILM) {
		if (year != 0)
			g_string_append_printf(url, "&year=%d", year);
	} else if (year != 0) {
		g_string_append_printf(url, "&first_air_date_year=%d", year);
	}
	path = g_string_free(url, FALSE);
	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tmdb_parse_search(kind, data, length, adult, results, error);
}

static gboolean
tmdb_title(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *id,
	AxisMetaTitle              **title,
	GError                     **error
){
	g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *region = region_setting(fetch);
	g_autofree gchar *path = g_strdup_printf("/3/%s/%s?language=%s&append_to_response=%s", media_of(kind), escaped,
		language, kind == AXIS_META_KIND_FILM ? "alternative_titles,external_ids,release_dates" :
		"alternative_titles,external_ids");
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tmdb_parse_title(kind, region, data, length, title, error);
}

static gboolean
tmdb_season(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	const gchar                 *series_id,
	gint                         number,
	AxisMetaSeason             **season,
	GError                     **error
){
	g_autofree gchar *escaped = g_uri_escape_string(series_id, NULL, FALSE);
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *path = g_strdup_printf("/3/tv/%s/season/%d?language=%s", escaped, number, language);
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tmdb_parse_season(number, data, length, season, error);
}

static gboolean
tmdb_find(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	const gchar                 *scheme,
	const gchar                 *value,
	GPtrArray                  **results,
	GError                     **error
){
	g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *path = NULL;
	g_autoptr(GBytes) body = NULL;
	const gchar *source;
	const gchar *data = NULL;
	gsize length = 0;

	if (strcmp(scheme, "imdb") == 0) {
		source = "imdb_id";
	} else if (strcmp(scheme, "tvdb") == 0) {
		/* TVDB numbers its series only. */
		if (kind != AXIS_META_KIND_SERIES) {
			*results = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
			return TRUE;
		}
		source = "tvdb_id";
	} else {
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "TMDb cannot look a title up by a %s identifier",
			scheme);
		return FALSE;
	}
	path = g_strdup_printf("/3/find/%s?external_source=%s&language=%s", escaped, source, language);
	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tmdb_parse_find(kind, scheme, value, data, length, adult_setting(fetch), results, error);
}

static gboolean
tmdb_list(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	AxisMetaList                 list,
	const gchar                 *id,
	guint                        page,
	AxisMetaListPage           **out,
	GError                     **error
){
	g_autofree gchar *base = axis_meta_tmdb_list_path(kind, list, id);
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *region = region_setting(fetch);
	g_autofree gchar *path = NULL;
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	if (base == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "TMDb has no such list");
		return FALSE;
	}
	if (axis_meta_tmdb_list_regional(kind, list))
		path = g_strdup_printf("%s?language=%s&page=%u&region=%s", base, language, page, region);
	else
		path = g_strdup_printf("%s?language=%s&page=%u", base, language, page);
	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tmdb_parse_list(kind, data, length, adult_setting(fetch), out, error);
}

static gboolean
tmdb_genres(
	const AxisMetaProviderInfo  *info,
	AxisMetaFetch               *fetch,
	AxisMetaKind                 kind,
	GPtrArray                  **genres,
	GError                     **error
){
	g_autofree gchar *language = language_setting(fetch);
	g_autofree gchar *path = g_strdup_printf("/3/genre/%s/list?language=%s", media_of(kind), language);
	g_autoptr(GBytes) body = NULL;
	const gchar *data = NULL;
	gsize length = 0;

	if (!get(fetch, path, &body, &data, &length, error))
		return FALSE;
	return axis_meta_tmdb_parse_genres(data, length, genres, error);
}

static gchar *
tmdb_image_path(
	const AxisMetaProviderInfo *info,
	const gchar                *ref,
	AxisMetaArtSize             size
){
	return axis_meta_tmdb_image_path(ref, size);
}

/* ── The provider ───────────────────────────────────────────────── */

static const AxisMetaProviderInfo tmdb_info = {
	AXIS_META_TMDB_NAME,
	"TMDb",
	AXIS_META_KIND_BIT(AXIS_META_KIND_FILM) | AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES),
	AXIS_META_TMDB_ATTRIBUTION,
	AXIS_META_TMDB_API_ORIGIN,
	AXIS_META_TMDB_IMAGE_ORIGIN,
	20.0,
	10,
	"Authorization",
	"Bearer ",
	"Paste the \"API Read Access Token\" from the API page of your TMDb account settings: the long value that "
	"starts with eyJ, not the 32-character \"API Key\".",
	tmdb_search,
	tmdb_title,
	tmdb_season,
	tmdb_find,
	tmdb_image_path,
	tmdb_settings,
	G_N_ELEMENTS(tmdb_settings),
	prepare_settings,
	NULL,
	tmdb_list,
	tmdb_genres,
	AXIS_META_TMDB_FILM_LISTS,
	AXIS_META_TMDB_SERIES_LISTS,
	/* The provider of films and series: no login, no other orders. */
	FALSE,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

const AxisMetaProviderInfo *
axis_meta_tmdb_get_info(void)
{
	return &tmdb_info;
}
