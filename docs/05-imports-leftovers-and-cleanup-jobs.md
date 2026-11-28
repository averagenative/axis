# Imports, leftovers and your own cleanup jobs

This page shows you what an import does with a download's files, how to
clear what the import did not take, and how to run a cleanup job of your
own beside Axis. Allow about 15 minutes to read it and check your settings.

The short version: an import **moves** the files it takes out of the
download folder and into your library. So anything still in a download
folder afterwards was not imported.

## Before you begin

You need a movie or an episode that Axis grabbed, so that a download
exists ([Find and download a movie](04-find-and-download-a-movie.md)), and
**Read-only storage** switched off in **Settings**.

## How an import moves files

Open **System**, then **Settings**, and find the **Imports** card. Four
settings decide what an import does:

| Setting | Values | What it does |
| --- | --- | --- |
| **Files taken from a download** | **Move into the library** (default), **Always hardlink** | **Move into the library** renames or copies the files into the library. **Always hardlink** keeps the files where they are and links them. |
| **After a torrent is imported** | **Remove it from the download client** (default), **Leave it seeding** | **Remove it from the download client** stops the torrent as soon as every wanted file is in the library. **Leave it seeding** keeps it seeding, which a private tracker that needs a ratio requires. A seeding torrent is hardlinked into the library, which takes no extra space. |
| **Download and library on different disks** | **Copy and verify** (default), **Never copy: block the import** | With **Copy and verify**, a file on another disk is copied. The Download clients and Storage pages say so once for each download client that has library folders on other disks: for a Usenet client, or a torrent client that is removed after import, the copy costs only time and needs no action; for a torrent left seeding, it costs as much space again while the torrent seeds. With **Never copy: block the import**, such an import is blocked and never copied. |
| **Keep files replaced by an upgrade for** | a duration, `2d` by default | When an upgrade replaces a file, the old one waits in `.axis-recycle` at the top of its disk for this long. `0` removes it as soon as the upgrade is recorded. A file removed on the **Duplicates** page waits there the same way. |

With the defaults, each import goes like this:

1. A Usenet download is moved at once. Axis never moves a file over another
   name.
2. For a torrent, Axis first asks the client to let go of the torrent,
   without its files. Axis moves nothing until the client says it has. If
   the client cannot be reached or does not say so, Axis tries a few more
   times, and then asks you on the import's page. You can **Import anyway**.
   If you moved the torrent to another category or label at the client, for
   example to keep it seeding, the torrent is yours: Axis leaves it there,
   hardlinks its files into the library (or copies them from another disk),
   and never removes or deletes anything of it. The import's page says so,
   and the **Leftovers in the download folders** page never offers its
   folder.
3. When the download and the library are on the same disk, each file is
   renamed, in an instant. From another disk, Axis copies the file, reads it
   back and checksums it, and deletes the download's file once the import is
   recorded. Axis deletes it only if nothing changed it since Axis started
   to read it: if a client or another program writes to it, even while Axis
   is still checking the copy, Axis keeps it and the import's page says
   that it is not the file copied. The import's **How it got there** column
   says
   **Moved**, **Moved (copied, verified)**, **Hardlinked** or **Copied,
   verified**.
4. Axis deletes what the import did not take (an `.nfo`, a sample, `par2`
   files) together with the download's folder. It does this only inside the
   client's folder for Axis's category or label, and only by strict rules.
   The import's page says what it did in **Folder at the download client** (for a torrent, **Files left at the download client**).

For the best result, keep downloads and library on one disk, mounted once
([the one layout rule](../deploy/README.md#the-one-layout-rule)). Then
every import is a rename.

Open **Activity**, then **Imports**, to see all of this. The page has three
cards: **Blocked imports**, **Waiting imports** and **Recent imports**. **File naming** (an
administrator's link on that page) shows what names Axis gives the files.

<!-- screenshot: the Imports page with one import Moved and one Blocked -->

## Assign a download's files by hand

Sometimes Axis cannot tell what a finished download holds, or reads it
wrongly: no file's name gives an episode, a movie's file is named as another
movie, or the series changed its numbering after the release was grabbed.
The import is then blocked, and nothing is guessed. You can say what the
files are yourself. You need to be an administrator.

1. Open **Activity**, then **Imports**, and open the blocked import (or one
   that ended partly imported).
2. Select **Assign by hand**.
3. Say what each video file is:
   - For a movie, choose the one file that is the movie.
   - For a series, write the episodes each file holds, for example `S01E03`,
     or `S01E03 S01E04` for a file that holds two. Use the numbering the
     series follows now. Leave a file's field empty to leave that file
     alone. The field starts with what Axis reads from the file's name, if
     anything.
4. Select **Review the assignment**. The next page writes nothing. It shows
   what each chosen file becomes, the name it gets in the library, its
   runtime beside the expected one, and every file in the library that it
   would replace, with that file's quality.
5. Tick each file that you want replaced. A file in the library is never
   replaced without its own tick. If a file runs much longer or shorter than
   expected, tick that you assign it all the same.
6. Select **Assign and import**.

Axis then imports the files you chose as what you said. It does not judge
their names, whether the episodes are wanted, or the quality profile. It
still checks that each file is a video it can read, and every rule about
disks and copying applies as for any import.

What to expect afterwards:

- A file you did not choose is left alone. It stays in the download: after
  an import that you assigned by hand, Axis does not delete the download's
  folder or anything left in it. Delete what is left yourself under
  **Leftovers in the download folders** when you no longer need it.
- A replaced file goes to the recycle folder, as with any upgrade. Axis
  deletes nothing at once.
- The file is marked as assigned by you and is **kept by hand**: Axis does
  not upgrade it by itself until you let it go on the title's page.
- If the library, the series or the import changed after the review was
  shown, nothing is applied and the review is shown again as it stands now.
- Only video files can be assigned. An archive, a sample that is not a
  video, or a password-protected file is never imported this way.
- An episode or a movie that another download is on its way for cannot be
  assigned until that download is done or removed.

To assign a download that has no open import, open **Import a movie by
hand**, choose the download and the movie, tick **Assign by hand**, and
select **Import the file for this movie**. Axis reads the files, imports
nothing, and waits for you on the import's page.

If the download is another title's, use **It is another movie** (or
**It is another series**) on the import's page first. The new import offers
**Assign by hand** in the same way.

## Delete a download you do not want

When an import is blocked, waiting or partly imported, the download might be
the wrong thing: a sample, a fake, or a release you do not want after all.
**Dismiss this import** closes the import and leaves the download on the disk
and at its client. **Delete this download** closes the import and deletes the
download in both places. You need to be an administrator.

1. Open **Activity**, then **Imports**, and open the import.
2. Select **Delete this download**. On a blocked or waiting import, it is
   beside **Dismiss this import**. On a partly imported one, it is in the
   card **Delete the rest of this download**. The **Downloads** page has the
   same link on the row of a finished download whose import is blocked or
   partly imported.
3. Read the page **Delete this download?**. It writes nothing. It shows:
   - **On the disk** and **At the download client**: the folder or file that
     goes.
   - **What**: a folder with the number of files in it, or one file. For a
     torrent that its client deletes, the page says that the client deletes
     the torrent's own files, and how many files the folder holds now. A
     file in that folder that is not the torrent's stays.
   - **Size**: the total size of those files.
   - **Who deletes it**: the torrent client, or Axis.
   - What stays: the files that this import already placed in the library,
     each with its place.
4. Optional: tick **Also put this release on the blocklist, so that no
   search takes it again**. The box is not ticked by default. Without it,
   the next search can take the same release again.
5. Select **Delete this download**.

Deleting cannot be undone. The files do not go to a recycle folder.

What happens next:

- The import is closed, with your name. Its state reads
  **Download deleted**. The grab, when one is open, is closed, and the movie
  or the episodes that were not imported are wanted again. Axis starts no
  search now. The next scheduled search takes the title up. To block the
  release and search at once instead, use **Block and search again** on the
  **Downloads** page.
- For a torrent that its client still has, Axis asks the client to remove
  the torrent with its files. Axis deletes nothing itself. Axis asks only
  when the client says the torrent is still at the place where it
  completed, and that place passes the rules below.
- For a Usenet download, the client removes the job and keeps its folder.
  Axis deletes the folder.
- When the client no longer has the download, for example because the
  torrent client dropped the torrent and left its folder, Axis deletes what
  the client left.
- The import's page says what was deleted under **At the download client**
  and under **Folder at the download client** (for a torrent, **Files left
  at the download client**).

Who deletes what:

| Download | The client still has it | The client no longer has it |
| --- | --- | --- |
| Usenet (SABnzbd) | The client removes the job. Axis deletes the job's folder. | Axis deletes the job's folder. |
| Torrent (Deluge, qBittorrent) | The client removes the torrent and deletes its files. | Axis deletes the torrent's folder or file, but only when the client keeps Axis's label or category in a folder of its own. |

What Axis never deletes this way:

- Anything in a library folder, a library folder itself, or a folder that
  holds a library folder.
- The client's download folder itself. Only the download's own folder or
  file inside it goes. A download that is one file, with no folder, loses
  only that file.
- Anything that Axis reaches through a symbolic link, or on another
  filesystem. A symbolic link inside the download's folder is removed as a
  link, and the file it points to stays.
- A folder or file that is not the one the client reported when the
  download completed. Axis records what was at that place then, and compares
  it now.
- Anything on a disk that is offline or mounted read-only, or while
  **Read-only storage** is on.
- A place that another download, or another open import, is in or around.
- A torrent that you moved in its client after it completed, for example
  into a library folder. A torrent client deletes a torrent's files wherever
  they are, so Axis does not ask it. The page says "the torrent is no longer
  where Axis recorded it". Remove the torrent in the client, or select
  **Dismiss this import**.

When nothing is at the download's place and the client no longer lists the
download, nothing is left to delete. The page says so, and you can still
confirm to close the import. Axis asks the client to delete nothing.

When one of these rules applies, the page says which, says where the files
are, and shows no button. Nothing changes. You can still dismiss the import
and delete the files yourself.

A file that the import already placed in the library by a hard link is not
affected. The download's name for the file goes. The library's name and the
content stay. The page says how many files that applies to.

If the client does not answer or the disk cannot be written in the moment
after you confirm, the import is closed and its page says what is still to
be done. The hourly job **Removing old replaced files and download leftovers
after imports** finishes it under the same rules. The job looks at the
download again first. If you moved the torrent in the meantime, the job
deletes nothing and the import's page says why.

<!-- screenshot: the Delete this download? page for a made-up Usenet download, with the path, the number of files, the size and the blocklist box -->

## Clean up duplicates

Axis keeps one copy of each movie and each episode. A grab by force, an
assignment by hand, or a link made by hand can leave a second file linked
to the same movie or episode. The **Duplicates** page lists those, says
which file Axis would keep, and lets an administrator remove the others.

A file that holds two episodes is not a duplicate by itself. It is listed
only when one of its episodes has another file too.

1. Open **Media**, then **Duplicates**. Each card is one movie or one
   episode. The table shows each **File** with how it was linked and the
   disk it is on, its **Quality**, its **Size**, its **Running time**, and
   the **Recommendation**.
2. Read the line under the card's title. "Axis would keep ..." names the
   file to keep and the reason. Any other line means that nothing is
   recommended, and you decide. The line says why.
3. Select **Review the clean-up**. The review lists each file with a tick.
   A file that Axis recommends removing is ticked for you. Change the ticks
   if you want another outcome.
4. Select **Move the ticked files to the recycle folder**.

Two files that are linked to one movie are not always copies of it. They
can be the two parts of one movie, or one of them can be cut short. Axis
recommends a file to keep, and ticks the others, only when all of the
following are true:

- The analysis has read every file of the group as the file is now. A file
  that was never analysed, that changed after it was analysed, that the
  analysis could not read, or that is empty, stops the recommendation. The
  page says "Axis could not read one of these files" and names the file.
- The running times are within 5% of each other. Otherwise the page says
  "These files run for different lengths, so they may not be copies of the
  same thing".
- No file name states a part of a set that another file name does not
  state, for example `CD1` and `CD2`, or `Part 1`.

When those are true, Axis chooses the file to keep the way it decides an
upgrade, in this order:

1. The quality that is earlier in the title's quality profile, when the
   profile lists both qualities.
2. Otherwise, the higher quality on the standard quality ladder. A quality
   that the profile does not list is not worse for that reason.
3. For the same quality, the later revision (a proper or a repack), then
   the higher score in the quality profile.
4. For the same quality, revision, and score, the larger file.
5. For the same size too, the file that is kept by hand or was linked by
   hand.

If the title's quality profile cannot be used, Axis cannot compare two
files of the same quality by revision and score. If their sizes differ, it
says "Axis cannot tell which file is better".

What to expect:

- A ticked file moves into `.axis-recycle` at the top of its own disk and
  stays there for the time set in **Keep files replaced by an upgrade
  for** (two days by default). Until then, you can move it back by hand.
- If that setting is `0`, the recycle folder keeps nothing. The review
  says that the file is deleted for good, and the button reads **Delete
  the ticked files for good**. You cannot undo this.
- A file that you do not tick stays where it is, linked as it is.
- At least one file always stays. A request that ticks every file is
  refused, and nothing is removed.
- The file that stays must be a real, readable file. Axis looks at the
  disk when you confirm. If every file that would stay is missing from its
  disk, empty, never analysed, or unreadable, the request is refused and
  nothing is removed. Scan the library, wait for the analysis, then open
  the review again.
- The same holds for a file that holds two episodes. Axis removes it from
  one episode only if the other episode keeps a real, readable file too.
- One request removes at most 20 files. Tick fewer, then come back for the
  rest.
- A file that is kept by hand, or that a person linked by hand, is never
  ticked for you. The review says who linked it. Tick it yourself only if
  you mean to remove it.
- Some files are not offered at all, and the review says why: a file with
  another name on its disk (a hard link), a file in a downloads folder, a
  file on a disk that is offline or read-only, any file while **Read-only
  storage** is on, and a file that is the only file of another episode on
  a disk that Axis can read.
- Subtitle files beside a removed file stay where they are.
- The title's history records who removed which file, where the file is in
  the recycle folder, and when it is deleted from there.

To find a removed file again, open **Duplicates**, then **Recycled
duplicates**. The list shows each removed file, the movie or episode it
was a file of, who removed it and when, where it is now, and when it is
deleted. The list has no restore button. To put a file back, move it by
hand from the place shown to the place it had, then scan the library.

If Axis loses the database while it removes a file, the page says what
happened to each file. A file can be "moved, but not recorded": it is in
the recycle folder, and the library still lists it for a short time. The
hourly job **Removing old replaced files and download leftovers after
imports** finishes the record. You do not need to do anything.

A viewer can open the page and read the recommendations. Only an
administrator sees **Review the clean-up**.

Disk usage has a separate report, **Likely duplicate files**, for files
with the same content and for folders of the same title in more than one
place, whether or not Axis links them. The **Duplicates** page links to
it.

<!-- screenshot: the Duplicates page with one made-up movie that has two files, and the recommendation -->

## Clear the leftovers

A leftover is a file or folder in a client's folder for Axis that belongs to
no download and no import. Examples are a download Axis gave up on whose files
the client kept, a download that failed, and what a rule left of a
download's folder.

1. Open **Activity**, then **Downloads**. When something is left, a line
   says "1 file or folder in the download clients' folders for Axis belongs
   to no download and no import, so none of it was imported". Select
   **Leftovers in the download folders**.
2. For each client, the page shows a card named **Leftovers at** the client,
   with **Download folder for Axis** and the **Disk** it is on. The table has the columns **Name**, **Size**,
   **Changed** and **Can be deleted**. A folder shows a **Folder** badge.
3. For an entry that says **Yes**, select **Delete**. The page **Delete a
   leftover** shows the entry's **Name**, **What** it is (a file, or a
   folder with everything in it), its **Size** and the folder it is **In
   folder**.
4. Tick **Delete NAME for good**, and select **Delete this leftover**. Deleting cannot be
   undone. Only this entry goes. Axis follows no symbolic link, enters no
   other filesystem, and keeps a file that has another name in the library.
   Axis deletes nothing unless you ask, one entry at a time.

An entry that does not say **Yes** gives its reason: it is in use at the
client, Axis cannot tell it apart from what the client has, or it is not
Axis's to delete.

Axis asks each client for everything it has, in every category or label,
not only Axis's. A download of another program, or a torrent you moved to a
category of your own, is not a leftover. Axis also asks every other
download client whose path mappings reach the folder, including one you
switched off in Axis, because its program may still be running. While such
a client cannot be reached, nothing in the folder can be deleted, so
remove a client from Axis when you no longer use it. When a client lists
an entry that Axis cannot read, nothing in the folder can be deleted
either, and the page names that entry and says why.

Give each client a folder of its own for Axis. If two clients keep Axis's
downloads in the same folder, or one inside the other, the page lists
nothing in it and names the other client. The page does the same when
another client whose mappings reach the folder keeps Axis's downloads in
no folder of their own, because they may be in this one. Each client's
page says the same under **Problem**: within a minute after you select
**Test**, and otherwise within the hour.

<!-- screenshot: the Leftovers in the download folders page listing two entries for a made-up client -->

## Run a cleanup job of your own

A job of your own that deletes old things in the download folders, such as
a cron job that removes every folder in a client's completed folder older
than a day, is safe beside Axis. Axis expects it. Give Axis's category or
label a folder of its own at the client, so that its downloads are not
mixed with other programs' downloads.

Here is what happens in each case:

| Your job deletes | What Axis does |
| --- | --- |
| A download before its import | The import ends as **Files gone**, which is final. Axis gives up on that download for good, and looks for the movie or episode again. |
| A folder after its import | Axis records the download as gone. Nothing is lost: the library has its own copy. |
| The files of a seeding torrent | The library's hardlink is not affected. The import's page says the files are gone. |

## What can go wrong

| What you see | What to do |
| --- | --- |
| The import is **Waiting** with "Storage is read-only" | Switch **Read-only storage** off in **Settings**. |
| **Retry job** on the **Jobs** page says "This job cannot be retried from the Jobs page. Retry the import on its page under Imports" | An import is tried again from its own page, which resets it and checks that no other import of its title is open. Open the import under **Imports** and select **Retry** there. Axis also takes up a waiting import by itself. |
| The import is **Blocked** | Open it. The **What resolves it** card lists what you can do. Reasons Axis never lets you override: a volume that is away or read-only, a copy that does not verify, a path outside a volume. |
| A torrent's import waits for its client | The client has not confirmed that it let go. Check that it answers, then **Import anyway** on the import's page if you know it is safe. |
| Every import is a copy and space doubles for a while | The download folder and the library are on different disks, or on one disk mounted twice. See the layout rule above. |
| **Files gone** on an import you did not expect | A cleanup job deleted the download first. Make its age longer than your slowest import. |
| A leftover does not say **Yes** | Read its reason. Delete it by hand on the disk if you are sure. |
| **Duplicates** says "Nothing was removed: the files, their links or their disks changed since this page was shown" | Something changed after you opened the review. The page shows the files as they are now. Look again, then confirm. |
| **Duplicates** says "An import of ... is open" or "A download for ... is on the way" | An import or a download of the same title can replace these files. Let it finish, or dismiss the import or remove the download, then open the review again. |
| **Duplicates** says a file "cannot be removed here" | The file is hard-linked, on a disk that cannot be written, or the only file of another episode. The message says which. Nothing was removed. |
| **Duplicates** says that no file that would stay "is one Axis can leave it with" | The file to keep is missing from its disk, empty, not analysed yet, or unreadable. The message names the file and the reason. Bring the disk back, scan the library, or wait for the analysis, then open the review again. |
| **Duplicates** says "Removing these files would leave ... without a file" | A ticked file also holds another episode, and that episode has no other real, readable file. Nothing was removed. Keep that file, or get another file for the other episode first. |
| **Duplicates** recommends nothing and says "These files run for different lengths" or "Axis could not read one of these files" | The files might not be copies of the same thing, or one might be damaged. Play the files to check them, then tick the one to remove yourself. |
| **Duplicates** says a file was "moved, but not recorded" | Axis lost the database after it moved the file. The file is in the recycle folder. The hourly job **Removing old replaced files and download leftovers after imports** finishes the record. |
| The Leftovers page says a client's folder for Axis "is not its own" | Two clients keep Axis's downloads in one folder. Give each client a folder of its own for Axis at the client. |
| The Leftovers page says a client's folder for Axis "may not be its own" | Another client whose path mappings reach the folder keeps Axis's downloads in no folder of their own. Give that client a folder of its own for Axis, or change its path mapping so that it does not reach this folder. |
| The Leftovers page says the client has something "Axis cannot read" | The client lists an entry that Axis cannot read, which the page names. Look at that download in the client. Nothing in the folder can be deleted until the entry can be read or is gone. |
| **Dismiss** says the import "cannot be dismissed now" | A run was interrupted after it moved a file into the library under a temporary name, and Axis cannot put the file back in the download now. Select **Retry this import**: Axis gives the file its name. Then dismiss the import if you still want to. If the message says to try again once it can be looked at, a disk is away, storage is read-only, or a folder cannot be opened. Fix that first, then dismiss again. |
| **Delete this download?** says "Axis does not delete this download" and shows no button | A rule refuses, and the sentence says which. The files are where the page says. Common causes: the client keeps Axis's category or label in no folder of its own, the download is in a library folder, you moved the torrent in its client, or the client's path mapping changed. Fix the cause, or select **Dismiss this import** and delete the files yourself. |
| **Delete this download?** says "It cannot be deleted now" | The client does not answer, the disk is offline or mounted read-only, or **Read-only storage** is on. Nothing was changed. Fix that, then open the page again. |
| After you confirm, the page **Delete this download?** is shown again and says that something "changed since this page was made" | The download, its place or the import changed after the page was made. Nothing was deleted. Read the page as it is now, then confirm again. |
| The import's page says the download "is not deleted yet" | The client did not answer, or the disk could not be written, in the moment after you confirmed. Axis looks again every hour. **At the download client** on the import's page says what is still to be done. |
| The import's page says the download's files "were left where they are" | A rule applied after you confirmed, for example because the client moved the torrent. The import is closed. **At the download client** and the line after it say which rule and where the files are. Delete them yourself, or on the **Leftovers in the download folders** page. |
| **Delete this download** says the download "cannot be deleted now" and names a temporary name | A run was interrupted after it moved a file into the library under a temporary name, and Axis cannot put the file back now. Select **Retry this import**, then delete the download if you still want to. |
| A dismissed import is **Blocked** again, saying "It was dismissed, but" | It was dismissed before Axis put such files back first, and its download's folder is gone. Select **Retry this import** to give the file its name. |
| **Retry this import** says "Nothing new was asked for: the import is waiting for its turn already" | The import is queued, and Axis looks at it as soon as its turn comes. The line under its state says how many jobs are ahead of it. Nothing more is needed. |
| A download waits with a path that "begins or ends with white space" | The client named the download's folder with a space at its start or end. Axis never trims a path, because the trimmed name can be another folder. Rename the folder at the client, or download the title again. |
| The client's line on **Downloads** says its list is not whole | The client holds more of Axis's downloads than Axis reads in one request (SABnzbd: 500), or lists an entry whose identifier Axis cannot read. Axis keeps the downloads it does not see as they were, and never takes them for gone. After an hour, the **Downloads** page says so too. |
| The client's line says Axis "cannot read its entries" for some downloads | The client lists those downloads without a name, a category or a label that Axis can read. Axis keeps those downloads as they were, and follows the others as before. Look at them in the client. |

## Verified

- On a real server, one Usenet download and one torrent were
  each moved into the library on the same disk. The torrent moved after
  its client confirmed it was gone. The **Leftovers in the download folders** page listed and deleted
  what was left.
- The cross-disk copy path (**Moved (copied, verified)**) has not run on
  real hardware. The tests cover it.
- The behaviour of your own cleanup job (**Files gone**, the recorded
  gone folder) has been tested with test doubles. It has not been seen on
  a real server.
- Deluge's answer has been seen for real. qBittorrent's has not, so the
  torrent steps with qBittorrent are believed and not proven.
- **Delete this download** has been tested with test doubles for the
  clients and a real directory for the disk. It has not run on a real
  server, with a real client or in a browser. A disk mounted read-only has
  no test of its own: the tests cover an offline disk and **Read-only
  storage**, which Axis refuses in the same place.
- The import page's line of its job, the answer to **Retry this import** while the
  import waits, and the return to the **Ways to resolve this import** card after an
  action have run in the tests only; they have not been seen in a browser.
