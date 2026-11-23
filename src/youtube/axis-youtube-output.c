/*
 * axis-youtube-output.c - reading what yt-dlp writes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "youtube/axis-youtube-output.h"

#include <math.h>
#include <string.h>

#include "download/axis-download-model.h"
#include "probe/axis-probe-json.h"
#include "youtube/axis-youtube-address.h"

#define ETA_MAX_S (G_GINT64_CONSTANT(10) * 365 * 86400)

/* ── Progress ───────────────────────────────────────────────────── */

/* A figure: a number that is not negative and at most @maximum, written
 * whole in @token; -1 for anything else ("NA", "None", a word). */
static gint64
figure(
	const gchar *token,
	gint64       maximum
){
	gchar *end = NULL;
	gdouble value;

	if (token == NULL || !(g_ascii_isdigit(token[0]) || token[0] == '.'))
		return -1;
	value = g_ascii_strtod(token, &end);
	if (end == token || *end != '\0' || !isfinite(value) || value < 0.0 || value > (gdouble)maximum)
		return -1;
	return (gint64)value;
}

gboolean
axis_youtube_progress_parse(
	const gchar         *line,
	gsize                length,
	AxisYoutubeProgress *progress
){
	g_autofree gchar *copy = NULL;
	g_auto(GStrv) tokens = NULL;
	gint64 total;

	g_return_val_if_fail(progress != NULL, FALSE);

	memset(progress, 0, sizeof(*progress));
	progress->done_bytes = progress->total_bytes = progress->rate_bps = progress->eta_s = -1;
	if (line == NULL || length < 5 || length > AXIS_YOUTUBE_LINE_MAX || memcmp(line, "AXIS ", 5) != 0 ||
	    memchr(line, '\0', length) != NULL)
		return FALSE;
	copy = g_strndup(line, length);
	tokens = g_strsplit(copy, " ", 8);
	if (g_strv_length(tokens) != 7)
		return FALSE;
	if (strcmp(tokens[1], "finished") == 0)
		progress->finished = TRUE;
	else if (strcmp(tokens[1], "downloading") != 0)
		return FALSE;
	progress->done_bytes = figure(tokens[2], AXIS_YOUTUBE_MAX_BYTES);
	total = figure(tokens[3], AXIS_YOUTUBE_MAX_BYTES);
	if (total < 0)
		total = figure(tokens[4], AXIS_YOUTUBE_MAX_BYTES);
	progress->total_bytes = total;
	progress->rate_bps = figure(tokens[5], AXIS_YOUTUBE_MAX_BYTES);
	progress->eta_s = figure(tokens[6], ETA_MAX_S);
	/* More done than the whole: the whole was an estimate. */
	if (progress->total_bytes >= 0 && progress->done_bytes > progress->total_bytes)
		progress->total_bytes = progress->done_bytes;
	return TRUE;
}

gint64
axis_youtube_progress_add(
	gint64                    *base,
	const AxisYoutubeProgress *progress
){
	gint64 done;

	g_return_val_if_fail(base != NULL && progress != NULL, 0);

	/* Each term is within the limit, so no sum of two passes a gint64; and
	 * the sum is brought back within it each time. */
	*base = CLAMP(*base, 0, AXIS_YOUTUBE_MAX_BYTES);
	done = *base + CLAMP(progress->done_bytes, 0, AXIS_YOUTUBE_MAX_BYTES);
	if (progress->finished) {
		*base += CLAMP(MAX(progress->done_bytes, progress->total_bytes), 0, AXIS_YOUTUBE_MAX_BYTES);
		*base = MIN(*base, AXIS_YOUTUBE_MAX_BYTES);
		return *base;
	}
	return MIN(done, AXIS_YOUTUBE_MAX_BYTES);
}

void
axis_youtube_lines_feed(
	AxisYoutubeLines    *lines,
	const guint8        *data,
	gsize                length,
	AxisYoutubeLineFunc  func,
	gpointer             user_data
){
	gsize i;

	g_return_if_fail(lines != NULL && func != NULL);

	for (i = 0; i < length; i++) {
		guint8 c = data[i];

		if (c == '\n' || c == '\r') {
			if (!lines->overlong && lines->used > 0)
				func(lines->buffer, lines->used, user_data);
			lines->used = 0;
			lines->overlong = FALSE;
			continue;
		}
		if (lines->overlong)
			continue;
		if (lines->used >= sizeof(lines->buffer)) {
			lines->overlong = TRUE;
			lines->used = 0;
			continue;
		}
		lines->buffer[lines->used++] = (gchar)c;
	}
}

/* ── A video's record ───────────────────────────────────────────── */

void
axis_youtube_info_free(AxisYoutubeInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->video_id);
	g_free(info->title);
	g_free(info->channel);
	g_free(info->channel_id);
	g_free(info->format_note);
	g_free(info->refusal);
	g_free(info);
}

static gchar *
clean_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_download_text_clean(axis_json_string(axis_json_member(object, name)), max_chars);
}

/* A channel's id as a folder's name carries it: 1 to 64 letters, digits, '-'
 * and '_'; %NULL for anything else. */
static gchar *
channel_id_of(const AxisJsonNode *root)
{
	const gchar *text = axis_json_string(axis_json_member(root, "channel_id"));
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > 64)
		return NULL;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			return NULL;
	}
	return g_strdup(text);
}

/* "avc1.640028" as "avc1": the letters and digits before the first '.', at
 * most 12; %NULL for none or yt-dlp's "none". */
static gchar *
codec_word(const AxisJsonNode *node)
{
	const gchar *text = axis_json_string(node);
	GString *word;

	if (text == NULL || strcmp(text, "none") == 0)
		return NULL;
	word = g_string_new(NULL);
	while (*text != '\0' && *text != '.' && word->len < 12) {
		if (!g_ascii_isalnum(*text))
			break;
		g_string_append_c(word, *text++);
	}
	if (word->len == 0) {
		g_string_free(word, TRUE);
		return NULL;
	}
	return g_string_free(word, FALSE);
}

static gint64
size_of(const AxisJsonNode *object)
{
	gdouble value = 0;

	if (axis_json_get_double(axis_json_member(object, "filesize"), 0.0, (gdouble)AXIS_YOUTUBE_MAX_BYTES, &value) ||
	    axis_json_get_double(axis_json_member(object, "filesize_approx"), 0.0, (gdouble)AXIS_YOUTUBE_MAX_BYTES, &value))
		return (gint64)value;
	return -1;
}

/* What would be taken: the formats yt-dlp chose (two, a video and its audio,
 * or the record itself for one that holds both). */
static void
read_formats(
	const AxisJsonNode *root,
	AxisYoutubeInfo    *info
){
	const AxisJsonNode *chosen = axis_json_member(root, "requested_formats");
	g_autofree gchar *video = NULL;
	g_autofree gchar *audio = NULL;
	gint64 height = 0;
	gint64 total = 0;
	gboolean sized = TRUE;
	guint n = axis_json_type(chosen) == AXIS_JSON_ARRAY ? axis_json_length(chosen) : 0;
	guint i;

	if (n == 0 || n > 8) {
		video = codec_word(axis_json_member(root, "vcodec"));
		audio = codec_word(axis_json_member(root, "acodec"));
		axis_json_get_integer(axis_json_member(root, "height"), 1, 100000, &height);
		info->expected_bytes = size_of(root);
	} else {
		for (i = 0; i < n; i++) {
			const AxisJsonNode *format = axis_json_element(chosen, i);
			gint64 size = size_of(format);
			gint64 h = 0;

			if (video == NULL)
				video = codec_word(axis_json_member(format, "vcodec"));
			if (audio == NULL)
				audio = codec_word(axis_json_member(format, "acodec"));
			if (axis_json_get_integer(axis_json_member(format, "height"), 1, 100000, &h) && h > height)
				height = h;
			if (size < 0 || total > AXIS_YOUTUBE_MAX_BYTES - size)
				sized = FALSE;
			else
				total += size;
		}
		info->expected_bytes = sized ? total : size_of(root);
	}
	if (height > 0 && video != NULL && audio != NULL)
		info->format_note = g_strdup_printf("%" G_GINT64_FORMAT "p, %s with %s", height, video, audio);
	else if (height > 0 && video != NULL)
		info->format_note = g_strdup_printf("%" G_GINT64_FORMAT "p, %s", height, video);
	else if (video != NULL || audio != NULL)
		info->format_note = g_strdup_printf("%s%s%s", video != NULL ? video : "", video != NULL && audio != NULL ?
			" with " : "", audio != NULL ? audio : "");
}

/* "20210315": a date between 2000 and 2100. */
static void
read_date(
	const AxisJsonNode *root,
	AxisYoutubeInfo    *info
){
	const gchar *text = axis_json_scalar_text(axis_json_member(root, "upload_date"));
	guint i;
	gint year;
	gint month;
	gint day;

	if (text == NULL || strlen(text) != 8)
		return;
	for (i = 0; i < 8; i++) {
		if (!g_ascii_isdigit(text[i]))
			return;
	}
	year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
	month = (text[4] - '0') * 10 + (text[5] - '0');
	day = (text[6] - '0') * 10 + (text[7] - '0');
	if (year < 2000 || year > 2100 || !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
		return;
	info->year = year;
	info->month = month;
	info->day = day;
}

static const gchar *
refusal_of(const AxisJsonNode *root)
{
	const gchar *type = axis_json_string(axis_json_member(root, "_type"));
	const gchar *live = axis_json_string(axis_json_member(root, "live_status"));
	const gchar *availability = axis_json_string(axis_json_member(root, "availability"));

	if (type != NULL && strcmp(type, "video") != 0)
		return "That address is a list of videos, not one video.";
	if (live != NULL && strcmp(live, "is_live") == 0)
		return "This is a live stream that is still running. Axis does not record live streams.";
	if (live != NULL && strcmp(live, "is_upcoming") == 0)
		return "This video has not been shown yet.";
	if (live != NULL && strcmp(live, "post_live") == 0)
		return "This live stream has just ended and YouTube is still preparing it. Try again later.";
	if (availability != NULL && (strcmp(availability, "private") == 0 || strcmp(availability, "premium_only") == 0 ||
	    strcmp(availability, "subscriber_only") == 0 || strcmp(availability, "needs_auth") == 0))
		return "This video needs a YouTube account (it is private, for members, or age restricted). Axis does not "
			"sign in to YouTube.";
	return NULL;
}

AxisYoutubeInfo *
axis_youtube_info_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autoptr(AxisYoutubeInfo) info = NULL;
	const AxisJsonNode *root;
	const gchar *id;
	gdouble duration = 0;

	if (problem != NULL)
		*problem = NULL;
	doc = data != NULL ? axis_json_parse(data, length, NULL) : NULL;
	root = doc != NULL ? axis_json_doc_get_root(doc) : NULL;
	if (root == NULL || axis_json_type(root) != AXIS_JSON_OBJECT) {
		if (problem != NULL)
			*problem = g_strdup("yt-dlp's answer about the video could not be read.");
		return NULL;
	}
	info = g_new0(AxisYoutubeInfo, 1);
	info->duration_s = -1;
	info->expected_bytes = -1;
	info->refusal = g_strdup(refusal_of(root));
	id = axis_json_string(axis_json_member(root, "id"));
	/* A list has no video id of its own: it is refused by its type above. */
	if (!axis_youtube_video_id_valid(id)) {
		if (info->refusal != NULL) {
			info->video_id = NULL;
			info->title = g_strdup("Untitled");
			info->channel = g_strdup("Unknown channel");
			return g_steal_pointer(&info);
		}
		if (problem != NULL)
			*problem = g_strdup("yt-dlp's answer names no video.");
		return NULL;
	}
	info->video_id = g_strdup(id);
	info->title = clean_member(root, "title", AXIS_YOUTUBE_TITLE_CHARS);
	if (info->title == NULL)
		info->title = g_strdup("Untitled");
	info->channel = clean_member(root, "channel", AXIS_YOUTUBE_CHANNEL_CHARS);
	if (info->channel == NULL)
		info->channel = clean_member(root, "uploader", AXIS_YOUTUBE_CHANNEL_CHARS);
	if (info->channel == NULL)
		info->channel = g_strdup("Unknown channel");
	info->channel_id = channel_id_of(root);
	read_date(root, info);
	if (axis_json_get_double(axis_json_member(root, "duration"), 0.0, 1000000.0, &duration))
		info->duration_s = (gint64)duration;
	read_formats(root, info);
	return g_steal_pointer(&info);
}

/* ── Why it failed ──────────────────────────────────────────────── */

static gboolean
holds(
	const gchar *lower,
	const gchar *words
){
	return strstr(lower, words) != NULL;
}

AxisYoutubeFailure
axis_youtube_failure_read(
	const gchar  *errors,
	gsize         length,
	gchar       **reason
){
	g_autofree gchar *copy = NULL;
	g_autofree gchar *said = NULL;
	g_autofree gchar *lower = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *last = NULL;
	guint i;

	g_return_val_if_fail(reason != NULL, AXIS_YOUTUBE_FAILURE_OTHER);

	*reason = NULL;
	if (errors != NULL && length > 0) {
		/* Only its end matters, and only so much of it is looked at. */
		gsize from = length > 16384 ? length - 16384 : 0;

		copy = g_strndup(errors + from, length - from);
		for (i = 0; i < length - from; i++) {
			if (copy[i] == '\0')
				copy[i] = ' ';
		}
		lines = g_strsplit(copy, "\n", -1);
		for (i = 0; lines[i] != NULL; i++) {
			if (g_str_has_prefix(lines[i], "ERROR:"))
				last = lines[i] + strlen("ERROR:");
		}
	}
	said = axis_download_text_clean(last, AXIS_YOUTUBE_REASON_CHARS);
	if (said == NULL) {
		*reason = g_strdup("yt-dlp failed without saying why.");
		return AXIS_YOUTUBE_FAILURE_OTHER;
	}
	lower = g_ascii_strdown(said, -1);
	if (holds(lower, "not a bot") || holds(lower, "http error 429") || holds(lower, "too many requests")) {
		*reason = g_strdup("YouTube refused this server for now (too many requests, or it asked to confirm that the "
			"visitor is not a bot).");
		return AXIS_YOUTUBE_FAILURE_REFUSED;
	}
	if (holds(lower, "private video") || holds(lower, "members-only") || holds(lower, "members only") ||
	    holds(lower, "confirm your age") || holds(lower, "age-restricted") || holds(lower, "sign in")) {
		*reason = g_strdup("This video needs a YouTube account (it is private, for members, or age restricted). Axis "
			"does not sign in to YouTube.");
		return AXIS_YOUTUBE_FAILURE_ACCOUNT;
	}
	if (holds(lower, "in your country") || holds(lower, "from your location")) {
		*reason = g_strdup("This video is not available from this server's location.");
		return AXIS_YOUTUBE_FAILURE_PLACE;
	}
	/* (yt-dlp 2026.10.26 says "This video is unavailable" of a missing one.) */
	if (holds(lower, "video unavailable") || holds(lower, "is unavailable") || holds(lower, "has been removed") ||
	    holds(lower, "does not exist")) {
		*reason = g_strdup("YouTube says this video is not available.");
		return AXIS_YOUTUBE_FAILURE_GONE;
	}
	if (holds(lower, "requested format is not available")) {
		*reason = g_strdup("YouTube offers this video in no form Axis can take at this height (youtube.max_height).");
		return AXIS_YOUTUBE_FAILURE_OTHER;
	}
	if (holds(lower, "file too large") || holds(lower, "larger than max-filesize")) {
		*reason = g_strdup("The video is larger than the size limit (youtube.max_size).");
		return AXIS_YOUTUBE_FAILURE_OTHER;
	}
	*reason = g_strdup_printf("yt-dlp said: %s", said);
	return AXIS_YOUTUBE_FAILURE_OTHER;
}

gchar *
axis_youtube_version_read(
	const gchar *output,
	gsize        length
){
	gsize n = 0;

	if (output == NULL)
		return NULL;
	while (n < length && n <= 32 && (g_ascii_isdigit(output[n]) || output[n] == '.'))
		n++;
	if (n < 3 || n > 32 || !g_ascii_isdigit(output[0]))
		return NULL;
	if (n < length && output[n] != '\n' && output[n] != '\r')
		return NULL;
	return g_strndup(output, n);
}
