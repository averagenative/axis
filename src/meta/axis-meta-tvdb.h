/*
 * axis-meta-tvdb.h - TheTVDB (API version 4) as a metadata provider for
 * series (add-metadata-tvdb)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Needs the administrator's own TheTVDB API key (Settings, Movie and TV
 * details), and the subscriber PIN for a user-supported key only.  The key
 * (and PIN) travel only in the body of POST /v4/login to
 * https://api4.thetvdb.com (core/axis-http.h, the pinned login); the token
 * that login returns is kept in memory by the metadata layer, sent only as
 * "Authorization: Bearer <token>" to that origin, and renewed after 25 days
 * or on a 401.  A refused login stops every request until the key or the PIN
 * changes.  Posters come from TheTVDB's image origin
 * (meta/axis-meta-tvdb-assumed.h), without the token.  Both origins are
 * constants: tests copy the provider's info and point the copy at fake
 * servers on loopback.
 *
 * It is a secondary provider: never the provider of a kind (TMDb stays the
 * one searched and identified by), asked only where a caller names it, and
 * offered nowhere without its key.  Series only: a film is not asked about.
 *
 * Calls, as few as can be:
 *   log in       POST /v4/login
 *   search       /v4/search?query=&type=series&limit=&offset=[&year=]
 *   a series     /v4/series/{id}/extended?meta=translations&short=true
 *   by an id     /v4/search/remoteid/{imdb id}; a TVDB id is the series itself
 *   an order     /v4/series/{id}/episodes/{order}/{language}?page=N, every page
 *   the test     /v4/seasons/types
 *
 * Settings (metadata.tvdb_*): key (secret), pin (secret, optional), rate
 * (four a second), language (eng).  What the answers become:
 * axis-meta-tvdb-parse.h.
 */

#ifndef AXIS_META_TVDB_H
#define AXIS_META_TVDB_H

#include <glib.h>

#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

#define AXIS_META_TVDB_API_ORIGIN "https://api4.thetvdb.com"

#define AXIS_META_TVDB_SETTING_KEY       "metadata.tvdb_key"
#define AXIS_META_TVDB_SETTING_PIN       "metadata.tvdb_pin"
#define AXIS_META_TVDB_SETTING_RATE      "metadata.tvdb_rate"
#define AXIS_META_TVDB_SETTING_LANGUAGE  "metadata.tvdb_language"

/* The notice TheTVDB asks for wherever its data is shown, and the address it
 * links to (read: thetvdb.com/api-information). */
#define AXIS_META_TVDB_ATTRIBUTION \
	"Metadata provided by TheTVDB. Please consider adding missing information or subscribing."
#define AXIS_META_TVDB_ATTRIBUTION_URL "https://thetvdb.com/subscribe"

/**
 * axis_meta_tvdb_get_info:
 *
 * Returns: (transfer none): the provider, to give to axis_meta_add_provider()
 *   after TMDb's; it lives as long as the program
 */
const AxisMetaProviderInfo *
axis_meta_tvdb_get_info(void);

G_END_DECLS

#endif /* AXIS_META_TVDB_H */
