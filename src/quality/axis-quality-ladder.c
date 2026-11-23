/*
 * axis-quality-ladder.c - the ladder of qualities: placing a release on it,
 * its names and order
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "quality/axis-quality.h"

#include <string.h>

typedef struct
{
	AxisQuality       quality;
	const gchar      *name;
	const gchar      *label;
	AxisQualityClass  klass;
	AxisQualitySize   size;	/* the default limits, MiB per minute */
} Rung;

/*
 * The ladder, worst first, with the default size limits.  The limits are
 * wide: they are there to refuse a fake (a 300 MiB "remux") or an oddity (a
 * 60 GiB "WEB-DL"), not to choose between good releases; a profile that
 * wants small files sets its own (Space saver does).  The limits are in
 * mebibytes per minute of runtime; one is about 0.14 Mbit/s, so 25 MiB per
 * minute is a 3.5 Mbit/s encode and 400 a 56 Mbit/s remux.
 */
static const Rung rungs[AXIS_QUALITY_N_LADDER + 1] = {
	{ AXIS_QUALITY_UNKNOWN,      "unknown",      "Unknown",          AXIS_QUALITY_CLASS_UNKNOWN,  { 0, 0 } },
	{ AXIS_QUALITY_WORKPRINT,    "workprint",    "Workprint",        AXIS_QUALITY_CLASS_UNWANTED, { 1, 60 } },
	{ AXIS_QUALITY_CAM,          "cam",          "Cam",              AXIS_QUALITY_CLASS_UNWANTED, { 1, 60 } },
	{ AXIS_QUALITY_TELESYNC,     "telesync",     "Telesync",         AXIS_QUALITY_CLASS_UNWANTED, { 1, 60 } },
	{ AXIS_QUALITY_TELECINE,     "telecine",     "Telecine",         AXIS_QUALITY_CLASS_UNWANTED, { 1, 60 } },
	{ AXIS_QUALITY_SCREENER,     "screener",     "Screener",         AXIS_QUALITY_CLASS_UNWANTED, { 1, 60 } },
	{ AXIS_QUALITY_DVD,          "dvd",          "DVD",              AXIS_QUALITY_CLASS_SD,       { 2, 70 } },
	{ AXIS_QUALITY_SDTV,         "sdtv",         "SDTV",             AXIS_QUALITY_CLASS_SD,       { 1, 40 } },
	{ AXIS_QUALITY_WEB_480P,     "web_480p",     "WEB 480p",         AXIS_QUALITY_CLASS_SD,       { 1, 40 } },
	{ AXIS_QUALITY_BLURAY_480P,  "bluray_480p",  "BluRay 480p/576p", AXIS_QUALITY_CLASS_SD,       { 2, 70 } },
	{ AXIS_QUALITY_HDTV_720P,    "hdtv_720p",    "HDTV 720p",        AXIS_QUALITY_CLASS_720P,     { 3, 70 } },
	{ AXIS_QUALITY_WEBRIP_720P,  "webrip_720p",  "WEBRip 720p",      AXIS_QUALITY_CLASS_720P,     { 3, 70 } },
	{ AXIS_QUALITY_WEBDL_720P,   "webdl_720p",   "WEB-DL 720p",      AXIS_QUALITY_CLASS_720P,     { 3, 70 } },
	{ AXIS_QUALITY_BLURAY_720P,  "bluray_720p",  "BluRay 720p",      AXIS_QUALITY_CLASS_720P,     { 4, 100 } },
	{ AXIS_QUALITY_HDTV_1080P,   "hdtv_1080p",   "HDTV 1080p",       AXIS_QUALITY_CLASS_1080P,    { 4, 130 } },
	{ AXIS_QUALITY_WEBRIP_1080P, "webrip_1080p", "WEBRip 1080p",     AXIS_QUALITY_CLASS_1080P,    { 4, 130 } },
	{ AXIS_QUALITY_WEBDL_1080P,  "webdl_1080p",  "WEB-DL 1080p",     AXIS_QUALITY_CLASS_1080P,    { 4, 130 } },
	{ AXIS_QUALITY_BLURAY_1080P, "bluray_1080p", "BluRay 1080p",     AXIS_QUALITY_CLASS_1080P,    { 6, 200 } },
	{ AXIS_QUALITY_REMUX_1080P,  "remux_1080p",  "Remux 1080p",      AXIS_QUALITY_CLASS_1080P,    { 60, 400 } },
	{ AXIS_QUALITY_HDTV_2160P,   "hdtv_2160p",   "HDTV 2160p",       AXIS_QUALITY_CLASS_2160P,    { 10, 300 } },
	{ AXIS_QUALITY_WEBRIP_2160P, "webrip_2160p", "WEBRip 2160p",     AXIS_QUALITY_CLASS_2160P,    { 10, 300 } },
	{ AXIS_QUALITY_WEBDL_2160P,  "webdl_2160p",  "WEB-DL 2160p",     AXIS_QUALITY_CLASS_2160P,    { 10, 300 } },
	{ AXIS_QUALITY_BLURAY_2160P, "bluray_2160p", "BluRay 2160p",     AXIS_QUALITY_CLASS_2160P,    { 20, 500 } },
	{ AXIS_QUALITY_REMUX_2160P,  "remux_2160p",  "Remux 2160p",      AXIS_QUALITY_CLASS_2160P,    { 150, 1000 } },
	{ AXIS_QUALITY_DISC,         "disc",         "Disc",             AXIS_QUALITY_CLASS_DISC,     { 20, 1000 } }
};

static const Rung *
rung(AxisQuality quality)
{
	guint index = (guint)quality;

	return index <= AXIS_QUALITY_N_LADDER ? &rungs[index] : &rungs[0];
}

/* The resolution classes the ladder knows: 0 none, 1 SD, 2 720p, 3 1080p,
 * 4 2160p. */
static gint
resolution_step(AxisReleaseResolution resolution)
{
	switch (resolution) {
	case AXIS_RELEASE_RESOLUTION_480P:
	case AXIS_RELEASE_RESOLUTION_576P:
		return 1;
	case AXIS_RELEASE_RESOLUTION_720P:
		return 2;
	case AXIS_RELEASE_RESOLUTION_1080P:
		return 3;
	case AXIS_RELEASE_RESOLUTION_2160P:
		return 4;
	default:
		return 0;
	}
}

AxisQuality
axis_quality_of(
	AxisReleaseSource     source,
	AxisReleaseResolution resolution
){
	/* The qualities of the sources whose resolution goes with them. */
	static const AxisQuality hdtv[] = {
		AXIS_QUALITY_UNKNOWN, AXIS_QUALITY_SDTV, AXIS_QUALITY_HDTV_720P, AXIS_QUALITY_HDTV_1080P,
		AXIS_QUALITY_HDTV_2160P
	};
	static const AxisQuality webrip[] = {
		AXIS_QUALITY_UNKNOWN, AXIS_QUALITY_WEB_480P, AXIS_QUALITY_WEBRIP_720P, AXIS_QUALITY_WEBRIP_1080P,
		AXIS_QUALITY_WEBRIP_2160P
	};
	static const AxisQuality webdl[] = {
		AXIS_QUALITY_UNKNOWN, AXIS_QUALITY_WEB_480P, AXIS_QUALITY_WEBDL_720P, AXIS_QUALITY_WEBDL_1080P,
		AXIS_QUALITY_WEBDL_2160P
	};
	static const AxisQuality bluray[] = {
		AXIS_QUALITY_UNKNOWN, AXIS_QUALITY_BLURAY_480P, AXIS_QUALITY_BLURAY_720P, AXIS_QUALITY_BLURAY_1080P,
		AXIS_QUALITY_BLURAY_2160P
	};
	/* The ladder has no remux below 1080p: a remux there is that
	 * resolution's BluRay. */
	static const AxisQuality remux[] = {
		AXIS_QUALITY_UNKNOWN, AXIS_QUALITY_BLURAY_480P, AXIS_QUALITY_BLURAY_720P, AXIS_QUALITY_REMUX_1080P,
		AXIS_QUALITY_REMUX_2160P
	};
	gint step = resolution_step(resolution);

	switch (source) {
	/* By the source alone. */
	case AXIS_RELEASE_SOURCE_WORKPRINT:
		return AXIS_QUALITY_WORKPRINT;
	case AXIS_RELEASE_SOURCE_CAM:
		return AXIS_QUALITY_CAM;
	case AXIS_RELEASE_SOURCE_TELESYNC:
		return AXIS_QUALITY_TELESYNC;
	case AXIS_RELEASE_SOURCE_TELECINE:
		return AXIS_QUALITY_TELECINE;
	case AXIS_RELEASE_SOURCE_SCREENER:
		return AXIS_QUALITY_SCREENER;
	case AXIS_RELEASE_SOURCE_DVD:
		return AXIS_QUALITY_DVD;
	case AXIS_RELEASE_SOURCE_DISC:
		return AXIS_QUALITY_DISC;
	/* By the source and the resolution. */
	case AXIS_RELEASE_SOURCE_HDTV:
		return hdtv[step];
	case AXIS_RELEASE_SOURCE_WEBRIP:
		return webrip[step];
	case AXIS_RELEASE_SOURCE_WEBDL:
		return webdl[step];
	case AXIS_RELEASE_SOURCE_BLURAY:
		return bluray[step];
	case AXIS_RELEASE_SOURCE_REMUX:
		return remux[step];
	default:
		return AXIS_QUALITY_UNKNOWN;
	}
}

AxisQuality
axis_quality_of_release(const AxisRelease *release)
{
	if (release == NULL)
		return AXIS_QUALITY_UNKNOWN;
	return axis_quality_of(release->source, release->resolution);
}

const gchar *
axis_quality_name(AxisQuality quality)
{
	return rung(quality)->name;
}

gboolean
axis_quality_from_name(
	const gchar *name,
	AxisQuality *quality
){
	guint i;

	for (i = 1; name != NULL && i <= AXIS_QUALITY_N_LADDER; i++) {
		if (strcmp(rungs[i].name, name) == 0) {
			*quality = rungs[i].quality;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_quality_label(AxisQuality quality)
{
	return rung(quality)->label;
}

AxisQualityClass
axis_quality_class(AxisQuality quality)
{
	return rung(quality)->klass;
}

const gchar *
axis_quality_class_label(AxisQualityClass klass)
{
	switch (klass) {
	case AXIS_QUALITY_CLASS_UNWANTED:
		return "Unwanted";
	case AXIS_QUALITY_CLASS_SD:
		return "SD";
	case AXIS_QUALITY_CLASS_720P:
		return "720p";
	case AXIS_QUALITY_CLASS_1080P:
		return "1080p";
	case AXIS_QUALITY_CLASS_2160P:
		return "2160p";
	case AXIS_QUALITY_CLASS_DISC:
		return "Disc";
	default:
		return "Unknown";
	}
}

gboolean
axis_quality_is_unwanted(AxisQuality quality)
{
	return axis_quality_class(quality) == AXIS_QUALITY_CLASS_UNWANTED;
}

gint
axis_quality_compare(
	AxisQuality a,
	AxisQuality b
){
	guint x = (guint)a <= AXIS_QUALITY_N_LADDER ? (guint)a : 0;
	guint y = (guint)b <= AXIS_QUALITY_N_LADDER ? (guint)b : 0;

	return x < y ? -1 : x > y ? 1 : 0;
}

/* ── An unknown source at a resolution ─────────────────────────── */

/* The lowest stated source of @resolution's class. */
static AxisQuality
lowest_of(AxisReleaseResolution resolution)
{
	switch (resolution) {
	case AXIS_RELEASE_RESOLUTION_480P:
	case AXIS_RELEASE_RESOLUTION_576P:
		return AXIS_QUALITY_DVD;
	case AXIS_RELEASE_RESOLUTION_720P:
		return AXIS_QUALITY_HDTV_720P;
	case AXIS_RELEASE_RESOLUTION_1080P:
		return AXIS_QUALITY_HDTV_1080P;
	case AXIS_RELEASE_RESOLUTION_2160P:
		return AXIS_QUALITY_HDTV_2160P;
	default:
		return AXIS_QUALITY_UNKNOWN;
	}
}

/* Twice the rung of a quality; an unknown source at a resolution one below
 * twice the rung of that resolution's lowest stated source; nothing known
 * 0. */
static guint
on_disk_rank(
	AxisQuality           quality,
	AxisReleaseResolution resolution
){
	AxisQuality lowest;

	if (quality != AXIS_QUALITY_UNKNOWN && (guint)quality <= AXIS_QUALITY_N_LADDER)
		return 2 * (guint)quality;
	lowest = lowest_of(resolution);
	return lowest != AXIS_QUALITY_UNKNOWN ? 2 * (guint)lowest - 1 : 0;
}

const gchar *
axis_quality_unknown_source_label(AxisReleaseResolution resolution)
{
	switch (resolution) {
	case AXIS_RELEASE_RESOLUTION_480P:  return "an unknown source at 480p";
	case AXIS_RELEASE_RESOLUTION_576P:  return "an unknown source at 576p";
	case AXIS_RELEASE_RESOLUTION_720P:  return "an unknown source at 720p";
	case AXIS_RELEASE_RESOLUTION_1080P: return "an unknown source at 1080p";
	case AXIS_RELEASE_RESOLUTION_2160P: return "an unknown source at 2160p";
	default:                            return "an unknown quality";
	}
}

gboolean
axis_quality_above_unknown(
	AxisQuality           quality,
	AxisReleaseResolution resolution
){
	if (quality == AXIS_QUALITY_UNKNOWN || (guint)quality > AXIS_QUALITY_N_LADDER || axis_quality_is_unwanted(quality))
		return FALSE;
	return on_disk_rank(quality, AXIS_RELEASE_RESOLUTION_NONE) > on_disk_rank(AXIS_QUALITY_UNKNOWN, resolution);
}

guint
axis_quality_on_disk_rank(
	AxisQuality           quality,
	AxisReleaseResolution resolution
){
	return on_disk_rank(quality, resolution);
}

gint
axis_quality_compare_on_disk(
	AxisQuality           a,
	AxisReleaseResolution a_resolution,
	AxisQuality           b,
	AxisReleaseResolution b_resolution
){
	guint x = on_disk_rank(a, a_resolution);
	guint y = on_disk_rank(b, b_resolution);

	return x < y ? -1 : x > y ? 1 : 0;
}

AxisQualitySize
axis_quality_default_size(AxisQuality quality)
{
	return rung(quality)->size;
}

const gchar *
axis_quality_source_label(AxisReleaseSource source)
{
	switch (source) {
	case AXIS_RELEASE_SOURCE_CAM:       return "Cam (a recording in a cinema)";
	case AXIS_RELEASE_SOURCE_TELESYNC:  return "Telesync (a recording in a cinema)";
	case AXIS_RELEASE_SOURCE_TELECINE:  return "Telecine (a copy of the film print)";
	case AXIS_RELEASE_SOURCE_SCREENER:  return "Screener (a pre-release copy)";
	case AXIS_RELEASE_SOURCE_WORKPRINT: return "Workprint (an unfinished cut)";
	case AXIS_RELEASE_SOURCE_DVD:       return "DVD";
	case AXIS_RELEASE_SOURCE_HDTV:      return "HDTV";
	case AXIS_RELEASE_SOURCE_WEBRIP:    return "WEBRip";
	case AXIS_RELEASE_SOURCE_WEBDL:     return "WEB-DL";
	case AXIS_RELEASE_SOURCE_BLURAY:    return "BluRay";
	case AXIS_RELEASE_SOURCE_REMUX:     return "Remux";
	case AXIS_RELEASE_SOURCE_DISC:      return "Full disc";
	default:                            return NULL;
	}
}

gchar *
axis_quality_explain_place(const AxisRelease *release)
{
	const gchar *resolution;
	AxisQuality quality;

	if (release == NULL)
		return g_strdup("unknown: nothing was read");
	resolution = axis_release_resolution_name(release->resolution);
	quality = axis_quality_of_release(release);
	if (release->source == AXIS_RELEASE_SOURCE_NONE && resolution == NULL)
		return g_strdup("unknown: neither a source nor a resolution was found in the name");
	if (release->source == AXIS_RELEASE_SOURCE_NONE)
		return g_strdup("unknown: no source (WEB-DL, BluRay, ...) was found in the name");
	switch (release->source) {
	case AXIS_RELEASE_SOURCE_CAM:
	case AXIS_RELEASE_SOURCE_TELESYNC:
	case AXIS_RELEASE_SOURCE_TELECINE:
	case AXIS_RELEASE_SOURCE_SCREENER:
	case AXIS_RELEASE_SOURCE_WORKPRINT:
	case AXIS_RELEASE_SOURCE_DVD:
	case AXIS_RELEASE_SOURCE_DISC:
		return g_strdup_printf("%s, by its source alone, whatever its resolution", axis_quality_label(quality));
	default:
		break;
	}
	if (resolution == NULL)
		return g_strdup("unknown: no resolution (720p, 1080p, ...) was found in the name");
	if (release->source == AXIS_RELEASE_SOURCE_REMUX && quality != AXIS_QUALITY_REMUX_1080P &&
	    quality != AXIS_QUALITY_REMUX_2160P)
		return g_strdup_printf("%s, as the ladder has no remux below 1080p", axis_quality_label(quality));
	return g_strdup(axis_quality_label(quality));
}
