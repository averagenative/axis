/*
 * axis-tv-web-view.c - the TV module's pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET /series                 the series as posters, each with how
 *                               complete it is and its worst state
 *                               (?state=<name> filters)
 *   GET /series/:id             a series: its facts, its seasons (each a
 *                               <details> with its monitoring and counts)
 *                               with their episodes, its history
 *                               (?season=N opens that season)
 *   GET /series/:id/releases    the latest search of an episode
 *                               (?episode=ID) or a season (?season=N): every
 *                               release with its verdict, its reasons and
 *                               what it holds; the blocklist
 *   GET /series/calendar        the episodes of the last week and the next
 *                               two, by the day they air
 *   GET /series/wanted          the episodes wanted or needing attention
 *   GET /series/history         what happened to every series
 *
 * Everything that came from a provider, an indexer or a client is set as
 * text.  Every form posts with the session's CSRF field and is drawn for
 * administrators only.
 */

#include "modules/tv/axis-tv-web.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-manual.h"
#include "web/axis-web-meta-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-titles.h"
#include "core/axis-size.h"
#include "release/axis-release.h"

#define ADD axis_web_storage_add

/* ── Small pieces ───────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *h = level == 1 ? htmx_h1_new() : level == 2 ? htmx_h2_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(h), text);
	return HTMX_NODE(h);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	if (node != NULL)
		ADD(cell, node);
	return HTMX_NODE(cell);
}

static HtmxNode *
cell_time(gint64 us)
{
	g_autofree gchar *text = axis_web_format_time(us, "");

	return cell_text(text);
}

static HtmxNode *
bdi_text(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text != NULL ? text : "");
	return HTMX_NODE(bdi);
}

static HtmxNode *
link_text(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);

	ADD(link, bdi_text(text));
	return HTMX_NODE(link);
}

/* A one-button form posting to @action with hidden fields (name, value
 * pairs ended by %NULL). */
static HtmxNode *
button_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *css,
	...
) G_GNUC_NULL_TERMINATED;

static HtmxNode *
button_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *css,
	...
){
	HtmxNode *form = axis_web_auth_post_form(request, action);
	const gchar *name;
	va_list ap;

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	va_start(ap, css);
	while ((name = va_arg(ap, const gchar *)) != NULL) {
		const gchar *value = va_arg(ap, const gchar *);

		ADD(form, axis_web_auth_hidden(name, value));
	}
	va_end(ap);
	ADD(form, axis_web_auth_button(label, css));
	return form;
}

static HtmxNode *
check_field(
	const gchar *id,
	const gchar *label_text,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *div = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, "yes");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	ADD(div, input);
	ADD(div, label);
	return HTMX_NODE(div);
}

static HtmxNode *
confirm_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *opener,
	const gchar *confirm_id,
	const gchar *confirm_text,
	const gchar *button,
	const gchar *name,
	const gchar *value
){
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(details), "film-confirm");
	htmx_node_set_text_content(HTMX_NODE(summary), opener);
	ADD(details, summary);
	htmx_element_add_class(HTMX_ELEMENT(form), "film-confirm-form");
	if (name != NULL)
		ADD(form, axis_web_auth_hidden(name, value));
	ADD(form, check_field(confirm_id, confirm_text, "confirm", FALSE));
	ADD(form, axis_web_auth_button(button, "button-danger button-small"));
	ADD(details, form);
	return HTMX_NODE(details);
}

static HtmxNode *
tab_item(
	const gchar *href,
	const gchar *label,
	guint        count,
	gboolean     current
){
	HtmxLi *item = htmx_li_new();
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxSpan *text = htmx_span_new();
	HtmxSpan *figure = htmx_span_new();
	g_autofree gchar *number = g_strdup_printf("%u", count);

	if (current) {
		htmx_element_add_class(HTMX_ELEMENT(link), "current");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "page");
	}
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_add_class(HTMX_ELEMENT(figure), "tab-count");
	htmx_node_set_text_content(HTMX_NODE(figure), number);
	ADD(link, text);
	ADD(link, figure);
	ADD(item, link);
	return HTMX_NODE(item);
}

/* An episode's quality, its size on disk and its copies: "WEB-DL 1080p,
 * 1.2 GiB", "Unknown, 2 copies". */
static gchar *
quality_text(const AxisTvEpisode *e)
{
	GString *text;

	if (e->n_files == 0)
		return g_strdup("");
	text = g_string_new(e->file_quality != AXIS_QUALITY_UNKNOWN ? axis_quality_label(e->file_quality) :
		e->file_resolution != AXIS_RELEASE_RESOLUTION_NONE ? axis_quality_unknown_source_label(e->file_resolution) :
		"Unknown");
	if (e->files_size > 0) {
		g_autofree gchar *size = axis_size_format((guint64)e->files_size);

		g_string_append_printf(text, ", %s", size);
	}
	if (e->n_files > 1)
		g_string_append_printf(text, ", %u copies", e->n_files);
	return g_string_free(text, FALSE);
}

static HtmxNode *
unplaced_card(const AxisTvDiskDetail *detail);

static HtmxNode *
folders_card(const AxisTvDiskDetail *detail);

/* ── Reading every series with its episodes ─────────────────────── */

typedef struct
{
	GPtrArray   *series;	/* AxisTvSeries */
	GPtrArray   *episodes;	/* AxisTvEpisode */
	GPtrArray   *summaries;	/* AxisTvSummary (the grid) */
	GPtrArray   *posters;	/* gchar *, per summary */
	AxisTvRules  rules;
	gboolean     filtered;
	AxisTvState  filter;
	guint        counts[AXIS_TV_N_STATES];
	gchar       *done;
	GError      *error;
	/* The list (the grid's page of it). */
	gint64       user_id;
	gchar       *name;
	gchar       *sort;
	gchar       *view_asked;
	gchar       *after;
	gchar       *before;
	AxisWebView  view;
	GArray      *sizes;	/* gint64, per summary */
	guint        matching;
	gchar       *next;
	gchar       *previous;
	/* The library roots for TV, by name ("Shows on Disk one"), for the words
	 * of an empty list. */
	GPtrArray   *tv_roots;	/* gchar * */
	/* The page's entries in order: a managed series (its summary, in
	 * @summaries) or a series on disk (in @disks); each entry's poster. */
	GPtrArray   *disks;	/* AxisTvOnDisk, the page's series on disk */
	GArray      *entries;	/* Entry */
	guint        on_disk;	/* series on disk in all (counted as in the library) */
	guint        review;	/* folders identification holds for a person */
	guint        waiting;	/* and for the provider */
	gboolean     identify_on;
	/* Wanted, state=held: only the episodes whose last search found a
	 * release only a person can grab. */
	gboolean     held;
	/* Wanted: how many episodes are wanted in all (the tab All). */
	guint        total;
} AllWork;

/* An entry of a page: index into @summaries or @disks. */
typedef struct
{
	gboolean disk;
	guint    index;
	gint64   size;
	gchar   *poster;
} Entry;

static void
all_work_free(gpointer data)
{
	AllWork *work = data;

	g_clear_pointer(&work->series, g_ptr_array_unref);
	g_clear_pointer(&work->episodes, g_ptr_array_unref);
	g_clear_pointer(&work->summaries, g_ptr_array_unref);
	g_clear_pointer(&work->posters, g_ptr_array_unref);
	g_free(work->done);
	g_clear_error(&work->error);
	g_free(work->name);
	g_free(work->sort);
	g_free(work->view_asked);
	g_free(work->after);
	g_free(work->before);
	g_clear_pointer(&work->sizes, g_array_unref);
	g_free(work->next);
	g_free(work->previous);
	g_clear_pointer(&work->tv_roots, g_ptr_array_unref);
	g_clear_pointer(&work->disks, g_ptr_array_unref);
	if (work->entries != NULL) {
		guint i;

		for (i = 0; i < work->entries->len; i++)
			g_free(g_array_index(work->entries, Entry, i).poster);
		g_array_unref(work->entries);
	}
	g_free(work);
}

static const AxisTvSeries *
series_of(
	const AllWork *work,
	gint64         id
){
	guint i;

	for (i = 0; i < work->series->len; i++) {
		const AxisTvSeries *s = g_ptr_array_index(work->series, i);

		if (s->id == id)
			return s;
	}
	return NULL;
}

/* ── The grid ───────────────────────────────────────────────────── */

/* A series as the list orders it: a managed one's summary, or a series on
 * disk; its size on disk. */
typedef struct
{
	AxisTvSummary *summary;	/* (nullable) */
	AxisTvOnDisk  *disk;	/* (nullable) */
	gint64         size;
} Listed;

static const gchar *
listed_title(const Listed *l)
{
	const gchar *title = l->summary != NULL ? l->summary->series->title : l->disk->title;

	return title != NULL ? title : "";
}

static gint
listed_year(const Listed *l)
{
	return l->summary != NULL ? l->summary->series->year : l->disk->year;
}

static gint64
listed_added(const Listed *l)
{
	return l->summary != NULL ? l->summary->series->added_us : l->disk->found_us;
}

/* An entry's key, unique: a managed series' id, a series on disk's title
 * id negated. */
static gint64
listed_id(const Listed *l)
{
	return l->summary != NULL ? l->summary->series->id : -l->disk->title_id;
}

static gint
compare_listed(
	gconstpointer a,
	gconstpointer b,
	gpointer      data
){
	const Listed *x = a;
	const Listed *y = b;
	const gchar *sort = data;
	gint64 ix = listed_id(x);
	gint64 iy = listed_id(y);
	g_autofree gchar *tx = NULL;
	g_autofree gchar *ty = NULL;
	gint c;

	if (g_strcmp0(sort, "size") == 0 && x->size != y->size)
		return x->size > y->size ? -1 : 1;
	if (g_strcmp0(sort, "year") == 0 && listed_year(x) != listed_year(y))
		return listed_year(x) > listed_year(y) ? -1 : 1;
	if (g_strcmp0(sort, "title") == 0 || g_strcmp0(sort, "year") == 0) {
		tx = g_utf8_casefold(listed_title(x), -1);
		ty = g_utf8_casefold(listed_title(y), -1);
		c = g_utf8_collate(tx, ty);
		if (c != 0)
			return c;
		if (listed_year(x) != listed_year(y))
			return listed_year(x) < listed_year(y) ? -1 : 1;
		return ix < iy ? -1 : ix > iy;
	}
	if (listed_added(x) != listed_added(y) && g_strcmp0(sort, "size") != 0)
		return listed_added(x) > listed_added(y) ? -1 : 1;
	return ix > iy ? -1 : ix < iy;
}

/* A page's cursor: a series' id, or a series on disk's title id negated;
 * 0 for none. */
static gint64
cursor_id(const gchar *text)
{
	gint64 value = 0;

	if (text == NULL || !g_ascii_string_to_signed(text, 10, -G_MAXINT64, G_MAXINT64, &value, NULL))
		return 0;
	return value;
}

static void
listed_free(Listed *l)
{
	axis_tv_summary_free(l->summary);
	axis_tv_on_disk_free(l->disk);
	l->summary = NULL;
	l->disk = NULL;
}

/*
 * A page of the series: the managed ones and the series on disk.  A managed
 * series' state is the worst of its episodes', so the tabs' counts need every
 * managed series' state: the episodes are counted by the database, by what
 * a state is derived from, and no episode is read (axis_tv_store_overview():
 * two statements whatever the library's size), with every series' size and
 * whether its name fits what was typed (one statement); the series on disk
 * are read in one pass over identification's linked units
 * (axis_tv_store_on_disk()), each counted as in the library.  Then
 * everything is ordered and cut to the page, and only the page's are read
 * further: its posters (one statement), the episodes of its series that need
 * attention (one statement, for what is wrong and where it is put right) and
 * its series on disk's episodes (one statement).  The folders identification
 * holds for a person are counted in one more.
 */
static void
grid_run(
	AxisTv   *self,
	gpointer  data
){
	AllWork *work = data;
	AxisDbConn *conn;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GPtrArray) disks = NULL;
	g_autoptr(GArray) listed = g_array_new(FALSE, FALSE, sizeof(Listed));
	g_autoptr(GHashTable) facts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *key = work->name != NULL ? axis_release_title_key(work->name) : NULL;
	g_autofree const gchar **providers = NULL;
	g_autofree const gchar **refs = NULL;
	g_autofree gchar **hashes = NULL;
	gboolean disk_listed = !work->filtered || work->filter == AXIS_TV_IN_LIBRARY;
	guint limit;
	guint start = 0;
	guint end;
	gint64 after = cursor_id(work->after);
	gint64 before = after == 0 ? cursor_id(work->before) : 0;
	gboolean ok;
	guint n;
	guint i;

	axis_tv_get_rules(self, &work->rules);
	axis_web_view_choose(self->pool, work->user_id, "series", work->view_asked, &work->view);
	limit = axis_web_view_page_size(work->view);
	if (key != NULL && key[0] == '\0')
		g_clear_pointer(&key, g_free);
	if (key != NULL) {
		guint filled = 0;

		do {
			if (!axis_meta_fill_name_keys(self->meta, 2000, &filled, &work->error))
				return;
		} while (filled == 2000);
	}
	conn = axis_db_pool_acquire(self->pool, &work->error);
	if (conn == NULL)
		return;
	{
		const gchar *params[1] = { key };

		/* Counted by the database, no episode read (axis_tv_store_overview());
		 * every series' size on disk in one pass over the series' files. */
		ok = axis_tv_store_overview(self, conn, NULL, AXIS_TV_MEMBERS_OF_NONE, &all, NULL, NULL, &work->error) &&
			axis_db_conn_exec(conn, "SELECT s.id, coalesce(z.size, 0),"
				" $1::text IS NULL OR t.name_keys LIKE '%' || $1::text || '%'"
				" FROM series s JOIN meta_title t ON t.id = s.title_id"
				" LEFT JOIN (SELECT sf.series_id, sum(lf.size)::bigint AS size FROM series_file sf"
				"  JOIN library_file lf ON lf.id = sf.file_id WHERE lf.state = 'present' GROUP BY sf.series_id) z"
				"  ON z.series_id = s.id", 1, params, &result, &work->error) &&
			axis_tv_store_on_disk(conn, NULL, key, &disks, &work->error) &&
			axis_tv_store_held(conn, &work->review, &work->waiting, &work->error);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return;
	/* The roots for TV, for the words of an empty list. */
	{
		g_autoptr(GPtrArray) roots = NULL;
		g_autoptr(GError) local = NULL;

		work->tv_roots = g_ptr_array_new_with_free_func(g_free);
		if (axis_storage_list_roots(self->storage, 0, &roots, &local)) {
			for (i = 0; i < roots->len; i++) {
				const AxisRootInfo *root = g_ptr_array_index(roots, i);

				if (root->media_type == AXIS_MEDIA_TV)
					g_ptr_array_add(work->tv_roots, g_strdup(root->name));
			}
		} else {
			g_debug("series: the roots could not be read: %s", local->message);
		}
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 *id = g_new(gint64, 1);
		gint64 *size = g_new(gint64, 2);
		gboolean match = FALSE;

		axis_db_result_get_int64(result, i, 0, id);
		axis_db_result_get_int64(result, i, 1, &size[0]);
		axis_db_result_get_boolean(result, i, 2, &match);
		size[1] = match;
		g_hash_table_insert(facts, id, size);
	}
	work->summaries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_summary_free);
	work->disks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_on_disk_free);
	work->entries = g_array_new(FALSE, TRUE, sizeof(Entry));
	while (all->len > 0) {
		AxisTvSummary *summary = g_ptr_array_steal_index(all, 0);
		const gint64 *found = g_hash_table_lookup(facts, &summary->series->id);
		Listed item;

		/* Not of the name typed: not counted. */
		if (found == NULL || found[1] == 0) {
			axis_tv_summary_free(summary);
			continue;
		}
		work->counts[summary->state]++;
		if (work->filtered && summary->state != work->filter) {
			axis_tv_summary_free(summary);
			continue;
		}
		memset(&item, 0, sizeof(item));
		item.summary = summary;
		item.size = found[0];
		g_array_append_val(listed, item);
	}
	/* The series on disk: each in the library, never in another state. */
	work->on_disk = disks->len;
	work->counts[AXIS_TV_IN_LIBRARY] += disks->len;
	while (disks->len > 0) {
		AxisTvOnDisk *disk = g_ptr_array_steal_index(disks, 0);
		Listed item;

		if (!disk_listed) {
			axis_tv_on_disk_free(disk);
			continue;
		}
		memset(&item, 0, sizeof(item));
		item.disk = disk;
		item.size = disk->size;
		g_array_append_val(listed, item);
	}
	g_array_sort_with_data(listed, compare_listed, work->sort != NULL ? work->sort : "added");
	work->matching = listed->len;

	/* The page: after an entry, before one, or the first (an entry no longer
	 * listed: the first). */
	end = MIN(limit, listed->len);
	for (i = 0; i < listed->len; i++) {
		gint64 id = listed_id(&g_array_index(listed, Listed, i));

		if (after != 0 && id == after) {
			start = i + 1;
			end = MIN(start + limit, listed->len);
		} else if (before != 0 && id == before) {
			end = i;
			start = end > limit ? end - limit : 0;
		}
	}
	if (end < listed->len && end > start)
		work->next = g_strdup_printf("%" G_GINT64_FORMAT, listed_id(&g_array_index(listed, Listed, end - 1)));
	if (start > 0 && end > start)
		work->previous = g_strdup_printf("%" G_GINT64_FORMAT, listed_id(&g_array_index(listed, Listed, start)));
	n = end > start ? end - start : 0;
	providers = g_new0(const gchar *, MAX(n, 1));
	refs = g_new0(const gchar *, MAX(n, 1));
	hashes = g_new0(gchar *, MAX(n, 1));
	for (i = 0; i < listed->len; i++) {
		Listed *item = &g_array_index(listed, Listed, i);
		Entry entry;

		if (i < start || i >= end) {
			listed_free(item);
			continue;
		}
		memset(&entry, 0, sizeof(entry));
		entry.disk = item->disk != NULL;
		entry.size = item->size;
		if (item->disk != NULL) {
			entry.index = work->disks->len;
			providers[i - start] = item->disk->provider;
			refs[i - start] = item->disk->poster_ref;
			g_ptr_array_add(work->disks, item->disk);
		} else {
			entry.index = work->summaries->len;
			providers[i - start] = item->summary->series->provider;
			refs[i - start] = item->summary->series->poster_ref;
			g_ptr_array_add(work->summaries, item->summary);
		}
		g_array_append_val(work->entries, entry);
	}
	/* The page's posters at once, and its series on disk's episodes. */
	if (n > 0)
		axis_meta_artwork_find_many(self->meta, providers, refs, n, AXIS_META_ART_POSTER_SMALL, TRUE, hashes);
	for (i = 0; i < work->entries->len; i++)
		g_array_index(work->entries, Entry, i).poster = hashes[i];
	/* Only the page's are read further: the episodes of its series that need
	 * attention, for what is wrong with each and where it is put right (one
	 * statement for all of them), and its series on disk's episodes. */
	for (i = 0, n = 0; i < work->summaries->len; i++) {
		const AxisTvSummary *summary = g_ptr_array_index(work->summaries, i);

		n += summary->counts[AXIS_TV_ATTENTION] > 0 ? 1 : 0;
	}
	if (work->disks->len == 0 && n == 0)
		return;
	conn = axis_db_pool_acquire(self->pool, &work->error);
	if (conn == NULL)
		return;
	if (axis_tv_store_overview_attention(self, conn, work->summaries, 0, work->summaries->len, &work->error) &&
	    work->disks->len > 0)
		axis_tv_store_on_disk_counts(conn, work->disks, work->rules.today, &work->error);
	axis_db_pool_release(self->pool, conn);
}

/* What identification holds back, and where to see it: the series folders
 * on disk that are not in the list yet. */
static HtmxNode *
review_line(
	guint    review,
	guint    waiting,
	gboolean identify_on
){
	HtmxP *line = htmx_p_new();
	GString *text = g_string_new(NULL);

	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	htmx_element_add_class(HTMX_ELEMENT(line), "film-review");
	if (review > 0) {
		g_autofree gchar *count = axis_web_count(review, "series folder or file on disk is", "series folders and files "
			"on disk are");

		g_string_append_printf(text, "%s not listed yet: identification was not sure which series %s, and waits for "
			"a person to say. ", count, review == 1 ? "it is" : "each is");
	}
	if (waiting > 0) {
		g_autofree gchar *count = review > 0 ? axis_web_count(waiting, "more waits", "more wait") :
			axis_web_count(waiting, "series folder or file on disk waits", "series folders and files on disk wait");

		g_string_append_printf(text, "%s for TMDb, which did not answer; Identify asks again "
			"by itself. ", count);
	}
	if (identify_on) {
		htmx_node_set_text_content(HTMX_NODE(line), text->str);
		ADD(line, axis_web_browser_link("/identify", review > 0 ? "Review the waiting folders on the Identify page" :
			"See the waiting folders on the Identify page"));
	} else {
		g_string_append(text, "Identify is switched off: an administrator can switch it on under Modules.");
		htmx_node_set_text_content(HTMX_NODE(line), text->str);
	}
	g_string_free(text, TRUE);
	return HTMX_NODE(line);
}

static const AxisTvState grid_order[] = {
	AXIS_TV_ATTENTION, AXIS_TV_WANTED, AXIS_TV_TO_IMPORT, AXIS_TV_SEARCHING, AXIS_TV_DOWNLOADING,
	AXIS_TV_UPGRADE_WANTED, AXIS_TV_IN_LIBRARY, AXIS_TV_NOT_AIRED, AXIS_TV_UNMONITORED
};

static HtmxResponse *
grid_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	AllWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNav *tabs;
	HtmxUl *list;
	const gchar *done;
	guint total = 0;
	guint i;

	static const gchar *const sorts[] = { "added", "title", "year", "size", NULL };
	static const gchar *const sort_labels[] = { "Added", "Title", "Year", "Size", NULL };
	AxisWebListAddress address;
	guint n;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	for (i = 0; i < AXIS_TV_N_STATES; i++)
		total += work->counts[i];
	memset(&address, 0, sizeof(address));
	address.path = "/series";
	address.state = work->filtered ? axis_tv_state_name(work->filter) : NULL;
	address.name = work->name;
	address.sort = work->sort;
	address.default_sort = "added";
	address.view = work->view;
	address.after = work->after;
	address.before = work->before;
	ADD(content, heading(1, "All series"));
	done = axis_tv_web_done_message(work->done);
	if (done != NULL)
		ADD(content, axis_web_auth_notice(FALSE, done));
	tabs = htmx_nav_new();
	htmx_element_add_class(HTMX_ELEMENT(tabs), "kind-tabs");
	htmx_element_set_attribute(HTMX_ELEMENT(tabs), "aria-label", "TV by state");
	list = htmx_ul_new();
	ADD(list, axis_web_list_tab_new(&address, NULL, "All", total));
	for (i = 0; i < G_N_ELEMENTS(grid_order); i++) {
		AxisTvState state = grid_order[i];

		if (work->counts[state] == 0 && !(work->filtered && work->filter == state))
			continue;
		ADD(list, axis_web_list_tab_new(&address, axis_tv_state_name(state), axis_tv_state_label(state),
			work->counts[state]));
	}
	ADD(tabs, list);
	ADD(content, tabs);
	ADD(content, axis_web_list_bar_new(&address, "series", sorts, sort_labels));
	if (work->review > 0 || work->waiting > 0)
		ADD(content, review_line(work->review, work->waiting, work->identify_on));
	n = work->entries->len;
	if (n == 0 && total == 0 && work->name == NULL && work->tv_roots != NULL && work->tv_roots->len > 0) {
		/* There are roots for TV, and nothing of them is listed: say why,
		 * not "none". */
		g_autofree gchar *roots = NULL;
		g_autofree gchar *text = NULL;

		g_ptr_array_add(work->tv_roots, NULL);
		roots = g_strjoinv(", ", (gchar **)work->tv_roots->pdata);
		g_ptr_array_remove_index(work->tv_roots, work->tv_roots->len - 1);
		if (work->identify_on)
			text = g_strdup_printf("No series are listed yet. Identify looks at the series folders in %s %s after "
				"each scan and lists here every series it recognises, with what of it is on disk; a folder it is not "
				"sure about waits on the Identify page. A series you add here is listed too.",
				work->tv_roots->len == 1 ? "the TV library folder" : "the TV library folders", roots);
		else
			text = g_strdup_printf("No series are listed yet. Identify is switched off, so the series in %s %s are "
				"not recognised: an administrator can switch it on under Modules. A series you add here is listed "
				"too.", work->tv_roots->len == 1 ? "the TV library folder" : "the TV library folders", roots);
		ADD(content, axis_web_storage_text("no-results", text));
	} else if (n == 0) {
		ADD(content, axis_web_storage_text("no-results", total == 0 && work->name == NULL ? "No series yet. Find one "
			"with the search box above and add it from its page." : work->name != NULL ? "No series' name matches." :
			"No series is in this state. Choose All to see every series."));
	} else {
		g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
		AxisWebTitleRow *rows = g_new0(AxisWebTitleRow, n);

		for (i = 0; i < n; i++) {
			const Entry *entry = &g_array_index(work->entries, Entry, i);
			gint64 size = entry->size;
			GString *facts;
			gchar *href;
			gchar *episodes;

			if (entry->disk) {
				const AxisTvOnDisk *d = g_ptr_array_index(work->disks, entry->index);
				gchar *reason = axis_tv_web_disk_reason(d);

				href = g_strdup_printf("/series/on-disk/%" G_GINT64_FORMAT, d->title_id);
				episodes = axis_tv_on_disk_episodes_text(d);
				facts = g_string_new("On disk, not managed by Axis");
				g_string_append_printf(facts, ", %s", episodes);
				if (d->n_folders > 1)
					g_string_append_printf(facts, ", in %u folders", d->n_folders);
				if (d->n_away > 0)
					g_string_append(facts, ", a disk offline");
				g_ptr_array_add(owned, reason);
				rows[i].title = d->title;
				rows[i].year = d->year;
				rows[i].provider = d->provider;
				rows[i].poster_ref = d->poster_ref;
				rows[i].mark = axis_tv_web_mark(AXIS_TV_IN_LIBRARY);
				rows[i].state = axis_tv_state_label(AXIS_TV_IN_LIBRARY);
				rows[i].reason = reason;
				rows[i].added_us = d->found_us;
				rows[i].extra = axis_tv_web_disk_completeness(d);
			} else {
				const AxisTvSummary *summary = g_ptr_array_index(work->summaries, entry->index);
				const AxisTvSeries *s = summary->series;

				href = g_strdup_printf("/series/%" G_GINT64_FORMAT, s->id);
				episodes = g_strdup_printf("%u of %u aired %s", summary->in_library, summary->aired_monitored,
					summary->aired_monitored == 1 ? "episode" : "episodes");
				facts = g_string_new(NULL);
				if (s->profile_name != NULL)
					g_string_append_printf(facts, "Quality profile %s", s->profile_name);
				g_string_append_printf(facts, "%s%s", facts->len > 0 ? ", " : "", episodes);
				rows[i].title = s->title;
				rows[i].year = s->year;
				rows[i].provider = s->provider;
				rows[i].poster_ref = s->poster_ref;
				rows[i].mark = axis_tv_web_mark(summary->state);
				rows[i].state = axis_tv_state_label(summary->state);
				rows[i].reason = summary->reason;
				rows[i].attention = summary->state == AXIS_TV_ATTENTION;
				/* What needs attention links to where it is put right. */
				if (rows[i].attention) {
					gchar *where = axis_tv_web_summary_href(summary);

					g_ptr_array_add(owned, where);
					rows[i].reason_href = where;
				}
				rows[i].added_us = s->added_us;
				rows[i].extra = axis_tv_web_completeness(summary);
			}
			if (size > 0) {
				g_autofree gchar *text = axis_size_format((guint64)size);

				g_string_append_printf(facts, ", %s", text);
			}
			g_ptr_array_add(owned, href);
			g_ptr_array_add(owned, episodes);
			g_ptr_array_add(owned, g_string_free(facts, FALSE));
			rows[i].href = href;
			rows[i].kind = AXIS_META_KIND_SERIES;
			rows[i].poster = entry->poster;
			rows[i].facts = g_ptr_array_index(owned, owned->len - 1);
			rows[i].quality = episodes;
			rows[i].size = size > 0 ? size : -1;
		}
		ADD(content, axis_web_titles_new(work->view, "Series", "Episodes", rows, n));
		for (i = 0; i < n; i++)
			axis_web_title_row_clear(&rows[i]);
		g_free(rows);
	}
	ADD(content, axis_web_list_foot_new(&address, work->matching, n, "series", "series", work->next,
		work->previous));
	if (work->on_disk > 0)
		ADD(content, axis_web_storage_text("help", "A series marked On disk, not managed by Axis is one Identify "
			"recognised in a TV library folder: Axis never searches for, downloads, renames or moves anything of it. "
			"Open one and choose Manage this series to have Axis look after it; its episodes on disk then count as "
			"in the library."));
	return axis_web_page_respond(request, "All series", HTMX_NODE(content));
}

/* A query value as it may be used: valid UTF-8 of at most @max bytes and
 * not empty, else %NULL. */
static gchar *
query_value(
	HtmxRequest *request,
	const gchar *name,
	gsize        max
){
	const gchar *value = htmx_request_get_query_param(request, name);
	g_autofree gchar *stripped = NULL;

	if (value == NULL || strlen(value) > max || !g_utf8_validate(value, -1, NULL))
		return NULL;
	stripped = g_strstrip(g_strdup(value));
	return stripped[0] != '\0' ? g_steal_pointer(&stripped) : NULL;
}

static HtmxResponse *
handle_grid(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AllWork *work = g_new0(AllWork, 1);
	const AxisAuthState *auth = axis_web_auth_state(request);
	const gchar *state = htmx_request_get_query_param(request, "state");
	const gchar *done = htmx_request_get_query_param(request, "done");
	g_autofree gchar *sort = query_value(request, "sort", 16);
	g_autofree gchar *view = query_value(request, "view", 16);
	AxisWebView viewed;

	work->filtered = state != NULL && axis_tv_state_from_name(state, &work->filter);
	if (axis_tv_web_done_message(done) != NULL)
		work->done = g_strdup(done);
	work->user_id = auth != NULL ? auth->user_id : 0;
	if (sort != NULL && (strcmp(sort, "title") == 0 || strcmp(sort, "year") == 0 || strcmp(sort, "size") == 0 ||
	                     strcmp(sort, "added") == 0))
		work->sort = g_steal_pointer(&sort);
	if (view != NULL && axis_web_view_from_name(view, &viewed))
		work->view_asked = g_steal_pointer(&view);
	work->name = query_value(request, "q", 300);
	work->after = query_value(request, "after", 24);
	work->before = query_value(request, "before", 24);
	{
		AxisWeb *web = axis_web_from_request(request);
		AxisModuleRegistry *registry = web != NULL ? axis_web_get_registry(web) : NULL;

		work->identify_on = registry != NULL && axis_module_registry_is_enabled(registry, AXIS_IDENTIFY_MODULE_ID);
	}
	return axis_tv_web_dispatch(user_data, request, grid_run, grid_done, work, all_work_free);
}

/* ── A series' page ─────────────────────────────────────────────── */

typedef struct
{
	gint64          series_id;
	gint            open_season;	/* -1: the default */
	gchar          *done;
	AxisTvSeries   *series;
	GPtrArray      *episodes;
	AxisTvSummary  *summary;
	GPtrArray      *events;
	gchar          *poster;
	AxisTvChoices   choices;
	gboolean        choices_read;
	AxisTvRules     rules;
	AxisTvDiskDetail *disk;	/* (nullable) what Identify found of it on disk */
	AxisTvWork     *work;	/* its searches in hand and lately ended */
	gboolean        lines_only;	/* the live fragment of its lines */
	gboolean        start_only;	/* the confirmation of "Let Axis search for it" */
	AxisWebNotice  *notice;	/* (nullable) the page's, while it is drawn */
	/* Its numbering (add-metadata-tvdb): the other source it may follow, and
	 * why the one it follows is not refreshed. */
	gchar          *numbering_to;	/* (nullable) */
	gchar          *not_refreshed;	/* (nullable) */
	GPtrArray      *misplaced;	/* (nullable) AxisTvMisplaced: files whose names say another episode */
	gboolean        tvdb_key;	/* Axis has a TheTVDB key (what an anime series without numbers is told) */
	GError         *error;
} SeriesWork;

static void
series_work_free(gpointer data)
{
	SeriesWork *work = data;

	g_clear_pointer(&work->misplaced, g_ptr_array_unref);
	g_free(work->numbering_to);
	g_free(work->not_refreshed);

	/* The summary owns the series. */
	if (work->summary != NULL)
		axis_tv_summary_free(work->summary);
	else
		axis_tv_series_free(work->series);
	g_clear_pointer(&work->episodes, g_ptr_array_unref);
	g_clear_pointer(&work->events, g_ptr_array_unref);
	axis_tv_work_free(work->work);
	g_free(work->poster);
	g_free(work->done);
	axis_tv_choices_clear(&work->choices);
	axis_tv_disk_detail_free(work->disk);
	g_clear_error(&work->error);
	g_free(work);
}

static void
series_run(
	AxisTv   *self,
	gpointer  data
){
	SeriesWork *work = data;
	g_autoptr(GError) local = NULL;

	axis_tv_get_rules(self, &work->rules);
	if (!axis_tv_get(self, work->series_id, &work->series, &work->episodes, &work->error))
		return;
	work->summary = axis_tv_summary_new(work->series, work->episodes, &work->rules);
	/* Its searches in hand and lately ended: two statements for every line. */
	if (!axis_tv_get_work(self, work->series_id, AXIS_WEB_ACTIVITY_OUTCOME_S, &work->work, &work->error))
		return;
	/* The live fragment needs nothing more, nor the confirmation. */
	if (work->lines_only || work->start_only)
		return;
	if (!axis_tv_list_events(self, work->series_id, 0, AXIS_TV_WEB_EVENTS_SHOWN, &work->events, &work->error))
		return;
	work->poster = axis_meta_artwork_find(self->meta, work->series->provider, work->series->poster_ref,
		AXIS_META_ART_POSTER_SMALL, TRUE);
	{
		g_autoptr(GPtrArray) numbering = axis_tv_numbering_choices(self, work->series);

		if (numbering->len > 1)
			work->numbering_to = g_strdup(g_ptr_array_index(numbering, 1));
		work->not_refreshed = axis_tv_numbering_not_refreshed(self, work->series);
	}
	work->tvdb_key = axis_tv_tvdb_offered(self);
	if (!axis_tv_list_misplaced(self, work->series_id, 0, &work->misplaced, NULL, NULL, &local)) {
		g_debug("series: the files whose names say another episode could not be read: %s", local->message);
		g_clear_error(&local);
	}
	work->choices_read = axis_tv_web_read_choices(self, &work->choices, &local);
	/* What Identify found of it on disk: the files on no episode, the
	 * folders. */
	g_clear_error(&local);
	if (!axis_tv_get_disk_detail(self, work->series->title_id, &work->disk, &local))
		g_debug("series: what is on disk of a series could not be read: %s", local->message);
}

static const gchar *
event_label(const gchar *kind)
{
	static const struct
	{
		const gchar *kind;
		const gchar *label;
	} labels[] = {
		{ "added", "Added" }, { "removed", "Removed" }, { "changed", "Changed" }, { "monitored", "Monitoring" },
		{ "episodes", "Episode list updated" }, { "searched", "Searched" }, { "skipped", "Not searched" },
		{ "grabbed", "Grabbed" }, { "not_sent", "Not sent" }, { "failed", "Failed" },
		{ "blocklisted", "Blocklisted" }, { "unblocked", "Off the blocklist" }, { "removal", "Removed at the download client" },
		{ "completed", "Downloaded" }, { "abandoned", "Given up" }, { "imported", "Imported" },
		{ "import_blocked", "Import blocked" }, { "import_waiting", "Import waits" },
		{ "import_action", "Import decision" }, { "upgraded", "Upgraded" }, { "identified", "Found on disk" },
		{ "unidentified", "No longer found on disk" }, { NULL, NULL }
	};
	guint i;

	for (i = 0; kind != NULL && labels[i].kind != NULL; i++) {
		if (strcmp(labels[i].kind, kind) == 0)
			return labels[i].label;
	}
	return kind != NULL ? kind : "";
}

static const AxisWebColumn event_columns[] = {
	{ "When", AXIS_WEB_COL_DATE },
	{ "What", AXIS_WEB_COL_TEXT },
	{ "Details", AXIS_WEB_COL_WIDE },
	{ "By", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxNode *
events_card(const GPtrArray *events)
{
	HtmxNode *card = axis_web_storage_card("History");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (events == NULL || events->len == 0) {
		ADD(card, axis_web_storage_text("help", "Nothing has happened yet."));
		return card;
	}
	table = axis_web_table_new("series-events", event_columns);
	body = htmx_tbody_new();
	for (i = 0; i < events->len; i++) {
		const AxisTvEvent *e = g_ptr_array_index(events, i);
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_time(e->created_us));
		ADD(row, cell_text(event_label(e->kind)));
		ADD(row, cell_node(axis_web_breakable(e->text)));
		ADD(row, cell_text(e->by_user != NULL ? e->by_user : "Axis"));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const AxisWebColumn episode_columns[] = {
	{ "Episode", AXIS_WEB_COL_TEXT },
	{ "Title", AXIS_WEB_COL_NAME },
	{ "Air date", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

/* The same for a series marked as anime: with the absolute number TheTVDB
 * gives each episode, empty where it gives none (add-anime-support, A1). */
static const AxisWebColumn episode_columns_anime[] = {
	{ "Episode", AXIS_WEB_COL_TEXT },
	{ "Absolute", AXIS_WEB_COL_TEXT },
	{ "Title", AXIS_WEB_COL_NAME },
	{ "Air date", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

/* An empty, hidden place for an act the page does not offer now, so that a
 * live update can put one there later. */
static HtmxNode *
no_act(const gchar *id)
{
	HtmxSpan *act = htmx_span_new();

	htmx_element_set_id(HTMX_ELEMENT(act), id);
	htmx_element_add_class(HTMX_ELEMENT(act), "activity-act");
	htmx_element_set_attribute(HTMX_ELEMENT(act), "hidden", "hidden");
	return HTMX_NODE(act);
}

/* An episode's search button.  One with no air date is never searched for by
 * itself: a person may all the same.  While a search does its work, or just
 * did, the button says so and cannot be pressed; none while it is in
 * flight. */
static HtmxNode *
episode_search_act(
	HtmxRequest         *request,
	const SeriesWork    *work,
	const AxisTvEpisode *e
){
	AxisTvState state = axis_tv_episode_derive(e, work->series, &work->rules, NULL);
	gboolean undated = state == AXIS_TV_NOT_AIRED && e->air_date == NULL;
	g_autofree gchar *episode = axis_tv_episode_label(e->season, e->number);
	g_autofree gchar *search = g_strdup_printf("Search for %s", episode);

	if (axis_tv_episode_in_flight(e)) {
		g_autofree gchar *id = g_strdup_printf("act-episode-%" G_GINT64_FORMAT, e->id);

		return no_act(id);
	}
	return axis_tv_web_search_act(request, e->series_id, AXIS_TV_SCOPE_EPISODE, e->id, e->season, FALSE, work->work,
		undated ? "Search anyway" : e->last_search_us > 0 && state != AXIS_TV_IN_LIBRARY ? "Search again now" :
		search, "button-small", NULL);
}

/* What a season offers: how many of its monitored episodes aired, and how
 * many have no air date (and are not in flight). */
static void
season_counts(
	const SeriesWork *work,
	gint              season,
	guint            *aired,
	guint            *undated
){
	guint i;

	*aired = 0;
	*undated = 0;
	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

		if (e->season != season || e->gone)
			continue;
		if (e->monitored && axis_tv_episode_aired(e, &work->rules, NULL))
			(*aired)++;
		if (axis_tv_episode_derive(e, work->series, &work->rules, NULL) == AXIS_TV_NOT_AIRED && e->air_date == NULL &&
		    !axis_tv_episode_in_flight(e))
			(*undated)++;
	}
}

/* A season's acts: Search the season (once one has aired), Search all with
 * no air date (two or more), each an empty place otherwise. */
static HtmxNode *
season_search_act(
	HtmxRequest      *request,
	const SeriesWork *work,
	gint              season,
	gboolean          undated_act
){
	guint aired = 0;
	guint undated = 0;
	g_autofree gchar *id = g_strdup_printf(undated_act ? "act-undated-%d" : "act-season-%d", season);
	g_autofree gchar *label = NULL;

	season_counts(work, season, &aired, &undated);
	if (!undated_act)
		return aired > 0 ? axis_tv_web_search_act(request, work->series->id, AXIS_TV_SCOPE_SEASON, season, season,
			FALSE, work->work, "Search for this season", "button-small", NULL) : no_act(id);
	if (undated <= 1)
		return no_act(id);
	label = g_strdup_printf("Search all %u with no air date", undated);
	return axis_tv_web_search_act(request, work->series->id, AXIS_TV_SCOPE_SEASON, season, season, TRUE, work->work,
		label, "button-small", NULL);
}

/* Search all wanted, while anything is wanted; while every wanted episode is
 * in a search, a button that says so. */
static HtmxNode *
series_search_act(
	HtmxRequest      *request,
	const SeriesWork *work
){
	const AxisTvSummary *summary = work->summary;

	if (summary->counts[AXIS_TV_WANTED] + summary->counts[AXIS_TV_ATTENTION] +
	    summary->counts[AXIS_TV_UPGRADE_WANTED] > 0)
		return axis_tv_web_search_act(request, work->series->id, AXIS_TV_SCOPE_SERIES, 0, -1, FALSE, work->work,
			"Search for every wanted episode", "button-primary button-small", NULL);
	if (summary->counts[AXIS_TV_SEARCHING] > 0) {
		g_autofree gchar *label = g_strdup_printf("Searching for %u", summary->counts[AXIS_TV_SEARCHING]);
		HtmxSpan *act = htmx_span_new();

		htmx_element_set_id(HTMX_ELEMENT(act), "act-series");
		htmx_element_add_class(HTMX_ELEMENT(act), "activity-act");
		ADD(act, axis_web_activity_busy_button(label, "Every wanted episode is in a search that is queued or "
			"running: the line at the top says where they stand."));
		return HTMX_NODE(act);
	}
	return no_act("act-series");
}

static HtmxNode *
episode_row(
	HtmxRequest         *request,
	const SeriesWork    *work,
	const AxisTvEpisode *e,
	gboolean             admin
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxDiv *controls = htmx_div_new();
	g_autofree gchar *label = axis_tv_episode_label(e->season, e->number);
	g_autofree gchar *reason = NULL;
	g_autofree gchar *releases = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT,
		e->series_id, e->id);
	g_autofree gchar *anchor = g_strdup_printf("episode-%" G_GINT64_FORMAT, e->id);
	AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;
	AxisTvState state = axis_tv_episode_derive_attention(e, work->series, &work->rules, &reason, &kind);
	g_autofree gchar *css = g_strdup_printf("episode-%s", axis_tv_state_name(state));

	/* Each row is a place a link can lead to (what needs attention). */
	htmx_element_set_id(HTMX_ELEMENT(row), anchor);
	htmx_element_add_class(HTMX_ELEMENT(row), css);
	ADD(row, cell_text(label));
	if (work->series->anime) {
		/* TheTVDB's own number of it, under the aired order the series
		 * follows; nothing where TheTVDB gives none, or under a numbering
		 * that has none. */
		g_autofree gchar *absolute = e->absolute > 0 ? g_strdup_printf("%d", e->absolute) : g_strdup("");
		HtmxNode *cell = cell_text(absolute);

		htmx_element_add_class(HTMX_ELEMENT(cell), "episode-absolute");
		ADD(row, cell);
	}
	ADD(name, bdi_text(e->title != NULL ? e->title : "Untitled"));
	ADD(name, axis_web_storage_text("episode-reason", reason));
	/* A link a person made: said, with how. */
	if (e->hand_how != NULL && e->n_files > 0)
		ADD(name, axis_web_storage_text("range-detail episode-hand", g_strcmp0(e->hand_how, "forced") == 0 ?
			"Its file came from a release grabbed by force: a person said what it is." :
			g_strcmp0(e->hand_how, "assigned") == 0 ? "Its file was assigned by hand." :
			g_strcmp0(e->hand_how, "brought") == 0 ? "Its file was migrated from Sonarr, on the episode Sonarr had "
			"it on: Axis upgrades it by the series' quality profile, as any file." : "Its file was linked by hand."));
	/* Where what is wrong is put right, next to what it says. */
	if (kind == AXIS_ACQUIRE_ATTENTION_IMPORT && e->import_id > 0) {
		g_autofree gchar *import = g_strdup_printf("/imports/%" G_GINT64_FORMAT, e->import_id);

		ADD(name, axis_web_browser_link(import, "See the import and what resolves it"));
	} else if (kind == AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT) {
		ADD(name, axis_web_browser_link("/downloads", "See the download on the Downloads page"));
	} else if (kind != AXIS_ACQUIRE_ATTENTION_NONE && e->search_id > 0) {
		ADD(name, axis_web_browser_link(releases, "See what was found and what you can do"));
	} else if (kind == AXIS_ACQUIRE_ATTENTION_NO_ROOT || kind == AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE ||
	           kind == AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE) {
		ADD(name, axis_web_browser_link("#settings", "Settings for this series"));
	}
	/* What is happening to it now, or what its last search found lately. */
	ADD(name, axis_tv_web_episode_line(work->series, e, work->work));
	if (work->notice != NULL)
		axis_web_notice_put(work->notice, HTMX_NODE(name), anchor);
	ADD(row, name);
	ADD(row, cell_text(e->air_date != NULL ? e->air_date : "Not known"));
	ADD(row, cell_node(axis_tv_web_mark(state)));
	{
		g_autofree gchar *quality = quality_text(e);

		ADD(row, cell_text(quality));
	}
	htmx_element_add_class(HTMX_ELEMENT(controls), "film-actions");
	if (e->search_id > 0 || e->grab_id > 0) {
		g_autofree gchar *found = g_strdup_printf("Releases found for %s", label);

		ADD(controls, axis_web_browser_link(releases, found));
	}
	if (admin && !e->gone) {
		g_autofree gchar *monitor = g_strdup_printf("/series/%" G_GINT64_FORMAT "/monitor", e->series_id);
		g_autofree gchar *item = axis_tv_int_text(e->id);
		g_autofree gchar *season = axis_tv_int_text(e->season);
		g_autofree gchar *stop = g_strdup_printf("Stop monitoring %s", label);
		g_autofree gchar *start = g_strdup_printf("Monitor %s", label);

		ADD(controls, episode_search_act(request, work, e));
		/* What the indexers have for it, whatever the rules say now. */
		ADD(controls, axis_tv_web_manual_act(request, e->series_id, AXIS_TV_SCOPE_EPISODE, e->id, e->season,
			work->work, "Manual search"));
		/* (Its season with it: the page comes back with that season open.) */
		if (e->monitored)
			ADD(controls, button_form(request, monitor, stop, "button-small", "episode", item, "season", season,
				NULL));
		else
			ADD(controls, button_form(request, monitor, start, "button-small", "episode", item, "season", season,
				"monitored", "yes", NULL));
		/* A file a person made the link of, or keeps: kept by hand or let
		 * go, here (add-force-grab-and-assign). */
		if (e->hand_files != NULL && e->n_files > 0) {
			g_autofree gchar *keep = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links/keep", e->series_id);
			g_autofree gchar *let_go = g_strdup_printf("Let Axis upgrade %s again", label);
			g_autofree gchar *hold = g_strdup_printf("Keep %s by hand", label);
			g_auto(GStrv) files = g_strsplit(e->hand_files, ",", -1);
			guint k;

			/* Each file has a control of its own: the one that is kept is
			 * let go, the one that is not is kept. */
			for (k = 0; files[k] != NULL; k++) {
				gchar *colon = strchr(files[k], ':');
				gboolean kept = colon != NULL && g_strcmp0(colon + 1, "1") == 0;

				if (colon != NULL)
					*colon = '\0';
				ADD(controls, button_form(request, keep, kept ? let_go : hold, "button-small", "file", files[k],
					"keep", kept ? "no" : "yes", "season", season, NULL));
			}
		}
	}
	ADD(row, cell_node(HTMX_NODE(controls)));
	return HTMX_NODE(row);
}

static HtmxNode *
season_section(
	HtmxRequest      *request,
	const SeriesWork *work,
	gint              season,
	gboolean          open,
	gboolean          admin
){
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxDiv *body = htmx_div_new();
	HtmxDiv *actions = htmx_div_new();
	g_autofree gchar *id = g_strdup_printf("season-%d", season);
	g_autofree gchar *name = season == 0 ? g_strdup("Specials") : g_strdup_printf("Season %d", season);
	g_autofree gchar *meta = NULL;
	g_autofree gchar *season_text = axis_tv_int_text(season);
	HtmxNode *table = axis_web_table_new("series-episodes", work->series->anime ? episode_columns_anime :
		episode_columns);
	HtmxTbody *rows = htmx_tbody_new();
	gboolean monitored = FALSE;
	gboolean any_search = FALSE;
	guint total = 0;
	guint have = 0;
	guint wanted = 0;
	guint undated = 0;
	guint i;

	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
		AxisTvState state;

		if (e->season != season || e->gone)
			continue;
		total++;
		monitored = e->season_monitored;
		state = axis_tv_episode_derive(e, work->series, &work->rules, NULL);
		if (e->n_files > 0)
			have++;
		if (state == AXIS_TV_WANTED || state == AXIS_TV_ATTENTION)
			wanted++;
		if (state == AXIS_TV_NOT_AIRED && e->air_date == NULL && !axis_tv_episode_in_flight(e))
			undated++;
		ADD(rows, episode_row(request, work, e, admin));
	}
	meta = wanted > 0 ? g_strdup_printf("%u of %u %s in the library, %u wanted", have, total,
			total == 1 ? "episode" : "episodes", wanted) :
		g_strdup_printf("%u of %u %s in the library", have, total, total == 1 ? "episode" : "episodes");
	htmx_element_set_id(HTMX_ELEMENT(details), id);
	htmx_element_add_class(HTMX_ELEMENT(details), "season");
	htmx_details_set_open(details, open);
	ADD(summary, axis_web_storage_text("season-name", name));
	ADD(summary, axis_web_storage_text("season-monitored", monitored ? "Monitored" : "Not monitored"));
	ADD(summary, axis_web_storage_text("season-meta", meta));
	ADD(details, summary);
	htmx_element_add_class(HTMX_ELEMENT(body), "season-body");
	htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
	htmx_element_add_class(HTMX_ELEMENT(actions), "season-actions");
	/* What a person asked for of it, or what was found lately; and what
	 * was just done, here. */
	if (work->notice != NULL)
		axis_web_notice_put(work->notice, HTMX_NODE(body), id);
	ADD(body, axis_tv_web_season_line(work->series, season, work->work));
	if (admin) {
		g_autofree gchar *monitor = g_strdup_printf("/series/%" G_GINT64_FORMAT "/monitor", work->series->id);
		g_autofree gchar *check = g_strdup_printf("season-%d-monitored", season);
		HtmxNode *form = axis_web_auth_post_form(request, monitor);

		htmx_element_add_class(HTMX_ELEMENT(form), "season-monitor");
		ADD(form, axis_web_auth_hidden("season", season_text));
		ADD(form, check_field(check, "Monitored", "monitored", monitored));
		ADD(form, axis_web_auth_button("Save monitoring of this season", "button-small"));
		ADD(actions, form);
		ADD(actions, season_search_act(request, work, season, FALSE));
		ADD(actions, axis_tv_web_manual_act(request, work->series->id, AXIS_TV_SCOPE_SEASON, season, season,
			work->work, "Manual search of the season"));
		ADD(actions, season_search_act(request, work, season, TRUE));
	}
	for (i = 0; i < work->episodes->len && !any_search; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

		any_search = e->season == season;
	}
	{
		g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?season=%d", work->series->id,
			season);
		g_autofree gchar *found = season == 0 ? g_strdup("Releases found for the specials") :
			g_strdup_printf("Releases found for season %d", season);

		if (any_search)
			ADD(actions, axis_web_browser_link(href, found));
	}
	ADD(body, actions);
	if (undated > 0) {
		g_autofree gchar *text = undated == 1 ?
			g_strdup_printf("One monitored episode of this season has no air date in the provider's data. Axis searches "
				"for an episode by itself only once its air date has passed, so this one waits until the provider gives "
				"a date, or until %s.", admin ? "you search for it with Search anyway" :
				"an administrator searches for it by hand") :
			g_strdup_printf("%u monitored episodes of this season have no air date in the provider's data. Axis "
				"searches for an episode by itself only once its air date has passed, so these wait until the provider "
				"gives dates, or until %s.", undated, admin ? "you search for them with Search anyway, one at a time "
				"or all at once" : "an administrator searches for them by hand");

		ADD(body, axis_web_storage_text("help season-undated", text));
	}
	ADD(table, rows);
	ADD(body, table);
	ADD(details, body);
	return HTMX_NODE(details);
}

/* "12 of 13 seasons monitored; the specials are not monitored; new seasons
 * are monitored when they are announced." */
static gchar *
monitoring_words(const AxisTvSeries *s)
{
	GString *words = g_string_new(NULL);

	if (s->n_seasons == 0)
		g_string_append(words, "No season is known yet");
	else
		g_string_append_printf(words, "%u of %u %s monitored", s->n_seasons_monitored, s->n_seasons,
			s->n_seasons == 1 ? "season" : "seasons");
	if (s->has_specials)
		g_string_append(words, s->specials_monitored ? "; the specials are monitored" :
			"; the specials are not monitored");
	g_string_append(words, s->monitor_new ? "; new seasons are monitored when they are announced." :
		"; new seasons are not monitored until you choose them.");
	return g_string_free(words, FALSE);
}

/*
 * The monitoring of the whole series, at the top of its page: how it stands,
 * in words, and for an administrator one form that changes it without
 * opening each season (POST /series/:id/monitor with "whole").  The seasons'
 * own ticks below stay.
 */
static HtmxNode *
monitoring_card(
	HtmxRequest      *request,
	const SeriesWork *work,
	gboolean          admin
){
	static const gchar *const choices[][2] = {
		{ "keep", "Leave the seasons as they are" },
		{ "all", "Monitor every season" },
		{ "latest", "Monitor the latest season only" },
		{ "none", "Stop monitoring every season" }
	};
	const AxisTvSeries *s = work->series;
	HtmxNode *card = axis_web_storage_card("Monitoring");
	g_autofree gchar *words = monitoring_words(s);
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "monitoring");
	if (work->notice != NULL)
		axis_web_notice_put(work->notice, card, "monitoring");
	ADD(card, axis_web_storage_text("series-monitoring-now", words));
	if (s->monitor_choice == AXIS_TV_MONITOR_BROUGHT)
		ADD(card, axis_web_storage_text("help series-monitoring-brought", admin ?
			"Monitored as in Sonarr: each season and episode as it was there when the series was migrated. A choice for "
			"the seasons below replaces that." :
			"Monitored as in Sonarr: each season and episode as it was there when the series was migrated."));
	if (admin) {
		g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/monitor", s->id);
		HtmxNode *form = axis_web_auth_post_form(request, action);
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("series-monitoring-seasons");
		HtmxSelect *select = htmx_select_new_with_name("whole");

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		htmx_element_add_class(HTMX_ELEMENT(form), "series-monitoring");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Seasons");
		htmx_element_set_id(HTMX_ELEMENT(select), "series-monitoring-seasons");
		for (i = 0; i < G_N_ELEMENTS(choices); i++) {
			HtmxOption *option = htmx_option_new_with_value(choices[i][0], choices[i][1]);

			htmx_option_set_selected(option, i == 0);
			ADD(select, option);
		}
		ADD(field, label);
		ADD(field, select);
		ADD(form, field);
		ADD(form, check_field("series-monitoring-specials", "With every season, monitor the specials too", "specials",
			FALSE));
		ADD(form, check_field("series-monitoring-new", "Monitor new seasons and episodes as they are announced", "new",
			s->monitor_new));
		ADD(form, axis_web_auth_button("Save monitoring of the whole series", "button-small"));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help series-monitoring-help", "With the box ticked, a season the provider "
			"announces later is monitored from the day it is listed, so its episodes are searched for as they air; "
			"without it, a new season waits until you monitor it. A new episode of a season Axis knows is monitored as "
			"its season is, either way. \"Monitor every season\" leaves the specials as they are unless you tick their "
			"box. \"Monitor the latest season only\" stops monitoring every other season and the specials. \"Stop "
			"monitoring every season\" stops the specials too."));
		ADD(card, axis_web_storage_text("help series-monitoring-files", "A change here changes only what Axis searches "
			"for. No file is deleted, moved or unlinked: an episode that has a file keeps it, monitored or not. "
			"Nothing is searched for at once: the next automatic search takes the episodes that are wanted now."));
	}
	return card;
}

static HtmxNode *
facts_card(
	HtmxRequest      *request,
	const SeriesWork *work,
	gboolean          admin
){
	const AxisTvSeries *s = work->series;
	HtmxNode *card = axis_web_storage_card("Settings for this series");
	HtmxDl *list = htmx_dl_new();
	g_autofree gchar *synced = axis_web_format_time(s->synced_us, "not yet");
	g_autofree gchar *destination = s->root_name != NULL ? g_strdup_printf("%s on %s", s->root_name,
		s->volume_name != NULL ? s->volume_name : "a disk") : g_strdup("None: an administrator can choose one");
	gboolean editable = admin && work->choices_read;
	g_autoptr(GPtrArray) claims = axis_tv_claim_texts(s);
	const gchar *pairs[14];
	guint first;
	guint i;

	/* The card a page's "Change the quality profile" leads to, and where its
	 * actions come back to with their notice. */
	htmx_element_set_id(HTMX_ELEMENT(card), "settings");
	if (work->notice != NULL)
		axis_web_notice_put(work->notice, card, "settings");
	/* The quality profile and the destination are said once: as the fields
	 * an administrator changes, else as values. */
	if (editable) {
		g_autofree gchar *edit = g_strdup_printf("/series/%" G_GINT64_FORMAT "/edit", s->id);
		HtmxNode *form = axis_web_auth_post_form(request, edit);
		HtmxDiv *field = htmx_div_new();
		HtmxDiv *root_field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("series-profile");
		HtmxLabel *root_label = htmx_label_new_for("series-root");
		HtmxSelect *profile = htmx_select_new_with_name("profile");
		HtmxSelect *root = htmx_select_new_with_name("root");

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		htmx_element_add_class(HTMX_ELEMENT(form), "series-options");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_element_add_class(HTMX_ELEMENT(root_field), "field");
		htmx_element_add_class(HTMX_ELEMENT(root_field), "field-wide");
		htmx_node_set_text_content(HTMX_NODE(label), "Quality profile");
		htmx_node_set_text_content(HTMX_NODE(root_label), "Destination");
		htmx_element_set_id(HTMX_ELEMENT(profile), "series-profile");
		htmx_element_set_id(HTMX_ELEMENT(root), "series-root");
		for (i = 0; i < work->choices.profiles->len; i++) {
			const AxisTvChoice *c = g_ptr_array_index(work->choices.profiles, i);
			g_autofree gchar *value = axis_tv_int_text(c->id);
			HtmxOption *option;

			if (!c->usable && c->id != s->profile_id)
				continue;
			option = htmx_option_new_with_value(value, c->label);
			htmx_option_set_selected(option, c->id == s->profile_id);
			ADD(profile, option);
		}
		for (i = 0; i < work->choices.roots->len; i++) {
			const AxisTvChoice *c = g_ptr_array_index(work->choices.roots, i);
			g_autofree gchar *value = axis_tv_int_text(c->id);
			HtmxOption *option;

			if (!c->usable && c->id != s->root_id)
				continue;
			option = htmx_option_new_with_value(value, c->label);
			htmx_option_set_selected(option, c->id == s->root_id);
			ADD(root, option);
		}
		ADD(field, label);
		ADD(field, profile);
		ADD(root_field, root_label);
		ADD(root_field, root);
		ADD(form, field);
		ADD(form, root_field);
		ADD(form, axis_web_auth_button("Save quality profile and destination", "button-small"));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help", "A new quality profile searches again at once for every wanted "
			"episode, with the new profile's rules. A new destination alone searches for nothing."));
	}
	/* Its kind (add-anime-support, A1): the box of the Add form, as the
	 * series stands.  Only a person ticks or clears it; saving it is one
	 * write and one history line, and moves nothing. */
	if (admin) {
		g_autofree gchar *kind = g_strdup_printf("/series/%" G_GINT64_FORMAT "/kind", s->id);
		HtmxNode *form = axis_web_auth_post_form(request, kind);

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		htmx_element_add_class(HTMX_ELEMENT(form), "series-kind");
		ADD(form, check_field("series-anime", AXIS_TV_WEB_ANIME_LABEL, "anime", s->anime));
		ADD(form, axis_web_auth_button("Save the kind of series", "button-small"));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help series-kind-help", s->anime ?
			"Marked as anime: the card \"Anime numbering\" above says what its absolute numbers are. Clearing the box "
			"changes no file, no link and no numbering." :
			"Ticking the box changes no file, no link and no numbering: the series' page then shows the absolute number "
			"TheTVDB gives each episode. " AXIS_TV_WEB_ANIME_SAYS));
	}
	pairs[0] = "Quality profile";
	pairs[1] = s->profile_name;
	pairs[2] = "Destination";
	pairs[3] = destination;
	pairs[4] = "What to monitor";
	pairs[5] = axis_tv_monitor_label(s->monitor_choice);
	pairs[6] = "New seasons";
	pairs[7] = s->monitor_new ? "Monitored when announced" : "Not monitored until chosen";
	pairs[8] = "Details last updated";
	pairs[9] = synced;
	pairs[10] = "Next episode";
	pairs[11] = work->summary->next_air_date != NULL ? work->summary->next_air_date : "None announced";
	/* Whose seasons and episodes it follows (add-metadata-tvdb). */
	pairs[12] = "Episode numbering";
	pairs[13] = axis_tv_source_label(s->episode_source);
	htmx_element_add_class(HTMX_ELEMENT(list), "facts");
	for (first = editable ? 4 : 0, i = first; i < G_N_ELEMENTS(pairs); i += 2) {
		HtmxDt *term = htmx_dt_new();
		HtmxDd *value = htmx_dd_new();

		htmx_node_set_text_content(HTMX_NODE(term), pairs[i]);
		htmx_node_set_text_content(HTMX_NODE(value), pairs[i + 1] != NULL ? pairs[i + 1] : "");
		ADD(list, term);
		ADD(list, value);
	}
	ADD(card, list);
	if (s->n_mismatched > 0) {
		g_autofree gchar *text = g_strdup_printf("%u %s found by its searches %s numbered for episodes that %s "
			"does not have: release names often follow another numbering. Such a release is "
			"never grabbed or imported for a guessed episode; see the reasons on the releases.", s->n_mismatched,
			s->n_mismatched == 1 ? "release" : "releases", s->n_mismatched == 1 ? "was" : "were",
			axis_tv_source_list_name(s->episode_source));

		ADD(card, axis_web_storage_text("notice notice-warning", text));
	}
	/* Releases ruled out for stating only an absolute number, on a series
	 * that is not marked as anime: the one thing that would change it. */
	if (!s->anime && s->n_absolute_only > 0) {
		g_autofree gchar *text = g_strdup_printf("%u %s found by its searches %s only an absolute episode number, as "
			"anime releases often do, and %s ruled out. If this series is anime, mark it so below.", s->n_absolute_only,
			s->n_absolute_only == 1 ? "release" : "releases", s->n_absolute_only == 1 ? "states" : "state",
			s->n_absolute_only == 1 ? "was" : "were");

		ADD(card, axis_web_storage_text("notice anime-suggest", text));
	}
	if (s->n_by_place > 0) {
		g_autofree gchar *text = g_strdup_printf("TMDb gives no identifier for %u of its episodes: those are "
			"known by their season and number alone, so if it numbers them anew, Axis takes them for other "
			"episodes.", s->n_by_place);

		ADD(card, axis_web_storage_text("help", text));
	}
	if (s->other_live > 0)
		ADD(card, axis_web_storage_text("help", "A download of it that Axis gave up on is still at its download client: nothing else is grabbed for it until it is gone."));
	/* Identifiers two providers disagree about: shown, never settled by a
	 * guess. */
	for (i = 0; i < claims->len; i++)
		ADD(card, axis_web_storage_text("notice notice-warning", g_ptr_array_index(claims, i)));
	/* Its numbering: why it is not refreshed now, and the page that changes
	 * it, which shows what would change before anything does. */
	if (admin && work->not_refreshed != NULL)
		ADD(card, axis_web_storage_text("notice notice-warning", work->not_refreshed));
	if (admin && work->numbering_to != NULL) {
		g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=%s", s->id,
			work->numbering_to);
		g_autofree gchar *words = g_strdup_printf("Its episodes can be numbered as %s numbers them instead. The next "
			"page shows how each episode would map, and changes nothing until you confirm. ",
			axis_tv_source_label(work->numbering_to));
		HtmxP *line = htmx_p_new();
		HtmxSpan *text = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(text), words);
		ADD(line, text);
		ADD(line, axis_web_browser_link(href, "Change the episode numbering"));
		ADD(card, line);
	}
	/* Brought over and held (import-from-arr): the series, or some of its
	 * episodes whose files Axis does not see.  Said where its searches are
	 * started, with the ways on. */
	{
		guint unseen = 0;

		for (i = 0; work->episodes != NULL && i < work->episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

			if (!e->gone && axis_tv_hold_stops(e->search_hold))
				unseen++;
		}
		if (axis_tv_hold_stops(s->search_hold)) {
			/* Said before the press: which buttons start searching for the
			 * whole series, and which are one search and no more. */
			g_autofree gchar *text = g_strdup_printf("%s \"Search for every wanted episode\" below starts searching for "
				"the whole series, as \"Let Axis search for it\" does: Axis then searches for it by itself. A search of "
				"one episode or one season, and a manual search, are that search only: the series stays held.",
				axis_tv_hold_sentence(s->search_hold));

			ADD(card, axis_web_storage_text("help series-held", text));
		}
		if (unseen > 0) {
			g_autofree gchar *text = g_strdup_printf("%u %s held apart: Sonarr has a file for %s that Axis does not see "
				"(not in Axis's index at its path, another size, linked elsewhere, or on an episode Axis numbers "
				"otherwise). Axis does not search for %s, so that no second copy is fetched, until the episode has a file "
				"in Axis (a later migration, Identify, or a link by hand), or you search for that episode "
				"yourself. Starting to search for the series leaves %s held.", unseen,
				unseen == 1 ? "episode is" : "episodes are", unseen == 1 ? "it" : "each", unseen == 1 ? "it" : "them",
				unseen == 1 ? "it" : "them");

			ADD(card, axis_web_storage_text("help series-held-episodes", text));
		}
		if (admin && (unseen > 0 || axis_tv_hold_stops(s->search_hold))) {
			/* A page that asks first, and changes nothing by being opened. */
			g_autofree gchar *start = g_strdup_printf("/series/%" G_GINT64_FORMAT "/start-searching", s->id);
			HtmxP *held = htmx_p_new();

			htmx_element_add_class(HTMX_ELEMENT(held), "help");
			htmx_element_add_class(HTMX_ELEMENT(held), "series-held-start");
			ADD(held, axis_web_browser_link(start, "Let Axis search for it"));
			ADD(card, held);
		}
	}
	if (admin) {
		HtmxDiv *row = htmx_div_new();
		g_autofree gchar *sync = g_strdup_printf("/series/%" G_GINT64_FORMAT "/sync", s->id);
		g_autofree gchar *remove = g_strdup_printf("/series/%" G_GINT64_FORMAT "/remove", s->id);

		/* The card's actions in one row, the one that removes last and set
		 * apart. */
		htmx_element_add_class(HTMX_ELEMENT(row), "film-actions");
		htmx_element_add_class(HTMX_ELEMENT(row), "settings-actions");
		/* While every wanted episode is in a search, the button says so. */
		ADD(row, series_search_act(request, work));
		/* A person's look at what the indexers have for all of it. */
		ADD(row, axis_tv_web_manual_act(request, s->id, AXIS_TV_SCOPE_SERIES, 0, -1, work->work,
			"Manual search of the whole series"));
		ADD(row, button_form(request, sync, "Update details now", "button-small", NULL));
		{
			HtmxNode *remove_form = confirm_form(request, remove, "Remove this series", "series-remove-confirm",
				"Forget this series, its searches and its blocklist. No file is touched: while its episodes are on "
				"disk and recognised by Identify, it stays in TV as a series on disk that Axis does not manage",
				"Yes, remove this series", NULL, NULL);

			htmx_element_add_class(HTMX_ELEMENT(remove_form), "film-confirm-danger");
			htmx_element_add_class(HTMX_ELEMENT(remove_form), "actions-apart");
			ADD(row, remove_form);
		}
		ADD(card, row);
	}
	return card;
}

/*
 * "Anime numbering", for a series marked as anime (add-anime-support, A1):
 * that Axis numbers it by season, the state of the absolute numbers TheTVDB
 * gives its episodes (computed now, stored nowhere), what Axis does not do
 * with them yet, and, where it was migrated from Sonarr, what comparing
 * Sonarr's absolute numbers with TheTVDB's found.  %NULL for any other
 * series.
 */
static HtmxNode *
anime_card(
	const SeriesWork *work,
	gboolean          admin
){
	const AxisTvSeries *s = work->series;
	g_autofree gchar *state = NULL;
	AxisTvAbsolute absolute;
	HtmxNode *card;

	if (!s->anime)
		return NULL;
	card = axis_web_storage_card("Anime numbering");
	htmx_element_set_id(HTMX_ELEMENT(card), "anime-numbering");
	ADD(card, axis_web_storage_text(NULL, "This series is marked as anime. Axis numbers it by season, as media servers "
		"do (S06E12)."));
	axis_tv_absolute_state(s, work->episodes, &absolute);
	state = axis_tv_absolute_sentence(s, &absolute, work->tvdb_key);
	if (absolute.state == AXIS_TV_ABSOLUTE_NOT_USABLE) {
		ADD(card, axis_web_storage_text("notice notice-warning anime-state", state));
	} else if (absolute.state == AXIS_TV_ABSOLUTE_NO_SOURCE && admin && g_strcmp0(work->numbering_to, "tvdb") == 0) {
		/* With the page that changes its numbering, which shows how each
		 * episode would map and changes nothing until it is confirmed. */
		g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", s->id);
		g_autofree gchar *words = g_strconcat(state, " ", NULL);
		HtmxP *line = htmx_p_new();
		HtmxSpan *text = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "anime-state");
		htmx_node_set_text_content(HTMX_NODE(text), words);
		ADD(line, text);
		ADD(line, axis_web_browser_link(href, "Change the episode numbering"));
		ADD(card, line);
	} else {
		ADD(card, axis_web_storage_text("anime-state", state));
	}
	/* What is matched (slice A2), and what a search asks for (slice A3). */
	if (absolute.state == AXIS_TV_ABSOLUTE_USABLE) {
		ADD(card, axis_web_storage_text("anime-matched", "A release or a file named by an absolute number (\"Title - "
			"137\") is matched to the one aired episode TheTVDB gives that number: on a manual search, where you can grab "
			"it; when a search of its own returns such a release; when a download is imported; and on \"Place by its "
			"name\". A name that states a season and an episode (\"Title S06E12\") is matched by them, as for any series. "
			"The history says each time a number was used, with both numbers."));
		ADD(card, axis_web_storage_text("help anime-refused", "Not read, each with its reason on the manual search: a "
			"season with a bare number (\"Title S2 - 05\"), an episode its season does not have (\"Title S01E137\"), a "
			"number no aired episode has, and a special named by a word such as OVA. Grab one by force from a manual "
			"search when you know what it holds."));
		ADD(card, axis_web_storage_text("help anime-bare", "A release named with the number right after the title and "
			"nothing that marks it as an episode (\"Title 137\") is listed on a manual search with what it holds, and no "
			"search grabs it: another title's name can look the same (a sequel \"Title 2\", a film \"Title 13\"). Grab "
			"this one, on the manual search, takes it. A number under the name of a season or a sequel (\"Title 2nd "
			"Season - 05\") is not read."));
		ADD(card, axis_web_storage_text("help anime-searched", "Searched for by absolute number too. For an episode that "
			"has one, Axis asks each indexer by season and episode, as for any series, and by the series' title with the "
			"number (\"Title 137\"); and once more with the romanised name, when the series' details give one. Every "
			"request is made in the anime category as well as in TV. A search's record names the requests it made. What "
			"comes back is matched by the rules above: asking in more ways takes nothing that would not be taken "
			"otherwise."));
		ADD(card, axis_web_storage_text("help anime-batches", "A season search also asks by the series' name alone, "
			"which is how a batch is found. A batch (\"Title - 01-12\") is taken as exactly the episodes its numbers map "
			"to, in one season or several, and only when every monitored episode it holds is wanted, none is being "
			"downloaded and none has a file kept by hand. Each of its files is then placed only on one of those "
			"episodes, by the number in the file's own name; a file named any other way is left for you to assign."));
	} else {
		ADD(card, axis_web_storage_text("help anime-searched", "While this series' absolute numbers cannot be used, a "
			"release named \"Title - 137\" is ruled out, and says so on the manual search; a release named \"Title "
			"S06E12\" is matched as for any series. Axis searches for it by season and episode, in the anime category "
			"as well as in TV, and not by absolute number."));
	}
	/* The record of the mixed forms, counted over its kept searches. */
	if (s->n_season_absolute > 0) {
		g_autofree gchar *text = g_strdup_printf("%u %s found by its searches %s a season with a bare number (such as "
			"\"S2 - 05\") and %s not read: a group's season is often not TheTVDB's. Each is listed with its reason on "
			"the search that found it.", s->n_season_absolute, s->n_season_absolute == 1 ? "release" : "releases",
			s->n_season_absolute == 1 ? "states" : "state", s->n_season_absolute == 1 ? "was" : "were");

		ADD(card, axis_web_storage_text("notice anime-mixed", text));
	}
	/* Migrated from Sonarr as anime: Sonarr's own numbers beside TheTVDB's,
	 * as the run found them.  A report: nothing was migrated differently. */
	if (s->brought_absolute != NULL) {
		AxisTvAbsoluteCompare compare;

		if (axis_tv_absolute_compare_parse(s->brought_absolute, &compare)) {
			g_autofree gchar *sentence = axis_tv_absolute_compare_sentence(&compare);
			g_autofree gchar *list = axis_tv_absolute_compare_list(&compare);
			g_autofree gchar *found = g_strdup_printf("When it was migrated from Sonarr: %s", sentence);

			ADD(card, axis_web_storage_text(compare.differ > 0 ? "notice notice-warning anime-compared" :
				"help anime-compared", found));
			if (list != NULL) {
				g_autofree gchar *listed = g_strdup_printf("Where they differ: %s.", list);

				ADD(card, axis_web_storage_text("help anime-differs", listed));
			}
		}
		axis_tv_absolute_compare_clear(&compare);
	}
	return card;
}

static const AxisWebColumn lacked_columns[] = {
	{ "Episode", AXIS_WEB_COL_TEXT },
	{ "Title", AXIS_WEB_COL_NAME },
	{ "File", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* The episodes a change of numbering kept without a counterpart, with their
 * files (add-metadata-tvdb): their numbers are the other source's. */
static HtmxNode *
lacked_card(
	const SeriesWork *work,
	gboolean          admin
){
	g_autofree gchar *heading_text = g_strdup_printf("Episodes %s lacks",
		axis_tv_source_label(work->series->episode_source));
	HtmxNode *card = NULL;
	HtmxNode *table = NULL;
	HtmxTbody *body = NULL;
	guint i;

	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
		g_autofree gchar *label = NULL;
		g_autofree gchar *file = NULL;
		HtmxTr *row;

		if (!e->gone || e->source == NULL || g_strcmp0(e->source, work->series->episode_source) == 0)
			continue;
		if (card == NULL) {
			g_autofree gchar *help = g_strdup_printf("These episodes have no counterpart in %s. Each is kept with its "
				"files and history, numbered as %s numbers it, and never searched for; its files stay on disk where "
				"they are.", axis_tv_source_label(work->series->episode_source), axis_tv_source_label(e->source));

			card = axis_web_storage_card(heading_text);
			htmx_element_set_id(HTMX_ELEMENT(card), "lacked");
			ADD(card, axis_web_storage_text("help", help));
			table = axis_web_table_new("series-lacked", lacked_columns);
			body = htmx_tbody_new();
		}
		label = axis_tv_episode_label(e->season, e->number);
		file = e->n_files == 0 ? g_strdup("No file") : e->n_files == 1 ? g_strdup(e->file_path) :
			g_strdup_printf("%s and %u more", e->file_path, e->n_files - 1);
		row = htmx_tr_new();
		{
			g_autofree gchar *numbered = g_strdup_printf("%s (%s)", label, axis_tv_source_label(e->source));

			ADD(row, cell_text(numbered));
		}
		ADD(row, cell_node(bdi_text(e->title != NULL ? e->title : "Untitled")));
		ADD(row, cell_node(axis_web_breakable(file)));
		ADD(body, row);
	}
	if (card == NULL)
		return NULL;
	ADD(table, body);
	ADD(card, table);
	if (admin) {
		g_autofree gchar *links = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links#linked", work->series->id);

		ADD(card, axis_web_browser_link(links, "Move or unlink these files' links"));
	}
	return card;
}

static const AxisWebColumn misplaced_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "On", AXIS_WEB_COL_TEXT },
	{ "Its name says", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

/* The files whose names, read under the numbering the series follows, say
 * another episode than the one they are on; an administrator's, with the
 * way to place each by its name (its own confirmation first). */
static HtmxNode *
misplaced_card(
	const SeriesWork *work,
	gboolean          earlier
){
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint placeable = 0;
	guint shown = 0;
	guint i;

	if (work->misplaced == NULL)
		return NULL;
	for (i = 0; i < work->misplaced->len; i++)
		shown += ((const AxisTvMisplaced *)g_ptr_array_index(work->misplaced, i))->earlier == earlier;
	if (shown == 0)
		return NULL;
	if (earlier) {
		card = axis_web_storage_card("Named by Axis under the earlier numbering");
		htmx_element_set_id(HTMX_ELEMENT(card), "misplaced-earlier");
		ADD(card, axis_web_storage_text("help", "An import by Axis linked each of these files, or its name was read "
			"under another numbering than the one the series follows. Axis names a file by the number its episode had "
			"then, so the name is probably just the old number and the episode the file is on is right. They are never "
			"placed all at once. Placing one by its name asks you to confirm it first."));
	} else {
		card = axis_web_storage_card("Files whose names say another episode");
		htmx_element_set_id(HTMX_ELEMENT(card), "misplaced");
		ADD(card, axis_web_storage_text("help", "Each of these files was found by identification under the numbering "
			"the series follows, and its name says another episode than the one it is on. Placing one by its name moves "
			"only its link, never the file on disk, and shows what changes before it does."));
	}
	table = axis_web_table_new("series-misplaced", misplaced_columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->misplaced->len; i++) {
		const AxisTvMisplaced *m = g_ptr_array_index(work->misplaced, i);
		g_autofree gchar *on = NULL;
		g_autofree gchar *says = NULL;
		HtmxTr *row;
		HtmxDiv *controls;

		if (m->earlier != earlier)
			continue;
		on = axis_tv_episode_label(m->season, m->number);
		says = axis_tv_episode_label(m->name_season, m->name_number);
		row = htmx_tr_new();
		controls = htmx_div_new();
		ADD(row, cell_node(axis_web_breakable(m->path)));
		ADD(row, cell_text(on));
		ADD(row, cell_text(says));
		htmx_element_add_class(HTMX_ELEMENT(controls), "film-actions");
		/* The row says what placing this one file alone does; what the files
		 * make of each other together is Place all's to say. */
		if (m->all_placed)
			placeable++;
		if (m->refused == NULL) {
			g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT,
				work->series->id, m->file_id);

			ADD(controls, axis_web_browser_link(href, "Place by its name"));
		} else {
			ADD(controls, axis_web_storage_text("help", m->refused));
		}
		ADD(row, cell_node(HTMX_NODE(controls)));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	if (!earlier && placeable > 1) {
		g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", work->series->id);

		ADD(card, axis_web_browser_link(href, "Place all by their names"));
	}
	return card;
}

/* The topics a series' page follows: its searches, and its title's
 * downloads. */
static void
series_topics(
	const AxisTvSeries *series,
	gchar             **topics
){
	g_autofree gchar *key = g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":", series->id);

	topics[0] = axis_web_activity_topic(key);
	topics[1] = axis_web_activity_title_topic(series->title_id);
	topics[2] = NULL;
}

/* The notice of what was just done; a click the work in hand took is said
 * with where that work stands, for the row or card it came back to. */
static gchar *
series_notice(
	const SeriesWork *work,
	HtmxRequest      *request
){
	const gchar *place = axis_web_place_of(request);
	AxisTvScope scope = AXIS_TV_SCOPE_SERIES;
	g_autofree gchar *episode = NULL;
	gint64 item = 0;
	gint season = -1;
	gchar *answer;
	guint i;

	/* What was done at another title's card on this page (More like this) is
	 * said there, by the part that draws the card. */
	if (place != NULL && g_str_has_prefix(place, "title-"))
		return NULL;
	/* (A search of one episode that found it not wanted says nothing of the
	 * episodes held apart: they are not why.) */
	if (g_strcmp0(work->done, "already_searching") != 0 && g_strcmp0(work->done, "searched_recently") != 0)
		return axis_tv_web_done_text(work->done, work->series, g_strcmp0(work->done, "nothing_wanted") == 0 &&
			place != NULL && g_str_has_prefix(place, "episode-") ? NULL : work->episodes);
	if (place != NULL && g_str_has_prefix(place, "episode-")) {
		g_ascii_string_to_signed(place + strlen("episode-"), 10, 1, G_MAXINT64, &item, NULL);
		for (i = 0; i < work->episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

			if (e->id != item)
				continue;
			scope = AXIS_TV_SCOPE_EPISODE;
			season = e->season;
			episode = axis_tv_episode_label(e->season, e->number);
		}
	} else if (place != NULL && g_str_has_prefix(place, "season-")) {
		gint64 number = 0;

		if (g_ascii_string_to_signed(place + strlen("season-"), 10, 0, 10000, &number, NULL)) {
			scope = AXIS_TV_SCOPE_SEASON;
			item = number;
			season = (gint)number;
		}
	}
	answer = axis_tv_web_answer(work->done, work->series->id, scope, scope == AXIS_TV_SCOPE_SERIES ? 0 : item, season,
		episode, work->episodes, work->work);
	return answer != NULL ? answer : axis_tv_web_done_text(work->done, work->series, work->episodes);
}

/* The live fragment: every line and search button of the page, out of band,
 * drawn as the page draws them. */
static HtmxResponse *
series_lines_done(
	HtmxRequest *request,
	SeriesWork  *work
){
	gboolean admin = axis_web_request_is_admin(request);
	g_autoptr(HtmxFragment) fragment = htmx_fragment_new();
	g_autoptr(GArray) seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	guint i;
	guint k;

	axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_series_line(work->series, work->episodes, work->work));
	if (admin) {
		axis_web_activity_add_oob(HTMX_NODE(fragment), series_search_act(request, work));
		axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_manual_act(request, work->series->id,
			AXIS_TV_SCOPE_SERIES, 0, -1, work->work, "Manual search of the whole series"));
	}
	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
		gboolean known = FALSE;

		if (e->gone)
			continue;
		axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_episode_line(work->series, e, work->work));
		if (admin) {
			axis_web_activity_add_oob(HTMX_NODE(fragment), episode_search_act(request, work, e));
			axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_manual_act(request, e->series_id,
				AXIS_TV_SCOPE_EPISODE, e->id, e->season, work->work, "Manual search"));
		}
		for (k = 0; k < seasons->len && !known; k++)
			known = g_array_index(seasons, gint, k) == e->season;
		if (!known)
			g_array_append_val(seasons, e->season);
	}
	for (k = 0; k < seasons->len; k++) {
		gint season = g_array_index(seasons, gint, k);

		axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_season_line(work->series, season, work->work));
		if (!admin)
			continue;
		axis_web_activity_add_oob(HTMX_NODE(fragment), season_search_act(request, work, season, FALSE));
		axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_manual_act(request, work->series->id,
			AXIS_TV_SCOPE_SEASON, season, season, work->work, "Manual search of the season"));
		axis_web_activity_add_oob(HTMX_NODE(fragment), season_search_act(request, work, season, TRUE));
	}
	/* The probe again, with when to ask next. */
	{
		g_autofree gchar *url = g_strdup_printf("/series/%" G_GINT64_FORMAT "/activity", work->series->id);
		g_autofree gchar *trigger = axis_tv_web_trigger(work->work, work->episodes);

		axis_web_activity_add_oob(HTMX_NODE(fragment), axis_web_activity_probe(NULL, url, NULL, trigger));
	}
	return axis_web_activity_respond(HTMX_NODE(fragment));
}

/*
 * GET /series/:id/start-searching: what "Let Axis search for it" would do,
 * before it does it.  Opening it writes nothing.  The episodes Sonarr has
 * files for that Axis does not see are counted, and stay held unless the box
 * is ticked (it is not ticked for anyone): one press must not send Axis after
 * a second copy of each.
 */
static HtmxResponse *
series_start_done(
	HtmxRequest      *request,
	const SeriesWork *work
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const AxisTvSeries *s = work->series;
	g_autofree gchar *label = axis_tv_series_label(s->title, s->year);
	g_autofree gchar *head = g_strdup_printf("Let Axis search for %s", label);
	g_autofree gchar *back = g_strdup_printf("/series/%" G_GINT64_FORMAT, s->id);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/start-searching", s->id);
	gboolean held = axis_tv_hold_stops(s->search_hold);
	HtmxNode *card = axis_web_storage_card("What this starts");
	HtmxNode *form;
	HtmxP *away = htmx_p_new();
	guint unseen = 0;
	guint i;

	for (i = 0; work->episodes != NULL && i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

		if (!e->gone && axis_tv_hold_stops(e->search_hold))
			unseen++;
	}
	ADD(content, heading(1, head));
	htmx_element_set_id(HTMX_ELEMENT(card), "start-searching");
	if (!held && unseen == 0) {
		ADD(card, axis_web_storage_text(NULL, "Nothing of this series is held: Axis searches for it by itself already."));
		ADD(away, axis_web_browser_link(back, "Back to the series"));
		ADD(card, away);
		ADD(content, card);
		return axis_web_page_respond(request, head, HTMX_NODE(content));
	}
	ADD(card, axis_web_storage_text(NULL, held ? "This series was migrated from Sonarr and is held: Axis searches "
		"for nothing of it by itself. Confirming ends that. Axis then searches for its wanted episodes at the "
		"scheduler's ordinary pace; nothing is searched for at once." : "Axis searches for this series by itself "
		"already. Only the episodes below are held."));
	if (unseen > 0) {
		g_autofree gchar *text = g_strdup_printf("Sonarr has a file for %u %s that Axis does not see (not in Axis's index "
			"at its path, another size, linked elsewhere, or on an episode Axis numbers otherwise). %s held, so that "
			"no second copy is downloaded, unless you tick the box.", unseen, unseen == 1 ? "episode" : "episodes",
			unseen == 1 ? "It stays" : "They stay");

		ADD(card, axis_web_storage_text("series-start-unseen-count", text));
	} else {
		ADD(card, axis_web_storage_text(NULL, "Sonarr has no file for an episode of it that Axis does not see."));
	}
	ADD(card, axis_web_storage_text("help", "Opening this page changed nothing."));
	form = axis_web_auth_post_form(request, action);
	if (unseen > 0)
		ADD(form, check_field("series-start-unseen", "Search for those too; a second copy may be downloaded", "unseen",
			FALSE));
	ADD(form, axis_web_auth_button("Let Axis search for it", "button-primary"));
	ADD(card, form);
	ADD(away, axis_web_browser_link(back, "Cancel, and go back to the series"));
	ADD(card, away);
	ADD(content, card);
	return axis_web_page_respond(request, head, HTMX_NODE(content));
}

static HtmxResponse *
series_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	SeriesWork *work = data;
	gboolean admin = axis_web_request_is_admin(request);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(GArray) seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autofree gchar *label = NULL;
	g_autofree gchar *title_href = NULL;
	g_autofree gchar *probe = NULL;
	gchar *topics[3];
	AxisWebNotice notice;
	HtmxResponse *response;
	HtmxNode *card;
	HtmxDiv *head;
	HtmxDiv *info;
	AxisMetaTitle title;
	guint gone = 0;
	gint latest = -1;
	guint i;
	guint k;

	if (g_error_matches(work->error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->lines_only)
		return series_lines_done(request, work);
	if (work->start_only)
		return series_start_done(request, work);
	label = axis_tv_series_label(work->series->title, work->series->year);
	title_href = g_strdup_printf("/title/%" G_GINT64_FORMAT, work->series->title_id);
	/* At the row or card the post came back to; else at the top. */
	axis_web_notice_begin(&notice, request, FALSE, series_notice(work, request), HTMX_NODE(content));
	work->notice = &notice;

	/* The head: poster, name, state and how complete. */
	head = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(head), "series-head");
	memset(&title, 0, sizeof(title));
	title.id = work->series->title_id;
	title.kind = AXIS_META_KIND_SERIES;
	title.title = work->series->title;
	title.year = work->series->year;
	title.provider = work->series->provider;
	title.poster_ref = work->series->poster_ref;
	ADD(head, axis_web_meta_poster_new(&title, work->poster, FALSE, 0));
	info = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(info), "series-info");
	ADD(info, heading(1, label));
	{
		HtmxDiv *line = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
		ADD(line, axis_tv_web_mark(work->summary->state));
		if (work->summary->attention->len == 0)
			ADD(line, axis_web_storage_text("film-reason", work->summary->reason));
		ADD(info, line);
	}
	/* What Axis is doing for it now. */
	ADD(info, axis_tv_web_series_line(work->series, work->episodes, work->work));
	/* What needs attention, each reason with where it is put right. */
	if (work->summary->attention->len > 0)
		ADD(info, axis_tv_web_attention(work->summary, admin));
	ADD(info, axis_tv_web_completeness(work->summary));
	{
		HtmxP *line = htmx_p_new();
		HtmxSpan *text = htmx_span_new();
		g_autofree gchar *words = g_strdup_printf("Quality profile: %s. ", work->series->profile_name != NULL ?
			work->series->profile_name : "none");

		htmx_element_add_class(HTMX_ELEMENT(line), "series-profile-line");
		htmx_node_set_text_content(HTMX_NODE(text), words);
		ADD(line, text);
		ADD(line, axis_web_browser_link("#settings", admin ? "Change the quality profile" :
			"See the settings for this series"));
		ADD(info, line);
	}
	ADD(info, axis_web_browser_link(title_href, "Series overview"));
	ADD(head, info);
	ADD(content, head);
	/* What of it is monitored, and the one form for all of it. */
	ADD(content, monitoring_card(request, work, admin));

	/* The seasons, the latest first, the specials last. */
	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
		gboolean known = FALSE;

		if (e->gone) {
			gone++;
			continue;
		}
		for (k = 0; k < seasons->len && !known; k++)
			known = g_array_index(seasons, gint, k) == e->season;
		if (!known)
			g_array_append_val(seasons, e->season);
		latest = MAX(latest, e->season);
	}
	card = axis_web_storage_card("Episodes by season");
	/* Whose numbering the seasons are (add-metadata-tvdb), with TheTVDB's
	 * notice beside them when they are its. */
	{
		g_autofree gchar *whose = g_strdup_printf("Numbered as %s numbers them.",
			axis_tv_source_label(work->series->episode_source));
		const AxisMetaProviderInfo *tvdb = axis_tv_source_is_tvdb(work->series->episode_source) ?
			axis_meta_provider_find(self->meta, "tvdb") : NULL;
		HtmxNode *source = axis_web_storage_text("help seasons-source", whose);

		ADD(card, source);
		if (tvdb != NULL)
			ADD(card, axis_web_meta_attribution(tvdb));
		/* (Why it is not refreshed is the administrators' to put right.) */
		if (admin && work->not_refreshed != NULL)
			ADD(card, axis_web_storage_text("notice notice-warning", work->not_refreshed));
		/* Which file is which episode's, by hand (add-force-grab-and-assign). */
		if (admin) {
			g_autofree gchar *links = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", work->series->id);

			ADD(card, axis_web_browser_link(links, "Files and links"));
		}
	}
	if (seasons->len == 0) {
		g_autofree gchar *text = g_strdup_printf("No episodes are known yet: %s lists none, or its seasons have not "
			"been fetched. \"Update details now\" asks for them.",
			axis_tv_source_is_tvdb(work->series->episode_source) ? "TheTVDB" : "TMDb");

		ADD(card, axis_web_storage_text("help", text));
	}
	for (k = seasons->len; k > 0; k--) {
		gint season = g_array_index(seasons, gint, k - 1);

		if (season == 0)
			continue;
		ADD(card, season_section(request, work, season, work->open_season >= 0 ? season == work->open_season :
			season == latest, admin));
	}
	for (k = 0; k < seasons->len; k++) {
		if (g_array_index(seasons, gint, k) == 0)
			ADD(card, season_section(request, work, 0, work->open_season == 0, admin));
	}
	ADD(content, card);
	card = lacked_card(work, admin);
	if (card != NULL)
		ADD(content, card);
	card = admin ? misplaced_card(work, FALSE) : NULL;
	if (card != NULL)
		ADD(content, card);
	card = admin ? misplaced_card(work, TRUE) : NULL;
	if (card != NULL)
		ADD(content, card);
	/* (Those a change of numbering left are in the card above.) */
	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

		if (e->gone && g_strcmp0(e->source, work->series->episode_source) != 0 && e->source != NULL)
			gone--;
	}
	if (gone > 0) {
		g_autofree gchar *text = g_strdup_printf("%u %s no longer in %s: kept with %s files and history, "
			"never searched for.", gone, gone == 1 ? "episode is" : "episodes are",
			axis_tv_source_list_name(work->series->episode_source), gone == 1 ? "its" : "their");

		ADD(content, axis_web_storage_text("help", text));
	}
	card = unplaced_card(work->disk);
	if (card != NULL)
		ADD(content, card);
	card = folders_card(work->disk);
	if (card != NULL)
		ADD(content, card);
	card = anime_card(work, admin);
	if (card != NULL)
		ADD(content, card);
	ADD(content, facts_card(request, work, admin));
	ADD(content, events_card(work->events));
	/* Other series like this one (Discover's card, when it is on), last. */
	card = axis_web_title_more_new(request, AXIS_META_KIND_SERIES, work->series->title_id);
	if (card != NULL)
		ADD(content, card);
	axis_web_notice_end(&notice);
	work->notice = NULL;
	/* Its lines follow what happens, by the hub. */
	series_topics(work->series, topics);
	probe = g_strdup_printf("/series/%" G_GINT64_FORMAT "/activity", work->series->id);
	{
		g_autofree gchar *trigger = axis_tv_web_trigger(work->work, work->episodes);

		ADD(content, axis_web_activity_probe(request, probe, (const gchar *const *)topics, trigger));
	}
	for (i = 0; topics[i] != NULL; i++)
		g_free(topics[i]);
	{
		g_autofree gchar *page_title = g_strdup_printf("%s: episodes and downloads", label);

		response = axis_web_page_respond(request, page_title, HTMX_NODE(content));
	}
	return response;
}

static HtmxResponse *
handle_series(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	SeriesWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *season = htmx_request_get_query_param(request, "season");
	const gchar *done = htmx_request_get_query_param(request, "done");
	guint64 value = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	work = g_new0(SeriesWork, 1);
	work->series_id = id;
	work->open_season = season != NULL && g_ascii_string_to_unsigned(season, 10, 0, 10000, &value, NULL) ?
		(gint)value : -1;
	{
		g_autofree gchar *known = axis_tv_web_done_text(done, NULL, NULL);

		if (known != NULL)
			work->done = g_strdup(done);
	}
	return axis_tv_web_dispatch(user_data, request, series_run, series_done, work, series_work_free);
}

/* GET /series/:id/activity: the page's lines and search buttons, out of
 * band (what its probe asks for when the hub says something changed). */
static HtmxResponse *
handle_series_lines(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	SeriesWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	work = g_new0(SeriesWork, 1);
	work->series_id = id;
	work->open_season = -1;
	work->lines_only = TRUE;
	return axis_tv_web_dispatch(user_data, request, series_run, series_done, work, series_work_free);
}

/* GET /series/:id/start-searching: the confirmation (series_start_done()). */
static HtmxResponse *
handle_series_start(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	SeriesWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	work = g_new0(SeriesWork, 1);
	work->series_id = id;
	work->open_season = -1;
	work->start_only = TRUE;
	return axis_tv_web_dispatch(user_data, request, series_run, series_done, work, series_work_free);
}

/* ── What is on disk of a series ────────────────────────────────── */

static const AxisWebColumn unplaced_columns[] = {
	{ "File", AXIS_WEB_COL_NAME },
	{ "Why it is on no episode", AXIS_WEB_COL_WIDE },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ NULL, 0 }
};

static const AxisWebColumn folder_columns[] = {
	{ "Folder", AXIS_WEB_COL_NAME },
	{ "Disk", AXIS_WEB_COL_TEXT },
	{ "Videos", AXIS_WEB_COL_TEXT },
	{ "Recognised", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxNode *
size_cell(gint64 size)
{
	g_autofree gchar *text = size >= 0 ? axis_size_format((guint64)size) : g_strdup("");

	return cell_text(text);
}

static HtmxNode *
file_place(const AxisTvDiskFile *f)
{
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *where = g_strdup_printf("%s: ", f->root_name != NULL ? f->root_name : "a library folder");

	htmx_element_add_class(HTMX_ELEMENT(box), "identify-line");
	ADD(box, axis_web_storage_text("episode-reason", where));
	ADD(box, axis_web_browser_name_node(f->rel_path));
	if (f->away)
		ADD(box, axis_web_browser_badge("Disk offline", "badge-note"));
	return HTMX_NODE(box);
}

/* The files of a series' folders on disk that are on no episode of its
 * data, each with why; %NULL when there are none. */
static HtmxNode *
unplaced_card(const AxisTvDiskDetail *detail)
{
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (detail == NULL || detail->unplaced->len == 0)
		return NULL;
	card = axis_web_storage_card("Files on disk on no episode");
	ADD(card, axis_web_storage_text("help", "Identify reads each video in the series' folders for the episodes its "
		"name holds. These hold none of the series' episodes, so they count for none; nothing is done with them."));
	table = axis_web_table_new("series-unplaced", unplaced_columns);
	body = htmx_tbody_new();
	for (i = 0; i < detail->unplaced->len; i++) {
		const AxisTvDiskFile *f = g_ptr_array_index(detail->unplaced, i);
		HtmxTr *row = htmx_tr_new();
		g_autofree gchar *why = f->reason != NULL ? g_strdup_printf("%c%s.", g_ascii_toupper(f->reason[0]),
			f->reason + 1) : g_strdup("");

		ADD(row, cell_node(file_place(f)));
		ADD(row, cell_text(why));
		ADD(row, size_cell(f->size));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const gchar *
linked_words(const gchar *how)
{
	if (g_strcmp0(how, "auto") == 0)
		return "By Identify, sure";
	if (g_strcmp0(how, "confirmed") == 0)
		return "Confirmed by a person";
	if (g_strcmp0(how, "chosen") == 0)
		return "Chosen by a person";
	if (g_strcmp0(how, "imported") == 0)
		return "Imported by Axis";
	return "";
}

/* The folders identification linked to the series; %NULL for none. */
static HtmxNode *
folders_card(const AxisTvDiskDetail *detail)
{
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (detail == NULL || detail->folders->len == 0)
		return NULL;
	card = axis_web_storage_card(detail->folders->len == 1 ? "Folder of this series on disk" : "Folders of this series on disk");
	if (detail->folders->len > 1)
		ADD(card, axis_web_storage_text("help", "The series is in more than one folder (on more than one disk, or in "
			"more than one library folder): its episodes are read from all of them, and an episode in two of them has two "
			"copies."));
	table = axis_web_table_new("series-folders", folder_columns);
	body = htmx_tbody_new();
	for (i = 0; i < detail->folders->len; i++) {
		const AxisTvDiskFolder *folder = g_ptr_array_index(detail->folders, i);
		g_autofree gchar *href = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, folder->identity_id);
		g_autofree gchar *count = g_strdup_printf("%u", folder->n_files);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxDiv *disk = htmx_div_new();
		HtmxA *link = htmx_a_new_with_href(href);
		g_autofree gchar *where = g_strdup_printf("%s: ", folder->root_name != NULL ? folder->root_name : "a library folder");

		ADD(name, axis_web_storage_text("episode-reason", where));
		ADD(link, axis_web_browser_name_node(folder->rel_path));
		ADD(name, link);
		ADD(row, name);
		ADD(disk, bdi_text(folder->volume_name));
		if (folder->away)
			ADD(disk, axis_web_browser_badge("Disk offline", "badge-note"));
		ADD(row, cell_node(HTMX_NODE(disk)));
		ADD(row, cell_text(count));
		ADD(row, cell_text(linked_words(folder->linked_how)));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

typedef struct
{
	gint64            title_id;
	gchar            *done;
	AxisTvDiskDetail *detail;
	AxisMetaTitle    *title;	/* brief: for the poster and the heading */
	gchar            *poster;
	gint64            managed_id;	/* the series that manages it, when one does */
	AxisTvChoices     choices;
	gboolean          choices_read;
	AxisTvRules       rules;
	GError           *error;
} DiskWork;

static void
disk_work_free(gpointer data)
{
	DiskWork *work = data;

	g_free(work->done);
	axis_tv_disk_detail_free(work->detail);
	axis_meta_title_free(work->title);
	g_free(work->poster);
	axis_tv_choices_clear(&work->choices);
	g_clear_error(&work->error);
	g_free(work);
}

static void
disk_run(
	AxisTv   *self,
	gpointer  data
){
	DiskWork *work = data;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *title = axis_tv_int_text(work->title_id);
	g_autoptr(GError) local = NULL;
	const gchar *params[1];

	axis_tv_get_rules(self, &work->rules);
	params[0] = title;
	if (!axis_db_pool_exec(self->pool, "SELECT t.kind, t.provider, t.provider_id, t.title, t.year, t.poster_ref,"
	        " (SELECT s.id FROM series s WHERE s.title_id = t.id) FROM meta_title t WHERE t.id = $1::bigint", 1, params,
	        &rows, &work->error))
		return;
	if (axis_db_result_get_n_rows(rows) == 0 || g_strcmp0(axis_db_result_get_text(rows, 0, 0), "series") != 0) {
		g_set_error_literal(&work->error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series on disk");
		return;
	}
	axis_db_result_get_int64(rows, 0, 6, &work->managed_id);
	if (work->managed_id > 0)
		return;
	work->title = axis_meta_title_new(AXIS_META_KIND_SERIES);
	work->title->id = work->title_id;
	work->title->provider = g_strdup(axis_db_result_get_text(rows, 0, 1));
	work->title->provider_id = g_strdup(axis_db_result_get_text(rows, 0, 2));
	work->title->title = g_strdup(axis_db_result_get_text(rows, 0, 3));
	{
		gint64 year = 0;

		axis_db_result_get_int64(rows, 0, 4, &year);
		work->title->year = (gint)year;
	}
	work->title->poster_ref = g_strdup(axis_db_result_get_text(rows, 0, 5));
	if (!axis_tv_get_disk_detail(self, work->title_id, &work->detail, &work->error))
		return;
	if (work->detail->facts == NULL) {
		g_set_error_literal(&work->error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series on disk");
		return;
	}
	work->poster = axis_meta_artwork_find(self->meta, work->title->provider, work->title->poster_ref,
		AXIS_META_ART_POSTER_SMALL, TRUE);
	work->choices_read = axis_tv_web_read_choices(self, &work->choices, &local);
}

/* "On disk", "Missing", "Not aired": an episode of a series on disk. */
static HtmxNode *
disk_mark(
	const AxisTvDiskEpisode *e,
	const AxisTvRules       *rules
){
	HtmxSpan *mark;

	if (e->files->len > 0) {
		g_autofree gchar *css = g_strdup_printf("film-mark-%s", axis_tv_state_name(AXIS_TV_IN_LIBRARY));

		mark = htmx_span_new();
		htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark");
		htmx_element_add_class(HTMX_ELEMENT(mark), css);
		htmx_element_set_attribute(HTMX_ELEMENT(mark), "title", "On disk");
		htmx_node_set_text_content(HTMX_NODE(mark), "On disk");
		return HTMX_NODE(mark);
	}
	if (e->air_date == NULL || strcmp(e->air_date, rules->today) > 0)
		return axis_tv_web_mark(AXIS_TV_NOT_AIRED);
	mark = htmx_span_new();
	htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark");
	htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark-unmonitored");
	htmx_element_set_attribute(HTMX_ELEMENT(mark), "title", "Not on disk");
	htmx_node_set_text_content(HTMX_NODE(mark), "Missing");
	return HTMX_NODE(mark);
}

static const AxisWebColumn disk_episode_columns[] = {
	{ "Episode", AXIS_WEB_COL_TEXT },
	{ "Title", AXIS_WEB_COL_NAME },
	{ "Air date", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "File", AXIS_WEB_COL_WIDE },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ NULL, 0 }
};

static HtmxNode *
disk_season(
	const DiskWork *work,
	gint            season,
	gboolean        open
){
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxDiv *body = htmx_div_new();
	HtmxNode *table = axis_web_table_new("series-episodes", disk_episode_columns);
	HtmxTbody *rows = htmx_tbody_new();
	g_autofree gchar *id = g_strdup_printf("season-%d", season);
	g_autofree gchar *name = season == 0 ? g_strdup("Specials") : g_strdup_printf("Season %d", season);
	g_autofree gchar *meta = NULL;
	guint total = 0;
	guint have = 0;
	guint later = 0;
	guint i;
	guint k;

	for (i = 0; i < work->detail->episodes->len; i++) {
		const AxisTvDiskEpisode *e = g_ptr_array_index(work->detail->episodes, i);
		g_autofree gchar *label = NULL;
		HtmxTr *row;
		HtmxTd *files;
		GString *quality;
		gint64 size = 0;

		if (e->season != season)
			continue;
		total++;
		if (e->files->len > 0)
			have++;
		else if (e->air_date == NULL || strcmp(e->air_date, work->rules.today) > 0)
			later++;
		label = axis_tv_episode_label(e->season, e->number);
		row = htmx_tr_new();
		files = htmx_td_new();
		quality = g_string_new(NULL);
		ADD(row, cell_text(label));
		ADD(row, cell_node(bdi_text(e->title != NULL ? e->title : "Untitled")));
		ADD(row, cell_text(e->air_date != NULL ? e->air_date : "Not known"));
		ADD(row, cell_node(disk_mark(e, &work->rules)));
		for (k = 0; k < e->files->len; k++) {
			const AxisTvDiskFile *f = g_ptr_array_index(e->files, k);

			ADD(files, file_place(f));
			g_string_append_printf(quality, "%s%s", k > 0 ? ", " : "", axis_tv_disk_file_quality(f));
			size += f->size;
		}
		if (e->files->len > 1) {
			g_autofree gchar *copies = g_strdup_printf("%u copies", e->files->len);

			ADD(files, axis_web_storage_text("episode-reason", copies));
		}
		ADD(row, HTMX_NODE(files));
		ADD(row, cell_text(quality->str));
		ADD(row, size_cell(e->files->len > 0 ? size : -1));
		g_string_free(quality, TRUE);
		ADD(rows, row);
	}
	meta = later > 0 ? g_strdup_printf("%u of %u %s on disk, %u not aired yet", have, total,
			total == 1 ? "episode" : "episodes", later) :
		g_strdup_printf("%u of %u %s on disk", have, total, total == 1 ? "episode" : "episodes");
	htmx_element_set_id(HTMX_ELEMENT(details), id);
	htmx_element_add_class(HTMX_ELEMENT(details), "season");
	htmx_details_set_open(details, open);
	ADD(summary, axis_web_storage_text("season-name", name));
	ADD(summary, axis_web_storage_text("season-meta", meta));
	ADD(details, summary);
	htmx_element_add_class(HTMX_ELEMENT(body), "season-body");
	ADD(table, rows);
	ADD(body, table);
	ADD(details, body);
	return HTMX_NODE(details);
}

static HtmxResponse *
disk_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	DiskWork *work = data;
	gboolean admin = axis_web_request_is_admin(request);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(GArray) seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autofree gchar *label = NULL;
	g_autofree gchar *title_href = NULL;
	g_autofree gchar *reason = NULL;
	const AxisTvOnDisk *d;
	HtmxNode *card;
	HtmxDiv *head;
	HtmxDiv *info;
	const gchar *done;
	gint latest = -1;
	guint i;
	guint k;

	if (g_error_matches(work->error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such series on disk.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	/* Managed: its own page shows its episodes. */
	if (work->managed_id > 0) {
		g_autofree gchar *location = g_strdup_printf("/series/%" G_GINT64_FORMAT, work->managed_id);

		return axis_web_auth_redirect(request, location);
	}
	d = work->detail->facts;
	label = axis_tv_series_label(work->title->title, work->title->year);
	title_href = g_strdup_printf("/title/%" G_GINT64_FORMAT, work->title_id);
	done = axis_tv_web_done_message(work->done);
	if (done != NULL)
		ADD(content, axis_web_auth_notice(FALSE, done));

	head = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(head), "series-head");
	ADD(head, axis_web_meta_poster_new(work->title, work->poster, FALSE, 0));
	info = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(info), "series-info");
	ADD(info, heading(1, label));
	reason = axis_tv_web_disk_reason(d);
	{
		HtmxDiv *line = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
		ADD(line, axis_tv_web_mark(AXIS_TV_IN_LIBRARY));
		ADD(line, axis_web_storage_text("film-reason", reason));
		ADD(info, line);
	}
	ADD(info, axis_tv_web_disk_completeness(d));
	{
		g_autofree gchar *size = axis_size_format((guint64)MAX(d->size, 0));
		g_autoptr(GString) line = g_string_new(NULL);

		g_string_append_printf(line, "%u %s on disk, %s", d->n_files, d->n_files == 1 ? "video" : "videos", size);
		if (d->specials > 0)
			g_string_append_printf(line, "; %u %s on disk", d->specials, d->specials == 1 ? "special" : "specials");
		if (d->copies > 0)
			g_string_append_printf(line, "; %u %s more than one copy", d->copies, d->copies == 1 ? "episode has" :
				"episodes have");
		if (d->unplaced > 0)
			g_string_append_printf(line, "; %u %s on no episode (listed below)", d->unplaced, d->unplaced == 1 ?
				"file is" : "files are");
		g_string_append_c(line, '.');
		ADD(info, axis_web_storage_text("help", line->str));
	}
	ADD(info, axis_web_browser_link(title_href, "Series overview"));
	ADD(head, info);
	ADD(content, head);

	/* Managing it: for an administrator, the form; for anyone else, who
	 * can. */
	if (admin && work->choices_read)
		/* (This page does not read the title's details: the hint beside the
		 * "Anime" box is on the series' overview, which does.) */
		ADD(content, axis_tv_web_manage_form(request, d, &work->choices, FALSE));
	else
		ADD(content, axis_web_storage_text("help", admin ? "The quality profiles and library folders could not be read, so it cannot be "
			"managed now." : "Axis does not manage this series. An administrator can have Axis manage it from this "
			"page."));

	/* The seasons, the latest first, the specials last. */
	for (i = 0; i < work->detail->episodes->len; i++) {
		const AxisTvDiskEpisode *e = g_ptr_array_index(work->detail->episodes, i);
		gboolean known = FALSE;

		for (k = 0; k < seasons->len && !known; k++)
			known = g_array_index(seasons, gint, k) == e->season;
		if (!known)
			g_array_append_val(seasons, e->season);
		latest = MAX(latest, e->season);
	}
	card = axis_web_storage_card("Seasons on disk");
	if (work->detail->seasons_unknown->len > 0) {
		g_autoptr(GString) list = g_string_new(NULL);
		g_autofree gchar *text = NULL;

		for (i = 0; i < work->detail->seasons_unknown->len; i++)
			g_string_append_printf(list, "%s%d", i > 0 ? ", " : "", g_array_index(work->detail->seasons_unknown, gint,
				i));
		text = g_strdup_printf("The episodes of %s %s are not known yet: Identify fetches them on its next run, and "
			"their files are placed then.", work->detail->seasons_unknown->len == 1 ? "season" : "seasons", list->str);
		ADD(card, axis_web_storage_text("help", text));
	}
	if (seasons->len == 0)
		ADD(card, axis_web_storage_text("help", "No episodes of the series are known yet."));
	for (k = seasons->len; k > 0; k--) {
		gint season = g_array_index(seasons, gint, k - 1);

		if (season != 0)
			ADD(card, disk_season(work, season, season == latest));
	}
	for (k = 0; k < seasons->len; k++) {
		if (g_array_index(seasons, gint, k) == 0)
			ADD(card, disk_season(work, 0, FALSE));
	}
	ADD(content, card);
	card = unplaced_card(work->detail);
	if (card != NULL)
		ADD(content, card);
	card = folders_card(work->detail);
	if (card != NULL)
		ADD(content, card);
	{
		g_autofree gchar *page_title = g_strdup_printf("%s: episodes on disk", label);

		return axis_web_page_respond(request, page_title, HTMX_NODE(content));
	}
}

static HtmxResponse *
handle_disk(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	DiskWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *done = htmx_request_get_query_param(request, "done");

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series on disk.");
	work = g_new0(DiskWork, 1);
	work->title_id = id;
	if (axis_tv_web_done_message(done) != NULL)
		work->done = g_strdup(done);
	return axis_tv_web_dispatch(user_data, request, disk_run, disk_done, work, disk_work_free);
}

/* ── An episode's or a season's releases ────────────────────────── */

typedef struct
{
	gint64           series_id;
	gint64           episode_id;
	gint             season;
	gboolean         admin;
	gchar           *done;
	AxisTvSeries    *series;
	GPtrArray       *episodes;
	AxisTvReleases  *releases;
	AxisTvRules      rules;
	/* With every release rejected: what each usable profile would make of
	 * those the profile rejected (administrators). */
	AxisTvChoices    choices;
	gboolean         choices_read;
	GPtrArray       *tried;		/* AxisAcquireCandidate, borrowed */
	GArray          *profile_ids;	/* gint64 */
	GPtrArray       *tries;		/* AxisAcquireProfileTry, one per profile id */
	GError          *error;
} ReleasesWork;

static void
releases_work_free(gpointer data)
{
	ReleasesWork *work = data;

	axis_tv_series_free(work->series);
	g_clear_pointer(&work->episodes, g_ptr_array_unref);
	axis_tv_releases_free(work->releases);
	axis_tv_choices_clear(&work->choices);
	g_clear_pointer(&work->tried, g_ptr_array_unref);
	g_clear_pointer(&work->profile_ids, g_array_unref);
	g_clear_pointer(&work->tries, g_ptr_array_unref);
	g_free(work->done);
	g_clear_error(&work->error);
	g_free(work);
}

/* It numbers its episodes differently from the series' data: only Grab this
 * one, from a manual search, takes it (with its warning and tick). */
static gboolean
numbered_otherwise(const AxisAcquireCandidate *c)
{
	guint i;

	for (i = 0; c->reasons != NULL && i < c->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);

		if (g_strcmp0(r->kind, "wanted") == 0 && g_strcmp0(r->code, "not_in_data") == 0)
			return TRUE;
	}
	return FALSE;
}

/* A release the profile rejected that a person may take with one click: not
 * by a fixed rule, not on the blocklist, not numbered otherwise than the
 * data. */
static gboolean
rejected_by_profile(const AxisAcquireCandidate *c)
{
	return !c->accepted && !c->safety && !c->blocklisted && !numbered_otherwise(c);
}

static void
releases_run(
	AxisTv   *self,
	gpointer  data
){
	ReleasesWork *work = data;
	g_autoptr(GError) local = NULL;
	guint i;

	axis_tv_get_rules(self, &work->rules);
	if (!axis_tv_get(self, work->series_id, &work->series, &work->episodes, &work->error))
		return;
	if (!axis_tv_get_releases(self, work->series_id, work->episode_id, work->season, &work->releases, &work->error))
		return;
	/* Every release rejected: which profile would take them, by the decision
	 * a search makes. */
	if (!work->admin || work->releases->search == NULL ||
	    g_strcmp0(work->releases->search->outcome, "none_acceptable") != 0)
		return;
	work->choices_read = axis_tv_web_read_choices(self, &work->choices, &local);
	if (!work->choices_read) {
		g_debug("series: the profiles could not be read: %s", local->message);
		return;
	}
	work->tried = g_ptr_array_new();
	for (i = 0; i < work->releases->candidates->len; i++) {
		const AxisAcquireCandidate *c = g_ptr_array_index(work->releases->candidates, i);

		if (rejected_by_profile(c))
			g_ptr_array_add(work->tried, (gpointer)c);
	}
	work->profile_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	for (i = 0; i < work->choices.profiles->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(work->choices.profiles, i);

		if (c->usable || c->id == work->series->profile_id)
			g_array_append_val(work->profile_ids, c->id);
	}
	g_clear_error(&local);
	if (work->tried->len > 0 && !axis_tv_try_profiles(self, work->series_id, work->releases->search->id, work->tried,
	        work->profile_ids, &work->tries, &local))
		g_debug("series: the profiles could not be tried: %s", local->message);
}

static const AxisWebColumn release_columns[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "Episodes", AXIS_WEB_COL_TEXT },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Found at", AXIS_WEB_COL_TEXT },
	{ "Verdict", AXIS_WEB_COL_STATE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static HtmxNode *
verdict_badge(
	const AxisAcquireCandidate *c,
	gboolean                    mismatched
){
	if (c->note != NULL && strcmp(c->note, "Grabbed.") == 0)
		return axis_web_browser_badge("Grabbed", "badge-running");
	if (c->accepted) {
		g_autofree gchar *text = g_strdup_printf("Accepted, rank %u", c->rank);

		return axis_web_browser_badge(text, "badge-succeeded");
	}
	if (c->blocklisted)
		return axis_web_browser_badge("Blocklisted", "badge-warn");
	if (mismatched)
		return axis_web_browser_badge("Episode not listed", "badge-warn");
	return axis_web_browser_badge(c->safety ? "Ruled out" : "Rejected", NULL);
}

static gint
compare_candidates(
	gconstpointer a,
	gconstpointer b
){
	const AxisAcquireCandidate *x = *(const AxisAcquireCandidate *const *)a;
	const AxisAcquireCandidate *y = *(const AxisAcquireCandidate *const *)b;

	if (x->accepted != y->accepted)
		return x->accepted ? -1 : 1;
	if (x->accepted && x->rank != y->rank)
		return x->rank < y->rank ? -1 : 1;
	return x->position < y->position ? -1 : x->position > y->position;
}

static gchar *
age_text(gint64 us)
{
	gint64 seconds = (g_get_real_time() - us) / G_USEC_PER_SEC;

	if (us <= 0)
		return g_strdup("");
	if (seconds < 3600)
		return g_strdup("less than an hour");
	if (seconds < 48 * 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " hours", seconds / 3600);
	return g_strdup_printf("%" G_GINT64_FORMAT " days", seconds / 86400);
}

static HtmxNode *
release_row(
	HtmxRequest                *request,
	const ReleasesWork         *work,
	const AxisAcquireCandidate *c,
	const gchar                *holds,
	gboolean                    mismatched,
	gboolean                    admin,
	AxisWebNotice              *notice
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxBdi *bdi = htmx_bdi_new();
	g_autofree gchar *size = c->size_bytes >= 0 ? axis_quality_format_size(c->size_bytes) : NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *age = c->published_us > 0 ? age_text(c->published_us) : NULL;
	g_autofree gchar *row_id = g_strdup_printf("release-%" G_GINT64_FORMAT, (gint64)c->id);
	AxisQuality q;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(row), row_id);
	if (c->accepted)
		htmx_element_add_class(HTMX_ELEMENT(row), "release-accepted");
	{
		HtmxNode *here = axis_web_notice_take(notice, row_id);

		if (here != NULL)
			ADD(name, here);
	}
	ADD(bdi, axis_web_breakable(c->name));
	ADD(name, bdi);
	if (c->reasons->len > 0 || c->note != NULL || c->match_text != NULL) {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
		for (i = 0; i < c->reasons->len; i++) {
			const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);
			HtmxLi *item = htmx_li_new();
			g_autofree gchar *css = g_strdup_printf("reason-%s", r->kind != NULL ? r->kind : "profile");

			htmx_element_add_class(HTMX_ELEMENT(item), css);
			htmx_node_set_text_content(HTMX_NODE(item), r->text);
			ADD(list, item);
		}
		if (c->reasons->len == 0 && c->match_text != NULL) {
			HtmxLi *item = htmx_li_new();
			g_autofree gchar *text = g_strdup_printf("%c%s.", g_ascii_toupper(c->match_text[0]), c->match_text + 1);

			htmx_element_add_class(HTMX_ELEMENT(item), "reason-match");
			htmx_node_set_text_content(HTMX_NODE(item), text);
			ADD(list, item);
		}
		if (c->note != NULL) {
			HtmxLi *item = htmx_li_new();

			htmx_element_add_class(HTMX_ELEMENT(item), "reason-note");
			htmx_node_set_text_content(HTMX_NODE(item), c->note);
			ADD(list, item);
		}
		ADD(name, list);
	}
	ADD(row, name);
	ADD(row, cell_text(holds != NULL ? holds : ""));
	ADD(row, cell_text(c->quality != NULL && axis_quality_from_name(c->quality, &q) ? axis_quality_label(q) :
		"Unknown"));
	ADD(row, cell_text(size != NULL ? size : ""));
	if (c->protocol == AXIS_DOWNLOAD_TORRENT && c->seeders >= 0)
		where = g_strdup_printf("Torrent, %d %s", c->seeders, c->seeders == 1 ? "seeder" : "seeders");
	else if (c->protocol == AXIS_DOWNLOAD_TORRENT)
		where = g_strdup("Torrent");
	else
		where = g_strdup_printf("Usenet%s%s", age != NULL && age[0] != '\0' ? ", " : "", age != NULL ? age : "");
	{
		HtmxTd *cell = htmx_td_new();
		HtmxSpan *indexer = htmx_span_new();
		HtmxSpan *detail = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(indexer), c->indexer != NULL ? c->indexer : "An indexer");
		htmx_element_add_class(HTMX_ELEMENT(detail), "range-detail");
		htmx_node_set_text_content(HTMX_NODE(detail), where);
		ADD(cell, indexer);
		ADD(cell, detail);
		ADD(row, cell);
	}
	ADD(row, cell_node(verdict_badge(c, mismatched)));
	if (admin && (c->accepted || rejected_by_profile(c))) {
		g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/grab", work->series_id);
		g_autofree gchar *id = axis_tv_int_text(c->id);
		g_autofree gchar *back = axis_web_back_here(request);

		/* Back at this row, with what was sent. */
		ADD(row, cell_node(button_form(request, action, c->accepted ? "Grab" : "Grab it anyway", c->accepted ?
			"button-primary button-small" : "button-small", "candidate", id, "back", back != NULL ? back : "",
			AXIS_WEB_PLACE_AT, row_id, NULL)));
	} else if (admin && numbered_otherwise(c)) {
		ADD(row, cell_text("Only by a manual search"));
	} else {
		ADD(row, cell_text(""));
	}
	return HTMX_NODE(row);
}

static const AxisWebColumn block_columns[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "How it got here", AXIS_WEB_COL_TEXT },
	{ "Why", AXIS_WEB_COL_WIDE },
	{ "Since", AXIS_WEB_COL_DATE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

/* An entry's origin: a failure's, or a person's (add-block-release). */
static gchar *
block_origin(const AxisAcquireBlock *b)
{
	const gchar *who = b->created_by != NULL ? b->created_by : "an administrator";

	if (g_strcmp0(b->reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0)
		return g_strdup_printf("Blocked by %s", who);
	if (g_strcmp0(b->reason, "given_up") == 0)
		return g_strdup_printf("Download given up on by %s", who);
	return g_strdup("Failed");
}

static HtmxNode *
blocklist_card(
	HtmxRequest        *request,
	const ReleasesWork *work,
	gboolean            admin,
	AxisWebNotice      *notice
){
	HtmxNode *card = axis_web_storage_card("Blocklist of this series");
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/blocklist/clear", work->series_id);
	g_autofree gchar *back = axis_web_back_here(request);
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "blocklist");
	axis_web_notice_put(notice, card, "blocklist");
	if (work->releases->blocklist == NULL || work->releases->blocklist->len == 0) {
		ADD(card, axis_web_storage_text("help", "Nothing of this series is on its blocklist: a release that fails is "
			"put here and not tried again, and so is a release an administrator blocks, from a manual search or from "
			"a download."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "No search takes a release that is listed here. A release that failed is "
		"blocked from the indexer it came from, and by its hash when it is a torrent. A release a person blocked is "
		"blocked by its name from every indexer. Unblock takes one release off the list."));
	table = axis_web_table_new("series-blocklist", block_columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->releases->blocklist->len; i++) {
		const AxisAcquireBlock *b = g_ptr_array_index(work->releases->blocklist, i);
		HtmxTr *row = htmx_tr_new();
		g_autofree gchar *id = axis_tv_int_text(b->id);
		g_autofree gchar *origin = block_origin(b);

		ADD(row, cell_node(axis_web_breakable(b->release_name)));
		ADD(row, cell_text(origin));
		ADD(row, cell_text(b->reason_text != NULL ? b->reason_text : b->reason));
		ADD(row, cell_time(b->created_us));
		ADD(row, admin ? cell_node(button_form(request, action, "Unblock", "button-small", "entry", id, "back",
			back != NULL ? back : "", AXIS_WEB_PLACE_AT, "blocklist", NULL)) : cell_text(""));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

/* ── What can be done about what a search found ─────────────────── */

static const AxisTvChoice *
profile_choice(
	const AxisTvChoices *choices,
	gint64               id
){
	guint i;

	for (i = 0; choices->profiles != NULL && i < choices->profiles->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(choices->profiles, i);

		if (c->id == id)
			return c;
	}
	return NULL;
}

/* When Axis searches by itself next for what the page is for: the
 * episode's, or the earliest of the season's monitored episodes without a
 * file; 0 for not known. */
static gint64
next_search_of(
	const ReleasesWork *work,
	gint                season
){
	gint64 next = 0;
	guint i;

	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);

		if (work->episode_id > 0 ? e->id != work->episode_id : e->season != season || !e->monitored || e->n_files > 0)
			continue;
		if (e->next_search_us > 0 && (next == 0 || e->next_search_us < next))
			next = e->next_search_us;
	}
	return next;
}

/* "Use another quality profile for this series": each usable profile, with
 * what it makes of the releases the profile rejected (the decision a search
 * makes), saved through the series' own edit, which searches again. */
static HtmxNode *
profile_form(
	HtmxRequest        *request,
	const ReleasesWork *work,
	gint                season
){
	const AxisTvSeries *s = work->series;
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/edit", s->id);
	g_autofree gchar *root = axis_tv_int_text(s->root_id);
	g_autofree gchar *episode = work->episode_id > 0 ? axis_tv_int_text(work->episode_id) : NULL;
	g_autofree gchar *season_text = axis_tv_int_text(season);
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *back = axis_web_back_field(request);
	HtmxFieldset *set = htmx_fieldset_new();
	guint n = work->tried != NULL ? work->tried->len : 0;
	gint64 chosen = s->profile_id;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(form), "profile-choice");
	ADD(form, axis_web_auth_hidden("root", root));
	if (episode != NULL)
		ADD(form, axis_web_auth_hidden("episode", episode));
	else
		ADD(form, axis_web_auth_hidden("season", season_text));
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, "resolve"));
	/* The first other profile that takes what was found is chosen. */
	for (i = 0; work->tries != NULL && i < work->tries->len && chosen == s->profile_id; i++) {
		const AxisAcquireProfileTry *t = g_ptr_array_index(work->tries, i);

		if (t->n_accepted > 0 && t->profile_id != s->profile_id)
			chosen = t->profile_id;
	}
	htmx_element_add_class(HTMX_ELEMENT(set), "profile-group");
	ADD(set, htmx_legend_new_with_text("Use another quality profile for this series"));
	for (i = 0; work->profile_ids != NULL && i < work->profile_ids->len; i++) {
		gint64 id = g_array_index(work->profile_ids, gint64, i);
		const AxisTvChoice *c = profile_choice(&work->choices, id);
		const AxisAcquireProfileTry *t = work->tries != NULL && i < work->tries->len ?
			g_ptr_array_index(work->tries, i) : NULL;
		g_autofree gchar *value = axis_tv_int_text(id);
		g_autofree gchar *input_id = g_strdup_printf("profile-choice-%" G_GINT64_FORMAT, id);
		g_autofree gchar *note = NULL;
		HtmxDiv *div = htmx_div_new();
		HtmxInput *input = htmx_input_new(HTMX_INPUT_RADIO);
		HtmxLabel *label = htmx_label_new_for(input_id);
		HtmxSpan *name = htmx_span_new();
		HtmxSpan *what = htmx_span_new();

		if (c == NULL)
			continue;
		if (t == NULL || n == 0)
			note = g_strdup("");
		else if (t->n_accepted > 0)
			note = n == 1 ? g_strdup(": accepts this release") : g_strdup_printf(": accepts %u of these %u releases",
				t->n_accepted, n);
		else
			note = g_strdup_printf(": %s%s", n == 1 ? "rejects it" : "rejects them", t->reason != NULL ? ", " : "");
		htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
		htmx_element_add_class(HTMX_ELEMENT(div), t != NULL && t->n_accepted > 0 ? "profile-accepts" :
			"profile-rejects");
		htmx_input_set_name(input, "profile");
		htmx_input_set_value(input, value);
		htmx_element_set_id(HTMX_ELEMENT(input), input_id);
		if (id == chosen)
			htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
		htmx_node_set_text_content(HTMX_NODE(name), c->label);
		ADD(label, name);
		if (id == s->profile_id) {
			HtmxSpan *now = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(now), " (the series' profile now)");
			ADD(label, now);
		}
		htmx_element_add_class(HTMX_ELEMENT(what), "profile-verdict");
		htmx_node_set_text_content(HTMX_NODE(what), note);
		ADD(label, what);
		if (t != NULL && n > 0 && t->n_accepted == 0 && t->reason != NULL) {
			HtmxSpan *why = htmx_span_new();

			htmx_element_add_class(HTMX_ELEMENT(why), "profile-verdict");
			htmx_node_set_text_content(HTMX_NODE(why), t->reason);
			ADD(label, why);
		}
		ADD(div, input);
		ADD(div, label);
		ADD(set, div);
	}
	ADD(form, set);
	ADD(form, axis_web_auth_button("Use this profile and search again", "button-primary button-small"));
	return form;
}

/* The search's own place on the page: the releases' search form fields. */
static HtmxNode *
search_again(
	HtmxRequest        *request,
	const ReleasesWork *work,
	gint                season,
	const gchar        *back
){
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/search", work->series_id);
	g_autofree gchar *item = work->episode_id > 0 ? axis_tv_int_text(work->episode_id) : axis_tv_int_text(season);
	g_autofree gchar *season_text = axis_tv_int_text(season);

	return button_form(request, action, "Search again now", "button-primary button-small", "scope",
		work->episode_id > 0 ? "episode" : "season", "item", item, "season", season_text, "back", back,
		AXIS_WEB_PLACE_AT, "resolve", NULL);
}

/* Next to what the last search ended in: what puts it right, by its kind.
 * Administrators get the actions; anyone else the words. %NULL when there
 * is nothing to do. */
static HtmxNode *
resolve_box(
	HtmxRequest        *request,
	const ReleasesWork *work,
	gint                season,
	gboolean            admin,
	AxisWebNotice      *notice
){
	const AxisAcquireSearchInfo *s = work->releases->search;
	const AxisTvSeries *series = work->series;
	g_autofree gchar *back = axis_web_back_here(request);
	g_autofree gchar *settings = g_strdup_printf("/series/%" G_GINT64_FORMAT "#settings", work->series_id);
	AxisAcquireAttention kind;
	HtmxDiv *box;

	if (s == NULL || s->finished_us == 0)
		return NULL;
	if (back == NULL)
		back = g_strdup("");
	kind = axis_acquire_attention_of_outcome(s->outcome);
	if (kind == AXIS_ACQUIRE_ATTENTION_NONE && g_strcmp0(s->outcome, "nothing_found") != 0)
		return NULL;
	box = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(box), "resolve");
	htmx_element_add_class(HTMX_ELEMENT(box), "resolve-box");
	ADD(box, heading(3, "What you can do"));
	axis_web_notice_put(notice, HTMX_NODE(box), "resolve");
	if (g_strcmp0(s->outcome, "nothing_found") == 0) {
		gint64 next = next_search_of(work, season);
		g_autofree gchar *when = next > work->rules.now_us ? axis_web_format_time(next, "") : NULL;
		g_autofree gchar *text = when != NULL ? g_strdup_printf("Axis searches again by itself after %s, with more "
			"time between searches while nothing is found.", when) : g_strdup("Axis searches again by itself on the "
			"scheduler's next run.");

		ADD(box, axis_web_storage_text("help", text));
		if (admin)
			ADD(box, search_again(request, work, season, back));
		return HTMX_NODE(box);
	}
	if (kind == AXIS_ACQUIRE_ATTENTION_REJECTED) {
		guint n = 0;
		guint i;
		const AxisAcquireCandidate *only = NULL;

		for (i = 0; i < work->releases->candidates->len; i++) {
			const AxisAcquireCandidate *c = g_ptr_array_index(work->releases->candidates, i);

			if (rejected_by_profile(c)) {
				n++;
				only = c;
			}
		}
		if (!admin) {
			ADD(box, axis_web_storage_text("help", "An administrator can grab a release anyway, or use another quality "
				"profile for this series, which searches again with it."));
			return HTMX_NODE(box);
		}
		/* (a) Grab it anyway, with the guards of a grab by hand. */
		if (n == 1) {
			g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/grab", work->series_id);
			g_autofree gchar *id = axis_tv_int_text(only->id);
			HtmxDiv *line = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "film-actions");
			ADD(line, button_form(request, action, "Grab it anyway", "button-small", "candidate", id, "back", back,
				AXIS_WEB_PLACE_AT, "resolve", NULL));
			ADD(line, axis_web_storage_text("help", "Sends the release the profile rejected, as a grab by hand."));
			ADD(box, line);
		} else if (n > 1) {
			ADD(box, axis_web_storage_text("help", "Take one of the releases below with Grab it anyway: it is sent as a "
				"grab by hand, whatever the profile says."));
		}
		/* (b) Another profile, marked by what it makes of them. */
		if (work->choices_read && work->profile_ids != NULL && work->profile_ids->len > 0)
			ADD(box, profile_form(request, work, season));
		/* (c) The profile's own limits. */
		if (series->profile_slug != NULL) {
			HtmxP *line = htmx_p_new();
			g_autofree gchar *href = g_strdup_printf("/quality/%s", series->profile_slug);
			g_autofree gchar *words = g_strdup_printf("Change the limits of %s itself, on its page",
				series->profile_name != NULL ? series->profile_name : "the profile");

			htmx_element_add_class(HTMX_ELEMENT(line), "help");
			ADD(line, axis_web_browser_link(href, words));
			ADD(box, line);
		}
		return HTMX_NODE(box);
	}
	switch (kind) {
	case AXIS_ACQUIRE_ATTENTION_EXHAUSTED:
		ADD(box, axis_web_storage_text("help", "The releases that failed before are on the series' blocklist, below. "
			"Take one off it, or clear the blocklist, and search again; or wait for new releases."));
		if (admin) {
			g_autofree gchar *clear = g_strdup_printf("/series/%" G_GINT64_FORMAT "/blocklist/clear", work->series_id);
			HtmxDiv *line = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "film-actions");
			ADD(line, button_form(request, clear, "Clear the blocklist", "button-small", "back", back,
				AXIS_WEB_PLACE_AT, "resolve", NULL));
			ADD(line, search_again(request, work, season, back));
			ADD(box, line);
		}
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_INDEXER:
	case AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR:
	case AXIS_ACQUIRE_ATTENTION_NO_CLIENT:
	case AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR: {
		gboolean indexers = kind == AXIS_ACQUIRE_ATTENTION_NO_INDEXER || kind == AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR;

		if (admin) {
			HtmxP *line = htmx_p_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "help");
			ADD(line, axis_web_browser_link(indexers ? "/indexers" : "/downloads/clients", indexers ?
				"Look at the indexers" : "Look at the download clients"));
			ADD(box, line);
			ADD(box, search_again(request, work, season, back));
		} else {
			ADD(box, axis_web_storage_text("help", indexers ? "An administrator can look at the indexers and search "
				"again." : "An administrator can look at the download clients and search again."));
		}
		break;
	}
	case AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE:
	case AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE: {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		ADD(line, axis_web_browser_link(settings, kind == AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE ?
			"Choose another destination under Settings for this series" :
			"Choose another quality profile under Settings for this series"));
		ADD(box, line);
		if (admin)
			ADD(box, search_again(request, work, season, back));
		break;
	}
	case AXIS_ACQUIRE_ATTENTION_METADATA:
		if (admin) {
			g_autofree gchar *sync = g_strdup_printf("/series/%" G_GINT64_FORMAT "/sync", work->series_id);
			HtmxDiv *line = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "film-actions");
			ADD(line, button_form(request, sync, "Update details now", "button-small", "back", back,
				AXIS_WEB_PLACE_AT, "resolve", NULL));
			ADD(line, search_again(request, work, season, back));
			ADD(box, line);
		} else {
			ADD(box, axis_web_storage_text("help", "An administrator can update the series' details and search "
				"again."));
		}
		break;
	default:
		if (admin)
			ADD(box, search_again(request, work, season, back));
		else
			ADD(box, axis_web_storage_text("help", "An administrator can search again."));
		break;
	}
	return HTMX_NODE(box);
}

static HtmxResponse *
releases_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	ReleasesWork *work = data;
	gboolean admin = axis_web_request_is_admin(request);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(GPtrArray) order = g_ptr_array_new();
	g_autofree gchar *label = NULL;
	g_autofree gchar *what = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *page_title = NULL;
	const AxisAcquireSearchInfo *s;
	AxisWebNotice notice;
	HtmxNode *card;
	gint season = work->season;
	guint i;

	if (g_error_matches(work->error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	label = axis_tv_series_label(work->series->title, work->series->year);
	if (work->episode_id > 0) {
		for (i = 0; i < work->episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
			g_autofree gchar *number = NULL;

			if (e->id != work->episode_id || what != NULL)
				continue;
			season = e->season;
			number = axis_tv_episode_label(e->season, e->number);
			what = e->title != NULL ? g_strdup_printf("%s, %s", number, e->title) : g_strdup(number);
		}
		if (what == NULL)
			return axis_web_error_respond(request, 404, "Not found", "The series has no such episode.");
	} else {
		what = season == 0 ? g_strdup("the specials") : g_strdup_printf("season %d", season);
	}
	back = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%d", work->series_id, season);
	if (work->episode_id == 0 && season == 0)
		page_title = g_strdup_printf("Releases found for the specials of %s", work->series->title);
	else
		page_title = g_strdup_printf("Releases found for %s %s", work->series->title, what);
	/* The notice where the form was: the box of what puts it right, a
	 * release's row, the blocklist (add-activity-feedback). */
	axis_web_notice_begin(&notice, request, FALSE, axis_tv_web_done_text(work->done, work->series, work->episodes),
		HTMX_NODE(content));
	ADD(content, heading(1, page_title));
	{
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "series-back");
		ADD(line, link_text(back, label));
		ADD(content, line);
	}
	/* A person's own look: everything the indexers have, to choose from. */
	{
		HtmxDiv *actions = htmx_div_new();
		const gchar *level = work->episode_id > 0 ? "episode" : "season";
		gint64 item = work->episode_id > 0 ? work->episode_id : season;
		g_autofree gchar *manual = axis_web_manual_path("/series", work->series_id, level, item);

		htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
		if (admin)
			ADD(actions, axis_web_manual_button(request, "/series", work->series_id, level, item, "Manual search"));
		ADD(actions, axis_web_browser_link(manual, "The last manual search"));
		ADD(content, actions);
	}
	card = axis_web_storage_card("Releases found by the last search");
	s = work->releases->search;
	if (s == NULL) {
		ADD(card, axis_web_storage_text("help", "Not searched for yet. What a search finds is listed here, each release "
			"accepted or rejected with every reason and the episodes it holds."));
	} else {
		g_autofree gchar *when = axis_web_format_time(s->finished_us > 0 ? s->finished_us : s->started_us, "");
		g_autofree gchar *found = s->how != NULL ? g_strdup(s->how) : g_strdup_printf("%u found", s->n_found);
		g_autofree gchar *line = g_strdup_printf("Last search %s%s%s. %s; %u accepted.", when,
			s->requested_by != NULL ? " by " : "", s->requested_by != NULL ? s->requested_by : "", found, s->n_accepted);

		ADD(card, axis_web_storage_text("film-search-line", line));
		if (s->outcome_text != NULL)
			ADD(card, axis_web_storage_text("film-outcome", s->outcome_text));
		/* Next to what it ended in, what puts it right. */
		{
			HtmxNode *resolve = resolve_box(request, work, season, admin, &notice);

			if (resolve != NULL)
				ADD(card, resolve);
		}
		for (i = 0; i < work->releases->candidates->len; i++)
			g_ptr_array_add(order, g_ptr_array_index(work->releases->candidates, i));
		g_ptr_array_sort(order, compare_candidates);
		if (order->len > 0) {
			HtmxNode *table = axis_web_table_new("series-releases", release_columns);
			HtmxTbody *body = htmx_tbody_new();

			for (i = 0; i < order->len && i < AXIS_TV_WEB_RELEASES_SHOWN; i++) {
				const AxisAcquireCandidate *c = g_ptr_array_index(order, i);
				const gchar *holds = NULL;
				gboolean mismatched = FALSE;
				guint k;

				for (k = 0; k < work->releases->candidates->len; k++) {
					if (g_ptr_array_index(work->releases->candidates, k) != c)
						continue;
					holds = g_ptr_array_index(work->releases->holds, k);
					mismatched = g_array_index(work->releases->mismatched, gboolean, k);
				}
				ADD(body, release_row(request, work, c, holds, mismatched, admin, &notice));
			}
			ADD(table, body);
			ADD(card, table);
			ADD(card, axis_web_storage_text("help", "\"Ruled out\": a fixed rule rejects it (another series, no "
				"episode named, a sample...), and no one can "
				"grab it. \"Episode not listed\": it is numbered for an episode the episode list does not have; "
				"Settings for this series counts these."));
			if (admin)
				ADD(card, axis_web_storage_text("help", "\"Grab it anyway\" sends a release that was rejected (not "
					"wanted, or by the profile), as a grab by hand; the episodes it holds are claimed all the same, and "
					"one in flight stops it. A release numbered differently from TMDb's episode list is taken only by "
					"Grab this one on a manual search, after its warning."));
		}
	}
	ADD(content, card);
	ADD(content, blocklist_card(request, work, admin, &notice));
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, page_title, HTMX_NODE(content));
}

static HtmxResponse *
handle_releases(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ReleasesWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *episode = htmx_request_get_query_param(request, "episode");
	const gchar *season = htmx_request_get_query_param(request, "season");
	guint64 value = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	work = g_new0(ReleasesWork, 1);
	work->series_id = id;
	work->season = -1;
	work->admin = axis_web_request_is_admin(request);
	{
		const gchar *done = htmx_request_get_query_param(request, "done");
		g_autofree gchar *known = axis_tv_web_done_text(done, NULL, NULL);

		if (known != NULL)
			work->done = g_strdup(done);
	}
	if (episode != NULL && g_ascii_string_to_unsigned(episode, 10, 1, G_MAXINT64, &value, NULL))
		work->episode_id = (gint64)value;
	else if (season != NULL && g_ascii_string_to_unsigned(season, 10, 0, 10000, &value, NULL))
		work->season = (gint)value;
	if (work->episode_id == 0 && work->season < 0) {
		releases_work_free(work);
		return axis_web_error_respond(request, 400, "Not shown", "Say which episode or season.");
	}
	return axis_tv_web_dispatch(user_data, request, releases_run, releases_done, work, releases_work_free);
}

/* ── The calendar ───────────────────────────────────────────────── */

/* The days the calendar shows: a week back, two ahead. */
static void
calendar_days(
	const AxisTvRules *rules,
	gchar              from[11],
	gchar              to[11]
){
	axis_tv_today(rules->now_us - (gint64)7 * 86400 * G_USEC_PER_SEC, from);
	axis_tv_today(rules->now_us + (gint64)14 * 86400 * G_USEC_PER_SEC, to);
}

/* The series (as a list reads them) and the episodes that air in the
 * calendar's three weeks, by the index on the air date: two statements, and
 * no episode of another day is read. */
static void
calendar_run(
	AxisTv   *self,
	gpointer  data
){
	AllWork *work = data;
	g_autofree gchar *days = NULL;
	AxisDbConn *conn;
	gchar from[11];
	gchar to[11];

	axis_tv_get_rules(self, &work->rules);
	calendar_days(&work->rules, from, to);
	days = g_strdup_printf("{%s,%s}", from, to);
	conn = axis_db_pool_acquire(self->pool, &work->error);
	if (conn == NULL)
		return;
	if (axis_tv_store_series(conn, AXIS_TV_SERIES_LIST, NULL, &work->series, &work->error))
		axis_tv_store_episodes_of(conn, AXIS_TV_EPISODES_OF_DAYS, days, FALSE, &work->episodes, &work->error);
	axis_db_pool_release(self->pool, conn);
}

static gint
compare_dates(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static gchar *
day_heading(
	const gchar *date,
	const gchar *today
){
	gint64 us = axis_tv_date_us(date);
	g_autoptr(GDateTime) when = us > 0 ? g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC) : NULL;
	g_autofree gchar *text = when != NULL ? g_date_time_format(when, "%A %-e %B") : g_strdup(date);

	return g_strdup_printf("%s%s", g_strstrip(text), strcmp(date, today) == 0 ? ", today" : "");
}

static HtmxResponse *
calendar_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	AllWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(GPtrArray) days = g_ptr_array_new_with_free_func(g_free);
	gchar from[11];
	gchar to[11];
	HtmxDiv *calendar;
	guint shown = 0;
	guint i;
	guint k;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	calendar_days(&work->rules, from, to);
	ADD(content, heading(1, "TV calendar"));
	ADD(content, axis_web_storage_text(NULL, "The episodes of the series Axis manages, from a week ago to two weeks "
		"ahead, by the day they air (in UTC), with the state of each. An episode already in the library is not "
		"listed, and neither is anything of a series on disk that Axis does not manage."));
	/* The days with episodes, in order. */
	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
		gboolean known = FALSE;

		if (e->air_date == NULL || strcmp(e->air_date, from) < 0 || strcmp(e->air_date, to) > 0 || e->n_files > 0)
			continue;
		for (k = 0; k < days->len && !known; k++)
			known = strcmp(g_ptr_array_index(days, k), e->air_date) == 0;
		if (!known)
			g_ptr_array_add(days, g_strdup(e->air_date));
	}
	g_ptr_array_sort(days, compare_dates);
	calendar = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(calendar), "series-calendar");
	for (k = 0; k < days->len; k++) {
		const gchar *date = g_ptr_array_index(days, k);
		g_autofree gchar *head = day_heading(date, work->rules.today);
		HtmxElement *section = HTMX_ELEMENT(htmx_section_new());
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(section, "calendar-day");
		if (strcmp(date, work->rules.today) == 0)
			htmx_element_add_class(section, "calendar-today");
		ADD(section, heading(2, head));
		htmx_element_add_class(HTMX_ELEMENT(list), "calendar-list");
		for (i = 0; i < work->episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
			const AxisTvSeries *s;
			g_autofree gchar *label = NULL;
			g_autofree gchar *number = NULL;
			g_autofree gchar *href = NULL;
			HtmxLi *item;

			if (g_strcmp0(e->air_date, date) != 0 || e->n_files > 0 || (s = series_of(work, e->series_id)) == NULL)
				continue;
			label = axis_tv_series_label(s->title, s->year);
			number = axis_tv_episode_label(e->season, e->number);
			href = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%d", s->id, e->season);
			item = htmx_li_new();
			htmx_element_add_class(HTMX_ELEMENT(item), "calendar-item");
			ADD(item, axis_tv_web_mark(axis_tv_episode_derive(e, s, &work->rules, NULL)));
			ADD(item, link_text(href, label));
			ADD(item, axis_web_storage_text("calendar-number", number));
			ADD(item, axis_web_storage_text("calendar-title", e->title != NULL ? e->title : ""));
			ADD(list, item);
			shown++;
		}
		ADD(section, list);
		ADD(calendar, section);
	}
	if (shown == 0) {
		/* (The empty calendar is not shown: it goes.) */
		g_object_unref(calendar);
		ADD(content, axis_web_storage_text("no-results", work->series->len == 0 ? "No series is managed yet: add one, "
			"or manage one on disk from its page, and its episodes appear here by the day they air." : "No episode of "
			"your series that is not in the library airs in these three weeks."));
	} else {
		ADD(content, calendar);
	}
	return axis_web_page_respond(request, "TV calendar", HTMX_NODE(content));
}

static HtmxResponse *
handle_calendar(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_tv_web_dispatch(user_data, request, calendar_run, calendar_done, g_new0(AllWork, 1), all_work_free);
}

/* ── Wanted ─────────────────────────────────────────────────────── */

/* The series and the episode share a column: the reason beside them needs
 * the room. */
static const AxisWebColumn wanted_columns[] = {
	{ "Episode", AXIS_WEB_COL_NAME },
	{ "Air date", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Why", AXIS_WEB_COL_WIDE },
	{ "Last searched", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

/* What the list holds: the episodes missing (wanted, searched for, needing
 * attention); an episode on disk below its profile's cutoff only under its
 * own tab (Upgrades), so that the list never fills with episodes on disk. */
static gboolean
wanted_state(
	AxisTvState state,
	gboolean    upgrades
){
	if (upgrades)
		return state == AXIS_TV_UPGRADE_WANTED;
	return state == AXIS_TV_WANTED || state == AXIS_TV_ATTENTION || state == AXIS_TV_SEARCHING;
}

/* How many episodes a page of Wanted lists. */
#define WANTED_PAGE 100

/*
 * A page of Wanted, in three statements whatever the library's size.  The
 * series and their episodes counted by state (axis_tv_store_overview()): the
 * tabs' numbers, and the place of every episode that is in a state the tab
 * lists (for state=held, of those that need attention because their last
 * search found a release only a person can grab: series_search.n_person_only,
 * the count the page "Needs attention" reads for its line about them).  They
 * are ordered by series, season and number and cut to the page; then the
 * facts of the page's episodes alone are read (the episode facts statement,
 * by their ids), for the words of each row.  No episode that is not on the
 * page is read.
 */
static void
wanted_run(
	AxisTv   *self,
	gpointer  data
){
	AllWork *work = data;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GArray) members = NULL;
	g_autoptr(GArray) kept = g_array_new(FALSE, FALSE, sizeof(AxisTvMember));
	g_autoptr(GString) ids = g_string_new("{");
	gboolean upgrades = work->filtered && work->filter == AXIS_TV_UPGRADE_WANTED;
	gint64 after = cursor_id(work->after);
	gint64 before = after == 0 ? cursor_id(work->before) : 0;
	AxisTvTotals totals;
	AxisDbConn *conn;
	guint start = 0;
	guint end;
	guint i;

	axis_tv_get_rules(self, &work->rules);
	conn = axis_db_pool_acquire(self->pool, &work->error);
	if (conn == NULL)
		return;
	if (!axis_tv_store_overview(self, conn, NULL, upgrades ? AXIS_TV_MEMBERS_OF_UPGRADES : AXIS_TV_MEMBERS_OF_WANTED,
	        &all, &totals, &members, &work->error)) {
		axis_db_pool_release(self->pool, conn);
		return;
	}
	memcpy(work->counts, totals.counts, sizeof(work->counts));
	work->total = totals.counts[AXIS_TV_WANTED] + totals.counts[AXIS_TV_ATTENTION] + totals.counts[AXIS_TV_SEARCHING];
	work->series = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_series_free);
	for (i = 0; i < all->len; i++) {
		AxisTvSummary *summary = g_ptr_array_index(all, i);

		g_ptr_array_add(work->series, g_steal_pointer(&summary->series));
	}
	/* The tab's episodes, in order (the members are ordered already). */
	for (i = 0; i < members->len; i++) {
		const AxisTvMember *member = &g_array_index(members, AxisTvMember, i);

		if (!wanted_state(member->state, upgrades) || (work->filtered && member->state != work->filter) ||
		    (work->held && !member->held))
			continue;
		g_array_append_val(kept, *member);
	}
	work->matching = kept->len;
	/* The page: after an episode, before one, or the first (an episode no
	 * longer listed: the first). */
	end = MIN(WANTED_PAGE, kept->len);
	for (i = 0; i < kept->len; i++) {
		gint64 id = g_array_index(kept, AxisTvMember, i).id;

		if (after != 0 && id == after) {
			start = i + 1;
			end = MIN(start + WANTED_PAGE, kept->len);
		} else if (before != 0 && id == before) {
			end = i;
			start = end > WANTED_PAGE ? end - WANTED_PAGE : 0;
		}
	}
	if (end < kept->len && end > start)
		work->next = g_strdup_printf("%" G_GINT64_FORMAT, g_array_index(kept, AxisTvMember, end - 1).id);
	if (start > 0 && end > start)
		work->previous = g_strdup_printf("%" G_GINT64_FORMAT, g_array_index(kept, AxisTvMember, start).id);
	for (i = start; i < end; i++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, ids->len > 1 ? "," : "",
			g_array_index(kept, AxisTvMember, i).id);
	g_string_append_c(ids, '}');
	if (end > start)
		axis_tv_store_episodes_of(conn, AXIS_TV_EPISODES_OF_IDS, ids->str, FALSE, &work->episodes, &work->error);
	else
		work->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_episode_free);
	axis_db_pool_release(self->pool, conn);
}

static HtmxResponse *
wanted_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	AllWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card;
	HtmxNav *tabs;
	HtmxUl *list;
	HtmxNode *table;
	HtmxTbody *body;
	guint total = work->total;
	guint rows = 0;
	guint held = work->matching;
	guint i;
	AxisWebListAddress address;

	gboolean upgrades = work->filtered && work->filter == AXIS_TV_UPGRADE_WANTED;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	memset(&address, 0, sizeof(address));
	address.path = "/series/wanted";
	address.state = work->held ? "held" : upgrades ? "upgrade_wanted" : work->filtered ? "attention" : NULL;
	address.after = work->after;
	address.before = work->before;
	ADD(content, heading(1, "Wanted episodes"));
	ADD(content, axis_web_storage_text(NULL, "The aired episodes Axis is looking for, and those that need a decision. "
		"Each is searched for automatically, the ones aired in the last week first, with more time between searches "
		"while nothing is found. An episode already on disk is not listed here; one below its quality profile's cutoff is "
		"under Upgrade wanted."));
	card = axis_web_storage_card(upgrades ? "Episodes wanted for an upgrade" : work->held ?
		"Episodes with a release only you can grab" : work->filtered ? "Episodes that need attention" :
		"All wanted episodes");
	table = axis_web_table_new("series-wanted", wanted_columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(work->episodes, i);
		const AxisTvSeries *s = series_of(work, e->series_id);
		g_autofree gchar *reason = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *number = NULL;
		g_autofree gchar *href = NULL;
		AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;
		AxisTvState state;
		HtmxTr *row;

		if (s == NULL)
			continue;
		state = axis_tv_episode_derive_attention(e, s, &work->rules, &reason, &kind);
		/* The page's episodes were chosen by their state as it was counted
		 * (for state=held: those the page "Needs attention" counts on its
		 * line about the releases that wait for Grab this one, by the same
		 * rule).  One whose state changed between the two statements is left
		 * out: the next view lists it where it is now. */
		if (!wanted_state(state, upgrades) || (work->filtered && state != work->filter) ||
		    (work->held && kind != AXIS_ACQUIRE_ATTENTION_REJECTED))
			continue;
		label = axis_tv_series_label(s->title, s->year);
		number = axis_tv_episode_label(e->season, e->number);
		href = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%d", s->id, e->season);
		row = htmx_tr_new();
		{
			HtmxTd *cell = htmx_td_new();
			HtmxP *line = htmx_p_new();
			g_autofree gchar *text = e->title != NULL ? g_strdup_printf("%s, %s", number, e->title) :
				g_strdup(number);

			htmx_element_add_class(HTMX_ELEMENT(line), "episode-line");
			ADD(line, bdi_text(text));
			ADD(cell, link_text(href, label));
			ADD(cell, line);
			ADD(row, cell);
		}
		ADD(row, cell_text(e->air_date));
		ADD(row, cell_node(axis_tv_web_mark(state)));
		if (state == AXIS_TV_ATTENTION) {
			/* What is wrong, a link to where it is put right. */
			AxisTvAttentionItem item;
			g_autofree gchar *where = NULL;

			memset(&item, 0, sizeof(item));
			item.episode_id = e->id;
			item.season = e->season;
			item.number = e->number;
			item.kind = kind;
			item.search_id = e->search_id;
			item.import_id = e->import_id;
			where = axis_tv_web_attention_href(s->id, &item);
			ADD(row, cell_node(link_text(where, reason)));
		} else {
			ADD(row, cell_text(reason));
		}
		ADD(row, cell_time(e->last_search_us));
		ADD(body, row);
		rows++;
	}
	tabs = htmx_nav_new();
	htmx_element_add_class(HTMX_ELEMENT(tabs), "kind-tabs");
	htmx_element_set_attribute(HTMX_ELEMENT(tabs), "aria-label", "Wanted episodes");
	list = htmx_ul_new();
	ADD(list, tab_item("/series/wanted", "All", total, !work->filtered));
	ADD(list, tab_item("/series/wanted?state=attention", "Needs attention", work->counts[AXIS_TV_ATTENTION],
		work->filtered && work->filter == AXIS_TV_ATTENTION && !work->held));
	/* A part of "Needs attention", reached from the page of that name: its
	 * tab is there only while it is the one open (its number is not known
	 * to the other tabs, which do not read the searches). */
	if (work->held)
		ADD(list, tab_item("/series/wanted?state=held", "Waiting for Grab this one", held, TRUE));
	if (work->counts[AXIS_TV_UPGRADE_WANTED] > 0 || upgrades)
		ADD(list, tab_item("/series/wanted?state=upgrade_wanted", "Upgrade wanted", work->counts[AXIS_TV_UPGRADE_WANTED],
			upgrades));
	ADD(tabs, list);
	ADD(content, tabs);
	ADD(table, body);
	if (rows == 0) {
		/* What is not shown is freed with the page's other pieces. */
		HtmxDiv *unused = htmx_div_new();

		ADD(unused, card);
		ADD(unused, table);
		g_object_unref(unused);
		ADD(content, axis_web_storage_text("no-results", upgrades ? "No episode on disk is wanted for an upgrade." :
			work->held ? "No episode has a release that waits for Grab this one." :
			work->filtered ? "No episode needs attention." : "No episode is wanted."));
	} else {
		ADD(card, table);
		ADD(content, card);
		/* How many there are in all and how many of them this page lists,
		 * with the pages before and after it. */
		ADD(content, axis_web_list_foot_new(&address, work->matching, rows, "episode", "episodes", work->next,
			work->previous));
	}
	return axis_web_page_respond(request, "Wanted episodes", HTMX_NODE(content));
}

static HtmxResponse *
handle_wanted(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AllWork *work = g_new0(AllWork, 1);
	const gchar *state = htmx_request_get_query_param(request, "state");

	/* "held": the episodes that need attention and wait for Grab this one. */
	work->held = g_strcmp0(state, "held") == 0;
	work->filtered = work->held || g_strcmp0(state, "attention") == 0 || g_strcmp0(state, "upgrade_wanted") == 0;
	work->filter = g_strcmp0(state, "upgrade_wanted") == 0 ? AXIS_TV_UPGRADE_WANTED : AXIS_TV_ATTENTION;
	work->after = query_value(request, "after", 24);
	work->before = query_value(request, "before", 24);
	return axis_tv_web_dispatch(user_data, request, wanted_run, wanted_done, work, all_work_free);
}

/* ── History ────────────────────────────────────────────────────── */

typedef struct
{
	gint64     before;
	GPtrArray *events;
	GError    *error;
} HistoryWork;

static void
history_work_free(gpointer data)
{
	HistoryWork *work = data;

	g_clear_pointer(&work->events, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
history_run(
	AxisTv   *self,
	gpointer  data
){
	HistoryWork *work = data;

	axis_tv_list_events(self, 0, work->before, AXIS_TV_WEB_HISTORY_PAGE + 1, &work->events, &work->error);
}

static const AxisWebColumn history_columns[] = {
	{ "When", AXIS_WEB_COL_DATE },
	{ "Series", AXIS_WEB_COL_NAME },
	{ "What", AXIS_WEB_COL_TEXT },
	{ "Details", AXIS_WEB_COL_WIDE },
	{ "By", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxResponse *
history_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	HistoryWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	guint shown;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, heading(1, "TV history"));
	ADD(content, axis_web_storage_text(NULL, "What happened to every series: episodes TMDb added or numbered "
		"anew, searches and what they found, grabs and why each release was chosen, failures, the blocklist, "
		"arrivals, imports, and every administrator's action."));
	shown = MIN(work->events->len, AXIS_TV_WEB_HISTORY_PAGE);
	if (shown == 0) {
		ADD(content, axis_web_storage_text("no-results", work->before > 0 ? "Nothing older." : "No series has a history "
			"yet. Add a series, and its searches, downloads and imports are listed here."));
	} else {
		HtmxNode *card = axis_web_storage_card("Events");
		HtmxNode *table = axis_web_table_new("series-history", history_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < shown; i++) {
			const AxisTvEvent *e = g_ptr_array_index(work->events, i);
			HtmxTr *row = htmx_tr_new();

			ADD(row, cell_time(e->created_us));
			if (e->series_id > 0) {
				g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT, e->series_id);

				ADD(row, cell_node(link_text(href, e->title_label)));
			} else {
				ADD(row, cell_node(bdi_text(e->title_label)));
			}
			ADD(row, cell_text(event_label(e->kind)));
			ADD(row, cell_node(axis_web_breakable(e->text)));
			ADD(row, cell_text(e->by_user != NULL ? e->by_user : "Axis"));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
		if (work->events->len > AXIS_TV_WEB_HISTORY_PAGE) {
			const AxisTvEvent *last = g_ptr_array_index(work->events, shown - 1);
			g_autofree gchar *href = g_strdup_printf("/series/history?before=%" G_GINT64_FORMAT, last->id);
			HtmxDiv *pager = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
			ADD(pager, axis_web_browser_link(href, "Older TV events"));
			ADD(card, pager);
		}
		ADD(content, card);
	}
	return axis_web_page_respond(request, "TV history", HTMX_NODE(content));
}

static HtmxResponse *
handle_history(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HistoryWork *work = g_new0(HistoryWork, 1);
	const gchar *before = htmx_request_get_query_param(request, "before");
	guint64 value = 0;

	if (before != NULL && g_ascii_string_to_unsigned(before, 10, 1, G_MAXINT64, &value, NULL))
		work->before = (gint64)value;
	return axis_tv_web_dispatch(user_data, request, history_run, history_done, work, history_work_free);
}

void
axis_tv_web_register_pages(
	AxisTv  *self,
	AxisWeb *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/series", AXIS_WEB_ROUTE_DEFAULT, handle_grid, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/calendar", AXIS_WEB_ROUTE_DEFAULT, handle_calendar, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/wanted", AXIS_WEB_ROUTE_DEFAULT, handle_wanted, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/history", AXIS_WEB_ROUTE_DEFAULT, handle_history, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/on-disk/:id", AXIS_WEB_ROUTE_DEFAULT, handle_disk, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id", AXIS_WEB_ROUTE_DEFAULT, handle_series, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id/activity", AXIS_WEB_ROUTE_DEFAULT, handle_series_lines,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id/releases", AXIS_WEB_ROUTE_DEFAULT, handle_releases, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id/start-searching", AXIS_WEB_ROUTE_ADMIN, handle_series_start,
		self);
}
