# Recognize the movies and series already on your disks

This page shows you how to let Axis match the movies and series you already
have to titles, how to review the matches it is not sure of, and how to have
Axis manage a title that is already on disk. Allow about 30 minutes, plus
the time identification takes on a large library.

Matching tells Axis what you have, so it does not search for it again and
weighs a better release against the file you own. Matching never renames,
moves or changes a file.

## Before you begin

You need:

- A TMDb token, so that Axis can ask which title a folder is
  ([Connect indexers and download clients](03-connect-indexers-and-download-clients.md)).
- A **Movies** or **TV** library folder, scanned at least once
  ([Add disks and library folders](02-add-disks-and-library-folders.md)).
- The **Identify** module switched on. If it is off, **System**, then
  **Modules**, lets an administrator switch it on.

## Steps

1. Wait for the first complete scan of the library folder. Axis starts identification
   by itself, within ten minutes of a scan. It looks at a limited number of
   folders at each run (the next run follows within a minute while work
   remains), so a large library takes hours. This protects the
   details provider from a flood of requests. The setting **Folders looked
   at per run** (**Identify** card in **Settings**) controls it.
2. Open **Library**, then **Identify**. The page is headed **Identify movies
   and series on disk** and lists **Library
   folders of movies and TV**. Each library folder shows its progress, for example "40 of 120
   movie folders and files looked at; 80 left, a few at each run." To start
   a run at once, select **Identify new and changed files now**.
3. Open a library folder. It lists what waits for you:

   | Section | What it holds | What you do |
   | --- | --- | --- |
   | **Likely: confirm or choose** | Axis has a best guess and is not sure. | Select **Confirm this movie** (or **Confirm this series**) on the right title, or **Choose this movie** or **Choose this series** on another candidate. |
   | **Unknown: search or mark** | Axis found no candidate. | Select **Search for the movie** (or **Search for the series**) and pick one. Or select **Mark as not a movie** (or **Mark as not a series**) so that Axis leaves the entry alone. |
   | **Linked, now uncertain** | Folders that stay linked, although Axis looked at them again and is no longer sure of the link. Each says what it is linked to and why it is uncertain now. | Select **Keep this link** if the link is right, **Choose this movie** or **Choose this series** on another candidate, or **Undo the link**. |
   | **Linked lately** | What is matched. | Select **Undo the link** if a match is wrong. It goes back to review. |
   | **Marked as not a movie** or **Marked as not a series** | What you marked. | Select **Undo "not a movie"** (or **Undo "not a series"**) to review it again. |
   | **Waiting for the details provider** | The details provider did not answer for a title the folder may be, so nothing is decided yet. It names that title and lists the candidates known so far. | Nothing. Axis looks at it again by itself. To choose now, open the folder. |

   Axis links a match by itself only when it is sure: an identifier in the
   name or an `.nfo` file agrees, or the title, year and running time agree
   within a tolerance. The settings **Runtime difference allowed for a sure match, in
   percent** and **Runtime difference allowed for a sure match, in minutes**
   (the larger of the two counts) decide
   what "agree" means. Axis guesses nothing.

   When several movies share a name, Axis reads each one's full record and
   takes the one whose year is the folder's year and whose running time
   fits the file. It counts the year of the first release, and the year the
   movie came out in cinemas in your region when that is within two years
   of the first. A release on disc or by streaming does not count. Axis
   links the folder by itself only when every other movie of the name a
   year off runs clearly longer or shorter than the file. If the year and
   the running time do not tell two movies apart, Axis holds the folder,
   and its reasons give each movie's year and running time so that you can
   choose at a glance. For example, a documentary of 2012 that runs 95
   minutes and a drama of 2013 that runs 100 minutes share a name: a folder
   named with 2012 whose file runs 100 minutes waits for you. The order of
   the provider's answers and a movie's popularity never decide.

   The year that counts is the year of the movie's first release, wherever
   that was. A folder named with that year matches even when the movie came
   out in your region years later, as many movies first released outside
   the United States did. If the folder's year is two years from the
   provider's, for example the year of a festival showing, Axis holds the
   folder under **Likely**, and its reasons name both years.

   A folder named "Name (Year)" is read as that title, whatever the name is
   made of: a number such as `412 (2011)`, one or two letters, or a word
   that looks like a tag or a quality word, such as `4K (2015)`. The same
   holds for a video named that way.

   A folder named by another name of a movie, such as its title in another
   country, is linked by itself only when Axis read every answer of that
   year for its other names. Otherwise it waits under **Likely**.

   A few more cases that Axis links by itself:

   - A folder named "Title (Year)" that holds one video, even when the
     video's own name gives no title, such as `movie.mkv`.
   - A series folder named "Title (Year)" whose episode files give only
     their numbers and episode titles.
   - A movie whose file runs longer or shorter than the provider says, by
     up to a quarter (up to 60 percent longer when the name says
     **Extended**, **Director's Cut**, **Unrated** or another longer cut),
     when it is the only movie of its name and the folder's name and the
     file's name agree on its title and year. A name that says only
     "Collection", "Version", "Cut" or "Edition" does not count as a longer
     cut.

   A series folder named without a year is the normal case. Axis links it
   by its episodes instead, when exactly one series of that name fits them:
   at least 3 numbered episodes in at least 3 files, none in a season the
   series does not have, at least 90 percent of them episodes the series
   has, and a running time that does not contradict. Axis compares every
   series of that name, including a series known by that name in another
   country. If two series of one name both fit, Axis never chooses between
   them. It holds the folder and shows both as candidates. It also holds the
   folder when the provider lists no seasons or episode counts for another
   series of that name, because the episodes cannot be checked against it.

   After an upgrade that changes these rules, Axis looks again by itself,
   a batch at a time, at every folder it held for review. When the upgrade
   changes how series without a year are compared, it also looks again at
   every series it linked by its episodes. It never changes a folder that
   you decided, and it never removes a link.

   Only you change a link. When Axis looks again at a linked folder and is
   no longer sure of the link, or finds another title, the link stays and
   the folder moves to **Linked, now uncertain**. The same holds when the
   provider does not answer: the link stays, and Axis looks again later.

   Every folder held for you shows a line that starts "To settle it:". It
   says what to choose, or what in the names would let Axis link the folder
   by itself, such as adding the year the series first aired in brackets
   after the title.

   Each candidate shows its poster, its year, and its running time. A
   series shows its number of seasons and the length of an episode
   instead. Compare the running time with the folder's own, which the card
   shows under the folder's name. A poster that Axis has not kept yet
   loads when its card comes into view.

   After you select **Confirm this movie**, **Choose this movie**, **Mark as not a movie**, **Keep
   this link**, or **Undo the link** on a card, the page shows the next card in the
   same list. A notice directly before that card says what you did, for
   example "Linked Paper Lantern (2004) to Paper Lantern (2004)." To take
   it back, select **Undo the link** in the notice. If the card was the last one in
   the list, the page shows the card before it. If the list has no more
   cards, the page shows the list's heading. This also works on a list's
   own page, such as **All 40 unknown folders and files**, and on a folder's page that you
   opened from a list: you return to that list.

   <!-- screenshot: the Identify page for a TV library folder with likely and unknown entries -->

4. When a library folder is matched, look at the result under **Media**:

   - **Movies**, **All** lists every movie Axis identified on disk. A movie
     Axis does not manage shows **In library**, with its copies and any
     offline disk.
   - **TV**, **All** lists every series, marked **On disk, not managed by
     Axis**.

## Have Axis manage a title that is on disk

By default, Axis never searches for, downloads, upgrades, renames or moves
a title it only recognized. To change that for one title:

1. Open the movie or series from **Movies** or **TV**.
2. For a movie, read "On disk, not managed by Axis". Choose its
   **Quality profile** and **Destination**, and decide **Monitored for
   upgrades**. Select **Manage this movie**.
3. For a series, open its page. Choose its **Quality profile**,
   **Destination** and **What to monitor**: **Future episodes only**, **Every
   episode** or **Nothing yet**. Select **Manage this series**.

The episodes on disk then count as in the library. Only the episodes you
choose to monitor are wanted, and nothing is searched for right away.

To manage every identified title of a library folder at once, use the **Manage every
identified movie** (or **series**) card on the library folder's page. Choose a
**Quality profile** and **Monitoring**, and select **Manage N movies**.

To stop, select **Remove this movie** (or **Remove this series**). Axis
forgets its searches and blocklist. No file is touched, and the title stays
listed as on disk.

## What the screen shows

A series on disk has a **Seasons on disk** card. Each episode shows whether
it is on disk, with its file, quality and size. Files Axis found in the
series' folders that fit no episode are listed under **Files on disk on no
episode**, each with the reason. If a season's episode list is not known
yet, the page says "Identify fetches them on its next run, and their files
are placed then."

## What can go wrong

| What you see | What to do |
| --- | --- |
| "Waiting for the library folder's first complete scan." | Wait, or select **Scan now** on the library folder under **Library**, **Files**. |
| "No details provider serves movies and series yet" | Add the TMDb token in the Setup guide. |
| "The details provider did not answer: the next try is after ..." | Axis retries by itself. Check your network and the token. |
| A folder waits under **Waiting for the details provider** | The details provider did not answer for a title the folder may be. Axis tries again after 15 minutes, then waits twice as long each time, up to a day. To try at once, select **Identify new and changed files now**. |
| A run stops with "The details provider holds Axis off" | The details provider asked Axis to slow down, or failed several times in a row. Axis goes on by itself after the wait. |
| A folder is listed under **Linked, now uncertain** | Read its line that starts "Linked to". If the link is right, select **Keep this link**. Otherwise choose another candidate or select **Undo the link**. |
| A folder says it holds more videos than Axis reads | Axis reads at most 64 videos of a movie folder and 5,000 of a series folder. The rest are not identified. Split the folder, one folder for each title. |
| A series waits under **Likely** or **Unknown** | Read its "To settle it:" line. A folder with too few numbered episodes (fewer than 3), a season the series lacks, or a running time that contradicts is held. Confirm it, or use **Search for the series**. |
| A series with files "on no episode" | The file names do not read as episodes the provider lists. Read the reason beside each file. |
| A candidate shows its title in place of a poster | The provider has no poster for that title, or its image host did not answer in time. Open the page again later. |
| A movie waits under **Unknown** with "the name looks like part of a series" | Its name holds an episode or special number, such as `S05M01`, so it may be part of a series rather than a movie. The search box on its page holds the words after that number. Search for its own title and choose it, or select **Mark as not a movie**. |
| A movie waits under **Likely** because "the year is two off" | The folder's year differs from the provider's by two years, often the year of a festival showing. If the reasons name the right movie, select **Confirm**. |
| A file of a managed series is not linked again after a run, or stays on an episode its name does not say | A person linked or unlinked it by hand on the series' **Files and links** page. A run never changes that. To hand the file back, select **Let Axis manage this file again** there. The next page shows which episodes Axis then places the file on. |
| A file of a managed movie is not linked again after a run, or stays linked to a movie its folder does not say | A person linked or unlinked it by hand on the movie's **Files and links** page. A run never changes that. To hand the file back, select **Let Axis manage this file again** there. The next page says what follows. |
| **Identify** is missing from the menu | An administrator switches it on under **System**, **Modules**. |

## Verified

- Movie identification has run on a real library.
- Series identification has run on generated libraries only (up to 1,500
  series and 60,000 episode files). The first real run held 92 percent of
  series for review because their folders carry no year. The change
  `fix-series-match-without-year` (link by episodes, the "To settle it:"
  line, and runs that follow each other within a minute) is built and
  tested on generated data. It has not been seen on a real server, so
  expect some series to wait for you.
- The pages that list the whole library under **Movies** and **TV**, and the
  **Manage** actions, have run on generated libraries only.
- The rules for movies of one name, the year of the release in your region,
  the wider running time, the folders that wait for the provider, the
  uncertain links and the second page of a search (change
  `fix-identify-hunt-findings`) are tested against a stand-in for the
  provider with made-up titles. They have not run against TMDb or a real
  library yet. A movie fetched before the upgrade counts its year in your
  region only after Axis fetches its full record again.
- The posters and running times of candidates, and the return to the next
  card after an action (change `fix-identify-review-flow`), are tested
  against a stand-in for the provider and its image host with made-up
  titles. They have not been seen in a browser yet.
- The year of a movie's first release, the folders two years off, names
  that are numbers or tags, and names that look like part of a series
  (change `fix-identify-regional-years`) are tested against stand-ins for
  the provider with made-up titles, not against TMDb or a real library.
  After the upgrade, Axis asks TMDb again for every movie name it
  searches, because the stored answers gave the year of the release in
  the United States.
