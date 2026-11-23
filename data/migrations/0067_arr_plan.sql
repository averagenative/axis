-- 0067_arr_plan.sql - what a reading of Radarr or Sonarr found, kept for the
-- preview (import-from-arr, slice R2)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A reading (the job arr.read) stores what the arr app has as a plan: one
-- row for the reading, one for each movie.  The preview is drawn from these
-- rows and from Axis's own tables, without asking the arr app again.  A plan
-- writes nothing to the library: no movie, link, blocklist entry or search.
--
--   arr_plan.state   'reading' while the job works, 'complete' when it read
--                    everything, 'failed' when it could not.  Only a complete
--                    plan can be previewed, and for 24 hours: the job
--                    arr.cleanup deletes older ones.
--   problem          why a reading failed, in fixed words (never the key,
--                    never text of an answer).
--   profiles         the app's quality profiles, [{"id": 4, "name": "HD"}].
--   folders          the app's root folders, [{"path": "/movies",
--                    "accessible": true}].
--
--   arr_plan_item    one per movie, in the order of the app's answer: the
--                    three kinds of fact a reading keeps (what identifies it,
--                    what the person decided about it, its file) and no more.
--   match            'matched' (title_id is the stored title), 'no_id' (the
--                    app has no TMDb identifier for it), 'unknown' (TMDb does
--                    not know the identifier), 'not_looked_up' (TMDb did not
--                    answer), 'incomplete' (the app's record of it could not
--                    be used), 'pending' while the reading is at work.
--   in_queue         the app was downloading it when it was read.
--
-- The plan holds titles and paths of a person's library; nothing of it is
-- logged.  Forgetting a connection deletes its plans (ON DELETE CASCADE).
--
-- New tables only: nothing existing is rewritten.  The foreign key from
-- arr_plan_item.title_id to meta_title briefly takes a lock on meta_title
-- while it is added (SHARE ROW EXCLUSIVE: its reads go on, its writes wait),
-- and the tables are empty, so nothing is scanned.

CREATE TABLE arr_plan (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	source_id   bigint      NOT NULL REFERENCES arr_source (id) ON DELETE CASCADE,
	state       text        NOT NULL DEFAULT 'reading',
	started_at  timestamptz NOT NULL DEFAULT now(),
	started_by  text,
	finished_at timestamptz,
	problem     text,
	n_items     integer     NOT NULL DEFAULT 0,
	profiles    jsonb       NOT NULL DEFAULT '[]',
	folders     jsonb       NOT NULL DEFAULT '[]',

	CONSTRAINT arr_plan_state CHECK (state IN ('reading', 'complete', 'failed')),
	CONSTRAINT arr_plan_finished CHECK ((state = 'reading') = (finished_at IS NULL)),
	CONSTRAINT arr_plan_texts CHECK ((problem IS NULL OR char_length(problem) BETWEEN 1 AND 300)
		AND (started_by IS NULL OR char_length(started_by) BETWEEN 1 AND 200)),
	CONSTRAINT arr_plan_lists CHECK (jsonb_typeof(profiles) = 'array' AND jsonb_typeof(folders) = 'array'),
	CONSTRAINT arr_plan_count CHECK (n_items BETWEEN 0 AND 100000)
);

CREATE INDEX arr_plan_source ON arr_plan (source_id, id DESC);

CREATE TABLE arr_plan_item (
	plan_id          bigint  NOT NULL REFERENCES arr_plan (id) ON DELETE CASCADE,
	seq              integer NOT NULL,
	arr_id           bigint,
	title            text,
	year             integer,
	tmdb_id          bigint,
	imdb_id          text,
	monitored        boolean NOT NULL DEFAULT false,
	min_availability text,
	profile_id       bigint,
	path             text,
	root_path        text,
	has_file         boolean NOT NULL DEFAULT false,
	file_path        text,
	file_size        bigint,
	file_quality     text,
	file_scene       text,
	in_queue         boolean NOT NULL DEFAULT false,
	match            text    NOT NULL DEFAULT 'pending',
	title_id         bigint  REFERENCES meta_title (id) ON DELETE SET NULL,

	PRIMARY KEY (plan_id, seq),
	CONSTRAINT arr_plan_item_match
		CHECK (match IN ('pending', 'matched', 'no_id', 'unknown', 'not_looked_up', 'incomplete')),
	CONSTRAINT arr_plan_item_availability
		CHECK (min_availability IS NULL OR min_availability IN ('announced', 'in_cinemas', 'released')),
	CONSTRAINT arr_plan_item_texts CHECK ((title IS NULL OR char_length(title) BETWEEN 1 AND 300)
		AND (imdb_id IS NULL OR imdb_id ~ '^tt[0-9]{7,10}$')
		AND (path IS NULL OR octet_length(path) BETWEEN 1 AND 4096)
		AND (root_path IS NULL OR octet_length(root_path) BETWEEN 1 AND 4096)
		AND (file_path IS NULL OR octet_length(file_path) BETWEEN 1 AND 4096)
		AND (file_quality IS NULL OR char_length(file_quality) BETWEEN 1 AND 40)
		AND (file_scene IS NULL OR char_length(file_scene) BETWEEN 1 AND 300)),
	CONSTRAINT arr_plan_item_numbers CHECK ((year IS NULL OR year BETWEEN 1870 AND 2200)
		AND (file_size IS NULL OR file_size >= 0))
);

-- A stored title that goes (two records merged) lets go of its items.
CREATE INDEX arr_plan_item_title ON arr_plan_item (title_id) WHERE title_id IS NOT NULL;
