-- 0050_volume_path_swap.sql - two volumes change paths in one statement; a
-- volume's filesystem UUID as last seen
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- volume_path_unique: when two disks that carry markers are swapped between
-- their mount points, each volume finds the other's marker at its path, and
-- each must be pointed at the other's path.  Re-pointing one onto the
-- other's path, with the other proven at the first one's path, re-points
-- both in one UPDATE.  A unique constraint that is not deferrable is checked
-- at every row, which refuses that statement half-way; one that is
-- deferrable but initially immediate is checked at the end of each
-- statement, as the SQL standard has it.  Nothing defers it further: no
-- moment, inside the transaction or out, has two volumes at one path, and no
-- volume is ever at a made-up path.
--
-- fs_uuid: the filesystem's UUID as the kernel gave it (FS_IOC_GETFSUUID) the
-- last time the volume was found (by its marker, or by its filesystem
-- identity), as fsid and device are; NULL when the kernel gave none then.  It
-- is not what a volume with a marker is recognised by (its marker is): it is
-- evidence for whether another directory is on the volume's filesystem,
-- where the filesystem's identifier is only the number of its device (xfs,
-- f2fs).  Every check that finds the volume, and every re-pointing, records
-- it again, so a filesystem the volume no longer lives on stops counting.  A
-- volume without a marker starts with the UUID it was added with.

ALTER TABLE volume
	DROP CONSTRAINT volume_path_unique,
	ADD CONSTRAINT volume_path_unique UNIQUE (path) DEFERRABLE INITIALLY IMMEDIATE,
	ADD COLUMN fs_uuid text,
	ADD CONSTRAINT volume_fs_uuid_form CHECK (fs_uuid IS NULL OR fs_uuid ~ '^[0-9a-f]{2,32}$');

UPDATE volume SET fs_uuid = ident_fs_uuid WHERE identity = 'filesystem';
