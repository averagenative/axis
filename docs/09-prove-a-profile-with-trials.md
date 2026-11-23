# Prove a conversion profile with trials

This page shows you how to convert one film as a trial, read the ten checks
Axis runs on the result, play the file, and prove the profile so that Axis
may replace originals with it. Allow about an hour of your time, plus the
time an encode takes.

A trial never touches your library. The converted file is kept aside in the
disk's work area (`.axis-work/` at the top of the disk) so that you can play
it. Axis replaces an original only after the profile is proven, and only
after the new file has passed every check.

## Before you begin

You need:

- A conversion worker that is connected and can reach the disk that holds
  the film. A worker can run on the same machine as Axis. If you want one
  on a faster machine, read
  [Run conversions on a worker on another machine](11-run-conversions-on-a-worker.md)
  first. A trial queues and waits until a worker takes it.
- **Read-only storage** switched off in **Settings** (**Storage** card). The
  work area is written to disk, so the condition "Storage may be written
  (Read-only storage is off)" must hold, even for a trial.
- At least three films you can play on your household's players, so that
  the proof rests on different files. The setting **Passed trials
  needed to prove a conversion profile version** (**Conversions** card)
  sets the number. Three is the
  default.
- A read of [See what converting would save](08-see-what-converting-would-save.md).

After every start, Axis holds conversion work until you press **Resume
conversion work** on the **Conversions** or **Conversion workers** page.
This keeps a server that stopped under a conversion's load from starting
that load again by itself. Downloads, imports and scans go on.

## Steps

1. Open **Library**, then **Files**, and open a video file. An
   administrator sees a **Convert this file** card.

   <!-- screenshot: a file page with the Convert this file card -->

2. In **Conversion profile**, choose a profile. A profile that is not
   proven says "locked: trials only". In **Trial or replacement**, choose
   **Trial: the original stays**. Select **Review the conversion**.
3. Read the page **Try a conversion**. **What will happen** names the file,
   its size, the conversion profile, the estimated size after, the
   **Playback verdict** (**Safe**, **Risky** or **Unknown**), the
   **Encoder** and the **New name**. The **Conditions for this conversion,
   checked now** table lists each condition with **What must hold**,
   **Now** (**Holds**, **Refuses** or **Not needed**) and **What was
   found**. The conditions include that storage may be written, that the
   disk is online and mounted read-write, that a worker can make this
   conversion and that enough free space stays on the disk.
4. If a condition refuses, the card says "This conversion cannot start now:"
   with the reason. Fix it and try again. Otherwise select **Start the
   trial**.
5. Open **Library**, then **Conversions**. The conversion
   moves through **Waiting for a worker**, **Being converted on a worker**,
   **Being checked** and **Done**. Open it to see progress. The card **The ten
   checks of the converted file** lists ten checks, each **Passed**,
   **Failed**, **Not run** or **To come**:

   | Check | What it asks |
   | --- | --- |
   | 1 | The converted file is the one asked for |
   | 2 | The original is unchanged |
   | 3 | The converted file has the planned streams |
   | 4 | The converted picture has the planned properties |
   | 5 | The converted file decodes whole |
   | 6 | The converted file is as long as the original |
   | 7 | The copied tracks are the original's |
   | 8 | The picture matches the original's |
   | 9 | The saving is worth it |
   | 10 | The conditions still hold |

   The first check that fails rejects the file, removes it and leaves the
   original as it was. The state then says **Rejected by a check**.

   Axis judges a trial against the plan it gave the worker. If you change a
   setting such as **Software encoders' preset**, or the worker measures its
   encoders again, while the trial waits for its checks, the trial is still
   judged against that plan. If Axis itself changed in the meantime (an
   upgrade), the conversion ends **Failed** with a message that starts with
   "Not judged". That is not a rejection: it does not count against the
   conversion profile, and you can convert the file again.

   <!-- screenshot: a conversion page with the ten checks passed -->

6. When the state is **Done**, play the trial file on your household's
   players. The conversion's page shows where it is kept. By default it
   stays for 14 days (the setting **Keep trial files for**), or until you
   remove it from the page.
7. Repeat steps 1 to 6 on two more films.
8. Open **Conversions**. In **Profile versions being proven**, the profile
   shows **Locked: trials only** and its **Passed trials** against **Trials
   needed**. When the passed trials reach the number needed and no check of
   the converted file rejected one, a **Review and prove this version**
   button appears. Select it.
9. Read the page **Prove a conversion profile version by hand**. Tick "I
   played these N trial files on the household's players and they played
   correctly: ... may replace files, deleting their originals", and select
   **Prove this version**. Axis records your name, the time and the trials
   it rests on.

The version now shows **Proven by hand**. A proven profile still needs a
safe playback verdict for the film's library folder, and every other
condition to hold, before Axis replaces an original. To replace one file,
open its page, choose **Replace the original** in **Trial or replacement**,
read the conditions,
tick **I understand: the original will be deleted**, and select **Replace
the original**. The old file waits in the disk's recycle folder for the
time **Keep replaced originals for** sets (the default is 0, which removes
it as soon as the new file is in place).

## What can go wrong

| What you see | What to do |
| --- | --- |
| **Refused: a condition did not hold** | Read the conditions on the conversion's page. A common one is the playback verdict not being safe. |
| **Rejected by a check** | Open the conversion and read the failed check and "What was found". Nothing was changed. |
| **Failed**, "Not judged: ..." | Axis changed between the encode and the checks, so it could not judge the file against the plan it gave. Nothing was changed, and the profile can still be proven. Convert the file again. |
| **Waiting for a worker** forever | No worker can reach the disk, or conversion work is paused after a restart. Open **Conversion workers**, and select **Resume conversion work**. |
| "Storage is read-only (Settings, Read-only storage): nothing is converted until ..." | Switch **Read-only storage** off. |
| **Review and prove this version** does not appear | Fewer trials passed than needed, or a trial was rejected by a check of the converted file. If that rejection was not the profile's fault (a check that was wrong and has been fixed since, for example), open the trial's page, write why in **Set this trial aside**, and select the button. The trial then counts neither for nor against the profile, its page says who set it aside and why, and a conversion run of trials that paused for it can be resumed. When a later version of Axis finds that a check was too strict, its earlier rejections stop counting by themselves, and their pages say why. When it finds that a check was too lenient, trials that passed before stop counting towards proving, so you may need more trials. |
| A leftover under **Leftover files in work areas** | A worker died and left a file. Axis removes it after the time **Remove work-area leftovers after** sets, and never removes a name that is not its own. |
| A replacement is refused on a network disk | The setting **Replace files on network filesystems** is off by default. Trials are allowed. |

## Verified

- The automated tests convert generated clips.
- On a real server, trial conversions of real movies have passed all ten
  checks. A hardware encoder on a worker on another machine encoded them
  through an sshfs mount.
- Before that, a first trial on real hardware did not finish its checks.
  Check 4 was then changed to compare what the picture means (an untagged
  range is limited range) and the check messages name what differed.
- Proving a version, replacing an original and the recycle folder have
  run in the tests only. No original in a real library has been replaced.
