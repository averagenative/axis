-- 0013_compat_server_kinds.sql - more than one kind of media server at once
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Migration 0012 gave every row that comes from a media server a
-- server_kind (default 'plex') but keyed nothing by it, and kept one state
-- row for one server.  Jellyfin is a second kind, connected at the same
-- time, so what a server calls its ids (a library's key, a device's id)
-- only means something together with the kind:
--
--  - compat_state: one row per kind (the Plex row keeps id 1; another
--    kind's row is made when its connection is first used);
--  - compat_library, compat_device, compat_play, compat_session,
--    compat_path_map: the kind is part of the key (compat_device had no
--    server_kind column; it gets one, 'plex' for what is there), and the
--    foreign keys to compat_device say which kind;
--  - compat_evidence: a stream's evidence may now name the property that
--    failed (the container, a profile, a bit depth, a level or a subtitle
--    format) when the server states that cause; the codec's own evidence is
--    failed_property = '' as before.  A session then has at most one row per
--    kind of stream and property.
--
-- Every row there is is a Plex row and stays what it was: nothing is
-- deleted, nothing is duplicated, only constraints are widened.

ALTER TABLE compat_state DROP CONSTRAINT compat_state_single;
ALTER TABLE compat_state ADD CONSTRAINT compat_state_id CHECK (id >= 1);
ALTER TABLE compat_state ADD CONSTRAINT compat_state_kind_unique UNIQUE (server_kind);
ALTER TABLE compat_state ADD CONSTRAINT compat_state_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$');

ALTER TABLE compat_library DROP CONSTRAINT compat_library_pkey;
ALTER TABLE compat_library ADD CONSTRAINT compat_library_pkey PRIMARY KEY (server_kind, key);
ALTER TABLE compat_library ADD CONSTRAINT compat_library_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$');

ALTER TABLE compat_path_map DROP CONSTRAINT compat_path_map_unique;
ALTER TABLE compat_path_map ADD CONSTRAINT compat_path_map_unique UNIQUE (server_kind, server_path);
ALTER TABLE compat_path_map ADD CONSTRAINT compat_path_map_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$');

-- The devices, and the tables that point at them.
ALTER TABLE compat_play DROP CONSTRAINT compat_play_device_id_fkey;
ALTER TABLE compat_session DROP CONSTRAINT compat_session_device_id_fkey;
ALTER TABLE compat_device DROP CONSTRAINT compat_device_pkey;
ALTER TABLE compat_device ADD COLUMN server_kind text NOT NULL DEFAULT 'plex';
ALTER TABLE compat_device ADD CONSTRAINT compat_device_pkey PRIMARY KEY (server_kind, client_id);
ALTER TABLE compat_device ADD CONSTRAINT compat_device_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$');

ALTER TABLE compat_play DROP CONSTRAINT compat_play_pkey;
ALTER TABLE compat_play ADD CONSTRAINT compat_play_pkey
	PRIMARY KEY (server_kind, device_id, library_key, rating_key, viewed_at, account_id);
ALTER TABLE compat_play ADD CONSTRAINT compat_play_device_fkey
	FOREIGN KEY (server_kind, device_id) REFERENCES compat_device (server_kind, client_id) ON DELETE CASCADE;
ALTER TABLE compat_play ADD CONSTRAINT compat_play_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$');

ALTER TABLE compat_session DROP CONSTRAINT compat_session_unique;
ALTER TABLE compat_session ADD CONSTRAINT compat_session_unique UNIQUE (server_kind, server_id, session_key, rating_key);
ALTER TABLE compat_session ADD CONSTRAINT compat_session_device_fkey
	FOREIGN KEY (server_kind, device_id) REFERENCES compat_device (server_kind, client_id) ON DELETE CASCADE;
ALTER TABLE compat_session ADD CONSTRAINT compat_session_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$');

-- Evidence names the property that failed, when the server says so.  The
-- container column then holds the container that was not taken.
ALTER TABLE compat_evidence ADD COLUMN failed_property text NOT NULL DEFAULT '';
ALTER TABLE compat_evidence DROP CONSTRAINT compat_evidence_pkey;
ALTER TABLE compat_evidence ADD CONSTRAINT compat_evidence_pkey PRIMARY KEY (session_id, kind, failed_property);
ALTER TABLE compat_evidence DROP CONSTRAINT compat_evidence_form;
ALTER TABLE compat_evidence ADD CONSTRAINT compat_evidence_form CHECK (kind BETWEEN 1 AND 3 AND hdr BETWEEN -1 AND 1
	AND effect IN ('unknown', 'direct', 'passthrough', 'converted_codec', 'converted_limit',
		'converted_unclear')
	AND codec ~ '^[a-z0-9_.+-]{1,32}$'
	AND (profile IS NULL OR char_length(profile) <= 64)
	AND bit_depth BETWEEN 0 AND 64 AND channels BETWEEN 0 AND 256
	AND (res_class IS NULL OR res_class IN ('sd', '720p', '1080p', '2160p', 'other'))
	AND (container IS NULL OR container ~ '^[a-z0-9_.+-]{1,16}$')
	AND (out_codec IS NULL OR out_codec ~ '^[a-z0-9_.+-]{1,32}$')
	AND failed_property IN ('', 'container', 'profile', 'bit_depth', 'level', 'subtitle_format')
	-- A property is named only by a conversion the server blames on it.
	AND (failed_property = '' OR effect = 'converted_codec')
	-- The container is evidence for a play with no conversion at all, and
	-- the container that was not taken for a container failure.
	AND (container IS NULL OR effect = 'direct' OR failed_property = 'container'));
