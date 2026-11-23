-- 0034_conversions.sql - conversions of single files: what was asked, every
-- gate, check and placement step, the original retired into the recycle
-- folder and what became of it, the files a revision is not offered again,
-- the work areas' leftovers, and the proof of a revision by hand from trials
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The conversion code (src/transcode/axis-conversion*.c) replaces media
-- files and removes their originals, so these tables carry what recovery
-- needs to finish or undo a placement from the records alone:
--
--  - A conversion row is written before anything is made, and every
--    placement step is recorded on it (step, temp_name, the inodes) before or
--    after the step as design.md of add-conversion-execution says.  It keeps
--    the root's path and the file's path itself, so a removed root or file
--    row leaves the history (and what recovery must know) in place.
--  - One conversion in flight per file (conversion_in_flight).
--  - The replaced original is in the volume's recycle folder (the import
--    pipeline's ".axis-recycle"), recorded here with its inode and size; it is
--    removed only when that is still what is there, after recycle_remove_after.
--  - A proof of method 'trials' (an administrator's, from passed trials)
--    names its trials, and the database checks them.

CREATE TABLE conversion (
	id                   bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	-- The file: its index row while it exists, its volume, its root and the
	-- paths (stored form, axis-library-name.h) as they were at request.
	file_id              bigint      REFERENCES library_file (id) ON DELETE SET NULL,
	volume_id            bigint      REFERENCES volume (id) ON DELETE SET NULL,
	root_id              bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	root_path            text        COLLATE "C" NOT NULL,
	rel_path             text        COLLATE "C" NOT NULL,
	-- Where the new file goes: rel_path, or rel_path with the target's
	-- extension.
	new_rel_path         text        COLLATE "C" NOT NULL,
	container            text        NOT NULL,
	revision_id          bigint      NOT NULL REFERENCES conversion_profile_revision (id) ON DELETE RESTRICT,
	outcome              text        NOT NULL,
	state                text        NOT NULL DEFAULT 'queued',
	requested_by         text        NOT NULL,
	requested_at         timestamptz NOT NULL DEFAULT now(),
	verdict              text        NOT NULL,
	-- The source as it was at request.
	src_size             bigint      NOT NULL,
	src_mtime_sec        bigint      NOT NULL,
	src_mtime_nsec       integer     NOT NULL,
	src_ctime_sec        bigint      NOT NULL,
	src_ctime_nsec       integer     NOT NULL,
	src_inode            bigint      NOT NULL,
	src_nlink            bigint      NOT NULL,
	-- The ladder quality recorded for it (quality.h), NULL when none was.
	source_quality       text,
	estimate_bytes       bigint,
	-- The encode: its job, the attempt that ran, the worker and what it ran.
	job_id               bigint,
	finish_job_id        bigint,
	attempt              integer,
	worker               text,
	machine              text,
	encoder              text,
	args                 text,
	-- The output: the name the daemon chose for the attempt that completed,
	-- the size the worker reported, and the stamp after it was settled.
	output_name          text,
	output_reported      bigint,
	output_inode         bigint,
	output_size          bigint,
	output_mtime_sec     bigint,
	output_mtime_nsec    integer,
	output_ctime_sec     bigint,
	output_ctime_nsec    integer,
	-- Placement: the step reached and the temporary name in the source's
	-- folder.
	step                 text,
	temp_name            text,
	-- The original in the recycle folder (path from the volume's top, stored
	-- form), and what became of it.
	recycle_path         text        COLLATE "C",
	recycle_inode        bigint,
	recycle_size         bigint,
	recycle_state        text,
	recycle_remove_after timestamptz,
	recycle_removed_at   timestamptz,
	-- A trial's kept output.
	trial_name           text,
	trial_remove_after   timestamptz,
	trial_state          text,
	-- How it ended.
	failed_check         integer,
	result_text          text,
	finished_at          timestamptz,
	updated_at           timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT conversion_outcome CHECK (outcome IN ('trial', 'replace')),
	CONSTRAINT conversion_state CHECK (state IN ('queued', 'encoding', 'verifying', 'placing', 'done', 'refused',
		'rejected', 'failed', 'cancelled')),
	CONSTRAINT conversion_verdict CHECK (verdict IN ('safe', 'risky', 'unknown')),
	CONSTRAINT conversion_container CHECK (container IN ('mkv', 'mp4')),
	CONSTRAINT conversion_paths CHECK (rel_path <> '' AND new_rel_path <> '' AND rel_path !~ '(^/|/$|//)'
		AND new_rel_path !~ '(^/|/$|//)' AND root_path !~ '(^/|/$|//)'),
	CONSTRAINT conversion_stamp CHECK (src_size >= 0 AND src_mtime_nsec BETWEEN 0 AND 999999999
		AND src_ctime_nsec BETWEEN 0 AND 999999999 AND src_nlink >= 0),
	CONSTRAINT conversion_quality CHECK (source_quality IS NULL OR source_quality ~ '^[a-z0-9_]{1,32}$'),
	CONSTRAINT conversion_names CHECK ((output_name IS NULL OR output_name ~ '^[0-9]{1,19}-[0-9]{1,3}\.tmp$')
		AND (temp_name IS NULL OR temp_name ~ '^\.axis-partial-[0-9]{1,20}$')
		AND (trial_name IS NULL OR trial_name ~ '^trial-[0-9]{1,19}\.(mkv|mp4)$')),
	CONSTRAINT conversion_step CHECK (step IS NULL OR step IN ('placing', 'adopted', 'linked', 'swapped', 'published',
		'retired', 'recorded')),
	CONSTRAINT conversion_recycle CHECK (recycle_state IS NULL OR recycle_state IN ('kept', 'removed', 'gone',
		'changed')),
	CONSTRAINT conversion_trial CHECK (trial_state IS NULL OR trial_state IN ('kept', 'removed', 'gone', 'changed')),
	CONSTRAINT conversion_check_number CHECK (failed_check IS NULL OR failed_check BETWEEN 1 AND 9),
	CONSTRAINT conversion_texts CHECK (char_length(requested_by) BETWEEN 1 AND 200
		AND (result_text IS NULL OR char_length(result_text) <= 2000)
		AND (worker IS NULL OR char_length(worker) <= 64) AND (machine IS NULL OR char_length(machine) <= 80)
		AND (encoder IS NULL OR char_length(encoder) <= 32) AND (args IS NULL OR octet_length(args) <= 65536))
);

-- One conversion of a file in flight at a time.
CREATE UNIQUE INDEX conversion_in_flight ON conversion (file_id)
	WHERE state IN ('queued', 'encoding', 'verifying', 'placing');
CREATE INDEX conversion_file ON conversion (file_id, id DESC);
CREATE INDEX conversion_revision ON conversion (revision_id, outcome, state);
CREATE INDEX conversion_open ON conversion (id) WHERE state IN ('queued', 'encoding', 'verifying', 'placing')
	OR recycle_state = 'kept' OR trial_state = 'kept';

-- Each check of a conversion, with what was measured.
CREATE TABLE conversion_check (
	conversion_id bigint      NOT NULL REFERENCES conversion (id) ON DELETE CASCADE,
	number        integer     NOT NULL,
	result        text        NOT NULL,
	detail        text        NOT NULL,
	checked_at    timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (conversion_id, number),
	CONSTRAINT conversion_check_form CHECK (number BETWEEN 1 AND 9 AND result IN ('passed', 'failed', 'skipped')
		AND char_length(detail) <= 2000)
);

-- A file a revision is not offered again: its output was not worth it, or
-- it was converted with it.
CREATE TABLE conversion_memory (
	file_id       bigint      NOT NULL REFERENCES library_file (id) ON DELETE CASCADE,
	revision_id   bigint      NOT NULL REFERENCES conversion_profile_revision (id) ON DELETE RESTRICT,
	reason        text        NOT NULL,
	conversion_id bigint      REFERENCES conversion (id) ON DELETE SET NULL,
	recorded_at   timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (file_id, revision_id),
	CONSTRAINT conversion_memory_reason CHECK (reason IN ('not_worth_it', 'converted'))
);

-- What a work area held that no conversion accounts for, as the tidy job
-- last listed it; removed after the grace time when it is still the same
-- file (a name not of Axis's forms is listed and never removed).
CREATE TABLE conversion_leftover (
	volume_id     bigint      NOT NULL REFERENCES volume (id) ON DELETE CASCADE,
	name          text        COLLATE "C" NOT NULL,
	inode         bigint      NOT NULL,
	size          bigint      NOT NULL,
	ours          boolean     NOT NULL,
	first_seen_at timestamptz NOT NULL DEFAULT now(),
	last_seen_at  timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (volume_id, name),
	CONSTRAINT conversion_leftover_form CHECK (octet_length(name) BETWEEN 1 AND 1020 AND size >= 0)
);

-- ── Proof by trials ───────────────────────────────────────────────

ALTER TABLE conversion_profile_proof ADD COLUMN trials bigint[];
ALTER TABLE conversion_profile_proof DROP CONSTRAINT conversion_profile_proof_method;
ALTER TABLE conversion_profile_proof ADD CONSTRAINT conversion_profile_proof_method
	CHECK (method IN ('canary', 'carried', 'trials') AND (method = 'carried') = (carried_from IS NOT NULL)
		AND (method = 'trials') = (trials IS NOT NULL)
		AND (method <> 'trials' OR (recorded_by IS NOT NULL AND cardinality(trials) BETWEEN 1 AND 100)));

-- A proof is never changed; a carried one comes from an earlier revision of
-- the same profile; one by trials names only done trials of its own revision
-- that passed every check, on as many different files as it names trials.
CREATE OR REPLACE FUNCTION conversion_profile_proof_check() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF TG_OP = 'UPDATE' THEN
		RAISE EXCEPTION 'a proof is never changed' USING ERRCODE = 'restrict_violation';
	END IF;
	IF NEW.method = 'carried' AND NOT EXISTS (
		SELECT 1 FROM conversion_profile_revision a JOIN conversion_profile_revision b ON b.profile_id = a.profile_id
		WHERE a.id = NEW.carried_from AND b.id = NEW.revision_id AND a.number < b.number) THEN
		RAISE EXCEPTION 'a proof is carried only from an earlier revision of the same profile'
			USING ERRCODE = 'check_violation';
	END IF;
	IF NEW.method = 'trials' THEN
		IF (SELECT count(DISTINCT c.file_id) FROM conversion c
		    WHERE c.id = ANY (NEW.trials) AND c.revision_id = NEW.revision_id AND c.outcome = 'trial'
		      AND c.state = 'done' AND c.file_id IS NOT NULL
		      AND (SELECT count(*) FROM conversion_check k WHERE k.conversion_id = c.id AND k.result = 'passed') = 9)
		   <> cardinality(NEW.trials) THEN
			RAISE EXCEPTION 'a proof by trials names only passed trials of its revision, one per file'
				USING ERRCODE = 'check_violation';
		END IF;
	END IF;
	RETURN NEW;
END $$;
