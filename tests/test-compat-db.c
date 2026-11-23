/*
 * test-compat-db.c - the media server connection, the collection of
 * sessions and history, the evidence tables, the verdicts and the
 * recommendation, against a database and a fake media server
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios of client-compatibility covered here: Token stays secret
 * (settings), Connection test, Mapped through the path map, Unmapped
 * library, Evidence is per stream, Unexplained conversion, Unknown decision
 * value, History has no decisions, Session recorded once, Retention,
 * Safe, Risky, Unknown is never safe, No evidence, Pinned device, Current
 * failures, Largest safe saving, Nothing safe, Names off by default.
 *
 * The sessions come from tests/fixtures/compat (the shapes the real server
 * was seen to answer) and tests/fixtures/compat/constructed (shapes not
 * yet seen, labelled so).
 */

#include <glib.h>
#include <string.h>

#include "axis-test-compat.h"
#include "axis-test-jobs.h"
#include "core/axis-error.h"

typedef struct
{
	AxisTestCompat *t;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the media server is not the one*");
	fx->t = axis_test_compat_new(GPOINTER_TO_INT(data) != 0);
	if (fx->t != NULL) {
		axis_test_compat_connect(fx->t);
		if (GPOINTER_TO_INT(data) != 0)
			axis_test_compat_map_standard(fx->t);
	}
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_compat_free(fx->t);
	axis_test_log_reset();
}

/* The observed session, playing the film "Big (2001)" of the Films root. */
static void
serve_session(
	Fixture     *fx,
	const gchar *fixture,
	const gchar *from,
	const gchar *to
){
	g_autofree gchar *text = axis_test_compat_fixture(fixture, from, to);

	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, text, 0);
}

static void
serve_big(
	Fixture     *fx,
	const gchar *session_id
){
	g_autofree gchar *text = axis_test_compat_fixture("session-converted-audio.json", "Invented Film (2001)", "Big (2001)");
	GString *s = g_string_new(text);
	g_autofree gchar *out = NULL;

	g_string_replace(s, "sess-converted-audio-1", session_id, 0);
	out = g_string_free(s, FALSE);
	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, out, 0);
}

static AxisCompatPollSummary
poll(
	Fixture  *fx,
	gboolean  force
){
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_poll(fx->t->compat, force, NULL, &summary, &error));
	g_assert_no_error(error);
	return summary;
}

static gchar *
query(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->t->lib->db, sql);
}

/* ── Settings ───────────────────────────────────────────────────── */

/* Spec: Token stays secret (the token is a secret setting; the address is validated on write) */
static void
test_settings_validate_the_address_and_keep_the_token_secret(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad[] = {
		"ftp://plex.example:21", "http://user:hunter2@plex.example:32400", "http://plex.example:32400/library",
		"http://plex.example:32400/?X-Plex-Token=hunter2", "plex.example:32400", "http://", "https://plex.example#hunter2", NULL
	};
	g_autofree gchar *shown = NULL;
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_ADDRESS, bad[i], &error));
		g_assert_error(error, AXIS_ERROR, AXIS_ERROR_SETTING);
		/* The refusal names the setting, never the value. */
		g_assert_null(strstr(error->message, "hunter2"));
	}
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_ADDRESS, "https://plex.example:32400", NULL));
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_ADDRESS, "", NULL));

	/* The token is never given back. */
	g_assert_true(axis_settings_is_set(fx->t->settings, AXIS_COMPAT_SETTING_TOKEN));
	shown = axis_settings_format(fx->t->settings, AXIS_COMPAT_SETTING_TOKEN);
	g_assert_null(shown);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_ACCOUNT_NAMES, "false", NULL));
	g_assert_cmpint(axis_settings_get_duration(fx->t->settings, AXIS_COMPAT_SETTING_LOOKBACK), ==, 180 * 86400);
	g_assert_cmpint(axis_settings_get_duration(fx->t->settings, AXIS_COMPAT_SETTING_RETENTION), ==, 400 * 86400);
	g_assert_false(axis_settings_get_boolean(fx->t->settings, AXIS_COMPAT_SETTING_ACCOUNT_NAMES));
	g_assert_false(axis_settings_get_boolean(fx->t->settings, AXIS_COMPAT_SETTING_UNTRUSTED_TLS));
}

/* ── The connection ─────────────────────────────────────────────── */

/* Spec: Connection test */
static void
test_connection_test_reports_name_and_version_or_the_reason(Fixture *fx, gconstpointer data)
{
	AxisCompatTestResult result;

	SKIP_WITHOUT_DB(fx);
	axis_compat_test_connection(fx->t->compat, NULL, &result);
	g_assert_true(result.ok);
	g_assert_cmpstr(result.name, ==, "Invented Server");
	g_assert_cmpstr(result.version, ==, "1.43.5.0-invented");
	axis_compat_test_result_clear(&result);

	/* The server refuses the token. */
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	axis_compat_test_connection(fx->t->compat, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "token was refused"));
	g_assert_null(strstr(result.message, AXIS_TEST_COMPAT_TOKEN));
	axis_compat_test_result_clear(&result);

	/* Not a media server at all. */
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_NORMAL, 200, "<html>hello</html>", 0);
	axis_compat_test_connection(fx->t->compat, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "could not be used"));
	axis_compat_test_result_clear(&result);

	/* Not set up. */
	g_assert_true(axis_settings_clear(fx->t->settings, AXIS_COMPAT_SETTING_TOKEN, NULL));
	axis_compat_test_connection(fx->t->compat, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "not both set"));
	axis_compat_test_result_clear(&result);
	g_assert_false(axis_compat_is_configured(fx->t->compat));
}

/* ── Sessions ───────────────────────────────────────────────────── */

/* Spec: Session recorded once / Evidence is per stream / Mapped through the path map */
static void
test_a_session_is_recorded_once_and_matched_through_the_path_map(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	gint64 big;
	g_autofree gchar *file_id = NULL;
	g_autofree gchar *effect = NULL;
	g_autofree gchar *codec = NULL;
	g_autofree gchar *container = NULL;
	g_autofree gchar *expected = NULL;

	SKIP_WITHOUT_DB(fx);
	big = axis_test_compat_file_id(fx->t, fx->t->s.films_a, "Big (2001)/Big (2001).mkv");
	expected = g_strdup_printf("%" G_GINT64_FORMAT, big);
	serve_big(fx, "sess-converted-audio-1");
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.playing, ==, 1);
	g_assert_cmpuint(summary.recorded, ==, 1);
	g_assert_cmpuint(summary.matched, ==, 1);
	g_assert_cmpuint(summary.updated, ==, 0);

	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);
	file_id = query(fx, "SELECT file_id FROM compat_session");
	g_assert_cmpstr(file_id, ==, expected);
	/* One stream's evidence each: video passed through, audio converted. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 2);
	effect = query(fx, "SELECT effect FROM compat_evidence WHERE kind = 1");
	g_assert_cmpstr(effect, ==, "passthrough");
	g_clear_pointer(&effect, g_free);
	effect = query(fx, "SELECT effect || ' ' || codec || ' ' || out_codec FROM compat_evidence WHERE kind = 2");
	g_assert_cmpstr(effect, ==, "converted_codec eac3 mp3");
	/* A passed-through stream gives no container evidence. */
	container = query(fx, "SELECT container FROM compat_evidence WHERE kind = 1");
	g_assert_null(container);
	/* The probe's facts describe the film: Big is 1080p H.264. */
	codec = query(fx, "SELECT codec || ' ' || res_class FROM compat_evidence WHERE kind = 1");
	g_assert_cmpstr(codec, ==, "h264 1080p");

	/* The same session in the next polls: still one record. */
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.recorded, ==, 0);
	g_assert_cmpuint(summary.updated, ==, 1);
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.updated, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 2);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", "polls = 3"), ==, 1);
	/* The path was given with the session: the item was never asked for. */
	g_assert_cmpuint(axis_test_plex_count(fx->t->plex, "/library/metadata/501"), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(fx->t->plex));
}

/* Spec: Session recorded once (across a restart) */
static void
test_a_restart_does_not_duplicate_sessions(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	AxisCompat *second;

	SKIP_WITHOUT_DB(fx);
	serve_big(fx, "sess-restart-1");
	poll(fx, TRUE);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);

	/* A new object over the same database, as after a restart. */
	second = axis_compat_new(fx->t->lib->pool, fx->t->lib->storage, fx->t->settings, fx->t->estimator);
	g_assert_true(axis_compat_poll(second, TRUE, NULL, &summary, NULL));
	g_assert_cmpuint(summary.recorded, ==, 0);
	g_assert_cmpuint(summary.updated, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 2);
	axis_compat_unref(second);

	/* Another session of the same film is another record. */
	serve_big(fx, "sess-restart-2");
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.recorded, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 2);
}

/* The item's file is asked for once, when the session does not state it. */
static void
test_the_file_is_looked_up_once_when_the_session_does_not_say(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autofree gchar *text = axis_test_compat_fixture("session-converted-audio.json",
		"\"file\":\"/data/movies/Invented Film (2001)/Invented Film (2001).mkv\",", "");
	g_autofree gchar *item = axis_test_compat_fixture("item-film.json", "Invented Film (2001)", "Big (2001)");

	SKIP_WITHOUT_DB(fx);
	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, text, 0);
	axis_test_plex_route(fx->t->plex, "/library/metadata/501", -1, AXIS_TEST_PLEX_NORMAL, 200, item, 0);
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.recorded, ==, 1);
	g_assert_cmpuint(summary.matched, ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->t->plex, "/library/metadata/501"), ==, 1);
	poll(fx, TRUE);
	g_assert_cmpuint(axis_test_plex_count(fx->t->plex, "/library/metadata/501"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", "file_id IS NOT NULL"), ==, 1);
}

/* An unmatched session is kept as evidence about the device but left out
 * of the reports about files. */
static void
test_an_unmatched_session_is_evidence_but_not_a_file_report(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autoptr(GPtrArray) failures = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GError) error = NULL;
	const AxisCompatFailure *f;

	SKIP_WITHOUT_DB(fx);
	/* The film the fixture names is not in the index. */
	serve_session(fx, "session-converted-audio.json", NULL, NULL);
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.recorded, ==, 1);
	g_assert_cmpuint(summary.matched, ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", "file_id IS NULL"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 2);

	g_assert_true(axis_compat_report_failures(fx->t->compat, &failures, &files, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(failures->len, ==, 1);
	f = g_ptr_array_index(failures, 0);
	g_assert_cmpstr(f->client_id, ==, "client-android-1");
	g_assert_cmpstr(f->codec, ==, "eac3");
	g_assert_cmpstr(f->out_codec, ==, "mp3");
	g_assert_cmpint(f->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpuint(f->plays, ==, 1);
	g_assert_cmpuint(f->files, ==, 0);
	g_assert_cmpuint(files->len, ==, 0);
}

/* Spec: Current failures */
static void
test_what_fails_today_lists_device_codec_reason_and_plays(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) failures = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *size = NULL;
	const AxisCompatFailure *f;
	const AxisCompatFailedFile *file;

	SKIP_WITHOUT_DB(fx);
	size = axis_test_db_queryf(fx->t->lib->db, "SELECT size FROM library_file WHERE id = %" G_GINT64_FORMAT,
		axis_test_compat_file_id(fx->t, fx->t->s.films_a, "Big (2001)/Big (2001).mkv"));
	/* Three plays of the same film that the device could not take audio of. */
	serve_big(fx, "sess-fail-1");
	poll(fx, TRUE);
	serve_big(fx, "sess-fail-2");
	poll(fx, TRUE);
	serve_big(fx, "sess-fail-3");
	poll(fx, TRUE);

	g_assert_true(axis_compat_report_failures(fx->t->compat, &failures, &files, &error));
	g_assert_no_error(error);
	/* The passed-through video is not a failure; the audio is. */
	g_assert_cmpuint(failures->len, ==, 1);
	f = g_ptr_array_index(failures, 0);
	g_assert_cmpstr(f->client_id, ==, "client-android-1");
	g_assert_cmpstr(f->device_name, ==, "Pixel");
	g_assert_cmpint(f->kind, ==, AXIS_COMPAT_STREAM_AUDIO);
	g_assert_cmpstr(f->codec, ==, "eac3");
	g_assert_cmpint(f->effect, ==, AXIS_EFFECT_CONVERTED_CODEC);
	g_assert_cmpuint(f->plays, ==, 3);
	g_assert_cmpuint(f->remote_plays, ==, 3);
	g_assert_cmpuint(f->files, ==, 1);
	g_assert_cmpint(f->bytes, ==, g_ascii_strtoll(size, NULL, 10));
	g_assert_cmpuint(files->len, ==, 1);
	file = g_ptr_array_index(files, 0);
	g_assert_cmpstr(file->rel_path, ==, "Big (2001)/Big (2001).mkv");
	g_assert_cmpuint(file->plays, ==, 3);
}

/* Shapes the daemon has not seen, stored as the classification says. */
static void
test_constructed_shapes_are_stored_as_classified(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *fixture;
		const gchar *kind;
		const gchar *expected;
	} table[] = {
		/* Spec: Observed direct play */
		{ "constructed/session-direct.json", "1", "direct hevc mkv" },
		{ "constructed/session-direct.json", "2", "direct eac3 mkv" },
		/* Spec: Unknown decision value */
		{ "constructed/session-unknown-decision.json", "1", "unknown h264 -" },
		/* Spec: Unexplained conversion */
		{ "constructed/session-unclear.json", "2", "converted_unclear aac aac" },
		/* Spec: Bitrate-limited conversion */
		{ "constructed/session-bandwidth.json", "1", "converted_limit h264 h264" },
		{ "constructed/session-bandwidth-codec.json", "1", "converted_limit hevc h264" },
		{ "constructed/session-video-converted.json", "1", "converted_codec hevc h264" },
		{ "constructed/session-subtitle.json", "3", "converted_unclear pgs -" }
	};
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		g_autofree gchar *sql = NULL;
		g_autofree gchar *got = NULL;

		serve_session(fx, table[i].fixture, NULL, NULL);
		poll(fx, TRUE);
		sql = g_strdup_printf(
			"SELECT e.effect || ' ' || e.codec || ' ' || coalesce(e.container, coalesce(e.out_codec, '-'))"
			" FROM compat_evidence e JOIN compat_session s ON s.id = e.session_id"
			" WHERE e.kind = %s ORDER BY s.id DESC LIMIT 1", table[i].kind);
		got = query(fx, sql);
		g_assert_cmpstr(got, ==, table[i].expected);
	}
}

/* Spec: Names off by default */
static void
test_account_names_are_not_stored_unless_the_setting_is_on(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *account = NULL;
	g_autofree gchar *name = NULL;

	SKIP_WITHOUT_DB(fx);
	serve_big(fx, "sess-privacy-1");
	poll(fx, TRUE);
	account = query(fx, "SELECT account_id FROM compat_session");
	g_assert_cmpstr(account, ==, "2");
	name = query(fx, "SELECT account_name FROM compat_session");
	g_assert_null(name);
	/* The name is in no table at all. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session s", "to_jsonb(s)::text LIKE '%Bob Example%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_device d", "to_jsonb(d)::text LIKE '%Bob Example%'"), ==, 0);

	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_ACCOUNT_NAMES, "true", NULL));
	serve_big(fx, "sess-privacy-2");
	poll(fx, TRUE);
	g_clear_pointer(&name, g_free);
	name = query(fx, "SELECT account_name FROM compat_session WHERE session_key = 'sess-privacy-2'");
	g_assert_cmpstr(name, ==, "Bob Example");
}

/* The adaptive interval, and what a failing server does. */
static void
test_the_interval_adapts_and_failures_back_off(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delay = NULL;
	g_autofree gchar *idle = NULL;
	g_autofree gchar *message = NULL;

	SKIP_WITHOUT_DB(fx);
	/* Nothing playing: the next look is later, and again later. */
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.playing, ==, 0);
	delay = query(fx, "SELECT round(extract(epoch FROM next_poll_at - last_poll_at))::int FROM compat_state");
	g_assert_cmpstr(delay, ==, "60");
	idle = query(fx, "SELECT idle_polls FROM compat_state");
	g_assert_cmpstr(idle, ==, "1");
	/* Not due: nothing is asked. */
	axis_test_plex_reset_requests(fx->t->plex);
	summary = poll(fx, FALSE);
	g_assert_true(summary.skipped_poll);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->plex), ==, 0);
	poll(fx, TRUE);
	g_clear_pointer(&delay, g_free);
	delay = query(fx, "SELECT round(extract(epoch FROM next_poll_at - last_poll_at))::int FROM compat_state");
	g_assert_cmpstr(delay, ==, "120");
	/* The interval stops growing at the idle limit (5 minutes). */
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_state SET idle_polls = 12");
	poll(fx, TRUE);
	g_clear_pointer(&delay, g_free);
	delay = query(fx, "SELECT round(extract(epoch FROM next_poll_at - last_poll_at))::int FROM compat_state");
	g_assert_cmpstr(delay, ==, "300");

	/* Something playing: the short interval again. */
	serve_big(fx, "sess-adaptive-1");
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.playing, ==, 1);
	g_clear_pointer(&delay, g_free);
	delay = query(fx, "SELECT round(extract(epoch FROM next_poll_at - last_poll_at))::int FROM compat_state");
	g_assert_cmpstr(delay, ==, "30");
	g_clear_pointer(&idle, g_free);
	idle = query(fx, "SELECT idle_polls FROM compat_state");
	g_assert_cmpstr(idle, ==, "0");

	/* A server that fails: a cleaned message, and a longer wait each time. */
	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 500, "{\"error\":\"" AXIS_TEST_COMPAT_TOKEN "\"}", 0);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, AXIS_TEST_COMPAT_TOKEN));
	g_assert_null(strstr(error->message, "error"));
	g_clear_error(&error);
	g_clear_pointer(&delay, g_free);
	delay = query(fx, "SELECT round(extract(epoch FROM next_poll_at - last_poll_at))::int FROM compat_state");
	g_assert_cmpstr(delay, ==, "30");
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_clear_error(&error);
	g_clear_pointer(&delay, g_free);
	delay = query(fx, "SELECT round(extract(epoch FROM next_poll_at - last_poll_at))::int FROM compat_state");
	g_assert_cmpstr(delay, ==, "60");
	message = query(fx, "SELECT last_error FROM compat_state");
	g_assert_nonnull(strstr(message, "500"));
	g_assert_null(strstr(message, AXIS_TEST_COMPAT_TOKEN));
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "failures = 2"), ==, 1);

	/* The server is back: the failures are forgotten. */
	axis_test_plex_clear_routes(fx->t->plex);
	axis_test_plex_standard(fx->t->plex);
	poll(fx, TRUE);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "failures = 0 AND last_error IS NULL"), ==, 1);
}

/* Spec: Bad responses are survivable (the poll's side) */
static void
test_a_lying_server_does_not_break_the_poll(Fixture *fx, gconstpointer data)
{
	static const gchar *const lies[] = {
		"{\"MediaContainer\":{\"Metadata\":[{\"ratingKey\":\"1\"}]}}",
		"{\"MediaContainer\":{\"Metadata\":[7,null,\"x\",{}]}}",
		"{\"MediaContainer\":{\"Metadata\":[{\"ratingKey\":\"1\",\"Player\":{\"machineIdentifier\":\"d\"},"
		"\"Session\":{\"id\":\"s\"},\"Media\":[{},{}]}]}}",
		NULL
	};
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; lies[i] != NULL; i++) {
		axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, lies[i], 0);
		g_assert_true(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
		g_assert_no_error(error);
	}
	/* A session that lists several media is recorded, without evidence. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 0);
	/* The answer is not JSON: the poll fails, softly. */
	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, "<<<not json>>>", 0);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_SERVER);
	g_assert_nonnull(strstr(error->message, "could not be used"));
}

/* Spec: Bad responses are survivable (too large and too slow, for the poll) */
static void
test_oversized_and_silent_answers_fail_the_poll_softly(Fixture *fx, gconstpointer data)
{
	AxisCompatTuning tuning;
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 700;
	tuning.max_body_bytes = 64 * 1024;
	axis_compat_set_tuning(fx->t->compat, &tuning);

	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_OVERSIZE, 200, NULL, 1024 * 1024);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_nonnull(strstr(error->message, "larger than the limit"));
	g_clear_error(&error);

	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_nonnull(strstr(error->message, "in time"));
	g_clear_error(&error);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "failures = 2"), ==, 1);
}

/* A server that invents a session at every look cannot fill the table. */
static void
test_the_session_table_has_a_limit(Fixture *fx, gconstpointer data)
{
	AxisCompatTuning tuning;
	AxisCompatPollSummary summary;

	SKIP_WITHOUT_DB(fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.max_session_rows = 2;
	axis_compat_set_tuning(fx->t->compat, &tuning);
	serve_big(fx, "sess-limit-1");
	poll(fx, TRUE);
	serve_big(fx, "sess-limit-2");
	poll(fx, TRUE);
	/* Full: a third is not recorded... */
	serve_big(fx, "sess-limit-3");
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.recorded, ==, 0);
	g_assert_cmpuint(summary.skipped, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 2);
	/* ...but one already stored still is updated. */
	serve_big(fx, "sess-limit-2");
	summary = poll(fx, TRUE);
	g_assert_cmpuint(summary.updated, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", "polls = 2"), ==, 1);
}

/* ── History ────────────────────────────────────────────────────── */

/* Spec: History has no decisions */
static void
test_history_says_who_played_which_library_and_adds_no_evidence(Fixture *fx, gconstpointer data)
{
	AxisCompatHistorySummary summary;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *plex_id = NULL;
	g_autofree gchar *name = NULL;

	SKIP_WITHOUT_DB(fx);
	g_assert_true(axis_compat_import_history(fx->t->compat, FALSE, NULL, &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(summary.pages, ==, 2);
	g_assert_cmpuint(summary.seen, ==, 5);
	/* The play older than the retention (500 days) is not kept. */
	g_assert_cmpuint(summary.plays, ==, 4);
	g_assert_true(summary.complete);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play", NULL), ==, 4);
	/* No decision, no evidence, no session. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 0);
	/* Devices are created for what played; the unlisted one has a stand-in. */
	plex_id = query(fx, "SELECT plex_id FROM compat_device WHERE client_id = 'client-tizen-1'");
	g_assert_cmpstr(plex_id, ==, "11");
	name = query(fx, "SELECT name FROM compat_device WHERE client_id = 'client-tizen-1'");
	g_assert_cmpstr(name, ==, "Living room TV");
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_device", "client_id = 'plex:99'"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_device", "client_id = 'client-roku-1'"), ==, 0);
	/* Account ids are opaque numbers; no name is stored. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play", "account_id IN ('1', '2')"), ==, 4);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play p", "to_jsonb(p)::text LIKE '%Example%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "history_at IS NOT NULL AND history_error IS NULL"), ==, 1);
	g_assert_false(axis_test_plex_saw_non_get(fx->t->plex));

	/* Again: the day's import is done, and a forced one adds nothing. */
	g_assert_true(axis_compat_import_history(fx->t->compat, FALSE, NULL, &summary, &error));
	g_assert_true(summary.skipped);
	g_assert_true(axis_compat_import_history(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_cmpuint(summary.plays, ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play", NULL), ==, 4);
}

/* ── Retention ──────────────────────────────────────────────────── */

/* Spec: Retention */
static void
test_retention_removes_what_is_older_than_the_period(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint sessions = 0;
	guint plays = 0;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_evidence(fx->t, "old-dev", "1", 1, "hevc", "main 10", 10, "1080p", 0, "mkv", "direct", FALSE, 500);
	axis_test_compat_evidence(fx->t, "new-dev", "1", 1, "hevc", "main 10", 10, "1080p", 0, "mkv", "direct", FALSE, 10);
	axis_test_compat_evidence(fx->t, "pinned-dev", "1", 1, "hevc", "main 10", 10, "1080p", 0, "mkv", "direct", FALSE, 500);
	axis_test_compat_play(fx->t, "old-dev", "1", 500);
	axis_test_compat_play(fx->t, "new-dev", "1", 10);
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_device SET last_seen = now() - interval '500 days'"
		" WHERE client_id IN ('old-dev', 'pinned-dev')");
	g_assert_true(axis_compat_set_pinned(fx->t->compat, "pinned-dev", TRUE, &error));

	g_assert_true(axis_compat_apply_retention(fx->t->compat, &sessions, &plays, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(sessions, ==, 2);
	g_assert_cmpuint(plays, ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_evidence", NULL), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play", NULL), ==, 1);
	/* A device nothing refers to any more goes, unless it is pinned. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_device", "client_id = 'old-dev'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_device", "client_id = 'pinned-dev'"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_device", "client_id = 'new-dev'"), ==, 1);

	/* A shorter retention is honoured. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_SETTING_RETENTION, "7d", NULL));
	g_assert_true(axis_compat_apply_retention(fx->t->compat, &sessions, &plays, &error));
	g_assert_cmpuint(sessions, ==, 1);
	g_assert_cmpuint(plays, ==, 1);
}

/* ── Libraries and verdicts ─────────────────────────────────────── */

/* The libraries come from the server; a poll fetches them. */
static void
fetch_libraries(Fixture *fx)
{
	poll(fx, TRUE);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_library", NULL), ==, 3);
}

/* Spec: Mapped through the path map / Unmapped library */
static void
test_libraries_map_through_the_path_map_and_unmapped_ones_are_excluded(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	g_autofree gchar *shows = NULL;
	const AxisCompatLibraryReport *films;
	const AxisCompatLibraryReport *series;
	const AxisCompatLibraryReport *music;

	SKIP_WITHOUT_DB(fx);
	fetch_libraries(fx);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	series = axis_test_compat_library(report, "2");
	music = axis_test_compat_library(report, "3");

	g_assert_cmpint(films->state, ==, AXIS_MAP_MAPPED);
	g_assert_cmpuint(films->root_ids->len, ==, 1);
	g_assert_cmpint(g_array_index(films->root_ids, gint64, 0), ==, fx->t->s.films_a);
	g_assert_cmpstr(g_ptr_array_index(films->root_names, 0), ==, "Films");
	g_assert_cmpint(music->state, ==, AXIS_MAP_MAPPED);
	/* One of the show library's two folders reaches no root. */
	g_assert_cmpint(series->state, ==, AXIS_MAP_PARTLY);
	g_assert_cmpuint(series->unmapped_folders->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(series->unmapped_folders, 0), ==, "/data/tv-extra");
	/* Excluded from verdicts: it has no target at all. */
	g_assert_cmpuint(series->targets->len, ==, 0);
	g_assert_cmpint(series->recommended, ==, -1);

	/* Mapping the folder makes it a library with verdicts. */
	shows = g_build_filename(fx->t->s.disk_a, "Shows", NULL);
	axis_test_compat_map(fx->t, "/data/tv-extra", shows);
	g_clear_pointer(&report, axis_compat_report_free);
	report = axis_test_compat_report(fx->t);
	series = axis_test_compat_library(report, "2");
	g_assert_cmpint(series->state, ==, AXIS_MAP_MAPPED);
	g_assert_cmpuint(series->targets->len, >, 0);
	g_assert_nonnull(axis_test_compat_target(series, "space_saver"));
}

/* Nothing is mapped without the administrator's map. */
static void
test_without_a_path_map_nothing_is_mapped_and_a_suggestion_is_offered(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	g_autoptr(GPtrArray) map = NULL;
	const AxisCompatLibraryReport *music;
	const AxisCompatSuggestion *s;

	SKIP_WITHOUT_DB(fx);
	/* The standard map is removed again. */
	g_assert_true(axis_compat_list_path_map(fx->t->compat, &map, NULL));
	{
		guint i;

		for (i = 0; i < map->len; i++)
			g_assert_true(axis_compat_path_map_remove(fx->t->compat, ((AxisCompatPathEntry *)g_ptr_array_index(map, i))->id, NULL));
	}
	fetch_libraries(fx);
	report = axis_test_compat_report(fx->t);
	music = axis_test_compat_library(report, "3");
	g_assert_cmpint(music->state, ==, AXIS_MAP_UNMAPPED);
	g_assert_cmpuint(music->targets->len, ==, 0);
	/* "/data/music" ends like the Music root: a suggestion, not a mapping. */
	g_assert_cmpuint(music->suggestions->len, ==, 1);
	s = g_ptr_array_index(music->suggestions, 0);
	g_assert_cmpstr(s->folder, ==, "/data/music");
	g_assert_cmpint(s->root_id, ==, fx->t->s.music_a);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_path_map", NULL), ==, 0);
}

static void
test_path_map_entries_are_checked(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *problem = NULL;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	g_assert_false(axis_compat_path_map_add(fx->t->compat, "data/movies", "/media/x", &problem, &error));
	g_assert_nonnull(problem);
	g_clear_error(&error);
	g_clear_pointer(&problem, g_free);
	g_assert_false(axis_compat_path_map_add(fx->t->compat, "/data/x", "/media/../x", &problem, &error));
	g_assert_nonnull(problem);
	g_clear_error(&error);
	g_clear_pointer(&problem, g_free);
	/* The same media server path twice. */
	g_assert_false(axis_compat_path_map_add(fx->t->compat, "/data/movies", "/media/other", &problem, &error));
	g_assert_nonnull(strstr(problem, "already mapped"));
}

/* Spec: Safe, Risky, Unknown is never safe, No evidence, Pinned device */
static void
test_verdicts_follow_the_evidence_of_the_devices_that_played(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	const AxisCompatLibraryReport *films;
	const AxisCompatTargetVerdict *tv;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	fetch_libraries(fx);

	/* No play of the library recorded: unknown for every target. */
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpuint(films->n_devices, ==, 0);
	g_assert_cmpuint(films->targets->len, >, 0);
	{
		guint i;

		for (i = 0; i < films->targets->len; i++)
			g_assert_cmpint(((AxisCompatTargetVerdict *)g_ptr_array_index(films->targets, i))->verdict, ==,
				AXIS_VERDICT_UNKNOWN);
	}
	g_assert_cmpint(films->recommended, ==, -1);
	/* The library has files that were never probed: the hardest case is assumed. */
	g_assert_false(films->probed);
	g_assert_cmpstr(films->res_class, ==, "2160p");
	g_clear_pointer(&report, axis_compat_report_free);

	/* A device that played the library yesterday and has been seen playing
	 * HEVC Main 10 at 2160p, SDR and HDR, in Matroska. */
	axis_test_compat_device(fx->t, "tv", "Living room TV");
	axis_test_compat_play(fx->t, "tv", "1", 1);
	axis_test_compat_evidence(fx->t, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 0, "mkv", "direct", FALSE, 1);
	axis_test_compat_evidence(fx->t, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 1, "mkv", "direct", FALSE, 1);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpuint(films->n_devices, ==, 1);
	tv = axis_test_compat_target(films, "space_saver");
	g_assert_cmpint(tv->verdict, ==, AXIS_VERDICT_SAFE);
	g_assert_cmpuint(tv->n_safe, ==, 1);
	/* The Balanced target wants HEVC Main (8-bit): only Main 10 was seen, and
	 * nothing but the same profile is taken to imply it. */
	g_assert_cmpint(axis_test_compat_target(films, "balanced")->verdict, ==, AXIS_VERDICT_UNKNOWN);
	/* H.264 was never seen. */
	g_assert_cmpint(axis_test_compat_target(films, "max_compat")->verdict, ==, AXIS_VERDICT_UNKNOWN);
	g_clear_pointer(&report, axis_compat_report_free);

	/* Spec: a device that played the library only before the window does not count... */
	axis_test_compat_device(fx->t, "tablet", "Old tablet");
	axis_test_compat_play(fx->t, "tablet", "1", 200);
	axis_test_compat_evidence(fx->t, "tablet", "1", 1, "hevc", "main 10", 10, "1080p", 0, NULL, "converted_codec", FALSE, 200);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpuint(films->n_devices, ==, 1);
	g_assert_cmpint(axis_test_compat_target(films, "space_saver")->verdict, ==, AXIS_VERDICT_SAFE);
	g_clear_pointer(&report, axis_compat_report_free);

	/* ...unless it is marked always-consider: it was seen having HEVC converted. */
	g_assert_true(axis_compat_set_pinned(fx->t->compat, "tablet", TRUE, &error));
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpuint(films->n_devices, ==, 2);
	tv = axis_test_compat_target(films, "space_saver");
	g_assert_cmpint(tv->verdict, ==, AXIS_VERDICT_RISKY);
	g_assert_cmpuint(tv->n_risky, ==, 1);
	{
		const AxisCompatDeviceVerdict *first = g_ptr_array_index(tv->devices, 0);

		/* The risky device is first, named, with the reason. */
		g_assert_cmpstr(first->client_id, ==, "tablet");
		g_assert_true(first->pinned);
		g_assert_cmpint(first->verdict, ==, AXIS_VERDICT_RISKY);
		g_assert_nonnull(strstr(first->detail, "converted"));
	}
	g_clear_pointer(&report, axis_compat_report_free);

	/* A device with only conversions that a bandwidth limit or an unclear
	 * cause explain is unknown, never risky and never safe. */
	g_assert_true(axis_compat_set_pinned(fx->t->compat, "tablet", FALSE, &error));
	axis_test_db_exec(fx->t->lib->db, "DELETE FROM compat_device WHERE client_id = 'tablet'");
	axis_test_compat_device(fx->t, "phone", "Phone");
	axis_test_compat_play(fx->t, "phone", "1", 3);
	axis_test_compat_evidence(fx->t, "phone", "1", 1, "hevc", "main 10", 10, "1080p", 0, NULL, "converted_limit", TRUE, 3);
	axis_test_compat_evidence(fx->t, "phone", "1", 1, "hevc", "main 10", 10, "1080p", 0, NULL, "converted_unclear", TRUE, 3);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	tv = axis_test_compat_target(films, "space_saver");
	g_assert_cmpint(tv->verdict, ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpuint(tv->n_unknown, ==, 1);
	g_assert_cmpuint(tv->n_risky, ==, 0);
	/* Unknown comes before safe in the list of devices. */
	g_assert_cmpstr(((AxisCompatDeviceVerdict *)g_ptr_array_index(tv->devices, 0))->client_id, ==, "phone");
	g_clear_pointer(&report, axis_compat_report_free);
}

/* Spec: No evidence (a library nobody has played) / Unknown is never safe */
static void
test_a_library_nobody_played_is_unknown_for_every_target(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	const AxisCompatLibraryReport *music;
	guint i;

	SKIP_WITHOUT_DB(fx);
	fetch_libraries(fx);
	report = axis_test_compat_report(fx->t);
	music = axis_test_compat_library(report, "3");
	g_assert_cmpuint(music->n_devices, ==, 0);
	g_assert_cmpuint(music->targets->len, >, 0);
	for (i = 0; i < music->targets->len; i++) {
		const AxisCompatTargetVerdict *tv = g_ptr_array_index(music->targets, i);

		g_assert_cmpint(tv->target.media, ==, AXIS_TARGET_AUDIO);
		g_assert_cmpint(tv->verdict, ==, AXIS_VERDICT_UNKNOWN);
	}
	g_assert_cmpint(music->recommended, ==, -1);
}

/* Spec: Largest safe saving / Nothing safe */
static void
test_the_recommendation_is_the_largest_safe_saving(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	const AxisCompatLibraryReport *films;
	const AxisCompatTargetVerdict *tv;

	SKIP_WITHOUT_DB(fx);
	fetch_libraries(fx);
	/* The TV has been seen playing everything: Space saver and Balanced are safe. */
	axis_test_compat_play(fx->t, "tv", "1", 1);
	axis_test_compat_evidence(fx->t, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 0, "mkv", "direct", FALSE, 1);
	axis_test_compat_evidence(fx->t, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 1, "mkv", "direct", FALSE, 1);
	axis_test_compat_evidence(fx->t, "tv", "1", 1, "hevc", "main", 8, "2160p", 0, "mkv", "direct", FALSE, 1);

	/* Without estimates nothing is recommended, however safe. */
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_false(report->have_estimates);
	g_assert_cmpint(axis_test_compat_target(films, "space_saver")->verdict, ==, AXIS_VERDICT_SAFE);
	g_assert_cmpint(axis_test_compat_target(films, "balanced")->verdict, ==, AXIS_VERDICT_SAFE);
	g_assert_cmpint(films->recommended, ==, -1);
	g_clear_pointer(&report, axis_compat_report_free);
	report = NULL;

	axis_test_compat_fact(fx->t, "space_saver", fx->t->s.films_a, 10000, 4000);
	axis_test_compat_fact(fx->t, "balanced", fx->t->s.films_a, 10000, 5000);
	axis_test_compat_fact(fx->t, "max_compat", fx->t->s.films_a, 10000, 9000);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_true(report->have_estimates);
	tv = axis_test_compat_target(films, "space_saver");
	g_assert_cmpint(tv->saving, ==, 6000);
	g_assert_cmpint(axis_test_compat_target(films, "balanced")->saving, ==, 5000);
	/* Two targets are safe: the larger saving wins. */
	g_assert_cmpint(films->recommended, >=, 0);
	g_assert_cmpstr(((AxisCompatTargetVerdict *)g_ptr_array_index(films->targets, (guint)films->recommended))->target.id, ==,
		"space_saver");
	g_clear_pointer(&report, axis_compat_report_free);

	/* A second device cannot be shown to play Main 10 SDR: Space saver is
	 * unknown, Balanced (which it has been seen playing) is recommended. */
	axis_test_compat_play(fx->t, "box", "1", 2);
	axis_test_compat_evidence(fx->t, "box", "1", 1, "hevc", "main", 8, "2160p", 0, "mkv", "direct", FALSE, 2);
	axis_test_compat_evidence(fx->t, "box", "1", 1, "hevc", "main 10", 10, "2160p", 1, "mkv", "direct", FALSE, 2);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpint(axis_test_compat_target(films, "space_saver")->verdict, ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(axis_test_compat_target(films, "balanced")->verdict, ==, AXIS_VERDICT_SAFE);
	g_assert_cmpstr(((AxisCompatTargetVerdict *)g_ptr_array_index(films->targets, (guint)films->recommended))->target.id, ==,
		"balanced");
	/* The target with the largest saving is the one held back, and the next
	 * is the one recommended. */
	g_assert_cmpstr(((AxisCompatTargetVerdict *)g_ptr_array_index(films->targets, (guint)films->best))->target.id, ==,
		"space_saver");
	g_clear_pointer(&report, axis_compat_report_free);

	/* A device nobody knows anything about: nothing is safe, nothing is recommended. */
	axis_test_compat_play(fx->t, "stranger", "1", 2);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpint(axis_test_compat_target(films, "balanced")->verdict, ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(films->recommended, ==, -1);
	g_assert_cmpstr(((AxisCompatTargetVerdict *)g_ptr_array_index(films->targets, (guint)films->best))->target.id, ==,
		"space_saver");
	/* The devices that block it are named, the unknown ones among them. */
	tv = g_ptr_array_index(films->targets, (guint)films->best);
	g_assert_cmpuint(tv->n_unknown, ==, 2);
	g_assert_cmpint(films->next, >=, 0);
}

/* The verdict of space_saver for the Films library now. */
static AxisCompatVerdict
films_space_saver(Fixture *fx)
{
	g_autoptr(AxisCompatReport) report = axis_test_compat_report(fx->t);
	const AxisCompatLibraryReport *films = axis_test_compat_library(report, "1");

	return axis_test_compat_target(films, "space_saver")->verdict;
}

/* The record of a done replacement of the Films root's @rel by Space saver,
 * from a source of @source_hdr, the file now being its output; and that
 * file's probe as the output's (10 bits, no transfer function). */
static void
replaced(
	Fixture     *fx,
	const gchar *rel,
	const gchar *source_hdr
){
	g_autofree gchar *revision = axis_test_db_query(fx->t->lib->db,
		"SELECT current_revision_id FROM conversion_profile WHERE slug = 'space_saver'");
	g_autofree gchar *record = g_strdup_printf("INSERT INTO conversion (file_id, volume_id, root_id, root_path, rel_path,"
		" new_rel_path, container, revision_id, outcome, state, requested_by, verdict, src_size, src_mtime_sec,"
		" src_mtime_nsec, src_ctime_sec, src_ctime_nsec, src_inode, src_nlink, output_inode, output_size,"
		" output_mtime_sec, output_mtime_nsec, source_hdr, made_hdr, finished_at) SELECT f.id, f.volume_id, f.root_id,"
		" 'Films', f.rel_path, f.rel_path, 'mkv', %s, 'replace', 'done', 'alice', 'safe', f.size * 3, f.mtime_sec,"
		" f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode + 100000, 1, f.inode, f.size, f.mtime_sec, f.mtime_nsec, '%s',"
		" 'sdr', now() FROM library_file f WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path = '%s'", revision,
		source_hdr, fx->t->s.films_a, rel);

	axis_test_db_exec(fx->t->lib->db, record);
}

static void
probed_as_output(
	Fixture     *fx,
	const gchar *rel
){
	g_autofree gchar *sql = g_strdup_printf("UPDATE media_info SET video_codec = 'hevc', bit_depth = 10,"
		" hdr_format = NULL, hdr_uncertain = true WHERE file_id = %" G_GINT64_FORMAT,
		axis_test_compat_file_id(fx->t, fx->t->s.films_a, rel));

	axis_test_db_exec(fx->t->lib->db, sql);
}

/*
 * Spec (client-compatibility): A file Axis made from an SDR source is SDR.
 * A library of SDR files whose players are seen to direct-play HEVC Main 10
 * SDR (and no HDR) is safe for Space saver.  Space saver's replacement of an
 * untagged SDR source is 10 bits without a transfer function, which the
 * probe calls HDR uncertain: without what Axis recorded of it, the library
 * would count as possibly HDR and the verdict turn unknown after its first
 * replacement, refusing every later one.  With the record (made from an SDR
 * source the probe was certain of, a plan that kept SDR, done, the file
 * still that output) it stays safe; a file Axis did not make, a source that
 * was not certainly SDR, or the file replaced since by something else, keeps
 * the probe's caution.
 */
static void
test_a_file_axis_made_from_sdr_stays_sdr(Fixture *fx, gconstpointer data)
{
	AxisTestDb *db;
	g_autofree gchar *sql = NULL;

	SKIP_WITHOUT_DB(fx);
	db = fx->t->lib->db;
	fetch_libraries(fx);
	/* The Films root with its SDR files only, all of them probed. */
	sql = g_strdup_printf("DELETE FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND id NOT IN (SELECT file_id"
		" FROM media_info WHERE status = 'ok' AND hdr_format = 'sdr' AND NOT hdr_uncertain)", fx->t->s.films_a);
	axis_test_db_exec(db, sql);
	axis_test_compat_device(fx->t, "tv", "Living room TV");
	axis_test_compat_play(fx->t, "tv", "1", 1);
	axis_test_compat_evidence(fx->t, "tv", "1", 1, "hevc", "main 10", 10, "1080p", 0, "mkv", "direct", FALSE, 1);
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_SAFE);

	/* Big (2001) replaced by Space saver: its probe is now HDR uncertain.
	 * Without a record, a file of that kind Axis did not make: the probe's
	 * caution, unknown. */
	probed_as_output(fx, "Big (2001)/Big (2001).mkv");
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_UNKNOWN);
	/* From a source that was not certainly SDR: not known either. */
	replaced(fx, "Big (2001)/Big (2001).mkv", "unclassified");
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_UNKNOWN);
	axis_test_db_exec(db, "DELETE FROM conversion");
	/* What Axis recorded of the replacement it made. */
	replaced(fx, "Big (2001)/Big (2001).mkv", "sdr");
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_SAFE);
	/* A run of replacements does not talk itself out of permission: a
	 * second file replaced the same way, still safe. */
	probed_as_output(fx, "Remux (2002)/Remux (2002).mkv");
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_UNKNOWN);
	replaced(fx, "Remux (2002)/Remux (2002).mkv", "sdr");
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_SAFE);

	/* Big (2001) replaced since by something else (an upgrade's import): it
	 * is no longer what Axis made, and the caution is back. */
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE library_file SET inode = inode + 7 WHERE id = %" G_GINT64_FORMAT,
		axis_test_compat_file_id(fx->t, fx->t->s.films_a, "Big (2001)/Big (2001).mkv"));
	axis_test_db_exec(db, sql);
	g_assert_cmpint(films_space_saver(fx), ==, AXIS_VERDICT_UNKNOWN);
}

/* Library size is reported per root, so a library's saving adds its roots'. */
static void
test_a_library_with_two_roots_adds_their_savings(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	g_autofree gchar *films2 = NULL;
	const AxisCompatLibraryReport *films;

	SKIP_WITHOUT_DB(fx);
	films2 = g_build_filename(fx->t->s.disk_b, "Films2", NULL);
	/* A second folder of the movie library on the other disk. */
	axis_test_plex_route(fx->t->plex, "/library/sections", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"{\"MediaContainer\":{\"Directory\":[{\"key\":\"1\",\"type\":\"movie\",\"title\":\"Films\","
		"\"Location\":[{\"path\":\"/data/movies\"},{\"path\":\"/data/movies2\"}]}]}}", 0);
	axis_test_compat_map(fx->t, "/data/movies2", films2);
	poll(fx, TRUE);
	axis_test_compat_fact(fx->t, "space_saver", fx->t->s.films_a, 10000, 4000);
	axis_test_compat_fact(fx->t, "space_saver", fx->t->s.films_b, 2000, 500);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "1");
	g_assert_cmpint(films->state, ==, AXIS_MAP_MAPPED);
	g_assert_cmpuint(films->root_ids->len, ==, 2);
	g_assert_cmpint(axis_test_compat_target(films, "space_saver")->saving, ==, 6000 + 1500);
}

/* The server changes: what was recorded about the old one is removed. */
static void
test_another_server_starts_the_records_again(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);
	serve_big(fx, "sess-old-server");
	poll(fx, TRUE);
	axis_test_compat_play(fx->t, "x", "1", 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 1);
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"{\"MediaContainer\":{\"machineIdentifier\":\"another-server-id\",\"version\":\"1\"}}", 0);
	axis_test_plex_route(fx->t->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"{\"MediaContainer\":{\"size\":0}}", 0);
	poll(fx, TRUE);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_session", NULL), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play", NULL), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "server_id = 'another-server-id'"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*the media server is not the one*"), ==, 1);
}

/* An unconfigured connection does nothing at all. */
static void
test_without_a_connection_nothing_is_asked(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	AxisCompatHistorySummary history;

	SKIP_WITHOUT_DB(fx);
	g_assert_true(axis_settings_clear(fx->t->settings, AXIS_COMPAT_SETTING_ADDRESS, NULL));
	summary = poll(fx, TRUE);
	g_assert_true(summary.skipped_poll);
	g_assert_true(axis_compat_import_history(fx->t->compat, TRUE, NULL, &history, NULL));
	g_assert_true(history.skipped);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->plex), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(name, library, func) \
	g_test_add("/compat-db/" name, Fixture, GINT_TO_POINTER(library), fixture_setup, func, fixture_teardown)
	ADD("settings", 0, test_settings_validate_the_address_and_keep_the_token_secret);
	ADD("connection-test", 0, test_connection_test_reports_name_and_version_or_the_reason);
	ADD("no-connection", 0, test_without_a_connection_nothing_is_asked);
	ADD("session/once", 1, test_a_session_is_recorded_once_and_matched_through_the_path_map);
	ADD("session/restart", 1, test_a_restart_does_not_duplicate_sessions);
	ADD("session/item-lookup", 1, test_the_file_is_looked_up_once_when_the_session_does_not_say);
	ADD("session/unmatched", 0, test_an_unmatched_session_is_evidence_but_not_a_file_report);
	ADD("session/failures", 1, test_what_fails_today_lists_device_codec_reason_and_plays);
	ADD("session/constructed", 0, test_constructed_shapes_are_stored_as_classified);
	ADD("session/privacy", 1, test_account_names_are_not_stored_unless_the_setting_is_on);
	ADD("poll/interval", 1, test_the_interval_adapts_and_failures_back_off);
	ADD("poll/lies", 0, test_a_lying_server_does_not_break_the_poll);
	ADD("poll/too-big-or-slow", 0, test_oversized_and_silent_answers_fail_the_poll_softly);
	ADD("poll/session-limit", 0, test_the_session_table_has_a_limit);
	ADD("history", 0, test_history_says_who_played_which_library_and_adds_no_evidence);
	ADD("retention", 0, test_retention_removes_what_is_older_than_the_period);
	ADD("mapping/path-map", 1, test_libraries_map_through_the_path_map_and_unmapped_ones_are_excluded);
	ADD("mapping/suggestion", 1, test_without_a_path_map_nothing_is_mapped_and_a_suggestion_is_offered);
	ADD("mapping/entries", 1, test_path_map_entries_are_checked);
	ADD("verdicts/evidence", 1, test_verdicts_follow_the_evidence_of_the_devices_that_played);
	ADD("verdicts/no-plays", 1, test_a_library_nobody_played_is_unknown_for_every_target);
	ADD("verdicts/recommendation", 1, test_the_recommendation_is_the_largest_safe_saving);
	ADD("verdicts/two-roots", 1, test_a_library_with_two_roots_adds_their_savings);
	ADD("verdicts/made-by-axis", 1, test_a_file_axis_made_from_sdr_stays_sdr);
	ADD("server/change", 1, test_another_server_starts_the_records_again);
#undef ADD
	return g_test_run();
}
