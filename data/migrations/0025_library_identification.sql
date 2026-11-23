-- 0025_library_identification.sql - what the films already in the library
-- are: each film folder or file of a films root matched to a metadata title,
-- with its evidence and a confidence; what people confirmed, chose, marked or
-- undid; and the films' links to those files
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Identification (src/identify/) only reads the library: the index, the
-- stored probe results, and sidecar .nfo files through a volume guard taken
-- to read.  Nothing here, and nothing in the code that writes it, renames,
-- moves, creates or changes a file.
--
-- A scope is what is looked at in one go: a directory of a films root that
-- holds video files directly, or one video file at the top of the root.  Its
-- fingerprint is a hash of the names and sizes of its videos and of whether
-- each has a probed duration: a scope whose fingerprint is the one recorded is
-- not looked at again, so a rescan of an unchanged root asks nothing of the
-- metadata provider.
--
-- A unit is what is identified: a film folder (the videos of a folder that
-- read as one title), or one file (a video at the top of the root, or one of
-- several films in one folder).  Its identity says what it is whether or not
-- the film is managed.  A managed film's movie_file rows are made from its
-- linked identities (movie_file.identity_id) and go with them; a film added
-- later gets them at once.  An import's link of a file wins: such a row has no
-- identity, and the identity of its unit says "imported by Axis".

-- Per films root: how far identification has got.
CREATE TABLE library_identity_root (
	root_id       bigint      PRIMARY KEY REFERENCES library_root (id) ON DELETE CASCADE,
	-- Scopes the last run found, and how many of them it left to look at.
	scopes        integer     NOT NULL DEFAULT 0,
	pending       integer     NOT NULL DEFAULT 0,
	last_run_at   timestamptz,
	last_run_text text,
	-- The metadata provider did not answer: the next run waits until then.
	retry_after   timestamptz,

	CONSTRAINT library_identity_root_counts CHECK (scopes >= 0 AND pending >= 0),
	CONSTRAINT library_identity_root_text CHECK (last_run_text IS NULL OR char_length(last_run_text) <= 1000)
);

CREATE TABLE library_identity_scope (
	root_id     bigint      NOT NULL REFERENCES library_root (id) ON DELETE CASCADE,
	-- A directory, or a file at the top of the root: the stored form of
	-- src/library/axis-library-name.h, relative to the root.
	path        text        COLLATE "C" NOT NULL,
	path_hash   bytea       GENERATED ALWAYS AS (library_path_hash(path)) STORED,
	fingerprint text        NOT NULL,
	examined_at timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (root_id, path_hash),
	CONSTRAINT library_identity_scope_form CHECK (path <> '' AND path !~ '(^/|/$|//)'
		AND char_length(path) <= 4096 AND fingerprint ~ '^[0-9a-f]{32}$')
);

CREATE TABLE library_identity (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	root_id     bigint      NOT NULL REFERENCES library_root (id) ON DELETE CASCADE,
	-- The scope it was found in (library_identity_scope.path_hash).
	scope_hash  bytea       NOT NULL,
	unit_kind   text        NOT NULL,
	-- The folder, or the file (its first video when it holds several).
	rel_path    text        COLLATE "C" NOT NULL,
	path_hash   bytea       GENERATED ALWAYS AS (library_path_hash(rel_path)) STORED,
	-- linked: it is title_id (linked_how says how); review: a person must
	-- decide; not_film: a person said it is not a film; waiting: the provider
	-- did not answer, looked at again later.
	state       text        NOT NULL DEFAULT 'review',
	-- What the evidence said when it was last looked at.
	confidence  text,
	title_id    bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	-- auto (sure), confirmed (a person confirmed the best candidate), chosen
	-- (a person chose another title), imported (an import of Axis linked it).
	linked_how  text,
	-- A person decided (linked, chose, marked, undid): who and when.  Once a
	-- person has decided, nothing is linked by itself again.
	decided_by  text,
	decided_at  timestamptz,
	held        boolean     NOT NULL DEFAULT false,
	-- [{"c": code, "t": text}]
	reasons     jsonb       NOT NULL DEFAULT '[]',
	-- [{"id": meta_title id, "t": why}], best first, at most eight.
	candidates  jsonb       NOT NULL DEFAULT '[]',
	-- What the names said, in words; the title and year read, for a search.
	read_text   text,
	read_title  text,
	read_year   integer,
	-- The main video's probed duration (NULL: not probed).
	duration_ms bigint,
	problem     text,
	examined_at timestamptz NOT NULL DEFAULT now(),
	created_at  timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT library_identity_path UNIQUE (root_id, path_hash),
	CONSTRAINT library_identity_kind CHECK (unit_kind IN ('folder', 'file')),
	CONSTRAINT library_identity_state CHECK (state IN ('linked', 'review', 'not_film', 'waiting')),
	CONSTRAINT library_identity_confidence CHECK (confidence IS NULL OR confidence IN ('sure', 'likely', 'unknown')),
	CONSTRAINT library_identity_how CHECK (linked_how IS NULL OR linked_how IN ('auto', 'confirmed', 'chosen',
		'imported')),
	CONSTRAINT library_identity_linked CHECK (state = 'linked' OR linked_how IS NULL),
	CONSTRAINT library_identity_form CHECK (
		rel_path <> '' AND rel_path !~ '(^/|/$|//)' AND char_length(rel_path) <= 4096
		AND jsonb_typeof(reasons) = 'array' AND jsonb_typeof(candidates) = 'array'
		AND (decided_by IS NULL OR char_length(decided_by) BETWEEN 1 AND 200)
		AND (read_text IS NULL OR char_length(read_text) <= 2000)
		AND (read_title IS NULL OR char_length(read_title) BETWEEN 1 AND 200)
		AND (read_year IS NULL OR read_year BETWEEN 1870 AND 2200)
		AND (duration_ms IS NULL OR duration_ms >= 0)
		AND (problem IS NULL OR char_length(problem) <= 1000))
);

CREATE INDEX library_identity_scope_idx ON library_identity (root_id, scope_hash);
CREATE INDEX library_identity_title_idx ON library_identity (title_id) WHERE title_id IS NOT NULL;
CREATE INDEX library_identity_list_idx ON library_identity (root_id, state, confidence, id);

-- The videos of a unit.  A library file is in at most one unit.  best: the
-- one a film's upgrades are judged by (the best by quality).
CREATE TABLE library_identity_file (
	file_id      bigint      PRIMARY KEY REFERENCES library_file (id) ON DELETE CASCADE,
	identity_id  bigint      NOT NULL REFERENCES library_identity (id) ON DELETE CASCADE,
	best         boolean     NOT NULL DEFAULT false,
	-- Its ladder quality (src/quality/axis-quality.h), NULL when its name
	-- states no source; then resolution, when known, makes it "an unknown
	-- source at that resolution".
	quality      text,
	resolution   text,
	quality_text text,

	CONSTRAINT library_identity_file_form CHECK ((quality IS NULL OR quality ~ '^[a-z0-9_]{1,32}$')
		AND (resolution IS NULL OR resolution IN ('480p', '576p', '720p', '1080p', '2160p'))
		AND (quality_text IS NULL OR char_length(quality_text) <= 1000))
);

CREATE INDEX library_identity_file_identity ON library_identity_file (identity_id);

-- What people did, with who.  The unit's path is kept with the action, so the
-- record outlives the unit.
CREATE TABLE library_identity_action (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	identity_id bigint      REFERENCES library_identity (id) ON DELETE SET NULL,
	root_id     bigint      REFERENCES library_root (id) ON DELETE CASCADE,
	rel_path    text        COLLATE "C",
	action      text        NOT NULL,
	title_id    bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	by_user     text        NOT NULL,
	text        text,
	created_at  timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT library_identity_action_kind CHECK (action IN ('confirm', 'choose', 'not_film', 'undo', 'add_all',
		'look_again')),
	CONSTRAINT library_identity_action_texts CHECK (char_length(by_user) BETWEEN 1 AND 200
		AND (text IS NULL OR char_length(text) <= 2000) AND (rel_path IS NULL OR char_length(rel_path) <= 4096))
);

CREATE INDEX library_identity_action_identity ON library_identity_action (identity_id, id);
CREATE INDEX library_identity_action_root ON library_identity_action (root_id, id DESC);

-- A film's link to a file learns where it came from besides an import: the
-- identity it was made from, and the file's resolution when its quality is
-- an unknown source.  A row has one origin at most.
ALTER TABLE movie_file
	ADD COLUMN identity_id bigint REFERENCES library_identity (id) ON DELETE CASCADE,
	ADD COLUMN resolution  text,
	ADD CONSTRAINT movie_file_resolution CHECK (resolution IS NULL OR resolution IN ('480p', '576p', '720p', '1080p',
		'2160p')),
	ADD CONSTRAINT movie_file_one_origin CHECK (identity_id IS NULL OR import_id IS NULL);

CREATE INDEX movie_file_identity ON movie_file (identity_id) WHERE identity_id IS NOT NULL;

ALTER TABLE movie_event DROP CONSTRAINT movie_event_kind;
ALTER TABLE movie_event ADD CONSTRAINT movie_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'searched',
	'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal', 'completed', 'available',
	'abandoned', 'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded', 'identified',
	'unidentified'));

-- When two records of a title merge, an identity of the record that goes
-- follows its identifiers to the one kept (as a film does, 0023_movies.sql).
CREATE FUNCTION library_identity_follow_title_merge() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF OLD.title_id <> NEW.title_id
	   AND NOT EXISTS (SELECT 1 FROM meta_external_id WHERE title_id = OLD.title_id) THEN
		UPDATE library_identity SET title_id = NEW.title_id WHERE title_id = OLD.title_id;
	END IF;
	RETURN NULL;
END $$;

CREATE TRIGGER library_identity_follows_title_merge AFTER UPDATE OF title_id ON meta_external_id
	FOR EACH ROW EXECUTE FUNCTION library_identity_follow_title_merge();
