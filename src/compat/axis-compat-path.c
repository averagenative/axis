/*
 * axis-compat-path.c - the server's address, and mapping its paths to ours
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-path.h"

#include <string.h>

/* ── The address ────────────────────────────────────────────────── */

void
axis_compat_address_clear(AxisCompatAddress *address)
{
	axis_http_origin_clear(address);
}

static void
set_problem(
	gchar      **problem,
	const gchar *text
){
	if (problem != NULL)
		*problem = g_strdup(text);
}

gboolean
axis_compat_address_parse(
	const gchar       *text,
	AxisCompatAddress *address,
	gchar            **problem
){
	return axis_http_origin_parse(text, address, problem);
}

gboolean
axis_compat_address_validate(
	const gchar  *text,
	gchar       **reason
){
	AxisCompatAddress address;
	gboolean ok = axis_compat_address_parse(text, &address, reason);

	axis_compat_address_clear(&address);
	if (!ok && *reason == NULL)
		*reason = g_strdup("must be an http or https address");
	return ok;
}

/* ── Paths ──────────────────────────────────────────────────────── */

gchar *
axis_compat_path_normalize(
	const gchar  *text,
	gchar       **problem
){
	GString *out;
	const gchar *p;
	const gchar *segment;
	gsize length;

	if (text == NULL || text[0] != '/') {
		set_problem(problem, "must be an absolute path");
		return NULL;
	}
	length = strlen(text);
	if (length > AXIS_COMPAT_PATH_MAX_BYTES || !g_utf8_validate(text, (gssize)length, NULL)) {
		set_problem(problem, "must be valid text of at most 1024 bytes");
		return NULL;
	}
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p))) {
			set_problem(problem, "must not contain control characters");
			return NULL;
		}
	}
	if (length == 1)
		return g_strdup("/");
	if (text[length - 1] == '/')
		length--;
	out = g_string_new(NULL);
	segment = text + 1;
	while ((gsize)(segment - text) <= length) {
		const gchar *end = memchr(segment, '/', length - (gsize)(segment - text));
		gsize n = end != NULL ? (gsize)(end - segment) : length - (gsize)(segment - text);

		if (n == 0 || (n == 1 && segment[0] == '.') || (n == 2 && segment[0] == '.' && segment[1] == '.')) {
			g_string_free(out, TRUE);
			set_problem(problem, "must not have empty, . or .. parts");
			return NULL;
		}
		g_string_append_c(out, '/');
		g_string_append_len(out, segment, (gssize)n);
		if (end == NULL)
			break;
		segment = end + 1;
	}
	return g_string_free(out, FALSE);
}

void
axis_compat_path_entry_free(AxisCompatPathEntry *entry)
{
	if (entry == NULL)
		return;
	g_free(entry->server);
	g_free(entry->axis);
	g_free(entry->kind);
	g_free(entry);
}

void
axis_compat_root_ref_free(AxisCompatRootRef *root)
{
	if (root == NULL)
		return;
	g_free(root->path);
	g_free(root->media_type);
	g_free(root);
}

void
axis_compat_suggestion_free(AxisCompatSuggestion *suggestion)
{
	if (suggestion == NULL)
		return;
	g_free(suggestion->folder);
	g_free(suggestion->root_path);
	g_free(suggestion);
}

/* Whether @path is @prefix or lies below it (both normalised). */
static gboolean
at_or_below(
	const gchar *path,
	const gchar *prefix
){
	gsize n = strlen(prefix);

	if (strcmp(prefix, "/") == 0)
		return TRUE;
	return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static gboolean
strictly_below(
	const gchar *path,
	const gchar *prefix
){
	return strcmp(path, prefix) != 0 && at_or_below(path, prefix);
}

gchar *
axis_compat_path_map_apply(
	GPtrArray   *entries,
	const gchar *server_path
){
	g_autofree gchar *path = axis_compat_path_normalize(server_path, NULL);
	const AxisCompatPathEntry *best = NULL;
	const gchar *rest;
	guint i;

	if (path == NULL || entries == NULL)
		return NULL;
	for (i = 0; i < entries->len; i++) {
		const AxisCompatPathEntry *e = g_ptr_array_index(entries, i);

		if (at_or_below(path, e->server) && (best == NULL || strlen(e->server) > strlen(best->server)))
			best = e;
	}
	if (best == NULL)
		return NULL;
	rest = strcmp(best->server, "/") == 0 ? path : path + strlen(best->server);
	if (rest[0] == '\0')
		return g_strdup(best->axis);
	if (strcmp(best->axis, "/") == 0)
		return g_strdup(rest);
	/* A longer prefix can carry the result past what a path may be: such a
	 * path is not mapped, like any other path that is not a path here. */
	if (strlen(best->axis) + strlen(rest) > AXIS_COMPAT_PATH_MAX_BYTES)
		return NULL;
	return g_strconcat(best->axis, rest, NULL);
}

gboolean
axis_compat_path_holds(
	const gchar *folder,
	const gchar *path
){
	g_autofree gchar *a = axis_compat_path_normalize(folder, NULL);
	g_autofree gchar *b = axis_compat_path_normalize(path, NULL);

	return a != NULL && b != NULL && at_or_below(b, a);
}

gchar *
axis_compat_path_map_reverse(
	GPtrArray   *entries,
	const gchar *axis_path
){
	g_autofree gchar *path = axis_compat_path_normalize(axis_path, NULL);
	const AxisCompatPathEntry *best = NULL;
	const gchar *rest;
	guint i;

	if (path == NULL || entries == NULL)
		return NULL;
	for (i = 0; i < entries->len; i++) {
		const AxisCompatPathEntry *e = g_ptr_array_index(entries, i);

		if (at_or_below(path, e->axis) && (best == NULL || strlen(e->axis) > strlen(best->axis)))
			best = e;
	}
	if (best == NULL)
		return NULL;
	rest = strcmp(best->axis, "/") == 0 ? path : path + strlen(best->axis);
	if (rest[0] == '\0')
		return g_strdup(best->server);
	if (strcmp(best->server, "/") == 0)
		return g_strdup(rest);
	if (strlen(best->server) + strlen(rest) > AXIS_COMPAT_PATH_MAX_BYTES)
		return NULL;
	return g_strconcat(best->server, rest, NULL);
}

const AxisCompatRootRef *
axis_compat_root_for_path(
	GPtrArray    *roots,
	const gchar  *axis_path,
	gchar       **rel_path
){
	const AxisCompatRootRef *best = NULL;
	guint i;

	if (rel_path != NULL)
		*rel_path = NULL;
	if (roots == NULL || axis_path == NULL)
		return NULL;
	for (i = 0; i < roots->len; i++) {
		const AxisCompatRootRef *r = g_ptr_array_index(roots, i);

		if (strictly_below(axis_path, r->path) && (best == NULL || strlen(r->path) > strlen(best->path)))
			best = r;
	}
	if (best != NULL && rel_path != NULL) {
		const gchar *rest = strcmp(best->path, "/") == 0 ? axis_path + 1 : axis_path + strlen(best->path) + 1;

		*rel_path = g_strdup(rest);
	}
	return best;
}

/* ── Library mapping ────────────────────────────────────────────── */

void
axis_compat_mapping_clear(AxisCompatMapping *mapping)
{
	if (mapping == NULL)
		return;
	if (mapping->root_ids != NULL)
		g_array_unref(mapping->root_ids);
	if (mapping->unmapped_folders != NULL)
		g_ptr_array_unref(mapping->unmapped_folders);
	if (mapping->suggestions != NULL)
		g_ptr_array_unref(mapping->suggestions);
	memset(mapping, 0, sizeof(*mapping));
}

static gint
compare_int64(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

/* How many trailing path segments @a and @b have in common. */
static guint
common_tail(
	const gchar *a,
	const gchar *b
){
	gsize i = strlen(a);
	gsize j = strlen(b);
	guint count = 0;

	while (i > 0 && j > 0) {
		gsize ie = i;
		gsize je = j;

		while (i > 0 && a[i - 1] != '/')
			i--;
		while (j > 0 && b[j - 1] != '/')
			j--;
		if (ie - i == 0 || je - j == 0 || ie - i != je - j || g_ascii_strncasecmp(a + i, b + j, ie - i) != 0)
			break;
		count++;
		if (i > 0)
			i--;
		if (j > 0)
			j--;
	}
	return count;
}

void
axis_compat_map_library(
	GPtrArray         *folders,
	GPtrArray         *entries,
	GPtrArray         *roots,
	AxisCompatMapping *mapping
){
	guint reached = 0;
	guint i;
	guint j;

	memset(mapping, 0, sizeof(*mapping));
	mapping->root_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	mapping->unmapped_folders = g_ptr_array_new_with_free_func(g_free);
	mapping->suggestions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_suggestion_free);

	for (i = 0; folders != NULL && i < folders->len; i++) {
		const gchar *folder = g_ptr_array_index(folders, i);
		g_autofree gchar *mapped = axis_compat_path_map_apply(entries, folder);
		g_autofree gchar *plain = axis_compat_path_normalize(folder, NULL);
		gboolean found = FALSE;

		for (j = 0; mapped != NULL && roots != NULL && j < roots->len; j++) {
			const AxisCompatRootRef *r = g_ptr_array_index(roots, j);

			if (at_or_below(mapped, r->path) || at_or_below(r->path, mapped)) {
				gint64 id = r->id;
				guint k;
				gboolean seen = FALSE;

				for (k = 0; k < mapping->root_ids->len; k++)
					seen = seen || g_array_index(mapping->root_ids, gint64, k) == id;
				if (!seen)
					g_array_append_val(mapping->root_ids, id);
				found = TRUE;
			}
		}
		if (found) {
			reached++;
			continue;
		}
		g_ptr_array_add(mapping->unmapped_folders, g_strdup(folder));
		if (plain != NULL && roots != NULL) {
			const AxisCompatRootRef *best = NULL;
			guint best_n = 0;
			guint ties = 0;

			for (j = 0; j < roots->len; j++) {
				const AxisCompatRootRef *r = g_ptr_array_index(roots, j);
				guint n = common_tail(plain, r->path);

				if (n > best_n) {
					best_n = n;
					best = r;
					ties = 1;
				} else if (n == best_n && n > 0) {
					ties++;
				}
			}
			if (best != NULL && ties == 1) {
				AxisCompatSuggestion *s = g_new0(AxisCompatSuggestion, 1);

				s->folder = g_strdup(plain);
				s->root_id = best->id;
				s->root_path = g_strdup(best->path);
				g_ptr_array_add(mapping->suggestions, s);
			}
		}
	}
	g_array_sort(mapping->root_ids, compare_int64);
	if (folders == NULL || folders->len == 0 || reached == 0)
		mapping->state = AXIS_MAP_UNMAPPED;
	else if (reached < folders->len)
		mapping->state = AXIS_MAP_PARTLY;
	else
		mapping->state = AXIS_MAP_MAPPED;
}
