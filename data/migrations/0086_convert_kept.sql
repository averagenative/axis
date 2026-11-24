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
--   conversion_run_entry
--                    convert_kept: the run's choice covers this file: it was
--                    kept when the list was made, or when the kept files were
--                    added to it, by a run with the choice.  A file that
--                    becomes kept afterwards is not covered: the condition
--                    "kept" refuses it when its turn comes, as without the
--                    choice.
--                    confirmed_by, confirmed_at: for a file added to a run
--                    that replaces originals, the administrator who confirmed
--                    that its original is replaced, and when (the run's own
--                    confirmed_by is for the files it had then); NULL for
--                    every other entry.
--
--   conversion       convert_kept: the choice covered the file when the
--                    conversion was asked for: the person (or the run) chose
--                    to convert kept files and the file was kept then.  The
--                    condition "kept" is read with it when a worker takes the
--                    work and just before the original is replaced, so a file
--                    that becomes kept after the request is refused there as
--                    without the choice.
--
-- Only adds columns with defaults: safe on a live database; every row there
-- was keeps the behaviour it had (the choice is off).  The entries' check is
-- added NOT VALID: every row there has the two new columns NULL, so nothing
-- needs reading, and the check holds for every row written from now on.

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

ALTER TABLE conversion_run_entry
	ADD COLUMN convert_kept boolean     NOT NULL DEFAULT false,
	ADD COLUMN confirmed_by text,
	ADD COLUMN confirmed_at timestamptz,
	ADD CONSTRAINT conversion_run_entry_confirmed CHECK (
		(confirmed_by IS NULL) = (confirmed_at IS NULL)
		AND (confirmed_by IS NULL OR char_length(confirmed_by) BETWEEN 1 AND 200)) NOT VALID;

ALTER TABLE conversion_run_entry VALIDATE CONSTRAINT conversion_run_entry_confirmed;

ALTER TABLE conversion
	ADD COLUMN convert_kept boolean NOT NULL DEFAULT false;
