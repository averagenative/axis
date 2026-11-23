/*
 * axis-web-meta.h - the metadata module: its jobs, its settings, the
 * Metadata page (System, administrators), the artwork route, and the title
 * search and title pages everyone logged in uses
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /metadata          providers (key set or not, state, limit, last
 *                           problem, attribution), what is kept, the
 *                           refresh, and a test lookup (?q=&kind=&year=
 *                           &provider=) that shows the result cards
 *   POST /metadata/refresh  queue a refresh now
 *   GET  /art/:hash         a stored image, to anyone logged in: the hash
 *                           is checked (64 lower-case hex digits) and is
 *                           the only thing the file is found by; the
 *                           stored type, nosniff, cached for a year
 *                           (the name is the content)
 *
 *   GET  /search            films and series by name (?q=&kind=all|film|
 *                           series&page=1..3), a limited number of times a
 *                           minute for each user (web/axis-web-search.c);
 *                           the box for it is in the top bar of every page
 *   GET  /search/poster     one poster for the card waiting for it (htmx)
 *   GET  /title/:id         a film or series by the store's own number, and
 *                           for a series its seasons and episodes
 *   GET  /title/:id/season/:number   one season's episodes (htmx)
 *                           (web/axis-web-title.c)
 *
 * What a module adds to a title's page and to a result card (its actions)
 * is registered with axis_web_add_title_actions() (web/axis-web.h); with
 * none registered there is no such area.
 */

#ifndef AXIS_WEB_META_H
#define AXIS_WEB_META_H

#include "core/axis-module.h"
#include "meta/axis-meta.h"

G_BEGIN_DECLS

typedef struct _AxisWebMeta AxisWebMeta;

AxisWebMeta *
axis_web_meta_new(AxisMeta *meta);

/* Waits for the page work in flight (main loop only). */
void
axis_web_meta_stop(AxisWebMeta *self);

void
axis_web_meta_free(AxisWebMeta *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebMeta, axis_web_meta_free)

/* The module, with the settings of @self's providers.  Register it after
 * every provider has been added to the layer. */
const AxisModuleInfo *
axis_web_meta_get_module(AxisWebMeta *self);

G_END_DECLS

#endif /* AXIS_WEB_META_H */
