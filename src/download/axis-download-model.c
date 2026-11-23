/*
 * axis-download-model.c - states, protocols, priorities and checks
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-model.h"

#include <string.h>

#include "library/axis-library-name.h"

/* ── Names of things ────────────────────────────────────────────── */

static const gchar *const protocol_names[] = { "usenet", "torrent", "youtube", NULL };
static const gchar *const protocol_labels[] = { "Usenet", "Torrent", "YouTube", NULL };

static const gchar *const state_names[] = {
	"sending", "queued", "downloading", "paused", "completed", "failed", "gone", "removed", "imported", NULL
};
static const gchar *const state_labels[] = {
	"Sending", "Queued", "Downloading", "Paused", "Completed", "Failed", "Gone", "Removed", "Imported", NULL
};

static const gchar *const priority_names[] = { "low", "normal", "high", NULL };
static const gchar *const priority_labels[] = { "Low", "Normal", "High", NULL };

static const gchar *const client_state_names[] = { "unchecked", "ok", "unreachable", "refused", "failing", NULL };
static const gchar *const client_state_labels[] = {
	"Not checked yet", "Working", "Not answering", "Access refused", "Failing", NULL
};

static gboolean
lookup(
	const gchar *const *names,
	const gchar        *text,
	gint               *value
){
	gint i;

	for (i = 0; text != NULL && names[i] != NULL; i++) {
		if (strcmp(names[i], text) == 0) {
			*value = i;
			return TRUE;
		}
	}
	return FALSE;
}

static const gchar *
name_of(
	const gchar *const *names,
	guint               n,
	gint                value
){
	return value >= 0 && (guint)value < n ? names[value] : NULL;
}

const gchar *
axis_download_protocol_to_string(AxisDownloadProtocol protocol)
{
	return name_of(protocol_names, 3, (gint)protocol);
}

gboolean
axis_download_address_valid(const gchar *text)
{
	const gchar *p;
	gsize length;

	if (text == NULL || !g_str_has_prefix(text, "https://"))
		return FALSE;
	length = strlen(text);
	if (length <= strlen("https://") || length > AXIS_DOWNLOAD_ADDRESS_MAX)
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_download_protocol_from_string(
	const gchar          *text,
	AxisDownloadProtocol *protocol
){
	gint value;

	if (!lookup(protocol_names, text, &value))
		return FALSE;
	*protocol = (AxisDownloadProtocol)value;
	return TRUE;
}

const gchar *
axis_download_protocol_label(AxisDownloadProtocol protocol)
{
	return name_of(protocol_labels, 3, (gint)protocol);
}

const gchar *
axis_download_state_to_string(AxisDownloadState state)
{
	return name_of(state_names, AXIS_DOWNLOAD_N_STATES, (gint)state);
}

gboolean
axis_download_state_from_string(
	const gchar       *text,
	AxisDownloadState *state
){
	gint value;

	if (!lookup(state_names, text, &value))
		return FALSE;
	*state = (AxisDownloadState)value;
	return TRUE;
}

const gchar *
axis_download_state_label(AxisDownloadState state)
{
	return name_of(state_labels, AXIS_DOWNLOAD_N_STATES, (gint)state);
}

gboolean
axis_download_state_is_active(AxisDownloadState state)
{
	return state == AXIS_DOWNLOAD_SENDING || state == AXIS_DOWNLOAD_QUEUED || state == AXIS_DOWNLOAD_DOWNLOADING ||
		state == AXIS_DOWNLOAD_PAUSED;
}

const gchar *
axis_download_priority_to_string(AxisDownloadPriority priority)
{
	return name_of(priority_names, 3, (gint)priority);
}

gboolean
axis_download_priority_from_string(
	const gchar          *text,
	AxisDownloadPriority *priority
){
	gint value;

	if (!lookup(priority_names, text, &value))
		return FALSE;
	*priority = (AxisDownloadPriority)value;
	return TRUE;
}

const gchar *
axis_download_priority_label(AxisDownloadPriority priority)
{
	return name_of(priority_labels, 3, (gint)priority);
}

const gchar *
axis_download_client_state_to_string(AxisDownloadClientState state)
{
	return name_of(client_state_names, 5, (gint)state);
}

gboolean
axis_download_client_state_from_string(
	const gchar             *text,
	AxisDownloadClientState *state
){
	gint value;

	if (!lookup(client_state_names, text, &value))
		return FALSE;
	*state = (AxisDownloadClientState)value;
	return TRUE;
}

const gchar *
axis_download_client_state_label(AxisDownloadClientState state)
{
	return name_of(client_state_labels, 5, (gint)state);
}

/* ── Checks ─────────────────────────────────────────────────────── */

gchar *
axis_download_text_clean(
	const gchar *text,
	guint        max_chars
){
	g_autofree gchar *clean = NULL;
	GString *out;
	const gchar *p;
	gboolean space = FALSE;

	if (text == NULL)
		return NULL;
	/* Bounded before anything else looks at it. */
	clean = axis_library_text_clean(text, max_chars + 64);
	if (clean == NULL)
		return NULL;
	out = g_string_new(NULL);
	for (p = clean; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isspace(c)) {
			space = out->len > 0;
			continue;
		}
		if (space)
			g_string_append_c(out, ' ');
		space = FALSE;
		g_string_append_unichar(out, c);
	}
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	if (g_utf8_strlen(out->str, -1) > (glong)max_chars) {
		gchar *end = g_utf8_offset_to_pointer(out->str, (glong)max_chars);

		g_string_truncate(out, (gsize)(end - out->str));
		while (out->len > 0 && out->str[out->len - 1] == ' ')
			g_string_truncate(out, out->len - 1);
	}
	return g_string_free(out, FALSE);
}

gboolean
axis_download_category_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n == 0 || n > 64 || !g_ascii_isalnum(text[0]))
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_isalnum(text[i]) && text[i] != '.' && text[i] != '_' && text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_download_base_path_normalize(
	const gchar  *text,
	gchar       **normalized,
	const gchar **problem
){
	g_autofree gchar *trimmed = NULL;
	gsize n;
	gsize i;
	gsize segment_start;

	if (normalized != NULL)
		*normalized = NULL;
	if (problem != NULL)
		*problem = NULL;
	trimmed = g_strstrip(g_strdup(text != NULL ? text : ""));
	n = strlen(trimmed);
	/* One trailing slash (or several) is only the way it was typed. */
	while (n > 0 && trimmed[n - 1] == '/')
		trimmed[--n] = '\0';
	if (n == 0) {
		if (normalized != NULL)
			*normalized = g_strdup("");
		return TRUE;
	}
	if (n > AXIS_DOWNLOAD_BASE_PATH_MAX) {
		if (problem != NULL)
			*problem = "is too long";
		return FALSE;
	}
	if (trimmed[0] != '/') {
		if (problem != NULL)
			*problem = "does not start with a slash";
		return FALSE;
	}
	segment_start = 1;
	for (i = 1; i <= n; i++) {
		if (trimmed[i] == '/' || trimmed[i] == '\0') {
			gsize length = i - segment_start;

			if (length == 0) {
				if (problem != NULL)
					*problem = "has an empty part (two slashes in a row)";
				return FALSE;
			}
			if ((length == 1 && trimmed[segment_start] == '.') ||
			    (length == 2 && trimmed[segment_start] == '.' && trimmed[segment_start + 1] == '.')) {
				if (problem != NULL)
					*problem = "has a '.' or '..' part";
				return FALSE;
			}
			segment_start = i + 1;
			continue;
		}
		if (!g_ascii_isalnum(trimmed[i]) && trimmed[i] != '.' && trimmed[i] != '_' && trimmed[i] != '~' &&
		    trimmed[i] != '-') {
			if (problem != NULL)
				*problem = "has a character other than letters, digits, '.', '_', '~' and '-' in its parts";
			return FALSE;
		}
	}
	if (normalized != NULL)
		*normalized = g_strdup(trimmed);
	return TRUE;
}

gboolean
axis_download_kind_name_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n == 0 || n > 16 || !g_ascii_islower(text[0]))
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_islower(text[i]) && !g_ascii_isdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_download_ref_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n == 0 || n > AXIS_DOWNLOAD_REF_MAX)
		return FALSE;
	for (i = 0; i < n; i++) {
		if ((guchar)text[i] <= 0x20 || (guchar)text[i] >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_download_module_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n == 0 || n > 32 || !g_ascii_islower(text[0]))
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_islower(text[i]) && !g_ascii_isdigit(text[i]) && text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_download_hash_valid(const gchar *text)
{
	gsize i;

	if (text == NULL || strlen(text) != 40)
		return FALSE;
	for (i = 0; i < 40; i++) {
		if (!g_ascii_isdigit(text[i]) && !(text[i] >= 'a' && text[i] <= 'f'))
			return FALSE;
	}
	return TRUE;
}

/* ── Magnet links ───────────────────────────────────────────────── */

#define MAGNET_PREFIX "magnet:?xt=urn:btih:"

/* What may follow the hash, as in a URL's query. */
static gboolean
is_param_char(guchar c)
{
	if (g_ascii_isalnum(c))
		return TRUE;
	return c != '\0' && strchr("-._~:/?@!$&'()*+,;=%[]", c) != NULL;
}

static gint
base32_value(gchar c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a';
	if (c >= '2' && c <= '7')
		return c - '2' + 26;
	return -1;
}

/* 32 base32 characters (160 bits) as 40 hex digits. */
static gchar *
base32_to_hex(const gchar *text)
{
	guint8 bytes[20];
	guint64 buffer = 0;
	guint bits = 0;
	guint n = 0;
	guint i;
	GString *out;

	memset(bytes, 0, sizeof(bytes));
	for (i = 0; i < 32; i++) {
		buffer = (buffer << 5) | (guint64)base32_value(text[i]);
		bits += 5;
		if (bits >= 8) {
			bits -= 8;
			bytes[n++] = (guint8)((buffer >> bits) & 0xff);
		}
	}
	out = g_string_sized_new(40);
	for (i = 0; i < 20; i++)
		g_string_append_printf(out, "%02x", bytes[i]);
	return g_string_free(out, FALSE);
}

gboolean
axis_download_magnet_parse(
	const gchar  *text,
	gchar       **info_hash,
	const gchar **problem
){
	const gchar *hash;
	const gchar *p;
	gsize n;
	gsize run = 0;
	gboolean hex = TRUE;
	gboolean b32 = TRUE;

	if (info_hash != NULL)
		*info_hash = NULL;
	if (text == NULL || text[0] == '\0') {
		if (problem != NULL)
			*problem = "there is no magnet link";
		return FALSE;
	}
	n = strlen(text);
	if (n > AXIS_DOWNLOAD_MAGNET_MAX) {
		if (problem != NULL)
			*problem = "the magnet link is too long";
		return FALSE;
	}
	if (g_ascii_strncasecmp(text, MAGNET_PREFIX, strlen(MAGNET_PREFIX)) != 0) {
		if (problem != NULL)
			*problem = "a magnet link starts with magnet:?xt=urn:btih:";
		return FALSE;
	}
	hash = text + strlen(MAGNET_PREFIX);
	for (p = hash; *p != '\0' && *p != '&'; p++) {
		run++;
		hex = hex && g_ascii_isxdigit(*p);
		b32 = b32 && base32_value(*p) >= 0;
	}
	if (!((run == 40 && hex) || (run == 32 && b32))) {
		if (problem != NULL)
			*problem = "the magnet link's hash is not 40 hexadecimal or 32 base32 characters";
		return FALSE;
	}
	for (; *p != '\0'; p++) {
		if (!is_param_char((guchar)*p)) {
			if (problem != NULL)
				*problem = "the magnet link has characters a link cannot have";
			return FALSE;
		}
	}
	if (info_hash != NULL)
		*info_hash = run == 40 ? g_ascii_strdown(hash, 40) : base32_to_hex(hash);
	return TRUE;
}

/* ── Items ──────────────────────────────────────────────────────── */

AxisDownloadItem *
axis_download_item_new(void)
{
	AxisDownloadItem *item = g_new0(AxisDownloadItem, 1);

	item->size_bytes = -1;
	item->done_bytes = -1;
	item->rate_bps = -1;
	item->eta_s = -1;
	return item;
}

void
axis_download_item_free(AxisDownloadItem *item)
{
	if (item == NULL)
		return;
	g_free(item->client_ref);
	g_free(item->name);
	g_free(item->own_name);
	g_free(item->unreadable);
	g_free(item->category);
	g_free(item->info_hash);
	g_free(item->path);
	g_free(item->place);
	g_free(item->message);
	g_free(item);
}

static void
replace_clean(
	gchar **text,
	guint   max_chars
){
	gchar *clean = axis_download_text_clean(*text, max_chars);

	g_free(*text);
	*text = clean;
}

gboolean
axis_download_item_check(AxisDownloadItem *item)
{
	if (item == NULL || item->unreadable != NULL || !axis_download_ref_valid(item->client_ref) ||
	    item->category == NULL)
		return FALSE;
	if ((guint)item->state > AXIS_DOWNLOAD_ITEM_FAILED)
		return FALSE;
	/* The name as the client gave it (the kind's raw text, or else the name
	 * before it is cleaned), when it can be kept exactly: what is compared
	 * with names on disk.  The cleaned one is for showing. */
	if (item->own_name == NULL)
		item->own_name = g_strdup(item->name);
	if (item->own_name != NULL) {
		const gchar *p = item->own_name;

		if (p[0] != '\0' && strlen(p) <= AXIS_DOWNLOAD_PATH_MAX && g_utf8_validate(p, -1, NULL)) {
			for (; *p != '\0' && (guchar)*p >= 0x20 && *p != 0x7f; p++)
				;
		}
		if (item->own_name[0] == '\0' || *p != '\0')
			g_clear_pointer(&item->own_name, g_free);
	}
	replace_clean(&item->name, AXIS_DOWNLOAD_NAME_MAX_CHARS);
	replace_clean(&item->message, AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
	if (item->name == NULL)
		return FALSE;
	if (item->info_hash != NULL) {
		gchar *lower = g_ascii_strdown(item->info_hash, -1);

		g_free(item->info_hash);
		item->info_hash = lower;
		if (!axis_download_hash_valid(item->info_hash))
			g_clear_pointer(&item->info_hash, g_free);
	}
	/* A path is used only as the volume guard allows (download/axis-download-path.h);
	 * here it is only bounded and kept free of control characters. */
	if (item->path != NULL && (strlen(item->path) > AXIS_DOWNLOAD_PATH_MAX ||
	                           !g_utf8_validate(item->path, -1, NULL)))
		g_clear_pointer(&item->path, g_free);
	if (item->path != NULL) {
		const gchar *p;

		for (p = item->path; *p != '\0'; p++) {
			if ((guchar)*p < 0x20 || *p == 0x7f) {
				g_clear_pointer(&item->path, g_free);
				break;
			}
		}
	}
	/* Its place, the same way. */
	if (item->place != NULL && (strlen(item->place) > AXIS_DOWNLOAD_PATH_MAX ||
	                            !g_utf8_validate(item->place, -1, NULL)))
		g_clear_pointer(&item->place, g_free);
	if (item->place != NULL) {
		const gchar *p;

		for (p = item->place; *p != '\0'; p++) {
			if ((guchar)*p < 0x20 || *p == 0x7f) {
				g_clear_pointer(&item->place, g_free);
				break;
			}
		}
	}
	if (item->size_bytes < -1)
		item->size_bytes = -1;
	if (item->done_bytes < -1)
		item->done_bytes = -1;
	if (item->size_bytes >= 0 && item->done_bytes > item->size_bytes)
		item->done_bytes = item->size_bytes;
	if (item->rate_bps < -1)
		item->rate_bps = -1;
	if (item->eta_s < -1)
		item->eta_s = -1;
	return TRUE;
}
