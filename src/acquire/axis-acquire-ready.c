/*
 * axis-acquire-ready.c - can a title be searched for and downloaded now
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "acquire/axis-acquire-ready.h"

#include <string.h>

static const struct
{
	const gchar *label;
	const gchar *href;
	const gchar *action;
} items[AXIS_READY_N_ITEMS] = {
	[AXIS_READY_METADATA] = { "A TMDb token", "/admin/settings#module-metadata", "Add a TMDb token in Settings" },
	[AXIS_READY_INDEXER] = { "An indexer", "/indexers", "Set up an indexer" },
	[AXIS_READY_CLIENT] = { "A download client", "/downloads/clients", "Set up a download client" },
	/* The labels name the types as the navigation does; the sentences of
	 * a check say "films" and "series". */
	[AXIS_READY_FILMS_ROOT] = { "A library folder for Movies", "/storage", "Add a library folder for Movies" },
	[AXIS_READY_SERIES_ROOT] = { "A library folder for TV", "/storage", "Add a library folder for TV" },
	[AXIS_READY_WRITABLE] = { "Axis may write to the library", "/admin/settings#module-storage",
		"Switch read-only mode off in Settings" }
};

const gchar *
axis_ready_item_label(AxisReadyItem item)
{
	g_return_val_if_fail((guint)item < AXIS_READY_N_ITEMS, "");

	return items[item].label;
}

const gchar *
axis_ready_item_href(AxisReadyItem item)
{
	g_return_val_if_fail((guint)item < AXIS_READY_N_ITEMS, "/");

	return items[item].href;
}

const gchar *
axis_ready_item_action(AxisReadyItem item)
{
	g_return_val_if_fail((guint)item < AXIS_READY_N_ITEMS, "");

	return items[item].action;
}

void
axis_readiness_clear(AxisReadiness *ready)
{
	guint i;

	if (ready == NULL)
		return;
	for (i = 0; i < AXIS_READY_N_ITEMS; i++) {
		g_clear_pointer(&ready->checks[i].text, g_free);
		g_clear_pointer(&ready->checks[i].missing, g_free);
	}
	memset(ready, 0, sizeof(*ready));
}

static void
set_check(
	AxisReadiness *ready,
	AxisReadyItem  item,
	gboolean       done,
	gchar         *text,
	gchar         *missing
){
	AxisReadyCheck *check = &ready->checks[item];

	check->asked = TRUE;
	check->done = done;
	g_free(check->text);
	check->text = text;
	g_free(check->missing);
	check->missing = done ? NULL : missing;
	if (done)
		g_free(missing);
}

/* "Prowlarr", "Prowlarr and Jackett", "Prowlarr and 2 more". */
static gchar *
names_text(GPtrArray *names)
{
	if (names->len == 0)
		return g_strdup("");
	if (names->len == 1)
		return g_strdup(g_ptr_array_index(names, 0));
	if (names->len == 2)
		return g_strdup_printf("%s and %s", (const gchar *)g_ptr_array_index(names, 0),
			(const gchar *)g_ptr_array_index(names, 1));
	return g_strdup_printf("%s and %u more", (const gchar *)g_ptr_array_index(names, 0), names->len - 1);
}

/* ── Metadata ───────────────────────────────────────────────────── */

static void
check_metadata(
	AxisMeta      *meta,
	guint          kinds,
	AxisReadiness *ready
){
	const gchar *label = NULL;
	gboolean films = (kinds & AXIS_READY_FILMS) == 0;
	gboolean series = (kinds & AXIS_READY_SERIES) == 0;
	guint i;

	for (i = 0; meta != NULL && i < axis_meta_n_providers(meta); i++) {
		AxisMetaProviderStatus status;

		axis_meta_provider_status(meta, i, &status);
		/* A secondary provider (TheTVDB) searches and adds nothing itself. */
		if (!status.info->secondary && (!status.needs_key || status.key_set)) {
			if ((status.info->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_FILM)) != 0)
				films = TRUE;
			if ((status.info->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES)) != 0)
				series = TRUE;
			if (label == NULL)
				label = status.info->label;
		}
		axis_meta_provider_status_clear(&status);
	}
	if (films && series && label != NULL)
		set_check(ready, AXIS_READY_METADATA, TRUE, g_strdup_printf("%s has its key: Axis can look titles up.", label),
			NULL);
	else
		set_check(ready, AXIS_READY_METADATA, FALSE, g_strdup("No TMDb token is stored, so Axis cannot look "
			"movies and series up. TMDb gives one free of charge on its own site."), g_strdup("no TMDb token is stored"));
}

/* ── Indexer sources ────────────────────────────────────────────── */

static const gchar *
source_trouble(AxisIndexerSourceState state)
{
	switch (state) {
	case AXIS_INDEXER_SOURCE_UNREACHABLE:
		return "does not answer";
	case AXIS_INDEXER_SOURCE_REFUSED:
		return "refused its key";
	case AXIS_INDEXER_SOURCE_FAILING:
		return "answers with errors";
	default:
		return NULL;
	}
}

/* Why a source's releases cannot be fetched at all: every indexer of it that
 * is on redirects its downloads, and following is switched off. */
#define ALL_REFUSED "redirects every download to its indexers, and following download redirects is switched off"

/* Whether every indexer of @source that is on is in that case. */
static gboolean
every_download_refused(const AxisIndexerSourceInfo *source)
{
	guint on = 0;
	guint i;

	if (source->follow_redirects || source->indexers == NULL)
		return FALSE;
	for (i = 0; i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (!info->enabled)
			continue;
		if (!info->redirect)
			return FALSE;
		on++;
	}
	return on > 0;
}

static gboolean
check_indexer(
	AxisIndexers   *indexers,
	AxisReadiness  *ready,
	GError        **error
){
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GPtrArray) working = g_ptr_array_new();
	const AxisIndexerSourceInfo *broken = NULL;
	const AxisIndexerSourceInfo *partial = NULL;
	const gchar *partial_protocol = NULL;
	const gchar *broken_trouble = NULL;
	gboolean broken_refused = FALSE;
	guint enabled = 0;
	guint i;

	if (!axis_indexers_list_sources(indexers, &sources, error))
		return FALSE;
	for (i = 0; i < sources->len; i++) {
		const AxisIndexerSourceInfo *source = g_ptr_array_index(sources, i);
		const gchar *trouble;

		if (!source->enabled)
			continue;
		enabled++;
		trouble = source_trouble(source->state);
		/* A source whose every download is refused finds nothing that can
		 * be fetched: it needs attention as much as one that fails. */
		if (trouble == NULL && every_download_refused(source)) {
			trouble = ALL_REFUSED;
			if (broken == NULL)
				broken_refused = TRUE;
		}
		if (trouble == NULL) {
			g_ptr_array_add(working, source->name);
			/* Its releases of one protocol cannot be fetched: that protocol
			 * needs attention. */
			if (partial == NULL && axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_USENET)) {
				partial = source;
				partial_protocol = "Usenet";
			} else if (partial == NULL && axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_TORRENT)) {
				partial = source;
				partial_protocol = "torrent";
			}
		} else if (broken == NULL) {
			broken = source;
			broken_trouble = trouble;
		}
	}
	if (working->len > 0) {
		g_autofree gchar *names = names_text(working);
		GString *text = g_string_new(NULL);

		g_string_append_printf(text, "%s %s for releases.", names, working->len == 1 ? "searches" : "search");
		if (partial != NULL)
			g_string_append_printf(text, " Some of %s's %s indexers redirect their downloads and following those "
				"redirects is switched off, so their releases cannot be fetched: see its page.", partial->name,
				partial_protocol);
		set_check(ready, AXIS_READY_INDEXER, TRUE, g_string_free(text, FALSE), NULL);
	} else if (sources->len == 0) {
		set_check(ready, AXIS_READY_INDEXER, FALSE, g_strdup("No indexer is set up, so nothing can be "
			"searched for: add a Usenet indexer, a torrent site, or connect Prowlarr, under Indexers."),
			g_strdup("no indexer is set up"));
	} else if (enabled == 0) {
		set_check(ready, AXIS_READY_INDEXER, FALSE, g_strdup(sources->len == 1 ? "The indexer is switched "
			"off, so nothing can be searched for." : "Every indexer is switched off, so nothing can be "
			"searched for."), g_strdup(sources->len == 1 ? "the indexer is switched off" :
			"every indexer is switched off"));
	} else if (broken_refused) {
		set_check(ready, AXIS_READY_INDEXER, FALSE, g_strdup_printf("%s %s, so nothing it finds can be fetched%s.",
			broken->name, broken_trouble, enabled > 1 ? " (nor does any other indexer work)" : ""),
			g_strdup_printf("the indexer %s", broken_trouble));
	} else {
		set_check(ready, AXIS_READY_INDEXER, FALSE, g_strdup_printf("%s %s, so nothing can be searched for%s.",
			broken->name, broken_trouble, enabled > 1 ? " (nor does any other indexer work)" : ""),
			g_strdup_printf("the indexer %s", broken_trouble));
	}
	return TRUE;
}

/* ── Download clients ───────────────────────────────────────────── */

static const gchar *
client_trouble(AxisDownloadClientState state)
{
	switch (state) {
	case AXIS_DOWNLOAD_CLIENT_UNREACHABLE:
		return "does not answer";
	case AXIS_DOWNLOAD_CLIENT_REFUSED:
		return "refused its secret";
	case AXIS_DOWNLOAD_CLIENT_FAILING:
		return "answers with errors";
	default:
		return NULL;
	}
}

static gboolean
check_client(
	AxisDownloads  *downloads,
	AxisReadiness  *ready,
	GError        **error
){
	g_autoptr(GPtrArray) clients = NULL;
	g_autoptr(GPtrArray) usenet = g_ptr_array_new();
	g_autoptr(GPtrArray) torrent = g_ptr_array_new();
	const AxisDownloadClientInfo *broken = NULL;
	const gchar *trouble = NULL;
	guint enabled = 0;
	guint i;

	if (!axis_downloads_list_clients(downloads, &clients, error))
		return FALSE;
	for (i = 0; i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		const AxisDownloadKindInfo *kind = axis_downloads_find_kind(downloads, client->kind);
		const gchar *why = kind == NULL ? "is of a kind this Axis does not have" : client_trouble(client->state);

		if (!client->enabled)
			continue;
		enabled++;
		if (why != NULL) {
			if (broken == NULL) {
				broken = client;
				trouble = why;
			}
			continue;
		}
		g_ptr_array_add(kind->protocol == AXIS_DOWNLOAD_USENET ? usenet : torrent, client->name);
	}
	ready->usenet = usenet->len > 0;
	ready->torrent = torrent->len > 0;
	if (ready->usenet || ready->torrent) {
		g_autofree gchar *usenet_names = names_text(usenet);
		g_autofree gchar *torrent_names = names_text(torrent);
		gchar *text;

		if (ready->usenet && ready->torrent)
			text = g_strdup_printf("%s %s Usenet releases and %s %s torrents.", usenet_names,
				usenet->len == 1 ? "takes" : "take", torrent_names, torrent->len == 1 ? "takes" : "take");
		else if (ready->usenet)
			text = g_strdup_printf("%s %s Usenet releases. No client takes torrents, so they are passed over.",
				usenet_names, usenet->len == 1 ? "takes" : "take");
		else
			text = g_strdup_printf("%s %s torrents. No client takes Usenet releases, so they are passed over.",
				torrent_names, torrent->len == 1 ? "takes" : "take");
		set_check(ready, AXIS_READY_CLIENT, TRUE, text, NULL);
	} else if (clients->len == 0) {
		set_check(ready, AXIS_READY_CLIENT, FALSE, g_strdup("No download client is set up, so nothing found can be "
			"downloaded. Axis sends releases to one, such as SABnzbd or Deluge."),
			g_strdup("no download client is set up"));
	} else if (enabled == 0) {
		set_check(ready, AXIS_READY_CLIENT, FALSE, g_strdup(clients->len == 1 ? "The download client is switched "
			"off, so nothing found can be downloaded." : "Every download client is switched off, so nothing found "
			"can be downloaded."), g_strdup(clients->len == 1 ? "the download client is switched off" :
			"every download client is switched off"));
	} else {
		set_check(ready, AXIS_READY_CLIENT, FALSE, g_strdup_printf("%s %s, so nothing found can be downloaded%s.",
			broken->name, trouble, enabled > 1 ? " (nor does any other client work)" : ""),
			g_strdup_printf("the download client %s", trouble));
	}
	return TRUE;
}

/* ── Roots ──────────────────────────────────────────────────────── */

static gboolean
check_root(
	AxisStorage    *storage,
	AxisMediaType   type,
	AxisReadyItem   item,
	AxisReadiness  *ready,
	GError        **error
){
	const gchar *noun = type == AXIS_MEDIA_MOVIES ? "movies" : "series";
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GPtrArray) good = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *problem = NULL;
	g_autofree gchar *short_problem = NULL;
	guint found = 0;
	guint i;

	if (!axis_storage_list_roots(storage, 0, &roots, error))
		return FALSE;
	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		g_autoptr(AxisVolumeInfo) volume = NULL;

		if (root->media_type != type)
			continue;
		found++;
		volume = axis_storage_get_volume(storage, root->volume_id);
		if (volume != NULL && volume->state == AXIS_VOLUME_ONLINE) {
			g_ptr_array_add(good, g_strdup_printf("%s, on %s,", root->name, volume->name));
			continue;
		}
		if (problem != NULL)
			continue;
		if (volume != NULL && volume->state == AXIS_VOLUME_READ_ONLY) {
			problem = g_strdup_printf("%s is on %s, which is mounted read-only", root->name, volume->name);
			short_problem = g_strdup_printf("the library folder for %s is on a disk mounted read-only", noun);
		} else {
			problem = g_strdup_printf("%s is on %s, which is not online", root->name,
				volume != NULL ? volume->name : "a disk Axis no longer knows");
			short_problem = g_strdup_printf("the library folder for %s is not online", noun);
		}
	}
	if (good->len > 0) {
		g_autofree gchar *names = NULL;

		/* "Films, on Disk one, takes new films."; "Films, on Disk one, and
		 * 1 more take new films." */
		if (good->len == 1)
			names = g_strdup(g_ptr_array_index(good, 0));
		else
			names = g_strdup_printf("%s and %u more", (const gchar *)g_ptr_array_index(good, 0), good->len - 1);
		set_check(ready, item, TRUE, g_strdup_printf("%s %s new %s.", names, good->len == 1 ? "takes" : "take", noun),
			NULL);
	} else if (found == 0) {
		set_check(ready, item, FALSE, g_strdup_printf("No library folder for %s is set up, so a %s has nowhere to go.",
			noun, type == AXIS_MEDIA_MOVIES ? "movie" : "series"),
			g_strdup_printf("no library folder for %s is set up", noun));
	} else {
		set_check(ready, item, FALSE, g_strdup_printf("%s, so nothing can be put there.", problem),
			g_steal_pointer(&short_problem));
	}
	return TRUE;
}

/* ── All of it ──────────────────────────────────────────────────── */

gboolean
axis_acquire_check_ready(
	AxisMeta       *meta,
	AxisIndexers   *indexers,
	AxisDownloads  *downloads,
	AxisStorage    *storage,
	guint           kinds,
	AxisReadiness  *ready,
	GError        **error
){
	g_return_val_if_fail(indexers != NULL && downloads != NULL && storage != NULL, FALSE);
	g_return_val_if_fail(ready != NULL, FALSE);

	memset(ready, 0, sizeof(*ready));
	check_metadata(meta, kinds, ready);
	if (!check_indexer(indexers, ready, error) || !check_client(downloads, ready, error) ||
	    ((kinds & AXIS_READY_FILMS) != 0 &&
	     !check_root(storage, AXIS_MEDIA_MOVIES, AXIS_READY_FILMS_ROOT, ready, error)) ||
	    ((kinds & AXIS_READY_SERIES) != 0 &&
	     !check_root(storage, AXIS_MEDIA_TV, AXIS_READY_SERIES_ROOT, ready, error))) {
		axis_readiness_clear(ready);
		return FALSE;
	}
	if (axis_storage_is_read_only_mode(storage))
		set_check(ready, AXIS_READY_WRITABLE, FALSE, g_strdup("Storage is in read-only mode: downloads can arrive, "
			"but nothing is imported into the library until it is switched off."),
			g_strdup("storage is in read-only mode, so nothing can be imported"));
	else
		set_check(ready, AXIS_READY_WRITABLE, TRUE, g_strdup("Axis may write to the library."), NULL);
	return TRUE;
}

gboolean
axis_readiness_complete(const AxisReadiness *ready)
{
	guint i;

	g_return_val_if_fail(ready != NULL, FALSE);

	for (i = 0; i < AXIS_READY_N_ITEMS; i++) {
		if (ready->checks[i].asked && !ready->checks[i].done)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_readiness_can_get(
	const AxisReadiness *ready,
	guint                kinds
){
	g_return_val_if_fail(ready != NULL, FALSE);

	if (!ready->checks[AXIS_READY_INDEXER].done || !ready->checks[AXIS_READY_CLIENT].done)
		return FALSE;
	if ((kinds & AXIS_READY_FILMS) != 0 && !ready->checks[AXIS_READY_FILMS_ROOT].done)
		return FALSE;
	if ((kinds & AXIS_READY_SERIES) != 0 && !ready->checks[AXIS_READY_SERIES_ROOT].done)
		return FALSE;
	return TRUE;
}
