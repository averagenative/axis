-- 0078_delete_download.sql - "Delete this download" on an import that waits
-- for a person (add-replace-grab-and-delete-download, slice B)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An administrator can delete the download of an import that is blocked,
-- waiting or partly imported (src/import/axis-import-delete.c).  The import
-- is closed as dismissed with the reason "download_deleted" and its client
-- removal pending; the client is then asked to remove the download with its
-- data, and what it keeps is deleted by the removal of a download's leftover
-- (src/import/axis-import-folder.c), at once and, for what could not be
-- finished, by the hourly cleanup.
--
--   import_action.action     "delete_download": who asked, and when
--   movie_grab.close_reason,
--   series_grab.close_reason "deleted": the grab whose download a person
--                            deleted from its import's page
--
-- import.reason is not listed by a constraint (import_texts checks its form
-- only, ^[a-z_]{1,40}$), so "download_deleted" needs nothing here.
-- import.client_removal has every state the request uses already (pending,
-- done, failed, kept, gone: migration 0041).
--
-- Safe on a live database: three check constraints replaced by wider ones.
-- No column, no index, no row changed.

ALTER TABLE import_action DROP CONSTRAINT import_action_kind;
ALTER TABLE import_action ADD CONSTRAINT import_action_kind CHECK (action IN ('import_anyway', 'choose_file', 'assign',
	'dismiss', 'retry', 'manual', 'assign_files', 'delete_download'));

ALTER TABLE movie_grab DROP CONSTRAINT movie_grab_close_reason;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone', 'not_imported', 'deleted'));

ALTER TABLE series_grab DROP CONSTRAINT series_grab_close_reason;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone', 'not_imported', 'deleted'));
