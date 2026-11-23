/*
 * axis-analysis-rules.h - what counts as a leftover
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The leftover rules are data: each is a name, a label, a plain-language
 * description and one SQL condition over a library_file row "f" and its
 * root "r".  The same condition text is used twice, so that a figure and
 * the list it links to cannot disagree: by the refresh, which counts what
 * matches, and by the file list's "leftover" filter (axis-library-browse.c),
 * which lists it.
 *
 * Every rule is conservative: a file is a leftover only when its name and
 * place say so, and a name that merely resembles a rule ("Example.mkv",
 * "Sampler.mkv", a long film with "Sample" in its title) is not one.
 * Files under a downloads root are never leftovers (they are seeding), and
 * nothing here deletes or moves a file: the page only lists.
 *
 *  sample     a video whose name has the word "sample" and which is no more
 *             than 100 MiB, or any video in a folder named Sample or
 *             Samples (movies, TV and other roots)
 *  extras     files in a folder named Extras, Featurettes, Behind The
 *             Scenes, Deleted Scenes, Interviews or Trailers, and files
 *             named like "Film-trailer.mkv" (movies and TV roots)
 *  subtitle   a subtitle with no video beside it whose name it begins
 *             with (the video's name without its extension, then a
 *             non-letter: "Film.en.srt" belongs to "Film.mkv"); in a folder
 *             named Subs or Subtitles it belongs to any video in the
 *             folder above.  A video that is itself not seen any more
 *             (suspected, missing) still counts as beside it.
 *  junk       Thumbs.db, desktop.ini, .DS_Store, AppleDouble "._" files,
 *             .url, .sfv and .diz files, RARBG notices
 *
 * Empty folders cannot be listed: the index records files, not folders.
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_ANALYSIS_RULES_H
#define AXIS_ANALYSIS_RULES_H

#include <glib.h>

G_BEGIN_DECLS

/* The biggest "sample" that is still a sample, in bytes. */
#define AXIS_ANALYSIS_SAMPLE_MAX_BYTES 104857600

typedef struct
{
	const gchar *id;
	const gchar *label;
	const gchar *help;
	const gchar *condition;	/* SQL over library_file f and library_root r */
} AxisLeftoverRule;

/* The rules, ended by an entry whose id is %NULL. */
const AxisLeftoverRule *
axis_leftover_rules(void);

/**
 * axis_leftover_condition:
 * @id: a rule's id, or "any"
 *
 * Returns: (transfer none) (nullable): the SQL condition ("any" is every
 *   rule's, or-ed), or %NULL for an id no rule has.  Static for a rule's id;
 *   for "any", also static.
 */
const gchar *
axis_leftover_condition(const gchar *id);

/* The ids a filter accepts: every rule's, then "any"; ended by %NULL. */
const gchar *const *
axis_leftover_filter_values(void);

G_END_DECLS

#endif /* AXIS_ANALYSIS_RULES_H */
