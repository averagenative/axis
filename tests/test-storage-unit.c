/*
 * test-storage-unit.c - the storage code that needs no database: the
 * marker's format and file, path checking, the filesystem walk, and the
 * bounded helper with deadlines
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "axis-test-storage.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-storage-fs.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-storage-private.h"
#include "storage/axis-storage-probe.h"
#include "storage/axis-volume-marker.h"

#define UUID "3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0b"

#define CANONICAL \
	"axis-volume 1\n" \
	"uuid " UUID "\n" \
	"created 2026-11-12T10:15:30Z\n" \
	"note " AXIS_VOLUME_MARKER_NOTE "\n"

/* ── Marker format ──────────────────────────────────────────────── */

static gboolean
parse_text(
	const gchar       *text,
	AxisVolumeMarker  *marker,
	GError           **error
){
	return axis_volume_marker_parse((const guint8 *)text, strlen(text), marker, error);
}

static void
test_marker_round_trip(void)
{
	AxisVolumeMarker marker;
	gchar buffer[AXIS_VOLUME_MARKER_MAX_BYTES];
	g_autoptr(GError) error = NULL;
	gsize length;

	g_assert_true(parse_text(CANONICAL, &marker, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(marker.uuid, ==, UUID);
	g_assert_cmpint(marker.created, ==, G_GINT64_CONSTANT(1794478530));

	length = axis_volume_marker_format(&marker, buffer);
	g_assert_cmpuint(length, ==, strlen(CANONICAL));
	g_assert_cmpint(memcmp(buffer, CANONICAL, length), ==, 0);

	/* The epoch and the last second the format holds. */
	marker.created = 0;
	length = axis_volume_marker_format(&marker, buffer);
	g_assert_nonnull(g_strstr_len(buffer, (gssize)length, "created 1970-01-01T00:00:00Z\n"));
	marker.created = AXIS_VOLUME_MARKER_MAX_TIME;
	length = axis_volume_marker_format(&marker, buffer);
	g_assert_nonnull(g_strstr_len(buffer, (gssize)length, "created 9999-12-31T23:59:59Z\n"));
	g_assert_true(axis_volume_marker_parse((const guint8 *)buffer, length, &marker, NULL));
	g_assert_cmpint(marker.created, ==, AXIS_VOLUME_MARKER_MAX_TIME);

	/* A leap day. */
	g_assert_true(parse_text("axis-volume 1\nuuid " UUID "\ncreated 2024-02-29T23:59:59Z\nnote "
		AXIS_VOLUME_MARKER_NOTE "\n", &marker, NULL));
	g_assert_cmpint(marker.created, ==, G_GINT64_CONSTANT(1709251199));
}

static void
assert_refused(
	const gchar *text,
	gint         code
){
	AxisVolumeMarker marker;
	g_autoptr(GError) error = NULL;

	memset(&marker, 0x5a, sizeof(marker));
	if (parse_text(text, &marker, &error))
		g_error("accepted a marker that should be refused:\n%s", text);
	g_assert_error(error, AXIS_VOLUME_MARKER_ERROR, code);
}

static void
test_marker_refused(void)
{
	GString *big;
	static const gchar *const bad[] = {
		"",
		"axis-volume 1\n",
		/* Truncated, padded, other line endings. */
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE,
		CANONICAL "\n",
		" " CANONICAL,
		"axis-volume 1\r\nuuid " UUID "\ncreated 2026-11-12T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		/* The UUID: upper case, all zeros, misplaced hyphen, not hex. */
		"axis-volume 1\nuuid 3F2A5C1E-7B9D-4E2F-8A6B-1C3D5E7F9A0B\ncreated 2026-11-12T10:15:30Z\nnote "
			AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid 00000000-0000-0000-0000-000000000000\ncreated 2026-11-12T10:15:30Z\nnote "
			AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid 3f2a5c1e7-b9d-4e2f-8a6b-1c3d5e7f9a0b\ncreated 2026-11-12T10:15:30Z\nnote "
			AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid 3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0g\ncreated 2026-11-12T10:15:30Z\nnote "
			AXIS_VOLUME_MARKER_NOTE "\n",
		/* Times that do not exist, or outside 1970 to 9999. */
		"axis-volume 1\nuuid " UUID "\ncreated 2026-02-30T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2025-02-29T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2026-13-12T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12T24:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12T10:60:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12T10:15:60Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 1969-12-31T23:59:59Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12 10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12T10:15:30+00\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		/* The note, word for word. */
		"axis-volume 1\nuuid " UUID "\ncreated 2026-11-12T10:15:30Z\nnote Axis recognises this disk "
			"by this file. Do not edit, delete or copy it!\n",
		/* Versions that are not later ones. */
		"axis-volume 0\nuuid " UUID "\ncreated 2026-11-12T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		"axis-volume 01\nuuid " UUID "\ncreated 2026-11-12T10:15:30Z\nnote " AXIS_VOLUME_MARKER_NOTE "\n",
		NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++)
		assert_refused(bad[i], AXIS_VOLUME_MARKER_ERROR_INVALID);

	/* A marker of a later version says so. */
	assert_refused("axis-volume 2\nuuid " UUID "\ncreated 2026-11-12T10:15:30Z\n",
		AXIS_VOLUME_MARKER_ERROR_VERSION);
	assert_refused("axis-volume 17\n", AXIS_VOLUME_MARKER_ERROR_VERSION);

	/* Anything larger than the cap is refused unread. */
	big = g_string_new(CANONICAL);
	while (big->len <= AXIS_VOLUME_MARKER_MAX_BYTES)
		g_string_append_c(big, 'x');
	assert_refused(big->str, AXIS_VOLUME_MARKER_ERROR_TOO_LARGE);
	g_string_free(big, TRUE);
}

static void
test_uuid_validity(void)
{
	AxisVolumeMarker marker;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_volume_uuid_is_valid(UUID));
	g_assert_false(axis_volume_uuid_is_valid(NULL));
	g_assert_false(axis_volume_uuid_is_valid(""));
	g_assert_false(axis_volume_uuid_is_valid(UUID "0"));
	g_assert_false(axis_volume_uuid_is_valid("3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0"));
	g_assert_false(axis_volume_uuid_is_valid("00000000-0000-0000-0000-000000000000"));

	/* A new marker has a random version 4 UUID, created now. */
	g_assert_true(axis_volume_marker_new(&marker, &error));
	g_assert_no_error(error);
	g_assert_true(axis_volume_uuid_is_valid(marker.uuid));
	g_assert_cmpint(marker.uuid[14], ==, '4');
	g_assert_true(strchr("89ab", marker.uuid[19]) != NULL);
	g_assert_cmpint(ABS(marker.created - g_get_real_time() / G_USEC_PER_SEC), <, 60);
}

/* ── The marker file ────────────────────────────────────────────── */

static gint
open_dir(const gchar *path)
{
	gint fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	return fd;
}

static void
test_marker_file_write(void)
{
	gchar *dir = axis_test_storage_dir_new();
	g_autofree gchar *path = g_build_filename(dir, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *content = NULL;
	g_autofree gchar *again = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeMarker marker;
	AxisVolumeMarker other;
	AxisVolumeMarker back;
	struct stat st;
	gint fd = open_dir(dir);

	g_assert_true(axis_volume_marker_new(&marker, NULL));
	g_assert_true(axis_volume_marker_write(fd, &marker, &error));
	g_assert_no_error(error);

	/* In place, in its one form, readable by everyone, nothing left over. */
	content = axis_test_storage_read(path);
	g_assert_nonnull(content);
	g_assert_true(g_str_has_prefix(content, "axis-volume 1\nuuid "));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0644);
	g_assert_cmpuint(axis_test_storage_count_temporaries(dir), ==, 0);
	g_assert_cmpint(axis_volume_marker_read(fd, &back, NULL), ==, AXIS_VOLUME_MARKER_FOUND);
	g_assert_cmpstr(back.uuid, ==, marker.uuid);
	g_assert_cmpint(back.created, ==, marker.created);

	/* A second marker never replaces the first. */
	g_assert_true(axis_volume_marker_new(&other, NULL));
	g_assert_false(axis_volume_marker_write(fd, &other, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	again = axis_test_storage_read(path);
	g_assert_cmpstr(again, ==, content);
	g_assert_cmpuint(axis_test_storage_count_temporaries(dir), ==, 0);

	close(fd);
	axis_test_storage_dir_free(dir);
}

static void
test_marker_file_hostile(void)
{
	gchar *dir = axis_test_storage_dir_new();
	g_autofree gchar *path = g_build_filename(dir, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *target = g_build_filename(dir, "target", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	GString *big = g_string_new(NULL);
	AxisVolumeMarker marker;
	gint err = 0;
	gint fd = open_dir(dir);

	/* None at all. */
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_MISSING);

	/* A symbolic link to a valid marker is not a marker, and its target is
	 * left as it is. */
	axis_test_storage_mkdir(target);
	axis_test_storage_write_marker(target, UUID);
	{
		g_autofree gchar *link_target = g_build_filename(target, AXIS_VOLUME_MARKER_NAME, NULL);

		g_assert_cmpint(symlink(link_target, path), ==, 0);
	}
	before = axis_test_storage_digest(target);
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_INVALID);
	/* Nor is it replaced by a new marker, and its target is untouched. */
	g_assert_true(axis_volume_marker_new(&marker, NULL));
	g_assert_false(axis_volume_marker_write(fd, &marker, NULL));
	after = axis_test_storage_digest(target);
	g_assert_cmpstr(before, ==, after);
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
	g_assert_cmpuint(axis_test_storage_count_temporaries(dir), ==, 0);
	g_assert_cmpint(unlink(path), ==, 0);

	/* A FIFO is refused without waiting for a writer. */
	g_assert_cmpint(mkfifo(path, 0644), ==, 0);
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_INVALID);
	g_assert_cmpint(unlink(path), ==, 0);

	/* A directory. */
	axis_test_storage_mkdir(path);
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_INVALID);
	g_assert_cmpint(rmdir(path), ==, 0);

	/* Too large, and malformed. */
	while (big->len < 64 * 1024)
		g_string_append(big, CANONICAL);
	axis_test_storage_write(path, big->str);
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_INVALID);
	axis_test_storage_write(path, "axis-volume 1\nuuid nonsense\n");
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_INVALID);

	/* A later version says so through the errno. */
	axis_test_storage_write(path, "axis-volume 2\n");
	g_assert_cmpint(axis_volume_marker_read(fd, &marker, &err), ==, AXIS_VOLUME_MARKER_INVALID);
	g_assert_cmpint(err, ==, EPROTO);

	g_string_free(big, TRUE);
	close(fd);
	axis_test_storage_dir_free(dir);
}

/* ── Paths ──────────────────────────────────────────────────────── */

static void
assert_path(
	const gchar *input,
	gboolean     absolute,
	const gchar *expected
){
	g_autofree gchar *out = NULL;
	const gchar *problem = NULL;
	gboolean ok = axis_storage_path_normalize(input, absolute, &out, &problem);

	if (expected == NULL) {
		if (ok)
			g_error("accepted '%s' as '%s'", input, out);
		g_assert_nonnull(problem);
		g_assert_null(out);
		/* The reason never repeats the input. */
		if (input != NULL && strlen(input) > 3)
			g_assert_null(strstr(problem, input));
		return;
	}
	if (!ok)
		g_error("refused '%s': %s", input, problem);
	g_assert_cmpstr(out, ==, expected);
}

static void
test_path_normalize(void)
{
	GString *long_path = g_string_new(NULL);
	GString *deep = g_string_new(NULL);
	guint i;

	assert_path("/media/disk1", TRUE, "/media/disk1");
	assert_path("  /media//disk1/  ", TRUE, "/media/disk1");
	assert_path("/media/My Disk", TRUE, "/media/My Disk");
	assert_path("/media/caf\xc3\xa9", TRUE, "/media/caf\xc3\xa9");
	assert_path("/media/.hidden", TRUE, "/media/.hidden");
	assert_path("/media/a..b", TRUE, "/media/a..b");

	assert_path(NULL, TRUE, NULL);
	assert_path("", TRUE, NULL);
	assert_path("/", TRUE, NULL);
	assert_path("///", TRUE, NULL);
	assert_path("media/disk1", TRUE, NULL);
	assert_path("/media/../etc", TRUE, NULL);
	assert_path("/media/./disk", TRUE, NULL);
	assert_path("/media/disk/..", TRUE, NULL);
	assert_path("/media/dis\nk", TRUE, NULL);
	assert_path("/media/dis\tk", TRUE, NULL);
	assert_path("/media/dis\x7fk", TRUE, NULL);
	assert_path("/media/dis\xc2\x85k", TRUE, NULL);
	assert_path("/media/\xff\xfe", TRUE, NULL);
	assert_path("/media/disk /", TRUE, NULL);
	assert_path("/media/ disk", TRUE, NULL);
	assert_path("* %* /", FALSE, NULL);

	assert_path("", FALSE, "");
	assert_path(NULL, FALSE, "");
	assert_path("Movies", FALSE, "Movies");
	assert_path("Movies/HD/", FALSE, "Movies/HD");
	assert_path("Movies//HD", FALSE, "Movies/HD");
	assert_path("/Movies", FALSE, NULL);
	assert_path("..", FALSE, NULL);
	assert_path("../x", FALSE, NULL);
	assert_path("Movies/../../x", FALSE, NULL);
	assert_path(".", FALSE, NULL);

	/* Bounds: segment length, total length, depth, input length. */
	g_string_append(long_path, "/");
	for (i = 0; i < 256; i++)
		g_string_append_c(long_path, 'a');
	assert_path(long_path->str, TRUE, NULL);
	g_string_truncate(long_path, 0);
	for (i = 0; i < 20; i++)
		g_string_append(long_path, "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	assert_path(long_path->str, TRUE, NULL);
	for (i = 0; i < 65; i++)
		g_string_append(deep, "/d");
	assert_path(deep->str, TRUE, NULL);
	g_string_truncate(long_path, 0);
	for (i = 0; i < AXIS_STORAGE_INPUT_MAX + 10; i++)
		g_string_append_c(long_path, '/');
	assert_path(long_path->str, TRUE, NULL);

	g_string_free(long_path, TRUE);
	g_string_free(deep, TRUE);
}

static void
test_path_contains(void)
{
	g_assert_true(axis_storage_path_contains("/media/disk", "/media/disk"));
	g_assert_true(axis_storage_path_contains("/media/disk", "/media/disk/Movies"));
	g_assert_false(axis_storage_path_contains("/media/disk", "/media/disk2"));
	g_assert_false(axis_storage_path_contains("/media/disk/Movies", "/media/disk"));
	g_assert_true(axis_storage_path_contains("", "Movies"));
	g_assert_true(axis_storage_path_contains("", ""));
	g_assert_true(axis_storage_path_contains("Movies", "Movies/HD"));
	g_assert_false(axis_storage_path_contains("Movies", "MoviesHD"));
	g_assert_false(axis_storage_path_contains("Movies/HD", "Movies"));
}

static void
test_label_normalize(void)
{
	g_autofree gchar *label = NULL;
	const gchar *problem = NULL;
	GString *long_name = g_string_new(NULL);
	guint i;

	g_assert_true(axis_storage_label_normalize("  Disk One  ", &label, &problem));
	g_assert_cmpstr(label, ==, "Disk One");
	g_clear_pointer(&label, g_free);
	g_assert_true(axis_storage_label_normalize("<b>\"x\" & 'y'</b>", &label, NULL));
	g_assert_cmpstr(label, ==, "<b>\"x\" & 'y'</b>");

	g_assert_false(axis_storage_label_normalize(NULL, NULL, &problem));
	g_assert_false(axis_storage_label_normalize("   ", NULL, &problem));
	g_assert_false(axis_storage_label_normalize("a\nb", NULL, &problem));
	g_assert_false(axis_storage_label_normalize("a\xc2\x9b" "b", NULL, &problem));
	g_assert_false(axis_storage_label_normalize("\xff", NULL, &problem));
	for (i = 0; i < 64; i++)
		g_string_append(long_name, "\xc3\xa9");
	g_assert_true(axis_storage_label_normalize(long_name->str, NULL, &problem));
	g_string_append_c(long_name, 'x');
	g_assert_false(axis_storage_label_normalize(long_name->str, NULL, &problem));
	g_string_free(long_name, TRUE);
}

/* ── Walking paths ──────────────────────────────────────────────── */

static void
test_fs_walk(void)
{
	gchar *dir = axis_test_storage_dir_new();
	g_autofree gchar *real = g_build_filename(dir, "real", NULL);
	g_autofree gchar *deep = g_build_filename(dir, "real", "a", "b", NULL);
	g_autofree gchar *link = g_build_filename(dir, "link", NULL);
	g_autofree gchar *file = g_build_filename(dir, "real", "file", NULL);
	g_autofree gchar *through = g_build_filename(dir, "link", "a", NULL);
	g_autofree gchar *missing = g_build_filename(dir, "real", "nope", NULL);
	g_autofree gchar *inner_link = g_build_filename(dir, "real", "a", "up", NULL);
	g_autofree gchar *outside = g_build_filename(dir, "outside", NULL);
	AxisStorageFsIdentity identity;
	AxisStorageFsInfo info;
	gboolean empty = TRUE;
	gint err = 0;
	gint fd;
	gint sub;

	axis_test_storage_mkdir(deep);
	axis_test_storage_mkdir(outside);
	axis_test_storage_write(file, "x");
	g_assert_cmpint(symlink(real, link), ==, 0);
	g_assert_cmpint(symlink(outside, inner_link), ==, 0);

	/* A real directory opens; the walk never follows a link, anywhere. */
	fd = axis_storage_fs_open_path(real, &err);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(axis_storage_fs_open_path(link, &err), ==, -1);
	g_assert_cmpint(err, ==, ELOOP);
	g_assert_cmpint(axis_storage_fs_open_path(through, &err), ==, -1);
	g_assert_cmpint(err, ==, ELOOP);
	g_assert_cmpint(axis_storage_fs_open_path(missing, &err), ==, -1);
	g_assert_cmpint(err, ==, ENOENT);
	g_assert_cmpint(axis_storage_fs_open_path(file, &err), ==, -1);
	g_assert_cmpint(err, ==, ENOTDIR);

	/* Beneath a directory: no link, no "..", no escape. */
	sub = axis_storage_fs_open_beneath(fd, "a/b", &err);
	g_assert_cmpint(sub, >=, 0);
	close(sub);
	sub = axis_storage_fs_open_beneath(fd, "", &err);
	g_assert_cmpint(sub, >=, 0);
	close(sub);
	g_assert_cmpint(axis_storage_fs_open_beneath(fd, "a/up", &err), ==, -1);
	g_assert_cmpint(err, ==, ELOOP);
	g_assert_cmpint(axis_storage_fs_open_beneath(fd, "a/..", &err), ==, -1);
	g_assert_cmpint(err, ==, EINVAL);
	g_assert_cmpint(axis_storage_fs_open_beneath(fd, "..", &err), ==, -1);
	g_assert_cmpint(axis_storage_fs_open_beneath(fd, "/etc", &err), ==, -1);
	g_assert_cmpint(axis_storage_fs_open_beneath(fd, "nope", &err), ==, -1);
	g_assert_cmpint(err, ==, ENOENT);

	/* What it is: a directory of the temporary filesystem, not its top. */
	g_assert_true(axis_storage_fs_identify(fd, &identity, &err));
	g_assert_false(identity.mount_root);
	g_assert_cmpuint(identity.ino, >, 0);
	g_assert_true(axis_storage_fs_info(fd, &info, &err));
	g_assert_cmpuint(info.total_bytes, >, 0);
	g_assert_cmpuint(info.free_bytes, <=, info.total_bytes);
	g_assert_cmpuint(info.available_bytes, <=, info.free_bytes);
	g_assert_false(info.read_only);
	g_assert_true(axis_storage_fs_is_empty(fd, &empty, &err));
	g_assert_false(empty);
	close(fd);

	fd = axis_storage_fs_open_path(deep, &err);
	g_assert_true(axis_storage_fs_is_empty(fd, &empty, &err));
	g_assert_true(empty);
	close(fd);

	/* "/" itself is a mount root. */
	fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_true(axis_storage_fs_identify(fd, &identity, &err));
	g_assert_true(identity.mount_root);
	close(fd);

	axis_test_storage_dir_free(dir);
}

/* ── What each part of a filesystem's identity is worth ─────────── */

#define XFS_MAGIC   G_GUINT64_CONSTANT(0x58465342)
#define EXT4_MAGIC  G_GUINT64_CONSTANT(0xEF53)
#define ZFS_MAGIC   G_GUINT64_CONSTANT(0x2FC12FC1)
#define XFS_UUID    "4f2a9c1e7b3d4e5fa6b7c8d9e0f1a2b3"
#define OTHER_FS_UUID "0a1b2c3d4e5f60718293a4b5c6d7e8f9"

/* What a probe of a directory found, made up. */
static void
made_up_probe(
	AxisVolumeProbe *probe,
	guint64          magic,
	const gchar     *fsid,
	const gchar     *fs_uuid,
	guint64          dev,
	guint64          ino,
	guint64          bytes
){
	memset(probe, 0, sizeof(*probe));
	probe->fd = -1;
	probe->held_fd = -1;
	probe->info_ok = TRUE;
	probe->info.magic = magic;
	g_strlcpy(probe->info.fsid, fsid, sizeof(probe->info.fsid));
	probe->info.total_bytes = bytes;
	probe->identity.dev = dev;
	probe->identity.ino = ino;
	g_strlcpy(probe->fs_uuid, fs_uuid != NULL ? fs_uuid : "", sizeof(probe->fs_uuid));
}

static void
made_up_identity(
	AxisVolumeFsIdentity *identity,
	guint64               magic,
	const gchar          *fsid,
	const gchar          *fs_uuid,
	guint64               ino,
	guint64               bytes
){
	memset(identity, 0, sizeof(*identity));
	identity->magic = magic;
	g_strlcpy(identity->fsid, fsid, sizeof(identity->fsid));
	g_strlcpy(identity->fs_uuid, fs_uuid != NULL ? fs_uuid : "", sizeof(identity->fs_uuid));
	identity->ino = ino;
	identity->bytes = bytes;
}

static void
assert_differs(
	const AxisVolumeFsIdentity *identity,
	const AxisVolumeProbe      *probe,
	const gchar                *part
){
	const gchar *what = NULL;

	g_assert_false(axis_volume_fs_identity_matches(identity, probe, &what));
	g_assert_cmpstr(what, ==, part);
}

/*
 * Storage hunt, H4: an xfs disk added without a marker, whose kernel gave a
 * UUID, has for an identifier only its device's number (8:1 when added).  A
 * reboot finds the disks in another order: 8:17 now, the identifier with
 * it.  The UUID decides, not the device's number; another UUID, or none
 * given now, is another filesystem.  The same for re-pointing, which asks
 * the same question.
 */
static void
test_identity_device_number(void)
{
	AxisVolumeFsIdentity identity;
	AxisVolumeProbe probe;

	made_up_identity(&identity, XFS_MAGIC, "0000080100000000", XFS_UUID, 128, G_GUINT64_CONSTANT(4000000000000));
	made_up_probe(&probe, XFS_MAGIC, "0000080100000000", XFS_UUID, makedev(8, 1), 128,
		G_GUINT64_CONSTANT(4000000000000));
	g_assert_true(axis_volume_fs_identity_matches(&identity, &probe, NULL));

	/* After the reboot: another device number, the same UUID. */
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 128,
		G_GUINT64_CONSTANT(4000000000000));
	g_assert_true(axis_volume_fs_identity_matches(&identity, &probe, NULL));

	/* Another disk at 8:1, or at 8:17, with another UUID: not it. */
	made_up_probe(&probe, XFS_MAGIC, "0000080100000000", OTHER_FS_UUID, makedev(8, 1), 128,
		G_GUINT64_CONSTANT(4000000000000));
	assert_differs(&identity, &probe, "the filesystem's UUID");
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", OTHER_FS_UUID, makedev(8, 17), 128,
		G_GUINT64_CONSTANT(4000000000000));
	assert_differs(&identity, &probe, "the filesystem's UUID");
	/* The kernel gives no UUID now: never recognised by the device alone. */
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", NULL, makedev(8, 17), 128,
		G_GUINT64_CONSTANT(4000000000000));
	assert_differs(&identity, &probe, "the filesystem's UUID, which the kernel does not give now");
	/* Another type, another directory. */
	made_up_probe(&probe, EXT4_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 128,
		G_GUINT64_CONSTANT(4000000000000));
	assert_differs(&identity, &probe, "the filesystem's type");
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 131,
		G_GUINT64_CONSTANT(4000000000000));
	assert_differs(&identity, &probe, "the directory (its inode)");

	/* An identifier of the filesystem's own (ext4, from its UUID) is
	 * compared, UUID or not, whatever the device's number. */
	made_up_identity(&identity, EXT4_MAGIC, "03350eabfcdf1e9a", XFS_UUID, 2, G_GUINT64_CONSTANT(2000000000000));
	made_up_probe(&probe, EXT4_MAGIC, "03350eabfcdf1e9a", XFS_UUID, makedev(259, 9), 2,
		G_GUINT64_CONSTANT(2000000000000));
	g_assert_true(axis_volume_fs_identity_matches(&identity, &probe, NULL));
	made_up_probe(&probe, EXT4_MAGIC, "03350eabfcdf1e9b", XFS_UUID, makedev(259, 9), 2,
		G_GUINT64_CONSTANT(2000000000000));
	assert_differs(&identity, &probe, "the filesystem's identifier");
}

/*
 * Storage hunt, H5: the size is no identity where something stronger
 * matched.  A read-only ZFS dataset reports its size with its pool's free
 * space, which moves as the pool is written; an ext4 grown, a new size.  The
 * same identifier of its own (or UUID): the same filesystem.
 */
static void
test_identity_size(void)
{
	AxisVolumeFsIdentity identity;
	AxisVolumeProbe probe;

	made_up_identity(&identity, ZFS_MAGIC, "7d3a0c94e5b2f106", NULL, 34, G_GUINT64_CONSTANT(900000000000));
	made_up_probe(&probe, ZFS_MAGIC, "7d3a0c94e5b2f106", NULL, makedev(0, 61), 34, G_GUINT64_CONSTANT(899000000000));
	g_assert_true(axis_volume_fs_identity_matches(&identity, &probe, NULL));
	/* Another dataset of the pool: another identifier. */
	made_up_probe(&probe, ZFS_MAGIC, "7d3a0c94e5b2f107", NULL, makedev(0, 61), 34, G_GUINT64_CONSTANT(900000000000));
	assert_differs(&identity, &probe, "the filesystem's identifier");

	made_up_identity(&identity, XFS_MAGIC, "0000080100000000", XFS_UUID, 128, G_GUINT64_CONSTANT(4000000000000));
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 128,
		G_GUINT64_CONSTANT(8000000000000));
	g_assert_true(axis_volume_fs_identity_matches(&identity, &probe, NULL));
}

static AxisVolumeInfo *
made_up_volume(
	const gchar *fsid,
	const gchar *device,
	const gchar *fs_uuid
){
	AxisVolumeInfo *row = g_new0(AxisVolumeInfo, 1);

	row->id = 1;
	row->uuid = g_strdup("6a1f0e2d-3c4b-4a59-8687-9a0b1c2d3e4f");
	row->name = g_strdup("First");
	row->path = g_strdup("/media/first");
	row->fs_type = g_strdup("xfs");
	row->fsid = g_strdup(fsid);
	row->device = g_strdup(device);
	row->state = AXIS_VOLUME_OFFLINE;
	row->has_marker = TRUE;
	/* What its last check found of the filesystem's UUID. */
	row->fs_uuid = g_strdup(fs_uuid);
	return row;
}

/* Storage hunt, H3: what proves a directory on another volume's filesystem,
 * whether or not that volume is online. */
static void
test_same_filesystem_proof(void)
{
	g_autoptr(AxisVolumeInfo) own = made_up_volume("03350eabfcdf1e9a", "259:4", NULL);
	g_autoptr(AxisVolumeInfo) xfs = made_up_volume("0000080100000000", "8:1", NULL);
	g_autoptr(AxisVolumeInfo) xfs_uuid = made_up_volume("0000080100000000", "8:1", XFS_UUID);
	g_autoptr(AxisVolumeInfo) unmarked = made_up_volume("0000080100000000", "8:1", NULL);
	g_autoptr(AxisVolumeInfo) subvolume = made_up_volume("7a11e0d3b2c19f08", "0:52", XFS_UUID);
	AxisVolumeProbe probe;

	/* A volume without a marker: its recorded identity's UUID counts too. */
	unmarked->has_marker = FALSE;
	g_strlcpy(unmarked->fs_identity.fsid, "0000080100000000", sizeof(unmarked->fs_identity.fsid));
	g_strlcpy(unmarked->fs_identity.fs_uuid, XFS_UUID, sizeof(unmarked->fs_identity.fs_uuid));

	/* Its identifier of its own, the one recorded: proven, on any device. */
	made_up_probe(&probe, EXT4_MAGIC, "03350eabfcdf1e9a", NULL, makedev(259, 9), 2, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(own, &probe), ==, AXIS_VOLUME_FS_PROVEN);
	made_up_probe(&probe, EXT4_MAGIC, "03350eabfcdf1e9b", NULL, makedev(259, 9), 2, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(own, &probe), ==, AXIS_VOLUME_FS_OTHER);
	/* Another filesystem on the device the volume was last seen on. */
	made_up_probe(&probe, EXT4_MAGIC, "1111111122222222", NULL, makedev(259, 4), 2, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(own, &probe), ==, AXIS_VOLUME_FS_SAME_DEVICE);

	/* xfs: the identifier is the device's number; the same number proves
	 * nothing more than the device (another disk can be found there). */
	made_up_probe(&probe, XFS_MAGIC, "0000080100000000", NULL, makedev(8, 1), 128, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(xfs, &probe), ==, AXIS_VOLUME_FS_SAME_DEVICE);
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", NULL, makedev(8, 17), 128, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(xfs, &probe), ==, AXIS_VOLUME_FS_OTHER);
	/* A volume with a marker whose last check recorded the filesystem's
	 * UUID (review of the fixes, item 1): its disk back at another device
	 * number, at another mount point, is proven its filesystem by the UUID,
	 * on any device; another UUID is not. */
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 128, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(xfs_uuid, &probe), ==, AXIS_VOLUME_FS_PROVEN);
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", OTHER_FS_UUID, makedev(8, 17), 128, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(xfs_uuid, &probe), ==, AXIS_VOLUME_FS_OTHER);
	/* The same for one without a marker, by the UUID it is recognised by. */
	made_up_probe(&probe, XFS_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 128, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(unmarked, &probe), ==, AXIS_VOLUME_FS_PROVEN);

	/* A filesystem with an identifier of its own per part (btrfs's
	 * subvolumes share one UUID): its identifier decides, not the UUID. */
	made_up_probe(&probe, G_GUINT64_CONSTANT(0x9123683E), "7a11e0d3b2c19f09", XFS_UUID, makedev(0, 53), 256, 1);
	g_assert_cmpint(axis_volume_probe_same_filesystem(subvolume, &probe), ==, AXIS_VOLUME_FS_OTHER);
}

/* The restore page's comparison of the filesystem at a volume's path with
 * the one the volume was last seen on. */
static void
test_restore_compare(void)
{
	g_autoptr(AxisVolumeInfo) own = made_up_volume("03350eabfcdf1e9a", "259:4", NULL);
	g_autoptr(AxisVolumeInfo) xfs = made_up_volume("0000080100000000", "8:1", NULL);
	g_autoptr(AxisVolumeInfo) xfs_uuid = made_up_volume("0000080100000000", "8:1", XFS_UUID);
	g_autoptr(AxisVolumeInfo) none = made_up_volume("", "", NULL);
	g_autofree gchar *text = NULL;

	g_free(own->fs_type);
	own->fs_type = g_strdup("ext4");
	xfs->has_space = TRUE;
	xfs->total_bytes = G_GUINT64_CONSTANT(4000000000000);
	g_assert_cmpint(axis_volume_restore_compare(own, EXT4_MAGIC, "03350eabfcdf1e9a", NULL, makedev(259, 9), 1, &text),
		==, AXIS_MARKER_RESTORE_FS_SAME);
	g_assert_nonnull(strstr(text, "is the one this disk was last seen on"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(axis_volume_restore_compare(own, EXT4_MAGIC, "4444444455555555", NULL, makedev(259, 4), 1, &text),
		==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	g_assert_nonnull(strstr(text, "its identifier was 03350eabfcdf1e9a, and it is 4444444455555555 now"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(axis_volume_restore_compare(own, XFS_MAGIC, "03350eabfcdf1e9a", NULL, makedev(259, 4), 1, &text),
		==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	g_assert_nonnull(strstr(text, "that was ext4, and this is xfs"));
	g_clear_pointer(&text, g_free);

	/* xfs, no UUID recorded: the same device number and size is all that
	 * can be compared. */
	g_assert_cmpint(axis_volume_restore_compare(xfs, XFS_MAGIC, "0000080100000000", XFS_UUID, makedev(8, 1),
		G_GUINT64_CONSTANT(4000000000000), &text), ==, AXIS_MARKER_RESTORE_FS_DEVICE_ONLY);
	g_assert_nonnull(strstr(text, "cannot tell"));
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(axis_volume_restore_compare(xfs, XFS_MAGIC, "0000080100000000", NULL, makedev(8, 1),
		G_GUINT64_CONSTANT(2000000000000), &text), ==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(axis_volume_restore_compare(xfs, XFS_MAGIC, "0000081100000000", NULL, makedev(8, 17),
		G_GUINT64_CONSTANT(4000000000000), &text), ==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	g_assert_nonnull(strstr(text, "found in another order"));
	g_clear_pointer(&text, g_free);

	/* xfs with the UUID its last check recorded: the UUID decides, at any
	 * device number. */
	g_assert_cmpint(axis_volume_restore_compare(xfs_uuid, XFS_MAGIC, "0000081100000000", XFS_UUID, makedev(8, 17), 1,
		&text), ==, AXIS_MARKER_RESTORE_FS_SAME);
	g_assert_nonnull(strstr(text, XFS_UUID));
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(axis_volume_restore_compare(xfs_uuid, XFS_MAGIC, "0000080100000000", OTHER_FS_UUID, makedev(8, 1),
		1, &text), ==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	g_assert_nonnull(strstr(text, "its UUID was"));
	g_clear_pointer(&text, g_free);

	g_assert_cmpint(axis_volume_restore_compare(none, XFS_MAGIC, "0000080100000000", NULL, makedev(8, 1), 1, &text), ==,
		AXIS_MARKER_RESTORE_FS_UNKNOWN);
	g_assert_nonnull(text);
}

/* A file at @dir/@name holding @text, its times set @age seconds back (a
 * negative age: ahead). */
static void
temporary_at(
	const gchar *dir,
	const gchar *name,
	const gchar *text,
	gint64       age
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	struct timespec times[2];

	axis_test_storage_write(path, text);
	times[0].tv_sec = (time_t)(g_get_real_time() / G_USEC_PER_SEC - age);
	times[0].tv_nsec = 0;
	times[1] = times[0];
	g_assert_cmpint(utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW), ==, 0);
}

static gboolean
exists_in(
	const gchar *dir,
	const gchar *name
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);

	return g_file_test(path, G_FILE_TEST_EXISTS);
}

/*
 * Review of the fixes, item 5: what an interrupted marker write left is
 * removed by what two looks found, never by a file's time against a clock.
 * A file a year old by its time is kept at the first look; it goes at a
 * look ten minutes (by the monotonic clock) after one that found the very
 * same file; one that changed between the looks (a write in progress)
 * starts again, as does one first seen at the later look; one with a time
 * in the future (a server's clock ahead) goes as the old one; and a name
 * this process's own marker write made is never removed.
 */
static void
test_marker_leftovers(void)
{
	gchar *dir = axis_test_storage_dir_new();
	g_autoptr(GHashTable) seen = axis_volume_marker_seen_new();
	g_autofree gchar *own = NULL;
	g_autofree gchar *writing = NULL;
	AxisVolumeMarker marker;
	gint64 start = G_GINT64_CONSTANT(5000000000);
	gint64 age = (gint64)AXIS_VOLUME_MARKER_STALE_SECONDS * G_USEC_PER_SEC;
	gint top;

	top = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(top, >=, 0);
	/* This process's own write: its temporary name, left as a crash of a
	 * write in progress would leave it. */
	g_assert_true(axis_volume_marker_new(&marker, NULL));
	g_assert_true(axis_volume_marker_write(top, &marker, NULL));
	own = axis_volume_marker_test_last_temporary();
	g_assert_nonnull(own);
	temporary_at(dir, own, "a marker being written\n", 365 * 24 * 3600);
	temporary_at(dir, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789abcdef", "a marker being written\n", 365 * 24 * 3600);
	temporary_at(dir, AXIS_VOLUME_MARKER_NAME ".tmp-1111111111111111", "a marker being written\n", -3600);
	temporary_at(dir, AXIS_VOLUME_MARKER_NAME ".tmp-fedcba9876543210", "a marker", 10);

	/* The first look removes nothing, however old a file's time. */
	g_assert_cmpuint(axis_volume_marker_remove_stale(top, seen, start, age, NULL), ==, 0);
	g_assert_true(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789abcdef"));
	/* Too soon after it: nothing either. */
	g_assert_cmpuint(axis_volume_marker_remove_stale(top, seen, start + age - 1, age, NULL), ==, 0);

	/* A write goes on in one; another appears. */
	writing = g_build_filename(dir, AXIS_VOLUME_MARKER_NAME ".tmp-fedcba9876543210", NULL);
	axis_test_storage_write(writing, "a marker being written, more of it\n");
	temporary_at(dir, AXIS_VOLUME_MARKER_NAME ".tmp-2222222222222222", "a marker being written\n", 365 * 24 * 3600);

	/* Ten minutes after the first look: the two found the same go. */
	g_assert_cmpuint(axis_volume_marker_remove_stale(top, seen, start + age, age, NULL), ==, 2);
	g_assert_false(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789abcdef"));
	g_assert_false(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-1111111111111111"));
	g_assert_true(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-fedcba9876543210"));
	g_assert_true(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-2222222222222222"));
	g_assert_true(exists_in(dir, own));

	/* Ten minutes later again: those unchanged since go; never its own. */
	g_assert_cmpuint(axis_volume_marker_remove_stale(top, seen, start + 2 * age, age, NULL), ==, 2);
	g_assert_false(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-fedcba9876543210"));
	g_assert_false(exists_in(dir, AXIS_VOLUME_MARKER_NAME ".tmp-2222222222222222"));
	g_assert_true(exists_in(dir, own));
	g_assert_true(exists_in(dir, AXIS_VOLUME_MARKER_NAME));
	close(top);
	axis_test_storage_dir_free(dir);
}

/*
 * Storage hunt, minor (d): reading the marker and looking whether the
 * volume's top is empty, at every check, write no access time where the
 * kernel allows that (to the file's owner), and still work where it does
 * not.  The access times are set a year back, before the last change, where
 * a read would set them again (relatime does).
 */
static void
test_no_access_time(void)
{
	gchar *dir = axis_test_storage_dir_new();
	AxisVolumeMarker marker;
	AxisVolumeMarker back;
	struct timespec times[2];
	struct stat st;
	gboolean empty = TRUE;
	gint top;
	gint root;
	gint fd;

	top = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(top, >=, 0);
	g_assert_true(axis_volume_marker_new(&marker, NULL));
	g_assert_true(axis_volume_marker_write(top, &marker, NULL));
	times[0].tv_sec = (time_t)(g_get_real_time() / G_USEC_PER_SEC) - 365 * 24 * 3600;
	times[0].tv_nsec = 0;
	times[1].tv_sec = 0;
	times[1].tv_nsec = UTIME_OMIT;
	g_assert_cmpint(utimensat(top, AXIS_VOLUME_MARKER_NAME, times, AT_SYMLINK_NOFOLLOW), ==, 0);
	g_assert_cmpint(futimens(top, times), ==, 0);

	g_assert_cmpint(axis_volume_marker_read(top, &back, NULL), ==, AXIS_VOLUME_MARKER_FOUND);
	g_assert_cmpstr(back.uuid, ==, marker.uuid);
	g_assert_true(axis_storage_fs_is_empty(top, &empty, NULL));
	g_assert_false(empty);
	g_assert_cmpint(fstatat(top, AXIS_VOLUME_MARKER_NAME, &st, AT_SYMLINK_NOFOLLOW), ==, 0);
	g_assert_cmpint((gint64)st.st_atim.tv_sec, ==, (gint64)times[0].tv_sec);
	g_assert_cmpint(fstat(top, &st), ==, 0);
	g_assert_cmpint((gint64)st.st_atim.tv_sec, ==, (gint64)times[0].tv_sec);
	close(top);

	/* Not the owner (/etc is root's, unless this runs as root): opened all
	 * the same, without O_NOATIME. */
	root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(root, >=, 0);
	fd = axis_storage_fs_openat_noatime(root, "etc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	close(root);
	axis_test_storage_dir_free(dir);
}

/* What a disk without a marker can be recognised by: an identifier of the
 * filesystem's own, never a device number. */
static void
test_fs_identifier(void)
{
	gchar *dir = axis_test_storage_dir_new();
	gchar uuid[AXIS_STORAGE_FS_UUID_CHARS + 1];
	AxisStorageFsInfo info;
	AxisStorageFsIdentity identity;
	gint err = 0;
	gint fd;

	/* xfs, exFAT, FAT, f2fs: f_fsid is new_encode_dev(st_dev), then zero. */
	g_assert_true(axis_storage_fs_fsid_is_device("0000080100000000", makedev(8, 1)));
	g_assert_true(axis_storage_fs_fsid_is_device("0000fd0300000000", makedev(253, 3)));
	/* A minor number above 255 is spread as the kernel spreads it. */
	g_assert_true(axis_storage_fs_fsid_is_device("0010031000000000", makedev(3, 0x110)));
	g_assert_true(axis_storage_fs_fsid_is_device("0000000000000000", makedev(8, 1)));
	g_assert_true(axis_storage_fs_fsid_is_device("", makedev(8, 1)));
	g_assert_true(axis_storage_fs_fsid_is_device(NULL, makedev(8, 1)));
	/* ext4's (from its UUID) and anything else are not. */
	g_assert_false(axis_storage_fs_fsid_is_device("03350eabfcdf1e9a", makedev(259, 4)));
	g_assert_false(axis_storage_fs_fsid_is_device("0000080100000000", makedev(8, 2)));
	g_assert_false(axis_storage_fs_fsid_is_device("0000080100000001", makedev(8, 1)));

	g_assert_true(axis_storage_fs_type_is_local(0xEF53));
	g_assert_true(axis_storage_fs_type_is_local(0x9123683E));
	g_assert_true(axis_storage_fs_type_is_local(0x01021994));
	g_assert_false(axis_storage_fs_type_is_local(0x65735546));	/* fuse */
	g_assert_false(axis_storage_fs_type_is_local(0x6969));		/* nfs */
	g_assert_false(axis_storage_fs_type_is_local(0xFF534D42));	/* cifs */
	g_assert_false(axis_storage_fs_type_is_local(0x794C7630));	/* overlay */
	g_assert_false(axis_storage_fs_type_is_local(0));

	/* Whatever the kernel says of the scratch directory's filesystem, the
	 * answer is lower-case hexadecimal, or nothing. */
	uuid[0] = '\0';
	fd = axis_storage_fs_open_path(dir, &err);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(axis_storage_fs_info(fd, &info, &err));
	g_assert_true(axis_storage_fs_identify(fd, &identity, &err));
	if (axis_storage_fs_type_is_local(info.magic) && axis_storage_fs_read_uuid(fd, uuid)) {
		g_assert_cmpuint(strlen(uuid) % 2, ==, 0);
		g_assert_cmpuint(strspn(uuid, "0123456789abcdef"), ==, strlen(uuid));
		g_assert_cmpuint(strspn(uuid, "0"), <, strlen(uuid));
	} else {
		g_assert_cmpstr(uuid, ==, "");
	}
	close(fd);
	axis_test_storage_dir_free(dir);
}

/* ── The helper with deadlines ──────────────────────────────────── */

typedef struct
{
	AxisTestGate *gate;
	gint          fd;
	gint         *freed;
} HangData;

static void
hang_run(gpointer data)
{
	HangData *hang = data;

	/* Holds a descriptor, as a check stuck on a disk would. */
	hang->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	axis_test_gate_pass(hang->gate);
}

static void
hang_free(gpointer data)
{
	HangData *hang = data;

	if (hang->fd >= 0)
		close(hang->fd);
	g_atomic_int_inc(hang->freed);
	g_free(hang);
}

static AxisStorageProbeCall *
start_hang(
	AxisStorageProbe  *probe,
	const gchar       *key,
	AxisTestGate      *gate,
	gint              *freed,
	GError           **error
){
	HangData *hang = g_new0(HangData, 1);

	hang->gate = gate;
	hang->fd = -1;
	hang->freed = freed;
	return axis_storage_probe_start(probe, key, hang_run, hang, hang_free, error);
}

static void
quick_run(gpointer data)
{
	*(gint *)data = 42;
}

/* Spec: Hung filesystem (the bound on what hangs cost) */
static void
test_probe_bounds(void)
{
	AxisStorageProbe *probe = axis_storage_probe_new(3);
	AxisTestGate *gate = axis_test_gate_new();
	guint fds_before = axis_test_storage_count_fds();
	gint freed = 0;
	gint64 started;
	guint i;
	gint64 deadline;

	/* Three hung calls on three keys miss their deadlines... */
	for (i = 0; i < 3; i++) {
		g_autofree gchar *key = g_strdup_printf("volume:%u", i);
		g_autoptr(GError) error = NULL;
		AxisStorageProbeCall *call = start_hang(probe, key, gate, &freed, &error);

		g_assert_no_error(error);
		g_assert_nonnull(call);
		started = g_get_monotonic_time();
		g_assert_false(axis_storage_probe_wait(call, started + 50 * 1000));
		g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
		axis_storage_probe_call_unref(call);
	}
	g_assert_cmpuint(axis_storage_probe_get_running(probe), ==, 3);
	g_assert_cmpuint(axis_storage_probe_get_abandoned(probe), ==, 3);

	/* ...a fourth key gets no thread: the helper is full... */
	for (i = 0; i < 20; i++) {
		g_autoptr(GError) error = NULL;
		gint *slot = g_new0(gint, 1);

		g_assert_null(axis_storage_probe_start(probe, "volume:other", quick_run, slot, g_free,
			&error));
		g_assert_error(error, AXIS_STORAGE_PROBE_ERROR, AXIS_STORAGE_PROBE_ERROR_FULL);
	}

	/* ...and a hung key gets no second thread however often it is asked. */
	for (i = 0; i < 20; i++) {
		g_autoptr(GError) error = NULL;

		g_assert_null(start_hang(probe, "volume:0", gate, &freed, &error));
		g_assert_error(error, AXIS_STORAGE_PROBE_ERROR, AXIS_STORAGE_PROBE_ERROR_STUCK);
	}
	/* The refused calls' data was freed at once; the hung ones' is held. */
	g_assert_cmpint(g_atomic_int_get(&freed), ==, 20);
	g_assert_cmpuint(axis_storage_probe_get_running(probe), ==, 3);
	g_assert_cmpuint(axis_test_storage_count_fds(), <=, fds_before + 3);

	/* When the kernel lets go, every thread ends and frees what it held. */
	axis_test_gate_open(gate);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (axis_storage_probe_get_running(probe) > 0 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_cmpuint(axis_storage_probe_get_running(probe), ==, 0);
	g_assert_cmpuint(axis_storage_probe_get_abandoned(probe), ==, 0);
	g_assert_cmpint(g_atomic_int_get(&freed), ==, 23);
	g_assert_cmpuint(axis_test_storage_count_fds(), ==, fds_before);

	/* And the keys are usable again. */
	{
		g_autoptr(GError) error = NULL;
		gint *slot = g_new0(gint, 1);
		AxisStorageProbeCall *call = axis_storage_probe_start(probe, "volume:0", quick_run, slot,
			g_free, &error);

		g_assert_no_error(error);
		g_assert_true(axis_storage_probe_wait(call, g_get_monotonic_time() + 10 * G_USEC_PER_SEC));
		g_assert_cmpint(*(gint *)axis_storage_probe_call_get_data(call), ==, 42);
		axis_storage_probe_call_unref(call);
	}

	axis_storage_probe_unref(probe);
	axis_test_gate_free(gate);
}

/* A hung call outlives the helper's owner without harm. */
static void
test_probe_outlives_owner(void)
{
	AxisStorageProbe *probe = axis_storage_probe_new(2);
	AxisTestGate *gate = axis_test_gate_new();
	gint freed = 0;
	AxisStorageProbeCall *call;
	gint64 deadline;

	call = start_hang(probe, "volume:1", gate, &freed, NULL);
	g_assert_nonnull(call);
	g_assert_false(axis_storage_probe_wait(call, g_get_monotonic_time() + 20 * 1000));
	axis_storage_probe_call_unref(call);
	axis_storage_probe_unref(probe);

	axis_test_gate_open(gate);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (g_atomic_int_get(&freed) == 0 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_cmpint(g_atomic_int_get(&freed), ==, 1);
	axis_test_gate_free(gate);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/storage/marker/round-trip", test_marker_round_trip);
	g_test_add_func("/storage/marker/refused", test_marker_refused);
	g_test_add_func("/storage/marker/uuid", test_uuid_validity);
	g_test_add_func("/storage/marker/file-write", test_marker_file_write);
	g_test_add_func("/storage/marker/file-hostile", test_marker_file_hostile);
	g_test_add_func("/storage/path/normalize", test_path_normalize);
	g_test_add_func("/storage/path/contains", test_path_contains);
	g_test_add_func("/storage/path/label", test_label_normalize);
	g_test_add_func("/storage/fs/walk", test_fs_walk);
	g_test_add_func("/storage/fs/identifier", test_fs_identifier);
	g_test_add_func("/storage/identity/device-number", test_identity_device_number);
	g_test_add_func("/storage/identity/size", test_identity_size);
	g_test_add_func("/storage/identity/same-filesystem", test_same_filesystem_proof);
	g_test_add_func("/storage/identity/restore-compare", test_restore_compare);
	g_test_add_func("/storage/fs/no-access-time", test_no_access_time);
	g_test_add_func("/storage/marker/leftovers", test_marker_leftovers);
	g_test_add_func("/storage/probe/bounds", test_probe_bounds);
	g_test_add_func("/storage/probe/outlives-owner", test_probe_outlives_owner);

	return g_test_run();
}
