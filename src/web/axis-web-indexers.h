/*
 * axis-web-indexers.h - the indexers module: the Indexers page (System,
 * administrators), the jobs that forget old searches and read the native
 * indexers' capabilities again, and the module's settings
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /indexers                    the indexers and sources, and adding one (admin)
 *   POST /indexers                    add one (admin)
 *   POST /indexers/search             a test search of them all, answered on the page (admin)
 *   GET  /indexers/:id                one: its state, what it can search by (or the
 *                                     indexers it reports), a test search, its settings (admin)
 *   POST /indexers/:id                save its settings (admin)
 *   POST /indexers/:id/test           reach it now and read what it can do (admin)
 *   POST /indexers/:id/search         a test search, answered on the page (admin)
 *   POST /indexers/:id/accept-files   accept the other address of files its test saw (admin)
 *   POST /indexers/:id/delete         forget it (admin)
 *   GET  /indexers/:id/check          "Check this indexer": the form (admin; one that runs a
 *                                     definition)
 *   POST /indexers/:id/check          one search by the words typed, shown step by step (admin)
 *   POST /indexers/definitions/:id/override         a built-in definition's override,
 *                                                   pasted or uploaded (admin)
 *   POST /indexers/definitions/:id/override/remove  back to the built-in copy (admin)
 *
 * When the module starts, the definitions Axis ships are written to the
 * database (axis_indexers_sync_shipped()), with the overrides an
 * administrator placed in the data directory's "definitions"
 * (axis_web_indexers_set_definitions_dir()).  The dashboard lists, under what
 * needs attention, every switched-on indexer that is not working, and one
 * whose pages no longer look as expected as such.
 *
 * A source's key is never put in a page: its field is a password field with
 * no value, which replaces the stored key only when something is typed in it;
 * the submitted value is used once and wiped.  A release's fetch reference
 * and magnet link are not available to the pages at all (the layer's
 * listings do not carry them).  Names from indexers are shown as text,
 * escaped; what was typed into the test search is not logged and not kept.
 * The test search is a POST (it makes outbound requests), so it carries the
 * CSRF token; a GET never contacts a source.
 *
 * A form shown in the setup guide (the functions below, given a return
 * path) posts to these same handlers and comes back to the guide
 * (web/axis-web-setup-path.h).
 */

#ifndef AXIS_WEB_INDEXERS_H
#define AXIS_WEB_INDEXERS_H

#include "core/axis-module.h"
#include "indexer/axis-indexer.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebIndexers AxisWebIndexers;

AxisWebIndexers *
axis_web_indexers_new(AxisIndexers *indexers);

/* Where an administrator may place overrides of the built-in definitions
 * ("<data directory>/definitions"; %NULL for nowhere), read when the module
 * starts. */
void
axis_web_indexers_set_definitions_dir(
	AxisWebIndexers *self,
	const gchar     *path
);

/* Waits for the page work in flight (main loop). */
void
axis_web_indexers_stop(AxisWebIndexers *self);

void
axis_web_indexers_free(AxisWebIndexers *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebIndexers, axis_web_indexers_free)

const AxisModuleInfo *
axis_web_indexers_get_module(void);

/* ── The forms, wherever they are shown ────────────────────────── */

/**
 * axis_web_indexers_add_form:
 * @kind_name: the kind of what is added ("newznab", "torznab", "prowlarr")
 * @return_path: (nullable): an address of the setup guide to come back to
 *
 * The Indexers page's form that adds an indexer or a source of that kind,
 * empty and switched on.
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_indexers_add_form(
	AxisIndexers *indexers,
	HtmxRequest  *request,
	const gchar  *kind_name,
	const gchar  *return_path
);

/* A source's own settings form, as its page has it (never its key); the
 * fields' ids are unique to the source. */
HtmxNode *
axis_web_indexers_edit_form(
	AxisIndexers                *indexers,
	HtmxRequest                 *request,
	const AxisIndexerSourceInfo *source,
	const gchar                 *return_path
);

/* The button that reaches the source now and reads its indexers (@label:
 * "Test it now"). */
HtmxNode *
axis_web_indexers_test_form(
	HtmxRequest *request,
	gint64       source_id,
	const gchar *label,
	const gchar *css_class,
	const gchar *return_path
);

G_END_DECLS

#endif /* AXIS_WEB_INDEXERS_H */
