/*
 * test-compat-jellyfin.c - the Jellyfin parsers, the table of stated
 * reasons, the evidence made of them, and the verdicts that honour the
 * property a reason blames; no database, no network
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The recorded answers of a real server (tests/fixtures/compat/jellyfin/
 * recorded) and the constructed ones beside them (constructed/, labelled in
 * the README there) are read from the project root.
 *
 * Spec scenarios: Stated codec reason; Stated bandwidth reason; Unknown
 * reason; Container-only failure; Not yet seen.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "compat/axis-compat-evidence.h"
#include "compat/axis-compat-jellyfin.h"
#include "compat/axis-compat-plex.h"
#include "compat/axis-compat-reasons.h"
#include "compat/axis-compat-verdict.h"

#define RECORDED "tests/fixtures/compat/jellyfin/recorded/"
#define CONSTRUCTED "tests/fixtures/compat/jellyfin/constructed/"

#define V AXIS_COMPAT_STREAM_VIDEO
#define A AXIS_COMPAT_STREAM_AUDIO
#define S AXIS_COMPAT_STREAM_SUBTITLE

static gchar *
read_file(
	const gchar *dir,
	const gchar *name
){
	g_autofree gchar *path = g_strconcat(dir, name, NULL);
	gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

static GPtrArray *
sessions_of(
	const gchar *dir,
	const gchar *name
){
	g_autofree gchar *text = read_file(dir, name);
	g_autofree gchar *problem = NULL;
	GPtrArray *sessions = axis_compat_jellyfin_parse_sessions(text, strlen(text), &problem);

	g_assert_nonnull(sessions);
	return sessions;
}

static const AxisCompatSession *
session_of_device(
	GPtrArray   *sessions,
	const gchar *device
){
	guint i;

	for (i = 0; i < sessions->len; i++) {
		const AxisCompatSession *s = g_ptr_array_index(sessions, i);

		if (g_strcmp0(s->device_client_id, device) == 0)
			return s;
	}
	g_error("no session of %s", device);
	return NULL;
}

static const AxisCompatStream *
stream_of(
	const AxisCompatSession *s,
	gint                     type,
	guint                    nth
){
	guint i;

	for (i = 0; i < s->streams->len; i++) {
		const AxisCompatStream *stream = g_ptr_array_index(s->streams, i);

		if (stream->type == type && nth-- == 0)
			return stream;
	}
	return NULL;
}

/* The evidence row for a kind of stream and property, or NULL. */
static const AxisCompatEvidence *
row_of(
	GPtrArray          *evidence,
	gint                kind,
	AxisCompatProperty  property
){
	guint i;

	for (i = 0; i < evidence->len; i++) {
		const AxisCompatEvidence *e = g_ptr_array_index(evidence, i);

		if (e->kind == kind && e->property == property)
			return e;
	}
	return NULL;
}

static void
assert_clean(const gchar *text)
{
	const guchar *p;

	if (text == NULL)
		return;
	g_assert_true(g_utf8_validate(text, -1, NULL));
	for (p = (const guchar *)text; *p != '\0'; p++)
		g_assert_true(*p >= 0x20 && *p != 0x7f);
}

/* ── Parsers on the recorded answers ────────────────────────────── */

static void
test_parse_identity(void)
{
	g_autofree gchar *info = read_file(RECORDED, "identity.json");
	g_autofree gchar *pub = read_file(RECORDED, "public.json");
	AxisCompatIdentity identity;
	g_autofree gchar *problem = NULL;

	memset(&identity, 0, sizeof(identity));
	g_assert_true(axis_compat_jellyfin_parse_identity(info, strlen(info), &identity, &problem));
	g_assert_cmpstr(identity.machine_id, ==, "df1a584d4cef4af88f0d75e7a582ff28");
	g_assert_cmpstr(identity.version, ==, "10.11.8");
	g_assert_cmpstr(identity.name, ==, "83b9dc12ff49");
	axis_compat_identity_clear(&identity);
	g_assert_true(axis_compat_jellyfin_parse_identity(pub, strlen(pub), &identity, &problem));
	g_assert_cmpstr(identity.machine_id, ==, "df1a584d4cef4af88f0d75e7a582ff28");
	axis_compat_identity_clear(&identity);

	/* An object that does not say which server it is is not refused here
	 * (the caller refuses it); what is not an object is. */
	g_assert_true(axis_compat_jellyfin_parse_identity("{}", 2, &identity, &problem));
	g_assert_null(identity.machine_id);
	axis_compat_identity_clear(&identity);
	g_assert_false(axis_compat_jellyfin_parse_identity("[]", 2, &identity, &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);
	g_assert_false(axis_compat_jellyfin_parse_identity("<html>hello</html>", 18, &identity, &problem));
	g_assert_null(strstr(problem, "hello"));
	g_clear_pointer(&problem, g_free);
	g_assert_false(axis_compat_jellyfin_parse_identity("{\"MediaContainer\":{}}x", 22, &identity, &problem));
}

static void
test_parse_libraries(void)
{
	g_autofree gchar *text = read_file(RECORDED, "libraries.json");
	g_autofree gchar *mixed = read_file(CONSTRUCTED, "libraries-mixed.json");
	g_autofree gchar *problem = NULL;
	GPtrArray *libraries = axis_compat_jellyfin_parse_libraries(text, strlen(text), &problem);
	const AxisCompatLibrary *l;

	g_assert_nonnull(libraries);
	g_assert_cmpuint(libraries->len, ==, 2);
	l = g_ptr_array_index(libraries, 0);
	g_assert_cmpstr(l->key, ==, "db4c1708cbb5dd1676284a40f2950aba");
	g_assert_cmpstr(l->kind, ==, "movie");
	g_assert_cmpstr(l->title, ==, "Films");
	g_assert_cmpuint(l->folders->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(l->folders, 0), ==, "/media/films");
	l = g_ptr_array_index(libraries, 1);
	g_assert_cmpstr(l->kind, ==, "artist");
	g_assert_cmpstr(l->title, ==, "Music");
	g_ptr_array_unref(libraries);

	/* Mixed content states no collection type; one that cannot be told
	 * apart (no id) is dropped; locations that are not text are dropped. */
	libraries = axis_compat_jellyfin_parse_libraries(mixed, strlen(mixed), &problem);
	g_assert_nonnull(libraries);
	g_assert_cmpuint(libraries->len, ==, 4);
	l = g_ptr_array_index(libraries, 0);
	g_assert_cmpstr(l->kind, ==, "other");
	g_assert_cmpuint(l->folders->len, ==, 2);
	g_assert_cmpstr(((AxisCompatLibrary *)g_ptr_array_index(libraries, 1))->kind, ==, "show");
	g_assert_cmpstr(((AxisCompatLibrary *)g_ptr_array_index(libraries, 2))->key, ==, "aa000000000000000000000000000003");
	l = g_ptr_array_index(libraries, 3);
	g_assert_cmpuint(l->folders->len, ==, 2);	/* "7" is text; null is not */
	g_ptr_array_unref(libraries);

	g_assert_null(axis_compat_jellyfin_parse_libraries("{}", 2, &problem));
	g_clear_pointer(&problem, g_free);
	g_assert_null(axis_compat_jellyfin_parse_libraries("nope", 4, &problem));
}

static void
test_parse_devices(void)
{
	g_autofree gchar *text = read_file(RECORDED, "devices-playing.json");
	g_autofree gchar *problem = NULL;
	GPtrArray *devices = axis_compat_jellyfin_parse_devices(text, strlen(text), &problem);
	const AxisCompatDevice *d;

	g_assert_nonnull(devices);
	g_assert_cmpuint(devices->len, ==, 8);
	d = g_ptr_array_index(devices, 0);
	g_assert_cmpstr(d->client_id, ==, "axis-test-speaker");
	g_assert_cmpstr(d->name, ==, "Kitchen Speaker");
	g_assert_cmpstr(d->product, ==, "AxisTestSpeaker");
	g_assert_cmpint(d->plex_id, ==, -1);
	g_ptr_array_unref(devices);
	g_assert_null(axis_compat_jellyfin_parse_devices("[]", 2, &problem));
}

static void
test_parse_sessions_recorded(void)
{
	GPtrArray *sessions = sessions_of(RECORDED, "sessions-playing.json");
	const AxisCompatSession *s;
	const AxisCompatStream *st;

	/* Seven clients play; the eighth is signed in and plays nothing. */
	g_assert_cmpuint(sessions->len, ==, 7);

	s = session_of_device(sessions, "axis-test-tv");
	g_assert_cmpstr(s->session_key, ==, "5ea38503b216d9179d5bfd42cf69486d");
	g_assert_cmpstr(s->rating_key, ==, "ca8139a7c41a55835046264d60e1a618");
	g_assert_cmpstr(s->item_type, ==, "movie");
	g_assert_cmpstr(s->device_name, ==, "Living Room TV");
	g_assert_cmpstr(s->product, ==, "AxisTestPlayer");
	g_assert_cmpstr(s->account_id, ==, "00a92bb1464e4f77bd70b5f880fdd6ef");
	g_assert_cmpstr(s->account_name, ==, "axis-test");
	g_assert_null(s->library_key);
	g_assert_cmpstr(s->container, ==, "mkv");
	g_assert_cmpstr(s->file, ==, "/media/films/Test Film H264 (2024)/Test Film H264 (2024).mkv");
	g_assert_cmpint(s->part_decision, ==, AXIS_COMPAT_DECISION_DIRECT);
	g_assert_false(s->transcode.present);
	g_assert_null(s->transcode.reasons);
	g_assert_false(s->ambiguous);
	g_assert_cmpint(s->player_local, ==, AXIS_COMPAT_UNSTATED);
	g_assert_cmpint(s->bandwidth_kbps, ==, -1);
	g_assert_cmpuint(s->streams->len, ==, 2);
	st = stream_of(s, V, 0);
	g_assert_cmpstr(st->codec, ==, "h264");
	g_assert_cmpstr(st->profile, ==, "constrained baseline");
	g_assert_cmpint(st->bit_depth, ==, 8);
	g_assert_cmpint(st->width, ==, 640);
	g_assert_cmpint(st->height, ==, 360);
	g_assert_cmpint(st->hdr, ==, AXIS_COMPAT_NO);
	st = stream_of(s, A, 0);
	g_assert_cmpstr(st->codec, ==, "aac");
	g_assert_cmpint(st->channels, ==, 1);

	s = session_of_device(sessions, "axis-test-phone");
	g_assert_true(s->transcode.present);
	g_assert_cmpint(s->part_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_assert_cmpint(s->transcode.video_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_assert_cmpint(s->transcode.audio_decision, ==, AXIS_COMPAT_DECISION_COPY);
	g_assert_cmpstr(s->transcode.video_codec, ==, "h264");
	g_assert_cmpstr(s->transcode.container, ==, "ts");
	g_assert_cmpuint(s->transcode.reasons->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(s->transcode.reasons, 0), ==, "ContainerNotSupported");
	g_assert_cmpstr(g_ptr_array_index(s->transcode.reasons, 1), ==, "VideoCodecNotSupported");
	st = stream_of(s, V, 0);
	g_assert_cmpstr(st->codec, ==, "hevc");
	g_assert_cmpint(st->bit_depth, ==, 10);
	g_assert_cmpstr(st->profile, ==, "main 10");

	s = session_of_device(sessions, "axis-test-stick");
	g_assert_cmpint(s->transcode.video_decision, ==, AXIS_COMPAT_DECISION_COPY);
	g_assert_cmpint(s->transcode.audio_decision, ==, AXIS_COMPAT_DECISION_COPY);
	s = session_of_device(sessions, "axis-test-hotel");
	g_assert_cmpint(s->transcode.video_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_assert_cmpint(s->transcode.audio_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	s = session_of_device(sessions, "axis-test-console");
	g_assert_cmpint(s->transcode.video_decision, ==, AXIS_COMPAT_DECISION_COPY);
	g_assert_cmpint(s->transcode.audio_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	/* The client did not report its audio track: nothing says which. */
	g_assert_cmpint(stream_of(s, A, 0)->selected, ==, AXIS_COMPAT_UNSTATED);
	g_assert_cmpint(stream_of(s, A, 1)->selected, ==, AXIS_COMPAT_UNSTATED);
	g_assert_cmpuint(s->streams->len, ==, 4);
	s = session_of_device(sessions, "axis-test-speaker");
	g_assert_cmpstr(s->item_type, ==, "audio");
	g_assert_cmpstr(s->container, ==, "flac");
	g_assert_cmpstr(stream_of(s, A, 0)->codec, ==, "flac");
	g_assert_cmpint(stream_of(s, A, 0)->bit_depth, ==, 16);
	g_ptr_array_unref(sessions);

	sessions = sessions_of(RECORDED, "sessions-idle.json");
	g_assert_cmpuint(sessions->len, ==, 0);
	g_ptr_array_unref(sessions);
}

static void
test_parse_sessions_constructed(void)
{
	GPtrArray *sessions = sessions_of(CONSTRUCTED, "session-audio-selected.json");
	const AxisCompatSession *s = g_ptr_array_index(sessions, 0);

	g_assert_cmpint(stream_of(s, A, 0)->selected, ==, AXIS_COMPAT_NO);
	g_assert_cmpint(stream_of(s, A, 1)->selected, ==, AXIS_COMPAT_YES);
	g_ptr_array_unref(sessions);

	sessions = sessions_of(CONSTRUCTED, "session-subtitle-burned.json");
	s = g_ptr_array_index(sessions, 0);
	g_assert_cmpint(stream_of(s, S, 0)->selected, ==, AXIS_COMPAT_YES);
	g_assert_cmpint(s->transcode.subtitle_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_ptr_array_unref(sessions);

	/* Another version of the item than the one whose streams are listed. */
	sessions = sessions_of(CONSTRUCTED, "session-other-version.json");
	s = g_ptr_array_index(sessions, 0);
	g_assert_true(s->ambiguous);
	g_ptr_array_unref(sessions);

	/* A conversion with no TranscodingInfo is not an explained one. */
	sessions = sessions_of(CONSTRUCTED, "session-no-transcoding-info.json");
	s = g_ptr_array_index(sessions, 0);
	g_assert_false(s->transcode.present);
	g_assert_cmpint(s->part_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_ptr_array_unref(sessions);
}

static void
test_parse_sessions_odd_input(void)
{
	GPtrArray *sessions = sessions_of(CONSTRUCTED, "session-odd-ids.json");
	GPtrArray *again = sessions_of(CONSTRUCTED, "session-odd-ids.json");
	const AxisCompatSession *s;
	guint i;
	guint j;

	/* The string that is not a session is dropped; the rest are sessions. */
	g_assert_cmpuint(sessions->len, ==, 4);

	/* A web client's long device id becomes a key that is the same again. */
	s = g_ptr_array_index(sessions, 0);
	g_assert_true(g_str_has_prefix(s->device_client_id, "h:"));
	g_assert_cmpuint(strlen(s->device_client_id), ==, 42);
	g_assert_true(axis_compat_plex_id_valid(s->device_client_id));
	g_assert_cmpstr(s->device_client_id, ==, ((AxisCompatSession *)g_ptr_array_index(again, 0))->device_client_id);
	g_assert_cmpint(s->part_decision, ==, AXIS_COMPAT_DECISION_DIRECT);

	/* A number is text; a null is nothing; a control character refuses an id. */
	s = g_ptr_array_index(sessions, 1);
	g_assert_cmpstr(s->session_key, ==, "12345");
	g_assert_null(s->device_client_id);
	s = g_ptr_array_index(sessions, 2);
	g_assert_null(s->device_client_id);

	/* Out-of-range numbers are not stated. */
	s = g_ptr_array_index(sessions, 3);
	g_assert_cmpuint(s->streams->len, ==, 2);
	{
		const AxisCompatStream *st = g_ptr_array_index(s->streams, 0);

		g_assert_cmpint(st->bit_depth, ==, -1);
		g_assert_cmpint(st->width, ==, -1);
		g_assert_cmpint(st->height, ==, -1);
		g_assert_cmpint(st->bitrate_kbps, ==, -1);
		g_assert_cmpint(st->selected, ==, AXIS_COMPAT_UNSTATED);
	}
	for (i = 0; i < sessions->len; i++) {
		s = g_ptr_array_index(sessions, i);
		assert_clean(s->session_key);
		assert_clean(s->rating_key);
		assert_clean(s->device_name);
		assert_clean(s->file);
		for (j = 0; j < s->streams->len; j++)
			assert_clean(((AxisCompatStream *)g_ptr_array_index(s->streams, j))->codec);
	}
	g_ptr_array_unref(sessions);
	g_ptr_array_unref(again);
}

static void
test_parse_item(void)
{
	g_autofree gchar *one = read_file(CONSTRUCTED, "item-one.json");
	g_autofree gchar *many = read_file(RECORDED, "items.json");
	g_autofree gchar *problem = NULL;
	AxisCompatItem item;

	memset(&item, 0, sizeof(item));
	g_assert_true(axis_compat_jellyfin_parse_item(one, strlen(one), &item, &problem));
	g_assert_cmpstr(item.file, ==, "/media/films/Test Film H264 (2024)/Test Film H264 (2024).mkv");
	g_assert_cmpint(item.n_parts, ==, 1);
	axis_compat_item_clear(&item);
	/* More than one item for the one id: which is not known. */
	g_assert_true(axis_compat_jellyfin_parse_item(many, strlen(many), &item, &problem));
	g_assert_null(item.file);
	g_assert_cmpint(item.n_parts, ==, 0);
	axis_compat_item_clear(&item);
	g_assert_true(axis_compat_jellyfin_parse_item("{}", 2, &item, &problem));
	g_assert_null(item.file);
	g_assert_false(axis_compat_jellyfin_parse_item("[]", 2, &item, &problem));
}

static void
test_keys(void)
{
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *longest = g_strnfill(600, 'x');
	g_autofree gchar *long_valid = g_strnfill(200, 'y');

	a = axis_compat_jellyfin_key("axis-test-tv");
	g_assert_cmpstr(a, ==, "axis-test-tv");
	g_clear_pointer(&a, g_free);
	a = axis_compat_jellyfin_key(long_valid);
	b = axis_compat_jellyfin_key(long_valid);
	g_assert_true(g_str_has_prefix(a, "h:"));
	g_assert_cmpstr(a, ==, b);
	g_assert_true(axis_compat_plex_id_valid(a));
	c = axis_compat_jellyfin_key("not/valid+at=all");
	g_assert_true(g_str_has_prefix(c, "h:"));
	g_assert_cmpstr(c, !=, a);
	g_clear_pointer(&c, g_free);
	g_assert_null(axis_compat_jellyfin_key(NULL));
	g_assert_null(axis_compat_jellyfin_key(""));
	g_assert_null(axis_compat_jellyfin_key("a\nb"));
	g_assert_null(axis_compat_jellyfin_key("a\x7f"));
	g_assert_null(axis_compat_jellyfin_key("\xff\xfe"));
	g_assert_null(axis_compat_jellyfin_key(longest));
	/* A key that climbs out of a path is not a valid id and is hashed. */
	c = axis_compat_jellyfin_key("../x");
	g_assert_true(g_str_has_prefix(c, "h:"));
}

static void
test_parse_hostile_input(void)
{
	GString *json = g_string_new("[");
	g_autofree gchar *problem = NULL;
	GPtrArray *sessions;
	const AxisCompatSession *s;
	guint i;

	/* More sessions than the limit, each with more reasons than the limit
	 * and one far too long. */
	for (i = 0; i < 300; i++) {
		guint j;

		g_string_append_printf(json, "%s{\"Id\":\"s%u\",\"DeviceId\":\"d%u\",\"NowPlayingItem\":{\"Id\":\"i%u\","
			"\"Type\":\"Movie\"},\"TranscodingInfo\":{\"IsVideoDirect\":false,\"TranscodeReasons\":[",
			i > 0 ? "," : "", i, i, i);
		for (j = 0; j < 100; j++)
			g_string_append_printf(json, "%s\"Reason%u\"", j > 0 ? "," : "", j);
		g_string_append_printf(json, ",\"%0200u\"]}}", 0u);
	}
	g_string_append_c(json, ']');
	sessions = axis_compat_jellyfin_parse_sessions(json->str, json->len, &problem);
	g_assert_nonnull(sessions);
	g_assert_cmpuint(sessions->len, ==, AXIS_COMPAT_MAX_SESSIONS);
	s = g_ptr_array_index(sessions, 0);
	g_assert_cmpuint(s->transcode.reasons->len, ==, AXIS_COMPAT_MAX_REASONS);
	g_ptr_array_unref(sessions);

	g_string_assign(json, "[{\"Id\":\"s\",\"DeviceId\":\"d\",\"NowPlayingItem\":{\"Id\":\"i\"},"
		"\"TranscodingInfo\":{\"TranscodeReasons\":[\"");
	for (i = 0; i < 200; i++)
		g_string_append(json, "R");
	g_string_append(json, "\"]}}]");
	sessions = axis_compat_jellyfin_parse_sessions(json->str, json->len, &problem);
	s = g_ptr_array_index(sessions, 0);
	g_assert_cmpuint(strlen(g_ptr_array_index(s->transcode.reasons, 0)), ==, AXIS_COMPAT_MAX_REASON_CHARS);
	g_ptr_array_unref(sessions);

	/* Nonsense is refused whole, with a problem that quotes none of it. */
	g_assert_null(axis_compat_jellyfin_parse_sessions("{\"secret-word\":1}", 17, &problem));
	g_assert_nonnull(problem);
	g_assert_null(strstr(problem, "secret-word"));
	g_clear_pointer(&problem, g_free);
	g_assert_null(axis_compat_jellyfin_parse_sessions("[[[[[[[[[[[[[[[[[[[[[[[[[[[[", 28, &problem));
	g_clear_pointer(&problem, g_free);
	g_assert_null(axis_compat_jellyfin_parse_sessions("", 0, &problem));
	g_string_free(json, TRUE);
}

/* ── The table of reasons ───────────────────────────────────────── */

typedef struct
{
	const gchar          *name;
	AxisCompatReasonScope scope;
	AxisCompatEffect      effect;
	AxisCompatProperty    property;
} Expected;

/* Written out here, apart from the table in the source: what each of the
 * server's reasons proves about the device. */
static const Expected expected[] = {
	{ "ContainerNotSupported", AXIS_REASON_CONTAINER, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_CONTAINER },
	{ "VideoCodecNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_NONE },
	{ "AudioCodecNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_NONE },
	{ "SubtitleCodecNotSupported", AXIS_REASON_SUBTITLE, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_SUBTITLE_FORMAT },
	{ "AudioIsExternal", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "SecondaryAudioNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "VideoProfileNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_PROFILE },
	{ "VideoLevelNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_LEVEL },
	{ "VideoResolutionNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_LIMIT, AXIS_PROPERTY_NONE },
	{ "VideoBitDepthNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_BIT_DEPTH },
	{ "VideoFramerateNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "RefFramesNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AnamorphicVideoNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "InterlacedVideoNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AudioChannelsNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AudioProfileNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_PROFILE },
	{ "AudioSampleRateNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AudioBitDepthNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "ContainerBitrateExceedsLimit", AXIS_REASON_ANY, AXIS_EFFECT_CONVERTED_LIMIT, AXIS_PROPERTY_NONE },
	{ "VideoBitrateNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_LIMIT, AXIS_PROPERTY_NONE },
	{ "AudioBitrateNotSupported", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_LIMIT, AXIS_PROPERTY_NONE },
	{ "UnknownVideoStreamInfo", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "UnknownAudioStreamInfo", AXIS_REASON_AUDIO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "DirectPlayError", AXIS_REASON_ANY, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "VideoRangeTypeNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "VideoCodecTagNotSupported", AXIS_REASON_VIDEO, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE },
	{ "StreamCountExceedsLimit", AXIS_REASON_ANY, AXIS_EFFECT_CONVERTED_UNCLEAR, AXIS_PROPERTY_NONE }
};

/* Spec: Stated codec reason / Stated bandwidth reason: each reason maps as
 * the table says. */
static void
test_each_reason_maps_as_the_table_says(void)
{
	guint i;

	g_assert_cmpuint(axis_compat_reason_count(), ==, G_N_ELEMENTS(expected));
	for (i = 0; i < G_N_ELEMENTS(expected); i++) {
		const AxisCompatReason *r = axis_compat_reason_lookup(expected[i].name);

		g_assert_nonnull(r);
		g_assert_cmpstr(r->name, ==, expected[i].name);
		g_assert_cmpint(r->scope, ==, expected[i].scope);
		g_assert_cmpint(r->effect, ==, expected[i].effect);
		g_assert_cmpint(r->property, ==, expected[i].property);
		/* A property is blamed only by a codec-level failure. */
		if (r->property != AXIS_PROPERTY_NONE)
			g_assert_cmpint(r->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	}
}

/* The server's own list (recorded from its API description) and the table
 * are the same set. */
static void
test_the_table_is_the_servers_list(void)
{
	g_autofree gchar *text = read_file(RECORDED, "api-description.json");
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *root;
	JsonArray *list;
	guint i;

	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	root = json_node_get_object(json_parser_get_root(parser));
	list = json_object_get_array_member(root, "TranscodeReason");
	g_assert_cmpuint(json_array_get_length(list), ==, axis_compat_reason_count());
	for (i = 0; i < json_array_get_length(list); i++)
		g_assert_nonnull(axis_compat_reason_lookup(json_array_get_string_element(list, i)));
	for (i = 0; i < axis_compat_reason_count(); i++) {
		guint j;
		gboolean found = FALSE;

		for (j = 0; j < json_array_get_length(list); j++)
			found = found || g_strcmp0(json_array_get_string_element(list, j), axis_compat_reason_at(i)->name) == 0;
		g_assert_true(found);
	}
}

/* Spec: Unknown reason: not in the table, so cause unclear. */
static void
test_a_reason_the_table_does_not_know_is_not_in_it(void)
{
	g_assert_null(axis_compat_reason_lookup("SomethingTheNextVersionInvented"));
	g_assert_null(axis_compat_reason_lookup("containernotsupported"));
	g_assert_null(axis_compat_reason_lookup(" ContainerNotSupported"));
	g_assert_null(axis_compat_reason_lookup(""));
	g_assert_null(axis_compat_reason_lookup(NULL));
	g_assert_null(axis_compat_reason_at(axis_compat_reason_count()));
	/* It could be about any stream. */
	g_assert_true(axis_compat_reason_applies(NULL, V));
	g_assert_true(axis_compat_reason_applies(NULL, A));
	g_assert_true(axis_compat_reason_applies(NULL, S));
	g_assert_true(axis_compat_reason_applies(axis_compat_reason_lookup("VideoCodecNotSupported"), V));
	g_assert_false(axis_compat_reason_applies(axis_compat_reason_lookup("VideoCodecNotSupported"), A));
	g_assert_false(axis_compat_reason_applies(axis_compat_reason_lookup("SubtitleCodecNotSupported"), V));
	g_assert_true(axis_compat_reason_applies(axis_compat_reason_lookup("ContainerBitrateExceedsLimit"), S));
	g_assert_false(axis_compat_reason_applies(axis_compat_reason_lookup("ContainerNotSupported"), V));
}

/* ── Evidence ───────────────────────────────────────────────────── */

/* A session of an H.264 8-bit film with AAC and SubRip, converted in all its
 * streams with these reasons (NULL: none stated). */
static AxisCompatSession *
made_session(
	const gchar *const *reasons,
	AxisCompatDecision  video,
	AxisCompatDecision  audio,
	AxisCompatDecision  subtitle
){
	AxisCompatSession *s = g_new0(AxisCompatSession, 1);
	AxisCompatStream *v = g_new0(AxisCompatStream, 1);
	AxisCompatStream *a = g_new0(AxisCompatStream, 1);
	AxisCompatStream *t = g_new0(AxisCompatStream, 1);
	guint i;

	s->streams = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_stream_free);
	s->media_bitrate_kbps = -1;
	s->bandwidth_kbps = -1;
	s->player_local = AXIS_COMPAT_UNSTATED;
	s->container = g_strdup("mkv");
	s->part_decision = AXIS_COMPAT_DECISION_TRANSCODE;
	v->type = V;
	v->codec = g_strdup("h264");
	v->profile = g_strdup("high");
	v->bit_depth = 8;
	v->width = 1920;
	v->height = 1080;
	v->channels = -1;
	v->bitrate_kbps = -1;
	v->selected = AXIS_COMPAT_UNSTATED;
	v->hdr = AXIS_COMPAT_NO;
	a->type = A;
	a->codec = g_strdup("aac");
	a->bit_depth = a->width = a->height = -1;
	a->channels = 2;
	a->bitrate_kbps = -1;
	a->selected = AXIS_COMPAT_UNSTATED;
	a->hdr = AXIS_COMPAT_UNSTATED;
	t->type = S;
	t->codec = g_strdup("subrip");
	t->bit_depth = t->width = t->height = t->channels = -1;
	t->bitrate_kbps = -1;
	t->selected = AXIS_COMPAT_UNSTATED;
	t->hdr = AXIS_COMPAT_UNSTATED;
	g_ptr_array_add(s->streams, v);
	g_ptr_array_add(s->streams, a);
	g_ptr_array_add(s->streams, t);
	s->transcode.present = TRUE;
	s->transcode.video_decision = video;
	s->transcode.audio_decision = audio;
	s->transcode.subtitle_decision = subtitle;
	s->transcode.video_codec = g_strdup("h264");
	s->transcode.audio_codec = g_strdup("aac");
	if (reasons != NULL) {
		s->transcode.reasons = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; reasons[i] != NULL; i++)
			g_ptr_array_add(s->transcode.reasons, g_strdup(reasons[i]));
	}
	return s;
}

static GPtrArray *
evidence_of_reasons(
	const gchar *const *reasons,
	AxisCompatDecision  video,
	AxisCompatDecision  audio,
	AxisCompatDecision  subtitle
){
	AxisCompatSession *s = made_session(reasons, video, audio, subtitle);
	GPtrArray *evidence = axis_compat_classify_session(s, NULL);

	axis_compat_session_free(s);
	return evidence;
}

/* Spec: Stated codec reason, Stated bandwidth reason, Container-only
 * failure: every reason in the table, alone, gives the evidence the table
 * says and none that it does not. */
static void
test_every_reason_gives_the_evidence_the_table_says(void)
{
	static const AxisCompatDecision T = AXIS_COMPAT_DECISION_TRANSCODE;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(expected); i++) {
		const Expected *x = &expected[i];
		const gchar *reasons[2];
		GPtrArray *evidence;
		gint kinds[3] = { V, A, S };
		guint k;

		reasons[0] = x->name;
		reasons[1] = NULL;
		evidence = evidence_of_reasons(reasons, T, T, T);
		for (k = 0; k < 3; k++) {
			gint kind = kinds[k];
			gboolean applies = x->scope == AXIS_REASON_ANY ||
				(x->scope == AXIS_REASON_VIDEO && kind == V) || (x->scope == AXIS_REASON_AUDIO && kind == A) ||
				(x->scope == AXIS_REASON_SUBTITLE && kind == S);
			const AxisCompatEvidence *base = row_of(evidence, kind, AXIS_PROPERTY_NONE);
			const AxisCompatEvidence *blamed = x->property != AXIS_PROPERTY_NONE ?
				row_of(evidence, kind, x->property) : NULL;

			if (x->scope == AXIS_REASON_CONTAINER) {
				/* It explains no stream's conversion: cause unclear for
				 * each, and the container is blamed beside the main stream. */
				g_assert_nonnull(base);
				g_assert_cmpint(base->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
				g_assert_null(base->stated_reason);
				if (kind == V) {
					const AxisCompatEvidence *c = row_of(evidence, V, AXIS_PROPERTY_CONTAINER);

					g_assert_nonnull(c);
					g_assert_cmpint(c->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
					g_assert_cmpstr(c->container, ==, "mkv");
					g_assert_cmpstr(c->stated_reason, ==, x->name);
				} else {
					g_assert_null(row_of(evidence, kind, AXIS_PROPERTY_CONTAINER));
				}
			} else if (!applies && x->scope == AXIS_REASON_SUBTITLE && kind == V) {
				/* The picture is re-encoded to burn the subtitle in: the
				 * subtitle explains it. */
				g_assert_null(base);
			} else if (!applies) {
				/* A reason about another stream says nothing about this one. */
				g_assert_nonnull(base);
				g_assert_cmpint(base->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
				g_assert_null(base->stated_reason);
			} else if (x->property != AXIS_PROPERTY_NONE) {
				/* Blamed on a property: that row says it, and the codec's own
				 * row is not made to say it too. */
				g_assert_nonnull(blamed);
				g_assert_cmpint(blamed->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
				g_assert_cmpstr(blamed->stated_reason, ==, x->name);
				g_assert_null(base);
			} else {
				g_assert_nonnull(base);
				g_assert_cmpint(base->effect, ==, x->effect);
				g_assert_cmpstr(base->stated_reason, ==, x->name);
			}
		}
		/* No row ever blames the codec for a reason that blames a property,
		 * and the only codec-level failures are the codec reasons. */
		for (k = 0; k < evidence->len; k++) {
			const AxisCompatEvidence *e = g_ptr_array_index(evidence, k);

			if (e->effect == AXIS_EFFECT_CONVERTED_CODEC && e->property == AXIS_PROPERTY_NONE)
				g_assert_true(x->effect == AXIS_EFFECT_CONVERTED_CODEC && x->property == AXIS_PROPERTY_NONE);
		}
		g_ptr_array_unref(evidence);
	}
}

/* Spec: Unknown reason. */
static void
test_a_reason_the_table_does_not_know_is_cause_unclear(void)
{
	const gchar *only[] = { "SomethingTheNextVersionInvented", NULL };
	const gchar *with_codec[] = { "SomethingTheNextVersionInvented", "VideoCodecNotSupported", NULL };
	GPtrArray *evidence = evidence_of_reasons(only, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_COPY,
		AXIS_COMPAT_DECISION_NONE);
	const AxisCompatEvidence *e = row_of(evidence, V, AXIS_PROPERTY_NONE);

	g_assert_nonnull(e);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
	g_assert_cmpstr(e->stated_reason, ==, "SomethingTheNextVersionInvented");
	g_assert_cmpint(row_of(evidence, A, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
	g_ptr_array_unref(evidence);

	/* What it states beside a known reason does not weaken the known one. */
	evidence = evidence_of_reasons(with_codec, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_COPY,
		AXIS_COMPAT_DECISION_NONE);
	e = row_of(evidence, V, AXIS_PROPERTY_NONE);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(e->stated_reason, ==, "SomethingTheNextVersionInvented,VideoCodecNotSupported");
	g_ptr_array_unref(evidence);
}

/* Spec: Stated codec reason / bandwidth: which wins when several are stated. */
static void
test_stated_reasons_combine_codec_over_limit_over_unclear(void)
{
	const gchar *codec_and_limit[] = { "ContainerBitrateExceedsLimit", "VideoCodecNotSupported", NULL };
	const gchar *limit_and_unclear[] = { "VideoFramerateNotSupported", "ContainerBitrateExceedsLimit", NULL };
	const gchar *bit_depth_and_unclear[] = { "VideoBitDepthNotSupported", "VideoFramerateNotSupported", NULL };
	const gchar *bit_depth_and_codec[] = { "VideoBitDepthNotSupported", "VideoCodecNotSupported", NULL };
	GPtrArray *evidence;

	evidence = evidence_of_reasons(codec_and_limit, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_TRANSCODE,
		AXIS_COMPAT_DECISION_NONE);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	/* The codec reason is about the video only: the audio was limited. */
	g_assert_cmpint(row_of(evidence, A, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_LIMIT);
	g_ptr_array_unref(evidence);

	evidence = evidence_of_reasons(limit_and_unclear, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_COPY,
		AXIS_COMPAT_DECISION_NONE);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_LIMIT);
	g_ptr_array_unref(evidence);

	/* A property blamed beside an unclear cause: both rows, the codec never blamed. */
	evidence = evidence_of_reasons(bit_depth_and_unclear, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_COPY,
		AXIS_COMPAT_DECISION_NONE);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_BIT_DEPTH)->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_ptr_array_unref(evidence);

	evidence = evidence_of_reasons(bit_depth_and_codec, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_COPY,
		AXIS_COMPAT_DECISION_NONE);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_BIT_DEPTH)->bit_depth, ==, 8);
	g_ptr_array_unref(evidence);
}

/* Inference is used only when no reason is stated. */
static void
test_inference_only_when_nothing_is_stated(void)
{
	AxisCompatSession *s = made_session(NULL, AXIS_COMPAT_DECISION_TRANSCODE, AXIS_COMPAT_DECISION_COPY,
		AXIS_COMPAT_DECISION_NONE);
	GPtrArray *evidence;

	/* The output codec differs from the source codec: Plex's inference. */
	g_free(s->transcode.video_codec);
	s->transcode.video_codec = g_strdup("vp9");
	evidence = axis_compat_classify_session(s, NULL);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_null(row_of(evidence, V, AXIS_PROPERTY_NONE)->stated_reason);
	g_ptr_array_unref(evidence);

	/* With a reason stated, the inference is not used: an unclear reason
	 * keeps the codec out of it although the output codec differs. */
	s->transcode.reasons = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(s->transcode.reasons, g_strdup("VideoFramerateNotSupported"));
	evidence = axis_compat_classify_session(s, NULL);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
	g_ptr_array_unref(evidence);
	axis_compat_session_free(s);

	/* A conversion with no TranscodingInfo at all is not explained: nothing
	 * is concluded about any stream. */
	{
		GPtrArray *sessions = sessions_of(CONSTRUCTED, "session-no-transcoding-info.json");

		evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
		g_assert_cmpuint(evidence->len, ==, 2);
		g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_UNKNOWN);
		g_assert_cmpint(row_of(evidence, A, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_UNKNOWN);
		g_ptr_array_unref(evidence);
		g_ptr_array_unref(sessions);
	}
}

/* The recorded sessions, through the whole chain from the server's JSON to
 * evidence. */
static void
test_recorded_sessions_give_the_evidence_a_person_would_expect(void)
{
	GPtrArray *sessions = sessions_of(RECORDED, "sessions-playing.json");
	GPtrArray *evidence;
	const AxisCompatEvidence *e;

	/* Direct play: both streams, with the container. */
	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-tv"), NULL);
	g_assert_cmpuint(evidence->len, ==, 2);
	e = row_of(evidence, V, AXIS_PROPERTY_NONE);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_DIRECT);
	g_assert_cmpstr(e->codec, ==, "h264");
	g_assert_cmpstr(e->profile, ==, "constrained baseline");
	g_assert_cmpint(e->bit_depth, ==, 8);
	g_assert_cmpstr(e->container, ==, "mkv");
	g_assert_cmpint(e->hdr, ==, 0);
	e = row_of(evidence, A, AXIS_PROPERTY_NONE);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_DIRECT);
	g_assert_cmpstr(e->codec, ==, "aac");
	g_ptr_array_unref(evidence);

	/* The phone: the codec and the container named, the audio passed. */
	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-phone"), NULL);
	g_assert_cmpuint(evidence->len, ==, 3);
	e = row_of(evidence, V, AXIS_PROPERTY_NONE);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(e->codec, ==, "hevc");
	g_assert_cmpstr(e->out_codec, ==, "h264");
	g_assert_cmpstr(e->stated_reason, ==, "VideoCodecNotSupported");
	e = row_of(evidence, V, AXIS_PROPERTY_CONTAINER);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(e->container, ==, "mkv");
	g_assert_cmpstr(e->stated_reason, ==, "ContainerNotSupported");
	g_assert_cmpint(row_of(evidence, A, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
	g_ptr_array_unref(evidence);

	/* The stick: the codec is shown to be taken; only the container fails. */
	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-stick"), NULL);
	g_assert_cmpuint(evidence->len, ==, 3);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_CONTAINER)->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpint(row_of(evidence, A, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
	g_ptr_array_unref(evidence);

	/* The tablet: the bit depth is blamed, the codec is not. */
	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-tablet"), NULL);
	g_assert_cmpuint(evidence->len, ==, 2);
	g_assert_null(row_of(evidence, V, AXIS_PROPERTY_NONE));
	e = row_of(evidence, V, AXIS_PROPERTY_BIT_DEPTH);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpint(e->bit_depth, ==, 10);
	g_assert_cmpstr(e->codec, ==, "hevc");
	g_assert_cmpstr(e->stated_reason, ==, "VideoBitDepthNotSupported");
	g_ptr_array_unref(evidence);

	/* The hotel: a bitrate limit is held against no format. */
	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-hotel"), NULL);
	g_assert_cmpuint(evidence->len, ==, 2);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_LIMIT);
	g_assert_cmpint(row_of(evidence, A, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_CONVERTED_LIMIT);
	g_assert_cmpstr(row_of(evidence, V, AXIS_PROPERTY_NONE)->stated_reason, ==, "ContainerBitrateExceedsLimit");
	g_ptr_array_unref(evidence);

	/* The console did not say which audio track plays: no audio evidence. */
	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-console"), NULL);
	g_assert_cmpuint(evidence->len, ==, 2);
	g_assert_cmpint(row_of(evidence, V, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
	g_assert_null(row_of(evidence, A, AXIS_PROPERTY_NONE));
	/* The one subtitle track was not said to be in use: nothing is known of it. */
	g_assert_cmpint(row_of(evidence, S, AXIS_PROPERTY_NONE)->effect, ==, AXIS_EFFECT_UNKNOWN);
	g_ptr_array_unref(evidence);

	evidence = axis_compat_classify_session(session_of_device(sessions, "axis-test-speaker"), NULL);
	g_assert_cmpuint(evidence->len, ==, 1);
	e = row_of(evidence, A, AXIS_PROPERTY_NONE);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_DIRECT);
	g_assert_cmpstr(e->codec, ==, "flac");
	g_assert_cmpstr(e->container, ==, "flac");
	g_ptr_array_unref(evidence);
	g_ptr_array_unref(sessions);
}

static void
test_constructed_sessions_give_their_evidence(void)
{
	GPtrArray *sessions;
	GPtrArray *evidence;
	const AxisCompatEvidence *e;

	/* A client that reports its track: that track's codec is the one blamed. */
	sessions = sessions_of(CONSTRUCTED, "session-audio-selected.json");
	evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	e = row_of(evidence, A, AXIS_PROPERTY_NONE);
	g_assert_nonnull(e);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(e->codec, ==, "ac3");
	g_assert_cmpstr(e->stated_reason, ==, "AudioCodecNotSupported");
	g_ptr_array_unref(evidence);
	g_ptr_array_unref(sessions);

	/* A subtitle that had to be burned in: the format is blamed, not a codec. */
	sessions = sessions_of(CONSTRUCTED, "session-subtitle-burned.json");
	evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	e = row_of(evidence, S, AXIS_PROPERTY_SUBTITLE_FORMAT);
	g_assert_nonnull(e);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(e->codec, ==, "subrip");
	g_assert_null(row_of(evidence, V, AXIS_PROPERTY_NONE));
	g_assert_null(row_of(evidence, V, AXIS_PROPERTY_SUBTITLE_FORMAT));
	g_ptr_array_unref(evidence);
	g_ptr_array_unref(sessions);

	/* A level: recorded against the level, which no target names. */
	sessions = sessions_of(CONSTRUCTED, "session-level.json");
	evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	e = row_of(evidence, V, AXIS_PROPERTY_LEVEL);
	g_assert_nonnull(e);
	g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_null(row_of(evidence, V, AXIS_PROPERTY_NONE));
	g_ptr_array_unref(evidence);
	g_ptr_array_unref(sessions);

	/* The stated words are cleaned and cut, and the unknown ones are unclear. */
	sessions = sessions_of(CONSTRUCTED, "session-unknown-reasons.json");
	{
		const AxisCompatSession *s = g_ptr_array_index(sessions, 0);
		guint i;

		g_assert_cmpuint(s->transcode.reasons->len, ==, 4);
		for (i = 0; i < s->transcode.reasons->len; i++) {
			assert_clean(g_ptr_array_index(s->transcode.reasons, i));
			g_assert_cmpuint(g_utf8_strlen(g_ptr_array_index(s->transcode.reasons, i), -1), <=,
				AXIS_COMPAT_MAX_REASON_CHARS);
		}
		evidence = axis_compat_classify_session(s, NULL);
		e = row_of(evidence, V, AXIS_PROPERTY_NONE);
		g_assert_cmpint(e->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
		assert_clean(e->stated_reason);
		g_assert_cmpuint(g_utf8_strlen(e->stated_reason, -1), <=, AXIS_COMPAT_MAX_REASON_CHARS);
		for (i = 0; i < evidence->len; i++)
			g_assert_false(((AxisCompatEvidence *)g_ptr_array_index(evidence, i))->effect == AXIS_EFFECT_CONVERTED_CODEC);
		g_ptr_array_unref(evidence);
	}
	g_ptr_array_unref(sessions);

	/* The streams of another version say nothing about what played. */
	sessions = sessions_of(CONSTRUCTED, "session-other-version.json");
	evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	g_assert_cmpuint(evidence->len, ==, 0);
	g_ptr_array_unref(evidence);
	g_ptr_array_unref(sessions);
}

/* ── Verdicts that honour the property ──────────────────────────── */

static AxisCompatCombo
combo(
	gint         kind,
	const gchar *codec,
	const gchar *profile,
	gint         depth,
	const gchar *res,
	gint         hdr,
	const gchar *container
){
	AxisCompatCombo c;

	memset(&c, 0, sizeof(c));
	c.kind = kind;
	c.codec = codec;
	c.profile = profile;
	c.bit_depth = depth;
	c.res_class = res;
	c.hdr = hdr;
	c.container = container;
	return c;
}

static AxisCompatVerdict
verdict_for(
	const AxisCompatCombo *want,
	const AxisCompatObservation *obs,
	guint n
){
	return axis_compat_device_verdict(want, obs, n, NULL, NULL);
}

static AxisCompatObservation
observed(
	AxisCompatEffect    effect,
	AxisCompatProperty  property,
	AxisCompatCombo     c
){
	AxisCompatObservation o;

	memset(&o, 0, sizeof(o));
	o.combo = c;
	o.effect = effect;
	o.property = property;
	o.plays = 1;
	return o;
}

/* Spec: Container-only failure. */
static void
test_a_container_failure_does_not_count_against_the_codec_elsewhere(void)
{
	AxisCompatObservation failed = observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_CONTAINER,
		combo(V, "h264", "high", 8, "1080p", 0, "mkv"));
	AxisCompatObservation passed = observed(AXIS_EFFECT_PASSTHROUGH, AXIS_PROPERTY_NONE,
		combo(V, "h264", "high", 8, "1080p", 0, NULL));
	AxisCompatObservation both[2];
	AxisCompatCombo in_mkv = combo(V, "h264", "high", 8, "1080p", 0, "mkv");
	AxisCompatCombo in_mp4 = combo(V, "h264", "high", 8, "1080p", 0, "mp4");
	AxisCompatCombo anywhere = combo(V, "h264", "high", 8, "1080p", 0, NULL);
	AxisCompatWhy why;

	/* In the container that was not taken: risky. */
	g_assert_cmpint(verdict_for(&in_mkv, &failed, 1), ==, AXIS_VERDICT_RISKY);
	/* In another: the failure says nothing; nothing else is known. */
	g_assert_cmpint(axis_compat_device_verdict(&in_mp4, &failed, 1, &why, NULL), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_PROPERTY_ONLY);
	/* With no container named, a container failure fails nothing. */
	g_assert_cmpint(verdict_for(&anywhere, &failed, 1), ==, AXIS_VERDICT_UNKNOWN);
	/* The same failure without the property is the codec failing, and counts
	 * against it in every container. */
	failed.property = AXIS_PROPERTY_NONE;
	g_assert_cmpint(verdict_for(&in_mp4, &failed, 1), ==, AXIS_VERDICT_RISKY);
	failed.property = AXIS_PROPERTY_CONTAINER;
	/* The codec passed through is shown to be taken, but not in a container. */
	both[0] = failed;
	both[1] = passed;
	g_assert_cmpint(verdict_for(&in_mkv, both, 2), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(verdict_for(&in_mp4, both, 2), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(verdict_for(&anywhere, both, 2), ==, AXIS_VERDICT_SAFE);
}

static void
test_a_bit_depth_or_profile_failure_counts_only_at_and_above_it(void)
{
	AxisCompatObservation depth10 = observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_BIT_DEPTH,
		combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv"));
	AxisCompatObservation main_profile = observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_PROFILE,
		combo(V, "hevc", "main", 8, "1080p", 0, "mkv"));
	AxisCompatObservation level = observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_LEVEL,
		combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv"));
	AxisCompatObservation unvalued = observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_BIT_DEPTH,
		combo(V, "hevc", NULL, 0, NULL, -1, NULL));
	AxisCompatCombo ten = combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv");
	AxisCompatCombo eight = combo(V, "hevc", "main", 8, "1080p", 0, "mkv");
	AxisCompatCombo twelve = combo(V, "hevc", "main 12", 12, "1080p", 0, "mkv");

	g_assert_cmpint(verdict_for(&ten, &depth10, 1), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(verdict_for(&twelve, &depth10, 1), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(verdict_for(&eight, &depth10, 1), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(verdict_for(&eight, &main_profile, 1), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(verdict_for(&ten, &main_profile, 1), ==, AXIS_VERDICT_UNKNOWN);
	/* No target names a level: it can fail none. */
	g_assert_cmpint(verdict_for(&ten, &level, 1), ==, AXIS_VERDICT_UNKNOWN);
	/* A failure that does not say what it is about fails nothing. */
	g_assert_cmpint(verdict_for(&ten, &unvalued, 1), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(verdict_for(&ten, NULL, 0), ==, AXIS_VERDICT_UNKNOWN);
}

static void
test_a_subtitle_format_failure_fails_that_format(void)
{
	AxisCompatObservation failed = observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_SUBTITLE_FORMAT,
		combo(S, "subrip", NULL, 0, NULL, -1, NULL));
	AxisCompatCombo same = combo(S, "subrip", NULL, 0, NULL, -1, NULL);
	AxisCompatCombo other = combo(S, "ass", NULL, 0, NULL, -1, NULL);

	g_assert_cmpint(verdict_for(&same, &failed, 1), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(verdict_for(&other, &failed, 1), ==, AXIS_VERDICT_UNKNOWN);
}

/* The reference for axis_compat_fails_property(), written differently: from
 * the point of view of "what would the device have to take". */
static gboolean
reference_fails(
	AxisCompatProperty     property,
	const AxisCompatCombo *failed,
	const AxisCompatCombo *want
){
	if (failed->kind != want->kind || g_strcmp0(failed->codec, want->codec) != 0)
		return FALSE;
	switch (property) {
	case AXIS_PROPERTY_CONTAINER:
		return failed->container != NULL && want->container != NULL && g_strcmp0(failed->container, want->container) == 0;
	case AXIS_PROPERTY_PROFILE:
		return failed->profile != NULL && want->profile != NULL && g_strcmp0(failed->profile, want->profile) == 0;
	case AXIS_PROPERTY_BIT_DEPTH:
		return failed->bit_depth > 0 && want->bit_depth > 0 && want->bit_depth >= failed->bit_depth;
	case AXIS_PROPERTY_SUBTITLE_FORMAT:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Every pair over a small universe: the function agrees with the reference,
 * and a failure blamed on a property never fails a combination that differs
 * in that property. */
static void
test_fails_property_exhaustive(void)
{
	static const gchar *const codecs[] = { "h264", "hevc" };
	static const gchar *const profiles[] = { NULL, "main", "main 10" };
	static const gint depths[] = { 0, 8, 10 };
	static const gchar *const resolutions[] = { NULL, "1080p", "2160p" };
	static const gint hdrs[] = { -1, 0, 1 };
	static const gchar *const containers[] = { NULL, "mkv", "mp4" };
	static const AxisCompatProperty properties[] = {
		AXIS_PROPERTY_CONTAINER, AXIS_PROPERTY_PROFILE, AXIS_PROPERTY_BIT_DEPTH, AXIS_PROPERTY_LEVEL,
		AXIS_PROPERTY_SUBTITLE_FORMAT
	};
	gulong checked = 0;
	guint a;

	for (a = 0; a < 2 * 3 * 3 * 3 * 3 * 3; a++) {
		guint x = a;
		AxisCompatCombo failed = combo(V, codecs[x % 2], profiles[(x / 2) % 3], depths[(x / 6) % 3],
			resolutions[(x / 18) % 3], hdrs[(x / 54) % 3], containers[(x / 162) % 3]);
		guint b;

		for (b = 0; b < 2 * 3 * 3 * 3 * 3 * 3; b++) {
			guint y = b;
			AxisCompatCombo want = combo(V, codecs[y % 2], profiles[(y / 2) % 3], depths[(y / 6) % 3],
				resolutions[(y / 18) % 3], hdrs[(y / 54) % 3], containers[(y / 162) % 3]);
			guint p;

			for (p = 0; p < G_N_ELEMENTS(properties); p++) {
				gboolean got = axis_compat_fails_property(&failed, properties[p], &want);

				g_assert_cmpint(got, ==, reference_fails(properties[p], &failed, &want));
				if (properties[p] == AXIS_PROPERTY_CONTAINER && g_strcmp0(failed.container, want.container) != 0)
					g_assert_false(got);
				if (properties[p] == AXIS_PROPERTY_PROFILE && g_strcmp0(failed.profile, want.profile) != 0)
					g_assert_false(got);
				if (properties[p] == AXIS_PROPERTY_BIT_DEPTH && want.bit_depth < failed.bit_depth)
					g_assert_false(got);
				if (properties[p] == AXIS_PROPERTY_LEVEL)
					g_assert_false(got);
				checked++;
			}
			/* No property is the codec-level rule, as it always was. */
			g_assert_cmpint(axis_compat_fails_property(&failed, AXIS_PROPERTY_NONE, &want), ==,
				axis_compat_fails(&failed, &want));
		}
	}
	g_assert_cmpuint(checked, >, 100000);
}

/* The exhaustive verdict test of the Plex rules, extended: over every subset
 * of a universe that now has conversions blamed on a property, safe still
 * needs a direct play that covers the combination, and risky needs a failure
 * that is at or above it in the property it blames. */
static void
test_device_verdict_exhaustive_with_properties(void)
{
	AxisCompatCombo want = combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv");
	AxisCompatObservation universe[] = {
		observed(AXIS_EFFECT_DIRECT, AXIS_PROPERTY_NONE, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_CONTAINER, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_CONTAINER, combo(V, "hevc", "main 10", 10, "1080p", 0, "mp4")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_BIT_DEPTH, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_BIT_DEPTH, combo(V, "hevc", "main 12", 12, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_PROFILE, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_PROFILE, combo(V, "hevc", "main", 8, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_LEVEL, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_CODEC, AXIS_PROPERTY_NONE, combo(V, "hevc", "main 10", 10, "2160p", 0, "mkv")),
		observed(AXIS_EFFECT_CONVERTED_LIMIT, AXIS_PROPERTY_NONE, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv"))
	};
	guint n = G_N_ELEMENTS(universe);
	guint mask;

	for (mask = 0; mask < (1u << n); mask++) {
		AxisCompatObservation picked[16];
		guint count = 0;
		guint i;
		/* Safe: the direct play (0). Risky: a failure that names the
		 * container the combination is in (1), a bit depth at or below it
		 * (3), or its profile (5). The failures of another container (2), a
		 * higher bit depth (4), another profile (6), a level (7), a harder
		 * combination (8) and a limit (9) fail nothing. */
		gboolean risky = (mask & ((1u << 1) | (1u << 3) | (1u << 5))) != 0;
		gboolean safe = (mask & 1u) != 0;
		AxisCompatVerdict wanted = risky ? AXIS_VERDICT_RISKY : (safe ? AXIS_VERDICT_SAFE : AXIS_VERDICT_UNKNOWN);

		for (i = 0; i < n; i++) {
			if ((mask & (1u << i)) != 0)
				picked[count++] = universe[i];
		}
		g_assert_cmpint(verdict_for(&want, picked, count), ==, wanted);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(name, func) g_test_add_func("/compat-jellyfin/" name, func)
	ADD("parse/identity", test_parse_identity);
	ADD("parse/libraries", test_parse_libraries);
	ADD("parse/devices", test_parse_devices);
	ADD("parse/sessions-recorded", test_parse_sessions_recorded);
	ADD("parse/sessions-constructed", test_parse_sessions_constructed);
	ADD("parse/sessions-odd-input", test_parse_sessions_odd_input);
	ADD("parse/item", test_parse_item);
	ADD("parse/keys", test_keys);
	ADD("parse/hostile", test_parse_hostile_input);
	ADD("reasons/each-reason", test_each_reason_maps_as_the_table_says);
	ADD("reasons/the-servers-list", test_the_table_is_the_servers_list);
	ADD("reasons/unknown", test_a_reason_the_table_does_not_know_is_not_in_it);
	ADD("evidence/every-reason", test_every_reason_gives_the_evidence_the_table_says);
	ADD("evidence/unknown-reason", test_a_reason_the_table_does_not_know_is_cause_unclear);
	ADD("evidence/combining", test_stated_reasons_combine_codec_over_limit_over_unclear);
	ADD("evidence/inference-only-when-nothing-stated", test_inference_only_when_nothing_is_stated);
	ADD("evidence/recorded", test_recorded_sessions_give_the_evidence_a_person_would_expect);
	ADD("evidence/constructed", test_constructed_sessions_give_their_evidence);
	ADD("verdict/container", test_a_container_failure_does_not_count_against_the_codec_elsewhere);
	ADD("verdict/depth-profile-level", test_a_bit_depth_or_profile_failure_counts_only_at_and_above_it);
	ADD("verdict/subtitle", test_a_subtitle_format_failure_fails_that_format);
	ADD("verdict/fails-property-exhaustive", test_fails_property_exhaustive);
	ADD("verdict/exhaustive-with-properties", test_device_verdict_exhaustive_with_properties);
#undef ADD
	return g_test_run();
}
