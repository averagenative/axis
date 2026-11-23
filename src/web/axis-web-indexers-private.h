/*
 * axis-web-indexers-private.h - shared by the files of the indexers pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-web-indexers.c              the module: work off the main loop, the job
 *   axis-web-indexers-pages.c        the pages
 *   axis-web-indexers-definitions.c  the site definitions: installed ones,
 *                                    installing, one's page, removing
 */

#ifndef AXIS_WEB_INDEXERS_PRIVATE_H
#define AXIS_WEB_INDEXERS_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-notice.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_INDEXERS_MAX_PENDING 8

/* The test search shows this many of the releases it found. */
#define AXIS_WEB_INDEXERS_SHOWN       100

struct _AxisWebIndexers
{
	AxisIndexers *indexers;	/* owned reference */
	guint         pending;	/* (main loop) dispatched, not yet answered */
	gboolean      stopping;	/* (main loop) */
	gchar        *definitions_dir;	/* nullable: overrides placed by an administrator */
};

typedef void (*AxisWebIndexersWorkFunc)(AxisWebIndexers *self, gpointer data);

typedef HtmxResponse *(*AxisWebIndexersDoneFunc)(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop.  Returns the deferred response for the handler
 * to return, or at once a 503 when too much is waiting or the pages are
 * stopping (then neither runs).  @free_data is called on the main loop in
 * every case.
 */
HtmxResponse *
axis_web_indexers_dispatch(
	AxisWebIndexers         *self,
	HtmxRequest             *request,
	AxisWebIndexersWorkFunc  work,
	AxisWebIndexersDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
);

void
axis_web_indexers_register_pages(
	AxisWebIndexers *self,
	AxisWeb         *web
);

/* ── axis-web-indexers-definitions.c ─────────────────────────────── */

/* The Indexers page's card of installed definitions (element-type
 * AxisIndexerDefinitionInfo, nullable), with the form that installs one.  Its
 * id is "definitions": a notice for that place goes at its top. */
HtmxNode *
axis_web_indexers_definitions_card(
	HtmxRequest   *request,
	GPtrArray     *definitions,
	AxisWebNotice *notice
);

/* The routes of a definition's page, installing and removing. */
/* "Check this indexer" (axis-web-indexers-check.c). */
void
axis_web_indexers_register_check(
	AxisWebIndexers *self,
	AxisWeb         *web
);

/* The form that adds an indexer running @info (axis-web-indexers-
 * definitions.c); @prefix makes its fields' ids unique on a page. */
HtmxNode *
axis_web_indexers_definition_add_form(
	HtmxRequest                     *request,
	const AxisIndexerDefinitionInfo *info,
	const gchar                     *prefix
);

void
axis_web_indexers_register_definitions(
	AxisWebIndexers *self,
	AxisWeb         *web
);

/* ── axis-web-indexers-jackett.c ─────────────────────────────────── */

/* A ticked file of Jackett's is the form field "pick-<its name>" with the
 * value "yes"; the installed list's ticks and Update buttons belong to the
 * form of this id (their "form" attribute), which is in the section below. */
#define AXIS_WEB_INDEXERS_JACKETT_PICK        "pick-"
#define AXIS_WEB_INDEXERS_JACKETT_UPDATE_FORM "jackett-update"

/* "Fetch definitions from Jackett" for the definitions' card: what it does,
 * the licence, the button that asks for the list; with @any_fetched, the
 * check for newer versions and the form that updates the ticked ones. */
HtmxNode *
axis_web_indexers_jackett_section(
	HtmxRequest *request,
	gboolean     any_fetched
);

/* Where Jackett's definitions are and the licence they are under, with the
 * links. */
HtmxNode *
axis_web_indexers_jackett_licence(void);

/* The routes of the page, the asking, the fetch and putting back.  Before
 * the definitions' own routes: "/indexers/definitions/jackett" is not a
 * definition's page. */
void
axis_web_indexers_register_jackett(
	AxisWebIndexers *self,
	AxisWeb         *web
);

/* ── axis-web-indexers-pages.c ───────────────────────────────────── */

/* The Indexers page with a notice (a refusal: @status 400), for the
 * definitions' handlers. */
HtmxResponse *
axis_web_indexers_list_response(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	GPtrArray       *sources,
	GPtrArray       *definitions,
	const gchar     *notice,
	gboolean         notice_error,
	guint            status
);

G_END_DECLS

#endif /* AXIS_WEB_INDEXERS_PRIVATE_H */
