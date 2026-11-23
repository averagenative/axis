/*
 * axis-test-library.h - test support for the scanner: a database, storage
 * and library, disks and roots under a scratch directory, files with any
 * name, and a digest of a root's index
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Disks are directories under the scratch directory (volumes added with
 * the "not a mount point" confirmation), unplugged by moving them away and
 * leaving an empty directory, as in test-storage.c.
 */

#ifndef AXIS_TEST_LIBRARY_H
#define AXIS_TEST_LIBRARY_H

#include <glib.h>

#include "axis-test-db.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "library/axis-library.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

typedef struct
{
	AxisTestDb   *db;
	AxisDbPool   *pool;
	AxisSettings *settings;
	AxisStorage  *storage;
	AxisLibrary  *library;
	gchar        *dir;		/* the scratch directory */
	gchar        *media;	/* where disks are */
	gchar        *stash;	/* where unplugged disks go */
} AxisTestLibrary;

/*
 * Everything a scan test needs, with the racy window at 0 (files written
 * by the test a moment ago are not taken as "just modified") and the
 * guard checked at every turn.  NULL when database tests are skipped.
 */
AxisTestLibrary *
axis_test_library_new(guint pool_size);

void
axis_test_library_free(AxisTestLibrary *t);

/* The default tuning above, with @tuning's non-zero fields on top. */
void
axis_test_library_tune(
	AxisTestLibrary         *t,
	const AxisLibraryTuning *tuning
);

/* A disk at media/<name>, holding a readme.  Returns its path. */
gchar *
axis_test_library_disk(
	AxisTestLibrary *t,
	const gchar     *name
);

gint64
axis_test_library_add_volume(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *name
);

/*
 * Adds @path as a further volume.  These tests' disks are directories of
 * one scratch filesystem, which Axis refuses as two volumes whether or not
 * the first is online (what is recorded of it proves the filesystem the
 * same).  The record of every volume already added is first made to say
 * nothing of its filesystem (no identifier, no device), as for disks last
 * seen elsewhere; a check that finds one online records them again.  Real
 * separate filesystems are in the tests that mount tmpfs.
 */
gint64
axis_test_library_add_volume_beside(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *name
);

/* Makes the directory @rel on the volume at @volume_path and adds it as a
 * root. */
gint64
axis_test_library_add_root(
	AxisTestLibrary *t,
	gint64           volume_id,
	const gchar     *volume_path,
	const gchar     *rel,
	AxisMediaType    type
);

/* Scans a root; the scan must run.  The result. */
AxisScanResult *
axis_test_library_scan(
	AxisTestLibrary *t,
	gint64           root_id
);

/* As above, asserting the outcome. */
AxisScanResult *
axis_test_library_scan_expect(
	AxisTestLibrary *t,
	gint64           root_id,
	AxisScanStatus   status,
	AxisScanReason   reason
);

/* The disk at @path goes away, leaving an empty directory; and comes back. */
void
axis_test_library_unplug(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *as
);

void
axis_test_library_plug(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *as
);

/* The first column of the first row. */
gchar *
axis_test_library_q(
	AxisTestLibrary *t,
	const gchar     *format,
	...
) G_GNUC_PRINTF(2, 3);

gint64
axis_test_library_n(
	AxisTestLibrary *t,
	const gchar     *format,
	...
) G_GNUC_PRINTF(2, 3);

/* Everything the index records about a root's files except when each was
 * last seen: equal digests mean no record's content changed. */
gchar *
axis_test_library_index(
	AxisTestLibrary *t,
	gint64           root_id
);

/* Writes @content to the file @name (any bytes but '/' and NUL) in @dir. */
void
axis_test_write_named(
	const gchar *dir,
	const gchar *name,
	const gchar *content
);

/* Sets a file's modification time (and leaves its access time). */
void
axis_test_set_mtime(
	const gchar *path,
	gint64       seconds,
	glong        nanoseconds
);

/* mkdir -p @dir, and writes @n empty files f0..f(n-1) in it. */
void
axis_test_make_files(
	const gchar *dir,
	guint        n
);

G_END_DECLS

#endif /* AXIS_TEST_LIBRARY_H */
