/*
 * axis-web-compat-private.h - shared by the files of the Compatibility pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_COMPAT_PRIVATE_H
#define AXIS_WEB_COMPAT_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web-analysis-private.h"
#include "web/axis-web-compat.h"

G_BEGIN_DECLS

/* Page work waiting for a thread beyond this is answered 503. */
#define AXIS_WEB_COMPAT_MAX_PENDING 16

struct _AxisWebCompat
{
	AxisCompat *compat;	/* owned reference */
	guint       pending;
	gboolean    stopping;
};

typedef void (*AxisWebCompatWorkFunc)(AxisWebCompat *self, gpointer data);

typedef HtmxResponse *(*AxisWebCompatDoneFunc)(
	AxisWebCompat *self,
	HtmxRequest   *request,
	gpointer       data
);

HtmxResponse *
axis_web_compat_dispatch(
	AxisWebCompat         *self,
	HtmxRequest           *request,
	AxisWebCompatWorkFunc  work,
	AxisWebCompatDoneFunc  done,
	gpointer               data,
	GDestroyNotify         free_data
);

void
axis_web_compat_register_pages(
	AxisWebCompat *self,
	AxisWeb       *web
);

G_END_DECLS

#endif /* AXIS_WEB_COMPAT_PRIVATE_H */
