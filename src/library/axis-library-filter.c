/*
 * axis-library-filter.c - the file list's filters, sort and paging, parsed
 * strictly from a query string
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-library-filter.h"

#include <stdlib.h>
#include <string.h>

#include "library/axis-analysis-rules.h"
#include "transcode/axis-estimate.h"
#include "transcode/axis-profile.h"

G_DEFINE_QUARK(axis-library-filter-error-quark, axis_library_filter_error)

/* "eligible", "skipped", then the reasons' ids; filled once. */
static const gchar *outcomes[AXIS_ESTIMATE_N_REASONS + 2];

const gchar *const *
axis_library_filter_outcomes(void)
{
	static gsize ready = 0;

	if (g_once_init_enter(&ready)) {
		const gchar *const *ids = axis_estimate_reason_ids();
		guint n = 0;
		guint i;

		outcomes[n++] = "eligible";
		outcomes[n++] = "skipped";
		for (i = 0; ids[i] != NULL && n + 1 < G_N_ELEMENTS(outcomes); i++)
			outcomes[n++] = ids[i];
		outcomes[n] = NULL;
		g_once_init_leave(&ready, 1);
	}
	return outcomes;
}

gboolean
axis_library_filter_needs_estimator(const AxisLibraryFilter *filter)
{
	return filter->target != NULL || filter->outcome != NULL;
}

static const gchar *const types[] = {
	"movies", "tv", "music", "audiobooks", "books", "downloads", "other", "youtube", NULL
};

static const gchar *const kinds[] = {
	"video", "audio", "subtitle", "image", "text", "other", "symlink", NULL
};
static const gchar *const resolutions[] = { "sd", "720p", "1080p", "2160p", "other", NULL };
static const gchar *const hdr_classes[] = {
	"sdr", "hlg", "hdr10", "hdr10plus", "dolby_vision", "uncertain", NULL
};
static const gchar *const probe_states[] = { "ok", "failed", "timeout", "none", NULL };
static const gchar *const states[] = { "present", "suspected", "missing", NULL };
static const gchar *const pinned_values[] = { "yes", "no", NULL };
static const gchar *const directions[] = { "asc", "desc", NULL };

static const gchar *const sort_names[] = { "name", "size", "modified", "duration", "bitrate" };
static const gchar *const sort_labels[] = { "Name", "Size", "Modified", "Duration", "Bitrate" };

G_STATIC_ASSERT(G_N_ELEMENTS(sort_names) == AXIS_LIBRARY_SORT_COUNT);
G_STATIC_ASSERT(G_N_ELEMENTS(sort_labels) == AXIS_LIBRARY_SORT_COUNT);

const gchar *const *
axis_library_filter_kinds(void)
{
	return kinds;
}

const gchar *const *
axis_library_filter_resolutions(void)
{
	return resolutions;
}

const gchar *const *
axis_library_filter_hdr_classes(void)
{
	return hdr_classes;
}

const gchar *const *
axis_library_filter_probe_states(void)
{
	return probe_states;
}

const gchar *const *
axis_library_filter_states(void)
{
	return states;
}

const gchar *const *
axis_library_filter_types(void)
{
	return types;
}

const gchar *
axis_library_sort_to_string(AxisLibrarySort sort)
{
	return (guint)sort < AXIS_LIBRARY_SORT_COUNT ? sort_names[sort] : sort_names[0];
}

const gchar *
axis_library_sort_label(AxisLibrarySort sort)
{
	return (guint)sort < AXIS_LIBRARY_SORT_COUNT ? sort_labels[sort] : sort_labels[0];
}

gboolean
axis_library_sort_default_descending(AxisLibrarySort sort)
{
	return sort != AXIS_LIBRARY_SORT_NAME;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

void
axis_library_filter_init(AxisLibraryFilter *filter)
{
	memset(filter, 0, sizeof(*filter));
	filter->pinned = -1;
	filter->changed = -1;
	filter->state = states[0];
	filter->min_mib = -1;
	filter->max_mib = -1;
	filter->min_bpp = -1;
	filter->max_bpp = -1;
	filter->sort = AXIS_LIBRARY_SORT_NAME;
	filter->descending = FALSE;
}

void
axis_library_filter_clear(AxisLibraryFilter *filter)
{
	g_free(filter->container);
	g_free(filter->video_codec);
	g_free(filter->audio_codec);
	g_free(filter->name);
	g_free(filter->ids);
	g_free(filter->target);
	axis_library_filter_init(filter);
}

void
axis_library_filter_copy(
	AxisLibraryFilter       *to,
	const AxisLibraryFilter *from
){
	*to = *from;
	to->container = g_strdup(from->container);
	to->video_codec = g_strdup(from->video_codec);
	to->audio_codec = g_strdup(from->audio_codec);
	to->name = g_strdup(from->name);
	to->ids = from->ids != NULL ? g_memdup2(from->ids, from->n_ids * sizeof(gint64)) : NULL;
	to->target = g_strdup(from->target);
}

/* ── Parsing ────────────────────────────────────────────────────── */

static void
fail(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_LIBRARY_FILTER_ERROR, AXIS_LIBRARY_FILTER_ERROR_INVALID, message);
}

static gint
hex_value(gchar c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Form decoding of @length bytes: '+' is a space, %XX a byte.  NULL for a
 * malformed escape or a NUL. */
static gchar *
decode(
	const gchar *text,
	gsize        length
){
	GString *out = g_string_sized_new(length);
	gsize i;

	for (i = 0; i < length; i++) {
		if (text[i] == '%') {
			gint high;
			gint low;

			if (i + 2 >= length) {
				g_string_free(out, TRUE);
				return NULL;
			}
			high = hex_value(text[i + 1]);
			low = hex_value(text[i + 2]);
			if (high < 0 || low < 0 || (high == 0 && low == 0)) {
				g_string_free(out, TRUE);
				return NULL;
			}
			g_string_append_c(out, (gchar)(high * 16 + low));
			i += 2;
		} else if (text[i] == '+') {
			g_string_append_c(out, ' ');
		} else if (text[i] == '\0') {
			g_string_free(out, TRUE);
			return NULL;
		} else {
			g_string_append_c(out, text[i]);
		}
	}
	return g_string_free(out, FALSE);
}

/* A whole number without sign or leading zero, at most 18 digits. */
static gboolean
parse_number(
	const gchar *text,
	gint64      *value
){
	gsize length = strlen(text);
	gint64 result = 0;
	gsize i;

	if (length == 0 || length > 18 || (text[0] == '0' && length > 1))
		return FALSE;
	for (i = 0; i < length; i++) {
		if (text[i] < '0' || text[i] > '9')
			return FALSE;
		result = result * 10 + (text[i] - '0');
	}
	*value = result;
	return TRUE;
}

static const gchar *
parse_enum(
	const gchar *const *values,
	const gchar        *text
){
	guint i;

	for (i = 0; values[i] != NULL; i++) {
		if (strcmp(values[i], text) == 0)
			return values[i];
	}
	return NULL;
}

/* Lower-case letters, digits and . , _ + - */
static gboolean
token_valid(const gchar *text)
{
	gsize length = strlen(text);
	gsize i;

	if (length == 0 || length > AXIS_LIBRARY_FILTER_TOKEN_MAX)
		return FALSE;
	for (i = 0; i < length; i++) {
		gchar c = text[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '_' ||
		      c == '+' || c == '-'))
			return FALSE;
	}
	return TRUE;
}

/* Text a person typed: valid UTF-8, no control, format (direction marks,
 * zero-width joiners) or surrogate characters, 3 to 64 characters (a
 * shorter text cannot use the trigram index, so it would read every
 * name). */
static gboolean
name_valid(const gchar *text)
{
	const gchar *p;
	glong chars;

	if (!g_utf8_validate(text, -1, NULL))
		return FALSE;
	chars = g_utf8_strlen(text, -1);
	if (chars < AXIS_LIBRARY_FILTER_NAME_MIN_CHARS || chars > AXIS_LIBRARY_FILTER_NAME_CHARS)
		return FALSE;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		switch (g_unichar_type(g_utf8_get_char(p))) {
		case G_UNICODE_CONTROL:
		case G_UNICODE_FORMAT:
		case G_UNICODE_SURROGATE:
		case G_UNICODE_UNASSIGNED:
			return FALSE;
		default:
			break;
		}
	}
	return TRUE;
}

typedef enum
{
	KEY_ROOT,
	KEY_KIND,
	KEY_CONTAINER,
	KEY_VCODEC,
	KEY_RES,
	KEY_HDR,
	KEY_ACODEC,
	KEY_PINNED,
	KEY_PROBE,
	KEY_STATE,
	KEY_MIN,
	KEY_MAX,
	KEY_NAME,
	KEY_CHANGED,
	KEY_VOLUME,
	KEY_TYPE,
	KEY_UNDER,
	KEY_IDS,
	KEY_MIN_BPP,
	KEY_MAX_BPP,
	KEY_LEFTOVER,
	KEY_TARGET,
	KEY_REV,
	KEY_OUTCOME,
	KEY_SORT,
	KEY_DIR,
	KEY_AFTER,
	KEY_SINCE,
	KEY_COUNT
} Key;

static const gchar *const key_names[KEY_COUNT] = {
	"root", "kind", "container", "vcodec", "res", "hdr", "acodec", "pinned", "probe", "state",
	"min_mib", "max_mib", "name", "changed", "volume", "type", "under", "ids", "min_bpp", "max_bpp", "leftover",
	"target", "rev", "outcome", "sort", "dir", "after", "since"
};

static gint
compare_ids(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : x > y;
}

/* "12,7,40": up to AXIS_LIBRARY_FILTER_MAX_IDS different positive numbers.
 * Stored in ascending order, so that there is one spelling per list. */
static gboolean
parse_ids(
	const gchar  *text,
	gint64      **ids,
	guint        *n_ids
){
	g_auto(GStrv) parts = g_strsplit(text, ",", -1);
	g_autofree gint64 *list = NULL;
	guint n = g_strv_length(parts);
	guint i;

	if (n == 0 || n > AXIS_LIBRARY_FILTER_MAX_IDS)
		return FALSE;
	list = g_new(gint64, n);
	for (i = 0; i < n; i++) {
		guint j;

		if (!parse_number(parts[i], &list[i]) || list[i] < 1)
			return FALSE;
		for (j = 0; j < i; j++) {
			if (list[j] == list[i])
				return FALSE;
		}
	}
	qsort(list, n, sizeof(gint64), compare_ids);
	*ids = g_steal_pointer(&list);
	*n_ids = n;
	return TRUE;
}

static gboolean
key_lookup(
	const gchar *name,
	Key         *key
){
	guint i;

	for (i = 0; i < KEY_COUNT; i++) {
		if (strcmp(key_names[i], name) == 0) {
			*key = (Key)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* Sets one parameter; the value is non-empty and decoded. */
static gboolean
apply(
	AxisLibraryFilter  *filter,
	Key                 key,
	const gchar        *value,
	gboolean           *dir_set,
	gboolean           *descending,
	GError            **error
){
	gint64 number = 0;
	const gchar *found;

	switch (key) {
	case KEY_ROOT:
		if (!parse_number(value, &number) || number < 1) {
			fail(error, "The library folder is not a number.");
			return FALSE;
		}
		filter->root_id = number;
		return TRUE;
	case KEY_KIND:
		found = parse_enum(kinds, value);
		if (found == NULL) {
			fail(error, "The type of file is not one this list knows.");
			return FALSE;
		}
		filter->kind = found;
		return TRUE;
	case KEY_CONTAINER:
		if (!token_valid(value)) {
			fail(error, "The container is not a container name.");
			return FALSE;
		}
		filter->container = g_strdup(value);
		return TRUE;
	case KEY_VCODEC:
		if (!token_valid(value)) {
			fail(error, "The video codec is not a codec name.");
			return FALSE;
		}
		filter->video_codec = g_strdup(value);
		return TRUE;
	case KEY_RES:
		found = parse_enum(resolutions, value);
		if (found == NULL) {
			fail(error, "The resolution class is not one this list knows.");
			return FALSE;
		}
		filter->resolution = found;
		return TRUE;
	case KEY_HDR:
		found = parse_enum(hdr_classes, value);
		if (found == NULL) {
			fail(error, "The HDR format is not one this list knows.");
			return FALSE;
		}
		filter->hdr = found;
		return TRUE;
	case KEY_ACODEC:
		if (!token_valid(value)) {
			fail(error, "The audio codec is not a codec name.");
			return FALSE;
		}
		filter->audio_codec = g_strdup(value);
		return TRUE;
	case KEY_PINNED:
		found = parse_enum(pinned_values, value);
		if (found == NULL) {
			fail(error, "Pinned is either yes or no.");
			return FALSE;
		}
		filter->pinned = strcmp(found, "yes") == 0 ? 1 : 0;
		return TRUE;
	case KEY_PROBE:
		found = parse_enum(probe_states, value);
		if (found == NULL) {
			fail(error, "The file reading state is not one this list knows.");
			return FALSE;
		}
		filter->probe = found;
		return TRUE;
	case KEY_STATE:
		found = parse_enum(states, value);
		if (found == NULL) {
			fail(error, "The scan state is not one this list knows.");
			return FALSE;
		}
		filter->state = found;
		return TRUE;
	case KEY_MIN:
	case KEY_MAX:
		if (!parse_number(value, &number) || number > 999999999) {
			fail(error, "A size is a whole number of MiB.");
			return FALSE;
		}
		if (key == KEY_MIN)
			filter->min_mib = number;
		else
			filter->max_mib = number;
		return TRUE;
	case KEY_NAME:
		if (!name_valid(value)) {
			fail(error, "The name to look for is not valid text of 3 to 64 characters.");
			return FALSE;
		}
		filter->name = g_strdup(value);
		return TRUE;
	case KEY_CHANGED:
		found = parse_enum(pinned_values, value);
		if (found == NULL) {
			fail(error, "Changed is either yes or no.");
			return FALSE;
		}
		filter->changed = strcmp(found, "yes") == 0 ? 1 : 0;
		return TRUE;
	case KEY_VOLUME:
	case KEY_UNDER:
		if (!parse_number(value, &number) || number < 1) {
			fail(error, key == KEY_VOLUME ? "The disk is not a number." : "The folder is not a number.");
			return FALSE;
		}
		if (key == KEY_VOLUME)
			filter->volume_id = number;
		else
			filter->dir_id = number;
		return TRUE;
	case KEY_TYPE:
		found = parse_enum(types, value);
		if (found == NULL) {
			fail(error, "The type of library folder is not one this list knows.");
			return FALSE;
		}
		filter->type = found;
		return TRUE;
	case KEY_IDS:
		if (!parse_ids(value, &filter->ids, &filter->n_ids)) {
			fail(error, "The files are not a list of up to 24 different numbers.");
			return FALSE;
		}
		return TRUE;
	case KEY_MIN_BPP:
	case KEY_MAX_BPP:
		if (!parse_number(value, &number) || number > AXIS_LIBRARY_FILTER_MAX_BPP) {
			fail(error, "Bits per pixel are a whole number of thousandths.");
			return FALSE;
		}
		if (key == KEY_MIN_BPP)
			filter->min_bpp = number;
		else
			filter->max_bpp = number;
		return TRUE;
	case KEY_LEFTOVER:
		found = parse_enum(axis_leftover_filter_values(), value);
		if (found == NULL) {
			fail(error, "The leftover rule is not one this list knows.");
			return FALSE;
		}
		filter->leftover = found;
		return TRUE;
	case KEY_TARGET:
		if (!axis_profile_slug_valid(value)) {
			fail(error, "The conversion profile is not one Axis has.");
			return FALSE;
		}
		filter->target = g_strdup(value);
		return TRUE;
	case KEY_REV:
		if (!parse_number(value, &number) || number < 1 || number > AXIS_LIBRARY_FILTER_MAX_REV) {
			fail(error, "The version is not a number.");
			return FALSE;
		}
		filter->revision = (gint)number;
		return TRUE;
	case KEY_OUTCOME:
		found = parse_enum(axis_library_filter_outcomes(), value);
		if (found == NULL) {
			fail(error, "The outcome is not one this list knows.");
			return FALSE;
		}
		filter->outcome = found;
		return TRUE;
	case KEY_SORT: {
		guint i;

		for (i = 0; i < AXIS_LIBRARY_SORT_COUNT; i++) {
			if (strcmp(sort_names[i], value) == 0) {
				filter->sort = (AxisLibrarySort)i;
				return TRUE;
			}
		}
		fail(error, "The list cannot be sorted that way.");
		return FALSE;
	}
	case KEY_DIR:
		found = parse_enum(directions, value);
		if (found == NULL) {
			fail(error, "The order is either ascending or descending.");
			return FALSE;
		}
		*dir_set = TRUE;
		*descending = strcmp(found, "desc") == 0;
		return TRUE;
	case KEY_AFTER:
	case KEY_SINCE:
		if (!parse_number(value, &number) || number < 1) {
			fail(error, "The position in the list is not a number.");
			return FALSE;
		}
		if (key == KEY_AFTER)
			filter->after = number;
		else
			filter->since = number;
		return TRUE;
	case KEY_COUNT:
	default:
		break;
	}
	fail(error, "The filter is not valid.");
	return FALSE;
}

gboolean
axis_library_filter_parse(
	const gchar        *query,
	AxisLibraryFilter  *filter,
	GError            **error
){
	gboolean seen[KEY_COUNT];
	gboolean dir_set = FALSE;
	gboolean descending = FALSE;
	const gchar *p;
	guint pairs = 0;

	axis_library_filter_init(filter);
	memset(seen, 0, sizeof(seen));
	if (query == NULL || query[0] == '\0')
		return TRUE;
	if (strlen(query) > AXIS_LIBRARY_FILTER_MAX_QUERY) {
		fail(error, "The filter is too long.");
		return FALSE;
	}

	p = query;
	for (;;) {
		const gchar *end = strchr(p, '&');
		const gchar *equals;
		gsize segment = end != NULL ? (gsize)(end - p) : strlen(p);
		g_autofree gchar *name = NULL;
		g_autofree gchar *value = NULL;
		Key key = KEY_COUNT;

		if (segment > 0) {
			if (++pairs > AXIS_LIBRARY_FILTER_MAX_PAIRS) {
				fail(error, "The filter has too many parts.");
				goto bad;
			}
			equals = memchr(p, '=', segment);
			if (equals == NULL) {
				/* A part that is not name=value. */
				fail(error, "The filter is not one this list knows.");
				goto bad;
			}
			name = decode(p, (gsize)(equals - p));
			value = decode(equals + 1, segment - (gsize)(equals - p) - 1);
			if (name == NULL || value == NULL || !key_lookup(name, &key)) {
				fail(error, "The filter is not one this list knows.");
				goto bad;
			}
			if (seen[key]) {
				fail(error, "A filter is given twice.");
				goto bad;
			}
			seen[key] = TRUE;
			if (value[0] != '\0' && !apply(filter, key, value, &dir_set, &descending, error))
				goto bad;
		}
		if (end == NULL)
			break;
		p = end + 1;
	}

	if (filter->min_mib >= 0 && filter->max_mib >= 0 && filter->min_mib > filter->max_mib) {
		fail(error, "The smallest size is larger than the largest.");
		goto bad;
	}
	if (filter->min_bpp >= 0 && filter->max_bpp >= 0 && filter->min_bpp >= filter->max_bpp) {
		fail(error, "The lowest bits per pixel is not below the highest.");
		goto bad;
	}
	if (filter->outcome != NULL && filter->target == NULL) {
		fail(error, "An outcome needs a conversion profile.");
		goto bad;
	}
	if (filter->revision > 0 && filter->target == NULL) {
		fail(error, "A version needs a conversion profile.");
		goto bad;
	}
	filter->descending = dir_set ? descending : axis_library_sort_default_descending(filter->sort);
	return TRUE;

bad:
	axis_library_filter_clear(filter);
	return FALSE;
}

/* ── Writing ────────────────────────────────────────────────────── */

static void
append(
	GString     *out,
	const gchar *key,
	const gchar *value
){
	g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

	if (out->len > 0)
		g_string_append_c(out, '&');
	g_string_append_printf(out, "%s=%s", key, escaped);
}

static void
append_number(
	GString     *out,
	const gchar *key,
	gint64       value
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);

	append(out, key, text);
}

gchar *
axis_library_filter_to_query(
	const AxisLibraryFilter *filter,
	gboolean                 with_cursor
){
	GString *out = g_string_new(NULL);

	if (filter->root_id > 0)
		append_number(out, "root", filter->root_id);
	if (filter->kind != NULL)
		append(out, "kind", filter->kind);
	if (filter->container != NULL)
		append(out, "container", filter->container);
	if (filter->video_codec != NULL)
		append(out, "vcodec", filter->video_codec);
	if (filter->resolution != NULL)
		append(out, "res", filter->resolution);
	if (filter->hdr != NULL)
		append(out, "hdr", filter->hdr);
	if (filter->audio_codec != NULL)
		append(out, "acodec", filter->audio_codec);
	if (filter->pinned >= 0)
		append(out, "pinned", filter->pinned == 1 ? "yes" : "no");
	if (filter->probe != NULL)
		append(out, "probe", filter->probe);
	if (strcmp(filter->state, states[0]) != 0)
		append(out, "state", filter->state);
	if (filter->min_mib >= 0)
		append_number(out, "min_mib", filter->min_mib);
	if (filter->max_mib >= 0)
		append_number(out, "max_mib", filter->max_mib);
	if (filter->name != NULL)
		append(out, "name", filter->name);
	if (filter->changed >= 0)
		append(out, "changed", filter->changed == 1 ? "yes" : "no");
	if (filter->volume_id > 0)
		append_number(out, "volume", filter->volume_id);
	if (filter->type != NULL)
		append(out, "type", filter->type);
	if (filter->dir_id > 0)
		append_number(out, "under", filter->dir_id);
	if (filter->n_ids > 0) {
		GString *list = g_string_new(NULL);
		guint i;

		for (i = 0; i < filter->n_ids; i++)
			g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", filter->ids[i]);
		append(out, "ids", list->str);
		g_string_free(list, TRUE);
	}
	if (filter->min_bpp >= 0)
		append_number(out, "min_bpp", filter->min_bpp);
	if (filter->max_bpp >= 0)
		append_number(out, "max_bpp", filter->max_bpp);
	if (filter->leftover != NULL)
		append(out, "leftover", filter->leftover);
	if (filter->target != NULL)
		append(out, "target", filter->target);
	if (filter->target != NULL && filter->revision > 0)
		append_number(out, "rev", filter->revision);
	if (filter->outcome != NULL)
		append(out, "outcome", filter->outcome);
	if (filter->sort != AXIS_LIBRARY_SORT_NAME)
		append(out, "sort", axis_library_sort_to_string(filter->sort));
	if (filter->descending != axis_library_sort_default_descending(filter->sort))
		append(out, "dir", filter->descending ? "desc" : "asc");
	if (with_cursor && filter->after > 0)
		append_number(out, "after", filter->after);
	if (with_cursor && filter->since > 0)
		append_number(out, "since", filter->since);

	return g_string_free(out, FALSE);
}

gboolean
axis_library_filter_is_narrowed(const AxisLibraryFilter *filter)
{
	return filter->root_id > 0 || filter->kind != NULL || filter->container != NULL ||
	       filter->video_codec != NULL || filter->resolution != NULL || filter->hdr != NULL ||
	       filter->audio_codec != NULL || filter->pinned >= 0 || filter->probe != NULL ||
	       strcmp(filter->state, states[0]) != 0 || filter->min_mib >= 0 || filter->max_mib >= 0 ||
	       filter->name != NULL || filter->changed >= 0 || filter->volume_id > 0 || filter->type != NULL || filter->dir_id > 0 ||
	       filter->n_ids > 0 || filter->min_bpp >= 0 || filter->max_bpp >= 0 || filter->leftover != NULL ||
	       filter->target != NULL || filter->outcome != NULL;
}
