/*
 * axis-web-quality-private.h - shared by the files of the quality pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_QUALITY_PRIVATE_H
#define AXIS_WEB_QUALITY_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web-browser-private.h"
#include "web/axis-web-quality.h"

G_BEGIN_DECLS

#define AXIS_WEB_QUALITY_MAX_PENDING 16

/* At most this many characters of a release name are tried. */
#define AXIS_WEB_QUALITY_NAME_MAX AXIS_RELEASE_NAME_MAX

struct _AxisWebQuality
{
	AxisQualityStore *store;	/* owned reference */
	AxisCompat       *compat;	/* owned reference, or NULL */
	guint             pending;
	gboolean          stopping;
};

typedef void (*AxisWebQualityWorkFunc)(AxisWebQuality *self, gpointer data);

typedef HtmxResponse *(*AxisWebQualityDoneFunc)(
	AxisWebQuality *self,
	HtmxRequest    *request,
	gpointer        data
);

/* Runs @work on a worker thread, then answers with what @done returns on
 * the main loop (a 503 at once when too much is waiting). */
HtmxResponse *
axis_web_quality_dispatch(
	AxisWebQuality         *self,
	HtmxRequest            *request,
	AxisWebQualityWorkFunc  work,
	AxisWebQualityDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
);

/* An error for a store failure (404, 403, 409, 422 or 500). */
HtmxResponse *
axis_web_quality_error(
	HtmxRequest  *request,
	const GError *error
);

/* Only what a quality name, a number, a size or a list of rule values is
 * made of: what may go back into an attribute. */
gchar *
axis_web_quality_reflect(const gchar *text);

/* "/quality/<slug>" */
gchar *
axis_web_quality_href(const gchar *slug);

/* The copy form of @stored, with its name field. */
HtmxNode *
axis_web_quality_copy_form(
	HtmxRequest             *request,
	const AxisQualityStored *stored
);

/* The Try a name form, @profiles (AxisQualityStored) to choose from, with
 * the values given (each nullable). */
HtmxNode *
axis_web_quality_try_form(
	GPtrArray   *profiles,
	const gchar *name,
	const gchar *slug,
	const gchar *runtime,
	const gchar *size
);

void
axis_web_quality_register_pages(
	AxisWebQuality *self,
	AxisWeb        *web
);

void
axis_web_quality_register_form(
	AxisWebQuality *self,
	AxisWeb        *web
);

G_END_DECLS

#endif /* AXIS_WEB_QUALITY_PRIVATE_H */
