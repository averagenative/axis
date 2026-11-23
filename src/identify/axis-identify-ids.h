/*
 * axis-identify-ids.h - provider identifiers in file and folder names, and in
 * the bytes of a sidecar .nfo file
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Names and .nfo files come from disk, written by anyone: hostile input.
 * Both readers look only for the few forms below, compare letters ignoring
 * ASCII case, take digits strictly (ASCII 0-9 only) and never guess.
 *
 * In a name, a token in the forms Plex, Jellyfin and Axis's own import write:
 *
 *   {tmdb-6003}  {tmdbid-6003}  [tmdb-6003]  [tmdbid-6003]  [tmdbid=6003]
 *   {imdb-tt0600003}   [imdbid-tt0600003]   (and the other mixes)
 *   {tvdb-82001}   [tvdbid-82001]            (a series': films ignore it)
 *
 * The bracket must close with its own kind; a TMDb or TVDB identifier is 1
 * to 12 digits not starting with 0, an IMDb one "tt" and 7 to 9 digits.  Anything
 * else is not a token and stays in the name.  The name is given back with
 * every token taken out, for the release parser.
 *
 * In an .nfo file, at most %AXIS_IDENTIFY_NFO_MAX bytes are looked at, as
 * bytes, never parsed as XML or as text:
 *
 *   IMDb  "tt" and 7 to 9 digits, with no letter or digit just before it
 *         and no digit or letter just after
 *   TMDb  "themoviedb.org/movie/<digits>" (not /tv/), "<tmdbid>digits<", and
 *         "<uniqueid ...type="tmdb"...>digits<" (quotes of either kind)
 *
 * Two different identifiers of one scheme in one name or one file are a
 * conflict: neither is believed.
 *
 * GLib only, no I/O: fuzzed by tests/fuzz/fuzz-identify-name.c and
 * tests/fuzz/fuzz-identify-nfo.c.
 */

#ifndef AXIS_IDENTIFY_IDS_H
#define AXIS_IDENTIFY_IDS_H

#include <glib.h>

G_BEGIN_DECLS

/* A name longer than this finds nothing (and is given back as it is). */
#define AXIS_IDENTIFY_NAME_MAX  1024

/* At most this many bytes of an .nfo file are read or scanned. */
#define AXIS_IDENTIFY_NFO_MAX   65536

/**
 * AxisIdentifyIds:
 * @tmdb: the TMDb identifier (digits), "" when none was found
 * @imdb: the IMDb identifier ("tt" and digits), "" when none was found
 * @tmdb_conflict: two different TMDb identifiers were found (@tmdb is "")
 * @imdb_conflict: two different IMDb identifiers were found (@imdb is "")
 * @tvdb: the TVDB identifier (digits, a series'), "" when none was found;
 *   only names carry one (an .nfo's is not read)
 * @tvdb_conflict: two different TVDB identifiers were found (@tvdb is "")
 */
typedef struct
{
	gchar    tmdb[16];
	gchar    imdb[16];
	gboolean tmdb_conflict;
	gboolean imdb_conflict;
	gchar    tvdb[16];
	gboolean tvdb_conflict;
} AxisIdentifyIds;

/* Empty. */
void
axis_identify_ids_init(AxisIdentifyIds *ids);

/* Whether @ids holds a TMDb or IMDb identifier that may be believed (a
 * film's evidence; a series' takes the TVDB one too). */
gboolean
axis_identify_ids_any(const AxisIdentifyIds *ids);

/* Whether @ids found a conflict of TMDb or IMDb identifiers (a series'
 * evidence looks at @tvdb_conflict too). */
gboolean
axis_identify_ids_conflict(const AxisIdentifyIds *ids);

/**
 * axis_identify_name_ids:
 * @name: (nullable): a file or folder name, valid UTF-8 or not
 * @length: its length in bytes, or -1 when it ends at a NUL
 * @ids: (out caller-allocates): what was found (initialised here)
 *
 * Returns: (transfer full): @name with every token taken out and the white
 *   space around it made one space, trimmed; "" for %NULL.  A name longer
 *   than %AXIS_IDENTIFY_NAME_MAX, or holding a NUL, is given back as far as
 *   the NUL with nothing found.
 */
gchar *
axis_identify_name_ids(
	const gchar     *name,
	gssize           length,
	AxisIdentifyIds *ids
);

/**
 * axis_identify_nfo_scan:
 * @data: (array length=length) (nullable): the start of an .nfo file
 * @length: how many bytes there are; only the first %AXIS_IDENTIFY_NFO_MAX
 *   are looked at
 * @ids: (out caller-allocates): what was found (initialised here)
 */
void
axis_identify_nfo_scan(
	const guint8    *data,
	gsize            length,
	AxisIdentifyIds *ids
);

G_END_DECLS

#endif /* AXIS_IDENTIFY_IDS_H */
