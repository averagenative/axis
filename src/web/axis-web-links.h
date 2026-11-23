/*
 * axis-web-links.h - the pieces the modules' links pages share
 * (add-force-grab-and-assign, slices C1 and C2)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A links page posts its plan as one field for each file (a prefix and the
 * library file's id) and guards its change with a digest.  The pages are the
 * modules' own (src/modules/tv/axis-tv-web-links.c,
 * src/modules/movies/axis-movies-web-links.c); what both need to read such a
 * form and to draw a tick is here.
 */

#ifndef AXIS_WEB_LINKS_H
#define AXIS_WEB_LINKS_H

#include <glib.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/* The most files one plan may name. */
#define AXIS_WEB_LINKS_PLAN_MAX 500

/* The field names: to_<file>, unlink_<file>, other_<file>. */
#define AXIS_WEB_LINKS_TO     "to_"
#define AXIS_WEB_LINKS_UNLINK "unlink_"
#define AXIS_WEB_LINKS_OTHER  "other_"

/* Whether @key is @prefix and a library file's id; *@file_id is it. */
gboolean
axis_web_links_field_file(
	const gchar *key,
	const gchar *prefix,
	gint64      *file_id
);

/* Whether @digest has the form of one (32 lower-case hex digits). */
gboolean
axis_web_links_digest_valid(const gchar *digest);

/* A checkbox that posts "yes", ticked when @checked. */
HtmxNode *
axis_web_links_tick_box(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
);

G_END_DECLS

#endif /* AXIS_WEB_LINKS_H */
