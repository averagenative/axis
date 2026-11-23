/*
 * axis-download-qbittorrent-parse.c - qBittorrent's Web API v2 answers, read
 * into the downloads layer's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-qbittorrent-parse.h"

#include <string.h>

/* A time left of more than this (ten years) is not believed. */
#define TIME_LEFT_MAX_S ((gint64)10 * 365 * 86400)

/* What qBittorrent says an unknown time left is (a hundred days). */
#define ETA_NEVER_S 8640000

/* The most bytes a torrent may say it is (a petabyte). */
#define BYTES_MAX ((gint64)1 << 50)

/* The most bytes a second a torrent may say it moves (a terabyte). */
#define RATE_MAX ((gint64)1 << 40)

/* The longest category kept. */
#define CATEGORY_MAX 64

/* ── Plain text ─────────────────────────────────────────────────── */

/* The trimmed body, as a new string ("" for none). */
static gchar *
trimmed(
	const gchar *data,
	gsize        length
){
	gchar *copy;

	if (data == NULL)
		length = 0;
	/* A body with a NUL in it is not text. */
	if (length > 0 && memchr(data, '\0', length) != NULL)
		return g_strdup("\x01");
	copy = g_strndup(data != NULL ? data : "", length);
	return g_strstrip(copy);
}

AxisQbitText
axis_qbit_parse_text(
	const gchar *data,
	gsize        length
){
	g_autofree gchar *text = trimmed(data, length);

	if (text[0] == '\0' || strcmp(text, "Ok.") == 0)
		return AXIS_QBIT_TEXT_OK;
	if (strcmp(text, "Fails.") == 0)
		return AXIS_QBIT_TEXT_FAILS;
	return AXIS_QBIT_TEXT_OTHER;
}

gchar *
axis_qbit_parse_version(
	const gchar *data,
	gsize        length
){
	g_autofree gchar *text = trimmed(data, length);
	gsize n = strlen(text);
	gsize i;

	if (n == 0 || n > AXIS_QBIT_VERSION_MAX)
		return NULL;
	for (i = 0; i < n; i++) {
		gchar c = text[i];

		if (!g_ascii_isalnum(c) && c != '.' && c != '_' && c != '+' && c != '-')
			return NULL;
	}
	return g_steal_pointer(&text);
}

/* ── Fields ─────────────────────────────────────────────────────── */

static gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_download_text_clean(axis_json_string(axis_json_member(object, name)), max_chars);
}

gboolean
axis_qbit_hash_text_valid(const gchar *text)
{
	gsize i;

	if (text == NULL || strlen(text) != 40)
		return FALSE;
	for (i = 0; i < 40; i++) {
		if (!g_ascii_isxdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

gint
axis_qbit_parse_has_category(
	const gchar *data,
	gsize        length,
	const gchar *name
){
	AxisJsonDoc *doc;
	const AxisJsonNode *root;
	gint found = 0;
	guint i;

	if (data == NULL || length == 0)
		return -1;
	doc = axis_json_parse(data, length, NULL);
	if (doc == NULL)
		return -1;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		axis_json_doc_free(doc);
		return -1;
	}
	for (i = 0; name != NULL && i < axis_json_length(root) && i < AXIS_QBIT_MAX_LIST; i++) {
		const gchar *one = axis_json_member_name(root, i);

		if (one != NULL && strcmp(one, name) == 0)
			found = 1;
	}
	axis_json_doc_free(doc);
	return found;
}

/* ── A category's folder ────────────────────────────────────────── */

/* @text as a path's text: valid UTF-8 within the layer's bound, no control
 * characters (a name's own spaces are its name). */
static gboolean
path_text_ok(
	const gchar *text,
	gsize        length
){
	gsize i;

	if (text == NULL || length == 0 || length > AXIS_DOWNLOAD_PATH_MAX || !g_utf8_validate(text, (gssize)length, NULL))
		return FALSE;
	for (i = 0; i < length; i++) {
		if ((guchar)text[i] < 0x20 || text[i] == 0x7f)
			return FALSE;
	}
	return TRUE;
}

void
axis_qbit_category_free(AxisQbitCategory *category)
{
	if (category == NULL)
		return;
	g_free(category->name);
	g_free(category->save_path);
	g_free(category);
}

GPtrArray *
axis_qbit_parse_categories(
	const gchar *data,
	gsize        length
){
	AxisJsonDoc *doc;
	const AxisJsonNode *root;
	GPtrArray *out;
	guint i;

	if (data == NULL || length == 0)
		return NULL;
	doc = axis_json_parse(data, length, NULL);
	if (doc == NULL)
		return NULL;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		axis_json_doc_free(doc);
		return NULL;
	}
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_qbit_category_free);
	for (i = 0; i < axis_json_length(root) && i < AXIS_QBIT_MAX_LIST; i++) {
		const gchar *name = axis_json_member_name(root, i);
		const gchar *path = axis_json_string(axis_json_member(axis_json_member_value(root, i), "savePath"));
		AxisQbitCategory *category;

		if (name == NULL || name[0] == '\0' || !g_utf8_validate(name, -1, NULL))
			continue;
		category = g_new0(AxisQbitCategory, 1);
		category->name = g_strdup(name);
		if (path == NULL || path[0] == '\0')
			category->save_path = g_strdup("");
		else if (path_text_ok(path, strlen(path)))
			category->save_path = g_strdup(path);
		g_ptr_array_add(out, category);
	}
	axis_json_doc_free(doc);
	return out;
}

gchar *
axis_qbit_parse_path_text(
	const gchar *data,
	gsize        length
){
	gsize end = length;

	if (data == NULL)
		return NULL;
	/* A line ending after the text is not part of it; a space is part of the
	 * path it ends (never trimmed: the trimmed path is another place). */
	if (end > 0 && data[end - 1] == '\n')
		end--;
	if (end > 0 && data[end - 1] == '\r')
		end--;
	if (!path_text_ok(data, end))
		return NULL;
	return g_strndup(data, end);
}

/* ── States ─────────────────────────────────────────────────────── */

gboolean
axis_qbit_state_map(
	const gchar    *word,
	AxisQbitMapped *mapped
){
	static const struct
	{
		const gchar          *word;
		AxisDownloadItemState state;
		gboolean              finished;
		gboolean              moving;
	} table[] = {
		{ "downloading", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "forcedDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "stalledDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "metaDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "forcedMetaDL", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, FALSE },
		{ "queuedDL", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "allocating", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "checkingDL", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "checkingResumeData", AXIS_DOWNLOAD_ITEM_QUEUED, FALSE, FALSE },
		{ "pausedDL", AXIS_DOWNLOAD_ITEM_PAUSED, FALSE, FALSE },
		{ "stoppedDL", AXIS_DOWNLOAD_ITEM_PAUSED, FALSE, FALSE },
		{ "uploading", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "forcedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "stalledUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "queuedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "checkingUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "pausedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "stoppedUP", AXIS_DOWNLOAD_ITEM_COMPLETED, TRUE, FALSE },
		{ "moving", AXIS_DOWNLOAD_ITEM_DOWNLOADING, FALSE, TRUE },
		{ "error", AXIS_DOWNLOAD_ITEM_FAILED, FALSE, FALSE },
		{ "missingFiles", AXIS_DOWNLOAD_ITEM_FAILED, FALSE, FALSE }
	};
	guint i;

	mapped->state = AXIS_DOWNLOAD_ITEM_QUEUED;
	mapped->finished = FALSE;
	mapped->moving = FALSE;
	if (word == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (strcmp(word, table[i].word) == 0) {
			mapped->state = table[i].state;
			mapped->finished = table[i].finished;
			mapped->moving = table[i].moving;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── Torrents ───────────────────────────────────────────────────── */

void
axis_qbit_slot_free(AxisQbitSlot *slot)
{
	if (slot == NULL)
		return;
	axis_download_item_free(slot->item);
	g_free(slot->word);
	g_free(slot);
}

/* @text as a folder name or a folder: valid UTF-8 within the layer's bound,
 * no control characters; not cleaned (a name's own spaces are its name). */
static gboolean
plain_text_ok(const gchar *text)
{
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > AXIS_DOWNLOAD_PATH_MAX || !g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* @text without its trailing slashes (a lone "/" stays), as a new string. */
static gchar *
without_trailing_slashes(const gchar *text)
{
	gsize n = strlen(text);

	while (n > 1 && text[n - 1] == '/')
		n--;
	return g_strndup(text, n);
}

/* Where the torrent's files are: "content_path" when it is a plain absolute
 * path; otherwise (qBittorrent 4.1 to 4.3.1 have none) "save_path" and the
 * torrent's own name, which must be a single path segment. */
static gchar *
path_of(
	const AxisJsonNode *status,
	const gchar        *name
){
	const gchar *content = axis_json_string(axis_json_member(status, "content_path"));
	const gchar *save = axis_json_string(axis_json_member(status, "save_path"));
	g_autofree gchar *folder = NULL;

	if (plain_text_ok(content) && content[0] == '/') {
		gchar *path = without_trailing_slashes(content);

		if (strcmp(path, "/") != 0)
			return path;
		g_free(path);
		return NULL;
	}
	if (!plain_text_ok(save) || !plain_text_ok(name) || strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || save[0] != '/')
		return NULL;
	folder = without_trailing_slashes(save);
	if (strlen(folder) + 1 + strlen(name) > AXIS_DOWNLOAD_PATH_MAX)
		return NULL;
	if (strcmp(folder, "/") == 0)
		return g_strconcat("/", name, NULL);
	return g_strdup_printf("%s/%s", folder, name);
}

/* One torrent of the list; %NULL when it is another category's, or (with
 * *@malformed) when it cannot be read: not an object, no hash, no name. */
static AxisQbitSlot *
torrent_slot(
	const AxisJsonNode *status,
	const gchar        *category,
	gboolean           *malformed
){
	AxisQbitSlot *slot;
	AxisDownloadItem *item;
	AxisQbitMapped mapped;
	const gchar *hash;
	const gchar *raw_name;
	const gchar *raw_state;
	gchar *word;
	gchar *shown_category;
	gint64 size = -1;
	gint64 rate = -1;
	gint64 eta = -1;
	gdouble progress = -1;
	gboolean known;

	*malformed = FALSE;
	if (axis_json_type(status) != AXIS_JSON_OBJECT) {
		*malformed = TRUE;
		return NULL;
	}
	/* Another category's is not Axis's, whatever else it holds. */
	shown_category = text_member(status, "category", CATEGORY_MAX);
	if (category != NULL && (shown_category == NULL || strcmp(shown_category, category) != 0)) {
		g_free(shown_category);
		return NULL;
	}
	hash = axis_json_string(axis_json_member(status, "hash"));
	if (!axis_qbit_hash_text_valid(hash)) {
		g_free(shown_category);
		*malformed = TRUE;
		return NULL;
	}
	if (shown_category == NULL)
		shown_category = g_strdup("");
	item = axis_download_item_new();
	item->client_ref = g_ascii_strdown(hash, -1);
	item->info_hash = g_strdup(item->client_ref);
	item->name = text_member(status, "name", AXIS_DOWNLOAD_NAME_MAX_CHARS);
	item->category = shown_category;
	/* Its hash read, the rest not: returned as one that cannot be read, so
	 * that the layer knows which torrent it is. */
	if (item->name == NULL) {
		item->unreadable = g_strdup("it has no name Axis can read");
		slot = g_new0(AxisQbitSlot, 1);
		slot->item = item;
		return slot;
	}
	raw_name = axis_json_string(axis_json_member(status, "name"));
	/* The name exactly as qBittorrent gave it, for comparing with names on
	 * disk (axis_download_item_check() keeps it only when it can be kept). */
	item->own_name = g_strdup(raw_name);
	raw_state = axis_json_string(axis_json_member(status, "state"));

	word = axis_download_text_clean(raw_state, AXIS_QBIT_WORD_MAX);
	known = axis_qbit_state_map(word, &mapped);

	/* What it wants is the selected files ("size"); "total_size" is all of them. */
	if (!axis_json_get_integer(axis_json_member(status, "size"), 0, BYTES_MAX, &size) &&
	    !axis_json_get_integer(axis_json_member(status, "total_size"), 0, BYTES_MAX, &size))
		size = -1;
	if (!axis_json_get_double(axis_json_member(status, "progress"), 0.0, 1.0, &progress))
		progress = -1;
	if (!axis_json_get_integer(axis_json_member(status, "dlspeed"), 0, RATE_MAX, &rate))
		rate = -1;
	if (!axis_json_get_integer(axis_json_member(status, "eta"), 0, TIME_LEFT_MAX_S, &eta) || eta == 0 ||
	    eta >= ETA_NEVER_S)
		eta = -1;
	item->size_bytes = size;
	if (size >= 0 && progress >= 0) {
		gint64 done = (gint64)((gdouble)size * progress);

		item->done_bytes = done > size ? size : done;
	}

	item->state = mapped.state;
	/* Where its files are now, in progress too (qBittorrent keeps a torrent
	 * in progress in its save path, unless a folder for incomplete torrents is
	 * set); not while it moves them. */
	if (!(known && mapped.moving))
		item->place = path_of(status, raw_name);
	if (known && mapped.moving) {
		/* Moving is downloading, all of it: the files are not where they will be. */
		item->done_bytes = size;
	} else if (known && mapped.finished) {
		item->done_bytes = size;
		item->path = path_of(status, raw_name);
	} else if (known && mapped.state == AXIS_DOWNLOAD_ITEM_DOWNLOADING) {
		item->rate_bps = rate;
		item->eta_s = eta;
	} else if (known && mapped.state == AXIS_DOWNLOAD_ITEM_FAILED) {
		item->message = g_strdup(strcmp(word, "missingFiles") == 0 ?
			"qBittorrent reports that the files of this torrent are missing" :
			"qBittorrent reports this torrent as in error");
	}

	slot = g_new0(AxisQbitSlot, 1);
	slot->item = item;
	slot->word = word;
	slot->known = known;
	return slot;
}

GPtrArray *
axis_qbit_parse_torrents(
	const gchar  *data,
	gsize         length,
	const gchar  *category,
	gchar       **problem
){
	return axis_qbit_parse_torrents_counted(data, length, category, NULL, NULL, problem);
}

GPtrArray *
axis_qbit_parse_torrents_counted(
	const gchar  *data,
	gsize         length,
	const gchar  *category,
	guint        *unreadable,
	gboolean     *cut,
	gchar       **problem
){
	g_autoptr(GHashTable) seen = NULL;
	g_autofree gchar *why = NULL;
	AxisJsonDoc *doc;
	const AxisJsonNode *root;
	GPtrArray *out;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (unreadable != NULL)
		*unreadable = 0;
	if (cut != NULL)
		*cut = FALSE;
	if (data == NULL || length == 0) {
		if (problem != NULL)
			*problem = g_strdup("it is empty");
		return NULL;
	}
	doc = axis_json_parse(data, length, &why);
	if (doc == NULL) {
		if (problem != NULL)
			*problem = g_strdup(why != NULL ? why : "it is not JSON");
		return NULL;
	}
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_ARRAY) {
		axis_json_doc_free(doc);
		if (problem != NULL)
			*problem = g_strdup("it is not a list of torrents");
		return NULL;
	}
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_qbit_slot_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < axis_json_length(root); i++) {
		gboolean malformed = FALSE;
		AxisQbitSlot *slot;

		if (out->len >= AXIS_QBIT_MAX_TORRENTS) {
			/* The rest is not looked at: the list is not whole. */
			if (cut != NULL)
				*cut = TRUE;
			break;
		}
		slot = torrent_slot(axis_json_element(root, i), category, &malformed);
		if (slot == NULL) {
			if (malformed && unreadable != NULL)
				(*unreadable)++;
			continue;
		}
		/* A hash once. */
		if (g_hash_table_contains(seen, slot->item->client_ref)) {
			axis_qbit_slot_free(slot);
			continue;
		}
		g_hash_table_add(seen, slot->item->client_ref);
		g_ptr_array_add(out, slot);
	}
	axis_json_doc_free(doc);
	return out;
}
