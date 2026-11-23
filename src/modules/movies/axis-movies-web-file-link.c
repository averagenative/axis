/*
 * axis-movies-web-file-link.c - "Link to a movie or episode by hand": the
 * entry from a file to the links pages (add-force-grab-and-assign, task 5.3)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET /library/file/:id/link[?q=text]
 *                                     shows the file (name, folder, size) and
 *                                     lets an administrator pick where it
 *                                     belongs: for a file in a library folder
 *                                     for movies, a movie Axis manages (a
 *                                     search by name, the results as links to
 *                                     /films/:id/links?file=N); for one in a
 *                                     library folder for TV, a series (links
 *                                     to /series/:id/links?file=N).  Writes
 *                                     nothing: the links pages check and
 *                                     change.  A file that is not a present
 *                                     video file of such a folder is told so
 *                                     in a sentence, with no form.
 *
 * An administrator's (AXIS_WEB_ROUTE_ADMIN).  The page is the Movies
 * module's because it holds the files' links for movies; the search of the
 * series it offers for a TV file only reads the series table.
 */

#include "modules/movies/axis-movies-web.h"

#include <string.h>

#include "core/axis-size.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-text.h"

#define ADD axis_web_storage_add
#define SHOWN 25
#define QUERY_MAX 100

typedef struct
{
	gint64   id;
	gchar   *path;		/* stored form */
	gchar   *root_name;
	gint64   size;
	gboolean usable;	/* a present video file of a library folder for @media */
	gchar   *media;		/* "movies", "tv" or another */
	gchar   *query;
	GPtrArray *found;	/* FoundTitle */
	gboolean more;
	gboolean missing;
	GError  *error;
} FileLinkWork;

typedef struct
{
	gint64  id;
	gchar  *label;
} FoundTitle;

static void
found_free(gpointer data)
{
	FoundTitle *found = data;

	g_free(found->label);
	g_free(found);
}

static void
file_link_free(gpointer data)
{
	FileLinkWork *work = data;

	g_free(work->path);
	g_free(work->root_name);
	g_free(work->media);
	g_free(work->query);
	g_clear_pointer(&work->found, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
file_link_run(
	AxisMovies *self,
	gpointer    data
){
	FileLinkWork *work = data;
	g_autoptr(AxisDbResult) file = NULL;
	g_autoptr(AxisDbResult) titles = NULL;
	g_autofree gchar *id = axis_movies_int_text(work->id);
	const gchar *params[2];
	const gchar *state;
	const gchar *kind;
	AxisDbConn *conn;
	gboolean movies;
	guint i;

	conn = axis_db_pool_acquire(self->pool, &work->error);
	if (conn == NULL)
		return;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT lf.rel_path, lf.size, lf.state, lf.kind, r.media_type, r.name FROM library_file lf"
	        " JOIN library_root r ON r.id = lf.root_id WHERE lf.id = $1::bigint", 1, params, &file, &work->error))
		goto out;
	if (axis_db_result_get_n_rows(file) == 0) {
		work->missing = TRUE;
		goto out;
	}
	work->path = g_strdup(axis_db_result_get_text(file, 0, 0));
	axis_db_result_get_int64(file, 0, 1, &work->size);
	state = axis_db_result_get_text(file, 0, 2);
	kind = axis_db_result_get_text(file, 0, 3);
	work->media = g_strdup(axis_db_result_get_text(file, 0, 4));
	work->root_name = g_strdup(axis_db_result_get_text(file, 0, 5));
	movies = g_strcmp0(work->media, "movies") == 0;
	work->usable = g_strcmp0(state, "present") == 0 && g_strcmp0(kind, "video") == 0 &&
		(movies || g_strcmp0(work->media, "tv") == 0);
	if (!work->usable || work->query == NULL)
		goto out;
	/* A name search: a managed title whose name holds the words (no pattern
	 * characters to quote). */
	params[0] = work->query;
	if (!axis_db_conn_exec(conn, movies ?
	        "SELECT m.id, t.title, t.year FROM movie m JOIN meta_title t ON t.id = m.title_id"
	        " WHERE position(lower($1::text) in lower(t.title)) > 0 ORDER BY t.title, t.year, m.id LIMIT 26" :
	        "SELECT s.id, t.title, t.year FROM series s JOIN meta_title t ON t.id = s.title_id"
	        " WHERE position(lower($1::text) in lower(t.title)) > 0 ORDER BY t.title, t.year, s.id LIMIT 26",
	        1, params, &titles, &work->error))
		goto out;
	work->found = g_ptr_array_new_with_free_func(found_free);
	for (i = 0; i < axis_db_result_get_n_rows(titles); i++) {
		FoundTitle *found;
		gint64 year = 0;

		if (work->found->len >= SHOWN) {
			work->more = TRUE;
			break;
		}
		found = g_new0(FoundTitle, 1);
		axis_db_result_get_int64(titles, i, 0, &found->id);
		axis_db_result_get_int64(titles, i, 2, &year);
		found->label = axis_movies_title_label(axis_db_result_get_text(titles, i, 1), (gint)year);
		g_ptr_array_add(work->found, found);
	}
out:
	axis_db_pool_release(self->pool, conn);
}

static HtmxResponse *
file_link_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	FileLinkWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *size = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *dir = NULL;
	const gchar *slash;
	const gchar *name;
	HtmxHeading *h1;
	HtmxNode *card;
	HtmxNode *facts;
	gboolean movies;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->missing)
		return axis_web_error_respond(request, 404, "Not found", "There is no such file.");
	movies = g_strcmp0(work->media, "movies") == 0;
	slash = strrchr(work->path, '/');
	name = slash != NULL ? slash + 1 : work->path;
	dir = slash != NULL ? g_strndup(work->path, (gsize)(slash - work->path)) : g_strdup("");
	size = axis_size_format((guint64)MAX(work->size, 0));
	action = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/link", work->id);
	back = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, work->id);

	h1 = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(h1), "Link a file to a movie or episode by hand");
	ADD(content, h1);
	card = axis_web_storage_card("The file");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "Name", axis_web_browser_name_node(name));
	axis_web_browser_fact_node(facts, "Folder", axis_web_browser_name_node(dir[0] != '\0' ? dir : "(the top of the library folder)"));
	axis_web_browser_fact(facts, "Library folder", work->root_name != NULL ? work->root_name : "");
	axis_web_browser_fact(facts, "Size", size);
	ADD(card, facts);
	ADD(content, card);

	if (!work->usable) {
		ADD(content, axis_web_storage_text("notice notice-warning", "Only a video file that is present in a library "
			"folder for movies or for TV can be linked by hand, and this file is not one."));
		ADD(content, axis_web_browser_link(back, "Back to the file"));
		return axis_web_page_respond(request, "Link a file by hand", HTMX_NODE(content));
	}
	card = axis_web_storage_card(movies ? "Which movie is it?" : "Which series is it?");
	{
		HtmxForm *form = htmx_form_new();

		htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);
		htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
		ADD(form, axis_web_storage_text_field("file-link-q", movies ? "Part of the movie's name" :
			"Part of the series' name", "q", work->query, NULL, movies ?
			"Search the movies Axis manages. The next page lists the file, shows what linking it changes, and changes "
			"nothing until you confirm." : "Search the series Axis manages. The next page lists the file, shows "
			"what linking it changes, and changes nothing until you confirm."));
		ADD(form, axis_web_auth_button("Search", "button-small"));
		ADD(card, form);
	}
	if (work->found != NULL) {
		if (work->found->len == 0) {
			ADD(card, axis_web_storage_text("help", movies ? "Axis manages no movie with that in its name. Add the "
				"movie first, then link the file to it." : "Axis manages no series with that in its name. Add the "
				"series first, then link the file to it."));
		} else {
			HtmxUl *list = htmx_ul_new();

			htmx_element_set_id(HTMX_ELEMENT(list), "file-link-results");
			for (i = 0; i < work->found->len; i++) {
				const FoundTitle *found = g_ptr_array_index(work->found, i);
				g_autofree gchar *href = g_strdup_printf(movies ? "/films/%" G_GINT64_FORMAT "/links?file=%"
					G_GINT64_FORMAT : "/series/%" G_GINT64_FORMAT "/links?file=%" G_GINT64_FORMAT, found->id, work->id);
				HtmxLi *item = htmx_li_new();

				ADD(item, axis_web_browser_link(href, found->label));
				ADD(list, item);
			}
			ADD(card, list);
			if (work->more)
				ADD(card, axis_web_storage_text("help", "There are more: type more of the name to narrow it."));
		}
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link(back, "Back to the file"));
	return axis_web_page_respond(request, "Link a file by hand", HTMX_NODE(content));
}

static HtmxResponse *
handle_file_link(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	FileLinkWork *work;
	const gchar *q = htmx_request_get_query_param(request, "q");
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such file.");
	work = g_new0(FileLinkWork, 1);
	work->id = id;
	if (q != NULL) {
		g_autofree gchar *cut = axis_movies_text_cut(q, QUERY_MAX);

		work->query = cut != NULL ? g_strstrip(g_steal_pointer(&cut)) : NULL;
		if (work->query != NULL && work->query[0] == '\0')
			g_clear_pointer(&work->query, g_free);
	}
	return axis_movies_web_dispatch(user_data, request, file_link_run, file_link_done, work, file_link_free);
}

void
axis_movies_web_register_file_link(
	AxisMovies *self,
	AxisWeb    *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/library/file/:id/link", AXIS_WEB_ROUTE_ADMIN, handle_file_link, self);
}
