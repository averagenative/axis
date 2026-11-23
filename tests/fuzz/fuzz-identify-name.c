/*
 * fuzz-identify-name.c - libFuzzer target for what identification reads from
 * names: identifier tokens, a name's reading, the units of a folder and the
 * decision over them (identify/axis-identify-ids.h, axis-identify-rules.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked; the rest is the input.
 *
 *  0  any bytes as a name through the token reader: what is found is a
 *     well-formed identifier that the name holds; what is left is no longer
 *     than the name, holds no NUL, is valid UTF-8 when the name was, has no
 *     space at its ends, and reading it again finds nothing that was not
 *     found the first time (a token never appears by taking another out).
 *  1  a folder and up to 16 file names, one per line, through the
 *     reader and the grouping: every file is in exactly one group, in
 *     order, at most one group is the folder's, none is empty; then each
 *     group's reading and a decision against two fixed films: a verdict
 *     other than unknown names a title among its candidates, at most eight
 *     candidates, and every reason is a non-empty sentence of valid UTF-8.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "identify/axis-identify-ids.h"
#include "identify/axis-identify-rules.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_ids(
	const AxisIdentifyIds *ids,
	const gchar           *name,
	gsize                  length
){
	if (ids->tmdb[0] != '\0') {
		gsize n = strlen(ids->tmdb);
		gsize i;

		if (n < 1 || n > 12 || ids->tmdb[0] == '0' || g_strstr_len(name, (gssize)length, ids->tmdb) == NULL)
			abort();
		for (i = 0; i < n; i++) {
			if (!g_ascii_isdigit(ids->tmdb[i]))
				abort();
		}
	}
	if (ids->imdb[0] != '\0') {
		gsize n = strlen(ids->imdb);

		if (n < 9 || n > 11 || strncmp(ids->imdb, "tt", 2) != 0 ||
		    g_strstr_len(name, (gssize)length, ids->imdb + 2) == NULL)
			abort();
	}
	if (ids->tvdb[0] != '\0') {
		gsize n = strlen(ids->tvdb);
		gsize i;

		if (n < 1 || n > 12 || ids->tvdb[0] == '0' || g_strstr_len(name, (gssize)length, ids->tvdb) == NULL)
			abort();
		for (i = 0; i < n; i++) {
			if (!g_ascii_isdigit(ids->tvdb[i]))
				abort();
		}
	}
	if ((ids->tmdb_conflict && ids->tmdb[0] != '\0') || (ids->imdb_conflict && ids->imdb[0] != '\0') ||
	    (ids->tvdb_conflict && ids->tvdb[0] != '\0'))
		abort();
}

static void
tokens(
	const gchar *name,
	gsize        length
){
	AxisIdentifyIds ids;
	AxisIdentifyIds again;
	g_autofree gchar *left = axis_identify_name_ids(name, (gssize)length, &ids);
	g_autofree gchar *twice = NULL;
	gsize n = strlen(left);

	check_ids(&ids, name, length);
	if (memchr(name, '\0', length) == NULL && n > length)
		abort();
	if (n > 0 && (left[0] == ' ' || left[n - 1] == ' ') && length <= AXIS_IDENTIFY_NAME_MAX &&
	    memchr(name, '\0', length) == NULL)
		abort();
	if (memchr(name, '\0', length) == NULL && length <= AXIS_IDENTIFY_NAME_MAX &&
	    g_utf8_validate(name, (gssize)length, NULL) && !g_utf8_validate(left, -1, NULL))
		abort();
	twice = axis_identify_name_ids(left, -1, &again);
	/* Every token went, and taking them out made none. */
	if (memchr(name, '\0', length) == NULL && length <= AXIS_IDENTIFY_NAME_MAX &&
	    (axis_identify_ids_any(&again) || axis_identify_ids_conflict(&again) || again.tvdb[0] != '\0' ||
	     again.tvdb_conflict || strcmp(twice, left) != 0))
		abort();
}

static AxisMetaTitle *
fixed(
	gint64       id,
	const gchar *title,
	gint         year,
	gint         runtime
){
	AxisMetaTitle *t = axis_meta_title_new(AXIS_META_KIND_FILM);

	t->id = id;
	t->title = g_strdup(title);
	t->year = year;
	t->runtime = runtime;
	return t;
}

static void
groups(
	const gchar *input,
	gsize        length
){
	g_autofree gchar *copy = g_strndup(input, length);
	g_auto(GStrv) parts = g_strsplit(copy, "\n", 18);
	g_autoptr(AxisIdentifyName) folder = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_name_free);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(AxisMetaTitle) a = fixed(1, "Hearth", 1995, 170);
	g_autoptr(AxisMetaTitle) b = fixed(2, "Hearth", 1986, 101);
	g_autofree gboolean *seen = NULL;
	const AxisMetaTitle *titles[2];
	guint folder_groups = 0;
	guint next = 0;
	guint i;
	guint k;

	if (parts[0] == NULL)
		return;
	if (parts[0][0] != '\0')
		folder = axis_identify_name_read(parts[0], 2026);
	for (i = 1; parts[i] != NULL && i <= 16; i++)
		g_ptr_array_add(names, axis_identify_name_read(parts[i], 2026));
	found = axis_identify_group(folder, (const AxisIdentifyName *const *)names->pdata, names->len);
	seen = g_new0(gboolean, names->len + 1);
	for (i = 0; i < found->len; i++) {
		const AxisIdentifyGroup *group = g_ptr_array_index(found, i);

		if (group->members->len == 0)
			abort();
		if (group->folder)
			folder_groups++;
		if (group->folder && folder == NULL)
			abort();
		for (k = 0; k < group->members->len; k++) {
			guint member = g_array_index(group->members, guint, k);

			if (member >= names->len || seen[member])
				abort();
			if (k > 0 && member <= g_array_index(group->members, guint, k - 1))
				abort();
			seen[member] = TRUE;
			next++;
		}
	}
	if (next != names->len || folder_groups > 1)
		abort();

	titles[0] = a;
	titles[1] = b;
	for (i = 0; i < found->len; i++) {
		const AxisIdentifyGroup *group = g_ptr_array_index(found, i);
		const AxisIdentifyName *file = g_ptr_array_index(names, g_array_index(group->members, guint, 0));
		g_autoptr(AxisIdentifyVerdict) v = NULL;
		AxisIdentifyEvidence e;

		memset(&e, 0, sizeof(e));
		axis_identify_read(group->folder ? folder : NULL, file, &e.reading);
		e.found = titles;
		e.n_found = 2;
		e.detail = a;
		e.duration_ms = (gint64)(length % 300) * 60000;
		e.percent = 10;
		e.minutes = 5;
		if (file->ids.tmdb[0] != '\0') {
			e.name_id.state = AXIS_IDENTIFY_ID_FOUND;
			e.name_id.title = b;
		} else if (file->ids.imdb[0] != '\0') {
			/* An .nfo's identifier, weighed against the names. */
			e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
			e.nfo_id.title = (length & 1) != 0 ? a : b;
		}
		v = axis_identify_decide(&e);
		/* An .nfo no name reads as caps the unit at likely, whatever the
		 * names find. */
		if (e.nfo_id.state == AXIS_IDENTIFY_ID_FOUND && v->confidence == AXIS_IDENTIFY_SURE &&
		    !axis_identify_corroborates(&e.reading, e.nfo_id.title))
			abort();
		if (v->candidates->len > AXIS_IDENTIFY_MAX_CANDIDATES || v->reasons->len == 0)
			abort();
		if (v->confidence != AXIS_IDENTIFY_UNKNOWN) {
			gboolean listed = FALSE;

			if (v->title == NULL)
				abort();
			for (k = 0; k < v->candidates->len; k++)
				listed |= g_ptr_array_index(v->candidates, k) == v->title;
			if (!listed)
				abort();
		}
		for (k = 0; k < v->reasons->len; k++) {
			const AxisIdentifyReason *r = g_ptr_array_index(v->reasons, k);

			if (r->code == NULL || r->code[0] == '\0' || r->text == NULL || r->text[0] == '\0' ||
			    !g_utf8_validate(r->text, -1, NULL))
				abort();
		}
		{
			g_autofree gchar *words = axis_identify_name_describe(file);

			if (words == NULL || !g_utf8_validate(words, -1, NULL))
				abort();
		}
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1)
		return 0;
	switch (data[0] % 2) {
	case 0:
		tokens((const gchar *)data + 1, size - 1);
		break;
	default:
		groups((const gchar *)data + 1, size - 1);
		break;
	}
	return 0;
}
