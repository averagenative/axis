-- 0018_metadata.sql - what the metadata providers said: titles, their
-- identifiers and other names, seasons and episodes, searches, artwork
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Every provider answer the daemon uses is kept here, and pages and jobs
-- read these tables, never a provider directly (src/meta/axis-meta.h).
--
-- A title's identity is its external identifiers, never its text: one row
-- of meta_external_id per (kind, scheme, value), so the same title found by
-- a search and later by an IMDb id is one meta_title.  The kind is part of
-- the key because TMDb numbers films and series separately (film 550 and
-- series 550 are different titles).
--
-- Freshness is decided from fetched_at (anything about the title stored)
-- and detail_fetched_at (its full record fetched); viewed_at is when a page
-- last showed the title, one of the reasons the refresh job keeps it
-- current.  Dates are dates; text is cleaned before it is stored.

CREATE TABLE meta_title (
	id                bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	kind              text        NOT NULL,
	title             text        NOT NULL,
	original_title    text,
	year              integer,
	overview          text,
	runtime_minutes   integer,
	status            text,
	genres            text[]      NOT NULL DEFAULT '{}',
	original_language text,
	release_date      date,
	first_air_date    date,
	last_air_date     date,
	poster_ref        text,
	backdrop_ref      text,
	provider          text        NOT NULL,
	provider_id       text        NOT NULL,
	fetched_at        timestamptz NOT NULL DEFAULT now(),
	detail_fetched_at timestamptz,
	viewed_at         timestamptz,
	created_at        timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT meta_title_kind CHECK (kind IN ('film', 'series')),
	CONSTRAINT meta_title_status CHECK (status IS NULL OR status IN ('released', 'continuing', 'ended', 'upcoming')),
	CONSTRAINT meta_title_text CHECK (char_length(title) BETWEEN 1 AND 500),
	CONSTRAINT meta_title_year CHECK (year IS NULL OR year BETWEEN 1870 AND 2200),
	CONSTRAINT meta_title_runtime CHECK (runtime_minutes IS NULL OR runtime_minutes BETWEEN 1 AND 10000),
	CONSTRAINT meta_title_provider_form CHECK (provider ~ '^[a-z][a-z0-9]{0,15}$'),
	CONSTRAINT meta_title_ref_form CHECK ((poster_ref IS NULL OR poster_ref ~ '^/[A-Za-z0-9._/-]{1,199}$')
		AND (backdrop_ref IS NULL OR backdrop_ref ~ '^/[A-Za-z0-9._/-]{1,199}$'))
);

CREATE INDEX meta_title_refresh ON meta_title (provider, (coalesce(detail_fetched_at, '-infinity'::timestamptz)));
CREATE INDEX meta_title_viewed ON meta_title (viewed_at) WHERE viewed_at IS NOT NULL;

CREATE TABLE meta_external_id (
	title_id bigint NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	kind     text   NOT NULL,
	scheme   text   NOT NULL,
	value    text   NOT NULL,

	PRIMARY KEY (kind, scheme, value),
	CONSTRAINT meta_external_id_kind CHECK (kind IN ('film', 'series')),
	-- tmdb, imdb, tvdb, or a provider's own scheme (its name)
	CONSTRAINT meta_external_id_scheme CHECK (scheme ~ '^[a-z][a-z0-9]{1,15}$'),
	CONSTRAINT meta_external_id_value CHECK (value ~ '^[A-Za-z0-9]{1,24}$')
);

CREATE INDEX meta_external_id_title ON meta_external_id (title_id);

CREATE TABLE meta_alt_title (
	title_id bigint NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	title    text   NOT NULL,
	country  text   NOT NULL DEFAULT '',
	language text   NOT NULL DEFAULT '',

	PRIMARY KEY (title_id, title, country, language),
	CONSTRAINT meta_alt_title_text CHECK (char_length(title) BETWEEN 1 AND 500),
	CONSTRAINT meta_alt_title_country CHECK (country ~ '^([A-Z]{2})?$'),
	CONSTRAINT meta_alt_title_language CHECK (language ~ '^([a-z]{2,3})?$')
);

CREATE TABLE meta_season (
	title_id            bigint      NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	number              integer     NOT NULL,
	name                text,
	air_date            date,
	overview            text,
	episode_count       integer,
	fetched_at          timestamptz NOT NULL DEFAULT now(),
	-- When its episodes were fetched; NULL until they are.
	episodes_fetched_at timestamptz,

	PRIMARY KEY (title_id, number),
	CONSTRAINT meta_season_number CHECK (number BETWEEN 0 AND 10000),
	CONSTRAINT meta_season_count CHECK (episode_count IS NULL OR episode_count BETWEEN 0 AND 100000)
);

CREATE TABLE meta_episode (
	title_id        bigint  NOT NULL,
	season_number   integer NOT NULL,
	number          integer NOT NULL,
	absolute_number integer,
	name            text,
	air_date        date,
	overview        text,
	runtime_minutes integer,

	PRIMARY KEY (title_id, season_number, number),
	-- A season moves with its episodes when two records of a title merge.
	FOREIGN KEY (title_id, season_number) REFERENCES meta_season (title_id, number)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT meta_episode_number CHECK (number BETWEEN 0 AND 100000),
	CONSTRAINT meta_episode_absolute CHECK (absolute_number IS NULL OR absolute_number BETWEEN 1 AND 1000000),
	CONSTRAINT meta_episode_runtime CHECK (runtime_minutes IS NULL OR runtime_minutes BETWEEN 1 AND 10000)
);

-- A lookup and what it answered: a search by text ('search'), a lookup by
-- an external identifier ('find') or of a title by the provider's own id
-- ('title').  The query is the normalised text, or "scheme:value", or the
-- id.  An empty result_ids is a negative answer: nothing found.
CREATE TABLE meta_search (
	provider   text        NOT NULL,
	kind       text        NOT NULL,
	lookup     text        NOT NULL,
	query      text        NOT NULL,
	year       integer     NOT NULL DEFAULT 0,
	result_ids bigint[]    NOT NULL DEFAULT '{}',
	fetched_at timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (provider, kind, lookup, query, year),
	CONSTRAINT meta_search_kind CHECK (kind IN ('film', 'series')),
	CONSTRAINT meta_search_lookup CHECK (lookup IN ('search', 'find', 'title')),
	CONSTRAINT meta_search_query CHECK (char_length(query) BETWEEN 1 AND 300),
	CONSTRAINT meta_search_year CHECK (year = 0 OR year BETWEEN 1870 AND 2200),
	CONSTRAINT meta_search_results CHECK (cardinality(result_ids) <= 100)
);

CREATE INDEX meta_search_fetched ON meta_search (fetched_at);

-- One image as a provider names it (source_ref) at one size.  The file is
-- in the data directory's artwork directory, named by its content's
-- SHA-256 (several rows may share one); a row without sha256 is an image
-- that was refused (problem says why), kept so that it is not fetched
-- again until it is stale.
CREATE TABLE meta_artwork (
	provider     text        NOT NULL,
	source_ref   text        NOT NULL,
	size         text        NOT NULL,
	sha256       text,
	bytes        bigint,
	content_type text,
	width        integer,
	height       integer,
	problem      text,
	fetched_at   timestamptz NOT NULL DEFAULT now(),
	last_used_at timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (provider, source_ref, size),
	CONSTRAINT meta_artwork_size CHECK (size IN ('poster-small', 'poster', 'backdrop')),
	CONSTRAINT meta_artwork_ref_form CHECK (source_ref ~ '^/[A-Za-z0-9._/-]{1,199}$'),
	CONSTRAINT meta_artwork_hash_form CHECK (sha256 IS NULL OR sha256 ~ '^[0-9a-f]{64}$'),
	CONSTRAINT meta_artwork_kept_or_refused CHECK (
		(sha256 IS NOT NULL AND bytes IS NOT NULL AND content_type IS NOT NULL AND width IS NOT NULL
		 AND height IS NOT NULL AND problem IS NULL)
		OR (sha256 IS NULL AND problem IS NOT NULL)),
	CONSTRAINT meta_artwork_type CHECK (content_type IS NULL OR content_type IN ('image/jpeg', 'image/png', 'image/webp')),
	CONSTRAINT meta_artwork_bytes CHECK (bytes IS NULL OR bytes BETWEEN 1 AND 104857600),
	CONSTRAINT meta_artwork_dimensions CHECK ((width IS NULL OR width BETWEEN 1 AND 100000)
		AND (height IS NULL OR height BETWEEN 1 AND 100000)),
	CONSTRAINT meta_artwork_problem CHECK (problem IS NULL OR char_length(problem) BETWEEN 1 AND 300)
);

CREATE INDEX meta_artwork_hash ON meta_artwork (sha256) WHERE sha256 IS NOT NULL;
