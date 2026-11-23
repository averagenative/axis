/*
 * axis-test-media.c - test support: small media files made with ffmpeg
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-test-media.h"

#include <stdlib.h>
#include <string.h>

#include "axis-test-storage.h"

static gchar *cache_dir;
static gint state;	/* 0 not tried, 1 ready, -1 unavailable */

static const struct
{
	const gchar *name;
	const gchar *args;
} clips[] = {
	{ "cover.png", "-f lavfi -i testsrc2=size=64x64:rate=1 -frames:v 1" },
	{ "sdr.mkv", "-f lavfi -i testsrc2=size=160x120:rate=24:duration=1 -f lavfi -i sine=frequency=440:duration=1"
	              " -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 48k" },
	{ "multi.mkv", "-f lavfi -i testsrc2=size=160x120:rate=24:duration=1 -f lavfi -i sine=frequency=440:duration=1"
	               " -f lavfi -i sine=frequency=880:duration=1 -i @CACHE@/en.srt -i @CACHE@/fr.srt"
	               " -map 0 -map 1 -map 2 -map 3 -map 4 -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 48k -c:s srt"
	               " -metadata:s:a:0 language=eng -metadata:s:a:1 language=fra"
	               " -metadata:s:s:0 language=eng -metadata:s:s:1 language=fra" },
	{ "hdr10.mkv", "-f lavfi -i testsrc2=size=320x240:rate=24:duration=1 -vf format=yuv420p10le -c:v libx265"
	               " -x265-params 'colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:"
	               "master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):"
	               "max-cll=1000,400:hdr10=1:repeat-headers=1:log-level=error'" },
	{ "pq-bare.mkv", "-f lavfi -i testsrc2=size=320x240:rate=24:duration=1 -vf format=yuv420p10le -c:v libx265"
	                 " -x265-params 'colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:"
	                 "repeat-headers=1:log-level=error'" },
	{ "unsig10.mkv", "-f lavfi -i testsrc2=size=320x240:rate=24:duration=1 -vf format=yuv420p10le -c:v libx265"
	                 " -x265-params log-level=error" },
	{ "tone1.flac", "-f lavfi -i sine=frequency=440:duration=1 -i @CACHE@/cover.png -map 0 -map 1 -c:a flac"
	                " -c:v copy -disposition:v attached_pic -metadata title='Track One' -metadata artist='The Band'"
	                " -metadata album='The Album' -metadata track=1/2 -metadata disc=1/1"
	                " -metadata 'REPLAYGAIN_TRACK_GAIN=-6.5 dB'" },
	{ "tone2.flac", "-f lavfi -i sine=frequency=550:duration=1 -c:a flac -metadata title='Track Two'"
	                " -metadata artist='The Band' -metadata album='The Album' -metadata track=2/2" },
	{ "tone.mp3", "-f lavfi -i sine=frequency=660:duration=1 -c:a libmp3lame -b:a 128k -metadata title=Song" },
	{ "short.mp4", "-f lavfi -i testsrc2=size=160x120:rate=24:duration=1 -c:v libx264 -pix_fmt yuv420p" },
	{ "other.ts", "-f lavfi -i sine=frequency=1234:duration=1 -ar 12000 -c:a aac" }
};

static void
cleanup(void)
{
	if (cache_dir != NULL)
		axis_test_storage_dir_free(cache_dir);
	cache_dir = NULL;
}

static void
make_clip(
	const gchar *name,
	const gchar *args
){
	g_autofree gchar *expanded = NULL;
	g_autofree gchar *out = g_build_filename(cache_dir, name, NULL);
	g_autofree gchar *command = NULL;
	g_autofree gchar *standard_error = NULL;
	g_auto(GStrv) parts = g_strsplit(args, "@CACHE@", -1);
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) error = NULL;
	gint status = 0;

	expanded = g_strjoinv(cache_dir, parts);
	command = g_strdup_printf("ffmpeg -v error -y %s %s", expanded, out);
	g_assert_true(g_shell_parse_argv(command, NULL, &argv, &error));
	g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
		NULL, &standard_error, &status, &error));
	if (!g_spawn_check_wait_status(status, NULL))
		g_error("ffmpeg could not make %s: %s", name, standard_error != NULL ? standard_error : "?");
}

gboolean
axis_test_media_available(void)
{
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *ffprobe = NULL;
	g_autofree gchar *en = NULL;
	g_autofree gchar *fr = NULL;
	guint i;

	if (state != 0)
		return state > 0;
	ffmpeg = g_find_program_in_path("ffmpeg");
	ffprobe = g_find_program_in_path("ffprobe");
	if (ffmpeg == NULL || ffprobe == NULL) {
		state = -1;
		return FALSE;
	}

	cache_dir = axis_test_storage_dir_new();
	atexit(cleanup);
	en = g_build_filename(cache_dir, "en.srt", NULL);
	fr = g_build_filename(cache_dir, "fr.srt", NULL);
	axis_test_storage_write(en, "1\n00:00:00,000 --> 00:00:01,000\nHello\n");
	axis_test_storage_write(fr, "1\n00:00:00,000 --> 00:00:01,000\nBonjour\n");
	for (i = 0; i < G_N_ELEMENTS(clips); i++)
		make_clip(clips[i].name, clips[i].args);
	state = 1;
	return TRUE;
}

gchar *
axis_test_media_path(const gchar *name)
{
	g_return_val_if_fail(cache_dir != NULL, NULL);

	return g_build_filename(cache_dir, name, NULL);
}
