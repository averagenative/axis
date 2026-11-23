-- 0055_manual_search.sql - what add-manual-search records
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- movie_search.cause, series_search.cause: "list" is a manual search, a
-- person's look: it asks every indexer and stores every release it found,
-- and grabs nothing by itself.  It ends with the outcome "listed".  The
-- title's state and its automatic searches read the last search that is
-- not one.
--
-- movie_grab.set_aside, series_grab.set_aside: a person set the quality
-- profile aside for this grab: "anyway" (Grab it anyway on a release the
-- profile rejected) or "hand" (Grab this one, from a manual search).  Only
-- such a grab's import sets the profile aside, and for a series only for
-- the files whose episodes the grab claimed.  NULL for every other grab,
-- every grab made before this version included (manual alone, which an
-- administrator's accepted Grab sets too, waives nothing).
--
-- movie_grab.replaces_files, series_grab.replaces_files: the files in the
-- library a person ticked replacing although the release is not better
-- than them, as the confirmation showed them ([{"f": library_file id,
-- "q": quality name or null, "r": resolution or null}]).  The import
-- replaces a file it is not better than only when it is one of these.
--
-- movie_grab.numbering_ok, series_grab.numbering_ok: the person ticked
-- taking a release that numbers its episodes differently from the series'
-- data (series_candidate.missing).  Its import places a file that holds a
-- claimed episode and a number the data does not have for the claimed
-- episode alone, and notes the other number; without it, never.
--
-- series_candidate.missing: the episodes a release names that the series'
-- data does not have ("S01E06"), when it names some it has too.  It is
-- never grabbed by a search, nor by Grab it anyway; Grab this one takes it
-- for those it has, with the tick above.
--
-- series_grab.close_reason: "not_imported", a grab by hand let go because
-- its import could take no file of the download (its episodes are free
-- again; the import and the download are left for a person).

ALTER TABLE movie_search DROP CONSTRAINT movie_search_cause;
ALTER TABLE movie_search ADD CONSTRAINT movie_search_cause
	CHECK (cause IN ('added', 'manual', 'scheduled', 'retry', 'profile', 'list'));
ALTER TABLE movie_search DROP CONSTRAINT movie_search_outcome;
ALTER TABLE movie_search ADD CONSTRAINT movie_search_outcome CHECK (outcome IS NULL OR outcome IN ('grabbed',
	'nothing_found', 'none_acceptable', 'exhausted', 'no_indexer', 'indexer_error', 'no_client', 'client_error',
	'root_offline', 'profile_unusable', 'metadata_missing', 'in_flight', 'failed', 'listed'));

ALTER TABLE series_search DROP CONSTRAINT series_search_cause;
ALTER TABLE series_search ADD CONSTRAINT series_search_cause
	CHECK (cause IN ('added', 'manual', 'scheduled', 'retry', 'profile', 'list'));
ALTER TABLE series_search DROP CONSTRAINT series_search_outcome;
ALTER TABLE series_search ADD CONSTRAINT series_search_outcome CHECK (outcome IS NULL OR outcome IN ('grabbed',
	'nothing_found', 'none_acceptable', 'exhausted', 'no_indexer', 'indexer_error', 'no_client', 'client_error',
	'root_offline', 'profile_unusable', 'metadata_missing', 'in_flight', 'failed', 'listed'));

ALTER TABLE movie_grab ADD COLUMN set_aside text;
ALTER TABLE movie_grab ADD COLUMN replaces_files jsonb NOT NULL DEFAULT '[]';
ALTER TABLE movie_grab ADD COLUMN numbering_ok boolean NOT NULL DEFAULT false;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_set_aside CHECK ((set_aside IS NULL OR set_aside IN ('anyway',
	'hand')) AND jsonb_typeof(replaces_files) = 'array');

ALTER TABLE series_grab ADD COLUMN set_aside text;
ALTER TABLE series_grab ADD COLUMN replaces_files jsonb NOT NULL DEFAULT '[]';
ALTER TABLE series_grab ADD COLUMN numbering_ok boolean NOT NULL DEFAULT false;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_set_aside CHECK ((set_aside IS NULL OR set_aside IN ('anyway',
	'hand')) AND jsonb_typeof(replaces_files) = 'array');
ALTER TABLE series_grab DROP CONSTRAINT series_grab_close_reason;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone', 'not_imported'));

ALTER TABLE series_candidate ADD COLUMN missing text;
ALTER TABLE series_candidate ADD CONSTRAINT series_candidate_missing
	CHECK (missing IS NULL OR char_length(missing) BETWEEN 1 AND 200);
