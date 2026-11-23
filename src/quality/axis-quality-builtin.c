/*
 * axis-quality-builtin.c - the built-in profiles, slugs and names
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The built-ins are read-only (copy one to change it) and good enough that
 * most users never edit one.  Axis is built to save space, and
 * originals are not kept after conversion, so the defaults lean small: only
 * Best 1080p takes a remux, only 4K takes 2160p, and nothing but Anything
 * takes a disc image or a release of unknown quality.  Every one rejects
 * 3D and avoids hard-coded subtitles.
 */

#include "quality/axis-quality-private.h"

#include <string.h>

static const AxisQualityBuiltin builtins[AXIS_QUALITY_N_BUILTINS] = {
	{ "space_saver", "Small 1080p",
	  "1080p from streaming services only, HEVC preferred, at most about 1.5 GiB an hour of video; not upgraded "
	  "once one is there." },
	{ "balanced", "Balanced 1080p",
	  "1080p from TV, streaming services or a BluRay encode; upgraded until a BluRay encode is there. The default "
	  "for a new title." },
	{ "best_1080p", "Best 1080p",
	  "1080p up to a remux of the disc; upgraded until a remux is there. Large: a remux is 20 to 40 GiB." },
	{ "uhd", "4K",
	  "2160p from streaming services or a BluRay encode, HDR preferred and Dolby Vision without an HDR10 fallback "
	  "avoided; upgraded until a 2160p WEB-DL is there." },
	{ "anything", "Anything",
	  "Every quality but cinema recordings and pre-release copies, best first, then releases of unknown quality "
	  "and disc images; upgraded until a 1080p WEB-DL is there. For what is hard to find." }
};

const AxisQualityBuiltin *
axis_quality_builtins(void)
{
	return builtins;
}

const AxisQualityBuiltin *
axis_quality_builtin(const gchar *slug)
{
	guint i;

	for (i = 0; slug != NULL && i < AXIS_QUALITY_N_BUILTINS; i++) {
		if (strcmp(builtins[i].slug, slug) == 0)
			return &builtins[i];
	}
	return NULL;
}

gboolean
axis_quality_slug_reserved(const gchar *slug)
{
	return axis_quality_builtin(slug) != NULL;
}

gboolean
axis_quality_slug_valid(const gchar *slug)
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

/* ── Building them ──────────────────────────────────────────────── */

/* Appends @quality: in a tier of its own, or with the one before. */
static void
allow(
	AxisQualityProfile *p,
	AxisQuality         quality,
	gboolean            equal_to_previous
){
	guint n = p->n_allowed;

	p->allowed[n] = quality;
	p->tier[n] = n == 0 ? 0 : equal_to_previous ? p->tier[n - 1] : p->tier[n - 1] + 1;
	p->n_allowed++;
}

static void
rule(
	AxisQualityProfile   *p,
	AxisQualityCondition  condition,
	AxisQualityEffect     effect,
	gint                  weight,
	const gchar          *value
){
	AxisQualityRule *r = &p->rules[p->n_rules++];

	memset(r, 0, sizeof(*r));
	r->condition = condition;
	r->effect = effect;
	r->weight = weight;
	if (value != NULL) {
		g_strlcpy(r->values[0], value, sizeof(r->values[0]));
		r->n_values = 1;
	}
}

/* What every built-in has. */
static void
common_rules(AxisQualityProfile *p)
{
	rule(p, AXIS_QUALITY_IF_3D, AXIS_QUALITY_REJECT, 0, NULL);
	rule(p, AXIS_QUALITY_IF_HARDSUB, AXIS_QUALITY_AVOID, 100, NULL);
}

static void
build_space_saver(AxisQualityProfile *p)
{
	allow(p, AXIS_QUALITY_WEBDL_1080P, FALSE);
	allow(p, AXIS_QUALITY_WEBRIP_1080P, TRUE);
	p->cutoff = AXIS_QUALITY_WEBDL_1080P;
	/* The limits are in mebibytes.  25 MiB a minute is 2.4 GiB for a
	 * 97-minute film: an HEVC encode, or a lean H.264 one. */
	p->sizes[AXIS_QUALITY_WEBDL_1080P].min = 3;
	p->sizes[AXIS_QUALITY_WEBDL_1080P].max = 25;
	p->sizes[AXIS_QUALITY_WEBRIP_1080P].min = 3;
	p->sizes[AXIS_QUALITY_WEBRIP_1080P].max = 25;
	p->max_size_mb = 6144;	/* 6 GiB */
	rule(p, AXIS_QUALITY_IF_CODEC, AXIS_QUALITY_PREFER, 100, "h265");
	common_rules(p);
}

static void
build_balanced(AxisQualityProfile *p)
{
	allow(p, AXIS_QUALITY_BLURAY_1080P, FALSE);
	allow(p, AXIS_QUALITY_WEBDL_1080P, FALSE);
	allow(p, AXIS_QUALITY_WEBRIP_1080P, TRUE);
	allow(p, AXIS_QUALITY_HDTV_1080P, FALSE);
	p->cutoff = AXIS_QUALITY_BLURAY_1080P;
	p->max_size_mb = 24576;	/* 24 GiB */
	rule(p, AXIS_QUALITY_IF_CODEC, AXIS_QUALITY_PREFER, 25, "h265");
	common_rules(p);
}

static void
build_best_1080p(AxisQualityProfile *p)
{
	allow(p, AXIS_QUALITY_REMUX_1080P, FALSE);
	allow(p, AXIS_QUALITY_BLURAY_1080P, FALSE);
	allow(p, AXIS_QUALITY_WEBDL_1080P, FALSE);
	allow(p, AXIS_QUALITY_WEBRIP_1080P, TRUE);
	allow(p, AXIS_QUALITY_HDTV_1080P, FALSE);
	p->cutoff = AXIS_QUALITY_REMUX_1080P;
	p->max_size_mb = 61440;	/* 60 GiB */
	common_rules(p);
}

static void
build_uhd(AxisQualityProfile *p)
{
	allow(p, AXIS_QUALITY_BLURAY_2160P, FALSE);
	allow(p, AXIS_QUALITY_WEBDL_2160P, FALSE);
	allow(p, AXIS_QUALITY_WEBRIP_2160P, TRUE);
	p->cutoff = AXIS_QUALITY_WEBDL_2160P;
	p->max_size_mb = 81920;	/* 80 GiB */
	rule(p, AXIS_QUALITY_IF_HDR, AXIS_QUALITY_PREFER, 50, "any");
	rule(p, AXIS_QUALITY_IF_HDR, AXIS_QUALITY_AVOID, 100, "dv_only");
	common_rules(p);
}

static void
build_anything(AxisQualityProfile *p)
{
	static const AxisQuality order[] = {
		AXIS_QUALITY_REMUX_2160P, AXIS_QUALITY_BLURAY_2160P, AXIS_QUALITY_WEBDL_2160P, AXIS_QUALITY_WEBRIP_2160P,
		AXIS_QUALITY_HDTV_2160P, AXIS_QUALITY_REMUX_1080P, AXIS_QUALITY_BLURAY_1080P, AXIS_QUALITY_WEBDL_1080P,
		AXIS_QUALITY_WEBRIP_1080P, AXIS_QUALITY_HDTV_1080P, AXIS_QUALITY_BLURAY_720P, AXIS_QUALITY_WEBDL_720P,
		AXIS_QUALITY_WEBRIP_720P, AXIS_QUALITY_HDTV_720P, AXIS_QUALITY_BLURAY_480P, AXIS_QUALITY_WEB_480P,
		AXIS_QUALITY_SDTV, AXIS_QUALITY_DVD,
		/* Last: a media server does not play a disc image directly. */
		AXIS_QUALITY_DISC
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(order); i++)
		allow(p, order[i], FALSE);
	p->cutoff = AXIS_QUALITY_WEBDL_1080P;
	p->allow_unknown = TRUE;
	p->max_size_mb = 102400;	/* 100 GiB */
	common_rules(p);
}

gboolean
axis_quality_builtin_profile(
	const gchar        *slug,
	AxisQualityProfile *profile
){
	static const struct
	{
		const gchar *slug;
		void       (*build)(AxisQualityProfile *p);
	} table[] = {
		{ "space_saver", build_space_saver },
		{ "balanced", build_balanced },
		{ "best_1080p", build_best_1080p },
		{ "uhd", build_uhd },
		{ "anything", build_anything }
	};
	AxisQualityProfile built;
	g_autofree gchar *canonical = NULL;
	guint i;

	for (i = 0; slug != NULL && i < G_N_ELEMENTS(table); i++) {
		if (strcmp(table[i].slug, slug) != 0)
			continue;
		axis_quality_profile_init(&built);
		table[i].build(&built);
		/* Through the strict reader, as a stored one would be. */
		canonical = axis_quality_profile_canonical(&built);
		return axis_quality_profile_parse(canonical, -1, profile, NULL);
	}
	return FALSE;
}

/* ── Names ──────────────────────────────────────────────────────── */

/* Characters a name never keeps: they change how text around them is
 * shown (direction overrides and isolates, zero-width joiners). */
static gboolean
invisible(gunichar c)
{
	return (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069) ||
		c == 0xfeff || c == 0x2028 || c == 0x2029;
}

gchar *
axis_quality_clean_name(const gchar *text)
{
	GString *out;
	const gchar *p;
	guint chars = 0;
	gboolean space = FALSE;

	if (text == NULL || !g_utf8_validate(text, -1, NULL))
		return NULL;
	out = g_string_new(NULL);
	for (p = text; *p != '\0' && chars < AXIS_QUALITY_NAME_MAX_CHARS; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (invisible(c))
			continue;
		if (g_unichar_iscntrl(c) || g_unichar_isspace(c)) {
			space = TRUE;
			continue;
		}
		if (space && out->len > 0) {
			g_string_append_c(out, ' ');
			chars++;
			if (chars >= AXIS_QUALITY_NAME_MAX_CHARS)
				break;
		}
		space = FALSE;
		g_string_append_unichar(out, c);
		chars++;
	}
	while (out->len > 0 && out->str[out->len - 1] == ' ')
		g_string_truncate(out, out->len - 1);
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}
