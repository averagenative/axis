-- 0066_arr_source.sql - a connection to a person's Radarr or Sonarr, to bring
-- their library over (import-from-arr, slice R1)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Axis reads an arr app through its HTTP API with GET requests only, and
-- changes nothing there.  A connection is the app, a name, where it is, and
-- its API key:
--
--   origin       scheme, host and port ("http://radarr:7878").  Any host the
--                server can reach is allowed, private and loopback ones too:
--                only an administrator sets it.
--   path_prefix  the path the app is served under ("/radarr"), '' for none.
--   mappings     what the preview's tables and options were last set to (an
--                object: the app's quality profiles to Axis's, the app's
--                folders to library folders, the options).  Nothing in it is
--                a secret.
--   last_test    what "Test the connection" last found, in a fixed word;
--                version is the app's own, kept only when the answer named
--                the right app.
--
-- The key sits in a table of its own, as download_client_secret does: no
-- listing selects it and no page renders it.  It is kept as it is: the
-- database's own credentials sit beside it, so encrypting it with a key on
-- the same host would protect nothing.
--
-- New tables only: nothing existing is read, locked or rewritten.

CREATE TABLE arr_source (
	id               bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	app              text        NOT NULL,
	name             text        NOT NULL,
	origin           text        NOT NULL,
	path_prefix      text        NOT NULL DEFAULT '',
	accept_untrusted boolean     NOT NULL DEFAULT false,
	mappings         jsonb       NOT NULL DEFAULT '{}',
	made_by          text,
	last_test        text,
	version          text,
	tested_at        timestamptz,
	created_at       timestamptz NOT NULL DEFAULT now(),
	updated_at       timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT arr_source_name_unique UNIQUE (name),
	CONSTRAINT arr_source_app CHECK (app IN ('radarr', 'sonarr')),
	CONSTRAINT arr_source_name_form
		CHECK (char_length(name) BETWEEN 1 AND 64 AND name !~ '[\x01-\x1f\x7f]'),
	CONSTRAINT arr_source_origin_form
		CHECK (char_length(origin) BETWEEN 8 AND 255 AND origin ~ '^https?://[^/?#@\s]+$'),
	CONSTRAINT arr_source_prefix_form
		CHECK (path_prefix = '' OR (char_length(path_prefix) <= 128 AND path_prefix ~ '^(/[A-Za-z0-9._~-]+)+$'
		       AND path_prefix !~ '/\.\.?(/|$)')),
	CONSTRAINT arr_source_mappings_form
		CHECK (jsonb_typeof(mappings) = 'object' AND octet_length(mappings::text) <= 262144),
	CONSTRAINT arr_source_made_by CHECK (made_by IS NULL OR char_length(made_by) BETWEEN 1 AND 200),
	CONSTRAINT arr_source_last_test
		CHECK (last_test IS NULL OR last_test IN ('ok', 'refused', 'not_app', 'no_answer', 'certificate')),
	CONSTRAINT arr_source_version CHECK (version IS NULL OR version ~ '^[A-Za-z0-9.-]{1,32}$')
);

CREATE TABLE arr_source_secret (
	source_id bigint      PRIMARY KEY REFERENCES arr_source (id) ON DELETE CASCADE,
	secret    text        NOT NULL,
	set_at    timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT arr_source_secret_form CHECK (char_length(secret) BETWEEN 1 AND 1024)
);
