/*
 * axis-web-block.h - Block and search again: a download a person does not
 * want, for every module that grabs (add-block-release)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module registers and gets these routes under its prefix, both an
 * administrator's:
 *
 *   GET  <prefix>/block-release?grab=|download=|import=
 *        the confirmation: what blocking this release would do.  It writes
 *        nothing.  The grab is named by its id, by the download it sent
 *        (the Downloads page) or by an import of that download (a file's
 *        row on the title's page).  With &done=cancelled|blocked: what was
 *        just done.
 *   POST <prefix>/block-release   (owner, grab, confirm=yes)
 *        Block and search again; then back to the page above.
 *
 * A grab that is open (its download is at a client, or waits for its
 * import) is given up on through the failure path, as "Give up on this
 * download" does, with the release on the blocklist under the person's
 * name.  A grab that is closed (its release is in the library, or it ended
 * some other way) is only put on the blocklist: no file, link, import or
 * client is touched, and the page says so before and after.
 */

#ifndef AXIS_WEB_BLOCK_H
#define AXIS_WEB_BLOCK_H

#include <glib.h>
#include <htmx-glib.h>

#include "acquire/axis-acquire.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

/**
 * AxisWebBlockRead:
 * @title: the owner's label ("Lantern Bay (2025)")
 * @back: the owner's page (a path on this site)
 * @blocklist: where its blocklist is listed (a path with its anchor)
 * @grab: the grab
 *
 * What a module reads for the page.
 */
typedef struct
{
	gchar                *title;
	gchar                *back;
	gchar                *blocklist;
	AxisAcquireBlockGrab *grab;
} AxisWebBlockRead;

/**
 * AxisWebBlockInfo:
 * @prefix: the module's routes' prefix ("/series", "/films")
 * @noun: what an owner is ("series", "movie")
 * @hold, @release, @status: as AxisWebManualInfo's
 * @find: (worker thread) the grab named by the first of @grab_id,
 *   @download_id and @import_id that is not 0, with its owner; an error in
 *   the module's words when there is none
 * @block: (worker thread) Block and search again (axis_acquire_block_grab())
 */
typedef struct
{
	const gchar *prefix;
	const gchar *noun;
	gboolean (*hold)(gpointer data);
	void     (*release)(gpointer data);
	guint    (*status)(const GError *error);
	gboolean (*find)(gpointer data, gint64 grab_id, gint64 download_id, gint64 import_id, AxisWebBlockRead *read,
	                 GError **error);
	gboolean (*block)(gpointer data, gint64 owner_id, gint64 grab_id, const gchar *user, gboolean *cancelled,
	                  GError **error);
} AxisWebBlockInfo;

/* Registers the routes (from the module's register_routes); @info must
 * outlive the shell, @data is the module's. */
void
axis_web_block_register(
	AxisWeb                *web,
	const AxisWebBlockInfo *info,
	gpointer                data
);

/* "<prefix>/block-release?<by>=<id>": @by is "grab", "download" or
 * "import". */
gchar *
axis_web_block_path(
	const gchar *prefix,
	const gchar *by,
	gint64       id
);

G_END_DECLS

#endif /* AXIS_WEB_BLOCK_H */
