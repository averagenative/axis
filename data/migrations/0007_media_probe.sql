-- 0007_media_probe.sql - what is inside each indexed video and audio file
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The probe (src/probe/) runs ffprobe on files of kind video or audio and
-- records the result here.  Later work decides from these rows which files
-- may be converted and their originals deleted, so the rules are strict:
--
--  - Unknown is NULL, never a guess.  A number that ffprobe printed but
--    that is out of any sane range (a 70000-pixel width, a negative
--    duration) is stored as NULL, not as it was; the CHECK constraints
--    below restate the ranges so that no other writer can store one.
--  - A stream's HDR class (hdr_format) is one of sdr, hlg, hdr10, hdr10plus,
--    dolby_vision.  hdr_uncertain is set whenever the signals conflict,
--    are incomplete, or look like Dolby Vision or dynamic HDR metadata
--    without matching the expected shape exactly.  A video stream that is
--    not a cover picture has a hdr_format or hdr_uncertain (or both):
--    "hdr_format IS NULL AND NOT hdr_uncertain" never occurs for one.
--    Whoever decides what may be converted must treat hdr_uncertain as
--    "not eligible", and anything but a plain sdr as needing its own rule.
--  - media_info.hdr_format is the most restrictive class among the file's
--    video streams (dolby_vision, then hdr10plus, hdr10, hlg, sdr), and
--    media_info.hdr_uncertain is true if any of them is uncertain.
--  - A row describes the file as it was when probed: file_size, mtime,
--    ctime and inode are the values the probe saw on the open file, equal
--    to the library_file row it was stored against.  A probe whose file
--    changed meanwhile stores nothing.
--  - status 'failed' and 'timeout' rows have an error text and no streams;
--    they are not retried until the file changes (library_file.changed), an
--    administrator asks, or probe_version is raised.
--  - Everything text comes from ffprobe and so from the file: it has been
--    cut to a bounded length and cleaned (valid UTF-8, no control
--    characters) before it got here, and must still be escaped by whatever
--    shows it.

CREATE TABLE media_info (
	file_id               bigint      PRIMARY KEY REFERENCES library_file (id) ON DELETE CASCADE,
	status                text        NOT NULL,
	error                 text,
	-- Raised when the mapping from ffprobe's output changes; older rows are
	-- probed again, at low priority.
	probe_version         integer     NOT NULL,
	probed_at             timestamptz NOT NULL DEFAULT now(),
	probe_ms              integer,
	tool_version          text,

	-- The file as it was probed (see above).
	file_size             bigint      NOT NULL,
	mtime_sec             bigint      NOT NULL,
	mtime_nsec            integer     NOT NULL,
	ctime_sec             bigint      NOT NULL,
	ctime_nsec            integer     NOT NULL,
	inode                 bigint      NOT NULL,

	-- The container.
	container             text,
	duration_ms           bigint,
	bitrate               bigint,
	-- bitrate is size / duration rather than what the container says.
	bitrate_estimated     boolean     NOT NULL DEFAULT false,
	chapter_count         integer,
	stream_count          integer,
	-- Streams beyond the ones stored (at most 512 are).
	streams_omitted       integer     NOT NULL DEFAULT 0,
	-- Of the stored streams; a cover picture is not a video stream here
	-- (see has_embedded_art).
	video_streams         integer     NOT NULL DEFAULT 0,
	audio_streams         integer     NOT NULL DEFAULT 0,
	subtitle_streams      integer     NOT NULL DEFAULT 0,

	-- The main video stream: the largest picture that is not a cover.
	video_codec           text,
	width                 integer,
	height                integer,
	-- 'sd', '720p', '1080p', '2160p' or 'other'; from the dimensions, once,
	-- here, so that every filter and report agrees.
	resolution_class      text,
	bit_depth             integer,
	frame_rate            double precision,
	hdr_format            text,
	hdr_uncertain         boolean     NOT NULL DEFAULT false,
	hdr_note              text,
	-- Video bitrate / (width * height * frame rate).
	bits_per_pixel        double precision,
	bits_per_pixel_estimated boolean  NOT NULL DEFAULT false,
	-- NULL: not known (field order is often not signalled).
	interlaced            boolean,
	vfr_suspected         boolean,

	-- The main audio stream: the first default one, else the first.
	audio_codec           text,
	audio_channels        integer,
	audio_sample_rate     integer,
	audio_bit_depth       integer,
	-- NULL: not known for this codec.
	audio_lossless        boolean,
	-- Of every audio stream; NULL if any has no bitrate at all.
	audio_bitrate_total   bigint,
	audio_bitrate_total_estimated boolean NOT NULL DEFAULT false,

	-- Tags and cover art (music and audiobooks).
	tag_title             text,
	tag_artist            text,
	tag_album             text,
	has_track             boolean     NOT NULL DEFAULT false,
	has_disc              boolean     NOT NULL DEFAULT false,
	has_musicbrainz       boolean     NOT NULL DEFAULT false,
	has_replaygain        boolean     NOT NULL DEFAULT false,
	has_embedded_art      boolean     NOT NULL DEFAULT false,
	art_width             integer,
	art_height            integer,
	-- SHA-256 (hex) over the root, the file's directory and the album tag,
	-- so that the tracks of one album in one folder share it.  NULL without
	-- an album tag.
	album_key             text,

	CONSTRAINT media_info_status_known CHECK (status IN ('ok', 'failed', 'timeout')),
	CONSTRAINT media_info_error_iff_not_ok CHECK ((status = 'ok') = (error IS NULL)),
	CONSTRAINT media_info_error_sane
		CHECK (error IS NULL OR (char_length(error) BETWEEN 1 AND 2000 AND error !~ '[\x01-\x1f\x7f]')),
	CONSTRAINT media_info_version_sane CHECK (probe_version >= 1),
	CONSTRAINT media_info_stamp_sane CHECK (file_size >= 0
		AND mtime_nsec BETWEEN 0 AND 999999999 AND ctime_nsec BETWEEN 0 AND 999999999),
	CONSTRAINT media_info_ok_has_container CHECK (status <> 'ok' OR container IS NOT NULL),
	CONSTRAINT media_info_not_ok_is_bare CHECK (status = 'ok' OR (container IS NULL AND stream_count IS NULL
		AND video_streams = 0 AND audio_streams = 0 AND subtitle_streams = 0 AND hdr_format IS NULL
		AND NOT hdr_uncertain)),
	CONSTRAINT media_info_ranges CHECK (
		(duration_ms IS NULL OR duration_ms BETWEEN 0 AND 34560000000)
		AND (bitrate IS NULL OR bitrate BETWEEN 0 AND 1000000000000)
		AND (chapter_count IS NULL OR chapter_count BETWEEN 0 AND 1000000)
		AND (stream_count IS NULL OR stream_count BETWEEN 0 AND 100000)
		AND streams_omitted BETWEEN 0 AND 100000
		AND video_streams BETWEEN 0 AND 512 AND audio_streams BETWEEN 0 AND 512
		AND subtitle_streams BETWEEN 0 AND 512
		AND (width IS NULL OR width BETWEEN 1 AND 32768)
		AND (height IS NULL OR height BETWEEN 1 AND 32768)
		AND (bit_depth IS NULL OR bit_depth BETWEEN 1 AND 64)
		AND (frame_rate IS NULL OR (frame_rate > 0 AND frame_rate <= 1000000))
		AND (bits_per_pixel IS NULL OR (bits_per_pixel >= 0 AND bits_per_pixel < 1000000))
		AND (audio_channels IS NULL OR audio_channels BETWEEN 1 AND 256)
		AND (audio_sample_rate IS NULL OR audio_sample_rate BETWEEN 1 AND 100000000)
		AND (audio_bit_depth IS NULL OR audio_bit_depth BETWEEN 1 AND 64)
		AND (audio_bitrate_total IS NULL OR audio_bitrate_total BETWEEN 0 AND 1000000000000)
		AND (art_width IS NULL OR art_width BETWEEN 1 AND 32768)
		AND (art_height IS NULL OR art_height BETWEEN 1 AND 32768)),
	CONSTRAINT media_info_resolution_class_known
		CHECK (resolution_class IS NULL OR resolution_class IN ('sd', '720p', '1080p', '2160p', 'other')),
	CONSTRAINT media_info_hdr_known
		CHECK (hdr_format IS NULL OR hdr_format IN ('sdr', 'hlg', 'hdr10', 'hdr10plus', 'dolby_vision')),
	CONSTRAINT media_info_album_key_form CHECK (album_key IS NULL OR album_key ~ '^[0-9a-f]{64}$')
);

-- Files to probe again after a mapping change.
CREATE INDEX media_info_version_idx ON media_info (probe_version);

-- Failures, for the pages that list them.
CREATE INDEX media_info_failed_idx ON media_info (status) WHERE status <> 'ok';

-- The tracks of an album.
CREATE INDEX media_info_album_idx ON media_info (album_key) WHERE album_key IS NOT NULL;

-- One row per stream of a file that was probed successfully.
CREATE TABLE media_stream (
	file_id               bigint      NOT NULL REFERENCES media_info (file_id) ON DELETE CASCADE,
	-- ffprobe's stream index.
	idx                   integer     NOT NULL,
	kind                  text        NOT NULL,
	codec                 text,
	profile               text,
	duration_ms           bigint,
	bitrate               bigint,
	-- No bitrate of its own: derived from the container's.
	bitrate_estimated     boolean     NOT NULL DEFAULT false,
	language              text,
	title                 text,
	is_default            boolean     NOT NULL DEFAULT false,
	is_forced             boolean     NOT NULL DEFAULT false,
	-- A cover picture, not a video.
	attached_pic          boolean     NOT NULL DEFAULT false,

	-- Video.
	width                 integer,
	height                integer,
	pixel_format          text,
	bit_depth             integer,
	frame_rate            double precision,
	-- The lowest rate that fits every timestamp; far from frame_rate for
	-- variable frame rate.
	real_frame_rate       double precision,
	field_order           text,
	color_range           text,
	color_space           text,
	color_transfer        text,
	color_primaries       text,
	hdr_format            text,
	hdr_uncertain         boolean     NOT NULL DEFAULT false,
	hdr_note              text,
	has_mastering_display boolean     NOT NULL DEFAULT false,
	has_content_light     boolean     NOT NULL DEFAULT false,
	dv_profile            integer,
	dv_level              integer,
	-- dv_bl_signal_compatibility_id as ffprobe prints it; dv_hdr10_compatible
	-- is true for 1, false for 0, 2 and 4, and NULL for anything else.
	dv_bl_compat_id       integer,
	dv_hdr10_compatible   boolean,

	-- Audio.
	channels              integer,
	channel_layout        text,
	sample_rate           integer,
	bit_depth_audio       integer,
	lossless              boolean,

	-- Subtitles: 'text', 'image', or NULL when the codec is not known.
	subtitle_kind         text,

	PRIMARY KEY (file_id, idx),
	CONSTRAINT media_stream_kind_known
		CHECK (kind IN ('video', 'audio', 'subtitle', 'attachment', 'data', 'unknown')),
	CONSTRAINT media_stream_idx_sane CHECK (idx BETWEEN 0 AND 100000),
	CONSTRAINT media_stream_ranges CHECK (
		(duration_ms IS NULL OR duration_ms BETWEEN 0 AND 34560000000)
		AND (bitrate IS NULL OR bitrate BETWEEN 0 AND 1000000000000)
		AND (width IS NULL OR width BETWEEN 1 AND 32768)
		AND (height IS NULL OR height BETWEEN 1 AND 32768)
		AND (bit_depth IS NULL OR bit_depth BETWEEN 1 AND 64)
		AND (frame_rate IS NULL OR (frame_rate > 0 AND frame_rate <= 1000000))
		AND (real_frame_rate IS NULL OR (real_frame_rate > 0 AND real_frame_rate <= 1000000))
		AND (channels IS NULL OR channels BETWEEN 1 AND 256)
		AND (sample_rate IS NULL OR sample_rate BETWEEN 1 AND 100000000)
		AND (bit_depth_audio IS NULL OR bit_depth_audio BETWEEN 1 AND 64)
		AND (dv_profile IS NULL OR dv_profile BETWEEN 0 AND 255)
		AND (dv_level IS NULL OR dv_level BETWEEN 0 AND 255)
		AND (dv_bl_compat_id IS NULL OR dv_bl_compat_id BETWEEN 0 AND 15)),
	CONSTRAINT media_stream_hdr_known
		CHECK (hdr_format IS NULL OR hdr_format IN ('sdr', 'hlg', 'hdr10', 'hdr10plus', 'dolby_vision')),
	CONSTRAINT media_stream_hdr_only_video CHECK (kind = 'video' OR (hdr_format IS NULL AND NOT hdr_uncertain)),
	-- Never "no class and no doubt" for a picture that is not a cover.
	CONSTRAINT media_stream_video_classified
		CHECK (kind <> 'video' OR attached_pic OR hdr_format IS NOT NULL OR hdr_uncertain),
	CONSTRAINT media_stream_dv_needs_class
		CHECK (dv_profile IS NULL AND dv_bl_compat_id IS NULL OR hdr_format = 'dolby_vision'),
	CONSTRAINT media_stream_subtitle_kind_known
		CHECK (subtitle_kind IS NULL OR subtitle_kind IN ('text', 'image'))
);

-- Files a probe could not finish for a reason that is not the file's
-- failure (it changed or went away meanwhile): not tried again before
-- "until", which backs off with each attempt.  Cleared by a stored result.
CREATE TABLE media_probe_defer (
	file_id               bigint      PRIMARY KEY REFERENCES library_file (id) ON DELETE CASCADE,
	until                 timestamptz NOT NULL,
	attempts              integer     NOT NULL DEFAULT 1,
	reason                text        NOT NULL,

	CONSTRAINT media_probe_defer_reason_sane
		CHECK (char_length(reason) <= 200 AND reason !~ '[\x01-\x1f\x7f]')
);
