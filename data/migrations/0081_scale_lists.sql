-- 0081_scale_lists.sql - indexes for the Jobs list and the searches that
-- never finished (scale-database-second-batch)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Measured on the generated database of tests/test-db-scale.c (2,000,000
-- jobs, 145,000 searches of series; the figures are in
-- openspec/changes/scale-database-second-batch/design.md).
--
-- 1. The Jobs list shows the newest jobs that pass its filters: a state, a
--    queue, and the kinds that are work or upkeep.  Ordered by id alone, a
--    page walked the table until enough jobs passed, which for a filter
--    that few jobs pass (the failed ones, a quiet queue, the work among the
--    upkeep) was every job kept: about a second at 2,000,000.  In this index
--    the jobs of one kind, queue and state lie together, newest last: a page
--    is the newest few of each such group, and the groups themselves (some
--    hundreds at most) are found by stepping through the index.  The names
--    of the queues, which the page's filter offers, come from the same
--    steps.
--
-- 2. The follow-up of each module asks every minute for the searches that
--    never finished.  They are none, or a few; they were looked for by
--    reading every search kept.
--
-- Applying it: as 0080.  CREATE INDEX blocks writes to its table while it
-- builds (reads go on) and the lock lasts until the migration's transaction
-- ends.  The daemon applies migrations before it starts anything that
-- writes.  On the database above job_list_idx took about three seconds to
-- build and is 105 MB (design.md).

-- 1. The jobs of a kind, a queue and a state, by id.
CREATE INDEX job_list_idx ON job (kind, queue, state, id);

-- 2. The searches that have not finished.
CREATE INDEX series_search_unfinished ON series_search (started_at) WHERE finished_at IS NULL;
CREATE INDEX movie_search_unfinished ON movie_search (started_at) WHERE finished_at IS NULL;
