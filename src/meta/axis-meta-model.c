/*
 * axis-meta-model.c - what a metadata provider's answer becomes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-model.h"

#include <string.h>

#include "library/axis-library-name.h"
#include "meta/axis-meta-provider.h"

G_DEFINE_QUARK(axis-meta-error-quark, axis_meta_error)

const gchar *
axis_meta_art_size_to_string(AxisMetaArtSize size)
{
	switch (size) {
	case AXIS_META_ART_POSTER_SMALL:
		return "poster-small";
	case AXIS_META_ART_POSTER:
		return "poster";
	case AXIS_META_ART_BACKDROP:
		return "backdrop";
	default:
		return NULL;
	}
}

/* ── Kinds and states ───────────────────────────────────────────── */

const gchar *
axis_meta_kind_to_string(AxisMetaKind kind)
{
	switch (kind) {
	case AXIS_META_KIND_FILM:
		return "film";
	case AXIS_META_KIND_SERIES:
		return "series";
	default:
		return NULL;
	}
}

gboolean
axis_meta_kind_from_string(
	const gchar  *text,
	AxisMetaKind *kind
){
	if (g_strcmp0(text, "film") == 0) {
		*kind = AXIS_META_KIND_FILM;
		return TRUE;
	}
	if (g_strcmp0(text, "series") == 0) {
		*kind = AXIS_META_KIND_SERIES;
		return TRUE;
	}
	return FALSE;
}

static const gchar *const status_names[] = { NULL, "released", "continuing", "ended", "upcoming" };

const gchar *
axis_meta_status_to_string(AxisMetaStatus status)
{
	if ((guint)status >= G_N_ELEMENTS(status_names))
		return NULL;
	return status_names[status];
}

AxisMetaStatus
axis_meta_status_from_string(const gchar *text)
{
	guint i;

	for (i = 1; text != NULL && i < G_N_ELEMENTS(status_names); i++) {
		if (strcmp(text, status_names[i]) == 0)
			return (AxisMetaStatus)i;
	}
	return AXIS_META_STATUS_UNKNOWN;
}

/* ── Records ────────────────────────────────────────────────────── */

void
axis_meta_external_id_free(AxisMetaExternalId *id)
{
	if (id == NULL)
		return;
	g_free(id->scheme);
	g_free(id->value);
	g_free(id);
}

void
axis_meta_alt_title_free(AxisMetaAltTitle *alt)
{
	if (alt == NULL)
		return;
	g_free(alt->title);
	g_free(alt->country);
	g_free(alt->language);
	g_free(alt);
}

AxisMetaEpisode *
axis_meta_episode_new(void)
{
	return g_new0(AxisMetaEpisode, 1);
}

void
axis_meta_episode_free(AxisMetaEpisode *episode)
{
	if (episode == NULL)
		return;
	g_free(episode->name);
	g_free(episode->air_date);
	g_free(episode->overview);
	g_free(episode->provider_id);
	g_free(episode);
}

gboolean
axis_meta_episode_id_valid(const gchar *id)
{
	gsize n = 0;
	const gchar *p;

	if (id == NULL)
		return FALSE;
	for (p = id; *p != '\0'; p++, n++) {
		if (!g_ascii_isalnum(*p) || n >= 24)
			return FALSE;
	}
	return n > 0;
}

AxisMetaSeason *
axis_meta_season_new(gint number)
{
	AxisMetaSeason *season = g_new0(AxisMetaSeason, 1);

	season->number = number;
	season->episode_count = -1;
	return season;
}

void
axis_meta_season_free(AxisMetaSeason *season)
{
	if (season == NULL)
		return;
	g_free(season->name);
	g_free(season->air_date);
	g_free(season->overview);
	if (season->episodes != NULL)
		g_ptr_array_unref(season->episodes);
	g_free(season);
}

AxisMetaTitle *
axis_meta_title_new(AxisMetaKind kind)
{
	AxisMetaTitle *title = g_new0(AxisMetaTitle, 1);

	title->kind = kind;
	title->ids = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_external_id_free);
	return title;
}

void
axis_meta_title_free(AxisMetaTitle *title)
{
	if (title == NULL)
		return;
	g_free(title->provider);
	g_free(title->provider_id);
	g_free(title->title);
	g_free(title->original_title);
	g_free(title->overview);
	g_free(title->original_language);
	g_free(title->release_date);
	g_free(title->first_air_date);
	g_free(title->last_air_date);
	g_free(title->poster_ref);
	g_free(title->backdrop_ref);
	if (title->genres != NULL)
		g_ptr_array_unref(title->genres);
	if (title->ids != NULL)
		g_ptr_array_unref(title->ids);
	if (title->alt_titles != NULL)
		g_ptr_array_unref(title->alt_titles);
	if (title->seasons != NULL)
		g_ptr_array_unref(title->seasons);
	if (title->sources != NULL)
		g_ptr_array_unref(title->sources);
	g_free(title);
}

const gchar *
axis_meta_title_get_id(
	const AxisMetaTitle *title,
	const gchar         *scheme
){
	guint i;

	for (i = 0; title->ids != NULL && scheme != NULL && i < title->ids->len; i++) {
		const AxisMetaExternalId *id = g_ptr_array_index(title->ids, i);

		if (strcmp(id->scheme, scheme) == 0)
			return id->value;
	}
	return NULL;
}

gboolean
axis_meta_title_add_id(
	AxisMetaTitle *title,
	const gchar   *scheme,
	const gchar   *value
){
	AxisMetaExternalId *id;

	if (!axis_meta_id_valid(scheme, value) || title->ids->len >= AXIS_META_MAX_IDS ||
	    axis_meta_title_get_id(title, scheme) != NULL)
		return FALSE;
	id = g_new0(AxisMetaExternalId, 1);
	id->scheme = g_strdup(scheme);
	id->value = g_strdup(value);
	g_ptr_array_add(title->ids, id);
	return TRUE;
}

/* ── Cleaning ───────────────────────────────────────────────────── */

gchar *
axis_meta_clean_text(
	const gchar *text,
	guint        max_chars
){
	g_autofree gchar *clean = NULL;
	GString *out;
	const gchar *p;
	gboolean space = FALSE;
	guint chars = 0;

	if (text == NULL || max_chars == 0)
		return NULL;
	/* Bounded first: a run of spaces that would be collapsed may be cut
	 * short, never anything longer than this kept. */
	clean = axis_library_text_clean(text, max_chars * 4 + 16);
	out = g_string_new(NULL);
	for (p = clean; *p != '\0' && chars < max_chars; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isspace(c)) {
			space = out->len > 0;
			continue;
		}
		if (space) {
			/* A space is kept only with something after it. */
			if (chars + 2 > max_chars)
				break;
			g_string_append_c(out, ' ');
			chars++;
			space = FALSE;
		}
		g_string_append_unichar(out, c);
		chars++;
	}
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

/* Whether @text is what cleaning it would give. */
static gboolean
text_is_clean(
	const gchar *text,
	guint        max_chars
){
	g_autofree gchar *again = NULL;

	if (text == NULL || !g_utf8_validate(text, -1, NULL))
		return FALSE;
	again = axis_meta_clean_text(text, max_chars);
	return again != NULL && strcmp(again, text) == 0;
}

static gboolean
optional_clean(
	const gchar *text,
	guint        max_chars
){
	return text == NULL || text_is_clean(text, max_chars);
}

gboolean
axis_meta_year_valid(gint64 year)
{
	return year >= AXIS_META_YEAR_MIN && year <= AXIS_META_YEAR_MAX;
}

gboolean
axis_meta_date_valid(const gchar *text)
{
	gint year;
	gint month;
	gint day;
	guint i;

	if (text == NULL || strlen(text) != 10 || text[4] != '-' || text[7] != '-')
		return FALSE;
	for (i = 0; i < 10; i++) {
		if (i != 4 && i != 7 && !g_ascii_isdigit(text[i]))
			return FALSE;
	}
	year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
	month = (text[5] - '0') * 10 + (text[6] - '0');
	day = (text[8] - '0') * 10 + (text[9] - '0');
	if (!axis_meta_year_valid(year) || month < 1 || month > 12 || day < 1 || day > 31)
		return FALSE;
	return g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year);
}

gboolean
axis_meta_provider_name_valid(const gchar *name)
{
	const gchar *p;

	if (name == NULL || name[0] < 'a' || name[0] > 'z' || strlen(name) > 16)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_meta_scheme_valid(const gchar *scheme)
{
	return axis_meta_provider_name_valid(scheme) && strlen(scheme) >= 2;
}

gboolean
axis_meta_source_valid(const gchar *source)
{
	const gchar *dash;
	const gchar *p;

	if (source == NULL)
		return FALSE;
	dash = strchr(source, '-');
	if (dash == NULL)
		return axis_meta_provider_name_valid(source);
	{
		g_autofree gchar *provider = g_strndup(source, (gsize)(dash - source));

		if (!axis_meta_provider_name_valid(provider))
			return FALSE;
	}
	if (dash[1] == '\0' || strlen(dash + 1) > 16)
		return FALSE;
	for (p = dash + 1; *p != '\0'; p++) {
		if (*p < 'a' || *p > 'z')
			return FALSE;
	}
	return TRUE;
}

gchar *
axis_meta_source_provider(const gchar *source)
{
	const gchar *dash;

	if (!axis_meta_source_valid(source))
		return NULL;
	dash = strchr(source, '-');
	return dash != NULL ? g_strndup(source, (gsize)(dash - source)) : g_strdup(source);
}

static gboolean
all_digits(
	const gchar *text,
	gsize        min,
	gsize        max
){
	gsize n = strlen(text);
	gsize i;

	if (n < min || n > max)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_meta_id_valid(
	const gchar *scheme,
	const gchar *value
){
	gsize n;
	gsize i;

	if (!axis_meta_scheme_valid(scheme) || value == NULL)
		return FALSE;
	if (strcmp(scheme, "imdb") == 0)
		return value[0] == 't' && value[1] == 't' && all_digits(value + 2, 7, 10);
	if (strcmp(scheme, "tmdb") == 0 || strcmp(scheme, "tvdb") == 0)
		return value[0] != '0' && all_digits(value, 1, 12);
	n = strlen(value);
	if (n < 1 || n > 24)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_isalnum(value[i]))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_meta_image_ref_valid(const gchar *ref)
{
	gsize n;
	gsize i;
	gsize segment = 1;

	if (ref == NULL || ref[0] != '/')
		return FALSE;
	n = strlen(ref);
	if (n < 2 || n > 200 || ref[n - 1] == '/')
		return FALSE;
	for (i = 1; i <= n; i++) {
		if (i == n || ref[i] == '/') {
			gsize len = i - segment;

			if (len == 0 || (len == 1 && ref[segment] == '.') ||
			    (len == 2 && ref[segment] == '.' && ref[segment + 1] == '.'))
				return FALSE;
			segment = i + 1;
			continue;
		}
		if (!g_ascii_isalnum(ref[i]) && ref[i] != '.' && ref[i] != '_' && ref[i] != '-')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_meta_country_valid(const gchar *country)
{
	return country != NULL && strlen(country) == 2 && g_ascii_isupper(country[0]) && g_ascii_isupper(country[1]);
}

gboolean
axis_meta_language_valid(const gchar *language)
{
	gsize n;
	gsize i;

	if (language == NULL)
		return FALSE;
	n = strlen(language);
	if (n < 2 || n > 3)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_islower(language[i]))
			return FALSE;
	}
	return TRUE;
}

/* ── Checking a whole answer ────────────────────────────────────── */

static gboolean
refuse(
	gchar      **problem,
	const gchar *text
){
	if (problem != NULL)
		*problem = g_strdup(text);
	return FALSE;
}

static gboolean
optional_date(const gchar *text)
{
	return text == NULL || axis_meta_date_valid(text);
}

static gboolean
season_summary_check(
	const AxisMetaSeason *season,
	gchar               **problem
){
	if (season->number < 0 || season->number > 10000)
		return refuse(problem, "a season number is out of range");
	if (!optional_clean(season->name, AXIS_META_NAME_MAX_CHARS) ||
	    !optional_clean(season->overview, AXIS_META_OVERVIEW_MAX_CHARS))
		return refuse(problem, "a season's text is not clean");
	if (!optional_date(season->air_date))
		return refuse(problem, "a season's date is not a date");
	if (season->episode_count < -1 || season->episode_count > 100000)
		return refuse(problem, "a season's episode count is out of range");
	return TRUE;
}

gboolean
axis_meta_title_check(
	const AxisMetaTitle  *title,
	const gchar          *provider,
	gchar               **problem
){
	const gchar *own;
	guint i;
	guint j;

	if (title == NULL || axis_meta_kind_to_string(title->kind) == NULL)
		return refuse(problem, "it is neither a movie nor a series");
	if (!text_is_clean(title->title, AXIS_META_TITLE_MAX_CHARS))
		return refuse(problem, "it has no title");
	if (!axis_meta_provider_name_valid(provider) || g_strcmp0(title->provider, provider) != 0)
		return refuse(problem, "it does not name the provider it came from");
	own = axis_meta_title_get_id(title, provider);
	if (title->provider_id == NULL || own == NULL || strcmp(own, title->provider_id) != 0 ||
	    !axis_meta_id_valid(provider, title->provider_id))
		return refuse(problem, "it has no identifier of the provider's own");
	if (title->ids->len > AXIS_META_MAX_IDS)
		return refuse(problem, "it has too many identifiers");
	for (i = 0; i < title->ids->len; i++) {
		const AxisMetaExternalId *id = g_ptr_array_index(title->ids, i);

		if (!axis_meta_id_valid(id->scheme, id->value))
			return refuse(problem, "an identifier is not valid");
		for (j = 0; j < i; j++) {
			const AxisMetaExternalId *other = g_ptr_array_index(title->ids, j);

			if (strcmp(other->scheme, id->scheme) == 0)
				return refuse(problem, "it has two identifiers in one scheme");
		}
	}
	if (title->year != 0 && !axis_meta_year_valid(title->year))
		return refuse(problem, "its year is out of range");
	if (title->regional_year != 0 && (!axis_meta_year_valid(title->regional_year) || title->year == 0 ||
	    title->regional_year == title->year || ABS(title->regional_year - title->year) > 2))
		return refuse(problem, "its year in the region is not one within two years of its first release");
	if (title->runtime < 0 || title->runtime > 10000)
		return refuse(problem, "its running time is out of range");
	if ((guint)title->status > AXIS_META_STATUS_UPCOMING)
		return refuse(problem, "its state is not one Axis knows");
	if (!optional_clean(title->original_title, AXIS_META_TITLE_MAX_CHARS) ||
	    !optional_clean(title->overview, AXIS_META_OVERVIEW_MAX_CHARS))
		return refuse(problem, "a text is not clean");
	if (title->original_language != NULL && !axis_meta_language_valid(title->original_language))
		return refuse(problem, "its language is not a language code");
	if (!optional_date(title->release_date) || !optional_date(title->first_air_date) ||
	    !optional_date(title->last_air_date))
		return refuse(problem, "a date is not a date");
	if ((title->poster_ref != NULL && !axis_meta_image_ref_valid(title->poster_ref)) ||
	    (title->backdrop_ref != NULL && !axis_meta_image_ref_valid(title->backdrop_ref)))
		return refuse(problem, "an image reference is not valid");
	if (title->genres != NULL) {
		if (title->genres->len > AXIS_META_MAX_GENRES)
			return refuse(problem, "it has too many genres");
		for (i = 0; i < title->genres->len; i++) {
			if (!text_is_clean(g_ptr_array_index(title->genres, i), AXIS_META_GENRE_MAX_CHARS))
				return refuse(problem, "a genre is not clean");
		}
	}
	if (title->alt_titles != NULL) {
		if (title->alt_titles->len > AXIS_META_MAX_ALT_TITLES)
			return refuse(problem, "it has too many other names");
		for (i = 0; i < title->alt_titles->len; i++) {
			const AxisMetaAltTitle *alt = g_ptr_array_index(title->alt_titles, i);

			if (!text_is_clean(alt->title, AXIS_META_TITLE_MAX_CHARS) ||
			    (alt->country != NULL && !axis_meta_country_valid(alt->country)) ||
			    (alt->language != NULL && !axis_meta_language_valid(alt->language)))
				return refuse(problem, "another name is not valid");
		}
	}
	if (title->seasons != NULL) {
		if (title->kind != AXIS_META_KIND_SERIES)
			return refuse(problem, "a movie has seasons");
		if (title->seasons->len > AXIS_META_MAX_SEASONS)
			return refuse(problem, "it has too many seasons");
		for (i = 0; i < title->seasons->len; i++) {
			const AxisMetaSeason *season = g_ptr_array_index(title->seasons, i);

			if (!season_summary_check(season, problem))
				return FALSE;
			if (season->episodes != NULL)
				return refuse(problem, "a season in a title carries its episodes");
			for (j = 0; j < i; j++) {
				const AxisMetaSeason *other = g_ptr_array_index(title->seasons, j);

				if (other->number == season->number)
					return refuse(problem, "a season is listed twice");
			}
		}
	}
	if (title->sources != NULL) {
		if (title->kind != AXIS_META_KIND_SERIES)
			return refuse(problem, "a movie has episode orders");
		if (title->sources->len > AXIS_META_MAX_SOURCES)
			return refuse(problem, "it has too many episode orders");
		for (i = 0; i < title->sources->len; i++) {
			const gchar *source = g_ptr_array_index(title->sources, i);
			g_autofree gchar *owner = axis_meta_source_provider(source);

			if (owner == NULL || g_strcmp0(owner, title->provider) != 0)
				return refuse(problem, "an episode order is not the provider's own");
			for (j = 0; j < i; j++) {
				if (strcmp(g_ptr_array_index(title->sources, j), source) == 0)
					return refuse(problem, "an episode order is listed twice");
			}
		}
	}
	return TRUE;
}

gboolean
axis_meta_season_check(
	const AxisMetaSeason  *season,
	gchar                **problem
){
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GHashTable) ids = NULL;
	guint i;

	if (season == NULL)
		return refuse(problem, "there is no season");
	if (!season_summary_check(season, problem))
		return FALSE;
	if (season->episodes == NULL)
		return refuse(problem, "it has no list of episodes");
	if (season->episodes->len > AXIS_META_MAX_EPISODES)
		return refuse(problem, "it has too many episodes");
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	ids = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < season->episodes->len; i++) {
		const AxisMetaEpisode *episode = g_ptr_array_index(season->episodes, i);

		if (episode->season_number != season->number)
			return refuse(problem, "an episode is of another season");
		if (episode->number < 0 || episode->number > 100000)
			return refuse(problem, "an episode number is out of range");
		if (!g_hash_table_add(seen, GINT_TO_POINTER(episode->number + 1)))
			return refuse(problem, "an episode is listed twice");
		if (episode->absolute_number < 0 || episode->absolute_number > 1000000)
			return refuse(problem, "an absolute number is out of range");
		if (episode->runtime < 0 || episode->runtime > 10000)
			return refuse(problem, "an episode's running time is out of range");
		if (!optional_clean(episode->name, AXIS_META_NAME_MAX_CHARS) ||
		    !optional_clean(episode->overview, AXIS_META_OVERVIEW_MAX_CHARS))
			return refuse(problem, "an episode's text is not clean");
		if (!optional_date(episode->air_date))
			return refuse(problem, "an episode's date is not a date");
		if (episode->provider_id != NULL && (!axis_meta_episode_id_valid(episode->provider_id) ||
		    !g_hash_table_add(ids, episode->provider_id)))
			return refuse(problem, "an episode's identifier is not valid or is listed twice");
	}
	return TRUE;
}

/* ── Lists ──────────────────────────────────────────────────────── */

static const gchar *const list_names[AXIS_META_N_LISTS] = {
	"popular", "trending", "now", "upcoming", "top_rated", "recommended"
};

const gchar *
axis_meta_list_to_string(AxisMetaList list)
{
	if ((guint)list >= AXIS_META_N_LISTS)
		return NULL;
	return list_names[list];
}

gboolean
axis_meta_list_from_string(
	const gchar  *text,
	AxisMetaList *list
){
	guint i;

	for (i = 0; text != NULL && i < AXIS_META_N_LISTS; i++) {
		if (strcmp(text, list_names[i]) == 0) {
			*list = (AxisMetaList)i;
			return TRUE;
		}
	}
	return FALSE;
}

AxisMetaListEntry *
axis_meta_list_entry_new(AxisMetaTitle *title)
{
	AxisMetaListEntry *entry = g_new0(AxisMetaListEntry, 1);

	entry->title = title;
	entry->rating = -1;
	entry->genre_ids = g_array_new(FALSE, FALSE, sizeof(gint));
	return entry;
}

void
axis_meta_list_entry_free(AxisMetaListEntry *entry)
{
	if (entry == NULL)
		return;
	axis_meta_title_free(entry->title);
	if (entry->genre_ids != NULL)
		g_array_unref(entry->genre_ids);
	g_free(entry);
}

AxisMetaListPage *
axis_meta_list_page_new(void)
{
	AxisMetaListPage *page = g_new0(AxisMetaListPage, 1);

	page->entries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_list_entry_free);
	return page;
}

void
axis_meta_list_page_free(AxisMetaListPage *page)
{
	if (page == NULL)
		return;
	if (page->entries != NULL)
		g_ptr_array_unref(page->entries);
	g_free(page);
}

void
axis_meta_genre_free(AxisMetaGenre *genre)
{
	if (genre == NULL)
		return;
	g_free(genre->name);
	g_free(genre);
}

gboolean
axis_meta_list_page_check(
	const AxisMetaListPage  *page,
	AxisMetaKind             kind,
	const gchar             *provider,
	gchar                  **problem
){
	g_autoptr(GHashTable) seen = NULL;
	guint i;
	guint j;

	if (page == NULL || page->entries == NULL)
		return refuse(problem, "there is no list");
	if (page->entries->len > AXIS_META_MAX_RESULTS)
		return refuse(problem, "the list has too many entries");
	if (page->total_pages > AXIS_META_LIST_MAX_PAGE)
		return refuse(problem, "the list says it has too many pages");
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < page->entries->len; i++) {
		const AxisMetaListEntry *entry = g_ptr_array_index(page->entries, i);

		if (entry == NULL || entry->title == NULL || entry->title->kind != kind || entry->title->detail)
			return refuse(problem, "an entry is not a brief record of the kind asked for");
		if (!axis_meta_title_check(entry->title, provider, problem))
			return FALSE;
		if (!g_hash_table_add(seen, entry->title->provider_id))
			return refuse(problem, "a title is listed twice");
		if (entry->rating < -1 || entry->rating > 100)
			return refuse(problem, "a rating is out of range");
		if (entry->votes < 0)
			return refuse(problem, "a vote count is out of range");
		if (entry->genre_ids == NULL || entry->genre_ids->len > AXIS_META_MAX_GENRES)
			return refuse(problem, "an entry has too many genres");
		for (j = 0; j < entry->genre_ids->len; j++) {
			gint id = g_array_index(entry->genre_ids, gint, j);
			guint k;

			if (id < 1)
				return refuse(problem, "a genre number is out of range");
			for (k = 0; k < j; k++) {
				if (g_array_index(entry->genre_ids, gint, k) == id)
					return refuse(problem, "a genre is listed twice");
			}
		}
	}
	return TRUE;
}

gboolean
axis_meta_genres_check(
	const GPtrArray  *genres,
	gchar           **problem
){
	g_autoptr(GHashTable) seen = NULL;
	guint i;

	if (genres == NULL)
		return refuse(problem, "there is no list of genres");
	if (genres->len > AXIS_META_MAX_GENRE_LIST)
		return refuse(problem, "there are too many genres");
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (i = 0; i < genres->len; i++) {
		const AxisMetaGenre *genre = g_ptr_array_index(genres, i);

		if (genre == NULL || genre->id < 1 || !g_hash_table_add(seen, GINT_TO_POINTER(genre->id)))
			return refuse(problem, "a genre number is not valid or is listed twice");
		if (!text_is_clean(genre->name, AXIS_META_GENRE_MAX_CHARS))
			return refuse(problem, "a genre's name is not clean");
	}
	return TRUE;
}

/* ── Search text ────────────────────────────────────────────────── */

gchar *
axis_meta_query_clean(const gchar *text)
{
	return axis_meta_clean_text(text, AXIS_META_QUERY_MAX_CHARS);
}

#define QUERY_KEY_MAX_BYTES 300

gchar *
axis_meta_query_key(const gchar *text)
{
	g_autofree gchar *clean = axis_meta_query_clean(text);
	gchar *key;
	guint round;

	if (clean == NULL)
		return NULL;
	key = g_strdup(clean);
	/* Folding and normalising each feed the other; a few rounds settle
	 * every ordinary text, and the result is the same for the same input
	 * whatever happens. */
	for (round = 0; round < 3; round++) {
		g_autofree gchar *folded = g_utf8_casefold(key, -1);
		gchar *normal = g_utf8_normalize(folded, -1, G_NORMALIZE_ALL);
		gboolean same;

		if (normal == NULL)
			break;
		same = strcmp(normal, key) == 0;
		g_free(key);
		key = normal;
		if (same)
			break;
	}
	{
		gchar *again = axis_meta_clean_text(key, QUERY_KEY_MAX_BYTES);

		g_free(key);
		key = again;
	}
	if (key == NULL)
		return NULL;
	if (strlen(key) > QUERY_KEY_MAX_BYTES) {
		const gchar *end = key + QUERY_KEY_MAX_BYTES;

		while (end > key && (((guchar)*end) & 0xC0) == 0x80)
			end--;
		key[end - key] = '\0';
		g_strchomp(key);
	}
	if (key[0] == '\0') {
		g_free(key);
		return NULL;
	}
	return key;
}
