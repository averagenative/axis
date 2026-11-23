# Convert a whole library with a conversion run

This page shows you how to plan a conversion run, read its list, set
limits, start it, and watch it work through your library. Allow about 30
minutes to set up. The run itself takes as long as the files take.

A conversion run works through a list of files for you. The files that would
save the most come first, and a series is kept together so that it finishes
as a whole. Axis converts one file at a time by default, and each file is an
ordinary conversion with its ten checks.

## Before you begin

You need:

- A conversion worker that is connected and can reach your disks
  ([Run conversions on a worker on another machine](11-run-conversions-on-a-worker.md)).
- A conversion profile you have proven by trials, if you want the run to
  replace originals
  ([Prove a conversion profile with trials](09-prove-a-profile-with-trials.md)).
  With a locked profile, a run makes trials until enough have passed. Then
  it pauses, so you can play the trial files, prove the profile, and switch
  the run to replacing originals with the same list.
- **Read-only storage** switched off.
- A backup. A replacement deletes the original once the new file has
  passed every check, or after the time **Keep replaced originals for**
  sets.

## Steps

1. Open **Library**, then **Conversion runs**, and select **Plan
   a conversion run**.
2. In **Files and conversion profile for the run**, choose **Which files**:

   - **Every movie library folder**
   - **Every TV library folder**
   - **The library folders ticked below**
   - **One series (choose it below)**, with the series from **Series on
     disk**

3. In **Conversion profile**, choose the profile in **Convert with**.
4. Select **Make the list**. Axis shows "The run is made: its list is being
   made now (this page follows it). Nothing is converted until you start
   it." The state is **Making the list**, then **Ready to start**.
5. Read **Movies and series in this run**. Each row is a movie or a series,
   with **Rank**, **Files**, **Size now**, **After (estimated)**, **Saving**
   and **Progress**.
   The largest saving comes first. Open a row to see its files. Select
   **Take out of the run** on anything you do not want converted.

   <!-- screenshot: a conversion run list with series grouped and savings in the Saving column -->

6. Read **Files left out, and why**. Every file Axis looked at and would not
   convert is here with its reason, for example **Converted with this
   version already**, **Remembered as not worth it** or **Another
   conversion of it is in flight**. The counts add up, so nothing
   disappears without a reason.
7. In **Start, pause, cancel or limit this run**, under **Limits**, set what
   you want, and select **Save the limits**:

   | Limit | What it does |
   | --- | --- |
   | **Files at once** | 1 to 8, and never more than the connected workers can take. |
   | **Stop after this many files** | A cap on files. A series is kept whole, so the list ends after the series where the cap is reached. |
   | **Only until this many GiB are saved (estimated)** | A cap on the estimated saving. |
   | **A time budget, in hours** | The run asks for nothing after this long. What is in flight finishes. |
   | **Only from (HH:MM)** and **Until (HH:MM)** | A daily window, in the server's local time, such as 22:00 to 06:00. |

8. Choose how to start:

   - **Start with trials**: each file is converted and checked, and the
     converted file is kept aside to be played. The library is not touched.
   - **Start replacing originals**: tick the confirmation ("I understand:
     the originals will be deleted") and select it. This button is offered
     only when the profile's current version is proven.

9. Watch the run page. It shows **State**, **Files**, **Movies and series
   finished**, **Saved so far** and **Files being converted or checked
   now**. Each file has a state:
   **Waiting**, **Waiting (its trial passed)**, a conversion state such as
   **Being converted on a worker**, **Trial done**, **Replaced**,
   **Skipped**, **Failed** or **Taken out**. The card **Estimates and real
   sizes** compares what the converted files came to with what Axis
   estimated.
10. To control the run, use **Pause the run** (what is in flight finishes),
    **Resume the run** and **Cancel the run**. To switch a trial run to
    replacing, after you proved the version, select **Replace originals
    from now on**. The files converted as trials are converted again as
    replacements.

## What the screen shows

Axis stops a run by itself in a few cases and says why on the run page:

- When a set number of files in a row fail or are rejected by a check
  (the setting **Pause a conversion run after failures in a row**), the run
  pauses. Something they share, such as the profile, a worker or a disk, is
  more likely than bad luck. Look at their conversions, and then resume.
- When a worker pauses after an encoder crash, the run pauses too.
- When a disk is offline, its files wait ("an offline disk is not a
  failure") and the run goes on with the others.
- When no connected worker can reach a disk, its files wait too. A file of
  that disk that the run already asked for, and that no worker has taken
  yet, is given back: its conversion says "Given back by conversion run"
  and why, and the file waits in the list again. The run names the disk
  among those whose files wait and goes on with the other disks' files.
- When no worker is connected, the run waits and goes on by itself when one
  is.
- When a conversion ends "Not judged" (Axis changed between the encode and
  the checks), the run asks for that file again. This does not count as a
  failure.

Only one run runs at a time.

## What can go wrong

| What you see | What to do |
| --- | --- |
| "Tick at least one library folder." or "Choose a series." | Complete the form. |
| "There is no usable video conversion profile." | Open **Conversion profiles**, copy a built-in profile, and check that the profile validates. |
| The state is **The list could not be made** | Read the message on the run page. Make the run again. |
| "Nothing would be converted: see what is not in the list, below." | Look at **Files left out, and why**. The profile may skip everything, for example HEVC sources. |
| **Start replacing originals** is missing | The version is locked. "A run with a locked version makes trials until enough have passed, then pauses for you to prove it." |
| "Conversion work has been paused since Axis started" | Select **Resume conversion work**. |
| "Replacing originals needs the profile's current version: this run's is not, so it can only make trials." | Make a new run with the current version. |

## Verified

- Conversion runs have run on generated libraries only (up to 50,000 files
  and 1,500 series), with the tests standing in for the worker's results.
  On a real library, a run has gone only as far as its first trials. No
  run has replaced an original there.
- Everything a run asks of a conversion rests on the single-file
  conversions. Those have passed trials of real movies, and have not
  replaced an original in a real library
  ([Prove a conversion profile with trials](09-prove-a-profile-with-trials.md)).
- Planned and not built yet: a space projection per disk, other orderings,
  a per-disk read limit, standing runs, and estimates corrected from the
  results.
