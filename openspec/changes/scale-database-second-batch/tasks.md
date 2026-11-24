# Tasks

## 1. Measuring

- [x] 1.1 `tests/test-db-scale.c`, `/db/scale/second`: what this change fixed, on the generated database, with bounds that `make check` holds at a tenth of the scale. "Before" is the first batch's report (`/db/scale/report` of that change), run again on this machine.
- [x] 1.2 `AXIS_DB_SCALE_PLANS` prints the whole plan of every statement measured.
- [x] 1.3 Each fixed statement checked against what it gave before: the Jobs list against the statement it replaced, page for page; the stored releases against the rows a statement a row stored; the scheduler against the count of episodes that are due.

## 2. The TV scheduler and the sync

- [x] 2.1 `axis_tv_store_schedule_series()`: the series with an episode that can be due, told by the database from the episodes' facts and the profiles' own lists of qualities they do not upgrade from.
- [x] 2.2 `axis_tv_run_schedule()` reads those series' episodes a page of 50 series at a time and keeps of the plans only what the run can queue.
- [x] 2.3 `axis_tv_sync_batch()`: the series synced a run follow the number of series, between 5 and 500, within two minutes.
- [x] 2.4 Tests: what is read and what is not, a page of one series against a page of fifty, more planned than a run queues, the pace of the sync (`/tv/schedule/*`).

## 3. The Jobs list

- [x] 3.1 `0081_scale_lists.sql`: `job_list_idx (kind, queue, state, id)`.
- [x] 3.2 The list's statement and the queues' names read the groups of that index; the page, its order and its cursor unchanged.
- [x] 3.3 Measured with every filter the page has, and one that nothing passes.

## 4. Searches

- [x] 4.1 A search's releases stored 1,000 a statement; what a series keeps of each release in one statement more (`candidates_stored()` in place of `candidate_stored()`).
- [x] 4.2 The prune of old searches once an hour, 1,000 a statement, ten statements a time (`axis_acquire_prune_searches()`); test `/movies/prune-paced`.
- [x] 4.3 `series_search_unfinished`, `movie_search_unfinished`: the searches that never finished.

## 5. The lists' counts

- [x] 5.1 The Movies list's state statement joins each kind of fact once (`STATE_FROM` in `axis-movies-list.c`); the movies' facts read the searches in flight once.
- [x] 5.2 The TV lists' counting statement no longer orders every file and every class (no measurable gain at the design scale).
- [ ] 5.3 `/series` and `/series/wanted` still count every episode at each view. Measured and said in `design.md`; not fixed here.

## 6. History retention

- [x] 6.1 `AXIS_SETTING_ZERO_OFF`: a setting whose 0 switches it off beside a minimum.
- [x] 6.2 The module `history` (`src/jobs/axis-history.c`): four keeping times, the hourly job `history.prune`, one bounded statement a table sent again and again, a cursor so that kept rows do not block.
- [x] 6.3 `0082_history_retention.sql`: the indexes the sweeps and their checks read.
- [x] 6.4 Tests: one case for each keep rule, the bounds of a run, 0 keeps everything, the settings page (`tests/test-history-retention.c`); `/db/scale/retention`.
- [x] 6.5 The user guide: "Choose how long Axis keeps history".

## 7. Left as measured

- [ ] 7.1 The movies' scheduler still reads every monitored movie's facts at each run (0.5 s at 20,000 movies).
- [ ] 7.2 The metrics count every download and every import by state at each scrape (22 ms and 25 ms).
- [ ] 7.3 `import_recycle` is not pruned, and an import that retired a file is kept with it.
