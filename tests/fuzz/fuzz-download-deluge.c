/*
 * fuzz-download-deluge.c - libFuzzer target for the Deluge kind's reading of
 * Deluge's JSON-RPC answers (download/axis-download-deluge-parse.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' any answer, as an envelope: its
 * kind, and what each question of the kind makes of its result; '1' a list
 * of torrents (an object keyed by hash, without the envelope); '2' text for a
 * label); the rest is the input.  Whatever it is, the parser returns without
 * crashing or allocating without bound, and what it returns is of the forms
 * the layer applies its own checks to:
 *
 *  - an envelope is exactly one of ok (with a document and a result), an
 *    error or not authenticated (no document, Deluge's words cleaned and cut,
 *    or none), unreadable (with a reason);
 *  - a hash taken from a result is 40 lower-case hexadecimal digits; a
 *    host's identifier is one web.connect can be given;
 *  - a list holds at most AXIS_DELUGE_MAX_TORRENTS slots, each with a valid
 *    hash (once) that is both its identifier and its info-hash, a clean name
 *    and a label, sizes that are -1 or not negative, no more done than the
 *    whole, a path only when completed and a message only when failed; the
 *    layer's own check accepts every one;
 *  - a label that is accepted is 1 to 64 of the lower-case letters, digits,
 *    '.', '_' and '-', starting with a letter or a digit;
 *  - a label's "Move completed to" folder is given only when the label moves
 *    to one, as plain text within the bound; the core's settings are plain
 *    text; a list of labels holds valid labels only; words taken to say that
 *    Deluge has no such torrent name the error and that very hash.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "download/axis-download-deluge-parse.h"
#include "download/axis-download-model.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static gboolean
hash_lower_ok(const gchar *hash)
{
	gsize i;

	if (hash == NULL || strlen(hash) != 40)
		return FALSE;
	for (i = 0; i < 40; i++) {
		if (!g_ascii_isxdigit(hash[i]) || g_ascii_isupper(hash[i]))
			return FALSE;
	}
	return TRUE;
}

static void
check_slots(GPtrArray *slots)
{
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;

	if (slots == NULL)
		return;
	if (slots->len > AXIS_DELUGE_MAX_TORRENTS)
		abort();
	for (i = 0; i < slots->len; i++) {
		AxisDelugeSlot *slot = g_ptr_array_index(slots, i);
		AxisDownloadItem *item = slot->item;

		if (item == NULL)
			abort();
		if (!hash_lower_ok(item->client_ref) || g_strcmp0(item->client_ref, item->info_hash) != 0)
			abort();
		if (!axis_download_ref_valid(item->client_ref) || !axis_download_hash_valid(item->info_hash))
			abort();
		if (g_hash_table_contains(seen, item->client_ref))
			abort();
		g_hash_table_add(seen, item->client_ref);
		/* One read by its hash only says why, has no place read, and is never
		 * used. */
		if (item->unreadable != NULL) {
			if (item->path != NULL || item->place != NULL || slot->known || axis_download_item_check(item))
				abort();
			continue;
		}
		/* No label is the label "" (another torrent's than Axis's). */
		if (item->name == NULL || item->name[0] == '\0' || item->category == NULL)
			abort();
		if (slot->word != NULL && g_utf8_strlen(slot->word, -1) > AXIS_DELUGE_WORD_MAX)
			abort();
		if (item->size_bytes < -1 || item->done_bytes < -1 || item->eta_s < -1 || item->rate_bps < -1)
			abort();
		if (item->size_bytes >= 0 && item->done_bytes > item->size_bytes)
			abort();
		if (item->path != NULL && item->state != AXIS_DOWNLOAD_ITEM_COMPLETED)
			abort();
		if (item->path != NULL && (item->path[0] != '/' || strlen(item->path) > AXIS_DOWNLOAD_PATH_MAX))
			abort();
		if (item->message != NULL && item->state != AXIS_DOWNLOAD_ITEM_FAILED)
			abort();
		if (item->state == AXIS_DOWNLOAD_ITEM_FAILED && item->message == NULL)
			abort();
		if (!axis_download_item_check(item))
			abort();
	}
}

/* Text without control characters, valid UTF-8, not empty. */
static gboolean
plain_ok(const gchar *text)
{
	const gchar *p;

	if (text == NULL || text[0] == '\0' || !g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* The Label plugin's options, the core's settings and the labels: a folder
 * only when the label moves to one, texts that are plain, labels valid. */
static void
check_label_answers(const AxisJsonNode *result)
{
	gchar *folder = NULL;
	gchar *value;
	GPtrArray *labels;
	gint found = axis_deluge_parse_label_folder(result, &folder);
	guint i;

	if (found < -1 || found > 1 || (found == 1) != (folder != NULL))
		abort();
	if (folder != NULL && (!plain_ok(folder) || strlen(folder) > AXIS_DOWNLOAD_PATH_MAX))
		abort();
	g_free(folder);
	/* One torrent's label, by its hash: found only with a bounded label. */
	{
		gchar *label = NULL;
		gchar *name = NULL;
		gint has = axis_deluge_parse_label_of(result, "0123456789abcdef0123456789abcdef01234567", &label, &name);

		if (has < -1 || has > 1 || (has == 1) != (label != NULL) || (label != NULL && strlen(label) > 64) ||
		    (has != 1 && name != NULL))
			abort();
		g_free(label);
		g_free(name);
	}
	value = axis_deluge_parse_config_text(result, "download_location");
	if (value != NULL && !plain_ok(value))
		abort();
	g_free(value);
	labels = axis_deluge_parse_labels(result);
	if (labels != NULL) {
		if (labels->len > AXIS_DELUGE_MAX_LIST)
			abort();
		for (i = 0; i < labels->len; i++) {
			if (!axis_deluge_label_valid(g_ptr_array_index(labels, i)))
				abort();
		}
		g_ptr_array_unref(labels);
	}
}

static void
check_answer(
	const gchar *text,
	gsize        length
){
	AxisJsonDoc *doc = NULL;
	const AxisJsonNode *result = NULL;
	gchar *message = NULL;
	AxisDelugeReplyKind kind = axis_deluge_reply_parse(text, length, 1, &doc, &result, &message);

	if (kind == AXIS_DELUGE_REPLY_OK) {
		gchar *hash;
		gchar *version;
		gchar *host_id = NULL;
		gchar *host = NULL;
		gint port = 0;
		gboolean value = FALSE;
		GPtrArray *slots;
		gint found;

		if (doc == NULL || result == NULL || message != NULL)
			abort();
		(void)axis_deluge_parse_boolean(result, &value);
		hash = axis_deluge_parse_hash(result);
		if (hash != NULL && !hash_lower_ok(hash))
			abort();
		g_free(hash);
		version = axis_deluge_parse_version(result);
		if (version != NULL && (version[0] == '\0' || g_utf8_strlen(version, -1) > 64))
			abort();
		g_free(version);
		found = axis_deluge_parse_has_name(result, "Label");
		if (found < -1 || found > 1)
			abort();
		found = axis_deluge_parse_has_label(result, "axis");
		if (found < -1 || found > 1)
			abort();
		if (axis_deluge_parse_host(result, &host_id, &host, &port)) {
			if (!axis_download_ref_valid(host_id) || port < 0 || port > 65535)
				abort();
			if (host != NULL && g_utf8_strlen(host, -1) > 100)
				abort();
		} else if (host_id != NULL || host != NULL) {
			abort();
		}
		g_free(host_id);
		g_free(host);
		slots = axis_deluge_parse_torrents(result);
		check_slots(slots);
		if (slots != NULL)
			g_ptr_array_unref(slots);
		check_label_answers(result);
		axis_json_doc_free(doc);
	} else {
		if (doc != NULL || result != NULL)
			abort();
		if (kind == AXIS_DELUGE_REPLY_UNREADABLE && message == NULL)
			abort();
		if (message != NULL && g_utf8_strlen(message, -1) > 300)
			abort();
		/* Deluge's words about a torrent it does not have name it. */
		if (axis_deluge_says_not_in_session(message, "0123456789abcdef0123456789abcdef01234567")) {
			g_autofree gchar *lower = g_ascii_strdown(message, -1);

			if (strstr(lower, "0123456789abcdef0123456789abcdef01234567 not in session") == NULL ||
			    strstr(lower, "invalidtorrenterror") == NULL)
				abort();
		}
	}
	g_free(message);
}

static void
check_list(
	const gchar *text,
	gsize        length
){
	AxisJsonDoc *doc = axis_json_parse(text, length, NULL);
	GPtrArray *slots;

	if (doc == NULL)
		return;
	slots = axis_deluge_parse_torrents(axis_json_doc_get_root(doc));
	check_slots(slots);
	if (slots != NULL)
		g_ptr_array_unref(slots);
	axis_json_doc_free(doc);
}

static void
check_label(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);

	if (axis_deluge_label_valid(copy)) {
		const gchar *p;

		if (strlen(copy) > 64 || !g_ascii_isalnum(copy[0]) || g_ascii_isupper(copy[0]))
			abort();
		for (p = copy; *p != '\0'; p++) {
			if (!(*p >= 'a' && *p <= 'z') && !g_ascii_isdigit(*p) && *p != '.' && *p != '_' && *p != '-')
				abort();
		}
	}
	g_free(copy);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text;
	gsize length;

	if (size < 1 || size > 1 << 20)
		return 0;
	text = (const gchar *)data + 1;
	length = size - 1;
	switch (data[0]) {
	case '0':
		check_answer(text, length);
		break;
	case '1':
		check_list(text, length);
		break;
	case '2':
		check_label(text, length);
		break;
	default:
		break;
	}
	return 0;
}
