/*
 * test-library-scan.c - the scanner on generated trees: the index, names,
 * containment, offline volumes, missing files, moves, pinning, kinds,
 * killing a scan, one scan per volume, a hung walk, and a large tree
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test makes its own database and scratch directory.  Disks are
 * directories (see axis-test-library.h); test-library-mounts.c has the
 * cases that need real mounts.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <malloc.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "axis-test-case.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library.h"
#include "library/axis-library-name.h"

typedef struct
{
	AxisTestLibrary *t;
	gchar           *disk;
	gchar           *films;	/* the root's directory */
	gint64           volume;
	gint64           root;
	AxisTestGate    *gate;
	gchar           *hook_path;	/* the directory a hook acts on */
	gint             hook_calls;	/* atomic */
	gint             hook_at;
	gboolean         unplugged;
	GCancellable    *cancellable;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*was given up*");
	fx->gate = axis_test_gate_new();

	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Films", AXIS_MEDIA_MOVIES);
	fx->films = g_build_filename(fx->disk, "Films", NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_gate_open(fx->gate);
	if (fx->t != NULL && fx->unplugged)
		axis_test_library_plug(fx->t, fx->disk, "away");
	axis_test_library_free(fx->t);
	axis_test_gate_free(fx->gate);
	g_clear_object(&fx->cancellable);
	g_free(fx->disk);
	g_free(fx->films);
	g_free(fx->hook_path);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void
put(
	Fixture     *fx,
	const gchar *rel,
	const gchar *content
){
	g_autofree gchar *path = g_build_filename(fx->films, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);

	axis_test_storage_mkdir(dir);
	axis_test_storage_write(path, content);
}

static void
drop(
	Fixture     *fx,
	const gchar *rel
){
	g_autofree gchar *path = g_build_filename(fx->films, rel, NULL);

	g_assert_cmpint(g_unlink(path), ==, 0);
}

static gchar *
in_films(
	Fixture     *fx,
	const gchar *rel
){
	return g_build_filename(fx->films, rel, NULL);
}

static AxisScanResult *
scan(Fixture *fx)
{
	return axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
}

static gint64
n(
	Fixture     *fx,
	const gchar *where
){
	return axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT
		" AND (%s)", fx->root, where);
}

static gchar *
column(
	Fixture     *fx,
	const gchar *rel,
	const gchar *name
){
	return axis_test_library_q(fx->t, "SELECT %s::text FROM library_file WHERE root_id = %" G_GINT64_FORMAT
		" AND rel_path = '%s'", name, fx->root, rel);
}

static void
assert_column(
	Fixture     *fx,
	const gchar *rel,
	const gchar *name,
	const gchar *expected
){
	g_autofree gchar *value = column(fx, rel, name);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s of %s: expected %s, got %s", name, rel, expected != NULL ? expected : "NULL",
			value != NULL ? value : "NULL");
}

/* As the probe will: the content has been looked at. */
static void
clear_changed(Fixture *fx)
{
	g_autofree gchar *sql = g_strdup_printf("UPDATE library_file SET changed = false WHERE root_id = %"
		G_GINT64_FORMAT, fx->root);

	axis_test_db_exec(fx->t->db, sql);
}

static void
tune(
	Fixture           *fx,
	AxisLibraryTuning *tuning
){
	tuning->hook_data = fx;
	axis_test_library_tune(fx->t, tuning);
}

/* ── The index ──────────────────────────────────────────────────── */

static void
basic_tree(Fixture *fx)
{
	g_autofree gchar *empty = in_films(fx, "Empty");

	put(fx, "A (2001)/A.mkv", "aaaa");
	put(fx, "A (2001)/A.en.srt", "subs");
	put(fx, "B/Extras/x.mp4", "xxxxxxxx");
	put(fx, "B/poster.JPG", "jpg");
	put(fx, "notes.txt", "notes!");
	axis_test_storage_mkdir(empty);
}

/* Spec: New files are indexed */
static void
test_new_files(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) result = NULL;
	g_autofree gchar *rows = NULL;

	SKIP_WITHOUT_DB(fx);
	basic_tree(fx);

	result = scan(fx);
	g_assert_cmpuint(result->walk.files, ==, 5);
	g_assert_cmpuint(result->walk.dirs, ==, 4);
	g_assert_cmpuint(result->new_files, ==, 5);
	g_assert_cmpuint(result->present_after, ==, 5);
	rows = axis_test_library_q(fx->t, "SELECT string_agg(rel_path || ':' || size || ':' || kind, ','"
		" ORDER BY rel_path) FROM library_file WHERE root_id = %" G_GINT64_FORMAT, fx->root);
	g_assert_cmpstr(rows, ==, "A (2001)/A.en.srt:4:subtitle,A (2001)/A.mkv:4:video,"
		"B/Extras/x.mp4:8:video,B/poster.JPG:3:image,notes.txt:6:text");
	g_assert_cmpint(n(fx, "state = 'present' AND changed AND first_seen_generation = 1"
		" AND last_seen_generation = 1 AND NOT pinned AND nlink = 1"), ==, 5);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_scan WHERE root_id = %"
		G_GINT64_FORMAT " AND status = 'complete' AND files = 5", fx->root), ==, 1);
}

/* Spec: Unchanged files are not rewritten */
static void
test_unchanged(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	SKIP_WITHOUT_DB(fx);
	basic_tree(fx);
	first = scan(fx);
	clear_changed(fx);
	before = axis_test_library_index(fx->t, fx->root);

	second = scan(fx);
	after = axis_test_library_index(fx->t, fx->root);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpuint(second->new_files, ==, 0);
	g_assert_cmpuint(second->changed_files, ==, 0);
	g_assert_cmpuint(second->suspected, ==, 0);
	g_assert_cmpint(n(fx, "changed"), ==, 0);
	g_assert_cmpint(n(fx, "last_seen_generation = 2"), ==, 5);
}

/* Spec: Modified file is flagged */
static void
test_modified(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autofree gchar *poster = NULL;
	g_autofree gchar *nsec = NULL;
	struct stat st;

	SKIP_WITHOUT_DB(fx);
	basic_tree(fx);
	poster = in_films(fx, "B/poster.JPG");
	axis_test_set_mtime(poster, 1700000000, 123456789);
	first = scan(fx);
	clear_changed(fx);

	/* One grows; the other keeps its size and moves its time by 1 ns. */
	put(fx, "A (2001)/A.mkv", "aaaaaaaa");
	axis_test_set_mtime(poster, 1700000000, 123456790);
	g_assert_cmpint(stat(poster, &st), ==, 0);
	g_assert_cmpint(st.st_mtim.tv_nsec, ==, 123456790);

	second = scan(fx);
	g_assert_cmpuint(second->changed_files, ==, 2);
	g_assert_cmpuint(second->new_files, ==, 0);
	assert_column(fx, "A (2001)/A.mkv", "size", "8");
	assert_column(fx, "A (2001)/A.mkv", "changed", "true");
	assert_column(fx, "A (2001)/A.mkv", "changed_generation", "2");
	assert_column(fx, "B/poster.JPG", "changed", "true");
	nsec = column(fx, "B/poster.JPG", "mtime_nsec");
	g_assert_cmpstr(nsec, ==, "123456790");
	g_assert_cmpint(n(fx, "changed"), ==, 2);
}

/* A file looked at just after it was modified cannot later be proven
 * unchanged by an equal size and time: it counts as changed once more. */
static void
test_racy(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autoptr(AxisScanResult) third = NULL;
	g_autoptr(AxisScanResult) fourth = NULL;

	SKIP_WITHOUT_DB(fx);
	basic_tree(fx);

	memset(&tuning, 0, sizeof(tuning));
	tuning.racy_window_ns = G_GINT64_CONSTANT(3600) * 1000000000;
	tune(fx, &tuning);
	first = scan(fx);
	g_assert_cmpint(n(fx, "stat_racy"), ==, 5);
	clear_changed(fx);

	/* Nothing differs, but nothing is proven either. */
	second = scan(fx);
	g_assert_cmpuint(second->changed_files, ==, 5);
	clear_changed(fx);

	/* Looked at well after the modification: the last racy look still
	 * counts once, then the file is settled. */
	memset(&tuning, 0, sizeof(tuning));
	tune(fx, &tuning);
	third = scan(fx);
	g_assert_cmpuint(third->changed_files, ==, 5);
	g_assert_cmpint(n(fx, "stat_racy"), ==, 0);
	clear_changed(fx);
	fourth = scan(fx);
	g_assert_cmpuint(fourth->changed_files, ==, 0);
}

/* ── Containment ────────────────────────────────────────────────── */

/* Spec: Symbolic link to elsewhere */
static void
test_symlink_out(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) result = NULL;
	g_autofree gchar *outside = g_build_filename(fx->t->dir, "outside", NULL);
	g_autofree gchar *secret = g_build_filename(outside, "secret.mkv", NULL);
	g_autofree gchar *link = in_films(fx, "elsewhere");
	g_autofree gchar *file_link = in_films(fx, "secret-link.mkv");
	g_autofree gchar *target = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(outside);
	axis_test_storage_write(secret, "not ours");
	put(fx, "ours.mkv", "ours");
	g_assert_cmpint(symlink(outside, link), ==, 0);
	g_assert_cmpint(symlink(secret, file_link), ==, 0);

	result = scan(fx);
	g_assert_cmpuint(result->walk.symlinks, ==, 2);
	g_assert_cmpuint(result->walk.files, ==, 1);
	assert_column(fx, "elsewhere", "kind", "symlink");
	assert_column(fx, "secret-link.mkv", "kind", "symlink");
	target = column(fx, "elsewhere", "link_target");
	g_assert_cmpstr(target, ==, outside);
	g_assert_cmpint(n(fx, "rel_path LIKE '%secret.mkv%' AND kind <> 'symlink'"), ==, 0);
	g_assert_cmpint(n(fx, "true"), ==, 3);
}

/* Spec: Link loop */
static void
test_link_loop(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) result = NULL;
	g_autofree gchar *one = in_films(fx, "loop1");
	g_autofree gchar *two = in_films(fx, "loop2");
	g_autofree gchar *self = in_films(fx, "sub/self");
	g_autofree gchar *up = in_films(fx, "sub/up");

	SKIP_WITHOUT_DB(fx);
	put(fx, "sub/film.mkv", "film");
	g_assert_cmpint(symlink("loop2", one), ==, 0);
	g_assert_cmpint(symlink("loop1", two), ==, 0);
	g_assert_cmpint(symlink(".", self), ==, 0);
	g_assert_cmpint(symlink("../..", up), ==, 0);

	result = scan(fx);
	g_assert_cmpuint(result->walk.symlinks, ==, 4);
	g_assert_cmpuint(result->walk.files, ==, 1);
	g_assert_cmpuint(result->walk.dirs, ==, 1);
}

static void
swap_hook(
	const gchar *path,
	gpointer     data
){
	Fixture *fx = data;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *aside = NULL;
	g_autofree gchar *outside = NULL;

	if (g_strcmp0(path, fx->hook_path) != 0 || g_atomic_int_add(&fx->hook_calls, 1) != 0)
		return;

	/* Between the listing and the opening: the directory becomes a link
	 * to somewhere else, or another directory takes its place. */
	dir = in_films(fx, path);
	aside = g_strconcat(dir, ".aside", NULL);
	outside = g_build_filename(fx->t->dir, "outside", NULL);
	g_assert_cmpint(g_rename(dir, aside), ==, 0);
	if (fx->hook_at == 0) {
		g_assert_cmpint(symlink(outside, dir), ==, 0);
	} else {
		g_autofree gchar *other = in_films(fx, "Other");

		g_assert_cmpint(g_rename(other, dir), ==, 0);
	}
}

/* A directory replaced between being listed and being opened is skipped,
 * and what was recorded under it is not taken for gone. */
static void
test_replaced_directory(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autoptr(AxisScanResult) third = NULL;
	g_autofree gchar *outside = g_build_filename(fx->t->dir, "outside", NULL);
	g_autofree gchar *link = in_films(fx, "Swap");
	g_autofree gchar *aside = in_films(fx, "Swap.aside");
	g_autofree gchar *bait = g_build_filename(outside, "bait.mkv", NULL);
	g_autofree gchar *finding = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(outside);
	axis_test_storage_write(bait, "outside the root");
	put(fx, "Swap/inside.mkv", "inside");
	put(fx, "Other/other.mkv", "other");
	put(fx, "kept.mkv", "kept");
	first = scan(fx);

	/* A symbolic link put in its place. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.before_directory = swap_hook;
	fx->hook_path = g_strdup("Swap");
	tune(fx, &tuning);
	second = scan(fx);
	g_assert_cmpuint(second->walk.replaced, ==, 1);
	g_assert_cmpuint(second->suspected, ==, 0);
	assert_column(fx, "Swap/inside.mkv", "state", "present");
	g_assert_cmpint(n(fx, "rel_path LIKE '%bait%'"), ==, 0);
	finding = axis_test_library_q(fx->t, "SELECT kind || ':' || rel_path FROM library_scan_finding"
		" WHERE scan_id = %" G_GINT64_FORMAT, second->scan_id);
	g_assert_cmpstr(finding, ==, "replaced:Swap");

	/* Another directory moved into its place. */
	g_assert_cmpint(g_unlink(link), ==, 0);
	g_assert_cmpint(g_rename(aside, link), ==, 0);
	g_atomic_int_set(&fx->hook_calls, 0);
	fx->hook_at = 1;
	third = scan(fx);
	g_assert_cmpuint(third->walk.replaced, ==, 1);
	assert_column(fx, "Swap/inside.mkv", "state", "present");
}

static void
move_sibling_hook(
	const gchar *path,
	gpointer     data
){
	Fixture *fx = data;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	const gchar *other;

	if (g_atomic_int_add(&fx->hook_calls, 1) != 0)
		return;
	/* Whichever of the two is opened first, the other (already listed)
	 * moves into it before it is looked at. */
	other = strcmp(path, "One") == 0 ? "Two" : "One";
	from = in_films(fx, other);
	to = g_strdup_printf("%s/%s/%s-moved", fx->films, path, other);
	g_assert_cmpint(g_rename(from, to), ==, 0);
	fx->hook_path = g_strdup(other);
}

/* A folder that goes away between being listed and being looked at is not
 * examined: nothing recorded under it is taken for gone. */
static void
test_vanished_directory(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *kept = NULL;

	SKIP_WITHOUT_DB(fx);
	put(fx, "One/a.mkv", "a");
	put(fx, "Two/b.mkv", "b");
	first = scan(fx);

	memset(&tuning, 0, sizeof(tuning));
	tuning.before_directory = move_sibling_hook;
	tune(fx, &tuning);
	second = scan(fx);
	g_assert_nonnull(fx->hook_path);
	g_assert_cmpuint(second->walk.vanished, ==, 1);
	g_assert_cmpuint(second->suspected, ==, 0);
	where = g_strdup_printf("rel_path LIKE '%s/%%' AND state = 'present' AND unexamined_generation = 2",
		fx->hook_path);
	g_assert_cmpint(n(fx, where), ==, 1);
	kept = axis_test_library_q(fx->t, "SELECT kind || ':' || rel_path FROM library_scan_finding"
		" WHERE scan_id = %" G_GINT64_FORMAT, second->scan_id);
	g_assert_cmpstr(kept, ==, g_strcmp0(fx->hook_path, "One") == 0 ? "vanished:One" : "vanished:Two");
}

/* ── Names ──────────────────────────────────────────────────────── */

static const gchar *const hostile_names[] = {
	"Film.MKV",
	"film.mkv",
	"caf\xc3\xa9.mkv",
	"cafe\xcc\x81.mkv",
	"new\nline.mkv",
	"quote\"s 'and' ;semi;colon.mkv",
	"<script>alert(1)<\\script>.mkv",
	"<img src=x onerror=alert(1)>",
	"-leading-dash.mkv",
	"--",
	" spaced ",
	"back\\slash",
	"\\xFF",
	"\xff",
	"latin1 caf\xe9.mkv",
	"\x80\x81\xfe",
	"\x01\x1b[31mred",
	"\xe2\x80\xae" "fdp.mkv",
	"percent%_under",
	NULL
};

/* The bytes of every path in the root's index, from its stored form. */
static GHashTable *
indexed_paths(
	Fixture *fx,
	guint   *n_invalid
){
	GHashTable *paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *hex = axis_test_library_q(fx->t,
		"SELECT string_agg(encode(convert_to(rel_path, 'UTF8'), 'hex'), ',') FROM library_file"
		" WHERE root_id = %" G_GINT64_FORMAT, fx->root);
	g_auto(GStrv) items = g_strsplit(hex != NULL ? hex : "", ",", -1);
	guint i;

	*n_invalid = 0;
	for (i = 0; items[i] != NULL && items[i][0] != '\0'; i++) {
		gsize length = strlen(items[i]) / 2;
		gchar *stored = g_malloc0(length + 1);
		GString *bytes = NULL;
		gsize k;

		for (k = 0; k < length; k++)
			stored[k] = (gchar)((g_ascii_xdigit_value(items[i][2 * k]) << 4) |
				g_ascii_xdigit_value(items[i][2 * k + 1]));
		g_assert_true(g_utf8_validate(stored, -1, NULL));
		if (!axis_library_name_decode(stored, &bytes))
			g_error("not a canonical stored form: %s", stored);
		if (axis_library_name_is_invalid(stored))
			(*n_invalid)++;
		g_hash_table_add(paths, g_string_free(bytes, FALSE));
		g_free(stored);
	}

	return paths;
}

/* Spec: Non-UTF-8 name; Hostile-looking name (stored verbatim) */
static void
test_names(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autoptr(GHashTable) paths = NULL;
	g_autofree gchar *odd_dir = in_films(fx, "dir\xfe");
	guint n_invalid = 0;
	guint expected_invalid = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(odd_dir);
	for (i = 0; hostile_names[i] != NULL; i++) {
		axis_test_write_named(fx->films, hostile_names[i], hostile_names[i]);
		axis_test_write_named(odd_dir, hostile_names[i], "in an odd folder");
		if (!g_utf8_validate(hostile_names[i], -1, NULL))
			expected_invalid++;
	}
	expected_invalid += i;	/* everything in "dir\xfe" */

	first = scan(fx);
	g_assert_cmpuint(first->new_files, ==, 2 * i);

	paths = indexed_paths(fx, &n_invalid);
	g_assert_cmpuint(g_hash_table_size(paths), ==, 2 * i);
	g_assert_cmpuint(n_invalid, ==, expected_invalid);
	g_assert_cmpint(n(fx, "name_invalid_utf8"), ==, expected_invalid);
	for (i = 0; hostile_names[i] != NULL; i++) {
		g_autofree gchar *nested = g_strconcat("dir\xfe/", hostile_names[i], NULL);

		if (!g_hash_table_contains(paths, hostile_names[i]))
			g_error("name %u is not indexed as it is", i);
		g_assert_true(g_hash_table_contains(paths, nested));
	}

	/* Found again exactly as stored. */
	second = scan(fx);
	g_assert_cmpuint(second->new_files, ==, 0);
	g_assert_cmpuint(second->changed_files, ==, 0);
	g_assert_cmpuint(second->suspected, ==, 0);
}

/* ── Offline ────────────────────────────────────────────────────── */

static void
many_files(
	Fixture *fx,
	guint    dirs,
	guint    files
){
	guint i;

	for (i = 0; i < dirs; i++) {
		g_autofree gchar *dir = g_strdup_printf("%s/d%02u", fx->films, i);

		axis_test_make_files(dir, files);
	}
}

/* Spec: Volume offline before the scan */
static void
test_offline_before(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) offline = NULL;
	g_autoptr(AxisScanResult) back = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *status = NULL;

	SKIP_WITHOUT_DB(fx);
	many_files(fx, 3, 4);
	first = scan(fx);
	before = axis_test_library_index(fx->t, fx->root);

	axis_test_library_unplug(fx->t, fx->disk, "away");
	fx->unplugged = TRUE;
	offline = axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_UNAVAILABLE,
		AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);
	after = axis_test_library_index(fx->t, fx->root);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(n(fx, "state <> 'present'"), ==, 0);
	status = axis_test_library_q(fx->t, "SELECT status || '/' || reason FROM library_scan WHERE id = %"
		G_GINT64_FORMAT, offline->scan_id);
	g_assert_cmpstr(status, ==, "unavailable/volume_unavailable");

	axis_test_library_plug(fx->t, fx->disk, "away");
	fx->unplugged = FALSE;
	back = scan(fx);
	g_assert_cmpuint(back->suspected, ==, 0);
	g_assert_cmpuint(back->present_after, ==, 12);
}

static void
unplug_hook(
	const gchar *path,
	gpointer     data
){
	Fixture *fx = data;

	if (fx->unplugged || (fx->hook_path != NULL && g_strcmp0(path, fx->hook_path) != 0))
		return;
	if (g_atomic_int_add(&fx->hook_calls, 1) < fx->hook_at)
		return;
	axis_test_library_unplug(fx->t, fx->disk, "away");
	fx->unplugged = TRUE;
}

static void
unplug_in_finish(
	const gchar *stage,
	guint        batch,
	gpointer     data
){
	Fixture *fx = data;

	if (strcmp(stage, "finish") == 0 && !fx->unplugged) {
		axis_test_library_unplug(fx->t, fx->disk, "away");
		fx->unplugged = TRUE;
	}
}

/* Spec: Volume disappears mid-scan: while walking, right after the last
 * directory, and inside the final transaction. */
static void
test_offline_mid_scan(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	g_autoptr(AxisScanResult) first = NULL;
	guint round;

	SKIP_WITHOUT_DB(fx);
	many_files(fx, 12, 5);
	first = scan(fx);

	/* What a complete scan would conclude is gone. */
	drop(fx, "d03/f1");
	drop(fx, "d07/f2");
	drop(fx, "d11/f4");

	for (round = 0; round < 3; round++) {
		g_autoptr(AxisScanResult) result = NULL;

		memset(&tuning, 0, sizeof(tuning));
		tuning.batch_entries = 5;
		if (round == 0) {
			tuning.before_directory = unplug_hook;
			fx->hook_at = 4;
		} else if (round == 1) {
			tuning.after_last = unplug_hook;
			fx->hook_at = 0;
		} else {
			tuning.before_commit = unplug_in_finish;
		}
		g_atomic_int_set(&fx->hook_calls, 0);
		tune(fx, &tuning);

		result = axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_UNAVAILABLE,
			AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);
		g_assert_true(fx->unplugged);
		g_assert_cmpuint(result->suspected, ==, 0);
		g_assert_cmpint(n(fx, "state <> 'present'"), ==, 0);
		g_assert_cmpint(n(fx, "true"), ==, 60);
		g_assert_cmpint(axis_test_library_n(fx->t, "SELECT complete_generation FROM library_root_state"
			" WHERE root_id = %" G_GINT64_FORMAT, fx->root), ==, 1);

		axis_test_library_plug(fx->t, fx->disk, "away");
		fx->unplugged = FALSE;
	}

	/* Back, and complete: now the three are suspected. */
	memset(&tuning, 0, sizeof(tuning));
	tune(fx, &tuning);
	{
		g_autoptr(AxisScanResult) back = scan(fx);

		g_assert_cmpuint(back->suspected, ==, 3);
	}
}

/* Spec: Unreadable directory */
static void
test_unreadable(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autofree gchar *locked = in_films(fx, "Locked");
	g_autofree gchar *listed = in_films(fx, "Listed");
	g_autofree gchar *findings = NULL;

	SKIP_WITHOUT_DB(fx);
	if (geteuid() == 0) {
		g_test_skip("permissions do not stop root");
		return;
	}
	put(fx, "Locked/a.mkv", "a");
	put(fx, "Locked/deeper/a2.mkv", "a2");
	put(fx, "Listed/b.mkv", "b");
	put(fx, "Open/c.mkv", "c");
	put(fx, "gone.mkv", "gone");
	first = scan(fx);

	g_assert_cmpint(chmod(locked, 0), ==, 0);
	/* Can be listed, but its entries cannot be looked at. */
	g_assert_cmpint(chmod(listed, 0444), ==, 0);
	drop(fx, "gone.mkv");

	second = scan(fx);
	g_assert_cmpuint(second->walk.unreadable, ==, 2);
	g_assert_cmpuint(second->suspected, ==, 1);
	assert_column(fx, "gone.mkv", "state", "suspected");
	assert_column(fx, "Locked/a.mkv", "state", "present");
	assert_column(fx, "Locked/deeper/a2.mkv", "state", "present");
	assert_column(fx, "Listed/b.mkv", "state", "present");
	assert_column(fx, "Locked/a.mkv", "unexamined_generation", "2");
	findings = axis_test_library_q(fx->t, "SELECT string_agg(kind || ':' || rel_path, ',' ORDER BY rel_path)"
		" FROM library_scan_finding WHERE scan_id = %" G_GINT64_FORMAT, second->scan_id);
	g_assert_cmpstr(findings, ==, "unreadable:Listed,unreadable:Locked");

	g_assert_cmpint(chmod(locked, 0755), ==, 0);
	g_assert_cmpint(chmod(listed, 0755), ==, 0);
}

/* ── Missing ────────────────────────────────────────────────────── */

/* As if an hour had passed since @rel was suspected, by both clocks. */
static void
age_suspicion(
	Fixture     *fx,
	const gchar *rel,
	gboolean     database_clock,
	gboolean     boot_clock
){
	g_autofree gchar *sql = g_strdup_printf("UPDATE library_file SET"
		" suspected_at = suspected_at - interval '%s',"
		" suspected_boottime_us = suspected_boottime_us - %s"
		" WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'",
		database_clock ? "2 hours" : "0", boot_clock ? "7200000000" : "0", fx->root, rel);

	axis_test_db_exec(fx->t->db, sql);
}

/* Spec: First absence; Second absence; File reappears */
static void
test_missing(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *id = NULL;
	g_autofree gchar *size = NULL;
	g_autofree gchar *again = NULL;
	AxisScanResult *result;

	SKIP_WITHOUT_DB(fx);
	put(fx, "A.mkv", "aaaa");
	put(fx, "B.mkv", "bbbbbb");
	axis_scan_result_free(scan(fx));
	id = column(fx, "B.mkv", "id");

	/* First absence: suspected, otherwise intact. */
	drop(fx, "B.mkv");
	result = scan(fx);
	g_assert_cmpuint(result->suspected, ==, 1);
	axis_scan_result_free(result);
	assert_column(fx, "B.mkv", "state", "suspected");
	assert_column(fx, "B.mkv", "size", "6");
	assert_column(fx, "B.mkv", "id", id);
	assert_column(fx, "A.mkv", "state", "present");

	/* Absent again, but less than an hour later: still only suspected. */
	result = scan(fx);
	g_assert_cmpuint(result->confirmed, ==, 0);
	axis_scan_result_free(result);
	assert_column(fx, "B.mkv", "state", "suspected");

	/* An hour on: missing. */
	age_suspicion(fx, "B.mkv", TRUE, TRUE);
	result = scan(fx);
	g_assert_cmpuint(result->confirmed, ==, 1);
	g_assert_cmpuint(result->missing_after, ==, 1);
	axis_scan_result_free(result);
	assert_column(fx, "B.mkv", "state", "missing");

	/* Back: present again, the same record. */
	put(fx, "B.mkv", "bbbbbb");
	result = scan(fx);
	g_assert_cmpuint(result->reappeared, ==, 1);
	g_assert_cmpuint(result->new_files, ==, 0);
	axis_scan_result_free(result);
	assert_column(fx, "B.mkv", "state", "present");
	assert_column(fx, "B.mkv", "id", id);
	assert_column(fx, "B.mkv", "suspected_at", NULL);
	assert_column(fx, "B.mkv", "missing_at", NULL);

	/* Gone for good: missing, then removed 30 days later. */
	drop(fx, "B.mkv");
	axis_scan_result_free(scan(fx));
	age_suspicion(fx, "B.mkv", TRUE, TRUE);
	axis_scan_result_free(scan(fx));
	assert_column(fx, "B.mkv", "state", "missing");
	again = g_strdup_printf("UPDATE library_file SET missing_at = missing_at - interval '29 days'"
		" WHERE root_id = %" G_GINT64_FORMAT, fx->root);
	axis_test_db_exec(fx->t->db, again);
	result = scan(fx);
	g_assert_cmpuint(result->removed, ==, 0);
	axis_scan_result_free(result);
	g_free(again);
	again = g_strdup_printf("UPDATE library_file SET missing_at = missing_at - interval '2 days'"
		" WHERE root_id = %" G_GINT64_FORMAT, fx->root);
	axis_test_db_exec(fx->t->db, again);
	result = scan(fx);
	g_assert_cmpuint(result->removed, ==, 1);
	axis_scan_result_free(result);
	g_assert_cmpint(n(fx, "rel_path = 'B.mkv'"), ==, 0);
	size = column(fx, "A.mkv", "state");
	g_assert_cmpstr(size, ==, "present");
}

/* A change of the database's clock is not an hour passing: within one
 * boot, the boot clock must agree. */
static void
test_clock_change(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *boot = NULL;
	AxisScanResult *result;

	SKIP_WITHOUT_DB(fx);
	put(fx, "A.mkv", "aaaa");
	put(fx, "B.mkv", "bbbb");
	axis_scan_result_free(scan(fx));
	drop(fx, "B.mkv");
	axis_scan_result_free(scan(fx));
	boot = column(fx, "B.mkv", "suspected_boot_id");
	if (boot == NULL) {
		g_test_skip("this system has no boot id");
		return;
	}

	/* The database's clock jumped two hours; the machine's did not. */
	age_suspicion(fx, "B.mkv", TRUE, FALSE);
	result = scan(fx);
	g_assert_cmpuint(result->confirmed, ==, 0);
	axis_scan_result_free(result);
	assert_column(fx, "B.mkv", "state", "suspected");

	/* Suspected in another boot: the database's clock alone decides. */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET suspected_boot_id = "
		"'00000000-0000-0000-0000-000000000000' WHERE rel_path = 'B.mkv'");
	result = scan(fx);
	g_assert_cmpuint(result->confirmed, ==, 1);
	axis_scan_result_free(result);
	assert_column(fx, "B.mkv", "state", "missing");
}

static void
cancel_after_batch(
	guint    batch,
	gpointer data
){
	Fixture *fx = data;

	if (batch == 1)
		g_cancellable_cancel(fx->cancellable);
}

static void
cancel_in_finish(
	const gchar *stage,
	guint        batch,
	gpointer     data
){
	Fixture *fx = data;

	if (strcmp(stage, "finish") == 0)
		g_cancellable_cancel(fx->cancellable);
}

static gboolean
yes_shutdown(gpointer data)
{
	return TRUE;
}

/* Spec: Cancelled scan confirms nothing */
static void
test_cancelled(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	AxisScanOptions options;
	g_autoptr(GError) error = NULL;
	guint round;

	SKIP_WITHOUT_DB(fx);
	many_files(fx, 6, 5);
	put(fx, "B.mkv", "b");
	axis_scan_result_free(scan(fx));
	drop(fx, "B.mkv");
	axis_scan_result_free(scan(fx));
	age_suspicion(fx, "B.mkv", TRUE, TRUE);
	drop(fx, "d01/f1");

	memset(&tuning, 0, sizeof(tuning));
	tuning.batch_entries = 4;
	tuning.after_batch = cancel_after_batch;
	tune(fx, &tuning);

	/* Stopped during the walk; by a shutdown; and at the very end, inside
	 * the final transaction. */
	for (round = 0; round < 3; round++) {
		g_autoptr(AxisScanResult) result = NULL;
		g_autofree gchar *status = NULL;

		if (round == 2) {
			memset(&tuning, 0, sizeof(tuning));
			tuning.before_commit = cancel_in_finish;
			tune(fx, &tuning);
		}
		g_clear_object(&fx->cancellable);
		fx->cancellable = g_cancellable_new();
		memset(&options, 0, sizeof(options));
		options.cancellable = fx->cancellable;
		options.is_shutdown = round == 1 ? yes_shutdown : NULL;
		g_assert_true(axis_library_scan_root(fx->t->library, fx->root, &options, &result, &error));
		g_assert_no_error(error);
		g_assert_cmpint(result->status, ==, AXIS_SCAN_INCOMPLETE);
		g_assert_cmpint(result->reason, ==, round == 1 ? AXIS_SCAN_REASON_SHUTDOWN :
			AXIS_SCAN_REASON_CANCELLED);
		g_assert_cmpuint(result->suspected, ==, 0);
		assert_column(fx, "B.mkv", "state", "suspected");
		assert_column(fx, "d01/f1", "state", "present");
		status = axis_test_library_q(fx->t, "SELECT status FROM library_scan WHERE id = %" G_GINT64_FORMAT,
			result->scan_id);
		g_assert_cmpstr(status, ==, "incomplete");
	}
}

/* ── Moves ──────────────────────────────────────────────────────── */

/* Spec: Renamed file */
static void
test_rename(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *id = NULL;
	g_autofree gchar *copied_id = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *to = NULL;
	g_autofree gchar *twin1 = NULL;
	g_autofree gchar *twin2 = NULL;
	g_autofree gchar *twin3 = NULL;
	AxisScanResult *result;

	SKIP_WITHOUT_DB(fx);
	put(fx, "A/a.mkv", "aaaa");
	put(fx, "c.mkv", "cccc");
	put(fx, "keep.mkv", "keep");
	axis_scan_result_free(scan(fx));
	clear_changed(fx);
	id = column(fx, "A/a.mkv", "id");

	/* Renamed into another folder: the same record, unchanged content. */
	from = in_films(fx, "A/a.mkv");
	to = in_films(fx, "B/renamed.mkv");
	put(fx, "B/placeholder.txt", "x");
	g_assert_cmpint(g_rename(from, to), ==, 0);
	result = scan(fx);
	g_assert_cmpuint(result->moved, ==, 1);
	g_assert_cmpuint(result->new_files, ==, 1);	/* the placeholder */
	g_assert_cmpuint(result->suspected, ==, 0);
	axis_scan_result_free(result);
	assert_column(fx, "B/renamed.mkv", "id", id);
	assert_column(fx, "B/renamed.mkv", "changed", "false");
	g_assert_cmpint(n(fx, "rel_path = 'A/a.mkv'"), ==, 0);
	g_assert_cmpint(n(fx, "moved_at IS NOT NULL"), ==, 1);

	/* Copied with its time and the original deleted: another inode, the
	 * only match by size and time.  The same record, to be looked at again. */
	clear_changed(fx);
	copied_id = column(fx, "c.mkv", "id");
	{
		g_autofree gchar *c = in_films(fx, "c.mkv");
		g_autofree gchar *d = in_films(fx, "d.mkv");

		axis_test_set_mtime(c, 1600000000, 5);
		axis_scan_result_free(scan(fx));
		clear_changed(fx);
		axis_test_storage_write(d, "cccc");
		axis_test_set_mtime(d, 1600000000, 5);
		g_assert_cmpint(g_unlink(c), ==, 0);
	}
	result = scan(fx);
	g_assert_cmpuint(result->moved, ==, 1);
	g_assert_cmpuint(result->suspected, ==, 0);
	axis_scan_result_free(result);
	assert_column(fx, "d.mkv", "id", copied_id);
	assert_column(fx, "d.mkv", "changed", "true");

	/* Two that could be it: no guess, a deletion and a new file. */
	twin1 = in_films(fx, "twin1.mkv");
	twin2 = in_films(fx, "twin2.mkv");
	twin3 = in_films(fx, "twin3.mkv");
	axis_test_storage_write(twin1, "tttt");
	axis_test_storage_write(twin2, "tttt");
	axis_test_set_mtime(twin1, 1500000000, 7);
	axis_test_set_mtime(twin2, 1500000000, 7);
	axis_scan_result_free(scan(fx));
	axis_test_storage_write(twin3, "tttt");
	axis_test_set_mtime(twin3, 1500000000, 7);
	g_assert_cmpint(g_unlink(twin1), ==, 0);
	g_assert_cmpint(g_unlink(twin2), ==, 0);
	result = scan(fx);
	g_assert_cmpuint(result->moved, ==, 0);
	g_assert_cmpuint(result->suspected, ==, 2);
	g_assert_cmpuint(result->new_files, ==, 1);
	axis_scan_result_free(result);
}

/* ── Pinned ─────────────────────────────────────────────────────── */

/* Spec: Hardlinked file; Link removed; (and a downloads root) */
static void
test_pinned(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *x = NULL;
	g_autofree gchar *y = NULL;
	g_autofree gchar *outside = g_build_filename(fx->disk, "seeding.mkv", NULL);
	g_autofree gchar *h = NULL;
	g_autofree gchar *downloads_dir = g_build_filename(fx->disk, "Downloads", NULL);
	g_autofree gchar *z = NULL;
	gint64 downloads;
	AxisScanResult *result;

	SKIP_WITHOUT_DB(fx);
	put(fx, "x.mkv", "xxxx");
	put(fx, "alone.mkv", "alone");
	x = in_films(fx, "x.mkv");
	y = in_films(fx, "y.mkv");
	h = in_films(fx, "h.mkv");
	g_assert_cmpint(link(x, y), ==, 0);
	/* Hard-linked from outside any root, as a downloader seeding it. */
	axis_test_storage_write(outside, "seed");
	g_assert_cmpint(link(outside, h), ==, 0);

	result = scan(fx);
	g_assert_cmpuint(result->pinned_after, ==, 3);
	axis_scan_result_free(result);
	assert_column(fx, "x.mkv", "pinned", "true");
	assert_column(fx, "x.mkv", "pinned_links", "true");
	assert_column(fx, "x.mkv", "nlink", "2");
	assert_column(fx, "y.mkv", "pinned", "true");
	assert_column(fx, "h.mkv", "pinned", "true");
	assert_column(fx, "alone.mkv", "pinned", "false");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(DISTINCT inode) FROM library_file"
		" WHERE root_id = %" G_GINT64_FORMAT " AND rel_path IN ('x.mkv', 'y.mkv')", fx->root), ==, 1);

	/* The second link goes: x is no longer pinned. */
	g_assert_cmpint(g_unlink(y), ==, 0);
	g_assert_cmpint(g_unlink(outside), ==, 0);
	axis_scan_result_free(scan(fx));
	assert_column(fx, "x.mkv", "pinned", "false");
	assert_column(fx, "x.mkv", "pinned_links", "false");
	assert_column(fx, "x.mkv", "nlink", "1");
	assert_column(fx, "h.mkv", "pinned", "false");
	assert_column(fx, "y.mkv", "state", "suspected");

	/* Everything under a downloads root is pinned. */
	downloads = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Downloads", AXIS_MEDIA_DOWNLOADS);
	z = g_build_filename(downloads_dir, "z.mkv", NULL);
	axis_test_storage_write(z, "zzzz");
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, downloads, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE root_id = %"
		G_GINT64_FORMAT " AND pinned AND pinned_downloads AND NOT pinned_links", downloads), ==, 1);
}

/* ── Kinds and ignored names ────────────────────────────────────── */

/* Spec: Kinds; Partial download is not indexed */
static void
test_kinds_and_partials(Fixture *fx, gconstpointer data)
{
	static const gchar *const patterns[] = { "*.nfo", "Sample", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kinds = NULL;
	g_autofree gchar *findings = NULL;
	AxisScanResult *result;

	SKIP_WITHOUT_DB(fx);
	put(fx, "a.MKV", "v");
	put(fx, "b.Flac", "a");
	put(fx, "c.srt", "s");
	put(fx, "d.PNG", "i");
	put(fx, "e.nfo", "t");
	put(fx, "f.rar", "o");
	put(fx, "Sample/s.mkv", "sample");
	put(fx, "g.mkv.part", "partial");
	put(fx, "h.mkv.!qB", "partial");
	put(fx, ".DS_Store", "junk");
	put(fx, "_UNPACK_Film/part1.mkv", "unpacking");

	result = scan(fx);
	g_assert_cmpuint(result->walk.partials, ==, 3);
	g_assert_cmpuint(result->walk.ignored, ==, 1);
	g_assert_cmpuint(result->walk.files, ==, 7);
	g_assert_cmpuint(result->walk.kind_files[AXIS_FILE_KIND_VIDEO], ==, 2);
	axis_scan_result_free(result);
	kinds = axis_test_library_q(fx->t, "SELECT string_agg(rel_path || '=' || kind, ',' ORDER BY rel_path)"
		" FROM library_file WHERE root_id = %" G_GINT64_FORMAT, fx->root);
	g_assert_cmpstr(kinds, ==, "Sample/s.mkv=video,a.MKV=video,b.Flac=audio,c.srt=subtitle,d.PNG=image,"
		"e.nfo=text,f.rar=other");
	findings = axis_test_library_q(fx->t, "SELECT string_agg(kind || ':' || rel_path, ',' ORDER BY rel_path)"
		" FROM library_scan_finding f JOIN library_scan s ON s.id = f.scan_id"
		" WHERE s.root_id = %" G_GINT64_FORMAT, fx->root);
	g_assert_cmpstr(findings, ==, "partial:_UNPACK_Film,partial:g.mkv.part,partial:h.mkv.!qB");

	/* The root's own patterns leave out more; what was recorded under them
	 * is not taken for gone. */
	g_assert_true(axis_library_set_root_config(fx->t->library, fx->root, 6 * 3600, patterns, &error));
	g_assert_no_error(error);
	result = scan(fx);
	g_assert_cmpuint(result->walk.ignored, ==, 3);
	g_assert_cmpuint(result->walk.files, ==, 5);
	g_assert_cmpuint(result->suspected, ==, 0);
	axis_scan_result_free(result);
	assert_column(fx, "e.nfo", "state", "present");
	assert_column(fx, "Sample/s.mkv", "state", "present");

	/* Settings are checked. */
	g_assert_false(axis_library_set_root_config(fx->t->library, fx->root, 60, NULL, &error));
	g_assert_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_INVALID);
	g_clear_error(&error);
	{
		static const gchar *const bad[] = { "a/b", NULL };

		g_assert_false(axis_library_set_root_config(fx->t->library, fx->root, 3600, bad, &error));
		g_assert_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_INVALID);
		g_clear_error(&error);
	}
	g_assert_false(axis_library_set_root_config(fx->t->library, 999999, 3600, NULL, &error));
	g_assert_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND);
}

/* An empty root while files are recorded is not "everything is gone", and
 * a root whose directory went away is not either. */
static void
test_empty_or_missing_root(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *aside = g_strconcat(fx->films, ".aside", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	SKIP_WITHOUT_DB(fx);
	put(fx, "A.mkv", "a");
	put(fx, "B/b.mkv", "b");
	axis_scan_result_free(scan(fx));
	before = axis_test_library_index(fx->t, fx->root);

	g_assert_cmpint(g_rename(fx->films, aside), ==, 0);
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_INCOMPLETE,
		AXIS_SCAN_REASON_ROOT_MISSING));
	axis_test_storage_mkdir(fx->films);
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_INCOMPLETE,
		AXIS_SCAN_REASON_ROOT_EMPTY));
	after = axis_test_library_index(fx->t, fx->root);
	g_assert_cmpstr(before, ==, after);

	g_assert_cmpint(g_rmdir(fx->films), ==, 0);
	g_assert_cmpint(g_rename(aside, fx->films), ==, 0);
	axis_scan_result_free(scan(fx));
	g_assert_cmpint(n(fx, "state = 'present'"), ==, 2);
}

/* ── A hung walk ────────────────────────────────────────────────── */

static void
hang_hook(
	const gchar *path,
	gpointer     data
){
	Fixture *fx = data;

	if (g_strcmp0(path, fx->hook_path) == 0)
		axis_test_gate_pass(fx->gate);
}

static gboolean
walks_ended(gpointer data)
{
	return axis_library_walk_get_live() == 0;
}

static gpointer
cancel_later(gpointer data)
{
	Fixture *fx = data;

	while (axis_test_gate_get_arrived(fx->gate) == 0)
		g_usleep(1000);
	g_usleep(100000);
	g_cancellable_cancel(fx->cancellable);
	return NULL;
}

/* A walk stuck in the filesystem is given up, the scan reports it, and
 * the volume is not walked again until the stuck walk has ended; a stuck
 * walk does not hold up cancellation either. */
static void
test_hung_walk(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	AxisScanOptions options;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisScanResult) result = NULL;
	GThread *thread;
	gint64 started;

	SKIP_WITHOUT_DB(fx);
	put(fx, "A.mkv", "a");
	put(fx, "Hang/b.mkv", "b");
	axis_scan_result_free(scan(fx));
	drop(fx, "A.mkv");

	memset(&tuning, 0, sizeof(tuning));
	tuning.before_directory = hang_hook;
	tuning.stall_ms = 300;
	fx->hook_path = g_strdup("Hang");
	tune(fx, &tuning);

	started = g_get_monotonic_time();
	result = axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_STALLED);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
	g_assert_cmpuint(result->suspected, ==, 0);
	g_clear_pointer(&result, axis_scan_result_free);
	g_assert_cmpuint(axis_library_get_abandoned_walks(fx->t->library), ==, 1);

	/* Not again while it is stuck. */
	result = axis_test_library_scan_expect(fx->t, fx->root, AXIS_SCAN_UNAVAILABLE,
		AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);
	g_clear_pointer(&result, axis_scan_result_free);
	assert_column(fx, "A.mkv", "state", "present");

	/* The filesystem answers again: the walk ends, the volume is scanned. */
	axis_test_gate_open(fx->gate);
	g_assert_true(axis_test_pump_until(walks_ended, NULL, AXIS_TEST_WAIT_MS));
	memset(&tuning, 0, sizeof(tuning));
	tune(fx, &tuning);
	result = scan(fx);
	g_assert_cmpuint(result->suspected, ==, 1);
	g_clear_pointer(&result, axis_scan_result_free);
	g_assert_cmpuint(axis_library_get_abandoned_walks(fx->t->library), ==, 0);

	/* Cancelled while the walk is stuck: back within a couple of seconds. */
	axis_test_gate_free(fx->gate);
	fx->gate = axis_test_gate_new();
	memset(&tuning, 0, sizeof(tuning));
	tuning.before_directory = hang_hook;
	tune(fx, &tuning);
	fx->cancellable = g_cancellable_new();
	memset(&options, 0, sizeof(options));
	options.cancellable = fx->cancellable;
	thread = g_thread_new("cancel", cancel_later, fx);
	started = g_get_monotonic_time();
	g_assert_true(axis_library_scan_root(fx->t->library, fx->root, &options, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 4 * G_USEC_PER_SEC);
	g_assert_cmpint(result->reason, ==, AXIS_SCAN_REASON_CANCELLED);
	g_thread_join(thread);
	axis_test_gate_open(fx->gate);
	g_assert_true(axis_test_pump_until(walks_ended, NULL, AXIS_TEST_WAIT_MS));
}

/* ── Killed mid-scan ────────────────────────────────────────────── */

#define KILL_BATCH 10

static gint kill_mode;	/* 0 inside a batch, 1 between batches, 2 inside the end */

static void
kill_before_commit(
	const gchar *stage,
	guint        batch,
	gpointer     data
){
	if ((kill_mode == 0 && strcmp(stage, "batch") == 0 && batch == 3) ||
	    (kill_mode == 2 && strcmp(stage, "finish") == 0))
		kill(getpid(), SIGKILL);
}

static void
kill_after_batch(
	guint    batch,
	gpointer data
){
	if (kill_mode == 1 && batch == 2)
		kill(getpid(), SIGKILL);
}

/* In the child: a library of its own on the parent's database, scanning
 * until it kills itself. */
static void
kill_child(void)
{
	g_autoptr(GError) error = NULL;
	AxisDbPool *pool;
	AxisSettings *settings;
	AxisStorage *storage;
	AxisLibrary *library;
	AxisLibraryTuning tuning;
	gint64 root = g_ascii_strtoll(g_getenv("AXIS_TEST_KILL_ROOT"), NULL, 10);

	kill_mode = (gint)g_ascii_strtoll(g_getenv("AXIS_TEST_KILL_MODE"), NULL, 10);
	pool = axis_db_pool_new(g_getenv("AXIS_TEST_KILL_DB"), 3, &error);
	g_assert_no_error(error);
	settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(settings, AXIS_STORAGE_MODULE_ID,
		axis_storage_get_setting_table(), &error));
	g_assert_true(axis_settings_load(settings, NULL, &error));
	storage = axis_storage_new(pool, settings);
	g_assert_true(axis_storage_load(storage, &error));
	library = axis_library_new(pool, storage);
	memset(&tuning, 0, sizeof(tuning));
	tuning.batch_entries = KILL_BATCH;
	tuning.racy_window_ns = 0;
	tuning.guard_interval_ms = 1;
	tuning.before_commit = kill_before_commit;
	tuning.after_batch = kill_after_batch;
	axis_library_set_tuning(library, &tuning);

	axis_library_scan_root(library, root, NULL, NULL, &error);
	g_error("the scan was meant to be killed");
}

/* Spec: Killed mid-scan */
static void
test_killed(gconstpointer data)
{
	gint mode = GPOINTER_TO_INT(data);
	g_autoptr(AxisScanResult) last = NULL;
	g_autofree gchar *root_text = NULL;
	g_autofree gchar *mode_text = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *disk = NULL;
	g_autofree gchar *status = NULL;
	g_auto(GStrv) envp = NULL;
	AxisTestLibrary *t;
	gint64 volume;
	gint64 root;
	guint i;

	if (g_test_subprocess()) {
		kill_child();
		return;
	}

	axis_test_log_init();
	t = axis_test_library_new(6);
	if (t == NULL)
		return;
	disk = axis_test_library_disk(t, "disk1");
	volume = axis_test_library_add_volume(t, disk, "Disk");
	root = axis_test_library_add_root(t, volume, disk, "Films", AXIS_MEDIA_MOVIES);
	films = g_build_filename(disk, "Films", NULL);
	for (i = 0; i < 6; i++) {
		g_autofree gchar *dir = g_strdup_printf("%s/d%u", films, i);

		axis_test_make_files(dir, 10);
	}
	axis_scan_result_free(axis_test_library_scan(t, root));

	/* What the killed scan sees: some gone, some new, one renamed. */
	for (i = 0; i < 4; i++) {
		g_autofree gchar *gone = g_strdup_printf("%s/d5/f%u", films, i);

		g_assert_cmpint(g_unlink(gone), ==, 0);
	}
	{
		g_autofree gchar *fresh = g_strdup_printf("%s/new", films);
		g_autofree gchar *from = g_strdup_printf("%s/d0/f9", films);
		g_autofree gchar *to = g_strdup_printf("%s/new/renamed", films);

		axis_test_make_files(fresh, 15);
		g_assert_cmpint(g_rename(from, to), ==, 0);
	}

	root_text = g_strdup_printf("%" G_GINT64_FORMAT, root);
	mode_text = g_strdup_printf("%d", mode);
	envp = g_get_environ();
	envp = g_environ_setenv(envp, "AXIS_TEST_KILL_DB", axis_test_db_get_url(t->db), TRUE);
	envp = g_environ_setenv(envp, "AXIS_TEST_KILL_ROOT", root_text, TRUE);
	envp = g_environ_setenv(envp, "AXIS_TEST_KILL_MODE", mode_text, TRUE);
	g_test_trap_subprocess_with_envp(NULL, (const gchar *const *)envp, 60 * G_USEC_PER_SEC, 0);
	g_test_trap_assert_failed();
	/* Killed, not failed some other way. */
	g_test_trap_assert_stderr("");

	/* What it left: a scan still "running", the batches it committed and
	 * nothing it concluded. */
	status = axis_test_library_q(t, "SELECT status FROM library_scan WHERE root_id = %" G_GINT64_FORMAT
		" ORDER BY id DESC LIMIT 1", root);
	g_assert_cmpstr(status, ==, "running");
	g_assert_cmpint(axis_test_library_n(t, "SELECT count(*) FROM library_file WHERE root_id = %"
		G_GINT64_FORMAT " AND state <> 'present'", root), ==, 0);
	g_assert_cmpint(axis_test_library_n(t, "SELECT complete_generation FROM library_root_state"
		" WHERE root_id = %" G_GINT64_FORMAT, root), ==, 1);
	if (mode == 0 || mode == 1)
		g_assert_cmpint(axis_test_library_n(t, "SELECT count(*) FROM library_file WHERE root_id = %"
			G_GINT64_FORMAT " AND last_seen_generation = 2", root), ==, 2 * KILL_BATCH);
	else
		g_assert_cmpint(axis_test_library_n(t, "SELECT count(*) FROM library_file WHERE root_id = %"
			G_GINT64_FORMAT " AND last_seen_generation = 2", root), ==, 71);

	/* The next scan just carries on: nothing to repair. */
	last = axis_test_library_scan_expect(t, root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	g_assert_cmpuint(last->changed_files, ==, 0);
	g_assert_cmpuint(last->reappeared, ==, 0);
	g_assert_cmpuint(last->suspected, ==, 4);
	g_assert_cmpuint(last->moved, ==, 1);
	g_assert_cmpuint(last->present_after, ==, 71);
	g_clear_pointer(&status, g_free);
	status = axis_test_library_q(t, "SELECT string_agg(status || '/' || coalesce(reason, '-'), ','"
		" ORDER BY id) FROM library_scan WHERE root_id = %" G_GINT64_FORMAT, root);
	g_assert_cmpstr(status, ==, "complete/-,incomplete/interrupted,complete/-");
	g_assert_cmpint(axis_test_library_n(t, "SELECT count(*) FROM library_file WHERE root_id = %"
		G_GINT64_FORMAT " AND rel_path = 'new/renamed' AND first_seen_generation = 1", root), ==, 1);

	axis_test_library_free(t);
	axis_test_log_reset();
}

/* ── One scan per volume ────────────────────────────────────────── */

static gboolean
scan_job(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return axis_library_run_scan_job(user_data, context, error);
}

static gboolean
due_job(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return axis_library_run_due_job(user_data, context, error);
}

typedef struct
{
	AxisTestDb  *db;
	gint64       root;
	const gchar *status;
} ScanWait;

static gboolean
scan_reached(gpointer data)
{
	ScanWait *wait = data;
	g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM library_scan WHERE root_id = %"
		G_GINT64_FORMAT " AND status = '%s'", wait->root, wait->status);
	g_autofree gchar *value = axis_test_db_query(wait->db, sql);

	return g_strcmp0(value, "0") != 0;
}

/* Spec: One scan per volume */
static void
test_one_per_volume(Fixture *fx, gconstpointer data)
{
	static const AxisJobHandlerInfo handlers[] = {
		{ AXIS_LIBRARY_SCAN_KIND, AXIS_LIBRARY_SCAN_QUEUE, 1, scan_job, "Scanning", AXIS_JOB_CLASS_WORK },
		{ AXIS_LIBRARY_DUE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, due_job, "Due", AXIS_JOB_CLASS_UPKEEP },
		{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
	};
	AxisLibraryTuning tuning;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *disk2 = NULL;
	g_autofree gchar *order = NULL;
	gint64 second_root;
	gint64 volume2;
	gint64 other_root;
	gint64 job_a = 0;
	gint64 job_a2 = 0;
	ScanWait wait;
	guint enqueued = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	put(fx, "Slow/a.mkv", "a");
	second_root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "TV", AXIS_MEDIA_TV);
	/* Both "disks" are directories of one filesystem, which storage
	 * refuses as two volumes (axis_test_library_add_volume_beside()); the
	 * first is away while the second is added. */
	axis_test_library_unplug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	disk2 = axis_test_library_disk(fx->t, "disk2");
	volume2 = axis_test_library_add_volume_beside(fx->t, disk2, "Disk two");
	axis_test_library_plug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	other_root = axis_test_library_add_root(fx->t, volume2, disk2, "Music", AXIS_MEDIA_MUSIC);

	memset(&tuning, 0, sizeof(tuning));
	tuning.before_directory = hang_hook;
	fx->hook_path = g_strdup("Slow");
	tune(fx, &tuning);

	registry = axis_job_registry_new();
	for (i = 0; handlers[i].kind != NULL; i++)
		g_assert_true(axis_job_registry_add(registry, &handlers[i], fx->t->library, &error));
	queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_concurrency(queue, AXIS_LIBRARY_SCAN_QUEUE, 2);
	axis_job_queue_set_poll_interval(queue, 50);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* Requests for two roots of one volume make one job. */
	g_assert_true(axis_library_request_scan(fx->t->library, fx->root, &job_a, &error));
	g_assert_true(axis_library_request_scan(fx->t->library, second_root, &job_a2, &error));
	g_assert_cmpint(job_a, ==, job_a2);
	g_assert_true(axis_library_request_scan(fx->t->library, other_root, NULL, &error));
	g_assert_no_error(error);

	/* The first root of the first volume is held in its walk... */
	while (axis_test_gate_get_arrived(fx->gate) == 0)
		axis_test_pump_for(10);

	/* ...while the other volume's root is scanned. */
	wait.db = fx->t->db;
	wait.root = other_root;
	wait.status = "complete";
	g_assert_true(axis_test_pump_until(scan_reached, &wait, AXIS_TEST_WAIT_MS));
	/* The second root of the first volume waits its turn. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_scan WHERE root_id = %"
		G_GINT64_FORMAT, second_root), ==, 0);
	/* Nothing else scans that volume meanwhile. */
	g_assert_false(axis_library_scan_root(fx->t->library, second_root, NULL, NULL, &error));
	g_assert_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_BUSY);
	g_clear_error(&error);
	/* The schedule adds no second job for a volume that has one. */
	g_assert_true(axis_library_enqueue_due(fx->t->library, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 0);

	axis_test_gate_open(fx->gate);
	wait.root = second_root;
	g_assert_true(axis_test_pump_until(scan_reached, &wait, AXIS_TEST_WAIT_MS));
	order = axis_test_library_q(fx->t, "SELECT (b.started_at >= a.finished_at)::text"
		" FROM library_scan a, library_scan b WHERE a.root_id = %" G_GINT64_FORMAT
		" AND b.root_id = %" G_GINT64_FORMAT, fx->root, second_root);
	g_assert_cmpstr(order, ==, "true");
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'library.scan'"
		" AND state = 'succeeded'", "2", AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_root_state"
		" WHERE scan_requested_at IS NOT NULL"), ==, 0);

	g_assert_true(axis_job_queue_stop(queue, 5000));
}

/* What is due: never scanned, an interval on, a failed scan an hour on;
 * a volume that is not online only for a request. */
static void
test_due(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint enqueued = 0;

	SKIP_WITHOUT_DB(fx);
	put(fx, "a.mkv", "a");

	g_assert_true(axis_library_enqueue_due(fx->t->library, &enqueued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(enqueued, ==, 1);
	g_assert_true(axis_library_enqueue_due(fx->t->library, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 0);
	axis_test_db_exec(fx->t->db, "DELETE FROM job");

	axis_scan_result_free(scan(fx));
	g_assert_true(axis_library_enqueue_due(fx->t->library, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 0);

	/* Six hours on. */
	axis_test_db_exec(fx->t->db, "UPDATE library_scan SET started_at = started_at - interval '6 hours'");
	g_assert_true(axis_library_enqueue_due(fx->t->library, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 1);
	axis_test_db_exec(fx->t->db, "DELETE FROM job");

	/* Offline: due scans wait, a request is answered. */
	axis_test_library_unplug(fx->t, fx->disk, "away");
	fx->unplugged = TRUE;
	{
		g_autoptr(AxisVolumeInfo) info = NULL;

		g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, &info, &error));
		g_assert_false(axis_volume_state_is_usable(info->state));
	}
	g_assert_true(axis_library_enqueue_due(fx->t->library, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 0);
	g_assert_true(axis_library_request_scan(fx->t->library, fx->root, NULL, &error));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE kind = 'library.scan'"
		" AND priority = 10"), ==, 1);
}

/* ── A large tree ───────────────────────────────────────────────── */

#if defined(__SANITIZE_ADDRESS__)
extern size_t __sanitizer_get_current_allocated_bytes(void);

static gsize
heap_in_use(void)
{
	return __sanitizer_get_current_allocated_bytes();
}
#else
static gsize
heap_in_use(void)
{
	struct mallinfo2 info = mallinfo2();

	return info.uordblks + info.hblkhd;
}
#endif

static gsize heap_peak;

static void
sample_heap(
	guint    batch,
	gpointer data
){
	gsize now = heap_in_use();

	if (now > heap_peak)
		heap_peak = now;
}

/* The heap's growth at its highest during a scan of @root. */
static gsize
scan_growth(
	Fixture          *fx,
	gint64            root,
	AxisScanResult  **result,
	gdouble          *seconds
){
	gsize before;
	gint64 started;

	while (g_main_context_iteration(NULL, FALSE))
		;
	before = heap_in_use();
	heap_peak = before;
	started = g_get_monotonic_time();
	*result = axis_test_library_scan_expect(fx->t, root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	*seconds = (gdouble)(g_get_monotonic_time() - started) / G_USEC_PER_SEC;
	return heap_peak > before ? heap_peak - before : 0;
}

/* Spec: Large tree */
static void
test_large_tree(Fixture *fx, gconstpointer data)
{
	AxisLibraryTuning tuning;
	g_autofree gchar *small_dir = NULL;
	g_autofree gchar *large_dir = NULL;
	gint64 small_root;
	gint64 large_root;
	gsize small_growth;
	gsize large_growth;
	gsize again_growth;
	gdouble seconds;
	gdouble again_seconds;
	AxisScanResult *result;
	guint i;

	SKIP_WITHOUT_DB(fx);
	small_root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Small", AXIS_MEDIA_OTHER);
	large_root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Large", AXIS_MEDIA_OTHER);
	small_dir = g_build_filename(fx->disk, "Small", NULL);
	large_dir = g_build_filename(fx->disk, "Large", NULL);
	for (i = 0; i < 300; i++) {
		g_autofree gchar *dir = g_strdup_printf("%s/%03u/sub", large_dir, i);

		axis_test_make_files(dir, 1000);
		if (i < 30) {
			g_autofree gchar *small = g_strdup_printf("%s/%03u/sub", small_dir, i);

			axis_test_make_files(small, 1000);
		}
	}

	memset(&tuning, 0, sizeof(tuning));
	tuning.after_batch = sample_heap;
	tuning.guard_interval_ms = AXIS_LIBRARY_GUARD_INTERVAL_MS;
	tune(fx, &tuning);

	small_growth = scan_growth(fx, small_root, &result, &seconds);
	g_assert_cmpuint(result->walk.files, ==, 30000);
	axis_scan_result_free(result);

	large_growth = scan_growth(fx, large_root, &result, &seconds);
	g_assert_cmpuint(result->walk.files, ==, 300000);
	g_assert_cmpuint(result->new_files, ==, 300000);
	axis_scan_result_free(result);

	again_growth = scan_growth(fx, large_root, &result, &again_seconds);
	g_assert_cmpuint(result->changed_files, ==, 0);
	g_assert_cmpuint(result->present_after, ==, 300000);
	axis_scan_result_free(result);

	g_test_message("30 000 files: heap +%" G_GSIZE_FORMAT " KiB; 300 000 files: heap +%" G_GSIZE_FORMAT
		" KiB, %.1f s (%.0f files/s); again, unchanged: heap +%" G_GSIZE_FORMAT " KiB, %.1f s "
		"(%.0f files/s)", small_growth / 1024, large_growth / 1024, seconds, 300000 / seconds,
		again_growth / 1024, again_seconds, 300000 / again_seconds);

	/* Bounded, and not growing with ten times the files. */
	g_assert_cmpuint(large_growth, <, 32 * 1024 * 1024);
	g_assert_cmpuint(again_growth, <, 32 * 1024 * 1024);
	g_assert_cmpuint(large_growth, <, small_growth + 8 * 1024 * 1024);
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD_TEST(path, func) \
	AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)

	ADD_TEST("/library/scan/index/new", test_new_files);
	ADD_TEST("/library/scan/index/unchanged", test_unchanged);
	ADD_TEST("/library/scan/index/modified", test_modified);
	ADD_TEST("/library/scan/index/racy", test_racy);
	ADD_TEST("/library/scan/containment/symlink-out", test_symlink_out);
	ADD_TEST("/library/scan/containment/link-loop", test_link_loop);
	ADD_TEST("/library/scan/containment/replaced-directory", test_replaced_directory);
	ADD_TEST("/library/scan/containment/vanished-directory", test_vanished_directory);
	ADD_TEST("/library/scan/names", test_names);
	ADD_TEST("/library/scan/offline/before", test_offline_before);
	ADD_TEST("/library/scan/offline/mid-scan", test_offline_mid_scan);
	ADD_TEST("/library/scan/offline/unreadable-directory", test_unreadable);
	ADD_TEST("/library/scan/root/empty-or-missing", test_empty_or_missing_root);
	ADD_TEST("/library/scan/missing/sequence", test_missing);
	ADD_TEST("/library/scan/missing/clock-change", test_clock_change);
	ADD_TEST("/library/scan/missing/cancelled", test_cancelled);
	ADD_TEST("/library/scan/moves", test_rename);
	ADD_TEST("/library/scan/pinned", test_pinned);
	ADD_TEST("/library/scan/kinds-and-partials", test_kinds_and_partials);
	ADD_TEST("/library/scan/hung-walk", test_hung_walk);
	ADD_TEST("/library/scan/jobs/one-per-volume", test_one_per_volume);
	ADD_TEST("/library/scan/jobs/due", test_due);
	ADD_TEST("/library/scan/large-tree", test_large_tree);
	g_test_add_data_func("/library/scan/killed/in-batch", GINT_TO_POINTER(0), test_killed);
	g_test_add_data_func("/library/scan/killed/between-batches", GINT_TO_POINTER(1), test_killed);
	g_test_add_data_func("/library/scan/killed/in-conclusion", GINT_TO_POINTER(2), test_killed);

	return g_test_run();
}
