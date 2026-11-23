# Design

## The database measured

`tests/test-db-scale.c` makes one database with `generate_series`, then runs `VACUUM ANALYZE`. At `AXIS_DB_SCALE=100` it holds:

| What | Rows |
| --- | --- |
| Series, of 3 seasons of 20 episodes | 5,000 |
| Episodes | 300,000 |
| Episode files | 242,000 |
| Movies | 20,000 |
| Library files | 392,000 |
| Searches | 145,500 |
| Stored search results | 980,000 |
| Grabs | 257,000 |
| Downloads | 242,040 |
| Imports | 292,460 |
| Import files | 2,920,600 |
| History events | 3,008,000 |
| Jobs, of which 300 wait or run | 2,000,350 |

It takes about 250 s to make at that scale, and 26 s at the default tenth.

The server is the throwaway PostgreSQL 17 of `tests/with-postgres.sh`: default memory settings, `fsync` off. The times below are of a warm cache on one desktop machine. Read them as orders of magnitude.

## How a statement is measured

The test calls the daemon's own function. The pool tells the test each statement it sent, with its values (`axis_db_pool_set_statement_observer()`). The test writes the values into the statement and runs `EXPLAIN (ANALYZE, BUFFERS)` on it, in a transaction that it rolls back. So the plan is of the statement that ships, not of a copy.

A statement that no function sends alone (a delete of chosen rows), or whose function needs a tool or a client the test does not have, is in the test as text. Each of those is named with its source file.

Before and after come from one run. The test drops the indexes of the migration, measures, builds each index and times it, and measures again.

"In the database" is the plan's `Execution Time`. "The call" is the daemon's function, the fastest of three.

## What was fixed

All figures are at the design scale, in milliseconds.

### 1. The jobs that wait or run

No index held both live states. `job_claim_idx` holds the queued jobs and `job_lease_expiry_idx` the running ones, and the planner does not join two partial indexes for `state IN ('queued', 'running')`. Every such statement read the table.

`CREATE INDEX job_live_idx ON job (kind, queue) WHERE state IN ('queued', 'running')` serves all of them as they are written. No statement needed a rewrite.

| Statement | Caller | Before | After | Plan after |
| --- | --- | --- | --- | --- |
| The header's count | `axis_web_jobs_count_activity()`, from `axis-web-jobs-bridge.c` at each change of a job's state (at most every 500 ms) and every 30 s | 62.1 | 0.1 | Bitmap Index Scan on job_live_idx (rows=301) |
| Waiting and running, of work or upkeep | `axis_web_jobs_count_active()`, the Jobs page and the dashboard's reading | 58.5 | 0.1 | Bitmap Index Scan on job_live_idx |
| Running now | `axis_web_jobs_count_running()`, the Jobs page's live rows | 0.0 | 0.0 | Index Scan using job_lease_expiry_idx (before too) |
| The work that waits or runs | `axis-web-jobs-activity.c`, the page Now | 59.0 | 1.1 | Bitmap Heap Scan on job j, by job_live_idx |
| The work that ended this hour | the same page | 0.5 | 0.2 | Index Scan Backward using job_finished_idx (before too) |
| The probe queue's top-up | `axis_probe_enqueue_due()`, every 60 s | 58.4 | 0.0 | Index Only Scan using job_live_idx |
| Whether an Analysis refresh is due | `axis_analysis_enqueue_due()`, every 300 s | 1240.0 | 0.3 | Index Only Scan Backward using job_probe_ended_idx |
| Metrics, jobs by queue and state | `axis-web-metrics.c`, each scrape | 60.6 | 0.2 | Bitmap Index Scan on job_live_idx |
| The fetches known | `axis-youtube-fetch.c`, at a fetch | 61.6 | 0.0 | Index Scan using job_live_idx |
| A measurement to cancel | `axis-estimate-sample.c`, when a person cancels | 76.3 | 0.0 | Index Scan using job_live_idx |
| The kinds stored | `axis-job-queue.c`, at start-up | 61.5 | 0.2 | Bitmap Index Scan on job_live_idx |

Before, each plan was `Parallel Seq Scan on job` over 2,000,350 rows.

The check whether a refresh is due was the worst, and for another reason. It asks for `max(finished_at)` of the probes that succeeded. The planner answers that by walking `job_finished_idx` from the newest job back to the first probe. A library that was probed long ago has few probes among the jobs kept, or none: the walk covers nearly every finished job. The statement names that value three times, and the walk ran three times (about 400 ms each).

`CREATE INDEX job_probe_ended_idx ON job (finished_at) WHERE kind = 'probe.file' AND state = 'succeeded'` answers it from one index entry. The index holds the probes only. The statement is unchanged.

### 2. Deleting stored search results, searches and episodes

`series_grab.candidate_id`, `.search_id` and `.episode_id`, and `movie_grab.candidate_id` and `.search_id`, are foreign keys with `ON DELETE SET NULL` and had no index. Each deleted parent row made the database read the grabs.

| Statement | Caller | Before | After | Plan line |
| --- | --- | --- | --- | --- |
| Delete 1,000 stored results of series | the cascade of the prune of old searches (`prune_searches()`, every 60 s), a change of numbering (`axis-tv-numbering.c`) | 12,284 | 9.3 | Trigger for constraint series_grab_candidate_id_fkey: 10,984 before, 8.7 after |
| Delete 1,000 stored results of movies | the same prune for movies | 594 | 76 | Trigger for constraint movie_grab_candidate_id_fkey |
| Delete 100 searches of series, 20 results each | the prune | 22,118 | 35 | series_grab_candidate_id_fkey on series_candidate: 19,426 before, 17 after; series_grab_search_id_fkey: 960 before, 1 after |
| Delete 100 episodes | removing a series | 18,625 | 30 | series_event_episode_id_fkey: 14,907 before, 12 after; series_grab_episode_id_fkey: 2,201 before, 9 after |

The indexes are partial (`WHERE candidate_id IS NOT NULL` and so on). The look-up of the foreign key is `candidate_id = $1`, which implies the index's condition, so the planner uses it.

The prune matters most. With every search old enough to go, it deletes 110,000 searches and their 280,000 stored results in one statement. After, that takes 5.9 s. Before, at 10 ms a search and 10 ms a stored result, it would take about an hour, and the session's statement timeout is 60 s: the statement would be cancelled at every run and the backlog would never shrink.

### 3. Deleting library files and titles

| Statement | Caller | Before | After | Plan line |
| --- | --- | --- | --- | --- |
| Delete 1,000 library files | a scan that drops missing files (`axis-library-index.c`), an import that replaces a file (`axis-import-run.c`), a conversion (`axis-conversion-place.c`) | 221,642 | 130 to 750 | Trigger for constraint import_file_library_file_id_fkey: 212,878 before, 606 after |
| Delete 100 titles that nothing manages | a merge of two records (`axis-meta-store.c`) | 20,442 | 57 | series_event_title_id_fkey: 13,385 before, 15 after; download_title_id_fkey: 2,252 before, 1 after; import_title_id_fkey: 2,040 before, 16 after; movie_event_title_id_fkey: 1,315 before, 15 after |
| The history of a movie that was removed | `axis-movies-store.c`, a title's page | 0.7 | 0.1 | Bitmap Index Scan on movie_event_title (rows=20) |

`download.title_id` was not in the reading. The two indexes on it hold only the downloads still at a client. It is indexed here (`download_title`). The look-up of a claim's download in `axis-acquire-follow.c` (`module = AND title_id = AND release_name =`) uses it too.

`estimate_sample.file_id` and `library_identity_action.title_id` are indexed as well. Both tables are empty in the generated database, so their figures say nothing.

### 4. The imports that ended

| Statement | Caller | Before | After | Plan after |
| --- | --- | --- | --- | --- |
| The 50 imports that ended last | `axis_import_list()`, the Imports page | 661 (the call: 572) | 0.3 (the call: 1.6) | Index Scan using import_ended (rows=50) |

Before: `Seq Scan on import` of 292,400 rows, a subquery for each, then a sort. `CREATE INDEX import_ended ON import (finished_at DESC, id DESC) WHERE state IN ('imported', 'dismissed')` matches the statement's order.

### 5. Autovacuum on the job table

`ALTER TABLE job SET (autovacuum_vacuum_scale_factor = 0.02, autovacuum_analyze_scale_factor = 0.02)`. At 2,000,000 rows the default waits for 400,000 dead rows. No test reads or depends on the default.

### 6. JIT

`src/db/axis-db-pool.c` sets `jit = off` in every pooled session. The three places that set it for one transaction no longer do: the TV lists' counting statement (`axis-tv-store.c`), the estimator's loader (`axis-estimator-load.c`, two places) and a conversion run's list (`axis-conversion-run-list.c`).

Measured in one run with the pool's sessions as they are now, then with each session set back to `jit = on`:

| What | JIT off | JIT on |
| --- | --- | --- |
| The Analysis refresh of 392,000 files | 3,722 | 4,973 |
| The episodes counted by class, the slowest list statement | 1,034 | 2,502 (1,812 of it compiling) |
| The movies' scheduler | 533 | 1,734 |
| The page `/films` | 150 | 867 |
| The page `/series` | 987 | 1,013 |
| The page `/series/wanted` | 975 | 997 |
| The page `/jobs` | 1,015 | 1,009 |

Nothing measured is slower with it off. `/series` and Wanted hardly move because their statement already ran with it off.

### 7. Indexes dropped and kept

Dropped:

- `series_candidate_search` and `movie_candidate_search` have exactly the columns of their tables' `UNIQUE (search_id, position)`.
- `meta_episode_provider_id` is partial (`WHERE provider_id IS NOT NULL`). A statement can use it only if its own condition implies that. Every statement that reads `meta_episode` looks an episode up by title and place, or by title and day. The one that compares `provider_id` does it inside `OR me.provider_id IS NULL` (`axis-arr-preview.c`), which implies nothing.

Kept, against the reading:

- `library_file_inode_idx` is used by the delete of a file by volume and inode (`axis-import-run.c`) and by the join of hard-link partners in the Analysis refresh (`axis-analysis-refresh.c`).
- `meta_episode_air_date` is used by the view `library_episode_file`, which joins episodes by title and day.
- `series_episode_identity_provider (source, provider_id)`: no statement names `provider_id`, but the one reader (`axis-tv-numbering.c`) filters `source`, the index's first column. The planner could choose it. Not shown unusable, so not dropped.

### 8. A sync writes only what changed

`axis-tv-sync.c` updated every episode of a series at each sync. The trigger `series_episode_remembers` fires on any update that names the identity columns, so each update also rewrote the row of `series_episode_identity`. Two dead rows an episode, at each sync.

The update now repeats what it sets in its `WHERE` with `IS DISTINCT FROM`. `/tv/metadata` holds that a sync that changes nothing leaves every row of both tables the version it was (`ctid` and `xmin`).

`series_episode_identity.held_at` is no longer moved by an unchanged sync. Nothing reads it.

## The migration at an upgrade

`0080_scale_indexes.sql` is one transaction. Each `CREATE INDEX` blocks writes to its table until the transaction ends; reads go on. Built one at a time on the design scale:

| Index | Build | Size |
| --- | --- | --- |
| series_event_episode | 529 ms | 24 MB |
| series_event_title | 482 ms | 20 MB |
| import_file_library_file | 227 ms | 5.3 MB |
| job_live_idx | 99 ms | 16 kB |
| job_probe_ended_idx | 98 ms | 16 kB |
| import_ended | 93 ms | 9.0 MB |
| movie_event_title | 79 ms | 9.4 MB |
| series_grab_for_episode | 66 ms | 5.3 MB |
| import_title | 53 ms | 1.7 MB |
| download_title | 52 ms | 1.7 MB |
| series_grab_candidate, series_grab_search | 18 ms, 19 ms | 448 kB each |
| movie_grab_candidate, movie_grab_search | 4 ms, 3 ms | 240 kB each |
| estimate_sample_file, library_identity_action_title | 1 ms each | empty |
| All 16 | 1.8 s | |

The three drops take no time. The daemon applies migrations before it starts its jobs, so nothing of its own waits for the locks. A slow disk and a cold cache can make this several times longer; it stays a matter of seconds.

The runner (`src/db/axis-db-migrate.c`) wraps a migration in one transaction and switches the statement timeout off for it. `CREATE INDEX CONCURRENTLY` cannot run in a transaction. The runner allows it only in a file that starts with `-- axis:no-transaction`, and such a file must be exactly one statement. Concurrent builds would therefore need one migration file an index. At 1.8 s in all that is not worth it here. Use it when a single index on a table of tens of millions of rows is added to a running system.

## What was measured and left

These need a statement or a job redesigned. The figures are for ordering the next batch.

| What | Figure at the design scale | Where the time goes |
| --- | --- | --- |
| The TV scheduler, every 15 minutes (`axis_tv_run_schedule()`) | 66.4 s a run. 2.6 s of it in the database, in one statement that reads the facts of all 300,000 episodes. The process's peak memory rises from 19 MiB to 475 MiB. | The rest is in C: every series is compared with every episode (5,000 x 300,000). No statement comes near the 60 s timeout; the job itself takes longer than a minute. |
| The series' metadata sync (`axis-tv-follow.c`) | 5 series a run, a run every 900 s: 5,000 series take 10.4 days to go round. The statement that picks them is under 20 ms. | The batch size. A series is meant to be followed every 6 hours. |
| The Jobs list, first page (`axis_web_jobs_list()`) | 930 ms in the database, 1.0 s the page | Parallel Seq Scan on job (all 2,000,350 rows), top-N sort. `state::text = ANY ($1)` and the order `(state IN ('queued', 'running')) DESC, id DESC` match no index. |
| The Jobs list, a page half way | 513 ms | The same, over the rows before the cursor. |
| The Jobs list, the failed ones | 114 ms | Parallel Seq Scan; the cast keeps `job_failed_idx` from being used. |
| The queues' names for the Jobs page | 117 ms | `SELECT DISTINCT queue` reads every job. |
| The pages `/series` and `/series/wanted` | 984 ms and 965 ms | The episodes counted by class: 1,034 ms over 300,000 episodes. |
| The page `/` before the board is started | 1,665 ms | Read at the view. Once started it is drawn from memory. |
| The page `/series/calendar` | 224 ms | 2.3 MB of HTML: one series in ten of the generated ones airs this week. |
| The movies' scheduler (`axis_movies_run_schedule()`) | 3.6 s the first run (20,010 statements, about one a movie), 0.5 s after. 467 ms in the statement that reads every monitored movie's facts. | A statement a movie where what the scheduler last saw of it changed, which is every movie on the first run; every movie's facts at each run. |
| The Movies list's counts, and its attention count | 165 ms and 164 ms | A subquery for each of the 20,000 movies. |
| The prune of old searches, nothing old enough | 7 ms | Seq Scan on series_search (145,500 rows), every 60 s. |
| The prune, every search a month old | 5.9 s for 110,000 searches | The cascade to 280,000 stored results. One statement, one transaction. |
| Storing one search's 1,000 results | 239 ms, one INSERT a row | The same rows in one statement: 15 ms. |
| The last job of a kind: the metadata refresh, a measurement, a run of a connection | 67 ms, 71 ms, 73 ms | Parallel Seq Scan on job. Each is asked by a page, not on a timer. |
| Metrics, downloads and imports by state | 22 ms and 25 ms a scrape | A read of each table. Left as it is: an estimate would change what the metrics say, and the cost is small. It grows with the history. |
| The other downloads of a volume (`axis-import-folder.c`, `axis-import-leftovers.c`) | 75 ms | Seq Scan on download and on import. An index on `download.volume_id` does not help: nearly every download is on the one volume. It needs an index of the downloads still at a client by volume. |

## What the measurements contradicted

- The count of running jobs already used `job_lease_expiry_idx`. Only the statements that name both live states read the table.
- The job counts cost about 60 ms each at 2,000,000 jobs, not seconds: the scan is parallel and the table was just vacuumed. They run at every change of a job's state, so the index is still worth it.
- The check whether an Analysis refresh is due was the slowest periodic statement on the job table (1.2 s every 5 minutes), and the reading listed it only as one of the shape.
- `download.volume_id`: an index cannot help the look-up (see the table above). Not added.
- `movie_event WHERE title_id`: 0.7 ms before. The planner used `movie_event_movie` for `movie_id IS NULL`, which few rows match. The new index is for the delete of a title.
- `library_file_inode_idx` and `meta_episode_air_date` are used.
- `download.title_id` was missing from the list of foreign keys.
