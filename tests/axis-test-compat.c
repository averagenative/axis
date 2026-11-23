/*
 * axis-test-compat.c - test support for client compatibility
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-compat.h"

#include <string.h>

#include "axis-test-jobs.h"
#include "transcode/axis-estimator.h"

AxisTestCompat *
axis_test_compat_new(gboolean with_library)
{
	AxisTestCompat *t;
	g_autoptr(GError) error = NULL;
	AxisLibraryTuning tuning;

	axis_test_log_init();
	t = g_new0(AxisTestCompat, 1);
	t->lib = axis_test_library_new(10);
	if (t->lib == NULL) {
		g_free(t);
		return NULL;
	}
	memset(&tuning, 0, sizeof(tuning));
	tuning.racy_window_ns = 0;
	tuning.guard_interval_ms = 1;
	axis_library_set_tuning(t->lib->library, &tuning);

	t->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(t->settings, "estimate", axis_estimator_get_setting_table(), &error));
	g_assert_true(axis_settings_add_table(t->settings, AXIS_COMPAT_MODULE_ID, axis_compat_get_setting_table(), &error));
	g_assert_true(axis_settings_load(t->settings, NULL, &error));
	g_assert_no_error(error);
	t->estimator = axis_estimator_new(t->lib->pool, t->lib->storage, t->settings);
	t->compat = axis_compat_new(t->lib->pool, t->lib->storage, t->settings, t->estimator);
	t->plex = axis_test_plex_new();
	g_assert_nonnull(t->plex);
	axis_test_plex_standard(t->plex);

	if (with_library) {
		axis_test_savings_build(t->lib, &t->s);
		t->has_library = TRUE;
	}
	return t;
}

void
axis_test_jellyfin_standard(AxisTestPlex *server)
{
	const gchar *dir = "tests/fixtures/compat/jellyfin/recorded/";
	g_autofree gchar *identity = g_strconcat(dir, "identity.json", NULL);
	g_autofree gchar *libraries = g_strconcat(dir, "libraries.json", NULL);
	g_autofree gchar *devices = g_strconcat(dir, "devices-playing.json", NULL);
	g_autofree gchar *sessions = g_strconcat(dir, "sessions-playing.json", NULL);
	g_autofree gchar *items = g_strconcat(dir, "items.json", NULL);

	axis_test_plex_route_file(server, "/System/Info", -1, identity);
	axis_test_plex_route_file(server, "/Library/VirtualFolders", -1, libraries);
	axis_test_plex_route_file(server, "/Devices", -1, devices);
	axis_test_plex_route_file(server, "/Sessions", -1, sessions);
	axis_test_plex_route_file(server, "/Items", -1, items);
}

void
axis_test_compat_enable_jellyfin(AxisTestCompat *t)
{
	g_assert_null(t->jellyfin);
	t->jellyfin = axis_test_plex_new();
	g_assert_nonnull(t->jellyfin);
	axis_test_jellyfin_standard(t->jellyfin);
}

void
axis_test_compat_connect_jellyfin(AxisTestCompat *t)
{
	g_autofree gchar *base = axis_test_plex_base(t->jellyfin);
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, base, &error));
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_JELLYFIN_TOKEN, AXIS_TEST_COMPAT_JELLYFIN_KEY, &error));
	g_assert_no_error(error);
}

void
axis_test_compat_map_jellyfin_standard(AxisTestCompat *t)
{
	g_autofree gchar *films = g_build_filename(t->s.disk_a, "Films", NULL);
	g_autofree gchar *music = g_build_filename(t->s.disk_a, "Music", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *problem = NULL;

	g_assert_true(axis_compat_path_map_add_kind(t->compat, AXIS_COMPAT_KIND_JELLYFIN, "/media/films", films,
		&problem, &error));
	g_assert_true(axis_compat_path_map_add_kind(t->compat, AXIS_COMPAT_KIND_JELLYFIN, "/media/music", music,
		&problem, &error));
	g_assert_no_error(error);
}

void
axis_test_compat_free(AxisTestCompat *t)
{
	if (t == NULL)
		return;
	axis_test_plex_free(t->jellyfin);
	axis_test_plex_free(t->plex);
	axis_compat_unref(t->compat);
	axis_estimator_unref(t->estimator);
	axis_settings_unref(t->settings);
	if (t->has_library)
		axis_test_savings_clear(&t->s);
	axis_test_library_free(t->lib);
	g_free(t);
}

void
axis_test_compat_connect(AxisTestCompat *t)
{
	g_autofree gchar *base = axis_test_plex_base(t->plex);
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_SETTING_ADDRESS, base, &error));
	g_assert_true(axis_settings_set(t->settings, AXIS_COMPAT_SETTING_TOKEN, AXIS_TEST_COMPAT_TOKEN, &error));
	g_assert_no_error(error);
}

void
axis_test_compat_map(
	AxisTestCompat *t,
	const gchar    *server_path,
	const gchar    *axis_path
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *problem = NULL;

	g_assert_true(axis_compat_path_map_add(t->compat, server_path, axis_path, &problem, &error));
	g_assert_no_error(error);
}

void
axis_test_compat_map_standard(AxisTestCompat *t)
{
	g_autofree gchar *films = g_build_filename(t->s.disk_a, "Films", NULL);
	g_autofree gchar *shows = g_build_filename(t->s.disk_a, "Shows", NULL);
	g_autofree gchar *music = g_build_filename(t->s.disk_a, "Music", NULL);

	axis_test_compat_map(t, "/data/movies", films);
	axis_test_compat_map(t, "/data/tv", shows);
	axis_test_compat_map(t, "/data/music", music);
}

gint64
axis_test_compat_file_id(
	AxisTestCompat *t,
	gint64          root_id,
	const gchar    *rel
){
	g_autofree gchar *id = axis_test_db_queryf(t->lib->db,
		"SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'", root_id, rel);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

gchar *
axis_test_compat_fixture(
	const gchar *name,
	const gchar *from,
	const gchar *to
){
	g_autofree gchar *path = g_strconcat("tests/fixtures/compat/", name, NULL);
	gchar *text = NULL;
	GString *s;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	s = g_string_new(text);
	g_free(text);
	if (from != NULL)
		g_string_replace(s, from, to, 0);
	return g_string_free(s, FALSE);
}

/* ── Rows ───────────────────────────────────────────────────────── */

void
axis_test_compat_device(
	AxisTestCompat *t,
	const gchar    *client_id,
	const gchar    *name
){
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO compat_device (client_id, name) VALUES ('%s', '%s') ON CONFLICT DO NOTHING",
		client_id, name != NULL ? name : client_id);

	axis_test_db_exec(t->lib->db, sql);
}

void
axis_test_compat_play(
	AxisTestCompat *t,
	const gchar    *device,
	const gchar    *library_key,
	guint           age_days
){
	static guint serial;
	g_autofree gchar *sql = NULL;

	axis_test_compat_device(t, device, NULL);
	sql = g_strdup_printf(
		"INSERT INTO compat_play (device_id, library_key, rating_key, viewed_at) VALUES ('%s', '%s', 'r%u',"
		" now() - interval '%u days')", device, library_key, ++serial, age_days);
	axis_test_db_exec(t->lib->db, sql);
}

void
axis_test_compat_evidence(
	AxisTestCompat *t,
	const gchar    *device,
	const gchar    *library_key,
	gint            kind,
	const gchar    *codec,
	const gchar    *profile,
	gint            bit_depth,
	const gchar    *res_class,
	gint            hdr,
	const gchar    *container,
	const gchar    *effect,
	gboolean        remote,
	guint           age_days
){
	static guint serial;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *profile_sql = profile != NULL ? g_strdup_printf("'%s'", profile) : g_strdup("NULL");
	g_autofree gchar *res_sql = res_class != NULL ? g_strdup_printf("'%s'", res_class) : g_strdup("NULL");
	g_autofree gchar *container_sql = container != NULL ? g_strdup_printf("'%s'", container) : g_strdup("NULL");

	axis_test_compat_device(t, device, NULL);
	serial++;
	sql = g_strdup_printf(
		"INSERT INTO compat_session (server_id, session_key, rating_key, device_id, library_key, remote, first_seen,"
		" last_seen) VALUES ('test-server', 's%u', 'i%u', '%s', '%s', %s, now() - interval '%u days',"
		" now() - interval '%u days') RETURNING id", serial, serial, device, library_key, remote ? "true" : "false",
		age_days, age_days);
	id = axis_test_db_query(t->lib->db, sql);
	g_assert_nonnull(id);
	g_free(sql);
	sql = g_strdup_printf(
		"INSERT INTO compat_evidence (session_id, kind, codec, profile, bit_depth, res_class, hdr, container, effect)"
		" VALUES (%s, %d, '%s', %s, %d, %s, %d, %s, '%s')", id, kind, codec, profile_sql, bit_depth, res_sql, hdr,
		container_sql, effect);
	axis_test_db_exec(t->lib->db, sql);
}

static gchar *
sql_text(const gchar *text)
{
	return text != NULL ? g_strdup_printf("'%s'", text) : g_strdup("NULL");
}

void
axis_test_compat_device_of(
	AxisTestCompat *t,
	const gchar    *server_kind,
	const gchar    *client_id,
	const gchar    *name
){
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO compat_device (server_kind, client_id, name) VALUES ('%s', '%s', '%s') ON CONFLICT DO NOTHING",
		server_kind != NULL ? server_kind : "plex", client_id, name != NULL ? name : client_id);

	axis_test_db_exec(t->lib->db, sql);
}

void
axis_test_compat_play_of(
	AxisTestCompat *t,
	const gchar    *server_kind,
	const gchar    *device,
	const gchar    *library_key,
	guint           age_days
){
	static guint serial;
	g_autofree gchar *sql = NULL;

	axis_test_compat_device_of(t, server_kind, device, NULL);
	sql = g_strdup_printf(
		"INSERT INTO compat_play (server_kind, device_id, library_key, rating_key, viewed_at) VALUES ('%s', '%s', '%s',"
		" 'r%u', now() - interval '%u days')", server_kind != NULL ? server_kind : "plex", device, library_key,
		++serial, age_days);
	axis_test_db_exec(t->lib->db, sql);
}

void
axis_test_compat_add_evidence(
	AxisTestCompat         *t,
	const AxisTestEvidence *e
){
	static guint serial = 5000;
	const gchar *kind_name = e->server_kind != NULL ? e->server_kind : "plex";
	g_autofree gchar *id = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *library = sql_text(e->library_key);
	g_autofree gchar *profile = sql_text(e->profile);
	g_autofree gchar *res = sql_text(e->res_class);
	g_autofree gchar *container = sql_text(e->container);
	g_autofree gchar *stated = sql_text(e->stated_reason);

	axis_test_compat_device_of(t, kind_name, e->device, NULL);
	serial++;
	sql = g_strdup_printf(
		"INSERT INTO compat_session (server_kind, server_id, session_key, rating_key, device_id, library_key, remote,"
		" first_seen, last_seen) VALUES ('%s', 'test-server-%s', 's%u', 'i%u', '%s', %s, %s, now() - interval '%u days',"
		" now() - interval '%u days') RETURNING id", kind_name, kind_name, serial, serial, e->device, library,
		e->remote ? "true" : "false", e->age_days, e->age_days);
	id = axis_test_db_query(t->lib->db, sql);
	g_assert_nonnull(id);
	g_free(sql);
	sql = g_strdup_printf(
		"INSERT INTO compat_evidence (session_id, kind, codec, profile, bit_depth, res_class, hdr, container, effect,"
		" failed_property, stated_reason) VALUES (%s, %d, '%s', %s, %d, %s, %d, %s, '%s', '%s', %s)", id, e->kind,
		e->codec, profile, e->bit_depth, res, e->hdr, container, e->effect, e->property != NULL ? e->property : "",
		stated);
	axis_test_db_exec(t->lib->db, sql);
}

void
axis_test_compat_fact(
	AxisTestCompat *t,
	const gchar    *target,
	gint64          root_id,
	gint64          bytes,
	gint64          mid
){
	g_autofree gchar *snapshot = axis_test_db_query(t->lib->db,
		"SELECT coalesce((SELECT id FROM estimate_snapshot ORDER BY id DESC LIMIT 1), 0)");
	g_autofree gchar *sql = NULL;

	if (g_strcmp0(snapshot, "0") == 0) {
		g_free(snapshot);
		snapshot = axis_test_db_query(t->lib->db,
			"INSERT INTO estimate_snapshot (taken_at, trigger, took_ms, coverage, config)"
			" VALUES (now(), 'direct', 1, '{}', '{}') RETURNING id");
	}
	/* Facts are keyed by profile revision: the current one of @target. */
	sql = g_strdup_printf(
		"INSERT INTO estimate_fact (snapshot_id, revision_id, report, dim, paths, bytes, low, mid, high)"
		" VALUES (%s, (SELECT current_revision_id FROM conversion_profile WHERE slug = '%s'), 'root', '%"
		G_GINT64_FORMAT "', 1, %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")",
		snapshot, target, root_id, bytes, mid, mid, mid);
	axis_test_db_exec(t->lib->db, sql);
}

gint64
axis_test_compat_count(
	AxisTestCompat *t,
	const gchar    *table,
	const gchar    *where
){
	g_autofree gchar *count = axis_test_db_queryf(t->lib->db, "SELECT count(*) FROM %s WHERE %s", table,
		where != NULL ? where : "true");

	return g_ascii_strtoll(count, NULL, 10);
}

AxisCompatReport *
axis_test_compat_report(AxisTestCompat *t)
{
	AxisCompatReport *report = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_compute_report(t->compat, &report, &error));
	g_assert_no_error(error);
	return report;
}

const AxisCompatLibraryReport *
axis_test_compat_library(
	AxisCompatReport *report,
	const gchar      *key
){
	const AxisCompatLibraryReport *library = axis_compat_report_find_library(report, key);

	g_assert_nonnull(library);
	return library;
}

const AxisCompatTargetVerdict *
axis_test_compat_target(
	const AxisCompatLibraryReport *library,
	const gchar                   *target_id
){
	guint i;

	for (i = 0; i < library->targets->len; i++) {
		const AxisCompatTargetVerdict *tv = g_ptr_array_index(library->targets, i);

		if (strcmp(tv->target.id, target_id) == 0)
			return tv;
	}
	return NULL;
}
