# Add disks and library folders

This page shows you how to tell Axis which disks it may use and which
folders on them hold your movies and TV. Allow about 10 minutes, plus the
time the first scan takes.

Two words matter here:

- A **disk** is one disk or filesystem. Axis recognizes it by a small
  marker file, `.axis-volume`, at its top, and not by where it is mounted.
  Until Axis can write the marker, it uses the filesystem's identifier.
  A disk that is not mounted shows as offline, never as empty.
- A **library folder** is a folder on a disk that holds one kind of media,
  such as movies or TV. Axis scans each library folder, and imports
  downloads into the library folder a title is set to.

## Before you begin

You need:

- Axis running, and an administrator account
  ([Install Axis](01-install-and-look-read-only.md)).
- Each disk mounted into the Axis container, whole, at a path of its own
  such as `/media/disk1`. Read-only (`:ro`) is fine. Use the directory as
  Axis sees it inside the container, and not the path on the host.

## Add a disk

1. Open **Library**, then **Disks**. The page is headed
   **Disks and library folders**.
2. In the **Add a disk** card, enter the **Path** the disk is mounted
   at, such as `/media/disk1`. Enter a **Name** if you want one. The name is
   optional, and the directory's own name is used when it is empty.
3. Select **Look at this directory**. Axis looks at the directory and writes
   nothing yet.
4. Read the **What Axis found in this directory** card. It shows:

   | Fact | What it tells you |
   | --- | --- |
   | **Path** | The path Axis looked at. |
   | **Filesystem** | The filesystem type and its device. |
   | **Mount point** | **Yes** when the directory is the top of a mounted disk. |
   | **Mounted** | **Read-write** or **Read-only**. |
   | **Space** | Free and total space. |
   | **Marker** | **None**, or the identifier of a marker already there. |
   | **Contents** | **Empty** or **Has files or folders**. |

   <!-- screenshot: the Add a disk confirmation page for a demo disk -->

5. If Axis shows a warning, read it. The most common one is that the
   directory is not a mount point. A disk mounted here later would then look
   like a missing disk. If the directory is meant to be a disk, tick **This
   directory is not a mount point; add it as a disk anyway.**
6. Check the **Name**. The button depends on the disk:

   - A disk mounted read-write with no marker: **Write the marker and add
     the disk**. Axis writes one small file, `.axis-volume`, and nothing
     else.
   - A disk mounted read-only with no marker: **Add the disk**. The page
     says the disk is mounted read-only, so Axis writes nothing now. Axis
     recognizes the disk by its filesystem's identifier, its type, and the
     directory, until it can write the marker. For a filesystem whose
     identifier is only its device number, such as XFS, Axis uses the
     filesystem's UUID instead, so a reboot that finds the disks in another
     order does not lose the disk. The size does not count: a disk that you
     grow, or a ZFS dataset whose size moves as its pool fills, is still
     recognized. The first time Axis finds the disk mounted read-write, it
     writes the marker, even while **Read-only storage** is on.
   - A disk that already carries an Axis marker: **Add the disk**. Axis
     writes nothing.

Axis shows "The disk has been added." The new disk appears in the list
with a state badge. A disk without a marker also shows "(no marker yet)"
beside its filesystem. Its page says "Its filesystem identifier: ... It has
no marker yet." in **Recognised by**, and **Marker written** says **Not
yet**.

| State | Meaning |
| --- | --- |
| **Online** | The disk is there and Axis recognizes it. |
| **Mounted read-only** | The disk is there, but mounted read-only. |
| **Offline** | The disk is not mounted, did not answer, or (for a disk without a marker) the filesystem or directory at the path is not the one recorded. Axis never deletes anything it knows about because a disk is offline. |
| **Not recognised** | The directory has files but no marker, or carries another disk's marker, or the marker is damaged. |

A disk without a marker can be scanned, browsed and identified. No
conversion worker is given work on it, because a worker recognizes a disk
only by its marker. The **Conversion workers** and **Conversions** pages
name such disks.

Open a disk to see its reason in words, to select **Check this disk now**,
or to change its path when you mount the disk somewhere else. Changing the
path writes nothing: Axis checks that the marker there is this disk's.

If you swap two disks between their mount points, each disk shows
**Not recognised** with the other's marker. Open either disk and give the
other disk's path as the new path. When the other disk is at this disk's
path, Axis changes both paths at once. If the other disk is somewhere else,
Axis refuses and names that disk: point it at its disk first.

## Restore a deleted marker

If someone deletes a disk's `.axis-volume` file, the disk shows
**Not recognised** and its page offers **Restore this disk's marker**. The
page compares the filesystem at the disk's path with the one Axis last saw
the disk on, and says which it is:

- The same filesystem: the page says so, and **Write the marker again**
  writes the disk's own marker.
- Another filesystem, or one that Axis cannot tell apart (a filesystem
  whose identifier is only its device number): the page says so plainly.
  Folders with the names of the disk's library folders can be on any disk,
  so they do not prove it. Tick the box only if you are sure this is the
  same disk.

Axis never writes the marker into a directory of another disk's
filesystem, whether or not that disk is online.

## Add library folders

1. On **Disks and library folders**, select **Library folders on these
   disks**.
2. In the **Add a library folder** card, choose the **Disk**. A disk that is
   not online is disabled in the list.
3. Enter the **Directory**: a path relative to the top of the disk, such as
   `Movies`, or the full path of a folder inside it. Empty means the whole
   disk.
4. Choose **What it holds**: **Movies**, **TV**, **Music**,
   **Audiobooks**, **Books**, **Downloads** or **Other**. Axis uses
   **Movies** and **TV** library folders for finding, downloading and
   identifying today.
5. Enter a **Name** if you want one. Select **Add the library folder**.

Axis shows "The library folder has been added." and starts to scan it.
Library folders on one disk never contain one another.

Repeat for each folder. A typical setup has one **Movies** library folder and
one **TV** library folder on each disk that holds them.

## What the screen shows

- On **Disks and library folders**, each disk has a **Checked** time and a
  **Space** figure. A disk that is offline shows "Last known: ..." with the
  figures from when it was last online.
- On **Files** (under **Library**), a library folder shows
  "Scanning: ..." until its first scan completes. Select **Scan now** on a
  library folder to scan it again. Axis also rescans on a schedule.
- After the first complete scan of a **Movies** or **TV** library folder,
  Axis starts to identify what is on it within ten minutes
  ([Recognize what is already on your disks](06-identify-movies-and-series-on-disk.md)).

## What can go wrong

| What you see | What to do |
| --- | --- |
| "There is no directory at this path." | Use the path inside the container. Check the `volumes:` lines of `compose.yaml`. |
| "The directory is empty and is not a mount point: this is what a disk that is not mounted looks like. Mount the disk first." | The disk is not mounted on the host, or the mount did not reach the container. |
| "The filesystem is mounted read-only and has no marker yet." followed by a reason (a network, FUSE or layered filesystem, or one that reports only a device number) and "Mount it read-write once so that Axis can write its marker; it may be read-only after that." | Axis cannot recognize this disk without a marker. Remove `:ro` from the disk's line, restart the container, add the disk, and then put `:ro` back. |
| "The filesystem is mounted read-only, has no marker yet, and this directory is empty." | An empty directory looks like a disk that is not mounted, so Axis refuses it. Add it once it holds files, or mount it read-write once. |
| The disk is **Offline** and its page says the disk has no marker yet and the directory is not the filesystem or directory recorded | Another disk is mounted at the path, or the disk was reformatted. Mount the right disk. If the disk really changed, remove the disk and add it again. |
| "This directory is on the same filesystem as disk ..." | Two directories of one disk cannot be two disks. Add the disk's top directory once, and use library folders for the folders. |
| "This directory is on the filesystem of disk ..., which is not online now" | The directory is on the disk that disk was last found on. If that disk is mounted here now, change that disk's path instead of adding a new disk. If that disk is somewhere else, change its path to there first, or remove it if it is gone for good. Axis then no longer counts this filesystem as that disk's. |
| "This is already the path of disk ..." when you change a disk's path | Another disk is recorded at that path, and it is not there, so the two cannot swap. Point the other disk at its path first. |
| "This directory carries the marker of disk ... " | The disk is already added under another path. Change that disk's path instead of adding it again. |
| The state is **Not recognised** after a reboot | Another disk is mounted at the path, or the mount failed and the empty directory shows. Mount the right disk, and then select **Check this disk now**. |
| The **Disk** list on the library folder form has no usable disk | Add a disk first, or bring an offline disk back. |

To remove a disk, open it and select **Remove this disk**. Axis forgets the
disk and what it knows about its files. Nothing on the disk is changed.
If the disk had a marker, the marker stays, so adding the directory again
recognizes the disk.

## Verified

- Adding disks and library folders, and the scans that follow, have been
  used on a real server: the real imports and the real identification run
  listed on the other pages needed them.
- Adding a disk that is mounted read-only, and recognizing it by its
  filesystem until the marker is written, is built and tested on generated
  data. It has not been seen on a real server.
- The queue records no run on a real server for a disk that goes offline
  and comes back. The tests cover it.
- Repairing two swapped disks, restoring a marker, and recognizing XFS by
  its UUID after a reboot are tested on generated data and on small test
  filesystems. They have not been seen on a real server, and no ZFS dataset
  has been tried.
