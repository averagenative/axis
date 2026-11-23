-- 0064_import_assignment.sql - what a person said the files of a download
-- are, when they assigned them by hand (add-force-grab-and-assign, slice B)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An import that is blocked or partly imported can be assigned by hand
-- (src/import/axis-import-assign.c): an administrator says which video file
-- is the movie, or which episodes each video file holds.  The import's next
-- run takes those files as that, without judging their names, and leaves
-- every other file of the download alone.
--
--   import_assignment  one row for each file a person assigned; a new
--                      assignment of the import replaces its rows.
--     import_id, position  the file, as import_file lists it
--     path              its path in the download as the review showed it:
--                       the run takes the file only when it is still there
--     episode_ids       a series: its episodes (series_episode.id); empty
--                       for a movie's file.  Not a foreign key: the table
--                       serves every module
--     replaces          the library files the person ticked to be replaced
--     replaces_quality  each one's quality when it was ticked ("unknown" for
--                       none): a tick does not hold for a file that changed
--     runtime_ok        the person ticked "assign it all the same" for a
--                       runtime far from the expected one
--     by_user, created_at  who and when
--
-- import.assign_first: a manual import opened to be assigned by hand: its run
-- reads the files and then waits for the assignment instead of deciding by
-- the names.
--
-- import_action.action: "assign_files".
--
-- Safe on a live database: one new table, one column with a constant default,
-- and one check constraint replaced by a wider one.

CREATE TABLE import_assignment (
	import_id        bigint      NOT NULL REFERENCES import (id) ON DELETE CASCADE,
	position         integer     NOT NULL,
	path             text        NOT NULL,
	episode_ids      bigint[]    NOT NULL DEFAULT '{}',
	replaces         bigint[]    NOT NULL DEFAULT '{}',
	replaces_quality text[]      NOT NULL DEFAULT '{}',
	runtime_ok       boolean     NOT NULL DEFAULT false,
	by_user          text        NOT NULL,
	created_at       timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (import_id, position),
	CONSTRAINT import_assignment_form CHECK (position >= 0 AND char_length(path) <= 4096
		AND cardinality(episode_ids) <= 64 AND cardinality(replaces) <= 256
		AND cardinality(replaces) = cardinality(replaces_quality)
		AND char_length(by_user) BETWEEN 1 AND 200)
);

ALTER TABLE import ADD COLUMN assign_first boolean NOT NULL DEFAULT false;

ALTER TABLE import_action DROP CONSTRAINT import_action_kind;
ALTER TABLE import_action ADD CONSTRAINT import_action_kind CHECK (action IN ('import_anyway', 'choose_file', 'assign',
	'dismiss', 'retry', 'manual', 'assign_files'));
