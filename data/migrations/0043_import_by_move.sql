-- 0043_import_by_move.sql - imports that move a download's files into the
-- library; downloads whose files disappeared before their import; what is
-- left in a client's folder for Axis
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An import used to hardlink a download's files into the library (copying
-- them across volumes) and leave them where the client put them.  With
-- import.take_files "move" (the default) it moves them instead, so that
-- whatever is still in a download folder afterwards was not imported:
--
--   move       renamed into the library on one volume (never replacing a
--              name; the file is under one name or the other at every
--              moment)
--   move_copy  copied to the library's volume, verified, fsynced, and the
--              download's file deleted afterwards, once the import is
--              recorded
--
-- beside hardlink and copy, which leave the download's file where it is.  A
-- torrent's files are moved only once its client has let go of it (the
-- torrent is removed from the client without its data first).
--
-- What became of each download file a move took, on its import_file row:
--
--   moved    renamed into the library: nothing of it is left at its place
--   pending  a verified copy is in the library; the download's file is still
--            to be deleted (after the import is recorded)
--   deleted  the download's file was deleted, being the very file copied
--   gone     the download's file was no longer at its place
--   kept     the download's file was left: it is not the file copied any more
--            (another file has its name), with the reason in note
--
-- And on the import, for the folder's removal after a move (the moved files
-- are not in the folder any more, so an imported file still in it cannot be
-- what tells it is the folder imported from): the inode of the source folder
-- when the walk found it.  seen_at: when a walk last found the download's
-- files, for the page of an import whose files then disappeared.

ALTER TABLE import DROP CONSTRAINT import_mode;
ALTER TABLE import ADD CONSTRAINT import_mode CHECK (mode IS NULL OR mode IN ('hardlink', 'copy', 'move', 'move_copy'));
ALTER TABLE import
	ADD COLUMN source_inode bigint,
	ADD COLUMN seen_at      timestamptz;

ALTER TABLE import_file DROP CONSTRAINT import_file_mode;
ALTER TABLE import_file ADD CONSTRAINT import_file_mode CHECK (mode IS NULL OR mode IN ('hardlink', 'copy', 'move',
	'move_copy'));
ALTER TABLE import_file
	ADD COLUMN source_state text,
	ADD CONSTRAINT import_file_source_state CHECK (source_state IS NULL OR source_state IN ('moved', 'pending',
		'deleted', 'gone', 'kept'));

CREATE INDEX import_file_source_due ON import_file (import_id) WHERE source_state = 'pending';

-- A completed download whose files were deleted or moved by something else
-- before they were imported (a cleanup job of the download folder, a person):
-- its import ends dismissed with the reason files_gone, and its grab closes so,
-- with no blocklist entry (the release was not at fault), and the title is
-- wanted again.

ALTER TABLE movie_grab DROP CONSTRAINT movie_grab_close_reason;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone'));

ALTER TABLE series_grab DROP CONSTRAINT series_grab_close_reason;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone'));

-- What the Leftovers page last found in each client's folder for Axis that no
-- download and no import accounts for (for the links on the Downloads page and
-- the dashboard; the page itself always looks again).

ALTER TABLE download_client
	ADD COLUMN leftover_count      integer,
	ADD COLUMN leftover_checked_at timestamptz,
	ADD CONSTRAINT download_client_leftover_count CHECK (leftover_count IS NULL OR leftover_count >= 0);

-- A move across volumes deletes the download's file only while it is the very
-- file copied.  Its inode and size alone do not tell that: a file deleted by
-- something else and replaced at the same place by one of the same size can
-- have the same inode number.  The times the file had when it was copied tell
-- it too: its modification time and its change time (which an untouched file
-- keeps; a new file, a write, a chmod or a new link change them), in
-- nanoseconds since the epoch.  A row without them keeps the download's file.

ALTER TABLE import_file
	ADD COLUMN source_mtime_ns bigint,
	ADD COLUMN source_ctime_ns bigint;

-- A torrent whose files are moved is first removed from its client without
-- its data, and the client is asked again until it says it no longer has it.
-- release_asked_at is when Axis first asked, recorded before the request: when
-- the run dies between the client's removal and the record of it, the next run
-- that finds the torrent gone knows that Axis had asked, and does not say that
-- something else removed it.

ALTER TABLE import
	ADD COLUMN release_asked_at timestamptz;

-- Which client and which torrent (its identifier there) Axis asked to let go,
-- recorded with release_asked_at: a later run can still ask that client about
-- that torrent when the download's own record is gone.

ALTER TABLE import
	ADD COLUMN release_client_id bigint,
	ADD COLUMN release_ref       text;

-- The inode of the library folder a file's temporary name was made in, when
-- it was made there: a run taken up again looks for the file only in that very
-- folder (one renamed away and made again of the same name is another folder).

ALTER TABLE import_file
	ADD COLUMN dest_dir_inode bigint;
