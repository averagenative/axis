-- 0069_arr_series.sql - what a reading of Sonarr found: a series as an item of
-- the plan, with its episodes and its episode files (import-from-arr, slice S1)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A reading of Sonarr stores a series as a row of arr_plan_item, as a reading
-- of Radarr stores a movie, with what only a series has:
--
--   tvdb_id        Sonarr's TheTVDB identifier of the series, by which it is
--                  matched (a movie's is tmdb_id).
--   series_type    'standard', 'daily' or 'anime' (NULL: a movie, or not said).
--   monitor_new    whether Sonarr monitors seasons that come later (NULL: not
--                  said, as with Sonarr 3).
--   seasons        Sonarr's monitoring of each season, [{"n": 1, "m": true}]
--                  (season 0 is the specials).
--   n_episodes, n_episodes_monitored, n_episodes_with_file, n_files, n_odd
--                  what the series' episode list and episode-file list held:
--                  episodes, those Sonarr monitors, those it has a file for,
--                  its files, and entries that could not be read.  NULL: the
--                  two lists were not asked for (a series that is not matched
--                  is not asked about).
--
--   match          gains 'not_listed': TMDb knows the series, and TheTVDB's
--                  aired order, which a series from Sonarr follows, does not
--                  list it.
--
--   arr_plan_episode  one row for each episode Sonarr lists of a series whose
--                  lists were read: its place in TheTVDB's aired order (season,
--                  episode), TheTVDB's episode id, whether Sonarr monitors it,
--                  and Sonarr's number of the file that holds it (NULL: none).
--   arr_plan_file  one row for each episode file: Sonarr's number of it (one
--                  file's number on several episodes is a multi-episode file),
--                  its path as Sonarr sees it, its size, Sonarr's name of its
--                  quality and the release it came from.
--
-- Both belong to their item: deleting the item (a reading that starts over, a
-- reading that failed, a plan a day old, a connection forgotten) deletes them
-- (ON DELETE CASCADE), so a reading that stops half-way leaves no row behind.
-- Nothing of them is logged.
--
-- Safe on a live database: the new columns have no default, so no row of
-- arr_plan_item is rewritten; its CHECKs are added NOT VALID and validated
-- after, which reads the table once (it holds the plans of the last day: a few
-- thousand rows).  The migration runs in one transaction, so reads and writes
-- of arr_plan_item wait for its commit: a reading at work then waits a moment.
-- The two tables are new and empty.

ALTER TABLE arr_plan_item ADD COLUMN tvdb_id bigint;
ALTER TABLE arr_plan_item ADD COLUMN series_type text;
ALTER TABLE arr_plan_item ADD COLUMN monitor_new boolean;
ALTER TABLE arr_plan_item ADD COLUMN seasons jsonb;
ALTER TABLE arr_plan_item ADD COLUMN n_episodes integer;
ALTER TABLE arr_plan_item ADD COLUMN n_episodes_monitored integer;
ALTER TABLE arr_plan_item ADD COLUMN n_episodes_with_file integer;
ALTER TABLE arr_plan_item ADD COLUMN n_files integer;
ALTER TABLE arr_plan_item ADD COLUMN n_odd integer;

ALTER TABLE arr_plan_item DROP CONSTRAINT arr_plan_item_match;
ALTER TABLE arr_plan_item ADD CONSTRAINT arr_plan_item_match CHECK (match IN ('pending', 'matched', 'no_id', 'unknown',
	'not_looked_up', 'incomplete', 'not_listed')) NOT VALID;
ALTER TABLE arr_plan_item VALIDATE CONSTRAINT arr_plan_item_match;

ALTER TABLE arr_plan_item ADD CONSTRAINT arr_plan_item_series CHECK (
	(series_type IS NULL OR series_type IN ('standard', 'daily', 'anime'))
	AND (seasons IS NULL OR (jsonb_typeof(seasons) = 'array' AND octet_length(seasons::text) <= 65536))
	AND (n_episodes IS NULL OR n_episodes >= 0)
	AND (n_episodes_monitored IS NULL OR n_episodes_monitored >= 0)
	AND (n_episodes_with_file IS NULL OR n_episodes_with_file >= 0)
	AND (n_files IS NULL OR n_files >= 0)
	AND (n_odd IS NULL OR n_odd >= 0)) NOT VALID;
ALTER TABLE arr_plan_item VALIDATE CONSTRAINT arr_plan_item_series;

CREATE TABLE arr_plan_episode (
	plan_id     bigint  NOT NULL,
	seq         integer NOT NULL,
	ord         integer NOT NULL,
	arr_id      bigint,
	tvdb_id     bigint,
	season      integer NOT NULL,
	episode     integer NOT NULL,
	air_date    date,
	monitored   boolean NOT NULL,
	file_arr_id bigint,

	PRIMARY KEY (plan_id, seq, ord),
	CONSTRAINT arr_plan_episode_item FOREIGN KEY (plan_id, seq) REFERENCES arr_plan_item (plan_id, seq)
		ON DELETE CASCADE,
	CONSTRAINT arr_plan_episode_place CHECK (season BETWEEN 0 AND 10000 AND episode BETWEEN 0 AND 100000
		AND ord >= 0)
);

CREATE TABLE arr_plan_file (
	plan_id bigint  NOT NULL,
	seq     integer NOT NULL,
	ord     integer NOT NULL,
	arr_id  bigint  NOT NULL,
	path    text,
	size    bigint,
	quality text,
	scene   text,

	PRIMARY KEY (plan_id, seq, ord),
	CONSTRAINT arr_plan_file_item FOREIGN KEY (plan_id, seq) REFERENCES arr_plan_item (plan_id, seq)
		ON DELETE CASCADE,
	CONSTRAINT arr_plan_file_form CHECK (ord >= 0
		AND (path IS NULL OR octet_length(path) BETWEEN 1 AND 4096)
		AND (size IS NULL OR size >= 0)
		AND (quality IS NULL OR char_length(quality) BETWEEN 1 AND 40)
		AND (scene IS NULL OR char_length(scene) BETWEEN 1 AND 300))
);
