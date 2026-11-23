/*
 * axis-test-savings.c - test support for the Savings estimates
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-test-savings.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-analysis.h"
#include "axis-test-storage.h"
#include "probe/axis-probe-model.h"

gsize
axis_test_savings_size(
	const AxisTestProbeStream *streams,
	gdouble               seconds
){
	gdouble bits = 0;
	guint i;

	for (i = 0; streams[i].kind != NULL; i++)
		bits += (gdouble)MAX(streams[i].bitrate, 0);
	return (gsize)(bits * seconds / 8.0 * 1.01) + 64;
}

static gchar *
number_or_null(gint64 value)
{
	return value > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, value) : g_strdup("NULL");
}

static gchar *
text_or_null(const gchar *value)
{
	return value != NULL ? g_strdup_printf("'%s'", value) : g_strdup("NULL");
}

static gchar *
double_or_null(gdouble value)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	if (value <= 0)
		return g_strdup("NULL");
	g_ascii_formatd(buffer, sizeof(buffer), "%.6f", value);
	return g_strdup(buffer);
}

void
axis_test_savings_probe(
	AxisTestLibrary      *t,
	gint64                file_id,
	const gchar          *container,
	gdouble               seconds,
	const AxisTestProbeStream *streams,
	const gchar          *interlaced
){
	const AxisTestProbeStream *video = NULL;
	const AxisTestProbeStream *audio = NULL;
	g_autofree gchar *info = NULL;
	g_autofree gchar *v_codec = NULL;
	g_autofree gchar *v_width = NULL;
	g_autofree gchar *v_height = NULL;
	g_autofree gchar *v_res = NULL;
	g_autofree gchar *v_depth = NULL;
	g_autofree gchar *v_fps = NULL;
	g_autofree gchar *v_hdr = NULL;
	g_autofree gchar *v_bpp = NULL;
	g_autofree gchar *a_codec = NULL;
	g_autofree gchar *a_channels = NULL;
	g_autofree gchar *a_rate = NULL;
	g_autofree gchar *a_bits = NULL;
	g_autofree gchar *a_lossless = NULL;
	gint64 total = 0;
	guint n_video = 0;
	guint n_audio = 0;
	guint n_sub = 0;
	gboolean uncertain = FALSE;
	guint i;

	for (i = 0; streams[i].kind != NULL; i++) {
		total += MAX(streams[i].bitrate, 0);
		if (strcmp(streams[i].kind, "video") == 0) {
			n_video++;
			if (video == NULL)
				video = &streams[i];
			if (streams[i].hdr_uncertain || streams[i].hdr == NULL)
				uncertain = TRUE;
		} else if (strcmp(streams[i].kind, "audio") == 0) {
			n_audio++;
			if (audio == NULL)
				audio = &streams[i];
		} else if (strcmp(streams[i].kind, "subtitle") == 0) {
			n_sub++;
		}
	}
	v_codec = text_or_null(video != NULL ? video->codec : NULL);
	v_width = number_or_null(video != NULL ? video->width : 0);
	v_height = number_or_null(video != NULL ? video->height : 0);
	v_res = text_or_null(video != NULL ? axis_probe_resolution_class(video->width, video->height) : NULL);
	v_depth = number_or_null(video != NULL ? (g_strcmp0(video->hdr, "sdr") == 0 ? 8 : 10) : 0);
	v_fps = double_or_null(video != NULL ? video->fps : 0);
	v_hdr = text_or_null(video != NULL ? video->hdr : NULL);
	v_bpp = double_or_null(video != NULL && video->width > 0 ? (gdouble)video->bitrate /
		((gdouble)video->width * video->height * video->fps) : 0);
	a_codec = text_or_null(audio != NULL ? audio->codec : NULL);
	a_channels = number_or_null(audio != NULL ? audio->channels : 0);
	a_rate = number_or_null(audio != NULL ? audio->sample_rate : 0);
	a_bits = number_or_null(audio != NULL ? audio->bits : 0);
	a_lossless = g_strdup(audio != NULL && audio->lossless != NULL ? audio->lossless : "NULL");

	info = g_strdup_printf(
		"INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, container, duration_ms, bitrate, stream_count, video_streams, audio_streams,"
		" subtitle_streams, video_codec, width, height, resolution_class, bit_depth, frame_rate, hdr_format,"
		" hdr_uncertain, bits_per_pixel, interlaced, audio_codec, audio_channels, audio_sample_rate, audio_bit_depth,"
		" audio_lossless)"
		" SELECT f.id, 'ok', %d, f.size, f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, '%s',"
		" %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %u, %u, %u, %u, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s,"
		" %s, %s, %s, %s FROM library_file f WHERE f.id = %" G_GINT64_FORMAT,
		AXIS_PROBE_VERSION, container, (gint64)(seconds * 1000), total, n_video + n_audio + n_sub, n_video, n_audio,
		n_sub, v_codec, v_width, v_height, v_res, v_depth, v_fps, v_hdr,
		video != NULL && uncertain ? "true" : "false", v_bpp, interlaced != NULL ? interlaced : "NULL",
		a_codec, a_channels, a_rate, a_bits, a_lossless, file_id);
	axis_test_db_exec(t->db, info);

	for (i = 0; streams[i].kind != NULL; i++) {
		const AxisTestProbeStream *s = &streams[i];
		gboolean is_video = strcmp(s->kind, "video") == 0;
		gboolean is_sub = strcmp(s->kind, "subtitle") == 0;
		g_autofree gchar *codec = text_or_null(s->codec);
		g_autofree gchar *rate = number_or_null(s->bitrate);
		g_autofree gchar *width = number_or_null(is_video ? s->width : 0);
		g_autofree gchar *height = number_or_null(is_video ? s->height : 0);
		g_autofree gchar *fps = double_or_null(is_video ? s->fps : 0);
		g_autofree gchar *hdr = text_or_null(is_video ? s->hdr : NULL);
		g_autofree gchar *field = text_or_null(is_video ? s->field_order : NULL);
		g_autofree gchar *channels = number_or_null(is_video ? 0 : s->channels);
		g_autofree gchar *sample_rate = number_or_null(is_video ? 0 : s->sample_rate);
		g_autofree gchar *bits = number_or_null(is_video ? 0 : s->bits);
		g_autofree gchar *depth = number_or_null(is_video ? (g_strcmp0(s->hdr, "sdr") == 0 ? 8 : 10) : 0);
		g_autofree gchar *language = text_or_null(s->language);
		g_autofree gchar *subkind = text_or_null(is_sub ? (axis_probe_subtitle_kind(s->codec) == AXIS_SUBTITLE_TEXT ?
			"text" : axis_probe_subtitle_kind(s->codec) == AXIS_SUBTITLE_IMAGE ? "image" : NULL) : NULL);
		g_autofree gchar *dv = g_strcmp0(s->hdr, "dolby_vision") == 0 ? g_strdup("8") : g_strdup("NULL");
		g_autofree gchar *sql = g_strdup_printf(
			"INSERT INTO media_stream (file_id, idx, kind, codec, bitrate, width, height, bit_depth, frame_rate,"
			" field_order, hdr_format, hdr_uncertain, dv_profile, channels, sample_rate, bit_depth_audio, lossless,"
			" language, is_default, subtitle_kind, duration_ms)"
			" VALUES (%" G_GINT64_FORMAT ", %u, '%s', %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s,"
			" %s, %s, %" G_GINT64_FORMAT ")",
			file_id, i, s->kind, codec, rate, width, height, depth, fps, field, hdr,
			s->hdr_uncertain ? "true" : "false", dv, channels, sample_rate, bits,
			!is_video && s->lossless != NULL ? s->lossless : "NULL", language, i <= 1 ? "true" : "false", subkind,
			(gint64)(seconds * 1000));

		axis_test_db_exec(t->db, sql);
	}
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE library_file SET changed = false WHERE id = %" G_GINT64_FORMAT,
			file_id);

		axis_test_db_exec(t->db, sql);
	}
}

static void
link_into(
	const gchar *disk,
	const gchar *from_root,
	const gchar *from_rel,
	const gchar *to_root,
	const gchar *to_rel
){
	g_autofree gchar *from = g_build_filename(disk, from_root, from_rel, NULL);
	g_autofree gchar *to = g_build_filename(disk, to_root, to_rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(to);

	axis_test_storage_mkdir(dir);
	if (link(from, to) != 0)
		g_error("cannot link %s to %s: %s", from, to, g_strerror(errno));
}

/* A file at the size its streams imply. */
static void
put(
	const gchar          *disk,
	const gchar          *root,
	const gchar          *rel,
	const AxisTestProbeStream *streams
){
	axis_test_scenario_put(disk, root, rel, axis_test_savings_size(streams, 1.0));
}

#define V(codec, bitrate, width, height, hdr) \
	{ "video", codec, bitrate, width, height, 24.0, hdr, FALSE, 0, 0, 0, NULL, "progressive", NULL }
#define A(codec, bitrate, channels, lossless) \
	{ "audio", codec, bitrate, 0, 0, 0, NULL, FALSE, channels, 48000, 0, lossless, NULL, "eng" }
#define END { NULL, NULL, 0, 0, 0, 0, NULL, FALSE, 0, 0, 0, NULL, NULL, NULL }

static const AxisTestProbeStream big[] = { V("h264", 8000000, 1920, 1080, "sdr"), A("eac3", 640000, 6, "false"), END };
static const AxisTestProbeStream remux[] = {
	V("h264", 25000000, 1920, 1080, "sdr"), A("truehd", 3000000, 8, "true"), END
};
static const AxisTestProbeStream small[] = { V("h264", 600000, 1280, 720, "sdr"), A("aac", 128000, 2, "false"), END };
static const AxisTestProbeStream hevc[] = { V("hevc", 2000000, 1920, 1080, "sdr"), A("aac", 128000, 2, "false"), END };
static const AxisTestProbeStream dv[] = {
	V("hevc", 20000000, 3840, 2160, "dolby_vision"), A("eac3", 640000, 6, "false"), END
};
static const AxisTestProbeStream uncertain[] = {
	{ "video", "h264", 8000000, 1920, 1080, 24.0, "sdr", TRUE, 0, 0, 0, NULL, "progressive", NULL },
	A("aac", 128000, 2, "false"), END
};
static const AxisTestProbeStream hdr10[] = {
	V("hevc", 40000000, 3840, 2160, "hdr10"), A("eac3", 640000, 6, "false"), END
};
static const AxisTestProbeStream episode[] = { V("h264", 4000000, 1280, 720, "sdr"), A("aac", 128000, 2, "false"), END };
static const AxisTestProbeStream interlaced[] = {
	{ "video", "h264", 4000000, 1280, 720, 24.0, "sdr", FALSE, 0, 0, 0, NULL, "tt", NULL },
	A("aac", 128000, 2, "false"), END
};
static const AxisTestProbeStream flac[] = {
	{ "audio", "flac", 900000, 0, 0, 0, NULL, FALSE, 2, 44100, 16, "true", NULL, NULL }, END
};
static const AxisTestProbeStream mp3_high[] = {
	{ "audio", "mp3", 320000, 0, 0, 0, NULL, FALSE, 2, 44100, 0, "false", NULL, NULL }, END
};
static const AxisTestProbeStream wav[] = {
	{ "audio", "pcm_s16le", 1411200, 0, 0, 0, NULL, FALSE, 2, 44100, 16, "true", NULL, NULL }, END
};
static const AxisTestProbeStream book[] = {
	{ "audio", "mp3", 128000, 0, 0, 0, NULL, FALSE, 2, 44100, 0, "false", NULL, NULL }, END
};
static const AxisTestProbeStream other[] = { V("h264", 10000000, 1920, 1080, "sdr"), A("aac", 192000, 2, "false"), END };

static void
probe(
	AxisTestLibrary      *t,
	gint64                root,
	const gchar          *rel,
	const gchar          *container,
	const AxisTestProbeStream *streams,
	const gchar          *interlaced_text
){
	axis_test_savings_probe(t, axis_test_scenario_file(t, root, rel), container, 1.0, streams, interlaced_text);
}

void
axis_test_savings_build(
	AxisTestLibrary *t,
	AxisTestSavings *s
){
	g_autoptr(GError) error = NULL;

	memset(s, 0, sizeof(*s));
	s->disk_a = axis_test_library_disk(t, "disk1");
	s->vol_a = axis_test_library_add_volume(t, s->disk_a, "Disk A");
	axis_test_library_unplug(t, s->disk_a, "away");
	g_assert_true(axis_storage_check_volume(t->storage, s->vol_a, NULL, &error));
	s->disk_b = axis_test_library_disk(t, "disk2");
	s->vol_b = axis_test_library_add_volume_beside(t, s->disk_b, "Disk B");
	axis_test_library_plug(t, s->disk_a, "away");
	g_assert_true(axis_storage_check_volume(t->storage, s->vol_a, NULL, &error));
	g_assert_no_error(error);

	s->films_a = axis_test_library_add_root(t, s->vol_a, s->disk_a, "Films", AXIS_MEDIA_MOVIES);
	s->shows_a = axis_test_library_add_root(t, s->vol_a, s->disk_a, "Shows", AXIS_MEDIA_TV);
	s->music_a = axis_test_library_add_root(t, s->vol_a, s->disk_a, "Music", AXIS_MEDIA_MUSIC);
	s->books_a = axis_test_library_add_root(t, s->vol_a, s->disk_a, "Books", AXIS_MEDIA_AUDIOBOOKS);
	s->down_a = axis_test_library_add_root(t, s->vol_a, s->disk_a, "Downloads", AXIS_MEDIA_DOWNLOADS);
	s->films_b = axis_test_library_add_root(t, s->vol_b, s->disk_b, "Films2", AXIS_MEDIA_MOVIES);

	put(s->disk_a, "Films", "Big (2001)/Big (2001).mkv", big);
	put(s->disk_a, "Films", "Remux (2002)/Remux (2002).mkv", remux);
	put(s->disk_a, "Films", "Small (2003)/Small (2003).mkv", small);
	put(s->disk_a, "Films", "HEVC (2004)/HEVC (2004).mkv", hevc);
	put(s->disk_a, "Films", "DV (2005)/DV (2005).mkv", dv);
	put(s->disk_a, "Films", "Uncertain (2006)/Uncertain (2006).mkv", uncertain);
	axis_test_scenario_put(s->disk_a, "Films", "Unprobed.mkv", 2000);
	axis_test_scenario_put(s->disk_a, "Films", "Broken.mkv", 3000);
	put(s->disk_a, "Films", "Linked (2007)/Linked (2007).mkv", big);
	link_into(s->disk_a, "Films", "Linked (2007)/Linked (2007).mkv", "Films", "Linked (2007)/Linked copy.mkv");
	link_into(s->disk_a, "Films", "Linked (2007)/Linked (2007).mkv", "Downloads", "Linked.mkv");
	put(s->disk_a, "Films", "HDR10 (2008)/HDR10 (2008).mkv", hdr10);
	put(s->disk_a, "Shows", "Show (2010)/Season 1/Show - S01E01.mkv", episode);
	put(s->disk_a, "Shows", "Show (2010)/Season 1/Show - S01E02.mkv", interlaced);
	put(s->disk_a, "Music", "Band/Album/01 One.flac", flac);
	put(s->disk_a, "Music", "Band/Album/02 Two.flac", flac);
	put(s->disk_a, "Music", "Band/Mixed/01 A.flac", flac);
	put(s->disk_a, "Music", "Band/Mixed/02 B.mp3", mp3_high);
	put(s->disk_a, "Music", "Loose.wav", wav);
	put(s->disk_a, "Books", "Author/Book/Part 1.mp3", book);
	put(s->disk_a, "Books", "Author/Book/Part 2.mp3", book);
	put(s->disk_b, "Films2", "Other (2011)/Other (2011).mkv", other);

	axis_scan_result_free(axis_test_library_scan_expect(t, s->films_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->shows_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->music_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->books_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->down_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->films_b, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));

	probe(t, s->films_a, "Big (2001)/Big (2001).mkv", "matroska,webm", big, "false");
	probe(t, s->films_a, "Remux (2002)/Remux (2002).mkv", "matroska,webm", remux, "false");
	probe(t, s->films_a, "Small (2003)/Small (2003).mkv", "matroska,webm", small, "false");
	probe(t, s->films_a, "HEVC (2004)/HEVC (2004).mkv", "matroska,webm", hevc, "false");
	probe(t, s->films_a, "DV (2005)/DV (2005).mkv", "matroska,webm", dv, "false");
	probe(t, s->films_a, "Uncertain (2006)/Uncertain (2006).mkv", "matroska,webm", uncertain, "false");
	axis_test_probe_failed(t, s->films_a, "Broken.mkv");
	probe(t, s->films_a, "Linked (2007)/Linked (2007).mkv", "matroska,webm", big, "false");
	probe(t, s->films_a, "Linked (2007)/Linked copy.mkv", "matroska,webm", big, "false");
	probe(t, s->down_a, "Linked.mkv", "matroska,webm", big, "false");
	probe(t, s->films_a, "HDR10 (2008)/HDR10 (2008).mkv", "matroska,webm", hdr10, "false");
	probe(t, s->shows_a, "Show (2010)/Season 1/Show - S01E01.mkv", "matroska,webm", episode, "false");
	probe(t, s->shows_a, "Show (2010)/Season 1/Show - S01E02.mkv", "matroska,webm", interlaced, NULL);
	probe(t, s->music_a, "Band/Album/01 One.flac", "flac", flac, NULL);
	probe(t, s->music_a, "Band/Album/02 Two.flac", "flac", flac, NULL);
	probe(t, s->music_a, "Band/Mixed/01 A.flac", "flac", flac, NULL);
	probe(t, s->music_a, "Band/Mixed/02 B.mp3", "mp3", mp3_high, NULL);
	probe(t, s->music_a, "Loose.wav", "wav", wav, NULL);
	probe(t, s->books_a, "Author/Book/Part 1.mp3", "mp3", book, NULL);
	probe(t, s->books_a, "Author/Book/Part 2.mp3", "mp3", book, NULL);
	probe(t, s->films_b, "Other (2011)/Other (2011).mkv", "matroska,webm", other, "false");
	axis_test_db_exec(t->db, "UPDATE library_file SET changed = false WHERE id IN (SELECT file_id FROM media_info)");
}

void
axis_test_savings_clear(AxisTestSavings *s)
{
	g_free(s->disk_a);
	g_free(s->disk_b);
	memset(s, 0, sizeof(*s));
}
