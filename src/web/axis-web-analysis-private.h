/*
 * axis-web-analysis-private.h - shared by the files of the Analysis pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_ANALYSIS_PRIVATE_H
#define AXIS_WEB_ANALYSIS_PRIVATE_H

#include <htmx-glib.h>

#include "library/axis-analysis.h"
#include "library/axis-library-filter.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

G_BEGIN_DECLS

/* Page work waiting for a thread beyond this is answered 503. */
#define AXIS_WEB_ANALYSIS_MAX_PENDING 16

struct _AxisWebAnalysis
{
	AxisAnalysis *analysis;	/* owned reference */
	guint         pending;	/* dispatched, not yet answered */
	gboolean      stopping;
};

typedef void (*AxisWebAnalysisWorkFunc)(AxisWebAnalysis *self, gpointer data);

typedef HtmxResponse *(*AxisWebAnalysisDoneFunc)(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	gpointer         data
);

/* Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop; a 503 at once when too much is waiting. */
HtmxResponse *
axis_web_analysis_dispatch(
	AxisWebAnalysis         *self,
	HtmxRequest             *request,
	AxisWebAnalysisWorkFunc  work,
	AxisWebAnalysisDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
);

/* ── Charts (axis-web-analysis-chart.c) ─────────────────────────── */

/**
 * AxisChartBar:
 * @label: the row's name (a name from the library: shown as text only)
 * @value: the size of the bar
 * @value_text: what is written beside it
 * @href: (nullable): where the row goes (a path made of safe characters)
 * @muted: drawn in the colour of "last known" figures
 */
typedef struct
{
	gchar    *label;
	gint64    value;
	gchar    *value_text;
	gchar    *href;
	gboolean  muted;
} AxisChartBar;

AxisChartBar *
axis_chart_bar_new(
	const gchar *label,
	gint64       value,
	const gchar *value_text,
	const gchar *href,
	gboolean     muted
);

void
axis_chart_bar_free(AxisChartBar *bar);

/* Horizontal bars as an SVG (at most 30 rows): geometry in attributes,
 * colour in the stylesheet, each row a link when it has a @href.  A chart
 * never stands alone: the page also has the table. */
HtmxNode *
axis_web_analysis_bar_chart(
	const gchar *title,
	GPtrArray   *bars	/* AxisChartBar */
);

/* A line of the library's size over time. */
HtmxNode *
axis_web_analysis_growth_chart(
	const gchar *title,
	GPtrArray   *points	/* AxisAnalysisPoint, oldest first */
);

/* ── Pieces (axis-web-analysis-view.c) ──────────────────────────── */

/* "/library/files?<query>" for @filter; %NULL when the file list would
 * refuse it (a value its parser does not accept). */
gchar *
axis_web_analysis_files_href(const AxisLibraryFilter *filter);

/* A cell or a span holding @text, a link when @href is set. */
HtmxNode *
axis_web_analysis_figure(
	const gchar *text,
	const gchar *href
);

/* What the snapshot could not see, as notices and a list; @scope picks the
 * lines that matter to the page. */
typedef enum
{
	AXIS_ANALYSIS_COVER_SCAN  = 1 << 0,
	AXIS_ANALYSIS_COVER_PROBE = 1 << 1,
	AXIS_ANALYSIS_COVER_HDR   = 1 << 2,
	AXIS_ANALYSIS_COVER_ALL   = 7
} AxisAnalysisCoverScope;

HtmxNode *
axis_web_analysis_coverage(
	const AxisAnalysisSnapshot *snapshot,
	guint                       scope
);

/* When the snapshot was taken and how, and the refresh control. */
HtmxNode *
axis_web_analysis_snapshot_header(
	HtmxRequest                *request,
	const AxisAnalysisSnapshot *snapshot,
	const AxisAnalysisStatus   *status
);

/* The refreshing state (id analysis-status). */
HtmxNode *
axis_web_analysis_status_block(const AxisAnalysisStatus *status);

/* The names of a volume or root in the snapshot, or "(removed)". */
const gchar *
axis_web_analysis_volume_name(
	const AxisAnalysisSnapshot *snapshot,
	gint64                      id
);

const gchar *
axis_web_analysis_root_name(
	const AxisAnalysisSnapshot *snapshot,
	gint64                      id
);

/* The page for a snapshot that does not exist yet. */
HtmxNode *
axis_web_analysis_empty(
	HtmxRequest              *request,
	const AxisAnalysisStatus *status
);

/* "1 file", "12 files". */
gchar *
axis_web_analysis_count_label(gint64 count);

/* A conversion profile as the Space savings and Player compatibility pages
 * write it: "Video: Space saver", or the name alone when the family and the
 * name are the same word ("Audiobooks"). */
gchar *
axis_web_analysis_profile_label(
	const gchar *family,
	const gchar *name
);

/* Navigation between the reports. */
HtmxNode *
axis_web_analysis_tabs(const gchar *current);

/* ── Pages (axis-web-analysis-pages.c) ──────────────────────────── */

void
axis_web_analysis_register_pages(
	AxisWebAnalysis *self,
	AxisWeb         *web
);

G_END_DECLS

#endif /* AXIS_WEB_ANALYSIS_PRIVATE_H */
