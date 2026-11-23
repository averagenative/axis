/*
 * axis-analysis-name.h - the name normaliser of the "same title" duplicate
 * tier and of the split-across-volumes report
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Titles are not identified yet (that comes with the media lookup
 * changes), so "the same film" can only be told from file and folder names.
 * This is deliberately a small, strict function and not a release-name
 * parser: it removes only what cannot tell two titles apart (case,
 * accents, punctuation, separators) and refuses to compare anything it is
 * not sure of.  A name it cannot read is not compared at all; a wrong
 * group is worse than a missed one, because findings are acted on later.
 * It is replaced by real identification when that lands.
 *
 * The rules:
 *
 *  - Normalising text: decomposed (NFKD) and case-folded; combining marks
 *    dropped; letters and digits kept; '&' read as "and"; apostrophes
 *    dropped; every other run of characters (spaces, dots, underscores,
 *    dashes, brackets) one space; trimmed.  Nothing else: articles, "and",
 *    edition words and quality words stay, so "The Mazework" and "Mazework"
 *    are different titles.
 *  - A film is title and year.  The year is the last token of four digits
 *    from 1900 to 2099, standing on its own, that has a title before it
 *    ("Steel.Drifter.2049.2017.2160p" is "steel drifter 2049", 2017).  The
 *    file's own name is read first; only a name with no year token at all
 *    falls back to the folder it is in.  A film without a year is not
 *    compared: two films of one title and different years must never
 *    group.
 *  - An episode is show, season and episode: "S01E02", or "1x02".  A name
 *    with a second episode in it ("S01E01E02", "S01E01-02") is not
 *    compared.  The show is the text before the marker, or the show's
 *    folder when there is none; the year comes from the show's text, or
 *    from the folder when the folder is the same show.  A show with a
 *    year and the same show without are different shows.
 *  - A track is artist, album and title from tags, all three present.
 *  - A series is the folder directly below a root, an album the folder two
 *    levels below it (artist and album); their keys are the normalised
 *    folder names, with the year for a series that has one.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-analysis-name.c).
 */

#ifndef AXIS_ANALYSIS_NAME_H
#define AXIS_ANALYSIS_NAME_H

#include <glib.h>

G_BEGIN_DECLS

/* Input longer than this (bytes) is not compared. */
#define AXIS_ANALYSIS_NAME_MAX_INPUT  1024

/**
 * axis_analysis_normalise:
 * @text: (nullable): text to compare
 *
 * Returns: (transfer full) (nullable): the normalised text ("" when nothing
 *   is left of it), or %NULL for %NULL, text that is not valid UTF-8, text
 *   longer than %AXIS_ANALYSIS_NAME_MAX_INPUT, or text whose normal form
 *   would be
 */
gchar *
axis_analysis_normalise(const gchar *text);

/**
 * axis_analysis_parse_film:
 * @path: a path relative to a root, in the stored form (or a bare name)
 * @title: (out) (transfer full): the normalised title, not empty
 * @year: (out): the year, 1900 to 2099
 *
 * Returns: %TRUE if the name says a title and a year unambiguously
 */
gboolean
axis_analysis_parse_film(
	const gchar  *path,
	gchar       **title,
	gint         *year
);

/**
 * axis_analysis_parse_episode:
 * @path: a path relative to a root, in the stored form
 * @show: (out) (transfer full): the normalised show title, not empty
 * @show_year: (out): the show's year, 0 when none is given
 * @season: (out): the season
 * @episode: (out): the episode
 *
 * Returns: %TRUE if the name says one episode of a show unambiguously
 */
gboolean
axis_analysis_parse_episode(
	const gchar  *path,
	gchar       **show,
	gint         *show_year,
	gint         *season,
	gint         *episode
);

/* The comparison keys (never %NULL for a key that exists, %NULL for
 * something not comparable): equal keys are likely the same title. */
gchar *
axis_analysis_film_key(const gchar *path);

gchar *
axis_analysis_episode_key(const gchar *path);

gchar *
axis_analysis_track_key(
	const gchar *artist,
	const gchar *album,
	const gchar *title
);

/* A show's folder name: "title|year", the year 0 when the name has none. */
gchar *
axis_analysis_show_key(const gchar *folder);

/* An artist's and an album's folder names: "artist|album". */
gchar *
axis_analysis_album_key(
	const gchar *artist_folder,
	const gchar *album_folder
);

G_END_DECLS

#endif /* AXIS_ANALYSIS_NAME_H */
