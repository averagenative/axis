-- 0075_series_kind.sql - a series' kind (standard or anime), and Sonarr's
-- absolute episode numbers kept for one comparison (add-anime-support, A1)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
--   series.kind       'standard' or 'anime'.  Every series that exists is
--                     standard.  Anime is set only by an administrator (the
--                     Add form, the series' page) or by a migration from a
--                     Sonarr that has the series as anime; Axis never sets it
--                     by itself.  In slice A1 the kind changes nothing but
--                     what the series' page shows.
--   series.brought_absolute  NULL, or what a migration found when it compared
--                     Sonarr's absolute number of each episode with
--                     TheTVDB's, for a series Sonarr has as anime:
--                     {"agree": N, "differ": N, "one_side": N,
--                     "list": [[season, episode, Sonarr's, TheTVDB's], ...]}
--                     (the first 50 that differ).  A report only: nothing is
--                     linked, monitored or held by it.  Written once, by the
--                     run that made the series.
--   arr_plan_episode.absolute_number  Sonarr's "absoluteEpisodeNumber" of the
--                     episode, NULL for none.  Read for that comparison and
--                     for nothing else: Axis never takes an absolute number
--                     from Sonarr.
--
-- Safe on a live database with series in it: series.kind has a constant
-- default, so no row is rewritten, and the two other columns have none.  Each
-- CHECK is added NOT VALID and validated after, which reads its table once.
-- The migration runs in one transaction, so the locks are held to its commit;
-- series is small, and arr_plan_episode holds the plans of the last day.  A
-- plan read before this migration has no absolute numbers and its anime series
-- were not looked up: reading the connection again gives both.

ALTER TABLE series ADD COLUMN kind text NOT NULL DEFAULT 'standard';
ALTER TABLE series ADD CONSTRAINT series_kind CHECK (kind IN ('standard', 'anime')) NOT VALID;
ALTER TABLE series VALIDATE CONSTRAINT series_kind;

ALTER TABLE series ADD COLUMN brought_absolute jsonb;
ALTER TABLE series ADD CONSTRAINT series_brought_absolute CHECK (brought_absolute IS NULL
	OR (jsonb_typeof(brought_absolute) = 'object' AND octet_length(brought_absolute::text) <= 3072)) NOT VALID;
ALTER TABLE series VALIDATE CONSTRAINT series_brought_absolute;

ALTER TABLE arr_plan_episode ADD COLUMN absolute_number integer;
ALTER TABLE arr_plan_episode ADD CONSTRAINT arr_plan_episode_absolute CHECK (absolute_number IS NULL
	OR absolute_number BETWEEN 1 AND 1000000) NOT VALID;
ALTER TABLE arr_plan_episode VALIDATE CONSTRAINT arr_plan_episode_absolute;
