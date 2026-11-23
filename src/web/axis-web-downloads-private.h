/*
 * axis-web-downloads-private.h - shared by the files of the downloads pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-web-downloads.c          the module: work off the main loop, live
 *                                 progress, the job, the dashboard part
 *   axis-web-downloads-clients.c  the Download clients pages
 *   axis-web-downloads-pages.c    the Downloads page and its rows
 */

#ifndef AXIS_WEB_DOWNLOADS_PRIVATE_H
#define AXIS_WEB_DOWNLOADS_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_DOWNLOADS_MAX_PENDING 16

/* Live progress is published at most this often. */
#define AXIS_WEB_DOWNLOADS_COALESCE_MS 400

struct _AxisWebDownloads
{
	AxisDownloads *downloads;	/* owned reference */
	AxisStorage   *storage;	/* owned reference */
	AxisWebHub    *hub;		/* nullable, not owned */
	guint          pending;	/* (main loop) dispatched, not yet answered */
	gboolean       stopping;	/* (main loop) */
	/* Live progress: what the layer said changed, from any thread. */
	GMutex         lock;
	GHashTable    *dirty;	/* (lock) gint64 * */
	gboolean       set_changed;	/* (lock) */
	guint          flush_id;	/* (lock) the timeout that will publish */
	gboolean       flushing;	/* (main loop) a read is in flight */
	gint           closed;	/* atomic: no more flushes */
};

typedef void (*AxisWebDownloadsWorkFunc)(AxisWebDownloads *self, gpointer data);

typedef HtmxResponse *(*AxisWebDownloadsDoneFunc)(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop; with @request %NULL, @done is called with it
 * and its answer is ignored.  Returns the deferred response for the handler
 * to return, or at once a 503 (and %NULL without a request) when too much
 * is waiting or the pages are stopping (then neither runs).  @free_data is
 * called on the main loop in every case.
 */
HtmxResponse *
axis_web_downloads_dispatch(
	AxisWebDownloads         *self,
	HtmxRequest              *request,
	AxisWebDownloadsWorkFunc  work,
	AxisWebDownloadsDoneFunc  done,
	gpointer                  data,
	GDestroyNotify            free_data
);

/* ── Pieces (axis-web-downloads-pages.c) ────────────────────────── */

/* "5 seconds ago", "3 hours ago", "2 days ago". */
gchar *
axis_web_downloads_age_text(gint64 us);

/* A thin bar of @fraction (0 to 1), as the dashboard's space bars. */
HtmxNode *
axis_web_downloads_bar(gdouble fraction);

/* The state badge of a client. */
HtmxNode *
axis_web_downloads_client_badge(AxisDownloadClientState state);

/* One row of the in-progress table (id download-N), for a page or the hub. */
HtmxNode *
axis_web_downloads_active_row(
	const AxisDownloadInfo *info,
	gboolean                admin
);

/* The columns of the in-progress table. */
const AxisWebColumn *
axis_web_downloads_active_columns(gboolean admin);

/* Whether any of @downloads (AxisDownloadInfo, nullable) is the built-in
 * client's (a YouTube video, which Axis downloads itself): with none set up
 * by an administrator there are still downloads to show. */
gboolean
axis_web_downloads_any_builtin(GPtrArray *downloads);

/* What a download is (its title, else its release name). */
const gchar *
axis_web_downloads_label(const AxisDownloadInfo *info);

/* ── Pages ──────────────────────────────────────────────────────── */

void
axis_web_downloads_register_pages(
	AxisWebDownloads *self,
	AxisWeb          *web
);

void
axis_web_downloads_register_clients(
	AxisWebDownloads *self,
	AxisWeb          *web
);

G_END_DECLS

#endif /* AXIS_WEB_DOWNLOADS_PRIVATE_H */
