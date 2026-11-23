-- 0074_definition_fetch.sql - indexer definitions an administrator fetches
-- from Jackett's repository: where an installed definition came from, the
-- file it replaced, and the list of what the repository had when it was last
-- asked
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Axis ships none of those files and fetches nothing by itself
-- (add-definition-fetch).  An administrator asks for the list (one commit of
-- the repository, kept here with each file's name, size and git blob id) and
-- then for the files they tick, each downloaded at that commit and installed
-- as a row of indexer_definition like a file installed by hand.
--
-- A fetched row says so: origin ('jackett'), the file's name there, the
-- commit it was read at, its git blob id, the SHA-256 of the file as fetched
-- and when.  A row whose sha256 differs from origin_sha256 was changed by
-- hand since (a file installed over it): it is not overwritten without a
-- tick.  The file a fetch replaced is kept whole in previous_body, with
-- where it came from, so that it can be put back once.
--
-- Safe on a live database: the columns are added without a default (no
-- rewrite), the check is over a table of at most a thousand small rows, and
-- the two tables are new.  An older daemon reads none of this.

ALTER TABLE indexer_definition
	ADD COLUMN origin          text,
	ADD COLUMN origin_file     text,
	ADD COLUMN origin_commit   text,
	ADD COLUMN origin_blob     text,
	ADD COLUMN origin_sha256   text,
	ADD COLUMN origin_at       timestamptz,
	ADD COLUMN previous_body   text,
	ADD COLUMN previous_name   text,
	ADD COLUMN previous_origin text,
	ADD COLUMN previous_commit text,
	ADD COLUMN previous_blob   text,
	ADD COLUMN previous_sha256 text,
	ADD COLUMN previous_at     timestamptz;

ALTER TABLE indexer_definition
	ADD CONSTRAINT indexer_definition_origin CHECK (
		(origin IS NULL AND origin_file IS NULL AND origin_commit IS NULL AND origin_blob IS NULL
			AND origin_sha256 IS NULL AND origin_at IS NULL)
		OR (origin = 'jackett'
			AND origin_file IS NOT NULL AND origin_commit IS NOT NULL
			AND origin_blob IS NOT NULL AND origin_sha256 IS NOT NULL
			AND origin_file ~ '^[a-z0-9][a-z0-9-]{0,63}\.yml$'
			AND origin_commit ~ '^[0-9a-f]{40}$'
			AND origin_blob ~ '^[0-9a-f]{40}$'
			AND origin_sha256 ~ '^[0-9a-f]{64}$'
			AND origin_at IS NOT NULL)),
	ADD CONSTRAINT indexer_definition_previous CHECK (
		(previous_body IS NULL AND previous_name IS NULL AND previous_origin IS NULL AND previous_commit IS NULL
			AND previous_blob IS NULL AND previous_sha256 IS NULL AND previous_at IS NULL)
		OR (previous_body IS NOT NULL AND previous_name IS NOT NULL
			AND octet_length(previous_body) BETWEEN 1 AND 262144
			AND char_length(previous_name) BETWEEN 1 AND 100
			AND ((previous_origin IS NULL AND previous_commit IS NULL AND previous_blob IS NULL
					AND previous_sha256 IS NULL AND previous_at IS NULL)
				OR (previous_origin = 'jackett'
					AND previous_commit IS NOT NULL AND previous_blob IS NOT NULL
					AND previous_sha256 IS NOT NULL
					AND previous_commit ~ '^[0-9a-f]{40}$'
					AND previous_blob ~ '^[0-9a-f]{40}$'
					AND previous_sha256 ~ '^[0-9a-f]{64}$'
					AND previous_at IS NOT NULL))));

-- What the repository had when an administrator last asked: one row for the
-- one place asked, replaced whole by the next asking.
CREATE TABLE definition_listing (
	origin    text        PRIMARY KEY,
	commit_id text        NOT NULL,
	listed_at timestamptz NOT NULL DEFAULT now(),
	n_left_out integer    NOT NULL DEFAULT 0,
	cut       boolean     NOT NULL DEFAULT false,
	CONSTRAINT definition_listing_form CHECK (
		origin = 'jackett' AND commit_id ~ '^[0-9a-f]{40}$' AND n_left_out >= 0)
);

CREATE TABLE definition_listing_file (
	origin     text   NOT NULL REFERENCES definition_listing (origin) ON DELETE CASCADE,
	name       text   NOT NULL,
	blob       text   NOT NULL,
	size_bytes bigint NOT NULL,
	PRIMARY KEY (origin, name),
	CONSTRAINT definition_listing_file_form CHECK (
		name ~ '^[a-z0-9][a-z0-9-]{0,63}\.yml$'
		AND blob ~ '^[0-9a-f]{40}$'
		AND size_bytes >= 0)
);
