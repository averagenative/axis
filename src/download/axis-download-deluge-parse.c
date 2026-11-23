/*
 * axis-download-deluge-parse.c - Deluge's JSON-RPC answers, read into the
 * downloads layer's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-deluge-parse.h"

#include <string.h>

/* A time left of more than this (ten years) is not believed. */
#define TIME_LEFT_MAX_S ((gint64)10 * 365 * 86400)

/* The most bytes a torrent may say it is (a petabyte). */
#define BYTES_MAX ((gint64)1 << 50)

/* The most bytes a second a torrent may say it moves (a terabyte). */
#define RATE_MAX ((gint64)1 << 40)

/* The longest label kept. */
#define LABEL_MAX 64

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
axis_deluge_label_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n == 0 || n > LABEL_MAX)
		return FALSE;
	for (i = 0; i < n; i++) {
		gchar c = text[i];

		if (!(c >= 'a' && c <= 'z') && !g_ascii_isdigit(c) && c != '.' && c != '_' && c != '-')
			return FALSE;
	}
	return g_ascii_isalnum(text[0]);
}

static gboolean
hash_text_valid(const gchar *text)
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

gchar *
axis_deluge_parse_hash(const AxisJsonNode *result)
{
	const gchar *text = axis_json_string(result);

	return hash_text_valid(text) ? g_ascii_strdown(text, -1) : NULL;
}

gboolean
axis_deluge_parse_boolean(
	const AxisJsonNode *result,
	gboolean           *value
){
	if (axis_json_type(result) != AXIS_JSON_BOOLEAN)
		return FALSE;
	return axis_json_get_boolean(result, value);
}

gchar *
axis_deluge_parse_version(const AxisJsonNode *result)
{
	return axis_download_text_clean(axis_json_string(result), 64);
}

static gint
has_name(
	const AxisJsonNode *result,
	const gchar        *name,
	gboolean            fold
){
	gint found = 0;
	guint i;

	if (axis_json_type(result) != AXIS_JSON_ARRAY)
		return -1;
	for (i = 0; i < axis_json_length(result) && i < AXIS_DELUGE_MAX_LIST; i++) {
		const gchar *one = axis_json_string(axis_json_element(result, i));

		if (one == NULL || name == NULL)
			continue;
		if (fold ? g_ascii_strcasecmp(one, name) == 0 : strcmp(one, name) == 0)
			found = 1;
	}
	return found;
}

gint
axis_deluge_parse_has_name(
	const AxisJsonNode *result,
	const gchar        *name
){
	return has_name(result, name, TRUE);
}

gint
axis_deluge_parse_has_label(
	const AxisJsonNode *result,
	const gchar        *label
){
	return has_name(result, label, FALSE);
}

gboolean
axis_deluge_parse_host(
	const AxisJsonNode  *result,
	gchar              **id,
	gchar              **host,
	gint                *port
){
	guint i;

	*id = NULL;
	*host = NULL;
	*port = 0;
	if (axis_json_type(result) != AXIS_JSON_ARRAY)
		return FALSE;
	for (i = 0; i < axis_json_length(result) && i < AXIS_DELUGE_MAX_LIST; i++) {
		const AxisJsonNode *entry = axis_json_element(result, i);
		const gchar *host_id = axis_json_string(axis_json_element(entry, 0));
		gint64 number = 0;

		if (axis_json_type(entry) != AXIS_JSON_ARRAY || !axis_download_ref_valid(host_id))
			continue;
		*id = g_strdup(host_id);
		*host = axis_download_text_clean(axis_json_string(axis_json_element(entry, 1)), 100);
		if (axis_json_get_integer(axis_json_element(entry, 2), 1, 65535, &number))
			*port = (gint)number;
		return TRUE;
	}
	return FALSE;
}

/* ── A torrent Deluge does not have; the label's folder ─────────── */

gboolean
axis_deluge_says_not_in_session(
	const gchar *words,
	const gchar *hash
){
	g_autofree gchar *lower = NULL;
	g_autofree gchar *wanted = NULL;
	g_autofree gchar *hash_lower = NULL;

	if (words == NULL || !hash_text_valid(hash))
		return FALSE;
	lower = g_ascii_strdown(words, -1);
	hash_lower = g_ascii_strdown(hash, -1);
	wanted = g_strdup_printf("torrent_id %s not in session", hash_lower);
	return strstr(lower, "invalidtorrenterror") != NULL && strstr(lower, wanted) != NULL;
}

static gboolean plain_text_ok(const gchar *text);

static gboolean
exactly_true(const AxisJsonNode *node)
{
	gboolean value = FALSE;

	return axis_json_type(node) == AXIS_JSON_BOOLEAN && axis_json_get_boolean(node, &value) && value;
}

gint
axis_deluge_parse_label_folder(
	const AxisJsonNode  *result,
	gchar              **folder
){
	const gchar *path;

	*folder = NULL;
	if (axis_json_type(result) != AXIS_JSON_OBJECT)
		return -1;
	if (!exactly_true(axis_json_member(result, "apply_move_completed")) ||
	    !exactly_true(axis_json_member(result, "move_completed")))
		return 0;
	path = axis_json_string(axis_json_member(result, "move_completed_path"));
	if (!plain_text_ok(path))
		return 0;
	*folder = g_strdup(path);
	return 1;
}

gchar *
axis_deluge_parse_config_text(
	const AxisJsonNode *result,
	const gchar        *key
){
	const gchar *text;

	if (axis_json_type(result) != AXIS_JSON_OBJECT || key == NULL)
		return NULL;
	text = axis_json_string(axis_json_member(result, key));
	return plain_text_ok(text) ? g_strdup(text) : NULL;
}

GPtrArray *
axis_deluge_parse_labels(const AxisJsonNode *result)
{
	GPtrArray *out;
	guint i;

	if (axis_json_type(result) != AXIS_JSON_ARRAY)
		return NULL;
	out = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(result) && i < AXIS_DELUGE_MAX_LIST; i++) {
		const gchar *one = axis_json_string(axis_json_element(result, i));

		if (axis_deluge_label_valid(one))
			g_ptr_array_add(out, g_strdup(one));
	}
	return out;
}

/* ── The envelope ───────────────────────────────────────────────── */

static gboolean
says_not_authenticated(const gchar *text)
{
	g_autofree gchar *lower = NULL;

	if (text == NULL)
		return FALSE;
	lower = g_ascii_strdown(text, -1);
	return strstr(lower, "not authenticated") != NULL;
}

AxisDelugeReplyKind
axis_deluge_reply_parse(
	const gchar         *data,
	gsize                length,
	gint64               id,
	AxisJsonDoc        **doc,
	const AxisJsonNode **result,
	gchar              **message
){
	g_autofree gchar *problem = NULL;
	AxisJsonDoc *parsed;
	const AxisJsonNode *root;
	const AxisJsonNode *answered;
	const AxisJsonNode *error;
	const AxisJsonNode *value;
	gint64 number = 0;

	*doc = NULL;
	*result = NULL;
	*message = NULL;
	if (data == NULL || length == 0) {
		*message = g_strdup("it is empty");
		return AXIS_DELUGE_REPLY_UNREADABLE;
	}
	parsed = axis_json_parse(data, length, &problem);
	if (parsed == NULL) {
		*message = g_strdup(problem != NULL ? problem : "it is not JSON");
		return AXIS_DELUGE_REPLY_UNREADABLE;
	}
	root = axis_json_doc_get_root(parsed);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		axis_json_doc_free(parsed);
		*message = g_strdup("it is not an object");
		return AXIS_DELUGE_REPLY_UNREADABLE;
	}
	/* Its id is the one sent: an answer to another request is not this one's. */
	answered = axis_json_member(root, "id");
	if (axis_json_type(answered) != AXIS_JSON_NUMBER || !axis_json_get_integer(answered, G_MININT64, G_MAXINT64, &number) ||
	    number != id) {
		axis_json_doc_free(parsed);
		*message = g_strdup("it does not carry the id of the request");
		return AXIS_DELUGE_REPLY_UNREADABLE;
	}
	error = axis_json_member(root, "error");
	value = axis_json_member(root, "result");
	if (error != NULL && axis_json_type(error) != AXIS_JSON_NULL) {
		gint64 code = 0;
		gboolean has_code;
		AxisDelugeReplyKind kind;
		gchar *words;

		if (axis_json_type(error) != AXIS_JSON_OBJECT || (value != NULL && axis_json_type(value) != AXIS_JSON_NULL)) {
			*message = g_strdup(axis_json_type(error) != AXIS_JSON_OBJECT ? "its error is not an object" :
				"it has both a result and an error");
			axis_json_doc_free(parsed);
			return AXIS_DELUGE_REPLY_UNREADABLE;
		}
		has_code = axis_json_type(axis_json_member(error, "code")) == AXIS_JSON_NUMBER &&
			axis_json_get_integer(axis_json_member(error, "code"), G_MININT32, G_MAXINT32, &code);
		words = text_member(error, "message", AXIS_DELUGE_ERROR_MAX_CHARS);
		kind = (has_code && code == 1) || says_not_authenticated(words) ? AXIS_DELUGE_REPLY_NOT_AUTH :
			AXIS_DELUGE_REPLY_ERROR;
		axis_json_doc_free(parsed);
		*message = words;
		return kind;
	}
	if (value == NULL) {
		axis_json_doc_free(parsed);
		*message = g_strdup("it has neither a result nor an error");
		return AXIS_DELUGE_REPLY_UNREADABLE;
	}
	*doc = parsed;
	*result = value;
	return AXIS_DELUGE_REPLY_OK;
}

/* ── Torrents ───────────────────────────────────────────────────── */

void
axis_deluge_slot_free(AxisDelugeSlot *slot)
{
	if (slot == NULL)
		return;
	axis_download_item_free(slot->item);
	g_free(slot->word);
	g_free(slot);
}

typedef enum
{
	PHASE_UNKNOWN,
	PHASE_QUEUED,
	PHASE_DOWNLOADING,
	PHASE_PAUSED,
	PHASE_MOVING,	/* downloaded, not yet where it will be */
	PHASE_SEEDING,
	PHASE_ERROR
} Phase;

static Phase
state_phase(const gchar *word)
{
	static const struct
	{
		const gchar *word;
		Phase        phase;
	} table[] = {
		{ "Downloading", PHASE_DOWNLOADING }, { "Queued", PHASE_QUEUED }, { "Checking", PHASE_QUEUED },
		{ "Allocating", PHASE_QUEUED }, { "Paused", PHASE_PAUSED }, { "Moving", PHASE_MOVING },
		{ "Seeding", PHASE_SEEDING }, { "Error", PHASE_ERROR }
	};
	guint i;

	if (word == NULL)
		return PHASE_UNKNOWN;
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (g_ascii_strcasecmp(word, table[i].word) == 0)
			return table[i].phase;
	}
	return PHASE_UNKNOWN;
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

/* The torrent's files' place: "download_location" and the torrent's own
 * name.  The name is a single path segment or nothing. */
static gchar *
path_of(
	const AxisJsonNode *status,
	const gchar        *name
){
	const gchar *location = axis_json_string(axis_json_member(status, "download_location"));
	gsize n;

	if (!plain_text_ok(location) || !plain_text_ok(name) || strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return NULL;
	if (location[0] != '/')
		return NULL;
	n = strlen(location);
	while (n > 1 && location[n - 1] == '/')
		n--;
	if (n + 1 + strlen(name) > AXIS_DOWNLOAD_PATH_MAX)
		return NULL;
	if (n == 1)
		return g_strconcat("/", name, NULL);
	return g_strdup_printf("%.*s/%s", (int)n, location, name);
}

/* A slot for an entry that cannot be read but by its hash. */
static AxisDelugeSlot *
unreadable_slot(AxisDownloadItem *item)
{
	AxisDelugeSlot *slot = g_new0(AxisDelugeSlot, 1);

	slot->item = item;
	return slot;
}

static AxisDelugeSlot *
torrent_slot(
	const gchar        *key,
	const AxisJsonNode *status
){
	AxisDelugeSlot *slot;
	AxisDownloadItem *item;
	const gchar *raw_name;
	gchar *word;
	gchar *label;
	Phase phase;
	gboolean finished = FALSE;
	gint64 size = -1;
	gint64 done = -1;
	gint64 rate = -1;
	gint64 eta = -1;
	gdouble percent = -1;
	gboolean seen_finished;
	const AxisJsonNode *finished_node;

	if (!hash_text_valid(key))
		return NULL;
	item = axis_download_item_new();
	item->client_ref = g_ascii_strdown(key, -1);
	item->info_hash = g_strdup(item->client_ref);
	/* Its hash read, the rest not: returned as one that cannot be read, so
	 * that the layer knows which torrent it is. */
	if (axis_json_type(status) != AXIS_JSON_OBJECT) {
		item->unreadable = g_strdup("Deluge's entry for it is not an object");
		return unreadable_slot(item);
	}
	item->name = text_member(status, "name", AXIS_DOWNLOAD_NAME_MAX_CHARS);
	label = text_member(status, "label", 64);
	/* No label at all ("") is not Axis's label: another torrent's, not one
	 * that cannot be read. */
	if (label == NULL && g_strcmp0(axis_json_string(axis_json_member(status, "label")), "") == 0)
		label = g_strdup("");
	item->category = label;
	if (item->name == NULL || label == NULL) {
		item->unreadable = g_strdup(item->name == NULL ? "it has no name Axis can read" :
			"its label cannot be read (the Label plugin did not answer for it)");
		return unreadable_slot(item);
	}
	raw_name = axis_json_string(axis_json_member(status, "name"));
	/* The name exactly as Deluge gave it, for comparing with names on disk
	 * (axis_download_item_check() keeps it only when it can be kept). */
	item->own_name = g_strdup(raw_name);

	word = text_member(status, "state", AXIS_DELUGE_WORD_MAX);
	phase = state_phase(word);
	finished_node = axis_json_member(status, "is_finished");
	seen_finished = axis_json_type(finished_node) == AXIS_JSON_BOOLEAN && axis_json_get_boolean(finished_node, &finished);

	if (!axis_json_get_integer(axis_json_member(status, "total_wanted"), 0, BYTES_MAX, &size))
		size = -1;
	if (!axis_json_get_integer(axis_json_member(status, "total_done"), 0, BYTES_MAX, &done))
		done = -1;
	if (axis_json_type(axis_json_member(status, "progress")) != AXIS_JSON_NULL &&
	    !axis_json_get_double(axis_json_member(status, "progress"), 0, 100, &percent))
		percent = -1;
	if (!axis_json_get_integer(axis_json_member(status, "download_payload_rate"), 0, RATE_MAX, &rate))
		rate = -1;
	if (!axis_json_get_integer(axis_json_member(status, "eta"), 0, TIME_LEFT_MAX_S, &eta) || eta == 0)
		eta = -1;
	item->size_bytes = size;
	if (size >= 0 && done >= 0)
		item->done_bytes = done > size ? size : done;
	else if (size >= 0 && percent >= 0)
		item->done_bytes = (gint64)((gdouble)size * percent / 100.0);

	/* Where its files are now, in progress too (its save path); not while
	 * Deluge moves them. */
	if (phase != PHASE_MOVING)
		item->place = path_of(status, raw_name);
	if (phase == PHASE_MOVING) {
		/* Moving wins over finished: the files are not where they will be. */
		item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
		item->done_bytes = size;
	} else if (seen_finished && finished) {
		/* Finished is completed, and stays so while it seeds; whatever the
		 * state word says. */
		item->state = AXIS_DOWNLOAD_ITEM_COMPLETED;
		item->done_bytes = size;
		item->path = path_of(status, raw_name);
		phase = PHASE_SEEDING;
	} else {
		switch (phase) {
		case PHASE_DOWNLOADING:
			item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
			item->rate_bps = rate;
			item->eta_s = eta;
			break;
		case PHASE_PAUSED:
			item->state = AXIS_DOWNLOAD_ITEM_PAUSED;
			break;
		case PHASE_SEEDING:
			/* Seeding with is_finished absent or false: finished by the
			 * state's own word. */
			item->state = AXIS_DOWNLOAD_ITEM_COMPLETED;
			item->done_bytes = size;
			item->path = path_of(status, raw_name);
			break;
		case PHASE_ERROR:
			item->state = AXIS_DOWNLOAD_ITEM_FAILED;
			item->message = text_member(status, "tracker_status", AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
			if (item->message == NULL)
				item->message = g_strdup("Deluge reports this torrent as in error");
			break;
		default:
			/* Queued, or a word not known (the kind decides what that means). */
			item->state = AXIS_DOWNLOAD_ITEM_QUEUED;
			break;
		}
	}
	slot = g_new0(AxisDelugeSlot, 1);
	slot->item = item;
	slot->word = word;
	slot->known = phase != PHASE_UNKNOWN;
	return slot;
}

gint
axis_deluge_parse_label_of(
	const AxisJsonNode  *result,
	const gchar         *hash,
	gchar              **label,
	gchar              **name
){
	guint i;

	*label = NULL;
	if (name != NULL)
		*name = NULL;
	if (axis_json_type(result) != AXIS_JSON_OBJECT || !hash_text_valid(hash))
		return -1;
	for (i = 0; i < axis_json_length(result) && i < AXIS_DELUGE_MAX_TORRENTS; i++) {
		const gchar *key = axis_json_member_name(result, i);
		const AxisJsonNode *status = axis_json_member_value(result, i);
		const gchar *text;

		if (key == NULL || g_ascii_strcasecmp(key, hash) != 0)
			continue;
		if (axis_json_type(status) != AXIS_JSON_OBJECT)
			return -1;
		/* No "label" at all: the Label plugin did not answer for it. */
		text = axis_json_string(axis_json_member(status, "label"));
		if (text == NULL || strlen(text) > LABEL_MAX)
			return -1;
		*label = g_strdup(text);
		if (name != NULL)
			*name = text_member(status, "name", AXIS_DOWNLOAD_NAME_MAX_CHARS);
		return 1;
	}
	/* More than was looked at (Deluge left out the filter and answered every
	 * torrent): what was not looked at may be it, so nothing can be told. */
	if (axis_json_length(result) > AXIS_DELUGE_MAX_TORRENTS)
		return -1;
	return 0;
}

GPtrArray *
axis_deluge_parse_torrents(const AxisJsonNode *result)
{
	return axis_deluge_parse_torrents_counted(result, NULL, NULL);
}

GPtrArray *
axis_deluge_parse_torrents_counted(
	const AxisJsonNode *result,
	guint              *unreadable,
	gboolean           *cut
){
	GPtrArray *out;
	g_autoptr(GHashTable) seen = NULL;
	guint i;

	if (unreadable != NULL)
		*unreadable = 0;
	if (cut != NULL)
		*cut = FALSE;
	if (axis_json_type(result) != AXIS_JSON_OBJECT)
		return NULL;
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_deluge_slot_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < axis_json_length(result); i++) {
		AxisDelugeSlot *slot;

		if (out->len >= AXIS_DELUGE_MAX_TORRENTS) {
			/* The rest is not looked at: the list is not whole. */
			if (cut != NULL)
				*cut = TRUE;
			break;
		}
		/* One whose hash cannot be read is counted; one whose hash is read and
		 * nothing else is a slot that says so (its item's @unreadable). */
		slot = torrent_slot(axis_json_member_name(result, i), axis_json_member_value(result, i));
		if (slot == NULL) {
			if (unreadable != NULL)
				(*unreadable)++;
			continue;
		}
		/* A hash once (the keys of one object may differ only in case). */
		if (g_hash_table_contains(seen, slot->item->client_ref)) {
			axis_deluge_slot_free(slot);
			continue;
		}
		g_hash_table_add(seen, slot->item->client_ref);
		g_ptr_array_add(out, slot);
	}
	return out;
}
