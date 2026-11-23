-- 0028_series.sql - the series Axis manages: each series, its seasons and
-- episodes (following the metadata store), its searches and the releases
-- each considered, its grabs with the episodes each is for, its blocklist,
-- its history, and the links from library files to the episodes they hold;
-- and what the import pipeline needs to take a download in file by file
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An episode's state (wanted, downloading, in library, ...) is not stored:
-- it is derived from these facts, the downloads, the search jobs and the
-- files, by one query and one C function (src/modules/tv/axis-tv-store.c,
-- axis-tv-state.c).
--
-- An episode is known by the metadata episode it is (meta_episode's
-- provider_id, 0027_meta_episode_id.sql), not by its number: when the
-- provider numbers it anew, its row here keeps its id, and with it its
-- grabs, its files and its history, and takes the new number.  An episode a
-- provider gives no id for is known by its place (season, number) instead,
-- and the series' page says so.  An episode that is no longer in the
-- metadata is kept, marked gone, with what refers to it.
--
-- Grabbing once, per episode: a grab is written as a claim, in one
-- transaction with the series' row locked and the episodes it is for locked
-- and read; series_grab_episode holds the same rule in the database: an
-- episode is in at most one open grab (the partial unique index
-- series_grab_episode_one_open), so two releases that overlap never both go
-- out.  A grab's rows stop being open when the grab is closed (the trigger
-- below).
--
-- The search, candidate, grab and blocklist tables have the columns of
-- their films counterparts (0023_movies.sql), which the shared acquisition
-- layer (src/acquire/) reads and writes, and a few of their own.

CREATE TABLE series (
	id                 bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	title_id           bigint      NOT NULL REFERENCES meta_title (id) ON DELETE RESTRICT,
	quality_profile_id bigint      NOT NULL REFERENCES quality_profile (id) ON DELETE RESTRICT,
	root_id            bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	-- What was chosen to monitor when it was added, and whether a season the
	-- metadata announces later is monitored.
	monitor_choice     text        NOT NULL DEFAULT 'all',
	monitor_new        boolean     NOT NULL DEFAULT true,
	season_folders     boolean     NOT NULL DEFAULT true,
	added_by           text,
	added_at           timestamptz NOT NULL DEFAULT now(),
	updated_at         timestamptz NOT NULL DEFAULT now(),
	-- When its seasons and episodes last followed the metadata store.
	synced_at          timestamptz,

	CONSTRAINT series_title_unique UNIQUE (title_id),
	CONSTRAINT series_monitor_choice CHECK (monitor_choice IN ('all', 'future', 'latest', 'first', 'none')),
	CONSTRAINT series_added_by CHECK (added_by IS NULL OR char_length(added_by) BETWEEN 1 AND 200),
	CONSTRAINT series_season_folders CHECK (season_folders)
);

CREATE INDEX series_quality_profile ON series (quality_profile_id);
CREATE INDEX series_root ON series (root_id);

CREATE TABLE series_season (
	id        bigint  GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	series_id bigint  NOT NULL REFERENCES series (id) ON DELETE CASCADE,
	number    integer NOT NULL,
	monitored boolean NOT NULL,

	CONSTRAINT series_season_unique UNIQUE (series_id, number),
	CONSTRAINT series_season_number CHECK (number BETWEEN 0 AND 10000)
);

CREATE TABLE series_episode (
	id              bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	series_id       bigint      NOT NULL REFERENCES series (id) ON DELETE CASCADE,
	-- The metadata episode it is: the provider's own id (NULL: none given,
	-- then its place is its identity).
	provider_id     text,
	season_number   integer     NOT NULL,
	number          integer     NOT NULL,
	absolute_number integer,
	title           text,
	air_date        date,
	runtime_minutes integer,
	monitored       boolean     NOT NULL,
	-- No longer in the metadata (kept with its links and history).
	gone_at         timestamptz,
	added_at        timestamptz NOT NULL DEFAULT now(),
	-- The pacing of automatic searches, per episode (src/acquire/ paces it).
	last_search_at  timestamptz,
	next_search_at  timestamptz,
	search_floor_s  integer     NOT NULL DEFAULT 21600,

	CONSTRAINT series_episode_form CHECK (season_number BETWEEN 0 AND 10000 AND number BETWEEN 0 AND 100000
		AND (provider_id IS NULL OR provider_id ~ '^[A-Za-z0-9]{1,24}$')
		AND (absolute_number IS NULL OR absolute_number BETWEEN 1 AND 1000000)
		AND (title IS NULL OR char_length(title) <= 500)
		AND (runtime_minutes IS NULL OR runtime_minutes BETWEEN 1 AND 10000)),
	CONSTRAINT series_episode_floor CHECK (search_floor_s BETWEEN 60 AND 31536000)
);

-- One row per metadata episode: by its id, or (with none) by its place.
CREATE UNIQUE INDEX series_episode_provider ON series_episode (series_id, provider_id) WHERE provider_id IS NOT NULL;
CREATE UNIQUE INDEX series_episode_place ON series_episode (series_id, season_number, number)
	WHERE provider_id IS NULL AND gone_at IS NULL;
CREATE INDEX series_episode_number ON series_episode (series_id, season_number, number);
CREATE INDEX series_episode_air ON series_episode (air_date) WHERE gone_at IS NULL;

-- One search: of an episode, a season or the whole series.
CREATE TABLE series_search (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	series_id     bigint      NOT NULL REFERENCES series (id) ON DELETE CASCADE,
	cause         text        NOT NULL,
	requested_by  text,
	started_at    timestamptz NOT NULL DEFAULT now(),
	finished_at   timestamptz,
	outcome       text,
	outcome_text  text,
	how           text,
	n_sources     integer     NOT NULL DEFAULT 0,
	n_found       integer     NOT NULL DEFAULT 0,
	n_accepted    integer     NOT NULL DEFAULT 0,
	n_compared    integer     NOT NULL DEFAULT 0,
	-- What it was for, and the de-duplication key of its job.
	scope         text        NOT NULL DEFAULT 'episode',
	episode_id    bigint      REFERENCES series_episode (id) ON DELETE SET NULL,
	season_number integer,
	job_key       text,
	-- Releases numbered for episodes the series' data does not have.
	n_mismatched  integer     NOT NULL DEFAULT 0,

	CONSTRAINT series_search_cause CHECK (cause IN ('added', 'manual', 'scheduled', 'retry')),
	CONSTRAINT series_search_outcome CHECK (outcome IS NULL OR outcome IN ('grabbed', 'nothing_found',
		'none_acceptable', 'exhausted', 'no_indexer', 'indexer_error', 'no_client', 'client_error', 'root_offline',
		'profile_unusable', 'metadata_missing', 'in_flight', 'failed')),
	CONSTRAINT series_search_finished CHECK ((finished_at IS NULL) = (outcome IS NULL)),
	CONSTRAINT series_search_scope CHECK (scope IN ('episode', 'season', 'series')),
	CONSTRAINT series_search_texts CHECK ((requested_by IS NULL OR char_length(requested_by) BETWEEN 1 AND 200)
		AND (outcome_text IS NULL OR char_length(outcome_text) <= 2000)
		AND (how IS NULL OR char_length(how) <= 1000)
		AND (job_key IS NULL OR char_length(job_key) <= 100)),
	CONSTRAINT series_search_counts CHECK (n_sources >= 0 AND n_found >= 0 AND n_accepted >= 0 AND n_compared >= 0
		AND n_mismatched >= 0)
);

CREATE INDEX series_search_series ON series_search (series_id, id DESC);
CREATE INDEX series_search_episode ON series_search (episode_id, id DESC) WHERE episode_id IS NOT NULL;

CREATE TABLE series_candidate (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	search_id    bigint      NOT NULL REFERENCES series_search (id) ON DELETE CASCADE,
	position     integer     NOT NULL,
	release_id   bigint      NOT NULL,
	source_id    bigint      NOT NULL,
	name         text        NOT NULL,
	protocol     text        NOT NULL,
	size_bytes   bigint,
	published_at timestamptz,
	seeders      integer,
	indexer      text,
	indexer_id   bigint      NOT NULL DEFAULT 0,
	info_hash    text,
	name_key     text        NOT NULL,
	quality      text        NOT NULL,
	match        text        NOT NULL,
	match_text   text,
	accepted     boolean     NOT NULL,
	safety       boolean     NOT NULL,
	blocklisted  boolean     NOT NULL DEFAULT false,
	rank         integer     NOT NULL DEFAULT 0,
	score        integer     NOT NULL DEFAULT 0,
	reasons      jsonb       NOT NULL DEFAULT '[]',
	note         text,
	-- The episodes it holds (as the series' data has them), in words.
	episode_ids  bigint[]    NOT NULL DEFAULT '{}',
	holds        text,
	-- Numbered for an episode the series' data does not have.
	mismatched   boolean     NOT NULL DEFAULT false,

	CONSTRAINT series_candidate_position UNIQUE (search_id, position),
	CONSTRAINT series_candidate_form CHECK (
		char_length(name) BETWEEN 1 AND 300 AND name !~ '[\x01-\x1f\x7f]'
		AND protocol IN ('usenet', 'torrent')
		AND match IN ('match', 'none', 'ambiguous')
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND char_length(name_key) BETWEEN 1 AND 400
		AND (indexer IS NULL OR char_length(indexer) <= 100)
		AND (match_text IS NULL OR char_length(match_text) <= 1000)
		AND (note IS NULL OR char_length(note) <= 1000)
		AND (holds IS NULL OR char_length(holds) <= 200)
		AND cardinality(episode_ids) <= 2000
		AND jsonb_typeof(reasons) = 'array'
		AND rank >= 0 AND (size_bytes IS NULL OR size_bytes >= 0) AND (seeders IS NULL OR seeders >= 0))
);

CREATE INDEX series_candidate_search ON series_candidate (search_id, position);

CREATE TABLE series_grab (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	series_id     bigint      NOT NULL REFERENCES series (id) ON DELETE CASCADE,
	search_id     bigint      REFERENCES series_search (id) ON DELETE SET NULL,
	candidate_id  bigint      REFERENCES series_candidate (id) ON DELETE SET NULL,
	release_name  text        NOT NULL,
	protocol      text        NOT NULL,
	source_id     bigint,
	indexer       text,
	indexer_id    bigint      NOT NULL DEFAULT 0,
	size_bytes    bigint,
	quality       text,
	name_key      text        NOT NULL,
	info_hash     text,
	why           text,
	manual        boolean     NOT NULL DEFAULT false,
	grabbed_by    text,
	download_id   bigint      REFERENCES download (id) ON DELETE SET NULL,
	created_at    timestamptz NOT NULL DEFAULT now(),
	sent_at       timestamptz,
	completed_at  timestamptz,
	closed_at     timestamptz,
	close_reason  text,
	close_text    text,
	removal       text,
	removal_text  text,
	retry_at      timestamptz,
	-- What it was grabbed for (the search's scope), and its job's key.
	scope         text        NOT NULL DEFAULT 'episode',
	episode_id    bigint      REFERENCES series_episode (id) ON DELETE SET NULL,
	season_number integer,
	job_key       text,
	holds         text,

	CONSTRAINT series_grab_form CHECK (
		char_length(release_name) BETWEEN 1 AND 300
		AND protocol IN ('usenet', 'torrent')
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND char_length(name_key) BETWEEN 1 AND 400
		AND (grabbed_by IS NULL OR char_length(grabbed_by) BETWEEN 1 AND 200)
		AND (why IS NULL OR char_length(why) <= 2000)
		AND (close_text IS NULL OR char_length(close_text) <= 1000)
		AND (removal_text IS NULL OR char_length(removal_text) <= 1000)
		AND (holds IS NULL OR char_length(holds) <= 200)
		AND (job_key IS NULL OR char_length(job_key) <= 100)),
	CONSTRAINT series_grab_close CHECK ((closed_at IS NULL) = (close_reason IS NULL)),
	CONSTRAINT series_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN ('not_sent', 'failed',
		'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported', 'dismissed')),
	CONSTRAINT series_grab_removal CHECK (removal IS NULL OR removal IN ('pending', 'done', 'failed')),
	CONSTRAINT series_grab_scope CHECK (scope IN ('episode', 'season', 'series'))
);

CREATE INDEX series_grab_series ON series_grab (series_id, id DESC);
CREATE INDEX series_grab_download ON series_grab (download_id) WHERE download_id IS NOT NULL;
CREATE INDEX series_grab_open ON series_grab (series_id) WHERE closed_at IS NULL;

-- The episodes one grab is for.  open mirrors the grab's being open, and an
-- episode is in at most one open grab: the database's own guard against
-- grabbing an episode twice (the claim transaction is the first).
CREATE TABLE series_grab_episode (
	grab_id    bigint  NOT NULL REFERENCES series_grab (id) ON DELETE CASCADE,
	episode_id bigint  NOT NULL REFERENCES series_episode (id) ON DELETE CASCADE,
	open       boolean NOT NULL DEFAULT true,

	PRIMARY KEY (grab_id, episode_id)
);

CREATE UNIQUE INDEX series_grab_episode_one_open ON series_grab_episode (episode_id) WHERE open;
CREATE INDEX series_grab_episode_episode ON series_grab_episode (episode_id);

CREATE FUNCTION series_grab_closed() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	UPDATE series_grab_episode SET open = false WHERE grab_id = NEW.id AND open;
	RETURN NULL;
END $$;

CREATE TRIGGER series_grab_closes AFTER UPDATE OF closed_at ON series_grab
	FOR EACH ROW WHEN (NEW.closed_at IS NOT NULL AND OLD.closed_at IS NULL)
	EXECUTE FUNCTION series_grab_closed();

-- The facts query asks, for every series, whether a download of it is
-- still at a client.
CREATE INDEX download_tv_live ON download (title_id)
	WHERE module = 'tv' AND state IN ('sending', 'queued', 'downloading', 'paused');

CREATE TABLE series_blocklist (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	series_id    bigint      NOT NULL REFERENCES series (id) ON DELETE CASCADE,
	key          text        NOT NULL,
	name_key     text        NOT NULL,
	info_hash    text,
	release_name text        NOT NULL,
	indexer      text,
	reason       text        NOT NULL,
	reason_text  text,
	grab_id      bigint      REFERENCES series_grab (id) ON DELETE SET NULL,
	created_by   text,
	created_at   timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT series_blocklist_unique UNIQUE (series_id, key),
	CONSTRAINT series_blocklist_form CHECK (
		(key ~ '^hash:[0-9a-f]{40}$' OR (key LIKE 'name:%' AND char_length(key) <= 410))
		AND char_length(name_key) BETWEEN 1 AND 400
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND char_length(release_name) BETWEEN 1 AND 300
		AND reason IN ('failed', 'stalled', 'gone', 'not_taken', 'unfetchable', 'refused', 'given_up')
		AND (reason_text IS NULL OR char_length(reason_text) <= 1000)
		AND (created_by IS NULL OR char_length(created_by) BETWEEN 1 AND 200))
);

-- What happened to a series, kept when the series is removed (with its
-- name); an event about one episode names it.
CREATE TABLE series_event (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	series_id   bigint      REFERENCES series (id) ON DELETE SET NULL,
	title_id    bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	title_label text        NOT NULL,
	episode_id  bigint      REFERENCES series_episode (id) ON DELETE SET NULL,
	kind        text        NOT NULL,
	text        text        NOT NULL,
	by_user     text,
	search_id   bigint,
	grab_id     bigint,
	created_at  timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT series_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'monitored', 'episodes', 'searched',
		'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal', 'completed', 'abandoned',
		'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded')),
	CONSTRAINT series_event_texts CHECK (char_length(title_label) BETWEEN 1 AND 600
		AND char_length(text) BETWEEN 1 AND 4000
		AND (by_user IS NULL OR char_length(by_user) BETWEEN 1 AND 200))
);

CREATE INDEX series_event_series ON series_event (series_id, id DESC);

-- A file of the library that holds episodes of a series: one, or several
-- (a double episode), each linked.  Only the import pipeline links them.
CREATE TABLE series_file (
	file_id      bigint      PRIMARY KEY REFERENCES library_file (id) ON DELETE CASCADE,
	series_id    bigint      NOT NULL REFERENCES series (id) ON DELETE CASCADE,
	quality      text,
	release_name text,
	quality_text text,
	import_id    bigint      REFERENCES import (id) ON DELETE SET NULL,
	linked_at    timestamptz NOT NULL DEFAULT now(),
	linked_by    text,

	CONSTRAINT series_file_form CHECK ((quality IS NULL OR quality ~ '^[a-z0-9_]{1,32}$')
		AND (release_name IS NULL OR char_length(release_name) BETWEEN 1 AND 300)
		AND (quality_text IS NULL OR char_length(quality_text) <= 1000)
		AND (linked_by IS NULL OR char_length(linked_by) BETWEEN 1 AND 200))
);

CREATE INDEX series_file_series ON series_file (series_id);

CREATE TABLE series_file_episode (
	file_id    bigint NOT NULL REFERENCES series_file (file_id) ON DELETE CASCADE,
	episode_id bigint NOT NULL REFERENCES series_episode (id) ON DELETE CASCADE,

	PRIMARY KEY (file_id, episode_id)
);

CREATE INDEX series_file_episode_episode ON series_file_episode (episode_id);

-- A series follows its record through a merge of two records, as a film
-- does (0023_movies.sql).
CREATE FUNCTION series_follow_title_merge() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF OLD.title_id <> NEW.title_id
	   AND NOT EXISTS (SELECT 1 FROM meta_external_id WHERE title_id = OLD.title_id) THEN
		UPDATE series SET title_id = NEW.title_id, updated_at = now() WHERE title_id = OLD.title_id;
	END IF;
	RETURN NULL;
END $$;

CREATE TRIGGER series_follows_title_merge AFTER UPDATE OF title_id ON meta_external_id
	FOR EACH ROW EXECUTE FUNCTION series_follow_title_merge();

-- Imports file by file (a series' download): a file holds episodes, and an
-- import whose wanted files were not all placed ends partly imported (its
-- placed files stay, each recorded; "retry" takes it up again).
ALTER TABLE import DROP CONSTRAINT import_state;
ALTER TABLE import ADD CONSTRAINT import_state CHECK (state IN ('waiting', 'running', 'blocked', 'imported',
	'dismissed', 'partial'));
ALTER TABLE import DROP CONSTRAINT import_finished;
ALTER TABLE import ADD CONSTRAINT import_finished CHECK ((state IN ('imported', 'dismissed', 'partial')) =
	(finished_at IS NOT NULL));
ALTER TABLE import_file DROP CONSTRAINT import_file_role;
ALTER TABLE import_file ADD CONSTRAINT import_file_role CHECK (role IS NULL OR role IN ('film', 'subtitle', 'extra',
	'episode'));

-- A film has one open import at a time.  A series' downloads each hold
-- their own episodes: several may wait or be blocked at once (one blocked
-- download does not hold up the others), and one is placed at a time.
DROP INDEX import_open_owner;
CREATE UNIQUE INDEX import_open_owner ON import (module, owner_id)
	WHERE owner_id IS NOT NULL AND module <> 'tv' AND state IN ('waiting', 'running', 'blocked');
CREATE UNIQUE INDEX import_running_owner ON import (module, owner_id)
	WHERE owner_id IS NOT NULL AND state = 'running';
