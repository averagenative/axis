# Design: speed up the lists and the dashboard

## What was measured

A library of 2,400 movies, 206 series, 22,500 episodes, 14,600 episode files and a job table of 160,000 rows, with PostgreSQL warm and nothing else running:

| Page | Time |
| --- | --- |
| Dashboard | 10 to 12 s |
| TV list | 9 to 10 s |
| Wanted | 8 to 9 s, 288 kB |
| Downloads, Imports, Needs attention | 2 to 30 ms |

Two statements ran for seconds, more than once a page: every series with what its own page shows (about 2.7 s), and the facts of every episode (about 2.6 s).

## Who read everything, and what each needs

| Caller | What it read | What it needs | What it reads now |
| --- | --- | --- | --- |
| Dashboard, TV figures (`dash_collect`) | every series, every episode | episodes by state, the episodes that air today | the counted classes; eight episodes of today by the air date index; in the background only |
| TV list (`grid_run`) | every series, every episode | each series' state and counts, a page of rows | the counted classes; the episodes of the page's series that need attention |
| Wanted (`wanted_run`) | every series, every episode | the tabs' numbers, a page of rows | the counted classes with the place of each candidate episode; the facts of the page's 100 episodes |
| Calendar | every series, every episode | the episodes of three weeks without a file | the episodes whose air date is in the three weeks |
| Needs attention, the series' lines | 50 series a batch, in the background | which series need attention and why | unchanged: bounded batches, never on a page view; each batch is cheaper |
| Title page, Discover and search cards (`axis_tv_get_by_titles`) | the page's titles only | state of each | unchanged in shape; the series are read without the per-series lookups |
| A series' page | that series | everything | unchanged: that series only |
| Scheduler (`axis_tv_follow_run`) | every series, every episode, in a job | every episode's facts | the series as a list reads them; the episode facts with the searches in flight joined. Not a page view |

## Counting without a second definition of a state

A state exists only in C: `axis_tv_episode_derive_attention()` works it out from an episode's facts. A copy of that rule in SQL could come to differ from what a series' page says.

So the database does not derive a state. It groups the episodes of each series by the facts the rule reads: monitored or not, aired or not (as a day: the last day whose wait has passed), in an open grab or not and whether that grab has arrived, the state of its import, a download left at a client, a search in flight, a file or not, kept by hand or brought over, the qualities of its files, the outcome of its last search and whether that search found a release only a person can grab. A series has a handful of such classes whatever its length: 6,916 classes for 36,000 episodes on the fixture.

Each class is then given to the one rule as an episode, and its count is added to the series' summary. The lists therefore say what every series' page says.

Two things need more than a count:

- A series that needs attention shows what is wrong and where to put it right. That is read from the episodes of those series, for the series on the page only, in one statement.
- Wanted lists episodes. The class statement also returns the place (season, number, id) of each episode that can be in a listed state by its facts alone. The page orders them, cuts them to 100, and reads the facts of those 100.

The statement reads the files, the open grabs, their imports, the downloads left at a client and the searches in flight once each and joins them. Only the last search of an episode is looked up by itself, and only for an episode that can be wanted: aired, monitored, with no file and in no grab.

## The searches in flight and the job table

Finished jobs are kept 7 days (succeeded and cancelled) and 30 days (failed) by default; an hourly job deletes older ones. The settings are "Keep succeeded and cancelled jobs for" and "Keep failed jobs for", with a minimum of one day. On an installation five days old nothing is old enough to delete yet, so 160,000 rows is the retention working as set, at about 32,000 jobs a day. It levels off near 225,000 rows.

The lookups of queued and running jobs by key use `job_dedupe_key_idx`, a partial unique index over the queued and running jobs only. It does not grow with the finished jobs. The cost was the number of lookups, one for each episode, not the size of the table. The jobs in flight are now read once from that index (330 rows on the fixture, 4 ms) and joined. No migration is needed.

## Plans, before and after

On the fixture: 1,500 series, 36,000 episodes, 22,650 files, 23,100 searches, 33,000 releases found, 300 open grabs, 160,330 jobs.

Before, every series with what its own page shows:

```
Incremental Sort (actual time=572.690..677.427 rows=1500)
  Bitmap Index Scan on series_candidate_search (loops=23100)   three times
JIT: Total 574.015 ms
Execution Time: 681.827 ms
```

Before, the search in flight of every episode, a row at a time:

```
Index Scan using job_dedupe_key_idx on job j (loops=36000)
Execution Time: 74.029 ms
```

Before, the facts of every episode (with the jobs already joined): 233 ms in the database for 36,000 rows of 42 columns, then each series' episodes found by going through all 36,000 rows.

After, the series of a list: `Execution Time: 4.176 ms`, no subquery a row.

After, the episodes counted by class:

```
GroupAggregate (actual time=95.097..166.785 rows=6916)
  Bitmap Heap Scan on job jj (actual time=0.060..4.140 rows=330 loops=1)
  Index Scan using series_search_episode on series_search s2 (loops=11550)
JIT: Total 56.121 ms
Execution Time: 185.574 ms
```

The planner prices the lookup of the last search as if every episode had one made, which puts the statement over the price at which the server compiles the plan first. That compilation took 56 ms of 185 ms, so the daemon runs this statement with `SET LOCAL jit = off`. The same compilation was 574 ms of the 682 ms of the old series statement.

## Times on the fixture

| What | Before | After, sanitizers | After, no sanitizers |
| --- | --- | --- | --- |
| Every series counted (what the dashboard, the list and Wanted each did at a view) | 3,863 ms sanitizers, 1,982 ms without | 142 ms | 121 ms |
| Dashboard, started | at least the row above | 1 to 3 ms, no statement | 0 to 1 ms, no statement |
| Dashboard's figures, read in the background | | 213 ms | 190 ms |
| TV list | at least the first row | 195 to 214 ms, 11 to 13 statements | 159 to 170 ms |
| Wanted | at least the first row | 177 to 184 ms, 6 statements, 62 kB | 142 to 145 ms |
| Calendar (1,800 episodes in its three weeks) | at least the first row | 105 ms | 40 ms |
| A series' page (24 episodes) | not measured | 38 ms | 28 ms |
| Movies list, a page of 60 of 5,000 (not changed) | | 73 ms | not measured |

The pages before the change were not timed on this fixture. The first row is the function each of them called, timed in the same test program.

## The dashboard

The dashboard's parts keep their two functions: one reads figures on a worker thread, one draws them on the main loop. What changes is when the first runs.

- `axis_web_dashboard_start()` starts the board; the daemon calls it beside the start of the board of what needs attention. From then on no page view runs a part's reading.
- What is cached: the figures each part last gave, in memory, for all viewers. Nothing is stored.
- When it is read again: at the start; when the shell is told that something happened, which is every event the board of what needs attention is told of (a job ended, an import, a person's action); when a dashboard is opened; and every 60 seconds. Never sooner than 5 seconds after the reading before. A burst of events is one reading. All parts are read on one worker thread, one after another.
- How an open page hears of it: after a reading the topic `dashboard` is published on the hub. The cards ask for themselves again (`/?part=cards`) and say which cards they have; the answer is 204 when nothing changed. Asking for the cards never causes a reading.
- What a card shows while a value is unknown: a part that was never read says "Counting..." in its card. A part whose reading failed keeps what it last gave.
- Stopping: the reading is called off through its `AxisDbInterrupt`, as the board of what needs attention does, so a stop does not wait on the database.
- A shell that was never started reads at every view, as before. The tests' pages do that.

A part that only administrators see is read for the board whoever is looking, and drawn for administrators only.

## What is not changed

- The scheduler still reads every episode's facts, in its job. Its statement is cheaper (the series without the per-series lookups, the jobs joined), but it still finds each series' episodes by going through all of them.
- The series' lines of Needs attention still go through the series 50 at a time in the background.
- The equivalence test has no converted files and no profile that cannot be used.
