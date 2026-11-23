-- 0035_indexer_native.sql - native indexers (Newznab, Torznab) beside the
-- sources of many (Prowlarr); what a search found at several indexers
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A native indexer is a row of indexer_source of kind 'newznab' (Usenet) or
-- 'torznab' (torrents): one indexer that Axis asks itself, with the user's
-- own key, at its own address and base path.  Its key is in
-- indexer_source_secret, as a source's is; it is sent in the query of the
-- requests to its own origin (the protocol's way) and never anywhere else.
-- A source (Prowlarr) keeps contributing the indexers configured in it.
--
-- What only a native indexer has:
--   protocol             what it serves (from its kind; NULL for a source)
--   movie_categories,    the Newznab categories it is searched in
--   tv_categories
--   files_origin         another origin ("https://host:port") an
--                        administrator accepted for its files, when its
--                        files are not on its own; files_origin_seen is one
--                        its last test saw, shown to accept or not.  Neither
--                        holds a path or a query, so neither holds a key
--   caps_*               what its t=caps said, as last read
--   api_*, grab_*        its daily allowances as it last reported them
--                        (<newznab:apilimits>), counted on between reports
--   held_until,          it said its request (or download) limit is reached:
--   grabs_held_until     not asked before then
-- Every indexer and source has a priority (lower first): of two that found
-- the same release, the one it is fetched from first.
--
-- The indexers a source reports gain host: the host the source says each is
-- at (Prowlarr's indexerUrls), so that one also configured natively is asked
-- natively and not through the source.
--
-- A stored release gains group_key, a SHA-256 of what makes it the same
-- release at another indexer (a torrent's info-hash; a Usenet release's
-- exact name and size): a fetch that fails at one is tried at the others
-- whose searches have not expired.  A release's secret row says whether its
-- fetch reference is on the indexer's accepted other origin.

ALTER TABLE indexer_source
	ADD COLUMN protocol            text,
	ADD COLUMN priority            integer   NOT NULL DEFAULT 25,
	ADD COLUMN movie_categories    integer[] NOT NULL DEFAULT '{2000}',
	ADD COLUMN tv_categories       integer[] NOT NULL DEFAULT '{5000}',
	ADD COLUMN files_origin        text,
	ADD COLUMN files_origin_seen   text,
	ADD COLUMN caps_at             timestamptz,
	ADD COLUMN caps_problem        text,
	ADD COLUMN caps_modes          smallint  NOT NULL DEFAULT 0,
	ADD COLUMN caps_movie_ids      smallint  NOT NULL DEFAULT 0,
	ADD COLUMN caps_tv_ids         smallint  NOT NULL DEFAULT 0,
	ADD COLUMN caps_movie_text     boolean   NOT NULL DEFAULT false,
	ADD COLUMN caps_tv_text        boolean   NOT NULL DEFAULT false,
	ADD COLUMN caps_tv_numbers     boolean   NOT NULL DEFAULT false,
	ADD COLUMN caps_limit_max      integer,
	ADD COLUMN caps_limit_default  integer,
	ADD COLUMN caps_categories     text[]    NOT NULL DEFAULT '{}',
	ADD COLUMN api_used            integer,
	ADD COLUMN api_max             integer,
	ADD COLUMN grab_used           integer,
	ADD COLUMN grab_max            integer,
	ADD COLUMN limits_at           timestamptz,
	ADD COLUMN held_until          timestamptz,
	ADD COLUMN grabs_held_until    timestamptz,
	ADD COLUMN hold_message        text;

ALTER TABLE indexer_source
	ADD CONSTRAINT indexer_source_native_protocol CHECK (
		(kind = 'newznab' AND protocol = 'usenet')
		OR (kind = 'torznab' AND protocol = 'torrent')
		OR (kind NOT IN ('newznab', 'torznab') AND protocol IS NULL)),
	ADD CONSTRAINT indexer_source_native_form CHECK (
		priority BETWEEN 1 AND 50
		AND cardinality(movie_categories) BETWEEN 0 AND 16
		AND cardinality(tv_categories) BETWEEN 0 AND 16
		AND (files_origin IS NULL OR (char_length(files_origin) BETWEEN 8 AND 255
		     AND files_origin ~ '^https?://[^/?#@\s]+$'))
		AND (files_origin_seen IS NULL OR (char_length(files_origin_seen) BETWEEN 8 AND 255
		     AND files_origin_seen ~ '^https?://[^/?#@\s]+$'))
		AND (caps_problem IS NULL OR (char_length(caps_problem) BETWEEN 1 AND 300
		     AND caps_problem !~ '[\x01-\x1f\x7f]'))
		AND caps_modes BETWEEN 0 AND 7 AND caps_movie_ids BETWEEN 0 AND 7 AND caps_tv_ids BETWEEN 0 AND 7
		AND (caps_limit_max IS NULL OR caps_limit_max BETWEEN 1 AND 100000)
		AND (caps_limit_default IS NULL OR caps_limit_default BETWEEN 1 AND 100000)
		AND cardinality(caps_categories) <= 100
		AND (api_used IS NULL OR api_used >= 0) AND (api_max IS NULL OR api_max >= 0)
		AND (grab_used IS NULL OR grab_used >= 0) AND (grab_max IS NULL OR grab_max >= 0)
		AND (hold_message IS NULL OR (char_length(hold_message) BETWEEN 1 AND 300
		     AND hold_message !~ '[\x01-\x1f\x7f]')));

ALTER TABLE indexer_source_indexer
	ADD COLUMN host text,
	ADD CONSTRAINT indexer_source_indexer_host CHECK (
		host IS NULL OR (char_length(host) BETWEEN 1 AND 253 AND host ~ '^[a-z0-9.:\[\]-]+$'));

ALTER TABLE indexer_release
	ADD COLUMN group_key text,
	ADD CONSTRAINT indexer_release_group_key CHECK (group_key IS NULL OR group_key ~ '^[0-9a-f]{64}$');

CREATE INDEX indexer_release_group ON indexer_release (group_key) WHERE group_key IS NOT NULL;

ALTER TABLE indexer_release_secret
	ADD COLUMN files_origin boolean NOT NULL DEFAULT false;
