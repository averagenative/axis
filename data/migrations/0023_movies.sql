-- 0023_movies.sql - the films Axis manages: each film, its searches and the
-- releases each one considered, its grabs, its blocklist, its history, and
-- the link from a film to its files in the library
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A film's state (wanted, downloading, to import, ...) is not stored: it is
-- derived from the facts here, the download it was sent as (download), the
-- job that searches for it (job, by its de-duplication key) and the files
-- linked to it, by one query and one C function
-- (src/modules/movies/axis-movies-store.c, axis-movies-state.c).
--
-- Grabbing once: a grab is written as a claim, in one transaction with the
-- film's row locked and its grabs and downloads read under that lock, before
-- the release is fetched or sent; the unique index movie_grab_one_open holds
-- the same rule in the database: at most one grab of a film is open (not
-- closed).  A grab is closed when it failed and was dealt with, was never
-- sent, or its film's download was otherwise ended; a completed grab stays
-- open until the import pipeline takes it.
--
-- The blocklist is per film, keyed by what identifies a release: its
-- info-hash when it is a torrent whose hash is known ('hash:' and the
-- hash), else the source, the indexer and the release name in a folded form
-- ('name:' and that).  Every entry keeps the folded name too, so that a
-- torrent listed later without its hash is still recognised.

CREATE TABLE movie (
	id                 bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	title_id           bigint      NOT NULL REFERENCES meta_title (id) ON DELETE RESTRICT,
	quality_profile_id bigint      NOT NULL REFERENCES quality_profile (id) ON DELETE RESTRICT,
	-- Where it goes when it is imported (the import pipeline's); a removed
	-- root leaves the film without one, which the film's page says.
	root_id            bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	monitored          boolean     NOT NULL DEFAULT true,
	min_availability   text        NOT NULL DEFAULT 'released',
	added_by           text,
	added_at           timestamptz NOT NULL DEFAULT now(),
	updated_at         timestamptz NOT NULL DEFAULT now(),
	-- The pacing of automatic searches: the last search, the earliest time
	-- the scheduler searches again (NULL: as soon as it may), and the floor
	-- between two searches, which doubles while searches find nothing.
	last_search_at     timestamptz,
	next_search_at     timestamptz,
	search_floor_s     integer     NOT NULL DEFAULT 21600,
	-- What the scheduler last saw of its availability, to notice a change.
	seen_available     boolean,
	seen_release_date  date,

	CONSTRAINT movie_title_unique UNIQUE (title_id),
	CONSTRAINT movie_availability CHECK (min_availability IN ('announced', 'in_cinemas', 'released')),
	CONSTRAINT movie_added_by CHECK (added_by IS NULL OR char_length(added_by) BETWEEN 1 AND 200),
	CONSTRAINT movie_floor CHECK (search_floor_s BETWEEN 60 AND 31536000)
);

CREATE INDEX movie_quality_profile ON movie (quality_profile_id);
CREATE INDEX movie_root ON movie (root_id);

-- One search for a film and how it ended.  It is written when its releases
-- have been decided (finished_at still NULL while the grab is tried), or at
-- once when nothing could be asked.
CREATE TABLE movie_search (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	movie_id     bigint      NOT NULL REFERENCES movie (id) ON DELETE CASCADE,
	cause        text        NOT NULL,
	requested_by text,
	started_at   timestamptz NOT NULL DEFAULT now(),
	finished_at  timestamptz,
	outcome      text,
	outcome_text text,
	-- How it was searched, in words ("Prowlarr by IMDb identifier").
	how          text,
	n_sources    integer     NOT NULL DEFAULT 0,
	n_found      integer     NOT NULL DEFAULT 0,
	n_accepted   integer     NOT NULL DEFAULT 0,
	-- The titles the release names were compared with (this film and the
	-- others of that name the metadata provider knows).
	n_compared   integer     NOT NULL DEFAULT 0,

	CONSTRAINT movie_search_cause CHECK (cause IN ('added', 'manual', 'scheduled', 'retry')),
	CONSTRAINT movie_search_outcome CHECK (outcome IS NULL OR outcome IN ('grabbed', 'nothing_found',
		'none_acceptable', 'exhausted', 'no_indexer', 'indexer_error', 'no_client', 'client_error', 'root_offline',
		'profile_unusable', 'metadata_missing', 'in_flight', 'failed')),
	CONSTRAINT movie_search_finished CHECK ((finished_at IS NULL) = (outcome IS NULL)),
	CONSTRAINT movie_search_texts CHECK ((requested_by IS NULL OR char_length(requested_by) BETWEEN 1 AND 200)
		AND (outcome_text IS NULL OR char_length(outcome_text) <= 2000)
		AND (how IS NULL OR char_length(how) <= 1000)),
	CONSTRAINT movie_search_counts CHECK (n_sources >= 0 AND n_found >= 0 AND n_accepted >= 0 AND n_compared >= 0)
);

CREATE INDEX movie_search_movie ON movie_search (movie_id, id DESC);

-- Every release a search considered, with its verdict and every reason.
-- release_id is the indexer layer's stored release (indexer_release.id),
-- through which a release is fetched while that search lives (fifteen
-- minutes by default); it has no foreign key because it expires.
CREATE TABLE movie_candidate (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	search_id    bigint      NOT NULL REFERENCES movie_search (id) ON DELETE CASCADE,
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
	-- [{"k": "safety"|"profile"|"upgrade"|"blocklist", "c": code, "t": text}]
	reasons      jsonb       NOT NULL DEFAULT '[]',
	-- What happened to it in the grab, when anything did ("grabbed", or why
	-- an accepted one was passed over).
	note         text,

	CONSTRAINT movie_candidate_position UNIQUE (search_id, position),
	CONSTRAINT movie_candidate_form CHECK (
		char_length(name) BETWEEN 1 AND 300 AND name !~ '[\x01-\x1f\x7f]'
		AND protocol IN ('usenet', 'torrent')
		AND match IN ('match', 'none', 'ambiguous')
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND char_length(name_key) BETWEEN 1 AND 400
		AND (indexer IS NULL OR char_length(indexer) <= 100)
		AND (match_text IS NULL OR char_length(match_text) <= 1000)
		AND (note IS NULL OR char_length(note) <= 1000)
		AND jsonb_typeof(reasons) = 'array'
		AND rank >= 0 AND (size_bytes IS NULL OR size_bytes >= 0) AND (seeders IS NULL OR seeders >= 0))
);

CREATE INDEX movie_candidate_search ON movie_candidate (search_id, position);

-- A release grabbed for a film: claimed (download_id NULL, not closed),
-- sent (download_id set), completed, or closed with the reason.
CREATE TABLE movie_grab (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	movie_id     bigint      NOT NULL REFERENCES movie (id) ON DELETE CASCADE,
	search_id    bigint      REFERENCES movie_search (id) ON DELETE SET NULL,
	candidate_id bigint      REFERENCES movie_candidate (id) ON DELETE SET NULL,
	release_name text        NOT NULL,
	protocol     text        NOT NULL,
	source_id    bigint,
	indexer      text,
	indexer_id   bigint      NOT NULL DEFAULT 0,
	size_bytes   bigint,
	quality      text,
	name_key     text        NOT NULL,
	info_hash    text,
	-- Why it was chosen, in words.
	why          text,
	manual       boolean     NOT NULL DEFAULT false,
	grabbed_by   text,
	download_id  bigint      REFERENCES download (id) ON DELETE SET NULL,
	created_at   timestamptz NOT NULL DEFAULT now(),
	sent_at      timestamptz,
	completed_at timestamptz,
	closed_at    timestamptz,
	close_reason text,
	close_text   text,
	-- Its download's removal from the client after a failure: asked for and
	-- not done yet, done, or failed (tried again).
	removal      text,
	removal_text text,
	-- When the search for the next release was queued after it failed.
	retry_at     timestamptz,

	CONSTRAINT movie_grab_form CHECK (
		char_length(release_name) BETWEEN 1 AND 300
		AND protocol IN ('usenet', 'torrent')
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND char_length(name_key) BETWEEN 1 AND 400
		AND (grabbed_by IS NULL OR char_length(grabbed_by) BETWEEN 1 AND 200)
		AND (why IS NULL OR char_length(why) <= 2000)
		AND (close_text IS NULL OR char_length(close_text) <= 1000)
		AND (removal_text IS NULL OR char_length(removal_text) <= 1000)),
	CONSTRAINT movie_grab_close CHECK ((closed_at IS NULL) = (close_reason IS NULL)),
	CONSTRAINT movie_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN ('not_sent', 'failed',
		'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up')),
	CONSTRAINT movie_grab_removal CHECK (removal IS NULL OR removal IN ('pending', 'done', 'failed'))
);

-- At most one open grab per film: the database's own guard against grabbing
-- twice (the claim transaction is the first).
CREATE UNIQUE INDEX movie_grab_one_open ON movie_grab (movie_id) WHERE closed_at IS NULL;
CREATE INDEX movie_grab_movie ON movie_grab (movie_id, id DESC);
CREATE INDEX movie_grab_download ON movie_grab (download_id) WHERE download_id IS NOT NULL;

-- The facts query asks, for every film, whether a download of its title is
-- still at a client (download keeps every download ever made).
CREATE INDEX download_movies_live ON download (title_id)
	WHERE module = 'movies' AND state IN ('sending', 'queued', 'downloading', 'paused');

CREATE TABLE movie_blocklist (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	movie_id     bigint      NOT NULL REFERENCES movie (id) ON DELETE CASCADE,
	key          text        NOT NULL,
	name_key     text        NOT NULL,
	info_hash    text,
	release_name text        NOT NULL,
	indexer      text,
	reason       text        NOT NULL,
	reason_text  text,
	grab_id      bigint      REFERENCES movie_grab (id) ON DELETE SET NULL,
	created_by   text,
	created_at   timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT movie_blocklist_unique UNIQUE (movie_id, key),
	CONSTRAINT movie_blocklist_form CHECK (
		(key ~ '^hash:[0-9a-f]{40}$' OR (key LIKE 'name:%' AND char_length(key) <= 410))
		AND char_length(name_key) BETWEEN 1 AND 400
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND char_length(release_name) BETWEEN 1 AND 300
		AND reason IN ('failed', 'stalled', 'gone', 'not_taken', 'unfetchable', 'refused', 'given_up')
		AND (reason_text IS NULL OR char_length(reason_text) <= 1000)
		AND (created_by IS NULL OR char_length(created_by) BETWEEN 1 AND 200))
);

-- What happened to a film, kept when the film is removed (with its name).
CREATE TABLE movie_event (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	movie_id    bigint      REFERENCES movie (id) ON DELETE SET NULL,
	title_id    bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	title_label text        NOT NULL,
	kind        text        NOT NULL,
	text        text        NOT NULL,
	by_user     text,
	search_id   bigint,
	grab_id     bigint,
	created_at  timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT movie_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'searched', 'skipped', 'grabbed',
		'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal', 'completed', 'available', 'abandoned')),
	CONSTRAINT movie_event_texts CHECK (char_length(title_label) BETWEEN 1 AND 600
		AND char_length(text) BETWEEN 1 AND 4000
		AND (by_user IS NULL OR char_length(by_user) BETWEEN 1 AND 200))
);

CREATE INDEX movie_event_movie ON movie_event (movie_id, id DESC);

-- A file of the library that is a film's.  Only the import pipeline links
-- files (add-import-pipeline, and later add-library-identification); a film
-- with a linked file that is present is in the library.
CREATE TABLE movie_file (
	file_id   bigint      PRIMARY KEY REFERENCES library_file (id) ON DELETE CASCADE,
	movie_id  bigint      NOT NULL REFERENCES movie (id) ON DELETE CASCADE,
	-- The ladder quality recorded for it (src/quality/axis-quality.h).
	quality   text,
	linked_at timestamptz NOT NULL DEFAULT now(),
	linked_by text,

	CONSTRAINT movie_file_form CHECK ((quality IS NULL OR quality ~ '^[a-z0-9_]{1,32}$')
		AND (linked_by IS NULL OR char_length(linked_by) BETWEEN 1 AND 200))
);

CREATE INDEX movie_file_movie ON movie_file (movie_id);

-- When two records of a title merge (src/meta/axis-meta-store.c moves the
-- other record's identifiers to the one kept, then deletes the other), a
-- film of the record that goes follows its identifiers to the one kept.
-- Only when the old record is left with no identifier at all (a merge): an
-- identifier that merely moves between two titles that both remain moves
-- no film.  When both records are films Axis manages the merge is refused
-- (the second film would break movie_title_unique): two films that turn
-- out to be one need a person.
CREATE FUNCTION movie_follow_title_merge() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF OLD.title_id <> NEW.title_id
	   AND NOT EXISTS (SELECT 1 FROM meta_external_id WHERE title_id = OLD.title_id) THEN
		UPDATE movie SET title_id = NEW.title_id, updated_at = now() WHERE title_id = OLD.title_id;
	END IF;
	RETURN NULL;
END $$;

CREATE TRIGGER movie_follows_title_merge AFTER UPDATE OF title_id ON meta_external_id
	FOR EACH ROW EXECUTE FUNCTION movie_follow_title_merge();
