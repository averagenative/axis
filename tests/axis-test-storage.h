/*
 * axis-test-storage.h - test support for the storage code: scratch
 * directories, markers, tree digests, descriptor counts and waiting
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything a storage test touches lives in a directory made by
 * axis_test_storage_dir_new() under the system's temporary directory and
 * removed by axis_test_storage_dir_free(), which never follows a symbolic
 * link and never descends onto another filesystem.
 */

#ifndef AXIS_TEST_STORAGE_H
#define AXIS_TEST_STORAGE_H

#include <glib.h>

G_BEGIN_DECLS

/* A new, empty scratch directory ("axis-storage-XXXXXX"). */
gchar *
axis_test_storage_dir_new(void);

/* Removes a scratch directory and everything in it. */
void
axis_test_storage_dir_free(gchar *dir);

/* mkdir -p, failing the test on error. */
void
axis_test_storage_mkdir(const gchar *path);

/* Writes @content to @path, failing the test on error. */
void
axis_test_storage_write(
	const gchar *path,
	const gchar *content
);

/* Writes a valid marker for @uuid into the directory @dir. */
void
axis_test_storage_write_marker(
	const gchar *dir,
	const gchar *uuid
);

/* The bytes of @path (NULL when it cannot be read). */
gchar *
axis_test_storage_read(const gchar *path);

/*
 * A description of everything under @root (names, kinds, sizes, modes,
 * modification times, contents of regular files, link targets), without
 * following links: two equal digests mean nothing was changed.
 */
gchar *
axis_test_storage_digest(const gchar *root);

/* Open descriptors of this process. */
guint
axis_test_storage_count_fds(void);

/* Names in @dir that start with ".axis-volume.tmp" (leftover temporary
 * markers). */
guint
axis_test_storage_count_temporaries(const gchar *dir);

/* A gate a hook can block on until a test opens it. */
typedef struct _AxisTestGate AxisTestGate;

AxisTestGate *
axis_test_gate_new(void);

/* Blocks while the gate is closed; counts the callers. */
void
axis_test_gate_pass(AxisTestGate *gate);

void
axis_test_gate_open(AxisTestGate *gate);

/* How many callers have reached the gate so far. */
guint
axis_test_gate_get_arrived(AxisTestGate *gate);

void
axis_test_gate_free(AxisTestGate *gate);

G_END_DECLS

#endif /* AXIS_TEST_STORAGE_H */
