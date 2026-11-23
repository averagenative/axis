/*
 * axis-indexer-newznab.h - the native kinds: an indexer that speaks Newznab
 * (Usenet) or Torznab (torrents), asked by Axis itself
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each configured indexer of these kinds is one indexer: its address and
 * base path (<address><base>/api is its endpoint), its key, its categories
 * for movies and for TV.  The kind reads its capabilities (t=caps: at a
 * test, when never read, and daily by the job "indexers.caps") and decides
 * from them how a search is asked: by an identifier where the indexer takes
 * one for that kind of search, by words otherwise (and only by words when
 * its capabilities could not be read).  A test also asks for what is newest,
 * which proves the key and shows where the indexer serves its files: an
 * origin that is not its own is shown, to accept or not, and nothing is
 * fetched from it until it is accepted.
 *
 * The indexer's error documents (<error code description>, status 200) are
 * read as what they say: 100 to 199 the key or the account
 * (%AXIS_INDEXER_ERROR_AUTH), 500 its request limit and 501 its download
 * limit (%AXIS_INDEXER_ERROR_RATE_LIMITED, and the indexer is held: not
 * asked again for an hour, or for files), others as refused or failing.
 * Its allowances (<newznab:apilimits>) are reported to the layer whenever
 * an answer states them.
 *
 * Everything an answer says is read by indexer/axis-indexer-newznab-parse.h.
 */

#ifndef AXIS_INDEXER_NEWZNAB_H
#define AXIS_INDEXER_NEWZNAB_H

#include "indexer/axis-indexer-kind.h"

G_BEGIN_DECLS

#define AXIS_INDEXER_NEWZNAB_NAME "newznab"
#define AXIS_INDEXER_TORZNAB_NAME "torznab"

/* How long an indexer that said its limit is reached is held. */
#define AXIS_INDEXER_NEWZNAB_HOLD_S 3600

/* How many of the newest releases a test asks for. */
#define AXIS_INDEXER_NEWZNAB_TEST_LIMIT 10

/* The kinds, for axis_indexers_add_kind(). */
const AxisIndexerKindInfo *
axis_indexer_newznab_kind(void);

const AxisIndexerKindInfo *
axis_indexer_torznab_kind(void);

G_END_DECLS

#endif /* AXIS_INDEXER_NEWZNAB_H */
