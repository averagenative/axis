/*
 * axis-volume-proof.c - proving which volume is mounted at a path
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "storage/axis-volume-proof.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "storage/axis-storage-path.h"

struct _AxisVolumeProof
{
	gchar                 *path;
	gint                   fd;
	AxisStorageFsIdentity  identity;
	gchar                  uuid[AXIS_VOLUME_UUID_LEN + 1];
	AxisVolumeProofStatus  failed;	/* a check's first failure, or OK */
};

static AxisVolumeProofStatus
open_failure(gint err)
{
	switch (err) {
	case ENOENT:
		return AXIS_VOLUME_PROOF_MISSING;
	case ELOOP:
	case ENOTDIR:
		return AXIS_VOLUME_PROOF_LINK;
	case EINVAL:
	case ENAMETOOLONG:
		return AXIS_VOLUME_PROOF_BAD_PATH;
	default:
		return AXIS_VOLUME_PROOF_UNREADABLE;
	}
}

static AxisVolumeProofStatus
marker_failure(AxisVolumeMarkerStatus status)
{
	switch (status) {
	case AXIS_VOLUME_MARKER_MISSING:
		return AXIS_VOLUME_PROOF_NO_MARKER;
	case AXIS_VOLUME_MARKER_INVALID:
		return AXIS_VOLUME_PROOF_BAD_MARKER;
	default:
		return AXIS_VOLUME_PROOF_UNREADABLE;
	}
}

AxisVolumeProof *
axis_volume_proof_make(
	const gchar           *path,
	AxisVolumeProofStatus *status
){
	g_autofree gchar *normalised = NULL;
	AxisVolumeProof *self;
	AxisVolumeMarker marker;
	AxisVolumeMarkerStatus found;
	gint err = 0;
	gint fd;

	*status = AXIS_VOLUME_PROOF_BAD_PATH;
	if (path == NULL || !axis_storage_path_normalize(path, TRUE, &normalised, NULL) ||
	    strcmp(normalised, path) != 0)
		return NULL;

	fd = axis_storage_fs_open_path(path, &err);
	if (fd < 0) {
		*status = open_failure(err);
		return NULL;
	}
	self = g_new0(AxisVolumeProof, 1);
	self->fd = fd;
	self->path = g_strdup(path);
	if (!axis_storage_fs_identify(fd, &self->identity, &err)) {
		axis_volume_proof_free(self);
		*status = AXIS_VOLUME_PROOF_UNREADABLE;
		return NULL;
	}
	memset(&marker, 0, sizeof(marker));
	found = axis_volume_marker_read(fd, &marker, &err);
	if (found != AXIS_VOLUME_MARKER_FOUND) {
		axis_volume_proof_free(self);
		*status = marker_failure(found);
		return NULL;
	}
	memcpy(self->uuid, marker.uuid, sizeof(self->uuid));
	self->failed = AXIS_VOLUME_PROOF_OK;
	*status = AXIS_VOLUME_PROOF_OK;
	return self;
}

AxisVolumeProofStatus
axis_volume_proof_check(AxisVolumeProof *self)
{
	AxisStorageFsIdentity now;
	AxisVolumeMarker marker;
	AxisVolumeMarkerStatus found;
	gint err = 0;
	gint fd;

	g_return_val_if_fail(self != NULL, AXIS_VOLUME_PROOF_UNREADABLE);

	if (self->failed != AXIS_VOLUME_PROOF_OK)
		return self->failed;

	/* The path still leads to the very directory held. */
	fd = axis_storage_fs_open_path(self->path, &err);
	if (fd < 0) {
		self->failed = err == ENOENT || err == ELOOP || err == ENOTDIR ? AXIS_VOLUME_PROOF_REPLACED :
			open_failure(err);
		return self->failed;
	}
	if (!axis_storage_fs_identify(fd, &now, &err)) {
		close(fd);
		self->failed = AXIS_VOLUME_PROOF_UNREADABLE;
		return self->failed;
	}
	close(fd);
	if (now.dev != self->identity.dev || now.ino != self->identity.ino ||
	    (now.has_mnt_id && self->identity.has_mnt_id && now.mnt_id != self->identity.mnt_id)) {
		self->failed = AXIS_VOLUME_PROOF_REPLACED;
		return self->failed;
	}

	/* And the marker read through it is still the one proved. */
	memset(&marker, 0, sizeof(marker));
	found = axis_volume_marker_read(self->fd, &marker, &err);
	if (found != AXIS_VOLUME_MARKER_FOUND) {
		self->failed = marker_failure(found);
		return self->failed;
	}
	if (strcmp(marker.uuid, self->uuid) != 0) {
		self->failed = AXIS_VOLUME_PROOF_OTHER_VOLUME;
		return self->failed;
	}
	return AXIS_VOLUME_PROOF_OK;
}

const gchar *
axis_volume_proof_get_uuid(AxisVolumeProof *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->uuid;
}

const gchar *
axis_volume_proof_get_path(AxisVolumeProof *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->path;
}

gint
axis_volume_proof_get_fd(AxisVolumeProof *self)
{
	g_return_val_if_fail(self != NULL, -1);

	return self->fd;
}

guint64
axis_volume_proof_get_dev(AxisVolumeProof *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->identity.dev;
}

AxisVolumeProof *
axis_volume_proof_dup(AxisVolumeProof *self)
{
	AxisVolumeProof *copy;
	gint fd;

	g_return_val_if_fail(self != NULL, NULL);

	fd = fcntl(self->fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return NULL;
	copy = g_new0(AxisVolumeProof, 1);
	*copy = *self;
	copy->fd = fd;
	copy->path = g_strdup(self->path);
	return copy;
}

void
axis_volume_proof_free(AxisVolumeProof *self)
{
	if (self == NULL)
		return;
	if (self->fd >= 0)
		close(self->fd);
	g_free(self->path);
	g_free(self);
}

const gchar *
axis_volume_proof_status_describe(AxisVolumeProofStatus status)
{
	switch (status) {
	case AXIS_VOLUME_PROOF_OK:
		return "the volume's marker is there";
	case AXIS_VOLUME_PROOF_BAD_PATH:
		return "the path is not an absolute path in its normalised form";
	case AXIS_VOLUME_PROOF_MISSING:
		return "there is no directory at the path";
	case AXIS_VOLUME_PROOF_LINK:
		return "a symbolic link or something that is not a directory is on the path";
	case AXIS_VOLUME_PROOF_NO_MARKER:
		return "the directory has no volume marker (is the disk mounted?)";
	case AXIS_VOLUME_PROOF_BAD_MARKER:
		return "the directory's volume marker is not valid";
	case AXIS_VOLUME_PROOF_UNREADABLE:
		return "the directory or its marker cannot be read";
	case AXIS_VOLUME_PROOF_REPLACED:
		return "another directory is at the path now";
	case AXIS_VOLUME_PROOF_OTHER_VOLUME:
		return "the directory holds another volume's marker now";
	default:
		return "the volume could not be proved";
	}
}
