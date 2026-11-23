/*
 * axis-tv-state.c - the TV module's pure parts: monitoring choices, the
 * states and their derivation, and what a release (or a file) holds
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "modules/tv/axis-tv-private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(axis-tv-error-quark, axis_tv_error)

/* ── Monitoring ─────────────────────────────────────────────────── */

static const struct
{
	const gchar *name;
	const gchar *label;
} monitors[] = {
	{ "all", "Every episode" },
	{ "future", "Future episodes only" },
	{ "latest", "The latest season" },
	{ "first", "The first season" },
	{ "none", "Nothing yet" },
	/* Copied from the app a series was brought over from; never a choice
	 * of a form (axis_tv_bring()). */
	{ "brought", "As in Sonarr" }
};

const gchar *
axis_tv_monitor_name(AxisTvMonitor monitor)
{
	return (guint)monitor < G_N_ELEMENTS(monitors) ? monitors[monitor].name : NULL;
}

const gchar *
axis_tv_monitor_label(AxisTvMonitor monitor)
{
	return (guint)monitor < G_N_ELEMENTS(monitors) ? monitors[monitor].label : "Unknown";
}

gboolean
axis_tv_monitor_from_name(
	const gchar   *name,
	AxisTvMonitor *monitor
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(monitors); i++) {
		if (strcmp(monitors[i].name, name) == 0) {
			*monitor = (AxisTvMonitor)i;
			return TRUE;
		}
	}
	return FALSE;
}

static const gchar *const scopes[] = { "episode", "season", "series" };

const gchar *
axis_tv_scope_name(AxisTvScope scope)
{
	return (guint)scope < G_N_ELEMENTS(scopes) ? scopes[scope] : NULL;
}

gboolean
axis_tv_scope_from_name(
	const gchar *name,
	AxisTvScope *scope
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(scopes); i++) {
		if (strcmp(scopes[i], name) == 0) {
			*scope = (AxisTvScope)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── States ─────────────────────────────────────────────────────── */

static const struct
{
	const gchar *name;
	const gchar *label;
	guint        severity;
} state_table[AXIS_TV_N_STATES] = {
	{ "not_aired", "Not aired", 1 },
	{ "wanted", "Wanted", 6 },
	{ "searching", "Searching", 4 },
	{ "downloading", "Downloading", 3 },
	{ "to_import", "To import", 5 },
	{ "in_library", "In library", 0 },
	{ "upgrade_wanted", "Upgrade wanted", 2 },
	{ "attention", "Needs attention", 7 },
	{ "unmonitored", "Not monitored", 0 }
};

const gchar *
axis_tv_state_name(AxisTvState state)
{
	return (guint)state < AXIS_TV_N_STATES ? state_table[state].name : "unknown";
}

const gchar *
axis_tv_state_label(AxisTvState state)
{
	return (guint)state < AXIS_TV_N_STATES ? state_table[state].label : "Unknown";
}

guint
axis_tv_state_severity(AxisTvState state)
{
	return (guint)state < AXIS_TV_N_STATES ? state_table[state].severity : 0;
}

gboolean
axis_tv_state_from_name(
	const gchar *name,
	AxisTvState *state
){
	guint i;

	for (i = 0; name != NULL && i < AXIS_TV_N_STATES; i++) {
		if (strcmp(state_table[i].name, name) == 0) {
			*state = (AxisTvState)i;
			return TRUE;
		}
	}
	return FALSE;
}

void
axis_tv_today(
	gint64 now_us,
	gchar  today[11]
){
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_utc(now_us / G_USEC_PER_SEC);
	g_autofree gchar *text = now != NULL ? g_date_time_format(now, "%Y-%m-%d") : g_strdup("1970-01-01");

	g_strlcpy(today, text, 11);
}

gint64
axis_tv_date_us(const gchar *date)
{
	gint year = 0;
	gint month = 0;
	gint day = 0;
	g_autoptr(GDateTime) when = NULL;

	if (date == NULL || strlen(date) != 10 || sscanf(date, "%4d-%2d-%2d", &year, &month, &day) != 3)
		return 0;
	when = g_date_time_new_utc(year, month, day, 0, 0, 0);
	return when != NULL ? g_date_time_to_unix(when) * G_USEC_PER_SEC : 0;
}

void
axis_tv_rules_init(
	AxisTvRules *rules,
	gint64       now_us
){
	memset(rules, 0, sizeof(*rules));
	rules->now_us = now_us;
	axis_tv_today(now_us, rules->today);
	rules->air_delay_s = 3600;
	rules->give_up_after_s = 6 * 3600;
}

void
axis_tv_series_free(AxisTvSeries *series)
{
	if (series == NULL)
		return;
	g_free(series->title);
	g_free(series->original_title);
	g_free(series->poster_ref);
	g_free(series->provider);
	g_free(series->tvdb_id);
	g_free(series->tmdb_id);
	g_free(series->imdb_id);
	g_free(series->profile_name);
	g_free(series->profile_slug);
	g_free(series->root_name);
	g_free(series->volume_name);
	g_free(series->added_by);
	g_free(series->episode_source);
	g_free(series->search_hold);
	g_free(series->brought_absolute);
	g_free(series->claims);
	g_free(series->record_id);
	g_free(series->synced_record_id);
	g_free(series);
}

const gchar *
axis_tv_source_label(const gchar *source)
{
	static const struct
	{
		const gchar *source;
		const gchar *label;
	} labels[] = {
		{ "tmdb", "TMDb" },
		{ "tvdb", "TheTVDB's aired order" },
		{ "tvdb-dvd", "TheTVDB's DVD order" },
		{ "tvdb-alternate", "TheTVDB's alternate order" },
		{ "tvdb-regional", "TheTVDB's regional order" },
		{ "tvdb-altdvd", "TheTVDB's alternate DVD order" },
		{ "tvdb-alttwo", "TheTVDB's second alternate order" }
	};
	guint i;

	for (i = 0; source != NULL && i < G_N_ELEMENTS(labels); i++) {
		if (strcmp(labels[i].source, source) == 0)
			return labels[i].label;
	}
	return source != NULL ? source : "TMDb";
}

gboolean
axis_tv_source_is_tvdb(const gchar *source)
{
	return source != NULL && (strcmp(source, "tvdb") == 0 || g_str_has_prefix(source, "tvdb-"));
}

const gchar *
axis_tv_source_list_name(const gchar *source)
{
	return axis_tv_source_is_tvdb(source) ? axis_tv_source_label(source) : "TMDb's episode list";
}

static const gchar *
scheme_label(const gchar *scheme)
{
	if (g_strcmp0(scheme, "tmdb") == 0)
		return "TMDb";
	if (g_strcmp0(scheme, "tvdb") == 0)
		return "TheTVDB";
	if (g_strcmp0(scheme, "imdb") == 0)
		return "IMDb";
	return scheme;
}

GPtrArray *
axis_tv_claim_texts(const AxisTvSeries *series)
{
	GPtrArray *texts = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) claims = NULL;
	guint i;

	if (series->claims == NULL || series->claims[0] == '\0')
		return texts;
	claims = g_strsplit(series->claims, "\x1e", 20);
	for (i = 0; claims[i] != NULL; i++) {
		g_auto(GStrv) part = g_strsplit(claims[i], "\x1f", 5);

		if (g_strv_length(part) != 5)
			continue;
		if (strcmp(part[3], "names") == 0)
			g_ptr_array_add(texts, g_strdup_printf("%s's record of this series names %s %s, which Axis holds as "
				"another series, %s. The two are kept apart: a provider's word about another's identifier never "
				"joins two records.", scheme_label(part[0]), scheme_label(part[1]), part[2], part[4]));
		else
			g_ptr_array_add(texts, g_strdup_printf("%s's record of another series, %s, names this series' %s "
				"identifier %s. The two are kept apart: a provider's word about another's identifier never joins "
				"two records.", scheme_label(part[0]), part[4], scheme_label(part[1]), part[2]));
	}
	return texts;
}

void
axis_tv_episode_free(AxisTvEpisode *episode)
{
	if (episode == NULL)
		return;
	g_free(episode->provider_id);
	g_free(episode->source);
	g_free(episode->title);
	g_free(episode->air_date);
	g_free(episode->grab_release);
	g_free(episode->grab_holds);
	g_free(episode->client_name);
	g_free(episode->import_state);
	g_free(episode->import_text);
	g_free(episode->worst_release);
	g_free(episode->file_path);
	g_free(episode->search_outcome);
	g_free(episode->search_text);
	g_free(episode->kept_by);
	g_free(episode->hand_how);
	g_free(episode->hand_files);
	g_free(episode->search_hold);
	g_free(episode);
}

void
axis_tv_attention_item_free(AxisTvAttentionItem *item)
{
	if (item == NULL)
		return;
	g_free(item->reason);
	g_free(item);
}

void
axis_tv_summary_free(AxisTvSummary *summary)
{
	if (summary == NULL)
		return;
	axis_tv_series_free(summary->series);
	g_free(summary->reason);
	g_free(summary->next_air_date);
	g_clear_pointer(&summary->attention, g_ptr_array_unref);
	g_free(summary);
}

gboolean
axis_tv_episode_aired(
	const AxisTvEpisode *episode,
	const AxisTvRules   *rules,
	gint64              *from
){
	gint64 start = axis_tv_date_us(episode->air_date);

	if (from != NULL)
		*from = start > 0 ? start + rules->air_delay_s * G_USEC_PER_SEC : 0;
	return start > 0 && start + rules->air_delay_s * G_USEC_PER_SEC <= rules->now_us;
}

gboolean
axis_tv_episode_upgradable(
	const AxisTvEpisode *episode,
	const AxisTvSeries  *series
){
	/* A file of unknown quality is not replaced by itself: nothing says a
	 * release would be better.  One of an unknown source whose resolution is
	 * known (a file found on disk named with no source) is, when the profile
	 * says an unknown source may be upgraded, as a film's is. */
	if (episode->n_files == 0 || !series->profile_usable || !series->profile_upgrades)
		return FALSE;
	/* A file a person keeps by hand (a forced grab's, an assigned one) is not
	 * upgraded by itself: the releases Axis would pick are the ones in doubt
	 * (add-force-grab-and-assign).  A person lets it go on the series' page. */
	if (episode->kept_by != NULL)
		return FALSE;
	if (episode->worst_quality == AXIS_QUALITY_UNKNOWN)
		return episode->worst_resolution != AXIS_RELEASE_RESOLUTION_NONE && !episode->worst_converted &&
			series->profile_upgrade_unknown &&
			axis_quality_compare_on_disk(AXIS_QUALITY_UNKNOWN, episode->worst_resolution, series->profile_cutoff,
				AXIS_RELEASE_RESOLUTION_NONE) < 0;
	return axis_quality_compare(episode->worst_quality, series->profile_cutoff) < 0;
}

gboolean
axis_tv_episode_in_flight(const AxisTvEpisode *episode)
{
	return episode->grab_id > 0 || episode->removal_pending;
}

gboolean
axis_tv_outcome_needs_attention(const gchar *outcome)
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

gchar *
axis_tv_episode_label(
	gint season,
	gint number
){
	return g_strdup_printf("S%02dE%02d", season, number);
}

gchar *
axis_tv_series_label(
	const gchar *title,
	gint         year
){
	g_autofree gchar *cut = axis_tv_text_cut(title, 500);

	if (year > 0)
		return g_strdup_printf("%s (%d)", cut != NULL ? cut : "Untitled", year);
	return g_strdup(cut != NULL ? cut : "Untitled");
}

static gchar *
time_text(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%Y-%m-%d %H:%M UTC") : g_strdup("an unknown time");
}

static const gchar *
quality_text(const AxisTvEpisode *e)
{
	if (e->file_quality != AXIS_QUALITY_UNKNOWN)
		return axis_quality_label(e->file_quality);
	return axis_quality_unknown_source_label(e->file_resolution);
}

AxisTvState
axis_tv_episode_derive_attention(
	const AxisTvEpisode   *e,
	const AxisTvSeries    *s,
	const AxisTvRules     *rules,
	gchar                **reason,
	AxisAcquireAttention  *kind_out
){
	g_autofree gchar *why = NULL;
	AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;
	AxisTvState state;
	gint64 from = 0;
	gboolean aired = axis_tv_episode_aired(e, rules, &from);

	if (e->gone && e->source != NULL && s->episode_source != NULL && strcmp(e->source, s->episode_source) != 0) {
		/* Kept by a change of numbering (add-metadata-tvdb). */
		state = AXIS_TV_UNMONITORED;
		why = g_strdup_printf("It is on an episode %s lacks: it is kept here, numbered as %s numbers it, with its "
			"files and history, and never searched for.", axis_tv_source_label(s->episode_source),
			axis_tv_source_label(e->source));
	} else if (e->gone) {
		state = AXIS_TV_UNMONITORED;
		why = g_strdup_printf("It is no longer in %s. It is kept here with its files and history.",
			axis_tv_source_list_name(s->episode_source));
	} else if (e->grab_id > 0 && !e->grab_completed) {
		state = AXIS_TV_DOWNLOADING;
		if (e->download_id == 0)
			why = g_strdup_printf("%s is being handed to a download client.", e->grab_release != NULL ?
				e->grab_release : "A release");
		else if (e->download_known && e->download_size > 0 && e->download_done >= 0)
			why = g_strdup_printf("%s, %d%% at %s.", e->grab_release != NULL ? e->grab_release : "Downloading",
				(gint)(e->download_done * 100 / MAX(e->download_size, 1)), e->client_name != NULL ? e->client_name :
				"its client");
		else
			why = g_strdup_printf("%s is at %s.", e->grab_release != NULL ? e->grab_release : "A release",
				e->client_name != NULL ? e->client_name : "a download client");
	} else if (e->grab_id > 0 && !(e->n_files > 0 && g_strcmp0(e->import_state, "partial") == 0)) {
		/* (Partly imported, an episode that has its file is in the library:
		 * the grab stays open for the others.) */
		if (e->n_files == 0 && (g_strcmp0(e->import_state, "blocked") == 0 ||
		    g_strcmp0(e->import_state, "partial") == 0)) {
			state = AXIS_TV_ATTENTION;
			kind = AXIS_ACQUIRE_ATTENTION_IMPORT;
			why = g_strdup_printf("Downloaded, and its import needs a person: %s", e->import_text != NULL ?
				e->import_text : "see the import.");
		} else {
			state = AXIS_TV_TO_IMPORT;
			why = g_strdup_printf("%s has arrived and waits for its import%s%s", e->grab_release != NULL ?
				e->grab_release : "Its download", e->import_text != NULL ? ": " : ".", e->import_text != NULL ?
				e->import_text : "");
		}
	} else if (e->removal_pending) {
		state = AXIS_TV_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT;
		why = g_strdup("A download of it that Axis gave up on is still at its download client: nothing else is grabbed for "
			"it until that is gone.");
	} else if ((e->search_queued || e->search_running) && e->monitored) {
		state = AXIS_TV_SEARCHING;
		why = g_strdup(e->search_running ? "A search that holds it is running." : "A search that holds it is "
			"queued.");
	} else if (e->n_files > 0) {
		if (e->monitored && axis_tv_episode_upgradable(e, s)) {
			state = AXIS_TV_UPGRADE_WANTED;
			why = g_strdup_printf("In the library as %s, below the cutoff of its quality profile (%s): a better release is "
				"sought.", quality_text(e), axis_quality_label(s->profile_cutoff));
		} else if (e->kept_by != NULL && e->kept_brought) {
			/* Kept because it was brought over (import-from-arr): no person
			 * chose that, so none is named. */
			state = AXIS_TV_IN_LIBRARY;
			why = g_strdup_printf("In the library as %s. Migrated from Sonarr; Axis does not replace it. Let Axis "
				"upgrade it again on its row.", quality_text(e));
		} else if (e->kept_by != NULL) {
			/* Kept by hand: said, with who and the way to let it go. */
			state = AXIS_TV_IN_LIBRARY;
			why = g_strdup_printf("In the library as %s. Kept by hand by %s: not upgraded by itself. Let Axis upgrade "
				"it again on its row.", quality_text(e), e->kept_by);
		} else {
			state = AXIS_TV_IN_LIBRARY;
			why = g_strdup_printf("In the library as %s.", quality_text(e));
		}
	} else if (!e->monitored) {
		state = AXIS_TV_UNMONITORED;
		why = g_strdup("Not monitored: it is not searched for.");
	} else if (!aired) {
		state = AXIS_TV_NOT_AIRED;
		if (e->air_date == NULL) {
			why = g_strdup("No air date is known for it. Axis searches for an episode by itself only once its air "
				"date has passed, so this one waits until the provider gives a date or an administrator searches for "
				"it by hand.");
		} else if (strcmp(e->air_date, rules->today) > 0) {
			why = g_strdup_printf("Airs on %s.", e->air_date);
		} else {
			g_autofree gchar *when = time_text(from);

			why = g_strdup_printf("Airs on %s: it is searched for from %s.", e->air_date, when);
		}
	} else if (s->root_id == 0) {
		state = AXIS_TV_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_NO_ROOT;
		why = g_strdup("The series' destination library folder was removed: nothing is searched for or "
			"sent. Choose another under Settings for this series.");
	} else if (!axis_volume_state_is_usable(s->volume_state)) {
		state = AXIS_TV_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE;
		why = g_strdup_printf("The series' destination, %s, is on %s, which is offline: nothing is searched for or "
			"sent until it is back.", s->root_name != NULL ? s->root_name : "its root",
			s->volume_name != NULL ? s->volume_name : "a disk");
	} else if (!s->profile_usable) {
		state = AXIS_TV_ATTENTION;
		kind = AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE;
		why = g_strdup_printf("The series' quality profile, %s, cannot be used: its settings are not valid. Fix it under Quality profiles, or choose another under Settings for this series.",
			s->profile_name != NULL ? s->profile_name : "");
	} else if (axis_tv_outcome_needs_attention(e->search_outcome)) {
		state = AXIS_TV_ATTENTION;
		kind = axis_acquire_attention_of_outcome(e->search_outcome);
		why = g_strdup(e->search_text != NULL ? e->search_text : "The last search failed: see Releases found by the last search.");
	} else if (e->last_search_us > 0 && e->next_search_us > rules->now_us) {
		g_autofree gchar *next = time_text(e->next_search_us);

		state = AXIS_TV_WANTED;
		why = g_strdup_printf("Aired on %s and not in the library.%s The next automatic search is after %s.",
			e->air_date, g_strcmp0(e->search_outcome, "nothing_found") == 0 ? " The last search found no release." :
			"", next);
	} else {
		state = AXIS_TV_WANTED;
		why = g_strdup_printf("Aired on %s and not in the library: %s.", e->air_date,
			e->last_search_us > 0 ? "the next automatic search after its wait takes it again" :
			"it is searched for by the next automatic search");
	}
	/* Brought over and held: the state is what it is, and the reason says
	 * that Axis does not search for it by itself yet (and never that a search
	 * will come).  Only where a search would otherwise come: an episode that
	 * is wanted, or to be upgraded. */
	if ((state == AXIS_TV_WANTED || state == AXIS_TV_UPGRADE_WANTED) && axis_tv_held(s, e)) {
		const gchar *held = axis_tv_hold_sentence(axis_tv_hold_stops(e->search_hold) ? e->search_hold :
			s->search_hold);
		gchar *said = state == AXIS_TV_WANTED ? g_strdup_printf("Aired on %s and not in the library. %s",
			e->air_date != NULL ? e->air_date : "a day not known", held) :
			g_strdup_printf("In the library as %s, below the cutoff of its quality profile (%s). %s", quality_text(e),
				axis_quality_label(s->profile_cutoff), held);

		g_free(why);
		why = said;
	}
	if (reason != NULL)
		*reason = g_steal_pointer(&why);
	if (kind_out != NULL)
		*kind_out = state == AXIS_TV_ATTENTION ? kind : AXIS_ACQUIRE_ATTENTION_NONE;
	return state;
}

AxisTvState
axis_tv_episode_derive(
	const AxisTvEpisode *e,
	const AxisTvSeries  *s,
	const AxisTvRules   *rules,
	gchar              **reason
){
	return axis_tv_episode_derive_attention(e, s, rules, reason, NULL);
}

/* ── What needs attention, grouped ──────────────────────────────── */

static void
group_free(AxisTvAttentionGroup *group)
{
	g_ptr_array_unref(group->items);
	g_free(group);
}

static gint
compare_groups(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvAttentionGroup *x = *(const AxisTvAttentionGroup *const *)a;
	const AxisTvAttentionGroup *y = *(const AxisTvAttentionGroup *const *)b;

	/* The most common first; of as many, the one met first. */
	if (x->items->len != y->items->len)
		return x->items->len > y->items->len ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

GPtrArray *
axis_tv_attention_groups(const GPtrArray *items)
{
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)group_free);
	guint i;
	guint k;

	for (i = 0; items != NULL && i < items->len; i++) {
		const AxisTvAttentionItem *item = g_ptr_array_index(items, i);
		AxisTvAttentionGroup *group = NULL;

		for (k = 0; k < groups->len && group == NULL; k++) {
			AxisTvAttentionGroup *g = g_ptr_array_index(groups, k);

			if (g->kind == item->kind && g_strcmp0(g->reason, item->reason) == 0)
				group = g;
		}
		if (group == NULL) {
			group = g_new0(AxisTvAttentionGroup, 1);
			group->kind = item->kind;
			group->reason = item->reason;
			group->order = groups->len;
			group->items = g_ptr_array_new();
			g_ptr_array_add(groups, group);
		}
		g_ptr_array_add(group->items, (gpointer)item);
	}
	g_ptr_array_sort(groups, compare_groups);
	return groups;
}

gchar *
axis_tv_attention_sentence(const GPtrArray *items)
{
	g_autoptr(GPtrArray) groups = NULL;
	const AxisTvAttentionGroup *top;

	if (items == NULL || items->len == 0)
		return NULL;
	groups = axis_tv_attention_groups(items);
	top = g_ptr_array_index(groups, 0);
	if (items->len == 1) {
		const AxisTvAttentionItem *item = g_ptr_array_index(items, 0);
		g_autofree gchar *label = axis_tv_episode_label(item->season, item->number);

		return g_strdup_printf("%s needs attention: %s", label, item->reason != NULL ? item->reason : "");
	}
	if (groups->len == 1)
		return g_strdup_printf("%u episodes need attention: %s", items->len, top->reason != NULL ? top->reason : "");
	return g_strdup_printf("%u episodes need attention. The most common reason, for %u of them: %s", items->len,
		top->items->len, top->reason != NULL ? top->reason : "");
}

/* ── What a release holds ───────────────────────────────────────── */

void
axis_tv_holds_clear(AxisTvHolds *holds)
{
	if (holds == NULL)
		return;
	g_free(holds->text);
	g_free(holds->label);
	g_clear_pointer(&holds->indices, g_array_unref);
	g_clear_pointer(&holds->found, g_array_unref);
	g_free(holds->found_label);
	g_free(holds->missing);
	g_free(holds->absolute_text);
	g_free(holds->person_why);
	memset(holds, 0, sizeof(*holds));
}

static gboolean
refuse(
	AxisTvHolds *holds,
	const gchar *code,
	gchar       *text
){
	holds->ok = FALSE;
	holds->code = code;
	g_free(holds->text);
	holds->text = text;
	g_array_set_size(holds->indices, 0);
	return FALSE;
}

gchar *
axis_tv_episode_label_of(
	gint        season,
	const gint *numbers,
	guint       n
){
	GString *label = g_string_new(NULL);
	gboolean run = n > 1;
	guint i;

	if (n == 0)
		return g_string_free(label, FALSE);
	g_string_append_printf(label, "S%02dE%02d", season, numbers[0]);
	for (i = 1; i < n; i++) {
		if (numbers[i] != numbers[i - 1] + 1)
			run = FALSE;
	}
	if (run) {
		g_string_append_printf(label, "-E%02d", numbers[n - 1]);
	} else {
		for (i = 1; i < n; i++)
			g_string_append_printf(label, "E%02d", numbers[i]);
	}
	return g_string_free(label, FALSE);
}

static gint
find_known(
	const AxisTvKnown *known,
	guint              n,
	gint               season,
	gint               number
){
	guint i;

	for (i = 0; i < n; i++) {
		if (known[i].season == season && known[i].number == number)
			return (gint)i;
	}
	return -1;
}

static void
whole_seasons(
	const AxisTvKnown *known,
	guint              n,
	gint               first,
	gint               last,
	AxisTvHolds       *holds
){
	gint season;
	guint i;

	for (season = first; season <= last; season++) {
		guint before = holds->indices->len;

		for (i = 0; i < n; i++) {
			if (known[i].season == season)
				g_array_append_val(holds->indices, i);
		}
		if (holds->indices->len == before) {
			refuse(holds, "no_such_season", g_strdup_printf("it is a pack of season %d, and the episode list has no "
				"such season", season));
			return;
		}
	}
	holds->ok = TRUE;
	holds->pack = TRUE;
	holds->season_first = first;
	holds->season_last = last;
	holds->label = first == last ? g_strdup_printf("S%02d", first) : g_strdup_printf("S%02d-S%02d", first, last);
	holds->text = first == last ? g_strdup_printf("it is the whole of season %d", first) :
		g_strdup_printf("it holds seasons %d to %d", first, last);
}

void
axis_tv_match_release(
	const AxisRelease  *parsed,
	const AxisTvKnown  *known,
	guint               n,
	AxisTvHolds        *holds
){
	AxisReleaseStates states;
	guint i;

	memset(holds, 0, sizeof(*holds));
	holds->indices = g_array_new(FALSE, FALSE, sizeof(guint));
	holds->season_first = -1;
	holds->season_last = -1;

	/* What the name says, by its own words (the reading identification's
	 * files share), then against the series' data. */
	axis_release_states(parsed, &states);
	switch (states.kind) {
	case AXIS_RELEASE_HOLDS_DATE: {
		/* A daily show: the episode aired that day, when exactly one did. */
		guint found = 0;
		guint at = 0;

		for (i = 0; i < n; i++) {
			if (g_strcmp0(known[i].air_date, states.date) == 0) {
				found++;
				at = i;
			}
		}
		if (found == 0) {
			refuse(holds, "no_such_date", g_strdup_printf("it is dated %s, and no episode of the series aired that "
				"day", states.date));
			return;
		}
		if (found > 1) {
			refuse(holds, "ambiguous_date", g_strdup_printf("%u episodes of the series aired on %s: it is not "
				"certainly one of them", found, states.date));
			return;
		}
		g_array_append_val(holds->indices, at);
		holds->ok = TRUE;
		holds->season_first = known[at].season;
		holds->season_last = known[at].season;
		holds->label = axis_tv_episode_label(known[at].season, known[at].number);
		holds->text = g_strdup_printf("it is dated %s: %s", states.date, holds->label);
		return;
	}
	case AXIS_RELEASE_HOLDS_COMPLETE: {
		gint first = G_MAXINT;
		gint last = -1;

		for (i = 0; i < n; i++) {
			if (known[i].season < 1)
				continue;
			first = MIN(first, known[i].season);
			last = MAX(last, known[i].season);
		}
		if (last < 0) {
			refuse(holds, "no_such_season", g_strdup("it is the complete series, and the episode list has no "
				"season"));
			return;
		}
		whole_seasons(known, n, first, last, holds);
		return;
	}
	case AXIS_RELEASE_HOLDS_SEASONS:
		whole_seasons(known, n, states.season_first, states.season_last, holds);
		return;
	case AXIS_RELEASE_HOLDS_NOTHING:
		if (g_strcmp0(states.code, "absolute_only") == 0)
			refuse(holds, "absolute_only", states.with_season ? g_strdup_printf("it states a season and an absolute "
				"episode number (%d), which is not matched", states.absolute) : g_strdup_printf("it states only an "
				"absolute episode number (%d), which is not matched to a season and an episode", states.absolute));
		else if (g_strcmp0(states.code, "cross_season") == 0)
			refuse(holds, "cross_season", g_strdup("it numbers episodes across seasons, which is not read"));
		else
			refuse(holds, "no_episode", g_strdup("it names no season or episode"));
		return;
	default:
		break;
	}
	{
		g_autoptr(GString) missing = g_string_new(NULL);
		g_autofree gint *numbers = g_new(gint, MAX(states.n_episodes, 1));
		g_autofree gint *have = g_new(gint, MAX(states.n_episodes, 1));
		guint n_have = 0;

		for (i = 0; i < states.n_episodes; i++) {
			gint at = find_known(known, n, states.season_first, states.episodes[i]);
			guint index;

			numbers[i] = states.episodes[i];
			if (at < 0) {
				g_autofree gchar *label = axis_tv_episode_label(states.season_first, states.episodes[i]);

				g_string_append_printf(missing, "%s%s", missing->len > 0 ? ", " : "", label);
				continue;
			}
			index = (guint)at;
			have[n_have++] = states.episodes[i];
			g_array_append_val(holds->indices, index);
		}
		if (missing->len > 0) {
			/* The numbering trap: release names follow another numbering than
			 * the series' data.  Never guessed; counted on the series' page.
			 * What it names that the data has is kept apart, for a person. */
			if (holds->indices->len > 0) {
				holds->found = g_array_copy(holds->indices);
				holds->found_label = axis_tv_episode_label_of(states.season_first, have, n_have);
				holds->missing = g_strdup(missing->str);
			}
			refuse(holds, "no_such_episode", g_strdup_printf("it is numbered %s, and there is no such episode in the "
				"episode list", missing->str));
			return;
		}
		holds->ok = TRUE;
		holds->season_first = states.season_first;
		holds->season_last = states.season_first;
		holds->label = axis_tv_episode_label_of(states.season_first, numbers, states.n_episodes);
		holds->text = g_strdup_printf("it holds %s", holds->label);
	}
}

/* ── A name numbered absolutely, for an anime series (slice A2) ──── */

const gchar *
axis_tv_absolute_state_words(AxisTvAbsoluteState state)
{
	switch (state) {
	case AXIS_TV_ABSOLUTE_USABLE:
		return "they can be used";
	case AXIS_TV_ABSOLUTE_NONE_GIVEN:
		return "TheTVDB gives none for it";
	case AXIS_TV_ABSOLUTE_NOT_USABLE:
		return "two episodes share a number, or the numbers are not in order";
	default:
		return "the series does not follow TheTVDB's aired order, which is where they come from";
	}
}

static void
holds_init(AxisTvHolds *holds)
{
	memset(holds, 0, sizeof(*holds));
	holds->indices = g_array_new(FALSE, FALSE, sizeof(guint));
	holds->season_first = -1;
	holds->season_last = -1;
}

static gboolean
name_separator(gchar c)
{
	return c == ' ' || c == '.' || c == '_' || c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}' ||
		(guchar)c < 0x20;
}

/* Whether @name names a special by a word ("OVA 2", "SP01") and does not
 * state season 0 outright ("S00E02", "0x02"), which is matched as for any
 * series. */
static gboolean
names_special(const gchar *name)
{
	gboolean word = FALSE;
	const gchar *p = name;

	while (*p != '\0') {
		const gchar *start;
		gsize length;

		while (*p != '\0' && name_separator(*p))
			p++;
		start = p;
		while (*p != '\0' && !name_separator(*p))
			p++;
		length = (gsize)(p - start);
		if (length == 0)
			continue;
		/* "S00E02", "S0", "0x02". */
		if ((start[0] == 's' || start[0] == 'S') && length >= 2 && start[1] == '0') {
			gsize k = 1;

			while (k < length && start[k] == '0')
				k++;
			if (k == length || start[k] == 'e' || start[k] == 'E')
				return FALSE;
		}
		if (start[0] == '0' && length >= 3) {
			gsize k = 0;

			while (k < length && start[k] == '0')
				k++;
			if (k < length && (start[k] == 'x' || start[k] == 'X'))
				return FALSE;
		}
		if (axis_release_special_word(start, length))
			word = TRUE;
	}
	return word;
}

/* Whether @name writes @value with a decimal part after a lone hyphen
 * ("- 12.5"): reading a name by itself takes the whole part as the number. */
static gboolean
name_has_half(
	const gchar *name,
	gint         value,
	gchar        half[4]
){
	const gchar *p;

	for (p = name; *p != '\0'; p++) {
		const gchar *q;
		const gchar *d;
		gint v = 0;
		guint digits = 0;

		if (*p != '-' || (p > name && p[-1] != ' ') || p[1] != ' ')
			continue;
		q = p + 1;
		while (*q == ' ')
			q++;
		for (d = q; g_ascii_isdigit(*d) && digits < 5; d++, digits++)
			v = v * 10 + (*d - '0');
		if (digits == 0 || digits > 4 || v != value || *d != '.' || !g_ascii_isdigit(d[1]))
			continue;
		if (g_ascii_isdigit(d[2]) ? g_ascii_isalnum(d[3]) : g_ascii_isalnum(d[2]))
			continue;
		half[0] = d[1];
		half[1] = g_ascii_isdigit(d[2]) ? d[2] : '\0';
		half[2] = '\0';
		return TRUE;
	}
	return FALSE;
}

/* The one live episode of seasons 1 and up whose absolute number is @value:
 * its index; -1 for none, -2 when two have it (the numbers are not usable,
 * whatever the caller was told). */
static gint
find_absolute(
	const AxisTvKnown *known,
	const gint        *absolute,
	guint              n,
	gint               value
){
	gint at = -1;
	guint i;

	for (i = 0; absolute != NULL && i < n; i++) {
		if (known[i].season < 1 || absolute[i] != value)
			continue;
		if (at >= 0)
			return -2;
		at = (gint)i;
	}
	return at;
}

typedef struct
{
	gint   season;
	gint   number;
	gint64 id;
	gint   absolute;
} Numbered;

static gint
compare_numbered(
	gconstpointer a,
	gconstpointer b
){
	const Numbered *x = a;
	const Numbered *y = b;

	if (x->season != y->season)
		return x->season < y->season ? -1 : 1;
	if (x->number != y->number)
		return x->number < y->number ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

/* Whether the absolute numbers of @known are each on one episode and rise
 * with season and episode: the rule of axis_tv_absolute_state(), checked
 * again on what the matcher was given, so that a caller that passes a stale
 * state still matches nothing through numbers that are out of order. */
static gboolean
known_in_order(
	const AxisTvKnown *known,
	const gint        *absolute,
	guint              n
){
	g_autofree Numbered *numbered = g_new(Numbered, MAX(n, 1));
	guint count = 0;
	guint i;

	for (i = 0; absolute != NULL && i < n; i++) {
		if (known[i].season < 1 || absolute[i] <= 0)
			continue;
		numbered[count].season = known[i].season;
		numbered[count].number = known[i].number;
		numbered[count].id = known[i].id;
		numbered[count].absolute = absolute[i];
		count++;
	}
	if (count > 1)
		qsort(numbered, count, sizeof(numbered[0]), compare_numbered);
	for (i = 1; i < count; i++) {
		if (numbered[i].absolute <= numbered[i - 1].absolute)
			return FALSE;
	}
	return TRUE;
}

/* The title keys of @names, each owned; a name with no key is left out. */
static GPtrArray *
name_keys(
	const gchar *const *names,
	guint               n
){
	GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; names != NULL && i < n; i++) {
		gchar *key = names[i] != NULL ? axis_release_title_key(names[i]) : NULL;

		if (key != NULL && key[0] != '\0')
			g_ptr_array_add(keys, key);
		else
			g_free(key);
	}
	return keys;
}

/*
 * Where @text goes on after a name: the byte at which a part of it that ends
 * at a word's end has one of @keys, with a letter or a digit still to come
 * ("Tenkuu Rangers 2nd Season" after "Tenkuu Rangers": 14).  0 when no part of
 * it is a name, and when the whole of it is.  Compared by title key at the end
 * of a word, as a title is matched: never a part of a word.
 */
static gsize
goes_on_after(
	const gchar     *text,
	const GPtrArray *keys
){
	const gchar *p;
	gboolean in_word = FALSE;

	if (text == NULL || keys->len == 0 || !g_utf8_validate(text, -1, NULL))
		return 0;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gboolean alnum = g_unichar_isalnum(g_utf8_get_char(p));

		if (in_word && !alnum) {
			g_autofree gchar *part = g_strndup(text, (gsize)(p - text));
			g_autofree gchar *key = axis_release_title_key(part);
			const gchar *q;
			gboolean more = FALSE;
			guint k;

			for (q = p; *q != '\0' && !more; q = g_utf8_next_char(q))
				more = g_unichar_isalnum(g_utf8_get_char(q));
			if (!more)
				return 0;
			for (k = 0; key != NULL && k < keys->len; k++) {
				if (strcmp(key, g_ptr_array_index(keys, k)) == 0)
					return (gsize)(p - text);
			}
		}
		in_word = alnum;
	}
	return 0;
}

/* More words than this in a title are not looked through for a name. */
#define NAME_WORDS_MAX 48

/*
 * Whether a run of whole words of @text, and not the whole of it, has one of
 * @keys: at the start ("Tenkuu Rangers Rising"), at the end ("Zoku Tenkuu
 * Rangers") or in the middle.  Compared by title key, as a title is matched:
 * never a part of a word.
 */
static gboolean
holds_a_name(
	const gchar     *text,
	const GPtrArray *keys
){
	gsize starts[NAME_WORDS_MAX];
	gsize ends[NAME_WORDS_MAX];
	guint n = 0;
	const gchar *p;
	gboolean in_word = FALSE;
	guint s;
	guint e;
	guint k;

	if (text == NULL || keys->len == 0 || !g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; ; p = g_utf8_next_char(p)) {
		gboolean alnum = *p != '\0' && g_unichar_isalnum(g_utf8_get_char(p));

		if (alnum && !in_word) {
			if (n == NAME_WORDS_MAX)
				return FALSE;
			starts[n] = (gsize)(p - text);
		} else if (!alnum && in_word) {
			ends[n++] = (gsize)(p - text);
		}
		in_word = alnum;
		if (*p == '\0')
			break;
	}
	for (s = 0; s < n; s++) {
		for (e = s; e < n; e++) {
			g_autofree gchar *part = NULL;
			g_autofree gchar *key = NULL;

			if (s == 0 && e == n - 1)
				continue;
			part = g_strndup(text + starts[s], ends[e] - starts[s]);
			key = axis_release_title_key(part);
			for (k = 0; key != NULL && key[0] != '\0' && k < keys->len; k++) {
				if (strcmp(key, g_ptr_array_index(keys, k)) == 0)
					return TRUE;
			}
		}
	}
	return FALSE;
}

/* A character of a name that is neither part of a word nor one that release
 * names put between words or leave out freely. */
static gboolean
is_mark(gunichar c)
{
	return !g_unichar_isalnum(c) && !g_unichar_isspace(c) && !g_unichar_ismark(c) && c != '.' && c != '_' &&
		c != '-' && c != ':' && c != ',';
}

/*
 * Whether @said is the series' title or original title as a release writes
 * it: the same title key, and no punctuation that the title does not have
 * ("Tenkuu Rangers!!" and "Tenkuu Rangers'" are other names than "Tenkuu
 * Rangers": a sequel is often told from its series by such a mark alone).  A
 * release leaves punctuation out freely, so less of it is the same title.
 */
static gboolean
own_title(
	const gchar           *said,
	const AxisTvAnimeRead *read
){
	g_autofree gchar *key = NULL;
	guint own;
	guint i;

	if (said == NULL || read->titles == NULL || read->n_titles == 0 || !g_utf8_validate(said, -1, NULL))
		return FALSE;
	key = axis_release_title_key(said);
	if (key == NULL || key[0] == '\0')
		return FALSE;
	own = MIN(MAX(read->n_own, 1), read->n_titles);
	for (i = 0; i < own; i++) {
		g_autofree gchar *mine = read->titles[i] != NULL ? axis_release_title_key(read->titles[i]) : NULL;
		const gchar *p;
		gboolean more = FALSE;

		if (mine == NULL || strcmp(mine, key) != 0 || !g_utf8_validate(read->titles[i], -1, NULL))
			continue;
		for (p = said; *p != '\0' && !more; p = g_utf8_next_char(p)) {
			gunichar c = g_utf8_get_char(p);
			const gchar *q;
			guint in_said = 0;
			guint in_title = 0;

			if (!is_mark(c))
				continue;
			for (q = said; *q != '\0'; q = g_utf8_next_char(q))
				in_said += g_utf8_get_char(q) == c ? 1 : 0;
			for (q = read->titles[i]; *q != '\0'; q = g_utf8_next_char(q))
				in_title += g_utf8_get_char(q) == c ? 1 : 0;
			more = in_said > in_title;
		}
		if (!more)
			return TRUE;
	}
	return FALSE;
}

/*
 * Whether the absolute number @value cannot be a part's own count: the series
 * has one season (specials apart), or @value is higher than the episode count
 * of every season after the first.  ("Rising Storm - 05" for a series of
 * three seasons of twelve can be the fifth of the second or third; "- 17"
 * cannot.)
 */
static gboolean
beyond_parts(
	const AxisTvKnown *known,
	guint              n,
	gint               value
){
	gint first = G_MAXINT;
	guint i;
	guint k;

	for (i = 0; i < n; i++) {
		if (known[i].season >= 1)
			first = MIN(first, known[i].season);
	}
	for (i = 0; i < n; i++) {
		gint count = 0;

		if (known[i].season <= first || known[i].season < 1)
			continue;
		for (k = 0; k < n; k++)
			count += known[k].season == known[i].season ? 1 : 0;
		if (value <= count)
			return FALSE;
	}
	return TRUE;
}

/* The year a name states in a bracket after @from ("- 05 (2011)"), which
 * reading a name by itself does not take for the name's year there; 0 for
 * none. */
static gint
year_after(
	const gchar *name,
	gsize        from
){
	const gchar *p;

	if (from > strlen(name))
		return 0;
	for (p = name + from; *p != '\0'; p++) {
		if ((*p == '(' || *p == '[') && g_ascii_isdigit(p[1]) && g_ascii_isdigit(p[2]) && g_ascii_isdigit(p[3]) &&
		    g_ascii_isdigit(p[4]) && (p[5] == ')' || p[5] == ']')) {
			gint year = (p[1] - '0') * 1000 + (p[2] - '0') * 100 + (p[3] - '0') * 10 + (p[4] - '0');

			if (year >= 1900 && year <= 2100)
				return year;
		}
	}
	return 0;
}

/*
 * Whether @said, the title an absolute number stands under, names a part of
 * the series and not the series: it is not the series' title or original
 * title, and it holds one of the series' names and more words, before it,
 * after it or both ("Tenkuu Rangers 2nd Season", "Tenkuu Rangers II", "Zoku
 * Tenkuu Rangers" for "Tenkuu Rangers").  The providers list a season's and a
 * sequel's name among a series' other names, and a number under such a name
 * counts from that part's first episode: it is not the series' absolute
 * number.  (A long name of the whole series that holds a short one of its
 * names is refused with them: nothing tells the two apart.)
 */
static gboolean
part_name(
	const gchar           *said,
	const AxisTvAnimeRead *read
){
	g_autoptr(GPtrArray) keys = NULL;
	g_autofree gchar *key = NULL;
	guint own;
	guint i;

	if (said == NULL || read->titles == NULL || read->n_titles == 0)
		return FALSE;
	key = axis_release_title_key(said);
	if (key == NULL || key[0] == '\0')
		return FALSE;
	own = MIN(MAX(read->n_own, 1), read->n_titles);
	for (i = 0; i < own; i++) {
		g_autofree gchar *mine = read->titles[i] != NULL ? axis_release_title_key(read->titles[i]) : NULL;

		if (mine != NULL && strcmp(mine, key) == 0)
			return FALSE;
	}
	keys = name_keys(read->titles, read->n_titles);
	return holds_a_name(said, keys);
}

/* Whether @rest, what another title's name goes on with after one of the
 * series' names, is the number @value and nothing more ("2", "II", "02",
 * "2nd Season", "Season 2", "Part 2"). */
static gboolean
rest_is_number(
	const gchar *rest,
	gint         value
){
	static const gchar *const before[] = { "season", "part", NULL };
	static const gchar *const behind[] = { "", "st", "nd", "rd", "th", "season", "stseason", "ndseason", "rdseason",
		"thseason", NULL };
	g_autofree gchar *key = axis_release_title_key(rest);
	const gchar *p = key;
	gchar *end = NULL;
	gint64 number;
	guint i;

	if (key == NULL)
		return FALSE;
	for (i = 0; before[i] != NULL; i++) {
		if (g_str_has_prefix(p, before[i])) {
			p += strlen(before[i]);
			break;
		}
	}
	if (!g_ascii_isdigit(*p))
		return FALSE;
	number = g_ascii_strtoll(p, &end, 10);
	if (number != value)
		return FALSE;
	for (i = 0; behind[i] != NULL; i++) {
		if (strcmp(end, behind[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

gchar *
axis_tv_anime_other_title(
	const AxisTvAnimeRead *read,
	const gchar *const    *others,
	guint                  n_others
){
	g_autoptr(GPtrArray) keys = NULL;
	AxisReleaseAfter mine;
	guint i;

	if (read == NULL || !read->anime || read->name == NULL || read->titles == NULL || read->n_titles == 0 ||
	    others == NULL)
		return NULL;
	axis_release_read_after(read->name, read->titles, read->n_titles, &mine);
	if (!mine.read)
		return NULL;
	keys = name_keys(read->titles, read->n_titles);
	for (i = 0; i < n_others; i++) {
		AxisReleaseAfter theirs;
		gsize rest;

		if (others[i] == NULL || others[i][0] == '\0')
			continue;
		/* The name begins with the other title's name as well: with the same
		 * name (nothing says whose number it is), or with a longer one that
		 * takes the number in ("Tenkuu Rangers 13"). */
		axis_release_read_after(read->name, &others[i], 1, &theirs);
		if (theirs.titled && theirs.title_end >= mine.title_end)
			return g_strdup_printf("another title that was looked up is named \"%s\", and the name begins with that "
				"too: nothing says which of them it is", others[i]);
		/* The other title is the series' name and this very number ("Tenkuu
		 * Rangers 2" for a name that says "Tenkuu Rangers 02"). */
		rest = goes_on_after(others[i], keys);
		if (rest > 0 && rest_is_number(others[i] + rest, mine.first))
			return g_strdup_printf("another title that was looked up is named \"%s\": the number %d may be part of "
				"that name and no episode", others[i], mine.first);
	}
	return NULL;
}

/* A number a name writes for the height of its picture. */
static gboolean
is_resolution(gint value)
{
	return value == 480 || value == 540 || value == 576 || value == 720 || value == 1080 || value == 2160;
}

/*
 * Whether @name states a season by itself and goes on with a bare number
 * ("S2 05", "Season 2 05", "2nd Season 05", "S2 [05]"): reading a name by
 * itself gives a whole season and drops a number.  *@said: the season the name
 * says; *@number: the number after it.  A year and a picture's height are not
 * such a number.
 */
static gboolean
season_then_number(
	const gchar *name,
	gint        *said,
	gint        *number
){
	const gchar *p = name;
	gint season = -1;	/* the season the word before stated */
	gint ordinal = -1;	/* "2nd": 2, when the word before was one */
	gboolean word_season = FALSE;

	while (*p != '\0') {
		const gchar *start;
		gsize length;
		gsize digits = 0;
		gint value = 0;
		gint was_season = season;
		gint was_ordinal = ordinal;
		gboolean was_word = word_season;

		while (*p != '\0' && name_separator(*p))
			p++;
		start = p;
		while (*p != '\0' && !name_separator(*p))
			p++;
		length = (gsize)(p - start);
		if (length == 0)
			continue;
		/* "S2 #05". */
		if (was_season >= 0 && length > 1 && start[0] == '#') {
			start++;
			length--;
		}
		season = -1;
		ordinal = -1;
		word_season = FALSE;
		/* A lone hyphen between the two is the parser's own form ("S2 - 05"),
		 * and is read past here too. */
		if (was_season >= 0 && length == 1 && start[0] == '-') {
			season = was_season;
			continue;
		}
		while (digits < length && digits < 5 && g_ascii_isdigit(start[digits])) {
			value = value * 10 + (start[digits] - '0');
			digits++;
		}
		if (digits > 0 && digits <= 4 && (digits == length || (digits + 2 == length && (start[digits] == 'v' ||
		    start[digits] == 'V') && g_ascii_isdigit(start[digits + 1])))) {
			if (was_season >= 0 && !(digits == 4 && value >= 1900 && value <= 2100) && !is_resolution(value)) {
				const gchar *q = p;

				/* "S02 12 Episodes": how many the season has, not one. */
				while (*q != '\0' && name_separator(*q))
					q++;
				if (g_ascii_strncasecmp(q, "episodes", 8) == 0 || g_ascii_strncasecmp(q, "eps", 3) == 0)
					continue;
				*said = was_season;
				*number = value;
				return TRUE;
			}
			/* "Season 2": the season itself. */
			if (was_word && digits <= 2 && digits == length)
				season = value;
			continue;
		}
		/* "2nd", "3rd": a season when "Season" follows. */
		if (digits > 0 && digits <= 2 && digits + 2 == length && (g_ascii_strncasecmp(start + digits, "st", 2) == 0 ||
		    g_ascii_strncasecmp(start + digits, "nd", 2) == 0 || g_ascii_strncasecmp(start + digits, "rd", 2) == 0 ||
		    g_ascii_strncasecmp(start + digits, "th", 2) == 0)) {
			ordinal = value;
			continue;
		}
		if (length == 6 && g_ascii_strncasecmp(start, "season", 6) == 0) {
			if (was_ordinal >= 0)
				season = was_ordinal;
			else
				word_season = TRUE;
			continue;
		}
		/* "S2", "S02". */
		if ((start[0] == 's' || start[0] == 'S') && (length == 2 || length == 3) && g_ascii_isdigit(start[1]) &&
		    (length == 2 || g_ascii_isdigit(start[2])))
			season = (gint)g_ascii_strtoll(start + 1, NULL, 10);
	}
	return FALSE;
}

/* "2nd", "3rd", "second": the count of a season written before its word. */
static gboolean
word_is_ordinal(const gchar *word)
{
	static const gchar *const words[] = {
		"first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth", "final", NULL
	};
	gsize length = strlen(word);
	guint i;

	if (length >= 3 && length <= 4 && g_ascii_isdigit(word[0]) && (length == 3 || g_ascii_isdigit(word[1])) &&
	    (strcmp(word + length - 2, "st") == 0 || strcmp(word + length - 2, "nd") == 0 ||
	     strcmp(word + length - 2, "rd") == 0 || strcmp(word + length - 2, "th") == 0))
		return TRUE;
	for (i = 0; words[i] != NULL; i++) {
		if (strcmp(word, words[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* "2", "02", "1+2", "two", "ii": the count of a season or a part written
 * after its word. */
static gboolean
word_is_count(const gchar *word)
{
	static const gchar *const words[] = {
		"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "i", "ii", "iii", "iv", "v",
		"vi", NULL
	};
	guint i;

	if (g_ascii_isdigit(word[0]))
		return TRUE;
	for (i = 0; words[i] != NULL; i++) {
		if (strcmp(word, words[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Words of a name, after the series' name, looked through at most. */
#define DOUBT_WORDS_MAX 256

/*
 * What stands after the series' name in @name (from the byte @from on, in
 * brackets or bare) that says the absolute number read from it is not the
 * series' absolute number, or not one episode's, though reading the name by
 * itself and the reading after a title both pass it by (the review of slice
 * A3):
 *
 *  - a season or a part, however written: "[2nd Season]", "Second Season",
 *    "Season Two", "[Season 1+2]", "[S2]", "(Part 2)", "Cour 2".  A group's
 *    season or part is often not TheTVDB's, and the number may count from its
 *    first episode;
 *  - a word for something that is no episode: a soundtrack, a manga, a
 *    trailer, a sample, a recap;
 *  - for one number (@n_numbers 1, @first): a range that begins with it and
 *    that reading the name by itself cut short ("01-12v2", "E01-E12",
 *    "E01-12");
 *  - a number or a range with a comma and another number after it ("01-12,
 *    14").
 *
 * Returns: (nullable) (transfer full): why it is not read, as a clause; *@code
 * (static): the refusal's code.  %NULL when nothing of the kind stands there.
 */
static gchar *
doubt_after_name(
	const gchar  *name,
	gsize         from,
	gint          first,
	guint         n_numbers,
	const gchar **code
){
	static const gchar *const no_episode[] = {
		"ost", "soundtrack", "manga", "trailer", "sample", "recap", "chapter", "chapters", "novel", "artbook", NULL
	};
	g_autoptr(GPtrArray) words = g_ptr_array_new_with_free_func(g_free);
	const gchar *p;
	guint i;
	guint k;

	if (name == NULL || from > strlen(name))
		return NULL;
	for (p = name + from; *p != '\0' && words->len < DOUBT_WORDS_MAX; ) {
		const gchar *start;

		while (*p != '\0' && name_separator(*p))
			p++;
		start = p;
		while (*p != '\0' && !name_separator(*p))
			p++;
		if (p > start)
			g_ptr_array_add(words, g_ascii_strdown(start, (gssize)(p - start)));
	}
	for (i = 0; i < words->len; i++) {
		const gchar *word = g_ptr_array_index(words, i);
		const gchar *before = i > 0 ? g_ptr_array_index(words, i - 1) : NULL;
		const gchar *next = i + 1 < words->len ? g_ptr_array_index(words, i + 1) : NULL;
		gsize length = strlen(word);
		const gchar *digits = word;
		gchar *end = NULL;

		/* A season or a part. */
		if (((strcmp(word, "season") == 0 || strcmp(word, "seasons") == 0) &&
		     ((before != NULL && word_is_ordinal(before)) || (next != NULL && word_is_count(next)))) ||
		    ((strcmp(word, "part") == 0 || strcmp(word, "pt") == 0 || strcmp(word, "cour") == 0) && next != NULL &&
		     word_is_count(next)) ||
		    (word[0] == 's' && (length == 2 || length == 3) && g_ascii_isdigit(word[1]) &&
		     (length == 2 || g_ascii_isdigit(word[2])))) {
			g_autofree gchar *said = word[0] == 's' && g_ascii_isdigit(word[1]) ? g_strdup(word) :
				before != NULL && word_is_ordinal(before) && strncmp(word, "season", 6) == 0 ?
				g_strdup_printf("%s %s", before, word) : g_strdup_printf("%s %s", word, next != NULL ? next : "");

			*code = "season_and_absolute";
			return g_strdup_printf("it states a season and an absolute episode number (%d, and a season or a part of "
				"the series after it: \"%s\"), which is not matched: a group's season or part is often not TheTVDB's, "
				"and the number may count from its first episode", first, said);
		}
		/* Something that is no episode. */
		for (k = 0; no_episode[k] != NULL; k++) {
			if (strcmp(word, no_episode[k]) == 0) {
				*code = "absolute_not_read";
				return g_strdup_printf("a word after the number (\"%s\") says that it is not an episode of the series "
					"(a soundtrack, a book, a trailer, a sample or a recap): it is not read as an episode", word);
			}
		}
		/* "E01-E12", "01-12v2": a range reading the name by itself cut short. */
		if (digits[0] == 'e')
			digits += digits[1] == 'p' ? 2 : 1;
		if (n_numbers == 1 && g_ascii_isdigit(digits[0]) && g_ascii_strtoll(digits, &end, 10) == first && end != NULL &&
		    (*end == '-' || *end == '~')) {
			end++;
			if (*end == 'e')
				end += end[1] == 'p' ? 2 : 1;
			if (g_ascii_isdigit(*end)) {
				*code = "absolute_not_read";
				return g_strdup_printf("it states a range (\"%s\") that is not read whole: it is not read as the one "
					"episode %d", word, first);
			}
		}
		/* "01-12, 14", "05, 07". */
		if (length >= 2 && word[length - 1] == ',' && g_ascii_isdigit(word[length - 2]) && next != NULL &&
		    g_ascii_isdigit(next[0])) {
			*code = "absolute_not_read";
			return g_strdup_printf("more numbers follow the number after the series' name (\"%s %s\"): it is not read "
				"as an episode", word, next);
		}
	}
	return NULL;
}

gboolean
axis_tv_anime_claims(
	const AxisRelease      *parsed,
	const AxisTvAnimeRead  *read,
	gint                   *version,
	gchar                 **why_not
){
	AxisReleaseStates states;
	AxisReleaseAfter after;

	if (version != NULL)
		*version = 0;
	if (why_not != NULL)
		*why_not = NULL;
	if (read == NULL || !read->anime || read->state != AXIS_TV_ABSOLUTE_USABLE || read->name == NULL ||
	    read->titles == NULL || read->n_titles == 0)
		return FALSE;
	axis_release_states(parsed, &states);
	axis_release_read_after(read->name, read->titles, read->n_titles, &after);
	/* Claimed only when reading the name by itself states nothing at all: a
	 * season, an episode, a date or an absolute number it states is what the
	 * name says, under the title it states. */
	if (after.read && states.kind == AXIS_RELEASE_HOLDS_NOTHING && g_strcmp0(states.code, "no_episode") == 0) {
		/* A year in the name that is not the series' (one off is taken, as a
		 * title's year is): another title's release. */
		if (parsed != NULL && parsed->year > 0 && read->year > 0 && (parsed->year > read->year + 1 ||
		    parsed->year < read->year - 1)) {
			if (why_not != NULL)
				*why_not = g_strdup_printf("the year in its name is %d and the series' is %d: it is not read as an "
					"episode of this series", parsed->year, read->year);
			return FALSE;
		}
		if (version != NULL)
			*version = after.version;
		return TRUE;
	}
	/* It begins with the series' name and goes on with something doubtful
	 * ("Tenkuu Rangers 2 - 05": this series' fifth, or another title's?). */
	if (why_not != NULL && !after.read && after.titled && axis_release_after_words(after.code) != NULL &&
	    strcmp(after.code, "no_number") != 0)
		*why_not = g_strdup(axis_release_after_words(after.code));
	return FALSE;
}

void
axis_tv_match_release_anime(
	const AxisRelease     *parsed,
	const AxisTvKnown     *known,
	guint                  n,
	const AxisTvAnimeRead *read,
	AxisTvHolds           *holds
){
	AxisReleaseStates states;
	AxisReleaseAfter after;
	g_autofree gint *numbers = NULL;
	g_autofree guint *at = NULL;
	g_autofree gchar *person_why = NULL;
	guint n_numbers = 0;
	gboolean from_after = FALSE;
	gint highest = 0;
	guint i;

	/* Not marked as anime: exactly as any series, by the same code. */
	if (read == NULL || !read->anime) {
		axis_tv_match_release(parsed, known, n, holds);
		return;
	}
	memset(&after, 0, sizeof(after));
	axis_release_states(parsed, &states);
	switch (states.kind) {
	case AXIS_RELEASE_HOLDS_EPISODES:
		/* A group's count of specials is not TheTVDB's: "OVA 2" is not read
		 * as S00E02 for an anime series.  "S00E02" written outright is. */
		if (states.season_first == 0 && read->name != NULL && names_special(read->name)) {
			holds_init(holds);
			refuse(holds, "special", g_strdup("it names a special by a word (such as OVA), and a group's count of "
				"specials is not TheTVDB's: assign it by hand"));
			return;
		}
		/* A season and an episode: matched by them alone, as for any series. */
		axis_tv_match_release(parsed, known, n, holds);
		if (!holds->ok || parsed->absolute_count == 0 || read->state != AXIS_TV_ABSOLUTE_USABLE ||
		    !known_in_order(known, read->absolute, n))
			return;
		/* Both numberings are stated: they must name the same episodes. */
		for (i = 0; i < parsed->absolute_count && i < AXIS_RELEASE_MAX_NUMBERS; i++) {
			gint found = find_absolute(known, read->absolute, n, parsed->absolute[i]);
			gboolean among = FALSE;
			guint k;

			for (k = 0; found >= 0 && k < holds->indices->len; k++)
				among = among || g_array_index(holds->indices, guint, k) == (guint)found;
			if (!among || parsed->absolute_count != holds->indices->len) {
				g_autofree gchar *label = g_strdup(holds->label);
				g_autofree gchar *other = found >= 0 ? axis_tv_episode_label(known[found].season, known[found].number) :
					NULL;

				refuse(holds, "numberings_differ", other != NULL ?
					g_strdup_printf("it says %s and absolute %d, which is %s by TheTVDB: its two numberings name "
						"different episodes", label, parsed->absolute[i], other) :
					g_strdup_printf("it says %s and absolute %d, which no episode of the series has: its two "
						"numberings do not agree", label, parsed->absolute[i]));
				return;
			}
		}
		return;
	case AXIS_RELEASE_HOLDS_NOTHING:
		if (g_strcmp0(states.code, "absolute_only") == 0 && states.with_season) {
			holds_init(holds);
			refuse(holds, "season_and_absolute", g_strdup_printf("it states a season and an absolute episode number "
				"(season %d, %d), which is not matched: a group's season is often not TheTVDB's, so it is not read as "
				"S%02dE%02d", parsed->season_first, states.absolute, parsed->season_first, states.absolute));
			return;
		}
		if (g_strcmp0(states.code, "absolute_only") == 0) {
			/* "- 05 - 06", "- 05 [06]": reading a name by itself keeps the
			 * first number and drops the second.  Which of the two is the
			 * episode is not known, so neither is taken. */
			if (read->name != NULL && read->titles != NULL && read->n_titles > 0) {
				AxisReleaseAfter second;

				axis_release_read_after(read->name, read->titles, read->n_titles, &second);
				/* (A word after the number that says it is not one episode:
				 * "- 02 Movie", "- 02 BD", "- 05 of 12".  Other words after it
				 * are an episode's title, and are read past.) */
				if (second.titled && !second.read && (g_strcmp0(second.code, "two_numbers") == 0 ||
				    g_strcmp0(second.code, "not_episode") == 0)) {
					holds_init(holds);
					refuse(holds, "absolute_not_read", g_strdup(axis_release_after_words(second.code)));
					return;
				}
				if (second.titled && second.read) {
					/* The two readings must say the same number under the
					 * same name.  With "Tenkuu Rangers 3" among the series'
					 * names, "Tenkuu Rangers - 3 - 05" is absolute 3 by itself
					 * and the fifth of "Tenkuu Rangers 3" read after a title. */
					g_autofree gchar *span = g_strndup(read->name + second.title_at, second.title_end - second.title_at);
					g_autofree gchar *theirs = axis_release_title_key(span);
					g_autofree gchar *mine = parsed->title != NULL ? axis_release_title_key(parsed->title) : NULL;
					gint first = parsed->absolute_count > 0 ? parsed->absolute[0] : 0;
					gint year = year_after(read->name, second.number_end);

					if (second.first != first || g_strcmp0(theirs, mine) != 0) {
						holds_init(holds);
						refuse(holds, "absolute_not_read", g_strdup_printf("its name can be read two ways: as %d under "
							"\"%s\", or as %d under \"%s\". Nothing says which is meant, so it is not read as an episode",
							first, parsed->title != NULL ? parsed->title : "", second.first, span));
						return;
					}
					/* "- 05 (2011)": a year after the number is held to the
					 * rule for a name's year. */
					if (year > 0 && read->year > 0 && (year > read->year + 1 || year < read->year - 1)) {
						holds_init(holds);
						refuse(holds, "absolute_not_read", g_strdup_printf("the year in its name is %d and the series' "
							"is %d: it is not read as an episode of this series", year, read->year));
						return;
					}
				}
			}
			n_numbers = MIN(parsed->absolute_count, (guint)AXIS_RELEASE_MAX_NUMBERS);
			numbers = g_new(gint, MAX(n_numbers, 1));
			for (i = 0; i < n_numbers; i++)
				numbers[i] = parsed->absolute[i];
			break;
		}
		if (g_strcmp0(states.code, "no_episode") == 0 && read->name != NULL && read->titles != NULL &&
		    read->n_titles > 0) {
			/* Reading the name by itself found no episode: the reading after
			 * the series' own name. */
			axis_release_read_after(read->name, read->titles, read->n_titles, &after);
			if (after.read && after.last - after.first < AXIS_RELEASE_MAX_NUMBERS) {
				from_after = TRUE;
				n_numbers = (guint)(after.last - after.first + 1);
				numbers = g_new(gint, n_numbers);
				for (i = 0; i < n_numbers; i++)
					numbers[i] = after.first + (gint)i;
				break;
			}
			if (after.titled && axis_release_after_words(after.code) != NULL &&
			    g_strcmp0(after.code, "no_number") != 0) {
				holds_init(holds);
				refuse(holds, g_strcmp0(after.code, "special") == 0 ? "special" : "absolute_not_read",
					g_strdup(axis_release_after_words(after.code)));
				return;
			}
		}
		axis_tv_match_release(parsed, known, n, holds);
		return;
	default: {
		gint said = 0;
		gint bare = 0;

		/* "S2 05", "2nd Season 05": a season and a bare number, as "S2 - 05"
		 * is, which reading a name by itself gives as a whole season (the
		 * second, or the fifth). */
		if (states.kind == AXIS_RELEASE_HOLDS_SEASONS && states.season_first == states.season_last &&
		    read->name != NULL && season_then_number(read->name, &said, &bare)) {
			holds_init(holds);
			refuse(holds, "season_and_absolute", g_strdup_printf("it states a season and an absolute episode number "
				"(season %d, %d), which is not matched: a group's season is often not TheTVDB's, so it is not read as "
				"S%02dE%02d", said, bare, said, bare));
			return;
		}
		/* "- 05 Complete": reading a name by itself gives the complete
		 * series and drops the number. */
		if (states.kind == AXIS_RELEASE_HOLDS_COMPLETE && read->name != NULL && read->titles != NULL &&
		    read->n_titles > 0) {
			AxisReleaseAfter numbered;

			axis_release_read_after(read->name, read->titles, read->n_titles, &numbered);
			if (numbered.titled && !numbered.read && g_strcmp0(numbered.code, "not_episode") == 0) {
				holds_init(holds);
				refuse(holds, "absolute_not_read", g_strdup(axis_release_after_words(numbered.code)));
				return;
			}
		}
		/* A date, a whole season, "Complete": as for any series. */
		axis_tv_match_release(parsed, known, n, holds);
		return;
	}
	}

	/* Absolute numbers, and nothing else, are stated. */
	holds_init(holds);
	if (n_numbers == 0) {
		refuse(holds, "no_episode", g_strdup("it names no season or episode"));
		return;
	}
	holds->batch = n_numbers > 1;
	if (read->state != AXIS_TV_ABSOLUTE_USABLE || !known_in_order(known, read->absolute, n)) {
		refuse(holds, "absolute_unusable", g_strdup_printf("it states only an absolute episode number (%d), and "
			"TheTVDB's absolute numbers for this series cannot be used: %s", numbers[0],
			axis_tv_absolute_state_words(read->state == AXIS_TV_ABSOLUTE_USABLE ? AXIS_TV_ABSOLUTE_NOT_USABLE :
			read->state)));
		return;
	}
	/* What stands after the series' name can say that the number is not the
	 * series' absolute number or not one episode's, in ways both readings of
	 * the name pass by: a season or a part in words, something that is no
	 * episode, a range cut short.  For one number and for a range alike. */
	if (read->name != NULL) {
		g_autofree gchar *doubt = NULL;
		const gchar *doubt_code = NULL;
		gsize from = 0;

		if (from_after) {
			from = after.title_end;
		} else if (read->titles != NULL && read->n_titles > 0) {
			AxisReleaseAfter titled;

			axis_release_read_after(read->name, read->titles, read->n_titles, &titled);
			if (titled.titled)
				from = titled.title_end;
		}
		doubt = doubt_after_name(read->name, from, numbers[0], n_numbers, &doubt_code);
		if (doubt != NULL) {
			refuse(holds, doubt_code, g_steal_pointer(&doubt));
			return;
		}
	}
	/* The number counts the series' episodes only under the series' own name.
	 * Under a name that is the series' name with more after it (a season's, a
	 * sequel's, which the providers list among a series' other names) it
	 * counts that part's episodes: "Tenkuu Rangers 2nd Season - 05" is the
	 * fifth of the second season, never absolute 5. */
	{
		g_autofree gchar *said = from_after ? g_strndup(read->name + after.title_at, after.title_end - after.title_at) :
			g_strdup(parsed->title);

		if (part_name(said, read)) {
			refuse(holds, "absolute_part_name", g_strdup_printf("it is numbered %d under the name \"%s\", which is one "
				"of the series' names with more words: the name of a part of the series (a season or a sequel), "
				"whose numbers are not the series' absolute numbers. It is not read as an episode", numbers[0], said));
			return;
		}
		/* A year in the name that is not the series' (one off is taken, as a
		 * title's year is). */
		if (parsed->year > 0 && read->year > 0 && (parsed->year > read->year + 1 || parsed->year < read->year - 1)) {
			refuse(holds, "absolute_not_read", g_strdup_printf("the year in its name is %d and the series' is %d: it is "
				"not read as an episode of this series", parsed->year, read->year));
			return;
		}
		/* What a search does not take by itself (a person may): a number read
		 * only because it follows the series' name; and a number under a name
		 * that is not the series' title or original title, when a later
		 * season has that many episodes.  No rule of names tells the name of
		 * a part ("Rising Storm") from a name of the whole series, so the
		 * number has to.  (Written as a rule to loosen once searches ask for
		 * absolute numbers.  They do since slice A3, which left it as it is:
		 * asking by a name finds such a release, and does not say whose
		 * count its number is.  For a range, the number is its first.) */
		if (from_after)
			person_why = g_strdup("Its name was read as the series' name followed by an episode number, a form another "
				"title's name can have too: no search takes it.");
		else if (said != NULL && !own_title(said, read) && !beyond_parts(known, n, numbers[0]))
			person_why = g_strdup_printf("It is numbered %d under the name \"%s\", which is not the series' title, and "
				"a later season of the series has that many episodes: the number may count the episodes of a part of "
				"the series, not the whole. No search takes it.", numbers[0], said);
	}
	if (!from_after && n_numbers == 1 && read->name != NULL) {
		gchar half[4];

		if (name_has_half(read->name, numbers[0], half)) {
			refuse(holds, "absolute_decimal", g_strdup_printf("%d.%s is not an episode number", numbers[0], half));
			return;
		}
	}
	/* A bare number of three or four digits is the number its digits say, or
	 * a season and an episode written together ("101": S01E01).  When the
	 * series has that season and episode, nothing settles which is meant.  A
	 * hyphen before it, or an "E", does. */
	if (from_after && !after.range && !after.hyphen && after.digits >= 3) {
		gint as_season = numbers[0] / 100;
		gint as_number = numbers[0] % 100;

		if (find_known(known, n, as_season, as_number) >= 0) {
			g_autofree gchar *label = axis_tv_episode_label(as_season, as_number);

			refuse(holds, "absolute_ambiguous", g_strdup_printf("the number %d after the series' name can be the "
				"absolute number %d or %s written together, and the series has that episode: it is not read. A name "
				"that says \"- %d\" or %s is", numbers[0], numbers[0], label, numbers[0], label));
			return;
		}
	}
	for (i = 0; read->absolute != NULL && i < n; i++) {
		if (known[i].season >= 1)
			highest = MAX(highest, read->absolute[i]);
	}
	at = g_new(guint, n_numbers);
	for (i = 0; i < n_numbers; i++) {
		gint found;

		if (numbers[i] < 1 || numbers[i] > 9999) {
			refuse(holds, "absolute_zero", g_strdup_printf("%d is not an episode number", numbers[i]));
			return;
		}
		if (i > 0 && numbers[i] != numbers[i - 1] + 1) {
			refuse(holds, "absolute_not_read", g_strdup("the absolute numbers it states are not one number or one "
				"rising run"));
			return;
		}
		found = find_absolute(known, read->absolute, n, numbers[i]);
		if (found == -2) {
			refuse(holds, "absolute_unusable", g_strdup_printf("two episodes of the series have the absolute number "
				"%d: TheTVDB's absolute numbers for this series cannot be used", numbers[i]));
			return;
		}
		if (found < 0) {
			refuse(holds, "absolute_unknown", numbers[i] > highest ?
				g_strdup_printf("no episode of the series has the absolute number %d (the highest TheTVDB gives is %d)",
					numbers[i], highest) :
				g_strdup_printf("no episode of the series has the absolute number %d", numbers[i]));
			return;
		}
		if (known[found].air_date == NULL || read->today == NULL || strcmp(known[found].air_date, read->today) > 0) {
			g_autofree gchar *label = axis_tv_episode_label(known[found].season, known[found].number);

			refuse(holds, "absolute_unaired", g_strdup_printf("absolute %d is %s, which has not aired%s", numbers[i],
				label, known[found].air_date == NULL ? " (it has no air date)" : ""));
			return;
		}
		at[i] = (guint)found;
	}
	{
		g_autofree gchar *first = axis_tv_episode_label(known[at[0]].season, known[at[0]].number);
		g_autofree gchar *last = axis_tv_episode_label(known[at[n_numbers - 1]].season, known[at[n_numbers - 1]].number);

		if (n_numbers == 1) {
			g_array_append_val(holds->indices, at[0]);
			holds->ok = TRUE;
			holds->by_absolute = TRUE;
			holds->season_first = known[at[0]].season;
			holds->season_last = known[at[0]].season;
			holds->label = g_strdup(first);
			holds->absolute_text = g_strdup_printf("%d is %s by TheTVDB", numbers[0], first);
			holds->text = g_strdup_printf("it holds %s (absolute %s)", first, holds->absolute_text);
			holds->person_only = person_why != NULL;
			holds->person_why = g_steal_pointer(&person_why);
			return;
		}
		/* A range: exactly one or more whole seasons is their pack.  Any
		 * other range is a batch (below). */
		whole_seasons(known, n, known[at[0]].season, known[at[n_numbers - 1]].season, holds);
		if (holds->ok && holds->indices->len == n_numbers) {
			g_autofree gchar *said = g_steal_pointer(&holds->text);

			holds->by_absolute = TRUE;
			holds->batch = TRUE;
			holds->absolute_text = g_strdup_printf("%d to %d are %s to %s by TheTVDB", numbers[0],
				numbers[n_numbers - 1], first, last);
			holds->text = g_strdup_printf("%s (absolute %s)", said, holds->absolute_text);
			holds->person_only = person_why != NULL;
			holds->person_why = g_steal_pointer(&person_why);
			return;
		}
		g_clear_pointer(&holds->label, g_free);
		holds->pack = FALSE;
		holds->season_first = -1;
		holds->season_last = -1;
		holds->batch = TRUE;
		if (!read->batches) {
			refuse(holds, "absolute_batch", g_strdup_printf("it is a batch of part of a season (absolute %d to %d, which "
				"are %s to %s by TheTVDB): only a search takes a batch, as the episodes it holds", numbers[0],
				numbers[n_numbers - 1], first, last));
			return;
		}
		/* A batch (slice A3): exactly the episodes its numbers map to, each
		 * looked up above, of one season or of several.  Never a season: what
		 * it holds is these episodes and no other. */
		g_clear_pointer(&holds->text, g_free);
		g_array_set_size(holds->indices, 0);
		g_array_append_vals(holds->indices, at, n_numbers);
		holds->ok = TRUE;
		holds->code = NULL;
		holds->by_absolute = TRUE;
		holds->season_first = known[at[0]].season;
		holds->season_last = known[at[n_numbers - 1]].season;
		holds->label = g_strdup_printf("%s-%s", first, last);
		holds->absolute_text = g_strdup_printf("%d to %d are %s to %s by TheTVDB", numbers[0], numbers[n_numbers - 1],
			first, last);
		holds->text = g_strdup_printf("it holds %s to %s (absolute %s): a batch of %u episodes", first, last,
			holds->absolute_text, n_numbers);
		holds->person_only = person_why != NULL;
		holds->person_why = g_steal_pointer(&person_why);
	}
}
