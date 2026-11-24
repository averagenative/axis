# Design

## The database measured

The generated database of the first batch (`tests/test-db-scale.c`, `AXIS_DB_SCALE=100`): 5,000 series of 60 episodes, 300,000 episodes, 242,000 episode files, 20,000 movies, 392,000 library files, 145,500 searches with 980,000 stored results, 257,000 grabs, 242,040 downloads, 292,460 imports with 2,920,600 files, 3,008,000 history events, 2,000,350 jobs of which 300 wait or run. It took about 230 s to make.

The server is the throwaway PostgreSQL 17 of `tests/with-postgres.sh`, default memory settings, a warm cache, one desktop machine. The figures are orders of magnitude.

"Before" is the first batch's `/db/scale/report`, run again on this machine with the code as it was. "After" is `/db/scale/second` (and `/db/scale/retention`) of this change. A statement is explained as the daemon sent it, with its values written in (`EXPLAIN (ANALYZE, BUFFERS)`); "in the database" is the plan's `Execution Time`. `AXIS_DB_SCALE_PLANS=1` prints each whole plan.

## 1. The TV scheduler

`axis_tv_run_schedule()` read every series, then every episode's facts in one statement, and then, for each series, went through all the episodes to find its own: 5,000 x 300,000 comparisons, twice.

Now:

1. One statement names the series that have an episode that can be due (`axis_tv_store_schedule_series()`). An episode can be due when it is listed, monitored and aired, its next search time has come, and it has no file or has a file that its series' profile might replace.
2. The episodes of those series are read 50 series at a time (`AXIS_TV_SCHEDULE_PAGE`), with the facts statement every page already uses.
3. Each page's series are planned by the rule that planned them before (`plan_one_series()`, unchanged). After each page the plans are cut to what the run can still queue: the best `AXIS_TV_PER_SERIES_RUN + 2` of each series, and of those the best `2 x tv.searches_per_run + 32`.

### How the database tells "might replace" without a second rule

Whether a file is to be upgraded is decided by `axis_tv_episode_upgradable()` in C, from the profile's cutoff. A copy of that in SQL could come to differ. So the statement is given, for each profile that upgrades, the list of qualities it does not upgrade from: the qualities at its cutoff and above, by `axis_quality_compare()`, the function the rule itself uses. An episode whose files are all of such a quality, none of them made by a conversion (which counts as its source's quality), is settled. Any other file (of no known quality, below the cutoff, a conversion's) leaves its series to be read, and the rule decides.

The statement therefore names more series than have something due (an episode in flight, held, or with a search queued is among them), never fewer. `/db/scale/second` holds this: the number of episodes due that a run reports equals the count the test makes itself.

### What a run queues

The same searches as before. Planning a series reads that series' episodes only, so reading them a page at a time changes nothing. The cut after each page keeps a superset of what the run takes: the run takes at most `AXIS_TV_PER_SERIES_RUN` of a series and `tv.searches_per_run` in all, in the order the plans are sorted in. The slack is for a search that someone else asked for between the reading and the queueing, which the run passes over. More than two of those for one series in one run, or more than `tv.searches_per_run + 32` in all, would make a run queue fewer searches than it could have; the next run queues them.

`report.considered`, which only the job's log line shows, counted the wanted episodes of every series. It now counts those of the series that were read. `report.due` is unchanged.

### Figures

| | Before | After |
| --- | --- | --- |
| A run, every aired episode wanted and due (the generated database as made) | 53.1 s | 4.6 s |
| The process's peak memory in that run | 19 MiB to 476 MiB | 19 MiB to 24 MiB |
| In the database, that run | 2,503 ms, the longest statement 2,463 ms | 1,355 ms in 216 statements, the longest 548 ms |
| A run, 250 of the 5,000 series with something due | as above: everything was read | 0.6 s, of which 375 ms the statement that names the series |

The statement that names the series reads every episode's row and every episode file once, with hash joins: `HashAggregate` over `Hash Join` of `series_episode` and the files grouped by episode. It does not use an index, and its time follows the number of episodes: 375 ms at 300,000. Whether an episode is due depends on its next search time and its files, which no index of a fixed predicate holds. That is the part of a run that still scans.

## 2. The series following the metadata

A sync (`axis_tv_sync()`) reads the metadata store, not the provider: the store follows the provider at the provider's pace, in `axis-meta-refresh.c`, under the provider's limiter. A sync asks the provider only for a season whose episodes were never fetched, and waits at most 4 s for each. On the generated database a sync of a series of 60 episodes is 75 statements and about 29 ms.

The target is the one the code already states: a series follows the metadata every six hours. Each run takes `ceil(series x 900 s / 6 h)` of the series that are behind, the longest behind first:

| Series | A run syncs | Round |
| --- | --- | --- |
| up to 120 | 5 (the least) | under 6 h |
| 1,000 | 42 | 6 h |
| 5,000 | 209 | 6 h (10.4 days before) |
| 12,000 and more | 500 (the most) | 6 h at 12,000, longer above |

- No burst at a start: after a start or a long stop every series is behind, and they are taken at the same pace. `/tv/schedule/sync-paced` holds that.
- A run begins no sync after it has spent 120 s on them (a provider slow to give seasons that were never fetched); the rest wait for the next run.
- At 5,000 series a run's 209 syncs took 6.0 s.

Two days, as the task suggested, would be 26 a run at 5,000 series. Six hours costs about six seconds every fifteen minutes there and keeps what the code promised, so six hours it is.

## 3. The Jobs list

The list was already paged by a cursor. Its statement could not use an index: `state::text = ANY ($1)`, `(kind = ANY ($6)) = ($5 = 'upkeep')` and the order `(state IN ('queued', 'running')) DESC, id DESC` match none, so every page read and sorted every job.

`CREATE INDEX job_list_idx ON job (kind, queue, state, id)`. In it the jobs of one kind, queue and state lie together in the order of their ids. The statement:

1. finds every (kind, queue, state) the table holds by stepping through the index, one step a group (a recursive query; 44 groups on the generated database, some hundreds at most);
2. keeps the groups the filters let through, and for each takes the newest `page + 1` ids below the cursor (an index-only range scan, backward);
3. takes the newest `page + 1` of those, live ones first, and reads those rows by their primary key.

Every filter is a condition on a group, so a filter that few jobs pass, or none, costs no more than one that many pass. The queues' names are the groups' queues.

Each page was compared with the page the old statement gives, for every filter the page has (`measure_jobs_page()` in the test).

| Page | Before | After | Plan after |
| --- | --- | --- | --- |
| First page | 920 ms | 2.1 ms | Index Only Scan using job_list_idx, 44 loops; Index Scan using job_pkey, 51 loops |
| The work, a page half way | 511 ms | 1.0 ms | the same |
| The failed work | 116 ms | 0.8 ms | the same |
| A state and a queue that no job has | a read of every job | 0.4 ms | job_pkey never executed |
| The queues' names | 118 ms | 0.4 ms | Index Only Scan using job_list_idx |
| The page `/jobs` | 1,009 ms | 7 ms | |

No plan reads the job table as a whole.

The statements that ask for the last job of a kind (asked by a page, not on a timer) now read that kind's jobs through the index instead of every job. Where the kind has few jobs that is nothing: a measurement and a run of a connection, 71 ms and 73 ms before, 0.0 ms. Where it has many it is no gain: the last metadata refresh, 67 ms before, is 122 ms, a bitmap scan of that kind's 111,000 generated jobs and a sort. A real installation keeps one refresh job a day, so the generated figure overstates it; the statement itself is unchanged and still sorts all the jobs of its kind.

The counts of the Jobs page and the header were fixed by the first batch (`job_live_idx`). The count of failed work for Needs attention reads `job_failed_idx`, which holds the failed jobs only.

### The index at an upgrade

`job_list_idx` took 2,851 ms to build on 2,000,350 jobs and is 105 MB (the two indexes of unfinished searches: 13 ms and 2 ms, 8 kB each). It is a plain `CREATE INDEX` in the migration's transaction, which blocks writes to `job` while it builds. The daemon applies migrations before it starts its queue, so nothing of its own waits, as the first batch found. `CREATE INDEX CONCURRENTLY` needs a migration file of its own that is one statement outside a transaction (`-- axis:no-transaction`); for a build of this length it is not worth leaving an invalid index behind if the build is interrupted.

The index is written at every change of a job's state, as the job's other indexes are (a change of state already moves the row out of the partial indexes, so no update of a job is a heap-only one).

## 4. Storing a search's results

`store_candidates()` sent one INSERT a release, and for a series up to three UPDATEs more a release (`candidate_stored()`).

- The releases go in one statement for up to 1,000: `INSERT ... SELECT ... FROM jsonb_to_recordset($2::jsonb)`. One fixed statement text and two values, whatever the number of rows. A multi-row `VALUES` with 22 values a row was tried first: 42 ms for 1,000 rows, most of it parsing 22,000 values.
- The hook is `candidates_stored()`: once for all the releases. For a series it is one UPDATE joined to a JSON array (the episodes each release holds), and one UPDATE of the search's two counts.
- Conflict behaviour is unchanged: there is no `ON CONFLICT`; a row that breaks `UNIQUE (search_id, position)` or a check fails the statement and with it the search's transaction.
- The rows are the same: the test stores the same thousand both ways and compares a digest of every column.

| | Before | After |
| --- | --- | --- |
| 1,000 releases of one search | 177 ms | 24 ms |

## 5. The prune of old searches

It ran at the end of every follow-up, every minute, for each module: one DELETE of every search old enough, with a subquery for each.

- Once an hour, and at the first follow-up after a start (`AXIS_ACQUIRE_PRUNE_EVERY_S`; the time of the last one is kept in memory).
- At most 1,000 searches a statement, each statement its own transaction, at most ten a time. A search takes its stored releases with it (20 each on the generated database).
- What is kept is unchanged: the latest five of an owner, the manual ones counted apart, and any of the last week.

| | Before | After |
| --- | --- | --- |
| Nothing old enough | 6.6 ms, every minute | 23.6 ms, once an hour |
| Every search a month old | 110,000 searches and 280,000 releases in one statement: 4,483 ms, one transaction | 10,000 searches a time in ten statements: 202 ms in all, 19.7 ms the one explained |

The statement still reads `series_search` from its start until it has found its 1,000 (`Seq Scan`, stopped by the `LIMIT`). With nothing to delete it reads all of it. That table is itself bounded by this prune: the latest ten of each title and a week's searches.

The follow-up's question for the searches that never finished (every minute) read every search: 5.3 ms. `series_search_unfinished` and `movie_search_unfinished` hold just those: 0.0 ms, `Index Scan using series_search_unfinished`.

## 6. The lists' counts

### Movies

The statement that works out every film's state ran six look-ups for each film (the open grab, two imports, the files, the last search, a download left at a client, a search in flight). Each kind of fact is now read once and joined:

- what is under way is read from the partial indexes that hold just it: `movie_grab_one_open`, `import_open_owner`, `download_movies_live`, `job_dedupe_key_idx`;
- the film's own latest import is read only where it is one that waits, runs or is blocked, which is all the state reads of it; that it is the latest is checked for those few;
- the files are grouped by film, each file looked up by its id (`OFFSET 0` holds the planner to that: joined as two tables it walked the whole file index);
- the last searches are read in one pass of `movie_search_movie`.

The state each film gets is unchanged (`tests/test-web-movies.c`, `tests/test-movies.c`).

| | Before | After |
| --- | --- | --- |
| The list's counts, 20,000 films | 161 ms | 79 ms |
| The count of films that need attention | 155 ms | 75 ms |
| The page `/films` | 152 ms | 81 ms |

About half. Of the 79 ms, 36 ms is the files grouped by film (15,000 look-ups by id) and the rest merge joins over the 20,000 films.

It still reads every film: the counts are of every film's state. Its cost follows the number of films, not the history.

The count for Needs attention is not made at a page view: the board makes it in the background, at most every five minutes (the `attention` spec). It is the same statement, so it gained the same.

### The films' facts and `job_live_idx`

`facts_select` looked the search in flight up once a film: `WHERE j.dedupe_key = ... AND j.state IN ('queued', 'running')`. Since the first batch's `job_live_idx` exists, the planner prices it the same as `job_dedupe_key_idx` and chose it, reading every job that waits or runs for each film: 0.06 ms a film with 300 such jobs, 1.2 s of the movies' scheduler's statement (1,668 ms; 467 ms in the first batch's own report, before that index). The searches in flight are now read once and joined.

### TV

`/series` and `/series/wanted` count every episode by class at each view (`classes_select`, of speed-up-lists-and-dashboard). The statement ordered the files of every episode (an ordered `string_agg`) and its result; neither order is needed, and both are gone.

| | Before | After |
| --- | --- | --- |
| The episodes counted by class, 300,000 episodes | 1,013 ms | 1,123 ms |
| The page `/series` | 968 ms | 1,092 ms |
| The page `/series/wanted` | 958 ms | 1,068 ms |

**This is not fixed, and not faster.** At 30 % of the scale the statement went from 360 ms to 316 ms; at the design scale the two runs differ by less than the machine's noise between them (other work ran beside the second). Read it as unchanged. What remains is the work itself: 300,000 episodes joined to 242,000 files and grouped, at each view. A larger `work_mem` for the statement was measured and changes nothing (the sorts that spilled are the ones removed). Making these pages cost a page needs their counts to come from memory, read in the background and told to open pages, as the dashboard's are since speed-up-lists-and-dashboard. That needs every write that changes a series' state to say so (there is no such signal in the TV module today), and it makes the tabs' counts a few seconds old. It is a change of its own, with that decision in it.

## 7. History retention

### The settings

A new required module, **History** (`src/jobs/axis-history.c`), with its own card on the Settings page after **Job queue**:

| Key | Label | Default |
| --- | --- | --- |
| `history.keep_events` | Keep the history of what happened to movies and series for | 0 |
| `history.keep_grabs` | Keep the record of each release grabbed for | 0 |
| `history.keep_downloads` | Keep finished downloads on the Downloads page for | 0 |
| `history.keep_imports` | Keep finished imports on the Imports page for | 0 |

Each takes 30 days to 3,650 days, or 0 for "kept forever", which is the default. A duration could not have both a minimum and a 0, so the settings got a flag, `AXIS_SETTING_ZERO_OFF`.

The default deletes nothing: an upgrade keeps all history until an administrator chooses a time, and the help text says so. It suggests a year, which is long enough that nothing a person looks at in the ordinary way goes, and which bounds every table by a year's activity plus what the rules below keep.

### The job

`history.prune`, "Deleting old history of movies, series, downloads and imports", upkeep, every hour.

- One statement a table, sent again and again. It takes the oldest 1,000 rows that ended before the keeping time (250 imports: each takes up to 200 files with it), after where the statement before stopped, through an index on (ended at, id). It deletes those of them that nothing needs, in its own transaction.
- The place it stopped at is kept in memory. Rows that are kept for good are therefore passed once a sweep, and never stand in front of rows that may go. A restart sweeps from the oldest again.
- At most 50 statements a table a run. About 50,000 rows of each kind go an hour, so a backlog of a million goes in a day.
- An error in one table does not stop the others: the run remembers the first error, goes on, logs the later ones and ends as failed with the first. A cancelled run stops at once.
- Order in a run: events, imports, grabs, downloads. A row goes only after what named it.

### What is never deleted

A row that another row names is kept, whether the reference is a foreign key or a plain column. `ON DELETE SET NULL` is not taken as leave to delete: `series_file.import_id IS NOT NULL` is itself a fact that the episodes' state reads.

- **Events** (`series_event`, `movie_event`, by `created_at`). Kept: the 30 latest of each movie and series (the pages show 30 and 20), and of each title that was removed; the kinds `added` and `removed`; every event with `by_user` set. Nothing names an event.
- **Imports** (ended ones only: `imported`, `dismissed`; by `finished_at`). Never looked at: waiting, running, blocked, partly imported. Kept: one that `series_file.import_id` or `movie_file.import_id` names (a file in the library came by it); one that `import_recycle` names; one with `client_removal` pending, failed or kept (a torrent left seeding: the Leftovers and folder-removal code count its folder as in use); `folder_removal` pending or waiting; a file still under a temporary name, or whose source is still to be deleted; one whose download is still at its client or has a folder removal pending; the latest of each (module, owner), which the movie's facts read; one with a job queued or running. Its files, actions and assignments are its own and go with it.
- **Grabs** (closed ones only, by `closed_at`). Kept: `removal` pending or failed (the follow-up asks again); one that a blocklist entry, an event, or an import (by `grab_id` with its module, or by the download) names; one whose download is still at its client or has a folder removal pending; a series' grab closed as imported with "Imported, with no file for:" (the search's `had_before` rule reads it so as not to grab that release again); the 5 latest of each movie and series. A series' grab takes its `series_grab_episode` rows with it.
- **Downloads** (`failed`, `gone`, `removed`, `imported` with `finished_at`). Kept: one that any grab or import names; `folder_removal` pending or waiting.

Together these keep the whole record of every file that is in the library now: its import, that import's grab and that grab's download.

Not deleted at all, and said so: `import_recycle` (and with it every import that retired a file, its grab and its download). The stored searches are not this job's; section 5 prunes them.

One race is accepted: a person who blocks a year-old closed grab, or imports a year-old finished download by hand, in the same milliseconds as the batch that deletes it, gets a row that names a grab that is gone. For a blocklist entry (`series_blocklist.grab_id`, `movie_blocklist.grab_id`: foreign keys, `ON DELETE SET NULL`) the link becomes NULL. For the plain columns `series_event.grab_id`, `movie_event.grab_id` and `import.grab_id` (no foreign key) the id stays and names no row. Every reader tolerates that: the pages never read an event's `grab_id` (their column lists leave it out), an import's `grab_id` is loaded into `AxisImportInfo` and not used, `axis-tv-place.c` only tests it for NULL to order the open imports, an import finds its grab by its download and never by that column, and this job's own checks (`NOT EXISTS ... grab_id = g.id`) only look for rows that name an existing grab. Locking the rows looked at would write to every one of them.

### The migration

`0082_history_retention.sql`, eleven indexes, 1.2 s in all on the generated database:

| Index | For | Build | Size |
| --- | --- | --- | --- |
| `series_event_age (created_at, id)`, without the events kept for good | the sweep | 811 ms | 81 MB |
| `movie_event_age` | the sweep | 87 ms | 9.3 MB |
| `series_grab_age (closed_at, id)` of closed grabs | the sweep | 78 ms | 7.5 MB |
| `movie_grab_age` | the sweep | 7 ms | 480 kB |
| `download_age (finished_at, id)` of ended downloads | the sweep | 85 ms | 7.5 MB |
| `series_event_grab`, `movie_event_grab` | "an event names this grab" | 138 ms, 20 ms | 8 kB each |
| `series_blocklist_grab`, `movie_blocklist_grab` | the same, and the foreign key's look-up at a delete | 1 ms each | 8 kB each |
| `series_file_import`, `movie_file_import` | "a file came by this import", and the foreign key's look-up | 20 ms, 2 ms | 8 kB each |

The last six are small only because the generated database leaves those columns empty. The imports' sweep uses `import_ended` of the first batch.

### Figures

Half of every kind aged 400 days, every keeping time at a year (the defaults keep everything and would delete nothing):

| Run | Time | Statements | Longest | Events | Imports | Grabs | Downloads |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 2,729 ms | 259 | 28 ms | 47,220 | 12,282 | 50,000 | 2,078 |
| 2 | 2,614 ms | 259 | 19 ms | 47,225 | 12,280 | 46,195 | 4,362 |
| 3 | 1,928 ms | 202 | 20 ms | 47,212 | 12,280 | 12,288 | 2,750 |

Every batch is an Index Only Scan on its age index (backward on `import_ended`), then the primary key with `= ANY`; the checks use `series_event_series`, `import_owner`, `import_download`, `download_pkey` and the new indexes. No plan reads a history table as a whole; the test holds that. A batch is 2 to 9 ms in the database. No movie event and no movie grab went: every generated movie is inside its floor (15 events, one grab). The downloads go slowly because each waits for its import.

## What is left as measured

| What | Figure | Why it is left |
| --- | --- | --- |
| `/series` and `/series/wanted` | about 1 s each, as before | section 6 |
| The last metadata refresh, asked by a page | 122 ms on the generated jobs | section 3 |
| The movies' scheduler | 0.5 s a run in the statement that reads every monitored movie's facts | not in this batch's list; the look-up it lost is in section 6 |
| The statement that names the series with something due | 375 ms every 15 minutes | section 1 |
| Metrics, downloads and imports by state | 22 ms and 25 ms a scrape | unchanged; retention now bounds both tables |
| `import_recycle` | grows | not shown safe to delete |

## What a reviewer should check

- The defaults are 0: nothing is deleted until an administrator sets a time. A run whose deletion fails in one table goes on with the others and fails with the first error.
- `report.considered` in the scheduler's log line means something narrower.
- `candidate_stored()` became `candidates_stored()`: a module's hook is called once for all the releases of a search.
- The prune's time is kept in memory, so each start prunes once at the first follow-up.
