/*
 * axis-meta-tmdb-parse.h - TMDb's version 3 answers, read into the metadata
 * model, and the few rules about TMDb's values that need no network
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything TMDb says is untrusted input.  Every field is optional and an
 * unreadable one (missing, null, the wrong type, out of range, not valid
 * text) is left out; an entry without an identifier or a title is skipped;
 * lists are cut at the model's limits; a document that is not an object, or
 * lacks the list or identifier the call is about, is
 * %AXIS_META_ERROR_UNREADABLE and gives nothing.  What comes back passes
 * axis_meta_title_check() and axis_meta_season_check().  No address in an
 * answer is ever used: an image is a path of the strict form below or
 * nothing.
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h), no
 * I/O: read by tests/fuzz/fuzz-meta-tmdb.c.  The provider itself, which
 * makes the requests, is meta/axis-meta-tmdb.h.
 */

#ifndef AXIS_META_TMDB_PARSE_H
#define AXIS_META_TMDB_PARSE_H

#include <glib.h>

#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

#define AXIS_META_TMDB_NAME "tmdb"

/**
 * axis_meta_tmdb_parse_search:
 * @kind: film or series
 * @data: the answer of /3/search/movie or /3/search/tv
 * @adult: whether titles TMDb marks as adult are kept
 * @titles: (out) (transfer full): brief records of @kind, at most
 *   %AXIS_META_MAX_RESULTS, each TMDb id once
 */
gboolean
axis_meta_tmdb_parse_search(
	AxisMetaKind   kind,
	const gchar   *data,
	gsize          length,
	gboolean       adult,
	GPtrArray    **titles,
	GError       **error
);

/**
 * axis_meta_tmdb_parse_find:
 * @kind: film or series: the results of that kind are read
 * @scheme: (nullable): the scheme of the identifier that was asked for
 *   ("imdb", "tvdb")
 * @value: (nullable): the identifier; when exactly one title came back it
 *   carries it
 * @data: the answer of /3/find
 */
gboolean
axis_meta_tmdb_parse_find(
	AxisMetaKind   kind,
	const gchar   *scheme,
	const gchar   *value,
	const gchar   *data,
	gsize          length,
	gboolean       adult,
	GPtrArray    **titles,
	GError       **error
);

/**
 * axis_meta_tmdb_parse_title:
 * @kind: film or series
 * @region: (nullable): the country (upper case, two letters) whose release
 *   dates are read for a film
 * @data: the answer of /3/movie/{id} or /3/tv/{id} with the alternative
 *   titles, external identifiers and (films) release dates appended
 *
 * A film's release date is the earliest theatrical (limited or wide),
 * digital or physical date TMDb lists for @region; without one it is TMDb's
 * own release date.  Its year is the year of TMDb's own date, so that it is
 * the same in search results and here.
 */
gboolean
axis_meta_tmdb_parse_title(
	AxisMetaKind    kind,
	const gchar    *region,
	const gchar    *data,
	gsize           length,
	AxisMetaTitle **title,
	GError        **error
);

/* Season @number of a series with its episodes (/3/tv/{id}/season/{n}).
 * Absolute numbers are not TMDb's and stay 0. */
gboolean
axis_meta_tmdb_parse_season(
	gint             number,
	const gchar     *data,
	gsize            length,
	AxisMetaSeason **season,
	GError         **error
);

/* ── Lists ──────────────────────────────────────────────────────── */

/* The lists TMDb offers: every one for films; no upcoming list for series. */
#define AXIS_META_TMDB_FILM_LISTS \
	(AXIS_META_LIST_BIT(AXIS_META_LIST_POPULAR) | AXIS_META_LIST_BIT(AXIS_META_LIST_TRENDING) | \
	 AXIS_META_LIST_BIT(AXIS_META_LIST_NOW) | AXIS_META_LIST_BIT(AXIS_META_LIST_UPCOMING) | \
	 AXIS_META_LIST_BIT(AXIS_META_LIST_TOP_RATED) | AXIS_META_LIST_BIT(AXIS_META_LIST_RECOMMENDED))
#define AXIS_META_TMDB_SERIES_LISTS \
	(AXIS_META_LIST_BIT(AXIS_META_LIST_POPULAR) | AXIS_META_LIST_BIT(AXIS_META_LIST_TRENDING) | \
	 AXIS_META_LIST_BIT(AXIS_META_LIST_NOW) | AXIS_META_LIST_BIT(AXIS_META_LIST_TOP_RATED) | \
	 AXIS_META_LIST_BIT(AXIS_META_LIST_RECOMMENDED))

/**
 * axis_meta_tmdb_list_path:
 * @id: (nullable): for recommendations, TMDb's id of the title (a valid
 *   one); %NULL for every other list
 *
 * The path of @list of @kind without its query: "/3/movie/popular",
 * "/3/trending/tv/week", "/3/movie/12345/recommendations" and so on.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL when TMDb has no
 *   such list (or @id is missing or not valid)
 */
gchar *
axis_meta_tmdb_list_path(
	AxisMetaKind  kind,
	AxisMetaList  list,
	const gchar  *id
);

/* Whether @list of @kind is asked for with the region setting: the film
 * lists but trending and recommendations. */
gboolean
axis_meta_tmdb_list_regional(
	AxisMetaKind kind,
	AxisMetaList list
);

/**
 * axis_meta_tmdb_parse_list:
 * @kind: film or series
 * @data: the answer of a list (/3/movie/popular, /3/trending/..., ...)
 * @adult: whether titles TMDb marks as adult are kept
 * @page: (out) (transfer full): brief records of @kind, at most
 *   %AXIS_META_MAX_RESULTS, each TMDb id once, with the rating (vote_average
 *   in tenths; none without a vote), the vote count, the genre numbers and
 *   the pages TMDb says the list has (at most %AXIS_META_LIST_MAX_PAGE)
 *
 * An entry of the other kind (a "media_type" that is not @kind's) is dropped.
 */
gboolean
axis_meta_tmdb_parse_list(
	AxisMetaKind        kind,
	const gchar        *data,
	gsize               length,
	gboolean            adult,
	AxisMetaListPage  **page,
	GError            **error
);

/* The genres of /3/genre/movie/list or /3/genre/tv/list (AxisMetaGenre),
 * each number once, at most %AXIS_META_MAX_GENRE_LIST. */
gboolean
axis_meta_tmdb_parse_genres(
	const gchar  *data,
	gsize         length,
	GPtrArray   **genres,
	GError      **error
);

/* ── TMDb's own forms ───────────────────────────────────────────── */

/* "/", 1 to 100 letters and digits, then ".jpg", ".png" or ".webp". */
gboolean
axis_meta_tmdb_image_ref_valid(const gchar *ref);

/* The path on the image origin: "/t/p/w342/<file>" (poster cards),
 * "/t/p/w780/<file>" (poster), "/t/p/w1280/<file>" (backdrop); %NULL for a
 * reference of any other form. */
gchar *
axis_meta_tmdb_image_path(
	const gchar     *ref,
	AxisMetaArtSize  size
);

/**
 * axis_meta_tmdb_key_valid:
 *
 * A check of the key setting: printable characters without spaces, and not
 * the short version 3 API key (32 hexadecimal characters), which TMDb's
 * bearer authentication does not take.  The reason says which value to paste
 * and never contains the text.
 */
gboolean
axis_meta_tmdb_key_valid(
	const gchar  *text,
	gchar       **reason
);

/* "en", "en-US", "pt-BR": two or three lower-case letters, then optionally
 * a hyphen and two upper-case. */
gboolean
axis_meta_tmdb_language_valid(const gchar *text);

/* Two upper-case letters. */
gboolean
axis_meta_tmdb_region_valid(const gchar *text);

G_END_DECLS

#endif /* AXIS_META_TMDB_PARSE_H */
