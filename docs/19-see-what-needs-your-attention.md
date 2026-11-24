# See what needs your attention

This page shows you how to find out, from any page, that something in Axis
is waiting for a person, and how to deal with it. Allow about two minutes.

Axis stops in a few places where only a person can decide, and some things
break in ways that Axis cannot mend. Each of these is shown on its own page.
The bell in the header gathers them, so that you do not have to look on
every page.

## Before you begin

You need an administrator account. Every notice that Axis has today is
dealt with by an action that only an administrator has, so only
administrators see the bell. Other accounts see no bell, and the page behind
it refuses them.

## Read the bell

The bell is at the top of every page, to the right of the search box. On a
phone it is in the same line as the search box and stays at the top of the
screen when you scroll.

| What you see | What it means |
| --- | --- |
| A muted bell with no number | Nothing needs attention. |
| A bell with a number | That many notices are waiting. One notice can stand for many things, for example "3 imports are blocked". |
| A number in the warning color | Something waits for you, or is worth a look. |
| A number in the danger color | At least one notice is **Not working**. |
| **99+** | There are more than 99 notices. |

The number changes while the page is open. You do not need to reload.

## Open the page

1. Select the bell. The **Needs attention** page opens.
2. Read the notices. They are in up to three groups, the most serious
   first:

   - **Not working**: something is broken, and media cannot arrive or be
     stored until it is mended. Example: "The disk Attic has been offline
     for 25 minutes."
   - **Waiting for you**: Axis stopped at a point where only a person can
     decide. Example: "3 imports are blocked: each needs a person to decide
     what to do with its files."
   - **Worth a look**: something is degraded, and the rest still works.
     Example: "The indexer Lanternfish refused its API key." while another
     indexer still answers.

3. Select the link under a notice, for example **Open the disk** or **Open
   Imports**. The page where you deal with it opens.
4. Deal with the cause there. The notice goes away by itself. You do not
   mark it as done.

## What Axis tells you about

| Notice | When it appears | Where you deal with it |
| --- | --- | --- |
| A disk is offline | After the disk has been offline for 10 minutes | The disk's page under **Library**, **Disks** |
| A disk is not recognized at its path | At once | The disk's page |
| Imports are blocked or partly done | At once | **Activity**, **Imports** |
| Movies need attention | At once | **Media**, **Movies**, **Wanted** |
| Series need attention | At once | **Media**, **TV**, **Wanted** |
| A download client refused the password or key | At once | The client's page under **System**, **Download clients** |
| A download client does not answer, or fails | After 30 minutes | The client's page |
| An indexer refused its API key | At once | The indexer's page under **System**, **Indexers** |
| An indexer does not answer, or fails | After 30 minutes, and only when Axis has contacted it at least twice | The indexer's page |
| A details provider refused its key | At once | **System**, **Movie and TV details** |
| Monitored movies migrated from Radarr have no file, or series migrated from Sonarr have monitored episodes without a file, and Axis does not search for them until you select **Start searching**. Migrated movies and series that have their files give no notice. | At once | The migration's page under **System**, **Migrate** |

Axis also tells you about the following. Each notice ends with a link that
names the page where you deal with it, for example **Open Identify**. The
table gives the words of that link.

| Notice | Group | When it appears | Link on the notice |
| --- | --- | --- | --- |
| A disk is almost full | Worth a look | At once, when its free space is under the share that you set. See [Choose when a disk counts as almost full](#choose-when-a-disk-counts-as-almost-full). | **Open the disk** |
| A disk is mounted read-only | Worth a look | At once, while **Read-only storage** is off | **Open the disk** |
| Axis could not write its marker file on a disk | Worth a look | At once | **Open the disk** |
| Imports wait because **Read-only storage** is on | Waiting for you | At once | **Open Settings, Storage** |
| Finished downloads cannot be imported yet | Waiting for you | 10 minutes after the download finished. The notice gives no reason. The **Downloads** page shows the reason for each download. | **Open Downloads** |
| Files and folders are left in the download folders | Worth a look | After the hourly count finds them | **Open Leftovers in the download folders** |
| Axis does not ask an indexer, because its pages changed or it answers with a browser challenge | Worth a look, or Not working when no other indexer can be searched | At once. The notice stays until the indexer answers a search or a test properly. | **Open the indexer** |
| Movies or series folders in the library wait for you to say which title they are | Waiting for you | At once | **Open Identify** |
| Movies or series folders that Axis linked by itself no longer look like their titles | Worth a look | At once | **Open Identify** |
| Files could not be read, or took too long to read | Worth a look | At once | **Open the files** |
| Files were not found by the last complete scan, or are missing | Worth a look | At once | **Open the files** |
| Library folders were never completely scanned | Worth a look | When a scan of the folder ended and no scan of it ever ended complete. Not shown for a folder whose first scan is still waiting or running, or whose disk is offline. | **Open Library** |
| Axis has not reached a media server | Worth a look | After 30 minutes without a good check, and at least two failed checks | **Open Player compatibility** |
| Workers are paused after their encoders crashed | Waiting for you | At once | **Open Conversion workers** |
| Workers were refused because they run another version of Axis | Worth a look | At once | **Open Conversion workers** |
| Workers have not been heard from | Worth a look | After about 10 minutes of silence, and not before Axis has been running for 10 minutes. A worker that is silent for more than a day is no longer shown. | **Open Conversion workers** |
| A conversion run paused itself | Waiting for you | At once | **Open the conversion run** |
| A conversion run is paused by a person | Worth a look | At once | **Open the conversion run** |
| A conversion run is ready and was not started | Worth a look | 10 minutes after Axis finished the run's list | **Open the conversion run** |
| Jobs failed for good | Worth a look | At once, for every failed job that the **Jobs** page still lists. Background upkeep jobs are not counted. | **Open Jobs** |
| Downloads of YouTube videos failed | Worth a look | At once | **Open YouTube** |
| Movies or episodes have more than one file | Worth a look | After the hourly count finds them | **Open Duplicates** |
| Series have episodes that only a person can take | Waiting for you | When the series are counted | **Open TV, Wanted** |
| Requests wait for approval | Waiting for you | At once. The notice gives their number and how long the oldest has waited, and names no person and no title. It goes when none waits. A request that was approved automatically never appears. See [Let other people ask for titles](20-let-other-people-ask-for-titles.md). | **Open Requests** |
| Approved requests could not be added | Waiting for you | At once, until you try again on the **Requests** page | **Open Requests** |

### Releases that only you can take

Sometimes Axis finds a release for an episode and no search grabs it by
itself. This happens for a release that is numbered differently from the
series' data, for a release that would replace a file that you keep by
hand, and for some releases of anime series. Axis gives these series a
notice of their own: "2 series have episodes that only a person can take".

To take such a release:

1. Select **Open TV, Wanted** on the notice. The **Wanted episodes** page
   opens on the tab **Waiting for Grab this one**, which lists only these
   episodes.
2. Select the reason beside an episode. The episode's releases open.
3. Find the release, read the reason beside it, and select **Grab this
   one**.

A series that is counted in this notice for an episode is not counted for
the same episode in "series need attention". When you grab the release,
the notice goes away the next time Axis counts the series.

Axis counts a release here only when taking it would fill the episode that
Axis searched for. A release that holds a different episode is not counted,
even when it is also numbered differently from the series' data.

### Choose when a disk counts as almost full

1. Open **Settings**, and go to the **Storage** settings.
2. In **Tell me when a disk has less free space than (percent)**, enter a
   whole number from 0 to 100. The default is 5, which tells you about a
   disk that is more than 95 percent full.
3. Save the settings.

To never be told about a full disk, enter 0.

### Imports that wait for Read-only storage

While **Read-only storage** is on, Axis imports nothing, and every finished
download waits. Axis shows one notice for all of them. If you keep
**Read-only storage** on for a reason, dismiss the notice once. It stays
dismissed while more downloads arrive.

Axis waits before it tells you about a disk, a download client, an
indexer, a media server, a silent worker, or a finished download that
cannot be imported, because these usually come right by themselves within
minutes. A refused key never comes right by itself, so Axis tells you at
once.

Axis does not tell you about things that end by themselves, such as an
indexer that asked Axis to wait, a daily allowance that is used up, or a
download that is stalled. It also does not count a movie or a series whose
only problem is the consequence of another notice. For example, when no
indexer works, the indexer has a notice and the movies that could not be
searched for do not.

Axis counts each wait from when the thing stopped working. A download
client that stops answering and later answers with errors is one outage.
When you switch a download client or an indexer off and on again, its wait
begins again.

The movies and series are counted again after a search or an import ends,
at most once in 5 minutes, and every 10 minutes when nothing ended. The
other notices are read again within about 10 seconds of a change, and every
minute.

## Dismiss a notice

To take a notice out of your count without dealing with its cause, select
**Dismiss** beside it.

- The notice moves to **Dismissed** at the bottom of the page. The bell no
  longer counts it for you.
- A dismissal is yours alone. Other administrators still see the notice.
- A dismissal changes nothing else. The disk is still offline, and the
  imports are still blocked.
- The notice comes back when the same thing happens again, when there are
  more of them, or when it becomes more serious. For example, you dismiss
  "3 imports are blocked" and a fourth import is blocked: the notice is
  back. When one of the three is decided, the notice stays dismissed.
- A dismissed notice does not come back only because time passed.
- A dismissal lasts only while its notice lasts. When the cause is dealt
  with and the notice goes away, the dismissal ends for everyone. The next
  time the same kind of thing happens, the notice is shown again. For
  example, you dismiss "12 movies need attention", all twelve are mended,
  and later other movies need attention: the notice is back, whatever
  their number.
- A dismissal is kept while Axis cannot tell whether the notice still
  holds. For example, you dismiss a notice about a disk and restart Axis.
  The notice is still dismissed after Axis has checked the disk again. The
  same is true while Axis counts the series for the first time after a
  start, while a library folder is scanned, and while a disk is offline
  for a notice that depends on that disk.

To put a dismissed notice back in the list, open **Dismissed** and select
**Show again**.

## The card on the dashboard

The dashboard has a card that is also named **Needs attention**. The card
is the top of the same list as the page:

- It shows your five most serious notices. Each line starts with its
  group, for example **Not working**, and the sentence is a link to the
  page where you deal with it.
- When you have more than five notices, the card ends with a link such as
  **All 7 on the Needs attention page**.
- A notice that you dismissed is not on your card.
- The card follows the same times as the page. A disk that went offline two
  minutes ago is not on the card yet. The **Disk space** card beside it
  marks the disk as offline at once.

To dismiss a notice, open the page. The card has no **Dismiss** button.

The card is drawn when you load the dashboard. It does not change while the
dashboard is open; the bell does.

Only administrators see this card.

## What can go wrong

| What you see | What to do |
| --- | --- |
| There is no bell in the header | You are not an administrator. Ask one, or log in as one. |
| "Axis has not looked yet since it started." | Axis was started less than a minute ago. Wait, or reload the page. |
| "Axis could not read the imports at its last look." | Axis could not reach its database for that group of notices. The page shows what it last knew and says since when. It tries again every minute. If the message stays, check the database. See [Back up, restore, and repair the database](14-back-up-restore-and-repair-the-database.md). |
| A notice about a details provider's key is back after you dismissed it | Axis was restarted. Axis keeps that refusal in memory only, so after a restart it counts as new. Change the key to end it. |
| A movie or a series that you just fixed is still counted | The movies and series are counted at most once in 5 minutes. Wait, and the number follows. |
| A disk that is offline has no notice just after Axis starts | Axis judges a disk only after its own first check of it, which runs as Axis starts. The notice appears at the next look. |
| An indexer that fails has no notice | Axis contacts an indexer only when it searches. The notice appears after a second failed contact and 30 minutes. To check now, open the indexer's page and test it. |
| The number does not change until you reload | Your browser did not open the live connection. The count is right each time a page loads. |
| The dashboard has no **Needs attention** card | You are not an administrator. |
| The dashboard no longer says "downloads failed today" | Axis tries the next release by itself after a download fails. When no release is left, the movie or the series needs attention, and that has a notice. |
| A finished download that cannot be imported has no notice | The download finished less than 10 minutes ago, or a disk went offline before the notice appeared. While a disk is offline, Axis leaves this notice as it was, because the offline disk can be the cause. The notice follows when every disk is found again. |
| A notice still shows figures from before a disk went offline | While a disk is offline, Axis cannot tell whether the disk is almost full, or whether downloads can be imported. Axis keeps those notices as it last knew them until the disk is found again. |
| A series is counted in "series have episodes that only a person can take" after you grabbed the release and the grab failed | The notice follows the episode's last search. It goes away when Axis searches for the episode again. |
| A release that waits for **Grab this one** has no notice | The release was found by a search of a whole season or a whole series. Axis counts only releases found by a search of one episode. Open the series' manual search to take it. |
| The notice about duplicates, or about leftovers, is still there after you removed them | Axis counts these once an hour. The notice follows at the next count. |
| There is no notice about duplicates just after an update | The first hourly count after the update writes the number. |
| A worker that you switch off every night has a notice every evening | A worker that is not heard from for about 10 minutes has a notice. The notice goes away after a day of silence. Dismiss it, or revoke the worker while it is not in use. |
| The notice about failed jobs counts jobs that failed weeks ago | The notice counts every failed job that the **Jobs** page lists. Retry the jobs, or dismiss the notice. Axis removes a failed job after the keeping time for failed jobs. |

## Verified

- The bell, the page, every notice in the first table, the waits,
  dismissing, and who can see what are covered by the test suite, with a
  real database and made-up titles (`tests/test-web-attention.c`).
- Of the second table, these are covered by the test suite: a disk that is
  almost full, imports that wait for **Read-only storage**, finished
  downloads that cannot be imported, leftovers, an indexer that Axis does
  not ask, what waits in **Identify**, files that could not be read or
  were not found, library folders never scanned, a media server that is not
  reached, a paused, refused, or silent worker, a conversion run that is
  paused by a person or is ready, failed jobs, failed YouTube downloads,
  duplicates, and series with episodes that only a person can take.
- A dismissal that is kept across a restart, during the first count of the
  series, and while a disk is offline is covered by the test suite.
- Not covered by a test: a disk mounted read-only, a marker file that
  cannot be written, and a conversion run that paused itself.
- The card on the dashboard is covered by the test suite. It was not looked
  at in a browser.
- The header was measured in headless Chrome at 360, 390, 768, 1024, 1233
  and 1280 pixels, with the bell at nothing, 3 and **99+**. Nothing
  overlaps, the search box keeps its place, and no page scrolls sideways.
- Not tried on a real server, on a real phone, or in Safari. The notices
  have only been seen on a demo instance filled with made-up titles.
