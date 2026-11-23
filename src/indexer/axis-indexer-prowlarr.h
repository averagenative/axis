/*
 * axis-indexer-prowlarr.h - the Prowlarr kind of indexer source
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Prowlarr (https://prowlarr.com) searches every indexer configured in it
 * with one request.  This kind speaks its API v1 with GET only, the key in
 * the X-Api-Key header:
 *
 *   GET /api/v1/system/status       its version (the test)
 *   GET /api/v1/indexer             the indexers, with protocol and
 *                                   capabilities
 *   GET /api/v1/indexerstatus       the indexers that are failing now
 *   GET /api/v1/search?query=..&type=..&categories=..&indexerIds=..&limit=..
 *                                   releases, a JSON array
 *
 * and fetches a release from the "downloadUrl" Prowlarr gave for it (a
 * path on Prowlarr itself, which holds Prowlarr's key as a parameter), or
 * takes its "magnetUrl".  Prowlarr's own download address may answer with
 * the file or with a redirect to a magnet link.
 *
 * What is known of the API is its published documentation and memory, not a
 * recording: tests/fixtures/prowlarr/README.md lists what is believed and not
 * verified, and the first run against a real Prowlarr is expected to find
 * corrections.  The JSON is read by indexer/axis-indexer-prowlarr-parse.h.
 */

#ifndef AXIS_INDEXER_PROWLARR_H
#define AXIS_INDEXER_PROWLARR_H

#include "indexer/axis-indexer-kind.h"

G_BEGIN_DECLS

#define AXIS_INDEXER_PROWLARR_NAME "prowlarr"

/* The kind, for axis_indexers_add_kind(). */
const AxisIndexerKindInfo *
axis_indexer_prowlarr_kind(void);

G_END_DECLS

#endif /* AXIS_INDEXER_PROWLARR_H */
