/*
 * axis-storage-path.c - checking the paths and names an administrator types
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GLib only, so that the fuzz target can be built from this file alone.
 */

#include "storage/axis-storage-path.h"

#include <string.h>

/* The first byte after the leading white space, and the length up to the
 * trailing white space.  @text is at most AXIS_STORAGE_INPUT_MAX long. */
static void
trim(
	const gchar  *text,
	const gchar **start,
	gsize        *length
){
	gsize end = strlen(text);
	gsize begin = 0;

	while (begin < end && g_ascii_isspace(text[begin]))
		begin++;
	while (end > begin && g_ascii_isspace(text[end - 1]))
		end--;

	*start = text + begin;
	*length = end - begin;
}

/* C0 controls, DEL and the C1 controls (U+0080 to U+009F, two bytes in
 * UTF-8).  @text is valid UTF-8. */
static gboolean
has_control(
	const gchar *text,
	gsize        length
){
	gsize i;

	for (i = 0; i < length; i++) {
		guchar c = (guchar)text[i];

		if (c < 0x20 || c == 0x7f)
			return TRUE;
		if (c == 0xc2 && i + 1 < length && (guchar)text[i + 1] >= 0x80 &&
		    (guchar)text[i + 1] <= 0x9f)
			return TRUE;
	}

	return FALSE;
}

/* Bounded strlen: whether @text is longer than @max bytes. */
static gboolean
longer_than(
	const gchar *text,
	gsize        max
){
	gsize i;

	for (i = 0; i <= max; i++) {
		if (text[i] == '\0')
			return FALSE;
	}

	return TRUE;
}

static gboolean
refuse(
	const gchar **problem,
	const gchar  *why
){
	if (problem != NULL)
		*problem = why;
	return FALSE;
}

gboolean
axis_storage_path_normalize(
	const gchar  *input,
	gboolean      absolute,
	gchar       **normalised,
	const gchar **problem
){
	const gchar *start;
	gsize length;
	gsize at;
	guint depth = 0;
	GString *out;

	if (normalised != NULL)
		*normalised = NULL;
	if (problem != NULL)
		*problem = NULL;

	if (input == NULL)
		input = "";
	if (longer_than(input, AXIS_STORAGE_INPUT_MAX))
		return refuse(problem, "The path is too long.");

	trim(input, &start, &length);
	if (!g_utf8_validate(start, (gssize)length, NULL))
		return refuse(problem, "The path is not valid text (UTF-8).");
	if (has_control(start, length))
		return refuse(problem, "The path contains control characters such as a line break.");

	if (absolute && length == 0)
		return refuse(problem, "A path is needed.");
	if (absolute && start[0] != '/')
		return refuse(problem, "The path must be absolute: it starts with /.");
	if (!absolute && length > 0 && start[0] == '/')
		return refuse(problem, "The path must be relative to the volume: it does not start with /.");

	out = g_string_sized_new(length + 1);
	at = 0;
	while (at < length) {
		gsize begin;
		gsize segment;

		while (at < length && start[at] == '/')
			at++;
		if (at == length)
			break;

		begin = at;
		while (at < length && start[at] != '/')
			at++;
		segment = at - begin;

		if ((segment == 1 && start[begin] == '.') ||
		    (segment == 2 && start[begin] == '.' && start[begin + 1] == '.')) {
			g_string_free(out, TRUE);
			return refuse(problem, "The path may not contain a . or .. segment.");
		}
		if (segment > AXIS_STORAGE_SEGMENT_MAX) {
			g_string_free(out, TRUE);
			return refuse(problem, "A name in the path is longer than 255 bytes.");
		}
		/* "disk /" would otherwise mean "disk " here and "disk" once
		 * trimmed again: one path, one meaning. */
		if (start[begin] == ' ' || start[at - 1] == ' ') {
			g_string_free(out, TRUE);
			return refuse(problem, "A name in the path begins or ends with a space.");
		}
		if (++depth > AXIS_STORAGE_DEPTH_MAX) {
			g_string_free(out, TRUE);
			return refuse(problem, "The path has too many levels.");
		}

		if (absolute || out->len > 0)
			g_string_append_c(out, '/');
		g_string_append_len(out, start + begin, (gssize)segment);
	}

	if (absolute && out->len == 0) {
		g_string_free(out, TRUE);
		return refuse(problem, "The top directory / cannot be a volume.");
	}
	if (out->len > AXIS_STORAGE_PATH_MAX) {
		g_string_free(out, TRUE);
		return refuse(problem, "The path is too long.");
	}

	if (normalised != NULL)
		*normalised = g_string_free(out, FALSE);
	else
		g_string_free(out, TRUE);
	return TRUE;
}

gboolean
axis_storage_path_normalize_exact(
	const gchar  *input,
	gboolean      absolute,
	gchar       **normalised,
	const gchar **problem
){
	gsize n;

	if (normalised != NULL)
		*normalised = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (input != NULL && !longer_than(input, AXIS_STORAGE_INPUT_MAX)) {
		n = strlen(input);
		if (n > 0 && (g_ascii_isspace(input[0]) || g_ascii_isspace(input[n - 1])))
			return refuse(problem, "The path begins or ends with white space, which would be another path trimmed.");
	}
	return axis_storage_path_normalize(input, absolute, normalised, problem);
}

gboolean
axis_storage_path_contains(
	const gchar *outer,
	const gchar *inner
){
	gsize length;

	g_return_val_if_fail(outer != NULL, FALSE);
	g_return_val_if_fail(inner != NULL, FALSE);

	length = strlen(outer);
	if (length == 0)
		return inner[0] != '/';
	if (strncmp(outer, inner, length) != 0)
		return FALSE;

	return inner[length] == '\0' || inner[length] == '/';
}

gboolean
axis_storage_label_normalize(
	const gchar  *input,
	gchar       **label,
	const gchar **problem
){
	const gchar *start;
	gsize length;

	if (label != NULL)
		*label = NULL;
	if (problem != NULL)
		*problem = NULL;

	if (input == NULL)
		input = "";
	if (longer_than(input, AXIS_STORAGE_INPUT_MAX))
		return refuse(problem, "The name is too long.");

	trim(input, &start, &length);
	if (length == 0)
		return refuse(problem, "A name is needed.");
	if (!g_utf8_validate(start, (gssize)length, NULL))
		return refuse(problem, "The name is not valid text (UTF-8).");
	if (has_control(start, length))
		return refuse(problem, "The name contains control characters such as a line break.");
	if (g_utf8_strlen(start, (gssize)length) > AXIS_STORAGE_LABEL_MAX_CHARS)
		return refuse(problem, "The name is longer than 64 characters.");

	if (label != NULL)
		*label = g_strndup(start, length);
	return TRUE;
}

gchar *
axis_storage_path_last_segment(const gchar *path)
{
	const gchar *slash;

	g_return_val_if_fail(path != NULL, NULL);

	slash = strrchr(path, '/');
	return g_strdup(slash != NULL ? slash + 1 : path);
}
