/*
 * axis-library-walk.h - walking a root's directory tree, on a thread of its
 * own, without following anything
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The walk starts from a directory descriptor (the root's, opened through a
 * volume guard) and goes depth first, iteratively, holding one open
 * directory per level and nothing else per file, so its memory does not
 * depend on how many files there are.  Every name is looked at with
 * statx(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT) relative to its directory's
 * descriptor and every directory is opened with openat(O_NOFOLLOW |
 * O_DIRECTORY), never through a path:
 *
 *  - a symbolic link is reported as a link and never followed;
 *  - a directory on another filesystem, or another mount of this one (a
 *    bind mount), is skipped and reported, whether it was there from the
 *    start or appeared during the walk;
 *  - a directory is opened only if what was opened is what was looked at
 *    (same device, inode and mount), so one replaced by a symbolic link or
 *    another directory between the two is skipped and reported;
 *  - entries deeper than AXIS_LIBRARY_DEPTH_MAX, or whose path is longer
 *    than AXIS_LIBRARY_PATH_MAX bytes, are skipped and reported.
 *
 * Whatever could not be looked at (skipped, ignored, unreadable, gone or
 * replaced) is reported as "not examined" by its stored path, so that
 * nothing recorded under it is taken for absent.  Errors that may mean
 * the disk is failing (EIO and the like, as opposed to a permission or a
 * name that went away) also mark the walk as having had I/O errors: a
 * scan with those does not count as complete.
 *
 * The walk only reads: directories are opened read-only (O_NOATIME where
 * the owner allows it), files are never opened, links are read with
 * readlinkat().  It never touches the database; it hands what it found
 * over in batches, at most two waiting at a time (it blocks until the
 * consumer takes one).
 *
 * A directory read can block in the kernel on a hung disk; no deadline can
 * interrupt that.  So the walk runs on its own thread, which the consumer
 * can abandon: an abandoned walk stops at its next step (once the kernel
 * lets go), closes its descriptors and drops its last reference.
 */

#ifndef AXIS_LIBRARY_WALK_H
#define AXIS_LIBRARY_WALK_H

#include <glib.h>

#include "library/axis-library-kind.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_WALK_FINDING_OTHER_FS,
	AXIS_WALK_FINDING_TOO_DEEP,
	AXIS_WALK_FINDING_TOO_LONG,
	AXIS_WALK_FINDING_UNREADABLE,
	AXIS_WALK_FINDING_IO_ERROR,
	AXIS_WALK_FINDING_VANISHED,
	AXIS_WALK_FINDING_REPLACED,
	AXIS_WALK_FINDING_PARTIAL
} AxisWalkFindingKind;

#define AXIS_WALK_FINDING_KIND_COUNT 8

/* "other_filesystem", "too_deep", ... (as stored) */
const gchar *
axis_walk_finding_kind_to_string(AxisWalkFindingKind kind);

typedef struct
{
	AxisWalkFindingKind  kind;
	gchar               *path;	/* stored form */
	gchar               *detail;	/* nullable; plain ASCII from the walk itself */
} AxisWalkFinding;

/**
 * AxisWalkEntry:
 * @path: the stored form of the path relative to the root
 * @invalid: some segment of it is not valid UTF-8
 * @kind: from the name, or %AXIS_FILE_KIND_SYMLINK
 * @racy: the file was modified (or its inode changed) within the racy
 *   window before it was looked at, so a later look that finds nothing
 *   different proves nothing
 * @target: (nullable): a link's target in stored form
 *
 * A regular file or symbolic link, as statx() saw it.
 */
typedef struct
{
	gchar        *path;
	gboolean      invalid;
	AxisFileKind  kind;
	gint64        size;
	gint64        mtime_sec;
	gint32        mtime_nsec;
	gint64        ctime_sec;
	gint32        ctime_nsec;
	guint64       inode;
	guint64       nlink;
	gboolean      racy;
	gchar        *target;
} AxisWalkEntry;

/**
 * AxisWalkBatch:
 * @entries: (element-type AxisWalkEntry)
 * @unexamined: (element-type utf8): stored paths not looked at (with all
 *   that is under them)
 * @findings: (element-type AxisWalkFinding): what to report by path; the
 *   walk hands over at most AXIS_WALK_MAX_FINDINGS in all and counts the
 *   rest
 */
typedef struct
{
	GArray    *entries;
	GPtrArray *unexamined;
	GPtrArray *findings;
} AxisWalkBatch;

#define AXIS_WALK_MAX_FINDINGS         200
#define AXIS_WALK_MAX_PARTIAL_FINDINGS 50

void
axis_walk_batch_free(AxisWalkBatch *batch);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWalkBatch, axis_walk_batch_free)

/* What the walk met, so far or in all. */
typedef struct
{
	guint64 dirs;
	guint64 files;		/* regular files */
	guint64 bytes;
	guint64 symlinks;
	guint64 special;	/* fifos, sockets, devices: not indexed */
	guint64 ignored;
	guint64 partials;
	guint64 other_fs;
	guint64 too_deep;
	guint64 too_long;
	guint64 unreadable;
	guint64 io_errors;
	guint64 vanished;
	guint64 replaced;
	guint64 findings;	/* all findings, stored or not */
	guint64 kind_files[AXIS_FILE_KIND_COUNT];
	guint64 kind_bytes[AXIS_FILE_KIND_COUNT];
	guint64 root_entries;	/* entries in the root directory itself */
} AxisWalkTotals;

typedef enum
{
	AXIS_WALK_RUNNING,
	AXIS_WALK_DONE,		/* every directory that could be read was */
	AXIS_WALK_STOPPED,	/* axis_library_walk_stop() */
	AXIS_WALK_FAILED	/* the walk could not start (no thread, a bad root) */
} AxisWalkStatus;

/*
 * Test hooks, called on the walk's thread: before a subdirectory found in
 * the listing is opened (@path is its stored path), and after the last
 * directory, before the walk reports that it is done.
 */
typedef void (*AxisWalkHook)(
	const gchar *path,
	gpointer     data
);

typedef struct
{
	guint         batch_entries;	/* 0: 1000 */
	gint64        racy_window_ns;	/* < 0: 2 s */
	gchar       **patterns;		/* (nullable) the root's ignore patterns */
	AxisWalkHook  before_directory;
	AxisWalkHook  after_last;
	gpointer      hook_data;
} AxisWalkConfig;

typedef struct _AxisLibraryWalk AxisLibraryWalk;

/**
 * axis_library_walk_start:
 * @root_fd: the root's directory; the walk owns it from now on (closed on
 *   failure too)
 * @config: copied
 * @error: (nullable): return location for an error
 *
 * Starts the walk's thread at idle I/O priority and lowered CPU priority
 * (both per thread, on Linux).
 *
 * Returns: (transfer full) (nullable): the walk
 */
AxisLibraryWalk *
axis_library_walk_start(
	gint                   root_fd,
	const AxisWalkConfig  *config,
	GError               **error
);

/**
 * axis_library_walk_next:
 * @self: a walk
 * @timeout_ms: how long to wait for a batch
 * @finished: (out): %TRUE once the walk has ended and every batch has been
 *   taken
 *
 * Returns: (transfer full) (nullable): the next batch, or %NULL when none
 *   came within @timeout_ms or the walk is finished
 */
AxisWalkBatch *
axis_library_walk_next(
	AxisLibraryWalk *self,
	guint            timeout_ms,
	gboolean        *finished
);

/* Tells the walk to stop at its next step (and stop waiting to hand over a
 * batch). */
void
axis_library_walk_stop(AxisLibraryWalk *self);

/* Waits up to @timeout_ms for the walk's thread to end.  Returns: whether
 * it has */
gboolean
axis_library_walk_wait(
	AxisLibraryWalk *self,
	guint            timeout_ms
);

AxisWalkStatus
axis_library_walk_get_status(AxisLibraryWalk *self);

/* The totals as of the last batch handed over (all of them, once the walk
 * has ended). */
void
axis_library_walk_get_totals(
	AxisLibraryWalk *self,
	AxisWalkTotals  *totals
);

/* Whether an error that may mean a failing disk was met. */
gboolean
axis_library_walk_had_io_errors(AxisLibraryWalk *self);

/* A counter that moves whenever the walk does something: entries looked
 * at, directories opened. */
guint64
axis_library_walk_get_steps(AxisLibraryWalk *self);

/* The device, inode and mount of the root directory, as the walk found it
 * when it started. */
void
axis_library_walk_get_root_identity(
	AxisLibraryWalk *self,
	guint64         *dev,
	guint64         *ino
);

/* Walks whose thread has not ended yet (abandoned ones included). */
guint
axis_library_walk_get_live(void);

/* Drops the caller's reference; a thread still running keeps its own and
 * ends on its own. */
void
axis_library_walk_unref(AxisLibraryWalk *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisLibraryWalk, axis_library_walk_unref)

G_END_DECLS

#endif /* AXIS_LIBRARY_WALK_H */
