/*
 * axis-tv-search.c - the series' side of the acquisition layer: what a
 * search of an episode, a season or a whole series wants, what each release
 * holds, the claim of the episodes a release is for, and the history; the
 * tv.search job's work and an administrator's grab
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The search itself (asking the indexers, reading, matching the names to the
 * series against every series of that name, deciding, ranking, storing every
 * release, claiming, fetching and sending once) is src/acquire/'s.  What a
 * series adds:
 *
 *  - load(): the series and its episodes (THE episode facts), and what the
 *    target wants: the episode; a season's wanted aired episodes; every
 *    wanted aired episode.  Each is paced by itself.
 *  - prepare(): the series' record, every other series of its name (a
 *    release that matches one of them is not this series), and the request:
 *    an episode by its season and number, a season, or the series' name.
 *  - judge(): what the release holds (axis_tv_match_release(): never a
 *    guess), then whether it fits what was asked: an episode search takes a
 *    release that holds that episode; a season search a whole-season pack
 *    of that season; a series search packs of whole seasons.  A release
 *    holding several seasons only fits a series search.  Every monitored
 *    episode it holds must be wanted or an upgrade, and none in flight.  It
 *    is decided against the worst file of the episodes it would replace
 *    (decide_each), or against nothing when one of them has no file.
 *  - claim_check()/claimed(): the episodes it holds locked (by id, in
 *    order), none of them in an open grab or a download Axis gave up on that
 *    is still at its client; then the grab's episode rows, which the unique
 *    index series_grab_episode_one_open guards besides.
 */

#include "modules/tv/axis-tv-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "identify/axis-identify.h"

#define LIVE_STATES "('sending', 'queued', 'downloading', 'paused')"

/* The file a release is decided against, kept with the want. */
typedef struct
{
	AxisQualityOnDisk  disk;
	AxisRelease       *fields;
	gchar             *name;
} Disk;

static void
disk_free(Disk *disk)
{
	axis_release_free(disk->fields);
	g_free(disk->name);
	g_free(disk);
}

/* What one search wants, beside the shared want. */
typedef struct
{
	AxisTv              *self;
	AxisTvSeries        *series;
	GPtrArray           *episodes;	/* AxisTvEpisode, not gone */
	AxisTvKnown         *known;
	AxisTvScope          scope;
	gint64               item;
	gint                 season;	/* a season search's */
	gint64               target;	/* an episode search's */
	AxisTvRules          rules;
	gchar               *job_key;
	AxisMetaTitleAnswer  title;
	GPtrArray           *others;	/* AxisMetaTitle, owned */
	GPtrArray           *disks;	/* Disk */
	/* By force: what the person named (the target's), and what the grab is
	 * bound to as force_preview() resolved it: the claim takes these episodes
	 * instead of the ones the release's name gives. */
	gboolean             named_set;
	GArray              *named;	/* (nullable) gint64, owned */
	gboolean             named_whole;
	gint                 named_season;
	GArray              *forced_ids;	/* (nullable) gint64: the episodes bound */
	gint                 forced_season;	/* the season bound whole, -1: episodes */
	gchar               *forced_label;	/* (nullable) "S01E03", "season 1" */
	/* What the matcher is given of an anime series (add-anime-support, A2);
	 * for any other series only "not anime". */
	AxisTvAnime          anime;
	/* (nullable) An anime series': the releases, by name, that were grabbed by
	 * absolute numbers and imported with no file for an episode they were
	 * grabbed for, which still has none: each with those episodes, "S02E01".
	 * No search grabs one of them again (the A3 review). */
	GHashTable          *had;
} Want;

static void
want_free(Want *w)
{
	axis_tv_anime_clear(&w->anime);
	g_clear_pointer(&w->had, g_hash_table_unref);
	axis_tv_series_free(w->series);
	g_clear_pointer(&w->episodes, g_ptr_array_unref);
	g_free(w->known);
	g_free(w->job_key);
	axis_meta_title_answer_clear(&w->title);
	g_clear_pointer(&w->others, g_ptr_array_unref);
	g_clear_pointer(&w->disks, g_ptr_array_unref);
	g_clear_pointer(&w->named, g_array_unref);
	g_clear_pointer(&w->forced_ids, g_array_unref);
	g_free(w->forced_label);
	g_free(w);
}

/* What one release holds: the episodes' ids, in words, and what it names
 * that the data does not have. */
typedef struct
{
	GArray   *ids;	/* gint64 */
	gchar    *holds;
	gboolean  mismatched;
	gchar    *missing;
	/* It was left first because it does not hold what was searched for (see
	 * reject_gives_nothing_searched()) and is only also one that a person
	 * alone could take: grabbing it would not give the search what it looked
	 * for, so it is not counted as a release that waits for a person. */
	gboolean  beside;
} Held;

/*
 * Whether @code (one of judge()'s) says that the release does not hold what
 * was searched for: another episode, nothing that is wanted, not the season
 * or the packs asked for.  Such a release stays that whatever is said beside
 * it: a person who grabbed it would not fill the episode the search was for.
 * (A release that holds it and is left for another reason, a file kept by
 * hand, the profile, an episode it also holds, is not one of these: a
 * person's grab fills the episode.)
 */
static gboolean
reject_gives_nothing_searched(const gchar *code)
{
	return g_strcmp0(code, "other_episode") == 0 || g_strcmp0(code, "not_wanted") == 0 ||
		g_strcmp0(code, "not_season") == 0 || g_strcmp0(code, "not_pack") == 0 ||
		g_strcmp0(code, "several_seasons") == 0;
}

static void
held_free(Held *held)
{
	g_clear_pointer(&held->ids, g_array_unref);
	g_free(held->holds);
	g_free(held->missing);
	g_free(held);
}

/* Wanted: monitored, and with no file (aired, or the one a search of it
 * asked for), or with one below the cutoff. */
static gboolean
episode_wanted(
	const Want          *w,
	const AxisTvEpisode *e
){
	if (!e->monitored || e->gone)
		return FALSE;
	/* An episode held because the app it was brought from has a file Axis
	 * does not see: no search takes it, alone or in a pack. */
	if (axis_tv_hold_stops(e->search_hold))
		return FALSE;
	if (e->n_files > 0)
		return axis_tv_episode_upgradable(e, w->series);
	return axis_tv_episode_aired(e, &w->rules, NULL) || e->id == w->target;
}

static gboolean
load(
	gpointer                  data,
	AxisDbConn               *conn,
	const AxisAcquireTarget  *target,
	AxisAcquireWant         **want_out,
	const gchar             **skip,
	GError                  **error
){
	AxisTv *self = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", target->owner_id);
	AxisAcquireWant *want;
	Want *w;
	guint i;

	*want_out = NULL;
	*skip = NULL;
	if (!axis_tv_store_series_one(conn, target->owner_id, &series, error))
		return FALSE;
	if (series == NULL) {
		*skip = "the series was removed";
		return TRUE;
	}
	/* The gate again, where every search job passes whatever queued it and
	 * whenever: a series migrated and held is searched by nothing but a
	 * person's own search or grab, which passes by what it is (its cause),
	 * not by ending the hold: the series stays held for everything else. */
	if (!target->person && !target->by_hand && axis_tv_held(series, NULL)) {
		*skip = "it was migrated and is held: Axis does not search for it until searching is started";
		return TRUE;
	}
	if (!axis_tv_store_episodes(conn, ids, FALSE, &episodes, error))
		return FALSE;
	w = g_new0(Want, 1);
	w->self = self;
	w->item = target->item_id;
	if (!axis_tv_scope_from_name(target->scope, &w->scope))
		w->scope = AXIS_TV_SCOPE_EPISODE;
	w->season = w->scope == AXIS_TV_SCOPE_SEASON ? (gint)target->item_id : -1;
	w->target = w->scope == AXIS_TV_SCOPE_EPISODE ? target->item_id : 0;
	w->job_key = target->by_hand ? axis_tv_manual_key(series->id, w->scope, target->item_id) :
		axis_tv_search_key(series->id, w->scope, target->item_id);
	axis_tv_get_rules(self, &w->rules);
	w->forced_season = -1;
	w->named_set = target->named_set;
	w->named_whole = target->named_set && target->named_whole;
	w->named_season = w->named_whole ? (gint)CLAMP(target->named_part, 0, 100000) : -1;
	if (target->named_set && target->named != NULL) {
		w->named = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_vals(w->named, target->named->data, target->named->len);
	}
	w->disks = g_ptr_array_new_with_free_func((GDestroyNotify)disk_free);
	w->known = g_new0(AxisTvKnown, MAX(episodes->len, 1));
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		w->known[i].id = e->id;
		w->known[i].season = e->season;
		w->known[i].number = e->number;
		w->known[i].air_date = e->air_date;
	}
	/* An anime series: the state of its absolute numbers, each episode's
	 * number and the series' names, for names numbered absolutely. */
	if (!axis_tv_anime_init(&w->anime, conn, series, episodes, w->rules.today, error)) {
		want_free(w);
		return FALSE;
	}
	/* And the releases it had before that left an episode without a file
	 * (finished() in axis-tv-import.c records them): never for any other
	 * series, whose searches read nothing here. */
	if (w->anime.read.anime) {
		g_autoptr(AxisDbResult) had = NULL;
		g_autofree gchar *id = axis_tv_int_text(series->id);
		const gchar *params[1];

		params[0] = id;
		if (!axis_db_conn_exec(conn, "SELECT g.release_name, string_agg(DISTINCT 'S' || lpad(e.season_number::text, 2,"
		        " '0') || 'E' || lpad(e.number::text, 2, '0'), ', ') FROM series_grab g JOIN series_grab_episode ge"
		        " ON ge.grab_id = g.id JOIN series_episode e ON e.id = ge.episode_id WHERE g.series_id = $1::bigint"
		        " AND g.close_reason = 'imported' AND g.close_text LIKE '" AXIS_TV_GRAB_LEFT_HOLES "%'"
		        " AND g.release_name IS NOT NULL AND e.gone_at IS NULL AND NOT EXISTS (SELECT 1 FROM series_file_episode"
		        " sfe WHERE sfe.episode_id = e.id) GROUP BY g.release_name LIMIT 200", 1, params, &had, error)) {
			want_free(w);
			return FALSE;
		}
		for (i = 0; i < axis_db_result_get_n_rows(had); i++) {
			if (w->had == NULL)
				w->had = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			g_hash_table_insert(w->had, g_strdup(axis_db_result_get_text(had, i, 0)),
				g_strdup(axis_db_result_get_text(had, i, 1)));
		}
	}

	want = axis_acquire_want_new();
	want->owner_id = series->id;
	want->title_id = series->title_id;
	want->label = axis_tv_series_label(series->title, series->year);
	want->in_flight = FALSE;
	want->profile_id = series->profile_id;
	want->profile_name = g_strdup(series->profile_name);
	want->profile_usable = series->profile_usable;
	want->root_id = series->root_id;
	want->root_name = g_strdup(series->root_name);
	want->volume_id = series->volume_id;
	want->volume_name = g_strdup(series->volume_name);
	want->runtime_minutes = series->runtime;
	w->series = g_steal_pointer(&series);
	w->episodes = g_steal_pointer(&episodes);
	want->data = w;
	want->data_free = (GDestroyNotify)want_free;
	*want_out = want;

	/* What it wants, each episode paced by itself. */
	for (i = 0; i < w->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(w->episodes, i);

		if (w->scope == AXIS_TV_SCOPE_EPISODE) {
			if (e->id != w->target)
				continue;
			g_array_append_val(want->pace_ids, e->id);
			if (axis_tv_episode_in_flight(e) && !target->by_hand)
				*skip = "a grab of it is in flight";
			/* (A person's look at what there is for it, or their grab of
			 * one of those, is theirs to make; Search now ended the
			 * episode's hold before it queued.) */
			else if (axis_tv_hold_stops(e->search_hold) && !target->by_hand)
				*skip = "the app it was migrated from has a file for it that Axis does not see: it is held";
			continue;
		}
		if (w->scope == AXIS_TV_SCOPE_SEASON && e->season != w->season)
			continue;
		if (axis_tv_episode_in_flight(e) || !episode_wanted(w, e))
			continue;
		g_array_append_val(want->pace_ids, e->id);
	}
	/* A person's own look or grab: whatever the automatic rules want now;
	 * only an episode gone from the data stops it. */
	if (want->pace_ids->len == 0 && *skip == NULL && (w->scope == AXIS_TV_SCOPE_EPISODE || !target->by_hand))
		*skip = w->scope == AXIS_TV_SCOPE_EPISODE ? "the episode is no longer in the series' data" :
			"nothing in it is wanted now";
	return TRUE;
}

static const AxisTvEpisode *
episode_by_id(
	const Want *w,
	gint64      id
){
	guint i;

	for (i = 0; i < w->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(w->episodes, i);

		if (e->id == id)
			return e;
	}
	return NULL;
}

/*
 * What a search of an anime series asks besides the request of any series
 * (add-anime-support, slice A3).  Anime releases are named by an absolute
 * number and filed under TV/Anime, and no indexer has a parameter for an
 * absolute number, so:
 *
 *  - every request is marked as an anime series' (@request, and each one
 *    added): the indexer layer then asks in TV/Anime beside the TV categories;
 *  - an episode that has an absolute number, of a series whose numbers are
 *    usable, is also asked for by the words of the series' title and the
 *    number ("Tenkuu Rangers 137", padded to the series' width), and by the
 *    same with one more name of the series (axis_tv_anime_second_name()) when
 *    it has one: at most three requests where a series makes one;
 *  - a season is also asked for by the series' name alone, which is how a
 *    batch is found (no words can name a range).  That is the very request a
 *    search of the whole series makes, so the indexer layer keeps one answer
 *    for every season searched while it lasts.
 *
 * Each added request is a request of its own to the indexer layer: it counts
 * against every indexer's own limits and holds, and an indexer that takes no
 * words is shown as not asked.  What comes back is matched and judged exactly
 * as what the first request returns: asking differently grabs nothing that
 * the matcher would not take.
 */
static void
anime_asks(
	Want                *w,
	AxisAcquireWant     *want,
	const AxisMetaTitle *record,
	AxisIndexerRequest  *request,
	const AxisTvEpisode *target
){
	AxisIndexerRequest words;

	request->anime = TRUE;
	axis_indexer_request_init(&words, AXIS_INDEXER_QUERY_TEXT);
	words.anime = TRUE;
	if (w->scope == AXIS_TV_SCOPE_EPISODE) {
		const gchar *second = axis_tv_anime_second_name(record);
		guint digits = axis_tv_absolute_digits(w->anime.read.absolute, w->episodes->len);
		gint absolute = 0;
		guint i;

		if (target == NULL)
			return;
		for (i = 0; w->anime.read.absolute != NULL && i < w->episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(w->episodes, i);

			if (e->id == target->id && e->season >= 1)
				absolute = w->anime.read.absolute[i];
		}
		if (w->anime.read.state == AXIS_TV_ABSOLUTE_USABLE && absolute > 0) {
			g_autofree gchar *one = axis_tv_absolute_words(record->title, absolute, digits);
			g_autofree gchar *two = axis_tv_absolute_words(second, absolute, digits);

			want->ask_label = g_strdup_printf("by season %d episode %d, in TV and Anime", target->season,
				target->number);
			if (one != NULL) {
				g_autofree gchar *label = g_strdup_printf("by the words \"%s\", in TV and Anime", one);

				axis_acquire_want_add_ask(want, &words, one, label);
			}
			if (two != NULL) {
				g_autofree gchar *label = g_strdup_printf("by the words \"%s\", in TV and Anime", two);

				axis_acquire_want_add_ask(want, &words, two, label);
			}
		} else {
			want->ask_label = g_strdup_printf("by season %d episode %d, in TV and Anime (not by an absolute number: "
				"%s)", target->season, target->number, w->anime.read.state == AXIS_TV_ABSOLUTE_USABLE ?
				"TheTVDB gives this episode none" : axis_tv_absolute_state_words(w->anime.read.state));
		}
	} else if (w->scope == AXIS_TV_SCOPE_SEASON) {
		want->ask_label = g_strdup_printf("by season %d, in TV and Anime", w->season);
		if (record->title != NULL && record->title[0] != '\0')
			axis_acquire_want_add_ask(want, &words, record->title, "by the series' name, in TV and Anime, for batches");
	} else {
		want->ask_label = g_strdup("by the series' name, in TV and Anime");
	}
}

static gboolean
prepare(
	gpointer             data,
	AxisAcquireWant     *want,
	GCancellable        *cancellable,
	AxisIndexerRequest  *request,
	GPtrArray          **titles,
	gchar              **problem,
	GError             **error
){
	AxisTv *self = data;
	Want *w = want->data;
	g_autoptr(GError) local = NULL;
	AxisMetaSearchAnswer same;
	const AxisMetaTitle *record;
	const AxisTvEpisode *target = NULL;
	guint i;

	*titles = NULL;
	*problem = NULL;
	memset(&same, 0, sizeof(same));
	if (!axis_meta_get_title(self->meta, w->series->title_id, AXIS_TV_META_WAIT_MS, &w->title, &local) ||
	    w->title.title == NULL) {
		*problem = g_strdup_printf("The series' details could not be read%s%s",
			local != NULL ? ": " : ".", local != NULL ? local->message : "");
		return TRUE;
	}
	g_clear_error(&local);
	record = w->title.title;
	/* Every series of its name: a release that is one of them is not this
	 * one (ambiguous is never a match). */
	w->others = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	if (axis_meta_search(self->meta, record->provider, AXIS_META_KIND_SERIES, record->title, 0, AXIS_TV_META_WAIT_MS,
	        &same, &local)) {
		for (i = 0; same.titles != NULL && i < same.titles->len; i++) {
			AxisMetaTitle *other = g_ptr_array_index(same.titles, i);

			if (other->id == record->id)
				continue;
			same.titles->pdata[i] = NULL;
			g_ptr_array_add(w->others, other);
		}
	} else {
		g_debug("series: the series of that name could not be looked up: %s", local->message);
	}
	axis_meta_search_answer_clear(&same);
	*titles = g_ptr_array_new();
	g_ptr_array_add(*titles, (gpointer)record);
	for (i = 0; i < w->others->len; i++)
		g_ptr_array_add(*titles, g_ptr_array_index(w->others, i));

	switch (w->scope) {
	case AXIS_TV_SCOPE_EPISODE:
		target = episode_by_id(w, w->target);
		axis_indexer_request_init(request, AXIS_INDEXER_QUERY_EPISODE);
		request->season = target != NULL ? target->season : 0;
		request->episode = target != NULL ? target->number : 1;
		break;
	case AXIS_TV_SCOPE_SEASON:
		axis_indexer_request_init(request, AXIS_INDEXER_QUERY_SEASON);
		request->season = w->season;
		break;
	default:
		/* The whole series: its name, for packs of several seasons. */
		axis_indexer_request_init(request, AXIS_INDEXER_QUERY_TEXT);
		break;
	}
	request->title = record->title;
	if (w->scope != AXIS_TV_SCOPE_SERIES) {
		const gchar *imdb = axis_meta_title_get_id(record, "imdb");

		request->tvdb_id = w->series->tvdb_id != NULL ? g_ascii_strtoll(w->series->tvdb_id, NULL, 10) : 0;
		request->tmdb_id = w->series->tmdb_id != NULL ? g_ascii_strtoll(w->series->tmdb_id, NULL, 10) : 0;
		request->imdb_id = imdb != NULL && axis_indexer_imdb_valid(imdb) ? imdb : NULL;
	}
	/* A series that is not marked as anime: the request above, and nothing
	 * else, exactly as before slice A3. */
	if (w->anime.read.anime)
		anime_asks(w, want, record, request, target);
	return TRUE;
}

/* Not wanted: the profile's kind of rejection, an administrator may grab it
 * all the same. */
static void
not_wanted(
	AxisAcquireConsidered *c,
	const gchar           *code,
	const gchar           *phrase,
	gchar                 *text
){
	c->rejected = TRUE;
	c->reject_code = code;
	c->reject_phrase = phrase;
	g_free(c->reject_text);
	c->reject_text = text;
}

static void
judge(
	gpointer               data,
	AxisAcquireWant       *want,
	AxisAcquireConsidered *c
){
	Want *w = want->data;
	AxisTvHolds holds;
	const AxisTvEpisode *offender = NULL;
	const AxisTvEpisode *flying = NULL;
	const AxisTvEpisode *kept = NULL;
	const AxisTvEpisode *worst = NULL;
	gboolean hole = FALSE;
	gboolean holds_target = FALSE;
	gboolean partial = FALSE;
	gboolean batch = FALSE;
	guint wanted = 0;
	guint wanted_of_season = 0;
	gint runtime = 0;
	Held *held = g_new0(Held, 1);
	guint i;

	held->ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	c->data = held;
	c->data_free = (GDestroyNotify)held_free;
	/* An anime series whose absolute numbers are usable: a name numbered
	 * absolutely is the episode that has the number (never for any other
	 * series: then this is axis_tv_match_release()). */
	w->anime.read.name = w->anime.read.anime && c->release != NULL ? c->release->name : NULL;
	if (w->anime.read.anime) {
		gint version = 0;

		/* "Title 12v2", which reading by itself leaves in the title: the
		 * version is a revision, as the parser's own is. */
		if (axis_tv_anime_claims(c->parsed, &w->anime.read, &version, NULL) && version > 0 && c->parsed != NULL &&
		    c->parsed->version == 0)
			c->parsed->version = version;
	}
	/* A search judging a release reads a batch (a range of absolute numbers
	 * that is not whole seasons) as the episodes it maps to: slice A3. */
	w->anime.read.batches = w->anime.read.anime;
	axis_tv_match_release_anime(c->parsed, w->known, w->episodes->len, &w->anime.read, &holds);
	w->anime.read.name = NULL;
	w->anime.read.batches = FALSE;
	batch = holds.ok && holds.batch && !holds.pack;
	if (!holds.ok && holds.found != NULL && holds.found->len > 0) {
		/* Numbered beyond the data in part: what it names that the data
		 * has, for a person to grab by hand; never a search's. */
		guint first = g_array_index(holds.found, guint, 0);

		partial = TRUE;
		held->mismatched = TRUE;
		held->missing = g_strdup(holds.missing);
		g_array_set_size(holds.indices, 0);
		g_array_append_vals(holds.indices, holds.found->data, holds.found->len);
		g_free(holds.label);
		holds.label = g_strdup(holds.found_label);
		g_free(holds.text);
		holds.text = g_strdup_printf("it holds %s", holds.found_label);
		holds.season_first = w->known[first].season;
		holds.season_last = w->known[first].season;
		holds.ok = TRUE;
	}
	if (!holds.ok) {
		/* Never guessed: not this series' episodes, by a fixed rule.  A
		 * numbering the series' data does not have is counted. */
		held->mismatched = g_strcmp0(holds.code, "no_such_episode") == 0 || g_strcmp0(holds.code,
			"no_such_season") == 0;
		c->match = AXIS_RELEASE_MATCH_NONE;
		g_free(c->match_text);
		c->match_text = g_steal_pointer(&holds.text);
		c->reject_phrase = held->mismatched ? "numbered for no episode in the series' data" :
			g_strcmp0(holds.code, "absolute_only") == 0 ? "only an absolute episode number" :
			g_strcmp0(holds.code, "no_episode") == 0 ? "naming no episode" :
			/* An anime series' own refusals (axis_tv_match_release_anime()). */
			g_strcmp0(holds.code, "season_and_absolute") == 0 ? "a season with a bare number" :
			g_strcmp0(holds.code, "absolute_batch") == 0 ? "a batch of part of a season" :
			g_strcmp0(holds.code, "special") == 0 ? "a special named by a word" :
			g_strcmp0(holds.code, "numberings_differ") == 0 ? "two numberings that name different episodes" :
			holds.code != NULL && g_str_has_prefix(holds.code, "absolute_") ?
			"an absolute number that is not matched" : "not matched to an episode";
		axis_tv_holds_clear(&holds);
		return;
	}
	held->holds = g_strdup(holds.label);
	for (i = 0; i < holds.indices->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(w->episodes, g_array_index(holds.indices, guint, i));

		g_array_append_val(held->ids, e->id);
		if (e->id == w->target)
			holds_target = TRUE;
		runtime += e->runtime > 0 ? e->runtime : w->series->runtime;
		if (axis_tv_episode_in_flight(e)) {
			if (flying == NULL)
				flying = e;
			continue;
		}
		/* A file kept by hand, monitored or not: no search replaces it. */
		if (e->kept_by != NULL && e->n_files > 0 && kept == NULL)
			kept = e;
		if (!e->monitored)
			continue;
		if (episode_wanted(w, e)) {
			wanted++;
			if (e->season == w->season)
				wanted_of_season++;
			if (e->n_files == 0)
				hole = TRUE;
			else if (worst == NULL || axis_quality_compare_on_disk(e->worst_quality, e->worst_resolution,
			         worst->worst_quality, worst->worst_resolution) < 0)
				worst = e;
		} else if (offender == NULL) {
			offender = e;
		}
	}
	{
		g_autofree gchar *said = c->match_text;

		c->match_text = g_strdup_printf("%s; %s", said != NULL ? said : "it matches the series", holds.text);
	}
	/* What was asked for. */
	if (w->scope == AXIS_TV_SCOPE_EPISODE && !holds_target) {
		const AxisTvEpisode *t = episode_by_id(w, w->target);
		g_autofree gchar *label = t != NULL ? axis_tv_episode_label(t->season, t->number) : g_strdup("the episode");

		not_wanted(c, "other_episode", "other episodes", g_strdup_printf("It holds %s, not %s that was searched for.",
			holds.label, label));
	} else if (w->scope == AXIS_TV_SCOPE_SEASON && batch && wanted_of_season == 0) {
		/* An anime series' batch fits a season search when it holds a wanted
		 * episode of that season (and a search of the whole series always);
		 * the rule for packs below then decides, as for a pack. */
		not_wanted(c, "not_season", "a batch with nothing wanted of the season", g_strdup_printf("It is a batch that "
			"holds %s, and nothing it holds of season %d is wanted.", holds.label, w->season));
	} else if (w->scope == AXIS_TV_SCOPE_SEASON && !batch && (!holds.pack || holds.season_first != w->season ||
	           holds.season_last != w->season)) {
		not_wanted(c, "not_season", "not the whole season", g_strdup_printf("A season search takes the whole season: "
			"it holds %s. The season's episodes are sought one by one when no whole season is had.", holds.label));
	} else if (w->scope == AXIS_TV_SCOPE_SERIES && !holds.pack && !batch) {
		not_wanted(c, "not_pack", "not whole seasons", g_strdup_printf("A search of the whole series takes packs of "
			"whole seasons: it holds %s.", holds.label));
	} else if (holds.season_last > holds.season_first && w->scope != AXIS_TV_SCOPE_SERIES && !batch) {
		not_wanted(c, "several_seasons", "several seasons", g_strdup_printf("It holds several seasons (%s), which only "
			"a search of the whole series takes.", holds.label));
	} else if (partial) {
		not_wanted(c, "not_in_data", "numbered beyond the series' data", g_strdup_printf("It also names %s, which the "
			"series' data does not have: no search takes it. Only Grab this one, from a manual search, takes it for "
			"%s, after a warning that it is numbered differently.", held->missing, holds.label));
	} else if (flying != NULL) {
		g_autofree gchar *label = axis_tv_episode_label(flying->season, flying->number);

		not_wanted(c, "in_flight", "holding an episode in flight", g_strdup_printf("It holds %s, whose grab is in "
			"flight.", label));
	} else if (kept != NULL) {
		g_autofree gchar *label = axis_tv_episode_label(kept->season, kept->number);

		if (kept->kept_brought)
			not_wanted(c, "kept_by_hand", "replacing a file kept as it was migrated", g_strdup_printf("It holds %s, "
				"and the file in the library was migrated from Sonarr; Axis does not replace it: no search does. "
				"Only Grab this one, from a manual search, replaces it, with the tick that names it.", label));
		else
			not_wanted(c, "kept_by_hand", "replacing a file kept by hand", g_strdup_printf("It holds %s, and the file "
				"in the library is kept by hand by %s: no search replaces it. Only Grab this one, from a manual "
				"search, replaces it, with the tick that names it.", label, kept->kept_by));
	} else if (offender != NULL) {
		g_autofree gchar *label = axis_tv_episode_label(offender->season, offender->number);

		not_wanted(c, "holds_unwanted", "holding an episode not wanted", g_strdup_printf("It holds %s, which is "
			"monitored and not wanted (in the library at its profile's cutoff, or not aired).", label));
	} else if (wanted == 0) {
		not_wanted(c, "not_wanted", "not wanted", g_strdup_printf("Nothing it holds (%s) is wanted.", holds.label));
	}
	/* Numbered beyond the data, and left for another reason: said too, and
	 * stored as numbered beyond the data whatever else is said (no one click
	 * takes it: see the releases page). */
	if (partial && g_strcmp0(c->reject_code, "not_in_data") != 0 && c->reject_text != NULL) {
		gchar *more = g_strdup_printf("%s It also names %s, which the series' data does not have: only Grab this "
			"one, from a manual search, takes it.", c->reject_text, held->missing);

		/* What it was left for stays the reason that counts. */
		held->beside = reject_gives_nothing_searched(c->reject_code);
		g_free(c->reject_text);
		c->reject_text = more;
		c->reject_code = "not_in_data";
	}
	/* An anime series' release held by a reading too weak for a search to
	 * grab on (the number only follows the series' name; or it stands under a
	 * name that is not the series' title and can be a part's own count): a
	 * person's to grab, never a search's, whatever else is said. */
	if (holds.person_only && holds.person_why != NULL) {
		if (c->rejected && c->reject_text != NULL) {
			gchar *more = g_strdup_printf("%s %s Only Grab this one, from a manual search, takes it.", c->reject_text,
				holds.person_why);

			g_free(c->reject_text);
			c->reject_text = more;
		} else {
			not_wanted(c, "bare_number", "an absolute number no search takes by itself", g_strdup_printf("%s Only Grab "
				"this one, from a manual search, takes it for %s.", holds.person_why, holds.label));
		}
	}
	/* A release that was downloaded before, by absolute numbers, and had no
	 * file for an episode that still has none: grabbing it again fills
	 * nothing.  A person may all the same. */
	if (w->had != NULL && c->release != NULL && c->release->name != NULL &&
	    g_strcmp0(c->reject_code, "not_in_data") != 0) {
		const gchar *holes = g_hash_table_lookup(w->had, c->release->name);

		/* (Whatever else is said of it, which is kept.) */
		if (holes != NULL && c->rejected && reject_gives_nothing_searched(c->reject_code))
			held->beside = TRUE;
		if (holes != NULL)
			not_wanted(c, "had_before", "downloaded before without a file for an episode", g_strdup_printf("%s%sThis "
				"release was downloaded before and %s got no file from it: no search grabs it again. Only Grab this "
				"one, from a manual search, takes it.", c->rejected && c->reject_text != NULL ? c->reject_text : "",
				c->rejected && c->reject_text != NULL ? " " : "", holes));
	}
	/* Decided against the worst file of what it would replace; against
	 * nothing when it fills an episode that has none. */
	c->runtime_minutes = runtime;
	c->noun = holds.pack ? "season" : batch ? "batch" : "episode";
	if (!hole && worst != NULL) {
		Disk *disk = g_new0(Disk, 1);

		axis_acquire_disk_set(&disk->disk, &disk->fields, &disk->name, worst->worst_quality,
			worst->worst_resolution, worst->worst_release);
		/* A file Axis converted counts as its source's quality. */
		disk->disk.converted = worst->worst_converted;
		disk->disk.source_quality = worst->worst_converted ? worst->worst_quality : AXIS_QUALITY_UNKNOWN;
		g_ptr_array_add(w->disks, disk);
		c->disk = &disk->disk;
	}
	axis_tv_holds_clear(&holds);
}

/*
 * A release whose name, read by itself, is none of the titles compared: for
 * an anime series whose absolute numbers are usable, a name that begins with
 * the series' own name and goes on with one number or one range ("Tenkuu
 * Rangers 137 [1080p]": read by itself, its title is "Tenkuu Rangers 137") is
 * the series' all the same, and is judged as any release of it.  The shared
 * code calls this only for a name that matched no title at all: one that is
 * another series of the lookup, by its own title ("Tenkuu Rangers 2"), never
 * comes here.  For a series that is not marked as anime nothing changes.
 *
 * Nothing in such a name marks the number as an episode's, and the name of
 * another title (a sequel, a film, a volume) can be the very same.  So:
 *  - a name that states a year that is not the series', and a name that one
 *    of the other titles looked up could own (the same name, a longer one, or
 *    the series' name with that number), is not claimed at all;
 *  - what is claimed is listed with what it holds and is never a search's to
 *    grab: only a person takes it, with Grab this one.  (Whichever request
 *    returned it: since slice A3 a search also asks by the words of the
 *    series' name and an absolute number, and a release that comes back for
 *    those words is no more certainly the series' than before.)
 */
static void
unmatched(
	gpointer               data,
	AxisAcquireWant       *want,
	AxisAcquireConsidered *c
){
	Want *w = want->data;
	g_autofree gchar *why_not = NULL;
	gboolean claims;

	if (!w->anime.read.anime || c->release == NULL || c->parsed == NULL)
		return;
	w->anime.read.name = c->release->name;
	claims = axis_tv_anime_claims(c->parsed, &w->anime.read, NULL, &why_not);
	if (claims && w->others != NULL && w->others->len > 0) {
		g_autoptr(GPtrArray) names = g_ptr_array_new();
		guint i;
		guint k;

		for (i = 0; i < w->others->len; i++) {
			const AxisMetaTitle *other = g_ptr_array_index(w->others, i);

			if (other->title != NULL)
				g_ptr_array_add(names, other->title);
			if (other->original_title != NULL)
				g_ptr_array_add(names, other->original_title);
			for (k = 0; other->alt_titles != NULL && k < other->alt_titles->len; k++) {
				const AxisMetaAltTitle *alt = g_ptr_array_index(other->alt_titles, k);

				if (alt != NULL && alt->title != NULL)
					g_ptr_array_add(names, alt->title);
			}
		}
		why_not = axis_tv_anime_other_title(&w->anime.read, (const gchar *const *)names->pdata, names->len);
		claims = why_not == NULL;
	}
	w->anime.read.name = NULL;
	if (claims) {
		c->match = AXIS_RELEASE_MATCH_YES;
		c->matched = 0;
		g_free(c->match_text);
		c->match_text = g_strdup("it begins with the series' name and goes on with a number, and the series is marked "
			"as anime");
		/* (judge() leaves what was read this way to a person: the matcher
		 * says so of every hold that came from the reading after a title.) */
		judge(data, want, c);
	} else if (why_not != NULL) {
		/* It begins with the series' name, and what follows is doubtful: the
		 * reason a person reads, never a match. */
		g_free(c->match_text);
		c->match_text = g_strdup_printf("it begins with the series' name, but %s", why_not);
		c->reject_phrase = "a name not read as an episode";
	}
}

/* The episodes a grab is for: the search's, else the stored candidate's. */
static gboolean
grab_episodes(
	AxisDbConn                 *conn,
	const AxisAcquireWant      *want,
	const AxisAcquireGrabSpec  *grab,
	GArray                    **ids,
	gchar                     **holds,
	GError                    **error
){
	const Want *w = want != NULL ? want->data : NULL;

	*ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	*holds = NULL;
	/* By force: the episodes the person named, never the ones the release's
	 * name gives.  A whole season: exactly the episodes of it the digest
	 * covered (force_preview() read them), not the season read again here: one
	 * added in between would be claimed unshown. */
	if (grab->bound) {
		if (w == NULL || w->forced_ids == NULL)
			return TRUE;
		*holds = g_strdup(w->forced_label);
		g_array_append_vals(*ids, w->forced_ids->data, w->forced_ids->len);
		return TRUE;
	}
	if (grab->considered != NULL && grab->considered->data != NULL) {
		const Held *held = grab->considered->data;

		g_array_append_vals(*ids, held->ids->data, held->ids->len);
		*holds = g_strdup(held->holds);
		return TRUE;
	}
	if (grab->candidate_id > 0) {
		g_autoptr(AxisDbResult) row = NULL;
		g_autofree gchar *id = axis_tv_int_text(grab->candidate_id);
		const gchar *params[1];
		guint i;

		params[0] = id;
		if (!axis_db_conn_exec(conn, "SELECT unnest(episode_ids), holds FROM series_candidate WHERE id = $1::bigint", 1,
		        params, &row, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(row); i++) {
			gint64 value = 0;

			axis_db_result_get_int64(row, i, 0, &value);
			g_array_append_val(*ids, value);
			if (*holds == NULL && axis_db_result_get_text(row, i, 1) != NULL)
				*holds = g_strdup(axis_db_result_get_text(row, i, 1));
		}
	}
	return TRUE;
}

static gchar *
id_array(const GArray *ids)
{
	GString *array = g_string_new("{");
	guint i;

	for (i = 0; i < ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(array, '}');
	return g_string_free(array, FALSE);
}

/* In the claim's transaction, the series' row locked: the episodes the
 * release holds locked in their order, and none of them in flight. */
static gboolean
claim_check(
	gpointer                   data,
	AxisDbConn                *conn,
	AxisAcquireWant           *want,
	const AxisAcquireGrabSpec *grab,
	gboolean                  *gone,
	gboolean                  *in_flight,
	GError                   **error
){
	g_autoptr(GArray) ids = NULL;
	g_autoptr(AxisDbResult) locked = NULL;
	g_autoptr(AxisDbResult) flying = NULL;
	g_autofree gchar *holds = NULL;
	g_autofree gchar *array = NULL;
	g_autofree gchar *series = axis_tv_int_text(want->owner_id);
	const gchar *params[2];

	*gone = FALSE;
	*in_flight = FALSE;
	/* A release matched under another numbering than the series follows now
	 * (it changed its numbering since: add-metadata-tvdb) holds other
	 * episodes than its name says under this one: never claimed. */
	{
		g_autoptr(AxisDbResult) under = NULL;
		g_autofree gchar *search = axis_tv_int_text(grab->search_id);
		g_autofree gchar *candidate = axis_tv_int_text(grab->candidate_id);
		const gchar *was = NULL;
		const gchar *now;
		const gchar *stamps[3];

		stamps[0] = series;
		stamps[1] = search;
		stamps[2] = candidate;
		if (!axis_db_conn_exec(conn, "SELECT s.episode_source, (SELECT coalesce((SELECT x.source FROM series_search x"
		        " WHERE x.id = $2::bigint), (SELECT y.source FROM series_candidate c JOIN series_search y ON y.id ="
		        " c.search_id WHERE c.id = $3::bigint))) FROM series s WHERE s.id = $1::bigint", 3, stamps, &under, error))
			return FALSE;
		now = axis_db_result_get_n_rows(under) > 0 ? axis_db_result_get_text(under, 0, 0) : NULL;
		if (axis_db_result_get_n_rows(under) > 0)
			was = axis_db_result_get_text(under, 0, 1);
		if (was == NULL && want->data != NULL && ((Want *)want->data)->series != NULL)
			was = ((Want *)want->data)->series->episode_source;
		if (now != NULL && (was == NULL ? grab->candidate_id > 0 && grab->search_id <= 0 : strcmp(was, now) != 0)) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_EXPIRED, "this release was found while the series was "
				"numbered as %s numbers it; it follows %s now, so the release is not grabbed: search again",
				was != NULL ? axis_tv_source_label(was) : "another source", axis_tv_source_label(now));
			return FALSE;
		}
		if (want->data != NULL && ((Want *)want->data)->series != NULL && now != NULL &&
		    g_strcmp0(((Want *)want->data)->series->episode_source, now) != 0) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_EXPIRED, "the series' numbering changed while this search "
				"ran (it follows %s now), so nothing it found is grabbed: search again", axis_tv_source_label(now));
			return FALSE;
		}
	}
	/* By force, under the series' lock: not while an import of the series is
	 * open (the shared predicate placing and the numbering switch use). */
	if (grab->bound) {
		g_autofree gchar *busy = NULL;

		if (!axis_tv_import_open(conn, want->owner_id, &busy, error))
			return FALSE;
		if (busy != NULL) {
			g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED, "%s Nothing is grabbed by force for the series "
				"until then", busy);
			return FALSE;
		}
	}
	if (!grab_episodes(conn, want, grab, &ids, &holds, error))
		return FALSE;
	/* A release for no episode is never claimed. */
	if (ids->len == 0) {
		*in_flight = TRUE;
		return TRUE;
	}
	array = id_array(ids);
	params[0] = series;
	params[1] = array;
	if (!axis_db_conn_exec(conn, "SELECT id FROM series_episode WHERE series_id = $1::bigint AND id = ANY ($2::bigint[])"
	        " AND gone_at IS NULL ORDER BY id FOR UPDATE", 2, params, &locked, error))
		return FALSE;
	if (axis_db_result_get_n_rows(locked) != ids->len) {
		*gone = TRUE;
		return TRUE;
	}
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM series_grab_episode ge WHERE ge.episode_id = ANY ($2::bigint[])"
	        " AND ge.open AND $1::bigint IS NOT NULL"
	        " UNION ALL SELECT 1 FROM series_grab_episode ge JOIN series_grab g ON g.id = ge.grab_id"
	        " JOIN download d ON d.id = g.download_id WHERE ge.episode_id = ANY ($2::bigint[])"
	        " AND g.closed_at IS NOT NULL AND g.removal IN ('pending', 'failed') AND d.state IN " LIVE_STATES
	        " LIMIT 1", 2, params, &flying, error))
		return FALSE;
	*in_flight = axis_db_result_get_n_rows(flying) > 0;
	return TRUE;
}

static gboolean
claimed(
	gpointer                   data,
	AxisDbConn                *conn,
	AxisAcquireWant           *want,
	const AxisAcquireGrabSpec *grab,
	gint64                     grab_id,
	GError                   **error
){
	Want *w = want->data;
	g_autoptr(GArray) ids = NULL;
	g_autofree gchar *holds = NULL;
	g_autofree gchar *array = NULL;
	g_autofree gchar *grab_text = axis_tv_int_text(grab_id);
	g_autofree gchar *episode = w->scope == AXIS_TV_SCOPE_EPISODE ? axis_tv_int_text(w->target) : NULL;
	g_autofree gchar *season = w->scope == AXIS_TV_SCOPE_SEASON ? axis_tv_int_text(w->season) : NULL;
	const gchar *params[6];

	if (!grab_episodes(conn, want, grab, &ids, &holds, error))
		return FALSE;
	array = id_array(ids);
	params[0] = grab_text;
	params[1] = axis_tv_scope_name(w->scope);
	params[2] = episode;
	params[3] = season;
	params[4] = w->job_key;
	params[5] = holds;
	if (!axis_db_conn_exec(conn, "UPDATE series_grab SET scope = $2, episode_id = $3::bigint, season_number = $4::integer,"
	        " job_key = $5, holds = left($6, 200) WHERE id = $1::bigint", 6, params, NULL, error))
		return FALSE;
	/* By force for a whole season: its files are placed by the numbers in
	 * their own names within it. */
	if (grab->bound && w->forced_season >= 0) {
		g_autofree gchar *bound_season = g_strdup_printf("%d", w->forced_season);

		params[1] = bound_season;
		if (!axis_db_conn_exec(conn, "UPDATE series_grab SET bound_season = $2::integer WHERE id = $1::bigint", 2, params,
		        NULL, error))
			return FALSE;
	}
	params[1] = array;
	/* The database's own guard: an episode in an open grab already is a
	 * unique violation here, which the shared claim takes as in flight. */
	if (!axis_db_conn_exec(conn, "INSERT INTO series_grab_episode (grab_id, episode_id)"
	        " SELECT $1::bigint, unnest($2::bigint[])", 2, params, NULL, error))
		return FALSE;
	/* An anime series' release matched through an absolute number: the
	 * history says so, with both numbers and the day, whoever grabbed it
	 * (add-anime-support, A2).  A grab by force is bound to what a person
	 * named, not to what the name gives. */
	if (w->anime.read.anime && !grab->bound && grab->name != NULL) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(grab->name);
		AxisTvHolds found;
		gboolean ok = TRUE;

		w->anime.read.name = grab->name;
		/* (As judge() read it: a batch is the episodes it maps to.  This
		 * reading only words the history line; what is claimed is what was
		 * judged, by episode id.) */
		w->anime.read.batches = TRUE;
		axis_tv_match_release_anime(parsed, w->known, w->episodes->len, &w->anime.read, &found);
		w->anime.read.name = NULL;
		w->anime.read.batches = FALSE;
		if (found.ok && found.by_absolute) {
			/* A report on the claim, which cannot fail the claim: inside a
			 * savepoint, so a line that cannot be written leaves the claim's
			 * transaction usable.  It is logged, and the grab goes on. */
			g_autoptr(GError) failure = NULL;

			ok = axis_db_conn_exec(conn, "SAVEPOINT axis_absolute_grabbed", 0, NULL, NULL, error);
			if (ok && !axis_tv_store_eventf(conn, w->series, w->scope == AXIS_TV_SCOPE_EPISODE ? w->target : 0, "changed",
			        grab->user, &failure, "The release %s was matched by its absolute number: %s (TheTVDB's numbering as "
			        "of %s). It is grabbed for %s.%s", grab->name, found.absolute_text, w->rules.today, found.label,
			        found.batch && !found.pack ? " It is a batch: each of its files is placed only on one of these "
			        "episodes, by the number in the file's own name." : "")) {
				g_message("series: the history line of a release matched by its absolute number could not be written: "
					"%s", failure != NULL ? failure->message : "unknown error");
				ok = axis_db_conn_exec(conn, "ROLLBACK TO SAVEPOINT axis_absolute_grabbed", 0, NULL, NULL, error);
			}
			ok = ok && axis_db_conn_exec(conn, "RELEASE SAVEPOINT axis_absolute_grabbed", 0, NULL, NULL, error);
		}
		axis_tv_holds_clear(&found);
		return ok;
	}
	return TRUE;
}

static gboolean
search_stored(
	gpointer          data,
	AxisDbConn       *conn,
	AxisAcquireWant  *want,
	gint64            search_id,
	GError          **error
){
	Want *w = want->data;
	g_autofree gchar *id = axis_tv_int_text(search_id);
	g_autofree gchar *episode = w->scope == AXIS_TV_SCOPE_EPISODE ? axis_tv_int_text(w->target) : NULL;
	g_autofree gchar *season = w->scope == AXIS_TV_SCOPE_SEASON ? axis_tv_int_text(w->season) : NULL;
	const gchar *params[5];

	params[0] = id;
	params[1] = axis_tv_scope_name(w->scope);
	params[2] = episode;
	params[3] = season;
	params[4] = w->job_key;
	return axis_db_conn_exec(conn, "UPDATE series_search SET scope = $2, episode_id = $3::bigint,"
		" season_number = $4::integer, job_key = $5 WHERE id = $1::bigint", 5, params, NULL, error);
}

/*
 * Whether @code (a release's reason for not being wanted, or %NULL) is one
 * of those that leave the release to a person: every search passes it over,
 * and Grab this one, from a manual search, takes it.  The codes judge() gives
 * with the words "Only Grab this one": numbered beyond the series' data, a
 * file kept by hand in its place, an absolute number read too weakly (anime),
 * downloaded before without a file for an episode (anime).
 */
static gboolean
axis_tv_reject_is_person_only(const gchar *code)
{
	return g_strcmp0(code, "not_in_data") == 0 || g_strcmp0(code, "kept_by_hand") == 0 ||
		g_strcmp0(code, "bare_number") == 0 || g_strcmp0(code, "had_before") == 0;
}

/* What every stored release of a search holds, and the search's two counts
 * of them, in two statements however many the releases are (three
 * statements a release before). */
static gboolean
candidates_stored(
	gpointer          data,
	AxisDbConn       *conn,
	AxisAcquireWant  *want,
	gint64            search_id,
	const GPtrArray  *considered,
	GError          **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *search = axis_tv_int_text(search_id);
	guint person_only = 0;
	guint mismatched = 0;
	guint rows = 0;
	guint i;

	json_builder_begin_array(builder);
	for (i = 0; i < considered->len; i++) {
		const AxisAcquireConsidered *c = g_ptr_array_index(considered, i);
		const Held *held = c->data;
		g_autofree gchar *array = NULL;

		/* A release no search takes by itself and a person may (Grab this one,
		 * from a manual search): counted on its search, which is what the page
		 * "Needs attention" reads for its line about them (a candidate's reasons
		 * are a jsonb with no index).  Not one that was left for another reason
		 * first and only says so beside it (Held.beside): taking it would not
		 * fill what was searched for. */
		if (axis_tv_reject_is_person_only(c->rejected ? c->reject_code : NULL) && (held == NULL || !held->beside))
			person_only++;
		if (held == NULL || c->candidate_id <= 0)
			continue;
		array = id_array(held->ids);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, c->candidate_id);
		json_builder_set_member_name(builder, "ids");
		json_builder_add_string_value(builder, array);
		json_builder_set_member_name(builder, "holds");
		if (held->holds != NULL)
			json_builder_add_string_value(builder, held->holds);
		else
			json_builder_add_null_value(builder);
		json_builder_set_member_name(builder, "mismatched");
		json_builder_add_boolean_value(builder, held->mismatched);
		json_builder_set_member_name(builder, "missing");
		if (held->missing != NULL)
			json_builder_add_string_value(builder, held->missing);
		else
			json_builder_add_null_value(builder);
		json_builder_end_object(builder);
		rows++;
		if (held->mismatched)
			mismatched++;
	}
	json_builder_end_array(builder);
	if (rows > 0) {
		g_autoptr(JsonNode) root = json_builder_get_root(builder);
		g_autofree gchar *json = json_to_string(root, FALSE);
		const gchar *params[1];

		params[0] = json;
		if (!axis_db_conn_exec(conn, "UPDATE series_candidate c SET episode_ids = x.ids::bigint[],"
		        " holds = left(x.holds, 200), mismatched = x.mismatched, missing = left(x.missing, 200)"
		        " FROM jsonb_to_recordset($1::jsonb) AS x(id bigint, ids text, holds text, mismatched boolean,"
		        " missing text) WHERE c.id = x.id", 1, params, NULL, error))
			return FALSE;
	}
	if (person_only > 0 || mismatched > 0) {
		g_autofree gchar *n_person = g_strdup_printf("%u", person_only);
		g_autofree gchar *n_mismatched = g_strdup_printf("%u", mismatched);
		const gchar *params[3];

		params[0] = search;
		params[1] = n_person;
		params[2] = n_mismatched;
		if (!axis_db_conn_exec(conn, "UPDATE series_search SET n_person_only = n_person_only + $2::integer,"
		        " n_mismatched = n_mismatched + $3::integer WHERE id = $1::bigint", 3, params, NULL, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
event(
	gpointer               data,
	AxisDbConn            *conn,
	gint64                 owner_id,
	const AxisAcquireWant *want,
	const gchar           *kind,
	const gchar           *user,
	gint64                 search_id,
	gint64                 grab_id,
	const gchar           *text,
	GError               **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *series = NULL;
	g_autofree gchar *label = NULL;
	const gchar *params[1];
	gint64 title_id = 0;
	gint64 year = 0;

	if (want != NULL) {
		const Want *w = want->data;

		return axis_tv_store_event(conn, owner_id, w->series->title_id, want->label,
			w->scope == AXIS_TV_SCOPE_EPISODE ? w->target : 0, kind, text, user, search_id, grab_id, error);
	}
	series = axis_tv_int_text(owner_id);
	params[0] = series;
	if (!axis_db_conn_exec(conn, "SELECT s.title_id, t.title, t.year FROM series s JOIN meta_title t ON t.id = s.title_id"
	        " WHERE s.id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0)
		return TRUE;
	axis_db_result_get_int64(row, 0, 0, &title_id);
	axis_db_result_get_int64(row, 0, 2, &year);
	label = axis_tv_series_label(axis_db_result_get_text(row, 0, 1), (gint)year);
	return axis_tv_store_event(conn, owner_id, title_id, label, 0, kind, text, user, search_id, grab_id, error);
}

/* After a failed grab: the search it came from, again (cause "retry"). */
static gboolean
retry(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *grab = axis_tv_int_text(grab_id);
	const gchar *params[1];
	AxisTvScope scope = AXIS_TV_SCOPE_EPISODE;
	gint64 item = 0;

	params[0] = grab;
	if (!axis_db_conn_exec(conn, "SELECT scope, episode_id, season_number FROM series_grab WHERE id = $1::bigint", 1,
	        params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0 || !axis_tv_scope_from_name(axis_db_result_get_text(row, 0, 0), &scope))
		return TRUE;
	if (scope == AXIS_TV_SCOPE_EPISODE)
		axis_db_result_get_int64(row, 0, 1, &item);
	else if (scope == AXIS_TV_SCOPE_SEASON)
		axis_db_result_get_int64(row, 0, 2, &item);
	if (scope == AXIS_TV_SCOPE_EPISODE && item <= 0)
		return TRUE;
	return axis_tv_store_enqueue_search(conn, owner_id, scope, item, "retry", user, NULL, error);
}

static gboolean
follow_more(
	gpointer                  data,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	return axis_tv_request_imports(data, report, error);
}

/* Whether an episode the grab was for is monitored (automatic searches look
 * for it). */
static gboolean
monitored(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        grab_id,
	gboolean     *is_monitored,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *grab = axis_tv_int_text(grab_id);
	const gchar *params[1];

	*is_monitored = FALSE;
	params[0] = grab;
	if (!axis_db_conn_exec(conn, "SELECT coalesce(bool_or(e.monitored), false) FROM series_grab_episode ge"
	        " JOIN series_episode e ON e.id = ge.episode_id WHERE ge.grab_id = $1::bigint", 1, params, &row, error))
		return FALSE;
	*is_monitored = axis_db_result_get_n_rows(row) > 0 && g_strcmp0(axis_db_result_get_text(row, 0, 0), "t") == 0;
	return TRUE;
}

static gboolean
replaced_files(
	AxisDbConn              *conn,
	const gchar             *held,
	AxisAcquireHandPreview  *preview,
	GError                 **error
);

static gboolean
force_preview(
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
);

/* What a grab by hand of a stored release would fill and replace: the
 * episodes it holds, with what the automatic rules say of each, the files
 * they have, and what it names that the data does not have. */
static gboolean
hand_preview(
	gpointer                    data,
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
){
	Want *w = want->data;
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(GString) found = g_string_new(NULL);
	g_autofree gchar *id = axis_tv_int_text(candidate->id);
	g_autofree gchar *held = NULL;
	g_auto(GStrv) ids = NULL;
	const gchar *params[1];
	guint i;

	/* By force: bound to what the person names, not to what the name gives. */
	if (preview->force)
		return force_preview(conn, want, candidate, preview, error);
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT array_to_string(episode_ids, ','), missing FROM series_candidate"
	        " WHERE id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0)
		return TRUE;
	held = g_strdup_printf("{%s}", axis_db_result_get_text(row, 0, 0) != NULL ? axis_db_result_get_text(row, 0, 0) :
		"");
	if (!replaced_files(conn, held, preview, error))
		return FALSE;
	ids = g_strsplit(axis_db_result_get_text(row, 0, 0) != NULL ? axis_db_result_get_text(row, 0, 0) : "", ",", 0);
	if (axis_db_result_get_text(row, 0, 1) != NULL)
		preview->missing = g_strdup(axis_db_result_get_text(row, 0, 1));
	for (i = 0; ids[i] != NULL; i++) {
		const AxisTvEpisode *e = ids[i][0] != '\0' ? episode_by_id(w, g_ascii_strtoll(ids[i], NULL, 10)) : NULL;
		g_autofree gchar *label = NULL;
		g_autoptr(GString) notes = NULL;

		if (ids[i][0] == '\0')
			continue;
		if (e == NULL) {
			if (preview->refused == NULL)
				preview->refused = g_strdup("An episode it holds is no longer in the series' data.");
			continue;
		}
		label = axis_tv_episode_label(e->season, e->number);
		/* In flight: said apart from every other refusal, so that the layer can
		 * tell which grabs stand in the way and whether the one can be replaced
		 * (standing(), below). */
		if (axis_tv_episode_in_flight(e) && preview->flying == NULL)
			preview->flying = g_strdup_printf("It holds %s, whose grab is in flight: nothing else is grabbed for it "
				"until that one is done with.", label);
		notes = g_string_new(NULL);
		if (e->air_date == NULL)
			g_string_append(notes, "no air date");
		if (!e->monitored)
			g_string_append_printf(notes, "%snot monitored", notes->len > 0 ? ", " : "");
		g_ptr_array_add(preview->fills, notes->len > 0 ? g_strdup_printf("%s (%s)", label, notes->str) :
			g_strdup(label));
		g_string_append_printf(found, "%s%s", found->len > 0 ? ", " : "", label);
	}
	if (preview->fills->len == 0 && preview->refused == NULL)
		preview->refused = g_strdup("It holds no episode of the series' data.");
	/* Numbered otherwise than the data: what its import will do, said
	 * before it is taken, with a tick of its own. */
	if (preview->missing != NULL && found->len > 0)
		preview->numbering = g_strdup_printf("This release numbers its episodes differently from the series' data (it "
			"names %s, which the data does not have). Files are placed by number, so they may land on the wrong "
			"episodes. A file that also names a number the data does not have is placed for the episodes of %s it "
			"names alone, and its record notes the other number; a file of such numbers only is left in the "
			"download.", preview->missing, found->str);
	return TRUE;
}

/* The files in the library of the episodes @held ("{1,2}"), each once, by the
 * quality the series' page shows (a converted file's: its source's), and
 * whether a person keeps it by hand. */
static gboolean
replaced_files(
	AxisDbConn              *conn,
	const gchar             *held,
	AxisAcquireHandPreview  *preview,
	GError                 **error
){
	g_autoptr(AxisDbResult) files = NULL;
	const gchar *params[1];
	guint i;

	params[0] = held;
	if (!axis_db_conn_exec(conn, "SELECT DISTINCT ON (lf.id) lf.id, e.season_number, e.number, sf.quality,"
	        " sf.resolution, cv.converted, cv.source_quality, sf.kept, coalesce(sf.hand_by, 'a person'),"
	        " sf.kept_how IS NOT DISTINCT FROM '" AXIS_TV_KEPT_HOW_BROUGHT "'"
	        " AND sf.hand_how IS NOT DISTINCT FROM 'brought'"
	        " FROM series_file_episode sfe"
	        " JOIN series_episode e ON e.id = sfe.episode_id JOIN series_file sf ON sf.file_id = sfe.file_id"
	        " JOIN library_file lf ON lf.id = sf.file_id"
	        " LEFT JOIN LATERAL (SELECT true AS converted, x.source_quality FROM conversion x WHERE x.file_id = lf.id"
	        "  AND x.state = 'done' AND x.outcome = 'replace' ORDER BY x.id DESC LIMIT 1) cv ON true"
	        " WHERE sfe.episode_id = ANY ($1::bigint[]) AND lf.state = 'present' AND lf.kind = 'video'"
	        " ORDER BY lf.id, e.season_number, e.number LIMIT 200", 1, params, &files, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		const gchar *quality_name = axis_db_result_get_text(files, i, 3);
		AxisQuality quality = AXIS_QUALITY_UNKNOWN;
		AxisReleaseResolution resolution = AXIS_RELEASE_RESOLUTION_NONE;
		gboolean converted = FALSE;
		gint64 file_id = 0;
		gint64 season = 0;
		gint64 number = 0;
		g_autofree gchar *label = NULL;
		g_autofree gchar *file = NULL;
		AxisAcquireReplaced *replaced;
		gboolean kept = FALSE;

		axis_db_result_get_int64(files, i, 0, &file_id);
		axis_db_result_get_int64(files, i, 1, &season);
		axis_db_result_get_int64(files, i, 2, &number);
		axis_db_result_get_boolean(files, i, 5, &converted);
		if (converted)
			quality_name = axis_db_result_get_text(files, i, 6);
		if (quality_name == NULL || !axis_quality_from_name(quality_name, &quality)) {
			quality = AXIS_QUALITY_UNKNOWN;
			if (!converted)
				resolution = axis_identify_resolution_from_name(axis_db_result_get_text(files, i, 4));
		}
		label = axis_tv_episode_label((gint)season, (gint)number);
		file = g_strdup_printf("%s's file", label);
		replaced = axis_acquire_replaced_new(file_id, file, quality, resolution);
		/* Kept by hand: replaced only with the tick that names it. */
		axis_db_result_get_boolean(files, i, 7, &kept);
		if (kept) {
			gboolean brought = FALSE;

			axis_db_result_get_boolean(files, i, 9, &brought);
			replaced->kept_by = g_strdup(axis_db_result_get_text(files, i, 8));
			/* Kept because it was migrated: no person is named. */
			replaced->kept_from = brought ? "Sonarr" : NULL;
		}
		g_ptr_array_add(preview->replaces, replaced);
	}
	return TRUE;
}

/* "season 1", "the specials". */
static gchar *
season_words(gint season)
{
	return season == 0 ? g_strdup("the specials") : g_strdup_printf("season %d", season);
}

/*
 * What a grab by force of @candidate would do (add-force-grab-and-assign,
 * F2).  The release's name cannot say what it is, so the person does: the
 * episodes they tick, or a whole season.  Until they have named anything the
 * page proposes what the name gives when the series' data has it, else what
 * the search was for (the episode of an episode search, the season of a
 * season search), and nothing for a search of the whole series.  What is
 * resolved here is what the claim takes (the want's forced_ids).
 */
static gboolean
force_preview(
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
){
	Want *w = want->data;
	g_autoptr(AxisDbResult) under = NULL;
	g_autoptr(AxisRelease) parsed = axis_release_parse(candidate->name);
	g_autoptr(GHashTable) picked = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) more = g_string_new(NULL);
	g_autoptr(GString) label = g_string_new(NULL);
	g_autoptr(GString) said = g_string_new(NULL);
	g_autofree gchar *series = axis_tv_int_text(want->owner_id);
	g_autofree gchar *id = axis_tv_int_text(candidate->id);
	g_autofree gchar *busy = NULL;
	g_autofree gchar *held = NULL;
	const AxisTvEpisode *searched = w->scope == AXIS_TV_SCOPE_EPISODE ? episode_by_id(w, w->target) : NULL;
	const gchar *params[2];
	AxisTvHolds holds;
	gint level = w->scope == AXIS_TV_SCOPE_SEASON ? w->season : searched != NULL ? searched->season : -1;
	gint season = -1;
	gint last_season = -1;
	guint n_season = 0;
	guint n = 0;
	guint i;

	preview->choices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_choice_free);
	/* Found under another numbering than the series follows now: what it was
	 * listed against is not what the picker would show. */
	params[0] = series;
	params[1] = id;
	if (!axis_db_conn_exec(conn, "SELECT s.episode_source, y.source FROM series s, series_candidate c"
	        " JOIN series_search y ON y.id = c.search_id WHERE s.id = $1::bigint AND c.id = $2::bigint", 2, params,
	        &under, error))
		return FALSE;
	if (axis_db_result_get_n_rows(under) > 0 && axis_db_result_get_text(under, 0, 0) != NULL &&
	    g_strcmp0(axis_db_result_get_text(under, 0, 0), axis_db_result_get_text(under, 0, 1)) != 0) {
		const gchar *was = axis_db_result_get_text(under, 0, 1);

		preview->refused = g_strdup_printf("This release was found while the series was numbered as %s numbers it; it "
			"follows %s now. Search again, then grab it by force from the new list.", was != NULL ?
			axis_tv_source_label(was) : "another source", axis_tv_source_label(axis_db_result_get_text(under, 0, 0)));
		g_clear_pointer(&preview->choices, g_ptr_array_unref);
		return TRUE;
	}
	/* Not while an import of the series is open: its files may be the very
	 * episodes named (the predicate placing and the numbering switch use). */
	if (!axis_tv_import_open(conn, want->owner_id, &busy, error))
		return FALSE;
	if (busy != NULL) {
		preview->refused = g_strdup_printf("%s Nothing is grabbed by force for the series until then.", busy);
		g_clear_pointer(&preview->choices, g_ptr_array_unref);
		return TRUE;
	}

	/* What the name gives of the series' data, and what it says in its own
	 * numbering (which is set aside). */
	w->anime.read.name = w->anime.read.anime ? candidate->name : NULL;
	axis_tv_match_release_anime(parsed, w->known, w->episodes->len, &w->anime.read, &holds);
	w->anime.read.name = NULL;
	if (parsed->season_first >= 0 && parsed->season_last <= parsed->season_first) {
		if (parsed->episode_count == 0)
			g_string_append_printf(said, "S%02d", parsed->season_first);
		for (i = 0; i < parsed->episode_count && i < AXIS_RELEASE_MAX_NUMBERS; i++)
			g_string_append_printf(said, "%sS%02dE%02d", i > 0 ? ", " : "", parsed->season_first, parsed->episodes[i]);
	}

	/* What it is bound to. */
	if (w->named_set) {
		if (w->named_whole && w->named != NULL && w->named->len > 0) {
			preview->unnamed = TRUE;
			preview->pick_help = g_strdup("Choose either episodes or a whole season, not both.");
		} else if (w->named_whole) {
			season = w->named_season;
		} else if (w->named != NULL && w->named->len > 0) {
			for (i = 0; i < w->named->len; i++) {
				const AxisTvEpisode *e = episode_by_id(w, g_array_index(w->named, gint64, i));

				if (e == NULL) {
					preview->refused = g_strdup("An episode named is no longer in the series' data: open the manual "
						"search again.");
					axis_tv_holds_clear(&holds);
					return TRUE;
				}
				g_hash_table_add(picked, (gpointer)&e->id);
			}
		} else {
			preview->unnamed = TRUE;
			preview->pick_help = g_strdup("Say what this release is: tick its episodes, or choose a whole season.");
		}
	} else if (holds.ok && holds.pack && holds.season_first == holds.season_last) {
		season = holds.season_first;
	} else if ((holds.ok && !holds.pack) || (!holds.ok && holds.found != NULL && holds.found->len > 0)) {
		const GArray *indices = holds.ok ? holds.indices : holds.found;

		for (i = 0; i < indices->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(w->episodes, g_array_index(indices, guint, i));

			g_hash_table_add(picked, (gpointer)&e->id);
		}
	} else if (searched != NULL) {
		g_hash_table_add(picked, (gpointer)&searched->id);
	} else if (w->scope == AXIS_TV_SCOPE_SEASON) {
		season = w->season;
	} else {
		preview->unnamed = TRUE;
		preview->pick_help = g_strdup("Say what this release is: tick its episodes, or choose a whole season.");
	}
	axis_tv_holds_clear(&holds);
	/* A whole season outside the level searched is not among the choices, so
	 * a grab bound to it could not be posted: say why, and where to go. */
	if (season >= 0 && level >= 0 && season != level) {
		g_autofree gchar *there = season_words(season);
		g_autofree gchar *here = season_words(level);

		preview->unnamed = TRUE;
		preview->pick_help = g_strdup_printf("This release looks like a whole %s, but this search is for %s, and a "
			"whole season is offered only from a search of that season. Open the manual search of %s to grab it by "
			"force as that season, or tick episodes of %s below.", there, here, there, here);
		preview->pick_link = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=season&item=%d", want->owner_id,
			season);
		preview->pick_link_text = g_strdup_printf("Manual search of %s", there);
		season = -1;
	}
	/* A whole season: every episode of it the data has. */
	for (i = 0; season >= 0 && i < w->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(w->episodes, i);

		if (e->season == season)
			g_hash_table_add(picked, (gpointer)&e->id);
	}
	if (season >= 0 && g_hash_table_size(picked) == 0) {
		preview->unnamed = TRUE;
		preview->pick_help = g_strdup_printf("The series' data has no season %d.", season);
		season = -1;
	}

	if (level < 0 && w->episodes->len > AXIS_ACQUIRE_FORCE_PICK_MAX) {
		preview->refused = g_strdup_printf("The series has %u episodes, more than a page can offer to tick (%d). Search "
			"one season, then grab it by force from that list.", w->episodes->len, AXIS_ACQUIRE_FORCE_PICK_MAX);
		g_clear_pointer(&preview->choices, g_ptr_array_unref);
		return TRUE;
	}
	/* The picker: the episodes of the level searched and whatever is picked
	 * besides, then their seasons whole.  In the order of the data. */
	g_string_append_printf(more, "src:%s|season:%d", w->series->episode_source != NULL ? w->series->episode_source :
		"", season);
	for (i = 0; i < w->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(w->episodes, i);
		gboolean is_picked = g_hash_table_contains(picked, &e->id);
		g_autofree gchar *number = axis_tv_episode_label(e->season, e->number);

		if (is_picked) {
			g_autoptr(GString) notes = g_string_new(NULL);

			/* (Said apart, as in hand_preview(): see standing().) */
			if (axis_tv_episode_in_flight(e) && preview->flying == NULL)
				preview->flying = g_strdup_printf("%s is in flight: a grab of it is open, or a download of it that "
					"Axis gave up on is still at its client. Nothing else is grabbed for it until that is done with.",
					number);
			if (e->air_date == NULL)
				g_string_append(notes, "no air date");
			if (!e->monitored)
				g_string_append_printf(notes, "%snot monitored", notes->len > 0 ? ", " : "");
			g_ptr_array_add(preview->fills, notes->len > 0 ? g_strdup_printf("%s (%s)", number, notes->str) :
				g_strdup(number));
			g_array_append_val(ids, e->id);
			g_string_append_printf(more, "|%" G_GINT64_FORMAT ":%d", e->id, e->monitored);
			if (n < 12)
				g_string_append_printf(label, "%s%s", n > 0 ? ", " : "", number);
			n++;
		}
		if (level >= 0 && e->season != level && !is_picked)
			continue;
		{
			g_autofree gchar *text = e->title != NULL ? g_strdup_printf("%s, %s", number, e->title) :
				g_strdup(number);

			g_ptr_array_add(preview->choices, axis_acquire_choice_new(e->id, text, FALSE, is_picked && season < 0));
		}
	}
	/* The seasons of the episodes offered, each whole. */
	for (i = 0; i <= w->episodes->len; i++) {
		const AxisTvEpisode *e = i < w->episodes->len ? g_ptr_array_index(w->episodes, i) : NULL;

		if (e != NULL && level >= 0 && e->season != level)
			continue;
		if (e != NULL && e->season == last_season) {
			n_season++;
			continue;
		}
		if (last_season >= 0) {
			g_autofree gchar *words = season_words(last_season);
			g_autofree gchar *text = g_strdup_printf("All of %s (%u episode%s)", words, n_season, n_season == 1 ? "" :
				"s");

			g_ptr_array_add(preview->choices, axis_acquire_choice_new(last_season, text, TRUE,
				season == last_season));
		}
		last_season = e != NULL ? e->season : -1;
		n_season = 1;
	}
	if (preview->unnamed || preview->refused != NULL)
		return TRUE;

	/* What it is bound to, in words; what the claim takes. */
	if (season >= 0) {
		g_string_truncate(label, 0);
		g_string_append_printf(label, "%s", season == 0 ? "the specials" : "season ");
		if (season > 0)
			g_string_append_printf(label, "%d", season);
	} else if (n > 12) {
		g_string_append_printf(label, " and %u more", n - 12);
	}
	preview->bound_text = g_strdup_printf("%s of %s", label->str, want->label);
	g_clear_pointer(&w->forced_ids, g_array_unref);
	w->forced_ids = g_array_ref(ids);
	w->forced_season = season;
	g_free(w->forced_label);
	w->forced_label = g_strdup(label->str);
	preview->digest_more = g_strdup(more->str);
	held = id_array(ids);
	if (!replaced_files(conn, held, preview, error))
		return FALSE;

	/* What its import does. */
	{
		gboolean same = season >= 0 ? parsed->season_first == season && parsed->episode_count == 0 :
			strcmp(said->str, label->str) == 0;
		g_autofree gchar *own = said->len > 0 && !same ? g_strdup_printf(" The release's own numbering (%s) is set "
			"aside.", said->str) : g_strdup("");
		g_autofree gchar *how = season >= 0 ? g_strdup_printf("When it has downloaded, each video file is placed by "
				"the episode its own name states in %s: the series' name in a file's name is not judged. A name "
				"that states a season the series has is read as it says, so a file of another season is left; only a "
				"season the series does not have (the release's own numbering) is read as %s, and then by the "
				"episode number alone. A file whose name gives no episode of %s is left in the download for you to "
				"assign by hand; nothing is guessed.", label->str, label->str, label->str) :
			n > 1 ? g_strdup_printf("When it has downloaded, each video file is placed by the episode its own name "
				"states among %s (a double episode's name states both). If you tick that one video file holds all of "
				"them, the download's one video file is imported as all of them without reading its name. A file whose "
				"name states none of them is left in the download for you to assign by hand; nothing is guessed.",
				label->str) :
			g_strdup_printf("When it has downloaded, its one video file is imported as %s without reading its name. "
				"If the download holds several video files, each is placed by the episode number in its own name "
				"among %s, and a file whose name gives none of them is left in the download for you to assign by "
				"hand; nothing is guessed.", label->str, label->str);

		preview->import_text = g_strdup_printf("%s%s Each file must still be a video that can be read, and its runtime "
			"is compared with the episode's. The quality profile %s is set aside. The files are then kept by hand: no "
			"automatic search replaces them until you let them go on the series' page.", how, own,
			want->profile_name != NULL ? want->profile_name : "");
	}
	return TRUE;
}

/*
 * The grabs that stand in the way of a grab by hand of @candidate
 * (add-replace-grab-and-delete-download, slice A): every open grab that
 * holds an episode the release is for, and every closed one of those whose
 * download is still being removed from its client; each with what it holds
 * and the episodes it holds that the release is not for (those stop
 * downloading too when the grab is replaced: a grab is ended whole).  The
 * episodes the release is for: what it holds by its name, or by force what
 * the person named (force_preview() resolved them).
 */
static gboolean
standing(
	gpointer                      data,
	AxisDbConn                   *conn,
	AxisAcquireWant              *want,
	const AxisAcquireCandidate   *candidate,
	const AxisAcquireHandPreview *preview,
	GPtrArray                    *out,
	GError                      **error
){
	const Want *w = want->data;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *series = axis_tv_int_text(want->owner_id);
	g_autofree gchar *array = NULL;
	const gchar *params[2];
	guint i;

	if (preview->force) {
		if (w->forced_ids == NULL || w->forced_ids->len == 0)
			return TRUE;
		array = id_array(w->forced_ids);
	} else {
		g_autoptr(AxisDbResult) row = NULL;
		g_autofree gchar *id = axis_tv_int_text(candidate->id);

		params[0] = id;
		if (!axis_db_conn_exec(conn, "SELECT array_to_string(episode_ids, ',') FROM series_candidate"
		        " WHERE id = $1::bigint", 1, params, &row, error))
			return FALSE;
		if (axis_db_result_get_n_rows(row) == 0 || axis_db_result_get_text(row, 0, 0) == NULL ||
		    axis_db_result_get_text(row, 0, 0)[0] == '\0')
			return TRUE;
		array = g_strdup_printf("{%s}", axis_db_result_get_text(row, 0, 0));
	}
	params[0] = series;
	params[1] = array;
	if (!axis_db_conn_exec(conn, "SELECT g.id, coalesce(g.holds, (SELECT string_agg('S' ||"
	        " lpad(e.season_number::text, 2, '0') || 'E' || lpad(e.number::text, 2, '0'), ', '"
	        " ORDER BY e.season_number, e.number) FROM series_grab_episode x JOIN series_episode e ON e.id = x.episode_id"
	        " WHERE x.grab_id = g.id)),"
	        " (SELECT string_agg('S' || lpad(e.season_number::text, 2, '0') || 'E' || lpad(e.number::text, 2, '0'), ', '"
	        " ORDER BY e.season_number, e.number) FROM series_grab_episode x JOIN series_episode e ON e.id = x.episode_id"
	        " WHERE x.grab_id = g.id AND NOT (x.episode_id = ANY ($2::bigint[])))"
	        " FROM series_grab g WHERE g.series_id = $1::bigint"
	        " AND EXISTS (SELECT 1 FROM series_grab_episode ge WHERE ge.grab_id = g.id"
	        "  AND ge.episode_id = ANY ($2::bigint[]))"
	        " AND (g.closed_at IS NULL OR (g.removal IN ('pending', 'failed') AND EXISTS (SELECT 1 FROM download d"
	        "  WHERE d.id = g.download_id AND d.state IN " LIVE_STATES "))) ORDER BY g.id LIMIT 50", 2, params, &rows,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 id = 0;

		axis_db_result_get_int64(rows, i, 0, &id);
		g_ptr_array_add(out, axis_acquire_standing_new(id, axis_db_result_get_text(rows, i, 1),
			axis_db_result_get_text(rows, i, 2)));
	}
	return TRUE;
}

static const AxisAcquireOwnerInfo acquire_info = {
	AXIS_TV_MODULE_ID,
	"series",
	"series",
	"series_id",
	"series_search",
	"series_candidate",
	"series_grab",
	"series_blocklist",
	"series_episode",
	"s.job_key",
	"g.job_key",
	AXIS_TV_SETTING_FLOOR,
	AXIS_TV_SETTING_FLOOR_MAX,
	AXIS_TV_SETTING_GIVE_UP,
	AXIS_TV_SETTING_PROTOCOL,
	AXIS_TV_SETTING_RETENTION,
	AXIS_TV_SETTING_LANGUAGES,
	TRUE,
	load,
	NULL,
	prepare,
	judge,
	search_stored,
	candidates_stored,
	claim_check,
	claimed,
	event,
	retry,
	follow_more,
	monitored,
	hand_preview,
	TRUE,	/* force: Grab it by force is offered, for the episodes a person names */
	unmatched,
	standing
};

const AxisAcquireOwnerInfo *
axis_tv_acquire_info(void)
{
	return &acquire_info;
}

/* ── The job's work ─────────────────────────────────────────────── */

gboolean
axis_tv_run_search(
	AxisTv                   *self,
	gint64                    series_id,
	AxisTvScope               scope,
	gint64                    item,
	const gchar              *cause,
	const gchar              *user,
	GCancellable             *cancellable,
	AxisAcquireSearchReport  *report_out,
	GError                  **error
){
	AxisAcquireSearchReport report;
	AxisAcquireTarget target;
	gboolean fall_back;

	memset(&target, 0, sizeof(target));
	target.owner_id = series_id;
	target.scope = axis_tv_scope_name(scope);
	target.item_id = item;
	/* What lets it pass a brought series' hold (load()). */
	target.person = axis_tv_cause_is_person(cause);
	if (!axis_acquire_run_search(self->acquire, &target, cause, user, cancellable, &report, error))
		return FALSE;
	if (report_out != NULL)
		*report_out = report;
	/* No whole season (or several) to be had: the narrower searches.  (A
	 * manual search lists, and a person chooses: nothing follows it.) */
	fall_back = scope != AXIS_TV_SCOPE_EPISODE && report.searched && (g_strcmp0(report.outcome, "nothing_found") == 0 ||
		g_strcmp0(report.outcome, "none_acceptable") == 0 || g_strcmp0(report.outcome, "exhausted") == 0);
	if (fall_back) {
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
		guint queued = 0;
		gboolean ok;

		if (conn == NULL)
			return FALSE;
		ok = axis_tv_queue_narrower(self, conn, series_id, scope, item, cause, user, &queued, error);
		axis_db_pool_release(self->pool, conn);
		return ok;
	}
	return TRUE;
}

/* ── Another profile, tried on what a search found ──────────────── */

gboolean
axis_tv_try_profiles(
	AxisTv           *self,
	gint64            series_id,
	gint64            search_id,
	const GPtrArray  *candidates,
	const GArray     *profile_ids,
	GPtrArray       **tries,
	GError          **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *search = axis_tv_int_text(search_id);
	g_autofree gchar *series = axis_tv_int_text(series_id);
	AxisAcquireTarget target;
	const gchar *params[2];
	gint64 episode = 0;
	gint64 season = 0;

	*tries = NULL;
	/* What the search was for: the releases are read against it again. */
	params[0] = search;
	params[1] = series;
	if (!axis_db_pool_exec(self->pool, "SELECT scope, episode_id, season_number FROM series_search WHERE id = $1::bigint"
	        " AND series_id = $2::bigint", 2, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that search is not the series'");
		return FALSE;
	}
	memset(&target, 0, sizeof(target));
	target.owner_id = series_id;
	/* A person's look at what another profile would take: no search, and
	 * nothing a brought series' hold is there to stop. */
	target.person = TRUE;
	target.scope = g_strcmp0(axis_db_result_get_text(row, 0, 0), "season") == 0 ? "season" :
		g_strcmp0(axis_db_result_get_text(row, 0, 0), "series") == 0 ? "series" : "episode";
	axis_db_result_get_int64(row, 0, 1, &episode);
	axis_db_result_get_int64(row, 0, 2, &season);
	target.item_id = strcmp(target.scope, "episode") == 0 ? episode : strcmp(target.scope, "season") == 0 ? season : 0;
	if (!axis_acquire_try_profiles(self->acquire, &target, candidates, profile_ids, tries, error)) {
		if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(error);
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		}
		return FALSE;
	}
	return TRUE;
}

/* ── An administrator's grab ────────────────────────────────────── */

/* What the search that found a stored release was for. */
static gboolean
target_of(
	AxisTv             *self,
	gint64              series_id,
	gint64              candidate_id,
	AxisAcquireTarget  *target,
	GError            **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *candidate = axis_tv_int_text(candidate_id);
	g_autofree gchar *series = axis_tv_int_text(series_id);
	const gchar *params[2];
	gint64 episode = 0;
	gint64 season = 0;

	params[0] = candidate;
	params[1] = series;
	if (!axis_db_pool_exec(self->pool, "SELECT s.scope, s.episode_id, s.season_number FROM series_candidate c"
	        " JOIN series_search s ON s.id = c.search_id WHERE c.id = $1::bigint AND s.series_id = $2::bigint", 2, params,
	        &row, error))
		return FALSE;
	memset(target, 0, sizeof(*target));
	target->owner_id = series_id;
	target->scope = "episode";
	/* A release a person picks from a search's list: theirs, so a brought
	 * series' hold does not stop it (load()). */
	target->person = TRUE;
	if (axis_db_result_get_n_rows(row) > 0) {
		target->scope = g_strcmp0(axis_db_result_get_text(row, 0, 0), "season") == 0 ? "season" :
			g_strcmp0(axis_db_result_get_text(row, 0, 0), "series") == 0 ? "series" : "episode";
		axis_db_result_get_int64(row, 0, 1, &episode);
		axis_db_result_get_int64(row, 0, 2, &season);
		target->item_id = strcmp(target->scope, "episode") == 0 ? episode : strcmp(target->scope, "season") == 0 ?
			season : 0;
	}
	return TRUE;
}

static gboolean
grab_failed(
	AxisAcquireGrabResult   result,
	const gchar            *problem,
	GError                **error
);

gboolean
axis_tv_grab(
	AxisTv       *self,
	gint64        series_id,
	gint64        candidate_id,
	const gchar  *user,
	gint64       *grab_id,
	GError      **error
){
	g_autofree gchar *problem = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireTarget target;

	if (!target_of(self, series_id, candidate_id, &target, error) ||
	    !axis_acquire_grab(self->acquire, &target, candidate_id, user, grab_id, &result, &problem, error))
		return FALSE;
	return grab_failed(result, problem, error);
}

gboolean
axis_tv_grab_by_hand(
	AxisTv                      *self,
	gint64                       series_id,
	gint64                       candidate_id,
	const AxisAcquireHandTicks  *ticks,
	const gchar                 *user,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result_out,
	GError                     **error
){
	g_autofree gchar *problem = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireTarget target;

	if (!target_of(self, series_id, candidate_id, &target, error) ||
	    !axis_acquire_grab_by_hand(self->acquire, &target, candidate_id, user, ticks, grabbed, &result, &problem,
	        error))
		return FALSE;
	if (result_out != NULL)
		*result_out = result;
	/* A replace of the download in flight that sent nothing: the layer's own
	 * words (which download, why, and what happens next). */
	if (result == AXIS_ACQUIRE_GRAB_IN_FLIGHT && problem != NULL && ticks != NULL && ticks->in_flight != NULL &&
	    ticks->in_flight->grab_id > 0) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "%s", problem);
		return FALSE;
	}
	return grab_failed(result, problem, error);
}

gboolean
axis_tv_hand_preview(
	AxisTv                   *self,
	gint64                    series_id,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
){
	AxisAcquireTarget target;

	if (!target_of(self, series_id, candidate_id, &target, error))
		return FALSE;
	if (!axis_acquire_hand_preview(self->acquire, &target, candidate_id, preview, error)) {
		if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(error);
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that release is not one of the "
				"series' searches'");
		}
		return FALSE;
	}
	return TRUE;
}

/* ── Grab it by force (add-force-grab-and-assign, F2) ───────────── */

static void
pick_into(
	const AxisTvForcePick *pick,
	AxisAcquireTarget     *target
){
	if (pick == NULL || !pick->named)
		return;
	target->named_set = TRUE;
	target->named = pick->episodes;
	target->named_whole = pick->whole_season;
	target->named_part = pick->season;
}

gboolean
axis_tv_force_preview(
	AxisTv                   *self,
	gint64                    series_id,
	gint64                    candidate_id,
	const AxisTvForcePick    *pick,
	AxisAcquireHandPreview  **preview,
	GError                  **error
){
	AxisAcquireTarget target;

	if (!target_of(self, series_id, candidate_id, &target, error))
		return FALSE;
	pick_into(pick, &target);
	if (!axis_acquire_force_preview(self->acquire, &target, candidate_id, preview, error)) {
		if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(error);
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that release is not one of the "
				"series' searches'");
		}
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_tv_grab_by_force(
	AxisTv                       *self,
	gint64                        series_id,
	gint64                        candidate_id,
	const AxisTvForcePick        *pick,
	const AxisAcquireForceTicks  *ticks,
	const gchar                  *user,
	AxisAcquireHandGrabbed       *grabbed,
	AxisAcquireGrabResult        *result_out,
	GError                      **error
){
	g_autofree gchar *problem = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireTarget target;
	AxisTvForcePick none;

	/* The grab is bound to what was named, and to nothing the page would only
	 * have proposed. */
	memset(&none, 0, sizeof(none));
	none.named = TRUE;
	if (pick == NULL || !pick->named)
		pick = &none;
	if (!target_of(self, series_id, candidate_id, &target, error))
		return FALSE;
	pick_into(pick, &target);
	if (!axis_acquire_grab_by_force(self->acquire, &target, candidate_id, user, ticks, grabbed, &result, &problem,
	        error))
		return FALSE;
	if (result_out != NULL)
		*result_out = result;
	/* By force, a refusal is a rule that protects the system (or no fixed
	 * rule at all), in its own words. */
	if (result == AXIS_ACQUIRE_GRAB_REFUSED) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED, "this release cannot be grabbed by force: %s",
			problem != NULL ? problem : "a fixed rule that protects the system rules it out");
		return FALSE;
	}
	if (result == AXIS_ACQUIRE_GRAB_IN_FLIGHT && problem != NULL) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "%s", problem);
		return FALSE;
	}
	return grab_failed(result, problem, error);
}

gboolean
axis_tv_set_kept(
	AxisTv       *self,
	gint64        series_id,
	gint64        file_id,
	gboolean      kept,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *series = axis_tv_int_text(series_id);
	g_autofree gchar *file = axis_tv_int_text(file_id);
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	g_autofree gchar *label = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[4];
	const gchar *episodes;
	AxisDbConn *conn;
	gint64 title_id = 0;
	gint64 year = 0;
	gboolean was_brought = FALSE;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = series;
	params[1] = file;
	params[2] = kept ? "true" : "false";
	params[3] = by;
	/* A file kept by a person who made no link keeps how it was linked (its
	 * hand_how stays as it is): only the keeping is theirs, said in the
	 * history.  So is the keeping, or the letting go, of a file that was kept
	 * because it was migrated: kept_how goes, and "Let Axis upgrade these
	 * files" on its connection's page never touches it again. */
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "UPDATE series_file sf SET kept = $3::boolean, kept_how = NULL,"
			" hand_by = CASE WHEN $3::boolean"
			" THEN coalesce($4::text, sf.hand_by) ELSE sf.hand_by END FROM series s JOIN meta_title t"
			/* (How it was kept until now, for the history's words.) */
			" ON t.id = s.title_id, (SELECT o.file_id, o.kept_how FROM series_file o WHERE o.file_id = $2::bigint) was"
			" WHERE sf.series_id = $1::bigint AND sf.file_id = $2::bigint AND s.id = sf.series_id"
			" AND was.file_id = sf.file_id"
			" RETURNING s.title_id, t.title, t.year, (SELECT string_agg('S' || lpad(e.season_number::text, 2, '0')"
			" || 'E' || lpad(e.number::text, 2, '0'), ', ' ORDER BY e.season_number, e.number)"
			" FROM series_file_episode sfe JOIN series_episode e ON e.id = sfe.episode_id"
			" WHERE sfe.file_id = sf.file_id), coalesce(was.kept_how = '" AXIS_TV_KEPT_HOW_BROUGHT "', false)", 4, params,
			&row, error);
	if (ok && axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that file is not this series'");
		ok = FALSE;
	}
	if (ok) {
		axis_db_result_get_int64(row, 0, 0, &title_id);
		axis_db_result_get_int64(row, 0, 2, &year);
		label = axis_tv_series_label(axis_db_result_get_text(row, 0, 1), (gint)year);
		episodes = axis_db_result_get_text(row, 0, 3) != NULL ? axis_db_result_get_text(row, 0, 3) : "an episode";
		axis_db_result_get_boolean(row, 0, 4, &was_brought);
		text = kept ? g_strdup_printf("The file of %s is kept by hand%s%s: no automatic search and no Search now "
				"replaces it until a person lets it go.", episodes, by != NULL ? " by " : "", by != NULL ? by : "") :
			was_brought ? g_strdup_printf("The file of %s, kept since it was migrated, is let go%s%s: it is "
				"upgraded by the ordinary rules again.", episodes, by != NULL ? " by " : "", by != NULL ? by : "") :
			g_strdup_printf("The file of %s is no longer kept by hand%s%s: it is upgraded by the ordinary rules again.",
				episodes, by != NULL ? ", let go by " : "", by != NULL ? by : "");
		ok = axis_tv_store_event(conn, series_id, title_id, label, 0, "hand_link", text, by, 0, 0, error) &&
			axis_db_conn_commit(conn, error);
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* SENT: %TRUE; anything else in the series' words. */
static gboolean
grab_failed(
	AxisAcquireGrabResult   result,
	const gchar            *problem,
	GError                **error
){
	switch (result) {
	case AXIS_ACQUIRE_GRAB_SENT:
		return TRUE;
	case AXIS_ACQUIRE_GRAB_NOT_FOUND:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		break;
	case AXIS_ACQUIRE_GRAB_NO_CANDIDATE:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that release is not one of the series' "
			"searches'");
		break;
	case AXIS_ACQUIRE_GRAB_REFUSED:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED, "a fixed rule rejects this release, and no one may "
			"grab it: %s", problem != NULL ? problem : "it is not certainly episodes of this series.");
		break;
	case AXIS_ACQUIRE_GRAB_BLOCKLISTED:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_BLOCKLISTED, "this release is on the series' "
			"blocklist: take it off the blocklist first");
		break;
	case AXIS_ACQUIRE_GRAB_IN_FLIGHT:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "a grab of an episode it holds is in "
			"flight already");
		break;
	case AXIS_ACQUIRE_GRAB_NEEDS_TICK:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED, "%s", problem != NULL ? problem : "it needs a tick "
			"of its own. Nothing was sent");
		break;
	case AXIS_ACQUIRE_GRAB_CHANGED:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED, "%s", problem != NULL ? problem : "what its "
			"confirmation showed has changed. Nothing was sent");
		break;
	case AXIS_ACQUIRE_GRAB_REPLACE_STUCK:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, "%s", problem != NULL ? problem : "the download "
			"in flight could not be replaced. Nothing was sent");
		break;
	case AXIS_ACQUIRE_GRAB_OFFLINE:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_UNUSABLE, "the series' destination is not online: "
			"nothing is sent until it is back");
		break;
	case AXIS_ACQUIRE_GRAB_EXPIRED:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_EXPIRED, "the search that found it has expired: "
			"search again");
		break;
	case AXIS_ACQUIRE_GRAB_GONE:
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series or one of the episodes it "
			"holds was removed meanwhile");
		break;
	case AXIS_ACQUIRE_GRAB_NO_CLIENT:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_UNUSABLE, "%s", problem != NULL ? problem :
			"no usable download client takes it");
		break;
	default:
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_FAILED, "%s", problem != NULL ? problem :
			"it could not be sent");
		break;
	}
	return FALSE;
}
