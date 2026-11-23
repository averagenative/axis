/*
 * axis-arr-preview.c - what a run would bring: the preview of a plan under
 * the connection's mappings and options, and saving those
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One preview serves both apps: the items, the two mapping tables, the
 * reasons and the pages are the same; a Sonarr's adds the reasons only a
 * series has, the count of each series' episode files (rows of their own,
 * read a batch at a time) and the list of the series that would be brought.
 *
 * axis_arr_preview() only reads: the plan, Axis's quality profiles, its
 * library folders for movies (or series), the titles it manages and its file
 * index.
 * The arr app is not asked again, and nothing on any disk is opened: a file
 * of the app's is looked up in the index by library folder and path, and
 * nowhere else (never by name or size nearby).
 *
 * A row of the two tables that has no saved choice shows a suggestion, worked
 * out here at each view and never stored (suggest_folders(),
 * chosen_profile()): it counts as a saved choice does, and the digest of a
 * run is of the choices in force, saved or suggested.  A suggested folder
 * only says where the lookup above is made; a file is still linked at
 * exactly its mapped path and size.
 *
 * axis_arr_save_mappings() writes arr_source.mappings and nothing else.
 */

#include "arr/axis-arr-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "download/axis-download-path.h"
#include "quality/axis-quality-store.h"
#include "storage/axis-storage-path.h"

/* Files looked up a statement. */
#define LOOKUP_BATCH 2000

/* Folder rows shown, at most. */
#define MAX_ROWS 500

/* ── Words ──────────────────────────────────────────────────────── */

static const gchar *const skip_keys[AXIS_ARR_SKIP_N] = {
	"bring", "incomplete", "twice", "no-id", "unknown", "not-looked-up", "already", "removed", "unmonitored", "profile",
	"folder", "offline", "no-tvdb-key", "not-listed",
	"unknown-kind"
};

const gchar *
axis_arr_skip_key(AxisArrSkip skip)
{
	return (guint)skip < AXIS_ARR_SKIP_N ? skip_keys[skip] : skip_keys[0];
}

gboolean
axis_arr_skip_from_key(
	const gchar *key,
	AxisArrSkip *skip
){
	guint i;

	for (i = 1; key != NULL && i < AXIS_ARR_SKIP_N; i++) {
		if (strcmp(key, skip_keys[i]) == 0) {
			*skip = (AxisArrSkip)i;
			return TRUE;
		}
	}
	return FALSE;
}

gchar *
axis_arr_skip_label(
	AxisArrSkip skip,
	AxisArrApp  app
){
	const gchar *label = axis_arr_app_label(app);
	gboolean series = app == AXIS_ARR_SONARR;

	switch (skip) {
	case AXIS_ARR_SKIP_INCOMPLETE:
		return g_strdup_printf("%s's answer for it is incomplete", label);
	case AXIS_ARR_SKIP_TWICE:
		return g_strdup_printf("%s lists it twice", label);
	case AXIS_ARR_SKIP_NO_ID:
		return g_strdup_printf("No match: %s has no %s identifier for it", label, series ? "TheTVDB" : "TMDb");
	case AXIS_ARR_SKIP_UNKNOWN:
		return g_strdup_printf("No match: TMDb no longer has the entry %s points to. "
			"It was probably removed there; nothing can be migrated for it.", label);
	case AXIS_ARR_SKIP_NOT_LOOKED_UP:
		return g_strdup(series ? "Not looked up yet: TMDb or TheTVDB did not answer" :
			"Not looked up yet: TMDb did not answer");
	case AXIS_ARR_SKIP_NO_TVDB_KEY:
		return g_strdup("No TheTVDB key in Axis");
	case AXIS_ARR_SKIP_NOT_LISTED:
		return g_strdup("TheTVDB's aired order does not list it");
	case AXIS_ARR_SKIP_UNKNOWN_KIND:
		return g_strdup_printf("%s gives a kind of series Axis does not know", label);
	case AXIS_ARR_SKIP_ALREADY:
		return g_strdup("Already in Axis");
	case AXIS_ARR_SKIP_REMOVED:
		return g_strdup("Removed in Axis after it was migrated");
	case AXIS_ARR_SKIP_UNMONITORED:
		return g_strdup_printf("Not monitored in %s", label);
	case AXIS_ARR_SKIP_PROFILE:
		return g_strdup("Quality profile not mapped");
	case AXIS_ARR_SKIP_FOLDER:
		return g_strdup("Folder not mapped");
	case AXIS_ARR_SKIP_OFFLINE:
		return g_strdup("Disk offline");
	default:
		return g_strdup("Would be migrated");
	}
}

gchar *
axis_arr_file_label(
	AxisArrFileState state,
	AxisArrApp       app
){
	const gchar *label = axis_arr_app_label(app);

	switch (state) {
	case AXIS_ARR_FILE_FOUND:
		return g_strdup("Found at its path");
	case AXIS_ARR_FILE_NOT_INDEXED:
		return g_strdup("Not in Axis's index at that path");
	case AXIS_ARR_FILE_OTHER_SIZE:
		return g_strdup_printf("Another size than %s recorded", label);
	case AXIS_ARR_FILE_LINKED_ELSEWHERE:
		return g_strdup_printf("Linked in Axis to another %s", axis_arr_app_noun(app, FALSE));
	case AXIS_ARR_FILE_UNMAPPABLE:
		return g_strdup("Path cannot be mapped");
	case AXIS_ARR_FILE_NO_PATH:
		return g_strdup_printf("%s gives no path or size for it", label);
	default:
		return g_strdup("No file");
	}
}

/* ── Freeing ────────────────────────────────────────────────────── */

static void
choice_free(gpointer data)
{
	AxisArrChoice *choice = data;

	g_free(choice->name);
	g_free(choice);
}

static void
root_choice_free(gpointer data)
{
	AxisArrRootChoice *root = data;

	g_free(root->name);
	g_free(root->volume_name);
	g_free(root);
}

static void
profile_row_free(gpointer data)
{
	AxisArrProfileRow *row = data;

	g_free(row->name);
	g_free(row);
}

static void
folder_row_free(gpointer data)
{
	AxisArrFolderRow *row = data;

	g_free(row->prefix);
	g_free(row->below);
	g_free(row);
}

static void
skip_row_free(gpointer data)
{
	AxisArrSkipRow *row = data;

	g_free(row->title);
	g_free(row->detail);
	g_free(row);
}

static void
file_row_free(gpointer data)
{
	AxisArrFileRow *row = data;

	g_free(row->title);
	g_free(row->arr_path);
	g_free(row->mapped);
	g_free(row->other);
	g_free(row);
}

static void
bring_row_free(gpointer data)
{
	AxisArrBringRow *row = data;

	g_free(row->title);
	g_free(row->seasons_on);
	g_free(row->seasons_off);
	g_free(row->profile);
	g_free(row->root);
	g_free(row->absolute);
	g_free(row->absolute_list);
	g_free(row);
}

void
axis_arr_preview_free(AxisArrPreview *preview)
{
	if (preview == NULL)
		return;
	g_clear_pointer(&preview->bringing, g_ptr_array_unref);
	g_clear_pointer(&preview->source, axis_arr_source_free);
	g_clear_pointer(&preview->plan, axis_arr_plan_free);
	g_clear_pointer(&preview->latest, axis_arr_plan_free);
	g_clear_pointer(&preview->quality_profiles, g_ptr_array_unref);
	g_clear_pointer(&preview->roots, g_ptr_array_unref);
	g_clear_pointer(&preview->profile_rows, g_ptr_array_unref);
	g_clear_pointer(&preview->folder_rows, g_ptr_array_unref);
	g_clear_pointer(&preview->skipped, g_ptr_array_unref);
	g_clear_pointer(&preview->unusable, g_ptr_array_unref);
	g_free(preview->digest);
	g_free(preview->digest_start);
	g_free(preview->choices);
	g_free(preview);
}

void
axis_arr_decision_free(gpointer data)
{
	AxisArrDecision *decision = data;

	if (decision == NULL)
		return;
	g_free(decision->other);
	g_clear_pointer(&decision->files, g_array_unref);
	g_free(decision);
}

/* ── The stored mappings ────────────────────────────────────────── */

typedef struct
{
	gchar  *prefix;
	gint64  root_id;
	gchar  *below;
} FolderMap;

static void
folder_map_free(gpointer data)
{
	FolderMap *map = data;

	g_free(map->prefix);
	g_free(map->below);
	g_free(map);
}

typedef struct
{
	GHashTable *profiles;	/* gint64 * (the app's profile) to gint64 * (Axis's, 0 for none) */
	GPtrArray  *folders;	/* FolderMap */
	gboolean    unmonitored;
} Mappings;

static void
mappings_clear(Mappings *mappings)
{
	g_clear_pointer(&mappings->profiles, g_hash_table_unref);
	g_clear_pointer(&mappings->folders, g_ptr_array_unref);
}

static gint64 *
int64_new(gint64 value)
{
	gint64 *copy = g_new(gint64, 1);

	*copy = value;
	return copy;
}

static gboolean
node_int(
	JsonNode *node,
	gint64   *value
){
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return FALSE;
	*value = json_node_get_int(node);
	return TRUE;
}

static const gchar *
node_string(JsonNode *node)
{
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

/* What the connection stores, read as carefully as anything else: a member
 * out of shape is as if it were not there.  @unmonitored is what the option
 * to bring the titles the app does not monitor is until a person has saved a
 * choice: on for Sonarr (the series a person has files for stay outside Axis
 * otherwise), off for Radarr.  A choice saved is read as it was saved. */
static void
mappings_parse(
	const gchar *text,
	gboolean     unmonitored,
	Mappings    *mappings
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	JsonNode *node;

	mappings->profiles = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	mappings->folders = g_ptr_array_new_with_free_func(folder_map_free);
	mappings->unmonitored = unmonitored;
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL) ||
	    json_parser_get_root(parser) == NULL || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return;
	root = json_node_get_object(json_parser_get_root(parser));
	node = json_object_get_member(root, "profiles");
	if (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) {
		JsonObject *profiles = json_node_get_object(node);
		JsonObjectIter iter;
		const gchar *name;
		JsonNode *value;

		json_object_iter_init(&iter, profiles);
		while (json_object_iter_next(&iter, &name, &value)) {
			gchar *end = NULL;
			gint64 arr_id = g_ascii_strtoll(name, &end, 10);
			gint64 chosen = 0;

			if (end == name || *end != '\0' || arr_id <= 0 || !node_int(value, &chosen) || chosen < 0 ||
			    g_hash_table_size(mappings->profiles) >= AXIS_ARR_MAX_PROFILES)
				continue;
			g_hash_table_insert(mappings->profiles, int64_new(arr_id), int64_new(chosen));
		}
	}
	node = json_object_get_member(root, "folders");
	if (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) {
		JsonArray *folders = json_node_get_array(node);
		guint i;

		for (i = 0; i < json_array_get_length(folders) && mappings->folders->len < AXIS_ARR_MAX_FOLDER_ROWS; i++) {
			JsonNode *entry = json_array_get_element(folders, i);
			JsonObject *row;
			FolderMap *map;
			gchar *prefix = NULL;
			gchar *below = NULL;
			gint64 root_id = 0;

			if (entry == NULL || !JSON_NODE_HOLDS_OBJECT(entry))
				continue;
			row = json_node_get_object(entry);
			/* (A row with no library folder, "root" 0, is a person's choice
			 * of "not mapped": it keeps a suggestion away.) */
			if (!node_int(json_object_get_member(row, "root"), &root_id) || root_id < 0 ||
			    !axis_download_path_map_normalize(node_string(json_object_get_member(row, "prefix")),
			        node_string(json_object_get_member(row, "below")), &prefix, &below, NULL))
				continue;
			map = g_new0(FolderMap, 1);
			map->prefix = prefix;
			map->root_id = root_id;
			map->below = below;
			g_ptr_array_add(mappings->folders, map);
		}
	}
	node = json_object_get_member(root, "unmonitored");
	if (node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		mappings->unmonitored = json_node_get_boolean(node);
}

gboolean
axis_arr_save_mappings(
	AxisArr                    *self,
	gint64                      source_id,
	const AxisArrProfileInput  *profiles,
	guint                       n_profiles,
	const AxisArrFolderInput   *folders,
	guint                       n_folders,
	gboolean                    unmonitored,
	GError                    **error
){
	g_autoptr(JsonObject) root = json_object_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *text = NULL;
	JsonObject *profile_object = json_object_new();
	JsonArray *folder_array = json_array_new();
	const gchar *params[2];
	guint kept = 0;
	guint pass;
	guint i;

	json_object_set_object_member(root, "profiles", profile_object);
	json_object_set_array_member(root, "folders", folder_array);
	json_object_set_boolean_member(root, "unmonitored", unmonitored);
	for (i = 0; i < n_profiles && i < AXIS_ARR_MAX_PROFILES; i++) {
		g_autofree gchar *name = g_strdup_printf("%" G_GINT64_FORMAT, profiles[i].arr_id);

		if (profiles[i].arr_id <= 0 || profiles[i].chosen < 0)
			continue;
		json_object_set_int_member(profile_object, name, profiles[i].chosen);
	}
	/* The rows that map first, then (while there is room) the rows a person
	 * left at "not mapped": that is a choice too, kept so that no suggestion
	 * takes its place. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < n_folders; i++) {
			g_autofree gchar *typed = folders[i].prefix != NULL ? g_strstrip(g_strdup(folders[i].prefix)) : NULL;
			g_autofree gchar *prefix = NULL;
			g_autofree gchar *below = NULL;
			const gchar *problem = NULL;
			gboolean maps = folders[i].root_id > 0;
			JsonObject *row;

			/* A row with nothing typed is no row. */
			if (typed == NULL || typed[0] == '\0' || maps != (pass == 0))
				continue;
			if (!axis_download_path_map_normalize(typed, maps ? folders[i].below : NULL, &prefix, &below, &problem)) {
				/* @problem is a fixed sentence that does not repeat what was typed. */
				g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "A folder row was refused. %s",
					problem != NULL ? problem : "Its path is not valid.");
				return FALSE;
			}
			if (g_hash_table_contains(seen, prefix))
				continue;
			if (kept >= AXIS_ARR_MAX_FOLDER_ROWS) {
				if (!maps)
					continue;
				g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "At most %d folders can be mapped.",
					AXIS_ARR_MAX_FOLDER_ROWS);
				return FALSE;
			}
			kept++;
			g_hash_table_add(seen, g_strdup(prefix));
			row = json_object_new();
			json_object_set_string_member(row, "prefix", prefix);
			json_object_set_int_member(row, "root", maps ? folders[i].root_id : 0);
			json_object_set_string_member(row, "below", below);
			json_array_add_object_element(folder_array, row);
		}
	}
	json_node_set_object(node, root);
	json_generator_set_root(generator, node);
	text = json_generator_to_data(generator, NULL);
	params[0] = id_text;
	params[1] = text;
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE arr_source SET mappings = $2::jsonb, updated_at = now() WHERE id = $1::bigint", 2, params, &result,
	        error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "There is no such connection.");
		return FALSE;
	}
	return TRUE;
}

/* ── The plan in memory ─────────────────────────────────────────── */

/* What is kept of a movie while the plan is counted: no title and, once it
 * is decided, no path; a title and a path are read again for the rows of
 * the page shown (a plan has up to 100,000, and several previews run at
 * once). */
typedef struct
{
	guint             seq;
	gint              year;
	gboolean          monitored;
	gint64            profile_id;
	gint64            arr_id;
	gint64            ext_id;	/* its TMDb identifier (a movie), its TheTVDB identifier (a series) */
	/* A series: Sonarr's type of it, and what its two lists held (n_episodes
	 * is -1 when they were not read). */
	gboolean          anime;
	gboolean          unknown_kind;	/* Sonarr's kind of series is a word Axis does not know */
	gint              n_episodes;
	guint             n_episodes_monitored;
	guint             n_episodes_with_file;
	guint             n_files;
	guint             n_odd;
	guint             n_seasons_on;	/* the seasons Sonarr monitors */
	guint             n_found;	/* of its files, those in the index at their path and of their size */
	gint              shown_row;	/* the row it is counted under, -1 for none */
	gchar            *path;		/* normalised, or NULL when it is no path; until decided */
	gboolean          has_file;
	gchar            *file_path;	/* until decided */
	gint64            file_size;
	gchar            *match;
	gint64            title_id;
	gboolean          managed;
	gboolean          brought_before;	/* by this connection (arr_brought) */
	gboolean          brought_here;	/* ... and that movie is the one in Axis now, under this entry's number */
	gboolean          hold_unseen;	/* the movie is held as its file was not seen */
	/* Decided here. */
	gint64            to_profile;	/* Axis's profile it would get */
	gint64            to_root;	/* the library folder it would get */
	gint64            file_id;	/* its file's row of the index, when it is there */
	gboolean          file_linked;	/* that file is linked to a movie */
	gboolean          file_unlinked;	/* a person unlinked it by hand */
	gboolean          file_identified;	/* its link is identification's alone: not kept, no hand stamp, no import */
	AxisArrSkip       skip;
	gint              mapped_row;	/* the row that maps it, -1 for none */
	AxisArrFileState  file;
	gint              file_row;	/* the row its file is under, -1 for none */
	gint64            file_root;
	gchar            *file_stored;
	gchar            *other;
	/* A series: its episode files for the run (only when a run asks: a series
	 * that would be brought gets every one, one brought before only those to
	 * link now), and how many files a run would link now of one brought
	 * before. */
	GArray           *file_decisions;	/* (nullable) AxisArrFileDecision */
	guint             n_relink;
} Item;

static void
item_free(gpointer data)
{
	Item *item = data;

	g_clear_pointer(&item->file_decisions, g_array_unref);
	g_free(item->path);
	g_free(item->file_path);
	g_free(item->match);
	g_free(item->file_stored);
	g_free(item->other);
	g_free(item);
}

typedef struct
{
	AxisArr        *self;
	AxisDbConn     *conn;
	AxisArrPreview *preview;
	Mappings        mappings;
	GPtrArray      *items;		/* Item */
	GPtrArray      *maps;		/* AxisDownloadPathMap: the rows that map */
	GHashTable     *seen_tmdb;	/* gint64 *: the identifiers an item took */
	GHashTable     *seen_arr;	/* gint64 *: the app's ids an item took */
	GPtrArray      *decisions;	/* (nullable) AxisArrDecision: what a run would do, for the run */
	gboolean        sonarr;	/* the titles are series */
	/* What a profile of the app's with no saved choice and no namesake in
	 * Axis is set to: the profile new movies (or series) get by the saved
	 * defaults of the person who read the app, else Axis's default profile;
	 * 0 for none. */
	gint64          person_profile;
	gint64          default_profile;
	AxisArrPages    pages;
	/* The plan's seq of each row of the three pages shown. */
	GArray         *skip_seqs;
	GArray         *file_seqs;
	GArray         *bring_seqs;
} Work;

static AxisArrRootChoice *
find_root(
	AxisArrPreview *preview,
	gint64          id
){
	guint i;

	for (i = 0; id > 0 && i < preview->roots->len; i++) {
		AxisArrRootChoice *root = g_ptr_array_index(preview->roots, i);

		if (root->id == id)
			return root;
	}
	return NULL;
}

static gboolean
has_profile(
	AxisArrPreview *preview,
	gint64          id
){
	guint i;

	for (i = 0; id > 0 && i < preview->quality_profiles->len; i++) {
		if (((const AxisArrChoice *)g_ptr_array_index(preview->quality_profiles, i))->id == id)
			return TRUE;
	}
	return FALSE;
}

static gboolean
load_choices(
	Work    *work,
	GError **error
){
	g_autoptr(AxisDbResult) profiles = NULL;
	g_autoptr(AxisDbResult) roots = NULL;
	g_autoptr(AxisDbResult) defaults = NULL;
	AxisArrPreview *preview = work->preview;
	const gchar *params[3];
	guint i;

	/* The profile a title added from a list gets: by the defaults the person
	 * who read the app saved for that kind ("Save as my defaults": the kinds
	 * are AXIS_MOVIES_WEB_DEFAULTS_KIND and AXIS_TV_WEB_DEFAULTS_KIND), and by
	 * Axis's default profile.  The reader's, not the viewer's: the page and
	 * the run then decide alike, whoever looks. */
	params[0] = work->sonarr ? "series" : "movies";
	params[1] = preview->plan->started_by;
	params[2] = AXIS_QUALITY_DEFAULT_SLUG;
	if (!axis_db_conn_exec(work->conn,
	        "SELECT coalesce((SELECT p.id FROM app_user u JOIN quality_profile p"
	        "   ON p.id = CASE WHEN jsonb_typeof(u.add_defaults -> $1::text -> 'profile') = 'number'"
	        "    AND (u.add_defaults -> $1::text ->> 'profile') ~ '^[0-9]{1,18}$'"
	        "    THEN (u.add_defaults -> $1::text ->> 'profile')::bigint END"
	        "  WHERE u.username = $2::text), 0),"
	        " coalesce((SELECT p.id FROM quality_profile p WHERE p.slug = coalesce((SELECT s.value #>> '{}' FROM setting s"
	        "  WHERE s.key = '" AXIS_QUALITY_SETTING_DEFAULT "' AND jsonb_typeof(s.value) = 'string'), $3::text)), 0)", 3, params,
	        &defaults, error))
		return FALSE;
	if (axis_db_result_get_n_rows(defaults) > 0) {
		work->person_profile = axis_arr_int_at(defaults, 0, 0, 0);
		work->default_profile = axis_arr_int_at(defaults, 0, 1, 0);
	}
	/* The library folders of the app's media type: movies, or series. */
	params[0] = work->sonarr ? "tv" : "movies";
	if (!axis_db_conn_exec(work->conn, "SELECT id, name FROM quality_profile ORDER BY lower(name), id", 0, NULL,
	        &profiles, error) ||
	    !axis_db_conn_exec(work->conn,
	        "SELECT r.id, r.name, v.name, v.state IN ('online', 'read_only'),"
	        " " AXIS_ARR_US("(SELECT max(s.finished_at) FROM library_scan s WHERE s.root_id = r.id"
	        " AND s.status = 'complete')")
	        " FROM library_root r JOIN volume v ON v.id = r.volume_id WHERE r.media_type = $1"
	        " ORDER BY lower(r.name), r.id", 1, params, &roots, error))
		return FALSE;
	preview->quality_profiles = g_ptr_array_new_with_free_func(choice_free);
	for (i = 0; i < axis_db_result_get_n_rows(profiles); i++) {
		AxisArrChoice *choice = g_new0(AxisArrChoice, 1);

		choice->id = axis_arr_int_at(profiles, i, 0, 0);
		choice->name = axis_arr_text_at(profiles, i, 1);
		g_ptr_array_add(preview->quality_profiles, choice);
	}
	preview->roots = g_ptr_array_new_with_free_func(root_choice_free);
	for (i = 0; i < axis_db_result_get_n_rows(roots); i++) {
		AxisArrRootChoice *root = g_new0(AxisArrRootChoice, 1);

		root->id = axis_arr_int_at(roots, i, 0, 0);
		root->name = axis_arr_text_at(roots, i, 1);
		root->volume_name = axis_arr_text_at(roots, i, 2);
		root->online = axis_arr_bool_at(roots, i, 3);
		root->scanned_us = axis_arr_int_at(roots, i, 4, 0);
		g_ptr_array_add(preview->roots, root);
	}
	return TRUE;
}

static AxisArrFolderRow *
find_row(
	AxisArrPreview *preview,
	const gchar    *prefix,
	gint           *index
){
	guint i;

	for (i = 0; i < preview->folder_rows->len; i++) {
		AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);

		if (strcmp(row->prefix, prefix) == 0) {
			if (index != NULL)
				*index = (gint)i;
			return row;
		}
	}
	return NULL;
}

static AxisArrFolderRow *
add_row(
	AxisArrPreview *preview,
	const gchar    *prefix
){
	AxisArrFolderRow *row = find_row(preview, prefix, NULL);

	if (row != NULL || preview->folder_rows->len >= MAX_ROWS)
		return row;
	row = g_new0(AxisArrFolderRow, 1);
	row->prefix = g_strdup(prefix);
	row->below = g_strdup("");
	row->accessible = TRUE;
	/* Until a saved choice or a suggestion says otherwise. */
	row->from = AXIS_ARR_FROM_NOTHING;
	g_ptr_array_add(preview->folder_rows, row);
	return row;
}

static gint
compare_rows(
	gconstpointer a,
	gconstpointer b
){
	const AxisArrFolderRow *first = *(AxisArrFolderRow *const *)a;
	const AxisArrFolderRow *second = *(AxisArrFolderRow *const *)b;

	return strcmp(first->prefix, second->prefix);
}

/* The app's profiles and root folders, as the reading stored them. */
static gboolean
load_plan_lists(
	Work    *work,
	GError **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	AxisArrPreview *preview = work->preview;
	const gchar *params[1];
	guint i;

	params[0] = plan_text;
	if (!axis_db_conn_exec(work->conn, "SELECT profiles::text, folders::text FROM arr_plan WHERE id = $1::bigint", 1,
	        params, &result, error))
		return FALSE;
	preview->profile_rows = g_ptr_array_new_with_free_func(profile_row_free);
	preview->folder_rows = g_ptr_array_new_with_free_func(folder_row_free);
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	if (json_parser_load_from_data(parser, axis_db_result_get_text(result, 0, 0), -1, NULL) &&
	    json_parser_get_root(parser) != NULL && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) {
		JsonArray *array = json_node_get_array(json_parser_get_root(parser));

		for (i = 0; i < json_array_get_length(array) && i < AXIS_ARR_MAX_PROFILES; i++) {
			JsonNode *entry = json_array_get_element(array, i);
			AxisArrProfileRow *row;
			const gchar *name;
			gint64 id = 0;

			if (entry == NULL || !JSON_NODE_HOLDS_OBJECT(entry))
				continue;
			name = node_string(json_object_get_member(json_node_get_object(entry), "name"));
			if (!node_int(json_object_get_member(json_node_get_object(entry), "id"), &id) || name == NULL)
				continue;
			row = g_new0(AxisArrProfileRow, 1);
			row->arr_id = id;
			row->name = g_strdup(name);
			g_ptr_array_add(preview->profile_rows, row);
		}
	}
	if (json_parser_load_from_data(parser, axis_db_result_get_text(result, 0, 1), -1, NULL) &&
	    json_parser_get_root(parser) != NULL && JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) {
		JsonArray *array = json_node_get_array(json_parser_get_root(parser));

		for (i = 0; i < json_array_get_length(array) && i < AXIS_ARR_MAX_FOLDERS; i++) {
			JsonNode *entry = json_array_get_element(array, i);
			g_autofree gchar *prefix = NULL;
			AxisArrFolderRow *row;
			JsonNode *accessible;

			if (entry == NULL || !JSON_NODE_HOLDS_OBJECT(entry) ||
			    !axis_storage_path_normalize_exact(node_string(json_object_get_member(json_node_get_object(entry),
			        "path")), TRUE, &prefix, NULL))
				continue;
			row = add_row(preview, prefix);
			if (row == NULL)
				continue;
			row->is_root = TRUE;
			accessible = json_object_get_member(json_node_get_object(entry), "accessible");
			if (accessible != NULL && JSON_NODE_HOLDS_VALUE(accessible) &&
			    json_node_get_value_type(accessible) == G_TYPE_BOOLEAN)
				row->accessible = json_node_get_boolean(accessible);
		}
	}
	return TRUE;
}

/* The folders the movies live under are rows too, root folder or not: the
 * first MAX_ROWS in the order of the plan. */
static gboolean
load_roots(
	Work    *work,
	GError **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	const gchar *params[1];
	guint i;

	params[0] = plan_text;
	if (!axis_db_conn_exec(work->conn,
	        "SELECT root_path FROM arr_plan_item WHERE plan_id = $1::bigint AND root_path IS NOT NULL"
	        " GROUP BY root_path ORDER BY min(seq) LIMIT 1000", 1, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		g_autofree gchar *root_path = NULL;

		if (axis_storage_path_normalize_exact(axis_db_result_get_text(result, i, 0), TRUE, &root_path, NULL))
			add_row(work->preview, root_path);
	}
	return TRUE;
}

/* The longest row that contains @path, by whole segments; among the rows
 * that map (a library folder is chosen) when @mapped, else among all. */
static gint
row_of(
	AxisArrPreview *preview,
	const gchar    *path,
	gboolean        mapped
){
	gsize best_length = 0;
	gint best = -1;
	guint i;

	for (i = 0; path != NULL && i < preview->folder_rows->len; i++) {
		const AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);
		gsize length = strlen(row->prefix);

		if ((mapped && row->root_id <= 0) || !axis_storage_path_contains(row->prefix, path))
			continue;
		if (best < 0 || length > best_length) {
			best = (gint)i;
			best_length = length;
		}
	}
	return best;
}

/* The profile of Axis's an arr profile becomes: the one a person saved
 * ("none" too); else, suggested, the one of the same name, the default of
 * the person who read the app, Axis's default profile; else none. */
static gint64
chosen_profile(
	Work              *work,
	AxisArrProfileRow *row
){
	const gint64 *stored = g_hash_table_lookup(work->mappings.profiles, &row->arr_id);
	guint i;

	row->from = AXIS_ARR_FROM_SAVED;
	if (stored != NULL)
		return has_profile(work->preview, *stored) ? *stored : 0;
	row->from = AXIS_ARR_FROM_NAME;
	for (i = 0; i < work->preview->quality_profiles->len; i++) {
		const AxisArrChoice *choice = g_ptr_array_index(work->preview->quality_profiles, i);
		g_autofree gchar *ours = g_utf8_casefold(choice->name, -1);
		g_autofree gchar *theirs = g_utf8_casefold(row->name, -1);

		if (strcmp(ours, theirs) == 0)
			return choice->id;
	}
	row->from = AXIS_ARR_FROM_PERSON;
	if (has_profile(work->preview, work->person_profile))
		return work->person_profile;
	row->from = AXIS_ARR_FROM_DEFAULT;
	if (has_profile(work->preview, work->default_profile))
		return work->default_profile;
	row->from = AXIS_ARR_FROM_NOTHING;
	return 0;
}

static AxisArrProfileRow *
profile_row_of(
	Work   *work,
	gint64  arr_id
){
	AxisArrProfileRow *row;
	guint i;

	for (i = 0; i < work->preview->profile_rows->len; i++) {
		row = g_ptr_array_index(work->preview->profile_rows, i);
		if (row->arr_id == arr_id)
			return row;
	}
	/* A profile the app's own list did not name. */
	if (work->preview->profile_rows->len >= AXIS_ARR_MAX_PROFILES)
		return NULL;
	row = g_new0(AxisArrProfileRow, 1);
	row->arr_id = arr_id;
	row->name = g_strdup_printf("Profile %" G_GINT64_FORMAT, arr_id);
	row->chosen = chosen_profile(work, row);
	g_ptr_array_add(work->preview->profile_rows, row);
	return row;
}

/* ── Folders suggested from the index ───────────────────────────── */

/* One place Axis's index holds some of a row's sampled files at: a library
 * folder and a path below it. */
typedef struct
{
	guint   row;
	gint64  root;
	gchar  *below;
	guint   n;
} Place;

static void
place_free(gpointer data)
{
	Place *place = data;

	g_free(place->below);
	g_free(place);
}

/* The app's files a suggestion is decided on: at most AXIS_ARR_SUGGEST_SAMPLE
 * for each of its root folders, AXIS_ARR_SUGGEST_MAX in all, the first of
 * each title before the second of any (a series' files are rows of their
 * own, a movie's file is on its item). */
static const gchar *const series_sample_sql =
	"SELECT s.path, s.size FROM (SELECT f.path, f.size, f.seq, f.ord,"
	" row_number() OVER (PARTITION BY i.root_path ORDER BY f.ord, f.seq) AS n"
	" FROM arr_plan_file f JOIN arr_plan_item i ON i.plan_id = f.plan_id AND i.seq = f.seq"
	" WHERE f.plan_id = $1::bigint AND f.path IS NOT NULL AND f.size IS NOT NULL) s"
	" WHERE s.n <= " G_STRINGIFY(AXIS_ARR_SUGGEST_SAMPLE)
	" ORDER BY s.n, s.seq, s.ord LIMIT " G_STRINGIFY(AXIS_ARR_SUGGEST_MAX);

static const gchar *const movie_sample_sql =
	"SELECT s.file_path, s.file_size FROM (SELECT i.file_path, i.file_size, i.seq,"
	" row_number() OVER (PARTITION BY i.root_path ORDER BY i.seq) AS n"
	" FROM arr_plan_item i WHERE i.plan_id = $1::bigint AND i.has_file AND i.file_path IS NOT NULL"
	" AND i.file_size IS NOT NULL) s"
	" WHERE s.n <= " G_STRINGIFY(AXIS_ARR_SUGGEST_SAMPLE)
	" ORDER BY s.n, s.seq LIMIT " G_STRINGIFY(AXIS_ARR_SUGGEST_MAX);

/*
 * The library folder, and the path below it, of each of the app's folders
 * that has no saved choice, worked out from Axis's own index: a sample of
 * the app's files under the folder is looked for among the indexed files of
 * the same size in the library folders of the app's media type, each at the
 * path the app gives it below its folder (so the series' or the movie's
 * folder name and all that follows), below the library folder itself or below
 * any folder in it.  The one place that holds at least
 * AXIS_ARR_SUGGEST_PERCENT in a hundred of the sample, and at least
 * AXIS_ARR_SUGGEST_MIN files (all of them, of fewer), is the suggestion; two
 * such places, or none, suggest nothing, and the row says why.
 *
 * Reads only, and only the plan and the index: no file or folder is opened.
 * Two statements a view (the second once for each LOOKUP_BATCH files), and
 * none once every row has a saved choice.
 */
static gboolean
suggest_folders(
	Work    *work,
	GError **error
){
	g_autoptr(GPtrArray) maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);
	g_autoptr(GHashTable) places = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, place_free);
	g_autoptr(AxisDbResult) sample = NULL;
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	g_autofree guint *reach = NULL;
	g_autofree Place **pick = NULL;
	AxisArrPreview *preview = work->preview;
	GPtrArray *rows = preview->folder_rows;
	GHashTableIter iter;
	gpointer value;
	const gchar *params[2];
	gboolean any = FALSE;
	guint next = 0;
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisArrFolderRow *row = g_ptr_array_index(rows, i);

		any = any || row->from == AXIS_ARR_FROM_NOTHING;
		/* (The "library folder" of these maps is the row's place, from 1.) */
		g_ptr_array_add(maps, axis_download_path_map_new(row->prefix, (gint64)i + 1, ""));
	}
	/* (With no library folder of the kind there is nothing to suggest, and
	 * the page says that there is none.) */
	if (!any || preview->roots->len == 0)
		return TRUE;
	params[0] = plan_text;
	if (!axis_db_conn_exec(work->conn, work->sonarr ? series_sample_sql : movie_sample_sql, 1, params, &sample, error))
		return FALSE;
	while (next < axis_db_result_get_n_rows(sample)) {
		g_autoptr(JsonArray) asked = json_array_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *text = NULL;

		for (; next < axis_db_result_get_n_rows(sample) && json_array_get_length(asked) < LOOKUP_BATCH; next++) {
			g_autofree gchar *stored = NULL;
			AxisArrFolderRow *row;
			JsonObject *one;
			gint64 place = 0;

			/* The file's path below the longest of the app's folders that
			 * holds it, as the preview itself would map it. */
			if (axis_arr_map_path(maps, axis_db_result_get_text(sample, next, 0), &place, &stored) !=
			    AXIS_ARR_PATH_MAPPED || place <= 0 || place > rows->len)
				continue;
			row = g_ptr_array_index(rows, (guint)(place - 1));
			if (row->from != AXIS_ARR_FROM_NOTHING)
				continue;
			row->n_sampled++;
			one = json_object_new();
			json_object_set_int_member(one, "ord", next);
			json_object_set_int_member(one, "r", place - 1);
			json_object_set_string_member(one, "p", stored);
			json_object_set_int_member(one, "s", axis_arr_int_at(sample, next, 1, -1));
			json_array_add_object_element(asked, one);
		}
		if (json_array_get_length(asked) == 0)
			continue;
		json_node_set_array(node, asked);
		json_generator_set_root(generator, node);
		text = json_generator_to_data(generator, NULL);
		params[0] = text;
		params[1] = work->sonarr ? "tv" : "movies";
		/* By size first (library_file_size_idx), then the path: the file's
		 * own, or one that ends with "/" and the file's. */
		if (!axis_db_conn_exec(work->conn,
		        "SELECT q.r, f.root_id, left(f.rel_path, length(f.rel_path) - length(q.p)), count(DISTINCT q.ord)"
		        " FROM jsonb_to_recordset($1::jsonb) AS q(ord integer, r integer, p text, s bigint)"
		        " JOIN library_file f ON f.size = q.s AND f.state = 'present'"
		        "  AND (f.rel_path = q.p OR right(f.rel_path, length(q.p) + 1) = '/' || q.p)"
		        " JOIN library_root lr ON lr.id = f.root_id AND lr.media_type = $2"
		        " GROUP BY 1, 2, 3", 2, params, &result, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			gint64 at = axis_arr_int_at(result, i, 0, -1);
			gint64 root = axis_arr_int_at(result, i, 1, 0);
			const gchar *below = axis_db_result_get_text(result, i, 2);
			g_autofree gchar *key = NULL;
			Place *place;

			if (at < 0 || at >= rows->len || root <= 0)
				continue;
			key = g_strdup_printf("%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%s", at, root, below != NULL ? below : "");
			place = g_hash_table_lookup(places, key);
			if (place == NULL) {
				place = g_new0(Place, 1);
				place->row = (guint)at;
				place->root = root;
				place->below = g_strdup(below != NULL ? below : "");
				/* ("Shows/" is the folder Shows.) */
				if (g_str_has_suffix(place->below, "/"))
					place->below[strlen(place->below) - 1] = '\0';
				g_hash_table_insert(places, g_steal_pointer(&key), place);
			}
			place->n += (guint)axis_arr_int_at(result, i, 3, 0);
		}
	}
	reach = g_new0(guint, rows->len + 1);
	pick = g_new0(Place *, rows->len + 1);
	g_hash_table_iter_init(&iter, places);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Place *place = value;
		AxisArrFolderRow *row = g_ptr_array_index(rows, place->row);
		guint need = (row->n_sampled * AXIS_ARR_SUGGEST_PERCENT + 99) / 100;

		need = MAX(need, MIN(row->n_sampled, AXIS_ARR_SUGGEST_MIN));
		row->n_sample_found = MAX(row->n_sample_found, place->n);
		if (place->n >= need) {
			reach[place->row]++;
			pick[place->row] = place;
		}
	}
	for (i = 0; i < rows->len; i++) {
		AxisArrFolderRow *row = g_ptr_array_index(rows, i);
		g_autofree gchar *prefix = NULL;
		g_autofree gchar *below = NULL;

		if (row->from != AXIS_ARR_FROM_NOTHING)
			continue;
		if (row->n_sampled == 0) {
			row->unsure = AXIS_ARR_UNSURE_NO_FILES;
		} else if (reach[i] > 1) {
			row->unsure = AXIS_ARR_UNSURE_TWO;
		} else if (reach[i] == 0 || find_root(preview, pick[i]->root) == NULL ||
		           !axis_download_path_map_normalize(row->prefix, pick[i]->below, &prefix, &below, NULL)) {
			row->unsure = AXIS_ARR_UNSURE_NOT_FOUND;
		} else {
			row->from = AXIS_ARR_FROM_FILES;
			row->root_id = pick[i]->root;
			row->n_sample_found = pick[i]->n;
			g_free(row->below);
			row->below = g_steal_pointer(&below);
		}
	}
	return TRUE;
}

/* The rows, with what the connection maps them to: a person's saved choices,
 * and for the rows that have none what Axis suggests. */
static gboolean
apply_mappings(
	Work    *work,
	GError **error
){
	AxisArrPreview *preview = work->preview;
	guint i;

	for (i = 0; i < work->mappings.folders->len; i++) {
		const FolderMap *map = g_ptr_array_index(work->mappings.folders, i);
		AxisArrFolderRow *row;

		/* "Not mapped", saved: the row stays so, and gets no suggestion.
		 * (Of a folder that is no row of the page it says nothing.) */
		if (map->root_id <= 0) {
			row = find_row(preview, map->prefix, NULL);
			if (row != NULL)
				row->from = AXIS_ARR_FROM_SAVED;
			continue;
		}
		row = add_row(preview, map->prefix);
		if (row == NULL)
			continue;
		row->from = AXIS_ARR_FROM_SAVED;
		/* A library folder that is gone, or is not one for movies, maps
		 * nothing. */
		if (find_root(preview, map->root_id) == NULL)
			continue;
		row->root_id = map->root_id;
		g_free(row->below);
		row->below = g_strdup(map->below);
	}
	g_ptr_array_sort(preview->folder_rows, compare_rows);
	if (!suggest_folders(work, error))
		return FALSE;
	work->maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);
	for (i = 0; i < preview->folder_rows->len; i++) {
		const AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);

		if (row->root_id > 0)
			g_ptr_array_add(work->maps, axis_download_path_map_new(row->prefix, row->root_id, row->below));
	}
	for (i = 0; i < preview->profile_rows->len; i++) {
		AxisArrProfileRow *row = g_ptr_array_index(preview->profile_rows, i);

		row->chosen = chosen_profile(work, row);
	}
	preview->unmonitored = work->mappings.unmonitored;
	return TRUE;
}

/* Whether @key is new to @set, and then in it. */
static gboolean
first_of(
	GHashTable *set,
	gint64      key
){
	if (key <= 0 || g_hash_table_contains(set, &key))
		return key <= 0;
	g_hash_table_add(set, int64_new(key));
	return TRUE;
}

/* Whether, and why not, each title from @from on would be brought; where
 * its file is.  Its path and its file's are of no use after. */
static void
decide(
	Work *work,
	guint from
){
	AxisArrPreview *preview = work->preview;
	guint i;

	for (i = from; i < work->items->len; i++) {
		Item *item = g_ptr_array_index(work->items, i);
		gboolean usable = g_strcmp0(item->match, "incomplete") != 0;
		gboolean first_tmdb = usable ? first_of(work->seen_tmdb, item->ext_id) : TRUE;
		gboolean first_arr = usable ? first_of(work->seen_arr, item->arr_id) : TRUE;
		gint shown;

		preview->n_titles++;
		if (usable && item->monitored)
			preview->n_monitored++;
		if (usable && item->has_file)
			preview->n_with_file++;
		/* A series whose two lists were read: what they held. */
		if (usable && item->n_episodes >= 0) {
			preview->n_series_read++;
			preview->n_episodes += (guint)item->n_episodes;
			preview->n_episodes_monitored += item->n_episodes_monitored;
			preview->n_episodes_with_file += item->n_episodes_with_file;
			preview->n_odd += item->n_odd;
		}
		item->mapped_row = usable ? row_of(preview, item->path, TRUE) : -1;
		shown = item->mapped_row >= 0 ? item->mapped_row : usable ? row_of(preview, item->path, FALSE) : -1;
		item->shown_row = shown;
		if (shown >= 0)
			((AxisArrFolderRow *)g_ptr_array_index(preview->folder_rows, (guint)shown))->n_titles++;

		/* Its file: under which row, and at which place in Axis. */
		if (usable && item->has_file) {
			g_autofree gchar *normal = NULL;

			if (item->file_path == NULL || item->file_size < 0) {
				item->file = AXIS_ARR_FILE_NO_PATH;
				item->file_row = shown;
			} else if (axis_arr_map_path(work->maps, item->file_path, &item->file_root, &item->file_stored) ==
			           AXIS_ARR_PATH_MAPPED) {
				axis_storage_path_normalize_exact(item->file_path, TRUE, &normal, NULL);
				item->file_row = row_of(preview, normal, TRUE);
				/* Until the index says otherwise. */
				item->file = AXIS_ARR_FILE_NOT_INDEXED;
			} else {
				axis_storage_path_normalize_exact(item->file_path, TRUE, &normal, NULL);
				item->file_row = row_of(preview, normal, FALSE);
				item->file = AXIS_ARR_FILE_UNMAPPABLE;
			}
			if (item->file_row >= 0)
				((AxisArrFolderRow *)g_ptr_array_index(preview->folder_rows, (guint)item->file_row))->n_files++;
		}

		if (!usable) {
			item->skip = AXIS_ARR_SKIP_INCOMPLETE;
		} else if (!first_tmdb || !first_arr) {
			/* The same movie again: it would be made once. */
			item->skip = AXIS_ARR_SKIP_TWICE;
		} else if (work->sonarr && !preview->series_ready) {
			/* A series would follow TheTVDB's aired order, which Axis cannot
			 * fetch now: none is brought, whatever else is so of it. */
			item->skip = AXIS_ARR_SKIP_NO_TVDB_KEY;
		} else if (work->sonarr && item->unknown_kind) {
			/* (A series Sonarr has as anime is decided as any other.) */
			item->skip = AXIS_ARR_SKIP_UNKNOWN_KIND;
		} else if (g_strcmp0(item->match, "no_id") == 0) {
			item->skip = AXIS_ARR_SKIP_NO_ID;
		} else if (g_strcmp0(item->match, "unknown") == 0) {
			item->skip = AXIS_ARR_SKIP_UNKNOWN;
		} else if (g_strcmp0(item->match, "not_listed") == 0) {
			item->skip = AXIS_ARR_SKIP_NOT_LISTED;
		} else if (g_strcmp0(item->match, "matched") != 0 || item->title_id <= 0 ||
		           (work->sonarr && item->n_episodes < 0)) {
			/* (A series is matched with its lists read, or not at all.) */
			item->skip = AXIS_ARR_SKIP_NOT_LOOKED_UP;
		} else if (item->managed) {
			item->skip = AXIS_ARR_SKIP_ALREADY;
		} else if (item->brought_before) {
			/* This connection brought it, and a person removed it in Axis
			 * since: bringing it again would undo that. */
			item->skip = AXIS_ARR_SKIP_REMOVED;
		} else if (!item->monitored && !preview->unmonitored) {
			item->skip = AXIS_ARR_SKIP_UNMONITORED;
		} else {
			AxisArrProfileRow *profile = profile_row_of(work, item->profile_id);
			const AxisArrFolderRow *row = item->mapped_row >= 0 ?
				g_ptr_array_index(preview->folder_rows, (guint)item->mapped_row) : NULL;
			const AxisArrRootChoice *root = row != NULL ? find_root(preview, row->root_id) : NULL;

			if (profile != NULL)
				profile->n_titles++;
			item->to_profile = profile != NULL ? profile->chosen : 0;
			item->to_root = root != NULL ? root->id : 0;
			if (profile == NULL || profile->chosen <= 0)
				item->skip = AXIS_ARR_SKIP_PROFILE;
			else if (root == NULL)
				item->skip = AXIS_ARR_SKIP_FOLDER;
			else if (!root->online)
				item->skip = AXIS_ARR_SKIP_OFFLINE;
			else
				item->skip = AXIS_ARR_SKIP_NONE;
		}
		if (item->skip == AXIS_ARR_SKIP_NONE && item->n_episodes >= 0) {
			preview->bring_episodes += (guint)item->n_episodes;
			preview->bring_episodes_monitored += item->n_episodes_monitored;
		}
		/* The monitoring that comes with it: a title the app does not monitor
		 * comes unmonitored, a series so with no season and no episode
		 * monitored. */
		if (item->skip == AXIS_ARR_SKIP_NONE && item->monitored) {
			preview->bring_monitored++;
			preview->bring_seasons_on += item->n_seasons_on;
			preview->bring_episodes_on += item->n_episodes_monitored;
		}
		g_clear_pointer(&item->path, g_free);
		g_clear_pointer(&item->file_path, g_free);
	}
}

/* Items read @LOAD_BATCH a statement, and decided as they come: the whole
 * plan is never in memory with its titles and paths. */
#define LOAD_BATCH 2000

/* The two apps' items give the same columns: a movie's file is on its item
 * and a series has none there (its files are rows of their own); a series
 * gives its type and what its two lists held. */
#define ITEMS_TAIL \
	" FROM arr_plan_item i WHERE i.plan_id = $1::bigint AND i.seq > $2::integer ORDER BY i.seq" \
	" LIMIT " G_STRINGIFY(LOAD_BATCH)

static const gchar *const movie_items_sql =
	"SELECT i.seq, i.year, i.monitored, i.profile_id, i.path, i.has_file, i.file_path, i.file_size,"
	" i.match, i.title_id, i.in_queue, i.arr_id, i.tmdb_id,"
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM movie m WHERE m.title_id = i.title_id)),"
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM arr_brought b WHERE b.source_id = $3::bigint"
	"   AND b.title_id = i.title_id)),"
	/* The movie it brought, and only under the number it was brought for: a movie
	 * removed and added again by a person, or a second entry of Radarr for the
	 * same title, is not this connection's to link. */
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM arr_brought b JOIN movie m ON m.id = b.movie_id"
	"   WHERE b.source_id = $3::bigint AND b.title_id = i.title_id AND m.title_id = i.title_id"
	"   AND b.arr_id = i.arr_id)),"
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM movie m WHERE m.title_id = i.title_id"
	"   AND m.search_hold = 'file_unseen')),"
	" false, -1, 0, 0, 0, 0, false, 0"
	ITEMS_TAIL;

static const gchar *const series_items_sql =
	"SELECT i.seq, i.year, i.monitored, i.profile_id, i.path, false, NULL::text, NULL::bigint,"
	" i.match, i.title_id, i.in_queue, i.arr_id, i.tvdb_id,"
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM series s WHERE s.title_id = i.title_id)),"
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM arr_brought b WHERE b.source_id = $3::bigint"
	"   AND b.title_id = i.title_id)),"
	/* The series it brought, and only under the number it was brought for (as a
	 * movie above): its files that Axis did not link then are this
	 * connection's to link.  (An episode's hold is the episode's, not asked
	 * about here.) */
	" (i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM arr_brought b JOIN series s ON s.id = b.series_id"
	"   WHERE b.source_id = $3::bigint AND b.title_id = i.title_id AND s.title_id = i.title_id"
	"   AND b.arr_id = i.arr_id)),"
	" false,"
	" coalesce(i.series_type = 'anime', false), coalesce(i.n_episodes, -1), coalesce(i.n_episodes_monitored, 0),"
	" coalesce(i.n_episodes_with_file, 0), coalesce(i.n_files, 0), coalesce(i.n_odd, 0),"
	" (i.series_type IS NULL),"
	/* The seasons Sonarr monitors ([{"n": 1, "m": true}]). */
	" (SELECT count(*) FROM jsonb_array_elements(coalesce(i.seasons, '[]'::jsonb)) e WHERE e -> 'm' = 'true'::jsonb)"
	ITEMS_TAIL;

static gboolean
load_and_decide(
	Work    *work,
	GError **error
){
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	g_autofree gchar *source_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->source->id);
	guint last = 0;
	gboolean more = TRUE;

	work->items = g_ptr_array_new_with_free_func(item_free);
	work->seen_tmdb = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	work->seen_arr = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	while (more) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *after = g_strdup_printf("%u", last);
		const gchar *params[3];
		guint from = work->items->len;
		guint i;

		params[0] = plan_text;
		params[1] = after;
		params[2] = source_text;
		if (!axis_db_conn_exec(work->conn, work->sonarr ? series_items_sql : movie_items_sql, 3, params, &result, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			Item *item = g_new0(Item, 1);

			item->seq = (guint)axis_arr_int_at(result, i, 0, 0);
			item->year = (gint)axis_arr_int_at(result, i, 1, 0);
			item->monitored = axis_arr_bool_at(result, i, 2);
			item->profile_id = axis_arr_int_at(result, i, 3, 0);
			axis_storage_path_normalize_exact(axis_db_result_get_text(result, i, 4), TRUE, &item->path, NULL);
			item->has_file = axis_arr_bool_at(result, i, 5);
			item->file_path = axis_arr_text_at(result, i, 6);
			item->file_size = axis_arr_int_at(result, i, 7, -1);
			item->match = axis_arr_text_at(result, i, 8);
			item->title_id = axis_arr_int_at(result, i, 9, 0);
			item->arr_id = axis_arr_int_at(result, i, 11, 0);
			item->ext_id = axis_arr_int_at(result, i, 12, 0);
			item->managed = axis_arr_bool_at(result, i, 13);
			item->brought_before = axis_arr_bool_at(result, i, 14);
			item->brought_here = axis_arr_bool_at(result, i, 15);
			item->hold_unseen = axis_arr_bool_at(result, i, 16);
			item->anime = axis_arr_bool_at(result, i, 17);
			item->n_episodes = (gint)axis_arr_int_at(result, i, 18, -1);
			item->n_episodes_monitored = (guint)axis_arr_int_at(result, i, 19, 0);
			item->n_episodes_with_file = (guint)axis_arr_int_at(result, i, 20, 0);
			item->n_files = (guint)axis_arr_int_at(result, i, 21, 0);
			item->n_odd = (guint)axis_arr_int_at(result, i, 22, 0);
			item->unknown_kind = axis_arr_bool_at(result, i, 23);
			item->n_seasons_on = (guint)axis_arr_int_at(result, i, 24, 0);
			item->shown_row = -1;
			item->mapped_row = -1;
			item->file_row = -1;
			last = item->seq;
			g_ptr_array_add(work->items, item);
			if (axis_arr_bool_at(result, i, 10))
				work->preview->n_in_queue++;
		}
		more = axis_db_result_get_n_rows(result) == LOAD_BATCH;
		decide(work, from);
	}
	g_clear_pointer(&work->seen_tmdb, g_hash_table_unref);
	g_clear_pointer(&work->seen_arr, g_hash_table_unref);
	return TRUE;
}

/* The app's files in Axis's index, by library folder and stored path: a
 * file is there at its path or it is not. */
static gboolean
look_up_files(
	Work    *work,
	GError **error
){
	AxisArrPreview *preview = work->preview;
	guint next = 0;

	while (next < work->items->len) {
		g_autoptr(JsonArray) rows = json_array_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *text = NULL;
		const gchar *params[1];
		guint i;

		for (; next < work->items->len && json_array_get_length(rows) < LOOKUP_BATCH; next++) {
			const Item *item = g_ptr_array_index(work->items, next);
			JsonObject *row;

			if (item->file_stored == NULL)
				continue;
			row = json_object_new();
			json_object_set_int_member(row, "ord", next);
			json_object_set_int_member(row, "root", item->file_root);
			json_object_set_string_member(row, "p", item->file_stored);
			json_array_add_object_element(rows, row);
		}
		if (json_array_get_length(rows) == 0)
			break;
		json_node_set_array(node, rows);
		json_generator_set_root(generator, node);
		text = json_generator_to_data(generator, NULL);
		params[0] = text;
		if (!axis_db_conn_exec(work->conn,
		        "SELECT q.ord, f.size, (f.state = 'present'), coalesce(m.title_id, 0), t.title, t.year, f.id,"
		        " mf.file_id IS NOT NULL, EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = f.id),"
		        /* Identification's own link: no person's, no import's, not kept. */
		        " coalesce(mf.hand_at IS NULL AND mf.import_id IS NULL AND NOT mf.kept, false)"
		        " FROM jsonb_to_recordset($1::jsonb) AS q(ord integer, root bigint, p text)"
		        " JOIN library_file f ON f.root_id = q.root AND f.path_hash = library_path_hash(q.p) AND f.rel_path = q.p"
		        " LEFT JOIN movie_file mf ON mf.file_id = f.id"
		        " LEFT JOIN movie m ON m.id = mf.movie_id"
		        " LEFT JOIN meta_title t ON t.id = m.title_id", 1, params, &result, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			gint64 ord = axis_arr_int_at(result, i, 0, -1);
			gint64 linked = axis_arr_int_at(result, i, 3, 0);
			Item *item;

			if (ord < 0 || ord >= work->items->len)
				continue;
			item = g_ptr_array_index(work->items, (guint)ord);
			if (!axis_arr_bool_at(result, i, 2))
				continue;
			if (axis_arr_int_at(result, i, 1, -1) != item->file_size) {
				item->file = AXIS_ARR_FILE_OTHER_SIZE;
				continue;
			}
			item->file_id = axis_arr_int_at(result, i, 6, 0);
			item->file_linked = axis_arr_bool_at(result, i, 7);
			item->file_unlinked = axis_arr_bool_at(result, i, 8);
			item->file_identified = axis_arr_bool_at(result, i, 9);
			/* At its path, of its size: the folder's mapping holds. */
			if (item->file_row >= 0)
				((AxisArrFolderRow *)g_ptr_array_index(preview->folder_rows, (guint)item->file_row))->n_found++;
			if (linked > 0 && linked != item->title_id) {
				gint64 year = axis_arr_int_at(result, i, 5, 0);

				item->file = AXIS_ARR_FILE_LINKED_ELSEWHERE;
				g_free(item->other);
				item->other = year > 0 ? g_strdup_printf("%s (%" G_GINT64_FORMAT ")",
					axis_db_result_get_text(result, i, 4), year) : axis_arr_text_at(result, i, 4);
			} else {
				item->file = AXIS_ARR_FILE_FOUND;
			}
		}
	}
	return TRUE;
}

/* ── A series' episode files ────────────────────────────────────── */

/* The item of the plan's @seq (the items are in that order). */
static Item *
item_of_seq(
	Work  *work,
	guint  seq
){
	guint low = 0;
	guint high = work->items->len;

	while (low < high) {
		guint middle = low + (high - low) / 2;
		Item *item = g_ptr_array_index(work->items, middle);

		if (item->seq == seq)
			return item;
		if (item->seq < seq)
			low = middle + 1;
		else
			high = middle;
	}
	return NULL;
}

/* One episode file while its batch is looked up. */
typedef struct
{
	Item             *item;
	gint64            arr_id;	/* Sonarr's number of the file */
	gint64            file_id;	/* its row of the index, when it is found */
	gint64            other_size_id;	/* its row of the index, when that is of another size */
	gboolean          linked;	/* that row is linked to a series */
	gboolean          by_name;	/* that link is identification's own: no person's, no import's, not kept */
	gboolean          unlinked;	/* a person unlinked it by hand */
	gchar            *arr_path;	/* as Sonarr gives it; nullable */
	gint64            size;
	AxisArrFileState  state;
	gint              row;		/* the folder row it is under, -1 for none */
	gint64            root;
	gchar            *stored;
	gchar            *other;
} SeriesFile;

static void
series_file_free(gpointer data)
{
	SeriesFile *file = data;

	g_free(file->arr_path);
	g_free(file->stored);
	g_free(file->other);
	g_free(file);
}

/* The files of one batch that map, in Axis's index: by library folder and
 * stored path, as a movie's file is, and nowhere else. */
static gboolean
look_up_batch(
	Work       *work,
	GPtrArray  *batch,
	GError    **error
){
	g_autoptr(JsonArray) rows = json_array_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[1];
	guint i;

	for (i = 0; i < batch->len; i++) {
		const SeriesFile *file = g_ptr_array_index(batch, i);
		JsonObject *row;

		if (file->stored == NULL)
			continue;
		row = json_object_new();
		json_object_set_int_member(row, "ord", i);
		json_object_set_int_member(row, "root", file->root);
		json_object_set_string_member(row, "p", file->stored);
		json_array_add_object_element(rows, row);
	}
	if (json_array_get_length(rows) == 0)
		return TRUE;
	json_node_set_array(node, rows);
	json_generator_set_root(generator, node);
	text = json_generator_to_data(generator, NULL);
	params[0] = text;
	if (!axis_db_conn_exec(work->conn,
	        "SELECT q.ord, f.size, (f.state = 'present'), coalesce(s.title_id, 0), t.title, t.year, f.id,"
	        " sf.file_id IS NOT NULL, EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = f.id),"
	        " coalesce(sf.hand_at IS NULL AND sf.import_id IS NULL AND NOT sf.kept, false)"
	        " FROM jsonb_to_recordset($1::jsonb) AS q(ord integer, root bigint, p text)"
	        " JOIN library_file f ON f.root_id = q.root AND f.path_hash = library_path_hash(q.p) AND f.rel_path = q.p"
	        " LEFT JOIN series_file sf ON sf.file_id = f.id"
	        " LEFT JOIN series s ON s.id = sf.series_id"
	        " LEFT JOIN meta_title t ON t.id = s.title_id", 1, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 ord = axis_arr_int_at(result, i, 0, -1);
		gint64 linked = axis_arr_int_at(result, i, 3, 0);
		SeriesFile *file;

		if (ord < 0 || ord >= batch->len)
			continue;
		file = g_ptr_array_index(batch, (guint)ord);
		if (!axis_arr_bool_at(result, i, 2))
			continue;
		if (axis_arr_int_at(result, i, 1, -1) != file->size) {
			file->state = AXIS_ARR_FILE_OTHER_SIZE;
			/* Never linked by a run; kept so that the run can say when
			 * identification holds it on other episodes than Sonarr's. */
			file->other_size_id = axis_arr_int_at(result, i, 6, 0);
			continue;
		}
		file->file_id = axis_arr_int_at(result, i, 6, 0);
		file->linked = axis_arr_bool_at(result, i, 7);
		file->unlinked = axis_arr_bool_at(result, i, 8);
		file->by_name = axis_arr_bool_at(result, i, 9);
		/* At its path, of its size: the folder's mapping holds. */
		if (file->row >= 0)
			((AxisArrFolderRow *)g_ptr_array_index(work->preview->folder_rows, (guint)file->row))->n_found++;
		if (linked > 0 && linked != file->item->title_id) {
			gint64 year = axis_arr_int_at(result, i, 5, 0);

			file->state = AXIS_ARR_FILE_LINKED_ELSEWHERE;
			g_free(file->other);
			file->other = year > 0 ? g_strdup_printf("%s (%" G_GINT64_FORMAT ")", axis_db_result_get_text(result, i, 4),
				year) : axis_arr_text_at(result, i, 4);
		} else {
			file->state = AXIS_ARR_FILE_FOUND;
		}
	}
	return TRUE;
}

static void
decide_file(
	Item             *item,
	const SeriesFile *file
){
	AxisArrFileDecision decision;

	if (item->file_decisions == NULL)
		item->file_decisions = g_array_new(FALSE, FALSE, sizeof(AxisArrFileDecision));
	decision.arr_id = file->arr_id;
	decision.state = file->state;
	decision.file_id = file->state == AXIS_ARR_FILE_FOUND ? file->file_id : 0;
	decision.other_size_id = file->state == AXIS_ARR_FILE_OTHER_SIZE ? file->other_size_id : 0;
	g_array_append_val(item->file_decisions, decision);
}

/*
 * The files of one batch that a later run would link: of a series this
 * connection brought before and that is still in Axis, a file the index holds
 * now at its path and of its size, linked to nothing, not unlinked by a
 * person, and on episodes that all match episodes of that series (the TV
 * module's one rule, AXIS_TV_BROUGHT_EPISODE_MATCH: a file on an episode
 * Axis's numbering does not have is never linked, so it is not offered again
 * and again).  One statement a batch.
 */
static gboolean
relink_batch(
	Work       *work,
	GPtrArray  *batch,
	GError    **error
){
	g_autoptr(JsonArray) rows = json_array_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	g_autofree gchar *source_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->source->id);
	g_autofree gchar *text = NULL;
	const gchar *params[3];
	guint i;

	for (i = 0; i < batch->len; i++) {
		const SeriesFile *file = g_ptr_array_index(batch, i);
		JsonObject *row;

		/* Linked to nothing; or linked to this very series (FOUND: not to
		 * another) by identification, by its name: a run moves such a link
		 * to Sonarr's episodes or stamps it where it is (link_file() in the
		 * TV module).  A link a person or an import made is never offered. */
		if (file->item->skip != AXIS_ARR_SKIP_ALREADY || !file->item->brought_here ||
		    file->state != AXIS_ARR_FILE_FOUND || file->file_id <= 0 || (file->linked && !file->by_name) ||
		    file->unlinked)
			continue;
		row = json_object_new();
		json_object_set_int_member(row, "ord", i);
		json_object_set_int_member(row, "seq", file->item->seq);
		json_object_set_int_member(row, "f", file->arr_id);
		json_object_set_int_member(row, "t", file->item->title_id);
		json_object_set_int_member(row, "id", file->file_id);
		json_object_set_boolean_member(row, "n", file->linked);
		json_array_add_object_element(rows, row);
	}
	if (json_array_get_length(rows) == 0)
		return TRUE;
	json_node_set_array(node, rows);
	json_generator_set_root(generator, node);
	text = json_generator_to_data(generator, NULL);
	params[0] = plan_text;
	params[1] = text;
	params[2] = source_text;
	if (!axis_db_conn_exec(work->conn,
	        /* The second column: the series still follows the numbering Sonarr's
	         * places are in.  One a person switched to another numbering is
	         * linked nothing (the same season and number may be another episode
	         * there): its files are counted, to be said, and not offered.  The
	         * last condition is match_episodes()'s in the TV module: a place
	         * Sonarr lists twice matches nothing, so the preview and the run
	         * agree. */
	        "SELECT q.ord, s.episode_source = '" AXIS_ARR_SERIES_SOURCE "'"
	        " FROM jsonb_to_recordset($2::jsonb) AS q(ord integer, seq integer, f bigint, t bigint, id bigint, n boolean)"
	        " JOIN arr_brought b ON b.source_id = $3::bigint AND b.title_id = q.t"
	        " JOIN series s ON s.id = b.series_id"
	        " WHERE EXISTS (SELECT 1 FROM arr_plan_episode pe WHERE pe.plan_id = $1::bigint AND pe.seq = q.seq"
	        "   AND pe.file_arr_id = q.f)"
	        /* A file identification linked by its name (q.n): only of a series
	         * that still follows Sonarr's numbering.  On other episodes than
	         * the ones Sonarr has it on (all of which match, by the condition
	         * below) a run moves its link; on the very same ones a run stamps
	         * it as brought over, and so kept.  Either way it is offered. */
	        " AND (NOT q.n OR s.episode_source = '" AXIS_ARR_SERIES_SOURCE "')"
	        " AND (s.episode_source <> '" AXIS_ARR_SERIES_SOURCE "'"
	        "  OR NOT EXISTS (SELECT 1 FROM arr_plan_episode pe WHERE pe.plan_id = $1::bigint AND pe.seq = q.seq"
	        "   AND pe.file_arr_id = q.f AND (NOT EXISTS (SELECT 1 FROM series_episode e WHERE "
	        AXIS_TV_BROUGHT_EPISODE_MATCH("e", "s", "pe.season", "pe.episode", "pe.tvdb_id") ")"
	        "   OR (SELECT count(*) FROM arr_plan_episode twice WHERE twice.plan_id = pe.plan_id AND twice.seq = pe.seq"
	        "    AND twice.season = pe.season AND twice.episode = pe.episode) <> 1)))", 3, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 ord = axis_arr_int_at(result, i, 0, -1);
		SeriesFile *file;

		if (ord < 0 || ord >= batch->len)
			continue;
		if (!axis_arr_bool_at(result, i, 1)) {
			work->preview->n_relink_renumbered++;
			continue;
		}
		file = g_ptr_array_index(batch, (guint)ord);
		file->item->n_relink++;
		work->preview->n_relink++;
		if (work->decisions != NULL)
			decide_file(file->item, file);
	}
	return TRUE;
}

/*
 * Every episode file the reading stored, @LOOKUP_BATCH at a time, in the
 * plan's order: mapped through the folder table, looked up in the index,
 * counted under its folder row and its series, and (of a series that would
 * be brought) listed when it would not be linked.  The files are never all
 * in memory: a batch is counted and let go.
 */
static gboolean
look_up_series_files(
	Work    *work,
	GError **error
){
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	AxisArrPreview *preview = work->preview;
	guint last_seq = 0;
	gint64 last_ord = -1;
	gboolean more = TRUE;

	while (more) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autoptr(GPtrArray) batch = g_ptr_array_new_with_free_func(series_file_free);
		g_autofree gchar *seq_text = g_strdup_printf("%u", last_seq);
		g_autofree gchar *ord_text = g_strdup_printf("%" G_GINT64_FORMAT, last_ord);
		const gchar *params[3];
		guint i;

		params[0] = plan_text;
		params[1] = seq_text;
		params[2] = ord_text;
		if (!axis_db_conn_exec(work->conn,
		        "SELECT seq, ord, path, size, arr_id FROM arr_plan_file WHERE plan_id = $1::bigint"
		        " AND (seq, ord) > ($2::integer, $3::integer) ORDER BY seq, ord LIMIT " G_STRINGIFY(LOOKUP_BATCH), 3,
		        params, &result, error))
			return FALSE;
		more = axis_db_result_get_n_rows(result) == LOOKUP_BATCH;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			g_autofree gchar *normal = NULL;
			SeriesFile *file;
			Item *item;

			last_seq = (guint)axis_arr_int_at(result, i, 0, 0);
			last_ord = axis_arr_int_at(result, i, 1, 0);
			item = item_of_seq(work, last_seq);
			if (item == NULL)
				continue;
			file = g_new0(SeriesFile, 1);
			file->item = item;
			file->arr_path = axis_arr_text_at(result, i, 2);
			file->size = axis_arr_int_at(result, i, 3, -1);
			file->arr_id = axis_arr_int_at(result, i, 4, 0);
			file->row = -1;
			if (file->arr_path == NULL || file->size < 0) {
				file->state = AXIS_ARR_FILE_NO_PATH;
				file->row = item->shown_row;
			} else if (axis_arr_map_path(work->maps, file->arr_path, &file->root, &file->stored) ==
			           AXIS_ARR_PATH_MAPPED) {
				axis_storage_path_normalize_exact(file->arr_path, TRUE, &normal, NULL);
				file->row = row_of(preview, normal, TRUE);
				/* Until the index says otherwise. */
				file->state = AXIS_ARR_FILE_NOT_INDEXED;
			} else {
				axis_storage_path_normalize_exact(file->arr_path, TRUE, &normal, NULL);
				file->row = row_of(preview, normal, FALSE);
				file->state = AXIS_ARR_FILE_UNMAPPABLE;
			}
			if (file->row >= 0)
				((AxisArrFolderRow *)g_ptr_array_index(preview->folder_rows, (guint)file->row))->n_files++;
			g_ptr_array_add(batch, file);
		}
		if (!look_up_batch(work, batch, error) || !relink_batch(work, batch, error))
			return FALSE;
		for (i = 0; i < batch->len; i++) {
			SeriesFile *file = g_ptr_array_index(batch, i);

			/* Only the files of a series that would be brought are a run's
			 * (and those a later run links, counted above). */
			if (file->item->skip != AXIS_ARR_SKIP_NONE)
				continue;
			if (work->decisions != NULL)
				decide_file(file->item, file);
			preview->files[file->state]++;
			preview->bring_files++;
			if (file->state == AXIS_ARR_FILE_FOUND) {
				file->item->n_found++;
				continue;
			}
			if (preview->n_unusable / AXIS_ARR_PAGE_ROWS == work->pages.file_page) {
				AxisArrFileRow *row = g_new0(AxisArrFileRow, 1);
				const AxisArrRootChoice *root = find_root(preview, file->root);

				g_array_append_val(work->file_seqs, file->item->seq);
				row->year = file->item->year;
				row->state = file->state;
				row->arr_path = g_steal_pointer(&file->arr_path);
				if (root != NULL && file->stored != NULL)
					row->mapped = g_strdup_printf("%s: %s", root->name, file->stored);
				row->other = g_steal_pointer(&file->other);
				g_ptr_array_add(preview->unusable, row);
			}
			preview->n_unusable++;
		}
	}
	return TRUE;
}

/* "1, 2, 4" and "specials, 3" of a series' seasons as the reading stored
 * them ([{"n": 1, "m": true}]): those Sonarr monitors, and those it does not. */
static void
seasons_text(
	const gchar  *json,
	gchar       **on,
	gchar       **off
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GString) monitored = g_string_new(NULL);
	g_autoptr(GString) not = g_string_new(NULL);
	JsonArray *array;
	guint i;

	*on = NULL;
	*off = NULL;
	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL) || json_parser_get_root(parser) == NULL ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
		return;
	array = json_node_get_array(json_parser_get_root(parser));
	for (i = 0; i < json_array_get_length(array) && i < AXIS_ARR_MAX_SEASONS; i++) {
		JsonNode *entry = json_array_get_element(array, i);
		JsonNode *flag;
		GString *to;
		gint64 number = 0;

		if (entry == NULL || !JSON_NODE_HOLDS_OBJECT(entry) ||
		    !node_int(json_object_get_member(json_node_get_object(entry), "n"), &number))
			continue;
		flag = json_object_get_member(json_node_get_object(entry), "m");
		to = flag != NULL && JSON_NODE_HOLDS_VALUE(flag) && json_node_get_value_type(flag) == G_TYPE_BOOLEAN &&
			json_node_get_boolean(flag) ? monitored : not;
		if (to->len > 0)
			g_string_append(to, ", ");
		if (number == 0)
			g_string_append(to, "specials");
		else
			g_string_append_printf(to, "%" G_GINT64_FORMAT, number);
	}
	if (monitored->len > 0)
		*on = g_strdup(monitored->str);
	if (not->len > 0)
		*off = g_strdup(not->str);
}

/*
 * For the series of the page shown that Sonarr has as anime: Sonarr's own
 * absolute number of each episode beside the one TheTVDB gives the episode
 * at the same place, as the metadata store holds TheTVDB's aired order (the
 * rows the series' episodes are made from when it is brought, so this is what
 * the run's own comparison will find, short of a refresh between the two).
 * The match is the run's (AXIS_TV_BROUGHT_EPISODE_MATCH, on the store's
 * rows): the place, with TheTVDB's episode id as the check; a place Sonarr
 * lists twice and an entry that could not be read are no comparison; seasons
 * 1 and up only.
 *
 * A REPORT ONLY.  It is computed after every count, decision and digest of
 * the preview, for the rows shown, and nothing reads it but the page.
 */
static gboolean
compare_absolute(
	Work    *work,
	GError **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) list = g_string_new("{");
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	AxisArrPreview *preview = work->preview;
	AxisTvAbsoluteCompare compare;
	const gchar *params[2];
	guint at = 0;
	guint i;

	for (i = 0; preview->bringing != NULL && i < preview->bringing->len; i++) {
		const AxisArrBringRow *row = g_ptr_array_index(preview->bringing, i);

		if (row->anime)
			g_string_append_printf(list, "%s%u", list->len > 1 ? "," : "", row->seq);
	}
	if (list->len == 1)
		return TRUE;
	g_string_append_c(list, '}');
	params[0] = plan_text;
	params[1] = list->str;
	if (!axis_db_conn_exec(work->conn,
	        "SELECT p.seq, p.season, p.episode, p.a, coalesce(me.absolute_number, 0)"
	        " FROM (SELECT pe.seq, pe.season, pe.episode, pe.tvdb_id, pe.odd, coalesce(pe.absolute_number, 0) AS a,"
	        "   count(*) OVER (PARTITION BY pe.seq, pe.season, pe.episode) AS n FROM arr_plan_episode pe"
	        "   WHERE pe.plan_id = $1::bigint AND pe.seq = ANY($2::integer[])) p"
	        " JOIN arr_plan_item i ON i.plan_id = $1::bigint AND i.seq = p.seq"
	        " JOIN meta_episode me ON me.title_id = i.title_id AND me.source = '" AXIS_ARR_SERIES_SOURCE "'"
	        "   AND me.season_number = p.season AND me.number = p.episode"
	        "   AND (p.tvdb_id IS NULL OR p.tvdb_id <= 0 OR me.provider_id IS NULL OR me.provider_id = p.tvdb_id::text)"
	        " WHERE p.n = 1 AND NOT p.odd AND p.season > 0 ORDER BY p.seq, p.season, p.episode", 2, params, &result,
	        error))
		return FALSE;
	for (i = 0; i < preview->bringing->len; i++) {
		AxisArrBringRow *row = g_ptr_array_index(preview->bringing, i);

		if (!row->anime)
			continue;
		axis_tv_absolute_compare_init(&compare);
		/* (The rows come in the order of their series, as the page's do.) */
		while (at < axis_db_result_get_n_rows(result) && (guint)axis_arr_int_at(result, at, 0, 0) < row->seq)
			at++;
		for (; at < axis_db_result_get_n_rows(result) && (guint)axis_arr_int_at(result, at, 0, 0) == row->seq; at++)
			axis_tv_absolute_compare_add(&compare, (gint)axis_arr_int_at(result, at, 1, 0),
				(gint)axis_arr_int_at(result, at, 2, 0), (gint)axis_arr_int_at(result, at, 3, 0),
				(gint)axis_arr_int_at(result, at, 4, 0));
		row->absolute_differ = compare.differ;
		row->absolute = axis_tv_absolute_compare_sentence(&compare);
		row->absolute_list = axis_tv_absolute_compare_list(&compare);
		axis_tv_absolute_compare_clear(&compare);
	}
	return TRUE;
}

/* The titles of the rows of the pages shown, the app's path of a movie's
 * file that cannot be used, and a listed series' seasons: the only ones
 * read. */
static gboolean
fill_pages(
	Work    *work,
	GError **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GHashTable) at = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GString) list = g_string_new("{");
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, work->preview->plan->id);
	AxisArrPreview *preview = work->preview;
	GArray *skip_seqs = work->skip_seqs;
	GArray *file_seqs = work->file_seqs;
	GArray *bring_seqs = work->bring_seqs;
	g_autoptr(GError) failure = NULL;
	const gchar *params[2];
	guint i;

	if (skip_seqs->len + file_seqs->len + bring_seqs->len == 0)
		return TRUE;
	for (i = 0; i < skip_seqs->len; i++)
		g_string_append_printf(list, "%s%u", list->len > 1 ? "," : "", g_array_index(skip_seqs, guint, i));
	for (i = 0; i < file_seqs->len; i++)
		g_string_append_printf(list, "%s%u", list->len > 1 ? "," : "", g_array_index(file_seqs, guint, i));
	for (i = 0; i < bring_seqs->len; i++)
		g_string_append_printf(list, "%s%u", list->len > 1 ? "," : "", g_array_index(bring_seqs, guint, i));
	g_string_append_c(list, '}');
	params[0] = plan_text;
	params[1] = list->str;
	if (!axis_db_conn_exec(work->conn,
	        "SELECT seq, title, file_path, seasons::text FROM arr_plan_item WHERE plan_id = $1::bigint"
	        " AND seq = ANY($2::integer[])", 2, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_hash_table_insert(at, GUINT_TO_POINTER((guint)axis_arr_int_at(result, i, 0, 0)), GUINT_TO_POINTER(i + 1));
	for (i = 0; i < bring_seqs->len; i++) {
		AxisArrBringRow *row = g_ptr_array_index(preview->bringing, i);
		guint found = GPOINTER_TO_UINT(g_hash_table_lookup(at, GUINT_TO_POINTER(g_array_index(bring_seqs, guint, i))));

		if (found > 0) {
			row->title = axis_arr_text_at(result, found - 1, 1);
			seasons_text(axis_db_result_get_text(result, found - 1, 3), &row->seasons_on, &row->seasons_off);
		}
	}
	for (i = 0; i < skip_seqs->len; i++) {
		AxisArrSkipRow *row = g_ptr_array_index(preview->skipped, i);
		guint found = GPOINTER_TO_UINT(g_hash_table_lookup(at, GUINT_TO_POINTER(g_array_index(skip_seqs, guint, i))));

		if (found > 0)
			row->title = axis_arr_text_at(result, found - 1, 1);
	}
	for (i = 0; i < file_seqs->len; i++) {
		AxisArrFileRow *row = g_ptr_array_index(preview->unusable, i);
		guint found = GPOINTER_TO_UINT(g_hash_table_lookup(at, GUINT_TO_POINTER(g_array_index(file_seqs, guint, i))));

		if (found > 0) {
			row->title = axis_arr_text_at(result, found - 1, 1);
			/* A movie's file is on its item; an episode file's path came
			 * with the file. */
			if (row->arr_path == NULL)
				row->arr_path = axis_arr_text_at(result, found - 1, 2);
		}
	}
	/* Last, and read by nothing but the page: the anime series' numbers.  A
	 * report cannot fail the preview it is in: a comparison that fails is
	 * logged and shown as one that could not be made.  (The preview runs no
	 * transaction of its own, so a failed statement poisons nothing.) */
	if (!compare_absolute(work, &failure)) {
		g_message("arr: the absolute numbers of the anime series could not be compared: %s",
			failure != NULL ? failure->message : "unknown error");
		for (i = 0; preview->bringing != NULL && i < preview->bringing->len; i++) {
			AxisArrBringRow *row = g_ptr_array_index(preview->bringing, i);

			if (!row->anime)
				continue;
			g_clear_pointer(&row->absolute, g_free);
			g_clear_pointer(&row->absolute_list, g_free);
			row->absolute_differ = 0;
			row->absolute = g_strdup("Absolute numbers: could not be compared. Nothing is migrated differently "
				"because of it.");
		}
	}
	return TRUE;
}

/* The counts and the two lists' pages. */
static gboolean
tally(
	Work         *work,
	GError      **error
){
	GArray *skip_seqs = work->skip_seqs;
	GArray *file_seqs = work->file_seqs;
	AxisArrPreview *preview = work->preview;
	AxisArrSkip filter = work->pages.skip_filter;
	guint skip_page = work->pages.skip_page;
	guint file_page = work->pages.file_page;
	guint n_bring = 0;
	guint i;

	for (i = 0; i < work->items->len; i++) {
		Item *item = g_ptr_array_index(work->items, i);
		/* A movie this connection brought before, still in Axis, whose file
		 * was not seen then and is in the index now, linked to nothing and
		 * not unlinked by a person: a run links it.  So does it a file that
		 * identification linked to that movie since (FOUND: to no other), which
		 * is the app's file all the same: the run stamps it as brought over,
		 * and so kept (link_file() in the movies module).  A series so
		 * brought, with episode files a run would link now (counted with its
		 * files, relink_batch()). */
		gboolean relink = work->sonarr ? item->n_relink > 0 :
			item->skip == AXIS_ARR_SKIP_ALREADY && item->brought_here &&
			item->file == AXIS_ARR_FILE_FOUND && item->file_id > 0 && !item->file_unlinked &&
			(!item->file_linked || item->hold_unseen || item->file_identified);

		preview->counts[item->skip]++;
		if (relink && !work->sonarr)
			preview->n_relink++;
		/* What a run would do: a Radarr's decisions are of movies, a Sonarr's
		 * of series, each with its episode files. */
		if (work->decisions != NULL && (relink || item->skip == AXIS_ARR_SKIP_NONE)) {
			AxisArrDecision *decision = g_new0(AxisArrDecision, 1);

			decision->seq = item->seq;
			decision->arr_id = item->arr_id;
			decision->title_id = item->title_id;
			decision->relink = relink;
			decision->profile_id = item->to_profile;
			decision->root_id = item->to_root;
			decision->has_file = work->sonarr ? item->n_files > 0 : item->has_file;
			decision->file = item->file;
			decision->file_id = item->file == AXIS_ARR_FILE_FOUND ? item->file_id : 0;
			decision->other = g_strdup(item->other);
			decision->files = g_steal_pointer(&item->file_decisions);
			g_ptr_array_add(work->decisions, decision);
		}
		if (item->skip != AXIS_ARR_SKIP_NONE) {
			if (filter != AXIS_ARR_SKIP_NONE && item->skip != filter)
				continue;
			if (preview->n_skipped_listed / AXIS_ARR_PAGE_ROWS == skip_page) {
				AxisArrSkipRow *row = g_new0(AxisArrSkipRow, 1);

				g_array_append_val(skip_seqs, item->seq);
				row->year = item->year;
				row->seq = item->seq;
				row->skip = item->skip;
				row->title_id = item->skip == AXIS_ARR_SKIP_ALREADY ? item->title_id : 0;
				if (item->skip == AXIS_ARR_SKIP_OFFLINE && item->mapped_row >= 0) {
					const AxisArrFolderRow *folder = g_ptr_array_index(preview->folder_rows, (guint)item->mapped_row);
					const AxisArrRootChoice *root = find_root(preview, folder->root_id);

					row->detail = root != NULL ? g_strdup(root->volume_name) : NULL;
				}
				/* A plan read before the anime kind was kept: its series says
				 * what to do, not "did not answer". */
				if (item->skip == AXIS_ARR_SKIP_NOT_LOOKED_UP && work->sonarr && item->anime)
					row->detail = g_strdup("anime");
				g_ptr_array_add(preview->skipped, row);
			}
			preview->n_skipped_listed++;
			continue;
		}
		if (work->sonarr) {
			/* One Sonarr has as anime is brought as any other; counted, to
			 * be said once above the list. */
			if (item->anime)
				preview->bring_anime++;
			/* A series that would be brought, on the page shown: how Sonarr
			 * monitors it, what its lists held, and where it would go.  Its
			 * files were counted with them (look_up_series_files()). */
			if (n_bring / AXIS_ARR_PAGE_ROWS == work->pages.bring_page) {
				AxisArrBringRow *row = g_new0(AxisArrBringRow, 1);
				const AxisArrRootChoice *root = find_root(preview, item->to_root);
				guint p;

				g_array_append_val(work->bring_seqs, item->seq);
				row->year = item->year;
				row->seq = item->seq;
				row->monitored = item->monitored;
				row->n_episodes = (guint)MAX(item->n_episodes, 0);
				row->n_episodes_monitored = item->n_episodes_monitored;
				row->n_episodes_with_file = item->n_episodes_with_file;
				row->n_files = item->n_files;
				row->n_found = item->n_found;
				row->n_odd = item->n_odd;
				row->anime = item->anime;
				row->root = root != NULL ? g_strdup(root->name) : NULL;
				for (p = 0; p < preview->quality_profiles->len; p++) {
					const AxisArrChoice *choice = g_ptr_array_index(preview->quality_profiles, p);

					if (choice->id == item->to_profile)
						row->profile = g_strdup(choice->name);
				}
				g_ptr_array_add(preview->bringing, row);
			}
			n_bring++;
			continue;
		}
		preview->files[item->file]++;
		if (item->file == AXIS_ARR_FILE_NONE || item->file == AXIS_ARR_FILE_FOUND)
			continue;
		if (preview->n_unusable / AXIS_ARR_PAGE_ROWS == file_page) {
			AxisArrFileRow *row = g_new0(AxisArrFileRow, 1);
			const AxisArrRootChoice *root = find_root(preview, item->file_root);

			g_array_append_val(file_seqs, item->seq);
			row->year = item->year;
			row->state = item->file;
			if (root != NULL && item->file_stored != NULL)
				row->mapped = g_strdup_printf("%s: %s", root->name, item->file_stored);
			row->other = g_strdup(item->other);
			g_ptr_array_add(preview->unusable, row);
		}
		preview->n_unusable++;
	}
	/* What a confirmation is of: the plan, the choices and options, and
	 * everything the page counts.  A page made before any of them changed
	 * carries another digest, and its run is refused. */
	{
		g_autoptr(GString) text = g_string_new(NULL);
		g_autoptr(GString) choices = g_string_new(NULL);

		/* The choices in force, saved or suggested: what the page showed is
		 * what a run holds to.  A suggestion that would come out otherwise
		 * now (the index changed, a default did) is another digest, as a
		 * saved choice changed is. */
		g_string_append_printf(choices, "%" G_GINT64_FORMAT "|%d", preview->plan->id, preview->unmonitored ? 1 : 0);
		for (i = 0; i < preview->profile_rows->len; i++) {
			const AxisArrProfileRow *row = g_ptr_array_index(preview->profile_rows, i);

			g_string_append_printf(choices, "|p%" G_GINT64_FORMAT "=%" G_GINT64_FORMAT, row->arr_id, row->chosen);
			if (row->n_titles > 0 && row->from != AXIS_ARR_FROM_SAVED && row->from != AXIS_ARR_FROM_NOTHING)
				preview->n_suggested++;
		}
		for (i = 0; i < preview->folder_rows->len; i++) {
			const AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);

			if (row->from == AXIS_ARR_FROM_FILES)
				preview->n_suggested++;
			if (row->root_id > 0)
				g_string_append_printf(choices, "|f%" G_GSIZE_FORMAT ":%s=%" G_GINT64_FORMAT ":%s", strlen(row->prefix),
					row->prefix, row->root_id, row->below);
		}
		preview->choices = g_compute_checksum_for_string(G_CHECKSUM_SHA256, choices->str, -1);
		g_string_append(text, preview->choices);
		for (i = 0; i < AXIS_ARR_SKIP_N; i++)
			g_string_append_printf(text, "|%u", preview->counts[i]);
		for (i = 0; i < AXIS_ARR_FILE_N; i++)
			g_string_append_printf(text, "|%u", preview->files[i]);
		g_string_append_printf(text, "|%u", preview->n_relink);
		preview->digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text->str, -1);
		/* The same run with its tick to start searching at its end. */
		preview->digest_start = axis_arr_run_digest(preview->digest, TRUE);
	}
	return fill_pages(work, error);
}

static gboolean
preview_work(
	AxisArr             *self,
	gint64               source_id,
	const AxisArrPages  *pages,
	AxisArrPreview     **out,
	GPtrArray          **decisions,
	GError             **error
);

gboolean
axis_arr_preview(
	AxisArr         *self,
	gint64           source_id,
	AxisArrSkip      skip_filter,
	guint            skip_page,
	guint            file_page,
	AxisArrPreview **out,
	GError         **error
){
	AxisArrPages pages;

	memset(&pages, 0, sizeof(pages));
	pages.skip_filter = skip_filter;
	pages.skip_page = skip_page;
	pages.file_page = file_page;
	return preview_work(self, source_id, &pages, out, NULL, error);
}

gboolean
axis_arr_preview_pages(
	AxisArr             *self,
	gint64               source_id,
	const AxisArrPages  *pages,
	AxisArrPreview     **out,
	GError             **error
){
	return preview_work(self, source_id, pages, out, NULL, error);
}

gboolean
axis_arr_preview_decide(
	AxisArr         *self,
	gint64           source_id,
	AxisArrSkip      skip_filter,
	guint            skip_page,
	guint            file_page,
	AxisArrPreview **out,
	GPtrArray      **decisions,
	GError         **error
){
	AxisArrPages pages;

	memset(&pages, 0, sizeof(pages));
	pages.skip_filter = skip_filter;
	pages.skip_page = skip_page;
	pages.file_page = file_page;
	return preview_work(self, source_id, &pages, out, decisions, error);
}

static gboolean
preview_work(
	AxisArr             *self,
	gint64               source_id,
	const AxisArrPages  *pages,
	AxisArrPreview     **out,
	GPtrArray          **decisions,
	GError             **error
){
	g_autoptr(AxisArrPreview) preview = g_new0(AxisArrPreview, 1);
	g_autoptr(GPtrArray) decided = decisions != NULL ? g_ptr_array_new_with_free_func(axis_arr_decision_free) : NULL;
	Work work;
	gboolean ok;

	*out = NULL;
	if (decisions != NULL)
		*decisions = NULL;
	memset(&work, 0, sizeof(work));
	work.self = self;
	work.preview = preview;
	work.decisions = decided;
	if (pages != NULL)
		work.pages = *pages;
	if ((guint)work.pages.skip_filter >= AXIS_ARR_SKIP_N)
		work.pages.skip_filter = AXIS_ARR_SKIP_NONE;
	if (!axis_arr_get_source(self, source_id, &preview->source, error) ||
	    !axis_arr_plans(self, source_id, &preview->latest, &preview->plan, error))
		return FALSE;
	work.sonarr = preview->source->app == AXIS_ARR_SONARR;
	/* Whether a series can be brought at all now; a movie always can. */
	preview->series_ready = !work.sonarr || axis_arr_series_ready(self);
	if (preview->plan == NULL) {
		*out = g_steal_pointer(&preview);
		return TRUE;
	}
	preview->skip_filter = work.pages.skip_filter;
	preview->skip_page = work.pages.skip_page;
	preview->file_page = work.pages.file_page;
	preview->bring_page = work.pages.bring_page;
	preview->skipped = g_ptr_array_new_with_free_func(skip_row_free);
	preview->unusable = g_ptr_array_new_with_free_func(file_row_free);
	if (work.sonarr)
		preview->bringing = g_ptr_array_new_with_free_func(bring_row_free);
	work.skip_seqs = g_array_new(FALSE, FALSE, sizeof(guint));
	work.file_seqs = g_array_new(FALSE, FALSE, sizeof(guint));
	work.bring_seqs = g_array_new(FALSE, FALSE, sizeof(guint));
	mappings_parse(preview->source->mappings, work.sonarr, &work.mappings);
	work.conn = axis_db_pool_acquire(self->pool, error);
	if (work.conn == NULL) {
		mappings_clear(&work.mappings);
		g_array_unref(work.skip_seqs);
		g_array_unref(work.file_seqs);
		g_array_unref(work.bring_seqs);
		return FALSE;
	}
	ok = load_choices(&work, error) && load_plan_lists(&work, error) && load_roots(&work, error);
	if (ok) {
		/* A movie's file is on its item; a series' files are rows of their
		 * own, counted before the series are listed. */
		ok = apply_mappings(&work, error) && load_and_decide(&work, error) &&
			(work.sonarr ? look_up_series_files(&work, error) : look_up_files(&work, error)) &&
			tally(&work, error);
	}
	axis_db_pool_release(self->pool, work.conn);
	g_array_unref(work.skip_seqs);
	g_array_unref(work.file_seqs);
	g_array_unref(work.bring_seqs);
	g_clear_pointer(&work.seen_tmdb, g_hash_table_unref);
	g_clear_pointer(&work.seen_arr, g_hash_table_unref);
	g_clear_pointer(&work.items, g_ptr_array_unref);
	g_clear_pointer(&work.maps, g_ptr_array_unref);
	mappings_clear(&work.mappings);
	if (ok) {
		*out = g_steal_pointer(&preview);
		if (decisions != NULL)
			*decisions = g_steal_pointer(&decided);
	}
	return ok;
}
