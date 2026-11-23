-- 0010_space_estimate.sql - calibration samples and snapshots of the Savings
-- reports
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The estimator (src/transcode/axis-estimate*.c) decides, for a file and a
-- target, whether the file would be converted and how large it would be
-- after.  Those per-file answers are computed when they are needed and
-- never stored: a change to the model or a new calibration sample can then
-- never leave a stale answer behind.  What is stored:
--
--  - estimate_sample: one row per real result, either a sample measured by
--    the optional measurement job (encoding short excerpts of a file to a
--    pipe) or, later, a real conversion.  A row holds the source facts the
--    model needs to predict the output again (so a corrected prior table
--    re-reads old samples correctly) and the output bitrate that was seen.
--    Nothing here is a path: a sample refers to its file by id, and keeps
--    its facts when the file goes (ON DELETE SET NULL).
--  - estimate_snapshot / estimate_fact: the Savings reports, computed by the
--    estimate.refresh job like the Analysis reports (0009): written in one
--    transaction, so a visible snapshot is complete; aggregates and bounded
--    lists only, never one row per file.  Middle totals are sums of the
--    per-file middle estimates, rounded per file to whole bytes, so a total
--    is exactly the sum of what the file pages show.
--
-- Retention: the newest snapshot is always kept, others for seven days.

CREATE TABLE estimate_sample (
	id                bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	recorded_at       timestamptz NOT NULL DEFAULT now(),
	-- 'measured' by the measurement job, 'converted' by a real conversion.
	origin            text        NOT NULL,
	-- The job that measured it, if any.
	job_id            bigint,
	file_id           bigint      REFERENCES library_file (id) ON DELETE SET NULL,
	-- The target (axis-estimate-target.c) and how it was encoded.
	target            text        NOT NULL,
	codec             text        NOT NULL,
	family            text        NOT NULL,
	quality           integer     NOT NULL,
	-- What was encoded: 'video' (the main video stream) or 'audio' (the
	-- main audio stream).
	part              text        NOT NULL,
	-- The source, as the model reads it.
	source_codec      text        NOT NULL,
	width             integer,
	height            integer,
	frame_rate        double precision,
	source_bitrate    bigint      NOT NULL,
	sample_rate       integer,
	channels          integer,
	bit_depth         integer,
	-- How much was encoded, and the bitrate of what came out.
	seconds           double precision NOT NULL,
	output_bitrate    bigint      NOT NULL,

	CONSTRAINT estimate_sample_origin_known CHECK (origin IN ('measured', 'converted')),
	CONSTRAINT estimate_sample_part_known CHECK (part IN ('video', 'audio')),
	CONSTRAINT estimate_sample_names CHECK (target ~ '^[a-z][a-z0-9_]{0,40}$'
		AND codec ~ '^[a-z0-9_]{1,32}$' AND family ~ '^[a-z]{1,16}$'
		AND source_codec ~ '^[a-z0-9_.-]{1,64}$'),
	CONSTRAINT estimate_sample_quality CHECK (quality BETWEEN 1 AND 7),
	CONSTRAINT estimate_sample_ranges CHECK (
		(width IS NULL OR width BETWEEN 1 AND 32768)
		AND (height IS NULL OR height BETWEEN 1 AND 32768)
		AND (frame_rate IS NULL OR (frame_rate > 0 AND frame_rate <= 1000))
		AND source_bitrate BETWEEN 1 AND 1000000000000
		AND (sample_rate IS NULL OR sample_rate BETWEEN 1 AND 100000000)
		AND (channels IS NULL OR channels BETWEEN 1 AND 256)
		AND (bit_depth IS NULL OR bit_depth BETWEEN 1 AND 64)
		AND seconds > 0 AND seconds <= 86400
		AND output_bitrate BETWEEN 1 AND 1000000000000),
	-- A video sample has the picture the model needs.
	CONSTRAINT estimate_sample_video_facts CHECK (part <> 'video'
		OR (width IS NOT NULL AND height IS NOT NULL AND frame_rate IS NOT NULL))
);

CREATE INDEX estimate_sample_target_idx ON estimate_sample (target, id);

CREATE TABLE estimate_snapshot (
	id                   bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	taken_at             timestamptz NOT NULL,
	finished_at          timestamptz NOT NULL DEFAULT now(),
	trigger              text        NOT NULL,
	job_id               bigint,
	took_ms              integer     NOT NULL,
	-- The newest analysis snapshot and calibration sample when it was taken:
	-- a refresh is due when either has moved on.
	analysis_snapshot_id bigint,
	last_sample_id       bigint      NOT NULL DEFAULT 0,
	-- What the snapshot could not see, in the keys of the Analysis coverage.
	coverage             jsonb       NOT NULL,
	-- The targets as configured, the model's version and calibration.
	config               jsonb       NOT NULL,

	CONSTRAINT estimate_snapshot_trigger_known CHECK (trigger IN ('schedule', 'request', 'direct')),
	CONSTRAINT estimate_snapshot_sane CHECK (took_ms >= 0 AND last_sample_id >= 0),
	CONSTRAINT estimate_snapshot_objects
		CHECK (jsonb_typeof(coverage) = 'object' AND jsonb_typeof(config) = 'object')
);

CREATE INDEX estimate_snapshot_taken_idx ON estimate_snapshot (taken_at DESC);

-- One figure of a target.  report is 'total', 'volume', 'root', 'category',
-- 'res', 'vcodec', 'acodec' (dim the key), 'skip' (dim the reason), 'top'
-- (the files with the largest estimated saving, in extra->'items').  For
-- every report but 'skip', paths and bytes are the files that would be
-- converted (hard links are pinned, so each is one file) and low, mid and
-- high their estimated size after; for 'skip', the files skipped for that
-- reason and their size (hard links counted once, as in the Analysis).
CREATE TABLE estimate_fact (
	snapshot_id bigint NOT NULL REFERENCES estimate_snapshot (id) ON DELETE CASCADE,
	target      text   NOT NULL,
	report      text   NOT NULL,
	dim         text   NOT NULL DEFAULT '',
	paths       bigint NOT NULL DEFAULT 0,
	bytes       bigint NOT NULL DEFAULT 0,
	low         bigint NOT NULL DEFAULT 0,
	mid         bigint NOT NULL DEFAULT 0,
	high        bigint NOT NULL DEFAULT 0,
	extra       jsonb  NOT NULL DEFAULT '{}',

	PRIMARY KEY (snapshot_id, target, report, dim),
	CONSTRAINT estimate_fact_names CHECK (target ~ '^[a-z][a-z0-9_]{0,40}$'
		AND report ~ '^[a-z][a-z0-9_]{0,40}$' AND octet_length(dim) <= 200),
	-- Never negative, and low <= mid <= high <= bytes for what is converted.
	CONSTRAINT estimate_fact_sane CHECK (paths >= 0 AND bytes >= 0 AND low >= 0
		AND low <= mid AND mid <= high AND (report = 'skip' OR high <= bytes)),
	CONSTRAINT estimate_fact_extra_object CHECK (jsonb_typeof(extra) = 'object'),
	CONSTRAINT estimate_fact_extra_bounded CHECK (pg_column_size(extra) <= 4000000)
);
