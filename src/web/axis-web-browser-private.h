/*
 * axis-web-browser-private.h - shared by the files of the Library pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_BROWSER_PRIVATE_H
#define AXIS_WEB_BROWSER_PRIVATE_H

#include <htmx-glib.h>

#include "library/axis-library-browse.h"
#include "library/axis-library-filter.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-storage-private.h"

G_BEGIN_DECLS

/* Page work waiting for a thread beyond this is answered 503. */
#define AXIS_WEB_BROWSER_MAX_PENDING 16

/* What tells the folders of one media type's roots apart. */
typedef struct
{
	AxisWebFolderTitlesFunc  func;	/* nullable: none */
	gpointer                 user_data;
	gchar                   *module_id;	/* owned */
} AxisWebBrowserTitles;

struct _AxisWebBrowser
{
	AxisLibrary *library;	/* owned reference */
	AxisProbe   *probe;	/* owned reference */
	AxisStorage *storage;	/* owned reference */
	AxisEstimator *estimator;	/* owned reference, nullable: lists by estimate */
	AxisWebBrowserTitles titles[AXIS_MEDIA_TYPE_COUNT];
	AxisWebHub  *hub;	/* nullable, not owned */
	guint        pending;	/* dispatched, not yet answered */
	gboolean     stopping;
	guint        listener;	/* the storage listener */
	guint        tick_id;	/* the live timer */
	gboolean     tick_busy;	/* a tick's query is in flight */
	gboolean     primed;	/* the live state below is current */
	GHashTable  *live;	/* gint64 root -> LiveRoot */
};

typedef void (*AxisWebBrowserWorkFunc)(AxisWebBrowser *self, gpointer data);

typedef HtmxResponse *(*AxisWebBrowserDoneFunc)(
	AxisWebBrowser *self,
	HtmxRequest    *request,
	gpointer        data
);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop.  Returns the deferred response for the handler
 * to return, or at once a 503 when too much is waiting or the pages are
 * stopping (then neither runs).  @free_data is called on the main loop in
 * every case.
 */
HtmxResponse *
axis_web_browser_dispatch(
	AxisWebBrowser         *self,
	HtmxRequest            *request,
	AxisWebBrowserWorkFunc  work,
	AxisWebBrowserDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
);

/* ── Live state (axis-web-browser.c) ─────────────────────────────── */

void
axis_web_browser_live_start(AxisWebBrowser *self);

void
axis_web_browser_live_stop(AxisWebBrowser *self);

/* ── Pieces (axis-web-browser-view.c) ───────────────────────────── */

#define BROWSER_ADD axis_web_storage_add

HtmxNode *
axis_web_browser_link(
	const gchar *href,
	const gchar *text
);

HtmxNode *
axis_web_browser_facts_new(void);

void
axis_web_browser_fact(
	HtmxNode    *list,
	const gchar *label,
	const gchar *text
);

void
axis_web_browser_fact_node(
	HtmxNode    *list,
	const gchar *label,
	HtmxNode    *value
);

HtmxNode *
axis_web_browser_badge(
	const gchar *text,
	const gchar *css
);

HtmxNode *
axis_web_browser_warning(const gchar *text);

/* A name (one segment, or a whole path) in its stored form, as text
 * inside <bdi>, with a marker when it is not valid UTF-8. */
HtmxNode *
axis_web_browser_name_node(const gchar *stored);

gchar *
axis_web_browser_format_size(guint64 bytes);

gchar *
axis_web_browser_format_count(guint64 value);

/* The scan progress of a root (id root-N-progress): the bar and counts
 * while a scan runs, a line while one is requested, else nothing. */
HtmxNode *
axis_web_browser_progress_block(const AxisRootOverview *root);

/* The files table of a folder or the flat list. */
HtmxNode *
axis_web_browser_file_table(
	GPtrArray *files,
	gboolean   show_directory
);

/* The paging line: "Next" when @next_href is set, "First page" when
 * @first_href is. */
HtmxNode *
axis_web_browser_pager(
	const gchar *first_href,
	const gchar *next_href
);

/* ── Pages ──────────────────────────────────────────────────────── */

void
axis_web_browser_register_pages(
	AxisWebBrowser *self,
	AxisWeb        *web
);

void
axis_web_browser_register_actions(
	AxisWebBrowser *self,
	AxisWeb        *web
);

/* What needs attention in the library (axis-web-browser-dashboard.c). */
void
axis_web_browser_register_dashboard(
	AxisWebBrowser *self,
	AxisWeb        *web
);

G_END_DECLS

#endif /* AXIS_WEB_BROWSER_PRIVATE_H */
