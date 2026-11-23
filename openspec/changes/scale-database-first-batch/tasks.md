# Tasks

## 1. The database at the design scale, and the measuring

- [x] 1.1 The pool tells an observer every statement its connections send (`axis_db_pool_set_statement_observer()`).
- [x] 1.2 `tests/test-db-scale.c`: the database made by `generate_series`, at `AXIS_DB_SCALE` percent of the design scale (10 by default, 100 by hand), then `VACUUM ANALYZE`.
- [x] 1.3 A function of the daemon is called, the statement it sent is explained with the values it was sent with; a statement that changes rows in a transaction rolled back.
- [x] 1.4 Before and after in one run: the new indexes are dropped, the measurements taken, each index built and timed, the measurements taken again (`/db/scale/fixes`).
- [x] 1.5 What is only measured (`/db/scale/report`).

## 2. Fixes, each measured before and after

- [x] 2.1 `job_live_idx`: the jobs that wait or run. Every statement of that shape checked: none reads the whole table after.
- [x] 2.2 `job_probe_ended_idx`: when the last probe ended, for the check whether an Analysis refresh is due.
- [x] 2.3 Grabs by stored result, search and episode (`series_grab_candidate`, `series_grab_search`, `series_grab_for_episode`, `movie_grab_candidate`, `movie_grab_search`).
- [x] 2.4 The other foreign keys (`import_file_library_file`, `estimate_sample_file`, `series_event_episode`, `series_event_title`, `movie_event_title`, `import_title`, `download_title`, `library_identity_action_title`).
- [x] 2.5 `import_ended`: the list of imports that ended.
- [x] 2.6 Autovacuum at 2 % on the job table.
- [x] 2.7 JIT off in every pooled session; the three places that set it by hand no longer do.
- [x] 2.8 Dropped: `series_candidate_search`, `movie_candidate_search`, `meta_episode_provider_id`. Kept, with the statements that use them: `library_file_inode_idx`, `meta_episode_air_date`, `series_episode_identity_provider`.
- [x] 2.9 A sync writes only the episodes that differ, with a test that an unchanged sync writes no row (`/tv/metadata`).

## 3. Measured, not fixed here (the next batch, in the order the figures give)

- [ ] 3.1 The TV scheduler reads every episode of the library at each run, and compares every series with every episode.
- [ ] 3.2 The series' metadata is followed five series a run.
- [ ] 3.3 The Jobs list sorts every job for each page; the list of queues reads every job.
- [ ] 3.4 The prune of old searches reads every search with a subquery for each.
- [ ] 3.5 The movies' scheduler and the Movies list's counts read every movie.
- [ ] 3.6 A search's results are stored one INSERT a row.
- [ ] 3.7 The last job of a kind (the metadata refresh, a measurement, a run of a connection) is found by reading the finished jobs.
- [ ] 3.8 The metrics count every download and every import by state at each scrape.
