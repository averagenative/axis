/*
 * axis-import-name.c - the names Axis gives what it imports, and what it
 * reads from the names it finds
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "import/axis-import-name.h"

#include <string.h>

/* ── Layouts ────────────────────────────────────────────────────── */

static const gchar *const layout_names[] = { "tmdb", "jellyfin", "plain" };

const gchar *
axis_import_layout_name(AxisImportLayout layout)
{
	return (guint)layout < G_N_ELEMENTS(layout_names) ? layout_names[layout] : "tmdb";
}

gboolean
axis_import_layout_from_name(
	const gchar      *name,
	AxisImportLayout *layout
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(layout_names); i++) {
		if (strcmp(name, layout_names[i]) == 0) {
			*layout = (AxisImportLayout)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── The character rule ─────────────────────────────────────────── */

static gboolean
is_direction(gunichar c)
{
	return c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
		(c >= 0x2066 && c <= 0x2069);
}

/* Dots and spaces off both ends, in place. */
static void
trim_ends(GString *text)
{
	gsize start = 0;

	while (start < text->len && (text->str[start] == ' ' || text->str[start] == '.'))
		start++;
	if (start > 0)
		g_string_erase(text, 0, (gssize)start);
	while (text->len > 0 && (text->str[text->len - 1] == ' ' || text->str[text->len - 1] == '.'))
		g_string_truncate(text, text->len - 1);
}

gchar *
axis_import_name_clean(
	const gchar *text,
	gssize       length
){
	GString *out;
	const gchar *p;
	gsize left;

	if (text == NULL)
		return g_strdup("");
	left = length < 0 ? strlen(text) : (gsize)length;
	out = g_string_sized_new(left + 8);
	p = text;
	while (left > 0) {
		gunichar c;
		gsize n;

		if (*p == '\0') {
			/* A NUL inside the text is dropped, as a control character. */
			p++;
			left--;
			continue;
		}
		c = g_utf8_get_char_validated(p, (gssize)left);
		if (c == (gunichar)-1 || c == (gunichar)-2) {
			/* Not UTF-8: U+FFFD, a byte at a time. */
			g_string_append(out, "\xEF\xBF\xBD");
			p++;
			left--;
			continue;
		}
		if (c == 0) {
			p++;
			left--;
			continue;
		}
		n = (gsize)(g_utf8_next_char(p) - p);
		if (n == 0 || n > left)
			n = 1;
		if (c == '/' || c == '\\')
			g_string_append_c(out, '-');
		else if (c == ':')
			g_string_append(out, " -");
		else if (!g_unichar_iscntrl(c) && !is_direction(c))
			g_string_append_len(out, p, (gssize)n);
		p += n;
		left -= n;
	}
	trim_ends(out);
	return g_string_free(out, FALSE);
}

gboolean
axis_import_name_valid(const gchar *name)
{
	gsize length;
	const gchar *p;

	if (name == NULL)
		return FALSE;
	length = strlen(name);
	if (length == 0 || length > AXIS_IMPORT_COMPONENT_MAX || !g_utf8_validate(name, (gssize)length, NULL))
		return FALSE;
	if (name[0] == '.' || name[length - 1] == '.' || name[length - 1] == ' ')
		return FALSE;
	for (p = name; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c == '/' || g_unichar_iscntrl(c) || is_direction(c))
			return FALSE;
	}
	return TRUE;
}

/* The longest prefix of @text (valid UTF-8) of at most @max bytes that ends
 * on a character boundary and does not leave a combining mark without the
 * letter it belongs to. */
static gsize
cut_point(
	const gchar *text,
	gsize        length,
	gsize        max
){
	gsize cut;

	if (length <= max)
		return length;
	cut = max;
	while (cut > 0 && ((guchar)text[cut] & 0xC0) == 0x80)
		cut--;
	/* A mark cut off its letter takes the letter with it. */
	while (cut > 0 && g_unichar_ismark(g_utf8_get_char(text + cut))) {
		cut--;
		while (cut > 0 && ((guchar)text[cut] & 0xC0) == 0x80)
			cut--;
	}
	return cut;
}

/* @text cut to @max bytes, its new end trimmed of dots and spaces. */
static void
cut_to(
	GString *text,
	gsize    max
){
	gsize cut = cut_point(text->str, text->len, max);

	g_string_truncate(text, cut);
	while (text->len > 0 && (text->str[text->len - 1] == ' ' || text->str[text->len - 1] == '.'))
		g_string_truncate(text, text->len - 1);
}

/* ── The format ─────────────────────────────────────────────────── */

typedef enum
{
	TOKEN_EDITION,
	TOKEN_QUALITY,
	TOKEN_HDR,
	TOKEN_AUDIO,
	TOKEN_CODEC,
	TOKEN_GROUP,
	TOKEN_AIRDATE,
	N_TOKENS
} TokenId;

static const gchar *const token_names[N_TOKENS] = { "edition", "quality", "hdr", "audio", "codec", "group",
	"airdate" };

typedef struct
{
	gboolean  token;
	TokenId   id;
	gchar    *text;	/* literal text, or the token's prefix */
	gchar    *suffix;
} Segment;

static void
segment_free(gpointer data)
{
	Segment *segment = data;

	g_free(segment->text);
	g_free(segment->suffix);
	g_free(segment);
}

static gboolean
affix_char(gchar c)
{
	return c == ' ' || c == '-' || c == '_' || c == '.' || c == '[' || c == ']' || c == '(' || c == ')';
}

static gboolean
refuse(
	gchar       **reason,
	const gchar  *text
){
	if (reason != NULL)
		*reason = g_strdup(text);
	return FALSE;
}

/* The segments of a valid format, or %NULL with the reason. */
static GPtrArray *
format_parse(
	const gchar  *format,
	gchar       **reason
){
	g_autoptr(GPtrArray) segments = g_ptr_array_new_with_free_func(segment_free);
	g_autoptr(GString) literal = g_string_new(NULL);
	gsize length;
	const gchar *p;

	if (reason != NULL)
		*reason = NULL;
	if (format == NULL)
		return g_steal_pointer(&segments);
	length = strlen(format);
	if (length > AXIS_IMPORT_FORMAT_MAX) {
		refuse(reason, "The format is too long: at most 200 bytes.");
		return NULL;
	}
	if (!g_utf8_validate(format, (gssize)length, NULL)) {
		refuse(reason, "The format is not valid UTF-8 text.");
		return NULL;
	}
	for (p = format; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_iscntrl(c) || is_direction(c)) {
			refuse(reason, "The format cannot contain control or direction characters.");
			return NULL;
		}
		if (c == '/' || c == '\\') {
			refuse(reason, "The format cannot contain '/' or '\\': a file name cannot.");
			return NULL;
		}
	}

	p = format;
	while (*p != '\0') {
		const gchar *start;
		const gchar *name;
		gsize name_length;
		Segment *segment;
		guint id;

		if (*p == '}') {
			refuse(reason, "The format has a '}' that closes no token.");
			return NULL;
		}
		if (*p != '{') {
			g_string_append_c(literal, *p);
			p++;
			continue;
		}
		/* A token. */
		if (literal->len > 0) {
			segment = g_new0(Segment, 1);
			segment->text = g_strdup(literal->str);
			g_ptr_array_add(segments, segment);
			g_string_truncate(literal, 0);
		}
		p++;
		start = p;
		while (affix_char(*p))
			p++;
		name = p;
		while (*p >= 'a' && *p <= 'z')
			p++;
		name_length = (gsize)(p - name);
		segment = g_new0(Segment, 1);
		segment->token = TRUE;
		segment->text = g_strndup(start, (gsize)(name - start));
		start = p;
		while (affix_char(*p))
			p++;
		segment->suffix = g_strndup(start, (gsize)(p - start));
		if (*p != '}') {
			segment_free(segment);
			refuse(reason, *p == '\0' ? "The format has a '{' that is not closed." :
				"A token is written as '{', a name, '}' (with spaces, '-', '_', '.', brackets or parentheses "
				"around the name if wanted).");
			return NULL;
		}
		p++;
		for (id = 0; id < N_TOKENS; id++) {
			if (strlen(token_names[id]) == name_length && strncmp(token_names[id], name, name_length) == 0)
				break;
		}
		if (id == N_TOKENS) {
			segment_free(segment);
			refuse(reason, "The format names a token that does not exist: the tokens are edition, quality, hdr, "
				"audio, codec, group and airdate (an episode's).");
			return NULL;
		}
		segment->id = (TokenId)id;
		g_ptr_array_add(segments, segment);
	}
	if (literal->len > 0) {
		Segment *segment = g_new0(Segment, 1);

		segment->text = g_strdup(literal->str);
		g_ptr_array_add(segments, segment);
	}
	return g_steal_pointer(&segments);
}

gboolean
axis_import_format_check(
	const gchar  *format,
	gchar       **reason
){
	g_autoptr(GPtrArray) segments = format_parse(format, reason);

	return segments != NULL;
}

/* The format with the tokens of @values that @include (one flag per
 * segment) keeps, its runs of spaces made one, its end trimmed. */
static gchar *
format_render(
	GPtrArray      *segments,
	gchar         **values,
	const gboolean *include
){
	g_autoptr(GString) out = g_string_new(NULL);
	GString *collapsed;
	guint i;

	for (i = 0; i < segments->len; i++) {
		const Segment *segment = g_ptr_array_index(segments, i);

		if (!segment->token) {
			g_string_append(out, segment->text);
			continue;
		}
		if (!include[i] || values[segment->id] == NULL || values[segment->id][0] == '\0')
			continue;
		g_string_append(out, segment->text);
		g_string_append(out, values[segment->id]);
		g_string_append(out, segment->suffix);
	}
	collapsed = g_string_sized_new(out->len);
	for (i = 0; i < out->len; i++) {
		if (out->str[i] == ' ' && collapsed->len > 0 && collapsed->str[collapsed->len - 1] == ' ')
			continue;
		g_string_append_c(collapsed, out->str[i]);
	}
	while (collapsed->len > 0 && (collapsed->str[collapsed->len - 1] == ' ' ||
	                              collapsed->str[collapsed->len - 1] == '.'))
		g_string_truncate(collapsed, collapsed->len - 1);
	return g_string_free(collapsed, FALSE);
}

/* ── Building a name ────────────────────────────────────────────── */

void
axis_import_name_clear(AxisImportName *name)
{
	if (name == NULL)
		return;
	g_clear_pointer(&name->folder, g_free);
	g_clear_pointer(&name->season_folder, g_free);
	g_clear_pointer(&name->file, g_free);
	g_clear_pointer(&name->stem, g_free);
	name->shortened = FALSE;
}

static gboolean
all_digits(
	const gchar *text,
	gsize        min,
	gsize        max
){
	gsize n = text != NULL ? strlen(text) : 0;
	gsize i;

	if (n < min || n > max)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

static gchar *
identifier_text(const AxisImportNameInput *input)
{
	gboolean tmdb = all_digits(input->tmdb_id, 1, 12);
	gboolean imdb = input->imdb_id != NULL && g_str_has_prefix(input->imdb_id, "tt") &&
		all_digits(input->imdb_id + 2, 1, 12);
	gboolean tvdb = input->episode && all_digits(input->tvdb_id, 1, 12);

	switch (input->layout) {
	case AXIS_IMPORT_LAYOUT_PLAIN:
		return NULL;
	case AXIS_IMPORT_LAYOUT_JELLYFIN:
		if (tvdb)
			return g_strdup_printf("[tvdbid-%s]", input->tvdb_id);
		if (tmdb)
			return g_strdup_printf("[tmdbid-%s]", input->tmdb_id);
		if (imdb)
			return g_strdup_printf("[imdbid-%s]", input->imdb_id);
		return NULL;
	default:
		if (tvdb)
			return g_strdup_printf("{tvdb-%s}", input->tvdb_id);
		if (tmdb)
			return g_strdup_printf("{tmdb-%s}", input->tmdb_id);
		if (imdb)
			return g_strdup_printf("{imdb-%s}", input->imdb_id);
		return NULL;
	}
}

/* ".mkv" from what was given: a dot, then 1 to 16 ASCII letters or digits,
 * lower-cased; %NULL when it is not one. */
static gchar *
extension_of(const gchar *text)
{
	gsize n = text != NULL ? strlen(text) : 0;
	gsize i;

	if (n < 2 || n > 17 || text[0] != '.')
		return NULL;
	for (i = 1; i < n; i++) {
		if (!g_ascii_isalnum(text[i]))
			return NULL;
	}
	return g_ascii_strdown(text, (gssize)n);
}

/* A value for a token: the character rule, and nothing left is nothing. */
static gchar *
value_of(const gchar *text)
{
	gchar *clean;

	if (text == NULL)
		return NULL;
	clean = axis_import_name_clean(text, -1);
	if (clean[0] == '\0') {
		g_free(clean);
		return NULL;
	}
	return clean;
}

static gchar *
edition_value(const gchar *text)
{
	g_autofree gchar *clean = value_of(text);
	g_autoptr(GString) out = NULL;
	const gchar *p;
	guint n = 0;

	if (clean == NULL)
		return NULL;
	out = g_string_new(NULL);
	for (p = clean; *p != '\0' && n < AXIS_IMPORT_EDITION_CHARS; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		/* Braces would end Plex's mark early. */
		if (c == '{' || c == '}')
			continue;
		g_string_append_unichar(out, c);
		n++;
	}
	while (out->len > 0 && (out->str[out->len - 1] == ' ' || out->str[out->len - 1] == '.'))
		g_string_truncate(out, out->len - 1);
	if (out->len == 0)
		return NULL;
	return g_strdup_printf("{edition-%s}", out->str);
}

/* "S01E02", "S01E02-E03" (a run), "S01E02E05" (not one). */
static gchar *
episode_mark(
	gint        season,
	const gint *episodes,
	guint       n
){
	GString *mark = g_string_new(NULL);
	gboolean run = n > 1;
	guint i;

	g_string_append_printf(mark, "S%02dE%02d", season, episodes[0]);
	for (i = 1; i < n; i++) {
		if (episodes[i] != episodes[i - 1] + 1)
			run = FALSE;
	}
	if (run) {
		g_string_append_printf(mark, "-E%02d", episodes[n - 1]);
	} else {
		for (i = 1; i < n; i++)
			g_string_append_printf(mark, "E%02d", episodes[i]);
	}
	return g_string_free(mark, FALSE);
}

static gboolean
episode_input_valid(const AxisImportNameInput *input)
{
	guint i;

	if (input->season < 0 || input->season > 9999 || input->episodes == NULL || input->n_episodes == 0 ||
	    input->n_episodes > AXIS_IMPORT_EPISODES_MAX)
		return FALSE;
	for (i = 0; i < input->n_episodes; i++) {
		if (input->episodes[i] < 0 || input->episodes[i] > 99999)
			return FALSE;
	}
	return TRUE;
}

static gsize
episode_length(
	const GString *series,
	const gchar   *year,
	const gchar   *mark,
	const GString *episode_title,
	const gchar   *rest,
	const gchar   *ext
){
	return series->len + strlen(year) + 3 + strlen(mark) + (episode_title != NULL ? 3 + episode_title->len : 0) +
		strlen(rest) + strlen(ext);
}

/* An episode's file (the folder is made already): see EPISODES. */
static gboolean
build_episode(
	const AxisImportNameInput  *input,
	GPtrArray                  *segments,
	const gchar                *title,
	const gchar                *ext,
	gsize                       budget,
	AxisImportName             *name,
	const gchar               **problem
){
	g_autoptr(GString) series = g_string_new(title);
	g_autofree gchar *year = input->year > 0 && input->year <= 9999 ? g_strdup_printf(" (%d)", input->year) :
		g_strdup("");
	g_autofree gchar *mark = episode_mark(input->season, input->episodes, input->n_episodes);
	g_autofree gchar *rest = NULL;
	g_autofree gboolean *include = NULL;
	GString *episode_title = NULL;
	gchar *values[N_TOKENS];
	guint i;

	name->season_folder = g_strdup_printf("Season %02d", input->season);
	if (input->keep != NULL) {
		/* Renaming off: the release's own name, in the season's folder. */
		g_autofree gchar *kept = axis_import_name_clean(input->keep, -1);
		g_autoptr(GString) stem = g_string_new(kept);

		if (stem->len > strlen(ext) && g_ascii_strcasecmp(stem->str + stem->len - strlen(ext), ext) == 0)
			g_string_truncate(stem, stem->len - strlen(ext));
		while (stem->len > 0 && (stem->str[stem->len - 1] == ' ' || stem->str[stem->len - 1] == '.'))
			g_string_truncate(stem, stem->len - 1);
		if (stem->len == 0)
			g_string_printf(stem, "%s%s - %s", series->str, year, mark);
		if (stem->len + strlen(ext) > budget) {
			cut_to(stem, budget - strlen(ext));
			name->shortened = TRUE;
			if (stem->len == 0)
				g_string_assign(stem, mark);
		}
		name->stem = g_string_free(g_steal_pointer(&stem), FALSE);
		name->file = g_strconcat(name->stem, ext, NULL);
		return TRUE;
	}
	memset(values, 0, sizeof(values));
	values[TOKEN_EDITION] = edition_value(input->edition);
	values[TOKEN_QUALITY] = value_of(input->quality);
	values[TOKEN_HDR] = value_of(input->hdr);
	values[TOKEN_AUDIO] = value_of(input->audio);
	values[TOKEN_CODEC] = value_of(input->codec);
	values[TOKEN_GROUP] = value_of(input->group);
	values[TOKEN_AIRDATE] = value_of(input->air_date);
	{
		gchar *clean = value_of(input->episode_title);

		if (clean != NULL) {
			episode_title = g_string_new(clean);
			g_free(clean);
		}
	}
	include = g_new(gboolean, segments->len + 1);
	for (i = 0; i < segments->len; i++)
		include[i] = TRUE;
	rest = format_render(segments, values, include);

	/* The episode's title is cut first, to nothing. */
	if (episode_title != NULL && episode_length(series, year, mark, episode_title, rest, ext) > budget) {
		gsize without = episode_length(series, year, mark, episode_title, rest, ext) - episode_title->len;

		if (without < budget)
			cut_to(episode_title, budget - without);
		if (without >= budget || episode_title->len == 0) {
			g_string_free(episode_title, TRUE);
			episode_title = NULL;
		}
		name->shortened = TRUE;
	}
	/* Then the format's tokens, the last first. */
	for (i = segments->len; i > 0 && episode_length(series, year, mark, episode_title, rest, ext) > budget; i--) {
		const Segment *segment = g_ptr_array_index(segments, i - 1);

		if (!segment->token)
			continue;
		include[i - 1] = FALSE;
		g_free(rest);
		rest = format_render(segments, values, include);
		name->shortened = TRUE;
	}
	if (episode_length(series, year, mark, episode_title, rest, ext) > budget) {
		g_free(rest);
		rest = g_strdup("");
		name->shortened = TRUE;
	}
	/* Then the series' title, never to nothing. */
	if (episode_length(series, year, mark, episode_title, rest, ext) > budget) {
		gsize others = episode_length(series, year, mark, episode_title, rest, ext) - series->len;

		if (others < budget)
			cut_to(series, budget - others);
		name->shortened = TRUE;
		if (others >= budget || series->len == 0) {
			for (i = 0; i < N_TOKENS; i++)
				g_free(values[i]);
			if (episode_title != NULL)
				g_string_free(episode_title, TRUE);
			if (problem != NULL)
				*problem = "the name does not fit in its length limit";
			return FALSE;
		}
	}
	for (i = 0; i < N_TOKENS; i++)
		g_free(values[i]);
	name->stem = g_strdup_printf("%s%s - %s%s%s%s", series->str, year, mark, episode_title != NULL ? " - " : "",
		episode_title != NULL ? episode_title->str : "", rest);
	name->file = g_strconcat(name->stem, ext, NULL);
	if (episode_title != NULL)
		g_string_free(episode_title, TRUE);
	return TRUE;
}

gboolean
axis_import_name_build(
	const AxisImportNameInput  *input,
	AxisImportName             *name,
	const gchar               **problem
){
	g_autoptr(GPtrArray) segments = NULL;
	g_autofree gchar *ext = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *rest = NULL;
	g_autoptr(GString) title = NULL;
	g_autoptr(GString) tail = g_string_new(NULL);
	g_autofree gboolean *include = NULL;
	gchar *values[N_TOKENS];
	gsize budget;
	guint i;

	memset(name, 0, sizeof(*name));
	memset(values, 0, sizeof(values));
	if (problem != NULL)
		*problem = NULL;
	budget = input->budget > 0 ? MIN(input->budget, (gsize)AXIS_IMPORT_COMPONENT_MAX) : AXIS_IMPORT_NAME_BUDGET;
	ext = extension_of(input->extension);
	if (ext == NULL) {
		if (problem != NULL)
			*problem = "the file has no extension Axis can keep";
		return FALSE;
	}
	segments = format_parse(input->format != NULL ? input->format : AXIS_IMPORT_DEFAULT_FORMAT, NULL);
	if (segments == NULL) {
		if (problem != NULL)
			*problem = "the file format is not valid";
		return FALSE;
	}
	if (input->episode && !episode_input_valid(input)) {
		if (problem != NULL)
			*problem = "the episodes are not ones a name can state";
		return FALSE;
	}

	/* The folder: title, year, identifier. */
	{
		gchar *clean = axis_import_name_clean(input->title, -1);

		title = g_string_new(clean[0] != '\0' ? clean : "Untitled");
		g_free(clean);
	}
	if (input->year > 0 && input->year <= 9999)
		g_string_append_printf(tail, " (%d)", input->year);
	id = identifier_text(input);
	if (id != NULL)
		g_string_append_printf(tail, " %s", id);
	/* The title is cut only as far as the folder and the shortest file
	 * need. */
	if (title->len + tail->len + strlen(ext) > budget) {
		gsize room = budget > tail->len + strlen(ext) ? budget - tail->len - strlen(ext) : 0;

		cut_to(title, room);
		name->shortened = TRUE;
		if (title->len == 0) {
			if (problem != NULL)
				*problem = "the name does not fit in its length limit";
			return FALSE;
		}
	}
	name->folder = g_strconcat(title->str, tail->str, NULL);
	if (input->episode) {
		gboolean built = build_episode(input, segments, title->str, ext, budget, name, problem);

		if (!built)
			axis_import_name_clear(name);
		return built;
	}

	/* Renaming off: the release's own name, in the film's folder. */
	if (input->keep != NULL) {
		g_autofree gchar *kept = axis_import_name_clean(input->keep, -1);
		g_autoptr(GString) stem = g_string_new(kept);
		gsize ext_at = stem->len;

		/* Its own extension is the one kept. */
		if (stem->len > strlen(ext) && g_ascii_strcasecmp(stem->str + stem->len - strlen(ext), ext) == 0)
			ext_at = stem->len - strlen(ext);
		g_string_truncate(stem, ext_at);
		while (stem->len > 0 && (stem->str[stem->len - 1] == ' ' || stem->str[stem->len - 1] == '.'))
			g_string_truncate(stem, stem->len - 1);
		if (stem->len == 0)
			g_string_assign(stem, name->folder);
		if (stem->len + strlen(ext) > budget) {
			cut_to(stem, budget - strlen(ext));
			name->shortened = TRUE;
			if (stem->len == 0)
				g_string_assign(stem, name->folder);
		}
		name->stem = g_string_free(g_steal_pointer(&stem), FALSE);
		name->file = g_strconcat(name->stem, ext, NULL);
		return TRUE;
	}

	values[TOKEN_EDITION] = edition_value(input->edition);
	values[TOKEN_QUALITY] = value_of(input->quality);
	values[TOKEN_HDR] = value_of(input->hdr);
	values[TOKEN_AUDIO] = value_of(input->audio);
	values[TOKEN_CODEC] = value_of(input->codec);
	values[TOKEN_GROUP] = value_of(input->group);
	/* Every token included; then the last one dropped, and the next, while
	 * the file does not fit. */
	include = g_new(gboolean, segments->len + 1);
	for (i = 0; i < segments->len; i++)
		include[i] = TRUE;
	rest = format_render(segments, values, include);
	for (i = segments->len; i > 0 && strlen(name->folder) + strlen(rest) + strlen(ext) > budget; i--) {
		const Segment *segment = g_ptr_array_index(segments, i - 1);

		if (!segment->token)
			continue;
		include[i - 1] = FALSE;
		g_free(rest);
		rest = format_render(segments, values, include);
		name->shortened = TRUE;
	}
	if (strlen(name->folder) + strlen(rest) + strlen(ext) > budget) {
		/* Only the format's own text is left, and it does not fit. */
		g_free(rest);
		rest = g_strdup("");
		name->shortened = TRUE;
	}
	for (i = 0; i < N_TOKENS; i++)
		g_free(values[i]);
	name->stem = g_strconcat(name->folder, rest, NULL);
	name->file = g_strconcat(name->stem, ext, NULL);
	return TRUE;
}

/* ── What a found name is ───────────────────────────────────────── */

static gchar *
lower_ascii(
	const gchar *name,
	gsize        length
){
	gchar *out = g_malloc(length + 1);
	gsize i;

	for (i = 0; i < length; i++)
		out[i] = g_ascii_tolower(name[i]);
	out[length] = '\0';
	return out;
}

AxisImportFolder
axis_import_folder_kind(
	const gchar  *name,
	gsize         length,
	const gchar **extra
){
	static const gchar *const ignored[] = {
		"sample", "samples", "proof", "proofs", "screens", "screenshots", "screenshot", "screencaps", "extras",
		"extra", "@eadir", "plex versions", "metadata", "extrafanart", "lost+found", "$recycle.bin", "#recycle",
		"@recycle", "system volume information", NULL
	};
	static const struct
	{
		const gchar *lower;
		const gchar *shown;
	} extras[] = {
		{ "behind the scenes", "Behind The Scenes" },
		{ "deleted scenes", "Deleted Scenes" },
		{ "featurettes", "Featurettes" },
		{ "interviews", "Interviews" },
		{ "scenes", "Scenes" },
		{ "shorts", "Shorts" },
		{ "trailers", "Trailers" },
		{ "other", "Other" }
	};
	static const gchar *const subs[] = { "subs", "subtitles", "sub", NULL };
	g_autofree gchar *lower = NULL;
	guint i;

	if (extra != NULL)
		*extra = NULL;
	if (length == 0 || name[0] == '.')
		return AXIS_IMPORT_FOLDER_IGNORED;
	if (memchr(name, '\0', length) != NULL)
		return AXIS_IMPORT_FOLDER_IGNORED;
	lower = lower_ascii(name, length);
	for (i = 0; ignored[i] != NULL; i++) {
		if (strcmp(lower, ignored[i]) == 0)
			return AXIS_IMPORT_FOLDER_IGNORED;
	}
	/* SABnzbd's working folders. */
	if (g_str_has_prefix(lower, "_unpack_") || g_str_has_prefix(lower, "_failed_"))
		return AXIS_IMPORT_FOLDER_IGNORED;
	for (i = 0; i < G_N_ELEMENTS(extras); i++) {
		if (strcmp(lower, extras[i].lower) == 0) {
			if (extra != NULL)
				*extra = extras[i].shown;
			return AXIS_IMPORT_FOLDER_EXTRA;
		}
	}
	for (i = 0; subs[i] != NULL; i++) {
		if (strcmp(lower, subs[i]) == 0)
			return AXIS_IMPORT_FOLDER_SUBS;
	}
	return AXIS_IMPORT_FOLDER_PLAIN;
}

gchar *
axis_import_extension(
	const gchar *name,
	gsize        length
){
	gsize dot = length;
	gsize i;

	for (i = length; i > 0; i--) {
		if (name[i - 1] == '.') {
			dot = i - 1;
			break;
		}
	}
	if (dot == length || dot == 0 || length - dot < 2 || length - dot > 17)
		return NULL;
	for (i = dot + 1; i < length; i++) {
		if (!g_ascii_isalnum(name[i]))
			return NULL;
	}
	return lower_ascii(name + dot, length - dot);
}

static gboolean
in_list(
	const gchar        *ext,
	const gchar *const *list
){
	guint i;

	for (i = 0; ext != NULL && list[i] != NULL; i++) {
		if (strcmp(ext, list[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

static const gchar *const video_extensions[] = {
	".mkv", ".mp4", ".m4v", ".avi", ".mov", ".wmv", ".ts", ".m2ts", ".mts", ".mpg", ".mpeg", ".vob", ".webm",
	".flv", ".ogv", ".divx", ".xvid", ".mk3d", ".wtv", ".3gp", ".asf", NULL
};

static const gchar *const subtitle_extensions[] = {
	".srt", ".ass", ".ssa", ".sub", ".idx", ".sup", ".vtt", NULL
};

static const gchar *const archive_extensions[] = {
	".rar", ".zip", ".7z", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".zipx", ".lzh", ".arj", NULL
};

AxisImportFileKind
axis_import_file_kind(
	const gchar *name,
	gsize        length
){
	g_autofree gchar *ext = NULL;
	g_autofree gchar *lower = NULL;

	if (length == 0 || name[0] == '.' || memchr(name, '\0', length) != NULL)
		return AXIS_IMPORT_FILE_IGNORED;
	lower = lower_ascii(name, length);
	if (strcmp(lower, "thumbs.db") == 0 || strcmp(lower, "desktop.ini") == 0)
		return AXIS_IMPORT_FILE_IGNORED;
	ext = axis_import_extension(name, length);
	if (ext == NULL)
		return AXIS_IMPORT_FILE_OTHER;
	if (in_list(ext, video_extensions))
		return AXIS_IMPORT_FILE_VIDEO;
	if (in_list(ext, subtitle_extensions))
		return AXIS_IMPORT_FILE_SUBTITLE;
	if (in_list(ext, archive_extensions))
		return AXIS_IMPORT_FILE_ARCHIVE;
	/* .r00 to .r99 and .001 to .999: the parts of a split archive. */
	if (strlen(ext) == 4 && ((ext[1] == 'r' && g_ascii_isdigit(ext[2]) && g_ascii_isdigit(ext[3])) ||
	    (g_ascii_isdigit(ext[1]) && g_ascii_isdigit(ext[2]) && g_ascii_isdigit(ext[3]))))
		return AXIS_IMPORT_FILE_ARCHIVE;
	return AXIS_IMPORT_FILE_OTHER;
}

/* The stem of @name: without its extension, if it has one. */
static gsize
stem_length(
	const gchar *name,
	gsize        length
){
	g_autofree gchar *ext = axis_import_extension(name, length);

	return ext != NULL ? length - strlen(ext) : length;
}

gboolean
axis_import_extra_suffix(
	const gchar  *name,
	gsize         length,
	const gchar **type
){
	static const gchar *const suffixes[] = {
		"behindthescenes", "deleted", "featurette", "interview", "scene", "short", "trailer", "other", NULL
	};
	gsize stem = stem_length(name, length);
	guint i;

	if (type != NULL)
		*type = NULL;
	for (i = 0; suffixes[i] != NULL; i++) {
		gsize n = strlen(suffixes[i]);

		if (stem > n + 1 && name[stem - n - 1] == '-' &&
		    g_ascii_strncasecmp(name + stem - n, suffixes[i], n) == 0) {
			if (type != NULL)
				*type = suffixes[i];
			return TRUE;
		}
	}
	return FALSE;
}

/* The words (runs of ASCII letters and digits, lower-cased) of @name. */
static GPtrArray *
words_of(
	const gchar *name,
	gsize        length
){
	GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
	gsize i = 0;

	while (i < length && words->len < 256) {
		gsize start;

		while (i < length && !g_ascii_isalnum(name[i]))
			i++;
		start = i;
		while (i < length && g_ascii_isalnum(name[i]))
			i++;
		if (i > start)
			g_ptr_array_add(words, lower_ascii(name + start, i - start));
	}
	return words;
}

static guint
count_word(
	GPtrArray   *words,
	const gchar *word
){
	guint n = 0;
	guint i;

	for (i = 0; i < words->len; i++) {
		if (strcmp(g_ptr_array_index(words, i), word) == 0)
			n++;
	}
	return n;
}

gboolean
axis_import_sample_marked(
	const gchar *name,
	gsize        length,
	const gchar *title_words
){
	g_autoptr(GPtrArray) words = words_of(name, stem_length(name, length));
	g_autoptr(GPtrArray) own = title_words != NULL ? words_of(title_words, strlen(title_words)) :
		g_ptr_array_new_with_free_func(g_free);

	/* More of them than the title has. */
	return count_word(words, "sample") > count_word(own, "sample");
}

gboolean
axis_import_part_marked(
	const gchar *name,
	gsize        length,
	gint        *part
){
	static const gchar *const marks[] = { "cd", "dvd", "part", "pt", "disc", "disk", NULL };
	g_autoptr(GPtrArray) words = words_of(name, stem_length(name, length));
	guint i;
	guint m;

	if (part != NULL)
		*part = 0;
	for (i = 0; i < words->len; i++) {
		const gchar *word = g_ptr_array_index(words, i);

		for (m = 0; marks[m] != NULL; m++) {
			gsize n = strlen(marks[m]);
			const gchar *digits = NULL;

			if (strncmp(word, marks[m], n) != 0)
				continue;
			if (word[n] != '\0')
				digits = word + n;
			else if (i + 1 < words->len)
				digits = g_ptr_array_index(words, i + 1);
			if (digits != NULL && all_digits(digits, 1, 1) && digits[0] != '0') {
				if (part != NULL)
					*part = digits[0] - '0';
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* ── Subtitles ──────────────────────────────────────────────────── */

static const struct
{
	const gchar *code;	/* ISO 639-1 */
	const gchar *b;		/* ISO 639-2/B */
	const gchar *t;		/* ISO 639-2/T, when it differs */
	const gchar *english;
} languages[] = {
	{ "en", "eng", NULL, "english" }, { "fr", "fre", "fra", "french" }, { "de", "ger", "deu", "german" },
	{ "es", "spa", NULL, "spanish" }, { "it", "ita", NULL, "italian" }, { "pt", "por", NULL, "portuguese" },
	{ "nl", "dut", "nld", "dutch" }, { "sv", "swe", NULL, "swedish" }, { "no", "nor", NULL, "norwegian" },
	{ "nb", "nob", NULL, "bokmal" }, { "da", "dan", NULL, "danish" }, { "fi", "fin", NULL, "finnish" },
	{ "pl", "pol", NULL, "polish" }, { "ru", "rus", NULL, "russian" }, { "ja", "jpn", NULL, "japanese" },
	{ "zh", "chi", "zho", "chinese" }, { "ko", "kor", NULL, "korean" }, { "ar", "ara", NULL, "arabic" },
	{ "he", "heb", NULL, "hebrew" }, { "hu", "hun", NULL, "hungarian" }, { "cs", "cze", "ces", "czech" },
	{ "el", "gre", "ell", "greek" }, { "tr", "tur", NULL, "turkish" }, { "hi", "hin", NULL, "hindi" },
	{ "th", "tha", NULL, "thai" }, { "ro", "rum", "ron", "romanian" }, { "bg", "bul", NULL, "bulgarian" },
	{ "hr", "hrv", NULL, "croatian" }, { "sr", "srp", NULL, "serbian" }, { "uk", "ukr", NULL, "ukrainian" },
	{ "vi", "vie", NULL, "vietnamese" }, { "id", "ind", NULL, "indonesian" }, { "ms", "may", "msa", "malay" },
	{ "fa", "per", "fas", "persian" }, { "is", "ice", "isl", "icelandic" }, { "et", "est", NULL, "estonian" },
	{ "lv", "lav", NULL, "latvian" }, { "lt", "lit", NULL, "lithuanian" }, { "sk", "slo", "slk", "slovak" },
	{ "sl", "slv", NULL, "slovenian" }, { "ca", "cat", NULL, "catalan" }, { "eu", "baq", "eus", "basque" },
	{ "gl", "glg", NULL, "galician" }, { "ta", "tam", NULL, "tamil" }, { "te", "tel", NULL, "telugu" },
	{ "bn", "ben", NULL, "bengali" }, { "ur", "urd", NULL, "urdu" }, { "sq", "alb", "sqi", "albanian" },
	{ "mk", "mac", "mkd", "macedonian" }, { "bs", "bos", NULL, "bosnian" }, { "fil", "fil", NULL, "filipino" },
	{ "la", "lat", NULL, "latin" }, { "cy", "wel", "cym", "welsh" }, { "ga", "gle", NULL, "irish" }
};

/* The ISO 639-1 code of a word ("en", "eng", "English", "en-US"), static;
 * %NULL when it is none. */
static const gchar *
language_of(const gchar *lower)
{
	gsize n = strlen(lower);
	guint i;

	/* "en-us", "pt-br": the language before the region. */
	if (n == 5 && lower[2] == '-' && g_ascii_isalpha(lower[3]) && g_ascii_isalpha(lower[4])) {
		gchar code[3] = { lower[0], lower[1], '\0' };

		for (i = 0; i < G_N_ELEMENTS(languages); i++) {
			if (strcmp(languages[i].code, code) == 0)
				return languages[i].code;
		}
		return NULL;
	}
	for (i = 0; i < G_N_ELEMENTS(languages); i++) {
		if (strcmp(languages[i].code, lower) == 0 || strcmp(languages[i].b, lower) == 0 ||
		    (languages[i].t != NULL && strcmp(languages[i].t, lower) == 0) ||
		    strcmp(languages[i].english, lower) == 0)
			return languages[i].code;
	}
	return NULL;
}

static gboolean
word_separator(gchar c)
{
	return c == '.' || c == '_' || c == ' ' || c == '[' || c == ']' || c == '(' || c == ')';
}

gboolean
axis_import_subtitle_read(
	const gchar        *name,
	gsize               length,
	AxisImportSubtitle *subtitle
){
	g_autofree gchar *ext = axis_import_extension(name, length);
	gsize end;
	gboolean pending_hi = FALSE;
	guint read = 0;
	guint i;

	memset(subtitle, 0, sizeof(*subtitle));
	if (ext == NULL || !in_list(ext, subtitle_extensions))
		return FALSE;
	for (i = 0; subtitle_extensions[i] != NULL; i++) {
		if (strcmp(ext, subtitle_extensions[i]) == 0)
			subtitle->extension = subtitle_extensions[i];
	}
	end = length - strlen(ext);
	/* Words from the end, at most a few. */
	while (end > 0 && read < 6) {
		gsize start = end;
		g_autofree gchar *word = NULL;
		const gchar *language;

		while (start > 0 && !word_separator(name[start - 1]))
			start--;
		if (start == end) {
			end--;
			continue;
		}
		if (end - start > 16)
			break;
		word = lower_ascii(name + start, end - start);
		read++;
		end = start;
		if (strcmp(word, "forced") == 0 || strcmp(word, "foreign") == 0) {
			subtitle->forced = TRUE;
			continue;
		}
		if (strcmp(word, "sdh") == 0 || strcmp(word, "cc") == 0) {
			subtitle->sdh = TRUE;
			continue;
		}
		if (strcmp(word, "default") == 0)
			continue;
		if (strcmp(word, "hi") == 0 && subtitle->language == NULL && !pending_hi) {
			/* Hindi alone; hearing impaired beside a language. */
			pending_hi = TRUE;
			continue;
		}
		language = language_of(word);
		if (language != NULL && subtitle->language == NULL) {
			subtitle->language = language;
			continue;
		}
		break;
	}
	if (pending_hi) {
		if (subtitle->language != NULL)
			subtitle->sdh = TRUE;
		else
			subtitle->language = "hi";
	}
	return TRUE;
}

gchar *
axis_import_subtitle_name(
	const gchar              *stem,
	const AxisImportSubtitle *subtitle,
	guint                     number
){
	g_autoptr(GString) suffix = g_string_new(NULL);
	g_autoptr(GString) base = g_string_new(stem != NULL ? stem : "");

	g_string_append_printf(suffix, ".%s", subtitle->language != NULL ? subtitle->language : "und");
	if (subtitle->forced)
		g_string_append(suffix, ".forced");
	if (subtitle->sdh)
		g_string_append(suffix, ".sdh");
	if (subtitle->language == NULL && number == 0)
		number = 1;
	if (number > 0)
		g_string_append_printf(suffix, ".%u", number);
	g_string_append(suffix, subtitle->extension != NULL ? subtitle->extension : ".srt");
	/* Beside the film's file, so the whole of 255 bytes: the temporary name
	 * is not made from it. */
	if (base->len + suffix->len > AXIS_IMPORT_COMPONENT_MAX)
		cut_to(base, AXIS_IMPORT_COMPONENT_MAX - suffix->len);
	g_string_append(base, suffix->str);
	return g_string_free(g_steal_pointer(&base), FALSE);
}

gchar *
axis_import_extra_name(
	const gchar *name,
	gsize        length
){
	g_autofree gchar *clean = axis_import_name_clean(name, (gssize)length);
	g_autofree gchar *ext = axis_import_extension(clean, strlen(clean));
	g_autoptr(GString) stem = NULL;
	const gchar *type = NULL;
	gsize stem_end;
	g_autofree gchar *suffix = NULL;

	if (ext == NULL || clean[0] == '\0')
		return NULL;
	stem_end = strlen(clean) - strlen(ext);
	stem = g_string_new_len(clean, (gssize)stem_end);
	if (!axis_import_extra_suffix(clean, strlen(clean), &type))
		return NULL;
	if (stem->len + strlen(ext) <= AXIS_IMPORT_NAME_BUDGET)
		return g_strconcat(stem->str, ext, NULL);
	/* The descriptive part is cut; "-type" stays. */
	suffix = g_strdup_printf("-%s", type);
	g_string_truncate(stem, stem->len - strlen(suffix));
	cut_to(stem, AXIS_IMPORT_NAME_BUDGET - strlen(ext) - strlen(suffix));
	if (stem->len == 0)
		g_string_assign(stem, "Extra");
	return g_strconcat(stem->str, suffix, ext, NULL);
}
