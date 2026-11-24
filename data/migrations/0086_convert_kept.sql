-- 0086_convert_kept.sql - the choice to convert the files Axis keeps, stored
-- with a conversion run and with a single conversion
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- (convert-kept-files.)
--
-- A file kept by hand, or brought over from Radarr or Sonarr, is not replaced
-- by another release.  A conversion is a different thing, guarded by its own
-- conditions and ten checks, so an administrator may choose, per run or per
-- single conversion, to convert kept files too.  The mark itself (movie_file
-- .kept, series_file.kept) is never read or written through these columns: a
-- converted file is as kept as its original was.
--
--   conversion_run   convert_kept: the run's list includes kept files, and
--                    every conversion it asks for carries the choice.
--                    convert_kept_by, convert_kept_at: who chose it and when
--                    (both set exactly when the choice is on).
--                    relist_state: set while kept files are being added to a
--                    run that had a list already: the state the run goes back
--                    to when the job ends ('ready', 'paused' or 'finished');
--                    NULL for a run whose first list is being made.
--
--   conversion       convert_kept: the person (or the run) chose to convert
--                    the file although it is kept; the condition "kept" is
--                    read with it at the request, when a worker takes the
--                    work and just before the original is replaced.
--
-- Only adds columns with defaults: safe on a live database; every row there
-- was keeps the behaviour it had (the choice is off).

ALTER TABLE conversion_run
	ADD COLUMN convert_kept    boolean     NOT NULL DEFAULT false,
	ADD COLUMN convert_kept_by text,
	ADD COLUMN convert_kept_at timestamptz,
	ADD COLUMN relist_state    text,
	ADD CONSTRAINT conversion_run_convert_kept CHECK (
		convert_kept = (convert_kept_by IS NOT NULL)
		AND convert_kept = (convert_kept_at IS NOT NULL)
		AND (convert_kept_by IS NULL OR char_length(convert_kept_by) BETWEEN 1 AND 200)),
	ADD CONSTRAINT conversion_run_relist CHECK (
		relist_state IS NULL OR (relist_state IN ('ready', 'paused', 'finished') AND state = 'listing' AND convert_kept));

ALTER TABLE conversion
	ADD COLUMN convert_kept boolean NOT NULL DEFAULT false;
