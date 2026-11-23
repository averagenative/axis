# Scale the database: first batch

## Why

Reading the statements the daemon sends turned up places where the cost grows with the size of the history, not with the work done: counts of the few jobs that wait or run that read every job that ever ended, deletes that read a whole table once for each row deleted, and a list ordered by a column no index has. None of this shows on a small database.

The design scale is 5,000 series, 300,000 episodes, 20,000 movies, 400,000 library files, 1,000,000 stored search results, 250,000 grabs, 300,000 imports with 3,000,000 files, 3,000,000 history events and 2,000,000 jobs of which 300 wait or run. Nothing in the tests held a database of that size, so the reading could not be checked.

This change builds that database, measures every finding on it, and fixes the ones that an index, a session setting or a guarded statement fixes. The findings that need a statement or a job redesigned are measured and left for the next batch.

## What changes

- A test program, `tests/test-db-scale.c`, makes the database at the design scale (a tenth of it in `make check`), calls the daemon's own functions, and explains the statements they sent as they were sent. It reports each fix before and after, and holds bounds on the fixed ones.
- The pool can tell a test every statement its connections send (`axis_db_pool_set_statement_observer()`), so that what is measured is what ships.
- One migration, `0080_scale_indexes.sql`:
  - an index of the jobs that wait or run, and one of the probes that ended (the check whether an Analysis refresh is due walked every finished job when no probe was among them);
  - indexes on the foreign keys whose parent's delete read a large table (grabs by search result, search and episode; import files by library file; history events by episode and title; imports and downloads by title; samples by file; identification actions by title);
  - an index for the list of imports that ended;
  - autovacuum thresholds of 2 % on the job table;
  - three indexes dropped that no statement can use.
- Every pooled session has PostgreSQL's JIT off. The three places that turned it off by hand no longer do.
- A sync of a series' episodes with the metadata writes only the rows that differ.

No page shows different data.

## Impact

- Capabilities: `database` (the session, the foreign keys), `job-queue` (no periodic statement reads the whole job table), `tv` (an unchanged sync writes nothing).
- Code: `src/db/axis-db-pool.c`, `src/db/axis-db-conn.c`, `src/modules/tv/axis-tv-sync.c`, `src/modules/tv/axis-tv-store.c`, `src/transcode/axis-estimator-load.c`, `src/transcode/axis-conversion-run-list.c`, `data/migrations/0080_scale_indexes.sql`.
- Tests: `tests/test-db-scale.c` (new), `tests/test-tv.c`, `tests/scale/test-library-scale.c`.
- Upgrade: the migration builds 16 indexes in one transaction. On the design scale that takes a few seconds, during which the daemon has not started its jobs yet. `design.md` has the time of each.
- Behavior that differs: none on any page. `series_episode_identity.held_at`, which nothing reads, is no longer moved by a sync that changes nothing.
