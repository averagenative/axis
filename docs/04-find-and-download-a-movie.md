# Find and download a movie

This page shows you how to add a movie, watch Axis search for it, read why
it chose one release, and follow the download into your library. The
example is a film called "The Lantern Keepers". Allow about 15 minutes of
your time. The download itself takes as long as your client takes.

## Before you begin

You need:

- A TMDb token, an indexer that is **Working**, a download client that is
  **Working** with its path mapped, and a **Movies** library folder on an
  online disk
  ([Connect indexers and download clients](03-connect-indexers-and-download-clients.md),
  [Add disks and library folders](02-add-disks-and-library-folders.md)).
- **Read-only storage** switched off. Open **System**, then **Settings**, and
  find **Read-only storage** in the **Disks** card. While it is on, the
  download arrives at the client but Axis imports nothing, and the import's
  page says "Read-only storage is on (Settings, Storage; storage.read_only)".
  The Setup guide step **Letting Axis write** explains what changes when you
  switch it off, and what Axis never does. Do one disk at a time, and have a
  backup.
- A default quality profile. The setup step **What to download** shows the
  choice. Each profile ranks the qualities it accepts and the sizes it
  allows.

## Add the movie

1. In the box at the top of any page, leave **Search in** on its default,
   type `The Lantern Keepers` in the search box (its hint says "Search
   titles"), and press Enter. Results appear in tabs: **All**, **Movies**
   and **TV**.
2. Open the result with the right year. The title page shows its
   **Details**: original title, runtime, genres and the rest.

   <!-- screenshot: the title page of a made-up movie with the Add to Movies form -->

3. In the add form, read the lead line: "Not among your movies. Add it, and
   Axis looks for a release the quality profile accepts and sends it to a
   download client." Then choose:

   - **Quality profile**: which releases are acceptable.
   - **Destination**: the **Movies** library folder the file goes to.
   - **Search from**: when Axis may first search. A movie that is not out
     yet waits until its availability, as the setting **Minimum
     availability of a new movie** says.
   - **Monitored**: Axis keeps looking for it and for upgrades.
   - **Search now**: search as soon as you add it.

4. Select **Add to Movies**. Axis shows "The movie was added." and, with
   **Search now** ticked, "A search is queued: this page shows what it found
   once it has run."

If the form says "There is no library folder for movies on a disk that is
online, so there is nowhere for it to go," add a **Movies** root first.

### Add several movies from a list

The search results and the **Discover** pages have an **Add to Movies**
button on each card. To add several movies in a row:

1. Above the list, in the bar that reads **New movies go to**, choose the
   **Movies** folder and, after **with**, the quality profile. A folder on a
   disk that Axis cannot write to now is listed but cannot be chosen.
2. Select **Add to Movies** on each card you want. Each card's **Add with
   options** line shows the folder and profile its **Add to Movies** uses.

Your choice applies at once to every card on the page. Axis keeps it for
your account, so the next visit uses it too. To add one movie another way,
select **Add with options** on its card: its page opens with your choice selected,
and what you choose there is used for that movie only. Without a browser
that runs scripts, select **Save as my defaults** after you choose.

A folder on a disk that is offline or mounted read-only is listed with the
reason and cannot be chosen, in the bar or on a title page. If the folder you
chose becomes one of these, the bar says so and **Add to Movies** uses the
default folder until you choose again.

After **Add to Movies** or **Hide from Discover**, the page comes back to the same list,
with the same tab and filters, at the card you used. The notice there names
the movie, for example "Added The Lantern Keepers (2023) to Movies." The
title page you open from a card has a link back to the list at that card.
On a phone, the bar stays just under the search box while you scroll.

### Find more movies like one you have

To go from a movie you like to others like it, open the movie's page and
find the **More like this** card. It lists up to 12 movies that TMDb
recommends for that movie, as cards like those of **Discover**. A movie you
already have shows its state, and its card opens its page. On the others,
**Add to Movies** adds the movie in one step and comes back to the card, and
**Add with options** opens its page. The first time you open a movie's page,
the card says "Axis is asking TMDb what is like this movie." and the movies
appear a moment later. Axis keeps them for a week, so later visits ask TMDb
nothing. The card works even when **Recommended for you** is turned off in
Settings. If TMDb recommends nothing for the movie, the card says so.

## Read what Axis decided

On the movie's page, a state badge says where the movie stands. The states
are **Not available yet**, **Wanted**, **Searching**, **Downloading**, **To
import**, **In library**, **Upgrade wanted** and **Needs attention**. A
line under it says why, for example "Searching the indexers." or "Downloading
at Torrents."

The **Releases found by the last search** card keeps every release Axis
considered:

1. Read the line above the table: "Last search ..., on adding. 18 found; 3
   accepted". It names who asked for the search and how many releases
   passed.
2. In the table, each row is one release with its **Quality**, **Size**,
   **Found at** (the indexer, and the number of seeders or the age) and a
   **Verdict**:

   | Verdict | Meaning |
   | --- | --- |
   | **Grabbed** | Axis sent this release to the download client. |
   | **Accepted, rank 1** | The profile accepts it. The number is its rank. The lowest rank is the best. |
   | **Rejected** | The profile does not accept it. The reasons are listed under the release name. |
   | **Ruled out** | A safety rule excludes it, whatever the profile says. |
   | **Blocklisted** | A download of it failed earlier, or a person blocked it, so Axis does not try it again. |

3. For a release you disagree with, select **Grab** on an accepted one, or
   **Grab it anyway** on a rejected one. **Grab it anyway** sends it as a
   grab by hand. Axis records the choice.

Use these controls on the same page:

- **Search for this movie** searches again. After a search that found
  nothing, the button reads **Search again now**, and the line under the state
  says when Axis searches again by itself.
- **Give up on this download** puts the release on the blocklist, removes it
  from the download client, and looks for the next best. Select **Yes, give
  up on this download** to confirm. The client deletes the download's files,
  with one exception: when a torrent's client says that the torrent is in a
  library folder (someone moved it there in the client), Axis removes the
  torrent from the client and leaves its files. The **Downloads** page then
  says "Files left at the download client: ..." for that download. The same
  applies to **Block and search again**, to a download that fails, to
  **Remove** on the **Downloads** page, and to a torrent that Axis removes
  from its client after its import.
- The **Blocklist of this movie** card lists what Axis does not try again,
  and how each release got there. Select **Unblock** for one entry, or
  **Clear the blocklist**. To add a release yourself, see
  [Block a release you do not want](#block-a-release-you-do-not-want).
- The **Settings for this movie** card holds the **Quality profile**,
  **Destination**, **Search from** and **Monitored**. Select **Save** to keep
  a change. **Remove this movie** is last, apart from the other controls.
  The **Settings for this movie** link beside the profile name leads to the
  card.

When you change the quality profile, Axis searches for a monitored movie
again at once with the new profile, if the movie is still wanted under it.
The notice after **Save** says so, or says why nothing was searched for. A
change of destination alone searches for nothing.

### Put right a movie that needs attention

When a movie needs attention, the line under its state says why, and a link
beside it leads to what you can do. The **What you can do** box sits under
the state. It offers what fits the reason:

- Every release was rejected by the profile: select **Grab it anyway** to
  take the release (when there is only one), or choose a profile under **Use
  another quality profile for this movie**. Each profile is marked with what
  it makes of the releases found: "accepts this release", or "rejects it"
  with the reason. The first other profile that accepts is chosen. Select
  **Use this profile and search again**. Axis saves the profile, searches
  again, and comes back with a notice that names the new profile. To change
  the limits of the profile itself, follow the link to its page.
- Every acceptable release failed before: take one off the blocklist, or
  clear it, and search again.
- No indexer or no download client: follow the link to the **Indexers** or
  **Download clients** page, then select **Search for this movie**.
- No destination, or the profile cannot be used: choose another in
  **Settings for this movie**.

On the **Movies** list and on **Wanted**, the reason of a movie that needs
attention is a link to that box. The dashboard names the most common reason
after the count.

### Search by hand and choose a release

A manual search asks every indexer now and lists everything it finds. It
grabs nothing by itself: you choose. It does not change the movie's state or
when Axis searches by itself, and it runs even when the movie is not out yet
or is in the library already.

1. On the movie's page, select **Manual search**, beside **Search for this movie**.
   Axis opens the movie's **Manual search** page. While the search runs, the
   page says so and shows the results when they are in. If the page does not
   change by itself, select **Reload this page**.
2. Read the releases. They are in four groups:

   | Group | What it holds |
   | --- | --- |
   | **Acceptable under the profile** | Releases the profile accepts, best first. |
   | **Rejected by the profile** | Releases the profile rejects, each with its reasons. |
   | **Not what this search asked for** | Releases of this movie that the automatic rules do not take. |
   | **Ruled out** | Another movie, not certainly this one, a sample, an archive, or a release on the blocklist. **Grab this one** does not take these. Where the rule is only about the name, you can grab the release by force (see the next section). |

3. To change the order, select a link after **Sort**: **Size**, **Age**,
   **Quality**, **Indexer** or **Seeders**. To show the releases of one
   indexer or one kind, select its link after **Indexer** or **Kind**.
4. Select **Grab this one** on the release you want. A confirmation page
   says in one sentence what happens, for example "Grab ... (WEB-DL 1080p)
   by hand for the movie, with the quality profile Small 1080p set aside."
   It also names any file in the library that the release replaces, and
   says whether that is an upgrade or a downgrade.
5. If the release is not better than the file in the library, tick
   **Replace ..., which this release is not better than**. Without the tick,
   Axis sends nothing.
6. Select **Grab this one by hand**. Axis sends the release and records that
   you chose it by hand.

The import of a release you grab by hand does not refuse it for the
profile's rules. It still checks that the file is this movie, and it
replaces a better file only when you ticked the box for that file. A
release the profile accepts, grabbed with **Grab**, is imported by the
profile's rules as before. An indexer gives a
release only for a while after its search, about fifteen minutes. After
that, select **Manual search again** before you grab one.

Only an administrator can search by hand and grab. Other accounts see the
list and the reasons. The movie's **Releases found by the last search** card keeps the last automatic
search, and its link **The last manual search**
leads to this page.

### Replace a download in flight

Axis downloads one release of a movie at a time. When a release of the movie
is downloading and you find a better one on the **Manual search** page, you
can replace the download with the release you chose.

1. Select **Grab this one** on the release you want (or **Grab it by
   force**, for a release that Axis cannot match by its name). The
   confirmation page has a card, **A download is in flight**. It names the
   release that is downloading, its download client and how far it is.
2. Tick **Replace the download in flight with this one**.
3. Tick any other box that the page asks for, then select **Grab this one by
   hand** (or **Grab it by force**).

Axis then does the following, in this order:

1. It makes every check of the new grab that it can make first. If one
   fails, for example a box that is not ticked or a destination disk that is
   offline, nothing changes and the download continues.
2. It ends the download in flight and asks the download client to remove it
   with its data. The movie's history says who replaced it, and with which
   release.
3. When nothing of the old download is left at the client, it sends the
   release you chose.

The release you replaced is not put on the blocklist, so a later search can
take it again. Replacing starts no search.

The page does not offer the replace in these cases, and says why:

| What the page says | What to do |
| --- | --- |
| "... has arrived and waits for its import. Resolve the import first." | Select **Open its import**. A download that has completed is not replaced. To get rid of it, use **Delete this download** on the import's page (see [Imports, leftovers, and cleanup jobs](05-imports-leftovers-and-cleanup-jobs.md)). |
| "A release ... is being sent to the client right now. Try again in a moment." | Open the confirmation again after a few seconds. |
| "Axis is still removing an earlier download ... from the client. Nothing is grabbed until it is gone." | Wait. Axis asks the client again every minute. If it does not go, remove it in the client. |
| "This very release is the one in flight" | Nothing: the release you chose is the one that is downloading. |
| "This release is the same torrent as the one in flight ..." | Nothing: another indexer lists the torrent that is downloading. |
| "... and another download of the movie is still at a download client ..." | Select **Open the Downloads page** and remove that other download first. Axis grabs nothing for the movie while it is there, so a replace would end your download and send nothing. |
| "It is on the blocklist ..." or "A fixed rule rules it out ..." | That rule still applies. A release that Axis does not let you grab is not offered as a replacement. |

After you confirm, one of these can happen instead of the replace:

| What the page says | What happened | What to do |
| --- | --- | --- |
| "The download in flight changed after you opened this page ..." | The download arrived, or another grab took its place, while the page was open. Nothing was ended and nothing was sent. | Read the page again. It shows what is true now. |
| "... is not replaced. Its client could not be asked where it is: ..." | The download is a torrent, and its client did not answer, or part of the client's list could not be read. Nothing was ended and nothing was sent. The download continues. | Try again when the client answers. |
| "... is not replaced. Axis does not ask the client to delete it: ..." | The download is a torrent, and its client says that its files are in a library folder, or in a place that none of the client's path mappings reaches. Nothing was ended. | Remove the torrent in the client yourself, or add a path mapping for the folder the client downloads to, on the client's page. |
| "... was ended, but it is still at its client. ... Nothing was sent." | The client answered at first, then did not remove the old download. Axis keeps asking it every minute, and grabs nothing else for the movie until the download is gone. | When it is gone, grab the release again from the manual search, or leave the movie to the next search. |
| "The download in flight ... was removed, but this release could not be sent: ..." | The old download is gone, and the indexer or the client did not give or take the new release. Nothing is downloading for the movie now. | Choose another release. If the movie is monitored, the next scheduled search also looks for it. If it is not monitored, nothing searches for it until you do. |
| "... was removed, and a search grabbed ... for the movie in the same moment." | A scheduled search took a release between the two steps. Only that one download was sent. | Replace that download from the manual search if you want another release. |

For a torrent, Axis asks the client where the torrent's files are before it
asks the client to delete them. A torrent that someone moved into a library
folder in the client is never deleted through this page.

If the old download could not be removed at first, and it then finishes at
the client in a library folder, Axis stops asking the client and leaves the
download there. Axis does not import it. The **Downloads** page shows it
under **Finished lately** with "Left at the download client: It was to be replaced,
could not be removed ...", and the movie's history says the same. Remove the
torrent in the client. The movie is no longer in flight, so it can be
grabbed again.

Only an administrator can replace a download.

### Block a release you do not want

To keep Axis from taking a release, put it on the movie's blocklist
yourself. Only an administrator can block.

- On the **Manual search** page, select **Block this release** on the
  release's row. The page comes back at that row, which now reads "On the
  blocklist (blocked by ...)" with your name. Nothing is downloaded or
  deleted.
- For a download you do not want, select **Block and search again** on its
  row of the **Downloads** page. A page says what happens before anything
  changes. Tick the box and select **Block and search again**. Axis blocks
  the release, asks the download client to remove the download with its
  files, and looks for the next best release. After a release you chose by
  hand, you choose the next one too.
- For a file that is in the library already, select **Block this release**
  on the file's row of the movie's page. Axis only blocks the release for
  the future. The file, its link to the movie and everything else stay as
  they are. To replace the file, search for the movie afterwards.

A release you block is blocked by its name from every indexer, and by its
hash when it is a torrent. A release that failed is blocked only from the
indexer it came from. The **Blocklist of this movie** card says how each
entry got there: **Failed**, **Blocked by** a person, or **Download given up
on by** a person. To undo a block, select **Unblock** on its entry.

### Grab a release that Axis cannot match by its name

Sometimes the right release is under **Ruled out**. Axis reads a release's
name to decide whether it is your movie. When the name fits more than one
movie that Axis knows (two movies of the same name and year, for example),
when it reads as another title or another year, or when no title can be read
from it, Axis does not guess, and no search takes the release. If you have
checked the release yourself, you can grab it by force.

1. On the movie's **Manual search** page, find the release under
   **Ruled out**. A release you can force says "Ruled out by a fixed rule
   about its name: Axis cannot tell that it is this movie."
2. Select **Grab it by force**. The confirmation page has four parts:

   | Part | What it says |
   | --- | --- |
   | **The rule you are setting aside** | The rule in Axis's own words, for example "it could be more than one title (2 titles fit ...)". |
   | **What it is grabbed for** | The movie, and nothing else. |
   | **What happens when it has downloaded** | The file is imported as this movie without judging its name again. |
   | **What it replaces** | Each file of the movie in the library, with its quality and whether the release is better. |

3. To replace a file in the library, tick **Replace ...** for that file.
   A forced release replaces nothing that you did not tick, even when it is
   better. Without the tick for a file that would be replaced, Axis sends
   nothing.
4. Tick **I have checked that this release is ...**.
5. Select **Grab it by force**. Axis sends the release and records the rule
   you set aside, with your name, in the movie's history.

If something changed after you opened the confirmation page (a file of the
movie arrived, or another grab was made), Axis sends nothing and shows the
page again as it is now. Check it and confirm again.

Some releases cannot be forced by anyone: a sample, a password-protected
release, a name that ends in an archive's or a program's extension, and one
part of a multi-part Usenet post. Axis could not import what they deliver.
A release on the blocklist must be taken off the blocklist first.

The import of a forced release still checks that the file is a video that
can be read and that its runtime is close to the movie's. If a check fails,
the import is blocked and you decide on the **Imports** page.

### A file that is kept by hand

The file of a forced release is kept by hand. On the movie's page, the
**Files of this movie** card says "From a release grabbed by force by ..."
and "Kept by hand", and the movie's state says who keeps it. Axis does not
upgrade a file that is kept by hand: neither the scheduled search nor
**Search for this movie** grabs a replacement. You forced the release
because Axis could not tell which releases are this movie, so Axis does not
choose its replacement either.

- To let Axis upgrade the file by the profile's rules again, select
  **Let Axis upgrade it again** beside the file.
- To keep it by hand again, select **Keep it by hand**.
- To replace a kept file yourself, use **Manual search** and grab a release
  by hand. Its confirmation names the file that it replaces.

Only an administrator can grab by force and change whether a file is kept.

### Say by hand which files are a movie's

Sometimes a file on disk is linked to the wrong movie, or to none because
Axis could not match its name. An administrator can put that right on the
movie's links page. Only the link changes. Axis never moves, renames, or
deletes the file on disk.

1. Open the movie's page and select **Files and links**.
2. Find the file. The page lists every video file that is linked to the
   movie, with how its link was made, and the video files in the movie's
   folder that have no link.
3. If the file is somewhere else in the library, open it on the **Library**
   page and note the number at the end of its address, for example `123` in
   `/library/file/123`. Enter that number under **A file from elsewhere in
   the library** and select **Show this file**.
4. Say what the file is:
   - To link a file to the movie, select **Link to this movie**. A file that
     is linked to another movie leaves that movie.
   - To remove a link, select **Unlink**.
   - To leave a file as it is, leave its row empty.
5. Select **Review the change**. The review shows, for every file and every
   movie that the change touches, what is linked before and after. It also
   says whether a movie that is left without a file is searched for again.
6. Select **Change the links**.

You can also start from the file. On the **Identify** page, a file that Axis
could not match has the link **Link to a movie or episode by hand**, and the
file's page in the **Library** has the same link. It shows the file's name,
folder, and size, and a form to search the movies you manage (or the series,
for a file in a TV library folder) by name. Select a result to open that
movie's or series' links page with the file already listed. Nothing changes
until you confirm there. A file that is not a video, or is not in a library
folder for movies or TV, gets no form.

When you link a file to a movie that already has a file, both files stay
linked. Axis never replaces the movie's present file by itself. The review
says that the movie then has two files, and offers a tick box for each
present file to unlink it instead. After you tick, select **Show the review
with these ticks**, check the result, and then select **Change the links**.
A movie with more than one file is judged for upgrades by its best file.

A link that you make this way is marked as made by hand and kept by hand.
Identification and imports do not move, remove, or replace it, and Axis does
not search for an upgrade. A file that you unlink stays unlinked:
identification does not link it again. The links page lists it as unlinked by
hand.

To hand a file back, select **Let Axis manage this file again** under
**Linked, kept or unlinked by hand** on the links page. A page says what
follows before anything changes:

- For a file that you unlinked, identification links it again at its next
  run if it reads the file as a movie that Axis manages.
- For a file that identification also reads as this movie, the link stays
  and Axis upgrades the file by the ordinary rules again.
- For a file that identification reads as another movie, or as none, there
  is nothing to hand back. The link stays as you made it. To give it up,
  unlink the file.

You cannot change links while an import of the movie is open or a download
for it is in progress. The review says which one to wait for.

To ask "why did it grab nothing?", read the line under the state badge and
the reasons under each release. Typical reasons are a quality the profile
does not accept, a size outside the allowed range, a release too old for
your Usenet provider's retention, or a name that does not match the movie.

## Follow it to the library

The movie's page says what is happening to it under its state: a search
that is running or waiting for its turn (since when, and how many searches
are ahead of it), its download at the download client (how far it is), or, for
fifteen minutes after a search ends, what that search found. The line
changes by itself while the page is open.

**Search for this movie** cannot be selected while a search of the movie is in hand,
and reads **Searching** or **Waiting for its turn**. For a minute after a
search ends it reads **Searched a moment ago**, and the notice says when you
can ask again. **Manual search** leads to the manual search in hand
instead. After any action on the page, it comes back to the card you used,
with the notice there.

The **Activity** tab lists in plain words everything Axis is doing now, the
downloads, the work waiting for its turn, and what ended in the last hour.
The header says how many things are running and what kind they are.

1. Open **Activity**, then **Downloads**. A download shows under **In
   progress** with its **Progress**, **Speed** and **State**. When it is
   finished it moves to **Finished lately**.
2. Open **Activity**, then **Imports**. The import shows under **Waiting imports**
   or **Recent imports**. A state badge says **Importing**, **Imported**,
   **Waiting**, **Blocked**, **Dismissed** or **Files gone**. What an
   import does with the files is in
   [Imports, leftovers and your own cleanup jobs](05-imports-leftovers-and-cleanup-jobs.md).
3. When the import ends, the movie's state is **In library**. Its **Files of
   this movie** card shows the file, its library folder, its quality, and
   **How it got there**: **Moved**, **Hardlinked** or **Copied**.
4. **History** (under **Movies**) lists every step with its time: searched,
   grabbed, arrived, imported.

An administrator can take a download out of its download client on the
**Downloads** page:

1. Select **Remove** in the download's row. A page says what happens before
   anything changes.
2. To have the client delete the download's files too, tick **Also delete
   its files at the client**.
3. Tick the box that names the client, and select **Remove this download**.

When a torrent's client says that the torrent is in a library folder
(someone moved it there in the client), the page says so and does not offer
to delete the files. Axis removes the torrent from the client and leaves
its files where they are. The notice says so, and the download's line under
**Finished lately** reads "Files left at the download client: ..." with the
reason. A torrent in the client's own download folder, and one at a place
that no path mapping of the client reaches, is deleted by the client when
you tick the box.

If you use Plex or Jellyfin, Axis can ask it to rescan the one folder the
import changed. The Setup guide step **Media servers (Plex, Jellyfin)** connects one.

## What can go wrong

| What you see | What to do |
| --- | --- |
| **Needs attention**: "Its destination library folder was removed: choose another under Settings for this movie." | Change **Destination** in **Settings for this movie**, and select **Save**. |
| **Needs attention**: "Its destination ... is on ..., which is offline: nothing is searched for or sent until it is back." | Bring the volume back. |
| The state stays **Not available yet**: "Not searched for before ... (search from: ...)." | Wait for the date, or change **Search from** in **Settings for this movie**. |
| A search finds releases and every one is **Rejected** | Read **What you can do** under the state. Choose a profile marked "accepts this release" and select **Use this profile and search again**, or use **Grab it anyway** on a release you accept. To see every release and choose one, use **Manual search**. |
| **Grab this one** says "the search that found it has expired: search again" | Select **Manual search again**, then grab the release from the new list. |
| **Grab this one** shows **A download is in flight** | Another release of the movie is downloading. To take the release you chose instead, see [Replace a download in flight](#replace-a-download-in-flight). |
| **Wanted** forever, last search found nothing | Check that an indexer is **Working** and that it covers movies. |
| A search job is **Failed** on the **Jobs** page | Open the job and select **Retry job**. Axis runs that same job again, with its attempts counted from the start, and lists how the earlier run ended under **Earlier runs of this job**. If another job is already searching for the movie, **Retry job** names that job and runs nothing; follow that job instead. |
| The download is done and the import is **Blocked** | Open the import. The **What resolves it** card offers **Import anyway** (tick "I checked: it is this movie"), **Import this file**, **Assign to it** for another movie, or **Dismiss (leave the download where it is)**. |
| The import is **Waiting**: its path does not map | Add a path mapping on the client's page. |

## Verified

- One movie has been taken end to end on a real server: search, SABnzbd,
  a hardlinked import, and Plex.
- Imports that move the files (the default now) have run on a real server
  for one Usenet download and one torrent, on the same disk. The cross-disk
  copy path has not run on real hardware.
- qBittorrent has not run against a real client. Torznab feeds have run
  against test doubles only.
- Searching and the release verdicts have run against real indexers, as
  [the previous page](03-connect-indexers-and-download-clients.md) says.
- **Retry job** on the **Jobs** page and the queue searches run on have run in
  the tests only, with test jobs, not on a real server.
- The **What you can do** box, the profiles marked by what they accept, and
  the search after a new quality profile are checked by the tests with a
  made-up movie; they have not been seen in a browser yet.
- **Manual search**, its groups, sorting and filtering, **Grab this one**
  with its confirmation, and the import of a release grabbed by hand have
  run in the tests only, with made-up movies and test doubles for the
  indexer and the download client. They have not run against a real
  indexer or been seen in a browser yet.
- **Replace the download in flight with this one** has run in the tests
  only, with made-up movies and test doubles for the indexer and the download
  client (a Usenet client and a torrent client). It has not run against a
  real download client or been seen in a browser yet. What qBittorrent and
  Deluge report as the place of an unfinished torrent has not been checked
  against a real client.
- The movie's line of what is happening, the buttons that say a search is in
  hand, the answer to a search asked twice, the return to the card you used,
  and the **Activity** tab have run in the tests only, with a made-up movie;
  they have not been seen in a browser yet.
- Adding several movies from a list (the bar above it, the return to the
  card, the link back from a title page) has run in the tests and in
  screenshots of the demo at phone width only. That the bar's change is
  saved without a reload has been checked as far as the server's answer.
