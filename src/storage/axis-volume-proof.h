/*
 * axis-volume-proof.h - proving which volume is mounted at a path, without
 * the database: what axis-worker does before it reports a volume and
 * before and during a job on it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A proof opens a configured directory from "/" one component at a time
 * without following a symbolic link (axis_storage_fs_open_path()), keeps
 * that descriptor, and reads the volume's marker through it
 * (axis_volume_marker_read()).  The UUID it read is what the worker
 * reports; the daemon accepts the mapping only if it is that volume's.
 * Checking a proof again opens the path anew and requires the very same
 * directory (device, inode and mount) as the one held, then reads the
 * marker again through the held descriptor and requires the same UUID: a
 * disk unmounted (an empty directory left), swapped for another, or the
 * directory replaced by a link or renamed away all fail the check.  Files
 * of the volume are then opened beneath the held descriptor
 * (axis-storage-open.h), never through the path.
 *
 * Every function blocks on the filesystem: a caller that must not hang on
 * a dead mount runs them on a helper with a deadline
 * (axis-storage-probe.h).  Nothing here writes.
 */

#ifndef AXIS_VOLUME_PROOF_H
#define AXIS_VOLUME_PROOF_H

#include <glib.h>

#include "storage/axis-storage-fs.h"
#include "storage/axis-volume-marker.h"

G_BEGIN_DECLS

/**
 * AxisVolumeProofStatus:
 * @AXIS_VOLUME_PROOF_OK: the directory holds a valid marker (and, for a
 *   check, is still the same directory with the same marker)
 * @AXIS_VOLUME_PROOF_BAD_PATH: the configured path is not an absolute,
 *   normalised path
 * @AXIS_VOLUME_PROOF_MISSING: there is no directory at the path
 * @AXIS_VOLUME_PROOF_LINK: a symbolic link, or something that is not a
 *   directory, is on the way
 * @AXIS_VOLUME_PROOF_NO_MARKER: the directory has no marker (an empty
 *   directory where a disk should be mounted)
 * @AXIS_VOLUME_PROOF_BAD_MARKER: something of the marker's name that is not
 *   a valid marker
 * @AXIS_VOLUME_PROOF_UNREADABLE: the directory or the marker cannot be read
 * @AXIS_VOLUME_PROOF_REPLACED: a check found another directory at the path
 *   than the one held
 * @AXIS_VOLUME_PROOF_OTHER_VOLUME: a check found another volume's marker
 */
typedef enum
{
	AXIS_VOLUME_PROOF_OK,
	AXIS_VOLUME_PROOF_BAD_PATH,
	AXIS_VOLUME_PROOF_MISSING,
	AXIS_VOLUME_PROOF_LINK,
	AXIS_VOLUME_PROOF_NO_MARKER,
	AXIS_VOLUME_PROOF_BAD_MARKER,
	AXIS_VOLUME_PROOF_UNREADABLE,
	AXIS_VOLUME_PROOF_REPLACED,
	AXIS_VOLUME_PROOF_OTHER_VOLUME
} AxisVolumeProofStatus;

typedef struct _AxisVolumeProof AxisVolumeProof;

/**
 * axis_volume_proof_make:
 * @path: the directory where a volume is mounted on this machine
 * @status: (out): what was found
 *
 * Returns: (transfer full) (nullable): the proof, or %NULL when @status is
 *   not %AXIS_VOLUME_PROOF_OK
 */
AxisVolumeProof *
axis_volume_proof_make(
	const gchar           *path,
	AxisVolumeProofStatus *status
);

/* Checks @self again (see above).  A proof that failed a check stays
 * failed: every later check returns the same status. */
AxisVolumeProofStatus
axis_volume_proof_check(AxisVolumeProof *self);

/* Returns: (transfer none): the UUID read from the marker */
const gchar *
axis_volume_proof_get_uuid(AxisVolumeProof *self);

/* Returns: (transfer none): the path the proof was made at */
const gchar *
axis_volume_proof_get_path(AxisVolumeProof *self);

/* Returns: the directory's descriptor, owned by the proof */
gint
axis_volume_proof_get_fd(AxisVolumeProof *self);

/* The device the directory is on. */
guint64
axis_volume_proof_get_dev(AxisVolumeProof *self);

/* A copy of @self with a descriptor of its own (dup()) on the same
 * directory, for a check made on another thread; %NULL when no descriptor
 * is to be had. */
AxisVolumeProof *
axis_volume_proof_dup(AxisVolumeProof *self);

void
axis_volume_proof_free(AxisVolumeProof *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVolumeProof, axis_volume_proof_free)

/* A sentence for a status, which does not repeat the path. */
const gchar *
axis_volume_proof_status_describe(AxisVolumeProofStatus status);

G_END_DECLS

#endif /* AXIS_VOLUME_PROOF_H */
