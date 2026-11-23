# Speed up the lists and the dashboard

## Why

On a library of 2,400 movies, 206 series and 22,500 episodes, with a job table of 160,000 rows, the dashboard took 10 to 12 seconds to open, the TV list 9 to 10 seconds and Wanted 8 to 9 seconds (288 kB of HTML). The other pages took milliseconds.

Each of the three slow pages read every series with what only a series' own page shows, and then the facts of every episode of the library, with several lookups for each episode. The pages then worked out each series' state from all its episodes. The bare scale fixture, 1,500 series with no files, searches or jobs, hid the cost.

The dashboard is the page that is opened most. It must open at once, show what it last knew, and update by itself.

## What changes

- The dashboard is drawn from memory. The figures of every card are read in the background, when something happened, when a dashboard is opened and every minute. An open dashboard is told after each reading and updates its cards without a reload. A card whose figures were not read yet says "Counting...".
- The TV list, Wanted, the calendar and the dashboard's TV figures no longer read every episode. The database counts the episodes by the facts a state is derived from, and the one function that derives a state is given each group. The counts, states and sentences are the same as before.
- Wanted is paged, 100 episodes a page, and says how many there are in all.
- The searches in flight are read once and joined, not looked up for each episode.
- No migration. The index the searches in flight need is there already.

## Impact

- Capabilities: `web-shell` (the dashboard), `tv` (the list, Wanted, the calendar).
- Code: `src/web/axis-web-dashboard.c`, `src/modules/tv/axis-tv-store.c`, `src/modules/tv/axis-tv-web-view.c`, `src/modules/tv/axis-tv-web.c`, `src/modules/tv/axis-tv-follow.c`.
- Tests: `tests/test-web-tv-scale.c` (new), `tests/test-web.c`.
- Behavior that differs: Wanted lists 100 episodes a page where it listed the first 500. A part of the dashboard that only administrators see is read in the background for the board, and still shown to administrators only.
