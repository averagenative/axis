/*
 * axis-web-identify-private.h - shared by the files of the Identify pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-web-identify.c        the module: work off the main loop, the jobs,
 *                              the dashboard's item, the title page's part
 *   axis-web-identify-pages.c  the pages and the actions
 */

#ifndef AXIS_WEB_IDENTIFY_PRIVATE_H
#define AXIS_WEB_IDENTIFY_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web.h"
#include "web/axis-web-identify.h"

G_BEGIN_DECLS

#define AXIS_WEB_IDENTIFY_MAX_PENDING 16

/* Units of each review list on a root's page, and per page of one list. */
#define AXIS_WEB_IDENTIFY_SHOWN       10
#define AXIS_WEB_IDENTIFY_PAGE        25

struct _AxisWebIdentify
{
	AxisIdentify     *identify;	/* owned reference */
	AxisQualityStore *quality;	/* owned reference */
	guint             pending;	/* (main loop) */
	gboolean          stopping;	/* (main loop) */
};

typedef void (*AxisWebIdentifyWorkFunc)(AxisWebIdentify *self, gpointer data);

typedef HtmxResponse *(*AxisWebIdentifyDoneFunc)(
	AxisWebIdentify *self,
	HtmxRequest     *request,
	gpointer         data
);

/* As axis_web_imports_dispatch(). */
HtmxResponse *
axis_web_identify_dispatch(
	AxisWebIdentify         *self,
	HtmxRequest             *request,
	AxisWebIdentifyWorkFunc  work,
	AxisWebIdentifyDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
);

void
axis_web_identify_register_pages(
	AxisWebIdentify *self,
	AxisWeb         *web
);

/* A unit's videos as a table (the title page shows them too).  @offer_link:
 * each row offers "Link to a movie or episode by hand" (an administrator, a
 * unit Axis did not match). */
HtmxNode *
axis_web_identify_files_table(
	const AxisIdentifyUnitInfo *unit,
	gboolean                    offer_link
);

/* "Sure", "Linked by a person", "Likely", ... as a badge. */
HtmxNode *
axis_web_identify_badge(const AxisIdentifyUnitInfo *unit);

G_END_DECLS

#endif /* AXIS_WEB_IDENTIFY_PRIVATE_H */
