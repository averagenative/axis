/*
 * axis-web-jobs-view.h - the elements of the Jobs pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One builder per element, used for the first load of a page and for the
 * live updates alike, so the two cannot drift apart.  Everything that
 * came from the database goes in as element text or an attribute value,
 * which the elements escape; nothing here writes markup by hand.
 *
 * Live fragments are shared by every viewer of a topic, so they cannot
 * hold anything that belongs to one session.  The CSRF token is not in
 * them: the buttons are htmx requests, which carry it from the page's
 * body.  What differs by role is the buttons, so there are two variants
 * of each fragment, published on two topics.
 */

#ifndef AXIS_WEB_JOBS_VIEW_H
#define AXIS_WEB_JOBS_VIEW_H

#include <glib.h>
#include <htmx-glib.h>

#include "web/axis-web-jobs-data.h"

G_BEGIN_DECLS

/* Topics.  Administrators follow the "admin" variants, which carry the
 * cancel and retry buttons; others follow the plain ones. */
#define AXIS_JOBS_TOPIC_ACTIVITY    "activity"
#define AXIS_JOBS_TOPIC_LIST        "jobs"
#define AXIS_JOBS_TOPIC_LIST_ADMIN  "jobs:admin"

/* Events the page reacts to: "oob" carries elements that replace their
 * namesakes (see axis_web_page_add_topic()), "jobs-changed" tells a list
 * to fetch itself again. */
#define AXIS_JOBS_EVENT_OOB         "oob"
#define AXIS_JOBS_EVENT_CHANGED     "jobs-changed"

/* The topic of one job's page; the caller frees it. */
gchar *
axis_web_jobs_topic_for_job(
	gint64    id,
	gboolean  admin
);

/* The page that lists what Axis is doing, in a person's words. */
#define AXIS_JOBS_ACTIVITY_PATH     "/activity"

/* <a id="activity">: how many things Axis is doing now (jobs of work
 * running and downloads in progress), what they are by kind ("2 searches,
 * 1 download"), linking to the Activity page.  One element: the header may
 * put it where it likes. */
HtmxNode *
axis_web_jobs_activity_new(const AxisJobsActivity *activity);

/* <span class="job-name">: what the job does in plain words, linking to its
 * page, and its kind and number as a quiet detail beside; a kind without a
 * label is named by itself. */
HtmxNode *
axis_web_jobs_name_new(const AxisJobRow *row);

/* <tr id="job-N">: one row of the list. */
HtmxNode *
axis_web_jobs_row_new(
	const AxisJobRow *row,
	gboolean          admin
);

/* <section id="job-detail">: everything about one job. */
HtmxNode *
axis_web_jobs_detail_new(
	const AxisJobRow *row,
	gboolean          admin
);

/* <section id="job-payload">: the payload as text.  Not part of the detail
 * fragment, which live updates replace: a payload can hold anything a
 * module put in it, so only administrators are shown it, and it does not
 * change while the job exists. */
HtmxNode *
axis_web_jobs_payload_new(const AxisJobRow *row);

/* <section id="job-earlier-runs">: how the job ended before each retry
 * (row->earlier, from axis_web_jobs_get()), newest first, with who retried
 * it for an administrator; %NULL when it has none.  Not part of the detail
 * fragment either: it changes only with a retry, which loads the page
 * again. */
HtmxNode *
axis_web_jobs_earlier_new(
	const AxisJobRow *row,
	gboolean          admin
);

/* <div id="jobs-list">: the table of @rows (element-type AxisJobRow) and
 * the links to the neighbouring pages.  It fetches itself again when the
 * stream says the list changed or (re)opened.  @next is the cursor of the
 * next page, or %NULL.  @brief: <div id="upkeep-list"> instead, the first
 * rows only, with a link to the rest when @next is set. */
HtmxNode *
axis_web_jobs_list_new(
	const AxisJobFilter *filter,
	GPtrArray           *rows,
	const gchar         *next,
	gboolean             admin,
	gboolean             brief
);

/* <span id="upkeep-counts">: how much upkeep runs and waits. */
HtmxNode *
axis_web_jobs_upkeep_counts_new(
	gint64 running,
	gint64 queued
);

/* <details id="jobs-upkeep">, closed: the heading "Upkeep" with its counts,
 * and the brief list of @rows (the filter's state and queue, upkeep
 * only). */
HtmxNode *
axis_web_jobs_upkeep_new(
	const AxisJobFilter *filter,
	GPtrArray           *rows,
	const gchar         *next,
	gboolean             admin,
	gint64               running,
	gint64               queued
);

/* The filter form: selects for work or upkeep, state and queue (@queues is
 * %NULL terminated, or %NULL). */
HtmxNode *
axis_web_jobs_filter_form_new(
	const AxisJobFilter *filter,
	const gchar *const  *queues
);

/* @node rendered as an out-of-band replacement of the element with its id.
 * Returns: (transfer full): markup */
gchar *
axis_web_jobs_render_oob(HtmxNode *node);

G_END_DECLS

#endif /* AXIS_WEB_JOBS_VIEW_H */
