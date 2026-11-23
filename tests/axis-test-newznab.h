/*
 * axis-test-newznab.h - test support: a stand-in Newznab indexer, or a
 * stand-in Torznab feed, on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * It answers as the Newznab and Torznab specifications and their documented
 * answers say an indexer does (tests/fixtures/newznab/README.md: everything
 * here is believed, nothing recorded: no real indexer was asked), and it
 * records every request (through axis-test-plex.h).
 *
 *   GET <prefix>/api?t=caps&apikey=K          caps.xml (torznab-caps.xml)
 *   GET <prefix>/api?t=search|movie|tvsearch  a body the test set for that t,
 *                                             else the fixture: search-movie.xml
 *                                             for t=movie and t=search,
 *                                             search-tv.xml for t=tvsearch
 *                                             (torznab-search.xml for a feed)
 *   GET <prefix>/api?t=get&id=ID&apikey=K     a release's file, by its id:
 *                                             "nzb-..." an NZB, "html-..." a
 *                                             page that is not one, "gone-..."
 *                                             error 300, "limit-..." error 501
 *   GET /getnzb/ID.nzb...                     an NZB (the other address form
 *                                             indexers use); outside the prefix
 *   GET /dl/NAME/?...file=F                   a torrent file (Jackett's form,
 *                                             outside the feed's path); F
 *                                             "magnet" answers 302 to a magnet
 *
 * The key is taken in the query's apikey only, as the protocol has it: a
 * wrong or missing one is answered with <error code="100">, status 200.  A
 * key in a header (any header holding it) is counted as bad.
 *
 * The fixtures' @ORIGIN@ (the stand-in's origin), @BASE@ (its prefix), @KEY@
 * (its key) and @FILES@ (where its results say its files are: its own origin
 * unless a test set another) are put in.
 */

#ifndef AXIS_TEST_NEWZNAB_H
#define AXIS_TEST_NEWZNAB_H

#include <glib.h>

#include "axis-test-plex.h"

G_BEGIN_DECLS

typedef struct _AxisTestNewznab AxisTestNewznab;

typedef enum
{
	AXIS_TEST_NEWZNAB_NORMAL,
	AXIS_TEST_NEWZNAB_DOWN,		/* the head never comes */
	AXIS_TEST_NEWZNAB_BROKEN,	/* 500 */
	AXIS_TEST_NEWZNAB_LIMIT,	/* every API call: error 500, request limit reached */
	AXIS_TEST_NEWZNAB_NO_CAPS,	/* t=caps: error 203, function not available */
	AXIS_TEST_NEWZNAB_JUNK_CAPS	/* t=caps: a page that is not XML */
} AxisTestNewznabMode;

/* A stand-in on 127.0.0.1 taking @key, serving its API under @prefix ("" for
 * the top); @torznab: a Torznab feed (torrents), else a Newznab indexer. */
AxisTestNewznab *
axis_test_newznab_new(
	const gchar *key,
	gboolean     torznab,
	const gchar *prefix
);

void
axis_test_newznab_free(AxisTestNewznab *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_newznab_base(AxisTestNewznab *self);

AxisTestPlex *
axis_test_newznab_server(AxisTestNewznab *self);

void
axis_test_newznab_set_mode(
	AxisTestNewznab     *self,
	AxisTestNewznabMode  mode
);

/* The key it takes from now on. */
void
axis_test_newznab_set_key(
	AxisTestNewznab *self,
	const gchar     *key
);

/* Where its results say its files are (another stand-in's base); %NULL for
 * its own origin. */
void
axis_test_newznab_set_files(
	AxisTestNewznab *self,
	const gchar     *base
);

/* The answer of t=caps from now on (copied, with the substitutions);
 * %NULL for the fixture's. */
void
axis_test_newznab_set_caps(
	AxisTestNewznab *self,
	const gchar     *body
);

/**
 * axis_test_newznab_set_search:
 * @t: "search", "movie" or "tvsearch", or %NULL for every one
 * @body: the answer (copied; the substitutions are made when it is served)
 */
void
axis_test_newznab_set_search(
	AxisTestNewznab *self,
	const gchar     *t,
	const gchar     *body
);

/* The answer of the @index-th search request (0 the first) from now on, in
 * place of any set for its t; "@FAIL@" is a 500 with no feed. */
void
axis_test_newznab_set_search_page(
	AxisTestNewznab *self,
	guint            index,
	const gchar     *body
);

/* A search answer takes this long (the server is busy meanwhile). */
void
axis_test_newznab_set_delay(
	AxisTestNewznab *self,
	guint            milliseconds
);

/* A fixture of tests/fixtures/newznab, with the substitutions made. */
gchar *
axis_test_newznab_fixture(
	AxisTestNewznab *self,
	const gchar     *name
);

/**
 * axis_test_newznab_item:
 * @title, @guid: the item's
 * @size: its size, or -1 for none stated
 * @id: what its file's address asks for (see t=get above), or %NULL for no
 *   address
 *
 * One <item> of a Newznab answer, its file on @FILES@.
 */
gchar *
axis_test_newznab_item(
	AxisTestNewznab *self,
	const gchar     *title,
	const gchar     *guid,
	gint64           size,
	const gchar     *id
);

/* An RSS answer holding @items (concatenated <item>s) and, when @api_max is
 * not 0, an apilimits element with those numbers. */
gchar *
axis_test_newznab_feed(
	const gchar *items,
	gint         api_used,
	gint         api_max,
	gint         grab_used,
	gint         grab_max
);

/* ── What it received ───────────────────────────────────────────── */

/* Requests of any kind. */
guint
axis_test_newznab_n_requests(AxisTestNewznab *self);

/* Requests of /api with t=search, movie or tvsearch. */
guint
axis_test_newznab_n_searches(AxisTestNewznab *self);

/* Requests of /api with t=caps. */
guint
axis_test_newznab_n_caps(AxisTestNewznab *self);

/* Requests of a release's file (t=get, /getnzb/, /dl/). */
guint
axis_test_newznab_n_files(AxisTestNewznab *self);

/* The query of the @index-th request of /api (raw), or %NULL. */
gchar *
axis_test_newznab_query(
	AxisTestNewznab *self,
	guint            index
);

/* The query of the last search (raw), or %NULL. */
gchar *
axis_test_newznab_last_search(AxisTestNewznab *self);

/* Requests that carried a wrong or no key, or the key in a header, or used
 * another method than GET. */
guint
axis_test_newznab_n_bad(AxisTestNewznab *self);

G_END_DECLS

#endif /* AXIS_TEST_NEWZNAB_H */
