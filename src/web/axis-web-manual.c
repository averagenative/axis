/*
 * axis-web-manual.c - a manual search's pages, for every module that
 * searches (see axis-web-manual.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What a manual search found is listed in four groups (acceptable under the
 * profile, rejected by the profile, not what the search asked for, ruled
 * out: axis_acquire_candidate_group()), each sorted by the page's link, and
 * kept or left out by its filter links; nothing here runs script but htmx's
 * polling of the results card while a search runs.  Everything that came
 * from an indexer (names, indexers' names) is set as text.
 */

#include "web/axis-web-manual.h"

#include <string.h>

#include "quality/axis-quality.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"
#include "web/axis-web-text.h"

#define ADD axis_web_storage_add

/* A search's releases can be fetched while the indexer layer keeps it. */
#define FETCHABLE_US ((gint64)15 * 60 * G_USEC_PER_SEC)

/* The ruled out group is open from the start when it has no more rows. */
#define RULED_OUT_OPEN_MAX 20

/* The most rows of one group shown. */
#define ROWS_SHOWN 200

typedef struct
{
	const AxisWebManualInfo *info;
	gpointer                 data;
} Registered;

void
axis_web_manual_read_clear(AxisWebManualRead *read)
{
	if (read == NULL)
		return;
	g_free(read->title);
	g_free(read->what);
	g_free(read->back);
	g_free(read->automatic);
	g_free(read->state);
	g_free(read->blocklist);
	axis_acquire_search_info_free(read->search);
	g_clear_pointer(&read->candidates, g_ptr_array_unref);
	g_clear_pointer(&read->holds, g_ptr_array_unref);
	memset(read, 0, sizeof(*read));
}

gchar *
axis_web_manual_path(
	const gchar *prefix,
	gint64       owner_id,
	const gchar *level,
	gint64       item
){
	if (item < 0)
		return g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual?level=%s", prefix, owner_id, level);
	return g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual?level=%s&item=%" G_GINT64_FORMAT, prefix, owner_id, level,
		item);
}

HtmxNode *
axis_web_manual_button(
	HtmxRequest *request,
	const gchar *prefix,
	gint64       owner_id,
	const gchar *level,
	gint64       item,
	const gchar *label
){
	g_autofree gchar *action = g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual", prefix, owner_id);
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	ADD(form, axis_web_auth_hidden("level", level));
	if (item >= 0) {
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, item);

		ADD(form, axis_web_auth_hidden("item", text));
	}
	ADD(form, axis_web_auth_button(label, "button-small"));
	return form;
}

/* ── Work off the main loop ─────────────────────────────────────── */

typedef struct _Work Work;

struct _Work
{
	const Registered        *reg;
	HtmxRequest             *request;
	HtmxResponse            *deferred;
	void                   (*run)(Work *work);
	HtmxResponse          *(*done)(Work *work);
	AxisWebManualTarget      target;
	gchar                   *page;		/* the level's page, with the order (where a redirect and a link back go) */
	gchar                   *base;		/* the level's page, as it is without an order */
	const gchar             *dir;		/* static, "asc" or "desc", %NULL: the column's own */
	gchar                   *user;
	gint64                   candidate;
	AxisAcquireHandTicks     ticks;		/* as posted */
	AxisAcquireHandGrabbed   grabbed;	/* what the grab recorded */
	/* A grab by force, as posted: the digest, the ticks. */
	gchar                   *digest;
	gboolean                 checked;
	gboolean                 one_file;	/* one file holds the episodes picked */
	GArray                  *replace;	/* (nullable) gint64: the library files ticked */
	GArray                  *picked;	/* (nullable) gint64: what the person named (the target's) */
	/* The replace of the download in flight, as posted (replace, replace_ok,
	 * replace_others, replace_digest). */
	AxisAcquireReplaceTicks  swap;
	gchar                   *swap_digest;
	gboolean                 changed;	/* what the page showed is not so now: shown again */
	gboolean                 part;		/* the results card alone */
	const gchar             *sort;		/* static, %NULL: by rank */
	gchar                   *indexer;	/* (nullable) the filter */
	const gchar             *protocol;	/* static, (nullable) the filter */
	AxisWebManualRead        read;
	AxisAcquireHandPreview  *preview;
	gboolean                 created;
	AxisWebNotice           *notice;	/* (nullable) the page's, while it is drawn */
	GError                  *error;
};

static void
work_free(Work *work)
{
	g_clear_object(&work->request);
	g_clear_object(&work->deferred);
	g_free(work->page);
	g_free(work->base);
	g_free(work->user);
	g_free(work->indexer);
	g_free(work->digest);
	g_free(work->swap_digest);
	g_clear_pointer(&work->replace, g_array_unref);
	g_clear_pointer(&work->picked, g_array_unref);
	axis_web_manual_read_clear(&work->read);
	axis_acquire_hand_preview_free(work->preview);
	g_clear_error(&work->error);
	g_free(work);
}

static void
work_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Work *work = task_data;

	work->run(work);
	g_task_return_boolean(task, TRUE);
}

static void
work_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Work *work = user_data;
	HtmxResponse *response = work->done(work);

	htmx_response_complete(work->deferred, response);
	if (response != NULL)
		g_object_unref(response);
	work->reg->info->release(work->reg->data);
	work_free(work);
}

static HtmxResponse *
dispatch(Work *work)
{
	HtmxResponse *returned;
	GTask *task;

	/* Never drop ticks silently: what the page showed would not be what is
	 * sent. */
	if ((work->picked != NULL && work->picked->len > AXIS_ACQUIRE_FORCE_PICK_MAX) ||
	    (work->replace != NULL && work->replace->len > AXIS_ACQUIRE_FORCE_PICK_MAX)) {
		g_autofree gchar *text = g_strdup_printf("The form names more than %d items, which is more than Axis takes "
			"at once. Name fewer, or choose a whole season.", AXIS_ACQUIRE_FORCE_PICK_MAX);

		returned = axis_web_error_respond(work->request, 400, "Too many ticks", text);
		work_free(work);
		return returned;
	}
	if (!work->reg->info->hold(work->reg->data)) {
		returned = axis_web_error_respond(work->request, 503, "Busy", "The server is busy. Try again in a few "
			"seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		work_free(work);
		return returned;
	}
	work->deferred = htmx_response_new_deferred();
	returned = g_object_ref(work->deferred);
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
	return returned;
}

/* The module's error as a page. */
static HtmxResponse *
failed(
	Work        *work,
	const gchar *heading
){
	guint status = work->reg->info->status(work->error);
	g_autofree gchar *message = NULL;

	if (status == 0)
		return axis_web_internal_error(work->request, work->error);
	message = g_strdup_printf("%c%s%s", g_ascii_toupper(work->error->message[0]), work->error->message + 1,
		g_str_has_suffix(work->error->message, ".") ? "" : ".");
	return axis_web_error_respond(work->request, status, heading, message);
}

/* ── What the page is for ───────────────────────────────────────── */

static const AxisWebManualLevel *
level_of(
	const AxisWebManualInfo *info,
	const gchar             *name
){
	guint i;

	for (i = 0; name != NULL && info->levels[i].name != NULL; i++) {
		if (strcmp(info->levels[i].name, name) == 0)
			return &info->levels[i];
	}
	return NULL;
}

static gboolean
number_of(
	const gchar *text,
	gint64      *value
){
	guint64 parsed = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 0, G_MAXINT64, &parsed, NULL))
		return FALSE;
	*value = (gint64)parsed;
	return TRUE;
}

/* The numbers of the fields "<prefix><number>" of @fields set to "yes" (a
 * tick box for each of a list's rows), one more than AXIS_ACQUIRE_FORCE_PICK_MAX
 * at most (so that dispatch() can refuse a form that names more). */
static GArray *
ticked_fields(
	GHashTable  *fields,
	const gchar *prefix
){
	GArray *numbers = g_array_new(FALSE, FALSE, sizeof(gint64));
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (fields == NULL)
		return numbers;
	g_hash_table_iter_init(&iter, fields);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		gint64 number = 0;

		if (g_str_has_prefix(key, prefix) && g_strcmp0(value, "yes") == 0 &&
		    number_of((const gchar *)key + strlen(prefix), &number) && number > 0 &&
		    numbers->len <= AXIS_ACQUIRE_FORCE_PICK_MAX)
			g_array_append_val(numbers, number);
	}
	return numbers;
}

static const gchar *sort_of(const gchar *text);
static const gchar *dir_of(const gchar *text);

/* The field a file's replace tick is posted as, and what a person names in a
 * force confirmation's picker. */
#define REPLACE_PREFIX "replace_"
#define PICK_PREFIX "pick_"

/* What the person named for a grab by force, from the picker's query or the
 * confirmation's form: pick_<id>=yes for each item, or whole=<n>.  A post
 * always names (nothing, when nothing was ticked); a query when it says
 * picked=yes. */
static void
read_pick(
	Work       *work,
	GHashTable *fields,
	gboolean    posted
){
	const gchar *whole = fields != NULL ? g_hash_table_lookup(fields, "whole") : NULL;
	const gchar *said = fields != NULL ? g_hash_table_lookup(fields, "picked") : NULL;

	work->picked = ticked_fields(fields, PICK_PREFIX);
	work->target.named = posted || g_strcmp0(said, "yes") == 0;
	work->target.picked = work->picked;
	work->target.whole = whole != NULL && whole[0] != '\0' && number_of(whole, &work->target.part);
}

/* A new piece of work for @request, its target read from the query (GET)
 * or the form (POST); %NULL with *@refused set when the request does not
 * say what it is for. */
static Work *
work_new(
	const Registered  *reg,
	HtmxRequest       *request,
	GHashTable        *params,
	gboolean           posted,
	HtmxResponse     **refused
){
	const gchar *level = posted ? htmx_request_get_form_value(request, "level") :
		htmx_request_get_query_param(request, "level");
	const gchar *item = posted ? htmx_request_get_form_value(request, "item") :
		htmx_request_get_query_param(request, "item");
	g_autofree gchar *none = g_strdup_printf("There is no such %s.", reg->info->noun);
	Work *work;

	*refused = NULL;
	work = g_new0(Work, 1);
	work->reg = reg;
	work->request = g_object_ref(request);
	work->user = g_strdup(axis_web_request_get_username(request));
	work->target.owner_id = axis_web_storage_param_id(params);
	work->target.level = level_of(reg->info, level);
	work->target.item = -1;
	if (work->target.owner_id <= 0) {
		*refused = axis_web_error_respond(request, 404, "Not found", none);
	} else if (work->target.level == NULL ||
	           (work->target.level->item && !number_of(item, &work->target.item))) {
		*refused = axis_web_error_respond(request, 400, "Not shown", "Say what the manual search is of.");
	}
	if (*refused != NULL) {
		work_free(work);
		return NULL;
	}
	work->base = axis_web_manual_path(reg->info->prefix, work->target.owner_id, work->target.level->name,
		work->target.level->item ? work->target.item : -1);
	/* The order the page was in, from a link or from the form that comes back
	 * to it: a fixed list's key and direction, or none. */
	work->sort = sort_of(posted ? htmx_request_get_form_value(request, "sort") :
		htmx_request_get_query_param(request, "sort"));
	work->dir = work->sort != NULL ? dir_of(posted ? htmx_request_get_form_value(request, "dir") :
		htmx_request_get_query_param(request, "dir")) : NULL;
	work->page = work->sort != NULL ? g_strdup_printf("%s&sort=%s%s%s", work->base, work->sort,
		work->dir != NULL ? "&dir=" : "", work->dir != NULL ? work->dir : "") : g_strdup(work->base);
	return work;
}

/* ── Sorting and filtering ──────────────────────────────────────── */

static const struct
{
	const gchar *key;
	const gchar *label;
	gboolean     desc;	/* the order a first choice gives */
} sorts[] = {
	{ NULL, "Rank", FALSE },
	{ "size", "Size", TRUE },
	{ "age", "Age", FALSE },
	{ "quality", "Quality", TRUE },
	{ "indexer", "Indexer", FALSE },
	{ "seeders", "Seeders", TRUE },
	{ "release", "Release", FALSE }
};

/* The key in the fixed list, as the list has it (%NULL: not in it). */
static const gchar *
sort_of(const gchar *text)
{
	guint i;

	for (i = 1; text != NULL && i < G_N_ELEMENTS(sorts); i++) {
		if (strcmp(sorts[i].key, text) == 0)
			return sorts[i].key;
	}
	return NULL;
}

/* The key a column's heading sorts by (%NULL: it does not sort). */
static const gchar *
sort_key_of_heading(const gchar *heading)
{
	return g_strcmp0(heading, "Release") == 0 ? "release" : g_strcmp0(heading, "Quality") == 0 ? "quality" :
		g_strcmp0(heading, "Size") == 0 ? "size" : g_strcmp0(heading, "Age") == 0 ? "age" :
		g_strcmp0(heading, "Found at") == 0 ? "indexer" : NULL;
}

/* "asc" or "desc" when @text is one, else %NULL. */
static const gchar *
dir_of(const gchar *text)
{
	return g_strcmp0(text, "asc") == 0 ? "asc" : g_strcmp0(text, "desc") == 0 ? "desc" : NULL;
}

/* Whether the order of @sort is descending, the page's @dir or else the
 * one its first choice gives.  A column's order is of what it shows: for
 * Age, "asc" is the youngest first. */
static gboolean
sort_desc(
	const gchar *sort,
	const gchar *dir
){
	guint i;

	if (dir != NULL)
		return strcmp(dir, "desc") == 0;
	for (i = 1; sort != NULL && i < G_N_ELEMENTS(sorts); i++) {
		if (strcmp(sorts[i].key, sort) == 0)
			return sorts[i].desc;
	}
	return FALSE;
}

/* The page with this order and these filters (each %NULL: none). */
static gchar *
href_of_dir(
	const Work  *work,
	const gchar *sort,
	const gchar *dir,
	const gchar *indexer,
	const gchar *protocol
){
	GString *href = g_string_new(work->base);

	if (sort != NULL)
		g_string_append_printf(href, "&sort=%s", sort);
	if (sort != NULL && dir != NULL)
		g_string_append_printf(href, "&dir=%s", dir);
	if (indexer != NULL) {
		g_autofree gchar *escaped = g_uri_escape_string(indexer, NULL, TRUE);

		g_string_append_printf(href, "&indexer=%s", escaped);
	}
	if (protocol != NULL)
		g_string_append_printf(href, "&protocol=%s", protocol);
	return g_string_free(href, FALSE);
}

/* The page with this order (the page's own direction while the order is the
 * one it has) and these filters. */
static gchar *
href_of(
	const Work  *work,
	const gchar *sort,
	const gchar *indexer,
	const gchar *protocol
){
	return href_of_dir(work, sort, g_strcmp0(sort, work->sort) == 0 ? work->dir : NULL, indexer, protocol);
}

/* The hidden fields that keep the page's order through a form. */
static void
order_hidden(
	const Work *work,
	HtmxNode   *form
){
	if (work->sort == NULL)
		return;
	ADD(form, axis_web_auth_hidden("sort", work->sort));
	if (work->dir != NULL)
		ADD(form, axis_web_auth_hidden("dir", work->dir));
}

typedef struct
{
	const AxisAcquireCandidate *c;
	const gchar                *holds;
	AxisAcquireGroup            group;
	gint                        quality;	/* the ladder's order, -1 unknown */
	gboolean                    force;		/* ruled out, and offered Grab it by force */
} Row;

typedef struct
{
	const gchar *sort;
	gboolean     desc;
} Order;

/* @x before @y (negative) by a figure, in the direction of the order, a
 * figure not known after every other. */
static gint
compare_figure(
	gboolean x_known,
	gint64   x,
	gboolean y_known,
	gint64   y,
	gboolean desc
){
	gint order;

	if (!x_known || !y_known)
		return x_known == y_known ? 0 : x_known ? -1 : 1;
	order = x == y ? 0 : x < y ? -1 : 1;
	return desc ? -order : order;
}

static gint
compare_rows(
	gconstpointer a,
	gconstpointer b,
	gpointer      data
){
	const Row *x = *(const Row *const *)a;
	const Row *y = *(const Row *const *)b;
	const Order *by = data;
	const gchar *sort = by->sort;
	gint order = 0;

	if (x->group != y->group)
		return x->group < y->group ? -1 : 1;
	if (sort == NULL) {
		/* The ruled out rows that can be grabbed by force first. */
		if (x->force != y->force)
			return x->force ? -1 : 1;
		if (x->c->accepted && y->c->accepted && x->c->rank != y->c->rank)
			order = x->c->rank < y->c->rank ? -1 : 1;
	} else if (strcmp(sort, "size") == 0) {
		order = compare_figure(x->c->size_bytes >= 0, x->c->size_bytes, y->c->size_bytes >= 0, y->c->size_bytes,
			by->desc);
	} else if (strcmp(sort, "age") == 0) {
		/* Younger is the lesser age. */
		order = compare_figure(x->c->published_us > 0, -x->c->published_us, y->c->published_us > 0,
			-y->c->published_us, by->desc);
	} else if (strcmp(sort, "quality") == 0) {
		order = compare_figure(x->quality >= 0, x->quality, y->quality >= 0, y->quality, by->desc);
	} else if (strcmp(sort, "indexer") == 0) {
		if (x->c->indexer == NULL || y->c->indexer == NULL)
			order = x->c->indexer == y->c->indexer ? 0 : x->c->indexer == NULL ? 1 : -1;
		else
			order = by->desc ? -g_utf8_collate(x->c->indexer, y->c->indexer) :
				g_utf8_collate(x->c->indexer, y->c->indexer);
	} else if (strcmp(sort, "release") == 0) {
		order = g_utf8_collate(x->c->name, y->c->name);
		order = by->desc ? -order : order;
	} else if (strcmp(sort, "seeders") == 0) {
		order = compare_figure(x->c->seeders >= 0, x->c->seeders, y->c->seeders >= 0, y->c->seeders, by->desc);
	}
	if (order != 0)
		return order;
	return x->c->position < y->c->position ? -1 : x->c->position > y->c->position;
}

static gboolean
row_kept(
	const Work *work,
	const Row  *row
){
	if (work->indexer != NULL && g_strcmp0(row->c->indexer, work->indexer) != 0)
		return FALSE;
	if (work->protocol != NULL && strcmp(work->protocol, axis_download_protocol_to_string(row->c->protocol)) != 0)
		return FALSE;
	return TRUE;
}

static HtmxNode *
choice_link(
	const gchar *href,
	const gchar *label,
	gboolean     current
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), label);
	if (current) {
		htmx_element_add_class(HTMX_ELEMENT(link), "current");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "true");
	}
	return HTMX_NODE(link);
}

static HtmxNode *
links_group(const gchar *label)
{
	HtmxDiv *group = htmx_div_new();
	HtmxSpan *name = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(group), "list-links");
	htmx_element_set_attribute(HTMX_ELEMENT(group), "role", "group");
	htmx_element_set_attribute(HTMX_ELEMENT(group), "aria-label", label);
	htmx_element_add_class(HTMX_ELEMENT(name), "list-links-label");
	htmx_node_set_text_content(HTMX_NODE(name), label);
	ADD(group, name);
	return HTMX_NODE(group);
}

/* Sort by, then each indexer and each kind found: links that reload the
 * page, the others kept as they are. */
static HtmxNode *
choices_bar(const Work *work)
{
	HtmxDiv *bar = htmx_div_new();
	HtmxNode *order = links_group("Sort");
	g_autoptr(GPtrArray) indexers = g_ptr_array_new();
	gboolean usenet = FALSE;
	gboolean torrent = FALSE;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(bar), "list-choices");
	htmx_element_add_class(HTMX_ELEMENT(bar), "manual-choices");
	for (i = 0; i < G_N_ELEMENTS(sorts); i++) {
		g_autofree gchar *href = href_of(work, sorts[i].key, work->indexer, work->protocol);

		ADD(order, choice_link(href, sorts[i].label, g_strcmp0(work->sort, sorts[i].key) == 0));
	}
	ADD(bar, order);
	for (i = 0; i < work->read.candidates->len; i++) {
		const AxisAcquireCandidate *c = g_ptr_array_index(work->read.candidates, i);
		guint k;
		gboolean seen = FALSE;

		usenet = usenet || c->protocol == AXIS_DOWNLOAD_USENET;
		torrent = torrent || c->protocol == AXIS_DOWNLOAD_TORRENT;
		for (k = 0; c->indexer != NULL && k < indexers->len && !seen; k++)
			seen = strcmp(g_ptr_array_index(indexers, k), c->indexer) == 0;
		if (c->indexer != NULL && !seen)
			g_ptr_array_add(indexers, c->indexer);
	}
	if (indexers->len > 1 || work->indexer != NULL) {
		HtmxNode *from = links_group("Indexer");
		g_autofree gchar *all = href_of(work, work->sort, NULL, work->protocol);

		ADD(from, choice_link(all, "All", work->indexer == NULL));
		for (i = 0; i < indexers->len; i++) {
			const gchar *name = g_ptr_array_index(indexers, i);
			g_autofree gchar *href = href_of(work, work->sort, name, work->protocol);

			ADD(from, choice_link(href, name, g_strcmp0(work->indexer, name) == 0));
		}
		ADD(bar, from);
	}
	if ((usenet && torrent) || work->protocol != NULL) {
		HtmxNode *kind = links_group("Kind");
		g_autofree gchar *any = href_of(work, work->sort, work->indexer, NULL);
		g_autofree gchar *nzb = href_of(work, work->sort, work->indexer, "usenet");
		g_autofree gchar *tor = href_of(work, work->sort, work->indexer, "torrent");

		ADD(kind, choice_link(any, "Any", work->protocol == NULL));
		ADD(kind, choice_link(nzb, "Usenet", g_strcmp0(work->protocol, "usenet") == 0));
		ADD(kind, choice_link(tor, "Torrent", g_strcmp0(work->protocol, "torrent") == 0));
		ADD(bar, kind);
	}
	return HTMX_NODE(bar);
}

/* ── The page ───────────────────────────────────────────────────── */

static const struct
{
	const gchar *heading;
	const gchar *help;
} groups[AXIS_ACQUIRE_N_GROUPS] = {
	{ "Acceptable under the profile", "The profile accepts these, best first by its own order." },
	{ "Rejected by the profile", "The profile rejects these (a quality it does not allow, a size, a rule, or not "
	  "wanted over the file in the library). Grabbed by hand, the profile is set aside." },
	{ "Not what this search asked for", "These are releases of this title that the automatic rules do not take "
	  "here: single episodes on a season search, a pack on an episode search, episodes not wanted now (no air date, "
	  "not monitored, in the library). Grabbed by hand, those rules are set aside." },
	{ "Ruled out", "A fixed rule rules these out (another title, not certainly this one, a sample, no episode or "
	  "film identifiable, an archive, a program), or they are on the blocklist: they failed before, or a person "
	  "blocked them. No search takes them, and they are not grabbed by hand like the others." }
};

/* What the "Ruled out" group adds where the module offers Grab it by force. */
static const gchar ruled_out_force[] = " Where the rule is only about the name, and Axis cannot tell what the release "
	"is, the row says so: an administrator who can tell may grab it by force.";

/* Whether @c is offered "Grab it by force" on this page: the module takes
 * forced grabs, it is not on the blocklist, and every fixed rule that rules
 * it out is about its name (axis_acquire_force_allowed()). */
static gboolean
forceable(
	const Work                 *work,
	const AxisAcquireCandidate *c
){
	return work->reg->info->force_preview != NULL && work->reg->info->force_grab != NULL && !c->blocklisted &&
		axis_acquire_force_allowed(c, NULL);
}

static const AxisWebColumn columns_holds[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "Holds", AXIS_WEB_COL_TEXT },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Age", AXIS_WEB_COL_TEXT },
	{ "Found at", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static const AxisWebColumn columns_plain[] = {
	{ "Release", AXIS_WEB_COL_NAME },
	{ "Quality", AXIS_WEB_COL_TEXT },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Age", AXIS_WEB_COL_TEXT },
	{ "Found at", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

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

static void
add_reason(
	HtmxUl      *list,
	const gchar *css,
	const gchar *text
){
	HtmxLi *item = htmx_li_new();

	htmx_element_add_class(HTMX_ELEMENT(item), css);
	htmx_node_set_text_content(HTMX_NODE(item), text);
	ADD(list, item);
}

/* A GET form to the confirmation (@force: of a grab by force): it changes
 * nothing. */
static HtmxNode *
grab_link(
	const Work                 *work,
	const AxisAcquireCandidate *c,
	gboolean                    force
){
	HtmxForm *form = htmx_form_new();
	HtmxButton *button = htmx_button_new_submit();
	g_autofree gchar *action = g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual/%s", work->reg->info->prefix,
		work->target.owner_id, force ? "force" : "grab");
	g_autofree gchar *candidate = g_strdup_printf("%" G_GINT64_FORMAT, c->id);

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	ADD(form, axis_web_auth_hidden("candidate", candidate));
	ADD(form, axis_web_auth_hidden("level", work->target.level->name));
	if (work->target.level->item) {
		g_autofree gchar *item = g_strdup_printf("%" G_GINT64_FORMAT, work->target.item);

		ADD(form, axis_web_auth_hidden("item", item));
	}
	order_hidden(work, HTMX_NODE(form));
	htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
	if (c->accepted)
		htmx_element_add_class(HTMX_ELEMENT(button), "button-primary");
	htmx_node_set_text_content(HTMX_NODE(button), force ? "Grab it by force" : "Grab this one");
	ADD(form, button);
	return HTMX_NODE(form);
}

/* The id of a release's row: where the page comes back to after Block this
 * release. */
#define ROW_PREFIX "manual-release-"

/* Block this release (add-block-release): one post, back at this row. */
static HtmxNode *
block_form(
	const Work                 *work,
	const AxisAcquireCandidate *c
){
	g_autofree gchar *action = g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual/block", work->reg->info->prefix,
		work->target.owner_id);
	g_autofree gchar *candidate = g_strdup_printf("%" G_GINT64_FORMAT, c->id);
	HtmxNode *form = axis_web_auth_post_form(work->request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	ADD(form, axis_web_auth_hidden("candidate", candidate));
	ADD(form, axis_web_auth_hidden("level", work->target.level->name));
	if (work->target.level->item) {
		g_autofree gchar *item = g_strdup_printf("%" G_GINT64_FORMAT, work->target.item);

		ADD(form, axis_web_auth_hidden("item", item));
	}
	order_hidden(work, form);
	ADD(form, axis_web_auth_button("Block this release", "button-small"));
	return form;
}

/* "On the blocklist (blocked by <person>): take it off the blocklist to grab
 * it.", with the way to the blocklist, where each entry has its Unblock. */
static void
add_blocked_note(
	const Work                 *work,
	HtmxUl                     *list,
	const AxisAcquireCandidate *c
){
	HtmxLi *item = htmx_li_new();
	HtmxSpan *words = htmx_span_new();
	g_autofree gchar *text = c->blocked_by_person ? g_strdup_printf("On the blocklist (blocked by %s): take it off "
		"the blocklist to grab it. ", c->blocked_by != NULL ? c->blocked_by : "an administrator") :
		g_strdup("On the blocklist: take it off the blocklist to grab it. ");

	htmx_element_add_class(HTMX_ELEMENT(item), "reason-note");
	htmx_node_set_text_content(HTMX_NODE(words), text);
	ADD(item, words);
	if (work->read.blocklist != NULL)
		ADD(item, axis_web_browser_link(work->read.blocklist, "See the blocklist"));
	ADD(list, item);
}

static HtmxNode *
release_row(
	const Work *work,
	const Row  *row,
	gboolean    admin
){
	const AxisAcquireCandidate *c = row->c;
	HtmxTr *tr = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxBdi *bdi = htmx_bdi_new();
	HtmxUl *list = htmx_ul_new();
	g_autofree gchar *size = c->size_bytes >= 0 ? axis_quality_format_size(c->size_bytes) : NULL;
	g_autofree gchar *age = age_text(c->published_us);
	g_autofree gchar *where = NULL;
	g_autofree gchar *row_id = g_strdup_printf(ROW_PREFIX "%" G_GINT64_FORMAT, c->id);
	AxisQuality q;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(tr), row_id);
	if (c->accepted)
		htmx_element_add_class(HTMX_ELEMENT(tr), "release-accepted");
	/* What was just done to this row, said in it. */
	if (work->notice != NULL) {
		HtmxNode *said = axis_web_notice_take(work->notice, row_id);

		if (said != NULL)
			ADD(name, said);
	}
	ADD(bdi, axis_web_breakable(c->name));
	ADD(name, bdi);
	htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
	for (i = 0; i < c->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);
		g_autofree gchar *css = g_strdup_printf("reason-%s", r->kind != NULL ? r->kind : "profile");

		add_reason(list, css, r->text);
	}
	if (c->reasons->len == 0 && c->match_text != NULL) {
		g_autofree gchar *text = g_strdup_printf("%c%s.", g_ascii_toupper(c->match_text[0]), c->match_text + 1);

		add_reason(list, "reason-match", text);
	}
	if (row->group == AXIS_ACQUIRE_GROUP_RULED_OUT) {
		g_autofree gchar *by_force = g_strdup_printf("Ruled out by a fixed rule about its name: Axis cannot tell that "
			"it is this %s. An administrator who can tell may grab it by force.", work->reg->info->noun);

		/* On the blocklist: not grabbed by hand or by force until it is taken
		 * off it.  Said also of a release a rule about its name rules out, which
		 * may be grabbed by force once it is off the blocklist. */
		if (c->blocklisted && (!c->safety || axis_acquire_force_allowed(c, NULL)))
			add_blocked_note(work, list, c);
		else
			add_reason(list, "reason-note", forceable(work, c) ? by_force : "Ruled out by a fixed rule: no one can "
				"grab it.");
	}
	ADD(name, list);
	ADD(tr, name);
	if (work->read.holds != NULL)
		ADD(tr, cell_text(row->holds != NULL ? row->holds : ""));
	ADD(tr, cell_text(c->quality != NULL && axis_quality_from_name(c->quality, &q) ? axis_quality_label(q) :
		"Unknown"));
	ADD(tr, cell_text(size != NULL ? size : ""));
	ADD(tr, cell_text(age));
	if (c->protocol == AXIS_DOWNLOAD_TORRENT && c->seeders >= 0)
		where = g_strdup_printf("Torrent, %d %s", c->seeders, c->seeders == 1 ? "seeder" : "seeders");
	else
		where = g_strdup(c->protocol == AXIS_DOWNLOAD_TORRENT ? "Torrent" : "Usenet");
	{
		HtmxTd *cell = htmx_td_new();
		HtmxSpan *indexer = htmx_span_new();
		HtmxSpan *detail = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(indexer), c->indexer != NULL ? c->indexer : "An indexer");
		htmx_element_add_class(HTMX_ELEMENT(detail), "range-detail");
		htmx_node_set_text_content(HTMX_NODE(detail), where);
		ADD(cell, indexer);
		ADD(cell, detail);
		ADD(tr, cell);
	}
	{
		HtmxTd *cell = htmx_td_new();
		gboolean grabbable = row->group != AXIS_ACQUIRE_GROUP_RULED_OUT || forceable(work, c);

		if (admin && grabbable)
			ADD(cell, grab_link(work, c, row->group == AXIS_ACQUIRE_GROUP_RULED_OUT));
		/* What can be grabbed can be blocked, so that no search takes it. */
		if (admin && grabbable && !c->blocklisted && work->reg->info->block != NULL)
			ADD(cell, block_form(work, c));
		ADD(tr, cell);
	}
	return HTMX_NODE(tr);
}

/* The results card: what the level's last manual search found, or that one
 * runs (followed by htmx, with a plain reload link). */
static HtmxNode *
results_card(
	const Work *work,
	gboolean    admin
){
	const AxisWebManualRead *read = &work->read;
	const AxisAcquireSearchInfo *s = read->search;
	HtmxNode *card = axis_web_storage_card("What the manual search found");
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	guint counts[AXIS_ACQUIRE_N_GROUPS] = { 0 };
	guint hidden = 0;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "manual-results");
	if (work->notice != NULL)
		axis_web_notice_put(work->notice, card, "manual-results");
	if (read->running) {
		/* Followed and reloaded as it is sorted and filtered now. */
		g_autofree gchar *here = href_of(work, work->sort, work->indexer, work->protocol);
		g_autofree gchar *part = g_strdup_printf("%s&part=results", here);
		g_autofree gchar *text = g_strdup_printf("A manual search of %s is %s: this card follows it and shows what it "
			"found once it is done.", read->what, read->state != NULL ? read->state : "running");
		HtmxP *line = htmx_p_new();

		htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-get", part);
		htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-trigger", "every 5s");
		htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-swap", "outerHTML");
		ADD(card, axis_web_activity_line("manual-running", AXIS_WEB_ACTIVITY_RUNNING, text));
		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		ADD(line, axis_web_browser_link(here, "Reload this page"));
		ADD(card, line);
	}
	if (s == NULL) {
		if (!read->running) {
			g_autofree gchar *text = g_strdup_printf("No manual search of %s yet.%s", read->what, admin ?
				" Manual search asks every indexer now and lists everything it finds." : "");

			ADD(card, axis_web_storage_text("help", text));
		}
		return card;
	}
	{
		g_autofree gchar *when = axis_web_format_time(s->finished_us > 0 ? s->finished_us : s->started_us, "");
		g_autofree gchar *line = g_strdup_printf("%s %s%s%s.%s%s", read->running ? "The one before: searched" :
			"Searched", when, s->requested_by != NULL ? " by " : "", s->requested_by != NULL ? s->requested_by : "",
			s->how != NULL ? " " : "", s->how != NULL ? s->how : "");

		ADD(card, axis_web_storage_text("film-search-line", line));
		if (s->outcome_text != NULL)
			ADD(card, axis_web_storage_text("film-outcome", s->outcome_text));
		if (s->finished_us > 0 && g_get_real_time() - s->finished_us > FETCHABLE_US && read->candidates->len > 0)
			ADD(card, axis_web_browser_warning("These releases were found more than fifteen minutes ago. An indexer "
				"gives a release only for a while after its search: search again before you grab one."));
	}
	if (read->candidates->len == 0)
		return card;
	for (i = 0; i < read->candidates->len; i++) {
		const AxisAcquireCandidate *c = g_ptr_array_index(read->candidates, i);
		Row *row = g_new0(Row, 1);
		AxisQuality q;

		row->c = c;
		row->holds = read->holds != NULL && i < read->holds->len ? g_ptr_array_index(read->holds, i) : NULL;
		row->group = axis_acquire_candidate_group(c);
		row->quality = c->quality != NULL && axis_quality_from_name(c->quality, &q) ? (gint)q : -1;
		row->force = row->group == AXIS_ACQUIRE_GROUP_RULED_OUT && forceable(work, c);
		if (!row_kept(work, row)) {
			hidden++;
			g_free(row);
			continue;
		}
		counts[row->group]++;
		g_ptr_array_add(rows, row);
	}
	{
		Order by;

		by.sort = work->sort;
		by.desc = sort_desc(work->sort, work->dir);
		g_ptr_array_sort_with_data(rows, compare_rows, &by);
	}
	ADD(card, choices_bar(work));
	if (hidden > 0) {
		g_autofree gchar *count = axis_web_count(hidden, "release", "releases");
		g_autofree gchar *all = href_of(work, work->sort, NULL, NULL);
		g_autofree gchar *text = g_strdup_printf("The filter hides %s. ", count);
		HtmxP *line = htmx_p_new();
		HtmxSpan *span = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(span), text);
		ADD(line, span);
		ADD(line, axis_web_browser_link(all, "Show them all"));
		ADD(card, line);
	}
	{
		guint g;
		guint at = 0;
		const gchar *landed = axis_web_place_of(work->request);

		for (g = 0; g < AXIS_ACQUIRE_N_GROUPS; g++) {
			g_autofree gchar *title = g_strdup_printf("%s (%u)", groups[g].heading, counts[g]);
			g_autofree gchar *id = g_strdup_printf("manual-group-%u", g);
			/* The ruled out can be hundreds: in a disclosure, closed when long, open
			 * when it is short, anchored into, or was just sorted. */
			gboolean folds = g == AXIS_ACQUIRE_GROUP_RULED_OUT && counts[g] > 0;
			HtmxNode *section = folds ? HTMX_NODE(htmx_details_new()) : HTMX_NODE(htmx_div_new());
			g_autoptr(GPtrArray) hrefs = g_ptr_array_new_with_free_func(g_free);
			AxisWebColumnSort heads[8] = { { NULL, NULL } };
			const AxisWebColumn *columns = read->holds != NULL ? columns_holds : columns_plain;
			HtmxNode *table;
			HtmxTbody *body;
			guint shown = 0;
			guint c;

			htmx_element_set_id(HTMX_ELEMENT(section), id);
			htmx_element_add_class(HTMX_ELEMENT(section), "manual-group");
			if (folds) {
				HtmxSummary *summary = htmx_summary_new();

				htmx_node_set_text_content(HTMX_NODE(summary), title);
				htmx_element_add_class(HTMX_ELEMENT(summary), "manual-fold");
				ADD(section, summary);
				/* (Or a row of it: a release that was just blocked is here now.) */
				htmx_details_set_open(HTMX_DETAILS(section), counts[g] <= RULED_OUT_OPEN_MAX ||
					g_strcmp0(landed, id) == 0 || (landed != NULL && g_str_has_prefix(landed, ROW_PREFIX)));
			} else {
				ADD(section, heading(3, title));
			}
			for (c = 0; counts[g] > 0 && columns[c].heading != NULL && c < G_N_ELEMENTS(heads) - 1; c++) {
				const gchar *key = sort_key_of_heading(columns[c].heading);
				gboolean current = key != NULL && g_strcmp0(work->sort, key) == 0;
				gboolean desc = key != NULL && (current ? !sort_desc(key, work->dir) : sort_desc(key, NULL));
				gchar *href;
				gchar *place;

				if (key == NULL)
					continue;
				/* Where the page comes back to: this table, open. */
				href = href_of_dir(work, key, desc ? "desc" : "asc", work->indexer, work->protocol);
				place = g_strdup_printf("%s&at=%s#%s", href, id, id);
				g_free(href);
				g_ptr_array_add(hrefs, place);
				heads[c].href = place;
				heads[c].sorted = current ? (sort_desc(key, work->dir) ? "descending" : "ascending") : NULL;
			}
			if (counts[g] == 0) {
				ADD(section, axis_web_storage_text("help", "None."));
				ADD(card, section);
				continue;
			}
			{
				/* Force is promised only where the action exists. */
				g_autofree gchar *help = g_strconcat(groups[g].help, g == AXIS_ACQUIRE_GROUP_RULED_OUT &&
					work->reg->info->force_preview != NULL && work->reg->info->force_grab != NULL ?
					ruled_out_force : "", NULL);

				ADD(section, axis_web_storage_text("help", help));
			}
			table = axis_web_table_new_sortable("manual-releases", columns, heads);
			body = htmx_tbody_new();
			for (; at < rows->len && ((const Row *)g_ptr_array_index(rows, at))->group == (AxisAcquireGroup)g; at++) {
				if (shown++ < ROWS_SHOWN)
					ADD(body, release_row(work, g_ptr_array_index(rows, at), admin));
			}
			ADD(table, body);
			ADD(section, table);
			if (shown > ROWS_SHOWN) {
				g_autofree gchar *more = axis_web_count(shown - ROWS_SHOWN, "more release", "more releases");
				g_autofree gchar *text = g_strdup_printf("And %s: use a filter to see them.", more);

				ADD(section, axis_web_storage_text("help", text));
			}
			ADD(card, section);
		}
	}
	if (admin)
		ADD(card, axis_web_storage_text("help", "Grab this one shows what it fills and what it replaces, then sends it "
			"as a grab by hand: the quality profile and the automatic rules are set aside, and its import does not "
			"refuse what it was grabbed for for them. A release that would replace a better file, or that numbers its "
			"parts differently from the data, needs a second tick."));
	if (admin && work->reg->info->block != NULL)
		ADD(card, axis_web_storage_text("help", "Block this release puts a release on the blocklist under your name: "
			"no search takes it again, from any indexer when it has the same name, and nothing is downloaded or "
			"deleted. To undo it, take it off the blocklist."));
	if (!admin)
		ADD(card, axis_web_storage_text("help", "An administrator can grab any of these releases but the ruled out "
			"ones by hand."));
	return card;
}

/* Whether the level's last manual search ended a moment ago: asking again
 * makes nothing new until AXIS_ACQUIRE_SEARCH_AGAIN_S has passed. */
static gboolean
ended_lately(const AxisWebManualRead *read)
{
	return read->search != NULL && read->search->finished_us > 0 &&
		g_get_real_time() - read->search->finished_us < (gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC;
}

static gchar *
done_text(
	const gchar             *key,
	const AxisWebManualRead *read
){
	if (g_strcmp0(key, "searching") == 0)
		return g_strdup("A manual search is queued: this page shows what it finds once it has run. It grabs nothing "
			"by itself.");
	if (g_strcmp0(key, "already_searching") == 0) {
		/* Said with what does the work now. */
		if (read->running)
			return g_strdup_printf("Nothing new was asked for: a manual search of %s is %s already. This page shows "
				"what it finds.", read->what, read->state != NULL ? read->state : "queued or running");
		if (ended_lately(read)) {
			g_autofree gchar *again = axis_web_activity_clock(read->search->finished_us +
				(gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC, g_get_real_time());

			return g_strdup_printf("Nothing new was asked for: a manual search of %s ended a moment ago, and what it "
				"found is below. You can ask again from %s.", read->what, again);
		}
		return g_strdup("A manual search of it was queued or running already: this page shows what it finds.");
	}
	if (g_strcmp0(key, "grabbed") == 0)
		return g_strdup("The release was sent to a download client, grabbed by hand with the quality profile set "
			"aside.");
	if (g_strcmp0(key, "blocked") == 0)
		return g_strdup("Blocked: this release is on the blocklist now, and no search takes it, from any indexer. "
			"Nothing was downloaded, removed or deleted. To undo it, take it off the blocklist.");
	return NULL;
}

static void
page_run(Work *work)
{
	work->reg->info->read(work->reg->data, &work->target, &work->read, &work->error);
	if (work->error == NULL && work->read.candidates == NULL)
		work->read.candidates = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_candidate_free);
}

static HtmxResponse *
page_done(Work *work)
{
	gboolean admin = axis_web_request_is_admin(work->request);
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *title = NULL;
	AxisWebNotice notice;
	HtmxResponse *response;

	if (work->error != NULL)
		return failed(work, "Not shown");
	if (work->part) {
		g_autoptr(HtmxNode) card = results_card(work, admin);
		g_autofree gchar *markup = NULL;

		/* This card is not drawn as a page is: the roles of its cells here. */
		axis_web_table_apply_roles(card);
		markup = htmx_element_render(HTMX_ELEMENT(card));

		response = htmx_response_new();
		htmx_response_set_content(response, markup);
		htmx_response_set_content_type(response, "text/html; charset=utf-8");
		return response;
	}
	content = htmx_div_new();
	title = g_strdup_printf("Manual search of %s", work->read.what);
	/* Drawn in the results card when the post came back to it. */
	axis_web_notice_begin(&notice, work->request, FALSE, done_text(htmx_request_get_query_param(work->request,
		"done"), &work->read), HTMX_NODE(content));
	work->notice = &notice;
	ADD(content, heading(1, title));
	{
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "series-back");
		ADD(line, axis_web_browser_link(work->read.back, work->read.title));
		ADD(content, line);
	}
	ADD(content, axis_web_storage_text("help", "A manual search asks every indexer now and lists everything they "
		"answered, grouped by what the quality profile and the automatic rules make of each release. It grabs "
		"nothing by itself, and it does not move or replace the automatic searches."));
	{
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
		/* While one is in hand, or just done, asking again makes nothing:
		 * the button says so. */
		if (admin && work->read.running)
			ADD(actions, axis_web_activity_busy_button("Manual search running", "This page follows it and shows what "
				"it finds."));
		else if (admin && ended_lately(&work->read))
			ADD(actions, axis_web_activity_busy_button("Searched a moment ago", "What it found is below; you can ask "
				"again in a minute."));
		else if (admin) {
			HtmxNode *again = axis_web_manual_button(work->request, work->reg->info->prefix, work->target.owner_id,
				work->target.level->name, work->target.level->item ? work->target.item : -1,
				work->read.search != NULL ? "Manual search again" : "Manual search now");

			order_hidden(work, again);
			ADD(actions, again);
		}
		if (work->read.automatic != NULL)
			ADD(actions, axis_web_browser_link(work->read.automatic, "The last automatic search's releases"));
		ADD(content, actions);
	}
	ADD(content, results_card(work, admin));
	axis_web_notice_end(&notice);
	work->notice = NULL;
	response = axis_web_page_respond(work->request, title, HTMX_NODE(content));
	return response;
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HtmxResponse *refused = NULL;
	Work *work = work_new(user_data, request, params, FALSE, &refused);
	const gchar *indexer;
	const gchar *protocol;

	if (work == NULL)
		return refused;
	work->part = g_strcmp0(htmx_request_get_query_param(request, "part"), "results") == 0;
	indexer = htmx_request_get_query_param(request, "indexer");
	if (indexer != NULL && indexer[0] != '\0' && strlen(indexer) <= 100 && g_utf8_validate(indexer, -1, NULL))
		work->indexer = g_strdup(indexer);
	protocol = htmx_request_get_query_param(request, "protocol");
	work->protocol = g_strcmp0(protocol, "usenet") == 0 ? "usenet" : g_strcmp0(protocol, "torrent") == 0 ?
		"torrent" : NULL;
	work->run = page_run;
	work->done = page_done;
	return dispatch(work);
}

/* ── Asking for one ─────────────────────────────────────────────── */

static void
search_run(Work *work)
{
	work->reg->info->search(work->reg->data, &work->target, work->user, &work->created, &work->error);
}

static HtmxResponse *
search_done(Work *work)
{
	if (work->error != NULL)
		return failed(work, "Not searched");
	axis_web_storage_audit(work->request, "%s %" G_GINT64_FORMAT " manual search of the %s asked for",
		work->reg->info->noun, work->target.owner_id, work->target.level->name);
	return axis_web_place_redirect(work->request, work->page, work->created ? "searching" : "already_searching",
		"manual-results");
}

static HtmxResponse *
handle_search(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HtmxResponse *refused = NULL;
	Work *work = work_new(user_data, request, params, TRUE, &refused);

	if (work == NULL)
		return refused;
	work->run = search_run;
	work->done = search_done;
	return dispatch(work);
}

/* ── Grab this one ──────────────────────────────────────────────── */

/* "A", "A and B", "A, B and C". */
static gchar *
joined(const GPtrArray *items)
{
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; items != NULL && i < items->len; i++) {
		if (i > 0)
			g_string_append(text, i + 1 == items->len ? " and " : ", ");
		g_string_append(text, g_ptr_array_index(items, i));
	}
	return g_string_free(text, FALSE);
}

static const gchar *
file_quality(const AxisAcquireReplaced *r)
{
	if (r->quality != AXIS_QUALITY_UNKNOWN)
		return axis_quality_label(r->quality);
	return axis_quality_unknown_source_label(r->resolution);
}

/* The one sentence: what it fills, what it replaces and how, and that the
 * profile is set aside. */
static gchar *
confirm_sentence(const AxisAcquireHandPreview *p)
{
	const AxisAcquireCandidate *c = p->candidate;
	g_autofree gchar *fills = joined(p->fills);
	g_autoptr(GPtrArray) replaced = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *replaces = NULL;
	AxisQuality q;
	guint i;

	for (i = 0; i < p->replaces->len; i++) {
		const AxisAcquireReplaced *r = g_ptr_array_index(p->replaces, i);

		g_ptr_array_add(replaced, g_strdup_printf("%s (%s: %s)", r->label, file_quality(r), r->better > 0 ?
			"an upgrade" : r->better < 0 ? "a downgrade" : "not an upgrade"));
	}
	replaces = joined(replaced);
	return g_strdup_printf("Grab %s (%s) by hand for %s%s%s, with the quality profile %s set aside.", c->name,
		c->quality != NULL && axis_quality_from_name(c->quality, &q) ? axis_quality_label(q) : "unknown quality",
		fills, replaced->len > 0 ? ", replacing " : "", replaces, p->profile_name != NULL ? p->profile_name : "");
}

/* A second tick: unticked, @name=yes when ticked. */
static HtmxNode *
check_box(
	const gchar *name,
	const gchar *id,
	const gchar *text
){
	HtmxDiv *div = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, "yes");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_node_set_text_content(HTMX_NODE(label), text);
	ADD(div, input);
	ADD(div, label);
	return HTMX_NODE(div);
}

/* ── Replace the download in flight (add-replace-grab-and-delete-download, A) ── */

/* The fields of a confirmation that offers the replace. */
#define SWAP_GRAB "replace"
#define SWAP_OK "replace_ok"
#define SWAP_OTHERS "replace_others"
#define SWAP_DIGEST "replace_digest"

/* What the post says of the replace: nothing unless it names a grab. */
static void
read_swap(
	Work        *work,
	HtmxRequest *request
){
	gint64 grab = 0;

	if (!number_of(htmx_request_get_form_value(request, SWAP_GRAB), &grab) || grab <= 0)
		return;
	work->swap_digest = g_strdup(htmx_request_get_form_value(request, SWAP_DIGEST));
	work->swap.grab_id = grab;
	work->swap.ok = axis_web_storage_form_yes(request, SWAP_OK);
	work->swap.others = axis_web_storage_form_yes(request, SWAP_OTHERS);
	work->swap.digest = work->swap_digest;
}

/* Where what stands in the way is dealt with: its import, or the Downloads
 * page.  Added to @card when the preview names either. */
static void
standing_links(
	const AxisAcquireHandPreview *p,
	HtmxNode                     *card
){
	if (!p->in_flight || (p->standing_import <= 0 && !p->standing_downloads))
		return;
	{
		HtmxP *line = htmx_p_new();

		htmx_element_set_id(HTMX_ELEMENT(line), "replace-links");
		if (p->standing_import > 0) {
			g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT, p->standing_import);

			ADD(line, axis_web_browser_link(href, "Open its import"));
		} else {
			ADD(line, axis_web_browser_link("/downloads", "Open the Downloads page"));
		}
		ADD(card, line);
	}
}

/* The card that says which download is in flight and what replacing it does
 * (%NULL when the preview does not offer it).  It changes nothing: the ticks
 * are in the confirmation's own form (replace_form()). */
static HtmxNode *
replace_card(const AxisAcquireHandPreview *p)
{
	const AxisAcquireStanding *s;
	g_autofree gchar *size = NULL;
	g_autofree gchar *far = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *what = NULL;
	HtmxNode *card;

	if (!p->replaceable || p->standing == NULL || p->standing->len != 1)
		return NULL;
	s = g_ptr_array_index(p->standing, 0);
	card = axis_web_storage_card("A download is in flight");
	htmx_element_set_id(HTMX_ELEMENT(card), "replace-offer");
	size = s->size_bytes > 0 ? axis_quality_format_size(s->size_bytes) : NULL;
	if (s->size_bytes > 0 && s->done_bytes >= 0)
		far = g_strdup_printf("%d %% of %s", (gint)CLAMP(s->done_bytes * 100 / s->size_bytes, 0, 100), size);
	else
		far = g_strdup(g_strcmp0(s->state, "queued") == 0 ? "queued, nothing downloaded yet" :
			g_strcmp0(s->state, "paused") == 0 ? "paused" : "how far it is, is not known");
	now = g_strdup_printf("Downloading now: %s, at %s, %s.%s%s%s", s->release != NULL ? s->release : "a release",
		s->client != NULL ? s->client : "a download client", far, s->holds != NULL ? " It holds " : "",
		s->holds != NULL ? s->holds : "", s->holds != NULL ? "." : "");
	ADD(card, axis_web_storage_text("manual-sentence", now));
	what = g_strdup_printf("Nothing else is grabbed while it downloads. To take this release instead, tick the box "
		"below: Axis then removes %s from its client with its data, and sends this release. The release replaced is "
		"not blocked, so a later search may take it again. Axis asks the client where the download is first: if the "
		"client does not answer, or the download is somewhere Axis does not let a client delete, nothing is ended and "
		"nothing is sent. If the client then fails to remove it, the download is ended, nothing is sent, and Axis "
		"keeps asking the client.", s->release != NULL ? s->release : "it");
	ADD(card, axis_web_storage_text("help", what));
	if (s->others != NULL) {
		g_autofree gchar *others = g_strdup_printf("The download in flight is ended whole. It also holds %s, which "
			"this release does not: they stop downloading too and go back to ordinary searching.", s->others);

		ADD(card, axis_web_browser_warning(others));
	}
	return card;
}

/* The replace's part of the confirmation's form: the grab, the digest of
 * what the card shows, and its ticks. */
static void
replace_form(
	const AxisAcquireHandPreview *p,
	HtmxNode                     *form
){
	const AxisAcquireStanding *s;
	g_autofree gchar *grab = NULL;

	if (!p->replaceable || p->standing == NULL || p->standing->len != 1)
		return;
	s = g_ptr_array_index(p->standing, 0);
	grab = g_strdup_printf("%" G_GINT64_FORMAT, s->grab_id);
	ADD(form, axis_web_auth_hidden(SWAP_GRAB, grab));
	ADD(form, axis_web_auth_hidden(SWAP_DIGEST, p->replace_digest != NULL ? p->replace_digest : ""));
	ADD(form, check_box(SWAP_OK, "replace-ok", "Replace the download in flight with this one"));
	if (s->others != NULL) {
		g_autofree gchar *text = g_strdup_printf("%s stop downloading too", s->others);

		ADD(form, check_box(SWAP_OTHERS, "replace-others", text));
	}
}

static void
confirm_run(Work *work)
{
	work->reg->info->preview(work->reg->data, &work->target, work->candidate, &work->preview, &work->error);
}

/* @notice: (nullable) why the page is shown again. */
static HtmxResponse *
confirm_page(
	Work        *work,
	const gchar *notice
){
	const AxisAcquireHandPreview *p = work->preview;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *candidate = NULL;
	HtmxNode *card;
	HtmxNode *form;

	if (work->error != NULL)
		return failed(work, "Not shown");
	content = htmx_div_new();
	ADD(content, heading(1, "Grab this release by hand?"));
	{
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "series-back");
		ADD(line, axis_web_browser_link(work->page, "Back to the manual search"));
		ADD(content, line);
	}
	if (notice != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", notice));
	card = axis_web_storage_card("What it does");
	htmx_element_set_id(HTMX_ELEMENT(card), "manual-confirm");
	if (p->refused != NULL) {
		g_autofree gchar *name = g_strdup_printf("%s cannot be grabbed.", p->candidate->name);

		ADD(card, axis_web_storage_text("film-outcome", name));
		ADD(card, axis_web_storage_text("help", p->refused));
		/* A grab in the way that cannot be replaced: where it is dealt with. */
		standing_links(p, card);
		ADD(content, card);
		return axis_web_page_respond(work->request, "Grab by hand", HTMX_NODE(content));
	}
	/* One download in flight that can be replaced: which, and what that does. */
	{
		HtmxNode *offer = replace_card(p);

		if (offer != NULL)
			ADD(content, offer);
	}
	{
		g_autofree gchar *sentence = confirm_sentence(p);

		ADD(card, axis_web_storage_text("manual-sentence", sentence));
	}
	/* Numbered otherwise than the data: the module's warning, and what its
	 * import does then. */
	if (p->numbering != NULL) {
		ADD(card, axis_web_browser_warning(p->numbering));
	} else if (p->missing != NULL) {
		g_autofree gchar *text = g_strdup_printf("It also names %s, which the data of this %s does not have.",
			p->missing, work->reg->info->noun);

		ADD(card, axis_web_storage_text("help", text));
	}
	action = g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual/grab", work->reg->info->prefix, work->target.owner_id);
	candidate = g_strdup_printf("%" G_GINT64_FORMAT, work->candidate);
	form = axis_web_auth_post_form(work->request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "manual-confirm-form");
	ADD(form, axis_web_auth_hidden("candidate", candidate));
	ADD(form, axis_web_auth_hidden("level", work->target.level->name));
	if (work->target.level->item) {
		g_autofree gchar *item = g_strdup_printf("%" G_GINT64_FORMAT, work->target.item);

		ADD(form, axis_web_auth_hidden("item", item));
	}
	order_hidden(work, form);
	ADD(form, axis_web_auth_hidden("confirm", "yes"));
	/* Replacing a file it is not better than: a tick of its own. */
	if (p->needs_tick) {
		g_autoptr(GPtrArray) worse = g_ptr_array_new_with_free_func(g_free);
		g_autofree gchar *which = NULL;
		g_autofree gchar *text = NULL;
		guint i;

		for (i = 0; i < p->replaces->len; i++) {
			const AxisAcquireReplaced *r = g_ptr_array_index(p->replaces, i);

			if (r->better <= 0)
				g_ptr_array_add(worse, g_strdup(r->label));
		}
		which = joined(worse);
		text = g_strdup_printf("Replace %s, which this release is not better than", which);
		ADD(form, check_box("downgrade", "manual-downgrade", text));
	}
	/* A file kept by hand: replaced only with the tick that names it, though
	 * this release be better. */
	{
		guint i;

		for (i = 0; i < p->replaces->len; i++) {
			const AxisAcquireReplaced *r = g_ptr_array_index(p->replaces, i);
			g_autofree gchar *name = g_strdup_printf(REPLACE_PREFIX "%" G_GINT64_FORMAT, r->file_id);
			g_autofree gchar *id = g_strdup_printf("manual-replace-%" G_GINT64_FORMAT, r->file_id);
			g_autofree gchar *text = NULL;
			g_autofree gchar *kept_words = NULL;

			if (r->kept_by == NULL)
				continue;
			kept_words = axis_acquire_replaced_kept_words(r);
			text = g_strdup_printf("Replace %s (%s), which is %s", r->label, file_quality(r), kept_words);
			ADD(form, check_box(name, id, text));
		}
	}
	/* Numbered otherwise than the data: a tick of its own too. */
	if (p->numbering != NULL)
		ADD(form, check_box("numbering", "manual-numbering", "Take it all the same, numbered differently from the "
			"data"));
	replace_form(p, form);
	ADD(form, axis_web_auth_button("Grab this one by hand", "button-primary button-small"));
	ADD(card, form);
	ADD(content, card);
	return axis_web_page_respond(work->request, "Grab by hand", HTMX_NODE(content));
}

static HtmxResponse *
confirm_done(Work *work)
{
	return confirm_page(work, NULL);
}

static HtmxResponse *
handle_confirm(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HtmxResponse *refused = NULL;
	Work *work = work_new(user_data, request, params, FALSE, &refused);

	if (work == NULL)
		return refused;
	if (!number_of(htmx_request_get_query_param(request, "candidate"), &work->candidate) || work->candidate <= 0) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not shown", "Choose a release.");
	}
	work->run = confirm_run;
	work->done = confirm_done;
	return dispatch(work);
}

/* Why a confirmation that offered a replace is shown again. */
static const gchar swap_changed[] = "The download in flight changed after you opened this page (it arrived, another "
	"grab took its place, or it was dealt with). Nothing was ended and nothing was sent. Check the page again below.";

static void
grab_run(Work *work)
{
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;

	if (work->reg->info->grab(work->reg->data, &work->target, work->candidate, &work->ticks, work->user,
	        &work->grabbed, &result, &work->error) || result != AXIS_ACQUIRE_GRAB_CHANGED || work->swap.grab_id <= 0)
		return;
	/* A replace was asked for and what stands in the way is not what the page
	 * showed: nothing was written, and the page is shown again as it is now. */
	g_clear_error(&work->error);
	work->changed = TRUE;
	work->reg->info->preview(work->reg->data, &work->target, work->candidate, &work->preview, &work->error);
}

/* ", in place of grab N, whose download was removed from its client". */
static gchar *
swap_audit(const Work *work)
{
	if (work->grabbed.replaced_grab <= 0)
		return g_strdup("");
	return g_strdup_printf(", in place of the download in flight (grab %" G_GINT64_FORMAT "), which was ended and "
		"removed from its client with its data", work->grabbed.replaced_grab);
}

static HtmxResponse *
grab_done(Work *work)
{
	g_autofree gchar *replaced = NULL;
	g_autofree gchar *swapped = NULL;

	if (work->error != NULL)
		return failed(work, "Not grabbed");
	if (work->changed) {
		HtmxResponse *response = confirm_page(work, swap_changed);

		htmx_response_set_status(response, 409);
		return response;
	}
	swapped = swap_audit(work);
	/* What the grab recorded, not what was posted. */
	if (work->grabbed.n_replaced > 0)
		replaced = g_strdup_printf(", replacing %s it is not better than as ticked", work->grabbed.n_replaced == 1 ?
			"a file" : "files");
	axis_web_storage_audit(work->request, "%s %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT " grabbed by hand from "
		"a manual search, the quality profile set aside%s%s%s", work->reg->info->noun, work->target.owner_id,
		work->candidate, replaced != NULL ? replaced : "", work->grabbed.numbering ?
		", numbered differently from the data as ticked" : "", swapped);
	/* Back at the list it was taken from. */
	return axis_web_place_redirect(work->request, work->page, "grabbed", "manual-results");
}

static HtmxResponse *
handle_grab(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HtmxResponse *refused = NULL;
	Work *work = work_new(user_data, request, params, TRUE, &refused);

	if (work == NULL)
		return refused;
	if (!number_of(htmx_request_get_form_value(request, "candidate"), &work->candidate) || work->candidate <= 0) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not grabbed", "Choose a release.");
	}
	if (!axis_web_storage_form_yes(request, "confirm")) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not grabbed", "Confirm the grab on its page first.");
	}
	work->ticks.replace_better = axis_web_storage_form_yes(request, "downgrade");
	work->ticks.numbering = axis_web_storage_form_yes(request, "numbering");
	/* The files kept by hand ticked by name: replace_<library file id>=yes. */
	work->replace = ticked_fields(htmx_request_get_form_data(request), REPLACE_PREFIX);
	work->ticks.replace = work->replace;
	/* The replace of the download in flight, when the page offered it. */
	read_swap(work, request);
	work->ticks.in_flight = work->swap.grab_id > 0 ? &work->swap : NULL;
	work->run = grab_run;
	work->done = grab_done;
	return dispatch(work);
}

/* ── Block this release (add-block-release) ─────────────────────── */

static void
block_run(Work *work)
{
	work->reg->info->block(work->reg->data, &work->target, work->candidate, work->user, &work->error);
}

static HtmxResponse *
block_done(Work *work)
{
	g_autofree gchar *row_id = NULL;

	if (work->error != NULL)
		return failed(work, "Not blocked");
	axis_web_storage_audit(work->request, "%s %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT " put on the blocklist "
		"from a manual search", work->reg->info->noun, work->target.owner_id, work->candidate);
	/* Back at its row, which says who blocked it. */
	row_id = g_strdup_printf(ROW_PREFIX "%" G_GINT64_FORMAT, work->candidate);
	return axis_web_place_redirect(work->request, work->page, "blocked", row_id);
}

static HtmxResponse *
handle_block(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const Registered *reg = user_data;
	HtmxResponse *refused = NULL;
	Work *work;

	if (reg->info->block == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is no such page.");
	work = work_new(reg, request, params, TRUE, &refused);
	if (work == NULL)
		return refused;
	if (!number_of(htmx_request_get_form_value(request, "candidate"), &work->candidate) || work->candidate <= 0) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not blocked", "Choose a release.");
	}
	work->run = block_run;
	work->done = block_done;
	return dispatch(work);
}

/* ── Grab it by force (add-force-grab-and-assign) ───────────────── */

static HtmxNode *
paragraph(
	const gchar *css,
	const gchar *text
){
	return axis_web_storage_text(css, text);
}

/* The hidden fields that say what a force page is of. */
static void
force_hidden(
	const Work *work,
	HtmxNode   *form
){
	g_autofree gchar *candidate = g_strdup_printf("%" G_GINT64_FORMAT, work->candidate);

	ADD(form, axis_web_auth_hidden("candidate", candidate));
	ADD(form, axis_web_auth_hidden("level", work->target.level->name));
	if (work->target.level->item) {
		g_autofree gchar *item = g_strdup_printf("%" G_GINT64_FORMAT, work->target.item);

		ADD(form, axis_web_auth_hidden("item", item));
	}
	order_hidden(work, form);
}

/*
 * The picker of a force confirmation, for an owner with parts (a series):
 * each item with a tick box (ticked when the grab would be bound to it as
 * the page stands), and the whole parts in a list.  A GET form that shows the
 * confirmation again for what is named: it changes nothing, so the files
 * listed and the digest are always of what the person sees picked.
 */
static HtmxNode *
pick_form(
	const Work                   *work,
	const AxisAcquireHandPreview *p
){
	HtmxForm *form = htmx_form_new();
	HtmxSelect *select = htmx_select_new_with_name("whole");
	HtmxLabel *label = htmx_label_new_for("force-whole");
	HtmxButton *button = htmx_button_new_submit();
	HtmxDiv *field = htmx_div_new();
	HtmxDiv *list = htmx_div_new();
	g_autofree gchar *action = g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual/force", work->reg->info->prefix,
		work->target.owner_id);
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_id(HTMX_ELEMENT(form), "force-pick");
	htmx_element_add_class(HTMX_ELEMENT(form), "manual-confirm-form");
	force_hidden(work, HTMX_NODE(form));
	ADD(form, axis_web_auth_hidden("picked", "yes"));
	htmx_element_add_class(HTMX_ELEMENT(list), "force-pick-list");
	ADD(select, htmx_option_new_with_value("", "Not a whole season: the episodes ticked"));
	for (i = 0; i < p->choices->len; i++) {
		const AxisAcquireChoice *choice = g_ptr_array_index(p->choices, i);
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, choice->id);

		if (choice->whole) {
			HtmxOption *option = htmx_option_new_with_value(value, choice->label);

			htmx_option_set_selected(option, choice->picked);
			ADD(select, option);
		} else {
			g_autofree gchar *name = g_strdup_printf(PICK_PREFIX "%" G_GINT64_FORMAT, choice->id);
			g_autofree gchar *id = g_strdup_printf("force-pick-%" G_GINT64_FORMAT, choice->id);
			HtmxDiv *row = htmx_div_new();
			HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
			HtmxLabel *text = htmx_label_new_for(id);

			htmx_element_add_class(HTMX_ELEMENT(row), "field-check");
			htmx_input_set_name(input, name);
			htmx_input_set_value(input, "yes");
			htmx_element_set_id(HTMX_ELEMENT(input), id);
			if (choice->picked)
				htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
			htmx_node_set_text_content(HTMX_NODE(text), choice->label);
			ADD(row, input);
			ADD(row, text);
			ADD(list, row);
		}
	}
	ADD(form, list);
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_element_set_id(HTMX_ELEMENT(select), "force-whole");
	htmx_node_set_text_content(HTMX_NODE(label), "Or a whole season");
	ADD(field, label);
	ADD(field, select);
	ADD(form, field);
	htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
	htmx_node_set_text_content(HTMX_NODE(button), "Use what I named");
	ADD(form, button);
	return HTMX_NODE(form);
}

/* The card that says what the grab is bound to, with the picker when the
 * owner has parts. */
static HtmxNode *
bound_card(
	const Work                   *work,
	const AxisAcquireHandPreview *p
){
	HtmxNode *card = axis_web_storage_card("What it is grabbed for");
	const gchar *label = p->owner_label != NULL ? p->owner_label : "this title";

	htmx_element_set_id(HTMX_ELEMENT(card), "force-bound");
	if (p->choices == NULL) {
		g_autofree gchar *text = g_strdup_printf("It is grabbed for the %s %s, and for nothing else.",
			work->reg->info->noun, label);

		ADD(card, paragraph("manual-sentence", text));
		return card;
	}
	if (p->unnamed) {
		ADD(card, paragraph("manual-sentence", p->pick_help != NULL ? p->pick_help : "Say what this release is."));
		if (p->pick_link != NULL) {
			HtmxP *line = htmx_p_new();

			ADD(line, axis_web_browser_link(p->pick_link, p->pick_link_text != NULL ? p->pick_link_text : p->pick_link));
			ADD(card, line);
		}
	} else if (p->bound_text != NULL) {
		g_autofree gchar *text = g_strdup_printf("It is grabbed for %s, and for nothing else.", p->bound_text);

		ADD(card, paragraph("manual-sentence", text));
	}
	ADD(card, paragraph("help", "Axis cannot read from the name what this release is, so you say it: tick the "
		"episodes it holds (two for a double episode), or choose a whole season for a pack. Then select Use what I "
		"named: the page shows what that would replace, and nothing is grabbed until you confirm below."));
	ADD(card, pick_form(work, p));
	return card;
}

/*
 * The confirmation of a grab by force, in four parts: the rule being set
 * aside, what the grab is bound to, what its import does, and what it would
 * replace (each file with a tick of its own).  @notice: (nullable) why it is
 * shown again.
 */
static HtmxResponse *
force_page(
	Work        *work,
	const gchar *notice
){
	const AxisAcquireHandPreview *p = work->preview;
	const AxisAcquireCandidate *c = p->candidate;
	const gchar *noun = work->reg->info->noun;
	const gchar *label = p->owner_label != NULL ? p->owner_label : "this title";
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *action = NULL;
	HtmxNode *card;
	HtmxNode *form;
	AxisQuality q;
	guint i;

	ADD(content, heading(1, "Grab this release by force?"));
	{
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "series-back");
		ADD(line, axis_web_browser_link(work->page, "Back to the manual search"));
		ADD(content, line);
	}
	if (notice != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", notice));
	if (p->refused != NULL) {
		g_autofree gchar *name = g_strdup_printf("%s cannot be grabbed by force.", c->name);

		card = axis_web_storage_card("Why not");
		htmx_element_set_id(HTMX_ELEMENT(card), "force-refused");
		ADD(card, paragraph("film-outcome", name));
		ADD(card, paragraph("help", p->refused));
		/* A grab in the way that cannot be replaced: where it is dealt with. */
		standing_links(p, card);
		ADD(content, card);
		/* What was named can be changed (an episode in flight left out). */
		if (p->choices != NULL && p->choices->len > 0)
			ADD(content, bound_card(work, p));
		return axis_web_page_respond(work->request, "Grab by force", HTMX_NODE(content));
	}
	if (p->bound_text != NULL)
		label = p->bound_text;

	/* 1. The rule set aside, in its stored words. */
	card = axis_web_storage_card("The rule you are setting aside");
	htmx_element_set_id(HTMX_ELEMENT(card), "force-rules");
	{
		g_autofree gchar *size = c->size_bytes >= 0 ? axis_quality_format_size(c->size_bytes) : NULL;
		g_autofree gchar *what = g_strdup_printf("%s (%s%s%s%s%s)", c->name,
			c->quality != NULL && axis_quality_from_name(c->quality, &q) ? axis_quality_label(q) : "unknown quality",
			size != NULL ? ", " : "", size != NULL ? size : "", c->indexer != NULL ? ", from " : "",
			c->indexer != NULL ? c->indexer : "");

		ADD(card, paragraph("manual-sentence", what));
	}
	for (i = 0; c->reasons != NULL && i < c->reasons->len; i++) {
		const AxisAcquireReason *r = g_ptr_array_index(c->reasons, i);
		g_autofree gchar *text = NULL;

		if (g_strcmp0(r->kind, "safety") != 0)
			continue;
		text = g_strdup_printf("Axis rules this release out: %s%s", r->text != NULL ? r->text : "",
			r->text != NULL && g_str_has_suffix(r->text, ".") ? "" : ".");
		ADD(card, paragraph("force-rule", text));
	}
	ADD(card, paragraph("help", "Axis cannot tell from the name that this release is what you are looking for, so no "
		"search takes it. You are setting that aside: only do so when you have checked the release yourself."));
	ADD(content, card);

	/* 2. What it is bound to: the owner, or what the person names of it. */
	ADD(content, bound_card(work, p));
	/* Nothing named yet: nothing to confirm. */
	if (p->unnamed)
		return axis_web_page_respond(work->request, "Grab by force", HTMX_NODE(content));

	/* 3. What its import does. */
	card = axis_web_storage_card("What happens when it has downloaded");
	htmx_element_set_id(HTMX_ELEMENT(card), "force-import");
	if (p->import_text != NULL) {
		ADD(card, paragraph("help", p->import_text));
	} else {
		g_autofree gchar *text = g_strdup_printf("Its video file is imported as %s without judging its name again. The "
			"file must still be a video that can be read, and its runtime is compared with the %s's: an import that "
			"fails those checks is blocked for you to decide. The quality profile %s is set aside. The file is then "
			"kept by hand: no automatic search replaces it until you let it go on the %s's page.", label, noun,
			p->profile_name != NULL ? p->profile_name : "", noun);

		ADD(card, paragraph("help", text));
	}
	ADD(content, card);

	/* One download in flight that can be replaced: which, and what that does. */
	{
		HtmxNode *offer = replace_card(p);

		if (offer != NULL)
			ADD(content, offer);
	}

	/* 4. What it replaces, and the form. */
	card = axis_web_storage_card("What it replaces");
	htmx_element_set_id(HTMX_ELEMENT(card), "force-confirm");
	action = g_strdup_printf("%s/%" G_GINT64_FORMAT "/manual/force", work->reg->info->prefix, work->target.owner_id);
	form = axis_web_auth_post_form(work->request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "manual-confirm-form");
	force_hidden(work, form);
	ADD(form, axis_web_auth_hidden("confirm", "yes"));
	ADD(form, axis_web_auth_hidden("digest", p->digest != NULL ? p->digest : ""));
	/* What is picked as the page stands: the post binds the grab to it. */
	for (i = 0; p->choices != NULL && i < p->choices->len; i++) {
		const AxisAcquireChoice *choice = g_ptr_array_index(p->choices, i);
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, choice->id);
		g_autofree gchar *name = g_strdup_printf(PICK_PREFIX "%" G_GINT64_FORMAT, choice->id);

		if (!choice->picked)
			continue;
		if (choice->whole)
			ADD(form, axis_web_auth_hidden("whole", value));
		else
			ADD(form, axis_web_auth_hidden(name, "yes"));
	}
	if (p->replaces->len == 0) {
		g_autofree gchar *text = p->choices != NULL ? g_strdup("What you named has no file in the library: nothing "
			"is replaced.") : g_strdup_printf("The %s has no file in the library: nothing is replaced.", noun);

		ADD(card, paragraph("help", text));
	} else {
		ADD(card, paragraph("help", "Each file below is replaced only when you tick it. A replaced file goes to the "
			"recycle folder for the retention time, as with any upgrade."));
	}
	for (i = 0; i < p->replaces->len; i++) {
		const AxisAcquireReplaced *r = g_ptr_array_index(p->replaces, i);
		g_autofree gchar *name = g_strdup_printf(REPLACE_PREFIX "%" G_GINT64_FORMAT, r->file_id);
		g_autofree gchar *id = g_strdup_printf("force-replace-%" G_GINT64_FORMAT, r->file_id);
		g_autofree gchar *kept_words = r->kept_by != NULL ? axis_acquire_replaced_kept_words(r) : NULL;
		g_autofree gchar *text = g_strdup_printf("Replace %s (%s) with this release: %s%s%s", r->label, file_quality(r),
			r->better > 0 ? "this release is better" : r->better < 0 ? "this release is worse, or not known to be better" :
			"the same quality", kept_words != NULL ? "; the file is " : "", kept_words != NULL ? kept_words : "");

		ADD(form, check_box(name, id, text));
	}
	/* Several episodes picked one by one: a file holds them all only when the
	 * person says so (or its own name states the range). */
	{
		guint episodes = 0;

		for (i = 0; p->choices != NULL && i < p->choices->len; i++) {
			const AxisAcquireChoice *choice = g_ptr_array_index(p->choices, i);

			if (choice->picked && !choice->whole)
				episodes++;
		}
		if (episodes > 1)
			ADD(form, check_box("one_file", "force-one-file", "One video file of the download holds all of these "
				"episodes (a double episode). Leave this off for a download of several files: each is then placed by "
				"the episode its own name states."));
	}
	{
		g_autofree gchar *text = g_strdup_printf("I have checked that this release is %s", label);

		ADD(form, check_box("checked", "force-checked", text));
	}
	replace_form(p, form);
	ADD(form, axis_web_auth_button("Grab it by force", "button-primary button-small"));
	ADD(card, form);
	ADD(content, card);
	return axis_web_page_respond(work->request, "Grab by force", HTMX_NODE(content));
}

static void
force_confirm_run(Work *work)
{
	work->reg->info->force_preview(work->reg->data, &work->target, work->candidate, &work->preview, &work->error);
}

static HtmxResponse *
force_confirm_done(Work *work)
{
	if (work->error != NULL)
		return failed(work, "Not shown");
	return force_page(work, NULL);
}

static HtmxResponse *
handle_force_confirm(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const Registered *reg = user_data;
	HtmxResponse *refused = NULL;
	Work *work;

	if (reg->info->force_preview == NULL || reg->info->force_grab == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is no such page.");
	work = work_new(reg, request, params, FALSE, &refused);
	if (work == NULL)
		return refused;
	if (!number_of(htmx_request_get_query_param(request, "candidate"), &work->candidate) || work->candidate <= 0) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not shown", "Choose a release.");
	}
	/* What the person named in the picker, if they have. */
	read_pick(work, htmx_request_get_query_params(request), FALSE);
	work->run = force_confirm_run;
	work->done = force_confirm_done;
	return dispatch(work);
}

static void
force_run(Work *work)
{
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireForceTicks ticks;

	memset(&ticks, 0, sizeof(ticks));
	ticks.checked = work->checked;
	ticks.replace = work->replace;
	ticks.digest = work->digest;
	ticks.one_file = work->one_file;
	ticks.in_flight = work->swap.grab_id > 0 ? &work->swap : NULL;
	if (work->reg->info->force_grab(work->reg->data, &work->target, work->candidate, &ticks, work->user,
	        &work->grabbed, &result, &work->error) || result != AXIS_ACQUIRE_GRAB_CHANGED)
		return;
	/* What the page showed is not so now: nothing was sent, and the page is
	 * shown again as it is now. */
	g_clear_error(&work->error);
	work->changed = TRUE;
	work->reg->info->force_preview(work->reg->data, &work->target, work->candidate, &work->preview, &work->error);
}

static HtmxResponse *
force_done(Work *work)
{
	if (work->error != NULL)
		return failed(work, "Not grabbed");
	if (work->changed) {
		HtmxResponse *response = force_page(work, "What this page showed changed after you opened it (a file "
			"arrived, another grab was made, or the release was judged again). Nothing was sent. Check it again "
			"below, then confirm.");

		htmx_response_set_status(response, 409);
		return response;
	}
	/* What the grab recorded, not what was posted. */
	{
		g_autofree gchar *swapped = swap_audit(work);

		axis_web_storage_audit(work->request, "%s %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT " grabbed by force "
			"from a manual search, %u fixed rule%s about its name set aside, bound to it, the quality profile set "
			"aside, replacing %u file%s as ticked%s", work->reg->info->noun, work->target.owner_id, work->candidate,
			work->grabbed.n_forced, work->grabbed.n_forced == 1 ? "" : "s", work->grabbed.n_replaced,
			work->grabbed.n_replaced == 1 ? "" : "s", swapped);
	}
	return axis_web_place_redirect(work->request, work->page, "grabbed", "manual-results");
}

static HtmxResponse *
handle_force(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const Registered *reg = user_data;
	HtmxResponse *refused = NULL;
	Work *work;

	if (reg->info->force_preview == NULL || reg->info->force_grab == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is no such page.");
	work = work_new(reg, request, params, TRUE, &refused);
	if (work == NULL)
		return refused;
	if (!number_of(htmx_request_get_form_value(request, "candidate"), &work->candidate) || work->candidate <= 0) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not grabbed", "Choose a release.");
	}
	if (!axis_web_storage_form_yes(request, "confirm")) {
		work_free(work);
		return axis_web_error_respond(request, 400, "Not grabbed", "Confirm the grab on its page first.");
	}
	work->digest = g_strdup(htmx_request_get_form_value(request, "digest"));
	work->checked = axis_web_storage_form_yes(request, "checked");
	work->one_file = axis_web_storage_form_yes(request, "one_file");
	/* The files ticked: replace_<library file id>=yes, one field each. */
	work->replace = ticked_fields(htmx_request_get_form_data(request), REPLACE_PREFIX);
	/* What the grab is bound to: exactly what the form names. */
	read_pick(work, htmx_request_get_form_data(request), TRUE);
	/* The replace of the download in flight, when the page offered it. */
	read_swap(work, request);
	work->run = force_run;
	work->done = force_done;
	return dispatch(work);
}

/* ── The routes ─────────────────────────────────────────────────── */

void
axis_web_manual_register(
	AxisWeb                 *web,
	const AxisWebManualInfo *info,
	gpointer                 data
){
	Registered *reg = g_new0(Registered, 1);
	g_autofree gchar *page = g_strdup_printf("%s/:id/manual", info->prefix);
	g_autofree gchar *grab = g_strdup_printf("%s/:id/manual/grab", info->prefix);
	g_autofree gchar *force = g_strdup_printf("%s/:id/manual/force", info->prefix);
	g_autofree gchar *block = g_strdup_printf("%s/:id/manual/block", info->prefix);

	reg->info = info;
	reg->data = data;
	axis_web_keep(web, reg, g_free);
	axis_web_route_full(web, HTMX_METHOD_GET, page, AXIS_WEB_ROUTE_DEFAULT, handle_page, reg);
	axis_web_route_full(web, HTMX_METHOD_POST, page, AXIS_WEB_ROUTE_ADMIN, handle_search, reg);
	axis_web_route_full(web, HTMX_METHOD_GET, grab, AXIS_WEB_ROUTE_ADMIN, handle_confirm, reg);
	axis_web_route_full(web, HTMX_METHOD_POST, grab, AXIS_WEB_ROUTE_ADMIN, handle_grab, reg);
	/* By force: an administrator's, both; a module that does not take forced
	 * grabs answers 404. */
	axis_web_route_full(web, HTMX_METHOD_GET, force, AXIS_WEB_ROUTE_ADMIN, handle_force_confirm, reg);
	axis_web_route_full(web, HTMX_METHOD_POST, force, AXIS_WEB_ROUTE_ADMIN, handle_force, reg);
	/* Block this release: an administrator's; a module without it answers 404. */
	axis_web_route_full(web, HTMX_METHOD_POST, block, AXIS_WEB_ROUTE_ADMIN, handle_block, reg);
}
