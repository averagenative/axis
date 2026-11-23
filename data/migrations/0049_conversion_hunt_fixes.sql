-- 0049_conversion_hunt_fixes.sql - what fix-conversion-hunt-findings records
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- conversion.passed_*: the converted file as it was when every one of its
-- checks passed (its stamp, the daemon's digest of samples of it, and, when
-- the measurements were a worker's bound by the whole file, that file's
-- SHA-256).  A finish job tried again after that (a scan held the volume, a
-- database error, a daemon that stopped) judges nothing again for the same
-- file: it runs the light checks (the file asked for, the original
-- unchanged, worth it, the gates) and places it.  The stamp is compared as
-- it was then, as Axis left it after settling it (output_*), or, after a
-- stop between settling and that record, in the one shape settling gives
-- (the original's modification time); anything else is checked from the
-- start.
--
-- conversion.claims: how many times the encode was handed to a worker; the
-- output's name is <id>-<claims>.tmp, so that every claim has a name of its
-- own (a give-back uses no attempt of the job, and a name left by an earlier
-- claim never blocks the next).  Before 0049 the name was <id>-<attempt>.tmp,
-- the attempt recorded at each claim, which only grows: a conversion of
-- before starts at its recorded attempt, so that its next claim takes a name
-- none of its earlier claims can have left.
--
-- conversion.plan_caps, plan_preset, plan_threads: what the plan given to
-- the worker was made from besides the file and the revision (the worker's
-- measured encoders as JSON, the software preset and threads).  Check 3 makes
-- the plan again from these, not from the settings and the capabilities as
-- they are when the checks run.
--
-- conversion.retryable: the conversion ended without a judgement of the
-- converted file, for a reason that is neither the file's nor the
-- revision's (the plan made again from its records differs from the one the
-- worker was given: Axis itself changed).  It is not a rejection by a check
-- (proving by hand does not count it); a conversion run asks for the file
-- again.
--
-- conversion.set_aside_*: a trial rejected by a check of the converted file
-- (3 to 8) that an administrator set aside, with who, when and why (a check
-- found wrong since, say): it counts neither for nor against proving its
-- revision, and its page says so.  Nothing else can be set aside.
--
-- conversion.checks_version: the version of the checks' rules the build
-- that judged the conversion had (transcode/axis-conversion.h); NULL for one
-- judged before versions were recorded, which counts as earlier than any.
-- The build's marks say which rules changed in which direction since
-- (axis-conversion-rules.c): a rejection by a check found wrongly strict
-- after it was judged, and a pass judged before a check was found too
-- lenient, stop counting for proving.
--
-- conversion_run_entry.asked_at: when the step marked the entry asked; a
-- request that committed but was not linked yet is linked only to a
-- conversion requested since then (an entry that moved from trials to
-- replacing has its trial too).

ALTER TABLE conversion
	ADD COLUMN passed_at         timestamptz,
	ADD COLUMN passed_inode      bigint,
	ADD COLUMN passed_size       bigint,
	ADD COLUMN passed_mtime_sec  bigint,
	ADD COLUMN passed_mtime_nsec integer,
	ADD COLUMN passed_ctime_sec  bigint,
	ADD COLUMN passed_ctime_nsec integer,
	ADD COLUMN passed_sample     text,
	ADD COLUMN passed_digest     text,
	ADD COLUMN claims            integer NOT NULL DEFAULT 0,
	ADD COLUMN plan_caps         text,
	ADD COLUMN plan_preset       text,
	ADD COLUMN plan_threads      integer,
	ADD COLUMN retryable         boolean NOT NULL DEFAULT false,
	ADD COLUMN set_aside_at      timestamptz,
	ADD COLUMN set_aside_by      text,
	ADD COLUMN set_aside_reason  text,
	ADD COLUMN checks_version    integer,
	ADD CONSTRAINT conversion_set_aside_complete
		CHECK ((set_aside_at IS NULL) = (set_aside_by IS NULL) AND (set_aside_by IS NULL) = (set_aside_reason IS NULL)),
	ADD CONSTRAINT conversion_set_aside_what
		CHECK (set_aside_at IS NULL OR coalesce(outcome = 'trial' AND state = 'rejected' AND failed_check BETWEEN 3 AND 8,
			false)),
	ADD CONSTRAINT conversion_set_aside_texts
		CHECK ((set_aside_by IS NULL OR char_length(set_aside_by) BETWEEN 1 AND 200)
			AND (set_aside_reason IS NULL OR (char_length(set_aside_reason) BETWEEN 1 AND 500
				AND set_aside_reason !~ '[\x01-\x1f\x7f]'))),
	ADD CONSTRAINT conversion_checks_version_range CHECK (checks_version IS NULL OR checks_version BETWEEN 0 AND 10000),
	ADD CONSTRAINT conversion_passed_complete
		CHECK ((passed_at IS NULL) = (passed_inode IS NULL) AND (passed_inode IS NULL) = (passed_size IS NULL)
			AND (passed_size IS NULL) = (passed_mtime_sec IS NULL) AND (passed_mtime_sec IS NULL) = (passed_mtime_nsec IS NULL)
			AND (passed_mtime_nsec IS NULL) = (passed_ctime_sec IS NULL)
			AND (passed_ctime_sec IS NULL) = (passed_ctime_nsec IS NULL) AND (passed_ctime_nsec IS NULL) = (passed_sample IS NULL)),
	ADD CONSTRAINT conversion_passed_forms
		CHECK ((passed_sample IS NULL OR passed_sample ~ '^[0-9a-f]{64}$')
			AND (passed_digest IS NULL OR passed_digest ~ '^[0-9a-f]{64}$')
			AND (passed_mtime_nsec IS NULL OR passed_mtime_nsec BETWEEN 0 AND 999999999)
			AND (passed_ctime_nsec IS NULL OR passed_ctime_nsec BETWEEN 0 AND 999999999)),
	ADD CONSTRAINT conversion_claims_range CHECK (claims BETWEEN 0 AND 999),
	ADD CONSTRAINT conversion_plan_inputs
		CHECK ((plan_caps IS NULL OR octet_length(plan_caps) <= 65536)
			AND (plan_preset IS NULL OR plan_preset ~ '^[a-z]{1,16}$')
			AND (plan_threads IS NULL OR plan_threads BETWEEN 0 AND 64));

UPDATE conversion SET claims = least(greatest(coalesce(attempt, 0), 0), 999) WHERE coalesce(attempt, 0) > 0;

ALTER TABLE conversion_run_entry
	ADD COLUMN asked_at timestamptz;
