/*
 * axis-download-qbittorrent.c - the qBittorrent download client kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "download/axis-download-qbittorrent.h"

#include <string.h>

#include "core/axis-http.h"
#include "download/axis-download-qbittorrent-parse.h"
#include "storage/axis-storage-path.h"

#define API "/api/v2"

/* At most this many torrents are remembered; more clears the memory (it only
 * makes the kind quieter, never righter). */
#define MEMORY_MAX 4096

/* At most this many state words are logged. */
#define WORDS_MAX 64

/* At most this many clients' refused logins are remembered. */
#define HOLDS_MAX 64

/* The longest file name made for an upload, before ".torrent". */
#define UPLOAD_NAME_MAX 100

/* A torrent just added may not be listed at once: asked for this many times,
 * this long apart. */
#define SEEN_TRIES      6
#define SEEN_WAIT_US    (300 * 1000)

/* The user name qBittorrent has until it is changed. */
#define DEFAULT_USER    "admin"

/* ── What is remembered ─────────────────────────────────────────── */

typedef struct
{
	gchar    *fingerprint;	/* of the user name and password the login was refused for */
	gint64    until_us;	/* g_get_monotonic_time() */
	gboolean  banned;	/* a 403, not a wrong password */
} Hold;

G_LOCK_DEFINE_STATIC(memory);
static GHashTable *remembered;	/* (memory) "client\nhash" -> 1 + state */
static GHashTable *logged_words;	/* (memory) gchar * */
static GHashTable *holds;	/* (memory) client's name -> Hold */

static void
hold_free(gpointer data)
{
	Hold *hold = data;

	g_free(hold->fingerprint);
	g_free(hold);
}

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
axis_download_qbittorrent_forget(void)
{
	G_LOCK(memory);
	g_clear_pointer(&remembered, g_hash_table_unref);
	g_clear_pointer(&logged_words, g_hash_table_unref);
	g_clear_pointer(&holds, g_hash_table_unref);
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
		g_message("downloads: qBittorrent client %s reported a torrent state Axis does not know (\"%s\"); torrents "
			"keep their last known state", axis_download_conn_get_label(conn), shown);
}

/* ── Refused logins ─────────────────────────────────────────────── */

/* A digest of the user name and password a login was made with: enough to
 * tell that they changed, held in memory only and never shown. */
static gchar *
fingerprint(AxisDownloadConn *conn)
{
	const gchar *user = axis_download_conn_get_username(conn);
	const gchar *password = axis_download_conn_get_secret(conn);
	g_autofree gchar *both = g_strdup_printf("%s\n%s", user != NULL ? user : DEFAULT_USER,
		password != NULL ? password : "");
	gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, both, -1);

	memset(both, 0, strlen(both));
	return digest;
}

static void
hold_set(
	AxisDownloadConn *conn,
	const gchar      *print,
	guint             seconds,
	gboolean          banned
){
	Hold *hold = g_new0(Hold, 1);

	hold->fingerprint = g_strdup(print);
	hold->until_us = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;
	hold->banned = banned;
	G_LOCK(memory);
	if (holds == NULL)
		holds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, hold_free);
	if (g_hash_table_size(holds) >= HOLDS_MAX)
		g_hash_table_remove_all(holds);
	g_hash_table_insert(holds, g_strdup(axis_download_conn_get_label(conn)), hold);
	G_UNLOCK(memory);
}

static void
hold_clear(AxisDownloadConn *conn)
{
	G_LOCK(memory);
	if (holds != NULL)
		g_hash_table_remove(holds, axis_download_conn_get_label(conn));
	G_UNLOCK(memory);
}

/* Whether a refused login of these credentials is still being waited out:
 * the minutes left (at least 1) and whether it was a ban. */
static gboolean
hold_active(
	AxisDownloadConn *conn,
	const gchar      *print,
	guint            *minutes,
	gboolean         *banned
){
	gboolean active = FALSE;
	Hold *hold;

	G_LOCK(memory);
	hold = holds != NULL ? g_hash_table_lookup(holds, axis_download_conn_get_label(conn)) : NULL;
	if (hold != NULL && strcmp(hold->fingerprint, print) == 0 && hold->until_us > g_get_monotonic_time()) {
		gint64 left_s = (hold->until_us - g_get_monotonic_time()) / G_USEC_PER_SEC;

		*minutes = (guint)(left_s / 60) + 1;
		*banned = hold->banned;
		active = TRUE;
	}
	G_UNLOCK(memory);
	return active;
}

/* ── Requests ───────────────────────────────────────────────────── */

/* Bytes that are wiped when they are let go: a request body holds the
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

static gboolean
fail(
	GError           **error,
	AxisDownloadError  code,
	const gchar       *message
){
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, (gint)code, message);
	return FALSE;
}

/* @text as an application/x-www-form-urlencoded value, for a body. */
static void
form_append(
	GString     *body,
	const gchar *name,
	const gchar *value
){
	g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

	if (body->len > 0)
		g_string_append_c(body, '&');
	g_string_append_printf(body, "%s=%s", name, escaped);
	/* The escaped copy may hold the password. */
	memset(escaped, 0, strlen(escaped));
}

/* The headers qBittorrent's CSRF check looks at: the client's own address,
 * twice.  @store holds five pointers. */
static const gchar *const *
own_headers(
	AxisDownloadConn *conn,
	const gchar     **store
){
	const gchar *address = axis_download_conn_get_address(conn);

	if (address == NULL || address[0] == '\0')
		return NULL;
	store[0] = "Referer";
	store[1] = address;
	store[2] = "Origin";
	store[3] = address;
	store[4] = NULL;
	return store;
}

static gboolean
raw_get(
	AxisDownloadConn  *conn,
	const gchar       *path,
	GBytes           **answer,
	GError           **error
){
	const gchar *store[5];

	return axis_download_conn_get(conn, path, own_headers(conn, store), answer, error);
}

static gboolean
raw_post(
	AxisDownloadConn  *conn,
	const gchar       *path,
	const gchar       *content_type,
	GBytes            *body,
	GBytes           **answer,
	GError           **error
){
	const gchar *store[5];

	return axis_download_conn_post(conn, path, content_type, body, own_headers(conn, store), answer, error);
}

/* The minutes as words. */
static gchar *
minutes_words(guint minutes)
{
	return minutes <= 1 ? g_strdup("a minute") : g_strdup_printf("%u minutes", minutes);
}

/*
 * Logs in: a POST of the user name and the password, on a clean slate.
 * @deliberate: a person asked for it (the test): the wait after a refused login
 * does not stop it.  A refused password is %AXIS_DOWNLOAD_ERROR_AUTH; so is a
 * ban (a 403), and either is remembered so that no second attempt follows by
 * itself for a while.
 */
static gboolean
login(
	AxisDownloadConn  *conn,
	gboolean           deliberate,
	GError           **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *print = NULL;
	g_autofree gchar *words = NULL;
	g_autofree gchar *text = NULL;
	const gchar *password = axis_download_conn_get_secret(conn);
	const gchar *user = axis_download_conn_get_username(conn);
	GString *form;
	gsize length = 0;
	const gchar *data;
	guint minutes = 0;
	gboolean banned = FALSE;
	gsize size = 0;
	gchar *taken;

	if (password == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "no password is set");
	print = fingerprint(conn);
	if (!deliberate && hold_active(conn, print, &minutes, &banned)) {
		words = minutes_words(minutes);
		text = banned ?
			g_strdup_printf("qBittorrent answered 403 to the login a moment ago, which means it has most likely banned "
				"this address after too many failed logins. Axis is not trying again for %s, and does not retry in a "
				"loop: check the user name and password, or lift the ban in qBittorrent. Testing the client tries once "
				"more.", words) :
			g_strdup_printf("qBittorrent refused the user name or password a moment ago, so Axis is not trying again for "
				"%s (every wrong attempt counts toward a ban of this address). Save the right ones, or test the client, "
				"to try now.", words);
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, text);
	}
	axis_download_conn_forget_session(conn);
	/* Room for the longest password and user name escaped, so that the buffer
	 * is never moved (and a copy of it left behind). */
	form = g_string_sized_new(8192);
	form_append(form, "username", user != NULL ? user : DEFAULT_USER);
	form_append(form, "password", password);
	size = form->len;
	taken = g_string_free(form, FALSE);
	body = bytes_wiped(taken, size);
	if (!raw_post(conn, API "/auth/login", "application/x-www-form-urlencoded", body, &answer, &failure)) {
		guint status = axis_download_conn_get_last_status(conn);

		if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH) && status == 403) {
			hold_set(conn, print, AXIS_DOWNLOAD_QBITTORRENT_BAN_HOLD_S, TRUE);
			return fail(error, AXIS_DOWNLOAD_ERROR_AUTH,
				"qBittorrent answered 403 to the login: it has most likely banned this address after too many failed "
				"logins (it lifts a ban by itself, after an hour unless it was set otherwise). Axis is not trying again "
				"for 15 minutes and does not retry in a loop: check the user name and password, or lift the ban in "
				"qBittorrent, then test the client.");
		}
		if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH)) {
			hold_set(conn, print, AXIS_DOWNLOAD_QBITTORRENT_WRONG_HOLD_S, FALSE);
			return fail(error, AXIS_DOWNLOAD_ERROR_AUTH,
				"qBittorrent refused the user name or password (it answered 401). Axis is not trying again for 10 "
				"minutes, since every wrong attempt counts toward a ban of this address; save the right ones, or test "
				"the client, to try now.");
		}
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	data = g_bytes_get_data(answer, &length);
	switch (axis_qbit_parse_text(data, length)) {
	case AXIS_QBIT_TEXT_OK:
		break;
	case AXIS_QBIT_TEXT_FAILS:
		hold_set(conn, print, AXIS_DOWNLOAD_QBITTORRENT_WRONG_HOLD_S, FALSE);
		return fail(error, AXIS_DOWNLOAD_ERROR_AUTH,
			"qBittorrent refused the user name or password (it answered \"Fails.\"). Axis is not trying again for 10 "
			"minutes, since every wrong attempt counts toward a ban of this address; save the right ones, or test the "
			"client, to try now.");
	default:
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"qBittorrent's answer to the login cannot be read: it is neither \"Ok.\" nor \"Fails.\", so this may not be "
			"qBittorrent's web interface");
	}
	if (!axis_download_conn_has_session(conn))
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"qBittorrent accepted the login but set no session, so this may not be qBittorrent's web interface");
	hold_clear(conn);
	return TRUE;
}

typedef enum
{
	MODE_NEVER_RETRY,	/* a call that changes something: a lost session is reported, not repaired */
	MODE_RETRY_ONCE		/* a call that changes nothing (or that is the same twice): login again, once */
} Mode;

/*
 * One call: logs in first when there is no session, and sends it.  A 403
 * is a lost session (the call did nothing): with @mode MODE_RETRY_ONCE the
 * session is made again and the call sent once more; with MODE_NEVER_RETRY it
 * is %AXIS_DOWNLOAD_ERROR_REFUSED and the next call logs in.  Any other
 * failure is the connection's own.  @body is %NULL for a GET.
 */
static gboolean
call(
	AxisDownloadConn  *conn,
	Mode               mode,
	const gchar       *path,
	const gchar       *content_type,
	GBytes            *body,
	GBytes           **answer,
	GError           **error
){
	guint attempt;

	*answer = NULL;
	if (!axis_download_conn_has_session(conn) && !login(conn, FALSE, error))
		return FALSE;
	for (attempt = 0; attempt < 2; attempt++) {
		g_autoptr(GError) failure = NULL;
		gboolean ok = body != NULL ? raw_post(conn, path, content_type, body, answer, &failure) :
			raw_get(conn, path, answer, &failure);

		if (ok)
			return TRUE;
		if (!g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH) ||
		    axis_download_conn_get_last_status(conn) != 403) {
			g_propagate_error(error, g_steal_pointer(&failure));
			return FALSE;
		}
		axis_download_conn_forget_session(conn);
		if (mode == MODE_NEVER_RETRY)
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
				"qBittorrent no longer accepts the session, so nothing was done; it will not be repeated by itself");
		if (attempt > 0)
			return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "qBittorrent refused the session again right after a login");
		if (!login(conn, FALSE, error))
			return FALSE;
	}
	return fail(error, AXIS_DOWNLOAD_ERROR_AUTH, "qBittorrent refused the session again right after a login");
}

static gboolean
get_text(
	AxisDownloadConn  *conn,
	const gchar       *path,
	GBytes           **answer,
	GError           **error
){
	return call(conn, MODE_RETRY_ONCE, path, NULL, NULL, answer, error);
}

/* ── The category ───────────────────────────────────────────────── */

/* Whether the category is there: 1, 0; -1 with @error. */
static gint
category_listed(
	AxisDownloadConn  *conn,
	const gchar       *category,
	GError           **error
){
	g_autoptr(GBytes) answer = NULL;
	gsize length = 0;
	const gchar *data;
	gint found;

	if (!get_text(conn, API "/torrents/categories", &answer, error))
		return -1;
	data = g_bytes_get_data(answer, &length);
	found = axis_qbit_parse_has_category(data, length, category);
	if (found < 0) {
		fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"qBittorrent's answer cannot be read: it has no list of categories");
		return -1;
	}
	return found;
}

/* Makes sure @category exists, creating it when it does not.  Nothing has been
 * added to qBittorrent by then, so a failure here leaves nothing behind.
 * *@created: the category was made. */
static gboolean
ensure_category(
	AxisDownloadConn  *conn,
	const gchar       *category,
	gboolean          *created,
	GError           **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GError) failure = NULL;
	GString *form;
	gsize size;
	gint listed;

	*created = FALSE;
	listed = category_listed(conn, category, error);
	if (listed < 0)
		return FALSE;
	if (listed > 0)
		return TRUE;
	form = g_string_new(NULL);
	form_append(form, "category", category);
	form_append(form, "savePath", "");
	size = form->len;
	body = g_bytes_new_take(g_string_free(form, FALSE), size);
	if (!call(conn, MODE_RETRY_ONCE, API "/torrents/createCategory", "application/x-www-form-urlencoded", body,
	        &answer, &failure)) {
		g_autoptr(GError) reread = NULL;

		/* A 409 is said to mean "exists", and also "not a name it takes": read the list again. */
		if (axis_download_conn_get_last_status(conn) == 409 && category_listed(conn, category, &reread) == 1)
			return TRUE;
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	*created = TRUE;
	return TRUE;
}

/* ── Test ───────────────────────────────────────────────────────── */

static gboolean
qbit_test(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	AxisDownloadTestResult      *result,
	GError                     **error
){
	g_autoptr(GBytes) version_answer = NULL;
	g_autoptr(GBytes) api_answer = NULL;
	g_autoptr(GError) ignored = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *api = NULL;
	g_autoptr(GString) note = g_string_new(NULL);
	gboolean created = FALSE;
	gsize length = 0;
	const gchar *data;

	if (!axis_download_category_valid(category))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a category name Axis can use");
	/* A fresh login: the password is the one thing that is being tested. */
	if (!login(conn, TRUE, error))
		return FALSE;
	if (!get_text(conn, API "/app/version", &version_answer, error))
		return FALSE;
	data = g_bytes_get_data(version_answer, &length);
	version = axis_qbit_parse_version(data, length);
	if (version == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"qBittorrent's answer cannot be read: /app/version is not a version, so this may not be qBittorrent");
	if (get_text(conn, API "/app/webapiVersion", &api_answer, &ignored)) {
		data = g_bytes_get_data(api_answer, &length);
		api = axis_qbit_parse_version(data, length);
	}
	if (!ensure_category(conn, category, &created, error))
		return FALSE;
	if (created)
		g_string_append_printf(note, "The category \"%s\" did not exist in qBittorrent; it was made. ", category);
	g_string_append(note, "qBittorrent ignores the priority set here.");
	result->version = api != NULL ? g_strdup_printf("%s (Web API %s)", version, api) : g_steal_pointer(&version);
	result->category_exists = 1;
	result->note = g_strdup(note->str);
	return TRUE;
}

/* ── Add ────────────────────────────────────────────────────────── */

/* The file name an upload carries (qBittorrent only shows it): made here, from
 * the release's own name with everything but letters, digits, '.', '_' and '-'
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

static void
multipart_text(
	GString     *body,
	const gchar *boundary,
	const gchar *name,
	const gchar *value
){
	g_string_append_printf(body, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary, name,
		value);
}

/* A boundary that is not in @file. */
static gchar *
boundary_for(GBytes *file)
{
	gsize size = 0;
	const gchar *data = file != NULL ? g_bytes_get_data(file, &size) : NULL;
	gchar *boundary;

	do {
		boundary = g_strdup_printf("----AxisForm%08x%08x%08x", g_random_int(), g_random_int(), g_random_int());
		if (data == NULL || memmem(data, size, boundary, strlen(boundary)) == NULL)
			return boundary;
		g_free(boundary);
	} while (TRUE);
}

/* The form of /torrents/add: the magnet or the file, the category, and not
 * paused (4.x calls that "paused", 5.x "stopped"; each ignores the other). */
static GBytes *
add_form(
	const AxisDownloadRelease  *release,
	const gchar                *category,
	gchar                     **content_type
){
	g_autofree gchar *boundary = boundary_for(release->file);
	GString *body = g_string_new(NULL);
	gsize size;

	if (release->magnet != NULL) {
		multipart_text(body, boundary, "urls", release->magnet);
	} else {
		g_autofree gchar *file_name = upload_name(release);
		gsize file_size = 0;
		const gchar *file = g_bytes_get_data(release->file, &file_size);

		g_string_append_printf(body, "--%s\r\nContent-Disposition: form-data; name=\"torrents\"; filename=\"%s\"\r\n"
			"Content-Type: application/x-bittorrent\r\n\r\n", boundary, file_name);
		g_string_append_len(body, file, (gssize)file_size);
		g_string_append(body, "\r\n");
	}
	multipart_text(body, boundary, "category", category);
	multipart_text(body, boundary, "paused", "false");
	multipart_text(body, boundary, "stopped", "false");
	g_string_append_printf(body, "--%s--\r\n", boundary);
	*content_type = g_strdup_printf("multipart/form-data; boundary=%s", boundary);
	size = body->len;
	return g_bytes_new_take(g_string_free(body, FALSE), size);
}

/* What /torrents/info says of @hash: its slot (the category as it is, "" for
 * none), or %NULL when it is not listed.  *@ok is %FALSE with @error when the
 * answer could not be had or read: also when it holds an entry Axis cannot
 * read (it may be this torrent's: "not listed" would then be a guess), or this
 * torrent's entry cannot be read. */
static AxisQbitSlot *
look_up(
	AxisDownloadConn  *conn,
	const gchar       *hash,
	gboolean          *ok,
	GError           **error
){
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	g_autofree gchar *path = g_strdup_printf(API "/torrents/info?hashes=%s", hash);
	g_autofree gchar *problem = NULL;
	guint unreadable = 0;
	gboolean cut = FALSE;
	gsize length = 0;
	const gchar *data;
	guint i;

	*ok = FALSE;
	if (!get_text(conn, path, &answer, error))
		return NULL;
	data = g_bytes_get_data(answer, &length);
	slots = axis_qbit_parse_torrents_counted(data, length, NULL, &unreadable, &cut, &problem);
	if (slots == NULL) {
		g_autofree gchar *text = g_strdup_printf("qBittorrent's answer cannot be read: %s", problem != NULL ? problem :
			"it is not a list of torrents");

		fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, text);
		return NULL;
	}
	if (unreadable > 0 || cut) {
		fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "qBittorrent's answer about the torrent holds an entry Axis cannot "
			"read, so whether it still has the torrent cannot be told");
		return NULL;
	}
	for (i = 0; i < slots->len; i++) {
		AxisQbitSlot *slot = g_ptr_array_index(slots, i);

		if (g_ascii_strcasecmp(slot->item->client_ref, hash) != 0)
			continue;
		if (slot->item->unreadable != NULL) {
			g_autofree gchar *text = g_strdup_printf("qBittorrent's entry for the torrent cannot be read (%s), so what "
				"it is doing with it cannot be told", slot->item->unreadable);

			fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, text);
			return NULL;
		}
		*ok = TRUE;
		return g_ptr_array_steal_index(slots, i);
	}
	*ok = TRUE;
	return NULL;
}

static gboolean
qbit_add(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const AxisDownloadRelease   *release,
	const gchar                 *category,
	AxisDownloadPriority         priority,
	gchar                      **client_ref,
	GError                     **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *content_type = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *hash = NULL;
	AxisQbitSlot *slot = NULL;
	gboolean created = FALSE;
	gboolean looked = FALSE;
	gsize length = 0;
	const gchar *data;
	guint tries;

	/* Refused before anything is sent. */
	if (release->protocol != AXIS_DOWNLOAD_TORRENT || (release->magnet == NULL) == (release->file == NULL))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID,
			"qBittorrent takes torrent files and magnet links, not Usenet releases");
	if (release->file != NULL && g_bytes_get_size(release->file) == 0)
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "the release file is empty");
	if (!axis_download_hash_valid(release->info_hash))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID,
			"the release has no info-hash Axis can look for it in qBittorrent by");
	if (!axis_download_category_valid(category))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a category name Axis can use");
	hash = g_strdup(release->info_hash);

	/* A session qBittorrent accepts, the category made sure of, and the hash
	 * not there yet, before the torrent exists: whatever fails here leaves
	 * nothing behind. */
	if (!ensure_category(conn, category, &created, error))
		return FALSE;
	slot = look_up(conn, hash, &looked, error);
	if (!looked)
		return FALSE;
	if (slot != NULL) {
		text = g_strdup_printf("qBittorrent has this torrent already%s%s%s; Axis does not take over a torrent it did "
			"not add, so it is left as it is", slot->item->category[0] != '\0' ? " (in the category \"" : "",
			slot->item->category[0] != '\0' ? slot->item->category : "", slot->item->category[0] != '\0' ? "\")" : "");
		axis_qbit_slot_free(slot);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}

	/* The add itself: never repeated, whatever happens. */
	body = add_form(release, category, &content_type);
	if (!call(conn, MODE_NEVER_RETRY, API "/torrents/add", content_type, body, &answer, &failure)) {
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	data = g_bytes_get_data(answer, &length);
	switch (axis_qbit_parse_text(data, length)) {
	case AXIS_QBIT_TEXT_OK:
		break;
	case AXIS_QBIT_TEXT_FAILS:
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
			"qBittorrent answered \"Fails.\" and does not say why: it cannot read the file or the link, or already has "
			"the torrent");
	default:
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"qBittorrent's answer to the add cannot be read: it is neither \"Ok.\" nor \"Fails.\"");
	}

	/* "Ok." carries no hash: the torrent is asked for by the one Axis computed. */
	for (tries = 0; tries < SEEN_TRIES; tries++) {
		g_autoptr(GError) lookup_error = NULL;

		if (tries > 0)
			g_usleep(SEEN_WAIT_US);
		slot = look_up(conn, hash, &looked, &lookup_error);
		if (slot != NULL)
			break;
		if (!looked && tries + 1 == SEEN_TRIES)
			return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
				"qBittorrent said \"Ok.\" but its list of torrents could not be read afterwards, so it is not known "
				"whether it took the torrent");
	}
	if (slot == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"qBittorrent said \"Ok.\" but does not list the torrent, so it is not known whether it took it");
	if (strcmp(slot->item->category, category) != 0) {
		axis_qbit_slot_free(slot);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED,
			"qBittorrent said \"Ok.\" but the torrent is not in Axis's category, so Axis does not follow it and leaves it "
			"as it is");
	}
	axis_qbit_slot_free(slot);
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
	AxisQbitSlot     *slot
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
qbit_list(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	GPtrArray                  **items,
	GError                     **error
){
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *problem = NULL;
	gsize length = 0;
	const gchar *data;
	guint i;

	if (category != NULL && !axis_download_category_valid(category))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a category name Axis can use");
	/* Only qBittorrent's torrents in Axis's category (and then only those of
	 * exactly that name: its filter may take sub-categories along); the layer
	 * filters again whatever comes.  Without one, every torrent it has. */
	path = category != NULL ? g_strdup_printf(API "/torrents/info?category=%s", category) :
		g_strdup(API "/torrents/info");
	if (!get_text(conn, path, &answer, error))
		return FALSE;
	data = g_bytes_get_data(answer, &length);
	{
		guint unreadable = 0;
		gboolean cut = FALSE;

		slots = axis_qbit_parse_torrents_counted(data, length, category, &unreadable, &cut, &problem);
		/* What could not be read, or was not looked at, is not taken for
		 * absent where that matters (axis_download_conn_set_list_partial()). */
		if (slots != NULL && (unreadable > 0 || cut))
			axis_download_conn_set_list_partial(conn);
	}
	if (slots == NULL) {
		g_autofree gchar *text = g_strdup_printf("qBittorrent's answer cannot be read: %s", problem != NULL ? problem :
			"it is not a list of torrents");

		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, text);
	}

	*items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	for (i = 0; i < slots->len; i++) {
		AxisQbitSlot *slot = g_ptr_array_index(slots, i);

		/* Only what qBittorrent said in words known is remembered: a placeholder
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

static gboolean
qbit_remove(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	gboolean                     delete_data,
	GError                     **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) answer = NULL;
	AxisQbitSlot *before;
	GString *form;
	gboolean looked = FALSE;
	guint tries;

	if (!axis_download_hash_valid(client_ref))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not an info-hash qBittorrent gives");
	/* qBittorrent answers a delete 200 whether it had the torrent or not, so
	 * it is asked first: one it does not list is gone already (something
	 * else removed it), and nothing is asked of it. */
	before = look_up(conn, client_ref, &looked, error);
	if (!looked)
		return FALSE;
	if (before == NULL) {
		forget_torrent(conn, client_ref);
		return fail(error, AXIS_DOWNLOAD_ERROR_GONE, "qBittorrent no longer lists the torrent: something else removed "
			"it already");
	}
	/* Only a torrent still in Axis's category: one a person moved to another
	 * is theirs now, and is left alone. */
	if (g_strcmp0(before->item->category, axis_download_conn_get_category(conn)) != 0) {
		g_autofree gchar *text = g_strdup_printf("qBittorrent has the torrent in the category \"%s\" now, not in "
			"Axis's: a person moved it there, so it is not Axis's to remove, and it was left alone",
			before->item->category != NULL ? before->item->category : "");

		axis_qbit_slot_free(before);
		forget_torrent(conn, client_ref);
		return fail(error, AXIS_DOWNLOAD_ERROR_NOT_OURS, text);
	}
	axis_qbit_slot_free(before);
	form = g_string_new(NULL);
	form_append(form, "hashes", client_ref);
	form_append(form, "deleteFiles", delete_data ? "true" : "false");
	{
		gsize size = form->len;

		body = g_bytes_new_take(g_string_free(form, FALSE), size);
	}
	/* Removing twice is harmless: a lost session is made again, once. */
	if (!call(conn, MODE_RETRY_ONCE, API "/torrents/delete", "application/x-www-form-urlencoded", body, &answer,
	        error))
		return FALSE;
	/* qBittorrent answers 200 whether it had the torrent or not: ask. */
	for (tries = 0; tries < SEEN_TRIES; tries++) {
		AxisQbitSlot *slot;

		if (tries > 0)
			g_usleep(SEEN_WAIT_US);
		slot = look_up(conn, client_ref, &looked, error);
		if (!looked)
			return FALSE;
		if (slot == NULL) {
			forget_torrent(conn, client_ref);
			return TRUE;
		}
		axis_qbit_slot_free(slot);
	}
	return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, "qBittorrent still lists the torrent after it was asked to remove it");
}

/* The torrent @client_ref in any category (its "category" as qBittorrent says
 * it, "" for none), or %NULL when it does not list it. */
static gboolean
qbit_find(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	AxisDownloadItem           **item,
	GError                     **error
){
	AxisQbitSlot *slot;
	gboolean looked = FALSE;

	*item = NULL;
	if (!axis_download_hash_valid(client_ref))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not an info-hash qBittorrent gives");
	slot = look_up(conn, client_ref, &looked, error);
	if (!looked)
		return FALSE;
	if (slot != NULL) {
		*item = g_steal_pointer(&slot->item);
		axis_qbit_slot_free(slot);
	}
	return TRUE;
}

/* ── The category's folder ──────────────────────────────────────── */

/* @path as a folder: normalised and absolute, or %NULL. */
static gchar *
folder_of(const gchar *path)
{
	gchar *normal = NULL;

	if (path == NULL || !axis_storage_path_normalize(path, TRUE, &normal, NULL))
		return NULL;
	return normal;
}

/*
 * Where qBittorrent puts the torrents of Axis's category: the category's own
 * save path (GET /torrents/categories, "savePath"), when it is a full path
 * and neither qBittorrent's default save path (GET /app/defaultSavePath) nor
 * another category's folder is it or lies inside it (a category with no save
 * path of its own is taken as the default save path and its name, as
 * qBittorrent's automatic management puts it; believed).  A category with no
 * save path, or a relative one, is no folder of its own.  Read only.  Asked
 * only for a torrent qBittorrent no longer has after its import; a torrent
 * that was saved elsewhere (qBittorrent's manual management ignores the
 * category's path) is then outside this folder and is left.
 */
static gboolean
qbit_category_folder(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	gchar                      **folder,
	GError                     **error
){
	g_autoptr(GBytes) categories_answer = NULL;
	g_autoptr(GBytes) default_answer = NULL;
	g_autoptr(GPtrArray) categories = NULL;
	g_autofree gchar *own = NULL;
	g_autofree gchar *general = NULL;
	g_autofree gchar *text = NULL;
	const AxisQbitCategory *mine = NULL;
	gsize length = 0;
	const gchar *data;
	guint i;

	*folder = NULL;
	if (!axis_download_category_valid(category))
		return fail(error, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a category name Axis can use");
	if (!get_text(conn, API "/torrents/categories", &categories_answer, error))
		return FALSE;
	data = g_bytes_get_data(categories_answer, &length);
	categories = axis_qbit_parse_categories(data, length);
	if (categories == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "qBittorrent's answer cannot be read: it has no list of "
			"categories");
	for (i = 0; i < categories->len; i++) {
		const AxisQbitCategory *one = g_ptr_array_index(categories, i);

		if (strcmp(one->name, category) == 0)
			mine = one;
	}
	if (mine == NULL) {
		text = g_strdup_printf("qBittorrent has no category called \"%s\"", category);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	if (mine->save_path == NULL || mine->save_path[0] == '\0') {
		text = g_strdup_printf("the category \"%s\" has no save path of its own in qBittorrent", category);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	own = folder_of(mine->save_path);
	if (own == NULL) {
		text = g_strdup_printf("the category \"%s\" has a save path that is not a full path", category);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	if (!get_text(conn, API "/app/defaultSavePath", &default_answer, error))
		return FALSE;
	data = g_bytes_get_data(default_answer, &length);
	{
		g_autofree gchar *raw = axis_qbit_parse_path_text(data, length);

		general = folder_of(raw);
	}
	if (general == NULL)
		return fail(error, AXIS_DOWNLOAD_ERROR_UNREADABLE, "qBittorrent's answer cannot be read: its default save "
			"path is not a full path");
	if (axis_storage_path_contains(own, general)) {
		text = g_strdup_printf("the category \"%s\" saves to %s, which is (or holds) qBittorrent's default save path "
			"for every torrent, not a folder of the category's own", category, own);
		return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
	}
	for (i = 0; i < categories->len; i++) {
		const AxisQbitCategory *one = g_ptr_array_index(categories, i);
		g_autofree gchar *theirs = NULL;

		if (one == mine)
			continue;
		if (one->save_path != NULL && one->save_path[0] == '\0') {
			g_autofree gchar *implied = g_strconcat(general, "/", one->name, NULL);

			theirs = folder_of(implied);
		} else if (one->save_path != NULL) {
			theirs = folder_of(one->save_path);
		}
		if (theirs == NULL) {
			text = g_strdup_printf("the category \"%s\" saves to a folder that cannot be told apart from the category "
				"\"%s\"'s", one->name, category);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
		if (axis_storage_path_contains(own, theirs)) {
			text = g_strdup_printf("the category \"%s\" saves to %s too, or to a folder inside it, so it is not the "
				"category \"%s\"'s own", one->name, own, category);
			return fail(error, AXIS_DOWNLOAD_ERROR_REFUSED, text);
		}
	}
	*folder = g_steal_pointer(&own);
	return TRUE;
}

/* ── The kind ───────────────────────────────────────────────────── */

static const AxisDownloadKindInfo qbittorrent_kind = {
	AXIS_DOWNLOAD_QBITTORRENT_NAME, "qBittorrent", AXIS_DOWNLOAD_TORRENT, "Password", "Category",
	"qBittorrent's web interface address, such as http://qbittorrent:8080. If it is served under a path, put that in "
	"the base path. qBittorrent ignores the priority set here.",
	qbit_test, qbit_add, qbit_list, qbit_remove, NULL, "User name", qbit_category_folder, qbit_find, FALSE
};

const AxisDownloadKindInfo *
axis_download_qbittorrent_kind(void)
{
	return &qbittorrent_kind;
}
