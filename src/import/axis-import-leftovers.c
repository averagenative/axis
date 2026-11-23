/*
 * axis-import-leftovers.c - what is left in a download client's folder for
 * Axis that no download and no import accounts for: the Leftovers page's
 * list, and the one deletion a person asks for there
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * With imports that move a download's files, anything still in a client's
 * folder for Axis was not imported: a download given up on whose files the
 * client kept, one that failed, the remainder of a folder a rule left, files
 * a person put there.  The page lists them per client, so that they can be
 * deleted on purpose; nothing here is deleted without a person asking, one
 * entry at a time.
 *
 * Only a client's own folder for Axis is looked in: the folder its kind says
 * it keeps Axis's category or label in (axis_downloads_category_folder(),
 * which refuses a folder shared with other downloads), reached through the
 * client's path mapping (the mapping must hold that folder), on a known
 * volume, not the volume's top or a folder of Axis's own, not inside a
 * library root other than a downloads root and holding none.  Through a read
 * guard (storage read-only allows looking), without following a link.  What
 * a download Axis knows that is still at its client (sending to completed, or
 * a torrent left seeding), or an import still open (waiting, running,
 * blocked, partly imported), is in or around is accounted for and not listed:
 * by the place recorded, by the place the client says now (in progress too:
 * a torrent is in its save folder before it completes), or by the name the
 * client gives it.  While a download Axis follows there has no place known
 * (one being sent, or the client cannot be asked), nothing is offered for
 * deletion ("it may belong to ..."); nor ever a name the client gives a
 * download it has not finished (SABnzbd's _UNPACK_ and _FAILED_ folders,
 * qBittorrent's .!qB files).  The list is bounded: at most
 * AXIS_IMPORT_LEFTOVERS_SHOWN entries are shown, by name, and at most
 * AXIS_IMPORT_LEFTOVERS_COUNTED counted; a folder's size is added up over at
 * most AXIS_IMPORT_LEFTOVER_SIZE_NAMES names.
 *
 * A deletion asks all of it again, through a write guard (refused while
 * storage is read-only or the volume is offline or mounted read-only), and
 * goes through the write layer's own removals: the entry must be the very
 * inode the page listed, one name directly in the client's folder for Axis,
 * not a link (never followed), on the volume's filesystem; a folder must
 * hold no library root, no other filesystem, and be within the removal's
 * bounds.  An entry that is not there any more is "already gone", not an
 * error.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "import/axis-import-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-write.h"

void
axis_import_leftover_free(AxisImportLeftover *entry)
{
	if (entry == NULL)
		return;
	g_free(entry->name);
	g_free(entry->stored);
	g_free(entry->why);
	g_free(entry);
}

void
axis_import_leftover_folder_free(AxisImportLeftoverFolder *folder)
{
	if (folder == NULL)
		return;
	g_free(folder->client_name);
	g_free(folder->folder);
	g_free(folder->volume_name);
	g_free(folder->problem);
	g_free(folder->caution);
	g_clear_pointer(&folder->entries, g_ptr_array_unref);
	g_free(folder);
}

/* ── The folder ─────────────────────────────────────────────────── */

typedef struct
{
	gint64        volume;
	gchar        *rel;		/* the client's folder for Axis on the volume */
	gchar        *client_folder;
	AxisDbResult *roots;	/* rel_path, downloads, name */
	AxisDbResult *places;	/* the places live downloads and open imports account for */
	GPtrArray    *item_places;	/* where the client's downloads are now, on the volume */
	GPtrArray    *item_keys;	/* their names in the client's folder, for names that differ only a little */
	GPtrArray    *item_names;	/* the names of those it says no place of */
	GPtrArray    *loose;	/* the names of those it keeps loose in the client's folder itself */
	GPtrArray    *unplaced;	/* downloads Axis follows there whose place nobody says yet */
	gchar        *unknown;	/* why what the client has is not all known: nothing is offered */
	gchar        *shared;	/* another client's folder for Axis is this one, or one holds the other */
	gchar        *undetermined;	/* another client's folder for Axis cannot be worked out */
} Place;

static void
place_clear(Place *place)
{
	g_free(place->rel);
	g_free(place->client_folder);
	g_clear_pointer(&place->roots, axis_db_result_free);
	g_clear_pointer(&place->places, axis_db_result_free);
	g_clear_pointer(&place->item_places, g_ptr_array_unref);
	g_clear_pointer(&place->item_keys, g_ptr_array_unref);
	g_clear_pointer(&place->item_names, g_ptr_array_unref);
	g_clear_pointer(&place->loose, g_ptr_array_unref);
	g_clear_pointer(&place->unplaced, g_ptr_array_unref);
	g_clear_pointer(&place->unknown, g_free);
	g_clear_pointer(&place->shared, g_free);
	g_clear_pointer(&place->undetermined, g_free);
}

/*
 * The names a download client gives a download it has not finished, which
 * Axis never offers to delete: SABnzbd's folders of a job being unpacked and
 * its own "__ADMIN__" folders, qBittorrent's ".!qB" for a file not complete,
 * the ".part" and ".parts" of others, and folders named for incomplete
 * downloads.  (SABnzbd's "_FAILED_" folder of a job that failed is the
 * client's while it still lists that job, which accounts for it by its name;
 * once it does not, it is a leftover a person may delete.)
 */
static gboolean
client_internal(const gchar *name)
{
	static const gchar *const prefixes[] = { "_UNPACK_", "__ADMIN__", NULL };
	static const gchar *const suffixes[] = { ".!qB", ".part", ".parts", ".!ut", NULL };
	static const gchar *const whole[] = { "incomplete", "_incomplete", ".incomplete", "__ADMIN__", NULL };
	guint i;

	for (i = 0; prefixes[i] != NULL; i++) {
		if (g_str_has_prefix(name, prefixes[i]))
			return TRUE;
	}
	for (i = 0; suffixes[i] != NULL; i++) {
		if (g_str_has_suffix(name, suffixes[i]))
			return TRUE;
	}
	for (i = 0; whole[i] != NULL; i++) {
		if (g_ascii_strcasecmp(name, whole[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* @name as a filesystem that ignores case or normalises Unicode may hold it:
 * composed, case folded; %NULL for a name that is not text. */
static gchar *
folded(const gchar *name)
{
	g_autofree gchar *normal = NULL;

	if (name == NULL || !g_utf8_validate(name, -1, NULL))
		return NULL;
	normal = g_utf8_normalize(name, -1, G_NORMALIZE_ALL_COMPOSE);
	return normal != NULL ? g_utf8_casefold(normal, -1) : NULL;
}

/* Whether @entry (one name in the client's folder) is the download the
 * client calls @name, as the client names what it puts there: the name, the
 * name with an incomplete file's suffix, or a job being unpacked or that
 * failed.  With @loosely, names that differ only by case or Unicode
 * normalisation count too (a share that ignores case, or normalises names,
 * shows the client's name so). */
static gboolean
named_for(
	const gchar *entry,
	const gchar *name,
	gboolean     loosely
){
	static const gchar *const prefixes[] = { "", "_UNPACK_", "_FAILED_", NULL };
	static const gchar *const suffixes[] = { "", ".!qB", ".part", NULL };
	g_autofree gchar *entry_folded = loosely ? folded(entry) : NULL;
	guint i;
	guint k;

	if (name == NULL || name[0] == '\0' || (loosely && entry_folded == NULL))
		return FALSE;
	for (i = 0; prefixes[i] != NULL; i++) {
		for (k = 0; suffixes[k] != NULL; k++) {
			g_autofree gchar *candidate = g_strconcat(prefixes[i], name, suffixes[k], NULL);

			if (strcmp(entry, candidate) == 0)
				return TRUE;
			if (loosely) {
				g_autofree gchar *candidate_folded = folded(candidate);

				if (candidate_folded != NULL && strcmp(entry_folded, candidate_folded) == 0)
					return TRUE;
			}
		}
	}
	return FALSE;
}

/* Whether @path is the volume's top or in a folder of Axis's own there. */
static gboolean
axis_own(const gchar *path)
{
	gsize first;

	if (path == NULL || path[0] == '\0')
		return TRUE;
	first = strcspn(path, "/");
	return first >= 6 && strncmp(path, ".axis-", 6) == 0;
}

static gboolean client_now(AxisImport *self, gint64 client_id, Place *place, GError **error);

/* ── The other clients ──────────────────────────────────────────── */

void
axis_import_others_clear(AxisImportOthers *others)
{
	if (others == NULL)
		return;
	g_clear_pointer(&others->places, g_ptr_array_unref);
	g_clear_pointer(&others->loose, g_ptr_array_unref);
	g_clear_pointer(&others->names, g_ptr_array_unref);
	g_clear_pointer(&others->unplaced, g_ptr_array_unref);
	g_clear_pointer(&others->unknown, g_free);
	g_clear_pointer(&others->shared, g_free);
	g_clear_pointer(&others->undetermined, g_free);
}

static void
others_unknown(
	AxisImportOthers *others,
	gchar            *text
){
	if (others->unknown == NULL)
		others->unknown = text;
	else
		g_free(text);
}

gboolean
axis_import_others(
	AxisImport        *self,
	gint64             client_id,
	gint64             volume,
	const gchar       *folder_rel,
	AxisImportOthers  *others,
	GError           **error
){
	g_autoptr(GPtrArray) neighbours = NULL;
	guint i;

	memset(others, 0, sizeof(*others));
	others->places = g_ptr_array_new_with_free_func(g_free);
	others->loose = g_ptr_array_new_with_free_func(g_free);
	others->names = g_ptr_array_new_with_free_func(g_free);
	others->unplaced = g_ptr_array_new_with_free_func(g_free);
	if (folder_rel == NULL)
		return TRUE;
	if (!axis_downloads_neighbours(self->downloads, client_id, volume, folder_rel, &neighbours, error))
		return FALSE;
	for (i = 0; i < neighbours->len; i++) {
		const AxisDownloadNeighbour *n = g_ptr_array_index(neighbours, i);
		g_autoptr(AxisDbResult) rows = NULL;
		g_autofree gchar *client = axis_import_int_text(n->client_id);
		const gchar *params[1];
		guint k;

		/* Its own folder for Axis: the same as this one, or one holds the
		 * other, and the two clients' downloads are not told apart. */
		if (n->folder != NULL && others->shared == NULL) {
			g_autofree gchar *rel = NULL;
			g_autofree gchar *why = NULL;
			gint64 where = 0;

			if (!axis_downloads_map_client_path(self->downloads, n->client_id, n->folder, &where, &rel, NULL, &why,
			        error)) {
				if (error != NULL && *error != NULL)
					return FALSE;
			} else if (where == volume && (axis_storage_path_contains(rel, folder_rel) ||
			           axis_storage_path_contains(folder_rel, rel))) {
				others->shared = g_strdup_printf("the download client %s keeps Axis's downloads in %s, which is this "
					"folder, or holds it, or is inside it: two clients' downloads in one folder for Axis cannot be told "
					"apart, so Axis deletes nothing there. Give each client a folder of its own for Axis", n->name,
					n->folder);
			}
		}
		if (n->folder_failure != NULL)
			others_unknown(others, g_strdup_printf("the download client %s, whose path mappings reach this folder, could "
				"not be asked where it keeps Axis's downloads (%s), so any of what is here may be its", n->name,
				n->folder_failure));
		/* One whose folder for Axis cannot be worked out may keep Axis's
		 * downloads here: not told apart from this client's. */
		if (n->folder == NULL && n->folder_none != NULL && others->undetermined == NULL)
			others->undetermined = g_strdup_printf("the download client %s, whose path mappings reach this folder, keeps "
				"Axis's downloads in no folder Axis can work out (%s), so they may be in this one and cannot be told "
				"apart. Give it a folder of its own for Axis, or a path mapping that does not reach this folder", n->name,
				n->folder_none);
		if (n->items == NULL) {
			others_unknown(others, g_strdup_printf("the download client %s, whose path mappings reach this folder, could "
				"not be asked what it has now (%s), so any of what is here may be its", n->name,
				n->failure != NULL ? n->failure : "no answer"));
			continue;
		}
		if (n->unreadable > 0)
			others_unknown(others, g_strdup_printf("the download client %s, whose path mappings reach this folder, has "
				"something Axis cannot read (%s), so any of what is here may be its", n->name,
				n->unreadable_why != NULL ? n->unreadable_why : "its list was not whole"));
		for (k = 0; k < n->items->len; k++) {
			const AxisDownloadItem *item = g_ptr_array_index(n->items, k);
			const gchar *place = item->place != NULL ? item->place : item->path;
			g_autofree gchar *normal = NULL;
			g_autofree gchar *rel = NULL;
			g_autofree gchar *why = NULL;
			gint64 where = 0;

			if (place == NULL) {
				if (item->own_name != NULL)
					g_ptr_array_add(others->names, g_strdup(item->own_name));
				else
					others_unknown(others, g_strdup_printf("the download client %s has a download (the entry %s, %s) "
						"whose name Axis cannot read exactly and whose place it does not say, so any of what is here may "
						"be it", n->name, item->client_ref, item->name));
				continue;
			}
			if (!axis_storage_path_normalize_exact(place, TRUE, &normal, NULL)) {
				others_unknown(others, g_strdup_printf("the download client %s has a download (the entry %s, %s) whose "
					"place Axis cannot read exactly, so any of what is here may be it", n->name, item->client_ref,
					item->name));
				continue;
			}
			if (!axis_downloads_map_client_path(self->downloads, n->client_id, normal, &where, &rel, NULL, &why, error)) {
				if (error != NULL && *error != NULL)
					return FALSE;
				continue;
			}
			if (where != volume)
				continue;
			if (axis_storage_path_contains(rel, folder_rel))
				g_ptr_array_add(others->loose, g_strdup(item->name));
			else if (axis_storage_path_contains(folder_rel, rel))
				g_ptr_array_add(others->places, g_steal_pointer(&rel));
		}
		/* Downloads Axis follows there whose place nobody says yet. */
		params[0] = client;
		if (!axis_db_pool_exec(self->pool, "SELECT client_ref, release_name FROM download WHERE client_id = $1::bigint"
		        " AND rel_path IS NULL AND state IN ('sending', 'queued', 'downloading', 'paused', 'completed')"
		        " ORDER BY id", 1, params, &rows, error))
			return FALSE;
		for (k = 0; k < axis_db_result_get_n_rows(rows); k++) {
			const gchar *ref = axis_db_result_get_text(rows, k, 0);
			gboolean listed = FALSE;
			guint m;

			for (m = 0; ref != NULL && m < n->items->len; m++) {
				if (g_strcmp0(((const AxisDownloadItem *)g_ptr_array_index(n->items, m))->client_ref, ref) == 0)
					listed = TRUE;
			}
			if (!listed)
				g_ptr_array_add(others->unplaced, g_strdup(axis_db_result_get_text(rows, k, 1) != NULL ?
					axis_db_result_get_text(rows, k, 1) : "a download"));
		}
	}
	return TRUE;
}

/*
 * The client's folder for Axis, as the client says now and its mapping puts
 * it, and what the rules say of it.  %TRUE with place filled; %FALSE with
 * *@problem (for the page) when it cannot be looked in, or with @error for
 * the database.
 */
static gboolean
find_place(
	AxisImport  *self,
	gint64       client_id,
	Place       *place,
	gchar      **problem,
	GError     **error
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *volume = NULL;
	const gchar *params[1];
	guint i;

	memset(place, 0, sizeof(*place));
	*problem = NULL;
	if (!axis_downloads_category_folder(self->downloads, client_id, &place->client_folder, &local)) {
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND)) {
			*problem = g_strdup_printf("It keeps Axis's downloads in no folder of their own (%s), so there is no "
				"folder of Axis's to look in: what is in its folder for every download is not Axis's to list.",
				local->message);
			/* No folder of its own: none shared either (this page says why). */
			axis_downloads_set_folder_warning(self->downloads, client_id, NULL);
		} else {
			*problem = g_strdup_printf("It could not be asked where it keeps Axis's downloads (%s).", local->message);
		}
		return FALSE;
	}
	if (!axis_downloads_map_client_path(self->downloads, client_id, place->client_folder, &place->volume, &place->rel,
	        &prefix, &why, &local)) {
		if (local != NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		*problem = g_strdup_printf("Its folder for Axis (%s) is not reached by its path mappings (%s): map it on the "
			"client's page.", place->client_folder, why);
		axis_downloads_set_folder_warning(self->downloads, client_id, NULL);
		return FALSE;
	}
	if (prefix == NULL || !axis_storage_path_contains(prefix, place->client_folder)) {
		*problem = g_strdup_printf("The path mapping that reaches its folder for Axis (%s) begins inside it, so the "
			"folder cannot be told on the volume.", place->client_folder);
		return FALSE;
	}
	if (axis_own(place->rel)) {
		*problem = g_strdup_printf("Its folder for Axis (%s) is the top of its volume, or a folder of Axis's own there.",
			place->client_folder);
		return FALSE;
	}
	volume = axis_import_int_text(place->volume);
	params[0] = volume;
	if (!axis_db_pool_exec(self->pool, "SELECT rel_path, media_type = 'downloads', name FROM library_root"
	        " WHERE volume_id = $1::bigint ORDER BY id", 1, params, &place->roots, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(place->roots); i++) {
		const gchar *root = axis_db_result_get_text(place->roots, i, 0);
		gboolean downloads = g_strcmp0(axis_db_result_get_text(place->roots, i, 1), "t") == 0;

		if (root == NULL)
			continue;
		if ((!downloads && axis_storage_path_contains(root, place->rel)) ||
		    axis_storage_path_contains(place->rel, root)) {
			*problem = g_strdup_printf("Its folder for Axis (%s) is in the library root %s, or holds it: Axis lists "
				"nothing there.", place->client_folder, axis_db_result_get_text(place->roots, i, 2));
			return FALSE;
		}
	}
	/* Downloads Axis follows, a torrent left at its client (seeding, or one a
	 * person moved to another category or label: whatever its download's
	 * state says now, as tracking no longer sees it there), and open imports,
	 * by the place recorded. */
	if (!axis_db_pool_exec(self->pool, "SELECT rel_path FROM download d WHERE volume_id = $1::bigint"
	        " AND rel_path IS NOT NULL AND (state IN ('sending', 'queued', 'downloading', 'paused', 'completed')"
	        "  OR (protocol = 'torrent' AND EXISTS (SELECT 1 FROM import j"
	        "  WHERE j.download_id = d.id AND j.client_removal = 'kept')))"
	        " UNION ALL SELECT source_path FROM import WHERE source_volume_id = $1::bigint"
	        " AND state IN ('waiting', 'running', 'blocked', 'partial')", 1, params, &place->places, error))
		return FALSE;
	if (!client_now(self, client_id, place, error))
		return FALSE;
	/* Another client keeps Axis's downloads in this folder too (or in one
	 * around it or inside it): nothing in it is told apart, nothing listed. */
	if (place->shared != NULL) {
		*problem = g_strdup_printf("Its folder for Axis (%s) is not its own: %s.", place->client_folder, place->shared);
		/* Said on the client's page too, from its next look on. */
		axis_downloads_set_folder_warning(self->downloads, client_id, *problem);
		return FALSE;
	}
	/* Another client's folder for Axis that cannot be worked out may be this
	 * one: the same, as far as anything here is told apart. */
	if (place->undetermined != NULL) {
		*problem = g_strdup_printf("Its folder for Axis (%s) may not be its own: %s.", place->client_folder,
			place->undetermined);
		axis_downloads_set_folder_warning(self->downloads, client_id, *problem);
		return FALSE;
	}
	axis_downloads_set_folder_warning(self->downloads, client_id, NULL);
	return TRUE;
}

/*
 * What the client has now, in every state, by what it says of each: its
 * place (a torrent in progress is in its save folder already), mapped onto
 * the volume, or its name when it says no place.  One it keeps loose in the
 * client's folder itself (a torrent without a folder of its own) could be any
 * entry of it.  The downloads Axis follows at the client that have no place
 * recorded and that the client does not list (one being sent) are unplaced:
 * nothing that could be one of them is offered for deletion.  When the list
 * cannot be had, is not whole, holds what Axis could not read, or places a
 * download inside the client's folder somewhere the mapping does not put on
 * this volume, what the client has is not all known (place->unknown), and
 * nothing at all is offered.
 */
static gboolean
client_now(
	AxisImport  *self,
	gint64       client_id,
	Place       *place,
	GError     **error
){
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *client = axis_import_int_text(client_id);
	g_autofree gchar *unreadable_why = NULL;
	const gchar *params[1];
	guint unreadable = 0;
	guint i;

	place->item_places = g_ptr_array_new_with_free_func(g_free);
	place->item_keys = g_ptr_array_new_with_free_func(g_free);
	place->item_names = g_ptr_array_new_with_free_func(g_free);
	place->loose = g_ptr_array_new_with_free_func(g_free);
	place->unplaced = g_ptr_array_new_with_free_func(g_free);
	/* Everything it has, in every category or label. */
	if (!axis_downloads_client_items_full(self->downloads, client_id, &items, &unreadable, &unreadable_why, &local)) {
		place->unknown = g_strdup_printf("Its client could not be asked what it has now (%s), so nothing here is "
			"offered for deletion: any of it may be a download still at the client.", local->message);
		items = NULL;
	} else if (unreadable > 0) {
		place->unknown = g_strdup_printf("Its client has %u %s Axis cannot read (the first: %s), so nothing here is "
			"offered for deletion: any of it may be what the client has.", unreadable,
			unreadable == 1 ? "thing" : "things", unreadable_why != NULL ? unreadable_why : "its list was not whole");
	}
	for (i = 0; items != NULL && i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);
		const gchar *where = item->place != NULL ? item->place : item->path;
		g_autofree gchar *normal = NULL;
		g_autofree gchar *rel = NULL;
		g_autofree gchar *why = NULL;
		gint64 volume = 0;
		gboolean mapped;
		gboolean inside;

		/* By its name exactly as the client gives it, when it says no
		 * place; a place or a name Axis cannot read exactly tells nothing,
		 * and then nothing is offered. */
		if (where == NULL) {
			if (item->own_name != NULL)
				g_ptr_array_add(place->item_names, g_strdup(item->own_name));
			else if (place->unknown == NULL)
				place->unknown = g_strdup_printf("Its client has a download (%s) whose name Axis cannot read exactly and "
					"whose place it does not say, so nothing here is offered for deletion: any of it may be that "
					"download.", item->name);
			continue;
		}
		if (!axis_storage_path_normalize_exact(where, TRUE, &normal, NULL)) {
			if (place->unknown == NULL)
				place->unknown = g_strdup_printf("Its client has a download (%s) whose place Axis cannot read exactly, so "
					"nothing here is offered for deletion: any of it may be that download.", item->name);
			continue;
		}
		mapped = axis_downloads_map_client_path(self->downloads, client_id, normal, &volume, &rel, NULL, &why, error);
		if (!mapped && error != NULL && *error != NULL)
			return FALSE;
		inside = axis_storage_path_contains(place->client_folder, normal);
		if (!mapped || volume != place->volume) {
			/* Inside the client's folder for Axis, but not where that folder
			 * is: the mapping does not tell where it is. */
			if (inside && place->unknown == NULL)
				place->unknown = g_strdup_printf("Its client has %s in its folder for Axis, and its path mappings put "
					"it somewhere else, so nothing here is offered for deletion: any of it may be that download.",
					item->name);
			continue;
		}
		/* At the client's folder itself (or around it): loose in it. */
		if (axis_storage_path_contains(rel, place->rel)) {
			g_ptr_array_add(place->loose, g_strdup(item->name));
			continue;
		}
		if (axis_storage_path_contains(place->rel, rel)) {
			const gchar *below = rel + strlen(place->rel) + 1;

			g_ptr_array_add(place->item_keys, g_strndup(below, strcspn(below, "/")));
		}
		g_ptr_array_add(place->item_places, g_steal_pointer(&rel));
	}
	params[0] = client;
	if (!axis_db_pool_exec(self->pool, "SELECT client_ref, release_name FROM download WHERE client_id = $1::bigint"
	        " AND rel_path IS NULL AND state IN ('sending', 'queued', 'downloading', 'paused', 'completed')"
	        " ORDER BY id", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *ref = axis_db_result_get_text(rows, i, 0);
		gboolean listed = FALSE;
		guint k;

		for (k = 0; ref != NULL && items != NULL && k < items->len; k++) {
			if (g_strcmp0(((const AxisDownloadItem *)g_ptr_array_index(items, k))->client_ref, ref) == 0)
				listed = TRUE;
		}
		if (!listed)
			g_ptr_array_add(place->unplaced, g_strdup(axis_db_result_get_text(rows, i, 1) != NULL ?
				axis_db_result_get_text(rows, i, 1) : "a download"));
	}
	/* And what the other clients whose path mappings reach this folder have
	 * in it or around it: a download of another client is not a leftover
	 * either. */
	{
		AxisImportOthers others;

		if (!axis_import_others(self, client_id, place->volume, place->rel, &others, error)) {
			axis_import_others_clear(&others);
			return FALSE;
		}
		for (i = 0; i < others.places->len; i++) {
			const gchar *rel = g_ptr_array_index(others.places, i);

			if (axis_storage_path_contains(place->rel, rel) && strcmp(place->rel, rel) != 0) {
				const gchar *below = rel + strlen(place->rel) + 1;

				g_ptr_array_add(place->item_keys, g_strndup(below, strcspn(below, "/")));
			}
			g_ptr_array_add(place->item_places, g_strdup(rel));
		}
		for (i = 0; i < others.loose->len; i++)
			g_ptr_array_add(place->loose, g_strdup(g_ptr_array_index(others.loose, i)));
		for (i = 0; i < others.names->len; i++)
			g_ptr_array_add(place->item_names, g_strdup(g_ptr_array_index(others.names, i)));
		for (i = 0; i < others.unplaced->len; i++)
			g_ptr_array_add(place->unplaced, g_strdup(g_ptr_array_index(others.unplaced, i)));
		if (others.unknown != NULL && place->unknown == NULL)
			place->unknown = g_strdup_printf("Nothing here is offered for deletion: %s.", others.unknown);
		if (others.shared != NULL)
			place->shared = g_strdup(others.shared);
		if (others.undetermined != NULL)
			place->undetermined = g_strdup(others.undetermined);
		axis_import_others_clear(&others);
	}
	return TRUE;
}

/* Whether a live download or an open import is at @path (@name in the
 * client's folder), in it or around it: by its place recorded, by the place
 * its client says now, or by the name its client gives it. */
static gboolean
accounted(
	const Place *place,
	const gchar *path,
	const gchar *name
){
	guint i;

	for (i = 0; place->places != NULL && i < axis_db_result_get_n_rows(place->places); i++) {
		const gchar *other = axis_db_result_get_text(place->places, i, 0);

		if (other != NULL && (axis_storage_path_contains(path, other) || axis_storage_path_contains(other, path)))
			return TRUE;
	}
	for (i = 0; place->item_places != NULL && i < place->item_places->len; i++) {
		const gchar *other = g_ptr_array_index(place->item_places, i);

		if (axis_storage_path_contains(path, other) || axis_storage_path_contains(other, path))
			return TRUE;
	}
	for (i = 0; place->item_names != NULL && i < place->item_names->len; i++) {
		if (named_for(name, g_ptr_array_index(place->item_names, i), FALSE))
			return TRUE;
	}
	return FALSE;
}

/* Why @name may not be offered for deletion although nothing accounts for
 * it, or %NULL: in use at the client, or Axis cannot tell that it is not. */
static gchar *
doubt(
	const Place *place,
	const gchar *name
){
	guint i;

	if (client_internal(name))
		return g_strdup("Its name is one its client gives a download it has not finished (being unpacked, or "
			"incomplete), so Axis never deletes it.");
	if (place->unknown != NULL)
		return g_strdup(place->unknown);
	if (place->loose != NULL && place->loose->len > 0)
		return g_strdup_printf("It may be part of %s, a download its client keeps loose in this folder (with no folder "
			"of its own), whose files Axis cannot tell from what else is here.",
			(const gchar *)g_ptr_array_index(place->loose, 0));
	if (place->unplaced != NULL && place->unplaced->len > 0)
		return g_strdup_printf("It may belong to %s%s, a download Axis follows at this client whose place the client "
			"does not say yet: nothing that could be it is deleted until it does.",
			(const gchar *)g_ptr_array_index(place->unplaced, 0), place->unplaced->len > 1 ? " (or another such)" : "");
	/* Names that differ only by case or Unicode normalisation may be the
	 * same on this share. */
	for (i = 0; place->item_names != NULL && i < place->item_names->len; i++) {
		if (named_for(name, g_ptr_array_index(place->item_names, i), TRUE))
			return g_strdup_printf("It may be %s, a download its client has now: their names differ only by case or "
				"by how their letters are written, which a share may not tell apart.",
				(const gchar *)g_ptr_array_index(place->item_names, i));
	}
	for (i = 0; place->item_keys != NULL && i < place->item_keys->len; i++) {
		if (named_for(name, g_ptr_array_index(place->item_keys, i), TRUE))
			return g_strdup_printf("It may be %s, where a download its client has now is: their names differ only by "
				"case or by how their letters are written, which a share may not tell apart.",
				(const gchar *)g_ptr_array_index(place->item_keys, i));
	}
	return NULL;
}

/* The library root @path is or holds, if one: its name. */
static const gchar *
holds_root(
	const Place *place,
	const gchar *path
){
	guint i;

	for (i = 0; place->roots != NULL && i < axis_db_result_get_n_rows(place->roots); i++) {
		const gchar *root = axis_db_result_get_text(place->roots, i, 0);

		if (root != NULL && axis_storage_path_contains(path, root))
			return axis_db_result_get_text(place->roots, i, 2);
	}
	return NULL;
}

/* ── A folder's size ────────────────────────────────────────────── */

typedef struct
{
	guint64  dev;
	guint64  bytes;
	guint    names;
	gboolean partial;	/* stopped at the bound */
	gboolean other_fs;
	gboolean too_deep;
} Size;

static void
add_size(
	Size  *size,
	gint   dir,
	guint  depth
){
	gint copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR *stream = copy >= 0 ? fdopendir(copy) : NULL;
	struct dirent *entry;

	if (stream == NULL) {
		if (copy >= 0)
			close(copy);
		return;
	}
	while ((entry = readdir(stream)) != NULL) {
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (++size->names > AXIS_IMPORT_LEFTOVER_SIZE_NAMES) {
			size->partial = TRUE;
			break;
		}
		if (fstatat(dirfd(stream), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (S_ISREG(st.st_mode)) {
			size->bytes += (guint64)st.st_size;
		} else if (S_ISDIR(st.st_mode)) {
			gint child;

			if ((guint64)st.st_dev != size->dev) {
				size->other_fs = TRUE;
				continue;
			}
			if (depth + 1 > AXIS_VOLUME_FOLDER_DEPTH) {
				size->too_deep = TRUE;
				continue;
			}
			child = openat(dirfd(stream), entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
			if (child < 0)
				continue;
			add_size(size, child, depth + 1);
			close(child);
			if (size->partial)
				break;
		}
	}
	closedir(stream);
}

/* ── Listing ────────────────────────────────────────────────────── */

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* An entry of the folder: what it is, and whether Axis's rules let it go
 * now (@writable: a write guard of the volume can be had, else @why_not). */
static AxisImportLeftover *
describe(
	const Place *place,
	gint         dir,
	guint64      dev,
	const gchar *name,
	gboolean     detail,
	gboolean     writable,
	const gchar *why_not
){
	g_autofree gchar *path = g_strconcat(place->rel, "/", name, NULL);
	AxisImportLeftover *entry = g_new0(AxisImportLeftover, 1);
	GString *stored = g_string_new(NULL);
	struct stat st;
	const gchar *root;

	axis_library_name_encode(stored, name, strlen(name));
	entry->stored = g_string_free(stored, FALSE);
	entry->name = axis_library_name_display(entry->stored);
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		entry->why = g_strdup("It was gone when Axis looked at it.");
		return entry;
	}
	entry->inode = (guint64)st.st_ino;
	entry->modified_us = (gint64)st.st_mtim.tv_sec * G_USEC_PER_SEC + st.st_mtim.tv_nsec / 1000;
	entry->folder = S_ISDIR(st.st_mode);
	if (S_ISLNK(st.st_mode)) {
		entry->why = g_strdup("It is a symbolic link, which Axis never follows or deletes.");
		return entry;
	}
	if ((guint64)st.st_dev != dev) {
		entry->why = g_strdup("It is another filesystem than its volume's.");
		return entry;
	}
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
		entry->why = g_strdup("It is not a file or a folder.");
		return entry;
	}
	if (S_ISREG(st.st_mode)) {
		entry->size_bytes = (guint64)st.st_size;
	} else if (detail) {
		Size size;
		gint child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		memset(&size, 0, sizeof(size));
		size.dev = dev;
		if (child >= 0) {
			add_size(&size, child, 0);
			close(child);
		}
		entry->size_bytes = size.bytes;
		entry->size_partial = size.partial;
		if (size.other_fs) {
			entry->why = g_strdup("Another filesystem is mounted inside it.");
			return entry;
		}
		if (size.too_deep || size.names > AXIS_VOLUME_FOLDER_ENTRIES) {
			entry->why = g_strdup_printf("It holds folders more than %u deep, or more than %u names, which Axis does "
				"not delete.", (guint)AXIS_VOLUME_FOLDER_DEPTH, (guint)AXIS_VOLUME_FOLDER_ENTRIES);
			return entry;
		}
	}
	root = holds_root(place, path);
	if (root != NULL) {
		entry->why = g_strdup_printf("It is or holds the library root %s.", root);
		return entry;
	}
	entry->why = doubt(place, name);
	if (entry->why != NULL)
		return entry;
	if (!writable) {
		entry->why = g_strdup(why_not);
		return entry;
	}
	entry->deletable = TRUE;
	return entry;
}

/* One client's folder: its entries no download and no import accounts for. */
static gboolean
list_one(
	AxisImport                *self,
	const AxisDownloadClientInfo *client,
	gboolean                   detail,
	AxisImportLeftoverFolder **out,
	GError                   **error
){
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *why_not = NULL;
	AxisImportLeftoverFolder *folder = g_new0(AxisImportLeftoverFolder, 1);
	struct dirent *entry;
	Place place;
	DIR *stream;
	gint dir;
	gint copy;
	guint i;

	folder->client_id = client->id;
	folder->client_name = g_strdup(client->name);
	folder->entries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_leftover_free);
	*out = folder;
	if (!find_place(self, client->id, &place, &problem, error)) {
		folder->folder = g_strdup(place.client_folder);
		place_clear(&place);
		if (problem == NULL)
			return FALSE;
		folder->problem = g_steal_pointer(&problem);
		return TRUE;
	}
	folder->folder = g_strdup(place.client_folder);
	volume = axis_storage_get_volume(self->storage, place.volume);
	folder->volume_name = g_strdup(volume != NULL ? volume->name : NULL);
	reader = axis_volume_guard_acquire(self->storage, place.volume, AXIS_VOLUME_INTENT_READ, &local);
	if (reader == NULL) {
		folder->problem = g_strdup_printf("Its volume cannot be looked at now: %s", local->message);
		place_clear(&place);
		return TRUE;
	}
	g_clear_error(&local);
	/* Whether anything could be deleted now. */
	writer = axis_volume_guard_acquire(self->storage, place.volume, AXIS_VOLUME_INTENT_WRITE, &local);
	if (writer == NULL)
		why_not = g_strdup_printf("Nothing is deleted now: %s", local->message);
	g_clear_error(&local);
	dir = axis_volume_guard_open_dir(reader, place.rel, &local);
	if (dir < 0) {
		folder->problem = g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ?
			g_strdup_printf("Its folder for Axis (%s) is not there.", place.client_folder) :
			g_strdup_printf("Its folder for Axis (%s) cannot be opened: %s", place.client_folder, local->message);
		place_clear(&place);
		return TRUE;
	}
	copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	stream = copy >= 0 ? fdopendir(copy) : NULL;
	if (stream == NULL) {
		if (copy >= 0)
			close(copy);
		close(dir);
		folder->problem = g_strdup_printf("Its folder for Axis (%s) cannot be read.", place.client_folder);
		place_clear(&place);
		return TRUE;
	}
	while ((entry = readdir(stream)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *why = NULL;
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		path = g_strconcat(place.rel, "/", entry->d_name, NULL);
		if (accounted(&place, path, entry->d_name))
			continue;
		if (folder->n_total + folder->n_kept >= AXIS_IMPORT_LEFTOVERS_COUNTED) {
			folder->more = TRUE;
			break;
		}
		/* Counted as a leftover only what a person can act on: not what is in
		 * use at the client or cannot be told from it, nor what Axis never
		 * deletes (a link, another filesystem, a library root). */
		why = doubt(&place, entry->d_name);
		if (why == NULL && (fstatat(dirfd(stream), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
		    S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
		    (guint64)st.st_dev != axis_volume_guard_get_dev(reader) || holds_root(&place, path) != NULL))
			why = g_strdup("not Axis's to delete");
		if (why != NULL)
			folder->n_kept++;
		else
			folder->n_total++;
		g_ptr_array_add(names, g_strdup(entry->d_name));
	}
	closedir(stream);
	folder->caution = g_strdup(place.unknown);
	g_ptr_array_sort(names, compare_names);
	for (i = 0; detail && i < names->len && i < AXIS_IMPORT_LEFTOVERS_SHOWN; i++)
		g_ptr_array_add(folder->entries, describe(&place, dir, axis_volume_guard_get_dev(reader),
			g_ptr_array_index(names, i), TRUE, writer != NULL, why_not));
	close(dir);
	place_clear(&place);
	return TRUE;
}

gboolean
axis_import_leftovers(
	AxisImport  *self,
	gint64       client_id,
	gboolean     detail,
	GPtrArray  **folders,
	GError     **error
){
	g_autoptr(GPtrArray) clients = NULL;
	guint i;

	*folders = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_leftover_folder_free);
	if (!axis_downloads_list_clients(self->downloads, &clients, error))
		return FALSE;
	for (i = 0; i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		AxisImportLeftoverFolder *folder = NULL;
		g_autofree gchar *id = NULL;
		g_autofree gchar *count = NULL;
		const gchar *params[2];

		if (client_id > 0 && client->id != client_id)
			continue;
		if (!list_one(self, client, detail, &folder, error)) {
			axis_import_leftover_folder_free(folder);
			return FALSE;
		}
		/* What the Downloads page and the dashboard link to. */
		id = axis_import_int_text(client->id);
		count = folder->problem == NULL ? axis_import_int_text((gint64)folder->n_total) : NULL;
		params[0] = id;
		params[1] = count;
		if (!axis_db_pool_exec(self->pool, "UPDATE download_client SET leftover_count = $2::integer,"
		        " leftover_checked_at = now() WHERE id = $1::bigint", 2, params, NULL, error)) {
			axis_import_leftover_folder_free(folder);
			return FALSE;
		}
		g_ptr_array_add(*folders, folder);
	}
	return TRUE;
}

/* ── Deleting one ───────────────────────────────────────────────── */

gboolean
axis_import_leftover_delete(
	AxisImport                  *self,
	gint64                       client_id,
	const gchar                 *stored,
	guint64                      inode,
	AxisImportLeftoverOutcome   *outcome,
	gchar                      **text,
	GError                     **error
){
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GString) bytes = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *shown = NULL;
	const gchar *root;
	struct stat st;
	Place place;
	gint dir;

	*outcome = AXIS_IMPORT_LEFTOVER_REFUSED;
	*text = NULL;
	if (stored == NULL || !axis_library_name_decode(stored, &bytes) || bytes->len == 0 ||
	    memchr(bytes->str, '\0', bytes->len) != NULL || strchr(bytes->str, '/') != NULL ||
	    strcmp(bytes->str, ".") == 0 || strcmp(bytes->str, "..") == 0 || inode == 0) {
		*text = g_strdup("That is not one entry of a client's folder for Axis.");
		return TRUE;
	}
	shown = axis_library_name_display(stored);
	if (!find_place(self, client_id, &place, &problem, error)) {
		place_clear(&place);
		if (problem == NULL)
			return FALSE;
		*text = g_strdup_printf("Nothing was deleted: %s", problem);
		return TRUE;
	}
	path = g_strconcat(place.rel, "/", bytes->str, NULL);
	if (accounted(&place, path, bytes->str)) {
		*text = g_strdup_printf("Nothing was deleted: a download Axis follows, or an import still open, is in %s or "
			"around it now.", shown);
		place_clear(&place);
		return TRUE;
	}
	{
		g_autofree gchar *why = doubt(&place, bytes->str);

		if (why != NULL) {
			*text = g_strdup_printf("Nothing was deleted: %s", why);
			place_clear(&place);
			return TRUE;
		}
	}
	root = holds_root(&place, path);
	if (root != NULL) {
		*text = g_strdup_printf("Nothing was deleted: %s is or holds the library root %s.", shown, root);
		place_clear(&place);
		return TRUE;
	}
	/* Never while storage is read-only, or the volume offline or mounted
	 * read-only. */
	guard = axis_volume_guard_acquire(self->storage, place.volume, AXIS_VOLUME_INTENT_WRITE, &local);
	if (guard == NULL) {
		*text = g_strdup_printf("Nothing was deleted: %s", local->message);
		place_clear(&place);
		return TRUE;
	}
	g_clear_error(&local);
	dir = axis_volume_guard_open_dir(guard, place.rel, &local);
	if (dir < 0) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			*outcome = AXIS_IMPORT_LEFTOVER_GONE;
			*text = g_strdup_printf("%s was already gone: the client's folder for Axis is not there any more.", shown);
		} else {
			*text = g_strdup_printf("Nothing was deleted: the client's folder for Axis cannot be opened (%s).",
				local->message);
		}
		place_clear(&place);
		return TRUE;
	}
	place_clear(&place);
	if (fstatat(dir, bytes->str, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		gint err = errno;

		close(dir);
		if (err == ENOENT) {
			*outcome = AXIS_IMPORT_LEFTOVER_GONE;
			*text = g_strdup_printf("%s was already gone (something else deleted it after the list was made).", shown);
			return TRUE;
		}
		*text = g_strdup_printf("Nothing was deleted: %s cannot be looked at (%s).", shown, g_strerror(err));
		return TRUE;
	}
	if ((guint64)st.st_ino != inode) {
		close(dir);
		*outcome = AXIS_IMPORT_LEFTOVER_CHANGED;
		*text = g_strdup_printf("Nothing was deleted: something else has the name %s now than what the list showed. "
			"Look at the list again.", shown);
		return TRUE;
	}
	if (S_ISLNK(st.st_mode) || (guint64)st.st_dev != axis_volume_guard_get_dev(guard) ||
	    (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
		close(dir);
		*text = g_strdup_printf("Nothing was deleted: %s is a symbolic link, another filesystem, or not a file or "
			"folder.", shown);
		return TRUE;
	}
	if (S_ISREG(st.st_mode)) {
		AxisVolumeRecycledOutcome gone;

		if (!axis_volume_write_remove_file(guard, dir, bytes->str, inode, &gone, &local)) {
			close(dir);
			*text = g_strdup_printf("Nothing was deleted: %s", local->message);
			return TRUE;
		}
		close(dir);
		if (gone == AXIS_VOLUME_RECYCLED_REMOVED) {
			*outcome = AXIS_IMPORT_LEFTOVER_DELETED;
			*text = g_strdup_printf("%s was deleted.", shown);
		} else if (gone == AXIS_VOLUME_RECYCLED_GONE) {
			*outcome = AXIS_IMPORT_LEFTOVER_GONE;
			*text = g_strdup_printf("%s was already gone.", shown);
		} else {
			*outcome = AXIS_IMPORT_LEFTOVER_CHANGED;
			*text = g_strdup_printf("Nothing was deleted: %s changed while Axis looked at it.", shown);
		}
	} else {
		AxisVolumeFolderOutcome gone;
		guint removed = 0;

		if (!axis_volume_write_remove_folder(guard, dir, bytes->str, inode, &gone, &removed, &local)) {
			close(dir);
			*text = removed > 0 ?
				g_strdup_printf("%s was partly deleted (%u files and folders) and then stopped: %s", shown, removed,
					local->message) :
				g_strdup_printf("Nothing was deleted: %s", local->message);
			return TRUE;
		}
		close(dir);
		switch (gone) {
		case AXIS_VOLUME_FOLDER_REMOVED:
			*outcome = AXIS_IMPORT_LEFTOVER_DELETED;
			*text = g_strdup_printf("%s was deleted, with the %u files and folders in it.", shown, removed);
			break;
		case AXIS_VOLUME_FOLDER_GONE:
			*outcome = AXIS_IMPORT_LEFTOVER_GONE;
			*text = g_strdup_printf("%s was already gone.", shown);
			break;
		case AXIS_VOLUME_FOLDER_OTHER_FS:
			*text = g_strdup_printf("Nothing was deleted: another filesystem is mounted inside %s.", shown);
			break;
		case AXIS_VOLUME_FOLDER_TOO_BIG:
			*text = g_strdup_printf("Nothing was deleted: %s holds more than %u files and folders, or folders more than "
				"%u deep.", shown, (guint)AXIS_VOLUME_FOLDER_ENTRIES, (guint)AXIS_VOLUME_FOLDER_DEPTH);
			break;
		default:
			*outcome = AXIS_IMPORT_LEFTOVER_CHANGED;
			*text = g_strdup_printf("Nothing was deleted: %s changed while Axis looked at it.", shown);
			break;
		}
	}
	if (*outcome == AXIS_IMPORT_LEFTOVER_DELETED) {
		g_autofree gchar *id = axis_import_int_text(client_id);
		const gchar *params[1];

		g_message("leftovers: %s deleted from the folder for Axis of download client %" G_GINT64_FORMAT, shown,
			client_id);
		params[0] = id;
		axis_db_pool_exec(self->pool, "UPDATE download_client SET leftover_count = greatest(leftover_count - 1, 0)"
			" WHERE id = $1::bigint", 1, params, NULL, NULL);
	}
	return TRUE;
}
