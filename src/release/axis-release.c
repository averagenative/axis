/*
 * axis-release.c - what a release name was read into: its structure, the
 * names of its values, and a one-line description
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "release/axis-release.h"

#include <string.h>

void
axis_release_free(AxisRelease *release)
{
	if (release == NULL)
		return;
	g_free(release->title);
	g_free(release->group);
	g_free(release->checksum);
	if (release->unknown != NULL)
		g_ptr_array_unref(release->unknown);
	g_free(release);
}

gboolean
axis_release_is_episode(const AxisRelease *release)
{
	g_return_val_if_fail(release != NULL, FALSE);
	return release->season_first >= 0 || release->episode_count > 0 || release->absolute_count > 0 ||
		release->air_year > 0 || release->complete;
}

/* ── Names ──────────────────────────────────────────────────────── */

const gchar *
axis_release_resolution_name(AxisReleaseResolution resolution)
{
	switch (resolution) {
	case AXIS_RELEASE_RESOLUTION_480P:  return "480p";
	case AXIS_RELEASE_RESOLUTION_576P:  return "576p";
	case AXIS_RELEASE_RESOLUTION_720P:  return "720p";
	case AXIS_RELEASE_RESOLUTION_1080P: return "1080p";
	case AXIS_RELEASE_RESOLUTION_2160P: return "2160p";
	default:                            return NULL;
	}
}

const gchar *
axis_release_source_name(AxisReleaseSource source)
{
	switch (source) {
	case AXIS_RELEASE_SOURCE_CAM:       return "cam";
	case AXIS_RELEASE_SOURCE_TELESYNC:  return "telesync";
	case AXIS_RELEASE_SOURCE_TELECINE:  return "telecine";
	case AXIS_RELEASE_SOURCE_SCREENER:  return "screener";
	case AXIS_RELEASE_SOURCE_WORKPRINT: return "workprint";
	case AXIS_RELEASE_SOURCE_DVD:       return "dvd";
	case AXIS_RELEASE_SOURCE_HDTV:      return "hdtv";
	case AXIS_RELEASE_SOURCE_WEBRIP:    return "webrip";
	case AXIS_RELEASE_SOURCE_WEBDL:     return "webdl";
	case AXIS_RELEASE_SOURCE_BLURAY:    return "bluray";
	case AXIS_RELEASE_SOURCE_REMUX:     return "remux";
	case AXIS_RELEASE_SOURCE_DISC:      return "disc";
	default:                            return NULL;
	}
}

const gchar *
axis_release_codec_name(AxisReleaseCodec codec)
{
	switch (codec) {
	case AXIS_RELEASE_CODEC_H264:  return "h264";
	case AXIS_RELEASE_CODEC_H265:  return "h265";
	case AXIS_RELEASE_CODEC_AV1:   return "av1";
	case AXIS_RELEASE_CODEC_VC1:   return "vc1";
	case AXIS_RELEASE_CODEC_MPEG2: return "mpeg2";
	case AXIS_RELEASE_CODEC_XVID:  return "xvid";
	case AXIS_RELEASE_CODEC_VP9:   return "vp9";
	default:                       return NULL;
	}
}

/* The name of @flag in @names, where names[i] is the name of 1 << i. */
static const gchar *
flag_name(
	const gchar *const *names,
	guint               count,
	guint               flag
){
	guint i;

	for (i = 0; i < count; i++) {
		if (flag == (1u << i))
			return names[i];
	}
	return NULL;
}

static const gchar *const hdr_names[] = {
	"hdr", "hdr10", "hdr10plus", "dv", "hlg", "sdr"
};

static const gchar *const audio_names[] = {
	"truehd", "dtsx", "dtshdma", "dtshd", "dts", "eac3", "ac3", "aac", "flac", "opus", "mp3", "pcm"
};

static const gchar *const edition_names[] = {
	"extended", "directors", "theatrical", "unrated", "uncut", "remastered", "restored", "imax",
	"criterion", "finalcut", "openmatte", "special", "collectors", "anniversary", "ultimate",
	"definitive", "alternate", "other"
};

static const gchar *const lang_mark_names[] = {
	"multi", "dual", "dubbed", "subbed", "hardsub"
};

static const gchar *const revision_names[] = {
	"proper", "repack", "rerip", "real"
};

static const gchar *const mark_names[] = {
	"limited", "internal", "fix", "readnfo", "hybrid"
};

static const gchar *const stop_names[] = {
	"sample", "password", "3d", "obfuscated", "archive", "executable", "subject"
};

const gchar *
axis_release_hdr_name(guint flag)
{
	return flag_name(hdr_names, G_N_ELEMENTS(hdr_names), flag);
}

const gchar *
axis_release_audio_name(guint flag)
{
	return flag_name(audio_names, G_N_ELEMENTS(audio_names), flag);
}

const gchar *
axis_release_edition_name(guint flag)
{
	return flag_name(edition_names, G_N_ELEMENTS(edition_names), flag);
}

const gchar *
axis_release_lang_mark_name(guint flag)
{
	return flag_name(lang_mark_names, G_N_ELEMENTS(lang_mark_names), flag);
}

const gchar *
axis_release_revision_name(guint flag)
{
	return flag_name(revision_names, G_N_ELEMENTS(revision_names), flag);
}

const gchar *
axis_release_mark_name(guint flag)
{
	return flag_name(mark_names, G_N_ELEMENTS(mark_names), flag);
}

const gchar *
axis_release_stop_name(guint flag)
{
	return flag_name(stop_names, G_N_ELEMENTS(stop_names), flag);
}

const gchar *
axis_release_match_name(AxisReleaseMatch match)
{
	switch (match) {
	case AXIS_RELEASE_MATCH_YES:       return "match";
	case AXIS_RELEASE_MATCH_AMBIGUOUS: return "ambiguous";
	default:                           return "no match";
	}
}

/* ── Description ────────────────────────────────────────────────── */

static void
append_flags(
	GString            *out,
	const gchar        *label,
	const gchar *const *names,
	guint               count,
	guint               flags
){
	gboolean first = TRUE;
	guint i;

	if (flags == 0)
		return;
	g_string_append_printf(out, " %s=", label);
	for (i = 0; i < count; i++) {
		if (flags & (1u << i)) {
			if (!first)
				g_string_append_c(out, ',');
			g_string_append(out, names[i]);
			first = FALSE;
		}
	}
}

static void
append_numbers(
	GString     *out,
	const gchar *label,
	const gint  *numbers,
	guint        count
){
	guint i;

	if (count == 0)
		return;
	g_string_append_printf(out, " %s=", label);
	/* A run written out is shown as a range. */
	for (i = 0; i < count;) {
		guint j = i;

		while (j + 1 < count && numbers[j + 1] == numbers[j] + 1)
			j++;
		if (i > 0)
			g_string_append_c(out, ',');
		if (j > i + 1)
			g_string_append_printf(out, "%d-%d", numbers[i], numbers[j]);
		else if (j == i + 1)
			g_string_append_printf(out, "%d,%d", numbers[i], numbers[j]);
		else
			g_string_append_printf(out, "%d", numbers[i]);
		i = j + 1;
	}
}

gchar *
axis_release_describe(const AxisRelease *release)
{
	GString *out = g_string_new(NULL);
	guint i;

	g_return_val_if_fail(release != NULL, g_strdup(""));

	if (release->title != NULL)
		g_string_append_printf(out, "title='%s'", release->title);
	else
		g_string_append(out, "title=none");
	if (release->year > 0)
		g_string_append_printf(out, " year=%d", release->year);
	if (release->season_first >= 0) {
		if (release->season_last != release->season_first)
			g_string_append_printf(out, " seasons=%d-%d", release->season_first, release->season_last);
		else
			g_string_append_printf(out, " season=%d", release->season_first);
	}
	append_numbers(out, "episodes", release->episodes, release->episode_count);
	append_numbers(out, "absolute", release->absolute, release->absolute_count);
	if (release->air_year > 0)
		g_string_append_printf(out, " date=%04d-%02d-%02d", release->air_year, release->air_month,
			release->air_day);
	if (release->complete)
		g_string_append(out, " complete");
	if (release->version > 0)
		g_string_append_printf(out, " version=%d", release->version);
	if (release->resolution != AXIS_RELEASE_RESOLUTION_NONE)
		g_string_append_printf(out, " resolution=%s%s", axis_release_resolution_name(release->resolution),
			release->interlaced ? "(interlaced)" : "");
	if (release->source != AXIS_RELEASE_SOURCE_NONE)
		g_string_append_printf(out, " source=%s", axis_release_source_name(release->source));
	if (release->service != NULL)
		g_string_append_printf(out, " service=%s", release->service);
	if (release->codec != AXIS_RELEASE_CODEC_NONE)
		g_string_append_printf(out, " codec=%s", axis_release_codec_name(release->codec));
	if (release->bit_depth > 0)
		g_string_append_printf(out, " bits=%d", release->bit_depth);
	append_flags(out, "hdr", hdr_names, G_N_ELEMENTS(hdr_names), release->hdr);
	append_flags(out, "audio", audio_names, G_N_ELEMENTS(audio_names), release->audio);
	if (release->atmos)
		g_string_append(out, " atmos");
	if (release->channels[0] != '\0')
		g_string_append_printf(out, " channels=%s", release->channels);
	append_flags(out, "editions", edition_names, G_N_ELEMENTS(edition_names), release->editions);
	if (release->language_count > 0) {
		g_string_append(out, " languages=");
		for (i = 0; i < release->language_count; i++)
			g_string_append_printf(out, "%s%s", i > 0 ? "," : "", release->languages[i]);
	}
	append_flags(out, "lang", lang_mark_names, G_N_ELEMENTS(lang_mark_names), release->language_marks);
	append_flags(out, "revision", revision_names, G_N_ELEMENTS(revision_names), release->revision);
	append_flags(out, "marks", mark_names, G_N_ELEMENTS(mark_names), release->marks);
	append_flags(out, "stop", stop_names, G_N_ELEMENTS(stop_names), release->stop);
	if (release->group != NULL)
		g_string_append_printf(out, " group='%s'", release->group);
	if (release->checksum != NULL)
		g_string_append_printf(out, " checksum=%s", release->checksum);
	if (release->unknown != NULL && release->unknown->len > 0) {
		g_string_append(out, " unknown=[");
		for (i = 0; i < release->unknown->len; i++)
			g_string_append_printf(out, "%s'%s'", i > 0 ? " " : "",
				(const gchar *)g_ptr_array_index(release->unknown, i));
		g_string_append_c(out, ']');
	}
	return g_string_free(out, FALSE);
}
