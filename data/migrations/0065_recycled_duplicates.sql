-- 0065_recycled_duplicates.sql - what a duplicate removed by hand was a file
-- of, and who removed it (add-force-grab-and-assign, slice D)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A file removed on the Duplicates page is retired into the recycle folder of
-- its disk by the import's own step, with no import behind it
-- (import_recycle.import_id IS NULL).  An upgrade's old file is found again
-- on its import's page; a removed duplicate has no such page, so its row says
-- by itself what it was:
--
--   label    what it was a file of, as the Duplicates page named it
--            ("Hearth (1995)", "Harbour Lights (2024) S01E03").  Set only for
--            a duplicate a person removed; NULL for an import's file.
--   by_user  who removed it.
--
-- /duplicates/recycled lists the rows that have a label.  The cleanup job
-- finishes, or forgets, a row left 'moving' with no import (the daemon died,
-- or lost the database, between the move and its record).

ALTER TABLE import_recycle
	ADD COLUMN label   text,
	ADD COLUMN by_user text,
	ADD CONSTRAINT import_recycle_by_hand CHECK ((label IS NULL OR char_length(label) BETWEEN 1 AND 500)
		AND (by_user IS NULL OR char_length(by_user) BETWEEN 1 AND 200));

CREATE INDEX import_recycle_labelled ON import_recycle (retired_at) WHERE label IS NOT NULL;
