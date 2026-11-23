/*
 * axis-tv-web.h - shared by the files of the TV module's pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-tv-web.c       routes, the work off the main loop, the title
 *                       actions, the dashboard, the actions (POST)
 *   axis-tv-web-view.c  the pages: the Series grid, a series' page with its
 *                       seasons and episodes, an episode's or a season's
 *                       releases, the Calendar, Wanted and History
 *
 * Any logged-in user sees every page; every action is an administrator's
 * POST (AXIS_WEB_ROUTE_ADMIN, with the CSRF token), recorded in the series'
 * history with the user, and the pages offer no form to anyone else.
 */

#ifndef AXIS_TV_WEB_H
#define AXIS_TV_WEB_H

#include <htmx-glib.h>

#include "modules/tv/axis-tv-private.h"
#include "web/axis-web.h"
#include "web/axis-web-activity.h"

G_BEGIN_DECLS

#define AXIS_TV_WEB_RELEASES_SHOWN  150
#define AXIS_TV_WEB_HISTORY_PAGE    50
#define AXIS_TV_WEB_EVENTS_SHOWN    20

typedef void (*AxisTvWorkFunc)(AxisTv *self, gpointer data);

typedef HtmxResponse *(*AxisTvDoneFunc)(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
);

/* Runs @work on a worker thread, then answers @request with @done on the
 * main loop (a 503 at once when too much waits or the pages stop). */
HtmxResponse *
axis_tv_web_dispatch(
	AxisTv          *self,
	HtmxRequest     *request,
	AxisTvWorkFunc   work,
	AxisTvDoneFunc   done,
	gpointer         data,
	GDestroyNotify   free_data
);

/* A choice for the add and options forms: a profile, a root. */
typedef struct
{
	gint64   id;
	gchar   *label;
	gchar   *slug;
	gboolean usable;
	guint64  free_bytes;
	const gchar *why_not;	/* roots: why a new series cannot go there now
				 * (axis_volume_state_unwritable_why()), or NULL */
} AxisTvChoice;

typedef struct
{
	GPtrArray     *profiles;	/* AxisTvChoice */
	GPtrArray     *roots;		/* AxisTvChoice */
	gchar         *default_slug;
	AxisTvMonitor  monitor;
} AxisTvChoices;

void
axis_tv_choices_clear(AxisTvChoices *choices);

gboolean
axis_tv_web_read_choices(
	AxisTv         *self,
	AxisTvChoices  *choices,
	GError        **error
);

/* The small mark of an episode's (or a series' worst) state. */
HtmxNode *
axis_tv_web_mark(AxisTvState state);

/* How complete a series is: "12 of 20" with a bar. */
HtmxNode *
axis_tv_web_completeness(const AxisTvSummary *summary);

/* The label of the "Anime" box, on the Add and Manage forms and on a series'
 * settings card; and what every page that offers or shows the kind says of
 * what marking a series does (add-anime-support: slices A2 and A3). */
#define AXIS_TV_WEB_ANIME_LABEL "Anime: its releases are often numbered 1, 2, 3 and on across seasons"
#define AXIS_TV_WEB_ANIME_SAYS "Axis recognises a release or a file named by an absolute episode number for a series " \
	"marked as anime, when TheTVDB's absolute numbers for it can be used. It then also searches for each episode by " \
	"that number, and searches in the anime category."

/* The add form of a series not managed yet (administrators).  @anime_hint:
 * the title's stored details say animation from Japan, said beside the
 * "Anime" box (which is never ticked by Axis); @tvdb_key: Axis has a TheTVDB
 * key, so a series ticked as anime can follow TheTVDB's aired order. */
HtmxNode *
axis_tv_web_add_form(
	HtmxRequest          *request,
	gint64                title_id,
	const AxisTvChoices  *choices,
	gboolean              anime_hint,
	gboolean              tvdb_key
);

/* A Discover card's one-step add (administrators): a button that adds the
 * series with what the add form selects at first (the default profile, the
 * first usable root, the default monitoring, no specials, search now) and
 * comes back to the page, and a link to its title page for the choices.
 * Without a usable profile or root, only the link. */
HtmxNode *
axis_tv_web_quick_add(
	HtmxRequest          *request,
	gint64                title_id,
	const AxisTvChoices  *choices
);

/* The person's kind in their add defaults (axis_web_request_add_defaults()). */
#define AXIS_TV_WEB_DEFAULTS_KIND "series"

/* The id of the bar above a list (also its place for the notice). */
#define AXIS_TV_WEB_DEFAULTS_BAR "add-defaults-series"

/* The profile, root and monitoring the person's Adds from a list use: their
 * own choice where it can still be used, else the installation's. */
void
axis_tv_web_defaults(
	HtmxRequest          *request,
	const AxisTvChoices  *choices,
	const AxisTvChoice  **profile,
	const AxisTvChoice  **root,
	AxisTvMonitor        *monitor
);

/* The bar above a list of titles (administrators): "New series go to
 * [folder] with [profile] monitoring [which]", a form that stores the
 * choice for this person (POST /series/defaults); with htmx a change posts
 * by itself and the answer (axis_tv_web_defaults_answer()) swaps the bar and
 * the cards' Adds.  @page: (nullable): the list's address it comes back to;
 * %NULL for the request's own. */
HtmxNode *
axis_tv_web_defaults_bar(
	HtmxRequest         *request,
	const AxisTvChoices *choices,
	const gchar         *page
);

/* The answer to an htmx post of the bar: the bar again, every card's
 * inputs and the words of its Options, out of band. */
HtmxResponse *
axis_tv_web_defaults_answer(
	HtmxRequest         *request,
	const AxisTvChoices *choices
);

/* How much of a series on disk is there: "47 of 62 episodes" with a bar. */
HtmxNode *
axis_tv_web_disk_completeness(const AxisTvOnDisk *disk);

/* What a series on disk is, a sentence: on disk and not managed, and a disk
 * of it that is away. */
gchar *
axis_tv_web_disk_reason(const AxisTvOnDisk *disk);

/* "Manage this series" for a series on disk (administrators): a profile, a
 * destination (its first folder's root first), what to monitor (future
 * episodes by default); nothing is searched for. */
HtmxNode *
axis_tv_web_manage_form(
	HtmxRequest          *request,
	const AxisTvOnDisk   *disk,
	const AxisTvChoices  *choices,
	gboolean              anime_hint
);

/* The notice for ?done=<key>, or %NULL. */
const gchar *
axis_tv_web_done_message(const gchar *key);

/* The same with what only the series' page knows: a new quality profile's
 * name ("profile", "profile_none"), and with @episodes (nullable,
 * AxisTvEpisode) whether nothing was searched for because episodes are held
 * apart; %NULL for a key that is none (transfer full). */
gchar *
axis_tv_web_done_text(
	const gchar        *key,
	const AxisTvSeries *series,
	const GPtrArray    *episodes
);

/* Where an episode that needs attention is put right: its releases (a
 * search's outcome), its import, else its row on the series' page
 * (transfer full). */
gchar *
axis_tv_web_attention_href(
	gint64                     series_id,
	const AxisTvAttentionItem *item
);

/* Where the series' most common reason is put right (its first episode's),
 * or %NULL when nothing needs attention (transfer full). */
gchar *
axis_tv_web_summary_href(const AxisTvSummary *summary);

/* What needs attention in a series, for its title page and its own page:
 * each reason in its words (the most common first, a few), its episodes
 * linked to where each is put right, and what to do about it; how many more
 * there are, with a link.  Pages an administrator alone may open are named,
 * not linked, for anyone else. */
HtmxNode *
axis_tv_web_attention(
	const AxisTvSummary *summary,
	gboolean             admin
);

/* ── What is happening (axis-tv-web-status.c) ───────────────────── */

/* The ids of the lines: "activity-episode-<id>", "activity-season-<n>",
 * and the series' own. */
#define AXIS_TV_WEB_STATUS_SERIES "activity-series"

gchar *
axis_tv_web_episode_line_id(gint64 episode_id);

/* An episode's line: what its download does, else the search in hand that
 * does its work, else what the last one found a while ago; empty and hidden
 * when nothing. */
HtmxNode *
axis_tv_web_episode_line(
	const AxisTvSeries  *series,
	const AxisTvEpisode *e,
	const AxisTvWork    *work
);

/* A season's line: the search in hand that does its work, else what the
 * last one found a while ago. */
HtmxNode *
axis_tv_web_season_line(
	const AxisTvSeries *series,
	gint                season,
	const AxisTvWork   *work
);

/* The series' line: how many of its searches are in hand (running,
 * waiting), how many episodes are downloading; else what its last search of
 * all of it found a while ago. */
HtmxNode *
axis_tv_web_series_line(
	const AxisTvSeries *series,
	const GPtrArray    *episodes,
	const AxisTvWork   *work
);

/* A search button (in <span id="act-episode-<id>|act-season-<n>|
 * act-undated-<n>|act-series">): the form, or, while a search that does its
 * work is in hand or ended a moment ago, a button that says so and cannot be
 * pressed.  @undated: the season's episodes with no air date (@item the
 * season); @back: (nullable) the page to come back to. */
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
);

/* A Manual search button ("act-manual-..."), or while one is in hand or
 * ended a moment ago, a link to its page. */
HtmxNode *
axis_tv_web_manual_act(
	HtmxRequest       *request,
	gint64             series_id,
	AxisTvScope        scope,
	gint64             item,
	gint               season,
	const AxisTvWork  *work,
	const gchar       *label
);

/* The notice of a click the work in hand answered ("already_searching",
 * "searched_recently") for @scope/@item, in words of where that work stands
 * now; %NULL for another word. */
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
);

/* When the series' page fetches its lines again (axis_web_activity_trigger()):
 * every while its searches or downloads are in hand, and once when a button
 * that says "a moment ago" may be pressed again. */
gchar *
axis_tv_web_trigger(
	const AxisTvWork *work,
	const GPtrArray  *episodes
);

/* The pages (axis-tv-web-view.c). */
void
axis_tv_web_register_pages(
	AxisTv  *self,
	AxisWeb *web
);

/* Changing a series' numbering: its confirmation page and the change
 * (axis-tv-web-numbering.c). */
void
axis_tv_web_register_links(
	AxisTv  *self,
	AxisWeb *web
);

void
axis_tv_web_register_numbering(
	AxisTv  *self,
	AxisWeb *web
);

G_END_DECLS

#endif /* AXIS_TV_WEB_H */
