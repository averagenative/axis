/*
 * axis-web-discover.h - the Discover module: its pages, its job and its
 * settings
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /discover/movies             Discover movies: a list, its filters, a page of
 *   GET  /discover/tv                 it (list=, genre=, from=, to=, rating=, hide=,
 *                                     page=)
 *   GET  /discover/not-interested     the titles marked Not interested (page=)
 *   POST /discover/dismiss            mark a title Not interested (title=, back=)
 *   POST /discover/undismiss          show it again (title=, back=)
 *   POST /discover/refresh            fetch the lists now (back=)
 *   GET  /discover/like/:id           "More like this" of the title :id (the
 *                                     store's number): its card, for htmx;
 *                                     without script, back to the title's page
 *
 * Everyone logged in may look; the POSTs are administrators'.  The pages
 * read the stored lists (discover/axis-discover.h) and ask TMDb nothing.
 * The movies and TV modules draw each card's state or its one-step add
 * (AXIS_WEB_ACTION_DISCOVER).
 *
 * More like this (add-more-like-this): Discover is also a part of a title's
 * own page (/title/:id), where it draws a card of what TMDb recommends for
 * that title.  Fresh stored recommendations are drawn with the page; when
 * there are none the card loads /discover/like/:id by itself, and that route
 * alone asks TMDb: one request, for the one title, never with fresh data
 * stored.
 */

#ifndef AXIS_WEB_DISCOVER_H
#define AXIS_WEB_DISCOVER_H

#include <glib.h>

#include "core/axis-module.h"
#include "discover/axis-discover.h"

G_BEGIN_DECLS

typedef struct _AxisWebDiscover AxisWebDiscover;

/* Borrows a reference of @discover. */
AxisWebDiscover *
axis_web_discover_new(AxisDiscover *discover);

/* Waits for the pages' work in flight (main loop). */
void
axis_web_discover_stop(AxisWebDiscover *self);

void
axis_web_discover_free(AxisWebDiscover *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebDiscover, axis_web_discover_free)

/* The module: register it with the AxisWebDiscover as its user data, after
 * the metadata module (and after the movies and TV modules, whose pieces its
 * cards show). */
const AxisModuleInfo *
axis_web_discover_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_DISCOVER_H */
