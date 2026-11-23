/*
 * axis-indexer-sites.h - the kinds for three public torrent sites with a JSON
 * API of their own: YTS (movies), EZTV (TV) and The Pirate Bay
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each configured indexer of these kinds is one site at an address the
 * administrator can change (the sites move between domains from time to
 * time: the default is a constant here, shown on the pages, and Axis never
 * looks for a new one).  They take no account and no key.  A kind builds one
 * request to its site's own origin (a GET of a path, like every kind), reads
 * the answer with indexer/axis-indexer-sites-parse.h, and returns releases
 * that are an info-hash, a name and sizes: the magnet link is built by Axis,
 * and nothing is fetched from the site at grab time (the kind's fetch returns
 * the stored link).
 *
 *   YTS            movies, by IMDb identifier, else by title; a TV search
 *                  finds nothing
 *   EZTV           TV, by IMDb identifier only, up to five pages of a
 *                  hundred; a search without one is refused, saying so; a
 *                  movie search finds nothing
 *   The Pirate Bay a movie by its IMDb identifier, else (or when that finds
 *                  nothing) its title and year, in the movie categories; an
 *                  episode or a season by its title and SxxEyy or Sxx in the
 *                  TV categories; words as typed in Video.  Each result's
 *                  category is mapped to movies or TV (one outside video is
 *                  left out) and a VIP or trusted uploader is a flag
 *
 * A request waits for the site's budget first, as every kind's does: one
 * request every two seconds unless the setting
 * %AXIS_INDEXER_SETTING_SITE_RATE says otherwise, and the limiter's back-off
 * after failures.  A site that does not answer, answers with an error or with
 * something that is not its list (a parking page where it used to be) is
 * shown as failing, and the other indexers are asked as before.
 */

#ifndef AXIS_INDEXER_SITES_H
#define AXIS_INDEXER_SITES_H

#include "indexer/axis-indexer-kind.h"

G_BEGIN_DECLS

#define AXIS_INDEXER_YTS_NAME        "yts"
#define AXIS_INDEXER_EZTV_NAME       "eztv"
#define AXIS_INDEXER_PIRATEBAY_NAME  "piratebay"

/* The addresses the sites were at when this was written: believed, not
 * verified (tests/fixtures/sites/README.md). */
#define AXIS_INDEXER_YTS_ADDRESS        "https://yts.gg"
#define AXIS_INDEXER_EZTV_ADDRESS       "https://eztvx.to"
#define AXIS_INDEXER_PIRATEBAY_ADDRESS  "https://apibay.org"

/* The kinds, for axis_indexers_add_kind(). */
const AxisIndexerKindInfo *
axis_indexer_yts_kind(void);

const AxisIndexerKindInfo *
axis_indexer_eztv_kind(void);

const AxisIndexerKindInfo *
axis_indexer_piratebay_kind(void);

G_END_DECLS

#endif /* AXIS_INDEXER_SITES_H */
