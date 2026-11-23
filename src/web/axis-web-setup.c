/*
 * axis-web-setup.c - the setup guide: its lifetime, the work it hands to
 * threads, the record of choices, and what each layer's check says
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Whether a step is done is read from the layer behind it every time, never
 * stored: the record holds only what an administrator chose (not now, not
 * used).  Each step's check is the layer's own:
 *
 *   titles    a provider of films (and series) has its key, and its last
 *             request did not fail (axis_acquire_check_ready()'s metadata
 *             item, and the provider's own state)
 *   library   a root for each media type that is on, on a volume that is
 *             online (the checklist's root items)
 *   indexers  a source switched on whose last contact worked, reporting at
 *             least one indexer that is on
 *   clients   a client switched on whose last contact worked, with a path
 *             mapping, and no folder it reported that maps nowhere
 *   quality   the default profile exists and its definition is usable
 *   players   a media server whose last look worked (optional)
 *   writing   storage.read_only is off (the checklist's last item)
 *   existing  every films root has been looked at once (optional)
 */

#include "web/axis-web-setup-private.h"

#include <string.h>

#include "import/axis-import.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef void (*WorkFunc)(AxisWebSetup *self, gpointer data);

typedef HtmxResponse *(*DoneFunc)(
	AxisWebSetup *self,
	HtmxRequest  *request,
	gpointer      data
);

typedef struct
{
	AxisWebSetup   *owner;
	WorkFunc        work;
	DoneFunc        done;
	HtmxRequest    *request;
	HtmxResponse   *deferred;
	gpointer        data;
	GDestroyNotify  free_data;
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
	htmx_response_complete(task->deferred, response);
	g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_object_unref(task->request);
	g_object_unref(task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebSetup   *self,
	HtmxRequest    *request,
	WorkFunc        work,
	DoneFunc        done,
	gpointer        data,
	GDestroyNotify  free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_WEB_SETUP_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		returned = axis_web_error_respond(request, 503, "Axis is busy", "Axis is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	task = g_new0(Task, 1);
	task->owner = self;
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

AxisModuleRegistry *
axis_web_setup_registry(AxisWebSetup *self)
{
	return self->web != NULL ? axis_web_get_registry(self->web) : NULL;
}

/* ── The record of choices ──────────────────────────────────────── */

gboolean
axis_web_setup_load_choices(
	AxisDbPool  *pool,
	guint       *skipped,
	guint       *dismissed,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	*skipped = 0;
	*dismissed = 0;
	if (!axis_db_pool_exec(pool, "SELECT step, choice FROM setup_step", 0, NULL, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *slug = axis_db_result_get_text(result, i, 0);
		const gchar *choice = axis_db_result_get_text(result, i, 1);
		AxisSetupStep step;

		if (!axis_setup_step_from_slug(slug, &step))
			continue;
		if (g_strcmp0(choice, "skipped") == 0)
			*skipped |= AXIS_SETUP_BIT(step);
		else if (g_strcmp0(choice, "dismissed") == 0)
			*dismissed |= AXIS_SETUP_BIT(step);
	}
	return TRUE;
}

/* @choice "skipped" or "dismissed", or %NULL to forget the step's. */
static gboolean
store_choice(
	AxisDbPool     *pool,
	AxisSetupStep   step,
	const gchar    *choice,
	const gchar    *user,
	GError        **error
){
	const gchar *params[3];

	params[0] = axis_setup_step_slug(step);
	if (choice == NULL)
		return axis_db_pool_exec(pool, "DELETE FROM setup_step WHERE step = $1", 1, params, NULL, error);
	params[1] = choice;
	params[2] = user;
	return axis_db_pool_exec(pool, "INSERT INTO setup_step (step, choice, changed_by) VALUES ($1, $2, $3)"
		" ON CONFLICT (step) DO UPDATE SET choice = excluded.choice, changed_by = excluded.changed_by,"
		" changed_at = now()", 3, params, NULL, error);
}

/* ── What a client's folders are for Axis ───────────────────────── */

static void
folder_free(AxisSetupFolder *folder)
{
	if (folder == NULL)
		return;
	g_free(folder->path);
	g_free(folder->rel_path);
	g_free(folder->problem);
	g_free(folder);
}

static gboolean
has_folder(
	GPtrArray   *folders,
	gint64       client_id,
	const gchar *path
){
	guint i;

	for (i = 0; i < folders->len; i++) {
		const AxisSetupFolder *folder = g_ptr_array_index(folders, i);

		if (folder->client_id == client_id && strcmp(folder->path, path) == 0)
			return TRUE;
	}
	return FALSE;
}

static guint
n_reported(
	GPtrArray *folders,
	gint64     client_id
){
	guint n = 0;
	guint i;

	for (i = 0; i < folders->len; i++) {
		const AxisSetupFolder *folder = g_ptr_array_index(folders, i);

		n += folder->client_id == client_id && folder->reported;
	}
	return n;
}

/*
 * The folders of every client: the prefix of each of its mappings, and the
 * folder each of its recent finished downloads was in, as the client said,
 * with where the layer's own mapping puts that download.
 */
static void
collect_folders(
	AxisWebSetup     *self,
	AxisSetupFigures *f
){
	g_autoptr(GPtrArray) arrived = NULL;
	g_autoptr(GPtrArray) active = NULL;
	GPtrArray *lists[2];
	guint i;
	guint l;

	f->folders = g_ptr_array_new_with_free_func((GDestroyNotify)folder_free);
	for (i = 0; i < f->clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(f->clients, i);
		guint m;

		for (m = 0; m < client->maps->len; m++) {
			const AxisDownloadPathMapInfo *map = g_ptr_array_index(client->maps, m);
			AxisSetupFolder *folder = g_new0(AxisSetupFolder, 1);

			folder->client_id = client->id;
			folder->path = g_strdup(map->client_prefix);
			folder->mapped = TRUE;
			folder->volume_id = map->volume_id;
			folder->rel_path = g_strdup(map->rel_path);
			g_ptr_array_add(f->folders, folder);
		}
	}
	/* Only what finished (a download in progress may be in the client's
	 * folder for unfinished ones, which needs no mapping). */
	if (!axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_ARRIVED, 50, &arrived, NULL))
		arrived = NULL;
	if (!axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_ACTIVE, 50, &active, NULL))
		active = NULL;
	lists[0] = arrived;
	lists[1] = active;
	for (l = 0; l < G_N_ELEMENTS(lists); l++) {
		for (i = 0; lists[l] != NULL && i < lists[l]->len; i++) {
			const AxisDownloadInfo *info = g_ptr_array_index(lists[l], i);
			const AxisDownloadClientInfo *client = NULL;
			g_autoptr(GPtrArray) maps = NULL;
			g_autofree gchar *dir = NULL;
			g_autofree gchar *rel = NULL;
			const gchar *why = NULL;
			AxisSetupFolder *folder;
			gint64 volume_id = 0;
			guint c;

			if (info->client_path == NULL || info->client_path[0] != '/')
				continue;
			if (l == 1 && info->problem == NULL)
				continue;
			for (c = 0; c < f->clients->len; c++) {
				const AxisDownloadClientInfo *candidate = g_ptr_array_index(f->clients, c);

				if (candidate->id == info->client_id)
					client = candidate;
			}
			if (client == NULL || n_reported(f->folders, client->id) >= AXIS_WEB_SETUP_MAX_FOLDERS)
				continue;
			dir = g_path_get_dirname(info->client_path);
			if (has_folder(f->folders, client->id, dir))
				continue;
			maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);
			for (c = 0; c < client->maps->len; c++) {
				const AxisDownloadPathMapInfo *map = g_ptr_array_index(client->maps, c);

				g_ptr_array_add(maps, axis_download_path_map_new(map->client_prefix, map->volume_id, map->rel_path));
			}
			folder = g_new0(AxisSetupFolder, 1);
			folder->client_id = client->id;
			folder->path = g_steal_pointer(&dir);
			folder->reported = TRUE;
			switch (axis_download_path_apply(maps, info->client_path, &volume_id, &rel, &why)) {
			case AXIS_DOWNLOAD_PATH_MAPPED: {
				const gchar *slash = strrchr(rel, '/');

				folder->mapped = TRUE;
				folder->volume_id = volume_id;
				folder->rel_path = slash != NULL ? g_strndup(rel, (gsize)(slash - rel)) : g_strdup("");
				break;
			}
			case AXIS_DOWNLOAD_PATH_UNMAPPED:
				folder->problem = g_strdup("maps to no disk Axis knows: add a mapping");
				break;
			default:
				folder->problem = g_strdup_printf("cannot be mapped: %s", why != NULL ? why : "it is not a path Axis "
					"can use");
				break;
			}
			g_ptr_array_add(f->folders, folder);
		}
	}
}

/* ── The words of the checks ────────────────────────────────────── */

/* "API key" stays; "Web password" reads "web password" in a sentence. */
static gchar *
in_sentence(const gchar *label)
{
	gchar *text = g_strdup(label != NULL ? label : "secret");

	if (text[0] != '\0' && g_ascii_isupper(text[0]) && (text[1] == '\0' || g_ascii_islower(text[1])))
		text[0] = g_ascii_tolower(text[0]);
	return text;
}

gchar *
axis_web_setup_source_sentence(const AxisIndexerSourceInfo *source)
{
	guint total;
	guint off = 0;
	guint failing = 0;
	guint i;

	if (!source->enabled)
		return g_strdup_printf("%s is switched off, so it is not searched.", source->name);
	switch (source->state) {
	case AXIS_INDEXER_SOURCE_OK:
		if (source->native) {
			/* An indexer itself: what it is searched by. */
			if (source->caps.read_us == 0 || source->caps.problem != NULL)
				return g_strdup_printf("%s answered; its capabilities %s, so it is searched by words only.",
					source->name, source->caps.read_us == 0 ? "are not read yet" : "could not be read");
			return g_strdup_printf("%s answered, and is searched by %s.", source->name,
				(source->caps.movie_ids | source->caps.tv_ids) != 0 ? "the identifiers it takes, and words where it "
				"takes none" : "words (it takes no identifier)");
		}
		break;
	case AXIS_INDEXER_SOURCE_REFUSED:
		return g_strdup_printf("%s refused the key: check the API key below.", source->name);
	case AXIS_INDEXER_SOURCE_UNREACHABLE:
		return g_strdup_printf("%s did not answer%s%s.", source->name, source->state_message != NULL ? ": " : "",
			source->state_message != NULL ? source->state_message : "");
	case AXIS_INDEXER_SOURCE_FAILING:
		return g_strdup_printf("%s answered, but not usefully%s%s.", source->name,
			source->state_message != NULL ? ": " : "", source->state_message != NULL ? source->state_message : "");
	default:
		return g_strdup_printf("%s is saved but not tested yet: Axis has not asked it anything. Test it now.",
			source->name);
	}
	total = source->indexers != NULL ? source->indexers->len : 0;
	for (i = 0; i < total; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (!info->enabled)
			off++;
		else if (info->failing)
			failing++;
	}
	if (total == 0)
		return g_strdup_printf("%s answered, but reports no indexers: add your indexers in %s, then test it again.",
			source->name, source->name);
	if (off == total)
		return g_strdup_printf("%s answered and reports %u %s, all of them disabled: switch some on in %s, then test "
			"it again.", source->name, total, total == 1 ? "indexer" : "indexers", source->name);
	{
		GString *text = g_string_new(NULL);

		g_string_append_printf(text, "%s answered and reports %u %s", source->name, total, total == 1 ? "indexer" :
			"indexers");
		if (off > 0)
			g_string_append_printf(text, ", %u of them disabled", off);
		if (failing > 0)
			g_string_append_printf(text, "%s%u failing now", off > 0 ? " and " : ", ", failing);
		g_string_append_c(text, '.');
		if (axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_USENET) ||
		    axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_TORRENT))
			g_string_append(text, " Some of its indexers redirect their downloads to their own sites and following "
				"those redirects is switched off, so their releases cannot be fetched: switch on \"Follow download "
				"redirects to the indexers\" in its settings.");
		return g_string_free(text, FALSE);
	}
}

/* A source that answered and has an indexer that is on. */
static gboolean
source_works(const AxisIndexerSourceInfo *source)
{
	guint i;

	if (!source->enabled || source->state != AXIS_INDEXER_SOURCE_OK || source->indexers == NULL)
		return FALSE;
	for (i = 0; i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (info->enabled)
			return TRUE;
	}
	return FALSE;
}

gchar *
axis_web_setup_client_sentence(
	const AxisDownloadClientInfo *client,
	const AxisDownloadKindInfo   *kind
){
	g_autofree gchar *secret = in_sentence(kind != NULL ? kind->secret_label : NULL);
	const gchar *tag = kind != NULL && kind->protocol == AXIS_DOWNLOAD_TORRENT ? "label" : "category";

	if (!client->enabled)
		return g_strdup_printf("%s is switched off, so nothing is sent to it.", client->name);
	if (kind == NULL)
		return g_strdup_printf("%s is of a kind this Axis does not have.", client->name);
	switch (client->state) {
	case AXIS_DOWNLOAD_CLIENT_OK:
		/* A client that does not make it by itself (SABnzbd) puts what it is
		 * sent with an unknown one in its default, where Axis does not look. */
		if (client->category_exists == 0)
			return g_strdup_printf("%s answered, but has no %s called %s: add it in the client's own settings%s, or "
				"what Axis sends lands where Axis does not look.", client->name, tag, client->category,
				g_strcmp0(kind->name, "sabnzbd") == 0 ? " (in SABnzbd: Config, Categories)" : "");
		return g_strdup_printf("%s answered%s%s%s.", client->name, client->category_exists > 0 ? ", and its " : "",
			client->category_exists > 0 ? tag : "", client->category_exists > 0 ? " is there" : "");
	case AXIS_DOWNLOAD_CLIENT_REFUSED:
		return g_strdup_printf("%s refused the %s: check it below.", client->name, secret);
	case AXIS_DOWNLOAD_CLIENT_UNREACHABLE:
		return g_strdup_printf("%s did not answer%s%s.", client->name, client->state_message != NULL ? ": " : "",
			client->state_message != NULL ? client->state_message : "");
	case AXIS_DOWNLOAD_CLIENT_FAILING:
		return g_strdup_printf("%s answered, but not usefully%s%s.", client->name,
			client->state_message != NULL ? ": " : "", client->state_message != NULL ? client->state_message : "");
	default:
		return g_strdup_printf("%s is saved but not tested yet: Axis has not asked it anything. Test it now.",
			client->name);
	}
}

/* A client that answered, with a mapping, and none of its reported folders
 * left unmapped. */
static gboolean
client_works(
	const AxisSetupFigures       *f,
	const AxisDownloadClientInfo *client
){
	guint i;

	if (!client->enabled || client->state != AXIS_DOWNLOAD_CLIENT_OK || client->category_exists == 0 ||
	    client->maps->len == 0)
		return FALSE;
	for (i = 0; i < f->folders->len; i++) {
		const AxisSetupFolder *folder = g_ptr_array_index(f->folders, i);

		if (folder->client_id == client->id && !folder->mapped)
			return FALSE;
	}
	return TRUE;
}

/* The volume of @id's name, or %NULL. */
static const gchar *
volume_name(
	const AxisSetupFigures *f,
	gint64                  id
){
	guint i;

	for (i = 0; f->volumes != NULL && i < f->volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(f->volumes, i);

		if (volume->id == id)
			return volume->name;
	}
	return NULL;
}

/* The first provider that takes a key (TMDb in this Axis). */
static const AxisMetaProviderStatus *
key_provider(const AxisSetupFigures *f)
{
	guint i;

	for (i = 0; f->providers != NULL && i < f->providers->len; i++) {
		const AxisMetaProviderStatus *status = &g_array_index(f->providers, AxisMetaProviderStatus, i);

		if (status->needs_key)
			return status;
	}
	return NULL;
}

static void
state_titles(AxisSetupFigures *f)
{
	AxisSetupState *s = &f->states[AXIS_SETUP_TITLES];
	const AxisMetaProviderStatus *p = key_provider(f);
	const gchar *label;

	if (!f->meta_on || f->providers == NULL || f->providers->len == 0) {
		s->off = TRUE;
		s->summary = g_strdup("Metadata is switched off on the Modules page, so nothing can be looked up.");
		return;
	}
	label = p != NULL ? p->info->label : g_array_index(f->providers, AxisMetaProviderStatus, 0).info->label;
	if (!f->ready.checks[AXIS_READY_METADATA].done) {
		s->summary = g_strdup_printf("No token is stored for %s yet, so Axis cannot look any title up.", label);
		return;
	}
	if (p != NULL && p->key_set && (p->limit.failures > 0 || p->limit.backoff_until_us != 0)) {
		s->summary = g_strdup_printf("%s has a token, but the last time Axis asked it, it failed%s%s.", label,
			p->limit.last_error != NULL ? ": " : "", p->limit.last_error != NULL ? p->limit.last_error : "");
		return;
	}
	s->done = TRUE;
	if (p != NULL && p->limit.requests > 0)
		s->summary = g_strdup_printf("%s has its token and answered the last time Axis asked it.", label);
	else
		s->summary = g_strdup_printf("%s has its token. Axis has not asked it anything since it started: check the "
			"token to be sure.", label);
}

static void
state_library(AxisSetupFigures *f)
{
	AxisSetupState *s = &f->states[AXIS_SETUP_LIBRARY];
	GString *text;

	if (!f->storage_on) {
		s->off = TRUE;
		s->summary = g_strdup("Storage is switched off on the Modules page.");
		return;
	}
	if (!f->films && !f->series) {
		s->off = TRUE;
		s->summary = g_strdup("Movies and TV are both switched off on the Modules page, so there is no library to "
			"keep yet.");
		return;
	}
	text = g_string_new(NULL);
	if (f->volumes == NULL || f->volumes->len == 0)
		g_string_append(text, "No disk has been added yet. ");
	if (f->films)
		g_string_append(text, f->ready.checks[AXIS_READY_FILMS_ROOT].text);
	if (f->series)
		g_string_append_printf(text, "%s%s", f->films ? " " : "", f->ready.checks[AXIS_READY_SERIES_ROOT].text);
	s->summary = g_string_free(text, FALSE);
	s->done = (!f->films || f->ready.checks[AXIS_READY_FILMS_ROOT].done) &&
		(!f->series || f->ready.checks[AXIS_READY_SERIES_ROOT].done);
}

static void
state_indexers(AxisSetupFigures *f)
{
	AxisSetupState *s = &f->states[AXIS_SETUP_INDEXERS];
	const AxisIndexerSourceInfo *shown = NULL;
	guint i;

	if (!f->indexers_on) {
		s->off = TRUE;
		s->summary = g_strdup("Indexers are switched off on the Modules page, so nothing can be searched for.");
		return;
	}
	for (i = 0; i < f->sources->len; i++) {
		const AxisIndexerSourceInfo *source = g_ptr_array_index(f->sources, i);

		if (source_works(source)) {
			s->done = TRUE;
			shown = source;
			break;
		}
		if (shown == NULL || (!shown->enabled && source->enabled))
			shown = source;
	}
	if (shown == NULL)
		s->summary = g_strdup("No indexer is set up yet, nor Prowlarr, so nothing can be searched for.");
	else
		s->summary = axis_web_setup_source_sentence(shown);
}

static void
state_clients(
	AxisWebSetup     *self,
	AxisSetupFigures *f
){
	AxisSetupState *s = &f->states[AXIS_SETUP_CLIENTS];
	const AxisDownloadClientInfo *shown = NULL;
	guint i;

	if (!f->downloads_on) {
		s->off = TRUE;
		s->summary = g_strdup("Downloads are switched off on the Modules page, so nothing found can be downloaded.");
		return;
	}
	for (i = 0; i < f->clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(f->clients, i);

		if (client_works(f, client)) {
			s->done = TRUE;
			shown = client;
			break;
		}
		if (shown == NULL || (!shown->enabled && client->enabled))
			shown = client;
	}
	if (shown == NULL) {
		s->summary = g_strdup("No download client is set up yet, so nothing found can be downloaded.");
		return;
	}
	{
		const AxisDownloadKindInfo *kind = axis_downloads_find_kind(self->downloads, shown->kind);
		g_autofree gchar *state = axis_web_setup_client_sentence(shown, kind);
		const AxisSetupFolder *unmapped = NULL;

		for (i = 0; i < f->folders->len; i++) {
			const AxisSetupFolder *folder = g_ptr_array_index(f->folders, i);

			if (folder->client_id == shown->id && !folder->mapped && unmapped == NULL)
				unmapped = folder;
		}
		if (shown->state != AXIS_DOWNLOAD_CLIENT_OK || !shown->enabled)
			s->summary = g_steal_pointer(&state);
		else if (unmapped != NULL)
			s->summary = g_strdup_printf("%s The client's folder %s %s.", state, unmapped->path, unmapped->problem);
		else if (shown->maps->len == 0)
			s->summary = g_strdup_printf("%s No folder of its is mapped yet: tell Axis where its files appear.",
				state);
		else {
			const AxisDownloadPathMapInfo *map = g_ptr_array_index(shown->maps, 0);
			const gchar *where = volume_name(f, map->volume_id);

			s->summary = g_strdup_printf("%s Its folder %s is on %s for Axis.", state, map->client_prefix,
				where != NULL ? where : "a disk Axis no longer knows");
		}
	}
}

static void
state_quality(AxisSetupFigures *f)
{
	AxisSetupState *s = &f->states[AXIS_SETUP_QUALITY];
	const AxisQualityStored *chosen = NULL;
	guint i;

	if (!f->quality_on || f->profiles == NULL) {
		s->off = TRUE;
		s->summary = g_strdup("Quality profiles are switched off on the Modules page.");
		return;
	}
	for (i = 0; i < f->profiles->len; i++) {
		const AxisQualityStored *stored = g_ptr_array_index(f->profiles, i);

		if (g_strcmp0(stored->slug, f->default_slug) == 0)
			chosen = stored;
	}
	if (chosen == NULL) {
		s->summary = g_strdup_printf("The default quality profile, %s, does not exist: choose another.", f->default_slug);
		return;
	}
	if (!chosen->usable) {
		s->summary = g_strdup_printf("The default quality profile, %s, cannot be used: %s", chosen->name,
			chosen->unusable != NULL ? chosen->unusable : "its definition is not valid.");
		return;
	}
	s->done = TRUE;
	s->summary = f->default_chosen ? g_strdup_printf("%s is the default for new titles.", chosen->name) :
		g_strdup_printf("%s is the default for new titles: Axis's own starting choice until you pick one.",
			chosen->name);
}

static void
state_players(
	AxisWebSetup     *self,
	AxisSetupFigures *f
){
	AxisSetupState *s = &f->states[AXIS_SETUP_PLAYERS];
	const AxisCompatServerStatus *failing = NULL;
	const AxisCompatServerStatus *waiting = NULL;
	guint i;

	if (self->compat == NULL || !f->compat_on) {
		s->off = TRUE;
		s->summary = g_strdup(self->compat == NULL ? "Player compatibility is not part of this Axis." :
			"Player compatibility is switched off on the Modules page.");
		return;
	}
	for (i = 0; f->coverage_read && f->coverage.servers != NULL && i < f->coverage.servers->len; i++) {
		const AxisCompatServerStatus *server = g_ptr_array_index(f->coverage.servers, i);

		if (!server->configured)
			continue;
		if (server->last_ok_us > 0 && server->failures == 0) {
			s->done = TRUE;
			s->summary = g_strdup_printf("%s answered the last look%s%s%s.", server->label,
				server->server_name != NULL ? " (" : "", server->server_name != NULL ? server->server_name : "",
				server->server_name != NULL ? ")" : "");
			return;
		}
		if (server->failures > 0 && failing == NULL)
			failing = server;
		else if (waiting == NULL)
			waiting = server;
	}
	if (failing != NULL)
		s->summary = g_strdup_printf("%s did not answer the last look%s%s.", failing->label,
			failing->last_error != NULL ? ": " : "", failing->last_error != NULL ? failing->last_error : "");
	else if (waiting != NULL)
		s->summary = g_strdup_printf("%s is set up, and Axis has not reached it yet: test the connection.",
			waiting->label);
	else
		s->summary = g_strdup("No media server is set up. Nothing about finding or downloading needs one.");
}

static void
state_writing(AxisSetupFigures *f)
{
	AxisSetupState *s = &f->states[AXIS_SETUP_WRITING];

	s->done = !f->read_only;
	s->summary = g_strdup(f->read_only ? "Storage is read-only: downloads can arrive at the client, but nothing is "
		"imported into the library." : "Axis may write: what arrives is imported into the library.");
}

static void
state_existing(
	AxisWebSetup     *self,
	AxisSetupFigures *f
){
	AxisSetupState *s = &f->states[AXIS_SETUP_EXISTING];
	guint looked = 0;
	guint linked = 0;
	guint review = 0;
	guint i;

	if (self->identify == NULL || !f->identify_on || !f->films || f->identify_roots == NULL) {
		s->off = TRUE;
		s->summary = g_strdup(self->identify == NULL ? "Identification is not part of this Axis." : !f->films ?
			"Movies is switched off on the Modules page, and only movies are recognised yet." :
			"Identification is switched off on the Modules page.");
		return;
	}
	if (f->identify_roots->len == 0) {
		s->summary = g_strdup("There is no library folder for Movies yet: add one in Disks and library folders.");
		return;
	}
	for (i = 0; i < f->identify_roots->len; i++) {
		const AxisIdentifyRootInfo *root = g_ptr_array_index(f->identify_roots, i);

		looked += root->last_run_us > 0;
		linked += root->sure + root->confirmed;
		review += root->likely + root->unknown;
	}
	if (looked < f->identify_roots->len) {
		const AxisIdentifyRootInfo *first = g_ptr_array_index(f->identify_roots, 0);
		g_autofree gchar *which = f->identify_roots->len == 1 ? g_strdup_printf("The Movies library folder %s has "
			"not been looked at yet.", first->name) : g_strdup_printf("%u of the %u Movies library folders have been "
			"looked at.", looked,
			f->identify_roots->len);

		s->summary = g_strdup_printf("%s Axis looks within ten minutes of a scan by itself, or now with Identify new "
			"and changed files now.", which);
		return;
	}
	s->done = TRUE;
	s->summary = g_strdup_printf("Every Movies library folder has been looked at: %u %s recognised, %u waiting for you to "
		"confirm or choose.", linked, linked == 1 ? "folder" : "folders", review);
}

static void
compute_states(
	AxisWebSetup     *self,
	AxisSetupFigures *f
){
	guint i;

	state_titles(f);
	state_library(f);
	state_indexers(f);
	state_clients(self, f);
	state_quality(f);
	state_players(self, f);
	state_writing(f);
	state_existing(self, f);
	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		AxisSetupState *s = &f->states[i];

		s->skipped = !s->done && (f->skipped & AXIS_SETUP_BIT(i)) != 0;
		s->dismissed = !s->done && (f->dismissed & AXIS_SETUP_BIT(i)) != 0;
	}
}

gint
axis_setup_figures_resume(const AxisSetupFigures *figures)
{
	guint i;

	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		const AxisSetupState *s = &figures->states[i];

		if (!s->done && !s->off && !s->dismissed && !s->skipped)
			return (gint)i;
	}
	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		const AxisSetupState *s = &figures->states[i];

		if (!s->done && !s->off && !s->dismissed)
			return (gint)i;
	}
	return -1;
}

gint
axis_setup_figures_next(
	const AxisSetupFigures *figures,
	AxisSetupStep           step
){
	guint i;

	for (i = (guint)step + 1; i < AXIS_SETUP_N_STEPS; i++) {
		const AxisSetupState *s = &figures->states[i];

		if (!s->done && !s->off && !s->dismissed)
			return (gint)i;
	}
	return -1;
}

/* ── Reading everything ─────────────────────────────────────────── */

void
axis_setup_figures_free(AxisSetupFigures *f)
{
	guint i;

	if (f == NULL)
		return;
	axis_readiness_clear(&f->ready);
	for (i = 0; f->providers != NULL && i < f->providers->len; i++)
		axis_meta_provider_status_clear(&g_array_index(f->providers, AxisMetaProviderStatus, i));
	g_clear_pointer(&f->providers, g_array_unref);
	g_clear_pointer(&f->sources, g_ptr_array_unref);
	g_clear_pointer(&f->clients, g_ptr_array_unref);
	g_clear_pointer(&f->folders, g_ptr_array_unref);
	g_clear_pointer(&f->warnings, g_ptr_array_unref);
	g_clear_pointer(&f->volumes, g_ptr_array_unref);
	g_clear_pointer(&f->roots, g_ptr_array_unref);
	g_clear_pointer(&f->profiles, g_ptr_array_unref);
	g_free(f->default_slug);
	if (f->coverage_read)
		axis_compat_coverage_clear(&f->coverage);
	g_clear_pointer(&f->identify_roots, g_ptr_array_unref);
	for (i = 0; i < AXIS_SETUP_N_STEPS; i++)
		g_free(f->states[i].summary);
	g_free(f->check_text);
	g_clear_error(&f->error);
	g_free(f);
}

static gboolean
module_on(
	AxisModuleRegistry *registry,
	const gchar        *id
){
	return registry == NULL || axis_module_registry_is_enabled(registry, id);
}

/* Reads what every step needs (a worker thread).  On a database failure
 * @f->error is set and the rest may be missing. */
static void
collect(
	AxisWebSetup       *self,
	AxisModuleRegistry *registry,
	AxisSetupFigures   *f
){
	g_autoptr(GError) local = NULL;
	guint kinds = 0;
	guint i;

	f->films = module_on(registry, AXIS_MOVIES_MODULE_ID);
	f->series = module_on(registry, AXIS_TV_MODULE_ID);
	f->meta_on = self->meta != NULL && module_on(registry, AXIS_META_MODULE_ID);
	f->indexers_on = module_on(registry, AXIS_INDEXER_MODULE_ID);
	f->downloads_on = module_on(registry, AXIS_DOWNLOAD_MODULE_ID);
	f->storage_on = module_on(registry, AXIS_STORAGE_MODULE_ID);
	f->quality_on = self->quality != NULL && module_on(registry, AXIS_QUALITY_MODULE_ID);
	f->compat_on = self->compat != NULL && module_on(registry, AXIS_COMPAT_MODULE_ID);
	f->identify_on = self->identify != NULL && module_on(registry, AXIS_IDENTIFY_MODULE_ID);
	kinds = (f->films ? AXIS_READY_FILMS : 0) | (f->series ? AXIS_READY_SERIES : 0);

	if (!axis_acquire_check_ready(f->meta_on ? self->meta : NULL, self->indexers, self->downloads, self->storage, kinds,
	        &f->ready, &f->error))
		return;
	f->providers = g_array_new(FALSE, TRUE, sizeof(AxisMetaProviderStatus));
	for (i = 0; f->meta_on && i < axis_meta_n_providers(self->meta); i++) {
		AxisMetaProviderStatus status;

		/* The guide's step is the provider of titles; a secondary one
		 * (TheTVDB) is set on the Metadata page. */
		if (axis_meta_provider_at(self->meta, i)->secondary)
			continue;
		axis_meta_provider_status(self->meta, i, &status);
		g_array_append_val(f->providers, status);
	}
	if (!axis_indexers_list_sources(self->indexers, &f->sources, &f->error) ||
	    !axis_downloads_list_clients(self->downloads, &f->clients, &f->error) ||
	    !axis_storage_list_roots(self->storage, 0, &f->roots, &f->error))
		return;
	f->volumes = axis_storage_list_volumes(self->storage);
	collect_folders(self, f);
	if (!axis_import_volume_warnings(axis_downloads_get_pool(self->downloads), &f->warnings, &local)) {
		g_debug("setup guide: the volumes could not be compared: %s", local->message);
		g_clear_error(&local);
	}
	if (self->quality != NULL) {
		if (!axis_quality_store_list(self->quality, &f->profiles, &f->error))
			return;
		f->default_slug = axis_quality_store_default_slug(self->quality);
		f->default_chosen = axis_settings_lookup(self->settings, AXIS_QUALITY_SETTING_DEFAULT, NULL) != NULL &&
			axis_settings_is_set(self->settings, AXIS_QUALITY_SETTING_DEFAULT);
	}
	if (f->compat_on) {
		f->coverage_read = axis_compat_load_coverage(self->compat, &f->coverage, &local);
		if (!f->coverage_read) {
			g_debug("setup guide: the media servers' state could not be read: %s", local->message);
			g_clear_error(&local);
		}
	}
	if (f->identify_on && !axis_identify_list_roots(self->identify, &f->identify_roots, &f->error))
		return;
	f->recycle_s = axis_settings_lookup(self->settings, AXIS_IMPORT_SETTING_RETENTION, NULL) != NULL ?
		axis_settings_get_duration(self->settings, AXIS_IMPORT_SETTING_RETENTION) : -1;
	f->read_only = axis_storage_is_read_only_mode(self->storage);
	if (!axis_web_setup_load_choices(axis_storage_get_pool(self->storage), &f->skipped, &f->dismissed, &f->error))
		return;
	compute_states(self, f);
}

/* ── The pages' work ────────────────────────────────────────────── */

typedef enum
{
	WORK_OVERVIEW,
	WORK_STEP,
	WORK_CHECK,
	WORK_CHOICE
} WorkKind;

typedef struct
{
	WorkKind            kind;
	AxisSetupStep       step;
	AxisModuleRegistry *registry;
	gchar              *done;	/* a known word from the address */
	gchar              *server;	/* the media server a check is of */
	const gchar        *choice;	/* "skipped", "dismissed", or NULL to forget */
	gchar              *user;
	AxisSetupFigures   *figures;
} Work;

static void
work_free(gpointer data)
{
	Work *work = data;

	g_free(work->done);
	g_free(work->server);
	g_free(work->user);
	axis_setup_figures_free(work->figures);
	g_free(work);
}

/* The token check: a lookup through the layer, as the Metadata page's Try a
 * lookup, of a film every provider knows. */
static void
check_titles(
	AxisWebSetup     *self,
	AxisSetupFigures *f
){
	const gchar *provider = NULL;
	const gchar *label = NULL;
	AxisMetaSearchAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 asked_us = g_get_real_time();
	guint i;

	f->checked = TRUE;
	for (i = 0; self->meta != NULL && i < axis_meta_n_providers(self->meta); i++) {
		const AxisMetaProviderInfo *info = axis_meta_provider_at(self->meta, i);

		if (info->secondary)
			continue;
		if (provider == NULL || info->key_header != NULL) {
			provider = info->name;
			label = info->label;
			if (info->key_header != NULL)
				break;
		}
	}
	if (provider == NULL || !f->meta_on) {
		f->check_text = g_strdup("TMDb cannot be asked: switch on Movie and TV details under Modules.");
		return;
	}
	memset(&answer, 0, sizeof(answer));
	if (!axis_meta_search(self->meta, provider, AXIS_META_KIND_FILM, AXIS_WEB_SETUP_CHECK_TITLE,
	        AXIS_WEB_SETUP_CHECK_YEAR, AXIS_META_DEFAULT_WAIT_MS, &answer, &error)) {
		f->check_text = g_strdup_printf("The lookup could not be asked: %s.", error->message);
		return;
	}
	if (answer.saved) {
		f->check_text = g_strdup_printf("%s did not answer the lookup: %s.", label,
			answer.problem != NULL ? answer.problem : "no reason was given");
	} else if (answer.fetched_us < asked_us) {
		g_autofree gchar *when = axis_web_format_time(answer.fetched_us, "earlier");

		f->check_ok = TRUE;
		f->check_text = g_strdup_printf("%s was not asked: Axis already had a fresh answer to this lookup, from %s, so "
			"this does not show whether the token works now.", label, when);
	} else {
		guint n = answer.titles != NULL ? answer.titles->len : 0;

		f->check_ok = TRUE;
		f->check_text = g_strdup_printf("%s answered: it knows %u %s called %s from %d. The token works.", label, n,
			n == 1 ? "movie" : "movies", AXIS_WEB_SETUP_CHECK_TITLE, AXIS_WEB_SETUP_CHECK_YEAR);
	}
	axis_meta_search_answer_clear(&answer);
}

/* The media server's own connection test. */
static void
check_players(
	AxisWebSetup     *self,
	AxisSetupFigures *f,
	const gchar      *server
){
	AxisCompatTestResult result;
	const gchar *label;

	f->checked = TRUE;
	if (self->compat == NULL || !f->compat_on) {
		f->check_text = g_strdup("Player compatibility is switched off.");
		return;
	}
	label = axis_compat_kind_label(server);
	if (!axis_compat_is_configured_kind(self->compat, server)) {
		f->check_text = g_strdup_printf("%s is not set up: give its address and token first.", label);
		return;
	}
	memset(&result, 0, sizeof(result));
	axis_compat_test_connection_kind(self->compat, server, NULL, &result);
	f->check_ok = result.ok;
	if (result.ok)
		f->check_text = g_strdup_printf("Connected to %s%s%s.", result.name != NULL ? result.name : label,
			result.version != NULL ? ", version " : "", result.version != NULL ? result.version : "");
	else
		f->check_text = g_strdup_printf("The connection to %s failed: %s", label, result.message != NULL ?
			result.message : "no reason was given.");
	axis_compat_test_result_clear(&result);
}

static void
work_run(
	AxisWebSetup *self,
	gpointer      data
){
	Work *work = data;
	AxisSetupFigures *f = g_new0(AxisSetupFigures, 1);

	work->figures = f;
	f->recycle_s = -1;
	if (work->kind == WORK_CHOICE &&
	    !store_choice(axis_storage_get_pool(self->storage), work->step, work->choice, work->user, &f->error))
		return;
	/* The check first, so that what the page then reads includes its answer. */
	if (work->kind == WORK_CHECK) {
		AxisSetupFigures before;

		memset(&before, 0, sizeof(before));
		before.meta_on = self->meta != NULL && module_on(work->registry, AXIS_META_MODULE_ID);
		before.compat_on = self->compat != NULL && module_on(work->registry, AXIS_COMPAT_MODULE_ID);
		if (work->step == AXIS_SETUP_TITLES)
			check_titles(self, &before);
		else
			check_players(self, &before, work->server);
		f->checked = before.checked;
		f->check_ok = before.check_ok;
		f->check_text = before.check_text;
	}
	collect(self, work->registry, f);
}

static HtmxResponse *
work_done(
	AxisWebSetup *self,
	HtmxRequest  *request,
	gpointer      data
){
	Work *work = data;
	gint next;

	if (work->figures->error != NULL)
		return axis_web_internal_error(request, work->figures->error);
	switch (work->kind) {
	case WORK_OVERVIEW:
		return axis_web_setup_overview_page(self, request, work->figures, work->done);
	case WORK_CHOICE:
		axis_web_storage_audit(request, "setup guide: step %s %s", axis_setup_step_slug(work->step),
			work->choice != NULL ? work->choice : "asked for again");
		if (work->choice == NULL) {
			/* Back to the step's actions, where it was asked for again. */
			g_autofree gchar *location = axis_web_setup_return_location(axis_setup_step_path(work->step), NULL);

			return axis_web_place_redirect(request, location, "restored", "step-actions");
		}
		next = axis_setup_figures_next(work->figures, work->step);
		{
			g_autofree gchar *location = axis_web_setup_return_location(next >= 0 ?
				axis_setup_step_path((AxisSetupStep)next) : AXIS_WEB_SETUP_PATH, work->choice);

			return axis_web_auth_redirect(request, location);
		}
	default:
		return axis_web_setup_step_page(self, request, work->figures, work->step, work->done);
	}
}

/* ── Handlers ───────────────────────────────────────────────────── */

/* The words a redirect to the guide may carry: the handlers' own. */
static const gchar *const done_words[] = {
	"saved", "saved-restart", "unchanged", "added", "tested", "map-added", "map-removed", "root-added", "run",
	"skipped", "dismissed", "restored", NULL
};

static gchar *
done_of(HtmxRequest *request)
{
	const gchar *done = htmx_request_get_query_param(request, "done");

	return done != NULL && g_strv_contains(done_words, done) ? g_strdup(done) : NULL;
}

static Work *
work_new(
	AxisWebSetup *self,
	WorkKind      kind
){
	Work *work = g_new0(Work, 1);

	work->kind = kind;
	work->registry = axis_web_setup_registry(self);
	return work;
}

static gboolean
step_param(
	GHashTable    *params,
	AxisSetupStep *step
){
	return params != NULL && axis_setup_step_from_slug(g_hash_table_lookup(params, "step"), step);
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "The setup guide has no such step.");
}

static HtmxResponse *
handle_overview(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = work_new(user_data, WORK_OVERVIEW);

	work->done = done_of(request);
	return dispatch(user_data, request, work_run, work_done, work, work_free);
}

static HtmxResponse *
handle_step(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisSetupStep step;
	Work *work;

	if (!step_param(params, &step))
		return not_found(request);
	work = work_new(user_data, WORK_STEP);
	work->step = step;
	work->done = done_of(request);
	return dispatch(user_data, request, work_run, work_done, work, work_free);
}

static HtmxResponse *
handle_check(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisSetupStep step;
	const gchar *server = htmx_request_get_form_value(request, "server");
	Work *work;

	if (!step_param(params, &step) || (step != AXIS_SETUP_TITLES && step != AXIS_SETUP_PLAYERS))
		return not_found(request);
	if (step == AXIS_SETUP_PLAYERS && (server == NULL || axis_compat_kind_label(server) == NULL))
		return axis_web_error_respond(request, 400, "No media server chosen", "Choose Plex or Jellyfin, then test again.");
	work = work_new(user_data, WORK_CHECK);
	work->step = step;
	work->server = g_strdup(server);
	return dispatch(user_data, request, work_run, work_done, work, work_free);
}

static HtmxResponse *
handle_choice(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	const gchar *choice
){
	AxisSetupStep step;
	const gchar *user = axis_web_request_get_username(request);
	Work *work;

	if (!step_param(params, &step))
		return not_found(request);
	if (user == NULL)
		return axis_web_error_respond(request, 403, "Administrators only", "This page is only for administrators.");
	work = work_new(user_data, WORK_CHOICE);
	work->step = step;
	work->choice = choice;
	work->user = g_strdup(user);
	return dispatch(user_data, request, work_run, work_done, work, work_free);
}

static HtmxResponse *
handle_skip(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return handle_choice(request, params, user_data, "skipped");
}

static HtmxResponse *
handle_dismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return handle_choice(request, params, user_data, "dismissed");
}

static HtmxResponse *
handle_restore(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return handle_choice(request, params, user_data, NULL);
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebSetup *self = user_data;

	self->web = web;
	axis_web_route_full(web, HTMX_METHOD_GET, AXIS_WEB_SETUP_PATH, AXIS_WEB_ROUTE_ADMIN, handle_overview, self);
	axis_web_route_full(web, HTMX_METHOD_GET, AXIS_WEB_SETUP_PATH "/:step", AXIS_WEB_ROUTE_ADMIN, handle_step, self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WEB_SETUP_PATH "/:step/check", AXIS_WEB_ROUTE_ADMIN, handle_check,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WEB_SETUP_PATH "/:step/skip", AXIS_WEB_ROUTE_ADMIN, handle_skip,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WEB_SETUP_PATH "/:step/dismiss", AXIS_WEB_ROUTE_ADMIN,
		handle_dismiss, self);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WEB_SETUP_PATH "/:step/restore", AXIS_WEB_ROUTE_ADMIN,
		handle_restore, self);
}

static const AxisNavEntry nav_entries[] = {
	/* Last in System: once everything is set up it is rarely needed. */
	{ "Setup guide", AXIS_WEB_SETUP_PATH, 90, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_setup_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_WEB_SETUP_MODULE_ID;
		info.name = "Setup guide";
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Walks an administrator through what Axis needs before it can find, download and import "
			"a title: the TMDb token, the disks and library folders, an indexer, a download client and "
			"where its files appear, a default quality profile, and letting Axis write. Each step shows the real "
			"form of the page it belongs to and what that page's own test says.";
		info.flags = AXIS_MODULE_REQUIRED;
	}
	return &info;
}

AxisWebSetup *
axis_web_setup_new(const AxisWebSetupLayers *layers)
{
	AxisWebSetup *self;

	g_return_val_if_fail(layers != NULL && layers->settings != NULL && layers->indexers != NULL &&
		layers->downloads != NULL && layers->storage != NULL, NULL);

	self = g_new0(AxisWebSetup, 1);
	self->settings = axis_settings_ref(layers->settings);
	self->meta = layers->meta != NULL ? axis_meta_ref(layers->meta) : NULL;
	self->indexers = axis_indexers_ref(layers->indexers);
	self->downloads = axis_downloads_ref(layers->downloads);
	self->storage = axis_storage_ref(layers->storage);
	self->quality = layers->quality != NULL ? axis_quality_store_ref(layers->quality) : NULL;
	self->compat = layers->compat != NULL ? axis_compat_ref(layers->compat) : NULL;
	self->identify = layers->identify != NULL ? axis_identify_ref(layers->identify) : NULL;
	return self;
}

void
axis_web_setup_stop(AxisWebSetup *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_setup_free(AxisWebSetup *self)
{
	if (self == NULL)
		return;
	axis_web_setup_stop(self);
	axis_settings_unref(self->settings);
	g_clear_pointer(&self->meta, axis_meta_unref);
	axis_indexers_unref(self->indexers);
	axis_downloads_unref(self->downloads);
	axis_storage_unref(self->storage);
	g_clear_pointer(&self->quality, axis_quality_store_unref);
	g_clear_pointer(&self->compat, axis_compat_unref);
	g_clear_pointer(&self->identify, axis_identify_unref);
	g_free(self);
}
