/*
 * axis-release-audit.c - run the release-name parser over a file of real
 * names and say what it could not read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-release-audit [--top N] [--each] FILE
 *   axis-release-audit --json FILE
 *
 * FILE has one name per line ("-" reads standard input).  The summary
 * counts the names with no title, films with no year, and names with no
 * resolution, source, codec or group; lists the words not understood by
 * how often they occur; and lists the names understood least.  --each
 * prints every name with what was read from it, for checking by hand.
 *
 * --json prints one JSON object a line instead, for comparing with another
 * reading of the same names: the fields found, under the names and in the
 * values the corpora in tests/fixtures/release use (a field not found is
 * left out).  A line may also carry, after a tab each, the kind of title
 * it should be ("movie" or "tv"), that title, its year (0 for none) and
 * any other names it has; the name is then matched against that one title
 * as the movies and TV imports do, and the object adds "match" (as
 * axis_release_match_name() says it) and "match_reason".
 *
 * A developer's tool, built with the tests and never installed.  It reads
 * the file and writes to standard output only: the names stay on the
 * machine they are on.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "release/axis-release.h"

typedef struct
{
	gchar *name;
	guint  score;
	gchar *reading;
} Hard;

static gint
by_score(
	gconstpointer a,
	gconstpointer b
){
	const Hard *x = *(const Hard *const *)a;
	const Hard *y = *(const Hard *const *)b;

	if (x->score != y->score)
		return x->score > y->score ? -1 : 1;
	return strcmp(x->name, y->name);
}

static void
hard_free(Hard *hard)
{
	g_free(hard->name);
	g_free(hard->reading);
	g_free(hard);
}

typedef struct
{
	const gchar *word;
	guint        count;
} Word;

static gint
by_count(
	gconstpointer a,
	gconstpointer b
){
	const Word *x = a;
	const Word *y = b;

	if (x->count != y->count)
		return x->count > y->count ? -1 : 1;
	return strcmp(x->word, y->word);
}

static gchar *
read_input(
	const gchar  *path,
	GError      **error
){
	gchar *text = NULL;

	if (strcmp(path, "-") == 0) {
		GString *in = g_string_new(NULL);
		gchar buffer[65536];
		gsize n;

		while ((n = fread(buffer, 1, sizeof(buffer), stdin)) > 0)
			g_string_append_len(in, buffer, (gssize)n);
		return g_string_free(in, FALSE);
	}
	if (!g_file_get_contents(path, &text, NULL, error))
		return NULL;
	return text;
}

static void
line(
	const gchar *label,
	guint        count,
	guint        of
){
	if (of > 0)
		printf("  %-26s %7u  (%.1f%%)\n", label, count, 100.0 * count / of);
	else
		printf("  %-26s %7u\n", label, count);
}

/* ── --json ─────────────────────────────────────────────────────── */

typedef const gchar *(*FlagName)(guint flag);

static void
add_flags(
	JsonBuilder *b,
	const gchar *member,
	guint        flags,
	FlagName     flag_name
){
	guint bit;

	if (flags == 0)
		return;
	json_builder_set_member_name(b, member);
	json_builder_begin_array(b);
	for (bit = 0; bit < 32; bit++) {
		if ((flags & (1u << bit)) && flag_name(1u << bit) != NULL)
			json_builder_add_string_value(b, flag_name(1u << bit));
	}
	json_builder_end_array(b);
}

static void
add_numbers(
	JsonBuilder *b,
	const gchar *member,
	const gint  *numbers,
	guint        count
){
	guint i;

	if (count == 0)
		return;
	json_builder_set_member_name(b, member);
	json_builder_begin_array(b);
	for (i = 0; i < count; i++)
		json_builder_add_int_value(b, numbers[i]);
	json_builder_end_array(b);
}

static void
add_string(
	JsonBuilder *b,
	const gchar *member,
	const gchar *value
){
	if (value == NULL)
		return;
	json_builder_set_member_name(b, member);
	json_builder_add_string_value(b, value);
}

/* What was read, in the corpora's labels. */
static void
add_reading(
	JsonBuilder       *b,
	const AxisRelease *r
){
	guint i;

	add_string(b, "title", r->title);
	if (r->year > 0) {
		json_builder_set_member_name(b, "year");
		json_builder_add_int_value(b, r->year);
	}
	if (r->season_first >= 0) {
		json_builder_set_member_name(b, "season");
		if (r->season_last != r->season_first) {
			json_builder_begin_array(b);
			json_builder_add_int_value(b, r->season_first);
			json_builder_add_int_value(b, r->season_last);
			json_builder_end_array(b);
		} else {
			json_builder_add_int_value(b, r->season_first);
		}
	}
	add_numbers(b, "episodes", r->episodes, r->episode_count);
	add_numbers(b, "absolute", r->absolute, r->absolute_count);
	if (r->air_year > 0) {
		g_autofree gchar *date = g_strdup_printf("%04d-%02d-%02d", r->air_year, r->air_month, r->air_day);

		add_string(b, "date", date);
	}
	if (r->complete) {
		json_builder_set_member_name(b, "complete");
		json_builder_add_boolean_value(b, TRUE);
	}
	if (r->version > 0) {
		json_builder_set_member_name(b, "version");
		json_builder_add_int_value(b, r->version);
	}
	add_string(b, "resolution", axis_release_resolution_name(r->resolution));
	if (r->interlaced) {
		json_builder_set_member_name(b, "interlaced");
		json_builder_add_boolean_value(b, TRUE);
	}
	add_string(b, "source", axis_release_source_name(r->source));
	add_string(b, "service", r->service);
	add_string(b, "codec", axis_release_codec_name(r->codec));
	if (r->bit_depth > 0) {
		json_builder_set_member_name(b, "bits");
		json_builder_add_int_value(b, r->bit_depth);
	}
	add_flags(b, "hdr", r->hdr, axis_release_hdr_name);
	add_flags(b, "audio", r->audio, axis_release_audio_name);
	if (r->atmos) {
		json_builder_set_member_name(b, "atmos");
		json_builder_add_boolean_value(b, TRUE);
	}
	if (r->channels[0] != '\0')
		add_string(b, "channels", r->channels);
	add_flags(b, "editions", r->editions, axis_release_edition_name);
	if (r->language_count > 0) {
		json_builder_set_member_name(b, "languages");
		json_builder_begin_array(b);
		for (i = 0; i < r->language_count; i++)
			json_builder_add_string_value(b, r->languages[i]);
		json_builder_end_array(b);
	}
	add_flags(b, "lang_marks", r->language_marks, axis_release_lang_mark_name);
	add_flags(b, "revision", r->revision, axis_release_revision_name);
	add_flags(b, "marks", r->marks, axis_release_mark_name);
	add_flags(b, "stop", r->stop, axis_release_stop_name);
	add_string(b, "group", r->group);
	add_string(b, "checksum", r->checksum);
	if (r->unknown->len > 0) {
		json_builder_set_member_name(b, "unknown");
		json_builder_begin_array(b);
		for (i = 0; i < r->unknown->len; i++)
			json_builder_add_string_value(b, g_ptr_array_index(r->unknown, i));
		json_builder_end_array(b);
	}
}

/* The title a line names after its tabs ("movie|tv", title, year, other
 * names...), or %NULL when it names none. */
static AxisMetaTitle *
line_title(gchar **fields)
{
	AxisMetaTitle *title;
	guint i;

	if (fields[0] == NULL || fields[1] == NULL || fields[2] == NULL)
		return NULL;
	if (strcmp(fields[1], "movie") == 0 || strcmp(fields[1], "film") == 0)
		title = axis_meta_title_new(AXIS_META_KIND_FILM);
	else if (strcmp(fields[1], "tv") == 0 || strcmp(fields[1], "series") == 0)
		title = axis_meta_title_new(AXIS_META_KIND_SERIES);
	else
		return NULL;
	title->title = fields[2][0] != '\0' ? g_strdup(fields[2]) : NULL;
	if (fields[3] != NULL) {
		title->year = atoi(fields[3]);
		for (i = 4; fields[i] != NULL; i++) {
			AxisMetaAltTitle *alt;

			if (fields[i][0] == '\0')
				continue;
			if (title->original_title == NULL) {
				title->original_title = g_strdup(fields[i]);
				continue;
			}
			if (title->alt_titles == NULL)
				title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
			alt = g_new0(AxisMetaAltTitle, 1);
			alt->title = g_strdup(fields[i]);
			g_ptr_array_add(title->alt_titles, alt);
		}
	}
	return title;
}

static void
json_line(
	JsonGenerator *gen,
	gchar         *line
){
	gchar **fields = g_strsplit(line, "\t", -1);
	const gchar *name = fields[0];
	AxisMetaTitle *title = line_title(fields);
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;
	AxisRelease *r = axis_release_parse(name);

	json_builder_begin_object(b);
	add_string(b, "name", name);
	add_reading(b, r);
	if (title != NULL) {
		const AxisMetaTitle *candidates[1];
		gchar *reason = NULL;
		AxisReleaseMatch match;

		candidates[0] = title;
		match = axis_release_match(r, candidates, 1, NULL, &reason);
		add_string(b, "match", axis_release_match_name(match));
		add_string(b, "match_reason", reason);
		g_free(reason);
		axis_meta_title_free(title);
	}
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	text = json_generator_to_data(gen, NULL);
	printf("%s\n", text);
	axis_release_free(r);
	g_strfreev(fields);
}

static void
json_mode(gchar *text)
{
	g_autoptr(JsonGenerator) gen = json_generator_new();
	gchar **lines = g_strsplit(text, "\n", -1);
	guint k;

	for (k = 0; lines[k] != NULL; k++) {
		gchar *line = lines[k];
		gsize length = strlen(line);

		if (length > 0 && line[length - 1] == '\r')
			line[length - 1] = '\0';
		if (line[0] == '\0')
			continue;
		/* Valid text only: the generator needs it, and the parser finds
		 * nothing in anything else. */
		if (!g_utf8_validate(line, -1, NULL)) {
			printf("{\"invalid\":true}\n");
			continue;
		}
		json_line(gen, line);
	}
	g_strfreev(lines);
}

int
main(int argc, char **argv)
{
	gint top = 20;
	gboolean each = FALSE;
	gboolean json = FALSE;
	const gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	gchar **lines;
	GHashTable *words;
	GPtrArray *hard;
	GArray *ranked;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint names = 0;
	guint unreadable = 0;
	guint episodes = 0;
	guint no_title = 0;
	guint no_year = 0;
	guint no_resolution = 0;
	guint no_source = 0;
	guint no_codec = 0;
	guint no_group = 0;
	guint with_unknown = 0;
	guint unknown_words = 0;
	guint stops[7] = { 0 };
	guint recordings = 0;
	gint i;
	guint k;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
			top = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--each") == 0) {
			each = TRUE;
		} else if (strcmp(argv[i], "--json") == 0) {
			json = TRUE;
		} else if (path == NULL) {
			path = argv[i];
		} else {
			path = NULL;
			break;
		}
	}
	if (path == NULL || top < 0) {
		fprintf(stderr, "usage: axis-release-audit [--top N] [--each] FILE (\"-\" for standard input)\n"
			"       axis-release-audit --json FILE\n");
		return 2;
	}
	text = read_input(path, &error);
	if (text == NULL) {
		fprintf(stderr, "axis-release-audit: %s\n", error->message);
		return 1;
	}
	if (json) {
		json_mode(text);
		return 0;
	}

	words = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	hard = g_ptr_array_new_with_free_func((GDestroyNotify)hard_free);
	lines = g_strsplit(text, "\n", -1);
	for (k = 0; lines[k] != NULL; k++) {
		gchar *name = g_strstrip(lines[k]);
		AxisRelease *r;
		gboolean film;
		guint score = 0;
		guint u;

		if (name[0] == '\0')
			continue;
		names++;
		if (strlen(name) > AXIS_RELEASE_NAME_MAX || !g_utf8_validate(name, -1, NULL)) {
			unreadable++;
			continue;
		}
		r = axis_release_parse(name);
		film = !axis_release_is_episode(r);
		if (!film)
			episodes++;
		if (r->title == NULL) {
			no_title++;
			score += 5;
		}
		if (film && r->year == 0) {
			no_year++;
			score++;
		}
		if (r->resolution == AXIS_RELEASE_RESOLUTION_NONE) {
			no_resolution++;
			score++;
		}
		if (r->source == AXIS_RELEASE_SOURCE_NONE) {
			no_source++;
			score++;
		}
		if (r->codec == AXIS_RELEASE_CODEC_NONE)
			no_codec++;
		if (r->group == NULL)
			no_group++;
		if (r->source >= AXIS_RELEASE_SOURCE_CAM && r->source <= AXIS_RELEASE_SOURCE_WORKPRINT)
			recordings++;
		for (u = 0; u < G_N_ELEMENTS(stops); u++) {
			if (r->stop & (1u << u))
				stops[u]++;
		}
		if (r->unknown->len > 0)
			with_unknown++;
		for (u = 0; u < r->unknown->len; u++) {
			const gchar *word = g_ptr_array_index(r->unknown, u);
			gpointer count = g_hash_table_lookup(words, word);

			g_hash_table_replace(words, g_strdup(word), GUINT_TO_POINTER(GPOINTER_TO_UINT(count) + 1));
			unknown_words++;
			score++;
		}
		if (each) {
			g_autofree gchar *reading = axis_release_describe(r);

			printf("%s\n    %s\n", name, reading);
		}
		if (score > 0) {
			Hard *h = g_new0(Hard, 1);

			h->name = g_strdup(name);
			h->score = score;
			h->reading = axis_release_describe(r);
			g_ptr_array_add(hard, h);
		}
		axis_release_free(r);
	}
	g_strfreev(lines);

	printf("axis-release-audit: %s\n", path);
	line("names", names, 0);
	line("not valid text, too long", unreadable, names);
	line("episodes", episodes, names);
	line("films", names - unreadable - episodes, names);
	line("no title", no_title, names);
	line("films with no year", no_year, names - unreadable - episodes);
	line("no resolution", no_resolution, names);
	line("no source", no_source, names);
	line("no video codec", no_codec, names);
	line("no group", no_group, names);
	line("with words not understood", with_unknown, names);
	line("words not understood", unknown_words, 0);
	line("recordings (cam to workprint)", recordings, names);
	for (k = 0; k < G_N_ELEMENTS(stops); k++) {
		g_autofree gchar *label = g_strdup_printf("stop: %s", axis_release_stop_name(1u << k));

		line(label, stops[k], names);
	}

	ranked = g_array_new(FALSE, FALSE, sizeof(Word));
	g_hash_table_iter_init(&iter, words);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Word w;

		w.word = key;
		w.count = GPOINTER_TO_UINT(value);
		g_array_append_val(ranked, w);
	}
	g_array_sort(ranked, by_count);
	printf("words not understood, by how often (%u different; the first %d):\n", ranked->len, top * 2);
	for (k = 0; k < ranked->len && (gint)k < top * 2; k++) {
		const Word *w = &g_array_index(ranked, Word, k);

		printf("  %7u  %s\n", w->count, w->word);
	}

	g_ptr_array_sort(hard, by_score);
	printf("names understood least (%u with something missing; the first %d):\n", hard->len, top);
	for (k = 0; k < hard->len && (gint)k < top; k++) {
		const Hard *h = g_ptr_array_index(hard, k);

		printf("  %3u  %s\n       %s\n", h->score, h->name, h->reading);
	}

	g_array_unref(ranked);
	g_ptr_array_unref(hard);
	g_hash_table_unref(words);
	return 0;
}
