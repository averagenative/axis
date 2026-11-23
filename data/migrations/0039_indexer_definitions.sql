-- 0039_indexer_definitions.sql - indexer definitions (the community's
-- "Cardigann" YAML files) installed by an administrator, and indexers that
-- run one
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A definition is the administrator's data: Axis ships none.  Each installed
-- one is a row of indexer_definition, its file kept whole (at most 256 KiB,
-- valid UTF-8: the YAML reader refuses anything else before it is stored),
-- one row for each of the definitions' own ids; installing a file with an id
-- already installed replaces it.  Whether a definition can be used is not
-- stored: it is judged again whenever it is read, by the engine of the Axis
-- that reads it.
--
-- An indexer that runs a definition is a row of indexer_source of kind
-- 'cardigann' (torrents, no key) naming its definition; the definition cannot
-- be removed while an indexer runs it.

CREATE TABLE indexer_definition (
	id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	def_id       text        NOT NULL UNIQUE,
	name         text        NOT NULL,
	body         text        NOT NULL,
	sha256       text        NOT NULL,
	installed_at timestamptz NOT NULL DEFAULT now(),
	updated_at   timestamptz NOT NULL DEFAULT now(),
	CONSTRAINT indexer_definition_form CHECK (
		def_id ~ '^[A-Za-z0-9._-]{1,64}$'
		AND char_length(name) BETWEEN 1 AND 100 AND name !~ '[\x01-\x1f\x7f]'
		AND octet_length(body) BETWEEN 1 AND 262144
		AND sha256 ~ '^[0-9a-f]{64}$')
);

ALTER TABLE indexer_source
	ADD COLUMN definition_id bigint REFERENCES indexer_definition (id) ON DELETE RESTRICT;

CREATE INDEX indexer_source_definition_idx ON indexer_source (definition_id) WHERE definition_id IS NOT NULL;

ALTER TABLE indexer_source DROP CONSTRAINT indexer_source_native_protocol;

ALTER TABLE indexer_source
	ADD CONSTRAINT indexer_source_native_protocol CHECK (
		(kind = 'newznab' AND protocol = 'usenet')
		OR (kind IN ('torznab', 'yts', 'eztv', 'piratebay', 'cardigann') AND protocol = 'torrent')
		OR (kind NOT IN ('newznab', 'torznab', 'yts', 'eztv', 'piratebay', 'cardigann') AND protocol IS NULL)),
	ADD CONSTRAINT indexer_source_definition CHECK ((kind = 'cardigann') = (definition_id IS NOT NULL));
