/*
 * axis-identify-run.c - one run over a root of movies or for TV: which
 * scopes are new or changed, the units of each, their evidence (names,
 * identifiers, a sidecar .nfo, the provider's answers, the probe), the
 * verdict, and storing it; for TV, what each episode file's name holds and
 * the seasons the linked series need
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Read-only on the library: the only file this opens is a sidecar .nfo,
 * through a read guard, with axis_storage_open_file_beneath() (one
 * component at a time from the volume's top, no symbolic link, a regular
 * file only, read-only), and of it at most AXIS_IDENTIFY_NFO_MAX bytes are
 * read.  Nothing here writes to a volume.
 *
 * A root for TV: a scope is a folder at its top (a series' folder, every
 * video below it, its season folders included) or a video at its top, and
 * is one unit, looked up as a series with the rules films use.  The folder's
 * name is read with the name most of its episode files share
 * (axis_identify_series_name()), and the runtime checked is that of the
 * episode the main video's name holds.  No .nfo is read for a series.
 */

#include "identify/axis-identify-private.h"

#include <errno.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-open.h"
#include "storage/axis-volume-guard.h"

/* ── The run ────────────────────────────────────────────────────── */

typedef struct
{
	AxisIdentify               *self;
	gint64                      root_id;
	AxisMetaKind                kind;	/* film (movies) or series (tv) */
	gchar                      *root_rel;
	gint64                      volume_id;
	AxisVolumeGuard            *guard;	/* (nullable) taken at the first .nfo */
	const AxisMetaProviderInfo *provider;
	AxisIdentifyTuning          tuning;
	gint                        percent;
	gint                        minutes;
	gint64                      deadline_us;
	AxisJobContext             *context;
	AxisIdentifyReport         *report;
	gboolean                    stop;	/* the provider or the volume did not answer */
	gboolean                    halt;	/* stop once the scope being looked at is stored */
	gchar                      *stop_text;
	gint64                      scan_generation;	/* the root's, as the run began */
	gchar                      *started;	/* the database's time as the run began */
} Run;

typedef struct
{
	gint64              file_id;
	gchar              *rel_path;	/* stored form */
	gchar              *base;	/* stored form */
	gint64              size;
	gint64              duration_ms;
	gchar              *res_class;
	gint64              imported_title;
	AxisIdentifyName   *name;
	AxisIdentifyQuality quality;
	gboolean            best;
	/* A series' episode file: the season of its season folder (-1: none),
	 * what its name holds, and the title its own name states when that is
	 * not its series folder's (its comparison key, and as written). */
	gint                 folder_season;
	AxisIdentifyEpisodes episodes;
	gchar               *title_key;
	gchar               *name_title;
} Video;

static void
video_free(Video *video)
{
	if (video == NULL)
		return;
	g_free(video->rel_path);
	g_free(video->base);
	g_free(video->res_class);
	axis_identify_name_free(video->name);
	axis_identify_quality_clear(&video->quality);
	axis_identify_episodes_clear(&video->episodes);
	g_free(video->title_key);
	g_free(video->name_title);
	g_free(video);
}

/* The answers a unit's evidence points into: kept until it is stored. */
typedef struct
{
	gboolean             is_search;
	gboolean             is_season;
	AxisMetaSearchAnswer search;
	AxisMetaTitleAnswer  title;
	AxisMetaSeasonAnswer season;
} Held;

static void
held_free(Held *held)
{
	if (held == NULL)
		return;
	if (held->is_search)
		axis_meta_search_answer_clear(&held->search);
	else if (held->is_season)
		axis_meta_season_answer_clear(&held->season);
	else
		axis_meta_title_answer_clear(&held->title);
	g_free(held);
}

typedef struct
{
	gboolean             folder;
	gchar               *rel_path;
	GPtrArray           *videos;	/* Video, borrowed */
	Video               *main;
	Video               *named;	/* a series': the file read with its folder (nullable) */
	AxisIdentifyVerdict *verdict;
	gchar               *read_text;
	gchar               *read_title;
	gint                 read_year;
	GPtrArray           *held;	/* Held */
	guint                not_read;	/* videos of its scope beyond the most read */
} Unit;

static void
unit_free(Unit *unit)
{
	if (unit == NULL)
		return;
	g_free(unit->rel_path);
	g_ptr_array_unref(unit->videos);
	axis_identify_verdict_free(unit->verdict);
	g_free(unit->read_text);
	g_free(unit->read_title);
	g_ptr_array_unref(unit->held);
	g_free(unit);
}

static void
stop_run(
	Run         *run,
	const gchar *text
){
	if (run->stop)
		return;
	run->stop = TRUE;
	run->stop_text = g_strdup(text);
}

/* ── Lookups ────────────────────────────────────────────────────── */

typedef enum
{
	LOOK_FOUND,
	LOOK_NOT_FOUND,
	LOOK_WAIT,	/* the provider did not answer: the run stops */
	LOOK_UNTOLD	/* a full record was not answered: only the search's record is stored */
} Look;

static gboolean
not_answered(
	gboolean saved,
	gint64   fetched_us
){
	return saved && fetched_us == 0;
}

/* Whether the run's provider holds Axis off now: it told Axis to wait (a
 * 429 with its time), or it is backed off after failures (errors, 5xx,
 * timeouts in a row). */
static gboolean
provider_paused(Run *run)
{
	guint n = axis_meta_n_providers(run->self->meta);
	gboolean paused = FALSE;
	guint i;

	for (i = 0; i < n && !paused; i++) {
		AxisMetaProviderStatus status;

		if (axis_meta_provider_at(run->self->meta, i) != run->provider)
			continue;
		axis_meta_provider_status(run->self->meta, i, &status);
		paused = status.limit.paused_until_us > 0 || status.limit.backoff_until_us > 0;
		axis_meta_provider_status_clear(&status);
	}
	return paused;
}

/*
 * A stored title's full record (its years, runtime, other names,
 * identifiers, a series' seasons).  LOOK_UNTOLD: the store holds only the
 * search's record of it and the provider did not give the full one now
 * (an error, a timeout, a 404): *@title is the stored record, and
 * *@problem (nullable, owned by the unit) says why.  The search's record
 * never stands in for the full one: its age is not the full record's (the
 * folder waits; examine_unit() stops the run after it when the provider
 * holds Axis off).
 */
static Look
full_record(
	Run                  *run,
	Unit                 *unit,
	gint64                title_id,
	guint                 wait_ms,
	const AxisMetaTitle **title,
	const gchar         **problem,
	GError              **error
){
	Held *held = g_new0(Held, 1);
	g_autoptr(GError) local = NULL;

	*title = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (!axis_meta_get_title(run->self->meta, title_id, wait_ms, &held->title, &local)) {
		held_free(held);
		if (local != NULL && local->domain == AXIS_META_ERROR)
			return LOOK_NOT_FOUND;
		g_propagate_error(error, g_steal_pointer(&local));
		return LOOK_WAIT;
	}
	g_ptr_array_add(unit->held, held);
	if (held->title.title == NULL)
		return wait_ms > 0 && not_answered(held->title.saved, held->title.fetched_us) ? LOOK_WAIT : LOOK_NOT_FOUND;
	if (wait_ms > 0 && not_answered(held->title.saved, held->title.fetched_us))
		return LOOK_WAIT;
	if (held->title.title->kind != run->kind)
		return LOOK_NOT_FOUND;
	*title = held->title.title;
	if (wait_ms > 0 && !held->title.title->detail && (held->title.saved || held->title.not_found)) {
		if (problem != NULL)
			*problem = held->title.problem != NULL ? held->title.problem : held->title.not_found ?
				"it said it has no such title" : NULL;
		return LOOK_UNTOLD;
	}
	return LOOK_FOUND;
}

/* The runtime of episode @number of season @season of the stored series
 * @title_id (its season fetched now when it never was): 0 when not known. */
static Look
episode_runtime(
	Run      *run,
	Unit     *unit,
	gint64    title_id,
	gint      season,
	gint      number,
	gint     *runtime,
	GError  **error
){
	g_autoptr(GError) local = NULL;
	Held *held = g_new0(Held, 1);
	guint i;

	*runtime = 0;
	held->is_season = TRUE;
	if (!axis_meta_get_season(run->self->meta, title_id, season, run->tuning.wait_ms, &held->season, &local)) {
		held_free(held);
		if (local != NULL && local->domain == AXIS_META_ERROR)
			return LOOK_NOT_FOUND;
		g_propagate_error(error, g_steal_pointer(&local));
		return LOOK_WAIT;
	}
	g_ptr_array_add(unit->held, held);
	if (held->season.season == NULL || held->season.season->episodes == NULL)
		return not_answered(held->season.saved, held->season.fetched_us) ? LOOK_WAIT : LOOK_NOT_FOUND;
	for (i = 0; i < held->season.season->episodes->len; i++) {
		const AxisMetaEpisode *episode = g_ptr_array_index(held->season.season->episodes, i);

		if (episode->number == number) {
			*runtime = MAX(episode->runtime, 0);
			return LOOK_FOUND;
		}
	}
	return LOOK_NOT_FOUND;
}

/* The title of the run's kind @scheme:@value names, by the provider's own
 * id or by a find. */
static Look
look_up_id(
	Run                  *run,
	Unit                 *unit,
	const gchar          *scheme,
	const gchar          *value,
	const AxisMetaTitle **title,
	GError              **error
){
	g_autoptr(GError) local = NULL;
	Held *held;
	guint i;

	*title = NULL;
	if (!axis_meta_id_valid(scheme, value))
		return LOOK_NOT_FOUND;
	held = g_new0(Held, 1);
	if (strcmp(scheme, run->provider->name) == 0) {
		if (!axis_meta_lookup(run->self->meta, run->provider->name, run->kind, value, run->tuning.wait_ms,
		        &held->title, &local)) {
			held_free(held);
			if (local != NULL && local->domain == AXIS_META_ERROR)
				return LOOK_NOT_FOUND;
			g_propagate_error(error, g_steal_pointer(&local));
			return LOOK_WAIT;
		}
		g_ptr_array_add(unit->held, held);
		if (held->title.title != NULL && held->title.title->kind == run->kind) {
			*title = held->title.title;
			return LOOK_FOUND;
		}
		if (held->title.not_found)
			return LOOK_NOT_FOUND;
		return not_answered(held->title.saved, held->title.fetched_us) ? LOOK_WAIT : LOOK_NOT_FOUND;
	}
	held->is_search = TRUE;
	if (!axis_meta_find(run->self->meta, run->provider->name, run->kind, scheme, value, run->tuning.wait_ms,
	        &held->search, &local)) {
		held_free(held);
		if (local != NULL && local->domain == AXIS_META_ERROR)
			return LOOK_NOT_FOUND;
		g_propagate_error(error, g_steal_pointer(&local));
		return LOOK_WAIT;
	}
	g_ptr_array_add(unit->held, held);
	for (i = 0; held->search.titles != NULL && i < held->search.titles->len; i++) {
		const AxisMetaTitle *found = g_ptr_array_index(held->search.titles, i);
		Look look;

		if (found->kind != run->kind || found->id <= 0)
			continue;
		/* The identifier names the title: its stored record does, when its
		 * full one is not answered. */
		look = full_record(run, unit, found->id, run->tuning.wait_ms, title, NULL, error);
		return look == LOOK_UNTOLD ? LOOK_FOUND : look;
	}
	return not_answered(held->search.saved, held->search.fetched_us) ? LOOK_WAIT : LOOK_NOT_FOUND;
}

/* The provider's titles of the run's kind called what @name says, page
 * @page of its answer, added to @found; *@answers (optional): how many
 * answers the page held. */
static Look
search_name(
	Run                    *run,
	Unit                   *unit,
	const AxisIdentifyName *name,
	guint                   page,
	GPtrArray              *found,
	guint                  *answers,
	GError                **error
){
	g_autoptr(GError) local = NULL;
	Held *held;
	guint i;

	if (answers != NULL)
		*answers = 0;
	if (name == NULL || name->key == NULL || name->release->title == NULL)
		return LOOK_NOT_FOUND;
	held = g_new0(Held, 1);
	held->is_search = TRUE;
	if (!axis_meta_search_page(run->self->meta, run->provider->name, run->kind, name->release->title, 0, page,
	        run->tuning.wait_ms, &held->search, &local)) {
		held_free(held);
		if (local != NULL && local->domain == AXIS_META_ERROR)
			return LOOK_NOT_FOUND;
		g_propagate_error(error, g_steal_pointer(&local));
		return LOOK_WAIT;
	}
	g_ptr_array_add(unit->held, held);
	if (not_answered(held->search.saved, held->search.fetched_us))
		return LOOK_WAIT;
	if (answers != NULL && held->search.titles != NULL)
		*answers = held->search.titles->len;
	for (i = 0; held->search.titles != NULL && i < held->search.titles->len; i++) {
		AxisMetaTitle *title = g_ptr_array_index(held->search.titles, i);
		guint k;
		gboolean seen = FALSE;

		if (title->kind != run->kind || title->id <= 0)
			continue;
		for (k = 0; k < found->len; k++) {
			if (((AxisMetaTitle *)g_ptr_array_index(found, k))->id == title->id)
				seen = TRUE;
		}
		if (!seen)
			g_ptr_array_add(found, title);
	}
	return LOOK_FOUND;
}

/* ── .nfo files ─────────────────────────────────────────────────── */

/* The stored name without its extension. */
static gchar *
stem_of(const gchar *base)
{
	const gchar *dot = strrchr(base, '.');

	return dot != NULL && dot != base ? g_strndup(base, (gsize)(dot - base)) : g_strdup(base);
}

/* Reads and scans one .nfo; %FALSE (and the run stopped) when the volume is
 * away. */
static gboolean
read_nfo(
	Run             *run,
	const gchar     *rel_path,
	AxisIdentifyIds *ids
){
	g_autoptr(GError) local = NULL;
	g_autofree guint8 *buffer = NULL;
	AxisStorageOpenStatus status;
	struct stat st;
	gsize total = 0;
	gint saved = 0;
	gint fd = -1;

	axis_identify_ids_init(ids);
	if (run->guard == NULL) {
		run->guard = axis_volume_guard_acquire(run->self->storage, run->volume_id, AXIS_VOLUME_INTENT_READ, &local);
		if (run->guard == NULL) {
			stop_run(run, "The volume is away: identification waits for it.");
			return FALSE;
		}
	}
	status = axis_storage_open_file_beneath(axis_volume_guard_get_fd(run->guard), run->root_rel, rel_path, &fd, &st,
		&saved);
	if (status != AXIS_STORAGE_OPEN_OK) {
		/* Not there, or not a plain file: nothing to read, as long as the
		 * volume is still the one checked. */
		if (!axis_volume_guard_check(run->guard, NULL)) {
			stop_run(run, "The volume went away: identification waits for it.");
			return FALSE;
		}
		return TRUE;
	}
	buffer = g_malloc(AXIS_IDENTIFY_NFO_MAX);
	while (total < AXIS_IDENTIFY_NFO_MAX) {
		gssize n = read(fd, buffer + total, AXIS_IDENTIFY_NFO_MAX - total);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		total += (gsize)n;
	}
	close(fd);
	axis_identify_nfo_scan(buffer, total, ids);
	return TRUE;
}

/* The identifiers of the unit's .nfo files: its video's own ("Film.nfo"),
 * then, for a folder, "movie.nfo" and any other, at most
 * AXIS_IDENTIFY_MAX_NFO. */
static gboolean
nfo_ids(
	Run             *run,
	Unit            *unit,
	AxisIdentifyIds *ids,
	gboolean        *any,
	GError         **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	g_autofree gchar *dir = NULL;
	g_autofree gchar *stem = stem_of(unit->main->base);
	const gchar *params[2];
	guint read = 0;
	guint n;
	guint i;
	gint pass;

	axis_identify_ids_init(ids);
	*any = FALSE;
	dir = unit->folder ? g_strdup(unit->rel_path) : g_path_get_dirname(unit->main->rel_path);
	if (strcmp(dir, ".") == 0) {
		g_free(dir);
		dir = g_strdup("");
	}
	params[0] = root;
	params[1] = dir;
	if (!axis_db_pool_exec(run->self->pool, "SELECT f.rel_path, f.base_name FROM library_file f WHERE f.root_id = $1::bigint"
	        " AND f.dir_hash = library_path_hash($2) AND f.kind <> 'symlink' AND f.state = 'present'"
	        " AND lower(f.base_name) LIKE '%.nfo' ORDER BY f.base_name LIMIT 20", 2, params, &rows, error))
		return FALSE;
	n = axis_db_result_get_n_rows(rows);
	/* Its own first, then movie.nfo, then the rest (a folder's only). */
	for (pass = 0; pass < 3 && read < AXIS_IDENTIFY_MAX_NFO && !run->stop; pass++) {
		for (i = 0; i < n && read < AXIS_IDENTIFY_MAX_NFO && !run->stop; i++) {
			const gchar *base = axis_db_result_get_text(rows, i, 1);
			g_autofree gchar *own = g_strconcat(stem, ".nfo", NULL);
			gboolean is_own = g_ascii_strcasecmp(base, own) == 0;
			gboolean is_movie = g_ascii_strcasecmp(base, "movie.nfo") == 0;
			AxisIdentifyIds found;

			if ((pass == 0 && !is_own) || (pass == 1 && (is_own || !is_movie)) || (pass == 2 && (is_own || is_movie)))
				continue;
			if (pass > 0 && !unit->folder)
				continue;
			if (!read_nfo(run, axis_db_result_get_text(rows, i, 0), &found))
				return TRUE;
			read++;
			if (found.tmdb[0] != '\0' && ids->tmdb[0] != '\0' && strcmp(found.tmdb, ids->tmdb) != 0)
				ids->tmdb_conflict = TRUE;
			else if (found.tmdb[0] != '\0' && !ids->tmdb_conflict)
				memcpy(ids->tmdb, found.tmdb, sizeof(ids->tmdb));
			if (found.imdb[0] != '\0' && ids->imdb[0] != '\0' && strcmp(found.imdb, ids->imdb) != 0)
				ids->imdb_conflict = TRUE;
			else if (found.imdb[0] != '\0' && !ids->imdb_conflict)
				memcpy(ids->imdb, found.imdb, sizeof(ids->imdb));
			ids->tmdb_conflict |= found.tmdb_conflict;
			ids->imdb_conflict |= found.imdb_conflict;
			*any = TRUE;
		}
	}
	if (ids->tmdb_conflict)
		ids->tmdb[0] = '\0';
	if (ids->imdb_conflict)
		ids->imdb[0] = '\0';
	return TRUE;
}

/* ── Identifiers ────────────────────────────────────────────────── */

typedef struct
{
	gchar        token[40];
	const gchar *where;
	gchar        tmdb[16];
	gchar        imdb[16];
	gchar        tvdb[16];
	gboolean     conflict;
	gboolean     any;
} Picked;

/* @series: a series' evidence, which takes TVDB identifiers too. */
static void
pick_merge(
	Picked                *pick,
	const AxisIdentifyIds *ids,
	const gchar           *where,
	gboolean               series
){
	if (series) {
		if (ids->tvdb_conflict) {
			pick->conflict = TRUE;
			if (pick->where == NULL)
				pick->where = where;
		}
		if (ids->tvdb[0] != '\0') {
			if (pick->tvdb[0] != '\0' && strcmp(pick->tvdb, ids->tvdb) != 0)
				pick->conflict = TRUE;
			else if (pick->tvdb[0] == '\0')
				memcpy(pick->tvdb, ids->tvdb, sizeof(pick->tvdb));
			if (pick->where == NULL)
				pick->where = where;
			pick->any = TRUE;
		}
	}
	if (axis_identify_ids_conflict(ids))
		pick->conflict = TRUE;
	if (ids->tmdb[0] != '\0') {
		if (pick->tmdb[0] != '\0' && strcmp(pick->tmdb, ids->tmdb) != 0)
			pick->conflict = TRUE;
		else if (pick->tmdb[0] == '\0')
			memcpy(pick->tmdb, ids->tmdb, sizeof(pick->tmdb));
		if (pick->where == NULL)
			pick->where = where;
		pick->any = TRUE;
	}
	if (ids->imdb[0] != '\0') {
		if (pick->imdb[0] != '\0' && strcmp(pick->imdb, ids->imdb) != 0)
			pick->conflict = TRUE;
		else if (pick->imdb[0] == '\0')
			memcpy(pick->imdb, ids->imdb, sizeof(pick->imdb));
		if (pick->where == NULL)
			pick->where = where;
		pick->any = TRUE;
	}
	if (axis_identify_ids_conflict(ids) && pick->where == NULL)
		pick->where = where;
}

/* The evidence of what @pick names: looked up (TMDb first), with the other
 * scheme, when given, checked against the title's own identifiers. */
static Look
weigh_pick(
	Run                    *run,
	Unit                   *unit,
	Picked                 *pick,
	AxisIdentifyIdEvidence *evidence,
	GError                **error
){
	const AxisMetaTitle *title = NULL;
	const gchar *scheme;
	const gchar *value;
	Look look;

	memset(evidence, 0, sizeof(*evidence));
	evidence->where = pick->where;
	if (pick->conflict) {
		evidence->state = AXIS_IDENTIFY_ID_CONFLICT;
		return LOOK_FOUND;
	}
	if (!pick->any)
		return LOOK_FOUND;
	/* TMDb's first, then a series' TVDB, then IMDb. */
	scheme = pick->tmdb[0] != '\0' ? "tmdb" : pick->tvdb[0] != '\0' ? "tvdb" : "imdb";
	value = pick->tmdb[0] != '\0' ? pick->tmdb : pick->tvdb[0] != '\0' ? pick->tvdb : pick->imdb;
	g_snprintf(pick->token, sizeof(pick->token), "%s %s", scheme, value);
	evidence->token = pick->token;
	look = look_up_id(run, unit, scheme, value, &title, error);
	if (look == LOOK_WAIT)
		return LOOK_WAIT;
	if (look == LOOK_NOT_FOUND || title == NULL) {
		evidence->state = AXIS_IDENTIFY_ID_NOT_FOUND;
		return LOOK_FOUND;
	}
	/* More than one scheme given: they must name one title. */
	if (pick->imdb[0] != '\0' && strcmp(scheme, "imdb") != 0) {
		const gchar *own = axis_meta_title_get_id(title, "imdb");

		if (own != NULL && strcmp(own, pick->imdb) != 0) {
			evidence->state = AXIS_IDENTIFY_ID_CONFLICT;
			return LOOK_FOUND;
		}
	}
	if (pick->tvdb[0] != '\0' && strcmp(scheme, "tvdb") != 0) {
		const gchar *own = axis_meta_title_get_id(title, "tvdb");

		if (own != NULL && strcmp(own, pick->tvdb) != 0) {
			evidence->state = AXIS_IDENTIFY_ID_CONFLICT;
			return LOOK_FOUND;
		}
	}
	evidence->state = AXIS_IDENTIFY_ID_FOUND;
	evidence->title = title;
	return LOOK_FOUND;
}

/* ── One unit ───────────────────────────────────────────────────── */

static gchar *
read_words(
	const Run              *run,
	const Unit             *unit,
	const AxisIdentifyName *folder,
	const AxisIdentifyName *file_name
){
	g_autoptr(GString) text = g_string_new(NULL);
	g_autofree gchar *file = axis_identify_name_describe(file_name);

	if (folder != NULL) {
		g_autofree gchar *words = axis_identify_name_describe(folder);

		g_string_append_printf(text, "The folder reads as %s; ", words);
		if (run->kind == AXIS_META_KIND_SERIES)
			g_string_append_printf(text, "its episode files as %s.", file);
		else
			g_string_append_printf(text, "the file as %s.", file);
	} else {
		g_string_append_printf(text, "The file reads as %s.", file);
	}
	if (unit->videos->len > 1)
		g_string_append_printf(text, " %u videos.", unit->videos->len);
	return axis_identify_text_cut(text->str, 1900);
}

/* A series' main video: the first, by path, that is probed and holds one
 * episode by its numbers; else the first holding one; else the first. */
static Video *
series_main(GPtrArray *videos)
{
	Video *one = NULL;
	guint i;

	for (i = 0; i < videos->len; i++) {
		Video *video = g_ptr_array_index(videos, i);

		if (video->episodes.code != NULL || video->episodes.n != 1)
			continue;
		if (video->duration_ms > 0)
			return video;
		if (one == NULL)
			one = video;
	}
	return one != NULL ? one : g_ptr_array_index(videos, 0);
}

/* The files of a series' unit whose own names state another title than
 * @own (the key the unit was read as): that title, kept for the pages. */
static void
mark_other_titles(
	Unit        *unit,
	const gchar *own
){
	guint i;

	for (i = 0; i < unit->videos->len; i++) {
		Video *video = g_ptr_array_index(unit->videos, i);

		g_clear_pointer(&video->title_key, g_free);
		g_clear_pointer(&video->name_title, g_free);
		if (video->name->key == NULL || own == NULL || strcmp(video->name->key, own) == 0)
			continue;
		video->title_key = axis_identify_text_cut(video->name->key, 400);
		video->name_title = axis_identify_text_cut(video->name->release->title, 200);
	}
}

/* What a series' episode files hold, the episodes of another title than
 * @own (the key the unit was read as) left out. */
static void
series_shape(
	const Unit        *unit,
	const gchar       *own,
	AxisIdentifyShape *shape
){
	guint i;

	axis_identify_shape_init(shape);
	for (i = 0; i < unit->videos->len; i++) {
		const Video *video = g_ptr_array_index(unit->videos, i);

		if (video->name->key != NULL && own != NULL && strcmp(video->name->key, own) != 0 &&
		    axis_release_is_episode(video->name->release))
			continue;
		axis_identify_shape_add(shape, &video->episodes);
	}
	axis_identify_shape_finish(shape);
}

/* Whether @title is in @details (by its id). */
static gboolean
in_details(
	GPtrArray           *details,
	const AxisMetaTitle *title
){
	guint i;

	for (i = 0; i < details->len; i++) {
		const AxisMetaTitle *d = g_ptr_array_index(details, i);

		if (d == title || (title->id > 0 && d->id == title->id))
			return TRUE;
	}
	return FALSE;
}

/* Whether a record of @details has @name as a title or another name. */
static gboolean
named_in_details(
	GPtrArray              *details,
	const AxisIdentifyName *name
){
	guint i;

	for (i = 0; i < details->len; i++) {
		if (axis_identify_title_fits(name, g_ptr_array_index(details, i)))
			return TRUE;
	}
	return FALSE;
}

/* Whether @brief (a search's answer) could be the title @name means or stand
 * in its way: its year is not known, or within AXIS_IDENTIFY_YEAR_REACH of
 * the names' year (any, without one).  One further off is neither read in
 * full nor waited for. */
static gboolean
within_reach(
	const AxisIdentifyName *name,
	const AxisMetaTitle    *brief
){
	gint years[3];
	guint n = axis_identify_title_years(brief, years);
	guint i;

	if (name->year <= 0 || n == 0)
		return TRUE;
	for (i = 0; i < n; i++) {
		if (ABS(years[i] - name->year) <= AXIS_IDENTIFY_YEAR_REACH)
			return TRUE;
	}
	return FALSE;
}

/*
 * The first AXIS_IDENTIFY_OTHER_NAME_LOOKS answers of @found[@from, @to)
 * within reach of the names' year read in full, into @details: a search's
 * answers carry no other names, and one of them may have the name read as
 * one.  *@partial: some answers within reach were left unread.  A record
 * not answered ends it: *@untold is that title.
 */
static Look
other_names(
	Run                    *run,
	Unit                   *unit,
	const AxisIdentifyName *name,
	GPtrArray              *found,
	guint                   from,
	guint                   to,
	GPtrArray              *details,
	gboolean               *partial,
	const AxisMetaTitle   **untold,
	const gchar           **problem,
	GError                **error
){
	guint looked = 0;
	guint i;

	for (i = from; i < to; i++) {
		const AxisMetaTitle *brief = g_ptr_array_index(found, i);
		const AxisMetaTitle *detail = NULL;
		Look look;

		if (!within_reach(name, brief))
			continue;
		if (looked >= AXIS_IDENTIFY_OTHER_NAME_LOOKS) {
			*partial = TRUE;
			break;
		}
		looked++;
		if (in_details(details, brief))
			continue;
		look = full_record(run, unit, brief->id, run->tuning.wait_ms, &detail, problem, error);
		if (look == LOOK_WAIT)
			return LOOK_WAIT;
		if (look == LOOK_UNTOLD) {
			*untold = brief;
			return LOOK_FOUND;
		}
		if (detail != NULL)
			g_ptr_array_add(details, (gpointer)detail);
	}
	return LOOK_FOUND;
}

/*
 * The titles of the name read with a year, or a film's or a dated series'
 * without one (rule 4): their full records, into @details.  When no answer
 * of the search (@found, its first page, which held @answers) has the name
 * as its title, the first few are read in full for their other names, and
 * when none has it there either and the page was full, the second page is
 * asked for (added to @found) and looked at the same way.  Then every answer
 * that has the name, at most AXIS_IDENTIFY_DETAILS of them: those within
 * reach of the name's year (within_reach()) first, the nearest first (*@more:
 * more of them have it), then those out of reach by a search answer's year,
 * the nearest first, while the count allows (a search answer's year alone
 * does not rule a title out: fix-identify-regional-years; one whose stored
 * year is its full record's is out of reach indeed, and not read again).
 * A name without a year, only when it is the
 * one answer of the name (more make it ambiguous whatever their records
 * say).  *@partial: answers within reach were left unread for their other
 * names.  A full record not answered ends it (*@untold is that title, and
 * nothing is decided), except for a title out of reach while another title
 * of the name is within reach: that one is judged as stored, and nothing
 * waits for it (as before this change).
 */
static Look
name_records(
	Run                    *run,
	Unit                   *unit,
	const AxisIdentifyName *name,
	guint                   answers,
	GPtrArray              *found,
	GPtrArray              *details,
	gboolean               *more,
	gboolean               *partial,
	const AxisMetaTitle   **untold,
	const gchar           **problem,
	GError                **error
){
	g_autofree guint *order = NULL;
	gboolean any_near = FALSE;
	guint fetched = 0;
	guint limit;
	guint pass;
	guint n;
	guint i;
	Look look;

	*more = FALSE;
	*partial = FALSE;
	*untold = NULL;
	order = g_new0(guint, MAX(found->len, 1));
	if (axis_identify_named_order(name, (const AxisMetaTitle *const *)found->pdata, found->len, order) == 0) {
		look = other_names(run, unit, name, found, 0, found->len, details, partial, untold, problem, error);
		if (look != LOOK_FOUND || *untold != NULL)
			return look;
		if (!named_in_details(details, name) && answers >= AXIS_IDENTIFY_SEARCH_PAGE) {
			g_autofree guint *second = NULL;
			guint before = found->len;

			look = search_name(run, unit, name, 2, found, NULL, error);
			if (look == LOOK_WAIT)
				return LOOK_WAIT;
			second = g_new0(guint, MAX(found->len - before, 1));
			if (found->len > before && axis_identify_named_order(name, (const AxisMetaTitle *const *)found->pdata +
			        before, found->len - before, second) == 0) {
				look = other_names(run, unit, name, found, before, found->len, details, partial, untold, problem,
					error);
				if (look != LOOK_FOUND || *untold != NULL)
					return look;
			}
		}
		g_free(order);
		order = g_new0(guint, MAX(found->len, 1));
	}
	n = axis_identify_named_order(name, (const AxisMetaTitle *const *)found->pdata, found->len, order);
	limit = name->year > 0 ? AXIS_IDENTIFY_DETAILS : n == 1 ? 1 : 0;
	for (i = 0; i < n; i++)
		any_near = any_near || within_reach(name, g_ptr_array_index(found, order[i]));
	/* Within reach first; then, with what the count leaves, out of reach. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			const AxisMetaTitle *brief = g_ptr_array_index(found, order[i]);
			const AxisMetaTitle *detail = NULL;
			gboolean near = within_reach(name, brief);

			/* Out of reach by its full record's own year (stored), it is out
			 * of reach indeed: not read again. */
			if (in_details(details, brief) || near != (pass == 0) || (!near && brief->detail))
				continue;
			if (fetched >= limit) {
				/* One within reach left unread could be the one; one out of
				 * reach is judged as stored. */
				if (near && name->year > 0)
					*more = TRUE;
				break;
			}
			look = full_record(run, unit, brief->id, run->tuning.wait_ms, &detail, problem, error);
			if (look == LOOK_WAIT)
				return LOOK_WAIT;
			fetched++;
			if (look == LOOK_UNTOLD) {
				if (!near && any_near) {
					if (problem != NULL)
						*problem = NULL;
					continue;
				}
				*untold = brief;
				return LOOK_FOUND;
			}
			if (detail != NULL)
				g_ptr_array_add(details, (gpointer)detail);
		}
	}
	return LOOK_FOUND;
}

/*
 * A series' folder whose names give no year (rule 5): every series of the
 * name read, from the search's answers (@found), with its full record (its
 * seasons), compared together: those that have it as their title or
 * original title, and those of the first few others whose full records have
 * it as another name; at most AXIS_IDENTIFY_SERIES_COMPARED (*@more: there
 * were more).  For each that the episodes on disk fit, the runtime of the
 * episode the main video holds (that season fetched now: it is needed
 * anyway once linked).  A full record not answered ends it: *@untold is
 * that series.
 */
static Look
series_candidates(
	Run                     *run,
	Unit                    *unit,
	const AxisIdentifyName  *name,
	const AxisIdentifyShape *shape,
	GPtrArray               *found,
	GArray                  *out,
	gboolean                *more,
	gboolean                *partial,
	const AxisMetaTitle    **untold,
	const gchar            **problem,
	GError                 **error
){
	guint looked = 0;
	guint i;
	Look look;

	*more = FALSE;
	*partial = FALSE;
	*untold = NULL;
	for (i = 0; i < found->len; i++) {
		const AxisMetaTitle *brief = g_ptr_array_index(found, i);
		const AxisMetaTitle *detail = NULL;
		AxisIdentifySeriesCandidate c;

		if (!axis_identify_title_fits(name, brief))
			continue;
		if (out->len >= AXIS_IDENTIFY_SERIES_COMPARED) {
			*more = TRUE;
			continue;
		}
		look = full_record(run, unit, brief->id, run->tuning.wait_ms, &detail, problem, error);
		if (look == LOOK_WAIT)
			return LOOK_WAIT;
		if (look == LOOK_UNTOLD) {
			*untold = brief;
			return LOOK_FOUND;
		}
		if (detail == NULL)
			continue;
		memset(&c, 0, sizeof(c));
		c.title = detail;
		g_array_append_val(out, c);
	}
	/* The others, whatever was found by the title: a series known by the
	 * name in another country is as much a series of the name.  *@partial:
	 * some were left unread. */
	for (i = 0; i < found->len; i++) {
		const AxisMetaTitle *brief = g_ptr_array_index(found, i);
		const AxisMetaTitle *detail = NULL;
		AxisIdentifySeriesCandidate c;

		if (axis_identify_title_fits(name, brief))
			continue;
		if (looked >= AXIS_IDENTIFY_OTHER_NAME_LOOKS) {
			*partial = TRUE;
			break;
		}
		looked++;
		look = full_record(run, unit, brief->id, run->tuning.wait_ms, &detail, problem, error);
		if (look == LOOK_WAIT)
			return LOOK_WAIT;
		if (look == LOOK_UNTOLD) {
			*untold = brief;
			return LOOK_FOUND;
		}
		if (detail == NULL || !axis_identify_title_fits(name, detail))
			continue;
		if (out->len >= AXIS_IDENTIFY_SERIES_COMPARED) {
			*more = TRUE;
			continue;
		}
		memset(&c, 0, sizeof(c));
		c.title = detail;
		c.other_name = TRUE;
		g_array_append_val(out, c);
	}
	if (unit->main->episodes.code != NULL || unit->main->episodes.n != 1 || unit->main->episodes.season <= 0 ||
	    unit->main->duration_ms <= 0)
		return LOOK_FOUND;
	for (i = 0; i < out->len; i++) {
		AxisIdentifySeriesCandidate *c = &g_array_index(out, AxisIdentifySeriesCandidate, i);
		AxisIdentifyFit fit;

		axis_identify_shape_fit(shape, c->title, &fit);
		if (!fit.fits)
			continue;
		look = episode_runtime(run, unit, c->title->id, unit->main->episodes.season, unit->main->episodes.numbers[0],
			&c->episode_runtime, error);
		if (look == LOOK_WAIT)
			return LOOK_WAIT;
	}
	return LOOK_FOUND;
}

/* Looks at one unit; %FALSE only for a database error.  run->stop says the
 * provider (or the volume) did not answer: then unit->verdict is %NULL. */
static gboolean
examine_unit(
	Run                    *run,
	Unit                   *unit,
	const AxisIdentifyName *folder,
	GError                **error
){
	g_autoptr(GPtrArray) found = g_ptr_array_new();
	g_autoptr(GPtrArray) details = g_ptr_array_new();
	g_autoptr(GArray) of_name = g_array_new(FALSE, TRUE, sizeof(AxisIdentifySeriesCandidate));
	g_auto(AxisIdentifyShape) shape = { 0 };
	const AxisIdentifyName *file_name;
	AxisIdentifyEvidence e;
	gboolean series = run->kind == AXIS_META_KIND_SERIES;
	guint answers = 0;
	Picked names;
	Picked nfo;
	Look look;
	guint i;

	memset(&e, 0, sizeof(e));
	memset(&names, 0, sizeof(names));
	memset(&nfo, 0, sizeof(nfo));
	e.kind = run->kind;
	e.percent = run->percent;
	e.minutes = run->minutes;
	e.duration_ms = unit->main->duration_ms;
	e.n_videos = unit->videos->len;
	e.not_read = unit->not_read;
	e.editions = unit->main->name->release->editions | (folder != NULL ? folder->release->editions : 0);
	/* A series' folder is read with the name its episode files share; when
	 * no episode file names a series, by its own name alone (a file such as
	 * "Lantern Row - Pilot Party" names an episode, not a title). */
	file_name = !series ? unit->main->name : unit->named != NULL ? unit->named->name :
		folder != NULL ? NULL : unit->main->name;
	axis_identify_read(folder, file_name, &e.reading);
	unit->read_text = read_words(run, unit, folder, file_name);
	if (e.reading.primary != NULL && e.reading.primary->release->title != NULL) {
		unit->read_title = axis_identify_text_cut(e.reading.primary->release->title, 200);
		unit->read_year = e.reading.year >= 1870 && e.reading.year <= 2200 ? e.reading.year : 0;
	}
	/* What a series' episode files hold, for its verdict (and, with no year,
	 * to check against each series of its name). */
	if (series) {
		series_shape(unit, e.reading.primary != NULL ? e.reading.primary->key : NULL, &shape);
		e.shape = &shape;
	}

	/* An import of Axis placed a file of it: that stands. */
	for (i = 0; i < unit->videos->len; i++) {
		const Video *video = g_ptr_array_index(unit->videos, i);
		const AxisMetaTitle *title = NULL;

		if (video->imported_title <= 0)
			continue;
		if (full_record(run, unit, video->imported_title, 0, &title, NULL, error) == LOOK_FOUND && title != NULL) {
			e.imported = title;
			break;
		}
		if (error != NULL && *error != NULL)
			return FALSE;
	}

	/* Identifiers in the names: the folder's and every video's. */
	if (e.imported == NULL) {
		if (folder != NULL)
			pick_merge(&names, &folder->ids, "the folder name", series);
		for (i = 0; i < unit->videos->len; i++) {
			const Video *video = g_ptr_array_index(unit->videos, i);

			pick_merge(&names, &video->name->ids, "the file name", series);
		}
		look = weigh_pick(run, unit, &names, &e.name_id, error);
		if (look == LOOK_WAIT) {
			if (error != NULL && *error != NULL)
				return FALSE;
			stop_run(run, "The details provider did not answer: identification goes on later.");
			return TRUE;
		}
	}

	/* A sidecar .nfo, when the names carry nothing that settles it (a
	 * film's: a series' tvshow.nfo is not read). */
	if (e.imported == NULL && !series && !axis_identify_id_decides(&e.name_id, &e.reading)) {
		AxisIdentifyIds ids;
		gboolean any = FALSE;

		if (!nfo_ids(run, unit, &ids, &any, error))
			return FALSE;
		if (run->stop)
			return TRUE;
		if (any) {
			pick_merge(&nfo, &ids, "the .nfo file", FALSE);
			look = weigh_pick(run, unit, &nfo, &e.nfo_id, error);
			if (look == LOOK_WAIT) {
				if (error != NULL && *error != NULL)
					return FALSE;
				stop_run(run, "The details provider did not answer: identification goes on later.");
				return TRUE;
			}
		}
	}

	/* The names, searched, when nothing above settles it. */
	if (e.imported == NULL && !axis_identify_id_decides(&e.name_id, &e.reading) &&
	    !(e.name_id.state == AXIS_IDENTIFY_ID_NONE && axis_identify_nfo_decides(&e.nfo_id, &e.reading))) {
		look = search_name(run, unit, e.reading.primary, 1, found, &answers, error);
		if (look != LOOK_WAIT && e.reading.kind == AXIS_IDENTIFY_READ_DISAGREE)
			look = search_name(run, unit, e.reading.secondary, 1, found, NULL, error);
		if (look == LOOK_WAIT) {
			if (error != NULL && *error != NULL)
				return FALSE;
			stop_run(run, "The details provider did not answer: identification goes on later.");
			return TRUE;
		}
		if (series && e.reading.kind != AXIS_IDENTIFY_READ_DISAGREE && e.reading.primary != NULL &&
		    e.reading.year == 0 && found->len > 0) {
			/* A series' folder with no year: every series of its name, with
			 * its seasons (rule 5). */
			look = series_candidates(run, unit, e.reading.primary, &shape, found, of_name, &e.series_more,
				&e.others_partial, &e.untold, &e.untold_problem, error);
			if (look == LOOK_WAIT) {
				if (error != NULL && *error != NULL)
					return FALSE;
				stop_run(run, "The details provider did not answer: identification goes on later.");
				return TRUE;
			}
			e.series = (const AxisIdentifySeriesCandidate *)(gpointer)of_name->data;
			e.n_series = of_name->len;
		} else if (e.reading.kind != AXIS_IDENTIFY_READ_DISAGREE && e.reading.primary != NULL && found->len > 0) {
			/* Every title of the name, in full (rule 4); the other names of
			 * the first answers when none has it as its title. */
			look = name_records(run, unit, e.reading.primary, answers, found, details, &e.details_more,
				&e.others_partial, &e.untold, &e.untold_problem, error);
			if (look == LOOK_WAIT) {
				if (error != NULL && *error != NULL)
					return FALSE;
				stop_run(run, "The details provider did not answer: identification goes on later.");
				return TRUE;
			}
			/* A series' episode file runs as the episode its name holds: that
			 * season of each series of the name within a year of the name's
			 * is fetched now (it is needed anyway once linked; one a year off
			 * is ruled out only by its runtime). */
			for (i = 0; series && e.untold == NULL && e.reading.year > 0 && unit->main->episodes.code == NULL &&
			     unit->main->episodes.n == 1 && unit->main->duration_ms > 0 && i < details->len; i++) {
				const AxisMetaTitle *detail = g_ptr_array_index(details, i);
				AxisIdentifySeriesCandidate c;
				gint years[3];
				guint n_years = axis_identify_title_years(detail, years);
				gboolean near = FALSE;
				guint k;

				for (k = 0; k < n_years; k++)
					near = near || ABS(years[k] - e.reading.year) <= 1;
				if (!near || !axis_identify_title_fits(e.reading.primary, detail) ||
				    of_name->len >= AXIS_IDENTIFY_DETAILS)
					continue;
				memset(&c, 0, sizeof(c));
				c.title = detail;
				look = episode_runtime(run, unit, detail->id, unit->main->episodes.season,
					unit->main->episodes.numbers[0], &c.episode_runtime, error);
				if (look == LOOK_WAIT) {
					if (error != NULL && *error != NULL)
						return FALSE;
					stop_run(run, "The details provider did not answer: identification goes on later.");
					return TRUE;
				}
				g_array_append_val(of_name, c);
			}
			e.series = (const AxisIdentifySeriesCandidate *)(gpointer)of_name->data;
			e.n_series = of_name->len;
		}
		/* (The second page of a search may have been added.) */
		e.found = (const AxisMetaTitle *const *)found->pdata;
		e.n_found = found->len;
		e.details = (const AxisMetaTitle *const *)details->pdata;
		e.n_details = details->len;
	}
	unit->verdict = axis_identify_decide(&e);
	/* A record not answered while the provider holds Axis off (told to wait,
	 * or backing off after failures): this folder waits, and the run stops
	 * once its folder is stored, rather than make every folder after it wait
	 * on its own; it goes on after the root's wait. */
	if (unit->verdict->waiting && provider_paused(run))
		run->halt = TRUE;
	/* Which of a series' files state another title than the unit was read
	 * as (checked against the series' names when shown). */
	if (series)
		mark_other_titles(unit, e.reading.primary != NULL ? e.reading.primary->key : NULL);
	return TRUE;
}

/* ── One scope ──────────────────────────────────────────────────── */

/* The scope's videos that count, with their probe and any import's link, at
 * most AXIS_IDENTIFY_MAX_VIDEOS of a film's folder and
 * AXIS_IDENTIFY_MAX_EPISODE_FILES of a series'; *@total: how many it has. */
static gboolean
load_videos(
	Run          *run,
	const gchar  *path,
	gboolean      loose,
	GPtrArray   **videos,
	guint        *total,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	g_autofree gchar *sql = NULL;
	gboolean series = run->kind == AXIS_META_KIND_SERIES;
	const gchar *params[2];
	guint i;

	params[0] = root;
	params[1] = path;
	*total = 0;
	if (!series)
		sql = g_strdup_printf("SELECT f.id, f.rel_path, f.base_name, f.size, mi.duration_ms, mi.resolution_class,"
			" (SELECT m.title_id FROM movie_file mf JOIN movie m ON m.id = mf.movie_id"
			"   WHERE mf.file_id = f.id AND mf.import_id IS NOT NULL), count(*) OVER ()"
			" FROM library_file f JOIN library_root r ON r.id = f.root_id"
			" LEFT JOIN media_info mi ON mi.file_id = f.id AND mi.status = 'ok'"
			" WHERE f.root_id = $1::bigint AND %s AND %s ORDER BY f.base_name, f.id LIMIT %d",
			loose ? "f.path_hash = library_path_hash($2)" : "f.dir_hash = library_path_hash($2)",
			axis_identify_counted_condition(), AXIS_IDENTIFY_MAX_VIDEOS);
	else
		/* A series' folder: every video below it, by the index of the files'
		 * paths within the root (the range of the folder's prefix). */
		sql = g_strdup_printf("SELECT f.id, f.rel_path, f.base_name, f.size, mi.duration_ms, mi.resolution_class,"
			" (SELECT s.title_id FROM series_file sf JOIN series s ON s.id = sf.series_id"
			"   WHERE sf.file_id = f.id AND sf.import_id IS NOT NULL), count(*) OVER ()"
			" FROM library_file f JOIN library_root r ON r.id = f.root_id"
			" LEFT JOIN media_info mi ON mi.file_id = f.id AND mi.status = 'ok'"
			" WHERE f.root_id = $1::bigint AND %s AND %s ORDER BY f.rel_path, f.id LIMIT %d",
			loose ? "f.path_hash = library_path_hash($2)" : "starts_with(f.rel_path, $2 || '/')"
			" AND left(f.rel_path, 600) COLLATE \"C\" >= left($2 || '/', 600)"
			" AND (length($2) >= 599 OR left(f.rel_path, 600) COLLATE \"C\" < $2 || '0')",
			axis_identify_counted_condition(), AXIS_IDENTIFY_MAX_EPISODE_FILES);
	if (!axis_db_pool_exec(run->self->pool, sql, 2, params, &rows, error))
		return FALSE;
	*videos = g_ptr_array_new_with_free_func((GDestroyNotify)video_free);
	if (axis_db_result_get_n_rows(rows) > 0)
		*total = (guint)CLAMP(axis_identify_row_int(rows, 0, 7, 0), 0, G_MAXINT32);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		Video *video = g_new0(Video, 1);
		g_autofree gchar *shown = NULL;

		video->file_id = axis_identify_row_int(rows, i, 0, 0);
		video->rel_path = axis_identify_row_text(rows, i, 1);
		video->base = axis_identify_row_text(rows, i, 2);
		video->size = axis_identify_row_int(rows, i, 3, 0);
		video->duration_ms = MAX(axis_identify_row_int(rows, i, 4, 0), 0);
		video->res_class = axis_identify_row_text(rows, i, 5);
		video->imported_title = axis_identify_row_int(rows, i, 6, 0);
		shown = axis_library_name_display(video->base != NULL ? video->base : "");
		video->name = axis_identify_name_read(shown, run->tuning.latest_year);
		video->folder_season = -1;
		if (series) {
			/* The season folder: the first folder below the series' own. */
			if (!loose && video->rel_path != NULL && strlen(video->rel_path) > strlen(path) + 1) {
				const gchar *below = video->rel_path + strlen(path) + 1;
				const gchar *slash = strchr(below, '/');

				if (slash != NULL) {
					g_autofree gchar *stored = g_strndup(below, (gsize)(slash - below));
					g_autofree gchar *folder = axis_library_name_display(stored);

					video->folder_season = axis_identify_season_folder(folder);
				}
			}
			axis_identify_episodes_read(video->name->release, video->folder_season, &video->episodes);
		}
		g_ptr_array_add(*videos, video);
	}
	return TRUE;
}

/* The longest by its probed duration; the largest when none is probed. */
static Video *
main_video(GPtrArray *videos)
{
	Video *best = NULL;
	guint i;

	for (i = 0; i < videos->len; i++) {
		Video *video = g_ptr_array_index(videos, i);

		if (best == NULL || video->duration_ms > best->duration_ms ||
		    (video->duration_ms == best->duration_ms && video->size > best->size))
			best = video;
	}
	return best;
}

/* The copy a film's upgrades are judged by: the best by quality, then the
 * largest. */
static void
mark_best(GPtrArray *videos)
{
	Video *best = NULL;
	guint i;

	for (i = 0; i < videos->len; i++) {
		Video *video = g_ptr_array_index(videos, i);
		gint order;

		video->best = FALSE;
		if (best == NULL) {
			best = video;
			continue;
		}
		order = axis_quality_compare_on_disk(video->quality.quality, video->quality.resolution, best->quality.quality,
			best->quality.resolution);
		if (order > 0 || (order == 0 && video->size > best->size))
			best = video;
	}
	if (best != NULL)
		best->best = TRUE;
}

typedef struct
{
	gint64   id;
	gchar   *rel_path;
	gchar   *state;
	gboolean held;
	gint64   title_id;
	gchar   *how;
	gchar   *decided_by;
	gint     tries;	/* its waits for the provider so far */
	gint     rules;	/* the version of the rules that decided it */
	gboolean matched;
} Existing;

static void
existing_free(Existing *existing)
{
	g_free(existing->rel_path);
	g_free(existing->state);
	g_free(existing->how);
	g_free(existing->decided_by);
	g_free(existing);
}

static gchar *
candidates_json(const AxisIdentifyVerdict *verdict)
{
	g_autoptr(GString) json = g_string_new("[");
	guint i;

	for (i = 0; verdict != NULL && i < verdict->candidates->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(verdict->candidates, i);

		if (title->id <= 0)
			continue;
		g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT "}", json->len > 1 ? "," : "", title->id);
	}
	g_string_append_c(json, ']');
	return g_string_free(g_steal_pointer(&json), FALSE);
}

static void
set_text(
	JsonBuilder *builder,
	const gchar *name,
	const gchar *value
){
	json_builder_set_member_name(builder, name);
	if (value != NULL)
		json_builder_add_string_value(builder, value);
	else
		json_builder_add_null_value(builder);
}

/* A series' unit's videos, each with what its name holds, in two
 * statements whatever their number (the rows as one JSON document each). */
static gboolean
store_episode_files(
	AxisDbConn  *conn,
	gint64       identity_id,
	Unit        *unit,
	GError     **error
){
	g_autoptr(JsonBuilder) files = json_builder_new();
	g_autoptr(JsonBuilder) episodes = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) files_root = NULL;
	g_autoptr(JsonNode) episodes_root = NULL;
	g_autofree gchar *files_json = NULL;
	g_autofree gchar *episodes_json = NULL;
	g_autofree gchar *id = axis_identify_int_text(identity_id);
	const gchar *params[2];
	guint i;
	guint k;

	json_builder_begin_array(files);
	json_builder_begin_array(episodes);
	for (i = 0; i < unit->videos->len; i++) {
		const Video *video = g_ptr_array_index(unit->videos, i);
		g_autofree gchar *quality_text = axis_identify_text_cut(video->quality.text, 1000);
		g_autofree gchar *episode_text = axis_identify_text_cut(video->episodes.text, 1000);

		json_builder_begin_object(files);
		json_builder_set_member_name(files, "f");
		json_builder_add_int_value(files, video->file_id);
		set_text(files, "q", video->quality.quality != AXIS_QUALITY_UNKNOWN ? axis_quality_name(video->quality.quality) :
			NULL);
		set_text(files, "res", video->quality.quality == AXIS_QUALITY_UNKNOWN ?
			axis_release_resolution_name(video->quality.resolution) : NULL);
		set_text(files, "qt", quality_text);
		set_text(files, "c", video->episodes.code);
		set_text(files, "t", episode_text);
		set_text(files, "k", video->title_key);
		set_text(files, "n", video->name_title);
		json_builder_end_object(files);
		if (video->episodes.code != NULL)
			continue;
		if (video->episodes.date[0] != '\0') {
			json_builder_begin_object(episodes);
			json_builder_set_member_name(episodes, "f");
			json_builder_add_int_value(episodes, video->file_id);
			json_builder_set_member_name(episodes, "p");
			json_builder_add_int_value(episodes, 0);
			set_text(episodes, "d", video->episodes.date);
			json_builder_end_object(episodes);
			continue;
		}
		for (k = 0; k < video->episodes.n; k++) {
			json_builder_begin_object(episodes);
			json_builder_set_member_name(episodes, "f");
			json_builder_add_int_value(episodes, video->file_id);
			json_builder_set_member_name(episodes, "p");
			json_builder_add_int_value(episodes, k);
			json_builder_set_member_name(episodes, "s");
			json_builder_add_int_value(episodes, video->episodes.season);
			json_builder_set_member_name(episodes, "e");
			json_builder_add_int_value(episodes, video->episodes.numbers[k]);
			json_builder_end_object(episodes);
		}
	}
	json_builder_end_array(files);
	json_builder_end_array(episodes);
	files_root = json_builder_get_root(files);
	episodes_root = json_builder_get_root(episodes);
	json_generator_set_root(generator, files_root);
	files_json = json_generator_to_data(generator, NULL);
	json_generator_set_root(generator, episodes_root);
	episodes_json = json_generator_to_data(generator, NULL);
	params[0] = id;
	params[1] = files_json;
	if (!axis_db_conn_exec(conn, "WITH rows AS (SELECT * FROM jsonb_to_recordset($2::jsonb) AS x (f bigint, q text,"
	        " res text, qt text, c text, t text, k text, n text))"
	        " INSERT INTO library_identity_file (file_id, identity_id, best, quality, resolution, quality_text,"
	        " episode_code, episode_text, title_key, name_title)"
	        " SELECT x.f, $1::bigint, false, x.q, x.res, x.qt, x.c, x.t, x.k, x.n FROM rows x"
	        " ON CONFLICT (file_id) DO UPDATE SET identity_id = EXCLUDED.identity_id, best = EXCLUDED.best,"
	        " quality = EXCLUDED.quality, resolution = EXCLUDED.resolution, quality_text = EXCLUDED.quality_text,"
	        " episode_code = EXCLUDED.episode_code, episode_text = EXCLUDED.episode_text,"
	        " title_key = EXCLUDED.title_key, name_title = EXCLUDED.name_title", 2, params, NULL, error))
		return FALSE;
	/* What their names hold (a file taken over from another unit loses
	 * what was read for it there). */
	params[1] = episodes_json;
	return axis_db_conn_exec(conn, "DELETE FROM library_identity_episode WHERE file_id IN (SELECT file_id FROM"
		" library_identity_file WHERE identity_id = $1::bigint)", 1, params, NULL, error) &&
		axis_db_conn_exec(conn, "INSERT INTO library_identity_episode (file_id, position, season_number, number,"
			" air_date) SELECT x.f, x.p, x.s, x.e, x.d FROM jsonb_to_recordset($2::jsonb) AS x (f bigint, p smallint,"
			" s integer, e integer, d date) WHERE $1::bigint IS NOT NULL", 2, params, NULL, error);
}

/* At most this many characters of a reason in the line that says why a link
 * is now uncertain (the column holds 1,000). */
#define DOUBT_CAUSE_CHARS 600

/*
 * Why a link made before is listed for a person: who made it, under which
 * rules, and what the look again found ("linked under earlier rules; now
 * uncertain (the reason)").
 */
static gchar *
doubt_text(
	const Existing            *existing,
	const AxisIdentifyVerdict *v
){
	const gchar *cause = axis_identify_doubt_cause(v->reasons);
	g_autofree gchar *found = NULL;

	if (v->confidence == AXIS_IDENTIFY_SURE && v->title != NULL)
		found = v->title->year > 0 ? g_strdup_printf("the evidence now points to %s (%d)", v->title->title,
			v->title->year) : g_strdup_printf("the evidence now points to %s", v->title->title);
	else if (cause != NULL && g_utf8_strlen(cause, -1) > DOUBT_CAUSE_CHARS) {
		g_autofree gchar *cut = g_utf8_substring(cause, 0, DOUBT_CAUSE_CHARS);

		found = g_strdup_printf("it is now uncertain (%s...)", cut);
	} else if (cause != NULL)
		found = g_strdup_printf("it is now uncertain (%s)", cause);
	else
		found = g_strdup("the evidence no longer makes it sure");
	return g_strdup_printf("%s%s; looked at again, %s, so the link stands until a person decides",
		g_strcmp0(existing->how, "imported") == 0 ? "An import of Axis linked it" : "It was linked by itself",
		existing->rules < AXIS_IDENTIFY_RULES_VERSION ? " under earlier rules" : "", found);
}

/* One unit's row, files and the owner's links, in the scope's transaction. */
static gboolean
store_unit(
	Run         *run,
	AxisDbConn  *conn,
	const gchar *scope,
	Unit        *unit,
	Existing    *existing,
	GError     **error
){
	const AxisIdentifyVerdict *v = unit->verdict;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	g_autofree gchar *reasons = axis_identify_reasons_to_json(v->reasons);
	g_autofree gchar *candidates = candidates_json(v);
	g_autofree gchar *duration = unit->main->duration_ms > 0 ? axis_identify_int_text(unit->main->duration_ms) : NULL;
	g_autofree gchar *year = unit->read_year > 0 ? g_strdup_printf("%d", unit->read_year) : NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *title_text = NULL;
	g_autofree gchar *rules = g_strdup_printf("%d", AXIS_IDENTIFY_RULES_VERSION);
	g_autofree gchar *tries_text = NULL;
	g_autofree gchar *wait_text = NULL;
	const gchar *state;
	const gchar *how = NULL;
	const gchar *user = NULL;
	gint64 title_id = 0;
	gint64 id;
	g_autofree gchar *doubt = NULL;
	gint tries = 0;
	gboolean was_linked = existing != NULL && g_strcmp0(existing->state, "linked") == 0;
	gboolean imported = v->confidence == AXIS_IDENTIFY_SURE && v->title != NULL && v->title->id > 0 &&
		v->reasons->len > 0 && g_strcmp0(((AxisIdentifyReason *)g_ptr_array_index(v->reasons, 0))->code,
		"imported") == 0;
	gboolean kept = FALSE;
	const gchar *params[18];
	guint i;

	/* What it is now: an import's link wins; then a person's decision stands;
	 * then a link made before stands (only a person changes a link: a
	 * verdict that does not confirm it lists it for a person, one not told
	 * yet leaves it as it was); then a verdict that could not be told yet
	 * waits; then only a sure match links. */
	if (imported) {
		state = "linked";
		how = "imported";
		title_id = v->title->id;
	} else if (existing != NULL && !existing->held && was_linked && existing->title_id > 0 &&
	           (v->waiting || v->confidence != AXIS_IDENTIFY_SURE || v->title == NULL ||
	            v->title->id != existing->title_id)) {
		state = "linked";
		how = existing->how;
		title_id = existing->title_id;
		kept = TRUE;
		if (!v->waiting)
			doubt = doubt_text(existing, v);
	} else if (existing != NULL && existing->held) {
		state = existing->state;
		how = existing->how;
		title_id = existing->title_id;
		user = existing->decided_by;
		if (g_strcmp0(state, "linked") == 0 && title_id <= 0) {
			state = "review";
			how = NULL;
		}
		if (g_strcmp0(state, "waiting") == 0)
			state = "review";
	} else if (v->waiting) {
		state = "waiting";
	} else if (v->confidence == AXIS_IDENTIFY_SURE && v->title != NULL && v->title->id > 0) {
		state = "linked";
		how = "auto";
		title_id = v->title->id;
	} else {
		state = "review";
	}
	title_text = title_id > 0 ? axis_identify_int_text(title_id) : NULL;
	/* A wait for the provider (a unit waiting, or a link kept while its
	 * look could not be told): looked at again after a delay that doubles
	 * with each wait, from AXIS_IDENTIFY_RETRY_S up to a day. */
	if (v->waiting && (kept || g_strcmp0(state, "waiting") == 0)) {
		tries = (existing != NULL ? MIN(existing->tries, 999) : 0) + 1;
		wait_text = g_strdup_printf("%d", (gint)MIN((gint64)AXIS_IDENTIFY_RETRY_S << MIN(tries - 1, 10),
			(gint64)AXIS_IDENTIFY_WAIT_MAX_S));
	}
	tries_text = g_strdup_printf("%d", tries);

	params[0] = root;
	params[1] = scope;
	params[2] = unit->folder ? "folder" : "file";
	params[3] = unit->rel_path;
	params[4] = state;
	params[5] = axis_identify_confidence_name(v->confidence);
	params[6] = g_strcmp0(state, "linked") == 0 ? title_text : NULL;
	params[7] = g_strcmp0(state, "linked") == 0 ? how : NULL;
	params[8] = reasons;
	params[9] = candidates;
	params[10] = unit->read_text;
	params[11] = unit->read_title;
	params[12] = year;
	params[13] = duration;
	params[14] = rules;
	params[15] = wait_text;
	params[16] = tries_text;
	params[17] = doubt;
	if (existing != NULL && v->waiting && (kept || existing->held)) {
		/* Not told yet: the link (or the person's decision) and what was
		 * said of it stay as they were; only the wait is recorded (and that
		 * these rules looked). */
		const gchar *wp[4];

		id_text = axis_identify_int_text(existing->id);
		wp[0] = id_text;
		wp[1] = rules;
		wp[2] = wait_text;
		wp[3] = tries_text;
		if (!axis_db_conn_exec(conn, "UPDATE library_identity SET rules = $2::smallint,"
		        " retry_at = now() + make_interval(secs => $3::integer), tries = $4::smallint, examined_at = now()"
		        " WHERE id = $1::bigint", 4, wp, NULL, error))
			return FALSE;
		id = existing->id;
	} else if (existing != NULL) {
		id_text = axis_identify_int_text(existing->id);
		params[1] = id_text;
		if (!axis_db_conn_exec(conn, "UPDATE library_identity SET unit_kind = $3, state = $5, confidence = $6,"
		        " title_id = $7::bigint, linked_how = $8, reasons = $9::jsonb, candidates = $10::jsonb, read_text = $11,"
		        " read_title = $12, read_year = $13::integer, duration_ms = $14::bigint, problem = NULL,"
		        " rules = $15::smallint, retry_at = now() + make_interval(secs => $16::integer), tries = $17::smallint,"
		        " doubt = $18, examined_at = now() WHERE id = $2::bigint AND root_id = $1::bigint AND rel_path = $4", 18,
		        params, NULL, error))
			return FALSE;
		id = existing->id;
	} else {
		if (!axis_db_conn_exec(conn, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state,"
		        " confidence, title_id, linked_how, reasons, candidates, read_text, read_title, read_year, duration_ms,"
		        " rules, retry_at, tries) VALUES ($1::bigint, library_path_hash($2), $3, $4, $5, $6, $7::bigint, $8,"
		        " $9::jsonb, $10::jsonb, $11, $12, $13::integer, $14::bigint, $15::smallint,"
		        " now() + make_interval(secs => $16::integer), $17::smallint) RETURNING id", 17, params, &inserted,
		        error))
			return FALSE;
		id = axis_identify_row_int(inserted, 0, 0, 0);
		id_text = axis_identify_int_text(id);
	}

	/* Its videos (a series' at once, with what each name holds). */
	if (run->kind == AXIS_META_KIND_SERIES && !store_episode_files(conn, id, unit, error))
		return FALSE;
	for (i = 0; run->kind != AXIS_META_KIND_SERIES && i < unit->videos->len; i++) {
		const Video *video = g_ptr_array_index(unit->videos, i);
		g_autofree gchar *file = axis_identify_int_text(video->file_id);
		g_autofree gchar *text = axis_identify_text_cut(video->quality.text, 1000);
		const gchar *fp[6];

		fp[0] = file;
		fp[1] = id_text;
		fp[2] = video->best ? "true" : "false";
		fp[3] = video->quality.quality != AXIS_QUALITY_UNKNOWN ? axis_quality_name(video->quality.quality) : NULL;
		fp[4] = video->quality.quality == AXIS_QUALITY_UNKNOWN ?
			axis_release_resolution_name(video->quality.resolution) : NULL;
		fp[5] = text;
		if (!axis_db_conn_exec(conn, "INSERT INTO library_identity_file (file_id, identity_id, best, quality, resolution,"
		        " quality_text) VALUES ($1::bigint, $2::bigint, $3::boolean, $4, $5, $6) ON CONFLICT (file_id) DO UPDATE"
		        " SET identity_id = EXCLUDED.identity_id, best = EXCLUDED.best, quality = EXCLUDED.quality,"
		        " resolution = EXCLUDED.resolution, quality_text = EXCLUDED.quality_text", 6, fp, NULL, error))
			return FALSE;
	}

	/* The owner's links follow. */
	if (g_strcmp0(state, "linked") == 0) {
		if (!axis_identify_owner_link(run->self, run->kind, conn, id, title_id, user, error))
			return FALSE;
		if (run->report != NULL && kept && v->waiting)
			run->report->waiting++;
		else if (run->report != NULL && kept)
			run->report->review++;
		else if (run->report != NULL && !(existing != NULL && existing->held))
			run->report->linked++;
	} else {
		if (was_linked && !axis_identify_owner_unlink(run->self, run->kind, conn, id, NULL, error))
			return FALSE;
		if (run->report != NULL && g_strcmp0(state, "review") == 0)
			run->report->review++;
		if (run->report != NULL && g_strcmp0(state, "waiting") == 0)
			run->report->waiting++;
	}
	return TRUE;
}

static gboolean
store_scope(
	Run         *run,
	const gchar *scope,
	const gchar *fingerprint,
	GPtrArray   *units,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GPtrArray) existing = g_ptr_array_new_with_free_func((GDestroyNotify)existing_free);
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	const gchar *params[3];
	AxisDbConn *conn;
	gboolean ok;
	guint i;
	guint k;

	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = root;
	params[1] = scope;
	params[2] = fingerprint;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "SELECT id, rel_path, state, held, title_id, linked_how, decided_by, tries, rules"
			" FROM library_identity WHERE root_id = $1::bigint AND scope_hash = library_path_hash($2) ORDER BY id"
			" FOR UPDATE", 2, params, &rows, error);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		Existing *e = g_new0(Existing, 1);

		e->id = axis_identify_row_int(rows, i, 0, 0);
		e->rel_path = axis_identify_row_text(rows, i, 1);
		e->state = axis_identify_row_text(rows, i, 2);
		axis_db_result_get_boolean(rows, i, 3, &e->held);
		e->title_id = axis_identify_row_int(rows, i, 4, 0);
		e->how = axis_identify_row_text(rows, i, 5);
		e->decided_by = axis_identify_row_text(rows, i, 6);
		e->tries = (gint)axis_identify_row_int(rows, i, 7, 0);
		e->rules = (gint)axis_identify_row_int(rows, i, 8, 1);
		g_ptr_array_add(existing, e);
	}
	/* The scope's videos are written again, unit by unit. */
	ok = ok && axis_db_conn_exec(conn, "DELETE FROM library_identity_file WHERE identity_id IN (SELECT id FROM"
		" library_identity WHERE root_id = $1::bigint AND scope_hash = library_path_hash($2))", 2, params, NULL, error);
	for (i = 0; ok && i < units->len; i++) {
		Unit *unit = g_ptr_array_index(units, i);
		Existing *match = NULL;

		for (k = 0; k < existing->len; k++) {
			Existing *e = g_ptr_array_index(existing, k);

			if (!e->matched && strcmp(e->rel_path, unit->rel_path) == 0) {
				match = e;
				e->matched = TRUE;
				break;
			}
		}
		ok = store_unit(run, conn, scope, unit, match, error);
	}
	/* Units the scope no longer has go, with the owner's links. */
	for (i = 0; ok && i < existing->len; i++) {
		Existing *e = g_ptr_array_index(existing, i);
		g_autofree gchar *id = NULL;
		const gchar *one[1];

		if (e->matched)
			continue;
		id = axis_identify_int_text(e->id);
		one[0] = id;
		ok = axis_identify_owner_unlink(run->self, run->kind, conn, e->id, NULL, error) &&
			axis_db_conn_exec(conn, "DELETE FROM library_identity WHERE id = $1::bigint", 1, one, NULL, error);
	}
	ok = ok && axis_db_conn_exec(conn, "INSERT INTO library_identity_scope (root_id, path, fingerprint)"
		" VALUES ($1::bigint, $2, $3) ON CONFLICT (root_id, path_hash) DO UPDATE SET fingerprint = EXCLUDED.fingerprint,"
		" examined_at = now()", 3, params, NULL, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	return ok;
}

/* The units of one scope, looked at and stored; *@done is %FALSE when the run
 * stopped before it could store them. */
static gboolean
examine_scope(
	Run         *run,
	const gchar *path,
	gboolean     loose,
	const gchar *fingerprint,
	gboolean    *done,
	GError     **error
){
	g_autoptr(GPtrArray) videos = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(GPtrArray) units = g_ptr_array_new_with_free_func((GDestroyNotify)unit_free);
	g_autoptr(AxisIdentifyName) folder = NULL;
	g_autofree const AxisIdentifyName **names = NULL;
	guint total = 0;
	guint not_read;
	guint i;
	guint k;

	*done = FALSE;
	if (!load_videos(run, path, loose, &videos, &total, error))
		return FALSE;
	if (videos->len == 0) {
		/* Gone meanwhile: the next run forgets it. */
		*done = TRUE;
		return TRUE;
	}
	/* More videos than are read of one folder: the rest are in no unit, and
	 * every unit of the folder says so (the folder's own is held). */
	not_read = total > videos->len ? total - videos->len : 0;
	if (not_read > 0) {
		g_autofree gchar *shown = axis_library_name_display(path);
		g_autofree gchar *cut = axis_identify_text_cut(shown, 300);

		g_message("identify: the folder '%s' of root %" G_GINT64_FORMAT " holds %u videos; only the first %u are "
			"identified, the other %u are in no unit", cut != NULL ? cut : "", run->root_id, total, videos->len, not_read);
	}
	if (!loose) {
		g_autofree gchar *base = g_path_get_basename(path);
		g_autofree gchar *shown = axis_library_name_display(base);

		folder = axis_identify_name_read(shown, run->tuning.latest_year);
	}
	names = g_new0(const AxisIdentifyName *, videos->len);
	for (i = 0; i < videos->len; i++)
		names[i] = ((Video *)g_ptr_array_index(videos, i))->name;
	if (run->kind == AXIS_META_KIND_SERIES) {
		/* A series' folder is one unit, whatever its files say (a file
		 * that states another title is held by itself, not split off); a
		 * video at the top of the root is one of its own. */
		Unit *unit = g_new0(Unit, 1);
		gint named = axis_identify_series_name(names, videos->len);

		unit->folder = !loose;
		unit->videos = g_ptr_array_new();
		unit->held = g_ptr_array_new_with_free_func((GDestroyNotify)held_free);
		for (k = 0; k < videos->len; k++)
			g_ptr_array_add(unit->videos, g_ptr_array_index(videos, k));
		unit->main = series_main(unit->videos);
		unit->named = named >= 0 ? g_ptr_array_index(videos, named) : NULL;
		unit->rel_path = g_strdup(unit->folder ? path : ((Video *)g_ptr_array_index(unit->videos, 0))->rel_path);
		unit->not_read = not_read;
		g_ptr_array_add(units, unit);
	} else {
		groups = axis_identify_group(folder, names, videos->len);
	}
	for (i = 0; groups != NULL && i < groups->len; i++) {
		const AxisIdentifyGroup *group = g_ptr_array_index(groups, i);
		Unit *unit = g_new0(Unit, 1);

		unit->folder = group->folder && !loose;
		unit->videos = g_ptr_array_new();
		unit->held = g_ptr_array_new_with_free_func((GDestroyNotify)held_free);
		for (k = 0; k < group->members->len; k++)
			g_ptr_array_add(unit->videos, g_ptr_array_index(videos, g_array_index(group->members, guint, k)));
		unit->main = main_video(unit->videos);
		unit->rel_path = g_strdup(unit->folder ? path : ((Video *)g_ptr_array_index(unit->videos, 0))->rel_path);
		unit->not_read = not_read;
		g_ptr_array_add(units, unit);
	}
	for (i = 0; i < units->len && !run->stop; i++) {
		Unit *unit = g_ptr_array_index(units, i);

		if (!examine_unit(run, unit, unit->folder ? folder : NULL, error))
			return FALSE;
		if (run->stop)
			break;
		for (k = 0; k < unit->videos->len; k++) {
			Video *video = g_ptr_array_index(unit->videos, k);

			axis_identify_quality_clear(&video->quality);
			/* A series' folder names the series, not a quality. */
			axis_identify_file_quality(video->name->release, unit->folder && folder != NULL &&
				run->kind != AXIS_META_KIND_SERIES ? folder->release : NULL, video->res_class, &video->quality);
		}
		/* A film's copy its upgrades are judged by; a series' episodes are
		 * judged each by its own files. */
		if (run->kind != AXIS_META_KIND_SERIES)
			mark_best(unit->videos);
		if (run->report != NULL)
			run->report->units++;
	}
	if (run->stop) {
		if (run->report != NULL)
			run->report->waiting++;
		return TRUE;
	}
	if (!store_scope(run, path, fingerprint, units, error))
		return FALSE;
	*done = TRUE;
	return TRUE;
}

/* ── The scopes of a root ───────────────────────────────────────── */

/* The scope of a counted video: its directory, or itself at the top; in a
 * root for TV, the folder at the top it is in (a series'), or itself. */
#define SCOPE_OF "(CASE WHEN strpos(f.rel_path, '/') = 0 THEN f.rel_path ELSE library_dir_of(f.rel_path) END)"
#define SCOPE_OF_SERIES "(CASE WHEN strpos(f.rel_path, '/') = 0 THEN f.rel_path ELSE split_part(f.rel_path, '/', 1) END)"

static const gchar *
scope_of(const Run *run)
{
	return run->kind == AXIS_META_KIND_SERIES ? SCOPE_OF_SERIES : SCOPE_OF;
}

/* What a scope's fingerprint is made of, for each of its videos: a film
 * folder's names and sizes (its files are side by side), a series' paths
 * within it (its season folders count) and sizes; each with whether it is
 * probed. */
static const gchar *
fingerprint_of(const Run *run)
{
	return run->kind == AXIS_META_KIND_SERIES ?
		"f.rel_path || '/' || f.size::text || '/' || (mi.duration_ms IS NOT NULL)::text" :
		"f.base_name || '/' || f.size::text || '/' || (mi.duration_ms IS NOT NULL)::text";
}

static gboolean
forget_gone(
	Run      *run,
	guint    *removed,
	GError  **error
){
	g_autoptr(AxisDbResult) gone = NULL;
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	g_autofree gchar *sql = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok;

	*removed = 0;
	params[0] = root;
	sql = g_strdup_printf("WITH cur AS (SELECT DISTINCT library_path_hash(%s) AS h FROM library_file f"
		" JOIN library_root r ON r.id = f.root_id WHERE f.root_id = $1::bigint AND %s)"
		" DELETE FROM library_identity_scope s WHERE s.root_id = $1::bigint"
		" AND NOT EXISTS (SELECT 1 FROM cur WHERE cur.h = s.path_hash) RETURNING s.path_hash", scope_of(run),
		axis_identify_counted_condition());
	conn = axis_db_pool_acquire(run->self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The units of a scope that went go with it (and, through movie_file's
	 * key, the links made from them).  So do units of a scope never stored. */
	ok = axis_db_conn_begin(conn, error) && axis_db_conn_exec(conn, sql, 1, params, &gone, error) &&
		axis_db_conn_exec(conn, "DELETE FROM library_identity i WHERE i.root_id = $1::bigint AND NOT EXISTS"
			" (SELECT 1 FROM library_identity_scope s WHERE s.root_id = i.root_id AND s.path_hash = i.scope_hash)", 1,
			params, NULL, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(run->self->pool, conn);
	if (ok)
		*removed = axis_db_result_get_n_rows(gone);
	return ok;
}

static gboolean
cancelled(
	Run     *run,
	GError **error
){
	if (run->context == NULL || !axis_job_context_is_cancelled(run->context))
		return FALSE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "identification was cancelled");
	return TRUE;
}

/* Where the root is, and what the scheduler compares next time: the scan
 * generation and the database's time when this run began. */
static gboolean
record_root(
	Run         *run,
	guint        scopes,
	guint        pending,
	const gchar *text,
	gboolean     retry,
	GError     **error
){
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	g_autofree gchar *s = g_strdup_printf("%u", scopes);
	g_autofree gchar *p = g_strdup_printf("%u", pending);
	g_autofree gchar *wait = g_strdup_printf("%d", AXIS_IDENTIFY_RETRY_S);
	g_autofree gchar *cut = axis_identify_text_cut(text, 1000);
	g_autofree gchar *generation = axis_identify_int_text(run->scan_generation);
	g_autofree gchar *seasons = g_strdup_printf("%u", run->report != NULL ? run->report->seasons_pending : 0);
	const gchar *params[9];

	params[0] = root;
	params[1] = s;
	params[2] = p;
	params[3] = cut;
	params[4] = retry ? "true" : "false";
	params[5] = wait;
	params[6] = generation;
	params[7] = run->started;
	params[8] = seasons;
	return axis_db_pool_exec(run->self->pool, "INSERT INTO library_identity_root (root_id, scopes, pending, last_run_at,"
		" last_run_text, retry_after, scan_generation, run_started_at, seasons_pending) VALUES ($1::bigint,"
		" $2::integer, $3::integer, now(), $4, CASE WHEN $5::boolean THEN now() + make_interval(secs => $6::integer) END,"
		" $7::bigint, coalesce($8::timestamptz, now()), $9::integer)"
		" ON CONFLICT (root_id) DO UPDATE SET scopes = EXCLUDED.scopes, pending = EXCLUDED.pending,"
		" last_run_at = EXCLUDED.last_run_at, last_run_text = EXCLUDED.last_run_text,"
		" retry_after = EXCLUDED.retry_after, scan_generation = EXCLUDED.scan_generation,"
		" run_started_at = EXCLUDED.run_started_at, seasons_pending = EXCLUDED.seasons_pending", 9, params, NULL,
		error);
}

/*
 * A root for TV: the seasons its linked series need, fetched now when they
 * never were, within the run's time and AXIS_IDENTIFY_SEASONS_PER_RUN: first
 * the full records of series linked by a person from a search result (their
 * seasons are not known yet), then every season of every linked series whose
 * episodes were never fetched.  Each is asked once (the metadata layer keeps
 * it, and its refresh keeps it fresh); a season the provider does not answer
 * for ends the run, one it says does not exist is left (the next run asks
 * the store again, which answers from its negative answer).
 * report->seasons_pending: those still to fetch.
 */
static gboolean
fetch_seasons(
	Run     *run,
	GError **error
){
	g_autoptr(AxisDbResult) titles = NULL;
	g_autoptr(AxisDbResult) seasons = NULL;
	g_autoptr(AxisDbResult) left = NULL;
	g_autofree gchar *root = axis_identify_int_text(run->root_id);
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_IDENTIFY_SEASONS_PER_RUN);
	const gchar *params[2];
	guint attempted = 0;
	gint64 unfetched = 0;
	guint i;

	params[0] = root;
	params[1] = limit;
	if (!axis_db_pool_exec(run->self->pool, "SELECT t.id FROM meta_title t WHERE t.kind = 'series'"
	        " AND t.detail_fetched_at IS NULL AND t.id IN (SELECT i.title_id FROM library_identity i"
	        " WHERE i.root_id = $1::bigint AND i.state = 'linked' AND i.title_id IS NOT NULL)"
	        " ORDER BY t.id LIMIT $2::integer", 2, params, &titles, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(titles) && !run->stop; i++) {
		g_autoptr(GError) local = NULL;
		AxisMetaTitleAnswer answer;

		if (g_get_monotonic_time() > run->deadline_us || attempted >= AXIS_IDENTIFY_SEASONS_PER_RUN)
			break;
		memset(&answer, 0, sizeof(answer));
		attempted++;
		if (!axis_meta_get_title(run->self->meta, axis_identify_row_int(titles, i, 0, 0), run->tuning.wait_ms,
		        &answer, &local)) {
			if (local != NULL && local->domain != AXIS_META_ERROR) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			continue;
		}
		if (answer.title == NULL && not_answered(answer.saved, answer.fetched_us))
			stop_run(run, "The details provider did not answer: identification goes on later.");
		axis_meta_title_answer_clear(&answer);
	}
	/* The seasons of the source each series is followed in. */
	if (!axis_db_pool_exec(run->self->pool, "SELECT ms.title_id, ms.number, ms.source FROM meta_season_followed ms"
	        " JOIN meta_title t ON t.id = ms.title_id AND t.kind = 'series'"
	        " WHERE ms.episodes_fetched_at IS NULL AND ms.title_id IN (SELECT i.title_id FROM library_identity i"
	        "  WHERE i.root_id = $1::bigint AND i.state = 'linked' AND i.title_id IS NOT NULL)"
	        " ORDER BY ms.title_id, ms.number DESC LIMIT $2::integer", 2, params, &seasons, error) ||
	    !axis_db_pool_exec(run->self->pool, "SELECT count(*) FROM meta_season_followed ms"
	        " WHERE ms.episodes_fetched_at IS NULL"
	        " AND ms.title_id IN (SELECT i.title_id FROM library_identity i WHERE i.root_id = $1::bigint"
	        " AND i.state = 'linked' AND i.title_id IS NOT NULL)", 1, params, &left, error))
		return FALSE;
	unfetched = axis_identify_row_int(left, 0, 0, 0);
	for (i = 0; i < axis_db_result_get_n_rows(seasons) && !run->stop; i++) {
		g_autoptr(GError) local = NULL;
		AxisMetaSeasonAnswer answer;

		if (g_get_monotonic_time() > run->deadline_us || attempted >= AXIS_IDENTIFY_SEASONS_PER_RUN)
			break;
		if (cancelled(run, error))
			return FALSE;
		memset(&answer, 0, sizeof(answer));
		attempted++;
		if (!axis_meta_get_source_season(run->self->meta, axis_identify_row_int(seasons, i, 0, 0),
		        axis_db_result_get_text(seasons, i, 2), (gint)axis_identify_row_int(seasons, i, 1, 0),
		        run->tuning.wait_ms, &answer, &local)) {
			if (local != NULL && local->domain != AXIS_META_ERROR) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			unfetched = MAX(unfetched - 1, 0);
			continue;
		}
		if (answer.season != NULL && answer.season->episodes_fetched_us > 0) {
			run->report->seasons++;
			unfetched = MAX(unfetched - 1, 0);
		} else if (not_answered(answer.saved, answer.fetched_us) && !answer.not_found) {
			stop_run(run, "The details provider did not answer: identification goes on later.");
		} else {
			/* Not to be had now: not asked for again by this run's count. */
			unfetched = MAX(unfetched - 1, 0);
		}
		axis_meta_season_answer_clear(&answer);
	}
	run->report->seasons_pending = (guint)MIN(unfetched, (gint64)G_MAXINT32);
	return TRUE;
}

gboolean
axis_identify_run_root(
	AxisIdentify        *self,
	gint64               root_id,
	AxisJobContext      *context,
	AxisIdentifyReport  *report,
	GError             **error
){
	g_autoptr(AxisDbResult) root = NULL;
	g_autoptr(AxisDbResult) scopes = NULL;
	g_autoptr(AxisDbResult) count = NULL;
	g_autofree gchar *root_text = axis_identify_int_text(root_id);
	g_autofree gchar *limit = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *text = NULL;
	AxisIdentifyReport local_report;
	const gchar *params[2];
	gint64 per_run;
	guint total = 0;
	guint changed = 0;
	guint i;
	Run run;
	gboolean ok = TRUE;

	if (report == NULL)
		report = &local_report;
	memset(report, 0, sizeof(*report));
	memset(&run, 0, sizeof(run));
	run.self = self;
	run.root_id = root_id;
	run.context = context;
	run.report = report;
	axis_identify_get_tuning(self, &run.tuning);
	run.percent = (gint)axis_identify_setting_integer(self, AXIS_IDENTIFY_SETTING_PERCENT, AXIS_IDENTIFY_DEFAULT_PERCENT);
	run.minutes = (gint)axis_identify_setting_integer(self, AXIS_IDENTIFY_SETTING_MINUTES, AXIS_IDENTIFY_DEFAULT_MINUTES);
	per_run = axis_identify_setting_integer(self, AXIS_IDENTIFY_SETTING_PER_RUN, 25);
	run.deadline_us = g_get_monotonic_time() + (gint64)run.tuning.budget_ms * 1000;

	params[0] = root_text;
	if (!axis_db_pool_exec(self->pool, "SELECT r.volume_id, r.rel_path, r.media_type, coalesce(s.complete_generation, 0),"
	        " ir.retry_after > now(), now()::text FROM library_root r LEFT JOIN library_root_state s ON s.root_id = r.id"
	        " LEFT JOIN library_identity_root ir ON ir.root_id = r.id WHERE r.id = $1::bigint", 1, params, &root, error))
		return FALSE;
	if (axis_db_result_get_n_rows(root) == 0) {
		report->skipped = "the root is gone";
		return TRUE;
	}
	run.kind = axis_identify_root_kind(axis_db_result_get_text(root, 0, 2));
	if (run.kind == 0) {
		report->skipped = "not a root of movies or for TV";
		return TRUE;
	}
	/* What the scheduler compares with next time: a scan or a probe after
	 * this point is a change. */
	run.scan_generation = axis_identify_row_int(root, 0, 3, 0);
	run.started = axis_identify_row_text(root, 0, 5);
	if (run.scan_generation <= 0) {
		report->skipped = "the root has no complete scan yet";
		ok = record_root(&run, 0, 0, "Waiting for the root's first complete scan.", FALSE, error);
		g_free(run.started);
		return ok;
	}
	{
		gboolean waiting = FALSE;

		if (axis_db_result_get_boolean(root, 0, 4, &waiting) && waiting) {
			report->skipped = "waiting for the details provider";
			g_free(run.started);
			return TRUE;
		}
	}
	run.provider = axis_identify_provider_for(self, run.kind);
	if (run.provider == NULL) {
		/* Looked at again after the wait, in case one has been added. */
		report->skipped = run.kind == AXIS_META_KIND_SERIES ? "no details provider serves series" :
			"no details provider serves movies";
		ok = record_root(&run, 0, 0, run.kind == AXIS_META_KIND_SERIES ? "No details provider serves series: "
			"identification waits for one." : "No details provider serves movies: identification waits for one.", TRUE,
			error);
		g_free(run.started);
		return ok;
	}
	run.volume_id = axis_identify_row_int(root, 0, 0, 0);
	run.root_rel = axis_identify_row_text(root, 0, 1);

	/* What went first: then what is new or changed, the newest first. */
	if (!forget_gone(&run, &report->removed, error))
		goto out_fail;
	limit = g_strdup_printf("%" G_GINT64_FORMAT, CLAMP(per_run, 1, 500));
	params[1] = limit;
	/* (A scope with a unit to look at again counts as changed: one decided
	 * by older rules that no person decided, once, and one whose wait for
	 * the provider is over; AXIS_IDENTIFY_AGAIN.) */
	sql = g_strdup_printf("WITH cur AS (SELECT %s AS path, bool_or(strpos(f.rel_path, '/') = 0) AS loose,"
		" md5(string_agg(%s, '|' ORDER BY %s, f.id)) AS fp, max(f.created_at) AS newest"
		" FROM library_file f JOIN library_root r ON r.id = f.root_id"
		" LEFT JOIN media_info mi ON mi.file_id = f.id AND mi.status = 'ok' AND mi.duration_ms IS NOT NULL"
		" WHERE f.root_id = $1::bigint AND %s GROUP BY 1),"
		" diff AS (SELECT c.* FROM cur c LEFT JOIN library_identity_scope s ON s.root_id = $1::bigint"
		"   AND s.path_hash = library_path_hash(c.path) WHERE s.fingerprint IS DISTINCT FROM c.fp"
		"   OR EXISTS (SELECT 1 FROM library_identity i WHERE i.root_id = $1::bigint"
		"   AND i.scope_hash = library_path_hash(c.path) AND " AXIS_IDENTIFY_AGAIN "))"
		" SELECT d.path, d.loose, d.fp, count(*) OVER (), (SELECT count(*) FROM cur) FROM diff d"
		" ORDER BY d.newest DESC, d.path LIMIT $2::integer", scope_of(&run), fingerprint_of(&run),
		run.kind == AXIS_META_KIND_SERIES ? "f.rel_path" : "f.base_name", axis_identify_counted_condition());
	if (!axis_db_pool_exec(self->pool, sql, 2, params, &scopes, error))
		goto out_fail;
	if (axis_db_result_get_n_rows(scopes) > 0) {
		changed = (guint)axis_identify_row_int(scopes, 0, 3, 0);
		total = (guint)axis_identify_row_int(scopes, 0, 4, 0);
	} else {
		if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM library_identity_scope WHERE root_id = $1::bigint", 1,
		        params, &count, error))
			goto out_fail;
		total = (guint)axis_identify_row_int(count, 0, 0, 0);
	}
	report->scopes = total;

	for (i = 0; i < axis_db_result_get_n_rows(scopes); i++) {
		gboolean loose = FALSE;
		gboolean done = FALSE;

		if (cancelled(&run, error))
			goto out_fail;
		if (g_get_monotonic_time() > run.deadline_us)
			break;
		axis_db_result_get_boolean(scopes, i, 1, &loose);
		if (!examine_scope(&run, axis_db_result_get_text(scopes, i, 0), loose, axis_db_result_get_text(scopes, i, 2),
		        &done, error))
			goto out_fail;
		if (done)
			report->examined++;
		if (run.halt)
			stop_run(&run, "The details provider holds Axis off: identification goes on later.");
		if (run.stop)
			break;
		if (context != NULL && changed > 0)
			axis_job_context_set_progress(context, (gdouble)(i + 1) / (gdouble)MIN(changed, (guint)per_run), NULL);
	}
	report->pending = changed - MIN(changed, report->examined);
	/* A root for TV: the seasons its series need, with the time left. */
	if (run.kind == AXIS_META_KIND_SERIES && !run.stop && !fetch_seasons(&run, error))
		goto out_fail;
	report->provider_down = run.stop;
	if (run.stop)
		text = g_strdup_printf("%s %u looked at, %u left.", run.stop_text, report->examined, report->pending);
	else if (report->examined == 0 && report->seasons == 0)
		text = g_strdup("Nothing new or changed.");
	else if (run.kind == AXIS_META_KIND_SERIES)
		text = g_strdup_printf("Looked at %u of the new or changed folders and files (%u linked, %u to review%s), %u left; %u %s "
			"of the series fetched, %u left.", report->examined, report->linked, report->review,
			report->waiting > 0 ? ", some waiting for the details provider" : "", report->pending, report->seasons,
			report->seasons == 1 ? "season" : "seasons", report->seasons_pending);
	else
		text = g_strdup_printf("Looked at %u of the new or changed folders and files (%u linked, %u to review%s), %u left.",
			report->examined, report->linked, report->review,
			report->waiting > 0 ? ", some waiting for the details provider" : "", report->pending);
	ok = record_root(&run, total, report->pending, text, run.stop, error);
	if (run.guard != NULL)
		axis_volume_guard_release(run.guard);
	g_free(run.root_rel);
	g_free(run.stop_text);
	g_free(run.started);
	return ok;

out_fail:
	if (run.guard != NULL)
		axis_volume_guard_release(run.guard);
	g_free(run.root_rel);
	g_free(run.stop_text);
	g_free(run.started);
	return FALSE;
}
