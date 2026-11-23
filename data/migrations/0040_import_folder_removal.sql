-- 0040_import_folder_removal.sql - the leftover folder of a Usenet download
-- after its import
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- SABnzbd's history delete with del_files=1 deletes the files of a job that
-- failed, not of one that completed: after an import the job's entry is gone
-- from SABnzbd and its folder stays where it finished, with the film's file
-- (now one name of the library's file) and whatever else SABnzbd left beside
-- it.  Axis removes that folder itself (src/import/axis-import-folder.c),
-- once the import is recorded and the client has removed the job, and only
-- when every rule there allows it.  What happened is recorded here, beside
-- client_removal:
--
--   pending  to be looked at: once client_removal is done, by the import's
--            own job and then by the hourly cleanup (import.cleanup)
--   waiting  it may be removed but not now (storage read-only, the volume
--            offline or mounted read-only, the client not answering); looked
--            at again every hour
--   done     the folder was removed, with everything in it
--   gone     nothing was at its place any more
--   left     it is not a folder Axis may remove; the text says which rule
--
-- A torrent is not affected: what happens to it after its import is still
-- import.torrents_after, through its client.
--
-- The imports recorded before this change: every imported Usenet download
-- was removed from its client in the belief that its files went with it.
-- Each one is looked at once more (pending), under the same rules, so a
-- folder that stayed is removed and one that is gone is recorded as gone.

ALTER TABLE import
	ADD COLUMN folder_removal      text,
	ADD COLUMN folder_removal_text text,
	ADD CONSTRAINT import_folder_removal CHECK (folder_removal IS NULL OR folder_removal IN
		('pending', 'waiting', 'done', 'gone', 'left')),
	ADD CONSTRAINT import_folder_removal_text CHECK (folder_removal_text IS NULL
		OR char_length(folder_removal_text) <= 1000);

UPDATE import i SET folder_removal = 'pending'
	WHERE i.state = 'imported' AND i.download_id IS NOT NULL AND i.client_removal IS NOT NULL
	AND EXISTS (SELECT 1 FROM download d WHERE d.id = i.download_id AND d.protocol = 'usenet');

CREATE INDEX import_folder_due ON import (id) WHERE folder_removal IN ('pending', 'waiting');
