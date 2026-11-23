# Say rightly whether a series is monitored, and change it in one place

## Why

A series whose seasons are all monitored and wholly in the library read "Not monitored" on its own page, above "Every aired episode is in the library." The series was monitored: only its specials were not. The mark is alarming and wrong.

The series' own state is the worst state of its episodes. "In the library" and "Not monitored" are equally mild, so the episode that was counted first decided between them. A series' page counts the specials first. The lists count the episodes in the order the database returns their groups, so a list could say either.

The monitoring of a series could be changed only one season or one episode at a time, and whether new seasons are monitored could not be changed at all after the series was added. A person who wants future seasons of a series had no way to say so.

## What changes

- A series reads "Not monitored" only when nothing of it is monitored: no season, no episode, and new seasons are not monitored either. Otherwise, a series with nothing under way reads "In library" when an episode of it is in the library, and "Not aired" when none is. Episodes that are not monitored, such as the specials or a season that was left out, never make a monitored series read as not monitored.
- The sentence beside the mark says what happens to new episodes: "New seasons and episodes are monitored as they are announced." A series of which nothing is monitored says so, and says how many of its episodes are in the library.
- The series' own page, the TV list in every view, and the series' card on a title's page use the one rule. The list that the database counts and the page that reads every episode agree.
- A series' page has a "Monitoring" card at the top. It states how the series stands in words, for example "12 of 13 seasons monitored; the specials are not monitored; new seasons are monitored when they are announced." For an administrator the card has one form: what to do with the seasons (leave them, monitor every season, monitor the latest season only, stop monitoring every season), whether the specials go with every season, and whether new seasons and episodes are monitored as they are announced.
- The form changes only what is wanted. No file is deleted, moved or unlinked. Nothing is searched for at once.
- No migration.

## Impact

- Capabilities: `tv`.
- Code: `src/modules/tv/axis-tv-store.c` (the series' state and sentence, the seasons counted with a series), `src/modules/tv/axis-tv.c` (the change of a whole series), `src/modules/tv/axis-tv-sync.c` (the rule of the Add form's choices, shared), `src/modules/tv/axis-tv-web.c` and `src/modules/tv/axis-tv-web-view.c` (the form and the card).
- Tests: `tests/test-tv.c`, `tests/test-web-tv.c`, `tests/test-web-tv-scale.c`.
- Documents: `docs/07-add-a-series.md`.
- Behavior that differs: a series that read "Not monitored" while a season of it was monitored, or while new seasons were to be monitored, reads "In library" or "Not aired". The TV list reads one small count of seasons for each series.
