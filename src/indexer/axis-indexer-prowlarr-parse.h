/*
 * axis-indexer-prowlarr-parse.h - Prowlarr's JSON answers, read into the
 * indexer layer's words, and the one place that knows how a search is asked
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything Prowlarr says is untrusted input, and through it everything
 * every indexer says: a release's name comes from whoever uploaded it.  What
 * is known of Prowlarr's format is its published API (v1) and the answers of
 * a real Prowlarr 3.0.1, recorded and scrubbed (tests/fixtures/prowlarr and
 * its README).  Still every field is optional and type-checked: one that is
 * missing, null, of the wrong type, out of range or not valid text is left
 * out, and a result without a name and a protocol is skipped.  Lists are cut
 * (at %AXIS_INDEXER_MAX_RESULTS results), strings are cleaned and cut, sizes
 * and counts are range-checked.  An identifier of 0 (imdbId, tmdbId, tvdbId)
 * is none, as Prowlarr writes it.
 *
 * A search result is accepted only with a way to fetch it: an address on the
 * source's own origin and under its base path (axis_indexer_fetch_ref()),
 * given as downloadUrl (the file) or as magnetUrl (for a torrent, Prowlarr's
 * address that redirects to the magnet link), or a valid magnet link.  The
 * file's address is kept when there is one, else the link's.  A result that
 * names an address elsewhere in either field is dropped and counted,
 * whatever else it has: a fetch is only ever made from the source.  The
 * accepted address and the magnet link are secrets (the address holds the
 * source's key as a parameter): they are in the AxisIndexerFound only, never
 * in the release the pages see.  A result's guid is read only as a hash, to
 * merge two answers (AxisIndexerFound.merge_key).
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h), no
 * I/O: read by tests/fuzz/fuzz-indexer-prowlarr.c.  The kind, which makes the
 * requests, is indexer/axis-indexer-prowlarr.h.
 */

#ifndef AXIS_INDEXER_PROWLARR_PARSE_H
#define AXIS_INDEXER_PROWLARR_PARSE_H

#include <glib.h>

#include "core/axis-http-origin.h"
#include "indexer/axis-indexer-model.h"
#include "probe/axis-probe-json.h"

G_BEGIN_DECLS

/**
 * axis_prowlarr_parse_search:
 * @data: the body of a 2xx answer of GET /api/v1/search
 * @origin: the source's configured origin
 * @base_path: its base path ("" for none)
 * @now_us: the current time (microseconds since 1970), for refusing a
 *   publication date in the future; 0 for no check
 * @outcome: (out): filled (initialised first): the accepted results and the
 *   counts of those left out; clear it with axis_indexer_outcome_clear()
 * @problem: (out) (optional) (transfer full): when this fails, why, a fixed
 *   sentence fragment
 *
 * Returns: %TRUE for an answer that is a JSON array (of anything: what
 *   cannot be read is counted as skipped)
 */
gboolean
axis_prowlarr_parse_search(
	const gchar          *data,
	gsize                 length,
	const AxisHttpOrigin *origin,
	const gchar          *base_path,
	gint64                now_us,
	AxisIndexerOutcome   *outcome,
	gchar               **problem
);

/**
 * axis_prowlarr_parse_indexers:
 * @data: the body of GET /api/v1/indexer
 * @indexers: (out) (transfer full) (element-type AxisIndexerInfo): at most
 *   %AXIS_INDEXER_MAX_INDEXERS, each with a name and a protocol, the
 *   identifiers it takes, whether its downloads redirect ("redirect"), and
 *   the host it is at (its "baseUrl" field, else its first "indexerUrls":
 *   believed, not in the recording; only the host is read)
 */
gboolean
axis_prowlarr_parse_indexers(
	const gchar  *data,
	gsize         length,
	GPtrArray   **indexers,
	gchar       **problem
);

/**
 * axis_prowlarr_parse_failing:
 * @data: the body of GET /api/v1/indexerstatus
 * @now_us: the current time
 * @ids: (out) (transfer full) (element-type gint64): the identifiers of the
 *   indexers whose status says they are disabled until a time after @now_us
 */
gboolean
axis_prowlarr_parse_failing(
	const gchar  *data,
	gsize         length,
	gint64        now_us,
	GArray      **ids,
	gchar       **problem
);

/* GET /api/v1/system/status: its "version", cleaned and cut; %NULL when it
 * has none or the answer is not an object. */
gchar *
axis_prowlarr_parse_version(
	const gchar *data,
	gsize        length
);

/**
 * axis_prowlarr_identifiers:
 * @request: a checked request
 * @modes: (out) (array fixed-size=3): the identifier searches that can be
 *   made, in the order they are tried
 *
 * Films: IMDb, then TMDb.  Episodes and seasons: TVDB, then IMDb, then TMDb.
 * A text search has none.
 *
 * Returns: how many
 */
guint
axis_prowlarr_identifiers(
	const AxisIndexerRequest *request,
	AxisIndexerMode          *modes
);

/**
 * axis_prowlarr_search_path:
 * @request: a checked request
 * @mode: how to ask: by an identifier, or by words
 *
 * The path and query of GET /api/v1/search for @request, with the search
 * written as Prowlarr's search page accepts it: for an identifier the query
 * is its form ("{ImdbId:tt0600003}", "{TmdbId:6003}", "{TvdbId:82002}"
 * followed by "{Season:1}" and "{Episode:2}"), with the type "movie" or
 * "tvsearch"; for words the type is "search" and the query is the title, the
 * year or "S01E02", as a person would type it.  Categories and indexer
 * identifiers are repeated parameters; a film is searched in 2000 and
 * episodes in 5000 unless the request says.  Everything in the query is
 * percent-encoded.
 *
 * This is the one function that knows these forms.  They were confirmed
 * against a real Prowlarr 3.0.1 (tests/fixtures/prowlarr/README.md).
 *
 * Returns: (nullable): the path, or %NULL when @request cannot be asked in
 *   @mode (the identifier is missing, or there are no words)
 */
gchar *
axis_prowlarr_search_path(
	const AxisIndexerRequest *request,
	AxisIndexerMode           mode
);

G_END_DECLS

#endif /* AXIS_INDEXER_PROWLARR_PARSE_H */
