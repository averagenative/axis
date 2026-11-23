/*
 * axis-web-browser.h - the Library pages: volumes and roots, folders, the
 * filtered file list, and the actions that ask for scans and probes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_browser_get_module() after the probe module (it
 * depends on the storage, library and probe modules), passing the
 * AxisWebBrowser as its user data.  The module "library-ui" runs no jobs;
 * it only reads the index and asks for work.
 *
 * Pages (every logged-in user may look; the actions are administrators'
 * and every POST needs the CSRF token):
 *
 *   GET  /library                    volumes and their roots: state, usage,
 *                                    files, size, last complete scan, scan
 *                                    progress
 *   GET  /library/list               that list alone (it refreshes itself)
 *   GET  /library/root/:id           a root's top folder
 *   GET  /library/folder/:id         a folder: breadcrumbs, subfolders and
 *                                    files, with totals as of the last
 *                                    complete scan; ?q= filters the
 *                                    subfolders by name, and under a films
 *                                    or series root ?view= shows them as
 *                                    posters, an overview or a table, each
 *                                    with the title it holds
 *                                    (axis_web_browser_set_folder_titles())
 *   GET  /library/files              every file, filtered, sorted and paged
 *                                    (the query is parsed strictly: an
 *                                    unknown filter or value is a 400); a
 *                                    target and outcome are decided by the
 *                                    estimator (axis_web_browser_set_estimator()),
 *                                    and answered 404 without it
 *   GET  /library/files/changed      the "list changed, reload" notice
 *   POST /library/root/:id/scan      asks for a scan of the root
 *   POST /library/folder/:id/probe   asks for the files under a folder to be
 *                                    probed again (a capped number)
 *
 * The file's own page, and "probe this file again", are the probe module's
 * (/library/file/:id).  A folder is addressed by its row id, a list's
 * cursor is a row id: nothing from a file name is ever part of a URL, an
 * attribute or a log line; names are shown as text inside <bdi>, in the
 * display form of axis_library_name_display().
 *
 * Live: a page that follows the "library" topic hears when a scan starts,
 * progresses or ends and when a volume changes state; the list page hears
 * that a scan of the roots it lists completed and shows a notice offering
 * to reload.  A timer polls for that only while someone listens.
 */

#ifndef AXIS_WEB_BROWSER_H
#define AXIS_WEB_BROWSER_H

#include <glib.h>

#include "core/axis-module.h"
#include "library/axis-library.h"
#include "library/axis-library-browse.h"
#include "meta/axis-meta-model.h"
#include "probe/axis-probe.h"
#include "storage/axis-storage.h"
#include "transcode/axis-estimator.h"
#include "web/axis-web-hub.h"

G_BEGIN_DECLS

#define AXIS_WEB_BROWSER_MODULE_ID  "library-ui"

/* The hub topic the pages follow, and the events on it. */
#define AXIS_WEB_BROWSER_TOPIC         "library"
#define AXIS_WEB_BROWSER_EVENT_OOB     "oob"
#define AXIS_WEB_BROWSER_EVENT_REFRESH "library-refresh"
#define AXIS_WEB_BROWSER_EVENT_SCANNED "scan-done"

/* How often the live state is looked at while someone listens. */
#define AXIS_WEB_BROWSER_TICK_SECONDS  2

typedef struct _AxisWebBrowser AxisWebBrowser;

/**
 * axis_web_browser_new:
 * @library: the file index; referenced
 * @probe: the probe, for the probe requests; referenced
 * @storage: the volumes; referenced
 * @hub: (nullable): where live events go; not owned, and it must outlive
 *   the result
 *
 * Create on the main thread.
 */
AxisWebBrowser *
axis_web_browser_new(
	AxisLibrary *library,
	AxisProbe   *probe,
	AxisStorage *storage,
	AxisWebHub  *hub
);

/* Stops the live updates and waits (running the default main context) for
 * the page work in flight. */
void
axis_web_browser_stop(AxisWebBrowser *self);

/* Lets the file list evaluate the estimator's filters (target, outcome)
 * while the estimate module is on; main thread, before serving.
 * @estimator is referenced; %NULL removes it. */
void
axis_web_browser_set_estimator(
	AxisWebBrowser *self,
	AxisEstimator  *estimator
);

/* ── What a folder is ───────────────────────────────────────────── */

/**
 * AxisWebFolderTitle:
 * @dir_id: the folder (one of those asked about)
 * @title: (nullable) (transfer full): the title the folder holds, a brief
 *   record (id, kind, provider, title, year, poster_ref), %NULL when it is
 *   not identified
 * @poster: (nullable) (transfer full): that title's kept poster's hash
 * @unit_id: where it can be identified: its unit on the Identify page, 0
 *   for none
 * @not_title: a person said it holds no title (a film folder marked "not a
 *   film")
 *
 * A folder of a films or series root that holds a title, or would: the
 * Library lists it with its title, or as not identified.  A folder that is
 * not one (a season of a series, a film's extras) is not given.
 */
typedef struct
{
	gint64         dir_id;
	AxisMetaTitle *title;
	gchar         *poster;
	gint64         unit_id;
	gboolean       not_title;
} AxisWebFolderTitle;

void
axis_web_folder_title_free(AxisWebFolderTitle *item);

/**
 * AxisWebFolderTitlesFunc:
 * @user_data: what was set with it
 * @root_id: the root the folders are in
 * @dirs: (element-type AxisDirRecord): the folders, at most a page and one
 * @titles: (element-type AxisWebFolderTitle): what each is, added here;
 *   the free function is set
 * @error: (nullable): return location for an error
 *
 * Called on a worker thread, a few statements for the whole page.  A
 * failure leaves the folders listed by name alone.
 */
typedef gboolean (*AxisWebFolderTitlesFunc)(
	gpointer    user_data,
	gint64      root_id,
	GPtrArray  *dirs,
	GPtrArray  *titles,
	GError    **error
);

/**
 * axis_web_browser_set_folder_titles:
 * @type: the roots it tells about (films or series)
 * @module_id: the module that must be on for it to be asked
 * @func: (nullable): %NULL removes it
 * @user_data: for @func; not owned, and it must outlive the pages' work
 *   (axis_web_browser_stop())
 *
 * Lets the folder lists of @type's roots show what each folder is, with the
 * same three views as the title lists (src/web/axis-web-titles.h), the view
 * kept per account as "library-films" or "library-series".  Main thread,
 * before serving.
 */
void
axis_web_browser_set_folder_titles(
	AxisWebBrowser          *self,
	AxisMediaType            type,
	const gchar             *module_id,
	AxisWebFolderTitlesFunc  func,
	gpointer                 user_data
);

void
axis_web_browser_free(AxisWebBrowser *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebBrowser, axis_web_browser_free)

const AxisModuleInfo *
axis_web_browser_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_BROWSER_H */
