/*
 * fuzz-import-name.c - libFuzzer target for the names the import pipeline
 * gives and the character rule
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked; the rest is the input.
 *
 *  0  a title, an edition, a quality, a group, an extension and a format,
 *     separated by 0x1f, and a budget from the second byte: when the names
 *     can be built, both are names Axis may give (valid UTF-8, no '/', no
 *     control or direction character, not hidden, no trailing dot or space),
 *     within the budget, and the file begins with the folder and ends with
 *     the extension; with the default budget its temporary name fits too.
 *  1  any bytes through the character rule: valid UTF-8, none of what the
 *     rule removes is left, and cleaning again changes nothing.
 *  2  a format: a format that passes the check always builds a name for an
 *     ordinary title; one that does not never does.
 *  3  a found name through the readers (folders, kinds, extensions, extras,
 *     samples, parts, an extra's name): nothing crashes, and an extra's name,
 *     when there is one, is a name Axis may give.
 *  4  an episode: a series' title, an episode's title, a quality, a group,
 *     an extension and a format, separated by 0x1f; the season, the
 *     episodes (1 to 4 of them) and the budget from the first bytes.  When
 *     the names can be built, the folder, the season's folder and the file
 *     are names Axis may give within the budget, the season's folder is
 *     "Season NN", the file states the episode mark, ends with the extension
 *     and starts with its stem; renamed, it starts with the series' title
 *     as the folder has it, cut or not.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "import/axis-import-name.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static gboolean
direction(gunichar c)
{
	return c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
		(c >= 0x2066 && c <= 0x2069);
}

static void
check_clean(const gchar *clean)
{
	const gchar *p;
	gsize n = strlen(clean);

	if (!g_utf8_validate(clean, -1, NULL))
		abort();
	if (n > 0 && (clean[0] == '.' || clean[0] == ' ' || clean[n - 1] == '.' || clean[n - 1] == ' '))
		abort();
	for (p = clean; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c == '/' || c == '\\' || c == ':' || g_unichar_iscntrl(c) || direction(c))
			abort();
	}
}

static void
check_name(
	const AxisImportName *name,
	gsize                 budget,
	const gchar          *extension
){
	g_autofree gchar *lower = g_ascii_strdown(extension, -1);

	if (!axis_import_name_valid(name->folder) || !axis_import_name_valid(name->file))
		abort();
	if (strlen(name->folder) > budget || strlen(name->file) > budget)
		abort();
	if (!g_str_has_suffix(name->file, lower))
		abort();
	if (!g_str_has_prefix(name->file, name->stem))
		abort();
	if (budget == AXIS_IMPORT_NAME_BUDGET && strlen(name->file) + AXIS_IMPORT_TEMP_MAX > AXIS_IMPORT_COMPONENT_MAX)
		abort();
}

static void
check_build(
	const guint8 *data,
	gsize         size
){
	g_autofree gchar *text = NULL;
	g_auto(GStrv) parts = NULL;
	AxisImportNameInput input;
	AxisImportName name;
	gsize budget;

	if (size < 1)
		return;
	budget = data[0] < 16 ? AXIS_IMPORT_NAME_BUDGET : (gsize)data[0];
	text = g_strndup((const gchar *)data + 1, size - 1);
	parts = g_strsplit(text, "\x1f", 8);
	memset(&input, 0, sizeof(input));
	input.title = parts[0];
	input.year = size % 3 == 0 ? 0 : 1900 + (gint)(size % 200);
	input.tmdb_id = size % 5 == 0 ? NULL : "12345";
	input.imdb_id = "tt0001";
	input.layout = (AxisImportLayout)(size % 3);
	if (parts[0] != NULL && parts[1] != NULL) {
		input.edition = parts[1];
		if (parts[2] != NULL) {
			input.quality = parts[2];
			if (parts[3] != NULL) {
				input.group = parts[3];
				input.hdr = parts[3];
				if (parts[4] != NULL) {
					input.extension = parts[4];
					if (parts[5] != NULL)
						input.format = parts[5];
				}
			}
		}
	}
	if (input.extension == NULL)
		input.extension = ".mkv";
	input.budget = budget;
	if ((size & 8) != 0)
		input.keep = parts[0];
	memset(&name, 0, sizeof(name));
	if (axis_import_name_build(&input, &name, NULL)) {
		check_name(&name, budget, input.extension);
		axis_import_name_clear(&name);
	}
}

static void
check_rule(
	const guint8 *data,
	gsize         size
){
	g_autofree gchar *clean = axis_import_name_clean((const gchar *)data, (gssize)size);
	g_autofree gchar *again = NULL;

	check_clean(clean);
	again = axis_import_name_clean(clean, -1);
	if (strcmp(clean, again) != 0)
		abort();
}

static void
check_format(
	const guint8 *data,
	gsize         size
){
	g_autofree gchar *format = g_strndup((const gchar *)data, size);
	g_autofree gchar *reason = NULL;
	gboolean valid = axis_import_format_check(format, &reason);
	AxisImportNameInput input;
	AxisImportName name;
	gboolean built;

	if (!valid && reason == NULL)
		abort();
	memset(&input, 0, sizeof(input));
	input.title = "An Ordinary Title";
	input.year = 2001;
	input.tmdb_id = "42";
	input.quality = "WEB-DL 1080p";
	input.group = "GRP";
	input.edition = "Extended";
	input.extension = ".mkv";
	input.format = format;
	memset(&name, 0, sizeof(name));
	built = axis_import_name_build(&input, &name, NULL);
	/* NUL inside the input makes the format shorter than it: only judge
	 * what was checked. */
	if (memchr(data, '\0', size) == NULL && built != valid)
		abort();
	if (built) {
		check_name(&name, AXIS_IMPORT_NAME_BUDGET, ".mkv");
		axis_import_name_clear(&name);
	}
}

static void
check_found(
	const guint8 *data,
	gsize         size
){
	const gchar *name = (const gchar *)data;
	const gchar *extra = NULL;
	const gchar *type = NULL;
	g_autofree gchar *ext = NULL;
	g_autofree gchar *extra_name = NULL;
	gint part = 0;

	(void)axis_import_folder_kind(name, size, &extra);
	(void)axis_import_file_kind(name, size);
	ext = axis_import_extension(name, size);
	if (ext != NULL && (ext[0] != '.' || strlen(ext) > 17))
		abort();
	(void)axis_import_extra_suffix(name, size, &type);
	(void)axis_import_sample_marked(name, size, size > 4 ? "Sample Title" : NULL);
	if (axis_import_part_marked(name, size, &part) && (part < 1 || part > 9))
		abort();
	if (memchr(name, '\0', size) == NULL) {
		g_autofree gchar *copy = g_strndup(name, size);

		extra_name = axis_import_extra_name(copy, size);
		if (extra_name != NULL && !axis_import_name_valid(extra_name))
			abort();
	}
}

static void
check_episode(
	const guint8 *data,
	gsize         size
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *mark = NULL;
	g_autofree gchar *season_folder = NULL;
	g_auto(GStrv) parts = NULL;
	AxisImportNameInput input;
	AxisImportName name;
	gint episodes[4];
	guint n;
	guint i;
	gsize budget;

	if (size < 4)
		return;
	budget = data[0] < 16 ? AXIS_IMPORT_NAME_BUDGET : (gsize)data[0];
	n = 1 + data[1] % 4;
	for (i = 0; i < n; i++)
		episodes[i] = (gint)(data[2] % 40) + (data[1] & 0x10 ? (gint)i * 2 : (gint)i);
	text = g_strndup((const gchar *)data + 4, size - 4);
	parts = g_strsplit(text, "\x1f", 8);
	memset(&input, 0, sizeof(input));
	input.episode = TRUE;
	input.title = parts[0];
	input.year = data[3] % 3 == 0 ? 0 : 1950 + data[3] % 70;
	input.tvdb_id = data[3] % 5 == 0 ? NULL : "350001";
	input.tmdb_id = "201";
	input.layout = (AxisImportLayout)(data[3] % 3);
	input.season = data[3] % 7 == 0 ? 0 : (gint)(data[3] % 30);
	input.episodes = episodes;
	input.n_episodes = n;
	input.air_date = "2024-03-15";
	if (parts[0] != NULL && parts[1] != NULL) {
		input.episode_title = parts[1];
		if (parts[2] != NULL) {
			input.quality = parts[2];
			if (parts[3] != NULL) {
				input.group = parts[3];
				if (parts[4] != NULL) {
					input.extension = parts[4];
					if (parts[5] != NULL)
						input.format = parts[5];
				}
			}
		}
	}
	if (input.extension == NULL)
		input.extension = ".mkv";
	input.budget = budget;
	if ((data[1] & 0x20) != 0)
		input.keep = parts[0];
	memset(&name, 0, sizeof(name));
	if (!axis_import_name_build(&input, &name, NULL))
		return;
	check_name(&name, budget, input.extension);
	season_folder = g_strdup_printf("Season %02d", input.season);
	if (name.season_folder == NULL || strcmp(name.season_folder, season_folder) != 0 ||
	    !axis_import_name_valid(name.season_folder))
		abort();
	if (input.keep == NULL) {
		g_autofree gchar *first = g_strdup_printf("S%02dE%02d", input.season, episodes[0]);
		const gchar *year = strstr(name.folder, input.year > 0 ? " (" : "");

		if (strstr(name.file, first) == NULL)
			abort();
		/* The file's series title is the folder's, or a cut of it. */
		if (year != NULL && input.year > 0) {
			gsize title_length = (gsize)(year - name.folder);
			gsize k;

			for (k = 0; k < title_length && name.file[k] != '\0' && name.file[k] != ' ' ; k++) {
				if (name.file[k] != name.folder[k])
					abort();
			}
		}
	}
	axis_import_name_clear(&name);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1 || size > 4096)
		return 0;
	switch (data[0] % 5) {
	case 0:
		check_build(data + 1, size - 1);
		break;
	case 1:
		check_rule(data + 1, size - 1);
		break;
	case 2:
		check_format(data + 1, size - 1);
		break;
	case 3:
		check_found(data + 1, size - 1);
		break;
	default:
		check_episode(data + 1, size - 1);
		break;
	}
	return 0;
}
