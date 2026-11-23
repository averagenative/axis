/*
 * axis-web-downloads.h - the downloads module: its tracking job, its
 * settings, the Download clients pages (System, administrators), the
 * Downloads page (Activity, everyone logged in) and the dashboard's
 * "Coming in"
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /downloads                     in progress (live), then recent
 *   GET  /downloads/list?part=active|recent   one of its lists (htmx)
 *   GET  /downloads/remove/:id          confirm removing one (admin)
 *   POST /downloads/remove/:id          remove it, with or without its
 *                                       files at the client (admin)
 *   GET  /downloads/clients             the clients, and adding one (admin)
 *   POST /downloads/clients             add one (admin)
 *   GET  /downloads/clients/:id         one client: state, settings,
 *                                       path mappings, removing it (admin)
 *   POST /downloads/clients/:id         save its settings (admin)
 *   POST /downloads/clients/:id/test    test it now (admin)
 *   POST /downloads/clients/:id/maps    add a path mapping (admin)
 *   POST /downloads/clients/:id/maps/remove   remove one (admin)
 *   POST /downloads/clients/:id/delete  forget the client (admin)
 *
 * A client's secret is never put in a page: its field is a password field
 * with no value, which replaces the stored secret only when something is
 * typed in it; the submitted value is used once and wiped.  Progress is
 * published live on the topic "downloads" (rows without actions) and
 * "downloads:admin" (rows with them).
 *
 * A form shown in the setup guide (the functions below, given a return
 * path) posts to these same handlers and comes back to the guide
 * (web/axis-web-setup-path.h).
 */

#ifndef AXIS_WEB_DOWNLOADS_H
#define AXIS_WEB_DOWNLOADS_H

#include "core/axis-module.h"
#include "download/axis-download.h"
#include "storage/axis-storage.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebDownloads AxisWebDownloads;

#define AXIS_DOWNLOADS_TOPIC         "downloads"
#define AXIS_DOWNLOADS_TOPIC_ADMIN   "downloads:admin"
#define AXIS_DOWNLOADS_EVENT_OOB     "oob"
#define AXIS_DOWNLOADS_EVENT_CHANGED "downloads-changed"

/**
 * axis_web_downloads_new:
 * @hub: (nullable): where live progress is published; not owned
 *
 * Returns: (transfer full): the module's state
 */
AxisWebDownloads *
axis_web_downloads_new(
	AxisDownloads *downloads,
	AxisStorage   *storage,
	AxisWebHub    *hub
);

/* Waits for the page work in flight and stops publishing (main loop). */
void
axis_web_downloads_stop(AxisWebDownloads *self);

void
axis_web_downloads_free(AxisWebDownloads *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebDownloads, axis_web_downloads_free)

const AxisModuleInfo *
axis_web_downloads_get_module(void);

/* ── The forms, wherever they are shown ────────────────────────── */

/**
 * axis_web_downloads_add_form:
 * @return_path: (nullable): an address of the setup guide to come back to
 *
 * The Download clients page's "Add a client" form, empty and switched on,
 * with Axis's own category.
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_downloads_add_form(
	AxisDownloads *downloads,
	HtmxRequest   *request,
	const gchar   *return_path
);

/* A client's own settings form, as its page has it (never its secret); the
 * fields' ids are unique to the client. */
HtmxNode *
axis_web_downloads_edit_form(
	AxisDownloads                *downloads,
	HtmxRequest                  *request,
	const AxisDownloadClientInfo *client,
	const gchar                  *return_path
);

/* The button that reaches the client now ("Test it now"). */
HtmxNode *
axis_web_downloads_test_form(
	HtmxRequest *request,
	gint64       client_id,
	const gchar *label,
	const gchar *css_class,
	const gchar *return_path
);

/**
 * axis_web_downloads_map_form:
 * @volumes: (element-type AxisVolumeInfo): the volumes to choose from
 *
 * A client's "Add a mapping" form: its path, a volume, the path on it.
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_downloads_map_form(
	HtmxRequest *request,
	gint64       client_id,
	GPtrArray   *volumes,
	const gchar *return_path
);

G_END_DECLS

#endif /* AXIS_WEB_DOWNLOADS_H */
