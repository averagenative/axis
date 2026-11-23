-- 0068_arr_brought.sql - what was brought over from Radarr, the hold on a
-- brought movie's searches, and the mark of a brought file's link
-- (import-from-arr, slice R3)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A run (the job arr.apply) adds the movies a preview showed, links the
-- files Axis's index holds at their mapped paths, and holds every movie it
-- brings from automatic searches until a person starts them.
--
--   arr_brought       one row for each title a connection brought: what a
--                     second run, a continued run and the report read.
--   title_id          the stored title; a connection brings a title once.
--   movie_id          the movie it made (NULL once that movie was removed in
--                     Axis: a later run does not bring it again, since that
--                     would undo what a person did).
--   arr_id            the app's own number of it, by which the app's download
--                     queue names it.
--   label             "Title (Year)", for the report after the movie went.
--   file              what became of the app's file: 'none' (the app has no
--                     file), 'linked', 'already' (it was linked to this movie
--                     already), 'not_indexed', 'other_size', 'elsewhere'
--                     (linked to another movie, named in file_detail),
--                     'unmappable', 'no_path', 'unlinked' (a person unlinked
--                     it by hand in Axis: left so).
--
--   arr_source.search_started_at  when a person last started searching for
--                     the movies this connection brought (NULL: never).
--
--   movie.search_hold  NULL, or why no automatic search runs for the movie:
--                     'brought' (brought over, searching not started yet),
--                     'file_unseen' (the app has a file for it that Axis
--                     cannot see), 'arr_downloading' (the app was downloading
--                     it when searching was started).  A person's own search
--                     of the movie ends it.
--
--   movie_file.hand_how, series_file.hand_how: 'brought', the link was made
--                     by bringing over.  It has no import and no identity, so
--                     identification leaves it alone; kept stays false, so it
--                     is upgraded by the ordinary rules.
--
-- Safe on a live database: the new column has no default (no row is
-- rewritten), and each CHECK is added NOT VALID and validated after, so the
-- tables are read once to check it.  The migration runs in one transaction, so
-- the table locks of these statements are held to its commit: reads and writes
-- of movie, movie_file and series_file wait for it.  At these tables' sizes
-- that is a brief wait.

CREATE TABLE arr_brought (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	source_id   bigint      NOT NULL REFERENCES arr_source (id) ON DELETE CASCADE,
	title_id    bigint      NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	movie_id    bigint      REFERENCES movie (id) ON DELETE SET NULL,
	arr_id      bigint,
	label       text        NOT NULL,
	file        text        NOT NULL DEFAULT 'none',
	file_detail text,
	brought_at  timestamptz NOT NULL DEFAULT now(),
	brought_by  text,
	updated_at  timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT arr_brought_once UNIQUE (source_id, title_id),
	CONSTRAINT arr_brought_file CHECK (file IN ('none', 'linked', 'already', 'not_indexed', 'other_size', 'elsewhere',
		'unmappable', 'no_path', 'unlinked')),
	CONSTRAINT arr_brought_texts CHECK (char_length(label) BETWEEN 1 AND 320
		AND (file_detail IS NULL OR char_length(file_detail) BETWEEN 1 AND 320)
		AND (brought_by IS NULL OR char_length(brought_by) BETWEEN 1 AND 200))
);

CREATE INDEX arr_brought_movie ON arr_brought (movie_id) WHERE movie_id IS NOT NULL;
CREATE INDEX arr_brought_title ON arr_brought (title_id);

ALTER TABLE arr_source ADD COLUMN search_started_at timestamptz;

ALTER TABLE movie ADD COLUMN search_hold text;
ALTER TABLE movie ADD CONSTRAINT movie_search_hold CHECK (search_hold IS NULL OR search_hold IN ('brought',
	'file_unseen', 'arr_downloading')) NOT VALID;
ALTER TABLE movie VALIDATE CONSTRAINT movie_search_hold;

-- The held movies are few beside the whole table once searching has started.
CREATE INDEX movie_search_hold_idx ON movie (search_hold) WHERE search_hold IS NOT NULL;

ALTER TABLE movie_file DROP CONSTRAINT movie_file_hand;
ALTER TABLE movie_file ADD CONSTRAINT movie_file_hand CHECK ((hand_how IS NULL OR hand_how IN ('forced', 'assigned',
	'linked', 'brought')) AND (hand_how IS NULL) = (hand_at IS NULL)
	AND (hand_by IS NULL OR char_length(hand_by) BETWEEN 1 AND 200)) NOT VALID;
ALTER TABLE movie_file VALIDATE CONSTRAINT movie_file_hand;

ALTER TABLE series_file DROP CONSTRAINT series_file_hand;
ALTER TABLE series_file ADD CONSTRAINT series_file_hand CHECK ((hand_how IS NULL OR hand_how IN ('forced', 'assigned',
	'linked', 'brought')) AND (hand_how IS NULL) = (hand_at IS NULL)
	AND (hand_by IS NULL OR char_length(hand_by) BETWEEN 1 AND 200)) NOT VALID;
ALTER TABLE series_file VALIDATE CONSTRAINT series_file_hand;
