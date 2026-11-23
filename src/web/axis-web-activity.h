/*
 * axis-web-activity.h - what Axis is doing about what a page shows: the
 * status lines, the buttons that say their work is in hand, and how a page
 * follows them live
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A page that lets a person start work (a search, a scan) shows, on the row
 * or card the work is for, one line of what is happening: what, since when,
 * how far where that is known, and once it has ended what came of it, for a
 * while.  The button that starts it says the work is in hand and is
 * disabled while it is.  Both are drawn on the server from the jobs in hand
 * (jobs/axis-job-live.h) and what the work recorded, so a plain reload is
 * right without any stream.
 *
 * Live, a page asks one hub topic per thing it shows work for: the jobs of a
 * key family ("act:" and the job keys up to their first number:
 * "act:tv.search:12" for every search of series 12) and the title's
 * downloads ("act:title:7").  When such a job or download changes, the
 * shell publishes the event %AXIS_WEB_ACTIVITY_EVENT on the topic (at most
 * twice a second, and only to a topic someone follows: never per row); the
 * page's one probe element then asks the page's own fragment, which answers
 * every status line of the page out of band in one request, read with the
 * page's own statements.  No script of ours: htmx and its SSE extension.
 */

#ifndef AXIS_WEB_ACTIVITY_H
#define AXIS_WEB_ACTIVITY_H

#include <glib.h>
#include <htmx-glib.h>

#include "jobs/axis-job-live.h"

G_BEGIN_DECLS

/* The event a page's probe reacts to. */
#define AXIS_WEB_ACTIVITY_EVENT     "activity"

/* The topic told of every change of work and downloads: the Activity
 * page's. */
#define AXIS_WEB_ACTIVITY_TOPIC_ALL "act:all"

/* How long the outcome of ended work stays on its line. */
#define AXIS_WEB_ACTIVITY_OUTCOME_S (15 * 60)

/* "12 seconds ago", "3 minutes ago", "2 hours ago", else the date. */
gchar *
axis_web_activity_ago(
	gint64 us,
	gint64 now_us
);

/* "18:02 UTC" (today), else "2026-11-18 18:02 UTC". */
gchar *
axis_web_activity_clock(
	gint64 us,
	gint64 now_us
);

/* The topic of a job key's family ("act:tv.search:12"), or %NULL for a key
 * that names no number or makes no valid topic. */
gchar *
axis_web_activity_topic(const gchar *key);

/* The topic of a title's downloads and imports: "act:title:7". */
gchar *
axis_web_activity_title_topic(gint64 title_id);

/**
 * axis_web_activity_state:
 * @job: a job in hand
 * @now_us: now
 *
 * Where it stands, to follow "A search of season 1 is ": "running, started
 * 12 seconds ago: 3 of 6 indexers have answered", "waiting for its turn,
 * queued 40 seconds ago, 4 ahead of it", "waiting to try again at 18:05
 * UTC".
 *
 * Returns: (transfer full): the words
 */
gchar *
axis_web_activity_state(
	const AxisJobLive *job,
	gint64             now_us
);

/* How a line looks: work under way, waiting, ended well, ended with nothing,
 * ended badly. */
typedef enum
{
	AXIS_WEB_ACTIVITY_RUNNING,
	AXIS_WEB_ACTIVITY_WAITING,
	AXIS_WEB_ACTIVITY_DONE,
	AXIS_WEB_ACTIVITY_NOTHING,
	AXIS_WEB_ACTIVITY_FAILED
} AxisWebActivityTone;

/* A search's outcome (its "outcome" word) in words that follow "it":
 * "found 6 releases and sent one to a download client", "found nothing",
 * "did not finish: <what it said>"; *@tone how its line looks. */
gchar *
axis_web_activity_outcome(
	const gchar          *outcome,
	const gchar          *outcome_text,
	guint                 n_found,
	AxisWebActivityTone  *tone
);

/* A status line: <p id="@id" class="activity-line activity-@tone"
 * role="status">; with @text %NULL an empty, hidden one (the place a live
 * update lands). */
HtmxNode *
axis_web_activity_line(
	const gchar         *id,
	AxisWebActivityTone  tone,
	const gchar         *text
);

/* A button that cannot be pressed now, saying why ("Searching", "Searched a
 * moment ago"), with @title as its tooltip. */
HtmxNode *
axis_web_activity_busy_button(
	const gchar *label,
	const gchar *title
);

/* The id of a page's probe, which its fragment sends again. */
#define AXIS_WEB_ACTIVITY_PROBE_ID "activity-probe"

/* How often a page with work in hand fetches its lines again without the
 * hub (a safety net for an event lost on the way). */
#define AXIS_WEB_ACTIVITY_SAFETY_S 30

/**
 * axis_web_activity_trigger:
 * @in_hand: something on the page is queued, running or downloading
 * @wake_s: seconds until a button that says "a moment ago" may be pressed
 *   again, 0 for none
 *
 * When a page's lines are fetched again: on %AXIS_WEB_ACTIVITY_EVENT, the
 * last of a burst winning (a delay each event starts again, not a throttle,
 * which drops the last); when the stream opens; while @in_hand every
 * %AXIS_WEB_ACTIVITY_SAFETY_S; once @wake_s after it is drawn.  Without
 * anything in hand nothing is fetched by time.
 *
 * Returns: (transfer full): an hx-trigger value
 */
gchar *
axis_web_activity_trigger(
	gboolean in_hand,
	gint64   wake_s
);

/* Seconds until a search that ended at @ended_us may be asked again (by
 * %AXIS_ACQUIRE_SEARCH_AGAIN_S), at the clock @now_us; 0 when it may. */
gint64
axis_web_activity_wake(
	gint64 ended_us,
	gint64 now_us
);

/* The sooner of two wakes (0 is none). */
gint64
axis_web_activity_sooner(
	gint64 a,
	gint64 b
);

/**
 * axis_web_activity_probe:
 * @request: the page's request (its topics); %NULL for the fragment's copy
 * @url: the page's fragment of status lines
 * @topics: (array zero-terminated=1) (nullable): the topics to follow
 * @trigger: axis_web_activity_trigger()
 *
 * Asks the page to follow @topics and returns the hidden element that, on
 * @trigger, gets @url and swaps nothing but the out-of-band elements of the
 * answer.  The answer sends it again (out of band) with the trigger of the
 * moment.
 *
 * Returns: (transfer full): the element
 */
HtmxNode *
axis_web_activity_probe(
	HtmxRequest        *request,
	const gchar        *url,
	const gchar *const *topics,
	const gchar        *trigger
);

/* Marks @node to replace its namesake on the page (hx-swap-oob) and adds it
 * to @fragment. */
void
axis_web_activity_add_oob(
	HtmxNode *fragment,
	HtmxNode *node
);

/* The answer to a probe: @fragment's elements, as text/html. */
HtmxResponse *
axis_web_activity_respond(HtmxNode *fragment);

G_END_DECLS

#endif /* AXIS_WEB_ACTIVITY_H */
