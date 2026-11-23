/*
 * axis-library-hand.h - what a person did by hand to a library file's link,
 * shared by the modules' links pages (add-force-grab-and-assign, slices C1
 * and C2)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The mark of a file a person unlinked by hand (library_hand_unlink,
 * 0063_hand_unlink.sql), and the sentence that says how a file's link was
 * made.  The mark is the file's, whichever title it was unlinked from.
 */

#ifndef AXIS_LIBRARY_HAND_H
#define AXIS_LIBRARY_HAND_H

#include <glib.h>

#include "db/axis-db-conn.h"

G_BEGIN_DECLS

/* How a file's link was made, or why it has none: what the sentence is made
 * from. */
typedef struct
{
	const gchar *other;		/* (nullable) the other title it is linked to */
	gboolean     linked;		/* to the title of the page */
	gboolean     unlinked_hand;
	const gchar *unlinked_by;
	const gchar *unlinked_on;	/* "2026-11-20" */
	const gchar *hand_how;		/* (nullable) "forced", "assigned", "linked", "brought" */
	const gchar *brought_from;	/* (nullable) for "brought": the app's name ("Sonarr"); %NULL: Radarr */
	const gchar *hand_by;
	const gchar *hand_on;
	gboolean     by_import;
	gboolean     by_identity;
	gboolean     kept;
	gboolean     kept_brought;	/* with @kept: kept because it was brought over (kept_how), by no person's choice */
} AxisLibraryHandHow;

/* "Linked by hand by root on 2026-11-20; kept by hand", "Brought over from
 * Radarr by root on 2026-11-21; Axis does not replace it", "No link", ... */
gchar *
axis_library_hand_how(const AxisLibraryHandHow *how);

/* On @conn, in the caller's transaction: @file_id was unlinked by hand from
 * @owner_id of @module by @by (nullable). */
gboolean
axis_library_hand_unlink_mark(
	AxisDbConn   *conn,
	gint64        file_id,
	const gchar  *module,
	gint64        owner_id,
	const gchar  *by,
	GError      **error
);

/* The mark of @file_id goes, whichever title it was made for. */
gboolean
axis_library_hand_unlink_clear(
	AxisDbConn   *conn,
	gint64        file_id,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_LIBRARY_HAND_H */
