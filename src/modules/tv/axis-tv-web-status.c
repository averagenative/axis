/*
 * axis-tv-web-status.c - what a series' page says is happening: the status
 * line of each episode, season and the series, the search buttons that say
 * their work is in hand, and the answer to a click the work in hand took
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every line and button is drawn from the series' work in hand and its
 * searches that ended a while ago (axis_tv_get_work(): two statements) and
 * the episodes' facts the page reads anyway, so the page, a reload and the
 * live fragment (GET /series/:id/activity) are the same code.
 */

#include "modules/tv/axis-tv-web.h"

#include <string.h>

#include "core/axis-size.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-manual.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Words ──────────────────────────────────────────────────────── */

static gchar *
scope_what(
	AxisTvScope  scope,
	gint64       item,
	const gchar *episode
){
	if (scope == AXIS_TV_SCOPE_EPISODE)
		return g_strdup(episode != NULL ? episode : "the episode");
	if (scope == AXIS_TV_SCOPE_SEASON)
		return item == 0 ? g_strdup("the specials") : g_strdup_printf("season %" G_GINT64_FORMAT, item);
	return g_strdup("the whole series");
}

/* What is in hand for @scope/@item, or what ended a while ago; %NULL for
 * nothing. */
static gchar *
target_words(
	gint64                series_id,
	AxisTvScope           scope,
	gint64                item,
	gint                  season,
	const gchar          *episode,
	const AxisTvWork     *work,
	AxisWebActivityTone  *tone
){
	g_autofree gchar *what = scope_what(scope, item, episode);
	const AxisTvEnded *ended;
	const AxisTvEnded *listed;
	g_autofree gchar *ago = NULL;
	g_autofree gchar *outcome = NULL;
	guint family;

	for (family = 0; family < 2; family++) {
		const AxisJobLive *job = axis_tv_work_in_hand(work, series_id, scope, item, family == 1);
		g_autofree gchar *state = NULL;

		if (job == NULL)
			continue;
		state = axis_web_activity_state(job, work->now_us);
		*tone = job->running ? AXIS_WEB_ACTIVITY_RUNNING : AXIS_WEB_ACTIVITY_WAITING;
		return g_strdup_printf("A %ssearch of %s is %s.", family == 1 ? "manual " : "", what, state);
	}
	ended = axis_tv_work_ended(work, series_id, scope, item, FALSE);
	listed = axis_tv_work_ended(work, series_id, scope, item, TRUE);
	if (listed != NULL && (ended == NULL || listed->finished_us > ended->finished_us))
		ended = listed;
	if (ended != NULL) {
		ago = axis_web_activity_ago(ended->finished_us, work->now_us);
		outcome = axis_web_activity_outcome(ended->outcome, ended->outcome_text, ended->n_found, tone);
		return g_strdup_printf("%s %s: it %s.", ended->list ? "A manual search ended" : "Searched", ago, outcome);
	}
	/* For what it is worth: a wider search in hand (it takes whole packs; it
	 * is no reason not to search for this one). */
	for (family = 0; scope != AXIS_TV_SCOPE_SERIES && family < 2; family++) {
		const AxisJobLive *job = NULL;
		g_autofree gchar *wider = NULL;
		g_autofree gchar *state = NULL;

		if (scope == AXIS_TV_SCOPE_EPISODE && season >= 0)
			job = axis_tv_work_in_hand(work, series_id, AXIS_TV_SCOPE_SEASON, season, family == 1);
		wider = job != NULL ? g_strdup_printf("%s as a whole", season == 0 ? "the specials" : "the season") :
			g_strdup("the whole series");
		if (job == NULL)
			job = axis_tv_work_in_hand(work, series_id, AXIS_TV_SCOPE_SERIES, 0, family == 1);
		if (job == NULL)
			continue;
		state = axis_web_activity_state(job, work->now_us);
		*tone = job->running ? AXIS_WEB_ACTIVITY_RUNNING : AXIS_WEB_ACTIVITY_WAITING;
		return g_strdup_printf("A %ssearch of %s is %s.", family == 1 ? "manual " : "", wider, state);
	}
	return NULL;
}

/* What a download of the episode is doing, or %NULL. */
static gchar *
download_words(const AxisTvEpisode *e)
{
	const gchar *client = e->client_name != NULL ? e->client_name : "its download client";

	if (!axis_tv_episode_in_flight(e) || !e->download_known)
		return NULL;
	switch (e->download_state) {
	case AXIS_DOWNLOAD_SENDING:
		return g_strdup_printf("Being sent to %s.", client);
	case AXIS_DOWNLOAD_QUEUED:
		return g_strdup_printf("Waiting for its turn at %s.", client);
	case AXIS_DOWNLOAD_PAUSED:
		return g_strdup_printf("Paused at %s.", client);
	case AXIS_DOWNLOAD_DOWNLOADING:
		if (e->download_size > 0 && e->download_done >= 0) {
			g_autofree gchar *done = axis_size_format((guint64)e->download_done);
			g_autofree gchar *size = axis_size_format((guint64)e->download_size);

			return g_strdup_printf("Downloading at %s: %d%%, %s of %s.", client,
				(gint)((gdouble)e->download_done * 100.0 / (gdouble)e->download_size), done, size);
		}
		return g_strdup_printf("Downloading at %s.", client);
	default:
		return NULL;
	}
}

/* ── Lines ──────────────────────────────────────────────────────── */

gchar *
axis_tv_web_episode_line_id(gint64 episode_id)
{
	return g_strdup_printf("activity-episode-%" G_GINT64_FORMAT, episode_id);
}

HtmxNode *
axis_tv_web_episode_line(
	const AxisTvSeries  *series,
	const AxisTvEpisode *e,
	const AxisTvWork    *work
){
	g_autofree gchar *id = axis_tv_web_episode_line_id(e->id);
	g_autofree gchar *label = axis_tv_episode_label(e->season, e->number);
	g_autofree gchar *text = download_words(e);
	AxisWebActivityTone tone = AXIS_WEB_ACTIVITY_RUNNING;

	if (text == NULL)
		text = target_words(series->id, AXIS_TV_SCOPE_EPISODE, e->id, e->season, label, work, &tone);
	return axis_web_activity_line(id, tone, text);
}

HtmxNode *
axis_tv_web_season_line(
	const AxisTvSeries *series,
	gint                season,
	const AxisTvWork   *work
){
	g_autofree gchar *id = g_strdup_printf("activity-season-%d", season);
	AxisWebActivityTone tone = AXIS_WEB_ACTIVITY_RUNNING;
	g_autofree gchar *text = target_words(series->id, AXIS_TV_SCOPE_SEASON, season, season, NULL, work, &tone);

	return axis_web_activity_line(id, tone, text);
}

HtmxNode *
axis_tv_web_series_line(
	const AxisTvSeries *series,
	const GPtrArray    *episodes,
	const AxisTvWork   *work
){
	g_autoptr(GString) text = g_string_new(NULL);
	AxisWebActivityTone tone = AXIS_WEB_ACTIVITY_RUNNING;
	guint running = 0;
	guint waiting = 0;
	guint downloading = 0;
	guint i;

	for (i = 0; work != NULL && work->live != NULL && i < work->live->len; i++) {
		const AxisJobLive *job = g_ptr_array_index(work->live, i);

		if (job->running)
			running++;
		else
			waiting++;
	}
	for (i = 0; episodes != NULL && i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		downloading += !e->gone && axis_tv_episode_in_flight(e) && e->download_known &&
			e->download_state == AXIS_DOWNLOAD_DOWNLOADING ? 1 : 0;
	}
	if (running + waiting > 0) {
		g_autofree gchar *count = axis_web_count(running + waiting, "search of this series is", "searches of this "
			"series are");

		g_string_append_printf(text, "%s in hand: %u running, %u waiting for %s turn.", count, running, waiting,
			running + waiting == 1 ? "its" : "their");
		tone = running > 0 ? AXIS_WEB_ACTIVITY_RUNNING : AXIS_WEB_ACTIVITY_WAITING;
	}
	if (downloading > 0) {
		g_autofree gchar *count = axis_web_count(downloading, "episode is", "episodes are");

		g_string_append_printf(text, "%s%s downloading.", text->len > 0 ? " " : "", count);
	}
	if (text->len == 0) {
		g_autofree gchar *words = target_words(series->id, AXIS_TV_SCOPE_SERIES, 0, -1, NULL, work, &tone);

		if (words != NULL)
			g_string_append(text, words);
	}
	return axis_web_activity_line(AXIS_TV_WEB_STATUS_SERIES, tone, text->len > 0 ? text->str : NULL);
}

/* ── Buttons ────────────────────────────────────────────────────── */

/* Whether the search of @scope/@item may not be asked now, by the server's
 * own rule (axis_tv_ask_search()): that very search in hand, or ended a
 * moment ago; @label and @title say so. */
static gboolean
busy(
	gint64             series_id,
	AxisTvScope        scope,
	gint64             item,
	gboolean           list,
	const AxisTvWork  *work,
	gchar            **label,
	gchar            **title
){
	const AxisJobLive *job = axis_tv_work_in_hand(work, series_id, scope, item, list);
	const AxisTvEnded *ended;
	AxisWebActivityTone tone;
	g_autofree gchar *what = scope_what(scope, item, NULL);

	if (job != NULL) {
		g_autofree gchar *state = axis_web_activity_state(job, work->now_us);

		*label = g_strdup(job->running ? "Searching" : "Waiting for its turn");
		*title = g_strdup_printf("A search of %s is %s.", what, state);
		return TRUE;
	}
	ended = axis_tv_work_ended(work, series_id, scope, item, list);
	if (ended != NULL && work->now_us - ended->finished_us < (gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC) {
		g_autofree gchar *outcome = axis_web_activity_outcome(ended->outcome, ended->outcome_text, ended->n_found,
			&tone);
		g_autofree gchar *again = axis_web_activity_clock(ended->finished_us +
			(gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC, work->now_us);

		*label = g_strdup("Searched a moment ago");
		*title = g_strdup_printf("A search of %s ended a moment ago and %s. It can be asked again from %s.", what,
			outcome, again);
		return TRUE;
	}
	return FALSE;
}

HtmxNode *
axis_tv_web_search_act(
	HtmxRequest       *request,
	gint64             series_id,
	AxisTvScope        scope,
	gint64             item,
	gint               season,
	gboolean           undated,
	const AxisTvWork  *work,
	const gchar       *label,
	const gchar       *css,
	const gchar       *back
){
	HtmxSpan *act = htmx_span_new();
	g_autofree gchar *id = NULL;
	g_autofree gchar *busy_label = NULL;
	g_autofree gchar *busy_title = NULL;

	if (undated)
		id = g_strdup_printf("act-undated-%" G_GINT64_FORMAT, item);
	else if (scope == AXIS_TV_SCOPE_EPISODE)
		id = g_strdup_printf("act-episode-%" G_GINT64_FORMAT, item);
	else if (scope == AXIS_TV_SCOPE_SEASON)
		id = g_strdup_printf("act-season-%" G_GINT64_FORMAT, item);
	else
		id = g_strdup("act-series");
	htmx_element_set_id(HTMX_ELEMENT(act), id);
	htmx_element_add_class(HTMX_ELEMENT(act), "activity-act");
	/* (Search all with no air date is a search of each such episode, decided
	 * episode by episode on the server: never drawn as busy.) */
	if (!undated && busy(series_id, scope, item, FALSE, work, &busy_label, &busy_title)) {
		ADD(act, axis_web_activity_busy_button(busy_label, busy_title));
	} else {
		g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/search", series_id);
		g_autofree gchar *item_text = g_strdup_printf("%" G_GINT64_FORMAT, item);
		g_autofree gchar *season_text = g_strdup_printf("%d", season);
		HtmxNode *form = axis_web_auth_post_form(request, action);

		htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
		ADD(form, axis_web_auth_hidden("scope", undated ? "undated" : axis_tv_scope_name(scope)));
		if (undated || scope != AXIS_TV_SCOPE_SERIES)
			ADD(form, axis_web_auth_hidden("item", item_text));
		if (!undated && scope == AXIS_TV_SCOPE_EPISODE && season >= 0)
			ADD(form, axis_web_auth_hidden("season", season_text));
		if (back != NULL)
			ADD(form, axis_web_auth_hidden("back", back));
		ADD(form, axis_web_auth_button(label, css));
		ADD(act, form);
	}
	return HTMX_NODE(act);
}

HtmxNode *
axis_tv_web_manual_act(
	HtmxRequest       *request,
	gint64             series_id,
	AxisTvScope        scope,
	gint64             item,
	gint               season,
	const AxisTvWork  *work,
	const gchar       *label
){
	HtmxSpan *act = htmx_span_new();
	const gchar *level = axis_tv_scope_name(scope);
	g_autofree gchar *id = scope == AXIS_TV_SCOPE_SERIES ? g_strdup("act-manual-series") :
		g_strdup_printf("act-manual-%s-%" G_GINT64_FORMAT, level, item);
	g_autofree gchar *busy_label = NULL;
	g_autofree gchar *busy_title = NULL;

	htmx_element_set_id(HTMX_ELEMENT(act), id);
	htmx_element_add_class(HTMX_ELEMENT(act), "activity-act");
	/* In hand or just done: its page, which follows it, rather than another. */
	if (busy(series_id, scope, item, TRUE, work, &busy_label, &busy_title)) {
		g_autofree gchar *href = axis_web_manual_path("/series", series_id, level, scope == AXIS_TV_SCOPE_SERIES ?
			-1 : item);
		g_autofree gchar *words = g_strdup_printf("%s: see the manual search", g_strcmp0(busy_label, "Searched a "
			"moment ago") == 0 ? "Listed a moment ago" : "Manual search in hand");
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(link), "button");
		htmx_element_add_class(HTMX_ELEMENT(link), "button-small");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "title", busy_title);
		htmx_node_set_text_content(HTMX_NODE(link), words);
		ADD(act, link);
	} else {
		ADD(act, axis_web_manual_button(request, "/series", series_id, level, scope == AXIS_TV_SCOPE_SERIES ? -1 :
			item, label));
	}
	return HTMX_NODE(act);
}

/* ── When the page asks again ───────────────────────────────────── */

gchar *
axis_tv_web_trigger(
	const AxisTvWork *work,
	const GPtrArray  *episodes
){
	gboolean in_hand = work != NULL && work->live != NULL && work->live->len > 0;
	gint64 wake = 0;
	guint i;

	for (i = 0; !in_hand && episodes != NULL && i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		in_hand = !e->gone && axis_tv_episode_in_flight(e);
	}
	/* A button that says "a moment ago" comes back when the minute is over. */
	for (i = 0; work != NULL && work->ended != NULL && i < work->ended->len; i++) {
		const AxisTvEnded *ended = g_ptr_array_index(work->ended, i);

		wake = axis_web_activity_sooner(wake, axis_web_activity_wake(ended->finished_us, work->now_us));
	}
	return axis_web_activity_trigger(in_hand, wake);
}

/* ── The answer to a click ──────────────────────────────────────── */

gchar *
axis_tv_web_answer(
	const gchar         *done,
	gint64               series_id,
	AxisTvScope          scope,
	gint64               item,
	gint                 season,
	const gchar         *episode,
	const GPtrArray     *episodes,
	const AxisTvWork    *work
){
	g_autofree gchar *what = scope_what(scope, item, episode);
	AxisWebActivityTone tone;

	if (g_strcmp0(done, "already_searching") == 0) {
		g_autofree gchar *words = NULL;

		if (scope == AXIS_TV_SCOPE_SERIES && axis_tv_work_in_hand(work, series_id, scope, 0, FALSE) == NULL) {
			g_autofree gchar *count = axis_web_count(work != NULL && work->live != NULL ? work->live->len : 0,
				"search of it is", "searches of it are");

			return g_strdup_printf("Every wanted episode is in a search that is queued or running already, so "
				"nothing new was queued: %s in hand now. Each episode's line says where its search stands.", count);
		}
		words = target_words(series_id, scope, item, season, episode, work, &tone);
		if (words != NULL && (tone == AXIS_WEB_ACTIVITY_RUNNING || tone == AXIS_WEB_ACTIVITY_WAITING)) {
			words[0] = g_ascii_tolower(words[0]);
			return g_strdup_printf("Nothing new was queued: %s", words);
		}
		if (words != NULL)
			return g_strdup_printf("Nothing new was queued: a search of %s was in hand already. It has ended "
				"since: %s", what, words);
		return g_strdup_printf("Nothing new was queued: a search of %s was in hand already.", what);
	}
	if (g_strcmp0(done, "searched_recently") == 0) {
		const AxisTvEnded *ended = axis_tv_work_ended(work, series_id, scope, item, FALSE);

		if (scope == AXIS_TV_SCOPE_SERIES && ended == NULL)
			return g_strdup("Nothing new was queued: every wanted episode was searched for a moment ago. Each "
				"episode's line says what its search found; you can ask again in a minute.");
		if (ended != NULL) {
			g_autofree gchar *ago = axis_web_activity_ago(ended->finished_us, work->now_us);
			g_autofree gchar *outcome = axis_web_activity_outcome(ended->outcome, ended->outcome_text,
				ended->n_found, &tone);
			g_autofree gchar *again = axis_web_activity_clock(ended->finished_us +
				(gint64)AXIS_ACQUIRE_SEARCH_AGAIN_S * G_USEC_PER_SEC, work->now_us);

			return g_strdup_printf("Nothing new was queued: a search of %s ended %s and %s. You can ask again from "
				"%s.", what, ago, outcome, again);
		}
		return g_strdup_printf("Nothing new was queued: %s was searched for a moment ago.", what);
	}
	(void)episodes;
	return NULL;
}
