/*
 * axis-quality-rule.c - preference rules: their conditions and values,
 * whether one holds for a release, and the words for them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "quality/axis-quality-private.h"

#include <stdlib.h>
#include <string.h>

/* ── Conditions ─────────────────────────────────────────────────── */

static const struct
{
	const gchar *name;
	const gchar *label;
} conditions[AXIS_QUALITY_N_CONDITIONS] = {
	{ "codec", "Video codec" },
	{ "bit_depth", "Bit depth" },
	{ "hdr", "HDR" },
	{ "audio", "Audio" },
	{ "channels", "Audio channels" },
	{ "language", "Language" },
	{ "language_mark", "Language marks" },
	{ "edition", "Edition" },
	{ "service", "Streaming service" },
	{ "group", "Release group" },
	{ "word", "Word in the name" },
	{ "3d", "3D" },
	{ "hardsub", "Hard subtitles" },
	{ "revision", "Proper or repack" }
};

const gchar *
axis_quality_condition_name(AxisQualityCondition condition)
{
	return (guint)condition < AXIS_QUALITY_N_CONDITIONS ? conditions[condition].name : NULL;
}

gboolean
axis_quality_condition_from_name(
	const gchar          *name,
	AxisQualityCondition *condition
){
	guint i;

	for (i = 0; name != NULL && i < AXIS_QUALITY_N_CONDITIONS; i++) {
		if (strcmp(conditions[i].name, name) == 0) {
			*condition = (AxisQualityCondition)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_quality_condition_label(AxisQualityCondition condition)
{
	return (guint)condition < AXIS_QUALITY_N_CONDITIONS ? conditions[condition].label : "?";
}

gboolean
axis_quality_condition_has_values(AxisQualityCondition condition)
{
	return condition != AXIS_QUALITY_IF_3D && condition != AXIS_QUALITY_IF_HARDSUB;
}

/* ── Effects ────────────────────────────────────────────────────── */

static const gchar *const effects[AXIS_QUALITY_N_EFFECTS] = { "require", "reject", "prefer", "avoid" };

const gchar *
axis_quality_effect_name(AxisQualityEffect effect)
{
	return (guint)effect < AXIS_QUALITY_N_EFFECTS ? effects[effect] : NULL;
}

gboolean
axis_quality_effect_from_name(
	const gchar       *name,
	AxisQualityEffect *effect
){
	guint i;

	for (i = 0; name != NULL && i < AXIS_QUALITY_N_EFFECTS; i++) {
		if (strcmp(effects[i], name) == 0) {
			*effect = (AxisQualityEffect)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── Values ─────────────────────────────────────────────────────── */

typedef struct
{
	const gchar *value;
	const gchar *label;
} Word;

static const Word codec_words[] = {
	{ "h264", "H.264 (x264)" }, { "h265", "HEVC (x265)" }, { "av1", "AV1" }, { "vc1", "VC-1" },
	{ "mpeg2", "MPEG-2" }, { "xvid", "XviD" }, { "vp9", "VP9" }, { NULL, NULL }
};

static const Word depth_words[] = {
	{ "8", "8-bit" }, { "10", "10-bit" }, { "12", "12-bit" }, { NULL, NULL }
};

static const Word hdr_words[] = {
	{ "hdr", "HDR (its kind not stated)" }, { "hdr10", "HDR10" }, { "hdr10plus", "HDR10+" },
	{ "dv", "Dolby Vision" }, { "hlg", "HLG" }, { "sdr", "SDR" }, { "any", "HDR of any kind" },
	{ "dv_only", "Dolby Vision without an HDR10 fallback" }, { NULL, NULL }
};

static const Word audio_words[] = {
	{ "truehd", "TrueHD" }, { "dtsx", "DTS:X" }, { "dtshdma", "DTS-HD MA" }, { "dtshd", "DTS-HD" },
	{ "dts", "DTS" }, { "eac3", "E-AC-3 (DD+)" }, { "ac3", "AC-3 (DD)" }, { "aac", "AAC" }, { "flac", "FLAC" },
	{ "opus", "Opus" }, { "mp3", "MP3" }, { "pcm", "PCM" }, { "atmos", "Atmos" }, { NULL, NULL }
};

static const Word mark_words[] = {
	{ "multi", "several audio languages (MULTi)" }, { "dual", "dual audio" }, { "dubbed", "dubbed" },
	{ "subbed", "subtitled" }, { NULL, NULL }
};

static const Word edition_words[] = {
	{ "extended", "Extended" }, { "directors", "Director's Cut" }, { "theatrical", "Theatrical" },
	{ "unrated", "Unrated" }, { "uncut", "Uncut" }, { "remastered", "Remastered" }, { "restored", "Restored" },
	{ "imax", "IMAX" }, { "criterion", "Criterion" }, { "finalcut", "Final Cut" }, { "openmatte", "Open Matte" },
	{ "special", "Special Edition" }, { "collectors", "Collector's Edition" },
	{ "anniversary", "Anniversary Edition" }, { "ultimate", "Ultimate Edition" },
	{ "definitive", "Definitive Edition" }, { "alternate", "Alternate Cut" }, { "other", "another edition" },
	{ NULL, NULL }
};

static const Word revision_words[] = {
	{ "proper", "a proper" }, { "repack", "a repack" }, { "rerip", "a rerip" }, { "real", "REAL (a second fix)" },
	{ NULL, NULL }
};

/* The languages the parser reads, by code. */
static const Word language_words[] = {
	{ "en", "English" }, { "fr", "French" }, { "de", "German" }, { "it", "Italian" }, { "es", "Spanish" },
	{ "pt", "Portuguese" }, { "ru", "Russian" }, { "ja", "Japanese" }, { "ko", "Korean" }, { "zh", "Chinese" },
	{ "hi", "Hindi" }, { "ta", "Tamil" }, { "te", "Telugu" }, { "ml", "Malayalam" }, { "kn", "Kannada" },
	{ "bn", "Bengali" }, { "pa", "Punjabi" }, { "mr", "Marathi" }, { "ur", "Urdu" }, { "ar", "Arabic" },
	{ "he", "Hebrew" }, { "tr", "Turkish" }, { "el", "Greek" }, { "pl", "Polish" }, { "cs", "Czech" },
	{ "sk", "Slovak" }, { "hu", "Hungarian" }, { "ro", "Romanian" }, { "bg", "Bulgarian" },
	{ "uk", "Ukrainian" }, { "sr", "Serbian" }, { "hr", "Croatian" }, { "sl", "Slovenian" }, { "nl", "Dutch" },
	{ "sv", "Swedish" }, { "da", "Danish" }, { "no", "Norwegian" }, { "fi", "Finnish" }, { "is", "Icelandic" },
	{ "th", "Thai" }, { "vi", "Vietnamese" }, { "id", "Indonesian" }, { "ms", "Malay" }, { "fa", "Persian" },
	{ NULL, NULL }
};

static const Word service_words[] = {
	{ "amazon", "Amazon" }, { "netflix", "Netflix" }, { "disney", "Disney+" }, { "hbomax", "HBO Max" },
	{ "max", "Max" }, { "apple", "Apple TV+" }, { "hulu", "Hulu" }, { "paramount", "Paramount+" },
	{ "peacock", "Peacock" }, { "itunes", "iTunes" }, { NULL, NULL }
};

static const Word *
fixed_words(AxisQualityCondition condition)
{
	switch (condition) {
	case AXIS_QUALITY_IF_CODEC:         return codec_words;
	case AXIS_QUALITY_IF_BIT_DEPTH:     return depth_words;
	case AXIS_QUALITY_IF_HDR:           return hdr_words;
	case AXIS_QUALITY_IF_AUDIO:         return audio_words;
	case AXIS_QUALITY_IF_LANGUAGE_MARK: return mark_words;
	case AXIS_QUALITY_IF_EDITION:       return edition_words;
	case AXIS_QUALITY_IF_REVISION:      return revision_words;
	default:                            return NULL;
	}
}

const gchar *const *
axis_quality_condition_values(AxisQualityCondition condition)
{
	/* Built once from the tables above (the same on every thread). */
	static const gchar **lists[AXIS_QUALITY_N_CONDITIONS];
	static gsize done;
	const Word *words;

	if ((guint)condition >= AXIS_QUALITY_N_CONDITIONS || (words = fixed_words(condition)) == NULL)
		return NULL;
	if (g_once_init_enter(&done)) {
		guint c;

		for (c = 0; c < AXIS_QUALITY_N_CONDITIONS; c++) {
			const Word *w = fixed_words((AxisQualityCondition)c);
			guint n = 0;
			guint i;

			if (w == NULL)
				continue;
			while (w[n].value != NULL)
				n++;
			lists[c] = g_new0(const gchar *, n + 1);
			for (i = 0; i < n; i++)
				lists[c][i] = w[i].value;
		}
		g_once_init_leave(&done, 1);
	}
	return (const gchar *const *)lists[condition];
}

static const gchar *
word_label(
	const Word  *words,
	const gchar *value
){
	guint i;

	for (i = 0; words != NULL && value != NULL && words[i].value != NULL; i++) {
		if (strcmp(words[i].value, value) == 0)
			return words[i].label;
	}
	return NULL;
}

static gboolean
all_chars(
	const gchar *value,
	const gchar *extra,
	gboolean     upper
){
	const gchar *p;

	for (p = value; *p != '\0'; p++) {
		gchar c = *p;

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (upper && c >= 'A' && c <= 'Z'))
			continue;
		if (extra != NULL && strchr(extra, c) != NULL)
			continue;
		return FALSE;
	}
	return TRUE;
}

/* "2.0", "5.1", "7.1.4": digits separated by single dots, two or three
 * parts, at most seven characters (the parser's own limit). */
static gboolean
channels_valid(const gchar *value)
{
	guint parts = 1;
	gboolean digit = FALSE;
	const gchar *p;

	if (strlen(value) > 7)
		return FALSE;
	for (p = value; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			digit = TRUE;
		} else if (*p == '.' && digit) {
			parts++;
			digit = FALSE;
		} else {
			return FALSE;
		}
	}
	return digit && parts >= 2 && parts <= 3;
}

gboolean
axis_quality_value_valid(
	AxisQualityCondition  condition,
	const gchar          *value
){
	gsize length;
	const Word *words;

	if (value == NULL || !axis_quality_condition_has_values(condition))
		return FALSE;
	length = strlen(value);
	if (length == 0 || length > AXIS_QUALITY_VALUE_MAX)
		return FALSE;
	words = fixed_words(condition);
	if (words != NULL)
		return word_label(words, value) != NULL;
	switch (condition) {
	case AXIS_QUALITY_IF_CHANNELS:
		return channels_valid(value);
	case AXIS_QUALITY_IF_LANGUAGE:
		return length == 2 && all_chars(value, NULL, FALSE) && !g_ascii_isdigit(value[0]) &&
			!g_ascii_isdigit(value[1]);
	case AXIS_QUALITY_IF_SERVICE:
		return length <= 24 && all_chars(value, NULL, FALSE) && g_ascii_isalpha(value[0]);
	case AXIS_QUALITY_IF_GROUP:
		return all_chars(value, "._+-", TRUE);
	case AXIS_QUALITY_IF_WORD:
		return all_chars(value, NULL, TRUE);
	default:
		return FALSE;
	}
}

const gchar *
axis_quality_value_label(
	AxisQualityCondition  condition,
	const gchar          *value
){
	const gchar *label = NULL;

	if ((guint)condition >= AXIS_QUALITY_N_CONDITIONS)
		return value;
	if (condition == AXIS_QUALITY_IF_LANGUAGE)
		label = word_label(language_words, value);
	else if (condition == AXIS_QUALITY_IF_SERVICE)
		label = word_label(service_words, value);
	else
		label = word_label(fixed_words(condition), value);
	return label != NULL ? label : value;
}

gboolean
axis_quality_value_equal(
	AxisQualityCondition  condition,
	const gchar          *a,
	const gchar          *b
){
	if (condition == AXIS_QUALITY_IF_GROUP || condition == AXIS_QUALITY_IF_WORD)
		return g_ascii_strcasecmp(a, b) == 0;
	return strcmp(a, b) == 0;
}

static int
compare_values(
	const void *a,
	const void *b
){
	const gchar *x = a;
	const gchar *y = b;
	gint c = g_ascii_strcasecmp(x, y);

	return c != 0 ? c : strcmp(x, y);
}

void
axis_quality_rule_sort_values(AxisQualityRule *rule)
{
	if (rule->n_values > 1 && rule->n_values <= AXIS_QUALITY_MAX_VALUES)
		qsort(rule->values, rule->n_values, sizeof(rule->values[0]), compare_values);
}

/* ── Whether a rule holds ───────────────────────────────────────── */

/* The flag of a set whose name is @value, 0 for none. */
static guint
flag_of(
	const gchar *(*name_of)(guint flag),
	const gchar  *value
){
	guint i;

	for (i = 0; i < 31; i++) {
		const gchar *n = name_of(1u << i);

		if (n != NULL && strcmp(n, value) == 0)
			return 1u << i;
	}
	return 0;
}

#define HDR_KINDS (AXIS_RELEASE_HDR_HDR | AXIS_RELEASE_HDR_HDR10 | AXIS_RELEASE_HDR_HDR10PLUS | \
	AXIS_RELEASE_HDR_DOLBY_VISION | AXIS_RELEASE_HDR_HLG)
#define HDR10_FALLBACK (AXIS_RELEASE_HDR_HDR | AXIS_RELEASE_HDR_HDR10 | AXIS_RELEASE_HDR_HDR10PLUS)

static gboolean
hdr_holds(
	guint        hdr,
	const gchar *value
){
	if (strcmp(value, "any") == 0)
		return (hdr & HDR_KINDS) != 0;
	if (strcmp(value, "dv_only") == 0)
		return (hdr & AXIS_RELEASE_HDR_DOLBY_VISION) != 0 && (hdr & HDR10_FALLBACK) == 0;
	return (hdr & flag_of(axis_release_hdr_name, value)) != 0;
}

/* The words of @text (letters and digits, split at anything else). */
static GPtrArray *
words_of(const gchar *text)
{
	GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
	const gchar *p = text;

	while (p != NULL && *p != '\0') {
		const gchar *start;

		while (*p != '\0' && !g_ascii_isalnum(*p))
			p++;
		start = p;
		while (*p != '\0' && g_ascii_isalnum(*p))
			p++;
		if (p > start && words->len < 256)
			g_ptr_array_add(words, g_strndup(start, (gsize)(p - start)));
	}
	return words;
}

/* Whether a word of @name, outside the title's own words, is @value. */
static gboolean
word_holds(
	const gchar       *name,
	const AxisRelease *release,
	const gchar       *value
){
	g_autoptr(GPtrArray) words = words_of(name);
	g_autoptr(GPtrArray) title = words_of(release->title);
	guint skip_from = G_MAXUINT;
	guint i;
	guint j;

	/* The first place the title's words stand in the name, in order. */
	for (i = 0; title->len > 0 && i + title->len <= words->len && skip_from == G_MAXUINT; i++) {
		for (j = 0; j < title->len; j++) {
			if (g_ascii_strcasecmp(g_ptr_array_index(words, i + j), g_ptr_array_index(title, j)) != 0)
				break;
		}
		if (j == title->len)
			skip_from = i;
	}
	for (i = 0; i < words->len; i++) {
		if (skip_from != G_MAXUINT && i >= skip_from && i < skip_from + title->len)
			continue;
		if (g_ascii_strcasecmp(g_ptr_array_index(words, i), value) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Whether @value of the rule's condition holds for @release. */
static gboolean
value_holds(
	AxisQualityCondition  condition,
	const gchar          *value,
	const AxisRelease    *release,
	const gchar          *name
){
	guint i;

	switch (condition) {
	case AXIS_QUALITY_IF_CODEC:
		return g_strcmp0(axis_release_codec_name(release->codec), value) == 0;
	case AXIS_QUALITY_IF_BIT_DEPTH:
		return release->bit_depth > 0 && atoi(value) == release->bit_depth;
	case AXIS_QUALITY_IF_HDR:
		return hdr_holds(release->hdr, value);
	case AXIS_QUALITY_IF_AUDIO:
		if (strcmp(value, "atmos") == 0)
			return release->atmos;
		return (release->audio & flag_of(axis_release_audio_name, value)) != 0;
	case AXIS_QUALITY_IF_CHANNELS:
		return release->channels[0] != '\0' && strcmp(release->channels, value) == 0;
	case AXIS_QUALITY_IF_LANGUAGE:
		for (i = 0; i < release->language_count && i < AXIS_RELEASE_MAX_LANGUAGES; i++) {
			if (g_strcmp0(release->languages[i], value) == 0)
				return TRUE;
		}
		return FALSE;
	case AXIS_QUALITY_IF_LANGUAGE_MARK:
		return (release->language_marks & flag_of(axis_release_lang_mark_name, value)) != 0 &&
			strcmp(value, "hardsub") != 0;
	case AXIS_QUALITY_IF_EDITION:
		return (release->editions & flag_of(axis_release_edition_name, value)) != 0;
	case AXIS_QUALITY_IF_SERVICE:
		return g_strcmp0(release->service, value) == 0;
	case AXIS_QUALITY_IF_GROUP:
		return release->group != NULL && g_ascii_strcasecmp(release->group, value) == 0;
	case AXIS_QUALITY_IF_WORD:
		return name != NULL && word_holds(name, release, value);
	case AXIS_QUALITY_IF_REVISION:
		return (release->revision & flag_of(axis_release_revision_name, value)) != 0;
	default:
		return FALSE;
	}
}

gboolean
axis_quality_rule_holds(
	const AxisQualityRule *rule,
	const AxisRelease     *release,
	const gchar           *name
){
	guint i;

	if (rule == NULL || release == NULL)
		return FALSE;
	if (rule->condition == AXIS_QUALITY_IF_3D)
		return (release->stop & AXIS_RELEASE_STOP_3D) != 0;
	if (rule->condition == AXIS_QUALITY_IF_HARDSUB)
		return (release->language_marks & AXIS_RELEASE_LANG_HARDSUB) != 0;
	for (i = 0; i < rule->n_values && i < AXIS_QUALITY_MAX_VALUES; i++) {
		if (value_holds(rule->condition, rule->values[i], release, name))
			return TRUE;
	}
	return FALSE;
}

gint
axis_quality_revision_rank(const AxisRelease *release)
{
	gint rank = 0;

	if (release == NULL)
		return 0;
	if ((release->revision & (AXIS_RELEASE_REVISION_PROPER | AXIS_RELEASE_REVISION_REPACK |
	                          AXIS_RELEASE_REVISION_RERIP)) != 0)
		rank = 1;
	if ((release->revision & AXIS_RELEASE_REVISION_REAL) != 0)
		rank = rank > 0 ? rank + 1 : 1;
	if (release->version > 1 && release->version - 1 > rank)
		rank = MIN(release->version - 1, 99);
	return rank;
}

/* ── Words ──────────────────────────────────────────────────────── */

gchar *
axis_quality_join(
	const GPtrArray *items,
	const gchar     *last
){
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; items != NULL && i < items->len; i++) {
		if (i > 0)
			g_string_append(out, i + 1 == items->len ? last : ", ");
		g_string_append(out, g_ptr_array_index((GPtrArray *)items, i));
	}
	return g_string_free(out, FALSE);
}

/* The labels of the flags of @flags in a set, as text. */
static void
add_flag_labels(
	GPtrArray            *out,
	AxisQualityCondition  condition,
	const gchar *(*name_of)(guint flag),
	guint                 flags
){
	guint i;

	for (i = 0; i < 31; i++) {
		const gchar *name = (flags & (1u << i)) != 0 ? name_of(1u << i) : NULL;

		if (name != NULL && (condition != AXIS_QUALITY_IF_LANGUAGE_MARK || strcmp(name, "hardsub") != 0))
			g_ptr_array_add(out, g_strdup(axis_quality_value_label(condition, name)));
	}
}

gchar *
axis_quality_rule_has_text(
	AxisQualityCondition  condition,
	const AxisRelease    *release
){
	g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_free);
	guint i;

	switch (condition) {
	case AXIS_QUALITY_IF_CODEC:
		if (release->codec != AXIS_RELEASE_CODEC_NONE)
			g_ptr_array_add(items, g_strdup(axis_quality_value_label(condition,
				axis_release_codec_name(release->codec))));
		break;
	case AXIS_QUALITY_IF_BIT_DEPTH:
		if (release->bit_depth > 0)
			g_ptr_array_add(items, g_strdup_printf("%d-bit", release->bit_depth));
		break;
	case AXIS_QUALITY_IF_HDR:
		add_flag_labels(items, condition, axis_release_hdr_name, release->hdr);
		break;
	case AXIS_QUALITY_IF_AUDIO:
		add_flag_labels(items, condition, axis_release_audio_name, release->audio);
		if (release->atmos)
			g_ptr_array_add(items, g_strdup("Atmos"));
		break;
	case AXIS_QUALITY_IF_CHANNELS:
		if (release->channels[0] != '\0')
			g_ptr_array_add(items, g_strdup_printf("%s channels", release->channels));
		break;
	case AXIS_QUALITY_IF_LANGUAGE:
		for (i = 0; i < release->language_count && i < AXIS_RELEASE_MAX_LANGUAGES; i++)
			g_ptr_array_add(items, g_strdup(axis_quality_value_label(condition, release->languages[i])));
		break;
	case AXIS_QUALITY_IF_LANGUAGE_MARK:
		add_flag_labels(items, condition, axis_release_lang_mark_name, release->language_marks);
		break;
	case AXIS_QUALITY_IF_EDITION:
		add_flag_labels(items, condition, axis_release_edition_name, release->editions);
		break;
	case AXIS_QUALITY_IF_SERVICE:
		if (release->service != NULL)
			g_ptr_array_add(items, g_strdup(axis_quality_value_label(condition, release->service)));
		break;
	case AXIS_QUALITY_IF_GROUP:
		if (release->group != NULL)
			g_ptr_array_add(items, g_strdup_printf("the group %s", release->group));
		break;
	case AXIS_QUALITY_IF_3D:
		if ((release->stop & AXIS_RELEASE_STOP_3D) != 0)
			g_ptr_array_add(items, g_strdup("3D"));
		break;
	case AXIS_QUALITY_IF_HARDSUB:
		if ((release->language_marks & AXIS_RELEASE_LANG_HARDSUB) != 0)
			g_ptr_array_add(items, g_strdup("hard-coded subtitles"));
		break;
	case AXIS_QUALITY_IF_REVISION:
		add_flag_labels(items, condition, axis_release_revision_name, release->revision);
		break;
	default:
		break;
	}
	if (items->len == 0)
		return NULL;
	return axis_quality_join(items, " and ");
}

gchar *
axis_quality_rule_matched_text(
	const AxisQualityRule *rule,
	const AxisRelease     *release,
	const gchar           *name
){
	g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *joined = NULL;
	guint i;

	if (rule->condition == AXIS_QUALITY_IF_3D)
		return axis_quality_rule_holds(rule, release, name) ? g_strdup("3D") : NULL;
	if (rule->condition == AXIS_QUALITY_IF_HARDSUB)
		return axis_quality_rule_holds(rule, release, name) ? g_strdup("hard-coded subtitles") : NULL;
	for (i = 0; i < rule->n_values && i < AXIS_QUALITY_MAX_VALUES; i++) {
		if (!value_holds(rule->condition, rule->values[i], release, name))
			continue;
		/* "Any HDR" says which the release has. */
		if (rule->condition == AXIS_QUALITY_IF_HDR && strcmp(rule->values[i], "any") == 0)
			g_ptr_array_add(items, axis_quality_rule_has_text(AXIS_QUALITY_IF_HDR, release));
		else
			g_ptr_array_add(items, g_strdup(axis_quality_value_label(rule->condition, rule->values[i])));
	}
	if (items->len == 0)
		return NULL;
	joined = axis_quality_join(items, " and ");
	switch (rule->condition) {
	case AXIS_QUALITY_IF_GROUP:
		return g_strdup_printf("the group %s", release->group != NULL ? release->group : joined);
	case AXIS_QUALITY_IF_WORD:
		return g_strdup_printf("the word %s in the name", joined);
	case AXIS_QUALITY_IF_CHANNELS:
		return g_strdup_printf("%s channels", joined);
	case AXIS_QUALITY_IF_EDITION:
		return g_strdup_printf("the %s edition", joined);
	case AXIS_QUALITY_IF_LANGUAGE:
		return g_strdup_printf("%s audio", joined);
	default:
		return g_steal_pointer(&joined);
	}
}

gchar *
axis_quality_rule_values_text(const AxisQualityRule *rule)
{
	g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *joined = NULL;
	guint i;

	if (rule->condition == AXIS_QUALITY_IF_3D)
		return g_strdup("3D");
	if (rule->condition == AXIS_QUALITY_IF_HARDSUB)
		return g_strdup("hard-coded subtitles");
	for (i = 0; i < rule->n_values && i < AXIS_QUALITY_MAX_VALUES; i++)
		g_ptr_array_add(items, g_strdup(axis_quality_value_label(rule->condition, rule->values[i])));
	joined = axis_quality_join(items, " or ");
	switch (rule->condition) {
	case AXIS_QUALITY_IF_GROUP:
		return g_strdup_printf(rule->n_values == 1 ? "the group %s" : "the groups %s", joined);
	case AXIS_QUALITY_IF_WORD:
		return g_strdup_printf(rule->n_values == 1 ? "the word %s" : "one of the words %s", joined);
	case AXIS_QUALITY_IF_CHANNELS:
		return g_strdup_printf("%s channels", joined);
	case AXIS_QUALITY_IF_EDITION:
		return g_strdup_printf("the %s edition", joined);
	case AXIS_QUALITY_IF_LANGUAGE:
		return g_strdup_printf("%s audio", joined);
	default:
		return g_steal_pointer(&joined);
	}
}

gchar *
axis_quality_rule_describe(const AxisQualityRule *rule)
{
	static const gchar *const verbs[AXIS_QUALITY_N_EFFECTS] = { "Require", "Reject", "Prefer", "Avoid" };
	g_autofree gchar *what = NULL;
	const gchar *verb;

	g_return_val_if_fail(rule != NULL, g_strdup(""));

	what = axis_quality_rule_values_text(rule);
	verb = (guint)rule->effect < AXIS_QUALITY_N_EFFECTS ? verbs[rule->effect] : "?";
	if (rule->effect == AXIS_QUALITY_PREFER || rule->effect == AXIS_QUALITY_AVOID)
		return g_strdup_printf("%s %s, by %d", verb, what, rule->weight);
	return g_strdup_printf("%s %s", verb, what);
}
