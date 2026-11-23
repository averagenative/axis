# Migrate from Radarr and Sonarr

This page shows you how to migrate the movies you manage in Radarr to Axis:
which movies you want, their quality profile, their folder, and the files
you already have. Allow about 30 minutes, plus the time Axis needs to read a
large library.

Axis migrates movies from Radarr and series from Sonarr. The steps are
the same for both. This page describes them for Radarr first. For what
differs for series, see [Migrate series from Sonarr](#migrate-series-from-sonarr).

What you can rely on:

- Axis only reads from Radarr. It changes nothing there.
- Axis moves, renames and deletes no file. It links the files that are
  already on your disks, where they are.
- Axis searches for nothing until you start it. You can look at Axis beside
  a working Radarr for as long as you like.

## Before you begin

You need:

- An administrator account in Axis.
- Radarr's address as Axis reaches it, and Radarr's API key. In Radarr, open
  **Settings**, then **General**, then **Security**.
- A TMDb token, so that Axis can match each movie to a title
  ([Connect indexers and download clients](03-connect-indexers-and-download-clients.md)).
- Your movie folder added to Axis as a **Movies** library folder, and scanned
  at least once ([Add disks and library folders](02-add-disks-and-library-folders.md)).
  Axis links a file only when its own index holds that file, so scan first.
- A quality profile in Axis for each Radarr profile you use. Axis does not
  create a profile from a Radarr profile. You choose one for each.

## Steps

1. Open **System**, then **Migrate**. Under **Add a connection**, select
   **Radarr** as the app, then enter a name, Radarr's address (for example
   `http://radarr:7878`) and the API key. Select **Add the connection**.

   A connection keeps the app you select. To connect Sonarr, add a second
   connection.

   Axis stores the key and never shows it again. It sends the key only to
   that address.
2. Select **Test the connection**. The page says what came back, for
   example "Radarr 5.2.6 answered."
3. Select **Read what Radarr has**. The reading is a job, and the page shows
   where it is. A reading adds no movie and links no file. Axis keeps it for
   a day.
4. Select **Open the preview**. The preview shows what a run would do, and
   changes nothing.

   The preview arrives filled in. The first line says how many movies would
   be migrated as monitored and how many as unmonitored. Axis sets each row
   of the two tables from what it already knows and marks the row
   **Suggested**. If you change nothing, you can confirm the run as it
   stands. Change any row that is wrong.

   | Part of the preview | What you do |
   | --- | --- |
   | **Quality profiles** | Check the Axis profile that each Radarr profile becomes. Axis suggests the profile with the same name. If there is none, Axis suggests the default profile for new movies of the person who read Radarr, or else the default profile of Axis. |
   | **Folders** | Check the Axis library folder for each Radarr folder. Axis suggests the library folder, and the path below it, where its index holds that folder's files at the same paths with the same sizes. |
   | **Found there** | Check the count, for example "934 of 1,012 files found here." If Axis suggests nothing, this column says why: Radarr has no files in the folder, too few of the files are in one library folder, or the files are in more than one place. Choose the folder yourself. |
   | **Options** | To migrate movies that Radarr does not monitor, select that option. They stay unmonitored. |
   | **Skipped** | Read why each movie would not be migrated. |
   | **Files that would not be linked** | Read why each file would not be linked. The movie is still migrated. |

   Select **Save the choices and count again** after each change. A choice
   that you save stays, and Axis never replaces it with a suggestion. This
   includes **(not mapped)** and **(none: skip these movies)**.
5. At the end of the preview, under **Migrate**, read what the run does.
6. Decide whether Axis starts searching when the run ends. The option
   **Start searching for these as soon as they are migrated** is cleared
   by default.

   | Option | What happens when the run ends |
   | --- | --- |
   | Cleared | Every movie that was migrated is held. Axis searches for nothing until you [start searching](#start-searching). |
   | Selected | Axis does what **Start searching** does. It reads what Radarr is downloading and leaves those movies held. If Radarr does not answer, Axis starts nothing and the connection's page says so. |

   Select the option only if Radarr no longer monitors these movies, or is
   about to be stopped. If both monitor a movie, both download it.
7. Select **Migrate N movies**.

   If you changed a choice in another window, or the library changed, Axis
   refuses the run and shows the preview again. Read it, then confirm again.
8. The connection's page shows the run's progress. When it ends, the
   **Migrated** card says how many movies were migrated and how many files
   were linked. If you selected the option, the card also says whether
   searching was started and how many movies were left out. Select **See
   what was migrated** for the list.

Unless you selected the option, every movie that was migrated is held: Axis
does not search for it. What that means depends on whether the movie has its
file:

- **A movie that has its file needs nothing from you.** Axis keeps the file
  and does not replace it. The movie's page says "Migrated from Radarr; Axis
  does not replace it." You do not need to start searching for it.
- **A monitored movie without a file waits.** Its page says "Migrated from
  Radarr: Axis does not search for it until you start searching." The movie
  list marks it **Held**.

The **Migrated** card on the connection's page states both numbers: how many
movies have their file, and how many are monitored and have no file. The
**Needs attention** page shows a notice for the second group only, for
example "34 movies migrated from Radarr have no file. Axis does not search
for them until you start searching." If every migrated movie has its file,
there is no notice.

A movie that was already in Axis before the run is left exactly as it is.
The run does not migrate it, so the run does not keep its files: Axis's
usual rules continue to apply to them. The preview says how many movies this
concerns. To keep a file of such a movie, open the movie, find the file
under **Files**, and select **Keep it by hand**.

Axis does not migrate Radarr's blocklist or its history. To block a
release in Axis, block it on the movie's page when you need to.

## Start searching

Starting searching affects only the movies without a file. Do this when you
are ready for Axis to download them. Until then, Radarr can keep working as
before. If every migrated movie has its file, you do not need to do this:
the files are kept and are not replaced, whether you start or not.

1. In Radarr, unmonitor the movies you migrated, or stop Radarr. Axis
   cannot do this for you. If both monitor a movie, both download it.
2. On the connection's page, select **Start searching for the movies
   without a file**.
3. Read the page. It says how many movies are monitored and have no file,
   how many have a file and stay as they are, at what pace Axis searches,
   which movies Radarr is downloading now (Axis leaves those out), and how
   many of the movies Radarr still monitors.
4. Select **Start searching for N movies without a file**. If no movie
   would be searched for, the button reads **End the hold of N movies**
   instead.

Nothing is searched for at once. The scheduler takes the movies at its usual
pace, so your download client fills as it would on any day. Axis searches
for the monitored movies that have no file. It does not replace a migrated
file: see [What happens to your files later](#what-happens-to-your-files-later).

Starting also ends the hold of the movies that have a file. Nothing changes
for them now. If such a file is lost later, or you let Axis upgrade it,
Axis searches for the movie then.

To start one movie only, open its page and select **Let Axis search for it**.
**Search for this movie** and **Manual search** on a held movie also end its
hold.

## A file that Axis does not see

Radarr can have a file for a movie that Axis did not link. The list of what
was migrated says why:

| Reason | What it means | What you do |
| --- | --- | --- |
| **Not in Axis's index at that path** | Axis has not scanned the file, or the folder choice is wrong. | Scan the library folder, or correct the folder choice. Then read and run again. |
| **Another size than Radarr recorded** | The file changed, or the choice points at another copy. | Check the folder choice. |
| **Linked in Axis to another movie** | Axis already links that file to another movie. | Open **Files and links** on either movie and put it right. |
| **Unlinked by hand in Axis** | You unlinked that file. Axis leaves it so. | Nothing, or link it again on **Files and links**. |

Such a movie is held apart. It stays held when you start the others, because
searching would download a second copy. It is released when a later run
finds the file, or when you select **Let Axis search for it** on its page.

## Run again later

You can read and run as often as you like. A later run:

- migrates the movies that are new in Radarr;
- links a file that Axis could not see before and holds now;
- changes nothing else. A movie that is already in Axis keeps its monitoring,
  its profile and its folder as you set them in Axis, and its links as you
  left them.

A movie that you removed in Axis after it was migrated does not come
back. To have it again, add it yourself.

## What happens to your files later

Axis keeps every file that was migrated. Axis does not upgrade or
replace such a file by itself, whatever the quality profile says, and whether
or not you started searching. The movie's page says "Migrated from
Radarr; Axis does not replace it."

To let Axis upgrade the files again:

- **Every file of a connection.** On the connection's page, on the
  **Migrated** card, select **Let Axis upgrade these files**. The page
  that opens says how many files it lets go, and changes nothing until you
  confirm. A file that you kept or linked by hand yourself is not counted
  and not changed.
- **One file.** Open the movie, find the file under **Files**, and select
  **Let Axis upgrade it again**. To keep it again later, select **Keep it by
  hand**.

After that, Axis replaces a file that is below the cutoff of its quality
profile, at the scheduler's usual pace. A movie that is still held is not
searched for until you start searching.

To replace one kept file yourself, use **Manual search** on the movie. The
page asks you to confirm that the file is replaced.

When Axis upgrades a movie, it puts the new file in a folder that Axis names,
inside the movie's library folder. It then moves the old file, with its
subtitles, to the recycle folder of the same disk. Axis keeps it there for
the time set in **Settings**, **Imports**. The movie never has two copies.

The folder that Radarr named stays where it is, with anything else it holds,
such as an `.nfo` file or artwork. Axis does not remove a folder. Remove it
yourself when you no longer need it.

## Undo

There is no undo for a whole run. To undo one movie, open it, and under
**Settings for this movie** remove it. Removing a movie touches no file.

To forget the connection and its API key, open the connection's page and
select **Forget this connection**. The movies that were migrated stay, and their
files stay kept. After you forget the connection, **Let Axis upgrade these
files** is no longer available, and you can let a file go only on its own
movie's or series' page. To let them all go, do that first. Movies
that are still held can then be started one at a time only, so start
searching first. Axis also forgets which movies you removed after they were
migrated, so a later connection to the same Radarr can migrate them again.

## Migrate series from Sonarr

You can connect Sonarr, read what it has, see what Axis would migrate, and
then migrate it: your series, the monitoring of each season and episode,
their quality profile, their folder, and the episode files you already have.
Axis only reads from Sonarr. It changes nothing there.

Sonarr numbers its series as TheTVDB does, and a series that comes from
Sonarr follows TheTVDB's aired order in Axis from the moment it is added.
For that reason Axis reads Sonarr, and migrates series, only when it has
a TheTVDB key.

You need:

- Sonarr's address as Axis reaches it (for example `http://sonarr:8989`), and
  Sonarr's API key. In Sonarr, open **Settings**, then **General**, then
  **Security**.
- A TMDb token and a TheTVDB key. To enter the TheTVDB key, open **System**,
  then **Movie and TV details**.
- Your series folder added to Axis as a **TV** library folder, and scanned at
  least once.

To read, preview and migrate:

1. Open **System**, then **Migrate**. Under **Add a connection**, select
   **Sonarr** as the app, then enter a name, the address and the API key.
   Select **Add the connection**.
2. Select **Test the connection**.
3. Select **Read what Sonarr has**. Axis asks Sonarr for its series, and then
   for the episodes and the episode files of each series, at most four
   requests a second. A library of 500 series takes about four minutes.
4. Select **Open the preview**. It arrives filled in, as it does for Radarr.
   The first line says how many series would be migrated as monitored and
   how many as unmonitored, and how many seasons and episodes would be
   migrated monitored. Check the quality profile that Axis suggests for each
   Sonarr profile and the library folder that it suggests for each Sonarr
   folder. If you change a row, select **Save the choices and count again**.
   The option **Also migrate unmonitored series** starts selected, so that a
   series that Sonarr does not monitor still comes to Axis, unmonitored, with
   its files linked and kept. Clear it to leave those series out. Axis keeps
   the choice that you save.
5. Under **Migrate**, read what the run does, then select the button
   that starts it. The connection's page shows where the run is. If the run
   stops, open the preview and confirm again: nothing is migrated twice.
6. Select **See what was migrated** to check each series.

The preview shows:

| Part of the preview | What it tells you |
| --- | --- |
| **Series that would be migrated** | For each series: whether Sonarr monitors it, which seasons it monitors, how many episodes it monitors and has a file for, the profile and library folder it would get, and how many of its files Axis holds at the same paths. |
| **Series that would be skipped** | Why each series would not be migrated. |
| **Files Axis cannot use** | Why each episode file would not be linked. |

Reasons that only a series has:

| Reason | What it means |
| --- | --- |
| **No TheTVDB key in Axis** | Axis has no TheTVDB key now. Enter one, then read Sonarr again. |
| **TheTVDB's aired order does not list it** | TheTVDB lists no episodes for the series, so Axis cannot number it as Sonarr does. |
| **Sonarr gives a kind of series Axis does not know** | Sonarr's type for the series is not standard, daily or anime. Axis does not migrate it. |

A series that Sonarr marks as anime is not skipped for that. See
[Anime series](#anime-series).

### What the run does with a series

- **The series.** Axis adds each series with the quality profile and the
  library folder that you chose, numbered as TheTVDB's aired order numbers
  it.
- **Monitoring.** Axis copies what Sonarr has: the series, each season, each
  episode, and whether seasons that come later are monitored. A series that
  Sonarr does not monitor is migrated, unless you clear the option **Also
  migrate unmonitored series**, with nothing monitored. Its files are linked
  and kept, so that Axis does not replace them. Without this option, a series
  that you have files for but that Sonarr does not monitor stays outside Axis.
- **Episodes that do not match.** To link a file, Axis matches an episode by
  its season and episode number in TheTVDB's aired order, and checks
  TheTVDB's identifier of the episode where Sonarr gives one. An episode that
  Sonarr has and Axis's numbering does not is not guessed, and Axis links no
  file to it. The list of what was migrated says how many such episodes
  each series has.
- **Monitoring of an episode whose identifier differs.** Monitoring goes by
  the season and episode number alone. If Axis has an episode at a place
  that Sonarr lists under another identifier, Axis monitors it as Sonarr
  monitors that place. If Sonarr has a file at that place, Axis holds the
  episode, so that Axis does not download a second copy. See
  [Start searching for series](#start-searching-for-series).
- **Episode files.** Axis links each file that its index holds at the same
  path and with the same size, to the episode or episodes that Sonarr has it
  on. A file with two episodes is linked to both. Axis moves, renames and
  deletes no file.
- **Files that Identify linked by name.** If Identify already linked a file
  to another episode of the same series than the one that Sonarr has it on,
  Axis moves the link to the episode that Sonarr has it on. The list of what
  was migrated counts these files. Identify does not move the link back.
  A later run does the same for a file that Identify linked after the first
  run. Axis does not move the link if it cannot check the file against
  Sonarr: an episode that Sonarr has the file on is not in Axis's numbering,
  or the file's size is not the size that Sonarr recorded. The list counts
  such a file as linked to another episode than Sonarr has it on. Check it
  on the series' **Links** page.
- **Files that are left alone.** A file that you or an import linked stays
  as it is, even on another episode than Sonarr's. A file that is linked to
  another series stays there. A file that you unlinked by hand stays
  unlinked. The list of what was migrated gives the count for each
  reason.

A file whose name states other numbers than the episode that Sonarr has it
on (for example, a file that Sonarr named by scene numbers) stays on the
episode that Sonarr has it on. Axis does not report it as misplaced, and
Identify does not move it. The series' **Links** page shows what the file's
name states. If the file is on the wrong episode, move the link there.

### Anime series

Axis migrates a series that Sonarr marks as anime exactly as it migrates any
other series. Axis matches the series by its TheTVDB identifier and numbers
it by season, as TheTVDB's aired order numbers it. Axis copies the
monitoring, links each file to the episode that Sonarr has it on, keeps
those files, and holds the series until you start searching. A file that is
named by an absolute number, for example
`[KumoSubs] Hoshi no Kagami - 14 [1080p].mkv`, stays on the episode that
Sonarr has it on.

Two things are different for an anime series:

- **The series is marked as anime in Axis.** Its **Episodes and downloads**
  page shows the **Anime numbering** card and an **Absolute** column. See
  [Anime series](07-add-a-series.md#anime-series).
- **Axis compares the absolute numbers.** Sonarr gives each episode an
  absolute number of its own. Axis compares that number with the number
  that TheTVDB gives the same episode, and reports the result. The preview
  shows the result in the series' row. After the run, the list of what was
  migrated, the series' history and the **Anime numbering** card show it.
  For example: "Absolute numbers: Sonarr and TheTVDB agree for 291 of 291
  episodes", or "differ for 3 of 291 episodes, the first S03E02 (Sonarr 29,
  TheTVDB 30)".

The comparison is a report. A difference changes nothing: Axis links,
monitors and holds every episode the same way whether the numbers agree or
not. Axis takes its absolute numbers from TheTVDB alone, and never from
Sonarr. If the numbers differ, check those episodes at TheTVDB.

After you start searching for a migrated anime series, Axis recognises a
release that is named by an absolute number, such as `Hoshi no Kagami -
137`, and also searches by that number, when TheTVDB's absolute numbers for
the series can be used. A release that is named by season and episode is
matched as for any series. See
[Releases and files named by absolute number](07-add-a-series.md#releases-and-files-named-by-absolute-number)
and
[How Axis searches for an anime series](07-add-a-series.md#how-axis-searches-for-an-anime-series).

If an earlier preview listed an anime series as skipped, read Sonarr again.
The new preview lists the series to be migrated.

### Start searching for series

Axis searches for nothing after the run, unless you selected **Start
searching for these as soon as they are migrated** when you confirmed
it. That option works as it does for movies: when the run ends, Axis reads
Sonarr's download queue, leaves each series that Sonarr is downloading for
held, and starts the others. If Sonarr does not answer, Axis starts nothing
and the connection's page says so. Without the option, every series that
was migrated is held, and its page says so.

Starting searching affects only the episodes without a file. The episode
files that were migrated are kept and are not replaced, whether you start or
not. The **Migrated** card on the connection's page says how many series
have a monitored episode without a file, and how many have none. The **Needs
attention** page shows a notice for the first group only, for example "3
series migrated from Sonarr have monitored episodes without a file. Axis
does not search for them until you start searching." An episode that has not
aired yet counts as an episode without a file, because Axis does not
download it while its series is held.

- To start all of them, open the connection's page and select **Start
  searching for the episodes without a file**. The page that opens reads
  Sonarr's download queue, leaves out each series that Sonarr is downloading
  for, and warns you while Sonarr still monitors the same series. Unmonitor
  them in Sonarr, or stop Sonarr, before you confirm.
- To start one series, open its page and select **Let Axis search for it**.
  A page opens that says what starts, and changes nothing until you confirm.
  **Search for every wanted episode** on the series' page also starts
  searching for the whole series.

After you start, the scheduler takes the series at its ordinary pace.

You can search a held series yourself without starting it:

- **Search now** for one episode or one season runs that search only.
- A manual search lists releases and grabs nothing by itself.

In both cases the series stays held, and Axis searches for nothing else of
it. If a download that came from your search fails, Axis does not search
for the next best release while the series is held. The series' history
says so. Search again yourself, or start searching for the series.

An episode that Sonarr has a file for, and that has no file in Axis, stays
held after you start: Axis would otherwise download a second copy. The
series' page says how many episodes are held this way. This hold ends in one
of these ways:

- The episode gets a file in Axis: a later run links it, Identify links it,
  or you link it by hand.
- You select **Search now** for that episode.
- On the page of **Let Axis search for it**, you select **Search for those
  too; a second copy may be downloaded**. The page says how many episodes
  the option covers. The option is cleared by default.

### Series whose numbering you change

Sonarr's seasons and episode numbers are TheTVDB's. If you change a series
to another numbering in Axis after you migrate it, a later run links no
more files for that series: the same season and episode number can be
another episode. The preview says how many files are affected. Link those
files by hand on the series' **Links** page.

### Upgrades of migrated episode files

Axis keeps every episode file that was migrated from Sonarr. Axis does not
upgrade or replace such a file by itself, before or after you start
searching. The episode's state says "Migrated from Sonarr; Axis does not
replace it." Axis also takes no season pack that holds an episode whose file
is kept. It searches for the season's missing episodes one by one instead.

To let Axis upgrade the files again:

- **Every file of a connection.** On the connection's page, select **Let
  Axis upgrade these files**, read the page that opens, and confirm. A file
  that you kept or linked by hand yourself is not changed.
- **One file.** On the series' page, on the episode's row, select the
  button that lets Axis upgrade that episode again, for example **Let Axis
  upgrade S01E04 again**.

Axis then upgrades a file by the rules of the series' quality profile:

- Axis puts the new file in a folder that it names itself, below the series'
  library folder.
- Axis moves the old file to the recycle folder of its disk, and deletes it
  after the retention time. The folder that Sonarr named stays, with anything
  else that is in it.
- A season pack replaces the files of its episodes one by one, each in this
  way.
- A file with two episodes is the file of both. If Axis upgrades one of the
  two episodes, the whole file goes to the recycle folder, and Axis then
  searches for the other episode again.

To stop Axis from upgrading one file again, open the series' page and, on
the episode's row, keep the file by hand.

## If something goes wrong

| What you see | What to do |
| --- | --- |
| "This address answers, but not as Sonarr" | Check the address and the app. A connection that you added as Sonarr must reach Sonarr, not Radarr. |
| "Nothing was read: Axis has no TheTVDB key" | Open **System**, then **Movie and TV details**, and enter a TheTVDB key. Then read Sonarr again. |
| "The key was refused" | Enter the API key again under **Settings for this connection**. |
| "This address answers, but not as Radarr" | Check the address. If Radarr has a URL base, add it, for example `http://media.example/radarr`. |
| "None of Radarr's files are at these paths" | The folder choice is not the same folder. Choose the library folder that holds these files. |
| "The preview changed after this page was made" | Read the preview again, then confirm. |
| "The last run did not finish" | Open the preview and confirm again. Nothing is migrated twice. |
| "Radarr did not answer" on the start page | Start Radarr, or select the box that says Radarr is stopped or no longer monitors these movies. |
