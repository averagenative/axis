/*
 * axis-movies-state.c - a film's states, and the one function that derives
 * a film's state from its facts
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure: plain structures in, a state and a sentence out.  The facts come
 * from axis_movies_store_facts() (axis-movies-store.c), the only query that
 * reads them; nothing stores a state.
 */

#include "modules/movies/axis-movies-private.h"

#include <stdio.h>
#include <string.h>

G_DEFINE_QUARK(axis-movies-error-quark, axis_movies_error)

/* ── Names ──────────────────────────────────────────────────────── */

static const struct
{
	const gchar *name;
	const gchar *label;
	const gchar *mark;
} states[AXIS_MOVIE_N_STATES] = {
	{ "not_available", "Not available yet", "Not available yet" },
	{ "wanted", "Wanted", "Wanted" },
	{ "searching", "Searching", "Searching" },
	{ "downloading", "Downloading", "Downloading" },
	{ "to_import", "To import", "To import" },
	{ "in_library", "In library", "In library" },
	{ "upgrade_wanted", "Upgrade wanted", "Upgrade wanted" },
	{ "attention", "Needs attention", "Needs attention" }
};

const gchar *
axis_movie_state_name(AxisMovieState state)
{
	return (guint)state < AXIS_MOVIE_N_STATES ? states[state].name : NULL;
}

gboolean
axis_movie_state_from_name(
	const gchar    *name,
	AxisMovieState *state
){
	guint i;

	for (i = 0; name != NULL && i < AXIS_MOVIE_N_STATES; i++) {
		if (strcmp(states[i].name, name) == 0) {
			*state = (AxisMovieState)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_movie_state_label(AxisMovieState state)
{
	return (guint)state < AXIS_MOVIE_N_STATES ? states[state].label : "Unknown";
}

const gchar *
axis_movie_state_short(AxisMovieState state)
{
	return (guint)state < AXIS_MOVIE_N_STATES ? states[state].mark : "Unknown";
}

static const struct
{
	const gchar *name;
	const gchar *label;
} availabilities[] = {
	{ "announced", "When announced" },
	{ "in_cinemas", "In cinemas" },
	{ "released", "Released" }
};

const gchar *
axis_movie_availability_name(AxisMovieAvailability availability)
{
	return (guint)availability < G_N_ELEMENTS(availabilities) ? availabilities[availability].name : NULL;
}

gboolean
axis_movie_availability_from_name(
	const gchar           *name,
	AxisMovieAvailability *availability
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(availabilities); i++) {
		if (strcmp(availabilities[i].name, name) == 0) {
			*availability = (AxisMovieAvailability)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_movie_availability_label(AxisMovieAvailability availability)
{
	return (guint)availability < G_N_ELEMENTS(availabilities) ? availabilities[availability].label : "Released";
}

/* ── Facts and rules ────────────────────────────────────────────── */

void
axis_movie_facts_free(AxisMovieFacts *facts)
{
	if (facts == NULL)
		return;
	g_free(facts->title);
	g_free(facts->original_title);
	g_free(facts->release_date);
	g_free(facts->poster_ref);
	g_free(facts->provider);
	g_free(facts->profile_name);
	g_free(facts->profile_slug);
	g_free(facts->root_name);
	g_free(facts->volume_name);
	g_free(facts->added_by);
	g_free(facts->grab_release);
	g_free(facts->grab_quality);
	g_free(facts->grab_by);
	g_free(facts->download_problem);
	g_free(facts->download_message);
	g_free(facts->client_name);
	g_free(facts->download_volume);
	g_free(facts->download_rel_path);
	g_free(facts->download_client_path);
	g_free(facts->search_outcome);
	g_free(facts->search_text);
	g_free(facts->import_state);
	g_free(facts->import_reason);
	g_free(facts->import_text);
	g_free(facts->away_volume);
	g_free(facts->kept_by);
	g_free(facts->held_forced);
	g_free(facts->search_hold);
	g_free(facts);
}

void
axis_movies_today(
	gint64 now_us,
	gchar  today[11]
){
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_utc(now_us / G_USEC_PER_SEC);
	g_autofree gchar *text = now != NULL ? g_date_time_format(now, "%Y-%m-%d") : NULL;

	g_strlcpy(today, text != NULL ? text : "1970-01-01", 11);
}

void
axis_movie_rules_init(
	AxisMovieRules *rules,
	gint64          now_us
){
	memset(rules, 0, sizeof(*rules));
	rules->now_us = now_us;
	axis_movies_today(now_us, rules->today);
	rules->released_after_days = 90;
	rules->give_up_after_s = 6 * 3600;
}

/* @date ("YYYY-MM-DD") plus @days, or %NULL when @date is not a date. */
static gchar *
date_plus(
	const gchar *date,
	gint         days
){
	guint year = 0;
	guint month = 0;
	guint day = 0;
	GDate when;
	gchar buffer[16];

	if (date == NULL || strlen(date) != 10 || sscanf(date, "%4u-%2u-%2u", &year, &month, &day) != 3 ||
	    !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
		return NULL;
	g_date_clear(&when, 1);
	g_date_set_dmy(&when, (GDateDay)day, (GDateMonth)month, (GDateYear)year);
	if (days > 0)
		g_date_add_days(&when, (guint)days);
	if (g_date_strftime(buffer, sizeof(buffer), "%Y-%m-%d", &when) == 0)
		return NULL;
	return g_strdup(buffer);
}

gboolean
axis_movie_available(
	const AxisMovieFacts *facts,
	const AxisMovieRules *rules,
	gchar               **from
){
	g_autofree gchar *date = NULL;

	if (from != NULL)
		*from = NULL;
	if (facts->availability == AXIS_MOVIE_ANNOUNCED)
		return TRUE;
	date = date_plus(facts->release_date, facts->availability == AXIS_MOVIE_RELEASED ?
		rules->released_after_days : 0);
	if (date == NULL)
		return facts->status == AXIS_META_STATUS_RELEASED;
	if (strcmp(date, rules->today) <= 0)
		return TRUE;
	if (from != NULL)
		*from = g_steal_pointer(&date);
	return FALSE;
}

gboolean
axis_movie_in_flight(const AxisMovieFacts *facts)
{
	if (facts->other_live > 0)
		return TRUE;
	if (facts->grab_id == 0 || facts->grab_completed_us > 0)
		return FALSE;
	/* Claimed and not yet handed over; or handed over and not complete,
	 * whatever its download says (a failed one is in flight until the
	 * follow-up has dealt with it). */
	return !(facts->download_known && facts->download_state == AXIS_DOWNLOAD_COMPLETED);
}

gboolean
axis_movie_outcome_needs_attention(const gchar *outcome)
{
	static const gchar *const attention[] = {
		"none_acceptable", "exhausted", "no_indexer", "indexer_error", "no_client", "client_error",
		"root_offline", "profile_unusable", "metadata_missing", "failed", NULL
	};
	guint i;

	for (i = 0; outcome != NULL && attention[i] != NULL; i++) {
		if (strcmp(attention[i], outcome) == 0)
			return TRUE;
	}
	return FALSE;
}

/* ── The derivation ─────────────────────────────────────────────── */

static gchar *
when_text(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%Y-%m-%d %H:%M UTC") : g_strdup("an unknown time");
}

static const gchar *
client_of(const AxisMovieFacts *facts)
{
	return facts->client_name != NULL ? facts->client_name : "the download client";
}

static gchar *
downloading_reason(
	const AxisMovieFacts *facts,
	const AxisMovieRules *rules
){
	const gchar *client = client_of(facts);

	if (facts->download_id == 0 || !facts->download_known)
		return g_strdup("Being handed to a download client.");
	switch (facts->download_state) {
	case AXIS_DOWNLOAD_SENDING:
		return g_strdup_printf("Sent to %s; waiting for the client to confirm that it took it.", client);
	case AXIS_DOWNLOAD_QUEUED:
		return g_strdup_printf("Queued at %s.", client);
	case AXIS_DOWNLOAD_PAUSED:
		return g_strdup_printf("Paused at %s.", client);
	case AXIS_DOWNLOAD_FAILED:
		return g_strdup_printf("The download failed at %s%s%s; Axis puts it on the blocklist and looks for "
			"another.", client, facts->download_message != NULL ? ": " : "",
			facts->download_message != NULL ? facts->download_message : "");
	case AXIS_DOWNLOAD_GONE:
		return g_strdup_printf("%s no longer has the download; Axis puts it on the blocklist and looks for "
			"another.", client);
	case AXIS_DOWNLOAD_REMOVED:
		return g_strdup("The download was removed from its download client; Axis is closing the grab.");
	default:
		break;
	}
	if (facts->download_stalled) {
		g_autofree gchar *since = when_text(facts->download_progress_us);
		g_autofree gchar *until = when_text(facts->download_progress_us + rules->give_up_after_s * G_USEC_PER_SEC);

		return g_strdup_printf("Stalled at %s: no progress since %s. Axis gives up on it at %s and tries the next "
			"release.", client, since, until);
	}
	if (facts->download_problem != NULL)
		return g_strdup(facts->download_problem);
	if (facts->download_size > 0 && facts->download_done >= 0) {
		g_autofree gchar *size = axis_quality_format_size(facts->download_size);
		gdouble fraction = (gdouble)facts->download_done / (gdouble)facts->download_size;

		return g_strdup_printf("%.0f%% of %s at %s.", CLAMP(fraction, 0.0, 1.0) * 100.0, size, client);
	}
	return g_strdup_printf("Downloading at %s.", client);
}

static gchar *
wanted_reason(
	const AxisMovieFacts *facts,
	const AxisMovieRules *rules
){
	g_autoptr(GString) text = g_string_new(NULL);

	if (!facts->monitored) {
		g_string_append(text, "Not monitored: Axis looks for it only when asked.");
		return g_string_free(g_steal_pointer(&text), FALSE);
	}
	if (facts->search_us == 0) {
		g_string_append(text, "Not searched for yet: the next automatic search takes it.");
	} else {
		g_autofree gchar *when = when_text(facts->search_us);

		if (g_strcmp0(facts->search_outcome, "nothing_found") == 0)
			g_string_append_printf(text, "The last search, %s, found no release.", when);
		else
			g_string_append_printf(text, "Last searched %s.", when);
		if (facts->next_search_us > rules->now_us) {
			g_autofree gchar *next = when_text(facts->next_search_us);

			g_string_append_printf(text, " The next automatic search is after %s.", next);
		} else {
			g_string_append(text, " The next automatic search takes it again.");
		}
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* A movie on disk that Axis does not manage: what is there, and its disk
 * when that is away. */
static gchar *
library_reason(const AxisMovieFacts *facts)
{
	GString *text = g_string_new("On disk");
	g_autofree gchar *away = axis_movie_library_away_note(facts);

	if (facts->file_quality != AXIS_QUALITY_UNKNOWN)
		g_string_append_printf(text, " as %s", axis_quality_label(facts->file_quality));
	else if (facts->file_resolution != AXIS_RELEASE_RESOLUTION_NONE)
		g_string_append_printf(text, " as %s", axis_quality_unknown_source_label(facts->file_resolution));
	g_string_append(text, ", not managed by Axis.");
	if (away != NULL)
		g_string_append_printf(text, " %s", away);
	return g_string_free(text, FALSE);
}

gchar *
axis_movie_library_away_note(const AxisMovieFacts *facts)
{
	if (facts->n_away > 0 && facts->n_away >= facts->n_files)
		return g_strdup_printf("Its disk, %s, is offline: it stays listed, since a disk that is away never "
			"means a movie is gone.", facts->away_volume != NULL ? facts->away_volume : "a disk");
	if (facts->n_away > 0)
		return g_strdup_printf("One of its disks, %s, is offline.", facts->away_volume != NULL ?
			facts->away_volume : "a disk");
	return NULL;
}

AxisMovieState
axis_movie_derive_attention(
	const AxisMovieFacts  *facts,
	const AxisMovieRules  *rules,
	gchar                **reason,
	AxisAcquireAttention  *kind_out
){
	g_autofree gchar *why = NULL;
	g_autofree gchar *from = NULL;
	AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;
	AxisMovieState state;

	if (kind_out != NULL)
		*kind_out = AXIS_ACQUIRE_ATTENTION_NONE;
	/* Not managed: it is what is on disk, and nothing else is asked of it. */
	if (facts->library_only) {
		if (reason != NULL)
			*reason = library_reason(facts);
		return AXIS_MOVIE_IN_LIBRARY;
	}
	if (facts->grab_id != 0 && facts->grab_completed_us == 0 &&
	    !(facts->download_known && facts->download_state == AXIS_DOWNLOAD_COMPLETED)) {
		state = AXIS_MOVIE_DOWNLOADING;
		why = downloading_reason(facts, rules);
	} else if (facts->grab_id != 0) {
		const gchar *where = facts->download_rel_path;
		g_autofree gchar *at = where != NULL && facts->download_volume != NULL ?
			g_strdup_printf("Downloaded to %s, at %s.", facts->download_volume, where) : g_strdup("Downloaded.");

		if (g_strcmp0(facts->import_state, "blocked") == 0) {
			state = AXIS_MOVIE_ATTENTION;
			kind = AXIS_ACQUIRE_ATTENTION_IMPORT;
			why = g_strdup_printf("%s Its import is blocked: %s", at, facts->import_text != NULL ? facts->import_text :
				"a person must decide.");
		} else if (g_strcmp0(facts->import_state, "waiting") == 0 && facts->import_text != NULL) {
			state = AXIS_MOVIE_TO_IMPORT;
			why = g_strdup_printf("%s It waits to be imported: %s Nothing has been moved yet.", at,
				facts->import_text);
		} else if (g_strcmp0(facts->import_state, "running") == 0) {
			state = AXIS_MOVIE_TO_IMPORT;
			why = g_strdup_printf("%s It is being imported.", at);
		} else {
			state = AXIS_MOVIE_TO_IMPORT;
			why = g_strdup_printf("%s It waits there for its import; nothing has been moved yet.", at);
		}
	} else if (facts->other_live > 0) {
		state = AXIS_MOVIE_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT;
		why = g_strdup("A download of it that Axis gave up on is still in its download client (it could not be removed "
			"yet; Axis tries again every minute). Nothing else is grabbed until it is gone.");
	} else if (g_strcmp0(facts->import_state, "blocked") == 0) {
		/* A manual import of it, blocked. */
		state = AXIS_MOVIE_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_IMPORT;
		if (facts->held_forced != NULL)
			why = g_strdup_printf("You grabbed %s by force, and its import could take no file of the download: %s Axis "
				"does not search for this movie while the download is held for you. Open its import to take a file "
				"by hand, or dismiss it and Axis searches again.", facts->held_forced, facts->import_text != NULL ?
				facts->import_text : "a person must decide.");
		else
			why = g_strdup_printf("An import of it is blocked: %s", facts->import_text != NULL ? facts->import_text :
				"a person must decide.");
	} else if (g_strcmp0(facts->import_state, "waiting") == 0 || g_strcmp0(facts->import_state, "running") == 0) {
		state = AXIS_MOVIE_TO_IMPORT;
		why = g_strdup_printf("An import of it %s%s%s", g_strcmp0(facts->import_state, "running") == 0 ?
			"is running." : "waits: ", g_strcmp0(facts->import_state, "running") == 0 ? "" :
			facts->import_text != NULL ? facts->import_text : "it is queued.", "");
	} else if (facts->search_running || facts->search_queued) {
		state = AXIS_MOVIE_SEARCHING;
		why = g_strdup(facts->search_running ? "Searching the indexers." : "A search is queued.");
	} else if (facts->n_files > 0) {
		gboolean unknown_source = facts->file_quality == AXIS_QUALITY_UNKNOWN &&
			facts->file_resolution != AXIS_RELEASE_RESOLUTION_NONE;

		if (facts->kept_by != NULL && facts->kept_brought) {
			/* Kept because it was brought over (import-from-arr): no person
			 * chose that, so none is named. */
			state = AXIS_MOVIE_IN_LIBRARY;
			why = g_strdup_printf("In the library%s%s. Migrated from Radarr; Axis does not replace it. Let Axis "
				"upgrade it again under Files, or replace it yourself from a manual search.",
				facts->file_quality != AXIS_QUALITY_UNKNOWN ? " as " : "",
				facts->file_quality != AXIS_QUALITY_UNKNOWN ? axis_quality_label(facts->file_quality) : "");
		} else if (facts->kept_by != NULL) {
			/* Kept by hand (a forced grab's file, or one a person keeps): no
			 * upgrade is wanted for it, whatever the cutoff says, until a
			 * person lets it go (add-force-grab-and-assign). */
			state = AXIS_MOVIE_IN_LIBRARY;
			why = g_strdup_printf("In the library%s%s, kept by hand by %s: it is not upgraded by itself. Let Axis "
				"upgrade it again under Files, or replace it yourself from a manual search.",
				facts->file_quality != AXIS_QUALITY_UNKNOWN ? " as " : "",
				facts->file_quality != AXIS_QUALITY_UNKNOWN ? axis_quality_label(facts->file_quality) : "",
				facts->kept_by);
		} else if (facts->monitored && facts->profile_upgrades && facts->file_quality != AXIS_QUALITY_UNKNOWN &&
		    facts->profile_cutoff != AXIS_QUALITY_UNKNOWN &&
		    axis_quality_compare(facts->file_quality, facts->profile_cutoff) < 0) {
			state = AXIS_MOVIE_UPGRADE_WANTED;
			why = g_strdup_printf("In the library as %s, below the cutoff of its quality profile (%s).",
				axis_quality_label(facts->file_quality), axis_quality_label(facts->profile_cutoff));
		} else if (unknown_source && facts->monitored && facts->profile_upgrades && facts->profile_upgrade_unknown &&
		           axis_quality_above_unknown(facts->profile_cutoff, facts->file_resolution)) {
			/* Only when the profile says a file of unknown source may go. */
			state = AXIS_MOVIE_UPGRADE_WANTED;
			why = g_strdup_printf("In the library as %s (its name states no source), below the cutoff of its quality "
				"profile (%s); the profile upgrades a file of unknown source.",
				axis_quality_unknown_source_label(facts->file_resolution), axis_quality_label(facts->profile_cutoff));
		} else if (unknown_source) {
			state = AXIS_MOVIE_IN_LIBRARY;
			why = g_strdup_printf("In the library as %s: its name states no source, and %s.",
				axis_quality_unknown_source_label(facts->file_resolution),
				!facts->profile_upgrades ? "the profile does not upgrade" :
				!facts->profile_upgrade_unknown ? "the profile does not upgrade a file of unknown source" :
				!facts->monitored ? "the movie is not monitored" : "it is above the cutoff of its quality profile");
		} else {
			state = AXIS_MOVIE_IN_LIBRARY;
			why = g_strdup_printf("In the library%s%s.", facts->file_quality != AXIS_QUALITY_UNKNOWN ? " as " : "",
				facts->file_quality != AXIS_QUALITY_UNKNOWN ? axis_quality_label(facts->file_quality) : "");
		}
	} else if (!axis_movie_available(facts, rules, &from)) {
		state = AXIS_MOVIE_NOT_AVAILABLE;
		if (from != NULL)
			why = g_strdup_printf("Not searched for before %s (search from: %s).", from,
				axis_movie_availability_label(facts->availability));
		else
			why = g_strdup_printf("No release date is known, and it is searched for from %s: it is not "
				"searched for until TMDb gives a release date.", axis_movie_availability_label(facts->availability));
	} else if (facts->root_id == 0) {
		state = AXIS_MOVIE_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_NO_ROOT;
		why = g_strdup("Its destination library folder was removed: choose another under Settings for this movie.");
	} else if (!axis_volume_state_is_usable(facts->volume_state)) {
		state = AXIS_MOVIE_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE;
		why = g_strdup_printf("Its destination, %s, is on %s, which is %s: nothing is searched for or sent until it "
			"is back.", facts->root_name != NULL ? facts->root_name : "its root",
			facts->volume_name != NULL ? facts->volume_name : "a disk",
			facts->volume_state == AXIS_VOLUME_MISMATCH ? "not the disk it should be" : "offline");
	} else if (!facts->profile_usable) {
		state = AXIS_MOVIE_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE;
		why = g_strdup_printf("Its quality profile, %s, cannot be used: its settings are not valid. Fix it under Quality profiles, or choose another under Settings for this movie.",
			facts->profile_name != NULL ? facts->profile_name : "");
	} else if (axis_movie_outcome_needs_attention(facts->search_outcome)) {
		state = AXIS_MOVIE_ATTENTION;
		kind = axis_acquire_attention_of_outcome(facts->search_outcome);
		why = g_strdup(facts->search_text != NULL ? facts->search_text : "The last search failed: see Releases found by the last search.");
	} else {
		state = AXIS_MOVIE_WANTED;
		why = wanted_reason(facts, rules);
	}
	/* Held from automatic searches (brought over, 0068_arr_brought.sql): its
	 * state is what it is, and the reason says that nothing is searched for.
	 * A movie that is only wanted says nothing else: what a wanted movie's
	 * reason says of the next search is not so for it.
	 * A movie whose file was brought over and is kept says nothing of the
	 * hold: its reason says already that the file was migrated and is not
	 * replaced, and no search waits for it, so "until you start searching"
	 * would promise something that starting does not do. */
	if (axis_movie_hold_stops(facts->search_hold) && state != AXIS_MOVIE_DOWNLOADING &&
	    state != AXIS_MOVIE_TO_IMPORT && state != AXIS_MOVIE_SEARCHING &&
	    !(state == AXIS_MOVIE_IN_LIBRARY && facts->n_files > 0 && facts->kept_by != NULL && facts->kept_brought &&
	      strcmp(facts->search_hold, AXIS_MOVIE_HOLD_BROUGHT) == 0)) {
		gchar *held = state == AXIS_MOVIE_WANTED || why == NULL ? g_strdup(axis_movie_hold_sentence(facts->search_hold)) :
			g_strdup_printf("%s %s", why, axis_movie_hold_sentence(facts->search_hold));

		g_free(why);
		why = held;
	}
	if (reason != NULL)
		*reason = g_steal_pointer(&why);
	if (kind_out != NULL)
		*kind_out = state == AXIS_MOVIE_ATTENTION ? kind : AXIS_ACQUIRE_ATTENTION_NONE;
	return state;
}

AxisMovieState
axis_movie_derive(
	const AxisMovieFacts *facts,
	const AxisMovieRules *rules,
	gchar               **reason
){
	return axis_movie_derive_attention(facts, rules, reason, NULL);
}

/* ── Blocklist keys ─────────────────────────────────────────────── */

gchar *
axis_movie_name_key(
	gint64       source_id,
	gint64       indexer_id,
	const gchar *indexer,
	const gchar *name
){
	return axis_acquire_name_key(source_id, indexer_id, indexer, name);
}
