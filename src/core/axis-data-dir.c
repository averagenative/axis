/*
 * axis-data-dir.c - the one directory the daemon keeps files of its own in
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "core/axis-data-dir.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/axis-error.h"

struct _AxisDataDir
{
	gint        refs;	/* atomic */
	gchar      *path;
	gint        fd;
	GMutex      lock;
	GHashTable *subdirs;	/* (lock) name -> GINT_TO_POINTER(fd + 1) */
};

/* ── Checks ─────────────────────────────────────────────────────── */

AxisDataDirOwner
axis_data_dir_owner_check(
	guint        uid,
	guint        gid,
	guint        mode,
	guint        euid,
	const guint *groups,
	guint        n_groups
){
	guint i;

	if ((mode & S_IWOTH) != 0)
		return AXIS_DATA_DIR_EVERYONE;
	if (uid == euid)
		return AXIS_DATA_DIR_OWN;
	/* A platform that assigns an arbitrary user id gives the directory to a
	 * group the process is in (group 0 on OpenShift, the pod's fsGroup on
	 * Kubernetes): accepted when that group may write and search it. */
	if ((mode & (S_IWGRP | S_IXGRP)) == (S_IWGRP | S_IXGRP)) {
		for (i = 0; i < n_groups; i++) {
			if (groups[i] == gid)
				return AXIS_DATA_DIR_GROUP;
		}
	}
	return AXIS_DATA_DIR_OTHER;
}

/* Whether the directory @fd (called @what in messages) is one the daemon
 * may keep its files in: a directory of its own user, or of one of its
 * groups with that group's write permission, that not everyone may write
 * in. */
static gboolean
check_directory(
	gint          fd,
	const gchar  *what,
	GError      **error
){
	struct stat st;
	gid_t supplementary[64];
	guint groups[65];
	gint n;
	gint i;

	if (fstat(fd, &st) != 0) {
		gint saved = errno;

		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s cannot be examined (%s)", what,
			g_strerror(saved));
		return FALSE;
	}
	if (!S_ISDIR(st.st_mode)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s is not a directory", what);
		return FALSE;
	}

	groups[0] = (guint)getegid();
	n = getgroups(G_N_ELEMENTS(supplementary), supplementary);
	if (n < 0)
		n = 0;
	for (i = 0; i < n; i++)
		groups[i + 1] = (guint)supplementary[i];

	switch (axis_data_dir_owner_check((guint)st.st_uid, (guint)st.st_gid, (guint)st.st_mode,
	                                  (guint)geteuid(), groups, (guint)n + 1)) {
	case AXIS_DATA_DIR_OWN:
	case AXIS_DATA_DIR_GROUP:
		return TRUE;
	case AXIS_DATA_DIR_EVERYONE:
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR,
			"%s can be written by every user (mode %04o): allow only its owner (chmod 700) or its "
			"group (chmod 770)", what, (guint)(st.st_mode & 07777));
		return FALSE;
	case AXIS_DATA_DIR_OTHER:
	default:
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR,
			"%s belongs to user %u, not to the user the daemon runs as (%u), and its group (%u) is "
			"not one of the daemon's with write permission: make it the daemon's (chown %u), or "
			"give it to one of the daemon's groups and let that group write (chgrp %u, chmod 770)",
			what, (guint)st.st_uid, (guint)geteuid(), (guint)st.st_gid, (guint)geteuid(),
			(guint)getegid());
		return FALSE;
	}
}

/* Opens @name beneath @dirfd as a directory, creating it if absent; never
 * through a symbolic link. */
static gint
open_or_create(
	gint          dirfd,
	const gchar  *name,
	const gchar  *what,
	GError      **error
){
	gint fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	gint saved;

	if (fd < 0 && errno == ENOENT) {
		if (mkdirat(dirfd, name, 0700) != 0 && errno != EEXIST) {
			saved = errno;
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s does not exist and cannot be created (%s)",
				what, g_strerror(saved));
			return -1;
		}
		fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	}
	if (fd < 0) {
		struct stat st;

		saved = errno;
		/* O_DIRECTORY with O_NOFOLLOW reports a link as "not a directory". */
		if ((saved == ENOTDIR || saved == ELOOP) && fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
		    S_ISLNK(st.st_mode))
			saved = ELOOP;
		if (saved == ELOOP)
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR,
				"%s is a symbolic link: name the directory itself", what);
		else if (saved == EACCES && fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_uid != geteuid())
			/* Most often a volume made for another user id. */
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR,
				"%s belongs to user %u, not to the user the daemon runs as (%u), and cannot be opened: make "
				"it the daemon's (chown %u)", what, (guint)st.st_uid, (guint)geteuid(), (guint)geteuid());
		else if (saved == ENOTDIR)
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s is not a directory", what);
		else
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s cannot be opened (%s)", what,
				g_strerror(saved));
		return -1;
	}
	if (!check_directory(fd, what, error)) {
		close(fd);
		return -1;
	}
	return fd;
}

/* A file can be made, written and removed in @fd. */
static gboolean
check_writable(
	gint          fd,
	const gchar  *what,
	GError      **error
){
	g_autofree gchar *name = g_strdup_printf(".axis-write-check-%d-%08x%08x", (gint)getpid(),
		g_random_int(), g_random_int());
	gint file = openat(fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	gint saved;
	gssize written;

	if (file < 0) {
		saved = errno;
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s cannot be written (%s)", what,
			g_strerror(saved));
		return FALSE;
	}
	written = write(file, "axis\n", 5);
	saved = errno;
	if (close(file) != 0 && written == 5) {
		written = -1;
		saved = errno;
	}
	unlinkat(fd, name, 0);
	if (written != 5) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s cannot be written (%s)", what,
			g_strerror(written < 0 ? saved : ENOSPC));
		return FALSE;
	}
	return TRUE;
}

/* ── The directory ──────────────────────────────────────────────── */

AxisDataDir *
axis_data_dir_open(
	const gchar  *path,
	GError      **error
){
	g_autofree gchar *parent = NULL;
	g_autofree gchar *what = NULL;
	const gchar *slash;
	AxisDataDir *self;
	gint parent_fd;
	gint fd;
	gint saved;

	g_return_val_if_fail(path != NULL, NULL);

	what = g_strdup_printf("the data directory %s", path);
	slash = strrchr(path, '/');
	if (path[0] != '/' || slash == NULL || slash[1] == '\0' || strcmp(slash + 1, ".") == 0 ||
	    strcmp(slash + 1, "..") == 0) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s is not an absolute path to a directory", what);
		return NULL;
	}
	parent = slash == path ? g_strdup("/") : g_strndup(path, (gsize)(slash - path));
	/* The way to the parent is the administrator's (it may cross a link);
	 * the directory itself is opened without following one. */
	parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent_fd < 0) {
		saved = errno;
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "%s cannot be used: its parent %s cannot be opened (%s)",
			what, parent, g_strerror(saved));
		return NULL;
	}
	fd = open_or_create(parent_fd, slash + 1, what, error);
	close(parent_fd);
	if (fd < 0)
		return NULL;
	if (!check_writable(fd, what, error)) {
		close(fd);
		return NULL;
	}

	self = g_new0(AxisDataDir, 1);
	self->refs = 1;
	self->path = g_strdup(path);
	self->fd = fd;
	g_mutex_init(&self->lock);
	self->subdirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	return self;
}

AxisDataDir *
axis_data_dir_ref(AxisDataDir *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

static void
close_subdir(
	gpointer key,
	gpointer value,
	gpointer user_data
){
	close(GPOINTER_TO_INT(value) - 1);
}

void
axis_data_dir_unref(AxisDataDir *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	g_hash_table_foreach(self->subdirs, close_subdir, NULL);
	g_hash_table_unref(self->subdirs);
	g_mutex_clear(&self->lock);
	close(self->fd);
	g_free(self->path);
	g_free(self);
}

const gchar *
axis_data_dir_get_path(AxisDataDir *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->path;
}

gint
axis_data_dir_get_fd(AxisDataDir *self)
{
	g_return_val_if_fail(self != NULL, -1);
	return self->fd;
}

static gboolean
subdir_name_valid(const gchar *name)
{
	const gchar *p;

	if (name == NULL || name[0] < 'a' || name[0] > 'z' || strlen(name) > 32)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
			return FALSE;
	}
	return TRUE;
}

gint
axis_data_dir_subdir(
	AxisDataDir  *self,
	const gchar  *name,
	GError      **error
){
	g_autofree gchar *what = NULL;
	gpointer found;
	gint fd;

	g_return_val_if_fail(self != NULL, -1);

	if (!subdir_name_valid(name)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR, "not a name for a part of the data directory");
		return -1;
	}
	g_mutex_lock(&self->lock);
	found = g_hash_table_lookup(self->subdirs, name);
	if (found != NULL) {
		g_mutex_unlock(&self->lock);
		return GPOINTER_TO_INT(found) - 1;
	}
	what = g_strdup_printf("%s/%s (in the data directory)", self->path, name);
	fd = open_or_create(self->fd, name, what, error);
	if (fd >= 0)
		g_hash_table_insert(self->subdirs, g_strdup(name), GINT_TO_POINTER(fd + 1));
	g_mutex_unlock(&self->lock);
	return fd;
}
