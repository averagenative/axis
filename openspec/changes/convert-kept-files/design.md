# Design

## Where the kept mark is read, and where it is passed over

The mark is `movie_file.kept` and `series_file.kept`. Conversions read it in two places and write it nowhere.

| Place | Before | Now |
| --- | --- | --- |
| The list of a run (`decide()` in `axis-conversion-run-list.c`) | A kept file is left out with the reason `kept`, before anything else is looked at. | The same, unless the run has `convert_kept`. Then the file goes on to the memory, the conversion in flight and the estimator's decision, as a file that is not kept does. |
| The condition "kept" (`axis_conversion_gates()`, for a replacement) | It refuses a kept file. | It refuses a kept file unless the caller passes the choice. Its words then say that the file is kept, was chosen, and stays kept. |

The gates run three times for a conversion: at the request, when a worker takes the work, and as check 10 just before placement. The request gets the choice from the person's tick or from the run's entry. The two later evaluations get it from `conversion.convert_kept`, which the request wrote. So a conversion asked for without the choice, of a file that is kept by the time it is checked, is rejected by check 10 as before.

The request writes `convert_kept` only when the choice is made and the file is kept at that moment. The choice is about the files an administrator saw kept, not a standing permission: a file that was not kept when it was asked for, and that a person keeps by hand while its conversion is in flight, is refused when a worker takes it, or by check 10, exactly as without the choice. A run's entry records the same thing for its file when the list is made or the kept files are added (`conversion_run_entry.convert_kept`), and the step passes the choice only for an entry that has it, so a file of the list kept later is refused when its turn comes, and skipped with the reason "kept".

Nothing else was changed to read the choice. A trial never needed it, because the condition applies to a replacement only. A run still lists kept files only with the choice, for trials too, so that a run of trials and the run it becomes when it is switched to replacing have the same list.

## The choice is the run's, not the file's

`conversion_run.convert_kept` is set when the run is made, or by "Add these N files to the run", with `convert_kept_by` and `convert_kept_at`. Nothing else writes it. Start, pause, resume, switching to replacing and setting limits do not touch it, so a run keeps its choice through all of them. The step reads it, with the entry's own `convert_kept` and `confirmed_by`, in the statement that marks the entry asked, so a step that began before a pause and an addition asks with the run as it is now. The request's audit line names who made the choice.

A run of the same selection and profile version that has not started is "the same run" for a second click of **Make the list** only when it has the same choice.

## Adding kept files to a list there is

A run's first list is built whole, in one transaction, and replaces anything there. That cannot be used for a paused or finished run: its entries are the record of what the run did.

"Add these N files to the run" therefore adds to the list:

1. In one transaction that locks the run: the run must be ready, paused or finished, must not have the choice, must have no entry in flight and at least one file left out as kept. The run gets the choice, `relist_state` (the state it was in), the state `listing` and a new list job.
2. The list job, seeing `relist_state`, reads only the files the run left out as kept, through the same statement and the same `decide()` as a first list, with the choice on. Each becomes an entry or is left out for its other reason.
3. Still in that one transaction: the rows left out as kept are removed, the reasons' counts are worked out again from the rows left out, the new entries of a unit the list has join it (after its files), the others make new units after those there are (ranked among themselves by estimated saving), a unit a person took out stays out with its new files, and the cap is worked out again.
4. The run goes back to the state it was in. A finished run to which files were added that it can convert (waiting, within its caps, its time budget not used up) becomes paused, saying who added how many, for a person to resume; one that can convert none of them stays finished and says why. No run starts by itself after an addition.

Nothing of an entry or a unit that was there changes, except a unit's totals.

## Adding kept files to a run that replaces originals

The confirmation a run that replaces originals has was given for the list it had. Adding files to it would widen that confirmation after it was given. So on such a run the action carries the sentence and the box of the switch to replacing, saying how many originals are replaced; the request without the tick is refused before anything changes (in `axis_conversion_runs_include_kept()`, so a forged form is refused too). The person who ticks is the one who chooses: `convert_kept_by` and `convert_kept_at` are their confirmation, and the list job copies them to each entry it adds (`conversion_run_entry.confirmed_by`, `confirmed_at`). The step asks for the replacement of such an entry as that person, with the confirmation that is the entry's; every other entry goes by the run's `confirmed_by` as before. The run waits paused, and its resume control says how many originals resuming it will replace (the entries that wait within its caps).

A run of trials adds the files as trials, with no tick: a trial touches nothing. When such a run is later switched to replacing, the switch's confirmation covers its whole list, including the files added. A run that is ready has not chosen yet: starting it to replace originals asks for the confirmation for every file of its list.

It is not offered for a running run, because the step reads the list while it asks for files. It is refused while a file is in flight, so that what is being converted is settled against the list it was asked from. The page says which of the two holds it back.

If the job fails or is lost, nothing of it was committed. The run goes back to its state with the choice off, and says so on its page. A person can cancel the run while the files are being added, as for a first list.

New units come after the units there were, not ranked among them. Ranking them together would renumber units and entries of a run that already asked for some. After a migration the units there were are few, so the order is close to the ranked one. A person who wants one ranked list makes a new run with the choice.

## Progress

The worker reports a fraction and a message with a job's progress ("converting: 94% of the source read"). It does not report frames or speed, and the protocol is not changed here. The server works out:

- the stage, from the conversion's state: waiting for a worker, encoding, checking, replacing, or keeping the trial aside;
- the fraction, from the running job of that stage only (a job that waits to be taken again has the figure of an attempt that is over), or nothing when the job has reported none;
- the time left, as the time the job has run times what is left over what is done;
- the speed, for an encode whose source's duration is known, as the fraction of the duration over the time run.

Speed and time left need ten seconds of running and one percent done, so that the first figures are not wild. The function is pure; the time a job has run is read by the same statement as the rest.

The bar is a `progress` element with `max` 100 and the percent as `value`, or without a value, which a browser draws as indeterminate. The same words are the bar's `aria-label` and a text beside it. The colour comes from the theme through `accent-color`, as for every progress element of the interface.

The pages follow a conversion the way a run's page already follows a run: a block with htmx attributes asks for itself again at an interval while there is something in flight, and what comes back when there is not asks for nothing. No script was added. These blocks are not on the live event stream: a conversion's progress is written by the worker API to the job's row many times a minute, and publishing each of those would put the stream's cost on every open page for the sake of one.

## What was left out

- Frames a second, and the bytes written so far, need the worker to report them.
- The dashboard's piece of a run shows the run's files bar, not the file in flight: the board reads the run's summary only.
- A run's page still takes up to 15 seconds to show a change; a conversion's page takes up to 5.
- The timed requests go on in a tab in the background. A condition on the trigger (`[document.visibilityState === 'visible']`) is evaluated as script by htmx, which the pages' content policy forbids. The Latest conversions list reads a source's duration for the conversions in flight only, so each of its timed requests costs little more than the list without bars.
- A cap worked out again after an addition counts whole units again, so a run can convert more files in all than a fresh list under the same cap would. The guide says so.
