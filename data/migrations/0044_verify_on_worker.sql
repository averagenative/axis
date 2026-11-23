-- 0044_verify_on_worker.sql - a converted file's checks measured on a worker
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The checks of a converted file (design.md of verify-on-worker) are decided
-- by the daemon, but the measurements of four of them (the full decode, the
-- picture's packets, the copied tracks' data, the picture comparison) may be
-- made by a worker, as a convert.verify job.  What the daemon keeps of that:
--
--  - checked_where: where the measurements were made ('server' or 'worker'),
--    set when the daemon measures or hands them out;
--  - verify_job_id: the convert.verify job; verifier: the worker whose
--    measurements were taken;
--  - check_plan: what the worker was asked to measure (the plan's JSON as
--    the protocol writes it), and check_source_sample: the digest of samples
--    of the original the daemon read from its own disk when it handed the
--    work out;
--  - handed_*: the converted file's stamp at that moment, which it must
--    still have when the measurements are used (a change time that moved
--    means something wrote it);
--  - evidence: the measurements as received (the protocol's JSON, written
--    again after it was read strictly), and evidence_at: when;
--  - checks_waiting: why the checks wait now (no worker, conversion work
--    paused since the daemon started), NULL when they do not.

ALTER TABLE conversion
	ADD COLUMN checked_where       text,
	ADD COLUMN verify_job_id       bigint,
	ADD COLUMN verifier            text,
	ADD COLUMN check_plan          text,
	ADD COLUMN check_source_sample text,
	ADD COLUMN handed_inode        bigint,
	ADD COLUMN handed_size         bigint,
	ADD COLUMN handed_mtime_sec    bigint,
	ADD COLUMN handed_mtime_nsec   integer,
	ADD COLUMN handed_ctime_sec    bigint,
	ADD COLUMN handed_ctime_nsec   integer,
	ADD COLUMN evidence            text,
	ADD COLUMN evidence_at         timestamptz,
	ADD COLUMN checks_waiting      text,
	ADD CONSTRAINT conversion_checked_where CHECK (checked_where IS NULL OR checked_where IN ('server', 'worker')),
	ADD CONSTRAINT conversion_verifier_form CHECK (verifier IS NULL OR verifier ~ '^[a-z][a-z0-9-]{0,47}$'),
	ADD CONSTRAINT conversion_check_sample_form
		CHECK (check_source_sample IS NULL OR check_source_sample ~ '^[0-9a-f]{64}$'),
	ADD CONSTRAINT conversion_handed_complete
		CHECK ((handed_inode IS NULL) = (handed_size IS NULL) AND (handed_size IS NULL) = (handed_mtime_sec IS NULL)
			AND (handed_mtime_sec IS NULL) = (handed_ctime_sec IS NULL)
			AND (handed_mtime_nsec IS NULL OR handed_mtime_nsec BETWEEN 0 AND 999999999)
			AND (handed_ctime_nsec IS NULL OR handed_ctime_nsec BETWEEN 0 AND 999999999)),
	ADD CONSTRAINT conversion_check_texts
		CHECK ((check_plan IS NULL OR octet_length(check_plan) <= 65536)
			AND (evidence IS NULL OR octet_length(evidence) <= 262144)
			AND (checks_waiting IS NULL OR (char_length(checks_waiting) BETWEEN 1 AND 500
				AND checks_waiting !~ '[\x01-\x1f\x7f]')));

CREATE INDEX conversion_verify_job ON conversion (verify_job_id) WHERE verify_job_id IS NOT NULL;
