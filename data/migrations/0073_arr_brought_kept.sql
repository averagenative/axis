-- 0073_arr_brought_kept.sql - a file brought over from Radarr or Sonarr is
-- kept: Axis never upgrades or replaces it by itself until a person lets it go
-- (import-from-arr, the owner's answer of 2026-11-21 to question 2: "They
-- shouldn't be auto upgraded")
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Until now a link made by bringing over (hand_how 'brought') had kept false,
-- and was upgraded by the ordinary rules once searching was started; and a
-- file of the app's that identification had linked first was left as
-- identification's link, not marked at all.  From now on the run records
-- either with hand_how 'brought' and kept true, and this migration does the
-- same for what was brought before it, so that what is in Axis already is
-- protected too.
--
--   movie_file.kept_how, series_file.kept_how  NULL, or 'brought': why the
--                     file is kept, where no person chose it.  'brought' is
--                     set with kept by the run and by this migration, and
--                     only on a kept row (the CHECK).  A person who keeps the
--                     file by hand, or lets it go, on its title's page clears
--                     it: the keeping (or not) is theirs from then on.  So
--                     does every unlinking, handing back and linking by hand.
--                     "Let Axis upgrade these files" on a connection's page
--                     lets go only of the links that still have hand_how
--                     'brought', kept true and kept_how 'brought': never a
--                     link a person kept or linked by hand since.
--   arr_source.run_note  NULL, or what the last run said about starting the
--                     searches at its end (the tick on the run's
--                     confirmation): fixed words and counts, no title.
--   arr_brought.run_job  the job (job.id, no foreign key: jobs are pruned)
--                     of the run that last brought the title or linked a file
--                     of it.  A run confirmed with the tick starts the
--                     searches of its own titles only, known by this.
--
-- What the data changes do, and what they cannot know:
--
--  1. Every link with hand_how 'brought' and kept false becomes kept, with
--     kept_how 'brought'.  A brought file a person kept by hand and then let
--     go again before this migration looks exactly like the old default (kept
--     false, hand_how 'brought'): the two cannot be told apart, and such a
--     file is kept again.  The person lets it go once more on its title's
--     page.  A brought row a person keeps now (kept true) is left as it is:
--     the keeping is theirs.
--  2. arr_brought records what became of a movie's file ('already': it was
--     linked to the movie already) and, for a series, how many of its files
--     were ("already" in counts), but not which library file that was.  So
--     this is done by title: for a movie whose row says 'already', and for a
--     series whose counts say at least one, every present video file of that
--     title that only identification linked (no hand stamp, no import, not
--     kept) is marked as brought over and kept.  That can mark a file of the
--     title that the app never had (a second copy identification found): it
--     errs on the side the owner asked for, kept, and the per-file control
--     lets it go.  hand_by is who ran the bringing over, where recorded.
--     A series whose counts also say that some files "differ" (the app has
--     them on other episodes than Axis) is not stamped by title: which files
--     those are cannot be known here, so a later run stamps the rest file by
--     file.
--
-- Safe on a live database: the new columns have no default (no row is
-- rewritten), and each CHECK is added NOT VALID and validated after, so each
-- table is read once to check it.  The UPDATEs touch only rows of what was
-- brought over: none on an installation that never brought anything over, and
-- otherwise one row a file (thousands at most), each a small row.  The
-- migration runs in one transaction, so the locks of these statements are held
-- to its commit; reads and writes of movie_file and series_file wait for it, a
-- moment's wait at these tables' sizes.

ALTER TABLE movie_file ADD COLUMN kept_how text;
ALTER TABLE movie_file ADD CONSTRAINT movie_file_kept_how CHECK ((kept_how IS NULL OR kept_how IN ('brought'))
	AND (kept_how IS NULL OR kept)) NOT VALID;
ALTER TABLE movie_file VALIDATE CONSTRAINT movie_file_kept_how;

ALTER TABLE series_file ADD COLUMN kept_how text;
ALTER TABLE series_file ADD CONSTRAINT series_file_kept_how CHECK ((kept_how IS NULL OR kept_how IN ('brought'))
	AND (kept_how IS NULL OR kept)) NOT VALID;
ALTER TABLE series_file VALIDATE CONSTRAINT series_file_kept_how;

-- 1. What was linked by bringing over.
UPDATE movie_file SET kept = true, kept_how = 'brought' WHERE hand_how = 'brought' AND kept = false;
UPDATE series_file SET kept = true, kept_how = 'brought' WHERE hand_how = 'brought' AND kept = false;

-- 2. What identification had linked first, by title.
UPDATE movie_file mf SET hand_how = 'brought', hand_at = now(), identity_id = NULL, kept = true, kept_how = 'brought',
	hand_by = (SELECT b.brought_by FROM arr_brought b WHERE b.movie_id = mf.movie_id AND b.file = 'already'
		ORDER BY b.id LIMIT 1)
	FROM library_file lf
	WHERE lf.id = mf.file_id AND lf.state = 'present' AND lf.kind = 'video'
	AND mf.hand_at IS NULL AND mf.import_id IS NULL AND NOT mf.kept
	AND EXISTS (SELECT 1 FROM arr_brought b WHERE b.movie_id = mf.movie_id AND b.file = 'already');
UPDATE series_file sf SET hand_how = 'brought', hand_at = now(), identity_id = NULL, kept = true, kept_how = 'brought',
	read_source = (SELECT s.episode_source FROM series s WHERE s.id = sf.series_id),
	hand_by = (SELECT b.brought_by FROM arr_brought b WHERE b.series_id = sf.series_id
		AND coalesce((b.counts ->> 'already')::integer, 0) > 0
		AND coalesce((b.counts ->> 'differs')::integer, 0) = 0 ORDER BY b.id LIMIT 1)
	FROM library_file lf
	WHERE lf.id = sf.file_id AND lf.state = 'present' AND lf.kind = 'video'
	AND sf.hand_at IS NULL AND sf.import_id IS NULL AND NOT sf.kept
	AND EXISTS (SELECT 1 FROM arr_brought b WHERE b.series_id = sf.series_id
		AND coalesce((b.counts ->> 'already')::integer, 0) > 0
		AND coalesce((b.counts ->> 'differs')::integer, 0) = 0);

ALTER TABLE arr_source ADD COLUMN run_note text;
ALTER TABLE arr_source ADD CONSTRAINT arr_source_run_note CHECK (run_note IS NULL OR char_length(run_note) <= 1000)
	NOT VALID;
ALTER TABLE arr_source VALIDATE CONSTRAINT arr_source_run_note;

ALTER TABLE arr_brought ADD COLUMN run_job bigint;
