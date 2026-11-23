-- 0029_title_lists.sql - lists of titles at scale: a name filter that ignores
-- case and accents, the newest films first by index, and each account's
-- choice of view for each list
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- name_keys holds the comparison key (axis_release_title_key(): decomposed,
-- case-folded, marks removed, "&" read as "and", roman numerals as numbers,
-- nothing but letters and digits) of a title's name, its original name and
-- every other name it has, joined by '|', which no key contains.  "Name
-- contains" is then the key of what was typed found anywhere in it.  The
-- metadata layer writes it with every title it stores (src/meta/
-- axis-meta-store.c) and fills in, on demand, the titles stored before this
-- migration, whose name_keys is NULL until then.

ALTER TABLE meta_title ADD COLUMN name_keys text;

CREATE INDEX meta_title_name_keys_missing ON meta_title (id) WHERE name_keys IS NULL;

-- As for file names (0008_library_browser.sql): a trigram index where the
-- extension can be had; without it a name filter reads the managed titles,
-- which a list of films or series is bounded by anyway.
DO $$
BEGIN
	CREATE EXTENSION IF NOT EXISTS pg_trgm;
	CREATE INDEX meta_title_name_keys_trgm ON meta_title USING gin (name_keys gin_trgm_ops);
EXCEPTION WHEN OTHERS THEN
	RAISE NOTICE 'pg_trgm is not available, title name filters will not be indexed: %', SQLERRM;
END
$$;

-- The orders of the lists of films and series, read a page at a time: the
-- newest first, by title, by year (the newest first, no year last).  The
-- title orders end on the title's id, which one film or series has.
CREATE INDEX movie_added ON movie (added_at DESC, id DESC);
CREATE INDEX meta_title_by_name ON meta_title (lower(title), coalesce(year, 0), id);
CREATE INDEX meta_title_by_year ON meta_title ((-coalesce(year, 0)), lower(title), id);

-- The view each account last chose for each list of titles ("films",
-- "series", "library-movies", ...): "posters", "overview" or "table".  An
-- account with no row for a list sees its default (posters).
CREATE TABLE user_list_view (
	user_id    bigint      NOT NULL REFERENCES app_user (id) ON DELETE CASCADE,
	list       text        NOT NULL,
	view       text        NOT NULL,
	updated_at timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (user_id, list),
	CONSTRAINT user_list_view_list CHECK (list ~ '^[a-z][a-z0-9-]{0,31}$'),
	CONSTRAINT user_list_view_view CHECK (view IN ('posters', 'overview', 'table'))
);
