# Convert the files Axis keeps, by choice, and show progress as bars

## Why

After a migration from Radarr or Sonarr nearly every file carries the kept mark: Axis does not replace it with another release. A conversion run leaves every kept file out, so on a migrated library a run finds almost nothing. One run listed 6 files and left 743 out as kept. The only way round was to let each file go on its own page, which also opens it to upgrades, and that is not what the owner wants.

The kept mark is about replacing a file with another download. A conversion is a different thing: it re-encodes the file that is there, and puts the result in its place only after ten checks. The owner may want that for kept files, and must be able to say so for a run in one step, without giving up the mark.

A conversion in flight says "Running: converting: 94% of the source read (94%)" as text, on a page that has to be loaded again, and a run shows counts. Progress that takes hours should be seen at a glance.

## What changes

- **Plan a conversion run** has a choice, **Also convert files Axis keeps**, off by default, with one sentence on what it means. A run made with it lists kept files like any other.
- A run made without it shows the kept files under **Files left out, and why**, with an action on that row, **Add these N files to the run**. It turns the choice on for the run and adds those files to its list. It is allowed while the run is ready to start, paused or finished, with no file in flight.
- The confirmation of a single replacement of a kept file offers the same choice as a box of its own.
- The choice is stored with the run, with who made it and when, and with each conversion asked for with it. It lifts the condition "kept" and no other, at the request, when a worker takes the work and just before the original is replaced. A conversion never changes the mark: the converted file is as kept as its original was.
- A conversion in flight has a progress bar (the HTML `progress` element) with its words beside it and as its accessible name: the stage in words, the percent, the speed as a multiple of real time and the time left when they can be worked out, and no value while nothing is known. The conversion's page and the **Latest conversions** table follow it without a reload.
- A run's page has a bar of the files settled out of the files within its caps, a bar of the bytes saved so far against the estimate, and a bar for each file in flight. The dashboard's piece of a run has the files bar.
- Migration `0086_convert_kept.sql`: four columns on `conversion_run` and one on `conversion`, all with defaults.

## What does not change

- Off by default, every run and every request behaves as before.
- Every other reason a file is left out, and every other condition and check, is as it was. A kept file that is pinned, of uncertain HDR, already efficient, remembered or in flight stays out for that reason.
- Upgrades, searches and the links pages read the kept mark as before.
- The worker protocol. Speed and time left are worked out by the server from the fraction the worker reports, the time the job has run and the source's duration.
- The Space savings estimates. They never left kept files out, so there is no separate figure to show; the user guide now says so.

## Impact

- Capabilities: `conversion-runs`, `conversion-execution`.
- Code: `src/transcode/axis-conversion-gates.c`, `axis-conversion-run.c`, `axis-conversion-run-list.c`, `axis-conversion-run-step.c`, `axis-conversion-run-read.c`, `axis-conversion.c`, `axis-conversion-verify.c`; `src/web/axis-web-conversion-runs.c`, `axis-web-conversions.c`; `data/web/axis.css`; `data/migrations/0086_convert_kept.sql`.
- Tests: `tests/test-conversion-runs.c`, `tests/test-conversion.c`, `tests/test-web-conversion-runs.c`.
- Docs: `docs/08`, `docs/09`, `docs/10`, `docs/16`.
- Risk: this widens what Axis may overwrite in a library. The one place the mark is passed over is the condition "kept" in `axis_conversion_gates()`, given the choice recorded with the conversion, and the list's same reason, given the run's.
