/*
 * test-download.c - the downloads layer against the fixture kind's fake
 * client, a real database and a real volume in a scratch directory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (downloaders):
 *   Test (a wrong secret: authentication refused, the secret not shown)
 *   Someone else's download; Removal of an unknown download
 *   Crash while sending (both windows, by injected faults, and a lost
 *     answer): never a second send
 *   Completed; Failed; Client unreachable
 *   Unmapped path; Outside every volume
 *   Redirect (nothing sent to the other address), and secrets and session
 *     cookies in no log line and no table but their own
 * and tasks 5.1 to 5.6 of add-downloader-core.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "download/axis-download.h"
#include "download/axis-download-bencode.h"

#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD
#define MAGNET_HASH "0123456789abcdef0123456789abcdef01234567"
#define MAGNET "magnet:?xt=urn:btih:" MAGNET_HASH "&dn=Some.Film.2020.1080p"

typedef struct
{
	AxisTestLibrary      *t;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads;
	gchar                *disk;
	gint64                volume_id;
	gint64                client_id;
} Fixture;

static AxisDownloads *
layer_new(Fixture *fx)
{
	AxisDownloads *downloads = axis_downloads_new(fx->t->pool, fx->t->storage, NULL);
	AxisDownloadTuning tuning;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_kind(downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(downloads, &tuning);
	return downloads;
}

static gint64
add_client(
	Fixture     *fx,
	const gchar *name,
	const gchar *kind,
	const gchar *address,
	const gchar *secret
){
	AxisDownloadClientSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = kind;
	spec.address = address;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_HIGH;
	spec.enabled = TRUE;
	spec.secret = secret;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->t = axis_test_library_new(4);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->fake = axis_test_download_fake_new(PASSWORD);
	fx->downloads = layer_new(fx);
	base = axis_test_download_fake_base(fx->fake);
	fx->client_id = add_client(fx, "Torrents", "fixture", base, PASSWORD);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL)
		return;
	axis_downloads_unref(fx->downloads);
	axis_test_download_fake_free(fx->fake);
	g_free(fx->disk);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

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

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static gint64
add_magnet(
	Fixture     *fx,
	const gchar *magnet,
	const gchar *name
){
	AxisDownloadRequest request;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = magnet;
	request.release_name = name;
	request.title_label = "Some Film (2020)";
	request.module = "movies";
	g_assert_true(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

static AxisDownloadPollReport
poll(Fixture *fx)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, fx->client_id, NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

static gchar *
state_of(
	Fixture *fx,
	gint64   id
){
	return q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, id);
}

static void
assert_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state
){
	g_autofree gchar *now = state_of(fx, id);

	g_assert_cmpstr(now, ==, state);
}

/* Messages logged at any level that match @pattern. */
static guint
logged(const gchar *pattern)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	guint n = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		n += axis_test_log_count(levels[i], pattern);
	return n;
}

/* The restarted daemon: a new layer on the same database.  A record left
 * 'sending' by the old one is made older than the reconciliation wait. */
static void
restart(Fixture *fx)
{
	axis_downloads_unref(fx->downloads);
	fx->downloads = layer_new(fx);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour' WHERE state = 'sending'");
}

/* ── Clients ────────────────────────────────────────────────────── */

static void
test_client_secret_is_kept_apart(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);

	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_true(client->secret_set);
	g_assert_cmpstr(client->category, ==, "axis");
	g_assert_cmpstr(client->kind, ==, "fixture");
	g_assert_true(client->plain_http);
	g_assert_cmpint(client->priority, ==, AXIS_DOWNLOAD_PRIORITY_HIGH);
	/* Only in its own table. */
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" PASSWORD "%'");
	assert_q(fx, "1", "SELECT count(*) FROM download_client_secret WHERE secret = '" PASSWORD "'");

	/* An edit without a secret keeps it. */
	memset(&spec, 0, sizeof(spec));
	spec.name = "Torrents renamed";
	spec.address = base;
	spec.category = "axis-films";
	spec.priority = AXIS_DOWNLOAD_PRIORITY_LOW;
	spec.enabled = TRUE;
	spec.secret = "";
	g_assert_true(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_no_error(error);
	assert_q(fx, "1", "SELECT count(*) FROM download_client_secret WHERE secret = '" PASSWORD "'");
	assert_q(fx, "axis-films", "SELECT category FROM download_client");

	/* Refusals say which field, never what was typed. */
	spec.address = "ftp://secret-host-name/";
	g_assert_false(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_assert_null(strstr(error->message, "secret-host-name"));
	g_clear_error(&error);
	spec.address = base;
	spec.category = "has space";
	g_assert_false(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	spec.category = NULL;
	spec.secret = "with\na line break";
	g_assert_false(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_assert_null(strstr(error->message, "line break"));
	g_clear_error(&error);
	spec.secret = NULL;
	spec.kind = "fixture";
	spec.name = "Torrents renamed";
	g_assert_false(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS);
	g_clear_error(&error);
	spec.kind = "nosuchkind";
	spec.name = "Another";
	g_assert_false(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
}

static void
test_client_test(Fixture *fx, gconstpointer data)
{
	AxisDownloadTestResult result;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(result.version, ==, "Fixture 1.0");
	g_assert_cmpint(result.category_exists, ==, 1);
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_cmpstr(client->version, ==, "Fixture 1.0");
	g_assert_cmpint(client->category_exists, ==, 1);
	g_assert_cmpint(client->last_contact_us, >, 0);
}

/* Spec: Test (a wrong secret) */
static void
test_client_test_with_a_wrong_secret(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 id = add_client(fx, "Wrong", "fixture", base, "not-the-password-6a7e");
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadTestResult result;

	g_assert_true(axis_downloads_test_client(fx->downloads, id, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(result.note, "The API key or password was refused"));
	g_assert_null(strstr(result.note, "not-the-password-6a7e"));
	axis_download_test_result_clear(&result);
	g_assert_true(axis_downloads_get_client(fx->downloads, id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_REFUSED);
	g_assert_nonnull(strstr(client->state_message, "The API key or password was refused"));
	g_assert_null(strstr(client->state_message, "not-the-password-6a7e"));
	g_assert_cmpuint(logged("*not-the-password-6a7e*"), ==, 0);
}

/* ── Adding ─────────────────────────────────────────────────────── */

static void
test_add_a_magnet(Fixture *fx, gconstpointer data)
{
	gint64 id = add_magnet(fx, MAGNET, "Some.Film.2020.1080p");
	g_autofree gchar *fake_id = axis_test_download_fake_last_id(fx->fake);
	g_autofree gchar *category = axis_test_download_fake_last_add_field(fx->fake, "category");
	g_autofree gchar *priority = axis_test_download_fake_last_add_field(fx->fake, "priority");
	g_autofree gchar *ref = q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	assert_state(fx, id, "queued");
	g_assert_cmpstr(ref, ==, fake_id);
	g_assert_cmpstr(category, ==, "axis");
	g_assert_cmpstr(priority, ==, "high");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpstr(info->info_hash, ==, MAGNET_HASH);
	g_assert_cmpstr(info->title_label, ==, "Some Film (2020)");
	g_assert_cmpstr(info->module, ==, "movies");
	g_assert_cmpstr(info->client_name, ==, "Torrents");

	/* The same torrent at the same client again is refused here. */
	{
		AxisDownloadRequest request;
		g_autoptr(GError) again = NULL;

		memset(&request, 0, sizeof(request));
		request.protocol = AXIS_DOWNLOAD_TORRENT;
		request.magnet = "magnet:?xt=urn:btih:" MAGNET_HASH;
		request.release_name = "Some.Film.2020.1080p";
		g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &again));
		g_assert_error(again, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS);
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);
	}
}

static void
test_add_a_torrent_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *hash = NULL;
	g_autoptr(GBytes) torrent = axis_test_download_torrent("Another.Film.2019", &hash);
	g_autofree gchar *stored = NULL;
	g_autofree gchar *file_name = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadRequest request;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.file = torrent;
	/* No name: the torrent's own. */
	g_assert_true(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_no_error(error);
	stored = q(fx, "SELECT info_hash || ' ' || release_name || ' ' || source FROM download WHERE id = %" G_GINT64_FORMAT,
		id);
	g_assert_true(g_str_has_prefix(stored, hash));
	g_assert_true(g_str_has_suffix(stored, " Another.Film.2019 file"));
	file_name = axis_test_download_fake_last_add_field(fx->fake, "file_name");
	g_assert_cmpstr(file_name, ==, "Another.Film.2019.torrent");
}

/* Task 5.6: magnet validation; an oversized upload; not a torrent. */
static void
test_add_refuses_what_cannot_be_sent(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad[] = {
		"magnet:?xt=urn:sha1:0123456789abcdef0123456789abcdef01234567",
		"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef0123456",	/* 39 */
		"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef012345678",	/* 41 */
		"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef0123456g",
		"magnet:?xt=urn:btih:" MAGNET_HASH "&dn=has space",
		"magnet:?xt=urn:btih:" MAGNET_HASH "&dn=<script>",
		"http://example.test/a.torrent",
		"",
		NULL
	};
	AxisDownloadRequest request;
	g_autoptr(GError) error = NULL;
	gsize big = AXIS_DOWNLOAD_MAX_RELEASE_BYTES + 1;
	g_autoptr(GBytes) oversized = g_bytes_new_take(g_malloc0(big), big);
	g_autoptr(GBytes) not_torrent = g_bytes_new_static("<nzb></nzb>", 11);
	guint i;

	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.release_name = "Name";
	for (i = 0; bad[i] != NULL; i++) {
		request.magnet = bad[i];
		g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
		g_clear_error(&error);
	}
	{
		gchar *long_magnet = g_strdup_printf("magnet:?xt=urn:btih:%s&tr=%s", MAGNET_HASH, "");
		GString *grown = g_string_new(long_magnet);

		while (grown->len <= AXIS_DOWNLOAD_MAGNET_MAX)
			g_string_append(grown, "x");
		request.magnet = grown->str;
		g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
		g_clear_error(&error);
		g_string_free(grown, TRUE);
		g_free(long_magnet);
	}
	request.magnet = NULL;
	request.file = oversized;
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	request.file = not_torrent;
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	/* A Usenet release: no client takes it. */
	request.protocol = AXIS_DOWNLOAD_USENET;
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT);
	g_clear_error(&error);
	/* For the torrent client, by its id: the other kind of release. */
	request.client_id = fx->client_id;
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	/* Nothing was recorded and nothing reached the client. */
	assert_q(fx, "0", "SELECT count(*) FROM download");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_download_fake_server(fx->fake)), ==, 0);

	/* Valid forms: a base32 hash, and further parameters. */
	g_assert_true(axis_download_magnet_parse("magnet:?xt=urn:btih:MFRGGZDFMFRGGZDFMFRGGZDFMFRGGZDF", NULL, NULL));
	g_assert_true(axis_download_magnet_parse("MAGNET:?xt=urn:btih:" MAGNET_HASH "&dn=A.B&tr=udp%3A%2F%2Ft:80", NULL,
		NULL));
}

/* ── Axis touches only its own (5.1) ────────────────────────────── */

/* Spec: Someone else's download; Removal of an unknown download */
static void
test_only_axis_downloads_are_touched(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id;

	/* Added by a person, in another category; and by another program in
	 * Axis's own category. */
	axis_test_download_fake_put(fx->fake, "manual-1", "Holiday.Videos", "manual", NULL, "downloading", 1000, 10,
		NULL, NULL);
	axis_test_download_fake_put(fx->fake, "radarr-1", "Other.Film.2018", "axis", NULL, "downloading", 1000, 10,
		NULL, NULL);
	id = add_magnet(fx, MAGNET, "Some.Film.2020.1080p");
	ref = q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000000, NULL, NULL);

	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(report.listed, ==, 2);	/* Axis's category only */
	g_assert_cmpuint(report.ignored, ==, 1);	/* the other program's, unrecorded */
	assert_state(fx, id, "downloading");
	assert_q(fx, "1", "SELECT count(*) FROM download");
	assert_q(fx, "0", "SELECT count(*) FROM download WHERE client_ref IN ('manual-1', 'radarr-1') OR "
		"release_name IN ('Holiday.Videos', 'Other.Film.2018')");

	/* Removal by an identifier Axis did not add: refused, nothing sent. */
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "manual-1", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_downloads_remove_ref(fx->downloads, fx->client_id, "radarr-1", TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* Its own: removed. */
	g_assert_true(axis_downloads_remove_ref(fx->downloads, fx->client_id, ref, TRUE, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "removed");
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);

	/* The others, untouched throughout. */
	poll(fx);
	poll(fx);
	poll(fx);
	poll(fx);
	g_assert_true(axis_test_download_fake_has(fx->fake, "manual-1"));
	g_assert_true(axis_test_download_fake_has(fx->fake, "radarr-1"));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 1);
	assert_q(fx, "1", "SELECT count(*) FROM download");
	assert_state(fx, id, "removed");
}

/* ── Sent once (5.2) ────────────────────────────────────────────── */

/* Spec: Crash while sending (stopped after recording, before sending) */
static void
test_crash_before_sending_is_never_sent(Fixture *fx, gconstpointer data)
{
	AxisDownloadRequest request;
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_RECORD);
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = MAGNET;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_cmpint(id, >, 0);
	assert_state(fx, id, "sending");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 0);

	/* A young 'sending' record is left alone by the old daemon... */
	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_NONE);
	report = poll(fx);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "sending");

	/* ...and reconciled by the restarted one: the client never had it. */
	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.not_found, ==, 1);
	assert_state(fx, id, "failed");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 0);
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 0);
	poll(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 0);
}

/* Spec: Crash while sending (stopped after sending, before recording) */
static void
test_crash_after_sending_is_adopted(Fixture *fx, gconstpointer data)
{
	AxisDownloadRequest request;
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *fake_id = NULL;
	g_autofree gchar *ref = NULL;
	gint64 id = 0;

	axis_downloads_set_fault(fx->downloads, AXIS_DOWNLOAD_FAULT_AFTER_SEND);
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = MAGNET;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	assert_state(fx, id, "sending");
	assert_q(fx, NULL, "SELECT client_ref FROM download");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);
	fake_id = axis_test_download_fake_last_id(fx->fake);

	restart(fx);
	report = poll(fx);
	g_assert_cmpuint(report.adopted, ==, 1);
	g_assert_cmpuint(report.not_found, ==, 0);
	assert_state(fx, id, "queued");
	ref = q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(ref, ==, fake_id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 1);
}

/* A Usenet release has no hash: it is matched by its name. */
static void
test_lost_answer_is_matched_by_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 nzb_client = add_client(fx, "Usenet", "fixturenzb", base, PASSWORD);
	g_autoptr(GBytes) nzb = g_bytes_new_static("<?xml version=\"1.0\"?><nzb/>", 27);
	AxisDownloadRequest request;
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	/* The client takes it and the answer never comes. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_ADD_HANGS);
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.file = nzb;
	request.release_name = "Some.Series.S01E01.720p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN);
	assert_state(fx, id, "sending");
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);

	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour'");
	g_assert_true(axis_downloads_poll_client(fx->downloads, nzb_client, NULL, &report, &error));
	g_assert_cmpuint(report.adopted, ==, 1);
	assert_state(fx, id, "queued");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);
}

static void
test_refused_and_unsent_are_failed(Fixture *fx, gconstpointer data)
{
	AxisDownloadRequest request;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *problem = NULL;
	AxisTestPlex *closed = axis_test_plex_new();
	g_autofree gchar *closed_base = axis_test_plex_base(closed);
	gint64 down_client;
	gint64 id = 0;

	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_ADD_REFUSED);
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = MAGNET;
	request.release_name = "Some.Film.2020.1080p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_clear_error(&error);
	assert_state(fx, id, "failed");
	problem = q(fx, "SELECT problem FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(g_str_has_prefix(problem, "Not sent: "));

	/* A client that cannot be reached at all: nothing went out. */
	axis_test_plex_free(closed);
	down_client = add_client(fx, "Down", "fixture", closed_base, PASSWORD);
	request.client_id = down_client;
	request.magnet = "magnet:?xt=urn:btih:" "1111111111111111111111111111111111111111";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE);
	assert_state(fx, id, "failed");
}

/* ── Tracking (5.3, 5.4) ────────────────────────────────────────── */

static gint64
active_download(
	Fixture  *fx,
	gchar   **ref
){
	gint64 id = add_magnet(fx, MAGNET, "Some.Film.2020.1080p");

	*ref = q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
	axis_test_download_fake_set(fx->fake, *ref, "downloading", 1600000000, NULL, NULL);
	poll(fx);
	assert_state(fx, id, "downloading");
	return id;
}

static void
add_map(
	Fixture     *fx,
	const gchar *prefix,
	const gchar *rel
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, prefix, fx->volume_id, rel, &error));
	g_assert_no_error(error);
}

/* Spec: Completed */
static void
test_completed_with_its_location(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "complete", "Some.Film.2020.1080p", NULL);
	g_autofree gchar *file = g_build_filename(place, "film.mkv", NULL);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 id = active_download(fx, &ref);

	axis_test_storage_mkdir(place);
	axis_test_storage_write(file, "film");
	add_map(fx, "/data/downloads", "downloads");
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/complete/Some.Film.2020.1080p",
		NULL);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 1);
	assert_state(fx, id, "completed");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpint(info->volume_id, ==, fx->volume_id);
	g_assert_cmpstr(info->rel_path, ==, "downloads/complete/Some.Film.2020.1080p");
	g_assert_cmpstr(info->volume_name, ==, "Disk one");
	g_assert_cmpfloat(axis_download_info_progress(info), ==, 1.0);
	g_assert_cmpint(info->completed_us, >, 0);
	g_assert_null(info->problem);
	/* What was at the place when it completed, for its leftover later. */
	{
		GStatBuf st;
		g_autofree gchar *expected = NULL;
		g_autofree gchar *recorded = q(fx, "SELECT location_inode FROM download WHERE id = %" G_GINT64_FORMAT, id);

		g_assert_cmpint(g_lstat(place, &st), ==, 0);
		expected = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)st.st_ino);
		g_assert_cmpstr(recorded, ==, expected);
	}
}

/* Spec: Failed */
static void
test_failed_with_the_clients_reason(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autofree gchar *message = NULL;
	gint64 id = active_download(fx, &ref);
	AxisDownloadPollReport report;

	axis_test_download_fake_set(fx->fake, ref, "failed", 1600000000, NULL, "Out of disk space\n(at the client)");
	report = poll(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	assert_state(fx, id, "failed");
	message = q(fx, "SELECT message FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(message, ==, "Out of disk space (at the client)");
}

/* Task 5.3 / Spec: Client unreachable */
static void
test_client_down_fails_nothing(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autofree gchar *seen_before = NULL;
	g_autofree gchar *seen_after = NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 id = active_download(fx, &ref);
	guint i;

	seen_before = q(fx, "SELECT seen_at FROM download WHERE id = %" G_GINT64_FORMAT, id);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	for (i = 0; i < 4; i++) {
		report = poll(fx);
		g_assert_false(report.reached);
	}
	assert_state(fx, id, "downloading");
	assert_q(fx, "1600000000", "SELECT done_bytes FROM download");
	assert_q(fx, "0", "SELECT missing_polls FROM download");
	seen_after = q(fx, "SELECT seen_at FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(seen_before, ==, seen_after);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_UNREACHABLE);
	g_assert_true(g_str_has_prefix(client->state_message, "Not answering: "));
	/* Its age is what the page shows: the client's last contact and the
	 * record's last sighting stay where they were. */
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_cmpint(info->client_state, ==, AXIS_DOWNLOAD_CLIENT_UNREACHABLE);
	g_assert_cmpint(info->seen_us, >, 0);
	g_assert_cmpint(info->client_contact_us, >, 0);
	g_clear_pointer(&info, axis_download_info_free);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* Back: tracking goes on from where it was. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 2000000000, NULL, NULL);
	report = poll(fx);
	g_assert_true(report.reached);
	assert_q(fx, "2000000000", "SELECT done_bytes FROM download");
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_OK);
	g_assert_null(client->state_message);
}

static void
test_gone_after_several_polls(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	gint64 id = active_download(fx, &ref);
	AxisDownloadPollReport report;

	axis_test_download_fake_drop(fx->fake, ref);
	poll(fx);
	assert_state(fx, id, "downloading");
	poll(fx);
	assert_state(fx, id, "downloading");
	report = poll(fx);
	g_assert_cmpuint(report.gone, ==, 1);
	assert_state(fx, id, "gone");
	/* A record that is gone cannot be removed. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_downloads_remove(fx->downloads, id, FALSE, &error));
		g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	}
}

/* Stalled: a flag on a torrent that has not grown for stall_after, not a
 * state; growing again clears it. */
static void
test_stalled_is_a_flag(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	gint64 id = active_download(fx, &ref);

	poll(fx);
	assert_q(fx, "f", "SELECT stalled FROM download");
	/* No growth for longer than the default hour. */
	axis_test_db_exec(fx->t->db, "UPDATE download SET progress_at = now() - interval '2 hours'");
	poll(fx);
	assert_q(fx, "t", "SELECT stalled FROM download");
	assert_state(fx, id, "downloading");
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1700000000, NULL, NULL);
	poll(fx);
	assert_q(fx, "f", "SELECT stalled FROM download");
	assert_state(fx, id, "downloading");
}

/* Task 5.4 / Spec: Unmapped path; Outside every volume */
static void
test_path_mapping(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autofree gchar *outside = axis_test_storage_dir_new();
	g_autofree gchar *links = g_build_filename(fx->disk, "links", NULL);
	g_autofree gchar *link = g_build_filename(links, "Some.Film.2020.1080p", NULL);
	g_autofree gchar *real = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p", NULL);
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	gint64 id = active_download(fx, &ref);

	/* Unmapped: it waits, and the client shows the problem. */
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/Some.Film.2020.1080p", NULL);
	report = poll(fx);
	g_assert_cmpuint(report.waiting, ==, 1);
	g_assert_cmpuint(report.completed, ==, 0);
	assert_state(fx, id, "downloading");
	assert_q(fx, NULL, "SELECT rel_path FROM download");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "no path mapping"));
	g_assert_cmpstr(info->client_path, ==, "/data/downloads/Some.Film.2020.1080p");
	g_clear_pointer(&info, axis_download_info_free);
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_nonnull(client->problem);
	g_assert_nonnull(strstr(client->problem, "1 finished download cannot be imported yet"));
	g_assert_cmpuint(client->n_waiting, ==, 1);
	g_clear_pointer(&client, axis_download_client_info_free);

	/* '..' is refused, never resolved. */
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/../../etc", NULL);
	add_map(fx, "/data", "");
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "'..'"));
	g_clear_pointer(&info, axis_download_info_free);

	/* Through a symbolic link out of the volume: refused. */
	axis_test_storage_mkdir(links);
	g_assert_cmpint(symlink(outside, link), ==, 0);
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/links/Some.Film.2020.1080p", NULL);
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "symbolic link"));
	g_clear_pointer(&info, axis_download_info_free);

	/* Mapped to a place where nothing is: waits. */
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/Some.Film.2020.1080p", NULL);
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "nothing is at the mapped place"));
	g_clear_pointer(&info, axis_download_info_free);

	/* Beneath the volume, and there: accepted. */
	axis_test_storage_mkdir(real);
	report = poll(fx);
	g_assert_cmpuint(report.completed, ==, 1);
	assert_state(fx, id, "completed");
	assert_q(fx, "downloads/Some.Film.2020.1080p", "SELECT rel_path FROM download");
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_null(client->problem);
	g_assert_cmpuint(client->n_waiting, ==, 0);

	/* A test checks the latest finished path against the mappings. */
	{
		AxisDownloadTestResult result;

		g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
		g_assert_nonnull(strstr(result.note, "maps to a volume"));
		axis_download_test_result_clear(&result);
		g_assert_true(axis_downloads_remove_path_map(fx->downloads, fx->client_id, "/data", &error));
		g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, &result, &error));
		g_assert_nonnull(strstr(result.note, "does not map"));
		axis_download_test_result_clear(&result);
	}
	axis_test_storage_dir_free(g_steal_pointer(&outside));
}

/* A volume that is not there: the path maps, but the download waits. */
static void
test_mapped_to_a_volume_that_is_offline(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autofree gchar *real = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p", NULL);
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = active_download(fx, &ref);

	axis_test_storage_mkdir(real);
	add_map(fx, "/data", "");
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/Some.Film.2020.1080p", NULL);
	poll(fx);
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "is not online"));
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	poll(fx);
	assert_state(fx, id, "completed");
}

/* Spec (downloaders): a path a client gives is never trimmed or changed into
 * another; one Axis cannot take exactly as it is is refused (finding 5 of
 * the stage 2 bug hunt: "<folder> " was recorded at "<folder>", another
 * folder beside it, which the import would have taken its files from).  The
 * download waits, saying why; so does the client's folder for Axis. */
static void
test_client_path_never_trimmed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autofree gchar *theirs = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p ", NULL);
	g_autofree gchar *other = g_build_filename(fx->disk, "downloads", "Some.Film.2020.1080p", NULL);
	g_autofree gchar *rel = NULL;
	g_autofree gchar *folder = NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	id = active_download(fx, &ref);
	/* The download's own folder ends in a space; beside it, another folder of
	 * the same name without it. */
	axis_test_storage_mkdir(theirs);
	axis_test_storage_mkdir(other);
	add_map(fx, "/data", "");
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/Some.Film.2020.1080p ", NULL);
	poll(fx);
	rel = q(fx, "SELECT coalesce(rel_path, '(none)') FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(rel, ==, "(none)");
	assert_state(fx, id, "downloading");
	g_assert_true(axis_downloads_get(fx->downloads, id, &info, &error));
	g_assert_nonnull(strstr(info->problem, "white space"));
	g_clear_pointer(&info, axis_download_info_free);
	/* Nor at its start. */
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, " /data/downloads/Some.Film.2020.1080p", NULL);
	poll(fx);
	assert_state(fx, id, "downloading");
	/* Several slashes, or one at the end, are the one path they mean. */
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data//downloads/Some.Film.2020.1080p/", NULL);
	poll(fx);
	assert_state(fx, id, "completed");
	g_clear_pointer(&rel, g_free);
	rel = q(fx, "SELECT rel_path FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(rel, ==, "downloads/Some.Film.2020.1080p");
	/* The client's folder for Axis: the same. */
	axis_test_download_fake_set_category_dir(fx->fake, "/data/downloads ");
	g_assert_false(axis_downloads_category_folder(fx->downloads, fx->client_id, &folder, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_null(folder);
}

/* Spec (downloaders): an item's name is shown cleaned, and kept exactly as
 * the client gave it for what is compared with names on disk (finding 5 of
 * the stage 2 bug hunt: the cleaned name, white space collapsed, was what the
 * Leftovers page and a folder's removal compared). */
static void
test_item_own_name(void)
{
	AxisDownloadItem *item = axis_download_item_new();

	item->client_ref = g_strdup("ref1");
	item->category = g_strdup("axis");
	item->name = g_strdup("Some  Film 2020 ");
	g_assert_true(axis_download_item_check(item));
	g_assert_cmpstr(item->name, ==, "Some Film 2020");
	g_assert_cmpstr(item->own_name, ==, "Some  Film 2020 ");
	axis_download_item_free(item);
	/* One that cannot be kept exactly has none. */
	item = axis_download_item_new();
	item->client_ref = g_strdup("ref1");
	item->category = g_strdup("axis");
	item->name = g_strdup("Some\tFilm");
	g_assert_true(axis_download_item_check(item));
	g_assert_null(item->own_name);
	axis_download_item_free(item);
	/* A kind's raw text is what is kept, not the name it cleaned already
	 * (review of the fixes, item 2: every kind cleaned the name it read, so
	 * the name kept was never the client's). */
	item = axis_download_item_new();
	item->client_ref = g_strdup("ref1");
	item->category = g_strdup("axis");
	item->name = g_strdup("Some Film 2020");
	item->own_name = g_strdup("Some  Film 2020 ");
	g_assert_true(axis_download_item_check(item));
	g_assert_cmpstr(item->name, ==, "Some Film 2020");
	g_assert_cmpstr(item->own_name, ==, "Some  Film 2020 ");
	axis_download_item_free(item);
	/* And a raw text that cannot be kept exactly is dropped, not cleaned. */
	item = axis_download_item_new();
	item->client_ref = g_strdup("ref1");
	item->category = g_strdup("axis");
	item->name = g_strdup("Some Film");
	item->own_name = g_strdup("Some\nFilm");
	g_assert_true(axis_download_item_check(item));
	g_assert_null(item->own_name);
	axis_download_item_free(item);
	/* An entry read by its identifier only is never used. */
	item = axis_download_item_new();
	item->client_ref = g_strdup("ref1");
	item->category = g_strdup("axis");
	item->name = g_strdup("Some Film");
	item->unreadable = g_strdup("it has no name Axis can read");
	g_assert_false(axis_download_item_check(item));
	axis_download_item_free(item);
}

static void
test_path_rules(void)
{
	g_autoptr(GPtrArray) maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);
	g_autofree gchar *rel = NULL;
	gint64 volume = 0;
	const gchar *problem = NULL;

	g_ptr_array_add(maps, axis_download_path_map_new("/downloads", 1, "dl"));
	g_ptr_array_add(maps, axis_download_path_map_new("/downloads/tv", 2, ""));
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/films/A", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_MAPPED);
	g_assert_cmpint(volume, ==, 1);
	g_assert_cmpstr(rel, ==, "dl/films/A");
	g_clear_pointer(&rel, g_free);
	/* The longest prefix wins, by whole segments. */
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/tv/B//", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_MAPPED);
	g_assert_cmpint(volume, ==, 2);
	g_assert_cmpstr(rel, ==, "B");
	g_clear_pointer(&rel, g_free);
	g_assert_cmpint(axis_download_path_apply(maps, "/downloadsX/A", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_UNMAPPED);
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/tv", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/./A", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_cmpint(axis_download_path_apply(maps, "downloads/A", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/a\nb", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	/* White space at either end is never trimmed away: refused. */
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/films/A ", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_cmpint(axis_download_path_apply(maps, "/downloads/films/A\t", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_cmpint(axis_download_path_apply(maps, " /downloads/films/A", &volume, &rel, &problem), ==,
		AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_null(rel);
	g_assert_cmpint(axis_download_path_apply(maps, NULL, &volume, &rel, &problem), ==, AXIS_DOWNLOAD_PATH_INVALID);
	g_assert_null(rel);
	g_assert_false(axis_download_path_map_normalize("relative", "", NULL, NULL, &problem));
	g_assert_false(axis_download_path_map_normalize("/ok", "/absolute", NULL, NULL, &problem));
	g_assert_false(axis_download_path_map_normalize("/ok", "a/../b", NULL, NULL, &problem));
	g_assert_true(axis_download_path_map_normalize("/ok//", "a/b/", NULL, NULL, &problem));
}

/* ── Secrets and redirects (5.5) ────────────────────────────────── */

/* Spec: Redirect; secrets and the session reach only the client */
static void
test_secrets_stay_with_their_client(Fixture *fx, gconstpointer data)
{
	AxisTestDownloadFake *other = axis_test_download_fake_new("other-password");
	g_autofree gchar *other_base = axis_test_download_fake_base(other);
	g_autofree gchar *elsewhere = g_strconcat(other_base, "/api/list", NULL);
	g_autofree gchar *session = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *other_transcript = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autoptr(GError) error = NULL;
	AxisDownloadPollReport report;
	guint logins = 0;
	guint i;

	add_magnet(fx, MAGNET, "Some.Film.2020.1080p");
	poll(fx);
	session = axis_test_download_fake_session(fx->fake);
	g_assert_nonnull(session);

	/* The password only in login bodies, to the client's own address; the
	 * session only in Cookie headers there. */
	requests = axis_test_plex_requests(axis_test_download_fake_server(fx->fake));
	for (i = 0; i < requests->len; i++) {
		const gchar *one = g_ptr_array_index(requests, i);

		if (g_str_has_prefix(one, "POST /api/login\n")) {
			logins++;
			g_assert_nonnull(strstr(one, PASSWORD));
		} else {
			g_assert_null(strstr(one, PASSWORD));
		}
		/* No request line carries a query (where a secret could be). */
		g_assert_null(memchr(one, '?', strcspn(one, "\n")));
	}
	g_assert_cmpuint(logins, ==, 1);
	transcript = axis_test_plex_transcript(axis_test_download_fake_server(fx->fake));
	g_assert_nonnull(strstr(transcript, "Cookie: fx_session=" ));

	/* A redirect elsewhere: nothing is sent there, not even the session. */
	axis_test_download_fake_set_redirect(fx->fake, elsewhere);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_REDIRECT);
	report = poll(fx);
	g_assert_false(report.reached);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_download_fake_server(other)), ==, 0);
	other_transcript = axis_test_plex_transcript(axis_test_download_fake_server(other));
	g_assert_null(strstr(other_transcript, PASSWORD));
	g_assert_null(strstr(other_transcript, session));
	g_assert_true(axis_downloads_get_client(fx->downloads, fx->client_id, &client, &error));
	g_assert_cmpint(client->state, ==, AXIS_DOWNLOAD_CLIENT_FAILING);
	g_assert_nonnull(strstr(client->state_message, "redirect"));
	g_assert_null(strstr(client->state_message, session));

	/* Neither is in any table but the secret's own, nor in the log. */
	assert_q(fx, "0", "SELECT count(*) FROM download d WHERE to_jsonb(d)::text LIKE '%" PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM download_client c WHERE to_jsonb(c)::text LIKE '%" PASSWORD "%'");
	assert_q(fx, "0", "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%" PASSWORD "%'");
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM download_client c, download d"
			" WHERE to_jsonb(c)::text LIKE '%%%s%%' OR to_jsonb(d)::text LIKE '%%%s%%'", session, session);

		assert_q(fx, "0", sql);
	}
	{
		g_autofree gchar *pattern = g_strdup_printf("*%s*", session);

		g_assert_cmpuint(logged(pattern), ==, 0);
		g_assert_cmpuint(logged("*" PASSWORD "*"), ==, 0);
	}
	axis_test_download_fake_free(other);
}

/* An expired session: the kind logs in again, once. */
static void
test_expired_session_logs_in_again(Fixture *fx, gconstpointer data)
{
	AxisDownloadPollReport report;

	poll(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "login", NULL), ==, 1);
	poll(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "login", NULL), ==, 1);
	axis_test_download_fake_expire_session(fx->fake);
	report = poll(fx);
	g_assert_true(report.reached);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "login", NULL), ==, 2);
}

/* ── The tracking loop ──────────────────────────────────────────── */

static void
test_tracking_polls_enabled_clients(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;
	gint64 off;

	off = add_client(fx, "Off", "fixturenzb", base, PASSWORD);
	memset(&spec, 0, sizeof(spec));
	spec.name = "Off";
	spec.address = base;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	spec.enabled = FALSE;
	g_assert_true(axis_downloads_save_client(fx->downloads, off, &spec, NULL, &error));
	g_assert_true(axis_downloads_run_tracking(fx->downloads, NULL, 0, &error));
	g_assert_no_error(error);
	assert_q(fx, "ok", "SELECT state FROM download_client WHERE name = 'Torrents'");
	assert_q(fx, "unchecked", "SELECT state FROM download_client WHERE name = 'Off'");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "list", NULL), ==, 1);
}

/* Spec (downloaders): a client switched off is sent no new download, and the
 * downloads it has already are still followed to the end; once none is in
 * progress, it is not polled any more. */
static void
test_tracking_follows_a_switched_off_client(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *ref = NULL;
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "complete", "Lanternfold.Harbour.2016.1080p", NULL);
	g_autofree gchar *file = g_build_filename(place, "film.mkv", NULL);
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;
	AxisDownloadRequest request;
	gint64 id = active_download(fx, &ref);
	guint lists;

	memset(&spec, 0, sizeof(spec));
	spec.name = "Torrents";
	spec.kind = "fixture";
	spec.address = base;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_HIGH;
	spec.enabled = FALSE;
	g_assert_true(axis_downloads_save_client(fx->downloads, fx->client_id, &spec, NULL, &error));
	g_assert_no_error(error);

	/* No new download goes to it. */
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = "magnet:?xt=urn:btih:89abcdef0123456789abcdef0123456789abcdef&dn=Quillmere.Station.2012";
	request.release_name = "Quillmere.Station.2012.720p";
	g_assert_false(axis_downloads_add(fx->downloads, &request, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);

	/* The one in progress moves on, and completes, as tracking finds. */
	axis_test_download_fake_set(fx->fake, ref, "downloading", 3200000000, NULL, NULL);
	lists = axis_test_download_fake_count(fx->fake, "list", NULL);
	g_assert_true(axis_downloads_run_tracking(fx->downloads, NULL, 0, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "list", NULL), ==, lists + 1);
	assert_q(fx, "3200000000", "SELECT done_bytes FROM download WHERE client_ref = 'fx-1'");
	axis_test_storage_mkdir(place);
	axis_test_storage_write(file, "film");
	add_map(fx, "/data/downloads", "downloads");
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000,
		"/data/downloads/complete/Lanternfold.Harbour.2016.1080p", NULL);
	g_assert_true(axis_downloads_run_tracking(fx->downloads, NULL, 0, &error));
	g_assert_no_error(error);
	assert_state(fx, id, "completed");

	/* Nothing in progress at it now: switched off, it is left alone. */
	lists = axis_test_download_fake_count(fx->fake, "list", NULL);
	g_assert_true(axis_downloads_run_tracking(fx->downloads, NULL, 0, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "list", NULL), ==, lists);
}

/* Spec (downloaders): before an import moves a torrent's files, its client
 * lets go of it: removed WITHOUT its data, then asked again until it says it
 * no longer has it.  A client that never says so, or cannot be reached, has
 * not let go; only a completed torrent is let go of so. */
static void
test_release_a_torrent(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *completed = NULL;
	g_autofree gchar *usenet = NULL;
	gboolean was_gone = TRUE;
	gint64 id = add_magnet(fx, MAGNET, "Some.Film.2020.1080p");

	ref = q(fx, "SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);
	completed = g_strdup_printf("UPDATE download SET state = 'completed', client_path = '/data/downloads/Some.Film.2020.1080p',"
		" volume_id = (SELECT id FROM volume ORDER BY id LIMIT 1), rel_path = 'downloads/Some.Film.2020.1080p',"
		" completed_at = now() WHERE id = %" G_GINT64_FORMAT, id);
	usenet = g_strdup_printf("UPDATE download SET protocol = 'usenet' WHERE id = %" G_GINT64_FORMAT, id);
	/* Not completed: refused, nothing asked. */
	g_assert_false(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	axis_test_db_exec(fx->t->db, completed);

	/* The client cannot be reached: not let go of. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	g_assert_false(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_nonnull(error);
	g_assert_false(g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE));
	g_clear_error(&error);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));

	/* Removed without its data, and a client that does not say it no longer
	 * has it: not taken to have let go. */
	g_assert_false(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);

	/* One that says so, as Deluge and qBittorrent do: let go of. */
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	axis_test_download_fake_put(fx->fake, ref, "Some.Film.2020.1080p", "axis", MAGNET_HASH, "completed", 1000, 1000,
		"/data/downloads/Some.Film.2020.1080p", NULL);
	g_assert_true(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_no_error(error);
	g_assert_false(was_gone);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	/* The record is not changed. */
	assert_state(fx, id, "completed");

	/* Already gone (something else removed it): let go of, and said so. */
	g_assert_true(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_no_error(error);
	g_assert_true(was_gone);

	/* A Usenet job is not let go of this way. */
	axis_test_db_exec(fx->t->db, usenet);
	g_assert_false(axis_downloads_release(fx->downloads, id, &was_gone, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
}

static void
test_delete_client(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ref = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = active_download(fx, &ref);

	g_assert_false(axis_downloads_delete_client(fx->downloads, fx->client_id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_BUSY);
	g_clear_error(&error);
	g_assert_true(axis_downloads_remove(fx->downloads, id, FALSE, &error));
	g_assert_true(axis_downloads_delete_client(fx->downloads, fx->client_id, &error));
	g_assert_no_error(error);
	assert_q(fx, "0", "SELECT count(*) FROM download_client_secret");
	/* The history stays, under the client's name. */
	assert_q(fx, "Torrents removed", "SELECT client_name || ' ' || state FROM download WHERE client_id IS NULL");
}

/* ── The bencode reader ─────────────────────────────────────────── */

static void
test_bencode(void)
{
	static const gchar torrent[] = "d4:infod4:name3:abc6:lengthi5eee";
	static const gchar *const bad[] = {
		"",
		"d4:infod4:name3:abce",		/* cut short */
		"d4:infoi1ee",			/* info is not a dictionary */
		"d4:infodee4:infodee",		/* two infos */
		"d4:infodeex",			/* trailing */
		"di1e4:infodee",			/* a key that is not a string */
		"d4:infod1:ai03eee",		/* leading zero */
		"d4:infod1:ai-0eee",		/* minus zero */
		"d4:infod1:a99999999999:xeee",	/* longer than left */
		"le",				/* not a dictionary */
		"d1:ai1ee",			/* no info */
		NULL
	};
	g_autofree gchar *hash = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *expected = g_compute_checksum_for_string(G_CHECKSUM_SHA1, "d4:name3:abc6:lengthi5ee", -1);
	GString *deep = g_string_new("d4:info");
	guint i;

	g_assert_true(axis_download_torrent_read((const guint8 *)torrent, strlen(torrent), &hash, &name, NULL));
	g_assert_cmpstr(hash, ==, expected);
	g_assert_cmpstr(name, ==, "abc");
	for (i = 0; bad[i] != NULL; i++) {
		const gchar *problem = NULL;

		if (axis_download_torrent_read((const guint8 *)bad[i], strlen(bad[i]), NULL, NULL, &problem))
			g_error("accepted: %s", bad[i]);
		g_assert_nonnull(problem);
	}
	/* Too deep. */
	for (i = 0; i < AXIS_BENCODE_MAX_DEPTH + 4; i++)
		g_string_append(deep, "l");
	for (i = 0; i < AXIS_BENCODE_MAX_DEPTH + 4; i++)
		g_string_append(deep, "e");
	g_string_append(deep, "e");
	g_assert_false(axis_download_torrent_read((const guint8 *)deep->str, deep->len, NULL, NULL, NULL));
	g_string_free(deep, TRUE);
	g_assert_true(axis_bencode_valid((const guint8 *)"i-12e", 5));
	g_assert_false(axis_bencode_valid((const guint8 *)"i12", 3));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/download/bencode", test_bencode);
	g_test_add_func("/download/path/rules", test_path_rules);
	g_test_add_func("/download/item/own-name", test_item_own_name);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/download/" path, Fixture, NULL, func)
	ADD("client/secret-kept-apart", test_client_secret_is_kept_apart);
	ADD("client/test", test_client_test);
	ADD("client/test-wrong-secret", test_client_test_with_a_wrong_secret);
	ADD("client/delete", test_delete_client);
	ADD("remove/release", test_release_a_torrent);
	ADD("add/magnet", test_add_a_magnet);
	ADD("add/torrent-file", test_add_a_torrent_file);
	ADD("add/refused-requests", test_add_refuses_what_cannot_be_sent);
	ADD("own/only-axis", test_only_axis_downloads_are_touched);
	ADD("once/crash-before-send", test_crash_before_sending_is_never_sent);
	ADD("once/crash-after-send", test_crash_after_sending_is_adopted);
	ADD("once/lost-answer", test_lost_answer_is_matched_by_name);
	ADD("once/refused-or-unsent", test_refused_and_unsent_are_failed);
	ADD("track/completed", test_completed_with_its_location);
	ADD("track/failed", test_failed_with_the_clients_reason);
	ADD("track/client-down", test_client_down_fails_nothing);
	ADD("track/gone", test_gone_after_several_polls);
	ADD("track/stalled", test_stalled_is_a_flag);
	ADD("track/loop", test_tracking_polls_enabled_clients);
	ADD("track/switched-off", test_tracking_follows_a_switched_off_client);
	ADD("path/mapping", test_path_mapping);
	ADD("path/offline-volume", test_mapped_to_a_volume_that_is_offline);
	ADD("path/never-trimmed", test_client_path_never_trimmed);
	ADD("secret/stays-with-client", test_secrets_stay_with_their_client);
	ADD("secret/session-again", test_expired_session_logs_in_again);
#undef ADD
	return g_test_run();
}
