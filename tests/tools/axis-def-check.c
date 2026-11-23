/*
 * axis-def-check.c - reads indexer definitions as Axis would, outside the
 * daemon: a developer's tool, built with the tests, never run by them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-def-check DIR...
 *       every *.yml under each DIR (not recursively): for each public
 *       definition without a login, "usable" or "not usable: <why>", and
 *       the totals; the other definitions are counted by why they are out.
 *       A usable one also has the engine build the requests of a movie
 *       search, an episode's and its newest page against its first link
 *       (asking nothing): one that builds none is counted apart
 *   axis-def-check --all DIR...
 *       the same line for every definition
 *   axis-def-check --requests DIR...
 *       as the first, with each request the engine would make (printed,
 *       never sent)
 *   axis-def-check --yaml FILE
 *       the YAML reader's tree of FILE as JSON (maps as objects, lists as
 *       arrays, scalars as strings, null as null), to compare with another
 *       reader
 *
 * The files are read as data, as the daemon reads an uploaded one; nothing
 * in them is run and nothing is fetched.  A later directory's file of the
 * same name replaces an earlier one's (give the schema versions in order).
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "core/axis-http-origin.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"
#include "indexer/def/axis-def-regex.h"
#include "indexer/def/axis-def-yaml.h"

static void
print_json_string(
	GString     *out,
	const gchar *text
){
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = text; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		if (c == '"' || c == '\\')
			g_string_append_printf(out, "\\%c", c);
		else if (c == '\n')
			g_string_append(out, "\\n");
		else if (c < 0x20)
			g_string_append_printf(out, "\\u%04x", c);
		else
			g_string_append_c(out, (gchar)c);
	}
	g_string_append_c(out, '"');
}

static void
print_node(
	GString            *out,
	const AxisYamlNode *node
){
	guint i;

	if (node->kind == AXIS_YAML_SCALAR) {
		if (axis_yaml_is_null(node))
			g_string_append(out, "null");
		else
			print_json_string(out, node->text);
		return;
	}
	g_string_append_c(out, node->kind == AXIS_YAML_MAP ? '{' : '[');
	for (i = 0; i < axis_yaml_length(node); i++) {
		if (i > 0)
			g_string_append_c(out, ',');
		if (node->kind == AXIS_YAML_MAP) {
			print_json_string(out, axis_yaml_key_at(node, i));
			g_string_append_c(out, ':');
		}
		print_node(out, axis_yaml_at(node, i));
	}
	g_string_append_c(out, node->kind == AXIS_YAML_MAP ? '}' : ']');
}

static int
dump_yaml(const gchar *path)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *problem = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisYamlNode) root = NULL;
	g_autoptr(GString) out = g_string_new(NULL);
	gsize length = 0;

	if (!g_file_get_contents(path, &data, &length, &error)) {
		fprintf(stderr, "%s\n", error->message);
		return 2;
	}
	root = axis_yaml_parse(data, length, &problem);
	if (root == NULL) {
		printf("{\"error\":");
		g_string_truncate(out, 0);
		print_json_string(out, problem);
		printf("%s}\n", out->str);
		return 1;
	}
	print_node(out, root);
	printf("%s\n", out->str);
	return 0;
}

/* The files to read: every *.yml of each directory, a later directory's
 * replacing an earlier one's of the same name; sorted by name. */
static GPtrArray *
collect(
	char **dirs,
	int    n
){
	g_autoptr(GHashTable) chosen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GList) names = NULL;
	GList *l;
	int i;

	for (i = 0; i < n; i++) {
		g_autoptr(GDir) dir = g_dir_open(dirs[i], 0, NULL);
		const gchar *name;

		if (dir == NULL) {
			fprintf(stderr, "cannot read the directory %s\n", dirs[i]);
			continue;
		}
		while ((name = g_dir_read_name(dir)) != NULL) {
			if (g_str_has_suffix(name, ".yml") || g_str_has_suffix(name, ".yaml"))
				g_hash_table_replace(chosen, g_strdup(name), g_build_filename(dirs[i], name, NULL));
		}
	}
	names = g_list_sort(g_hash_table_get_keys(chosen), (GCompareFunc)strcmp);
	for (l = names; l != NULL; l = l->next)
		g_ptr_array_add(paths, g_strdup(g_hash_table_lookup(chosen, l->data)));
	return paths;
}

/* --requests: print each request built. */
static gboolean show_requests = FALSE;

/*
 * Whether the engine builds the requests of a movie search, an episode's
 * and the newest page (what the definition covers) against its first link,
 * asking nothing; why not in @problem.
 */
static gboolean
builds_requests(
	const AxisDef  *def,
	gchar         **problem,
	guint          *n_paths
){
	AxisHttpOrigin origin;
	AxisDefSite site;
	AxisIndexerRequest requests[3];
	g_autoptr(GUri) uri = g_uri_parse(g_ptr_array_index(def->links, 0), G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *origin_text = NULL;
	g_autofree gchar *base_path = NULL;
	guint i;

	*n_paths = 0;
	*problem = NULL;
	if (uri == NULL) {
		*problem = g_strdup("its first link cannot be read");
		return FALSE;
	}
	origin_text = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), g_uri_get_host(uri),
		g_uri_get_port(uri)) : g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
	base_path = g_strdup(g_uri_get_path(uri));
	while (base_path[0] != '\0' && base_path[strlen(base_path) - 1] == '/')
		base_path[strlen(base_path) - 1] = '\0';
	if (!axis_http_origin_parse(origin_text, &origin, NULL)) {
		*problem = g_strdup("its first link is not an origin");
		return FALSE;
	}
	memset(&site, 0, sizeof(site));
	site.def = def;
	site.origin = &origin;
	site.base_path = base_path;
	site.now_us = g_get_real_time();
	axis_indexer_request_init(&requests[0], AXIS_INDEXER_QUERY_FILM);
	requests[0].title = "Hearth";
	requests[0].year = 1995;
	requests[0].imdb_id = "tt0600001";
	axis_indexer_request_init(&requests[1], AXIS_INDEXER_QUERY_EPISODE);
	requests[1].title = "The Long Ferry";
	requests[1].season = 1;
	requests[1].episode = 2;
	axis_indexer_request_init(&requests[2], AXIS_INDEXER_QUERY_TEXT);
	for (i = 0; i < G_N_ELEMENTS(requests); i++) {
		AxisDefSearch *search;

		if ((i == 0 && !def->covers_movies && !def->general) || (i == 1 && !def->covers_tv && !def->general))
			continue;
		search = axis_def_search_new(&site, &requests[i], problem);
		if (search == NULL) {
			axis_http_origin_clear(&origin);
			return FALSE;
		}
		*n_paths += axis_def_search_n_requests(search);
		if (show_requests) {
			guint k;

			for (k = 0; k < axis_def_search_n_requests(search); k++)
				printf("  %s %s\n", i == 0 ? "movie  " : i == 1 ? "episode" : "newest ",
					axis_def_search_request(search, k)->path);
		}
		axis_def_search_free(search);
	}
	axis_http_origin_clear(&origin);
	return TRUE;
}

/* The need that decides why a definition is out, for the totals. */
static const gchar *
first_need(const AxisDef *def)
{
	guint i;

	for (i = 0; i < def->needs->len; i++) {
		const AxisDefNeed *need = g_ptr_array_index(def->needs, i);

		if (need->never)
			return need->what;
	}
	return def->needs->len > 0 ? ((const AxisDefNeed *)g_ptr_array_index(def->needs, 0))->what : "";
}

static gint
by_count(
	gconstpointer a,
	gconstpointer b,
	gpointer      data
){
	GHashTable *counts = data;

	return GPOINTER_TO_INT(g_hash_table_lookup(counts, b)) - GPOINTER_TO_INT(g_hash_table_lookup(counts, a));
}

static int
check_all(
	gboolean   all,
	char     **dirs,
	int        n
){
	g_autoptr(GPtrArray) paths = collect(dirs, n);
	g_autoptr(GHashTable) reasons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) each_need = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GList) keys = NULL;
	GList *l;
	guint public_n = 0;
	guint usable = 0;
	guint unbuilt = 0;
	guint invalid = 0;
	guint out = 0;
	guint i;

	for (i = 0; i < paths->len; i++) {
		const gchar *path = g_ptr_array_index(paths, i);
		g_autofree gchar *base = g_path_get_basename(path);
		g_autofree gchar *data = NULL;
		g_autofree gchar *problem = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisDef) def = NULL;
		gsize length = 0;
		gboolean in_set;

		if (!g_file_get_contents(path, &data, &length, &error)) {
			printf("%-28s cannot be read: %s\n", base, error->message);
			continue;
		}
		def = axis_def_load(data, length, &problem);
		if (def == NULL) {
			invalid++;
			printf("%-28s not a definition: %s\n", base, problem);
			continue;
		}
		/* The population: public, without a login. */
		in_set = g_strcmp0(def->site_type, "public") == 0 && strstr(data, "\nlogin:") == NULL;
		if (!in_set) {
			out++;
			if (!all)
				continue;
		} else {
			public_n++;
		}
		if (axis_def_usable(def)) {
			g_autofree gchar *why = NULL;
			guint n_paths = 0;

			if (!builds_requests(def, &why, &n_paths)) {
				if (in_set)
					unbuilt++;
				printf("%-28s usable, but builds no request: %s\n", base, why);
				continue;
			}
			if (in_set)
				usable++;
			printf("%-28s usable%s%s%s (%u requests for a movie, an episode and the newest)\n", base,
				def->covers_movies ? " movies" : "", def->covers_tv ? " TV" : "", def->general ? " general" : "",
				n_paths);
		} else {
			g_autofree gchar *why = axis_def_problem(def);
			guint k;

			printf("%-28s not usable: %s\n", base, why);
			if (in_set) {
				const gchar *reason = first_need(def);

				g_hash_table_replace(reasons, g_strdup(reason),
					GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(reasons, reason)) + 1));
				for (k = 0; k < def->needs->len; k++) {
					const gchar *what = ((const AxisDefNeed *)g_ptr_array_index(def->needs, k))->what;

					g_hash_table_replace(each_need, g_strdup(what),
						GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(each_need, what)) + 1));
				}
			}
		}
	}
	printf("\nfiles %u; not definitions %u; private, semi-private or with a login %u\n", paths->len, invalid, out);
	printf("public without a login %u: usable %u (each builds its requests), usable but building no request %u, "
		"not usable %u\n", public_n, usable, unbuilt, public_n - usable - unbuilt);
	printf("\nwhy not usable (the deciding need of each):\n");
	keys = g_list_sort_with_data(g_hash_table_get_keys(reasons), by_count, reasons);
	for (l = keys; l != NULL; l = l->next)
		printf("%4d  %s\n", GPOINTER_TO_INT(g_hash_table_lookup(reasons, l->data)), (const gchar *)l->data);
	g_clear_pointer(&keys, g_list_free);
	printf("\nevery need, with how many of the not usable have it:\n");
	keys = g_list_sort_with_data(g_hash_table_get_keys(each_need), by_count, each_need);
	for (l = keys; l != NULL; l = l->next)
		printf("%4d  %s\n", GPOINTER_TO_INT(g_hash_table_lookup(each_need, l->data)), (const gchar *)l->data);
	return 0;
}

static void
print_bad_patterns(const GPtrArray *filters)
{
	guint i;

	for (i = 0; i < filters->len; i++) {
		const AxisDefFilter *filter = g_ptr_array_index(filters, i);
		g_autofree gchar *problem = NULL;

		if ((strcmp(filter->name, "re_replace") == 0 || strcmp(filter->name, "regexp") == 0) &&
		    filter->args[0] != NULL && !axis_def_regex_check(filter->args[0], &problem))
			printf("  %s: %s\n", problem, filter->args[0]);
	}
}

/* The regular expressions of FILE that are refused, with their text. */
static int
bad_patterns(const gchar *path)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisDef) def = NULL;
	gsize length = 0;
	guint i;

	if (!g_file_get_contents(path, &data, &length, NULL))
		return 2;
	def = axis_def_load(data, length, &problem);
	if (def == NULL) {
		printf("not a definition: %s\n", problem);
		return 1;
	}
	print_bad_patterns(def->keywords_filters);
	for (i = 0; i < def->fields->len; i++)
		print_bad_patterns(((const AxisDefBlock *)g_ptr_array_index(def->fields, i))->filters);
	return 0;
}

int
main(
	int    argc,
	char **argv
){
	if (argc == 3 && strcmp(argv[1], "--yaml") == 0)
		return dump_yaml(argv[2]);
	if (argc == 3 && strcmp(argv[1], "--patterns") == 0)
		return bad_patterns(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "--all") == 0)
		return check_all(TRUE, argv + 2, argc - 2);
	if (argc >= 3 && strcmp(argv[1], "--requests") == 0) {
		show_requests = TRUE;
		return check_all(FALSE, argv + 2, argc - 2);
	}
	if (argc >= 2 && argv[1][0] != '-')
		return check_all(FALSE, argv + 1, argc - 1);
	fprintf(stderr, "usage: %s [--all] DIR...\n       %s --yaml FILE\n", argv[0], argv[0]);
	return 2;
}
