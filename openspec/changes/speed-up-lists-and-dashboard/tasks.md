# Tasks

## 1. Dashboard

- [x] 1.1 Keep each part's figures in memory and read them on a worker thread, with the stop of the board of what needs attention (`AxisDbInterrupt`).
- [x] 1.2 Read again on the shell's events, when a dashboard is opened and on a timer, with a least gap.
- [x] 1.3 Publish the topic `dashboard` after a reading; answer `/?part=cards` from memory, with 204 for the cards a page has.
- [x] 1.4 Say "Counting..." in the card of a part not read yet.
- [x] 1.5 Start the board in the daemon.
- [x] 1.6 Test: a started dashboard answers while a reading is held, runs no reading on a view, updates after an event, and stops with a reading on its thread (`/web/dashboard/started`).

## 2. TV lists

- [x] 2.1 Count the episodes by class in one statement and derive each class's state with the one rule.
- [x] 2.2 Read the series of a list without the per-series lookups.
- [x] 2.3 Join the searches in flight, the open grab and the downloads left at a client in the episode facts.
- [x] 2.4 TV list: counts from the classes; the page's series that need attention read further.
- [x] 2.5 Wanted: pages of 100, the facts of the page's episodes only.
- [x] 2.6 Calendar: the episodes of its three weeks only.
- [x] 2.7 Dashboard's TV figures from the classes.
- [x] 2.8 Scheduler: the series as a list reads them.

## 3. Tests

- [x] 3.1 The counts, states and sentences agree with every episode's facts for 300 generated series in every state (`/web/tv-scale/counted-as-derived`).
- [x] 3.2 Wanted's pages reach every wanted episode once (`/web/tv-scale/wanted-pages`).
- [x] 3.3 On 1,500 series with files, searches, grabs and 160,000 jobs: every page is timed and bounded, reads a page's worth of episodes, and the started dashboard sends no statement (`/web/tv-scale/pages`).

## 4. Not done here

- [ ] 4.1 The scheduler's pass over every episode in C.
- [ ] 4.2 The series' lines of Needs attention from the classes.
- [ ] 4.3 A shorter default for how long finished jobs are kept.
