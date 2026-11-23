/*
 * axis-tv-numbering.c - changing the source of numbering a managed series
 * follows: the plan (what each episode becomes, read only), the change in one
 * transaction, and what the metadata layer is told about the sources in use
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-metadata-tvdb, V3 (design.md section 3).  The rule that maps each row
 * is in axis-tv.h, above AxisTvMapped.  The plan is made from one read of the
 * series' rows, their identities, their files, the other source's episodes
 * and what is in flight; the change makes the plan again inside its
 * transaction, under the series' lock, and goes ahead only when its digest
 * is the one the confirmation page showed.
 *
 * Media safety: nothing here touches a file.  Rows are keyed anew in place
 * (their ids stay), unmapped ones are marked gone with their links, every
 * link stays on its row, and only files no link held are placed by their
 * names (axis-tv-identify.c).
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

#include "release/axis-release.h"

/* ── What is read ───────────────────────────────────────────────── */

typedef struct
{
	gint64    file_id;
	gchar    *path;
	gboolean  identified;
	gint      name_season;	/* -1: its name gives no one episode */
	gint      name_number;
	gboolean  earlier;	/* an import by Axis linked it, or its name was read under another numbering than the target's */
	gchar    *hand;		/* (nullable) a person made its link: how and who, a sentence */
} File;

typedef struct
{
	gint64        id;
	gchar        *provider_id;
	gint          season;
	gint          number;
	gchar        *title;
	gchar        *air_date;
	gint          runtime;
	gboolean      monitored;
	gboolean      grab;
	gboolean      gone;		/* marked gone already, kept for its files */
	gchar        *key;		/* its title's key, or NULL */
	/* The identity it held under the target before. */
	gboolean      held;
	gchar        *held_id;
	gint          held_season;
	gint          held_number;
	GPtrArray    *files;	/* File */
	/* The outcome. */
	gint          target;	/* index into the targets, -1: none */
	AxisTvMapped  how;
	guint         files_after;
} Row;

typedef struct
{
	gint      season;
	gint      number;
	gchar    *provider_id;
	gchar    *name;
	gchar    *air_date;
	gint      runtime;
	gint      absolute;
	gchar    *key;
	gint      row;		/* the row mapped to it, -1: none */
	gboolean  claimed;	/* a row of the target source holds it already */
	gboolean  doubled;	/* its id is listed at another place too: never matched */
	gboolean  first;	/* the first place of a doubled id (the one the sync adds) */
	guint     proposals;
	guint     live_proposals;	/* of them, by rows not gone */
	guint     unlinked;	/* files no link holds that the change places here */
} Target;

/* A row of the target source kept from before (marked gone by an earlier
 * change): it comes back with its files. */
typedef struct
{
	gint64    id;
	gchar    *provider_id;
	gint      season;
	gint      number;
	gchar    *title;
	gchar    *air_date;
	guint     n_files;
	gchar    *file_path;
	gboolean  monitored;
} Kept;

static void
file_free(File *file)
{
	g_free(file->path);
	g_free(file->hand);
	g_free(file);
}

static void
row_free(Row *row)
{
	g_free(row->provider_id);
	g_free(row->title);
	g_free(row->air_date);
	g_free(row->key);
	g_free(row->held_id);
	g_clear_pointer(&row->files, g_ptr_array_unref);
	g_free(row);
}

static void
target_free(Target *target)
{
	g_free(target->provider_id);
	g_free(target->name);
	g_free(target->air_date);
	g_free(target->key);
	g_free(target);
}

static void
kept_free(Kept *kept)
{
	g_free(kept->provider_id);
	g_free(kept->title);
	g_free(kept->air_date);
	g_free(kept->file_path);
	g_free(kept);
}

typedef struct
{
	AxisTvSeries *series;
	gchar        *to;
	GPtrArray    *rows;		/* Row */
	GPtrArray    *targets;		/* Target */
	GPtrArray    *kept;		/* Kept */
	GHashTable   *season_on;	/* season + 1 -> 1 off, 2 on */
	GPtrArray    *waits;		/* gchar * */
	gchar        *digest;
	gchar        *busy;		/* (nullable) an import of the series is open: nothing is placed by name */
	guint         unfetched;	/* target seasons that list episodes never fetched */
	gint          unfetched_first;
	GHashTable   *held_ids;		/* the target's ids rows held before */
	GString      *notes;		/* what cannot be matched, plainly */
} Read;

static void
read_clear(Read *read)
{
	g_clear_pointer(&read->held_ids, g_hash_table_unref);
	if (read->notes != NULL)
		g_string_free(read->notes, TRUE);
	g_clear_pointer(&read->series, axis_tv_series_free);
	g_free(read->to);
	g_clear_pointer(&read->rows, g_ptr_array_unref);
	g_clear_pointer(&read->targets, g_ptr_array_unref);
	g_clear_pointer(&read->kept, g_ptr_array_unref);
	g_clear_pointer(&read->season_on, g_hash_table_unref);
	g_clear_pointer(&read->waits, g_ptr_array_unref);
	g_free(read->digest);
	g_free(read->busy);
	memset(read, 0, sizeof(*read));
}

static gint64
get_int(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
	return value;
}

static gchar *
get_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

static gboolean
get_bool(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/*
 * A title as it is compared: its letters and digits only, case folded; none
 * for an empty one or a stand-in for a missing title ("Episode 4", which
 * providers write where they have none).
 */
static gchar *
title_key(const gchar *title)
{
	g_autofree gchar *folded = NULL;
	GString *key;
	const gchar *p;
	gsize digits;

	if (title == NULL || title[0] == '\0' || !g_utf8_validate(title, -1, NULL))
		return NULL;
	folded = g_utf8_casefold(title, -1);
	key = g_string_new(NULL);
	for (p = folded; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isalnum(c))
			g_string_append_unichar(key, c);
	}
	digits = strspn(key->str + MIN(key->len, strlen("episode")), "0123456789");
	if (key->len == 0 || (g_str_has_prefix(key->str, "episode") && digits > 0 &&
	    strlen("episode") + digits == key->len)) {
		g_string_free(key, TRUE);
		return NULL;
	}
	return g_string_free(key, FALSE);
}

/* The one episode a file's name gives (the reading imports and
 * identification use), or -1. */
void
axis_tv_name_episode(
	const gchar *path,
	gint        *season,
	gint        *number
){
	g_autofree gchar *base = path != NULL ? g_path_get_basename(path) : NULL;
	g_autoptr(AxisRelease) release = NULL;
	AxisReleaseStates states;

	*season = -1;
	*number = -1;
	if (base == NULL)
		return;
	release = axis_release_parse(base);
	axis_release_states(release, &states);
	if (states.kind == AXIS_RELEASE_HOLDS_EPISODES && states.n_episodes == 1) {
		*season = states.season_first;
		*number = states.episodes[0];
	}
}

static gint
find_target(
	GPtrArray   *targets,
	const gchar *provider_id,
	gint         season,
	gint         number
){
	guint i;

	for (i = 0; i < targets->len; i++) {
		const Target *t = g_ptr_array_index(targets, i);

		if (provider_id != NULL ? g_strcmp0(t->provider_id, provider_id) == 0 :
		    t->season == season && t->number == number)
			return (gint)i;
	}
	return -1;
}

/* The episode a row of the target source kept from before comes back to, as
 * the sync takes it back: by its id, else (it has none) by its place, to an
 * episode that has no id either. */
static gint
find_kept_target(
	GPtrArray   *targets,
	const gchar *provider_id,
	gint         season,
	gint         number
){
	guint i;

	if (provider_id != NULL)
		return find_target(targets, provider_id, season, number);
	for (i = 0; i < targets->len; i++) {
		const Target *t = g_ptr_array_index(targets, i);

		if (t->provider_id == NULL && t->season == season && t->number == number)
			return (gint)i;
	}
	return -1;
}

/* Everything the plan is made from, on @conn (in the caller's transaction
 * when it changes the series). */
static gboolean
read_all(
	AxisTv       *self,
	AxisDbConn   *conn,
	gint64        series_id,
	const gchar  *to,
	Read         *read,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisDbResult) identities = NULL;
	g_autoptr(AxisDbResult) targets = NULL;
	g_autoptr(AxisDbResult) seasons = NULL;
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(AxisDbResult) monitoring = NULL;
	g_autoptr(AxisDbResult) grabs = NULL;
	g_autoptr(AxisDbResult) imports = NULL;
	g_autoptr(AxisDbResult) searches = NULL;
	g_autoptr(AxisDbResult) digest = NULL;
	g_autoptr(AxisDbResult) unlinked = NULL;
	g_autoptr(GHashTable) target_ids = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GHashTable) kept_by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autofree gchar *title = NULL;
	g_autofree gchar *key_prefix = NULL;
	const gchar *params[3];
	guint i;

	memset(read, 0, sizeof(*read));
	if (!axis_tv_store_series_one(conn, series_id, &read->series, error))
		return FALSE;
	if (read->series == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	read->to = g_strdup(to);
	read->rows = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	read->targets = g_ptr_array_new_with_free_func((GDestroyNotify)target_free);
	read->kept = g_ptr_array_new_with_free_func((GDestroyNotify)kept_free);
	read->season_on = g_hash_table_new(g_direct_hash, g_direct_equal);
	read->waits = g_ptr_array_new_with_free_func(g_free);
	read->held_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	read->unfetched_first = -1;
	title = axis_tv_int_text(read->series->title_id);
	params[0] = id;
	params[1] = to;
	params[2] = read->series->episode_source;
	if (!axis_db_conn_exec(conn, "SELECT e.id, e.provider_id, e.season_number, e.number, e.title, e.air_date::text,"
	        " coalesce(e.runtime_minutes, 0), e.monitored, e.source, e.gone_at IS NOT NULL,"
	        " EXISTS (SELECT 1 FROM series_grab_episode ge WHERE ge.episode_id = e.id AND ge.open)"
	        " FROM series_episode e WHERE e.series_id = $1::bigint AND (e.source = $2 OR (e.source = $3"
	        " AND (e.gone_at IS NULL OR EXISTS (SELECT 1 FROM series_file_episode gf JOIN library_file glf ON glf.id ="
	        " gf.file_id AND glf.state = 'present' WHERE gf.episode_id = e.id))))"
	        " ORDER BY e.season_number, e.number, e.id", 3, params, &rows, error) ||
	    !axis_db_conn_exec(conn, "SELECT i.episode_id, i.provider_id, i.season_number, i.number"
	        " FROM series_episode_identity i JOIN series_episode e ON e.id = i.episode_id"
	        " WHERE e.series_id = $1::bigint AND i.source = $2", 2, params, &identities, error) ||
	    !axis_db_conn_exec(conn, "SELECT sfe.episode_id, sf.file_id, lf.rel_path, sf.identity_id IS NOT NULL,"
	        " (SELECT CASE WHEN count(*) = 1 THEN min(lie.season_number) END FROM library_identity_episode lie"
	        "   WHERE lie.file_id = sf.file_id AND lie.air_date IS NULL),"
	        " (SELECT CASE WHEN count(*) = 1 THEN min(lie.number) END FROM library_identity_episode lie"
	        "   WHERE lie.file_id = sf.file_id AND lie.air_date IS NULL),"
	        " sf.import_id IS NOT NULL OR sf.read_source IS DISTINCT FROM $2,"
	        " CASE WHEN sf.hand_at IS NOT NULL THEN CASE sf.hand_how WHEN 'forced' THEN 'From a release grabbed by force'"
	        "   WHEN 'assigned' THEN 'Assigned by hand' WHEN 'brought' THEN 'Migrated from Sonarr'"
	        "   ELSE 'Linked by hand' END"
	        "   || coalesce(' by ' || sf.hand_by, '') END"
	        " FROM series_file sf JOIN series_file_episode sfe ON sfe.file_id = sf.file_id"
	        " JOIN library_file lf ON lf.id = sf.file_id AND lf.state = 'present'"
	        " WHERE sf.series_id = $1::bigint ORDER BY lf.rel_path, sf.file_id", 2, params, &files, error) ||
	    !axis_db_conn_exec(conn, "SELECT number, monitored FROM series_season WHERE series_id = $1::bigint", 1, params,
	        &monitoring, error) ||
	    !axis_db_conn_exec(conn, "SELECT g.release_name, coalesce(d.state, 'sending') FROM series_grab g"
	        " LEFT JOIN download d ON d.id = g.download_id WHERE g.series_id = $1::bigint AND g.closed_at IS NULL"
	        " ORDER BY g.id", 1, params, &grabs, error))
		return FALSE;
	params[0] = title;
	/* In the sync's order (axis-tv-sync.c): an id listed twice is kept where
	 * the season fetched last lists it, as the sync keeps it. */
	if (!axis_db_conn_exec(conn, "SELECT me.season_number, me.number, me.provider_id, me.name, me.air_date::text,"
	        " coalesce(me.runtime_minutes, 0), coalesce(me.absolute_number, 0) FROM meta_episode me"
	        " JOIN meta_season ms ON ms.title_id = me.title_id AND ms.source = me.source AND ms.number = me.season_number"
	        " WHERE me.title_id = $1::bigint AND me.source = $2"
	        " ORDER BY ms.episodes_fetched_at DESC NULLS LAST, me.season_number, me.number", 2, params, &targets, error) ||
	    !axis_db_conn_exec(conn, "SELECT number FROM meta_season WHERE title_id = $1::bigint AND source = $2"
	        " AND episodes_fetched_at IS NULL AND coalesce(episode_count, 1) > 0 ORDER BY number", 2, params, &seasons,
	        error) ||
	    !axis_db_conn_exec(conn, "SELECT coalesce(i.release_name, i.title_label), i.state FROM import i"
	        " WHERE i.module = '" AXIS_TV_MODULE_ID "' AND i.title_id = $1::bigint"
	        " AND i.state IN " AXIS_TV_IMPORT_OPEN " ORDER BY i.id", 1, params, &imports, error))
		return FALSE;
	/* The series' files identification found that no link holds, where the
	 * change will place them: read by the very placement it uses
	 * (library_episode_file, axis_tv_link_unlinked()), with the series
	 * following the other source for a moment, undone at once. */
	{
		const gchar *swap[2];
		gboolean placed;

		swap[0] = id;
		swap[1] = to;
		if (!axis_db_conn_exec(conn, "SAVEPOINT numbering_plan", 0, NULL, NULL, error))
			return FALSE;
		placed = axis_db_conn_exec(conn, "UPDATE series SET episode_source = $2 WHERE id = $1::bigint", 2, swap, NULL,
			error) && axis_db_conn_exec(conn, "SELECT v.season_number, v.number FROM library_episode_file v"
			" WHERE v.title_id = $1::bigint AND NOT EXISTS (SELECT 1 FROM series_file sf WHERE sf.file_id = v.file_id)"
			/* (A file a person unlinked by hand stays unlinked.) */
			" AND NOT EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = v.file_id)",
			1, params, &unlinked, error);
		if (!axis_db_conn_exec(conn, "ROLLBACK TO SAVEPOINT numbering_plan", 0, NULL, NULL, placed ? error : NULL) ||
		    !placed)
			return FALSE;
	}
	key_prefix = g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":%%", series_id);
	params[0] = key_prefix;
	if (!axis_db_conn_exec(conn, "SELECT count(*) FROM job WHERE kind = '" AXIS_TV_SEARCH_KIND "' AND state = 'running'"
	        " AND dedupe_key LIKE $1", 1, params, &searches, error))
		return FALSE;
	/* What the series and the other source are now, in one value: the rows,
	 * their links, the open grabs, the seasons' monitoring and the other
	 * source's episodes. */
	params[0] = id;
	params[1] = to;
	if (!axis_db_conn_exec(conn, "SELECT md5(concat_ws('|', s.episode_source,"
	        " coalesce((SELECT string_agg(e.id || ':' || e.source || ':' || coalesce(e.provider_id, '') || ':'"
	        "   || e.season_number || ':' || e.number || ':' || (e.gone_at IS NULL)::text || ':' || e.monitored::text"
	        "   || ':' || coalesce(e.title, '') || ':' || coalesce(e.air_date::text, '') || ':'"
	        "   || coalesce(e.runtime_minutes, 0), ',' ORDER BY e.id) FROM series_episode e WHERE e.series_id = s.id), ''),"
	        " coalesce((SELECT string_agg(f.file_id || ':' || f.episode_id || ':' || lf.rel_path || ':' || lf.state, ','"
	        "   ORDER BY f.file_id, f.episode_id) FROM series_file_episode f JOIN series_episode e ON e.id = f.episode_id"
	        "   JOIN library_file lf ON lf.id = f.file_id WHERE e.series_id = s.id), ''),"
	        " coalesce((SELECT string_agg(lie.file_id || ':' || coalesce(lie.season_number, -1) || ':'"
	        "   || coalesce(lie.number, -1) || ':' || coalesce(lie.air_date::text, ''), ',' ORDER BY lie.file_id,"
	        "   lie.season_number, lie.number) FROM library_identity_episode lie JOIN library_identity_file lif"
	        "   ON lif.file_id = lie.file_id JOIN library_identity li ON li.id = lif.identity_id"
	        "   WHERE li.title_id = s.title_id), ''),"
	        " coalesce((SELECT string_agg(g.id::text, ',' ORDER BY g.id) FROM series_grab g WHERE g.series_id = s.id"
	        "   AND g.closed_at IS NULL), ''),"
	        " coalesce((SELECT string_agg(ss.number || ':' || ss.monitored::text, ',' ORDER BY ss.number)"
	        "   FROM series_season ss WHERE ss.series_id = s.id), ''),"
	        " coalesce((SELECT string_agg(m.season_number || ':' || m.number || ':' || coalesce(m.provider_id, '') || ':'"
	        "   || coalesce(m.name, '') || ':' || coalesce(m.air_date::text, '') || ':' || coalesce(m.runtime_minutes, 0),"
	        "   ',' ORDER BY m.season_number, m.number) FROM meta_episode m WHERE m.title_id = s.title_id"
	        "   AND m.source = $2), ''))) FROM series s WHERE s.id = $1::bigint", 2, params, &digest, error))
		return FALSE;
	read->digest = get_text(digest, 0, 0);
	if (!axis_tv_import_open(conn, series_id, &read->busy, error))
		return FALSE;

	/* The other source's episodes. */
	for (i = 0; i < axis_db_result_get_n_rows(targets); i++) {
		Target *t = g_new0(Target, 1);

		t->season = (gint)get_int(targets, i, 0, 0);
		t->number = (gint)get_int(targets, i, 1, 0);
		t->provider_id = get_text(targets, i, 2);
		t->name = get_text(targets, i, 3);
		t->air_date = get_text(targets, i, 4);
		t->runtime = (gint)get_int(targets, i, 5, 0);
		t->absolute = (gint)get_int(targets, i, 6, 0);
		t->key = title_key(t->name);
		t->row = -1;
		g_ptr_array_add(read->targets, t);
		/* One id listed at two places (a provider's slip): neither place is
		 * matched; the sync adds the id once, at its first place. */
		if (t->provider_id != NULL) {
			Target *other = g_hash_table_lookup(target_ids, t->provider_id);

			if (other == NULL) {
				g_hash_table_insert(target_ids, t->provider_id, t);
			} else {
				g_autofree gchar *one = axis_tv_episode_label(other->season, other->number);
				g_autofree gchar *two = axis_tv_episode_label(t->season, t->number);

				if (!other->doubled) {
					other->doubled = TRUE;
					other->first = TRUE;
				}
				t->doubled = TRUE;
				if (read->notes == NULL)
					read->notes = g_string_new(NULL);
				g_string_append_printf(read->notes, "%s%s lists one episode at two places (%s and %s): neither is "
					"matched to an episode of the series, and it is added once, as %s.", read->notes->len > 0 ? " " : "",
					axis_tv_source_label(to), one, two, one);
			}
		}
	}
	read->unfetched = axis_db_result_get_n_rows(seasons);
	if (read->unfetched > 0)
		read->unfetched_first = (gint)get_int(seasons, 0, 0, 0);

	/* The series' rows: its live ones, and those of the target kept from
	 * before, which hold their episodes already. */
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *source = axis_db_result_get_text(rows, i, 8);

		if (g_strcmp0(source, to) == 0) {
			Kept *k = g_new0(Kept, 1);
			gint t;

			k->id = get_int(rows, i, 0, 0);
			k->provider_id = get_text(rows, i, 1);
			k->season = (gint)get_int(rows, i, 2, 0);
			k->number = (gint)get_int(rows, i, 3, 0);
			k->title = get_text(rows, i, 4);
			k->air_date = get_text(rows, i, 5);
			k->monitored = get_bool(rows, i, 7);
			t = find_kept_target(read->targets, k->provider_id, k->season, k->number);
			if (t >= 0)
				((Target *)g_ptr_array_index(read->targets, t))->claimed = TRUE;
			g_ptr_array_add(read->kept, k);
			g_hash_table_insert(kept_by_id, &k->id, k);
			continue;
		}
		{
			Row *r = g_new0(Row, 1);

			r->id = get_int(rows, i, 0, 0);
			r->provider_id = get_text(rows, i, 1);
			r->season = (gint)get_int(rows, i, 2, 0);
			r->number = (gint)get_int(rows, i, 3, 0);
			r->title = get_text(rows, i, 4);
			r->air_date = get_text(rows, i, 5);
			r->runtime = (gint)get_int(rows, i, 6, 0);
			r->monitored = get_bool(rows, i, 7);
			r->gone = get_bool(rows, i, 9);
			r->grab = get_bool(rows, i, 10);
			r->key = title_key(r->title);
			r->files = g_ptr_array_new_with_free_func((GDestroyNotify)file_free);
			r->target = -1;
			r->how = AXIS_TV_MAPPED_NONE;
			g_ptr_array_add(read->rows, r);
			g_hash_table_insert(by_id, &r->id, r);
		}
	}
	for (i = 0; i < axis_db_result_get_n_rows(identities); i++) {
		gint64 episode = get_int(identities, i, 0, 0);
		Row *r = g_hash_table_lookup(by_id, &episode);

		if (r == NULL)
			continue;
		r->held = TRUE;
		r->held_id = get_text(identities, i, 1);
		r->held_season = (gint)get_int(identities, i, 2, 0);
		r->held_number = (gint)get_int(identities, i, 3, 0);
		if (r->held_id != NULL)
			g_hash_table_add(read->held_ids, g_strdup(r->held_id));
	}
	/* The files no link holds, where the placement puts them. */
	for (i = 0; i < axis_db_result_get_n_rows(unlinked); i++) {
		gint t = find_target(read->targets, NULL, (gint)get_int(unlinked, i, 0, -1), (gint)get_int(unlinked, i, 1, -1));

		if (t >= 0)
			((Target *)g_ptr_array_index(read->targets, t))->unlinked++;
	}
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		gint64 episode = get_int(files, i, 0, 0);
		Row *r = g_hash_table_lookup(by_id, &episode);
		Kept *k = r == NULL ? g_hash_table_lookup(kept_by_id, &episode) : NULL;
		File *f;

		if (k != NULL) {
			k->n_files++;
			if (k->file_path == NULL)
				k->file_path = get_text(files, i, 2);
			continue;
		}
		if (r == NULL)
			continue;
		f = g_new0(File, 1);
		f->file_id = get_int(files, i, 1, 0);
		f->path = get_text(files, i, 2);
		f->identified = get_bool(files, i, 3);
		/* Identification placed it by its own reading of the name (and the
		 * folder's); an import named it itself, S01E02 and all. */
		if (f->identified && axis_db_result_get_text(files, i, 4) != NULL) {
			f->name_season = (gint)get_int(files, i, 4, -1);
			f->name_number = (gint)get_int(files, i, 5, -1);
		} else {
			axis_tv_name_episode(f->path, &f->name_season, &f->name_number);
		}
		f->earlier = get_bool(files, i, 6);
		f->hand = get_text(files, i, 7);
		g_ptr_array_add(r->files, f);
	}
	for (i = 0; i < axis_db_result_get_n_rows(monitoring); i++)
		g_hash_table_insert(read->season_on, GINT_TO_POINTER((gint)get_int(monitoring, i, 0, 0) + 1),
			GINT_TO_POINTER(get_bool(monitoring, i, 1) ? 2 : 1));

	/* What it waits for. */
	for (i = 0; i < axis_db_result_get_n_rows(grabs); i++)
		g_ptr_array_add(read->waits, g_strdup_printf("%s (%s)", axis_db_result_get_text(grabs, i, 0),
			g_strcmp0(axis_db_result_get_text(grabs, i, 1), "completed") == 0 ? "downloaded, to import" :
			"downloading"));
	for (i = 0; i < axis_db_result_get_n_rows(imports); i++) {
		const gchar *name = axis_db_result_get_text(imports, i, 0);
		gboolean named = FALSE;
		guint k;

		/* An open grab's import is named once. */
		for (k = 0; k < axis_db_result_get_n_rows(grabs) && !named; k++)
			named = g_strcmp0(axis_db_result_get_text(grabs, k, 0), name) == 0;
		if (!named)
			g_ptr_array_add(read->waits, g_strdup_printf("%s (an import that is %s)", name,
				g_strcmp0(axis_db_result_get_text(imports, i, 1), "partial") == 0 ?
				"partly imported: retry it or dismiss it" : axis_db_result_get_text(imports, i, 1)));
	}
	if (get_int(searches, 0, 0, 0) > 0)
		g_ptr_array_add(read->waits, g_strdup("a search of the series that is running now"));
	return TRUE;
}

/* ── The mapping ────────────────────────────────────────────────── */

typedef enum
{
	STEP_HELD,
	STEP_DATE_TITLE,
	STEP_DATE,
	STEP_TITLE,
	STEP_FILE_NAME
} Step;

static const AxisTvMapped step_how[] = {
	AXIS_TV_MAPPED_HELD, AXIS_TV_MAPPED_DATE_TITLE, AXIS_TV_MAPPED_DATE, AXIS_TV_MAPPED_TITLE,
	AXIS_TV_MAPPED_FILE_NAME
};

/* How many of @rows (live) have air date @date; of @key. */
static guint
rows_with(
	GPtrArray   *rows,
	const gchar *date,
	const gchar *key
){
	guint n = 0;
	guint i;

	for (i = 0; i < rows->len; i++) {
		const Row *r = g_ptr_array_index(rows, i);

		if ((date != NULL && g_strcmp0(r->air_date, date) == 0) || (key != NULL && g_strcmp0(r->key, key) == 0))
			n++;
	}
	return n;
}

static guint
targets_with(
	GPtrArray   *targets,
	const gchar *date,
	const gchar *key
){
	guint n = 0;
	guint i;

	for (i = 0; i < targets->len; i++) {
		const Target *t = g_ptr_array_index(targets, i);

		if ((date != NULL && g_strcmp0(t->air_date, date) == 0) || (key != NULL && g_strcmp0(t->key, key) == 0))
			n++;
	}
	return n;
}

static gboolean
runtimes_alike(
	gint a,
	gint b
){
	return a > 0 && b > 0 && MIN(a, b) * 2 >= MAX(a, b);
}

/* Whether @r may be paired with @t at @step. */
static gboolean
fits(
	Read         *read,
	const Row    *r,
	const Target *t,
	Step          step
){
	guint i;

	switch (step) {
	case STEP_HELD:
		if (!r->held)
			return FALSE;
		if (r->held_id == NULL)
			return r->held_season == t->season && r->held_number == t->number;
		if (g_strcmp0(r->held_id, t->provider_id) == 0)
			return TRUE;
		/* That source gave its episodes new ids since the row held one (its
		 * record changed while the series followed the other): the place it
		 * held, when its id is listed no more, the episode there has an id no
		 * row held, and its air date or its title is the row's (a provider
		 * that deleted the episode and listed another there is not taken for
		 * it: the ordinary rules decide, flagged). */
		return r->held_season == t->season && r->held_number == t->number &&
			find_target(read->targets, r->held_id, 0, 0) < 0 &&
			(t->provider_id == NULL || !g_hash_table_contains(read->held_ids, t->provider_id)) &&
			((r->air_date != NULL && g_strcmp0(r->air_date, t->air_date) == 0) ||
			 (r->key != NULL && g_strcmp0(r->key, t->key) == 0));
	case STEP_DATE_TITLE:
		return r->air_date != NULL && r->key != NULL && g_strcmp0(r->air_date, t->air_date) == 0 &&
			g_strcmp0(r->key, t->key) == 0;
	case STEP_DATE:
		return r->air_date != NULL && g_strcmp0(r->air_date, t->air_date) == 0 &&
			rows_with(read->rows, r->air_date, NULL) == 1 && targets_with(read->targets, t->air_date, NULL) == 1 &&
			runtimes_alike(r->runtime, t->runtime);
	case STEP_TITLE:
		return r->air_date == NULL && r->key != NULL && g_strcmp0(r->key, t->key) == 0 &&
			rows_with(read->rows, NULL, r->key) == 1 && targets_with(read->targets, NULL, t->key) == 1;
	default:
		/* Every file names this one episode. */
		if (r->files->len == 0)
			return FALSE;
		for (i = 0; i < r->files->len; i++) {
			const File *f = g_ptr_array_index(r->files, i);

			if (f->name_season != t->season || f->name_number != t->number)
				return FALSE;
		}
		return TRUE;
	}
}

/* One step: each unmapped row that fits exactly one free target proposes it,
 * and a target proposed by one row alone takes it. */
static void
map_step(
	Read *read,
	Step  step
){
	g_autoptr(GArray) choice = g_array_sized_new(FALSE, TRUE, sizeof(gint), read->rows->len);
	guint i;
	guint k;

	g_array_set_size(choice, read->rows->len);
	for (k = 0; k < read->targets->len; k++) {
		((Target *)g_ptr_array_index(read->targets, k))->proposals = 0;
		((Target *)g_ptr_array_index(read->targets, k))->live_proposals = 0;
	}
	for (i = 0; i < read->rows->len; i++) {
		const Row *r = g_ptr_array_index(read->rows, i);
		gint found = -1;
		guint n = 0;

		g_array_index(choice, gint, i) = -1;
		if (r->target >= 0)
			continue;
		for (k = 0; k < read->targets->len; k++) {
			const Target *t = g_ptr_array_index(read->targets, k);

			if (t->claimed || t->doubled || t->row >= 0 || !fits(read, r, t, step))
				continue;
			found = (gint)k;
			n++;
		}
		if (n == 1) {
			Target *t = g_ptr_array_index(read->targets, found);

			g_array_index(choice, gint, i) = found;
			t->proposals++;
			if (!r->gone)
				t->live_proposals++;
		}
	}
	for (i = 0; i < read->rows->len; i++) {
		Row *r = g_ptr_array_index(read->rows, i);
		gint found = g_array_index(choice, gint, i);
		Target *t;

		if (found < 0)
			continue;
		t = g_ptr_array_index(read->targets, found);
		/* One row alone; or, against rows already gone (duplicates kept for
		 * their files), the one live row: the gone ones stay as they are. */
		if (t->proposals != 1 && !(!r->gone && t->live_proposals == 1))
			continue;
		r->target = found;
		r->how = step_how[step];
		t->row = (gint)i;
	}
}

static gboolean
aired(
	const gchar       *date,
	const AxisTvRules *rules
){
	gint64 start = axis_tv_date_us(date);

	return start > 0 && start + rules->air_delay_s * G_USEC_PER_SEC <= rules->now_us;
}

/* A target's season monitoring as the sync gives a new row: its season's,
 * else the series' choice for new seasons. */
static gboolean
season_monitored(
	const Read *read,
	gint        season
){
	gint on = GPOINTER_TO_INT(g_hash_table_lookup(read->season_on, GINT_TO_POINTER(season + 1)));

	if (on != 0)
		return on == 2;
	return read->series->monitor_new && season > 0;
}

/* The episodes added, by season and number. */
static gint
compare_added(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvRenumberNew *x = *(const AxisTvRenumberNew *const *)a;
	const AxisTvRenumberNew *y = *(const AxisTvRenumberNew *const *)b;

	if (x->season != y->season)
		return x->season < y->season ? -1 : 1;
	return x->number < y->number ? -1 : x->number > y->number;
}

static gint
compare_place_files(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvPlaceFile *x = a;
	const AxisTvPlaceFile *y = b;

	return x->file_id < y->file_id ? -1 : x->file_id > y->file_id;
}

/* Where placing each file by its name after the change would put it: the
 * placement engine of axis-tv-place.c, run over the episodes as the change
 * would leave them (their files after it; the episodes it adds), the files
 * in the order the placing itself takes them (by id).  A file an import by
 * Axis linked, or whose name was read under another numbering than the one
 * followed after, is shown but never placed. */
static void
plan_names(
	const Read         *read,
	AxisTvRenumberPlan *plan,
	const AxisTvRules  *rules
){
	g_autoptr(GArray) eps = g_array_new(FALSE, TRUE, sizeof(AxisTvPlaceEp));
	g_autoptr(GArray) files = g_array_new(FALSE, TRUE, sizeof(AxisTvPlaceFile));
	g_autoptr(GArray) epidx = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	guint i;
	guint k;

	g_array_set_clear_func(files, (GDestroyNotify)axis_tv_place_file_clear);
	/* A file on several rows is never placed by its name. */
	for (i = 0; i < read->rows->len; i++) {
		const Row *r = g_ptr_array_index(read->rows, i);

		for (k = 0; k < r->files->len; k++) {
			gpointer key = GSIZE_TO_POINTER((gsize)((const File *)g_ptr_array_index(r->files, k))->file_id);

			g_hash_table_insert(seen, key, GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(seen, key)) + 1));
		}
	}
	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);
		gint at = -1;

		if (row->to_season >= 0) {
			AxisTvPlaceEp ep;

			memset(&ep, 0, sizeof(ep));
			ep.season = row->to_season;
			ep.number = row->to_number;
			ep.monitored = row->monitored;
			ep.aired = aired(row->to_air_date, rules);
			ep.grab = row->grab;
			ep.files = row->n_files_after;
			at = (gint)eps->len;
			g_array_append_val(eps, ep);
		}
		g_array_append_val(epidx, at);
	}
	for (i = 0; i < plan->added->len; i++) {
		const AxisTvRenumberNew *a = g_ptr_array_index(plan->added, i);
		AxisTvPlaceEp ep;

		memset(&ep, 0, sizeof(ep));
		ep.season = a->season;
		ep.number = a->number;
		ep.monitored = a->monitored;
		ep.aired = a->aired;
		ep.files = a->gets_file ? 1 : 0;
		g_array_append_val(eps, ep);
	}
	for (i = 0; i < read->rows->len; i++) {
		const Row *r = g_ptr_array_index(read->rows, i);
		gint at = g_array_index(epidx, gint, i);

		if (at < 0)
			continue;
		for (k = 0; k < r->files->len; k++) {
			const File *f = g_ptr_array_index(r->files, k);
			const AxisTvPlaceEp *ep = &g_array_index(eps, AxisTvPlaceEp, (guint)at);
			AxisTvPlaceFile pf;

			/* (A link a person made is never listed here and never placed by
			 * the box: it is in the group of its own.) */
			if (f->hand != NULL || f->name_season < 0 ||
			    GPOINTER_TO_UINT(g_hash_table_lookup(seen, GSIZE_TO_POINTER((gsize)f->file_id))) > 1 ||
			    (f->name_season == ep->season && f->name_number == ep->number))
				continue;
			memset(&pf, 0, sizeof(pf));
			pf.file_id = f->file_id;
			pf.path = g_strdup(f->path);
			pf.from = (guint)at;
			pf.name_season = f->name_season;
			pf.name_number = f->name_number;
			pf.earlier = f->earlier;
			pf.selected = !f->earlier;
			pf.to = -1;
			g_array_append_val(files, pf);
		}
	}
	g_array_sort(files, compare_place_files);
	axis_tv_place_run(eps, files, read->busy);
	plan->names_left = axis_tv_place_left(eps);
	for (i = 0; i < files->len; i++) {
		const AxisTvPlaceFile *f = &g_array_index(files, AxisTvPlaceFile, i);
		const AxisTvPlaceEp *on = &g_array_index(eps, AxisTvPlaceEp, f->from);
		g_autofree gchar *said = axis_tv_episode_label(f->name_season, f->name_number);
		AxisTvRenumberedFile *out = g_new0(AxisTvRenumberedFile, 1);

		out->path = g_strdup(f->path);
		out->name_season = f->name_season;
		out->name_number = f->name_number;
		out->to_season = on->season;
		out->to_number = on->number;
		out->earlier = f->earlier;
		out->placed = f->placed;
		if (f->earlier)
			out->by_name = g_strdup("Not placed by the box: its name was read under the earlier numbering, or an import "
				"by Axis made its link, so the name is probably just the old number");
		else if (f->placed)
			out->by_name = g_strdup_printf("Placed on %s", said);
		else
			out->by_name = g_strdup_printf("Not placed: %s", f->refused != NULL ? f->refused : "it cannot be");
		if (f->placed)
			plan->n_names_placed++;
		g_ptr_array_add(plan->names, out);
	}
}

static AxisTvRenumberPlan *
plan_from(
	AxisTv *self,
	Read   *read
){
	AxisTvRenumberPlan *plan = g_new0(AxisTvRenumberPlan, 1);
	AxisTvRules rules;
	guint i;
	guint k;

	axis_tv_get_rules(self, &rules);
	map_step(read, STEP_HELD);
	map_step(read, STEP_DATE_TITLE);
	map_step(read, STEP_DATE);
	map_step(read, STEP_TITLE);
	map_step(read, STEP_FILE_NAME);

	plan->series_id = read->series->id;
	plan->from = g_strdup(read->series->episode_source);
	plan->to = g_strdup(read->to);
	plan->digest = g_strdup(read->digest);
	plan->rows = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_renumbered_free);
	plan->added = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_renumber_new_free);
	plan->names = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_renumbered_file_free);
	plan->hand = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_renumbered_file_free);
	plan->waits = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < read->waits->len; i++)
		g_ptr_array_add(plan->waits, g_strdup(g_ptr_array_index(read->waits, i)));

	/* Every linked file stays on the row it is on (the episode it holds: the
	 * mapping says which episode that row is now); only a file no link held
	 * is placed by its name (Target.gets_file). */
	for (i = 0; i < read->rows->len; i++) {
		Row *r = g_ptr_array_index(read->rows, i);
		const Target *t = r->target >= 0 ? g_ptr_array_index(read->targets, r->target) : NULL;

		r->files_after = r->files->len + (t != NULL ? t->unlinked : 0);
	}
	if (read->notes != NULL)
		plan->notes = g_strdup(read->notes->str);
	for (i = 0; i < read->rows->len; i++) {
		const Row *r = g_ptr_array_index(read->rows, i);
		const Target *t = r->target >= 0 ? g_ptr_array_index(read->targets, r->target) : NULL;
		AxisTvRenumbered *out = g_new0(AxisTvRenumbered, 1);

		out->episode_id = r->id;
		out->season = r->season;
		out->number = r->number;
		out->title = g_strdup(r->title);
		out->air_date = g_strdup(r->air_date);
		out->how = r->how;
		out->to_season = t != NULL ? t->season : -1;
		out->to_number = t != NULL ? t->number : -1;
		out->to_title = t != NULL ? g_strdup(t->name) : NULL;
		out->to_air_date = t != NULL ? g_strdup(t->air_date) : NULL;
		out->monitored = r->monitored;
		out->grab = r->grab;
		out->n_files = r->files->len;
		out->n_files_after = t != NULL ? r->files_after : r->files->len;
		out->name_season = -1;
		out->name_number = -1;
		if (r->files->len > 0) {
			const File *f = g_ptr_array_index(r->files, 0);

			out->file_path = g_strdup(f->path);
			out->identified = f->identified;
			out->name_season = f->name_season;
			out->name_number = f->name_number;
		}
		out->wanted_before = !r->gone && r->monitored && aired(r->air_date, &rules) && r->files->len == 0;
		out->wanted_after = t != NULL && r->monitored && aired(t->air_date, &rules) && r->files_after == 0;
		plan->n_by[r->how]++;
		if (t == NULL && r->files->len > 0)
			plan->n_unmapped_files++;
		/* The links a person made, in a group of their own: each follows its
		 * row as every link does, and is shown with where that is before and
		 * after (add-force-grab-and-assign). */
		for (k = 0; k < r->files->len; k++) {
			const File *f = g_ptr_array_index(r->files, k);
			AxisTvRenumberedFile *hand;

			if (f->hand == NULL)
				continue;
			hand = g_new0(AxisTvRenumberedFile, 1);
			hand->path = g_strdup(f->path);
			hand->name_season = r->gone ? -1 : r->season;
			hand->name_number = r->gone ? -1 : r->number;
			hand->to_season = t != NULL ? t->season : -1;
			hand->to_number = t != NULL ? t->number : -1;
			hand->by_name = g_strdup(f->hand);
			g_ptr_array_add(plan->hand, hand);
		}
		for (k = 0; k < r->files->len; k++) {
			const File *f = g_ptr_array_index(r->files, k);

			/* A name that gives an episode, and the episode after is another. */
			if (f->name_season >= 0 && (t == NULL || f->name_season != t->season || f->name_number != t->number))
				plan->n_disagree++;
		}
		if (out->wanted_before)
			plan->wanted_before++;
		if (out->wanted_after)
			plan->wanted_after++;
		if (r->monitored && !r->gone)
			plan->monitored_before++;
		if (t != NULL && r->monitored)
			plan->monitored_after++;
		g_ptr_array_add(plan->rows, out);
	}
	/* The rows of the target kept from before come back. */
	for (i = 0; i < read->kept->len; i++) {
		const Kept *kept = g_ptr_array_index(read->kept, i);
		gint t = find_kept_target(read->targets, kept->provider_id, kept->season, kept->number);
		AxisTvRenumbered *out;

		if (t < 0)
			continue;
		out = g_new0(AxisTvRenumbered, 1);
		out->episode_id = kept->id;
		out->season = -1;
		out->number = -1;
		out->how = AXIS_TV_MAPPED_RETURNS;
		/* (Where the target lists it now: the sync moves it there.) */
		out->to_season = ((Target *)g_ptr_array_index(read->targets, t))->season;
		out->to_number = ((Target *)g_ptr_array_index(read->targets, t))->number;
		out->to_title = g_strdup(((Target *)g_ptr_array_index(read->targets, t))->name);
		out->to_air_date = g_strdup(((Target *)g_ptr_array_index(read->targets, t))->air_date);
		out->monitored = kept->monitored;
		out->n_files = kept->n_files;
		out->n_files_after = kept->n_files + ((Target *)g_ptr_array_index(read->targets, t))->unlinked;
		out->file_path = g_strdup(kept->file_path);
		out->name_season = -1;
		out->name_number = -1;
		out->wanted_after = kept->monitored && out->n_files_after == 0 && aired(out->to_air_date, &rules);
		plan->n_by[AXIS_TV_MAPPED_RETURNS]++;
		if (out->wanted_after)
			plan->wanted_after++;
		if (kept->monitored)
			plan->monitored_after++;
		g_ptr_array_add(plan->rows, out);
	}
	/* What the change adds: the target's episodes nothing holds. */
	for (k = 0; k < read->targets->len; k++) {
		const Target *t = g_ptr_array_index(read->targets, k);
		AxisTvRenumberNew *added;

		if (t->row >= 0 || t->claimed || (t->doubled && !t->first))
			continue;
		added = g_new0(AxisTvRenumberNew, 1);
		added->season = t->season;
		added->number = t->number;
		added->title = g_strdup(t->name);
		added->air_date = g_strdup(t->air_date);
		added->monitored = season_monitored(read, t->season);
		added->aired = aired(t->air_date, &rules);
		added->gets_file = t->unlinked > 0;
		added->new_season = !g_hash_table_contains(read->season_on, GINT_TO_POINTER(t->season + 1));
		if (added->new_season)
			plan->n_new_in_new_seasons++;
		if (added->monitored && added->aired && !added->gets_file) {
			plan->n_new_wanted++;
			if (!added->new_season)
				plan->n_old_wanted++;
		}
		if (added->monitored && !added->new_season)
			plan->n_old_monitored++;
		if (added->monitored) {
			plan->monitored_after++;
			plan->n_new_monitored++;
		}
		g_ptr_array_add(plan->added, added);
	}
	plan->wanted_after += plan->n_new_wanted;
	g_ptr_array_sort(plan->added, compare_added);

	plan_names(read, plan, &rules);

	/* Why it cannot be made now. */
	if (read->targets->len == 0)
		plan->problem = g_strdup_printf("No episodes of %s are known for this series yet, so nothing can be matched. "
			"Try again once %s has answered.", axis_tv_source_label(read->to),
			axis_tv_source_is_tvdb(read->to) ? "TheTVDB" : "TMDb");
	else if (read->unfetched > 0 && read->unfetched_first == 0)
		plan->problem = g_strdup_printf("The specials of %s have not been fetched yet, so the matching would miss "
			"them. Try again in a moment.", axis_tv_source_label(read->to));
	else if (read->unfetched > 0)
		plan->problem = g_strdup_printf("The episodes of season %d of %s have not been fetched yet, so the matching "
			"would miss them. Try again in a moment.", read->unfetched_first, axis_tv_source_label(read->to));

	/* The digest: what the series and the other source were read as, and
	 * the mapping made of them (every input of it, and what it says). */
	{
		g_autoptr(GString) text = g_string_new(read->digest);

		for (i = 0; i < plan->rows->len; i++) {
			const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

			g_string_append_printf(text, "|%" G_GINT64_FORMAT ":%d:%d:%d:%s:%s:%u:%u", row->episode_id, row->how,
				row->to_season, row->to_number, row->to_title != NULL ? row->to_title : "",
				row->to_air_date != NULL ? row->to_air_date : "", row->n_files, row->n_files_after);
		}
		for (i = 0; i < plan->added->len; i++) {
			const AxisTvRenumberNew *a = g_ptr_array_index(plan->added, i);

			g_string_append_printf(text, "|+%d:%d:%d:%d", a->season, a->number, a->monitored, a->gets_file);
		}
		for (i = 0; i < plan->hand->len; i++) {
			const AxisTvRenumberedFile *h = g_ptr_array_index(plan->hand, i);

			g_string_append_printf(text, "|h%s:%d:%d:%d:%d:%s", h->path, h->name_season, h->name_number, h->to_season,
				h->to_number, h->by_name);
		}
		g_free(plan->digest);
		plan->digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, text->str, -1);
	}
	return plan;
}

/* ── Public ─────────────────────────────────────────────────────── */

const gchar *
axis_tv_mapped_label(AxisTvMapped how)
{
	static const gchar *const labels[] = {
		"No counterpart", "As before", "Air date and title", "Air date only: check it", "Title only: check it",
		"File name: check it", "Kept from before: comes back"
	};

	return (guint)how < G_N_ELEMENTS(labels) ? labels[how] : "";
}

gboolean
axis_tv_mapped_flagged(AxisTvMapped how)
{
	return how == AXIS_TV_MAPPED_DATE || how == AXIS_TV_MAPPED_TITLE || how == AXIS_TV_MAPPED_FILE_NAME;
}

void
axis_tv_renumbered_free(AxisTvRenumbered *row)
{
	if (row == NULL)
		return;
	g_free(row->title);
	g_free(row->air_date);
	g_free(row->to_title);
	g_free(row->to_air_date);
	g_free(row->file_path);
	g_free(row);
}

void
axis_tv_renumbered_file_free(AxisTvRenumberedFile *file)
{
	if (file == NULL)
		return;
	g_free(file->path);
	g_free(file->by_name);
	g_free(file);
}

void
axis_tv_renumber_new_free(AxisTvRenumberNew *added)
{
	if (added == NULL)
		return;
	g_free(added->title);
	g_free(added->air_date);
	g_free(added);
}

void
axis_tv_renumber_plan_free(AxisTvRenumberPlan *plan)
{
	if (plan == NULL)
		return;
	g_free(plan->from);
	g_free(plan->to);
	g_clear_pointer(&plan->rows, g_ptr_array_unref);
	g_clear_pointer(&plan->added, g_ptr_array_unref);
	g_clear_pointer(&plan->names, g_ptr_array_unref);
	g_clear_pointer(&plan->hand, g_ptr_array_unref);
	g_free(plan->names_left);
	g_clear_pointer(&plan->waits, g_ptr_array_unref);
	g_free(plan->problem);
	g_free(plan->notes);
	g_free(plan->digest);
	g_free(plan);
}

GPtrArray *
axis_tv_numbering_choices(
	AxisTv             *self,
	const AxisTvSeries *series
){
	GPtrArray *choices = g_ptr_array_new_with_free_func(g_free);
	const AxisMetaProviderInfo *tvdb = axis_meta_provider_find(self->meta, "tvdb");
	gboolean follows_tvdb = axis_tv_source_is_tvdb(series->episode_source);

	g_ptr_array_add(choices, g_strdup(series->episode_source));
	/* Its record provider's own numbering, always (the way back). */
	if (series->provider != NULL && g_strcmp0(series->provider, series->episode_source) != 0)
		g_ptr_array_add(choices, g_strdup(series->provider));
	/* TheTVDB's aired order, while TheTVDB can be asked and knows it. */
	if (!follows_tvdb && tvdb != NULL && series->tvdb_id != NULL && axis_meta_provider_offered(self->meta, tvdb))
		g_ptr_array_add(choices, g_strdup("tvdb"));
	return choices;
}

static gboolean
choice_valid(
	AxisTv             *self,
	const AxisTvSeries *series,
	const gchar        *to,
	GError            **error
){
	g_autoptr(GPtrArray) choices = axis_tv_numbering_choices(self, series);
	guint i;

	if (to != NULL && g_strcmp0(to, series->episode_source) == 0) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "this series follows %s already",
			axis_tv_source_label(to));
		return FALSE;
	}
	for (i = 1; to != NULL && i < choices->len; i++) {
		if (strcmp(g_ptr_array_index(choices, i), to) == 0)
			return TRUE;
	}
	g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "this series cannot follow that numbering: "
		"TheTVDB's aired order is offered while TheTVDB has a key and knows the series");
	return FALSE;
}

/* The other source's episodes, fetched when they are not fresh (outside any
 * transaction: the provider may be slow, and a failure changes nothing). */
static void
fetch_target(
	AxisTv             *self,
	const AxisTvSeries *series,
	const gchar        *to
){
	axis_tv_fetch_seasons(self, series->title_id, to, AXIS_TV_META_WAIT_MS);
}

gboolean
axis_tv_plan_numbering(
	AxisTv               *self,
	gint64                series_id,
	const gchar          *to,
	gboolean              fetch,
	AxisTvRenumberPlan  **plan,
	GError              **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	AxisDbConn *conn;
	Read read;
	gboolean ok;

	*plan = NULL;
	if (!axis_tv_get(self, series_id, &series, NULL, error) || !choice_valid(self, series, to, error))
		return FALSE;
	if (fetch)
		fetch_target(self, series, to);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* (In a transaction that is rolled back: the read looks at where the
	 * change would place files by trying it, and undoes it.) */
	memset(&read, 0, sizeof(read));
	ok = axis_db_conn_begin(conn, error) && read_all(self, conn, series_id, to, &read, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		*plan = plan_from(self, &read);
	read_clear(&read);
	return ok;
}

static const gchar *
plural(
	guint        n,
	const gchar *one,
	const gchar *many
){
	return n == 1 ? one : many;
}

/* In the caller's transaction, the series locked and its plan's digest the
 * confirmed one: the rows keyed anew, the source followed, what it lacks
 * added, one event. */
static gboolean
apply(
	AxisTv              *self,
	AxisDbConn          *conn,
	Read                *read,
	AxisTvRenumberPlan  *plan,
	gboolean             leave_new_unmonitored,
	gboolean             place_by_names,
	const gchar         *user,
	GError             **error
){
	g_autoptr(AxisDbResult) top = NULL;
	g_autoptr(AxisTvSeries) after = NULL;
	g_autofree gchar *series_id = axis_tv_int_text(read->series->id);
	g_autofree gchar *max_id = NULL;
	g_autoptr(GString) how = g_string_new(NULL);
	const gchar *params[10];
	guint matched = 0;
	guint changes = 0;
	guint linked = 0;
	guint i;

	params[0] = series_id;
	params[1] = read->series->episode_source;
	if (!axis_db_conn_exec(conn, "SELECT coalesce(max(id), 0) FROM series_episode WHERE series_id = $1::bigint", 1,
	        params, &top, error))
		return FALSE;
	max_id = get_text(top, 0, 0);
	/* Every live row first steps aside (marked gone), so that no two rows
	 * hold one place for a moment; the mapped ones come back keyed anew. */
	if (!axis_db_conn_exec(conn, "UPDATE series_episode SET gone_at = now() WHERE series_id = $1::bigint"
	        " AND source = $2 AND gone_at IS NULL", 2, params, NULL, error))
		return FALSE;
	for (i = 0; i < read->rows->len; i++) {
		const Row *r = g_ptr_array_index(read->rows, i);
		const Target *t = r->target >= 0 ? g_ptr_array_index(read->targets, r->target) : NULL;
		g_autofree gchar *id = NULL;
		g_autofree gchar *season = NULL;
		g_autofree gchar *number = NULL;
		g_autofree gchar *absolute = NULL;
		g_autofree gchar *runtime = NULL;

		if (t == NULL)
			continue;
		id = axis_tv_int_text(r->id);
		season = axis_tv_int_text(t->season);
		number = axis_tv_int_text(t->number);
		absolute = axis_tv_int_text(t->absolute);
		runtime = axis_tv_int_text(t->runtime);
		params[0] = id;
		params[1] = read->to;
		params[2] = t->provider_id;
		params[3] = season;
		params[4] = number;
		params[5] = absolute;
		params[6] = t->name;
		params[7] = t->air_date;
		params[8] = runtime;
		params[9] = series_id;
		if (!axis_db_conn_exec(conn, "UPDATE series_episode SET source = $2, provider_id = $3, season_number ="
		        " $4::integer, number = $5::integer, absolute_number = nullif($6::integer, 0), title = left($7, 500),"
		        " air_date = $8::date, runtime_minutes = nullif($9::integer, 0), gone_at = NULL"
		        " WHERE id = $1::bigint AND series_id = $10::bigint", 10, params, NULL, error))
			return FALSE;
		matched++;
	}
	params[0] = series_id;
	params[1] = read->to;
	if (!axis_db_conn_exec(conn, "UPDATE series SET episode_source = $2, updated_at = now() WHERE id = $1::bigint", 2,
	        params, NULL, error))
		return FALSE;
	/* The rows of the source it follows now: those kept from before come
	 * back, what it lacks is added, monitored as its season is. */
	if (!axis_tv_store_series_one(conn, read->series->id, &after, error) || after == NULL ||
	    !axis_tv_sync_rows(self, conn, after, NULL, TRUE, &changes, error))
		return FALSE;
	/* Only the episodes of seasons the series did not have are left unmonitored
	 * by the box; a new episode of a season it has follows that season. */
	if (leave_new_unmonitored) {
		g_autoptr(GString) seasons = g_string_new("{");
		guint n_seasons = 0;

		for (i = 0; i < plan->added->len; i++) {
			const AxisTvRenumberNew *a = g_ptr_array_index(plan->added, i);

			if (a->new_season)
				g_string_append_printf(seasons, "%s%d", n_seasons++ > 0 ? "," : "", a->season);
		}
		g_string_append_c(seasons, '}');
		params[0] = series_id;
		params[1] = max_id;
		params[2] = seasons->str;
		if (n_seasons > 0 && !axis_db_conn_exec(conn, "UPDATE series_episode SET monitored = false WHERE series_id = $1::bigint"
		        " AND id > $2::bigint AND season_number = ANY($3::integer[])", 3, params, NULL, error))
			return FALSE;
	}
	/* Every linked file stays on its row: what it holds did not change, only
	 * the numbering.  A file identification found that no link held is
	 * placed now by its name, read under the new numbering. */
	if (!axis_tv_link_unlinked(conn, read->series->id, &linked, error))
		return FALSE;
	/* Releases found under the old numbering are never grabbed under the new
	 * one: the series' stored releases go (its searches' records stay), and
	 * every grab or listing made before is refused by its stamp
	 * (0061_numbering_stamps.sql). */
	params[0] = series_id;
	if (!axis_db_conn_exec(conn, "DELETE FROM series_candidate c USING series_search s WHERE s.id = c.search_id"
	        " AND s.series_id = $1::bigint", 1, params, NULL, error))
		return FALSE;
	/* Asked for: the files whose names, under the new numbering, say another
	 * episode are placed by them (each where it can be; axis-tv-place.c). */
	if (place_by_names && !axis_tv_place_in(self, conn, after, 0, NULL, user, NULL, NULL, error))
		return FALSE;

	for (i = AXIS_TV_MAPPED_HELD; i <= AXIS_TV_MAPPED_FILE_NAME; i++) {
		static const gchar *const words[] = {
			NULL, "as before", "by air date and title", "by air date only", "by title only", "by file name"
		};

		if (plan->n_by[i] > 0)
			g_string_append_printf(how, "%s%u %s", how->len > 0 ? ", " : "", plan->n_by[i], words[i]);
	}
	return axis_tv_store_eventf(conn, after, 0, "numbering", user, error, "Episode numbering changed from %s to %s%s%s: "
		"%u %s matched%s%s%s; %u kept without a counterpart%s; %u %s back from before; %zu new%s. No file was moved, "
		"renamed or deleted.", axis_tv_source_label(plan->from), axis_tv_source_label(plan->to),
		user != NULL ? " by " : "", user != NULL ? user : "", matched, plural(matched, "episode", "episodes"),
		how->len > 0 ? " (" : "", how->str, how->len > 0 ? ")" : "", plan->n_by[AXIS_TV_MAPPED_NONE],
		plan->n_unmapped_files > 0 ? " (their files stay linked to them)" : "", plan->n_by[AXIS_TV_MAPPED_RETURNS],
		plural(plan->n_by[AXIS_TV_MAPPED_RETURNS], "comes", "come"), (gsize)plan->added->len,
		leave_new_unmonitored ? ", those of new seasons left unmonitored" : plan->n_new_wanted > 0 ? ", some wanted" : "");
}

gboolean
axis_tv_change_numbering(
	AxisTv               *self,
	gint64                series_id,
	const gchar          *to,
	const gchar          *digest,
	gboolean              leave_new_unmonitored,
	const gchar          *user,
	AxisTvRenumberPlan  **plan_out,
	gboolean             *changed,
	GError              **error
){
	return axis_tv_change_numbering_full(self, series_id, to, digest, leave_new_unmonitored, FALSE, user, plan_out,
		changed, error);
}

gboolean
axis_tv_change_numbering_full(
	AxisTv               *self,
	gint64                series_id,
	const gchar          *to,
	const gchar          *digest,
	gboolean              leave_new_unmonitored,
	gboolean              place_by_names,
	const gchar          *user,
	AxisTvRenumberPlan  **plan_out,
	gboolean             *changed,
	GError              **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	AxisDbConn *conn;
	gboolean found = FALSE;
	gboolean ok;
	Read read;

	if (plan_out != NULL)
		*plan_out = NULL;
	*changed = FALSE;
	if (!axis_tv_get(self, series_id, &series, NULL, error) || !choice_valid(self, series, to, error))
		return FALSE;
	/* The other source's episodes first, as the page did. */
	fetch_target(self, series, to);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&read, 0, sizeof(read));
	ok = axis_db_conn_begin(conn, error) && axis_tv_store_lock(conn, series_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	ok = ok && read_all(self, conn, series_id, to, &read, error);
	if (ok) {
		plan = plan_from(self, &read);
		if (plan->waits->len > 0) {
			g_autoptr(GString) names = g_string_new(NULL);
			guint i;

			for (i = 0; i < plan->waits->len; i++)
				g_string_append_printf(names, "%s%s", i == 0 ? "" : i + 1 == plan->waits->len ? " and " : ", ",
					(const gchar *)g_ptr_array_index(plan->waits, i));
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "the numbering was not changed: it waits for "
				"%s. A release fetched for one numbering would be read against the other; the change can be made "
				"once each is imported or dismissed", names->str);
			ok = FALSE;
		} else if (g_strcmp0(plan->digest, digest) != 0 || g_strcmp0(read.series->episode_source, plan->from) != 0) {
			*changed = TRUE;
		} else if (plan->problem != NULL) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "the numbering was not changed: %s",
				plan->problem);
			ok = FALSE;
		} else {
			ok = apply(self, conn, &read, plan, leave_new_unmonitored, place_by_names, user, error) &&
				axis_db_conn_commit(conn, error);
		}
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	read_clear(&read);
	if (ok && !*changed) {
		axis_tv_note_followed(self);
		g_info("series %" G_GINT64_FORMAT ": its episode numbering is now %s", series_id, to);
	}
	if (ok && plan_out != NULL)
		*plan_out = g_steal_pointer(&plan);
	return ok;
}

void
axis_tv_note_followed(AxisTv *self)
{
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT DISTINCT episode_source FROM series ORDER BY 1", 0, NULL, &rows,
	        &error)) {
		g_message("series: the sources of numbering in use could not be read: %s", error->message);
		return;
	}
	sources = g_ptr_array_new();
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(sources, (gpointer)axis_db_result_get_text(rows, i, 0));
	g_ptr_array_add(sources, NULL);
	axis_meta_set_followed(self->meta, (const gchar *const *)sources->pdata);
}

gchar *
axis_tv_numbering_not_refreshed(
	AxisTv             *self,
	const AxisTvSeries *series
){
	g_autofree gchar *owner = axis_meta_source_valid(series->episode_source) ?
		axis_meta_source_provider(series->episode_source) : NULL;
	AxisMetaProviderStatus status;
	gchar *text = NULL;
	guint i;

	/* Only a source of a secondary provider (TheTVDB) is said here: TMDb's
	 * own state is the Metadata page's. */
	for (i = 0; owner != NULL && i < axis_meta_n_providers(self->meta); i++) {
		const AxisMetaProviderInfo *info = axis_meta_provider_at(self->meta, i);
		const gchar *why = NULL;

		if (info == NULL || strcmp(info->name, owner) != 0 || !info->secondary)
			continue;
		axis_meta_provider_status(self->meta, i, &status);
		if (status.needs_key && !status.key_set)
			why = "has no key (Settings, Movie and TV details)";
		else if (status.key_refused)
			why = "refused its key (the Metadata page says more)";
		else if (status.limit.paused_until_us != 0 || status.limit.backoff_until_us != 0)
			why = "is not answering now";
		if (why != NULL)
			text = g_strdup_printf("Its numbering is not being refreshed: %s %s. Its episodes, files, searches and "
				"imports carry on as they are. To follow %s again, change the episode numbering below.",
				info->label, why, series->provider != NULL ? axis_tv_source_label(series->provider) : "TMDb");
		axis_meta_provider_status_clear(&status);
	}
	return text;
}
