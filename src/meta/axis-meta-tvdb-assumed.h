/*
 * axis-meta-tvdb-assumed.h - what Axis assumes about TheTVDB's API version 4
 * where its public documentation is silent: TO CONFIRM IN THE LIVE SESSION
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything below was inferred, not read in TheTVDB's public
 * documentation; one session with a real key settles them (the live
 * session).  No other file holds
 * an assumption about TheTVDB: the provider (axis-meta-tvdb.c) and its parser
 * (axis-meta-tvdb-parse.c) take each from here, and the tests' stand-in
 * server (tests/test-meta-tvdb.c, tests/fixtures/tvdb/) answers as these say.
 * When the live session shows otherwise, change it here and in the fixtures.
 *
 * Read (not assumptions, for reference): the API's address
 * https://api4.thetvdb.com; POST /v4/login with {"apikey", "pin"} answering
 * data.token, valid a month; the bearer header; the endpoints' paths; the
 * episode orders' type strings; remoteIds as {id, type, sourceName}; an
 * episode's fields; three-letter language codes.
 */

#ifndef AXIS_META_TVDB_ASSUMED_H
#define AXIS_META_TVDB_ASSUMED_H

/* 1. The image host.  No host is named on the pages read; artwork answers
 * are assumed to carry full addresses on this origin, and an address on any
 * other host is dropped. */
#define AXIS_META_TVDB_IMAGE_ORIGIN "https://artworks.thetvdb.com"

/* 2. Thumbnails.  No smaller form of an image is assumed: every size Axis
 * asks for is the image's own address (AXIS_META_ART_POSTER_SMALL too). */
#define AXIS_META_TVDB_THUMBNAILS 0

/* 3. Remote ids' source names.  remoteIds[].sourceName for IMDb and TMDb
 * (no example on the pages read).  Compared exactly. */
#define AXIS_META_TVDB_REMOTE_IMDB "IMDB"
#define AXIS_META_TVDB_REMOTE_TMDB "TheMovieDB.com"

/* 4. /search/remoteid/{id} is read to take an IMDb id; whether it takes a
 * TMDb id is not stated, so Axis does not ask it one. */
#define AXIS_META_TVDB_REMOTEID_TAKES_TMDB 0

/* 5. The episodes of an order.  The language variant,
 * /series/{id}/episodes/{order}/{lang}?page=N, documents a 200 answer holding
 * only "series"; the episodes are assumed to be in data.episodes, as in the
 * variant without a language.  The page size is not documented and not
 * relied on: a page is followed by another while links.next is a non-null
 * string (its address is never requested), up to this many pages. */
#define AXIS_META_TVDB_EPISODES_PATH  "/v4/series/%s/episodes/%s/%s?page=%u"
#define AXIS_META_TVDB_EPISODE_PAGES  20

/* 6. Which orders a series has: assumed to be named by its extended record's
 * seasons, each with type.type (the order's type string). */
#define AXIS_META_TVDB_SEASON_TYPE_MEMBER "type"

/* 7. A series' state: status.name, assumed one of these words. */
#define AXIS_META_TVDB_STATUS_CONTINUING "Continuing"
#define AXIS_META_TVDB_STATUS_ENDED      "Ended"
#define AXIS_META_TVDB_STATUS_UPCOMING   "Upcoming"

/* 8. Answers and errors.  Every answer is assumed to be an object whose
 * "status" is "success" with the payload in "data"; any other status, even
 * in a 2xx answer, is an answer that cannot be used (its "message" is never
 * shown: it is the provider's text).  A refused key is assumed to be a 401
 * to the login (a 403 is taken the same way); a 401 to any other request
 * means the token is no longer good; a 429 is assumed for too many requests
 * (no limit is published) and takes the layer's pause. */
#define AXIS_META_TVDB_STATUS_SUCCESS "success"

/* 9. Search results: data[] of objects whose tvdb_id is a string of digits,
 * with translations and overviews as objects keyed by language, and
 * image_url a full address (as 1). */
#define AXIS_META_TVDB_SEARCH_ID_MEMBER "tvdb_id"

/* 10. Requests a second.  No limit is published and the terms forbid
 * excessive calls: four a second, two at once, is a guess on the safe side
 * (the setting metadata.tvdb_rate changes it). */
#define AXIS_META_TVDB_RATE  4.0
#define AXIS_META_TVDB_BURST 2

/* The assumptions a change of a series' numbering rests on (V3,
 * src/modules/tv/axis-tv-numbering.c); none is a constant read here, each
 * is a fact the matching relies on. */

/* 11. Air dates.  An episode's "aired" is the same calendar day TMDb gives
 * the episode (both the first broadcaster's own date, "YYYY-MM-DD"); an
 * episode TheTVDB dates a day apart from TMDb (another time zone) is not
 * matched by its date, and is matched only by its title or a file's name. */

/* 12. Titles.  An episode's "name" in the language variant is in the
 * language setting, as TMDb's is in its own: titles are compared as letters
 * and digits, case folded, so a title one of them gives in another language
 * matches nothing. */

/* 13. Running times.  An episode's "runtime" is in minutes, as TMDb's; a
 * match by air date alone needs both and within half of each other. */

/* 14. The aired order ("official") is the numbering release names and
 * indexers use (design.md section 5, inferred): a series changed to it is
 * searched for with its season and episode numbers. */

#endif /* AXIS_META_TVDB_ASSUMED_H */
