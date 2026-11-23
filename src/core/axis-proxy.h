/*
 * axis-proxy.h - trusted reverse proxies and forwarding headers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure functions over text: no sockets, no main loop.  Depends on GLib
 * and libc only, so that the fuzz target can build it alone.
 *
 * The client address, scheme and public host of a request are taken from
 * X-Forwarded-For, X-Forwarded-Proto and X-Forwarded-Host only when the
 * directly connected peer is in the trusted list; otherwise the headers are
 * ignored (not refused) and the peer itself is the client.
 * X-Forwarded-For is read from the right: each trusted proxy appends the
 * address it received the request from, so the first address from the
 * right that is not itself a trusted proxy is the client.  Anything the
 * client wrote further left is never looked at.  A value that is not well
 * formed is ignored, never guessed at.
 */

#ifndef AXIS_PROXY_H
#define AXIS_PROXY_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_PROXY_ERROR (axis_proxy_error_quark())

GQuark
axis_proxy_error_quark(void);

/* The only error: the text is not an acceptable trusted-proxy list. */
#define AXIS_PROXY_ERROR_INVALID 0

/* At most this many entries in a trusted-proxy list. */
#define AXIS_PROXY_LIST_MAX_ENTRIES 64

/* At most this many X-Forwarded-For entries are examined. */
#define AXIS_PROXY_MAX_HOPS 32

/* The client address used when the peer's address cannot be read. */
#define AXIS_PROXY_UNKNOWN_CLIENT "unknown"

typedef struct
{
	guint8 family;		/* 4 or 6 */
	guint8 bytes[16];	/* network order; the first 4 for IPv4 */
} AxisIpAddress;

/**
 * axis_ip_address_parse:
 * @text: an address such as "192.0.2.7" or "2001:db8::1"
 * @address: (out) (optional): the address; untouched on failure
 *
 * Accepts exactly a dotted-quad IPv4 address or an IPv6 address as
 * inet_pton() reads them, optionally in square brackets for IPv6.  No
 * port, no zone index, no surrounding space.  An IPv4-mapped IPv6 address
 * (::ffff:a.b.c.d) is returned as the IPv4 address it maps.
 *
 * Returns: %TRUE if @text is such an address
 */
gboolean
axis_ip_address_parse(
	const gchar   *text,
	AxisIpAddress *address
);

/**
 * axis_ip_address_to_string:
 *
 * Returns: (transfer full): the canonical text form of @address
 */
gchar *
axis_ip_address_to_string(const AxisIpAddress *address);

/* Whether @address is a loopback address (127.0.0.0/8, ::1). */
gboolean
axis_ip_address_is_loopback(const AxisIpAddress *address);

/* Whether @address is loopback, private (RFC 1918), link-local
 * (169.254.0.0/16, fe80::/10) or unique-local (fc00::/7): where a reverse
 * proxy, a container gateway or a health checker beside the daemon
 * connects from. */
gboolean
axis_ip_address_is_private(const AxisIpAddress *address);

/**
 * axis_proxy_address_key:
 * @text: (nullable): a client address as text
 *
 * What a per-address count counts @text by: an IPv4 address itself, an
 * IPv6 address by its /64 (what one subscriber is usually given, so one
 * client cannot become many by changing the rest), anything else as it is.
 *
 * Returns: (transfer full): the key
 */
gchar *
axis_proxy_address_key(const gchar *text);

/* The most addresses of one host name that are trusted; the rest are
 * dropped (the first AXIS_PROXY_NAME_MAX_ADDRESSES in address order). */
#define AXIS_PROXY_NAME_MAX_ADDRESSES 16

/* A host name's last good addresses are kept for this long after its
 * lookup starts failing, then dropped: long enough for a DNS blip, short
 * enough that a name that is gone stops being believed. */
#define AXIS_PROXY_NAME_GRACE_US (30 * G_USEC_PER_SEC)

/* The longest host name accepted (DNS's limit). */
#define AXIS_PROXY_NAME_MAX_LEN 253

typedef struct _AxisProxyList AxisProxyList;

/**
 * axis_proxy_name_is_valid:
 * @text: (nullable): a host name such as "traefik" or "proxy.example.net"
 *
 * Accepts dot-separated labels of ASCII letters, digits, '-' and '_', each
 * 1 to 63 characters and none starting or ending with '-', at most
 * %AXIS_PROXY_NAME_MAX_LEN characters in all, whose last label is not all
 * digits (so "300.1.1.1" is a bad address, not a name).  No trailing dot,
 * port, space or other character.
 *
 * Returns: %TRUE if @text is such a name
 */
gboolean
axis_proxy_name_is_valid(const gchar *text);

/**
 * axis_proxy_list_parse:
 * @text: comma-separated IP addresses, CIDR ranges ("10.0.0.0/8") and host
 *   names ("traefik"); empty (or only spaces) for none
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_PROXY_ERROR_INVALID, naming the offending entry, when
 * an entry is empty or is not an address, a range or a name, when a prefix
 * length is out of range or written with a sign or leading zero, when a
 * range has prefix length 0 (it would trust every address), or when there
 * are more than %AXIS_PROXY_LIST_MAX_ENTRIES entries.  Host bits set in a
 * range are cleared ("192.168.1.10/24" is 192.168.1.0/24); a name is
 * lower-cased.
 *
 * A name trusts nothing until a lookup has given it addresses: see
 * axis_proxy_list_name_result().
 *
 * Returns: (transfer full) (nullable): the list, or %NULL on error
 */
AxisProxyList *
axis_proxy_list_parse(
	const gchar  *text,
	GError      **error
);

void
axis_proxy_list_free(AxisProxyList *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisProxyList, axis_proxy_list_free)

guint
axis_proxy_list_get_length(const AxisProxyList *self);

/**
 * axis_proxy_list_to_string:
 *
 * Returns: (transfer full): the entries in canonical form, joined by
 *   ", "; a single address has no prefix length; "" for an empty list
 */
gchar *
axis_proxy_list_to_string(const AxisProxyList *self);

/**
 * axis_proxy_list_contains:
 * @self: (nullable): a list; %NULL is the empty list
 * @address: an address
 *
 * Returns: %TRUE if @address is one of the entries or inside one of the
 *   ranges (an IPv4 address never matches an IPv6 entry, or the reverse)
 */
gboolean
axis_proxy_list_contains(
	const AxisProxyList *self,
	const AxisIpAddress *address
);

/*
 * Host names.  The list only keeps what lookups found: it never looks
 * anything up itself (axis-proxy-resolver.h does that), so it stays pure
 * and every call is quick.  The functions below are safe to call from any
 * thread, and so is axis_proxy_list_contains().
 */

typedef enum
{
	AXIS_PROXY_NAME_PENDING,	/* no lookup has finished yet; trusts nothing */
	AXIS_PROXY_NAME_RESOLVED,	/* the last lookup worked */
	AXIS_PROXY_NAME_KEPT,		/* the last lookup failed; the earlier addresses still stand */
	AXIS_PROXY_NAME_FAILED		/* no addresses are trusted */
} AxisProxyNameState;

typedef struct
{
	gchar              *name;		/* the entry as written (a name in lower case) */
	gboolean            is_name;		/* a host name; else an address or a range, always RESOLVED */
	AxisProxyNameState  state;
	gchar             **addresses;		/* NULL-terminated; the trusted ones, in address order */
	gboolean            capped;		/* the lookup gave more than AXIS_PROXY_NAME_MAX_ADDRESSES */
	gint64              age_us;		/* since the last good lookup; -1 if none yet */
	gint64              grace_left_us;	/* for KEPT: how long the addresses still stand */
	gchar              *error;		/* why the last lookup failed, or NULL */
} AxisProxyNameInfo;

void
axis_proxy_name_info_free(AxisProxyNameInfo *info);

/** The time, in microseconds, that names are aged by (monotonic). */
typedef gint64 (*AxisProxyClock)(gpointer user_data);

/** Replaces the clock; for tests.  %NULL restores g_get_monotonic_time(). */
void
axis_proxy_list_set_clock(
	AxisProxyList  *self,
	AxisProxyClock  clock,
	gpointer        user_data
);

gint64
axis_proxy_list_now(AxisProxyList *self);

/** Returns: whether any entry is a host name */
gboolean
axis_proxy_list_has_names(const AxisProxyList *self);

/** Returns: (transfer full): the distinct names, lower case, NULL-terminated */
gchar **
axis_proxy_list_get_names(const AxisProxyList *self);

/**
 * axis_proxy_list_name_result:
 * @name: a name of the list, lower case
 * @addresses: (nullable): what the lookup gave
 * @n_addresses: how many; 0 is a failure ("it has no addresses")
 * @error: (nullable): why the lookup failed (when @addresses is %NULL)
 * @change: (out) (nullable): a sentence about what changed, for the log
 *
 * Records a lookup.  A good one replaces the name's addresses (sorted,
 * IPv4 before IPv6, duplicates removed, at most
 * %AXIS_PROXY_NAME_MAX_ADDRESSES kept).  A failed one drops them at once
 * if there were none, else keeps them for %AXIS_PROXY_NAME_GRACE_US from
 * the first failure and then drops them.
 *
 * Returns: %TRUE if the name's state or addresses changed, which is when
 *   it is worth a log line; %FALSE for a lookup that said what the last
 *   one did
 */
gboolean
axis_proxy_list_name_result(
	AxisProxyList       *self,
	const gchar         *name,
	const AxisIpAddress *addresses,
	guint                n_addresses,
	const gchar         *error,
	gchar              **change
);

/**
 * axis_proxy_list_describe_entries:
 *
 * Returns: (transfer full) (element-type AxisProxyNameInfo): one for each
 *   entry, in list order; an address or range is its own one "address"
 *   and never ages
 */
GPtrArray *
axis_proxy_list_describe_entries(AxisProxyList *self);

/**
 * axis_proxy_resolve:
 * @trusted: (nullable): the trusted proxies; %NULL for none
 * @peer: (nullable): the directly connected peer's address, as text
 * @peer_https: whether the direct connection itself is TLS
 * @forwarded_for: (nullable): every X-Forwarded-For value, joined by ","
 * @forwarded_proto: (nullable): every X-Forwarded-Proto value, joined
 *   by ","
 * @client: (out) (transfer full): the client's address in canonical
 *   form, or %AXIS_PROXY_UNKNOWN_CLIENT when @peer is not an address
 * @https: (out): whether the client's request arrived over HTTPS
 *
 * When @peer is not a trusted proxy, both headers are ignored: the client
 * is @peer and @https is @peer_https.  When it is, the client is the
 * rightmost X-Forwarded-For entry that is not a trusted proxy (stopping
 * at the first entry that is not an address, or after
 * %AXIS_PROXY_MAX_HOPS, at the last address read), and @https follows the
 * rightmost X-Forwarded-Proto value: "https" (any case) is HTTPS, "http"
 * (any case) is not, and any other value, or no header at all, keeps
 * @peer_https.
 */
void
axis_proxy_resolve(
	const AxisProxyList  *trusted,
	const gchar          *peer,
	gboolean              peer_https,
	const gchar          *forwarded_for,
	const gchar          *forwarded_proto,
	gchar               **client,
	gboolean             *https
);

/* The longest host accepted from X-Forwarded-Host, port included. */
#define AXIS_PROXY_HOST_MAX_LEN 255

/**
 * axis_proxy_host_is_valid:
 * @text: (nullable): a host as a Host header carries it
 *
 * Accepts exactly a name of ASCII letters, digits, '.', '-' and '_', or an
 * IPv6 address in square brackets, either followed by ":port" with a port
 * from 1 to 65535 written without a leading zero; at most
 * %AXIS_PROXY_HOST_MAX_LEN characters.  No space, no comma, no userinfo,
 * no path.
 *
 * Returns: %TRUE if @text is such a host
 */
gboolean
axis_proxy_host_is_valid(const gchar *text);

/**
 * axis_proxy_resolve_full:
 * @forwarded_host: (nullable): every X-Forwarded-Host value, joined by ","
 * @host: (out) (transfer full) (nullable): the public host the trusted
 *   proxy reports, or %NULL when the request's own Host header stands
 *
 * As axis_proxy_resolve(), and also the host: when @peer is a trusted proxy
 * and the rightmost X-Forwarded-Host value is a host by
 * axis_proxy_host_is_valid(), @host is that value lower-cased; in every
 * other case (untrusted peer, no header, an empty or malformed value) it is
 * %NULL.
 */
void
axis_proxy_resolve_full(
	const AxisProxyList  *trusted,
	const gchar          *peer,
	gboolean              peer_https,
	const gchar          *forwarded_for,
	const gchar          *forwarded_proto,
	const gchar          *forwarded_host,
	gchar               **client,
	gboolean             *https,
	gchar               **host
);

G_END_DECLS

#endif /* AXIS_PROXY_H */
