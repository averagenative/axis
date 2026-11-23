# Design

## Why the series read "Not monitored"

`summary_count()` in `src/modules/tv/axis-tv-store.c` keeps, as the series' state, the state of the episode counted first, and replaces it only with a state of greater severity. In `state_table` (`src/modules/tv/axis-tv-state.c`), "in_library" and "unmonitored" both have severity 0. When no episode is wanted, in flight, not aired or in need of attention, every episode is one of those two, and the first one counted wins.

- A series' own page reads its episodes by season and number (`axis_tv_summary_new()`). The specials are season 0, so a special that is not monitored and has no file is counted first: "Not monitored".
- The lists count groups of episodes (`classes_select`, added by `speed-up-lists-and-dashboard`). The statement orders the groups by series only, so the group counted first is whichever the database returns first. The list could say either.

The equivalence test `/web/tv-scale/counted-as-derived` did not catch the disagreement, because every generated series had an episode in a worse state.

## The rule

The rule is applied in one place, `summary_say()`, which both paths call after they count. It applies when the worst state has severity 0 and the series has episodes.

| Anything monitored? | An episode in the library? | State |
| --- | --- | --- |
| No | Any | Not monitored |
| Yes | Yes | In library |
| Yes | No | Not aired |

"Anything monitored" is `axis_tv_series_monitored()`: a season's own tick, the specials' tick, a monitored episode, or `series.monitor_new`. A series with a worse state keeps it, whatever is monitored: a download in flight is still shown.

The third row is a series that waits: for example, an ended series added with "Nothing yet" whose new seasons are monitored. "Not aired" with the sentence "No monitored episode has aired. New seasons and episodes are monitored as they are announced." says that.

### What the rule reads

The episodes' own ticks are counted by both paths already (`AxisTvSummary.monitored` is new). The seasons' ticks are not in the counted classes, and a season with no episode yet has no class at all. Each series therefore carries four small facts: the number of its seasons, the number that are monitored, whether it has specials and whether they are monitored. One subquery reads them from `series_season` by its unique index `(series_id, number)`, in the list's statement and in the page's.

`speed-up-lists-and-dashboard` made the list's statement one join with no subquery a row. This change adds one. It is an index probe that returns a handful of rows for each series. The alternative, joining the seasons grouped by series, reads the whole of `series_season` when one series is asked for, which every action on a series does.

Measured on the scale fixture (1,500 series, 36,000 episodes, `/web/tv-scale/pages`): the list's statement takes 9.7 ms with the count and 4.8 ms without it. The TV list's page takes 228 to 230 ms either way, so the count is left as a subquery.

### The sentence

- Not monitored: "No season is monitored, and new seasons are not: Axis searches for nothing of it." and, when there are files, "8 episodes are in the library."
- Everything monitored is in the library: "Every aired episode is in the library." When some episodes are neither monitored nor in the library, the sentence is "Every monitored episode that has aired is in the library.", because the other one is not true of the specials.
- Then, for a series with nothing under way: "New seasons and episodes are monitored as they are announced." when `monitor_new` is on. When it is off and the series is continuing with something monitored: "New episodes of a monitored season are monitored as they are announced; new seasons are not."

The sentences for series with something under way are unchanged.

## The whole-series control

### The form

One form in a "Monitoring" card under the series' head, above "Episodes by season". It posts to the existing route, `POST /series/:id/monitor`, which is an administrator's route with the shell's CSRF check. The field `whole` tells the handler that the change is for the whole series.

| Field | Values | Meaning |
| --- | --- | --- |
| `whole` | `keep`, `all`, `latest`, `none` | What to do with the seasons |
| `specials` | `yes` or absent | With `all`: the specials too |
| `new` | `yes` or absent | New seasons and episodes are monitored as they are announced |

A select and one button, not several buttons: the box for new seasons is saved with every submit, and one button makes that plain.

### What each choice does

| Choice | Seasons | Specials | Series' choice |
| --- | --- | --- | --- |
| Leave the seasons as they are | Unchanged | Unchanged | Unchanged |
| Monitor every season | All monitored | Unchanged, or monitored with the box | "Every episode" |
| Monitor the latest season only | Only the latest monitored | Not monitored | "The latest season" |
| Stop monitoring every season | None monitored | Not monitored | "Nothing yet" |

A season is changed with all its episodes, as the season's own form changes it. Which season a choice takes is decided by `axis_tv_monitor_takes_season()`, the rule the Add form's choices follow, moved out of `axis-tv-sync.c` so that there is one. "Every season" skips the specials unless they are asked for; that is the one difference from adding a series, where the specials start unmonitored.

"Future episodes only" and "The first season" are not offered. The first is decided an episode at a time by air date when a series is added, and "Leave the seasons as they are" with the box ticked does what a person means by it later. Nobody asked for the second.

### Episodes that have files

Only `series_season.monitored`, `series_episode.monitored`, `series_episode.next_search_at`, `series.monitor_new` and `series.monitor_choice` are written. No statement reads or writes a file, a link between a file and an episode, a grab or a download.

- An episode with a file that stops being monitored stays in the library. It is no longer upgraded.
- An episode with a file that becomes monitored is in the library, and wanted for an upgrade if its file is below the profile's cutoff and is not kept.
- An episode without a file that becomes monitored is wanted once it has aired.
- An episode without a file that stops being monitored is not searched for. A download of it in flight goes on.

### Searching

A change of one season queues nothing: it clears `next_search_at` of the episodes it monitors, and the scheduler takes them on its next pass, as whole seasons where enough of one is wanted. The whole-series change does the same. Monitoring every season of a long series can make hundreds of episodes wanted; queuing a search for each at once is what the scheduler's pacing exists to prevent. A person who wants them now selects "Search for every wanted episode".

The hold of a migrated series is not ended by a change of monitoring. "Let Axis search for it" ends it.

### A migrated series

Its choice is "As in Sonarr" (`brought`). Changing the box for new seasons alone leaves that. A choice for the seasons replaces it with the choice made, and the history line says what it was.

### The history

One line of the kind `monitored`, with the user, only when something changed. Examples:

- "Every season monitored, the specials left as they were by root (3 seasons and 31 episodes changed). What to monitor was "As in Sonarr" until now. No file was touched, and nothing was searched for at once."
- "New seasons are monitored when they are announced from now on by root. No file was touched, and nothing was searched for at once."

## A bulk control on the TV list

Not in this change. It would need:

- A selection on the list: a tick for each row and card, and a choice of everything the filter matches, across pages.
- A route that takes many series, and changes each in its own transaction so that one locked series does not hold the rest. `axis_tv_set_series_monitoring()` is that transaction.
- A confirmation page that says how many series and episodes change before anything is written, as the migration's pages do.
- A job, when the selection is large, so that the request does not wait for thousands of series.
- A limit on what becomes wanted at once, or the plain statement that the scheduler paces it.
- A history line for each series, and one audit line for the request.

## Not done

- The Add form does not offer "new seasons" apart from the choice. The choice still decides it when a series is added.
- The "Needs attention" lines of a series are unchanged.
