/*
 * axis-indexer-newznab-parse.h - Newznab and Torznab answers, read into the
 * indexer layer's words, and the one place that knows how a search is asked
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Newznab is the API every Usenet indexer speaks, with the user's own key;
 * Torznab is the same for torrents (Jackett, and Prowlarr's per-indexer
 * feeds).  One endpoint, <base>/api, takes "t" (the function) and "apikey"
 * in the query: t=caps describes the indexer, t=search, t=movie and
 * t=tvsearch answer with an RSS document of <item>s, and an error is an
 * <error code description> document, usually with status 200.
 *
 * What is known of the formats is the published specifications and the
 * documented answers (tests/fixtures/newznab/README.md lists every detail
 * believed and not verified: no real indexer was asked).  Everything is read
 * with the bounded reader of indexer/axis-indexer-xml.h, and the whole
 * document must be read without error before anything of it is taken: an
 * answer that is refused anywhere gives nothing.  Every field is optional and
 * checked: a number out of range, a date that cannot be read, an address
 * that is not one is left out; an item without a title is skipped, one
 * without a way to fetch it is counted and left out.  Names are cleaned and
 * cut; the attributes are matched by their local name ("attr" whether the
 * indexer wrote "newznab:attr" or "torznab:attr", or another prefix).
 *
 * A result's file is accepted only on the indexer's own configured origin
 * (scheme, host and port; any path: a Torznab provider such as Jackett
 * serves its files outside the feed's base path), or on the other origin an
 * administrator accepted for that indexer.  A result that names another
 * address is left out and counted, whatever else it has, and the first such
 * origin is kept for the administrator's test to show; nothing is ever
 * fetched from it.  The accepted address is a secret (it usually holds the
 * key or a token of the user's).  A Torznab result may give a magnet link
 * (its enclosure, its link, or the magneturl attribute): checked, and a
 * secret too.  A result's guid is read only as a hash, to tell results of
 * one indexer apart.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-indexer-newznab.c.  The kinds,
 * which make the requests, are indexer/axis-indexer-newznab.h.
 */

#ifndef AXIS_INDEXER_NEWZNAB_PARSE_H
#define AXIS_INDEXER_NEWZNAB_PARSE_H

#include <glib.h>

#include "core/axis-http-origin.h"
#include "indexer/axis-indexer-kind.h"
#include "indexer/axis-indexer-model.h"

G_BEGIN_DECLS

/* The capabilities' path (the key is added by the connection). */
#define AXIS_NEWZNAB_CAPS_PATH "/api?t=caps"

/* How many results a search asks for when the indexer did not say. */
#define AXIS_NEWZNAB_DEFAULT_LIMIT 100

/* The most pages a search reads: further results are left, and the result
 * says it was cut. */
#define AXIS_NEWZNAB_MAX_PAGES 5

/* What an indexer's error document said. */
typedef struct
{
	gint   code;
	gchar *description;	/* (nullable) cleaned and cut */
} AxisNewznabError;

void
axis_newznab_error_clear(AxisNewznabError *error);

typedef enum
{
	AXIS_NEWZNAB_READ_OK,
	AXIS_NEWZNAB_READ_ERROR,	/* the answer is an error document */
	AXIS_NEWZNAB_READ_INVALID	/* the answer cannot be read */
} AxisNewznabRead;

/**
 * axis_newznab_error_kind:
 * @code: a Newznab error code
 *
 * What it means for Axis: 100 to 199 (credentials, the account, its
 * privileges) %AXIS_INDEXER_ERROR_AUTH; 500 (the request limit) and 501
 * (the download limit) %AXIS_INDEXER_ERROR_RATE_LIMITED; 200 to 299 (a
 * missing or wrong parameter, a function not available) and 300 (no such
 * item) %AXIS_INDEXER_ERROR_REFUSED; 910 (the API is disabled)
 * %AXIS_INDEXER_ERROR_AUTH; anything else %AXIS_INDEXER_ERROR_FAILED.
 */
AxisIndexerError
axis_newznab_error_kind(gint code);

/**
 * axis_newznab_parse_caps:
 * @caps: (out): filled when the answer is a caps document (cleared first)
 * @allowance: (out) (optional): the allowances, when the answer states them
 *   (initialised first)
 * @version: (out) (optional) (transfer full): what the indexer says it is
 *   (its server's title and version), cleaned; %NULL when it says nothing
 * @error: (out): for an error document, its code and description
 * @problem: (out) (optional) (transfer full): for an answer that cannot be
 *   read, why, a fixed sentence fragment
 *
 * Reads t=caps.  A search that is "available" without "supportedParams" is
 * taken to accept what the specification gives by default (search: q;
 * tv-search: q, season, ep; movie-search: q, imdbid).
 */
AxisNewznabRead
axis_newznab_parse_caps(
	const gchar          *data,
	gsize                 length,
	AxisIndexerCaps      *caps,
	AxisIndexerAllowance *allowance,
	gchar               **version,
	AxisNewznabError     *error,
	gchar               **problem
);

/**
 * AxisNewznabContext:
 * @origin: the indexer's configured origin
 * @files_origin: (nullable): the other origin accepted for its files
 * @protocol: usenet (Newznab) or torrent (Torznab)
 * @now_us: the current time, for refusing a date far in the future; 0 for
 *   no check
 */
typedef struct
{
	const AxisHttpOrigin *origin;
	const AxisHttpOrigin *files_origin;
	AxisDownloadProtocol  protocol;
	gint64                now_us;
} AxisNewznabContext;

/**
 * axis_newznab_parse_search:
 * @outcome: (out): filled (initialised first) when the answer is a feed: the
 *   accepted results (each name, fetch address or magnet link checked; the
 *   release's indexer is the caller's to set) and the counts of those left
 *   out; clear it with axis_indexer_outcome_clear()
 * @allowance: (out) (optional): the allowances, when the answer states them
 * @error: (out): for an error document, its code and description
 *
 * Reads the RSS answer of t=search, t=movie or t=tvsearch.  Results are cut
 * at %AXIS_INDEXER_MAX_RESULTS and no more than %AXIS_INDEXER_MAX_SCAN are
 * looked at; two results of the same guid are one.
 */
AxisNewznabRead
axis_newznab_parse_search(
	const gchar               *data,
	gsize                      length,
	const AxisNewznabContext  *context,
	AxisIndexerOutcome        *outcome,
	AxisIndexerAllowance      *allowance,
	AxisNewznabError          *error,
	gchar                    **problem
);

/**
 * axis_newznab_parse_error_only:
 *
 * Whether @data is a well-formed error document; its code and description
 * in @error.  For an answer to a file's fetch that is not a file.
 */
gboolean
axis_newznab_parse_error_only(
	const gchar      *data,
	gsize             length,
	AxisNewznabError *error
);

/**
 * axis_newznab_identifiers:
 * @request: a checked request
 * @caps: (nullable): the indexer's capabilities; %NULL or never read: none
 * @modes: (out) (array fixed-size=3): the identifier searches the indexer
 *   takes for @request, in the order they are tried
 *
 * Movies: IMDb, then TMDb, by t=movie.  Episodes and seasons: TVDB, then
 * IMDb, then TMDb, by t=tvsearch.  Only those its capabilities name.
 *
 * Returns: how many
 */
guint
axis_newznab_identifiers(
	const AxisIndexerRequest *request,
	const AxisIndexerCaps    *caps,
	AxisIndexerMode          *modes
);

/**
 * axis_newznab_search_path:
 * @request: a checked request
 * @mode: by an identifier (one axis_newznab_identifiers() gave) or by words
 * @caps: (nullable): the indexer's capabilities
 * @categories: the categories to search in (the request's own, else the
 *   indexer's for movies or for TV); @n_categories of them
 *
 * The path and query of the search, without the key (the connection adds
 * it): "/api?t=movie&imdbid=0600001&cat=2000&extended=1&limit=100&offset=0".
 * An IMDb identifier is its digits without "tt".  By words: t=movie or
 * t=tvsearch with q (and season and ep) where the capabilities say those
 * take words, else t=search with q as a person would type it ("Hearth 1995",
 * "The Show S01E02").  Everything in the query is percent-encoded.
 *
 * Returns: (nullable): the path, or %NULL when @request cannot be asked in
 *   @mode
 */
gchar *
axis_newznab_search_path(
	const AxisIndexerRequest *request,
	AxisIndexerMode           mode,
	const AxisIndexerCaps    *caps,
	const gint               *categories,
	guint                     n_categories
);

/* How many results one page of a search asks for (and the most a page
 * holds): the indexer's own maximum when its capabilities say, else
 * %AXIS_NEWZNAB_DEFAULT_LIMIT. */
guint
axis_newznab_page_limit(const AxisIndexerCaps *caps);

/* @path (from axis_newznab_search_path()) asking for the page that starts at
 * result @offset, or %NULL when @path does not end in "&offset=0". */
gchar *
axis_newznab_path_at_offset(
	const gchar *path,
	guint        offset
);

/* The test's search for what is newest: "/api?t=search&extended=1&limit=N"
 * with @categories when there are some. */
gchar *
axis_newznab_latest_path(
	const gint *categories,
	guint       n_categories,
	guint       limit
);

/* An RFC 822 date ("Sat, 02 Mar 2024 10:11:12 +0000"), or ISO 8601, in
 * microseconds since 1970; 0 when it cannot be read. */
gint64
axis_newznab_parse_date(const gchar *text);

/* "https://host:port" of an http or https address, %NULL otherwise. */
gchar *
axis_newznab_origin_of(const gchar *url);

G_END_DECLS

#endif /* AXIS_INDEXER_NEWZNAB_PARSE_H */
