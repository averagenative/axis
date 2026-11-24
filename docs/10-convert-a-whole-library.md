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
4. In **Files Axis keeps**, decide whether to tick **Also convert files
   Axis keeps**. It is off by default. Axis keeps a file when you keep it by
   hand, and keeps nearly every file after a migration from Radarr or
   Sonarr. Without the tick, the run leaves every kept file out. With the
   tick, a kept file is re-encoded in place after every check passes. It
   stays kept, so Axis still does not replace it with another release. You
   can also decide later: see
   [Convert files that Axis keeps](#convert-files-that-axis-keeps).
5. Select **Make the list**. Axis shows "The run is made: its list is being
   made now (this page follows it). Nothing is converted until you start
   it." The state is **Making the list**, then **Ready to start**.
6. Read **Movies and series in this run**. Each row is a movie or a series,
   with **Rank**, **Files**, **Size now**, **After (estimated)**, **Saving**
   and **Progress**.
   The largest saving comes first. Open a row to see its files. Select
   **Take out of the run** on anything you do not want converted.

   <!-- screenshot: a conversion run list with series grouped and savings in the Saving column -->

7. Read **Files left out, and why**. Every file Axis looked at and would not
   convert is here with its reason, for example **Converted with this
   version already**, **Remembered as not worth it**, **Another
   conversion of it is in flight** or **Kept: Axis does not replace this
   file**. The counts add up, so nothing disappears without a reason.
8. In **Start, pause, cancel or limit this run**, under **Limits**, set what
   you want, and select **Save the limits**:

   | Limit | What it does |
   | --- | --- |
   | **Files at once** | 1 to 8, and never more than the connected workers can take. |
   | **Stop after this many files** | A cap on files. A series is kept whole, so the list ends after the series where the cap is reached. |
   | **Only until this many GiB are saved (estimated)** | A cap on the estimated saving. |
   | **A time budget, in hours** | The run asks for nothing after this long. What is in flight finishes. |
   | **Only from (HH:MM)** and **Until (HH:MM)** | A daily window, in the server's local time, such as 22:00 to 06:00. |

9. Choose how to start:

   - **Start with trials**: each file is converted and checked, and the
     converted file is kept aside to be played. The library is not touched.
   - **Start replacing originals**: tick the confirmation ("I understand:
     the originals will be deleted") and select it. This button is offered
     only when the profile's current version is proven.

10. Watch the run page. The card **Progress of this run** follows the run
    while it works. It shows **State**, **Files**, **Movies and series
    finished**, **Saved so far** and **Files being converted or checked
    now**, with progress bars:

    - A bar of the files that are settled (done, skipped or failed), out of
      the files within the run's caps, or out of the whole list when the
      run has no cap.
    - A bar of the space saved so far, against the saving Axis estimated
      for those files.
    - A bar for each file in flight. The words beside it name the stage
      (**Waiting for a worker**, **Encoding**, **Checking**, **Replacing**
      or **Keeping the trial aside**), the percent, the speed as a
      multiple of real time, and an estimate of the time left.

    Each bar has its words beside it, so the page reads the same without
    the bars. A bar that shows no amount means that the work goes on and
    the worker has not reported how far it is.

    Each file has a state:
    **Waiting**, **Waiting (its trial passed)**, a conversion state such as
    **Being converted on a worker**, **Trial done**, **Replaced**,
    **Skipped**, **Failed** or **Taken out**. The card **Estimates and real
    sizes** compares what the converted files came to with what Axis
    estimated.
11. To control the run, use **Pause the run** (what is in flight finishes),
    **Resume the run** and **Cancel the run**. To switch a trial run to
    replacing, after you proved the version, select **Replace originals
    from now on**. The files converted as trials are converted again as
    replacements. The run keeps its choice about kept files when you switch
    it. The form says how many originals the run replaces from now on, and
    how many of them are files that Axis keeps, including kept files you
    added as trials. If someone adds kept files after you load the page,
    Axis refuses your tick and changes nothing, so reload the page and
    confirm again.

## Convert files that Axis keeps

Axis keeps a file when you select **Keep it by hand** on it, and keeps
every file that was migrated from Radarr or Sonarr
([Migrate from Radarr and Sonarr](16-migrate-from-radarr-and-sonarr.md)).
Kept means that Axis does not replace the file with another release. A
conversion is a different thing: it re-encodes the file you have, and
replaces it only after the ten checks pass. So a run leaves kept files out
unless you choose to convert them, and you make that choice for each run.

After a migration, a run without the choice finds little to do. Its list is
short, and **Files left out, and why** shows most of the library under
**Kept: Axis does not replace this file**.

To add those files to a run you already made:

1. Open the run. It must be **Ready to start**, **Paused** or **Finished**,
   with no file in flight. Pause a running run first, and wait for the file
   in flight to finish.
2. In **Files left out, and why**, on the row **Kept: Axis does not replace
   this file**, read what the row says about this run:
   - On a run that replaces originals, the files are replaced too. The row
     says that their N originals are replaced by their converted files once
     every check passes, and shows the same warning and box as **Replace
     originals from now on**. Tick **I understand: the originals will be
     deleted**. Without the tick, Axis refuses and changes nothing. Axis
     records your confirmation for these files, with the time, and asks for
     each replacement in your name.
   - On a run of trials, the files are added as trials. Each is converted
     and checked, and the converted file is kept aside. The library is not
     touched, so there is nothing to tick.
   - On a run that is **Ready to start**, the files join the list. You
     choose trials or replacing when you start the run.
3. Select **Add these N files to the run**.
4. Wait while the state is **Making the list**. Axis decides each kept file
   as it decided the others. A file the profile would convert joins the
   list, after the files that were there. An episode joins its series. A
   file that has another reason of its own, for example **Already
   efficient**, moves to that reason. A file that is no longer in the
   library is not added, so fewer files can be added than the row said.
   Nothing the run already converted, skipped or took out changes.
5. Start or resume the run. The run never starts by itself after you add
   files. A run that had finished waits, paused, for you to select
   **Resume the run**. On a run that replaces originals, the button says
   how many originals resuming it replaces, for example **Resume the run,
   replacing up to 2 originals**. If the run has a cap, raise it under
   **Limits** to reach the added files. A finished run that cannot convert
   any of the added files, because they are beyond its caps, in a movie or
   series you took out, or past its time budget, stays finished and says
   so under **How it ended**. Make a new run with the choice instead.

A cap is worked out again when files are added, and it keeps a series
whole. So a run can convert more files in all than a new list under the
same cap would: the cap counts the files of each whole series again,
including the files it converted before.

The choice covers the files that are kept when you make it: the files of
the list when it is made, or the files you add. A file that was not kept
then, and that you keep by hand later, is not converted: the run skips it
when its turn comes, with **Kept: Axis does not replace this file**. A
single conversion works the same way.

The card **What this run converts, and how** says under **Files Axis keeps**
whether kept files are included, and who chose that and when.

What the choice changes, and what it does not:

- It lifts one reason only. A kept file that is left out for any other
  reason stays out, for example **Pinned** (it has another hard link),
  **HDR uncertain** or **Already efficient**. A kept file on an offline
  disk waits for its disk, like any other file.
- A kept file goes through the same conditions and the same ten checks as
  any other file. A replacement still needs a proven profile and a safe
  playback verdict.
- The file stays kept after it is converted. Upgrades, searches and the
  file's links page treat it as before.
- Only an administrator can make the choice, and Axis logs who made it.
- A trial of a kept file never needed the choice, because a trial replaces
  nothing. A run still lists kept files only with the choice.

To convert one kept file without a run, open the file, choose **Replace the
original** on the **Convert this file** card, and select **Review the
conversion**. When nothing else holds the file back, the page offers **Also
convert files Axis keeps** as a box of its own. Tick it, tick **I
understand: the original will be deleted**, and select **Replace the
original**. Without the first tick, Axis refuses with "Kept: Axis does not
replace this file."

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
| Nearly every file is under **Kept: Axis does not replace this file** | The library was migrated, or you keep its files by hand. Select **Add these N files to the run** on that row. See [Convert files that Axis keeps](#convert-files-that-axis-keeps). |
| "Pause the run to add these files." or "You can add these files once nothing of the run is in flight." | Pause the run, wait for the file in flight to finish, and then select **Add these N files to the run**. |
| "This run replaces originals: adding the files Axis keeps replaces their originals too" | The run replaces originals, and you did not tick the box. Read the warning on the row, tick **I understand: the originals will be deleted**, and select **Add these N files to the run** again. |
| "The files Axis keeps were not added, and the run is as it was" | The job that adds them stopped. The run and its list did not change, and the choice is off again. Read the rest of the message, and then select **Add these N files to the run** again. |
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
- Converting kept files has run in the tests only: one kept movie file
  replaced by a real encode on a generated clip, the claim, the last check
  and the placement of a kept file with a stand-in output, and runs on
  generated libraries. No kept file in a real library has been converted.
- The progress bars have been checked in the tests as HTML. They have not
  been looked at in a browser in every theme. The speed and the time left
  are worked out from the percent the worker reports and the time the job
  has run. The worker does not report frames a second.
- Planned and not built yet: a space projection per disk, other orderings,
  a per-disk read limit, standing runs, and estimates corrected from the
  results.
