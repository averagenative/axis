-- 0021_indexers.sql - the indexer sources Axis searches, the indexers they
-- report, and the searches it made (kept briefly) with their releases
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A source (Prowlarr, today) is configured by an administrator: its kind, its
-- address (an origin) and the base path it is served under, as a download
-- client's is.  Its key is in a table of its own, indexer_source_secret,
-- which only the code that connects to the source reads: a list of sources
-- never loads it.  It is stored as it is, as secret settings and download
-- client secrets are.
--
-- A search is stored with the releases it found for fifteen minutes (a
-- setting; expires_at), keyed by the normalised request (a SHA-256 of it: the
-- words that were searched for are not kept), so that the page that shows
-- releases and the job that picks one see the same list and a grab refers to
-- a release by its id.  A release's fetch reference (an address on the
-- source's own origin that embeds the source's key as a parameter) and its
-- magnet link (which may carry a tracker's passkey) are in
-- indexer_release_secret, never selected by a listing, never rendered, and
-- deleted with the release when its search expires.

CREATE TABLE indexer_source (
	id               bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	name             text        NOT NULL,
	kind             text        NOT NULL,
	address          text        NOT NULL,
	base_path        text        NOT NULL DEFAULT '',
	accept_untrusted boolean     NOT NULL DEFAULT false,
	enabled          boolean     NOT NULL DEFAULT true,
	-- What the last contact found.
	state            text        NOT NULL DEFAULT 'unchecked',
	state_message    text,
	version          text,
	last_checked_at  timestamptz,
	last_contact_at  timestamptz,
	-- When the list of its indexers was last read.
	indexers_at      timestamptz,
	created_at       timestamptz NOT NULL DEFAULT now(),
	updated_at       timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT indexer_source_name_unique UNIQUE (name),
	CONSTRAINT indexer_source_name_form
		CHECK (char_length(name) BETWEEN 1 AND 64 AND name !~ '[\x01-\x1f\x7f]'),
	CONSTRAINT indexer_source_kind_form CHECK (kind ~ '^[a-z][a-z0-9]{0,15}$'),
	CONSTRAINT indexer_source_address_form
		CHECK (char_length(address) BETWEEN 8 AND 255 AND address ~ '^https?://[^/?#@\s]+$'),
	CONSTRAINT indexer_source_base_path_form CHECK (
		base_path = ''
		OR (char_length(base_path) <= 128
		    AND base_path ~ '^(/[A-Za-z0-9._~-]+)+$'
		    AND base_path !~ '(^|/)\.\.?(/|$)')),
	CONSTRAINT indexer_source_state
		CHECK (state IN ('unchecked', 'ok', 'unreachable', 'refused', 'failing')),
	CONSTRAINT indexer_source_texts CHECK (
		(state_message IS NULL OR char_length(state_message) BETWEEN 1 AND 300)
		AND (version IS NULL OR (char_length(version) BETWEEN 1 AND 64 AND version !~ '[\x01-\x1f\x7f]')))
);

CREATE TABLE indexer_source_secret (
	source_id bigint      PRIMARY KEY REFERENCES indexer_source (id) ON DELETE CASCADE,
	secret    text        NOT NULL,
	set_at    timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT indexer_source_secret_form CHECK (char_length(secret) BETWEEN 1 AND 1024)
);

-- The indexers a source reports, as of the last time it was asked.  Their
-- names come from the source and from whoever configured it: shown as text.
CREATE TABLE indexer_source_indexer (
	source_id  bigint   NOT NULL REFERENCES indexer_source (id) ON DELETE CASCADE,
	indexer_id bigint   NOT NULL,
	name       text     NOT NULL,
	protocol   text     NOT NULL,
	enabled    boolean  NOT NULL,
	failing    boolean  NOT NULL DEFAULT false,
	-- Which identifiers it takes for a film and for an episode (bit 1: IMDb,
	-- 2: TMDb, 4: TVDB), as it said.
	movie_ids  smallint NOT NULL DEFAULT 0,
	tv_ids     smallint NOT NULL DEFAULT 0,

	PRIMARY KEY (source_id, indexer_id),
	CONSTRAINT indexer_source_indexer_form CHECK (
		indexer_id >= 0
		AND char_length(name) BETWEEN 1 AND 100 AND name !~ '[\x01-\x1f\x7f]'
		AND protocol IN ('usenet', 'torrent')
		AND movie_ids BETWEEN 0 AND 7 AND tv_ids BETWEEN 0 AND 7)
);

CREATE TABLE indexer_search (
	id                 bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	source_id          bigint      NOT NULL REFERENCES indexer_source (id) ON DELETE CASCADE,
	-- SHA-256 of the normalised request, in hex.
	request_key        text        NOT NULL,
	-- What was searched with: 'text', or the identifier ('imdb', 'tmdb',
	-- 'tvdb'); fell_back says an identifier search found nothing and a text
	-- search was made instead (mode is then 'text').
	mode               text        NOT NULL,
	fell_back          boolean     NOT NULL DEFAULT false,
	n_releases         integer     NOT NULL,
	-- Results left out: a fetch address that is not the source's own, no way
	-- to fetch, not readable; and whether the list was cut at its limit.
	n_off_origin       integer     NOT NULL DEFAULT 0,
	n_unusable         integer     NOT NULL DEFAULT 0,
	n_skipped          integer     NOT NULL DEFAULT 0,
	truncated          boolean     NOT NULL DEFAULT false,
	-- Indexers of the source that were on, and the names of those it was
	-- failing (as far as it says) when this was searched.
	n_indexers         integer,
	failed_names       text[]      NOT NULL DEFAULT '{}',
	created_at         timestamptz NOT NULL DEFAULT now(),
	expires_at         timestamptz NOT NULL,

	CONSTRAINT indexer_search_key_unique UNIQUE (source_id, request_key),
	CONSTRAINT indexer_search_form CHECK (
		request_key ~ '^[0-9a-f]{64}$'
		AND mode IN ('text', 'imdb', 'tmdb', 'tvdb')
		AND n_releases BETWEEN 0 AND 1000
		AND n_off_origin >= 0 AND n_unusable >= 0 AND n_skipped >= 0
		AND (n_indexers IS NULL OR n_indexers >= 0)
		AND cardinality(failed_names) <= 50)
);

CREATE INDEX indexer_search_expiry ON indexer_search (expires_at);

CREATE TABLE indexer_release (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	search_id    bigint      NOT NULL REFERENCES indexer_search (id) ON DELETE CASCADE,
	position     integer     NOT NULL,
	name         text        NOT NULL,
	size_bytes   bigint,
	protocol     text        NOT NULL,
	indexer_id   bigint      NOT NULL DEFAULT 0,
	indexer_name text,
	published_at timestamptz,
	seeders      integer,
	leechers     integer,
	grabs        integer,
	categories   integer[]   NOT NULL DEFAULT '{}',
	flags        text[]      NOT NULL DEFAULT '{}',
	imdb_id      text,
	tmdb_id      bigint,
	tvdb_id      bigint,
	info_hash    text,

	CONSTRAINT indexer_release_position UNIQUE (search_id, position),
	CONSTRAINT indexer_release_texts CHECK (
		char_length(name) BETWEEN 1 AND 300 AND name !~ '[\x01-\x1f\x7f]'
		AND (indexer_name IS NULL OR (char_length(indexer_name) BETWEEN 1 AND 100 AND indexer_name !~ '[\x01-\x1f\x7f]'))
		AND protocol IN ('usenet', 'torrent')
		AND (imdb_id IS NULL OR imdb_id ~ '^tt[0-9]{7,10}$')
		AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
		AND cardinality(categories) <= 16 AND cardinality(flags) <= 8),
	CONSTRAINT indexer_release_figures CHECK (
		(size_bytes IS NULL OR size_bytes >= 0) AND (seeders IS NULL OR seeders >= 0)
		AND (leechers IS NULL OR leechers >= 0) AND (grabs IS NULL OR grabs >= 0)
		AND indexer_id >= 0)
);

-- How a release is fetched: its address on the source's own origin (path and
-- query, which hold the source's key) and its magnet link.  Read only to
-- fetch the release; deleted with it.
CREATE TABLE indexer_release_secret (
	release_id bigint PRIMARY KEY REFERENCES indexer_release (id) ON DELETE CASCADE,
	fetch_ref  text,
	magnet     text,

	CONSTRAINT indexer_release_secret_form CHECK (
		(fetch_ref IS NOT NULL OR magnet IS NOT NULL)
		AND (fetch_ref IS NULL OR (char_length(fetch_ref) BETWEEN 2 AND 2048 AND fetch_ref ~ '^/'))
		AND (magnet IS NULL OR char_length(magnet) BETWEEN 20 AND 4096))
);
