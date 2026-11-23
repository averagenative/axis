-- 0077_attention_sources.sql - what the further notices of the page "Needs
-- attention" read: two stored counts and five partial indexes
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Slice N2 of add-attention-notices gives the page a notice for each of the
-- other states that wait on a person.  A notice is still worked out from the
-- state Axis keeps and never stored; a reading of the notices runs one small
-- statement a source, under a deadline, and none over a whole table.  These
-- are what that needs:
--
--   series_search.n_person_only
--                        how many of a search's releases no search takes by
--                        itself and a person may (Grab this one, from a
--                        manual search): numbered beyond the series' data, in
--                        the place of a file kept by hand, an anime release
--                        whose absolute number is read too weakly or that was
--                        downloaded before without a file for an episode.
--                        Written with the search's candidates, as
--                        n_mismatched is.  The reason itself is only inside
--                        series_candidate.reasons, a jsonb with no index.
--                        Searches made before this have 0: their releases are
--                        counted again by the next search of their episode.
--
--   attention_count      a number a job leaves for a notice, where working it
--                        out is too much for a reading: one row for each
--                        kind, with when it was counted.  Today one kind,
--                        'duplicates': the movies and episodes with more than
--                        one file, which the Duplicates page works out on
--                        each view; the hourly import cleanup counts them.
--                        It holds no notice, no name and no path.
--
--   import_waiting       the imports that wait (for Read-only storage to be
--                        switched off, a disk, room): the few the notice
--                        about Read-only storage counts.
--   library_identity_review, library_identity_doubt
--                        the movies and series folders found in the library
--                        that wait for a person to say which title they are,
--                        and those Axis linked by itself and is no longer
--                        sure of: both few, in a table of one row for each
--                        movie and series folder of the library.
--   job_failed_idx       the jobs that failed for good, by when: a handful
--                        among every job that ended.
--   youtube_video_failed the YouTube videos whose download failed, among
--                        every video listed.
--
-- Safe on a live database: the column is added with a constant default (no
-- rewrite), the table is new, and each index is built over a table of at most
-- one row for each import, each movie or series folder, or each job kept,
-- under a lock held for that long (none is over the index of files).  An
-- older daemon reads and writes none of this.

ALTER TABLE series_search ADD COLUMN n_person_only integer NOT NULL DEFAULT 0;

CREATE TABLE attention_count (
	kind       text        PRIMARY KEY,
	count      bigint      NOT NULL,
	counted_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT attention_count_form CHECK (kind ~ '^[a-z][a-z0-9_.]{0,62}$' AND count >= 0)
);

CREATE INDEX import_waiting ON import (id) WHERE state = 'waiting';
CREATE INDEX library_identity_review ON library_identity (root_id) WHERE state = 'review';
CREATE INDEX library_identity_doubt ON library_identity (id) WHERE doubt IS NOT NULL;
CREATE INDEX job_failed_idx ON job (finished_at) WHERE state = 'failed';
CREATE INDEX youtube_video_failed ON youtube_video (finished_at) WHERE state = 'failed';
