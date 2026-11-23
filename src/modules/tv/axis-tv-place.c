/*
 * axis-tv-place.c - a series' files whose names say another episode than
 * the one they are on, and placing them by their names
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-metadata-tvdb (design.md, open question 7, built on the first
 * switch of a real series): a file imported, or identified, under one numbering from a name
 * written in another stays on its episode through a change of numbering, as
 * a file's content does not change.  When its name is right and its episode
 * wrong, a person places it by its name: only the link moves (never the
 * file on disk), both episodes' history says so, and the link is marked as
 * read under the numbering the series follows (series_file.read_source).
 *
 * The reading is the confirmation page's: a file linked to one episode alone,
 * whose name (identification's reading of it, else the release parser's)
 * gives one episode.  Never by itself, never over another file.
 *
 * Two kinds of file are told apart.  Axis names a file it imports by its
 * episode's number at import time, so after a change of numbering (or after
 * the provider renumbers an episode) such a name is probably just the old
 * number, and the episode the file is on is right.  A file an import by Axis
 * linked, or whose name was read under another numbering than the series
 * follows now (series_file.read_source), is "earlier": it is listed apart and
 * is placed only one file at a time, on a page of its own; never by "Place
 * all", never by the numbering confirmation page's tick box.  The files
 * identification linked and read under the numbering followed now are the
 * ordinary ones.
 *
 * One engine (axis_tv_place_run()) serves the plan a page draws and the
 * placing itself: files go one after the other, each judged against what the
 * ones before it left.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

void
axis_tv_misplaced_free(AxisTvMisplaced *misplaced)
{
	if (misplaced == NULL)
		return;
	g_free(misplaced->path);
	g_free(misplaced->refused);
	g_free(misplaced->all_refused);
	g_free(misplaced);
}

void
axis_tv_place_file_clear(AxisTvPlaceFile *file)
{
	g_free(file->path);
	g_free(file->refused);
	g_free(file->all_refused);
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

/* ── The engine ─────────────────────────────────────────────────── */

void
axis_tv_place_run(
	GArray       *eps,
	GArray       *files,
	const gchar  *busy
){
	gboolean progress;
	guint i;
	guint k;

	for (i = 0; i < eps->len; i++) {
		AxisTvPlaceEp *ep = &g_array_index(eps, AxisTvPlaceEp, i);

		ep->before = ep->files;
	}
	for (i = 0; i < files->len; i++) {
		AxisTvPlaceFile *f = &g_array_index(files, AxisTvPlaceFile, i);

		if (!f->selected)
			continue;
		g_clear_pointer(&f->refused, g_free);
		f->placed = FALSE;
		f->leaves_wanted = FALSE;
		f->to = -1;
		for (k = 0; k < eps->len; k++) {
			const AxisTvPlaceEp *ep = &g_array_index(eps, AxisTvPlaceEp, k);

			if (ep->season == f->name_season && ep->number == f->name_number) {
				f->to = (gint)k;
				break;
			}
		}
	}
	/* In rounds: a file goes when its episode holds no file, so that one
	 * whose episode a file leaves in the same run goes after it (a chain
	 * resolves from its end); among those free at once, the lower id goes
	 * first.  What no round could place says why after the last. */
	do {
		progress = FALSE;
		for (i = 0; busy == NULL && i < files->len; i++) {
			AxisTvPlaceFile *f = &g_array_index(files, AxisTvPlaceFile, i);
			AxisTvPlaceEp *from;
			AxisTvPlaceEp *to;

			if (!f->selected || f->placed || f->to < 0)
				continue;
			from = &g_array_index(eps, AxisTvPlaceEp, f->from);
			to = &g_array_index(eps, AxisTvPlaceEp, (guint)f->to);
			if (to->files > 0 || from->grab || to->grab)
				continue;
			from->files--;
			to->files++;
			to->file = f->path;
			f->placed = TRUE;
			progress = TRUE;
		}
	} while (progress);
	for (i = 0; i < files->len; i++) {
		AxisTvPlaceFile *f = &g_array_index(files, AxisTvPlaceFile, i);
		const AxisTvPlaceEp *to;
		g_autofree gchar *said = NULL;

		if (!f->selected || f->placed)
			continue;
		to = f->to >= 0 ? &g_array_index(eps, AxisTvPlaceEp, (guint)f->to) : NULL;
		said = axis_tv_episode_label(f->name_season, f->name_number);
		if (busy != NULL)
			f->refused = g_strdup(busy);
		else if (to == NULL)
			f->refused = g_strdup_printf("The series has no episode %s in the numbering it follows.", said);
		else if (to->files > 0)
			f->refused = to->file != NULL ?
				g_strdup_printf("%s has a file of its own (%s): nothing is replaced.", said, to->file) :
				g_strdup_printf("%s has a file of its own: nothing is replaced.", said);
		else
			f->refused = g_strdup("A download for one of the two episodes is in flight: place it once that is imported "
				"or dismissed.");
	}
	for (i = 0; i < files->len; i++) {
		AxisTvPlaceFile *f = &g_array_index(files, AxisTvPlaceFile, i);
		const AxisTvPlaceEp *from;

		if (!f->selected || !f->placed)
			continue;
		from = &g_array_index(eps, AxisTvPlaceEp, f->from);
		f->leaves_wanted = from->files == 0 && from->monitored && from->aired;
	}
}

gchar *
axis_tv_place_left(const GArray *eps)
{
	g_autoptr(GString) items = g_string_new(NULL);
	guint n = 0;
	guint i;

	for (i = 0; i < eps->len; i++) {
		const AxisTvPlaceEp *ep = &g_array_index(eps, AxisTvPlaceEp, i);
		g_autofree gchar *label = NULL;

		if (ep->before == 0 || ep->files > 0)
			continue;
		label = axis_tv_episode_label(ep->season, ep->number);
		g_string_append_printf(items, "%s%s (%s)", n > 0 ? ", " : "", label,
			ep->monitored && ep->aired ? "wanted, searched for at once" : "not wanted");
		n++;
	}
	if (n == 0)
		return NULL;
	return g_strdup_printf("Placing all of these leaves %u %s without a file: %s.", n, n == 1 ? "episode" : "episodes",
		items->str);
}

gchar *
axis_tv_place_digest(
	const GArray *eps,
	const GArray *files
){
	g_autoptr(GString) text = g_string_new("place");
	guint i;
	guint k;

	for (i = 0; i < files->len; i++) {
		const AxisTvPlaceFile *f = &g_array_index(files, AxisTvPlaceFile, i);

		if (!f->selected)
			continue;
		g_string_append_printf(text, "|%" G_GINT64_FORMAT ":%u:%" G_GINT64_FORMAT ":%d", f->file_id, f->from,
			f->to >= 0 ? g_array_index(eps, AxisTvPlaceEp, (guint)f->to).id : 0, f->placed);
		if (f->refused != NULL)
			g_string_append(text, f->refused);
		/* What "left without a file, wanted" depends on, of both episodes. */
		for (k = 0; k < 2; k++) {
			const AxisTvPlaceEp *ep;

			if (k == 1 && f->to < 0)
				break;
			ep = &g_array_index(eps, AxisTvPlaceEp, k == 0 ? f->from : (guint)f->to);
			g_string_append_printf(text, "/%" G_GINT64_FORMAT ",%d,%d,%d,%u,%u", ep->id, ep->monitored, ep->aired,
				ep->grab, ep->before, ep->files);
		}
	}
	return g_compute_checksum_for_string(G_CHECKSUM_MD5, text->str, -1);
}

static GArray *
eps_copy(const GArray *eps)
{
	GArray *copy = g_array_sized_new(FALSE, FALSE, sizeof(AxisTvPlaceEp), eps->len);

	g_array_append_vals(copy, eps->data, eps->len);
	return copy;
}

/* ── What is read ───────────────────────────────────────────────── */

typedef struct
{
	GArray *eps;	/* AxisTvPlaceEp; .file owned here */
	GArray *files;	/* AxisTvPlaceFile */
	gchar  *busy;	/* (nullable) a manual import of the series is running */
} Model;

static void
model_clear(Model *model)
{
	guint i;

	if (model->eps != NULL) {
		for (i = 0; i < model->eps->len; i++)
			g_free((gchar *)g_array_index(model->eps, AxisTvPlaceEp, i).file);
		g_array_unref(model->eps);
	}
	g_clear_pointer(&model->files, g_array_unref);
	g_free(model->busy);
}

gboolean
axis_tv_import_open(
	AxisDbConn   *conn,
	gint64        series_id,
	gchar       **busy,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_tv_int_text(series_id);
	const gchar *params[1];
	const gchar *state;
	const gchar *reason;

	*busy = NULL;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT i.id, i.state, coalesce(i.reason_text, '') FROM import i"
	        " JOIN series s ON s.title_id = i.title_id WHERE s.id = $1::bigint AND i.module = '" AXIS_TV_MODULE_ID "'"
	        " AND i.state IN " AXIS_TV_IMPORT_OPEN " ORDER BY (i.grab_id IS NULL) DESC, i.id LIMIT 1", 1, params, &result,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	state = axis_db_result_get_text(result, 0, 1);
	reason = axis_db_result_get_text(result, 0, 2);
	if (g_strcmp0(state, "partial") == 0) {
		*busy = g_strdup_printf("An import of this series (open /imports/%" G_GINT64_FORMAT ") is partly imported: retry "
			"it or dismiss it there. Files are placed once no import is open.", get_int(result, 0, 0, 0));
		return TRUE;
	}
	*busy = g_strdup_printf("An import of this series (open /imports/%" G_GINT64_FORMAT " to let it finish or dismiss "
		"it) is %s%s%s: files are placed once no import is open.", get_int(result, 0, 0, 0),
		g_strcmp0(state, "running") == 0 ? "running" : g_strcmp0(state, "blocked") == 0 ?
		"waiting for a person to decide" : "waiting to go on", reason != NULL && reason[0] != '\0' ? ": " : "",
		reason != NULL ? reason : "");
	return TRUE;
}

static gboolean
model_read(
	AxisTv       *self,
	AxisDbConn   *conn,
	gint64        series_id,
	Model        *model,
	GError      **error
){
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) index = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autofree AxisTvKnown *known = NULL;
	guint n_known = 0;
	AxisTvAnime anime;
	const gchar *params[2];
	AxisTvRules rules;
	guint i;

	memset(model, 0, sizeof(*model));
	memset(&anime, 0, sizeof(anime));
	axis_tv_get_rules(self, &rules);
	/* An anime series: a file's name numbered absolutely is read through
	 * TheTVDB's numbers as they are now (add-anime-support, A2), so a file
	 * whose number is on another episode today is listed here.  For any other
	 * series nothing more is read. */
	if (!axis_tv_store_series_one(conn, series_id, &series, error))
		return FALSE;
	if (series != NULL && series->anime) {
		g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);

		if (!axis_tv_store_episodes(conn, ids, FALSE, &episodes, error) ||
		    !axis_tv_anime_init(&anime, conn, series, episodes, rules.today, error))
			return FALSE;
		n_known = episodes->len;
		known = g_new0(AxisTvKnown, MAX(n_known, 1));
		for (i = 0; i < n_known; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

			known[i].id = e->id;
			known[i].season = e->season;
			known[i].number = e->number;
			known[i].air_date = e->air_date;
		}
	}
	params[0] = id;
	params[1] = rules.today;
	if (!axis_db_conn_exec(conn, "SELECT e.id, e.season_number, e.number, e.monitored, e.air_date::text,"
	        " EXISTS (SELECT 1 FROM series_grab_episode ge WHERE ge.episode_id = e.id AND ge.open),"
	        " (SELECT count(*) FROM series_file_episode f JOIN library_file lf ON lf.id = f.file_id"
	        "   AND lf.state = 'present' WHERE f.episode_id = e.id),"
	        " (SELECT min(lf.rel_path) FROM series_file_episode f JOIN library_file lf ON lf.id = f.file_id"
	        "   AND lf.state = 'present' WHERE f.episode_id = e.id)"
	        " FROM series_episode e WHERE e.series_id = $1::bigint AND e.gone_at IS NULL"
	        " ORDER BY e.season_number, e.number, e.id", 1, params, &rows, error) ||
	    !axis_db_conn_exec(conn, "SELECT sf.file_id, lf.rel_path, e.id, sf.identity_id IS NOT NULL,"
	        " (SELECT CASE WHEN count(*) = 1 THEN min(lie.season_number) END FROM library_identity_episode lie"
	        "   WHERE lie.file_id = sf.file_id AND lie.air_date IS NULL),"
	        " (SELECT CASE WHEN count(*) = 1 THEN min(lie.number) END FROM library_identity_episode lie"
	        "   WHERE lie.file_id = sf.file_id AND lie.air_date IS NULL),"
	        " sf.import_id IS NOT NULL,"
	        " sf.read_source IS DISTINCT FROM (SELECT rs.episode_source FROM series rs WHERE rs.id = sf.series_id)"
	        " FROM series_file sf JOIN series_file_episode sfe ON sfe.file_id = sf.file_id"
	        " JOIN series_episode e ON e.id = sfe.episode_id AND e.gone_at IS NULL"
	        " JOIN library_file lf ON lf.id = sf.file_id AND lf.state = 'present'"
	        " WHERE sf.series_id = $1::bigint"
	        /* A link a person made is never listed and never moved: they put
	         * the file there knowing its name (add-force-grab-and-assign). */
	        " AND sf.hand_at IS NULL"
	        " AND (SELECT count(*) FROM series_file_episode x WHERE x.file_id = sf.file_id) = 1"
	        " ORDER BY sf.file_id", 1, params, &files, error) ||
	    !axis_tv_import_open(conn, series_id, &model->busy, error)) {
		axis_tv_anime_clear(&anime);
		return FALSE;
	}
	model->eps = g_array_sized_new(FALSE, TRUE, sizeof(AxisTvPlaceEp), axis_db_result_get_n_rows(rows));
	model->files = g_array_new(FALSE, TRUE, sizeof(AxisTvPlaceFile));
	g_array_set_clear_func(model->files, (GDestroyNotify)axis_tv_place_file_clear);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisTvPlaceEp ep;
		gint64 *key = g_new(gint64, 1);
		guint *value = g_new(guint, 1);

		memset(&ep, 0, sizeof(ep));
		ep.id = get_int(rows, i, 0, 0);
		ep.season = (gint)get_int(rows, i, 1, 0);
		ep.number = (gint)get_int(rows, i, 2, 0);
		axis_db_result_get_boolean(rows, i, 3, &ep.monitored);
		{
			gint64 start = axis_tv_date_us(axis_db_result_get_text(rows, i, 4));

			ep.aired = start > 0 && start + rules.air_delay_s * G_USEC_PER_SEC <= rules.now_us;
		}
		axis_db_result_get_boolean(rows, i, 5, &ep.grab);
		ep.files = (guint)get_int(rows, i, 6, 0);
		ep.file = g_strdup(axis_db_result_get_text(rows, i, 7));
		*key = ep.id;
		*value = i;
		g_hash_table_replace(index, key, value);
		g_array_append_val(model->eps, ep);
	}
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		AxisTvPlaceFile f;
		gint64 episode = get_int(files, i, 2, 0);
		const guint *at = g_hash_table_lookup(index, &episode);
		gboolean identified = FALSE;
		gboolean different = FALSE;

		if (at == NULL)
			continue;
		memset(&f, 0, sizeof(f));
		f.file_id = get_int(files, i, 0, 0);
		f.path = g_strdup(axis_db_result_get_text(files, i, 1));
		f.from = *at;
		f.name_season = -1;
		f.name_number = -1;
		f.to = -1;
		axis_db_result_get_boolean(files, i, 3, &identified);
		/* Identification placed it by its own reading of the name (and the
		 * folder's); an import named it itself. */
		if (identified && axis_db_result_get_text(files, i, 4) != NULL) {
			f.name_season = (gint)get_int(files, i, 4, -1);
			f.name_number = (gint)get_int(files, i, 5, -1);
		} else {
			axis_tv_name_episode_anime(f.path, &anime, known, n_known, &f.name_season, &f.name_number, NULL);
		}
		axis_db_result_get_boolean(files, i, 6, &f.by_import);
		axis_db_result_get_boolean(files, i, 7, &different);
		f.earlier = f.by_import || different;
		if (f.name_season < 0) {
			axis_tv_place_file_clear(&f);
			continue;
		}
		{
			const AxisTvPlaceEp *on = &g_array_index(model->eps, AxisTvPlaceEp, f.from);

			if (on->season == f.name_season && on->number == f.name_number) {
				axis_tv_place_file_clear(&f);
				continue;
			}
		}
		g_array_append_val(model->files, f);
	}
	axis_tv_anime_clear(&anime);
	return TRUE;
}

/* The listing: what "Place all" does (the ordinary files, one after the
 * other), then each earlier file judged on its own against the series as it
 * is. */
static void
model_judge(
	Model   *model,
	gint64   only,
	gchar  **left,
	gchar  **digest
){
	GArray *after;
	guint i;

	/* One file asked for: judged alone against the series as it is, never
	 * after the others (the page and the placing agree). */
	if (only > 0) {
		for (i = 0; i < model->files->len; i++) {
			AxisTvPlaceFile *f = &g_array_index(model->files, AxisTvPlaceFile, i);

			f->selected = f->file_id == only;
		}
		after = eps_copy(model->eps);
		axis_tv_place_run(after, model->files, model->busy);
		if (left != NULL)
			*left = axis_tv_place_left(after);
		if (digest != NULL)
			*digest = axis_tv_place_digest(after, model->files);
		g_array_unref(after);
		for (i = 0; i < model->files->len; i++) {
			AxisTvPlaceFile *f = &g_array_index(model->files, AxisTvPlaceFile, i);

			f->all_placed = f->selected && f->placed;
			f->all_leaves_wanted = f->selected && f->leaves_wanted;
			g_free(f->all_refused);
			f->all_refused = f->selected ? g_strdup(f->refused) : NULL;
		}
		return;
	}
	for (i = 0; i < model->files->len; i++) {
		AxisTvPlaceFile *f = &g_array_index(model->files, AxisTvPlaceFile, i);

		f->selected = !f->earlier;
	}
	after = eps_copy(model->eps);
	axis_tv_place_run(after, model->files, model->busy);
	if (left != NULL)
		*left = axis_tv_place_left(after);
	if (digest != NULL)
		*digest = axis_tv_place_digest(after, model->files);
	g_array_unref(after);
	/* What the run of all made of each file is kept for Place all's
	 * confirmation; then each file is judged alone against the series as it
	 * is, which is what its own row and its own button do
	 * (add-force-grab-and-assign, C1: a row never promises what only the run
	 * of all can do, nor refuses what the file alone can). */
	for (i = 0; i < model->files->len; i++) {
		AxisTvPlaceFile *f = &g_array_index(model->files, AxisTvPlaceFile, i);

		f->all_placed = f->selected && f->placed;
		f->all_leaves_wanted = f->selected && f->leaves_wanted;
		g_free(f->all_refused);
		f->all_refused = f->selected ? g_strdup(f->refused) : NULL;
	}
	for (i = 0; i < model->files->len; i++) {
		GArray *alone;
		guint k;

		for (k = 0; k < model->files->len; k++)
			g_array_index(model->files, AxisTvPlaceFile, k).selected = (k == i);
		alone = eps_copy(model->eps);
		axis_tv_place_run(alone, model->files, model->busy);
		g_array_unref(alone);
	}
}

gboolean
axis_tv_misplaced_in(
	AxisTv        *self,
	AxisDbConn    *conn,
	gint64         series_id,
	gint64         only,
	GPtrArray    **list,
	gchar        **left,
	gchar        **digest,
	GError       **error
){
	Model model;
	guint pass;
	guint i;

	*list = NULL;
	if (left != NULL)
		*left = NULL;
	if (digest != NULL)
		*digest = NULL;
	if (!model_read(self, conn, series_id, &model, error))
		return FALSE;
	model_judge(&model, only, left, digest);
	*list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_misplaced_free);
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < model.files->len; i++) {
			const AxisTvPlaceFile *f = &g_array_index(model.files, AxisTvPlaceFile, i);
			const AxisTvPlaceEp *from = &g_array_index(model.eps, AxisTvPlaceEp, f->from);
			AxisTvMisplaced *m;

			if (f->earlier != (pass == 1) || (only > 0 && f->file_id != only))
				continue;
			m = g_new0(AxisTvMisplaced, 1);
			m->file_id = f->file_id;
			m->path = g_strdup(f->path);
			m->episode_id = from->id;
			m->season = from->season;
			m->number = from->number;
			m->name_season = f->name_season;
			m->name_number = f->name_number;
			m->to_episode_id = f->to >= 0 ? g_array_index(model.eps, AxisTvPlaceEp, (guint)f->to).id : 0;
			m->refused = g_strdup(f->refused);
			m->leaves_wanted = f->leaves_wanted;
			m->all_placed = f->all_placed;
			m->all_refused = g_strdup(f->all_refused);
			m->all_leaves_wanted = f->all_leaves_wanted;
			m->earlier = f->earlier;
			m->by_import = f->by_import;
			g_ptr_array_add(*list, m);
		}
	}
	model_clear(&model);
	return TRUE;
}

gboolean
axis_tv_list_misplaced(
	AxisTv      *self,
	gint64       series_id,
	gint64       only,
	GPtrArray  **list,
	gchar      **left,
	gchar      **digest,
	GError     **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	*list = NULL;
	if (left != NULL)
		*left = NULL;
	if (digest != NULL)
		*digest = NULL;
	if (conn == NULL)
		return FALSE;
	ok = axis_tv_misplaced_in(self, conn, series_id, only, list, left, digest, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── Placing ────────────────────────────────────────────────────── */

gboolean
axis_tv_place_in(
	AxisTv              *self,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	gint64               file_id,
	const gchar         *digest,
	const gchar         *user,
	guint               *placed,
	gboolean            *changed,
	GError             **error
){
	Model model;
	GArray *after;
	gboolean found = FALSE;
	gboolean ok = FALSE;
	guint n = 0;
	guint i;

	if (placed != NULL)
		*placed = 0;
	if (changed != NULL)
		*changed = FALSE;
	if (!model_read(self, conn, series->id, &model, error))
		return FALSE;
	for (i = 0; i < model.files->len; i++) {
		AxisTvPlaceFile *f = &g_array_index(model.files, AxisTvPlaceFile, i);

		f->selected = file_id > 0 ? f->file_id == file_id : !f->earlier;
		found = found || (file_id > 0 && f->file_id == file_id);
	}
	if (file_id > 0 && !found) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that file's name says the episode it is on");
		goto out;
	}
	after = eps_copy(model.eps);
	axis_tv_place_run(after, model.files, model.busy);
	if (digest != NULL) {
		g_autofree gchar *now = axis_tv_place_digest(after, model.files);

		if (g_strcmp0(now, digest) != 0) {
			if (changed != NULL)
				*changed = TRUE;
			g_array_unref(after);
			ok = TRUE;
			goto out;
		}
	}
	g_array_unref(after);
	for (i = 0; i < model.files->len; i++) {
		const AxisTvPlaceFile *f = &g_array_index(model.files, AxisTvPlaceFile, i);
		const AxisTvPlaceEp *from = &g_array_index(model.eps, AxisTvPlaceEp, f->from);
		const AxisTvPlaceEp *to;
		g_autofree gchar *file = NULL;
		g_autofree gchar *from_id = NULL;
		g_autofree gchar *to_id = NULL;
		g_autofree gchar *was = NULL;
		g_autofree gchar *now = NULL;
		g_autofree gchar *by = NULL;
		const gchar *params[4];
		const gchar *stamp[2];

		if (!f->selected)
			continue;
		if (f->refused != NULL) {
			if (file_id > 0) {
				g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "%s", f->refused);
				goto out;
			}
			continue;
		}
		to = &g_array_index(model.eps, AxisTvPlaceEp, (guint)f->to);
		file = axis_tv_int_text(f->file_id);
		from_id = axis_tv_int_text(from->id);
		to_id = axis_tv_int_text(to->id);
		params[0] = file;
		params[1] = from_id;
		params[2] = to_id;
		params[3] = series->episode_source;
		stamp[0] = file;
		stamp[1] = series->episode_source;
		if (!axis_db_conn_exec(conn, "UPDATE series_file_episode SET episode_id = $3::bigint WHERE file_id = $1::bigint"
		        " AND episode_id = $2::bigint", 3, params, NULL, error) ||
		    !axis_db_conn_exec(conn, "UPDATE series_file SET read_source = $2 WHERE file_id = $1::bigint", 2, stamp,
		        NULL, error))
			goto out;
		was = axis_tv_episode_label(from->season, from->number);
		now = axis_tv_episode_label(f->name_season, f->name_number);
		by = user != NULL ? g_strdup_printf(" by %s", user) : g_strdup("");
		if (!axis_tv_store_eventf(conn, series, from->id, "changed", user, error, "The file %s was placed by its "
		        "name on %s%s: it is no longer on %s%s. The file on disk was not touched.", f->path, now, by, was,
		        f->leaves_wanted ? ", which is wanted again" : "") ||
		    !axis_tv_store_eventf(conn, series, to->id, "changed", user, error, "The file %s, on %s until now, "
		        "was placed by its name on %s%s. The file on disk was not touched.", f->path, was, now, by))
			goto out;
		n++;
	}
	if (file_id > 0 && n == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that file's name says the episode it is on");
		goto out;
	}
	if (placed != NULL)
		*placed = n;
	ok = TRUE;
out:
	model_clear(&model);
	return ok;
}

gboolean
axis_tv_place_by_name(
	AxisTv       *self,
	gint64        series_id,
	gint64        file_id,
	const gchar  *digest,
	const gchar  *user,
	guint        *placed,
	gboolean     *changed,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean found = FALSE;
	gboolean moved = FALSE;
	gboolean ok;

	if (placed != NULL)
		*placed = 0;
	if (changed != NULL)
		*changed = FALSE;
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_tv_store_lock(conn, series_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	ok = ok && axis_tv_store_series_one(conn, series_id, &series, error) && series != NULL &&
		axis_tv_place_in(self, conn, series, file_id, digest, user, placed, &moved, error) &&
		(moved || axis_db_conn_commit(conn, error));
	if (changed != NULL)
		*changed = moved;
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}
