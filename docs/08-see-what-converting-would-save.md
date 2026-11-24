# See what converting would save

This page shows you how to read Axis's estimate of the space you would free
by re-encoding your library, and how to tell which conversions your players
can handle. Nothing on this page changes a file. Allow about 15 minutes.

The estimate is made before anything is converted: what would be converted,
about how big it would be afterward, and what would be skipped and why.

## Before you begin

You need a scanned library whose file details have been read
([Add disks and library folders](02-add-disks-and-library-folders.md)).
You do not need a download client, an indexer or a worker. Storage can stay
read-only.

## Steps

1. Open **Library**, then **Space savings**. The page opens on the
   **Overview** tab.
2. If the page says "No estimate yet", wait for the scan and the reading of
   file details to finish, and then select **Refresh the estimates now**.
   Axis also refreshes after each scan.
3. Read the **Conversion profiles side by side** table. Each row is a
   conversion profile:

   | Column | What it tells you |
   | --- | --- |
   | **Conversion profile** | A built-in profile. For video: **Video: Space saver**, **Video: Balanced** and **Video: Maximum compatibility**. |
   | **Files to convert** | How many files the profile would convert. |
   | **Size now** and **Size after** | The size of those files now, and estimated after. |
   | **Saving** | The estimated saving, as a range. |
   | **Files skipped** | How many files the profile would leave alone. |
   | **Accuracy** | How sure the estimate is: **Based on measurements**, **Partly based on measurements** or **Informed guess**. |
   | **Plays on your devices** | Appears when a media server is connected. See step 6. |

   Video profiles and the music and audiobook profiles apply to different
   files, so their savings do not add up.

   The estimate counts every file, including the files that Axis keeps
   (files you keep by hand, and files that were migrated from Radarr or
   Sonarr). A conversion run leaves kept files out unless you choose to
   convert them, so after a migration a run's list can be much shorter than
   this table suggests. To reach the whole saving, see
   [Convert files that Axis keeps](10-convert-a-whole-library.md#convert-files-that-axis-keeps).

   <!-- screenshot: the Space savings overview with the three video profiles -->

4. Read the notice above the table. At first it says **Informed guesses
   only**: the estimates come from how the encoders usually behave, and not
   from conversions of your files, so the ranges are wide on purpose. A
   measurement on the **Accuracy** tab, or your first real conversions,
   narrows them.
5. Open one profile's tab, for example **Video: Balanced**. The page has:

   - **What this profile does**, in words.
   - **Saving across the whole library**: **Files it is for**, **Size of
     those files**, **Would be converted**, **Would be skipped**, **Current
     size of the files to convert**, **Size after** and **Saving**.
   - Breakdowns **Saving by library folder**, **Saving by media type**,
     **Saving by resolution class**, **Saving by current video codec** and
     **Saving by current audio codec**.
   - **Projected free space per disk**: **Free now**, the saving, and
     **Free after**.
   - **Why files would be skipped**: each file is counted once, under the
     first reason that applies, so you can see why a saving is smaller than
     you hoped.
   - **The files with the largest estimated saving**.

6. If you connected Plex or Jellyfin (Setup guide step **Players**), open
   **Library**, then **Player compatibility**. For each media server library
   it recommends a conversion profile, "Safe for all N devices that play it;
   saves about X", or **No profile is safe yet**. A conversion profile is
   safe for a media server library only when every device that played the
   library has been seen playing that combination directly. A device never
   seen with it makes the playback verdict **Unknown**, and unknown is never
   safe. This is because a conversion cannot be undone once the original is
   gone.

## What can go wrong

| What you see | What to do |
| --- | --- |
| "No estimates for this conversion profile yet." | Select **Refresh the estimates for this profile** on the profile's tab. |
| A disk shows "Offline: last known" | The disk is offline. The figures are from when it was last online. |
| The saving is a wide range | The estimate is an informed guess. Run a trial, or measure a profile on a few of your files on the **Accuracy** tab. |
| "The media server has listed no library yet." | Check the connection on **Player compatibility**, and select **Test the Plex connection** (or the one for your server). |
| "No media server library is mapped to a library folder yet, so there are no verdicts." | Map each media server library to a library folder under **Media server libraries and paths**. |
| **No profile is safe yet** for every library | The devices have not been seen playing the conversion profile. Import the play history, or wait for sessions to be seen. |

## Verified

- The estimator and the pages are built and tested. Every estimate on a
  library that has never been converted is an informed guess, and the page
  says so. The ranges have not been compared with real conversions of a
  real library, because none has run yet.
- The queue records no run of the player compatibility pages against a
  real Plex or Jellyfin. The separate feature that tells a media server to
  rescan after an import was checked against a real Jellyfin, and the queue
  says not against a real Plex. Treat Plex as unproven.
