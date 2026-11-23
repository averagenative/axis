/*
 * axis-http.c - the daemon's only outbound HTTP client
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Moved from compat/axis-compat-http.c when the metadata providers became
 * its second user; its behaviour for the media servers is unchanged (their
 * configs leave the newer fields at NULL, which are the old texts).  The
 * download clients added sessions: a connection that keeps its origin's
 * cookies in memory and, for a download client only, may POST.
 */

#include "core/axis-http.h"

#include <libsoup/soup.h>
#include <string.h>
#include <sys/socket.h>

#include "core/axis-http-cookie.h"
#include "core/axis-outbound.h"
#include "library/axis-library-name.h"

G_DEFINE_QUARK(axis-http-error-quark, axis_http_error)

#define READ_CHUNK 8192

/* Strings shorter than this are not blanked out of messages. */
#define MIN_SECRET_BYTES 4

struct _AxisHttpSession
{
	AxisHttpPurpose  purpose;
	AxisHttpOrigin   origin;	/* a copy */
	AxisHttpConfig   config;	/* points at the copies here */
	gchar           *auth_header;
	gchar           *auth_value;	/* secret */
	gchar           *token;		/* secret */
	gchar           *peer;
	gchar           *secret_name;
	gchar           *user_agent;
	gchar           *accept;
	GCancellable    *cancellable;	/* owned reference, nullable */
	AxisHttpCookies *cookies;	/* download clients only */
	GPtrArray       *secrets;	/* gchar *, wiped */
};

typedef struct
{
	const AxisHttpConfig *config;
	AxisHttpSession *session;	/* nullable */
	const gchar  *method;
	const gchar  *path;		/* as the caller gave it, for scrubbing */
	const gchar  *content_type;	/* POST */
	GBytes       *request_body;	/* POST, borrowed */
	const gchar  *body_secret;	/* in the body (the login's PIN), blanked out of messages; borrowed */
	const gchar  *peer;
	const gchar  *secret_name;
	AxisHttpResponse *response;	/* nullable */
	GMainContext *context;
	GMainLoop    *loop;
	SoupSession  *soup;
	SoupMessage  *message;
	GCancellable *cancel;
	GError       *error;
	GInputStream *stream;
	GByteArray   *body;
	guint         redirects;
	gsize         max_bytes;
	guint         timeout_ms;
	gboolean      finished;
	gboolean      timed_out;
	gboolean      too_large;
	gboolean      reading_refusal;	/* the body being read is a 403's, kept in part */
	gboolean      sent;		/* a request's head was written */
	/* A release's fetch (axis_http_fetch_release()): its one redirect off
	 * the origin, to the indexer's site. */
	gboolean      offsite_allowed;
	gboolean      offsite;		/* the request now goes to the indexer's site */
	gchar        *offsite_url;	/* SECRET (the indexer's key is in it), wiped */
	SoupSession  *offsite_soup;	/* its own: nothing of the first is shared */
	gboolean      address_checked;	/* the address connected to was found public */
	/* outbound.bind (core/axis-outbound.h), for a run it may apply to. */
	guint         bound;		/* connections tied to the outbound interface */
	guint         let_through;	/* connections not refused by the binding */
	gchar        *bind_problem;	/* why a connection of one family was not made */
	gchar         buffer[READ_CHUNK];
} Run;

/* The tests' allowances for the followed redirect (axis_http_set_offsite_testing()). */
static GMutex        testing_lock;
static gboolean      testing_loopback;	/* (testing_lock) */
static GTlsDatabase *testing_database;	/* (testing_lock) owned, nullable */

/* ── Messages ───────────────────────────────────────────────────── */

static gchar *
blank(
	gchar       *text,
	const gchar *secret
){
	gchar **parts;
	gchar *joined;

	if (secret == NULL || strlen(secret) < MIN_SECRET_BYTES)
		return text;
	parts = g_strsplit(text, secret, -1);
	joined = g_strjoinv("***", parts);
	g_strfreev(parts);
	g_free(text);
	return joined;
}

gchar *
axis_http_scrub(
	const gchar *text,
	const gchar *token,
	guint        max_chars
){
	g_autofree gchar *clean = axis_library_text_clean(text != NULL ? text : "", max_chars);
	gchar **parts;
	gchar *joined;

	if (token == NULL || token[0] == '\0')
		return g_steal_pointer(&clean);
	parts = g_strsplit(clean, token, -1);
	joined = g_strjoinv("***", parts);
	g_strfreev(parts);
	return joined;
}

gchar *
axis_http_scrub_queries(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text != NULL ? text : ""; *p != '\0'; p++) {
		if (*p != '?') {
			g_string_append_c(out, *p);
			continue;
		}
		g_string_append(out, "?***");
		while (p[1] != '\0' && p[1] != ' ' && p[1] != '\t' && p[1] != '\n' && p[1] != ')' && p[1] != '"' &&
		       p[1] != '\'')
			p++;
	}
	return g_string_free(out, FALSE);
}

/* Everything a message of @run must not hold, blanked out. */
static gchar *
scrub_message(
	Run         *run,
	const gchar *text
){
	gchar *out = axis_http_scrub(text, run->config->token, 300);
	const gchar *query;
	guint i;

	out = blank(out, run->config->auth_value);
	out = blank(out, run->body_secret);
	if (run->path != NULL && (query = strchr(run->path, '?')) != NULL && query[1] != '\0')
		out = blank(out, query + 1);
	/* The indexer's address and its query, which hold its key. */
	if (run->offsite_url != NULL) {
		out = blank(out, run->offsite_url);
		if ((query = strchr(run->offsite_url, '?')) != NULL && query[1] != '\0')
			out = blank(out, query + 1);
	}
	if (run->session != NULL) {
		gchar *scrubbed;

		for (i = 0; i < run->session->secrets->len; i++)
			out = blank(out, g_ptr_array_index(run->session->secrets, i));
		if (run->session->cookies != NULL) {
			scrubbed = axis_http_cookies_scrub(run->session->cookies, out);
			g_free(out);
			out = scrubbed;
		}
	}
	return out;
}

void
axis_http_response_clear(AxisHttpResponse *response)
{
	if (response == NULL)
		return;
	g_clear_pointer(&response->content_type, g_free);
	g_clear_pointer(&response->location, g_free);
	g_clear_pointer(&response->refusal, g_free);
	response->status = 0;
	response->retry_after_s = -1;
	response->ratelimit_wait_s = -1;
	response->sent = FALSE;
	response->offsite = FALSE;
	response->challenged = FALSE;
}

static void
fail(
	Run          *run,
	AxisHttpError code,
	const gchar  *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
fail(
	Run          *run,
	AxisHttpError code,
	const gchar  *format,
	...
){
	va_list args;
	g_autofree gchar *text = NULL;
	g_autofree gchar *clean = NULL;

	if (run->finished)
		return;
	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	clean = scrub_message(run, text);
	g_set_error_literal(&run->error, AXIS_HTTP_ERROR, (gint)code, clean);
	run->finished = TRUE;
	g_cancellable_cancel(run->cancel);
	g_main_loop_quit(run->loop);
}

static void
succeed(Run *run)
{
	if (run->finished)
		return;
	run->finished = TRUE;
	g_main_loop_quit(run->loop);
}

gboolean
axis_http_tls_error_is_certificate(const GError *error)
{
	return error != NULL && error->domain == G_TLS_ERROR &&
	       (error->code == G_TLS_ERROR_BAD_CERTIFICATE || error->code == G_TLS_ERROR_CERTIFICATE_REQUIRED);
}

/* An error from the library, as ours. */
static void
fail_from(
	Run          *run,
	const GError *error
){
	g_autofree gchar *said = axis_http_scrub_queries(error->message);

	if (run->timed_out) {
		fail(run, AXIS_HTTP_ERROR_TIMEOUT, "%s did not finish answering in time", run->peer);
	} else if (run->too_large) {
		fail(run, AXIS_HTTP_ERROR_TOO_LARGE, "%s's answer is larger than the limit", run->peer);
	} else if (g_cancellable_is_cancelled(run->config->cancellable)) {
		fail(run, AXIS_HTTP_ERROR_CANCELLED, "cancelled");
	} else if (error->domain == G_TLS_ERROR && !axis_http_tls_error_is_certificate(error)) {
		/* The handshake was dropped or never finished: not a certificate fault. */
		fail(run, AXIS_HTTP_ERROR_TLS, "%s did not complete a secure connection (%s); this is a fault at the "
			"site or on the way to it, and Axis will try again", run->peer, said);
	} else if (run->offsite && error->domain == G_TLS_ERROR) {
		/* No setting accepts the indexer's certificate: it must verify. */
		fail(run, AXIS_HTTP_ERROR_TLS, "%s's certificate was not accepted (%s); it must be valid for the system's "
			"certificate authorities, and a self-signed one is never accepted there", run->peer, said);
	} else if (error->domain == G_TLS_ERROR) {
		fail(run, AXIS_HTTP_ERROR_TLS, "%s's certificate was not accepted (%s); "
			"if it is self-signed, allow that in the settings", run->peer, said);
	} else if (run->bind_problem != NULL && run->let_through == 0) {
		/* Every address of the server was of a family the outbound
		 * interface has no address of: no connection was made. */
		fail(run, AXIS_HTTP_ERROR_CONNECT, "%s cannot be reached: %s; nothing was sent", run->peer,
			run->bind_problem);
	} else {
		fail(run, AXIS_HTTP_ERROR_CONNECT, "could not get an answer from %s (%s)", run->peer, said);
	}
}

/* ── Public addresses ───────────────────────────────────────────── */

/* Whether the IPv4 address @a (four bytes, network order) is public. */
static gboolean
ipv4_is_public(const guint8 *a)
{
	static const struct
	{
		guint32 net;
		guint   bits;
	} local[] = {
		{ 0x00000000, 8 },	/* 0.0.0.0/8: this network, unspecified */
		{ 0x0a000000, 8 },	/* 10.0.0.0/8: private */
		{ 0x64400000, 10 },	/* 100.64.0.0/10: shared (carrier-grade NAT) */
		{ 0x7f000000, 8 },	/* 127.0.0.0/8: loopback */
		{ 0xa9fe0000, 16 },	/* 169.254.0.0/16: link-local */
		{ 0xac100000, 12 },	/* 172.16.0.0/12: private */
		{ 0xc0000000, 24 },	/* 192.0.0.0/24: protocol assignments */
		{ 0xc0000200, 24 },	/* 192.0.2.0/24: documentation */
		{ 0xc0586300, 24 },	/* 192.88.99.0/24: 6to4 relays */
		{ 0xc0a80000, 16 },	/* 192.168.0.0/16: private */
		{ 0xc6120000, 15 },	/* 198.18.0.0/15: benchmarking */
		{ 0xc6336400, 24 },	/* 198.51.100.0/24: documentation */
		{ 0xcb007100, 24 },	/* 203.0.113.0/24: documentation */
		{ 0xe0000000, 4 },	/* 224.0.0.0/4: multicast */
		{ 0xf0000000, 4 }	/* 240.0.0.0/4: reserved, and the broadcast address */
	};
	guint32 value = ((guint32)a[0] << 24) | ((guint32)a[1] << 16) | ((guint32)a[2] << 8) | (guint32)a[3];
	guint i;

	for (i = 0; i < G_N_ELEMENTS(local); i++) {
		guint32 mask = 0xffffffffu << (32 - local[i].bits);

		if ((value & mask) == local[i].net)
			return FALSE;
	}
	return TRUE;
}

/* Whether the IPv6 address @a (sixteen bytes) is public: global unicast
 * outside the special ranges, with an IPv4 address inside judged as one. */
static gboolean
ipv6_is_public(const guint8 *a)
{
	static const guint8 zeros[16] = { 0 };

	/* ::ffff:0:0/96, IPv4-mapped: the IPv4 address it is. */
	if (memcmp(a, zeros, 10) == 0 && a[10] == 0xff && a[11] == 0xff)
		return ipv4_is_public(a + 12);
	/* 64:ff9b::/96, NAT64: reaches the IPv4 address inside. */
	if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xff && a[3] == 0x9b && memcmp(a + 4, zeros, 8) == 0)
		return ipv4_is_public(a + 12);
	/* 2002::/16, 6to4: the IPv4 address in its second to fifth bytes. */
	if (a[0] == 0x20 && a[1] == 0x02)
		return ipv4_is_public(a + 2);
	/* 2001:db8::/32 documentation; 2001::/32 Teredo, which tunnels to an
	 * address that cannot be seen here. */
	if (a[0] == 0x20 && a[1] == 0x01 && ((a[2] == 0x0d && a[3] == 0xb8) || (a[2] == 0x00 && a[3] == 0x00)))
		return FALSE;
	/* Only global unicast, 2000::/3, is public: that leaves out ::/128
	 * (unspecified), ::1 (loopback), ::/96 (the old IPv4-compatible forms),
	 * 100::/64 (discard), fc00::/7 (unique local), fe80::/10 (link-local),
	 * fec0::/10 (site-local) and ff00::/8 (multicast). */
	return (a[0] & 0xe0) == 0x20;
}

gboolean
axis_http_address_is_public(GInetAddress *address)
{
	const guint8 *bytes;

	g_return_val_if_fail(G_IS_INET_ADDRESS(address), FALSE);

	bytes = g_inet_address_to_bytes(address);
	switch (g_inet_address_get_family(address)) {
	case G_SOCKET_FAMILY_IPV4:
		return ipv4_is_public(bytes);
	case G_SOCKET_FAMILY_IPV6:
		return ipv6_is_public(bytes);
	default:
		return FALSE;
	}
}

/* A loopback address, IPv4 or IPv6, or IPv4 loopback inside IPv6. */
static gboolean
is_loopback(GInetAddress *address)
{
	static const guint8 zeros[16] = { 0 };
	const guint8 *a = g_inet_address_to_bytes(address);

	if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4)
		return a[0] == 127;
	if (memcmp(a, zeros, 15) == 0 && a[15] == 1)
		return TRUE;
	return memcmp(a, zeros, 10) == 0 && a[10] == 0xff && a[11] == 0xff && a[12] == 127;
}

void
axis_http_set_offsite_testing(
	gboolean      allow_loopback,
	GTlsDatabase *database
){
	g_mutex_lock(&testing_lock);
	testing_loopback = allow_loopback;
	if (database != NULL)
		g_object_ref(database);
	g_clear_object(&testing_database);
	testing_database = database;
	g_mutex_unlock(&testing_lock);
}

/* Whether a connection may be made to @address: a public one, or, for the
 * tests, loopback when they allowed it. */
static gboolean
may_connect(GSocketAddress *address)
{
	GInetAddress *inet;
	gboolean allowed;

	if (address == NULL || !G_IS_INET_SOCKET_ADDRESS(address))
		return FALSE;
	inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address));
	if (axis_http_address_is_public(inet))
		return TRUE;
	g_mutex_lock(&testing_lock);
	allowed = testing_loopback && is_loopback(inet);
	g_mutex_unlock(&testing_lock);
	return allowed;
}

/* ── The outbound binding ───────────────────────────────────────── */

static AxisOutboundDestination
destination_of(GInetAddress *inet)
{
	if (is_loopback(inet))
		return AXIS_OUTBOUND_TO_LOOPBACK;
	return axis_http_address_is_public(inet) ? AXIS_OUTBOUND_TO_PUBLIC : AXIS_OUTBOUND_TO_LOCAL;
}

/*
 * Each TCP connection of a run that outbound.bind may apply to, at
 * "connecting": the socket exists and connect() has not been called (GLib
 * gives the address it is about to connect to).  Whether the binding
 * applies is judged on that address; then the interface's state is read
 * now, and the socket is tied to the interface and its address, or closed
 * there and then, so that the connection is never made.  The run fails
 * unless only this address's family was missing (the server's other
 * addresses are still tried).
 */
static void
on_bind_event(
	SoupMessage        *message,
	GSocketClientEvent  event,
	GIOStream          *connection,
	gpointer            user_data
){
	Run *run = user_data;
	AxisOutboundTarget target;
	AxisOutboundScope scope;
	AxisOutboundState state;
	AxisOutboundVerdict verdict;
	GSocketAddress *address = NULL;
	GSocket *socket;
	g_autofree gchar *problem = NULL;
	gint family;

	if (event != G_SOCKET_CLIENT_CONNECTING || run->finished)
		return;
	if (!G_IS_SOCKET_CONNECTION(connection)) {
		fail(run, AXIS_HTTP_ERROR_CONNECT, "the connection to %s could not be checked against the outbound "
			"interface; refused, and nothing was sent", run->peer);
		return;
	}
	socket = g_socket_connection_get_socket(G_SOCKET_CONNECTION(connection));
	address = g_socket_connection_get_remote_address(G_SOCKET_CONNECTION(connection), NULL);
	if (address == NULL || !G_IS_INET_SOCKET_ADDRESS(address)) {
		g_clear_object(&address);
		g_socket_close(socket, NULL);
		fail(run, AXIS_HTTP_ERROR_CONNECT, "the connection to %s could not be checked against the outbound "
			"interface; refused, and nothing was sent", run->peer);
		return;
	}
	axis_outbound_get_config(&target, &scope);
	family = g_socket_get_family(socket) == G_SOCKET_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	if (!axis_outbound_applies(&target, scope, run->config->traffic,
	                           destination_of(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address))))) {
		g_object_unref(address);
		run->let_through++;
		return;
	}
	axis_outbound_read(&state);
	verdict = axis_outbound_decide(&target, scope, run->config->traffic,
		destination_of(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address))), family, &state, &problem);
	g_object_unref(address);
	switch (verdict) {
	case AXIS_OUTBOUND_PASS:
		run->let_through++;
		return;
	case AXIS_OUTBOUND_BIND:
		if (axis_outbound_bind_socket(g_socket_get_fd(socket), family, &state, &problem)) {
			run->bound++;
			run->let_through++;
			return;
		}
		break;
	case AXIS_OUTBOUND_REFUSE_FAMILY:
		/* This connection only: the server may have an address of the other
		 * family, which GLib tries next. */
		g_socket_close(socket, NULL);
		g_free(run->bind_problem);
		run->bind_problem = g_steal_pointer(&problem);
		return;
	case AXIS_OUTBOUND_REFUSE:
	default:
		break;
	}
	g_socket_close(socket, NULL);
	fail(run, AXIS_HTTP_ERROR_CONNECT, "%s; nothing was sent to %s", problem != NULL ? problem :
		"the outbound interface cannot be used", run->peer);
}

/* Just before a request is written: when its address is one the binding
 * applies to, a connection of this run must have been tied to the outbound
 * interface (every connection of the run's sessions is made by them, so this
 * holds while on_bind_event() does; it is the last guard). */
static void
on_bind_starting(
	SoupMessage *message,
	gpointer     user_data
){
	Run *run = user_data;
	AxisOutboundTarget target;
	AxisOutboundScope scope;
	GSocketAddress *address = soup_message_get_remote_address(message);
	gboolean applies = TRUE;

	if (run->finished)
		return;
	axis_outbound_get_config(&target, &scope);
	if (address != NULL && G_IS_INET_SOCKET_ADDRESS(address))
		applies = axis_outbound_applies(&target, scope, run->config->traffic,
			destination_of(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address))));
	if (applies && run->bound == 0)
		fail(run, AXIS_HTTP_ERROR_CONNECT, "the connection to %s was not tied to the outbound interface; refused, "
			"and nothing was sent", run->peer);
}

/* The binding's handlers, on a message of a run it may apply to. */
static void
watch_binding(Run *run)
{
	if (!axis_outbound_may_apply(run->config->traffic))
		return;
	g_signal_connect(run->message, "network-event", G_CALLBACK(on_bind_event), run);
	g_signal_connect(run->message, "starting", G_CALLBACK(on_bind_starting), run);
}

/* ── The exchange ───────────────────────────────────────────────── */

static void
send_request(
	Run         *run,
	const gchar *url,
	const gchar *const *headers
);

static void
send_offsite(Run *run);

static void
on_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Run *run = user_data;
	g_autoptr(GError) error = NULL;
	gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

	if (run->finished)
		return;
	if (run->reading_refusal) {
		/* A 403: the first bytes are kept for the caller, and it fails whatever
		 * becomes of the rest. */
		if (n > 0) {
			gsize room = AXIS_HTTP_REFUSAL_BYTES - run->body->len;

			g_byte_array_append(run->body, (const guint8 *)run->buffer, (guint)MIN((gsize)n, room));
			if (run->body->len < AXIS_HTTP_REFUSAL_BYTES) {
				g_input_stream_read_async(run->stream, run->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, run->cancel,
					on_read, run);
				return;
			}
		}
		if (run->body->len > 0)
			run->response->refusal = g_strndup((const gchar *)run->body->data, run->body->len);
		fail(run, AXIS_HTTP_ERROR_STATUS, "%s answered 403: the %s was refused", run->peer, run->secret_name);
		return;
	}
	if (n < 0) {
		fail_from(run, error);
		return;
	}
	if (n == 0) {
		succeed(run);
		return;
	}
	if (run->body->len + (gsize)n > run->max_bytes) {
		run->too_large = TRUE;
		fail(run, AXIS_HTTP_ERROR_TOO_LARGE, "%s's answer is larger than the limit", run->peer);
		return;
	}
	g_byte_array_append(run->body, (const guint8 *)run->buffer, (guint)n);
	g_input_stream_read_async(run->stream, run->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, run->cancel, on_read, run);
}

/* Whether @target is the configured origin (scheme, host and port). */
static gboolean
same_origin(
	const AxisHttpOrigin *origin,
	GUri                 *target
){
	const gchar *scheme = g_uri_get_scheme(target);
	const gchar *host = g_uri_get_host(target);
	gint port = g_uri_get_port(target);

	if (scheme == NULL || host == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(scheme, origin->https ? "https" : "http") != 0)
		return FALSE;
	if (g_ascii_strcasecmp(host, origin->host) != 0)
		return FALSE;
	if (port < 0)
		port = (gint)(origin->https ? 443 : 80);
	if ((guint)port != origin->port)
		return FALSE;
	if (g_uri_get_user(target) != NULL || g_uri_get_password(target) != NULL)
		return FALSE;
	return TRUE;
}

/* Keeps a refused redirect's Location for the caller, when it is plain
 * printable ASCII (no space, no control character): the caller reads it
 * and never requests it. */
static void
note_location(
	Run         *run,
	const gchar *location
){
	const gchar *p;

	if (run->response == NULL)
		return;
	g_clear_pointer(&run->response->location, g_free);
	for (p = location; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return;
	}
	run->response->location = g_strdup(location);
}

/* Plain printable ASCII: no space, no control character, nothing beyond. */
static gboolean
plain_ascii(const gchar *text)
{
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

/*
 * The one redirect a release's fetch may follow off the origin, to the
 * indexer's own site, under the rules of axis_http_fetch_release().  Where
 * it points is kept for this request only: the indexer's key is in it.
 */
static void
follow_offsite(
	Run         *run,
	const gchar *location,
	GUri        *target
){
	const gchar *scheme = g_uri_get_scheme(target);
	g_autoptr(GUri) exact = NULL;

	if (scheme == NULL || g_ascii_strcasecmp(scheme, "https") != 0) {
		/* A magnet link is the caller's to read; nothing else is taken. */
		note_location(run, location);
		fail(run, AXIS_HTTP_ERROR_REDIRECT,
			"%s redirected to an address that is not https; refused, and nothing was sent there", run->peer);
		return;
	}
	/* The address as it was written, %-encoding and all (a key can hold
	 * "%2B"), less any fragment, which is never sent. */
	exact = g_uri_parse_relative(soup_message_get_uri(run->message), location, G_URI_FLAGS_ENCODED, NULL);
	if (exact == NULL || !plain_ascii(location) || g_uri_get_host(exact) == NULL || g_uri_get_host(exact)[0] == '\0' ||
	    g_uri_get_user(exact) != NULL || g_uri_get_password(exact) != NULL) {
		fail(run, AXIS_HTTP_ERROR_REDIRECT,
			"%s redirected to an address that cannot be used; refused, and nothing was sent there", run->peer);
		return;
	}
	run->offsite_url = g_uri_to_string_partial(exact, G_URI_HIDE_FRAGMENT);
	run->offsite = TRUE;
	run->peer = "the indexer's site";
	if (run->response != NULL)
		run->response->offsite = TRUE;
	g_clear_object(&run->stream);
	send_offsite(run);
}

static void
follow_redirect(Run *run)
{
	SoupMessageHeaders *headers = soup_message_get_response_headers(run->message);
	const gchar *location = soup_message_headers_get_one(headers, "Location");
	g_autoptr(GUri) target = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;

	/* A body is sent once, to the address it was meant for: a POST is
	 * never sent again, wherever the redirect points. */
	if (strcmp(run->method, SOUP_METHOD_GET) != 0) {
		fail(run, AXIS_HTTP_ERROR_REDIRECT,
			"%s answered with a redirect; a request with a body is never sent again, so it was not followed",
			run->peer);
		return;
	}
	if (location == NULL || strlen(location) > AXIS_HTTP_MAX_LOCATION_BYTES) {
		fail(run, AXIS_HTTP_ERROR_REDIRECT, "%s redirected without saying where", run->peer);
		return;
	}
	/* The indexer's site answers once and is not followed further: one
	 * redirect off the origin, and no more of any kind. */
	if (run->offsite) {
		note_location(run, location);
		fail(run, AXIS_HTTP_ERROR_REDIRECT, "%s redirected again; only one redirect is followed, so this was not",
			run->peer);
		return;
	}
	target = g_uri_parse_relative(soup_message_get_uri(run->message), location, G_URI_FLAGS_NONE, &error);
	if (target != NULL && !same_origin(run->config->origin, target) && run->offsite_allowed) {
		follow_offsite(run, location, target);
		return;
	}
	if (target == NULL || !same_origin(run->config->origin, target)) {
		note_location(run, location);
		fail(run, AXIS_HTTP_ERROR_REDIRECT,
			"%s redirected to a different address; refused, and the %s was not sent there", run->peer,
			run->secret_name);
		return;
	}
	if (run->redirects >= AXIS_HTTP_MAX_REDIRECTS) {
		fail(run, AXIS_HTTP_ERROR_REDIRECT, "%s redirected too many times", run->peer);
		return;
	}
	run->redirects++;
	url = g_uri_to_string(target);
	g_clear_object(&run->stream);
	send_request(run, url, NULL);
}

gint64
axis_http_parse_retry_after(
	const gchar *value,
	gint64       now_s
){
	const gchar *p;
	gint64 seconds = 0;
	g_autoptr(GDateTime) when = NULL;

	if (value == NULL)
		return -1;
	while (*value == ' ' || *value == '\t')
		value++;
	if (*value == '\0' || strlen(value) > 64)
		return -1;
	if (g_ascii_isdigit(*value)) {
		for (p = value; g_ascii_isdigit(*p); p++) {
			if (seconds < AXIS_HTTP_MAX_RETRY_AFTER_S)
				seconds = seconds * 10 + (*p - '0');
		}
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p != '\0')
			return -1;
		return MIN(seconds, (gint64)AXIS_HTTP_MAX_RETRY_AFTER_S);
	}
	when = soup_date_time_new_from_http_string(value);
	if (when == NULL)
		return -1;
	seconds = g_date_time_to_unix(when) - now_s;
	return CLAMP(seconds, 0, (gint64)AXIS_HTTP_MAX_RETRY_AFTER_S);
}

gint64
axis_http_parse_ratelimit_reset(
	const gchar *value,
	gint64       now_s
){
	const gchar *p;
	gint64 reset = 0;

	if (value == NULL)
		return -1;
	while (*value == ' ' || *value == '\t')
		value++;
	if (!g_ascii_isdigit(*value) || strlen(value) > 20)
		return -1;
	for (p = value; g_ascii_isdigit(*p); p++) {
		/* Far beyond any real time: stop growing, the clamp decides. */
		if (reset < G_MAXINT64 / 100)
			reset = reset * 10 + (*p - '0');
	}
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '\0')
		return -1;
	return CLAMP(reset - now_s, 0, (gint64)AXIS_HTTP_MAX_RETRY_AFTER_S);
}

/* What the answer said beside its body, for the caller. */
static void
note_response(
	Run     *run,
	guint    status,
	gboolean success
){
	SoupMessageHeaders *headers;

	if (run->response == NULL)
		return;
	headers = soup_message_get_response_headers(run->message);
	run->response->status = status;
	run->response->retry_after_s = -1;
	run->response->ratelimit_wait_s = -1;
	run->response->challenged = FALSE;
	g_clear_pointer(&run->response->content_type, g_free);
	if (success) {
		const gchar *type = soup_message_headers_get_content_type(headers, NULL);

		if (type != NULL && strlen(type) <= 127)
			run->response->content_type = g_ascii_strdown(type, -1);
	} else {
		const gchar *mitigated = soup_message_headers_get_one(headers, "cf-mitigated");
		const gchar *remaining = soup_message_headers_get_one(headers, "x-ratelimit-remaining");

		/* Out of allowance (or not said): when it returns. */
		if (remaining == NULL || g_strcmp0(remaining, "0") == 0)
			run->response->ratelimit_wait_s = axis_http_parse_ratelimit_reset(
				soup_message_headers_get_one(headers, "x-ratelimit-reset"), g_get_real_time() / G_USEC_PER_SEC);

		run->response->retry_after_s = axis_http_parse_retry_after(
			soup_message_headers_get_one(headers, "Retry-After"), g_get_real_time() / G_USEC_PER_SEC);
		run->response->challenged = mitigated != NULL && g_ascii_strcasecmp(mitigated, "challenge") == 0;
	}
}

/* A download client's session keeps the cookies its origin sets (every
 * answer is from the origin: a redirect elsewhere is never followed). */
static void
keep_cookies(Run *run)
{
	SoupMessageHeaders *headers;
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;

	if (run->session == NULL || run->session->cookies == NULL)
		return;
	headers = soup_message_get_response_headers(run->message);
	soup_message_headers_iter_init(&iter, headers);
	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		if (g_ascii_strcasecmp(name, "Set-Cookie") == 0)
			axis_http_cookies_take(run->session->cookies, value);
	}
}

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Run *run = user_data;
	g_autoptr(GError) error = NULL;
	GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	guint status;
	goffset length;

	if (run->finished) {
		g_clear_object(&stream);
		return;
	}
	if (stream == NULL) {
		fail_from(run, error);
		return;
	}
	status = soup_message_get_status(run->message);
	/* What the indexer's site sets is never kept: it is not the origin. */
	if (!run->offsite)
		keep_cookies(run);
	if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
		g_object_unref(stream);
		note_response(run, status, FALSE);
		follow_redirect(run);
		return;
	}
	note_response(run, status, status >= 200 && status <= 299);
	if (status == 403 && !run->offsite && run->response != NULL &&
	    ((run->session != NULL && run->session->purpose == AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT) ||
	     (run->session == NULL && run->config->traffic == AXIS_OUTBOUND_TRAFFIC_INDEXER))) {
		/* A download client's 403 may say why in its body, and an indexer's
		 * may be a browser challenge's page: read the first bytes. */
		run->reading_refusal = TRUE;
		run->stream = stream;
		g_input_stream_read_async(run->stream, run->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, run->cancel, on_read, run);
		return;
	}
	if (status < 200 || status > 299) {
		g_object_unref(stream);
		/* The indexer's site never saw the source's key: its 401 is its own. */
		if ((status == 401 || status == 403) && !run->offsite)
			fail(run, AXIS_HTTP_ERROR_STATUS, "%s answered %u: the %s was refused", run->peer, status,
				run->secret_name);
		else
			fail(run, AXIS_HTTP_ERROR_STATUS, "%s answered %u", run->peer, status);
		return;
	}
	length = soup_message_headers_get_content_length(soup_message_get_response_headers(run->message));
	if (length > 0 && (guint64)length > run->max_bytes) {
		g_object_unref(stream);
		run->too_large = TRUE;
		fail(run, AXIS_HTTP_ERROR_TOO_LARGE, "%s's answer is larger than the limit", run->peer);
		return;
	}
	run->stream = stream;
	g_input_stream_read_async(run->stream, run->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, run->cancel, on_read, run);
}

static gboolean
on_accept_certificate(
	SoupMessage         *message,
	GTlsCertificate     *certificate,
	GTlsCertificateFlags errors,
	gpointer             user_data
){
	/* Connected only when the administrator allowed it. */
	return TRUE;
}

static void
on_wrote_headers(
	SoupMessage *message,
	gpointer     user_data
){
	Run *run = user_data;

	run->sent = TRUE;
	if (run->response != NULL)
		run->response->sent = TRUE;
}

static void
send_request(
	Run                *run,
	const gchar        *url,
	const gchar *const *headers
){
	SoupMessageHeaders *request_headers;
	guint i;

	g_clear_object(&run->message);
	run->message = soup_message_new(run->method, url);
	if (run->message == NULL) {
		fail(run, AXIS_HTTP_ERROR_CONFIG, "%s's address cannot be used", run->peer);
		return;
	}
	/* Every hop is checked here; the library never follows one. */
	soup_message_add_flags(run->message, SOUP_MESSAGE_NO_REDIRECT);
	request_headers = soup_message_get_request_headers(run->message);
	soup_message_headers_replace(request_headers, "Accept",
		run->config->accept != NULL ? run->config->accept : "application/json");
	if (run->config->auth_header != NULL && run->config->auth_header[0] != '\0' &&
	    run->config->auth_value != NULL && run->config->auth_value[0] != '\0')
		soup_message_headers_replace(request_headers, run->config->auth_header, run->config->auth_value);
	for (i = 0; headers != NULL && headers[i] != NULL && headers[i + 1] != NULL; i += 2)
		soup_message_headers_replace(request_headers, headers[i], headers[i + 1]);
	if (run->session != NULL && run->session->cookies != NULL) {
		gchar *cookie = axis_http_cookies_header(run->session->cookies);

		/* Only to this origin: the request goes nowhere else. */
		if (cookie != NULL)
			soup_message_headers_replace(request_headers, "Cookie", cookie);
		axis_http_cookies_header_free(cookie);
	}
	if (run->request_body != NULL)
		soup_message_set_request_body_from_bytes(run->message, run->content_type, run->request_body);
	if (run->config->accept_untrusted_certificate)
		g_signal_connect(run->message, "accept-certificate", G_CALLBACK(on_accept_certificate), run);
	g_signal_connect(run->message, "wrote-headers", G_CALLBACK(on_wrote_headers), run);
	watch_binding(run);
	soup_session_send_async(run->soup, run->message, G_PRIORITY_DEFAULT, run->cancel, on_sent, run);
}

/* A session: no proxy (the environment is not read, and the address
 * connected to is the server's own), no cookie jar of the library's (a
 * session keeps its own), no decoding. */
static SoupSession *
new_soup(
	const AxisHttpConfig *config,
	guint                 timeout_ms
){
	SoupSession *soup = soup_session_new_with_options("proxy-resolver", (GProxyResolver *)NULL,
		"user-agent", config->user_agent != NULL ? config->user_agent : "axisd",
		"timeout", (guint)((timeout_ms + 999) / 1000), NULL);

	soup_session_remove_feature_by_type(soup, SOUP_TYPE_CONTENT_DECODER);
	soup_session_remove_feature_by_type(soup, SOUP_TYPE_COOKIE_JAR);
	return soup;
}

static void
refuse_address(Run *run)
{
	fail(run, AXIS_HTTP_ERROR_REDIRECT, "%s is not at a public address (it is on this machine or a local network); "
		"refused, and nothing was sent there", run->peer);
}

/*
 * The address of each connection to the indexer's site, as it is made: at
 * "connecting" the address the connection is about to be made to (GLib
 * gives it before it calls connect()), at "connected" the peer it reached,
 * before a byte of TLS or HTTP.  One that is not public closes the socket
 * there and then, so the connection is never made (or nothing is sent on
 * it), and fails the request.
 */
static void
on_offsite_event(
	SoupMessage        *message,
	GSocketClientEvent  event,
	GIOStream          *connection,
	gpointer            user_data
){
	Run *run = user_data;
	GSocketAddress *address = NULL;
	gboolean allowed;

	if (event != G_SOCKET_CLIENT_CONNECTING && event != G_SOCKET_CLIENT_CONNECTED)
		return;
	if (G_IS_SOCKET_CONNECTION(connection))
		address = g_socket_connection_get_remote_address(G_SOCKET_CONNECTION(connection), NULL);
	allowed = !run->finished && may_connect(address);
	g_clear_object(&address);
	if (allowed) {
		if (event == G_SOCKET_CLIENT_CONNECTED)
			run->address_checked = TRUE;
		return;
	}
	if (G_IS_SOCKET_CONNECTION(connection))
		g_socket_close(g_socket_connection_get_socket(G_SOCKET_CONNECTION(connection)), NULL);
	run->address_checked = FALSE;
	refuse_address(run);
}

/* Just before the request is written: the connection it goes on must be one
 * that was checked, to an address that may be reached. */
static void
on_offsite_starting(
	SoupMessage *message,
	gpointer     user_data
){
	Run *run = user_data;

	if (!run->address_checked || !may_connect(soup_message_get_remote_address(message)))
		refuse_address(run);
}

/* The last guard: a request that went out on a connection that was not
 * checked fails, whatever it is answered (it cannot happen while the two
 * checks above hold). */
static void
on_offsite_wrote_headers(
	SoupMessage *message,
	gpointer     user_data
){
	Run *run = user_data;

	if (!run->address_checked)
		refuse_address(run);
}

/* The followed request to the indexer's site: a session of its own, the
 * system's authorities (or the tests' own), a connection of its own, the
 * address checked where it is connected to, and no header of the caller's,
 * of the source's or of Axis's but the user agent the session sets. */
static void
send_offsite(Run *run)
{
	GTlsDatabase *database = NULL;

	g_mutex_lock(&testing_lock);
	if (testing_database != NULL)
		database = g_object_ref(testing_database);
	g_mutex_unlock(&testing_lock);

	run->offsite_soup = new_soup(run->config, run->timeout_ms);
	if (database != NULL) {
		soup_session_set_tls_database(run->offsite_soup, database);
		g_object_unref(database);
	}
	g_clear_object(&run->message);
	run->message = soup_message_new(SOUP_METHOD_GET, run->offsite_url);
	if (run->message == NULL) {
		fail(run, AXIS_HTTP_ERROR_REDIRECT, "%s's address cannot be used; refused, and nothing was sent there",
			run->peer);
		return;
	}
	soup_message_add_flags(run->message, SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_NEW_CONNECTION);
	g_signal_connect(run->message, "network-event", G_CALLBACK(on_offsite_event), run);
	g_signal_connect(run->message, "starting", G_CALLBACK(on_offsite_starting), run);
	g_signal_connect(run->message, "wrote-headers", G_CALLBACK(on_offsite_wrote_headers), run);
	/* The indexer's site is the indexer's traffic: bound as the source is.
	 * Its connections are counted afresh: none of the source's carries it. */
	run->bound = 0;
	run->let_through = 0;
	g_clear_pointer(&run->bind_problem, g_free);
	watch_binding(run);
	soup_session_send_async(run->offsite_soup, run->message, G_PRIORITY_DEFAULT, run->cancel, on_sent, run);
}

static gboolean
on_deadline(gpointer user_data)
{
	Run *run = user_data;

	if (!run->finished) {
		run->timed_out = TRUE;
		fail(run, AXIS_HTTP_ERROR_TIMEOUT, "%s did not finish answering in time", run->peer);
	}
	return G_SOURCE_REMOVE;
}

static void
on_external_cancel(
	GCancellable *cancellable,
	gpointer      user_data
){
	Run *run = user_data;

	/* Any thread: cancelling wakes the loop through the operations. */
	g_cancellable_cancel(run->cancel);
	g_main_context_wakeup(run->context);
}

static gboolean
path_is_plain(const gchar *path)
{
	const gchar *p;

	if (path == NULL || path[0] != '/' || path[1] == '/' || strlen(path) > AXIS_HTTP_MAX_PATH_BYTES)
		return FALSE;
	for (p = path; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '\\' || *p == '#')
			return FALSE;
	}
	return TRUE;
}

/* A media type for a request body: printable ASCII, no line breaks. */
static gboolean
content_type_is_plain(const gchar *type)
{
	const gchar *p;

	if (type == NULL || type[0] == '\0' || strlen(type) > 200 || strchr(type, '/') == NULL)
		return FALSE;
	for (p = type; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* The pinned requests that are not a download client's and not a GET. */
typedef enum
{
	PINNED_NONE = 0,
	PINNED_RESCAN,	/* axis_http_post_rescan() */
	PINNED_LOGIN	/* axis_http_post_login() */
} Pinned;

static gboolean pinned_permitted(Pinned pinned, const gchar *method, const gchar *path);

/* One request, as the module comment describes; with @offsite_allowed, a
 * release's fetch (axis_http_fetch_release()); @pinned names the one pinned
 * request it is, if any, and @body_secret a secret its body holds. */
static gboolean
request(
	const AxisHttpConfig *config,
	AxisHttpSession      *session,
	const gchar          *method,
	const gchar          *path,
	const gchar          *content_type,
	GBytes               *request_body,
	const gchar *const   *headers,
	gboolean              offsite_allowed,
	Pinned                pinned,
	const gchar          *body_secret,
	GBytes              **body,
	AxisHttpResponse     *response,
	GError              **error
){
	Run run;
	g_autofree gchar *url = NULL;
	const gchar *peer;
	GSource *deadline;
	gulong handler = 0;
	guint timeout_ms;
	gboolean ok;

	*body = NULL;
	if (response != NULL) {
		response->status = 0;
		response->retry_after_s = -1;
		response->ratelimit_wait_s = -1;
		response->content_type = NULL;
		response->sent = FALSE;
		response->location = NULL;
		response->offsite = FALSE;
		response->refusal = NULL;
		response->challenged = FALSE;
	}
	peer = config->peer != NULL ? config->peer : "the media server";
	if (config->origin->base == NULL || !path_is_plain(path)) {
		g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG,
			"%s's address or the request is not valid", peer);
		return FALSE;
	}
	if (strcmp(method, SOUP_METHOD_GET) != 0) {
		/* Only a download client's connection, and the two pinned requests
		 * (made by axis_http_post_rescan() and axis_http_post_login() alone,
		 * with no session), send anything but GET. */
		gboolean download_post = session != NULL && session->purpose == AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT &&
			strcmp(method, SOUP_METHOD_POST) == 0;
		gboolean is_pinned = session == NULL && pinned_permitted(pinned, method, path);

		if (!download_post && !is_pinned) {
			g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG,
				"only a download client's connection may send anything but GET (and a media server one request, "
				"to rescan a folder, and a metadata provider one, to log in); nothing was sent to %s", peer);
			return FALSE;
		}
		if (request_body == NULL || !content_type_is_plain(content_type)) {
			g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG,
				"the request to %s has no body or no valid content type; nothing was sent", peer);
			return FALSE;
		}
		if (g_bytes_get_size(request_body) > AXIS_HTTP_MAX_REQUEST_BYTES) {
			g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_TOO_LARGE,
				"the request to %s is larger than the limit; nothing was sent", peer);
			return FALSE;
		}
	}
	if (g_cancellable_is_cancelled(config->cancellable)) {
		g_set_error_literal(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CANCELLED, "cancelled");
		return FALSE;
	}

	memset(&run, 0, sizeof(run));
	run.config = config;
	run.session = session;
	run.method = method;
	run.path = path;
	run.content_type = content_type;
	run.request_body = request_body;
	run.body_secret = body_secret;
	run.peer = peer;
	run.secret_name = config->secret_name != NULL ? config->secret_name : "token";
	run.response = response;
	run.max_bytes = config->max_bytes != 0 ? config->max_bytes : AXIS_HTTP_DEFAULT_MAX_BYTES;
	run.body = g_byte_array_new();
	run.cancel = g_cancellable_new();
	run.context = g_main_context_new();
	run.loop = g_main_loop_new(run.context, FALSE);
	run.offsite_allowed = offsite_allowed && session == NULL && strcmp(method, SOUP_METHOD_GET) == 0;
	timeout_ms = config->timeout_ms != 0 ? config->timeout_ms : AXIS_HTTP_DEFAULT_TIMEOUT_MS;
	run.timeout_ms = timeout_ms;

	g_main_context_push_thread_default(run.context);
	run.soup = new_soup(config, timeout_ms);

	deadline = g_timeout_source_new(timeout_ms);
	g_source_set_callback(deadline, on_deadline, &run, NULL);
	g_source_attach(deadline, run.context);
	if (config->cancellable != NULL)
		handler = g_cancellable_connect(config->cancellable, G_CALLBACK(on_external_cancel), &run, NULL);

	url = g_strconcat(config->origin->base, path, NULL);
	send_request(&run, url, headers);
	if (!run.finished)
		g_main_loop_run(run.loop);

	if (handler != 0)
		g_cancellable_disconnect(config->cancellable, handler);
	g_source_destroy(deadline);
	g_source_unref(deadline);
	g_cancellable_cancel(run.cancel);
	g_clear_object(&run.stream);
	g_clear_object(&run.message);
	g_clear_object(&run.soup);
	g_clear_object(&run.offsite_soup);
	while (g_main_context_iteration(run.context, FALSE))
		;
	g_main_context_pop_thread_default(run.context);
	if (run.offsite_url != NULL) {
		memset(run.offsite_url, 0, strlen(run.offsite_url));
		g_clear_pointer(&run.offsite_url, g_free);
	}
	g_free(run.bind_problem);

	ok = run.error == NULL;
	if (ok)
		*body = g_byte_array_free_to_bytes(run.body);
	else {
		g_byte_array_unref(run.body);
		g_propagate_error(error, run.error);
	}
	g_main_loop_unref(run.loop);
	g_main_context_unref(run.context);
	g_object_unref(run.cancel);
	return ok;
}

gboolean
axis_http_get(
	const AxisHttpConfig *config,
	const gchar          *path,
	const gchar *const   *headers,
	GBytes              **body,
	GError              **error
){
	return axis_http_get_full(config, path, headers, body, NULL, error);
}

gboolean
axis_http_get_full(
	const AxisHttpConfig *config,
	const gchar          *path,
	const gchar *const   *headers,
	GBytes              **body,
	AxisHttpResponse     *response,
	GError              **error
){
	g_return_val_if_fail(config != NULL && config->origin != NULL && body != NULL, FALSE);

	return request(config, NULL, SOUP_METHOD_GET, path, NULL, NULL, headers, FALSE, PINNED_NONE, NULL, body, response,
		error);
}

gboolean
axis_http_fetch_release(
	const AxisHttpConfig *config,
	const gchar          *path,
	const gchar *const   *headers,
	GBytes              **body,
	AxisHttpResponse     *response,
	GError              **error
){
	g_return_val_if_fail(config != NULL && config->origin != NULL && body != NULL, FALSE);

	return request(config, NULL, SOUP_METHOD_GET, path, NULL, NULL, headers, TRUE, PINNED_NONE, NULL, body, response,
		error);
}

/* ── Sessions ───────────────────────────────────────────────────── */

static void
wipe(gchar *text)
{
	if (text == NULL)
		return;
	memset(text, 0, strlen(text));
	g_free(text);
}

AxisHttpSession *
axis_http_session_new(
	const AxisHttpConfig *config,
	AxisHttpPurpose       purpose
){
	AxisHttpSession *self;

	g_return_val_if_fail(config != NULL && config->origin != NULL, NULL);

	self = g_new0(AxisHttpSession, 1);
	self->purpose = purpose == AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT ? AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT :
		AXIS_HTTP_PURPOSE_READ;
	self->origin.https = config->origin->https;
	self->origin.host = g_strdup(config->origin->host);
	self->origin.port = config->origin->port;
	self->origin.base = g_strdup(config->origin->base);
	self->auth_header = g_strdup(config->auth_header);
	self->auth_value = g_strdup(config->auth_value);
	self->token = g_strdup(config->token);
	self->peer = g_strdup(config->peer);
	self->secret_name = g_strdup(config->secret_name);
	self->user_agent = g_strdup(config->user_agent);
	self->accept = g_strdup(config->accept);
	self->cancellable = config->cancellable != NULL ? g_object_ref(config->cancellable) : NULL;
	self->secrets = g_ptr_array_new_with_free_func((GDestroyNotify)wipe);
	if (self->purpose == AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT)
		self->cookies = axis_http_cookies_new();

	memset(&self->config, 0, sizeof(self->config));
	self->config.origin = &self->origin;
	self->config.auth_header = self->auth_header;
	self->config.auth_value = self->auth_value;
	self->config.token = self->token;
	self->config.accept_untrusted_certificate = config->accept_untrusted_certificate;
	self->config.timeout_ms = config->timeout_ms;
	self->config.max_bytes = config->max_bytes;
	self->config.cancellable = self->cancellable;
	self->config.peer = self->peer;
	self->config.secret_name = self->secret_name;
	self->config.user_agent = self->user_agent;
	self->config.accept = self->accept;
	self->config.traffic = config->traffic;
	return self;
}

void
axis_http_session_free(AxisHttpSession *self)
{
	if (self == NULL)
		return;
	axis_http_origin_clear(&self->origin);
	g_free(self->auth_header);
	wipe(self->auth_value);
	wipe(self->token);
	g_free(self->peer);
	g_free(self->secret_name);
	g_free(self->user_agent);
	g_free(self->accept);
	g_clear_object(&self->cancellable);
	axis_http_cookies_free(self->cookies);
	g_ptr_array_unref(self->secrets);
	g_free(self);
}

AxisHttpPurpose
axis_http_session_get_purpose(AxisHttpSession *self)
{
	return self->purpose;
}

void
axis_http_session_add_secret(
	AxisHttpSession *self,
	const gchar     *secret
){
	g_return_if_fail(self != NULL);

	if (secret != NULL && strlen(secret) >= MIN_SECRET_BYTES)
		g_ptr_array_add(self->secrets, g_strdup(secret));
}

void
axis_http_session_set_cancellable(
	AxisHttpSession *self,
	GCancellable    *cancellable
){
	g_return_if_fail(self != NULL);

	if (cancellable != NULL)
		g_object_ref(cancellable);
	g_clear_object(&self->cancellable);
	self->cancellable = cancellable;
	self->config.cancellable = cancellable;
}

gboolean
axis_http_session_get(
	AxisHttpSession     *self,
	const gchar         *path,
	const gchar *const  *headers,
	GBytes             **body,
	AxisHttpResponse    *response,
	GError             **error
){
	g_return_val_if_fail(self != NULL && body != NULL, FALSE);

	return request(&self->config, self, SOUP_METHOD_GET, path, NULL, NULL, headers, FALSE, PINNED_NONE, NULL, body,
		response, error);
}

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
){
	g_return_val_if_fail(self != NULL && body != NULL, FALSE);

	return request(&self->config, self, SOUP_METHOD_POST, path, content_type, request_body, headers, FALSE,
		PINNED_NONE, NULL, body, response, error);
}

/* ── The one write to a media server ────────────────────────────── */

/* The pinned request, as data: nothing else lets a connection without a
 * download client's session send a method other than GET. */
static const struct
{
	const gchar *method;
	const gchar *path;
} rescan_request = { AXIS_HTTP_RESCAN_METHOD, AXIS_HTTP_RESCAN_PATH };

static const gchar *const rescan_types[] = { "Created", "Modified", "Deleted" };

gboolean
axis_http_rescan_permitted(
	const gchar *method,
	const gchar *path
){
	return method != NULL && path != NULL && strcmp(method, rescan_request.method) == 0 &&
		strcmp(path, rescan_request.path) == 0;
}

/* @text as the inside of a JSON string; the caller has checked it is valid
 * UTF-8 with no control character, so only the quote and the backslash
 * need an escape. */
static void
append_json_string(
	GString     *out,
	const gchar *text
){
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = text; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

static gboolean
rescan_path_ok(const gchar *path)
{
	const gchar *p;
	gsize length;

	if (path == NULL || path[0] != '/')
		return FALSE;
	length = strlen(path);
	if (length > AXIS_HTTP_RESCAN_MAX_PATH_BYTES || !g_utf8_validate(path, (gssize)length, NULL))
		return FALSE;
	for (p = path; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_http_post_rescan(
	const AxisHttpConfig       *config,
	const AxisHttpRescanUpdate *updates,
	guint                       n_updates,
	AxisHttpResponse           *response,
	GError                    **error
){
	g_autoptr(GBytes) payload = NULL;
	g_autoptr(GBytes) body = NULL;
	GString *text;
	guint i;
	gboolean ok;

	g_return_val_if_fail(config != NULL && config->origin != NULL, FALSE);

	if (response != NULL)
		memset(response, 0, sizeof(*response));
	if (updates == NULL || n_updates == 0 || n_updates > AXIS_HTTP_RESCAN_MAX_UPDATES) {
		g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG,
			"a rescan request names from 1 to %d folders; nothing was sent", AXIS_HTTP_RESCAN_MAX_UPDATES);
		return FALSE;
	}
	text = g_string_new("{\"Updates\":[");
	for (i = 0; i < n_updates; i++) {
		if (!rescan_path_ok(updates[i].path) || (guint)updates[i].type >= G_N_ELEMENTS(rescan_types)) {
			g_string_free(text, TRUE);
			g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG,
				"a folder of the rescan request is not valid; nothing was sent");
			return FALSE;
		}
		if (i > 0)
			g_string_append_c(text, ',');
		g_string_append(text, "{\"Path\":");
		append_json_string(text, updates[i].path);
		g_string_append(text, ",\"UpdateType\":");
		append_json_string(text, rescan_types[updates[i].type]);
		g_string_append_c(text, '}');
	}
	g_string_append(text, "]}");
	{
		gsize length = text->len;

		payload = g_bytes_new_take(g_string_free(text, FALSE), length);
	}
	ok = request(config, NULL, rescan_request.method, rescan_request.path, "application/json", payload, NULL, FALSE,
		PINNED_RESCAN, NULL, &body, response, error);
	return ok;
}

/* ── The one write to a metadata provider ───────────────────────── */

/* The pinned login, as data: with the rescan, the only request without a
 * download client's session that may be anything but a GET. */
static const struct
{
	const gchar *method;
	const gchar *path;
} login_request = { AXIS_HTTP_LOGIN_METHOD, AXIS_HTTP_LOGIN_PATH };

gboolean
axis_http_login_permitted(
	const gchar *method,
	const gchar *path
){
	return method != NULL && path != NULL && strcmp(method, login_request.method) == 0 &&
		strcmp(path, login_request.path) == 0;
}

static gboolean
pinned_permitted(
	Pinned       pinned,
	const gchar *method,
	const gchar *path
){
	switch (pinned) {
	case PINNED_RESCAN:
		return axis_http_rescan_permitted(method, path);
	case PINNED_LOGIN:
		return axis_http_login_permitted(method, path);
	default:
		return FALSE;
	}
}

/* A key or PIN as it goes into the login's body: printable ASCII with no
 * space, quote or backslash, so it needs no escape. */
static gboolean
login_secret_ok(const gchar *secret)
{
	const gchar *p;

	if (secret == NULL || secret[0] == '\0' || strlen(secret) > AXIS_HTTP_LOGIN_MAX_SECRET_BYTES)
		return FALSE;
	for (p = secret; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '"' || *p == '\\')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_http_post_login(
	const AxisHttpConfig  *config,
	const gchar           *key,
	const gchar           *pin,
	GBytes               **body,
	AxisHttpResponse      *response,
	GError               **error
){
	g_autoptr(GBytes) payload = NULL;
	gboolean with_pin = pin != NULL && pin[0] != '\0';
	AxisHttpConfig login;
	gchar *text;
	gboolean ok;

	g_return_val_if_fail(config != NULL && config->origin != NULL && body != NULL, FALSE);

	/* The key is blanked out of every message whatever the caller set. */
	login = *config;
	login.token = key;
	*body = NULL;
	if (response != NULL)
		memset(response, 0, sizeof(*response));
	if (config->auth_header != NULL || !login_secret_ok(key) || (with_pin && !login_secret_ok(pin))) {
		g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG,
			"the key or PIN for %s is not one a login can carry; nothing was sent",
			config->peer != NULL ? config->peer : "the provider");
		return FALSE;
	}
	text = with_pin ? g_strconcat("{\"apikey\":\"", key, "\",\"pin\":\"", pin, "\"}", NULL) :
		g_strconcat("{\"apikey\":\"", key, "\"}", NULL);
	payload = g_bytes_new_with_free_func(text, strlen(text), (GDestroyNotify)wipe, text);
	ok = request(&login, NULL, login_request.method, login_request.path, "application/json", payload, NULL, FALSE,
		PINNED_LOGIN, with_pin ? pin : NULL, body, response, error);
	return ok;
}

guint
axis_http_session_n_cookies(AxisHttpSession *self)
{
	return self != NULL ? axis_http_cookies_count(self->cookies) : 0;
}

void
axis_http_session_forget_cookies(AxisHttpSession *self)
{
	if (self != NULL)
		axis_http_cookies_clear(self->cookies);
}
