/*
 * axis-meta-model.h - what a metadata provider's answer becomes: titles,
 * their identifiers and other names, seasons and episodes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Providers keep their own formats in their own files and hand back these
 * structures, which the store keeps (meta_* tables, 0018_metadata.sql).
 * Provider data is untrusted input: a provider builds its answer with the
 * cleaning helpers below (every field optional, anything unreadable left
 * out), and axis_meta_title_check() and axis_meta_season_check() look at
 * the whole answer once more before anything is stored.  An answer that
 * fails is a failure of that request; nothing of it is kept.
 *
 * GLib only, no I/O: read by the fuzz targets through the fixture
 * provider's parser (tests/fuzz/fuzz-meta-fixture.c).
 */

#ifndef AXIS_META_MODEL_H
#define AXIS_META_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/* Limits on an answer (beyond these a list is cut, a text shortened). */
#define AXIS_META_MAX_RESULTS        50
#define AXIS_META_MAX_IDS            8
#define AXIS_META_MAX_GENRES         20
#define AXIS_META_MAX_ALT_TITLES     100
#define AXIS_META_MAX_SEASONS        200
#define AXIS_META_MAX_EPISODES       2000
#define AXIS_META_MAX_SOURCES        16
#define AXIS_META_TITLE_MAX_CHARS    500
#define AXIS_META_NAME_MAX_CHARS     200
#define AXIS_META_GENRE_MAX_CHARS    64
#define AXIS_META_OVERVIEW_MAX_CHARS 4000

/* Search text: what is typed is cut to this many characters. */
#define AXIS_META_QUERY_MAX_CHARS    100

/* Years and dates outside this range are not believed. */
#define AXIS_META_YEAR_MIN           1870
#define AXIS_META_YEAR_MAX           2200

/**
 * AxisMetaKind:
 * @AXIS_META_KIND_FILM: a film
 * @AXIS_META_KIND_SERIES: a series, with seasons and episodes
 *
 * Music and books add theirs later.
 */
typedef enum
{
	AXIS_META_KIND_FILM = 1,
	AXIS_META_KIND_SERIES = 2
} AxisMetaKind;

/* The bit of @kind in a provider's set of kinds. */
#define AXIS_META_KIND_BIT(kind) (1u << (guint)(kind))

/* "film", "series"; %NULL for anything else. */
const gchar *
axis_meta_kind_to_string(AxisMetaKind kind);

gboolean
axis_meta_kind_from_string(
	const gchar  *text,
	AxisMetaKind *kind
);

typedef enum
{
	AXIS_META_STATUS_UNKNOWN = 0,
	AXIS_META_STATUS_RELEASED,
	AXIS_META_STATUS_CONTINUING,
	AXIS_META_STATUS_ENDED,
	AXIS_META_STATUS_UPCOMING
} AxisMetaStatus;

/* "released", "continuing", "ended", "upcoming"; %NULL for unknown. */
const gchar *
axis_meta_status_to_string(AxisMetaStatus status);

/* Unknown for anything that is not one of the four. */
AxisMetaStatus
axis_meta_status_from_string(const gchar *text);

typedef struct
{
	gchar *scheme;	/* "tmdb", "imdb", "tvdb", or a provider's own (its name) */
	gchar *value;
} AxisMetaExternalId;

typedef struct
{
	gchar *title;
	gchar *country;		/* ISO 3166-1 alpha-2, upper case, or %NULL */
	gchar *language;	/* ISO 639, lower case, or %NULL */
} AxisMetaAltTitle;

/**
 * AxisMetaEpisode:
 * @provider_id: (nullable): the provider's own identifier of the episode
 *   (axis_meta_episode_id_valid()), which stays when the provider numbers it
 *   anew; %NULL when the provider gives none
 */
typedef struct
{
	gint   season_number;
	gint   number;
	gint   absolute_number;	/* 0 for none */
	gchar *name;
	gchar *air_date;	/* "YYYY-MM-DD" or %NULL */
	gchar *overview;
	gint   runtime;		/* minutes, 0 for unknown */
	gchar *provider_id;
} AxisMetaEpisode;

typedef struct
{
	gint       number;
	gchar     *name;
	gchar     *air_date;
	gchar     *overview;
	gint       episode_count;	/* -1 for unknown */
	GPtrArray *episodes;		/* AxisMetaEpisode; %NULL: not part of the answer */
	gint64     fetched_us;		/* from the store: when it was stored, 0 if not */
	gint64     episodes_fetched_us;	/* from the store: when its episodes were */
} AxisMetaSeason;

/**
 * AxisMetaTitle:
 * @id: the store's id, 0 for an answer not stored yet
 * @kind: film or series
 * @provider: the provider it came from
 * @provider_id: that provider's own id for it (also in @ids, under the
 *   provider's own scheme)
 * @genres: (element-type utf8) (nullable): %NULL when the answer did not
 *   include them (a search result), so that a stored list is kept
 * @alt_titles: (element-type AxisMetaAltTitle) (nullable): as @genres
 * @seasons: (element-type AxisMetaSeason) (nullable): a series' seasons,
 *   without their episodes; as @genres
 * @regional_year: a film's full record: the year of its theatrical release
 *   in the region when that year is not its first release's and within two
 *   years of it; 0 for none (a search result never gives one)
 * @detail: whether this is a title's full record (%FALSE: a search result)
 * @fetched_us, @detail_fetched_us, @viewed_us: from the store, 0 if never
 * @sources: (element-type utf8) (nullable): a series' full record from a
 *   provider with several episode orders: the sources of numbering it has
 *   (axis_meta_source_valid(), the provider's own, each once, at most
 *   %AXIS_META_MAX_SOURCES); %NULL when the answer does not say
 *
 * Text fields are cleaned (valid UTF-8, no control characters, cut to
 * their limits) or %NULL; numbers are 0 for unknown.
 */
typedef struct
{
	gint64          id;
	AxisMetaKind    kind;
	gchar          *provider;
	gchar          *provider_id;
	gchar          *title;
	gchar          *original_title;
	gint            year;
	gchar          *overview;
	gint            runtime;
	AxisMetaStatus  status;
	GPtrArray      *genres;
	gchar          *original_language;
	gchar          *release_date;
	gchar          *first_air_date;
	gchar          *last_air_date;
	gchar          *poster_ref;
	gchar          *backdrop_ref;
	GPtrArray      *ids;
	GPtrArray      *alt_titles;
	GPtrArray      *seasons;
	gint            regional_year;
	gboolean        detail;
	gint64          fetched_us;
	gint64          detail_fetched_us;
	gint64          viewed_us;
	GPtrArray      *sources;
} AxisMetaTitle;

AxisMetaTitle *
axis_meta_title_new(AxisMetaKind kind);

void
axis_meta_title_free(AxisMetaTitle *title);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMetaTitle, axis_meta_title_free)

AxisMetaSeason *
axis_meta_season_new(gint number);

void
axis_meta_season_free(AxisMetaSeason *season);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMetaSeason, axis_meta_season_free)

AxisMetaEpisode *
axis_meta_episode_new(void);

void
axis_meta_episode_free(AxisMetaEpisode *episode);

void
axis_meta_alt_title_free(AxisMetaAltTitle *alt);

void
axis_meta_external_id_free(AxisMetaExternalId *id);

/* Returns: the value of @title's id in @scheme, or %NULL */
const gchar *
axis_meta_title_get_id(
	const AxisMetaTitle *title,
	const gchar         *scheme
);

/**
 * axis_meta_title_add_id:
 *
 * Adds @scheme:@value when it is a valid identifier
 * (axis_meta_id_valid()), the title has fewer than %AXIS_META_MAX_IDS and
 * none in that scheme yet.
 *
 * Returns: %TRUE if it was added
 */
gboolean
axis_meta_title_add_id(
	AxisMetaTitle *title,
	const gchar   *scheme,
	const gchar   *value
);

/* ── Lists ──────────────────────────────────────────────────────── */

/**
 * AxisMetaList:
 * @AXIS_META_LIST_POPULAR: what many people look at now
 * @AXIS_META_LIST_TRENDING: what is rising this week
 * @AXIS_META_LIST_NOW: films in cinemas now; series with an episode in the
 *   coming week
 * @AXIS_META_LIST_UPCOMING: films coming to cinemas soon
 * @AXIS_META_LIST_TOP_RATED: the best rated
 * @AXIS_META_LIST_RECOMMENDED: the provider's recommendations for one title
 *
 * The lists of titles a provider may offer (AxisMetaProviderInfo.list).
 */
typedef enum
{
	AXIS_META_LIST_POPULAR = 0,
	AXIS_META_LIST_TRENDING,
	AXIS_META_LIST_NOW,
	AXIS_META_LIST_UPCOMING,
	AXIS_META_LIST_TOP_RATED,
	AXIS_META_LIST_RECOMMENDED
} AxisMetaList;

#define AXIS_META_N_LISTS 6

/* The bit of @list in a provider's set of lists. */
#define AXIS_META_LIST_BIT(list) (1u << (guint)(list))

/* The most pages of one list ever asked for. */
#define AXIS_META_LIST_MAX_PAGE 500

/* "popular", "trending", "now", "upcoming", "top_rated", "recommended";
 * %NULL for anything else. */
const gchar *
axis_meta_list_to_string(AxisMetaList list);

gboolean
axis_meta_list_from_string(
	const gchar  *text,
	AxisMetaList *list
);

/**
 * AxisMetaListEntry:
 * @title: (transfer full): a brief record; after axis_meta_fetch_list() its
 *   @id is the store's
 * @rating: the provider's average rating in tenths of a point out of ten
 *   (0 to 100), -1 when there is none (no votes, or not stated)
 * @votes: how many rated it, 0 when not stated
 * @genre_ids: (element-type gint): the provider's genre numbers (1 and up),
 *   each once, at most %AXIS_META_MAX_GENRES; never %NULL
 */
typedef struct
{
	AxisMetaTitle *title;
	gint           rating;
	gint           votes;
	GArray        *genre_ids;
} AxisMetaListEntry;

/* An entry holding @title (transfer full), with no rating, votes or genres. */
AxisMetaListEntry *
axis_meta_list_entry_new(AxisMetaTitle *title);

void
axis_meta_list_entry_free(AxisMetaListEntry *entry);

/**
 * AxisMetaListPage:
 * @entries: (element-type AxisMetaListEntry): in the provider's order, each
 *   title once
 * @total_pages: how many pages the provider says the list has, 0 when it
 *   does not say
 */
typedef struct
{
	GPtrArray *entries;
	guint      total_pages;
} AxisMetaListPage;

AxisMetaListPage *
axis_meta_list_page_new(void);

void
axis_meta_list_page_free(AxisMetaListPage *page);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMetaListPage, axis_meta_list_page_free)

/* A provider's genre: its number (1 and up) and its name, cleaned. */
typedef struct
{
	gint   id;
	gchar *name;
} AxisMetaGenre;

void
axis_meta_genre_free(AxisMetaGenre *genre);

/* The most genres of one kind kept. */
#define AXIS_META_MAX_GENRE_LIST 100

/**
 * axis_meta_list_page_check:
 * @page: a provider's answer for one page of a list of @kind
 * @provider: the provider that gave it
 * @problem: (out) (optional) (transfer full): why not
 *
 * At most %AXIS_META_MAX_RESULTS entries, each a brief record of @kind that
 * passes axis_meta_title_check(), each title once, its rating, votes and
 * genre numbers within their limits.
 *
 * Returns: %TRUE if it may be stored
 */
gboolean
axis_meta_list_page_check(
	const AxisMetaListPage  *page,
	AxisMetaKind             kind,
	const gchar             *provider,
	gchar                  **problem
);

/* As axis_meta_list_page_check() for a list of genres: at most
 * %AXIS_META_MAX_GENRE_LIST, numbers 1 and up each once, names clean. */
gboolean
axis_meta_genres_check(
	const GPtrArray  *genres,
	gchar           **problem
);

/* ── Cleaning and checking what a provider said ─────────────────── */

/**
 * axis_meta_clean_text:
 * @text: (nullable): a text from an answer
 * @max_chars: at most this many characters are kept
 *
 * Valid UTF-8, every control or direction-changing character replaced by a
 * space, runs of white space made one space, none at the ends.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when nothing is
 *   left
 */
gchar *
axis_meta_clean_text(
	const gchar *text,
	guint        max_chars
);

/* Whether @text is a real date "YYYY-MM-DD" within the years believed. */
gboolean
axis_meta_date_valid(const gchar *text);

/* Whether @year is within the years believed. */
gboolean
axis_meta_year_valid(gint64 year);

/* A provider's name, which is also its own identifier scheme: 1 to 16
 * lower-case letters and digits, starting with a letter. */
gboolean
axis_meta_provider_name_valid(const gchar *name);

/* "tmdb", "imdb", "tvdb", or any valid provider name. */
gboolean
axis_meta_scheme_valid(const gchar *scheme);

/**
 * axis_meta_source_valid:
 *
 * A source of numbering (add-metadata-tvdb): the seasons and episodes of a
 * series as one provider numbers them.  A provider's name alone is its own
 * seasons ("tmdb"; "tvdb", TheTVDB's aired order); followed by '-' and 1 to
 * 16 lower-case letters, another of its orders ("tvdb-dvd").
 *
 * Returns: %TRUE if @source is of that form
 */
gboolean
axis_meta_source_valid(const gchar *source);

/* The provider @source belongs to ("tvdb" for "tvdb-dvd"), or %NULL when it
 * is not a source. */
gchar *
axis_meta_source_provider(const gchar *source);

/**
 * axis_meta_id_valid:
 *
 * imdb: "tt" and 7 to 10 digits; tmdb and tvdb: 1 to 12 digits, not
 * starting with 0; another provider's own scheme: 1 to 24 letters and
 * digits.
 *
 * Returns: %TRUE if @value is an identifier in @scheme
 */
gboolean
axis_meta_id_valid(
	const gchar *scheme,
	const gchar *value
);

/* A provider's own identifier of an episode: 1 to 24 ASCII letters and
 * digits. */
gboolean
axis_meta_episode_id_valid(const gchar *id);

/* An image reference as a provider names it: "/" and then 1 to 199 letters,
 * digits, '.', '_', '-' and '/', with no empty, "." or ".." part. */
gboolean
axis_meta_image_ref_valid(const gchar *ref);

/* Two upper-case letters. */
gboolean
axis_meta_country_valid(const gchar *country);

/* Two or three lower-case letters. */
gboolean
axis_meta_language_valid(const gchar *language);

/**
 * axis_meta_title_check:
 * @title: an answer
 * @provider: the provider that gave it
 * @problem: (out) (optional) (transfer full): why not
 *
 * The whole answer is within its limits: a kind, a title, its provider and
 * the provider's own id (also among its ids), valid ids, dates, year,
 * references, codes and counts, clean texts, and its seasons (if any)
 * valid and each number once.
 *
 * Returns: %TRUE if it may be stored
 */
gboolean
axis_meta_title_check(
	const AxisMetaTitle  *title,
	const gchar          *provider,
	gchar               **problem
);

/* As axis_meta_title_check() for a season with its episodes: numbers in
 * range, each episode number once, and every episode in this season. */
gboolean
axis_meta_season_check(
	const AxisMetaSeason  *season,
	gchar                **problem
);

/* ── Search text ────────────────────────────────────────────────── */

/**
 * axis_meta_query_clean:
 * @text: (nullable): what was typed
 *
 * Returns: (transfer full) (nullable): what is sent: cleaned as
 *   axis_meta_clean_text(), cut to %AXIS_META_QUERY_MAX_CHARS; %NULL when
 *   nothing is left
 */
gchar *
axis_meta_query_clean(const gchar *text);

/**
 * axis_meta_query_key:
 * @text: (nullable): what was typed
 *
 * The text a saved search is found by: axis_meta_query_clean(), then
 * compatibility-normalised and case-folded, so that "The  Mazework" and "the
 * mazework" are one search.  Deterministic; at most 300 bytes.
 *
 * Returns: (transfer full) (nullable): the key, or %NULL when nothing is left
 */
gchar *
axis_meta_query_key(const gchar *text);

G_END_DECLS

#endif /* AXIS_META_MODEL_H */
