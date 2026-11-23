-- 0015_hdr10_values.sql - the values of HDR10 static metadata, and the probe
-- version 2 that stores them
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A conversion must give an HDR10 file's mastering display colour volume
-- (SMPTE ST 2086) and content light levels (CTA-861.3) back to the encoder
-- exactly, so version 2 of the probe (src/probe/axis-probe-model.h) stores
-- the values, not only whether they exist.  The units are those of the HEVC
-- bitstream: chromaticity coordinates in steps of 0.00002 (0 to 50000 is 0
-- to 1), luminance in steps of 0.0001 cd/m2, light levels in cd/m2.
--
--  - Every column is nullable: NULL is "not present, or not valid".  Values
--    are stored only when the whole set was complete, in range and the same
--    everywhere the file gave it (its container and its first frames); a
--    stream that claims such data and fails any of that is hdr_uncertain
--    and has no values.  The checks below restate the ranges and the
--    all-or-nothing rule, so that no other writer can store a partial set.
--  - Rows probed by version 1 are probed again only where version 2 can
--    differ: files with a stream that has mastering display or content
--    light data.  Every other row (failed and timed-out ones included) maps
--    exactly as before, so it is marked version 2 here instead of being
--    read again; a library of millions of files would otherwise count as
--    stale, and go unestimated, until every file had been probed once more.

ALTER TABLE media_stream
	ADD COLUMN md_red_x          integer,
	ADD COLUMN md_red_y          integer,
	ADD COLUMN md_green_x        integer,
	ADD COLUMN md_green_y        integer,
	ADD COLUMN md_blue_x         integer,
	ADD COLUMN md_blue_y         integer,
	ADD COLUMN md_white_x        integer,
	ADD COLUMN md_white_y        integer,
	ADD COLUMN md_min_luminance  bigint,
	ADD COLUMN md_max_luminance  bigint,
	ADD COLUMN cll_max_content   integer,
	ADD COLUMN cll_max_average   integer,
	ADD CONSTRAINT media_stream_mastering_whole CHECK (
		num_nulls(md_red_x, md_red_y, md_green_x, md_green_y, md_blue_x, md_blue_y, md_white_x, md_white_y,
			md_min_luminance, md_max_luminance) IN (0, 10)),
	ADD CONSTRAINT media_stream_mastering_ranges CHECK (md_max_luminance IS NULL OR (
		md_red_x BETWEEN 0 AND 50000 AND md_red_y BETWEEN 0 AND 50000
		AND md_green_x BETWEEN 0 AND 50000 AND md_green_y BETWEEN 0 AND 50000
		AND md_blue_x BETWEEN 0 AND 50000 AND md_blue_y BETWEEN 0 AND 50000
		AND md_white_x BETWEEN 0 AND 50000 AND md_white_y BETWEEN 0 AND 50000
		AND md_max_luminance BETWEEN 50000 AND 100000000
		AND md_min_luminance BETWEEN 0 AND 50000
		AND md_min_luminance < md_max_luminance)),
	ADD CONSTRAINT media_stream_light_whole CHECK (num_nulls(cll_max_content, cll_max_average) IN (0, 2)),
	ADD CONSTRAINT media_stream_light_ranges CHECK (cll_max_content IS NULL OR (
		cll_max_content BETWEEN 0 AND 65535 AND cll_max_average BETWEEN 0 AND 65535
		AND (cll_max_content = 0 OR cll_max_average = 0 OR cll_max_average <= cll_max_content))),
	-- Values only for a picture that claimed them.
	ADD CONSTRAINT media_stream_hdr10_values_claimed CHECK (
		(md_max_luminance IS NULL OR (kind = 'video' AND has_mastering_display))
		AND (cll_max_content IS NULL OR (kind = 'video' AND has_content_light)));

UPDATE media_info m SET probe_version = 2
	WHERE m.probe_version = 1
	AND NOT EXISTS (SELECT 1 FROM media_stream s WHERE s.file_id = m.file_id
		AND (s.has_mastering_display OR s.has_content_light));
