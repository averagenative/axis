/*
 * axis-indexer-kind.h - what a kind of indexer source is: a table of
 * functions, and the connection it is given
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A kind (Prowlarr; the tests' fixture is the same kind against a fake) is a
 * static AxisIndexerKindInfo in a file of its own that speaks its source's
 * API.  It never makes a request by itself and never sees an address or a
 * key: the layer gives each function an AxisIndexerConn to one configured
 * source, and the kind asks it for GETs of paths on that source's origin
 * (core/axis-http.h: the same fixed origin, redirects refused, bounded
 * answers and deadlines; a release's file alone may be behind one redirect
 * to the indexer's own site, axis_indexer_conn_fetch_release()).  Only GET:
 * a read session is all this layer has,
 * and the HTTP client's own rule that nothing else is sent holds.  The
 * source's key is placed by the connection, in the header the kind names
 * (%AxisIndexerKindInfo.key_header), on every request and nowhere else;
 * never in a path.
 *
 * A native indexer's kind (Newznab, Torznab: %AxisIndexerKindInfo.native)
 * speaks to one indexer, not to a source of many, and its protocol puts the
 * key in the query: the kind names the parameter
 * (%AxisIndexerKindInfo.key_param, "apikey") and the connection adds it to
 * every request axis_indexer_conn_get() makes, never to a release's fetch
 * reference (which carries whatever the indexer wrote in it).  The HTTP
 * client blanks the key and every query out of its messages.  What such a
 * kind learns on the way (the capabilities, the allowances an answer states,
 * an origin of files it saw, that the indexer said its limit is reached) it
 * tells the connection (axis_indexer_conn_learn_caps() and the others), and
 * the layer stores it after the call.
 *
 * A request waits for the source's budget first (a token of its limiter:
 * rate and burst, a pause after a 429, a back-off after failures), and at
 * most a few calls to a source run at once.
 *
 * What a kind returns is checked by the layer before it is stored: a
 * release's fetch reference must still be a path under the source's base
 * path, and names, counts and lists are cut again.
 *
 * Errors: %AXIS_INDEXER_ERROR_AUTH when the source refused the key,
 * %AXIS_INDEXER_ERROR_UNREACHABLE when it could not be reached,
 * %AXIS_INDEXER_ERROR_RATE_LIMITED when it asked to be left alone (or the
 * budget leaves no room), %AXIS_INDEXER_ERROR_UNREADABLE when its answer
 * could not be read, %AXIS_INDEXER_ERROR_REFUSED when what it returned is not
 * accepted (not an NZB or a torrent); what a connection function reported
 * is passed on as it is.  Messages never hold the key or a fetch reference
 * (the connection's are scrubbed; a kind writes its own from fixed text).
 *
 * Every function runs on a worker thread and may block on the network.
 */

#ifndef AXIS_INDEXER_KIND_H
#define AXIS_INDEXER_KIND_H

#include <gio/gio.h>
#include <glib.h>

#include "core/axis-http-origin.h"
#include "indexer/axis-indexer-model.h"

G_BEGIN_DECLS

#define AXIS_INDEXER_ERROR (axis_indexer_error_quark())

/**
 * AxisIndexerError:
 * @AXIS_INDEXER_ERROR_INVALID: the request is not one that can be made
 * @AXIS_INDEXER_ERROR_NOT_FOUND: no such source, or no such release (or its
 *   search has expired)
 * @AXIS_INDEXER_ERROR_NO_SOURCE: no enabled source to ask
 * @AXIS_INDEXER_ERROR_EXISTS: a source of that name exists
 * @AXIS_INDEXER_ERROR_AUTH: the source refused the key
 * @AXIS_INDEXER_ERROR_UNREACHABLE: the source could not be reached or did
 *   not answer in time
 * @AXIS_INDEXER_ERROR_RATE_LIMITED: the source asked Axis to wait, or the
 *   limit leaves no room in time
 * @AXIS_INDEXER_ERROR_BUSY: as many calls are running at the source as are
 *   allowed, and none ended in time
 * @AXIS_INDEXER_ERROR_UNREADABLE: the source's answer could not be read
 * @AXIS_INDEXER_ERROR_REFUSED: what the source returned is not accepted
 * @AXIS_INDEXER_ERROR_TOO_LARGE: an answer larger than the limit
 * @AXIS_INDEXER_ERROR_REDIRECT: the source redirected somewhere else; not
 *   followed
 * @AXIS_INDEXER_ERROR_CANCELLED: the daemon or the job is stopping
 * @AXIS_INDEXER_ERROR_FAILED: anything else
 */
typedef enum
{
	AXIS_INDEXER_ERROR_INVALID,
	AXIS_INDEXER_ERROR_NOT_FOUND,
	AXIS_INDEXER_ERROR_NO_SOURCE,
	AXIS_INDEXER_ERROR_EXISTS,
	AXIS_INDEXER_ERROR_AUTH,
	AXIS_INDEXER_ERROR_UNREACHABLE,
	AXIS_INDEXER_ERROR_RATE_LIMITED,
	AXIS_INDEXER_ERROR_BUSY,
	AXIS_INDEXER_ERROR_UNREADABLE,
	AXIS_INDEXER_ERROR_REFUSED,
	AXIS_INDEXER_ERROR_TOO_LARGE,
	AXIS_INDEXER_ERROR_REDIRECT,
	AXIS_INDEXER_ERROR_CANCELLED,
	AXIS_INDEXER_ERROR_FAILED
} AxisIndexerError;

GQuark
axis_indexer_error_quark(void);

/* One configured source, as a kind's function talks to it. */
typedef struct _AxisIndexerConn AxisIndexerConn;

/**
 * AxisIndexerGetOptions:
 * @accept: (nullable): the Accept header; %NULL is "application/json"
 * @max_bytes: the largest body accepted, 0 for the default (%AXIS_INDEXER_MAX_ANSWER_BYTES)
 * @slow: the call may take long (a search across every indexer, a file):
 *   it gets the longer deadline
 * @files_origin: a release's fetch only: ask the other origin an
 *   administrator accepted for the native indexer's files, not its own (it
 *   fails with %AXIS_INDEXER_ERROR_REFUSED, sending nothing, when none is
 *   accepted now)
 * @timeout_ms: the deadline, when not 0 (else the one @slow gives)
 * @headers: (nullable) (array zero-terminated=1): further request headers,
 *   names and values alternately, sent to the source's origin only (a
 *   definition's Referer or Cookie); the kind checks them
 */
typedef struct
{
	const gchar        *accept;
	gsize               max_bytes;
	gboolean            slow;
	gboolean            files_origin;
	guint               timeout_ms;
	const gchar *const *headers;
} AxisIndexerGetOptions;

/* The largest answer read from a source unless a kind asks for more. */
#define AXIS_INDEXER_MAX_ANSWER_BYTES   (16 * 1024 * 1024)

/* A native indexer's request's deadline: one indexer's search. */
#define AXIS_INDEXER_NATIVE_TIMEOUT_MS  30000

/**
 * AxisIndexerReply:
 * @status: the status of the last answer (0 for none)
 * @magnet: (nullable): for a redirect that was not followed (the call failed
 *   with %AXIS_INDEXER_ERROR_REDIRECT) whose Location is a "magnet:" link,
 *   that text as the source wrote it: untrusted, to be checked with
 *   axis_download_magnet_parse() and never requested
 * @refusal: (nullable): for a 403 from the source's origin, the first bytes
 *   of its body (core/axis-http.h): untrusted, to be compared with (a browser
 *   challenge's page) and never put in a message
 * @challenged: the source's origin answered with Cloudflare's
 *   "cf-mitigated: challenge" header (core/axis-http.h): a browser challenge,
 *   whatever its status
 */
typedef struct
{
	guint     status;
	gchar    *magnet;
	gchar    *refusal;
	gboolean  challenged;
} AxisIndexerReply;

void
axis_indexer_reply_clear(AxisIndexerReply *reply);

/**
 * axis_indexer_conn_get:
 * @path: an absolute path on the source's origin, with an optional query.
 *   A source served under a base path (set by the administrator,
 *   "/prowlarr") is asked for that prefix and this path: the kind names its
 *   own path ("/api/v1/search") and never sees the prefix.  A search's words
 *   go in the query, percent-encoded; the key never does
 * @options: (nullable): see #AxisIndexerGetOptions
 * @body: (out) (transfer full): the body of a 2xx answer
 * @reply: (out) (optional): what else the source said; cleared first
 *
 * Fails with %AXIS_INDEXER_ERROR_AUTH for a 401 or 403,
 * %AXIS_INDEXER_ERROR_UNREACHABLE when the source could not be reached or did
 * not answer in time, %AXIS_INDEXER_ERROR_RATE_LIMITED for a 429 (the source
 * is not asked again until the time it states) or when the budget leaves no
 * room, %AXIS_INDEXER_ERROR_TOO_LARGE for an answer beyond the limit,
 * %AXIS_INDEXER_ERROR_REDIRECT for a redirect that leaves the origin (never
 * followed; one within it is), %AXIS_INDEXER_ERROR_CANCELLED, and
 * %AXIS_INDEXER_ERROR_REFUSED or %AXIS_INDEXER_ERROR_FAILED for any other
 * status.
 *
 * Returns: %TRUE on a 2xx answer
 */
gboolean
axis_indexer_conn_get(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
);

/**
 * axis_indexer_conn_get_ref:
 * @path: a release's fetch reference: a path and query as the source gave it,
 *   already under its base path
 *
 * As axis_indexer_conn_get(), for a path that already carries the base path:
 * refused with %AXIS_INDEXER_ERROR_INVALID, sending nothing, unless
 * axis_indexer_path_ok() accepts it for the source's base path.  The path may
 * hold the source's key: errors never show it.
 */
gboolean
axis_indexer_conn_get_ref(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
);

/**
 * axis_indexer_conn_fetch_release:
 * @path: a release's fetch reference, as for axis_indexer_conn_get_ref()
 *
 * As axis_indexer_conn_get_ref(), for the fetch of a release's file and
 * nothing else.  When the source redirects the download to the indexer's own
 * site, that one redirect is followed (the HTTP client's release fetch,
 * core/axis-http.h: https, a public address checked where it is connected
 * to, nothing of the source's sent there) unless following is
 * switched off for the source: then it fails with
 * %AXIS_INDEXER_ERROR_REDIRECT, nothing sent there, and a message that names
 * the two ways out.  What goes wrong at the indexer's site fails with
 * %AXIS_INDEXER_ERROR_REFUSED (or _TOO_LARGE, _CANCELLED, _REDIRECT for a
 * second redirect or an address that is not public) and is not held against
 * the source.  A redirect to a "magnet:" link, from either, is reported in
 * @reply and never followed.
 */
gboolean
axis_indexer_conn_fetch_release(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
);

/* The indexers the source reported (element-type AxisIndexerInfo), as the
 * layer knows them now: possibly empty, never %NULL. */
const GPtrArray *
axis_indexer_conn_get_indexers(AxisIndexerConn *conn);

/* The source's configured origin, and its base path ("" for none). */
const AxisHttpOrigin *
axis_indexer_conn_get_origin(AxisIndexerConn *conn);

const gchar *
axis_indexer_conn_get_base_path(AxisIndexerConn *conn);

/* What messages call the source: its configured name. */
const gchar *
axis_indexer_conn_get_label(AxisIndexerConn *conn);

/* For a kind whose indexers each run an installed definition
 * (%AxisIndexerKindInfo.definition): the definition's file as it was
 * installed, valid for the call; %NULL for any other. */
const gchar *
axis_indexer_conn_get_definition(AxisIndexerConn *conn);

/**
 * AxisIndexerNative:
 * @protocol: the indexer's
 * @caps: its capabilities as last read (@caps->read_us 0: never), or as
 *   learned in this call
 * @movie_categories, @n_movie_categories: the categories searched for a
 *   movie (the administrator's)
 * @tv_categories, @n_tv_categories: and for TV
 * @files_origin: (nullable): the other origin accepted for its files
 * @priority: its priority (lower first)
 *
 * What a native indexer's kind knows of the indexer it speaks to.
 */
typedef struct
{
	AxisDownloadProtocol   protocol;
	const AxisIndexerCaps *caps;
	const gint            *movie_categories;
	guint                  n_movie_categories;
	const gint            *tv_categories;
	guint                  n_tv_categories;
	const AxisHttpOrigin  *files_origin;
	gint                   priority;
} AxisIndexerNative;

/* For a native indexer's kind: what is known of it; %NULL for a source. */
const AxisIndexerNative *
axis_indexer_conn_get_native(AxisIndexerConn *conn);

/* The capabilities just read (copied; their @read_us is set now), or why
 * they could not be (@caps->problem). */
void
axis_indexer_conn_learn_caps(
	AxisIndexerConn       *conn,
	const AxisIndexerCaps *caps
);

/* The allowances an answer stated (copied, as of now). */
void
axis_indexer_conn_learn_allowance(
	AxisIndexerConn            *conn,
	const AxisIndexerAllowance *allowance
);

/* An origin of files a test's answer named that is not the indexer's own
 * nor the accepted one ("https://host:port"): shown, never used. */
void
axis_indexer_conn_learn_other_origin(
	AxisIndexerConn *conn,
	const gchar     *origin
);

/**
 * axis_indexer_conn_hold:
 * @seconds: how long not to ask
 * @grabs: only files are held (the download limit), not searches
 * @message: why, safe to show
 *
 * The indexer said its limit is reached: its searches (or its files) are
 * not asked for until then, and the page says so.
 */
void
axis_indexer_conn_hold(
	AxisIndexerConn *conn,
	gint64           seconds,
	gboolean         grabs,
	const gchar     *message
);

/**
 * axis_indexer_conn_hold_searches:
 * @seconds: how long its searches pass it over
 * @message: why, safe to show ("... its pages no longer look as expected")
 *
 * For a native indexer that answered with something it should not (a page
 * that is not its site's): searches pass it over until then, saying
 * @message, and the page says so.  Unlike axis_indexer_conn_hold(), the
 * limiter is not paused: a test or a check still reaches the site, and one
 * that passes lifts the hold.
 */
void
axis_indexer_conn_hold_searches(
	AxisIndexerConn *conn,
	gint64           seconds,
	const gchar     *message
);

/* The trace a check asked for (indexer/def/axis-def-trace.h), or %NULL. */
struct _AxisDefTrace *
axis_indexer_conn_get_trace(AxisIndexerConn *conn);

/**
 * axis_indexer_conn_get_once:
 * @origin: the site's origin
 * @path: (nullable): a path on it, with its query
 * @headers: (nullable): name and value pairs, %NULL-terminated
 * @accept: (nullable): the Accept header
 * @max_bytes: the largest answer taken
 * @body: (out): the answer, on success
 * @status: (out): the HTTP status, 0 when nothing answered
 * @refusal: (out) (nullable): the first bytes of a 403's body, or %NULL
 * @challenged: (out) (optional): the answer said it is a browser challenge
 *   ("cf-mitigated: challenge")
 *
 * One GET to @origin, for `axisd --check-definition`, which has no source,
 * no key and no limiter (it paces its own requests): no key is sent, no
 * redirect is followed, the answer is bounded, the time is a native
 * indexer's, and it goes out as the daemon's indexer traffic does
 * (outbound.bind).  Every request of the indexer layer is made in
 * axis-indexer-conn.c; this is the one that belongs to no source.
 */
gboolean
axis_indexer_conn_get_once(
	const AxisHttpOrigin  *origin,
	const gchar           *path,
	const gchar *const    *headers,
	const gchar           *accept,
	gsize                  max_bytes,
	GBytes               **body,
	guint                 *status,
	gchar                **refusal,
	gboolean              *challenged,
	GError               **error
);

/* @text from the indexer (an error's description) cleaned, cut to
 * @max_chars, and with the key and anything that looks like a query blanked
 * out: safe to show. */
gchar *
axis_indexer_conn_scrub(
	AxisIndexerConn *conn,
	const gchar     *text,
	guint            max_chars
);

typedef struct _AxisIndexerKindInfo AxisIndexerKindInfo;

/**
 * AxisIndexerKindInfo:
 * @name: 1 to 16 lower-case letters and digits, as stored
 * @label: its name for people ("Prowlarr")
 * @secret_label: what its key is called ("API key")
 * @address_help: (nullable): the form's words about its address
 * @key_header: the request header that carries the key ("X-Api-Key")
 * @test: reaches the source, authenticates, and says what it is (its
 *   version)
 * @indexers: the indexers the source reports, with whether each is failing
 *   now (at most %AXIS_INDEXER_MAX_INDEXERS)
 * @search: searches for @request: by an identifier where the source takes
 *   one, by words otherwise, and says which (the outcome's mode, and whether
 *   words were used because the identifier found nothing); results whose
 *   fetch address is not the source's own are left out and counted
 * @fetch: fetches a release's file (an NZB or a torrent, checked by its
 *   bytes) or its magnet link
 * @data: the kind's own, for its functions
 * @native: the kind speaks to one indexer (Newznab, Torznab), not to a
 *   source of many: its @indexers reports the indexer itself
 * @protocol: a native kind's protocol
 * @key_param: (nullable): the query parameter that carries the key ("apikey")
 *   instead of a header; @key_header is then %NULL
 * @caps: (nullable): a native kind's reading of the indexer's capabilities
 *   alone (the scheduled refresh); it tells the connection what it read
 * @site: a native kind for a public site with an API of its own (YTS, EZTV,
 *   The Pirate Bay: indexer/axis-indexer-sites.h): it takes no key (@key_header
 *   and @key_param are both %NULL), is asked no faster than the setting
 *   %AXIS_INDEXER_SETTING_SITE_RATE says, and has a @default_address
 * @default_address: (nullable): a site kind's address when none is typed, shown
 *   by the pages (a site moves between domains: it is a setting, never found
 *   out by Axis)
 * @covers: (nullable): a site kind's own words for what it can be asked
 *   ("Movies, by IMDb identifier or title"), shown by the pages
 * @definition: a site kind whose indexers each run an installed definition
 *   (indexer/axis-indexer-cardigann.h): it has no @default_address (each
 *   indexer's address is one of its definition's links, or one typed), its
 *   indexers keep the base path of that address, and the connection gives
 *   the definition (axis_indexer_conn_get_definition())
 *
 * A kind of the indexer layer.  The fields after @data were added for
 * native indexers and sites: a source kind leaves them zero.
 */
struct _AxisIndexerKindInfo
{
	const gchar *name;
	const gchar *label;
	const gchar *secret_label;
	const gchar *address_help;
	const gchar *key_header;

	gboolean (*test)(const AxisIndexerKindInfo *self, AxisIndexerConn *conn, gchar **version, GError **error);
	gboolean (*indexers)(const AxisIndexerKindInfo *self, AxisIndexerConn *conn, GPtrArray **indexers,
	                     GError **error);
	gboolean (*search)(const AxisIndexerKindInfo *self, AxisIndexerConn *conn, const AxisIndexerRequest *request,
	                   AxisIndexerOutcome *outcome, GError **error);
	gboolean (*fetch)(const AxisIndexerKindInfo *self, AxisIndexerConn *conn, const AxisIndexerFetchRequest *request,
	                  AxisIndexerFetched *fetched, GError **error);

	gpointer data;

	gboolean              native;
	AxisDownloadProtocol  protocol;
	const gchar          *key_param;
	gboolean (*caps)(const AxisIndexerKindInfo *self, AxisIndexerConn *conn, GError **error);

	gboolean              site;
	const gchar          *default_address;
	const gchar          *covers;

	gboolean              definition;
};

G_END_DECLS

#endif /* AXIS_INDEXER_KIND_H */
