-- 0012_client_compatibility.sql - the media server connection, the devices
-- that play, the sessions observed, and the path map
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The client compatibility report (src/compat/) learns what the household's
-- players can play directly from a media server (Plex first), read-only.
-- Originals are deleted after conversion, so what is stored here decides
-- what the daemon dares to call safe, and the rule is that unknown is never
-- safe:
--
--  - compat_session / compat_evidence: one row per play *observed while it
--    played* (the server's session id and the item are the key, so a session
--    seen in many polls, or across a restart, is one row), and per stream the
--    effect the server's decision had: direct, passthrough, converted for a
--    codec the device did not take, converted for a bandwidth or quality
--    limit, converted for an unclear cause, or unknown.  This is the only
--    source of direct-play and conversion evidence: the server's play history
--    records no playback decision.
--  - compat_play: the imported history, which says only which device played
--    which library and when.  It never becomes evidence.
--  - compat_device: devices seen (by the id sessions use), with a name and
--    platform for administrators and a "pinned" flag (always consider).
--  - compat_library: the server's libraries as last fetched, with their
--    folders; compat_path_map: the administrator's map from the server's
--    paths to Axis's.  The mapping of a library to roots is worked out from
--    these two and the roots when it is needed; it is never stored, so it
--    can never be stale.
--  - compat_state: one row: which server the records belong to, when
--    collection started, and how the last polls went.
--
-- Privacy: the server's user names are not stored unless an administrator
-- turns that on (compat.store_account_names); an opaque account id is.  A
-- session holds no file path, only a reference to the Axis file it was
-- matched to.  Rows older than the retention period are removed.

-- Server-neutral: every row that comes from a media server says which kind
-- (server_kind, 'plex' today; a later change adds others).  Decisions are
-- stored as generic effects (direct, passthrough, converted_*, unknown),
-- never as a server's own words, and compat_evidence.stated_reason holds a
-- reason a server states for a conversion (Plex states none, so NULL; the
-- inferred effect stays beside it).
CREATE TABLE compat_state (
	id                integer     PRIMARY KEY DEFAULT 1,
	server_kind       text        NOT NULL DEFAULT 'plex',
	server_id         text,
	server_name       text,
	server_version    text,
	-- When collection started: the connection was first used.
	started_at        timestamptz,
	last_poll_at      timestamptz,
	last_poll_ok_at   timestamptz,
	last_error        text,
	failures          integer     NOT NULL DEFAULT 0,
	-- Do not poll before this; the interval adapts (idle backs off).
	next_poll_at      timestamptz,
	idle_polls        integer     NOT NULL DEFAULT 0,
	playing_now       integer     NOT NULL DEFAULT 0,
	history_at        timestamptz,
	history_error     text,
	libraries_at      timestamptz,

	CONSTRAINT compat_state_single CHECK (id = 1),
	CONSTRAINT compat_state_sane CHECK (failures >= 0 AND idle_polls >= 0 AND playing_now >= 0),
	CONSTRAINT compat_state_text CHECK (
		(last_error IS NULL OR (char_length(last_error) <= 600 AND last_error !~ '[\x01-\x1f\x7f]'))
		AND (history_error IS NULL OR (char_length(history_error) <= 600 AND history_error !~ '[\x01-\x1f\x7f]'))
		AND (server_id IS NULL OR char_length(server_id) <= 128)
		AND (server_name IS NULL OR char_length(server_name) <= 128)
		AND (server_version IS NULL OR char_length(server_version) <= 64))
);

INSERT INTO compat_state (id) VALUES (1);

CREATE TABLE compat_library (
	server_kind text      NOT NULL DEFAULT 'plex',
	key       text        PRIMARY KEY,
	kind      text        NOT NULL,
	title     text,
	folders   text[]      NOT NULL DEFAULT '{}',
	seen_at   timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT compat_library_form CHECK (key ~ '^[A-Za-z0-9_.:-]{1,128}$'
		AND kind IN ('movie', 'show', 'artist', 'other')
		AND (title IS NULL OR char_length(title) <= 128)
		AND cardinality(folders) <= 32)
);

CREATE TABLE compat_path_map (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	server_kind text        NOT NULL DEFAULT 'plex',
	server_path text        NOT NULL,
	axis_path   text        NOT NULL,
	created_at  timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT compat_path_map_unique UNIQUE (server_path),
	CONSTRAINT compat_path_map_form CHECK (server_path ~ '^/' AND axis_path ~ '^/'
		AND octet_length(server_path) <= 1024 AND octet_length(axis_path) <= 1024
		AND server_path !~ '[\x01-\x1f\x7f]' AND axis_path !~ '[\x01-\x1f\x7f]')
);

CREATE TABLE compat_device (
	client_id   text        PRIMARY KEY,
	plex_id     bigint,
	name        text,
	platform    text,
	product     text,
	pinned      boolean     NOT NULL DEFAULT false,
	first_seen  timestamptz NOT NULL DEFAULT now(),
	last_seen   timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT compat_device_form CHECK (client_id ~ '^[A-Za-z0-9_.:-]{1,128}$'
		AND (name IS NULL OR char_length(name) <= 128)
		AND (platform IS NULL OR char_length(platform) <= 64)
		AND (product IS NULL OR char_length(product) <= 64))
);

CREATE INDEX compat_device_plex_idx ON compat_device (plex_id) WHERE plex_id IS NOT NULL;

-- The imported history: who played what where and when, nothing about
-- how it played.
CREATE TABLE compat_play (
	server_kind text        NOT NULL DEFAULT 'plex',
	device_id   text        NOT NULL REFERENCES compat_device (client_id) ON DELETE CASCADE,
	library_key text        NOT NULL,
	rating_key  text        NOT NULL,
	viewed_at   timestamptz NOT NULL,
	account_id  text        NOT NULL DEFAULT '',

	PRIMARY KEY (device_id, library_key, rating_key, viewed_at, account_id),
	CONSTRAINT compat_play_form CHECK (library_key ~ '^[A-Za-z0-9_.:-]{1,128}$'
		AND rating_key ~ '^[A-Za-z0-9_.:-]{1,128}$' AND char_length(account_id) <= 128)
);

CREATE INDEX compat_play_library_idx ON compat_play (library_key, viewed_at);
CREATE INDEX compat_play_viewed_idx ON compat_play (viewed_at);

CREATE TABLE compat_session (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	server_kind   text        NOT NULL DEFAULT 'plex',
	server_id     text        NOT NULL,
	session_key   text        NOT NULL,
	rating_key    text        NOT NULL,
	device_id     text        NOT NULL REFERENCES compat_device (client_id) ON DELETE CASCADE,
	library_key   text,
	item_type     text,
	first_seen    timestamptz NOT NULL DEFAULT now(),
	last_seen     timestamptz NOT NULL DEFAULT now(),
	polls         integer     NOT NULL DEFAULT 1,
	-- Not known to be on the local network.
	remote        boolean     NOT NULL,
	bandwidth_kbps integer,
	-- The server's opaque account id; the name only when the setting says so.
	account_id    text,
	account_name  text,
	-- The Axis file it was matched to through the path map, if any.  An
	-- unmatched session still counts as evidence about the device.
	file_id       bigint      REFERENCES library_file (id) ON DELETE SET NULL,

	CONSTRAINT compat_session_unique UNIQUE (server_id, session_key, rating_key),
	CONSTRAINT compat_session_form CHECK (session_key ~ '^[A-Za-z0-9_.:-]{1,128}$'
		AND rating_key ~ '^[A-Za-z0-9_.:-]{1,128}$' AND polls >= 1
		AND (bandwidth_kbps IS NULL OR bandwidth_kbps >= 0)
		AND (account_id IS NULL OR char_length(account_id) <= 128)
		AND (account_name IS NULL OR char_length(account_name) <= 128))
);

CREATE INDEX compat_session_device_idx ON compat_session (device_id, last_seen);
CREATE INDEX compat_session_library_idx ON compat_session (library_key, last_seen);
CREATE INDEX compat_session_seen_idx ON compat_session (last_seen);
CREATE INDEX compat_session_file_idx ON compat_session (file_id) WHERE file_id IS NOT NULL;

-- One row per kind of stream (1 video, 2 audio, 3 subtitle) of a session.
CREATE TABLE compat_evidence (
	session_id  bigint   NOT NULL REFERENCES compat_session (id) ON DELETE CASCADE,
	kind        smallint NOT NULL,
	codec       text     NOT NULL,
	profile     text,
	bit_depth   integer  NOT NULL DEFAULT 0,
	res_class   text,
	hdr         smallint NOT NULL DEFAULT -1,
	channels    integer  NOT NULL DEFAULT 0,
	container   text,
	effect      text     NOT NULL,
	out_codec   text,
	-- A reason the server itself states for a conversion (NULL for Plex).
	stated_reason text,

	PRIMARY KEY (session_id, kind),
	CONSTRAINT compat_evidence_reason CHECK (stated_reason IS NULL OR char_length(stated_reason) <= 64),
	CONSTRAINT compat_evidence_form CHECK (kind BETWEEN 1 AND 3 AND hdr BETWEEN -1 AND 1
		AND effect IN ('unknown', 'direct', 'passthrough', 'converted_codec', 'converted_limit',
			'converted_unclear')
		AND codec ~ '^[a-z0-9_.+-]{1,32}$'
		AND (profile IS NULL OR char_length(profile) <= 64)
		AND bit_depth BETWEEN 0 AND 64 AND channels BETWEEN 0 AND 256
		AND (res_class IS NULL OR res_class IN ('sd', '720p', '1080p', '2160p', 'other'))
		AND (container IS NULL OR container ~ '^[a-z0-9_.+-]{1,16}$')
		AND (out_codec IS NULL OR out_codec ~ '^[a-z0-9_.+-]{1,32}$')
		-- The container is evidence only for a play with no conversion.
		AND (container IS NULL OR effect = 'direct'))
);
