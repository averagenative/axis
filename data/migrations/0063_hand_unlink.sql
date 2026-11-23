-- 0063_hand_unlink.sql - a file a person unlinked by hand stays unlinked
-- (add-force-grab-and-assign, slice C1)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- On a title's links page an administrator may unlink a file: its link goes,
-- the file stays on disk and in the library index.  Identification would
-- link it again at its next run (its name still says the title), so the
-- decision is remembered for each file:
--
--   library_hand_unlink  one row for each file a person unlinked by hand.
--     file_id   the file (the row goes with the file)
--     module    whose title it was unlinked from ("tv"; "movies" with C2)
--     owner_id  that title's row in its module (series.id): not a foreign
--               key, as the table serves every module; the module removes
--               its rows when it removes the title
--     by_user   who, created_at when
--
-- The mark is the file's, whichever title it was unlinked from: any owner's
-- link functions skip a file listed here (src/modules/tv/axis-tv-identify.c),
-- and an administrator of any title the file could belong to can clear it.  A
-- person linking the file by hand, letting Axis manage it again, or an
-- import by Axis linking a file at that path, removes its row
-- (src/modules/tv/axis-tv-links.c, axis-tv-import.c).

CREATE TABLE library_hand_unlink (
	file_id    bigint      PRIMARY KEY REFERENCES library_file (id) ON DELETE CASCADE,
	module     text        NOT NULL,
	owner_id   bigint      NOT NULL,
	by_user    text,
	created_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT library_hand_unlink_form CHECK (module ~ '^[a-z][a-z0-9_]{0,31}$' AND owner_id > 0
		AND (by_user IS NULL OR char_length(by_user) BETWEEN 1 AND 200))
);

CREATE INDEX library_hand_unlink_owner ON library_hand_unlink (module, owner_id);
