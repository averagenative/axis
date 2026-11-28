# Install Axis beside your existing stack and look at your library

This page shows you how to start Axis next to the programs you already run
(Radarr, Sonarr, Plex, Jellyfin) and look at your library without changing
it. Allow about 20 minutes.

Axis starts in read-only mode. In this mode it scans your disks and reports
what it finds, but it imports nothing and writes nothing to a library. It
makes one exception, which this page explains in step 6.

## Before you begin

You need:

- A Linux machine with Docker and the Compose plugin, or Podman with
  `podman compose`, and `git`.
- Your media disks mounted on that machine. Axis wants each disk mounted
  whole, so that it can recognize the disk later.
- A free TCP port. Axis listens on 7800 by default. Your existing programs
  keep their own ports, and Axis does not need them to stop.

Axis keeps its own PostgreSQL database in its own container. It does not
read or change the databases of Radarr, Sonarr or Plex.

## Steps

1. Choose how you run Axis. For a first look, use the
   [minimal Compose example](../deploy/examples/compose/README.md). The
   steps to edit `compose.yaml`, make the secret files and start the stack
   are in that README and in the
   [quick start](../README.md#quick-start), so this page does not repeat
   them. Two choices matter for this use case:

   - If port 7800 is taken, set `AXIS_PORT` in the `.env` file beside
     `compose.yaml`.
   - Mount each disk whole, and keep the `:ro` at the end of the line. The
     container then cannot write to your disks at all. Step 6 explains what
     this means for the disk's marker file.

2. Start the stack, and check that it is ready:

   ```sh
   docker compose up -d
   curl -s http://127.0.0.1:7800/readyz
   ```

   When Axis is ready, the answer is `{"status":"ready"}`. Until the
   database is up, Axis answers `503` to every page except the probes and
   keeps trying. It does not exit.

3. Find the one-time setup token. Axis prints it in its log at warning
   level:

   ```sh
   docker compose logs axis | grep "setup token"
   ```

   The token is the last word of the line.

4. Open `http://127.0.0.1:7800/setup`. The page is titled **Set up Axis**.
   Enter the **Setup token**, choose a **Username** (letters, digits, `.`,
   `_` and `-`), and enter a **Password (at least 10 characters)** twice.
   Select **Create administrator**.

   <!-- screenshot: the Set up Axis page with the token, username and password fields filled in -->

5. Log in with **Log in**. Axis opens the dashboard. The dashboard has a
   **Still needed before Axis can download** card, which lists what is
   still needed before Axis can find, download and import a movie.

6. Confirm that storage is read-only. Open **System**, then **Settings**,
   and find the **Storage** card. **Read-only storage** is on. While it is
   on, Axis changes nothing on any disk.

   There is one exception. Axis recognizes a disk by a small marker file,
   `.axis-volume`, at its top. The file holds a random identifier, the date
   and a short note, and nothing else. A disk that is mounted read-only has
   no marker yet, and Axis cannot write one. Axis adds the disk anyway and
   recognizes it by what it can read: the filesystem's identifier, its type
   and size, and the directory itself. The first time Axis finds the disk
   mounted read-write, it writes the marker, even while **Read-only
   storage** is on.

7. Add your disks and library folders. Follow
   [Add disks and library folders](02-add-disks-and-library-folders.md).

8. Look at what Axis found. Scans, file reading and identification all work on a
   disk without a marker. Some disks cannot be recognized without a marker
   (an empty directory, a network or FUSE filesystem, and filesystems such
   as xfs and exFAT that report only a device number). For those, mount the
   disk read-write once, add it so that Axis writes the marker, and mount it
   read-only again. See [Your disks](../deploy/README.md#your-disks).

9. Open **Library**:

   - **Files** shows each disk, its folders and its files, with what
     Axis learned by probing each video.
   - **Disk usage** shows where the space goes, by disk, library folder,
     media type, codec and resolution, with likely duplicates and the
     largest files.

   <!-- screenshot: the Disk usage page for a demo library -->

## What the screen shows

Right after you add a disk, **Files** shows a scan in progress, for
example "Scanning: 1,240 files in 96 folders". A large disk takes a while.
The numbers on **Disk usage** fill in after the scan finishes and the file
details have been read.

The Setup guide (**System**, then **Setup guide**) lists eight steps:
**Details from TMDb**, **Disks and library folders**, **Indexers**,
**Download clients**, **What to download**, **Media servers (Plex,
Jellyfin)** (optional), **Letting Axis write** and **Identifying the movies
already on disk** (optional). You do not need any of them to look at your library.

## What can go wrong

| What you see | What to do |
| --- | --- |
| Every page answers `503` | Axis is waiting for its database. Run `docker compose logs postgres`. A PostgreSQL older than 15 stops the start. |
| The log has no "setup token" line | The log level is above warning, or an account exists already. Set `AXIS_LOG_LEVEL` to `warning` or lower and restart (the setting **Log level** in Settings cannot help here, because it needs a login), or create the administrator from a secret file ([deployment notes](../deploy/README.md#the-first-administrator)). |
| "The setup token is not correct. Copy the newest one from the Axis log." | Restarting Axis prints a new token. Use the newest line. |
| "The filesystem is mounted read-only and has no marker yet." followed by a reason, and "Mount it read-write once so that Axis can write its marker; it may be read-only after that." | Axis cannot recognize this kind of disk without a marker. Take `:ro` off its line, restart, add the disk, and then put `:ro` back. |
| A container cannot read your files | The container user must be allowed to read your media. See [User and group](../deploy/README.md#user-and-group). On a host with SELinux enforcing, see [SELinux](../deploy/README.md#selinux). |

## Verified

- The Compose, full-stack, Traefik, Quadlet and `podman run` examples run
  for real under Podman (`make check-examples`). The Kubernetes manifests
  run on kind.
- The tag-triggered image workflow has never run. The first image is
  published when the tag `v0.1.0-rc2` is pushed and its workflow run
  finishes. Until then, you build the image from a checkout.
- Adding a disk that is mounted read-only (the change
  `add-volume-read-only`) is built and tested on generated data. It has not
  been seen on a real server. The example checks add a first disk through
  the web interface in every example they run.
