/*
 * axis-download-sabnzbd-parse.c - SABnzbd's JSON answers, read into the
 * downloads layer's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-sabnzbd-parse.h"

#include <string.h>

#include "storage/axis-storage-path.h"

/* The longest identifier SABnzbd gives a job that is kept. */
#define REF_MAX 64

/* SABnzbd's own words about a refusal, cut. */
#define ERROR_MAX_CHARS 200

/* A time left of more than this (ten years) is not believed. */
#define TIME_LEFT_MAX_S (10 * 365 * 86400)

/* The most bytes a history entry may say it is (a petabyte). */
#define BYTES_MAX ((gint64)1 << 50)

/* ── Fields ─────────────────────────────────────────────────────── */

static gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_download_text_clean(axis_json_string(axis_json_member(object, name)), max_chars);
}

/* A string member that is a job identifier, else %NULL. */
static gchar *
ref_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const gchar *text = axis_json_string(axis_json_member(object, name));

	return axis_sabnzbd_ref_valid(text) ? g_strdup(text) : NULL;
}

gboolean
axis_sabnzbd_ref_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n == 0 || n > REF_MAX)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_isalnum(text[i]) && text[i] != '_' && text[i] != '.' && text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_sabnzbd_megabytes_parse(
	const AxisJsonNode *node,
	gint64             *bytes
){
	gdouble megabytes = 0;

	if (axis_json_type(node) != AXIS_JSON_STRING && axis_json_type(node) != AXIS_JSON_NUMBER)
		return FALSE;
	if (!axis_json_get_double(node, 0, AXIS_SABNZBD_MAX_MEGABYTES, &megabytes))
		return FALSE;
	*bytes = (gint64)(megabytes * 1048576.0 + 0.5);
	return TRUE;
}

gint64
axis_sabnzbd_timeleft_parse(const gchar *text)
{
	gint64 parts[4];
	guint n = 0;
	gint64 seconds = 0;
	const gchar *p = text;
	guint i;

	if (text == NULL || text[0] == '\0')
		return -1;
	while (TRUE) {
		gint64 value = 0;
		guint digits = 0;

		while (g_ascii_isdigit(*p) && digits < 7) {
			value = value * 10 + (*p - '0');
			digits++;
			p++;
		}
		if (digits == 0 || n >= G_N_ELEMENTS(parts))
			return -1;
		parts[n++] = value;
		if (*p == '\0')
			break;
		if (*p != ':')
			return -1;
		p++;
	}
	if (n < 2)
		return -1;
	/* The last is seconds, then minutes, hours, days. */
	for (i = 0; i < n; i++) {
		static const gint64 unit[] = { 1, 60, 3600, 86400 };

		seconds += parts[n - 1 - i] * unit[i];
		if (seconds > TIME_LEFT_MAX_S)
			return -1;
	}
	return seconds;
}

/* ── Replies ────────────────────────────────────────────────────── */

static gboolean
mentions_key(const gchar *text)
{
	g_autofree gchar *lower = NULL;

	if (text == NULL)
		return FALSE;
	lower = g_ascii_strdown(text, -1);
	return strstr(lower, "api key") != NULL || strstr(lower, "apikey") != NULL || strstr(lower, "api-key") != NULL;
}

AxisSabnzbdReplyKind
axis_sabnzbd_reply_parse(
	const gchar   *data,
	gsize          length,
	AxisJsonDoc  **doc,
	gchar        **message
){
	g_autofree gchar *problem = NULL;
	g_autofree gchar *words = NULL;
	AxisJsonDoc *parsed;
	const AxisJsonNode *root;
	const AxisJsonNode *status;
	gboolean failed = FALSE;

	*doc = NULL;
	*message = NULL;
	if (data == NULL || length == 0) {
		*message = g_strdup("it is empty");
		return AXIS_SABNZBD_REPLY_UNREADABLE;
	}
	parsed = axis_json_parse(data, length, &problem);
	if (parsed == NULL) {
		/* An old SABnzbd says a wrong key in plain text whatever is asked. */
		g_autofree gchar *head = g_strndup(data, MIN(length, (gsize)200));

		g_strstrip(head);
		if (g_ascii_strncasecmp(head, "error", 5) == 0 && mentions_key(head)) {
			*message = g_strdup("the API key was refused");
			return AXIS_SABNZBD_REPLY_BAD_KEY;
		}
		*message = g_strdup(problem != NULL ? problem : "it is not JSON");
		return AXIS_SABNZBD_REPLY_UNREADABLE;
	}
	root = axis_json_doc_get_root(parsed);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		axis_json_doc_free(parsed);
		*message = g_strdup("it is not an object");
		return AXIS_SABNZBD_REPLY_UNREADABLE;
	}
	status = axis_json_member(root, "status");
	if (axis_json_type(status) == AXIS_JSON_BOOLEAN && axis_json_get_boolean(status, &failed))
		failed = !failed;
	else
		failed = FALSE;
	if (!failed) {
		*doc = parsed;
		return AXIS_SABNZBD_REPLY_OK;
	}
	words = text_member(root, "error", ERROR_MAX_CHARS);
	axis_json_doc_free(parsed);
	*message = g_steal_pointer(&words);
	return mentions_key(*message) ? AXIS_SABNZBD_REPLY_BAD_KEY : AXIS_SABNZBD_REPLY_REFUSED;
}

gchar *
axis_sabnzbd_parse_version(const AxisJsonNode *root)
{
	return text_member(root, "version", 64);
}

gint
axis_sabnzbd_parse_categories(
	const AxisJsonNode *root,
	const gchar        *category
){
	const AxisJsonNode *list = axis_json_member(root, "categories");
	gint found = 0;
	guint i;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return -1;
	for (i = 0; i < axis_json_length(list) && i < AXIS_SABNZBD_MAX_SLOTS; i++) {
		const gchar *name = axis_json_string(axis_json_element(list, i));

		if (name != NULL && category != NULL && g_ascii_strcasecmp(name, category) == 0)
			found = 1;
	}
	return found;
}

/* ── Folders ────────────────────────────────────────────────────── */

/* A folder setting as it is, when it can be one: a string of at most
 * AXIS_SABNZBD_PATH_MAX bytes of valid UTF-8 without control characters. */
static gchar *
path_text(const AxisJsonNode *node)
{
	const gchar *text = axis_json_string(node);
	gsize n;
	gsize i;

	if (text == NULL)
		return NULL;
	n = strlen(text);
	if (n > AXIS_SABNZBD_PATH_MAX || !g_utf8_validate(text, (gssize)n, NULL))
		return NULL;
	for (i = 0; i < n; i++) {
		if ((guchar)text[i] < 0x20 || text[i] == 0x7f)
			return NULL;
	}
	return g_strdup(text);
}

/* "config" of @root when there is one, else @root itself. */
static const AxisJsonNode *
config_of(const AxisJsonNode *root)
{
	const AxisJsonNode *config = axis_json_member(root, "config");

	return axis_json_type(config) == AXIS_JSON_OBJECT ? config : root;
}

gint
axis_sabnzbd_parse_category_dir(
	const AxisJsonNode  *root,
	const gchar         *category,
	gchar              **dir
){
	const AxisJsonNode *list = axis_json_member(config_of(root), "categories");
	guint i;

	*dir = NULL;
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return -1;
	for (i = 0; i < axis_json_length(list) && i < AXIS_SABNZBD_MAX_SLOTS; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const gchar *name = axis_json_string(axis_json_member(entry, "name"));

		if (name == NULL || category == NULL || g_ascii_strcasecmp(name, category) != 0)
			continue;
		*dir = path_text(axis_json_member(entry, "dir"));
		return *dir != NULL ? 1 : -1;
	}
	return 0;
}

gboolean
axis_sabnzbd_parse_other_dirs(
	const AxisJsonNode  *root,
	const gchar         *category,
	GPtrArray          **names,
	GPtrArray          **dirs
){
	const AxisJsonNode *list = axis_json_member(config_of(root), "categories");
	guint i;

	*names = NULL;
	*dirs = NULL;
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return FALSE;
	*names = g_ptr_array_new_with_free_func(g_free);
	*dirs = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(list) && i < AXIS_SABNZBD_MAX_SLOTS; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const gchar *name = axis_json_string(axis_json_member(entry, "name"));

		if (name == NULL || (category != NULL && g_ascii_strcasecmp(name, category) == 0))
			continue;
		g_ptr_array_add(*names, g_strdup(name));
		g_ptr_array_add(*dirs, path_text(axis_json_member(entry, "dir")));
	}
	return TRUE;
}

gchar *
axis_sabnzbd_parse_complete_dir(const AxisJsonNode *root)
{
	const AxisJsonNode *misc = axis_json_member(config_of(root), "misc");

	if (axis_json_type(misc) != AXIS_JSON_OBJECT)
		return NULL;
	return path_text(axis_json_member(misc, "complete_dir"));
}

gboolean
axis_sabnzbd_category_folder(
	const gchar  *complete_dir,
	const gchar  *dir,
	gchar       **folder,
	const gchar **problem
){
	g_autofree gchar *setting = g_strstrip(g_strdup(dir != NULL ? dir : ""));
	g_autofree gchar *base = NULL;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *normal = NULL;
	gsize n;

	*folder = NULL;
	*problem = NULL;
	/* "dir*": the job's files go straight into the folder. */
	n = strlen(setting);
	while (n > 0 && setting[n - 1] == '*')
		setting[--n] = '\0';
	g_strstrip(setting);
	if (setting[0] == '\0') {
		*problem = "SABnzbd's category for Axis has no folder of its own (its folder setting is empty), so its jobs "
			"sit among every other category's";
		return FALSE;
	}
	if (complete_dir != NULL && complete_dir[0] != '\0' &&
	    !axis_storage_path_normalize(complete_dir, TRUE, &base, NULL))
		g_clear_pointer(&base, g_free);
	if (setting[0] == '/') {
		joined = g_strdup(setting);
	} else {
		if (base == NULL) {
			*problem = "the category's folder is relative to SABnzbd's folder for finished downloads, and SABnzbd "
				"did not give that as a full path";
			return FALSE;
		}
		joined = g_strconcat(base, "/", setting, NULL);
	}
	if (!axis_storage_path_normalize(joined, TRUE, &normal, NULL)) {
		*problem = "SABnzbd's folder for the category cannot be used (it has a '.' or '..' part, or a character a "
			"path here cannot have)";
		return FALSE;
	}
	if (base != NULL && strcmp(base, normal) == 0) {
		*problem = "SABnzbd's category for Axis has no folder of its own (it is SABnzbd's folder for every finished "
			"download)";
		return FALSE;
	}
	*folder = g_steal_pointer(&normal);
	return TRUE;
}

gint
axis_sabnzbd_folder_shared(
	const gchar  *complete_dir,
	const gchar  *folder,
	const gchar  *dir,
	gchar       **theirs
){
	g_autofree gchar *setting = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *normal = NULL;
	gsize n;

	*theirs = NULL;
	if (dir == NULL || folder == NULL)
		return -1;
	setting = g_strstrip(g_strdup(dir));
	n = strlen(setting);
	while (n > 0 && setting[n - 1] == '*')
		setting[--n] = '\0';
	g_strstrip(setting);
	/* No folder of its own: its jobs go into the folder for every finished
	 * download, each in a folder of its own beside Axis's folder. */
	if (setting[0] == '\0')
		return 0;
	if (complete_dir != NULL && complete_dir[0] != '\0' &&
	    !axis_storage_path_normalize(complete_dir, TRUE, &base, NULL))
		g_clear_pointer(&base, g_free);
	if (setting[0] == '/')
		joined = g_strdup(setting);
	else if (base != NULL)
		joined = g_strconcat(base, "/", setting, NULL);
	else
		return -1;
	if (!axis_storage_path_normalize(joined, TRUE, &normal, NULL))
		return -1;
	/* The folder for every finished download: around Axis's, as usual. */
	if (base != NULL && strcmp(base, normal) == 0)
		return 0;
	if (!axis_storage_path_contains(folder, normal))
		return 0;
	*theirs = g_steal_pointer(&normal);
	return 1;
}

gboolean
axis_sabnzbd_parse_added(
	const AxisJsonNode  *root,
	gchar              **nzo_id,
	gchar              **message
){
	const AxisJsonNode *status = axis_json_member(root, "status");
	const AxisJsonNode *ids = axis_json_member(root, "nzo_ids");
	g_autofree gchar *words = text_member(root, "error", ERROR_MAX_CHARS);
	const gchar *why = NULL;
	gboolean ok = FALSE;

	*nzo_id = NULL;
	*message = NULL;
	if (axis_json_type(status) != AXIS_JSON_BOOLEAN || !axis_json_get_boolean(status, &ok) || !ok)
		why = "SABnzbd did not say that it took the file";
	else if (axis_json_type(ids) != AXIS_JSON_ARRAY || axis_json_length(ids) == 0)
		why = "SABnzbd took the file but made no job from it";
	else if (axis_json_length(ids) != 1)
		why = "SABnzbd made more than one job from the file";
	else {
		const gchar *id = axis_json_string(axis_json_element(ids, 0));

		if (!axis_sabnzbd_ref_valid(id))
			why = "SABnzbd gave no job identifier Axis can use";
		else
			*nzo_id = g_strdup(id);
	}
	if (why != NULL) {
		*message = words != NULL ? g_strdup_printf("%s: %s", why, words) : g_strdup(why);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_sabnzbd_parse_removed(
	const AxisJsonNode *root,
	const gchar        *nzo_id
){
	const AxisJsonNode *ids = axis_json_member(root, "nzo_ids");
	guint i;

	if (axis_json_type(ids) != AXIS_JSON_ARRAY || nzo_id == NULL)
		return FALSE;
	for (i = 0; i < axis_json_length(ids) && i < AXIS_SABNZBD_MAX_SLOTS; i++) {
		if (g_strcmp0(axis_json_string(axis_json_element(ids, i)), nzo_id) == 0)
			return TRUE;
	}
	return FALSE;
}

/* ── Slots ──────────────────────────────────────────────────────── */

void
axis_sabnzbd_slot_free(AxisSabnzbdSlot *slot)
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
	PHASE_AFTER,	/* post-processing: downloaded, not yet where it will be */
	PHASE_COMPLETED,
	PHASE_FAILED
} Phase;

static gboolean
word_is(
	const gchar *word,
	const gchar *wanted
){
	return word != NULL && g_ascii_strcasecmp(word, wanted) == 0;
}

static gboolean
word_in(
	const gchar        *word,
	const gchar *const *list
){
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		if (word_is(word, list[i]))
			return TRUE;
	}
	return FALSE;
}

static Phase
queue_phase(const gchar *word)
{
	static const gchar *const downloading[] = { "Downloading", "Fetching", "Grabbing", NULL };
	static const gchar *const queued[] = { "Queued", "Propagating", NULL };

	if (word_in(word, downloading))
		return PHASE_DOWNLOADING;
	if (word_in(word, queued))
		return PHASE_QUEUED;
	if (word_is(word, "Paused"))
		return PHASE_PAUSED;
	return PHASE_UNKNOWN;
}

static Phase
history_phase(const gchar *word)
{
	static const gchar *const after[] = {
		"Verifying", "Repairing", "Extracting", "Moving", "Running", "QuickCheck", "Queued", "Fetching", NULL
	};

	if (word_is(word, "Completed"))
		return PHASE_COMPLETED;
	if (word_is(word, "Failed"))
		return PHASE_FAILED;
	if (word_in(word, after))
		return PHASE_AFTER;
	return PHASE_UNKNOWN;
}

/* A final folder: text without control characters, within the layer's
 * bound; else %NULL.  Not cleaned (a folder's own spaces are its name). */
static gchar *
path_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const gchar *text = axis_json_string(axis_json_member(object, name));
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > AXIS_DOWNLOAD_PATH_MAX || !g_utf8_validate(text, -1, NULL))
		return NULL;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return NULL;
	}
	return g_strdup(text);
}

static AxisSabnzbdSlot *
slot_new(
	AxisDownloadItem *item,
	gchar            *word,
	Phase             phase,
	gboolean          history
){
	AxisSabnzbdSlot *slot = g_new0(AxisSabnzbdSlot, 1);

	slot->item = item;
	slot->word = word;
	slot->known = phase != PHASE_UNKNOWN;
	slot->history = history;
	return slot;
}

/* A slot for a job whose identifier is read and nothing else that matters:
 * returned so that the layer knows which job it is. */
static AxisSabnzbdSlot *
unreadable_slot(
	AxisDownloadItem *item,
	gboolean          history
){
	item->unreadable = g_strdup(item->name == NULL ? "it has no name Axis can read" :
		"its category cannot be read");
	return slot_new(item, NULL, PHASE_UNKNOWN, history);
}

static AxisSabnzbdSlot *
queue_slot(const AxisJsonNode *node)
{
	AxisDownloadItem *item;
	gchar *word;
	Phase phase;
	gint64 size = -1;
	gint64 left = -1;
	gdouble percent = -1;

	if (axis_json_type(node) != AXIS_JSON_OBJECT)
		return NULL;
	item = axis_download_item_new();
	item->client_ref = ref_member(node, "nzo_id");
	item->name = text_member(node, "filename", AXIS_DOWNLOAD_NAME_MAX_CHARS);
	/* The name exactly as SABnzbd gave it, for comparing with names on disk
	 * (axis_download_item_check() keeps it only when it can be kept). */
	item->own_name = g_strdup(axis_json_string(axis_json_member(node, "filename")));
	item->category = text_member(node, "cat", 64);
	if (item->client_ref == NULL) {
		axis_download_item_free(item);
		return NULL;
	}
	if (item->name == NULL || item->category == NULL)
		return unreadable_slot(item, FALSE);
	word = text_member(node, "status", AXIS_SABNZBD_WORD_MAX);
	phase = queue_phase(word);
	switch (phase) {
	case PHASE_DOWNLOADING:
		item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
		break;
	case PHASE_PAUSED:
		item->state = AXIS_DOWNLOAD_ITEM_PAUSED;
		break;
	default:
		/* Queued, or a word not known (the kind decides what that means). */
		item->state = AXIS_DOWNLOAD_ITEM_QUEUED;
		break;
	}
	if (!axis_sabnzbd_megabytes_parse(axis_json_member(node, "mb"), &size))
		size = -1;
	if (!axis_sabnzbd_megabytes_parse(axis_json_member(node, "mbleft"), &left))
		left = -1;
	if (axis_json_type(axis_json_member(node, "percentage")) != AXIS_JSON_NULL &&
	    !axis_json_get_double(axis_json_member(node, "percentage"), 0, 100, &percent))
		percent = -1;
	item->size_bytes = size;
	if (size >= 0 && left >= 0)
		item->done_bytes = left >= size ? 0 : size - left;
	else if (size >= 0 && percent >= 0)
		item->done_bytes = (gint64)((gdouble)size * percent / 100.0);
	if (phase == PHASE_DOWNLOADING)
		item->eta_s = axis_sabnzbd_timeleft_parse(axis_json_string(axis_json_member(node, "timeleft")));
	return slot_new(item, word, phase, FALSE);
}

static AxisSabnzbdSlot *
history_slot(const AxisJsonNode *node)
{
	AxisDownloadItem *item;
	gchar *word;
	Phase phase;
	gint64 bytes = -1;

	if (axis_json_type(node) != AXIS_JSON_OBJECT)
		return NULL;
	item = axis_download_item_new();
	item->client_ref = ref_member(node, "nzo_id");
	item->name = text_member(node, "name", AXIS_DOWNLOAD_NAME_MAX_CHARS);
	/* The name exactly as SABnzbd gave it, for comparing with names on disk
	 * (axis_download_item_check() keeps it only when it can be kept). */
	item->own_name = g_strdup(axis_json_string(axis_json_member(node, "name")));
	if (item->name == NULL) {
		const gchar *raw = axis_json_string(axis_json_member(node, "nzb_name"));

		/* The upload's own name, without its extension. */
		item->name = text_member(node, "nzb_name", AXIS_DOWNLOAD_NAME_MAX_CHARS);
		if (item->name != NULL && g_str_has_suffix(item->name, ".nzb") && strlen(item->name) > 4)
			item->name[strlen(item->name) - 4] = '\0';
		g_free(item->own_name);
		item->own_name = raw != NULL && g_str_has_suffix(raw, ".nzb") && strlen(raw) > 4 ?
			g_strndup(raw, strlen(raw) - 4) : g_strdup(raw);
	}
	item->category = text_member(node, "category", 64);
	if (item->client_ref == NULL) {
		axis_download_item_free(item);
		return NULL;
	}
	if (item->name == NULL || item->category == NULL)
		return unreadable_slot(item, TRUE);
	word = text_member(node, "status", AXIS_SABNZBD_WORD_MAX);
	phase = history_phase(word);
	if (!axis_json_get_integer(axis_json_member(node, "bytes"), 0, BYTES_MAX, &bytes))
		bytes = -1;
	item->size_bytes = bytes;
	switch (phase) {
	case PHASE_COMPLETED:
		item->state = AXIS_DOWNLOAD_ITEM_COMPLETED;
		item->done_bytes = bytes;
		/* The final folder; without one it is never completed (the layer
		 * keeps it downloading and says why). */
		item->path = path_member(node, "storage");
		break;
	case PHASE_FAILED:
		item->state = AXIS_DOWNLOAD_ITEM_FAILED;
		item->message = text_member(node, "fail_message", AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
		break;
	default:
		/* The work after the download, or a word not known: the files are
		 * not where they will be, so it is still downloading, all of it. */
		item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
		item->done_bytes = bytes;
		break;
	}
	return slot_new(item, word, phase, TRUE);
}

static GPtrArray *
parse_slots(
	const AxisJsonNode *root,
	const gchar        *list_name,
	gboolean            history,
	guint              *unreadable
){
	const AxisJsonNode *list = axis_json_member(root, list_name);
	const AxisJsonNode *slots = axis_json_member(list, "slots");
	GPtrArray *out;
	g_autoptr(GHashTable) seen = NULL;
	guint i;

	if (unreadable != NULL)
		*unreadable = 0;
	if (axis_json_type(list) != AXIS_JSON_OBJECT || axis_json_type(slots) != AXIS_JSON_ARRAY)
		return NULL;
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_sabnzbd_slot_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < axis_json_length(slots) && out->len < AXIS_SABNZBD_MAX_SLOTS; i++) {
		const AxisJsonNode *node = axis_json_element(slots, i);
		AxisSabnzbdSlot *slot = history ? history_slot(node) : queue_slot(node);

		/* A slot whose identifier cannot be read is counted: the list is then
		 * not whole.  One whose identifier is read and not its name or its
		 * category is a slot that says so (its item's @unreadable). */
		if (slot == NULL) {
			if (unreadable != NULL)
				(*unreadable)++;
			continue;
		}
		/* An identifier once. */
		if (g_hash_table_contains(seen, slot->item->client_ref)) {
			axis_sabnzbd_slot_free(slot);
			continue;
		}
		g_hash_table_add(seen, slot->item->client_ref);
		g_ptr_array_add(out, slot);
	}
	return out;
}

GPtrArray *
axis_sabnzbd_parse_queue(const AxisJsonNode *root)
{
	return parse_slots(root, "queue", FALSE, NULL);
}

GPtrArray *
axis_sabnzbd_parse_history(const AxisJsonNode *root)
{
	return parse_slots(root, "history", TRUE, NULL);
}

GPtrArray *
axis_sabnzbd_parse_queue_counted(
	const AxisJsonNode *root,
	guint              *unreadable
){
	return parse_slots(root, "queue", FALSE, unreadable);
}

GPtrArray *
axis_sabnzbd_parse_history_counted(
	const AxisJsonNode *root,
	guint              *unreadable
){
	return parse_slots(root, "history", TRUE, unreadable);
}
