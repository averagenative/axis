/*
 * fuzz-download-qbittorrent.c - libFuzzer target for the qBittorrent kind's
 * reading of qBittorrent's Web API answers
 * (download/axis-download-qbittorrent-parse.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' the plain text answers (a login's
 * or an add's, and a version); '1' a list of torrents of Axis's category; '2'
 * a list of torrents of any category; '3' a list of categories; '4' a state
 * word; '5' the categories with their save paths, and a default save path);
 * the rest is the input.  Whatever it is, the parser returns without
 * crashing or allocating without bound, and what it returns is of the forms
 * the layer applies its own checks to:
 *
 *  - a text is exactly one of the three answers; a version is 1 to
 *    AXIS_QBIT_VERSION_MAX of letters, digits and "._+-";
 *  - a list holds at most AXIS_QBIT_MAX_TORRENTS slots, each with a valid
 *    hash (once) that is both its identifier and its info-hash, a clean name
 *    and a category (exactly Axis's when one was asked for, else "" for none),
 *    sizes that are -1 or not negative, no more done than the whole, a path
 *    only when completed and a message only when failed; the layer's own
 *    check accepts every one;
 *  - a category list answers 1, 0 or -1 and nothing else.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "download/axis-download-qbittorrent-parse.h"
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
check_slots(
	GPtrArray   *slots,
	const gchar *category
){
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;

	if (slots == NULL)
		return;
	if (slots->len > AXIS_QBIT_MAX_TORRENTS)
		abort();
	for (i = 0; i < slots->len; i++) {
		AxisQbitSlot *slot = g_ptr_array_index(slots, i);
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
		/* One read by its hash only says why, has nothing else read, and is
		 * never used. */
		if (item->unreadable != NULL) {
			if (item->name != NULL || item->path != NULL || item->place != NULL || slot->known ||
			    axis_download_item_check(item))
				abort();
			continue;
		}
		if (item->name == NULL || item->name[0] == '\0' || item->category == NULL)
			abort();
		if (category != NULL && strcmp(item->category, category) != 0)
			abort();
		if (slot->word != NULL && g_utf8_strlen(slot->word, -1) > AXIS_QBIT_WORD_MAX)
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
		if (!slot->known && item->state != AXIS_DOWNLOAD_ITEM_QUEUED)
			abort();
		if (!axis_download_item_check(item))
			abort();
	}
}

static void
check_text(
	const gchar *text,
	gsize        length
){
	AxisQbitText kind = axis_qbit_parse_text(text, length);
	gchar *version = axis_qbit_parse_version(text, length);

	if (kind != AXIS_QBIT_TEXT_OK && kind != AXIS_QBIT_TEXT_FAILS && kind != AXIS_QBIT_TEXT_OTHER)
		abort();
	if (version != NULL) {
		gsize n = strlen(version);
		gsize i;

		if (n == 0 || n > AXIS_QBIT_VERSION_MAX)
			abort();
		for (i = 0; i < n; i++) {
			if (!g_ascii_isalnum(version[i]) && version[i] != '.' && version[i] != '_' && version[i] != '+' &&
			    version[i] != '-')
				abort();
		}
	}
	g_free(version);
}

static void
check_list(
	const gchar *text,
	gsize        length,
	const gchar *category
){
	gchar *problem = NULL;
	GPtrArray *slots = axis_qbit_parse_torrents(text, length, category, &problem);

	/* A problem is said exactly when there is no list. */
	if ((slots == NULL) != (problem != NULL))
		abort();
	if (problem != NULL && g_utf8_strlen(problem, -1) > 300)
		abort();
	check_slots(slots, category);
	if (slots != NULL)
		g_ptr_array_unref(slots);
	g_free(problem);
}

static void
check_categories(
	const gchar *text,
	gsize        length
){
	gint found = axis_qbit_parse_has_category(text, length, "axis");

	if (found < -1 || found > 1)
		abort();
}

/* Text without control characters, valid UTF-8. */
static gboolean
plain_ok(const gchar *text)
{
	const gchar *p;

	if (text == NULL || !g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* The categories with their save paths: names not empty, paths plain text
 * within the bound ("" for none) or left out (NULL); the default save path
 * the same, never empty, never with space around it. */
static void
check_folders(
	const gchar *text,
	gsize        length
){
	GPtrArray *categories = axis_qbit_parse_categories(text, length);
	gchar *path = axis_qbit_parse_path_text(text, length);
	guint i;

	if (categories != NULL) {
		if (categories->len > AXIS_QBIT_MAX_LIST)
			abort();
		for (i = 0; i < categories->len; i++) {
			const AxisQbitCategory *one = g_ptr_array_index(categories, i);

			if (one->name == NULL || one->name[0] == '\0' || !g_utf8_validate(one->name, -1, NULL))
				abort();
			if (one->save_path != NULL && (!plain_ok(one->save_path) ||
			    strlen(one->save_path) > AXIS_DOWNLOAD_PATH_MAX))
				abort();
		}
		g_ptr_array_unref(categories);
	}
	/* Only its line ending goes: a space at its start or its end is kept, for
	 * the layer to refuse such a path, never to take it trimmed (finding 5 of
	 * the stage 2 bug hunt). */
	if (path != NULL) {
		gsize n = strlen(path);

		if (n == 0 || n > AXIS_DOWNLOAD_PATH_MAX || !plain_ok(path))
			abort();
	}
	g_free(path);
}

static void
check_word(
	const gchar *text,
	gsize        length
){
	gchar *word = g_strndup(text, length);
	AxisQbitMapped mapped;
	gboolean known = axis_qbit_state_map(word, &mapped);

	if (!known && (mapped.state != AXIS_DOWNLOAD_ITEM_QUEUED || mapped.finished || mapped.moving))
		abort();
	if (mapped.finished && mapped.state != AXIS_DOWNLOAD_ITEM_COMPLETED)
		abort();
	if (mapped.moving && (mapped.finished || mapped.state != AXIS_DOWNLOAD_ITEM_DOWNLOADING))
		abort();
	g_free(word);
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
		check_text(text, length);
		break;
	case '1':
		check_list(text, length, "axis");
		break;
	case '2':
		check_list(text, length, NULL);
		break;
	case '3':
		check_categories(text, length);
		break;
	case '4':
		check_word(text, length);
		break;
	case '5':
		check_folders(text, length);
		break;
	default:
		break;
	}
	return 0;
}
