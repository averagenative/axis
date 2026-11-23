-- 0019_downloads.sql - the download clients Axis drives, and the record of
-- every download it started
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A download client (SABnzbd, Deluge, ...) is configured by an
-- administrator: its kind, its address, the category or label Axis gives
-- its downloads, the priority they get, and a mapping from the client's
-- paths to Axis's volumes.  Its secret (an API key or a password) is in a
-- table of its own, download_client_secret, which only the code that
-- connects to the client reads: a page or a list of clients never loads
-- it.  It is stored as it is, as secret settings are (the database
-- credentials are beside it on the same host; see the settings change).
--
-- A download is recorded before the release is sent ('sending', with no
-- identifier of the client's yet) and given the client's identifier
-- once the client has accepted it.  Axis lists, changes and removes only
-- downloads recorded here: anything else in the client is never touched.
-- A completed download's location is the client's path mapped onto a
-- volume (volume_id, rel_path), recorded only once the volume guard has
-- found it beneath that volume.

CREATE TABLE download_client (
	id               bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	name             text        NOT NULL,
	kind             text        NOT NULL,
	address          text        NOT NULL,
	accept_untrusted boolean     NOT NULL DEFAULT false,
	category         text        NOT NULL DEFAULT 'axis',
	priority         text        NOT NULL DEFAULT 'normal',
	enabled          boolean     NOT NULL DEFAULT true,
	-- What the last contact found.
	state            text        NOT NULL DEFAULT 'unchecked',
	state_message    text,
	version          text,
	category_exists  boolean,
	last_checked_at  timestamptz,
	last_contact_at  timestamptz,
	-- A problem with its downloads (a completed path that maps nowhere).
	problem          text,
	created_at       timestamptz NOT NULL DEFAULT now(),
	updated_at       timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT download_client_name_unique UNIQUE (name),
	CONSTRAINT download_client_name_form
		CHECK (char_length(name) BETWEEN 1 AND 64 AND name !~ '[\x01-\x1f\x7f]'),
	CONSTRAINT download_client_kind_form CHECK (kind ~ '^[a-z][a-z0-9]{0,15}$'),
	CONSTRAINT download_client_address_form
		CHECK (char_length(address) BETWEEN 8 AND 255 AND address ~ '^https?://[^/?#@\s]+$'),
	CONSTRAINT download_client_category_form CHECK (category ~ '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$'),
	CONSTRAINT download_client_priority CHECK (priority IN ('low', 'normal', 'high')),
	CONSTRAINT download_client_state
		CHECK (state IN ('unchecked', 'ok', 'unreachable', 'refused', 'failing')),
	CONSTRAINT download_client_texts CHECK (
		(state_message IS NULL OR char_length(state_message) BETWEEN 1 AND 300)
		AND (problem IS NULL OR char_length(problem) BETWEEN 1 AND 300)
		AND (version IS NULL OR (char_length(version) BETWEEN 1 AND 64 AND version !~ '[\x01-\x1f\x7f]')))
);

CREATE TABLE download_client_secret (
	client_id bigint      PRIMARY KEY REFERENCES download_client (id) ON DELETE CASCADE,
	secret    text        NOT NULL,
	set_at    timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT download_client_secret_form CHECK (char_length(secret) BETWEEN 1 AND 1024)
);

-- Where the client's paths are for Axis: a path beneath client_prefix is
-- the same path beneath rel_path on the volume.  The longest prefix that
-- contains a path is the one used.
CREATE TABLE download_path_map (
	client_id     bigint NOT NULL REFERENCES download_client (id) ON DELETE CASCADE,
	client_prefix text   NOT NULL,
	volume_id     bigint NOT NULL REFERENCES volume (id) ON DELETE CASCADE,
	rel_path      text   NOT NULL DEFAULT '',

	PRIMARY KEY (client_id, client_prefix),
	CONSTRAINT download_path_map_prefix_form
		CHECK (char_length(client_prefix) BETWEEN 2 AND 1024 AND client_prefix ~ '^/' AND client_prefix !~ '/$'
		       AND client_prefix !~ '[\x01-\x1f\x7f]'),
	CONSTRAINT download_path_map_rel_form
		CHECK (char_length(rel_path) <= 1024 AND rel_path !~ '^/' AND rel_path !~ '/$'
		       AND rel_path !~ '[\x01-\x1f\x7f]')
);

CREATE TABLE download (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	-- The client, and its name for pages once the client is removed.
	client_id     bigint      REFERENCES download_client (id) ON DELETE SET NULL,
	client_name   text        NOT NULL,
	-- The client's own identifier (SABnzbd's nzo_id, Deluge's hash); NULL
	-- while it is being sent, and when sending failed.
	client_ref    text,
	info_hash     text,
	protocol      text        NOT NULL,
	source        text        NOT NULL,
	release_name  text        NOT NULL,
	-- What it is for.
	title_id      bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	title_label   text,
	module        text,
	state         text        NOT NULL DEFAULT 'sending',
	size_bytes    bigint,
	done_bytes    bigint,
	rate_bps      bigint,
	eta_s         bigint,
	-- Where the client says its files are, and that place on a volume.
	client_path   text,
	volume_id     bigint      REFERENCES volume (id) ON DELETE SET NULL,
	rel_path      text,
	-- The client's own words (why it failed), and Axis's (why it waits).
	message       text,
	problem       text,
	stalled       boolean     NOT NULL DEFAULT false,
	-- Successful polls in a row that did not list it.
	missing_polls integer     NOT NULL DEFAULT 0,
	-- When its bytes last grew, and when the client last listed it.
	progress_at   timestamptz,
	seen_at       timestamptz,
	created_at    timestamptz NOT NULL DEFAULT now(),
	sent_at       timestamptz,
	updated_at    timestamptz NOT NULL DEFAULT now(),
	completed_at  timestamptz,
	finished_at   timestamptz,

	CONSTRAINT download_protocol CHECK (protocol IN ('usenet', 'torrent')),
	CONSTRAINT download_source CHECK (source IN ('file', 'magnet')),
	CONSTRAINT download_state CHECK (state IN ('sending', 'queued', 'downloading', 'paused', 'completed',
		'failed', 'gone', 'removed', 'imported')),
	CONSTRAINT download_ref_known CHECK (client_ref IS NOT NULL OR state IN ('sending', 'failed')),
	CONSTRAINT download_completed_located CHECK (state <> 'completed' OR rel_path IS NOT NULL),
	CONSTRAINT download_client_name_form
		CHECK (char_length(client_name) BETWEEN 1 AND 64 AND client_name !~ '[\x01-\x1f\x7f]'),
	CONSTRAINT download_ref_form
		CHECK (client_ref IS NULL OR (char_length(client_ref) BETWEEN 1 AND 255 AND client_ref !~ '[\x01-\x1f\x7f]')),
	CONSTRAINT download_hash_form CHECK (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$'),
	CONSTRAINT download_texts CHECK (
		char_length(release_name) BETWEEN 1 AND 300 AND release_name !~ '[\x01-\x1f\x7f]'
		AND (title_label IS NULL OR char_length(title_label) BETWEEN 1 AND 300)
		AND (module IS NULL OR module ~ '^[a-z][a-z0-9-]{0,31}$')
		AND (message IS NULL OR char_length(message) BETWEEN 1 AND 300)
		AND (problem IS NULL OR char_length(problem) BETWEEN 1 AND 300)
		AND (client_path IS NULL OR (char_length(client_path) BETWEEN 1 AND 1024 AND client_path !~ '[\x01-\x1f\x7f]'))
		AND (rel_path IS NULL OR (char_length(rel_path) BETWEEN 1 AND 1024 AND rel_path !~ '^/'))),
	CONSTRAINT download_figures CHECK ((size_bytes IS NULL OR size_bytes >= 0) AND (done_bytes IS NULL OR done_bytes >= 0)
		AND (rate_bps IS NULL OR rate_bps >= 0) AND (eta_s IS NULL OR eta_s >= 0)
		AND missing_polls >= 0)
);

-- One live record per client identifier.  A finished one (removed, gone,
-- failed, imported) keeps its identifier for the history, and the same
-- release may be sent again later.
CREATE UNIQUE INDEX download_client_ref_live ON download (client_id, client_ref)
	WHERE client_ref IS NOT NULL AND state IN ('queued', 'downloading', 'paused', 'completed');

CREATE INDEX download_active ON download (client_id)
	WHERE state IN ('sending', 'queued', 'downloading', 'paused');

CREATE INDEX download_recent ON download (updated_at DESC);
