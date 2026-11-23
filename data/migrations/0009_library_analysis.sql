-- 0009_library_analysis.sql - snapshots of the library reports
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The Analysis pages (src/web/axis-web-analysis*.c) read what the
-- analysis.refresh job (src/library/axis-analysis*.c) computed, never the
-- index itself, so a page costs the same however large the library is.
-- Decisions about converting and deleting files will be made from these
-- figures, so the rules are strict:
--
--  - A snapshot is written in one transaction (snapshot row and facts), so a
--    snapshot that is visible is complete.  taken_at is the time the
--    refresh's view of the index begins (the refresh reads the index in one
--    REPEATABLE READ transaction, so all of its figures describe the same
--    moment).
--  - Only present files are counted.  Files the last complete scan did not
--    find (suspected, missing) are counted on their own lines of the
--    coverage, never in a total.  Two paths of one volume with the same
--    inode (and more than one hard link) are one file: its size is counted
--    once in every byte total, attributed to the path with the lowest file
--    id outside a downloads root (a film imported by hard link from a
--    download counts as a film), else the lowest.  "paths" counts every
--    path, so that a figure equals the length of
--    the file list it links to; "linked_paths" in extra says how many of
--    those paths were not counted in "bytes".
--  - Files on a volume that is not usable when the snapshot is taken are
--    counted from the index as it last was and marked ("offline_paths" in
--    extra, the volume's state in its space row); nothing is dropped.
--  - Nothing here has one row per file.  A fact is an aggregate, or a
--    bounded list (at most a few hundred items) kept in extra->'items'.
--    Items carry file ids and the stored form of paths
--    (axis-library-name.h); they describe the file as it was at taken_at.
--  - coverage says what the snapshot could not see (files not probed,
--    failed probes, roots never completely scanned, volumes offline, ...),
--    config the thresholds it was computed with.
--
-- Retention (axis-analysis-refresh.c): the last snapshot of each UTC day is
-- kept for analysis.keep_daily, every other for analysis.keep_other; the
-- newest is always kept.

CREATE TABLE analysis_snapshot (
	id              bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	taken_at        timestamptz NOT NULL,
	finished_at     timestamptz NOT NULL DEFAULT now(),
	trigger         text        NOT NULL,
	job_id          bigint,
	took_ms         integer     NOT NULL,
	-- Present paths and their size (hard links counted once): the growth
	-- chart's points.
	paths           bigint      NOT NULL,
	bytes           bigint      NOT NULL,
	-- Capacity and used space of the volumes, as last measured, for the
	-- growth chart (NULL when no volume had been measured).
	volumes_total   bigint,
	volumes_used    bigint,
	coverage        jsonb       NOT NULL,
	config          jsonb       NOT NULL,

	CONSTRAINT analysis_snapshot_trigger_known CHECK (trigger IN ('schedule', 'request', 'direct')),
	CONSTRAINT analysis_snapshot_sane CHECK (paths >= 0 AND bytes >= 0 AND took_ms >= 0),
	CONSTRAINT analysis_snapshot_objects
		CHECK (jsonb_typeof(coverage) = 'object' AND jsonb_typeof(config) = 'object')
);

CREATE INDEX analysis_snapshot_taken_idx ON analysis_snapshot (taken_at DESC);

CREATE TABLE analysis_fact (
	snapshot_id bigint NOT NULL REFERENCES analysis_snapshot (id) ON DELETE CASCADE,
	report      text   NOT NULL,
	dim1        text   NOT NULL DEFAULT '',
	dim2        text   NOT NULL DEFAULT '',
	paths       bigint NOT NULL DEFAULT 0,
	bytes       bigint NOT NULL DEFAULT 0,
	extra       jsonb  NOT NULL DEFAULT '{}',

	PRIMARY KEY (snapshot_id, report, dim1, dim2),
	CONSTRAINT analysis_fact_report_form CHECK (report ~ '^[a-z][a-z0-9_]{0,40}$'),
	CONSTRAINT analysis_fact_sane CHECK (paths >= 0 AND bytes >= 0
		AND octet_length(dim1) <= 200 AND octet_length(dim2) <= 200),
	CONSTRAINT analysis_fact_extra_object CHECK (jsonb_typeof(extra) = 'object'),
	-- A list is bounded; an aggregate is small.
	CONSTRAINT analysis_fact_extra_bounded CHECK (pg_column_size(extra) <= 4000000)
);
