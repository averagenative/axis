/*
 * test-estimate.c - the estimator's decision and size model, without a
 * database: one fixture per reason, the order when several apply, the
 * ranges, monotonicity, copied and dropped tracks, albums, calibration,
 * totals, and random stream models
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>
#include <string.h>

#include "axis-test-estimate.h"
#include "transcode/axis-estimate.h"
#include "transcode/axis-estimate-private.h"

#define GB G_GINT64_CONSTANT(1000000000)

/* A 1080p H.264 film of two hours at 8 Mbit/s with a 5.1 E-AC-3 track. */
typedef struct
{
	AxisMediaInfo    *info;
	AxisEstimateFile  file;
} Case;

static void
case_video(
	Case        *c,
	const gchar *codec,
	gint         width,
	gint         height,
	gint64       bitrate
){
	memset(c, 0, sizeof(*c));
	c->info = axis_test_video_info(codec, width, height, 24.0, bitrate, 7200.0, AXIS_HDR_SDR);
	axis_test_estimate_index(&c->file.index, c->info, "video", AXIS_MEDIA_MOVIES, axis_test_natural_size(c->info));
	c->file.info = c->info;
}

static void
case_film(Case *c)
{
	case_video(c, "h264", 1920, 1080, 8000000);
}

/* A FLAC track of four minutes, 16-bit 44.1 kHz stereo, at 900 kbit/s. */
static void
case_track(Case *c)
{
	memset(c, 0, sizeof(*c));
	c->info = axis_test_audio_info("flac", AXIS_TRI_YES, 900000, 240.0, 2, 44100, 16);
	axis_test_estimate_index(&c->file.index, c->info, "audio", AXIS_MEDIA_MUSIC, axis_test_natural_size(c->info));
	c->file.index.rel_path = "Artist/Album/01 Track.flac";
	c->file.index.unit = "Artist/Album";
	c->file.index.unit_files = 1;
	c->file.info = c->info;
}

static void
case_clear(Case *c)
{
	axis_media_info_free(c->info);
	memset(c, 0, sizeof(*c));
}

/* The size the index and the record agree on, after a stream changed. */
static void
case_resize(Case *c)
{
	gint64 size = axis_test_natural_size(c->info);

	c->file.index.size = size;
	c->info->file_size = size;
}

static const AxisEstimateTarget *
target(const gchar *id)
{
	const AxisEstimateTarget *t = axis_estimate_builtin_target(id);

	g_assert_nonnull(t);
	return t;
}

static AxisEstimateReason
reason_of(
	Case                    *c,
	const gchar             *target_id,
	const AxisEstimateModel *model
){
	AxisEstimateResult result;

	return axis_estimate_eligibility(&c->file, target(target_id), model, &result);
}

static void
assert_reason(
	Case                    *c,
	const gchar             *target_id,
	const AxisEstimateModel *model,
	AxisEstimateReason       expected
){
	AxisEstimateReason got = reason_of(c, target_id, model);

	if (got != expected)
		g_error("%s: expected %s, got %s", target_id, axis_estimate_reason_to_string(expected),
			axis_estimate_reason_to_string(got));
}

/* ── Names ──────────────────────────────────────────────────────── */

static void
test_names(void)
{
	const gchar *const *ids = axis_estimate_reason_ids();
	const gchar *const *targets = axis_estimate_target_ids();
	const AxisEstimateTarget *builtins = axis_estimate_builtin_targets();
	guint i;

	/* Every reason but "eligible", in the order of the enumeration. */
	for (i = 0; ids[i] != NULL; i++) {
		AxisEstimateReason reason;

		g_assert_true(axis_estimate_reason_from_string(ids[i], &reason));
		g_assert_cmpint(reason, ==, i + 1);
		g_assert_cmpstr(axis_estimate_reason_to_string(reason), ==, ids[i]);
		g_assert_cmpstr(axis_estimate_reason_label(reason), !=, "");
		g_assert_cmpstr(axis_estimate_reason_help(reason), !=, "");
	}
	g_assert_cmpuint(i, ==, AXIS_ESTIMATE_N_REASONS - 1);

	for (i = 0; targets[i] != NULL; i++) {
		g_autofree gchar *text = NULL;

		g_assert_cmpstr(builtins[i].id, ==, targets[i]);
		g_assert_true(axis_estimate_builtin_target(targets[i]) == &builtins[i]);
		text = axis_estimate_target_describe(&builtins[i]);
		g_assert_cmpuint(strlen(text), >, 40);
	}
	g_assert_cmpuint(i, ==, AXIS_ESTIMATE_N_TARGETS);
	g_assert_null(axis_estimate_builtin_target("nope"));

	/* The three video and four audio families (the conversion-profiles spec). */
	g_assert_cmpint(target("space_saver")->media, ==, AXIS_TARGET_VIDEO);
	g_assert_cmpint(target("balanced")->media, ==, AXIS_TARGET_VIDEO);
	g_assert_cmpint(target("max_compat")->codec, ==, AXIS_TCODEC_H264);
	g_assert_cmpint(target("keep_lossless")->codec, ==, AXIS_TCODEC_FLAC);
	g_assert_cmpint(target("music_space_saver")->codec, ==, AXIS_TCODEC_OPUS);
	g_assert_cmpint(target("music_compat")->codec, ==, AXIS_TCODEC_MP3);
	g_assert_true(target("audiobooks")->allow_lossy_source);
}

/* ── One fixture per reason ─────────────────────────────────────── */

static void
test_each_reason(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	Case c;

	case_film(&c);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "balanced", model, AXIS_ESTIMATE_ELIGIBLE);
	/* Maximum compatibility has a threshold of its own (0.120 bits per
	 * pixel, about what its own encode makes of 1080p), no longer the
	 * Analysis "large for its class" one (0.30): the film's 0.16 is
	 * converted, H.264 at 0.098 is already efficient. */
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_ELIGIBLE);
	case_clear(&c);
	case_video(&c, "h264", 1920, 1080, 4900000);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_ALREADY_EFFICIENT);
	case_clear(&c);
	case_video(&c, "h264", 1920, 1080, 20000000);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_ELIGIBLE);
	case_clear(&c);

	/* Not applicable: a video target and a music library, an audio file. */
	case_film(&c);
	c.file.index.root_type = AXIS_MEDIA_MUSIC;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	c.file.index.root_type = AXIS_MEDIA_DOWNLOADS;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	c.file.index.root_type = AXIS_MEDIA_MOVIES;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	c.file.index.kind = "audio";
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	c.file.index.kind = NULL;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	case_clear(&c);

	case_film(&c);
	c.file.index.state = "suspected";
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_PRESENT);
	c.file.index.state = NULL;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_PRESENT);
	case_clear(&c);

	/* Pinned: the flag, or a link count that is not one. */
	case_film(&c);
	c.file.index.pinned = TRUE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PINNED);
	c.file.index.pinned = FALSE;
	c.file.index.nlink = 2;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PINNED);
	c.file.index.nlink = 0;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PINNED);
	case_clear(&c);

	case_film(&c);
	c.file.info = NULL;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_PROBED);
	c.file.info = c.info;
	g_free(c.info->status);
	c.info->status = g_strdup("timeout");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_FAILED);
	g_free(c.info->status);
	c.info->status = NULL;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_FAILED);
	g_free(c.info->status);
	c.info->status = g_strdup("ok");
	case_clear(&c);

	/* Stale: changed, an older probe, or a stamp that is not the index's. */
	case_film(&c);
	c.file.index.changed = TRUE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_STALE);
	c.file.index.changed = FALSE;
	c.info->probe_version = AXIS_PROBE_VERSION - 1;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_STALE);
	c.info->probe_version = AXIS_PROBE_VERSION;
	c.file.index.inode++;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_STALE);
	c.file.index.inode--;
	c.file.index.size++;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_STALE);
	c.file.index.size--;
	c.file.index.mtime_nsec++;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_STALE);
	c.file.index.mtime_nsec--;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	case_clear(&c);

	/* HDR: uncertain, unclassified, Dolby Vision, HDR10+, HDR to SDR. */
	case_film(&c);
	c.info->hdr_uncertain = TRUE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_HDR_UNCERTAIN);
	c.info->hdr_uncertain = FALSE;
	c.info->hdr_format = AXIS_HDR_NONE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_HDR_UNCERTAIN);
	c.info->hdr_format = AXIS_HDR_SDR;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->hdr_uncertain = TRUE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_HDR_UNCERTAIN);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->hdr_uncertain = FALSE;
	c.info->hdr_format = AXIS_HDR_DOLBY_VISION;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->hdr_format = AXIS_HDR_DOLBY_VISION;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_DOLBY_VISION);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_DOLBY_VISION);
	c.info->hdr_format = AXIS_HDR_HDR10PLUS;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->hdr_format = AXIS_HDR_HDR10PLUS;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_HDR10PLUS);
	c.info->hdr_format = AXIS_HDR_HDR10;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->hdr_format = AXIS_HDR_HDR10;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "balanced", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_HDR_TO_SDR);
	c.info->hdr_format = AXIS_HDR_HLG;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->hdr_format = AXIS_HDR_HLG;
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_HDR_TO_SDR);
	case_clear(&c);

	/* A second picture's HDR counts too. */
	case_film(&c);
	{
		AxisMediaStream *v = axis_test_add_stream(c.info, AXIS_STREAM_VIDEO, "h264");

		v->width = 320;
		v->height = 180;
		v->hdr_format = AXIS_HDR_DOLBY_VISION;
	}
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_DOLBY_VISION);
	case_clear(&c);

	/* Interlaced, or not known to be progressive. */
	case_film(&c);
	c.info->interlaced = AXIS_TRI_YES;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_INTERLACED);
	c.info->interlaced = AXIS_TRI_UNKNOWN;
	g_free(((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->field_order);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->field_order = NULL;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->field_order = g_strdup("tt");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_INTERLACED);
	case_clear(&c);

	/* Layout: a data stream, two pictures, streams left out, a container,
	 * image subtitles and an audio codec MP4 cannot carry. */
	case_film(&c);
	axis_test_add_stream(c.info, AXIS_STREAM_DATA, "bin_data");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_film(&c);
	{
		AxisMediaStream *v = axis_test_add_stream(c.info, AXIS_STREAM_VIDEO, "h264");

		v->width = 320;
		v->height = 180;
		v->hdr_format = AXIS_HDR_SDR;
	}
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_film(&c);
	c.info->streams_omitted = 1;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	c.info->streams_omitted = 0;
	g_free(c.info->container);
	c.info->container = g_strdup("flv");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	g_free(c.info->container);
	c.info->container = NULL;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	g_free(c.info->container);
	c.info->container = g_strdup("matroska,webm");
	axis_test_add_stream(c.info, AXIS_STREAM_SUBTITLE, "hdmv_pgs_subtitle");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_film(&c);
	axis_test_add_stream(c.info, AXIS_STREAM_SUBTITLE, "ass");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_film(&c);
	axis_test_add_stream(c.info, AXIS_STREAM_SUBTITLE, "some_new_subtitles");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_film(&c);
	g_free(((AxisMediaStream *)g_ptr_array_index(c.info->streams, 1))->codec);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 1))->codec = g_strdup("truehd");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_film(&c);
	axis_test_add_stream(c.info, AXIS_STREAM_ATTACHMENT, "ttf");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);

	/* A codec without figures. */
	case_video(&c, "prores", 1920, 1080, 8000000);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_CODEC_UNKNOWN);
	case_clear(&c);

	/* Missing and inconsistent facts. */
	case_film(&c);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->bitrate = AXIS_PROBE_UNKNOWN;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FACTS_MISSING);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->bitrate = 8000000;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->frame_rate = -1.0;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->real_frame_rate = -1.0;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FACTS_MISSING);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->frame_rate = 24.0;
	c.info->duration_ms = 0;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->duration_ms = 0;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FACTS_MISSING);
	case_clear(&c);
	case_film(&c);
	c.file.index.size /= 3;
	c.info->file_size = c.file.index.size;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FACTS_INCONSISTENT);
	case_clear(&c);

	/* A better codec; already efficient (the spec's scenarios). */
	case_video(&c, "av1", 1920, 1080, 3000000);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_BETTER_CODEC);
	case_clear(&c);
	case_video(&c, "hevc", 1920, 1080, 4000000);
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_BETTER_CODEC);
	/* 4 Mbit/s at 1080p24 is 0.080 bits per pixel: below the HEVC
	 * threshold (0.150). */
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ALREADY_EFFICIENT);
	case_clear(&c);
	case_video(&c, "hevc", 1920, 1080, 12000000);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	case_clear(&c);

	/* Would not shrink: 1.5 Mbit/s H.264 has little left to give. */
	case_video(&c, "h264", 1920, 1080, 1500000);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_WOULD_NOT_SHRINK);
	case_clear(&c);

	/* Offline: eligible, its volume away. */
	case_film(&c);
	c.file.index.volume_usable = FALSE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_OFFLINE);
	case_clear(&c);

	/* Audio: lossy source, codec unknown, layout, facts. */
	case_track(&c);
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	assert_reason(&c, "music_compat", model, AXIS_ESTIMATE_ELIGIBLE);
	/* FLAC at the highest level from FLAC saves too little. */
	assert_reason(&c, "keep_lossless", model, AXIS_ESTIMATE_WOULD_NOT_SHRINK);
	assert_reason(&c, "audiobooks", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	case_clear(&c);
	case_track(&c);
	g_free(((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->codec);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->codec = g_strdup("mp3");
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->lossless = AXIS_TRI_NO;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_LOSSY_SOURCE);
	assert_reason(&c, "keep_lossless", model, AXIS_ESTIMATE_LOSSY_SOURCE);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->lossless = AXIS_TRI_UNKNOWN;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_CODEC_UNKNOWN);
	case_clear(&c);
	case_track(&c);
	g_free(((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->codec);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->codec = g_strdup("wavpack");
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_CODEC_UNKNOWN);
	case_clear(&c);
	case_track(&c);
	axis_test_add_stream(c.info, AXIS_STREAM_SUBTITLE, "text");
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);
	case_track(&c);
	{
		/* A cover is not a second picture. */
		AxisMediaStream *cover = axis_test_add_stream(c.info, AXIS_STREAM_VIDEO, "mjpeg");

		cover->attached_pic = TRUE;
		c.info->has_embedded_art = TRUE;
	}
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	c.info->duration_ms = 0;
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->duration_ms = 0;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_FACTS_MISSING);
	case_clear(&c);
	case_track(&c);
	c.file.index.size /= 2;
	c.info->file_size = c.file.index.size;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_FACTS_INCONSISTENT);
	case_clear(&c);
	case_track(&c);
	c.file.index.volume_usable = FALSE;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_OFFLINE);
	case_clear(&c);

	/* Audiobooks: lossy allowed when well above the target; Opus at the
	 * target's bitrate is already efficient. */
	memset(&c, 0, sizeof(c));
	c.info = axis_test_audio_info("mp3", AXIS_TRI_NO, 128000, 3600.0, 2, 44100, -1);
	axis_test_estimate_index(&c.file.index, c.info, "audio", AXIS_MEDIA_AUDIOBOOKS, axis_test_natural_size(c.info));
	c.file.index.unit = "Author/Book";
	c.file.index.unit_files = 1;
	c.file.info = c.info;
	assert_reason(&c, "audiobooks", model, AXIS_ESTIMATE_ELIGIBLE);
	g_free(((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->codec);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->codec = g_strdup("opus");
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 0))->bitrate = 48000;
	case_resize(&c);
	assert_reason(&c, "audiobooks", model, AXIS_ESTIMATE_ALREADY_EFFICIENT);
	case_clear(&c);
}

/* ── The order when several apply ───────────────────────────────── */

/*
 * A film with every problem the video targets know, fixed one at a time
 * in the fixed order: at each step the reason is the first that is still
 * there, whatever else is wrong with it.
 */
static void
test_order_video(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	AxisMediaStream *v;
	AxisMediaStream *data;
	Case c;

	case_film(&c);
	v = g_ptr_array_index(c.info->streams, 0);
	c.file.index.kind = "audio";
	c.file.index.state = "missing";
	c.file.index.pinned = TRUE;
	c.file.index.changed = TRUE;
	c.file.index.volume_usable = FALSE;
	c.file.info = NULL;
	g_free(c.info->status);
	c.info->status = g_strdup("failed");
	c.info->hdr_uncertain = TRUE;
	c.info->hdr_format = AXIS_HDR_DOLBY_VISION;
	v->hdr_format = AXIS_HDR_DOLBY_VISION;
	c.info->interlaced = AXIS_TRI_YES;
	data = axis_test_add_stream(c.info, AXIS_STREAM_DATA, "bin_data");
	g_free(v->codec);
	v->codec = g_strdup("prores");
	v->bitrate = AXIS_PROBE_UNKNOWN;

	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	c.file.index.kind = "video";
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_PRESENT);
	c.file.index.state = "present";
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PINNED);
	c.file.index.pinned = FALSE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_NOT_PROBED);
	c.file.info = c.info;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_FAILED);
	g_free(c.info->status);
	c.info->status = g_strdup("ok");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_PROBE_STALE);
	c.file.index.changed = FALSE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_HDR_UNCERTAIN);
	c.info->hdr_uncertain = FALSE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_DOLBY_VISION);
	c.info->hdr_format = AXIS_HDR_HDR10PLUS;
	v->hdr_format = AXIS_HDR_HDR10PLUS;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_HDR10PLUS);
	c.info->hdr_format = AXIS_HDR_HDR10;
	v->hdr_format = AXIS_HDR_HDR10;
	assert_reason(&c, "max_compat", model, AXIS_ESTIMATE_HDR_TO_SDR);
	c.info->hdr_format = AXIS_HDR_SDR;
	v->hdr_format = AXIS_HDR_SDR;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_INTERLACED);
	c.info->interlaced = AXIS_TRI_UNKNOWN;
	g_clear_pointer(&v->field_order, g_free);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN);
	c.info->interlaced = AXIS_TRI_NO;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_LAYOUT);
	g_ptr_array_remove(c.info->streams, data);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_CODEC_UNKNOWN);
	g_free(v->codec);
	v->codec = g_strdup("av1");
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FACTS_MISSING);
	/* The size the index has is that of an 8 Mbit/s file: 30 Mbit/s does
	 * not fit in it. */
	v->bitrate = 30000000;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_FACTS_INCONSISTENT);
	v->bitrate = 8000000;
	c.info->bits_per_pixel = 8000000.0 / (1920.0 * 1080 * 24);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_BETTER_CODEC);
	g_free(v->codec);
	v->codec = g_strdup("hevc");
	c.info->bits_per_pixel = 0.1;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ALREADY_EFFICIENT);
	g_free(v->codec);
	v->codec = g_strdup("h264");
	v->bitrate = 1500000;
	c.info->bits_per_pixel = -1.0;
	case_resize(&c);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_WOULD_NOT_SHRINK);
	v->bitrate = 8000000;
	case_resize(&c);
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_OFFLINE);
	c.file.index.volume_usable = TRUE;
	assert_reason(&c, "space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	case_clear(&c);
}

/* The same for an album: a track's own reasons first, then the album's. */
static void
test_order_audio(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	AxisMediaStream *a;
	AxisMediaStream *sub;
	Case c;

	case_track(&c);
	a = g_ptr_array_index(c.info->streams, 0);
	c.file.index.root_type = AXIS_MEDIA_MOVIES;
	c.file.index.volume_usable = FALSE;
	sub = axis_test_add_stream(c.info, AXIS_STREAM_SUBTITLE, "text");
	g_free(a->codec);
	a->codec = g_strdup("wavpack");
	a->lossless = AXIS_TRI_UNKNOWN;

	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_NOT_APPLICABLE);
	c.file.index.root_type = AXIS_MEDIA_MUSIC;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_LAYOUT);
	g_ptr_array_remove(c.info->streams, sub);
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_CODEC_UNKNOWN);
	g_free(a->codec);
	a->codec = g_strdup("mp3");
	a->lossless = AXIS_TRI_NO;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_LOSSY_SOURCE);
	g_free(a->codec);
	a->codec = g_strdup("flac");
	a->lossless = AXIS_TRI_YES;
	c.info->duration_ms = 0;
	a->duration_ms = 0;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_FACTS_MISSING);
	c.info->duration_ms = 240000;
	a->duration_ms = 240000;
	a->bitrate = 9000000;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_FACTS_INCONSISTENT);
	a->bitrate = 900000;
	/* Two files in its album, only one given. */
	c.file.index.unit_files = 2;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_ALBUM);
	c.file.index.unit_files = 1;
	assert_reason(&c, "keep_lossless", model, AXIS_ESTIMATE_WOULD_NOT_SHRINK);
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_OFFLINE);
	c.file.index.volume_usable = TRUE;
	assert_reason(&c, "music_space_saver", model, AXIS_ESTIMATE_ELIGIBLE);
	case_clear(&c);
}

/* ── Ranges ─────────────────────────────────────────────────────── */

static void
test_never_larger(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	const AxisEstimateTarget *targets = axis_estimate_builtin_targets();
	static const gint64 rates[] = { 2500000, 5000000, 8000000, 15000000, 40000000 };
	static const gint sizes[][2] = { { 720, 480 }, { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
	guint checked = 0;
	guint i;
	guint j;
	guint k;

	for (i = 0; i < G_N_ELEMENTS(rates); i++) {
		for (j = 0; j < G_N_ELEMENTS(sizes); j++) {
			for (k = 0; k < 3; k++) {
				AxisEstimateResult result;
				Case c;

				case_video(&c, "h264", sizes[j][0], sizes[j][1], rates[i] * (sizes[j][0] / 720));
				axis_estimate_eligibility(&c.file, &targets[k], model, &result);
				/* Already efficient files are decided before any size. */
				g_assert_true(result.sized || result.reason == AXIS_ESTIMATE_ALREADY_EFFICIENT);
				g_assert_cmpint(result.low, >=, 0);
				g_assert_cmpint(result.low, <=, result.mid);
				g_assert_cmpint(result.mid, <=, result.high);
				if (axis_estimate_result_converts(&result)) {
					g_assert_cmpint(result.high, <=, c.file.index.size);
					g_assert_cmpint(result.mid, <, c.file.index.size);
					checked++;
				}
				case_clear(&c);
			}
		}
	}
	g_assert_cmpuint(checked, >, 20);
}

/* ── Monotonic ──────────────────────────────────────────────────── */

/* A model with samples in some cells, so that monotonicity is checked on a
 * calibrated table too (samples that pull a higher level below a lower
 * one). */
static AxisEstimateModel *
uneven_model(void)
{
	AxisEstimateModel *model = axis_estimate_model_new();
	AxisEstimateSample sample;
	guint i;

	memset(&sample, 0, sizeof(sample));
	sample.target = "space_saver";
	sample.codec = AXIS_TCODEC_HEVC;
	sample.family = AXIS_ENCODER_SOFTWARE;
	sample.part = "video";
	sample.source_codec = "h264";
	sample.width = 1920;
	sample.height = 1080;
	sample.frame_rate = 24.0;
	sample.source_bitrate = 8000000;
	sample.seconds = 60.0;
	for (i = 0; i < 25; i++) {
		/* Level 5 much smaller than its prior, level 3 much larger. */
		sample.quality = 5;
		sample.output_bitrate = 600000;
		g_assert_true(axis_estimate_model_add_sample(model, &sample));
		sample.quality = 3;
		sample.output_bitrate = 9000000;
		g_assert_true(axis_estimate_model_add_sample(model, &sample));
		/* 2160p far below 1080p. */
		sample.quality = 4;
		sample.width = 3840;
		sample.height = 2160;
		sample.source_bitrate = 30000000;
		sample.output_bitrate = 300000;
		g_assert_true(axis_estimate_model_add_sample(model, &sample));
		sample.width = 1920;
		sample.height = 1080;
		sample.source_bitrate = 8000000;
	}
	axis_estimate_model_finish(model);
	return model;
}

static void
assert_not_smaller(
	const AxisEstimateResult *lower,
	const AxisEstimateResult *higher,
	const gchar              *what
){
	if (!lower->sized || !higher->sized)
		return;
	if (higher->low < lower->low || higher->mid < lower->mid || higher->high < lower->high)
		g_error("%s: %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " after %" G_GINT64_FORMAT
			"/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, what, higher->low, higher->mid, higher->high, lower->low,
			lower->mid, lower->high);
}

static void
test_monotonic_quality(void)
{
	AxisEstimateModel *models[2];
	guint m;

	models[0] = axis_estimate_model_new();
	models[1] = uneven_model();
	for (m = 0; m < 2; m++) {
		guint t;

		for (t = 0; t < 3; t++) {
			guint n;

			for (n = 0; n < 200; n++) {
				AxisEstimateTarget copy = axis_estimate_builtin_targets()[t];
				AxisEstimateResult previous;
				gint width = g_test_rand_int_range(160, 5000);
				gint height = MAX(90, width * 9 / 16);
				gdouble bpp = g_test_rand_double_range(0.01, 1.5);
				gint64 rate = (gint64)(bpp * width * height * 24.0);
				gint level;
				Case c;

				case_video(&c, n % 3 == 0 ? "mpeg2video" : "h264", width, height, MAX(rate, 1));
				memset(&previous, 0, sizeof(previous));
				copy.n_quality_rules = 0;
				for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
					AxisEstimateResult result;

					copy.quality = level;
					axis_estimate_eligibility(&c.file, &copy, models[m], &result);
					if (level > AXIS_QUALITY_MIN)
						assert_not_smaller(&previous, &result, "a higher quality level");
					previous = result;
				}
				case_clear(&c);
			}
		}
	}
	axis_estimate_model_free(models[0]);
	axis_estimate_model_free(models[1]);
}

/* At equal source bits per pixel (and everything else equal), a larger
 * picture is never estimated smaller. */
static void
test_monotonic_resolution(void)
{
	AxisEstimateModel *models[2];
	guint m;

	models[0] = axis_estimate_model_new();
	models[1] = uneven_model();
	for (m = 0; m < 2; m++) {
		guint t;

		for (t = 0; t < 3; t++) {
			AxisEstimateTarget copy = axis_estimate_builtin_targets()[t];
			guint n;

			copy.n_quality_rules = 0;
			for (n = 0; n < 40; n++) {
				gdouble bpp = g_test_rand_double_range(0.02, 1.0);
				AxisEstimateResult previous;
				gint width;
				gboolean first = TRUE;

				for (width = 160; width <= 7680; width += 40) {
					AxisEstimateResult result;
					gint height = width * 9 / 16;
					Case c;

					case_video(&c, "h264", width, height, MAX((gint64)(bpp * width * height * 24.0), 1));
					axis_estimate_eligibility(&c.file, &copy, models[m], &result);
					if (!first)
						assert_not_smaller(&previous, &result, "a larger picture");
					previous = result;
					first = FALSE;
					case_clear(&c);
				}
			}
		}
	}
	axis_estimate_model_free(models[0]);
	axis_estimate_model_free(models[1]);
}

/* The priors alone are monotonic at equal source bitrate too. */
static void
test_monotonic_resolution_bitrate(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	guint t;

	for (t = 0; t < 3; t++) {
		AxisEstimateTarget copy = axis_estimate_builtin_targets()[t];
		guint n;

		copy.n_quality_rules = 0;
		for (n = 0; n < 20; n++) {
			gint64 rate = g_test_rand_int_range(500000, 60000000);
			AxisEstimateResult previous;
			gboolean first = TRUE;
			gint width;

			for (width = 320; width <= 7680; width += 40) {
				AxisEstimateResult result;
				Case c;

				case_video(&c, "h264", width, width * 9 / 16, rate);
				axis_estimate_eligibility(&c.file, &copy, model, &result);
				if (!first && previous.sized && result.sized && result.mid < previous.mid)
					g_error("width %d at %" G_GINT64_FORMAT " bit/s: %" G_GINT64_FORMAT " after %" G_GINT64_FORMAT,
						width, rate, result.mid, previous.mid);
				previous = result;
				first = FALSE;
				case_clear(&c);
			}
		}
	}
}

/* ── Copied and dropped tracks ──────────────────────────────────── */

static void
test_copied_and_dropped(void)
{
	static const gchar *const english[] = { "eng", NULL };
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	AxisEstimateTarget keep_english = *target("space_saver");
	AxisEstimateResult all;
	AxisEstimateResult some;
	AxisMediaStream *a;
	AxisMediaStream *french;
	gdouble french_bytes;
	Case c;

	/* A 4 GB TrueHD track is in every figure at its size. */
	case_film(&c);
	a = g_ptr_array_index(c.info->streams, 1);
	g_free(a->codec);
	a->codec = g_strdup("truehd");
	a->bitrate = (gint64)(4.0 * GB * 8 / 7200.0);
	a->lossless = AXIS_TRI_YES;
	case_resize(&c);
	axis_estimate_eligibility(&c.file, target("space_saver"), model, &all);
	g_assert_cmpint(all.reason, ==, AXIS_ESTIMATE_ELIGIBLE);
	g_assert_cmpint(all.low, >=, 4 * GB);
	g_assert_cmpint(all.mid, <, c.file.index.size);
	case_clear(&c);

	/* A French track dropped by a rule that keeps English: the estimate is
	 * smaller by its size. */
	case_film(&c);
	french = axis_test_add_stream(c.info, AXIS_STREAM_AUDIO, "eac3");
	french->bitrate = 640000;
	french->channels = 6;
	french->language = g_strdup("fre");
	case_resize(&c);
	french_bytes = 640000.0 * 7200.0 / 8.0;
	keep_english.keep_audio_languages = english;
	axis_estimate_eligibility(&c.file, target("space_saver"), model, &all);
	axis_estimate_eligibility(&c.file, &keep_english, model, &some);
	g_assert_cmpint(all.reason, ==, AXIS_ESTIMATE_ELIGIBLE);
	g_assert_cmpint(some.reason, ==, AXIS_ESTIMATE_ELIGIBLE);
	g_assert_cmpfloat(fabs((gdouble)(all.mid - some.mid) - french_bytes), <, french_bytes * 0.01);
	g_assert_cmpfloat(fabs((gdouble)(all.low - some.low) - french_bytes), <, french_bytes * 0.01);
	/* A track of unknown language is never dropped; neither is the last. */
	g_clear_pointer(&french->language, g_free);
	axis_estimate_eligibility(&c.file, &keep_english, model, &some);
	g_assert_cmpint(some.mid, ==, all.mid);
	g_free(((AxisMediaStream *)g_ptr_array_index(c.info->streams, 1))->language);
	((AxisMediaStream *)g_ptr_array_index(c.info->streams, 1))->language = g_strdup("ger");
	french->language = g_strdup("fre");
	axis_estimate_eligibility(&c.file, &keep_english, model, &some);
	g_assert_cmpint(some.reason, ==, AXIS_ESTIMATE_LAYOUT);
	case_clear(&c);

	/* Balanced adds a stereo AAC track; Maximum compatibility adds one
	 * unless the first track is one. */
	case_film(&c);
	axis_estimate_eligibility(&c.file, target("space_saver"), model, &all);
	axis_estimate_eligibility(&c.file, target("balanced"), model, &some);
	g_assert_cmpint(some.mid - all.mid, >, (gint64)(160000.0 * 7200 / 8));
	case_clear(&c);
}

/* ── Albums ─────────────────────────────────────────────────────── */

static void
test_albums(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	const AxisEstimateTarget *opus = target("music_space_saver");
	const AxisEstimateFile *unit[3];
	AxisEstimateResult results[3];
	Case tracks[3];
	guint i;

	for (i = 0; i < 3; i++) {
		case_track(&tracks[i]);
		tracks[i].file.index.file_id = 10 + i;
		tracks[i].file.index.unit_files = 3;
		unit[i] = &tracks[i].file;
	}

	axis_estimate_evaluate(unit, 3, opus, model, results);
	for (i = 0; i < 3; i++)
		g_assert_cmpint(results[i].reason, ==, AXIS_ESTIMATE_ELIGIBLE);

	/* One track lossy: the others are not eligible, with a reason naming
	 * the album and the track. */
	g_free(((AxisMediaStream *)g_ptr_array_index(tracks[1].info->streams, 0))->codec);
	((AxisMediaStream *)g_ptr_array_index(tracks[1].info->streams, 0))->codec = g_strdup("mp3");
	((AxisMediaStream *)g_ptr_array_index(tracks[1].info->streams, 0))->lossless = AXIS_TRI_NO;
	axis_estimate_evaluate(unit, 3, opus, model, results);
	g_assert_cmpint(results[0].reason, ==, AXIS_ESTIMATE_ALBUM);
	g_assert_cmpint(results[0].album_reason, ==, AXIS_ESTIMATE_LOSSY_SOURCE);
	g_assert_cmpint(results[0].album_file, ==, 11);
	g_assert_cmpint(results[1].reason, ==, AXIS_ESTIMATE_LOSSY_SOURCE);
	g_assert_cmpint(results[2].reason, ==, AXIS_ESTIMATE_ALBUM);

	/* A track pinned, the same. */
	((AxisMediaStream *)g_ptr_array_index(tracks[1].info->streams, 0))->lossless = AXIS_TRI_YES;
	g_free(((AxisMediaStream *)g_ptr_array_index(tracks[1].info->streams, 0))->codec);
	((AxisMediaStream *)g_ptr_array_index(tracks[1].info->streams, 0))->codec = g_strdup("flac");
	tracks[2].file.index.pinned = TRUE;
	axis_estimate_evaluate(unit, 3, opus, model, results);
	g_assert_cmpint(results[0].reason, ==, AXIS_ESTIMATE_ALBUM);
	g_assert_cmpint(results[0].album_reason, ==, AXIS_ESTIMATE_PINNED);
	g_assert_cmpint(results[1].reason, ==, AXIS_ESTIMATE_ALBUM);
	g_assert_cmpint(results[2].reason, ==, AXIS_ESTIMATE_PINNED);
	tracks[2].file.index.pinned = FALSE;

	/* Not given whole: two of three, or one alone. */
	axis_estimate_evaluate(unit, 2, opus, model, results);
	g_assert_cmpint(results[0].reason, ==, AXIS_ESTIMATE_ALBUM);
	g_assert_cmpint(results[1].reason, ==, AXIS_ESTIMATE_ALBUM);
	g_assert_cmpint(axis_estimate_eligibility(unit[0], opus, model, NULL), ==, AXIS_ESTIMATE_ALBUM);
	/* Files of another album mixed in. */
	tracks[2].file.index.unit = "Artist/Other";
	axis_estimate_evaluate(unit, 3, opus, model, results);
	g_assert_cmpint(results[0].reason, ==, AXIS_ESTIMATE_ALBUM);
	tracks[2].file.index.unit = "Artist/Album";

	/* The album's saving decides, not a track's. */
	axis_estimate_evaluate(unit, 3, target("keep_lossless"), model, results);
	for (i = 0; i < 3; i++)
		g_assert_cmpint(results[i].reason, ==, AXIS_ESTIMATE_WOULD_NOT_SHRINK);

	for (i = 0; i < 3; i++)
		case_clear(&tracks[i]);
}

static void
test_unit_key(void)
{
	static const struct
	{
		const gchar   *kind;
		AxisMediaType  type;
		const gchar   *path;
		const gchar   *key;
	} cases[] = {
		{ "audio", AXIS_MEDIA_MUSIC, "Artist/Album/01 Track.flac", "Artist/Album" },
		{ "audio", AXIS_MEDIA_MUSIC, "Artist/Album/CD1/01 Track.flac", "Artist/Album" },
		{ "audio", AXIS_MEDIA_MUSIC, "Artist/Loose.flac", "Artist" },
		{ "audio", AXIS_MEDIA_MUSIC, "Loose.flac", "" },
		{ "audio", AXIS_MEDIA_AUDIOBOOKS, "Author/Book/Part 01.mp3", "Author/Book" },
		{ "audio", AXIS_MEDIA_MOVIES, "Film/Soundtrack.flac", NULL },
		{ "video", AXIS_MEDIA_MUSIC, "Artist/Album/clip.mkv", NULL },
		{ NULL, 0, NULL, NULL }
	};
	guint i;

	for (i = 0; cases[i].kind != NULL; i++) {
		g_autofree gchar *key = axis_estimate_unit_key(cases[i].kind, cases[i].type, cases[i].path);

		g_assert_cmpstr(key, ==, cases[i].key);
	}
}

/* ── Quality rule ───────────────────────────────────────────────── */

static void
test_quality_rule(void)
{
	const AxisEstimateTarget *space = target("space_saver");
	AxisEstimateConfig config;
	AxisEstimateTarget configured;

	g_assert_cmpint(axis_estimate_target_quality_for(space, "1080p", 60 * GB), ==, 4);
	g_assert_cmpint(axis_estimate_target_quality_for(space, "2160p", 60 * GB), ==, 3);
	g_assert_cmpint(axis_estimate_target_quality_for(space, "2160p", 30 * GB), ==, 4);
	g_assert_cmpint(axis_estimate_target_quality_for(space, "2160p", 10 * GB), ==, 5);
	g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_HEVC, AXIS_ENCODER_SOFTWARE, 3), ==, 28);
	g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_HEVC, AXIS_ENCODER_SOFTWARE, 4), ==, 26);
	g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_HEVC, AXIS_ENCODER_SOFTWARE, 5), ==, 24);

	/* The encoder is the one setting left that shapes a target (the 4K
	 * sizes, the minimum saving and the threshold are the profile's now:
	 * tests/test-profile.c). */
	axis_estimate_config_init(&config);
	config.encoder = AXIS_ENCODER_NVENC;
	axis_estimate_target_configure(space, &config, &configured);
	g_assert_cmpint(configured.encoder, ==, AXIS_ENCODER_NVENC);
	g_assert_cmpint(axis_estimate_target_quality_for(&configured, "2160p", 45 * GB), ==, 4);
	g_assert_cmpfloat(configured.min_saving, ==, 0.15);
	g_assert_cmpint(configured.efficient_milli, ==, 150);
	/* Audio is always software. */
	axis_estimate_target_configure(target("music_space_saver"), &config, &configured);
	g_assert_cmpint(configured.encoder, ==, AXIS_ENCODER_SOFTWARE);
}

/* ── Calibration ────────────────────────────────────────────────── */

/* The video part the uncalibrated model predicts for the film, as a
 * bitrate. */
static gdouble
predicted_video_bitrate(
	const AxisEstimateModel *model,
	Case                    *c
){
	AxisEstimateShape shape;
	AxisEstimatePart part;

	axis_estimate_shape_init(&shape, c->info);
	g_assert_true(axis_estimate_model_video(model, target("space_saver"), &shape, "h264", FALSE, 4, &part));
	return part.mid * 8.0 / shape.seconds;
}

static AxisEstimateModel *
model_with(
	guint   n,
	gdouble ratio
){
	g_autoptr(AxisEstimateModel) prior = axis_estimate_model_new();
	AxisEstimateModel *model = axis_estimate_model_new();
	AxisEstimateSample sample;
	gdouble predicted;
	guint i;
	Case c;

	case_film(&c);
	predicted = predicted_video_bitrate(prior, &c);
	case_clear(&c);

	memset(&sample, 0, sizeof(sample));
	sample.target = "space_saver";
	sample.codec = AXIS_TCODEC_HEVC;
	sample.family = AXIS_ENCODER_SOFTWARE;
	sample.quality = 4;
	sample.part = "video";
	sample.source_codec = "h264";
	sample.width = 1920;
	sample.height = 1080;
	sample.frame_rate = 24.0;
	sample.source_bitrate = 8000000;
	sample.seconds = 60.0;
	for (i = 0; i < n; i++) {
		/* A little scatter around the ratio. */
		sample.output_bitrate = (gint64)(predicted * ratio * (1.0 + 0.02 * ((gint)(i % 5) - 2)));
		g_assert_true(axis_estimate_model_add_sample(model, &sample));
	}
	axis_estimate_model_finish(model);
	return model;
}

static void
test_calibration(void)
{
	g_autoptr(AxisEstimateModel) none = model_with(0, 0.7);
	g_autoptr(AxisEstimateModel) one = model_with(1, 0.7);
	g_autoptr(AxisEstimateModel) half = model_with(10, 0.7);
	g_autoptr(AxisEstimateModel) nineteen = model_with(19, 0.7);
	g_autoptr(AxisEstimateModel) full = model_with(20, 0.7);
	g_autoptr(AxisEstimateModel) more = model_with(60, 0.7);
	AxisEstimateResult r_none;
	AxisEstimateResult r_half;
	AxisEstimateResult r_full;
	AxisEstimateCellInfo info;
	guint cells[8];
	guint n_cells;
	gdouble before;
	gdouble after;
	Case c;

	case_film(&c);
	axis_estimate_eligibility(&c.file, target("space_saver"), none, &r_none);
	axis_estimate_eligibility(&c.file, target("space_saver"), half, &r_half);
	axis_estimate_eligibility(&c.file, target("space_saver"), full, &r_full);
	g_assert_cmpint(r_none.reason, ==, AXIS_ESTIMATE_ELIGIBLE);
	g_assert_cmpint(r_none.cell, ==, r_full.cell);

	/* Results 30 % below the estimate move it there: half of the way in
	 * log terms with ten samples, all of it with twenty. */
	g_assert_cmpint(r_half.mid, <, r_none.mid);
	g_assert_cmpint(r_full.mid, <, r_half.mid);
	before = predicted_video_bitrate(none, &c);
	after = predicted_video_bitrate(full, &c);
	g_assert_cmpfloat(fabs(after / before - 0.7), <, 0.01);
	after = predicted_video_bitrate(half, &c);
	g_assert_cmpfloat(fabs(after / before - sqrt(0.7)), <, 0.01);
	/* Calibrated, the range is the scatter seen, much narrower. */
	g_assert_cmpfloat((gdouble)(r_full.high - r_full.low), <, (gdouble)(r_none.high - r_none.low) / 3.0);

	/* The state at the thresholds. */
	axis_estimate_model_cell_info(none, r_none.cell, &info);
	g_assert_cmpint(info.state, ==, AXIS_CALIBRATION_NONE);
	g_assert_cmpuint(info.samples, ==, 0);
	axis_estimate_model_cell_info(one, r_none.cell, &info);
	g_assert_cmpint(info.state, ==, AXIS_CALIBRATION_PARTLY);
	g_assert_cmpuint(info.samples, ==, 1);
	axis_estimate_model_cell_info(nineteen, r_none.cell, &info);
	g_assert_cmpint(info.state, ==, AXIS_CALIBRATION_PARTLY);
	axis_estimate_model_cell_info(full, r_none.cell, &info);
	g_assert_cmpint(info.state, ==, AXIS_CALIBRATION_FULL);
	g_assert_cmpuint(info.samples, ==, AXIS_ESTIMATE_CALIBRATED_SAMPLES);
	g_assert_cmpfloat(fabs(info.factor - 0.7), <, 0.01);
	axis_estimate_model_cell_info(more, r_none.cell, &info);
	g_assert_cmpint(info.state, ==, AXIS_CALIBRATION_FULL);
	g_assert_cmpstr(info.name, ==, "hevc/software/q4/1080p");

	/* A target is calibrated only when all its cells are. */
	n_cells = axis_estimate_model_target_cells(full, target("space_saver"), cells, G_N_ELEMENTS(cells));
	g_assert_cmpuint(n_cells, ==, 4);
	g_assert_cmpint(axis_estimate_model_state_of(none, cells, n_cells), ==, AXIS_CALIBRATION_NONE);
	g_assert_cmpint(axis_estimate_model_state_of(full, cells, n_cells), ==, AXIS_CALIBRATION_PARTLY);
	g_assert_cmpint(axis_estimate_model_state_of(full, &r_full.cell, 1), ==, AXIS_CALIBRATION_FULL);
	g_assert_cmpint(axis_estimate_model_state_of(full, NULL, 0), ==, AXIS_CALIBRATION_NONE);
	case_clear(&c);

	/* Samples that do not fit the model are refused. */
	{
		AxisEstimateSample bad;

		memset(&bad, 0, sizeof(bad));
		g_assert_false(axis_estimate_model_add_sample(none, &bad));
		bad.target = "space_saver";
		bad.part = "video";
		bad.source_codec = "h264";
		bad.codec = AXIS_TCODEC_HEVC;
		bad.quality = 4;
		bad.width = 1920;
		bad.height = 1080;
		bad.frame_rate = NAN;
		bad.source_bitrate = 1000;
		bad.output_bitrate = 1000;
		bad.seconds = 10;
		g_assert_false(axis_estimate_model_add_sample(none, &bad));
		bad.frame_rate = 24;
		bad.source_codec = "prores";
		g_assert_false(axis_estimate_model_add_sample(none, &bad));
		bad.source_codec = "h264";
		bad.part = "audio";
		g_assert_false(axis_estimate_model_add_sample(none, &bad));
	}
}

/* An audio sample lands in its target's cell. */
static void
test_calibration_audio(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	AxisEstimateSample sample;
	AxisEstimateResult before;
	AxisEstimateResult after;
	AxisEstimateCellInfo info;
	guint i;
	Case c;

	case_track(&c);
	axis_estimate_eligibility(&c.file, target("music_space_saver"), model, &before);
	memset(&sample, 0, sizeof(sample));
	sample.target = "music_space_saver";
	sample.codec = AXIS_TCODEC_OPUS;
	sample.family = AXIS_ENCODER_SOFTWARE;
	sample.quality = 4;
	sample.part = "audio";
	sample.source_codec = "flac";
	sample.source_bitrate = 900000;
	sample.sample_rate = 44100;
	sample.channels = 2;
	sample.bit_depth = 16;
	sample.seconds = 60.0;
	sample.output_bitrate = 140000;
	for (i = 0; i < 20; i++)
		g_assert_true(axis_estimate_model_add_sample(model, &sample));
	axis_estimate_model_finish(model);
	axis_estimate_eligibility(&c.file, target("music_space_saver"), model, &after);
	axis_estimate_model_cell_info(model, after.cell, &info);
	g_assert_cmpstr(info.name, ==, "opus/music");
	g_assert_cmpint(info.state, ==, AXIS_CALIBRATION_FULL);
	g_assert_cmpint(after.mid, >, before.mid);
	case_clear(&c);
}

/* ── Totals ─────────────────────────────────────────────────────── */

static void
test_totals(void)
{
	g_autoptr(AxisEstimateModel) model = model_with(20, 0.7);
	g_autoptr(AxisEstimateTotal) same = axis_estimate_total_new();
	g_autoptr(AxisEstimateTotal) apart = axis_estimate_total_new();
	AxisEstimateCellInfo a_info;
	AxisEstimateCellInfo b_info;
	AxisEstimateResult a;
	AxisEstimateResult b;
	gint64 files;
	gint64 now;
	gint64 low;
	gint64 mid;
	gint64 high;
	guint uncalibrated = 0;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	/* The calibrated cell (a group of its own) and an uncalibrated one. */
	{
		Case c;
		AxisEstimateResult r;

		case_film(&c);
		axis_estimate_eligibility(&c.file, target("space_saver"), model, &r);
		a.cell = r.cell;
		case_clear(&c);
		case_video(&c, "h264", 1280, 720, 6000000);
		axis_estimate_eligibility(&c.file, target("space_saver"), model, &r);
		uncalibrated = r.cell;
		case_clear(&c);
	}
	axis_estimate_model_cell_info(model, a.cell, &a_info);
	axis_estimate_model_cell_info(model, uncalibrated, &b_info);
	g_assert_cmpuint(a_info.group, !=, b_info.group);

	a.reason = AXIS_ESTIMATE_ELIGIBLE;
	a.now = 1000;
	a.low = 300;
	a.mid = 400;
	a.high = 600;
	b = a;
	b.now = 2000;
	b.low = 500;
	b.mid = 700;
	b.high = 1000;

	/* Same group: deviations add up in full. */
	axis_estimate_total_add(same, model, &a);
	axis_estimate_total_add(same, model, &b);
	axis_estimate_total_get(same, &files, &now, &low, &mid, &high);
	g_assert_cmpint(files, ==, 2);
	g_assert_cmpint(now, ==, 3000);
	g_assert_cmpint(mid, ==, 1100);
	g_assert_cmpint(low, ==, 1100 - 300);
	g_assert_cmpint(high, ==, 1100 + 500);

	/* Different groups: as independent. */
	b.cell = uncalibrated;
	axis_estimate_total_add(apart, model, &a);
	axis_estimate_total_add(apart, model, &b);
	axis_estimate_total_get(apart, &files, &now, &low, &mid, &high);
	g_assert_cmpint(mid, ==, 1100);
	g_assert_cmpint(low, ==, 1100 - (gint64)llround(sqrt(100.0 * 100 + 200.0 * 200)));
	g_assert_cmpint(high, ==, 1100 + (gint64)llround(sqrt(200.0 * 200 + 300.0 * 300)));
	g_assert_cmpuint(axis_estimate_total_cells(apart)->len, ==, 2);
}

/* ── Random stream models ───────────────────────────────────────── */

static const gchar *const codecs[] = {
	"h264", "hevc", "av1", "vp9", "mpeg2video", "prores", "flac", "mp3", "aac", "opus", "eac3", "truehd",
	"pcm_s16le", "alac", "ape", "wavpack", "subrip", "ass", "hdmv_pgs_subtitle", "mov_text", "", "\xff\xfe", NULL
};
static const gchar *const containers[] = {
	"matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2", "avi", "mpegts", "flac", "mp3", "ogg", "flv", "", NULL
};

static gint64
random_number(void)
{
	switch (g_test_rand_int_range(0, 8)) {
	case 0:
		return -1;
	case 1:
		return 0;
	case 2:
		return 1;
	case 3:
		return G_MAXINT64;
	case 4:
		return AXIS_PROBE_MAX_BITRATE;
	default:
		return g_test_rand_int_range(1, G_MAXINT32);
	}
}

static gdouble
random_double(void)
{
	switch (g_test_rand_int_range(0, 7)) {
	case 0:
		return -1.0;
	case 1:
		return NAN;
	case 2:
		return INFINITY;
	case 3:
		return 0.0;
	case 4:
		return 1e300;
	default:
		return g_test_rand_double_range(0.001, 240.0);
	}
}

static const gchar *
random_codec(void)
{
	guint n = G_N_ELEMENTS(codecs) - 1;
	gint pick = g_test_rand_int_range(0, (gint)n + 1);

	return pick == (gint)n ? NULL : codecs[pick];
}

static AxisMediaInfo *
random_info(void)
{
	AxisMediaInfo *info = axis_media_info_new();
	gint n = g_test_rand_int_range(0, 6);
	gint i;

	info->status = g_test_rand_int_range(0, 10) == 0 ? g_strdup("failed") : g_strdup("ok");
	info->probe_version = g_test_rand_int_range(0, 3);
	info->container = g_strdup(containers[g_test_rand_int_range(0, G_N_ELEMENTS(containers) - 1)]);
	info->duration_ms = random_number();
	info->bitrate = random_number();
	info->hdr_format = (AxisHdrFormat)g_test_rand_int_range(0, 7);
	info->hdr_uncertain = g_test_rand_int_range(0, 4) == 0;
	info->interlaced = g_test_rand_int_range(-1, 2);
	info->bits_per_pixel = random_double();
	info->streams_omitted = g_test_rand_int_range(0, 8) == 0;
	info->has_embedded_art = g_test_rand_bit();
	info->art_width = (gint)g_test_rand_int_range(-1, 5000);
	info->art_height = (gint)g_test_rand_int_range(-1, 5000);
	info->resolution_class = g_test_rand_bit() ? "2160p" : NULL;
	for (i = 0; i < n; i++) {
		AxisMediaStream *s = axis_test_add_stream(info, (AxisStreamKind)g_test_rand_int_range(0, 6), random_codec());

		s->bitrate = random_number();
		s->bitrate_estimated = g_test_rand_bit();
		s->duration_ms = random_number();
		s->width = (gint)g_test_rand_int_range(-1, 40000);
		s->height = (gint)g_test_rand_int_range(-1, 40000);
		s->frame_rate = random_double();
		s->real_frame_rate = random_double();
		s->hdr_format = (AxisHdrFormat)g_test_rand_int_range(0, 6);
		s->hdr_uncertain = g_test_rand_int_range(0, 4) == 0;
		s->attached_pic = g_test_rand_int_range(0, 4) == 0;
		s->channels = (gint)g_test_rand_int_range(-1, 300);
		s->sample_rate = (gint)g_test_rand_int_range(-1, 400000);
		s->bit_depth_audio = (gint)g_test_rand_int_range(-1, 40);
		s->lossless = g_test_rand_int_range(-1, 2);
		s->is_default = g_test_rand_bit();
		s->subtitle_kind = (AxisSubtitleKind)g_test_rand_int_range(0, 3);
		if (g_test_rand_bit())
			s->field_order = g_strdup(g_test_rand_bit() ? "progressive" : "tt");
		if (g_test_rand_bit())
			s->language = g_strdup(g_test_rand_bit() ? "eng" : "fre");
	}
	return info;
}

static void
assert_sane(const AxisEstimateResult *r)
{
	g_assert_cmpint(r->reason, >=, 0);
	g_assert_cmpint(r->reason, <, AXIS_ESTIMATE_N_REASONS);
	g_assert_cmpint(r->now, >=, 0);
	g_assert_cmpint(r->low, >=, 0);
	g_assert_cmpint(r->low, <=, r->mid);
	g_assert_cmpint(r->mid, <=, r->high);
	if (r->sized)
		g_assert_cmpint(r->high, <=, r->now);
	if (axis_estimate_result_converts(r)) {
		g_assert_true(r->sized);
		g_assert_cmpint(r->mid, <, r->now);
	}
}

static void
test_property(void)
{
	static const gchar *const english[] = { "eng", NULL };
	g_autoptr(AxisEstimateModel) model = uneven_model();
	g_autoptr(AxisEstimateTotal) total = axis_estimate_total_new();
	const AxisEstimateTarget *targets = axis_estimate_builtin_targets();
	guint eligible = 0;
	guint i;

	for (i = 0; i < 20000; i++) {
		AxisMediaInfo *info = random_info();
		AxisEstimateTarget t = targets[i % AXIS_ESTIMATE_N_TARGETS];
		AxisEstimateFile file;
		AxisEstimateResult result;
		gint64 size;

		if (i % 11 == 0)
			t.keep_audio_languages = english;
		if (i % 13 == 0)
			t.reduce_to_cd = TRUE;
		if (i % 4 == 0)
			size = random_number();
		else
			size = axis_test_natural_size(info);
		memset(&file, 0, sizeof(file));
		axis_test_estimate_index(&file.index, info, t.media == AXIS_TARGET_VIDEO ? "video" : "audio",
			(AxisMediaType)g_test_rand_int_range(0, AXIS_MEDIA_TYPE_COUNT + 1), MAX(size, 0));
		file.index.unit = g_test_rand_bit() ? "A/B" : NULL;
		file.index.unit_files = (guint)g_test_rand_int_range(0, 3);
		file.index.volume_usable = g_test_rand_bit();
		file.info = g_test_rand_int_range(0, 20) == 0 ? NULL : info;

		axis_estimate_eligibility(&file, &t, model, &result);
		assert_sane(&result);
		if (axis_estimate_result_converts(&result)) {
			eligible++;
			axis_estimate_total_add(total, model, &result);
		}
		axis_media_info_free(info);
	}
	{
		gint64 files;
		gint64 now;
		gint64 low;
		gint64 mid;
		gint64 high;

		axis_estimate_total_get(total, &files, &now, &low, &mid, &high);
		g_assert_cmpint(files, ==, eligible);
		g_assert_cmpint(low, >=, 0);
		g_assert_cmpint(low, <=, mid);
		g_assert_cmpint(mid, <=, high);
		g_assert_cmpint(high, <=, now);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/estimate/names", test_names);
	g_test_add_func("/estimate/each-reason", test_each_reason);
	g_test_add_func("/estimate/order-video", test_order_video);
	g_test_add_func("/estimate/order-audio", test_order_audio);
	g_test_add_func("/estimate/never-larger", test_never_larger);
	g_test_add_func("/estimate/monotonic-quality", test_monotonic_quality);
	g_test_add_func("/estimate/monotonic-resolution", test_monotonic_resolution);
	g_test_add_func("/estimate/monotonic-resolution-bitrate", test_monotonic_resolution_bitrate);
	g_test_add_func("/estimate/copied-and-dropped", test_copied_and_dropped);
	g_test_add_func("/estimate/albums", test_albums);
	g_test_add_func("/estimate/unit-key", test_unit_key);
	g_test_add_func("/estimate/quality-rule", test_quality_rule);
	g_test_add_func("/estimate/calibration", test_calibration);
	g_test_add_func("/estimate/calibration-audio", test_calibration_audio);
	g_test_add_func("/estimate/totals", test_totals);
	g_test_add_func("/estimate/property", test_property);

	return g_test_run();
}
