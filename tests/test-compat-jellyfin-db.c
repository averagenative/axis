/*
 * test-compat-jellyfin-db.c - Jellyfin as a second kind of media server: its
 * collection into the tables, the secret, the two servers at once, a server
 * with no play history, and the migration that made room for it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The fake server is the one of axis-test-plex.h serving the answers a real
 * Jellyfin 10.11.8 gave (tests/fixtures/compat/jellyfin/recorded); a second
 * instance stands for Plex where both are connected.
 *
 * Spec scenarios of client-compatibility covered here: Connection test,
 * Secret stays secret, Read-only, Verdict across servers, One server down,
 * Stated codec reason, Stated bandwidth reason, Unknown reason, Container-only
 * failure, Not yet seen.
 */

#include <glib.h>
#include <string.h>
#include <time.h>

#include "axis-test-compat.h"
#include "axis-test-jobs.h"
#include "axis-test-case.h"
#include "db/axis-db-error.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"

#define F_LIBRARY 1
#define F_PLEX 2

typedef struct
{
	AxisTestCompat *t;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	gint flags = GPOINTER_TO_INT(data);

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the media server is not the one*");
	fx->t = axis_test_compat_new((flags & F_LIBRARY) != 0);
	if (fx->t == NULL)
		return;
	axis_test_compat_enable_jellyfin(fx->t);
	axis_test_compat_connect_jellyfin(fx->t);
	if ((flags & F_PLEX) != 0)
		axis_test_compat_connect(fx->t);
	if ((flags & F_LIBRARY) != 0) {
		axis_test_compat_map_standard(fx->t);
		axis_test_compat_map_jellyfin_standard(fx->t);
	}
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_compat_free(fx->t);
	axis_test_log_reset();
}

static AxisCompatPollSummary
poll(Fixture *fx)
{
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_no_error(error);
	return summary;
}

static gint64
count(
	Fixture     *fx,
	const gchar *table,
	const gchar *where
){
	return axis_test_compat_count(fx->t, table, where);
}

static gchar *
query(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->t->lib->db, sql);
}

/* Serves @fixture (a file of the Jellyfin fixtures, with @from replaced by
 * @to when given) as the sessions. */
static void
serve_sessions(
	Fixture     *fx,
	const gchar *file,
	const gchar *from,
	const gchar *to
){
	g_autofree gchar *name = g_strconcat("jellyfin/", file, NULL);
	g_autofree gchar *text = axis_test_compat_fixture(name, from, to);

	axis_test_plex_route(fx->t->jellyfin, "/Sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, text, 0);
}

/* Nothing of the key (or the token) is in any table. */
static void
assert_secret_in_no_table(
	Fixture     *fx,
	const gchar *secret
){
	static const gchar *const tables[] = {
		"job", "compat_state", "compat_session", "compat_device", "compat_library", "compat_play", "compat_path_map",
		"compat_evidence", "setting", NULL
	};
	guint i;

	for (i = 0; tables[i] != NULL; i++) {
		g_autofree gchar *table = g_strconcat(tables[i], " x", NULL);
		g_autofree gchar *where = g_strdup_printf("to_jsonb(x)::text LIKE '%%%s%%'", secret);

		/* The settings table holds the secret settings themselves, by design
		 * (never given back); every other table must not. */
		if (g_strcmp0(tables[i], "setting") == 0)
			continue;
		g_assert_cmpint(count(fx, table, where), ==, 0);
	}
}

static void
assert_secret_not_logged(const gchar *secret)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	g_autofree gchar *pattern = g_strdup_printf("*%s*", secret);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		g_assert_cmpuint(axis_test_log_count(levels[i], pattern), ==, 0);
}

/* ── Collection ─────────────────────────────────────────────────── */

/* Spec: Session recorded once / Stated reasons are evidence */
static void
test_the_recorded_sessions_are_collected_once_with_their_evidence(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autofree gchar *stated = NULL;

	SKIP_WITHOUT_DB(fx);
	summary = poll(fx);
	/* Seven clients play (the eighth session plays nothing and is no play). */
	g_assert_cmpuint(summary.playing, ==, 7);
	g_assert_cmpuint(summary.recorded, ==, 7);
	g_assert_cmpuint(summary.skipped, ==, 0);
	g_assert_cmpint(count(fx, "compat_session", "server_kind = 'jellyfin'"), ==, 7);
	g_assert_cmpint(count(fx, "compat_session", "server_kind <> 'jellyfin'"), ==, 0);
	/* The libraries and the devices (the ones that never played too). */
	g_assert_cmpint(count(fx, "compat_library", "server_kind = 'jellyfin'"), ==, 2);
	g_assert_cmpint(count(fx, "compat_device", "server_kind = 'jellyfin'"), ==, 8);
	g_assert_cmpint(count(fx, "compat_device", "server_kind = 'jellyfin' AND client_id = 'axis-test-setup'"), ==, 1);
	g_assert_cmpint(count(fx, "compat_device", "name = 'Pixel Phone' AND product = 'AxisTestPhone'"), ==, 1);
	/* The library of an item is worked out from its path. */
	g_assert_cmpint(count(fx, "compat_session", "library_key = '" AXIS_TEST_JELLYFIN_FILMS "'"), ==, 6);
	g_assert_cmpint(count(fx, "compat_session", "library_key = '" AXIS_TEST_JELLYFIN_MUSIC "'"), ==, 1);

	/* Direct play, both streams, with the container. */
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-tv' AND e.effect = 'direct' AND e.container = 'mkv'"), ==, 2);
	/* The phone: codec and container each named, with the server's words. */
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-phone' AND e.kind = 1 AND e.failed_property = '' AND e.effect = 'converted_codec'"
		" AND e.codec = 'hevc' AND e.out_codec = 'h264' AND e.stated_reason = 'VideoCodecNotSupported'"), ==, 1);
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-phone' AND e.kind = 1 AND e.failed_property = 'container' AND e.container = 'mkv'"
		" AND e.stated_reason = 'ContainerNotSupported'"), ==, 1);
	/* The tablet: the bit depth is blamed and the codec is not. */
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-tablet' AND e.kind = 1 AND e.failed_property = 'bit_depth' AND e.bit_depth = 10"), ==, 1);
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-tablet' AND e.kind = 1 AND e.failed_property = ''"), ==, 0);
	/* The hotel: a bitrate limit is a limit. */
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-hotel' AND e.effect = 'converted_limit'"), ==, 2);
	/* The stick: the codec was taken, only the container was not. */
	g_assert_cmpint(count(fx, "compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"s.device_id = 'axis-test-stick' AND e.kind = 1 AND e.failed_property = '' AND e.effect = 'passthrough'"), ==, 1);
	stated = query(fx, "SELECT count(*) FROM compat_evidence WHERE failed_property <> '' AND effect <> 'converted_codec'");
	g_assert_cmpstr(stated, ==, "0");

	/* The state of this server only. */
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'jellyfin' AND server_id = 'df1a584d4cef4af88f0d75e7a582ff28'"
		" AND server_version = '10.11.8' AND last_poll_ok_at IS NOT NULL AND failures = 0"), ==, 1);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'plex' AND last_poll_at IS NOT NULL"), ==, 0);

	/* Looked at again: the same sessions, seen again and not recorded again. */
	summary = poll(fx);
	g_assert_cmpuint(summary.recorded, ==, 0);
	g_assert_cmpuint(summary.updated, ==, 7);
	g_assert_cmpint(count(fx, "compat_session", NULL), ==, 7);
	g_assert_cmpint(count(fx, "compat_session", "polls = 2"), ==, 7);
}

/* Spec: Read-only. */
static void
test_only_get_requests_are_sent_and_only_the_key_header(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *header = NULL;
	g_autofree gchar *plain = NULL;
	g_autofree gchar *transcript = NULL;

	SKIP_WITHOUT_DB(fx);
	poll(fx);
	g_assert_false(axis_test_plex_saw_non_get(fx->t->jellyfin));
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/System/Info"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/Sessions"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/Library/VirtualFolders"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/Devices"), ==, 1);
	/* The sessions say the files they play: nothing is asked about them. */
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/Items"), ==, 0);
	header = axis_test_plex_last_header(fx->t->jellyfin, "/Sessions", "Authorization");
	g_assert_cmpstr(header, ==, "MediaBrowser Token=\"" AXIS_TEST_COMPAT_JELLYFIN_KEY "\"");
	plain = axis_test_plex_last_header(fx->t->jellyfin, "/Sessions", "X-Plex-Token");
	g_assert_null(plain);
	/* The key is never in a URL. */
	transcript = axis_test_plex_transcript(fx->t->jellyfin);
	g_assert_null(strstr(transcript, "api_key"));
	g_assert_null(strstr(transcript, "?" AXIS_TEST_COMPAT_JELLYFIN_KEY));
	/* The other server was never asked. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->plex), ==, 0);
}

/* A session that does not say its file: the item is asked for, with GET. */
static void
test_a_missing_path_is_asked_for_by_item(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autofree gchar *sessions = NULL;
	g_autofree gchar *items = NULL;

	SKIP_WITHOUT_DB(fx);
	sessions = axis_test_compat_fixture("jellyfin/constructed/session-no-transcoding-info.json", NULL, NULL);
	items = axis_test_compat_fixture("jellyfin/constructed/item-one.json", NULL, NULL);
	axis_test_plex_route(fx->t->jellyfin, "/Sessions", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"[{\"Id\":\"aaaa1111aaaa1111aaaa1111aaaa1111\",\"DeviceId\":\"d1\",\"DeviceName\":\"A\","
		"\"PlayState\":{\"PlayMethod\":\"DirectPlay\"},\"NowPlayingItem\":{\"Id\":\"ca8139a7c41a55835046264d60e1a618\","
		"\"Type\":\"Movie\"}}]", 0);
	axis_test_plex_route(fx->t->jellyfin, "/Items", -1, AXIS_TEST_PLEX_NORMAL, 200, items, 0);
	summary = poll(fx);
	g_assert_cmpuint(summary.recorded, ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/Items"), ==, 1);
	g_assert_false(axis_test_plex_saw_non_get(fx->t->jellyfin));
	/* The library follows from the path the item gave. */
	g_assert_cmpint(count(fx, "compat_session", "library_key = '" AXIS_TEST_JELLYFIN_FILMS "'"), ==, 1);
	/* Once known it is not asked again. */
	poll(fx);
	g_assert_cmpuint(axis_test_plex_count(fx->t->jellyfin, "/Items"), ==, 1);
	(void)sessions;
}

/* Spec: Stated reasons, cleaned and capped, are stored as the server said. */
static void
test_stated_reasons_are_stored_cleaned_and_capped(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *longest = NULL;
	g_autofree gchar *bad = NULL;

	SKIP_WITHOUT_DB(fx);
	serve_sessions(fx, "constructed/session-unknown-reasons.json", NULL, NULL);
	poll(fx);
	g_assert_cmpint(count(fx, "compat_evidence", "stated_reason IS NOT NULL"), >, 0);
	longest = query(fx, "SELECT max(char_length(stated_reason)) FROM compat_evidence");
	g_assert_cmpint(g_ascii_strtoll(longest, NULL, 10), <=, 64);
	bad = query(fx, "SELECT count(*) FROM compat_evidence WHERE stated_reason ~ '[\\x01-\\x1f\\x7f]'");
	g_assert_cmpstr(bad, ==, "0");
	/* A reason nobody knows, and the words of the ones the table knows that
	 * were mangled: cause unclear, and nothing is held against a codec. */
	g_assert_cmpint(count(fx, "compat_evidence", "effect = 'converted_codec'"), ==, 0);
	g_assert_cmpint(count(fx, "compat_evidence", "effect = 'converted_unclear' AND kind = 1"), ==, 1);
}

static void
test_ids_that_are_not_keys_are_made_into_keys_or_the_session_is_skipped(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;

	SKIP_WITHOUT_DB(fx);
	serve_sessions(fx, "constructed/session-odd-ids.json", NULL, NULL);
	summary = poll(fx);
	g_assert_cmpuint(summary.playing, ==, 4);
	g_assert_cmpuint(summary.recorded, ==, 2);
	g_assert_cmpuint(summary.skipped, ==, 2);
	g_assert_cmpint(count(fx, "compat_device", "client_id ~ '^h:[0-9a-f]{40}$'"), ==, 1);
	g_assert_cmpint(count(fx, "compat_device", "client_id = 'dev-numbers'"), ==, 1);
}

/* A server that is not the one the records belong to clears its records only. */
static void
test_a_different_server_clears_only_its_own_records(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);
	axis_test_compat_connect(fx->t);
	poll(fx);
	axis_test_compat_device(fx->t, "plex-tv", "Plex TV");
	g_assert_cmpint(count(fx, "compat_session", "server_kind = 'jellyfin'"), ==, 7);

	/* The Jellyfin address now belongs to another server. */
	{
		g_autofree gchar *info = axis_test_compat_fixture("jellyfin/recorded/identity.json", "df1a584d4cef4af88f0d75e7a582ff28",
			"0123456789abcdef0123456789abcdef");

		axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_NORMAL, 200, info, 0);
	}
	poll(fx);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'jellyfin' AND server_id = '0123456789abcdef0123456789abcdef'"), ==, 1);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'plex' AND server_id = 'aaaabbbbccccddddeeeeffff0000111122223333'"), ==, 1);
	g_assert_cmpint(count(fx, "compat_device", "client_id = 'plex-tv'"), ==, 1);
}

/* ── The secret ─────────────────────────────────────────────────── */

/* Spec: Secret stays secret. */
static void
test_each_secret_goes_only_to_its_own_server_in_its_own_header(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *plex_transcript = NULL;
	g_autofree gchar *jellyfin_transcript = NULL;
	g_autofree gchar *x_plex = NULL;
	g_autofree gchar *authorization = NULL;
	AxisCompatHistorySummary history;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	poll(fx);
	g_assert_true(axis_compat_import_history(fx->t->compat, TRUE, NULL, &history, &error));
	g_assert_no_error(error);
	/* Both servers were asked, each only with its own. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->plex), >, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->t->jellyfin), >, 0);
	plex_transcript = axis_test_plex_transcript(fx->t->plex);
	jellyfin_transcript = axis_test_plex_transcript(fx->t->jellyfin);
	g_assert_null(strstr(plex_transcript, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	g_assert_null(strstr(jellyfin_transcript, AXIS_TEST_COMPAT_TOKEN));
	g_assert_nonnull(strstr(plex_transcript, AXIS_TEST_COMPAT_TOKEN));
	g_assert_nonnull(strstr(jellyfin_transcript, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	/* Each in its own header, and in no other. */
	x_plex = axis_test_plex_last_header(fx->t->plex, "/status/sessions", "X-Plex-Token");
	g_assert_cmpstr(x_plex, ==, AXIS_TEST_COMPAT_TOKEN);
	g_assert_null(axis_test_plex_last_header(fx->t->plex, "/status/sessions", "Authorization"));
	authorization = axis_test_plex_last_header(fx->t->jellyfin, "/Sessions", "Authorization");
	g_assert_cmpstr(authorization, ==, "MediaBrowser Token=\"" AXIS_TEST_COMPAT_JELLYFIN_KEY "\"");
	g_assert_null(axis_test_plex_last_header(fx->t->jellyfin, "/Sessions", "X-Plex-Token"));
	g_assert_false(axis_test_plex_saw_non_get(fx->t->plex));
	g_assert_false(axis_test_plex_saw_non_get(fx->t->jellyfin));

	assert_secret_in_no_table(fx, AXIS_TEST_COMPAT_JELLYFIN_KEY);
	assert_secret_in_no_table(fx, AXIS_TEST_COMPAT_TOKEN);
	assert_secret_not_logged(AXIS_TEST_COMPAT_JELLYFIN_KEY);
	assert_secret_not_logged(AXIS_TEST_COMPAT_TOKEN);
}

/* A redirect to another address is refused, and nothing is sent there. */
static void
test_a_redirect_to_the_other_server_is_refused_and_nothing_is_sent_there(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *plex_base = axis_test_plex_base(fx->t->plex);
	g_autofree gchar *where = NULL;
	AxisCompatTestResult result;
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	where = g_strconcat(plex_base, "/identity", NULL);
	axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_REDIRECT, 302, where, 0);

	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "different address"));
	g_assert_null(strstr(result.message, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	axis_compat_test_result_clear(&result);

	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_nonnull(strstr(error->message, "different address"));
	g_assert_null(strstr(error->message, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	/* The Plex server was polled for its own sake and was sent nothing of
	 * Jellyfin's, and the redirect did not take Jellyfin's key there. */
	{
		g_autofree gchar *plex_transcript = axis_test_plex_transcript(fx->t->plex);

		g_assert_null(strstr(plex_transcript, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	}
	/* The failure is recorded against Jellyfin, scrubbed, and Plex's state is its own. */
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'jellyfin' AND failures = 1 AND last_error IS NOT NULL"), ==, 1);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'plex' AND failures = 0 AND last_poll_ok_at IS NOT NULL"), ==, 1);
	assert_secret_in_no_table(fx, AXIS_TEST_COMPAT_JELLYFIN_KEY);
	assert_secret_not_logged(AXIS_TEST_COMPAT_JELLYFIN_KEY);
}

static void
test_a_redirect_to_the_same_server_is_followed_with_the_key(Fixture *fx, gconstpointer data)
{
	AxisCompatTestResult result;
	g_autofree gchar *authorization = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/System/Info/Moved", 0);
	axis_test_plex_route_file(fx->t->jellyfin, "/System/Info/Moved", -1,
		"tests/fixtures/compat/jellyfin/recorded/identity.json");
	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_true(result.ok);
	authorization = axis_test_plex_last_header(fx->t->jellyfin, "/System/Info/Moved", "Authorization");
	g_assert_cmpstr(authorization, ==, "MediaBrowser Token=\"" AXIS_TEST_COMPAT_JELLYFIN_KEY "\"");
	axis_compat_test_result_clear(&result);
}

/* Spec: Connection test. */
static void
test_the_connection_test_names_the_server_or_says_why_not(Fixture *fx, gconstpointer data)
{
	AxisCompatTestResult result;

	SKIP_WITHOUT_DB(fx);
	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_true(result.ok);
	g_assert_cmpstr(result.name, ==, "83b9dc12ff49");
	g_assert_cmpstr(result.version, ==, "10.11.8");
	axis_compat_test_result_clear(&result);

	axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "token was refused"));
	g_assert_null(strstr(result.message, AXIS_TEST_COMPAT_JELLYFIN_KEY));
	axis_compat_test_result_clear(&result);

	/* A Plex server (or anything else) answering is not a Jellyfin server. */
	axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"{\"MediaContainer\":{\"machineIdentifier\":\"x\"}}", 0);
	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "does not say which server"));
	axis_compat_test_result_clear(&result);

	/* Plex's test is still Plex's. */
	axis_test_compat_connect(fx->t);
	axis_compat_test_connection(fx->t->compat, NULL, &result);
	g_assert_true(result.ok);
	g_assert_cmpstr(result.name, ==, "Invented Server");
	axis_compat_test_result_clear(&result);

	/* Not set up. */
	g_assert_true(axis_settings_clear(fx->t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, NULL));
	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "Jellyfin"));
	g_assert_nonnull(strstr(result.message, "not both set"));
	axis_compat_test_result_clear(&result);
	g_assert_false(axis_compat_is_configured_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN));
	g_assert_true(axis_compat_is_configured_kind(fx->t->compat, AXIS_COMPAT_KIND_PLEX));

	/* A key it cannot be sent with (a quote would end the header's value). */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, "abc\"def", NULL));
	axis_compat_test_connection_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN, NULL, &result);
	g_assert_false(result.ok);
	g_assert_nonnull(strstr(result.message, "cannot be sent"));
	g_assert_null(strstr(result.message, "abc"));
	axis_compat_test_result_clear(&result);
}

static void
test_the_settings_validate_the_address_and_keep_the_key_secret(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *shown = NULL;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	g_assert_false(axis_settings_set(fx->t->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, "http://user:hunter2@jf.example:8096",
		&error));
	g_assert_null(strstr(error->message, "hunter2"));
	g_clear_error(&error);
	g_assert_false(axis_settings_set(fx->t->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, "http://jf.example:8096/web", &error));
	g_clear_error(&error);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, "https://jf.example:8920", NULL));
	g_assert_true(axis_settings_is_set(fx->t->settings, AXIS_COMPAT_JELLYFIN_TOKEN));
	shown = axis_settings_format(fx->t->settings, AXIS_COMPAT_JELLYFIN_TOKEN);
	g_assert_null(shown);
	g_assert_false(axis_settings_get_boolean(fx->t->settings, AXIS_COMPAT_JELLYFIN_UNTRUSTED_TLS));
	g_assert_null(axis_compat_describe_address_kind(fx->t->compat, "nonsense"));
	{
		g_autofree gchar *address = axis_compat_describe_address_kind(fx->t->compat, AXIS_COMPAT_KIND_JELLYFIN);

		g_assert_cmpstr(address, ==, "https://jf.example:8920");
	}
}

/* ── Two servers at once ────────────────────────────────────────── */

/* Spec: One server down. */
static void
test_one_server_down_leaves_the_other_working(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	/* Plex fails: Jellyfin's sessions are recorded all the same. */
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_STATUS, 500, NULL, 500);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_nonnull(strstr(error->message, "500"));
	g_assert_cmpuint(summary.recorded, ==, 7);
	g_assert_cmpint(count(fx, "compat_session", "server_kind = 'jellyfin'"), ==, 7);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'plex' AND failures = 1 AND last_error IS NOT NULL"), ==, 1);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'jellyfin' AND failures = 0 AND last_error IS NULL AND"
		" last_poll_ok_at IS NOT NULL"), ==, 1);
	g_clear_error(&error);

	/* And the other way round: Jellyfin fails, Plex is polled. */
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"{\"MediaContainer\":{\"machineIdentifier\":\"aaaabbbbccccddddeeeeffff0000111122223333\",\"version\":\"1.43.5.0-invented\"}}", 0);
	axis_test_plex_route(fx->t->jellyfin, "/Sessions", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_nonnull(strstr(error->message, "503"));
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'jellyfin' AND failures = 1"), ==, 1);
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'plex' AND failures = 0 AND last_poll_ok_at IS NOT NULL"), ==, 1);
	/* The evidence recorded earlier is untouched. */
	g_assert_cmpint(count(fx, "compat_session", "server_kind = 'jellyfin'"), ==, 7);
	assert_secret_in_no_table(fx, AXIS_TEST_COMPAT_JELLYFIN_KEY);
}

/* A server that never answers does not hold up the other's polling. */
static void
test_a_silent_server_does_not_hold_up_the_other(Fixture *fx, gconstpointer data)
{
	AxisCompatTuning tuning;
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;
	gint64 start;
	gint64 took;

	SKIP_WITHOUT_DB(fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	axis_compat_set_tuning(fx->t->compat, &tuning);
	/* Both hang on their first request: serially that is twice the deadline. */
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	start = g_get_monotonic_time();
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	took = g_get_monotonic_time() - start;
	g_assert_nonnull(strstr(error->message, "in time"));
	g_assert_cmpint(took, <, 2500 * 1000);
	g_assert_cmpint(count(fx, "compat_state", "failures = 1"), ==, 2);
	g_clear_error(&error);

	/* One silent, one fine: the fine one is recorded in the time the other takes. */
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_NORMAL, 200,
		"{\"MediaContainer\":{\"machineIdentifier\":\"aaaabbbbccccddddeeeeffff0000111122223333\",\"version\":\"1.43.5.0-invented\"}}", 0);
	axis_test_plex_route(fx->t->jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	g_assert_false(axis_compat_poll(fx->t->compat, TRUE, NULL, &summary, &error));
	g_assert_cmpint(count(fx, "compat_state", "server_kind = 'plex' AND failures = 0 AND last_poll_ok_at IS NOT NULL"), ==, 1);
}

/* Spec: Devices are per server. */
static void
test_the_same_device_id_on_two_servers_is_two_devices(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) devices = NULL;
	AxisCompatDeviceInfo *info = NULL;
	g_autoptr(GError) error = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_device_of(fx->t, "plex", "tv", "Plex TV");
	axis_test_compat_device_of(fx->t, "jellyfin", "tv", "Jellyfin TV");
	axis_test_compat_add_evidence(fx->t, &(AxisTestEvidence){ "plex", "tv", "1", 1, "hevc", "main 10", 10, "1080p", 0,
		"mkv", "direct", NULL, NULL, FALSE, 1 });
	axis_test_compat_add_evidence(fx->t, &(AxisTestEvidence){ "jellyfin", "tv", AXIS_TEST_JELLYFIN_FILMS, 1, "h264",
		"high", 8, "1080p", 0, NULL, "converted_codec", "container", "ContainerNotSupported", FALSE, 1 });
	g_assert_cmpint(count(fx, "compat_device", "client_id = 'tv'"), ==, 2);

	g_assert_true(axis_compat_list_devices(fx->t->compat, &devices, &error));
	g_assert_cmpuint(devices->len, ==, 2);

	g_assert_true(axis_compat_get_device(fx->t->compat, "tv", &info, &error));
	g_assert_cmpstr(info->server_kind, ==, "plex");
	g_assert_cmpstr(info->name, ==, "Plex TV");
	g_assert_cmpuint(info->rows->len, ==, 1);
	axis_compat_device_info_free(info);
	g_assert_true(axis_compat_get_device(fx->t->compat, "jellyfin~tv", &info, &error));
	g_assert_cmpstr(info->server_kind, ==, "jellyfin");
	g_assert_cmpstr(info->name, ==, "Jellyfin TV");
	g_assert_cmpuint(info->rows->len, ==, 1);
	{
		const AxisCompatObsRow *row = g_ptr_array_index(info->rows, 0);

		g_assert_cmpint(row->obs.property, ==, AXIS_PROPERTY_CONTAINER);
		g_assert_cmpstr(row->stated, ==, "ContainerNotSupported");
	}
	axis_compat_device_info_free(info);
	g_assert_false(axis_compat_get_device(fx->t->compat, "nonsense~tv", &info, &error));
	g_assert_error(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* Pinning one does not pin the other. */
	g_assert_true(axis_compat_set_pinned(fx->t->compat, "jellyfin~tv", TRUE, &error));
	g_assert_cmpint(count(fx, "compat_device", "pinned AND server_kind = 'jellyfin'"), ==, 1);
	g_assert_cmpint(count(fx, "compat_device", "pinned AND server_kind = 'plex'"), ==, 0);
}

/* Spec: No history: considered only once seen. */
static void
test_a_device_with_no_history_counts_only_once_it_has_been_seen(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	const AxisCompatLibraryReport *films;
	const AxisCompatServerStatus *status;
	g_autoptr(GError) error = NULL;
	guint i;

	SKIP_WITHOUT_DB(fx);
	/* The server knows eight devices and none has been seen playing. */
	serve_sessions(fx, "recorded/sessions-idle.json", NULL, NULL);
	poll(fx);
	g_assert_cmpint(count(fx, "compat_device", "server_kind = 'jellyfin'"), ==, 8);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	g_assert_cmpstr(films->server_kind, ==, "jellyfin");
	g_assert_cmpint(films->state, ==, AXIS_MAP_MAPPED);
	g_assert_cmpuint(films->n_devices, ==, 0);
	for (i = 0; i < films->targets->len; i++)
		g_assert_cmpint(((AxisCompatTargetVerdict *)g_ptr_array_index(films->targets, i))->verdict, ==, AXIS_VERDICT_UNKNOWN);
	g_assert_cmpint(films->recommended, ==, -1);
	status = axis_compat_coverage_server(&report->coverage, AXIS_COMPAT_KIND_JELLYFIN);
	g_assert_nonnull(status);
	g_assert_false(status->has_history);
	g_assert_true(status->configured);
	g_assert_cmpint(status->devices, ==, 8);
	g_assert_cmpint(status->sessions, ==, 0);
	g_clear_pointer(&report, axis_compat_report_free);

	/* Once the clients are seen playing, the ones that played Films count. */
	serve_sessions(fx, "recorded/sessions-playing.json", NULL, NULL);
	poll(fx);
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	g_assert_cmpuint(films->n_devices, ==, 6);
	g_assert_cmpuint(axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_MUSIC)->n_devices, ==, 1);
	g_clear_pointer(&report, axis_compat_report_free);

	/* A device the server lists and that never played, marked always consider. */
	g_assert_true(axis_compat_set_pinned(fx->t->compat, "jellyfin~axis-test-setup", TRUE, &error));
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	g_assert_cmpuint(films->n_devices, ==, 7);
	g_clear_pointer(&report, axis_compat_report_free);

	/* A server that keeps no history imports none. */
	{
		AxisCompatHistorySummary history;

		g_assert_true(axis_compat_import_history(fx->t->compat, TRUE, NULL, &history, &error));
		g_assert_true(history.skipped);
		g_assert_cmpint(count(fx, "compat_play", "server_kind = 'jellyfin'"), ==, 0);
	}
}

/* A session whose library cannot be told counts toward every library of its
 * server: unknown is never safe. */
static void
test_a_session_in_no_library_counts_toward_every_library_of_its_server(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;

	SKIP_WITHOUT_DB(fx);
	serve_sessions(fx, "recorded/sessions-playing.json", "/media/films/Test Film H264 (2024)", "/elsewhere/Test Film H264 (2024)");
	poll(fx);
	/* tv, stick and hotel play a file outside every library. */
	g_assert_cmpint(count(fx, "compat_session", "library_key IS NULL"), ==, 3);
	report = axis_test_compat_report(fx->t);
	g_assert_cmpuint(axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS)->n_devices, ==, 6);
	/* The speaker played the music library, the three others might have. */
	g_assert_cmpuint(axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_MUSIC)->n_devices, ==, 4);
}

/* A path in two libraries at once is in neither that can be named. */
static void
test_a_path_in_two_libraries_is_in_neither(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *libraries = NULL;

	SKIP_WITHOUT_DB(fx);
	libraries = axis_test_compat_fixture("jellyfin/recorded/libraries.json", "\"/media/music\"", "\"/media/films\"");
	axis_test_plex_route(fx->t->jellyfin, "/Library/VirtualFolders", -1, AXIS_TEST_PLEX_NORMAL, 200, libraries, 0);
	poll(fx);
	g_assert_cmpint(count(fx, "compat_session", "library_key IS NOT NULL"), ==, 0);
}

/* Spec: Verdict across servers. */
static void
test_a_root_two_servers_serve_is_judged_by_the_devices_of_both(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	const AxisCompatLibraryReport *plex_films;
	const AxisCompatLibraryReport *jellyfin_films;
	const AxisCompatLibraryReport *plex_music;
	const AxisCompatTargetVerdict *tv;
	guint kinds = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	poll(fx);
	/* The Plex film library ("1") and Jellyfin's Films map to the one root.
	 * A Plex device was seen playing HEVC Main 10 in Matroska; a Jellyfin
	 * one was seen failing the Matroska container for HEVC. */
	axis_test_compat_play_of(fx->t, "plex", "plex-tv", "1", 1);
	axis_test_compat_add_evidence(fx->t, &(AxisTestEvidence){ "plex", "plex-tv", "1", 1, "hevc", "main 10", 10,
		"2160p", 0, "mkv", "direct", NULL, NULL, FALSE, 1 });
	axis_test_compat_add_evidence(fx->t, &(AxisTestEvidence){ "plex", "plex-tv", "1", 1, "hevc", "main 10", 10,
		"2160p", 1, "mkv", "direct", NULL, NULL, FALSE, 1 });
	axis_test_compat_add_evidence(fx->t, &(AxisTestEvidence){ "jellyfin", "jf-stick", AXIS_TEST_JELLYFIN_FILMS, 1,
		"hevc", "main 10", 10, "1080p", 0, "mkv", "converted_codec", "container", "ContainerNotSupported", FALSE, 1 });

	report = axis_test_compat_report(fx->t);
	plex_films = axis_test_compat_library(report, "1");
	jellyfin_films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	plex_music = axis_test_compat_library(report, "3");

	/* The Plex library alone has the Plex device and the Jellyfin library
	 * has the Jellyfin ones; each is judged by the devices of both. */
	g_assert_cmpuint(plex_films->n_devices, ==, jellyfin_films->n_devices);
	g_assert_cmpuint(plex_films->n_devices, >=, 2);
	tv = axis_test_compat_target(plex_films, "space_saver");
	g_assert_cmpint(tv->verdict, ==, AXIS_VERDICT_RISKY);
	for (i = 0; i < tv->devices->len; i++) {
		const AxisCompatDeviceVerdict *d = g_ptr_array_index(tv->devices, i);

		kinds |= g_strcmp0(d->server_kind, "plex") == 0 ? 1u : 2u;
	}
	g_assert_cmpuint(kinds, ==, 3);
	g_assert_cmpint(axis_test_compat_target(jellyfin_films, "space_saver")->verdict, ==, AXIS_VERDICT_RISKY);
	/* The music root is served by both too: the Jellyfin speaker that played
	 * it is considered for Plex's music library, which nobody played there. */
	g_assert_cmpuint(plex_music->n_devices, ==, 1);
	g_assert_cmpint(axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_MUSIC)->state, ==, AXIS_MAP_MAPPED);
}

/* Spec: Container-only failure (in the report). */
static void
test_a_container_failure_makes_the_target_in_that_container_risky_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisCompatReport) report = NULL;
	const AxisCompatLibraryReport *films;

	SKIP_WITHOUT_DB(fx);
	poll(fx);
	/* H.264 passed through and not taken in MP4: Maximum compatibility is
	 * H.264 in MP4. */
	axis_test_compat_play_of(fx->t, "jellyfin", "jf-mp4", AXIS_TEST_JELLYFIN_FILMS, 1);
	axis_test_compat_add_evidence(fx->t, &(AxisTestEvidence){ "jellyfin", "jf-mp4", AXIS_TEST_JELLYFIN_FILMS, 1,
		"h264", "high", 8, "1080p", 0, "mp4", "converted_codec", "container", "ContainerNotSupported", FALSE, 1 });
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	g_assert_cmpint(axis_test_compat_target(films, "max_compat")->verdict, ==, AXIS_VERDICT_RISKY);
	g_clear_pointer(&report, axis_compat_report_free);

	/* The same failure in Matroska: nothing against H.264 in MP4. */
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_evidence SET container = 'mkv' WHERE failed_property = 'container'"
		" AND session_id IN (SELECT id FROM compat_session WHERE device_id = 'jf-mp4')");
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	g_assert_cmpint(axis_test_compat_target(films, "max_compat")->verdict, ==, AXIS_VERDICT_UNKNOWN);
	g_clear_pointer(&report, axis_compat_report_free);

	/* Blamed on the codec itself, it counts in every container. */
	axis_test_db_exec(fx->t->lib->db, "UPDATE compat_evidence SET failed_property = '', container = NULL"
		" WHERE failed_property = 'container' AND session_id IN (SELECT id FROM compat_session WHERE device_id = 'jf-mp4')");
	report = axis_test_compat_report(fx->t);
	films = axis_test_compat_library(report, "jellyfin~" AXIS_TEST_JELLYFIN_FILMS);
	g_assert_cmpint(axis_test_compat_target(films, "max_compat")->verdict, ==, AXIS_VERDICT_RISKY);
}

/* A file the session names is matched to the index through the path map. */
static void
test_a_played_file_is_matched_through_the_jellyfin_path_map(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autoptr(GPtrArray) failures = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GError) error = NULL;
	const AxisCompatFailure *f;

	SKIP_WITHOUT_DB(fx);
	serve_sessions(fx, "recorded/sessions-playing.json", "Test Film H264 (2024)", "Big (2001)");
	summary = poll(fx);
	/* The TV, the stick and the hotel play it. */
	g_assert_cmpuint(summary.matched, ==, 3);
	g_assert_cmpint(count(fx, "compat_session", "file_id IS NOT NULL"), ==, 3);
	g_assert_true(axis_compat_report_failures(fx->t->compat, &failures, &files, &error));
	g_assert_cmpuint(failures->len, >, 0);
	f = g_ptr_array_index(failures, 0);
	g_assert_cmpstr(f->server_kind, ==, "jellyfin");
	g_assert_cmpuint(files->len, >, 0);
}

/* ── The migration ──────────────────────────────────────────────── */

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(gchar *const *)a, *(gchar *const *)b);
}

static void
assert_query(
	AxisTestDb  *db,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *got = axis_test_db_query(db, sql);

	g_assert_cmpstr(got, ==, expected);
}

static AxisDbMigration *
migrations_up_to(
	guint        last,
	GPtrArray  **owned
){
	g_autoptr(GDir) dir = g_dir_open("data/migrations", 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	AxisDbMigration *list = g_new0(AxisDbMigration, last);
	const gchar *name;
	guint i;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_ptr_array_sort(names, compare_names);
	*owned = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < last; i++) {
		const gchar *file = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename("data/migrations", file, NULL);
		gchar *sql = NULL;
		gchar *migration_name = NULL;
		guint version = 0;

		g_assert_true(axis_db_migration_parse_filename(file, &version, &migration_name));
		g_assert_cmpuint(version, ==, i + 1);
		g_assert_true(g_file_get_contents(path, &sql, NULL, NULL));
		g_ptr_array_add(*owned, sql);
		g_ptr_array_add(*owned, migration_name);
		list[i].version = version;
		list[i].name = migration_name;
		list[i].sql = sql;
		list[i].no_transaction = axis_db_migration_has_no_transaction_marker(sql);
	}
	return list;
}

static gchar *
digest(
	AxisTestDb  *db,
	const gchar *sql
){
	return axis_test_db_query(db, sql);
}

/* A database written under the previous schema, with Plex rows in every
 * table, is migrated: nothing is lost or duplicated, everything is Plex, and
 * the keys now hold a second kind. */
static void
test_the_migration_keeps_plex_rows_and_makes_room_for_a_second_kind(void)
{
	static const gchar *const summaries[] = {
		"SELECT string_agg(key || '/' || kind || '/' || coalesce(title, '') || '/' || array_to_string(folders, '|'), ',' ORDER BY key) FROM compat_library",
		"SELECT string_agg(server_path || '>' || axis_path, ',' ORDER BY server_path) FROM compat_path_map",
		"SELECT string_agg(client_id || '/' || coalesce(name, '') || '/' || coalesce(plex_id::text, '') || '/' || pinned::text, ',' ORDER BY client_id) FROM compat_device",
		"SELECT string_agg(device_id || '/' || library_key || '/' || rating_key || '/' || account_id, ',' ORDER BY device_id, rating_key) FROM compat_play",
		"SELECT string_agg(server_id || '/' || session_key || '/' || rating_key || '/' || device_id || '/' || coalesce(library_key, '') || '/' || remote::text, ',' ORDER BY session_key) FROM compat_session",
		"SELECT string_agg(s.session_key || '/' || e.kind || '/' || e.codec || '/' || e.effect || '/' || coalesce(e.container, '') || '/' || coalesce(e.stated_reason, ''), ',' ORDER BY s.session_key, e.kind) FROM compat_evidence e JOIN compat_session s ON s.id = e.session_id",
		"SELECT server_id || '/' || failures::text || '/' || coalesce(last_error, '') FROM compat_state",
		NULL
	};
	g_autoptr(AxisTestDb) db = axis_test_db_new();
	g_autoptr(GPtrArray) owned = NULL;
	g_autoptr(GError) error = NULL;
	AxisDbPool *pool;
	AxisDbMigration *list;
	g_autoptr(GPtrArray) before = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(AxisCompat) compat = NULL;
	AxisCompatDeviceInfo *info = NULL;
	guint i;

	if (db == NULL)
		return;
	axis_test_log_init();
	pool = axis_db_pool_new(axis_test_db_get_url(db), 3, &error);
	g_assert_no_error(error);
	list = migrations_up_to(12, &owned);
	g_assert_true(axis_db_migrate_with(pool, list, 12, &error));
	g_assert_no_error(error);

	/* What the previous release wrote. */
	axis_test_db_exec(db,
		"UPDATE compat_state SET server_id = 'machine-1', server_name = 'Old Plex', failures = 2, last_error = 'it broke';"
		"INSERT INTO compat_library (key, kind, title, folders) VALUES ('1', 'movie', 'Movies', '{/data/movies}'),"
		" ('2', 'show', 'Shows', '{/data/tv,/data/tv-extra}');"
		"INSERT INTO compat_path_map (server_path, axis_path) VALUES ('/data/movies', '/media/disk1/Films');"
		"INSERT INTO compat_device (client_id, plex_id, name, pinned) VALUES ('tv', 7, 'Living room', true),"
		" ('phone', NULL, 'Phone', false);"
		"INSERT INTO compat_play (device_id, library_key, rating_key, viewed_at) VALUES ('tv', '1', 'r1', now()),"
		" ('tv', '1', 'r2', now() - interval '1 day'), ('phone', '2', 'r3', now());"
		"INSERT INTO compat_session (server_id, session_key, rating_key, device_id, library_key, remote) VALUES"
		" ('machine-1', 's1', 'i1', 'tv', '1', false), ('machine-1', 's2', 'i2', 'phone', '2', true);"
		"INSERT INTO compat_evidence (session_id, kind, codec, effect, container, stated_reason)"
		" SELECT id, 1, 'hevc', 'direct', 'mkv', NULL FROM compat_session WHERE session_key = 's1';"
		"INSERT INTO compat_evidence (session_id, kind, codec, effect) SELECT id, 2, 'aac', 'passthrough'"
		" FROM compat_session WHERE session_key = 's1';"
		"INSERT INTO compat_evidence (session_id, kind, codec, effect, out_codec) SELECT id, 1, 'h264',"
		" 'converted_codec', 'vp9' FROM compat_session WHERE session_key = 's2';");
	for (i = 0; summaries[i] != NULL; i++)
		g_ptr_array_add(before, digest(db, summaries[i]));
	g_assert_cmpstr(g_ptr_array_index(before, 2), ==, "phone/Phone//false,tv/Living room/7/true");

	/* The current schema. */
	g_assert_true(axis_db_migrate(pool, &error));
	g_assert_no_error(error);
	for (i = 0; summaries[i] != NULL; i++) {
		g_autofree gchar *after = digest(db, summaries[i]);

		g_assert_cmpstr(after, ==, g_ptr_array_index(before, i));
	}
	/* Nothing lost, nothing duplicated, all of it Plex. */
	assert_query(db, "SELECT count(*) FROM compat_library WHERE server_kind = 'plex'", "2");
	assert_query(db, "SELECT count(*) FROM compat_device WHERE server_kind = 'plex'", "2");
	assert_query(db, "SELECT count(*) FROM compat_play WHERE server_kind = 'plex'", "3");
	assert_query(db, "SELECT count(*) FROM compat_session WHERE server_kind = 'plex'", "2");
	assert_query(db, "SELECT count(*) FROM compat_path_map WHERE server_kind = 'plex'", "1");
	assert_query(db, "SELECT count(*) FROM compat_evidence WHERE failed_property = ''", "3");
	assert_query(db, "SELECT count(*) FROM compat_state", "1");
	assert_query(db, "SELECT id || '/' || server_kind FROM compat_state", "1/plex");

	/* A second kind fits beside them. */
	axis_test_db_exec(db,
		"INSERT INTO compat_state (id, server_kind, server_id) VALUES (2, 'jellyfin', 'jf-1');"
		"INSERT INTO compat_library (server_kind, key, kind, title) VALUES ('jellyfin', '1', 'movie', 'Films');"
		"INSERT INTO compat_device (server_kind, client_id, name) VALUES ('jellyfin', 'tv', 'Jellyfin TV');"
		"INSERT INTO compat_path_map (server_kind, server_path, axis_path) VALUES ('jellyfin', '/data/movies', '/media/disk1/Films');"
		"INSERT INTO compat_play (server_kind, device_id, library_key, rating_key, viewed_at) VALUES"
		" ('jellyfin', 'tv', '1', 'r1', now());"
		"INSERT INTO compat_session (server_kind, server_id, session_key, rating_key, device_id, remote) VALUES"
		" ('jellyfin', 'jf-1', 's1', 'i1', 'tv', false);"
		"INSERT INTO compat_evidence (session_id, kind, codec, effect, container, failed_property, stated_reason)"
		" SELECT id, 1, 'hevc', 'converted_codec', 'mkv', 'container', 'ContainerNotSupported' FROM compat_session"
		" WHERE server_kind = 'jellyfin';");
	assert_query(db, "SELECT count(*) FROM compat_device WHERE client_id = 'tv'", "2");

	/* ...and the keys refuse a second of the same kind, a row without the
	 * device it names, a property without a conversion, a kind twice. */
	{
		static const gchar *const refused[] = {
			"INSERT INTO compat_device (client_id) VALUES ('tv')",
			"INSERT INTO compat_device (server_kind, client_id) VALUES ('jellyfin', 'tv')",
			"INSERT INTO compat_library (key, kind) VALUES ('1', 'movie')",
			"INSERT INTO compat_path_map (server_path, axis_path) VALUES ('/data/movies', '/other')",
			"INSERT INTO compat_play (server_kind, device_id, library_key, rating_key, viewed_at) VALUES ('jellyfin', 'nobody', '1', 'r', now())",
			"INSERT INTO compat_session (server_kind, server_id, session_key, rating_key, device_id, remote) VALUES ('jellyfin', 'jf-1', 'sx', 'ix', 'nobody', false)",
			"INSERT INTO compat_session (server_kind, server_id, session_key, rating_key, device_id, remote) VALUES ('jellyfin', 'jf-1', 's1', 'i1', 'tv', false)",
			"INSERT INTO compat_evidence (session_id, kind, codec, effect, failed_property) SELECT id, 2, 'aac', 'direct', 'container' FROM compat_session WHERE server_kind = 'jellyfin'",
			"INSERT INTO compat_evidence (session_id, kind, codec, effect, failed_property) SELECT id, 2, 'aac', 'converted_codec', 'sneeze' FROM compat_session WHERE server_kind = 'jellyfin'",
			"INSERT INTO compat_evidence (session_id, kind, codec, effect, container, failed_property, stated_reason) SELECT id, 1, 'hevc', 'converted_codec', 'mkv', 'container', 'x' FROM compat_session WHERE server_kind = 'jellyfin'",
			"INSERT INTO compat_state (id, server_kind) VALUES (3, 'jellyfin')",
			"INSERT INTO compat_state (id, server_kind) VALUES (4, 'Not A Kind')",
			NULL
		};

		for (i = 0; refused[i] != NULL; i++) {
			g_autoptr(GError) failure = NULL;

			g_assert_false(axis_db_pool_exec(pool, refused[i], 0, NULL, NULL, &failure));
			g_assert_nonnull(failure);
		}
	}

	/* A Plex device that goes takes its Plex rows and no Jellyfin ones. */
	axis_test_db_exec(db, "DELETE FROM compat_device WHERE server_kind = 'plex' AND client_id = 'tv'");
	assert_query(db, "SELECT count(*) FROM compat_session WHERE server_kind = 'plex'", "1");
	assert_query(db, "SELECT count(*) FROM compat_session WHERE server_kind = 'jellyfin'", "1");
	assert_query(db, "SELECT count(*) FROM compat_play WHERE server_kind = 'jellyfin'", "1");
	assert_query(db, "SELECT count(*) FROM compat_evidence", "2");

	/* The module reads the migrated database. */
	compat = axis_compat_new(pool, NULL, NULL, NULL);
	g_assert_true(axis_compat_list_devices(compat, &devices, &error));
	g_assert_cmpuint(devices->len, ==, 2);
	g_assert_true(axis_compat_get_device(compat, "phone", &info, &error));
	g_assert_cmpuint(info->rows->len, ==, 1);
	g_assert_cmpint(((AxisCompatObsRow *)g_ptr_array_index(info->rows, 0))->obs.property, ==, AXIS_PROPERTY_NONE);
	axis_compat_device_info_free(info);
	g_assert_true(axis_compat_get_device(compat, "jellyfin~tv", &info, &error));
	g_assert_cmpint(((AxisCompatObsRow *)g_ptr_array_index(info->rows, 0))->obs.property, ==, AXIS_PROPERTY_CONTAINER);
	axis_compat_device_info_free(info);
	g_free(list);
	axis_db_pool_free(pool);
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(name, flags, func) \
	AXIS_TEST_ADD_DB_CASE("/compat-jellyfin-db/" name, Fixture, GINT_TO_POINTER(flags), func)
	ADD("collect", 0, test_the_recorded_sessions_are_collected_once_with_their_evidence);
	ADD("read-only", 0, test_only_get_requests_are_sent_and_only_the_key_header);
	ADD("item-path", F_LIBRARY, test_a_missing_path_is_asked_for_by_item);
	ADD("stated-reasons", 0, test_stated_reasons_are_stored_cleaned_and_capped);
	ADD("odd-ids", 0, test_ids_that_are_not_keys_are_made_into_keys_or_the_session_is_skipped);
	ADD("other-server", F_PLEX, test_a_different_server_clears_only_its_own_records);
	ADD("secret/each-its-own", F_PLEX, test_each_secret_goes_only_to_its_own_server_in_its_own_header);
	ADD("secret/redirect-away", F_PLEX, test_a_redirect_to_the_other_server_is_refused_and_nothing_is_sent_there);
	ADD("secret/redirect-same", 0, test_a_redirect_to_the_same_server_is_followed_with_the_key);
	ADD("connection-test", 0, test_the_connection_test_names_the_server_or_says_why_not);
	ADD("settings", 0, test_the_settings_validate_the_address_and_keep_the_key_secret);
	ADD("two-servers/one-down", F_PLEX, test_one_server_down_leaves_the_other_working);
	ADD("two-servers/silent", F_PLEX, test_a_silent_server_does_not_hold_up_the_other);
	ADD("two-servers/devices", 0, test_the_same_device_id_on_two_servers_is_two_devices);
	ADD("two-servers/verdict", F_PLEX | F_LIBRARY, test_a_root_two_servers_serve_is_judged_by_the_devices_of_both);
	ADD("no-history", F_LIBRARY, test_a_device_with_no_history_counts_only_once_it_has_been_seen);
	ADD("no-library", F_LIBRARY, test_a_session_in_no_library_counts_toward_every_library_of_its_server);
	ADD("two-libraries", F_LIBRARY, test_a_path_in_two_libraries_is_in_neither);
	ADD("container-verdict", F_LIBRARY, test_a_container_failure_makes_the_target_in_that_container_risky_only);
	ADD("matched", F_LIBRARY, test_a_played_file_is_matched_through_the_jellyfin_path_map);
#undef ADD
	g_test_add_func("/compat-jellyfin-db/migration", test_the_migration_keeps_plex_rows_and_makes_room_for_a_second_kind);
	return g_test_run();
}
