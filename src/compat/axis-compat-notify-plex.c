/*
 * axis-compat-notify-plex.c - telling Plex which folder changed
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GET /library/sections/{id}/refresh?path=<folder as Plex sees it>, to the
 * section whose location holds the folder.  A GET: the media-server
 * connection already allows it.  When no section holds the folder nothing is
 * sent: there is no fall-back to scanning every section.
 */

#include "compat/axis-compat-notify.h"

#include <string.h>

#include "compat/axis-compat-plex.h"
#include "compat/axis-compat-private.h"
#include "jobs/axis-job-error.h"

typedef struct
{
	gchar *key;
	gchar *title;
	gchar *folder;	/* normalised */
} Location;

static void
location_free(Location *location)
{
	g_free(location->key);
	g_free(location->title);
	g_free(location->folder);
	g_free(location);
}

/* The libraries' locations as last read, one entry per folder. */
static gboolean
load_locations(
	AxisCompat  *self,
	GPtrArray  **locations,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint i;

	*locations = NULL;
	params[0] = AXIS_COMPAT_KIND_PLEX;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT l.key, l.title, f.folder FROM compat_library l"
	        " LEFT JOIN LATERAL unnest(l.folders) AS f(folder) ON true WHERE l.server_kind = $1::text"
	        " ORDER BY l.key, f.folder", 1, params, &result, error))
		return FALSE;
	*locations = g_ptr_array_new_with_free_func((GDestroyNotify)location_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Location *location = g_new0(Location, 1);

		location->key = g_strdup(axis_db_result_get_text(result, i, 0));
		location->title = g_strdup(axis_db_result_get_text(result, i, 1));
		location->folder = axis_compat_path_normalize(axis_db_result_get_text(result, i, 2), NULL);
		if (location->key == NULL || location->folder == NULL || !axis_compat_plex_id_valid(location->key))
			location_free(location);
		else
			g_ptr_array_add(*locations, location);
	}
	return TRUE;
}

/* The section whose location holds @folder: the longest location wins, then
 * the lowest key. */
static const Location *
section_for(
	GPtrArray   *locations,
	const gchar *folder
){
	const Location *best = NULL;
	guint i;

	for (i = 0; i < locations->len; i++) {
		const Location *location = g_ptr_array_index(locations, i);

		if (axis_compat_path_holds(location->folder, folder) &&
		    (best == NULL || strlen(location->folder) > strlen(best->folder)))
			best = location;
	}
	return best;
}

gboolean
axis_compat_plex_notify(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                  *items,
	GError                    **error
){
	g_autoptr(GPtrArray) locations = NULL;
	g_autoptr(GError) local = NULL;
	guint i;

	if (!load_locations(self, &locations, &local)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (locations->len == 0) {
		/* Never read: ask the server for its libraries once. */
		g_clear_error(&local);
		if (axis_compat_refresh_libraries(self, connection, cancellable, &local)) {
			g_clear_pointer(&locations, g_ptr_array_unref);
			if (!load_locations(self, &locations, &local)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
		} else if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_clear_error(&local);
	}
	for (i = 0; i < items->len; i++) {
		AxisCompatNotifyItem *item = g_ptr_array_index(items, i);
		const Location *section = section_for(locations, item->server_folder);
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *path = NULL;
		g_autoptr(GBytes) body = NULL;

		if (section == NULL) {
			item->result = AXIS_COMPAT_NOTIFY_UNMAPPED;
			item->note = g_strdup_printf("Plex has no library whose folders hold %s, so it was not asked to scan "
				"(it is never asked to scan a whole library).", item->server_folder);
			continue;
		}
		escaped = g_uri_escape_string(item->server_folder, NULL, FALSE);
		path = g_strdup_printf("/library/sections/%s/refresh?path=%s", section->key, escaped);
		if (axis_compat_fetch(self, connection, path, NULL, cancellable, &body, &local)) {
			item->result = AXIS_COMPAT_NOTIFY_SENT;
			continue;
		}
		if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		item->result = AXIS_COMPAT_NOTIFY_FAILED;
		item->note = g_strdup(local->message);
		g_clear_error(&local);
	}
	return TRUE;
}
