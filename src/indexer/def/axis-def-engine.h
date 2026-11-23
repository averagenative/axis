/*
 * axis-def-engine.h - running an indexer definition: its search requests,
 * and its answers read into releases
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The engine makes no request itself (indexer/axis-indexer-cardigann.h does,
 * through the indexer layer's connection); it says which to make and reads
 * what came back.  Its promises:
 *
 *  - Every request it builds is a path and query on the indexer's one
 *    configured origin (a link of the definition's, or the address an
 *    administrator typed): a relative path is joined to it, an absolute one
 *    taken on it, and an address the definition writes in full is used only
 *    when it is that origin; anything else fails the search, with nothing
 *    asked.  A search's words are encoded in the site's encoding.
 *  - A release found in an answer is kept only when it can be fetched
 *    without leaving the site: a magnet link the downloads layer accepts, an
 *    info-hash (the magnet link is built from it), or a download address on
 *    that same origin.  A result that names only an address elsewhere is
 *    left out and counted (%AxisIndexerOutcome.n_off_origin).  When the
 *    definition reads its download from a page (download.selectors or
 *    download.infohash), the address kept is that page's, read at grab time
 *    by axis_def_read_download() under the same rule.
 *  - Names are cleaned and cut, counts and sizes read or left unstated,
 *    dates outside 1990 to a day from now dropped; at most
 *    %AXIS_INDEXER_MAX_RESULTS releases are kept and %AXIS_INDEXER_MAX_SCAN
 *    rows read.
 *  - Reading an answer is bounded: the page reader's limits
 *    (indexer/def/axis-def-html.h), a selector budget of
 *    %AXIS_DEF_SELECT_STEPS steps, and the regular expressions' bounds and
 *    time budget (indexer/def/axis-def-regex.h).  An answer past them is
 *    refused whole, saying so.
 *  - A result whose site category maps only to Newznab's console, audio, PC
 *    or books classes is left out: it is not video.
 *  - An answer that is not what the definition reads is told apart from one
 *    with no results (#AxisDefVerdict): a browser challenge's page, and
 *    drift, "its pages no longer look as expected", when the definition
 *    names what every answer has (rows.expect) and it is missing, or the
 *    answer cannot be read as the definition says and it names that; and
 *    for any definition, when at least two rows were read and more than half
 *    of them were lost for a field every result must have (no title, a field
 *    that is not optional matched nothing, no info-hash and no download).
 *  - When a trace is given (axis_def_search_set_trace()), every step is
 *    recorded in it (indexer/def/axis-def-trace.h).
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_DEF_ENGINE_H
#define AXIS_DEF_ENGINE_H

#include <glib.h>

#include "core/axis-http-origin.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-trace.h"

G_BEGIN_DECLS

/* The selector steps reading one answer may take. */
#define AXIS_DEF_SELECT_STEPS (40 * 1000 * 1000)

/**
 * AxisDefSite:
 * @def: the definition
 * @origin: the indexer's configured origin
 * @base_path: the path of its link ("" or "/forum"), which relative paths
 *   are joined to
 * @now_us: now, for dates
 *
 * Where a definition is run.
 */
typedef struct
{
	const AxisDef        *def;
	const AxisHttpOrigin *origin;
	const gchar          *base_path;
	gint64                now_us;
} AxisDefSite;

/**
 * AxisDefRequest:
 * @path: the path and query to ask of the origin
 * @headers: (array zero-terminated=1): names and values, alternately, that
 *   the definition asks for (Referer, Accept, Cookie, User-Agent), checked
 * @response: how its answer is read
 */
typedef struct
{
	gchar           *path;
	gchar          **headers;
	AxisDefResponse  response;
} AxisDefRequest;

typedef struct _AxisDefSearch AxisDefSearch;

/**
 * axis_def_search_new:
 * @request: a checked request
 * @problem: (out) (optional) (transfer full): why no search can be made
 *   (nothing to search by, a request that would leave the site), safe to
 *   show
 *
 * The search's words and variables, and the requests to make.
 *
 * Returns: (transfer full) (nullable)
 */
AxisDefSearch *
axis_def_search_new(
	const AxisDefSite        *site,
	const AxisIndexerRequest *request,
	gchar                   **problem
);

void
axis_def_search_free(AxisDefSearch *search);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDefSearch, axis_def_search_free)

/* How the search was made (by an IMDb identifier, or words). */
AxisIndexerMode
axis_def_search_mode(const AxisDefSearch *search);

/* Whether the search has something to ask by: words (after the definition's
 * keyword filters), or an IMDb identifier the definition searches by.  A
 * search without either is not one a site can answer for this request: a
 * definition's path would fall back to its newest listing (what a test
 * asks for), which is not this title's releases. */
gboolean
axis_def_search_has_words(const AxisDefSearch *search);

guint
axis_def_search_n_requests(const AxisDefSearch *search);

const AxisDefRequest *
axis_def_search_request(
	const AxisDefSearch *search,
	guint                index
);

/**
 * axis_def_search_set_trace:
 * @trace: (nullable): where every step is recorded (borrowed: it must
 *   outlive the search)
 *
 * Records the definition, the words and the requests in @trace now, and
 * each answer read from here on.
 */
void
axis_def_search_set_trace(
	AxisDefSearch *search,
	AxisDefTrace  *trace
);

/**
 * AxisDefVerdict:
 * @AXIS_DEF_ANSWER_READ: read: its releases (perhaps none) were added
 * @AXIS_DEF_ANSWER_REFUSED: it cannot be read, went past a limit, or the
 *   site says it is an error (its error selector matched)
 * @AXIS_DEF_ANSWER_CHALLENGE: a browser challenge's page
 * @AXIS_DEF_ANSWER_DRIFT: its pages no longer look as expected
 *
 * What came of reading one answer.
 */
typedef enum
{
	AXIS_DEF_ANSWER_READ,
	AXIS_DEF_ANSWER_REFUSED,
	AXIS_DEF_ANSWER_CHALLENGE,
	AXIS_DEF_ANSWER_DRIFT
} AxisDefVerdict;

/**
 * AxisDefAnswerInfo:
 * @verdict: what came of it
 * @rows_matched: the rows the row selector matched
 * @kept: of them, the releases kept
 * @lost: lost for a field every result must have
 * @not_video: left out because their category is not video
 * @answer: (nullable): the trace's record of this answer, when the search
 *   has a trace with room for it: where the caller puts what it knows of
 *   the answer (its status), and only there
 */
typedef struct
{
	AxisDefVerdict      verdict;
	guint               rows_matched;
	guint               kept;
	guint               lost;
	guint               not_video;
	AxisDefTraceAnswer *answer;
} AxisDefAnswerInfo;

/**
 * axis_def_search_read_full:
 * @index: the request the answer is for
 * @body: the answer as it came (the site's encoding)
 * @seen: (nullable): merge keys already in @outcome (strings), added to
 * @outcome: (inout): initialised by the caller; releases are added
 * @info: (out) (optional): what came of it
 * @problem: (out) (optional) (transfer full): why the answer was refused,
 *   safe to show: for drift it begins with %AXIS_DEF_DRIFT_PROBLEM, for a
 *   challenge it is %AXIS_DEF_CHALLENGE_PROBLEM
 *
 * Returns: %TRUE when the answer was read (@info's verdict
 *   %AXIS_DEF_ANSWER_READ); else nothing is added
 */
gboolean
axis_def_search_read_full(
	AxisDefSearch       *search,
	guint                index,
	const gchar         *body,
	gsize                length,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisDefAnswerInfo   *info,
	gchar              **problem
);

/* As axis_def_search_read_full(), without @info. */
gboolean
axis_def_search_read(
	AxisDefSearch       *search,
	guint                index,
	const gchar         *body,
	gsize                length,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	gchar              **problem
);

/* Why an answer that is a browser challenge is refused. */
#define AXIS_DEF_CHALLENGE_PROBLEM "the site answered with a browser challenge instead of its page, which Axis " \
	"cannot pass: it is not usable from here, it needs a browser helper (a later stage)"

/* How the reason for drift begins. */
#define AXIS_DEF_DRIFT_PROBLEM "its pages no longer look as expected"

/* Whether @text is a browser challenge's page (Cloudflare's, DDoS-Guard's)
 * rather than the site's: what axis_def_search_read() refuses with
 * %AXIS_DEF_CHALLENGE_PROBLEM. */
gboolean
axis_def_is_challenge(const gchar *text);

/* Whether a release's address is the page its download is read from
 * (download.selectors or download.infohash), not the file. */
gboolean
axis_def_download_is_page(const AxisDef *def);

/**
 * axis_def_read_download:
 * @page_path: the page's path on the origin (the release's fetch address)
 * @body: the page as it came
 * @magnet: (out) (transfer full) (nullable): the magnet link found
 * @file_path: (out) (transfer full) (nullable): or the path of the file on
 *   the origin
 * @problem: (out) (optional) (transfer full)
 *
 * Reads the download from the page the way the definition says: the first
 * selector that gives a magnet link or an address on the origin, or the
 * info-hash and title.  An address elsewhere is never given.
 */
gboolean
axis_def_read_download(
	const AxisDefSite  *site,
	const gchar        *page_path,
	const gchar        *body,
	gsize               length,
	gchar             **magnet,
	gchar             **file_path,
	gchar             **problem
);

G_END_DECLS

#endif /* AXIS_DEF_ENGINE_H */
