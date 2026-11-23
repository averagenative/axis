/*
 * axis-analysis-rules.c - what counts as a leftover (see the header)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-analysis-rules.h"

#include <string.h>

#define SAMPLE_BYTES_TEXT "104857600"

G_STATIC_ASSERT(AXIS_ANALYSIS_SAMPLE_MAX_BYTES == 104857600);

/*
 * A folder of a given name anywhere in the path is looked for with LIKE on
 * "/" and the lower-cased path, and the regular expressions only run on
 * names that pass a cheap test: on two million files a case-insensitive
 * regular expression over every path costs seconds, these cost a fraction
 * of one.
 */
#define COND_SAMPLE \
	"(f.kind = 'video' AND r.media_type IN ('movies', 'tv', 'other') AND (" \
	"(lower(f.base_name) LIKE '%sample%' AND f.size <= " SAMPLE_BYTES_TEXT \
	" AND f.base_name ~* '(^|[^[:alpha:]])sample([^[:alpha:]]|$)')" \
	" OR ('/' || lower(f.rel_path)) LIKE ANY (ARRAY['%/sample/%', '%/samples/%'])))"

#define COND_EXTRAS \
	"(r.media_type IN ('movies', 'tv') AND (" \
	"('/' || lower(f.rel_path)) LIKE ANY (ARRAY['%/extras/%', '%/extra/%', '%/featurettes/%', '%/featurette/%'," \
	" '%/behind the scenes/%', '%/deleted scenes/%', '%/interviews/%', '%/trailers/%', '%/trailer/%'])" \
	" OR (position('-' IN f.base_name) > 0" \
	"  AND lower(f.base_name) ~ '-(trailer|featurette|behindthescenes|deleted|interview)\\.[a-z0-9]{1,5}$')))"

/* A subtitle belongs to a video beside it when it begins with the video's
 * name without its extension and a non-letter or digit (or nothing) follows. */
#define COND_SUBTITLE \
	"(f.kind = 'subtitle' AND r.media_type <> 'downloads'" \
	" AND NOT EXISTS (SELECT 1 FROM library_file v" \
	"   WHERE v.root_id = f.root_id AND v.dir_hash = f.dir_hash AND v.kind = 'video'" \
	"   AND lower(left(f.base_name, length(regexp_replace(v.base_name, '\\.[^.]*$', ''))))" \
	"     = lower(regexp_replace(v.base_name, '\\.[^.]*$', ''))" \
	"   AND (length(f.base_name) = length(regexp_replace(v.base_name, '\\.[^.]*$', ''))" \
	"     OR substr(f.base_name, length(regexp_replace(v.base_name, '\\.[^.]*$', '')) + 1, 1)" \
	"       !~ '[[:alnum:]]'))" \
	" AND NOT (library_base_name_of(library_dir_of(f.rel_path)) ~* '^(subs?|subtitles?)$'" \
	"   AND EXISTS (SELECT 1 FROM library_file v2" \
	"     WHERE v2.root_id = f.root_id AND v2.kind = 'video'" \
	"     AND v2.dir_hash = library_path_hash(library_dir_of(library_dir_of(f.rel_path))))))"

#define COND_JUNK \
	"(r.media_type <> 'downloads' AND (lower(f.base_name) IN ('thumbs.db', 'desktop.ini', '.ds_store')" \
	" OR f.base_name ~ '^\\._' OR f.base_name ~* '\\.(url|sfv|diz)$' OR f.base_name ~* '^rarbg.*\\.txt$'))"

static const AxisLeftoverRule rules[] = {
	{
		"sample", "Samples",
		"A video with the word \"sample\" in its name that is no more than 100 MiB, or any video in a "
		"folder named Sample or Samples.",
		COND_SAMPLE
	},
	{
		"extras", "Extras",
		"Files in a folder named Extras, Featurettes, Behind The Scenes, Deleted Scenes, Interviews or "
		"Trailers, and files named like Movie-trailer.mkv, under movies and TV roots. Some people keep these.",
		COND_EXTRAS
	},
	{
		"subtitle", "Subtitles without a video",
		"A subtitle with no video beside it that its name begins with. A subtitle in a Subs or "
		"Subtitles folder belongs to any video in the folder above.",
		COND_SUBTITLE
	},
	{
		"junk", "Known junk",
		"Thumbs.db, desktop.ini, .DS_Store, \"._\" files, .url, .sfv and .diz files and RARBG notices.",
		COND_JUNK
	},
	{ NULL, NULL, NULL, NULL }
};

static const gchar *const filter_values[] = { "sample", "extras", "subtitle", "junk", "any", NULL };

const AxisLeftoverRule *
axis_leftover_rules(void)
{
	return rules;
}

const gchar *const *
axis_leftover_filter_values(void)
{
	return filter_values;
}

const gchar *
axis_leftover_condition(const gchar *id)
{
	static const gchar any[] = "(" COND_SAMPLE " OR " COND_EXTRAS " OR " COND_SUBTITLE " OR " COND_JUNK ")";
	guint i;

	if (id == NULL)
		return NULL;
	if (strcmp(id, "any") == 0)
		return any;
	for (i = 0; rules[i].id != NULL; i++) {
		if (strcmp(rules[i].id, id) == 0)
			return rules[i].condition;
	}
	return NULL;
}
