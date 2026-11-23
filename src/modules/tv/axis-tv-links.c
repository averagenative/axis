/*
 * axis-tv-links.c - a series' files linked to its episodes by hand: the
 * links page's reading, the review of a plan and its change
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-force-grab-and-assign, slice C1.  An administrator says, file by file,
 * which episodes a video file of the series' folders holds: link it (to one
 * episode or several, a double episode), move its link, or unlink it.  Only
 * links change: no file on disk is moved, renamed or deleted.
 *
 * The plan is judged as one, on the series as it is after every change of
 * it, so two files can be given each other's episodes (a swap, which the
 * placing engine of axis-tv-place.c refuses by design).  An episode that is
 * given a file and holds another that is not part of the plan keeps both,
 * unless the person ticks unlinking the other.
 *
 * A link made here is stamped as made by a person (series_file.hand_how
 * "linked", hand_by, hand_at) and kept by hand (kept): identification,
 * placing by names, a change of numbering and imports leave it where it is.
 * A file left with no episode is remembered in library_hand_unlink, so that
 * identification does not link it again; that mark is the file's alone.
 * "Let Axis manage this file again" (axis_tv_links_manage()) hands the file
 * back to identification: a link of a person's is replaced, in one step, by
 * the episodes its name gives (never added to; when the name gives none it
 * stays, still marked by hand), after a review that shows the outcome.
 *
 * The review carries a digest of everything it shows; the change is made in
 * one transaction under the series' lock, only when the digest is still
 * that, no import of the series is open (axis_tv_import_open()) and no open
 * grab claims an episode it touches.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

#include "library/axis-library-hand.h"

/* ── What is read ───────────────────────────────────────────────── */

typedef struct
{
	gint64    id;
	gint      season;
	gint      number;
	gboolean  gone;
	gboolean  monitored;
	gboolean  aired;
	gchar    *grab;		/* (nullable) the release of the open grab that claims it */
	GArray   *files;	/* guint: the files on it now */
	GArray   *after;	/* guint: and after the plan */
	gboolean  touched;
} Ep;

typedef struct
{
	gint64    id;
	gchar    *path;
	gboolean  linked;	/* to this series */
	gchar    *other;	/* (nullable) the other series it is linked to */
	gchar    *hand_how;
	gchar    *hand_by;
	gchar    *hand_on;
	gboolean  kept;
	gboolean  kept_brought;	/* kept because it was brought over */
	gboolean  by_import;
	gint64    import_id;	/* with by_import: the import, while it is kept */
	gboolean  grab_open;	/* the grab of that import's download is still open */
	gboolean  by_identity;
	gboolean  unlinked_hand;
	gchar    *unlinked_by;
	gchar    *unlinked_on;
	GArray   *eps;		/* guint: the episodes it is on now */
	/* The plan. */
	gboolean  planned;	/* a person chose for it */
	GArray   *to;		/* guint: what they chose */
	GArray   *offer;	/* guint: (not planned) the episodes it would be unlinked from by its tick */
	gboolean  ticked;
	gboolean  changed;
	GArray   *after;	/* guint: (changed) its episodes after */
} LFile;

typedef struct
{
	AxisTvSeries *series;
	GPtrArray    *eps;	/* Ep */
	GPtrArray    *files;	/* LFile */
	gchar        *busy;
	guint         free_total;	/* the files with no link in the series' reach */
	/* An anime series: what reads a file's name numbered absolutely
	 * (add-anime-support, A2).  For any other series: empty. */
	AxisTvAnime   anime;
	GPtrArray    *anime_episodes;	/* (nullable) AxisTvEpisode, not gone */
	AxisTvKnown  *known;
	guint         n_known;
} Model;

void (*axis_tv_links_apply_hook)(gpointer data) = NULL;
gpointer axis_tv_links_apply_hook_data = NULL;

static void
ep_free(Ep *ep)
{
	g_free(ep->grab);
	g_clear_pointer(&ep->files, g_array_unref);
	g_clear_pointer(&ep->after, g_array_unref);
	g_free(ep);
}

static void
lfile_free(LFile *file)
{
	g_free(file->path);
	g_free(file->other);
	g_free(file->hand_how);
	g_free(file->hand_by);
	g_free(file->hand_on);
	g_free(file->unlinked_by);
	g_free(file->unlinked_on);
	g_clear_pointer(&file->eps, g_array_unref);
	g_clear_pointer(&file->to, g_array_unref);
	g_clear_pointer(&file->offer, g_array_unref);
	g_clear_pointer(&file->after, g_array_unref);
	g_free(file);
}

static void
model_clear(Model *model)
{
	axis_tv_series_free(model->series);
	g_clear_pointer(&model->eps, g_ptr_array_unref);
	g_clear_pointer(&model->files, g_ptr_array_unref);
	g_free(model->busy);
	axis_tv_anime_clear(&model->anime);
	g_clear_pointer(&model->anime_episodes, g_ptr_array_unref);
	g_free(model->known);
	memset(model, 0, sizeof(*model));
}

static gint64
get_int(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return 0;
	return value;
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

static gboolean
has(
	const GArray *list,
	guint         value
){
	guint i;

	for (i = 0; list != NULL && i < list->len; i++) {
		if (g_array_index(list, guint, i) == value)
			return TRUE;
	}
	return FALSE;
}

static GArray *
indices_new(void)
{
	return g_array_new(FALSE, FALSE, sizeof(guint));
}

static gint
compare_indices(
	gconstpointer a,
	gconstpointer b
){
	guint x = *(const guint *)a;
	guint y = *(const guint *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static gboolean
has_id(
	const GArray *list,
	gint64        value
){
	guint i;

	for (i = 0; list != NULL && i < list->len; i++) {
		if (g_array_index(list, gint64, i) == value)
			return TRUE;
	}
	return FALSE;
}

/*
 * @also: files to list when they are in the series' reach; @page: the page
 * of the files with no link to list (-1: none but @also).  The series' reach
 * is the video files linked to it, those of a folder that holds a linked
 * file of it, those identification linked to it, and those unlinked by hand
 * from it: no other file is read, whatever is asked for.
 */
static gboolean
model_read(
	AxisTv        *self,
	AxisDbConn    *conn,
	gint64         series_id,
	const GArray  *also,
	gint           page,
	Model         *model,
	GError       **error
){
	g_autoptr(AxisDbResult) eps = NULL;
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(GHashTable) index = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GString) more = g_string_new("{");
	g_autofree gchar *id = axis_tv_int_text(series_id);
	const gchar *params[2];
	AxisTvRules rules;
	guint i;

	memset(model, 0, sizeof(*model));
	if (!axis_tv_store_series_one(conn, series_id, &model->series, error))
		return FALSE;
	if (model->series == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	axis_tv_get_rules(self, &rules);
	if (model->series->anime) {
		g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);

		if (!axis_tv_store_episodes(conn, ids, FALSE, &model->anime_episodes, error) ||
		    !axis_tv_anime_init(&model->anime, conn, model->series, model->anime_episodes, rules.today, error)) {
			model_clear(model);
			return FALSE;
		}
		model->n_known = model->anime_episodes->len;
		model->known = g_new0(AxisTvKnown, MAX(model->n_known, 1));
		for (i = 0; i < model->n_known; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(model->anime_episodes, i);

			model->known[i].id = e->id;
			model->known[i].season = e->season;
			model->known[i].number = e->number;
			model->known[i].air_date = e->air_date;
		}
	}
	for (i = 0; also != NULL && i < also->len; i++)
		g_string_append_printf(more, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(also, gint64, i));
	g_string_append_c(more, '}');
	params[0] = id;
	params[1] = more->str;
	if (!axis_db_conn_exec(conn, "SELECT e.id, e.season_number, e.number, e.gone_at IS NOT NULL, e.monitored,"
	        " e.air_date::text, (SELECT g.release_name FROM series_grab_episode ge JOIN series_grab g ON g.id = ge.grab_id"
	        "   WHERE ge.episode_id = e.id AND ge.open ORDER BY g.id LIMIT 1)"
	        " FROM series_episode e WHERE e.series_id = $1::bigint"
	        " ORDER BY (e.gone_at IS NOT NULL), e.season_number, e.number, e.id", 1, params, &eps, error) ||
	    /* The series' linked videos; those asked for; and the videos with no
	     * link that are the series' by identification, by a person's unlink,
	     * or by lying in a folder that holds a linked file of it. */
	    !axis_db_conn_exec(conn, "WITH folders AS (SELECT DISTINCT olf.root_id, split_part(olf.rel_path, '/', 1) AS top"
	        "   FROM series_file o JOIN library_file olf ON olf.id = o.file_id WHERE o.series_id = $1::bigint"
	        "   AND position('/' in olf.rel_path) > 0)"
	        " SELECT lf.id, lf.rel_path, sf.series_id, sf.hand_how, sf.hand_by,"
	        " to_char(sf.hand_at AT TIME ZONE 'UTC', 'YYYY-MM-DD'), coalesce(sf.kept, false), sf.import_id IS NOT NULL,"
	        " sf.identity_id IS NOT NULL,"
	        " (SELECT string_agg(sfe.episode_id::text, ',' ORDER BY sfe.episode_id) FROM series_file_episode sfe"
	        "   WHERE sfe.file_id = lf.id),"
	        " hu.file_id IS NOT NULL, hu.by_user, to_char(hu.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD'),"
	        " (SELECT t.title FROM series os JOIN meta_title t ON t.id = os.title_id WHERE os.id = sf.series_id),"
	        " coalesce(sf.import_id, 0),"
	        " EXISTS (SELECT 1 FROM import ii JOIN series_grab xg ON xg.download_id = ii.download_id"
	        "   WHERE ii.id = sf.import_id AND xg.closed_at IS NULL),"
	        /* Kept because it was brought over, by no person's choice. */
	        " coalesce(sf.kept_how = '" AXIS_TV_KEPT_HOW_BROUGHT "' AND sf.hand_how = 'brought', false)"
	        " FROM library_file lf JOIN library_root r ON r.id = lf.root_id AND r.media_type = 'tv'"
	        " LEFT JOIN series_file sf ON sf.file_id = lf.id"
	        " LEFT JOIN library_hand_unlink hu ON hu.file_id = lf.id"
	        " WHERE lf.state = 'present' AND lf.kind = 'video' AND (sf.series_id = $1::bigint"
	        "   OR (((hu.module = '" AXIS_TV_MODULE_ID "' AND hu.owner_id = $1::bigint)"
	        "     OR EXISTS (SELECT 1 FROM library_identity_file lif JOIN library_identity li ON li.id = lif.identity_id"
	        "       JOIN series s ON s.title_id = li.title_id WHERE lif.file_id = lf.id AND s.id = $1::bigint"
	        "       AND li.state = 'linked')"
	        "     OR EXISTS (SELECT 1 FROM folders f WHERE f.root_id = lf.root_id"
	        "       AND f.top = split_part(lf.rel_path, '/', 1)))"
	        "     AND (sf.file_id IS NULL OR lf.id = ANY ($2::bigint[]))))"
	        " ORDER BY lf.rel_path, lf.id", 2, params, &files, error) ||
	    !axis_tv_import_open(conn, series_id, &model->busy, error)) {
		model_clear(model);
		return FALSE;
	}
	model->eps = g_ptr_array_new_with_free_func((GDestroyNotify)ep_free);
	model->files = g_ptr_array_new_with_free_func((GDestroyNotify)lfile_free);
	for (i = 0; i < axis_db_result_get_n_rows(eps); i++) {
		Ep *ep = g_new0(Ep, 1);
		gint64 *key = g_new(gint64, 1);
		gint64 start;

		ep->id = get_int(eps, i, 0);
		ep->season = (gint)get_int(eps, i, 1);
		ep->number = (gint)get_int(eps, i, 2);
		ep->gone = get_bool(eps, i, 3);
		ep->monitored = get_bool(eps, i, 4);
		start = axis_tv_date_us(axis_db_result_get_text(eps, i, 5));
		ep->aired = start > 0 && start + rules.air_delay_s * G_USEC_PER_SEC <= rules.now_us;
		ep->grab = g_strdup(axis_db_result_get_text(eps, i, 6));
		ep->files = indices_new();
		ep->after = indices_new();
		*key = ep->id;
		g_hash_table_insert(index, key, GUINT_TO_POINTER(i + 1));
		g_ptr_array_add(model->eps, ep);
	}
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		LFile *file = g_new0(LFile, 1);
		gint64 of = get_int(files, i, 2);
		const gchar *on = axis_db_result_get_text(files, i, 9);
		guint at = model->files->len;

		if (of == 0) {
			/* A file with no link: the page asked for, or one asked for. */
			guint seen = model->free_total++;

			if (!has_id(also, get_int(files, i, 0)) &&
			    (page < 0 || seen < (guint)page * AXIS_TV_LINK_PAGE || seen >= ((guint)page + 1) * AXIS_TV_LINK_PAGE)) {
				g_free(file);
				continue;
			}
		}
		file->id = get_int(files, i, 0);
		file->path = g_strdup(axis_db_result_get_text(files, i, 1));
		file->linked = of == series_id;
		file->eps = indices_new();
		if (of > 0 && !file->linked) {
			const gchar *title = axis_db_result_get_text(files, i, 13);

			file->other = g_strdup(title != NULL ? title : "another series");
		}
		if (file->linked) {
			g_auto(GStrv) ids = on != NULL ? g_strsplit(on, ",", -1) : NULL;
			guint k;

			file->hand_how = g_strdup(axis_db_result_get_text(files, i, 3));
			file->hand_by = g_strdup(axis_db_result_get_text(files, i, 4));
			file->hand_on = g_strdup(axis_db_result_get_text(files, i, 5));
			file->kept = get_bool(files, i, 6);
			file->kept_brought = file->kept && get_bool(files, i, 16);
			file->by_import = get_bool(files, i, 7);
			file->import_id = get_int(files, i, 14);
			file->grab_open = get_bool(files, i, 15);
			file->by_identity = get_bool(files, i, 8);
			for (k = 0; ids != NULL && ids[k] != NULL; k++) {
				gint64 episode = g_ascii_strtoll(ids[k], NULL, 10);
				guint found = GPOINTER_TO_UINT(g_hash_table_lookup(index, &episode));

				if (found == 0)
					continue;
				found--;
				g_array_append_val(file->eps, found);
				g_array_append_val(((Ep *)g_ptr_array_index(model->eps, found))->files, at);
			}
			g_array_sort(file->eps, compare_indices);
		} else if (file->other == NULL) {
			file->unlinked_hand = get_bool(files, i, 10);
			file->unlinked_by = g_strdup(axis_db_result_get_text(files, i, 11));
			file->unlinked_on = g_strdup(axis_db_result_get_text(files, i, 12));
		}
		g_ptr_array_add(model->files, file);
	}
	return TRUE;
}

/* ── Words ──────────────────────────────────────────────────────── */

static gchar *
ep_label(const Ep *ep)
{
	g_autofree gchar *label = axis_tv_episode_label(ep->season, ep->number);

	return ep->gone ? g_strdup_printf("%s (not in the numbering)", label) : g_steal_pointer(&label);
}

static gchar *
eps_text(
	const Model  *model,
	const GArray *eps
){
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; eps != NULL && i < eps->len; i++) {
		g_autofree gchar *label = ep_label(g_ptr_array_index(model->eps, g_array_index(eps, guint, i)));

		g_string_append_printf(text, "%s%s", i > 0 ? ", " : "", label);
	}
	if (text->len == 0)
		g_string_append(text, "No episode");
	return g_string_free(text, FALSE);
}

static gchar *
files_text(
	const Model  *model,
	const GArray *files
){
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; files != NULL && i < files->len; i++)
		g_string_append_printf(text, "%s%s", i > 0 ? "; " : "",
			((const LFile *)g_ptr_array_index(model->files, g_array_index(files, guint, i)))->path);
	if (text->len == 0)
		g_string_append(text, "No file");
	return g_string_free(text, FALSE);
}

static gchar *
ids_text(
	const Model  *model,
	const GArray *list,
	gboolean      of_files
){
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; list != NULL && i < list->len; i++) {
		guint at = g_array_index(list, guint, i);

		g_string_append_printf(text, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", of_files ?
			((const LFile *)g_ptr_array_index(model->files, at))->id : ((const Ep *)g_ptr_array_index(model->eps, at))->id);
	}
	return g_string_free(text, FALSE);
}

static gchar *
how_text(const LFile *file)
{
	AxisLibraryHandHow how;

	memset(&how, 0, sizeof(how));
	how.other = file->other;
	how.linked = file->linked;
	how.unlinked_hand = file->unlinked_hand;
	how.unlinked_by = file->unlinked_by;
	how.unlinked_on = file->unlinked_on;
	how.hand_how = file->hand_how;
	/* A series' brought link came from Sonarr. */
	how.brought_from = "Sonarr";
	how.hand_by = file->hand_by;
	how.hand_on = file->hand_on;
	how.by_import = file->by_import;
	how.by_identity = file->by_identity;
	how.kept = file->kept;
	how.kept_brought = file->kept_brought;
	return axis_library_hand_how(&how);
}

void
axis_tv_link_file_free(AxisTvLinkFile *file)
{
	if (file == NULL)
		return;
	g_free(file->path);
	g_free(file->on);
	g_free(file->how);
	g_free(file->other);
	g_free(file->name_says);
	g_free(file);
}

AxisTvLinkAsk *
axis_tv_link_ask_new(
	gint64       file_id,
	gboolean     unlink,
	const gchar *to
){
	AxisTvLinkAsk *ask = g_new0(AxisTvLinkAsk, 1);

	ask->file_id = file_id;
	ask->unlink = unlink;
	ask->to = g_strdup(to);
	return ask;
}

void
axis_tv_link_ask_free(AxisTvLinkAsk *ask)
{
	if (ask == NULL)
		return;
	g_free(ask->to);
	g_free(ask);
}

static void
change_free(AxisTvLinkChange *change)
{
	g_free(change->path);
	g_free(change->before);
	g_free(change->after);
	g_free(change->note);
	g_free(change);
}

static void
touched_free(AxisTvLinkTouched *touched)
{
	g_free(touched->label);
	g_free(touched->before);
	g_free(touched->after);
	g_free(touched->what);
	g_free(touched);
}

static void
offer_free(AxisTvLinkOffer *offer)
{
	g_free(offer->path);
	g_free(offer->episodes);
	g_free(offer->left);
	g_free(offer);
}

void
axis_tv_links_free(AxisTvLinks *links)
{
	if (links == NULL)
		return;
	axis_tv_series_free(links->series);
	g_clear_pointer(&links->files, g_ptr_array_unref);
	g_clear_pointer(&links->doubles, g_ptr_array_unref);
	g_free(links->busy);
	g_clear_pointer(&links->changes, g_ptr_array_unref);
	g_clear_pointer(&links->touched, g_ptr_array_unref);
	g_clear_pointer(&links->offers, g_ptr_array_unref);
	g_free(links->refused);
	g_free(links->digest);
	g_free(links);
}

/* The page's reading of @model (which gives up its series). */
static AxisTvLinks *
links_from(Model *model)
{
	AxisTvLinks *links = g_new0(AxisTvLinks, 1);
	guint i;

	links->series = g_steal_pointer(&model->series);
	links->busy = g_strdup(model->busy);
	links->free_total = model->free_total;
	links->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_file_free);
	links->doubles = g_ptr_array_new_with_free_func(g_free);
	links->changes = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
	links->touched = g_ptr_array_new_with_free_func((GDestroyNotify)touched_free);
	links->offers = g_ptr_array_new_with_free_func((GDestroyNotify)offer_free);
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);
		AxisTvLinkFile *out = g_new0(AxisTvLinkFile, 1);
		gint season = -1;
		gint number = -1;
		guint k;

		out->file_id = f->id;
		out->path = g_strdup(f->path);
		out->linked = f->linked;
		out->on = f->linked ? eps_text(model, f->eps) : NULL;
		for (k = 0; k < f->eps->len; k++)
			out->on_gone = out->on_gone || ((const Ep *)g_ptr_array_index(model->eps, g_array_index(f->eps, guint, k)))->gone;
		out->how = how_text(f);
		out->hand = f->hand_how != NULL || f->kept;
		out->kept = f->kept;
		out->import_id = f->linked ? f->import_id : 0;
		out->grab_open = f->linked && f->grab_open;
		out->unlinked_by_hand = f->unlinked_hand;
		out->other = g_strdup(f->other);
		axis_tv_name_episode_anime(f->path, &model->anime, model->known, model->n_known, &season, &number,
			&out->name_absolute);
		if (season >= 0)
			out->name_says = axis_tv_episode_label(season, number);
		/* A file brought over whose own name states another episode than the
		 * one Sonarr had it on (Sonarr may have named it under scene or
		 * absolute numbers): said here, once, and nowhere flagged as
		 * misplaced; nothing moves it by its name (axis-tv-brought.c). */
		if (f->linked && g_strcmp0(f->hand_how, "brought") == 0 && season >= 0 && f->eps->len > 0) {
			gboolean on_it = FALSE;

			for (k = 0; k < f->eps->len; k++) {
				const Ep *ep = g_ptr_array_index(model->eps, g_array_index(f->eps, guint, k));

				on_it = on_it || (ep->season == season && ep->number == number);
			}
			if (!on_it) {
				gchar *more = g_strdup_printf("%s. Its name states %s, another episode than the one Sonarr had it on: it "
					"stays where Sonarr had it, and Axis does not move it by its name", out->how, out->name_says);

				g_free(out->how);
				out->how = more;
			}
		}
		g_ptr_array_add(links->files, out);
	}
	for (i = 0; i < model->eps->len; i++) {
		const Ep *ep = g_ptr_array_index(model->eps, i);
		g_autofree gchar *label = NULL;
		g_autofree gchar *files = NULL;

		if (ep->files->len < 2)
			continue;
		label = ep_label(ep);
		files = files_text(model, ep->files);
		g_ptr_array_add(links->doubles, g_strdup_printf("%s has %u files linked: %s.", label, ep->files->len, files));
	}
	return links;
}

/* ── The plan ───────────────────────────────────────────────────── */

static LFile *
file_by_id(
	const Model *model,
	gint64       id,
	guint       *at
){
	guint i;

	for (i = 0; i < model->files->len; i++) {
		LFile *f = g_ptr_array_index(model->files, i);

		if (f->id == id) {
			if (at != NULL)
				*at = i;
			return f;
		}
	}
	return NULL;
}

/* "S01E03 S01E04" as episodes of the numbering the series follows. */
static gboolean
parse_to(
	const Model  *model,
	const gchar  *to,
	GArray       *out,
	GError      **error
){
	g_auto(GStrv) tokens = NULL;
	guint i;
	guint k;

	if (to == NULL)
		return TRUE;
	if (strlen(to) > 400) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "that is too long to be a list of episodes");
		return FALSE;
	}
	tokens = g_strsplit_set(to, " ,;\t\r\n", -1);
	for (i = 0; tokens[i] != NULL; i++) {
		const gchar *token = tokens[i];
		gchar *end = NULL;
		gint64 season;
		gint64 number;
		gboolean found = FALSE;

		if (token[0] == '\0')
			continue;
		if (strlen(token) > 16 || (token[0] != 's' && token[0] != 'S') || !g_ascii_isdigit(token[1]))
			goto bad;
		season = g_ascii_strtoll(token + 1, &end, 10);
		if (end == NULL || (*end != 'e' && *end != 'E') || !g_ascii_isdigit(end[1]))
			goto bad;
		number = g_ascii_strtoll(end + 1, &end, 10);
		if (end == NULL || *end != '\0' || season < 0 || season > 100000 || number < 0 || number > 100000)
			goto bad;
		for (k = 0; k < model->eps->len && !found; k++) {
			const Ep *ep = g_ptr_array_index(model->eps, k);

			if (ep->gone || ep->season != season || ep->number != number)
				continue;
			found = TRUE;
			if (!has(out, k))
				g_array_append_val(out, k);
		}
		if (!found) {
			g_autofree gchar *label = axis_tv_episode_label((gint)season, (gint)number);

			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "the series has no episode %s in the numbering it "
				"follows", label);
			return FALSE;
		}
		if (out->len > AXIS_TV_LINK_EPISODES_MAX) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "one file can be linked to at most %d episodes",
				AXIS_TV_LINK_EPISODES_MAX);
			return FALSE;
		}
		continue;
	bad:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "\"%s\" is not an episode: write each as S01E03, with a "
			"space between two", token);
		return FALSE;
	}
	g_array_sort(out, compare_indices);
	return TRUE;
}

/* What it is to @ep to be left without a file. */
static const gchar *
left_what(const Ep *ep)
{
	return ep->gone ? "Left without a file. The numbering lacks it, so it is never searched for." :
		ep->monitored && ep->aired ? "Left without a file: it is monitored and has aired, so it is wanted and searched for." :
		"Left without a file: not wanted (not monitored, or not aired), so not searched for.";
}

static gboolean
same(
	const GArray *a,
	const GArray *b
){
	guint i;

	if (a->len != b->len)
		return FALSE;
	for (i = 0; i < a->len; i++) {
		if (!has(b, g_array_index(a, guint, i)))
			return FALSE;
	}
	return TRUE;
}

/* Judges @asks with @others on @model, as one; fills the review of @links. */
static gboolean
plan(
	Model            *model,
	const GPtrArray  *asks,
	const GArray     *others,
	AxisTvLinks      *links,
	GError          **error
){
	g_autoptr(GString) digest = g_string_new("links");
	guint n = 0;
	guint i;
	guint k;

	for (i = 0; asks != NULL && i < asks->len; i++) {
		const AxisTvLinkAsk *ask = g_ptr_array_index(asks, i);
		g_autoptr(GArray) to = indices_new();
		LFile *f = file_by_id(model, ask->file_id, NULL);

		if (f == NULL) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "file %" G_GINT64_FORMAT " is not a video file of "
				"this series: one linked to it, in its folders, or linked to it by identification", ask->file_id);
			return FALSE;
		}
		if (f->other != NULL) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "%s is linked to %s: unlink it on that series' "
				"links page first", f->path, f->other);
			return FALSE;
		}
		if (f->planned) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "%s is named twice", f->path);
			return FALSE;
		}
		if (!parse_to(model, ask->to, to, error))
			return FALSE;
		if (ask->unlink && to->len > 0) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "%s cannot be both unlinked and given episodes",
				f->path);
			return FALSE;
		}
		if (ask->unlink && !f->linked) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "%s has no link to remove", f->path);
			return FALSE;
		}
		/* Nothing chosen, or what it is on already: left as it is. */
		if (!ask->unlink && (to->len == 0 || (f->linked && same(to, f->eps))))
			continue;
		f->planned = TRUE;
		f->changed = TRUE;
		f->to = g_steal_pointer(&to);
		f->after = g_array_copy(f->to);
		n++;
	}
	if (n == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "nothing was chosen: give a file its episodes, "
			"or tick Unlink");
		return FALSE;
	}
	/* A file outside the plan on an episode the plan gives a file: offered
	 * its tick; ticked, it leaves those episodes. */
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);
		guint e;

		if (!f->planned)
			continue;
		for (e = 0; e < f->to->len; e++) {
			guint at = g_array_index(f->to, guint, e);
			const Ep *ep = g_ptr_array_index(model->eps, at);

			if (has(f->eps, at))
				continue;
			for (k = 0; k < ep->files->len; k++) {
				LFile *x = g_ptr_array_index(model->files, g_array_index(ep->files, guint, k));

				if (x->planned)
					continue;
				if (x->offer == NULL)
					x->offer = indices_new();
				if (!has(x->offer, at))
					g_array_append_val(x->offer, at);
			}
		}
	}
	for (i = 0; i < model->files->len; i++) {
		LFile *x = g_ptr_array_index(model->files, i);

		if (x->offer == NULL)
			continue;
		g_array_sort(x->offer, compare_indices);
		for (k = 0; others != NULL && k < others->len && !x->ticked; k++)
			x->ticked = g_array_index(others, gint64, k) == x->id;
		if (!x->ticked)
			continue;
		x->changed = TRUE;
		x->after = indices_new();
		for (k = 0; k < x->eps->len; k++) {
			guint at = g_array_index(x->eps, guint, k);

			if (!has(x->offer, at))
				g_array_append_val(x->after, at);
		}
	}
	/* Each episode's files after, and which episodes are touched. */
	for (i = 0; i < model->eps->len; i++) {
		Ep *ep = g_ptr_array_index(model->eps, i);

		for (k = 0; k < ep->files->len; k++) {
			guint at = g_array_index(ep->files, guint, k);

			if (!((const LFile *)g_ptr_array_index(model->files, at))->changed)
				g_array_append_val(ep->after, at);
		}
	}
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);

		if (!f->changed)
			continue;
		for (k = 0; k < f->after->len; k++) {
			Ep *ep = g_ptr_array_index(model->eps, g_array_index(f->after, guint, k));

			g_array_append_val(ep->after, i);
			if (!has(f->eps, g_array_index(f->after, guint, k)))
				ep->touched = TRUE;
		}
		for (k = 0; k < f->eps->len; k++) {
			if (!has(f->after, g_array_index(f->eps, guint, k)))
				((Ep *)g_ptr_array_index(model->eps, g_array_index(f->eps, guint, k)))->touched = TRUE;
		}
		/* (What a tick would touch is touched by the plan already: an episode
		 * the plan gives a file.) */
	}
	for (i = 0; i < model->eps->len; i++)
		g_array_sort(((Ep *)g_ptr_array_index(model->eps, i))->after, compare_indices);

	/* Why not now. */
	if (model->busy != NULL) {
		links->refused = g_strdup_printf("No link can be changed now. %s", model->busy);
	} else {
		for (i = 0; i < model->eps->len && links->refused == NULL; i++) {
			const Ep *ep = g_ptr_array_index(model->eps, i);
			g_autofree gchar *label = NULL;

			if (!ep->touched || ep->grab == NULL)
				continue;
			label = ep_label(ep);
			links->refused = g_strdup_printf("A download for %s is in flight (%s). Wait for it to be imported, or dismiss "
				"it on the Downloads page (/downloads); then change the links.", label, ep->grab);
		}
	}

	/* The digest: the series and its numbering, every link of it, the plan
	 * as chosen, each tick offered, each touched episode and what "wanted"
	 * depends on, and why it cannot be applied. */
	g_string_append_printf(digest, "|%" G_GINT64_FORMAT "|%s|%s", links->series->id, links->series->episode_source,
		links->refused != NULL ? links->refused : "");
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);
		g_autofree gchar *on = ids_text(model, f->eps, FALSE);

		if (f->linked)
			g_string_append_printf(digest, "|L%" G_GINT64_FORMAT ":%s:%s:%d", f->id, on,
				f->hand_how != NULL ? f->hand_how : "", f->kept);
		if (f->planned) {
			g_autofree gchar *to = ids_text(model, f->to, FALSE);

			g_string_append_printf(digest, "|F%" G_GINT64_FORMAT ":%s:%d:%s", f->id, f->path, f->linked, to);
		}
		if (f->offer != NULL) {
			g_autofree gchar *from = ids_text(model, f->offer, FALSE);

			g_string_append_printf(digest, "|O%" G_GINT64_FORMAT ":%s:%d", f->id, from, f->ticked);
		}
		/* A tick, and what it leaves the file on. */
		if (f->ticked) {
			g_autofree gchar *left = ids_text(model, f->after, FALSE);

			g_string_append_printf(digest, "|T%" G_GINT64_FORMAT ":%s", f->id, left);
		}
	}
	for (i = 0; i < model->eps->len; i++) {
		const Ep *ep = g_ptr_array_index(model->eps, i);
		g_autofree gchar *on = NULL;

		if (!ep->touched)
			continue;
		on = ids_text(model, ep->files, TRUE);
		g_string_append_printf(digest, "|E%" G_GINT64_FORMAT ":%d:%d:%d:%s:%s", ep->id, ep->gone, ep->monitored, ep->aired,
			ep->grab != NULL ? ep->grab : "", on);
	}
	links->digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, digest->str, -1);

	/* The review. */
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);

		if (f->changed) {
			AxisTvLinkChange *change = g_new0(AxisTvLinkChange, 1);
			gint season = -1;
			gint number = -1;
			gboolean named = FALSE;

			change->file_id = f->id;
			change->path = g_strdup(f->path);
			change->before = eps_text(model, f->eps);
			change->after = eps_text(model, f->after);
			change->other = !f->planned;
			axis_tv_name_episode_anime(f->path, &model->anime, model->known, model->n_known, &season, &number, NULL);
			for (k = 0; k < f->after->len && !named; k++) {
				const Ep *ep = g_ptr_array_index(model->eps, g_array_index(f->after, guint, k));

				named = ep->season == season && ep->number == number;
			}
			if (!f->planned) {
				change->note = g_strdup(f->after->len > 0 ? "You ticked this file: it leaves the episodes named in the "
					"tick, and only those. It is then on its other episodes, kept by hand, so that nothing adds the removed "
					"episode again." :
					"You ticked this file: it has no episode left, so it is unlinked and left alone by identification.");
			} else if (season >= 0 && f->after->len > 0 && !named) {
				g_autofree gchar *says = axis_tv_episode_label(season, number);

				change->note = g_strdup_printf("Its name still says %s. A media server reads the name, so it may show "
					"this file as %s.", says, says);
			}
			g_ptr_array_add(links->changes, change);
		}
		if (f->offer != NULL) {
			AxisTvLinkOffer *offer = g_new0(AxisTvLinkOffer, 1);

			offer->file_id = f->id;
			offer->path = g_strdup(f->path);
			offer->episodes = eps_text(model, f->offer);
			{
				g_autoptr(GArray) left = indices_new();

				for (k = 0; k < f->eps->len; k++) {
					guint at = g_array_index(f->eps, guint, k);

					if (!has(f->offer, at))
						g_array_append_val(left, at);
				}
				offer->left = eps_text(model, left);
			}
			offer->ticked = f->ticked;
			g_ptr_array_add(links->offers, offer);
		}
	}
	for (i = 0; i < model->eps->len; i++) {
		const Ep *ep = g_ptr_array_index(model->eps, i);
		AxisTvLinkTouched *touched;

		if (!ep->touched)
			continue;
		touched = g_new0(AxisTvLinkTouched, 1);
		touched->episode_id = ep->id;
		touched->label = ep_label(ep);
		touched->before = files_text(model, ep->files);
		touched->after = files_text(model, ep->after);
		if (ep->after->len == 0 && ep->files->len > 0)
			touched->what = g_strdup(left_what(ep));
		else if (ep->after->len > 1 && ep->after->len > ep->files->len)
			touched->what = g_strdup_printf("%s has a file already. After this it has %u files linked: nothing is "
				"replaced, and nothing is unlinked unless you tick it below.", touched->label, ep->after->len);
		else if (ep->after->len > 1)
			touched->what = g_strdup_printf("After this it has %u files linked.", ep->after->len);
		g_ptr_array_add(links->touched, touched);
	}
	return TRUE;
}

/* ── Public ─────────────────────────────────────────────────────── */

gboolean
axis_tv_links_read(
	AxisTv        *self,
	gint64         series_id,
	const GArray  *also,
	guint          page,
	AxisTvLinks  **links,
	GError       **error
){
	AxisDbConn *conn;
	Model model;
	gboolean ok;

	g_return_val_if_fail(self != NULL && links != NULL, FALSE);

	*links = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = model_read(self, conn, series_id, also, (gint)MIN(page, 1000000u), &model, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*links = links_from(&model);
	(*links)->page = MIN(page, 1000000u);
	model_clear(&model);
	return TRUE;
}

/* The files a plan names, for the reading to hold them wherever they are. */
static GArray *
asked_files(const GPtrArray *asks)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint i;

	for (i = 0; asks != NULL && i < asks->len; i++)
		g_array_append_val(ids, ((const AxisTvLinkAsk *)g_ptr_array_index(asks, i))->file_id);
	return ids;
}

gboolean
axis_tv_links_review(
	AxisTv           *self,
	gint64            series_id,
	const GPtrArray  *asks,
	const GArray     *others,
	AxisTvLinks     **links,
	GError          **error
){
	g_autoptr(GArray) also = asked_files(asks);
	g_autoptr(AxisTvLinks) out = NULL;
	AxisDbConn *conn;
	Model model;
	gboolean ok;

	g_return_val_if_fail(self != NULL && links != NULL, FALSE);

	*links = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = model_read(self, conn, series_id, also, -1, &model, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	out = links_from(&model);
	ok = plan(&model, asks, others, out, error);
	model_clear(&model);
	if (ok)
		*links = g_steal_pointer(&out);
	return ok;
}

static gboolean
write_file(
	AxisDbConn          *conn,
	const Model         *model,
	const AxisTvSeries  *series,
	const LFile         *f,
	const gchar         *by,
	GError             **error
){
	g_autofree gchar *file = axis_tv_int_text(f->id);
	g_autofree gchar *owner = axis_tv_int_text(series->id);
	g_autofree gchar *ids = NULL;
	g_autofree gchar *array = NULL;
	const gchar *params[4];
	guint k;

	params[0] = file;
	params[1] = owner;
	params[2] = by;
	if (!f->planned && f->after->len > 0) {
		/* Ticked "unlink it instead", and it keeps other episodes: the episodes
		 * it leaves go, and what remains is set by hand (stamped, kept, its
		 * identity dropped), so that nothing adds the removed episode again. */
		g_autoptr(GArray) gone = indices_new();
		g_autofree gchar *list = NULL;

		for (k = 0; k < f->eps->len; k++) {
			if (!has(f->after, g_array_index(f->eps, guint, k)))
				g_array_append_val(gone, g_array_index(f->eps, guint, k));
		}
		list = ids_text(model, gone, FALSE);
		array = g_strdup_printf("{%s}", list);
		params[1] = array;
		params[3] = series->episode_source;
		if (!axis_db_conn_exec(conn, "DELETE FROM series_file_episode WHERE file_id = $1::bigint"
		        " AND episode_id = ANY ($2::bigint[])", 2, params, NULL, error))
			return FALSE;
		params[1] = owner;
		if (!axis_db_conn_exec(conn, "UPDATE series_file SET identity_id = NULL, hand_how = 'linked', hand_by = $3,"
		        " hand_at = now(), kept = true, kept_how = NULL, read_source = $4"
		        " WHERE file_id = $1::bigint AND series_id = $2::bigint", 4,
		        params, NULL, error))
			return FALSE;
	} else if (f->after->len == 0) {
		/* Unlinked: the link goes, the file stays, and the decision is
		 * remembered so that identification leaves the file alone. */
		if (!axis_db_conn_exec(conn, "DELETE FROM series_file WHERE file_id = $1::bigint AND series_id = $2::bigint", 2,
		        params, NULL, error) ||
		    !axis_library_hand_unlink_mark(conn, f->id, AXIS_TV_MODULE_ID, series->id, by, error))
			return FALSE;
	} else {
		g_autoptr(AxisDbResult) upsert = NULL;

		ids = ids_text(model, f->after, FALSE);
		array = g_strdup_printf("{%s}", ids);
		params[3] = series->episode_source;
		/* Linked by a person, and kept by hand: its quality, when the file had
		 * no link, is what identification read of it.  An import's link keeps
		 * its import (the file's history still names it); an identity's is
		 * dropped, so that removals by identity never reach the link. */
		if (!axis_db_conn_exec(conn, "INSERT INTO series_file (file_id, series_id, quality, resolution, quality_text,"
		        " linked_by, hand_how, hand_by, hand_at, kept, read_source) VALUES ($1::bigint, $2::bigint,"
		        " (SELECT lif.quality FROM library_identity_file lif WHERE lif.file_id = $1::bigint LIMIT 1),"
		        " (SELECT lif.resolution FROM library_identity_file lif WHERE lif.file_id = $1::bigint LIMIT 1),"
		        " (SELECT lif.quality_text FROM library_identity_file lif WHERE lif.file_id = $1::bigint LIMIT 1),"
		        " $3, 'linked', $3, now(), true, $4)"
		        " ON CONFLICT (file_id) DO UPDATE SET identity_id = NULL, hand_how = 'linked',"
		        " hand_by = EXCLUDED.hand_by, hand_at = now(), kept = true, kept_how = NULL,"
		        " read_source = EXCLUDED.read_source"
		        " WHERE series_file.series_id = EXCLUDED.series_id RETURNING file_id", 4, params, &upsert, error))
			return FALSE;
		/* Nothing written: the file is linked to another series now
		 * (identification linked it between the reading and here). */
		if (axis_db_result_get_n_rows(upsert) == 0) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "%s was linked to another series meanwhile, so "
				"nothing was changed", f->path);
			return FALSE;
		}
		if (!axis_db_conn_exec(conn, "DELETE FROM series_file_episode WHERE file_id = $1::bigint", 1, params, NULL,
		        error))
			return FALSE;
		params[1] = array;
		if (!axis_db_conn_exec(conn, "INSERT INTO series_file_episode (file_id, episode_id) SELECT $1::bigint, e"
		        " FROM unnest($2::bigint[]) AS e", 2, params, NULL, error) ||
		    !axis_library_hand_unlink_clear(conn, f->id, error))
			return FALSE;
	}
	/* Each episode it leaves and each it joins says so. */
	for (k = 0; k < f->eps->len; k++) {
		guint at = g_array_index(f->eps, guint, k);
		const Ep *ep = g_ptr_array_index(model->eps, at);
		g_autofree gchar *label = NULL;

		if (has(f->after, at))
			continue;
		label = ep_label(ep);
		if (!axis_tv_store_eventf(conn, series, ep->id, "hand_link", by, error, "The file %s was unlinked from %s by "
		        "hand%s%s%s. The file on disk was not touched.", f->path, label, by != NULL ? " by " : "",
		        by != NULL ? by : "", ep->after->len > 0 ? "" : ep->gone ? "; it has no file now" :
		        ep->monitored && ep->aired ? "; it has no file now and is wanted again" :
		        "; it has no file now and is not wanted"))
			return FALSE;
	}
	for (k = 0; k < f->after->len; k++) {
		guint at = g_array_index(f->after, guint, k);
		const Ep *ep = g_ptr_array_index(model->eps, at);
		g_autofree gchar *label = NULL;

		if (has(f->eps, at))
			continue;
		label = ep_label(ep);
		if (ep->after->len > 1) {
			g_autofree gchar *all = files_text(model, ep->after);

			if (!axis_tv_store_eventf(conn, series, ep->id, "hand_link", by, error, "The file %s was linked to %s by "
			        "hand%s%s. %s now has %u files linked: %s. The file on disk was not touched.", f->path, label,
			        by != NULL ? " by " : "", by != NULL ? by : "", label, ep->after->len, all))
				return FALSE;
		} else if (!axis_tv_store_eventf(conn, series, ep->id, "hand_link", by, error, "The file %s was linked to %s by "
		        "hand%s%s. The file on disk was not touched.", f->path, label, by != NULL ? " by " : "",
		        by != NULL ? by : "")) {
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * Locks the files @model changes (their rows of the index and of their
 * links, in the order of their ids), and reads who they are linked to: imports
 * and identification do not take the series' lock, so a file may have been
 * linked to another series since the reading.  *@stale: one has been.
 */
static gboolean
lock_files(
	AxisDbConn   *conn,
	const Model  *model,
	gint64        series_id,
	gboolean     *stale,
	GError      **error
){
	g_autoptr(GString) ids = g_string_new("{");
	g_autoptr(AxisDbResult) owners = NULL;
	const gchar *params[1];
	guint n = 0;
	guint i;

	*stale = FALSE;
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);

		if (!f->changed)
			continue;
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, n > 0 ? "," : "", f->id);
		n++;
	}
	g_string_append_c(ids, '}');
	params[0] = ids->str;
	if (!axis_db_conn_exec(conn, "SELECT id FROM library_file WHERE id = ANY ($1::bigint[]) ORDER BY id FOR UPDATE", 1, params,
	        NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT file_id FROM series_file WHERE file_id = ANY ($1::bigint[]) ORDER BY file_id"
	        " FOR UPDATE", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT lf.id, sf.series_id FROM library_file lf LEFT JOIN series_file sf ON sf.file_id ="
	        " lf.id WHERE lf.id = ANY ($1::bigint[])", 1, params, &owners, error))
		return FALSE;
	if (axis_db_result_get_n_rows(owners) != n)
		*stale = TRUE;
	for (i = 0; i < axis_db_result_get_n_rows(owners); i++) {
		const LFile *f = file_by_id(model, get_int(owners, i, 0), NULL);

		if (f == NULL || get_int(owners, i, 1) != (f->linked ? series_id : 0))
			*stale = TRUE;
	}
	return TRUE;
}

gboolean
axis_tv_links_apply(
	AxisTv           *self,
	gint64            series_id,
	const GPtrArray  *asks,
	const GArray     *others,
	const gchar      *digest,
	const gchar      *user,
	AxisTvLinks     **links,
	gboolean         *changed,
	GError          **error
){
	g_autoptr(GArray) also = asked_files(asks);
	g_autoptr(AxisTvLinks) out = NULL;
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	AxisDbConn *conn;
	Model model;
	gboolean found = FALSE;
	gboolean read = FALSE;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && digest != NULL, FALSE);

	if (links != NULL)
		*links = NULL;
	if (changed != NULL)
		*changed = FALSE;
	memset(&model, 0, sizeof(model));
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_tv_store_lock(conn, series_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	if (ok) {
		ok = model_read(self, conn, series_id, also, -1, &model, error);
		read = ok;
	}
	if (ok) {
		out = links_from(&model);
		ok = plan(&model, asks, others, out, error);
	}
	if (ok && g_strcmp0(out->digest, digest) != 0) {
		/* Not what the person saw: nothing is done, the review again. */
		if (changed != NULL)
			*changed = TRUE;
	} else if (ok && out->refused != NULL) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "%s", out->refused);
		ok = FALSE;
	} else if (ok) {
		gboolean stale = FALSE;

		if (axis_tv_links_apply_hook != NULL)
			axis_tv_links_apply_hook(axis_tv_links_apply_hook_data);
		ok = lock_files(conn, &model, series_id, &stale, error);
		if (ok && stale) {
			/* A file is another series' now (or gone): nothing is done, and
			 * the page is drawn again from what is there. */
			model_clear(&model);
			read = FALSE;
			g_clear_pointer(&out, axis_tv_links_free);
			ok = model_read(self, conn, series_id, also, -1, &model, error);
			read = ok;
			if (ok) {
				out = links_from(&model);
				ok = plan(&model, asks, others, out, error);
			}
			if (ok && changed != NULL) {
				*changed = TRUE;
			} else if (!ok && error != NULL && *error != NULL) {
				(*error)->code = AXIS_TV_ERROR_IN_FLIGHT;
			}
		} else if (ok) {
			for (i = 0; ok && i < model.files->len; i++) {
				const LFile *f = g_ptr_array_index(model.files, i);

				if (f->changed)
					ok = write_file(conn, &model, out->series, f, by, error);
			}
			ok = ok && axis_db_conn_commit(conn, error);
		}
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (read)
		model_clear(&model);
	if (links != NULL && out != NULL && out->digest != NULL)
		*links = g_steal_pointer(&out);
	return ok;
}

void
axis_tv_link_manage_free(AxisTvLinkManage *manage)
{
	if (manage == NULL)
		return;
	axis_tv_series_free(manage->series);
	g_free(manage->path);
	g_free(manage->before);
	g_free(manage->after);
	g_clear_pointer(&manage->touched, g_ptr_array_unref);
	g_free(manage->outcome);
	g_free(manage->refused);
	g_free(manage->digest);
	g_free(manage);
}

/*
 * What handing @file_id back does, judged on @model: fills @out; *@gives holds
 * the episodes (indices) identification gives the file by its name, for a file
 * a person linked or keeps that it places.
 */
static gboolean
manage_plan(
	AxisDbConn        *conn,
	Model             *model,
	gint64             series_id,
	gint64             file_id,
	AxisTvLinkManage  *out,
	GArray           **gives,
	gint64            *identity,
	GError           **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) digest = g_string_new("manage");
	g_autofree gchar *file = axis_tv_int_text(file_id);
	g_autofree gchar *owner = axis_tv_int_text(series_id);
	const gchar *params[2];
	LFile *f = file_by_id(model, file_id, NULL);
	guint i;
	guint k;

	*gives = indices_new();
	*identity = 0;
	if (f == NULL || f->other != NULL ||
	    !((f->linked && (f->hand_how != NULL || f->kept)) || (!f->linked && f->unlinked_hand))) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that file is not one a person linked, keeps or "
			"unlinked, in this series' reach");
		return FALSE;
	}
	out->file_id = file_id;
	out->path = g_strdup(f->path);
	out->before = eps_text(model, f->eps);
	out->touched = g_ptr_array_new_with_free_func((GDestroyNotify)touched_free);
	/* What identification gives the file by its name, for this series: the
	 * placing of identification (axis-tv-identify.c) for the one file. */
	params[0] = file;
	params[1] = owner;
	if (!axis_db_conn_exec(conn, "SELECT e.id, v.identity_id FROM library_episode_file v JOIN series s ON s.title_id ="
	        " v.title_id JOIN series_episode e ON e.series_id = s.id AND e.gone_at IS NULL AND e.season_number ="
	        " v.season_number AND e.number = v.number WHERE s.id = $2::bigint AND v.file_id = $1::bigint"
	        " ORDER BY v.identity_id, e.id", 2, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 id = get_int(rows, i, 0);

		for (k = 0; k < model->eps->len; k++) {
			if (((const Ep *)g_ptr_array_index(model->eps, k))->id == id && !has(*gives, k))
				g_array_append_val(*gives, k);
		}
		if (*identity == 0)
			*identity = get_int(rows, i, 1);
	}
	g_array_sort(*gives, compare_indices);
	if (!f->linked) {
		out->kind = AXIS_TV_MANAGE_UNLINKED;
		if ((*gives)->len > 0) {
			out->after = eps_text(model, *gives);
			out->outcome = g_strdup_printf("%s is no longer left alone. At its next run identification places it by its "
				"name, on %s.", f->path, out->after);
		} else {
			out->outcome = g_strdup_printf("%s is no longer left alone. Identification gives it no episode of this series "
				"by its name, so it stays without a link until something does.", f->path);
		}
	} else if (f->by_import) {
		out->kind = AXIS_TV_MANAGE_RELEASE;
		out->outcome = g_strdup_printf("An import linked %s, and identification never changes an import's link: it stays on "
			"%s as it is. Only the mark of a person goes, so no upgrade is held back for it.", f->path, out->before);
	} else if ((*gives)->len == 0) {
		out->kind = AXIS_TV_MANAGE_NOTHING;
		out->outcome = g_strdup_printf("Identification would give %s no episode by its name, so there is nothing to hand "
			"back: its link stays on %s as it is, still marked by hand.", f->path, out->before);
	} else {
		out->kind = AXIS_TV_MANAGE_PLACE;
		out->after = eps_text(model, *gives);
		out->outcome = g_strdup_printf("Identification places %s by its name. It is on %s now and is on %s afterwards: its "
			"links are replaced by that, not added to.", f->path, out->before, out->after);
	}
	if (out->kind == AXIS_TV_MANAGE_PLACE) {
		for (i = 0; i < model->eps->len; i++) {
			Ep *ep = g_ptr_array_index(model->eps, i);
			gboolean was = has(f->eps, i);
			gboolean will = has(*gives, i);
			AxisTvLinkTouched *touched;
			g_autoptr(GArray) rest = indices_new();

			if (was == will)
				continue;
			for (k = 0; k < ep->files->len; k++) {
				guint at = g_array_index(ep->files, guint, k);

				if (((const LFile *)g_ptr_array_index(model->files, at))->id != file_id)
					g_array_append_val(rest, at);
			}
			touched = g_new0(AxisTvLinkTouched, 1);
			touched->episode_id = ep->id;
			touched->label = ep_label(ep);
			touched->before = files_text(model, ep->files);
			if (was) {
				touched->after = rest->len > 0 ? files_text(model, rest) : g_strdup("No file");
				if (rest->len == 0)
					touched->what = g_strdup(left_what(ep));
			} else {
				touched->after = g_strdup(f->path);
				if (rest->len > 0) {
					g_autofree gchar *others = files_text(model, rest);

					g_free(touched->after);
					touched->after = g_strdup_printf("%s; %s", others, f->path);
					touched->what = g_strdup_printf("%s has a file already. After this it has %u files linked: nothing is "
						"replaced.", touched->label, rest->len + 1);
				}
			}
			g_ptr_array_add(out->touched, touched);
			if (model->busy == NULL && out->refused == NULL && ep->grab != NULL)
				out->refused = g_strdup_printf("A download for %s is in flight (%s). Wait for it to be imported, or dismiss "
					"it on the Downloads page (/downloads); then hand the file back.", touched->label, ep->grab);
		}
	}
	if (model->busy != NULL && out->kind == AXIS_TV_MANAGE_PLACE) {
		/* (Only a change of a link waits for an open import: a mark that
		 * goes changes none, and the import may be the reason to let go.) */
		g_free(out->refused);
		out->refused = g_strdup_printf("No link can be changed now. %s", model->busy);
	} else if (out->kind == AXIS_TV_MANAGE_NOTHING && out->refused == NULL) {
		out->refused = g_strdup(out->outcome);
	}
	g_string_append_printf(digest, "|%" G_GINT64_FORMAT "|%s|%" G_GINT64_FORMAT "|%d|%s|%s", series_id,
		model->series->episode_source, file_id, out->kind, out->before, out->after != NULL ? out->after : "");
	for (i = 0; i < out->touched->len; i++) {
		const AxisTvLinkTouched *touched = g_ptr_array_index(out->touched, i);

		g_string_append_printf(digest, "|E%" G_GINT64_FORMAT ":%s:%s", touched->episode_id, touched->before,
			touched->after);
	}
	g_string_append_printf(digest, "|%s", out->refused != NULL ? out->refused : "");
	out->digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, digest->str, -1);
	return TRUE;
}

gboolean
axis_tv_links_manage_review(
	AxisTv             *self,
	gint64              series_id,
	gint64              file_id,
	AxisTvLinkManage  **manage,
	GError            **error
){
	g_autoptr(GArray) also = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) gives = NULL;
	g_autoptr(AxisTvLinkManage) out = g_new0(AxisTvLinkManage, 1);
	AxisDbConn *conn;
	Model model;
	gint64 identity = 0;
	gboolean ok;

	g_return_val_if_fail(self != NULL && manage != NULL, FALSE);

	*manage = NULL;
	g_array_append_val(also, file_id);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = model_read(self, conn, series_id, also, -1, &model, error) &&
		manage_plan(conn, &model, series_id, file_id, out, &gives, &identity, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		if (model.series != NULL || model.eps != NULL)
			model_clear(&model);
		return FALSE;
	}
	out->series = g_steal_pointer(&model.series);
	model_clear(&model);
	*manage = g_steal_pointer(&out);
	return TRUE;
}

/* The history lines of a file handed back and placed. */
static gboolean
manage_events(
	AxisDbConn          *conn,
	const Model         *model,
	const AxisTvSeries  *series,
	const AxisTvLinkManage *plan,
	const gchar         *by
){
	const LFile *f = file_by_id(model, plan->file_id, NULL);
	guint i;

	for (i = 0; i < plan->touched->len; i++) {
		const AxisTvLinkTouched *touched = g_ptr_array_index(plan->touched, i);
		gboolean left = FALSE;
		guint k;

		for (k = 0; k < model->eps->len; k++) {
			if (((const Ep *)g_ptr_array_index(model->eps, k))->id == touched->episode_id)
				left = has(f->eps, k);
		}
		if (!axis_tv_store_eventf(conn, series, touched->episode_id, "hand_link", by, NULL, "The file %s was %s %s: Axis "
		        "manages it again and places it by its name%s%s. The file on disk was not touched.%s%s", f->path,
		        left ? "taken off" : "put on", touched->label, by != NULL ? ", let go by " : "", by != NULL ? by : "",
		        touched->what != NULL ? " " : "", touched->what != NULL ? touched->what : ""))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_tv_links_manage(
	AxisTv             *self,
	gint64              series_id,
	gint64              file_id,
	const gchar        *digest,
	const gchar        *user,
	AxisTvLinkManage  **manage,
	gboolean           *changed,
	GError            **error
){
	g_autoptr(GArray) also = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) gives = NULL;
	g_autoptr(AxisTvLinkManage) out = g_new0(AxisTvLinkManage, 1);
	g_autofree gchar *file = axis_tv_int_text(file_id);
	g_autofree gchar *owner = axis_tv_int_text(series_id);
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	const gchar *params[4];
	AxisDbConn *conn;
	Model model;
	gint64 identity = 0;
	gboolean found = FALSE;
	gboolean read = FALSE;
	gboolean ok;

	g_return_val_if_fail(self != NULL && digest != NULL, FALSE);

	if (manage != NULL)
		*manage = NULL;
	if (changed != NULL)
		*changed = FALSE;
	memset(&model, 0, sizeof(model));
	g_array_append_val(also, file_id);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = file;
	params[1] = owner;
	ok = axis_db_conn_begin(conn, error) && axis_tv_store_lock(conn, series_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	/* The file's rows are locked before it is read: imports and identification
	 * do not take the series' lock. */
	ok = ok && axis_db_conn_exec(conn, "SELECT id FROM library_file WHERE id = $1::bigint FOR UPDATE", 1, params, NULL,
		error) &&
		axis_db_conn_exec(conn, "SELECT file_id FROM series_file WHERE file_id = $1::bigint FOR UPDATE", 1, params, NULL,
			error);
	if (ok) {
		ok = model_read(self, conn, series_id, also, -1, &model, error);
		read = ok;
	}
	ok = ok && manage_plan(conn, &model, series_id, file_id, out, &gives, &identity, error);
	if (ok && g_strcmp0(out->digest, digest) != 0) {
		/* Not what the person saw: nothing is done, the review again. */
		if (changed != NULL)
			*changed = TRUE;
	} else if (ok && out->refused != NULL) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "%s", out->refused);
		ok = FALSE;
	} else if (ok) {
		g_autofree gchar *identity_text = axis_tv_int_text(identity);
		g_autofree gchar *ids = ids_text(&model, gives, FALSE);
		g_autofree gchar *array = g_strdup_printf("{%s}", ids);

		switch (out->kind) {
		case AXIS_TV_MANAGE_PLACE:
			/* The episodes the name gives replace the ones it is on, in this
			 * step, and the link is identification's again. */
			params[2] = identity_text;
			params[3] = model.series->episode_source;
			ok = axis_db_conn_exec(conn, "DELETE FROM series_file_episode WHERE file_id = $1::bigint", 1, params, NULL,
				error);
			params[1] = array;
			ok = ok && axis_db_conn_exec(conn, "INSERT INTO series_file_episode (file_id, episode_id) SELECT $1::bigint, e"
				" FROM unnest($2::bigint[]) AS e", 2, params, NULL, error);
			params[1] = owner;
			/* A file brought over keeps its stamp and no identity, as "Let Axis
			 * upgrade it again" leaves it: placed by name now, not kept, and a
			 * run does not keep it again (it would an unstamped link). */
			ok = ok && axis_db_conn_exec(conn, "UPDATE series_file SET"
				" identity_id = CASE WHEN hand_how = 'brought' THEN NULL ELSE $3::bigint END,"
				" hand_by = CASE WHEN hand_how = 'brought' THEN hand_by END,"
				" hand_at = CASE WHEN hand_how = 'brought' THEN hand_at END,"
				" hand_how = CASE WHEN hand_how = 'brought' THEN hand_how END,"
				" kept = false, kept_how = NULL, read_source = $4"
				" WHERE file_id = $1::bigint AND"
				" series_id = $2::bigint", 4, params, NULL, error) &&
				manage_events(conn, &model, model.series, out, by);
			break;
		case AXIS_TV_MANAGE_RELEASE:
			/* A file brought over keeps its stamp (hand_how 'brought', its
			 * hand_by and hand_at), as "Let Axis upgrade it again" leaves it:
			 * the person's choice sticks, because a run sees a stamped link
			 * and never keeps it again (an identification's own link, with no
			 * stamp, it would). */
			ok = axis_db_conn_exec(conn, "UPDATE series_file SET hand_how = CASE WHEN hand_how = 'brought' THEN hand_how END,"
				" hand_by = CASE WHEN hand_how = 'brought' THEN hand_by END,"
				" hand_at = CASE WHEN hand_how = 'brought' THEN hand_at END,"
				" kept = false, kept_how = NULL WHERE file_id = $1::bigint AND series_id = $2::bigint", 2, params, NULL,
				error);
			break;
		default:
			/* The mark is the file's: whichever series it was unlinked from. */
			ok = axis_library_hand_unlink_clear(conn, file_id, error);
			break;
		}
		ok = ok && axis_tv_store_eventf(conn, model.series, 0, "hand_link", by, error, "Axis manages the file %s again%s%s: "
			"identification, placing by names, upgrades and imports treat it by the ordinary rules. %s The file on disk "
			"was not touched.", out->path, by != NULL ? ", let go by " : "", by != NULL ? by : "", out->outcome) &&
			axis_db_conn_commit(conn, error);
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (read) {
		if (out->path != NULL && out->digest != NULL)
			out->series = g_steal_pointer(&model.series);
		model_clear(&model);
	}
	if (manage != NULL && out->digest != NULL)
		*manage = g_steal_pointer(&out);
	return ok;
}
