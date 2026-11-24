# Add a series and follow its episodes

This page shows you how to add a TV series, choose which episodes Axis
looks for, see how season packs work, and read the calendar. The example
series is "Harbour Lights". Allow about 15 minutes.

For a series that is already on your disks, see
[Recognize the movies and series already on your disks](06-identify-movies-and-series-on-disk.md).

## Before you begin

You need what the movie page needs: a TMDb token, a working indexer and
download client with a path mapping, and **Read-only storage** switched
off. You also need a **TV** library folder on an online disk
([Find and download a movie](04-find-and-download-a-movie.md)).

## Steps

1. Type `Harbour Lights` in the search box at the top of any page. Pick
   **TV** in **Search in**, or open the **TV** tab of the results.
2. Open the right result. Its title page lists **Seasons and air dates**.
3. In the add form, read: "Not among your series. Add it with what to
   monitor, and Axis looks for each episode once it has aired and until it
   is found." Choose:

   - **Quality profile** and **Destination** (a **TV** root).
   - **What to monitor**: **Every episode**, **Future episodes only**, **The
     latest season**, **The first season** or **Nothing yet**. The setting
     **What to monitor in a new series** (**TV** card in **Settings**) sets
     the default.
   - **Specials too**: tick it to include season 0.
   - **Search now**: search as soon as you add the series.
   - **Anime**: tick it for an anime series. Axis never ticks it for you.
     See [Anime series](#anime-series).

4. Select **Add to TV**. Open **Media**, then **TV**, then **All**, and
   open the series.

   <!-- screenshot: the Episodes and downloads page of a series, with its seasons and episode states -->

To add several series from the search results or a **Discover** page,
choose in the bar above the list where they go: **New series go to** a
**TV** folder, **with** a quality profile, **monitoring** which episodes.
Then select **Add to TV** on each card. The choice is kept for your account,
and a series' own **Add with options** still choose for that series alone. The page
comes back to the card you used, with a notice that names the series.

To find more series from one you like, open the series. Both of its pages,
**Series overview** and **Episodes and downloads**, have a **More like this**
card: up to 12 series that TMDb recommends for it. A series you already have
shows its state. On the others, **Add to TV** adds the series in one step and
comes back to the card. The first time you open the card, Axis asks TMDb and
the series appear a moment later; Axis keeps them for a week. If TMDb
recommends nothing, the card says so. A series that Axis has no TMDb ID for
has no card.

## What the screen shows

A series has two pages. **Series overview** shows TMDb's details and the
**Seasons and air dates** card. **Episodes and downloads** is the page Axis
manages. Each page links to the other. A series
Axis does not manage has **Episodes on disk** instead.

The top of the **Episodes and downloads** page says "Quality profile:" and the profile's name,
with a link, **Change the quality profile**, to the **Settings for this
series** card. That card holds **Quality profile** and **Destination**, which
you change there and keep with **Save quality profile and destination**, and shows **What to monitor**, **New
seasons** ("Monitored when announced" or "Not monitored until chosen"),
**Details last updated** (when Axis last read the provider) and **Next
episode**. Its controls are in one row: **Search for every wanted episode**
searches for everything wanted; **Update details now** asks the provider for new
episodes right away (Axis does this on a schedule anyway: each series about
every six hours, however many series you have); **Remove this
series**, last and apart from the others, forgets the series and touches no
file.

Under the series' name, a mark and a sentence say how the series stands.
The mark is the worst state of its episodes. A series reads **Not
monitored** only when nothing of it is monitored: no season, no episode, and
new seasons are not monitored either. Specials that you do not monitor, or a
season that you left out, do not make the series read **Not monitored**.
When every monitored episode that has aired is in the library, the series
reads **In library**, and the sentence says whether new seasons and episodes
are monitored as they are announced.

The **Monitoring** card, under the series' name, says what Axis monitors in
words, for example "12 of 13 seasons monitored; the specials are not
monitored; new seasons are monitored when they are announced." To change
the monitoring of the whole series without opening each season:

1. Under **Seasons**, select what to do with the seasons:
   - **Leave the seasons as they are** changes no season.
   - **Monitor every season** monitors every season and its episodes. The
     specials stay as they are unless you select **With every season,
     monitor the specials too**.
   - **Monitor the latest season only** monitors the latest season and stops
     monitoring every other season and the specials.
   - **Stop monitoring every season** stops monitoring every season and the
     specials.
1. Select or clear **Monitor new seasons and episodes as they are
   announced**. When it is selected, a season that the provider announces
   later is monitored from the day it is listed, so Axis searches for its
   episodes as they air. When it is cleared, a new season waits until you
   monitor it. A new episode of a season that Axis already knows is
   monitored as its season is, either way.
1. Select **Save monitoring of the whole series**.

The change affects only what Axis searches for. Axis does not delete, move
or unlink a file: an episode that has a file keeps it, whether you monitor
it or not. Axis searches for nothing at once. The next automatic search
takes the episodes that are wanted now. To search right away, select
**Search for every wanted episode**. The series' history records the change
with your user name.

A series that you migrated from Sonarr shows **As in Sonarr** under **What
to monitor** until you select something other than **Leave the seasons as
they are**. After that, it shows what you selected.

When you change the quality profile, Axis searches again at once for every
wanted episode the series monitors, with the new profile: a whole season
where enough of it is wanted, else each episode. The notice after **Save**
says so, with the new profile's name. A change of destination alone
searches for nothing.

Each season, in the **Episodes by season** card, lists its episodes with
**Episode**, **Title**, **Air date**, **State** and **Quality**. The states
are **Not aired**, **Wanted**, **Searching**, **Downloading**, **To import**,
**In library**, **Upgrade wanted**, **Needs attention** and **Not
monitored**. For one episode, select **Search for** the episode, **Monitor**
it or **Stop monitoring** it, each named with the episode, for example
**Search for S01E02**. Select **Search for this season** to search for the
whole season, and **Releases found for season 3** (or the season's number) to
read what Axis found. After a search that found nothing, an episode's
button reads **Search again now**, and its line says when Axis searches
again by itself.

### Anime series

Anime releases are often numbered 1, 2, 3 and on across seasons (an absolute
number), while Axis and media servers number episodes by season, for example
S06E12. To tell Axis that a series is anime, tick **Anime: its releases are
often numbered 1, 2, 3 and on across seasons**. The box is on the add form,
on the **Manage this series** form of a series on disk, and on the
**Settings for this series** card, where you keep it with **Save the kind of
series**. Only an administrator can change it.

Axis never marks a series as anime by itself. When the series' details say
that it is animation whose original language is Japanese, the add form says
"The details say this is animation from Japan." beside the box. The box
stays clear until you tick it. One-step **Add to TV** on a card adds a
standard series. To add an anime series, use **Add with options**. A series
you added with one step has no box on the card, but the **Settings for this
series** card on its page has it.

What the mark does today:

- **Numbering of a series that you add.** If Axis has a TheTVDB key and
  TheTVDB lists the series, the series follows TheTVDB's aired order from the
  start. That order carries absolute numbers. Otherwise the series follows
  the numbering of any new series, which has none. A series that you manage
  where its files are keeps the numbering that its files were read under.
- **The Anime numbering card.** The **Episodes and downloads** page shows
  this card for an anime series. The card says which state the series'
  absolute numbers are in.
- **The Absolute column.** Each episode's row shows the absolute number that
  TheTVDB gives the episode. The cell is empty when TheTVDB gives none.
- **Names numbered absolutely are matched.** See
  [Releases and files named by absolute number](#releases-and-files-named-by-absolute-number).
- **Searches ask in more ways.** See
  [How Axis searches for an anime series](#how-axis-searches-for-an-anime-series).

Marking or unmarking a series changes no file, no link and no numbering.
The series' history records it.

| State on the card | What it means |
| --- | --- |
| "Absolute numbers come from TheTVDB's aired order, and this series is numbered as TMDb numbers it, which has none." | The series does not follow TheTVDB's aired order. With a TheTVDB key, select **Change the episode numbering**. Without one, set a TheTVDB key on the **Movie and TV details** page first. |
| "TheTVDB gives no absolute numbers for this series." | The series follows TheTVDB's aired order, and TheTVDB numbers none of its episodes. |
| "TheTVDB gives absolute numbers for N of M episodes." | Each number is on one episode, and the numbers rise with season and episode. The card also says whether the numbers equal a plain count of the episodes, or from which episode they differ. A difference is not an error: it is the place to look. |
| "TheTVDB gives two episodes of this series the same absolute number", or "are not in order" | Axis does not use the series' absolute numbers until TheTVDB's data is corrected. The card names the episodes. |

Axis takes absolute numbers from TheTVDB alone. Axis does not count
episodes to make a number, and does not take a number from a file's name, a
release or Sonarr. A special never counts. You cannot edit an absolute
number in Axis. Correct a wrong number at TheTVDB.

#### Releases and files named by absolute number

When a series is marked as anime and the card says that TheTVDB gives
absolute numbers for its episodes, Axis matches a name that states an
absolute number to the one aired episode that TheTVDB gives that number. Axis
looks the number up. It never counts episodes or works a season out from the
number. For a series that is not marked as anime, nothing changes: such a
name is ruled out, as before.

Axis reads these forms for an anime series:

| Name | What Axis reads |
| --- | --- |
| `[Group] Title - 137 [1080p]`, `Title.E137.1080p`, `Title - Episode 137` | Absolute number 137. |
| `[Group] Title 137 [1080p]`, `Title.137.1080p.WEB` | Absolute number 137, because the name begins with the series' title or one of its other names, exactly, and one number follows. Axis lists such a release and no search grabs it. See [A bare number after the title](#a-bare-number-after-the-title). |
| `Title - 12v2` | Absolute number 12. The `v2` is a revision, ranked as a PROPER is. |
| `Title - 137 [A1B2C3D4]` | Absolute number 137. The checksum in brackets is ignored. |
| `Title S06E12` | Season 6, episode 12, as for any series. |
| `Title - S06E12 - 137` | Season 6, episode 12, when 137 is that same episode. |
| `Title (01-24)`, `Title - 01-24`, `Title 001-024` | A whole season, when the range is exactly every episode of one or more seasons. |
| `Title - 01-12`, `Title (13-28)`, when the range is not whole seasons | A batch: exactly the episodes that have those numbers, in one season or in several. Only a search takes a batch. See [Batches](#batches). |

Axis does not read these, and each says why on the manual search page and
on the import's page:

| Name | Reason |
| --- | --- |
| `Title S2 - 05`, `Title S2 05`, `Title 2nd Season 05` | A season with a bare number. A group's season is often not TheTVDB's season. |
| `Title - 05 [2nd Season]`, `Title - 01-12 Second Season`, `Title - 01-12 [Season 1+2]`, `Title - 01-12 (Part 2)`, `Title - 05 Cour 2`, `Title - 01-12 [S2]` | A season or a part of the series stands after the number. The number can count from that part's first episode. |
| `Title - 01-13 OST`, `Title - 01-13 [Manga]`, `Title - 05 Trailer`, `Title - 05 Sample`, `Title - 05 Recap` | A word after the number says that the release is not an episode: a soundtrack, a book, a trailer, a sample or a recap. |
| `Title - 01-12v2`, `Title.E01-E12`, `Title.E01-12`, `Title - 01-12, 14` | A range in a form that Axis does not read whole. Axis does not take it for its first episode alone. |
| `Title 2nd Season - 05`, `Title II - 05`, `Zoku Title - 05`, when the series' other names include `Title 2nd Season`, `Title II` or `Zoku Title` | The number stands under the name of a part of the series, a season or a sequel. It counts that part's episodes, so it is not the series' absolute number. |
| `Title - 05 - 06`, `Title - 05 & 06`, `Title - 05 [06]`, `Title 10 [05]` | Two numbers follow the title. Axis cannot tell which one is the episode. |
| `Title - 3 - 05`, when the series' other names include `Title 3` | The name can be read two ways. |
| `Title 02 Complete`, `Title - 03 BD`, `Title 02 Batch`, `Title 02 Vol 3`, `Title - 02 Movie`, `Title - 05 of 12` | The word after the number says that the number is not one episode. |
| `Title 05 (2011)`, when the series is of 2023 | The year in the name is not the series' year. A year one off is accepted. |
| `Title 02`, when the lookup of the series' name also returns a title named `Title 2` | The number can be part of the other title's name. |
| `Title S01E137`, when season 1 has no episode 137 | No such episode. Axis does not read it as absolute number 137. |
| `Title - S06E12 - 138`, when 138 is another episode | The two numberings name different episodes. |
| `Title - 300`, when no episode has that number | No episode has the number. Axis does not pick the nearest one. |
| `Title - 140`, when that episode has not aired | The episode has not aired. |
| `Title 101`, when the series also has season 1, episode 1 | The number can be absolute number 101 or S01E01. `Title - 101` and `Title S01E01` are read. |
| `Title 2 - 05`, when the series is named `Title` | Two numbers follow the title. The release can belong to another series. |
| `Title 2`, `Title 2023`, `Title - 12.5`, `Title - 00` | A single digit with no hyphen, a year, half an episode, and zero are not episode numbers. |
| `Title - 01-12.mkv`, as the name of one file | A file that states a range of part of a season. Axis places a file on the episodes its name states, and does not place one file on a batch. Assign it by hand. |
| `Title - OVA 2`, `Title - NCOP 01`, `Title - Movie` | A special, an extra or a film named by a word. Assign it by hand. |

Where the match applies:

- **Manual search.** A release that maps to an episode is listed with what
  it holds and the number it came from, for example "it holds S06E12
  (absolute 137 is S06E12 by TheTVDB)". You can grab it as any release.
- **Scheduled searches and Search now.** Axis asks indexers by season and
  episode and also by the absolute number, and matches what comes back by
  the same rules. A search can grab a release that is named by absolute
  number, unless the rules below leave that release to you. See
  [How Axis searches for an anime series](#how-axis-searches-for-an-anime-series).
- **Imports.** Axis places a file that is named by absolute number on the
  episode that has the number, but only when the download was grabbed for
  that episode. When one file of a download is numbered absolutely and Axis
  cannot place it, Axis places none of the download's files that it read by
  an absolute number. Select **Assign by hand** on the import's page to say
  which episodes the files hold.
- **Place by its name.** If TheTVDB later moves a number to another episode,
  the page lists a file whose name states that number. Axis never moves the
  file's link by itself.

##### A bare number after the title

In `[Group] Title 137 [1080p]`, nothing marks 137 as an episode: no hyphen
and no `E` stands before it. The name of another title can look the same,
for example a sequel named `Title 2` that a group writes as `Title 02`, or a
film named `Title 13`. Axis reads such a name for an anime series, shows
what it holds, and leaves the decision to you:

- No search grabs the release, not a scheduled search and not **Search
  now**. The release's reason says "no search takes it".
- To take it, open the manual search and select **Grab this one**.
- A file with such a name inside a download is imported as before, when the
  download was grabbed for that episode.

`[Group] Title - 137 [1080p]` and `Title.E137.1080p` state the number
themselves. A search grabs those when the name is the series' title or
original title.

##### A low number under another name

A release often uses one of the series' other names, for example its
romanised name. A later season can have a name of its own among those other
names, and Axis cannot tell the two kinds of name apart. Under a name that
is not the series' title or original title, a low number can therefore be
the count within a later season.

- When the series has one season, a search grabs the release.
- When the number is higher than the number of episodes in every season
  after the first, a search grabs the release.
- Otherwise no search grabs it. The release's reason says so. To take it,
  open the manual search and select **Grab this one**.

For a series with three seasons of 12 episodes, a search does not grab
`Other Name - 05`, and does grab `Other Name - 17`.

Asking indexers by the romanised name does not change this rule. A search
finds `Other Name - 05` and lists it, and you grab it.

##### How Axis searches for an anime series

Anime releases are usually named by an absolute number and filed in an
anime category. Indexers have no field for an absolute number, so Axis asks
for it as words. For a series that is marked as anime, each search makes
these requests of every indexer:

| Search | What Axis asks |
| --- | --- |
| An episode that has an absolute number, when the series' numbers can be used | The request of any series: the identifier with season and episode, or the words `Title S06E12`. Then the words `Title 137`. Then the same words with one more name of the series, `Other Name 137`, when the series' details give a romanised name. |
| An episode, when the series' numbers cannot be used or the episode has none | The request of any series alone. |
| A season | The request of any series for the season. Then the series' title alone, which is how Axis finds a batch. |
| The whole series | The series' title alone, as for any series. |

- **The number is padded.** Axis writes the number with two digits, or with
  three digits when the series has an absolute number of 100 or more: `Title
  05`, or `Title 005`.
- **The other name.** Axis asks by one more name at most. That name is the
  series' original title when the original title is in Latin letters and is
  not the title again. Otherwise, it is the first other name, in
  alphabetical order, that the details mark as Japanese and that is in
  Latin letters. Axis does not ask by a name that holds the title with more
  words, such as `Title 2nd Season`. When the details give no such name,
  Axis asks by the title alone. A release under any other name of the
  series is still matched when an indexer returns it.
- **The category.** Every request for an anime series asks in the anime
  category (5070) as well as in the indexer's TV categories. A request for
  a series that is not marked as anime is exactly what it was.
- **The cost.** An episode search makes up to two more requests of each
  indexer than the search of a standard series makes. A season search
  makes one more. Each request counts against
  the indexer's own request limits, and Axis passes over an indexer that is
  paused or near its daily allowance, as for any search. Axis keeps each
  answer for a short time and reuses it, so searching several seasons asks
  for the series' title once.
- **An indexer that takes no words.** An indexer that is searched by
  identifier only is not asked for the words. The search's record says so.
- **The search's record.** The series' history and the releases page name
  the requests, for example: "Asked by season 6 episode 12, in TV and
  Anime: ... Asked by the words "Title 137", in TV and Anime: ...". When
  Axis does not ask by absolute number, the record says why.

Asking in more ways finds more releases. It does not change which releases
Axis takes: every rule in
[Releases and files named by absolute number](#releases-and-files-named-by-absolute-number)
applies to what comes back.

##### Batches

A batch is a release that holds a run of episodes by absolute number, for
example `[Group] Title - 01-12 [1080p]` or `[Group] Title - 13-28 [BD]`.

- **What a batch holds.** Axis looks up every number in the range. The
  batch holds exactly those episodes, in one season or across several. If
  one number is on no episode, or on an episode that has not aired, Axis
  rules the whole batch out and names the number. A range that is exactly
  every episode of one or more seasons is a season pack, as before.
- **When a search takes it.** A search takes a batch only when every
  monitored episode that it holds is wanted, none is being downloaded, and
  none has a file that is kept by hand. This is the rule for season packs.
  An episode search takes a batch that holds the episode. A season search
  takes a batch that holds a wanted episode of that season. A search of the
  whole series can take any batch.
- **What stays yours.** A batch that is named with the range right after
  the title and no hyphen, such as `Title 01-12`, is listed and no search
  grabs it. Neither does a search grab a batch under a name that is not the
  series' title or original title when its first number is low. See
  [A bare number after the title](#a-bare-number-after-the-title) and
  [A low number under another name](#a-low-number-under-another-name). To
  take one, open the manual search and select **Grab this one**.
- **The import.** Axis places each file of a batch by its own name, on the
  episode that the name states, in that episode's season folder. Axis
  places a file only on an episode that the batch was grabbed for. This
  holds for a file that is named by absolute number and for a file that is
  named by season and episode, and it holds for any release that Axis
  grabbed by an absolute number, not only for a batch.
- **Whole or not at all.** Axis places none of the download's files, and
  the import waits for you, in these cases:
  - One file's name states an episode that the download was not grabbed
    for, or a number that is on no episode.
  - The files' names do not account for every episode that the download
    was grabbed for, and one file, such as `05.mkv`, does not say which
    episode it is. That file can be the missing episode.

  Each file's reason is on the import's page. Select **Assign by hand** to
  say which episodes the files hold. The episodes stay with that download
  until you do, so Axis does not grab them again.
- **A batch that lacks a file.** If every file is placed and one episode
  that the batch was grabbed for got no file, the import ends as imported.
  The series' history names the episode. Axis searches for that episode
  again, and no search grabs the same release again for it. To take that
  release again all the same, open the manual search and select **Grab
  this one**.
- **Upgrades.** A file from a batch is an ordinary import. Axis can replace
  it with a better release later, as for any file.

Each time Axis uses an absolute number, the series' history names both
numbers and the day, for example "Read by its absolute number: 137 is
S06E12 by TheTVDB (TheTVDB's numbering as of 2026-11-21)."

To take a release that Axis rules out, see
[Grab a release that Axis cannot match by its name](#grab-a-release-that-axis-cannot-match-by-its-name).

For an anime series that you migrate from Sonarr, the card also shows how
Sonarr's absolute numbers compare with TheTVDB's. See
[Anime series](16-migrate-from-radarr-and-sonarr.md#anime-series).

### See what Axis is doing

The series, each season and each episode have a line that says what is
happening to them now:

- A search that is running or waiting for its turn, since when, and how
  many other searches are ahead of it. When only a search of the season
  or of the whole series is in hand, an episode's line says so. That search
  looks for whole-season packs only, so you can still search for the
  episode itself.
- An episode's download at its download client: how far it is, as a percentage and
  in gigabytes.
- For fifteen minutes after a search ends, what it found: for example "found
  6 releases and sent one to a download client", "found nothing", or "did
  not finish" and why.

The lines change by themselves while the page is open. When you reload the
page, they show the state at that moment.

A search button that would only repeat a search in hand cannot be
selected. It reads **Searching** or **Waiting for its turn** while that same
search is in hand. It reads **Searched a moment ago** for a minute after that
same search ended, and becomes a button again by itself when the minute is
over. If you select a search while it is in hand anyway, for example from
another tab, Axis searches for nothing more and the notice says where the
search stands. If the scheduler had queued that search behind others, your
click moves it ahead of them. This keeps clicking from using up an
indexer's allowance.

After you select a search, **Monitor**, **Stop monitoring** or a **Save** button, the page
comes back to the episode, season or card you used, and the notice is shown
there instead of at the top of the page.

The **Activity** tab lists everything Axis is doing now, in plain words,
and what ended in the last hour. The header shows how many things are
running and what kind they are, and links to it.

### Episodes with no air date

Axis searches for an episode by itself only once its air date has passed.
When TMDb gives an episode no air date, it waits, and its line says
so. To look for it anyway:

1. Select **Search anyway** on its row.
2. To search for all such episodes of a season at once, select **Search all
   N with no air date** above the season's episodes. Axis queues one search
   for each.

### Put right an episode that needs attention

When a series needs attention, its title page and its own page say which
episodes need it and why, the most common reason first. Each episode is a
link to where you put it right: its releases, its import, or its row on the
series page. On the **TV** list and on **Wanted**, the reason is a link to
the same place, and the dashboard names the most common reason after the
count.

On an episode's releases, the **What you can do** box under the last
search's result offers what fits the reason:

- Every release was rejected by the profile: select **Grab it anyway** to
  take the release, or choose a profile under **Use another quality profile
  for this series**. Each profile is marked with what it makes of the
  releases found: "accepts this release", or "rejects it" with the reason.
  Select **Use this profile and search again**. Axis saves the profile for
  the whole series, searches again, and brings you back to the releases
  with a notice that names the new profile. To change the limits of the
  profile itself, follow the link to its page.
- Nothing was found: the box says when Axis searches again by itself. Select
  **Search again now** to search now.
- Every acceptable release failed before: select **Clear the blocklist**,
  then **Search again now**.
- No indexer or no download client: follow the link to the **Indexers** or
  **Download clients** page.
- The destination is offline, or the profile cannot be used: choose another
  in **Settings for this series**.

If you are not an administrator, you see why, and that an administrator can
act on it, but no buttons.

### Search by hand and choose a release

A manual search asks every indexer now and lists everything it finds, for
the whole series, one season or one episode. It grabs nothing by itself:
you choose. It does not change any episode's state or when Axis searches by
itself. It runs even for episodes with no air date, episodes you do not
monitor, and episodes already in the library.

1. On the series page, select one of these:

   - **Manual search of the whole series**, in **Settings for this series**.
   - **Manual search of the season**, above a season's episodes.
   - **Manual search**, on an episode's row.

   An episode's or a season's releases page has a **Manual search** button
   too. Axis opens that level's **Manual search** page. While the search
   runs, the page says so and shows the results when they are in. If the
   page does not change by itself, select **Reload this page**.
2. Read the releases. They are in four groups:

   | Group | What it holds |
   | --- | --- |
   | **Acceptable under the profile** | Releases the profile accepts, best first. |
   | **Rejected by the profile** | Releases the profile rejects, each with its reasons. |
   | **Not what this search asked for** | Releases of this series that the automatic rules do not take here: single episodes on a season search, a whole season on an episode search, or episodes not wanted now. |
   | **Ruled out** | Another series, no episode named, a sample, or a release on the blocklist. No one can grab these. |

   The **Episodes** column says which episodes each release holds.
3. To change the order, select a link after **Sort**: **Size**, **Age**,
   **Quality**, **Indexer** or **Seeders**. To show the releases of one
   indexer or one kind, select its link after **Indexer** or **Kind**.
4. Select **Grab this one** on the release you want. A confirmation page
   says in one sentence what happens, for example "Grab ... (WEB-DL 1080p)
   by hand for S01E03 (no air date, not monitored), with the quality profile
   Balanced 1080p set aside." It names the episodes the release fills and any
   file in the library it replaces, and says whether each is an upgrade or a
   downgrade.
5. If the release is not better than a file in the library, tick
   **Replace ..., which this release is not better than**. Without the tick,
   Axis sends nothing.
6. Select **Grab this one by hand**. Axis sends the release and records that
   you chose it by hand.

A grab by hand claims exactly the episodes the release holds, with or
without an air date, monitored or not. Their monitoring does not change.

A release that also names an episode that TMDb's episode list does not have
numbers its episodes differently from that list. You can take it only with **Grab
this one**, never with the one-click **Grab it anyway**. Its confirmation
warns that files are placed by number and may land on the wrong episodes,
says what the import does with a file that holds a known and an unknown
number (placed and named for the known episode, the other number noted),
and needs a second tick.

The import of a release you grab by hand does not refuse its files for the
profile's rules or because their episodes were not wanted, but only for the
episodes you claimed. Any other file in the download is judged by the usual
rules. The import still checks that each file is an episode of this series,
and it replaces a better file only when you ticked the box for that file.
If the import can take no file at all, Axis frees the episodes and the
series' history says why. When the download of a grab by hand fails, Axis
does not search again by itself: use **Manual search again**, or wait for
the scheduled search.

An indexer gives a release only for a while after its search, about fifteen
minutes. After that, select **Manual search again** before you grab one.

Only an administrator can search by hand and grab. Other accounts see the
list and the reasons.

To keep Axis from taking a release, select **Block this release** on its
row. The release goes on the series' blocklist under your name, for every
episode, and the row reads "On the blocklist (blocked by ...)". Nothing is
downloaded or deleted. For a download you do not want, select **Block and
search again** on its row of the **Downloads** page: a page says what
happens, then Axis blocks the release, has the download removed, and looks
for the next best. For a file that is in the library already, select
**Block this release** on its row of the series' **Files and links** page.
Axis then only blocks the release for the future and leaves the file and its
link alone. The blocklist is at the bottom of an episode's or a season's
releases page. It says how each release got there, and **Unblock** takes one
off. A release you block is blocked by its name from every indexer. A
release that failed is blocked only from the indexer it came from.

Adding a series can queue dozens of searches at once. They run one after
another on a queue of their own, the searches you asked for first, so they
never hold up Axis noticing that a download finished and importing it.

### Replace a download in flight

Axis downloads one release for an episode at a time. When a release that
holds an episode is downloading and you choose another release that holds
the same episode, the confirmation of **Grab this one** or **Grab it by
force** has a card, **A download is in flight**. It names the release that
is downloading, the episodes it holds, its download client and how far it
is.

1. Tick **Replace the download in flight with this one**.
2. If the download in flight holds episodes that the release you chose does
   not hold, the page names them. Tick the box that says they stop
   downloading too.
3. Select **Grab this one by hand** (or **Grab it by force**).

Axis makes every check of the new grab first, then ends the download in
flight and asks the download client to remove it with its data, and sends
the release you chose only when nothing of the old download is left at the
client. The release you replaced is not put on the blocklist, and replacing
starts no search.

A download is always ended whole. Axis never removes some files of a
download and keeps others. For example, when a pack of season 1 is
downloading and you replace it with a release of S01E03, the other episodes
of the pack stop downloading and go back to ordinary searching. The next
scheduled search looks for them again.

When more than one download is in the way, for example a pack that you chose
while two of its episodes are downloading as two releases, the page names
each download with its episodes and offers no replace. Give up on each of
them on the **Downloads** page first, then grab the pack.

A download that has arrived is not replaced: the page says so and leads to
its import. What the page says when the client cannot be asked or cannot
remove the old download, when the new release cannot be sent, or when the
download is a torrent that someone moved into a library folder is the same
as for a movie:
see [Replace a download in flight](04-find-and-download-a-movie.md#replace-a-download-in-flight).

Only an administrator can replace a download.

### Grab a release that Axis cannot match by its name

Some releases are listed under **Ruled out** only because of their names: the
name reads as another series, more than one series fits, the name states no
episode, or it states a number that the series' episode list does not have.
Axis never guesses, so no search takes such a release. When you have checked
the release yourself, you can grab it by force and say what it is.

1. Open a manual search of the series, a season, or an episode, and find the
   release under **Ruled out**. A release you can force says "Ruled out by a
   fixed rule about its name" and has a **Grab it by force** button.
2. Select **Grab it by force**. The confirmation page states each rule you
   are setting aside, then asks what the release is.
3. Under **What it is grabbed for**, tick the episodes the release holds.
   Tick two episodes for a double episode. For a pack, choose the season in
   **Or a whole season** instead. Axis proposes what the name and the search
   suggest: the episode you searched for, the season you searched, or the
   episodes the name gives when the episode list has them.
4. Select **Use what I named**. The page then shows what the import does and
   lists each file in the library that the release would replace.
5. Tick each file that you want replaced. A forced release replaces nothing
   that you did not tick, even when it is better.
6. Tick **I have checked that this release is ...**.
7. Select **Grab it by force**. Axis claims exactly the episodes you named,
   or every episode of the season, and records the rules you set aside under
   your name.

If anything the page showed changes before you confirm, Axis sends nothing
and shows the page again. This happens when a file arrives, another grab
claims an episode, or the series changes its numbering. Axis also refuses a
forced grab while an import of the series is open: finish or dismiss that
import first.

What the import does depends on what you named:

- **One episode.** When the download holds one video file, Axis imports it
  as that episode and does not read its name.
- **Several episodes.** Axis reads each file's own name for the episode it
  states among the episodes you named. When you tick **One video file of the
  download holds all of these episodes** and the download holds one video
  file, Axis imports that file as all of them without reading its name. Without
  that tick, a file whose name does not state the episodes stays in the
  download.
- **A whole season.** Axis reads each file's own name for its episode. The
  series' name in the file's name is not judged. A name that states a season
  the series has is read as it says, so a file of another season stays in the
  download. Only a name whose season the series does not have, or that
  states none, is read by its episode number within the season you chose, and
  never when another file of the download states that season itself.

A file whose name gives none of the episodes you named stays in the
download. The import lists it with the reason "assign it by hand", and Axis
guesses nothing for it. When no file can be placed at all, the import is
held with that reason: dismiss it and search again.

Some releases cannot be forced by anyone: a sample, a password-protected
release, an archive, a program, one part of a multi-part post, and a release
on the blocklist.

### An episode file that is kept by hand

The file of a forced release is kept by hand. The episode's row says "Kept by
hand by ...: not upgraded by itself", and that the file came from a release
grabbed by force. Axis does not upgrade such a file: no scheduled search and
no **Search** button grabs a replacement, and a season pack that would
replace it is rejected with the reason that the file is kept by hand.

To let Axis upgrade the file again, select **Let Axis upgrade S01E03 again**
on the episode's row. To keep it again, select **Keep S01E03 by hand**.

You can still replace a kept file yourself. **Grab this one** and **Grab it
by force** list the kept file on their confirmation with a tick box that
names it. Without that tick, Axis sends nothing. The one-click **Grab** and
**Grab it anyway** never replace a kept file. The same applies to a movie's
kept file.

### Say by hand which episode a file is

Sometimes a file is linked to the wrong episode, or to none. For example,
after a change of numbering a file can sit on one episode while its name says
another. An administrator can put that right on the series' links page.
Only the link changes. Axis never moves, renames, or deletes the file on
disk.

1. Open the series' page and select **Files and links** in the **Seasons**
   card.
2. Find the file. The page lists every video file that is linked to the
   series, with how its link was made, and the video files in the series'
   folders that have no link.
3. Say what the file is:
   - To link a file or move its link, write its episode in the row's field,
     for example `S01E03`. For a file that holds two episodes, write both
     with a space between: `S01E03 S01E04`.
   - To swap two files, give each file the other's episode.
   - To remove a link, select **Unlink**.
   - To leave a file as it is, leave its row empty.
4. Select **Review the change**. The review shows, for every file and every
   episode that the change touches, what is linked before and after. It also
   says which episodes are left without a file and whether each of those is
   wanted and searched for again. An episode that you do not monitor is not
   wanted.
5. Read the notes. When a file's name says another episode than the one you
   give it, the review says so. A media server reads the file's name, so it
   may still show the file as the episode in its name.
6. Select **Change the links**.

When you give a file an episode that already has a file, both files stay
linked. The review says that the episode then has two files, and offers a
tick box to unlink the other file instead. After you tick, select **Show the
review with these ticks**. The review then lists every file that the ticks
touch, with what is linked before and after. A ticked file that is also on
other episodes keeps those, and is then kept by hand, so that nothing adds the
removed episode again. Select **Let Axis manage this file again** to undo
that. Afterwards, the links page lists each episode that has more than
one file under **Episodes with more than one file**.

The links page lists the files that have no link 100 at a time. Use **Earlier
files** and **Later files** to move between pages. It lists only files that
belong to the series: files linked to it, files in its folders, and files that
identification linked to it.

A link that you make this way is marked as made by hand and kept by hand.
Identification, **Place by its name**, a change of numbering, and imports do
not move, remove, or replace it. A change of numbering lists such links in a
group of their own on its confirmation page. A file that you unlink stays
unlinked: identification does not link it again. The links page lists it as
unlinked by hand.

To hand a file back, select **Let Axis manage this file again** under
**Linked, kept or unlinked by hand** on the links page. Axis then places the
file by its name, as identification does. A page shows what that does before
anything changes:

- For a file that you linked, the episodes that the name gives replace the
  episodes the file is on. The page lists the episodes the file is on
  afterwards, and each episode that is left without a file, and whether that
  episode is wanted and searched for again. Select **Let Axis manage this
  file** to confirm.
- If the name gives the file no episode of the series, the page says so and
  offers nothing to confirm. The link stays as it is, still marked by hand.
- For a file that an import linked, the link stays where it is and only your
  marks go.
- For a file that you unlinked, the page says which episodes the next run
  places it on, if any.

The review is refused, and nothing changes, in these cases:

- An import of the series is open, including one that ended partly imported.
  The page names the import. Retry it or dismiss it on its page.
- A download is in flight for an episode that the change touches. Wait for it
  to be imported, or dismiss it on the **Downloads** page.
- The series changed after the review was made. The page shows the review
  again as it is now. Confirm it again if it is right.

### Season packs

When a season has at least two wanted aired episodes, and at least a set
share of its aired monitored episodes are wanted, Axis looks for the whole
season first, and then episode by episode. The setting **Search for a
whole season when this share of it is wanted, in percent** sets the share
(50 percent by default).

On the release lists, the **Episodes** column says whether a release is an
episode or a season. A release that is numbered for an episode the
provider does not list is never grabbed or imported for a guessed episode.
The series page says how many releases it passed over for that reason.

### The calendar

Open **Media**, **TV**, **Calendar**. The page is titled **TV calendar**. It lists the episodes of the series
Axis manages, from a week ago to two weeks ahead, by the day they air (in
UTC), each with its state. An episode that is in the library is not
listed.

### Wanted and history

**Wanted** (under **TV**) lists the aired episodes Axis is looking for,
with tabs **All**, **Needs attention** and **Upgrade wanted**. Axis searches
automatically, the episodes of the last week first. **History** lists what
happened to every series.

## What can go wrong

| What you see | What to do |
| --- | --- |
| "There is no library folder for TV on a disk that is online, so there is nowhere for its episodes to go." | Add a **TV** library folder first. |
| "TMDb gives no identifier for N of its episodes" | Those episodes are known by season and number alone. This is a notice, not an error. |
| "N releases found by its searches are numbered for episodes that TMDb's episode list does not have" | The release names follow another numbering. Read the reasons on the releases. Use **Grab it anyway** only when you know the episode. |
| A season search found single episodes and a pack, and rejected them all | Select **Manual search of the season**, then **Grab this one** on the release you want. |
| **Grab this one** says "the search that found it has expired: search again" | Select **Manual search again**, then grab the release from the new list. |
| **Grab this one** shows **A download is in flight** | Another release that holds one of its episodes is downloading. To take the release you chose instead, see [Replace a download in flight](#replace-a-download-in-flight). |
| **Needs attention** on an episode | Read the line beside it, and follow its link to what you can do: a missing destination, an offline disk or a search that ended badly. |
| "No air date is known for it." | Select **Search anyway**, or wait until the provider gives a date. |
| Season pack imported but some episodes missing | Open the series and read each episode's state. Axis searches for what is still wanted. |
| "Nothing new was queued: a search of season 1 ended a moment ago" | That search ran less than a minute ago. Read what it found on the season's line, or ask again at the time the notice gives. |
| A season search found no pack, and its episodes are searched one by one | This is expected: when no whole-season pack is acceptable, Axis searches for each wanted episode of the season by itself. |

## Verified

- The TV module has run against test doubles only: adding a series, its
  episodes, season packs, the calendar and its imports have not been run
  against a real indexer, client or series.
- The release parser has been audited against 21,154 real release names
  (episode titles agreed with the reference parsers in 89.8 percent of
  names).
- The pages that list series on disk have run on generated libraries only.
- That searches run on a queue of their own is checked by the tests; a
  real series with dozens of searches has not been added since.
- What needs attention, the **What you can do** box, **Search anyway** and
  the search after a new quality profile are checked by the tests with a
  made-up series; they have not been seen in a browser yet.
- **Manual search** of a series, a season and an episode, its groups,
  sorting and filtering, **Grab this one** with its confirmation, and the
  import of a release grabbed by hand have run in the tests only, with a
  made-up series and test doubles for the indexer and the download client.
  They have not run against a real indexer or been seen in a browser yet.
- **Replace the download in flight with this one** has run in the tests
  only, with a made-up series and test doubles for the indexer and the
  download client. It has not run against a real download client or been
  seen in a browser yet.
- The lines of what is happening, the buttons that say their search is in
  hand, the answer to a search asked twice, the return to the row you used,
  and the **Activity** tab have run in the tests only, with a made-up series.
  That the lines change by themselves while the page is open is checked as
  far as the hub's event and the page's fragment; it has not been seen in a
  browser yet.
