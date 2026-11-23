/*
 * axis-test-browser.c - test support for the Library pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-browser.h"

#include "library/axis-library-private.h"

void
axis_test_browser_generate(
	AxisTestLibrary *t,
	gint64           volume_id,
	gint64           root_id,
	guint            first,
	guint            last,
	guint            dirs
){
	g_autofree gchar *files = NULL;
	g_autofree gchar *probes = NULL;

	files = g_strdup_printf(
		"INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, state,"
		" first_seen_generation, last_seen_generation, changed_generation, changed, suspected_at,"
		" suspected_scan_id)"
		" SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'd' || (i %% %u) || '/s' || ((i / %u) %% 10) || '/f' || lpad(i::text, 8, '0') ||"
		"   CASE i %% 5 WHEN 0 THEN '.txt' WHEN 1 THEN '.flac' ELSE '.mkv' END,"
		" CASE i %% 5 WHEN 0 THEN 'text' WHEN 1 THEN 'audio' ELSE 'video' END,"
		" i * 1000::bigint, 1600000000 + (i * 7919) %% 100003, 0, 1600000000, 0, i::bigint,"
		" CASE WHEN i %% 7 = 0 THEN 2 ELSE 1 END, i %% 7 = 0, i %% 7 = 0, false,"
		" CASE WHEN i %% 97 = 0 THEN 'suspected' ELSE 'present' END, 1, 1, 1, false,"
		" CASE WHEN i %% 97 = 0 THEN now() END, CASE WHEN i %% 97 = 0 THEN 1 END"
		" FROM generate_series(%u::bigint, %u::bigint) i",
		root_id, volume_id, dirs, dirs, first, last);
	axis_test_db_exec(t->db, files);

	probes = g_strdup_printf(
		"INSERT INTO media_info (file_id, status, error, probe_version, file_size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, container, duration_ms, bitrate, video_streams, audio_streams,"
		" video_codec, width, height, resolution_class, hdr_format, hdr_uncertain, audio_codec)"
		" SELECT f.id, CASE WHEN f.inode %% 13 = 0 THEN 'failed' ELSE 'ok' END,"
		" CASE WHEN f.inode %% 13 = 0 THEN 'boom' END, 1, f.size, f.mtime_sec, 0, f.ctime_sec, 0, f.inode,"
		" CASE WHEN f.inode %% 13 = 0 THEN NULL WHEN f.kind = 'audio' THEN 'flac' ELSE 'matroska,webm' END,"
		" CASE WHEN f.inode %% 13 = 0 THEN NULL ELSE 60000 + (f.inode %% 7200) * 1000 END,"
		" CASE WHEN f.inode %% 13 = 0 THEN NULL ELSE 1000000 + (f.inode * 37) %% 8000000 END,"
		" CASE WHEN f.inode %% 13 = 0 OR f.kind = 'audio' THEN 0 ELSE 1 END,"
		" CASE WHEN f.inode %% 13 = 0 THEN 0 ELSE 1 END,"
		" CASE WHEN f.inode %% 13 = 0 OR f.kind = 'audio' THEN NULL WHEN f.inode %% 3 = 0 THEN 'hevc'"
		"   ELSE 'h264' END,"
		" CASE WHEN f.inode %% 13 = 0 OR f.kind = 'audio' THEN NULL ELSE 1920 END,"
		" CASE WHEN f.inode %% 13 = 0 OR f.kind = 'audio' THEN NULL ELSE 1080 END,"
		" CASE WHEN f.inode %% 13 = 0 OR f.kind = 'audio' THEN NULL"
		"   ELSE (ARRAY['sd', '720p', '1080p', '2160p'])[1 + (f.inode %% 4)::integer] END,"
		" CASE WHEN f.inode %% 13 = 0 OR f.kind = 'audio' THEN NULL WHEN f.inode %% 49 = 0 THEN 'dolby_vision'"
		"   WHEN f.inode %% 9 = 0 THEN 'hdr10' ELSE 'sdr' END,"
		" f.inode %% 13 <> 0 AND f.kind = 'video' AND f.inode %% 49 = 0,"
		" CASE WHEN f.inode %% 13 = 0 THEN NULL WHEN f.inode %% 2 = 0 THEN 'eac3' ELSE 'aac' END"
		" FROM library_file f WHERE f.root_id = %" G_GINT64_FORMAT " AND f.kind IN ('video', 'audio')"
		" AND f.inode %% 11 <> 0 AND f.inode BETWEEN %u AND %u",
		root_id, first, last);
	axis_test_db_exec(t->db, probes);
}

gdouble
axis_test_browser_update_dirs(
	AxisTestLibrary *t,
	gint64           root_id
){
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(t->pool, &error);
	gint64 started = g_get_monotonic_time();

	g_assert_no_error(error);
	g_assert_nonnull(conn);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "SET LOCAL statement_timeout = '600s'", 0, NULL, NULL, &error));
	g_assert_true(axis_library_index_update_dirs(conn, root_id, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_commit(conn, &error));
	axis_db_pool_release(t->pool, conn);

	return (gdouble)(g_get_monotonic_time() - started) / G_USEC_PER_SEC;
}
