/*
 * axis-download-sabnzbd.c - the SABnzbd download client kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-sabnzbd.h"

#include <string.h>

#include "core/axis-http.h"
#include "download/axis-download-sabnzbd-parse.h"

#define API_PATH "/api"
#define FORM_TYPE "application/x-www-form-urlencoded"

/* At most this many jobs are remembered; more clears the memory (it only
 * makes the kind quieter and quicker, never righter). */
#define MEMORY_MAX 4096

/* At most this many status words are logged. */
#define WORDS_MAX 64

/* The longest file name made for an upload, before ".nzb". */
#define UPLOAD_NAME_MAX 100

/* What SABnzbd's host-name refusal says (in its plain-text body). */
#define HOSTNAME_CHECK_WORDS "Hostname verification failed"

/* ── What is remembered ─────────────────────────────────────────── */

G_LOCK_DEFINE_STATIC(memory);
static GHashTable *remembered;	/* (memory) "client\njob" -> 1 + 8 * in_history + state */
static GHashTable *logged_words;	/* (memory) gchar * */
static GPtrArray  *forced_boundaries;	/* (memory) gchar *: for tests, tried before any random one */

static gchar *
memory_key(
	AxisDownloadConn *conn,
	const gchar      *ref
){
	return g_strdup_printf("%s\n%s", axis_download_conn_get_label(conn), ref);
}

static void
remember(
	AxisDownloadConn      *conn,
	const gchar           *ref,
	AxisDownloadItemState  state,
	gboolean               history
){
	gchar *key = memory_key(conn, ref);

	G_LOCK(memory);
	if (remembered == NULL)
		remembered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (g_hash_table_size(remembered) >= MEMORY_MAX && !g_hash_table_contains(remembered, key))
		g_hash_table_remove_all(remembered);
	g_hash_table_insert(remembered, key, GUINT_TO_POINTER(1 + (history ? 8 : 0) + (guint)state));
	G_UNLOCK(memory);
}

static gboolean
recall(
	AxisDownloadConn      *conn,
	const gchar           *ref,
	AxisDownloadItemState *state,
	gboolean              *history
){
	g_autofree gchar *key = memory_key(conn, ref);
	gpointer value = NULL;
	gboolean found = FALSE;

	G_LOCK(memory);
	if (remembered != NULL && g_hash_table_lookup_extended(remembered, key, NULL, &value)) {
		guint code = GPOINTER_TO_UINT(value) - 1;

		*history = code >= 8;
		*state = (AxisDownloadItemState)(code & 7);
		found = TRUE;
	}
	G_UNLOCK(memory);
	return found;
}

static void
forget_job(
	AxisDownloadConn *conn,
	const gchar      *ref
){
	g_autofree gchar *key = memory_key(conn, ref);

	G_LOCK(memory);
	if (remembered != NULL)
		g_hash_table_remove(remembered, key);
	G_UNLOCK(memory);
}

void
axis_download_sabnzbd_forget(void)
{
	G_LOCK(memory);
	g_clear_pointer(&remembered, g_hash_table_unref);
	g_clear_pointer(&logged_words, g_hash_table_unref);
	g_clear_pointer(&forced_boundaries, g_ptr_array_unref);
	G_UNLOCK(memory);
}

void
axis_download_sabnzbd_set_boundaries(const gchar *const *boundaries)
{
	guint i;

	G_LOCK(memory);
	g_clear_pointer(&forced_boundaries, g_ptr_array_unref);
	if (boundaries != NULL) {
		forced_boundaries = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; boundaries[i] != NULL; i++)
			g_ptr_array_add(forced_boundaries, g_strdup(boundaries[i]));
	}
	G_UNLOCK(memory);
}

/* The next boundary to try: a forced one (tests), else a random one. */
static gchar *
draw_boundary(void)
{
	gchar *forced = NULL;

	G_LOCK(memory);
	if (forced_boundaries != NULL && forced_boundaries->len > 0)
		forced = g_ptr_array_steal_index(forced_boundaries, 0);
	G_UNLOCK(memory);
	if (forced != NULL)
		return forced;
	return g_strdup_printf("axis-%08x%08x%08x%08x", g_random_int(), g_random_int(), g_random_int(), g_random_int());
}

/* Logs a status word the kind does not know, once per word. */
static void
log_unknown_word(
	AxisDownloadConn *conn,
	const gchar      *word
){
	gboolean first = FALSE;
	const gchar *shown = word != NULL ? word : "(none)";

	G_LOCK(memory);
	if (logged_words == NULL)
		logged_words = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (!g_hash_table_contains(logged_words, shown) && g_hash_table_size(logged_words) < WORDS_MAX) {
		g_hash_table_add(logged_words, g_strdup(shown));
		first = TRUE;
	}
	G_UNLOCK(memory);
	if (first)
		g_message("downloads: SABnzbd client %s reported a job status Axis does not know (\"%s\"); jobs keep "
			"their last known state", axis_download_conn_get_label(conn), shown);
}

/* ── Requests ───────────────────────────────────────────────────── */

/* Bytes that are wiped when they are let go: a request body holds the API
 * key. */
typedef struct
{
	gchar *data;
	gsize  length;
} Wiped;

static void
wiped_free(gpointer user_data)
{
	Wiped *wiped = user_data;
	volatile gchar *p = wiped->data;
	gsize i;

	for (i = 0; i < wiped->length; i++)
		p[i] = '\0';
	g_free(wiped->data);
	g_free(wiped);
}

/* Takes @data. */
static GBytes *
bytes_wiped(
	gchar *data,
	gsize  length
){
	Wiped *wiped = g_new0(Wiped, 1);

	wiped->data = data;
	wiped->length = length;
	return g_bytes_new_with_free_func(data, length, wiped_free, wiped);
}

static void
form_add(
	GString     *form,
	const gchar *name,
	const gchar *value
){
	g_autofree gchar *escaped = g_uri_escape_string(value != NULL ? value : "", NULL, FALSE);

	if (form->len > 0)
		g_string_append_c(form, '&');
	g_string_append_printf(form, "%s=%s", name, escaped);
}

/* @text with the key blanked out and cleaned, for a message. */
static gchar *
scrubbed(
	AxisDownloadConn *conn,
	const gchar      *text
){
	return axis_http_scrub(text, axis_download_conn_get_secret(conn), AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
}

static gboolean
fail(
	GError           **error,
	AxisDownloadError  code,
	const gchar       *message
){
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, (gint)code, message);
	return FALSE;
}

/* The key SABnzbd is called with, or an authentication failure. */
static const gchar *
key_of(
	AxisDownloadConn  *conn,
	GError           **error
){
	const gchar *key = axis_download_conn_get_secret(conn);

	if (key == NULL)
		fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "no API key is set");
	return key;
}

/*
 * SABnzbd's host-name check: a request whose Host is not in its
 * "host_whitelist" is answered 403 with the plain text "Access denied -
 * Hostname verification failed: https://sabnzbd.org/hostname-check", whatever
 * the key.  Told apart from a refused key, which it is not.  Always %FALSE.
 */
static gboolean
hostname_refused(
	AxisDownloadConn  *conn,
	GError           **error
){
	g_autoptr(GUri) uri = g_uri_parse(axis_download_conn_get_address(conn), G_URI_FLAGS_NONE, NULL);
	const gchar *host = uri != NULL ? g_uri_get_host(uri) : NULL;
	g_autofree gchar *text = NULL;

	text = g_strdup_printf("SABnzbd refused the host name Axis used for it (%s), so nothing was asked of it. Add that "
		"name to host_whitelist in SABnzbd's Special settings, or use an address SABnzbd already accepts",
		host != NULL ? host : "the one in the client's address");
	return fail(error, AXIS_DOWNLOAD_ERROR_FAILED, text);
}

/*
 * Posts @body and reads the answer: the document of an answer that is not a
 * refusal.  A refusal inside a 200 (status false) is
 * %AXIS_DOWNLOAD_ERROR_REFUSED with SABnzbd's words, or
 * %AXIS_DOWNLOAD_ERROR_AUTH when the words are about the API key.
 */
static gboolean
send_body(
	AxisDownloadConn  *conn,
	GBytes            *body,
	const gchar       *content_type,
	AxisJsonDoc      **doc,
	GError           **error
){
	g_autoptr(GBytes) answer = NULL;
	g_autofree gchar *words = NULL;
	g_autofree gchar *clean = NULL;
	g_autofree gchar *text = NULL;
	gsize length = 0;
	const gchar *data;

	*doc = NULL;
	if (!axis_download_conn_post(conn, API_PATH, content_type, body, NULL, &answer, error)) {
		if (axis_download_conn_get_last_status(conn) == 403 &&
		    strstr(axis_download_conn_get_last_refusal(conn), HOSTNAME_CHECK_WORDS) != NULL) {
			/* Not the key: SABnzbd answers every request 403 while the name
			 * Axis reaches it by is not in its host_whitelist. */
			g_clear_error(error);
			return hostname_refused(conn, error);
		}
		return FALSE;
	}
	data = g_bytes_get_data(answer, &length);
	switch (axis_sabnzbd_reply_parse(data, length, doc, &words)) {
	case AXIS_SABNZBD_REPLY_OK:
		return TRUE;
	case AXIS_SABNZBD_REPLY_BAD_KEY:
		clean = words != NULL ? scrubbed(conn, words) : NULL;
		text = clean != NULL ? g_strdup_printf("SABnzbd refused the API key (it says: %s)", clean) :
			g_strdup("SABnzbd refused the API key");
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, text);
	case AXIS_SABNZBD_REPLY_REFUSED:
		clean = words != NULL ? scrubbed(conn, words) : NULL;
		text = clean != NULL ? g_strdup_printf("SABnzbd refused it: %s", clean) : g_strdup("SABnzbd refused it");
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	default:
		text = g_strdup_printf("SABnzbd's answer cannot be read: %s", words != NULL ? words : "it is not JSON");
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, text);
	}
}

/*
 * One call: a POST of the form "mode=..&output=json&apikey=.." and the
 * further name and value pairs in @pairs (ended by %NULL), to "/api".
 * Nothing but the path is in the address.
 */
static gboolean
call(
	AxisDownloadConn   *conn,
	const gchar        *mode,
	const gchar *const *pairs,
	AxisJsonDoc       **doc,
	GError            **error
){
	const gchar *key = key_of(conn, error);
	GString *form;
	g_autoptr(GBytes) body = NULL;
	gsize length;
	guint i;

	*doc = NULL;
	if (key == NULL)
		return FALSE;
	form = g_string_new(NULL);
	form_add(form, "mode", mode);
	form_add(form, "output", "json");
	for (i = 0; pairs != NULL && pairs[i] != NULL && pairs[i + 1] != NULL; i += 2)
		form_add(form, pairs[i], pairs[i + 1]);
	form_add(form, "apikey", key);
	length = form->len;
	body = bytes_wiped(g_string_free(form, FALSE), length);
	return send_body(conn, body, FORM_TYPE, doc, error);
}

/* ── Test ───────────────────────────────────────────────────────── */

static gboolean
sabnzbd_test(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	AxisDownloadTestResult      *result,
	GError                     **error
){
	g_autoptr(AxisJsonDoc) version_doc = NULL;
	g_autoptr(AxisJsonDoc) cats_doc = NULL;
	g_autofree gchar *version = NULL;
	gint exists;

	/* The version needs no key, so it only shows that something answers. */
	if (!call(conn, "version", NULL, &version_doc, error))
		return FALSE;
	version = axis_sabnzbd_parse_version(axis_json_doc_get_root(version_doc));
	if (version == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"SABnzbd's answer cannot be read: it gives no version, so this may not be SABnzbd");

	/* The categories need the key: a wrong one is answered here. */
	if (!call(conn, "get_cats", NULL, &cats_doc, error))
		return FALSE;
	exists = axis_sabnzbd_parse_categories(axis_json_doc_get_root(cats_doc), category);
	result->version = g_steal_pointer(&version);
	result->category_exists = exists;
	if (exists == 0)
		result->note = g_strdup_printf("SABnzbd has no category called \"%s\". A job sent with an unknown category "
			"lands in SABnzbd's default one, which Axis does not look at; add the category in SABnzbd's settings "
			"first.", category != NULL ? category : "");
	else if (exists < 0)
		result->note = g_strdup("SABnzbd did not list its categories, so whether this one exists is not known.");
	return TRUE;
}

/* ── Add ────────────────────────────────────────────────────────── */

static gboolean
contains_bytes(
	const guint8 *haystack,
	gsize         length,
	const gchar  *needle
){
	gsize n = strlen(needle);
	gsize i;

	if (n == 0 || length < n)
		return FALSE;
	for (i = 0; i + n <= length; i++) {
		if (haystack[i] == (guint8)needle[0] && memcmp(haystack + i, needle, n) == 0)
			return TRUE;
	}
	return FALSE;
}

/* The file name an upload carries: made here, from the release's own name
 * with everything but letters, digits, '.', '_' and '-' turned into '_',
 * never beginning with a dot, and ending in ".nzb". */
static gchar *
upload_name(const AxisDownloadRelease *release)
{
	const gchar *source = release->file_name != NULL ? release->file_name : release->name;
	GString *out = g_string_new(NULL);
	gsize n = source != NULL ? strlen(source) : 0;
	gsize i;

	if (n > 4 && g_ascii_strcasecmp(source + n - 4, ".nzb") == 0)
		n -= 4;
	for (i = 0; i < n && out->len < UPLOAD_NAME_MAX; i++) {
		guchar c = (guchar)source[i];

		if (g_ascii_isalnum((gchar)c) || c == '_' || c == '-' || (c == '.' && out->len > 0))
			g_string_append_c(out, (gchar)c);
		else if (out->len > 0 || c != '.')
			g_string_append_c(out, '_');
	}
	if (out->len == 0)
		g_string_append(out, "release");
	g_string_append(out, ".nzb");
	return g_string_free(out, FALSE);
}

typedef struct
{
	const gchar *name;
	const gchar *value;
} Field;

/*
 * A multipart/form-data body of @fields and the file in the field
 * "name" with @file_name.  The boundary is random and checked not to
 * occur in any part: a delimiter is the boundary after two dashes, and
 * since the framing around the parts holds no copy of it, nothing a part
 * says can be taken for one.
 */
static GBytes *
multipart_new(
	const Field  *fields,
	guint         n_fields,
	const gchar  *file_name,
	GBytes       *file,
	gchar       **content_type
){
	gsize file_length = 0;
	const guint8 *file_data = g_bytes_get_data(file, &file_length);
	g_autofree gchar *boundary = NULL;
	GString *body;
	gsize length;
	guint attempt;
	guint i;

	for (attempt = 0; attempt < 16; attempt++) {
		gboolean clash = FALSE;

		g_free(boundary);
		boundary = draw_boundary();
		clash = contains_bytes(file_data, file_length, boundary) || strstr(file_name, boundary) != NULL;
		for (i = 0; i < n_fields && !clash; i++)
			clash = strstr(fields[i].name, boundary) != NULL || strstr(fields[i].value, boundary) != NULL;
		if (!clash)
			break;
		g_clear_pointer(&boundary, g_free);
	}
	if (boundary == NULL)
		return NULL;

	body = g_string_new(NULL);
	for (i = 0; i < n_fields; i++)
		g_string_append_printf(body, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary,
			fields[i].name, fields[i].value);
	g_string_append_printf(body, "--%s\r\nContent-Disposition: form-data; name=\"name\"; filename=\"%s\"\r\n"
		"Content-Type: application/x-nzb\r\n\r\n", boundary, file_name);
	g_string_append_len(body, (const gchar *)file_data, (gssize)file_length);
	g_string_append_printf(body, "\r\n--%s--\r\n", boundary);
	*content_type = g_strdup_printf("multipart/form-data; boundary=%s", boundary);
	length = body->len;
	return bytes_wiped(g_string_free(body, FALSE), length);
}

static const gchar *
priority_text(AxisDownloadPriority priority)
{
	switch (priority) {
	case AXIS_DOWNLOAD_PRIORITY_LOW:
		return "-1";
	case AXIS_DOWNLOAD_PRIORITY_HIGH:
		return "1";
	default:
		return "0";
	}
}

static gboolean
sabnzbd_add(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const AxisDownloadRelease   *release,
	const gchar                 *category,
	AxisDownloadPriority         priority,
	gchar                      **client_ref,
	GError                     **error
){
	g_autofree gchar *file_name = NULL;
	g_autofree gchar *content_type = NULL;
	g_autofree gchar *job_name = NULL;
	g_autofree gchar *nzo_id = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *shown = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(AxisJsonDoc) doc = NULL;
	const gchar *key;
	Field fields[6];
	guint n = 0;

	/* Refused before anything is sent. */
	if (release->protocol != AXIS_DOWNLOAD_USENET || release->magnet != NULL || release->file == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID,
			"SABnzbd takes Usenet release files (.nzb), not magnet links or torrents");
	if (g_bytes_get_size(release->file) == 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "the release file is empty");
	if (!axis_download_category_valid(category))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "the category is not one SABnzbd can be given");
	key = key_of(conn, error);
	if (key == NULL)
		return FALSE;

	file_name = upload_name(release);
	job_name = axis_download_text_clean(release->name, AXIS_DOWNLOAD_NAME_MAX_CHARS);
	fields[n].name = "apikey";
	fields[n++].value = key;
	fields[n].name = "mode";
	fields[n++].value = "addfile";
	fields[n].name = "output";
	fields[n++].value = "json";
	fields[n].name = "cat";
	fields[n++].value = category;
	fields[n].name = "priority";
	fields[n++].value = priority_text(priority);
	if (job_name != NULL) {
		fields[n].name = "nzbname";
		fields[n++].value = job_name;
	}
	body = multipart_new(fields, n, file_name, release->file, &content_type);
	if (body == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_FAILED, "no boundary could be found that is not in the release file");

	if (!send_body(conn, body, content_type, &doc, error))
		return FALSE;
	if (!axis_sabnzbd_parse_added(axis_json_doc_get_root(doc), &nzo_id, &message)) {
		shown = scrubbed(conn, message);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, shown != NULL ? shown : "SABnzbd did not take the file");
	}
	remember(conn, nzo_id, AXIS_DOWNLOAD_ITEM_QUEUED, FALSE);
	*client_ref = g_steal_pointer(&nzo_id);
	return TRUE;
}

/* ── List ───────────────────────────────────────────────────────── */

/* Whether the state of a slot whose word is not known is replaced by the
 * state last known for the job, and the word logged once. */
static void
settle_unknown(
	AxisDownloadConn      *conn,
	AxisSabnzbdSlot       *slot
){
	AxisDownloadItemState state;
	gboolean history;

	log_unknown_word(conn, slot->word);
	if (recall(conn, slot->item->client_ref, &state, &history)) {
		slot->item->state = state;
		/* A finished download with nothing known of its folder is not
		 * completed. */
		if (state == AXIS_DOWNLOAD_ITEM_COMPLETED && slot->item->path == NULL)
			slot->item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
	}
	/* Otherwise the parser's own: queued in the queue, downloading at 100 %
	 * in the history. */
}

/* How many calls of %AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT jobs each a list of
 * every job (queue or history) is read in at most. */
#define ALL_PAGES 20

/* Every job of SABnzbd's queue or history, whatever its category, read a page
 * at a time; *@partial when there may be more than was read (the last page was
 * full, or SABnzbd answered a later page with jobs already read: it does not
 * page).  *@unreadable counts the slots whose identifier could not be read. */
static GPtrArray *
list_every(
	AxisDownloadConn  *conn,
	gboolean           history,
	guint             *unreadable,
	gboolean          *partial,
	GError           **error
){
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT);
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_sabnzbd_slot_free);
	guint page;

	*unreadable = 0;
	*partial = FALSE;
	for (page = 0; page < ALL_PAGES; page++) {
		g_autoptr(AxisJsonDoc) doc = NULL;
		g_autoptr(GPtrArray) slots = NULL;
		g_autofree gchar *start = g_strdup_printf("%u", page * (guint)AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT);
		const gchar *pairs[6];
		guint bad = 0;
		guint fresh = 0;
		guint i;

		pairs[0] = "start";
		pairs[1] = start;
		pairs[2] = "limit";
		pairs[3] = limit;
		pairs[4] = NULL;
		pairs[5] = NULL;
		if (!call(conn, history ? "history" : "queue", pairs, &doc, error)) {
			g_ptr_array_unref(out);
			return NULL;
		}
		slots = history ? axis_sabnzbd_parse_history_counted(axis_json_doc_get_root(doc), &bad) :
			axis_sabnzbd_parse_queue_counted(axis_json_doc_get_root(doc), &bad);
		if (slots == NULL) {
			g_ptr_array_unref(out);
			fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, history ? "SABnzbd's answer cannot be read: it has no list of "
				"finished jobs" : "SABnzbd's answer cannot be read: it has no list of queued jobs");
			return NULL;
		}
		*unreadable += bad;
		for (i = 0; i < slots->len; i++) {
			AxisSabnzbdSlot *slot = g_ptr_array_index(slots, i);

			if (g_hash_table_contains(seen, slot->item->client_ref))
				continue;
			g_hash_table_add(seen, slot->item->client_ref);
			g_ptr_array_add(out, g_steal_pointer(&slots->pdata[i]));
			fresh++;
		}
		/* A page not full is the last. */
		if (slots->len + bad < (guint)AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT)
			return out;
		if (fresh == 0) {
			*partial = TRUE;
			return out;
		}
	}
	*partial = TRUE;
	return out;
}

static gboolean
sabnzbd_list(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	GPtrArray                  **items,
	GError                     **error
){
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT);
	g_autoptr(AxisJsonDoc) queue_doc = NULL;
	g_autoptr(AxisJsonDoc) history_doc = NULL;
	g_autoptr(GPtrArray) queue = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	g_autoptr(GHashTable) in_history = NULL;
	const gchar *pairs[10];
	guint queue_unreadable = 0;
	guint history_unreadable = 0;
	guint i;

	/* Both the names SABnzbd's versions have used for the filter; the layer
	 * filters again whatever comes. */
	pairs[0] = "start";
	pairs[1] = "0";
	pairs[2] = "limit";
	pairs[3] = limit;
	pairs[4] = "cat";
	pairs[5] = category;
	pairs[6] = "category";
	pairs[7] = category;
	pairs[8] = NULL;
	pairs[9] = NULL;

	/* Every job, whatever its category: each list a page at a time. */
	if (category == NULL) {
		gboolean queue_partial = FALSE;
		gboolean history_partial = FALSE;

		queue = list_every(conn, FALSE, &queue_unreadable, &queue_partial, error);
		if (queue == NULL)
			return FALSE;
		history = list_every(conn, TRUE, &history_unreadable, &history_partial, error);
		if (history == NULL)
			return FALSE;
		if (queue_partial || history_partial || queue_unreadable > 0 || history_unreadable > 0)
			axis_download_conn_set_list_partial(conn);
		goto merge;
	}
	/* A list that is not whole shows nothing: a job missing from one half
	 * would look gone. */
	if (!call(conn, "queue", pairs, &queue_doc, error))
		return FALSE;
	queue = axis_sabnzbd_parse_queue_counted(axis_json_doc_get_root(queue_doc), &queue_unreadable);
	if (queue == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"SABnzbd's answer cannot be read: it has no list of queued jobs");
	if (!call(conn, "history", pairs, &history_doc, error))
		return FALSE;
	history = axis_sabnzbd_parse_history_counted(axis_json_doc_get_root(history_doc), &history_unreadable);
	if (history == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"SABnzbd's answer cannot be read: it has no list of finished jobs");
	/* As many as were asked for: there may be more it did not show.  And a
	 * slot whose identifier could not be read may be one of Axis's jobs. */
	if (queue->len >= AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT || history->len >= AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT ||
	    queue_unreadable > 0 || history_unreadable > 0)
		axis_download_conn_set_list_partial(conn);

merge:

	/* A job in both (it moved between the two calls) is its history entry,
	 * the later stage. */
	in_history = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < history->len; i++) {
		const AxisSabnzbdSlot *slot = g_ptr_array_index(history, i);

		g_hash_table_add(in_history, slot->item->client_ref);
	}
	slots = g_ptr_array_new();
	for (i = 0; i < queue->len; i++) {
		AxisSabnzbdSlot *slot = g_ptr_array_index(queue, i);

		if (!g_hash_table_contains(in_history, slot->item->client_ref))
			g_ptr_array_add(slots, slot);
	}
	for (i = 0; i < history->len; i++)
		g_ptr_array_add(slots, g_ptr_array_index(history, i));

	*items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	for (i = 0; i < slots->len; i++) {
		AxisSabnzbdSlot *slot = g_ptr_array_index(slots, i);

		/* Only what SABnzbd said in words known is remembered: a placeholder
		 * stands in for a word that was not, and is not a state seen.  Nothing
		 * of a job that cannot be read, nor of every job listed (only Axis's
		 * are followed). */
		if (slot->item->unreadable != NULL || category == NULL)
			;
		else if (slot->known)
			remember(conn, slot->item->client_ref, slot->item->state, slot->history);
		else
			settle_unknown(conn, slot);
		g_ptr_array_add(*items, slot->item);
		slot->item = NULL;
	}
	return TRUE;
}

/* ── Remove ─────────────────────────────────────────────────────── */

/* One delete call, queue or history.  Sets *@removed when the answer says
 * the job was in that list and is gone (the queue's answer names the jobs
 * it removed; the history's does not always, so there it is only that no
 * refusal came).  On the history, del_files=1 deletes the files of a job that
 * failed only: a completed job's entry goes and its folder stays (seen from
 * SABnzbd 5.1.3), which is why the import removes that folder itself
 * afterwards (import/axis-import-folder.c). */
static gboolean
delete_from(
	AxisDownloadConn  *conn,
	const gchar       *mode,
	const gchar       *nzo_id,
	gboolean           delete_data,
	gboolean          *removed,
	GError           **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const gchar *pairs[7];

	pairs[0] = "name";
	pairs[1] = "delete";
	pairs[2] = "value";
	pairs[3] = nzo_id;
	pairs[4] = "del_files";
	pairs[5] = delete_data ? "1" : "0";
	pairs[6] = NULL;
	*removed = FALSE;
	if (!call(conn, mode, pairs, &doc, error))
		return FALSE;
	*removed = strcmp(mode, "queue") == 0 ? axis_sabnzbd_parse_removed(axis_json_doc_get_root(doc), nzo_id) : TRUE;
	return TRUE;
}

static gboolean
sabnzbd_remove(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	gboolean                     delete_data,
	GError                     **error
){
	AxisDownloadItemState state;
	gboolean in_history = FALSE;
	gboolean known;
	gboolean removed = FALSE;

	if (!axis_sabnzbd_ref_valid(client_ref))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a job identifier SABnzbd gives");
	known = recall(conn, client_ref, &state, &in_history);

	if (!known || !in_history) {
		g_autoptr(GError) refused = NULL;

		if (delete_from(conn, "queue", client_ref, delete_data, &removed, &refused)) {
			if (removed) {
				forget_job(conn, client_ref);
				return TRUE;
			}
		} else if (!g_error_matches(refused, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED)) {
			g_propagate_error(error, g_steal_pointer(&refused));
			return FALSE;
		}
		/* Not in the queue (it may have finished meanwhile): the history. */
	}
	if (!delete_from(conn, "history", client_ref, delete_data, &removed, error))
		return FALSE;
	forget_job(conn, client_ref);
	return TRUE;
}

/* ── The category's folder ──────────────────────────────────────── */

/*
 * Where SABnzbd puts the category's finished jobs.  SABnzbd deletes a
 * completed job's files with del_files=1 only when the job failed; a
 * completed job's folder stays where it finished, inside this folder, for
 * Axis to remove after its import.
 *
 * mode=get_config&section=categories (its list of objects with "name" and
 * "dir" was seen from SABnzbd 5.1.3), then
 * mode=get_config&section=misc&keyword=complete_dir (asked for by its
 * keyword alone, so that the rest of SABnzbd's settings, its keys among
 * them, is not sent): a relative category folder is beneath it, and an
 * absolute one that is it is no folder of the category's own.  Without it a
 * relative folder cannot be told; an absolute one is taken as it is.
 */
static gboolean
sabnzbd_category_folder(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	gchar                      **folder,
	GError                     **error
){
	static const gchar *const categories[] = { "section", "categories", NULL };
	static const gchar *const misc[] = { "section", "misc", "keyword", "complete_dir", NULL };
	g_autoptr(AxisJsonDoc) cats_doc = NULL;
	g_autoptr(AxisJsonDoc) misc_doc = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *complete = NULL;
	g_autofree gchar *text = NULL;
	const gchar *problem = NULL;
	gint found;

	*folder = NULL;
	if (!call(conn, "get_config", categories, &cats_doc, error))
		return FALSE;
	found = axis_sabnzbd_parse_category_dir(axis_json_doc_get_root(cats_doc), category, &dir);
	if (found < 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"SABnzbd's answer cannot be read: it gives no list of categories with their folders");
	if (found == 0) {
		text = g_strdup_printf("SABnzbd has no category called \"%s\"", category != NULL ? category : "");
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	{
		g_autofree gchar *setting = g_strstrip(g_strdup(dir));

		if (setting[0] != '\0' && setting[0] != '*') {
			if (!call(conn, "get_config", misc, &misc_doc, error))
				return FALSE;
			complete = axis_sabnzbd_parse_complete_dir(axis_json_doc_get_root(misc_doc));
			if (complete == NULL && setting[0] != '/')
				return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
					"SABnzbd's answer cannot be read: it does not say where its finished downloads go");
		}
	}
	if (!axis_sabnzbd_category_folder(complete, dir, folder, &problem))
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, problem);
	/* No other category's folder is it or lies inside it (as the torrent
	 * kinds ask of their labels and categories): only then is it Axis's
	 * alone. */
	{
		g_autoptr(GPtrArray) names = NULL;
		g_autoptr(GPtrArray) dirs = NULL;
		guint i;

		if (!axis_sabnzbd_parse_other_dirs(axis_json_doc_get_root(cats_doc), category, &names, &dirs)) {
			g_clear_pointer(folder, g_free);
			return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
				"SABnzbd's answer cannot be read: it gives no list of categories with their folders");
		}
		for (i = 0; i < names->len; i++) {
			g_autofree gchar *theirs = NULL;
			const gchar *name = g_ptr_array_index(names, i);
			gint shared = axis_sabnzbd_folder_shared(complete, *folder, g_ptr_array_index(dirs, i), &theirs);

			if (shared == 0)
				continue;
			g_free(text);
			text = shared > 0 ?
				g_strdup_printf("SABnzbd's category \"%s\" puts its finished jobs in %s, which is the folder of Axis's "
					"category or a folder inside it, so that folder is not Axis's category's alone", name, theirs) :
				g_strdup_printf("SABnzbd's category \"%s\" has a folder setting Axis cannot work out, so the folder of "
					"Axis's category cannot be told to be its alone", name);
			g_clear_pointer(folder, g_free);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
	}
	return TRUE;
}

/* ── The kind ───────────────────────────────────────────────────── */

static const AxisDownloadKindInfo sabnzbd_kind = {
	AXIS_DOWNLOAD_SABNZBD_NAME, "SABnzbd", AXIS_DOWNLOAD_USENET, "API key", "Category",
	"SABnzbd's own address, such as http://sabnzbd:8080. If it is served under a path, put that in the base path.",
	sabnzbd_test, sabnzbd_add, sabnzbd_list, sabnzbd_remove, NULL, NULL, sabnzbd_category_folder, NULL, FALSE
};

const AxisDownloadKindInfo *
axis_download_sabnzbd_kind(void)
{
	return &sabnzbd_kind;
}
