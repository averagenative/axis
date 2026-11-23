/*
 * axis-def.c - reading and judging an indexer definition
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The YAML tree is read into the model key by key.  Whatever key the model
 * does not know is a need ("the definition key X"), so that a definition
 * written for a newer engine is not used half.  Then every selector,
 * template and regular expression is read once, as the engine will read
 * it, and every filter looked up: one that fails is a need too.
 */

#include "indexer/def/axis-def.h"

#include <string.h>

#include "core/axis-http-origin.h"
#include "indexer/def/axis-def-filter.h"
#include "indexer/def/axis-def-regex.h"
#include "indexer/def/axis-def-select.h"
#include "indexer/def/axis-def-template.h"
#include "indexer/def/axis-def-yaml.h"

/* ── Newznab categories ─────────────────────────────────────────── */

static const struct
{
	const gchar *name;
	gint         number;
} newznab[] = {
	{ "Console", 1000 }, { "Console/NDS", 1010 }, { "Console/PSP", 1020 }, { "Console/Wii", 1030 },
	{ "Console/XBox", 1040 }, { "Console/XBox 360", 1050 }, { "Console/Wiiware", 1060 },
	{ "Console/XBox 360 DLC", 1070 }, { "Console/PS3", 1080 }, { "Console/Other", 1090 }, { "Console/3DS", 1110 },
	{ "Console/PS Vita", 1120 }, { "Console/WiiU", 1130 }, { "Console/XBox One", 1140 }, { "Console/PS4", 1180 },
	{ "Movies", 2000 }, { "Movies/Foreign", 2010 }, { "Movies/Other", 2020 }, { "Movies/SD", 2030 },
	{ "Movies/HD", 2040 }, { "Movies/UHD", 2045 }, { "Movies/BluRay", 2050 }, { "Movies/3D", 2060 },
	{ "Movies/DVD", 2070 }, { "Movies/WEB-DL", 2080 },
	{ "Audio", 3000 }, { "Audio/MP3", 3010 }, { "Audio/Video", 3020 }, { "Audio/Audiobook", 3030 },
	{ "Audio/Lossless", 3040 }, { "Audio/Other", 3050 }, { "Audio/Foreign", 3060 },
	{ "PC", 4000 }, { "PC/0day", 4010 }, { "PC/ISO", 4020 }, { "PC/Mac", 4030 }, { "PC/Mobile-Other", 4040 },
	{ "PC/Games", 4050 }, { "PC/Mobile-iOS", 4060 }, { "PC/Mobile-Android", 4070 },
	{ "TV", 5000 }, { "TV/WEB-DL", 5010 }, { "TV/Foreign", 5020 }, { "TV/SD", 5030 }, { "TV/HD", 5040 },
	{ "TV/UHD", 5045 }, { "TV/Other", 5050 }, { "TV/Sport", 5060 }, { "TV/Anime", 5070 },
	{ "TV/Documentary", 5080 },
	{ "XXX", 6000 }, { "XXX/DVD", 6010 }, { "XXX/WMV", 6020 }, { "XXX/XviD", 6030 }, { "XXX/x264", 6040 },
	{ "XXX/UHD", 6045 }, { "XXX/Pack", 6050 }, { "XXX/ImageSet", 6060 }, { "XXX/Other", 6070 }, { "XXX/SD", 6080 },
	{ "XXX/WEB-DL", 6090 },
	{ "Books", 7000 }, { "Books/Mags", 7010 }, { "Books/EBook", 7020 }, { "Books/Comics", 7030 },
	{ "Books/Technical", 7040 }, { "Books/Other", 7050 }, { "Books/Foreign", 7060 },
	{ "Other", 8000 }, { "Other/Misc", 8010 }, { "Other/Hashed", 8020 }
};

gint
axis_def_newznab_category(const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(newznab); i++) {
		if (g_ascii_strcasecmp(newznab[i].name, name) == 0)
			return newznab[i].number;
	}
	return 0;
}

/* ── Freeing ────────────────────────────────────────────────────── */

static void
filter_free(AxisDefFilter *filter)
{
	g_free(filter->name);
	g_strfreev(filter->args);
	g_free(filter);
}

static void
block_free(AxisDefBlock *block)
{
	if (block == NULL)
		return;
	g_free(block->name);
	g_free(block->selector);
	g_free(block->text);
	g_free(block->attribute);
	g_free(block->default_text);
	g_free(block->remove);
	g_ptr_array_unref(block->case_keys);
	g_ptr_array_unref(block->case_values);
	g_ptr_array_unref(block->filters);
	g_free(block);
}

static void
pair_free(AxisDefPair *pair)
{
	g_free(pair->name);
	g_free(pair->value);
	g_free(pair);
}

static void
path_free(AxisDefPath *path)
{
	g_free(path->path);
	g_ptr_array_unref(path->inputs);
	g_clear_pointer(&path->categories, g_ptr_array_unref);
	g_free(path->query_separator);
	g_free(path);
}

static void
category_free(AxisDefCategory *category)
{
	g_free(category->site_id);
	g_free(category->desc);
	g_free(category);
}

static void
setting_free(AxisDefSetting *setting)
{
	g_free(setting->name);
	g_free(setting->type);
	g_free(setting->label);
	g_free(setting->value);
	g_free(setting);
}

static void
error_free(AxisDefError *error)
{
	g_free(error->selector);
	block_free(error->message);
	g_free(error);
}

static void
need_free(AxisDefNeed *need)
{
	g_free(need->what);
	g_free(need);
}

void
axis_def_free(AxisDef *def)
{
	if (def == NULL)
		return;
	g_free(def->id);
	g_free(def->name);
	g_free(def->description);
	g_free(def->language);
	g_free(def->site_type);
	g_free(def->encoding);
	g_ptr_array_unref(def->links);
	g_ptr_array_unref(def->legacy_links);
	g_ptr_array_unref(def->categories);
	g_ptr_array_unref(def->settings);
	g_ptr_array_unref(def->paths);
	g_ptr_array_unref(def->inputs);
	g_ptr_array_unref(def->headers);
	g_ptr_array_unref(def->keywords_filters);
	g_ptr_array_unref(def->errors);
	g_free(def->rows_selector);
	g_free(def->rows_expect);
	g_ptr_array_unref(def->fields);
	g_ptr_array_unref(def->download_selectors);
	block_free(def->download_hash);
	block_free(def->download_title);
	g_ptr_array_unref(def->needs);
	g_free(def);
}

static AxisDef *
def_new(void)
{
	AxisDef *def = g_new0(AxisDef, 1);

	def->links = g_ptr_array_new_with_free_func(g_free);
	def->legacy_links = g_ptr_array_new_with_free_func(g_free);
	def->categories = g_ptr_array_new_with_free_func((GDestroyNotify)category_free);
	def->settings = g_ptr_array_new_with_free_func((GDestroyNotify)setting_free);
	def->paths = g_ptr_array_new_with_free_func((GDestroyNotify)path_free);
	def->inputs = g_ptr_array_new_with_free_func((GDestroyNotify)pair_free);
	def->headers = g_ptr_array_new_with_free_func((GDestroyNotify)pair_free);
	def->keywords_filters = g_ptr_array_new_with_free_func((GDestroyNotify)filter_free);
	def->errors = g_ptr_array_new_with_free_func((GDestroyNotify)error_free);
	def->fields = g_ptr_array_new_with_free_func((GDestroyNotify)block_free);
	def->download_selectors = g_ptr_array_new_with_free_func((GDestroyNotify)block_free);
	def->needs = g_ptr_array_new_with_free_func((GDestroyNotify)need_free);
	return def;
}

/* ── Needs ──────────────────────────────────────────────────────── */

/* A need's words, which may hold a piece of the definition (a key, a
 * filter's name) cut anywhere: valid UTF-8, control characters as spaces,
 * at most 200 characters. */
static gchar *
need_text(const gchar *what)
{
	g_autofree gchar *valid = g_utf8_make_valid(what, -1);
	GString *out = g_string_new(NULL);
	const gchar *p;
	guint n = 0;

	for (p = valid; *p != '\0' && n < 200; p = g_utf8_next_char(p), n++) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0))
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

static void
add_need(
	AxisDef     *def,
	gboolean     never,
	const gchar *what
){
	g_autofree gchar *text = need_text(what);
	AxisDefNeed *need;
	guint i;

	for (i = 0; i < def->needs->len; i++) {
		need = g_ptr_array_index(def->needs, i);
		if (strcmp(need->what, text) == 0)
			return;
	}
	if (def->needs->len >= 32)
		return;
	need = g_new0(AxisDefNeed, 1);
	need->what = g_steal_pointer(&text);
	need->never = never;
	g_ptr_array_add(def->needs, need);
}

/* A reader's "the selector :eq() is not supported" as the need "the selector
 * :eq()"; other problems as they are. */
static void
add_need_from(
	AxisDef     *def,
	const gchar *problem,
	const gchar *fallback
){
	static const gchar *const suffixes[] = { " is not supported here", " is not supported", " are not supported",
		NULL };
	guint i;

	if (problem == NULL) {
		add_need(def, FALSE, fallback);
		return;
	}
	for (i = 0; suffixes[i] != NULL; i++) {
		if (g_str_has_suffix(problem, suffixes[i])) {
			g_autofree gchar *what = g_strndup(problem, strlen(problem) - strlen(suffixes[i]));

			add_need(def, FALSE, what);
			return;
		}
	}
	{
		g_autofree gchar *what = g_strdup_printf("%s (%s)", fallback, problem);

		add_need(def, FALSE, what);
	}
}

gboolean
axis_def_usable(const AxisDef *def)
{
	return def != NULL && def->needs->len == 0;
}

gchar *
axis_def_problem(const AxisDef *def)
{
	GString *text;
	guint i;
	guint later = 0;

	if (def == NULL || def->needs->len == 0)
		return NULL;
	text = g_string_new(NULL);
	for (i = 0; i < def->needs->len; i++) {
		const AxisDefNeed *need = g_ptr_array_index(def->needs, i);

		if (!need->never)
			continue;
		g_string_append_printf(text, "%s%s", text->len > 0 ? "; " : "", need->what);
	}
	for (i = 0; i < def->needs->len; i++) {
		const AxisDefNeed *need = g_ptr_array_index(def->needs, i);

		if (need->never)
			continue;
		if (later == 0)
			g_string_append_printf(text, "%sneeds ", text->len > 0 ? "; it also " : "");
		else
			g_string_append(text, ", ");
		g_string_append(text, need->what);
		later++;
		if (later >= 4 && i + 1 < def->needs->len) {
			g_string_append(text, " and more");
			break;
		}
	}
	if (text->len > 0) {
		text->str[0] = g_ascii_toupper(text->str[0]);
		g_string_append_c(text, '.');
	}
	return g_string_free(text, FALSE);
}

/* ── Reading values ─────────────────────────────────────────────── */

/* A scalar's text (a number or a boolean as written), or %NULL. */
static gchar *
text_of(const AxisYamlNode *node)
{
	const gchar *text = axis_yaml_text(node);

	return text != NULL ? g_strdup(text) : NULL;
}

static gboolean
bool_of(
	const AxisYamlNode *node,
	gboolean            fallback
){
	gboolean value = fallback;

	if (node != NULL && !axis_yaml_get_bool(node, &value))
		return fallback;
	return value;
}

/* Every key of @map not in @known is a need ("the definition key x in
 * search.rows"). */
static void
check_keys(
	AxisDef            *def,
	const AxisYamlNode *map,
	const gchar *const *known,
	const gchar        *where
){
	guint i;
	guint k;

	for (i = 0; i < axis_yaml_length(map) && map->kind == AXIS_YAML_MAP; i++) {
		const gchar *key = axis_yaml_key_at(map, i);
		gboolean found = FALSE;

		for (k = 0; known[k] != NULL && !found; k++)
			found = strcmp(known[k], key) == 0;
		if (!found) {
			g_autofree gchar *what = g_strdup_printf("the definition key %.40s in %s", key, where);

			add_need(def, FALSE, what);
		}
	}
}

/* A list of texts (a scalar is a list of one). */
static void
read_texts(
	const AxisYamlNode *node,
	GPtrArray          *into,
	guint               max
){
	guint i;

	if (node == NULL)
		return;
	if (node->kind == AXIS_YAML_SCALAR) {
		if (axis_yaml_text(node) != NULL)
			g_ptr_array_add(into, g_strdup(axis_yaml_text(node)));
		return;
	}
	for (i = 0; i < axis_yaml_length(node) && into->len < max; i++) {
		const gchar *text = axis_yaml_text(axis_yaml_at(node, i));

		if (text != NULL)
			g_ptr_array_add(into, g_strdup(text));
	}
}

static GPtrArray *
read_filters(
	AxisDef            *def,
	const AxisYamlNode *node
){
	static const gchar *const keys[] = { "name", "args", NULL };
	GPtrArray *filters = g_ptr_array_new_with_free_func((GDestroyNotify)filter_free);
	guint i;

	if (node == NULL)
		return filters;
	if (node->kind != AXIS_YAML_SEQ) {
		add_need(def, FALSE, "filters that are not a list");
		return filters;
	}
	for (i = 0; i < axis_yaml_length(node) && i < AXIS_DEF_MAX_FILTERS; i++) {
		const AxisYamlNode *item = axis_yaml_at(node, i);
		const AxisYamlNode *args = axis_yaml_get(item, "args");
		AxisDefFilter *filter;
		GPtrArray *texts = g_ptr_array_new();
		guint k;

		if (item->kind != AXIS_YAML_MAP || axis_yaml_text(axis_yaml_get(item, "name")) == NULL) {
			add_need(def, FALSE, "a filter without its name");
			g_ptr_array_unref(texts);
			continue;
		}
		check_keys(def, item, keys, "a filter");
		filter = g_new0(AxisDefFilter, 1);
		filter->name = text_of(axis_yaml_get(item, "name"));
		if (args != NULL && args->kind == AXIS_YAML_SEQ) {
			for (k = 0; k < axis_yaml_length(args) && k < 16; k++) {
				const AxisYamlNode *arg = axis_yaml_at(args, k);

				g_ptr_array_add(texts, g_strdup(arg->kind == AXIS_YAML_SCALAR ? arg->text : ""));
			}
		} else if (args != NULL && args->kind == AXIS_YAML_SCALAR && !axis_yaml_is_null(args)) {
			g_ptr_array_add(texts, g_strdup(args->text));
		} else if (args != NULL && args->kind == AXIS_YAML_MAP) {
			add_need(def, FALSE, "filter arguments that are a map");
		}
		g_ptr_array_add(texts, NULL);
		filter->args = (gchar **)g_ptr_array_free(texts, FALSE);
		if (!axis_def_filter_known(filter->name)) {
			g_autofree gchar *what = g_strdup_printf("the filter %.40s", filter->name);

			add_need(def, FALSE, what);
		}
		g_ptr_array_add(filters, filter);
	}
	return filters;
}

static AxisDefBlock *
read_block(
	AxisDef            *def,
	const gchar        *name,
	const AxisYamlNode *node,
	const gchar        *where
){
	static const gchar *const keys[] = { "selector", "attribute", "optional", "default", "case", "remove", "text",
		"filters", NULL };
	AxisDefBlock *block = g_new0(AxisDefBlock, 1);
	const AxisYamlNode *cases;
	guint i;

	block->name = g_strdup(name != NULL ? name : "");
	block->case_keys = g_ptr_array_new_with_free_func(g_free);
	block->case_values = g_ptr_array_new_with_free_func(g_free);
	if (node == NULL || node->kind != AXIS_YAML_MAP) {
		block->filters = g_ptr_array_new_with_free_func((GDestroyNotify)filter_free);
		if (node != NULL && node->kind == AXIS_YAML_SCALAR)
			block->selector = text_of(node);
		return block;
	}
	check_keys(def, node, keys, where);
	block->selector = text_of(axis_yaml_get(node, "selector"));
	block->text = text_of(axis_yaml_get(node, "text"));
	if (block->text == NULL && axis_yaml_get(node, "text") != NULL && axis_yaml_get(node, "text")->kind ==
	    AXIS_YAML_SCALAR)
		block->text = g_strdup("");
	block->attribute = text_of(axis_yaml_get(node, "attribute"));
	block->optional = bool_of(axis_yaml_get(node, "optional"), FALSE);
	block->default_text = text_of(axis_yaml_get(node, "default"));
	block->remove = text_of(axis_yaml_get(node, "remove"));
	cases = axis_yaml_get(node, "case");
	for (i = 0; cases != NULL && cases->kind == AXIS_YAML_MAP && i < axis_yaml_length(cases) && i < 64; i++) {
		const AxisYamlNode *value = axis_yaml_at(cases, i);

		g_ptr_array_add(block->case_keys, g_strdup(axis_yaml_key_at(cases, i)));
		g_ptr_array_add(block->case_values, g_strdup(value->kind == AXIS_YAML_SCALAR ? value->text : ""));
	}
	block->filters = read_filters(def, axis_yaml_get(node, "filters"));
	return block;
}

static GPtrArray *
read_pairs(
	AxisDef            *def,
	const AxisYamlNode *node,
	gboolean            lists
){
	GPtrArray *pairs = g_ptr_array_new_with_free_func((GDestroyNotify)pair_free);
	guint i;

	for (i = 0; node != NULL && node->kind == AXIS_YAML_MAP && i < axis_yaml_length(node) &&
	     i < AXIS_DEF_MAX_INPUTS; i++) {
		const AxisYamlNode *value = axis_yaml_at(node, i);
		AxisDefPair *pair = g_new0(AxisDefPair, 1);

		pair->name = g_strdup(axis_yaml_key_at(node, i));
		/* A header's value is a list of one. */
		if (lists && value->kind == AXIS_YAML_SEQ)
			value = axis_yaml_at(value, 0);
		pair->value = g_strdup(value != NULL && value->kind == AXIS_YAML_SCALAR ? value->text : "");
		g_ptr_array_add(pairs, pair);
	}
	return pairs;
}

/* ── Links ──────────────────────────────────────────────────────── */

/* Whether @link is an http or https address with a host, and nothing but
 * a path after it. */
static gboolean
link_ok(const gchar *link)
{
	g_autoptr(GUri) uri = NULL;

	if (link == NULL || strlen(link) > 512)
		return FALSE;
	uri = g_uri_parse(link, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_uri_get_host(uri) == NULL || g_uri_get_host(uri)[0] == '\0')
		return FALSE;
	if (g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") != 0 && g_ascii_strcasecmp(g_uri_get_scheme(uri), "http") != 0)
		return FALSE;
	return g_uri_get_userinfo(uri) == NULL && g_uri_get_query(uri) == NULL && g_uri_get_fragment(uri) == NULL;
}

/* The scheme, host and port of @link, as an origin. */
static gboolean
link_origin(
	const gchar    *link,
	AxisHttpOrigin *origin
){
	g_autoptr(GUri) uri = g_uri_parse(link, G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *text = NULL;

	if (uri == NULL || g_uri_get_host(uri) == NULL)
		return FALSE;
	text = g_uri_get_port(uri) > 0 ?
		g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), g_uri_get_host(uri), g_uri_get_port(uri)) :
		g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
	return axis_http_origin_parse(text, origin, NULL);
}

gboolean
axis_def_has_link(
	const AxisDef *def,
	const gchar   *link
){
	AxisHttpOrigin want;
	GPtrArray *lists[2];
	guint l;
	guint i;
	gboolean found = FALSE;

	if (def == NULL || link == NULL || !link_origin(link, &want))
		return FALSE;
	lists[0] = def->links;
	lists[1] = def->legacy_links;
	for (l = 0; l < 2 && !found; l++) {
		for (i = 0; i < lists[l]->len && !found; i++) {
			AxisHttpOrigin have;

			if (link_origin(g_ptr_array_index(lists[l], i), &have)) {
				found = axis_http_origin_equal(&want, &have);
				axis_http_origin_clear(&have);
			}
		}
	}
	axis_http_origin_clear(&want);
	return found;
}

/* ── The parts ──────────────────────────────────────────────────── */

static void
read_caps(
	AxisDef            *def,
	const AxisYamlNode *caps
){
	static const gchar *const keys[] = { "categories", "categorymappings", "modes", "allowrawsearch",
		"allowtvsearchimdb", NULL };
	const AxisYamlNode *mappings = axis_yaml_get(caps, "categorymappings");
	const AxisYamlNode *categories = axis_yaml_get(caps, "categories");
	const AxisYamlNode *modes = axis_yaml_get(caps, "modes");
	gboolean other_only = TRUE;
	guint i;

	check_keys(def, caps, keys, "caps");
	for (i = 0; mappings != NULL && i < axis_yaml_length(mappings) && def->categories->len < AXIS_DEF_MAX_CATEGORIES;
	     i++) {
		const AxisYamlNode *item = axis_yaml_at(mappings, i);
		AxisDefCategory *category;

		if (item->kind != AXIS_YAML_MAP || axis_yaml_text(axis_yaml_get(item, "id")) == NULL)
			continue;
		category = g_new0(AxisDefCategory, 1);
		category->site_id = text_of(axis_yaml_get(item, "id"));
		category->newznab = axis_def_newznab_category(axis_yaml_text(axis_yaml_get(item, "cat")));
		category->desc = text_of(axis_yaml_get(item, "desc"));
		g_ptr_array_add(def->categories, category);
	}
	for (i = 0; categories != NULL && categories->kind == AXIS_YAML_MAP && i < axis_yaml_length(categories) &&
	     def->categories->len < AXIS_DEF_MAX_CATEGORIES; i++) {
		AxisDefCategory *category = g_new0(AxisDefCategory, 1);

		category->site_id = g_strdup(axis_yaml_key_at(categories, i));
		category->newznab = axis_def_newznab_category(axis_yaml_text(axis_yaml_at(categories, i)));
		g_ptr_array_add(def->categories, category);
	}
	for (i = 0; i < def->categories->len; i++) {
		const AxisDefCategory *category = g_ptr_array_index(def->categories, i);

		if (category->newznab >= 2000 && category->newznab < 3000)
			def->covers_movies = TRUE;
		if (category->newznab >= 5000 && category->newznab < 6000)
			def->covers_tv = TRUE;
		if (category->newznab < 8000)
			other_only = FALSE;
	}
	def->general = !def->covers_movies && !def->covers_tv && other_only && def->categories->len > 0;
	for (i = 0; modes != NULL && modes->kind == AXIS_YAML_MAP && i < axis_yaml_length(modes); i++) {
		const gchar *mode = axis_yaml_key_at(modes, i);
		const AxisYamlNode *params = axis_yaml_at(modes, i);
		guint bits = 0;
		guint k;

		for (k = 0; k < axis_yaml_length(params); k++) {
			const gchar *param = axis_yaml_text(axis_yaml_at(params, k));

			if (param == NULL)
				continue;
			if (strcmp(param, "q") == 0)
				bits |= AXIS_DEF_PARAM_Q;
			else if (strcmp(param, "season") == 0)
				bits |= AXIS_DEF_PARAM_SEASON;
			else if (strcmp(param, "ep") == 0)
				bits |= AXIS_DEF_PARAM_EP;
			else if (strcmp(param, "imdbid") == 0)
				bits |= AXIS_DEF_PARAM_IMDB;
			else if (strcmp(param, "tvdbid") == 0)
				bits |= AXIS_DEF_PARAM_TVDB;
			else if (strcmp(param, "tmdbid") == 0)
				bits |= AXIS_DEF_PARAM_TMDB;
		}
		if (strcmp(mode, "search") == 0) {
			def->modes |= AXIS_DEF_MODE_SEARCH;
		} else if (strcmp(mode, "tv-search") == 0) {
			def->modes |= AXIS_DEF_MODE_TV;
			def->tv_params = bits;
		} else if (strcmp(mode, "movie-search") == 0) {
			def->modes |= AXIS_DEF_MODE_MOVIE;
			def->movie_params = bits;
		}
	}
	if (!def->covers_movies && !def->covers_tv && !def->general)
		add_need(def, TRUE, "it has no movie, TV or general category, so there is nothing Axis would search it for");
}

/* A name that means a key or a password. */
static gboolean
secret_name(const gchar *name)
{
	g_autofree gchar *lower = g_ascii_strdown(name, -1);

	return strstr(lower, "key") != NULL || strstr(lower, "pass") != NULL || strstr(lower, "token") != NULL ||
		strstr(lower, "cookie") != NULL || strstr(lower, "rss") != NULL || strstr(lower, "uid") != NULL;
}

static void
read_settings(
	AxisDef            *def,
	const AxisYamlNode *settings
){
	static const gchar *const keys[] = { "name", "label", "type", "default", "options", "defaults", NULL };
	guint i;

	for (i = 0; settings != NULL && i < axis_yaml_length(settings) && i < AXIS_DEF_MAX_SETTINGS; i++) {
		const AxisYamlNode *item = axis_yaml_at(settings, i);
		const AxisYamlNode *value;
		AxisDefSetting *setting;
		gboolean checked = FALSE;

		if (item->kind != AXIS_YAML_MAP || axis_yaml_text(axis_yaml_get(item, "name")) == NULL)
			continue;
		check_keys(def, item, keys, "a setting");
		setting = g_new0(AxisDefSetting, 1);
		setting->name = text_of(axis_yaml_get(item, "name"));
		setting->type = text_of(axis_yaml_get(item, "type"));
		if (setting->type == NULL)
			setting->type = g_strdup("text");
		setting->label = text_of(axis_yaml_get(item, "label"));
		if (strcmp(setting->type, "info_flaresolverr") == 0)
			def->challenge_hint = TRUE;
		value = axis_yaml_get(item, "default");
		if (strcmp(setting->type, "checkbox") == 0) {
			setting->value = g_strdup(axis_yaml_get_bool(value, &checked) && checked ? "True" : "");
		} else if (g_str_has_prefix(setting->type, "info")) {
			setting->value = g_strdup("");
		} else {
			setting->value = g_strdup(value != NULL && value->kind == AXIS_YAML_SCALAR && !axis_yaml_is_null(value) ?
				value->text : "");
		}
		/* A key or a password the site needs: one that says it is
		 * optional is left empty. */
		if (strcmp(setting->type, "password") == 0 ||
		    (strcmp(setting->type, "text") == 0 && setting->value[0] == '\0' && secret_name(setting->name) &&
		     (setting->label == NULL || strstr(setting->label, "ptional") == NULL))) {
			g_autofree gchar *what = g_strdup_printf("its setting \"%.60s\" typed in (a key or a password)",
				setting->label != NULL ? setting->label : setting->name);

			add_need(def, FALSE, what);
		} else if (strcmp(setting->type, "select") == 0 && axis_yaml_get(item, "defaults") != NULL) {
			add_need(def, FALSE, "settings with several choices at once");
		}
		g_ptr_array_add(def->settings, setting);
	}
}

static gboolean
response_of(
	AxisDef            *def,
	const AxisYamlNode *response,
	AxisDefResponse    *kind
){
	const gchar *type = axis_yaml_text(axis_yaml_get(response, "type"));

	*kind = AXIS_DEF_HTML;
	if (response == NULL || type == NULL)
		return TRUE;
	if (g_ascii_strcasecmp(type, "json") == 0)
		*kind = AXIS_DEF_JSON;
	else if (g_ascii_strcasecmp(type, "xml") == 0)
		*kind = AXIS_DEF_XML;
	else if (g_ascii_strcasecmp(type, "html") != 0) {
		g_autofree gchar *what = g_strdup_printf("answers of the type %.20s", type);

		add_need(def, FALSE, what);
		return FALSE;
	}
	return TRUE;
}

/* An address in a path that names a host: what the definition would ask
 * outside its site, unless it is one of its own. */
static void
check_path_address(
	AxisDef     *def,
	const gchar *path
){
	const gchar *host;
	const gchar *end;

	if (path == NULL || !(g_str_has_prefix(path, "http://") || g_str_has_prefix(path, "https://") ||
	                      g_str_has_prefix(path, "//")))
		return;
	host = strstr(path, "//") + 2;
	end = host + strcspn(host, "/?#");
	if (memchr(host, '{', (gsize)(end - host)) != NULL) {
		add_need(def, TRUE, "it searches an address it builds from a setting, not one of its site's, which Axis "
			"does not do");
		return;
	}
	{
		g_autofree gchar *link = g_strndup(path, (gsize)(end - path));

		if (g_str_has_prefix(link, "//")) {
			gchar *https = g_strconcat("https:", link, NULL);

			g_free(link);
			link = https;
		}
		if (!axis_def_has_link(def, link))
			add_need(def, TRUE, "it searches an address that is not one of its site's, which Axis does not do");
	}
}

static void
read_paths(
	AxisDef            *def,
	const AxisYamlNode *search
){
	static const gchar *const keys[] = { "path", "method", "response", "inputs", "inheritinputs", "categories",
		"followredirect", "queryseparator", NULL };
	const AxisYamlNode *paths = axis_yaml_get(search, "paths");
	const AxisYamlNode *single = axis_yaml_get(search, "path");
	guint i;

	if (single != NULL && axis_yaml_text(single) != NULL) {
		AxisDefPath *path = g_new0(AxisDefPath, 1);

		path->path = text_of(single);
		path->inputs = g_ptr_array_new_with_free_func((GDestroyNotify)pair_free);
		path->inherit_inputs = TRUE;
		path->query_separator = g_strdup("&");
		g_ptr_array_add(def->paths, path);
	}
	for (i = 0; paths != NULL && i < axis_yaml_length(paths) && def->paths->len < AXIS_DEF_MAX_PATHS; i++) {
		const AxisYamlNode *item = axis_yaml_at(paths, i);
		const gchar *method = axis_yaml_text(axis_yaml_get(item, "method"));
		const AxisYamlNode *categories = axis_yaml_get(item, "categories");
		AxisDefPath *path;

		if (item->kind != AXIS_YAML_MAP || axis_yaml_text(axis_yaml_get(item, "path")) == NULL) {
			add_need(def, FALSE, "a search path without its path");
			continue;
		}
		check_keys(def, item, keys, "a search path");
		if (method != NULL && g_ascii_strcasecmp(method, "get") != 0)
			add_need(def, FALSE, "searches sent as a form (POST)");
		path = g_new0(AxisDefPath, 1);
		path->path = text_of(axis_yaml_get(item, "path"));
		response_of(def, axis_yaml_get(item, "response"), &path->response);
		if (axis_yaml_get(axis_yaml_get(item, "response"), "noResultsMessage") != NULL)
			add_need(def, FALSE, "a message for no results (noResultsMessage)");
		path->inputs = read_pairs(def, axis_yaml_get(item, "inputs"), FALSE);
		path->inherit_inputs = bool_of(axis_yaml_get(item, "inheritinputs"), TRUE);
		if (categories != NULL) {
			path->categories = g_ptr_array_new_with_free_func(g_free);
			read_texts(categories, path->categories, AXIS_DEF_MAX_CATEGORIES);
		}
		path->query_separator = text_of(axis_yaml_get(item, "queryseparator"));
		if (path->query_separator == NULL)
			path->query_separator = g_strdup("&");
		g_ptr_array_add(def->paths, path);
	}
	if (def->paths->len == 0)
		add_need(def, FALSE, "a search path");
	for (i = 0; i < def->paths->len; i++) {
		const AxisDefPath *path = g_ptr_array_index(def->paths, i);

		check_path_address(def, path->path);
		if (path->response != ((const AxisDefPath *)g_ptr_array_index(def->paths, 0))->response)
			add_need(def, FALSE, "search paths answered in different forms");
	}
}

static void
read_search(
	AxisDef            *def,
	const AxisYamlNode *search
){
	static const gchar *const keys[] = { "path", "paths", "allowEmptyInputs", "inputs", "headers", "keywordsfilters",
		"error", "preprocessingfilters", "rows", "fields", NULL };
	static const gchar *const row_keys[] = { "selector", "after", "filters", "count", "dateheaders", "remove",
		"attribute", "multiple", "missingAttributeEqualsNoResults", "optional", "text", "case", "expect", NULL };
	static const gchar *const headers[] = { "referer", "accept", "cookie", "user-agent", NULL };
	const AxisYamlNode *rows = axis_yaml_get(search, "rows");
	const AxisYamlNode *fields = axis_yaml_get(search, "fields");
	const AxisYamlNode *errors = axis_yaml_get(search, "error");
	const AxisYamlNode *row_filters;
	guint i;

	check_keys(def, search, keys, "search");
	read_paths(def, search);
	g_ptr_array_unref(def->inputs);
	def->inputs = read_pairs(def, axis_yaml_get(search, "inputs"), FALSE);
	def->allow_empty_inputs = bool_of(axis_yaml_get(search, "allowEmptyInputs"), FALSE);
	g_ptr_array_unref(def->headers);
	def->headers = read_pairs(def, axis_yaml_get(search, "headers"), TRUE);
	for (i = 0; i < def->headers->len; i++) {
		const AxisDefPair *header = g_ptr_array_index(def->headers, i);
		g_autofree gchar *lower = g_ascii_strdown(header->name, -1);
		guint k;
		gboolean known = FALSE;

		for (k = 0; headers[k] != NULL && !known; k++)
			known = strcmp(headers[k], lower) == 0;
		if (!known) {
			g_autofree gchar *what = g_strdup_printf("the request header %.40s", header->name);

			add_need(def, FALSE, what);
		}
	}
	g_ptr_array_unref(def->keywords_filters);
	def->keywords_filters = read_filters(def, axis_yaml_get(search, "keywordsfilters"));
	if (axis_yaml_get(search, "preprocessingfilters") != NULL)
		add_need(def, FALSE, "filters of the whole answer (preprocessingfilters)");
	for (i = 0; errors != NULL && i < axis_yaml_length(errors) && i < 16; i++) {
		const AxisYamlNode *item = axis_yaml_at(errors, i);
		AxisDefError *error;

		if (item->kind != AXIS_YAML_MAP || axis_yaml_text(axis_yaml_get(item, "selector")) == NULL)
			continue;
		error = g_new0(AxisDefError, 1);
		error->selector = text_of(axis_yaml_get(item, "selector"));
		if (axis_yaml_get(item, "message") != NULL)
			error->message = read_block(def, "", axis_yaml_get(item, "message"), "an error's message");
		g_ptr_array_add(def->errors, error);
	}
	if (rows == NULL || rows->kind != AXIS_YAML_MAP || axis_yaml_text(axis_yaml_get(rows, "selector")) == NULL) {
		add_need(def, FALSE, "a rows selector");
	} else {
		check_keys(def, rows, row_keys, "search.rows");
		def->rows_selector = text_of(axis_yaml_get(rows, "selector"));
		/* Axis's own: what every answer has, with results or without. */
		def->rows_expect = text_of(axis_yaml_get(rows, "expect"));
		if (axis_yaml_get(rows, "expect") != NULL && (def->rows_expect == NULL || def->rows_expect[0] == '\0'))
			add_need(def, FALSE, "a rows.expect that names something");
		if (axis_yaml_text(axis_yaml_get(rows, "after")) != NULL) {
			gint64 after = g_ascii_strtoll(axis_yaml_text(axis_yaml_get(rows, "after")), NULL, 10);

			def->rows_after = (gint)CLAMP(after, 0, 8);
		}
		if (axis_yaml_get(rows, "dateheaders") != NULL)
			add_need(def, FALSE, "dates from row headers (rows.dateheaders)");
		if (axis_yaml_get(rows, "remove") != NULL)
			add_need(def, FALSE, "rows.remove");
		if (axis_yaml_get(rows, "attribute") != NULL || axis_yaml_get(rows, "multiple") != NULL)
			add_need(def, FALSE, "rows from lists inside each row (rows.multiple)");
		if (axis_yaml_get(rows, "text") != NULL || axis_yaml_get(rows, "case") != NULL)
			add_need(def, FALSE, "rows.text and rows.case");
		row_filters = axis_yaml_get(rows, "filters");
		for (i = 0; row_filters != NULL && i < axis_yaml_length(row_filters); i++) {
			const gchar *name = axis_yaml_text(axis_yaml_get(axis_yaml_at(row_filters, i), "name"));

			if (g_strcmp0(name, "andmatch") == 0) {
				def->rows_andmatch = TRUE;
			} else if (g_strcmp0(name, "strdump") != 0) {
				g_autofree gchar *what = g_strdup_printf("the row filter %.40s", name != NULL ? name : "(unnamed)");

				add_need(def, FALSE, what);
			}
		}
	}
	for (i = 0; fields != NULL && fields->kind == AXIS_YAML_MAP && i < axis_yaml_length(fields) &&
	     def->fields->len < AXIS_DEF_MAX_FIELDS; i++) {
		const gchar *name = axis_yaml_key_at(fields, i);

		if (strchr(name, '|') != NULL) {
			add_need(def, FALSE, "field modifiers (title|append)");
			continue;
		}
		g_ptr_array_add(def->fields, read_block(def, name, axis_yaml_at(fields, i), "a field"));
	}
	if (axis_def_field(def, "title") == NULL)
		add_need(def, FALSE, "a title field");
	if (axis_def_field(def, "download") == NULL && axis_def_field(def, "magnet") == NULL &&
	    axis_def_field(def, "infohash") == NULL)
		add_need(def, FALSE, "a download, magnet or info-hash field");
}

static void
read_download(
	AxisDef            *def,
	const AxisYamlNode *download
){
	static const gchar *const keys[] = { "selectors", "infohash", "method", "before", "headers", NULL };
	static const gchar *const hash_keys[] = { "hash", "title", "usebeforeresponse", NULL };
	const AxisYamlNode *selectors = axis_yaml_get(download, "selectors");
	const AxisYamlNode *infohash = axis_yaml_get(download, "infohash");
	const gchar *method = axis_yaml_text(axis_yaml_get(download, "method"));
	guint i;

	check_keys(def, download, keys, "download");
	if (axis_yaml_get(download, "before") != NULL)
		add_need(def, FALSE, "a request before the download (download.before)");
	if (method != NULL && g_ascii_strcasecmp(method, "get") != 0)
		add_need(def, FALSE, "downloads sent as a form (POST)");
	if (axis_yaml_get(download, "headers") != NULL)
		add_need(def, FALSE, "headers on the download (download.headers)");
	for (i = 0; selectors != NULL && i < axis_yaml_length(selectors) && i < 16; i++)
		g_ptr_array_add(def->download_selectors, read_block(def, "", axis_yaml_at(selectors, i), "a download selector"));
	if (infohash != NULL) {
		check_keys(def, infohash, hash_keys, "download.infohash");
		if (bool_of(axis_yaml_get(infohash, "usebeforeresponse"), FALSE))
			add_need(def, FALSE, "a request before the download (download.before)");
		def->download_hash = read_block(def, "", axis_yaml_get(infohash, "hash"), "download.infohash.hash");
		def->download_title = read_block(def, "", axis_yaml_get(infohash, "title"), "download.infohash.title");
	}
}

/* ── Checking what the engine will read ─────────────────────────── */

/* The variables a definition's templates see when it is judged: its
 * settings' defaults, an empty search. */
static AxisTemplateVars *
judge_vars(const AxisDef *def)
{
	AxisTemplateVars *vars = g_new0(AxisTemplateVars, 1);
	guint i;

	vars->values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < def->settings->len; i++) {
		const AxisDefSetting *setting = g_ptr_array_index(def->settings, i);

		g_hash_table_replace(vars->values, g_strconcat("Config.", setting->name, NULL), g_strdup(setting->value));
	}
	g_hash_table_replace(vars->values, g_strdup("Config.sitelink"),
		g_strdup(def->links->len > 0 ? g_ptr_array_index(def->links, 0) : ""));
	vars->categories = g_ptr_array_new_with_free_func(g_free);
	return vars;
}

static void
judge_vars_free(AxisTemplateVars *vars)
{
	g_hash_table_unref(vars->values);
	g_ptr_array_unref(vars->categories);
	g_free(vars);
}

static void
check_template(
	AxisDef     *def,
	const gchar *text
){
	g_autofree gchar *problem = NULL;
	gboolean unsupported = FALSE;
	AxisTemplate *template_;

	if (!axis_template_has_actions(text))
		return;
	template_ = axis_template_parse(text, &problem, &unsupported);
	if (template_ == NULL) {
		if (unsupported)
			add_need_from(def, problem, "a template");
		else
			add_need_from(def, problem, "templates Axis can read");
		return;
	}
	axis_template_free(template_);
}

/* A selector as the engine will read it: rendered with the defaults first
 * when it is a template. */
static void
check_selector(
	AxisDef                *def,
	const gchar            *text,
	const AxisTemplateVars *vars,
	AxisDefRegexCache      *regex
){
	g_autofree gchar *rendered = NULL;
	g_autofree gchar *problem = NULL;
	gboolean unsupported = FALSE;
	AxisSelector *selector;

	if (text == NULL)
		return;
	check_template(def, text);
	rendered = axis_template_expand(text, vars, NULL, regex, NULL);
	if (rendered == NULL)
		return;
	selector = axis_selector_parse(rendered, &problem, &unsupported);
	if (selector == NULL) {
		if (unsupported)
			add_need_from(def, problem, "a selector");
		else
			add_need_from(def, problem, "selectors Axis can read");
		return;
	}
	axis_selector_free(selector);
}

/* A JSON path: "$", names joined by ".", "[n]" indexes. */
static void
check_json_path(
	AxisDef     *def,
	const gchar *text
){
	if (text == NULL)
		return;
	check_template(def, text);
	if (g_str_has_prefix(text, ".."))
		add_need(def, FALSE, "JSON paths to a row's parent (..)");
	else if (strchr(text, ':') != NULL || strchr(text, '*') != NULL || strchr(text, '?') != NULL ||
	         strchr(text, ' ') != NULL)
		add_need(def, FALSE, "JSON paths with filters (:has, :contains, * or ?)");
}

static void
check_filters(
	AxisDef         *def,
	const GPtrArray *filters
){
	guint i;

	for (i = 0; i < filters->len; i++) {
		const AxisDefFilter *filter = g_ptr_array_index(filters, i);
		guint k;

		for (k = 0; filter->args[k] != NULL; k++)
			check_template(def, filter->args[k]);
		if ((strcmp(filter->name, "re_replace") == 0 || strcmp(filter->name, "regexp") == 0) &&
		    filter->args[0] != NULL && !axis_template_has_actions(filter->args[0])) {
			g_autofree gchar *problem = NULL;

			if (!axis_def_regex_check(filter->args[0], &problem))
				add_need_from(def, problem, "regular expressions Axis can run");
		}
	}
}

static void
check_block(
	AxisDef                *def,
	const AxisDefBlock     *block,
	AxisDefResponse         response,
	const AxisTemplateVars *vars,
	AxisDefRegexCache      *regex
){
	guint i;

	if (block == NULL)
		return;
	if (response == AXIS_DEF_JSON)
		check_json_path(def, block->selector);
	else
		check_selector(def, block->selector, vars, regex);
	if (block->remove != NULL) {
		if (response == AXIS_DEF_JSON)
			add_need(def, FALSE, "remove in a JSON answer");
		else
			check_selector(def, block->remove, vars, regex);
	}
	check_template(def, block->text);
	check_template(def, block->default_text);
	for (i = 0; i < block->case_keys->len; i++) {
		const gchar *key = g_ptr_array_index(block->case_keys, i);

		check_template(def, g_ptr_array_index(block->case_values, i));
		if (strcmp(key, "*") != 0 && response != AXIS_DEF_JSON)
			check_selector(def, key, vars, regex);
	}
	check_filters(def, block->filters);
}

static void
check_all(AxisDef *def)
{
	AxisTemplateVars *vars = judge_vars(def);
	g_autoptr(AxisDefRegexCache) regex = axis_def_regex_cache_new(AXIS_DEF_REGEX_BUDGET_US);
	AxisDefResponse response = def->paths->len > 0 ?
		((const AxisDefPath *)g_ptr_array_index(def->paths, 0))->response : AXIS_DEF_HTML;
	guint i;
	guint k;

	for (i = 0; i < def->paths->len; i++) {
		const AxisDefPath *path = g_ptr_array_index(def->paths, i);

		check_template(def, path->path);
		for (k = 0; k < path->inputs->len; k++)
			check_template(def, ((const AxisDefPair *)g_ptr_array_index(path->inputs, k))->value);
	}
	for (k = 0; k < def->inputs->len; k++) {
		const AxisDefPair *input = g_ptr_array_index(def->inputs, k);

		check_template(def, input->value);
	}
	for (k = 0; k < def->headers->len; k++)
		check_template(def, ((const AxisDefPair *)g_ptr_array_index(def->headers, k))->value);
	check_filters(def, def->keywords_filters);
	if (response == AXIS_DEF_JSON) {
		check_json_path(def, def->rows_selector);
		if (def->rows_expect != NULL && def->rows_expect[0] != '\0')
			check_json_path(def, def->rows_expect);
	} else {
		check_selector(def, def->rows_selector, vars, regex);
		if (def->rows_expect != NULL && def->rows_expect[0] != '\0')
			check_selector(def, def->rows_expect, vars, regex);
	}
	for (i = 0; i < def->errors->len; i++) {
		const AxisDefError *error = g_ptr_array_index(def->errors, i);

		if (response == AXIS_DEF_JSON)
			add_need(def, FALSE, "error blocks in a JSON answer");
		else
			check_selector(def, error->selector, vars, regex);
		check_block(def, error->message, response, vars, regex);
	}
	for (i = 0; i < def->fields->len; i++)
		check_block(def, g_ptr_array_index(def->fields, i), response, vars, regex);
	for (i = 0; i < def->download_selectors->len; i++)
		check_block(def, g_ptr_array_index(def->download_selectors, i), AXIS_DEF_HTML, vars, regex);
	check_block(def, def->download_hash, AXIS_DEF_HTML, vars, regex);
	check_block(def, def->download_title, AXIS_DEF_HTML, vars, regex);
	judge_vars_free(vars);
}

/* ── Loading ────────────────────────────────────────────────────── */

static gboolean
id_ok(const gchar *id)
{
	const gchar *p;

	if (id == NULL || id[0] == '\0' || strlen(id) > 64)
		return FALSE;
	for (p = id; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-' && *p != '_')
			return FALSE;
	}
	return TRUE;
}

/* A short text of the definition's, cleaned: valid, no control
 * characters, at most @max characters. */
static gchar *
clean_text(
	const gchar *text,
	guint        max
){
	GString *out = g_string_new(NULL);
	const gchar *p;
	guint n = 0;

	for (p = text != NULL ? text : ""; *p != '\0' && n < max; p = g_utf8_next_char(p), n++) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || c == 0x7f)
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}
	return g_strstrip(g_string_free(out, FALSE));
}

AxisDef *
axis_def_load(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	static const gchar *const keys[] = { "id", "replaces", "name", "description", "language", "type", "encoding",
		"followredirect", "testlinktorrent", "requestDelay", "links", "legacylinks", "certificates", "caps",
		"settings", "login", "search", "download", "version", NULL };
	g_autoptr(AxisYamlNode) root = NULL;
	g_autofree gchar *why = NULL;
	AxisDef *def;
	const gchar *delay;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (length > AXIS_DEF_MAX_BYTES) {
		if (problem != NULL)
			*problem = g_strdup("The file is larger than a definition can be.");
		return NULL;
	}
	root = axis_yaml_parse(data, length, &why);
	if (root == NULL) {
		if (problem != NULL)
			*problem = g_strdup_printf("It is not YAML that Axis reads (%s).", why != NULL ? why : "unknown");
		return NULL;
	}
	if (root->kind != AXIS_YAML_MAP || !id_ok(axis_yaml_text(axis_yaml_get(root, "id"))) ||
	    axis_yaml_text(axis_yaml_get(root, "name")) == NULL) {
		if (problem != NULL)
			*problem = g_strdup("It is not an indexer definition: it has no id of letters, digits, dots and dashes, "
				"or no name.");
		return NULL;
	}
	def = def_new();
	def->id = text_of(axis_yaml_get(root, "id"));
	def->name = clean_text(axis_yaml_text(axis_yaml_get(root, "name")), 100);
	def->description = clean_text(axis_yaml_text(axis_yaml_get(root, "description")), 300);
	def->language = clean_text(axis_yaml_text(axis_yaml_get(root, "language")), 16);
	def->site_type = clean_text(axis_yaml_text(axis_yaml_get(root, "type")), 16);
	def->encoding = text_of(axis_yaml_get(root, "encoding"));
	if (def->name[0] == '\0') {
		g_free(def->name);
		def->name = g_strdup(def->id);
	}
	read_texts(axis_yaml_get(root, "links"), def->links, AXIS_DEF_MAX_LINKS);
	read_texts(axis_yaml_get(root, "legacylinks"), def->legacy_links, AXIS_DEF_MAX_LINKS);
	for (i = 0; i < def->links->len; i++) {
		if (!link_ok(g_ptr_array_index(def->links, i)))
			break;
	}
	if (def->links->len == 0 || i < def->links->len || axis_yaml_get(root, "search") == NULL) {
		if (problem != NULL)
			*problem = g_strdup(axis_yaml_get(root, "search") == NULL ? "It is not an indexer definition: it has no "
				"search." : "It is not an indexer definition: its links are missing or not all http or https "
				"addresses.");
		axis_def_free(def);
		return NULL;
	}
	/* Old addresses that are not addresses are left out. */
	for (i = def->legacy_links->len; i > 0; i--) {
		if (!link_ok(g_ptr_array_index(def->legacy_links, i - 1)))
			g_ptr_array_remove_index(def->legacy_links, i - 1);
	}
	check_keys(def, root, keys, "the definition");
	/* Axis's own: the file's revision, a whole number. */
	if (axis_yaml_get(root, "version") != NULL) {
		const gchar *text = axis_yaml_text(axis_yaml_get(root, "version"));
		guint64 value = 0;

		if (text != NULL && text[0] != '\0' && strlen(text) <= 7 && strspn(text, "0123456789") == strlen(text))
			value = g_ascii_strtoull(text, NULL, 10);
		if (value < 1 || value > AXIS_DEF_MAX_VERSION)
			add_need(def, FALSE, "a version that is a whole number from 1");
		else
			def->version = (gint)value;
	}
	delay = axis_yaml_text(axis_yaml_get(root, "requestDelay"));
	if (delay != NULL)
		def->request_delay = CLAMP(g_ascii_strtod(delay, NULL), 0.0, 60.0);
	if (axis_yaml_get(root, "login") != NULL)
		add_need(def, FALSE, "a login (stage 2)");
	else if (g_strcmp0(def->site_type, "public") != 0)
		add_need(def, FALSE, "an account (it is not a public site)");
	if (axis_yaml_get(root, "certificates") != NULL)
		add_need(def, FALSE, "certificates of its own");
	if (def->encoding != NULL && g_ascii_strcasecmp(def->encoding, "UTF-8") != 0 &&
	    g_ascii_strcasecmp(def->encoding, "UTF8") != 0) {
		g_autofree gchar *converted = g_convert("a", 1, def->encoding, "UTF-8", NULL, NULL, NULL);

		if (converted == NULL || strlen(def->encoding) > 32) {
			g_autofree gchar *what = g_strdup_printf("the encoding %.32s", def->encoding);

			add_need(def, FALSE, what);
		}
	}
	read_caps(def, axis_yaml_get(root, "caps"));
	read_settings(def, axis_yaml_get(root, "settings"));
	read_search(def, axis_yaml_get(root, "search"));
	if (axis_yaml_get(root, "download") != NULL)
		read_download(def, axis_yaml_get(root, "download"));
	check_all(def);
	return def;
}

/* ── Lookups ────────────────────────────────────────────────────── */

const AxisDefSetting *
axis_def_setting(
	const AxisDef *def,
	const gchar   *name
){
	guint i;

	for (i = 0; def != NULL && name != NULL && i < def->settings->len; i++) {
		const AxisDefSetting *setting = g_ptr_array_index(def->settings, i);

		if (strcmp(setting->name, name) == 0)
			return setting;
	}
	return NULL;
}

const AxisDefBlock *
axis_def_field(
	const AxisDef *def,
	const gchar   *name
){
	guint i;

	for (i = 0; def != NULL && name != NULL && i < def->fields->len; i++) {
		const AxisDefBlock *block = g_ptr_array_index(def->fields, i);

		if (strcmp(block->name, name) == 0)
			return block;
	}
	return NULL;
}
