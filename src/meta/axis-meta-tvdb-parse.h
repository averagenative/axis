/*
 * axis-meta-tvdb-parse.h - TheTVDB's API version 4 answers, read into the
 * metadata model, and the few rules about TheTVDB's values that need no
 * network
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything TheTVDB says is untrusted input, read as TMDb's is
 * (meta/axis-meta-tmdb-parse.h): every field optional and an unreadable one
 * left out; an entry without an identifier or a name skipped; lists cut at
 * the model's limits; a document that is not an object, that does not say
 * "success", or that lacks what the call is about is
 * %AXIS_META_ERROR_UNREADABLE and gives nothing.  What comes back passes
 * axis_meta_title_check() and axis_meta_season_check().  No address in an
 * answer is ever requested: an image is kept only as a path on TheTVDB's one
 * image origin, and a "next page" address only says that there is one.
 * Where the documentation is silent, the assumption is in
 * meta/axis-meta-tvdb-assumed.h.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-meta-tvdb.c.  The provider,
 * which makes the requests, is meta/axis-meta-tvdb.h.
 */

#ifndef AXIS_META_TVDB_PARSE_H
#define AXIS_META_TVDB_PARSE_H

#include <glib.h>

#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

#define AXIS_META_TVDB_NAME "tvdb"

/* TheTVDB's aired order: the source of numbering "tvdb". */
#define AXIS_META_TVDB_AIRED_ORDER "official"

/* ── Episode orders ─────────────────────────────────────────────── */

/**
 * axis_meta_tvdb_source_of_order:
 * @type: one of TheTVDB's order type strings ("official", "dvd", ...)
 *
 * The source of numbering Axis keeps it under: "tvdb" for the aired order,
 * "tvdb-dvd", "tvdb-alternate", "tvdb-regional", "tvdb-altdvd" and
 * "tvdb-alttwo" for the others.  The absolute order and "default" are no
 * source (absolute numbers are kept on each episode instead).
 *
 * Returns: (nullable): the source, or %NULL
 */
const gchar *
axis_meta_tvdb_source_of_order(const gchar *type);

/* The order type string of one of TheTVDB's sources, or %NULL. */
const gchar *
axis_meta_tvdb_order_of_source(const gchar *source);

/* Every source above, ended by %NULL (the provider's info->sources). */
const gchar *const *
axis_meta_tvdb_sources(void);

/* ── Answers ────────────────────────────────────────────────────── */

/* The token of /v4/login's answer (data.token), a secret: printable ASCII
 * without spaces, at most %AXIS_META_TOKEN_MAX_BYTES. */
gboolean
axis_meta_tvdb_parse_login(
	const gchar  *data,
	gsize         length,
	gchar       **token,
	GError      **error
);

/**
 * axis_meta_tvdb_parse_search:
 * @language: the language setting ("eng"): a result's name and overview in
 *   it, when the answer has them
 * @titles: (out) (transfer full): brief records of series, at most
 *   %AXIS_META_MAX_RESULTS, each TheTVDB id once, with no other provider's
 *   identifier (a brief record makes no claim about other schemes)
 *
 * The answer of /v4/search?type=series: entries of another type are dropped.
 */
gboolean
axis_meta_tvdb_parse_search(
	const gchar  *data,
	gsize         length,
	const gchar  *language,
	GPtrArray   **titles,
	GError      **error
);

/**
 * axis_meta_tvdb_parse_series:
 * @language: as for a search
 * @title: (out) (transfer full): the series' full record: its name (in
 *   @language when translated, else its own), its original name, overview,
 *   year, dates, state, average running time, genres, other names (aliases
 *   and translations), poster, its IMDb and TMDb identifiers from its remote
 *   ids, the orders it has (@title->sources) and its aired order's seasons
 *   (without their episodes)
 *
 * The answer of /v4/series/{id}/extended?meta=translations&short=true.
 */
gboolean
axis_meta_tvdb_parse_series(
	const gchar    *data,
	gsize           length,
	const gchar    *language,
	AxisMetaTitle **title,
	GError        **error
);

/**
 * axis_meta_tvdb_parse_remote:
 * @scheme: the scheme of the identifier that was asked for ("imdb")
 * @value: the identifier; when exactly one series came back it carries it
 *
 * The answer of /v4/search/remoteid/{id}: the series in it, as brief records.
 */
gboolean
axis_meta_tvdb_parse_remote(
	const gchar  *data,
	gsize         length,
	const gchar  *scheme,
	const gchar  *value,
	const gchar  *language,
	GPtrArray   **titles,
	GError      **error
);

/**
 * axis_meta_tvdb_parse_episodes:
 * @seasons: (element-type AxisMetaSeason): the seasons read so far (with
 *   their episodes); this page's episodes are added, a season made for each
 *   number new to it, kept in number order
 * @more: (out): the answer says another page follows
 *
 * One page of /v4/series/{id}/episodes/{order}/{lang}.  An episode without a
 * season or a number, or with a place or a TheTVDB id already read, is
 * skipped; at most %AXIS_META_MAX_SEASONS seasons and
 * %AXIS_META_MAX_EPISODES episodes each are kept.  Fails, adding nothing,
 * when the page is not readable.
 */
gboolean
axis_meta_tvdb_parse_episodes(
	const gchar  *data,
	gsize         length,
	GPtrArray    *seasons,
	gboolean     *more,
	GError      **error
);

/* The names of the orders /v4/seasons/types lists ("Aired Order", ...), at
 * most %AXIS_META_MAX_SOURCES * 4, each cleaned. */
gboolean
axis_meta_tvdb_parse_orders(
	const gchar  *data,
	gsize         length,
	GPtrArray   **names,
	GError      **error
);

/* ── TheTVDB's own forms ────────────────────────────────────────── */

/**
 * axis_meta_tvdb_image_ref:
 * @url: (nullable): an image address from an answer
 *
 * Returns: (transfer full) (nullable): its path, when it is a plain https
 *   address on %AXIS_META_TVDB_IMAGE_ORIGIN (no user, port, query or
 *   fragment) whose path passes axis_meta_image_ref_valid() and ends in
 *   ".jpg", ".jpeg", ".png" or ".webp"; %NULL for any other
 */
gchar *
axis_meta_tvdb_image_ref(const gchar *url);

/* The path on the image origin of @ref at @size: @ref itself (no thumbnail
 * form is assumed), or %NULL for a reference of another form. */
gchar *
axis_meta_tvdb_image_path(
	const gchar     *ref,
	AxisMetaArtSize  size
);

/* A key or a PIN as a login can carry it: 1 to 255 printable characters
 * without a space, a quote or a backslash.  The reason never holds it. */
gboolean
axis_meta_tvdb_secret_valid(
	const gchar  *text,
	gchar       **reason
);

/* Three lower-case letters ("eng"). */
gboolean
axis_meta_tvdb_language_valid(const gchar *text);

G_END_DECLS

#endif /* AXIS_META_TVDB_PARSE_H */
