/*
 * axis-web-profiles-private.h - shared by the files of the profile pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_PROFILES_PRIVATE_H
#define AXIS_WEB_PROFILES_PRIVATE_H

#include <htmx-glib.h>

#include "compat/axis-compat-report.h"
#include "web/axis-web-analysis-private.h"
#include "web/axis-web-profiles.h"

G_BEGIN_DECLS

#define AXIS_WEB_PROFILES_MAX_PENDING 16

/* How long the edit page's preview may read the index. */
#define AXIS_WEB_PROFILES_PREVIEW_MS 4000

struct _AxisWebProfiles
{
	AxisEstimator *estimator;	/* owned reference */
	AxisCompat    *compat;		/* owned reference, or NULL */
	guint          pending;
	gboolean       stopping;
};

typedef void (*AxisWebProfilesWorkFunc)(AxisWebProfiles *self, gpointer data);

typedef HtmxResponse *(*AxisWebProfilesDoneFunc)(
	AxisWebProfiles *self,
	HtmxRequest     *request,
	gpointer         data
);

HtmxResponse *
axis_web_profiles_dispatch(
	AxisWebProfiles         *self,
	HtmxRequest             *request,
	AxisWebProfilesWorkFunc  work,
	AxisWebProfilesDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
);

/* The compatibility report when the module is on (NULL otherwise, or when
 * it cannot be worked out: the pages show the rest). */
AxisCompatReport *
axis_web_profiles_compat_report(AxisWebProfiles *self);

/* "Video: Space saver" */
gchar *
axis_web_profiles_label(const AxisProfileRevision *revision);

/* The lock state as a badge and a sentence. */
HtmxNode *
axis_web_profiles_lock_badge(const AxisProfileRevision *revision);

const gchar *
axis_web_profiles_lock_sentence(const AxisProfileRevision *revision);

/* An error for a store failure (404, 403, 409, or 500). */
HtmxResponse *
axis_web_profiles_error(
	HtmxRequest  *request,
	const GError *error
);

void
axis_web_profiles_register_pages(
	AxisWebProfiles *self,
	AxisWeb         *web
);

void
axis_web_profiles_register_form(
	AxisWebProfiles *self,
	AxisWeb         *web
);

G_END_DECLS

#endif /* AXIS_WEB_PROFILES_PRIVATE_H */
