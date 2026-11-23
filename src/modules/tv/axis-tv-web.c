/*
 * axis-tv-web.c - the TV module's part of the web shell: its routes, the
 * work it hands to threads, its piece of every series' title page and
 * result card, its part of the dashboard, and the administrators' actions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   POST /series                      add a series (title, profile, root,
 *                                     monitor, specials, search)
 *   POST /series/:id/search           scope=episode|season|series, item=
 *   POST /series/:id/monitor          season=N or episode=ID, monitored=yes
 *   POST /series/:id/grab             a listed release (candidate=)
 *   POST /series/:id/give-up          an open grab (grab=, confirm=yes)
 *   POST /series/:id/blocklist/clear  one entry (entry=) or all
 *   GET, POST /series/block-release   Block and search again
 *                                     (web/axis-web-block.h)
 *   POST /series/:id/edit             profile, root
 *   POST /series/:id/kind             anime=yes: marked as anime; without it,
 *                                     standard again
 *   POST /series/:id/sync             follow the metadata now
 *   POST /series/:id/remove           forget the series (confirm=yes)
 *   /series/:id/manual...             a manual search of the series, a season
 *                                     or an episode (web/axis-web-manual.c)
 *
 * The pages are axis-tv-web-view.c's.  Every action is an administrator's,
 * checked by the route (AXIS_WEB_ROUTE_ADMIN) and the shell's CSRF check,
 * and recorded in the series' history with the user.
 */

#include "modules/tv/axis-tv-web.h"

#include <string.h>

#include "acquire/axis-acquire-ready.h"
#include "web/axis-user-store.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-block.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-downloads-private.h"
#include "web/axis-web-manual.h"
#include "web/axis-web-place.h"
#include "web/axis-web-ready.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Work off the main loop ─────────────────────────────────────── */

typedef struct
{
	AxisTv          *owner;
	AxisTvWorkFunc   work;
	AxisTvDoneFunc   done;
	HtmxRequest     *request;
	HtmxResponse    *deferred;
	gpointer         data;
	GDestroyNotify   free_data;
} Task;

static void
task_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Task *task = task_data;

	task->work(task->owner, task->data);
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;
	HtmxResponse *response;

	task->owner->pending--;
	response = task->done(task->owner, task->request, task->data);
	if (task->deferred != NULL)
		htmx_response_complete(task->deferred, response);
	if (response != NULL)
		g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	axis_tv_unref(task->owner);
	g_free(task);
}

HtmxResponse *
axis_tv_web_dispatch(
	AxisTv          *self,
	HtmxRequest     *request,
	AxisTvWorkFunc   work,
	AxisTvDoneFunc   done,
	gpointer         data,
	GDestroyNotify   free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_TV_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	task = g_new0(Task, 1);
	task->owner = axis_tv_ref(self);
	task->work = work;
	task->done = done;
	task->data = data;
	task->free_data = free_data;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	returned = g_object_ref(task->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

/* ── What the Library's folders are ────────────────────────────── */

gboolean
axis_tv_folder_titles(
	gpointer    user_data,
	gint64      root_id,
	GPtrArray  *dirs,
	GPtrArray  *titles,
	GError    **error
){
	AxisTv *self = user_data;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) list = g_string_new("{");
	g_autofree gchar *root = axis_tv_int_text(root_id);
	const gchar *params[2];
	guint asked = 0;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(dirs != NULL && titles != NULL, FALSE);

	/* A series' folder is at the top of its root; what is below it (its
	 * seasons) is not one. */
	for (i = 0; i < dirs->len; i++) {
		const AxisDirRecord *dir = g_ptr_array_index(dirs, i);

		if (dir->path == NULL || dir->path[0] == '\0' || strchr(dir->path, '/') != NULL)
			continue;
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, asked > 0 ? "," : "", dir->id);
		asked++;
	}
	if (asked == 0)
		return TRUE;
	g_string_append_c(list, '}');
	params[0] = list->str;
	params[1] = root;
	/* The first file under the folder linked to a series, walked by the
	 * index of the files' paths within the root (as the file list's folder
	 * filter is), else the series Identify linked the folder to; a folder of
	 * neither is not identified. */
	if (!axis_db_pool_exec(self->pool, "SELECT d.id, t.id, t.kind, t.provider, t.provider_id, t.title, t.year,"
	        " t.poster_ref, i.id, i.state FROM library_dir d"
	        " LEFT JOIN LATERAL (SELECT sf.series_id FROM library_file f JOIN series_file sf ON sf.file_id = f.id"
	        "   WHERE f.root_id = d.root_id AND starts_with(f.rel_path, d.path || '/')"
	        "   AND left(f.rel_path, 600) COLLATE \"C\" >= left(d.path || '/', 600)"
	        "   AND (length(d.path) >= 599 OR left(f.rel_path, 600) COLLATE \"C\" < d.path || '0')"
	        "   LIMIT 1) l ON true"
	        " LEFT JOIN series s ON s.id = l.series_id"
	        " LEFT JOIN library_identity i ON i.root_id = d.root_id AND i.path_hash = d.path_hash"
	        "   AND i.unit_kind = 'folder'"
	        " LEFT JOIN meta_title t ON t.id = coalesce(s.title_id, CASE WHEN i.state = 'linked' THEN i.title_id END)"
	        " WHERE d.id = ANY($1::bigint[]) AND d.root_id = $2::bigint", 2, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisWebFolderTitle *item = g_new0(AxisWebFolderTitle, 1);
		const gchar *kind_text = axis_db_result_get_text(rows, i, 2);
		AxisMetaKind kind = AXIS_META_KIND_SERIES;
		gint64 value = 0;

		axis_db_result_get_int64(rows, i, 0, &value);
		item->dir_id = value;
		value = 0;
		if (axis_db_result_get_int64(rows, i, 8, &value))
			item->unit_id = value;
		item->not_title = g_strcmp0(axis_db_result_get_text(rows, i, 9), "not_film") == 0;
		value = 0;
		if (axis_db_result_get_int64(rows, i, 1, &value) && value > 0 && kind_text != NULL &&
		    axis_meta_kind_from_string(kind_text, &kind)) {
			AxisMetaTitle *title = axis_meta_title_new(kind);
			gint64 year = 0;

			title->id = value;
			title->provider = g_strdup(axis_db_result_get_text(rows, i, 3));
			title->provider_id = g_strdup(axis_db_result_get_text(rows, i, 4));
			title->title = g_strdup(axis_db_result_get_text(rows, i, 5));
			if (axis_db_result_get_int64(rows, i, 6, &year))
				title->year = (gint)year;
			title->poster_ref = g_strdup(axis_db_result_get_text(rows, i, 7));
			if (title->provider != NULL && title->poster_ref != NULL)
				item->poster = axis_meta_artwork_find(self->meta, title->provider, title->poster_ref,
					AXIS_META_ART_POSTER_SMALL, TRUE);
			item->title = title;
		}
		g_ptr_array_add(titles, item);
	}
	return TRUE;
}

/* ── Choices ────────────────────────────────────────────────────── */

static void
choice_free(AxisTvChoice *choice)
{
	if (choice == NULL)
		return;
	g_free(choice->label);
	g_free(choice->slug);
	g_free(choice);
}

void
axis_tv_choices_clear(AxisTvChoices *choices)
{
	g_clear_pointer(&choices->profiles, g_ptr_array_unref);
	g_clear_pointer(&choices->roots, g_ptr_array_unref);
	g_clear_pointer(&choices->default_slug, g_free);
}

static gint
compare_roots(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvChoice *x = *(const AxisTvChoice *const *)a;
	const AxisTvChoice *y = *(const AxisTvChoice *const *)b;

	if (x->usable != y->usable)
		return x->usable ? -1 : 1;
	if (x->free_bytes != y->free_bytes)
		return x->free_bytes > y->free_bytes ? -1 : 1;
	return g_strcmp0(x->label, y->label);
}

gboolean
axis_tv_web_read_choices(
	AxisTv         *self,
	AxisTvChoices  *choices,
	GError        **error
){
	g_autoptr(GPtrArray) profiles = NULL;
	g_autoptr(GPtrArray) roots = NULL;
	g_autofree gchar *monitor = NULL;
	guint i;

	memset(choices, 0, sizeof(*choices));
	if (!axis_quality_store_list(self->quality, &profiles, error) ||
	    !axis_storage_list_roots(self->storage, 0, &roots, error))
		return FALSE;
	choices->default_slug = axis_quality_store_default_slug(self->quality);
	monitor = axis_tv_setting_string(self, AXIS_TV_SETTING_MONITOR, "all");
	choices->monitor = AXIS_TV_MONITOR_ALL;
	axis_tv_monitor_from_name(monitor, &choices->monitor);
	choices->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)choice_free);
	for (i = 0; i < profiles->len; i++) {
		const AxisQualityStored *stored = g_ptr_array_index(profiles, i);
		AxisTvChoice *choice = g_new0(AxisTvChoice, 1);

		choice->id = stored->id;
		choice->label = g_strdup(stored->name);
		choice->slug = g_strdup(stored->slug);
		choice->usable = stored->usable;
		g_ptr_array_add(choices->profiles, choice);
	}
	choices->roots = g_ptr_array_new_with_free_func((GDestroyNotify)choice_free);
	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		g_autoptr(AxisVolumeInfo) volume = NULL;
		AxisTvChoice *choice;

		if (root->media_type != AXIS_MEDIA_TV)
			continue;
		volume = axis_storage_get_volume(self->storage, root->volume_id);
		choice = g_new0(AxisTvChoice, 1);
		choice->id = root->id;
		choice->usable = volume != NULL && axis_volume_state_is_usable(volume->state);
		/* A new series goes only where it can be imported (the server
		 * checks again on Add). */
		choice->why_not = volume != NULL ? axis_volume_state_unwritable_why(volume->state) : "its disk is not known";
		choice->free_bytes = volume != NULL && volume->has_space ? volume->available_bytes : 0;
		if (volume != NULL && volume->has_space && choice->usable) {
			g_autofree gchar *free_text = axis_size_format(volume->available_bytes);

			choice->label = g_strdup_printf("%s on %s (%s free)", root->name, volume->name, free_text);
		} else {
			choice->label = g_strdup_printf("%s on %s (%s)", root->name, volume != NULL ? volume->name : "a disk",
				choice->usable ? "online" : "offline");
		}
		g_ptr_array_add(choices->roots, choice);
	}
	g_ptr_array_sort(choices->roots, compare_roots);
	return TRUE;
}

/* ── Pieces ─────────────────────────────────────────────────────── */

static const gchar *
mark_word(AxisTvState state)
{
	switch (state) {
	case AXIS_TV_NOT_AIRED:
		return "Not aired";
	case AXIS_TV_UPGRADE_WANTED:
		return "Upgrade wanted";
	case AXIS_TV_ATTENTION:
		return "Needs attention";
	case AXIS_TV_UNMONITORED:
		return "Not monitored";
	default:
		return axis_tv_state_label(state);
	}
}

HtmxNode *
axis_tv_web_mark(AxisTvState state)
{
	HtmxSpan *mark = htmx_span_new();
	g_autofree gchar *css = g_strdup_printf("film-mark-%s", axis_tv_state_name(state));

	htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark");
	htmx_element_add_class(HTMX_ELEMENT(mark), css);
	htmx_element_set_attribute(HTMX_ELEMENT(mark), "title", axis_tv_state_label(state));
	htmx_node_set_text_content(HTMX_NODE(mark), mark_word(state));
	return HTMX_NODE(mark);
}

HtmxNode *
axis_tv_web_completeness(const AxisTvSummary *summary)
{
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *text = summary->aired_monitored > 0 ? g_strdup_printf("%u of %u aired episodes in the library",
		summary->in_library, summary->aired_monitored) : g_strdup("No monitored episode has aired yet");

	htmx_element_add_class(HTMX_ELEMENT(box), "series-complete");
	ADD(box, axis_web_storage_text("series-complete-text", text));
	ADD(box, axis_web_downloads_bar(summary->aired_monitored > 0 ? (gdouble)summary->in_library /
		(gdouble)summary->aired_monitored : 0.0));
	return HTMX_NODE(box);
}

/* ── What needs attention ───────────────────────────────────────── */

/* How many reasons, and how many episodes of one, are named. */
#define ATTENTION_GROUPS_SHOWN 3
#define ATTENTION_EPISODES_SHOWN 4

static gboolean
kind_is_search(AxisAcquireAttention kind)
{
	return kind == AXIS_ACQUIRE_ATTENTION_REJECTED || kind == AXIS_ACQUIRE_ATTENTION_EXHAUSTED ||
		kind == AXIS_ACQUIRE_ATTENTION_FAILED || kind == AXIS_ACQUIRE_ATTENTION_NO_INDEXER ||
		kind == AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR || kind == AXIS_ACQUIRE_ATTENTION_NO_CLIENT ||
		kind == AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR || kind == AXIS_ACQUIRE_ATTENTION_METADATA;
}

gchar *
axis_tv_web_attention_href(
	gint64                     series_id,
	const AxisTvAttentionItem *item
){
	if (item->kind == AXIS_ACQUIRE_ATTENTION_IMPORT && item->import_id > 0)
		return g_strdup_printf("/imports/%" G_GINT64_FORMAT, item->import_id);
	if (kind_is_search(item->kind) && item->search_id > 0)
		return g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT, series_id,
			item->episode_id);
	return g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%d#episode-%" G_GINT64_FORMAT, series_id,
		item->season, item->episode_id);
}

gchar *
axis_tv_web_summary_href(const AxisTvSummary *summary)
{
	g_autoptr(GPtrArray) groups = NULL;
	const AxisTvAttentionGroup *top;

	if (summary->attention == NULL || summary->attention->len == 0)
		return NULL;
	groups = axis_tv_attention_groups(summary->attention);
	top = g_ptr_array_index(groups, 0);
	return axis_tv_web_attention_href(summary->series->id, g_ptr_array_index(top->items, 0));
}

/* Words beside links, as a span. */
static void
add_text(
	HtmxNode    *parent,
	const gchar *text
){
	HtmxSpan *span = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(span), text);
	ADD(parent, span);
}

/* What to do about one reason: links to where it is put right. */
static HtmxNode *
attention_do(
	const AxisTvSummary        *summary,
	const AxisTvAttentionGroup *group,
	gboolean                    admin
){
	const AxisTvSeries *s = summary->series;
	const AxisTvAttentionItem *first = g_ptr_array_index(group->items, 0);
	g_autofree gchar *here = axis_tv_web_attention_href(s->id, first);
	g_autofree gchar *settings = g_strdup_printf("/series/%" G_GINT64_FORMAT "#settings", s->id);
	HtmxP *line = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(line), "attention-do");
	switch (group->kind) {
	case AXIS_ACQUIRE_ATTENTION_IMPORT:
		ADD(line, axis_web_browser_link(here, "See the import and what resolves it"));
		break;
	case AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT:
		ADD(line, axis_web_browser_link("/downloads", "See the download on the Downloads page"));
		add_text(HTMX_NODE(line), ". Axis tries to remove it again every minute.");
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_ROOT:
		ADD(line, axis_web_browser_link(settings, "Choose a destination under Settings for this series"));
		break;
	case AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE:
		ADD(line, axis_web_browser_link("/storage", "See the disk on the Disks page"));
		add_text(HTMX_NODE(line), ", or ");
		ADD(line, axis_web_browser_link(settings, "choose another destination under Settings for this series"));
		break;
	case AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE: {
		g_autofree gchar *profile = s->profile_slug != NULL ? g_strdup_printf("/quality/%s", s->profile_slug) :
			g_strdup("/quality");

		ADD(line, axis_web_browser_link(profile, "See what is wrong with the quality profile"));
		add_text(HTMX_NODE(line), ", or ");
		ADD(line, axis_web_browser_link(settings, "choose another under Settings for this series"));
		break;
	}
	case AXIS_ACQUIRE_ATTENTION_NO_INDEXER:
	case AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR:
		if (admin) {
			ADD(line, axis_web_browser_link("/indexers", "Look at the indexers"));
			add_text(HTMX_NODE(line), ", then ");
			ADD(line, axis_web_browser_link(here, "search again from the episode's releases"));
		} else {
			add_text(HTMX_NODE(line), "An administrator can look at the indexers and search again.");
		}
		break;
	case AXIS_ACQUIRE_ATTENTION_NO_CLIENT:
	case AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR:
		if (admin) {
			ADD(line, axis_web_browser_link("/downloads/clients", "Look at the download clients"));
			add_text(HTMX_NODE(line), ", then ");
			ADD(line, axis_web_browser_link(here, "search again from the episode's releases"));
		} else {
			add_text(HTMX_NODE(line), "An administrator can look at the download clients and search again.");
		}
		break;
	case AXIS_ACQUIRE_ATTENTION_METADATA:
		ADD(line, axis_web_browser_link(settings, "Update details now, under Settings for this series"));
		break;
	default:
		ADD(line, axis_web_browser_link(here, first->search_id > 0 ? "See what was found and what you can do" :
			"See the episode"));
		break;
	}
	return HTMX_NODE(line);
}

HtmxNode *
axis_tv_web_attention(
	const AxisTvSummary *summary,
	gboolean             admin
){
	g_autoptr(GPtrArray) groups = axis_tv_attention_groups(summary->attention);
	HtmxDiv *box = htmx_div_new();
	HtmxUl *list = htmx_ul_new();
	guint shown = 0;
	guint i;
	guint k;

	htmx_element_set_id(HTMX_ELEMENT(box), "attention");
	htmx_element_add_class(HTMX_ELEMENT(box), "attention-box");
	if (summary->attention->len == 1) {
		const AxisTvAttentionItem *item = g_ptr_array_index(summary->attention, 0);
		g_autofree gchar *label = axis_tv_episode_label(item->season, item->number);
		g_autofree gchar *lead = g_strdup_printf("%s needs attention.", label);

		ADD(box, axis_web_storage_text("attention-lead", lead));
	} else {
		g_autofree gchar *lead = g_strdup_printf("%u episodes need attention.", summary->attention->len);

		ADD(box, axis_web_storage_text("attention-lead", lead));
	}
	htmx_element_add_class(HTMX_ELEMENT(list), "attention-reasons");
	for (i = 0; i < groups->len && i < ATTENTION_GROUPS_SHOWN; i++) {
		const AxisTvAttentionGroup *group = g_ptr_array_index(groups, i);
		HtmxLi *item = htmx_li_new();
		HtmxP *what = htmx_p_new();
		HtmxSpan *episodes = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(what), "attention-what");
		htmx_element_add_class(HTMX_ELEMENT(episodes), "attention-episodes");
		for (k = 0; k < group->items->len && k < ATTENTION_EPISODES_SHOWN; k++) {
			const AxisTvAttentionItem *e = g_ptr_array_index(group->items, k);
			g_autofree gchar *label = axis_tv_episode_label(e->season, e->number);
			g_autofree gchar *href = axis_tv_web_attention_href(summary->series->id, e);

			if (k > 0)
				add_text(HTMX_NODE(episodes), k + 1 == MIN(group->items->len, ATTENTION_EPISODES_SHOWN) &&
					group->items->len <= ATTENTION_EPISODES_SHOWN ? " and " : ", ");
			ADD(episodes, axis_web_browser_link(href, label));
		}
		if (group->items->len > ATTENTION_EPISODES_SHOWN) {
			g_autofree gchar *more = g_strdup_printf(" and %u more", group->items->len - ATTENTION_EPISODES_SHOWN);

			add_text(HTMX_NODE(episodes), more);
		}
		ADD(what, episodes);
		add_text(HTMX_NODE(what), ": ");
		add_text(HTMX_NODE(what), group->reason != NULL ? group->reason : "");
		ADD(item, what);
		ADD(item, attention_do(summary, group, admin));
		ADD(list, item);
		shown += group->items->len;
	}
	ADD(box, list);
	if (shown < summary->attention->len) {
		HtmxP *more = htmx_p_new();
		g_autofree gchar *text = g_strdup_printf("And %u more %s, for other reasons: ", summary->attention->len - shown,
			summary->attention->len - shown == 1 ? "episode" : "episodes");

		htmx_element_add_class(HTMX_ELEMENT(more), "attention-more");
		add_text(HTMX_NODE(more), text);
		ADD(more, axis_web_browser_link("/series/wanted?state=attention", "every episode that needs attention"));
		add_text(HTMX_NODE(more), ".");
		ADD(box, more);
	}
	return HTMX_NODE(box);
}

gchar *
axis_tv_web_done_text(
	const gchar        *key,
	const AxisTvSeries *series,
	const GPtrArray    *episodes
){
	const gchar *name = series != NULL && series->profile_name != NULL ? series->profile_name : "the new one";
	guint apart = 0;
	guint i;

	for (i = 0; episodes != NULL && i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		if (series != NULL && e->series_id == series->id && !e->gone && axis_tv_hold_stops(e->search_hold))
			apart++;
	}
	if (g_strcmp0(key, "profile") == 0)
		return g_strdup_printf("The quality profile is %s now. Every wanted episode is searched for again with it: what "
			"the searches find is listed under each episode's or season's releases.", name);
	if (g_strcmp0(key, "profile_none") == 0 && series != NULL && axis_tv_hold_stops(series->search_hold))
		return g_strdup_printf("The quality profile is %s now. The series was migrated and is held, so nothing was "
			"searched for: Axis searches for it once you start searching.", name);
	/* A person's search that queued nothing while episodes are held apart
	 * (Search now for the whole series, or for a season): that is why. */
	if (g_strcmp0(key, "nothing_wanted") == 0 && apart > 0)
		return g_strdup_printf("Nothing was searched for: %u %s held because Sonarr has %s that Axis does not see, and "
			"nothing else is wanted. To search for a held episode all the same, search for that episode, or open \"Let "
			"Axis search for it\".", apart, apart == 1 ? "episode is" : "episodes are", apart == 1 ? "a file for it" :
			"files for them");
	/* Not "nothing is wanted": what is missing is held, and that is why. */
	if (g_strcmp0(key, "profile_none") == 0 && apart > 0)
		return g_strdup_printf("The quality profile is %s now. Nothing was searched for: %u %s held apart because Sonarr "
			"has a file for %s that Axis does not see, and nothing else of the series is wanted under the profile.",
			name, apart, apart == 1 ? "episode is" : "episodes are", apart == 1 ? "it" : "each");
	if (g_strcmp0(key, "profile_none") == 0)
		return g_strdup_printf("The quality profile is %s now. Nothing of the series is wanted under it (no monitored "
			"episode is missing, or below its cutoff), so nothing was searched for.", name);
	if (g_strcmp0(key, "numbering") == 0 && series != NULL)
		return g_strdup_printf("The series follows %s now. Every file stays where it was on disk, linked to the episode "
			"it holds, under that episode's new number; episodes %s lacks are kept with their files, never searched "
			"for. No file was moved or renamed. Searches, imports and release names are read by this numbering from now "
			"on; releases found before were cleared.", axis_tv_source_label(series->episode_source),
			axis_tv_source_label(series->episode_source));
	return g_strdup(axis_tv_web_done_message(key));
}

const gchar *
axis_tv_web_done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "added", "The series was added: its wanted episodes are searched for as they air." },
		{ "managed", "Axis manages this series now. Its episodes on disk are in the library, so only the missing "
		  "episodes it monitors are wanted. Nothing was searched for, and nothing on disk was touched." },
		{ "unmanaged", "Axis no longer manages this series. No file was touched: its episodes are still on disk, so "
		  "it stays in TV as a series on disk that Axis does not manage." },
		{ "searching", "A search is queued: what it finds is listed under the episode's or the season's releases." },
		{ "already_searching", "Nothing new was queued: a search that does that work was queued or running already." },
		{ "searched_recently", "Nothing new was queued: that was searched for a moment ago. You can ask again in a "
		  "minute." },
		{ "nothing_wanted", "Nothing of it is wanted now: no search was queued." },
		{ "monitored", "Monitoring was changed." },
		{ "grabbed", "The release was sent to a download client." },
		{ "given_up", "Axis gave up on the download: it is on the blocklist, it is being removed from its client, and "
		  "the next best release is sought. A torrent's client deletes its files; SABnzbd keeps the files of a job "
		  "that finished, and Axis deletes them itself where the rules allow it. The Downloads page says what became "
		  "of them." },
		{ "cleared", "Taken off the blocklist." },
		{ "saved", "The series' settings were saved." },
		{ "started", "Axis searches for this series by itself from now on, at the scheduler's ordinary pace. Nothing was "
		  "searched for at once." },
		{ "not_held", "This series was not held: Axis searches for it by itself already." },
		{ "anime", "The series is marked as anime. Nothing else changed: no file, no link and no numbering. The card "
		  "\"Anime numbering\" says what its absolute numbers are." },
		{ "not_anime", "The series is no longer marked as anime. Nothing else changed: no file, no link and no "
		  "numbering." },
		{ "kind_same", "Nothing changed: the series was of that kind already." },
		{ "synced", "The series' details were updated: its seasons and episodes are as the provider lists them now." },
		{ "searching_undated", "Searches are queued for the season's episodes with no air date: what each finds is "
		  "listed under that episode's releases." },
		{ "nothing_undated", "No monitored episode of that season is missing without an air date: nothing was "
		  "searched for." },
		{ "removed", "The series was removed. No file was touched." },
		{ "numbering", "The episode numbering was changed. No file was moved or renamed." },
		{ "placed", "Placed by its name: only the file's link moved, to the episode its name says; the file on disk was "
		  "not touched. Both episodes' history says so." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(messages[i].key, key) == 0)
			return messages[i].text;
	}
	return NULL;
}

static HtmxNode *
select_field(
	const gchar *id,
	const gchar *label_text,
	const gchar *css,
	HtmxNode    *select
){
	HtmxDiv *div = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(div), css);
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	ADD(div, label);
	ADD(div, select);
	return HTMX_NODE(div);
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
profile_select(
	const AxisTvChoices *choices,
	gint64               current
){
	HtmxSelect *select = htmx_select_new_with_name("profile");
	guint i;

	for (i = 0; i < choices->profiles->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(choices->profiles, i);
		g_autofree gchar *value = axis_tv_int_text(c->id);
		HtmxOption *option;

		if (!c->usable && c->id != current)
			continue;
		option = htmx_option_new_with_value(value, c->label);
		htmx_option_set_selected(option, current > 0 ? c->id == current : g_strcmp0(c->slug, choices->default_slug) == 0);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
root_select(
	const AxisTvChoices *choices,
	gint64               current
){
	HtmxSelect *select = htmx_select_new_with_name("root");
	gboolean first = TRUE;
	guint i;

	for (i = 0; i < choices->roots->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(choices->roots, i);
		g_autofree gchar *value = axis_tv_int_text(c->id);
		g_autofree gchar *label = NULL;
		HtmxOption *option;

		/* A root on a disk that is away, or mounted read-only, is listed
		 * with why, and chosen only to keep a series' own. */
		if (c->why_not != NULL && c->id != current) {
			label = g_strdup_printf("%s: cannot be written to now, %s", c->label, c->why_not);
			option = htmx_option_new_with_value(value, label);
			htmx_element_set_attribute(HTMX_ELEMENT(option), "disabled", "disabled");
			ADD(select, option);
			continue;
		}
		option = htmx_option_new_with_value(value, c->label);
		htmx_option_set_selected(option, current > 0 ? c->id == current : first);
		first = FALSE;
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
monitor_select(AxisTvMonitor current)
{
	HtmxSelect *select = htmx_select_new_with_name("monitor");
	guint i;

	for (i = 0; i <= AXIS_TV_MONITOR_NONE; i++) {
		HtmxOption *option = htmx_option_new_with_value(axis_tv_monitor_name((AxisTvMonitor)i),
			axis_tv_monitor_label((AxisTvMonitor)i));

		htmx_option_set_selected(option, (AxisTvMonitor)i == current);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

/*
 * The "Anime" box of the Add and Manage forms (add-anime-support, A1): never
 * ticked by Axis, whatever the title's details say.  @hinted: the stored
 * details say animation whose original language is Japanese, which is said
 * beside the box and ticks nothing.  @numbering: what the series' numbering
 * will be, a sentence.
 */
static HtmxNode *
anime_field(
	const gchar *id,
	gboolean     hinted,
	const gchar *numbering
){
	HtmxDiv *box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "field-anime");
	ADD(box, check_field(id, AXIS_TV_WEB_ANIME_LABEL, "anime", FALSE));
	if (hinted)
		ADD(box, axis_web_storage_text("help anime-hint", "The details say this is animation from Japan."));
	ADD(box, axis_web_storage_text("help anime-numbering", numbering));
	ADD(box, axis_web_storage_text("help anime-says", AXIS_TV_WEB_ANIME_SAYS));
	return HTMX_NODE(box);
}

static guint
usable_roots(const AxisTvChoices *choices)
{
	guint n = 0;
	guint i;

	for (i = 0; i < choices->roots->len; i++)
		n += ((const AxisTvChoice *)g_ptr_array_index(choices->roots, i))->why_not == NULL ? 1 : 0;
	return n;
}

static const AxisTvChoice *first_profile(const AxisTvChoices *choices);
static const AxisTvChoice *first_root(const AxisTvChoices *choices);

/*
 * The add form's fields other than its button, in a disclosure: on a phone
 * it is closed, its summary names the profile and destination the form
 * holds, and the button adds with them; wider, axis.css shows the fields in
 * the form's row as they always were.
 */
static HtmxNode *
add_options(
	HtmxRequest         *request,
	const AxisTvChoices *choices
){
	const AxisTvChoice *profile;
	const AxisTvChoice *root;
	AxisTvMonitor monitor;
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxSpan *chosen = htmx_span_new();
	g_autofree gchar *text = NULL;

	axis_tv_web_defaults(request, choices, &profile, &root, &monitor);
	text = g_strdup_printf("%s, %s", profile != NULL ? profile->label : "", root != NULL ? root->label : "");
	htmx_element_add_class(HTMX_ELEMENT(details), "add-options");
	htmx_node_set_text_content(HTMX_NODE(summary), "Options");
	htmx_element_add_class(HTMX_ELEMENT(chosen), "add-options-chosen");
	htmx_node_set_text_content(HTMX_NODE(chosen), text);
	ADD(summary, chosen);
	ADD(details, summary);
	return HTMX_NODE(details);
}

HtmxNode *
axis_tv_web_add_form(
	HtmxRequest          *request,
	gint64                title_id,
	const AxisTvChoices  *choices,
	gboolean              anime_hint,
	gboolean              tvdb_key
){
	HtmxDiv *box = htmx_div_new();
	HtmxNode *form;
	HtmxNode *options;
	g_autofree gchar *title = axis_tv_int_text(title_id);

	htmx_element_add_class(HTMX_ELEMENT(box), "film-add");
	ADD(box, axis_web_storage_text("film-add-lead", "Not among your series. Add it with what to monitor, and Axis "
		"looks for each episode once it has aired and until it is found."));
	if (usable_roots(choices) == 0) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(line), "There is no library folder for TV on a disk that is online, so "
			"there is nowhere for its episodes to go. Add one first: ");
		ADD(line, axis_web_browser_link("/storage", "Disks"));
		ADD(box, line);
		return HTMX_NODE(box);
	}
	form = axis_web_auth_post_form(request, "/series");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	ADD(form, axis_web_auth_hidden("title", title));
	/* Opened from a list (Discover, the search results): back to it, at the
	 * title's card. */
	{
		g_autofree gchar *from = NULL;
		g_autofree gchar *card = NULL;

		if (axis_web_back_from_add(request, title_id, &from, &card)) {
			ADD(form, axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, from));
			ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, card));
		}
	}
	options = add_options(request, choices);
	/* Chosen at first: the person's own choice for Adds (the bar above a
	 * list); what is chosen here wins for this one. */
	{
		const AxisTvChoice *profile;
		const AxisTvChoice *root;
		AxisTvMonitor monitor;

		axis_tv_web_defaults(request, choices, &profile, &root, &monitor);
		ADD(options, select_field("series-add-profile", "Quality profile", NULL, profile_select(choices,
			profile != NULL ? profile->id : 0)));
		ADD(options, select_field("series-add-root", "Destination", "field-wide", root_select(choices,
			root != NULL ? root->id : 0)));
		ADD(options, select_field("series-add-monitor", "What to monitor", NULL, monitor_select(monitor)));
	}
	ADD(options, check_field("series-add-specials", "Monitor the specials too", "specials", FALSE));
	ADD(options, check_field("series-add-search", "Search now", "search", TRUE));
	/* Its kind: a person's to say, and what numbering that gives it. */
	ADD(options, anime_field("series-add-anime", anime_hint, tvdb_key ?
		"Ticked, the series follows TheTVDB's aired order from the start, which carries absolute numbers, when TheTVDB "
		"lists it; else the numbering of any new series." :
		"Axis has no TheTVDB key, so ticked or not the series follows the numbering of any new series, which has no "
		"absolute numbers. Set a TheTVDB key on the Movie and TV details page first to have them."));
	ADD(form, options);
	ADD(form, axis_web_auth_button("Add to TV", "button-primary"));
	ADD(box, form);
	return HTMX_NODE(box);
}

/* What profile_select() and root_select() select at first, or %NULL. */
static const AxisTvChoice *
first_profile(const AxisTvChoices *choices)
{
	const AxisTvChoice *first = NULL;
	guint i;

	for (i = 0; i < choices->profiles->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(choices->profiles, i);

		if (!c->usable)
			continue;
		if (g_strcmp0(c->slug, choices->default_slug) == 0)
			return c;
		if (first == NULL)
			first = c;
	}
	return first;
}

static const AxisTvChoice *
first_root(const AxisTvChoices *choices)
{
	guint i;

	for (i = 0; i < choices->roots->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(choices->roots, i);

		if (c->why_not == NULL)
			return c;
	}
	return NULL;
}

/* ── The person's defaults for Add from a list ──────────────────── */

/* The choice @id of @list a new series can take now (a profile that can be
 * used, a root it can be imported into), or %NULL. */
static const AxisTvChoice *
usable_choice(
	const GPtrArray *list,
	gint64           id
){
	guint i;

	for (i = 0; id > 0 && list != NULL && i < list->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(list, i);

		if (c->id == id)
			return c->usable && c->why_not == NULL ? c : NULL;
	}
	return NULL;
}

/* Whether the person chose a folder or a profile that cannot be taken now
 * (its disk away or read-only, or removed): their Adds use the
 * installation's until they choose again, and the bar says so. */
static gboolean
defaults_lapsed(
	HtmxRequest         *request,
	const AxisTvChoices *choices
){
	g_autoptr(JsonObject) mine = axis_web_request_add_defaults(request, AXIS_TV_WEB_DEFAULTS_KIND);
	gint64 root;
	gint64 profile;

	if (mine == NULL)
		return FALSE;
	root = json_object_get_int_member_with_default(mine, "root", 0);
	profile = json_object_get_int_member_with_default(mine, "profile", 0);
	return (root > 0 && usable_choice(choices->roots, root) == NULL) ||
		(profile > 0 && usable_choice(choices->profiles, profile) == NULL);
}

void
axis_tv_web_defaults(
	HtmxRequest          *request,
	const AxisTvChoices  *choices,
	const AxisTvChoice  **profile,
	const AxisTvChoice  **root,
	AxisTvMonitor        *monitor
){
	g_autoptr(JsonObject) mine = request != NULL ? axis_web_request_add_defaults(request, AXIS_TV_WEB_DEFAULTS_KIND) :
		NULL;
	const AxisTvChoice *chosen;
	AxisTvMonitor value;

	*profile = first_profile(choices);
	*root = first_root(choices);
	*monitor = choices->monitor;
	/* (A choice that cannot be taken now is no choice: the installation's
	 * defaults, as before any was made; the bar says so.) */
	if (mine == NULL || defaults_lapsed(request, choices))
		return;
	chosen = usable_choice(choices->profiles, json_object_get_int_member_with_default(mine, "profile", 0));
	if (chosen != NULL)
		*profile = chosen;
	chosen = usable_choice(choices->roots, json_object_get_int_member_with_default(mine, "root", 0));
	if (chosen != NULL)
		*root = chosen;
	if (axis_tv_monitor_from_name(json_object_get_string_member_with_default(mine, "monitor", ""), &value))
		*monitor = value;
}

/* The classes by which the bar's answer finds every card's pieces. */
#define CHOSEN_CLASS  "add-default-chosen-series"
#define PROFILE_CLASS "add-default-profile-series"
#define ROOT_CLASS    "add-default-root-series"
#define MONITOR_CLASS "add-default-monitor-series"

static HtmxNode *
classed_hidden(
	const gchar *name,
	const gchar *value,
	const gchar *css
){
	HtmxNode *input = axis_web_auth_hidden(name, value);

	htmx_element_add_class(HTMX_ELEMENT(input), css);
	return input;
}

static HtmxNode *
classed_id(
	const gchar *name,
	gint64       value,
	const gchar *css
){
	g_autofree gchar *text = axis_tv_int_text(value);

	return classed_hidden(name, text, css);
}

static HtmxNode *
chosen_span(
	const AxisTvChoice *profile,
	const AxisTvChoice *root,
	AxisTvMonitor       monitor
){
	g_autofree gchar *words = g_strdup_printf("%s, %s, %s", profile != NULL ? profile->label : "",
		root != NULL ? root->label : "", axis_tv_monitor_label(monitor));
	HtmxSpan *span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), "quick-add-chosen");
	htmx_element_add_class(HTMX_ELEMENT(span), CHOSEN_CLASS);
	htmx_node_set_text_content(HTMX_NODE(span), words);
	return HTMX_NODE(span);
}

/* The folder select of the bar: every series folder, those on a disk Axis
 * cannot write to now shown and not to be chosen, with why. */
static HtmxNode *
bar_root_select(
	const AxisTvChoices *choices,
	const AxisTvChoice  *current
){
	HtmxSelect *select = htmx_select_new_with_name("root");
	guint i;

	for (i = 0; i < choices->roots->len; i++) {
		const AxisTvChoice *c = g_ptr_array_index(choices->roots, i);
		g_autofree gchar *value = axis_tv_int_text(c->id);
		g_autofree gchar *label = c->why_not == NULL ? g_strdup(c->label) :
			g_strdup_printf("%s: cannot be written to now, %s", c->label, c->why_not);
		HtmxOption *option = htmx_option_new_with_value(value, label);

		if (c->why_not != NULL)
			htmx_element_set_attribute(HTMX_ELEMENT(option), "disabled", "disabled");
		htmx_option_set_selected(option, current != NULL && c->id == current->id);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

HtmxNode *
axis_tv_web_defaults_bar(
	HtmxRequest         *request,
	const AxisTvChoices *choices,
	const gchar         *page
){
	const AxisTvChoice *profile;
	const AxisTvChoice *root;
	AxisTvMonitor monitor;
	HtmxNode *form = axis_web_auth_post_form(request, "/series/defaults");
	g_autofree gchar *here = page == NULL ? axis_web_back_here(request) : NULL;
	HtmxNode *back = page != NULL || here != NULL ? axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, page != NULL ? page :
		here) : NULL;
	HtmxNode *button = axis_web_auth_button("Save as my defaults", "button-small");

	axis_tv_web_defaults(request, choices, &profile, &root, &monitor);
	htmx_element_set_id(HTMX_ELEMENT(form), AXIS_TV_WEB_DEFAULTS_BAR);
	htmx_element_add_class(HTMX_ELEMENT(form), "add-defaults");
	/* With htmx a change is stored at once and the cards follow; without,
	 * the button. */
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-post", "/series/defaults");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-trigger", "change");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-swap", "none");
	ADD(form, select_field("add-defaults-series-root", "New series go to", "add-defaults-field",
		bar_root_select(choices, root)));
	ADD(form, select_field("add-defaults-series-profile", "with", "add-defaults-field", profile_select(choices,
		profile != NULL ? profile->id : 0)));
	ADD(form, select_field("add-defaults-series-monitor", "monitoring", "add-defaults-field",
		monitor_select(monitor)));
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, AXIS_TV_WEB_DEFAULTS_BAR));
	ADD(form, button);
	if (defaults_lapsed(request, choices))
		ADD(form, axis_web_storage_text("help add-defaults-lapsed", "The folder or profile you chose cannot be used "
			"now (its disk is away or read-only, or it was removed): Adds use these until you choose again."));
	return form;
}

HtmxResponse *
axis_tv_web_defaults_answer(
	HtmxRequest         *request,
	const AxisTvChoices *choices
){
	g_autoptr(HtmxFragment) fragment = htmx_fragment_new();
	g_autofree gchar *page = axis_web_back_posted(request);
	const AxisTvChoice *profile;
	const AxisTvChoice *root;
	AxisTvMonitor monitor;
	HtmxNode *node;

	axis_tv_web_defaults(request, choices, &profile, &root, &monitor);
	axis_web_activity_add_oob(HTMX_NODE(fragment), axis_tv_web_defaults_bar(request, choices,
		page != NULL ? page : ""));
	if (profile != NULL && root != NULL) {
		node = classed_id("profile", profile->id, PROFILE_CLASS);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." PROFILE_CLASS);
		node = classed_id("root", root->id, ROOT_CLASS);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." ROOT_CLASS);
		node = classed_hidden("monitor", axis_tv_monitor_name(monitor), MONITOR_CLASS);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." MONITOR_CLASS);
		node = chosen_span(profile, root, monitor);
		axis_web_activity_add_oob(HTMX_NODE(fragment), node);
		htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "outerHTML:." CHOSEN_CLASS);
	}
	return axis_web_activity_respond(HTMX_NODE(fragment));
}

HtmxNode *
axis_tv_web_quick_add(
	HtmxRequest          *request,
	gint64                title_id,
	const AxisTvChoices  *choices
){
	const AxisTvChoice *profile;
	const AxisTvChoice *root;
	AxisTvMonitor monitor;
	/* Its page knows the list it came from, and comes back to this card. */
	g_autofree gchar *page = axis_web_back_title_href(request, title_id);
	HtmxDiv *box = htmx_div_new();
	HtmxNode *back = NULL;
	HtmxA *options;

	/* Where the person's Adds go (the bar above the list). */
	axis_tv_web_defaults(request, choices, &profile, &root, &monitor);
	htmx_element_add_class(HTMX_ELEMENT(box), "quick-add");
	if (profile != NULL && root != NULL)
		back = axis_web_back_add_field(request, title_id);
	if (back != NULL) {
		g_autofree gchar *title = axis_tv_int_text(title_id);
		g_autofree gchar *tip = g_strdup_printf("Add to TV with the quality profile %s and the destination %s. "
			"What to monitor: %s", profile->label, root->label, axis_tv_monitor_label(monitor));
		HtmxNode *form = axis_web_auth_post_form(request, "/series");
		HtmxNode *button = axis_web_auth_button("Add to TV", "button-primary button-small");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		ADD(form, axis_web_auth_hidden("title", title));
		ADD(form, classed_id("profile", profile->id, PROFILE_CLASS));
		ADD(form, classed_id("root", root->id, ROOT_CLASS));
		ADD(form, classed_hidden("monitor", axis_tv_monitor_name(monitor), MONITOR_CLASS));
		ADD(form, axis_web_auth_hidden("search", "yes"));
		ADD(form, back);
		/* Back at this card (Discover's id for it), or where the list says
		 * when the card is gone after the add. */
		{
			g_autofree gchar *card = axis_web_back_add_place(request, title_id);

			ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, card));
		}
		htmx_element_set_attribute(HTMX_ELEMENT(button), "title", tip);
		ADD(form, button);
		ADD(box, form);
	}
	/* The title page's form chooses everything (and says what is missing). */
	options = htmx_a_new_with_href(page);
	htmx_element_add_class(HTMX_ELEMENT(options), "quick-add-options");
	htmx_node_set_text_content(HTMX_NODE(options), "Add with options");
	htmx_element_set_attribute(HTMX_ELEMENT(options), "title", back != NULL ?
		"Choose the quality profile, the destination, what to monitor and the rest on the series' page" :
		"There is no usable quality profile or library folder for TV: the series' page says what is needed");
	/* Closed, it says what the Add uses. */
	if (back != NULL)
		ADD(options, chosen_span(profile, root, monitor));
	ADD(box, options);
	return HTMX_NODE(box);
}

HtmxNode *
axis_tv_web_disk_completeness(const AxisTvOnDisk *disk)
{
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *text = axis_tv_on_disk_episodes_text(disk);

	htmx_element_add_class(HTMX_ELEMENT(box), "series-complete");
	ADD(box, axis_web_storage_text("series-complete-text", text));
	ADD(box, axis_web_downloads_bar(disk->counted && disk->episodes > 0 ? (gdouble)MIN(disk->on_disk, disk->episodes) /
		(gdouble)disk->episodes : 0.0));
	return HTMX_NODE(box);
}

gchar *
axis_tv_web_disk_reason(const AxisTvOnDisk *disk)
{
	GString *text = g_string_new("On disk, not managed by Axis: nothing is searched for, downloaded, renamed or "
		"moved for it.");

	if (disk->n_away > 0 && disk->n_away >= disk->n_files)
		g_string_append_printf(text, " Its disk, %s, is offline: it stays listed, since a disk that is away never "
			"means a series is gone.", disk->away_volume != NULL ? disk->away_volume : "a disk");
	else if (disk->n_away > 0)
		g_string_append_printf(text, " One of its disks, %s, is offline.", disk->away_volume != NULL ?
			disk->away_volume : "a disk");
	return g_string_free(text, FALSE);
}

HtmxNode *
axis_tv_web_manage_form(
	HtmxRequest          *request,
	const AxisTvOnDisk   *disk,
	const AxisTvChoices  *choices,
	gboolean              anime_hint
){
	HtmxDiv *box = htmx_div_new();
	HtmxNode *form;
	HtmxSelect *monitor = htmx_select_new_with_name("monitor");
	g_autofree gchar *title = axis_tv_int_text(disk->title_id);
	static const AxisTvMonitor offered[] = { AXIS_TV_MONITOR_FUTURE, AXIS_TV_MONITOR_ALL, AXIS_TV_MONITOR_NONE };
	static const gchar *const labels[] = {
		"Future episodes only: new ones are searched for as they air",
		"Every episode: the missing ones are searched for, and the ones on disk upgraded as the profile says",
		"Nothing yet: no episode is monitored, and nothing is searched for"
	};
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(box), "film-add");
	ADD(box, axis_web_storage_text("film-add-lead", "Axis lists this series because its episodes are on disk, and "
		"does not manage it. Manage it, and its episodes on disk count as in the library: only the missing episodes "
		"you choose to monitor are wanted, and nothing is searched for now."));
	if (usable_roots(choices) == 0) {
		ADD(box, axis_web_storage_text("help", "There is no library folder for TV on a disk that is online, so it "
			"cannot be managed now. Add one first: "));
		ADD(box, axis_web_browser_link("/storage", "Disks"));
		return HTMX_NODE(box);
	}
	for (i = 0; i < G_N_ELEMENTS(offered); i++) {
		HtmxOption *option = htmx_option_new_with_value(axis_tv_monitor_name(offered[i]), labels[i]);

		htmx_option_set_selected(option, i == 0);
		ADD(monitor, option);
	}
	form = axis_web_auth_post_form(request, "/series");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	ADD(form, axis_web_auth_hidden("title", title));
	ADD(form, axis_web_auth_hidden("manage", "yes"));
	ADD(form, select_field("series-manage-profile", "Quality profile", NULL, profile_select(choices, 0)));
	ADD(form, select_field("series-manage-root", "Destination", "field-wide", root_select(choices,
		disk->root_id)));
	ADD(form, select_field("series-manage-monitor", "What to monitor", "field-wide", HTMX_NODE(monitor)));
	/* Its kind.  A series managed where its files are keeps the numbering
	 * those files were identified under (axis_tv_anime_source()). */
	ADD(form, anime_field("series-manage-anime", anime_hint, "Managed where its files are, the series keeps the "
		"numbering its files were read under, ticked or not. Its page then says whether it has absolute numbers, and "
		"leads to the page that changes its numbering, which shows how each episode would map first."));
	ADD(form, axis_web_auth_button("Manage this series", "button-primary"));
	ADD(box, form);
	return HTMX_NODE(box);
}

/* ── The title page and the result cards ────────────────────────── */

typedef struct
{
	AxisWebActionPlace  place;
	gboolean            is_series;
	gint64              title_id;
	AxisTvSummary      *summary;	/* (nullable): not managed */
	AxisTvChoices       choices;
	gboolean            choices_read;
	AxisReadiness       ready;	/* what is needed to download anything */
	gboolean            ready_read;
	GHashTable         *by_title;	/* cards: title id -> AxisTvSummary */
	AxisTvOnDisk       *disk;	/* (nullable): on disk, not managed */
	GHashTable         *on_disk;	/* cards: the title ids of series on disk */
} ActionFigures;

static void
action_figures_free(gpointer data)
{
	ActionFigures *figures = data;

	if (figures == NULL)
		return;
	axis_tv_summary_free(figures->summary);
	axis_tv_on_disk_free(figures->disk);
	axis_tv_choices_clear(&figures->choices);
	axis_readiness_clear(&figures->ready);
	g_clear_pointer(&figures->by_title, g_hash_table_unref);
	g_clear_pointer(&figures->on_disk, g_hash_table_unref);
	g_free(figures);
}

static gpointer
actions_collect(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
){
	AxisTv *self = user_data;
	ActionFigures *figures = g_new0(ActionFigures, 1);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GPtrArray) disks = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	figures->place = place;
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		if (title->kind == AXIS_META_KIND_SERIES && title->id > 0)
			g_array_append_val(ids, title->id);
	}
	if (ids->len == 0)
		return figures;
	/* The managed series among them, and the series on disk (two more
	 * statements, whatever their number). */
	if (!axis_tv_get_by_titles(self, ids, &series, &error) ||
	    !axis_tv_get_on_disk_by_titles(self, ids, &disks, &error)) {
		g_message("series: the title page's series could not be read: %s", error->message);
		action_figures_free(figures);
		return NULL;
	}
	if (place != AXIS_WEB_ACTION_PAGE) {
		figures->by_title = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
			(GDestroyNotify)axis_tv_summary_free);
		figures->on_disk = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
		while (series->len > 0) {
			AxisTvSummary *summary = g_ptr_array_steal_index(series, 0);
			gint64 *key = g_new(gint64, 1);

			*key = summary->series->title_id;
			g_hash_table_insert(figures->by_title, key, summary);
		}
		for (i = 0; i < disks->len; i++) {
			gint64 *key = g_new(gint64, 1);

			*key = ((AxisTvOnDisk *)g_ptr_array_index(disks, i))->title_id;
			g_hash_table_add(figures->on_disk, key);
		}
		/* A Discover card, or a search result, adds in one step: with the add
		 * form's defaults. */
		if (place == AXIS_WEB_ACTION_DISCOVER || place == AXIS_WEB_ACTION_CARD) {
			figures->choices_read = axis_tv_web_read_choices(self, &figures->choices, &error);
			if (!figures->choices_read) {
				g_message("series: the profiles and roots could not be read: %s", error->message);
				g_clear_error(&error);
			}
		}
		return figures;
	}
	figures->is_series = TRUE;
	figures->title_id = g_array_index(ids, gint64, 0);
	if (series->len > 0)
		figures->summary = g_ptr_array_steal_index(series, 0);
	else if (disks->len > 0)
		figures->disk = g_ptr_array_steal_index(disks, 0);
	figures->choices_read = axis_tv_web_read_choices(self, &figures->choices, &error);
	if (!figures->choices_read) {
		g_message("series: the profiles and roots could not be read: %s", error->message);
		g_clear_error(&error);
	}
	figures->ready_read = axis_acquire_check_ready(self->meta, self->indexers, self->downloads, self->storage,
		AXIS_READY_SERIES, &figures->ready, &error);
	if (!figures->ready_read)
		g_message("series: what is needed to download could not be read: %s", error->message);
	return figures;
}

static HtmxNode *
actions_render(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             data,
	gpointer             user_data
){
	ActionFigures *figures = data;
	gboolean admin = axis_web_request_is_admin(request);
	HtmxDiv *piece;

	if (figures == NULL || title->kind != AXIS_META_KIND_SERIES)
		return NULL;
	/* Above the list: where the person's Adds go (administrators). */
	if (place == AXIS_WEB_ACTION_BAR)
		return admin && figures->choices_read ? axis_tv_web_defaults_bar(request, &figures->choices, NULL) : NULL;
	if (place == AXIS_WEB_ACTION_CARD || place == AXIS_WEB_ACTION_DISCOVER) {
		AxisTvSummary *summary = figures->by_title != NULL ? g_hash_table_lookup(figures->by_title, &title->id) : NULL;

		if (summary != NULL)
			return axis_tv_web_mark(summary->state);
		/* On disk, not managed: in the library. */
		if (figures->on_disk != NULL && g_hash_table_contains(figures->on_disk, &title->id))
			return axis_tv_web_mark(AXIS_TV_IN_LIBRARY);
		if (!admin || !figures->choices_read || title->id <= 0)
			return NULL;
		return axis_tv_web_quick_add(request, title->id, &figures->choices);
	}
	if (!figures->is_series || place == AXIS_WEB_ACTION_DETAIL)
		return NULL;
	if (place == AXIS_WEB_ACTION_NOTICE) {
		/* A series with episodes still to get: what stands in the way of
		 * them is said at the top of its page. */
		if (figures->summary == NULL || !figures->ready_read || figures->summary->state == AXIS_TV_IN_LIBRARY ||
		    figures->summary->state == AXIS_TV_UNMONITORED)
			return NULL;
		return axis_web_ready_notice_new(&figures->ready, AXIS_READY_SERIES, "series", admin);
	}
	piece = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(piece), "film-panel");
	if (figures->summary != NULL) {
		g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT, figures->summary->series->id);
		HtmxDiv *state = htmx_div_new();
		HtmxDiv *line = htmx_div_new();
		g_autofree gchar *profile = g_strdup_printf("Quality profile: %s", figures->summary->series->profile_name);

		htmx_element_add_class(HTMX_ELEMENT(state), "film-state");
		htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
		ADD(line, axis_tv_web_mark(figures->summary->state));
		ADD(line, axis_web_storage_text("film-state-profile", profile));
		ADD(state, line);
		/* What needs attention says what is wrong and where it is put right;
		 * any other state says why in one line. */
		if (figures->summary->attention != NULL && figures->summary->attention->len > 0)
			ADD(state, axis_tv_web_attention(figures->summary, admin));
		else
			ADD(state, axis_web_storage_text("film-reason", figures->summary->reason));
		ADD(state, axis_tv_web_completeness(figures->summary));
		ADD(piece, state);
		ADD(piece, axis_web_browser_link(href, "Episodes and downloads"));
	} else if (figures->disk != NULL) {
		/* On disk, not managed: what it is, and how to have Axis manage it. */
		g_autofree gchar *href = g_strdup_printf("/series/on-disk/%" G_GINT64_FORMAT, figures->disk->title_id);
		g_autofree gchar *reason = axis_tv_web_disk_reason(figures->disk);
		HtmxDiv *state = htmx_div_new();
		HtmxDiv *line = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(state), "film-state");
		htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
		ADD(line, axis_tv_web_mark(AXIS_TV_IN_LIBRARY));
		ADD(state, line);
		ADD(state, axis_web_storage_text("film-reason", reason));
		ADD(state, axis_tv_web_disk_completeness(figures->disk));
		ADD(piece, state);
		ADD(piece, axis_web_browser_link(href, "Episodes on disk"));
		if (admin && figures->choices_read)
			ADD(piece, axis_tv_web_manage_form(request, figures->disk, &figures->choices,
				axis_tv_title_hints_anime(title->genres, title->original_language)));
		else
			ADD(piece, axis_web_storage_text("help", admin ? "The quality profiles and library folders could not be read, so it cannot "
				"be managed now." : "Axis does not manage this series. An administrator can have Axis manage it."));
	} else if (admin && figures->choices_read) {
		HtmxNode *note = figures->ready_read ? axis_web_ready_note_new(&figures->ready, AXIS_READY_SERIES, "series") :
			NULL;

		/* (What the details say of the title is a hint beside the box, never
		 * a tick.) */
		ADD(piece, axis_tv_web_add_form(request, title->id, &figures->choices,
			axis_tv_title_hints_anime(title->genres, title->original_language),
			axis_tv_tvdb_offered((AxisTv *)user_data)));
		/* Adding is still possible, but not surprising. */
		if (note != NULL)
			ADD(piece, note);
	} else {
		ADD(piece, axis_web_storage_text("help", admin ? "Not among your series: the quality profiles and library folders could not be "
			"read, so it cannot be added now." : "Not among your series. An administrator can add it."));
	}
	return HTMX_NODE(piece);
}

/* ── The dashboard ──────────────────────────────────────────────── */

typedef struct
{
	guint      counts[AXIS_TV_N_STATES];
	guint      kinds[AXIS_ACQUIRE_ATTENTION_FAILED + 1];	/* why they need attention */
	guint      series;
	guint      on_disk;	/* series on disk Axis does not manage */
	GPtrArray *today;	/* gchar *: "Series - S01E02 - Title" */
	GPtrArray *today_href;
	gint64     taken_us;
} DashFigures;

static void
dash_free(gpointer data)
{
	DashFigures *figures = data;

	if (figures == NULL)
		return;
	g_clear_pointer(&figures->today, g_ptr_array_unref);
	g_clear_pointer(&figures->today_href, g_ptr_array_unref);
	g_free(figures);
}

static gpointer
dash_collect(gpointer user_data)
{
	AxisTv *self = user_data;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(AxisDbResult) today = NULL;
	g_autoptr(AxisDbResult) disk = NULL;
	g_autoptr(GError) error = NULL;
	DashFigures *figures;
	AxisTvTotals totals;
	AxisTvRules rules;
	AxisDbConn *conn;
	const gchar *params[1];
	gint64 on_disk = 0;
	gboolean ok;
	guint i;

	/*
	 * No episode is read: the series' episodes are counted by state in the
	 * database (axis_tv_store_overview(): two statements whatever the
	 * library's size), and the episodes that air today are the first eight
	 * of that day, by the index on the air date.  (The dashboard reads this
	 * in the background, never on a page view: axis-web-dashboard.c.)
	 */
	axis_tv_get_rules(self, &rules);
	params[0] = rules.today;
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL)
		return NULL;
	ok = axis_tv_store_overview(self, conn, NULL, AXIS_TV_MEMBERS_OF_NONE, &series, &totals, NULL, &error) &&
		axis_db_conn_exec(conn, "SELECT s.id, t.title, t.year, e.season_number, e.number, e.title"
			" FROM series_episode e JOIN series s ON s.id = e.series_id JOIN meta_title t ON t.id = s.title_id"
			" WHERE e.air_date = $1::date AND e.gone_at IS NULL"
			" ORDER BY lower(t.title), t.year, s.id, e.season_number, e.number, e.id LIMIT 8", 1, params, &today,
			&error) &&
		/* The series on disk, each once: a linked unit with a video present. */
		axis_db_conn_exec(conn, "SELECT count(DISTINCT li.title_id) FROM library_identity li"
			" JOIN library_root r ON r.id = li.root_id AND r.media_type = 'tv'"
			" WHERE li.state = 'linked' AND li.title_id IS NOT NULL"
			" AND NOT EXISTS (SELECT 1 FROM series s WHERE s.title_id = li.title_id)"
			" AND EXISTS (SELECT 1 FROM library_identity_file lif JOIN library_file lf ON lf.id = lif.file_id"
			"  AND lf.state = 'present' WHERE lif.identity_id = li.id)", 0, NULL, &disk, &error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		g_debug("dashboard: cannot read the series: %s", error->message);
		return NULL;
	}
	axis_db_result_get_int64(disk, 0, 0, &on_disk);
	figures = g_new0(DashFigures, 1);
	figures->taken_us = rules.now_us;
	figures->series = series->len;
	figures->on_disk = (guint)MAX(on_disk, 0);
	figures->today = g_ptr_array_new_with_free_func(g_free);
	figures->today_href = g_ptr_array_new_with_free_func(g_free);
	memcpy(figures->counts, totals.counts, sizeof(figures->counts));
	memcpy(figures->kinds, totals.kinds, sizeof(figures->kinds));
	for (i = 0; i < axis_db_result_get_n_rows(today); i++) {
		const gchar *title = axis_db_result_get_text(today, i, 5);
		g_autofree gchar *label = NULL;
		g_autofree gchar *number = NULL;
		gint64 id = 0;
		gint64 year = 0;
		gint64 season = 0;
		gint64 episode = 0;

		axis_db_result_get_int64(today, i, 0, &id);
		axis_db_result_get_int64(today, i, 2, &year);
		axis_db_result_get_int64(today, i, 3, &season);
		axis_db_result_get_int64(today, i, 4, &episode);
		label = axis_tv_series_label(axis_db_result_get_text(today, i, 1), (gint)year);
		number = axis_tv_episode_label((gint)season, (gint)episode);
		g_ptr_array_add(figures->today, g_strdup_printf("%s %s%s%s", label, number, title != NULL ? ", " : "",
			title != NULL ? title : ""));
		g_ptr_array_add(figures->today_href, g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%d", id,
			(gint)season));
	}
	return figures;
}

static void
dash_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          data,
	gpointer          user_data
){
	DashFigures *figures = data;
	HtmxDiv *piece;
	HtmxP *head;
	g_autofree gchar *summary = NULL;
	guint i;

	if (figures == NULL || (figures->series == 0 && figures->on_disk == 0))
		return;
	if (figures->series == 0) {
		g_autofree gchar *count = axis_web_count(figures->on_disk, "series on disk", "series on disk");
		g_autofree gchar *text = g_strdup_printf("%s, none managed by Axis yet: nothing of them is searched for. Open "
			"one under TV to have Axis manage it.", count);

		piece = htmx_div_new();
		htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
		ADD(piece, axis_web_storage_text("card-foot", text));
		axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_COMING_IN, 25, "/series", HTMX_NODE(piece));
		return;
	}
	piece = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	head = htmx_p_new();
	htmx_element_add_class(HTMX_ELEMENT(head), "dash-subhead");
	htmx_node_set_text_content(HTMX_NODE(head), "Episodes airing today");
	ADD(piece, head);
	if (figures->today->len == 0) {
		ADD(piece, axis_web_storage_text("help", "No episode of your series airs today."));
	} else {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "space-list");
		for (i = 0; i < figures->today->len; i++) {
			HtmxLi *item = htmx_li_new();
			HtmxSpan *name = htmx_span_new();
			HtmxA *link = htmx_a_new_with_href(g_ptr_array_index(figures->today_href, i));
			HtmxBdi *text = htmx_bdi_new();

			htmx_element_add_class(HTMX_ELEMENT(item), "space-item");
			htmx_element_add_class(HTMX_ELEMENT(name), "space-name");
			htmx_node_set_text_content(HTMX_NODE(text), g_ptr_array_index(figures->today, i));
			ADD(link, text);
			ADD(name, link);
			ADD(item, name);
			ADD(list, item);
		}
		ADD(piece, list);
	}
	if (figures->on_disk > 0)
		summary = g_strdup_printf("%u series managed, and %u more on disk that Axis does not manage: %u episodes "
			"wanted, %u downloading, %u to import.", figures->series, figures->on_disk,
			figures->counts[AXIS_TV_WANTED] + figures->counts[AXIS_TV_SEARCHING], figures->counts[AXIS_TV_DOWNLOADING],
			figures->counts[AXIS_TV_TO_IMPORT]);
	else
		summary = g_strdup_printf("%u %s: %u episodes wanted, %u downloading, %u to import.", figures->series,
			figures->series == 1 ? "series" : "series", figures->counts[AXIS_TV_WANTED] +
			figures->counts[AXIS_TV_SEARCHING], figures->counts[AXIS_TV_DOWNLOADING],
			figures->counts[AXIS_TV_TO_IMPORT]);
	ADD(piece, axis_web_storage_text("card-foot", summary));
	ADD(piece, axis_web_dashboard_as_of("As of", figures->taken_us));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_COMING_IN, 25, "/series/calendar", HTMX_NODE(piece));
	/* The series that need attention are the attention source's to say
	 * (attention_source() below): the dashboard's card is drawn from it. */
}

/* ── The administrators' actions ────────────────────────────────── */

typedef enum
{
	ACT_ADD,
	ACT_SEARCH,
	ACT_MONITOR,
	ACT_GRAB,
	ACT_GIVE_UP,
	ACT_CLEAR,
	ACT_EDIT,
	ACT_SYNC,
	ACT_REMOVE,
	ACT_START,	/* "Let Axis search for it": ends the hold of a series migrated */
	ACT_KIND	/* marked as anime, or as standard again */
} Act;

typedef struct
{
	Act          act;
	gint64       series_id;
	gint64       title_id;
	gint64       arg;
	gint         season;
	gboolean     flag;
	AxisTvScope  scope;
	AxisTvSpec   spec;
	gchar       *user;
	gboolean     created;
	gboolean     manage;	/* an add from a series on disk's page */
	gboolean     on_disk;	/* removed, it is a series on disk again */
	gboolean     undated;	/* a search of a season's episodes with no air date */
	guint        queued;
	AxisTvAsked  asked;	/* a search: what came of it */
	gint64       episode;	/* an edit: the episode whose releases it came from */
	AxisTvEdited edited;
	gchar       *back;	/* (nullable) the page the form came from */
	gchar       *at;	/* (nullable) the place on it (web/axis-web-place.h) */
	GError      *error;
} ActWork;

static void
act_work_free(gpointer data)
{
	ActWork *work = data;

	g_free(work->user);
	g_free(work->back);
	g_free(work->at);
	g_clear_error(&work->error);
	g_free(work);
}

static void
act_run(
	AxisTv   *self,
	gpointer  data
){
	ActWork *work = data;
	g_autoptr(AxisTvSeries) series = NULL;

	work->spec.user = work->user;
	if (work->act == ACT_ADD) {
		axis_tv_add(self, &work->spec, &work->series_id, &work->error);
		return;
	}
	if (!axis_tv_get(self, work->series_id, &series, NULL, &work->error))
		return;
	work->title_id = series->title_id;
	switch (work->act) {
	case ACT_SEARCH:
		/* A person's: nothing new when the work is in hand or was just done. */
		if (work->undated)
			axis_tv_ask_undated(self, work->series_id, work->season, work->user, &work->asked, &work->queued,
				&work->error);
		else
			axis_tv_ask_search(self, work->series_id, work->scope, work->scope == AXIS_TV_SCOPE_SERIES ? 0 :
				work->arg, FALSE, work->user, &work->asked, &work->queued, &work->error);
		break;
	case ACT_MONITOR:
		if (work->arg > 0)
			axis_tv_set_episode_monitored(self, work->series_id, work->arg, work->flag, work->user, &work->error);
		else
			axis_tv_set_season_monitored(self, work->series_id, work->season, work->flag, work->user, &work->error);
		break;
	case ACT_GRAB:
		axis_tv_grab(self, work->series_id, work->arg, work->user, NULL, &work->error);
		break;
	case ACT_GIVE_UP:
		axis_tv_give_up(self, work->series_id, work->arg, work->user, &work->error);
		break;
	case ACT_CLEAR:
		axis_tv_clear_blocklist(self, work->series_id, work->arg, work->user, NULL, &work->error);
		break;
	case ACT_EDIT:
		axis_tv_edit_full(self, work->series_id, &work->spec, work->episode, work->season, &work->edited,
			&work->error);
		break;
	case ACT_SYNC:
		axis_tv_sync(self, work->series_id, TRUE, NULL, &work->error);
		break;
	case ACT_START:
		axis_tv_start_searching(self, work->series_id, work->flag, work->user, &work->created, &work->error);
		break;
	case ACT_KIND:
		axis_tv_set_kind(self, work->series_id, work->flag, work->user, &work->created, &work->error);
		break;
	default:
		/* No file is touched; whether it is a series on disk again says where
		 * the administrator goes next. */
		if (axis_tv_remove(self, work->series_id, work->user, &work->error)) {
			AxisDbConn *conn = axis_db_pool_acquire(self->pool, NULL);

			if (conn != NULL) {
				axis_tv_store_is_on_disk(conn, work->title_id, &work->on_disk, NULL);
				axis_db_pool_release(self->pool, conn);
			}
		}
		break;
	}
}

static const gchar *
act_heading(Act act)
{
	static const gchar *const headings[] = {
		"Not added", "Not searched", "Not changed", "Not grabbed", "Not given up", "Not cleared", "Not saved",
		"Not updated", "Not removed", "Not started", "Not changed"
	};

	return (guint)act < G_N_ELEMENTS(headings) ? headings[act] : "Not done";
}

static const gchar *
act_name(Act act)
{
	static const gchar *const names[] = {
		"added", "search asked for", "monitoring changed", "grab asked for", "given up", "blocklist cleared",
		"changed", "synced", "removed", "searching started", "kind changed"
	};

	return (guint)act < G_N_ELEMENTS(names) ? names[act] : "acted on";
}

static HtmxResponse *
act_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	ActWork *work = data;
	g_autofree gchar *message = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *season = NULL;
	g_autofree gchar *place = NULL;
	const gchar *key;

	if (work->error != NULL) {
		guint status;

		if (work->error->domain != AXIS_TV_ERROR)
			return axis_web_internal_error(request, work->error);
		switch (work->error->code) {
		case AXIS_TV_ERROR_NOT_FOUND:
			status = 404;
			break;
		case AXIS_TV_ERROR_INVALID:
			status = 400;
			break;
		case AXIS_TV_ERROR_FAILED:
			status = 502;
			break;
		default:
			status = 409;
			break;
		}
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, status, act_heading(work->act), message);
	}
	axis_web_storage_audit(request, "series %" G_GINT64_FORMAT " %s", work->series_id, act_name(work->act));
	switch (work->act) {
	case ACT_ADD:
		key = work->manage ? "managed" : "added";
		/* Added from another page (a Discover card): back to it, at the card. */
		if (work->back != NULL)
			return axis_web_place_redirect(request, work->back, key, work->at);
		break;
	case ACT_SEARCH:
		switch (work->asked) {
		case AXIS_TV_ASKED_QUEUED:
			key = work->undated ? "searching_undated" : "searching";
			break;
		case AXIS_TV_ASKED_IN_HAND:
			key = "already_searching";
			break;
		case AXIS_TV_ASKED_RECENT:
			key = "searched_recently";
			break;
		default:
			key = "nothing_wanted";
			break;
		}
		/* Back at the row or card whose button it was. */
		if (work->undated || work->scope == AXIS_TV_SCOPE_SEASON)
			place = g_strdup_printf("season-%d", work->season);
		else if (work->scope == AXIS_TV_SCOPE_EPISODE)
			place = g_strdup_printf("episode-%" G_GINT64_FORMAT, work->arg);
		else
			place = g_strdup("settings");
		break;
	case ACT_MONITOR:
		key = "monitored";
		place = work->arg > 0 ? g_strdup_printf("episode-%" G_GINT64_FORMAT, work->arg) :
			g_strdup_printf("season-%d", work->season);
		break;
	case ACT_GRAB:
		key = "grabbed";
		break;
	case ACT_GIVE_UP:
		key = "given_up";
		break;
	case ACT_CLEAR:
		key = "cleared";
		break;
	case ACT_EDIT:
		/* A new profile searches again: the notice says so, with the name. */
		key = !work->edited.profile_changed ? "saved" : work->edited.searches > 0 ? "profile" : "profile_none";
		place = g_strdup("settings");
		break;
	case ACT_SYNC:
		key = "synced";
		place = g_strdup("settings");
		break;
	case ACT_START:
		key = work->created ? "started" : "not_held";
		place = g_strdup("settings");
		break;
	case ACT_KIND:
		key = !work->created ? "kind_same" : work->flag ? "anime" : "not_anime";
		place = g_strdup("settings");
		break;
	default:
		if (work->on_disk) {
			location = g_strdup_printf("/series/on-disk/%" G_GINT64_FORMAT "?done=unmanaged", work->title_id);
			return axis_web_auth_redirect(request, location);
		}
		return axis_web_auth_redirect(request, "/series?done=removed");
	}
	/* Posted from another page of the series (its releases): back to it, at
	 * the place the form named there. */
	if (work->back != NULL)
		return axis_web_place_redirect(request, work->back, key, work->at);
	if (work->season >= 0)
		season = g_strdup_printf("&season=%d", work->season);
	location = g_strdup_printf("/series/%" G_GINT64_FORMAT "?done=%s%s", work->series_id, key,
		season != NULL ? season : "");
	return axis_web_place_redirect(request, location, NULL, place);
}

/* A whole number from the form (1 to 18 digits); 0 when missing. */
static gint64
form_id(
	HtmxRequest *request,
	const gchar *name
){
	guint64 value = 0;

	if (!axis_web_storage_form_u64(request, name, &value) || value == 0 || value > (guint64)G_MAXINT64)
		return 0;
	return (gint64)value;
}

/* A season's number from the form (0 to 10000), -1 when missing. */
static gint
form_season(
	HtmxRequest *request,
	const gchar *name
){
	const gchar *text = htmx_request_get_form_value(request, name);
	guint64 value = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 0, 10000, &value, NULL))
		return -1;
	return (gint)value;
}

static HtmxResponse *
start_act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	Act          act
){
	AxisTv *self = user_data;
	ActWork *work = g_new0(ActWork, 1);

	work->act = act;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->season = -1;
	if (act != ACT_ADD) {
		work->series_id = axis_web_storage_param_id(params);
		if (work->series_id <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
		}
		/* A form drawn on another page of the series comes back to it. */
		work->back = axis_web_back_posted(request);
	}
	{
		const gchar *at = htmx_request_get_form_value(request, AXIS_WEB_PLACE_AT);

		work->at = axis_web_place_id_valid(at) ? g_strdup(at) : NULL;
	}
	switch (act) {
	case ACT_ADD: {
		const gchar *monitor = htmx_request_get_form_value(request, "monitor");

		work->spec.title_id = form_id(request, "title");
		work->spec.profile_id = form_id(request, "profile");
		work->spec.root_id = form_id(request, "root");
		work->spec.specials = axis_web_storage_form_yes(request, "specials");
		work->spec.search_now = axis_web_storage_form_yes(request, "search");
		/* Anime only when the form's box was ticked: nothing else sets it. */
		work->spec.anime = axis_web_storage_form_yes(request, "anime");
		/* Managing a series on disk searches for nothing. */
		work->manage = axis_web_storage_form_yes(request, "manage");
		if (work->manage)
			work->spec.search_now = FALSE;
		if (work->spec.title_id <= 0 || monitor == NULL || !axis_tv_monitor_from_name(monitor, &work->spec.monitor)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not added", "The form is not complete.");
		}
		work->back = axis_web_back_posted(request);
		break;
	}
	case ACT_EDIT:
		work->spec.profile_id = form_id(request, "profile");
		work->spec.root_id = form_id(request, "root");
		/* From an episode's or a season's releases: that is searched for too
		 * with a new profile. */
		work->episode = form_id(request, "episode");
		work->season = work->episode > 0 ? -1 : form_season(request, "season");
		break;
	case ACT_SEARCH: {
		const gchar *scope = htmx_request_get_form_value(request, "scope");

		/* The episodes of a season with no air date, each by itself. */
		if (g_strcmp0(scope, "undated") == 0) {
			work->undated = TRUE;
			work->season = form_season(request, "item");
			if (work->season < 0) {
				act_work_free(work);
				return axis_web_error_respond(request, 400, "Not searched", "Say which season.");
			}
			break;
		}
		if (scope == NULL || !axis_tv_scope_from_name(scope, &work->scope)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not searched", "Say what to search for.");
		}
		if (work->scope == AXIS_TV_SCOPE_EPISODE) {
			work->arg = form_id(request, "item");
			work->season = form_season(request, "season");
		} else if (work->scope == AXIS_TV_SCOPE_SEASON) {
			work->season = form_season(request, "item");
			work->arg = work->season;
		}
		if ((work->scope == AXIS_TV_SCOPE_EPISODE && work->arg <= 0) ||
		    (work->scope == AXIS_TV_SCOPE_SEASON && work->season < 0)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not searched", "Say what to search for.");
		}
		break;
	}
	case ACT_MONITOR:
		work->arg = form_id(request, "episode");
		work->season = form_season(request, "season");
		work->flag = axis_web_storage_form_yes(request, "monitored");
		if (work->arg <= 0 && work->season < 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not changed", "Say which season or episode.");
		}
		break;
	case ACT_GRAB:
		work->arg = form_id(request, "candidate");
		if (work->arg <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not grabbed", "Choose a release.");
		}
		break;
	case ACT_GIVE_UP:
		work->arg = form_id(request, "grab");
		if (work->arg <= 0 || !axis_web_storage_form_yes(request, "confirm")) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, act_heading(act), "Tick the box to confirm.");
		}
		break;
	case ACT_CLEAR:
		work->arg = form_id(request, "entry");
		break;
	case ACT_START:
		/* The confirmation's tick: also the episodes Sonarr has files for
		 * that Axis does not see.  Without it they stay held. */
		work->flag = axis_web_storage_form_yes(request, "unseen");
		break;
	case ACT_KIND:
		/* The box of the series' settings card: ticked, anime. */
		work->flag = axis_web_storage_form_yes(request, "anime");
		break;
	case ACT_REMOVE:
		if (!axis_web_storage_form_yes(request, "confirm")) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, act_heading(act), "Tick the box to confirm.");
		}
		break;
	default:
		break;
	}
	return axis_tv_web_dispatch(self, request, act_run, act_done, work, act_work_free);
}

static HtmxResponse *
handle_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_ADD);
}

static HtmxResponse *
handle_search(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_SEARCH);
}

static HtmxResponse *
handle_monitor(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_MONITOR);
}

static HtmxResponse *
handle_grab(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_GRAB);
}

static HtmxResponse *
handle_give_up(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_GIVE_UP);
}

static HtmxResponse *
handle_clear(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_CLEAR);
}

static HtmxResponse *
handle_edit(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_EDIT);
}

static HtmxResponse *
handle_sync(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_SYNC);
}

static HtmxResponse *
handle_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_REMOVE);
}

/* POST /series/:id/kind (anime=yes, or without it): an administrator marks
 * the series as anime, or as standard again.  One write and one history
 * line; nothing else of the series changes. */
static HtmxResponse *
handle_kind(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_KIND);
}

/* POST /series/:id/start-searching: "Let Axis search for it", from its
 * confirmation (GET of the same address, in axis-tv-web-view.c). */
static HtmxResponse *
handle_start(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_START);
}

/* ── A manual search (web/axis-web-manual.c) ────────────────────── */

static const AxisWebManualLevel manual_levels[] = {
	{ "series", FALSE },
	{ "season", TRUE },
	{ "episode", TRUE },
	{ NULL, FALSE }
};

static gboolean
manual_hold(gpointer data)
{
	AxisTv *self = data;

	if (self->stopping || self->pending >= AXIS_TV_MAX_PENDING)
		return FALSE;
	axis_tv_ref(self);
	self->pending++;
	return TRUE;
}

static void
manual_release(gpointer data)
{
	AxisTv *self = data;

	self->pending--;
	axis_tv_unref(self);
}

static guint
manual_status(const GError *error)
{
	if (error->domain != AXIS_TV_ERROR)
		return 0;
	switch (error->code) {
	case AXIS_TV_ERROR_NOT_FOUND:
		return 404;
	case AXIS_TV_ERROR_INVALID:
		return 400;
	case AXIS_TV_ERROR_FAILED:
		return 502;
	default:
		return 409;
	}
}

static AxisTvScope
manual_scope(const AxisWebManualTarget *target)
{
	AxisTvScope scope = AXIS_TV_SCOPE_SERIES;

	axis_tv_scope_from_name(target->level->name, &scope);
	return scope;
}

static gboolean
manual_read(
	gpointer                   data,
	const AxisWebManualTarget *target,
	AxisWebManualRead         *read,
	GError                   **error
){
	AxisTv *self = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(AxisTvReleases) releases = NULL;
	AxisTvScope scope = manual_scope(target);
	gint64 id = target->owner_id;
	guint i;

	if (!axis_tv_get(self, id, &series, &episodes, error))
		return FALSE;
	read->title = axis_tv_series_label(series->title, series->year);
	if (scope == AXIS_TV_SCOPE_EPISODE) {
		for (i = 0; i < episodes->len && read->what == NULL; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
			g_autofree gchar *label = NULL;

			if (e->id != target->item || e->gone)
				continue;
			label = axis_tv_episode_label(e->season, e->number);
			read->what = e->title != NULL ? g_strdup_printf("%s, %s", label, e->title) : g_strdup(label);
			read->back = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%d#episode-%" G_GINT64_FORMAT, id,
				e->season, e->id);
			read->automatic = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT, id,
				e->id);
		}
	} else if (scope == AXIS_TV_SCOPE_SEASON) {
		for (i = 0; i < episodes->len && read->what == NULL; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

			if (e->season != target->item || e->gone)
				continue;
			read->what = target->item == 0 ? g_strdup("the specials") : g_strdup_printf("season %" G_GINT64_FORMAT,
				target->item);
			read->back = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%" G_GINT64_FORMAT, id, target->item);
			read->automatic = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?season=%" G_GINT64_FORMAT, id,
				target->item);
		}
	} else {
		read->what = g_strdup("the whole series");
		read->back = g_strdup_printf("/series/%" G_GINT64_FORMAT, id);
	}
	if (read->what == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, scope == AXIS_TV_SCOPE_EPISODE ?
			"the series has no such episode" : "the series has no such season");
		return FALSE;
	}
	read->blocklist = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases#blocklist", id);
	if (!axis_tv_get_manual(self, id, scope, scope == AXIS_TV_SCOPE_SERIES ? 0 : target->item, &releases,
	        &read->running, error))
		return FALSE;
	read->search = g_steal_pointer(&releases->search);
	read->candidates = g_steal_pointer(&releases->candidates);
	read->holds = g_steal_pointer(&releases->holds);
	/* Where the one in hand stands. */
	{
		g_autoptr(AxisTvWork) work = NULL;
		const AxisJobLive *job;

		if (!axis_tv_get_work(self, id, 0, &work, error))
			return FALSE;
		job = axis_tv_work_in_hand(work, id, scope, scope == AXIS_TV_SCOPE_SERIES ? 0 : target->item, TRUE);
		if (job != NULL)
			read->state = axis_web_activity_state(job, work->now_us);
	}
	return TRUE;
}

static gboolean
manual_search(
	gpointer                   data,
	const AxisWebManualTarget *target,
	const gchar               *user,
	gboolean                  *created,
	GError                   **error
){
	AxisTvScope scope = manual_scope(target);
	AxisTvAsked asked = AXIS_TV_ASKED_NOTHING;

	/* Nothing new while one of it or of a wider level is in hand, or one
	 * ended a moment ago. */
	*created = FALSE;
	if (!axis_tv_ask_search(data, target->owner_id, scope, scope == AXIS_TV_SCOPE_SERIES ? 0 : target->item, TRUE,
	        user, &asked, NULL, error))
		return FALSE;
	*created = asked == AXIS_TV_ASKED_QUEUED;
	return TRUE;
}

static gboolean
manual_preview(
	gpointer                   data,
	const AxisWebManualTarget *target,
	gint64                     candidate_id,
	AxisAcquireHandPreview   **preview,
	GError                   **error
){
	return axis_tv_hand_preview(data, target->owner_id, candidate_id, preview, error);
}

static gboolean
manual_grab(
	gpointer                    data,
	const AxisWebManualTarget  *target,
	gint64                      candidate_id,
	const AxisAcquireHandTicks *ticks,
	const gchar                *user,
	AxisAcquireHandGrabbed     *grabbed,
	AxisAcquireGrabResult      *result,
	GError                    **error
){
	return axis_tv_grab_by_hand(data, target->owner_id, candidate_id, ticks, user, grabbed, result, error);
}

/* What the person named on a force confirmation, as the series takes it. */
static void
manual_pick(
	const AxisWebManualTarget *target,
	AxisTvForcePick           *pick
){
	memset(pick, 0, sizeof(*pick));
	pick->named = target->named;
	pick->episodes = target->picked;
	pick->whole_season = target->whole;
	pick->season = (gint)CLAMP(target->part, 0, 100000);
}

static gboolean
manual_force_preview(
	gpointer                   data,
	const AxisWebManualTarget *target,
	gint64                     candidate_id,
	AxisAcquireHandPreview   **preview,
	GError                   **error
){
	AxisTvForcePick pick;

	manual_pick(target, &pick);
	return axis_tv_force_preview(data, target->owner_id, candidate_id, &pick, preview, error);
}

static gboolean
manual_force_grab(
	gpointer                     data,
	const AxisWebManualTarget   *target,
	gint64                       candidate_id,
	const AxisAcquireForceTicks *ticks,
	const gchar                 *user,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result,
	GError                     **error
){
	AxisTvForcePick pick;

	manual_pick(target, &pick);
	return axis_tv_grab_by_force(data, target->owner_id, candidate_id, &pick, ticks, user, grabbed, result, error);
}

/* Block this release (add-block-release). */
static gboolean
manual_block(
	gpointer                   data,
	const AxisWebManualTarget *target,
	gint64                     candidate_id,
	const gchar               *user,
	GError                   **error
){
	return axis_tv_block_candidate(data, target->owner_id, candidate_id, user, error);
}

static const AxisWebManualInfo manual_info = {
	"/series",
	"series",
	manual_levels,
	manual_hold,
	manual_release,
	manual_status,
	manual_read,
	manual_search,
	manual_preview,
	manual_grab,
	/* Grab it by force, for the episodes or the season a person names
	 * (add-force-grab-and-assign, F2). */
	manual_force_preview,
	manual_force_grab,
	manual_block
};

/* ── Block and search again (web/axis-web-block.c) ──────────────── */

static gboolean
block_find(
	gpointer           data,
	gint64             grab_id,
	gint64             download_id,
	gint64             import_id,
	AxisWebBlockRead  *read,
	GError           **error
){
	g_autoptr(AxisTvSeries) series = NULL;

	if (!axis_tv_block_find(data, grab_id, download_id, import_id, &read->grab, &series, error))
		return FALSE;
	read->title = axis_tv_series_label(series->title, series->year);
	read->back = g_strdup_printf("/series/%" G_GINT64_FORMAT, series->id);
	read->blocklist = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases#blocklist", series->id);
	return TRUE;
}

static gboolean
block_grab(
	gpointer      data,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
){
	return axis_tv_block_grab(data, owner_id, grab_id, user, cancelled, error);
}

static const AxisWebBlockInfo block_info = {
	"/series",
	"series",
	manual_hold,
	manual_release,
	manual_status,
	block_find,
	block_grab
};

/* ── Kept by hand ───────────────────────────────────────────────── */

typedef struct
{
	AxisTv       *self;
	HtmxRequest  *request;
	HtmxResponse *deferred;
	gint64        series_id;
	gint64        file_id;
	gboolean      kept;
	gchar        *user;
	gchar        *back;
	GError       *error;
} KeepWork;

static void
keep_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	KeepWork *work = task_data;

	axis_tv_set_kept(work->self, work->series_id, work->file_id, work->kept, work->user, &work->error);
	g_task_return_boolean(task, TRUE);
}

static void
keep_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	KeepWork *work = user_data;
	HtmxResponse *response;

	if (work->error != NULL) {
		guint status = manual_status(work->error);
		g_autofree gchar *message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]),
			work->error->message + 1);

		response = status == 0 ? axis_web_internal_error(work->request, work->error) :
			axis_web_error_respond(work->request, status, "Not changed", message);
	} else {
		axis_web_storage_audit(work->request, "series %" G_GINT64_FORMAT " file %" G_GINT64_FORMAT " %s",
			work->series_id, work->file_id, work->kept ? "kept by hand" : "no longer kept by hand");
		response = axis_web_place_redirect(work->request, work->back, work->kept ? "kept" : "let_go", NULL);
	}
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	manual_release(work->self);
	g_object_unref(work->request);
	g_object_unref(work->deferred);
	g_free(work->user);
	g_free(work->back);
	g_clear_error(&work->error);
	g_free(work);
}

/* POST /series/:id/links/keep (file, keep=yes|no, season): an administrator
 * sets or clears "kept by hand" on one file of the series. */
static HtmxResponse *
handle_keep(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisTv *self = user_data;
	const gchar *file = htmx_request_get_form_value(request, "file");
	const gchar *keep = htmx_request_get_form_value(request, "keep");
	const gchar *season = htmx_request_get_form_value(request, "season");
	guint64 file_id = 0;
	guint64 season_number = 0;
	HtmxResponse *returned;
	KeepWork *work;
	GTask *task;

	if (axis_web_storage_param_id(params) <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (file == NULL || !g_ascii_string_to_unsigned(file, 10, 1, G_MAXINT64, &file_id, NULL) ||
	    (g_strcmp0(keep, "yes") != 0 && g_strcmp0(keep, "no") != 0))
		return axis_web_error_respond(request, 400, "Not changed", "Say which file, and whether it is kept by hand.");
	if (!manual_hold(self)) {
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	work = g_new0(KeepWork, 1);
	work->self = self;
	work->request = g_object_ref(request);
	work->series_id = axis_web_storage_param_id(params);
	work->file_id = (gint64)file_id;
	work->kept = g_strcmp0(keep, "yes") == 0;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->back = season != NULL && g_ascii_string_to_unsigned(season, 10, 0, 100000, &season_number, NULL) ?
		g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%" G_GUINT64_FORMAT, work->series_id, season_number) :
		g_strdup_printf("/series/%" G_GINT64_FORMAT, work->series_id);
	work->deferred = htmx_response_new_deferred();
	returned = g_object_ref(work->deferred);
	task = g_task_new(NULL, NULL, keep_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, keep_thread);
	g_object_unref(task);
	return returned;
}

/* ── Where the person's Adds from a list go ─────────────────────── */

typedef struct
{
	gint64         user_id;
	gint64         root_id;
	gint64         profile_id;
	AxisTvMonitor  monitor;
	AxisTvChoices  choices;
	gchar         *all;
	const gchar   *refused;
	GError        *error;
} DefaultsWork;

static void
defaults_work_free(gpointer data)
{
	DefaultsWork *work = data;

	axis_tv_choices_clear(&work->choices);
	g_free(work->all);
	g_clear_error(&work->error);
	g_free(work);
}

static void
defaults_run(
	AxisTv   *self,
	gpointer  data
){
	DefaultsWork *work = data;
	g_autofree gchar *values = NULL;

	if (!axis_tv_web_read_choices(self, &work->choices, &work->error))
		return;
	/* Only a series folder on a disk Axis can write to now, and a profile
	 * that can be used: as the cards' own Options offer them. */
	if (usable_choice(work->choices.roots, work->root_id) == NULL) {
		work->refused = "That folder cannot be chosen: it is not a series folder, or its disk is away or mounted "
			"read-only now.";
		return;
	}
	if (usable_choice(work->choices.profiles, work->profile_id) == NULL) {
		work->refused = "That quality profile cannot be chosen.";
		return;
	}
	values = g_strdup_printf("{\"root\": %" G_GINT64_FORMAT ", \"profile\": %" G_GINT64_FORMAT ", \"monitor\": \"%s\"}",
		work->root_id, work->profile_id, axis_tv_monitor_name(work->monitor));
	axis_user_store_set_add_defaults(self->pool, work->user_id, AXIS_TV_WEB_DEFAULTS_KIND, values, &work->all,
		&work->error);
}

static HtmxResponse *
defaults_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	DefaultsWork *work = data;
	g_autofree gchar *back = NULL;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->refused != NULL)
		return axis_web_error_respond(request, 409, "Not chosen", work->refused);
	axis_web_request_note_add_defaults(request, work->all);
	axis_web_storage_audit(request, "series added from a list go to root %" G_GINT64_FORMAT " with profile %"
		G_GINT64_FORMAT, work->root_id, work->profile_id);
	/* With htmx: the bar and every card's Add, out of band; the page stays
	 * where it is.  Without: back to the list, at the bar. */
	if (htmx_request_is_htmx(request))
		return axis_tv_web_defaults_answer(request, &work->choices);
	back = axis_web_back_posted(request);
	return axis_web_place_redirect(request, back != NULL ? back : "/discover/tv", "add_defaults",
		AXIS_TV_WEB_DEFAULTS_BAR);
}

static HtmxResponse *
handle_defaults(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	DefaultsWork *work = g_new0(DefaultsWork, 1);
	const gchar *monitor = htmx_request_get_form_value(request, "monitor");

	work->user_id = axis_web_request_get_user_id(request);
	work->root_id = form_id(request, "root");
	work->profile_id = form_id(request, "profile");
	if (monitor == NULL || !axis_tv_monitor_from_name(monitor, &work->monitor)) {
		defaults_work_free(work);
		return axis_web_error_respond(request, 400, "Not chosen", "Say which episodes to monitor.");
	}
	return axis_tv_web_dispatch(user_data, request, defaults_run, defaults_done, work, defaults_work_free);
}

/* ── The Activity page's words ──────────────────────────────────── */

static gint64
payload_int(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return json_node_get_int(node);
}

static const gchar *
payload_text(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

/* "Searching for Lantern Bay (2025), season 1": the series and what of it,
 * read for all the searches at once (two statements). */
static void
activity_words(
	GPtrArray *items,
	gpointer   user_data
){
	AxisTv *self = user_data;
	g_autoptr(GString) series_ids = g_string_new("{");
	g_autoptr(GString) episode_ids = g_string_new("{");
	g_autoptr(AxisDbResult) series = NULL;
	g_autoptr(AxisDbResult) episodes = NULL;
	const gchar *params[1];
	guint n_series = 0;
	guint n_episodes = 0;
	guint i;
	guint r;

	for (i = 0; i < items->len; i++) {
		const AxisWebActivityItem *item = g_ptr_array_index(items, i);
		AxisTvScope scope = AXIS_TV_SCOPE_SERIES;

		g_string_append_printf(series_ids, "%s%" G_GINT64_FORMAT, n_series++ > 0 ? "," : "",
			payload_int(item->payload, "series"));
		if (axis_tv_scope_from_name(payload_text(item->payload, "scope"), &scope) && scope == AXIS_TV_SCOPE_EPISODE)
			g_string_append_printf(episode_ids, "%s%" G_GINT64_FORMAT, n_episodes++ > 0 ? "," : "",
				payload_int(item->payload, "item"));
	}
	g_string_append_c(series_ids, '}');
	g_string_append_c(episode_ids, '}');
	params[0] = series_ids->str;
	if (!axis_db_pool_exec(self->pool, "SELECT s.id, t.title, t.year FROM series s JOIN meta_title t ON t.id ="
	        " s.title_id WHERE s.id = ANY ($1::bigint[])", 1, params, &series, NULL))
		return;
	params[0] = episode_ids->str;
	if (!axis_db_pool_exec(self->pool, "SELECT id, season_number, number FROM series_episode"
	        " WHERE id = ANY ($1::bigint[])", 1, params, &episodes, NULL))
		return;
	for (i = 0; i < items->len; i++) {
		AxisWebActivityItem *item = g_ptr_array_index(items, i);
		gint64 series_id = payload_int(item->payload, "series");
		gint64 target = payload_int(item->payload, "item");
		gboolean list = g_strcmp0(payload_text(item->payload, "cause"), "list") == 0;
		AxisTvScope scope = AXIS_TV_SCOPE_SERIES;
		g_autofree gchar *label = NULL;
		g_autofree gchar *what = NULL;

		axis_tv_scope_from_name(payload_text(item->payload, "scope"), &scope);
		for (r = 0; r < axis_db_result_get_n_rows(series) && label == NULL; r++) {
			gint64 id = 0;
			gint64 year = 0;

			axis_db_result_get_int64(series, r, 0, &id);
			axis_db_result_get_int64(series, r, 2, &year);
			if (id == series_id)
				label = axis_tv_series_label(axis_db_result_get_text(series, r, 1), (gint)year);
		}
		if (label == NULL)
			continue;
		if (scope == AXIS_TV_SCOPE_EPISODE) {
			for (r = 0; r < axis_db_result_get_n_rows(episodes) && what == NULL; r++) {
				gint64 id = 0;
				gint64 season = 0;
				gint64 number = 0;

				axis_db_result_get_int64(episodes, r, 0, &id);
				axis_db_result_get_int64(episodes, r, 1, &season);
				axis_db_result_get_int64(episodes, r, 2, &number);
				if (id != target)
					continue;
				what = axis_tv_episode_label((gint)season, (gint)number);
				item->href = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%" G_GINT64_FORMAT "#episode-%"
					G_GINT64_FORMAT, series_id, season, id);
			}
		} else if (scope == AXIS_TV_SCOPE_SEASON) {
			what = target == 0 ? g_strdup("the specials") : g_strdup_printf("season %" G_GINT64_FORMAT, target);
			item->href = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=%" G_GINT64_FORMAT "#season-%"
				G_GINT64_FORMAT, series_id, target, target);
		} else {
			what = g_strdup(list ? "all of it" : "every wanted episode");
		}
		if (item->href == NULL)
			item->href = g_strdup_printf("/series/%" G_GINT64_FORMAT, series_id);
		item->words = g_strdup_printf("%s %s, %s", list ? "Manual search of" : "Searching for", label,
			what != NULL ? what : "an episode");
	}
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * The series' notice (add-attention-notices): how many series have an
 * episode that needs attention for a reason of its own
 * (axis_acquire_attention_is_own()), with the most common reason, as one line
 * that names no series.  Only a person ends these, so there is no wait.  A
 * release only a person can grab leaves its episode here, as "the profile
 * rejected what was found".  "newest" is the highest id among the series
 * counted.
 *
 * Why this is not one counting statement, as the movies' is: an episode's
 * state exists only in C.  axis_tv_episode_derive_attention() works it out
 * from the facts axis_tv_store_episodes() reads (its grab, its download, its
 * import, its files and their quality against the profile's cutoff, when it
 * aired by the series' rules, the search that last covered it), and the TV
 * pages, Wanted among them, show what that function says; there is no twin of
 * it in SQL, as the movies' list has, and one written for this count alone
 * would be a second definition of "needs attention" that could come to
 * differ from the page's.  So the same function is used, and the reading is
 * bounded another way:
 *
 *   - the series are gone through ATTENTION_BATCH at a time, by id; a batch
 *     is two statements (its series, their episodes), each cancelled by the
 *     server after four times the reading's deadline;
 *   - a reading goes through batches only until the reading's deadline has
 *     passed (at least one), and the next reading goes on where it stopped:
 *     a count takes as many readings as it needs, and the notice is the last
 *     finished count's until it is done;
 *   - a count begins only when axis_attention_titles_due() says so: after a
 *     search or an import ended, never sooner than five minutes after the
 *     count before was finished, and every ten minutes when nothing ended.
 */
#define ATTENTION_BATCH 50

/* As the movies': a search and an import, not tv.follow or tv.schedule. */
static const gchar *const attention_kinds[] = { AXIS_TV_SEARCH_KIND, AXIS_IMPORT_KIND, NULL };

/* One batch of the count under way.  *@done: there is no series after it. */
static gboolean
attention_batch(
	AxisTv    *self,
	guint      statement_ms,
	gboolean  *done,
	GError   **error
){
	g_autoptr(AxisDbResult) ids = NULL;
	g_autoptr(GPtrArray) summaries = NULL;
	g_autoptr(GString) titles = g_string_new("{");
	g_autoptr(GHashTable) person_only = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *set = g_strdup_printf("SET LOCAL statement_timeout = '%ums'", MAX(statement_ms, 1));
	g_autofree gchar *after = axis_tv_int_text(self->attention_pass.after_id);
	g_autofree gchar *limit = g_strdup_printf("%d", ATTENTION_BATCH);
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok;
	guint n = 0;
	guint i;
	guint k;

	*done = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = after;
	params[1] = limit;
	ok = axis_db_conn_begin(conn, error);
	if (ok) {
		ok = axis_db_conn_exec(conn, set, 0, NULL, NULL, error) &&
			axis_db_conn_exec(conn, "SELECT id, title_id FROM series WHERE id > $1::bigint ORDER BY id"
				" LIMIT $2::integer", 2, params, &ids, error);
		if (ok) {
			n = axis_db_result_get_n_rows(ids);
			for (i = 0; i < n; i++)
				g_string_append_printf(titles, "%s%s", i > 0 ? "," : "", axis_db_result_get_text(ids, i, 1));
			g_string_append_c(titles, '}');
			ok = n == 0 || axis_tv_store_summaries(self, conn, AXIS_TV_SERIES_TITLES, titles->str, &summaries, error);
		}
		/* Which of the searches that left these episodes needing attention
		 * found a release only a person can grab (series_search.n_person_only,
		 * written with the search's candidates): one more small statement, by
		 * the primary key, only when the batch has such episodes at all. */
		if (ok) {
			g_autoptr(GString) searches = g_string_new("{");

			for (i = 0; summaries != NULL && i < summaries->len; i++) {
				const AxisTvSummary *summary = g_ptr_array_index(summaries, i);

				for (k = 0; summary->attention != NULL && k < summary->attention->len; k++) {
					const AxisTvAttentionItem *episode = g_ptr_array_index(summary->attention, k);

					if (episode->kind == AXIS_ACQUIRE_ATTENTION_REJECTED && episode->search_id > 0)
						g_string_append_printf(searches, "%s%" G_GINT64_FORMAT, searches->len > 1 ? "," : "",
							episode->search_id);
				}
			}
			if (searches->len > 1) {
				g_autoptr(AxisDbResult) found = NULL;
				const gchar *one[1];

				g_string_append_c(searches, '}');
				one[0] = searches->str;
				ok = axis_db_conn_exec(conn, "SELECT id FROM series_search WHERE id = ANY($1::bigint[])"
					" AND n_person_only > 0", 1, one, &found, error);
				for (i = 0; ok && i < axis_db_result_get_n_rows(found); i++) {
					gint64 *id = g_new0(gint64, 1);

					axis_db_result_get_int64(found, i, 0, id);
					g_hash_table_add(person_only, id);
				}
			}
		}
		ok = ok && axis_db_conn_commit(conn, error);
		if (!ok)
			axis_db_conn_rollback(conn, NULL);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;

	for (i = 0; summaries != NULL && i < summaries->len; i++) {
		const AxisTvSummary *summary = g_ptr_array_index(summaries, i);
		guint own[AXIS_ACQUIRE_ATTENTION_FAILED + 1] = { 0 };
		guint most = AXIS_ACQUIRE_ATTENTION_NONE;
		guint held = 0;

		/* The series' reason: the most common among its episodes'.  An
		 * episode whose search found a release only a person can grab is not
		 * one of them: it has its own line (one cause, one notice). */
		for (k = 0; summary->attention != NULL && k < summary->attention->len; k++) {
			const AxisTvAttentionItem *episode = g_ptr_array_index(summary->attention, k);

			if (episode->kind == AXIS_ACQUIRE_ATTENTION_REJECTED &&
			    g_hash_table_contains(person_only, &episode->search_id)) {
				held++;
				continue;
			}
			if ((guint)episode->kind < G_N_ELEMENTS(own) && axis_acquire_attention_is_own(episode->kind) &&
			    ++own[episode->kind] > own[most])
				most = episode->kind;
		}
		if (held > 0) {
			self->attention_pass.held++;
			self->attention_pass.held_episodes += held;
			self->attention_pass.held_newest = MAX(self->attention_pass.held_newest, summary->series->id);
		}
		if (most == AXIS_ACQUIRE_ATTENTION_NONE)
			continue;
		self->attention_pass.count++;
		self->attention_pass.reasons[most]++;
		self->attention_pass.newest = MAX(self->attention_pass.newest, summary->series->id);
	}
	for (i = 0; i < n; i++) {
		gint64 id = 0;

		axis_db_result_get_int64(ids, i, 0, &id);
		self->attention_pass.after_id = MAX(self->attention_pass.after_id, id);
	}
	*done = n < ATTENTION_BATCH;
	return TRUE;
}

static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisTv *self = user_data;
	AxisAttentionTitles *titles = &self->attention;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	/* Called at every reading: it also remembers the jobs that ended. */
	gboolean due = axis_attention_titles_due(titles, reading, attention_kinds);

	if (!self->attention_pass.running && due) {
		memset(&self->attention_pass, 0, sizeof(self->attention_pass));
		self->attention_pass.running = TRUE;
		/* What ends from here on is for the count after this one. */
		titles->dirty = FALSE;
	}
	if (self->attention_pass.running) {
		gint64 until = g_get_monotonic_time() + (gint64)reading->deadline_ms * 1000;
		gboolean done = FALSE;

		do {
			/* A batch that fails is gone through again at the next reading. */
			if (!attention_batch(self, reading->deadline_ms * 4, &done, error))
				return FALSE;
		} while (!done && g_get_monotonic_time() < until);
		if (done) {
			guint top = AXIS_ACQUIRE_ATTENTION_NONE;
			guint n_reasons = 0;
			guint i;

			for (i = AXIS_ACQUIRE_ATTENTION_NONE + 1; i < G_N_ELEMENTS(self->attention_pass.reasons); i++) {
				n_reasons += self->attention_pass.reasons[i] > 0 ? 1 : 0;
				if (self->attention_pass.reasons[i] > self->attention_pass.reasons[top])
					top = i;
			}
			titles->count = self->attention_pass.count;
			titles->newest = self->attention_pass.newest;
			titles->top = top;
			titles->n_reasons = n_reasons;
			titles->read_us = reading->now_us;
			titles->known = TRUE;
			self->attention_held = self->attention_pass.held;
			self->attention_held_episodes = self->attention_pass.held_episodes;
			self->attention_held_newest = self->attention_pass.held_newest;
			self->attention_pass.running = FALSE;
		}
	}

	/* The first count since the daemon started is not finished (it takes as
	 * many readings as it needs): neither line is known, which is not "no
	 * series needs attention", so what users dismissed before the start is
	 * kept until the count is done. */
	if (!titles->known) {
		axis_attention_not_known(items, "tv.held", NULL);
		axis_attention_not_known(items, "tv.attention", NULL);
		return TRUE;
	}

	/*
	 * The releases that wait for a person's "Grab this one" (slice N2): the
	 * series with an episode that needs attention because its last search
	 * found a release no search takes by itself (numbered beyond the series'
	 * data, in the place of a file kept by hand, an anime release whose
	 * absolute number is read too weakly or that was downloaded before
	 * without a file for an episode).  Their own line, and not counted in the
	 * line below.  "newest" is the highest id among the series, as below: a
	 * search tried again is the same occurrence.
	 */
	if (self->attention_held > 0) {
		g_autofree gchar *held = self->attention_held == 1 ?
			g_strdup_printf("1 series has %s that only a person can take: Axis found a release for %s and no search "
				"grabs it by itself. Open the series' manual search and select Grab this one.",
				self->attention_held_episodes == 1 ? "an episode" : "episodes",
				self->attention_held_episodes == 1 ? "it" : "them") :
			g_strdup_printf("%" G_GUINT64_FORMAT " series have episodes that only a person can take: Axis found "
				"releases for them and no search grabs them by itself. Open each series' manual search and select Grab "
				"this one.", self->attention_held);

		item = axis_attention_item_new("tv.held", "", AXIS_ATTENTION_WAITING, self->attention_held,
			self->attention_held_newest);
		axis_attention_item_say(item, held, NULL, NULL);
		/* The Wanted page's list of exactly these episodes (state=held), by
		 * the rule of this count. */
		axis_attention_item_link(item, "/series/wanted?state=held", "Open TV, Wanted");
		g_ptr_array_add(items, item);
	}

	if (titles->count == 0)
		return TRUE;
	if (titles->count == 1)
		text = g_strdup_printf("1 series needs attention because %s.",
			axis_acquire_attention_because((AxisAcquireAttention)titles->top));
	else
		text = g_strdup_printf("%" G_GUINT64_FORMAT " series need attention, %s because %s.", titles->count,
			titles->n_reasons > 1 ? "most often" : "each",
			axis_acquire_attention_because((AxisAcquireAttention)titles->top));
	item = axis_attention_item_new("tv.attention", "", AXIS_ATTENTION_WAITING, titles->count, titles->newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/series/wanted?state=attention", "Open TV, Wanted");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* ── The routes ─────────────────────────────────────────────────── */

void
axis_tv_web_register(
	AxisTv  *self,
	AxisWeb *web
){
	axis_web_add_title_actions(web, actions_collect, actions_render, action_figures_free, self);
	axis_web_add_dashboard_part(web, dash_collect, dash_render, dash_free, self);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_COMING_IN);
	axis_web_add_attention_source(web, "the series", attention_source, self);
	/* Before "/series/:id": the first route that matches answers. */
	axis_web_block_register(web, &block_info, self);
	axis_tv_web_register_pages(self, web);
	axis_tv_web_register_numbering(self, web);
	axis_tv_web_register_links(self, web);
	axis_web_manual_register(web, &manual_info, self);
	axis_web_add_activity_words(web, AXIS_TV_SEARCH_KIND, activity_words, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series", AXIS_WEB_ROUTE_ADMIN, handle_add, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/defaults", AXIS_WEB_ROUTE_ADMIN, handle_defaults, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/search", AXIS_WEB_ROUTE_ADMIN, handle_search, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/monitor", AXIS_WEB_ROUTE_ADMIN, handle_monitor, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/grab", AXIS_WEB_ROUTE_ADMIN, handle_grab, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/give-up", AXIS_WEB_ROUTE_ADMIN, handle_give_up, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/blocklist/clear", AXIS_WEB_ROUTE_ADMIN, handle_clear,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/edit", AXIS_WEB_ROUTE_ADMIN, handle_edit, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/sync", AXIS_WEB_ROUTE_ADMIN, handle_sync, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/remove", AXIS_WEB_ROUTE_ADMIN, handle_remove, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/start-searching", AXIS_WEB_ROUTE_ADMIN, handle_start, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/kind", AXIS_WEB_ROUTE_ADMIN, handle_kind, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/links/keep", AXIS_WEB_ROUTE_ADMIN, handle_keep, self);
}
