/*
 * axis-def-engine.c - the engine of indexer definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/def/axis-def-engine.h"

#include <string.h>

#include "indexer/axis-indexer-sites-parse.h"
#include "indexer/def/axis-def-filter.h"
#include "indexer/def/axis-def-html.h"
#include "indexer/def/axis-def-regex.h"
#include "indexer/def/axis-def-select.h"
#include "indexer/def/axis-def-template.h"
#include "probe/axis-probe-json.h"

/* The most text a field's value keeps. */
#define VALUE_MAX_BYTES (16 * 1024)

/* Markers around an action's output in a path, so that what a template
 * puts in is encoded and what the definition wrote is not. */
#define MARK_OPEN  '\x01'
#define MARK_CLOSE '\x02'

/* A count above this is not believed. */
#define COUNT_MAX G_GINT64_CONSTANT(1000000000)

/* A publication date this far ahead of now is not believed. */
#define FUTURE_SLACK_US (G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC)
#define DATE_MIN_US (G_GINT64_CONSTANT(631152000) * G_USEC_PER_SEC)

struct _AxisDefSearch
{
	AxisDefSite         site;
	AxisTemplateVars    vars;
	gchar              *keywords;
	gchar              *words;	/* the request's, before the keyword filters: what andmatch holds a name to */
	GPtrArray          *requests;	/* AxisDefRequest */
	AxisIndexerMode     mode;
	AxisDefRegexCache  *regex;
	GHashTable         *selectors;	/* text -> AxisSelector * */
	GHashTable         *templates;	/* text -> AxisTemplate * */
	AxisDefTrace       *trace;	/* nullable, borrowed */
};

static void
request_free(AxisDefRequest *request)
{
	g_free(request->path);
	g_strfreev(request->headers);
	g_free(request);
}

void
axis_def_search_free(AxisDefSearch *search)
{
	if (search == NULL)
		return;
	g_clear_pointer(&search->vars.values, g_hash_table_unref);
	g_clear_pointer(&search->vars.categories, g_ptr_array_unref);
	g_free(search->keywords);
	g_free(search->words);
	g_clear_pointer(&search->requests, g_ptr_array_unref);
	axis_def_regex_cache_free(search->regex);
	g_clear_pointer(&search->selectors, g_hash_table_unref);
	g_clear_pointer(&search->templates, g_hash_table_unref);
	g_free(search);
}

AxisIndexerMode
axis_def_search_mode(const AxisDefSearch *search)
{
	return search->mode;
}

gboolean
axis_def_search_has_words(const AxisDefSearch *search)
{
	return search->mode == AXIS_INDEXER_BY_IMDB || (search->keywords != NULL && search->keywords[0] != '\0');
}

guint
axis_def_search_n_requests(const AxisDefSearch *search)
{
	return search->requests->len;
}

const AxisDefRequest *
axis_def_search_request(
	const AxisDefSearch *search,
	guint                index
){
	return index < search->requests->len ? g_ptr_array_index(search->requests, index) : NULL;
}

static void
set_problem(
	gchar       **problem,
	const gchar  *text
){
	if (problem != NULL && *problem == NULL)
		*problem = g_strdup(text);
}

/* ── Encodings ──────────────────────────────────────────────────── */

static gboolean
is_utf8(const gchar *encoding)
{
	return encoding == NULL || g_ascii_strcasecmp(encoding, "UTF-8") == 0 || g_ascii_strcasecmp(encoding, "UTF8") == 0;
}

/* @text's bytes in the site's encoding (characters it has no byte for as
 * '?'). */
static gchar *
to_site(
	const gchar *text,
	const gchar *encoding,
	gsize       *length
){
	gchar *converted;

	if (!is_utf8(encoding)) {
		converted = g_convert_with_fallback(text, -1, encoding, "UTF-8", "?", NULL, length, NULL);
		if (converted != NULL)
			return converted;
	}
	*length = strlen(text);
	return g_strdup(text);
}

/* An answer in the site's encoding as UTF-8. */
static gchar *
from_site(
	const gchar *body,
	gsize        length,
	const gchar *encoding
){
	if (!is_utf8(encoding)) {
		gchar *converted = g_convert_with_fallback(body, (gssize)length, "UTF-8", encoding, "\xef\xbf\xbd", NULL,
			NULL, NULL);

		if (converted != NULL)
			return converted;
	}
	return g_utf8_make_valid(body, (gssize)length);
}

/* Form encoding of @n bytes, as .NET's WebUtility.UrlEncode. */
static void
append_form_encoded(
	GString     *out,
	const gchar *bytes,
	gsize        n
){
	gsize i;

	for (i = 0; i < n; i++) {
		guchar c = (guchar)bytes[i];

		if (g_ascii_isalnum(c) || strchr("-_.!*()", c) != NULL)
			g_string_append_c(out, (gchar)c);
		else if (c == ' ')
			g_string_append_c(out, '+');
		else
			g_string_append_printf(out, "%%%02X", c);
	}
}

static void
append_encoded_value(
	GString     *out,
	const gchar *value,
	const gchar *encoding
){
	gsize n = 0;
	g_autofree gchar *bytes = to_site(value, encoding, &n);

	append_form_encoded(out, bytes, n);
}

/* An action's output in a path, marked to be encoded later. */
static gchar *
mark_output(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	g_string_append_c(out, MARK_OPEN);
	for (p = text; *p != '\0'; p++) {
		if (*p != MARK_OPEN && *p != MARK_CLOSE)
			g_string_append_c(out, *p);
	}
	g_string_append_c(out, MARK_CLOSE);
	return g_string_free(out, FALSE);
}

/* A rendered path: its marked parts encoded, every other byte that cannot
 * be in a path percent-encoded. */
static gchar *
encode_marked(
	const gchar *text,
	const gchar *encoding
){
	GString *out = g_string_new(NULL);
	const gchar *p = text;

	while (*p != '\0') {
		if (*p == MARK_OPEN) {
			const gchar *close = strchr(p + 1, MARK_CLOSE);
			g_autofree gchar *part = close != NULL ? g_strndup(p + 1, (gsize)(close - p - 1)) : g_strdup(p + 1);

			append_encoded_value(out, part, encoding);
			p = close != NULL ? close + 1 : p + strlen(p);
			continue;
		}
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '#' || *p == '\\' || *p == '"')
			g_string_append_printf(out, "%%%02X", (guchar)*p);
		else
			g_string_append_c(out, *p);
		p++;
	}
	return g_string_free(out, FALSE);
}

/* ── Templates and selectors, kept for the search ───────────────── */

static AxisTemplate *
template_of(
	AxisDefSearch  *search,
	const gchar    *text,
	gchar         **problem
){
	AxisTemplate *template_ = g_hash_table_lookup(search->templates, text);

	if (template_ != NULL)
		return template_;
	template_ = axis_template_parse(text, problem, NULL);
	if (template_ == NULL)
		return NULL;
	if (g_hash_table_size(search->templates) > 2048)
		g_hash_table_remove_all(search->templates);
	g_hash_table_insert(search->templates, g_strdup(text), template_);
	return template_;
}

/* @text rendered with the search's variables (and the row's). */
static gchar *
render(
	AxisDefSearch       *search,
	const gchar         *text,
	AxisTemplateEscape   escape,
	gchar              **problem
){
	AxisTemplate *template_;

	if (text == NULL)
		return g_strdup("");
	/* Text without an action is the definition's own: never escaped. */
	if (!axis_template_has_actions(text))
		return g_strdup(text);
	template_ = template_of(search, text, problem);
	if (template_ == NULL)
		return NULL;
	return axis_template_render(template_, &search->vars, escape, search->regex, problem);
}

static AxisSelector *
selector_of(
	AxisDefSearch  *search,
	const gchar    *text,
	gchar         **problem
){
	g_autofree gchar *rendered = render(search, text, NULL, problem);
	AxisSelector *selector;

	if (rendered == NULL)
		return NULL;
	selector = g_hash_table_lookup(search->selectors, rendered);
	if (selector != NULL)
		return selector;
	selector = axis_selector_parse(rendered, problem, NULL);
	if (selector == NULL)
		return NULL;
	if (g_hash_table_size(search->selectors) > 2048)
		g_hash_table_remove_all(search->selectors);
	g_hash_table_insert(search->selectors, g_steal_pointer(&rendered), selector);
	return selector;
}

/* ── The search ─────────────────────────────────────────────────── */

static void
set_var(
	AxisDefSearch *search,
	const gchar   *name,
	const gchar   *value
){
	g_hash_table_replace(search->vars.values, g_strdup(name), g_strdup(value != NULL ? value : ""));
}

static void
set_number(
	AxisDefSearch *search,
	const gchar   *name,
	gint64         value,
	gboolean       stated
){
	g_autofree gchar *text = stated ? g_strdup_printf("%" G_GINT64_FORMAT, value) : g_strdup("");

	set_var(search, name, text);
}

/* The search's words as a person would type them: a movie's title and year,
 * an episode's title with S01E02, a season's with S01. */
static gchar *
words_of(const AxisIndexerRequest *request)
{
	g_autofree gchar *title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);

	if (title == NULL)
		return g_strdup("");
	switch (request->kind) {
	case AXIS_INDEXER_QUERY_FILM:
		return request->year > 0 ? g_strdup_printf("%s %d", title, request->year) : g_strdup(title);
	case AXIS_INDEXER_QUERY_EPISODE:
		return g_strdup_printf("%s S%02dE%02d", title, MAX(request->season, 0), MAX(request->episode, 0));
	case AXIS_INDEXER_QUERY_SEASON:
		return g_strdup_printf("%s S%02d", title, MAX(request->season, 0));
	default:
		return g_strdup(title);
	}
}

/* The site's categories for what is searched: those mapped to the
 * request's Newznab categories, or to the kind's own (2000 movies, 5000
 * TV), counting a category's subcategories in it. */
static GPtrArray *
site_categories(
	const AxisDef            *def,
	const AxisIndexerRequest *request
){
	GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
	gint wanted[AXIS_INDEXER_MAX_CATEGORIES];
	guint n_wanted = 0;
	guint i;
	guint k;

	if (request->n_categories > 0) {
		for (i = 0; i < request->n_categories && i < AXIS_INDEXER_MAX_CATEGORIES; i++)
			wanted[n_wanted++] = request->categories[i];
	} else if (request->kind == AXIS_INDEXER_QUERY_FILM) {
		wanted[n_wanted++] = 2000;
	} else if (request->kind == AXIS_INDEXER_QUERY_EPISODE || request->kind == AXIS_INDEXER_QUERY_SEASON ||
	           request->anime) {
		/* (5000 counts TV/Anime, 5070, in it: words asked for an anime series
		 * are asked in the site's TV categories, its anime one among them.) */
		wanted[n_wanted++] = 5000;
	}
	for (i = 0; i < def->categories->len; i++) {
		const AxisDefCategory *category = g_ptr_array_index(def->categories, i);
		gboolean match = FALSE;
		guint j;

		for (k = 0; k < n_wanted && !match; k++)
			match = category->newznab == wanted[k] || (wanted[k] % 1000 == 0 && category->newznab / 1000 == wanted[k] / 1000);
		if (!match)
			continue;
		for (j = 0; j < ids->len; j++) {
			if (strcmp(g_ptr_array_index(ids, j), category->site_id) == 0)
				break;
		}
		if (j == ids->len)
			g_ptr_array_add(ids, g_strdup(category->site_id));
	}
	return ids;
}

/* "https://host" (the port only when it is not the scheme's own). */
static gchar *
origin_text(const AxisDefSite *site)
{
	const AxisHttpOrigin *origin = site->origin;
	gboolean plain_port = origin->https ? origin->port == 443 : origin->port == 80;
	gboolean ipv6 = strchr(origin->host, ':') != NULL;

	if (plain_port)
		return g_strdup_printf("%s://%s%s%s", origin->https ? "https" : "http", ipv6 ? "[" : "", origin->host,
			ipv6 ? "]" : "");
	return g_strdup(origin->base);
}

/* The site's link the paths are joined to: "https://host/base/". */
static gchar *
site_link(const AxisDefSite *site)
{
	g_autofree gchar *origin = origin_text(site);

	return g_strdup_printf("%s%s/", origin, site->base_path != NULL ? site->base_path : "");
}

static gboolean
setup_vars(
	AxisDefSearch            *search,
	const AxisIndexerRequest *request,
	gchar                   **problem
){
	const AxisDef *def = search->site.def;
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_utc(search->site.now_us / G_USEC_PER_SEC);
	g_autofree gchar *link = site_link(&search->site);
	g_autofree gchar *words = words_of(request);
	g_autofree gchar *imdb_short = NULL;
	const gchar *type;
	guint i;

	search->vars.values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < def->settings->len; i++) {
		const AxisDefSetting *setting = g_ptr_array_index(def->settings, i);
		g_autofree gchar *name = g_strconcat("Config.", setting->name, NULL);

		set_var(search, name, setting->value);
	}
	set_var(search, "Config.sitelink", link);
	if (now != NULL) {
		set_number(search, "Today.Year", g_date_time_get_year(now), TRUE);
		set_number(search, "Today.Month", g_date_time_get_month(now), TRUE);
		set_number(search, "Today.Day", g_date_time_get_day_of_month(now), TRUE);
	}
	/* The words, through the definition's keyword filters; a slash would
	 * break a path, so it is a space.  As they were, they are what andmatch
	 * holds a name to (a filter that makes "Made Up's" "Made Up s" makes
	 * the site's query, not the words searched for). */
	search->words = g_strdup(words);
	g_strdelimit(words, "/\\", ' ');
	search->keywords = g_steal_pointer(&words);
	for (i = 0; i < def->keywords_filters->len; i++) {
		const AxisDefFilter *filter = g_ptr_array_index(def->keywords_filters, i);
		GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
		gchar *next;
		guint k;

		for (k = 0; filter->args[k] != NULL; k++) {
			gchar *arg = render(search, filter->args[k], NULL, problem);

			if (arg == NULL) {
				g_ptr_array_unref(args);
				return FALSE;
			}
			g_ptr_array_add(args, arg);
		}
		g_ptr_array_add(args, NULL);
		next = axis_def_filter_apply(filter->name, (const gchar *const *)args->pdata, search->keywords,
			search->site.now_us, search->regex, problem);
		g_ptr_array_unref(args);
		if (next == NULL)
			return FALSE;
		g_free(search->keywords);
		search->keywords = g_strstrip(next);
	}
	type = request->kind == AXIS_INDEXER_QUERY_FILM ? "movie" : request->kind == AXIS_INDEXER_QUERY_TEXT ? "search" :
		"tvsearch";
	set_var(search, "Keywords", search->keywords);
	set_var(search, "Query.Keywords", search->keywords);
	set_var(search, "Query.Q", search->keywords);
	set_var(search, "Query.Type", type);
	set_var(search, "Query.Series", request->kind == AXIS_INDEXER_QUERY_FILM ? "" : request->title);
	set_var(search, "Query.Movie", request->kind == AXIS_INDEXER_QUERY_FILM ? request->title : "");
	set_number(search, "Query.Year", request->year, request->year > 0);
	set_number(search, "Query.Season", request->season, request->season >= 0 &&
		request->kind != AXIS_INDEXER_QUERY_FILM);
	set_number(search, "Query.Ep", request->episode, request->episode > 0 &&
		request->kind == AXIS_INDEXER_QUERY_EPISODE);
	if (request->imdb_id != NULL && axis_indexer_imdb_valid(request->imdb_id))
		imdb_short = g_strdup(request->imdb_id + 2);
	set_var(search, "Query.IMDBID", imdb_short != NULL ? request->imdb_id : "");
	set_var(search, "Query.IMDBIDShort", imdb_short != NULL ? imdb_short : "");
	set_number(search, "Query.TMDBID", request->tmdb_id, request->tmdb_id > 0);
	set_number(search, "Query.TVDBID", request->tvdb_id, request->tvdb_id > 0);
	set_var(search, "Query.Limit", "100");
	set_var(search, "Query.Offset", "0");
	search->vars.categories = site_categories(def, request);
	search->mode = AXIS_INDEXER_BY_TEXT;
	if (imdb_short != NULL && ((request->kind == AXIS_INDEXER_QUERY_FILM && (def->movie_params & AXIS_DEF_PARAM_IMDB)) ||
	    (request->kind != AXIS_INDEXER_QUERY_FILM && (def->tv_params & AXIS_DEF_PARAM_IMDB))))
		search->mode = AXIS_INDEXER_BY_IMDB;
	return TRUE;
}

static gchar *
escape_path_value(const gchar *text)
{
	return mark_output(text);
}

/* Whether the path is asked for the search's categories. */
static gboolean
path_wanted(
	const AxisDefSearch *search,
	const AxisDefPath   *path
){
	guint i;
	guint k;

	if (path->categories == NULL || search->vars.categories->len == 0)
		return TRUE;
	for (i = 0; i < path->categories->len; i++) {
		for (k = 0; k < search->vars.categories->len; k++) {
			if (strcmp(g_ptr_array_index(path->categories, i), g_ptr_array_index(search->vars.categories, k)) == 0)
				return TRUE;
		}
	}
	return FALSE;
}

/* The path and query a rendered path names on the origin, or %NULL when it
 * names another place. */
static gchar *
path_on_origin(
	const AxisDefSite *site,
	const gchar       *path
){
	if (g_str_has_prefix(path, "http://") || g_str_has_prefix(path, "https://") || g_str_has_prefix(path, "//")) {
		g_autofree gchar *url = g_str_has_prefix(path, "//") ? g_strconcat(site->origin->https ? "https:" : "http:",
			path, NULL) : g_strdup(path);
		gchar *on = NULL;

		/* A URL with no path is the site's root. */
		{
			const gchar *after = strstr(url, "//") + 2;

			if (strchr(after, '/') == NULL && strchr(after, '?') == NULL) {
				gchar *rooted = g_strconcat(url, "/", NULL);

				g_free(url);
				url = rooted;
			}
		}
		if (axis_indexer_fetch_ref(url, site->origin, "", &on) != AXIS_INDEXER_REF_OK) {
			g_free(on);
			return NULL;
		}
		return on;
	}
	if (path[0] == '/')
		return g_strdup(path);
	return g_strdup_printf("%s/%s", site->base_path != NULL ? site->base_path : "", path);
}

static void
add_input(
	GString     *query,
	const gchar *separator,
	const gchar *name,
	const gchar *value,
	const gchar *encoding
){
	if (query->len > 0)
		g_string_append(query, separator);
	append_encoded_value(query, name, encoding);
	g_string_append_c(query, '=');
	append_encoded_value(query, value, encoding);
}

/* A header the definition asks for, when its value is safe to send. */
static gboolean
header_ok(const gchar *value)
{
	const gchar *p;

	if (value == NULL || value[0] == '\0' || strlen(value) > 1024)
		return FALSE;
	for (p = value; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

static gboolean
build_requests(
	AxisDefSearch  *search,
	gchar         **problem
){
	const AxisDef *def = search->site.def;
	const gchar *encoding = def->encoding;
	guint i;
	guint k;

	search->requests = g_ptr_array_new_with_free_func((GDestroyNotify)request_free);
	for (i = 0; i < def->paths->len; i++) {
		const AxisDefPath *path = g_ptr_array_index(def->paths, i);
		g_autofree gchar *rendered = NULL;
		g_autofree gchar *encoded = NULL;
		g_autofree gchar *on = NULL;
		g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);
		GPtrArray *inputs = g_ptr_array_new();
		GString *query = g_string_new(NULL);
		GPtrArray *headers;
		AxisDefRequest *request;
		gboolean ok = TRUE;

		if (!path_wanted(search, path)) {
			g_ptr_array_unref(inputs);
			g_string_free(query, TRUE);
			continue;
		}
		rendered = render(search, path->path, escape_path_value, problem);
		if (rendered == NULL) {
			g_ptr_array_unref(inputs);
			g_string_free(query, TRUE);
			return FALSE;
		}
		encoded = encode_marked(rendered, encoding);
		on = path_on_origin(&search->site, encoded);
		if (on == NULL) {
			g_ptr_array_unref(inputs);
			g_string_free(query, TRUE);
			set_problem(problem, "the definition asks an address that is not its site's, so nothing was asked");
			return FALSE;
		}
		/* The path's own inputs, and the search's that it does not name. */
		for (k = 0; k < path->inputs->len; k++) {
			const AxisDefPair *input = g_ptr_array_index(path->inputs, k);

			g_ptr_array_add(inputs, (gpointer)input);
			g_hash_table_add(names, input->name);
		}
		for (k = 0; path->inherit_inputs && k < def->inputs->len; k++) {
			const AxisDefPair *input = g_ptr_array_index(def->inputs, k);

			if (!g_hash_table_contains(names, input->name))
				g_ptr_array_add(inputs, (gpointer)input);
		}
		for (k = 0; k < inputs->len && ok; k++) {
			const AxisDefPair *input = g_ptr_array_index(inputs, k);
			g_autofree gchar *value = render(search, input->value, NULL, problem);

			if (value == NULL) {
				ok = FALSE;
				break;
			}
			if (strcmp(input->name, "$raw") == 0) {
				g_autofree gchar *raw = encode_marked(value, encoding);

				if (raw[0] != '\0') {
					if (query->len > 0 && raw[0] != '&')
						g_string_append(query, path->query_separator);
					g_string_append(query, raw);
				}
				continue;
			}
			if (value[0] == '\0' && !def->allow_empty_inputs)
				continue;
			add_input(query, path->query_separator, input->name, value, encoding);
		}
		g_ptr_array_unref(inputs);
		if (!ok) {
			g_string_free(query, TRUE);
			return FALSE;
		}
		request = g_new0(AxisDefRequest, 1);
		request->response = path->response;
		if (query->len > 0)
			request->path = g_strdup_printf("%s%c%s", on, strchr(on, '?') != NULL ? '&' : '?', query->str);
		else
			request->path = g_strdup(on);
		g_string_free(query, TRUE);
		if (!axis_indexer_path_ok(request->path, "")) {
			request_free(request);
			set_problem(problem, "the definition builds a request that is not a valid path, so nothing was asked");
			return FALSE;
		}
		headers = g_ptr_array_new_with_free_func(g_free);
		for (k = 0; k < def->headers->len; k++) {
			const AxisDefPair *header = g_ptr_array_index(def->headers, k);
			g_autofree gchar *value = render(search, header->value, NULL, NULL);

			if (header_ok(value)) {
				g_ptr_array_add(headers, g_strdup(header->name));
				g_ptr_array_add(headers, g_steal_pointer(&value));
			}
		}
		g_ptr_array_add(headers, NULL);
		request->headers = (gchar **)g_ptr_array_free(headers, FALSE);
		g_ptr_array_add(search->requests, request);
	}
	if (search->requests->len == 0) {
		set_problem(problem, "the definition has no search path for what is searched");
		return FALSE;
	}
	return TRUE;
}

AxisDefSearch *
axis_def_search_new(
	const AxisDefSite        *site,
	const AxisIndexerRequest *request,
	gchar                   **problem
){
	AxisDefSearch *search;

	if (problem != NULL)
		*problem = NULL;
	if (site == NULL || site->def == NULL || site->origin == NULL || !axis_def_usable(site->def)) {
		set_problem(problem, "the definition is not usable");
		return NULL;
	}
	search = g_new0(AxisDefSearch, 1);
	search->site = *site;
	search->regex = axis_def_regex_cache_new(AXIS_DEF_REGEX_BUDGET_US);
	search->selectors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)axis_selector_free);
	search->templates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)axis_template_free);
	if (!setup_vars(search, request, problem) || !build_requests(search, problem)) {
		axis_def_search_free(search);
		return NULL;
	}
	return search;
}

void
axis_def_search_set_trace(
	AxisDefSearch *search,
	AxisDefTrace  *trace
){
	guint i;

	search->trace = trace;
	if (trace == NULL)
		return;
	g_free(trace->definition);
	trace->definition = search->site.def->version > 0 ?
		g_strdup_printf("%s, version %d", search->site.def->id, search->site.def->version) :
		g_strdup_printf("%s (it states no version)", search->site.def->id);
	g_free(trace->words);
	trace->words = axis_def_trace_text(search->keywords, AXIS_DEF_TRACE_VALUE_CHARS);
	g_ptr_array_set_size(trace->requests, 0);
	for (i = 0; i < search->requests->len; i++) {
		const AxisDefRequest *request = g_ptr_array_index(search->requests, i);

		g_ptr_array_add(trace->requests, axis_def_trace_text(request->path, 600));
	}
}

/* ── Reading a row ──────────────────────────────────────────────── */

/* Where a row's values are read from: an element, or a JSON value. */
typedef struct
{
	const AxisHtmlNode *element;
	const AxisJsonNode *json;
} Row;

typedef enum
{
	READ_OK,
	READ_MISSING,
	READ_FAILED
} ReadResult;

/* The JSON value at @path from @from: "$" is @root, then names joined by
 * '.' and "[n]" indexes. */
static const AxisJsonNode *
json_at(
	const AxisJsonNode *root,
	const AxisJsonNode *from,
	const gchar        *path
){
	g_auto(GStrv) parts = NULL;
	const AxisJsonNode *node = from;
	guint i;

	if (path == NULL)
		return NULL;
	if (path[0] == '$') {
		node = root;
		path++;
		if (*path == '.')
			path++;
	}
	if (*path == '\0')
		return node;
	parts = g_strsplit(path, ".", 32);
	for (i = 0; parts[i] != NULL && node != NULL; i++) {
		gchar *bracket = strchr(parts[i], '[');

		if (bracket != parts[i]) {
			g_autofree gchar *name = bracket != NULL ? g_strndup(parts[i], (gsize)(bracket - parts[i])) :
				g_strdup(parts[i]);

			node = axis_json_member(node, name);
		}
		while (bracket != NULL && node != NULL) {
			gint64 index = g_ascii_strtoll(bracket + 1, NULL, 10);

			node = index >= 0 && index < 100000 ? axis_json_element(node, (guint)index) : NULL;
			bracket = strchr(bracket + 1, '[');
		}
	}
	return node;
}

/* An element's value: its attribute, or its text without what @remove
 * names. */
static gchar *
element_value(
	AxisDefSearch      *search,
	const AxisHtmlNode *element,
	const AxisDefBlock *block,
	AxisSelectBudget   *budget,
	gchar             **problem
){
	if (block->attribute != NULL) {
		const gchar *value = axis_html_attr(element, block->attribute);

		return value != NULL ? g_strdup(value) : NULL;
	}
	if (block->remove != NULL) {
		AxisSelector *remove = selector_of(search, block->remove, problem);
		g_autoptr(GPtrArray) gone = NULL;
		gchar *text;
		guint i;

		if (remove == NULL)
			return NULL;
		gone = axis_selector_all(remove, element, 1000, budget);
		for (i = 0; i < gone->len; i++)
			((AxisHtmlNode *)g_ptr_array_index(gone, i))->excluded = TRUE;
		text = axis_html_text(element, VALUE_MAX_BYTES);
		for (i = 0; i < gone->len; i++)
			((AxisHtmlNode *)g_ptr_array_index(gone, i))->excluded = FALSE;
		return text;
	}
	return axis_html_text(element, VALUE_MAX_BYTES);
}

/* The element a block's selector picks in @row: the row itself when it
 * matches, else its first descendant that does. */
static const AxisHtmlNode *
pick(
	const AxisSelector *selector,
	const AxisHtmlNode *row,
	AxisSelectBudget   *budget
){
	if (row->type == AXIS_HTML_ELEMENT && axis_selector_matches(selector, row, row, budget))
		return row;
	return axis_selector_first(selector, row, budget);
}

static ReadResult
read_block_value(
	AxisDefSearch       *search,
	const AxisDefBlock  *block,
	const Row           *row,
	const AxisJsonNode  *json_root,
	AxisSelectBudget    *budget,
	gchar              **value,
	gchar              **problem
){
	*value = NULL;
	if (block->text != NULL) {
		*value = render(search, block->text, NULL, problem);
		return *value != NULL ? READ_OK : READ_FAILED;
	}
	if (block->selector == NULL)
		return READ_MISSING;
	if (row->json != NULL) {
		g_autofree gchar *path = render(search, block->selector, NULL, problem);
		const AxisJsonNode *node;
		const gchar *text;
		guint i;

		if (path == NULL)
			return READ_FAILED;
		node = json_at(json_root, row->json, path);
		text = axis_json_scalar_text(node);
		if (text == NULL && axis_json_type(node) == AXIS_JSON_BOOLEAN) {
			gboolean truth = FALSE;

			axis_json_get_boolean(node, &truth);
			text = truth ? "True" : "False";
		}
		if (text == NULL)
			return READ_MISSING;
		if (block->case_keys->len == 0) {
			*value = g_strdup(text);
			return READ_OK;
		}
		for (i = 0; i < block->case_keys->len; i++) {
			const gchar *key = g_ptr_array_index(block->case_keys, i);

			if (strcmp(key, "*") == 0 || strcmp(key, text) == 0) {
				*value = render(search, g_ptr_array_index(block->case_values, i), NULL, problem);
				return *value != NULL ? READ_OK : READ_FAILED;
			}
		}
		return READ_MISSING;
	}
	{
		AxisSelector *selector = selector_of(search, block->selector, problem);
		const AxisHtmlNode *element;
		guint i;

		if (selector == NULL)
			return READ_FAILED;
		element = pick(selector, row->element, budget);
		if (element == NULL)
			return budget->exhausted ? READ_FAILED : READ_MISSING;
		if (block->case_keys->len == 0) {
			*value = element_value(search, element, block, budget, problem);
			return *value != NULL ? READ_OK : READ_MISSING;
		}
		for (i = 0; i < block->case_keys->len; i++) {
			const gchar *key = g_ptr_array_index(block->case_keys, i);
			AxisSelector *test;

			if (strcmp(key, "*") == 0) {
				*value = render(search, g_ptr_array_index(block->case_values, i), NULL, problem);
				return *value != NULL ? READ_OK : READ_FAILED;
			}
			test = selector_of(search, key, problem);
			if (test == NULL)
				return READ_FAILED;
			if (axis_selector_matches(test, element, element, budget) || axis_selector_first(test, element, budget)) {
				*value = render(search, g_ptr_array_index(block->case_values, i), NULL, problem);
				return *value != NULL ? READ_OK : READ_FAILED;
			}
		}
		return READ_MISSING;
	}
}

/* A block's value through its filters; @value NULL and READ_MISSING for a
 * value that is not there and not optional.  @raw (optional): what was read
 * before the filters; @defaulted (optional): nothing was there, and the
 * block's default was taken. */
static ReadResult
block_value_seen(
	AxisDefSearch       *search,
	const AxisDefBlock  *block,
	const Row           *row,
	const AxisJsonNode  *json_root,
	AxisSelectBudget    *budget,
	gchar              **value,
	gchar              **raw,
	gboolean            *defaulted,
	gchar              **problem
){
	ReadResult result = read_block_value(search, block, row, json_root, budget, value, problem);
	guint i;

	if (raw != NULL)
		*raw = NULL;
	if (defaulted != NULL)
		*defaulted = FALSE;
	if (result == READ_FAILED)
		return result;
	if (result == READ_MISSING) {
		if (!block->optional)
			return READ_MISSING;
		if (defaulted != NULL)
			*defaulted = TRUE;
		*value = block->default_text != NULL ? render(search, block->default_text, NULL, problem) : g_strdup("");
		if (*value == NULL)
			return READ_FAILED;
		return READ_OK;
	}
	if (raw != NULL)
		*raw = g_strdup(*value);
	for (i = 0; i < block->filters->len; i++) {
		const AxisDefFilter *filter = g_ptr_array_index(block->filters, i);
		GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
		gchar *next;
		guint k;

		for (k = 0; filter->args[k] != NULL; k++) {
			gchar *arg = render(search, filter->args[k], NULL, problem);

			if (arg == NULL) {
				g_ptr_array_unref(args);
				g_clear_pointer(value, g_free);
				return READ_FAILED;
			}
			g_ptr_array_add(args, arg);
		}
		g_ptr_array_add(args, NULL);
		next = axis_def_filter_apply(filter->name, (const gchar *const *)args->pdata, *value, search->site.now_us,
			search->regex, problem);
		g_ptr_array_unref(args);
		g_free(*value);
		*value = next;
		if (next == NULL)
			return READ_FAILED;
	}
	g_strstrip(*value);
	return READ_OK;
}

static ReadResult
block_value(
	AxisDefSearch       *search,
	const AxisDefBlock  *block,
	const Row           *row,
	const AxisJsonNode  *json_root,
	AxisSelectBudget    *budget,
	gchar              **value,
	gchar              **problem
){
	return block_value_seen(search, block, row, json_root, budget, value, NULL, NULL, problem);
}

/* "Result.<name>" of the row being read. */
static const gchar *
result(
	AxisDefSearch *search,
	const gchar   *name
){
	g_autofree gchar *key = g_strconcat("Result.", name, NULL);
	const gchar *value = g_hash_table_lookup(search->vars.values, key);

	return value != NULL && value[0] != '\0' ? value : NULL;
}

/* Clears the row's results from the variables. */
static void
clear_results(AxisDefSearch *search)
{
	GHashTableIter iter;
	gpointer key;

	g_hash_table_iter_init(&iter, search->vars.values);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (g_str_has_prefix(key, "Result."))
			g_hash_table_iter_remove(&iter);
	}
}

/* The most words of a name or a search andmatch reads. */
#define AND_MATCH_MAX_WORDS 64

/* The plain letters a letter with a stroke, a ligature or a letter of its
 * own stands for: as release/axis-release-match.c's (the folding of
 * axis_release_title_key(), which the indexer layer does not include). */
static const gchar *
plain_letters(gunichar c)
{
	switch (c) {
	case 0x00f8:	/* ø */
		return "o";
	case 0x0111:	/* đ */
	case 0x00f0:	/* ð */
		return "d";
	case 0x0142:	/* ł */
		return "l";
	case 0x00df:	/* ß */
		return "ss";
	case 0x00e6:	/* æ */
		return "ae";
	case 0x0153:	/* œ */
		return "oe";
	case 0x00fe:	/* þ */
		return "th";
	case 0x0127:	/* ħ */
		return "h";
	case 0x0131:	/* ı */
		return "i";
	case 0x0167:	/* ŧ */
		return "t";
	default:
		return NULL;
	}
}

typedef struct
{
	GString  *text;
	gunichar  before;	/* the character before it, 0 at the start */
	gunichar  after;	/* the one after it, 0 at the end */
} Word;

static void
word_free(Word *word)
{
	g_string_free(word->text, TRUE);
	g_free(word);
}

static gboolean
is_mark(gunichar c)
{
	return c != 0 && !g_unichar_isspace(c);
}

/*
 * The words of @text as Axis compares titles: decomposed, case-folded,
 * marks removed, letters with strokes and ligatures written plain,
 * apostrophes dropped ("Made Up's" is "made ups"), "&" read as "and",
 * every other character that is not a letter or a digit a break between
 * words.  Single letters joined by dots are one word ("S.W.A.T." is "swat").
 * Of the words of a search (@searched), a single letter that touches a mark
 * ("X-" of "X-Made") is a leftover of punctuation and is not held to.
 */
static GPtrArray *
title_words(
	const gchar *text,
	gboolean     searched
){
	g_autoptr(GPtrArray) raw = g_ptr_array_new_with_free_func((GDestroyNotify)word_free);
	GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *valid = g_utf8_make_valid(text != NULL ? text : "", -1);
	g_autofree gchar *decomposed = g_utf8_normalize(valid, -1, G_NORMALIZE_NFKD);
	g_autofree gchar *folded = decomposed != NULL ? g_utf8_casefold(decomposed, -1) : NULL;
	g_autofree gchar *again = folded != NULL ? g_utf8_normalize(folded, -1, G_NORMALIZE_NFKD) : NULL;
	Word *word = NULL;
	gunichar before = 0;
	const gchar *p;
	guint i;

	for (p = again != NULL ? again : ""; *p != '\0' && raw->len < AND_MATCH_MAX_WORDS; p = g_utf8_next_char(p)) {
		gunichar c = g_unichar_tolower(g_utf8_get_char(p));
		const gchar *plain = plain_letters(c);
		GUnicodeType type = g_unichar_type(c);

		if (type == G_UNICODE_NON_SPACING_MARK || type == G_UNICODE_ENCLOSING_MARK ||
		    type == G_UNICODE_SPACING_MARK || c == '\'' || c == '`' || c == 0x2018 || c == 0x2019 || c == 0x02bc)
			continue;
		if (plain != NULL || g_unichar_isalnum(c)) {
			if (word == NULL) {
				word = g_new0(Word, 1);
				word->text = g_string_new(NULL);
				word->before = before;
			}
			if (plain != NULL)
				g_string_append(word->text, plain);
			else
				g_string_append_unichar(word->text, c);
			continue;
		}
		if (word != NULL) {
			word->after = c;
			g_ptr_array_add(raw, word);
			word = NULL;
		}
		before = c;
		if (c == '&') {
			/* "&amp;", as a feed that escaped a name twice writes "&". */
			while (strncmp(p + 1, "amp;", 4) == 0)
				p += 4;
			word = g_new0(Word, 1);
			word->text = g_string_new("and");
			word->after = ' ';
			g_ptr_array_add(raw, word);
			word = NULL;
			before = ' ';
		}
	}
	if (word != NULL)
		g_ptr_array_add(raw, word);

	for (i = 0; i < raw->len; i++) {
		Word *w = g_ptr_array_index(raw, i);
		gboolean single = g_utf8_strlen(w->text->str, -1) == 1 && g_unichar_isalpha(g_utf8_get_char(w->text->str));

		/* "s.w.a.t": single letters, a dot between each, are one word. */
		if (single && w->after == '.' && i + 1 < raw->len) {
			GString *joined = g_string_new(w->text->str);
			Word *last = w;

			while (i + 1 < raw->len) {
				Word *next = g_ptr_array_index(raw, i + 1);

				if (last->after != '.' || g_utf8_strlen(next->text->str, -1) != 1 ||
				    !g_unichar_isalpha(g_utf8_get_char(next->text->str)))
					break;
				g_string_append(joined, next->text->str);
				last = next;
				i++;
			}
			if (last != w) {
				g_ptr_array_add(words, g_string_free(joined, FALSE));
				continue;
			}
			g_string_free(joined, TRUE);
		}
		if (searched && single && (is_mark(w->before) || is_mark(w->after)))
			continue;
		g_ptr_array_add(words, g_strdup(w->text->str));
	}
	return words;
}

/* Whether the name holds every word of the words searched for, each as a
 * whole word (andmatch): "Made Up 2" is not in "Made Up 2160p", "Up" not in
 * "Upside", and "Café" is in "Cafe" and the other way round. */
static gboolean
and_match(
	const gchar *title,
	const gchar *searched
){
	g_autoptr(GPtrArray) wanted = title_words(searched, TRUE);
	g_autoptr(GPtrArray) have = title_words(title, FALSE);
	guint i;
	guint k;

	for (i = 0; i < wanted->len; i++) {
		for (k = 0; k < have->len; k++) {
			if (strcmp(g_ptr_array_index(wanted, i), g_ptr_array_index(have, k)) == 0)
				break;
		}
		if (k == have->len)
			return FALSE;
	}
	return TRUE;
}

/* "tt0600001" from what a site says an IMDb identifier is. */
static gchar *
imdb_of(const gchar *text)
{
	const gchar *p = text;
	guint64 value;

	if (text == NULL)
		return NULL;
	if (g_ascii_strncasecmp(p, "tt", 2) == 0)
		p += 2;
	if (*p == '\0' || strspn(p, "0123456789") != strlen(p) || strlen(p) > 10)
		return NULL;
	value = g_ascii_strtoull(p, NULL, 10);
	if (value == 0)
		return NULL;
	return g_strdup_printf("tt%07" G_GUINT64_FORMAT, value);
}

/* A release's download address as a path on the origin: resolved against
 * the page it was found on.  %NULL with @elsewhere set for an address on
 * another place. */
static gchar *
download_path(
	const AxisDefSite *site,
	const gchar       *page_path,
	const gchar       *link,
	gboolean          *elsewhere
){
	g_autofree gchar *origin = origin_text(site);
	g_autofree gchar *base = g_strconcat(origin, page_path, NULL);
	g_autofree gchar *resolved = NULL;
	g_autofree gchar *spaced = NULL;
	gchar *path = NULL;

	*elsewhere = FALSE;
	if (link == NULL || link[0] == '\0' || strlen(link) > AXIS_INDEXER_FETCH_PATH_MAX)
		return NULL;
	/* Spaces as sites leave them in addresses. */
	{
		GString *out = g_string_new(NULL);
		const gchar *p;

		for (p = link; *p != '\0'; p++) {
			if (*p == ' ')
				g_string_append(out, "%20");
			else
				g_string_append_c(out, *p);
		}
		spaced = g_string_free(out, FALSE);
	}
	resolved = g_uri_resolve_relative(base, spaced, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_ENCODED, NULL);
	if (resolved == NULL)
		return NULL;
	if (g_ascii_strncasecmp(resolved, "http://", 7) != 0 && g_ascii_strncasecmp(resolved, "https://", 8) != 0)
		return NULL;
	switch (axis_indexer_fetch_ref(resolved, site->origin, "", &path)) {
	case AXIS_INDEXER_REF_OK:
		return path;
	case AXIS_INDEXER_REF_OFF_ORIGIN:
		*elsewhere = TRUE;
		g_free(path);
		return NULL;
	default:
		g_free(path);
		return NULL;
	}
}

/* The site's categories of a row as Newznab numbers. */
static void
row_categories(
	const AxisDef      *def,
	const gchar        *site_id,
	const gchar        *desc,
	AxisIndexerRelease *release
){
	guint i;

	for (i = 0; i < def->categories->len && release->n_categories < AXIS_INDEXER_MAX_CATEGORIES; i++) {
		const AxisDefCategory *category = g_ptr_array_index(def->categories, i);
		guint k;

		if (category->newznab == 0)
			continue;
		if (!((site_id != NULL && strcmp(category->site_id, site_id) == 0) ||
		      (desc != NULL && category->desc != NULL && g_ascii_strcasecmp(category->desc, desc) == 0)))
			continue;
		for (k = 0; k < release->n_categories; k++) {
			if (release->categories[k] == category->newznab)
				break;
		}
		if (k == release->n_categories)
			release->categories[release->n_categories++] = category->newznab;
	}
}

/*
 * Where one answer's rows go, and what came of them: @lost counts the rows
 * lost for a field every result must have (by what was missing, in
 * @lost_by), @left_out those left out on purpose (@not_video of them for
 * their category).  @answer is the trace's, when there is one.
 */
typedef struct
{
	AxisIndexerOutcome *outcome;
	GHashTable         *seen;	/* merge key -> TRUE, borrowed or own */
	guint               scanned;
	guint               kept;
	guint               lost;
	guint               left_out;
	guint               not_video;
	GHashTable         *lost_by;	/* what was missing -> count */
	AxisDefTraceAnswer *answer;	/* nullable */
	gboolean            excerpts;
} Sink;

/* What became of a row. */
typedef enum
{
	FATE_KEPT,
	FATE_NO_TITLE,
	FATE_NOT_EVERY_WORD,
	FATE_TWO_HASHES,
	FATE_ELSEWHERE,
	FATE_UNUSABLE,
	FATE_NOT_VIDEO,
	FATE_TWICE
} Fate;

/* A row lost for @what (a field's name, or what it lacks). */
static void
sink_lost(
	Sink        *sink,
	const gchar *what
){
	sink->lost++;
	if (sink->lost_by != NULL)
		g_hash_table_replace(sink->lost_by, g_strdup(what),
			GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(sink->lost_by, what)) + 1));
}

/* Whether every one of a release's categories is of a class that is not
 * video (it has one at least). */
static gboolean
not_video(const AxisIndexerRelease *release)
{
	guint i;

	if (release->n_categories == 0)
		return FALSE;
	for (i = 0; i < release->n_categories; i++) {
		if (!axis_sites_category_not_video(release->categories[i]))
			return FALSE;
	}
	return TRUE;
}

/* The first of a release's categories that is not video, in words. */
static const gchar *
category_words(gint category)
{
	switch (category / 1000) {
	case 1:
		return "Console";
	case 3:
		return "Audio";
	case 4:
		return "PC";
	case 7:
		return "Books";
	default:
		return "Other";
	}
}

/*
 * The row's fields made into a release, kept when it can be fetched from
 * the site: its magnet link, its info-hash, or its download address on the
 * origin; and not when its category is not video.  @why (optional) is what
 * a trace says of a release left out of its category.
 */
static Fate
keep_row(
	AxisDefSearch  *search,
	const gchar    *page_path,
	Sink           *sink,
	const gchar   **why
){
	const AxisDef *def = search->site.def;
	const gchar *title = result(search, "title");
	const gchar *magnet_text = result(search, "magnet");
	const gchar *hash_text = result(search, "infohash");
	const gchar *download = result(search, "download");
	g_autoptr(AxisIndexerRelease) release = NULL;
	g_autofree gchar *name = axis_indexer_text_clean(title, AXIS_INDEXER_NAME_MAX_CHARS);
	g_autofree gchar *hash = NULL;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *fetch = NULL;
	g_autofree gchar *key = NULL;
	gboolean elsewhere = FALSE;
	AxisIndexerFound *found;
	gint64 number;

	if (why != NULL)
		*why = NULL;
	if (name == NULL) {
		sink->outcome->n_skipped++;
		sink_lost(sink, "a title");
		return FATE_NO_TITLE;
	}
	if (def->rows_andmatch && search->words != NULL && search->words[0] != '\0' && !and_match(name, search->words)) {
		sink->left_out++;
		return FATE_NOT_EVERY_WORD;
	}
	if (hash_text != NULL && !axis_sites_hash_read(hash_text, &hash))
		g_clear_pointer(&hash, g_free);
	/* A download that is a magnet link is one. */
	if (magnet_text == NULL && download != NULL && g_ascii_strncasecmp(download, "magnet:", 7) == 0) {
		magnet_text = download;
		download = NULL;
	}
	if (magnet_text != NULL) {
		g_autofree gchar *magnet_hash = NULL;

		if (strlen(magnet_text) <= AXIS_DOWNLOAD_MAGNET_MAX && axis_download_magnet_parse(magnet_text, &magnet_hash, NULL)) {
			if (hash != NULL && g_ascii_strcasecmp(hash, magnet_hash) != 0) {
				/* Two hashes for one row: neither is believed. */
				sink->outcome->n_skipped++;
				sink_lost(sink, "one info-hash");
				return FATE_TWO_HASHES;
			}
			magnet = g_strdup(magnet_text);
			if (hash == NULL)
				hash = g_ascii_strdown(magnet_hash, -1);
		}
	}
	if (magnet == NULL && hash != NULL)
		magnet = axis_sites_magnet(hash, name);
	if (download != NULL)
		fetch = download_path(&search->site, page_path, download, &elsewhere);
	if (magnet == NULL && fetch == NULL) {
		if (elsewhere) {
			sink->outcome->n_off_origin++;
			sink->left_out++;
			return FATE_ELSEWHERE;
		}
		sink->outcome->n_unusable++;
		sink_lost(sink, "an info-hash or a download on the site");
		return FATE_UNUSABLE;
	}
	release = axis_indexer_release_new();
	release->name = g_steal_pointer(&name);
	release->protocol = AXIS_DOWNLOAD_TORRENT;
	release->info_hash = g_steal_pointer(&hash);
	release->has_magnet = magnet != NULL;
	release->size_bytes = axis_def_parse_size(result(search, "size"));
	number = axis_def_parse_count(result(search, "seeders"));
	release->seeders = number >= 0 && number <= COUNT_MAX ? (gint)number : -1;
	number = axis_def_parse_count(result(search, "leechers"));
	release->leechers = number >= 0 && number <= COUNT_MAX ? (gint)number : -1;
	number = axis_def_parse_count(result(search, "grabs"));
	release->grabs = number >= 0 && number <= COUNT_MAX ? (gint)number : -1;
	release->published_us = axis_def_parse_date(result(search, "date"), search->site.now_us);
	if (release->published_us != 0 && (release->published_us < DATE_MIN_US ||
	    release->published_us > search->site.now_us + FUTURE_SLACK_US))
		release->published_us = 0;
	release->imdb_id = imdb_of(result(search, "imdbid") != NULL ? result(search, "imdbid") : result(search, "imdb"));
	number = axis_def_parse_count(result(search, "tmdbid"));
	release->tmdb_id = number > 0 && number <= COUNT_MAX ? number : 0;
	number = axis_def_parse_count(result(search, "tvdbid"));
	release->tvdb_id = number > 0 && number <= COUNT_MAX ? number : 0;
	row_categories(def, result(search, "category"), result(search, "categorydesc"), release);
	/* Software, music, books and games are no release of a film or an
	 * episode. */
	if (not_video(release)) {
		sink->left_out++;
		sink->not_video++;
		if (why != NULL)
			*why = category_words(release->categories[0]);
		return FATE_NOT_VIDEO;
	}
	key = g_strdup_printf("def|%s", release->info_hash != NULL ? release->info_hash : fetch != NULL ? fetch : magnet);
	found = g_new0(AxisIndexerFound, 1);
	found->merge_key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
	if (g_hash_table_contains(sink->seen, found->merge_key)) {
		axis_indexer_found_free(found);
		sink->left_out++;
		return FATE_TWICE;
	}
	g_hash_table_add(sink->seen, g_strdup(found->merge_key));
	found->magnet = g_steal_pointer(&magnet);
	found->fetch_path = g_steal_pointer(&fetch);
	found->release = g_steal_pointer(&release);
	g_ptr_array_add(sink->outcome->items, found);
	sink->kept++;
	return FATE_KEPT;
}

/* What a trace says a field is read by. */
static gchar *
how_read(
	const AxisDefBlock *block,
	gboolean            json
){
	GString *how = g_string_new(NULL);

	if (block->text != NULL)
		g_string_append_printf(how, "text %s", block->text);
	else if (block->selector != NULL && block->attribute != NULL)
		g_string_append_printf(how, "attribute %s of %s", block->attribute, block->selector);
	else if (block->selector != NULL)
		g_string_append_printf(how, "%s %s", json ? "path" : "selector", block->selector);
	else
		g_string_append(how, "nothing");
	if (block->case_keys->len > 0)
		g_string_append(how, ", by cases");
	if (block->filters->len > 0)
		g_string_append_printf(how, ", %u filter%s", block->filters->len, block->filters->len == 1 ? "" : "s");
	return g_string_free(how, FALSE);
}

/* @text, as far as a little past the excerpt's length, cut between
 * characters: a string of megabytes is not copied whole to be cut. */
static void
append_upto(
	GString     *out,
	const gchar *text
){
	gsize limit = AXIS_DEF_TRACE_EXCERPT_BYTES + 8;
	gsize room = out->len < limit ? limit - out->len : 0;
	gsize n = strnlen(text, room);

	if (text[n] != '\0') {
		while (n > 0 && ((guchar)text[n] & 0xc0) == 0x80)
			n--;
	}
	g_string_append_len(out, text, (gssize)n);
}

/* A JSON value written out for a person, cut: objects and lists to a depth of
 * three, then "...". */
static void
json_excerpt(
	GString            *out,
	const AxisJsonNode *node,
	guint               depth
){
	guint i;

	if (out->len > AXIS_DEF_TRACE_EXCERPT_BYTES)
		return;
	switch (axis_json_type(node)) {
	case AXIS_JSON_OBJECT:
		if (depth >= 3) {
			g_string_append(out, "{...}");
			return;
		}
		g_string_append_c(out, '{');
		for (i = 0; i < axis_json_length(node) && out->len <= AXIS_DEF_TRACE_EXCERPT_BYTES; i++) {
			g_string_append(out, i > 0 ? ", \"" : "\"");
			append_upto(out, axis_json_member_name(node, i));
			g_string_append(out, "\": ");
			json_excerpt(out, axis_json_member_value(node, i), depth + 1);
		}
		g_string_append_c(out, '}');
		return;
	case AXIS_JSON_ARRAY:
		if (depth >= 3) {
			g_string_append(out, "[...]");
			return;
		}
		g_string_append_c(out, '[');
		for (i = 0; i < axis_json_length(node) && out->len <= AXIS_DEF_TRACE_EXCERPT_BYTES; i++) {
			if (i > 0)
				g_string_append(out, ", ");
			json_excerpt(out, axis_json_element(node, i), depth + 1);
		}
		g_string_append_c(out, ']');
		return;
	case AXIS_JSON_STRING:
		g_string_append_c(out, '"');
		append_upto(out, axis_json_string(node));
		g_string_append_c(out, '"');
		return;
	default: {
		const gchar *text = axis_json_scalar_text(node);

		append_upto(out, text != NULL ? text : "null");
		return;
	}
	}
}

/* An excerpt of a row for a trace: its markup, or its JSON. */
static gchar *
row_excerpt(const Row *row)
{
	g_autofree gchar *text = NULL;

	if (row->element != NULL) {
		text = axis_html_markup(row->element, AXIS_DEF_TRACE_EXCERPT_BYTES * 2);
	} else {
		GString *out = g_string_new(NULL);

		json_excerpt(out, row->json, 0);
		text = g_string_free(out, FALSE);
	}
	return axis_def_trace_text(text, AXIS_DEF_TRACE_EXCERPT_BYTES);
}

/* Reads one row's fields in order, each seeing those before it. */
static gboolean
read_row(
	AxisDefSearch       *search,
	const Row           *row,
	const AxisJsonNode  *json_root,
	const gchar         *page_path,
	AxisSelectBudget    *budget,
	Sink                *sink,
	gchar              **problem
){
	const AxisDef *def = search->site.def;
	AxisDefTraceRow *traced;
	const gchar *why = NULL;
	Fate fate;
	guint i;

	clear_results(search);
	sink->scanned++;
	traced = axis_def_trace_add_row(sink->answer, sink->scanned);
	if (traced != NULL && sink->excerpts)
		traced->excerpt = row_excerpt(row);
	for (i = 0; i < def->fields->len; i++) {
		const AxisDefBlock *block = g_ptr_array_index(def->fields, i);
		g_autofree gchar *value = NULL;
		g_autofree gchar *raw = NULL;
		g_autofree gchar *field_problem = NULL;
		g_autofree gchar *key = NULL;
		gboolean defaulted = FALSE;
		ReadResult got = block_value_seen(search, block, row, json_root, budget, &value, &raw, &defaulted,
			&field_problem);

		if (traced != NULL) {
			g_autofree gchar *how = how_read(block, row->json != NULL);
			g_autofree gchar *note = got == READ_FAILED ? g_strdup_printf("failed: %s", field_problem != NULL ?
				field_problem : "it could not be read") : got == READ_MISSING ? g_strdup("matched nothing") :
				defaulted ? g_strdup("matched nothing: optional, so its default") : NULL;

			axis_def_trace_add_field(traced, block->name, how, raw, got == READ_OK ? value : NULL, note);
		}
		if (got == READ_FAILED) {
			g_autofree gchar *what = g_strdup_printf("the field %.64s", block->name);

			/* What cannot go on (a budget spent) ends the answer; a field
			 * that fails loses its row. */
			if (budget->exhausted || axis_def_regex_cache_exhausted(search->regex)) {
				set_problem(problem, budget->exhausted ? "reading it took more steps than the limit" :
					field_problem != NULL ? field_problem : "a regular expression went past its limits");
				return FALSE;
			}
			sink->outcome->n_skipped++;
			sink_lost(sink, what);
			if (traced != NULL)
				traced->fate = g_strdup_printf("left out: the field %.64s could not be read", block->name);
			return TRUE;
		}
		if (got == READ_MISSING) {
			g_autofree gchar *what = g_strdup_printf("the field %.64s", block->name);

			sink->outcome->n_skipped++;
			sink_lost(sink, what);
			if (traced != NULL)
				traced->fate = g_strdup_printf("left out: the field %.64s matched nothing, and it must be there",
					block->name);
			return TRUE;
		}
		key = g_strconcat("Result.", block->name, NULL);
		g_hash_table_replace(search->vars.values, g_steal_pointer(&key), g_steal_pointer(&value));
	}
	fate = keep_row(search, page_path, sink, &why);
	if (traced != NULL) {
		switch (fate) {
		case FATE_KEPT:
			traced->fate = g_strdup("kept");
			break;
		case FATE_NO_TITLE:
			traced->fate = g_strdup("left out: it has no title");
			break;
		case FATE_NOT_EVERY_WORD:
			traced->fate = g_strdup("left out: its name lacks one of the words searched for (andmatch)");
			break;
		case FATE_TWO_HASHES:
			traced->fate = g_strdup("left out: its magnet link and its info-hash name two torrents");
			break;
		case FATE_ELSEWHERE:
			traced->fate = g_strdup("left out: its download is only at another address, which Axis does not fetch");
			break;
		case FATE_UNUSABLE:
			traced->fate = g_strdup("left out: it has neither an info-hash nor a download on the site");
			break;
		case FATE_NOT_VIDEO:
			traced->fate = g_strdup_printf("left out: its category is not video (%s)", why != NULL ? why : "Other");
			break;
		default:
			traced->fate = g_strdup("left out: it is listed twice");
			break;
		}
	}
	return TRUE;
}

static gboolean
sink_full(Sink *sink)
{
	if (sink->scanned >= AXIS_INDEXER_MAX_SCAN || sink->outcome->items->len >= AXIS_INDEXER_MAX_RESULTS) {
		sink->outcome->truncated = TRUE;
		return TRUE;
	}
	return FALSE;
}

/* The site's error, when an error block's selector matches the answer. */
static gboolean
site_error(
	AxisDefSearch       *search,
	AxisHtmlNode        *root,
	AxisSelectBudget    *budget,
	gchar              **problem
){
	const AxisDef *def = search->site.def;
	guint i;

	for (i = 0; i < def->errors->len; i++) {
		const AxisDefError *error = g_ptr_array_index(def->errors, i);
		AxisSelector *selector = selector_of(search, error->selector, NULL);
		const AxisHtmlNode *match = selector != NULL ? axis_selector_first(selector, root, budget) : NULL;
		g_autofree gchar *message = NULL;
		g_autofree gchar *clean = NULL;
		Row row;

		if (match == NULL)
			continue;
		row.element = match;
		row.json = NULL;
		if (error->message != NULL)
			block_value(search, error->message, &row, NULL, budget, &message, NULL);
		if (message == NULL)
			message = axis_html_text(match, 600);
		clean = axis_indexer_text_clean(message, 200);
		{
			g_autofree gchar *what = g_strdup_printf("the site answered with an error: %s",
				clean != NULL ? clean : "(it says nothing more)");

			set_problem(problem, what);
		}
		return TRUE;
	}
	return FALSE;
}

/* How reading an answer's whole went. */
typedef enum
{
	ANSWER_OK,
	ANSWER_UNREADABLE,	/* not the HTML, XML or JSON it should be */
	ANSWER_REFUSED	/* the site's own error, a limit, a selector that does not read */
} AnswerRead;

/* An excerpt of a page for a trace: its body's markup (an XML feed's whole),
 * cut. */
static gchar *
page_excerpt(AxisHtmlNode *root)
{
	g_autoptr(AxisSelector) body = axis_selector_parse("body", NULL, NULL);
	AxisSelectBudget budget;
	const AxisHtmlNode *from;
	g_autofree gchar *text = NULL;

	memset(&budget, 0, sizeof(budget));
	budget.max_steps = 100000;
	from = body != NULL ? axis_selector_first(body, root, &budget) : NULL;
	/* More than is kept, so that a cut one says so ("..."). */
	text = axis_html_markup(from != NULL ? from : root, AXIS_DEF_TRACE_EXCERPT_BYTES * 2);
	return axis_def_trace_text(text, AXIS_DEF_TRACE_EXCERPT_BYTES);
}

static AnswerRead
read_markup(
	AxisDefSearch       *search,
	const gchar         *text,
	gboolean             xml,
	const gchar         *page_path,
	Sink                *sink,
	gboolean            *expect_found,
	guint               *rows_matched,
	gchar              **problem
){
	const AxisDef *def = search->site.def;
	g_autoptr(AxisHtmlDoc) doc = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *why = NULL;
	AxisSelectBudget budget;
	AxisSelector *selector;
	AxisHtmlNode *root;
	guint step;
	guint i;

	doc = axis_html_parse(text, strlen(text), xml, &why);
	if (doc == NULL) {
		set_problem(problem, why != NULL ? why : "the page cannot be read");
		return ANSWER_UNREADABLE;
	}
	root = axis_html_doc_root(doc);
	memset(&budget, 0, sizeof(budget));
	budget.max_steps = AXIS_DEF_SELECT_STEPS;
	if (site_error(search, root, &budget, problem))
		return ANSWER_REFUSED;
	/* What every answer has, with results or without. */
	if (def->rows_expect != NULL) {
		AxisSelector *expect = selector_of(search, def->rows_expect, problem);

		if (expect == NULL)
			return ANSWER_REFUSED;
		*expect_found = axis_selector_first(expect, root, &budget) != NULL;
	}
	selector = selector_of(search, def->rows_selector, problem);
	if (selector == NULL)
		return ANSWER_REFUSED;
	rows = axis_selector_all(selector, root, AXIS_INDEXER_MAX_SCAN * (guint)(def->rows_after + 1), &budget);
	step = (guint)def->rows_after + 1;
	*rows_matched = (rows->len + step - 1) / step;
	if (rows->len == 0 && sink->answer != NULL && sink->excerpts)
		sink->answer->excerpt = page_excerpt(root);
	for (i = 0; i < rows->len && !sink_full(sink); i += step) {
		AxisHtmlNode *element = g_ptr_array_index(rows, i);
		Row row;
		guint k;

		/* rows.after: the rows after it are part of it. */
		for (k = 1; k < step && i + k < rows->len; k++)
			axis_html_move_children(g_ptr_array_index(rows, i + k), element);
		row.element = element;
		row.json = NULL;
		if (!read_row(search, &row, NULL, page_path, &budget, sink, problem))
			return ANSWER_REFUSED;
	}
	if (budget.exhausted) {
		set_problem(problem, "reading it took more steps than the limit");
		return ANSWER_REFUSED;
	}
	return ANSWER_OK;
}

static AnswerRead
read_json(
	AxisDefSearch       *search,
	const gchar         *text,
	const gchar         *page_path,
	Sink                *sink,
	gboolean            *expect_found,
	guint               *rows_matched,
	gchar              **problem
){
	const AxisDef *def = search->site.def;
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *rows_path = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *rows;
	AxisSelectBudget budget;
	guint i;

	doc = axis_json_parse(text, strlen(text), &why);
	if (doc == NULL) {
		set_problem(problem, why != NULL ? why : "the answer is not JSON");
		return ANSWER_UNREADABLE;
	}
	root = axis_json_doc_get_root(doc);
	if (def->rows_expect != NULL) {
		g_autofree gchar *expect = render(search, def->rows_expect, NULL, problem);

		if (expect == NULL)
			return ANSWER_REFUSED;
		*expect_found = json_at(root, root, expect) != NULL;
	}
	rows_path = render(search, def->rows_selector, NULL, problem);
	if (rows_path == NULL)
		return ANSWER_REFUSED;
	rows = json_at(root, root, rows_path);
	memset(&budget, 0, sizeof(budget));
	budget.max_steps = AXIS_DEF_SELECT_STEPS;
	if (axis_json_type(rows) == AXIS_JSON_OBJECT) {
		Row row;

		*rows_matched = 1;
		row.element = NULL;
		row.json = rows;
		return read_row(search, &row, root, page_path, &budget, sink, problem) ? ANSWER_OK : ANSWER_REFUSED;
	}
	*rows_matched = axis_json_type(rows) == AXIS_JSON_ARRAY ? axis_json_length(rows) : 0;
	if (*rows_matched == 0 && sink->answer != NULL && sink->excerpts) {
		GString *out = g_string_new(NULL);
		g_autofree gchar *excerpt = NULL;

		json_excerpt(out, root, 0);
		excerpt = g_string_free(out, FALSE);
		sink->answer->excerpt = axis_def_trace_text(excerpt, AXIS_DEF_TRACE_EXCERPT_BYTES);
	}
	for (i = 0; i < axis_json_length(rows) && axis_json_type(rows) == AXIS_JSON_ARRAY && !sink_full(sink); i++) {
		Row row;

		row.element = NULL;
		row.json = axis_json_element(rows, i);
		if (axis_json_type(row.json) != AXIS_JSON_OBJECT) {
			sink->scanned++;
			sink->outcome->n_skipped++;
			sink_lost(sink, "a result that is an object");
			continue;
		}
		if (!read_row(search, &row, root, page_path, &budget, sink, problem))
			return ANSWER_REFUSED;
	}
	return ANSWER_OK;
}

/* What most rows were lost for. */
static const gchar *
most_lost_for(GHashTable *lost_by)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	const gchar *most = "a field every result must have";
	guint best = 0;

	g_hash_table_iter_init(&iter, lost_by);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (GPOINTER_TO_UINT(value) > best || (GPOINTER_TO_UINT(value) == best && strcmp(key, most) < 0)) {
			best = GPOINTER_TO_UINT(value);
			most = key;
		}
	}
	return most;
}

/* Whether @text loads Cloudflare's challenge orchestration: an address under
 * "/cdn-cgi/challenge-platform/" whose path goes on through "/orchestrate/"
 * (".../h/b/orchestrate/chl_page/v1", ".../h/g/orchestrate/managed/v1"). */
static gboolean
loads_orchestration(const gchar *text)
{
	static const gchar platform[] = "/cdn-cgi/challenge-platform/";
	const gchar *at = text;

	while ((at = strstr(at, platform)) != NULL) {
		const gchar *path = at + sizeof(platform) - 1;
		gsize span = strcspn(path, "\"' \t\r\n<>?");

		if (g_strstr_len(path, (gssize)MIN(span, 64), "/orchestrate/") != NULL ||
		    (span >= 12 && strncmp(path, "orchestrate/", 12) == 0))
			return TRUE;
		at = path;
	}
	return FALSE;
}

gboolean
axis_def_is_challenge(const gchar *text)
{
	/* Only what a challenge's page has: Cloudflare's challenge options
	 * object, its orchestration script, and the titles of Cloudflare's and
	 * DDoS-Guard's challenge pages.  Not "/cdn-cgi/challenge-platform/" (nor
	 * ".../h/"): Cloudflare puts its detection script into the ordinary pages
	 * of the sites it fronts, as ".../scripts/jsd/main.js" and as
	 * ".../h/b/scripts/jsd/<id>/main.js" (seen on 2026-11-18); nor a link to
	 * "ddos-guard.net", which an ordinary page may carry. */
	static const gchar *const markers[] = {
		"cf_chl_opt", "cf-browser-verification", "<title>Just a moment...</title>",
		"<title>Attention Required! | Cloudflare</title>", "<title>DDoS-Guard</title>", NULL
	};
	guint i;

	if (text == NULL)
		return FALSE;
	for (i = 0; markers[i] != NULL; i++) {
		if (strstr(text, markers[i]) != NULL)
			return TRUE;
	}
	return loads_orchestration(text);
}

/* The verdict on an answer read whole, and why when it is not read. */
static AxisDefVerdict
judge(
	const AxisDef  *def,
	AnswerRead      got,
	gboolean        expect_found,
	guint           rows_matched,
	const Sink     *sink,
	gchar         **problem
){
	g_autofree gchar *why = NULL;

	if (got == ANSWER_REFUSED)
		return AXIS_DEF_ANSWER_REFUSED;
	if (got == ANSWER_UNREADABLE) {
		/* Not the page or JSON the definition reads: when the definition
		 * says what every answer has, the site has changed (or the address is
		 * someone else's); else it is only unreadable, as before. */
		if (def->rows_expect == NULL)
			return AXIS_DEF_ANSWER_REFUSED;
		if (problem != NULL) {
			why = *problem;
			*problem = g_strdup_printf("%s: the answer is not what its definition reads (%s)", AXIS_DEF_DRIFT_PROBLEM,
				why != NULL ? why : "it cannot be read");
		}
		return AXIS_DEF_ANSWER_DRIFT;
	}
	if (def->rows_expect != NULL && !expect_found) {
		set_problem(problem, AXIS_DEF_DRIFT_PROBLEM ": the part every answer of the site has is missing");
		return AXIS_DEF_ANSWER_DRIFT;
	}
	if (rows_matched >= 2 && sink->lost * 2 > rows_matched) {
		why = g_strdup_printf("%s: %u of the %u results it listed lack %s", AXIS_DEF_DRIFT_PROBLEM, sink->lost,
			rows_matched, most_lost_for(sink->lost_by));
		set_problem(problem, why);
		return AXIS_DEF_ANSWER_DRIFT;
	}
	return AXIS_DEF_ANSWER_READ;
}

gboolean
axis_def_search_read(
	AxisDefSearch       *search,
	guint                index,
	const gchar         *body,
	gsize                length,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	gchar              **problem
){
	return axis_def_search_read_full(search, index, body, length, seen, outcome, NULL, problem);
}

gboolean
axis_def_search_read_full(
	AxisDefSearch       *search,
	guint                index,
	const gchar         *body,
	gsize                length,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisDefAnswerInfo   *info,
	gchar              **problem
){
	const AxisDefRequest *request = axis_def_search_request(search, index);
	const AxisDef *def = search->site.def;
	g_autoptr(GHashTable) own = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *page_path = NULL;
	AxisDefAnswerInfo own_info;
	AxisDefTraceAnswer *answer = NULL;
	AxisIndexerOutcome part;
	AnswerRead got;
	gboolean expect_found = FALSE;
	guint rows_matched = 0;
	Sink sink;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (info == NULL)
		info = &own_info;
	memset(info, 0, sizeof(*info));
	info->verdict = AXIS_DEF_ANSWER_REFUSED;
	if (request == NULL) {
		set_problem(problem, "there is no such request");
		return FALSE;
	}
	answer = axis_def_trace_add_answer(search->trace, request->path, length);
	info->answer = answer;
	if (answer != NULL) {
		g_free(answer->kind);
		answer->kind = g_strdup(request->response == AXIS_DEF_JSON ? "JSON" : request->response == AXIS_DEF_XML ?
			"XML" : "HTML");
		answer->expect = def->rows_expect != NULL ? axis_def_trace_text(def->rows_expect, 200) : NULL;
		answer->rows_selector = axis_def_trace_text(def->rows_selector, 200);
	}
	if (length > AXIS_HTML_MAX_BYTES) {
		set_problem(problem, "the answer is larger than the limit");
		if (answer != NULL)
			answer->verdict = g_strdup("refused: it is larger than the limit");
		return FALSE;
	}
	text = from_site(body != NULL ? body : "", body != NULL ? length : 0, def->encoding);
	/* A browser challenge in place of the page: stage 3's, not built.  In
	 * place of JSON only when it is markup (a name in JSON may say "Just a
	 * moment..."). */
	if ((request->response != AXIS_DEF_JSON || text[strspn(text, " \t\r\n")] == '<') && axis_def_is_challenge(text)) {
		set_problem(problem, AXIS_DEF_CHALLENGE_PROBLEM);
		info->verdict = AXIS_DEF_ANSWER_CHALLENGE;
		if (answer != NULL) {
			answer->challenge = TRUE;
			answer->verdict = g_strdup("a browser challenge, not the site's page");
		}
		return FALSE;
	}
	/* The page's own path, without its query, for resolving addresses. */
	page_path = g_strdup(request->path);
	if (strchr(page_path, '?') != NULL)
		*strchr(page_path, '?') = '\0';
	if (seen == NULL)
		own = seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	/* Read into a part, so that a refused answer adds nothing. */
	axis_indexer_outcome_init(&part);
	part.items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	memset(&sink, 0, sizeof(sink));
	sink.outcome = &part;
	sink.seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sink.lost_by = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sink.answer = answer;
	sink.excerpts = search->trace != NULL && search->trace->excerpts;
	{
		GHashTableIter iter;
		gpointer key;

		g_hash_table_iter_init(&iter, seen);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_hash_table_add(sink.seen, g_strdup(key));
	}
	if (request->response == AXIS_DEF_JSON)
		got = read_json(search, text, page_path, &sink, &expect_found, &rows_matched, problem);
	else
		got = read_markup(search, text, request->response == AXIS_DEF_XML, page_path, &sink, &expect_found,
			&rows_matched, problem);
	clear_results(search);
	info->verdict = judge(def, got, expect_found, rows_matched, &sink, problem);
	info->rows_matched = rows_matched;
	info->kept = sink.kept;
	info->lost = sink.lost;
	info->not_video = sink.not_video;
	if (answer != NULL) {
		answer->expect_found = expect_found;
		answer->rows_matched = rows_matched;
		answer->kept = sink.kept;
		answer->lost = sink.lost;
		answer->left_out = sink.left_out;
		answer->not_video = sink.not_video;
		if (info->verdict == AXIS_DEF_ANSWER_READ)
			answer->verdict = rows_matched == 0 ? g_strdup(def->rows_expect != NULL ? "no results (the part every "
				"answer has is there, so the site found nothing)" : "no results (its definition names no part "
				"every answer has, so this cannot be told from a page that changed)") :
				g_strdup_printf("read: %u release%s kept", sink.kept, sink.kept == 1 ? "" : "s");
		else
			answer->verdict = g_strdup_printf("%s%s", info->verdict == AXIS_DEF_ANSWER_DRIFT ? "" : "refused: ",
				problem != NULL && *problem != NULL ? *problem : "it could not be read");
	}
	g_hash_table_unref(sink.lost_by);
	if (info->verdict != AXIS_DEF_ANSWER_READ) {
		g_hash_table_unref(sink.seen);
		axis_indexer_outcome_clear(&part);
		return FALSE;
	}
	if (outcome->items == NULL)
		outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	for (i = 0; i < part.items->len && outcome->items->len < AXIS_INDEXER_MAX_RESULTS; i++) {
		AxisIndexerFound *found = g_ptr_array_index(part.items, i);

		g_hash_table_add(seen, g_strdup(found->merge_key));
		g_ptr_array_add(outcome->items, found);
		g_ptr_array_index(part.items, i) = NULL;
	}
	if (i < part.items->len)
		outcome->truncated = TRUE;
	outcome->n_off_origin += part.n_off_origin;
	outcome->n_unusable += part.n_unusable;
	outcome->n_skipped += part.n_skipped;
	outcome->truncated = outcome->truncated || part.truncated;
	g_hash_table_unref(sink.seen);
	/* What was moved is NULL in the part now; the rest is freed. */
	axis_indexer_outcome_clear(&part);
	return TRUE;
}

/* ── The download page ──────────────────────────────────────────── */

gboolean
axis_def_download_is_page(const AxisDef *def)
{
	return def->download_selectors->len > 0 || def->download_hash != NULL;
}

gboolean
axis_def_read_download(
	const AxisDefSite  *site,
	const gchar        *page_path,
	const gchar        *body,
	gsize               length,
	gchar             **magnet,
	gchar             **file_path,
	gchar             **problem
){
	g_autoptr(AxisDefSearch) search = NULL;
	g_autoptr(AxisHtmlDoc) doc = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *path_only = NULL;
	AxisIndexerRequest request;
	AxisSelectBudget budget;
	AxisHtmlNode *root;
	gboolean elsewhere = FALSE;
	Row row;
	guint i;

	*magnet = NULL;
	*file_path = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (length > AXIS_HTML_MAX_BYTES) {
		set_problem(problem, "the release's page is larger than the limit");
		return FALSE;
	}
	/* A search of nothing, for the variables (the settings, sitelink). */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	search = g_new0(AxisDefSearch, 1);
	search->site = *site;
	search->regex = axis_def_regex_cache_new(AXIS_DEF_REGEX_BUDGET_US);
	search->selectors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)axis_selector_free);
	search->templates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)axis_template_free);
	if (!setup_vars(search, &request, problem))
		return FALSE;
	text = from_site(body != NULL ? body : "", body != NULL ? length : 0, site->def->encoding);
	if (axis_def_is_challenge(text)) {
		set_problem(problem, AXIS_DEF_CHALLENGE_PROBLEM);
		return FALSE;
	}
	doc = axis_html_parse(text, strlen(text), FALSE, &why);
	if (doc == NULL) {
		set_problem(problem, why != NULL ? why : "the release's page cannot be read");
		return FALSE;
	}
	root = axis_html_doc_root(doc);
	memset(&budget, 0, sizeof(budget));
	budget.max_steps = AXIS_DEF_SELECT_STEPS;
	path_only = g_strdup(page_path);
	if (strchr(path_only, '?') != NULL)
		*strchr(path_only, '?') = '\0';
	row.element = root;
	row.json = NULL;
	for (i = 0; i < site->def->download_selectors->len; i++) {
		const AxisDefBlock *block = g_ptr_array_index(site->def->download_selectors, i);
		g_autofree gchar *value = NULL;
		gboolean off = FALSE;

		if (block_value(search, block, &row, NULL, &budget, &value, NULL) != READ_OK || value[0] == '\0')
			continue;
		if (g_ascii_strncasecmp(value, "magnet:", 7) == 0) {
			if (strlen(value) <= AXIS_DOWNLOAD_MAGNET_MAX && axis_download_magnet_parse(value, NULL, NULL)) {
				*magnet = g_steal_pointer(&value);
				return TRUE;
			}
			continue;
		}
		*file_path = download_path(site, path_only, value, &off);
		if (*file_path != NULL)
			return TRUE;
		elsewhere = elsewhere || off;
	}
	if (site->def->download_hash != NULL) {
		g_autofree gchar *hash_text = NULL;
		g_autofree gchar *title = NULL;
		g_autofree gchar *hash = NULL;

		if (block_value(search, site->def->download_hash, &row, NULL, &budget, &hash_text, NULL) == READ_OK &&
		    axis_sites_hash_read(hash_text, &hash)) {
			if (site->def->download_title != NULL)
				block_value(search, site->def->download_title, &row, NULL, &budget, &title, NULL);
			*magnet = axis_sites_magnet(hash, title);
			if (*magnet != NULL)
				return TRUE;
		}
	}
	set_problem(problem, elsewhere ? "the release's page gives its download only at another address, which Axis "
		"does not fetch" : "the release's page has no download the definition can read (the site may have "
		"changed its page)");
	return FALSE;
}
