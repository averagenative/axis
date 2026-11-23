/*
 * axis-test-sites.c - test support: a stand-in for YTS, EZTV or The Pirate Bay
 * on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-sites.h"

#include <string.h>

#define FIXTURES "tests/fixtures/sites/"

struct _AxisTestSites
{
	AxisTestPlex      *server;
	GMutex             lock;
	AxisTestSite       site;
	AxisTestSitesMode  mode;	/* (lock) */
	guint              delay_ms;	/* (lock) */
	GHashTable        *bodies;	/* (lock) "term|page" -> body */
	GPtrArray         *queries;	/* (lock) gchar *: every query */
	GPtrArray         *paths;	/* (lock) gchar *: every path */
	guint              n_bad;	/* (lock) */
};

gchar *
axis_test_sites_fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	return text;
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

static const gchar *
site_path(AxisTestSite site)
{
	switch (site) {
	case AXIS_TEST_SITE_YTS:
		return "/api/v2/list_movies.json";
	case AXIS_TEST_SITE_EZTV:
		return "/api/get-torrents";
	default:
		return "/q.php";
	}
}

static const gchar *
term_name(AxisTestSite site)
{
	switch (site) {
	case AXIS_TEST_SITE_YTS:
		return "query_term";
	case AXIS_TEST_SITE_EZTV:
		return "imdb_id";
	default:
		return "q";
	}
}

static void
bad(AxisTestSites *self)
{
	g_mutex_lock(&self->lock);
	self->n_bad++;
	g_mutex_unlock(&self->lock);
}

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestSites *self = user_data;
	const gchar *query = request->query != NULL ? request->query : "";
	g_autofree gchar *term = query_value(query, term_name(self->site));
	g_autofree gchar *page_text = query_value(query, "page");
	g_autofree gchar *by_term = NULL;
	g_autofree gchar *by_page = NULL;
	g_autofree gchar *raw = NULL;
	AxisTestSitesMode mode;
	guint delay;
	guint page = page_text != NULL ? (guint)g_ascii_strtoull(page_text, NULL, 10) : 1;
	const gchar *body;
	guint i;

	g_mutex_lock(&self->lock);
	g_ptr_array_add(self->queries, g_strdup(query));
	g_ptr_array_add(self->paths, g_strdup(request->path));
	mode = self->mode;
	delay = self->delay_ms;
	g_mutex_unlock(&self->lock);

	/* Nothing is sent to a public site but a GET of its own path: no
	 * credential, no cookie, no key in the query. */
	for (i = 0; request->headers != NULL && request->headers[i] != NULL && request->headers[i + 1] != NULL; i += 2) {
		if (g_ascii_strcasecmp(request->headers[i], "Authorization") == 0 ||
		    g_ascii_strcasecmp(request->headers[i], "Cookie") == 0 ||
		    g_ascii_strncasecmp(request->headers[i], "X-Api", 5) == 0)
			bad(self);
	}
	if (strcmp(request->method, "GET") != 0 || strstr(query, "apikey") != NULL || strstr(query, "key=") != NULL ||
	    strcmp(request->path, site_path(self->site)) != 0) {
		bad(self);
		answer_text(answer, 405, "text/plain", "no");
		return;
	}
	switch (mode) {
	case AXIS_TEST_SITES_DOWN:
		answer->hang = TRUE;
		return;
	case AXIS_TEST_SITES_BROKEN:
		answer_text(answer, 500, "text/plain", "Internal server error");
		return;
	case AXIS_TEST_SITES_GONE:
		answer_text(answer, 404, "text/html", "<html><body>Not found</body></html>");
		return;
	case AXIS_TEST_SITES_FORBIDDEN:
		answer_text(answer, 403, "text/html", "<html><body>Access denied</body></html>");
		return;
	case AXIS_TEST_SITES_MOVED:
		answer->status = 301;
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Location");
		answer->headers[1] = g_strdup("https://new-home.example/");
		return;
	case AXIS_TEST_SITES_PARKED:
		answer_text(answer, 200, "text/html", "<html><body><h1>This domain is for sale</h1></body></html>");
		return;
	case AXIS_TEST_SITES_LIMITED:
		answer_text(answer, 429, "text/plain", "slow down");
		answer->headers = g_new0(gchar *, 3);
		answer->headers[0] = g_strdup("Retry-After");
		answer->headers[1] = g_strdup("3");
		return;
	default:
		break;
	}
	if (delay > 0)
		g_usleep((gulong)delay * 1000);
	/* The test's body: for this term and page, this term, this page, any. */
	by_term = g_strdup_printf("%s|%u", term != NULL ? term : "", page);
	by_page = g_strdup_printf("|%u", page);
	g_mutex_lock(&self->lock);
	body = g_hash_table_lookup(self->bodies, by_term);
	if (body == NULL && term != NULL) {
		g_autofree gchar *any_page = g_strdup_printf("%s|0", term);

		body = g_hash_table_lookup(self->bodies, any_page);
	}
	if (body == NULL)
		body = g_hash_table_lookup(self->bodies, by_page);
	if (body == NULL)
		body = g_hash_table_lookup(self->bodies, "|0");
	raw = body != NULL ? g_strdup(body) : NULL;
	g_mutex_unlock(&self->lock);
	if (raw == NULL) {
		switch (self->site) {
		case AXIS_TEST_SITE_YTS:
			raw = axis_test_sites_fixture("yts-list.json");
			break;
		case AXIS_TEST_SITE_EZTV:
			raw = axis_test_sites_fixture("eztv-list.json");
			break;
		default:
			raw = axis_test_sites_fixture("tpb-list.json");
			break;
		}
	}
	answer_text(answer, 200, "application/json", raw);
}

AxisTestSites *
axis_test_sites_new(AxisTestSite site)
{
	AxisTestSites *self = g_new0(AxisTestSites, 1);

	g_mutex_init(&self->lock);
	self->site = site;
	self->bodies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->queries = g_ptr_array_new_with_free_func(g_free);
	self->paths = g_ptr_array_new_with_free_func(g_free);
	self->server = axis_test_plex_new();
	g_assert_nonnull(self->server);
	axis_test_plex_set_handler(self->server, handle, self);
	return self;
}

void
axis_test_sites_free(AxisTestSites *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_hash_table_unref(self->bodies);
	g_ptr_array_unref(self->queries);
	g_ptr_array_unref(self->paths);
	g_mutex_clear(&self->lock);
	g_free(self);
}

gchar *
axis_test_sites_base(AxisTestSites *self)
{
	return axis_test_plex_base(self->server);
}

AxisTestPlex *
axis_test_sites_server(AxisTestSites *self)
{
	return self->server;
}

void
axis_test_sites_set_mode(
	AxisTestSites     *self,
	AxisTestSitesMode  mode
){
	g_mutex_lock(&self->lock);
	self->mode = mode;
	g_mutex_unlock(&self->lock);
}

void
axis_test_sites_set_body(
	AxisTestSites *self,
	const gchar   *term,
	guint          page,
	const gchar   *body
){
	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->bodies, g_strdup_printf("%s|%u", term != NULL ? term : "", page), g_strdup(body));
	g_mutex_unlock(&self->lock);
}

void
axis_test_sites_set_delay(
	AxisTestSites *self,
	guint          milliseconds
){
	g_mutex_lock(&self->lock);
	self->delay_ms = milliseconds;
	g_mutex_unlock(&self->lock);
}

guint
axis_test_sites_n_requests(AxisTestSites *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->queries->len;
	g_mutex_unlock(&self->lock);
	return n;
}

gchar *
axis_test_sites_query(
	AxisTestSites *self,
	guint          index
){
	gchar *query = NULL;

	g_mutex_lock(&self->lock);
	if (index < self->queries->len)
		query = g_strdup(g_ptr_array_index(self->queries, index));
	g_mutex_unlock(&self->lock);
	return query;
}

gchar *
axis_test_sites_path(
	AxisTestSites *self,
	guint          index
){
	gchar *path = NULL;

	g_mutex_lock(&self->lock);
	if (index < self->paths->len)
		path = g_strdup(g_ptr_array_index(self->paths, index));
	g_mutex_unlock(&self->lock);
	return path;
}

guint
axis_test_sites_n_bad(AxisTestSites *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->n_bad;
	g_mutex_unlock(&self->lock);
	return n;
}
