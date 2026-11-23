/*
 * axis-movies-links.c - a movie's files linked to it by hand: the links
 * page's reading, the review of a plan and its change
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-force-grab-and-assign, slice C2: what axis-tv-links.c is for a series.
 * An administrator says, file by file, whether a video file of a library
 * folder for movies is this movie's: link it (a file with no link, or one
 * linked to another movie, which it then leaves) or unlink it.  Only links
 * change: no file on disk is moved, renamed or deleted.
 *
 * A movie may hold more than one linked file (movie_file is keyed by the
 * file; a movie with two is judged for upgrades by its best).  So a file
 * linked to a movie that has one already never takes its place by itself:
 * both stay linked, unless the person ticks unlinking the present file, each
 * by its own tick.
 *
 * A link made here is stamped as made by a person (movie_file.hand_how
 * "linked", hand_by, hand_at) and kept by hand (kept): identification and
 * imports leave it where it is, and no upgrade is searched for.  A file left
 * with no link is remembered in library_hand_unlink, so that identification
 * does not link it again; that mark is the file's alone.  "Let Axis manage
 * this file again" (axis_movies_links_manage()) takes the marks of a person
 * off one file, after a review that says what follows.
 *
 * The review carries a digest of everything it shows; the change is made in
 * one transaction with the movie locked, only when the digest is still that,
 * no import of the movie (or of a movie a file leaves) is open and no grab of
 * it is in flight.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "library/axis-library-hand.h"

/* The states of an import that is not over. */
#define IMPORT_OPEN "('waiting', 'running', 'blocked', 'partial')"

/* ── What is read ───────────────────────────────────────────────── */

/* Another movie a listed file is linked to. */
typedef struct
{
	gint64    id;
	gint64    title_id;
	gchar    *label;
	gboolean  monitored;
	guint     n_files;	/* its present video files */
	gchar    *grab;		/* (nullable) the release of its open grab */
	gint64    import_id;	/* its open import, or 0 */
	guint     leaving;	/* files the plan takes from it */
} Other;

typedef struct
{
	gint64    id;
	gchar    *path;
	gint64    owner;	/* the movie it is linked to, or 0 */
	gboolean  linked;	/* to this movie */
	Other    *other;	/* (nullable) (borrowed) the other movie it is linked to */
	gchar    *hand_how;
	gchar    *hand_by;
	gchar    *hand_on;
	gboolean  kept;
	gboolean  kept_brought;	/* kept because it was brought over */
	gboolean  by_import;
	gboolean  by_identity;
	gboolean  unlinked_hand;
	gchar    *unlinked_by;
	gchar    *unlinked_on;
	/* The plan. */
	gboolean  planned;	/* a person chose for it */
	gboolean  link;		/* (planned) linked to this movie; else unlinked */
	gboolean  offered;	/* (not planned) this movie's: its tick is offered */
	gboolean  ticked;
	gboolean  changed;
} LFile;

typedef struct
{
	AxisMovieFacts *movie;
	gchar          *label;
	GPtrArray      *files;	/* LFile */
	GPtrArray      *others;	/* Other */
	gchar          *busy;
	guint           free_total;
} Model;

static void
other_free(Other *other)
{
	g_free(other->label);
	g_free(other->grab);
	g_free(other);
}

static void
lfile_free(LFile *file)
{
	g_free(file->path);
	g_free(file->hand_how);
	g_free(file->hand_by);
	g_free(file->hand_on);
	g_free(file->unlinked_by);
	g_free(file->unlinked_on);
	g_free(file);
}

static void
model_clear(Model *model)
{
	axis_movie_facts_free(model->movie);
	g_free(model->label);
	g_clear_pointer(&model->files, g_ptr_array_unref);
	g_clear_pointer(&model->others, g_ptr_array_unref);
	g_free(model->busy);
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

/* Why no link of @movie_id can be changed now: an import of it is open. */
static gboolean
import_open(
	AxisDbConn   *conn,
	gint64        movie_id,
	gchar       **busy,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	const gchar *params[1];
	const gchar *state;
	const gchar *reason;

	*busy = NULL;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT i.id, i.state, coalesce(i.reason_text, '') FROM import i"
	        " WHERE i.module = '" AXIS_MOVIES_MODULE_ID "' AND i.owner_id = $1::bigint AND i.state IN " IMPORT_OPEN
	        " ORDER BY i.id LIMIT 1", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	state = axis_db_result_get_text(result, 0, 1);
	reason = axis_db_result_get_text(result, 0, 2);
	*busy = g_strdup_printf("An import of this movie (open /imports/%" G_GINT64_FORMAT " to let it finish or dismiss "
		"it) is %s%s%s: links are changed once no import is open.", get_int(result, 0, 0),
		g_strcmp0(state, "running") == 0 ? "running" : g_strcmp0(state, "blocked") == 0 ?
		"waiting for a person to decide" : g_strcmp0(state, "partial") == 0 ? "partly imported" : "waiting to go on",
		reason != NULL && reason[0] != '\0' ? ": " : "", reason != NULL ? reason : "");
	return TRUE;
}

/*
 * @also: files to list wherever they lie in a library folder for movies (a
 * file of another movie is listed only so); @page: the page of the files with
 * no link to list (-1: none but @also).  Without @also, the movie's reach is
 * the video files linked to it, those with no link in a folder that holds a
 * linked file of it, those identification holds as it, and those unlinked by
 * hand from it.
 */
static gboolean
model_read(
	AxisDbConn    *conn,
	gint64         movie_id,
	const GArray  *also,
	gint           page,
	Model         *model,
	GError       **error
){
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(GString) more = g_string_new("{");
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	const gchar *params[2];
	guint i;
	guint k;

	memset(model, 0, sizeof(*model));
	if (!axis_movies_store_facts_one(conn, movie_id, &model->movie, error))
		return FALSE;
	if (model->movie == NULL) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		return FALSE;
	}
	model->label = axis_movies_title_label(model->movie->title, model->movie->year);
	for (i = 0; also != NULL && i < also->len; i++)
		g_string_append_printf(more, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(also, gint64, i));
	g_string_append_c(more, '}');
	params[0] = id;
	params[1] = more->str;
	if (!axis_db_conn_exec(conn, "WITH folders AS (SELECT DISTINCT olf.root_id, regexp_replace(olf.rel_path, '/[^/]*$', '')"
	        "   AS dir FROM movie_file o JOIN library_file olf ON olf.id = o.file_id WHERE o.movie_id = $1::bigint"
	        "   AND position('/' in olf.rel_path) > 0)"
	        " SELECT lf.id, lf.rel_path, mf.movie_id, mf.hand_how, mf.hand_by,"
	        " to_char(mf.hand_at AT TIME ZONE 'UTC', 'YYYY-MM-DD'), coalesce(mf.kept, false), mf.import_id IS NOT NULL,"
	        " mf.identity_id IS NOT NULL, hu.file_id IS NOT NULL, hu.by_user,"
	        " to_char(hu.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD'), om.title_id, ot.title, ot.year, om.monitored,"
	        " (SELECT count(*) FROM movie_file x JOIN library_file xl ON xl.id = x.file_id WHERE x.movie_id = mf.movie_id"
	        "   AND xl.state = 'present' AND xl.kind = 'video'),"
	        " (SELECT g.release_name FROM movie_grab g WHERE g.movie_id = mf.movie_id AND g.closed_at IS NULL LIMIT 1),"
	        " (SELECT i.id FROM import i WHERE i.module = '" AXIS_MOVIES_MODULE_ID "' AND i.owner_id = mf.movie_id"
	        "   AND i.state IN " IMPORT_OPEN " ORDER BY i.id LIMIT 1),"
	        /* Kept because it was brought over, by no person's choice. */
	        " coalesce(mf.kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "' AND mf.hand_how = 'brought', false)"
	        " FROM library_file lf JOIN library_root r ON r.id = lf.root_id AND r.media_type = 'movies'"
	        " LEFT JOIN movie_file mf ON mf.file_id = lf.id"
	        " LEFT JOIN movie om ON om.id = mf.movie_id LEFT JOIN meta_title ot ON ot.id = om.title_id"
	        " LEFT JOIN library_hand_unlink hu ON hu.file_id = lf.id"
	        " WHERE lf.state = 'present' AND lf.kind = 'video' AND (mf.movie_id = $1::bigint"
	        "   OR lf.id = ANY ($2::bigint[])"
	        "   OR (mf.file_id IS NULL AND ((hu.module = '" AXIS_MOVIES_MODULE_ID "' AND hu.owner_id = $1::bigint)"
	        "     OR EXISTS (SELECT 1 FROM library_identity_file lif JOIN library_identity li ON li.id = lif.identity_id"
	        "       JOIN movie m ON m.title_id = li.title_id WHERE lif.file_id = lf.id AND m.id = $1::bigint"
	        "       AND li.state = 'linked')"
	        "     OR (position('/' in lf.rel_path) > 0 AND EXISTS (SELECT 1 FROM folders f WHERE f.root_id = lf.root_id"
	        "       AND f.dir = regexp_replace(lf.rel_path, '/[^/]*$', ''))))))"
	        " ORDER BY lf.rel_path, lf.id", 2, params, &files, error) ||
	    !import_open(conn, movie_id, &model->busy, error)) {
		model_clear(model);
		return FALSE;
	}
	model->files = g_ptr_array_new_with_free_func((GDestroyNotify)lfile_free);
	model->others = g_ptr_array_new_with_free_func((GDestroyNotify)other_free);
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		LFile *file;
		gint64 of = get_int(files, i, 2);

		if (of == 0) {
			/* A file with no link: the page asked for, or one asked for. */
			guint seen = model->free_total++;

			if (!has_id(also, get_int(files, i, 0)) &&
			    (page < 0 || seen < (guint)page * AXIS_MOVIES_LINK_PAGE ||
			     seen >= ((guint)page + 1) * AXIS_MOVIES_LINK_PAGE))
				continue;
		}
		file = g_new0(LFile, 1);
		file->id = get_int(files, i, 0);
		file->path = g_strdup(axis_db_result_get_text(files, i, 1));
		file->owner = of;
		file->linked = of == movie_id;
		if (of > 0 && !file->linked) {
			for (k = 0; k < model->others->len && file->other == NULL; k++) {
				if (((Other *)g_ptr_array_index(model->others, k))->id == of)
					file->other = g_ptr_array_index(model->others, k);
			}
			if (file->other == NULL) {
				Other *other = g_new0(Other, 1);

				other->id = of;
				other->title_id = get_int(files, i, 12);
				other->label = axis_movies_title_label(axis_db_result_get_text(files, i, 13) != NULL ?
					axis_db_result_get_text(files, i, 13) : "another movie", (gint)get_int(files, i, 14));
				other->monitored = get_bool(files, i, 15);
				other->n_files = (guint)MAX(get_int(files, i, 16), 0);
				other->grab = g_strdup(axis_db_result_get_text(files, i, 17));
				other->import_id = get_int(files, i, 18);
				g_ptr_array_add(model->others, other);
				file->other = other;
			}
		}
		if (file->linked) {
			file->hand_how = g_strdup(axis_db_result_get_text(files, i, 3));
			file->hand_by = g_strdup(axis_db_result_get_text(files, i, 4));
			file->hand_on = g_strdup(axis_db_result_get_text(files, i, 5));
			file->kept = get_bool(files, i, 6);
			file->kept_brought = file->kept && get_bool(files, i, 19);
			file->by_import = get_bool(files, i, 7);
			file->by_identity = get_bool(files, i, 8);
		} else if (file->other == NULL) {
			file->unlinked_hand = get_bool(files, i, 9);
			file->unlinked_by = g_strdup(axis_db_result_get_text(files, i, 10));
			file->unlinked_on = g_strdup(axis_db_result_get_text(files, i, 11));
		}
		g_ptr_array_add(model->files, file);
	}
	return TRUE;
}

/* ── Words ──────────────────────────────────────────────────────── */

static gchar *
how_text(const LFile *file)
{
	AxisLibraryHandHow how;

	memset(&how, 0, sizeof(how));
	how.other = file->other != NULL ? file->other->label : NULL;
	how.linked = file->linked;
	how.unlinked_hand = file->unlinked_hand;
	how.unlinked_by = file->unlinked_by;
	how.unlinked_on = file->unlinked_on;
	how.hand_how = file->hand_how;
	how.hand_by = file->hand_by;
	how.hand_on = file->hand_on;
	how.by_import = file->by_import;
	how.by_identity = file->by_identity;
	how.kept = file->kept;
	how.kept_brought = file->kept_brought;
	return axis_library_hand_how(&how);
}

/* The movie's files: now (@after FALSE), or after the plan. */
static gchar *
files_text(
	const Model *model,
	gboolean     after,
	guint       *n
){
	GString *text = g_string_new(NULL);
	guint count = 0;
	guint i;

	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);
		gboolean in = after ? (f->linked && !f->changed) || (f->planned && f->link) : f->linked;

		if (!in)
			continue;
		g_string_append_printf(text, "%s%s", count > 0 ? "; " : "", f->path);
		count++;
	}
	if (count == 0)
		g_string_append(text, "No file");
	if (n != NULL)
		*n = count;
	return g_string_free(text, FALSE);
}

/* What it is to a movie to be left without a file. */
static const gchar *
left_what(gboolean monitored)
{
	return monitored ? "Left without a file: it is monitored, so it is wanted and searched for again once it is "
		"available." : "Left without a file: it is not monitored, so it is not searched for.";
}

void
axis_movie_link_file_free(AxisMovieLinkFile *file)
{
	if (file == NULL)
		return;
	g_free(file->path);
	g_free(file->how);
	g_free(file->other);
	g_free(file);
}

static void
change_free(AxisMovieLinkChange *change)
{
	g_free(change->path);
	g_free(change->before);
	g_free(change->after);
	g_free(change->note);
	g_free(change);
}

static void
touched_free(AxisMovieLinkTouched *touched)
{
	g_free(touched->label);
	g_free(touched->before);
	g_free(touched->after);
	g_free(touched->what);
	g_free(touched);
}

static void
offer_free(AxisMovieLinkOffer *offer)
{
	g_free(offer->path);
	g_free(offer);
}

void
axis_movie_links_free(AxisMovieLinks *links)
{
	if (links == NULL)
		return;
	axis_movie_facts_free(links->movie);
	g_clear_pointer(&links->files, g_ptr_array_unref);
	g_free(links->doubles);
	g_free(links->busy);
	g_clear_pointer(&links->changes, g_ptr_array_unref);
	g_clear_pointer(&links->touched, g_ptr_array_unref);
	g_clear_pointer(&links->offers, g_ptr_array_unref);
	g_free(links->refused);
	g_free(links->digest);
	g_free(links);
}

/* The page's reading of @model (which gives up its movie's facts). */
static AxisMovieLinks *
links_from(Model *model)
{
	AxisMovieLinks *links = g_new0(AxisMovieLinks, 1);
	g_autofree gchar *all = NULL;
	guint n = 0;
	guint i;

	all = files_text(model, FALSE, &n);
	if (n > 1)
		links->doubles = g_strdup_printf("%s has %u files linked: %s.", model->label, n, all);
	links->busy = g_strdup(model->busy);
	links->free_total = model->free_total;
	links->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_link_file_free);
	links->changes = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
	links->touched = g_ptr_array_new_with_free_func((GDestroyNotify)touched_free);
	links->offers = g_ptr_array_new_with_free_func((GDestroyNotify)offer_free);
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);
		AxisMovieLinkFile *out = g_new0(AxisMovieLinkFile, 1);

		out->file_id = f->id;
		out->path = g_strdup(f->path);
		out->linked = f->linked;
		out->how = how_text(f);
		out->hand = f->hand_how != NULL || f->kept;
		out->kept = f->kept;
		out->unlinked_by_hand = f->unlinked_hand;
		out->other = f->other != NULL ? g_strdup(f->other->label) : NULL;
		g_ptr_array_add(links->files, out);
	}
	links->movie = g_steal_pointer(&model->movie);
	return links;
}

/* ── The plan ───────────────────────────────────────────────────── */

static LFile *
file_by_id(
	const Model *model,
	gint64       id
){
	guint i;

	for (i = 0; i < model->files->len; i++) {
		LFile *f = g_ptr_array_index(model->files, i);

		if (f->id == id)
			return f;
	}
	return NULL;
}

/* Judges @asks with @others on @model, as one; fills the review of @links
 * (whose movie is @movie: the model gave its facts up). */
static gboolean
plan(
	Model            *model,
	const AxisMovieFacts *movie,
	const GPtrArray  *asks,
	const GArray     *others,
	AxisMovieLinks   *links,
	GError          **error
){
	g_autoptr(GString) digest = g_string_new("movie-links");
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	guint n_before = 0;
	guint n_after = 0;
	guint n = 0;
	guint n_link = 0;
	guint i;

	for (i = 0; asks != NULL && i < asks->len; i++) {
		const AxisMovieLinkAsk *ask = g_ptr_array_index(asks, i);
		LFile *f = file_by_id(model, ask->file_id);

		if (f == NULL) {
			g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "file %" G_GINT64_FORMAT " is not a video "
				"file that is present in a library folder for movies", ask->file_id);
			return FALSE;
		}
		if (f->planned) {
			g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "%s is named twice", f->path);
			return FALSE;
		}
		if (ask->unlink && !f->linked) {
			g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "%s has no link to this movie to remove",
				f->path);
			return FALSE;
		}
		/* Linked to this movie already: left as it is. */
		if (!ask->unlink && f->linked)
			continue;
		f->planned = TRUE;
		f->link = !ask->unlink;
		f->changed = TRUE;
		if (f->link && f->other != NULL)
			f->other->leaving++;
		n_link += f->link ? 1 : 0;
		n++;
	}
	if (n == 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "nothing was chosen: tick Link to this "
			"movie for a file, or Unlink");
		return FALSE;
	}
	/* A file of the movie outside the plan, when the plan links one to it:
	 * offered its tick; ticked, it is unlinked.  Never without. */
	for (i = 0; n_link > 0 && i < model->files->len; i++) {
		LFile *x = g_ptr_array_index(model->files, i);

		if (!x->linked || x->planned)
			continue;
		x->offered = TRUE;
		x->ticked = has_id(others, x->id);
		x->changed = x->ticked;
	}
	before = files_text(model, FALSE, &n_before);
	after = files_text(model, TRUE, &n_after);

	/* Why not now. */
	if (model->busy != NULL) {
		links->refused = g_strdup_printf("No link can be changed now. %s", model->busy);
	} else if (movie->grab_id != 0) {
		links->refused = g_strdup_printf("A download for %s is in flight (%s). Wait for it to be imported, or dismiss it "
			"on the Downloads page (/downloads); then change the links.", model->label,
			movie->grab_release != NULL ? movie->grab_release : "its release");
	}
	for (i = 0; i < model->others->len && links->refused == NULL; i++) {
		const Other *other = g_ptr_array_index(model->others, i);

		if (other->leaving == 0)
			continue;
		if (other->import_id > 0)
			links->refused = g_strdup_printf("No file can leave %s now. An import of it is open (open /imports/%"
				G_GINT64_FORMAT " to let it finish or dismiss it): its links are changed once no import is open.",
				other->label, other->import_id);
		else if (other->grab != NULL)
			links->refused = g_strdup_printf("A download for %s is in flight (%s). Wait for it to be imported, or dismiss "
				"it on the Downloads page (/downloads); then change the links.", other->label, other->grab);
	}

	/* The digest: the movie and what "wanted" depends on, every link of it,
	 * the plan as chosen with each file's link before, each tick offered,
	 * each movie a file leaves, and why it cannot be applied. */
	g_string_append_printf(digest, "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%d|%s", movie->id, movie->title_id,
		movie->monitored, links->refused != NULL ? links->refused : "");
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);

		if (f->linked)
			g_string_append_printf(digest, "|L%" G_GINT64_FORMAT ":%s:%d", f->id, f->hand_how != NULL ? f->hand_how : "",
				f->kept);
		if (f->planned)
			g_string_append_printf(digest, "|F%" G_GINT64_FORMAT ":%s:%" G_GINT64_FORMAT ":%d", f->id, f->path, f->owner,
				f->link);
		if (f->offered)
			g_string_append_printf(digest, "|O%" G_GINT64_FORMAT ":%d", f->id, f->ticked);
	}
	for (i = 0; i < model->others->len; i++) {
		const Other *other = g_ptr_array_index(model->others, i);

		if (other->leaving > 0)
			g_string_append_printf(digest, "|M%" G_GINT64_FORMAT ":%u:%u:%d:%s:%" G_GINT64_FORMAT, other->id,
				other->n_files, other->leaving, other->monitored, other->grab != NULL ? other->grab : "",
				other->import_id);
	}
	links->digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, digest->str, -1);

	/* The review. */
	for (i = 0; i < model->files->len; i++) {
		const LFile *f = g_ptr_array_index(model->files, i);

		if (f->changed) {
			AxisMovieLinkChange *change = g_new0(AxisMovieLinkChange, 1);

			change->file_id = f->id;
			change->path = g_strdup(f->path);
			change->before = g_strdup(f->linked ? model->label : f->other != NULL ? f->other->label : "No movie");
			change->after = g_strdup(f->planned && f->link ? model->label : "No movie");
			change->other = !f->planned;
			if (!f->planned)
				change->note = g_strdup("You ticked this file: it is unlinked from this movie and left alone by "
					"identification. It stays on disk.");
			else if (f->link && f->other != NULL)
				change->note = g_strdup_printf("It is linked to %s now, and leaves it.", f->other->label);
			g_ptr_array_add(links->changes, change);
		}
		if (f->offered) {
			AxisMovieLinkOffer *offer = g_new0(AxisMovieLinkOffer, 1);

			offer->file_id = f->id;
			offer->path = g_strdup(f->path);
			offer->ticked = f->ticked;
			g_ptr_array_add(links->offers, offer);
		}
	}
	{
		AxisMovieLinkTouched *touched = g_new0(AxisMovieLinkTouched, 1);

		touched->movie_id = movie->id;
		touched->label = g_strdup(model->label);
		touched->before = g_steal_pointer(&before);
		touched->after = g_steal_pointer(&after);
		if (n_after == 0)
			touched->what = g_strdup(left_what(movie->monitored));
		else if (n_after > 1 && n_after > n_before && n_before > 0)
			touched->what = g_strdup_printf("%s has a file already. After this it has %u files linked: nothing is "
				"replaced, and nothing is unlinked unless you tick it below. A movie with more than one file is judged for "
				"upgrades by its best.", model->label, n_after);
		else if (n_after > 1)
			touched->what = g_strdup_printf("After this it has %u files linked.", n_after);
		else if (n_before == 0)
			touched->what = g_strdup("It has a file then: it is in the library, and its file is kept by hand.");
		g_ptr_array_add(links->touched, touched);
	}
	for (i = 0; i < model->others->len; i++) {
		const Other *other = g_ptr_array_index(model->others, i);
		AxisMovieLinkTouched *touched;
		guint left;

		if (other->leaving == 0)
			continue;
		left = other->n_files > other->leaving ? other->n_files - other->leaving : 0;
		touched = g_new0(AxisMovieLinkTouched, 1);
		touched->movie_id = other->id;
		touched->label = g_strdup(other->label);
		touched->before = g_strdup_printf("%u %s", other->n_files, other->n_files == 1 ? "file" : "files");
		touched->after = left == 0 ? g_strdup("No file") : g_strdup_printf("%u %s", left, left == 1 ? "file" : "files");
		touched->what = left == 0 ? g_strdup(left_what(other->monitored)) :
			g_strdup_printf("It keeps %u %s.", left, left == 1 ? "file" : "files");
		g_ptr_array_add(links->touched, touched);
	}
	return TRUE;
}

/* ── Public ─────────────────────────────────────────────────────── */

gboolean
axis_movies_links_read(
	AxisMovies      *self,
	gint64           movie_id,
	const GArray    *also,
	guint            page,
	AxisMovieLinks **links,
	GError         **error
){
	AxisDbConn *conn;
	Model model;
	gboolean ok;

	g_return_val_if_fail(self != NULL && links != NULL, FALSE);

	*links = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = model_read(conn, movie_id, also, (gint)MIN(page, 1000000u), &model, error);
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
		g_array_append_val(ids, ((const AxisMovieLinkAsk *)g_ptr_array_index(asks, i))->file_id);
	return ids;
}

gboolean
axis_movies_links_review(
	AxisMovies       *self,
	gint64            movie_id,
	const GPtrArray  *asks,
	const GArray     *others,
	AxisMovieLinks  **links,
	GError          **error
){
	g_autoptr(GArray) also = asked_files(asks);
	g_autoptr(AxisMovieLinks) out = NULL;
	AxisDbConn *conn;
	Model model;
	gboolean ok;

	g_return_val_if_fail(self != NULL && links != NULL, FALSE);

	*links = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = model_read(conn, movie_id, also, -1, &model, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	out = links_from(&model);
	ok = plan(&model, out->movie, asks, others, out, error);
	model_clear(&model);
	if (ok)
		*links = g_steal_pointer(&out);
	return ok;
}

static gboolean
eventf(
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        title_id,
	const gchar  *label,
	const gchar  *by,
	GError      **error,
	const gchar  *format,
	...
) G_GNUC_PRINTF(7, 8);

static gboolean
eventf(
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        title_id,
	const gchar  *label,
	const gchar  *by,
	GError      **error,
	const gchar  *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_movies_store_event(conn, movie_id, title_id, label, "hand_link", text, by, 0, 0, error);
}

/* @n_after: the movie's files after the plan; @all: their paths. */
static gboolean
write_file(
	AxisDbConn            *conn,
	const Model           *model,
	const AxisMovieFacts  *movie,
	const LFile           *f,
	const gchar           *by,
	guint                  n_after,
	const gchar           *all,
	GError               **error
){
	g_autofree gchar *file = axis_movies_int_text(f->id);
	g_autofree gchar *owner = axis_movies_int_text(movie->id);
	g_autofree gchar *was = axis_movies_int_text(f->owner);
	const gchar *params[4];

	params[0] = file;
	params[1] = owner;
	params[2] = by;
	params[3] = was;
	if (!(f->planned && f->link)) {
		/* Unlinked: the link goes, the file stays, and the decision is
		 * remembered so that identification leaves the file alone. */
		if (!axis_db_conn_exec(conn, "DELETE FROM movie_file WHERE file_id = $1::bigint AND movie_id = $2::bigint", 2,
		        params, NULL, error) ||
		    !axis_library_hand_unlink_mark(conn, f->id, AXIS_MOVIES_MODULE_ID, movie->id, by, error))
			return FALSE;
		return eventf(conn, movie->id, movie->title_id, model->label, by, error, "The file %s was unlinked from %s by "
			"hand%s%s. The file on disk was not touched.%s", f->path, model->label, by != NULL ? " by " : "",
			by != NULL ? by : "", n_after > 0 ? "" : movie->monitored ? " The movie has no file now, so it is wanted "
			"again." : " The movie has no file now; it is not monitored, so it is not searched for.");
	}
	{
		g_autoptr(AxisDbResult) upsert = NULL;

		/* Linked by a person, and kept by hand.  A file with no link takes
		 * its quality from what identification read of it; one that had a
		 * link keeps what it had, and its import (the file's history still
		 * names it); an identity's is dropped, so that removals by identity
		 * never reach the link.  The row is changed only when the file is
		 * still linked where the review read it. */
		if (!axis_db_conn_exec(conn, "INSERT INTO movie_file (file_id, movie_id, quality, resolution, quality_text,"
		        " linked_by, hand_how, hand_by, hand_at, kept) VALUES ($1::bigint, $2::bigint,"
		        " (SELECT lif.quality FROM library_identity_file lif WHERE lif.file_id = $1::bigint LIMIT 1),"
		        " (SELECT lif.resolution FROM library_identity_file lif WHERE lif.file_id = $1::bigint LIMIT 1),"
		        " (SELECT lif.quality_text FROM library_identity_file lif WHERE lif.file_id = $1::bigint LIMIT 1),"
		        " $3, 'linked', $3, now(), true)"
		        " ON CONFLICT (file_id) DO UPDATE SET movie_id = EXCLUDED.movie_id, identity_id = NULL,"
		        " import_id = NULL, hand_how = 'linked', hand_by = EXCLUDED.hand_by, hand_at = now(), kept = true,"
		        " kept_how = NULL"
		        " WHERE movie_file.movie_id = $4::bigint RETURNING file_id", 4, params, &upsert, error))
			return FALSE;
		if (axis_db_result_get_n_rows(upsert) == 0) {
			g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "%s was linked to another movie meanwhile, "
				"so nothing was changed", f->path);
			return FALSE;
		}
		if (!axis_library_hand_unlink_clear(conn, f->id, error))
			return FALSE;
	}
	if (f->other != NULL) {
		guint left = f->other->n_files > f->other->leaving ? f->other->n_files - f->other->leaving : 0;

		if (!eventf(conn, f->other->id, f->other->title_id, f->other->label, by, error, "The file %s was unlinked from "
		        "%s by hand%s%s: it is linked to %s now. The file on disk was not touched.%s", f->path, f->other->label,
		        by != NULL ? " by " : "", by != NULL ? by : "", model->label, left > 0 ? "" : f->other->monitored ?
		        " The movie has no file now, so it is wanted again." :
		        " The movie has no file now; it is not monitored, so it is not searched for."))
			return FALSE;
	}
	if (n_after > 1)
		return eventf(conn, movie->id, movie->title_id, model->label, by, error, "The file %s was linked to %s by hand%s%s. "
			"%s now has %u files linked: %s. The file on disk was not touched.", f->path, model->label,
			by != NULL ? " by " : "", by != NULL ? by : "", model->label, n_after, all);
	return eventf(conn, movie->id, movie->title_id, model->label, by, error, "The file %s was linked to %s by hand%s%s. "
		"The file on disk was not touched.", f->path, model->label, by != NULL ? " by " : "", by != NULL ? by : "");
}

/*
 * Locks the movies that lose a file to @model (in the order of their ids, and
 * without waiting: this movie's row is held already, and a change that holds
 * one of them and wants this one then fails here instead of waiting on it, so
 * two changes cannot wait on each other), and counts each one's files again
 * against what the review showed.  *@stale: a count differs, so what the
 * review said of that movie ("it has N files now") is not true.
 */
static gboolean
lock_others(
	AxisDbConn   *conn,
	const Model  *model,
	gboolean     *stale,
	GError      **error
){
	g_autoptr(GString) ids = g_string_new("{");
	g_autoptr(AxisDbResult) counts = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[1];
	guint n = 0;
	guint i;
	guint k;

	for (i = 0; i < model->others->len; i++) {
		const Other *other = g_ptr_array_index(model->others, i);

		if (other->leaving == 0)
			continue;
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, n > 0 ? "," : "", other->id);
		n++;
	}
	if (n == 0)
		return TRUE;
	g_string_append_c(ids, '}');
	params[0] = ids->str;
	if (!axis_db_conn_exec(conn, "SELECT id FROM movie WHERE id = ANY ($1::bigint[]) ORDER BY id FOR UPDATE NOWAIT", 1,
	        params, NULL, &local)) {
		/* Only "lock not available" (SQLSTATE 55P03, which the message names)
		 * is a try-again; any other failure is an error. */
		if (local != NULL && local->message != NULL && strstr(local->message, "SQLSTATE 55P03") != NULL)
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "Axis or another person is changing "
				"a movie these files are linked to right now, so nothing was changed. Try again in a moment.");
		else
			g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (!axis_db_conn_exec(conn, "SELECT x.movie_id, count(*) FROM movie_file x JOIN library_file xl ON xl.id = x.file_id"
	        " WHERE x.movie_id = ANY ($1::bigint[]) AND xl.state = 'present' AND xl.kind = 'video' GROUP BY x.movie_id", 1,
	        params, &counts, error))
		return FALSE;
	for (i = 0; i < model->others->len; i++) {
		const Other *other = g_ptr_array_index(model->others, i);
		gint64 now = 0;

		if (other->leaving == 0)
			continue;
		for (k = 0; k < axis_db_result_get_n_rows(counts); k++) {
			if (get_int(counts, k, 0) == other->id)
				now = get_int(counts, k, 1);
		}
		if (now != (gint64)other->n_files)
			*stale = TRUE;
	}
	return TRUE;
}

/*
 * Locks the files @model changes (their rows of the index and of their
 * links, in the order of their ids), and reads who they are linked to:
 * imports and identification do not take the movie's lock, and a file may be
 * another movie's, so one may have been linked elsewhere since the reading.
 * *@stale: one has been.
 */
static gboolean
lock_files(
	AxisDbConn   *conn,
	const Model  *model,
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
	if (!axis_db_conn_exec(conn, "SELECT id FROM library_file WHERE id = ANY ($1::bigint[]) ORDER BY id FOR UPDATE", 1,
	        params, NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT file_id FROM movie_file WHERE file_id = ANY ($1::bigint[]) ORDER BY file_id"
	        " FOR UPDATE", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT lf.id, mf.movie_id FROM library_file lf LEFT JOIN movie_file mf ON mf.file_id ="
	        " lf.id WHERE lf.id = ANY ($1::bigint[]) AND lf.state = 'present'", 1, params, &owners, error))
		return FALSE;
	if (axis_db_result_get_n_rows(owners) != n)
		*stale = TRUE;
	for (i = 0; i < axis_db_result_get_n_rows(owners); i++) {
		const LFile *f = file_by_id(model, get_int(owners, i, 0));

		if (f == NULL || get_int(owners, i, 1) != f->owner)
			*stale = TRUE;
	}
	if (*stale)
		return TRUE;
	return lock_others(conn, model, stale, error);
}

gboolean
axis_movies_links_apply(
	AxisMovies       *self,
	gint64            movie_id,
	const GPtrArray  *asks,
	const GArray     *others,
	const gchar      *digest,
	const gchar      *user,
	AxisMovieLinks  **links,
	gboolean         *changed,
	GError          **error
){
	g_autoptr(GArray) also = asked_files(asks);
	g_autoptr(AxisMovieLinks) out = NULL;
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
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
	ok = axis_db_conn_begin(conn, error) && axis_movies_store_lock(conn, movie_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		ok = FALSE;
	}
	if (ok) {
		ok = model_read(conn, movie_id, also, -1, &model, error);
		read = ok;
	}
	if (ok) {
		out = links_from(&model);
		ok = plan(&model, out->movie, asks, others, out, error);
	}
	if (ok && g_strcmp0(out->digest, digest) != 0) {
		/* Not what the person saw: nothing is done, the review again. */
		if (changed != NULL)
			*changed = TRUE;
	} else if (ok && out->refused != NULL) {
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "%s", out->refused);
		ok = FALSE;
	} else if (ok) {
		gboolean stale = FALSE;

		ok = lock_files(conn, &model, &stale, error);
		if (ok && stale) {
			/* A file is another movie's now (or gone): nothing is done, and
			 * the review is made again from what is there. */
			model_clear(&model);
			read = FALSE;
			g_clear_pointer(&out, axis_movie_links_free);
			ok = model_read(conn, movie_id, also, -1, &model, error);
			read = ok;
			if (ok) {
				out = links_from(&model);
				ok = plan(&model, out->movie, asks, others, out, error);
			}
			if (ok && changed != NULL)
				*changed = TRUE;
			else if (!ok && error != NULL && *error != NULL && (*error)->domain == AXIS_MOVIES_ERROR)
				(*error)->code = AXIS_MOVIES_ERROR_IN_FLIGHT;
		} else if (ok) {
			const AxisMovieLinkTouched *own = g_ptr_array_index(out->touched, 0);
			guint n_after = 0;

			for (i = 0; i < model.files->len; i++) {
				const LFile *f = g_ptr_array_index(model.files, i);

				n_after += ((f->linked && !f->changed) || (f->planned && f->link)) ? 1 : 0;
			}
			for (i = 0; ok && i < model.files->len; i++) {
				const LFile *f = g_ptr_array_index(model.files, i);

				if (f->changed)
					ok = write_file(conn, &model, out->movie, f, by, n_after, own->after, error);
			}
			ok = ok && axis_db_conn_commit(conn, error);
		}
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (read)
		model_clear(&model);
	/* A refusal the review does not state itself (a file taken meanwhile, a
	 * movie locked): the page shows the reason it was given. */
	if (!ok && out != NULL && out->refused == NULL && error != NULL && *error != NULL &&
	    g_error_matches(*error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT))
		out->refused = g_strdup((*error)->message);
	if (links != NULL && out != NULL && out->digest != NULL)
		*links = g_steal_pointer(&out);
	return ok;
}

/* ── Let Axis manage this file again ────────────────────────────── */

void
axis_movie_link_manage_free(AxisMovieLinkManage *manage)
{
	if (manage == NULL)
		return;
	axis_movie_facts_free(manage->movie);
	g_free(manage->path);
	g_free(manage->outcome);
	g_free(manage->refused);
	g_free(manage->digest);
	g_free(manage);
}

/*
 * What handing @file_id back does, judged on @model: fills @out.  *@identity:
 * the unit identification holds the file in as this movie (IDENTIFIED).
 */
static gboolean
manage_plan(
	AxisDbConn           *conn,
	const Model          *model,
	gint64                file_id,
	AxisMovieLinkManage  *out,
	gint64               *identity,
	GError              **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) digest = g_string_new("movie-manage");
	g_autofree gchar *file = axis_movies_int_text(file_id);
	g_autofree gchar *reads = NULL;
	const gchar *params[1];
	const LFile *f = file_by_id(model, file_id);
	gint64 unit = 0;
	gint64 title_id = 0;
	gint64 managed = 0;

	*identity = 0;
	if (f == NULL || f->other != NULL ||
	    !((f->linked && (f->hand_how != NULL || f->kept)) || (!f->linked && f->unlinked_hand))) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that file is not one a person linked, "
			"keeps or unlinked, in this movie's reach");
		return FALSE;
	}
	out->file_id = file_id;
	out->path = g_strdup(f->path);
	/* What identification holds the file as: its unit, when it is linked. */
	params[0] = file;
	if (!axis_db_conn_exec(conn, "SELECT li.id, li.title_id, m.id, t.title, t.year FROM library_identity_file lif"
	        " JOIN library_identity li ON li.id = lif.identity_id AND li.state = 'linked' AND li.title_id IS NOT NULL"
	        " JOIN meta_title t ON t.id = li.title_id LEFT JOIN movie m ON m.title_id = li.title_id"
	        " WHERE lif.file_id = $1::bigint LIMIT 1", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) > 0) {
		unit = get_int(rows, 0, 0);
		title_id = get_int(rows, 0, 1);
		managed = get_int(rows, 0, 2);
		reads = axis_movies_title_label(axis_db_result_get_text(rows, 0, 3), (gint)get_int(rows, 0, 4));
	}
	if (!f->linked) {
		out->kind = AXIS_MOVIES_MANAGE_UNLINKED;
		if (managed > 0)
			out->outcome = g_strdup_printf("%s is no longer left alone. Identification reads it as %s, and links it to that "
				"movie at its next run.", f->path, reads);
		else if (title_id > 0)
			out->outcome = g_strdup_printf("%s is no longer left alone. Identification reads it as %s, which Axis does not "
				"manage, so it stays without a link.", f->path, reads);
		else
			out->outcome = g_strdup_printf("%s is no longer left alone. Identification links it to no movie as things are, "
				"so it stays without a link until something does.", f->path);
	} else if (f->by_import) {
		out->kind = AXIS_MOVIES_MANAGE_RELEASE;
		out->outcome = g_strdup_printf("An import linked %s, and identification never changes an import's link: it stays "
			"linked to %s. Only the mark of a person goes, so upgrades are searched for by the ordinary rules again.",
			f->path, model->label);
	} else if (title_id > 0 && title_id == model->movie->title_id) {
		out->kind = AXIS_MOVIES_MANAGE_IDENTIFIED;
		*identity = unit;
		out->outcome = g_strdup_printf("Identification reads %s as %s too: the link stays, and is identification's "
			"again. Upgrades are searched for by the ordinary rules again.", f->path, model->label);
	} else {
		out->kind = AXIS_MOVIES_MANAGE_NOTHING;
		out->outcome = g_strdup_printf("Identification does not read %s as %s (it reads it as %s), so there is nothing to "
			"hand back: its link stays as it is, still marked by hand. To give the link up, unlink the file on the links "
			"page.", f->path, model->label, reads != NULL ? reads : "no movie");
		out->refused = g_strdup(out->outcome);
	}
	g_string_append_printf(digest, "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%d|%" G_GINT64_FORMAT "|%s",
		model->movie->id, file_id, out->kind, unit, out->outcome);
	out->digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, digest->str, -1);
	return TRUE;
}

gboolean
axis_movies_links_manage_review(
	AxisMovies           *self,
	gint64                movie_id,
	gint64                file_id,
	AxisMovieLinkManage **manage,
	GError              **error
){
	g_autoptr(GArray) also = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisMovieLinkManage) out = g_new0(AxisMovieLinkManage, 1);
	AxisDbConn *conn;
	Model model;
	gint64 identity = 0;
	gboolean ok;

	g_return_val_if_fail(self != NULL && manage != NULL, FALSE);

	*manage = NULL;
	memset(&model, 0, sizeof(model));
	g_array_append_val(also, file_id);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = model_read(conn, movie_id, also, -1, &model, error);
	if (ok) {
		ok = manage_plan(conn, &model, file_id, out, &identity, error);
		out->movie = g_steal_pointer(&model.movie);
		model_clear(&model);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*manage = g_steal_pointer(&out);
	return TRUE;
}

gboolean
axis_movies_links_manage(
	AxisMovies           *self,
	gint64                movie_id,
	gint64                file_id,
	const gchar          *digest,
	const gchar          *user,
	AxisMovieLinkManage **manage,
	gboolean             *changed,
	GError              **error
){
	g_autoptr(GArray) also = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisMovieLinkManage) out = g_new0(AxisMovieLinkManage, 1);
	g_autofree gchar *file = axis_movies_int_text(file_id);
	g_autofree gchar *owner = axis_movies_int_text(movie_id);
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	const gchar *params[3];
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
	ok = axis_db_conn_begin(conn, error) && axis_movies_store_lock(conn, movie_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		ok = FALSE;
	}
	/* The file's rows are locked before it is read: imports and identification
	 * do not take the movie's lock. */
	ok = ok && axis_db_conn_exec(conn, "SELECT id FROM library_file WHERE id = $1::bigint FOR UPDATE", 1, params, NULL,
		error) &&
		axis_db_conn_exec(conn, "SELECT file_id FROM movie_file WHERE file_id = $1::bigint FOR UPDATE", 1, params, NULL,
			error);
	if (ok) {
		ok = model_read(conn, movie_id, also, -1, &model, error);
		read = ok;
	}
	ok = ok && manage_plan(conn, &model, file_id, out, &identity, error);
	if (ok && g_strcmp0(out->digest, digest) != 0) {
		/* Not what the person saw: nothing is done, the review again. */
		if (changed != NULL)
			*changed = TRUE;
	} else if (ok && out->refused != NULL) {
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "%s", out->refused);
		ok = FALSE;
	} else if (ok) {
		g_autofree gchar *identity_text = axis_movies_int_text(identity);

		switch (out->kind) {
		case AXIS_MOVIES_MANAGE_IDENTIFIED:
			params[2] = identity_text;
			/* A file brought over keeps its stamp and no identity, so that a
			 * run does not keep it again. */
			ok = axis_db_conn_exec(conn, "UPDATE movie_file SET"
				" identity_id = CASE WHEN hand_how = 'brought' THEN NULL ELSE $3::bigint END,"
				" hand_by = CASE WHEN hand_how = 'brought' THEN hand_by END,"
				" hand_at = CASE WHEN hand_how = 'brought' THEN hand_at END,"
				" hand_how = CASE WHEN hand_how = 'brought' THEN hand_how END,"
				" kept = false, kept_how = NULL WHERE file_id = $1::bigint AND movie_id = $2::bigint", 3,
				params, NULL,
				error);
			break;
		case AXIS_MOVIES_MANAGE_RELEASE:
			/* A file brought over keeps its stamp, as "Let Axis upgrade it
			 * again" leaves it, so that a run does not keep it again. */
			ok = axis_db_conn_exec(conn, "UPDATE movie_file SET hand_how = CASE WHEN hand_how = 'brought' THEN hand_how END,"
				" hand_by = CASE WHEN hand_how = 'brought' THEN hand_by END,"
				" hand_at = CASE WHEN hand_how = 'brought' THEN hand_at END,"
				" kept = false, kept_how = NULL WHERE file_id = $1::bigint AND movie_id = $2::bigint", 2, params, NULL,
				error);
			break;
		default:
			/* The mark is the file's: whichever movie it was unlinked from.
			 * Its folder is looked at again by identification's next run (as
			 * after an import, axis-movies-import.c), which links the file
			 * when it reads it as a managed movie. */
			ok = axis_library_hand_unlink_clear(conn, file_id, error) &&
				axis_db_conn_exec(conn, "UPDATE library_identity_scope s SET fingerprint ="
					" '00000000000000000000000000000000' FROM library_identity i JOIN library_identity_file lif"
					" ON lif.identity_id = i.id WHERE lif.file_id = $1::bigint AND s.root_id = i.root_id"
					" AND s.path_hash = i.scope_hash", 1, params, NULL, error) &&
				axis_db_conn_exec(conn, "UPDATE library_identity_root ir SET pending = pending + 1 FROM library_identity i"
					" JOIN library_identity_file lif ON lif.identity_id = i.id WHERE lif.file_id = $1::bigint"
					" AND ir.root_id = i.root_id", 1, params, NULL, error);
			break;
		}
		ok = ok && eventf(conn, model.movie->id, model.movie->title_id, model.label, by, error, "Axis manages the file %s "
			"again%s%s: identification, upgrades and imports treat it by the ordinary rules. %s The file on disk was not "
			"touched.", out->path, by != NULL ? ", let go by " : "", by != NULL ? by : "", out->outcome) &&
			axis_db_conn_commit(conn, error);
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (read) {
		if (out->path != NULL && out->digest != NULL)
			out->movie = g_steal_pointer(&model.movie);
		model_clear(&model);
	}
	if (manage != NULL && out->digest != NULL)
		*manage = g_steal_pointer(&out);
	return ok;
}
