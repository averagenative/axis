/*
 * axis-outbound.c - tying the daemon's outbound connections to one network
 * interface, and failing closed when it is not there
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-outbound.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static GMutex             lock;
static AxisOutboundTarget configured;		/* (lock) off until configured */
static AxisOutboundScope  configured_scope;	/* (lock) */
static AxisOutboundState  last;			/* (lock) the last state read */
static gboolean           have_last;		/* (lock) */
static guint              watch_id;		/* the default main context's */

/* ── Pure parts ─────────────────────────────────────────────────── */

gboolean
axis_outbound_traffic_is_internet(AxisOutboundTraffic traffic)
{
	return traffic == AXIS_OUTBOUND_TRAFFIC_UNNAMED || traffic == AXIS_OUTBOUND_TRAFFIC_INDEXER ||
		traffic == AXIS_OUTBOUND_TRAFFIC_METADATA || traffic == AXIS_OUTBOUND_TRAFFIC_DEFINITIONS;
}

gboolean
axis_outbound_applies(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope,
	AxisOutboundTraffic       traffic,
	AxisOutboundDestination   destination
){
	if (target == NULL || target->kind == AXIS_OUTBOUND_OFF || destination == AXIS_OUTBOUND_TO_LOOPBACK)
		return FALSE;
	if (scope == AXIS_OUTBOUND_SCOPE_ALL)
		return TRUE;
	return destination == AXIS_OUTBOUND_TO_PUBLIC && axis_outbound_traffic_is_internet(traffic);
}

/* An IPv6 address a connection to the internet can come from: not
 * link-local, loopback, multicast or unspecified. */
static gboolean
ipv6_bindable(const guint8 *a)
{
	static const guint8 zeros[16] = { 0 };

	if (a[0] == 0xff || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80))
		return FALSE;
	if (memcmp(a, zeros, 15) == 0 && (a[15] == 0 || a[15] == 1))
		return FALSE;
	return TRUE;
}

static gboolean
is_up(guint flags)
{
	return (flags & IFF_UP) != 0 && (flags & IFF_RUNNING) != 0;
}

static void
resolve_interface(
	const AxisOutboundTarget *target,
	const AxisOutboundEntry  *entries,
	guint                     n_entries,
	AxisOutboundState        *state
){
	gboolean found = FALSE;
	guint flags = 0;
	guint i;

	for (i = 0; i < n_entries; i++) {
		const AxisOutboundEntry *entry = &entries[i];

		if (strcmp(entry->name, target->text) != 0)
			continue;
		found = TRUE;
		flags |= entry->flags;
		if (entry->family == AF_INET && !state->has_ipv4) {
			state->has_ipv4 = TRUE;
			memcpy(state->ipv4, entry->address, 4);
		} else if (entry->family == AF_INET6 && !state->has_ipv6 && ipv6_bindable(entry->address)) {
			state->has_ipv6 = TRUE;
			memcpy(state->ipv6, entry->address, 16);
		}
	}
	if (!found) {
		state->condition = AXIS_OUTBOUND_STATE_MISSING;
		return;
	}
	g_strlcpy(state->device, target->text, sizeof(state->device));
	if (!is_up(flags))
		state->condition = AXIS_OUTBOUND_STATE_DOWN;
	else if (!state->has_ipv4 && !state->has_ipv6)
		state->condition = AXIS_OUTBOUND_STATE_NO_ADDRESS;
	else
		state->condition = AXIS_OUTBOUND_STATE_UP;
}

static void
resolve_address(
	const AxisOutboundTarget *target,
	const AxisOutboundEntry  *entries,
	guint                     n_entries,
	AxisOutboundState        *state
){
	gsize length = target->family == AF_INET ? 4 : 16;
	guint i;

	for (i = 0; i < n_entries; i++) {
		const AxisOutboundEntry *entry = &entries[i];

		if (entry->family != target->family || memcmp(entry->address, target->address, length) != 0)
			continue;
		g_strlcpy(state->device, entry->name, sizeof(state->device));
		if (target->family == AF_INET) {
			state->has_ipv4 = TRUE;
			memcpy(state->ipv4, target->address, 4);
		} else {
			state->has_ipv6 = TRUE;
			memcpy(state->ipv6, target->address, 16);
		}
		state->condition = is_up(entry->flags) ? AXIS_OUTBOUND_STATE_UP : AXIS_OUTBOUND_STATE_DOWN;
		return;
	}
	state->condition = AXIS_OUTBOUND_STATE_MISSING;
}

void
axis_outbound_resolve(
	const AxisOutboundTarget *target,
	const AxisOutboundEntry  *entries,
	guint                     n_entries,
	AxisOutboundState        *state
){
	g_return_if_fail(target != NULL && state != NULL);

	memset(state, 0, sizeof(*state));
	switch (target->kind) {
	case AXIS_OUTBOUND_INTERFACE:
		resolve_interface(target, entries, n_entries, state);
		break;
	case AXIS_OUTBOUND_ADDRESS:
		resolve_address(target, entries, n_entries, state);
		break;
	case AXIS_OUTBOUND_OFF:
	default:
		state->condition = AXIS_OUTBOUND_STATE_OFF;
		break;
	}
}

static const gchar *
family_name(gint family)
{
	return family == AF_INET6 ? "IPv6" : "IPv4";
}

gchar *
axis_outbound_describe_problem(
	const AxisOutboundTarget *target,
	const AxisOutboundState  *state
){
	gboolean address = target->kind == AXIS_OUTBOUND_ADDRESS;

	switch (state->condition) {
	case AXIS_OUTBOUND_STATE_MISSING:
		return address ? g_strdup_printf("no interface has the outbound address %s", target->text) :
			g_strdup_printf("the outbound interface %s does not exist", target->text);
	case AXIS_OUTBOUND_STATE_DOWN:
		return address ? g_strdup_printf("the outbound address %s is on %s, which is down", target->text,
				state->device) :
			g_strdup_printf("the outbound interface %s is down", target->text);
	case AXIS_OUTBOUND_STATE_NO_ADDRESS:
		return g_strdup_printf("the outbound interface %s has no address", target->text);
	case AXIS_OUTBOUND_STATE_OFF:
	case AXIS_OUTBOUND_STATE_UP:
	default:
		return NULL;
	}
}

AxisOutboundVerdict
axis_outbound_decide(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope,
	AxisOutboundTraffic       traffic,
	AxisOutboundDestination   destination,
	gint                      family,
	const AxisOutboundState  *state,
	gchar                   **message
){
	if (message != NULL)
		*message = NULL;
	if (!axis_outbound_applies(target, scope, traffic, destination))
		return AXIS_OUTBOUND_PASS;
	if (state == NULL || state->condition != AXIS_OUTBOUND_STATE_UP) {
		if (message != NULL) {
			*message = state != NULL ? axis_outbound_describe_problem(target, state) : NULL;
			if (*message == NULL)
				*message = g_strdup_printf("the state of the outbound interface %s is not known", target->text);
		}
		return AXIS_OUTBOUND_REFUSE;
	}
	if ((family == AF_INET && state->has_ipv4) || (family == AF_INET6 && state->has_ipv6))
		return AXIS_OUTBOUND_BIND;
	if (message != NULL) {
		if (target->kind == AXIS_OUTBOUND_ADDRESS)
			*message = g_strdup_printf("the outbound address %s is %s, and this connection is %s", target->text,
				family_name(target->family), family_name(family));
		else
			*message = g_strdup_printf("the outbound interface %s has no %s address", target->text,
				family_name(family));
	}
	return AXIS_OUTBOUND_REFUSE_FAMILY;
}

gchar *
axis_outbound_describe_addresses(const AxisOutboundState *state)
{
	GString *text = g_string_new(NULL);
	gchar buffer[INET6_ADDRSTRLEN];

	if (state->has_ipv4 && inet_ntop(AF_INET, state->ipv4, buffer, sizeof(buffer)) != NULL)
		g_string_append(text, buffer);
	if (state->has_ipv6 && inet_ntop(AF_INET6, state->ipv6, buffer, sizeof(buffer)) != NULL)
		g_string_append_printf(text, "%s%s", text->len > 0 ? ", " : "", buffer);
	if (text->len == 0)
		g_string_append(text, "none");
	return g_string_free(text, FALSE);
}

gboolean
axis_outbound_state_equal(
	const AxisOutboundState *a,
	const AxisOutboundState *b
){
	if (a->condition != b->condition || strcmp(a->device, b->device) != 0)
		return FALSE;
	if (a->has_ipv4 != b->has_ipv4 || (a->has_ipv4 && memcmp(a->ipv4, b->ipv4, 4) != 0))
		return FALSE;
	if (a->has_ipv6 != b->has_ipv6 || (a->has_ipv6 && memcmp(a->ipv6, b->ipv6, 16) != 0))
		return FALSE;
	return TRUE;
}

/* ── The system ─────────────────────────────────────────────────── */

GArray *
axis_outbound_read_entries(void)
{
	GArray *entries = g_array_new(FALSE, TRUE, sizeof(AxisOutboundEntry));
	struct ifaddrs *list = NULL;
	struct ifaddrs *ifa;

	if (getifaddrs(&list) != 0)
		return entries;
	for (ifa = list; ifa != NULL && entries->len < AXIS_OUTBOUND_MAX_ENTRIES; ifa = ifa->ifa_next) {
		AxisOutboundEntry entry;
		gchar *colon;

		if (ifa->ifa_name == NULL)
			continue;
		memset(&entry, 0, sizeof(entry));
		g_strlcpy(entry.name, ifa->ifa_name, sizeof(entry.name));
		/* An IPv4 address's label ("eth0:1") is its device's name before
		 * the colon: a device name has none. */
		if ((colon = strchr(entry.name, ':')) != NULL)
			*colon = '\0';
		entry.flags = ifa->ifa_flags;
		if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET) {
			entry.family = AF_INET;
			memcpy(entry.address, &((const struct sockaddr_in *)(gconstpointer)ifa->ifa_addr)->sin_addr, 4);
		} else if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET6) {
			entry.family = AF_INET6;
			memcpy(entry.address, &((const struct sockaddr_in6 *)(gconstpointer)ifa->ifa_addr)->sin6_addr, 16);
		} else {
			entry.family = 0;	/* the link (AF_PACKET), or an address of another kind */
		}
		g_array_append_val(entries, entry);
	}
	freeifaddrs(list);
	return entries;
}

gboolean
axis_outbound_bind_socket(
	gint                     fd,
	gint                     family,
	const AxisOutboundState *state,
	gchar                  **message
){
	gchar address[INET6_ADDRSTRLEN] = "";
	gint result;
	gint saved;

	g_return_val_if_fail(state != NULL, FALSE);

	if (message != NULL)
		*message = NULL;
	if (state->condition != AXIS_OUTBOUND_STATE_UP || state->device[0] == '\0' ||
	    (family == AF_INET && !state->has_ipv4) || (family == AF_INET6 && !state->has_ipv6) ||
	    (family != AF_INET && family != AF_INET6)) {
		if (message != NULL)
			*message = g_strdup("the outbound interface cannot take this connection");
		return FALSE;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, state->device, (socklen_t)strlen(state->device)) != 0) {
		saved = errno;
		if (message != NULL && saved == EPERM)
			*message = g_strdup_printf("the system refused to tie the connection to the outbound interface %s (%s): "
				"that needs Linux 5.7 or later, or the CAP_NET_RAW capability", state->device, g_strerror(saved));
		else if (message != NULL)
			*message = g_strdup_printf("the connection could not be tied to the outbound interface %s (%s)",
				state->device, g_strerror(saved));
		return FALSE;
	}
#ifdef IP_BIND_ADDRESS_NO_PORT
	{
		/* The port is chosen at connect(), not at bind(): many connections
		 * from one address do not run out of ports. */
		gint one = 1;

		(void)setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one));
	}
#endif
	if (family == AF_INET) {
		struct sockaddr_in sin;

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		memcpy(&sin.sin_addr, state->ipv4, 4);
		inet_ntop(AF_INET, state->ipv4, address, sizeof(address));
		result = bind(fd, (const struct sockaddr *)(gconstpointer)&sin, sizeof(sin));
	} else {
		struct sockaddr_in6 sin6;

		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		memcpy(&sin6.sin6_addr, state->ipv6, 16);
		inet_ntop(AF_INET6, state->ipv6, address, sizeof(address));
		result = bind(fd, (const struct sockaddr *)(gconstpointer)&sin6, sizeof(sin6));
	}
	if (result != 0) {
		saved = errno;
		if (message != NULL)
			*message = g_strdup_printf("the outbound address %s could not be used (%s)", address, g_strerror(saved));
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_outbound_device_binding_check(gchar **message)
{
	gint fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	gint saved;

	if (message != NULL)
		*message = NULL;
	if (fd < 0) {
		saved = errno;
		if (message != NULL)
			*message = g_strdup_printf("a socket could not be made to try it (%s)", g_strerror(saved));
		return FALSE;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 2) != 0) {
		saved = errno;
		close(fd);
		if (message != NULL)
			*message = saved == EPERM ?
				g_strdup_printf("the system refuses it (%s): it needs Linux 5.7 or later, or the CAP_NET_RAW "
					"capability", g_strerror(saved)) :
				g_strdup_printf("the system refuses it (%s)", g_strerror(saved));
		return FALSE;
	}
	close(fd);
	return TRUE;
}

/* ── The daemon's binding ───────────────────────────────────────── */

/* What the scope binds, for the log. */
static const gchar *
scope_phrase(AxisOutboundScope scope)
{
	return scope == AXIS_OUTBOUND_SCOPE_ALL ?
		"connections to indexers, metadata providers, download clients and media servers, except on this machine," :
		"connections to indexers and metadata providers at public addresses";
}

static void
log_state(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope,
	const AxisOutboundState  *state
){
	g_autofree gchar *problem = axis_outbound_describe_problem(target, state);
	g_autofree gchar *addresses = axis_outbound_describe_addresses(state);

	if (state->condition == AXIS_OUTBOUND_STATE_UP)
		g_message("outbound connections: %s is up with %s; %s go out through it only", state->device, addresses,
			scope_phrase(scope));
	else if (problem != NULL)
		g_warning("outbound connections: %s; %s are refused until it is up, and nothing is sent another way",
			problem, scope_phrase(scope));
}

void
axis_outbound_configure(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope
){
	AxisOutboundState state;
	gboolean on;

	g_mutex_lock(&lock);
	if (target != NULL && target->kind != AXIS_OUTBOUND_OFF)
		configured = *target;
	else
		memset(&configured, 0, sizeof(configured));
	configured_scope = scope;
	have_last = FALSE;
	memset(&last, 0, sizeof(last));
	on = configured.kind != AXIS_OUTBOUND_OFF;
	g_mutex_unlock(&lock);

	if (on)
		axis_outbound_read(&state);
}

void
axis_outbound_get_config(
	AxisOutboundTarget *target,
	AxisOutboundScope  *scope
){
	g_mutex_lock(&lock);
	if (target != NULL)
		*target = configured;
	if (scope != NULL)
		*scope = configured_scope;
	g_mutex_unlock(&lock);
}

gboolean
axis_outbound_may_apply(AxisOutboundTraffic traffic)
{
	gboolean may;

	g_mutex_lock(&lock);
	may = configured.kind != AXIS_OUTBOUND_OFF &&
		(configured_scope == AXIS_OUTBOUND_SCOPE_ALL || axis_outbound_traffic_is_internet(traffic));
	g_mutex_unlock(&lock);
	return may;
}

void
axis_outbound_read(AxisOutboundState *state)
{
	AxisOutboundTarget target;
	AxisOutboundScope scope;
	GArray *entries;
	gboolean changed;

	g_return_if_fail(state != NULL);

	axis_outbound_get_config(&target, &scope);
	if (target.kind == AXIS_OUTBOUND_OFF) {
		memset(state, 0, sizeof(*state));
		state->condition = AXIS_OUTBOUND_STATE_OFF;
		return;
	}
	entries = axis_outbound_read_entries();
	axis_outbound_resolve(&target, (const AxisOutboundEntry *)(gconstpointer)entries->data, entries->len, state);
	g_array_unref(entries);

	g_mutex_lock(&lock);
	/* Logged once per change, and not when the binding changed meanwhile. */
	changed = memcmp(&configured, &target, sizeof(target)) == 0 &&
		(!have_last || !axis_outbound_state_equal(&last, state));
	if (changed) {
		last = *state;
		have_last = TRUE;
	}
	g_mutex_unlock(&lock);
	if (changed)
		log_state(&target, scope, state);
}

static gboolean
on_watch(gpointer user_data)
{
	AxisOutboundState state;

	axis_outbound_read(&state);
	return G_SOURCE_CONTINUE;
}

void
axis_outbound_watch_start(void)
{
	AxisOutboundTarget target;

	axis_outbound_get_config(&target, NULL);
	if (watch_id != 0 || target.kind == AXIS_OUTBOUND_OFF)
		return;
	watch_id = g_timeout_add_seconds(AXIS_OUTBOUND_WATCH_SECONDS, on_watch, NULL);
}

void
axis_outbound_watch_stop(void)
{
	if (watch_id != 0) {
		g_source_remove(watch_id);
		watch_id = 0;
	}
}
