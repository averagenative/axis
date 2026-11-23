/*
 * axis-library-walk.c - walking a root's directory tree, on a thread of its
 * own, without following anything
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "library/axis-library-walk.h"
#include "library/axis-library-name.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_BATCH_ENTRIES 1000
#define BATCH_MAX_BYTES       (1024 * 1024)
#define MAX_WAITING_BATCHES   2
#define DEFAULT_RACY_NS       (G_GINT64_CONSTANT(2) * 1000000000)
#define NICE_INCREMENT        10

/* ioprio_set(2) has no glibc wrapper. */
#define IOPRIO_WHO_PROCESS    1
#define IOPRIO_CLASS_IDLE     3
#define IOPRIO_CLASS_SHIFT    13

#define STATX_WANTED (STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_MTIME | \
                      STATX_CTIME | STATX_MNT_ID)

static gint live_walks;	/* atomic */

static const gchar *const finding_names[] = {
	"other_filesystem", "too_deep", "too_long", "unreadable", "io_error", "vanished", "replaced",
	"partial"
};

G_STATIC_ASSERT(G_N_ELEMENTS(finding_names) == AXIS_WALK_FINDING_KIND_COUNT);

const gchar *
axis_walk_finding_kind_to_string(AxisWalkFindingKind kind)
{
	return (guint)kind < AXIS_WALK_FINDING_KIND_COUNT ? finding_names[kind] : "io_error";
}

/* One open directory on the way down. */
typedef struct
{
	DIR      *dir;
	gsize     path_len;	/* its stored path's length in the path buffer */
	gsize     raw_len;	/* its path's length in bytes as on disk */
	guint     depth;	/* 0 for the root */
	gboolean  invalid;	/* its path has a segment that is not UTF-8 */
	gboolean  failed;	/* stop reading it */
} Level;

struct _AxisLibraryWalk
{
	gint            ref_count;	/* atomic */
	gint            stop_flag;	/* atomic */
	gint            steps;		/* atomic, wraps */

	/* Fixed once started */
	guint           batch_entries;
	gint64          racy_ns;
	gchar         **patterns;
	AxisWalkHook    before_directory;
	AxisWalkHook    after_last;
	gpointer        hook_data;
	guint64         root_dev;
	guint64         root_ino;
	guint64         root_mnt;
	gboolean        has_root_mnt;

	GMutex          lock;
	GCond           changed;
	GQueue          waiting;	/* (lock) AxisWalkBatch*, oldest first */
	AxisWalkStatus  status;		/* (lock) */
	gboolean        thread_done;	/* (lock) */
	AxisWalkTotals  totals;		/* (lock) as published */
	gboolean        io_errors;	/* (lock) as published */

	/* The walk's thread only */
	gint            root_fd;
	AxisWalkTotals  local;
	gboolean        local_io_errors;
	guint           findings_kept;
	guint           partials_kept;
	AxisWalkBatch  *current;
	gsize           current_bytes;
};

/* ── Batches ────────────────────────────────────────────────────── */

static void
entry_clear(gpointer data)
{
	AxisWalkEntry *entry = data;

	g_free(entry->path);
	g_free(entry->target);
}

static void
finding_free(gpointer data)
{
	AxisWalkFinding *finding = data;

	g_free(finding->path);
	g_free(finding->detail);
	g_free(finding);
}

static AxisWalkBatch *
batch_new(void)
{
	AxisWalkBatch *batch = g_new0(AxisWalkBatch, 1);

	batch->entries = g_array_new(FALSE, TRUE, sizeof(AxisWalkEntry));
	g_array_set_clear_func(batch->entries, entry_clear);
	batch->unexamined = g_ptr_array_new_with_free_func(g_free);
	batch->findings = g_ptr_array_new_with_free_func(finding_free);
	return batch;
}

void
axis_walk_batch_free(AxisWalkBatch *batch)
{
	if (batch == NULL)
		return;

	g_array_unref(batch->entries);
	g_ptr_array_unref(batch->unexamined);
	g_ptr_array_unref(batch->findings);
	g_free(batch);
}

static gboolean
stopping(AxisLibraryWalk *self)
{
	return g_atomic_int_get(&self->stop_flag) != 0;
}

static gboolean
batch_is_empty(const AxisWalkBatch *batch)
{
	return batch->entries->len == 0 && batch->unexamined->len == 0 && batch->findings->len == 0;
}

/* Hands the current batch over, waiting while two are waiting already. */
static void
flush(AxisLibraryWalk *self)
{
	AxisWalkBatch *batch = self->current;

	self->current = batch_new();
	self->current_bytes = 0;

	g_mutex_lock(&self->lock);
	while (g_queue_get_length(&self->waiting) >= MAX_WAITING_BATCHES && !stopping(self))
		g_cond_wait(&self->changed, &self->lock);
	if (stopping(self)) {
		g_mutex_unlock(&self->lock);
		axis_walk_batch_free(batch);
		return;
	}
	g_queue_push_tail(&self->waiting, batch);
	self->totals = self->local;
	self->io_errors = self->local_io_errors;
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);
}

static void
maybe_flush(AxisLibraryWalk *self)
{
	if (self->current->entries->len >= self->batch_entries || self->current_bytes >= BATCH_MAX_BYTES ||
	    self->current->unexamined->len >= self->batch_entries)
		flush(self);
}

static void
not_examined(
	AxisLibraryWalk *self,
	const gchar     *path
){
	g_ptr_array_add(self->current->unexamined, g_strdup(path));
	self->current_bytes += strlen(path) + 32;
	maybe_flush(self);
}

static void
report(
	AxisLibraryWalk     *self,
	AxisWalkFindingKind  kind,
	const gchar         *path,
	const gchar         *detail
){
	AxisWalkFinding *finding;

	self->local.findings++;
	if (self->findings_kept >= AXIS_WALK_MAX_FINDINGS)
		return;
	if (kind == AXIS_WALK_FINDING_PARTIAL) {
		if (self->partials_kept >= AXIS_WALK_MAX_PARTIAL_FINDINGS)
			return;
		self->partials_kept++;
	}
	self->findings_kept++;

	finding = g_new0(AxisWalkFinding, 1);
	finding->kind = kind;
	finding->path = g_strdup(path);
	finding->detail = g_strdup(detail);
	g_ptr_array_add(self->current->findings, finding);
	self->current_bytes += strlen(path) + 64;
	maybe_flush(self);
}

/* ── Looking at things ──────────────────────────────────────────── */

static gint64
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (gint64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static gint64
timestamp_ns(const struct statx_timestamp *ts)
{
	/* Far past or future times cannot overflow: clamp. */
	if (ts->tv_sec > G_MAXINT64 / 1000000000 - 1)
		return G_MAXINT64;
	if (ts->tv_sec < G_MININT64 / 1000000000 + 1)
		return G_MININT64;
	return ts->tv_sec * 1000000000 + (gint64)ts->tv_nsec;
}

/* Whether @stx (a directory found in a listing) is on the root's mount. */
static gboolean
same_mount(
	AxisLibraryWalk    *self,
	const struct statx *stx
){
	guint64 dev = (guint64)makedev(stx->stx_dev_major, stx->stx_dev_minor);

	if (dev != self->root_dev)
		return FALSE;
	if (self->has_root_mnt && (stx->stx_mask & STATX_MNT_ID) && stx->stx_mnt_id != self->root_mnt)
		return FALSE;
	if ((stx->stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) &&
	    (stx->stx_attributes & STATX_ATTR_MOUNT_ROOT))
		return FALSE;
	return TRUE;
}

static gint
open_directory(
	gint         dirfd,
	const gchar *name
){
	const gint flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC;
	gint fd;

	/* Reading a directory updates its access time unless O_NOATIME, which
	 * only the owner (or a capable process) may ask for. */
	fd = openat(dirfd, name, flags | O_NOATIME);
	if (fd < 0 && errno == EPERM)
		fd = openat(dirfd, name, flags);
	return fd;
}

/* The finding for an error opening or reading a directory, and whether it
 * may mean a failing disk (then the scan cannot be complete). */
static AxisWalkFindingKind
classify_error(
	gint      err,
	gboolean *io_error
){
	*io_error = FALSE;
	switch (err) {
	case EACCES:
	case EPERM:
		return AXIS_WALK_FINDING_UNREADABLE;
	case ENOENT:
		return AXIS_WALK_FINDING_VANISHED;
	case ELOOP:
	case ENOTDIR:
		return AXIS_WALK_FINDING_REPLACED;
	case EXDEV:
		return AXIS_WALK_FINDING_OTHER_FS;
	default:
		*io_error = TRUE;
		return AXIS_WALK_FINDING_IO_ERROR;
	}
}

static void
count_finding(
	AxisLibraryWalk     *self,
	AxisWalkFindingKind  kind
){
	switch (kind) {
	case AXIS_WALK_FINDING_OTHER_FS:
		self->local.other_fs++;
		break;
	case AXIS_WALK_FINDING_TOO_DEEP:
		self->local.too_deep++;
		break;
	case AXIS_WALK_FINDING_TOO_LONG:
		self->local.too_long++;
		break;
	case AXIS_WALK_FINDING_UNREADABLE:
		self->local.unreadable++;
		break;
	case AXIS_WALK_FINDING_VANISHED:
		self->local.vanished++;
		break;
	case AXIS_WALK_FINDING_REPLACED:
		self->local.replaced++;
		break;
	case AXIS_WALK_FINDING_PARTIAL:
		self->local.partials++;
		break;
	case AXIS_WALK_FINDING_IO_ERROR:
	default:
		self->local.io_errors++;
		self->local_io_errors = TRUE;
		break;
	}
}

/* @path (and all under it) was not looked at, for @kind. */
static void
skip(
	AxisLibraryWalk     *self,
	AxisWalkFindingKind  kind,
	const gchar         *path,
	const gchar         *detail
){
	count_finding(self, kind);
	report(self, kind, path, detail);
	not_examined(self, path);
}

static void
skip_errno(
	AxisLibraryWalk *self,
	const gchar     *path,
	gint             err
){
	gboolean io_error;
	AxisWalkFindingKind kind = classify_error(err, &io_error);

	skip(self, kind, path, g_strerror(err));
}

static AxisIgnoreReason
ignored(
	AxisLibraryWalk *self,
	const gchar     *name,
	gsize            length,
	gboolean         is_dir
){
	AxisIgnoreReason reason = axis_library_ignore_builtin(name, length, is_dir);
	guint i;

	if (reason != AXIS_IGNORE_NONE)
		return reason;
	for (i = 0; self->patterns != NULL && self->patterns[i] != NULL; i++) {
		if (axis_library_glob_match(self->patterns[i], name, length))
			return AXIS_IGNORE_USER;
	}

	return AXIS_IGNORE_NONE;
}

/* Counts an ignored name; nothing under it is examined. */
static void
leave_out(
	AxisLibraryWalk  *self,
	AxisIgnoreReason  reason,
	const gchar      *path
){
	if (reason == AXIS_IGNORE_PARTIAL) {
		count_finding(self, AXIS_WALK_FINDING_PARTIAL);
		report(self, AXIS_WALK_FINDING_PARTIAL, path, NULL);
	} else {
		self->local.ignored++;
	}
	not_examined(self, path);
}

static void
add_entry(
	AxisLibraryWalk    *self,
	const gchar        *path,
	gboolean            invalid,
	AxisFileKind        kind,
	const struct statx *stx,
	gint64              looked_ns,
	gchar              *target
){
	AxisWalkEntry entry;
	gint64 latest = MAX(timestamp_ns(&stx->stx_mtime), timestamp_ns(&stx->stx_ctime));

	memset(&entry, 0, sizeof(entry));
	entry.path = g_strdup(path);
	entry.invalid = invalid;
	entry.kind = kind;
	entry.size = (gint64)MIN(stx->stx_size, (guint64)G_MAXINT64);
	entry.mtime_sec = stx->stx_mtime.tv_sec;
	entry.mtime_nsec = (gint32)MIN(stx->stx_mtime.tv_nsec, 999999999u);
	entry.ctime_sec = stx->stx_ctime.tv_sec;
	entry.ctime_nsec = (gint32)MIN(stx->stx_ctime.tv_nsec, 999999999u);
	entry.inode = stx->stx_ino;
	entry.nlink = stx->stx_nlink;
	/* Modified within the window before the look (or "in the future"): a
	 * later modification within the same timestamp tick would look the
	 * same, so the next look must not take "the same" as unchanged. */
	entry.racy = latest >= looked_ns - self->racy_ns;
	entry.target = target;

	g_array_append_val(self->current->entries, entry);
	self->current_bytes += strlen(path) + (target != NULL ? strlen(target) : 0) + 96;

	self->local.kind_files[kind]++;
	self->local.kind_bytes[kind] += (guint64)entry.size;
	maybe_flush(self);
}

/* The target of the link @name in @dirfd, stored form, or NULL. */
static gchar *
read_target(
	gint         dirfd,
	const gchar *name
){
	gchar buffer[AXIS_LIBRARY_PATH_MAX + 1];
	GString *out;
	ssize_t n;

	n = readlinkat(dirfd, name, buffer, sizeof(buffer));
	if (n < 0 || (gsize)n >= sizeof(buffer))
		return NULL;

	out = g_string_sized_new((gsize)n + 8);
	axis_library_name_encode(out, buffer, (gsize)n);
	return g_string_free(out, FALSE);
}

/* Opens the subdirectory @name of @top (stored path @path) and pushes it,
 * after checking that what was opened is what was listed. */
static void
descend(
	AxisLibraryWalk    *self,
	GArray             *stack,
	guint               top_index,
	const gchar        *name,
	const gchar        *path,
	gsize               raw_len,
	gboolean            invalid,
	const struct statx *listed
){
	Level *top = &g_array_index(stack, Level, top_index);
	struct statx opened;
	Level level;
	guint depth = top->depth + 1;
	gint fd;
	DIR *dir;

	if (!same_mount(self, listed)) {
		skip(self, AXIS_WALK_FINDING_OTHER_FS, path, "another filesystem or mount");
		return;
	}
	/* Its entries would be deeper than allowed. */
	if (depth >= AXIS_LIBRARY_DEPTH_MAX) {
		skip(self, AXIS_WALK_FINDING_TOO_DEEP, path, NULL);
		return;
	}

	if (self->before_directory != NULL)
		self->before_directory(path, self->hook_data);

	fd = open_directory(dirfd(top->dir), name);
	if (fd < 0) {
		skip_errno(self, path, errno);
		return;
	}

	/* The directory opened must be the one listed: not something renamed
	 * or mounted into its place in between. */
	if (statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_INO | STATX_MNT_ID,
	          &opened) != 0) {
		gint err = errno;

		close(fd);
		skip_errno(self, path, err == ENOENT ? EIO : err);
		return;
	}
	if (!S_ISDIR(opened.stx_mode) || opened.stx_ino != listed->stx_ino ||
	    opened.stx_dev_major != listed->stx_dev_major || opened.stx_dev_minor != listed->stx_dev_minor ||
	    ((opened.stx_mask & STATX_MNT_ID) && (listed->stx_mask & STATX_MNT_ID) &&
	     opened.stx_mnt_id != listed->stx_mnt_id)) {
		close(fd);
		if (!same_mount(self, &opened))
			skip(self, AXIS_WALK_FINDING_OTHER_FS, path, "a mount appeared while it was walked");
		else
			skip(self, AXIS_WALK_FINDING_REPLACED, path, "replaced while it was walked");
		return;
	}
	if (!same_mount(self, &opened)) {
		close(fd);
		skip(self, AXIS_WALK_FINDING_OTHER_FS, path, "another filesystem or mount");
		return;
	}

	dir = fdopendir(fd);
	if (dir == NULL) {
		gint err = errno;

		close(fd);
		skip_errno(self, path, err);
		return;
	}

	self->local.dirs++;
	memset(&level, 0, sizeof(level));
	level.dir = dir;
	level.path_len = strlen(path);
	level.raw_len = raw_len;
	level.depth = depth;
	level.invalid = invalid;
	g_array_append_val(stack, level);
}

/* One entry of the directory at the top of @stack.  Leaves @path as it
 * found it, unless a level was pushed (then it is that level's path). */
static void
visit(
	AxisLibraryWalk *self,
	GArray          *stack,
	GString         *path,
	const gchar     *name
){
	guint top_index = stack->len - 1;
	Level *top = &g_array_index(stack, Level, top_index);
	gsize name_len = strlen(name);
	gsize raw_len = top->raw_len + (top->depth > 0 ? 1 : 0) + name_len;
	gsize saved = path->len;
	gint dfd = dirfd(top->dir);
	struct statx stx;
	gboolean invalid;
	AxisIgnoreReason reason;
	gint64 looked;

	if (path->len > 0)
		g_string_append_c(path, '/');
	invalid = !axis_library_name_encode(path, name, name_len) || top->invalid;

	if (statx(dfd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_SYNC_AS_STAT, STATX_WANTED,
	          &stx) != 0) {
		gint err = errno;

		if (err == ENOENT) {
			/* Gone since it was listed: perhaps a folder renamed into a
			 * part already walked.  Not looked at, so not taken for gone;
			 * the next scan sees where it went. */
			skip(self, AXIS_WALK_FINDING_VANISHED, path->str, NULL);
		} else if (err == EACCES || err == EPERM) {
			/* The directory can be listed but not searched: none of its
			 * entries can be looked at.  For the root itself, nothing
			 * can: never a complete walk. */
			g_string_truncate(path, saved);
			top->failed = TRUE;
			if (top->depth == 0) {
				count_finding(self, AXIS_WALK_FINDING_IO_ERROR);
				report(self, AXIS_WALK_FINDING_UNREADABLE, "", "listed but not searchable");
			} else {
				skip(self, AXIS_WALK_FINDING_UNREADABLE, path->str, "listed but not searchable");
			}
			return;
		} else {
			skip_errno(self, path->str, err);
		}
		g_string_truncate(path, saved);
		return;
	}
	looked = now_ns();

	if (raw_len > AXIS_LIBRARY_PATH_MAX) {
		skip(self, AXIS_WALK_FINDING_TOO_LONG, path->str, NULL);
		g_string_truncate(path, saved);
		return;
	}

	if (S_ISDIR(stx.stx_mode)) {
		reason = ignored(self, name, name_len, TRUE);
		if (reason != AXIS_IGNORE_NONE) {
			leave_out(self, reason, path->str);
		} else {
			gchar *child = g_strdup(path->str);

			descend(self, stack, top_index, name, child, raw_len, invalid, &stx);
			g_free(child);
			/* A pushed level keeps the path; otherwise it is restored. */
			if (stack->len == top_index + 1)
				g_string_truncate(path, saved);
			return;
		}
	} else if (S_ISREG(stx.stx_mode)) {
		reason = ignored(self, name, name_len, FALSE);
		if (reason != AXIS_IGNORE_NONE) {
			leave_out(self, reason, path->str);
		} else {
			AxisFileKind kind = axis_file_kind_from_name(name, name_len);

			self->local.files++;
			self->local.bytes += stx.stx_size;
			add_entry(self, path->str, invalid, kind, &stx, looked, NULL);
		}
	} else if (S_ISLNK(stx.stx_mode)) {
		reason = ignored(self, name, name_len, FALSE);
		if (reason != AXIS_IGNORE_NONE) {
			leave_out(self, reason, path->str);
		} else {
			self->local.symlinks++;
			add_entry(self, path->str, invalid, AXIS_FILE_KIND_SYMLINK, &stx, looked,
				read_target(dfd, name));
		}
	} else {
		self->local.special++;
	}

	g_string_truncate(path, saved);
}

static void
pop_level(
	GArray  *stack,
	GString *path
){
	Level *top = &g_array_index(stack, Level, stack->len - 1);

	closedir(top->dir);
	g_array_set_size(stack, stack->len - 1);
	if (stack->len > 0)
		g_string_truncate(path, g_array_index(stack, Level, stack->len - 1).path_len);
	else
		g_string_truncate(path, 0);
}

static void
lower_priority(void)
{
	pid_t tid = (pid_t)syscall(SYS_gettid);
	gint nice_now;

	/* Both are per thread on Linux; this thread is the walk's alone. */
	if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT) != 0)
		g_debug("library: idle I/O priority not available: %s", g_strerror(errno));
	errno = 0;
	nice_now = getpriority(PRIO_PROCESS, (id_t)tid);
	if (errno == 0 && setpriority(PRIO_PROCESS, (id_t)tid, MIN(nice_now + NICE_INCREMENT, 19)) != 0)
		g_debug("library: lower CPU priority not available: %s", g_strerror(errno));
}

static void
walk_run(AxisLibraryWalk *self)
{
	g_autoptr(GArray) stack = g_array_new(FALSE, TRUE, sizeof(Level));
	g_autoptr(GString) path = g_string_new(NULL);
	Level root;
	DIR *dir;

	lower_priority();

	dir = fdopendir(self->root_fd);
	if (dir == NULL) {
		gint err = errno;

		close(self->root_fd);
		self->root_fd = -1;
		/* Nothing could be looked at: never a complete walk. */
		count_finding(self, AXIS_WALK_FINDING_IO_ERROR);
		self->local_io_errors = TRUE;
		report(self, AXIS_WALK_FINDING_IO_ERROR, "", g_strerror(err));
		return;
	}
	self->root_fd = -1;

	memset(&root, 0, sizeof(root));
	root.dir = dir;
	g_array_append_val(stack, root);

	while (stack->len > 0) {
		Level *top = &g_array_index(stack, Level, stack->len - 1);
		struct dirent *entry;

		if (stopping(self))
			break;
		if (top->failed) {
			pop_level(stack, path);
			continue;
		}

		errno = 0;
		entry = readdir(top->dir);
		if (entry == NULL) {
			gint err = errno;

			if (err != 0) {
				/* Part of it may have been read; none of it counts. */
				if (top->depth == 0) {
					count_finding(self, AXIS_WALK_FINDING_IO_ERROR);
					self->local_io_errors = TRUE;
					report(self, AXIS_WALK_FINDING_IO_ERROR, "", g_strerror(err));
				} else {
					gboolean io_error;

					skip(self, classify_error(err, &io_error), path->str, g_strerror(err));
					if (io_error)
						self->local_io_errors = TRUE;
				}
			}
			pop_level(stack, path);
			continue;
		}
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		g_atomic_int_inc(&self->steps);
		if (top->depth == 0)
			self->local.root_entries++;
		visit(self, stack, path, entry->d_name);
	}

	while (stack->len > 0)
		pop_level(stack, path);

	if (!stopping(self) && self->after_last != NULL)
		self->after_last("", self->hook_data);
}

static gpointer
walk_thread(gpointer data)
{
	AxisLibraryWalk *self = data;
	gboolean stopped;

	walk_run(self);

	stopped = stopping(self);
	if (!stopped && !batch_is_empty(self->current))
		flush(self);

	g_mutex_lock(&self->lock);
	self->status = stopping(self) ? AXIS_WALK_STOPPED : AXIS_WALK_DONE;
	self->totals = self->local;
	self->io_errors = self->local_io_errors;
	self->thread_done = TRUE;
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);

	g_atomic_int_add(&live_walks, -1);
	axis_library_walk_unref(self);
	return NULL;
}

/* ── The consumer's side ────────────────────────────────────────── */

AxisLibraryWalk *
axis_library_walk_start(
	gint                   root_fd,
	const AxisWalkConfig  *config,
	GError               **error
){
	AxisLibraryWalk *self;
	struct statx stx;
	GThread *thread;
	GError *local = NULL;
	gint fd;

	g_return_val_if_fail(root_fd >= 0, NULL);
	g_return_val_if_fail(config != NULL, NULL);

	/* The same directory again, so that reading it leaves its access time
	 * alone where that is allowed. */
	fd = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC | O_NOATIME);
	if (fd >= 0) {
		close(root_fd);
		root_fd = fd;
	}

	memset(&stx, 0, sizeof(stx));
	if (statx(root_fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_INO | STATX_MNT_ID,
	          &stx) != 0 || !S_ISDIR(stx.stx_mode)) {
		gint err = stx.stx_mask == 0 ? errno : ENOTDIR;

		close(root_fd);
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "cannot look at the root: %s",
			g_strerror(err));
		return NULL;
	}

	self = g_new0(AxisLibraryWalk, 1);
	self->ref_count = 1;
	self->batch_entries = config->batch_entries > 0 ? config->batch_entries : DEFAULT_BATCH_ENTRIES;
	self->racy_ns = config->racy_window_ns >= 0 ? config->racy_window_ns : DEFAULT_RACY_NS;
	self->patterns = g_strdupv(config->patterns);
	self->before_directory = config->before_directory;
	self->after_last = config->after_last;
	self->hook_data = config->hook_data;
	self->root_dev = (guint64)makedev(stx.stx_dev_major, stx.stx_dev_minor);
	self->root_ino = stx.stx_ino;
	self->has_root_mnt = (stx.stx_mask & STATX_MNT_ID) != 0;
	self->root_mnt = self->has_root_mnt ? stx.stx_mnt_id : 0;
	self->root_fd = root_fd;
	self->status = AXIS_WALK_RUNNING;
	self->current = batch_new();
	g_mutex_init(&self->lock);
	g_cond_init(&self->changed);
	g_queue_init(&self->waiting);

	/* The thread's own reference. */
	g_atomic_int_inc(&self->ref_count);
	g_atomic_int_inc(&live_walks);
	thread = g_thread_try_new("axis-scan", walk_thread, self, &local);
	if (thread == NULL) {
		g_atomic_int_add(&live_walks, -1);
		g_atomic_int_add(&self->ref_count, -1);
		close(self->root_fd);
		self->root_fd = -1;
		self->status = AXIS_WALK_FAILED;
		self->thread_done = TRUE;
		axis_library_walk_unref(self);
		g_propagate_prefixed_error(error, local, "cannot start the scan's thread: ");
		return NULL;
	}
	g_thread_unref(thread);

	return self;
}

AxisWalkBatch *
axis_library_walk_next(
	AxisLibraryWalk *self,
	guint            timeout_ms,
	gboolean        *finished
){
	AxisWalkBatch *batch;
	gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

	g_mutex_lock(&self->lock);
	while (g_queue_is_empty(&self->waiting) && self->status == AXIS_WALK_RUNNING) {
		if (!g_cond_wait_until(&self->changed, &self->lock, deadline))
			break;
	}
	batch = g_queue_pop_head(&self->waiting);
	if (batch != NULL)
		g_cond_broadcast(&self->changed);
	*finished = batch == NULL && self->status != AXIS_WALK_RUNNING;
	g_mutex_unlock(&self->lock);

	return batch;
}

void
axis_library_walk_stop(AxisLibraryWalk *self)
{
	g_return_if_fail(self != NULL);

	g_atomic_int_set(&self->stop_flag, 1);
	g_mutex_lock(&self->lock);
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);
}

gboolean
axis_library_walk_wait(
	AxisLibraryWalk *self,
	guint            timeout_ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	gboolean done;

	g_mutex_lock(&self->lock);
	while (!self->thread_done) {
		if (!g_cond_wait_until(&self->changed, &self->lock, deadline))
			break;
	}
	done = self->thread_done;
	g_mutex_unlock(&self->lock);

	return done;
}

AxisWalkStatus
axis_library_walk_get_status(AxisLibraryWalk *self)
{
	AxisWalkStatus status;

	g_mutex_lock(&self->lock);
	status = self->status;
	g_mutex_unlock(&self->lock);

	return status;
}

void
axis_library_walk_get_totals(
	AxisLibraryWalk *self,
	AxisWalkTotals  *totals
){
	g_mutex_lock(&self->lock);
	*totals = self->totals;
	g_mutex_unlock(&self->lock);
}

gboolean
axis_library_walk_had_io_errors(AxisLibraryWalk *self)
{
	gboolean io_errors;

	g_mutex_lock(&self->lock);
	io_errors = self->io_errors;
	g_mutex_unlock(&self->lock);

	return io_errors;
}

guint64
axis_library_walk_get_steps(AxisLibraryWalk *self)
{
	return (guint64)(guint)g_atomic_int_get(&self->steps);
}

void
axis_library_walk_get_root_identity(
	AxisLibraryWalk *self,
	guint64         *dev,
	guint64         *ino
){
	*dev = self->root_dev;
	*ino = self->root_ino;
}

guint
axis_library_walk_get_live(void)
{
	return (guint)g_atomic_int_get(&live_walks);
}

void
axis_library_walk_unref(AxisLibraryWalk *self)
{
	AxisWalkBatch *batch;

	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	while ((batch = g_queue_pop_head(&self->waiting)) != NULL)
		axis_walk_batch_free(batch);
	axis_walk_batch_free(self->current);
	if (self->root_fd >= 0)
		close(self->root_fd);
	g_strfreev(self->patterns);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->changed);
	g_free(self);
}
