/*
 * axis-web-ready.h - what is needed to download, as the pages show it: the
 * dashboard's "Getting started" card, the notice at the top of a wanted
 * title's page and the note in the Add form
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The answer comes from axis_acquire_check_ready().  Only administrators are
 * given the links to where each thing is set up; everyone else reads the
 * plain statement and is told that an administrator can set it up.
 *
 * Register axis_web_ready_get_module() (a required module, "Getting
 * started") with an AxisWebReady as its user data: it adds the card to the
 * dashboard while anything is missing.  The films and series pages draw
 * the notice and the note themselves, from their own answer.
 *
 * Where the setup guide is (web/axis-web-setup.h), the card's links go to
 * the guide's step for each item, it offers the guide itself, and it leaves
 * out the items whose step an administrator said is not used.
 */

#ifndef AXIS_WEB_READY_H
#define AXIS_WEB_READY_H

#include <glib.h>
#include <htmx-glib.h>

#include "acquire/axis-acquire-ready.h"
#include "core/axis-module.h"

G_BEGIN_DECLS

#define AXIS_WEB_READY_MODULE_ID "getting-started"

typedef struct _AxisWebReady AxisWebReady;

/* The layers are referenced; @meta may be %NULL. */
AxisWebReady *
axis_web_ready_new(
	AxisMeta      *meta,
	AxisIndexers  *indexers,
	AxisDownloads *downloads,
	AxisStorage   *storage
);

void
axis_web_ready_free(AxisWebReady *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebReady, axis_web_ready_free)

const AxisModuleInfo *
axis_web_ready_get_module(void);

/**
 * axis_web_ready_checklist_new:
 * @ready: the answer
 * @admin: give the links to where each item is set up
 *
 * Returns: (transfer full): <ul class="ready-list">: every item asked about,
 *   done or needed, with its sentence
 */
HtmxNode *
axis_web_ready_checklist_new(
	const AxisReadiness *ready,
	gboolean             admin
);

/**
 * axis_web_ready_notice_new:
 * @ready: the answer
 * @kinds: %AXIS_READY_FILMS or %AXIS_READY_SERIES: what the title is
 * @noun: "film", "series"
 * @admin: with the links
 *
 * For the top of a wanted title's page: "Nothing can search for or
 * download this film yet: no indexer source is set up and no download
 * client is set up.", and when imports cannot write, that too.
 *
 * Returns: (transfer full) (nullable): the notice, or %NULL when nothing
 *   stands in the way
 */
HtmxNode *
axis_web_ready_notice_new(
	const AxisReadiness *ready,
	guint                kinds,
	const gchar         *noun,
	gboolean             admin
);

/**
 * axis_web_ready_note_new:
 *
 * For under the Add button (administrators only): adding works, but what
 * is missing is said with its links.
 *
 * Returns: (transfer full) (nullable): the note, or %NULL when nothing is
 *   missing
 */
HtmxNode *
axis_web_ready_note_new(
	const AxisReadiness *ready,
	guint                kinds,
	const gchar         *noun
);

G_END_DECLS

#endif /* AXIS_WEB_READY_H */
