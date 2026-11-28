/*
 * test-import-volume-notice.c - the notice for a download client whose
 * folder is on another disk than some library folders: one per client, not
 * one per pair; what it says by case (a Usenet client, a torrent client that
 * is removed after import, a torrent client left seeding); how many folders
 * it names
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test writes its rows by SQL.  Every name here is made up.
 */

#include <glib.h>
#include <stdarg.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "import/axis-import.h"

typedef struct
{
	AxisTestDb  *db;
	AxisDbPool  *pool;
	gint64       disk_a;
	gint64       disk_b;
	gint64       disk_c;
} Fixture;

static gint64
qi(Fixture *fx, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gint64
qi(Fixture *fx, const gchar *format, ...)
{
	g_autofree gchar *sql = NULL;
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	text = axis_test_db_query(fx->db, sql);
	return text != NULL ? g_ascii_strtoll(text, NULL, 10) : 0;
}

static gint64
add_disk(Fixture *fx, const gchar *uuid, const gchar *name)
{
	return qi(fx, "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at) VALUES ('%s', '%s',"
		" '/srv/%s', false, 'offline', now()) RETURNING id", uuid, name, uuid);
}

static void
add_root(Fixture *fx, gint64 disk, const gchar *type, const gchar *name)
{
	qi(fx, "INSERT INTO library_root (volume_id, rel_path, media_type, name) VALUES (%" G_GINT64_FORMAT
		", '%s', '%s', '%s') RETURNING id", disk, name, type, name);
}

static void
add_client(Fixture *fx, const gchar *name, const gchar *kind)
{
	gint64 id = qi(fx, "INSERT INTO download_client (name, kind, address) VALUES ('%s', '%s', 'http://made-up.test:1')"
		" RETURNING id", name, kind);

	qi(fx, "INSERT INTO download_path_map (client_id, client_prefix, volume_id, rel_path) VALUES (%" G_GINT64_FORMAT
		", '/data', %" G_GINT64_FORMAT ", '') RETURNING client_id", id, fx->disk_a);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 2);
	fx->disk_a = add_disk(fx, "5a5a5a5a-0001-4222-8333-444444444444", "Made-up disk A");
	fx->disk_b = add_disk(fx, "5a5a5a5a-0002-4222-8333-444444444444", "Made-up disk B");
	fx->disk_c = add_disk(fx, "5a5a5a5a-0003-4222-8333-444444444444", "Made-up disk C");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	if (fx->db != NULL)
		axis_test_db_free(fx->db);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

#define ADD(name, func) AXIS_TEST_ADD_DB_CASE("/import/volume-notice/" name, Fixture, NULL, func)

static GPtrArray *
warnings_of(Fixture *fx)
{
	GPtrArray *warnings = NULL;
	GError *error = NULL;

	g_assert_true(axis_import_volume_warnings(fx->pool, &warnings, &error));
	g_assert_no_error(error);
	return warnings;
}

static void
seed_on(Fixture *fx)
{
	axis_test_db_exec(fx->db, "INSERT INTO setting (key, value) VALUES ('import.torrents_after', '\"seed\"')");
}

/* Two clients and three folders on other disks make two notices, not six. */
static void
test_one_per_client(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) warnings = NULL;
	const AxisImportVolumeWarning *first;
	g_autofree gchar *text = NULL;

	add_root(fx, fx->disk_b, "movies", "Made-up films B");
	add_root(fx, fx->disk_c, "movies", "Made-up films C");
	add_root(fx, fx->disk_c, "movies", "Made-up films C2");
	add_root(fx, fx->disk_a, "movies", "Made-up films A");
	add_root(fx, fx->disk_b, "tv", "Made-up shows B");
	add_client(fx, "Made-up Newsbox", "sabnzbd");
	add_client(fx, "Made-up Torrentbox", "qbittorrent");
	warnings = warnings_of(fx);
	g_assert_cmpuint(warnings->len, ==, 2);
	first = g_ptr_array_index(warnings, 0);
	g_assert_cmpstr(first->client_name, ==, "Made-up Newsbox");
	g_assert_cmpuint(first->roots->len, ==, 3);
	text = axis_import_volume_warning_text(first);
	g_assert_nonnull(strstr(text, "Made-up Newsbox"));
	g_assert_nonnull(strstr(text, "disk Made-up disk A"));
	g_assert_nonnull(strstr(text, "Made-up films B (disk Made-up disk B)"));
	g_assert_nonnull(strstr(text, "Made-up films C2 (disk Made-up disk C)"));
	/* The folder on the client's own disk and the TV folder are not named. */
	g_assert_null(strstr(text, "Made-up films A"));
	g_assert_null(strstr(text, "shows"));
}

/* A Usenet client never seeds, even with seeding on: no word of seeding or
 * hardlinks, and information, not a warning. */
static void
test_usenet_text(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) warnings = NULL;
	g_autofree gchar *text = NULL;

	add_root(fx, fx->disk_b, "movies", "Made-up films B");
	add_client(fx, "Made-up Newsbox", "sabnzbd");
	seed_on(fx);
	warnings = warnings_of(fx);
	g_assert_cmpuint(warnings->len, ==, 1);
	g_assert_false(axis_import_volume_warning_is_costly(g_ptr_array_index(warnings, 0)));
	text = axis_import_volume_warning_text(g_ptr_array_index(warnings, 0));
	g_assert_nonnull(strstr(text, "copied instead of moved"));
	g_assert_nonnull(strstr(text, "needs no action"));
	g_assert_nonnull(strstr(text, "\"Download and library on different disks\""));
	g_assert_null(strstr(text, "seed"));
	g_assert_null(strstr(text, "hardlink"));
}

/* A torrent client that is removed after import (the default) costs only
 * the time of the copy. */
static void
test_torrent_removed_text(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) warnings = NULL;
	g_autofree gchar *text = NULL;

	add_root(fx, fx->disk_b, "movies", "Made-up films B");
	add_client(fx, "Made-up Torrentbox", "qbittorrent");
	warnings = warnings_of(fx);
	g_assert_cmpuint(warnings->len, ==, 1);
	g_assert_false(axis_import_volume_warning_is_costly(g_ptr_array_index(warnings, 0)));
	text = axis_import_volume_warning_text(g_ptr_array_index(warnings, 0));
	g_assert_nonnull(strstr(text, "needs no action"));
	g_assert_null(strstr(text, "seed"));
	g_assert_null(strstr(text, "hardlink"));
}

/* A torrent client left seeding keeps the point about space. */
static void
test_torrent_seeding_text(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) warnings = NULL;
	g_autofree gchar *text = NULL;

	add_root(fx, fx->disk_b, "movies", "Made-up films B");
	add_client(fx, "Made-up Torrentbox", "qbittorrent");
	seed_on(fx);
	warnings = warnings_of(fx);
	g_assert_cmpuint(warnings->len, ==, 1);
	g_assert_true(axis_import_volume_warning_is_costly(g_ptr_array_index(warnings, 0)));
	text = axis_import_volume_warning_text(g_ptr_array_index(warnings, 0));
	g_assert_nonnull(strstr(text, "as much space again while the torrent seeds"));
	g_assert_nonnull(strstr(text, "hardlink"));
	g_assert_nonnull(strstr(text, "Map the download client's folder"));
	g_assert_nonnull(strstr(text, "\"Download and library on different disks\""));
}

/* More folders than the notice names are counted. */
static void
test_many_folders(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) warnings = NULL;
	g_autofree gchar *text = NULL;
	guint i;

	for (i = 0; i < 8; i++) {
		g_autofree gchar *name = g_strdup_printf("Made-up films %u", i);

		add_root(fx, i % 2 == 0 ? fx->disk_b : fx->disk_c, "movies", name);
	}
	add_client(fx, "Made-up Newsbox", "sabnzbd");
	warnings = warnings_of(fx);
	g_assert_cmpuint(warnings->len, ==, 1);
	text = axis_import_volume_warning_text(g_ptr_array_index(warnings, 0));
	g_assert_nonnull(strstr(text, ", and 3 more."));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("one-per-client", test_one_per_client);
	ADD("usenet-text", test_usenet_text);
	ADD("torrent-removed-text", test_torrent_removed_text);
	ADD("torrent-seeding-text", test_torrent_seeding_text);
	ADD("many-folders", test_many_folders);
	return g_test_run();
}
