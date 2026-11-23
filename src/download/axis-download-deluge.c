/*
 * axis-download-deluge.c - the Deluge download client kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-deluge.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "core/axis-http.h"
#include "download/axis-download-deluge-parse.h"
#include "storage/axis-storage-path.h"

#define RPC_PATH "/json"
#define RPC_TYPE "application/json"

/* At most this many torrents are remembered; more clears the memory (it only
 * makes the kind quieter, never righter). */
#define MEMORY_MAX 4096

/* At most this many state words are logged. */
#define WORDS_MAX 64

/* The longest file name made for an upload, before ".torrent". */
#define UPLOAD_NAME_MAX 100

/* ── What is remembered ─────────────────────────────────────────── */

G_LOCK_DEFINE_STATIC(memory);
static GHashTable *remembered;	/* (memory) "client\nhash" -> 1 + state */
static GHashTable *logged_words;	/* (memory) gchar * */

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
	AxisDownloadItemState  state
){
	gchar *key = memory_key(conn, ref);

	G_LOCK(memory);
	if (remembered == NULL)
		remembered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (g_hash_table_size(remembered) >= MEMORY_MAX && !g_hash_table_contains(remembered, key))
		g_hash_table_remove_all(remembered);
	g_hash_table_insert(remembered, key, GUINT_TO_POINTER(1 + (guint)state));
	G_UNLOCK(memory);
}

static gboolean
recall(
	AxisDownloadConn      *conn,
	const gchar           *ref,
	AxisDownloadItemState *state
){
	g_autofree gchar *key = memory_key(conn, ref);
	gpointer value = NULL;
	gboolean found = FALSE;

	G_LOCK(memory);
	if (remembered != NULL && g_hash_table_lookup_extended(remembered, key, NULL, &value)) {
		*state = (AxisDownloadItemState)(GPOINTER_TO_UINT(value) - 1);
		found = TRUE;
	}
	G_UNLOCK(memory);
	return found;
}

static void
forget_torrent(
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
axis_download_deluge_forget(void)
{
	G_LOCK(memory);
	g_clear_pointer(&remembered, g_hash_table_unref);
	g_clear_pointer(&logged_words, g_hash_table_unref);
	G_UNLOCK(memory);
}

/* Logs a state word the kind does not know, once per word. */
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
		g_message("downloads: Deluge client %s reported a torrent state Axis does not know (\"%s\"); torrents keep "
			"their last known state", axis_download_conn_get_label(conn), shown);
}

/* ── Requests ───────────────────────────────────────────────────── */

/* Bytes that are wiped when they are let go: a request body holds the web
 * password (the login's). */
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

/* @text with the password blanked out and cleaned, for a message. */
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

static gint64
next_id(void)
{
	static volatile gint counter = 0;

	return (gint64)g_atomic_int_add(&counter, 1) + 1;
}

/* The request as a JSON-RPC body: @params (not consumed) as its list. */
static GBytes *
request_body(
	const gchar *method,
	JsonArray   *params,
	gint64       id
){
	g_autoptr(JsonGenerator) generator = json_generator_new();
	JsonObject *object = json_object_new();
	JsonNode *root = json_node_new(JSON_NODE_OBJECT);
	gsize length = 0;
	gchar *text;

	json_object_set_string_member(object, "method", method);
	json_object_set_array_member(object, "params", json_array_ref(params));
	json_object_set_int_member(object, "id", id);
	json_node_take_object(root, object);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, &length);
	json_node_unref(root);
	return bytes_wiped(text, length);
}

/*
 * One POST of a call and the reading of its envelope, nothing more: no login,
 * no retry.  Returns %TRUE when an envelope was read, with its kind (and, for
 * an error, Deluge's words, cleaned); %FALSE with @error for anything that
 * stopped it (transport, an answer that cannot be read).
 */
static gboolean
send_call(
	AxisDownloadConn     *conn,
	const gchar          *method,
	JsonArray            *params,
	AxisJsonDoc         **doc,
	const AxisJsonNode  **result,
	AxisDelugeReplyKind  *kind,
	gchar               **words,
	GError              **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) answer = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *text = NULL;
	gint64 id = next_id();
	gsize length = 0;
	const gchar *data;

	*doc = NULL;
	*result = NULL;
	*words = NULL;
	body = request_body(method, params, id);
	if (!axis_download_conn_post(conn, RPC_PATH, RPC_TYPE, body, NULL, &answer, error))
		return FALSE;
	data = g_bytes_get_data(answer, &length);
	*kind = axis_deluge_reply_parse(data, length, id, doc, result, &why);
	if (*kind == AXIS_DELUGE_REPLY_UNREADABLE) {
		text = g_strdup_printf("Deluge's answer cannot be read: %s", why != NULL ? why : "it is not JSON");
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, text);
	}
	if (*kind != AXIS_DELUGE_REPLY_OK)
		*words = why != NULL ? scrubbed(conn, why) : NULL;
	return TRUE;
}

/* Logs in: auth.login with the password, once, on a clean slate.  A refused
 * password is %AXIS_DOWNLOAD_ERROR_AUTH. */
static gboolean
login(
	AxisDownloadConn  *conn,
	GError           **error
){
	g_autoptr(JsonArray) params = json_array_new();
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autofree gchar *words = NULL;
	g_autofree gchar *text = NULL;
	const AxisJsonNode *result = NULL;
	AxisDelugeReplyKind kind;
	const gchar *password = axis_download_conn_get_secret(conn);
	gboolean accepted = FALSE;

	if (password == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "no web password is set");
	axis_download_conn_forget_session(conn);
	json_array_add_string_element(params, password);
	if (!send_call(conn, "auth.login", params, &doc, &result, &kind, &words, error))
		return FALSE;
	if (kind != AXIS_DELUGE_REPLY_OK) {
		text = words != NULL ? g_strdup_printf("Deluge refused the login (it says: %s)", words) :
			g_strdup("Deluge refused the login");
		return fail(error, kind == AXIS_DELUGE_REPLY_NOT_AUTH ? AXIS_DOWNLOAD_ERROR_AUTH : AXIS_DOWNLOAD_ERROR_REFUSED,
			text);
	}
	if (!axis_deluge_parse_boolean(result, &accepted))
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: the login's result is not true or false");
	if (!accepted)
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "Deluge refused the web password");
	if (!axis_download_conn_has_session(conn))
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge accepted the web password but set no session, so this may not be Deluge's web interface");
	return TRUE;
}

typedef enum
{
	RPC_NEVER_RETRY,	/* a call that changes something: a lost session is reported, not repaired */
	RPC_RETRY_ONCE		/* a call that changes nothing (or removes what this call made): login again, once */
} RpcMode;

/*
 * One call: logs in first when there is no session, and sends it.  An error
 * of Deluge's is %AXIS_DOWNLOAD_ERROR_REFUSED with its words.  A call answered
 * "not authenticated" did nothing; with @mode RPC_RETRY_ONCE the session is
 * made again and the call sent once more, with RPC_NEVER_RETRY it is
 * %AXIS_DOWNLOAD_ERROR_REFUSED and the next call logs in.  Takes @params
 * (%NULL for none).  On success *@result belongs to *@doc.  @refusal
 * (optional): for an error of Deluge's, its words (cleaned and cut), for the
 * caller to read what it says.
 */
static gboolean
rpc_words(
	AxisDownloadConn     *conn,
	const gchar          *method,
	JsonArray            *params,
	RpcMode               mode,
	AxisJsonDoc         **doc,
	const AxisJsonNode  **result,
	gchar               **refusal,
	GError              **error
){
	g_autoptr(JsonArray) list = params != NULL ? params : json_array_new();
	guint attempt;

	*doc = NULL;
	*result = NULL;
	if (refusal != NULL)
		*refusal = NULL;
	if (!axis_download_conn_has_session(conn) && !login(conn, error))
		return FALSE;
	for (attempt = 0; attempt < 2; attempt++) {
		g_autofree gchar *words = NULL;
		g_autofree gchar *text = NULL;
		AxisDelugeReplyKind kind = AXIS_DELUGE_REPLY_UNREADABLE;

		if (!send_call(conn, method, list, doc, result, &kind, &words, error))
			return FALSE;
		switch (kind) {
		case AXIS_DELUGE_REPLY_OK:
			return TRUE;
		case AXIS_DELUGE_REPLY_NOT_AUTH:
			axis_download_conn_forget_session(conn);
			if (mode == RPC_NEVER_RETRY)
				return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
					"Deluge no longer accepts the session, so nothing was done; it will not be repeated by itself");
			if (attempt > 0)
				return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "Deluge refused the session again right after a login");
			if (!login(conn, error))
				return FALSE;
			break;
		default:
			text = words != NULL ? g_strdup_printf("Deluge refused it: %s", words) : g_strdup("Deluge refused it");
			if (refusal != NULL)
				*refusal = g_steal_pointer(&words);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
	}
	return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "Deluge refused the session again right after a login");
}

static gboolean
rpc(
	AxisDownloadConn     *conn,
	const gchar          *method,
	JsonArray            *params,
	RpcMode               mode,
	AxisJsonDoc         **doc,
	const AxisJsonNode  **result,
	GError              **error
){
	return rpc_words(conn, method, params, mode, doc, result, NULL, error);
}

/* Makes sure the connection has a session Deluge still accepts, before a call
 * that must not be repeated: no session logs in, a session is asked
 * auth.check_session (which changes nothing). */
static gboolean
session_ready(
	AxisDownloadConn  *conn,
	GError           **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *result = NULL;
	gboolean valid = FALSE;

	if (axis_download_conn_get_secret(conn) == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "no web password is set");
	if (!axis_download_conn_has_session(conn))
		return login(conn, error);
	if (!rpc(conn, "auth.check_session", NULL, RPC_RETRY_ONCE, &doc, &result, error))
		return FALSE;
	if (axis_deluge_parse_boolean(result, &valid) && valid && axis_download_conn_has_session(conn))
		return TRUE;
	return login(conn, error);
}

/* ── The label ──────────────────────────────────────────────────── */

static gboolean
label_ok(
	const gchar  *label,
	GError      **error
){
	if (axis_deluge_label_valid(label))
		return TRUE;
	return fail(error, AXIS_DOWNLOAD_ERROR_INVALID,
		"Deluge's labels are lower case: use only lower-case letters, digits, '.', '_' and '-' for the label");
}

/*
 * The Label plugin must be enabled and @label must exist, made with
 * label.add when it does not.  Nothing has been added to Deluge by then, so a
 * failure here leaves nothing behind.  *@created: the label was made.
 */
static gboolean
ensure_label(
	AxisDownloadConn  *conn,
	const gchar       *label,
	gboolean          *created,
	GError           **error
){
	g_autoptr(AxisJsonDoc) plugins_doc = NULL;
	g_autoptr(AxisJsonDoc) labels_doc = NULL;
	g_autoptr(AxisJsonDoc) added_doc = NULL;
	const AxisJsonNode *result = NULL;
	gint listed;

	*created = FALSE;
	if (!rpc(conn, "core.get_enabled_plugins", NULL, RPC_RETRY_ONCE, &plugins_doc, &result, error))
		return FALSE;
	listed = axis_deluge_parse_has_name(result, "Label");
	if (listed < 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: it has no list of enabled plugins");
	if (listed == 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
			"The Label plugin is not enabled in Deluge (Preferences, Plugins). Axis tells its torrents apart by a label");
	if (!rpc(conn, "label.get_labels", NULL, RPC_RETRY_ONCE, &labels_doc, &result, error))
		return FALSE;
	listed = axis_deluge_parse_has_label(result, label);
	if (listed < 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "Deluge's answer cannot be read: it has no list of labels");
	if (listed > 0)
		return TRUE;
	{
		JsonArray *params = json_array_new();

		json_array_add_string_element(params, label);
		if (!rpc(conn, "label.add", params, RPC_NEVER_RETRY, &added_doc, &result, error))
			return FALSE;
	}
	*created = TRUE;
	return TRUE;
}

/* ── Test ───────────────────────────────────────────────────────── */

static gboolean
connected(
	AxisDownloadConn  *conn,
	gboolean          *value,
	GError           **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *result = NULL;

	*value = FALSE;
	if (!rpc(conn, "web.connected", NULL, RPC_RETRY_ONCE, &doc, &result, error))
		return FALSE;
	if (!axis_deluge_parse_boolean(result, value))
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: web.connected is not true or false");
	return TRUE;
}

/* The daemon's version, or %NULL (it is only information). */
static gchar *
daemon_version(AxisDownloadConn *conn)
{
	static const gchar *const methods[] = { "daemon.get_version", "daemon.info", NULL };
	guint i;

	for (i = 0; methods[i] != NULL; i++) {
		g_autoptr(AxisJsonDoc) doc = NULL;
		g_autoptr(GError) ignored = NULL;
		const AxisJsonNode *result = NULL;
		gchar *version;

		if (!rpc(conn, methods[i], NULL, RPC_RETRY_ONCE, &doc, &result, &ignored))
			continue;
		version = axis_deluge_parse_version(result);
		if (version != NULL)
			return version;
	}
	return NULL;
}

static gboolean
deluge_test(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	AxisDownloadTestResult      *result,
	GError                     **error
){
	g_autoptr(GString) note = g_string_new(NULL);
	g_autofree gchar *version = NULL;
	gboolean is_connected = FALSE;
	gboolean created = FALSE;

	if (!label_ok(category, error))
		return FALSE;
	/* A fresh login: the password is the one thing that is being tested. */
	if (!login(conn, error))
		return FALSE;
	if (!connected(conn, &is_connected, error))
		return FALSE;
	if (!is_connected) {
		g_autoptr(AxisJsonDoc) hosts_doc = NULL;
		g_autoptr(AxisJsonDoc) connect_doc = NULL;
		g_autoptr(JsonArray) params = json_array_new();
		g_autofree gchar *host_id = NULL;
		g_autofree gchar *host = NULL;
		g_autofree gchar *where = NULL;
		const AxisJsonNode *hosts = NULL;
		const AxisJsonNode *ignored = NULL;
		gint port = 0;

		if (!rpc(conn, "web.get_hosts", NULL, RPC_RETRY_ONCE, &hosts_doc, &hosts, error))
			return FALSE;
		if (!axis_deluge_parse_host(hosts, &host_id, &host, &port))
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
				"Deluge's web interface is not connected to a daemon, and lists none to connect to (add one in its "
				"Connection Manager)");
		where = port > 0 ? g_strdup_printf("%s:%d", host != NULL ? host : "a daemon", port) :
			g_strdup(host != NULL ? host : "a daemon");
		json_array_add_string_element(params, host_id);
		if (!rpc(conn, "web.connect", g_steal_pointer(&params), RPC_NEVER_RETRY, &connect_doc, &ignored, error))
			return FALSE;
		if (!connected(conn, &is_connected, error))
			return FALSE;
		if (!is_connected) {
			g_autofree gchar *text = g_strdup_printf("Deluge's web interface is not connected to a daemon, and "
				"connecting it to %s did not work", where);

			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
		g_string_append_printf(note, "Deluge's web interface was not connected to a daemon; it is now, to %s.", where);
	}

	version = daemon_version(conn);
	if (!ensure_label(conn, category, &created, error))
		return FALSE;
	if (created) {
		if (note->len > 0)
			g_string_append_c(note, ' ');
		g_string_append_printf(note, "The label \"%s\" did not exist in Deluge; it was made.", category);
	}
	if (note->len > 0)
		g_string_append_c(note, ' ');
	g_string_append(note, "Deluge ignores the priority set here.");
	result->version = g_steal_pointer(&version);
	result->category_exists = 1;
	result->note = g_strdup(note->str);
	return TRUE;
}

/* ── Add ────────────────────────────────────────────────────────── */

/* The file name an upload carries (Deluge only shows it): made here, from the
 * release's own name with everything but letters, digits, '.', '_' and '-'
 * turned into '_', never beginning with a dot, and ending in ".torrent". */
static gchar *
upload_name(const AxisDownloadRelease *release)
{
	const gchar *source = release->file_name != NULL ? release->file_name : release->name;
	GString *out = g_string_new(NULL);
	gsize n = source != NULL ? strlen(source) : 0;
	gsize i;

	if (n > 8 && g_ascii_strcasecmp(source + n - 8, ".torrent") == 0)
		n -= 8;
	for (i = 0; i < n && out->len < UPLOAD_NAME_MAX; i++) {
		guchar c = (guchar)source[i];

		if (g_ascii_isalnum((gchar)c) || c == '_' || c == '-' || (c == '.' && out->len > 0))
			g_string_append_c(out, (gchar)c);
		else if (out->len > 0 || c != '.')
			g_string_append_c(out, '_');
	}
	if (out->len == 0)
		g_string_append(out, "release");
	g_string_append(out, ".torrent");
	return g_string_free(out, FALSE);
}

/* Removes the torrent this add made, without its data, after the label could
 * not be set.  A session lost meanwhile is made again (removing twice is
 * harmless).  Returns whether it is gone. */
static gboolean
remove_stray(
	AxisDownloadConn *conn,
	const gchar      *hash
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autoptr(GError) ignored = NULL;
	JsonArray *params = json_array_new();
	const AxisJsonNode *result = NULL;
	gboolean removed = FALSE;

	json_array_add_string_element(params, hash);
	json_array_add_boolean_element(params, FALSE);
	if (!rpc(conn, "core.remove_torrent", params, RPC_RETRY_ONCE, &doc, &result, &ignored))
		return FALSE;
	return axis_deluge_parse_boolean(result, &removed) && removed;
}

static gboolean
deluge_add(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const AxisDownloadRelease   *release,
	const gchar                 *category,
	AxisDownloadPriority         priority,
	gchar                      **client_ref,
	GError                     **error
){
	g_autoptr(AxisJsonDoc) add_doc = NULL;
	g_autoptr(AxisJsonDoc) label_doc = NULL;
	g_autoptr(GError) label_error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *shown = NULL;
	const AxisJsonNode *result = NULL;
	JsonArray *params;
	JsonObject *options;
	gboolean created = FALSE;

	/* Refused before anything is sent. */
	if (release->protocol != AXIS_DOWNLOAD_TORRENT || (release->magnet == NULL) == (release->file == NULL))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID,
			"Deluge takes torrent files and magnet links, not Usenet releases");
	if (release->file != NULL && g_bytes_get_size(release->file) == 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "the release file is empty");
	if (!axis_download_hash_valid(release->info_hash))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "the release has no info-hash Axis can check Deluge's answer against");
	if (!label_ok(category, error))
		return FALSE;

	/* A session Deluge still accepts, and the label made sure of, before the
	 * torrent exists: whatever fails here leaves nothing behind. */
	if (!session_ready(conn, error) || !ensure_label(conn, category, &created, error))
		return FALSE;

	/* The add itself: never repeated, whatever happens. */
	params = json_array_new();
	options = json_object_new();
	if (release->file != NULL) {
		g_autofree gchar *file_name = upload_name(release);
		g_autofree gchar *dump = g_base64_encode(g_bytes_get_data(release->file, NULL), g_bytes_get_size(release->file));

		json_array_add_string_element(params, file_name);
		json_array_add_string_element(params, dump);
		json_array_add_object_element(params, options);
		if (!rpc(conn, "core.add_torrent_file", params, RPC_NEVER_RETRY, &add_doc, &result, error))
			return FALSE;
	} else {
		json_array_add_string_element(params, release->magnet);
		json_array_add_object_element(params, options);
		if (!rpc(conn, "core.add_torrent_magnet", params, RPC_NEVER_RETRY, &add_doc, &result, error))
			return FALSE;
	}
	hash = axis_deluge_parse_hash(result);
	if (hash == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, "Deluge did not say that it took the torrent: it gave no info-hash");
	if (g_ascii_strcasecmp(hash, release->info_hash) != 0) {
		/* Not the torrent that was sent: the stray one is not touched. */
		shown = g_strdup_printf("Deluge answered an info-hash (%s) that is not the one Axis computed (%s), so the add is "
			"treated as failed; the torrent in Deluge is not touched", hash, release->info_hash);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, shown);
	}

	/* The label.  Without it Axis cannot see the torrent, so a torrent that
	 * cannot be labelled is taken out again. */
	params = json_array_new();
	json_array_add_string_element(params, hash);
	json_array_add_string_element(params, category);
	if (!rpc(conn, "label.set_torrent", params, RPC_NEVER_RETRY, &label_doc, &result, &label_error)) {
		gboolean removed = remove_stray(conn, hash);

		text = g_strdup_printf("Deluge took the torrent but would not give it the label \"%s\" (%s); %s", category,
			label_error->message, removed ? "it was removed again" :
			"it could not be removed again either, so remove it in Deluge by hand");
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	remember(conn, hash, AXIS_DOWNLOAD_ITEM_QUEUED);
	*client_ref = g_steal_pointer(&hash);
	return TRUE;
}

/* ── List ───────────────────────────────────────────────────────── */

/* Replaces the state of a torrent whose word is not known by the state last
 * known for it, and logs the word once. */
static void
settle_unknown(
	AxisDownloadConn *conn,
	AxisDelugeSlot   *slot
){
	AxisDownloadItemState state;

	log_unknown_word(conn, slot->word);
	if (recall(conn, slot->item->client_ref, &state)) {
		slot->item->state = state;
		/* A finished download with nothing known of its folder is not
		 * completed (and one that is finished is never unknown). */
		if (state == AXIS_DOWNLOAD_ITEM_COMPLETED)
			slot->item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
	}
}

static gboolean
deluge_list(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	GPtrArray                  **items,
	GError                     **error
){
	static const gchar *const keys[] = {
		"name", "state", "progress", "total_wanted", "total_done", "download_payload_rate", "eta", "download_location",
		"is_finished", "label", "tracker_status", "time_added", NULL
	};
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	const AxisJsonNode *result = NULL;
	JsonArray *params;
	JsonObject *filter;
	JsonArray *wanted;
	guint i;

	if (category != NULL && !label_ok(category, error))
		return FALSE;
	/* Only Deluge's torrents with Axis's label; the layer filters again
	 * whatever comes.  Without one, every torrent it has. */
	params = json_array_new();
	filter = json_object_new();
	if (category != NULL)
		json_object_set_string_member(filter, "label", category);
	json_array_add_object_element(params, filter);
	wanted = json_array_new();
	for (i = 0; keys[i] != NULL; i++)
		json_array_add_string_element(wanted, keys[i]);
	json_array_add_array_element(params, wanted);
	if (!rpc(conn, "core.get_torrents_status", params, RPC_RETRY_ONCE, &doc, &result, error))
		return FALSE;
	{
		guint unreadable = 0;
		gboolean cut = FALSE;

		slots = axis_deluge_parse_torrents_counted(result, &unreadable, &cut);
		/* What could not be read, or was not looked at, is not taken for
		 * absent where that matters (axis_download_conn_set_list_partial()). */
		if (slots != NULL && (unreadable > 0 || cut))
			axis_download_conn_set_list_partial(conn);
	}
	if (slots == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "Deluge's answer cannot be read: it has no list of torrents");

	*items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	for (i = 0; i < slots->len; i++) {
		AxisDelugeSlot *slot = g_ptr_array_index(slots, i);

		/* Only what Deluge said in words known is remembered: a placeholder
		 * stands in for a word that was not, and is not a state seen.  Nothing
		 * of an entry that cannot be read, nor of every torrent listed (only
		 * Axis's are followed). */
		if (slot->item->unreadable != NULL || category == NULL)
			;
		else if (slot->known)
			remember(conn, slot->item->client_ref, slot->item->state);
		else
			settle_unknown(conn, slot);
		g_ptr_array_add(*items, slot->item);
		slot->item = NULL;
	}
	return TRUE;
}

/* ── Remove ─────────────────────────────────────────────────────── */

/* The torrent @hash in any label, asked of Deluge by its id, read only:
 * %TRUE with *@item (its label, "" for none) when Deluge has it, %TRUE with
 * %NULL when its answer does not hold it. */
static gboolean
deluge_find(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	AxisDownloadItem           **item,
	GError                     **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *name = NULL;
	const AxisJsonNode *result = NULL;
	JsonArray *params;
	JsonObject *filter;
	JsonArray *ids;
	JsonArray *wanted;
	gint found;

	*item = NULL;
	if (!axis_download_hash_valid(client_ref))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not an info-hash Deluge gives");
	if (!session_ready(conn, error))
		return FALSE;
	params = json_array_new();
	filter = json_object_new();
	ids = json_array_new();
	json_array_add_string_element(ids, client_ref);
	json_object_set_array_member(filter, "id", ids);
	json_array_add_object_element(params, filter);
	wanted = json_array_new();
	json_array_add_string_element(wanted, "name");
	json_array_add_string_element(wanted, "label");
	json_array_add_array_element(params, wanted);
	if (!rpc(conn, "core.get_torrents_status", params, RPC_RETRY_ONCE, &doc, &result, error))
		return FALSE;
	found = axis_deluge_parse_label_of(result, client_ref, &label, &name);
	if (found < 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "Deluge's answer cannot be read: it does not say the "
			"torrent's label");
	if (found == 0)
		return TRUE;
	*item = axis_download_item_new();
	(*item)->client_ref = g_ascii_strdown(client_ref, -1);
	(*item)->info_hash = g_strdup((*item)->client_ref);
	(*item)->category = g_steal_pointer(&label);
	(*item)->name = name != NULL ? g_steal_pointer(&name) : g_strdup((*item)->client_ref);
	return TRUE;
}

static gboolean
deluge_remove(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	gboolean                     delete_data,
	GError                     **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autoptr(GError) refused = NULL;
	g_autofree gchar *words = NULL;
	const AxisJsonNode *result = NULL;
	JsonArray *params;
	gboolean removed = FALSE;

	if (!axis_download_hash_valid(client_ref))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not an info-hash Deluge gives");
	if (!session_ready(conn, error))
		return FALSE;
	/* Only a torrent that still has Axis's label: one a person gave another
	 * label is theirs now, and is left alone.  (One Deluge does not have is
	 * asked to be removed all the same: its own answer says it is gone.) */
	{
		g_autoptr(AxisDownloadItem) now = NULL;

		if (!deluge_find(self, conn, client_ref, &now, error))
			return FALSE;
		if (now != NULL && g_ascii_strcasecmp(now->category, axis_download_conn_get_category(conn) != NULL ?
		        axis_download_conn_get_category(conn) : "") != 0) {
			g_autofree gchar *text = g_strdup_printf("Deluge has the torrent with the label \"%s\" now, not Axis's: a "
				"person gave it another label, so it is not Axis's to remove, and it was left alone", now->category);

			forget_torrent(conn, client_ref);
			return fail(error, AXIS_DOWNLOAD_ERROR_NOT_OURS, text);
		}
	}
	params = json_array_new();
	json_array_add_string_element(params, client_ref);
	json_array_add_boolean_element(params, delete_data);
	if (!rpc_words(conn, "core.remove_torrent", params, RPC_NEVER_RETRY, &doc, &result, &words, &refused)) {
		/* "torrent_id <hash> not in session." (seen from Deluge 2.2.0): it
		 * has no such torrent; something else removed it, and with it
		 * nothing of its files. */
		if (g_error_matches(refused, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) &&
		    axis_deluge_says_not_in_session(words, client_ref)) {
			forget_torrent(conn, client_ref);
			return fail(error, AXIS_DOWNLOAD_ERROR_GONE, "Deluge no longer has the torrent (it says it is not in "
				"its session): something else removed it already");
		}
		g_propagate_error(error, g_steal_pointer(&refused));
		return FALSE;
	}
	if (!axis_deluge_parse_boolean(result, &removed))
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: the removal's result is not true or false");
	if (!removed)
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, "Deluge did not remove the torrent");
	forget_torrent(conn, client_ref);
	return TRUE;
}

/* ── The label's folder ─────────────────────────────────────────── */

/* At most this many other labels are looked at; with more, the label's
 * folder is not taken as its own. */
#define OTHER_LABELS_MAX 100

/* @path as a folder: normalised and absolute, or %NULL. */
static gchar *
folder_of(const gchar *path)
{
	gchar *normal = NULL;

	if (path == NULL || !axis_storage_path_normalize(path, TRUE, &normal, NULL))
		return NULL;
	return normal;
}

/* What the Label plugin says of @label: 1 with *@folder (normalised) when it
 * moves finished torrents to a folder of its own, 0 when it does not; FALSE
 * with @error when it cannot be asked or read. */
static gboolean
label_folder(
	AxisDownloadConn  *conn,
	const gchar       *label,
	gint              *found,
	gchar            **folder,
	GError           **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autofree gchar *raw = NULL;
	const AxisJsonNode *result = NULL;
	JsonArray *params = json_array_new();

	*found = 0;
	*folder = NULL;
	json_array_add_string_element(params, label);
	if (!rpc(conn, "label.get_options", params, RPC_RETRY_ONCE, &doc, &result, error))
		return FALSE;
	*found = axis_deluge_parse_label_folder(result, &raw);
	if (*found < 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: the label's options are not an object");
	if (*found > 0) {
		*folder = folder_of(raw);
		if (*folder == NULL)
			*found = -1;
	}
	return TRUE;
}

/*
 * Where Deluge puts the finished torrents of Axis's label: the Label
 * plugin's "Move completed to" folder of the label (label.get_options:
 * "apply_move_completed" and "move_completed" on, "move_completed_path"),
 * which Deluge moves a labelled torrent's files into when it finishes.  It
 * is the label's own only when it is a full path, and neither Deluge's own
 * folders for every download (core.get_config_values, asked for those two
 * keys alone: "download_location", "move_completed_path") nor another
 * label's folder is it or lies inside it.  Read only.  Asked only for a
 * torrent Deluge no longer has after its import, whose leftover files Axis
 * removes when they are inside this folder.
 */
static gboolean
deluge_category_folder(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	gchar                      **folder,
	GError                     **error
){
	g_autoptr(AxisJsonDoc) plugins_doc = NULL;
	g_autoptr(AxisJsonDoc) config_doc = NULL;
	g_autoptr(AxisJsonDoc) labels_doc = NULL;
	g_autoptr(GPtrArray) labels = NULL;
	g_autoptr(GError) refused = NULL;
	g_autofree gchar *own = NULL;
	g_autofree gchar *text = NULL;
	const AxisJsonNode *result = NULL;
	static const gchar *const keys[] = { "download_location", "move_completed_path", NULL };
	JsonArray *params;
	JsonArray *wanted;
	gint found = 0;
	gint listed;
	guint i;

	*folder = NULL;
	if (!label_ok(category, error))
		return FALSE;
	if (!rpc(conn, "core.get_enabled_plugins", NULL, RPC_RETRY_ONCE, &plugins_doc, &result, error))
		return FALSE;
	listed = axis_deluge_parse_has_name(result, "Label");
	if (listed < 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: it has no list of enabled plugins");
	if (listed == 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
			"the Label plugin is not enabled in Deluge, so no folder is the label's own");
	if (!label_folder(conn, category, &found, &own, &refused)) {
		if (g_error_matches(refused, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED)) {
			text = g_strdup_printf("Deluge did not give the options of the label \"%s\" (%s)", category,
				refused->message);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
		g_propagate_error(error, g_steal_pointer(&refused));
		return FALSE;
	}
	if (found < 0) {
		text = g_strdup_printf("the label \"%s\" moves finished torrents to a folder that is not a full path", category);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	if (found == 0) {
		text = g_strdup_printf("the label \"%s\" has no folder of its own in Deluge: its \"Move completed to\" is not "
			"set in the Label plugin's options", category);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}

	/* Not Deluge's own folder for every download, nor one around it. */
	params = json_array_new();
	wanted = json_array_new();
	for (i = 0; keys[i] != NULL; i++)
		json_array_add_string_element(wanted, keys[i]);
	json_array_add_array_element(params, wanted);
	if (!rpc(conn, "core.get_config_values", params, RPC_RETRY_ONCE, &config_doc, &result, error))
		return FALSE;
	if (axis_json_type(result) != AXIS_JSON_OBJECT)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"Deluge's answer cannot be read: its settings are not an object");
	for (i = 0; keys[i] != NULL; i++) {
		g_autofree gchar *value = axis_deluge_parse_config_text(result, keys[i]);
		g_autofree gchar *general = folder_of(value);

		if (general != NULL && axis_storage_path_contains(own, general)) {
			text = g_strdup_printf("the label \"%s\" moves finished torrents to %s, which is (or holds) Deluge's own "
				"folder for every download, not a folder of the label's own", category, own);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
	}

	/* No other label's folder is it or lies inside it. */
	if (!rpc(conn, "label.get_labels", NULL, RPC_RETRY_ONCE, &labels_doc, &result, error))
		return FALSE;
	labels = axis_deluge_parse_labels(result);
	if (labels == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "Deluge's answer cannot be read: it has no list of labels");
	if (labels->len > OTHER_LABELS_MAX)
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
			"Deluge has too many labels for Axis to see that no other one shares the label's folder");
	for (i = 0; i < labels->len; i++) {
		const gchar *other = g_ptr_array_index(labels, i);
		g_autofree gchar *theirs = NULL;
		gint has = 0;

		if (strcmp(other, category) == 0)
			continue;
		if (!label_folder(conn, other, &has, &theirs, error))
			return FALSE;
		if (has < 0) {
			text = g_strdup_printf("the label \"%s\" moves its finished torrents to a folder that is not a full path, "
				"which cannot be told apart from the label \"%s\"'s", other, category);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
		if (has > 0 && axis_storage_path_contains(own, theirs)) {
			text = g_strdup_printf("the label \"%s\" moves its finished torrents to %s too, or to a folder inside it, "
				"so it is not the label \"%s\"'s own", other, own, category);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
	}
	*folder = g_steal_pointer(&own);
	return TRUE;
}

/* ── The kind ───────────────────────────────────────────────────── */

static const AxisDownloadKindInfo deluge_kind = {
	AXIS_DOWNLOAD_DELUGE_NAME, "Deluge", AXIS_DOWNLOAD_TORRENT, "Web password", "Label (lower case)",
	"Deluge's web interface address, such as http://deluge:8112. If it is served under a path, put that in the base "
	"path. Deluge ignores the priority set here.",
	deluge_test, deluge_add, deluge_list, deluge_remove, NULL, NULL, deluge_category_folder, deluge_find, FALSE
};

const AxisDownloadKindInfo *
axis_download_deluge_kind(void)
{
	return &deluge_kind;
}
