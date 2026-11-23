/*
 * fuzz-download-sabnzbd.c - libFuzzer target for the SABnzbd kind's reading
 * of SABnzbd's answers (download/axis-download-sabnzbd-parse.h), and for the
 * rule about a client's base path
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' any answer: its kind, version,
 * categories, an add's result and a removal's, the folder settings of
 * get_config and the category's folder made of them; '1' a queue; '2' a history;
 * '3' a size in megabytes inside a JSON document, and a time left as plain
 * text; '4' text for the base path); the rest is the input.  Whatever it is,
 * the parser returns without crashing or allocating without bound, and what
 * it returns is of the forms the layer applies its own checks to:
 *
 *  - an answer is exactly one of ok (with a document), refused or about the
 *    key (with the words, cleaned, or none), unreadable (with a reason);
 *  - a list holds at most AXIS_SABNZBD_MAX_SLOTS slots, each with a valid
 *    identifier (once), a clean name and a category, a state of the right
 *    list (completed and failed only from the history, paused only from
 *    the queue), sizes that are -1 or not negative, and no more done than
 *    the whole; the layer's own check accepts every one;
 *  - a size is the megabytes times 2^20, within the stated maximum; a time
 *    left is -1 or within ten years;
 *  - a base path that is accepted is "" or begins with a slash, has no
 *    trailing slash, no empty part, no "." or ".." part and only the
 *    permitted characters, and normalizing it again changes nothing; one
 *    that is refused gives a reason and nothing else.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "download/axis-download-model.h"
#include "download/axis-download-sabnzbd-parse.h"
#include "storage/axis-storage-path.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_slots(
	GPtrArray *slots,
	gboolean   history
){
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;

	if (slots == NULL)
		return;
	if (slots->len > AXIS_SABNZBD_MAX_SLOTS)
		abort();
	for (i = 0; i < slots->len; i++) {
		AxisSabnzbdSlot *slot = g_ptr_array_index(slots, i);
		AxisDownloadItem *item = slot->item;

		if (item == NULL || slot->history != history)
			abort();
		if (!axis_sabnzbd_ref_valid(item->client_ref) || !axis_download_ref_valid(item->client_ref))
			abort();
		if (g_hash_table_contains(seen, item->client_ref))
			abort();
		g_hash_table_add(seen, item->client_ref);
		/* One read by its identifier only (no name or no category) says why,
		 * has no path read, and is never used. */
		if (item->unreadable != NULL) {
			if ((item->name != NULL && item->category != NULL) || item->path != NULL || slot->known ||
			    axis_download_item_check(item))
				abort();
			continue;
		}
		if (item->name == NULL || item->name[0] == '\0' || item->category == NULL)
			abort();
		if (slot->word != NULL && g_utf8_strlen(slot->word, -1) > AXIS_SABNZBD_WORD_MAX)
			abort();
		if (!history && (item->state == AXIS_DOWNLOAD_ITEM_COMPLETED || item->state == AXIS_DOWNLOAD_ITEM_FAILED))
			abort();
		if (history && item->state == AXIS_DOWNLOAD_ITEM_PAUSED)
			abort();
		if (item->size_bytes < -1 || item->done_bytes < -1 || item->eta_s < -1 || item->rate_bps < -1)
			abort();
		if (item->size_bytes >= 0 && item->done_bytes > item->size_bytes)
			abort();
		if (item->path != NULL && item->state != AXIS_DOWNLOAD_ITEM_COMPLETED)
			abort();
		if (item->message != NULL && item->state != AXIS_DOWNLOAD_ITEM_FAILED)
			abort();
		if (!axis_download_item_check(item))
			abort();
	}
}

/* A folder setting read is a bounded text without control characters; the
 * category's folder made of it is absolute and normalised, or refused with a
 * reason, and never resolves a "." or "..". */
static void
check_path_text(const gchar *text)
{
	const gchar *p;

	if (text == NULL)
		return;
	if (strlen(text) > AXIS_SABNZBD_PATH_MAX || !g_utf8_validate(text, -1, NULL))
		abort();
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			abort();
	}
}

static void
check_folders(const AxisJsonNode *root)
{
	gchar *dir = NULL;
	gchar *complete = axis_sabnzbd_parse_complete_dir(root);
	gint found = axis_sabnzbd_parse_category_dir(root, "axis", &dir);

	if (found < -1 || found > 1 || (found == 1) != (dir != NULL))
		abort();
	check_path_text(dir);
	check_path_text(complete);
	if (dir != NULL) {
		gchar *folder = NULL;
		const gchar *problem = NULL;
		gboolean ok = axis_sabnzbd_category_folder(complete, dir, &folder, &problem);
		gchar **parts;
		guint i;

		if (ok != (folder != NULL) || ok == (problem != NULL))
			abort();
		if (folder != NULL) {
			if (folder[0] != '/' || g_str_has_suffix(folder, "/") || strstr(folder, "//") != NULL)
				abort();
			parts = g_strsplit(folder + 1, "/", -1);
			for (i = 0; parts[i] != NULL; i++) {
				if (strcmp(parts[i], ".") == 0 || strcmp(parts[i], "..") == 0 || parts[i][0] == '\0')
					abort();
			}
			g_strfreev(parts);
		}
		/* The other categories' folders against it: shared only with a
		 * folder of theirs that is it or inside it. */
		if (folder != NULL) {
			GPtrArray *names = NULL;
			GPtrArray *dirs = NULL;

			if (axis_sabnzbd_parse_other_dirs(root, "axis", &names, &dirs)) {
				if (names->len != dirs->len || names->len > AXIS_SABNZBD_MAX_SLOTS)
					abort();
				for (i = 0; i < dirs->len; i++) {
					gchar *theirs = NULL;
					gint shared = axis_sabnzbd_folder_shared(complete, folder, g_ptr_array_index(dirs, i), &theirs);

					check_path_text(g_ptr_array_index(dirs, i));
					if (shared < -1 || shared > 1 || (shared == 1) != (theirs != NULL) ||
					    (theirs != NULL && !axis_storage_path_contains(folder, theirs)))
						abort();
					g_free(theirs);
				}
				g_ptr_array_unref(names);
				g_ptr_array_unref(dirs);
			} else if (names != NULL || dirs != NULL) {
				abort();
			}
		}
		g_free(folder);
	}
	g_free(complete);
	g_free(dir);
}

static void
check_answer(
	const gchar *text,
	gsize        length
){
	AxisJsonDoc *doc = NULL;
	gchar *message = NULL;
	AxisSabnzbdReplyKind kind = axis_sabnzbd_reply_parse(text, length, &doc, &message);

	if (kind == AXIS_SABNZBD_REPLY_OK) {
		const AxisJsonNode *root;
		gchar *version;
		gchar *id = NULL;
		gchar *why = NULL;
		gboolean added;
		gint exists;

		if (doc == NULL || message != NULL)
			abort();
		root = axis_json_doc_get_root(doc);
		if (axis_json_type(root) != AXIS_JSON_OBJECT)
			abort();
		version = axis_sabnzbd_parse_version(root);
		if (version != NULL && (version[0] == '\0' || g_utf8_strlen(version, -1) > 64))
			abort();
		g_free(version);
		exists = axis_sabnzbd_parse_categories(root, "axis");
		if (exists < -1 || exists > 1)
			abort();
		added = axis_sabnzbd_parse_added(root, &id, &why);
		if (added && (id == NULL || why != NULL || !axis_sabnzbd_ref_valid(id)))
			abort();
		if (!added && (id != NULL || why == NULL))
			abort();
		(void)axis_sabnzbd_parse_removed(root, "SABnzbd_nzo_abc123");
		check_folders(root);
		g_free(id);
		g_free(why);
		axis_json_doc_free(doc);
	} else {
		if (doc != NULL)
			abort();
		if (kind == AXIS_SABNZBD_REPLY_UNREADABLE && message == NULL)
			abort();
		if (message != NULL && g_utf8_strlen(message, -1) > 300)
			abort();
	}
	g_free(message);
}

static void
check_list(
	const gchar *text,
	gsize        length,
	gboolean     history
){
	AxisJsonDoc *doc = axis_json_parse(text, length, NULL);
	GPtrArray *slots;

	if (doc == NULL)
		return;
	slots = history ? axis_sabnzbd_parse_history(axis_json_doc_get_root(doc)) :
		axis_sabnzbd_parse_queue(axis_json_doc_get_root(doc));
	check_slots(slots, history);
	if (slots != NULL)
		g_ptr_array_unref(slots);
	axis_json_doc_free(doc);
}

static void
check_figures(
	const gchar *text,
	gsize        length
){
	AxisJsonDoc *doc = axis_json_parse(text, length, NULL);
	gchar *plain = g_strndup(text, length);
	gint64 seconds = axis_sabnzbd_timeleft_parse(plain);

	if (seconds < -1 || seconds > (gint64)10 * 365 * 86400)
		abort();
	if (doc != NULL) {
		gint64 bytes = 0;

		if (axis_sabnzbd_megabytes_parse(axis_json_doc_get_root(doc), &bytes) &&
		    (bytes < 0 || (gdouble)bytes > AXIS_SABNZBD_MAX_MEGABYTES * 1048576.0 + 1))
			abort();
		axis_json_doc_free(doc);
	}
	g_free(plain);
}

static void
check_base_path(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar *normalized = NULL;
	gchar *again = NULL;
	const gchar *problem = NULL;

	if (axis_download_base_path_normalize(copy, &normalized, &problem)) {
		gchar **segments;
		guint i;

		if (normalized == NULL || problem != NULL || strlen(normalized) > AXIS_DOWNLOAD_BASE_PATH_MAX)
			abort();
		if (normalized[0] != '\0') {
			if (normalized[0] != '/' || g_str_has_suffix(normalized, "/") || strstr(normalized, "//") != NULL)
				abort();
			segments = g_strsplit(normalized + 1, "/", -1);
			for (i = 0; segments[i] != NULL; i++) {
				const gchar *p;

				if (segments[i][0] == '\0' || strcmp(segments[i], ".") == 0 || strcmp(segments[i], "..") == 0)
					abort();
				for (p = segments[i]; *p != '\0'; p++) {
					if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '~' && *p != '-')
						abort();
				}
			}
			g_strfreev(segments);
		}
		if (!axis_download_base_path_normalize(normalized, &again, NULL) || g_strcmp0(again, normalized) != 0)
			abort();
		g_free(again);
	} else if (normalized != NULL || problem == NULL) {
		abort();
	}
	g_free(normalized);
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
		check_list(text, length, FALSE);
		break;
	case '2':
		check_list(text, length, TRUE);
		break;
	case '3':
		check_figures(text, length);
		break;
	case '4':
		check_base_path(text, length);
		break;
	default:
		break;
	}
	return 0;
}
