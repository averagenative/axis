# Tasks

## 1. The choice, stored

- [x] 1.1 Migration `0086_convert_kept.sql`: `conversion_run.convert_kept`, `convert_kept_by`, `convert_kept_at`, `relist_state`; `conversion.convert_kept`; the checks that keep them consistent.
- [x] 1.2 The condition "kept" takes the choice (`axis_conversion_gates()`); the request records it; the claim and check 10 read it from the conversion's row.
- [x] 1.3 `axis_conversions_request_full()` and `axis_conversions_preview_full()`; `axis_conversions_request_for_run()` takes the run's choice.
- [x] 1.4 A run is made with or without the choice (`axis_conversion_runs_create_full()`); the list passes over the reason `kept` for a run that has it; the step passes it to every request.
- [x] 1.5 Audit lines name who made the choice: making a run, adding kept files, and each request of a kept file.

## 2. Adding kept files to a run

- [x] 2.1 `axis_conversion_runs_include_kept()`: ready, paused or finished, no file in flight, not chosen already.
- [x] 2.2 The list job adds to a list there is: only the files left out as kept, into the units there are or new ones after them, reasons counted again, cap worked out again.
- [x] 2.3 A finished run with files added waits paused; a failed or lost job leaves the run as it was, the choice off.
- [x] 2.4 Cancelling a run while files are added.

## 3. Pages

- [x] 3.1 Plan a conversion run: the choice, off, with its sentence.
- [x] 3.2 A run's page: **Files Axis keeps** under **What this run converts, and how**; **Add these N files to the run** on the kept row, and what to do when it is not offered.
- [x] 3.3 The confirmation of a single replacement of a kept file: the choice as a box of its own; the conversion's page says it was asked for with it.

## 4. Progress

- [x] 4.1 `axis_conversion_progress()`: stage, fraction, speed and time left from what the worker reports today.
- [x] 4.2 A conversion's page and the Latest conversions table: the bar with its words, following the conversion (`/conversions/:id/progress`, `/conversions/latest`).
- [x] 4.3 A run's page: files settled within the caps, bytes saved against the estimate, each file in flight; the dashboard's piece: the files bar.
- [x] 4.4 Styles for the bar in `data/web/axis.css`, with the theme's variables only.

## 5. Tests

- [x] 5.1 Off by default leaves kept files out as before; chosen, they are listed, a kept file with another reason stays out, the requests carry the choice, the marks are unchanged (`/conversion-runs/kept/by-choice`).
- [x] 5.2 Trials to replacing keeps the choice (`/conversion-runs/kept/trials-then-replace`).
- [x] 5.3 Adding to a ready run; refused for other states, a file in flight, twice, no person; a lost job (`/conversion-runs/kept/added-to-ready`).
- [x] 5.4 Adding to a paused run with files done, and to a finished one (`/conversion-runs/kept/added-later`).
- [x] 5.5 An episode joins its series; a unit taken out stays out; the cap again (`/conversion-runs/kept/added-to-series`).
- [x] 5.6 A kept file converted by a real worker through the ten checks, replaced, still kept; a file kept mid-flight without the choice rejected by check 10 (`/conversion/replace-kept-by-choice`, `/conversion/gate-kept`).
- [x] 5.7 The pages: only an administrator with the token makes the choice, on the plan, on the run and on a single request (`/web/conversion-runs/kept`, `/web/conversion-runs/kept-single`).
- [x] 5.8 The bars: no value before any progress, 0, 50 and 100 of 100, the words beside the bar and as its name, the blocks that follow and stop following, a run's bars (`/web/conversion-runs/progress`).

## 6. Docs

- [x] 6.1 `docs/10`: the choice when planning, the section on kept files, the bars; `docs/09`: the single file and the bars; `docs/08` and `docs/16`: what kept means for estimates and conversions.

## 7. After the review

- [x] 7.1 Adding kept files to a run that replaces originals needs the tick and the sentence of the switch to replacing; without it the request is refused and nothing changes; the confirmation is recorded for each file added (`conversion_run_entry.confirmed_by`, `confirmed_at`) and its replacement is asked for as that person.
- [x] 7.2 A run of trials says that the files are added as trials; a run never starts by itself after an addition; the resume control of a replacing run says how many originals it will replace.
- [x] 7.3 The choice covers a file kept when it is asked for: the request records `convert_kept` only for a file kept then, and a run's entry records whether its file was kept when listed or added (`conversion_run_entry.convert_kept`); a file kept later is refused at the claim, at check 10, or when its turn in a run comes.
- [x] 7.4 A finished run that can convert none of the files added stays finished and says why; the step reads the run's choice as it marks an entry asked.
- [x] 7.5 The progress block answers a conversion that is gone with a block that asks for nothing more; a fraction past the end is the end; speed and time left guarded; the source's duration is read for conversions in flight only.
- [x] 7.6 Tests: the claim, check 10 and the placement with the choice without an encoder (`/conversion-runs/kept/claim-check-place`); a file kept after listing (`/conversion-runs/kept/after-listing`); adding to a replacing run, with and without the tick (`/conversion-runs/kept/added-to-replacing`, `/web/conversion-runs/kept-replacing`); beyond the cap (`/conversion-runs/kept/added-beyond-cap`); the failed addition (`/conversion-runs/kept/add-fails`); pinned, remembered and in flight (`/conversion-runs/kept/other-reasons`); the figures at their edges (`/web/conversion-runs/progress-figures`).

- [x] 7.7 The switch to replacing originals says how many originals the run replaces from now on and how many of them are files Axis keeps, and refuses (409, nothing changed) when the run holds more kept files than the form showed; the add form of a run that waits for its profile to be proven says that the files are replaced after the switch, which asks again; a paused replacing run with nothing waiting resumes with the plain label (`/web/conversion-runs/kept-replacing`).

## 8. Not done here

- [ ] 8.1 A kept file converted in a real library, and the bars looked at in a browser in every theme.
- [ ] 8.2 Frames a second and bytes written, which need the worker to report them (a change to the worker protocol).
- [ ] 8.3 The file in flight on the dashboard's piece of a run.
- [ ] 8.4 A run's and a conversion's progress on the live event stream in place of the timed requests (which also go on in a background tab: a visibility condition on the trigger needs script evaluation, which the pages' content policy forbids).
- [ ] 8.5 Kept files added to a running run, and ranked among the units a run already had.
- [ ] 8.6 A cap worked out again after an addition counts the whole units again, so a run can convert more files in all than a fresh list under the same cap would (the guide says so).
