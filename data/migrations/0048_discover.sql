-- 0048_discover.sql - the Discover lists: what the metadata provider lists
-- as popular, trending, in cinemas or on the air, coming soon, top rated, or
-- recommended for one title, kept so that a page never asks the provider;
-- the provider's genre names; and the titles marked "not interested"
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A list is a snapshot (src/discover/axis-discover.h): replaced whole when
-- every page of it arrived, kept with the problem when a page did not.  Its
-- titles are the metadata store's (brief records), so a card is drawn from
-- meta_title like every other.  A recommendation list belongs to the title it
-- was asked for (seed_title_id), which goes with it.

CREATE TABLE discover_list (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	provider      text        NOT NULL,
	kind          text        NOT NULL,
	list          text        NOT NULL,
	seed_title_id bigint      REFERENCES meta_title (id) ON DELETE CASCADE,
	-- How many pages the snapshot holds.
	pages         integer     NOT NULL DEFAULT 0,
	-- The snapshot's answer arrived then; NULL while there has been none.
	fetched_at    timestamptz,
	-- The last attempt, good or not.
	tried_at      timestamptz NOT NULL DEFAULT now(),
	-- Why the last attempt failed; NULL when it did not.
	problem       text,

	CONSTRAINT discover_list_provider CHECK (provider ~ '^[a-z][a-z0-9]{0,15}$'),
	CONSTRAINT discover_list_kind CHECK (kind IN ('film', 'series')),
	CONSTRAINT discover_list_name CHECK (list IN ('popular', 'trending', 'now', 'upcoming', 'top_rated', 'recommended')),
	CONSTRAINT discover_list_seed CHECK ((list = 'recommended') = (seed_title_id IS NOT NULL)),
	CONSTRAINT discover_list_pages CHECK (pages BETWEEN 0 AND 10),
	CONSTRAINT discover_list_problem CHECK (problem IS NULL OR char_length(problem) BETWEEN 1 AND 300)
);

CREATE UNIQUE INDEX discover_list_key ON discover_list (provider, kind, list, (coalesce(seed_title_id, 0)));
CREATE INDEX discover_list_seed_title ON discover_list (seed_title_id) WHERE seed_title_id IS NOT NULL;

-- One title of a snapshot, at its place in it.  rating is the provider's
-- average in tenths of a point out of ten; NULL when nobody voted.
CREATE TABLE discover_entry (
	list_id   bigint    NOT NULL REFERENCES discover_list (id) ON DELETE CASCADE,
	position  integer   NOT NULL,
	title_id  bigint    NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	rating    integer,
	votes     integer   NOT NULL DEFAULT 0,
	genre_ids integer[] NOT NULL DEFAULT '{}',

	PRIMARY KEY (list_id, position),
	CONSTRAINT discover_entry_once UNIQUE (list_id, title_id),
	CONSTRAINT discover_entry_position CHECK (position BETWEEN 1 AND 1000),
	CONSTRAINT discover_entry_rating CHECK (rating IS NULL OR rating BETWEEN 0 AND 100),
	CONSTRAINT discover_entry_votes CHECK (votes >= 0),
	CONSTRAINT discover_entry_genres CHECK (cardinality(genre_ids) <= 20)
);

CREATE INDEX discover_entry_title ON discover_entry (title_id);

-- The provider's names for its genre numbers, for the filter.
CREATE TABLE discover_genre (
	provider   text        NOT NULL,
	kind       text        NOT NULL,
	genre_id   integer     NOT NULL,
	name       text        NOT NULL,
	fetched_at timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (provider, kind, genre_id),
	CONSTRAINT discover_genre_kind CHECK (kind IN ('film', 'series')),
	CONSTRAINT discover_genre_number CHECK (genre_id >= 1),
	CONSTRAINT discover_genre_name CHECK (char_length(name) BETWEEN 1 AND 64)
);

-- "Not interested": left out of every Discover list, for the installation.
CREATE TABLE discover_dismissed (
	title_id     bigint      PRIMARY KEY REFERENCES meta_title (id) ON DELETE CASCADE,
	dismissed_by text,
	dismissed_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT discover_dismissed_by CHECK (dismissed_by IS NULL OR char_length(dismissed_by) BETWEEN 1 AND 64)
);

CREATE INDEX discover_dismissed_when ON discover_dismissed (dismissed_at DESC, title_id);

-- Two records that turn out to be one title merge (the metadata store moves
-- the identifiers and deletes the other record): a title marked "not
-- interested" stays marked as the record that is kept, as a managed film
-- follows its record (0023_movies.sql).
CREATE FUNCTION discover_follow_title_merge() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF OLD.title_id <> NEW.title_id
	   AND NOT EXISTS (SELECT 1 FROM meta_external_id WHERE title_id = OLD.title_id) THEN
		INSERT INTO discover_dismissed (title_id, dismissed_by, dismissed_at)
			SELECT NEW.title_id, dismissed_by, dismissed_at FROM discover_dismissed WHERE title_id = OLD.title_id
			ON CONFLICT (title_id) DO NOTHING;
		DELETE FROM discover_dismissed WHERE title_id = OLD.title_id;
	END IF;
	RETURN NULL;
END $$;

CREATE TRIGGER discover_follows_title_merge AFTER UPDATE OF title_id ON meta_external_id
	FOR EACH ROW EXECUTE FUNCTION discover_follow_title_merge();
