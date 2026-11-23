/*
 * axis-probe-map.c - ffprobe's JSON to the model, defensively
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ffprobe's output varies with its version and with how broken the file
 * is: numbers are strings or numbers, fields come and go, and every text
 * is whatever the file's author wrote.  Nothing here assumes a field is
 * there, has the type it usually has, or is in range; see
 * axis-probe-model.h for the promises.
 */

#include "probe/axis-probe-model.h"

#include <math.h>
#include <string.h>

#include "library/axis-library-name.h"

static gint
find_main_video(const AxisMediaInfo *info);

/* ── Small readers ──────────────────────────────────────────────── */

/* Text from a string or number node, cleaned and cut; %NULL if absent or
 * empty. */
static gchar *
read_text(
	const AxisJsonNode *node,
	guint               max_chars
){
	const gchar *text = axis_json_scalar_text(node);
	gchar *clean;

	if (text == NULL)
		return NULL;
	clean = axis_library_text_clean(text, max_chars);
	if (clean != NULL) {
		g_strstrip(clean);
		if (clean[0] == '\0') {
			g_free(clean);
			return NULL;
		}
	}
	return clean;
}

/* A whole number in [@minimum, @maximum], else %AXIS_PROBE_UNKNOWN. */
static gint64
read_whole(
	const AxisJsonNode *node,
	gint64              minimum,
	gint64              maximum
){
	gint64 value;

	if (axis_json_get_integer(node, minimum, maximum, &value))
		return value;
	return AXIS_PROBE_UNKNOWN;
}

static gint
read_int(
	const AxisJsonNode *node,
	gint                minimum,
	gint                maximum
){
	return (gint)read_whole(node, minimum, maximum);
}

/* A duration in seconds ("1.023000") as milliseconds. */
static gint64
read_duration_ms(const AxisJsonNode *node)
{
	gdouble seconds;

	if (!axis_json_get_double(node, 0.0, (gdouble)AXIS_PROBE_MAX_DURATION_MS / 1000.0, &seconds))
		return AXIS_PROBE_UNKNOWN;
	return (gint64)(seconds * 1000.0 + 0.5);
}

/* A frame rate as "24000/1001" or a plain number; -1.0 when unknown. */
static gdouble
read_rate(const AxisJsonNode *node)
{
	const gchar *text = axis_json_scalar_text(node);
	gdouble value;

	if (text == NULL)
		return -1.0;
	if (strchr(text, '/') != NULL) {
		gchar *end = NULL;
		gint64 numerator;
		gint64 denominator;

		numerator = g_ascii_strtoll(text, &end, 10);
		if (end == text || *end != '/' || numerator <= 0)
			return -1.0;
		text = end + 1;
		denominator = g_ascii_strtoll(text, &end, 10);
		if (end == text || *end != '\0' || denominator <= 0)
			return -1.0;
		value = (gdouble)numerator / (gdouble)denominator;
	} else if (!axis_json_get_double(node, 0.0, AXIS_PROBE_MAX_FRAME_RATE, &value)) {
		return -1.0;
	}
	if (!(value > 0.0) || value > AXIS_PROBE_MAX_FRAME_RATE)
		return -1.0;
	return value;
}

static gint
read_flag(const AxisJsonNode *node)
{
	gboolean value;

	return axis_json_get_boolean(node, &value) && value;
}

/* A language code is a few letters, digits and dashes; anything else is
 * not one. */
static gchar *
read_language(const AxisJsonNode *node)
{
	gchar *text = read_text(node, 16);
	gchar *p;

	if (text == NULL)
		return NULL;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') {
			g_free(text);
			return NULL;
		}
	}
	return text;
}

/* ── Tags ───────────────────────────────────────────────────────── */

static void
set_first(
	gchar     **slot,
	gchar      *value
){
	if (*slot == NULL)
		*slot = value;
	else
		g_free(value);
}

/* Tags of the container and of audio streams, for the facts music needs.
 * Names are matched without regard to case; the first to give a value
 * wins, container tags before stream tags. */
static void
scan_tags(
	AxisMediaInfo      *info,
	const AxisJsonNode *tags,
	gchar             **album_artist
){
	guint length = axis_json_type(tags) == AXIS_JSON_OBJECT ? MIN(axis_json_length(tags), 1024u) : 0;
	guint i;

	for (i = 0; i < length; i++) {
		g_autofree gchar *name = g_ascii_strdown(axis_json_member_name(tags, i), 64);
		const AxisJsonNode *value = axis_json_member_value(tags, i);
		const gchar *text = axis_json_scalar_text(value);
		gboolean has_text = text != NULL && text[0] != '\0';

		if (!has_text)
			continue;
		if (strcmp(name, "title") == 0) {
			set_first(&info->tag_title, read_text(value, AXIS_PROBE_TAG_CHARS));
		} else if (strcmp(name, "artist") == 0) {
			set_first(&info->tag_artist, read_text(value, AXIS_PROBE_TAG_CHARS));
		} else if (strcmp(name, "album_artist") == 0 || strcmp(name, "albumartist") == 0) {
			set_first(album_artist, read_text(value, AXIS_PROBE_TAG_CHARS));
		} else if (strcmp(name, "album") == 0) {
			set_first(&info->tag_album, read_text(value, AXIS_PROBE_TAG_CHARS));
		} else if (strcmp(name, "track") == 0 || strcmp(name, "tracknumber") == 0 ||
		           strcmp(name, "track_number") == 0) {
			info->has_track = TRUE;
		} else if (strcmp(name, "disc") == 0 || strcmp(name, "discnumber") == 0 ||
		           strcmp(name, "disk") == 0) {
			info->has_disc = TRUE;
		} else if (g_str_has_prefix(name, "musicbrainz")) {
			info->has_musicbrainz = TRUE;
		} else if (g_str_has_prefix(name, "replaygain_") || g_str_has_prefix(name, "r128_")) {
			info->has_replaygain = TRUE;
		}
	}
}

/* A bitrate some muxers (mkvmerge) write as a tag, "BPS" or "BPS-eng". */
static gint64
read_bitrate_tag(const AxisJsonNode *tags)
{
	guint length = axis_json_type(tags) == AXIS_JSON_OBJECT ? MIN(axis_json_length(tags), 1024u) : 0;
	guint i;

	for (i = 0; i < length; i++) {
		const gchar *name = axis_json_member_name(tags, i);
		gint64 value;

		if (g_ascii_strncasecmp(name, "BPS", 3) != 0 || (name[3] != '\0' && name[3] != '-'))
			continue;
		value = read_whole(axis_json_member_value(tags, i), 1, AXIS_PROBE_MAX_BITRATE);
		if (value > 0)
			return value;
	}
	return AXIS_PROBE_UNKNOWN;
}

/* ── Streams ────────────────────────────────────────────────────── */

static AxisMediaStream *
stream_new(void)
{
	AxisMediaStream *s = g_new0(AxisMediaStream, 1);

	s->duration_ms = AXIS_PROBE_UNKNOWN;
	s->bitrate = AXIS_PROBE_UNKNOWN;
	s->width = AXIS_PROBE_UNKNOWN;
	s->height = AXIS_PROBE_UNKNOWN;
	s->bit_depth = AXIS_PROBE_UNKNOWN;
	s->frame_rate = -1.0;
	s->real_frame_rate = -1.0;
	s->dv_profile = AXIS_PROBE_UNKNOWN;
	s->dv_level = AXIS_PROBE_UNKNOWN;
	s->dv_bl_compat_id = AXIS_PROBE_UNKNOWN;
	s->dv_hdr10_compatible = AXIS_TRI_UNKNOWN;
	s->channels = AXIS_PROBE_UNKNOWN;
	s->sample_rate = AXIS_PROBE_UNKNOWN;
	s->bit_depth_audio = AXIS_PROBE_UNKNOWN;
	s->lossless = AXIS_TRI_UNKNOWN;
	s->tag_bitrate = AXIS_PROBE_UNKNOWN;
	axis_hdr_signals_init(&s->signals);

	return s;
}

static AxisStreamKind
read_kind(const AxisJsonNode *node)
{
	const gchar *text = axis_json_string(node);
	AxisStreamKind kind;

	if (text != NULL && axis_stream_kind_from_string(text, &kind))
		return kind;
	return AXIS_STREAM_UNKNOWN;
}

/* The bit depth of an audio sample, if it is known to be integer. */
static gint
read_audio_depth(const AxisJsonNode *object)
{
	gint depth = read_int(axis_json_member(object, "bits_per_raw_sample"), 1, 64);
	const gchar *format;

	if (depth > 0)
		return depth;
	depth = read_int(axis_json_member(object, "bits_per_sample"), 1, 64);
	if (depth > 0)
		return depth;
	format = axis_json_string(axis_json_member(object, "sample_fmt"));
	if (format != NULL) {
		if (strcmp(format, "u8") == 0 || strcmp(format, "u8p") == 0)
			return 8;
		if (strcmp(format, "s16") == 0 || strcmp(format, "s16p") == 0)
			return 16;
	}
	return AXIS_PROBE_UNKNOWN;
}

static AxisMediaStream *
map_stream(
	const AxisJsonNode *object,
	gint                position
){
	AxisMediaStream *s = stream_new();
	const AxisJsonNode *tags = axis_json_member(object, "tags");
	const AxisJsonNode *disposition = axis_json_member(object, "disposition");
	gint64 index = read_whole(axis_json_member(object, "index"), 0, 100000);
	gint64 value;

	s->index = index >= 0 ? (gint)index : position;
	s->kind = read_kind(axis_json_member(object, "codec_type"));
	s->codec = read_text(axis_json_member(object, "codec_name"), AXIS_PROBE_CODEC_CHARS);
	s->profile = read_text(axis_json_member(object, "profile"), AXIS_PROBE_CODEC_CHARS);
	s->duration_ms = read_duration_ms(axis_json_member(object, "duration"));
	value = read_whole(axis_json_member(object, "bit_rate"), 1, AXIS_PROBE_MAX_BITRATE);
	s->bitrate = value > 0 ? value : AXIS_PROBE_UNKNOWN;
	s->tag_bitrate = read_bitrate_tag(tags);
	s->language = read_language(axis_json_member(tags, "language"));
	s->title = read_text(axis_json_member(tags, "title"), AXIS_PROBE_TAG_CHARS);
	s->is_default = read_flag(axis_json_member(disposition, "default"));
	s->is_forced = read_flag(axis_json_member(disposition, "forced"));
	s->attached_pic = read_flag(axis_json_member(disposition, "attached_pic"));

	switch (s->kind) {
	case AXIS_STREAM_VIDEO:
		s->width = read_int(axis_json_member(object, "width"), 1, AXIS_PROBE_MAX_DIMENSION);
		s->height = read_int(axis_json_member(object, "height"), 1, AXIS_PROBE_MAX_DIMENSION);
		/* One without the other is no size. */
		if (s->width < 0 || s->height < 0)
			s->width = s->height = AXIS_PROBE_UNKNOWN;
		s->pixel_format = read_text(axis_json_member(object, "pix_fmt"), 32);
		s->bit_depth = read_int(axis_json_member(object, "bits_per_raw_sample"), 1, 64);
		if (s->bit_depth < 0)
			s->bit_depth = axis_probe_pixel_format_depth(s->pixel_format);
		s->frame_rate = read_rate(axis_json_member(object, "avg_frame_rate"));
		s->real_frame_rate = read_rate(axis_json_member(object, "r_frame_rate"));
		s->field_order = read_text(axis_json_member(object, "field_order"), 32);
		s->color_range = read_text(axis_json_member(object, "color_range"), 32);
		s->color_space = read_text(axis_json_member(object, "color_space"), 32);
		s->color_transfer = read_text(axis_json_member(object, "color_transfer"), 32);
		s->color_primaries = read_text(axis_json_member(object, "color_primaries"), 32);
		axis_probe_hdr_read_side_data(axis_json_member(object, "side_data_list"), &s->signals);
		break;
	case AXIS_STREAM_AUDIO:
		s->channels = read_int(axis_json_member(object, "channels"), 1, AXIS_PROBE_MAX_CHANNELS);
		s->channel_layout = read_text(axis_json_member(object, "channel_layout"), 64);
		s->sample_rate = read_int(axis_json_member(object, "sample_rate"), 1, AXIS_PROBE_MAX_SAMPLE_RATE);
		s->bit_depth_audio = read_audio_depth(object);
		s->lossless = axis_probe_audio_lossless(s->codec, s->profile);
		break;
	case AXIS_STREAM_SUBTITLE:
		s->subtitle_kind = axis_probe_subtitle_kind(s->codec);
		break;
	default:
		break;
	}

	return s;
}

/* ── ffprobe's own error ────────────────────────────────────────── */

static gchar *
describe_tool_error(const AxisJsonNode *error)
{
	g_autofree gchar *text = read_text(axis_json_member(error, "string"), 200);

	if (text != NULL)
		return g_strdup_printf("ffprobe could not read the file: %s.", text);
	return g_strdup("ffprobe could not read the file.");
}

/* ── Step 1: streams and container ──────────────────────────────── */

gboolean
axis_probe_map_streams(
	const gchar    *json,
	gsize           length,
	AxisMediaInfo **info_out,
	gchar         **error
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *album_artist = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *format;
	const AxisJsonNode *streams;
	const AxisJsonNode *chapters;
	guint total;
	guint i;

	*info_out = NULL;
	*error = NULL;

	doc = axis_json_parse(json, length, &problem);
	if (doc == NULL) {
		*error = g_strdup_printf("ffprobe's output could not be read (%s).", problem);
		return FALSE;
	}
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		*error = g_strdup("ffprobe's output was not what was expected.");
		return FALSE;
	}
	if (axis_json_member(root, "error") != NULL) {
		*error = describe_tool_error(axis_json_member(root, "error"));
		return FALSE;
	}

	info = axis_media_info_new();
	format = axis_json_member(root, "format");
	info->container = read_text(axis_json_member(format, "format_name"), AXIS_PROBE_NAME_CHARS);
	if (info->container == NULL) {
		*error = g_strdup("ffprobe gave no information about the container.");
		return FALSE;
	}
	info->duration_ms = read_duration_ms(axis_json_member(format, "duration"));
	info->bitrate = read_whole(axis_json_member(format, "bit_rate"), 1, AXIS_PROBE_MAX_BITRATE);
	scan_tags(info, axis_json_member(format, "tags"), &album_artist);

	chapters = axis_json_member(root, "chapters");
	if (axis_json_type(chapters) == AXIS_JSON_ARRAY)
		info->chapter_count = (gint)MIN(axis_json_length(chapters), (guint)AXIS_PROBE_MAX_CHAPTERS);

	streams = axis_json_member(root, "streams");
	total = axis_json_length(streams);
	if (axis_json_type(streams) != AXIS_JSON_ARRAY || total == 0) {
		*error = g_strdup("ffprobe found no streams in the file.");
		return FALSE;
	}
	info->stream_count = (gint)MIN(total, 100000u);
	for (i = 0; i < total && info->streams->len < AXIS_PROBE_MAX_STREAMS; i++) {
		const AxisJsonNode *object = axis_json_element(streams, i);

		if (axis_json_type(object) != AXIS_JSON_OBJECT)
			continue;
		g_ptr_array_add(info->streams, map_stream(object, (gint)i));
	}
	info->streams_omitted = info->stream_count - (gint)info->streams->len;
	if (info->streams->len == 0) {
		*error = g_strdup("ffprobe found no streams in the file.");
		return FALSE;
	}

	/* Some formats keep a file's tags with its audio stream (Ogg, Opus).
	 * For a file with a picture, that is the title of one track among
	 * several and says nothing about the file; for a music file it is
	 * what the file is called. */
	if (find_main_video(info) < 0) {
		for (i = 0; i < total && i < AXIS_PROBE_MAX_STREAMS; i++) {
			const AxisJsonNode *object = axis_json_element(streams, i);

			if (axis_json_type(object) == AXIS_JSON_OBJECT &&
			    read_kind(axis_json_member(object, "codec_type")) == AXIS_STREAM_AUDIO)
				scan_tags(info, axis_json_member(object, "tags"), &album_artist);
		}
	}
	if (info->tag_artist == NULL)
		info->tag_artist = g_steal_pointer(&album_artist);

	*info_out = g_steal_pointer(&info);
	return TRUE;
}

/* ── Step 2: the frames ─────────────────────────────────────────── */

/* The largest picture that is not a cover; -1 for none. */
static gint
find_main_video(const AxisMediaInfo *info)
{
	gint best = -1;
	gint64 best_pixels = -1;
	guint i;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
		gint64 pixels;

		if (s->kind != AXIS_STREAM_VIDEO || s->attached_pic)
			continue;
		pixels = s->width > 0 && s->height > 0 ? (gint64)s->width * s->height : 0;
		if (pixels > best_pixels) {
			best = (gint)i;
			best_pixels = pixels;
		}
	}
	return best;
}

gboolean
axis_media_info_wants_frames(
	const AxisMediaInfo *info,
	gint                *index
){
	gint main = find_main_video(info);
	const AxisMediaStream *s;

	if (main < 0)
		return FALSE;
	s = g_ptr_array_index(info->streams, main);
	if (!axis_probe_hdr_wants_frames(s))
		return FALSE;
	if (index != NULL)
		*index = s->index;
	return TRUE;
}

static gboolean
unspecified(const gchar *value)
{
	return value == NULL || strcmp(value, "unknown") == 0 || strcmp(value, "unspecified") == 0;
}

/* Fills a colour field of the stream from a frame's, or notes a conflict. */
static void
merge_colour(
	AxisMediaStream    *s,
	gchar             **field,
	const AxisJsonNode *frame_value
){
	g_autofree gchar *from_frame = read_text(frame_value, 32);

	if (unspecified(from_frame))
		return;
	if (unspecified(*field)) {
		g_free(*field);
		*field = g_steal_pointer(&from_frame);
	} else if (strcmp(*field, from_frame) != 0) {
		s->frame_color_conflict = TRUE;
	}
}

void
axis_probe_map_frames(
	AxisMediaInfo *info,
	const gchar   *json,
	gsize          length
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *frames;
	AxisMediaStream *s;
	gint main = find_main_video(info);
	guint count;
	guint i;

	info->frames_unavailable = TRUE;
	if (main < 0 || json == NULL)
		return;
	doc = axis_json_parse(json, length, NULL);
	if (doc == NULL || axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_OBJECT)
		return;
	frames = axis_json_member(axis_json_doc_get_root(doc), "frames");
	if (axis_json_type(frames) != AXIS_JSON_ARRAY || axis_json_length(frames) == 0)
		return;

	/* A frame was read. */
	info->frames_unavailable = FALSE;
	s = g_ptr_array_index(info->streams, main);
	count = MIN(axis_json_length(frames), (guint)AXIS_PROBE_MAX_FRAMES);
	for (i = 0; i < count; i++) {
		const AxisJsonNode *frame = axis_json_element(frames, i);

		if (axis_json_type(frame) != AXIS_JSON_OBJECT)
			continue;
		axis_probe_hdr_read_side_data_from(axis_json_member(frame, "side_data_list"), &s->signals, TRUE);
		merge_colour(s, &s->color_space, axis_json_member(frame, "color_space"));
		merge_colour(s, &s->color_transfer, axis_json_member(frame, "color_transfer"));
		merge_colour(s, &s->color_primaries, axis_json_member(frame, "color_primaries"));
		if (s->pixel_format == NULL) {
			s->pixel_format = read_text(axis_json_member(frame, "pix_fmt"), 32);
			if (s->bit_depth < 0)
				s->bit_depth = axis_probe_pixel_format_depth(s->pixel_format);
		}
	}
}

/* ── Step 3: bitrates ───────────────────────────────────────────── */

static gboolean
carries_media(const AxisMediaStream *s)
{
	return s->kind == AXIS_STREAM_AUDIO || (s->kind == AXIS_STREAM_VIDEO && !s->attached_pic);
}

/* A guess at an audio stream's bitrate that errs high (so that the video
 * gets less, and a later estimate of saving is not flattered); -1 if
 * there is none to make. */
static gint64
audio_guess(const AxisMediaStream *s)
{
	if (s->lossless == AXIS_TRI_YES) {
		gdouble raw;

		if (s->sample_rate <= 0 || s->channels <= 0 || s->bit_depth_audio <= 0)
			return AXIS_PROBE_UNKNOWN;
		raw = (gdouble)s->sample_rate * s->channels * s->bit_depth_audio * 0.6;
		if (raw < 1.0 || raw > (gdouble)AXIS_PROBE_MAX_BITRATE)
			return AXIS_PROBE_UNKNOWN;
		return (gint64)raw;
	}
	if (s->channels > 0)
		return (gint64)CLAMP(s->channels, 1, 12) * 64000;
	return 128000;
}

static void
estimate_bitrates(
	AxisMediaInfo *info,
	gint64         file_size
){
	GPtrArray *unknown_video = g_ptr_array_new();
	GPtrArray *unknown_audio = g_ptr_array_new();
	gint64 known = 0;
	gdouble remaining;
	guint i;

	if (info->bitrate <= 0 && file_size > 0 && info->duration_ms > 0) {
		gdouble derived = (gdouble)file_size * 8000.0 / (gdouble)info->duration_ms;

		if (derived >= 1.0 && derived <= (gdouble)AXIS_PROBE_MAX_BITRATE) {
			info->bitrate = (gint64)derived;
			info->bitrate_estimated = TRUE;
		}
	}
	if (info->bitrate <= 0)
		info->bitrate = AXIS_PROBE_UNKNOWN;

	for (i = 0; i < info->streams->len; i++) {
		AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (!carries_media(s))
			continue;
		if (s->bitrate <= 0 && s->tag_bitrate > 0) {
			s->bitrate = s->tag_bitrate;
		} else if (s->bitrate <= 0 && s->kind == AXIS_STREAM_AUDIO && s->codec != NULL &&
		           g_str_has_prefix(s->codec, "pcm_") && s->sample_rate > 0 && s->channels > 0 &&
		           s->bit_depth_audio > 0) {
			gdouble raw = (gdouble)s->sample_rate * s->channels * s->bit_depth_audio;

			if (raw <= (gdouble)AXIS_PROBE_MAX_BITRATE) {
				s->bitrate = (gint64)raw;
				s->bitrate_estimated = TRUE;
			}
		}
		if (s->bitrate > 0)
			known += s->bitrate;
		else if (s->kind == AXIS_STREAM_VIDEO)
			g_ptr_array_add(unknown_video, s);
		else
			g_ptr_array_add(unknown_audio, s);
	}

	/* What the container holds beyond the streams that state their size
	 * goes to the ones that do not, less its own overhead. */
	remaining = ((gdouble)info->bitrate - (gdouble)known) * 0.98;
	if (info->bitrate > 0 && remaining > 0 && unknown_video->len <= 1 &&
	    unknown_video->len + unknown_audio->len > 0) {
		if (unknown_video->len == 1) {
			gint64 guessed = 0;
			gboolean possible = TRUE;

			for (i = 0; i < unknown_audio->len; i++) {
				gint64 guess = audio_guess(g_ptr_array_index(unknown_audio, i));

				if (guess < 0)
					possible = FALSE;
				else
					guessed += guess;
			}
			if (possible && remaining - (gdouble)guessed >= 1.0) {
				AxisMediaStream *video = g_ptr_array_index(unknown_video, 0);

				video->bitrate = (gint64)(remaining - (gdouble)guessed);
				video->bitrate_estimated = TRUE;
				for (i = 0; i < unknown_audio->len; i++) {
					AxisMediaStream *audio = g_ptr_array_index(unknown_audio, i);

					audio->bitrate = audio_guess(audio);
					audio->bitrate_estimated = TRUE;
				}
			}
		} else {
			gdouble share = remaining / unknown_audio->len;

			if (share >= 1.0) {
				for (i = 0; i < unknown_audio->len; i++) {
					AxisMediaStream *audio = g_ptr_array_index(unknown_audio, i);

					audio->bitrate = (gint64)share;
					audio->bitrate_estimated = TRUE;
				}
			}
		}
	}

	g_ptr_array_unref(unknown_video);
	g_ptr_array_unref(unknown_audio);
}

/* ── Step 4: classification and derived facts ───────────────────── */

static gboolean
is_field_based(const gchar *order)
{
	return g_strcmp0(order, "tt") == 0 || g_strcmp0(order, "bb") == 0 || g_strcmp0(order, "tb") == 0 ||
		g_strcmp0(order, "bt") == 0;
}

static void
derive_video(AxisMediaInfo *info)
{
	const AxisMediaStream *main;
	gdouble fps;

	if (info->main_video < 0)
		return;
	main = g_ptr_array_index(info->streams, info->main_video);

	info->video_codec = g_strdup(main->codec);
	info->width = main->width;
	info->height = main->height;
	info->resolution_class = axis_probe_resolution_class(main->width, main->height);
	info->bit_depth = main->bit_depth;
	info->frame_rate = main->frame_rate;

	if (is_field_based(main->field_order))
		info->interlaced = AXIS_TRI_YES;
	else if (g_strcmp0(main->field_order, "progressive") == 0)
		info->interlaced = AXIS_TRI_NO;

	/* Bits per pixel per frame. */
	fps = main->frame_rate > 0 ? main->frame_rate :
		(main->real_frame_rate > 0 && main->real_frame_rate <= 240.0 ? main->real_frame_rate : -1.0);
	if (main->bitrate > 0 && main->width > 0 && main->height > 0 && fps > 0) {
		gdouble bpp = (gdouble)main->bitrate / ((gdouble)main->width * main->height * fps);

		if (isfinite(bpp) && bpp >= 0 && bpp < 1000000.0) {
			info->bits_per_pixel = bpp;
			info->bits_per_pixel_estimated = main->bitrate_estimated;
		}
	}

	/* Variable frame rate: the rate that fits every timestamp is far from
	 * the average.  Field rates of interlaced video are twice the frame
	 * rate and are not variable. */
	if (main->frame_rate > 0 && main->real_frame_rate > 0) {
		gdouble ratio = main->real_frame_rate / main->frame_rate;

		if (ratio > 0.99 && ratio < 1.01)
			info->vfr_suspected = AXIS_TRI_NO;
		else if (info->interlaced == AXIS_TRI_YES && ratio > 1.98 && ratio < 2.02)
			info->vfr_suspected = AXIS_TRI_NO;
		else
			info->vfr_suspected = AXIS_TRI_YES;
	}
}

static void
derive_audio(AxisMediaInfo *info)
{
	const AxisMediaStream *main;
	gint64 total = 0;
	gboolean complete = TRUE;
	gboolean estimated = FALSE;
	guint i;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_AUDIO)
			continue;
		if (info->main_audio < 0 || (s->is_default && !((AxisMediaStream *)g_ptr_array_index(
		                                  info->streams, info->main_audio))->is_default))
			info->main_audio = (gint)i;
		if (s->bitrate > 0) {
			total += s->bitrate;
			estimated = estimated || s->bitrate_estimated;
		} else {
			complete = FALSE;
		}
	}
	if (info->main_audio < 0)
		return;
	main = g_ptr_array_index(info->streams, info->main_audio);

	info->audio_codec = g_strdup(main->codec);
	info->audio_channels = main->channels;
	info->audio_sample_rate = main->sample_rate;
	info->audio_bit_depth = main->bit_depth_audio;
	info->audio_lossless = main->lossless;
	if (complete && total > 0 && total <= AXIS_PROBE_MAX_BITRATE) {
		info->audio_bitrate_total = total;
		info->audio_bitrate_total_estimated = estimated;
	}
}

void
axis_media_info_finish(
	AxisMediaInfo *info,
	gint64         file_size
){
	AxisHdrFormat file_format = AXIS_HDR_NONE;
	gboolean file_uncertain = FALSE;
	const gchar *file_note = NULL;
	gboolean wants;
	guint i;

	info->video_streams = 0;
	info->audio_streams = 0;
	info->subtitle_streams = 0;

	estimate_bitrates(info, file_size);
	info->main_video = find_main_video(info);
	wants = info->main_video >= 0 &&
		axis_probe_hdr_wants_frames(g_ptr_array_index(info->streams, info->main_video));

	for (i = 0; i < info->streams->len; i++) {
		AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		switch (s->kind) {
		case AXIS_STREAM_VIDEO:
			if (s->attached_pic) {
				if (!info->has_embedded_art) {
					info->has_embedded_art = TRUE;
					info->art_width = s->width;
					info->art_height = s->height;
				}
				break;
			}
			info->video_streams++;
			/* The main stream had its frame looked at when that was
			 * wanted; the others never are. */
			axis_probe_hdr_classify(s, (gint)i == info->main_video ?
				(!wants || !info->frames_unavailable) : FALSE);
			if (s->hdr_format > file_format)
				file_format = s->hdr_format;
			if (s->hdr_format == AXIS_HDR_NONE || s->hdr_uncertain) {
				file_uncertain = TRUE;
				if (file_note == NULL)
					file_note = s->hdr_note;
			}
			break;
		case AXIS_STREAM_AUDIO:
			info->audio_streams++;
			break;
		case AXIS_STREAM_SUBTITLE:
			info->subtitle_streams++;
			break;
		default:
			break;
		}
	}

	info->hdr_format = file_format;
	info->hdr_uncertain = file_uncertain;
	info->hdr_note = file_uncertain ? file_note : NULL;

	derive_video(info);
	derive_audio(info);
}

/* ── All of it ──────────────────────────────────────────────────── */

gboolean
axis_probe_map(
	const gchar    *streams_json,
	gsize           streams_length,
	const gchar    *frames_json,
	gsize           frames_length,
	gint64          file_size,
	AxisMediaInfo **info,
	gchar         **error
){
	if (!axis_probe_map_streams(streams_json, streams_length, info, error))
		return FALSE;
	if (axis_media_info_wants_frames(*info, NULL))
		axis_probe_map_frames(*info, frames_json, frames_length);
	axis_media_info_finish(*info, file_size);
	return TRUE;
}
