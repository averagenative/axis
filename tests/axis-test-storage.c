/*
 * axis-test-storage.c - test support for the storage code
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-test-storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/axis-volume-marker.h"

#define SCRATCH_PREFIX "axis-storage-"

gchar *
axis_test_storage_dir_new(void)
{
	g_autoptr(GError) error = NULL;
	gchar *dir = g_dir_make_tmp(SCRATCH_PREFIX "XXXXXX", &error);

	g_assert_no_error(error);
	g_assert_nonnull(dir);
	return dir;
}

/* The names in the directory @fd (not consumed). */
static GPtrArray *
list_names(gint fd)
{
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	gint copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	DIR *dir;
	struct dirent *entry;

	if (copy < 0)
		return names;
	dir = fdopendir(copy);
	if (dir == NULL) {
		close(copy);
		return names;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			g_ptr_array_add(names, g_strdup(entry->d_name));
	}
	closedir(dir);
	g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
	return names;
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* Removes @name in @parent: never follows a link, never leaves @dev. */
static void
remove_at(
	gint         parent,
	const gchar *name,
	dev_t        dev
){
	struct stat st;

	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return;

	if (S_ISDIR(st.st_mode)) {
		GPtrArray *names;
		gint fd;
		guint i;

		if (st.st_dev != dev) {
			g_printerr("axis-test-storage: leaving %s, which is on another filesystem\n", name);
			return;
		}
		fchmodat(parent, name, 0700, 0);
		fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return;
		names = list_names(fd);
		for (i = 0; i < names->len; i++)
			remove_at(fd, g_ptr_array_index(names, i), dev);
		g_ptr_array_unref(names);
		close(fd);
		unlinkat(parent, name, AT_REMOVEDIR);
	} else {
		unlinkat(parent, name, 0);
	}
}

void
axis_test_storage_dir_free(gchar *dir)
{
	g_autofree gchar *parent_path = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *tmp = NULL;
	g_autofree gchar *parent_canonical = NULL;
	struct stat st;
	gint parent;

	if (dir == NULL)
		return;

	/* Only ever a scratch directory of ours. */
	base = g_path_get_basename(dir);
	parent_path = g_path_get_dirname(dir);
	tmp = g_canonicalize_filename(g_get_tmp_dir(), "/");
	parent_canonical = g_canonicalize_filename(parent_path, "/");
	g_assert_true(g_str_has_prefix(base, SCRATCH_PREFIX));
	g_assert_cmpstr(parent_canonical, ==, tmp);

	parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent >= 0 && fstatat(parent, base, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
		remove_at(parent, base, st.st_dev);
	if (parent >= 0)
		close(parent);
	g_free(dir);
}

void
axis_test_storage_mkdir(const gchar *path)
{
	g_assert_cmpint(g_mkdir_with_parents(path, 0755), ==, 0);
}

void
axis_test_storage_write(
	const gchar *path,
	const gchar *content
){
	g_autoptr(GError) error = NULL;

	g_assert_true(g_file_set_contents(path, content, -1, &error));
	g_assert_no_error(error);
}

void
axis_test_storage_write_marker(
	const gchar *dir,
	const gchar *uuid
){
	AxisVolumeMarker marker;
	gchar buffer[AXIS_VOLUME_MARKER_MAX_BYTES];
	g_autofree gchar *path = g_build_filename(dir, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autoptr(GError) error = NULL;
	gsize length;

	memset(&marker, 0, sizeof(marker));
	g_strlcpy(marker.uuid, uuid, sizeof(marker.uuid));
	marker.created = 1762942530;
	length = axis_volume_marker_format(&marker, buffer);
	g_assert_cmpuint(length, >, 0);
	g_assert_true(g_file_set_contents(path, buffer, (gssize)length, &error));
	g_assert_no_error(error);
}

gchar *
axis_test_storage_read(const gchar *path)
{
	gchar *content = NULL;

	if (!g_file_get_contents(path, &content, NULL, NULL))
		return NULL;
	return content;
}

static void
digest_at(
	gint         parent,
	const gchar *name,
	const gchar *shown,
	GString     *out
){
	struct stat st;

	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		g_string_append_printf(out, "%s gone\n", shown);
		return;
	}

	g_string_append_printf(out, "%s mode=%o size=%" G_GINT64_FORMAT " mtime=%" G_GINT64_FORMAT ".%09ld",
		shown, (guint)st.st_mode, (gint64)st.st_size, (gint64)st.st_mtim.tv_sec,
		(glong)st.st_mtim.tv_nsec);

	if (S_ISLNK(st.st_mode)) {
		gchar target[4096];
		ssize_t n = readlinkat(parent, name, target, sizeof(target) - 1);

		target[n > 0 ? n : 0] = '\0';
		g_string_append_printf(out, " link=%s\n", target);
	} else if (S_ISREG(st.st_mode)) {
		gint fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
		guchar buffer[8192];
		ssize_t n;

		while (fd >= 0 && (n = read(fd, buffer, sizeof(buffer))) > 0)
			g_checksum_update(sum, buffer, (gssize)n);
		if (fd >= 0)
			close(fd);
		g_string_append_printf(out, " sha256=%s\n", g_checksum_get_string(sum));
		g_checksum_free(sum);
	} else if (S_ISDIR(st.st_mode)) {
		gint fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		GPtrArray *names;
		guint i;

		g_string_append(out, " dir\n");
		if (fd < 0)
			return;
		names = list_names(fd);
		g_ptr_array_sort(names, compare_names);
		for (i = 0; i < names->len; i++) {
			const gchar *child = g_ptr_array_index(names, i);
			g_autofree gchar *child_shown = g_strconcat(shown, "/", child, NULL);

			digest_at(fd, child, child_shown, out);
		}
		g_ptr_array_unref(names);
		close(fd);
	} else {
		g_string_append(out, " other\n");
	}
}

gchar *
axis_test_storage_digest(const gchar *root)
{
	GString *out = g_string_new(NULL);
	g_autofree gchar *parent_path = g_path_get_dirname(root);
	g_autofree gchar *base = g_path_get_basename(root);
	gint parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	g_assert_cmpint(parent, >=, 0);
	digest_at(parent, base, ".", out);
	close(parent);

	return g_string_free(out, FALSE);
}

guint
axis_test_storage_count_fds(void)
{
	GDir *dir = g_dir_open("/proc/self/fd", 0, NULL);
	guint n = 0;

	g_assert_nonnull(dir);
	while (g_dir_read_name(dir) != NULL)
		n++;
	g_dir_close(dir);

	/* Not counting the one reading the list. */
	return n > 0 ? n - 1 : 0;
}

guint
axis_test_storage_count_temporaries(const gchar *dir)
{
	GDir *listing = g_dir_open(dir, 0, NULL);
	const gchar *name;
	guint n = 0;

	g_assert_nonnull(listing);
	while ((name = g_dir_read_name(listing)) != NULL) {
		if (g_str_has_prefix(name, AXIS_VOLUME_MARKER_NAME ".tmp"))
			n++;
	}
	g_dir_close(listing);

	return n;
}

struct _AxisTestGate
{
	GMutex   lock;
	GCond    changed;
	gboolean open;
	guint    arrived;
};

AxisTestGate *
axis_test_gate_new(void)
{
	AxisTestGate *gate = g_new0(AxisTestGate, 1);

	g_mutex_init(&gate->lock);
	g_cond_init(&gate->changed);
	return gate;
}

void
axis_test_gate_pass(AxisTestGate *gate)
{
	g_mutex_lock(&gate->lock);
	gate->arrived++;
	g_cond_broadcast(&gate->changed);
	while (!gate->open)
		g_cond_wait(&gate->changed, &gate->lock);
	g_mutex_unlock(&gate->lock);
}

void
axis_test_gate_open(AxisTestGate *gate)
{
	g_mutex_lock(&gate->lock);
	gate->open = TRUE;
	g_cond_broadcast(&gate->changed);
	g_mutex_unlock(&gate->lock);
}

guint
axis_test_gate_get_arrived(AxisTestGate *gate)
{
	guint n;

	g_mutex_lock(&gate->lock);
	n = gate->arrived;
	g_mutex_unlock(&gate->lock);

	return n;
}

void
axis_test_gate_free(AxisTestGate *gate)
{
	if (gate == NULL)
		return;

	g_mutex_clear(&gate->lock);
	g_cond_clear(&gate->changed);
	g_free(gate);
}
