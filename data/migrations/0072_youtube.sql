-- 0072_youtube.sql - the YouTube downloader for one video (add-youtube-downloader,
-- slice Y1 of scope-youtube-source)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Axis runs yt-dlp itself, through a built-in download client, and saves a
-- video into a library root of a kind of its own.
--
--   youtube_state  one row: whether YouTube is switched on, by whom and when,
--                  and until when YouTube work is paused after YouTube refused
--                  this server (too many requests, a bot check).  Off until an
--                  administrator switches it on.
--   youtube_video  one row for each YouTube video id Axis was given: what
--                  yt-dlp said of it (title, channel, upload date, duration,
--                  the formats chosen), its state, its progress while it is
--                  fetched, and where its file went.  The video id is the key,
--                  so a video is fetched once.
--
--   download.protocol        gains 'youtube'.
--   download.source          gains 'address' (a video's address, where a
--                            Usenet or torrent release is a file or a magnet
--                            link).
--   download_client.address  may be empty: the built-in client has no address.
--                            A client an administrator adds always has one
--                            (the code that saves it checks that, as before).
--   library_root.media_type  gains 'youtube'.
--
-- Safe on a live database: the two tables are new.  Each CHECK that changes
-- is dropped and added again NOT VALID, then validated, which reads its table
-- once without blocking reads or writes of it for the read (download holds a
-- few thousand rows, download_client and library_root a handful).  Each new
-- CHECK allows everything the old one did, so no row can fail it.  The
-- migration runs in one transaction: a write to one of those tables waits a
-- moment for its commit.

ALTER TABLE download DROP CONSTRAINT download_protocol;
ALTER TABLE download ADD CONSTRAINT download_protocol
	CHECK (protocol IN ('usenet', 'torrent', 'youtube')) NOT VALID;
ALTER TABLE download VALIDATE CONSTRAINT download_protocol;

ALTER TABLE download DROP CONSTRAINT download_source;
ALTER TABLE download ADD CONSTRAINT download_source
	CHECK (source IN ('file', 'magnet', 'address')) NOT VALID;
ALTER TABLE download VALIDATE CONSTRAINT download_source;

ALTER TABLE download_client DROP CONSTRAINT download_client_address_form;
ALTER TABLE download_client ADD CONSTRAINT download_client_address_form
	CHECK (address = '' OR (char_length(address) BETWEEN 8 AND 255 AND address ~ '^https?://[^/?#@\s]+$')) NOT VALID;
ALTER TABLE download_client VALIDATE CONSTRAINT download_client_address_form;

ALTER TABLE library_root DROP CONSTRAINT library_root_media_type_known;
ALTER TABLE library_root ADD CONSTRAINT library_root_media_type_known
	CHECK (media_type IN ('movies', 'tv', 'music', 'audiobooks', 'books', 'downloads', 'other', 'youtube')) NOT VALID;
ALTER TABLE library_root VALIDATE CONSTRAINT library_root_media_type_known;

CREATE TABLE youtube_state (
	only_row     boolean     PRIMARY KEY DEFAULT true,
	enabled      boolean     NOT NULL DEFAULT false,
	enabled_by   text,
	enabled_at   timestamptz,
	-- YouTube refused this server: nothing is asked of it until then.  The
	-- pause doubles each time, from an hour to a day; a fetch that works
	-- starts it over.
	paused_until timestamptz,
	pause_step   integer     NOT NULL DEFAULT 0,

	CONSTRAINT youtube_state_one CHECK (only_row),
	CONSTRAINT youtube_state_form CHECK (pause_step BETWEEN 0 AND 16
		AND (enabled_by IS NULL OR (char_length(enabled_by) BETWEEN 1 AND 200 AND enabled_by !~ '[\x01-\x1f\x7f]')))
);

INSERT INTO youtube_state (only_row) VALUES (true);

CREATE TABLE youtube_video (
	video_id       text        PRIMARY KEY,
	-- listed: looked at, nothing asked.  wanted: its download is queued.
	-- fetching: yt-dlp is at work.  fetched: its file is in the library.
	-- failed, cancelled: not fetched; it can be asked for again.
	state          text        NOT NULL DEFAULT 'listed',
	title          text        NOT NULL,
	channel        text        NOT NULL,
	channel_id     text,
	upload_date    date,
	duration_s     integer,
	-- What is taken: "1080p, avc1 with mp4a".
	format_note    text,
	expected_bytes bigint,
	-- Where it goes, and where it went (the file, from the volume's top).
	root_id        bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	volume_id      bigint      REFERENCES volume (id) ON DELETE SET NULL,
	rel_path       text,
	size_bytes     bigint,
	done_bytes     bigint,
	rate_bps       bigint,
	eta_s          bigint,
	-- Why it failed, in Axis's words.
	message        text,
	job_id         bigint,
	asked_by       text,
	looked_at      timestamptz NOT NULL DEFAULT now(),
	wanted_at      timestamptz,
	started_at     timestamptz,
	finished_at    timestamptz,
	updated_at     timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT youtube_video_id_form CHECK (video_id ~ '^[A-Za-z0-9_-]{11}$'),
	CONSTRAINT youtube_video_state CHECK (state IN ('listed', 'wanted', 'fetching', 'fetched', 'failed',
		'cancelled')),
	CONSTRAINT youtube_video_fetched_located CHECK (state <> 'fetched' OR rel_path IS NOT NULL),
	CONSTRAINT youtube_video_texts CHECK (
		char_length(title) BETWEEN 1 AND 300 AND title !~ '[\x01-\x1f\x7f]'
		AND char_length(channel) BETWEEN 1 AND 200 AND channel !~ '[\x01-\x1f\x7f]'
		AND (channel_id IS NULL OR channel_id ~ '^[A-Za-z0-9_-]{1,64}$')
		AND (format_note IS NULL OR (char_length(format_note) BETWEEN 1 AND 200 AND format_note !~ '[\x01-\x1f\x7f]'))
		AND (message IS NULL OR (char_length(message) BETWEEN 1 AND 300 AND message !~ '[\x01-\x1f\x7f]'))
		AND (asked_by IS NULL OR (char_length(asked_by) BETWEEN 1 AND 200 AND asked_by !~ '[\x01-\x1f\x7f]'))
		AND (rel_path IS NULL OR (octet_length(rel_path) BETWEEN 1 AND 1024 AND rel_path !~ '^/'
			AND rel_path !~ '[\x01-\x1f\x7f]'))),
	CONSTRAINT youtube_video_figures CHECK (
		(duration_s IS NULL OR duration_s BETWEEN 0 AND 1000000)
		AND (expected_bytes IS NULL OR expected_bytes >= 0)
		AND (size_bytes IS NULL OR size_bytes >= 0) AND (done_bytes IS NULL OR done_bytes >= 0)
		AND (rate_bps IS NULL OR rate_bps >= 0) AND (eta_s IS NULL OR eta_s >= 0))
);

CREATE INDEX youtube_video_recent ON youtube_video (updated_at DESC);
