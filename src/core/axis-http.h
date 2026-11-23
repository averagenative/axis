/*
 * axis-http.h - the daemon's only outbound HTTP client: constrained
 * requests to one fixed origin at a time
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every outbound connection the daemon makes goes through here (the media
 * servers of compat/, the metadata providers of meta/, the download clients
 * of download/, and later indexers), so its rules are stated and tested
 * once.  It is built to be boring and to fail closed:
 *
 *  - Requests go only to the configured origin (scheme, host and port,
 *    core/axis-http-origin.h).  The secret is a request header chosen by
 *    the caller (each kind of server has its own), never part of a URL, and
 *    goes only to that origin.  The one exception is a native indexer's key
 *    (Newznab, Torznab), which that protocol takes in the query: the indexer
 *    layer's connection puts it there (indexer/axis-indexer-kind.h), and here
 *    it is still @token, blanked out of every message as every query is.
 *  - Only GET is sent, with one exception: a connection made for a
 *    configured download client (axis_http_session_new() with
 *    %AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT) may also POST a body to its own
 *    origin, because the download clients' APIs need it.  axis_http_get()
 *    and a session of any other purpose cannot send anything but GET:
 *    asked to POST, such a session fails without sending anything.
 *    The second exception is one pinned request to a media server, declared
 *    below as data (%AXIS_HTTP_RESCAN_METHOD, %AXIS_HTTP_RESCAN_PATH):
 *    axis_http_post_rescan() asks Jellyfin to rescan folders, with a body
 *    this file builds from a fixed shape.  Nothing else can be written to a
 *    media server.  The third is one pinned request to a metadata provider
 *    (%AXIS_HTTP_LOGIN_METHOD, %AXIS_HTTP_LOGIN_PATH): axis_http_post_login()
 *    exchanges TheTVDB's key (and PIN) for a token, with a body this file
 *    builds from those two values alone.
 *  - A redirect is followed only to the same scheme, host and port, at most
 *    %AXIS_HTTP_MAX_REDIRECTS times.  Any other redirect fails the request
 *    without a request to the other place: the secret is not sent there.
 *    A POST is never followed on any redirect, not even to its own origin:
 *    its body is sent once, to the address it was meant for.  (libsoup's
 *    own redirect handling is switched off: each hop is checked here.)
 *    The one exception is axis_http_fetch_release(), below: an indexer's
 *    release file, behind one redirect to the indexer's own site.
 *  - The body is read in pieces and refused beyond a size cap, and the
 *    whole exchange (connecting, the head, the body) has a deadline, so
 *    that a response that is too large, never ends or arrives a byte at a
 *    time ends the request with an error.
 *  - TLS certificates are verified, with an explicit option to accept an
 *    untrusted (self-signed) one.
 *  - Proxy environment variables are ignored; the response is not
 *    decompressed.  No cookies are kept, except by a download client's
 *    session: there, in memory only, the cookies its origin sets
 *    (core/axis-http-cookie.h), sent back to that origin only.
 *  - Every error message is written here from fixed text; what the library
 *    adds (a socket error) is cleaned, and the secret is blanked out of it
 *    if it ever appears, as are a session's cookies and further secrets, a
 *    request's query string and anything that looks like one.  Neither a
 *    request body nor a response body is ever put in a message.
 *  - When outbound.bind names an interface (core/axis-outbound.h), each
 *    TCP connection the binding applies to (by @traffic and the address
 *    connected to) is tied to that interface and its address as it is made,
 *    or not made at all when the interface is missing, down or without an
 *    address: the request then fails with %AXIS_HTTP_ERROR_CONNECT, saying
 *    so, and nothing was sent anywhere.  Every request has a libsoup
 *    session of its own, so no connection outlives the request it was made
 *    for: there is no pool of connections bound under an older state.
 *
 * What a request carries: the path and query the caller gives, Host, the
 * fixed user agent, Accept, the secret's header (when there is one), the
 * caller's own extra headers, and for a download client's session its
 * cookies and the POST body with its content type; nothing else.
 *
 * Blocking: call from a worker thread (a job).  A private main context is
 * used, so the caller's loop is not touched.
 */

#ifndef AXIS_HTTP_H
#define AXIS_HTTP_H

#include <gio/gio.h>
#include <glib.h>

#include "core/axis-http-origin.h"
#include "core/axis-outbound.h"

G_BEGIN_DECLS

#define AXIS_HTTP_MAX_REDIRECTS      3
#define AXIS_HTTP_DEFAULT_TIMEOUT_MS 15000
#define AXIS_HTTP_DEFAULT_MAX_BYTES  (4 * 1024 * 1024)

/* The largest body a POST may carry (a release file, with the encoding a
 * client's API wraps it in). */
#define AXIS_HTTP_MAX_REQUEST_BYTES  (96 * 1024 * 1024)

/* A Location header longer than this is not followed and not reported. */
#define AXIS_HTTP_MAX_LOCATION_BYTES 4096

/* A Retry-After longer than this is taken as this. */
#define AXIS_HTTP_MAX_RETRY_AFTER_S  86400

#define AXIS_HTTP_ERROR (axis_http_error_quark())

typedef enum
{
	AXIS_HTTP_ERROR_CONFIG,		/* no address, bad path */
	AXIS_HTTP_ERROR_CONNECT,	/* could not reach the server */
	AXIS_HTTP_ERROR_TLS,		/* the certificate was not accepted */
	AXIS_HTTP_ERROR_STATUS,		/* a status other than 2xx */
	AXIS_HTTP_ERROR_REDIRECT,	/* to somewhere else, or too many */
	AXIS_HTTP_ERROR_TOO_LARGE,
	AXIS_HTTP_ERROR_TIMEOUT,
	AXIS_HTTP_ERROR_CANCELLED,
	AXIS_HTTP_ERROR_FAILED
} AxisHttpError;

GQuark
axis_http_error_quark(void);

/**
 * AxisHttpConfig:
 * @origin: where the requests go; borrowed
 * @auth_header: (nullable): the name of the request header that carries the
 *   secret ("X-Plex-Token", "Authorization"); borrowed
 * @auth_value: (nullable): that header's value (the secret, or the secret
 *   inside the server's own syntax); borrowed.  The caller makes it from the
 *   settings of the same server @origin belongs to: it is sent to @origin
 *   and nowhere else, so one server's secret cannot reach another's address
 * @token: (nullable): the secret itself, blanked out of every message the
 *   client writes; borrowed
 * @accept_untrusted_certificate: accept a certificate that does not verify
 * @timeout_ms: the deadline of one request, 0 for the default
 * @max_bytes: the largest body accepted, 0 for the default
 * @cancellable: (nullable): cancels the request (a job being cancelled)
 * @peer: (nullable): what the messages call the server, such as "the
 *   metadata provider"; %NULL is "the media server"
 * @secret_name: (nullable): what the messages call the secret, such as
 *   "key"; %NULL is "token"
 * @user_agent: (nullable): the fixed User-Agent; %NULL is "axisd"
 * @accept: (nullable): the Accept header; %NULL is "application/json"
 * @traffic: what the connection is for, which decides whether outbound.bind
 *   applies to it (core/axis-outbound.h); 0 is
 *   %AXIS_OUTBOUND_TRAFFIC_UNNAMED, which is bound as an indexer's is, so a
 *   config that forgets to say is bound rather than not (a test reads the
 *   sources to check that every config says)
 *
 * Fields added later are at the end: a config made with memset() to zero
 * and the fields it needs behaves as before.
 */
typedef struct
{
	const AxisHttpOrigin *origin;
	const gchar          *auth_header;
	const gchar          *auth_value;
	const gchar          *token;
	gboolean              accept_untrusted_certificate;
	guint                 timeout_ms;
	gsize                 max_bytes;
	GCancellable         *cancellable;
	const gchar          *peer;
	const gchar          *secret_name;
	const gchar          *user_agent;
	const gchar          *accept;
	AxisOutboundTraffic   traffic;
} AxisHttpConfig;

/**
 * AxisHttpResponse:
 * @status: the status of the last answer received, 0 if none was
 * @retry_after_s: for an answer other than 2xx that carried Retry-After,
 *   the seconds to wait from now (0 to %AXIS_HTTP_MAX_RETRY_AFTER_S);
 *   -1 when it carried none
 * @ratelimit_wait_s: for an answer other than 2xx whose X-RateLimit-Remaining
 *   is 0 (or absent) and that carried X-RateLimit-Reset, the seconds until
 *   then (0 to %AXIS_HTTP_MAX_RETRY_AFTER_S); -1 when it carried none.  For a
 *   caller that has no Retry-After to go by (GitHub's API gives none for an
 *   ordinary refusal)
 * @content_type: (nullable): of a 2xx answer, its media type in lower case
 *   without parameters ("image/jpeg"), or %NULL when it stated none
 * @sent: the request's head was written to the server's connection: the
 *   server may have acted on it, whatever happened after.  %FALSE means it
 *   certainly was not (the connection or its TLS failed, or the request
 *   was refused here before it was made)
 * @location: (nullable): for a redirect that was refused because it leads
 *   to another origin (the request failed with %AXIS_HTTP_ERROR_REDIRECT),
 *   the Location header as the server wrote it, when it is plain printable
 *   ASCII of at most %AXIS_HTTP_MAX_LOCATION_BYTES bytes; otherwise %NULL.
 *   Nothing was sent to that place.  It is the server's text: untrusted,
 *   to be read (a magnet link, say) and never requested, and it is never
 *   put in an error message here
 * @offsite: the request followed its one redirect off the origin
 *   (axis_http_fetch_release() only): @status, @content_type and @location,
 *   and the failure if there was one, are the indexer's site's, not the
 *   configured server's
 * @refusal: (nullable): of a 403 answered to a download client's request or
 *   to an indexer's (traffic %AXIS_OUTBOUND_TRAFFIC_INDEXER, without a
 *   session), the first %AXIS_HTTP_REFUSAL_BYTES bytes of its body, so that a
 *   kind can tell one kind of refusal from another (SABnzbd's host-name check
 *   from a refused key; a site's browser challenge from a refusal).
 *   Untrusted text from the server: to be compared with, never put in a
 *   message.  %NULL for any other answer
 * @challenged: an answer that is not a 2xx came with Cloudflare's
 *   "cf-mitigated: challenge" header: a browser challenge in place of what
 *   was asked, whatever its status (403 or 503)
 *
 * What the server said, beside the body: for a caller that must honour a
 * request to wait (a 429), check what the server claims a body is, know
 * whether a request that failed may still have arrived, or read what a
 * redirect it was not allowed to follow pointed at.
 */
typedef struct
{
	guint     status;
	gint64    retry_after_s;
	gint64    ratelimit_wait_s;
	gchar    *content_type;
	gboolean  sent;
	gchar    *location;
	gboolean  offsite;
	gchar    *refusal;
	gboolean  challenged;
} AxisHttpResponse;

/* How much of a 403's body is kept in AxisHttpResponse.refusal. */
#define AXIS_HTTP_REFUSAL_BYTES 512

void
axis_http_response_clear(AxisHttpResponse *response);

/**
 * axis_http_get:
 * @config: the connection
 * @path: an absolute path with an optional query ("/identity",
 *   "/library/metadata/123"); never a full URL
 * @headers: (nullable): further request headers, name and value pairs
 *   ended by %NULL (the authentication and the accept header are added here)
 * @body: (out) (transfer full): the body of a 2xx answer
 * @error: (nullable): return location for an error in %AXIS_HTTP_ERROR
 *
 * Returns: %TRUE on a 2xx answer within the limits
 */
gboolean
axis_http_get(
	const AxisHttpConfig *config,
	const gchar          *path,
	const gchar *const   *headers,
	GBytes              **body,
	GError              **error
);

/**
 * axis_http_get_full:
 * @response: (out) (optional): what the server said; cleared first, and
 *   filled whether or not the request succeeds (free with
 *   axis_http_response_clear())
 *
 * As axis_http_get().
 */
gboolean
axis_http_get_full(
	const AxisHttpConfig *config,
	const gchar          *path,
	const gchar *const   *headers,
	GBytes              **body,
	AxisHttpResponse     *response,
	GError              **error
);

/**
 * axis_http_fetch_release:
 * @config: the indexer source's connection, as for axis_http_get_full()
 * @path: the release's fetch reference on the source's own origin
 * @headers: (nullable): as for axis_http_get(), sent to the source only
 * @body: (out) (transfer full): the body of a 2xx answer
 * @response: (out) (optional): as for axis_http_get_full();
 *   @response->offsite says which server answered last
 *
 * As axis_http_get_full(), with one difference, for the one request that
 * needs it: an indexer source that answers a release's download with a
 * redirect to the indexer's own site (Prowlarr's per-indexer "Redirect").
 * A redirect off the configured origin is followed once, and only:
 *
 *  - to an absolute https address with a host and no user or password, of
 *    plain printable ASCII and at most %AXIS_HTTP_MAX_LOCATION_BYTES;
 *  - to a public address: the address each connection is about to be made
 *    to is checked before it is made, and again once it is connected, and a
 *    loopback, private, shared, link-local, unique-local, site-local,
 *    multicast, unspecified, documentation, benchmarking or reserved address
 *    (IPv4, IPv6, and IPv4 inside IPv6: mapped, NAT64, 6to4) ends the request
 *    with nothing sent there.  A name is checked by what it resolves to at
 *    the moment of connecting, so a second lookup cannot differ;
 *  - as a fresh request on a connection of its own: the method GET, the
 *    path and query of that address, its Host, and the fixed user agent.
 *    Not the source's key or its header, not the caller's @headers, not the
 *    Accept header, not a cookie (none is kept), and nothing the source said;
 *  - with the certificate verified against the system's certificate
 *    authorities, whatever @config->accept_untrusted_certificate says;
 *  - within the same deadline and the same size cap, and not redirected
 *    again: a second redirect fails the request (its Location is reported,
 *    as for any refused redirect, never followed).
 *
 * The redirect's address holds the indexer's key: it is kept only for the
 * request, wiped after it, never in @response, never logged, and blanked out
 * of every message.  The body's kind (an NZB, a torrent) is the caller's to
 * check.  Only the indexer layer's release fetch calls this (a test reads
 * the sources: tests/test-http.c).
 *
 * Returns: %TRUE on a 2xx answer within the limits
 */
gboolean
axis_http_fetch_release(
	const AxisHttpConfig *config,
	const gchar          *path,
	const gchar *const   *headers,
	GBytes              **body,
	AxisHttpResponse     *response,
	GError              **error
);

/**
 * axis_http_address_is_public:
 * @address: an IPv4 or IPv6 address
 *
 * Whether a connection to @address leaves the local networks: %FALSE for
 * every range axis_http_fetch_release() refuses.
 */
gboolean
axis_http_address_is_public(GInetAddress *address);

/**
 * axis_http_set_offsite_testing:
 * @allow_loopback: let a loopback address (127.0.0.0/8, ::1, and the same
 *   inside IPv6) pass axis_http_fetch_release()'s check, and nothing else
 * @database: (nullable): the certificate authorities its followed request
 *   verifies against instead of the system's
 *
 * FOR TESTS ONLY: the stand-in for an indexer's site listens on loopback
 * with a test authority.  Nothing in the daemon calls this and no setting
 * reaches it (tests/test-http.c reads the sources to prove it).  %FALSE and
 * %NULL restore the rules.
 */
void
axis_http_set_offsite_testing(
	gboolean      allow_loopback,
	GTlsDatabase *database
);

/**
 * axis_http_parse_retry_after:
 * @value: (nullable): a Retry-After header: delta-seconds or an HTTP date
 * @now_s: the current Unix time, for a date
 *
 * Returns: the seconds to wait (0 to %AXIS_HTTP_MAX_RETRY_AFTER_S), or -1
 *   when @value is neither form
 */
gint64
axis_http_parse_retry_after(
	const gchar *value,
	gint64       now_s
);

/**
 * axis_http_parse_ratelimit_reset:
 * @value: (nullable): an X-RateLimit-Reset header: the Unix time (seconds)
 *   at which the allowance returns, as GitHub writes it
 * @now_s: the current Unix time
 *
 * Returns: the seconds to wait (0 to %AXIS_HTTP_MAX_RETRY_AFTER_S), or -1
 *   when @value is not whole digits
 */
gint64
axis_http_parse_ratelimit_reset(
	const gchar *value,
	gint64       now_s
);

/* Text safe to store and show: @text cleaned, @token blanked out. */
gchar *
axis_http_scrub(
	const gchar *text,
	const gchar *token,
	guint        max_chars
);

/* @text with every query string in it ("?" up to the next space) blanked
 * out: for text a library wrote, which might hold an address. */
gchar *
axis_http_scrub_queries(const gchar *text);

/* Whether @error is a certificate fault (bad or required certificate), as
 * opposed to a handshake that was dropped or never finished. */
gboolean
axis_http_tls_error_is_certificate(const GError *error);

/* ── The one write to a media server ────────────────────────────── */

/* Declared as data, and the only thing that lets a connection that is not a
 * download client's send anything but GET: this method to exactly this path
 * (no query), with a body this file builds (axis_http_post_rescan()). */
#define AXIS_HTTP_RESCAN_METHOD       "POST"
#define AXIS_HTTP_RESCAN_PATH         "/Library/Media/Updated"

/* At most this many folders in one request, each at most this many bytes. */
#define AXIS_HTTP_RESCAN_MAX_UPDATES  32
#define AXIS_HTTP_RESCAN_MAX_PATH_BYTES 1024

typedef enum
{
	AXIS_HTTP_RESCAN_CREATED,
	AXIS_HTTP_RESCAN_MODIFIED,
	AXIS_HTTP_RESCAN_DELETED
} AxisHttpRescanType;

typedef struct
{
	const gchar        *path;	/* a folder as the server sees it: absolute, valid UTF-8, no control characters */
	AxisHttpRescanType  type;
} AxisHttpRescanUpdate;

/**
 * axis_http_rescan_permitted:
 * @method: a request's method
 * @path: its path and query as it would be sent
 *
 * Whether a request that is not made for a download client's session may be
 * sent: only the pinned rescan request, by exact method and exact path.  The
 * same test gates every request in this file.
 */
gboolean
axis_http_rescan_permitted(
	const gchar *method,
	const gchar *path
);

/**
 * axis_http_post_rescan:
 * @config: the media server's connection, as for axis_http_get()
 * @updates: the folders and what happened to each
 * @n_updates: how many, 1 to %AXIS_HTTP_RESCAN_MAX_UPDATES
 * @response: (out) (optional): as for axis_http_get_full()
 *
 * POSTs {"Updates":[{"Path":"...","UpdateType":"Created"}, ...]} to
 * %AXIS_HTTP_RESCAN_PATH, with the secret in its header as for a read.  The
 * body is built here from @updates and nothing else; an update that is out of
 * shape (no path, a path that is not absolute or has a control character or
 * is too long, an unknown type) or too many of them fail with
 * %AXIS_HTTP_ERROR_CONFIG, sending nothing.  The answer's body is not wanted
 * (a 204 is the usual one).  A redirect of any kind fails the request with
 * %AXIS_HTTP_ERROR_REDIRECT: it is never followed, so the body is sent once,
 * to the origin it was meant for.
 *
 * Returns: %TRUE on a 2xx answer
 */
gboolean
axis_http_post_rescan(
	const AxisHttpConfig       *config,
	const AxisHttpRescanUpdate *updates,
	guint                       n_updates,
	AxisHttpResponse           *response,
	GError                    **error
);

/* ── The one write to a metadata provider ───────────────────────── */

/* Declared as data, as the rescan is: this method to exactly this path (no
 * query), with a body this file builds (axis_http_post_login()).  TheTVDB's
 * API version 4 logs in there (add-metadata-tvdb). */
#define AXIS_HTTP_LOGIN_METHOD        "POST"
#define AXIS_HTTP_LOGIN_PATH          "/v4/login"

/* The longest key and PIN a login body carries. */
#define AXIS_HTTP_LOGIN_MAX_SECRET_BYTES 255

/**
 * axis_http_login_permitted:
 *
 * Whether a request that is not a download client's may be sent as the
 * pinned login: exactly %AXIS_HTTP_LOGIN_METHOD to %AXIS_HTTP_LOGIN_PATH.
 */
gboolean
axis_http_login_permitted(
	const gchar *method,
	const gchar *path
);

/**
 * axis_http_post_login:
 * @config: the provider's connection, as for axis_http_get(), without an
 *   @auth_header (the login carries no credential header); @token should be
 *   @key, so that it is blanked out of every message
 * @key: the API key: 1 to %AXIS_HTTP_LOGIN_MAX_SECRET_BYTES printable ASCII
 *   characters, without a space, a quote or a backslash
 * @pin: (nullable): the subscriber PIN, of the same form; %NULL or "" sends
 *   none (the field is left out, as the provider asks)
 * @body: (out) (transfer full): the body of a 2xx answer (the token in it)
 * @response: (out) (optional): as for axis_http_get_full()
 *
 * POSTs {"apikey":"...","pin":"..."} to %AXIS_HTTP_LOGIN_PATH.  The body is
 * built here from @key and @pin and nothing else; a key or PIN out of shape
 * fails with %AXIS_HTTP_ERROR_CONFIG, sending nothing.  Both are blanked out
 * of every message.  A redirect of any kind fails the request with
 * %AXIS_HTTP_ERROR_REDIRECT: the body is sent once, to @config's origin.
 *
 * Returns: %TRUE on a 2xx answer within the limits
 */
gboolean
axis_http_post_login(
	const AxisHttpConfig  *config,
	const gchar           *key,
	const gchar           *pin,
	GBytes               **body,
	AxisHttpResponse      *response,
	GError               **error
);

/* ── Sessions ───────────────────────────────────────────────────── */

/**
 * AxisHttpPurpose:
 * @AXIS_HTTP_PURPOSE_READ: a media server or a metadata provider: GET only,
 *   no cookies, exactly as axis_http_get()
 * @AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT: a download client an administrator
 *   configured: GET and POST to its own origin, its session cookies kept in
 *   memory.  Only download/ makes such a session (a test checks that no
 *   other part of the daemon names this purpose).
 *
 * What a connection is for, which decides what it may send.
 */
typedef enum
{
	AXIS_HTTP_PURPOSE_READ = 0,
	AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT
} AxisHttpPurpose;

typedef struct _AxisHttpSession AxisHttpSession;

/**
 * axis_http_session_new:
 * @config: the connection; copied (its strings too), so it need not
 *   outlive the session; the cancellable is referenced
 * @purpose: what it is for
 *
 * A connection to @config's origin for several requests, used by one
 * thread at a time.  Every request is made as axis_http_get() makes it.
 *
 * Returns: (transfer full): the session; free it with
 *   axis_http_session_free(), which wipes its secrets and cookies
 */
AxisHttpSession *
axis_http_session_new(
	const AxisHttpConfig *config,
	AxisHttpPurpose       purpose
);

void
axis_http_session_free(AxisHttpSession *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisHttpSession, axis_http_session_free)

AxisHttpPurpose
axis_http_session_get_purpose(AxisHttpSession *self);

/* Further text blanked out of every message the session writes (a
 * password sent in a body, say); copied, wiped when the session is freed.
 * Text shorter than four bytes is not blanked. */
void
axis_http_session_add_secret(
	AxisHttpSession *self,
	const gchar     *secret
);

/* Replaces the session's cancellable (%NULL for none): a session kept
 * between jobs takes each job's. */
void
axis_http_session_set_cancellable(
	AxisHttpSession *self,
	GCancellable    *cancellable
);

/* As axis_http_get_full(), with the session's cookies. */
gboolean
axis_http_session_get(
	AxisHttpSession     *self,
	const gchar         *path,
	const gchar *const  *headers,
	GBytes             **body,
	AxisHttpResponse    *response,
	GError             **error
);

/**
 * axis_http_session_post:
 * @path: an absolute path with an optional query, as for axis_http_get()
 * @content_type: the body's media type ("application/json")
 * @request_body: the body, at most %AXIS_HTTP_MAX_REQUEST_BYTES
 * @headers: (nullable): further request headers, as for axis_http_get()
 * @body: (out) (transfer full): the body of a 2xx answer
 * @response: (out) (optional): as for axis_http_get_full()
 *
 * One POST.  Fails with %AXIS_HTTP_ERROR_CONFIG, sending nothing, unless the
 * session is a download client's (%AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT) and
 * the body is within its limit; with %AXIS_HTTP_ERROR_REDIRECT when the
 * answer is a redirect of any kind (it is never followed); otherwise as
 * axis_http_get().
 *
 * Returns: %TRUE on a 2xx answer within the limits
 */
gboolean
axis_http_session_post(
	AxisHttpSession     *self,
	const gchar         *path,
	const gchar         *content_type,
	GBytes              *request_body,
	const gchar *const  *headers,
	GBytes             **body,
	AxisHttpResponse    *response,
	GError             **error
);

/* How many cookies the session holds (never what they are). */
guint
axis_http_session_n_cookies(AxisHttpSession *self);

/* Wipes and forgets the session's cookies (a login to be made again). */
void
axis_http_session_forget_cookies(AxisHttpSession *self);

G_END_DECLS

#endif /* AXIS_HTTP_H */
