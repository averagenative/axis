# Scale the database: second batch

## Why

The first batch built a database at the design scale (5,000 series, 300,000 episodes, 20,000 movies, 1,000,000 stored search results, 250,000 grabs, 300,000 imports with 3,000,000 files, 3,000,000 history events, 2,000,000 jobs), fixed what an index fixes, and measured the rest. What it measured and left needs a statement or a job redesigned:

- A run of the TV scheduler took about a minute and 476 MiB: it read every episode, and looked for each series among all of them.
- The series followed the metadata five series a run, so 5,000 series took 10.4 days to go round.
- A page of the Jobs list read and sorted every job: about a second.
- A search stored the releases it considered one statement a row.
- The prune of old searches ran every minute, as one statement for everything old enough.
- The lists counted with a look-up for each title.
- Nothing ever deleted a history event, a grab, a download or an import.

## What changes

- **The TV scheduler** asks the database which series have an episode that can be due, reads the episodes of those series only, a page of 50 series at a time, and keeps of what it plans only what the run can queue. What it queues is what it queued before.
- **The series follow the metadata** at a pace that goes round every series in six hours whatever their number: 5 a run for 100 series, 209 a run for 5,000, the longest behind first, never more than 500 a run or two minutes of a run.
- **The Jobs list** reads a page from a new index of the jobs by kind, queue and state. The page, its order and its cursor are unchanged.
- **A search stores its results** in one statement for up to 1,000 releases.
- **Old searches** are deleted once an hour, at most 1,000 a statement and ten statements a time.
- **The Movies list's counts** and the movies' attention count read each kind of fact once and join; the movies' facts read the searches in flight once.
- **The TV lists' counting statement** loses two orderings it did not need. The pages `/series` and `/series/wanted` are not faster for it: see "Not changed".
- **History has a keeping time.** Four settings on a new **History settings** card say how long events, grabs, finished downloads and finished imports are kept (0 each, which keeps them forever until an administrator chooses a time; a year is suggested). An hourly job deletes what is older, a bounded batch a statement, and never a row that anything still names, that is open, or that explains the present state of a title.

## Impact

- Capabilities: `tv` (the scheduler, the pace of the sync), `job-ui` (the Jobs list), `movies` (the list's counts), `database` (how a search's results are stored, how old searches go), and a new capability `history-retention`.
- Code: `src/modules/tv/axis-tv-follow.c`, `src/modules/tv/axis-tv-store.c`, `src/modules/tv/axis-tv-search.c`, `src/web/axis-web-jobs-data.c`, `src/acquire/axis-acquire-search.c`, `src/acquire/axis-acquire-follow.c`, `src/modules/movies/axis-movies-list.c`, `src/modules/movies/axis-movies-store.c`, `src/jobs/axis-history.c` (new), `src/core/axis-settings.c`, `src/axisd/main.c`.
- Migrations: `0081_scale_lists.sql` (three indexes), `0082_history_retention.sql` (eleven indexes). Plain `CREATE INDEX` in the migration's transaction, as in the first batch: the daemon applies migrations before it starts anything that writes. `design.md` has each index's build time.
- Tests: `tests/test-db-scale.c` (`/db/scale/second`, `/db/scale/retention`), `tests/test-tv.c`, `tests/test-movies.c`, `tests/test-history-retention.c` (new), `tests/test-settings.c`, `tests/test-job-registry.c`.
- Behavior that differs:
  - Nothing is deleted by default: each keeping time is 0, which keeps history forever, as before. History older than the time an administrator sets is deleted, within the rules above.
  - A new required module, **History**, with its settings card and its hourly job on the Jobs page.
  - A series' episode list follows the metadata within six hours on a library of any size; before, that held only up to 120 series.
  - The scheduler's log line counts the wanted episodes of the series it read, not of every series.
  - Old searches leave within the hour after they are old enough, not within the minute.
- Not changed, and said in `design.md`: the pages `/series` and `/series/wanted` still count every episode at each view, about a second at the design scale. Two needless orderings are gone from the statement; its time did not measurably change.
