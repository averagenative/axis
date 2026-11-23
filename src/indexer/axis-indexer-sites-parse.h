/*
 * axis-indexer-sites-parse.h - the answers of three public torrent sites with
 * a JSON API of their own (YTS, EZTV, The Pirate Bay), read into the indexer
 * layer's words; the requests made to them; the magnet links built from what
 * they list
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These sites need no account.  What is known of their answers is written
 * from their public, documented shapes and is believed, not verified: no site
 * was asked (tests/fixtures/sites/README.md lists every detail).  Every field
 * is optional and checked: a number may arrive as text (apibay sends all of
 * them so, EZTV some), a number out of range, a date that cannot be read, an
 * identifier that is not one is left out; an item without a name or without a
 * 40-hex-digit info-hash is skipped, one that lists the site's "nothing
 * found" placeholder (a hash of zeros) is not a result.  An answer that is not
 * the JSON value its site gives (an object, for YTS and EZTV; a list, for The
 * Pirate Bay) is refused whole, as is one the site says is an error.  Names
 * are cleaned and cut; lists are cut at %AXIS_INDEXER_MAX_RESULTS and no more
 * than %AXIS_INDEXER_MAX_SCAN are looked at.
 *
 * A release from these sites is an info-hash, a name and sizes, with The
 * Pirate Bay's category (mapped to Newznab's numbers: a result outside video
 * is left out) and its uploader's VIP or trusted status as a flag.  Nothing
 * else a site says is believed (an IMDb identifier of The Pirate Bay's is
 * never read into a release: it is whatever the uploader typed, though the
 * site can be asked for it), and nothing is fetched from a site at grab time:
 * Axis builds the magnet link itself, from the hash, the name and the public
 * trackers listed in axis_sites_trackers(), and has the downloads layer's
 * validator accept it before it is kept.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-indexer-sites.c.  The kinds,
 * which make the requests, are indexer/axis-indexer-sites.h.
 */

#ifndef AXIS_INDEXER_SITES_PARSE_H
#define AXIS_INDEXER_SITES_PARSE_H

#include <glib.h>

#include "indexer/axis-indexer-model.h"

G_BEGIN_DECLS

/* The most results YTS and EZTV are asked for in one request. */
#define AXIS_SITES_YTS_LIMIT   50
#define AXIS_SITES_EZTV_LIMIT  100

/* The most pages of EZTV's list one search reads. */
#define AXIS_SITES_EZTV_MAX_PAGES 5

/* The longest name kept in a magnet link's "dn", in characters. */
#define AXIS_SITES_DN_MAX_CHARS 200

/* ── Magnet links ───────────────────────────────────────────────── */

/**
 * axis_sites_trackers:
 *
 * The public trackers every magnet link built here names, in order: the one
 * place that lists them.  A %NULL-ended vector of announce addresses,
 * static.
 */
const gchar *const *
axis_sites_trackers(void);

/**
 * axis_sites_hash_read:
 * @text: (nullable): what a site said is an info-hash
 * @hash: (out) (optional) (transfer full): it, in lower case
 *
 * Returns: %TRUE for 40 hexadecimal digits that are not all zeros (the
 *   placeholder The Pirate Bay lists when nothing was found)
 */
gboolean
axis_sites_hash_read(
	const gchar  *text,
	gchar       **hash
);

/**
 * axis_sites_magnet:
 * @hash: 40 lower-case hex digits
 * @name: (nullable): the release's name, for the link's "dn"
 *
 * "magnet:?xt=urn:btih:HASH&dn=NAME&tr=...", with the name and the trackers
 * percent-encoded, accepted by axis_download_magnet_parse() (a name too long
 * for a link is cut, and is left out when even that does not fit).
 *
 * Returns: (nullable): the link, or %NULL when @hash is not one
 */
gchar *
axis_sites_magnet(
	const gchar *hash,
	const gchar *name
);

/* ── Requests ───────────────────────────────────────────────────── */

/**
 * axis_sites_yts_path:
 * @request: a checked request
 * @page: 1 and up
 *
 * "/api/v2/list_movies.json?query_term=tt0600001&limit=50&page=1": by the
 * IMDb identifier when the request has one, else by the title.  %NULL when
 * the request has neither, or is not for a movie or words.
 */
gchar *
axis_sites_yts_path(
	const AxisIndexerRequest *request,
	guint                     page
);

/**
 * axis_sites_eztv_path:
 *
 * "/api/get-torrents?imdb_id=700001&limit=100&page=1": the IMDb identifier
 * as digits without "tt" and without leading zeros.  %NULL when the request
 * has no IMDb identifier, or is not for TV.
 */
gchar *
axis_sites_eztv_path(
	const AxisIndexerRequest *request,
	guint                     page
);

/* The Pirate Bay's categories asked for a movie (Movies, DVD, HD, 3D, UHD),
 * for TV (TV shows, HD, UHD) and for typed words (all of Video). */
#define AXIS_SITES_TPB_MOVIE_CATEGORIES "201,202,207,209,211"
#define AXIS_SITES_TPB_TV_CATEGORIES    "205,208,212"
#define AXIS_SITES_TPB_VIDEO_CATEGORY   "200"

/**
 * axis_sites_tpb_path:
 *
 * "/q.php?q=Hearth%201995&cat=201,202,207,209,211": the words as a person would
 * type them ("Hearth 1995", "The Show S01E02", "The Show S01") with the
 * categories of what is looked for (all of Video for words as typed).  %NULL
 * when the request has no title.
 */
gchar *
axis_sites_tpb_path(const AxisIndexerRequest *request);

/**
 * axis_sites_tpb_imdb_path:
 *
 * "/q.php?q=tt0600001&cat=201,202,207,209,211": a movie by its IMDb
 * identifier, which the site matches against what its uploads name.  %NULL
 * unless the request is a movie's with a valid identifier.
 */
gchar *
axis_sites_tpb_imdb_path(const AxisIndexerRequest *request);

/**
 * axis_sites_tpb_category:
 * @text: (nullable): The Pirate Bay's category of a result ("207")
 *
 * Its Newznab number: 2000 for movies (2040 HD, 2045 UHD, 2060 3D, 2070 DVD,
 * 2030 CAM), 5000 for TV (5040 HD, 5045 UHD), 3000 audio (music videos
 * too), 4000 applications, 1000 games, 6000 adult, 7000 e-books and comics,
 * 8000 other; 0 for text that is not one of the site's categories.
 */
gint
axis_sites_tpb_category(const gchar *text);

/* Whether a Newznab category is of a class that is plainly not video:
 * console games (1000), audio (3000), PC (4000) or books (7000). */
gboolean
axis_sites_category_not_video(gint category);

/* The latest-releases request of each site, for a test. */
gchar *
axis_sites_yts_latest_path(void);

gchar *
axis_sites_eztv_latest_path(void);

gchar *
axis_sites_tpb_latest_path(void);

/* ── Answers ────────────────────────────────────────────────────── */

/**
 * AxisSitesRead:
 * @n_entries: how many entries the answer listed (kept or not): EZTV is read
 *   page by page until a page has fewer than the limit
 * @total: how many the site says there are, -1 when it does not say
 * @n_not_video: results left out because their category is not video
 *
 * What one answer held besides its releases.
 */
typedef struct
{
	guint  n_entries;
	gint64 total;
	guint  n_not_video;
} AxisSitesRead;

/**
 * axis_sites_parse_yts:
 * @data: the answer
 * @length: its length in bytes
 * @now_us: the current time, for refusing a date far in the future; 0 for no
 *   check
 * @seen: (nullable): info-hashes already in @outcome (a hash table of
 *   strings), added to: a hash listed twice is kept once
 * @outcome: (inout): initialised by the caller; the releases (one for each
 *   torrent of each movie, named "Title (Year) [Quality] [Type] [Codec]
 *   [YTS]") are added, and the counts of those left out
 * @read: (out) (optional)
 * @problem: (out) (optional) (transfer full): why the answer was refused, a
 *   sentence fragment holding none of it
 *
 * Returns: %FALSE for an answer that is not a YTS answer (nothing was added)
 */
gboolean
axis_sites_parse_yts(
	const gchar         *data,
	gsize                length,
	gint64               now_us,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
);

/* EZTV's: one release for each torrent, named by its "title" else its
 * "filename". */
gboolean
axis_sites_parse_eztv(
	const gchar         *data,
	gsize                length,
	gint64               now_us,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
);

/* The Pirate Bay's: one release for each item, named by its "name", with its
 * category and, for a VIP or trusted uploader, the flag "VIP" or "Trusted";
 * the placeholder item (a hash of zeros) is no result, and an item whose
 * category is not video is left out (counted in @read). */
gboolean
axis_sites_parse_tpb(
	const gchar         *data,
	gsize                length,
	gint64               now_us,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
);

/* The name a YTS torrent is given, from its fields: "Hearth (1995) [1080p]
 * [BluRay] [x264] [YTS]".  Each of @quality, @type, @codec (nullable) is
 * left out when it is not a plain word; @year 0 leaves out the year. */
gchar *
axis_sites_yts_name(
	const gchar *title,
	gint         year,
	const gchar *quality,
	const gchar *type,
	const gchar *codec
);

G_END_DECLS

#endif /* AXIS_INDEXER_SITES_PARSE_H */
