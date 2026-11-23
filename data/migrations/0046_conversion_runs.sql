-- 0046_conversion_runs.sql - conversion runs: a list of a library's files,
-- the largest estimated saving first and a series kept together, worked
-- through by asking for ordinary single conversions one at a time; and what
-- a conversion made, so that a file Axis made from an SDR source counts as
-- SDR
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A run (src/transcode/axis-conversion-run*.c, design.md of
-- add-conversion-runs) adds no way of replacing a file: each file it works
-- on is a conversion row like any other, asked for through the same request
-- and its gates (conversion.run_id says which run asked).  What is kept here:
--
--  - conversion_run: the scope, the profile revision, what the run does
--    (trials, or replacing originals once a person confirmed it), its limits,
--    its state, and why it is paused (a person must act) or waiting (it goes
--    on by itself);
--  - conversion_run_unit: what the list is ranked by: a movie, or a series
--    (all its episode files, kept together so that a series is finished as a
--    unit), with its files, their size now and the estimate after;
--  - conversion_run_entry: one file of the list, in the order the run takes
--    them (units by rank, a series' episodes by season and number), with the
--    estimate made when the list was built and what became of it;
--  - conversion_run_excluded and conversion_run_reason: the files of the
--    scope that are not in the list, each with the one reason (the
--    estimator's, or the conversion's memory, or a conversion in flight), so
--    that they can be looked at;
--  - conversion_run_wait: the volumes whose files wait (offline, below their
--    free-space floor, or proved by no connected worker): an offline disk
--    freezes its files, it is not a failure of the run.

CREATE TABLE conversion_run (
	id             bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	revision_id    bigint      NOT NULL REFERENCES conversion_profile_revision (id) ON DELETE RESTRICT,
	-- 'movies' (every movies root), 'tv' (every TV root), 'roots' (root_ids),
	-- 'series' (the episode files of one series on disk, title_id).
	scope          text        NOT NULL,
	root_ids       bigint[],
	title_id       bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	scope_text     text        NOT NULL,
	state          text        NOT NULL DEFAULT 'listing',
	-- 'trial' until a person confirms replacing originals.
	outcome        text        NOT NULL DEFAULT 'trial',
	at_once        integer     NOT NULL DEFAULT 1,
	cap_files      integer,
	cap_bytes      bigint,
	cap_seconds    bigint,
	-- The last unit within the caps (NULL: every unit).
	cap_unit       integer,
	-- When it may ask for files: minutes after midnight, local time.
	window_start   integer,
	window_end     integer,
	created_by     text        NOT NULL,
	created_at     timestamptz NOT NULL DEFAULT now(),
	list_job_id    bigint,
	listed_at      timestamptz,
	list_text      text,
	scope_files    bigint      NOT NULL DEFAULT 0,
	started_by     text,
	started_at     timestamptz,
	confirmed_by   text,
	confirmed_at   timestamptz,
	finished_at    timestamptz,
	finish_text    text,
	-- Paused: a person must act (pause_by NULL: the run paused itself).
	pause_code     text,
	pause_text     text,
	paused_at      timestamptz,
	paused_by      text,
	-- Waiting: it goes on by itself when this clears.
	wait_code      text,
	wait_text      text,
	wait_since     timestamptz,
	failed_in_row  integer     NOT NULL DEFAULT 0,
	refused_in_row integer     NOT NULL DEFAULT 0,
	-- The unit it asked for a file of last: its files come first, so that a
	-- series it started is finished before another is (when a disk that
	-- went away comes back, the run does not leave the series it went on
	-- with half done).
	current_unit   integer,
	stepped_at     timestamptz,
	updated_at     timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT conversion_run_scope CHECK (scope IN ('movies', 'tv', 'roots', 'series')
		AND (scope <> 'roots' OR cardinality(root_ids) BETWEEN 1 AND 64)),
	CONSTRAINT conversion_run_state CHECK (state IN ('listing', 'ready', 'running', 'paused', 'finished', 'cancelled',
		'failed')),
	CONSTRAINT conversion_run_outcome CHECK (outcome IN ('trial', 'replace')
		AND (outcome = 'trial' OR confirmed_by IS NOT NULL)),
	CONSTRAINT conversion_run_limits CHECK (at_once BETWEEN 1 AND 8
		AND (cap_files IS NULL OR cap_files >= 1) AND (cap_bytes IS NULL OR cap_bytes >= 1)
		AND (cap_seconds IS NULL OR cap_seconds >= 60) AND (cap_unit IS NULL OR cap_unit >= 0)
		AND (window_start IS NULL) = (window_end IS NULL)
		AND (window_start IS NULL OR (window_start BETWEEN 0 AND 1439 AND window_end BETWEEN 0 AND 1439
			AND window_start <> window_end))),
	CONSTRAINT conversion_run_codes CHECK ((pause_code IS NULL) = (pause_text IS NULL)
		AND (wait_code IS NULL) = (wait_text IS NULL)
		AND (pause_code IS NULL OR pause_code ~ '^[a-z_]{1,32}$') AND (wait_code IS NULL OR wait_code ~ '^[a-z_]{1,32}$')),
	CONSTRAINT conversion_run_texts CHECK (char_length(scope_text) BETWEEN 1 AND 500
		AND char_length(created_by) BETWEEN 1 AND 200
		AND (started_by IS NULL OR char_length(started_by) BETWEEN 1 AND 200)
		AND (confirmed_by IS NULL OR char_length(confirmed_by) BETWEEN 1 AND 200)
		AND (paused_by IS NULL OR char_length(paused_by) BETWEEN 1 AND 200)
		AND (list_text IS NULL OR char_length(list_text) <= 2000)
		AND (finish_text IS NULL OR char_length(finish_text) <= 2000)
		AND (pause_text IS NULL OR char_length(pause_text) <= 2000)
		AND (wait_text IS NULL OR char_length(wait_text) <= 2000)),
	CONSTRAINT conversion_run_counters CHECK (failed_in_row >= 0 AND refused_in_row >= 0 AND scope_files >= 0)
);

-- One run asks for files at a time.
CREATE UNIQUE INDEX conversion_run_one_running ON conversion_run ((true)) WHERE state = 'running';
CREATE INDEX conversion_run_recent ON conversion_run (id DESC);

-- A movie, a series (every episode file of it, whichever root it is in), a
-- TV folder that is no identified series (taken as one), or a file.
CREATE TABLE conversion_run_unit (
	run_id    bigint  NOT NULL REFERENCES conversion_run (id) ON DELETE CASCADE,
	position  integer NOT NULL,
	unit_key  text    COLLATE "C" NOT NULL,
	kind      text    NOT NULL,
	title_id  bigint  REFERENCES meta_title (id) ON DELETE SET NULL,
	root_id   bigint  REFERENCES library_root (id) ON DELETE SET NULL,
	-- The folder (a series, a folder) or the file (stored form).
	rel_path  text    COLLATE "C" NOT NULL,
	files     integer NOT NULL,
	bytes     bigint  NOT NULL,
	mid       bigint  NOT NULL,
	removed   boolean NOT NULL DEFAULT false,

	PRIMARY KEY (run_id, position),
	CONSTRAINT conversion_run_unit_key UNIQUE (run_id, unit_key),
	CONSTRAINT conversion_run_unit_form CHECK (position >= 1 AND kind IN ('movie', 'series', 'folder', 'file')
		AND files >= 1 AND bytes >= 0 AND mid >= 0 AND octet_length(unit_key) <= 4200)
);

CREATE TABLE conversion_run_entry (
	id             bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	run_id         bigint      NOT NULL REFERENCES conversion_run (id) ON DELETE CASCADE,
	-- The order the run takes it in, and its unit's rank (0 while listed).
	position       integer     NOT NULL DEFAULT 0,
	unit_position  integer     NOT NULL DEFAULT 0,
	-- What it is grouped by, while the list is made.
	unit_key       text        COLLATE "C" NOT NULL,
	unit_kind      text        NOT NULL,
	unit_title_id  bigint,
	unit_path      text        COLLATE "C" NOT NULL,
	file_id        bigint      REFERENCES library_file (id) ON DELETE SET NULL,
	volume_id      bigint      REFERENCES volume (id) ON DELETE SET NULL,
	root_id        bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	rel_path       text        COLLATE "C" NOT NULL,
	season_number  integer,
	episode_number integer,
	-- When the list was made: its size, and the estimate after.
	size           bigint      NOT NULL,
	low            bigint      NOT NULL,
	mid            bigint      NOT NULL,
	high           bigint      NOT NULL,
	state          text        NOT NULL DEFAULT 'waiting',
	-- The conversion asked for it (and a trial of it, when it was asked again
	-- as a replacement).
	conversion_id  bigint      REFERENCES conversion (id) ON DELETE SET NULL,
	trial_id       bigint      REFERENCES conversion (id) ON DELETE SET NULL,
	outcome        text,
	result_size    bigint,
	reason_code    text,
	reason_text    text,
	settled_at     timestamptz,

	CONSTRAINT conversion_run_entry_state CHECK (state IN ('waiting', 'asked', 'done', 'skipped', 'failed', 'removed')),
	CONSTRAINT conversion_run_entry_outcome CHECK (outcome IS NULL OR outcome IN ('trial', 'replace')),
	CONSTRAINT conversion_run_entry_sizes CHECK (size >= 0 AND low >= 0 AND low <= mid AND mid <= high
		AND (result_size IS NULL OR result_size >= 0)),
	CONSTRAINT conversion_run_entry_form CHECK (unit_kind IN ('movie', 'series', 'folder', 'file')
		AND rel_path <> '' AND octet_length(unit_key) <= 4200
		AND (reason_code IS NULL OR reason_code ~ '^[a-z_0-9]{1,32}$')
		AND (reason_text IS NULL OR char_length(reason_text) <= 2000))
);

CREATE INDEX conversion_run_entry_order ON conversion_run_entry (run_id, position);
CREATE INDEX conversion_run_entry_unit ON conversion_run_entry (run_id, unit_position, position);
CREATE INDEX conversion_run_entry_waiting ON conversion_run_entry (run_id, position) WHERE state = 'waiting';
CREATE INDEX conversion_run_entry_asked ON conversion_run_entry (run_id) WHERE state = 'asked';
CREATE INDEX conversion_run_entry_settled ON conversion_run_entry (run_id, settled_at DESC) WHERE settled_at IS NOT NULL;
CREATE INDEX conversion_run_entry_file ON conversion_run_entry (file_id);
CREATE INDEX conversion_run_entry_conversion ON conversion_run_entry (conversion_id) WHERE conversion_id IS NOT NULL;
CREATE INDEX conversion_run_entry_trial ON conversion_run_entry (trial_id) WHERE trial_id IS NOT NULL;

-- The files of the scope that are not in the list, by reason.
CREATE TABLE conversion_run_excluded (
	run_id  bigint NOT NULL REFERENCES conversion_run (id) ON DELETE CASCADE,
	reason  text   NOT NULL,
	file_id bigint NOT NULL REFERENCES library_file (id) ON DELETE CASCADE,
	size    bigint NOT NULL,

	PRIMARY KEY (run_id, reason, file_id),
	CONSTRAINT conversion_run_excluded_form CHECK (reason ~ '^[a-z_0-9]{1,32}$' AND size >= 0)
);

CREATE INDEX conversion_run_excluded_largest ON conversion_run_excluded (run_id, reason, size DESC, file_id DESC);
CREATE INDEX conversion_run_excluded_file ON conversion_run_excluded (file_id);

CREATE TABLE conversion_run_reason (
	run_id bigint NOT NULL REFERENCES conversion_run (id) ON DELETE CASCADE,
	reason text   NOT NULL,
	files  bigint NOT NULL,
	bytes  bigint NOT NULL,

	PRIMARY KEY (run_id, reason),
	CONSTRAINT conversion_run_reason_form CHECK (reason ~ '^[a-z_0-9]{1,32}$' AND files >= 0 AND bytes >= 0)
);

-- The volumes whose files wait, as the run last found them.
CREATE TABLE conversion_run_wait (
	run_id    bigint      NOT NULL REFERENCES conversion_run (id) ON DELETE CASCADE,
	volume_id bigint      NOT NULL REFERENCES volume (id) ON DELETE CASCADE,
	code      text        NOT NULL,
	text      text        NOT NULL,
	since     timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (run_id, volume_id),
	CONSTRAINT conversion_run_wait_form CHECK (code IN ('offline', 'full', 'no_worker')
		AND char_length(text) BETWEEN 1 AND 2000)
);

-- ── What a conversion made ────────────────────────────────────────

-- run_id, run_entry_id: the run that asked for it.  source_hdr: the
-- source's HDR class as the probe classified it at the request ('sdr' only
-- when certain; anything uncertain is 'unclassified').  made_hdr: the class
-- of the picture the plan makes, recorded when a worker took the work.
ALTER TABLE conversion
	ADD COLUMN run_id       bigint REFERENCES conversion_run (id) ON DELETE SET NULL,
	ADD COLUMN run_entry_id bigint REFERENCES conversion_run_entry (id) ON DELETE SET NULL,
	ADD COLUMN source_hdr   text,
	ADD COLUMN made_hdr     text,
	ADD CONSTRAINT conversion_hdr_classes CHECK (
		(source_hdr IS NULL OR source_hdr IN ('sdr', 'hlg', 'hdr10', 'hdr10plus', 'dolby_vision', 'unclassified'))
		AND (made_hdr IS NULL OR made_hdr IN ('sdr', 'hlg', 'hdr10', 'hdr10plus', 'dolby_vision', 'unclassified')));

CREATE INDEX conversion_run_link ON conversion (run_id) WHERE run_id IS NOT NULL;
CREATE INDEX conversion_run_entry_link ON conversion (run_entry_id) WHERE run_entry_id IS NOT NULL;

-- The files whose content now is what Axis made from a source the probe
-- classified as SDR, with a plan that kept SDR, which passed every check and
-- was put in place: the file is still that output (its inode, size and
-- modification time are the ones recorded when it was settled).  Such a file
-- is SDR whatever its probe says: a picture of more than 8 bits that states
-- no transfer function is "HDR uncertain" to the probe (it cannot tell it
-- from Dolby Vision profile 5), but Axis knows what it made.  A file Axis did
-- not make keeps the probe's caution.
CREATE VIEW conversion_known_sdr AS
	SELECT DISTINCT c.file_id
	FROM conversion c JOIN library_file f ON f.id = c.file_id
	WHERE c.state = 'done' AND c.outcome = 'replace' AND c.source_hdr = 'sdr' AND c.made_hdr = 'sdr'
	  AND c.output_inode IS NOT NULL AND f.inode = c.output_inode AND f.size = c.output_size
	  AND f.mtime_sec = c.output_mtime_sec AND f.mtime_nsec = c.output_mtime_nsec;

-- Per profile revision, what its finished conversions came to beside what
-- was estimated when each was asked for: kept for a later change that
-- corrects the estimates; nothing reads it back into the estimator.
CREATE VIEW conversion_estimate_seen AS
	SELECT c.revision_id, count(*) AS files, sum(c.src_size) AS before_bytes, sum(c.estimate_bytes) AS estimated_bytes,
	       sum(c.output_size) AS actual_bytes
	FROM conversion c
	WHERE c.state = 'done' AND c.estimate_bytes IS NOT NULL AND c.output_size IS NOT NULL
	GROUP BY c.revision_id;
