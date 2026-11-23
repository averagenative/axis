-- 0070_arr_series_brought.sql - a series brought over from Sonarr: the hold
-- on its searches, the hold on an episode whose file Axis cannot see, how its
-- monitoring was chosen, and what a connection brought of it
-- (import-from-arr, slice S2)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A run (the job arr.apply) adds the series a preview showed through the TV
-- module's own Add, under TheTVDB's aired order, copies Sonarr's monitoring of
-- each season and episode, links the episode files Axis's index holds at their
-- mapped paths, and holds every series it brings from automatic searches until
-- a person starts them.
--
--   series.search_hold  NULL, or why no automatic search runs for the series:
--                     'brought' (brought over, searching not started yet),
--                     'arr_downloading' (the app was downloading for it when
--                     searching was started).  "Start searching", "Let Axis
--                     search for it" and a person's Search now for the whole
--                     series end it; a person's search of one episode or one
--                     season passes it and leaves it.
--   series_episode.search_hold  NULL, or 'file_unseen': the app has a file for
--                     the episode that Axis did not link (not in its index at
--                     the mapped path, another size, linked elsewhere, unlinked
--                     by hand, or on an episode Axis's numbering does not
--                     have).  It outlasts "Start searching": Axis would count
--                     the episode missing and fetch a second copy.  A person's
--                     Search now of the episode, the tick of "Let Axis search
--                     for it", or a file on the episode (a later run, or any
--                     other link) ends it.
--   series.monitor_choice  gains 'brought': each season's and each episode's
--                     monitoring was copied from the app, not chosen at Add.
--
--   arr_brought.series_id  the series a connection made (NULL for a movie's
--                     row, and once that series was removed in Axis: a later
--                     run does not bring it again).
--   arr_brought.counts  for a series, what became of the app's episode files
--                     and episodes, as numbers: {"files", "linked", "already",
--                     "elsewhere", "unlinked", "not_indexed", "other_size",
--                     "unmappable", "no_path", "unmatched_files",
--                     "unmatched_episodes", "held_episodes"}.
--   arr_brought.file  gains 'partly': some of a series' files were linked and
--                     some were not.
--
-- Safe on a live database: the new columns have no default (no row is
-- rewritten), and each CHECK is added NOT VALID and validated after, so each
-- table is read once to check it.  The migration runs in one transaction, so
-- the table locks of these statements are held to its commit: reads and writes
-- of series, series_episode and arr_brought wait for it.  series_episode is
-- the largest (tens of thousands of rows in a large library): its validation
-- is one sequential read, a moment's wait.  Existing rows keep NULL holds: no
-- series that is in Axis now becomes held.

ALTER TABLE series ADD COLUMN search_hold text;
ALTER TABLE series ADD CONSTRAINT series_search_hold CHECK (search_hold IS NULL OR search_hold IN ('brought',
	'arr_downloading')) NOT VALID;
ALTER TABLE series VALIDATE CONSTRAINT series_search_hold;

ALTER TABLE series_episode ADD COLUMN search_hold text;
ALTER TABLE series_episode ADD CONSTRAINT series_episode_search_hold CHECK (search_hold IS NULL
	OR search_hold IN ('file_unseen')) NOT VALID;
ALTER TABLE series_episode VALIDATE CONSTRAINT series_episode_search_hold;

-- The held are few beside the whole tables once searching has started.
CREATE INDEX series_search_hold_idx ON series (search_hold) WHERE search_hold IS NOT NULL;
CREATE INDEX series_episode_search_hold_idx ON series_episode (series_id) WHERE search_hold IS NOT NULL;

ALTER TABLE series DROP CONSTRAINT series_monitor_choice;
ALTER TABLE series ADD CONSTRAINT series_monitor_choice CHECK (monitor_choice IN ('all', 'future', 'latest', 'first',
	'none', 'brought')) NOT VALID;
ALTER TABLE series VALIDATE CONSTRAINT series_monitor_choice;

ALTER TABLE arr_brought ADD COLUMN series_id bigint REFERENCES series (id) ON DELETE SET NULL;
ALTER TABLE arr_brought ADD COLUMN counts jsonb;
CREATE INDEX arr_brought_series ON arr_brought (series_id) WHERE series_id IS NOT NULL;

ALTER TABLE arr_brought DROP CONSTRAINT arr_brought_file;
ALTER TABLE arr_brought ADD CONSTRAINT arr_brought_file CHECK (file IN ('none', 'linked', 'already', 'not_indexed',
	'other_size', 'elsewhere', 'unmappable', 'no_path', 'unlinked', 'partly')) NOT VALID;
ALTER TABLE arr_brought VALIDATE CONSTRAINT arr_brought_file;
ALTER TABLE arr_brought ADD CONSTRAINT arr_brought_counts CHECK (counts IS NULL OR (jsonb_typeof(counts) = 'object'
	AND octet_length(counts::text) <= 4096)) NOT VALID;
ALTER TABLE arr_brought VALIDATE CONSTRAINT arr_brought_counts;

-- A place Sonarr lists whose entry could not be read (its monitoring is no
-- yes or no): kept as a place only, so that the run leaves the episode there
-- unmonitored and held.  arr_plan_episode holds the plans of the last day; the
-- default is constant, so no row is rewritten.
ALTER TABLE arr_plan_episode ADD COLUMN odd boolean NOT NULL DEFAULT false;
