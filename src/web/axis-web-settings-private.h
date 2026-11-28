/*
 * axis-web-settings-private.h - shared by the files of the Settings and
 * Modules pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_SETTINGS_PRIVATE_H
#define AXIS_WEB_SETTINGS_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web-settings.h"
#include "web/axis-web-auth-private.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_SETTINGS_MAX_PENDING 16

struct _AxisWebSettings
{
	AxisSettings *settings;	/* not owned */
	AxisDbPool   *pool;	/* not owned, nullable */
	AxisWeb      *web;	/* set when the routes are registered */
	guint         subscription;
	guint         theme_subscription;
	guint         log_level_subscription;
	guint         pending;	/* dispatched, not yet answered */
	gboolean      stopping;
	gboolean      module_busy;	/* a module switch is being stored */
};

typedef void (*AxisWebSettingsWorkFunc)(gpointer data);

typedef HtmxResponse *(*AxisWebSettingsDoneFunc)(
	AxisWebSettings *self,
	HtmxRequest     *request,
	gpointer         data
);

/*
 * Runs @work(@data) on a worker thread, then answers @request with what
 * @done returns, on the main loop.  Returns the deferred response for the
 * handler to return, or at once a 503 when too much is waiting or the
 * pages are stopping (then neither runs).  @free_data is called on the
 * main loop in every case.
 */
HtmxResponse *
axis_web_settings_dispatch(
	AxisWebSettings         *self,
	HtmxRequest             *request,
	AxisWebSettingsWorkFunc  work,
	AxisWebSettingsDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
);

/* The registry the shell serves. */
AxisModuleRegistry *
axis_web_settings_registry(AxisWebSettings *self);

/* Adds @child to @parent and drops the caller's reference. */
void
axis_web_settings_add(
	gpointer parent,
	gpointer child
);

/* A card with an h2 heading. */
HtmxNode *
axis_web_settings_card(const gchar *heading);

/* The pages' routes. */
void
axis_web_settings_register_settings_page(AxisWebSettings *self, AxisWeb *web);

/* The History card's quick choices (web/axis-web-settings-presets.c): the
 * row of buttons for @module_id, or %NULL when the module has none; and the
 * route the buttons post to. */
HtmxNode *
axis_web_settings_presets_row(
	AxisSettings *settings,
	HtmxRequest  *request,
	const gchar  *module_id
);

void
axis_web_settings_register_presets(AxisWebSettings *self, AxisWeb *web);

void
axis_web_modules_register_page(AxisWebSettings *self, AxisWeb *web);

/* The Outbound network page (web/axis-web-outbound-page.c). */
void
axis_web_outbound_register_page(AxisWebSettings *self, AxisWeb *web);

G_END_DECLS

#endif /* AXIS_WEB_SETTINGS_PRIVATE_H */
