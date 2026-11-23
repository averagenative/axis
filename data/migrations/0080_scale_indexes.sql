-- 0080_scale_indexes.sql - indexes for a large library and a long history
-- (scale-database-first-batch)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Measured on a generated database of 5,000 series, 300,000 episodes,
-- 20,000 movies, 400,000 library files, 1,000,000 stored search results,
-- 250,000 grabs, 300,000 imports with 3,000,000 files, 3,000,000 history
-- events and 2,000,000 jobs (tests/test-db-scale.c; the figures are in
-- openspec/changes/scale-database-first-batch/design.md).
--
-- 1. The jobs that wait or run are a few hundred among millions that ended.
--    Every count of them (the header's, at each change of a job's state; the
--    probe queue's top-up; the metrics) read the whole table: no index held
--    both states.  job_live_idx holds exactly those rows.
--
-- 2. and 3. A foreign key with ON DELETE SET NULL or CASCADE makes the
--    database look for the referencing rows at every delete of a parent
--    row.  Without an index on the referencing column that is a read of the
--    whole referencing table, once per parent row deleted: deleting a
--    thousand stored search results read the grabs a thousand times.  Where
--    most rows hold NULL the index is partial (the look-up "column = value"
--    implies "column IS NOT NULL", so it is used all the same).
--
-- 4. The list of imports that ended is ordered by when they ended.
--
-- 5. The job table changes all day (every job is written four times or
--    more) while most of its rows never change again.  With the default
--    thresholds (a fifth of the table) a table of millions waits for
--    hundreds of thousands of dead rows before it is vacuumed or analysed;
--    a fiftieth keeps its small partial indexes and its statistics fresh.
--
-- Applying it: CREATE INDEX holds a lock that blocks writes to its table
-- while it builds (reads go on), and a migration is one transaction: the
-- locks are held until the last statement ends.  On the database above the
-- whole file applied in a few seconds (design.md has each index's time).
-- The daemon runs migrations before it starts anything that writes, so
-- nothing of its own waits; the cost is a start-up a few seconds longer,
-- once.

-- 1. Live jobs.
CREATE INDEX job_live_idx ON job (kind, queue) WHERE state IN ('queued', 'running');
-- When the last probe of a file ended (whether an Analysis refresh is due,
-- asked every five minutes: src/library/axis-analysis.c).  With no probe
-- among the jobs kept, which is the usual state of a library probed long
-- ago, the answer was found by walking every finished job, newest first.
-- The index holds the probes only: nothing where there are none.
CREATE INDEX job_probe_ended_idx ON job (finished_at) WHERE kind = 'probe.file' AND state = 'succeeded';

-- 2. What a deleted search result, search or episode was grabbed from.
CREATE INDEX series_grab_candidate ON series_grab (candidate_id) WHERE candidate_id IS NOT NULL;
CREATE INDEX series_grab_search ON series_grab (search_id) WHERE search_id IS NOT NULL;
CREATE INDEX series_grab_for_episode ON series_grab (episode_id) WHERE episode_id IS NOT NULL;
CREATE INDEX movie_grab_candidate ON movie_grab (candidate_id) WHERE candidate_id IS NOT NULL;
CREATE INDEX movie_grab_search ON movie_grab (search_id) WHERE search_id IS NOT NULL;

-- 3. What else names a library file, an episode or a title that is deleted.
CREATE INDEX import_file_library_file ON import_file (library_file_id) WHERE library_file_id IS NOT NULL;
CREATE INDEX estimate_sample_file ON estimate_sample (file_id) WHERE file_id IS NOT NULL;
CREATE INDEX series_event_episode ON series_event (episode_id) WHERE episode_id IS NOT NULL;
CREATE INDEX series_event_title ON series_event (title_id) WHERE title_id IS NOT NULL;
-- (Also the history of a movie that was removed: title_id = $1 AND movie_id
-- IS NULL ORDER BY id DESC, src/modules/movies/axis-movies-store.c.)
CREATE INDEX movie_event_title ON movie_event (title_id, id DESC) WHERE title_id IS NOT NULL;
CREATE INDEX import_title ON import (title_id) WHERE title_id IS NOT NULL;
-- (Also the download a claim sent and never handed over: module = and
-- title_id = and release_name =, src/acquire/axis-acquire-follow.c.  The two
-- indexes on title_id that exist hold only the downloads still at a client.)
CREATE INDEX download_title ON download (title_id) WHERE title_id IS NOT NULL;
CREATE INDEX library_identity_action_title ON library_identity_action (title_id) WHERE title_id IS NOT NULL;

-- 4. The imports that ended, the latest first.
CREATE INDEX import_ended ON import (finished_at DESC, id DESC) WHERE state IN ('imported', 'dismissed');

-- 5. The job table is vacuumed and analysed at a fiftieth, not a fifth.
ALTER TABLE job SET (autovacuum_vacuum_scale_factor = 0.02, autovacuum_analyze_scale_factor = 0.02);

-- 6. Indexes that nothing can use, each written at every row stored.
--
-- The two tables of stored search results each have UNIQUE (search_id,
-- position), whose own index has exactly the columns of these two.
DROP INDEX series_candidate_search;
DROP INDEX movie_candidate_search;
-- It holds only the episodes that have the provider's id of them (WHERE
-- provider_id IS NOT NULL), and no statement asks for an episode by that id:
-- every one looks an episode up by its title and place, or its title and
-- day, which the primary key and meta_episode_air_date serve.
DROP INDEX meta_episode_provider_id;
