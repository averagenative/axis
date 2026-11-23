/*
 * axis-indexer-cardigann.h - the kind of indexer that runs an installed
 * definition (the community's "Cardigann" format: indexer/def/axis-def.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each indexer of this kind runs one installed definition, against one
 * address: one of the definition's links (or old links) chosen when it was
 * added, or an address the administrator typed; a path in it is the base
 * path relative paths are joined to.  Like the public sites
 * (indexer/axis-indexer-sites.h) it takes no account and no key, is asked no
 * faster than the setting %AXIS_INDEXER_SETTING_SITE_RATE says (and, between
 * the requests of one search, no faster than the definition's own
 * requestDelay, up to %AXIS_CARDIGANN_MAX_DELAY_S), and every request is a GET
 * of a path on that one origin through the layer's connection: the engine
 * (indexer/def/axis-def-engine.h) builds no other, a redirect off the origin
 * is not followed, and a release whose download is elsewhere is left out.
 *
 * Its definition is read again and judged at every call; one that this
 * version of the engine cannot run (or that was replaced by one it cannot)
 * fails the call with the reason, and the indexer shows as failing.  What
 * it searches is what the definition's categories cover: a movie search of
 * a definition for TV only asks nothing, and finds nothing.  A search by an
 * IMDb identifier is made when the definition takes one; words otherwise.
 *
 * A release is its magnet link, or its info-hash (the link built from it),
 * or its download address on the origin; at grab time, a definition that
 * reads its download from the release's page has that page asked for, and
 * the link read from it under the same rules.  The torrent file is fetched
 * with the same on-origin GET as every other request, not the layer's
 * release fetch that may follow a download's redirect to an indexer's site:
 * a download that redirects off the origin is refused, nothing sent there
 * (one that redirects to a magnet link gives that link).
 *
 * Not built in this stage: logins, and anything behind a browser
 * challenge.  A definition with a login is "not usable yet: needs a login";
 * a site that answers with a challenge's page (Cloudflare's, DDoS-Guard's:
 * axis_def_is_challenge()), with a 403 whose first bytes are one, or with a
 * 403 or a 503 when its definition says it is known to challenge (an
 * info_flaresolverr setting), fails saying it is not usable from there and
 * needs a browser helper, and shows as failing.
 *
 * Drift (add-builtin-torrent-indexers): a search whose first answer is not
 * what the definition reads (the engine's %AXIS_DEF_ANSWER_DRIFT: "its pages
 * no longer look as expected"), or a browser challenge, fails in words that
 * say what to do, and holds the indexer: searches pass it over for
 * %AXIS_CARDIGANN_HOLD_S (axis_indexer_conn_hold_searches()), while a test or
 * a check still asks the site, and one that passes lifts the hold.  A check
 * (the layer's axis_indexers_check_source()) gives the connection a trace
 * (indexer/def/axis-def-trace.h), which every request and answer of the
 * search is recorded in.
 */

#ifndef AXIS_INDEXER_CARDIGANN_H
#define AXIS_INDEXER_CARDIGANN_H

#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-kind.h"

G_BEGIN_DECLS

#define AXIS_INDEXER_CARDIGANN_NAME "cardigann"

/* The longest wait between two requests of one search, in seconds,
 * whatever a definition's requestDelay says. */
#define AXIS_CARDIGANN_MAX_DELAY_S 10

/* How long searches pass over an indexer whose pages no longer look as
 * expected, or that answers with a browser challenge. */
#define AXIS_CARDIGANN_HOLD_S 3600

/* The words its state holds when its pages no longer look as expected
 * ("<name>'s pages no longer look as expected (...)"): what the dashboard
 * looks for. */
#define AXIS_CARDIGANN_DRIFT_WORDS AXIS_INDEXER_HOLD_DRIFT_WORDS

/* The most an answer may be. */
#define AXIS_CARDIGANN_ANSWER_BYTES (4 * 1024 * 1024)

/* The kind, for axis_indexers_add_kind(). */
const AxisIndexerKindInfo *
axis_indexer_cardigann_kind(void);

G_END_DECLS

#endif /* AXIS_INDEXER_CARDIGANN_H */
