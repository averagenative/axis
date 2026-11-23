/*
 * axis-test-analysis.c - test support for the Analysis reports
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-test-analysis.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-storage.h"

void
axis_test_scenario_put(
	const gchar *disk,
	const gchar *root_rel,
	const gchar *rel,
	gsize        size
){
	g_autofree gchar *path = g_build_filename(disk, root_rel, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *content = g_strnfill(size, 'x');

	axis_test_storage_mkdir(dir);
	axis_test_storage_write(path, content);
}

static void
link_file(
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

gint64
axis_test_scenario_file(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel
){
	g_autofree gchar *value = axis_test_library_q(t, "SELECT id FROM library_file WHERE root_id = %"
		G_GINT64_FORMAT " AND rel_path = '%s'", root, rel);

	if (value == NULL)
		g_error("there is no file '%s' in root %" G_GINT64_FORMAT, rel, root);
	return g_ascii_strtoll(value, NULL, 10);
}

/* The stamp columns of a probe row, taken from the file's row. */
#define STAMP "f.size, f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode"

static void
resolution_size(
	const gchar *res,
	gint        *width,
	gint        *height
){
	if (g_strcmp0(res, "2160p") == 0) {
		*width = 3840;
		*height = 2160;
	} else if (g_strcmp0(res, "1080p") == 0) {
		*width = 1920;
		*height = 1080;
	} else if (g_strcmp0(res, "720p") == 0) {
		*width = 1280;
		*height = 720;
	} else {
		*width = 720;
		*height = 480;
	}
}

void
axis_test_probe_video(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel,
	const gchar     *container,
	const gchar     *vcodec,
	const gchar     *res,
	const gchar     *hdr,
	const gchar     *bpp,
	gint64           duration_ms,
	const gchar     *acodec,
	gboolean         hdr_uncertain
){
	g_autofree gchar *info = NULL;
	g_autofree gchar *streams = NULL;
	gint width;
	gint height;

	resolution_size(res, &width, &height);
	info = g_strdup_printf(
		"INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, container, duration_ms, bitrate, stream_count, video_streams, audio_streams,"
		" video_codec, width, height, resolution_class, hdr_format, hdr_uncertain, bits_per_pixel, audio_codec,"
		" audio_lossless)"
		" SELECT f.id, 'ok', 1, " STAMP ", '%s', %" G_GINT64_FORMAT ", 1000000, 2, 1, 1, '%s', %d, %d, '%s', '%s',"
		" %s, %s, '%s', false FROM library_file f WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'",
		container, duration_ms, vcodec, width, height, res, hdr, hdr_uncertain ? "true" : "false", bpp, acodec,
		root, rel);
	axis_test_db_exec(t->db, info);

	streams = g_strdup_printf(
		"INSERT INTO media_stream (file_id, idx, kind, codec, width, height, hdr_format, hdr_uncertain, channels,"
		" sample_rate)"
		" SELECT f.id, 0, 'video', '%s', %d, %d, '%s', %s, NULL, NULL FROM library_file f"
		"  WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'"
		" UNION ALL SELECT f.id, 1, 'audio', '%s', NULL, NULL, NULL, false, 2, 48000 FROM library_file f"
		"  WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'",
		vcodec, width, height, hdr, hdr_uncertain ? "true" : "false", root, rel, acodec, root, rel);
	axis_test_db_exec(t->db, streams);
}

void
axis_test_probe_audio(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel,
	const gchar     *container,
	const gchar     *acodec,
	gboolean         lossless,
	gint64           duration_ms,
	const gchar     *artist,
	const gchar     *album,
	const gchar     *title
){
	g_autofree gchar *info = NULL;
	g_autofree gchar *streams = NULL;

	info = g_strdup_printf(
		"INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, container, duration_ms, bitrate, stream_count, video_streams, audio_streams,"
		" audio_codec, audio_lossless, tag_artist, tag_album, tag_title)"
		" SELECT f.id, 'ok', 1, " STAMP ", '%s', %" G_GINT64_FORMAT ", %d, 1, 0, 1, '%s', %s, '%s', '%s', '%s'"
		" FROM library_file f WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'",
		container, duration_ms, lossless ? 900000 : 128000, acodec, lossless ? "true" : "false", artist, album,
		title, root, rel);
	axis_test_db_exec(t->db, info);

	streams = g_strdup_printf(
		"INSERT INTO media_stream (file_id, idx, kind, codec, channels, sample_rate)"
		" SELECT f.id, 0, 'audio', '%s', 2, 44100 FROM library_file f"
		" WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'", acodec, root, rel);
	axis_test_db_exec(t->db, streams);
}

void
axis_test_probe_failed(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel
){
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO media_info (file_id, status, error, probe_version, file_size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode)"
		" SELECT f.id, 'failed', 'boom', 1, " STAMP " FROM library_file f"
		" WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'", root, rel);

	axis_test_db_exec(t->db, sql);
}

void
axis_test_scenario_clear(AxisTestScenario *s)
{
	g_free(s->disk_a);
	g_free(s->disk_b);
	memset(s, 0, sizeof(*s));
}

void
axis_test_scenario_build(
	AxisTestLibrary  *t,
	AxisTestScenario *s
){
	g_autoptr(GError) error = NULL;

	memset(s, 0, sizeof(*s));

	/* Two "disks" are directories of one filesystem, which storage
	 * refuses as two volumes (axis_test_library_add_volume_beside()); the
	 * first is away while the second is added. */
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
	s->down_a = axis_test_library_add_root(t, s->vol_a, s->disk_a, "Downloads", AXIS_MEDIA_DOWNLOADS);
	s->films_b = axis_test_library_add_root(t, s->vol_b, s->disk_b, "Films2", AXIS_MEDIA_MOVIES);
	s->shows_b = axis_test_library_add_root(t, s->vol_b, s->disk_b, "Shows2", AXIS_MEDIA_TV);
	s->music_b = axis_test_library_add_root(t, s->vol_b, s->disk_b, "Music2", AXIS_MEDIA_MUSIC);

	/* Volume A. */
	axis_test_scenario_put(s->disk_a, "Films", "Alder (1979)/Alder (1979) 2160p.mkv", 4000);
	axis_test_scenario_put(s->disk_a, "Films", "Alder (1979)/Alder (1979) 2160p.en.srt", 10);
	axis_test_scenario_put(s->disk_a, "Films", "Alder (1979)/Sample/alder-sample.mkv", 50);
	axis_test_scenario_put(s->disk_a, "Films", "Alder (1979)/Extras/making-of.mp4", 30);
	axis_test_scenario_put(s->disk_a, "Films", "Hearth (1995)/Hearth (1995).mkv", 1000);
	link_file(s->disk_a, "Films", "Hearth (1995)/Hearth (1995).mkv", "Films", "Hearth (1995)/Hearth (1995) copy.mkv");
	axis_test_scenario_put(s->disk_a, "Films", "Hearth (1995)/Other Movie.srt", 12);
	axis_test_scenario_put(s->disk_a, "Films", "Hearth (1986)/Hearth (1986).mkv", 1100);
	axis_test_scenario_put(s->disk_a, "Films", "Brannock (1985)/Brannock (1985).mkv", 777);
	axis_test_scenario_put(s->disk_a, "Films", "Dunmore (2021)/Dunmore (2021) 720p.mkv", 500);
	axis_test_scenario_put(s->disk_a, "Films", "Total Rewind (1990)/Total Rewind (1990).mkv", 600);
	axis_test_scenario_put(s->disk_a, "Films", "The Mazework (1999)/The Mazework (1999).mkv", 650);
	axis_test_scenario_put(s->disk_a, "Films", "The Mazework Reloaded (2003)/The Mazework Reloaded (2003).mkv", 660);
	axis_test_scenario_put(s->disk_a, "Films", "Unknown Film.mkv", 400);
	axis_test_scenario_put(s->disk_a, "Films", "Thumbs.db", 5);
	axis_test_scenario_put(s->disk_a, "Films", "Example.mkv", 40);
	axis_test_scenario_put(s->disk_a, "Films", "Sampler.mkv", 41);
	axis_test_scenario_put(s->disk_a, "Shows", "Show X (2010)/Season 1/Show X - S01E01.mkv", 300);
	axis_test_scenario_put(s->disk_a, "Shows", "Show X (2010)/Season 1/Show X - S01E02.mkv", 310);
	axis_test_scenario_put(s->disk_a, "Shows", "Show Y/Season 1/Show Y - S01E01.mkv", 250);
	axis_test_scenario_put(s->disk_a, "Music", "Artist One/Album One/01 Track.flac", 200);
	axis_test_scenario_put(s->disk_a, "Music", "Artist One/Album One/02 Track Two.flac", 210);
	axis_test_scenario_put(s->disk_a, "Music", "Artist Two/Album Two/01 Solo.flac", 220);
	link_file(s->disk_a, "Films", "Hearth (1995)/Hearth (1995).mkv", "Downloads", "Hearth.1995.1080p.mkv");

	/* Volume B. */
	axis_test_scenario_put(s->disk_b, "Films2", "Brannock (1985)/Brannock (1985).mkv", 777);
	axis_test_scenario_put(s->disk_b, "Films2", "Dunmore (2021)/Dunmore.2021.2160p.mkv", 3000);
	axis_test_scenario_put(s->disk_b, "Films2", "Total Rewind (2012)/Total Rewind (2012).mkv", 610);
	axis_test_scenario_put(s->disk_b, "Films2", "Unknown Film.mkv", 400);
	axis_test_scenario_put(s->disk_b, "Shows2", "Show X (2010)/Season 1/Show X - S01E03.mkv", 320);
	axis_test_scenario_put(s->disk_b, "Shows2", "Show X (2010)/Season 2/Show X - S02E01.mkv", 330);
	axis_test_scenario_put(s->disk_b, "Shows2", "Show X (2010)/Season 1/Show.X.2010.S01E01.720p.mkv", 150);
	axis_test_scenario_put(s->disk_b, "Music2", "Artist One/Album One/01 Track.mp3", 50);

	axis_scan_result_free(axis_test_library_scan_expect(t, s->films_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->shows_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->music_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->down_a, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->films_b, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->shows_b, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(t, s->music_b, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));

	/* What the probe found. */
	axis_test_probe_video(t, s->films_a, "Alder (1979)/Alder (1979) 2160p.mkv", "matroska,webm", "hevc", "2160p",
		"hdr10", "0.200", 7200000, "eac3", FALSE);
	axis_test_probe_failed(t, s->films_a, "Alder (1979)/Extras/making-of.mp4");
	axis_test_probe_video(t, s->films_a, "Hearth (1995)/Hearth (1995).mkv", "matroska,webm", "h264", "1080p", "sdr",
		"0.350", 6000000, "aac", FALSE);
	axis_test_probe_video(t, s->films_a, "Hearth (1995)/Hearth (1995) copy.mkv", "matroska,webm", "h264", "1080p", "sdr",
		"0.350", 6000000, "aac", FALSE);
	axis_test_probe_video(t, s->down_a, "Hearth.1995.1080p.mkv", "matroska,webm", "h264", "1080p", "sdr",
		"0.350", 6000000, "aac", FALSE);
	axis_test_probe_video(t, s->films_a, "Hearth (1986)/Hearth (1986).mkv", "matroska,webm", "h264", "1080p", "sdr",
		"0.100", 5400000, "aac", FALSE);
	axis_test_probe_video(t, s->films_a, "Brannock (1985)/Brannock (1985).mkv", "matroska,webm", "h264", "1080p", "sdr",
		"0.100", 8000000, "aac", FALSE);
	axis_test_probe_video(t, s->films_a, "Dunmore (2021)/Dunmore (2021) 720p.mkv", "matroska,webm", "h264", "720p", "sdr",
		"0.080", 9000000, "aac", FALSE);
	axis_test_probe_video(t, s->films_a, "Total Rewind (1990)/Total Rewind (1990).mkv", "matroska,webm", "h264", "sd",
		"sdr", "0.120", 6600000, "aac", TRUE);
	axis_test_probe_video(t, s->films_a, "The Mazework (1999)/The Mazework (1999).mkv", "matroska,webm", "h264", "1080p",
		"sdr", "0.120", 8160000, "aac", FALSE);
	axis_test_probe_video(t, s->films_a, "The Mazework Reloaded (2003)/The Mazework Reloaded (2003).mkv", "matroska,webm",
		"h264", "1080p", "sdr", "0.120", 8280000, "aac", FALSE);
	axis_test_probe_video(t, s->shows_a, "Show X (2010)/Season 1/Show X - S01E01.mkv", "matroska,webm", "h264",
		"1080p", "sdr", "0.050", 2700000, "aac", FALSE);
	axis_test_probe_video(t, s->shows_a, "Show X (2010)/Season 1/Show X - S01E02.mkv", "matroska,webm", "h264",
		"1080p", "sdr", "0.050", 2760000, "aac", FALSE);
	axis_test_probe_video(t, s->shows_a, "Show Y/Season 1/Show Y - S01E01.mkv", "matroska,webm", "h264", "1080p",
		"sdr", "0.050", 2640000, "aac", FALSE);
	axis_test_probe_audio(t, s->music_a, "Artist One/Album One/01 Track.flac", "flac", "flac", TRUE, 200000,
		"Artist One", "Album One", "Track");
	axis_test_probe_audio(t, s->music_a, "Artist One/Album One/02 Track Two.flac", "flac", "flac", TRUE, 210000,
		"Artist One", "Album One", "Track Two");
	axis_test_probe_audio(t, s->music_a, "Artist Two/Album Two/01 Solo.flac", "flac", "flac", TRUE, 220000,
		"Artist Two", "Album Two", "Solo");
	axis_test_probe_video(t, s->films_b, "Brannock (1985)/Brannock (1985).mkv", "matroska,webm", "h264", "1080p", "sdr",
		"0.100", 8000000, "aac", FALSE);
	axis_test_probe_video(t, s->films_b, "Dunmore (2021)/Dunmore.2021.2160p.mkv", "matroska,webm", "hevc", "2160p", "sdr",
		"0.100", 9000000, "aac", FALSE);
	axis_test_probe_video(t, s->films_b, "Total Rewind (2012)/Total Rewind (2012).mkv", "matroska,webm", "h264",
		"1080p", "sdr", "0.120", 6720000, "aac", FALSE);
	axis_test_probe_video(t, s->shows_b, "Show X (2010)/Season 1/Show X - S01E03.mkv", "matroska,webm", "h264",
		"1080p", "sdr", "0.050", 2820000, "aac", FALSE);
	axis_test_probe_video(t, s->shows_b, "Show X (2010)/Season 2/Show X - S02E01.mkv", "matroska,webm", "h264",
		"1080p", "sdr", "0.050", 2880000, "aac", FALSE);
	axis_test_probe_video(t, s->shows_b, "Show X (2010)/Season 1/Show.X.2010.S01E01.720p.mkv", "matroska,webm",
		"h264", "720p", "sdr", "0.040", 2700000, "aac", FALSE);
	axis_test_probe_audio(t, s->music_b, "Artist One/Album One/01 Track.mp3", "mp3", "mp3", FALSE, 200000,
		"Artist One", "Album One", "Track");

	/* The probe clears the flag when it stores a result. */
	axis_test_db_exec(t->db, "UPDATE library_file SET changed = false WHERE id IN (SELECT file_id FROM media_info)");
}
