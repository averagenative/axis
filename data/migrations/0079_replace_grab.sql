-- 0079_replace_grab.sql - "Replace the download in flight with this one" on
-- the confirmation of a grab by hand or by force
-- (add-replace-grab-and-delete-download, slice A)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An administrator who picks a release on a manual search while one grab of
-- the same thing is still downloading can end that grab and send the release
-- chosen in its place (src/acquire/axis-acquire-replace.c).  The grab that is
-- ended is closed with the reason "replaced", its removal pending, through
-- the path every ended grab takes (axis_acquire_handle_failure()).
--
--   movie_grab.close_reason,
--   series_grab.close_reason "replaced": the grab a person replaced with
--                            another release from a manual search
--
--   movie_grab.removal,
--   series_grab.removal      "kept": the download of a grab that was replaced
--                            finished at its client while its removal waited,
--                            at a place Axis does not let a client delete (a
--                            torrent moved into a library folder).  The client
--                            is not asked again, the download stays there, and
--                            removal_text says why (the Downloads page shows
--                            the same words from download.folder_removal_text)
--
-- Safe on a live database: four check constraints replaced by wider ones.
-- No column, no index, no row changed.

ALTER TABLE movie_grab DROP CONSTRAINT movie_grab_close_reason;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone', 'not_imported', 'deleted', 'replaced'));

ALTER TABLE movie_grab DROP CONSTRAINT movie_grab_removal;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_removal CHECK (removal IS NULL OR removal IN
	('pending', 'done', 'failed', 'kept'));

ALTER TABLE series_grab DROP CONSTRAINT series_grab_removal;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_removal CHECK (removal IS NULL OR removal IN
	('pending', 'done', 'failed', 'kept'));

ALTER TABLE series_grab DROP CONSTRAINT series_grab_close_reason;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone', 'not_imported', 'deleted', 'replaced'));
