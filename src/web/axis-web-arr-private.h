/*
 * axis-web-arr-private.h - shared by the files of the bring-over pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-web-arr.c          the module, its jobs, the connections' pages
 *   axis-web-arr-preview.c  the preview and its mapping tables
 */

#ifndef AXIS_WEB_ARR_PRIVATE_H
#define AXIS_WEB_ARR_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web.h"
#include "web/axis-web-arr.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_ARR_MAX_PENDING 16

/* The same for the preview page, which counts a whole plan. */
#define AXIS_WEB_ARR_PREVIEW_PENDING 2

struct _AxisWebArr
{
	AxisArr  *arr;		/* owned reference */
	guint     pending;	/* (main loop) dispatched, not yet answered */
	gboolean  stopping;	/* (main loop) */
};

typedef void (*AxisWebArrWorkFunc)(AxisWebArr *self, gpointer data);

typedef HtmxResponse *(*AxisWebArrDoneFunc)(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop.  Returns the deferred response for the handler
 * to return, or at once a 503 when too much is waiting or the pages are
 * stopping (then neither runs).  @free_data is called on the main loop in
 * every case.
 */
HtmxResponse *
axis_web_arr_dispatch(
	AxisWebArr         *self,
	HtmxRequest        *request,
	AxisWebArrWorkFunc  work,
	AxisWebArrDoneFunc  done,
	gpointer            data,
	GDestroyNotify      free_data
);

/* As axis_web_arr_dispatch(), with @limit tasks waiting at most: the preview
 * of a large plan is dear, and few run at once. */
HtmxResponse *
axis_web_arr_dispatch_limited(
	AxisWebArr         *self,
	HtmxRequest        *request,
	guint               limit,
	AxisWebArrWorkFunc  work,
	AxisWebArrDoneFunc  done,
	gpointer            data,
	GDestroyNotify      free_data
);

/* A form field, kept only when it is short, valid text. */
gchar *
axis_web_arr_form_text(
	HtmxRequest *request,
	const gchar *name,
	gsize        max_bytes
);

/* A labelled <select> in a div.field; @values and @labels end with %NULL. */
HtmxNode *
axis_web_arr_select(
	const gchar        *id,
	const gchar        *label,
	const gchar        *name,
	const gchar *const *values,
	const gchar *const *labels,
	const gchar        *current
);

/* A labelled checkbox, value "yes", ticked when @checked. */
HtmxNode *
axis_web_arr_checkbox(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
);

/* "/migrate/N" and what follows it. */
gchar *
axis_web_arr_path(
	gint64       source_id,
	const gchar *rest
);

void
axis_web_arr_register_preview(
	AxisWebArr *self,
	AxisWeb    *web
);

/* What the connection brought, on its page: the counts, the holds, and the
 * way to the list and to "Start searching" (axis-web-arr-run.c). */
HtmxNode *
axis_web_arr_brought_card(
	const AxisArrSource *source,
	const AxisArrRun    *run
);

/* The list of what was brought, and "Start searching". */
void
axis_web_arr_register_run(
	AxisWebArr *self,
	AxisWeb    *web
);

G_END_DECLS

#endif /* AXIS_WEB_ARR_PRIVATE_H */
