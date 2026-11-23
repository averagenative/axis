# Tasks

## 1. The series' own state

- [x] 1.1 Read each series' seasons, counted, with the series (`SERIES_SELECT_OF`).
- [x] 1.2 Count the monitored episodes in the summary, on both paths.
- [x] 1.3 Decide the state of a series with nothing under way by what is monitored (`summary_say()`, `axis_tv_series_monitored()`).
- [x] 1.4 Say what happens to new episodes, and say when nothing is monitored.

## 2. The whole-series control

- [x] 2.1 Share the rule of the Add form's choices (`axis_tv_monitor_takes_season()`).
- [x] 2.2 Change a whole series in one transaction, with one history line (`axis_tv_set_series_monitoring()`).
- [x] 2.3 Take the form on `POST /series/:id/monitor` (`whole`, `specials`, `new`), and come back to the card with a notice.
- [x] 2.4 The "Monitoring" card: the state in words for everyone, the form for administrators.

## 3. Tests

- [x] 3.1 The change of a whole series: every choice, the box alone, nothing changed, the history, a migrated series, no file or link changed, nothing queued (`/tv/whole-monitoring`).
- [x] 3.2 The page and the list for the series that read "Not monitored"; the form for an administrator, a viewer, and a forged post (`/web/tv/series-monitoring`).
- [x] 3.3 Five settled series counted and derived alike (`/web/tv-scale/counted-as-derived`).
- [x] 3.4 Run `test-tv`, `test-web-tv`, `test-web-tv-scale` and `test-web-attention` with the sanitizers.
- [x] 3.5 Measure the list's statement with and without the count of seasons at the scale fixture's size (see the design).
- [x] 3.6 Style the card (`data/web/axis.css`), phone width included.

## 4. Documents

- [x] 4.1 `docs/07-add-a-series.md`: the "Monitoring" card and what a series' mark means.

## 5. Not done here

- [ ] 5.1 A bulk control on the TV list (see the design).
