/*
 * axis-web-meta-private.h - shared by the files of the metadata pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Metadata page (axis-web-meta.c), the title search (axis-web-search.c)
 * and the title pages (axis-web-title.c) are one module and share its state
 * and its way of leaving the main loop for the database and the network.
 */

#ifndef AXIS_WEB_META_PRIVATE_H
#define AXIS_WEB_META_PRIVATE_H

#include <htmx-glib.h>

#include "meta/axis-meta.h"
#include "web/axis-web.h"
#include "web/axis-web-meta.h"

G_BEGIN_DECLS

struct _AxisWebMeta
{
	AxisMeta   *meta;		/* owned reference */
	guint       pending;
	gboolean    stopping;
	GHashTable *searchers;		/* user id (a gint64 *) -> SearchBucket *: the rate limit */
	/* Provider name -> when (gint64 *, seconds since 1970) this daemon first
	 * saw its key refused; the attention source's own (its reading thread
	 * alone touches it, never two at once), made when first needed. */
	GHashTable *refused_since;
};

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_WEB_META_MAX_PENDING 16

typedef void (*AxisWebMetaWorkFunc)(AxisWebMeta *self, gpointer data);

typedef HtmxResponse *(*AxisWebMetaDoneFunc)(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop.  Returns the deferred response for the handler
 * to return, or at once a 503 when too much is waiting or the pages are
 * stopping (then neither runs).  @free_data is called on the main loop in
 * every case.
 */
HtmxResponse *
axis_web_meta_dispatch(
	AxisWebMeta         *self,
	HtmxRequest         *request,
	AxisWebMetaWorkFunc  work,
	AxisWebMetaDoneFunc  done,
	gpointer             data,
	GDestroyNotify       free_data
);

/* Whether a task now would be refused for want of room. */
gboolean
axis_web_meta_is_busy(AxisWebMeta *self);

/* The routes of the title search (axis-web-search.c) and of the title pages
 * (axis-web-title.c), added by the module's register_routes. */
void
axis_web_search_register(
	AxisWebMeta *self,
	AxisWeb     *web
);

void
axis_web_title_register(
	AxisWebMeta *self,
	AxisWeb     *web
);

void
axis_web_search_free(AxisWebMeta *self);

/* ── Pieces (axis-web-meta-view.c) ──────────────────────────────── */

/* Adds @child to @parent and drops the caller's reference. */
void
axis_web_meta_add(
	gpointer parent,
	gpointer child
);

/* A parameter of the query, kept only when it is short, valid text. */
gchar *
axis_web_meta_param(
	HtmxRequest *request,
	const gchar *name,
	gsize        max_bytes
);

/* The notice a provider's terms require, wherever its data is shown, with
 * the direct link its terms ask for (AxisMetaProviderInfo.attribution_url). */
HtmxNode *
axis_web_meta_attribution(const AxisMetaProviderInfo *info);

/* The link of a provider's notice (it must have an attribution_url): its
 * name, to its address, opened elsewhere. */
HtmxNode *
axis_web_meta_attribution_link(const AxisMetaProviderInfo *info);

/* Why a provider did not answer, as a sentence: its own words (the layer's
 * @problem already names it), or "<label> did not answer." */
gchar *
axis_web_meta_why_text(
	const gchar *label,
	const gchar *problem
);

/* "just now", "3 hours ago", "2 days ago". */
gchar *
axis_web_meta_age_text(gint64 us);

/* "2026-11-10 12:00:00 UTC, 3 days ago". */
gchar *
axis_web_meta_when_text(gint64 us);

/*
 * A poster's place: the image when @hash (the stored image's content hash)
 * is given, else a tonal placeholder with the title set in it.  A
 * placeholder whose poster the provider has loads it later, once, when it
 * is on screen (htmx; without it the placeholder simply stays).  @large:
 * the title page's size, else a card's.
 */
HtmxNode *
axis_web_meta_poster_new(
	const AxisMetaTitle *title,
	const gchar         *hash,
	gboolean             large,
	guint                attempt
);

/* The lazy poster of @provider's @ref as /search/poster asks for it. */
#define AXIS_WEB_META_POSTER_PATH      "/search/poster"
#define AXIS_WEB_META_POSTER_RETRIES   5

/*
 * One result card: the poster, the title (linking to its page), the kind
 * and year, and two lines of the overview.  @corner (nullable, transfer
 * full) is a module's action for it.  @admin adds what the Metadata page
 * wants: the identifiers and when the record was saved.  @href (nullable)
 * is where the title links (its title page when %NULL), @line (nullable)
 * the words above the name in place of the kind and year, and @extra
 * (nullable, transfer full) a module's line under the overview.
 */
HtmxNode *
axis_web_meta_card_new_full(
	const AxisMetaTitle *title,
	const gchar         *poster_hash,
	HtmxNode            *corner,
	gboolean             admin,
	const gchar         *href,
	const gchar         *line,
	HtmxNode            *extra
);

/* What an Add that came back to a list's card says there, naming it: "Added
 * Paper Lantern (2019) to Movies. ..." for @done "added", or "managed";
 * %NULL for another word.  Transfer full. */
gchar *
axis_web_meta_added_text(
	const gchar         *done,
	const AxisMetaTitle *title
);

/* The same, linking to the title page, with no extra line. */
HtmxNode *
axis_web_meta_card_new(
	const AxisMetaTitle *title,
	const gchar         *poster_hash,
	HtmxNode            *corner,
	gboolean             admin
);

/* The title's identifiers as links to the services that know them, where
 * the identifier is valid and the service is one Axis links to (they open
 * elsewhere, with rel="noreferrer noopener"); others are shown as text.
 * Returns: (transfer full) (nullable) */
HtmxNode *
axis_web_meta_id_links(const AxisMetaTitle *title);

G_END_DECLS

#endif /* AXIS_WEB_META_PRIVATE_H */
