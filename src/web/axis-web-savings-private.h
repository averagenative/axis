/*
 * axis-web-savings-private.h - shared by the files of the Savings pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_SAVINGS_PRIVATE_H
#define AXIS_WEB_SAVINGS_PRIVATE_H

#include <htmx-glib.h>

#include "compat/axis-compat.h"
#include "web/axis-web-analysis-private.h"
#include "web/axis-web-savings.h"

G_BEGIN_DECLS

/* Page work waiting for a thread beyond this is answered 503. */
#define AXIS_WEB_SAVINGS_MAX_PENDING 16

struct _AxisWebSavings
{
	AxisEstimator *estimator;	/* owned reference */
	AxisCompat    *compat;		/* owned reference, or NULL: the verdicts are not shown */
	guint          pending;
	gboolean       stopping;
};

typedef void (*AxisWebSavingsWorkFunc)(AxisWebSavings *self, gpointer data);

typedef HtmxResponse *(*AxisWebSavingsDoneFunc)(
	AxisWebSavings *self,
	HtmxRequest    *request,
	gpointer        data
);

HtmxResponse *
axis_web_savings_dispatch(
	AxisWebSavings         *self,
	HtmxRequest            *request,
	AxisWebSavingsWorkFunc  work,
	AxisWebSavingsDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
);

/* "about 3.1 GB (2.4 GB to 3.9 GB)" */
gchar *
axis_web_savings_range(
	gint64 mid,
	gint64 low,
	gint64 high
);

void
axis_web_savings_register_pages(
	AxisWebSavings *self,
	AxisWeb        *web
);

G_END_DECLS

#endif /* AXIS_WEB_SAVINGS_PRIVATE_H */
