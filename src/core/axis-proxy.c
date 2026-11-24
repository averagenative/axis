/*
 * axis-proxy.c - trusted reverse proxies and forwarding headers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-proxy.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* Longer than any address's text form, brackets included. */
#define ADDRESS_TEXT_MAX 64

/* Longest list text accepted at all. */
#define LIST_TEXT_MAX 8192

G_DEFINE_QUARK(axis-proxy-error-quark, axis_proxy_error)

/* One entry of the list: an address or range, or a host name with what its
 * lookups found. */
typedef struct
{
	AxisIpAddress address;
	guint         prefix;	/* 1..32 or 1..128 */

	gchar        *name;	/* a host name; NULL for an address or range */
	GArray       *found;	/* of AxisIpAddress: the trusted addresses, sorted */
	gboolean      capped;
	gboolean      looked;	/* a lookup has finished */
	gint64        good_at;	/* when the last good lookup finished */
	gint64        failed_at;	/* when lookups began failing; 0 while they work */
	gchar        *error;
	AxisProxyNameState reported;	/* the state the last lookup's caller was told of */
} Range;

static void
range_clear(gpointer data)
{
	Range *range = data;

	g_free(range->name);
	g_free(range->error);
	if (range->found != NULL)
		g_array_free(range->found, TRUE);
}

struct _AxisProxyList
{
	GArray         *ranges;	/* of Range, in the order written */
	guint           names;	/* how many of them are names */
	GMutex          lock;	/* guards the names' fields */
	AxisProxyClock  clock;
	gpointer        clock_data;
};

static const guint8 v4_mapped_prefix[12] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
};

gboolean
axis_ip_address_parse(
	const gchar   *text,
	AxisIpAddress *address
){
	gchar buffer[ADDRESS_TEXT_MAX];
	AxisIpAddress parsed;
	gsize len;
	const gchar *start;

	if (text == NULL)
		return FALSE;

	len = strlen(text);
	start = text;
	if (len >= 2 && text[0] == '[' && text[len - 1] == ']') {
		start = text + 1;
		len -= 2;
	}
	if (len == 0 || len >= sizeof(buffer))
		return FALSE;
	memcpy(buffer, start, len);
	buffer[len] = '\0';

	memset(&parsed, 0, sizeof(parsed));
	if (start == text && inet_pton(AF_INET, buffer, parsed.bytes) == 1) {
		parsed.family = 4;
	} else if (inet_pton(AF_INET6, buffer, parsed.bytes) == 1) {
		parsed.family = 6;
		if (memcmp(parsed.bytes, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0) {
			memmove(parsed.bytes, parsed.bytes + 12, 4);
			memset(parsed.bytes + 4, 0, 12);
			parsed.family = 4;
		}
	} else {
		return FALSE;
	}

	if (address != NULL)
		*address = parsed;
	return TRUE;
}

gchar *
axis_ip_address_to_string(const AxisIpAddress *address)
{
	gchar buffer[INET6_ADDRSTRLEN];

	g_return_val_if_fail(address != NULL, NULL);

	if (inet_ntop(address->family == 4 ? AF_INET : AF_INET6, address->bytes,
	              buffer, sizeof(buffer)) == NULL)
		return g_strdup(AXIS_PROXY_UNKNOWN_CLIENT);

	return g_strdup(buffer);
}

gboolean
axis_ip_address_is_loopback(const AxisIpAddress *address)
{
	static const guint8 loopback6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	g_return_val_if_fail(address != NULL, FALSE);

	if (address->family == 4)
		return address->bytes[0] == 127;
	return memcmp(address->bytes, loopback6, 16) == 0;
}

gboolean
axis_ip_address_is_private(const AxisIpAddress *address)
{
	const guint8 *b;

	g_return_val_if_fail(address != NULL, FALSE);

	b = address->bytes;
	if (axis_ip_address_is_loopback(address))
		return TRUE;
	if (address->family == 4)
		return b[0] == 10 || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168) ||
		       (b[0] == 169 && b[1] == 254);
	return (b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80);
}

gchar *
axis_proxy_address_key(const gchar *text)
{
	AxisIpAddress address;
	g_autofree gchar *canonical = NULL;

	if (text == NULL || !axis_ip_address_parse(text, &address))
		return g_strdup(text != NULL ? text : AXIS_PROXY_UNKNOWN_CLIENT);
	if (address.family == 4)
		return axis_ip_address_to_string(&address);

	memset(address.bytes + 8, 0, 8);
	canonical = axis_ip_address_to_string(&address);
	return g_strconcat(canonical, "/64", NULL);
}

static guint
family_bits(guint8 family)
{
	return family == 4 ? 32 : 128;
}

/* Clears every bit after the first @prefix. */
static void
mask_address(
	AxisIpAddress *address,
	guint          prefix
){
	guint i;

	for (i = 0; i < 16; i++) {
		guint first_bit = i * 8;

		if (first_bit >= prefix)
			address->bytes[i] = 0;
		else if (first_bit + 8 > prefix)
			address->bytes[i] &= (guint8)(0xff << (8 - (prefix - first_bit)));
	}
}

/* Digits only, no sign, no leading zero, at most @max. */
static gboolean
parse_prefix(
	const gchar *text,
	guint        max,
	guint       *prefix
){
	guint value = 0;
	const gchar *p;

	if (text[0] == '\0' || (text[0] == '0' && text[1] != '\0'))
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return FALSE;
		value = value * 10 + (guint)(*p - '0');
		if (value > max)
			return FALSE;
	}

	*prefix = value;
	return TRUE;
}

gboolean
axis_proxy_name_is_valid(const gchar *text)
{
	gsize len;
	gsize label_start = 0;
	gsize i;
	gboolean all_digits = TRUE;

	if (text == NULL)
		return FALSE;
	len = strlen(text);
	if (len == 0 || len > AXIS_PROXY_NAME_MAX_LEN)
		return FALSE;

	for (i = 0; i <= len; i++) {
		gchar c = text[i];

		if (c == '.' || c == '\0') {
			gsize label_len = i - label_start;

			if (label_len == 0 || label_len > 63 || text[label_start] == '-' || text[i - 1] == '-')
				return FALSE;
			if (c == '\0')
				return !all_digits;
			label_start = i + 1;
			all_digits = TRUE;
			continue;
		}
		if (!g_ascii_isalnum(c) && c != '-' && c != '_')
			return FALSE;
		if (!g_ascii_isdigit(c))
			all_digits = FALSE;
	}

	return FALSE;
}

static gboolean
parse_range(
	const gchar  *entry,
	Range        *range,
	GError      **error
){
	g_autofree gchar *address_text = NULL;
	const gchar *slash = strchr(entry, '/');
	guint prefix;

	address_text = slash != NULL ? g_strndup(entry, (gsize)(slash - entry)) : g_strdup(entry);
	if (slash == NULL && !axis_ip_address_parse(address_text, NULL) && axis_proxy_name_is_valid(entry)) {
		range->name = g_ascii_strdown(entry, -1);
		range->found = g_array_new(FALSE, FALSE, sizeof(AxisIpAddress));
		return TRUE;
	}
	if (!axis_ip_address_parse(address_text, &range->address) ||
	    (slash != NULL && strchr(address_text, '[') != NULL)) {
		g_set_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
			"invalid entry '%s' (expected an IP address, a CIDR range such as "
			"192.168.1.0/24, or a host name such as traefik)", entry);
		return FALSE;
	}

	prefix = family_bits(range->address.family);
	if (slash != NULL) {
		if (!parse_prefix(slash + 1, family_bits(range->address.family), &prefix)) {
			g_set_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
				"invalid entry '%s' (the prefix length must be a number from 1 to %u)",
				entry, family_bits(range->address.family));
			return FALSE;
		}
		if (prefix == 0) {
			g_set_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
				"invalid entry '%s' (a prefix length of 0 would trust every address)",
				entry);
			return FALSE;
		}
	}

	mask_address(&range->address, prefix);
	range->prefix = prefix;
	return TRUE;
}

AxisProxyList *
axis_proxy_list_parse(
	const gchar  *text,
	GError      **error
){
	g_autoptr(AxisProxyList) list = NULL;
	g_auto(GStrv) entries = NULL;
	g_autofree gchar *copy = NULL;
	guint i;

	g_return_val_if_fail(text != NULL, NULL);

	list = g_new0(AxisProxyList, 1);
	list->ranges = g_array_new(FALSE, TRUE, sizeof(Range));
	g_array_set_clear_func(list->ranges, range_clear);
	g_mutex_init(&list->lock);

	if (strlen(text) > LIST_TEXT_MAX) {
		g_set_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
			"the list is longer than %d characters", LIST_TEXT_MAX);
		return NULL;
	}

	copy = g_strstrip(g_strdup(text));
	if (copy[0] == '\0')
		return g_steal_pointer(&list);

	entries = g_strsplit(copy, ",", -1);
	for (i = 0; entries[i] != NULL; i++) {
		gchar *entry = g_strstrip(entries[i]);
		Range range;

		if (entry[0] == '\0') {
			g_set_error_literal(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
				"empty entry (entries are separated by single commas)");
			return NULL;
		}
		if (i >= AXIS_PROXY_LIST_MAX_ENTRIES) {
			g_set_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
				"more than %d entries", AXIS_PROXY_LIST_MAX_ENTRIES);
			return NULL;
		}

		memset(&range, 0, sizeof(range));
		if (!parse_range(entry, &range, error))
			return NULL;
		if (range.name != NULL)
			list->names++;
		g_array_append_val(list->ranges, range);
	}

	return g_steal_pointer(&list);
}

void
axis_proxy_list_free(AxisProxyList *self)
{
	if (self == NULL)
		return;

	g_array_free(self->ranges, TRUE);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_proxy_name_info_free(AxisProxyNameInfo *info)
{
	if (info == NULL)
		return;

	g_free(info->name);
	g_strfreev(info->addresses);
	g_free(info->error);
	g_free(info);
}

void
axis_proxy_list_set_clock(
	AxisProxyList  *self,
	AxisProxyClock  clock,
	gpointer        user_data
){
	g_return_if_fail(self != NULL);

	self->clock = clock;
	self->clock_data = user_data;
}

gint64
axis_proxy_list_now(AxisProxyList *self)
{
	return self != NULL && self->clock != NULL ? self->clock(self->clock_data) : g_get_monotonic_time();
}

/*
 * A name's state at @now.  The addresses of a KEPT name stand until the
 * grace is over, whether or not another lookup has said so yet.
 */
static AxisProxyNameState
name_state(
	const Range *range,
	gint64       now
){
	if (!range->looked)
		return AXIS_PROXY_NAME_PENDING;
	if (range->failed_at == 0)
		return AXIS_PROXY_NAME_RESOLVED;
	if (range->found->len > 0 && now - range->failed_at < AXIS_PROXY_NAME_GRACE_US)
		return AXIS_PROXY_NAME_KEPT;
	return AXIS_PROXY_NAME_FAILED;
}

gboolean
axis_proxy_list_has_names(const AxisProxyList *self)
{
	return self != NULL && self->names > 0;
}

gchar **
axis_proxy_list_get_names(const AxisProxyList *self)
{
	GPtrArray *names = g_ptr_array_new();
	guint i;

	for (i = 0; self != NULL && i < self->ranges->len; i++) {
		const Range *range = &g_array_index(self->ranges, Range, i);
		guint j;
		gboolean seen = FALSE;

		if (range->name == NULL)
			continue;
		for (j = 0; j < names->len; j++)
			seen = seen || strcmp(g_ptr_array_index(names, j), range->name) == 0;
		if (!seen)
			g_ptr_array_add(names, g_strdup(range->name));
	}
	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(names, FALSE);
}

static gint
address_compare(
	gconstpointer a,
	gconstpointer b
){
	const AxisIpAddress *x = a;
	const AxisIpAddress *y = b;

	if (x->family != y->family)
		return (gint)x->family - (gint)y->family;
	return memcmp(x->bytes, y->bytes, sizeof(x->bytes));
}

static gboolean
addresses_equal(
	const GArray        *found,
	const AxisIpAddress *sorted,
	guint                n
){
	guint i;

	if (found->len != n)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (address_compare(&g_array_index(found, AxisIpAddress, i), &sorted[i]) != 0)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_proxy_list_name_result(
	AxisProxyList       *self,
	const gchar         *name,
	const AxisIpAddress *addresses,
	guint                n_addresses,
	const gchar         *error,
	gchar              **change
){
	gboolean changed = FALSE;
	gint64 now;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	if (change != NULL)
		*change = NULL;
	now = axis_proxy_list_now(self);

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->ranges->len; i++) {
		Range *range = &g_array_index(self->ranges, Range, i);
		AxisProxyNameState before;

		if (range->name == NULL || strcmp(range->name, name) != 0)
			continue;
		before = range->reported;

		if (addresses != NULL && n_addresses > 0) {
			g_autofree AxisIpAddress *sorted = g_memdup2(addresses, n_addresses * sizeof(*addresses));
			guint kept = 0;
			guint j;
			gboolean capped;
			gboolean same;

			qsort(sorted, n_addresses, sizeof(*sorted), address_compare);
			for (j = 0; j < n_addresses; j++) {
				if (kept > 0 && address_compare(&sorted[kept - 1], &sorted[j]) == 0)
					continue;
				sorted[kept++] = sorted[j];
			}
			capped = kept > AXIS_PROXY_NAME_MAX_ADDRESSES;
			if (capped)
				kept = AXIS_PROXY_NAME_MAX_ADDRESSES;

			same = before == AXIS_PROXY_NAME_RESOLVED && capped == range->capped &&
				addresses_equal(range->found, sorted, kept);
			g_array_set_size(range->found, 0);
			g_array_append_vals(range->found, sorted, kept);
			range->capped = capped;
			range->looked = TRUE;
			range->good_at = now;
			range->failed_at = 0;
			range->reported = AXIS_PROXY_NAME_RESOLVED;
			g_clear_pointer(&range->error, g_free);
			if (!same && !changed) {
				g_autofree gchar *list = NULL;
				GString *text = g_string_new(NULL);

				for (j = 0; j < kept; j++) {
					g_autofree gchar *one = axis_ip_address_to_string(&sorted[j]);

					g_string_append_printf(text, "%s%s", j > 0 ? ", " : "", one);
				}
				list = g_string_free(text, FALSE);
				if (change != NULL)
					*change = g_strdup_printf("trusted proxy name %s resolves to %s%s", name, list,
						capped ? " (more addresses were found; only the first "
							G_STRINGIFY(AXIS_PROXY_NAME_MAX_ADDRESSES) " are trusted)" : "");
			}
			changed = changed || !same;
		} else {
			const gchar *why = error != NULL ? error : "the name has no addresses";
			AxisProxyNameState after;

			if (range->failed_at == 0)
				range->failed_at = now;
			range->looked = TRUE;
			if (g_strcmp0(range->error, why) != 0) {
				g_free(range->error);
				range->error = g_strdup(why);
			}
			after = name_state(range, now);
			if (after == AXIS_PROXY_NAME_FAILED)
				g_array_set_size(range->found, 0);
			range->reported = after;
			if (after != before && !changed) {
				if (change != NULL)
					*change = after == AXIS_PROXY_NAME_KEPT ?
						g_strdup_printf("trusted proxy name %s does not resolve (%s); its earlier "
							"addresses are kept for %d seconds", name, why,
							(gint)(AXIS_PROXY_NAME_GRACE_US / G_USEC_PER_SEC)) :
						g_strdup_printf("trusted proxy name %s does not resolve (%s); no address is "
							"trusted for it", name, why);
			}
			changed = changed || after != before;
		}
	}
	g_mutex_unlock(&self->lock);

	return changed;
}

GPtrArray *
axis_proxy_list_describe_entries(AxisProxyList *self)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_proxy_name_info_free);
	gint64 now = axis_proxy_list_now(self);
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->ranges->len; i++) {
		const Range *range = &g_array_index(self->ranges, Range, i);
		AxisProxyNameInfo *info;
		guint j;

		info = g_new0(AxisProxyNameInfo, 1);
		if (range->name == NULL) {
			g_autofree gchar *text = axis_ip_address_to_string(&range->address);

			info->name = range->prefix != family_bits(range->address.family) ?
				g_strdup_printf("%s/%u", text, range->prefix) : g_strdup(text);
			info->state = AXIS_PROXY_NAME_RESOLVED;
			info->age_us = -1;
			info->addresses = g_new0(gchar *, 2);
			info->addresses[0] = g_strdup(info->name);
			g_ptr_array_add(out, info);
			continue;
		}
		info->name = g_strdup(range->name);
		info->is_name = TRUE;
		info->state = name_state(range, now);
		info->capped = range->capped;
		info->age_us = range->good_at != 0 ? now - range->good_at : -1;
		info->grace_left_us = info->state == AXIS_PROXY_NAME_KEPT ?
			AXIS_PROXY_NAME_GRACE_US - (now - range->failed_at) : 0;
		info->error = g_strdup(range->error);
		info->addresses = g_new0(gchar *, range->found->len + 1);
		if (info->state == AXIS_PROXY_NAME_RESOLVED || info->state == AXIS_PROXY_NAME_KEPT) {
			for (j = 0; j < range->found->len; j++)
				info->addresses[j] = axis_ip_address_to_string(&g_array_index(range->found,
					AxisIpAddress, j));
		}
		g_ptr_array_add(out, info);
	}
	g_mutex_unlock(&self->lock);

	return out;
}

guint
axis_proxy_list_get_length(const AxisProxyList *self)
{
	return self != NULL ? self->ranges->len : 0;
}

gchar *
axis_proxy_list_to_string(const AxisProxyList *self)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; self != NULL && i < self->ranges->len; i++) {
		const Range *range = &g_array_index(self->ranges, Range, i);
		g_autofree gchar *address = range->name == NULL ? axis_ip_address_to_string(&range->address) : NULL;

		if (i > 0)
			g_string_append(out, ", ");
		if (range->name != NULL) {
			g_string_append(out, range->name);
			continue;
		}
		g_string_append(out, address);
		if (range->prefix != family_bits(range->address.family))
			g_string_append_printf(out, "/%u", range->prefix);
	}

	return g_string_free(out, FALSE);
}

gboolean
axis_proxy_list_contains(
	const AxisProxyList *self,
	const AxisIpAddress *address
){
	AxisProxyList *list = (AxisProxyList *)self;
	gboolean found = FALSE;
	gint64 now = 0;
	guint i;

	g_return_val_if_fail(address != NULL, FALSE);

	if (self != NULL && self->names > 0) {
		now = axis_proxy_list_now(list);
		g_mutex_lock(&list->lock);
	}
	for (i = 0; !found && self != NULL && i < self->ranges->len; i++) {
		const Range *range = &g_array_index(self->ranges, Range, i);
		AxisIpAddress masked;
		guint j;

		if (range->name != NULL) {
			if (name_state(range, now) != AXIS_PROXY_NAME_RESOLVED &&
			    name_state(range, now) != AXIS_PROXY_NAME_KEPT)
				continue;
			for (j = 0; j < range->found->len; j++)
				found = found || address_compare(&g_array_index(range->found, AxisIpAddress, j),
					address) == 0;
			continue;
		}
		if (range->address.family != address->family)
			continue;
		masked = *address;
		mask_address(&masked, range->prefix);
		if (memcmp(masked.bytes, range->address.bytes, sizeof(masked.bytes)) == 0)
			found = TRUE;
	}
	if (self != NULL && self->names > 0)
		g_mutex_unlock(&list->lock);

	return found;
}

/* An address or range as it is written: "192.0.2.7", "10.0.0.0/8". */
static gchar *
range_text(const Range *range)
{
	g_autofree gchar *text = axis_ip_address_to_string(&range->address);

	return range->prefix != family_bits(range->address.family) ? g_strdup_printf("%s/%u", text, range->prefix) :
		g_strdup(text);
}

gboolean
axis_proxy_lists_overlap(
	const AxisProxyList  *self,
	const AxisProxyList  *other,
	gchar               **entry,
	gchar               **other_entry
){
	AxisProxyList *names = (AxisProxyList *)other;
	gboolean found = FALSE;
	gint64 now = 0;
	guint i;

	if (entry != NULL)
		*entry = NULL;
	if (other_entry != NULL)
		*other_entry = NULL;
	if (self == NULL || other == NULL)
		return FALSE;

	if (other->names > 0) {
		now = axis_proxy_list_now(names);
		g_mutex_lock(&names->lock);
	}
	for (i = 0; !found && i < self->ranges->len; i++) {
		const Range *mine = &g_array_index(self->ranges, Range, i);
		guint j;

		/* A name of @self stands for nothing here: see the header. */
		if (mine->name != NULL)
			continue;
		for (j = 0; !found && j < other->ranges->len; j++) {
			const Range *theirs = &g_array_index(other->ranges, Range, j);
			AxisIpAddress a;
			AxisIpAddress b;
			guint k;

			if (theirs->name != NULL) {
				if (name_state(theirs, now) != AXIS_PROXY_NAME_RESOLVED &&
				    name_state(theirs, now) != AXIS_PROXY_NAME_KEPT)
					continue;
				for (k = 0; !found && k < theirs->found->len; k++) {
					a = g_array_index(theirs->found, AxisIpAddress, k);
					if (a.family != mine->address.family)
						continue;
					mask_address(&a, mine->prefix);
					found = memcmp(a.bytes, mine->address.bytes, sizeof(a.bytes)) == 0;
				}
			} else if (theirs->address.family == mine->address.family) {
				/* Two ranges share an address exactly when the wider one
				 * holds the other's first. */
				a = mine->address;
				b = theirs->address;
				mask_address(&a, MIN(mine->prefix, theirs->prefix));
				mask_address(&b, MIN(mine->prefix, theirs->prefix));
				found = memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
			}
			if (found) {
				if (entry != NULL)
					*entry = range_text(mine);
				if (other_entry != NULL)
					*other_entry = theirs->name != NULL ? g_strdup(theirs->name) : range_text(theirs);
			}
		}
	}
	if (other->names > 0)
		g_mutex_unlock(&names->lock);

	return found;
}

/*
 * The entries of a comma-separated header value, trimmed of spaces and
 * tabs, in order.  Empty entries are kept (as ""), so that one is seen
 * as "not an address" rather than skipped.
 */
static gchar **
split_header(const gchar *value)
{
	gchar **items = g_strsplit(value, ",", -1);
	guint i;

	for (i = 0; items[i] != NULL; i++) {
		gchar *item = items[i];
		gsize len;

		while (*item == ' ' || *item == '\t')
			item++;
		len = strlen(item);
		while (len > 0 && (item[len - 1] == ' ' || item[len - 1] == '\t'))
			len--;
		memmove(items[i], item, len);
		items[i][len] = '\0';
	}

	return items;
}

/* A port: 1 to 65535, digits only, no leading zero. */
static gboolean
port_is_valid(const gchar *text)
{
	guint value = 0;
	const gchar *p;

	if (text[0] == '\0' || text[0] == '0')
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9' || p - text >= 5)
			return FALSE;
		value = value * 10 + (guint)(*p - '0');
	}
	return value >= 1 && value <= 65535;
}

gboolean
axis_proxy_host_is_valid(const gchar *text)
{
	gsize len;
	const gchar *port = NULL;
	const gchar *p;

	if (text == NULL)
		return FALSE;
	len = strlen(text);
	if (len == 0 || len > AXIS_PROXY_HOST_MAX_LEN)
		return FALSE;

	if (text[0] == '[') {
		const gchar *close = strchr(text, ']');
		g_autofree gchar *inside = NULL;
		AxisIpAddress address;

		if (close == NULL || close == text + 1)
			return FALSE;
		inside = g_strndup(text + 1, (gsize)(close - text - 1));
		/* An IPv6 address only: brackets around IPv4 are not a host. */
		if (strchr(inside, ':') == NULL || !axis_ip_address_parse(inside, &address))
			return FALSE;
		if (close[1] == '\0')
			return TRUE;
		return close[1] == ':' && port_is_valid(close + 2);
	}

	for (p = text; *p != '\0'; p++) {
		if (*p == ':') {
			if (port != NULL)
				return FALSE;
			port = p + 1;
			continue;
		}
		if (port != NULL)
			continue;
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-' && *p != '_')
			return FALSE;
	}
	if (text[0] == ':')
		return FALSE;

	return port == NULL || port_is_valid(port);
}

void
axis_proxy_resolve(
	const AxisProxyList  *trusted,
	const gchar          *peer,
	gboolean              peer_https,
	const gchar          *forwarded_for,
	const gchar          *forwarded_proto,
	gchar               **client,
	gboolean             *https
){
	axis_proxy_resolve_full(trusted, peer, peer_https, forwarded_for, forwarded_proto, NULL,
		client, https, NULL);
}

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
){
	AxisIpAddress current;
	gboolean secure = peer_https;

	g_return_if_fail(client != NULL);
	g_return_if_fail(https != NULL);

	if (host != NULL)
		*host = NULL;

	if (!axis_ip_address_parse(peer, &current)) {
		*client = g_strdup(AXIS_PROXY_UNKNOWN_CLIENT);
		*https = peer_https;
		return;
	}

	if (axis_proxy_list_contains(trusted, &current)) {
		if (forwarded_for != NULL) {
			g_auto(GStrv) hops = split_header(forwarded_for);
			guint n = g_strv_length(hops);
			guint seen = 0;

			while (n > 0 && seen < AXIS_PROXY_MAX_HOPS) {
				AxisIpAddress hop;

				n--;
				seen++;
				if (!axis_ip_address_parse(hops[n], &hop))
					break;
				current = hop;
				if (!axis_proxy_list_contains(trusted, &current))
					break;
			}
		}

		/* The nearest proxy's word, if it is one of the two; anything else
		 * says nothing, so the connection's own scheme stands. */
		if (forwarded_proto != NULL) {
			g_auto(GStrv) values = split_header(forwarded_proto);
			guint n = g_strv_length(values);

			if (n > 0 && g_ascii_strcasecmp(values[n - 1], "https") == 0)
				secure = TRUE;
			else if (n > 0 && g_ascii_strcasecmp(values[n - 1], "http") == 0)
				secure = FALSE;
		}

		if (forwarded_host != NULL && host != NULL) {
			g_auto(GStrv) values = split_header(forwarded_host);
			guint n = g_strv_length(values);

			if (n > 0 && axis_proxy_host_is_valid(values[n - 1]))
				*host = g_ascii_strdown(values[n - 1], -1);
		}
	}

	*client = axis_ip_address_to_string(&current);
	*https = secure;
}
