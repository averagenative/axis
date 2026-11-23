/*
 * test-probe-job.c - probing real files through the whole path: the index,
 * the volume guard, ffprobe, the model, the database; hostile names and
 * files; hung and runaway tools; changing files; volumes going away
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The media are made with ffmpeg's test sources (a second or two each,
 * once per run, in a cache directory), so ffmpeg and ffprobe must be
 * installed; without them these tests skip.  Tools that misbehave are
 * tests/fixtures/probe/fake-tool.sh, set as the probe's command.  Dolby
 * Vision and HDR10+ cannot be made, so they are not here: see
 * test-probe-map.c and the constructed fixtures.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-media.h"
#include "axis-test-storage.h"
#include "axis-test-case.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "probe/axis-probe-private.h"
#include "probe/axis-probe-run.h"
#include "storage/axis-storage-error.h"

#define SCRIPT "tests/fixtures/probe/fake-tool.sh"

/* ── The media, made once ───────────────────────────────────────── */

#define clips_ready axis_test_media_available
#define clip_path   axis_test_media_path

/* ── The fixture ────────────────────────────────────────────────── */

typedef enum
{
	HOOK_NONE,
	HOOK_APPEND,	/* the file grows */
	HOOK_REPLACE,	/* another file takes the name */
	HOOK_UNPLUG	/* the disk goes away */
} HookAction;

typedef struct
{
	AxisTestLibrary *t;
	AxisProbe       *probe;
	gchar           *disk;
	gchar           *films;
	gint64           volume;
	gint64           root;
	gchar           *disk2;
	gchar           *tracks;
	gint64           volume2;
	gint64           root2;
	AxisProbeTuning  tuning;
	gchar           *tool[4];
	HookAction       action;
	gchar           *hook_path;
	gint             hook_calls;
	gboolean         unplugged;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)
#define SKIP_WITHOUT_MEDIA(fx) \
	do { \
		if ((fx)->t == NULL) \
			return; \
		if (!clips_ready()) { \
			g_test_skip("ffmpeg and ffprobe are not installed"); \
			return; \
		} \
	} while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");

	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Films", AXIS_MEDIA_MOVIES);
	fx->films = g_build_filename(fx->disk, "Films", NULL);
	fx->tuning.hook_data = fx;
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t != NULL && fx->unplugged)
		axis_test_library_plug(fx->t, fx->disk, "away");
	if (fx->probe != NULL)
		axis_probe_unref(fx->probe);
	axis_test_library_free(fx->t);
	g_free(fx->disk);
	g_free(fx->films);
	g_free(fx->disk2);
	g_free(fx->tracks);
	g_free(fx->hook_path);
	g_free(fx->tool[0]);
	g_free(fx->tool[1]);
	g_free(fx->tool[2]);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void
tune(Fixture *fx)
{
	fx->tuning.hook_data = fx;
	axis_probe_set_tuning(fx->probe, &fx->tuning);
}

/* The fake tool in @mode becomes the probe's command. */
static void
use_fake(
	Fixture     *fx,
	const gchar *mode
){
	g_free(fx->tool[0]);
	g_free(fx->tool[1]);
	g_free(fx->tool[2]);
	fx->tool[0] = g_strdup("/bin/sh");
	fx->tool[1] = g_canonicalize_filename(SCRIPT, NULL);
	fx->tool[2] = g_strdup(mode);
	fx->tool[3] = NULL;
	fx->tuning.tool_command = (const gchar *const *)fx->tool;
	tune(fx);
}

static void
use_real(Fixture *fx)
{
	fx->tuning.tool_command = NULL;
	tune(fx);
}

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->t->db, sql);
}

static gint64
n(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gint64
n(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *value = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	value = axis_test_db_query(fx->t->db, sql);
	g_assert_nonnull(value);
	return g_ascii_strtoll(value, NULL, 10);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL",
			value != NULL ? value : "NULL");
}

/* Writes @length bytes as the file @name (any bytes but '/' and NUL). */
static void
put_bytes(
	const gchar *dir,
	const gchar *name,
	const gchar *bytes,
	gsize        length
){
	gint dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	gint fd;

	g_assert_cmpint(dfd, >=, 0);
	fd = openat(dfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0)
		g_error("cannot create a file in %s: %s", dir, g_strerror(errno));
	g_assert_cmpint(write(fd, bytes, length), ==, (gssize)length);
	close(fd);
	close(dfd);
}

/* Copies a clip into the root as @rel (made with its folders). */
static void
install(
	Fixture     *fx,
	const gchar *clip,
	const gchar *rel
){
	g_autofree gchar *source = clip_path(clip);
	g_autofree gchar *path = g_build_filename(fx->films, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *name = g_path_get_basename(path);
	g_autofree gchar *bytes = NULL;
	gsize length = 0;

	g_assert_true(g_file_get_contents(source, &bytes, &length, NULL));
	axis_test_storage_mkdir(dir);
	put_bytes(dir, name, bytes, length);
}

static void
install_text(
	Fixture     *fx,
	const gchar *rel,
	const gchar *text
){
	g_autofree gchar *path = g_build_filename(fx->films, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *name = g_path_get_basename(path);

	axis_test_storage_mkdir(dir);
	put_bytes(dir, name, text, strlen(text));
}

static void
scan(Fixture *fx)
{
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
}

static gint64
file_id(
	Fixture     *fx,
	const gchar *rel
){
	return n(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'", fx->root,
		rel);
}

static AxisProbeOutcome
probe(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;
	AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;

	if (!axis_probe_file(fx->probe, id, NULL, &outcome, &error))
		g_error("probing %" G_GINT64_FORMAT " failed: %s", id, error->message);
	return outcome;
}

static void
probe_fails(
	Fixture *fx,
	gint64   id,
	GQuark   domain,
	gint     code
){
	g_autoptr(GError) error = NULL;
	AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;

	g_assert_false(axis_probe_file(fx->probe, id, NULL, &outcome, &error));
	g_assert_error(error, domain, code);
}

/* Probes every video and audio file of the index, once each. */
static void
probe_all(Fixture *fx)
{
	g_autofree gchar *ids = q(fx, "SELECT coalesce(string_agg(id::text, ' ' ORDER BY id), '') FROM library_file"
		" WHERE kind IN ('video', 'audio')");
	g_auto(GStrv) list = g_strsplit(ids, " ", -1);
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		if (list[i][0] != '\0')
			probe(fx, g_ascii_strtoll(list[i], NULL, 10));
	}
}

static guint64
runs(Fixture *fx)
{
	return axis_probe_get_runs(fx->probe);
}

static guint
enqueue_due(
	Fixture  *fx,
	gboolean  stale
){
	g_autoptr(GError) error = NULL;
	guint enqueued = 0;

	g_assert_true(axis_probe_enqueue_due(fx->probe, stale, &enqueued, &error));
	g_assert_no_error(error);
	return enqueued;
}

/* A second volume with a root of its own.  Both "disks" are directories of
 * one filesystem, which storage only lets be two volumes while the first is
 * away. */
static void
add_second_volume(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	axis_test_library_unplug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	fx->disk2 = axis_test_library_disk(fx->t, "disk2");
	fx->volume2 = axis_test_library_add_volume_beside(fx->t, fx->disk2, "Disk two");
	axis_test_library_plug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	fx->root2 = axis_test_library_add_root(fx->t, fx->volume2, fx->disk2, "Music", AXIS_MEDIA_MUSIC);
	fx->tracks = g_build_filename(fx->disk2, "Music", NULL);
}

static void
hook_after_run(
	gint64   file_id_,
	gpointer data
){
	Fixture *fx = data;
	g_autofree gchar *path = NULL;

	fx->hook_calls++;
	if (fx->hook_path != NULL)
		path = g_build_filename(fx->films, fx->hook_path, NULL);

	switch (fx->action) {
	case HOOK_APPEND: {
		gint fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);

		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(write(fd, "more data\n", 10), ==, 10);
		close(fd);
		break;
	}
	case HOOK_REPLACE: {
		g_autofree gchar *other = g_strdup_printf("%s.new", path);
		g_autofree gchar *source = clip_path("tone.mp3");
		g_autofree gchar *bytes = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(source, &bytes, &length, NULL));
		g_assert_true(g_file_set_contents(other, bytes, (gssize)length, NULL));
		g_assert_cmpint(g_rename(other, path), ==, 0);
		break;
	}
	case HOOK_UNPLUG:
		axis_test_library_unplug(fx->t, fx->disk, "away");
		fx->unplugged = TRUE;
		break;
	case HOOK_NONE:
	default:
		break;
	}
}

/* ── Real files ─────────────────────────────────────────────────── */

/* Spec: Video file, Missing bitrate is estimated (real files). */
static void
test_video_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	struct stat st;
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "multi.mkv", "Movie (2020)/Movie.mkv");
	scan(fx);
	id = file_id(fx, "Movie (2020)/Movie.mkv");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND changed", id), ==, 1);

	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_STORED);
	g_assert_cmpuint(runs(fx), ==, 1);	/* 8-bit SDR needs no frame */

	assert_q(fx, "SELECT status || '|' || container || '|' || video_streams || '|' || audio_streams || '|' ||"
	         " subtitle_streams || '|' || stream_count FROM media_info",
	         "ok|matroska,webm|1|2|2|5");
	assert_q(fx, "SELECT video_codec || '|' || width || 'x' || height || '|' || resolution_class || '|' ||"
	         " bit_depth || '|' || hdr_format || '|' || hdr_uncertain || '|' || probe_version FROM media_info",
	         "h264|160x120|sd|8|sdr|false|" G_STRINGIFY(AXIS_PROBE_VERSION));
	assert_q(fx, "SELECT string_agg(codec || ':' || coalesce(language, '-') || ':' || coalesce(channel_layout, '-'),"
	         " ' ' ORDER BY idx) FROM media_stream WHERE kind = 'audio'", "aac:eng:mono aac:fra:mono");
	assert_q(fx, "SELECT string_agg(codec || ':' || subtitle_kind || ':' || language, ' ' ORDER BY idx)"
	         " FROM media_stream WHERE kind = 'subtitle'", "subrip:text:eng subrip:text:fra");
	/* Matroska states no stream bitrate: they are estimates. */
	assert_q(fx, "SELECT bool_and(bitrate_estimated AND bitrate > 0)::text FROM media_stream"
	         " WHERE kind IN ('video', 'audio')", "true");
	assert_q(fx, "SELECT (bits_per_pixel > 0 AND bits_per_pixel_estimated)::text FROM media_info", "true");
	assert_q(fx, "SELECT (tool_version ~ '^[0-9]')::text FROM media_info", "true");

	/* The row is for the file as it is. */
	path = g_build_filename(fx->films, "Movie (2020)/Movie.mkv", NULL);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	assert_q(fx, "SELECT (file_size = (SELECT size FROM library_file) AND inode = (SELECT inode FROM library_file))::text"
	         " FROM media_info", "true");
	g_assert_cmpint(n(fx, "SELECT file_size FROM media_info"), ==, (gint64)st.st_size);
	/* And the index knows it has been looked at. */
	assert_q(fx, "SELECT changed::text FROM library_file", "false");
}

/* Spec: Audio file. */
static void
test_audio_files(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "tone1.flac", "Album/01 One.flac");
	install(fx, "tone2.flac", "Album/02 Two.flac");
	install(fx, "tone2.flac", "Other/02 Two.flac");
	install(fx, "tone.mp3", "Single.mp3");
	scan(fx);
	probe_all(fx);

	assert_q(fx, "SELECT audio_codec || '|' || audio_sample_rate || '|' || audio_bit_depth || '|' ||"
	         " audio_lossless || '|' || audio_channels FROM media_info WHERE file_id = "
	         "(SELECT id FROM library_file WHERE rel_path = 'Album/01 One.flac')", "flac|44100|16|true|1");
	assert_q(fx, "SELECT tag_title || '|' || tag_artist || '|' || tag_album || '|' || has_track || '|' || has_disc"
	         " || '|' || has_replaygain || '|' || has_embedded_art || '|' || art_width || 'x' || art_height"
	         " FROM media_info WHERE file_id = (SELECT id FROM library_file WHERE rel_path = 'Album/01 One.flac')",
	         "Track One|The Band|The Album|true|true|true|true|64x64");
	assert_q(fx, "SELECT (audio_lossless = false)::text || '|' || audio_codec FROM media_info WHERE file_id = "
	         "(SELECT id FROM library_file WHERE rel_path = 'Single.mp3')", "true|mp3");
	assert_q(fx, "SELECT (video_streams = 0 AND hdr_format IS NULL AND NOT hdr_uncertain)::text FROM media_info"
	         " WHERE file_id = (SELECT id FROM library_file WHERE rel_path = 'Single.mp3')", "true");

	/* The tracks of one album in one folder share a key; the same album
	 * name in another folder does not. */
	assert_q(fx, "SELECT (a.album_key = b.album_key AND a.album_key ~ '^[0-9a-f]{64}$')::text"
	         " FROM media_info a, media_info b"
	         " WHERE a.file_id = (SELECT id FROM library_file WHERE rel_path = 'Album/01 One.flac')"
	         "   AND b.file_id = (SELECT id FROM library_file WHERE rel_path = 'Album/02 Two.flac')", "true");
	assert_q(fx, "SELECT (a.album_key <> c.album_key)::text FROM media_info a, media_info c"
	         " WHERE a.file_id = (SELECT id FROM library_file WHERE rel_path = 'Album/01 One.flac')"
	         "   AND c.file_id = (SELECT id FROM library_file WHERE rel_path = 'Other/02 Two.flac')", "true");
	assert_q(fx, "SELECT (album_key IS NULL)::text FROM media_info WHERE file_id ="
	         " (SELECT id FROM library_file WHERE rel_path = 'Single.mp3')", "true");
}

static void
test_hdr_real_files(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "hdr10.mkv", "hdr10.mkv");
	install(fx, "pq-bare.mkv", "pq-bare.mkv");
	install(fx, "unsig10.mkv", "unsig10.mkv");
	install(fx, "sdr.mkv", "sdr.mkv");
	scan(fx);
	probe_all(fx);

	/* Spec: HDR10 (PQ with mastering display metadata, 10 bits). */
	assert_q(fx, "SELECT hdr_format || '|' || hdr_uncertain || '|' || bit_depth || '|' || video_codec"
	         " FROM media_info WHERE file_id = (SELECT id FROM library_file WHERE rel_path = 'hdr10.mkv')",
	         "hdr10|false|10|hevc");
	assert_q(fx, "SELECT has_mastering_display::text || has_content_light::text FROM media_stream WHERE file_id ="
	         " (SELECT id FROM library_file WHERE rel_path = 'hdr10.mkv') AND kind = 'video'", "truetrue");
	/* Spec: Values stored (the clip's x265 master-display and max-cll, in
	 * the bitstream's units), and read back as they were stored. */
	assert_q(fx, "SELECT concat_ws(',', md_red_x, md_red_y, md_green_x, md_green_y, md_blue_x, md_blue_y, md_white_x,"
	         " md_white_y, md_min_luminance, md_max_luminance, cll_max_content, cll_max_average) FROM media_stream"
	         " WHERE file_id = (SELECT id FROM library_file WHERE rel_path = 'hdr10.mkv') AND kind = 'video'",
	         "34000,16000,13250,34500,7500,3000,15635,16450,1,10000000,1000,400");
	assert_q(fx, "SELECT (md_max_luminance IS NULL AND cll_max_content IS NULL)::text FROM media_stream WHERE file_id ="
	         " (SELECT id FROM library_file WHERE rel_path = 'pq-bare.mkv') AND kind = 'video'", "true");
	{
		g_autoptr(AxisFileView) view = NULL;
		g_autoptr(GError) error = NULL;
		const AxisMediaStream *s;

		g_assert_true(axis_probe_get_file_view(fx->probe, file_id(fx, "hdr10.mkv"), &view, &error));
		g_assert_no_error(error);
		g_assert_nonnull(view->info);
		s = g_ptr_array_index(view->info->streams, 0);
		g_assert_true(s->hdr10.mastering);
		g_assert_cmpint(s->hdr10.red_x, ==, 34000);
		g_assert_cmpint(s->hdr10.white_y, ==, 16450);
		g_assert_cmpint(s->hdr10.min_luminance, ==, 1);
		g_assert_cmpint(s->hdr10.max_luminance, ==, 10000000);
		g_assert_true(s->hdr10.light);
		g_assert_cmpint(s->hdr10.max_content, ==, 1000);
		g_assert_cmpint(s->hdr10.max_average, ==, 400);
	}
	/* Spec: Conflicting signals: flagged, not classified by guess. */
	assert_q(fx, "SELECT coalesce(hdr_format, 'none') || '|' || hdr_uncertain FROM media_info WHERE file_id ="
	         " (SELECT id FROM library_file WHERE rel_path = 'pq-bare.mkv')", "none|true");
	assert_q(fx, "SELECT coalesce(hdr_format, 'none') || '|' || hdr_uncertain FROM media_info WHERE file_id ="
	         " (SELECT id FROM library_file WHERE rel_path = 'unsig10.mkv')", "none|true");
	assert_q(fx, "SELECT hdr_format || '|' || hdr_uncertain FROM media_info WHERE file_id ="
	         " (SELECT id FROM library_file WHERE rel_path = 'sdr.mkv')", "sdr|false");
	/* Never "no class and no doubt" for a picture. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream WHERE kind = 'video' AND NOT attached_pic"
	                  " AND hdr_format IS NULL AND NOT hdr_uncertain"), ==, 0);
	/* The frame call: 3 files need it (10-bit), the SDR one does not. */
	g_assert_cmpuint(runs(fx), ==, 3 * 2 + 1);
}

/* ── Corrupt and hostile files ──────────────────────────────────── */

/* Spec: Corrupt file; other files continue to be probed. */
static void
test_corrupt_files(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *random_bytes = g_malloc(3000);
	g_autofree gchar *short_mp4 = NULL;
	g_autofree gchar *path = clip_path("short.mp4");
	gsize length = 0;
	guint i;
	guint64 before;

	SKIP_WITHOUT_MEDIA(fx);
	for (i = 0; i < 3000; i++)
		random_bytes[i] = (gchar)g_random_int_range(0, 256);
	g_assert_true(g_file_get_contents(path, &short_mp4, &length, NULL));
	axis_test_storage_mkdir(fx->films);
	put_bytes(fx->films, "a-random.mkv", random_bytes, 3000);
	put_bytes(fx->films, "b-cut.mp4", short_mp4, length / 2);
	put_bytes(fx->films, "c-empty.mkv", "", 0);
	put_bytes(fx->films, "d-text.mkv", "just some words\n", 16);
	install(fx, "sdr.mkv", "e-good.mkv");
	scan(fx);
	probe_all(fx);

	assert_q(fx, "SELECT string_agg(m.status, ' ' ORDER BY f.rel_path) FROM media_info m JOIN library_file f"
	         " ON f.id = m.file_id", "failed failed failed failed ok");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE status = 'failed' AND error ~ 'could not read'"),
	                ==, 4);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE status = 'failed' AND error ~ 'Invalid data'"),
	                >=, 3);
	/* No streams and no container for what failed. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream WHERE file_id IN"
	                  " (SELECT file_id FROM media_info WHERE status <> 'ok')"), ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE changed"), ==, 0);

	/* Failed once, not again: nothing is queued, nothing is run. */
	before = runs(fx);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);
	probe_all(fx);
	g_assert_cmpuint(runs(fx), ==, before);
}

/* Spec: Hostile file name. */
static void
test_hostile_names(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = {
		"-leading-dash.mkv", "--version.mkv", "-i.mkv", "-protocol_whitelist.mkv", "a:b.mkv", "file:x.mkv",
		"pipe:0.mkv", "fd:3.mkv", "http:evil.mkv", "concat:a|b.mkv", "two\nlines.mkv", "tab\there.mkv",
		"bad\xff.mkv", "quote'\".mkv", "[bracket].mkv", "$(touch pwned).mkv", "`id`.mkv", "; rm -rf.mkv",
		"-.mkv", NULL
	};
	g_auto(GStrv) argv = NULL;
	guint i;

	SKIP_WITHOUT_MEDIA(fx);
	for (i = 0; names[i] != NULL; i++)
		install(fx, "sdr.mkv", names[i]);
	scan(fx);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE kind = 'video'"), ==, (gint64)i);
	probe_all(fx);

	/* Every one was probed as the local file it is. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE status = 'ok' AND container = 'matroska,webm'"),
	                ==, (gint64)i);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE status <> 'ok'"), ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE name_invalid_utf8"), ==, 1);

	/* The command line has no room for a name: the file is descriptor 3,
	 * and the only protocol is the descriptor protocol. */
	argv = axis_probe_build_argv(fx->probe, "/usr/bin/ffprobe", FALSE, 0);
	g_assert_cmpstr(argv[0], ==, "/usr/bin/ffprobe");
	for (i = 0; argv[i] != NULL; i++) {
		if (strcmp(argv[i], "-protocol_whitelist") == 0)
			g_assert_cmpstr(argv[i + 1], ==, "fd");
		g_assert_null(strstr(argv[i], "file:"));
		g_assert_null(strstr(argv[i], "pipe:"));
	}
	g_assert_cmpstr(argv[i - 2], ==, "-i");
	g_assert_cmpstr(argv[i - 1], ==, "fd:");
	/* Nothing the probe made is left in the folder: it only reads. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE rel_path LIKE 'pwned%%'"), ==, 0);
}

static gint connections;	/* atomic */

static gboolean
on_incoming(
	GThreadedSocketService *service,
	GSocketConnection      *connection,
	GObject                *source,
	gpointer                data
){
	g_atomic_int_inc(&connections);
	return TRUE;
}

static gboolean
connection_seen(gpointer data)
{
	return g_atomic_int_get(&connections) > 0;
}

static void
run_tool_directly(
	const gchar *const *argv,
	gchar              **standard_output
){
	g_autofree gchar *standard_error = NULL;
	g_autoptr(GError) error = NULL;
	gint status = 0;

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, standard_output,
		&standard_error, &status, &error));
}

/* Spec: No network access from probing.  A playlist that names a URL, a
 * playlist and a script that name another local file, and a manifest:
 * none is followed, and each is recorded as a file the probe could not
 * read.  The controls show that the same files DO make ffprobe reach out
 * when it is run without the probe's arguments. */
static void
test_no_network_and_no_other_file(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketService) service = g_threaded_socket_service_new(4);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *url_playlist = NULL;
	g_autofree gchar *local_playlist = NULL;
	g_autofree gchar *control_url = NULL;
	g_autofree gchar *control_local = NULL;
	g_autofree gchar *output = NULL;
	guint16 port;

	SKIP_WITHOUT_MEDIA(fx);
	port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(service), NULL, &error);
	g_assert_no_error(error);
	g_signal_connect(service, "run", G_CALLBACK(on_incoming), NULL);
	g_socket_service_start(G_SOCKET_SERVICE(service));

	other = clip_path("other.ts");
	url_playlist = g_strdup_printf("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:1.0,\n"
		"http://127.0.0.1:%u/segment.ts\n#EXT-X-ENDLIST\n", port);
	local_playlist = g_strdup_printf("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:1.0,\n"
		"%s\n#EXT-X-ENDLIST\n", other);

	/* Controls: with the extension ffprobe wants and its protocols opened
	 * up, the playlists are followed. */
	control_url = g_build_filename(fx->disk, "control-url.m3u8", NULL);
	control_local = g_build_filename(fx->disk, "control-local.m3u8", NULL);
	axis_test_storage_write(control_url, url_playlist);
	axis_test_storage_write(control_local, local_playlist);
	{
		const gchar *argv[] = { "ffprobe", "-v", "quiet", "-rw_timeout", "3000000", "-protocol_whitelist",
			"file,http,tcp,crypto", "-i", control_url, NULL };

		run_tool_directly(argv, NULL);
	}
	/* The listener accepts on the main loop: pump it. */
	g_assert_true(axis_test_pump_until(connection_seen, NULL, 5000));	/* it sees what is sent to it */
	{
		const gchar *argv[] = { "ffprobe", "-v", "error", "-show_entries", "stream=sample_rate", "-of", "csv=p=0",
			control_local, NULL };

		run_tool_directly(argv, &output);
		g_assert_nonnull(strstr(output, "12000"));	/* the other file was read */
	}
	g_atomic_int_set(&connections, 0);

	/* The same contents, in files the library holds. */
	install_text(fx, "hls-url.mkv", url_playlist);
	install_text(fx, "hls-url.mp4", url_playlist);
	install_text(fx, "hls-local.mkv", local_playlist);
	{
		g_autofree gchar *concat_abs = g_strdup_printf("ffconcat version 1.0\nfile '%s'\n", other);

		install_text(fx, "concat-abs.mkv", concat_abs);
	}
	install_text(fx, "concat-rel.mkv", "ffconcat version 1.0\nfile 'other.ts'\n");
	install_text(fx, "concat-url.mkv", "ffconcat version 1.0\nfile 'http://127.0.0.1:1/x.ts'\n");
	install_text(fx, "sdp.mkv", "v=0\no=- 0 0 IN IP4 127.0.0.1\ns=x\nc=IN IP4 127.0.0.1\nt=0 0\n"
	             "m=video 5004 RTP/AVP 96\na=rtpmap:96 H264/90000\n");
	install_text(fx, "dash.mkv", "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" "
	             "type=\"static\" mediaPresentationDuration=\"PT1S\"><Period><AdaptationSet><Representation "
	             "id=\"1\" bandwidth=\"1000\"><BaseURL>http://127.0.0.1:1/</BaseURL><SegmentList>"
	             "<SegmentURL media=\"a.ts\"/></SegmentList></Representation></AdaptationSet></Period></MPD>");
	scan(fx);
	probe_all(fx);

	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE kind = 'video'"), ==, 8);
	/* Every one of them was refused: none is a format the probe reads. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE status = 'failed'"), ==, 8);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream"), ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE error IS NULL OR error = ''"), ==, 0);

	/* No request reached the listener, and nothing of the other file. */
	axis_test_pump_for(500);
	g_assert_cmpint(g_atomic_int_get(&connections), ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream WHERE sample_rate = 12000"), ==, 0);

	g_socket_service_stop(service);
	g_socket_listener_close(G_SOCKET_LISTENER(service));
}

/* The protocol whitelist alone (without the format whitelist) denies a
 * script that names another local file, even where the script format would
 * allow it (-safe 0 lets a concat script name any path). */
static void
test_protocol_whitelist_alone(Fixture *fx, gconstpointer data)
{
	gchar *dir = NULL;	/* freed by axis_test_storage_dir_free() */
	g_autofree gchar *script_path = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *script_text = NULL;
	g_autofree gchar *command_open = NULL;
	g_autofree gchar *command_closed = NULL;
	g_autofree gchar *output_open = NULL;
	g_autofree gchar *output_closed = NULL;
	g_autofree gchar *error_open = NULL;
	g_autofree gchar *error_closed = NULL;
	g_autoptr(GError) error = NULL;
	gint status = 0;

	SKIP_WITHOUT_MEDIA(fx);
	dir = axis_test_storage_dir_new();
	script_path = g_build_filename(dir, "concat.ffconcat", NULL);
	other = clip_path("other.ts");
	script_text = g_strdup_printf("ffconcat version 1.0\nfile '%s'\n", other);
	axis_test_storage_write(script_path, script_text);

	command_open = g_strdup_printf("exec ffprobe -v error -safe 0 -show_entries stream=sample_rate -of csv=p=0"
		" -i '%s'", script_path);
	command_closed = g_strdup_printf("exec ffprobe -v error -protocol_whitelist fd -safe 0 -fd 3"
		" -show_entries stream=sample_rate -of csv=p=0 -i fd: 3<'%s'", script_path);
	{
		const gchar *argv_open[] = { "sh", "-c", command_open, NULL };
		const gchar *argv_closed[] = { "sh", "-c", command_closed, NULL };

		/* Given as a path, the script is followed to the other file... */
		g_assert_true(g_spawn_sync(NULL, (gchar **)argv_open, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			&output_open, &error_open, &status, &error));
		g_assert_nonnull(strstr(output_open, "12000"));
		/* ...given as the probe gives it, the names in it are resolved
		 * against "fd:" and refused: only the descriptor can be read. */
		g_assert_true(g_spawn_sync(NULL, (gchar **)argv_closed, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			&output_closed, &error_closed, &status, &error));
		g_assert_null(strstr(output_closed, "12000"));
		g_assert_nonnull(strstr(error_closed, "Impossible to open"));
	}
	axis_test_storage_dir_free(dir);
}

/* ── Hung and runaway tools ─────────────────────────────────────── */

/* Spec: Probe tool hangs: killed, recorded as timed out, no child left. */
static void
test_hung_tool(Fixture *fx, gconstpointer data)
{
	AxisProbeOutcome outcome;
	guint fds;
	gint64 started;
	gint status;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	install(fx, "sdr.mkv", "warm-up.mkv");
	scan(fx);
	/* One probe first, so that the connection pool and the storage's
	 * helper threads have what they keep open. */
	probe(fx, file_id(fx, "warm-up.mkv"));
	use_fake(fx, "hang");
	fx->tuning.timeout_ms = 600;
	tune(fx);

	fds = axis_test_storage_count_fds();
	started = g_get_monotonic_time();
	outcome = probe(fx, file_id(fx, "a.mkv"));
	g_assert_cmpint(outcome, ==, AXIS_PROBE_OUTCOME_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);

	assert_q(fx, "SELECT status FROM media_info WHERE file_id = (SELECT id FROM library_file WHERE"
	         " rel_path = 'a.mkv')", "timeout");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE error ~ 'did not finish within 0.6 seconds'"), ==, 1);
	assert_q(fx, "SELECT changed::text FROM library_file WHERE rel_path = 'a.mkv'", "false");
	/* Nothing left: no descriptor, no child (running or zombie). */
	g_assert_cmpuint(axis_test_storage_count_fds(), ==, fds);
	g_assert_cmpint(waitpid(-1, &status, WNOHANG), ==, -1);
	g_assert_cmpint(errno, ==, ECHILD);
	g_assert_cmpuint(axis_probe_run_get_unreaped(), ==, 0);
	/* Recorded once. */
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_NOTHING);
}

/* Spec: Unexpected tool output (malformed, enormous, wrongly shaped). */
static void
test_odd_tool_output(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *mode;
		const gchar *expected;
	} cases[] = {
		{ "garbage", "could not be read" },
		{ "json-empty", "no information about the container" },
		{ "deep", "could not be read" },
		{ "exit3", "could not read the file" },
		{ "flood", "more than" },
		{ "segv", "crashed" },
		{ "evil-error", "boom" }
	};
	g_autofree gchar *shown = NULL;
	guint i;
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	id = file_id(fx, "a.mkv");
	fx->tuning.output_cap = 64 * 1024;
	fx->tuning.timeout_ms = 20000;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *text = NULL;
		const gchar *p;

		use_fake(fx, cases[i].mode);
		g_assert_true(axis_probe_request_file(fx->probe, id, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_FAILED);

		text = q(fx, "SELECT error FROM media_info WHERE status = 'failed'");
		g_assert_nonnull(text);
		if (strstr(text, cases[i].expected) == NULL)
			g_error("mode %s: the error was \"%s\", expected it to mention \"%s\"", cases[i].mode, text,
				cases[i].expected);
		/* Cleaned: nothing a page or a line of log could be broken by. */
		g_assert_true(g_utf8_validate(text, -1, NULL));
		for (p = text; *p != '\0'; p++) {
			g_assert_true((guchar)*p >= 0x20 && *p != 0x7f);
			g_assert_false((guchar)p[0] == 0xe2 && (guchar)p[1] == 0x80 && (guchar)p[2] == 0xae);
		}
		g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream"), ==, 0);
	}
	/* The error text of the last one reached the log, cleaned too. */
	shown = axis_test_log_find(G_LOG_LEVEL_INFO, "*probe:*boom*");
	g_assert_nonnull(shown);
	g_assert_null(strchr(shown, '\033'));
	g_assert_null(strchr(shown, '\a'));
	g_assert_null(strchr(shown, '\r'));
	g_assert_null(strchr(shown, '\n'));
}

/* Without ffprobe nothing is recorded: the file stays as it was. */
static void
test_tool_missing(Fixture *fx, gconstpointer data)
{
	const gchar *const missing[] = { "/nonexistent/ffprobe", NULL };
	g_autoptr(AxisSettings) settings = axis_settings_new();
	g_autoptr(AxisProbe) configured = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path_setting = NULL;
	guint enqueued = 0;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);

	/* A command that cannot be started: nothing is recorded and the file
	 * stays as it was. */
	fx->tuning.tool_command = missing;
	tune(fx);
	probe_fails(fx, file_id(fx, "a.mkv"), AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 0);
	assert_q(fx, "SELECT changed::text FROM library_file", "true");

	/* A program that is not installed, named in the settings: the same,
	 * and nothing is enqueued meanwhile, with one warning. */
	g_assert_true(axis_settings_add_table(settings, AXIS_PROBE_MODULE_ID, axis_probe_get_setting_table(), &error));
	g_assert_true(axis_settings_load(settings, NULL, &error));
	g_assert_no_error(error);
	path_setting = axis_settings_get_string(settings, AXIS_PROBE_SETTING_PATH);
	g_assert_cmpstr(path_setting, ==, "ffprobe");
	g_assert_true(axis_settings_set(settings, AXIS_PROBE_SETTING_PATH, "/nonexistent/dir/ffprobe", &error));
	g_assert_no_error(error);
	configured = axis_probe_new(fx->t->pool, fx->t->storage, settings);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*ffprobe was not found*");
	g_assert_true(axis_probe_enqueue_due(configured, FALSE, &enqueued, &error));
	g_assert_true(axis_probe_enqueue_due(configured, FALSE, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*ffprobe was not found*"), ==, 1);
	{
		AxisProbeOutcome outcome;

		g_assert_false(axis_probe_file(configured, file_id(fx, "a.mkv"), NULL, &outcome, &error));
		g_assert_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL);
		g_clear_error(&error);
	}

	/* The real one is found again: enqueued. */
	use_real(fx);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);

	/* The time limit setting is in effect, within its bounds. */
	g_assert_true(axis_settings_set(settings, AXIS_PROBE_SETTING_TIMEOUT, "90s", &error));
	g_assert_cmpuint(axis_probe_get_timeout_ms(configured), ==, 90000);
	g_assert_false(axis_settings_set(settings, AXIS_PROBE_SETTING_TIMEOUT, "1s", &error));
	g_clear_error(&error);
	g_assert_cmpuint(axis_probe_get_timeout_ms(configured), ==, 90000);
}

/* ── When files are probed ──────────────────────────────────────── */

/* Spec: New file is probed once. */
static void
test_new_file_probed_once(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);

	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);	/* already queued */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file' AND queue = 'probe'"
	                  " AND dedupe_key = 'probe.file:' || (SELECT id FROM library_file)"), ==, 1);
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_STORED);
	g_assert_cmpuint(runs(fx), ==, 1);

	/* A scan that finds nothing new probes nothing. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job");
	scan(fx);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_NOTHING);
	g_assert_cmpuint(runs(fx), ==, 1);
}

/* Spec: Changed file is re-probed. */
static void
test_changed_file_reprobed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	probe(fx, file_id(fx, "a.mkv"));
	first = q(fx, "SELECT file_size || '|' || container || '|' || audio_streams FROM media_info");

	/* The file becomes a different one: no audio, another container. */
	install(fx, "short.mp4", "a.mkv");
	scan(fx);
	assert_q(fx, "SELECT changed::text FROM library_file", "true");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_STORED);
	second = q(fx, "SELECT file_size || '|' || container || '|' || audio_streams FROM media_info");

	g_assert_cmpstr(first, !=, second);
	g_assert_nonnull(strstr(second, "mov,mp4"));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 1);	/* replaced, not added to */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream WHERE kind = 'audio'"), ==, 0);
	assert_q(fx, "SELECT changed::text FROM library_file", "false");
}

/* Spec: File still being written: the result is discarded. */
static void
test_file_changing_during_the_probe(Fixture *fx, gconstpointer data)
{
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	id = file_id(fx, "a.mkv");
	fx->tuning.after_run = hook_after_run;
	fx->hook_path = g_strdup("a.mkv");

	/* It grows while the tool reads it. */
	fx->action = HOOK_APPEND;
	tune(fx);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_DEFERRED);
	g_assert_cmpint(fx->hook_calls, ==, 1);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_stream"), ==, 0);
	assert_q(fx, "SELECT changed::text FROM library_file", "true");
	assert_q(fx, "SELECT attempts || '|' || (reason ~ 'while it was being probed')::text FROM media_probe_defer",
	         "1|true");
	/* It waits out its delay instead of being probed over and over. */
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);

	/* The next scan sees the new size; once the delay is over the file is
	 * probed, and the result is for the file as it is. */
	fx->action = HOOK_NONE;
	tune(fx);
	scan(fx);
	axis_test_db_exec(fx->t->db, "UPDATE media_probe_defer SET until = now() - interval '1 second'");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_STORED);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_probe_defer"), ==, 0);
	assert_q(fx, "SELECT (file_size = (SELECT size FROM library_file))::text FROM media_info", "true");
}

/* Another file taking the name while the tool runs is not the file that
 * was probed either. */
static void
test_file_replaced_during_the_probe(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	fx->tuning.after_run = hook_after_run;
	fx->hook_path = g_strdup("a.mkv");
	fx->action = HOOK_REPLACE;
	tune(fx);

	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_DEFERRED);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 0);
	assert_q(fx, "SELECT changed::text FROM library_file", "true");
}

/* A file that changed after the scan (but before the probe) is left to the
 * next scan. */
static void
test_file_changed_since_the_scan(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	path = g_build_filename(fx->films, "a.mkv", NULL);
	axis_test_set_mtime(path, 1000000000, 0);

	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_DEFERRED);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 0);
	g_assert_cmpuint(runs(fx), ==, 0);	/* the tool never ran */

	scan(fx);
	axis_test_db_exec(fx->t->db, "DELETE FROM media_probe_defer");
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_STORED);
}

/* Spec: Failed probe is not retried in a loop; an administrator can ask. */
static void
test_failure_is_not_retried_but_can_be_requested(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 job = 0;

	SKIP_WITHOUT_MEDIA(fx);
	install_text(fx, "bad.mkv", "not a video at all\n");
	scan(fx);
	id = file_id(fx, "bad.mkv");

	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_FAILED);
	g_assert_cmpuint(runs(fx), ==, 1);
	assert_q(fx, "SELECT status FROM media_info", "failed");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_NOTHING);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_NOTHING);
	g_assert_cmpuint(runs(fx), ==, 1);

	/* Asked for: probed again (and fails again). */
	g_assert_true(axis_probe_request_file(fx->probe, id, &job, &error));
	g_assert_no_error(error);
	g_assert_cmpint(job, >, 0);
	g_assert_cmpint(n(fx, "SELECT priority FROM job WHERE id = %" G_GINT64_FORMAT, job), ==,
	                AXIS_PROBE_REQUEST_PRIORITY);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_FAILED);
	g_assert_cmpuint(runs(fx), ==, 2);

	/* Changed: probed again, and now it is a video. */
	install(fx, "sdr.mkv", "bad.mkv");
	scan(fx);
	axis_test_db_exec(fx->t->db, "DELETE FROM job");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_STORED);
	assert_q(fx, "SELECT status FROM media_info", "ok");

	/* Not a media file, or gone: refused. */
	g_assert_false(axis_probe_request_file(fx->probe, 99999, NULL, &error));
	g_assert_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND);
}

/* The failed files of a root can be asked for together. */
static void
test_request_failed_files_of_a_root(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint requested = 0;
	gboolean capped = TRUE;

	SKIP_WITHOUT_MEDIA(fx);
	install_text(fx, "bad1.mkv", "not a video\n");
	install_text(fx, "bad2.mkv", "not a video either\n");
	install(fx, "sdr.mkv", "good.mkv");
	scan(fx);
	probe_all(fx);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info WHERE status = 'failed'"), ==, 2);

	g_assert_true(axis_probe_request_failed(fx->probe, fx->root, &requested, &capped, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(requested, ==, 2);
	g_assert_false(capped);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file'"), ==, 2);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM library_file WHERE changed"), ==, 2);

	g_assert_true(axis_probe_request_failed(fx->probe, fx->root2 + 1000, &requested, &capped, &error));
	g_assert_cmpuint(requested, ==, 0);
}

/* The version of the mapping is bumped: older records are probed again,
 * at low priority. */
static void
test_version_bump(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *next = g_strdup_printf("%d", AXIS_PROBE_VERSION + 1);
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	install_text(fx, "bad.mkv", "nothing\n");
	scan(fx);
	probe_all(fx);
	g_assert_cmpuint(enqueue_due(fx, TRUE), ==, 0);
	axis_test_db_exec(fx->t->db, "DELETE FROM job");

	fx->tuning.version = AXIS_PROBE_VERSION + 1;
	tune(fx);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);	/* nothing changed */
	g_assert_cmpuint(enqueue_due(fx, TRUE), ==, 2);	/* a failed record too */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file' AND priority = %d",
	                  AXIS_PROBE_STALE_PRIORITY), ==, 2);
	id = file_id(fx, "a.mkv");
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_STORED);
	assert_q(fx, "SELECT probe_version FROM media_info WHERE file_id = (SELECT id FROM library_file WHERE"
	         " rel_path = 'a.mkv')", next);
	/* Once each. */
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_NOTHING);
	probe_all(fx);
	axis_test_db_exec(fx->t->db, "DELETE FROM job");
	g_assert_cmpuint(enqueue_due(fx, TRUE), ==, 0);
}

/* A change a scan that did not complete saw waits for one that did. */
static void
test_waits_for_a_complete_scan(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	probe_all(fx);

	install(fx, "short.mp4", "a.mkv");
	scan(fx);
	/* As if the change had been noted by a scan that then failed. */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET changed_generation = changed_generation + 1,"
	                  " last_seen_generation = last_seen_generation + 1");
	axis_test_db_exec(fx->t->db, "UPDATE library_root_state SET generation = generation + 1");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_NOTHING);
	axis_test_db_exec(fx->t->db, "UPDATE library_root_state SET complete_generation = generation");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
}

/* Only video and audio that are present are probed. */
static void
test_only_media_that_is_present(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	install_text(fx, "notes.txt", "hello\n");
	install_text(fx, "poster.jpg", "jpg");
	install_text(fx, "subs.srt", "1\n");
	scan(fx);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpint(probe(fx, file_id(fx, "notes.txt")), ==, AXIS_PROBE_OUTCOME_NOTHING);
	g_assert_cmpint(probe(fx, file_id(fx, "poster.jpg")), ==, AXIS_PROBE_OUTCOME_NOTHING);
	g_assert_cmpint(probe(fx, 424242), ==, AXIS_PROBE_OUTCOME_NOTHING);
	g_assert_cmpuint(runs(fx), ==, 0);

	/* Gone files are not probed. */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET state = 'suspected', suspected_at = now(),"
	                  " suspected_scan_id = 1 WHERE rel_path = 'a.mkv'");
	axis_test_db_exec(fx->t->db, "DELETE FROM job");
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);
	g_assert_cmpint(probe(fx, file_id(fx, "a.mkv")), ==, AXIS_PROBE_OUTCOME_NOTHING);
}

/* ── Volumes ────────────────────────────────────────────────────── */

/* Spec: Volume goes offline: the status is unchanged, the job is retried. */
static void
test_volume_offline(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	id = file_id(fx, "a.mkv");
	probe(fx, id);
	before = q(fx, "SELECT status || '|' || file_size || '|' || probed_at::text FROM media_info");

	/* The file changes, and then the disk goes away. */
	install(fx, "short.mp4", "a.mkv");
	scan(fx);
	axis_test_library_unplug(fx->t, fx->disk, "away");
	fx->unplugged = TRUE;
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));

	probe_fails(fx, id, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_UNAVAILABLE);
	{
		g_autofree gchar *after = q(fx, "SELECT status || '|' || file_size || '|' || probed_at::text FROM media_info");

		g_assert_cmpstr(before, ==, after);
	}
	assert_q(fx, "SELECT changed::text FROM library_file", "true");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_probe_defer"), ==, 0);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);	/* offline volumes are not queued */

	/* Back: retried, and stored. */
	axis_test_library_plug(fx->t, fx->disk, "away");
	fx->unplugged = FALSE;
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_STORED);
	assert_q(fx, "SELECT status FROM media_info", "ok");
}

/* The volume goes while the tool is running: nothing is recorded, not
 * even as a failure. */
static void
test_volume_goes_during_the_probe(Fixture *fx, gconstpointer data)
{
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install_text(fx, "bad.mkv", "not a video\n");
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);

	/* A file the tool cannot read, on a disk that goes: the failure is
	 * the disk's, not the file's. */
	id = file_id(fx, "bad.mkv");
	fx->tuning.after_run = hook_after_run;
	fx->action = HOOK_UNPLUG;
	tune(fx);
	probe_fails(fx, id, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_UNAVAILABLE);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 0);
	assert_q(fx, "SELECT (SELECT count(*) FROM library_file WHERE changed)::text", "2");
}

/* The scan has priority: no probe starts during one, and enqueueing
 * waits; a probe does not make a scan fail. */
static void
test_scan_and_probe_exclude_each_other(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gchar *lock_sql;
	gchar *unlock_sql;
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan(fx);
	id = file_id(fx, "a.mkv");

	lock_sql = g_strdup_printf("SELECT pg_advisory_lock(hashtext('axis.library.volume'), %" G_GINT64_FORMAT ")",
		fx->volume);
	unlock_sql = g_strdup_printf("SELECT pg_advisory_unlock(hashtext('axis.library.volume'), %" G_GINT64_FORMAT ")",
		fx->volume);
	axis_test_db_exec(fx->t->db, lock_sql);	/* a scan of the volume is running */
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);
	probe_fails(fx, id, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_BUSY);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM media_info"), ==, 0);
	g_assert_cmpuint(runs(fx), ==, 0);
	axis_test_db_exec(fx->t->db, unlock_sql);

	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 1);
	g_assert_cmpint(probe(fx, id), ==, AXIS_PROBE_OUTCOME_STORED);
	/* And the probe's lock is gone with it. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory'"
	                  " AND database = (SELECT oid FROM pg_database WHERE datname = current_database())"
	                  " AND granted AND pid <> pg_backend_pid()"), ==, 0);
	g_free(lock_sql);
	g_free(unlock_sql);
}

typedef struct
{
	Fixture          *fx;
	gint64            id;
	AxisProbeOutcome  outcome;
	gint64            duration_us;
	GError           *error;
} Background;

static gpointer
probe_in_background(gpointer data)
{
	Background *bg = data;
	gint64 started = g_get_monotonic_time();

	axis_probe_file(bg->fx->probe, bg->id, NULL, &bg->outcome, &bg->error);
	bg->duration_us = g_get_monotonic_time() - started;
	return NULL;
}

static gboolean
a_probe_holds_a_lock(gpointer data)
{
	Fixture *fx = data;
	g_autofree gchar *value = axis_test_db_query(fx->t->db,
		"SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND granted AND pid <> pg_backend_pid()");

	return g_strcmp0(value, "0") != 0;
}

/* Spec (design): per-volume serialisation.  One probe of a volume at a
 * time; probes of other volumes are not held up; a scan of the volume is
 * not held up either. */
static void
test_one_probe_per_volume(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	Background bg;
	GThread *thread;
	gint64 started;
	gint64 slow;
	gint64 fast;
	gint64 other;

	SKIP_WITHOUT_MEDIA(fx);
	add_second_volume(fx);
	install(fx, "sdr.mkv", "a.mkv");
	install(fx, "multi.mkv", "b.mkv");
	scan(fx);
	{
		g_autofree gchar *track = g_build_filename(fx->tracks, "t.mkv", NULL);
		g_autofree gchar *source = clip_path("sdr.mkv");
		g_autofree gchar *bytes = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(source, &bytes, &length, NULL));
		g_assert_true(g_file_set_contents(track, bytes, (gssize)length, NULL));
		axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->root2, AXIS_SCAN_COMPLETE,
			AXIS_SCAN_REASON_NONE));
	}
	slow = file_id(fx, "a.mkv");
	fast = file_id(fx, "b.mkv");
	other = n(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT, fx->root2);

	use_fake(fx, "sleep2");
	fx->tuning.lock_wait_ms = 300;
	tune(fx);

	memset(&bg, 0, sizeof(bg));
	bg.fx = fx;
	bg.id = slow;
	thread = g_thread_new("probe", probe_in_background, &bg);
	g_assert_true(axis_test_pump_until(a_probe_holds_a_lock, fx, AXIS_TEST_WAIT_MS));

	/* The same volume: waits its 300 ms and gives up. */
	probe_fails(fx, fast, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_BUSY);
	/* Another volume: runs now, alongside. */
	started = g_get_monotonic_time();
	probe(fx, other);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3500000);
	/* A scan of the first volume is not refused. */
	scan(fx);

	g_thread_join(thread);
	g_assert_no_error(bg.error);
	g_assert_cmpint(bg.duration_us, >=, 1900000);
	/* The one that waited is still waiting for its turn, untouched. */
	assert_q(fx, "SELECT (SELECT count(*) FROM media_info WHERE file_id = " "(SELECT id FROM library_file"
	         " WHERE rel_path = 'b.mkv'))::text", "0");
}

/* Spec (design): enqueueing tops the queue up to a target rather than
 * flooding it, and takes the volumes in turn. */
static void
test_queue_is_topped_up_in_turn(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *order = NULL;
	guint i;

	SKIP_WITHOUT_DB(fx);
	add_second_volume(fx);
	axis_test_storage_mkdir(fx->films);
	for (i = 0; i < AXIS_PROBE_QUEUE_TARGET + 100; i++) {
		g_autofree gchar *name = g_strdup_printf("f%04u.mkv", i);

		put_bytes(fx->films, name, "x", 1);
	}
	for (i = 0; i < 3; i++) {
		g_autofree gchar *name = g_strdup_printf("m%u.flac", i);

		put_bytes(fx->tracks, name, "x", 1);
	}
	scan(fx);
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->root2, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
	use_fake(fx, "echo");

	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, AXIS_PROBE_QUEUE_TARGET);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file'"), ==, AXIS_PROBE_QUEUE_TARGET);
	g_assert_cmpuint(enqueue_due(fx, FALSE), ==, 0);	/* full */
	/* Alternating, while both have files. */
	order = q(fx, "SELECT string_agg(f.volume_id::text, ',' ORDER BY j.id) FROM"
	          " (SELECT * FROM job ORDER BY id LIMIT 6) j JOIN library_file f"
	          " ON f.id = (j.payload->>'file_id')::bigint");
	{
		g_autofree gchar *expected = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
			",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT, fx->volume, fx->volume2, fx->volume,
			fx->volume2, fx->volume, fx->volume2);

		g_assert_cmpstr(order, ==, expected);
	}
}

/* ── What the pages read ────────────────────────────────────────── */

static void
test_views(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisFileView) view = NULL;
	g_autoptr(AxisRootProbeView) root_view = NULL;
	gint64 good;
	gint64 bad;
	gint64 waiting;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "multi.mkv", "good.mkv");
	install_text(fx, "bad.mkv", "nothing here\n");
	scan(fx);
	good = file_id(fx, "good.mkv");
	bad = file_id(fx, "bad.mkv");
	probe(fx, good);
	probe(fx, bad);
	install(fx, "sdr.mkv", "waiting.mkv");
	scan(fx);
	waiting = file_id(fx, "waiting.mkv");

	g_assert_true(axis_probe_get_file_view(fx->probe, good, &view, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(view->state, ==, "present");
	g_assert_false(view->waiting);
	g_assert_nonnull(view->info);
	g_assert_cmpstr(view->info->status, ==, "ok");
	g_assert_cmpuint(view->info->streams->len, ==, 5);
	g_assert_cmpstr(view->info->video_codec, ==, "h264");
	g_assert_cmpint(view->info->hdr_format, ==, AXIS_HDR_SDR);
	g_assert_cmpstr(view->info->resolution_class, ==, "sd");
	g_clear_pointer(&view, axis_file_view_free);

	g_assert_true(axis_probe_get_file_view(fx->probe, bad, &view, &error));
	g_assert_cmpstr(view->info->status, ==, "failed");
	g_assert_nonnull(view->info->error);
	g_assert_cmpuint(view->info->streams->len, ==, 0);
	g_clear_pointer(&view, axis_file_view_free);

	g_assert_true(axis_probe_get_file_view(fx->probe, waiting, &view, &error));
	g_assert_null(view->info);
	g_assert_true(view->waiting);
	g_clear_pointer(&view, axis_file_view_free);

	g_assert_false(axis_probe_get_file_view(fx->probe, 31337, &view, &error));
	g_assert_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_assert_true(axis_probe_get_root_view(fx->probe, fx->root, &root_view, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(root_view->media_files, ==, 3);
	g_assert_cmpuint(root_view->probed, ==, 1);
	g_assert_cmpuint(root_view->failed, ==, 1);
	g_assert_cmpuint(root_view->never_probed, ==, 1);
	g_assert_cmpuint(root_view->waiting, ==, 1);
	g_assert_cmpuint(root_view->failures->len, ==, 1);
	g_assert_cmpuint(root_view->recent->len, ==, 1);
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/probe/job/video-file", test_video_file);
	ADD("/probe/job/audio-files", test_audio_files);
	ADD("/probe/job/hdr-real-files", test_hdr_real_files);
	ADD("/probe/job/corrupt-files", test_corrupt_files);
	ADD("/probe/job/hostile-names", test_hostile_names);
	ADD("/probe/job/no-network-and-no-other-file", test_no_network_and_no_other_file);
	ADD("/probe/job/protocol-whitelist-alone", test_protocol_whitelist_alone);
	ADD("/probe/job/hung-tool", test_hung_tool);
	ADD("/probe/job/odd-tool-output", test_odd_tool_output);
	ADD("/probe/job/tool-missing", test_tool_missing);
	ADD("/probe/job/new-file-probed-once", test_new_file_probed_once);
	ADD("/probe/job/changed-file-reprobed", test_changed_file_reprobed);
	ADD("/probe/job/file-changing-during-the-probe", test_file_changing_during_the_probe);
	ADD("/probe/job/file-replaced-during-the-probe", test_file_replaced_during_the_probe);
	ADD("/probe/job/file-changed-since-the-scan", test_file_changed_since_the_scan);
	ADD("/probe/job/failure-not-retried", test_failure_is_not_retried_but_can_be_requested);
	ADD("/probe/job/request-failed-files-of-a-root", test_request_failed_files_of_a_root);
	ADD("/probe/job/version-bump", test_version_bump);
	ADD("/probe/job/waits-for-a-complete-scan", test_waits_for_a_complete_scan);
	ADD("/probe/job/only-media-that-is-present", test_only_media_that_is_present);
	ADD("/probe/job/volume-offline", test_volume_offline);
	ADD("/probe/job/volume-goes-during-the-probe", test_volume_goes_during_the_probe);
	ADD("/probe/job/scan-and-probe-exclude-each-other", test_scan_and_probe_exclude_each_other);
	ADD("/probe/job/one-probe-per-volume", test_one_probe_per_volume);
	ADD("/probe/job/queue-topped-up-in-turn", test_queue_is_topped_up_in_turn);
	ADD("/probe/job/views", test_views);
#undef ADD

	return g_test_run();
}
