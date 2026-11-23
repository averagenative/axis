/*
 * axis-profile-definition.c - the strict definition format: parse, check,
 * normal order, canonical text, hash
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The JSON grammar is the probe's bounded reader (axis-probe-json.h); what
 * is checked here is the format of axis-profile.h.  Every check refuses:
 * there is no default for a missing member, no conversion of a wrong type
 * and no clamping of an out-of-range value.  A problem's message never
 * holds any of the input (an unknown member is not named), so it can be
 * shown and logged as it is.
 */

#include "transcode/axis-profile.h"

#include <string.h>

#include "probe/axis-probe-json.h"

/* ── Names and tables ───────────────────────────────────────────── */

static const gchar *const resolutions[] = { "sd", "720p", "1080p", "2160p", "other", NULL };
#define N_RESOLUTIONS 5

static const gchar *const video_libraries[] = { "movies", "tv", "other", NULL };
static const gchar *const audio_libraries[] = { "music", "audiobooks", NULL };

/* Quality numbers with a level in the size model, ascending. */
static const gint hevc_crfs[] = { 20, 22, 24, 26, 28, 30, 32, 0 };
static const gint h264_crfs[] = { 18, 19, 20, 22, 24, 26, 28, 0 };
static const gint no_crfs[] = { 0 };

/* LAME's documented average bitrates of V0 to V4 (kbit/s), which the
 * estimate takes as the nominal bitrate of a VBR encode. */
static const gint mp3_bitrates[] = { 245000, 225000, 190000, 175000, 165000 };

#define MAX_MEMBERS 24

const gchar *
axis_profile_kind_to_string(AxisProfileKind kind)
{
	return kind == AXIS_PROFILE_KIND_AUDIO ? "audio" : "video";
}

gboolean
axis_profile_kind_from_string(
	const gchar     *text,
	AxisProfileKind *kind
){
	if (g_strcmp0(text, "video") == 0) {
		*kind = AXIS_PROFILE_KIND_VIDEO;
		return TRUE;
	}
	if (g_strcmp0(text, "audio") == 0) {
		*kind = AXIS_PROFILE_KIND_AUDIO;
		return TRUE;
	}
	return FALSE;
}

const gchar *const *
axis_profile_resolutions(void)
{
	return resolutions;
}

const gchar *const *
axis_profile_library_names(AxisProfileKind kind)
{
	return kind == AXIS_PROFILE_KIND_AUDIO ? audio_libraries : video_libraries;
}

gboolean
axis_profile_library_type(
	const gchar   *name,
	AxisMediaType *type
){
	static const struct
	{
		const gchar   *name;
		AxisMediaType  type;
	} map[] = {
		{ "movies", AXIS_MEDIA_MOVIES }, { "tv", AXIS_MEDIA_TV }, { "other", AXIS_MEDIA_OTHER },
		{ "music", AXIS_MEDIA_MUSIC }, { "audiobooks", AXIS_MEDIA_AUDIOBOOKS }
	};
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(map); i++) {
		if (strcmp(map[i].name, name) == 0) {
			*type = map[i].type;
			return TRUE;
		}
	}
	return FALSE;
}

/* The roots a kind's libraries may name, as AXIS_TARGET_ROOT bits. */
static guint
library_mask(AxisProfileKind kind)
{
	const gchar *const *names = axis_profile_library_names(kind);
	guint mask = 0;
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		AxisMediaType type;

		if (axis_profile_library_type(names[i], &type))
			mask |= AXIS_TARGET_ROOT(type);
	}
	return mask;
}

const gint *
axis_profile_crf_values(AxisTargetCodec codec)
{
	switch (codec) {
	case AXIS_TCODEC_HEVC:
		return hevc_crfs;
	case AXIS_TCODEC_H264:
		return h264_crfs;
	default:
		return no_crfs;
	}
}

gboolean
axis_profile_crf_valid(
	AxisTargetCodec codec,
	gint            crf
){
	const gint *values = axis_profile_crf_values(codec);
	guint i;

	for (i = 0; values[i] != 0; i++) {
		if (values[i] == crf)
			return axis_estimate_quality_level(codec, crf) >= AXIS_QUALITY_MIN;
	}
	return FALSE;
}

gint
axis_profile_mp3_bitrate(gint quality)
{
	return quality >= 0 && quality < (gint)G_N_ELEMENTS(mp3_bitrates) ? mp3_bitrates[quality] : 0;
}

/* ── Problems ───────────────────────────────────────────────────── */

void
axis_profile_problem_free(AxisProfileProblem *problem)
{
	if (problem == NULL)
		return;
	g_free(problem->field);
	g_free(problem->message);
	g_free(problem);
}

GPtrArray *
axis_profile_problems_new(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_problem_free);
}

const gchar *
axis_profile_problems_find(
	const GPtrArray *problems,
	const gchar     *field
){
	guint i;

	for (i = 0; problems != NULL && i < problems->len; i++) {
		const AxisProfileProblem *p = g_ptr_array_index((GPtrArray *)problems, i);

		if (g_strcmp0(p->field, field) == 0)
			return p->message;
	}
	return NULL;
}

static void
problem(
	GPtrArray   *problems,
	const gchar *field,
	const gchar *message
){
	AxisProfileProblem *p;

	if (problems == NULL)
		return;
	p = g_new0(AxisProfileProblem, 1);
	p->field = g_strdup(field != NULL ? field : "");
	p->message = g_strdup(message);
	g_ptr_array_add(problems, p);
}

static void
rule_problem(
	GPtrArray   *problems,
	guint        index,
	const gchar *member,
	const gchar *message
){
	g_autofree gchar *field = member != NULL ? g_strdup_printf("quality.rules.%u.%s", index + 1, member) :
		g_strdup_printf("quality.rules.%u", index + 1);

	problem(problems, field, message);
}

/* ── Checking values ────────────────────────────────────────────── */

static gboolean
in_range(
	gint64 value,
	gint64 low,
	gint64 high
){
	return value >= low && value <= high;
}

static gboolean
language_valid(const gchar *code)
{
	guint i;

	for (i = 0; i < 3; i++) {
		if (code[i] < 'a' || code[i] > 'z')
			return FALSE;
	}
	return code[3] == '\0' && strcmp(code, "und") != 0;
}

static gboolean
rules_equal(
	const AxisProfileRule *a,
	const AxisProfileRule *b
){
	return a->resolution == b->resolution && a->min_bytes == b->min_bytes && a->max_bytes == b->max_bytes &&
		a->crf == b->crf;
}

static gboolean
check_video(
	const AxisProfileDefinition *def,
	GPtrArray                   *problems
){
	gboolean ok = TRUE;
	guint i;
	guint j;

	if (def->codec != AXIS_TCODEC_HEVC && def->codec != AXIS_TCODEC_H264) {
		problem(problems, "codec", "A video profile's codec is HEVC or H.264.");
		return FALSE;
	}
	if (!(def->bit_depth == 8 || (def->bit_depth == 10 && def->codec == AXIS_TCODEC_HEVC))) {
		problem(problems, "bit_depth", def->codec == AXIS_TCODEC_HEVC ? "HEVC is written at 8 or 10 bits." :
			"H.264 is written at 8 bits.");
		ok = FALSE;
	}
	if (def->keep_hdr && def->codec != AXIS_TCODEC_HEVC) {
		problem(problems, "hdr", "Only an HEVC profile can keep HDR; with H.264, HDR sources are skipped.");
		ok = FALSE;
	}
	if (def->n_encoders < 1 || def->n_encoders > AXIS_ENCODER_COUNT) {
		problem(problems, "encoders", "Choose one to three encoders, in order of preference.");
		ok = FALSE;
	} else {
		for (i = 0; i < def->n_encoders; i++) {
			if ((guint)def->encoders[i] >= AXIS_ENCODER_COUNT) {
				problem(problems, "encoders", "An encoder is not one of software, nvenc and vaapi.");
				ok = FALSE;
				break;
			}
			for (j = 0; j < i; j++) {
				if (def->encoders[j] == def->encoders[i]) {
					problem(problems, "encoders", "An encoder is listed twice.");
					ok = FALSE;
				}
			}
		}
	}
	if (!axis_profile_crf_valid(def->codec, def->crf)) {
		problem(problems, "quality.default", def->codec == AXIS_TCODEC_HEVC ?
			"For HEVC the quality is an even CRF from 20 to 32." :
			"For H.264 the quality is a CRF of 18, 19, 20, 22, 24, 26 or 28.");
		ok = FALSE;
	}
	if (def->n_rules > AXIS_PROFILE_MAX_RULES) {
		problem(problems, "quality.rules", "A quality rule has at most eight conditions.");
		return FALSE;
	}
	for (i = 0; i < def->n_rules; i++) {
		const AxisProfileRule *r = &def->rules[i];

		if (r->resolution < 0 || r->resolution > N_RESOLUTIONS) {
			rule_problem(problems, i, "resolution", "The resolution is not one of sd, 720p, 1080p, 2160p and other.");
			ok = FALSE;
		}
		if (r->min_bytes != 0 && !in_range(r->min_bytes, 1, AXIS_PROFILE_MAX_RULE_BYTES)) {
			rule_problem(problems, i, "min_bytes", "The smallest size is more than nothing and at most a petabyte.");
			ok = FALSE;
		}
		if (r->max_bytes != 0 && !in_range(r->max_bytes, 1, AXIS_PROFILE_MAX_RULE_BYTES)) {
			rule_problem(problems, i, "max_bytes", "The size limit is more than nothing and at most a petabyte.");
			ok = FALSE;
		}
		if (r->min_bytes > 0 && r->max_bytes > 0 && r->min_bytes >= r->max_bytes) {
			rule_problem(problems, i, "max_bytes", "The size limit must be above the smallest size.");
			ok = FALSE;
		}
		if (r->resolution == 0 && r->min_bytes == 0 && r->max_bytes == 0) {
			rule_problem(problems, i, NULL, "A condition needs a resolution, a size or both: one without "
				"would always match (that is the default's job).");
			ok = FALSE;
		}
		if (!axis_profile_crf_valid(def->codec, r->crf)) {
			rule_problem(problems, i, "crf", def->codec == AXIS_TCODEC_HEVC ?
				"For HEVC the quality is an even CRF from 20 to 32." :
				"For H.264 the quality is a CRF of 18, 19, 20, 22, 24, 26 or 28.");
			ok = FALSE;
		}
		for (j = 0; j < i; j++) {
			if (rules_equal(&def->rules[j], r)) {
				rule_problem(problems, i, NULL, "The same condition is there twice.");
				ok = FALSE;
			}
		}
	}
	if (def->audio != AXIS_AUDIO_COPY && def->audio != AXIS_AUDIO_COPY_ADD_STEREO &&
	    def->audio != AXIS_AUDIO_STEREO_FIRST) {
		problem(problems, "audio", "Audio is copied, copied with a stereo track added, or given a stereo track "
			"first.");
		ok = FALSE;
	} else if (def->audio == AXIS_AUDIO_COPY) {
		if (def->stereo_bitrate != 0) {
			problem(problems, "stereo_bitrate", "A stereo bitrate only applies when a stereo track is added.");
			ok = FALSE;
		}
	} else if (!in_range(def->stereo_bitrate, 96000, 320000)) {
		problem(problems, "stereo_bitrate", "The stereo AAC track is 96 to 320 kbit/s.");
		ok = FALSE;
	}
	if (def->n_languages > AXIS_PROFILE_MAX_LANGUAGES) {
		problem(problems, "keep_languages", "At most sixteen languages.");
		ok = FALSE;
	} else {
		for (i = 0; i < def->n_languages; i++) {
			if (!language_valid(def->languages[i])) {
				problem(problems, "keep_languages", "A language is a three-letter ISO 639-2 code such as eng "
					"(und, unknown, is always kept and is not listed).");
				ok = FALSE;
				break;
			}
			for (j = 0; j < i; j++) {
				if (strcmp(def->languages[j], def->languages[i]) == 0) {
					problem(problems, "keep_languages", "A language is listed twice.");
					ok = FALSE;
				}
			}
		}
	}
	if (def->container != AXIS_CONTAINER_MATROSKA && def->container != AXIS_CONTAINER_MP4) {
		problem(problems, "container", "A video profile writes Matroska or MP4.");
		ok = FALSE;
	}
	if (def->subtitles != AXIS_SUBTITLES_COPY && def->subtitles != AXIS_SUBTITLES_PLAIN_TEXT) {
		problem(problems, "subtitles", "Subtitles are copied, or text only.");
		ok = FALSE;
	} else if (def->container == AXIS_CONTAINER_MP4 && def->subtitles != AXIS_SUBTITLES_PLAIN_TEXT) {
		problem(problems, "subtitles", "MP4 cannot carry image subtitles: choose text only.");
		ok = FALSE;
	}
	if (!in_range(def->efficient_milli, 10, 1000)) {
		problem(problems, "efficient_bpp_milli", "The threshold is 10 to 1000 thousandths of a bit per pixel.");
		ok = FALSE;
	}
	/* Nothing of an audio profile. */
	if (def->bitrate != 0 || def->mono_bitrate != 0 || def->vbr_quality != -1 || def->reduce_to_cd ||
	    def->allow_lossy || def->efficient_percent != 0) {
		problem(problems, "", "A video profile has an audio profile's settings.");
		ok = FALSE;
	}
	return ok;
}

static gboolean
check_audio(
	const AxisProfileDefinition *def,
	GPtrArray                   *problems
){
	gboolean lossy = def->codec == AXIS_TCODEC_OPUS || def->codec == AXIS_TCODEC_AAC || def->codec == AXIS_TCODEC_MP3;
	gboolean rated = def->codec == AXIS_TCODEC_OPUS || def->codec == AXIS_TCODEC_AAC;
	gint64 low = def->codec == AXIS_TCODEC_OPUS ? 16000 : 32000;
	gint64 high = def->codec == AXIS_TCODEC_OPUS ? 256000 : 320000;
	gboolean ok = TRUE;

	if (!lossy && def->codec != AXIS_TCODEC_FLAC) {
		problem(problems, "codec", "An audio profile's codec is FLAC, Opus, AAC or MP3.");
		return FALSE;
	}
	if (rated) {
		if (!in_range(def->bitrate, low, high)) {
			problem(problems, "bitrate", def->codec == AXIS_TCODEC_OPUS ? "Opus is 16 to 256 kbit/s." :
				"AAC is 32 to 320 kbit/s.");
			ok = FALSE;
		}
		if (def->mono_bitrate != 0 && (!in_range(def->mono_bitrate, low, high) || def->mono_bitrate >= def->bitrate)) {
			problem(problems, "mono_bitrate", "The bitrate for a mono source is below the bitrate, in the same "
				"bounds.");
			ok = FALSE;
		}
	} else if (def->bitrate != 0 || def->mono_bitrate != 0) {
		problem(problems, "bitrate", "This codec takes no bitrate.");
		ok = FALSE;
	}
	if (def->codec == AXIS_TCODEC_MP3) {
		if (!in_range(def->vbr_quality, 0, 4)) {
			problem(problems, "vbr_quality", "MP3 is LAME V0 to V4.");
			ok = FALSE;
		}
	} else if (def->vbr_quality != -1) {
		problem(problems, "vbr_quality", "Only MP3 takes a VBR quality.");
		ok = FALSE;
	}
	if (def->reduce_to_cd && def->codec != AXIS_TCODEC_FLAC) {
		problem(problems, "reduce_to_cd", "Only FLAC is reduced to CD quality.");
		ok = FALSE;
	}
	if (lossy) {
		if (!in_range(def->efficient_percent, 100, 400)) {
			problem(problems, "efficient_percent", "The threshold is 100 to 400% of the bitrate.");
			ok = FALSE;
		}
	} else {
		if (def->allow_lossy) {
			problem(problems, "allow_lossy_source", "FLAC from a lossy source keeps the loss and is larger: it "
				"is never allowed.");
			ok = FALSE;
		}
		if (def->efficient_percent != 0) {
			problem(problems, "efficient_percent", "FLAC takes no threshold.");
			ok = FALSE;
		}
	}
	/* Nothing of a video profile. */
	if (def->bit_depth != 0 || def->keep_hdr || def->n_encoders != 0 || def->crf != 0 || def->n_rules != 0 ||
	    def->audio != AXIS_AUDIO_COPY || def->stereo_bitrate != 0 || def->n_languages != 0 ||
	    def->subtitles != AXIS_SUBTITLES_COPY || def->container != AXIS_CONTAINER_MATROSKA ||
	    def->efficient_milli != 0) {
		problem(problems, "", "An audio profile has a video profile's settings.");
		ok = FALSE;
	}
	return ok;
}

gboolean
axis_profile_definition_check(
	const AxisProfileDefinition *def,
	GPtrArray                   *problems
){
	gboolean ok = TRUE;
	guint mask;

	g_return_val_if_fail(def != NULL, FALSE);

	if (def->kind != AXIS_PROFILE_KIND_VIDEO && def->kind != AXIS_PROFILE_KIND_AUDIO) {
		problem(problems, "kind", "A profile is for video or for audio.");
		return FALSE;
	}
	mask = library_mask(def->kind);
	if (def->libraries == 0 || (def->libraries & ~mask) != 0) {
		problem(problems, "libraries", def->kind == AXIS_PROFILE_KIND_VIDEO ?
			"Choose at least one of movies, series and other video." : "Choose music, audiobooks or both.");
		ok = FALSE;
	}
	if (!in_range(def->min_saving_percent, AXIS_PROFILE_MIN_SAVING_FLOOR, AXIS_PROFILE_MAX_SAVING)) {
		problem(problems, "min_saving_percent", "The minimum saving is 5 to 95%: never below 5%.");
		ok = FALSE;
	}
	if (!in_range(def->min_saving_bytes, 0, AXIS_PROFILE_MAX_SAVING_BYTES)) {
		problem(problems, "min_saving_bytes", "The minimum saving in bytes is from nothing to a terabyte.");
		ok = FALSE;
	}
	if (def->kind == AXIS_PROFILE_KIND_VIDEO)
		return check_video(def, problems) && ok;
	return check_audio(def, problems) && ok;
}

/* ── Normal order ───────────────────────────────────────────────── */

static gint64
rule_low(const AxisProfileRule *r)
{
	return r->min_bytes > 0 ? r->min_bytes : 0;
}

static gint64
rule_high(const AxisProfileRule *r)
{
	return r->max_bytes > 0 ? r->max_bytes : G_MAXINT64;
}

/* Whether no file matches both. */
static gboolean
rules_disjoint(
	const AxisProfileRule *a,
	const AxisProfileRule *b
){
	if (a->resolution != 0 && b->resolution != 0 && a->resolution != b->resolution)
		return TRUE;
	return rule_high(a) <= rule_low(b) || rule_high(b) <= rule_low(a);
}

/* Whether the two may be swapped when next to each other without changing
 * what any file gets: no file matches both, or they give the same. */
static gboolean
rules_commute(
	const AxisProfileRule *a,
	const AxisProfileRule *b
){
	return a->crf == b->crf || rules_disjoint(a, b);
}

static gint
rule_compare(
	const AxisProfileRule *a,
	const AxisProfileRule *b
){
	if (a->resolution != b->resolution)
		return a->resolution < b->resolution ? -1 : 1;
	if (rule_low(a) != rule_low(b))
		return rule_low(a) < rule_low(b) ? -1 : 1;
	if (rule_high(a) != rule_high(b))
		return rule_high(a) < rule_high(b) ? -1 : 1;
	if (a->crf != b->crf)
		return a->crf < b->crf ? -1 : 1;
	return 0;
}

/*
 * The rules in their normal order: of all the orders reachable by swapping
 * neighbours that commute (which all mean the same), the smallest by
 * rule_compare().  Picks, again and again, the smallest rule that commutes
 * with every rule still before it.
 */
static void
normalize_rules(AxisProfileDefinition *def)
{
	AxisProfileRule remaining[AXIS_PROFILE_MAX_RULES];
	guint n = MIN(def->n_rules, AXIS_PROFILE_MAX_RULES);
	guint out = 0;

	memcpy(remaining, def->rules, n * sizeof(AxisProfileRule));
	while (n > 0) {
		gint best = -1;
		guint i;
		guint j;

		for (i = 0; i < n; i++) {
			gboolean free_to_move = TRUE;

			for (j = 0; j < i && free_to_move; j++) {
				if (!rules_commute(&remaining[j], &remaining[i]))
					free_to_move = FALSE;
			}
			if (free_to_move && (best < 0 || rule_compare(&remaining[i], &remaining[best]) < 0))
				best = (gint)i;
		}
		def->rules[out++] = remaining[best];
		memmove(&remaining[best], &remaining[best + 1], (n - (guint)best - 1) * sizeof(AxisProfileRule));
		n--;
	}
}

static int
compare_languages(
	const void *a,
	const void *b
){
	return strcmp((const gchar *)a, (const gchar *)b);
}

static void
normalize(AxisProfileDefinition *def)
{
	if (def->n_languages > 1 && def->n_languages <= AXIS_PROFILE_MAX_LANGUAGES)
		qsort(def->languages, def->n_languages, sizeof(def->languages[0]), compare_languages);
	if (def->n_rules <= AXIS_PROFILE_MAX_RULES)
		normalize_rules(def);
}

/* ── Reading ────────────────────────────────────────────────────── */

typedef struct
{
	const AxisJsonNode *object;
	const gchar        *prefix;	/* for problems: "" or "quality." */
	GPtrArray          *problems;
	gboolean            ok;
} Reader;

/* The member called @name, or %NULL. */
static const AxisJsonNode *
member(
	Reader      *r,
	const gchar *name
){
	return axis_json_member(r->object, name);
}

static gchar *
field_of(
	Reader      *r,
	const gchar *name
){
	return g_strconcat(r->prefix, name, NULL);
}

static void
fail(
	Reader      *r,
	const gchar *name,
	const gchar *message
){
	g_autofree gchar *field = name != NULL ? field_of(r, name) : g_strdup("");

	problem(r->problems, field, message);
	r->ok = FALSE;
}

/* A required member is there; a member that does not apply is not. */
static const AxisJsonNode *
need(
	Reader      *r,
	const gchar *name
){
	const AxisJsonNode *node = member(r, name);

	if (node == NULL)
		fail(r, name, "This is missing.");
	return node;
}

static void
refuse(
	Reader      *r,
	const gchar *name
){
	if (member(r, name) != NULL)
		fail(r, name, "This does not apply here.");
}

/* A whole number, any value: the bounds are checked on the definition
 * (axis_profile_definition_check()), never here. */
static gboolean
read_integer(
	Reader      *r,
	const gchar *name,
	gint64      *value
){
	const AxisJsonNode *node = need(r, name);

	if (node == NULL)
		return FALSE;
	if (axis_json_type(node) != AXIS_JSON_NUMBER || !axis_json_get_integer(node, G_MININT64, G_MAXINT64, value)) {
		fail(r, name, "This is a whole number.");
		return FALSE;
	}
	return TRUE;
}

/* As above for a member the definition holds in an int: a number that does
 * not fit becomes G_MININT32, which every bound refuses. */
static gboolean
read_int(
	Reader      *r,
	const gchar *name,
	gint        *value
){
	gint64 wide = 0;

	if (!read_integer(r, name, &wide))
		return FALSE;
	*value = wide >= G_MININT32 && wide <= G_MAXINT32 ? (gint)wide : G_MININT32;
	return TRUE;
}

static gboolean
read_boolean(
	Reader      *r,
	const gchar *name,
	gboolean    *value
){
	const AxisJsonNode *node = need(r, name);

	if (node == NULL)
		return FALSE;
	if (axis_json_type(node) != AXIS_JSON_BOOLEAN || !axis_json_get_boolean(node, value)) {
		fail(r, name, "This is true or false.");
		return FALSE;
	}
	return TRUE;
}

/* A string from @choices; its index, or -1. */
static gint
read_choice(
	Reader             *r,
	const gchar        *name,
	const gchar *const *choices,
	const gchar        *message
){
	const AxisJsonNode *node = need(r, name);
	const gchar *text;
	guint i;

	if (node == NULL)
		return -1;
	text = axis_json_type(node) == AXIS_JSON_STRING ? axis_json_string(node) : NULL;
	for (i = 0; text != NULL && choices[i] != NULL; i++) {
		if (strcmp(text, choices[i]) == 0)
			return (gint)i;
	}
	fail(r, name, message);
	return -1;
}

/* Every member is known and none is there twice. */
static void
check_members(
	Reader             *r,
	const gchar *const *known
){
	guint n = axis_json_length(r->object);
	guint i;
	guint j;

	if (n > MAX_MEMBERS) {
		fail(r, NULL, "The definition has more members than the format has.");
		return;
	}
	for (i = 0; i < n; i++) {
		const gchar *name = axis_json_member_name(r->object, i);

		if (name == NULL || !g_strv_contains(known, name)) {
			fail(r, NULL, r->prefix[0] == '\0' ? "The definition has a member the format does not define." :
				"The quality rule has a member the format does not define.");
			continue;
		}
		for (j = 0; j < i; j++) {
			if (g_strcmp0(axis_json_member_name(r->object, j), name) == 0)
				fail(r, name, "This is given twice.");
		}
	}
}

static void
read_libraries(
	Reader                *r,
	AxisProfileDefinition *def
){
	const AxisJsonNode *node = need(r, "libraries");
	const gchar *const *names = axis_profile_library_names(def->kind);
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) > 8) {
		fail(r, "libraries", "This is a list of libraries.");
		return;
	}
	for (i = 0; i < axis_json_length(node); i++) {
		const AxisJsonNode *item = axis_json_element(node, i);
		const gchar *text = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;
		AxisMediaType type;
		guint bit;

		if (text == NULL || !g_strv_contains(names, text) || !axis_profile_library_type(text, &type)) {
			fail(r, "libraries", def->kind == AXIS_PROFILE_KIND_VIDEO ?
				"A video profile's libraries are movies, tv and other." :
				"An audio profile's libraries are music and audiobooks.");
			return;
		}
		bit = AXIS_TARGET_ROOT(type);
		if ((def->libraries & bit) != 0) {
			fail(r, "libraries", "A library is listed twice.");
			return;
		}
		def->libraries |= bit;
	}
}

static void
read_encoders(
	Reader                *r,
	AxisProfileDefinition *def
){
	static const gchar *const families[] = { "software", "nvenc", "vaapi", NULL };
	const AxisJsonNode *node = need(r, "encoders");
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) > AXIS_ENCODER_COUNT ||
	    axis_json_length(node) == 0) {
		fail(r, "encoders", "This is a list of one to three encoders.");
		return;
	}
	for (i = 0; i < axis_json_length(node); i++) {
		const AxisJsonNode *item = axis_json_element(node, i);
		const gchar *text = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;
		AxisEncoderFamily family;

		if (text == NULL || !g_strv_contains(families, text) || !axis_encoder_family_from_string(text, &family)) {
			fail(r, "encoders", "An encoder is not one of software, nvenc and vaapi.");
			return;
		}
		def->encoders[def->n_encoders++] = family;
	}
}

static void
read_languages(
	Reader                *r,
	AxisProfileDefinition *def
){
	const AxisJsonNode *node = need(r, "keep_languages");
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) > AXIS_PROFILE_MAX_LANGUAGES) {
		fail(r, "keep_languages", "This is a list of at most sixteen language codes.");
		return;
	}
	for (i = 0; i < axis_json_length(node); i++) {
		const AxisJsonNode *item = axis_json_element(node, i);
		const gchar *text = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;

		if (text == NULL || strlen(text) != 3) {
			fail(r, "keep_languages", "A language is a three-letter ISO 639-2 code such as eng.");
			return;
		}
		memcpy(def->languages[def->n_languages], text, 4);
		def->n_languages++;
	}
}

static void
read_rule(
	Reader          *parent,
	const AxisJsonNode *node,
	guint            index,
	AxisProfileRule *rule
){
	static const gchar *const known[] = { "resolution", "min_bytes", "max_bytes", "crf", NULL };
	g_autofree gchar *prefix = g_strdup_printf("quality.rules.%u.", index + 1);
	Reader r;
	gint64 value = 0;

	memset(rule, 0, sizeof(*rule));
	r.object = node;
	r.prefix = prefix;
	r.problems = parent->problems;
	r.ok = TRUE;
	if (axis_json_type(node) != AXIS_JSON_OBJECT) {
		rule_problem(parent->problems, index, NULL, "A condition is an object.");
		parent->ok = FALSE;
		return;
	}
	check_members(&r, known);
	if (member(&r, "resolution") != NULL) {
		gint at = read_choice(&r, "resolution", resolutions, "The resolution is one of sd, 720p, 1080p, 2160p "
			"and other.");

		rule->resolution = at >= 0 ? at + 1 : -1;
	}
	/* Present means a size: 0 or less is refused (by the check), not read
	 * as "none". */
	if (member(&r, "min_bytes") != NULL && read_integer(&r, "min_bytes", &value))
		rule->min_bytes = value < 1 ? -1 : value;
	if (member(&r, "max_bytes") != NULL && read_integer(&r, "max_bytes", &value))
		rule->max_bytes = value < 1 ? -1 : value;
	read_int(&r, "crf", &rule->crf);
	if (!r.ok)
		parent->ok = FALSE;
}

static void
read_quality(
	Reader                *r,
	AxisProfileDefinition *def
){
	static const gchar *const known[] = { "default", "rules", NULL };
	const AxisJsonNode *node = need(r, "quality");
	const AxisJsonNode *rules;
	Reader q;
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_OBJECT) {
		fail(r, "quality", "The quality is an object with a default and rules.");
		return;
	}
	q.object = node;
	q.prefix = "quality.";
	q.problems = r->problems;
	q.ok = TRUE;
	check_members(&q, known);
	read_int(&q, "default", &def->crf);
	rules = need(&q, "rules");
	if (rules != NULL) {
		if (axis_json_type(rules) != AXIS_JSON_ARRAY || axis_json_length(rules) > AXIS_PROFILE_MAX_RULES) {
			fail(&q, "rules", "A quality rule has at most eight conditions.");
		} else {
			for (i = 0; i < axis_json_length(rules); i++) {
				read_rule(&q, axis_json_element(rules, i), i, &def->rules[def->n_rules]);
				def->n_rules++;
			}
		}
	}
	if (!q.ok)
		r->ok = FALSE;
}

static void
read_video(
	Reader                *r,
	AxisProfileDefinition *def
){
	static const gchar *const codecs[] = { "hevc", "h264", NULL };
	static const gchar *const hdr[] = { "skip", "keep", NULL };
	static const gchar *const audio[] = { "copy", "copy_add_stereo", "stereo_first", NULL };
	static const gchar *const subtitles[] = { "copy", "text_only", NULL };
	static const gchar *const containers[] = { "mkv", "mp4", NULL };
	gint at;

	at = read_choice(r, "codec", codecs, "A video profile's codec is hevc or h264.");
	def->codec = at == 1 ? AXIS_TCODEC_H264 : AXIS_TCODEC_HEVC;
	read_int(r, "bit_depth", &def->bit_depth);
	at = read_choice(r, "hdr", hdr, "HDR is keep or skip.");
	def->keep_hdr = at == 1;
	read_encoders(r, def);
	read_quality(r, def);
	at = read_choice(r, "audio", audio, "Audio is copy, copy_add_stereo or stereo_first.");
	def->audio = at == 1 ? AXIS_AUDIO_COPY_ADD_STEREO : at == 2 ? AXIS_AUDIO_STEREO_FIRST : AXIS_AUDIO_COPY;
	if (at == 1 || at == 2)
		read_int(r, "stereo_bitrate", &def->stereo_bitrate);
	else
		refuse(r, "stereo_bitrate");
	read_languages(r, def);
	at = read_choice(r, "subtitles", subtitles, "Subtitles are copy or text_only.");
	def->subtitles = at == 1 ? AXIS_SUBTITLES_PLAIN_TEXT : AXIS_SUBTITLES_COPY;
	at = read_choice(r, "container", containers, "The container is mkv or mp4.");
	def->container = at == 1 ? AXIS_CONTAINER_MP4 : AXIS_CONTAINER_MATROSKA;
	read_int(r, "efficient_bpp_milli", &def->efficient_milli);
}

static void
read_audio(
	Reader                *r,
	AxisProfileDefinition *def
){
	static const gchar *const codecs[] = { "flac", "opus", "aac", "mp3", NULL };
	static const AxisTargetCodec values[] = { AXIS_TCODEC_FLAC, AXIS_TCODEC_OPUS, AXIS_TCODEC_AAC, AXIS_TCODEC_MP3 };
	gint at;

	at = read_choice(r, "codec", codecs, "An audio profile's codec is flac, opus, aac or mp3.");
	if (at < 0)
		return;
	def->codec = values[at];
	if (def->codec == AXIS_TCODEC_OPUS || def->codec == AXIS_TCODEC_AAC) {
		read_int(r, "bitrate", &def->bitrate);
		if (member(r, "mono_bitrate") != NULL)
			read_int(r, "mono_bitrate", &def->mono_bitrate);
	} else {
		refuse(r, "bitrate");
		refuse(r, "mono_bitrate");
	}
	if (def->codec == AXIS_TCODEC_MP3)
		read_int(r, "vbr_quality", &def->vbr_quality);
	else
		refuse(r, "vbr_quality");
	if (def->codec == AXIS_TCODEC_FLAC) {
		read_boolean(r, "reduce_to_cd", &def->reduce_to_cd);
		refuse(r, "allow_lossy_source");
		refuse(r, "efficient_percent");
	} else {
		refuse(r, "reduce_to_cd");
		read_boolean(r, "allow_lossy_source", &def->allow_lossy);
		read_int(r, "efficient_percent", &def->efficient_percent);
	}
}

gboolean
axis_profile_definition_parse(
	const gchar           *text,
	gssize                 length,
	AxisProfileDefinition *def,
	GPtrArray             *problems
){
	static const gchar *const video_known[] = {
		"format", "kind", "libraries", "codec", "bit_depth", "hdr", "encoders", "quality", "audio", "stereo_bitrate",
		"keep_languages", "subtitles", "container", "efficient_bpp_milli", "min_saving_percent", "min_saving_bytes",
		NULL
	};
	static const gchar *const audio_known[] = {
		"format", "kind", "libraries", "codec", "bitrate", "mono_bitrate", "vbr_quality", "reduce_to_cd",
		"allow_lossy_source", "efficient_percent", "min_saving_percent", "min_saving_bytes", NULL
	};
	static const gchar *const kinds[] = { "video", "audio", NULL };
	g_autoptr(AxisJsonDoc) doc = NULL;
	AxisProfileDefinition local;
	AxisProfileDefinition *out = def != NULL ? def : &local;
	Reader r;
	gsize size;
	gint64 format = 0;
	gint at;

	memset(out, 0, sizeof(*out));
	out->vbr_quality = -1;
	if (text == NULL) {
		problem(problems, "", "There is no definition.");
		return FALSE;
	}
	size = length < 0 ? strlen(text) : (gsize)length;
	if (size > AXIS_PROFILE_MAX_TEXT) {
		problem(problems, "", "The definition is longer than the format allows.");
		return FALSE;
	}
	doc = axis_json_parse(text, size, NULL);
	if (doc == NULL || axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_OBJECT) {
		problem(problems, "", "The definition is not a JSON object.");
		return FALSE;
	}

	r.object = axis_json_doc_get_root(doc);
	r.prefix = "";
	r.problems = problems;
	r.ok = TRUE;
	/* Another format is not read at all: its members may mean something
	 * else. */
	if (!read_integer(&r, "format", &format) || format != AXIS_PROFILE_FORMAT) {
		if (r.ok)
			fail(&r, "format", "The definition is of a format this version does not read.");
		memset(out, 0, sizeof(*out));
		return FALSE;
	}
	at = read_choice(&r, "kind", kinds, "The kind is video or audio.");
	if (at < 0) {
		memset(out, 0, sizeof(*out));
		return FALSE;
	}
	out->kind = at == 1 ? AXIS_PROFILE_KIND_AUDIO : AXIS_PROFILE_KIND_VIDEO;
	check_members(&r, out->kind == AXIS_PROFILE_KIND_VIDEO ? video_known : audio_known);
	read_libraries(&r, out);
	if (out->kind == AXIS_PROFILE_KIND_VIDEO)
		read_video(&r, out);
	else
		read_audio(&r, out);
	read_int(&r, "min_saving_percent", &out->min_saving_percent);
	{
		gint64 bytes = 0;

		if (read_integer(&r, "min_saving_bytes", &bytes))
			out->min_saving_bytes = bytes;
	}
	if (!r.ok || !axis_profile_definition_check(out, problems)) {
		memset(out, 0, sizeof(*out));
		return FALSE;
	}
	normalize(out);
	return TRUE;
}

/* ── Writing ────────────────────────────────────────────────────── */

typedef struct
{
	const gchar *name;
	gchar       *json;
} Member;

static void
add_member(
	GArray      *members,
	const gchar *name,
	gchar       *json
){
	Member m;

	m.name = name;
	m.json = json;
	g_array_append_val(members, m);
}

static gint
compare_members(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(((const Member *)a)->name, ((const Member *)b)->name);
}

/* @value as a JSON string: only the characters JSON needs escaped are. */
static gchar *
json_string(const gchar *value)
{
	GString *out = g_string_new("\"");
	const guchar *p;

	for (p = (const guchar *)value; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_printf(out, "\\%c", *p);
		else if (*p < 0x20)
			g_string_append_printf(out, "\\u%04x", *p);
		else
			g_string_append_c(out, (gchar)*p);
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

static gchar *
json_int(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static gchar *
json_bool(gboolean value)
{
	return g_strdup(value ? "true" : "false");
}

/* The members in byte order of their names. */
static gchar *
json_object(GArray *members)
{
	GString *out = g_string_new("{");
	guint i;

	g_array_sort(members, compare_members);
	for (i = 0; i < members->len; i++) {
		Member *m = &g_array_index(members, Member, i);

		g_string_append_printf(out, "%s\"%s\":%s", i > 0 ? "," : "", m->name, m->json);
		g_free(m->json);
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static gchar *
json_libraries(const AxisProfileDefinition *def)
{
	static const gchar *const order[] = { "movies", "tv", "music", "audiobooks", "other", NULL };
	GString *out = g_string_new("[");
	guint n = 0;
	guint i;

	/* In the order of the root types. */
	for (i = 0; order[i] != NULL; i++) {
		AxisMediaType type;

		if (axis_profile_library_type(order[i], &type) && (def->libraries & AXIS_TARGET_ROOT(type)) != 0)
			g_string_append_printf(out, "%s\"%s\"", n++ > 0 ? "," : "", order[i]);
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

static gchar *
json_rules(const AxisProfileDefinition *def)
{
	GString *out = g_string_new("[");
	guint i;

	for (i = 0; i < def->n_rules && i < AXIS_PROFILE_MAX_RULES; i++) {
		const AxisProfileRule *rule = &def->rules[i];
		g_autoptr(GArray) members = g_array_new(FALSE, FALSE, sizeof(Member));
		g_autofree gchar *object = NULL;

		add_member(members, "crf", json_int(rule->crf));
		if (rule->resolution != 0)
			add_member(members, "resolution", rule->resolution >= 1 && rule->resolution <= N_RESOLUTIONS ?
				json_string(resolutions[rule->resolution - 1]) : json_int(rule->resolution));
		if (rule->min_bytes != 0)
			add_member(members, "min_bytes", json_int(rule->min_bytes));
		if (rule->max_bytes != 0)
			add_member(members, "max_bytes", json_int(rule->max_bytes));
		object = json_object(members);
		g_string_append_printf(out, "%s%s", i > 0 ? "," : "", object);
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

gchar *
axis_profile_definition_canonical(const AxisProfileDefinition *def)
{
	g_autoptr(GArray) members = g_array_new(FALSE, FALSE, sizeof(Member));
	AxisProfileDefinition copy;
	guint i;

	g_return_val_if_fail(def != NULL, NULL);

	/* A value out of its enumeration is written as "?", which parse()
	 * refuses: the writer never turns a broken definition into a valid
	 * one. */
	copy = *def;
	normalize(&copy);
	add_member(members, "format", json_int(AXIS_PROFILE_FORMAT));
	add_member(members, "kind", json_string(copy.kind == AXIS_PROFILE_KIND_VIDEO ? "video" :
		copy.kind == AXIS_PROFILE_KIND_AUDIO ? "audio" : "?"));
	add_member(members, "libraries", json_libraries(&copy));
	add_member(members, "codec", json_string(axis_target_codec_to_string(copy.codec)));
	add_member(members, "min_saving_percent", json_int(copy.min_saving_percent));
	add_member(members, "min_saving_bytes", json_int(copy.min_saving_bytes));
	if (copy.kind == AXIS_PROFILE_KIND_VIDEO) {
		GString *list = g_string_new("[");
		g_autoptr(GArray) quality = g_array_new(FALSE, FALSE, sizeof(Member));

		add_member(members, "bit_depth", json_int(copy.bit_depth));
		add_member(members, "hdr", json_string(copy.keep_hdr ? "keep" : "skip"));
		for (i = 0; i < copy.n_encoders && i < AXIS_ENCODER_COUNT; i++)
			g_string_append_printf(list, "%s\"%s\"", i > 0 ? "," : "", (guint)copy.encoders[i] < AXIS_ENCODER_COUNT ?
				axis_encoder_family_to_string(copy.encoders[i]) : "?");
		g_string_append_c(list, ']');
		add_member(members, "encoders", g_string_free(list, FALSE));
		add_member(quality, "default", json_int(copy.crf));
		add_member(quality, "rules", json_rules(&copy));
		add_member(members, "quality", json_object(quality));
		add_member(members, "audio", json_string(copy.audio == AXIS_AUDIO_COPY_ADD_STEREO ? "copy_add_stereo" :
			copy.audio == AXIS_AUDIO_STEREO_FIRST ? "stereo_first" : copy.audio == AXIS_AUDIO_COPY ? "copy" : "?"));
		if (copy.audio == AXIS_AUDIO_COPY_ADD_STEREO || copy.audio == AXIS_AUDIO_STEREO_FIRST)
			add_member(members, "stereo_bitrate", json_int(copy.stereo_bitrate));
		list = g_string_new("[");
		for (i = 0; i < copy.n_languages && i < AXIS_PROFILE_MAX_LANGUAGES; i++) {
			gchar code[4];
			g_autofree gchar *quoted = NULL;

			memcpy(code, copy.languages[i], 3);
			code[3] = '\0';
			quoted = json_string(code);
			g_string_append_printf(list, "%s%s", i > 0 ? "," : "", quoted);
		}
		g_string_append_c(list, ']');
		add_member(members, "keep_languages", g_string_free(list, FALSE));
		add_member(members, "subtitles", json_string(copy.subtitles == AXIS_SUBTITLES_PLAIN_TEXT ? "text_only" :
			copy.subtitles == AXIS_SUBTITLES_COPY ? "copy" : "?"));
		add_member(members, "container", json_string(copy.container == AXIS_CONTAINER_MP4 ? "mp4" :
			copy.container == AXIS_CONTAINER_MATROSKA ? "mkv" : "?"));
		add_member(members, "efficient_bpp_milli", json_int(copy.efficient_milli));
	} else {
		if (copy.codec == AXIS_TCODEC_OPUS || copy.codec == AXIS_TCODEC_AAC) {
			add_member(members, "bitrate", json_int(copy.bitrate));
			if (copy.mono_bitrate != 0)
				add_member(members, "mono_bitrate", json_int(copy.mono_bitrate));
		}
		if (copy.codec == AXIS_TCODEC_MP3)
			add_member(members, "vbr_quality", json_int(copy.vbr_quality));
		if (copy.codec == AXIS_TCODEC_FLAC) {
			add_member(members, "reduce_to_cd", json_bool(copy.reduce_to_cd));
		} else {
			add_member(members, "allow_lossy_source", json_bool(copy.allow_lossy));
			add_member(members, "efficient_percent", json_int(copy.efficient_percent));
		}
	}
	return json_object(members);
}

gchar *
axis_profile_hash(const gchar *canonical)
{
	g_return_val_if_fail(canonical != NULL, NULL);

	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical, -1);
}

gboolean
axis_profile_definition_equal(
	const AxisProfileDefinition *a,
	const AxisProfileDefinition *b
){
	g_autofree gchar *x = axis_profile_definition_canonical(a);
	g_autofree gchar *y = axis_profile_definition_canonical(b);

	return strcmp(x, y) == 0;
}

gint
axis_profile_quality_for(
	const AxisProfileDefinition *def,
	const gchar                 *resolution_class,
	gint64                       size
){
	guint i;

	for (i = 0; i < def->n_rules && i < AXIS_PROFILE_MAX_RULES; i++) {
		const AxisProfileRule *rule = &def->rules[i];
		const gchar *res = rule->resolution >= 1 && rule->resolution <= N_RESOLUTIONS ?
			resolutions[rule->resolution - 1] : NULL;

		if (rule->resolution < 0 || rule->resolution > N_RESOLUTIONS)
			continue;
		if (axis_estimate_rule_matches(res, rule->min_bytes, rule->max_bytes, resolution_class, size))
			return rule->crf;
	}
	return def->crf;
}

/* ── Slugs and names ────────────────────────────────────────────── */

gboolean
axis_profile_slug_valid(const gchar *slug)
{
	guint i;

	if (slug == NULL || slug[0] < 'a' || slug[0] > 'z')
		return FALSE;
	for (i = 1; slug[i] != '\0'; i++) {
		if (i > 40 || !((slug[i] >= 'a' && slug[i] <= 'z') || (slug[i] >= '0' && slug[i] <= '9') || slug[i] == '_'))
			return FALSE;
	}
	return TRUE;
}

/* Characters a name never keeps: they change how text around them is
 * shown (direction overrides and isolates, zero-width joiners). */
static gboolean
invisible(gunichar c)
{
	return (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069) ||
		c == 0xfeff || c == 0x2028 || c == 0x2029;
}

gchar *
axis_profile_clean_text(
	const gchar *text,
	guint        max_chars,
	gboolean     multiline
){
	GString *out;
	const gchar *p;
	guint chars = 0;
	gboolean space = FALSE;

	if (text == NULL || !g_utf8_validate(text, -1, NULL))
		return NULL;
	out = g_string_new(NULL);
	for (p = text; *p != '\0' && chars < max_chars; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (multiline && c == '\n') {
			while (out->len > 0 && out->str[out->len - 1] == ' ')
				g_string_truncate(out, out->len - 1);
			if (out->len > 0) {
				g_string_append_c(out, '\n');
				chars++;
			}
			space = FALSE;
			continue;
		}
		if (invisible(c))
			continue;
		if (g_unichar_iscntrl(c) || g_unichar_isspace(c)) {
			space = TRUE;
			continue;
		}
		if (space && out->len > 0 && out->str[out->len - 1] != '\n') {
			g_string_append_c(out, ' ');
			chars++;
			if (chars >= max_chars)
				break;
		}
		space = FALSE;
		g_string_append_unichar(out, c);
		chars++;
	}
	while (out->len > 0 && (out->str[out->len - 1] == ' ' || out->str[out->len - 1] == '\n'))
		g_string_truncate(out, out->len - 1);
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}
