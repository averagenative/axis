-- 0047_volume_without_marker.sql - a volume added from a disk mounted
-- read-only, recognised by its filesystem until its marker is written
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- identity: how the volume is recognised now.
--   marker      by the marker file at its top (every volume added before this
--               migration, and every volume added from a writable disk)
--   filesystem  it was added while its disk was mounted read-only and had no
--               marker, so nothing was written: it is recognised by what can be
--               read without writing, recorded when it was added (ident_*), until
--               Axis writes its marker (with the volume's uuid) the first time it
--               finds the disk mounted read-write; then 'marker', for good.
--
-- ident_magic: the filesystem's type (statfs f_type); ident_fsid: its
-- identifier (statfs f_fsid, 16 hexadecimal digits), one the filesystem reports
-- of its own and not a device number, unless ident_fs_uuid is known;
-- ident_fs_uuid: its UUID as the kernel gives it (FS_IOC_GETFSUUID), NULL when
-- the kernel gave none; ident_ino: the volume directory's inode number;
-- ident_bytes: the filesystem's size.  ident_magic and ident_ino hold the bits of
-- unsigned numbers.  All of them must be the same, and the directory not empty,
-- for the volume to be online; anything else is offline (other_filesystem),
-- never a guess, and the marker is never written there.  They are kept after the
-- marker is written, as a record of what the volume was first recognised by.
--
-- marker_created_at: when its marker was written; NULL while it has none.
-- marker_problem: why the last attempt to write the marker failed, while it has
-- none (shown on the volume's page).

ALTER TABLE volume
	ALTER COLUMN marker_created_at DROP NOT NULL,
	ADD COLUMN identity       text   NOT NULL DEFAULT 'marker',
	ADD COLUMN ident_magic    bigint,
	ADD COLUMN ident_fsid     text,
	ADD COLUMN ident_fs_uuid  text,
	ADD COLUMN ident_ino      bigint,
	ADD COLUMN ident_bytes    bigint,
	ADD COLUMN marker_problem text,
	ADD CONSTRAINT volume_identity_known CHECK (identity IN ('marker', 'filesystem')),
	ADD CONSTRAINT volume_identity_marker CHECK ((identity = 'marker') = (marker_created_at IS NOT NULL)),
	ADD CONSTRAINT volume_identity_filesystem CHECK (identity = 'marker' OR (ident_magic IS NOT NULL
		AND ident_fsid ~ '^[0-9a-f]{16}$' AND (ident_fs_uuid IS NULL OR ident_fs_uuid ~ '^[0-9a-f]{2,32}$')
		AND ident_ino IS NOT NULL AND ident_bytes >= 0)),
	ADD CONSTRAINT volume_marker_problem_sane CHECK (marker_problem IS NULL
		OR (identity = 'filesystem' AND char_length(marker_problem) BETWEEN 1 AND 300
			AND marker_problem !~ '[\x01-\x1f\x7f]')),
	DROP CONSTRAINT volume_reason_known,
	ADD CONSTRAINT volume_reason_known CHECK (state_reason IN ('none', 'unchecked', 'path_missing',
		'not_mounted', 'empty', 'timeout', 'io_error', 'no_marker', 'other_marker', 'bad_marker',
		'not_directory', 'other_filesystem'));
