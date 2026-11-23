/*
 * axis-test-prowlarr.c - test support: a stand-in for Prowlarr on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-prowlarr.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "axis-test-download.h"

#define FIXTURES "tests/fixtures/prowlarr/"

/* The oversized file's size: above the layer's limit (32 MiB). */
#define HUGE_BYTES (34 * 1024 * 1024)

/* What a recorded release's download address answers. */
typedef struct
{
	gboolean  torrent;
	gboolean  has_file;	/* it had a downloadUrl: the file is answered */
	gchar    *name;
	gchar    *magnet;	/* a link address's redirect: its magnet link */
} Recorded;

struct _AxisTestProwlarr
{
	AxisTestPlex         *server;
	GMutex                lock;
	gchar                *key;		/* (lock) */
	gchar                *prefix;
	gboolean              recorded;	/* the recordings, not the constructed answers */
	GHashTable           *releases;	/* "indexerId|file" -> Recorded *, recorded only */
	GArray               *redirecting;	/* gint64: indexers whose downloads redirect */
	gchar                *elsewhere;	/* (lock) */
	gchar                *site;		/* (lock) the indexer's site */
	gchar                *indexers;		/* (lock) a list of indexers a test set */
	gboolean              wrong_magnets;	/* (lock) */
	AxisTestProwlarrMode  mode;		/* (lock) */
	guint                 delay_ms;		/* (lock) */
	GHashTable           *searches;		/* (lock) type (or "") -> body */
	GPtrArray            *queries;		/* (lock) gchar * of every search */
	guint                 n_requests;	/* (lock) */
	guint                 n_downloads;	/* (lock) */
	guint                 n_bad;		/* (lock) */
	guint                 n_site_redirects;	/* (lock) */
};

/* ── Text helpers ───────────────────────────────────────────────── */

static gchar *
replace(
	gchar       *text,
	const gchar *from,
	const gchar *to
){
	gchar **parts = g_strsplit(text, from, -1);
	gchar *joined = g_strjoinv(to, parts);

	g_strfreev(parts);
	g_free(text);
	return joined;
}

gchar *
axis_test_prowlarr_fixture(
	AxisTestProwlarr *self,
	const gchar      *name
){
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autofree gchar *origin = axis_test_plex_base(self->server);
	g_autofree gchar *own = g_strconcat(origin, self->prefix, NULL);
	g_autofree gchar *key = NULL;
	gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	g_mutex_lock(&self->lock);
	key = g_strdup(self->key);
	g_mutex_unlock(&self->lock);
	text = replace(text, "@ORIGIN@", origin);
	text = replace(text, "@BASE@", self->prefix);
	text = replace(text, "@KEY@", key);
	/* The recordings, scrubbed to a made-up origin and key (only theirs: the
	 * key's digits are a hash's in a constructed answer). */
	if (g_str_has_prefix(name, "recorded/")) {
		text = replace(text, AXIS_TEST_PROWLARR_RECORDED_ORIGIN, own);
		text = replace(text, AXIS_TEST_PROWLARR_RECORDED_KEY, key);
	}
	return text;
}

gchar *
axis_test_prowlarr_release_json(
	AxisTestProwlarr *self,
	const gchar      *name,
	const gchar      *protocol,
	const gchar      *file,
	const gchar      *magnet
){
	g_autofree gchar *origin = axis_test_plex_base(self->server);
	g_autofree gchar *key = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *magnet_json = magnet != NULL ? g_strdup_printf("\"%s\"", magnet) : g_strdup("null");

	g_mutex_lock(&self->lock);
	key = g_strdup(self->key);
	g_mutex_unlock(&self->lock);
	download = file != NULL ? g_strdup_printf("\"%s%s/3/download?apikey=%s&link=ZmlsZQ&file=%s\"", origin,
		self->prefix, key, file) : g_strdup("null");
	return g_strdup_printf("[{\"title\":\"%s\",\"size\":1000000,\"indexerId\":3,\"indexer\":\"NewsOwl\","
		"\"publishDate\":\"2024-03-02T10:11:12Z\",\"downloadUrl\":%s,\"magnetUrl\":%s,\"protocol\":\"%s\","
		"\"seeders\":5,\"leechers\":1}]", name, download, magnet_json, protocol);
}

/* The value of @name in @query ("a=b&c=d"), as sent; or NULL. */
static gchar *
query_value(
	const gchar *query,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("%s=", name);
	const gchar *at = query;

	while (at != NULL && *at != '\0') {
		const gchar *end = strchr(at, '&');

		if (g_str_has_prefix(at, needle))
			return end != NULL ? g_strndup(at + strlen(needle), (gsize)(end - at) - strlen(needle)) :
				g_strdup(at + strlen(needle));
		at = end != NULL ? end + 1 : NULL;
	}
	return NULL;
}

/* Every value of @name in @query, as numbers. */
static GArray *
query_numbers(
	const gchar *query,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("%s=", name);
	GArray *values = g_array_new(FALSE, FALSE, sizeof(gint64));
	const gchar *at = query;

	while (at != NULL && *at != '\0') {
		const gchar *end = strchr(at, '&');

		if (g_str_has_prefix(at, needle)) {
			gint64 value = g_ascii_strtoll(at + strlen(needle), NULL, 10);

			g_array_append_val(values, value);
		}
		at = end != NULL ? end + 1 : NULL;
	}
	return values;
}

/* ── The recordings ─────────────────────────────────────────────── */

static void
recorded_free(gpointer data)
{
	Recorded *recorded = data;

	g_free(recorded->name);
	g_free(recorded->magnet);
	g_free(recorded);
}

static JsonNode *
parse_file(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	if (!json_parser_load_from_file(parser, path, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	return json_node_copy(json_parser_get_root(parser));
}

static const gchar *
member_text(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
		json_node_get_string(node) : NULL;
}

/* A magnet link for @hash as Prowlarr's redirects give it: its name and the
 * trackers, about 1.1 kB in all. */
static gchar *
long_magnet(
	const gchar *hash,
	const gchar *name
){
	GString *magnet = g_string_new(NULL);
	g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
	guint i;

	g_string_append_printf(magnet, "magnet:?xt=urn:btih:%s&dn=%s", hash, escaped);
	for (i = 0; magnet->len < 1100; i++)
		g_string_append_printf(magnet, "&tr=udp%%3a%%2f%%2ftracker%u.opentrackr.test%%3a1337%%2fannounce", i);
	return g_string_free(magnet, FALSE);
}

/* What each recorded release's address answers, by indexer and file. */
static void
read_recordings(AxisTestProwlarr *self)
{
	static const gchar *const searches[] = {
		"recorded/search-movie-imdb.json", "recorded/search-movie-text.json", "recorded/search-tv-tvdb.json", NULL
	};
	g_autoptr(JsonNode) indexers = parse_file("recorded/indexer.json");
	JsonArray *list = json_node_get_array(indexers);
	guint i;
	guint s;

	for (i = 0; i < json_array_get_length(list); i++) {
		JsonObject *indexer = json_array_get_object_element(list, i);

		if (json_object_get_boolean_member_with_default(indexer, "redirect", FALSE)) {
			gint64 id = json_object_get_int_member_with_default(indexer, "id", -1);

			g_array_append_val(self->redirecting, id);
		}
	}
	for (s = 0; searches[s] != NULL; s++) {
		g_autoptr(JsonNode) root = parse_file(searches[s]);
		JsonArray *results = json_node_get_array(root);

		for (i = 0; i < json_array_get_length(results); i++) {
			JsonObject *result = json_array_get_object_element(results, i);
			const gchar *download = member_text(result, "downloadUrl");
			const gchar *link = member_text(result, "magnetUrl");
			const gchar *address = download != NULL ? download : link;
			const gchar *query = address != NULL ? strchr(address, '?') : NULL;
			const gchar *guid = member_text(result, "guid");
			const gchar *hash = member_text(result, "infoHash");
			g_autofree gchar *file = query != NULL ? query_value(query + 1, "file") : NULL;
			Recorded *recorded;

			if (file == NULL)
				continue;
			recorded = g_new0(Recorded, 1);
			recorded->torrent = g_strcmp0(member_text(result, "protocol"), "torrent") == 0;
			recorded->has_file = download != NULL;
			recorded->name = g_strdup(member_text(result, "title"));
			/* The Pirate Bay's guid is its magnet link, as the redirect gives it. */
			if (guid != NULL && g_str_has_prefix(guid, "magnet:"))
				recorded->magnet = g_strdup(guid);
			else if (hash != NULL)
				recorded->magnet = long_magnet(hash, recorded->name != NULL ? recorded->name : file);
			g_hash_table_replace(self->releases, g_strdup_printf("%" G_GINT64_FORMAT "|%s",
				json_object_get_int_member_with_default(result, "indexerId", 0), file), recorded);
		}
	}
}

/* A recorded search answer with only the results of the indexers @ids
 * names (all of them when it names none), as Prowlarr asks only those. */
static gchar *
only_indexers(
	const gchar  *body,
	const GArray *ids
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) kept = NULL;
	JsonArray *results;
	JsonArray *out;
	guint i;
	guint j;

	if (ids->len == 0 || !json_parser_load_from_data(parser, body, -1, NULL) ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
		return g_strdup(body);
	results = json_node_get_array(json_parser_get_root(parser));
	out = json_array_new();
	for (i = 0; i < json_array_get_length(results); i++) {
		JsonNode *element = json_array_get_element(results, i);
		JsonNode *member;
		gint64 id;
		gboolean asked = FALSE;

		/* What is not a result of an indexer named is not answered. */
		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;
		member = json_object_get_member(json_node_get_object(element), "indexerId");
		if (member == NULL || !JSON_NODE_HOLDS_VALUE(member) || json_node_get_value_type(member) != G_TYPE_INT64)
			continue;
		id = json_node_get_int(member);
		for (j = 0; j < ids->len; j++)
			asked = asked || g_array_index(ids, gint64, j) == id;
		if (asked)
			json_array_add_element(out, json_node_copy(element));
	}
	kept = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(kept, out);
	generator = json_generator_new();
	json_generator_set_root(generator, kept);
	return json_generator_to_data(generator, NULL);
}

/* ── The server ─────────────────────────────────────────────────── */

static void
answer_bytes(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *type,
	GBytes             *body
){
	answer->status = status;
	answer->content_type = g_strdup(type);
	answer->body = body;
}

static void
answer_text(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *type,
	const gchar        *text
){
	answer_bytes(answer, status, type, g_bytes_new(text, strlen(text)));
}

/* Prowlarr's redirects are 301s. */
static void
answer_redirect(
	AxisTestPlexAnswer *answer,
	const gchar        *location
){
	answer->status = 301;
	answer->headers = g_new0(gchar *, 3);
	answer->headers[0] = g_strdup("Location");
	answer->headers[1] = g_strdup(location);
}

/* A download of an indexer whose "Redirect" is on: to the indexer's site,
 * with the indexer's own key in the address. */
static void
answer_site(
	AxisTestProwlarr   *self,
	const gchar        *file,
	AxisTestPlexAnswer *answer
){
	g_autofree gchar *site = NULL;
	g_autofree gchar *location = NULL;

	g_mutex_lock(&self->lock);
	site = g_strdup(self->site);
	self->n_site_redirects++;
	g_mutex_unlock(&self->lock);
	location = g_strdup_printf("%s" AXIS_TEST_PROWLARR_SITE_PATH "?t=get&id=%s&apikey=" AXIS_TEST_PROWLARR_INDEXER_KEY,
		site != NULL ? site : "https://indexer-site.invalid", file != NULL ? file : "none");
	answer_redirect(answer, location);
}

/* A link address's answer: its magnet link, or one of another torrent. */
static void
answer_magnet(
	AxisTestProwlarr   *self,
	const gchar        *magnet,
	AxisTestPlexAnswer *answer
){
	gboolean wrong;

	g_mutex_lock(&self->lock);
	wrong = self->wrong_magnets;
	g_mutex_unlock(&self->lock);
	if (wrong) {
		g_autofree gchar *other = long_magnet(AXIS_TEST_PROWLARR_WRONG_HASH, "Another.Torrent");

		answer_redirect(answer, other);
	} else {
		answer_redirect(answer, magnet);
	}
}

static gboolean
redirects(
	AxisTestProwlarr *self,
	gint64            indexer
){
	guint i;

	for (i = 0; i < self->redirecting->len; i++) {
		if (g_array_index(self->redirecting, gint64, i) == indexer)
			return TRUE;
	}
	return FALSE;
}

static void
serve_download(
	AxisTestProwlarr          *self,
	const AxisTestPlexRequest *request,
	const gchar               *path,
	AxisTestPlexAnswer        *answer
){
	g_autofree gchar *file = request->query != NULL ? query_value(request->query, "file") : NULL;
	g_autofree gchar *elsewhere = NULL;
	g_autofree gchar *moved = NULL;
	g_autofree gchar *key = NULL;
	gint64 indexer = g_ascii_strtoll(path + 1, NULL, 10);
	const Recorded *recorded = NULL;

	g_mutex_lock(&self->lock);
	elsewhere = g_strdup(self->elsewhere);
	moved = g_strdup_printf("%s/3/download?apikey=%s&link=bW92ZWQ&file=hearth-nzb", self->prefix, self->key);
	g_mutex_unlock(&self->lock);
	if (g_strcmp0(file, "hearth-indexer") == 0 || redirects(self, indexer)) {
		answer_site(self, file, answer);
	} else if (g_strcmp0(file, "hearth-nzb") == 0) {
		answer_text(answer, 200, "application/x-nzb", AXIS_TEST_PROWLARR_NZB);
	} else if (g_strcmp0(file, "hearth-torrent") == 0) {
		GBytes *torrent = axis_test_download_torrent("Hearth.1995", NULL);

		answer_bytes(answer, 200, "application/x-bittorrent", torrent);
	} else if (g_strcmp0(file, "hearth-magnet") == 0) {
		answer_magnet(self, AXIS_TEST_PROWLARR_REDIRECT_MAGNET, answer);
	} else if (g_strcmp0(file, "hearth-long-magnet") == 0) {
		g_autofree gchar *magnet = long_magnet(AXIS_TEST_PROWLARR_REDIRECT_HASH, "Hearth.1995.720p");

		answer_magnet(self, magnet, answer);
	} else if (g_strcmp0(file, "hearth-bad-magnet") == 0) {
		answer_redirect(answer, "magnet:?xt=urn:btih:zzzz");
	} else if (g_strcmp0(file, "hearth-elsewhere") == 0) {
		answer_redirect(answer, elsewhere != NULL ? elsewhere : "http://127.0.0.1:1/elsewhere");
	} else if (g_strcmp0(file, "hearth-moved") == 0) {
		answer_redirect(answer, moved);
	} else if (g_strcmp0(file, "hearth-html") == 0) {
		answer_text(answer, 200, "application/x-nzb", "<html><body>Please log in</body></html>");
	} else if (g_strcmp0(file, "hearth-huge") == 0) {
		guint8 *zeros = g_malloc0(HUGE_BYTES);

		answer_bytes(answer, 200, "application/x-nzb", g_bytes_new_take(zeros, HUGE_BYTES));
	} else if (g_strcmp0(file, "hearth-gone") == 0) {
		answer_text(answer, 404, "application/json", "{\"message\":\"NotFound\"}");
	} else if (self->recorded && file != NULL &&
	           (key = g_strdup_printf("%" G_GINT64_FORMAT "|%s", indexer, file)) != NULL &&
	           (recorded = g_hash_table_lookup(self->releases, key)) != NULL) {
		if (recorded->torrent && !recorded->has_file && recorded->magnet != NULL) {
			/* Known by its link address only: Prowlarr redirects to the magnet. */
			answer_magnet(self, recorded->magnet, answer);
		} else if (recorded->torrent) {
			answer_bytes(answer, 200, "application/x-bittorrent",
				axis_test_download_torrent(recorded->name != NULL ? recorded->name : "Recorded", NULL));
		} else {
			answer_text(answer, 200, "application/x-nzb", AXIS_TEST_PROWLARR_NZB);
		}
	} else {
		answer_text(answer, 400, "application/json", "{\"message\":\"unknown file\"}");
	}
}

/* The fixture a search of @type answers when the test set none. */
static gchar *
default_search(
	AxisTestProwlarr *self,
	const gchar      *type
){
	if (!self->recorded)
		return axis_test_prowlarr_fixture(self, "search-movie.json");
	if (g_strcmp0(type, "tvsearch") == 0)
		return axis_test_prowlarr_fixture(self, "recorded/search-tv-tvdb.json");
	if (g_strcmp0(type, "movie") == 0)
		return axis_test_prowlarr_fixture(self, "recorded/search-movie-imdb.json");
	return axis_test_prowlarr_fixture(self, "recorded/search-movie-text.json");
}

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestProwlarr *self = user_data;
	const gchar *sent = axis_test_plex_request_header(request, "X-Api-Key");
	const gchar *path = request->path;
	g_autofree gchar *wanted = NULL;
	g_autofree gchar *download_key = NULL;
	g_autofree gchar *body = NULL;
	const gchar *folder = self->recorded ? "recorded/" : "";
	AxisTestProwlarrMode mode;
	guint delay;
	gboolean download;
	gboolean keyed;

	g_mutex_lock(&self->lock);
	self->n_requests++;
	mode = self->mode;
	delay = self->delay_ms;
	wanted = g_strdup(self->key);
	g_mutex_unlock(&self->lock);

	if (mode == AXIS_TEST_PROWLARR_DOWN) {
		answer->hang = TRUE;
		return;
	}
	if (mode == AXIS_TEST_PROWLARR_TOO_MANY) {
		answer->status = 429;
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Retry-After");
		answer->headers[1] = g_strdup("30");
		return;
	}
	if (mode == AXIS_TEST_PROWLARR_BROKEN) {
		answer_text(answer, 500, "application/json", "{\"message\":\"Internal server error\"}");
		return;
	}
	if (self->prefix[0] != '\0') {
		if (!g_str_has_prefix(path, self->prefix) || path[strlen(self->prefix)] != '/') {
			g_mutex_lock(&self->lock);
			self->n_bad++;
			g_mutex_unlock(&self->lock);
			answer_text(answer, 404, "text/plain", "not found");
			return;
		}
		path += strlen(self->prefix);
	}
	download = g_regex_match_simple("^/[0-9]+/download$", path, 0, 0);
	download_key = request->query != NULL ? query_value(request->query, "apikey") : NULL;
	keyed = g_strcmp0(sent, wanted) == 0 || (download && g_strcmp0(download_key, wanted) == 0);
	if (!keyed) {
		g_mutex_lock(&self->lock);
		self->n_bad++;
		g_mutex_unlock(&self->lock);
		/* As Prowlarr answers it: a status, no body, no content type. */
		answer->status = 401;
		return;
	}
	if (!download && request->query != NULL && strstr(request->query, wanted) != NULL) {
		/* The key in the address of a request that is not a download. */
		g_mutex_lock(&self->lock);
		self->n_bad++;
		g_mutex_unlock(&self->lock);
	}
	if (strcmp(request->method, "GET") != 0) {
		g_mutex_lock(&self->lock);
		self->n_bad++;
		g_mutex_unlock(&self->lock);
		answer_text(answer, 405, "application/json", "{}");
		return;
	}

	if (download) {
		g_mutex_lock(&self->lock);
		self->n_downloads++;
		g_mutex_unlock(&self->lock);
		serve_download(self, request, path, answer);
	} else if (strcmp(path, "/api/v1/system/status") == 0) {
		g_autofree gchar *name = g_strconcat(folder, "system-status.json", NULL);

		body = axis_test_prowlarr_fixture(self, name);
		answer_text(answer, 200, "application/json", body);
	} else if (strcmp(path, "/api/v1/indexer") == 0) {
		g_autofree gchar *name = g_strconcat(folder, "indexer.json", NULL);

		g_mutex_lock(&self->lock);
		body = g_strdup(self->indexers);
		g_mutex_unlock(&self->lock);
		if (body == NULL)
			body = axis_test_prowlarr_fixture(self, name);
		answer_text(answer, 200, "application/json", body);
	} else if (strcmp(path, "/api/v1/indexerstatus") == 0) {
		g_autofree gchar *name = g_strconcat(folder, "indexerstatus.json", NULL);

		body = axis_test_prowlarr_fixture(self, name);
		answer_text(answer, 200, "application/json", body);
	} else if (strcmp(path, "/api/v1/search") == 0) {
		g_autofree gchar *type = request->query != NULL ? query_value(request->query, "type") : NULL;
		const gchar *chosen;

		g_mutex_lock(&self->lock);
		g_ptr_array_add(self->queries, g_strdup(request->query != NULL ? request->query : ""));
		chosen = g_hash_table_lookup(self->searches, type != NULL ? type : "");
		if (chosen == NULL)
			chosen = g_hash_table_lookup(self->searches, "");
		body = chosen != NULL ? g_strdup(chosen) : NULL;
		g_mutex_unlock(&self->lock);
		if (delay > 0)
			g_usleep((gulong)delay * 1000);
		if (body == NULL)
			body = default_search(self, type);
		/* Prowlarr asks only the indexers named, when the search names some. */
		{
			g_autoptr(GArray) ids = query_numbers(request->query != NULL ? request->query : "", "indexerIds");
			g_autofree gchar *kept = only_indexers(body, ids);

			answer_text(answer, 200, "application/json", kept);
		}
	} else {
		answer_text(answer, 404, "application/json", "{\"message\":\"NotFound\"}");
	}
}

static AxisTestProwlarr *
start(
	const gchar *key,
	const gchar *prefix,
	gboolean     recorded
){
	AxisTestProwlarr *self = g_new0(AxisTestProwlarr, 1);

	g_mutex_init(&self->lock);
	self->key = g_strdup(key);
	self->prefix = g_strdup(prefix != NULL ? prefix : "");
	self->recorded = recorded;
	self->releases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, recorded_free);
	self->redirecting = g_array_new(FALSE, FALSE, sizeof(gint64));
	self->searches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->queries = g_ptr_array_new_with_free_func(g_free);
	if (recorded)
		read_recordings(self);
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	axis_test_plex_set_handler(self->server, handle, self);
	return self;
}

AxisTestProwlarr *
axis_test_prowlarr_new(
	const gchar *key,
	const gchar *prefix
){
	return start(key, prefix, FALSE);
}

AxisTestProwlarr *
axis_test_prowlarr_new_recorded(
	const gchar *key,
	const gchar *prefix
){
	return start(key, prefix, TRUE);
}

void
axis_test_prowlarr_free(AxisTestProwlarr *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_hash_table_unref(self->searches);
	g_hash_table_unref(self->releases);
	g_array_unref(self->redirecting);
	g_ptr_array_unref(self->queries);
	g_free(self->key);
	g_free(self->prefix);
	g_free(self->elsewhere);
	g_free(self->site);
	g_free(self->indexers);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_test_prowlarr_set_indexers(
	AxisTestProwlarr *self,
	const gchar      *body
){
	g_mutex_lock(&self->lock);
	g_free(self->indexers);
	self->indexers = g_strdup(body);
	g_mutex_unlock(&self->lock);
}

gchar *
axis_test_prowlarr_base(AxisTestProwlarr *self)
{
	return axis_test_plex_base(self->server);
}

AxisTestPlex *
axis_test_prowlarr_server(AxisTestProwlarr *self)
{
	return self->server;
}

void
axis_test_prowlarr_set_mode(
	AxisTestProwlarr     *self,
	AxisTestProwlarrMode  mode
){
	g_mutex_lock(&self->lock);
	self->mode = mode;
	g_mutex_unlock(&self->lock);
}

void
axis_test_prowlarr_set_key(
	AxisTestProwlarr *self,
	const gchar      *key
){
	g_mutex_lock(&self->lock);
	g_free(self->key);
	self->key = g_strdup(key);
	g_mutex_unlock(&self->lock);
}

void
axis_test_prowlarr_set_elsewhere(
	AxisTestProwlarr *self,
	const gchar      *base
){
	g_mutex_lock(&self->lock);
	g_free(self->elsewhere);
	self->elsewhere = g_strdup(base);
	g_mutex_unlock(&self->lock);
}

void
axis_test_prowlarr_set_indexer_site(
	AxisTestProwlarr *self,
	const gchar      *base
){
	g_mutex_lock(&self->lock);
	g_free(self->site);
	self->site = g_strdup(base);
	g_mutex_unlock(&self->lock);
}

void
axis_test_prowlarr_set_wrong_magnets(
	AxisTestProwlarr *self,
	gboolean          wrong
){
	g_mutex_lock(&self->lock);
	self->wrong_magnets = wrong;
	g_mutex_unlock(&self->lock);
}

void
axis_test_prowlarr_set_search_delay(
	AxisTestProwlarr *self,
	guint             milliseconds
){
	g_mutex_lock(&self->lock);
	self->delay_ms = milliseconds;
	g_mutex_unlock(&self->lock);
}

void
axis_test_prowlarr_set_search(
	AxisTestProwlarr *self,
	const gchar      *type,
	const gchar      *body
){
	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->searches, g_strdup(type != NULL ? type : ""), g_strdup(body));
	g_mutex_unlock(&self->lock);
}

guint
axis_test_prowlarr_n_searches(AxisTestProwlarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->queries->len;
	g_mutex_unlock(&self->lock);
	return n;
}

gchar *
axis_test_prowlarr_search_query(
	AxisTestProwlarr *self,
	guint             index
){
	gchar *query = NULL;

	g_mutex_lock(&self->lock);
	if (index < self->queries->len)
		query = g_strdup(g_ptr_array_index(self->queries, index));
	g_mutex_unlock(&self->lock);
	return query;
}

guint
axis_test_prowlarr_n_requests(AxisTestProwlarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->n_requests;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_prowlarr_n_downloads(AxisTestProwlarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->n_downloads;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_prowlarr_n_bad(AxisTestProwlarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->n_bad;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_prowlarr_n_site_redirects(AxisTestProwlarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->n_site_redirects;
	g_mutex_unlock(&self->lock);
	return n;
}
