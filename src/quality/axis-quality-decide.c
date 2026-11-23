/*
 * axis-quality-decide.c - the decision: every release accepted or rejected
 * with every reason, the accepted ranked with their keys, and the upgrade
 * against the file on disk
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure: everything it needs is in its arguments, the time included.  Every
 * check is made for every release, so that a rejection lists all its
 * reasons, in a fixed order: the fixed rules, then the quality, the size,
 * the profile's rules, the language, the availability, and last the file
 * on disk.
 */

#include "quality/axis-quality-private.h"

#include <stdarg.h>
#include <string.h>

#include "core/axis-size.h"

/* The profile's size limits are in mebibytes (a limit per minute, MiB per
 * minute): the units the rest of the interface writes. */
#define MIB G_GINT64_CONSTANT(1048576)
#define DAY_US G_GINT64_CONSTANT(86400000000)

/* ── Sizes in words ─────────────────────────────────────────────── */

gchar *
axis_quality_format_size(gint64 bytes)
{
	if (bytes < 0)
		return g_strdup("an unknown size");
	return axis_size_format((guint64)bytes);
}

/* Two sizes in the same unit, with as many decimals as it takes for two
 * different sizes to read differently ("2.43 GiB is above the 2.42 GiB
 * limit", never "2.4 GiB is above the 2.4 GiB limit"). */
static void
format_pair(
	gint64   a,
	gint64   b,
	gchar  **ta,
	gchar  **tb
){
	guint unit = axis_size_unit_of((guint64)MAX(MAX(a, b), 0));
	guint decimals = 1;

	for (;;) {
		*ta = axis_size_format_as((guint64)MAX(a, 0), unit, decimals);
		*tb = axis_size_format_as((guint64)MAX(b, 0), unit, decimals);
		if (a == b || strcmp(*ta, *tb) != 0 || decimals >= 3)
			return;
		g_free(*ta);
		g_free(*tb);
		decimals++;
	}
}

/* ── Verdicts ───────────────────────────────────────────────────── */

static void
reason_free(AxisQualityReason *reason)
{
	g_free(reason->text);
	g_free(reason);
}

static void
line_free(AxisQualityScoreLine *line)
{
	g_free(line->text);
	g_free(line);
}

void
axis_quality_verdict_free(AxisQualityVerdict *verdict)
{
	if (verdict == NULL)
		return;
	g_free(verdict->upgrade_text);
	g_clear_pointer(&verdict->reasons, g_ptr_array_unref);
	g_clear_pointer(&verdict->lines, g_ptr_array_unref);
	g_clear_pointer(&verdict->notes, g_ptr_array_unref);
	g_free(verdict);
}

void
axis_quality_decision_free(AxisQualityDecision *decision)
{
	if (decision == NULL)
		return;
	g_clear_pointer(&decision->verdicts, g_ptr_array_unref);
	g_free(decision->order);
	g_free(decision->disk_text);
	g_free(decision);
}

static AxisQualityVerdict *
verdict_new(void)
{
	AxisQualityVerdict *v = g_new0(AxisQualityVerdict, 1);

	v->reasons = g_ptr_array_new_with_free_func((GDestroyNotify)reason_free);
	v->lines = g_ptr_array_new_with_free_func((GDestroyNotify)line_free);
	v->notes = g_ptr_array_new_with_free_func(g_free);
	return v;
}

static void reason(AxisQualityVerdict *v, AxisQualityReasonKind kind, const gchar *code, const gchar *format, ...)
	G_GNUC_PRINTF(4, 5);

static void
reason(
	AxisQualityVerdict    *v,
	AxisQualityReasonKind  kind,
	const gchar           *code,
	const gchar           *format,
	...
){
	AxisQualityReason *r = g_new0(AxisQualityReason, 1);
	va_list args;

	va_start(args, format);
	r->text = g_strdup_vprintf(format, args);
	va_end(args);
	r->kind = kind;
	r->code = code;
	g_ptr_array_add(v->reasons, r);
}

static void note(AxisQualityVerdict *v, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static void
note(
	AxisQualityVerdict *v,
	const gchar        *format,
	...
){
	va_list args;

	va_start(args, format);
	g_ptr_array_add(v->notes, g_strdup_vprintf(format, args));
	va_end(args);
}

static void
score_line(
	AxisQualityVerdict *v,
	gint                points,
	gchar              *text
){
	AxisQualityScoreLine *line = g_new0(AxisQualityScoreLine, 1);

	line->points = points;
	line->text = text;
	g_ptr_array_add(v->lines, line);
}

/* ── The players ────────────────────────────────────────────────── */

static const gchar *
compat_codec(AxisReleaseCodec codec)
{
	switch (codec) {
	case AXIS_RELEASE_CODEC_H264:  return "h264";
	case AXIS_RELEASE_CODEC_H265:  return "hevc";
	case AXIS_RELEASE_CODEC_AV1:   return "av1";
	case AXIS_RELEASE_CODEC_VC1:   return "vc1";
	case AXIS_RELEASE_CODEC_MPEG2: return "mpeg2video";
	case AXIS_RELEASE_CODEC_XVID:  return "mpeg4";
	case AXIS_RELEASE_CODEC_VP9:   return "vp9";
	default:                       return NULL;
	}
}

/* The audio codecs as the media servers name them (DTS's kinds are profiles
 * of "dts" there, so they are one codec here). */
static const struct
{
	guint        flag;
	const gchar *codec;
} compat_audio[] = {
	{ AXIS_RELEASE_AUDIO_TRUEHD, "truehd" },
	{ AXIS_RELEASE_AUDIO_DTS_X | AXIS_RELEASE_AUDIO_DTS_HD_MA | AXIS_RELEASE_AUDIO_DTS_HD | AXIS_RELEASE_AUDIO_DTS,
	  "dts" },
	{ AXIS_RELEASE_AUDIO_EAC3, "eac3" },
	{ AXIS_RELEASE_AUDIO_AC3, "ac3" },
	{ AXIS_RELEASE_AUDIO_AAC, "aac" },
	{ AXIS_RELEASE_AUDIO_FLAC, "flac" },
	{ AXIS_RELEASE_AUDIO_OPUS, "opus" },
	{ AXIS_RELEASE_AUDIO_MP3, "mp3" },
	{ AXIS_RELEASE_AUDIO_PCM, "pcm" }
};

static const gchar *
compat_res_class(AxisReleaseResolution resolution)
{
	switch (resolution) {
	case AXIS_RELEASE_RESOLUTION_480P:
	case AXIS_RELEASE_RESOLUTION_576P:
		return "sd";
	case AXIS_RELEASE_RESOLUTION_720P:
		return "720p";
	case AXIS_RELEASE_RESOLUTION_1080P:
		return "1080p";
	case AXIS_RELEASE_RESOLUTION_2160P:
		return "2160p";
	default:
		return NULL;
	}
}

guint
axis_quality_release_combos(
	const AxisRelease *release,
	AxisCompatCombo   *combos,
	guint              max
){
	guint n = 0;
	guint i;

	if (release == NULL || combos == NULL)
		return 0;
	if (compat_codec(release->codec) != NULL && n < max) {
		AxisCompatCombo *c = &combos[n++];
		guint hdr = release->hdr & ~(guint)AXIS_RELEASE_HDR_SDR;

		memset(c, 0, sizeof(*c));
		c->kind = AXIS_COMPAT_STREAM_VIDEO;
		c->codec = compat_codec(release->codec);
		c->bit_depth = release->bit_depth;
		c->res_class = compat_res_class(release->resolution);
		/* Not stated is not SDR: a name that says nothing of HDR is not
		 * compared on it. */
		c->hdr = hdr != 0 ? 1 : (release->hdr & AXIS_RELEASE_HDR_SDR) != 0 ? 0 : -1;
	}
	for (i = 0; i < G_N_ELEMENTS(compat_audio) && n < max; i++) {
		AxisCompatCombo *c;

		if ((release->audio & compat_audio[i].flag) == 0)
			continue;
		c = &combos[n++];
		memset(c, 0, sizeof(*c));
		c->kind = AXIS_COMPAT_STREAM_AUDIO;
		c->codec = compat_audio[i].codec;
		c->hdr = -1;
	}
	return n;
}

AxisCompatVerdict
axis_quality_players_verdict(
	const AxisCompatCombo *want,
	gpointer               user_data
){
	const AxisQualityPlayers *players = user_data;
	AxisCompatVerdict worst = AXIS_VERDICT_SAFE;
	guint i;

	if (players == NULL || players->n_devices == 0 || want == NULL)
		return AXIS_VERDICT_UNKNOWN;
	for (i = 0; i < players->n_devices; i++) {
		const AxisQualityDevice *d = &players->devices[i];

		worst = axis_compat_verdict_worse(worst, axis_compat_device_verdict(want, d->obs, d->n_obs, NULL, NULL));
	}
	return worst;
}

/* A combination in words: "HEVC (x265) 10-bit 2160p HDR video". */
static gchar *
combo_words(
	const AxisRelease     *release,
	const AxisCompatCombo *combo
){
	GString *out = g_string_new(NULL);
	guint i;

	if (combo->kind == AXIS_COMPAT_STREAM_VIDEO) {
		g_string_append(out, axis_quality_value_label(AXIS_QUALITY_IF_CODEC, axis_release_codec_name(release->codec)));
		if (combo->bit_depth > 0)
			g_string_append_printf(out, " %d-bit", combo->bit_depth);
		if (axis_release_resolution_name(release->resolution) != NULL)
			g_string_append_printf(out, " %s", axis_release_resolution_name(release->resolution));
		if (combo->hdr == 1)
			g_string_append(out, " HDR");
		g_string_append(out, " video");
	} else {
		const gchar *label = combo->codec;

		for (i = 0; i < G_N_ELEMENTS(compat_audio); i++) {
			if (strcmp(compat_audio[i].codec, combo->codec) == 0) {
				const gchar *name = axis_release_audio_name(release->audio & compat_audio[i].flag);

				/* One flag of the set: its own label; several (DTS-HD MA
				 * and DTS): the codec's. */
				label = name != NULL ? axis_quality_value_label(AXIS_QUALITY_IF_AUDIO, name) :
					strcmp(combo->codec, "dts") == 0 ? "DTS" : combo->codec;
			}
		}
		g_string_append_printf(out, "%s audio", label);
	}
	return g_string_free(out, FALSE);
}

/* The players preference: what it takes off the score, with its line, or a
 * note when it cannot say. */
static gint
players_points(
	const AxisQualityContext *ctx,
	const AxisRelease        *release,
	AxisQualityVerdict       *v
){
	AxisCompatCombo combos[1 + G_N_ELEMENTS(compat_audio)];
	g_autoptr(GPtrArray) risky = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) unknown = g_ptr_array_new_with_free_func(g_free);
	guint n;
	guint i;

	if (ctx->profile->players <= 0)
		return 0;
	if (ctx->players == NULL) {
		if (v != NULL)
			note(v, "The players preference is on, but no compatibility verdicts were given to apply it with.");
		return 0;
	}
	n = axis_quality_release_combos(release, combos, G_N_ELEMENTS(combos));
	for (i = 0; i < n; i++) {
		AxisCompatVerdict verdict = ctx->players(&combos[i], ctx->players_data);

		if (verdict == AXIS_VERDICT_RISKY)
			g_ptr_array_add(risky, combo_words(release, &combos[i]));
		else if (verdict != AXIS_VERDICT_SAFE)
			g_ptr_array_add(unknown, combo_words(release, &combos[i]));
	}
	if (v != NULL && unknown->len > 0) {
		g_autofree gchar *what = axis_quality_join(unknown, " and ");

		note(v, "Whether the household's players can play its %s directly is not known from the compatibility "
			"report.", what);
	}
	if (v != NULL && n == 0)
		note(v, "The name states no codec, so the players preference has nothing to go on.");
	if (risky->len == 0)
		return 0;
	if (v != NULL) {
		g_autofree gchar *what = axis_quality_join(risky, " and ");

		score_line(v, -ctx->profile->players, g_strdup_printf("-%d because a player of the household is known to need "
			"its %s transcoded (the compatibility report)", ctx->profile->players, what));
	}
	return -ctx->profile->players;
}

/* ── The score ──────────────────────────────────────────────────── */

/* The preference score of @release, with its lines in @v (nullable). */
static gint
score_of(
	const AxisQualityContext *ctx,
	const AxisRelease        *release,
	const gchar              *name,
	AxisQualityVerdict       *v
){
	const AxisQualityProfile *p = ctx->profile;
	gint score = 0;
	guint i;

	if (release == NULL)
		return 0;
	for (i = 0; i < p->n_rules && i < AXIS_QUALITY_MAX_RULES; i++) {
		const AxisQualityRule *rule = &p->rules[i];
		g_autofree gchar *matched = NULL;

		if (rule->effect != AXIS_QUALITY_PREFER && rule->effect != AXIS_QUALITY_AVOID)
			continue;
		if (!axis_quality_rule_holds(rule, release, name))
			continue;
		if (rule->effect == AXIS_QUALITY_PREFER) {
			score += rule->weight;
			if (v != NULL) {
				matched = axis_quality_rule_matched_text(rule, release, name);
				score_line(v, rule->weight, g_strdup_printf("+%d for %s, which this profile prefers", rule->weight,
					matched != NULL ? matched : "?"));
			}
		} else {
			score -= rule->weight;
			if (v != NULL) {
				matched = axis_quality_rule_matched_text(rule, release, name);
				score_line(v, -rule->weight, g_strdup_printf("-%d for %s, which this profile avoids", rule->weight,
					matched != NULL ? matched : "?"));
			}
		}
	}
	return score + players_points(ctx, release, v);
}

/* ── The checks ─────────────────────────────────────────────────── */

static const gchar *
noun_of(const AxisQualityContext *ctx)
{
	return ctx->title != NULL && ctx->title->noun != NULL ? ctx->title->noun : "movie";
}

static gint
runtime_of(const AxisQualityContext *ctx)
{
	return ctx->title != NULL && ctx->title->runtime_minutes > 0 && ctx->title->runtime_minutes <= 1000000 ?
		ctx->title->runtime_minutes : 0;
}

static void
check_safety(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c,
	AxisQualityVerdict         *v
){
	const AxisRelease *r = c->release;

	if ((r->stop & AXIS_RELEASE_STOP_SAMPLE) != 0)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "sample", "it is a sample, not the whole %s", noun_of(ctx));
	if ((r->stop & AXIS_RELEASE_STOP_PASSWORD) != 0)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "password", "it is password-protected");
	if ((r->stop & AXIS_RELEASE_STOP_ARCHIVE) != 0)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "archive", "the name ends in an archive's extension (.rar, .zip and "
			"the like), which a release's name never does");
	if ((r->stop & AXIS_RELEASE_STOP_EXECUTABLE) != 0)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "executable", "the name ends in a program's extension (.exe and the "
			"like)");
	if ((r->stop & AXIS_RELEASE_STOP_SUBJECT) != 0)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "post_subject", "the name is a Usenet post's subject line for one part "
			"of a multi-part upload (a part counter and a file's name in quotes), not a release's name");
	if ((r->stop & AXIS_RELEASE_STOP_OBFUSCATED) != 0)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "no_title", "the name is nothing but a hash-like string: no title can "
			"be read from it");
	else if (r->title == NULL)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "no_title", "no title could be read from the name");
	if (ctx->no_title) {
		note(v, "Not matched against a title: on a search, a release that is not the title asked for, or that "
			"could be more than one title, is always rejected.");
		return;
	}
	if (c->match == AXIS_RELEASE_MATCH_AMBIGUOUS)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "ambiguous", "it could be more than one title%s%s%s (an ambiguous "
			"match is never taken)", c->match_reason != NULL ? " (" : "",
			c->match_reason != NULL ? c->match_reason : "", c->match_reason != NULL ? ")" : "");
	else if (c->match != AXIS_RELEASE_MATCH_YES)
		reason(v, AXIS_QUALITY_REASON_SAFETY, "no_match", "it is not the title asked for%s%s%s",
			c->match_reason != NULL ? " (" : "", c->match_reason != NULL ? c->match_reason : "",
			c->match_reason != NULL ? ")" : "");
}

/* What the parser did not find, for an unknown quality. */
static const gchar *
missing_of(const AxisRelease *r)
{
	gboolean source = r->source != AXIS_RELEASE_SOURCE_NONE;
	gboolean resolution = r->resolution != AXIS_RELEASE_RESOLUTION_NONE;

	if (!source && !resolution)
		return "neither a source nor a resolution was found in the name";
	if (!source)
		return "no source (WEB-DL, BluRay, ...) was found in the name";
	return "no resolution was found in the name";
}

static void
check_quality(
	const AxisQualityContext *ctx,
	const AxisRelease        *r,
	AxisQualityVerdict       *v
){
	const AxisQualityProfile *p = ctx->profile;
	AxisQuality q = v->quality;

	if (q == AXIS_QUALITY_UNKNOWN) {
		if (!p->allow_unknown)
			reason(v, AXIS_QUALITY_REASON_PROFILE, "unknown_quality", "its quality is unknown (%s), and this "
				"profile does not take releases of unknown quality", missing_of(r));
		return;
	}
	if (axis_quality_is_unwanted(q) && !p->allow_unwanted)
		reason(v, AXIS_QUALITY_REASON_PROFILE, "unwanted", "%s is a cinema recording or a pre-release copy, and "
			"this profile's switch for those is off", axis_quality_label(q));
	if (axis_quality_profile_tier(p, q) < 0)
		reason(v, AXIS_QUALITY_REASON_PROFILE, "not_allowed", "%s is not one of this profile's qualities",
			axis_quality_label(q));
}

static void
check_size(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c,
	AxisQualityVerdict         *v
){
	const AxisQualityProfile *p = ctx->profile;
	gint runtime = runtime_of(ctx);
	AxisQuality q = v->quality;

	if (c->size_bytes < 0) {
		note(v, "Its size is not known, so no size limit was checked.");
		return;
	}
	if (q != AXIS_QUALITY_UNKNOWN && runtime > 0) {
		const AxisQualitySize *s = &p->sizes[q];

		if (s->max > 0 && c->size_bytes > (gint64)s->max * runtime * MIB) {
			g_autofree gchar *size = NULL;
			g_autofree gchar *limit = NULL;

			format_pair(c->size_bytes, (gint64)s->max * runtime * MIB, &size, &limit);
			reason(v, AXIS_QUALITY_REASON_PROFILE, "too_large", "%s is above the %s limit for a %d-minute %s at %s",
				size, limit, runtime, noun_of(ctx), axis_quality_label(q));
		}
		if (s->min > 0 && c->size_bytes < (gint64)s->min * runtime * MIB) {
			g_autofree gchar *size = NULL;
			g_autofree gchar *limit = NULL;

			format_pair(c->size_bytes, (gint64)s->min * runtime * MIB, &size, &limit);
			reason(v, AXIS_QUALITY_REASON_PROFILE, "too_small", "%s is below the %s a %d-minute %s at %s should be "
				"at least", size, limit, runtime, noun_of(ctx), axis_quality_label(q));
		}
	} else if (q != AXIS_QUALITY_UNKNOWN) {
		note(v, "The runtime is not known, so only this profile's limit for any release was checked, not the sizes "
			"per minute.");
	}
	if (p->max_size_mb > 0 && c->size_bytes > p->max_size_mb * MIB) {
		g_autofree gchar *size = NULL;
		g_autofree gchar *limit = NULL;

		format_pair(c->size_bytes, p->max_size_mb * MIB, &size, &limit);
		reason(v, AXIS_QUALITY_REASON_PROFILE, "over_limit", "%s is above this profile's %s limit for any release",
			size, limit);
	}
}

/* What a rule's condition says when the release states nothing of it. */
static const gchar *
none_stated(AxisQualityCondition condition)
{
	switch (condition) {
	case AXIS_QUALITY_IF_CODEC:         return "no video codec stated";
	case AXIS_QUALITY_IF_BIT_DEPTH:     return "no bit depth stated";
	case AXIS_QUALITY_IF_HDR:           return "no HDR stated";
	case AXIS_QUALITY_IF_AUDIO:         return "no audio stated";
	case AXIS_QUALITY_IF_CHANNELS:      return "no channels stated";
	case AXIS_QUALITY_IF_LANGUAGE:      return "no language stated";
	case AXIS_QUALITY_IF_LANGUAGE_MARK: return "no language mark";
	case AXIS_QUALITY_IF_EDITION:       return "no edition stated";
	case AXIS_QUALITY_IF_SERVICE:       return "no streaming service stated";
	case AXIS_QUALITY_IF_GROUP:         return "no release group stated";
	case AXIS_QUALITY_IF_WORD:          return "not in the name";
	case AXIS_QUALITY_IF_3D:            return "not 3D";
	case AXIS_QUALITY_IF_HARDSUB:       return "no hard-coded subtitles";
	case AXIS_QUALITY_IF_REVISION:      return "the original release";
	default:                            return "nothing of it stated";
	}
}

static void
check_rules(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c,
	AxisQualityVerdict         *v
){
	const AxisQualityProfile *p = ctx->profile;
	guint i;

	for (i = 0; i < p->n_rules && i < AXIS_QUALITY_MAX_RULES; i++) {
		const AxisQualityRule *rule = &p->rules[i];
		gboolean holds;

		if (rule->effect != AXIS_QUALITY_REQUIRE && rule->effect != AXIS_QUALITY_REJECT)
			continue;
		holds = axis_quality_rule_holds(rule, c->release, c->name);
		if (rule->effect == AXIS_QUALITY_REQUIRE && !holds) {
			g_autofree gchar *want = axis_quality_rule_values_text(rule);

			if (rule->condition == AXIS_QUALITY_IF_WORD) {
				reason(v, AXIS_QUALITY_REASON_PROFILE, "rule_require", "%s is not in the name, where the profile "
					"requires it", want);
			} else {
				g_autofree gchar *has = axis_quality_rule_has_text(rule->condition, c->release);

				reason(v, AXIS_QUALITY_REASON_PROFILE, "rule_require", "%s where the profile requires %s",
					has != NULL ? has : none_stated(rule->condition), want);
			}
		} else if (rule->effect == AXIS_QUALITY_REJECT && holds) {
			g_autofree gchar *matched = axis_quality_rule_matched_text(rule, c->release, c->name);

			reason(v, AXIS_QUALITY_REASON_PROFILE, "rule_reject", "%s, which this profile rejects",
				matched != NULL ? matched : "?");
		}
	}
}

static void
check_language(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c,
	AxisQualityVerdict         *v
){
	const AxisRelease *r = c->release;
	const gchar *const *wanted = ctx->title != NULL ? ctx->title->languages : NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autofree gchar *want = NULL;
	g_autofree gchar *has = NULL;
	guint i;

	if (wanted == NULL || wanted[0] == NULL || r->language_count == 0)
		return;
	if ((r->language_marks & (AXIS_RELEASE_LANG_MULTI | AXIS_RELEASE_LANG_DUAL)) != 0)
		return;
	for (i = 0; i < r->language_count && i < AXIS_RELEASE_MAX_LANGUAGES; i++) {
		if (r->languages[i] != NULL && g_strv_contains(wanted, r->languages[i]))
			return;
	}
	names = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; wanted[i] != NULL && i < 16; i++)
		g_ptr_array_add(names, g_strdup(axis_quality_value_label(AXIS_QUALITY_IF_LANGUAGE, wanted[i])));
	want = axis_quality_join(names, " or ");
	has = axis_quality_rule_has_text(AXIS_QUALITY_IF_LANGUAGE, r);
	reason(v, AXIS_QUALITY_REASON_PROFILE, "language", "%s, where %s is wanted", has != NULL ? has : "?", want);
}

static gint
age_days(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c
){
	if (c->published_us <= 0 || ctx->now_us <= 0)
		return -1;
	if (ctx->now_us <= c->published_us)
		return 0;
	return (gint)MIN((ctx->now_us - c->published_us) / DAY_US, G_MAXINT32);
}

static void
check_availability(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c,
	AxisQualityVerdict         *v
){
	gint age = age_days(ctx, c);

	if (c->protocol == AXIS_DOWNLOAD_USENET && ctx->retention_days > 0 && age > ctx->retention_days)
		reason(v, AXIS_QUALITY_REASON_PROFILE, "retention", "it was posted %d days ago, beyond the %d days the Usenet "
			"provider keeps", age, ctx->retention_days);
}

/* ── The keys ───────────────────────────────────────────────────── */

static gint
seeders_band(gint seeders)
{
	if (seeders < 0)
		return -1;
	if (seeders == 0)
		return 0;
	if (seeders < 5)
		return 1;
	if (seeders < 20)
		return 2;
	if (seeders < 100)
		return 3;
	return 4;
}

static void
fill_keys(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c,
	guint                       index,
	AxisQualityVerdict         *v
){
	const AxisQualityProfile *p = ctx->profile;
	gint tier = axis_quality_profile_tier(p, v->quality);
	guint tiers = axis_quality_profile_n_tiers(p);

	v->keys.tier = tier >= 0 ? (guint)tier : v->quality == AXIS_QUALITY_UNKNOWN && p->allow_unknown ? tiers :
		tiers + 1;
	v->keys.score = v->score;
	v->keys.revision = axis_quality_revision_rank(c->release);
	v->keys.protocol = c->protocol;
	v->keys.preferred = (ctx->protocol == AXIS_QUALITY_PROTOCOL_USENET && c->protocol == AXIS_DOWNLOAD_USENET) ||
		(ctx->protocol == AXIS_QUALITY_PROTOCOL_TORRENT && c->protocol == AXIS_DOWNLOAD_TORRENT);
	v->keys.availability = c->protocol == AXIS_DOWNLOAD_TORRENT ? seeders_band(c->seeders) : age_days(ctx, c);
	v->keys.size = c->size_bytes >= 0 ? c->size_bytes : -1;
	v->keys.index = index;
}

gint
axis_quality_keys_compare(
	const AxisQualityKeys *a,
	const AxisQualityKeys *b,
	AxisQualityKey        *deciding
){
	AxisQualityKey key = AXIS_QUALITY_KEY_NONE;
	gint result = 0;

	if (a->tier != b->tier) {
		key = AXIS_QUALITY_KEY_TIER;
		result = a->tier < b->tier ? -1 : 1;
	} else if (a->score != b->score) {
		key = AXIS_QUALITY_KEY_SCORE;
		result = a->score > b->score ? -1 : 1;
	} else if (a->revision != b->revision) {
		key = AXIS_QUALITY_KEY_REVISION;
		result = a->revision > b->revision ? -1 : 1;
	} else if (a->preferred != b->preferred) {
		key = AXIS_QUALITY_KEY_PROTOCOL;
		result = a->preferred ? -1 : 1;
	} else if (a->protocol == b->protocol && a->availability != b->availability) {
		key = AXIS_QUALITY_KEY_AVAILABILITY;
		if (a->protocol == AXIS_DOWNLOAD_TORRENT)
			result = a->availability > b->availability ? -1 : 1;
		else if (a->availability < 0 || b->availability < 0)
			result = a->availability >= 0 ? -1 : 1;
		else
			result = a->availability < b->availability ? -1 : 1;
	} else if (a->size != b->size) {
		key = AXIS_QUALITY_KEY_SIZE;
		if (a->size < 0 || b->size < 0)
			result = a->size >= 0 ? -1 : 1;
		else
			result = a->size < b->size ? -1 : 1;
	} else if (a->index != b->index) {
		result = a->index < b->index ? -1 : 1;
	}
	if (deciding != NULL)
		*deciding = key;
	return result;
}

static const gchar *
band_words(gint band)
{
	switch (band) {
	case 0:  return "no seeders";
	case 1:  return "1 to 4 seeders";
	case 2:  return "5 to 19 seeders";
	case 3:  return "20 to 99 seeders";
	case 4:  return "100 seeders or more";
	default: return "seeders not known";
	}
}

/* The quality of a tier, in words: its qualities, or unknown. */
static gchar *
tier_words(
	const AxisQualityProfile *p,
	const AxisQualityVerdict *v
){
	if (v->quality == AXIS_QUALITY_UNKNOWN)
		return g_strdup("unknown quality");
	if (axis_quality_profile_tier(p, v->quality) < 0)
		return g_strdup_printf("%s, not one of this profile's qualities", axis_quality_label(v->quality));
	return g_strdup(axis_quality_label(v->quality));
}

gchar *
axis_quality_explain_rank(
	const AxisQualityProfile *profile,
	const AxisQualityVerdict *a,
	const AxisQualityVerdict *b
){
	AxisQualityKey key;

	g_return_val_if_fail(profile != NULL && a != NULL && b != NULL, g_strdup(""));

	axis_quality_keys_compare(&a->keys, &b->keys, &key);
	switch (key) {
	case AXIS_QUALITY_KEY_TIER: {
		g_autofree gchar *x = tier_words(profile, a);
		g_autofree gchar *y = tier_words(profile, b);

		return g_strdup_printf("higher in this profile's order: %s against %s", x, y);
	}
	case AXIS_QUALITY_KEY_SCORE:
		return g_strdup_printf("the same quality and a higher score: %d against %d", a->keys.score, b->keys.score);
	case AXIS_QUALITY_KEY_REVISION:
		return g_strdup(a->keys.revision > 0 && b->keys.revision == 0 ? "the same quality and score, and a proper "
			"or repack where the other is the original" : "the same quality and score, and a later fix");
	case AXIS_QUALITY_KEY_PROTOCOL:
		return g_strdup_printf("otherwise equal, and %s is the protocol preferred",
			axis_download_protocol_to_string(a->keys.protocol));
	case AXIS_QUALITY_KEY_AVAILABILITY:
		if (a->keys.protocol == AXIS_DOWNLOAD_TORRENT)
			return g_strdup_printf("otherwise equal, and more seeders: %s against %s", band_words(a->keys.availability),
				band_words(b->keys.availability));
		if (b->keys.availability < 0)
			return g_strdup_printf("otherwise equal, and its age is known (%d days)", a->keys.availability);
		return g_strdup_printf("otherwise equal, and younger: %d days against %d", a->keys.availability,
			b->keys.availability);
	case AXIS_QUALITY_KEY_SIZE: {
		g_autofree gchar *x = NULL;
		g_autofree gchar *y = NULL;

		if (b->keys.size < 0)
			return g_strdup("otherwise equal, and its size is known");
		format_pair(a->keys.size, b->keys.size, &x, &y);
		return g_strdup_printf("otherwise equal, and smaller: %s against %s", x, y);
	}
	default:
		return g_strdup("equal on every key: kept in the order found");
	}
}

/* ── The file on disk ───────────────────────────────────────────── */

typedef struct
{
	gboolean    present;
	AxisQuality quality;	/* what it counts as */
	gboolean    unrecorded;	/* converted, its source's quality not recorded */
	/* An unknown source at a known resolution (a file found in the library
	 * whose name states no source). */
	gboolean    unknown_source;
	AxisReleaseResolution resolution;
	gboolean    reached;
	gchar      *reached_text;
	gint        score;
	gint        revision;
	gchar      *text;
} Disk;

static void
disk_clear(Disk *disk)
{
	g_free(disk->reached_text);
	g_free(disk->text);
}

static void
disk_read(
	const AxisQualityContext *ctx,
	Disk                     *disk
){
	const AxisQualityProfile *p = ctx->profile;
	const AxisQualityOnDisk *d = ctx->disk;
	const gchar *label;

	memset(disk, 0, sizeof(*disk));
	if (d == NULL)
		return;
	disk->present = TRUE;
	if (d->converted) {
		disk->quality = d->source_quality;
		disk->unrecorded = d->source_quality == AXIS_QUALITY_UNKNOWN;
		disk->text = disk->unrecorded ? g_strdup("a file Axis converted, of a source whose quality was not recorded") :
			g_strdup_printf("%s (Axis converted it from a %s, and it counts as that)",
				axis_quality_label(d->source_quality), axis_quality_label(d->source_quality));
	} else if (d->quality == AXIS_QUALITY_UNKNOWN && d->resolution != AXIS_RELEASE_RESOLUTION_NONE &&
	           (guint)d->resolution <= AXIS_RELEASE_RESOLUTION_2160P) {
		disk->quality = AXIS_QUALITY_UNKNOWN;
		disk->unknown_source = TRUE;
		disk->resolution = d->resolution;
		disk->text = g_strdup_printf("%s (its name states no source)",
			axis_quality_unknown_source_label(d->resolution));
	} else {
		disk->quality = d->quality;
		disk->text = g_strdup(axis_quality_label(d->quality));
	}
	disk->score = score_of(ctx, d->fields, d->name, NULL);
	disk->revision = axis_quality_revision_rank(d->fields);
	label = axis_quality_label(disk->quality);

	if (!p->upgrades) {
		disk->reached = TRUE;
		disk->reached_text = g_strdup("a file is on disk, and this profile does not upgrade");
	} else if (disk->unknown_source && !p->upgrade_unknown) {
		disk->reached = TRUE;
		disk->reached_text = g_strdup_printf("the file on disk is %s (its name states no source), and this profile "
			"does not upgrade a file of unknown source", axis_quality_unknown_source_label(disk->resolution));
	} else if (disk->unknown_source) {
		disk->reached = !axis_quality_above_unknown(p->cutoff, disk->resolution);
		if (disk->reached)
			disk->reached_text = g_strdup_printf("the file on disk, %s, is above this profile's cutoff (%s): no "
				"upgrade is sought", axis_quality_unknown_source_label(disk->resolution),
				axis_quality_label(p->cutoff));
	} else if (disk->unrecorded) {
		disk->reached = TRUE;
		disk->reached_text = g_strdup("Axis converted the file on disk and what it was made from was not "
			"recorded, so nothing is taken to be better than it");
	} else if (disk->quality != AXIS_QUALITY_UNKNOWN) {
		gint tier = axis_quality_profile_tier(p, disk->quality);

		if (tier >= 0)
			disk->reached = tier <= axis_quality_profile_tier(p, p->cutoff);
		else
			disk->reached = axis_quality_compare(disk->quality, p->cutoff) >= 0;
		if (disk->reached)
			disk->reached_text = tier >= 0 ? g_strdup_printf("the file on disk, %s, is at or above this profile's "
				"cutoff (%s): no upgrade is sought", label, axis_quality_label(p->cutoff)) :
				g_strdup_printf("the file on disk, %s, is not one of this profile's qualities and is at or above its "
				"cutoff (%s) in the standard order of qualities: no upgrade is sought", label, axis_quality_label(p->cutoff));
	}
}

static const gchar *
revision_words(gint rank)
{
	return rank == 0 ? "the original" : rank == 1 ? "a proper or repack" : "a later fix";
}

/* Whether the release is an upgrade; the reason when it is not. */
static void
check_upgrade(
	const AxisQualityContext *ctx,
	const Disk               *disk,
	AxisQualityVerdict       *v
){
	const AxisQualityProfile *p = ctx->profile;
	const gchar *mine = axis_quality_label(v->quality);
	const gchar *theirs = axis_quality_label(disk->quality);
	gint rt;
	gint dt;
	gint order;

	if (disk->reached) {
		reason(v, AXIS_QUALITY_REASON_UPGRADE, p->upgrades ? "cutoff" : "upgrades_off", "%s", disk->reached_text);
		return;
	}
	if (disk->unknown_source)
		theirs = axis_quality_unknown_source_label(disk->resolution);
	if (v->quality == AXIS_QUALITY_UNKNOWN) {
		reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "its quality is unknown, so it is not known to be better "
			"than the file on disk (%s)", theirs);
		return;
	}
	if (disk->unknown_source) {
		/* This profile upgrades an unknown source (else it was reached). */
		if (axis_quality_above_unknown(v->quality, disk->resolution)) {
			v->upgrade = TRUE;
			v->upgrade_text = g_strdup_printf("%s states its source, where the file on disk is %s (this profile "
				"upgrades a file of unknown source)", mine, theirs);
		} else {
			reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "not better than the file on disk: %s is below %s",
				mine, theirs);
		}
		return;
	}
	if (disk->quality == AXIS_QUALITY_UNKNOWN) {
		v->upgrade = TRUE;
		v->upgrade_text = g_strdup("the quality of the file on disk is not known");
		return;
	}
	rt = axis_quality_profile_tier(p, v->quality);
	dt = axis_quality_profile_tier(p, disk->quality);
	if (rt >= 0 && dt >= 0)
		order = rt < dt ? 1 : rt > dt ? -1 : 0;
	else
		order = axis_quality_compare(v->quality, disk->quality);
	if (order > 0) {
		v->upgrade = TRUE;
		v->upgrade_text = dt >= 0 ? g_strdup_printf("%s is above the %s on disk in this profile's order", mine, theirs) :
			g_strdup_printf("%s is above the %s on disk in the standard order of qualities (this profile does not list that one)", mine,
			theirs);
		return;
	}
	if (order < 0) {
		reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "not better than the file on disk: %s is below its %s%s",
			mine, theirs, dt >= 0 && rt >= 0 ? " in this profile's order" : " in the standard order of qualities");
		return;
	}
	/* The same quality (or equals). */
	if (p->upgrade_revisions && v->keys.revision > disk->revision) {
		v->upgrade = TRUE;
		v->upgrade_text = g_strdup_printf("%s of the same quality, where the file on disk is %s",
			revision_words(v->keys.revision), revision_words(disk->revision));
		return;
	}
	if (p->upgrade_score && v->score >= disk->score + p->min_score_gain) {
		v->upgrade = TRUE;
		v->upgrade_text = g_strdup_printf("the same quality with a score of %d against the file on disk's %d (this "
			"profile upgrades for %d more)", v->score, disk->score, p->min_score_gain);
		return;
	}
	if (v->score < disk->score)
		reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "not better than the file on disk (the same quality, a "
			"lower score: %d against %d)", v->score, disk->score);
	else if (v->score == disk->score)
		reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "not better than the file on disk (the same quality and "
			"the same score, %d)", v->score);
	else if (p->upgrade_score)
		reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "not better than the file on disk by enough (the same "
			"quality, a score of %d against %d, where this profile upgrades for %d more)", v->score, disk->score,
			p->min_score_gain);
	else
		reason(v, AXIS_QUALITY_REASON_UPGRADE, "not_better", "not better than the file on disk (the same quality; a "
			"score of %d against %d, and this profile does not upgrade for a better score)", v->score, disk->score);
}

/* ── The decision ───────────────────────────────────────────────── */

static AxisQualityVerdict *
evaluate(
	const AxisQualityContext   *ctx,
	gboolean                    valid,
	const Disk                 *disk,
	const AxisQualityCandidate *c,
	guint                       index
){
	AxisQualityVerdict *v = verdict_new();
	AxisRelease empty;
	AxisQualityCandidate local;
	guint i;

	if (c->release == NULL) {
		/* Nothing was read: a release with no title, of unknown quality. */
		memset(&empty, 0, sizeof(empty));
		empty.season_first = -1;
		empty.season_last = -1;
		local = *c;
		local.release = &empty;
		c = &local;
	}
	v->quality = axis_quality_of_release(c->release);
	check_safety(ctx, c, v);
	if (valid) {
		check_quality(ctx, c->release, v);
		check_size(ctx, c, v);
		check_rules(ctx, c, v);
		check_language(ctx, c, v);
		check_availability(ctx, c, v);
		v->score = score_of(ctx, c->release, c->name, v);
	} else {
		/* Fail closed: an invalid profile accepts nothing. */
		reason(v, AXIS_QUALITY_REASON_PROFILE, "profile_invalid", "the profile is not valid, so nothing is accepted "
			"with it");
	}
	fill_keys(ctx, c, index, v);

	v->acceptable = v->reasons->len == 0;
	for (i = 0; i < v->reasons->len; i++) {
		const AxisQualityReason *r = g_ptr_array_index(v->reasons, i);

		if (r->kind == AXIS_QUALITY_REASON_SAFETY)
			v->safety = TRUE;
	}
	if (valid && disk->present)
		check_upgrade(ctx, disk, v);
	v->accepted = v->acceptable && (!disk->present || v->upgrade);
	return v;
}

/* The accepted best first; stable, since the keys end with the input
 * position. */
static void
sort_accepted(
	GPtrArray *verdicts,
	guint     *order,
	guint      n
){
	guint i;
	guint j;

	/* An insertion sort: no global state, and the lists are short. */
	for (i = 1; i < n; i++) {
		guint item = order[i];
		const AxisQualityVerdict *v = g_ptr_array_index(verdicts, item);

		for (j = i; j > 0; j--) {
			const AxisQualityVerdict *w = g_ptr_array_index(verdicts, order[j - 1]);

			if (axis_quality_keys_compare(&w->keys, &v->keys, NULL) <= 0)
				break;
			order[j] = order[j - 1];
		}
		order[j] = item;
	}
}

AxisQualityDecision *
axis_quality_decide(
	const AxisQualityContext   *context,
	const AxisQualityCandidate *candidates,
	guint                       n
){
	AxisQualityDecision *decision = g_new0(AxisQualityDecision, 1);
	gboolean valid;
	Disk disk;
	guint accepted = 0;
	guint at = 0;
	guint i;

	g_return_val_if_fail(context != NULL && context->profile != NULL, decision);

	valid = axis_quality_profile_check(context->profile, NULL);
	disk_read(context, &disk);
	decision->verdicts = g_ptr_array_new_full(n, (GDestroyNotify)axis_quality_verdict_free);
	decision->order = g_new0(guint, n > 0 ? n : 1);
	for (i = 0; i < n; i++) {
		AxisQualityVerdict *v = evaluate(context, valid, &disk, &candidates[i], i);

		g_ptr_array_add(decision->verdicts, v);
		if (v->accepted)
			decision->order[accepted++] = i;
	}
	sort_accepted(decision->verdicts, decision->order, accepted);
	for (i = 0; i < accepted; i++)
		((AxisQualityVerdict *)g_ptr_array_index(decision->verdicts, decision->order[i]))->rank = i + 1;
	at = accepted;
	for (i = 0; i < n; i++) {
		if (!((AxisQualityVerdict *)g_ptr_array_index(decision->verdicts, i))->accepted)
			decision->order[at++] = i;
	}
	decision->n_accepted = accepted;
	decision->cutoff_reached = disk.present && disk.reached;
	decision->disk_text = g_steal_pointer(&disk.text);
	disk_clear(&disk);
	return decision;
}

AxisQualityVerdict *
axis_quality_decide_one(
	const AxisQualityContext   *context,
	const AxisQualityCandidate *candidate
){
	g_autoptr(AxisQualityDecision) decision = axis_quality_decide(context, candidate, 1);

	g_return_val_if_fail(decision->verdicts != NULL && decision->verdicts->len == 1, NULL);
	return g_ptr_array_steal_index(decision->verdicts, 0);
}
