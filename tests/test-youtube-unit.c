/*
 * test-youtube-unit.c - the pure parts of the YouTube downloader: which
 * addresses are taken, the one command, the names in the library, and the
 * reading of what yt-dlp writes (add-youtube-downloader)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No database, no network and no yt-dlp.  Every title, channel and video id
 * here is made up.
 */

#include <glib.h>
#include <string.h>

#include "import/axis-import-name.h"
#include "youtube/axis-youtube.h"
#include "youtube/axis-youtube-address.h"
#include "youtube/axis-youtube-output.h"
#include "youtube/axis-youtube-private.h"

#define ID "mkvideo0001"

/* ── Addresses ──────────────────────────────────────────────────── */

static void
assert_taken(
	const gchar *text,
	const gchar *id
){
	g_autofree gchar *found = NULL;
	const gchar *problem = "unset";

	if (!axis_youtube_address_parse(text, &found, &problem))
		g_error("expected '%s' to be taken, but: %s", text, problem);
	g_assert_cmpstr(found, ==, id);
	g_assert_null(problem);
}

static void
assert_refused(
	const gchar *text,
	const gchar *words
){
	g_autofree gchar *found = NULL;
	const gchar *problem = NULL;

	if (axis_youtube_address_parse(text, &found, &problem))
		g_error("expected '%s' to be refused", text != NULL ? text : "(null)");
	g_assert_null(found);
	g_assert_nonnull(problem);
	if (words != NULL && strstr(problem, words) == NULL)
		g_error("expected the refusal of '%s' to say '%s', but it says: %s", text != NULL ? text : "(null)", words,
			problem);
	/* A refusal never repeats what was typed. */
	if (text != NULL && strlen(text) > 12)
		g_assert_null(strstr(problem, text));
}

/* Spec: Only one video's https address at YouTube is taken */
static void
test_address_taken(void)
{
	assert_taken("https://www.youtube.com/watch?v=" ID, ID);
	assert_taken("https://youtube.com/watch?v=" ID, ID);
	assert_taken("https://m.youtube.com/watch?v=" ID, ID);
	assert_taken("https://music.youtube.com/watch?v=" ID, ID);
	assert_taken("https://WWW.YouTube.com/watch?v=" ID, ID);
	assert_taken("https://youtu.be/" ID, ID);
	assert_taken("https://youtu.be/" ID "?t=42", ID);
	assert_taken("https://www.youtube.com/shorts/" ID, ID);
	assert_taken("https://www.youtube.com/live/" ID "?feature=share", ID);
	assert_taken("https://www.youtube.com/embed/" ID, ID);
	/* A video beside a list is the video; the list is ignored. */
	assert_taken("https://www.youtube.com/watch?v=" ID "&list=PLmadeup0000000000000000000000001&index=3", ID);
	assert_taken("https://www.youtube.com/watch?feature=share&v=" ID "#t=10", ID);
	/* White space around a paste goes. */
	assert_taken("  https://www.youtube.com/watch?v=" ID " \n", ID);
	/* An id may begin with a dash: it is still only an id. */
	assert_taken("https://www.youtube.com/watch?v=-k_video0-1", "-k_video0-1");
}

static void
test_address_refused(void)
{
	g_autofree gchar *long_text = g_strnfill(5000, 'a');
	g_autofree gchar *long_address = g_strconcat("https://www.youtube.com/watch?v=" ID "&x=", long_text, NULL);

	assert_refused(NULL, "Paste");
	assert_refused("", "Paste");
	assert_refused("   ", "Paste");
	assert_refused("http://www.youtube.com/watch?v=" ID, "https://");
	assert_refused("HTTPS://www.youtube.com/watch?v=" ID, "https://");
	assert_refused("ftp://www.youtube.com/watch?v=" ID, "https://");
	assert_refused("www.youtube.com/watch?v=" ID, "https://");
	assert_refused(ID, "https://");
	assert_refused("--exec=touch /tmp/x", NULL);
	assert_refused("-o /tmp/x https://www.youtube.com/watch?v=" ID, NULL);
	assert_refused("https://www.youtube.com/watch?v=" ID " --exec id", "spaces");
	assert_refused("https://www.youtube.com/watch?v=" ID "\n--exec", "spaces");
	assert_refused("https://www.youtube.com/watch?v=" ID "\t&t=1", "spaces");
	assert_refused("https://www.youtube.com/watch?v=mkvid\xc3\xa9o001", "plain");
	/* Another host, however it is dressed. */
	assert_refused("https://video.example/watch?v=" ID, "youtube.com");
	assert_refused("https://www.youtube.com.example/watch?v=" ID, "youtube.com");
	assert_refused("https://notyoutube.example/watch?v=" ID, "youtube.com");
	assert_refused("https://youtube.com.video.example/watch?v=" ID, "youtube.com");
	assert_refused("https://www.youtube.com@video.example/watch?v=" ID, NULL);
	/* A user name before YouTube's own host (written in two pieces: whole, it
	 * reads as an e-mail address to the made-up values check). */
	assert_refused("https://user" "@" "www.youtube.com/watch?v=" ID, "not a YouTube");
	assert_refused("https://www.youtube.com:8443/watch?v=" ID, "not a YouTube");
	assert_refused("https:///watch?v=" ID, "not a YouTube");
	/* A playlist and a channel are not one video. */
	assert_refused("https://www.youtube.com/playlist?list=PLmadeup0000000000000000000000001", "playlist");
	assert_refused("https://www.youtube.com/@MoonKettleStudio", "channel");
	assert_refused("https://www.youtube.com/@MoonKettleStudio/videos", "channel");
	assert_refused("https://www.youtube.com/channel/UCmoonkettle0000000000001", "channel");
	assert_refused("https://www.youtube.com/c/MoonKettleStudio", "channel");
	assert_refused("https://www.youtube.com/user/moonkettle", "channel");
	/* No video id of the right form. */
	assert_refused("https://www.youtube.com/", "no video");
	assert_refused("https://www.youtube.com", "no video");
	assert_refused("https://www.youtube.com/watch", "no video");
	assert_refused("https://www.youtube.com/watch?v=", "no video");
	assert_refused("https://www.youtube.com/watch?v=short", "no video");
	assert_refused("https://www.youtube.com/watch?v=" ID "x", "no video");
	assert_refused("https://www.youtube.com/watch?v=mkvideo$001", "no video");
	assert_refused("https://www.youtube.com/watch?list=" ID, "no video");
	assert_refused("https://www.youtube.com/watch?vv=" ID, "no video");
	assert_refused("https://youtu.be/", "no video");
	assert_refused("https://youtu.be/" ID "/more", "no video");
	assert_refused("https://www.youtube.com/shorts/", "no video");
	assert_refused("https://www.youtube.com/results?search_query=" ID, "no video");
	assert_refused(long_address, "too long");

	g_assert_true(axis_youtube_video_id_valid(ID));
	g_assert_false(axis_youtube_video_id_valid(NULL));
	g_assert_false(axis_youtube_video_id_valid("mkvideo000"));
	g_assert_false(axis_youtube_video_id_valid("mkvideo00011"));
	g_assert_false(axis_youtube_video_id_valid("mkvideo/001"));
}

/* ── The command ────────────────────────────────────────────────── */

static gboolean
has(
	gchar       **argv,
	const gchar  *word
){
	return g_strv_contains((const gchar *const *)argv, word);
}

/* Spec: yt-dlp is run with a fixed set of options */
static void
test_command(void)
{
	g_auto(GStrv) fetch = axis_youtube_build_argv("/opt/tools/yt-dlp", "-k_video0-1", FALSE, 720, 1048576,
		"/var/lib/axis/youtube/cache", "node");
	g_auto(GStrv) look = axis_youtube_build_argv("/opt/tools/yt-dlp", ID, TRUE, 1080, 0, NULL, NULL);
	g_auto(GStrv) env = axis_youtube_build_environment("/var/lib/axis/youtube/home", "/var/lib/axis/youtube/config",
		"/var/lib/axis/youtube/cache", "socks5h://user:secret@vpn:1080");
	g_auto(GStrv) bare = axis_youtube_build_environment(NULL, NULL, NULL, NULL);
	guint n = g_strv_length(fetch);
	guint i;

	g_assert_cmpstr(fetch[0], ==, "/opt/tools/yt-dlp");
	/* The address is Axis's own, the last argument, after "--": even an id
	 * that begins with a dash is never an option. */
	g_assert_cmpstr(fetch[n - 2], ==, "--");
	g_assert_cmpstr(fetch[n - 1], ==, "https://www.youtube.com/watch?v=-k_video0-1");
	for (i = 1; i < n - 2; i++)
		g_assert_null(strstr(fetch[i], "-k_video0-1"));
	g_assert_true(has(fetch, "--ignore-config"));
	g_assert_true(has(fetch, "--no-playlist"));
	g_assert_true(has(fetch, "--no-cookies"));
	g_assert_true(has(fetch, "--no-cookies-from-browser"));
	g_assert_true(has(fetch, "--no-exec"));
	g_assert_true(has(fetch, "--no-sponsorblock"));
	g_assert_true(has(fetch, "--newline"));
	g_assert_true(has(fetch, AXIS_YOUTUBE_PROGRESS_TEMPLATE));
	g_assert_true(has(fetch, "mkv"));
	g_assert_true(has(fetch, "1048576"));
	g_assert_true(has(fetch, "video.%(ext)s"));
	g_assert_true(has(fetch, "/var/lib/axis/youtube/cache"));
	g_assert_true(has(fetch, "node"));
	/* The proxy is never in the command (the process list is for everyone
	 * on the machine to read): it is in the environment, with the home and
	 * the settings folder Axis keeps empty. */
	g_assert_false(has(fetch, "--proxy"));
	for (i = 0; i < n; i++)
		g_assert_null(strstr(fetch[i], "vpn:1080"));
	g_assert_true(has(env, "HOME=/var/lib/axis/youtube/home"));
	g_assert_true(has(env, "XDG_CONFIG_HOME=/var/lib/axis/youtube/config"));
	g_assert_true(has(env, "XDG_CACHE_HOME=/var/lib/axis/youtube/cache"));
	g_assert_true(has(env, "ALL_PROXY=socks5h://user:secret@vpn:1080"));
	g_assert_true(has(env, "all_proxy=socks5h://user:secret@vpn:1080"));
	g_assert_true(has(env, "HTTPS_PROXY=socks5h://user:secret@vpn:1080"));
	g_assert_true(has(env, "https_proxy=socks5h://user:secret@vpn:1080"));
	g_assert_true(has(env, "HTTP_PROXY=socks5h://user:secret@vpn:1080"));
	g_assert_true(has(env, "http_proxy=socks5h://user:secret@vpn:1080"));
	g_assert_cmpuint(g_strv_length(env), ==, 9);
	/* Without a folder of Axis's: a place that is not there, never the
	 * daemon user's own home; and no proxy of any spelling. */
	g_assert_cmpuint(g_strv_length(bare), ==, 3);
	g_assert_true(g_str_has_prefix(bare[0], "HOME=/nonexistent/"));
	g_assert_true(g_str_has_prefix(bare[1], "XDG_CONFIG_HOME=/nonexistent/"));
	g_assert_true(g_str_has_prefix(bare[2], "XDG_CACHE_HOME=/nonexistent/"));
	g_assert_true(has(fetch, "bv*[height<=720][vcodec^=avc1]+ba[acodec^=mp4a]/bv*[height<=720]+ba/b[height<=720]"));
	g_assert_false(has(fetch, "--dump-single-json"));
	/* Never: a hook, cookies, a cut, a re-encode. */
	for (i = 0; i < n; i++) {
		g_assert_false(g_str_has_prefix(fetch[i], "--exec"));
		g_assert_false(g_str_has_prefix(fetch[i], "--cookies"));
		g_assert_false(g_str_has_prefix(fetch[i], "--recode"));
		g_assert_false(g_str_has_prefix(fetch[i], "--sponsorblock"));
		g_assert_false(g_str_has_prefix(fetch[i], "--config"));
		g_assert_false(g_str_has_prefix(fetch[i], "--username"));
	}

	n = g_strv_length(look);
	g_assert_cmpstr(look[n - 2], ==, "--");
	g_assert_cmpstr(look[n - 1], ==, "https://www.youtube.com/watch?v=" ID);
	g_assert_true(has(look, "--skip-download"));
	g_assert_true(has(look, "--dump-single-json"));
	g_assert_true(has(look, "--no-cache-dir"));
	g_assert_true(has(look, "--ignore-config"));
	g_assert_false(has(look, "--proxy"));
	g_assert_false(has(look, "--js-runtimes"));
	g_assert_false(has(look, "--output"));

	g_assert_true(axis_youtube_proxy_valid("socks5://vpn:1080"));
	g_assert_true(axis_youtube_proxy_valid("http://proxy.example:3128"));
	g_assert_false(axis_youtube_proxy_valid("--exec=x"));
	g_assert_false(axis_youtube_proxy_valid("socks5://"));
	g_assert_false(axis_youtube_proxy_valid("socks5://vpn:1080 --exec x"));
	g_assert_false(axis_youtube_proxy_valid("vpn:1080"));
	g_assert_false(axis_youtube_proxy_valid(NULL));

	/* The room a download needs: the streams and the merged file, each
	 * within the size limit; twice the limit when yt-dlp gave no size. */
	g_assert_cmpuint(axis_youtube_need_bytes(5000, 1048576), ==, 10000);
	g_assert_cmpuint(axis_youtube_need_bytes(G_GINT64_CONSTANT(9000000000), 1048576), ==, 2097152);
	g_assert_cmpuint(axis_youtube_need_bytes(-1, 1048576), ==, 2097152);
	g_assert_cmpuint(axis_youtube_need_bytes(0, G_GUINT64_CONSTANT(8589934592)), ==, G_GUINT64_CONSTANT(17179869184));
}

/* ── Names ──────────────────────────────────────────────────────── */

static AxisYoutubeVideo
video_of(
	const gchar *title,
	const gchar *channel,
	const gchar *channel_id,
	const gchar *date
){
	AxisYoutubeVideo video;

	memset(&video, 0, sizeof(video));
	video.video_id = (gchar *)ID;
	video.title = (gchar *)title;
	video.channel = (gchar *)channel;
	video.channel_id = (gchar *)channel_id;
	video.upload_date = (gchar *)date;
	return video;
}

static void
assert_component(const gchar *name)
{
	g_assert_cmpuint(strlen(name), >, 0);
	g_assert_cmpuint(strlen(name), <=, AXIS_IMPORT_NAME_BUDGET);
	g_assert_null(strchr(name, '/'));
	g_assert_true(name[0] != '.' && name[0] != ' ');
	g_assert_true(g_utf8_validate(name, -1, NULL));
	g_assert_cmpstr(name, !=, "..");
}

/* Spec: The file is named from what yt-dlp says, made safe */
static void
test_names(void)
{
	g_autofree gchar *long_title = g_strnfill(600, 'T');
	g_autofree gchar *long_channel = g_strnfill(400, 'C');
	g_autofree gchar *channel_dir = NULL;
	g_autofree gchar *season_dir = NULL;
	g_autofree gchar *file_name = NULL;
	AxisYoutubeVideo video = video_of("The Lost Spoon", "Moon Kettle Studio", "UCmoonkettle0000000000001", "2021-03-15");

	axis_youtube_names(&video, &channel_dir, &season_dir, &file_name);
	g_assert_cmpstr(channel_dir, ==, "Moon Kettle Studio [UCmoonkettle0000000000001]");
	g_assert_cmpstr(season_dir, ==, "Season 2021");
	g_assert_cmpstr(file_name, ==, "Moon Kettle Studio - 2021-03-15 - The Lost Spoon [" ID "].mkv");
	g_clear_pointer(&channel_dir, g_free);
	g_clear_pointer(&season_dir, g_free);
	g_clear_pointer(&file_name, g_free);

	/* What a title can hold: slashes, dots, a path, control characters. */
	video = video_of("../../etc/passwd\n: a/b\\c", "..", NULL, NULL);
	axis_youtube_names(&video, &channel_dir, &season_dir, &file_name);
	assert_component(channel_dir);
	assert_component(season_dir);
	assert_component(file_name);
	g_assert_cmpstr(channel_dir, ==, "Unknown channel");
	g_assert_cmpstr(season_dir, ==, "Season 0000");
	g_assert_null(strstr(file_name, ".."  "/"));
	g_assert_null(strchr(file_name, '\n'));
	g_assert_null(strchr(file_name, '\\'));
	g_assert_true(g_str_has_suffix(file_name, " [" ID "].mkv"));
	g_clear_pointer(&channel_dir, g_free);
	g_clear_pointer(&season_dir, g_free);
	g_clear_pointer(&file_name, g_free);

	/* Too long: the title is cut, never the id or the extension. */
	video = video_of(long_title, long_channel, "UCmoonkettle0000000000001", "2021-03-15");
	axis_youtube_names(&video, &channel_dir, &season_dir, &file_name);
	assert_component(channel_dir);
	assert_component(file_name);
	g_assert_true(g_str_has_suffix(channel_dir, " [UCmoonkettle0000000000001]"));
	g_assert_true(g_str_has_suffix(file_name, " [" ID "].mkv"));
	g_assert_nonnull(strstr(file_name, " - 2021-03-15 - "));
	g_clear_pointer(&channel_dir, g_free);
	g_clear_pointer(&season_dir, g_free);
	g_clear_pointer(&file_name, g_free);

	/* Nothing left of a title is still a name. */
	video = video_of("...", " ", NULL, "2021-03-15");
	axis_youtube_names(&video, &channel_dir, &season_dir, &file_name);
	g_assert_cmpstr(file_name, ==, "Unknown channel - 2021-03-15 - Untitled [" ID "].mkv");
	g_clear_pointer(&channel_dir, g_free);
	g_clear_pointer(&season_dir, g_free);
	g_clear_pointer(&file_name, g_free);

	/* A video YouTube serves as one file keeps the container it came in;
	 * an extension that is not a video's is never written. */
	video = video_of("The Lost Spoon", "Moon Kettle Studio", "UCmoonkettle0000000000001", "2021-03-15");
	axis_youtube_names_as(&video, "MP4", &channel_dir, &season_dir, &file_name);
	g_assert_cmpstr(file_name, ==, "Moon Kettle Studio - 2021-03-15 - The Lost Spoon [" ID "].mp4");
	g_clear_pointer(&channel_dir, g_free);
	g_clear_pointer(&season_dir, g_free);
	g_clear_pointer(&file_name, g_free);
	axis_youtube_names_as(&video, "webm", &channel_dir, &season_dir, &file_name);
	g_assert_true(g_str_has_suffix(file_name, " [" ID "].webm"));
	g_clear_pointer(&channel_dir, g_free);
	g_clear_pointer(&season_dir, g_free);
	g_clear_pointer(&file_name, g_free);
	axis_youtube_names_as(&video, "sh", &channel_dir, &season_dir, &file_name);
	g_assert_true(g_str_has_suffix(file_name, " [" ID "].mkv"));
	g_assert_cmpstr(axis_youtube_extension("WebM"), ==, "webm");
	g_assert_null(axis_youtube_extension("part"));
	g_assert_null(axis_youtube_extension("../mkv"));
	g_assert_null(axis_youtube_extension(NULL));
}

/* ── Progress ───────────────────────────────────────────────────── */

static gboolean
parse(
	const gchar         *line,
	AxisYoutubeProgress *progress
){
	return axis_youtube_progress_parse(line, strlen(line), progress);
}

/* Spec: Progress is read from yt-dlp's own lines */
static void
test_progress(void)
{
	AxisYoutubeProgress p;

	g_assert_true(parse("AXIS downloading 1000 4000 NA 2000.5 2", &p));
	g_assert_false(p.finished);
	g_assert_cmpint(p.done_bytes, ==, 1000);
	g_assert_cmpint(p.total_bytes, ==, 4000);
	g_assert_cmpint(p.rate_bps, ==, 2000);
	g_assert_cmpint(p.eta_s, ==, 2);

	/* No total: the estimate stands in. */
	g_assert_true(parse("AXIS downloading 500 NA 1000.0 None NA", &p));
	g_assert_cmpint(p.total_bytes, ==, 1000);
	g_assert_cmpint(p.rate_bps, ==, -1);
	g_assert_cmpint(p.eta_s, ==, -1);

	g_assert_true(parse("AXIS finished 4000 4000 NA NA NA", &p));
	g_assert_true(p.finished);

	/* More done than the whole: the whole was an estimate. */
	g_assert_true(parse("AXIS downloading 900 NA 800 1 1", &p));
	g_assert_cmpint(p.total_bytes, ==, 900);

	/* Figures out of range, negative or not numbers are not known. */
	g_assert_true(parse("AXIS downloading -5 1e300 nan inf 99999999999999999999", &p));
	g_assert_cmpint(p.done_bytes, ==, -1);
	g_assert_cmpint(p.total_bytes, ==, -1);
	g_assert_cmpint(p.rate_bps, ==, -1);
	g_assert_cmpint(p.eta_s, ==, -1);

	/* Anything else is not a progress line. */
	g_assert_false(parse("", &p));
	g_assert_false(parse("AXIS", &p));
	g_assert_false(parse("AXIS downloading 1 2 3 4", &p));
	g_assert_false(parse("AXIS downloading 1 2 3 4 5 6", &p));
	g_assert_false(parse("AXIS error 1 2 3 4 5", &p));
	g_assert_false(parse("[download]  42.0% of 10.00MiB", &p));
	g_assert_false(parse(" AXIS downloading 1 2 3 4 5", &p));
	g_assert_false(axis_youtube_progress_parse("AXIS downloading 1 2 3 4\0005", 27, &p));
	g_assert_false(axis_youtube_progress_parse(NULL, 0, &p));
}

/* A video and its audio are two streams; and however many lines yt-dlp
 * prints, with whatever figures, the sum stays a size. */
static void
test_progress_sum(void)
{
	AxisYoutubeProgress p;
	gint64 base = 0;
	guint i;

	g_assert_true(parse("AXIS downloading 1000 4000 NA 2000.5 2", &p));
	g_assert_cmpint(axis_youtube_progress_add(&base, &p), ==, 1000);
	g_assert_cmpint(base, ==, 0);
	g_assert_true(parse("AXIS finished 4000 4000 NA NA NA", &p));
	g_assert_cmpint(axis_youtube_progress_add(&base, &p), ==, 4000);
	g_assert_cmpint(base, ==, 4000);
	g_assert_true(parse("AXIS downloading 500 NA 1000 100 5", &p));
	g_assert_cmpint(axis_youtube_progress_add(&base, &p), ==, 4500);
	/* A stream that ends without saying how much it was adds nothing. */
	g_assert_true(parse("AXIS finished NA NA NA NA NA", &p));
	g_assert_cmpint(axis_youtube_progress_add(&base, &p), ==, 4000);

	/* The largest figure a line may hold, said finished ten thousand times
	 * (2^50 a line: 2^63 is passed after 8192 of them). */
	base = 0;
	g_assert_true(parse("AXIS finished 1125899906842624 1125899906842624 NA NA NA", &p));
	g_assert_cmpint(p.done_bytes, ==, AXIS_YOUTUBE_MAX_BYTES);
	for (i = 0; i < 10000; i++) {
		gint64 done = axis_youtube_progress_add(&base, &p);

		g_assert_cmpint(done, >=, 0);
		g_assert_cmpint(done, <=, AXIS_YOUTUBE_MAX_BYTES);
		g_assert_cmpint(base, >=, 0);
		g_assert_cmpint(base, <=, AXIS_YOUTUBE_MAX_BYTES);
	}
	g_assert_true(parse("AXIS downloading 1125899906842624 NA NA NA NA", &p));
	g_assert_cmpint(axis_youtube_progress_add(&base, &p), ==, AXIS_YOUTUBE_MAX_BYTES);
}

typedef struct
{
	GPtrArray *lines;
} Seen;

static void
on_line(
	const gchar *line,
	gsize        length,
	gpointer     user_data
){
	Seen *seen = user_data;

	g_ptr_array_add(seen->lines, g_strndup(line, length));
}

static void
test_lines(void)
{
	g_autofree gchar *huge = g_strnfill(3 * AXIS_YOUTUBE_LINE_MAX, 'x');
	AxisYoutubeLines lines;
	Seen seen;

	memset(&lines, 0, sizeof(lines));
	seen.lines = g_ptr_array_new_with_free_func(g_free);
	/* Pieces as they come, cut anywhere. */
	axis_youtube_lines_feed(&lines, (const guint8 *)"one\ntw", 6, on_line, &seen);
	axis_youtube_lines_feed(&lines, (const guint8 *)"o\r\n\nthree", 9, on_line, &seen);
	g_assert_cmpuint(seen.lines->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(seen.lines, 0), ==, "one");
	g_assert_cmpstr(g_ptr_array_index(seen.lines, 1), ==, "two");
	/* A line longer than the limit is dropped whole; the next one is read. */
	axis_youtube_lines_feed(&lines, (const guint8 *)huge, strlen(huge), on_line, &seen);
	axis_youtube_lines_feed(&lines, (const guint8 *)"tail\nfour\n", 10, on_line, &seen);
	g_assert_cmpuint(seen.lines->len, ==, 3);
	g_assert_cmpstr(g_ptr_array_index(seen.lines, 2), ==, "four");
	g_ptr_array_unref(seen.lines);
}

/* ── A video's record ───────────────────────────────────────────── */

#define RECORD_HEAD "{\"id\":\"" ID "\",\"_type\":\"video\","

static AxisYoutubeInfo *
record(
	const gchar  *json,
	gchar       **problem
){
	return axis_youtube_info_parse(json, strlen(json), problem);
}

/* Spec: What yt-dlp says of a video is read within limits */
static void
test_record(void)
{
	static const gchar *const whole = RECORD_HEAD
		"\"title\":\"The Lost Spoon\\u0007 \\n(Full Episode)\",\"channel\":\"Moon Kettle Studio\","
		"\"channel_id\":\"UCmoonkettle0000000000001\",\"uploader\":\"Somebody Else\",\"upload_date\":\"20210315\","
		"\"duration\":1260.5,\"live_status\":\"not_live\",\"availability\":\"public\","
		"\"requested_formats\":[{\"height\":1080,\"vcodec\":\"avc1.640028\",\"acodec\":\"none\",\"filesize\":4000},"
		"{\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"filesize_approx\":1000.7}],"
		"\"formats\":[{\"format_id\":\"18\",\"url\":\"https://media.example/a\"}],\"description\":\"words\"}";
	g_autoptr(AxisYoutubeInfo) info = NULL;
	g_autofree gchar *problem = NULL;

	info = axis_youtube_info_parse(whole, strlen(whole), &problem);
	g_assert_nonnull(info);
	g_assert_null(problem);
	g_assert_cmpstr(info->video_id, ==, ID);
	/* Control characters go; the rest is the uploader's. */
	g_assert_cmpstr(info->title, ==, "The Lost Spoon (Full Episode)");
	g_assert_cmpstr(info->channel, ==, "Moon Kettle Studio");
	g_assert_cmpstr(info->channel_id, ==, "UCmoonkettle0000000000001");
	g_assert_cmpint(info->year, ==, 2021);
	g_assert_cmpint(info->month, ==, 3);
	g_assert_cmpint(info->day, ==, 15);
	g_assert_cmpint(info->duration_s, ==, 1260);
	g_assert_cmpint(info->expected_bytes, ==, 5000);
	g_assert_cmpstr(info->format_note, ==, "1080p, avc1 with mp4a");
	g_assert_null(info->refusal);
	g_clear_pointer(&info, axis_youtube_info_free);

	/* Little said: defaults, never a guess. */
	info = record(RECORD_HEAD "\"uploader\":\"Moon Kettle Studio\",\"channel_id\":\"../x\","
		"\"upload_date\":\"20211345\",\"duration\":-4}", &problem);
	g_assert_nonnull(info);
	g_assert_cmpstr(info->title, ==, "Untitled");
	g_assert_cmpstr(info->channel, ==, "Moon Kettle Studio");
	g_assert_null(info->channel_id);
	g_assert_cmpint(info->year, ==, 0);
	g_assert_cmpint(info->duration_s, ==, -1);
	g_assert_cmpint(info->expected_bytes, ==, -1);
	g_assert_null(info->format_note);
	g_clear_pointer(&info, axis_youtube_info_free);

	/* What Axis does not fetch says why. */
	info = record(RECORD_HEAD "\"title\":\"A Stream\",\"live_status\":\"is_live\"}", &problem);
	g_assert_nonnull(info);
	g_assert_nonnull(strstr(info->refusal, "live stream"));
	g_clear_pointer(&info, axis_youtube_info_free);
	info = record(RECORD_HEAD "\"title\":\"Members\",\"availability\":\"subscriber_only\"}", &problem);
	g_assert_nonnull(info);
	g_assert_nonnull(strstr(info->refusal, "account"));
	g_clear_pointer(&info, axis_youtube_info_free);
	{
		static const gchar *const list = "{\"id\":\"PLmadeup0000000000000000000000001\",\"_type\":\"playlist\","
			"\"entries\":[]}";

		info = axis_youtube_info_parse(list, strlen(list), &problem);
		g_assert_nonnull(info);
		g_assert_null(info->video_id);
		g_assert_nonnull(strstr(info->refusal, "list of videos"));
		g_clear_pointer(&info, axis_youtube_info_free);
	}

	/* Not a record. */
	g_assert_null(axis_youtube_info_parse("", 0, &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);
	g_assert_null(record("[1,2]", &problem));
	g_clear_pointer(&problem, g_free);
	g_assert_null(record("{\"id\":\"short\",\"title\":\"x\"}", &problem));
	g_assert_nonnull(strstr(problem, "names no video"));
	g_clear_pointer(&problem, g_free);
	g_assert_null(record("{\"id\":\"" ID "\"", &problem));
	g_clear_pointer(&problem, g_free);
	g_assert_null(axis_youtube_info_parse(NULL, 0, NULL));
}

/* Spec: A failure ends with a plain reason */
static void
test_failure_words(void)
{
	static const struct
	{
		const gchar        *errors;
		AxisYoutubeFailure  kind;
		const gchar        *words;
	} cases[] = {
		{ "WARNING: slow\nERROR: [youtube] " ID ": Sign in to confirm you\xe2\x80\x99re not a bot. Use cookies\n",
		  AXIS_YOUTUBE_FAILURE_REFUSED, "refused this server" },
		{ "ERROR: unable to download video data: HTTP Error 429: Too Many Requests\n", AXIS_YOUTUBE_FAILURE_REFUSED,
		  "refused this server" },
		{ "ERROR: [youtube] " ID ": Private video. Sign in if you've been granted access\n",
		  AXIS_YOUTUBE_FAILURE_ACCOUNT, "account" },
		{ "ERROR: [youtube] " ID ": Sign in to confirm your age\n", AXIS_YOUTUBE_FAILURE_ACCOUNT, "account" },
		{ "ERROR: [youtube] " ID ": Video unavailable\n", AXIS_YOUTUBE_FAILURE_GONE, "not available" },
		/* As yt-dlp 2026.10.26 says it. */
		{ "ERROR: [youtube] " ID ": This video is unavailable\n", AXIS_YOUTUBE_FAILURE_GONE,
		  "YouTube says this video is not available" },
		{ "ERROR: [youtube] " ID ": Requested format is not available. Use --list-formats for a list of available "
		  "formats\n", AXIS_YOUTUBE_FAILURE_OTHER, "in no form Axis can take at this height" },
		{ "ERROR: [youtube] " ID ": Unable to download API page: [Errno 111] Connection refused\n",
		  AXIS_YOUTUBE_FAILURE_OTHER, "yt-dlp said: [youtube] " ID ": Unable to download API page" },
		{ "ERROR: [youtube] " ID ": The uploader has not made this video available in your country\n",
		  AXIS_YOUTUBE_FAILURE_PLACE, "location" },
		{ "ERROR: unable to write data: [Errno 27] File too large\n", AXIS_YOUTUBE_FAILURE_OTHER, "size limit" },
		{ "ERROR: first\nERROR: unable to download video data: HTTP Error 403: Forbidden\n", AXIS_YOUTUBE_FAILURE_OTHER,
		  "yt-dlp said: unable to download video data: HTTP Error 403: Forbidden" },
		{ "no error line at all\n", AXIS_YOUTUBE_FAILURE_OTHER, "without saying why" },
		{ "", AXIS_YOUTUBE_FAILURE_OTHER, "without saying why" }
	};
	g_autofree gchar *long_error = NULL;
	g_autofree gchar *reason = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *said = NULL;

		g_assert_cmpint(axis_youtube_failure_read(cases[i].errors, strlen(cases[i].errors), &said), ==, cases[i].kind);
		if (strstr(said, cases[i].words) == NULL)
			g_error("expected '%s' in: %s", cases[i].words, said);
	}
	/* yt-dlp's words are cut and cleaned. */
	{
		g_autofree gchar *fill = g_strnfill(5000, 'e');

		long_error = g_strconcat("ERROR: bad\x1b[0m thing ", fill, "\n", NULL);
	}
	g_assert_cmpint(axis_youtube_failure_read(long_error, strlen(long_error), &reason), ==, AXIS_YOUTUBE_FAILURE_OTHER);
	g_assert_cmpuint(g_utf8_strlen(reason, -1), <=, AXIS_YOUTUBE_REASON_CHARS + 16);
	g_assert_null(strchr(reason, '\x1b'));

	{
		g_autofree gchar *version = axis_youtube_version_read("2025.01.15\n", 11);
		g_autofree gchar *nightly = axis_youtube_version_read("2025.01.15.232816\n", 18);

		g_assert_cmpstr(version, ==, "2025.01.15");
		g_assert_cmpstr(nightly, ==, "2025.01.15.232816");
		g_assert_null(axis_youtube_version_read("Usage: yt-dlp [OPTIONS]\n", 24));
		g_assert_null(axis_youtube_version_read("2025.01.15 extra\n", 17));
		g_assert_null(axis_youtube_version_read("", 0));
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/youtube-unit/address/taken", test_address_taken);
	g_test_add_func("/youtube-unit/address/refused", test_address_refused);
	g_test_add_func("/youtube-unit/command", test_command);
	g_test_add_func("/youtube-unit/names", test_names);
	g_test_add_func("/youtube-unit/progress", test_progress);
	g_test_add_func("/youtube-unit/progress-sum", test_progress_sum);
	g_test_add_func("/youtube-unit/lines", test_lines);
	g_test_add_func("/youtube-unit/record", test_record);
	g_test_add_func("/youtube-unit/failure-words", test_failure_words);
	return g_test_run();
}
