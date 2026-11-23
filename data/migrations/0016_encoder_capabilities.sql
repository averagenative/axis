-- 0016_encoder_capabilities.sql - what each machine's ffmpeg was shown to do
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The capability check (src/transcode/axis-convert-caps.h) makes test
-- encodes with the conversion builder's own arguments and reads them back
-- with the probe.  Its result is kept per machine ('local' is the daemon's
-- own; workers come later) together with the ffmpeg build it was measured
-- on: tool_digest is the SHA-256 of everything `ffmpeg -version` printed.
-- A build whose digest is not the stored one is measured again before any
-- conversion arguments are built for it, and the rows are replaced whole.
--
--  - encoder_tool: one row per machine: the build, when, and whether the
--    encoders the plans use beside the picture (AAC, mov_text, SubRip) are
--    there.
--  - encoder_capability: one row per encoder of the table: usable (an 8-bit
--    test came out as planned), ten_bit (a 10-bit HLG one did), hdr10 (an
--    HDR10 one came back with the same static metadata in its bitstream).
--    Each needs the one before.  reason says why not, when not.

CREATE TABLE encoder_tool (
	machine       text        PRIMARY KEY,
	tool_version  text        NOT NULL,
	tool_digest   text        NOT NULL,
	measured_at   timestamptz NOT NULL DEFAULT now(),
	aac           boolean     NOT NULL,
	mov_text      boolean     NOT NULL,
	subrip        boolean     NOT NULL,

	CONSTRAINT encoder_tool_machine_form CHECK (machine ~ '^[a-z][a-z0-9_-]{0,62}$'),
	CONSTRAINT encoder_tool_version_sane
		CHECK (char_length(tool_version) BETWEEN 1 AND 64 AND tool_version !~ '[\x01-\x1f\x7f]'),
	CONSTRAINT encoder_tool_digest_form CHECK (tool_digest ~ '^[0-9a-f]{64}$')
);

CREATE TABLE encoder_capability (
	machine   text    NOT NULL REFERENCES encoder_tool (machine) ON DELETE CASCADE,
	encoder   text    NOT NULL,
	usable    boolean NOT NULL,
	ten_bit   boolean NOT NULL,
	hdr10     boolean NOT NULL,
	reason    text,

	PRIMARY KEY (machine, encoder),
	CONSTRAINT encoder_capability_encoder_known CHECK (encoder IN ('libx265', 'libx264', 'hevc_nvenc', 'h264_nvenc',
		'hevc_vaapi', 'h264_vaapi')),
	CONSTRAINT encoder_capability_in_order CHECK ((usable OR NOT ten_bit) AND (ten_bit OR NOT hdr10)),
	CONSTRAINT encoder_capability_reason_sane
		CHECK (reason IS NULL OR (char_length(reason) BETWEEN 1 AND 500 AND reason !~ '[\x01-\x1f\x7f]'))
);
