/*
 * test-convert-real.c - conversion plans run for real on generated clips:
 * the outputs are probed with the probe's own calls and must be what the
 * plan says, stream by stream, colour and HDR10 values exactly; the
 * capability check against the installed ffmpeg; the hardware families
 * where it finds them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every clip is a second of ffmpeg's test sources, made here in a private
 * scratch directory and removed at the end; nothing else is read or
 * written.  Without ffmpeg and ffprobe the tests skip.  The same program
 * runs inside the container image (tests/check-convert-container.sh), so
 * that everything here is also shown on that image's ffmpeg; it then says
 * which version it ran with.
 *
 * The hardware encoders run only when AXIS_TEST_HWENC asks for them
 * (`make check-hwenc`): a GPU may be shared with other work, so `make
 * check` leaves it alone, and the capability check here is told to skip
 * those families (and shown not to have started them).  AXIS_TEST_HWENC=1
 * or all runs both families; nvenc or vaapi (or both, comma-separated)
 * runs those only.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-convert.h"
#include "axis-test-storage.h"
#include "probe/axis-probe-describe.h"
#include "probe/axis-probe-run.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-convert-caps.h"

#define GB G_GINT64_CONSTANT(1000000000)
#define RUN_TIMEOUT_MS 120000

typedef struct
{
	gchar                   *dir;
	gchar                   *ffmpeg;
	gchar                   *ffprobe;
	gchar                   *version;
	AxisEncoderCapabilities *caps;	/* measured on this machine */
	AxisEncoderCapabilities *all;	/* every encoder claimed capable */
	gboolean                 ready;
	gboolean                 tried;
} Media;

static Media media;

/* The multi-track clip's name: the kind of name that breaks a command
 * line (ffmpeg is never given it: the file is a descriptor). */
#define MULTI "-- multi $(x) ;.mkv"

static void ffmpeg(const gchar *first, ...) G_GNUC_NULL_TERMINATED;

/* ── Making clips ───────────────────────────────────────────────── */

static void
write_file(
	const gchar *name,
	const gchar *content,
	gssize       length
){
	g_autofree gchar *path = g_build_filename(media.dir, name, NULL);

	g_assert_true(g_file_set_contents(path, content, length, NULL));
}

/* Runs ffmpeg with @args (a NULL-ended list) in the scratch directory. */
static void
ffmpeg(const gchar *first, ...)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autofree gchar *errors = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *arg;
	va_list args;
	gint status = 0;

	g_ptr_array_add(argv, media.ffmpeg);
	g_ptr_array_add(argv, (gpointer)"-nostdin");
	g_ptr_array_add(argv, (gpointer)"-hide_banner");
	g_ptr_array_add(argv, (gpointer)"-loglevel");
	g_ptr_array_add(argv, (gpointer)"error");
	g_ptr_array_add(argv, (gpointer)"-y");
	va_start(args, first);
	for (arg = first; arg != NULL; arg = va_arg(args, const gchar *))
		g_ptr_array_add(argv, (gpointer)arg);
	va_end(args);
	g_ptr_array_add(argv, NULL);
	g_assert_true(g_spawn_sync(media.dir, (gchar **)argv->pdata, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
		&errors, &status, &error));
	if (!g_spawn_check_wait_status(status, NULL))
		g_error("ffmpeg could not make a clip: %s", errors != NULL ? errors : "?");
}

/* A two-by-two PGS subtitle: one display set at 0 s, a clearing one at
 * 1 s (ffmpeg has no PGS encoder). */
static void
write_pgs(void)
{
	static const guint8 sup[] = {
		'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0x16, 0, 19, 0x01, 0x40, 0x00, 0xf0, 0x10, 0, 0, 0x80, 0, 0, 1, 0, 0, 0, 0,
		0, 0x10, 0, 0x10,
		'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0x17, 0, 10, 1, 0, 0, 0x10, 0, 0x10, 0, 2, 0, 2,
		'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0x14, 0, 7, 0, 0, 1, 0xeb, 0x80, 0x80, 0xff,
		'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0x15, 0, 19, 0, 0, 0, 0xc0, 0, 0, 12, 0, 2, 0, 2, 1, 1, 0, 0, 1, 1, 0, 0,
		'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0, 0,
		'P', 'G', 0, 1, 0x5f, 0x90, 0, 0, 0, 0, 0x16, 0, 11, 0x01, 0x40, 0x00, 0xf0, 0x10, 0, 1, 0, 0, 0, 0,
		'P', 'G', 0, 1, 0x5f, 0x90, 0, 0, 0, 0, 0x17, 0, 10, 1, 0, 0, 0x10, 0, 0x10, 0, 2, 0, 2,
		'P', 'G', 0, 1, 0x5f, 0x90, 0, 0, 0, 0, 0x80, 0, 0
	};

	write_file("pgs.sup", (const gchar *)sup, sizeof(sup));
}

#define PICTURE "testsrc2=size=320x240:rate=24:duration=1"
#define TONE(f) "sine=frequency=" f ":duration=1"
#define BT709_FRAMES "setparams=field_mode=prog:color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv"
#define BT709_X264 "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv"

/* The hostile titles, filename and tags of the multi-track clip. */
#define HOSTILE_VIDEO_TITLE "Main <$(rm -rf ~)>"
#define HOSTILE_AUDIO_TITLE "-map 0:9"
#define HOSTILE_FILENAME "--evil$(x).ttf"
#define HOSTILE_FILE_TITLE "$(reboot) -f null -"
#define HOSTILE_CHAPTER "-y /etc/passwd"

static void
make_clips(void)
{
	write_file("en.srt", "1\n00:00:00,000 --> 00:00:01,000\nHello\n", -1);
	write_file("fr.srt", "1\n00:00:00,000 --> 00:00:01,000\nBonjour\n", -1);
	write_file("styled.ass", "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, "
		"PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
		"ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
		"Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n"
		"\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
		"Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Styled\n", -1);
	write_file("font.ttf", "not really a font", -1);
	write_file("meta.txt", ";FFMETADATA1\ntitle=" HOSTILE_FILE_TITLE "\ncomment=-map 0:9 `id`\n[CHAPTER]\nTIMEBASE=1/1000\n"
		"START=0\nEND=500\ntitle=" HOSTILE_CHAPTER "\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=500\nEND=1000\ntitle=Part; two\n",
		-1);
	write_pgs();

	/* Video, three audio tracks (stereo English default, 5.1 French
	 * commentary, mono of a nonsense language), SubRip English, SubRip
	 * French forced and for the hard of hearing, ASS Japanese, PGS
	 * German, a font; two chapters; titles, tags, a file name of the kind
	 * that would break a command line. */
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-f", "lavfi", "-i", TONE("440"), "-f", "lavfi", "-i", TONE("550"),
		"-f", "lavfi", "-i", TONE("660"), "-i", "en.srt", "-i", "fr.srt", "-i", "styled.ass", "-i", "pgs.sup",
		"-i", "meta.txt", "-attach", "font.ttf",
		"-map", "0", "-map", "1", "-map", "2", "-map", "3", "-map", "4", "-map", "5", "-map", "6", "-map", "7",
		"-map_metadata", "8", "-map_chapters", "8",
		"-filter:v:0", BT709_FRAMES, "-c:v:0", "libx264", "-pix_fmt:v:0", "yuv420p", "-x264-params", BT709_X264,
		"-c:a:0", "aac", "-ac:a:0", "2", "-c:a:1", "ac3", "-ac:a:1", "6", "-c:a:2", "aac", "-ac:a:2", "1",
		"-c:s:0", "srt", "-c:s:1", "srt", "-c:s:2", "ass", "-c:s:3", "copy",
		"-metadata:s:v:0", "title=" HOSTILE_VIDEO_TITLE, "-metadata:s:v:0", "BPS=99999999",
		"-metadata:s:v:0", "NUMBER_OF_FRAMES=7",
		"-metadata:s:a:0", "language=eng", "-metadata:s:a:0", "title=" HOSTILE_AUDIO_TITLE,
		"-metadata:s:a:1", "language=fra", "-metadata:s:a:1", "title=Surround 5.1",
		"-metadata:s:a:2", "language=-y", "-metadata:s:a:2", "title=;rm -rf /",
		"-metadata:s:s:0", "language=eng", "-metadata:s:s:1", "language=fra", "-metadata:s:s:2", "language=jpn",
		"-metadata:s:s:3", "language=ger",
		"-metadata:s:t:0", "mimetype=application/x-truetype-font", "-metadata:s:t:0", "filename=" HOSTILE_FILENAME,
		"-disposition:v:0", "default", "-disposition:a:0", "default", "-disposition:a:1", "comment",
		"-disposition:a:2", "0", "-disposition:s:0", "0", "-disposition:s:1", "forced+hearing_impaired",
		"-disposition:s:2", "0", "-disposition:s:3", "0",
		"./" MULTI, NULL);

	/* HDR10, with static metadata, and HLG. */
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-f", "lavfi", "-i", TONE("440"),
		"-vf", "setparams=field_mode=prog:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv,"
		"format=yuv420p10le", "-c:v", "libx265", "-preset", "ultrafast", "-x265-params",
		"colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:"
		"master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50):max-cll=1100,350:"
		"log-level=error", "-c:a", "aac", "-metadata:s:a:0", "language=eng", "hdr10.mkv", NULL);
	ffmpeg("-f", "lavfi", "-i", PICTURE,
		"-vf", "setparams=field_mode=prog:color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc:range=tv,"
		"format=yuv420p10le", "-c:v", "libx265", "-preset", "ultrafast", "-x265-params",
		"colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:range=limited:log-level=error", "hlg.mkv", NULL);
	/* Full-range BT.601 (SMPTE 170M) and a picture that states no colour. */
	ffmpeg("-f", "lavfi", "-i", PICTURE,
		"-vf", "setparams=field_mode=prog:color_primaries=smpte170m:color_trc=smpte170m:colorspace=smpte170m:range=pc",
		"-c:v", "libx264", "-pix_fmt", "yuv420p", "-x264-params",
		"colorprim=smpte170m:transfer=smpte170m:colormatrix=smpte170m:range=pc", "sd-pc.mkv", NULL);
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-vf", "setparams=field_mode=prog", "-c:v", "libx264", "-pix_fmt", "yuv420p",
		"unstated.mkv", NULL);
	/* And one that states no colour at all, not even its range, as many
	 * older encodes do (unstated.mkv says "tv": ffmpeg gives the frames a
	 * limited range, and Matroska writes it).  x264 writes no video signal
	 * type for a limited range, so the raw stream states nothing; a copy of
	 * it into Matroska has nothing to say either.  No B-frames: the raw
	 * stream's timestamps are then its order. */
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-vf", "setparams=field_mode=prog", "-c:v", "libx264", "-bf", "0",
		"-pix_fmt", "yuv420p", "-f", "h264", "untagged.h264", NULL);
	ffmpeg("-framerate", "24", "-i", "untagged.h264", "-c", "copy", "untagged.mkv", NULL);
	/* No track marked default at all. */
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-f", "lavfi", "-i", TONE("440"), "-f", "lavfi", "-i", TONE("550"),
		"-i", "en.srt", "-i", "fr.srt", "-map", "0", "-map", "1", "-map", "2", "-map", "3", "-map", "4",
		"-filter:v:0", BT709_FRAMES, "-c:v", "libx264", "-pix_fmt", "yuv420p", "-x264-params", BT709_X264,
		"-c:a", "aac", "-c:s", "srt", "-disposition:v:0", "0", "-disposition:a:0", "0", "-disposition:a:1", "0",
		"-disposition:s:0", "0", "-disposition:s:1", "0", "nodefault.mkv", NULL);
	/* For MP4: 5.1 AAC English (default), stereo AAC French, SubRip
	 * English (default) and French (forced), chapters. */
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-f", "lavfi", "-i", TONE("440"), "-f", "lavfi", "-i", TONE("550"),
		"-i", "en.srt", "-i", "fr.srt", "-i", "meta.txt", "-map", "0", "-map", "1", "-map", "2", "-map", "3",
		"-map", "4", "-map_metadata", "5", "-map_chapters", "5",
		"-filter:v:0", BT709_FRAMES, "-c:v", "libx264", "-pix_fmt", "yuv420p", "-x264-params", BT709_X264,
		"-c:a", "aac", "-ac:a:0", "6", "-ac:a:1", "2", "-c:s", "srt",
		"-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=fra", "-metadata:s:s:0", "language=eng",
		"-metadata:s:s:1", "language=fra", "-disposition:a:0", "default", "-disposition:a:1", "0",
		"-disposition:s:0", "default", "-disposition:s:1", "forced", "for-mp4.mkv", NULL);
	/* Surround only (5.1 AC-3). */
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-f", "lavfi", "-i", TONE("440"), "-filter:v:0", BT709_FRAMES, "-c:v",
		"libx264", "-pix_fmt", "yuv420p", "-x264-params", BT709_X264, "-c:a", "ac3", "-ac", "6",
		"-metadata:s:a:0", "language=eng", "-disposition:a:0", "default", "surround.mkv", NULL);
	/* Cover pictures: an MP4 one, and one stored the way mkvmerge does (an
	 * image attachment). */
	ffmpeg("-f", "lavfi", "-i", "testsrc2=size=64x64:rate=1", "-frames:v", "1", "cover.png", NULL);
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-i", "cover.png", "-map", "0", "-map", "1", "-filter:v:0", BT709_FRAMES,
		"-c:v:0", "libx264", "-pix_fmt:v:0", "yuv420p", "-x264-params", BT709_X264, "-c:v:1", "png",
		"-disposition:v:1", "attached_pic", "cover.mp4", NULL);
	ffmpeg("-f", "lavfi", "-i", PICTURE, "-attach", "cover.png", "-map", "0", "-filter:v:0", BT709_FRAMES,
		"-c:v", "libx264", "-pix_fmt", "yuv420p", "-x264-params", BT709_X264, "-metadata:s:t:0", "mimetype=image/png",
		"-metadata:s:t:0", "filename=cover.png", "cover.mkv", NULL);
}

/* ── Running ────────────────────────────────────────────────────── */

static gchar *
tool_version(void)
{
	const gchar *argv[3] = { media.ffmpeg, "-version", NULL };
	g_autofree gchar *out = NULL;
	gint status = 0;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status, NULL))
		return g_strdup("?");
	return axis_convert_parse_version(out, strlen(out));
}

static gboolean
media_ready(void)
{
	if (media.tried)
		return media.ready;
	media.tried = TRUE;
	media.ffmpeg = g_find_program_in_path("ffmpeg");
	media.ffprobe = g_find_program_in_path("ffprobe");
	if (media.ffmpeg == NULL || media.ffprobe == NULL)
		return FALSE;
	media.dir = axis_test_storage_dir_new();
	media.version = tool_version();
	g_test_message("ffmpeg %s at %s", media.version, media.ffmpeg);
	make_clips();
	media.ready = TRUE;
	return TRUE;
}

#define SKIP_WITHOUT_MEDIA() do { if (!media_ready()) { g_test_skip("ffmpeg is not installed"); return; } } while (0)

static gint
open_clip(const gchar *name)
{
	g_autofree gchar *path = g_build_filename(media.dir, name, NULL);
	gint fd = open(path, O_RDONLY | O_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	return fd;
}

/* What the probe makes of a clip (its own calls, its own mapper). */
static AxisMediaInfo *
probe_clip(
	const gchar *name,
	gint64      *size
){
	const gchar *prefix[2] = { media.ffprobe, NULL };
	g_autofree gchar *error = NULL;
	AxisMediaInfo *info = NULL;
	struct stat st;
	gint fd = open_clip(name);

	if (!axis_probe_describe_fd(prefix, fd, 30000, NULL, &info, &error))
		g_error("the probe could not read %s: %s", name, error);
	g_assert_cmpint(fstat(fd, &st), ==, 0);
	if (size != NULL)
		*size = (gint64)st.st_size;
	close(fd);
	return info;
}

/* Software capabilities as the check measures them, or every encoder
 * claimed capable (the plans of the hardware tests only run where the
 * check found the hardware). */
static const AxisEncoderCapabilities *
caps_for_software(void)
{
	if (media.all == NULL)
		media.all = axis_test_convert_caps_all();
	return media.all;
}

typedef struct
{
	AxisConvertProgress progress;
	gchar              *output;
	gchar              *errors;
} Ran;

static void
ran_clear(Ran *r)
{
	if (r->output != NULL)
		(void)g_unlink(r->output);
	g_free(r->output);
	g_free(r->errors);
	memset(r, 0, sizeof(*r));
}

/* Runs @plan on clip @name as a conversion runs it: the source as
 * descriptor 3, a new file of ours as descriptor 4, under the runner's
 * limits.  FALSE (with what ffmpeg said) when it fails. */
static gboolean
convert(
	const AxisConvertPlan *plan,
	const gchar           *name,
	Ran                   *ran
){
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autofree gchar *out_name = g_strdup_printf("out-%u.%s", g_random_int(),
		plan->container == AXIS_CONTAINER_MP4 ? "mp4" : "mkv");
	AxisProbeRunSpec spec;
	AxisProbeRunResult result;
	gint in_fd = open_clip(name);
	gint out_fd;
	guint i;
	gboolean ok;

	memset(ran, 0, sizeof(*ran));
	ran->output = g_build_filename(media.dir, out_name, NULL);
	out_fd = open(ran->output, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	g_assert_cmpint(out_fd, >=, 0);
	g_ptr_array_add(argv, media.ffmpeg);
	for (i = 0; plan->args[i] != NULL; i++)
		g_ptr_array_add(argv, plan->args[i]);
	g_ptr_array_add(argv, NULL);

	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv->pdata;
	spec.fd = in_fd;
	spec.output_fd = out_fd;
	spec.file_size_limit = G_GUINT64_CONSTANT(268435456);
	spec.timeout_ms = RUN_TIMEOUT_MS;
	spec.output_cap = 1024 * 1024;
	spec.error_cap = 16384;
	spec.memory_limit = plan->encoder->family == AXIS_ENCODER_SOFTWARE ? G_GUINT64_CONSTANT(8589934592) : 0;
	spec.working_directory = media.dir;
	spec.cpu_seconds = RUN_TIMEOUT_MS / 1000 * 16 + 60;
	axis_probe_run(&spec, &result);
	axis_convert_progress_init(&ran->progress, plan->duration_ms, plan->frame_rate);
	axis_convert_progress_feed(&ran->progress, (const gchar *)result.output->data, result.output->len);
	ran->errors = g_strndup((const gchar *)result.errors->data, result.errors->len);
	ok = result.status == AXIS_PROBE_RUN_OK;
	axis_probe_run_result_clear(&result);
	close(in_fd);
	close(out_fd);
	return ok;
}

static AxisMediaInfo *
probe_output(const Ran *ran)
{
	const gchar *prefix[2] = { media.ffprobe, NULL };
	g_autofree gchar *error = NULL;
	AxisMediaInfo *info = NULL;
	gint fd = open(ran->output, O_RDONLY | O_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	if (!axis_probe_describe_fd(prefix, fd, 30000, NULL, &info, &error))
		g_error("the probe could not read the output: %s", error);
	close(fd);
	return info;
}

/* Converts clip @name with @plan and asserts the output is exactly what
 * the plan says; returns the probe of the output. */
static AxisMediaInfo *
convert_and_check(
	const AxisConvertPlan *plan,
	const gchar           *name,
	Ran                   *ran
){
	g_autoptr(GPtrArray) mismatches = g_ptr_array_new_with_free_func(g_free);
	AxisMediaInfo *output;
	gdouble fraction = -1;
	guint i;

	if (!convert(plan, name, ran)) {
		g_autofree gchar *line = g_strjoinv(" ", plan->args);

		g_error("ffmpeg %s failed on %s: %s\narguments: %s", media.version, name, ran->errors, line);
	}
	output = probe_output(ran);
	if (!axis_convert_plan_check_output(plan, output, mismatches)) {
		for (i = 0; i < mismatches->len; i++)
			g_test_message("mismatch: %s", (const gchar *)g_ptr_array_index(mismatches, i));
		g_error("ffmpeg %s: the output of %s is not what the plan says: %s", media.version, name,
			(const gchar *)g_ptr_array_index(mismatches, 0));
	}
	/* The progress said where it was, and that it ended. */
	g_assert_true(ran->progress.ended);
	g_assert_cmpuint(ran->progress.blocks, >=, 1);
	g_assert_cmpint(ran->progress.frame, ==, 24);
	g_assert_true(axis_convert_progress_fraction(&ran->progress, &fraction));
	g_assert_cmpfloat_with_epsilon(fraction, 1.0, 0.05);
	return output;
}

/* The output's HDR10 values are in the picture's bitstream itself (not
 * only in the container, from which a decoder hands them to frames). */
static void
assert_in_bitstream(
	const AxisConvertPlan *plan,
	const Ran             *ran
){
	const gchar *ffmpeg_prefix[2] = { media.ffmpeg, NULL };
	const gchar *ffprobe_prefix[2] = { media.ffprobe, NULL };
	g_autofree gchar *why = NULL;
	AxisCapabilityTool tool;
	gint fd = open(ran->output, O_RDONLY | O_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = ffmpeg_prefix;
	tool.ffprobe = ffprobe_prefix;
	tool.scratch = media.dir;
	why = axis_convert_check_bitstream(&tool, plan, fd);
	close(fd);
	if (why != NULL)
		g_error("ffmpeg %s, %s: %s", media.version, plan->encoder->name, why);
}

static void
definition(
	const gchar           *slug,
	AxisProfileDefinition *def
){
	g_assert_true(axis_profile_definition_parse(axis_profile_builtin(slug)->definition, -1, def, NULL));
}

static AxisConvertPlan *
plan_for(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	gint64                         size,
	AxisEncoderFamily              family,
	const AxisEncoderCapabilities *caps
){
	AxisConvertOptions options;
	AxisConvertPlan *plan = NULL;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;

	memset(&options, 0, sizeof(options));
	options.software_preset = "ultrafast";
	options.nvenc_preset = "p1";
	options.threads = 2;
	if (!axis_convert_plan_build(def, info, size, family, caps, &options, &plan, &refusal))
		g_error("no plan: %s", axis_convert_refusal_describe(refusal));
	return plan;
}

/* What ffprobe itself prints about the output's streams (flags the probe
 * does not record, tags), one line per stream. */
static gchar *
raw_streams(const gchar *path)
{
	const gchar *argv[] = {
		media.ffprobe, "-v", "error", "-show_entries", "stream=index:stream_disposition=comment,hearing_impaired:"
		"stream_tags=BPS,NUMBER_OF_FRAMES,filename,mimetype", "-of", "compact=p=0", path, NULL
	};
	gchar *out = NULL;
	gint status = 0;

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
		&status, NULL));
	return out;
}

static gchar *
raw_format(const gchar *path)
{
	const gchar *argv[] = {
		media.ffprobe, "-v", "error", "-show_entries", "format_tags=title:chapter_tags=title", "-of", "compact=p=0",
		path, NULL
	};
	gchar *out = NULL;
	gint status = 0;

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
		&status, NULL));
	return out;
}

static const AxisMediaStream *
stream_of(
	const AxisMediaInfo *info,
	guint                i
){
	g_assert_cmpuint(i, <, info->streams->len);
	return g_ptr_array_index(info->streams, i);
}

/* ── Streams, flags, attachments, chapters, tags ────────────────── */

/* Spec: All kept streams present (two audio and two text subtitle streams
 * and a font are a part of this file), in order, with their flags, and
 * the chapters and tags; Spec: Hostile names and tags (the file's name,
 * titles, tags, chapter titles and an attachment's name are data that
 * arrives unchanged, never an argument). */
static void
test_multi_track(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autofree gchar *raw = NULL;
	g_autofree gchar *format = NULL;
	g_auto(GStrv) lines = NULL;
	AxisProfileDefinition def;
	Ran ran;
	gint64 size = 0;
	guint i;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip(MULTI, &size);
	g_assert_cmpuint(info->streams->len, ==, 9);
	definition("space_saver", &def);
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	for (i = 0; plan->args[i] != NULL; i++) {
		g_assert_null(strstr(plan->args[i], "multi"));
		g_assert_null(strstr(plan->args[i], "rm -rf"));
		g_assert_null(strstr(plan->args[i], "evil"));
		g_assert_null(strstr(plan->args[i], "reboot"));
		g_assert_null(strstr(plan->args[i], "passwd"));
	}
	out = convert_and_check(plan, MULTI, &ran);

	/* Exactly the planned streams, in order: picture, three audio, four
	 * subtitles, the font. */
	g_assert_cmpuint(out->streams->len, ==, 9);
	g_assert_cmpstr(stream_of(out, 0)->codec, ==, "hevc");
	g_assert_cmpint(stream_of(out, 0)->bit_depth, ==, 10);
	g_assert_cmpstr(stream_of(out, 1)->codec, ==, "aac");
	g_assert_cmpstr(stream_of(out, 2)->codec, ==, "ac3");
	g_assert_cmpint(stream_of(out, 2)->channels, ==, 6);
	g_assert_cmpstr(stream_of(out, 5)->codec, ==, "subrip");
	g_assert_true(stream_of(out, 5)->is_forced);
	g_assert_cmpstr(stream_of(out, 6)->codec, ==, "ass");
	g_assert_cmpstr(stream_of(out, 7)->codec, ==, "hdmv_pgs_subtitle");
	g_assert_cmpint(stream_of(out, 8)->kind, ==, AXIS_STREAM_ATTACHMENT);
	g_assert_true(stream_of(out, 1)->is_default);
	g_assert_false(stream_of(out, 2)->is_default);
	g_assert_false(stream_of(out, 4)->is_default);
	/* The text of the file, carried as data. */
	g_assert_cmpstr(stream_of(out, 0)->title, ==, HOSTILE_VIDEO_TITLE);
	g_assert_cmpstr(stream_of(out, 1)->title, ==, HOSTILE_AUDIO_TITLE);
	g_assert_cmpstr(stream_of(out, 3)->language, ==, "-y");
	g_assert_cmpstr(out->tag_title, ==, HOSTILE_FILE_TITLE);
	g_assert_cmpint(out->chapter_count, ==, 2);

	/* What the probe does not record: the other flags were kept (relative
	 * dispositions), the stale statistics tags were not, the font's name
	 * and type came through, and the chapter titles. */
	raw = raw_streams(ran.output);
	lines = g_strsplit(raw, "\n", -1);
	g_assert_cmpuint(g_strv_length(lines), >=, 9);
	g_assert_null(strstr(lines[0], "BPS="));
	g_assert_null(strstr(lines[0], "99999999"));
	g_assert_null(strstr(lines[0], "NUMBER_OF_FRAMES=7"));
	g_assert_nonnull(strstr(lines[2], "disposition:comment=1"));
	g_assert_nonnull(strstr(lines[5], "disposition:hearing_impaired=1"));
	g_assert_nonnull(strstr(lines[8], "tag:filename=" HOSTILE_FILENAME));
	g_assert_nonnull(strstr(lines[8], "tag:mimetype=application/x-truetype-font"));
	format = raw_format(ran.output);
	g_assert_nonnull(strstr(format, "tag:title=" HOSTILE_CHAPTER));
	g_assert_nonnull(strstr(format, "tag:title=Part; two"));
	ran_clear(&ran);
}

/* Spec: Dropped stream is explained: with only English kept, the French
 * track is not in the output and the plan says why; a track of no known
 * language is kept. */
static void
test_language_rule(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	Ran ran;
	gint64 size = 0;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip(MULTI, &size);
	definition("space_saver", &def);
	memcpy(def.languages[0], "eng", 4);
	def.n_languages = 1;
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	g_assert_cmpuint(plan->dropped->len, ==, 2);
	out = convert_and_check(plan, MULTI, &ran);
	/* English stays, French goes; "-y" is not a language anyone keeps or
	 * drops by name: dropped too, being known text not on the list. */
	g_assert_cmpuint(out->audio_streams, ==, 1);
	g_assert_cmpstr(stream_of(out, 1)->language, ==, "eng");
	ran_clear(&ran);
}

/* Spec: Added stereo track, in the order the profile states: after the
 * original (Balanced, Matroska), and first (Maximum compatibility, MP4). */
static void
test_added_stereo(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	Ran ran;
	gint64 size = 0;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("surround.mkv", &size);
	definition("balanced", &def);
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "surround.mkv", &ran);
	g_assert_cmpuint(out->streams->len, ==, 3);
	g_assert_cmpstr(stream_of(out, 1)->codec, ==, "ac3");
	g_assert_cmpint(stream_of(out, 1)->channels, ==, 6);
	g_assert_true(stream_of(out, 1)->is_default);
	g_assert_cmpstr(stream_of(out, 2)->codec, ==, "aac");
	g_assert_cmpint(stream_of(out, 2)->channels, ==, 2);
	g_assert_false(stream_of(out, 2)->is_default);
	g_assert_cmpstr(stream_of(out, 2)->language, ==, "eng");
	g_assert_cmpint(out->bit_depth, ==, 8);
	ran_clear(&ran);
	g_clear_pointer(&out, axis_media_info_free);
	g_clear_pointer(&plan, axis_convert_plan_free);

	def.audio = AXIS_AUDIO_STEREO_FIRST;
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "surround.mkv", &ran);
	g_assert_cmpstr(stream_of(out, 1)->codec, ==, "aac");
	g_assert_true(stream_of(out, 1)->is_default);
	g_assert_cmpstr(stream_of(out, 2)->codec, ==, "ac3");
	g_assert_false(stream_of(out, 2)->is_default);
	ran_clear(&ran);
}

/* Maximum compatibility: H.264 in MP4, stereo first, SubRip as mov_text
 * with its forced flag, MP4's own default rule, the chapters and MP4's
 * chapter track. */
static void
test_mp4_target(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	Ran ran;
	gint64 size = 0;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("for-mp4.mkv", &size);
	definition("max_compat", &def);
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "for-mp4.mkv", &ran);
	g_assert_cmpstr(out->container, ==, "mov,mp4,m4a,3gp,3g2,mj2");
	/* Video, stereo AAC (new), 5.1 AAC, stereo AAC French, two mov_text,
	 * the chapter track. */
	g_assert_cmpuint(out->streams->len, ==, 7);
	g_assert_cmpstr(stream_of(out, 0)->codec, ==, "h264");
	g_assert_cmpint(stream_of(out, 1)->channels, ==, 2);
	g_assert_true(stream_of(out, 1)->is_default);
	g_assert_cmpint(stream_of(out, 2)->channels, ==, 6);
	g_assert_false(stream_of(out, 2)->is_default);
	g_assert_cmpstr(stream_of(out, 4)->codec, ==, "mov_text");
	g_assert_true(stream_of(out, 4)->is_default);
	g_assert_true(stream_of(out, 5)->is_forced);
	g_assert_cmpint(stream_of(out, 6)->kind, ==, AXIS_STREAM_DATA);
	g_assert_cmpint(out->chapter_count, ==, 2);
	ran_clear(&ran);
}

/* No track marked default: none is in the output (ffmpeg would mark the
 * first itself without the explicit dispositions). */
static void
test_no_default_flags(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	Ran ran;
	gint64 size = 0;
	guint i;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("nodefault.mkv", &size);
	definition("space_saver", &def);
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "nodefault.mkv", &ran);
	for (i = 0; i < out->streams->len; i++)
		g_assert_false(stream_of(out, i)->is_default);
	ran_clear(&ran);
}

/* Covers: carried into MP4; refused for Matroska, where ffmpeg would write
 * one as a video track. */
static void
test_covers(void)
{
	g_autoptr(AxisMediaInfo) mp4 = NULL;
	g_autoptr(AxisMediaInfo) mkv = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition compat;
	AxisProfileDefinition space;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	AxisConvertPlan *none = NULL;
	Ran ran;
	gint64 size = 0;

	SKIP_WITHOUT_MEDIA();
	mp4 = probe_clip("cover.mp4", &size);
	definition("max_compat", &compat);
	plan = plan_for(&compat, mp4, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "cover.mp4", &ran);
	g_assert_true(stream_of(out, 1)->attached_pic);
	g_assert_cmpstr(stream_of(out, 1)->codec, ==, "png");
	ran_clear(&ran);

	mkv = probe_clip("cover.mkv", &size);
	g_assert_true(stream_of(mkv, 1)->attached_pic);
	definition("space_saver", &space);
	g_assert_false(axis_convert_plan_build(&space, mkv, size, AXIS_ENCODER_SOFTWARE, caps_for_software(), NULL, &none,
		&refusal));
	g_assert_null(none);
	g_assert_cmpint(refusal, ==, AXIS_CONVERT_REFUSE_COVER_IN_MATROSKA);
}

/* ── Colour and HDR ─────────────────────────────────────────────── */

/* Spec: HDR10: 10-bit, PQ, BT.2020, the source's mastering display and
 * content light values, exactly and in the bitstream. */
static void
test_hdr10(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	const AxisMediaStream *v;
	Ran ran;
	gint64 size = 0;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("hdr10.mkv", &size);
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(info->hdr_uncertain);
	definition("balanced", &def);
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "hdr10.mkv", &ran);
	v = stream_of(out, 0);
	g_assert_cmpint(v->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(v->hdr_uncertain);
	g_assert_cmpint(v->bit_depth, ==, 10);
	g_assert_cmpstr(v->color_transfer, ==, "smpte2084");
	g_assert_cmpstr(v->color_primaries, ==, "bt2020");
	g_assert_cmpstr(v->color_space, ==, "bt2020nc");
	g_assert_cmpstr(v->color_range, ==, "tv");
	g_assert_true(v->hdr10.mastering);
	g_assert_cmpint(v->hdr10.red_x, ==, 34000);
	g_assert_cmpint(v->hdr10.green_y, ==, 34500);
	g_assert_cmpint(v->hdr10.white_x, ==, 15635);
	g_assert_cmpint(v->hdr10.min_luminance, ==, 50);
	g_assert_cmpint(v->hdr10.max_luminance, ==, 40000000);
	g_assert_cmpint(v->hdr10.max_content, ==, 1100);
	g_assert_cmpint(v->hdr10.max_average, ==, 350);
	g_assert_true(v->signals.mastering_in_frames);
	g_assert_true(v->signals.light_in_frames);
	g_assert_false(v->signals.hdr10_conflict);
	assert_in_bitstream(plan, &ran);
	ran_clear(&ran);
}

/* Spec: HLG stays HLG. */
static void
test_hlg(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autoptr(AxisMediaInfo) out = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	Ran ran;
	gint64 size = 0;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("hlg.mkv", &size);
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_HLG);
	definition("space_saver", &def);
	plan = plan_for(&def, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
	out = convert_and_check(plan, "hlg.mkv", &ran);
	g_assert_cmpint(stream_of(out, 0)->hdr_format, ==, AXIS_HDR_HLG);
	g_assert_false(stream_of(out, 0)->hdr_uncertain);
	g_assert_cmpstr(stream_of(out, 0)->color_transfer, ==, "arib-std-b67");
	g_assert_false(stream_of(out, 0)->hdr10.mastering);
	ran_clear(&ran);
}

/* Spec: SDR colour: the four properties equal the source's, with x265 and
 * with x264 (full-range BT.601 here, the BT.709 film above); and unknown
 * colour stays unstated. */
static void
test_sdr_colour(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	AxisProfileDefinition space;
	AxisProfileDefinition compat;
	gint64 size = 0;
	guint which;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("sd-pc.mkv", &size);
	definition("space_saver", &space);
	definition("max_compat", &compat);
	for (which = 0; which < 2; which++) {
		g_autoptr(AxisConvertPlan) plan = plan_for(which == 0 ? &space : &compat, info, size, AXIS_ENCODER_SOFTWARE,
			caps_for_software());
		g_autoptr(AxisMediaInfo) out = NULL;
		const AxisMediaStream *v;
		Ran ran;

		out = convert_and_check(plan, "sd-pc.mkv", &ran);
		v = stream_of(out, 0);
		g_assert_cmpstr(v->color_range, ==, "pc");
		g_assert_cmpstr(v->color_space, ==, "smpte170m");
		g_assert_cmpstr(v->color_transfer, ==, "smpte170m");
		g_assert_cmpstr(v->color_primaries, ==, "smpte170m");
		g_assert_cmpint(v->hdr_format, ==, AXIS_HDR_SDR);
		ran_clear(&ran);
	}
	g_clear_pointer(&info, axis_media_info_free);

	info = probe_clip("unstated.mkv", &size);
	{
		g_autoptr(AxisConvertPlan) plan = plan_for(&compat, info, size, AXIS_ENCODER_SOFTWARE, caps_for_software());
		g_autoptr(AxisMediaInfo) out = NULL;
		const AxisConvertStream *e;
		Ran ran;

		g_assert_cmpuint(plan->streams->len, >, 0);
		e = g_ptr_array_index(plan->streams, 0);
		g_assert_null(e->color_primaries);
		g_assert_null(e->color_transfer);
		out = convert_and_check(plan, "unstated.mkv", &ran);
		g_assert_true(stream_of(out, 0)->color_primaries == NULL ||
			strcmp(stream_of(out, 0)->color_primaries, "unknown") == 0);
		ran_clear(&ran);
	}
}

/* The output of a picture that states no colour: both checks accept it
 * (the output's, 3, in convert_and_check(), and the picture's, 4, here);
 * its range is limited, stated or not, and the other three unstated. */
static void
assert_untagged_output(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *out
){
	const AxisMediaStream *v = stream_of(out, 0);
	g_autofree gchar *why = NULL;

	if (!axis_convert_plan_check_picture(plan, out, &why))
		g_error("ffmpeg %s, %s: the picture check rejects the output of a picture that states no colour: %s",
			media.version, plan->encoder->name, why);
	g_assert_null(why);
	g_assert_true(axis_convert_colour_same("range", NULL, v->color_range));
	g_assert_true(axis_convert_colour_same("matrix", NULL, v->color_space));
	g_assert_true(axis_convert_colour_same("transfer", NULL, v->color_transfer));
	g_assert_true(axis_convert_colour_same("primaries", NULL, v->color_primaries));
	g_test_message("ffmpeg %s, %s, from a picture that states no colour: range %s, %d bits, %s%s", media.version,
		plan->encoder->name, axis_convert_colour_said("range", v->color_range), v->bit_depth,
		axis_hdr_format_label(v->hdr_format), v->hdr_uncertain ? " (uncertain)" : "");
}

/* Spec: a picture that states none of its colour properties (the probe
 * records all four unstated) is converted by Space saver with x265 and by
 * Maximum compatibility with x264, and both checks accept the outputs:
 * ffmpeg's encoders write a limited range for frames that state none, and
 * that is the same picture. */
static void
test_untagged_colour(void)
{
	g_autoptr(AxisMediaInfo) info = NULL;
	AxisProfileDefinition space;
	AxisProfileDefinition compat;
	const AxisMediaStream *v;
	gint64 size = 0;
	guint which;

	SKIP_WITHOUT_MEDIA();
	info = probe_clip("untagged.mkv", &size);
	v = stream_of(info, 0);
	/* The clip is what it is for: nothing stated, SDR, certain. */
	g_assert_null(v->color_range);
	g_assert_null(v->color_space);
	g_assert_null(v->color_transfer);
	g_assert_null(v->color_primaries);
	g_assert_cmpint(v->bit_depth, ==, 8);
	g_assert_cmpint(v->hdr_format, ==, AXIS_HDR_SDR);
	g_assert_false(info->hdr_uncertain);
	definition("space_saver", &space);
	definition("max_compat", &compat);
	for (which = 0; which < 2; which++) {
		g_autoptr(AxisConvertPlan) plan = plan_for(which == 0 ? &space : &compat, info, size, AXIS_ENCODER_SOFTWARE,
			caps_for_software());
		g_autoptr(AxisMediaInfo) out = NULL;
		const AxisConvertStream *e = g_ptr_array_index(plan->streams, 0);
		Ran ran;

		g_assert_null(e->color_range);
		g_assert_null(e->color_space);
		g_assert_null(e->color_transfer);
		g_assert_null(e->color_primaries);
		out = convert_and_check(plan, "untagged.mkv", &ran);
		assert_untagged_output(plan, out);
		if (which == 0)
			g_assert_cmpint(stream_of(out, 0)->bit_depth, ==, 10);
		ran_clear(&ran);
	}
}

/* ── The capability check, and the hardware it finds ────────────── */

#define HARDWARE_FAMILIES (AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_NVENC) | AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_VAAPI))

/* The hardware families the tests leave alone, from AXIS_TEST_HWENC:
 * unset, empty or 0 is both; 1 or all is neither; otherwise a comma list of
 * the families to run (nvenc, vaapi). */
static guint
hardware_skipped(void)
{
	const gchar *value = g_getenv("AXIS_TEST_HWENC");
	g_auto(GStrv) words = NULL;
	guint skip = HARDWARE_FAMILIES;
	guint i;

	if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0)
		return skip;
	if (strcmp(value, "1") == 0 || strcmp(value, "all") == 0)
		return 0;
	words = g_strsplit(value, ",", -1);
	for (i = 0; words[i] != NULL; i++) {
		if (strcmp(words[i], "nvenc") == 0)
			skip &= ~AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_NVENC);
		else if (strcmp(words[i], "vaapi") == 0)
			skip &= ~AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_VAAPI);
		else
			g_error("AXIS_TEST_HWENC=%s: expected 0, 1, all, or a comma list of nvenc and vaapi", value);
	}
	return skip;
}

static const AxisEncoderCapabilities *
measured(void)
{
	if (media.caps == NULL) {
		g_autofree gchar *scratch = g_build_filename(media.dir, "check", NULL);
		const gchar *ffmpeg_prefix[2] = { media.ffmpeg, NULL };
		const gchar *ffprobe_prefix[2] = { media.ffprobe, NULL };
		g_autoptr(GError) error = NULL;
		g_autoptr(GString) record = g_string_new(NULL);
		const AxisConvertEncoder *table = axis_convert_encoders();
		AxisCapabilityTool tool;
		gint64 started = g_get_monotonic_time();
		guint i;

		g_assert_cmpint(g_mkdir(scratch, 0700), ==, 0);
		memset(&tool, 0, sizeof(tool));
		tool.ffmpeg = ffmpeg_prefix;
		tool.ffprobe = ffprobe_prefix;
		tool.scratch = scratch;
		tool.width = 320;
		tool.height = 240;
		tool.record = record;
		tool.skip_families = hardware_skipped();
		media.caps = axis_convert_capabilities_measure(&tool, &error);
		g_assert_no_error(error);
		g_assert_nonnull(media.caps);
		g_test_message("capability check of ffmpeg %s: %.1f s", media.caps->tool_version,
			(gdouble)(g_get_monotonic_time() - started) / G_USEC_PER_SEC);
		for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
			const AxisEncoderCapability *c = &media.caps->encoders[i];

			g_test_message("  %-11s usable=%d 10-bit=%d HDR10=%d %s", table[i].name, c->usable,
				c->ten_bit, c->hdr10, c->reason != NULL ? c->reason : "");
			/* A family left alone was not run, not even once. */
			if ((tool.skip_families & AXIS_CONVERT_FAMILY_BIT(table[i].family)) != 0) {
				g_assert_false(c->measured);
				g_assert_null(strstr(record->str, table[i].name));
			}
		}
		if (tool.skip_families != 0)
			g_test_message("hardware families left alone:%s%s (AXIS_TEST_HWENC=1, or make check-hwenc, runs them)",
				(tool.skip_families & AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_NVENC)) != 0 ? " nvenc" : "",
				(tool.skip_families & AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_VAAPI)) != 0 ? " vaapi" : "");
		/* It made and removed what it needed: the directory is empty. */
		{
			g_autoptr(GDir) d = g_dir_open(scratch, 0, NULL);

			g_assert_nonnull(d);
			g_assert_null(g_dir_read_name(d));
		}
	}
	return media.caps;
}

/* The check against the installed ffmpeg: x265 carries 10 bits and HDR10,
 * x264 8 bits; the version and digest are those of the binary. */
static void
test_capabilities_real(void)
{
	const AxisEncoderCapabilities *caps;
	const AxisEncoderCapability *x265;
	const AxisEncoderCapability *x264;

	SKIP_WITHOUT_MEDIA();
	caps = measured();
	g_assert_cmpstr(caps->tool_version, ==, media.version);
	g_assert_cmpuint(strlen(caps->tool_digest), ==, 64);
	g_assert_true(caps->aac);
	g_assert_true(caps->mov_text);
	g_assert_true(caps->subrip);
	x265 = axis_encoder_capabilities_get(caps, axis_convert_encoder_named("libx265"));
	x264 = axis_encoder_capabilities_get(caps, axis_convert_encoder_named("libx264"));
	if (!x265->hdr10)
		g_error("libx265 is not shown to carry HDR10: %s", x265->reason != NULL ? x265->reason : "?");
	g_assert_true(x265->usable && x265->ten_bit && x265->hdr10);
	g_assert_true(x264->usable);
	g_assert_false(x264->ten_bit);
}

/* The hardware families, for real, wherever the check found them (on a
 * machine without them: skipped, and said), when AXIS_TEST_HWENC asks. */
static void
test_hardware_found(void)
{
	const AxisEncoderCapabilities *caps;
	const AxisConvertEncoder *table = axis_convert_encoders();
	guint ran_any = 0;
	guint i;

	SKIP_WITHOUT_MEDIA();
	if (hardware_skipped() == HARDWARE_FAMILIES) {
		g_test_skip("hardware encodes are off: AXIS_TEST_HWENC=1 (make check-hwenc) runs them");
		return;
	}
	caps = measured();
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		const AxisConvertEncoder *e = &table[i];
		const AxisEncoderCapability *c = &caps->encoders[i];
		g_autoptr(AxisMediaInfo) info = NULL;
		g_autoptr(AxisConvertPlan) plan = NULL;
		g_autoptr(AxisMediaInfo) out = NULL;
		AxisProfileDefinition def;
		Ran ran;
		gint64 size = 0;

		if (e->family == AXIS_ENCODER_SOFTWARE || !c->usable)
			continue;
		/* The multi-track film, with the measured capabilities. */
		info = probe_clip(MULTI, &size);
		definition(e->codec == AXIS_TCODEC_HEVC ? (c->ten_bit ? "space_saver" : "balanced") : "max_compat", &def);
		if (e->codec == AXIS_TCODEC_H264) {
			g_clear_pointer(&info, axis_media_info_free);
			info = probe_clip("for-mp4.mkv", &size);
		}
		def.encoders[0] = e->family;
		def.n_encoders = 1;
		plan = plan_for(&def, info, size, e->family, caps);
		out = convert_and_check(plan, e->codec == AXIS_TCODEC_H264 ? "for-mp4.mkv" : MULTI, &ran);
		g_test_message("%s produced a file on this machine (%s)", e->name, media.version);
		ran_clear(&ran);
		ran_any++;
		/* A picture that states no colour: what this encoder writes for
		 * it (NVENC: a limited range, on 7.1 and 8.1) is the same picture
		 * to both checks. */
		{
			g_autoptr(AxisMediaInfo) bare = probe_clip("untagged.mkv", &size);
			g_autoptr(AxisConvertPlan) bare_plan = plan_for(&def, bare, size, e->family, caps);
			g_autoptr(AxisMediaInfo) bare_out = convert_and_check(bare_plan, "untagged.mkv", &ran);

			assert_untagged_output(bare_plan, bare_out);
			ran_clear(&ran);
		}
		if (!e->ten_bit)
			continue;
		if (!c->hdr10) {
			g_test_message("%s: HDR10 not shown: %s", e->name, c->reason != NULL ? c->reason : "?");
			continue;
		}
		/* HDR10 too, only where it was shown to carry it. */
		{
			g_autoptr(AxisMediaInfo) hdr = probe_clip("hdr10.mkv", &size);
			g_autoptr(AxisConvertPlan) hdr_plan = NULL;
			g_autoptr(AxisMediaInfo) hdr_out = NULL;

			hdr_plan = plan_for(&def, hdr, size, e->family, caps);
			hdr_out = convert_and_check(hdr_plan, "hdr10.mkv", &ran);
			assert_in_bitstream(hdr_plan, &ran);
			g_test_message("%s carried HDR10 on this machine", e->name);
			ran_clear(&ran);
		}
	}
	if (ran_any == 0)
		g_test_skip("no hardware encoder was found capable on this machine");
}

static void
cleanup(void)
{
	if (media.dir != NULL)
		axis_test_storage_dir_free(media.dir);
	g_free(media.ffmpeg);
	g_free(media.ffprobe);
	g_free(media.version);
	axis_encoder_capabilities_free(media.caps);
	axis_encoder_capabilities_free(media.all);
}

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/convert/real/multi-track", test_multi_track);
	g_test_add_func("/convert/real/language-rule", test_language_rule);
	g_test_add_func("/convert/real/added-stereo", test_added_stereo);
	g_test_add_func("/convert/real/mp4-target", test_mp4_target);
	g_test_add_func("/convert/real/no-default-flags", test_no_default_flags);
	g_test_add_func("/convert/real/covers", test_covers);
	g_test_add_func("/convert/real/hdr10", test_hdr10);
	g_test_add_func("/convert/real/hlg", test_hlg);
	g_test_add_func("/convert/real/sdr-colour", test_sdr_colour);
	g_test_add_func("/convert/real/untagged-colour", test_untagged_colour);
	g_test_add_func("/convert/real/capabilities", test_capabilities_real);
	g_test_add_func("/convert/real/hardware-found", test_hardware_found);

	status = g_test_run();
	cleanup();
	return status;
}
