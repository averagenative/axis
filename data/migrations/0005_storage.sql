-- 0005_storage.sql - volumes and library roots
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A volume is one filesystem.  It is identified by the UUID in the marker
-- file (".axis-volume") at the top of its directory, never by its path:
-- the path is where the daemon looks for it, and may be changed by an
-- administrator when a disk is mounted somewhere else.  device, fsid and
-- fs_type are what the filesystem looked like the last time the marker
-- matched; they are advisory.
--
-- mount_root records that the directory was a mount point when the marker
-- last matched.  Once true, a check that finds the directory is no longer
-- a mount point reports the volume offline whatever is in the directory:
-- an unmounted disk leaves its mount point behind, sometimes with a stale
-- copy of its files.
--
-- state is the result of the last check:
--   online     the marker is present and matches
--   read_only  as online, but the filesystem is mounted read-only
--   offline    the directory is missing, empty, not mounted, unreadable or
--              did not answer in time: retry later
--   mismatch   something else is there (another marker, no marker but
--              files, a broken marker, a symbolic link): needs a person
--
-- Nothing removes a volume, a root or anything that refers to them because
-- a volume is not online.  Rows are only removed by an administrator, and
-- library_root.volume_id is ON DELETE RESTRICT so that removing a volume
-- has to name its roots explicitly.

CREATE TABLE volume (
	id                bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	uuid              uuid        NOT NULL,
	name              text        NOT NULL,
	path              text        NOT NULL,
	mount_root        boolean     NOT NULL,
	fs_type           text        NOT NULL DEFAULT '',
	fsid              text        NOT NULL DEFAULT '',
	device            text        NOT NULL DEFAULT '',
	state             text        NOT NULL,
	state_reason      text        NOT NULL DEFAULT 'none',
	state_changed_at  timestamptz NOT NULL DEFAULT now(),
	last_checked_at   timestamptz,
	last_online_at    timestamptz,
	total_bytes       bigint,
	free_bytes        bigint,
	available_bytes   bigint,
	space_checked_at  timestamptz,
	marker_created_at timestamptz NOT NULL,
	created_at        timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT volume_uuid_unique UNIQUE (uuid),
	CONSTRAINT volume_path_unique UNIQUE (path),
	CONSTRAINT volume_name_unique UNIQUE (name),
	-- Absolute, no empty segments, no trailing '/', no '.' or '..'.  No
	-- control characters in paths or names, so that they can be logged
	-- and shown as they are.
	CONSTRAINT volume_path_form CHECK (path ~ '^(/[^/]+)+$' AND path !~ '/\.\.?(/|$)'
		AND octet_length(path) <= 1024 AND path !~ '[\x01-\x1f\x7f\u0080-\u009f]'),
	CONSTRAINT volume_name_form CHECK (char_length(name) BETWEEN 1 AND 64
		AND name !~ '[\x01-\x1f\x7f\u0080-\u009f]'),
	CONSTRAINT volume_state_known CHECK (state IN ('online', 'read_only', 'offline', 'mismatch')),
	CONSTRAINT volume_reason_known CHECK (state_reason IN ('none', 'unchecked', 'path_missing',
		'not_mounted', 'empty', 'timeout', 'io_error', 'no_marker', 'other_marker', 'bad_marker',
		'not_directory')),
	CONSTRAINT volume_space_sane CHECK (total_bytes >= 0 AND free_bytes >= 0 AND available_bytes >= 0)
);

-- A library root: a directory on a volume, by its path relative to the
-- volume's top ('' for the top itself), with what kind of media it holds.
-- Roots on one volume never contain one another; the code adding a root
-- checks that while holding the volume's row lock.
CREATE TABLE library_root (
	id         bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	volume_id  bigint      NOT NULL REFERENCES volume (id) ON DELETE RESTRICT,
	rel_path   text        NOT NULL,
	media_type text        NOT NULL,
	name       text        NOT NULL,
	created_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT library_root_path_unique UNIQUE (volume_id, rel_path),
	-- Relative, no empty segments, no '.' or '..'.
	CONSTRAINT library_root_path_form CHECK ((rel_path = '' OR rel_path ~ '^[^/]+(/[^/]+)*$')
		AND rel_path !~ '(^|/)\.\.?(/|$)' AND octet_length(rel_path) <= 1024
		AND rel_path !~ '[\x01-\x1f\x7f\u0080-\u009f]'),
	CONSTRAINT library_root_media_type_known CHECK (media_type IN ('movies', 'tv', 'music',
		'audiobooks', 'books', 'downloads', 'other')),
	CONSTRAINT library_root_name_form CHECK (char_length(name) BETWEEN 1 AND 64
		AND name !~ '[\x01-\x1f\x7f\u0080-\u009f]')
);

CREATE INDEX library_root_volume_idx ON library_root (volume_id);
