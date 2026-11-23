-- 0045_identify_series.sql - what the series already in the library are:
-- each series folder of a TV root matched to a series (by the units and
-- rules films use, 0025_library_identification.sql), what each of its
-- episode files says it holds, and a managed series' links to those files
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A TV root's scope is a folder at its top (a series' folder, with its
-- season folders and episodes below it) or a video at its top; the unit is
-- that folder or file, as for films.  Its identity says which series it is.
--
-- Each video of a series' unit is read by the release parser for what its
-- own name says it holds: an episode or several of one season (S01E02,
-- S01E02E03, S01E02-E04, 1x02), or an air date (a daily show).  That is
-- library_identity_episode.  A name that says none of that (no number, only
-- an absolute number, a whole season, episodes across seasons, a season
-- other than its season folder's) holds no episode: the file's
-- episode_code says why, and the series' pages show it.
--
-- Which episode of the series' data a file is, is read when it is shown,
-- never stored: library_episode_file (below) puts each file's numbers (or
-- its date) against meta_episode as the metadata has it now, so a season
-- fetched later, or an episode the provider adds, places a file without a
-- second look at the disk.  Nothing here is written by a page.
--
-- A managed series' series_file rows are made from its identities
-- (series_file.identity_id), as a film's movie_file rows are; an import's
-- link of a file wins over identification's.

ALTER TABLE library_identity_file
	-- NULL: its name holds episodes (library_identity_episode); else why not:
	-- no_episode, absolute_only, cross_season, season_pack, season_folder.
	ADD COLUMN episode_code text,
	-- What its name holds ("S01E02-E03", "dated 2024-03-15") or why not.
	ADD COLUMN episode_text text,
	-- The comparison key of the title its own name states, when that is not
	-- its series folder's (NULL otherwise): such a file is the series' only
	-- when the key is one of the series' names (meta_title.name_keys).
	ADD COLUMN title_key text,
	ADD COLUMN name_title text,
	ADD CONSTRAINT library_identity_file_episode CHECK (
		(episode_code IS NULL OR episode_code IN ('no_episode', 'absolute_only', 'cross_season', 'season_pack',
			'season_folder'))
		AND (episode_text IS NULL OR char_length(episode_text) <= 1000)
		AND (title_key IS NULL OR char_length(title_key) BETWEEN 1 AND 400)
		AND (name_title IS NULL OR char_length(name_title) BETWEEN 1 AND 200));

-- The episodes one file's name holds: a season and a number each, or the
-- date it aired.
CREATE TABLE library_identity_episode (
	file_id       bigint   NOT NULL REFERENCES library_identity_file (file_id) ON DELETE CASCADE,
	position      smallint NOT NULL,
	season_number integer,
	number        integer,
	air_date      date,

	PRIMARY KEY (file_id, position),
	CONSTRAINT library_identity_episode_form CHECK (position BETWEEN 0 AND 199
		AND ((season_number IS NOT NULL AND number IS NOT NULL AND air_date IS NULL)
			OR (season_number IS NULL AND number IS NULL AND air_date IS NOT NULL))
		AND (season_number IS NULL OR season_number BETWEEN 0 AND 10000)
		AND (number IS NULL OR number BETWEEN 0 AND 100000))
);

-- A daily show's episode by the day it aired.
CREATE INDEX meta_episode_air_date ON meta_episode (title_id, air_date) WHERE air_date IS NOT NULL;

-- How far a TV root's run got with the seasons its series need: the
-- seasons of its linked series whose episodes are not fetched yet.
ALTER TABLE library_identity_root
	ADD COLUMN seasons_pending integer NOT NULL DEFAULT 0,
	ADD CONSTRAINT library_identity_root_seasons CHECK (seasons_pending >= 0);

-- The episode files of the series on disk, placed as the metadata has the
-- series now: one row per file and episode of the series' data it holds.
-- A file is a linked unit's (in a root for TV) and present (a disk that is
-- offline freezes its rows as present); its name holds episodes; the title
-- its own name states, if any, is one of the series' names; and each of its
-- numbers is an episode of the series' data, or its date the day exactly
-- one episode aired.  A file that is none of that is not here: the pages
-- list it with the reason.
CREATE VIEW library_episode_file AS
	SELECT li.title_id, li.id AS identity_id, li.root_id, lif.file_id, me.season_number, me.number, false AS by_date
	FROM library_identity li
	JOIN library_root lr ON lr.id = li.root_id AND lr.media_type = 'tv'
	JOIN meta_title lt ON lt.id = li.title_id AND lt.kind = 'series'
	JOIN library_identity_file lif ON lif.identity_id = li.id AND lif.episode_code IS NULL
	JOIN library_file lf ON lf.id = lif.file_id AND lf.state = 'present'
	JOIN library_identity_episode lie ON lie.file_id = lif.file_id AND lie.air_date IS NULL
	JOIN meta_episode me ON me.title_id = li.title_id AND me.season_number = lie.season_number
		AND me.number = lie.number
	WHERE li.state = 'linked'
	  AND (lif.title_key IS NULL OR position('|' || lif.title_key || '|' IN '|' || coalesce(lt.name_keys, '') || '|') > 0)
	UNION ALL
	SELECT li.title_id, li.id, li.root_id, lif.file_id, me.season_number, me.number, true
	FROM library_identity li
	JOIN library_root lr ON lr.id = li.root_id AND lr.media_type = 'tv'
	JOIN meta_title lt ON lt.id = li.title_id AND lt.kind = 'series'
	JOIN library_identity_file lif ON lif.identity_id = li.id AND lif.episode_code IS NULL
	JOIN library_file lf ON lf.id = lif.file_id AND lf.state = 'present'
	JOIN library_identity_episode lie ON lie.file_id = lif.file_id AND lie.air_date IS NOT NULL
	JOIN meta_episode me ON me.title_id = li.title_id AND me.air_date = lie.air_date
	WHERE li.state = 'linked'
	  AND (lif.title_key IS NULL OR position('|' || lif.title_key || '|' IN '|' || coalesce(lt.name_keys, '') || '|') > 0)
	  AND NOT EXISTS (SELECT 1 FROM meta_episode m2 WHERE m2.title_id = me.title_id AND m2.air_date = me.air_date
		AND (m2.season_number <> me.season_number OR m2.number <> me.number));

-- A managed series' link to a file learns where it came from besides an
-- import: the identity it was made from, and the file's resolution when its
-- quality is an unknown source.  A row has one origin at most.
ALTER TABLE series_file
	ADD COLUMN identity_id bigint REFERENCES library_identity (id) ON DELETE CASCADE,
	ADD COLUMN resolution  text,
	ADD CONSTRAINT series_file_resolution CHECK (resolution IS NULL OR resolution IN ('480p', '576p', '720p', '1080p',
		'2160p')),
	ADD CONSTRAINT series_file_one_origin CHECK (identity_id IS NULL OR import_id IS NULL);

CREATE INDEX series_file_identity ON series_file (identity_id) WHERE identity_id IS NOT NULL;

ALTER TABLE series_event DROP CONSTRAINT series_event_kind;
ALTER TABLE series_event ADD CONSTRAINT series_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'monitored',
	'episodes', 'searched', 'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal',
	'completed', 'abandoned', 'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded', 'identified',
	'unidentified'));
