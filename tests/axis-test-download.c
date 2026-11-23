/*
 * axis-test-download.c - test support: the "fixture" download client kind
 * and the fake download client
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-download.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "download/axis-download-bencode.h"

/* ── The kind ───────────────────────────────────────────────────── */

static GBytes *
json_body(JsonBuilder *builder)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = json_builder_get_root(builder);
	gsize length = 0;
	gchar *text;

	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, &length);
	return g_bytes_new_take(text, length);
}

static JsonObject *
parse_object(
	GBytes  *body,
	GError **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	gsize length = 0;
	const gchar *data = g_bytes_get_data(body, &length);
	JsonNode *root;

	if (!json_parser_load_from_data(parser, data != NULL ? data : "", (gssize)length, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"the client's answer could not be read");
		return NULL;
	}
	return json_object_ref(json_node_get_object(root));
}

static gboolean
login(
	AxisDownloadConn  *conn,
	GError           **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) answer = NULL;
	const gchar *secret = axis_download_conn_get_secret(conn);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "password");
	json_builder_add_string_value(builder, secret != NULL ? secret : "");
	json_builder_end_object(builder);
	body = json_body(builder);
	return axis_download_conn_post(conn, "/api/login", "application/json", body, NULL, &answer, error);
}

/* One call, logging in first when there is no session, and once more when
 * the session has expired. */
static JsonObject *
call(
	AxisDownloadConn  *conn,
	const gchar       *path,
	JsonBuilder       *builder,
	GError           **error
){
	g_autoptr(GBytes) body = json_body(builder);
	guint attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		g_autoptr(GBytes) answer = NULL;
		g_autoptr(GError) local = NULL;

		if (!axis_download_conn_has_session(conn) && !login(conn, error))
			return NULL;
		if (axis_download_conn_post(conn, path, "application/json", body, NULL, &answer, &local))
			return parse_object(answer, error);
		if (attempt == 0 && g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH)) {
			axis_download_conn_forget_session(conn);
			continue;
		}
		/* The client said no to the request itself. */
		if (axis_download_conn_get_last_status(conn) == 400 || axis_download_conn_get_last_status(conn) == 404)
			g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED, "the client refused it (%u)",
				axis_download_conn_get_last_status(conn));
		else
			g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH, "the session was refused twice");
	return NULL;
}

static gboolean
fixture_test(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	AxisDownloadTestResult      *result,
	GError                     **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) answer = NULL;
	JsonArray *categories;
	guint i;

	json_builder_begin_object(builder);
	json_builder_end_object(builder);
	answer = call(conn, "/api/info", builder, error);
	if (answer == NULL)
		return FALSE;
	if (json_object_has_member(answer, "version"))
		result->version = g_strdup(json_object_get_string_member(answer, "version"));
	categories = json_object_has_member(answer, "categories") ? json_object_get_array_member(answer, "categories") :
		NULL;
	result->category_exists = 0;
	for (i = 0; categories != NULL && i < json_array_get_length(categories); i++) {
		if (g_strcmp0(json_array_get_string_element(categories, i), category) == 0)
			result->category_exists = 1;
	}
	return TRUE;
}

static gboolean
fixture_add(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const AxisDownloadRelease   *release,
	const gchar                 *category,
	AxisDownloadPriority         priority,
	gchar                      **client_ref,
	GError                     **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) answer = NULL;
	const gchar *id;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, release->name);
	json_builder_set_member_name(builder, "category");
	json_builder_add_string_value(builder, category);
	json_builder_set_member_name(builder, "priority");
	json_builder_add_string_value(builder, axis_download_priority_to_string(priority));
	if (release->magnet != NULL) {
		json_builder_set_member_name(builder, "magnet");
		json_builder_add_string_value(builder, release->magnet);
	} else {
		g_autofree gchar *encoded = g_base64_encode(g_bytes_get_data(release->file, NULL),
			g_bytes_get_size(release->file));

		json_builder_set_member_name(builder, "file");
		json_builder_add_string_value(builder, encoded);
		json_builder_set_member_name(builder, "file_name");
		json_builder_add_string_value(builder, release->file_name);
	}
	json_builder_end_object(builder);
	answer = call(conn, "/api/add", builder, error);
	if (answer == NULL)
		return FALSE;
	id = json_object_has_member(answer, "id") ? json_object_get_string_member(answer, "id") : NULL;
	*client_ref = g_strdup(id);
	return TRUE;
}

static AxisDownloadItemState
item_state(const gchar *text)
{
	if (g_strcmp0(text, "downloading") == 0)
		return AXIS_DOWNLOAD_ITEM_DOWNLOADING;
	if (g_strcmp0(text, "paused") == 0)
		return AXIS_DOWNLOAD_ITEM_PAUSED;
	if (g_strcmp0(text, "completed") == 0)
		return AXIS_DOWNLOAD_ITEM_COMPLETED;
	if (g_strcmp0(text, "failed") == 0)
		return AXIS_DOWNLOAD_ITEM_FAILED;
	return AXIS_DOWNLOAD_ITEM_QUEUED;
}

static gchar *
member_text(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return g_strdup(json_node_get_string(node));
}

static gint64
member_int(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return -1;
	return json_node_get_int(node);
}

static gboolean
fixture_list(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	GPtrArray                  **items,
	GError                     **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) answer = NULL;
	JsonArray *list;
	guint i;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "category");
	/* None: every item (the fake's list holds them all anyway). */
	if (category != NULL)
		json_builder_add_string_value(builder, category);
	else
		json_builder_add_null_value(builder);
	json_builder_end_object(builder);
	answer = call(conn, "/api/list", builder, error);
	if (answer == NULL)
		return FALSE;
	list = json_object_has_member(answer, "items") ? json_object_get_array_member(answer, "items") : NULL;
	*items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
		JsonObject *object = json_array_get_object_element(list, i);
		AxisDownloadItem *item;
		g_autofree gchar *state = NULL;

		if (object == NULL)
			continue;
		item = axis_download_item_new();
		item->client_ref = member_text(object, "id");
		item->name = member_text(object, "name");
		item->category = member_text(object, "category");
		item->info_hash = member_text(object, "hash");
		state = member_text(object, "state");
		item->state = item_state(state);
		item->size_bytes = member_int(object, "size");
		item->done_bytes = member_int(object, "done");
		item->rate_bps = member_int(object, "rate");
		item->eta_s = member_int(object, "eta");
		item->path = member_text(object, "path");
		/* The fake's path is where its files are, whatever its state. */
		item->place = member_text(object, "path");
		item->message = member_text(object, "message");
		/* An entry the kind reads by its id only (as a real kind does one
		 * without a readable name). */
		item->unreadable = member_text(object, "unreadable");
		g_ptr_array_add(*items, item);
	}
	return TRUE;
}

/* The torrent kind's look-up in every category: the fake's list holds them all. */
static gboolean
fixture_find(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	AxisDownloadItem           **item,
	GError                     **error
){
	g_autoptr(GPtrArray) items = NULL;
	guint i;

	*item = NULL;
	if (!fixture_list(self, conn, "", &items, error))
		return FALSE;
	for (i = 0; i < items->len; i++) {
		AxisDownloadItem *one = g_ptr_array_index(items, i);

		if (g_strcmp0(one->client_ref, client_ref) == 0) {
			*item = g_ptr_array_steal_index(items, i);
			if ((*item)->category == NULL)
				(*item)->category = g_strdup("");
			return TRUE;
		}
	}
	return TRUE;
}

static gboolean
fixture_remove(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *client_ref,
	gboolean                     delete_data,
	GError                     **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) answer = NULL;

	/* As the real torrent kinds (both fixture kinds stand in for one in the
	 * import tests): only an item still in Axis's category is removed. */
	{
		g_autoptr(AxisDownloadItem) now = NULL;

		if (!fixture_find(self, conn, client_ref, &now, error))
			return FALSE;
		if (now != NULL && g_ascii_strcasecmp(now->category, axis_download_conn_get_category(conn) != NULL ?
		        axis_download_conn_get_category(conn) : "") != 0) {
			g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS, "the fixture client has the torrent "
				"in the category \"%s\" now, not in Axis's", now->category);
			return FALSE;
		}
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, client_ref);
	json_builder_set_member_name(builder, "delete_data");
	json_builder_add_boolean_value(builder, delete_data);
	json_builder_end_object(builder);
	{
		GError *local = NULL;

		answer = call(conn, "/api/remove", builder, &local);
		/* 410: the fake has no such item (when it is told to say so). */
		if (answer == NULL && axis_download_conn_get_last_status(conn) == 410) {
			g_error_free(local);
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE,
				"the fixture client no longer has it");
			return FALSE;
		}
		if (answer == NULL)
			g_propagate_error(error, local);
	}
	return answer != NULL;
}

static gboolean
fixture_category_folder(
	const AxisDownloadKindInfo  *self,
	AxisDownloadConn            *conn,
	const gchar                 *category,
	gchar                      **folder,
	GError                     **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) answer = NULL;

	*folder = NULL;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "category");
	json_builder_add_string_value(builder, category);
	json_builder_end_object(builder);
	answer = call(conn, "/api/folder", builder, error);
	if (answer == NULL)
		return FALSE;
	*folder = member_text(answer, "dir");
	if (*folder == NULL) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED,
			"the category has no folder of its own at the fixture client");
		return FALSE;
	}
	return TRUE;
}

static const AxisDownloadKindInfo torrent_kind = {
	"fixture", "Fixture", AXIS_DOWNLOAD_TORRENT, "Password", "Category",
	"The fixture client's own address (tests only).",
	fixture_test, fixture_add, fixture_list, fixture_remove, NULL, NULL, fixture_category_folder, fixture_find, FALSE
};

static const AxisDownloadKindInfo usenet_kind = {
	"fixturenzb", "Fixture (Usenet)", AXIS_DOWNLOAD_USENET, "Password", "Category",
	"The fixture client's own address (tests only).",
	fixture_test, fixture_add, fixture_list, fixture_remove, NULL, NULL, fixture_category_folder, fixture_find, FALSE
};

const AxisDownloadKindInfo *
axis_test_download_kind(AxisDownloadProtocol protocol)
{
	return protocol == AXIS_DOWNLOAD_TORRENT ? &torrent_kind : &usenet_kind;
}

/* ── The fake client ────────────────────────────────────────────── */

typedef struct
{
	gchar  *id;
	gchar  *name;
	gchar  *category;
	gchar  *hash;
	gchar  *state;
	gint64  size;
	gint64  done;
	gchar  *path;
	gchar  *message;
	gchar  *unreadable;	/* why the fixture kind cannot read it but by its id */
} Item;

struct _AxisTestDownloadFake
{
	AxisTestPlex         *server;
	GMutex                lock;
	gchar                *password;
	gchar                *session;	/* (lock) */
	GPtrArray            *items;	/* (lock) Item */
	AxisTestDownloadMode  mode;	/* (lock) */
	gchar                *redirect;	/* (lock) */
	GPtrArray            *calls;	/* (lock) "op id" */
	GHashTable           *last_add;	/* (lock) field -> value */
	gchar                *last_id;	/* (lock) */
	guint                 next_id;	/* (lock) */
	gchar                *data_dir;	/* (lock) where the client's downloads are on disk */
	gboolean              keeps_completed;	/* (lock) as SABnzbd: delete_data spares a completed item */
	gchar                *category_dir;	/* (lock) what /api/folder answers, or NULL (404) */
	gboolean              says_gone;	/* (lock) a removal of an item it lacks is answered 410 */
};

static void
item_free(gpointer data)
{
	Item *item = data;

	g_free(item->id);
	g_free(item->name);
	g_free(item->category);
	g_free(item->hash);
	g_free(item->state);
	g_free(item->path);
	g_free(item->message);
	g_free(item->unreadable);
	g_free(item);
}

/* A file or a folder, with what is in it (never through a link). */
static void
remove_tree(const gchar *path)
{
	GDir *dir = NULL;
	const gchar *name;

	if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
		dir = g_dir_open(path, 0, NULL);
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);

		remove_tree(child);
	}
	if (dir != NULL)
		g_dir_close(dir);
	g_remove(path);
}

static Item *
find_item(
	AxisTestDownloadFake *self,
	const gchar          *id
){
	guint i;

	for (i = 0; id != NULL && i < self->items->len; i++) {
		Item *item = g_ptr_array_index(self->items, i);

		if (strcmp(item->id, id) == 0)
			return item;
	}
	return NULL;
}

static void
answer_json(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *text
){
	answer->status = status;
	answer->content_type = g_strdup("application/json");
	answer->body = g_bytes_new(text, strlen(text));
}

static gboolean
has_session(
	AxisTestDownloadFake      *self,
	const AxisTestPlexRequest *request
){
	const gchar *cookie = axis_test_plex_request_header(request, "Cookie");
	g_autofree gchar *wanted = NULL;

	if (cookie == NULL || self->session == NULL)
		return FALSE;
	wanted = g_strdup_printf("fx_session=%s", self->session);
	return strstr(cookie, wanted) != NULL;
}

static gchar *
items_json(AxisTestDownloadFake *self)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);
	for (i = 0; i < self->items->len; i++) {
		const Item *item = g_ptr_array_index(self->items, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, item->id);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, item->name);
		json_builder_set_member_name(builder, "category");
		json_builder_add_string_value(builder, item->category);
		if (item->hash != NULL) {
			json_builder_set_member_name(builder, "hash");
			json_builder_add_string_value(builder, item->hash);
		}
		json_builder_set_member_name(builder, "state");
		json_builder_add_string_value(builder, item->state);
		json_builder_set_member_name(builder, "size");
		json_builder_add_int_value(builder, item->size);
		json_builder_set_member_name(builder, "done");
		json_builder_add_int_value(builder, item->done);
		json_builder_set_member_name(builder, "rate");
		json_builder_add_int_value(builder, g_strcmp0(item->state, "downloading") == 0 ? 2500000 : 0);
		json_builder_set_member_name(builder, "eta");
		json_builder_add_int_value(builder, g_strcmp0(item->state, "downloading") == 0 && item->size > item->done ?
			(item->size - item->done) / 2500000 + 1 : 0);
		if (item->path != NULL) {
			json_builder_set_member_name(builder, "path");
			json_builder_add_string_value(builder, item->path);
		}
		if (item->message != NULL) {
			json_builder_set_member_name(builder, "message");
			json_builder_add_string_value(builder, item->message);
		}
		if (item->unreadable != NULL) {
			json_builder_set_member_name(builder, "unreadable");
			json_builder_add_string_value(builder, item->unreadable);
		}
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

static void
note_call(
	AxisTestDownloadFake *self,
	const gchar          *op,
	const gchar          *id
){
	g_ptr_array_add(self->calls, g_strdup_printf("%s %s", op, id != NULL ? id : ""));
}

static gchar *
body_text(
	JsonObject  *object,
	const gchar *name
){
	return object != NULL ? member_text(object, name) : NULL;
}

/* An added release: its hash from the magnet link or the torrent. */
static gchar *
release_hash(JsonObject *object)
{
	g_autofree gchar *magnet = body_text(object, "magnet");
	g_autofree gchar *file = body_text(object, "file");
	gchar *hash = NULL;

	if (magnet != NULL) {
		axis_download_magnet_parse(magnet, &hash, NULL);
	} else if (file != NULL) {
		gsize length = 0;
		g_autofree guchar *data = g_base64_decode(file, &length);

		axis_download_torrent_read(data, length, &hash, NULL, NULL);
	}
	return hash;
}

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestDownloadFake *self = user_data;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *object = NULL;
	gsize length = 0;
	const gchar *data = g_bytes_get_data(request->body, &length);
	const gchar *op = g_str_has_prefix(request->path, "/api/") ? request->path + 5 : request->path;

	if (length > 0 && json_parser_load_from_data(parser, data, (gssize)length, NULL) &&
	    json_parser_get_root(parser) != NULL && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		object = json_node_get_object(json_parser_get_root(parser));

	g_mutex_lock(&self->lock);
	if (self->mode == AXIS_TEST_DOWNLOAD_DOWN) {
		answer->hang = TRUE;
		goto out;
	}
	if (self->mode == AXIS_TEST_DOWNLOAD_REDIRECT) {
		answer->status = 307;
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Location");
		answer->headers[1] = g_strdup(self->redirect != NULL ? self->redirect : "/");
		goto out;
	}
	if (strcmp(request->method, "POST") != 0) {
		answer_json(answer, 405, "{}");
		goto out;
	}
	if (strcmp(op, "login") == 0) {
		g_autofree gchar *password = body_text(object, "password");

		note_call(self, "login", NULL);
		if (g_strcmp0(password, self->password) != 0) {
			answer_json(answer, 401, "{\"error\":\"wrong password\"}");
			goto out;
		}
		g_free(self->session);
		self->session = g_strdup_printf("%08x%08x%08x", g_random_int(), g_random_int(), g_random_int());
		answer_json(answer, 200, "{\"ok\":true}");
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Set-Cookie");
		answer->headers[1] = g_strdup_printf("fx_session=%s; Path=/; HttpOnly", self->session);
		goto out;
	}
	if (!has_session(self, request)) {
		note_call(self, "unauthorised", NULL);
		answer_json(answer, 401, "{\"error\":\"log in first\"}");
		goto out;
	}
	if (strcmp(op, "info") == 0) {
		note_call(self, "info", NULL);
		answer_json(answer, 200, "{\"version\":\"Fixture 1.0\",\"categories\":[\"axis\",\"manual\"]}");
	} else if (strcmp(op, "list") == 0) {
		g_autofree gchar *text = items_json(self);

		note_call(self, "list", NULL);
		if (self->mode == AXIS_TEST_DOWNLOAD_LIST_FAILS)
			answer_json(answer, 500, "{\"error\":\"the list failed\"}");
		else
			answer_json(answer, 200, text);
	} else if (strcmp(op, "add") == 0) {
		Item *item;
		g_autofree gchar *reply = NULL;
		static const gchar *const fields[] = { "name", "category", "priority", "magnet", "file_name", NULL };
		guint i;

		note_call(self, "add", NULL);
		if (self->mode == AXIS_TEST_DOWNLOAD_ADD_REFUSED) {
			answer_json(answer, 400, "{\"error\":\"not a release\"}");
			goto out;
		}
		g_hash_table_remove_all(self->last_add);
		for (i = 0; fields[i] != NULL; i++) {
			gchar *value = body_text(object, fields[i]);

			if (value != NULL)
				g_hash_table_insert(self->last_add, g_strdup(fields[i]), value);
		}
		item = g_new0(Item, 1);
		item->id = g_strdup_printf("fx-%u", ++self->next_id);
		item->name = body_text(object, "name");
		item->category = body_text(object, "category");
		item->hash = release_hash(object);
		item->state = g_strdup("queued");
		item->size = 4000000000;
		item->done = 0;
		g_ptr_array_add(self->items, item);
		g_free(self->last_id);
		self->last_id = g_strdup(item->id);
		if (self->mode == AXIS_TEST_DOWNLOAD_ADD_HANGS) {
			answer->hang = TRUE;
			goto out;
		}
		if (self->mode == AXIS_TEST_DOWNLOAD_ADD_FAILS) {
			answer_json(answer, 500, "{\"error\":\"internal\"}");
			goto out;
		}
		reply = g_strdup_printf("{\"id\":\"%s\"}", item->id);
		answer_json(answer, 200, reply);
	} else if (strcmp(op, "remove") == 0) {
		g_autofree gchar *id = body_text(object, "id");
		Item *item = find_item(self, id);

		note_call(self, "remove", id);
		if (item == NULL) {
			answer_json(answer, self->says_gone ? 410 : 404, "{\"error\":\"no such download\"}");
			goto out;
		}
		if (object != NULL && json_object_get_boolean_member_with_default(object, "delete_data", FALSE)) {
			note_call(self, "remove-data", id);
			/* A client that deletes data deletes its own names of it; one
			 * that behaves as SABnzbd does keeps a completed job's files
			 * and deletes a failed one's. */
			if (self->keeps_completed && g_strcmp0(item->state, "completed") == 0)
				note_call(self, "remove-kept-files", id);
			else if (self->data_dir != NULL && item->name != NULL && item->name[0] != '\0' &&
			    strchr(item->name, '/') == NULL && strcmp(item->name, "..") != 0) {
				g_autofree gchar *path = g_build_filename(self->data_dir, item->name, NULL);

				remove_tree(path);
			}
		}
		g_ptr_array_remove(self->items, item);
		answer_json(answer, 200, "{}");
	} else if (strcmp(op, "folder") == 0) {
		g_autofree gchar *category = body_text(object, "category");

		note_call(self, "folder", category);
		if (self->category_dir == NULL) {
			answer_json(answer, 404, "{\"error\":\"no folder for that category\"}");
		} else {
			g_autoptr(JsonBuilder) builder = json_builder_new();
			g_autoptr(JsonGenerator) generator = json_generator_new();
			g_autoptr(JsonNode) root = NULL;
			g_autofree gchar *text = NULL;

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "dir");
			json_builder_add_string_value(builder, self->category_dir);
			json_builder_end_object(builder);
			root = json_builder_get_root(builder);
			json_generator_set_root(generator, root);
			text = json_generator_to_data(generator, NULL);
			answer_json(answer, 200, text);
		}
	} else {
		answer_json(answer, 404, "{}");
	}
out:
	g_mutex_unlock(&self->lock);
}

AxisTestDownloadFake *
axis_test_download_fake_new(const gchar *password)
{
	AxisTestDownloadFake *self = g_new0(AxisTestDownloadFake, 1);

	g_mutex_init(&self->lock);
	self->password = g_strdup(password);
	self->items = g_ptr_array_new_with_free_func(item_free);
	self->calls = g_ptr_array_new_with_free_func(g_free);
	self->last_add = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	axis_test_plex_set_handler(self->server, handle, self);
	return self;
}

void
axis_test_download_fake_free(AxisTestDownloadFake *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_ptr_array_unref(self->items);
	g_ptr_array_unref(self->calls);
	g_hash_table_unref(self->last_add);
	g_free(self->password);
	g_free(self->data_dir);
	g_free(self->category_dir);
	g_free(self->session);
	g_free(self->redirect);
	g_free(self->last_id);
	g_mutex_clear(&self->lock);
	g_free(self);
}

gchar *
axis_test_download_fake_base(AxisTestDownloadFake *self)
{
	return axis_test_plex_base(self->server);
}

AxisTestPlex *
axis_test_download_fake_server(AxisTestDownloadFake *self)
{
	return self->server;
}

void
axis_test_download_fake_set_mode(
	AxisTestDownloadFake *self,
	AxisTestDownloadMode  mode
){
	g_mutex_lock(&self->lock);
	self->mode = mode;
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_set_redirect(
	AxisTestDownloadFake *self,
	const gchar          *location
){
	g_mutex_lock(&self->lock);
	g_free(self->redirect);
	self->redirect = g_strdup(location);
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_set_data_dir(
	AxisTestDownloadFake *self,
	const gchar          *dir
){
	g_mutex_lock(&self->lock);
	g_free(self->data_dir);
	self->data_dir = g_strdup(dir);
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_keep_completed(
	AxisTestDownloadFake *self,
	gboolean              keep
){
	g_mutex_lock(&self->lock);
	self->keeps_completed = keep;
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_set_category_dir(
	AxisTestDownloadFake *self,
	const gchar          *dir
){
	g_mutex_lock(&self->lock);
	g_free(self->category_dir);
	self->category_dir = g_strdup(dir);
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_set_unreadable(
	AxisTestDownloadFake *self,
	const gchar          *id,
	const gchar          *why
){
	Item *item;

	g_mutex_lock(&self->lock);
	item = find_item(self, id);
	if (item != NULL) {
		g_free(item->unreadable);
		item->unreadable = g_strdup(why);
	}
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_say_gone(
	AxisTestDownloadFake *self,
	gboolean              gone
){
	g_mutex_lock(&self->lock);
	self->says_gone = gone;
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_expire_session(AxisTestDownloadFake *self)
{
	g_mutex_lock(&self->lock);
	g_clear_pointer(&self->session, g_free);
	g_mutex_unlock(&self->lock);
}

gchar *
axis_test_download_fake_session(AxisTestDownloadFake *self)
{
	gchar *session;

	g_mutex_lock(&self->lock);
	session = g_strdup(self->session);
	g_mutex_unlock(&self->lock);
	return session;
}

void
axis_test_download_fake_put(
	AxisTestDownloadFake *self,
	const gchar          *id,
	const gchar          *name,
	const gchar          *category,
	const gchar          *hash,
	const gchar          *state,
	gint64                size,
	gint64                done,
	const gchar          *path,
	const gchar          *message
){
	Item *item;

	g_mutex_lock(&self->lock);
	item = find_item(self, id);
	if (item == NULL) {
		item = g_new0(Item, 1);
		item->id = g_strdup(id);
		g_ptr_array_add(self->items, item);
	}
	g_free(item->name);
	item->name = g_strdup(name);
	g_free(item->category);
	item->category = g_strdup(category);
	g_free(item->hash);
	item->hash = g_strdup(hash);
	g_free(item->state);
	item->state = g_strdup(state);
	item->size = size;
	item->done = done;
	g_free(item->path);
	item->path = g_strdup(path);
	g_free(item->message);
	item->message = g_strdup(message);
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_set(
	AxisTestDownloadFake *self,
	const gchar          *id,
	const gchar          *state,
	gint64                done,
	const gchar          *path,
	const gchar          *message
){
	Item *item;

	g_mutex_lock(&self->lock);
	item = find_item(self, id);
	g_assert_nonnull(item);
	g_free(item->state);
	item->state = g_strdup(state);
	item->done = done;
	g_free(item->path);
	item->path = g_strdup(path);
	g_free(item->message);
	item->message = g_strdup(message);
	g_mutex_unlock(&self->lock);
}

void
axis_test_download_fake_drop(
	AxisTestDownloadFake *self,
	const gchar          *id
){
	Item *item;

	g_mutex_lock(&self->lock);
	item = find_item(self, id);
	if (item != NULL)
		g_ptr_array_remove(self->items, item);
	g_mutex_unlock(&self->lock);
}

gboolean
axis_test_download_fake_has(
	AxisTestDownloadFake *self,
	const gchar          *id
){
	gboolean has;

	g_mutex_lock(&self->lock);
	has = find_item(self, id) != NULL;
	g_mutex_unlock(&self->lock);
	return has;
}

guint
axis_test_download_fake_n_items(AxisTestDownloadFake *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->items->len;
	g_mutex_unlock(&self->lock);
	return n;
}

gchar *
axis_test_download_fake_last_id(AxisTestDownloadFake *self)
{
	gchar *id;

	g_mutex_lock(&self->lock);
	id = g_strdup(self->last_id);
	g_mutex_unlock(&self->lock);
	return id;
}

guint
axis_test_download_fake_count(
	AxisTestDownloadFake *self,
	const gchar          *op,
	const gchar          *id
){
	g_autofree gchar *exact = g_strdup_printf("%s %s", op, id != NULL ? id : "");
	g_autofree gchar *prefix = g_strdup_printf("%s ", op);
	guint n = 0;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->calls->len; i++) {
		const gchar *one = g_ptr_array_index(self->calls, i);

		if (id != NULL ? strcmp(one, exact) == 0 : g_str_has_prefix(one, prefix))
			n++;
	}
	g_mutex_unlock(&self->lock);
	return n;
}

gchar *
axis_test_download_fake_last_add_field(
	AxisTestDownloadFake *self,
	const gchar          *field
){
	gchar *value;

	g_mutex_lock(&self->lock);
	value = g_strdup(g_hash_table_lookup(self->last_add, field));
	g_mutex_unlock(&self->lock);
	return value;
}

GBytes *
axis_test_download_torrent(
	const gchar  *name,
	gchar       **hash
){
	static const gchar announce[] = "http://tracker.test/announce";
	GString *out = g_string_new(NULL);
	gchar pieces[20];
	gchar *data;
	gsize length;

	g_string_append_printf(out, "d8:announce%" G_GSIZE_FORMAT ":%s4:info", strlen(announce), announce);
	memset(pieces, 'p', sizeof(pieces));
	g_string_append_printf(out, "d6:lengthi1000e4:name%" G_GSIZE_FORMAT ":%s12:piece lengthi16384e6:pieces20:",
		strlen(name), name);
	g_string_append_len(out, pieces, 20);
	g_string_append(out, "ee");
	length = out->len;
	data = g_string_free(out, FALSE);
	if (hash != NULL)
		g_assert_true(axis_download_torrent_read((const guint8 *)data, length, hash, NULL, NULL));
	return g_bytes_new_take(data, length);
}
