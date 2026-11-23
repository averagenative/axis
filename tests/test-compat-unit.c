/*
 * test-compat-unit.c - the pure parts of client compatibility: the Plex
 * parsers, the classification of a session into evidence, the "harder than"
 * rules, the verdicts and the recommendation, the address and the path map
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No database, no network.  Spec scenarios covered here:
 *   Evidence is per stream; Unexplained conversion; Unknown decision value;
 *   Observed direct play; Not observed; Bitrate-limited conversion; Passed
 *   through is not direct play; Safe; Risky; Unknown is never safe; No
 *   evidence; Largest safe saving; Nothing safe; Mapped through the path
 *   map; Unmapped library.
 * Fixtures marked "constructed" are not recorded from a real server
 * (tests/fixtures/compat/constructed/README.md).
 */

#include <glib.h>
#include <string.h>

#include "compat/axis-compat-evidence.h"
#include "compat/axis-compat-path.h"
#include "compat/axis-compat-plex.h"
#include "compat/axis-compat-verdict.h"

#define FIXTURES "tests/fixtures/compat/"

static gchar *
read_fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

static GPtrArray *
sessions_from_fixture(const gchar *name)
{
	g_autofree gchar *text = read_fixture(name);
	g_autofree gchar *problem = NULL;
	GPtrArray *sessions = axis_compat_plex_parse_sessions(text, strlen(text), &problem);

	g_assert_nonnull(sessions);
	return sessions;
}

static const AxisCompatEvidence *
evidence_of(
	GPtrArray *evidence,
	gint       kind
){
	guint i;

	for (i = 0; i < evidence->len; i++) {
		const AxisCompatEvidence *e = g_ptr_array_index(evidence, i);

		if (e->kind == kind)
			return e;
	}
	return NULL;
}

/* ── Parsers ────────────────────────────────────────────────────── */

static void
test_parse_identity_and_root(void)
{
	g_autofree gchar *text = read_fixture("identity.json");
	g_autofree gchar *root = read_fixture("constructed/root.json");
	g_autofree gchar *problem = NULL;
	AxisCompatIdentity identity;

	memset(&identity, 0, sizeof(identity));
	g_assert_true(axis_compat_plex_parse_identity(text, strlen(text), &identity, &problem));
	g_assert_cmpstr(identity.machine_id, ==, "aaaabbbbccccddddeeeeffff0000111122223333");
	g_assert_cmpstr(identity.version, ==, "1.43.5.0-invented");
	g_assert_null(identity.name);
	g_assert_true(axis_compat_plex_parse_root(root, strlen(root), &identity, &problem));
	g_assert_cmpstr(identity.name, ==, "Invented Server");
	axis_compat_identity_clear(&identity);
}

static void
test_parse_libraries(void)
{
	g_autofree gchar *text = read_fixture("sections.json");
	g_autofree gchar *problem = NULL;
	g_autoptr(GPtrArray) libraries = axis_compat_plex_parse_libraries(text, strlen(text), &problem);
	const AxisCompatLibrary *l;

	g_assert_nonnull(libraries);
	g_assert_cmpuint(libraries->len, ==, 3);
	l = g_ptr_array_index(libraries, 0);
	g_assert_cmpstr(l->key, ==, "1");
	g_assert_cmpstr(l->kind, ==, "movie");
	g_assert_cmpstr(l->title, ==, "Films");
	g_assert_cmpuint(l->folders->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(l->folders, 0), ==, "/data/movies");
	l = g_ptr_array_index(libraries, 1);
	g_assert_cmpstr(l->kind, ==, "show");
	g_assert_cmpuint(l->folders->len, ==, 2);
	l = g_ptr_array_index(libraries, 2);
	g_assert_cmpstr(l->kind, ==, "artist");
}

static void
test_parse_devices_accounts_history_item(void)
{
	g_autofree gchar *devices_text = read_fixture("devices.json");
	g_autofree gchar *accounts_text = read_fixture("accounts.json");
	g_autofree gchar *history_raw = read_fixture("history-1.json");
	GString *history_fixed = g_string_new(history_raw);
	g_autofree gchar *history_text = NULL;
	g_autofree gchar *item_text = read_fixture("item-film.json");
	g_autofree gchar *problem = NULL;
	g_autoptr(GPtrArray) devices = axis_compat_plex_parse_devices(devices_text, strlen(devices_text), &problem);
	g_autoptr(GPtrArray) accounts = axis_compat_plex_parse_accounts(accounts_text, strlen(accounts_text), &problem);
	AxisCompatHistory history;
	AxisCompatItem item;
	const AxisCompatDevice *d;
	const AxisCompatPlay *p;

	/* The fake server fills in the ages; here a time stands in. */
	g_string_replace(history_fixed, "@AGE:86400@", "1700000000", 0);
	g_string_replace(history_fixed, "@AGE:172800@", "1700000001", 0);
	g_string_replace(history_fixed, "@AGE:259200@", "1700000002", 0);
	history_text = g_string_free(history_fixed, FALSE);
	g_assert_cmpuint(devices->len, ==, 4);
	d = g_ptr_array_index(devices, 1);
	g_assert_cmpint(d->plex_id, ==, 12);
	g_assert_cmpstr(d->client_id, ==, "client-android-1");
	g_assert_cmpstr(d->name, ==, "Pixel");
	g_assert_cmpstr(d->platform, ==, "Android");
	g_assert_cmpuint(accounts->len, ==, 2);
	g_assert_cmpstr(((AxisCompatAccount *)g_ptr_array_index(accounts, 0))->id, ==, "1");

	memset(&history, 0, sizeof(history));
	g_assert_true(axis_compat_plex_parse_history(history_text, strlen(history_text), &history, &problem));
	g_assert_cmpint(history.total, ==, 5);
	g_assert_cmpuint(history.plays->len, ==, 3);
	p = g_ptr_array_index(history.plays, 0);
	g_assert_cmpint(p->device_id, ==, 11);
	g_assert_cmpstr(p->library_key, ==, "1");
	g_assert_cmpstr(p->rating_key, ==, "501");
	g_assert_cmpstr(p->account_id, ==, "1");
	g_assert_cmpint(p->viewed_at, >, 0);
	axis_compat_history_clear(&history);

	memset(&item, 0, sizeof(item));
	g_assert_true(axis_compat_plex_parse_item(item_text, strlen(item_text), &item, &problem));
	g_assert_cmpstr(item.file, ==, "/data/movies/Invented Film (2001)/Invented Film (2001).mkv");
	axis_compat_item_clear(&item);
}

static void
test_parse_the_observed_session(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("session-converted-audio.json");
	const AxisCompatSession *s;
	const AxisCompatStream *video;
	const AxisCompatStream *audio;

	g_assert_cmpuint(sessions->len, ==, 1);
	s = g_ptr_array_index(sessions, 0);
	g_assert_cmpstr(s->session_key, ==, "sess-converted-audio-1");
	g_assert_cmpstr(s->rating_key, ==, "501");
	g_assert_cmpstr(s->library_key, ==, "1");
	g_assert_cmpstr(s->device_client_id, ==, "client-android-1");
	g_assert_cmpstr(s->device_name, ==, "Pixel");
	g_assert_cmpstr(s->location, ==, "wan");
	g_assert_cmpint(s->bandwidth_kbps, ==, 20000);
	g_assert_cmpint(s->player_local, ==, AXIS_COMPAT_NO);
	g_assert_true(s->transcode.present);
	g_assert_cmpint(s->transcode.video_decision, ==, AXIS_COMPAT_DECISION_COPY);
	g_assert_cmpint(s->transcode.audio_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_assert_cmpint(s->transcode.subtitle_decision, ==, AXIS_COMPAT_DECISION_NONE);
	g_assert_cmpstr(s->transcode.source_audio_codec, ==, "eac3");
	g_assert_cmpstr(s->transcode.audio_codec, ==, "mp3");
	g_assert_cmpint(s->part_decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_assert_cmpstr(s->account_id, ==, "2");
	g_assert_cmpstr(s->file, ==, "/data/movies/Invented Film (2001)/Invented Film (2001).mkv");
	g_assert_cmpuint(s->streams->len, ==, 2);
	video = g_ptr_array_index(s->streams, 0);
	audio = g_ptr_array_index(s->streams, 1);
	g_assert_cmpint(video->type, ==, AXIS_COMPAT_STREAM_VIDEO);
	g_assert_cmpint(video->decision, ==, AXIS_COMPAT_DECISION_COPY);
	g_assert_cmpint(video->bit_depth, ==, 8);
	g_assert_cmpint(video->hdr, ==, AXIS_COMPAT_NO);
	g_assert_cmpint(audio->decision, ==, AXIS_COMPAT_DECISION_TRANSCODE);
	g_assert_cmpint(audio->channels, ==, 6);
}

static void
test_parse_empty_sessions(void)
{
	g_autofree gchar *text = read_fixture("sessions-empty.json");
	g_autofree gchar *problem = NULL;
	g_autoptr(GPtrArray) sessions = axis_compat_plex_parse_sessions(text, strlen(text), &problem);

	g_assert_nonnull(sessions);
	g_assert_cmpuint(sessions->len, ==, 0);
}

/* Spec: Unknown decision value */
static void
test_unknown_decision_word_is_unknown(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-unknown-decision.json");
	const AxisCompatSession *s = g_ptr_array_index(sessions, 0);
	const AxisCompatStream *video = g_ptr_array_index(s->streams, 0);

	g_assert_cmpint(video->decision, ==, AXIS_COMPAT_DECISION_UNKNOWN);
	g_assert_cmpint(s->transcode.video_decision, ==, AXIS_COMPAT_DECISION_UNKNOWN);
	g_assert_cmpint(axis_compat_decision_from_string("directplay"), ==, AXIS_COMPAT_DECISION_DIRECT);
	g_assert_cmpint(axis_compat_decision_from_string("frobnicate"), ==, AXIS_COMPAT_DECISION_UNKNOWN);
	g_assert_cmpint(axis_compat_decision_from_string(""), ==, AXIS_COMPAT_DECISION_NONE);
	g_assert_cmpint(axis_compat_decision_from_string(NULL), ==, AXIS_COMPAT_DECISION_NONE);
}

static void
test_parser_rejects_what_is_not_a_media_container(void)
{
	const gchar *bad[] = { "", "not json", "[1,2,3]", "{}", "{\"MediaContainer\":3}", "{\"MediaContainer\":",
		"null", NULL };
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autofree gchar *problem = NULL;
		g_autoptr(GPtrArray) sessions = axis_compat_plex_parse_sessions(bad[i], strlen(bad[i]), &problem);
		g_autoptr(GPtrArray) libraries = axis_compat_plex_parse_libraries(bad[i], strlen(bad[i]), NULL);
		AxisCompatIdentity identity;

		memset(&identity, 0, sizeof(identity));
		g_assert_null(sessions);
		g_assert_nonnull(problem);
		g_assert_null(libraries);
		g_assert_false(axis_compat_plex_parse_identity(bad[i], strlen(bad[i]), &identity, NULL));
	}
}

static void
test_parser_treats_out_of_range_and_odd_fields_as_not_stated(void)
{
	const gchar *json =
		"{\"MediaContainer\":{\"Metadata\":[{\"ratingKey\":\"5\",\"librarySectionID\":\"1\","
		"\"Player\":{\"machineIdentifier\":\"dev-1\",\"local\":\"maybe\"},"
		"\"Session\":{\"id\":\"s1\",\"bandwidth\":-5,\"location\":12},"
		"\"Media\":[{\"bitrate\":99999999999999,\"Part\":[{\"decision\":7,\"Stream\":["
		"{\"streamType\":1,\"width\":-1,\"height\":9999999,\"bitDepth\":\"x\",\"codec\":\"H264\",\"profile\":\"HIGH\"},"
		"{\"streamType\":99,\"codec\":{}}]}]}]}]}}";
	g_autoptr(GPtrArray) sessions = axis_compat_plex_parse_sessions(json, strlen(json), NULL);
	const AxisCompatSession *s;
	const AxisCompatStream *v;

	g_assert_nonnull(sessions);
	s = g_ptr_array_index(sessions, 0);
	g_assert_cmpint(s->player_local, ==, AXIS_COMPAT_UNSTATED);
	g_assert_cmpint(s->bandwidth_kbps, ==, -1);
	g_assert_cmpstr(s->location, ==, "12");
	g_assert_cmpint(s->media_bitrate_kbps, ==, -1);
	g_assert_cmpint(s->part_decision, ==, AXIS_COMPAT_DECISION_UNKNOWN);
	v = g_ptr_array_index(s->streams, 0);
	g_assert_cmpint(v->width, ==, -1);
	g_assert_cmpint(v->height, ==, -1);
	g_assert_cmpint(v->bit_depth, ==, -1);
	g_assert_cmpstr(v->codec, ==, "h264");
	g_assert_cmpstr(v->profile, ==, "high");
	v = g_ptr_array_index(s->streams, 1);
	g_assert_cmpint(v->type, ==, 0);
	g_assert_null(v->codec);
}

static void
test_parser_drops_long_ids_and_paths_instead_of_cutting_them(void)
{
	g_autofree gchar *long_id = g_strnfill(200, 'a');
	g_autofree gchar *long_path = g_strconcat("/", long_id, long_id, long_id, long_id, long_id, long_id, NULL);
	g_autofree gchar *json = g_strdup_printf(
		"{\"MediaContainer\":{\"Metadata\":[{\"ratingKey\":\"%s\",\"librarySectionID\":\"bad id!\","
		"\"Player\":{\"machineIdentifier\":\"d\"},\"Session\":{\"id\":\"s\"},"
		"\"Media\":[{\"Part\":[{\"file\":\"%s\"}]}]}]}}", long_id, long_path);
	g_autoptr(GPtrArray) sessions = axis_compat_plex_parse_sessions(json, strlen(json), NULL);
	const AxisCompatSession *s = g_ptr_array_index(sessions, 0);

	g_assert_null(s->rating_key);
	g_assert_null(s->library_key);
	g_assert_null(s->file);
	g_assert_true(axis_compat_plex_id_valid("client-1.a:b_c"));
	g_assert_false(axis_compat_plex_id_valid(""));
	g_assert_false(axis_compat_plex_id_valid("a/b"));
	g_assert_false(axis_compat_plex_id_valid("a b"));
	g_assert_false(axis_compat_plex_id_valid(".."));
	g_assert_false(axis_compat_plex_id_valid("a..b"));
	g_assert_true(axis_compat_plex_id_valid("."));
	g_assert_false(axis_compat_plex_id_valid(long_id));
}

static void
test_parser_caps_lists_and_cleans_text(void)
{
	GString *json = g_string_new("{\"MediaContainer\":{\"Device\":[");
	g_autoptr(GPtrArray) devices = NULL;
	guint i;
	const AxisCompatDevice *d;

	for (i = 0; i < AXIS_COMPAT_MAX_DEVICES + 50; i++)
		g_string_append_printf(json, "%s{\"id\":%u,\"clientIdentifier\":\"c%u\",\"name\":\"n\\u0007\\u202e x\"}",
			i > 0 ? "," : "", i, i);
	g_string_append(json, "]}}");
	devices = axis_compat_plex_parse_devices(json->str, json->len, NULL);
	g_assert_nonnull(devices);
	g_assert_cmpuint(devices->len, ==, AXIS_COMPAT_MAX_DEVICES);
	d = g_ptr_array_index(devices, 0);
	/* No control character and no direction override survives. */
	g_assert_null(strchr(d->name, '\a'));
	g_assert_null(strstr(d->name, "\xe2\x80\xae"));
	g_assert_true(g_utf8_validate(d->name, -1, NULL));
	g_string_free(json, TRUE);
}

/* ── Classification ─────────────────────────────────────────────── */

/* Spec: Evidence is per stream */
static void
test_observed_session_is_evidence_per_stream(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("session-converted-audio.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	const AxisCompatEvidence *video = evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO);
	const AxisCompatEvidence *audio = evidence_of(evidence, AXIS_COMPAT_STREAM_AUDIO);

	g_assert_cmpuint(evidence->len, ==, 2);
	/* The video was passed through: evidence for the codec, none for the container. */
	g_assert_nonnull(video);
	g_assert_cmpint(video->effect, ==, AXIS_EFFECT_PASSTHROUGH);
	g_assert_cmpstr(video->codec, ==, "h264");
	g_assert_cmpstr(video->profile, ==, "high");
	g_assert_cmpint(video->bit_depth, ==, 8);
	g_assert_cmpstr(video->res_class, ==, "1080p");
	g_assert_null(video->container);
	g_assert_true(video->remote);
	/* The audio was converted to another codec: the device did not take eac3. */
	g_assert_nonnull(audio);
	g_assert_cmpint(audio->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(audio->codec, ==, "eac3");
	g_assert_cmpstr(audio->out_codec, ==, "mp3");
	g_assert_cmpint(audio->channels, ==, 6);
	g_assert_true(audio->remote);
}

static void
test_direct_play_is_evidence_for_codec_and_container(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-direct.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	const AxisCompatEvidence *video = evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO);
	const AxisCompatEvidence *audio = evidence_of(evidence, AXIS_COMPAT_STREAM_AUDIO);

	g_assert_nonnull(video);
	g_assert_cmpint(video->effect, ==, AXIS_EFFECT_DIRECT);
	g_assert_cmpstr(video->codec, ==, "hevc");
	g_assert_cmpstr(video->profile, ==, "main 10");
	g_assert_cmpint(video->bit_depth, ==, 10);
	g_assert_cmpstr(video->res_class, ==, "2160p");
	g_assert_cmpint(video->hdr, ==, 1);
	g_assert_cmpstr(video->container, ==, "mkv");
	g_assert_false(video->remote);
	g_assert_nonnull(audio);
	g_assert_cmpint(audio->effect, ==, AXIS_EFFECT_DIRECT);
	g_assert_cmpstr(audio->codec, ==, "eac3");
}

static void
test_converted_video_means_the_codec_was_not_taken(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-video-converted.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	const AxisCompatEvidence *video = evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO);
	const AxisCompatEvidence *audio = evidence_of(evidence, AXIS_COMPAT_STREAM_AUDIO);

	g_assert_cmpint(video->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpstr(video->codec, ==, "hevc");
	g_assert_cmpstr(video->out_codec, ==, "h264");
	g_assert_cmpint(video->bit_depth, ==, 10);
	g_assert_cmpint(audio->effect, ==, AXIS_EFFECT_PASSTHROUGH);
}

/* Spec: Unexplained conversion */
static void
test_a_conversion_that_fits_nothing_is_cause_unclear(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-unclear.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	const AxisCompatEvidence *audio = evidence_of(evidence, AXIS_COMPAT_STREAM_AUDIO);

	g_assert_cmpint(audio->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
	g_assert_cmpint(evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
}

static void
test_a_converted_subtitle_is_always_cause_unclear(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-subtitle.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	const AxisCompatEvidence *subtitle = evidence_of(evidence, AXIS_COMPAT_STREAM_SUBTITLE);

	g_assert_nonnull(subtitle);
	g_assert_cmpint(subtitle->effect, ==, AXIS_EFFECT_CONVERTED_UNCLEAR);
	g_assert_cmpstr(subtitle->codec, ==, "pgs");
	g_assert_cmpint(evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO)->effect, ==, AXIS_EFFECT_PASSTHROUGH);
}

/* Spec: Unknown decision value */
static void
test_unknown_decision_gives_unknown_evidence(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-unknown-decision.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);

	g_assert_cmpint(evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO)->effect, ==, AXIS_EFFECT_UNKNOWN);
}

/* Spec: Bitrate-limited conversion */
static void
test_bandwidth_limited_conversions_do_not_count_against_a_codec(void)
{
	g_autoptr(GPtrArray) low = sessions_from_fixture("constructed/session-bandwidth.json");
	g_autoptr(GPtrArray) codec = sessions_from_fixture("constructed/session-bandwidth-codec.json");
	g_autoptr(GPtrArray) a = axis_compat_classify_session(g_ptr_array_index(low, 0), NULL);
	g_autoptr(GPtrArray) b = axis_compat_classify_session(g_ptr_array_index(codec, 0), NULL);

	/* Same codec, lower resolution, bandwidth below the source's bitrate. */
	g_assert_cmpint(evidence_of(a, AXIS_COMPAT_STREAM_VIDEO)->effect, ==, AXIS_EFFECT_CONVERTED_LIMIT);
	/* Even a changed codec is explained by a remote link too slow for the file. */
	g_assert_cmpint(evidence_of(b, AXIS_COMPAT_STREAM_VIDEO)->effect, ==, AXIS_EFFECT_CONVERTED_LIMIT);
}

static gchar *
local_conversion_json(
	const gchar *location,
	const gchar *local,
	gint         bandwidth,
	const gchar *source_codec,
	const gchar *out_codec,
	gint         source_height,
	gint         out_height
){
	return g_strdup_printf(
		"{\"MediaContainer\":{\"Metadata\":[{\"ratingKey\":\"9\",\"librarySectionID\":\"1\",\"Media\":[{\"bitrate\":10000,"
		"\"container\":\"mkv\",\"Part\":[{\"decision\":\"transcode\",\"container\":\"mkv\",\"Stream\":["
		"{\"streamType\":1,\"decision\":\"transcode\",\"codec\":\"%s\",\"bitDepth\":8,\"width\":%d,\"height\":%d,"
		"\"colorTrc\":\"bt709\"}]}]}],"
		"\"Player\":{\"machineIdentifier\":\"d\",\"local\":%s},\"Session\":{\"id\":\"s\",\"location\":\"%s\","
		"\"bandwidth\":%d},"
		"\"TranscodeSession\":{\"videoDecision\":\"transcode\",\"sourceVideoCodec\":\"%s\",\"videoCodec\":\"%s\","
		"\"height\":%d}}]}}", source_codec, source_height * 16 / 9, source_height, local, location, bandwidth,
		source_codec, out_codec, out_height);
}

static AxisCompatEffect
video_effect_of(const gchar *json)
{
	g_autoptr(GPtrArray) sessions = axis_compat_plex_parse_sessions(json, strlen(json), NULL);
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);
	const AxisCompatEvidence *video = evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO);

	return video != NULL ? video->effect : AXIS_EFFECT_UNKNOWN;
}

static void
test_classification_table_of_conversion_reasons(void)
{
	static const struct
	{
		const gchar     *location;
		const gchar     *local;
		gint             bandwidth;
		const gchar     *source;
		const gchar     *out;
		gint             source_height;
		gint             out_height;
		AxisCompatEffect expected;
	} table[] = {
		/* local, codec changes: the device did not take it */
		{ "lan", "true", 10000, "hevc", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_CODEC },
		/* local, same codec, lower resolution: a quality limit */
		{ "lan", "true", 10000, "h264", "h264", 1080, 720, AXIS_EFFECT_CONVERTED_LIMIT },
		/* local, same codec, same size: nothing explains it */
		{ "lan", "true", 10000, "h264", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_UNCLEAR },
		/* local, same codec, higher output: nothing explains it */
		{ "lan", "true", 10000, "h264", "h264", 720, 1080, AXIS_EFFECT_CONVERTED_UNCLEAR },
		/* remote, bandwidth below the bitrate: a limit, whatever the codec does */
		{ "wan", "false", 4000, "hevc", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_LIMIT },
		{ "wan", "false", 4000, "h264", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_LIMIT },
		/* remote, bandwidth enough: the codec rule applies */
		{ "wan", "false", 40000, "hevc", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_CODEC },
		{ "wan", "false", 40000, "h264", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_UNCLEAR },
		/* a bandwidth below the bitrate on a local session is not a remote limit */
		{ "lan", "true", 4000, "hevc", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_CODEC },
		/* bandwidth not stated: no limit can be claimed */
		{ "wan", "false", -1, "hevc", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_CODEC },
		/* the same codec under two names is the same codec */
		{ "lan", "true", 10000, "avc", "h264", 1080, 1080, AXIS_EFFECT_CONVERTED_UNCLEAR },
		{ "lan", "true", 10000, "hevc", "h265", 1080, 1080, AXIS_EFFECT_CONVERTED_UNCLEAR }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		g_autofree gchar *json = local_conversion_json(table[i].location, table[i].local, table[i].bandwidth,
			table[i].source, table[i].out, table[i].source_height, table[i].out_height);

		g_assert_cmpint(video_effect_of(json), ==, table[i].expected);
	}
}

static void
test_which_audio_track_played_is_not_guessed(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-two-audio.json");
	g_autoptr(GPtrArray) evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), NULL);

	g_assert_cmpint(evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO)->effect, ==, AXIS_EFFECT_DIRECT);
	g_assert_null(evidence_of(evidence, AXIS_COMPAT_STREAM_AUDIO));
}

static void
test_a_direct_play_needs_the_part_decision(void)
{
	const gchar *json =
		"{\"MediaContainer\":{\"Metadata\":[{\"ratingKey\":\"9\",\"Media\":[{\"Part\":[{\"container\":\"mkv\","
		"\"Stream\":[{\"streamType\":1,\"codec\":\"h264\",\"width\":1920,\"height\":1080}]}]}],"
		"\"Player\":{\"machineIdentifier\":\"d\",\"local\":true},\"Session\":{\"id\":\"s\"}}]}}";

	/* No conversion and no statement that it played directly. */
	g_assert_cmpint(video_effect_of(json), ==, AXIS_EFFECT_UNKNOWN);
}

static void
test_the_probe_describes_what_was_played(void)
{
	g_autoptr(GPtrArray) sessions = sessions_from_fixture("constructed/session-direct.json");
	AxisCompatFileFacts facts;
	g_autoptr(GPtrArray) evidence = NULL;
	const AxisCompatEvidence *video;
	AxisCompatFileFacts other;
	g_autoptr(GPtrArray) none = NULL;

	memset(&facts, 0, sizeof(facts));
	facts.video_codec = "hevc";
	facts.video_profile = "Main 10";
	facts.bit_depth = 10;
	facts.res_class = "1080p";
	facts.hdr = 0;
	facts.container = "matroska,webm";
	facts.audio_codec = "eac3";
	facts.audio_channels = 8;
	evidence = axis_compat_classify_session(g_ptr_array_index(sessions, 0), &facts);
	video = evidence_of(evidence, AXIS_COMPAT_STREAM_VIDEO);
	/* The probe's facts win over the server's summary (2160p, HDR). */
	g_assert_cmpstr(video->res_class, ==, "1080p");
	g_assert_cmpint(video->hdr, ==, 0);
	g_assert_cmpstr(video->container, ==, "mkv");
	g_assert_cmpint(evidence_of(evidence, AXIS_COMPAT_STREAM_AUDIO)->channels, ==, 8);

	/* A file whose probed codec is not the one the server states is not the
	 * file that played: no conclusion. */
	other = facts;
	other.video_codec = "h264";
	none = axis_compat_classify_session(g_ptr_array_index(sessions, 0), &other);
	g_assert_null(evidence_of(none, AXIS_COMPAT_STREAM_VIDEO));
}

static void
test_names_of_containers_and_codecs(void)
{
	static const struct
	{
		const gchar *in;
		const gchar *out;
	} containers[] = {
		{ "matroska,webm", "mkv" }, { "MKV", "mkv" }, { "webm", "mkv" }, { "mov,mp4,m4a,3gp,3g2,mj2", "mp4" },
		{ "mp4", "mp4" }, { "MP3", "mp3" }, { "flac", "flac" }, { "ogg", "ogg" }, { "avi", "avi" }
	};
	static const struct
	{
		const gchar *in;
		const gchar *out;
	} codecs[] = {
		{ "avc", "h264" }, { "H264", "h264" }, { "hvc1", "hevc" }, { "h265", "hevc" }, { "dca", "dts" },
		{ "ac-3", "ac3" }, { "e-ac-3", "eac3" }, { "AAC", "aac" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(containers); i++) {
		g_autofree gchar *family = axis_compat_container_family(containers[i].in);

		g_assert_cmpstr(family, ==, containers[i].out);
	}
	for (i = 0; i < G_N_ELEMENTS(codecs); i++) {
		g_autofree gchar *name = axis_compat_codec_name(codecs[i].in);

		g_assert_cmpstr(name, ==, codecs[i].out);
	}
	g_assert_null(axis_compat_container_family(NULL));
	g_assert_null(axis_compat_container_family("a very long container name that is not one"));
	g_assert_null(axis_compat_codec_name("bad name"));
}

/* ── "Harder than" ──────────────────────────────────────────────── */

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

	c.kind = kind;
	c.codec = codec;
	c.profile = profile;
	c.bit_depth = depth;
	c.res_class = res;
	c.hdr = hdr;
	c.container = container;
	return c;
}

#define V AXIS_COMPAT_STREAM_VIDEO
#define A AXIS_COMPAT_STREAM_AUDIO

/* Spec: Observed direct play */
static void
test_covers_table(void)
{
	static const struct
	{
		AxisCompatCombo have;
		AxisCompatCombo want;
		gboolean        expected;
		const gchar    *why;
	} table[] = {
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, TRUE, "itself" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, TRUE,
		  "higher resolution implies lower" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, FALSE,
		  "lower resolution implies nothing" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 8, "1080p", 0, "mkv" }, TRUE,
		  "higher bit depth implies lower" },
		{ { V, "hevc", "main 10", 8, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "lower bit depth implies nothing" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "h264", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "another codec" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mp4" }, FALSE,
		  "another container" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, NULL }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "a container never seen" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, NULL }, TRUE,
		  "no container asked" },
		{ { V, "hevc", "main", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "another profile" },
		{ { V, "hevc", NULL, 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "a profile never seen" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", NULL, 10, "1080p", 0, "mkv" }, TRUE,
		  "no profile asked" },
		{ { V, "hevc", "Main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "MKV" }, TRUE,
		  "case does not matter" },
		{ { V, "hevc", "main 10", 10, "other", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "an unordered resolution covers nothing" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "other", 0, "mkv" }, FALSE,
		  "and is never covered" },
		{ { V, "hevc", "main 10", 10, NULL, 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "a resolution never seen" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, NULL, 0, "mkv" }, TRUE,
		  "no resolution asked" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 1, "mkv" }, FALSE,
		  "SDR shows nothing about HDR" },
		{ { V, "hevc", "main 10", 10, "2160p", 1, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "nor the other way round" },
		{ { V, "hevc", "main 10", 10, "2160p", -1, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "an HDR class never seen" },
		{ { V, "hevc", "main 10", 10, "2160p", 1, "mkv" }, { V, "hevc", "main 10", 10, "1080p", -1, "mkv" }, TRUE,
		  "no HDR class asked" },
		{ { V, "hevc", "main 10", 0, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "a bit depth never seen" },
		{ { A, "aac", NULL, 0, NULL, -1, "mp4" }, { V, "aac", NULL, 0, NULL, -1, "mp4" }, FALSE, "another kind" },
		{ { A, "flac", NULL, 0, NULL, -1, "flac" }, { A, "flac", NULL, 0, NULL, -1, "flac" }, TRUE, "audio itself" },
		{ { A, "flac", NULL, 0, NULL, -1, "flac" }, { A, "mp3", NULL, 0, NULL, -1, "mp3" }, FALSE, "another audio codec" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (axis_compat_covers(&table[i].have, &table[i].want) != table[i].expected)
			g_error("covers case %u (%s): expected %d", i, table[i].why, table[i].expected);
	}
}

static void
test_covers_codec_never_covers_a_container(void)
{
	AxisCompatCombo have = combo(V, "h264", "high", 8, "1080p", 0, NULL);
	AxisCompatCombo with_container = combo(V, "h264", "high", 8, "1080p", 0, "mkv");
	AxisCompatCombo without = combo(V, "h264", "high", 8, "1080p", 0, NULL);

	g_assert_false(axis_compat_covers_codec(&have, &with_container));
	g_assert_true(axis_compat_covers_codec(&have, &without));
}

static void
test_fails_table(void)
{
	static const struct
	{
		AxisCompatCombo failed;
		AxisCompatCombo want;
		gboolean        expected;
		const gchar    *why;
	} table[] = {
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, TRUE, "itself" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, TRUE,
		  "a harder combination fails too" },
		{ { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "an easier one is not shown to fail" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main", 8, "1080p", 0, "mkv" }, FALSE,
		  "a lower bit depth is not shown to fail" },
		{ { V, "hevc", "main", 8, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, TRUE,
		  "a higher bit depth fails" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "h264", "high", 8, "1080p", 0, "mkv" }, FALSE,
		  "another codec" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mp4" }, TRUE,
		  "the container does not matter: the codec was not taken" },
		{ { V, "hevc", NULL, 0, NULL, -1, NULL }, { V, "hevc", "main 10", 10, "2160p", 1, "mkv" }, TRUE,
		  "a failure that states little applies to every hevc" },
		{ { V, "hevc", "main 10", 10, "other", 0, "mkv" }, { V, "hevc", "main 10", 10, "2160p", 0, "mkv" }, FALSE,
		  "an unordered resolution shows nothing" },
		{ { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, { V, "hevc", "main 10", 10, NULL, 0, "mkv" }, FALSE,
		  "a resolution never stated shows nothing" },
		{ { V, "hevc", "main 10", 10, "1080p", 1, "mkv" }, { V, "hevc", "main 10", 10, "1080p", 0, "mkv" }, FALSE,
		  "an HDR failure is not shown for SDR" },
		{ { A, "eac3", NULL, 0, NULL, -1, NULL }, { A, "eac3", NULL, 0, NULL, -1, "mkv" }, TRUE, "audio" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (axis_compat_fails(&table[i].failed, &table[i].want) != table[i].expected)
			g_error("fails case %u (%s): expected %d", i, table[i].why, table[i].expected);
	}
}

/* ── Verdicts ───────────────────────────────────────────────────── */

typedef struct
{
	AxisCompatEffect effect;
	AxisCompatCombo  combo;
} Item;

static AxisCompatVerdict
verdict_of(
	const AxisCompatCombo *want,
	const Item            *items,
	guint                  n,
	AxisCompatWhy         *why
){
	AxisCompatObservation obs[16];
	guint i;

	g_assert_cmpuint(n, <=, 16);
	for (i = 0; i < n; i++) {
		memset(&obs[i], 0, sizeof(obs[i]));
		obs[i].combo = items[i].combo;
		obs[i].effect = items[i].effect;
		obs[i].plays = 1;
	}
	return axis_compat_device_verdict(want, obs, n, why, NULL);
}

/* The target the table asks about: HEVC Main 10, 10-bit, 1080p, SDR, in Matroska. */
#define WANT combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv")

/* Spec: Safe / Risky / Unknown is never safe / No evidence */
static void
test_device_verdict_table(void)
{
	AxisCompatCombo want = WANT;
	AxisCompatWhy why;
	Item direct_same = { AXIS_EFFECT_DIRECT, WANT };
	Item direct_harder = { AXIS_EFFECT_DIRECT, combo(V, "hevc", "main 10", 10, "2160p", 0, "mkv") };
	Item direct_easier = { AXIS_EFFECT_DIRECT, combo(V, "hevc", "main 10", 10, "720p", 0, "mkv") };
	Item direct_other_container = { AXIS_EFFECT_DIRECT, combo(V, "hevc", "main 10", 10, "1080p", 0, "mp4") };
	Item direct_other_codec = { AXIS_EFFECT_DIRECT, combo(V, "h264", "high", 8, "1080p", 0, "mkv") };
	Item passed = { AXIS_EFFECT_PASSTHROUGH, combo(V, "hevc", "main 10", 10, "1080p", 0, NULL) };
	Item failed_same = { AXIS_EFFECT_CONVERTED_CODEC, combo(V, "hevc", "main 10", 10, "1080p", 0, "mkv") };
	Item failed_easier = { AXIS_EFFECT_CONVERTED_CODEC, combo(V, "hevc", "main 10", 8, "720p", 0, "mkv") };
	Item failed_harder = { AXIS_EFFECT_CONVERTED_CODEC, combo(V, "hevc", "main 10", 10, "2160p", 0, "mkv") };
	Item limited = { AXIS_EFFECT_CONVERTED_LIMIT, WANT };
	Item unclear = { AXIS_EFFECT_CONVERTED_UNCLEAR, WANT };
	Item unknown = { AXIS_EFFECT_UNKNOWN, WANT };
	Item both[2];

	/* No evidence at all. */
	g_assert_cmpint(verdict_of(&want, NULL, 0, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_NO_EVIDENCE);
	/* Direct play of the combination, or a harder one. */
	g_assert_cmpint(verdict_of(&want, &direct_same, 1, &why), ==, AXIS_VERDICT_SAFE);
	g_assert_cmpint(why, ==, AXIS_WHY_DIRECT);
	g_assert_cmpint(verdict_of(&want, &direct_harder, 1, &why), ==, AXIS_VERDICT_SAFE);
	/* An easier one, another container, another codec: unknown. */
	g_assert_cmpint(verdict_of(&want, &direct_easier, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_EASIER_ONLY);
	g_assert_cmpint(verdict_of(&want, &direct_other_container, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_EASIER_ONLY);
	g_assert_cmpint(verdict_of(&want, &direct_other_codec, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_NO_EVIDENCE);
	/* Passed through: the codec, not the container. */
	g_assert_cmpint(verdict_of(&want, &passed, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_PASSTHROUGH_ONLY);
	/* Seen failing the combination or a harder... */
	g_assert_cmpint(verdict_of(&want, &failed_same, 1, &why), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(why, ==, AXIS_WHY_CONVERTED);
	g_assert_cmpint(verdict_of(&want, &failed_easier, 1, &why), ==, AXIS_VERDICT_RISKY);
	/* ...but not failing a harder one only. */
	g_assert_cmpint(verdict_of(&want, &failed_harder, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	/* A bandwidth limit and an unclear cause are not evidence of failure. */
	g_assert_cmpint(verdict_of(&want, &limited, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_LIMITED_ONLY);
	g_assert_cmpint(verdict_of(&want, &unclear, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_UNCLEAR_ONLY);
	g_assert_cmpint(verdict_of(&want, &unknown, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(why, ==, AXIS_WHY_NO_EVIDENCE);
	/* Seen playing and seen failing the same combination: the cautious reading. */
	both[0] = direct_same;
	both[1] = failed_same;
	g_assert_cmpint(verdict_of(&want, both, 2, &why), ==, AXIS_VERDICT_RISKY);
	/* Seen playing a harder combination and failing an easier... (a contradiction): risky */
	both[0] = direct_harder;
	both[1] = failed_easier;
	g_assert_cmpint(verdict_of(&want, both, 2, &why), ==, AXIS_VERDICT_RISKY);
	/* Evidence for another stream kind says nothing. */
	{
		Item audio = { AXIS_EFFECT_DIRECT, combo(A, "hevc", NULL, 0, NULL, -1, "mkv") };

		g_assert_cmpint(verdict_of(&want, &audio, 1, &why), ==, AXIS_VERDICT_UNKNOWN);
	}
	/* A want that names no container is supported by a passed-through stream. */
	{
		AxisCompatCombo loose = combo(V, "hevc", "main 10", 10, "1080p", 0, NULL);

		g_assert_cmpint(verdict_of(&loose, &passed, 1, &why), ==, AXIS_VERDICT_SAFE);
	}
}

/* Reference model of the verdict, written differently from the code, over
 * every subset of a small universe of observations: "Table-test the verdict
 * function exhaustively". */
static void
test_device_verdict_exhaustive(void)
{
	AxisCompatCombo want = WANT;
	Item universe[] = {
		{ AXIS_EFFECT_DIRECT, WANT },
		{ AXIS_EFFECT_DIRECT, combo(V, "hevc", "main 10", 10, "2160p", 0, "mkv") },
		{ AXIS_EFFECT_DIRECT, combo(V, "hevc", "main 10", 10, "720p", 0, "mkv") },
		{ AXIS_EFFECT_DIRECT, combo(V, "h264", "high", 8, "2160p", 0, "mkv") },
		{ AXIS_EFFECT_PASSTHROUGH, combo(V, "hevc", "main 10", 10, "2160p", 0, NULL) },
		{ AXIS_EFFECT_CONVERTED_CODEC, combo(V, "hevc", "main 10", 8, "720p", 0, "mkv") },
		{ AXIS_EFFECT_CONVERTED_CODEC, combo(V, "hevc", "main 10", 10, "2160p", 0, "mkv") },
		{ AXIS_EFFECT_CONVERTED_LIMIT, WANT },
		{ AXIS_EFFECT_CONVERTED_UNCLEAR, WANT },
		{ AXIS_EFFECT_UNKNOWN, WANT }
	};
	guint n = G_N_ELEMENTS(universe);
	guint mask;

	for (mask = 0; mask < (1u << n); mask++) {
		Item picked[16];
		guint count = 0;
		guint i;
		/* Reference: safe iff the combination, or a harder one, was played directly (index 0, 1); risky
		 * iff a failure at or below it was seen (index 5); everything else is unknown. */
		gboolean safe = (mask & 1u) != 0 || (mask & 2u) != 0;
		gboolean risky = (mask & (1u << 5)) != 0;
		AxisCompatVerdict expected = risky ? AXIS_VERDICT_RISKY : (safe ? AXIS_VERDICT_SAFE : AXIS_VERDICT_UNKNOWN);

		for (i = 0; i < n; i++) {
			if ((mask & (1u << i)) != 0)
				picked[count++] = universe[i];
		}
		g_assert_cmpint(verdict_of(&want, picked, count, NULL), ==, expected);
		/* Nothing but a direct play ever makes it safe. */
		if (mask == 0)
			g_assert_cmpint(verdict_of(&want, picked, count, NULL), ==, AXIS_VERDICT_UNKNOWN);
	}
}

static void
test_verdict_words_and_worse(void)
{
	g_assert_cmpstr(axis_compat_verdict_to_string(AXIS_VERDICT_SAFE), ==, "safe");
	g_assert_cmpstr(axis_compat_verdict_label(AXIS_VERDICT_RISKY), ==, "Risky");
	g_assert_cmpint(axis_compat_verdict_worse(AXIS_VERDICT_SAFE, AXIS_VERDICT_SAFE), ==, AXIS_VERDICT_SAFE);
	g_assert_cmpint(axis_compat_verdict_worse(AXIS_VERDICT_SAFE, AXIS_VERDICT_UNKNOWN), ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(axis_compat_verdict_worse(AXIS_VERDICT_UNKNOWN, AXIS_VERDICT_RISKY), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(axis_compat_verdict_worse(AXIS_VERDICT_RISKY, AXIS_VERDICT_SAFE), ==, AXIS_VERDICT_RISKY);
	g_assert_cmpint(axis_compat_resolution_rank("sd"), <, axis_compat_resolution_rank("720p"));
	g_assert_cmpint(axis_compat_resolution_rank("720p"), <, axis_compat_resolution_rank("1080p"));
	g_assert_cmpint(axis_compat_resolution_rank("1080p"), <, axis_compat_resolution_rank("2160p"));
	g_assert_cmpint(axis_compat_resolution_rank("other"), ==, 0);
	g_assert_cmpint(axis_compat_resolution_rank(NULL), ==, 0);
}

/* ── Recommendation ─────────────────────────────────────────────── */

/* Spec: Largest safe saving */
static void
test_recommend_picks_the_largest_safe_saving(void)
{
	AxisCompatChoice choices[] = {
		{ AXIS_VERDICT_SAFE, 100 },
		{ AXIS_VERDICT_SAFE, 300 },
		{ AXIS_VERDICT_UNKNOWN, 900 },
		{ AXIS_VERDICT_RISKY, 700 }
	};
	gint best;
	gint next;

	g_assert_cmpint(axis_compat_recommend(choices, 4, &best, &next), ==, 1);
	g_assert_cmpint(best, ==, 2);
	g_assert_cmpint(next, ==, 3);
}

/* Spec: Nothing safe */
static void
test_recommend_nothing_when_nothing_is_safe(void)
{
	AxisCompatChoice choices[] = {
		{ AXIS_VERDICT_UNKNOWN, 500 },
		{ AXIS_VERDICT_RISKY, 800 },
		{ AXIS_VERDICT_UNKNOWN, 200 }
	};
	gint best;
	gint next;

	g_assert_cmpint(axis_compat_recommend(choices, 3, &best, &next), ==, -1);
	g_assert_cmpint(best, ==, 1);
	g_assert_cmpint(next, ==, 0);
}

static void
test_recommend_never_recommends_unknown_and_needs_a_saving(void)
{
	AxisCompatChoice unknown_only[] = { { AXIS_VERDICT_UNKNOWN, 1000000 } };
	AxisCompatChoice no_saving[] = { { AXIS_VERDICT_SAFE, 0 }, { AXIS_VERDICT_SAFE, -1 } };
	AxisCompatChoice tie[] = { { AXIS_VERDICT_SAFE, 50 }, { AXIS_VERDICT_SAFE, 50 } };
	gint best;
	gint next;

	g_assert_cmpint(axis_compat_recommend(unknown_only, 1, &best, &next), ==, -1);
	g_assert_cmpint(axis_compat_recommend(no_saving, 2, &best, &next), ==, -1);
	g_assert_cmpint(best, ==, -1);
	g_assert_cmpint(axis_compat_recommend(tie, 2, &best, &next), ==, 0);
	g_assert_cmpint(axis_compat_recommend(NULL, 0, &best, &next), ==, -1);
}

/* ── The address and the path map ───────────────────────────────── */

static void
test_address_table(void)
{
	static const struct
	{
		const gchar *text;
		gboolean     ok;
		const gchar *base;
	} table[] = {
		{ "http://192.168.1.10:32400", TRUE, "http://192.168.1.10:32400" },
		{ "http://192.168.1.10:32400/", TRUE, "http://192.168.1.10:32400" },
		{ "https://Plex.Example:443", TRUE, "https://plex.example:443" },
		{ "http://plex", TRUE, "http://plex:80" },
		{ "https://plex", TRUE, "https://plex:443" },
		{ "http://[::1]:32400", TRUE, "http://[::1]:32400" },
		{ "", FALSE, NULL },
		{ "ftp://plex:32400", FALSE, NULL },
		{ "file:///etc/passwd", FALSE, NULL },
		{ "plex:32400", FALSE, NULL },
		{ "//plex:32400", FALSE, NULL },
		{ "http://", FALSE, NULL },
		{ "http://user:pass@plex:32400", FALSE, NULL },
		{ "http://user@plex:32400", FALSE, NULL },
		{ "http://plex:32400/library", FALSE, NULL },
		{ "http://plex:32400/?X-Plex-Token=abc", FALSE, NULL },
		{ "http://plex:32400#frag", FALSE, NULL },
		{ "http://plex:0", FALSE, NULL },
		{ "http://plex:99999", FALSE, NULL },
		{ "http://pl ex:32400", FALSE, NULL },
		{ "http://plex:32400\n", FALSE, NULL },
		{ " http://plex:32400", FALSE, NULL },
		{ "http://plex\\evil:32400", FALSE, NULL },
		/* No percent-encoding, and a colon only in a real IPv6 address (the fuzzer
		 * found "http://----%3A/", whose decoded host was "----:"). */
		{ "http://----%3A/", FALSE, NULL },
		{ "http://plex%2eexample:32400", FALSE, NULL },
		{ "http://[::1%25eth0]:32400", FALSE, NULL },
		{ "http://[not:an:address]:32400", FALSE, NULL },
		{ "http://[2001:db8::1]:32400", TRUE, "http://[2001:db8::1]:32400" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		AxisCompatAddress address;
		g_autofree gchar *problem = NULL;
		gboolean ok = axis_compat_address_parse(table[i].text, &address, &problem);

		if (ok != table[i].ok)
			g_error("address case %u: %s", i, table[i].text);
		if (ok) {
			g_assert_cmpstr(address.base, ==, table[i].base);
			g_assert_null(problem);
		} else {
			g_assert_nonnull(problem);
			/* The reason never quotes the address. */
			g_assert_null(strstr(problem, "plex"));
		}
		axis_compat_address_clear(&address);
	}
}

static void
test_path_normalize_table(void)
{
	static const struct
	{
		const gchar *text;
		const gchar *expected;
	} table[] = {
		{ "/data/movies", "/data/movies" }, { "/data/movies/", "/data/movies" }, { "/", "/" },
		{ "data/movies", NULL }, { "", NULL }, { "/data//movies", NULL }, { "/data/./movies", NULL },
		{ "/data/../movies", NULL }, { "//", NULL }, { "/data/mov\nies", NULL }, { "/data/\xff", NULL }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		g_autofree gchar *path = axis_compat_path_normalize(table[i].text, NULL);

		g_assert_cmpstr(path, ==, table[i].expected);
	}
}

static GPtrArray *
map_of(const gchar *first, ...)
{
	GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_path_entry_free);
	va_list args;
	const gchar *server = first;

	va_start(args, first);
	while (server != NULL) {
		const gchar *axis = va_arg(args, const gchar *);
		AxisCompatPathEntry *e = g_new0(AxisCompatPathEntry, 1);

		e->server = g_strdup(server);
		e->axis = g_strdup(axis);
		g_ptr_array_add(entries, e);
		server = va_arg(args, const gchar *);
	}
	va_end(args);
	return entries;
}

static GPtrArray *
roots_of(const gchar *first, ...)
{
	GPtrArray *roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_root_ref_free);
	va_list args;
	const gchar *path = first;
	gint64 id = 1;

	va_start(args, first);
	while (path != NULL) {
		AxisCompatRootRef *r = g_new0(AxisCompatRootRef, 1);

		r->id = id++;
		r->path = g_strdup(path);
		r->media_type = g_strdup("movies");
		g_ptr_array_add(roots, r);
		path = va_arg(args, const gchar *);
	}
	va_end(args);
	return roots;
}

static void
test_path_map_applies_the_longest_prefix_on_a_boundary(void)
{
	g_autoptr(GPtrArray) map = map_of("/data", "/media/disk1", "/data/movies", "/media/disk3/movies", "/", "/mnt", NULL);
	g_autofree gchar *a = axis_compat_path_map_apply(map, "/data/movies/Film (2001)/Film.mkv");
	g_autofree gchar *b = axis_compat_path_map_apply(map, "/data/tv/Show/S01E01.mkv");
	g_autofree gchar *c = axis_compat_path_map_apply(map, "/data/movies");
	g_autofree gchar *d = axis_compat_path_map_apply(map, "/data/moviesextra/x.mkv");
	g_autofree gchar *e = axis_compat_path_map_apply(map, "/other/x.mkv");
	g_autofree gchar *f = axis_compat_path_map_apply(map, "/data/movies/../../etc/passwd");
	g_autofree gchar *g = axis_compat_path_map_apply(map, "relative/x.mkv");

	g_assert_cmpstr(a, ==, "/media/disk3/movies/Film (2001)/Film.mkv");
	g_assert_cmpstr(b, ==, "/media/disk1/tv/Show/S01E01.mkv");
	g_assert_cmpstr(c, ==, "/media/disk3/movies");
	/* "/data/moviesextra" is not below "/data/movies": the "/data" entry reaches it. */
	g_assert_cmpstr(d, ==, "/media/disk1/moviesextra/x.mkv");
	g_assert_cmpstr(e, ==, "/mnt/other/x.mkv");
	/* A path with a ".." is never mapped. */
	g_assert_null(f);
	g_assert_null(g);
	{
		/* A mapping to a longer prefix must not make a path longer than a
		 * path may be (found by the fuzzer): 1024 bytes as typed fit, and
		 * the result would not. */
		g_autofree gchar *filler = g_strnfill(AXIS_COMPAT_PATH_MAX_BYTES - strlen("/data/"), 'x');
		g_autofree gchar *longest = g_strconcat("/data/", filler, NULL);
		g_autofree gchar *fits = g_strndup(longest, AXIS_COMPAT_PATH_MAX_BYTES - 10);
		g_autofree gchar *over = axis_compat_path_map_apply(map, longest);
		g_autofree gchar *within = axis_compat_path_map_apply(map, fits);

		g_assert_cmpuint(strlen(longest), ==, AXIS_COMPAT_PATH_MAX_BYTES);
		g_assert_null(over);
		g_assert_nonnull(within);
		g_assert_cmpuint(strlen(within), <=, AXIS_COMPAT_PATH_MAX_BYTES);
	}
}

static void
test_root_for_path_finds_the_longest_root(void)
{
	g_autoptr(GPtrArray) roots = roots_of("/media/disk3", "/media/disk3/movies", NULL);
	g_autofree gchar *rel = NULL;
	const AxisCompatRootRef *r = axis_compat_root_for_path(roots, "/media/disk3/movies/Film/Film.mkv", &rel);

	g_assert_nonnull(r);
	g_assert_cmpint(r->id, ==, 2);
	g_assert_cmpstr(rel, ==, "Film/Film.mkv");
	g_clear_pointer(&rel, g_free);
	r = axis_compat_root_for_path(roots, "/media/disk3/other/x.mkv", &rel);
	g_assert_cmpint(r->id, ==, 1);
	g_assert_cmpstr(rel, ==, "other/x.mkv");
	g_clear_pointer(&rel, g_free);
	/* A root itself is not a file in it, and a longer name is not inside it. */
	{
		g_autoptr(GPtrArray) one = roots_of("/media/disk3/movies", NULL);

		g_assert_null(axis_compat_root_for_path(one, "/media/disk3/movies", &rel));
		g_assert_null(axis_compat_root_for_path(one, "/media/disk3/movies2/x.mkv", &rel));
	}
	g_assert_null(axis_compat_root_for_path(roots, "/media/disk33/x.mkv", &rel));
	g_assert_null(axis_compat_root_for_path(roots, "/elsewhere/x.mkv", &rel));
}

/* Spec: Mapped through the path map / Unmapped library */
static void
test_library_mapping_states_and_suggestions(void)
{
	g_autoptr(GPtrArray) map = map_of("/data/movies", "/media/disk3/movies", "/data/tv", "/media/disk3/tv", NULL);
	g_autoptr(GPtrArray) roots = roots_of("/media/disk3/movies", "/media/disk3/tv", "/media/disk4/music", "/media/disk4/audiobooks", NULL);
	g_autoptr(GPtrArray) mapped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) partly = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) unmapped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) none = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) inside = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) containing = g_ptr_array_new_with_free_func(g_free);
	AxisCompatMapping m;
	const AxisCompatSuggestion *s;

	g_ptr_array_add(mapped, g_strdup("/data/movies"));
	g_ptr_array_add(partly, g_strdup("/data/tv"));
	g_ptr_array_add(partly, g_strdup("/data/tv-extra"));
	g_ptr_array_add(unmapped, g_strdup("/data/music"));
	g_ptr_array_add(inside, g_strdup("/data/movies/Kids"));
	g_ptr_array_add(containing, g_strdup("/media/disk3"));

	axis_compat_map_library(mapped, map, roots, &m);
	g_assert_cmpint(m.state, ==, AXIS_MAP_MAPPED);
	g_assert_cmpuint(m.root_ids->len, ==, 1);
	g_assert_cmpint(g_array_index(m.root_ids, gint64, 0), ==, 1);
	g_assert_cmpuint(m.unmapped_folders->len, ==, 0);
	axis_compat_mapping_clear(&m);

	/* One of two folders reaches no root: partly mapped, so no verdicts. */
	axis_compat_map_library(partly, map, roots, &m);
	g_assert_cmpint(m.state, ==, AXIS_MAP_PARTLY);
	g_assert_cmpuint(m.unmapped_folders->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(m.unmapped_folders, 0), ==, "/data/tv-extra");
	axis_compat_mapping_clear(&m);

	/* No map entry reaches /data/music, but a root ends in "music": suggest it. */
	axis_compat_map_library(unmapped, map, roots, &m);
	g_assert_cmpint(m.state, ==, AXIS_MAP_UNMAPPED);
	g_assert_cmpuint(m.suggestions->len, ==, 1);
	s = g_ptr_array_index(m.suggestions, 0);
	g_assert_cmpstr(s->folder, ==, "/data/music");
	g_assert_cmpstr(s->root_path, ==, "/media/disk4/music");
	g_assert_cmpint(s->root_id, ==, 3);
	axis_compat_mapping_clear(&m);

	/* A library with no folders is unmapped. */
	axis_compat_map_library(none, map, roots, &m);
	g_assert_cmpint(m.state, ==, AXIS_MAP_UNMAPPED);
	axis_compat_mapping_clear(&m);

	/* A folder inside a root, and one that contains roots, reach them. */
	{
		g_autoptr(GPtrArray) wide = map_of("/data/movies", "/media/disk3/movies", "/all", "/media/disk3", NULL);

		axis_compat_map_library(inside, wide, roots, &m);
		g_assert_cmpint(m.state, ==, AXIS_MAP_MAPPED);
		axis_compat_mapping_clear(&m);
		g_clear_pointer(&containing, g_ptr_array_unref);
		containing = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(containing, g_strdup("/all"));
		axis_compat_map_library(containing, wide, roots, &m);
		g_assert_cmpint(m.state, ==, AXIS_MAP_MAPPED);
		g_assert_cmpuint(m.root_ids->len, ==, 2);
		axis_compat_mapping_clear(&m);
	}
}

static void
test_two_candidate_roots_make_no_suggestion(void)
{
	g_autoptr(GPtrArray) map = map_of(NULL);
	g_autoptr(GPtrArray) roots = roots_of("/media/disk3/movies", "/media/disk4/movies", NULL);
	g_autoptr(GPtrArray) folders = g_ptr_array_new_with_free_func(g_free);
	AxisCompatMapping m;

	g_ptr_array_add(folders, g_strdup("/data/movies"));
	axis_compat_map_library(folders, map, roots, &m);
	g_assert_cmpint(m.state, ==, AXIS_MAP_UNMAPPED);
	g_assert_cmpuint(m.suggestions->len, ==, 0);
	axis_compat_mapping_clear(&m);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/compat/parse/identity-and-root", test_parse_identity_and_root);
	g_test_add_func("/compat/parse/libraries", test_parse_libraries);
	g_test_add_func("/compat/parse/devices-accounts-history-item", test_parse_devices_accounts_history_item);
	g_test_add_func("/compat/parse/observed-session", test_parse_the_observed_session);
	g_test_add_func("/compat/parse/empty-sessions", test_parse_empty_sessions);
	g_test_add_func("/compat/parse/unknown-decision", test_unknown_decision_word_is_unknown);
	g_test_add_func("/compat/parse/not-a-container", test_parser_rejects_what_is_not_a_media_container);
	g_test_add_func("/compat/parse/out-of-range", test_parser_treats_out_of_range_and_odd_fields_as_not_stated);
	g_test_add_func("/compat/parse/long-ids", test_parser_drops_long_ids_and_paths_instead_of_cutting_them);
	g_test_add_func("/compat/parse/caps-and-cleaning", test_parser_caps_lists_and_cleans_text);

	g_test_add_func("/compat/evidence/per-stream", test_observed_session_is_evidence_per_stream);
	g_test_add_func("/compat/evidence/direct", test_direct_play_is_evidence_for_codec_and_container);
	g_test_add_func("/compat/evidence/converted-video", test_converted_video_means_the_codec_was_not_taken);
	g_test_add_func("/compat/evidence/unclear", test_a_conversion_that_fits_nothing_is_cause_unclear);
	g_test_add_func("/compat/evidence/subtitle", test_a_converted_subtitle_is_always_cause_unclear);
	g_test_add_func("/compat/evidence/unknown-decision", test_unknown_decision_gives_unknown_evidence);
	g_test_add_func("/compat/evidence/bandwidth", test_bandwidth_limited_conversions_do_not_count_against_a_codec);
	g_test_add_func("/compat/evidence/reasons-table", test_classification_table_of_conversion_reasons);
	g_test_add_func("/compat/evidence/two-audio", test_which_audio_track_played_is_not_guessed);
	g_test_add_func("/compat/evidence/part-decision", test_a_direct_play_needs_the_part_decision);
	g_test_add_func("/compat/evidence/probe-facts", test_the_probe_describes_what_was_played);
	g_test_add_func("/compat/evidence/names", test_names_of_containers_and_codecs);

	g_test_add_func("/compat/harder/covers", test_covers_table);
	g_test_add_func("/compat/harder/covers-codec", test_covers_codec_never_covers_a_container);
	g_test_add_func("/compat/harder/fails", test_fails_table);
	g_test_add_func("/compat/verdict/table", test_device_verdict_table);
	g_test_add_func("/compat/verdict/exhaustive", test_device_verdict_exhaustive);
	g_test_add_func("/compat/verdict/words", test_verdict_words_and_worse);
	g_test_add_func("/compat/recommend/largest-safe", test_recommend_picks_the_largest_safe_saving);
	g_test_add_func("/compat/recommend/nothing-safe", test_recommend_nothing_when_nothing_is_safe);
	g_test_add_func("/compat/recommend/never-unknown", test_recommend_never_recommends_unknown_and_needs_a_saving);

	g_test_add_func("/compat/address/table", test_address_table);
	g_test_add_func("/compat/path/normalize", test_path_normalize_table);
	g_test_add_func("/compat/path/map", test_path_map_applies_the_longest_prefix_on_a_boundary);
	g_test_add_func("/compat/path/root-for-path", test_root_for_path_finds_the_longest_root);
	g_test_add_func("/compat/path/library-mapping", test_library_mapping_states_and_suggestions);
	g_test_add_func("/compat/path/two-candidates", test_two_candidate_roots_make_no_suggestion);
	return g_test_run();
}
