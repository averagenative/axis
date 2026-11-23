-- 0041_import_client_gone.sql - a download already gone from its client
-- when Axis asks for its removal after the import; what a download removed
-- with its data outside an import left
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- After an import Axis asks the client to remove the download (a Usenet job
-- always, a torrent unless import.torrents_after is "seed").  A client may no
-- longer have it: a person removed it, or a plugin of the client did (one that
-- removes finished torrents and leaves their files), or another program.
-- Deluge says so ("torrent_id <hash> not in session."), and qBittorrent's list
-- does (it answers a delete 200 either way).  That is not a removal that
-- failed and is retried every hour, as it was recorded before; it is
--
--   gone     the client said it does not have the download; it removed
--            nothing now and deleted none of its files
--
-- beside pending, done, failed and kept.  A torrent gone so has its leftover
-- files looked at by the import (src/import/axis-import-folder.c,
-- folder_removal), under the same rules as a Usenet job's folder.
--
-- No row is changed here: one recorded "failed" because its client no longer
-- had it is asked again by the hourly cleanup, which now hears "gone" and
-- records it so.

ALTER TABLE import DROP CONSTRAINT import_removal;
ALTER TABLE import ADD CONSTRAINT import_removal CHECK (client_removal IS NULL OR client_removal IN
	('pending', 'done', 'failed', 'kept', 'gone'));

-- A download removed from its client with its data outside an import (a
-- person giving up on it, a failure put on the blocklist) can leave its files
-- too: SABnzbd keeps a completed job's folder, and a torrent the client no
-- longer had kept everything.  No import looks after those, so the download
-- records what became of them, with the same states as import.folder_removal
-- (pending, waiting, done, gone, left), set by the downloads layer when it
-- removes one so and decided by the import module's hourly cleanup.
--
-- What stands in for an imported file there is the inode of what was at the
-- place the client reported when the download completed (the file or the
-- folder), recorded by tracking from now on.  A download that completed
-- before this change has none, and its leftover is never deleted (the page
-- says where it is).

ALTER TABLE download
	ADD COLUMN location_inode      bigint,
	ADD COLUMN folder_removal      text,
	ADD COLUMN folder_removal_text text,
	ADD CONSTRAINT download_folder_removal CHECK (folder_removal IS NULL OR folder_removal IN
		('pending', 'waiting', 'done', 'gone', 'left')),
	ADD CONSTRAINT download_folder_removal_text CHECK (folder_removal_text IS NULL
		OR char_length(folder_removal_text) <= 1000);

CREATE INDEX download_folder_due ON download (id) WHERE folder_removal IN ('pending', 'waiting');
