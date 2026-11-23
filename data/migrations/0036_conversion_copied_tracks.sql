-- 0036_conversion_copied_tracks.sql - a tenth check of a converted file: the
-- tracks the plan copies are bit for bit the original's
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The new check comes after "Nothing was lost in time" (6), so the checks
-- that were 7, 8 and 9 (the same picture, worth it, the gates) become 8, 9
-- and 10, in the recorded checks and in the number of the check that
-- rejected a conversion.  A conversion checked before this has no check 7:
-- its page shows it as not run.  A proof by trials now needs trials that
-- passed all ten checks, so a trial checked before this cannot be one of
-- them (it is run again).

ALTER TABLE conversion_check DROP CONSTRAINT conversion_check_form;
ALTER TABLE conversion DROP CONSTRAINT conversion_check_number;

-- Two steps: the key is (conversion_id, number).
UPDATE conversion_check SET number = number + 100 WHERE number BETWEEN 7 AND 9;
UPDATE conversion_check SET number = number - 99 WHERE number BETWEEN 107 AND 109;
UPDATE conversion SET failed_check = failed_check + 1 WHERE failed_check BETWEEN 7 AND 9;

ALTER TABLE conversion_check ADD CONSTRAINT conversion_check_form CHECK (number BETWEEN 1 AND 10
	AND result IN ('passed', 'failed', 'skipped') AND char_length(detail) <= 2000);
ALTER TABLE conversion ADD CONSTRAINT conversion_check_number CHECK (failed_check IS NULL
	OR failed_check BETWEEN 1 AND 10);

-- As before, with ten checks passed instead of nine.
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
		      AND (SELECT count(*) FROM conversion_check k WHERE k.conversion_id = c.id AND k.result = 'passed') = 10)
		   <> cardinality(NEW.trials) THEN
			RAISE EXCEPTION 'a proof by trials names only passed trials of its revision, one per file'
				USING ERRCODE = 'check_violation';
		END IF;
	END IF;
	RETURN NEW;
END $$;
