/*
 * axis-test-newznab.c - test support: a stand-in Newznab indexer or Torznab
 * feed on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-newznab.h"

#include <string.h>

#include "axis-test-download.h"
#include "axis-test-prowlarr.h"

#define FIXTURES "tests/fixtures/newznab/"

/* The magnet link a /dl/.../?file=magnet answers with. */
#define DL_MAGNET \
	"magnet:?xt=urn:btih:cccccccccccccccccccccccccccccccccccccccc&dn=Hearth.1995.Feed&tr=http%3A%2F%2Ftracker.test%2Fa"

struct _AxisTestNewznab
{
	AxisTestPlex        *server;
	GMutex               lock;
	gboolean             torznab;
	gchar               *prefix;
	gchar               *key;	/* (lock) */
	gchar               *files;	/* (lock) */
	gchar               *caps;	/* (lock) */
	GHashTable          *searches;	/* (lock) t (or "") -> body */
	AxisTestNewznabMode  mode;	/* (lock) */
	guint                delay_ms;	/* (lock) */
	GPtrArray           *queries;	/* (lock) gchar *: every /api query */
	gchar               *last_search;	/* (lock) */
	guint                n_requests;	/* (lock) */
	guint                n_searches;	/* (lock) */
	guint                n_caps;	/* (lock) */
	guint                n_files;	/* (lock) */
	guint                n_bad;	/* (lock) */
};

/* ── Text ───────────────────────────────────────────────────────── */

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

/* @text with the stand-in's own put in. */
static gchar *
substitute(
	AxisTestNewznab *self,
	const gchar     *text
){
	g_autofree gchar *origin = axis_test_plex_base(self->server);
	g_autofree gchar *key = NULL;
	g_autofree gchar *files = NULL;
	gchar *out = g_strdup(text);

	g_mutex_lock(&self->lock);
	key = g_strdup(self->key);
	files = g_strdup(self->files != NULL ? self->files : origin);
	g_mutex_unlock(&self->lock);
	out = replace(out, "@FILES@", files);
	out = replace(out, "@ORIGIN@", origin);
	out = replace(out, "@BASE@", self->prefix);
	out = replace(out, "@KEY@", key);
	return out;
}

gchar *
axis_test_newznab_fixture(
	AxisTestNewznab *self,
	const gchar     *name
){
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	return substitute(self, text);
}

gchar *
axis_test_newznab_item(
	AxisTestNewznab *self,
	const gchar     *title,
	const gchar     *guid,
	gint64           size,
	const gchar     *id
){
	g_autofree gchar *address = id != NULL ? g_strdup_printf("@FILES@@BASE@/api?t=get&amp;id=%s&amp;apikey=@KEY@", id) :
		NULL;
	g_autofree gchar *enclosure = address != NULL ? g_strdup_printf("<enclosure url=\"%s\" length=\"%" G_GINT64_FORMAT
		"\" type=\"application/x-nzb\"/>", address, size > 0 ? size : 0) : g_strdup("");
	g_autofree gchar *sized = size >= 0 ? g_strdup_printf("<newznab:attr name=\"size\" value=\"%" G_GINT64_FORMAT
		"\"/>", size) : g_strdup("");
	g_autofree gchar *raw = g_strdup_printf("<item><title>%s</title><guid isPermaLink=\"true\">@ORIGIN@/details/%s"
		"</guid>%s%s%s<pubDate>Sat, 02 Mar 2024 10:11:12 +0000</pubDate>%s"
		"<newznab:attr name=\"category\" value=\"2000\"/>%s<newznab:attr name=\"grabs\" value=\"7\"/></item>",
		title, guid, address != NULL ? "<link>" : "", address != NULL ? address : "", address != NULL ? "</link>" : "",
		enclosure, sized);

	return substitute(self, raw);
}

gchar *
axis_test_newznab_feed(
	const gchar *items,
	gint         api_used,
	gint         api_max,
	gint         grab_used,
	gint         grab_max
){
	g_autofree gchar *limits = api_max != 0 ? g_strdup_printf("<newznab:apilimits apicurrent=\"%d\" apimax=\"%d\" "
		"grabcurrent=\"%d\" grabmax=\"%d\"/>", api_used, api_max, grab_used, grab_max) : g_strdup("");

	return g_strdup_printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\" "
		"xmlns:atom=\"http://www.w3.org/2005/Atom\" xmlns:newznab=\"http://www.newznab.com/DTD/2010/feeds/attributes/\">"
		"<channel><title>Stand-in</title><newznab:response offset=\"0\" total=\"1\"/>%s%s</channel></rss>\n", limits,
		items != NULL ? items : "");
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

/* ── The server ─────────────────────────────────────────────────── */

static void
answer_text(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *type,
	const gchar        *text
){
	answer->status = status;
	answer->content_type = g_strdup(type);
	answer->body = g_bytes_new(text, strlen(text));
}

static void
answer_error(
	AxisTestPlexAnswer *answer,
	gint                code,
	const gchar        *description
){
	g_autofree gchar *text = g_strdup_printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<error code=\"%d\" "
		"description=\"%s\"/>\n", code, description);

	/* As indexers answer them: with status 200. */
	answer_text(answer, 200, "application/xml", text);
}

static void
count(
	AxisTestNewznab *self,
	guint           *counter
){
	g_mutex_lock(&self->lock);
	(*counter)++;
	g_mutex_unlock(&self->lock);
}

/* A release's file by its id: "nzb-", "html-", "gone-", "limit-". */
static void
serve_file(
	AxisTestNewznab    *self,
	const gchar        *id,
	AxisTestPlexAnswer *answer
){
	count(self, &self->n_files);
	if (id != NULL && g_str_has_prefix(id, "html-"))
		answer_text(answer, 200, "application/x-nzb", "<html><body>Please log in</body></html>");
	else if (id != NULL && g_str_has_prefix(id, "gone-"))
		answer_error(answer, 300, "No such item");
	else if (id != NULL && g_str_has_prefix(id, "limit-"))
		answer_error(answer, 501, "Download limit reached");
	else if (id != NULL && g_str_has_prefix(id, "torrent-"))
		answer_text(answer, 200, "application/x-bittorrent", "not this");
	else
		answer_text(answer, 200, "application/x-nzb", AXIS_TEST_PROWLARR_NZB);
}

static void
serve_api(
	AxisTestNewznab           *self,
	const AxisTestPlexRequest *request,
	AxisTestNewznabMode        mode,
	const gchar               *key,
	guint                      delay,
	AxisTestPlexAnswer        *answer
){
	const gchar *query = request->query != NULL ? request->query : "";
	g_autofree gchar *t = query_value(query, "t");
	g_autofree gchar *sent = query_value(query, "apikey");
	g_autofree gchar *body = NULL;

	g_mutex_lock(&self->lock);
	g_ptr_array_add(self->queries, g_strdup(query));
	g_mutex_unlock(&self->lock);
	if (g_strcmp0(sent, key) != 0) {
		count(self, &self->n_bad);
		answer_error(answer, 100, "Incorrect user credentials");
		return;
	}
	if (mode == AXIS_TEST_NEWZNAB_LIMIT) {
		answer_error(answer, 500, "Request limit reached");
		return;
	}
	if (g_strcmp0(t, "caps") == 0) {
		count(self, &self->n_caps);
		if (mode == AXIS_TEST_NEWZNAB_NO_CAPS) {
			answer_error(answer, 203, "Function not available");
			return;
		}
		if (mode == AXIS_TEST_NEWZNAB_JUNK_CAPS) {
			answer_text(answer, 200, "text/html", "<html><body><p>Welcome<br></body></html>");
			return;
		}
		g_mutex_lock(&self->lock);
		body = self->caps != NULL ? g_strdup(self->caps) : NULL;
		g_mutex_unlock(&self->lock);
		if (body == NULL)
			body = axis_test_newznab_fixture(self, self->torznab ? "torznab-caps.xml" : "caps.xml");
		answer_text(answer, 200, "application/xml", body);
		return;
	}
	if (g_strcmp0(t, "get") == 0) {
		g_autofree gchar *id = query_value(query, "id");

		serve_file(self, id, answer);
		return;
	}
	if (g_strcmp0(t, "search") == 0 || g_strcmp0(t, "movie") == 0 || g_strcmp0(t, "tvsearch") == 0) {
		const gchar *chosen;
		g_autofree gchar *raw = NULL;

		g_autofree gchar *page_key = NULL;

		g_mutex_lock(&self->lock);
		page_key = g_strdup_printf("page:%u", self->n_searches);
		self->n_searches++;
		g_free(self->last_search);
		self->last_search = g_strdup(query);
		chosen = g_hash_table_lookup(self->searches, page_key);
		if (chosen == NULL)
			chosen = g_hash_table_lookup(self->searches, t);
		if (chosen == NULL)
			chosen = g_hash_table_lookup(self->searches, "");
		raw = chosen != NULL ? g_strdup(chosen) : NULL;
		g_mutex_unlock(&self->lock);
		if (delay > 0)
			g_usleep((gulong)delay * 1000);
		if (g_strcmp0(raw, "@FAIL@") == 0) {
			answer_text(answer, 500, "text/plain", "broken");
			return;
		}
		if (raw != NULL)
			body = substitute(self, raw);
		else if (self->torznab)
			body = axis_test_newznab_fixture(self, "torznab-search.xml");
		else
			body = axis_test_newznab_fixture(self, g_strcmp0(t, "tvsearch") == 0 ? "search-tv.xml" :
				"search-movie.xml");
		answer_text(answer, 200, self->torznab ? "application/rss+xml" : "application/xml", body);
		return;
	}
	answer_error(answer, 202, "No such function");
}

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestNewznab *self = user_data;
	g_autofree gchar *key = NULL;
	g_autofree gchar *api = g_strconcat(self->prefix, "/api", NULL);
	AxisTestNewznabMode mode;
	guint delay;
	guint i;

	g_mutex_lock(&self->lock);
	self->n_requests++;
	mode = self->mode;
	delay = self->delay_ms;
	key = g_strdup(self->key);
	g_mutex_unlock(&self->lock);

	if (mode == AXIS_TEST_NEWZNAB_DOWN) {
		answer->hang = TRUE;
		return;
	}
	/* The key is never in a header, and nothing but GET is sent. */
	for (i = 0; request->headers != NULL && request->headers[i] != NULL && request->headers[i + 1] != NULL; i += 2) {
		if (strstr(request->headers[i + 1], key) != NULL)
			count(self, &self->n_bad);
	}
	if (strcmp(request->method, "GET") != 0) {
		count(self, &self->n_bad);
		answer_text(answer, 405, "text/plain", "no");
		return;
	}
	if (mode == AXIS_TEST_NEWZNAB_BROKEN) {
		answer_text(answer, 500, "text/plain", "Internal server error");
		return;
	}
	if (strcmp(request->path, api) == 0) {
		serve_api(self, request, mode, key, delay, answer);
	} else if (g_str_has_prefix(request->path, "/getnzb/")) {
		g_autofree gchar *id = g_strdup(request->path + strlen("/getnzb/"));

		serve_file(self, id, answer);
	} else if (g_str_has_prefix(request->path, "/dl/")) {
		g_autofree gchar *file = request->query != NULL ? query_value(request->query, "file") : NULL;

		count(self, &self->n_files);
		if (g_strcmp0(file, "magnet") == 0) {
			answer->status = 302;
			answer->headers = g_new0(gchar *, 3);
			answer->headers[0] = g_strdup("Location");
			answer->headers[1] = g_strdup(DL_MAGNET);
		} else {
			answer->status = 200;
			answer->content_type = g_strdup("application/x-bittorrent");
			answer->body = axis_test_download_torrent(file != NULL ? file : "Feed", NULL);
		}
	} else {
		answer_text(answer, 404, "text/plain", "not found");
	}
}

AxisTestNewznab *
axis_test_newznab_new(
	const gchar *key,
	gboolean     torznab,
	const gchar *prefix
){
	AxisTestNewznab *self = g_new0(AxisTestNewznab, 1);

	g_mutex_init(&self->lock);
	self->torznab = torznab;
	self->prefix = g_strdup(prefix != NULL ? prefix : "");
	self->key = g_strdup(key);
	self->searches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->queries = g_ptr_array_new_with_free_func(g_free);
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	axis_test_plex_set_handler(self->server, handle, self);
	return self;
}

void
axis_test_newznab_free(AxisTestNewznab *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_hash_table_unref(self->searches);
	g_ptr_array_unref(self->queries);
	g_free(self->prefix);
	g_free(self->key);
	g_free(self->files);
	g_free(self->caps);
	g_free(self->last_search);
	g_mutex_clear(&self->lock);
	g_free(self);
}

gchar *
axis_test_newznab_base(AxisTestNewznab *self)
{
	return axis_test_plex_base(self->server);
}

AxisTestPlex *
axis_test_newznab_server(AxisTestNewznab *self)
{
	return self->server;
}

void
axis_test_newznab_set_mode(
	AxisTestNewznab     *self,
	AxisTestNewznabMode  mode
){
	g_mutex_lock(&self->lock);
	self->mode = mode;
	g_mutex_unlock(&self->lock);
}

void
axis_test_newznab_set_key(
	AxisTestNewznab *self,
	const gchar     *key
){
	g_mutex_lock(&self->lock);
	g_free(self->key);
	self->key = g_strdup(key);
	g_mutex_unlock(&self->lock);
}

void
axis_test_newznab_set_files(
	AxisTestNewznab *self,
	const gchar     *base
){
	g_mutex_lock(&self->lock);
	g_free(self->files);
	self->files = g_strdup(base);
	g_mutex_unlock(&self->lock);
}

void
axis_test_newznab_set_caps(
	AxisTestNewznab *self,
	const gchar     *body
){
	g_autofree gchar *done = body != NULL ? substitute(self, body) : NULL;

	g_mutex_lock(&self->lock);
	g_free(self->caps);
	self->caps = g_steal_pointer(&done);
	g_mutex_unlock(&self->lock);
}

void
axis_test_newznab_set_search(
	AxisTestNewznab *self,
	const gchar     *t,
	const gchar     *body
){
	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->searches, g_strdup(t != NULL ? t : ""), g_strdup(body));
	g_mutex_unlock(&self->lock);
}

void
axis_test_newznab_set_search_page(
	AxisTestNewznab *self,
	guint            index,
	const gchar     *body
){
	g_autofree gchar *key = g_strdup_printf("page:%u", index);

	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->searches, g_steal_pointer(&key), g_strdup(body));
	g_mutex_unlock(&self->lock);
}

void
axis_test_newznab_set_delay(
	AxisTestNewznab *self,
	guint            milliseconds
){
	g_mutex_lock(&self->lock);
	self->delay_ms = milliseconds;
	g_mutex_unlock(&self->lock);
}

static guint
read_count(
	AxisTestNewznab *self,
	const guint     *counter
){
	guint n;

	g_mutex_lock(&self->lock);
	n = *counter;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_newznab_n_requests(AxisTestNewznab *self)
{
	return read_count(self, &self->n_requests);
}

guint
axis_test_newznab_n_searches(AxisTestNewznab *self)
{
	return read_count(self, &self->n_searches);
}

guint
axis_test_newznab_n_caps(AxisTestNewznab *self)
{
	return read_count(self, &self->n_caps);
}

guint
axis_test_newznab_n_files(AxisTestNewznab *self)
{
	return read_count(self, &self->n_files);
}

guint
axis_test_newznab_n_bad(AxisTestNewznab *self)
{
	return read_count(self, &self->n_bad);
}

gchar *
axis_test_newznab_query(
	AxisTestNewznab *self,
	guint            index
){
	gchar *query = NULL;

	g_mutex_lock(&self->lock);
	if (index < self->queries->len)
		query = g_strdup(g_ptr_array_index(self->queries, index));
	g_mutex_unlock(&self->lock);
	return query;
}

gchar *
axis_test_newznab_last_search(AxisTestNewznab *self)
{
	gchar *query;

	g_mutex_lock(&self->lock);
	query = g_strdup(self->last_search);
	g_mutex_unlock(&self->lock);
	return query;
}
