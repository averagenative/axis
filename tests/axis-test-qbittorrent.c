/*
 * axis-test-qbittorrent.c - test support: a stand-in for qBittorrent's Web
 * API v2 (see axis-test-qbittorrent.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-test-qbittorrent.h"

#include <string.h>

#include "download/axis-download-bencode.h"
#include "download/axis-download-model.h"

typedef struct
{
	gchar   *hash;
	gchar   *name;
	gchar   *category;	/* NULL: none */
	gchar   *state;
	gint64   size;
	gdouble  progress;
	gchar   *save_path;
	gchar   *content_path;	/* NULL: none */
	gboolean hidden;	/* never listed */
} Torrent;

typedef struct
{
	guint   status;
	gchar  *type;
	gchar  *body;
} Raw;

struct _AxisTestQbit
{
	AxisTestPlex           *server;
	GMutex                  lock;
	AxisTestQbitGeneration  generation;
	AxisTestQbitMode        mode;
	gboolean                flags[AXIS_TEST_QBIT_N_FLAGS];
	gchar                  *username;
	gchar                  *password;
	gchar                  *prefix;		/* in front of /api/v2 */
	gchar                  *base;
	gchar                  *session;	/* (lock) the session it accepts, or NULL */
	GPtrArray              *sessions;	/* (lock) every session ever given */
	guint                   ban_after;
	guint                   failed;
	gboolean                banned;
	guint                   banned_logins;
	guint                   bad_requests;
	GPtrArray              *categories;	/* (lock) gchar * */
	GPtrArray              *torrents;	/* (lock) Torrent */
	GPtrArray              *calls;		/* (lock) */
	GHashTable             *raw;		/* (lock) path -> Raw */
	gchar                  *expire_before;
	gchar                  *redirect;
	GHashTable             *last_add;	/* (lock) field -> text */
	gchar                  *last_user;
	GHashTable             *save_paths;	/* (lock) category -> its save path ("" when none) */
	gchar                  *default_path;	/* (lock) the default save path */
};

static void
torrent_free(gpointer data)
{
	Torrent *torrent = data;

	g_free(torrent->hash);
	g_free(torrent->name);
	g_free(torrent->category);
	g_free(torrent->state);
	g_free(torrent->save_path);
	g_free(torrent->content_path);
	g_free(torrent);
}

static void
raw_free(gpointer data)
{
	Raw *raw = data;

	g_free(raw->type);
	g_free(raw->body);
	g_free(raw);
}

static void
note(
	AxisTestQbit *self,
	const gchar  *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
note(
	AxisTestQbit *self,
	const gchar  *format,
	...
){
	va_list ap;

	va_start(ap, format);
	g_ptr_array_add(self->calls, g_strdup_vprintf(format, ap));
	va_end(ap);
}

static void
answer_text(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *type,
	const gchar        *text
){
	answer->status = status;
	answer->content_type = type != NULL ? g_strdup(type) : NULL;
	answer->body = g_bytes_new(text, strlen(text));
}

static Torrent *
find_torrent(
	AxisTestQbit *self,
	const gchar  *hash
){
	guint i;

	for (i = 0; hash != NULL && i < self->torrents->len; i++) {
		Torrent *torrent = g_ptr_array_index(self->torrents, i);

		if (g_ascii_strcasecmp(torrent->hash, hash) == 0)
			return torrent;
	}
	return NULL;
}

static gboolean
has_name(
	GPtrArray   *names,
	const gchar *name
){
	guint i;

	for (i = 0; name != NULL && i < names->len; i++) {
		if (strcmp(g_ptr_array_index(names, i), name) == 0)
			return TRUE;
	}
	return FALSE;
}

/* ── Forms ──────────────────────────────────────────────────────── */

/* A urlencoded body as a table of name -> value. */
static GHashTable *
parse_form(GBytes *body)
{
	GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	gsize length = 0;
	const gchar *data = g_bytes_get_data(body, &length);
	g_autofree gchar *text = g_strndup(data != NULL ? data : "", length);
	gchar **pairs = g_strsplit(text, "&", -1);
	guint i;

	for (i = 0; pairs[i] != NULL; i++) {
		gchar *eq = strchr(pairs[i], '=');

		if (eq == NULL)
			continue;
		*eq = '\0';
		g_hash_table_replace(table, g_uri_unescape_string(pairs[i], NULL), g_uri_unescape_string(eq + 1, NULL));
	}
	g_strfreev(pairs);
	return table;
}

/* A multipart body as a table of name -> bytes (GBytes), with the names of
 * the files in @file_names (name -> file name). */
static GHashTable *
parse_multipart(
	const gchar  *content_type,
	GBytes       *body,
	GHashTable  **file_names
){
	GHashTable *parts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
	const gchar *at = content_type != NULL ? strstr(content_type, "boundary=") : NULL;
	g_autofree gchar *delimiter = NULL;
	gsize length = 0;
	const gchar *data = g_bytes_get_data(body, &length);
	const gchar *cursor = data;
	const gchar *end = data + length;

	*file_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (at == NULL || data == NULL)
		return parts;
	delimiter = g_strconcat("--", at + 9, NULL);
	while (cursor < end) {
		const gchar *start = memmem(cursor, (gsize)(end - cursor), delimiter, strlen(delimiter));
		const gchar *head;
		const gchar *blank;
		const gchar *next;
		g_autofree gchar *headers = NULL;
		gchar *name_at;

		if (start == NULL)
			break;
		head = start + strlen(delimiter);
		if (head + 2 <= end && head[0] == '-' && head[1] == '-')
			break;
		if (head + 2 <= end && head[0] == '\r' && head[1] == '\n')
			head += 2;
		blank = memmem(head, (gsize)(end - head), "\r\n\r\n", 4);
		if (blank == NULL)
			break;
		headers = g_strndup(head, (gsize)(blank - head));
		blank += 4;
		next = memmem(blank, (gsize)(end - blank), delimiter, strlen(delimiter));
		if (next == NULL)
			break;
		name_at = strstr(headers, "name=\"");
		if (name_at != NULL) {
			gchar *close = strchr(name_at + 6, '"');
			gchar *file_at = strstr(headers, "filename=\"");

			if (close != NULL) {
				gchar *name = g_strndup(name_at + 6, (gsize)(close - (name_at + 6)));

				/* The part ends before the CRLF that precedes the next delimiter. */
				g_hash_table_replace(parts, name, g_bytes_new(blank, (gsize)(next - blank) >= 2 ? (gsize)(next - blank) - 2 : 0));
				if (file_at != NULL) {
					gchar *file_close = strchr(file_at + 10, '"');

					if (file_close != NULL)
						g_hash_table_replace(*file_names, g_strdup(name),
							g_strndup(file_at + 10, (gsize)(file_close - (file_at + 10))));
				}
			}
		}
		cursor = next;
	}
	return parts;
}

static gchar *
part_text(
	GHashTable  *parts,
	const gchar *name
){
	GBytes *bytes = g_hash_table_lookup(parts, name);
	gsize length = 0;
	const gchar *data;

	if (bytes == NULL)
		return NULL;
	data = g_bytes_get_data(bytes, &length);
	return g_strndup(data != NULL ? data : "", length);
}

/* ── Answers ────────────────────────────────────────────────────── */

static gchar *
torrents_json(
	AxisTestQbit *self,
	const gchar  *category,
	gboolean      by_category,
	gchar       **hashes
){
	GString *out = g_string_new("[");
	gboolean first = TRUE;
	guint i;

	for (i = 0; i < self->torrents->len; i++) {
		const Torrent *torrent = g_ptr_array_index(self->torrents, i);
		gboolean downloading = strcmp(torrent->state, "downloading") == 0;
		gboolean wanted = TRUE;
		gchar *content;
		guint h;

		if (by_category) {
			const gchar *own = torrent->category != NULL ? torrent->category : "";

			if (category[0] == '\0')
				wanted = own[0] == '\0';
			else if (self->flags[AXIS_TEST_QBIT_SUBCATEGORIES])
				wanted = strcmp(own, category) == 0 ||
					(g_str_has_prefix(own, category) && own[strlen(category)] == '/');
			else
				wanted = strcmp(own, category) == 0;
		}
		if (hashes != NULL) {
			wanted = FALSE;
			for (h = 0; hashes[h] != NULL; h++) {
				if (g_ascii_strcasecmp(hashes[h], torrent->hash) == 0)
					wanted = TRUE;
			}
		}
		if (!wanted)
			continue;
		if (torrent->hidden)
			continue;
		content = self->flags[AXIS_TEST_QBIT_NO_CONTENT_PATH] || torrent->content_path == NULL ? g_strdup("null") :
			g_strdup_printf("\"%s\"", torrent->content_path);
		g_string_append_printf(out, "%s{\"added_on\": 1731000000, \"amount_left\": %" G_GINT64_FORMAT ", \"auto_tmm\": false, "
			"\"availability\": 1.0, \"category\": \"%s\", \"completed\": %" G_GINT64_FORMAT ", \"completion_on\": %s, "
			"\"content_path\": %s, \"dl_limit\": -1, \"dlspeed\": %d, \"downloaded\": %" G_GINT64_FORMAT ", "
			"\"eta\": %d, \"f_l_piece_prio\": false, \"force_start\": false, \"hash\": \"%s\", "
			"\"infohash_v1\": \"%s\", \"infohash_v2\": \"\", \"last_activity\": 1731000100, \"magnet_uri\": \"\", "
			"\"name\": \"%s\", \"num_complete\": 4, \"num_incomplete\": 2, \"num_leechs\": 1, \"num_seeds\": 3, "
			"\"priority\": 1, \"progress\": %.4f, \"ratio\": 0.0, \"save_path\": \"%s\", \"seq_dl\": false, "
			"\"size\": %" G_GINT64_FORMAT ", \"state\": \"%s\", \"super_seeding\": false, \"tags\": \"\", "
			"\"time_active\": 60, \"total_size\": %" G_GINT64_FORMAT ", \"tracker\": \"\", \"up_limit\": -1, "
			"\"uploaded\": 0, \"upspeed\": 0}",
			first ? "" : ", ", torrent->size - (gint64)((gdouble)torrent->size * torrent->progress),
			torrent->category != NULL ? torrent->category : "", (gint64)((gdouble)torrent->size * torrent->progress),
			torrent->progress >= 1.0 ? "1731000200" : "-1", content, downloading ? 5242880 : 0, (gint64)((gdouble)torrent->size * torrent->progress),
			downloading ? 300 : 8640000, torrent->hash, torrent->hash, torrent->name, torrent->progress,
			torrent->save_path, torrent->size, torrent->state, torrent->size);
		g_free(content);
		first = FALSE;
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

static gchar *
categories_json(AxisTestQbit *self)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; i < self->categories->len; i++) {
		const gchar *name = g_ptr_array_index(self->categories, i);
		const gchar *path = g_hash_table_lookup(self->save_paths, name);

		g_string_append_printf(out, "%s\"%s\": {\"name\": \"%s\", \"savePath\": \"%s\"}", i > 0 ? ", " : "", name, name,
			path != NULL ? path : "");
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static const gchar *
cookie_name(AxisTestQbit *self)
{
	static gchar name[64];

	if (self->generation == AXIS_TEST_QBIT_V4)
		return "SID";
	g_snprintf(name, sizeof(name), "QBT_SID_%u", (guint)axis_test_plex_port(self->server));
	return name;
}

static gboolean
has_session(
	AxisTestQbit              *self,
	const AxisTestPlexRequest *request
){
	const gchar *cookie = axis_test_plex_request_header(request, "Cookie");
	g_autofree gchar *wanted = NULL;

	if (cookie == NULL || self->session == NULL)
		return FALSE;
	wanted = g_strdup_printf("%s=%s", cookie_name(self), self->session);
	return strstr(cookie, wanted) != NULL;
}

static void
do_login(
	AxisTestQbit              *self,
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer
){
	g_autoptr(GHashTable) form = parse_form(request->body);
	const gchar *user = g_hash_table_lookup(form, "username");
	const gchar *password = g_hash_table_lookup(form, "password");

	g_free(self->last_user);
	self->last_user = g_strdup(user);
	if (self->banned) {
		self->banned_logins++;
		note(self, "banned:login");
		answer_text(answer, 403, "text/plain", "Your IP address has been banned after too many failed authentication attempts.");
		return;
	}
	note(self, "login");
	if (g_strcmp0(user, self->username) != 0 || g_strcmp0(password, self->password) != 0) {
		self->failed++;
		if (self->ban_after > 0 && self->failed >= self->ban_after)
			self->banned = TRUE;
		if (self->generation == AXIS_TEST_QBIT_V4)
			answer_text(answer, 200, "text/plain; charset=UTF-8", "Fails.");
		else
			answer_text(answer, 401, "text/plain", "Unauthorized");
		return;
	}
	self->failed = 0;
	g_free(self->session);
	self->session = g_strdup_printf("%08x%08x%08x%08x", g_random_int(), g_random_int(), g_random_int(), g_random_int());
	g_ptr_array_add(self->sessions, g_strdup(self->session));
	if (self->generation == AXIS_TEST_QBIT_V4)
		answer_text(answer, 200, "text/plain; charset=UTF-8", "Ok.");
	else
		answer->status = 204;
	if (!self->flags[AXIS_TEST_QBIT_NO_COOKIE]) {
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Set-Cookie");
		answer->headers[1] = g_strdup_printf("%s=%s; HttpOnly; SameSite=Strict; path=/", cookie_name(self), self->session);
	}
}

static void
add_torrent(
	AxisTestQbit *self,
	const gchar  *hash,
	const gchar  *name,
	const gchar  *category,
	gboolean      magnet
){
	Torrent *torrent = g_new0(Torrent, 1);

	torrent->hash = g_strdup(hash);
	torrent->name = g_strdup(name);
	torrent->category = self->flags[AXIS_TEST_QBIT_ADD_RECATEGORISES] ? g_strdup("manual") :
		category != NULL && category[0] != '\0' ? g_strdup(category) : NULL;
	torrent->state = g_strdup(magnet ? "metaDL" : "queuedDL");
	torrent->size = 1000;
	torrent->progress = 0.0;
	torrent->save_path = g_strdup("/downloads");
	torrent->content_path = g_strdup_printf("/downloads/%s", name);
	torrent->hidden = self->flags[AXIS_TEST_QBIT_ADD_HIDDEN];
	g_ptr_array_add(self->torrents, torrent);
}

static void
do_add(
	AxisTestQbit              *self,
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer
){
	const gchar *content_type = axis_test_plex_request_header(request, "Content-Type");
	g_autoptr(GHashTable) file_names = NULL;
	g_autoptr(GHashTable) parts = parse_multipart(content_type, request->body, &file_names);
	g_autofree gchar *urls = part_text(parts, "urls");
	g_autofree gchar *category = part_text(parts, "category");
	g_autofree gchar *paused = part_text(parts, "paused");
	g_autofree gchar *stopped = part_text(parts, "stopped");
	GBytes *file = g_hash_table_lookup(parts, "torrents");
	g_autofree gchar *hash = NULL;
	g_autofree gchar *name = NULL;
	Torrent *existing;

	note(self, "add");
	g_hash_table_remove_all(self->last_add);
	if (urls != NULL)
		g_hash_table_replace(self->last_add, g_strdup("urls"), g_strdup(urls));
	if (file != NULL) {
		g_hash_table_replace(self->last_add, g_strdup("torrents-name"),
			g_strdup(g_hash_table_lookup(file_names, "torrents") != NULL ? g_hash_table_lookup(file_names, "torrents") : ""));
		g_hash_table_replace(self->last_add, g_strdup("torrents-size"), g_strdup_printf("%" G_GSIZE_FORMAT, g_bytes_get_size(file)));
	}
	if (category != NULL)
		g_hash_table_replace(self->last_add, g_strdup("category"), g_strdup(category));
	if (paused != NULL)
		g_hash_table_replace(self->last_add, g_strdup("paused"), g_strdup(paused));
	if (stopped != NULL)
		g_hash_table_replace(self->last_add, g_strdup("stopped"), g_strdup(stopped));

	if (self->flags[AXIS_TEST_QBIT_ADD_FAILS]) {
		answer_text(answer, 200, "text/plain; charset=UTF-8", "Fails.");
		return;
	}
	if ((urls == NULL) == (file == NULL)) {
		answer_text(answer, 400, "text/plain", "Bad request");
		return;
	}
	if (urls != NULL) {
		if (!axis_download_magnet_parse(urls, &hash, NULL)) {
			answer_text(answer, 200, "text/plain; charset=UTF-8", "Fails.");
			return;
		}
		name = g_strdup("magnet");
	} else {
		gsize length = 0;
		const guchar *data = g_bytes_get_data(file, &length);

		if (!axis_download_torrent_read(data, length, &hash, &name, NULL)) {
			answer_text(answer, 200, "text/plain; charset=UTF-8", "Fails.");
			return;
		}
	}
	existing = find_torrent(self, hash);
	if (existing != NULL) {
		answer_text(answer, 200, "text/plain; charset=UTF-8", self->flags[AXIS_TEST_QBIT_ADD_MERGES] ? "Ok." : "Fails.");
		return;
	}
	add_torrent(self, hash, name, category, urls != NULL);
	if (self->flags[AXIS_TEST_QBIT_ADD_HANGS]) {
		answer->hang = TRUE;
		return;
	}
	answer_text(answer, 200, "text/plain; charset=UTF-8", "Ok.");
}

static void
dispatch_api(
	AxisTestQbit              *self,
	const AxisTestPlexRequest *request,
	const gchar               *api,
	AxisTestPlexAnswer        *answer
){
	Raw *raw = g_hash_table_lookup(self->raw, api);
	g_autoptr(GHashTable) query = NULL;
	g_autofree gchar *text = NULL;
	gboolean post = strcmp(request->method, "POST") == 0;

	if (strcmp(api, "/auth/login") == 0) {
		if (!post) {
			self->bad_requests++;
			answer_text(answer, 405, "text/plain", "Method Not Allowed");
			return;
		}
		if (raw != NULL) {
			note(self, "login");
			answer_text(answer, raw->status, raw->type, raw->body);
			return;
		}
		do_login(self, request, answer);
		return;
	}
	/* A banned address gets nothing. */
	if (self->banned) {
		note(self, "banned:%s", api);
		answer_text(answer, 403, "text/plain", "Your IP address has been banned after too many failed authentication attempts.");
		return;
	}
	if (self->expire_before != NULL && strcmp(self->expire_before, api) == 0) {
		g_clear_pointer(&self->session, g_free);
		g_clear_pointer(&self->expire_before, g_free);
	}
	if (!has_session(self, request)) {
		note(self, "unauthorised:%s", api);
		answer_text(answer, 403, "text/plain", "Forbidden");
		return;
	}
	if (raw != NULL) {
		note(self, "raw:%s", api);
		answer_text(answer, raw->status, raw->type, raw->body);
		return;
	}
	if (strcmp(api, "/app/version") == 0) {
		note(self, "version");
		answer_text(answer, 200, "text/plain; charset=UTF-8", self->generation == AXIS_TEST_QBIT_V4 ? "v4.6.2" : "v5.0.1");
	} else if (strcmp(api, "/app/webapiVersion") == 0) {
		note(self, "webapi");
		answer_text(answer, 200, "text/plain; charset=UTF-8", self->generation == AXIS_TEST_QBIT_V4 ? "2.9.3" : "2.11.2");
	} else if (strcmp(api, "/app/defaultSavePath") == 0) {
		note(self, "default-path");
		answer_text(answer, 200, "text/plain; charset=UTF-8", self->default_path);
	} else if (strcmp(api, "/torrents/categories") == 0) {
		note(self, "categories");
		text = categories_json(self);
		answer_text(answer, 200, "application/json", text);
	} else if (strcmp(api, "/torrents/createCategory") == 0) {
		g_autoptr(GHashTable) form = parse_form(request->body);
		const gchar *name = g_hash_table_lookup(form, "category");

		note(self, "create-category:%s", name != NULL ? name : "");
		if (!post || name == NULL || name[0] == '\0') {
			answer_text(answer, 400, "text/plain", "Category name is empty");
			return;
		}
		if (self->flags[AXIS_TEST_QBIT_CATEGORY_409] || has_name(self->categories, name)) {
			answer_text(answer, 409, "text/plain", "Category creation failed");
			return;
		}
		g_ptr_array_add(self->categories, g_strdup(name));
		answer->status = 200;
	} else if (strcmp(api, "/torrents/add") == 0) {
		if (!post) {
			self->bad_requests++;
			answer_text(answer, 405, "text/plain", "Method Not Allowed");
			return;
		}
		do_add(self, request, answer);
	} else if (strcmp(api, "/torrents/info") == 0) {
		gchar *category = NULL;
		gchar **hashes = NULL;
		gchar **pairs = g_strsplit(request->query != NULL ? request->query : "", "&", -1);
		guint i;

		for (i = 0; pairs[i] != NULL; i++) {
			if (g_str_has_prefix(pairs[i], "category="))
				category = g_uri_unescape_string(pairs[i] + 9, NULL);
			else if (g_str_has_prefix(pairs[i], "hashes=")) {
				g_autofree gchar *list = g_uri_unescape_string(pairs[i] + 7, NULL);

				hashes = g_strsplit(list, "|", -1);
			}
		}
		g_strfreev(pairs);
		if (hashes != NULL)
			note(self, "info:hashes=%s", hashes[0] != NULL ? hashes[0] : "");
		else if (category != NULL)
			note(self, "info:category=%s", category);
		else
			note(self, "info");
		text = torrents_json(self, category != NULL ? category : "", category != NULL, hashes);
		answer_text(answer, 200, "application/json", text);
		g_free(category);
		g_strfreev(hashes);
	} else if (strcmp(api, "/torrents/delete") == 0) {
		g_autoptr(GHashTable) form = parse_form(request->body);
		const gchar *list = g_hash_table_lookup(form, "hashes");
		const gchar *files = g_hash_table_lookup(form, "deleteFiles");
		gchar **hashes = g_strsplit(list != NULL ? list : "", "|", -1);
		guint i;

		if (!post || list == NULL) {
			g_strfreev(hashes);
			self->bad_requests++;
			answer_text(answer, 400, "text/plain", "Bad request");
			return;
		}
		for (i = 0; hashes[i] != NULL; i++) {
			Torrent *torrent = find_torrent(self, hashes[i]);

			note(self, "delete:%s:%s", hashes[i], g_strcmp0(files, "true") == 0 ? "data" : "keep");
			if (torrent != NULL && !self->flags[AXIS_TEST_QBIT_DELETE_KEEPS])
				g_ptr_array_remove(self->torrents, torrent);
		}
		g_strfreev(hashes);
		answer->status = 200;
	} else {
		self->bad_requests++;
		note(self, "unknown:%s", api);
		answer_text(answer, 404, "text/plain", "Not Found");
	}
}

static void
qb_dispatch(
	AxisTestQbit              *self,
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer
){
	g_autofree gchar *root = g_strconcat(self->prefix, "/api/v2", NULL);
	const gchar *referer = axis_test_plex_request_header(request, "Referer");
	const gchar *origin = axis_test_plex_request_header(request, "Origin");

	if (self->mode == AXIS_TEST_QBIT_DOWN) {
		answer->hang = TRUE;
		return;
	}
	if (self->mode == AXIS_TEST_QBIT_REDIRECT) {
		answer->status = 307;
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Location");
		answer->headers[1] = g_strdup(self->redirect != NULL ? self->redirect : "/");
		return;
	}
	if (!g_str_has_prefix(request->path, root)) {
		self->bad_requests++;
		note(self, "wrong-path:%s", request->path);
		answer_text(answer, 404, "text/plain", "Not Found");
		return;
	}
	/* Its CSRF check: a request that says it comes from another place is refused. */
	if ((referer != NULL && strcmp(referer, self->base) != 0) || (origin != NULL && strcmp(origin, self->base) != 0) ||
	    (referer == NULL && origin == NULL)) {
		self->bad_requests++;
		note(self, "csrf:%s", request->path + strlen(root));
		answer_text(answer, 401, "text/plain", "Unauthorized");
		return;
	}
	dispatch_api(self, request, request->path + strlen(root), answer);
}

static void
qb_handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestQbit *self = user_data;

	g_mutex_lock(&self->lock);
	qb_dispatch(self, request, answer);
	g_mutex_unlock(&self->lock);
}

/* ── The API of the stand-in ────────────────────────────────────── */

AxisTestQbit *
axis_test_qbit_new(
	AxisTestQbitGeneration  generation,
	const gchar            *username,
	const gchar            *password
){
	AxisTestQbit *self = g_new0(AxisTestQbit, 1);

	g_mutex_init(&self->lock);
	self->generation = generation;
	self->username = g_strdup(username);
	self->password = g_strdup(password);
	self->prefix = g_strdup("");
	self->ban_after = 5;
	self->sessions = g_ptr_array_new_with_free_func(g_free);
	self->categories = g_ptr_array_new_with_free_func(g_free);
	self->torrents = g_ptr_array_new_with_free_func(torrent_free);
	self->calls = g_ptr_array_new_with_free_func(g_free);
	self->raw = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, raw_free);
	self->last_add = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->save_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->default_path = g_strdup("/downloads");
	g_ptr_array_add(self->categories, g_strdup("axis"));
	g_ptr_array_add(self->categories, g_strdup("manual"));
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	self->base = axis_test_plex_base(self->server);
	axis_test_plex_set_handler(self->server, qb_handle, self);
	return self;
}

void
axis_test_qbit_free(AxisTestQbit *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_ptr_array_unref(self->sessions);
	g_ptr_array_unref(self->categories);
	g_ptr_array_unref(self->torrents);
	g_ptr_array_unref(self->calls);
	g_hash_table_unref(self->raw);
	g_hash_table_unref(self->last_add);
	g_hash_table_unref(self->save_paths);
	g_free(self->default_path);
	g_free(self->username);
	g_free(self->password);
	g_free(self->prefix);
	g_free(self->base);
	g_free(self->session);
	g_free(self->expire_before);
	g_free(self->redirect);
	g_free(self->last_user);
	g_mutex_clear(&self->lock);
	g_free(self);
}

gchar *
axis_test_qbit_base(AxisTestQbit *self)
{
	return g_strdup(self->base);
}

AxisTestPlex *
axis_test_qbit_server(AxisTestQbit *self)
{
	return self->server;
}

#define LOCKED(self, statement) \
	G_STMT_START { g_mutex_lock(&(self)->lock); statement; g_mutex_unlock(&(self)->lock); } G_STMT_END

void
axis_test_qbit_set_prefix(
	AxisTestQbit *self,
	const gchar  *prefix
){
	LOCKED(self, g_free(self->prefix); self->prefix = g_strdup(prefix));
}

void
axis_test_qbit_set_mode(
	AxisTestQbit     *self,
	AxisTestQbitMode  mode
){
	LOCKED(self, self->mode = mode);
}

void
axis_test_qbit_set_redirect(
	AxisTestQbit *self,
	const gchar  *location
){
	LOCKED(self, g_free(self->redirect); self->redirect = g_strdup(location));
}

void
axis_test_qbit_set_flag(
	AxisTestQbit     *self,
	AxisTestQbitFlag  flag,
	gboolean          on
){
	LOCKED(self, self->flags[flag] = on);
}

void
axis_test_qbit_set_ban_after(
	AxisTestQbit *self,
	guint         failures
){
	LOCKED(self, self->ban_after = failures);
}

void
axis_test_qbit_set_banned(
	AxisTestQbit *self,
	gboolean      banned
){
	LOCKED(self, self->banned = banned; self->failed = 0);
}

void
axis_test_qbit_set_raw(
	AxisTestQbit *self,
	const gchar  *path,
	guint         status,
	const gchar  *content_type,
	const gchar  *body
){
	Raw *raw = g_new0(Raw, 1);

	raw->status = status;
	raw->type = g_strdup(content_type);
	raw->body = g_strdup(body);
	LOCKED(self, g_hash_table_insert(self->raw, g_strdup(path), raw));
}

void
axis_test_qbit_clear_raw(
	AxisTestQbit *self,
	const gchar  *path
){
	LOCKED(self, g_hash_table_remove(self->raw, path));
}

void
axis_test_qbit_expire_session(AxisTestQbit *self)
{
	LOCKED(self, g_clear_pointer(&self->session, g_free));
}

void
axis_test_qbit_expire_before(
	AxisTestQbit *self,
	const gchar  *path
){
	LOCKED(self, g_free(self->expire_before); self->expire_before = g_strdup(path));
}

void
axis_test_qbit_add_category(
	AxisTestQbit *self,
	const gchar  *name
){
	g_mutex_lock(&self->lock);
	if (!has_name(self->categories, name))
		g_ptr_array_add(self->categories, g_strdup(name));
	g_mutex_unlock(&self->lock);
}

gboolean
axis_test_qbit_has_category(
	AxisTestQbit *self,
	const gchar  *name
){
	gboolean has;

	g_mutex_lock(&self->lock);
	has = has_name(self->categories, name);
	g_mutex_unlock(&self->lock);
	return has;
}

void
axis_test_qbit_clear_categories(AxisTestQbit *self)
{
	LOCKED(self, g_ptr_array_set_size(self->categories, 0));
}

void
axis_test_qbit_set_category_path(
	AxisTestQbit *self,
	const gchar  *name,
	const gchar  *path
){
	g_mutex_lock(&self->lock);
	if (!has_name(self->categories, name))
		g_ptr_array_add(self->categories, g_strdup(name));
	g_hash_table_insert(self->save_paths, g_strdup(name), g_strdup(path != NULL ? path : ""));
	g_mutex_unlock(&self->lock);
}

void
axis_test_qbit_set_default_path(
	AxisTestQbit *self,
	const gchar  *path
){
	LOCKED(self, g_free(self->default_path); self->default_path = g_strdup(path));
}

void
axis_test_qbit_put(
	AxisTestQbit *self,
	const gchar  *hash,
	const gchar  *name,
	const gchar  *category,
	const gchar  *state,
	gint64        size,
	gdouble       progress,
	const gchar  *save_path,
	const gchar  *content_path
){
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	if (torrent == NULL) {
		torrent = g_new0(Torrent, 1);
		torrent->hash = g_strdup(hash);
		g_ptr_array_add(self->torrents, torrent);
	}
	g_free(torrent->name);
	torrent->name = g_strdup(name);
	g_free(torrent->category);
	torrent->category = g_strdup(category);
	g_free(torrent->state);
	torrent->state = g_strdup(state);
	torrent->size = size;
	torrent->progress = progress;
	g_free(torrent->save_path);
	torrent->save_path = g_strdup(save_path);
	g_free(torrent->content_path);
	torrent->content_path = g_strdup(content_path);
	g_mutex_unlock(&self->lock);
}

void
axis_test_qbit_set_state(
	AxisTestQbit *self,
	const gchar  *hash,
	const gchar  *state,
	gdouble       progress
){
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	g_assert_nonnull(torrent);
	g_free(torrent->state);
	torrent->state = g_strdup(state);
	torrent->progress = progress;
	g_mutex_unlock(&self->lock);
}

void
axis_test_qbit_drop(
	AxisTestQbit *self,
	const gchar  *hash
){
	g_mutex_lock(&self->lock);
	g_ptr_array_remove(self->torrents, find_torrent(self, hash));
	g_mutex_unlock(&self->lock);
}

gboolean
axis_test_qbit_has(
	AxisTestQbit *self,
	const gchar  *hash
){
	gboolean has;

	g_mutex_lock(&self->lock);
	has = find_torrent(self, hash) != NULL;
	g_mutex_unlock(&self->lock);
	return has;
}

gchar *
axis_test_qbit_category_of(
	AxisTestQbit *self,
	const gchar  *hash
){
	gchar *category = NULL;
	Torrent *torrent;

	g_mutex_lock(&self->lock);
	torrent = find_torrent(self, hash);
	if (torrent != NULL)
		category = g_strdup(torrent->category);
	g_mutex_unlock(&self->lock);
	return category;
}

guint
axis_test_qbit_n_torrents(AxisTestQbit *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->torrents->len;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_qbit_count(
	AxisTestQbit *self,
	const gchar  *prefix
){
	guint n = 0;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->calls->len; i++) {
		if (g_str_has_prefix(g_ptr_array_index(self->calls, i), prefix))
			n++;
	}
	g_mutex_unlock(&self->lock);
	return n;
}

gchar *
axis_test_qbit_calls(AxisTestQbit *self)
{
	GString *out = g_string_new(NULL);
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->calls->len; i++)
		g_string_append_printf(out, "%s%s", i > 0 ? " " : "", (const gchar *)g_ptr_array_index(self->calls, i));
	g_mutex_unlock(&self->lock);
	return g_string_free(out, FALSE);
}

guint
axis_test_qbit_bad_requests(AxisTestQbit *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->bad_requests;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_qbit_banned_logins(AxisTestQbit *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->banned_logins;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_qbit_failed_logins(AxisTestQbit *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->failed;
	g_mutex_unlock(&self->lock);
	return n;
}

gchar *
axis_test_qbit_last_add(
	AxisTestQbit *self,
	const gchar  *field
){
	gchar *value;

	g_mutex_lock(&self->lock);
	value = g_strdup(g_hash_table_lookup(self->last_add, field));
	g_mutex_unlock(&self->lock);
	return value;
}

gchar *
axis_test_qbit_last_login_user(AxisTestQbit *self)
{
	gchar *value;

	g_mutex_lock(&self->lock);
	value = g_strdup(self->last_user);
	g_mutex_unlock(&self->lock);
	return value;
}

GPtrArray *
axis_test_qbit_sessions(AxisTestQbit *self)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->sessions->len; i++)
		g_ptr_array_add(out, g_strdup(g_ptr_array_index(self->sessions, i)));
	g_mutex_unlock(&self->lock);
	return out;
}

gchar *
axis_test_qbit_cookie_name(AxisTestQbit *self)
{
	return g_strdup(cookie_name(self));
}
