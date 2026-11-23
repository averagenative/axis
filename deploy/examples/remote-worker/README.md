<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# A worker on another machine: Axis on a server, conversions on a desktop with a GPU

Axis runs on a server that holds the disks. The conversions (re-encoding a
film to save space) run on a faster desktop with an NVIDIA GPU, so the
server only serves the files. `axis-worker` on the desktop talks to Axis
over https through the reverse proxy in front of it, and reads and writes
the media through a mount of the server's disks.

```
 desktop (GPU)                                  server
 +-----------------------------+   https    +---------------------------+
 | axis-worker (rootless       | ---------> | reverse proxy -> axisd     |
 | Podman, NVENC)              |            |                           |
 |   /media/disk1  <-- mount --+------------+-- /srv/disk1 (the disk)   |
 +-----------------------------+  NFS, sshfs +---------------------------+
                                  or SMB
```

The worker never decides anything about your library. It encodes into the
volume's work area (`.axis-work/` at the top of the disk) and reports the
file. Axis then runs ten checks on that file and, only for a replacement,
puts it in place. By default the server makes every measurement of those
checks; you can have the worker make the heavy ones instead (step 7).
Either way, Axis decides every check itself. The worker creates no file
anywhere else, renames nothing and removes only its own output.

The files here: this README, [`run`](run) (the worker with rootless Podman),
and [`axis-worker.container`](axis-worker.container) (the same as a Quadlet
unit, so that systemd runs it). Every name below (`axis.example.org`,
`server.example.org`, `/mnt/server/disk1`, the user `you`) is an example:
put yours in.

`make check-examples` does not run this example: it needs two machines and
a GPU.

## What you need

- Axis running on the server, behind a reverse proxy with TLS (the
  [Traefik example](../traefik/README.md), or your own), reachable from the
  desktop as `https://axis.example.org`.
- The server's disks mounted into Axis's container read-write, and
  Read-only storage off (Settings, Disks) on the server: conversions
  write the work area, and a replacement writes the library. Axis writes
  nothing until you allow it ([../../README.md](../../README.md), "The one
  layout rule").
- On the desktop: Podman 4.4 or newer (Quadlet: Podman 5), the NVIDIA driver
  and the NVIDIA Container Toolkit, and a way to mount the server's disks
  (below).
- The same Axis version on both: the worker's image tag is the server's.
  A worker of another version is refused and says so ("When something goes
  wrong").

## 1. Add the worker and keep its token

In Axis, open **Conversion workers** (Library), add a worker (for
example `desk`), and copy the token it shows. It is shown once; Axis keeps
only a hash of it.

On the desktop, as the user who will run the worker, put it in a file only
that user can read:

```sh
mkdir -p ~/.config/axis-worker
umask 077
cat > ~/.config/axis-worker/token     # paste the token, Enter, then Ctrl-D
chmod 600 ~/.config/axis-worker/token
```

Never put the token on a command line or in an environment variable: every
user of the machine can read those. The worker reads it from the file
(`AXIS_WORKER_TOKEN_FILE`) and warns if others can read it; `run` refuses to
start then.

## 2. The reverse proxy

The worker sends its token in an `Authorization` header to
`https://axis.example.org/api/v1/worker/...`. Route that path to Axis like
the rest (no login page, no rewriting), and keep the header.

On the server, list the proxy in `AXIS_TRUSTED_PROXIES`: its service name
in Compose (`AXIS_TRUSTED_PROXIES: traefik` in Axis's service), or its
address if it has a fixed one. Axis
then reads the worker's real address from the proxy's `X-Forwarded-For`: the
Conversion workers page shows it, and the throttle on wrong tokens counts per address
instead of putting every client behind the proxy in one bucket
([../../README.md](../../README.md), "Behind a proxy"). The worker refuses to
send its token over plain http to anything but its own machine.

## 3. Mount the server's disks on the desktop

Mount each disk Axis knows, whole, so that its top directory (the one with
Axis's marker, `.axis-volume`, and its work area, `.axis-work/`) is a
directory on the desktop. The worker finds out which disk is which by the
marker, not by the path, so the paths on the desktop are yours to choose;
the worker is told the paths it sees inside its container, and `run` maps
the desktop's mounts onto them (`/mnt/server/disk1` onto `/media/disk1`).

The mount must be **writable**: the worker writes its output into
`.axis-work/`. Files it creates there are created by the user the mount logs
in as on the server, and Axis accepts only an output owned by its own user
(the user its container runs as, 1000 by default) or by the original's
owner, and `.axis-work/` is that user's alone (mode 0700). So the mount must
reach the server as **Axis's user**.

Choose, in this order:

**NFS (best).** NFS shows the desktop the server's own inode numbers and
times to the nanosecond, so the worker checks every file as strictly as on
the server. Export the disk to the desktop, with Axis's user id mapped
(`all_squash,anonuid=1000,anongid=1000` in `/etc/exports` for that client
makes everything the desktop writes Axis's, whatever user writes it), and
mount it:

```sh
sudo mount -t nfs4 -o rw,hard server.example.org:/srv/disk1 /mnt/server/disk1
```

Nothing else is needed: leave `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`
empty.

**sshfs.** Easy where you have SSH already, but it does not keep file
identities: through it a file's inode number is made up (a small number such
as 7) and its modification time has no fraction of a second, and every file
shows one link. Log in as Axis's user on the server, and let other users,
the container among them, see the mount:

```sh
# once, as root: let a user's FUSE mount be seen by others
echo user_allow_other | sudo tee -a /etc/fuse.conf

sshfs axis@server.example.org:/srv/disk1 /mnt/server/disk1 \
    -o allow_other,reconnect,ServerAliveInterval=15,ServerAliveCountMax=3
```

`allow_other` (and `user_allow_other` in `/etc/fuse.conf`) was needed in the
first real run for a rootless container to see the mount at all. List the
disk in `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY` (`run` does by default).

**SMB/CIFS.** Also loses identities: times are kept in tenths of a
microsecond and inode numbers are the server's file index. Mount with the
Samba user that is Axis's user on the server, and list the disk in
`AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`:

```sh
sudo mount -t cifs //server.example.org/disk1 /mnt/server/disk1 \
    -o credentials=/home/you/.config/axis-worker/smb,uid=$(id -u),gid=$(id -g),serverino
```

What the setting changes. For a listed disk the worker checks a film by its
size, its modification time to the second, its link count as the mount
reports it, and samples of its content (eight blocks of 1 MiB read through
the mount, whose SHA-256 must equal what Axis read from its own disk when it
handed out the work); it does not compare the inode number or the time below
a second. Unlisted disks are checked as strictly as ever (the samples too).
Axis's own checks, on the server, do not change: before anything is placed
it compares the original's full stamp, inode and nanoseconds included, and
its real link count, so a film that a download client has hardlinked (one
it seeds) is never replaced, whatever sshfs says about links. The Conversion workers
page shows, per disk, how this worker checks a file there.

Check each mount before you start the worker: `ls -a /mnt/server/disk1`
must show `.axis-volume` (and, once a conversion was requested,
`.axis-work`). On a host with SELinux enforcing (Fedora), a FUSE or NFS mount
is refused to a container unless its label is disabled, which `run` does
(`--security-opt label=disable`).

## 4. The GPU in a container

With Podman, the NVIDIA Container Toolkit describes the GPU as a CDI device.
Generate the description once (again after a driver update) and check that
it names the GPU:

```sh
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
nvidia-ctk cdi list            # nvidia.com/gpu=all among them
```

`run` gives the container `--device nvidia.com/gpu=all`. With Docker
instead: `--gpus all` and `-e NVIDIA_DRIVER_CAPABILITIES=compute,video,utility`
(NVENC needs the `video` capability, which Docker's default leaves out).

The image's ffmpeg (Debian's) has NVENC compiled in. The worker measures
what it can really encode when it starts (a one-second test encode per
encoder, once per ffmpeg build) and reports it; the Conversion workers page shows
`hevc_nvenc` as usable when it works.

## 5. Start the worker

Change the lines marked `EDIT` in [`run`](run), then, as your user (never
root):

```sh
./run
podman logs -f axis-worker
```

Or, to have systemd keep it running, copy
[`axis-worker.container`](axis-worker.container) to
`~/.config/containers/systemd/`, change its `EDIT` lines (a disk mounted by
`/etc/fstab` is waited for; an sshfs mount made by hand is not, see the
unit's comments), and:

```sh
systemctl --user daemon-reload
systemctl --user start axis-worker
loginctl enable-linger        # once: run while you are logged out
```

What the options are for:

| Option | Why |
| --- | --- |
| `--userns=keep-id` (`UserNS=keep-id`) | The container runs as your user, so it can read your token file (mode 600) and see your mounts. Without it the container's user is another id on the host |
| `--security-opt label=disable` | SELinux: a FUSE (sshfs) or NFS mount is refused to the container otherwise |
| `--device nvidia.com/gpu=all` | The GPU (step 4) |
| `AXIS_WORKER_ENCODER_FAMILIES=nvenc` | The GPU only. The built-in profiles prefer the software encoder when a worker offers it (it compresses best), which on a desktop CPU is many times slower; a family left out is never run, not even measured |
| `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY` | The disks mounted with sshfs or SMB, by their path in the container (step 3); empty for NFS |
| `--cpus 4` (`--cpus=4`) | At most four of the desktop's cores. ffmpeg still decodes and muxes on the CPU; the encoder also runs at the lowest CPU priority and idle I/O priority, so the desktop stays usable. If Podman says the cpu controller is not available to your user, use `--cpu-shares 256` or leave it out |
| `--read-only`, `--cap-drop ALL`, `no-new-privileges`, `--pids-limit` | As locked down as Axis; the worker writes only to `/tmp` (its scratch and state) and to the work area |
| `--stop-timeout 75` | A stopped worker gives its job back (up to a minute) before it exits |

The worker keeps its encoder measurement in `/tmp`, so it measures once per
start. One job runs at a time (`AXIS_WORKER_MAX_JOBS`, default 1).

## 6. See it working, then convert

On the **Conversion workers** page your worker shows as Connected, with its
version, `hevc_nvenc` usable (10-bit and HDR10 as measured), and each disk
it proved, with where it sees it and how it checks a file there ("Sees this
volume through a network mount that does not keep file identities: by its
size, its time to the second and samples of its content" for an sshfs
disk). In its log: `registered as desk` and `volume ... proved and
accepted`.

Then try a film. On its file page, "Convert this file", choose a profile and
**Trial: the original stays**: the worker makes the new file, Axis checks it, and keeps it aside
in the work area for 14 days without touching the library. Its page
(Conversions) shows it go from queued to encoding (with how far the encoder
has read the source) to verifying to done, and the ten checks with their
figures. Play the trial on your devices.

A **replacement** (the original deleted after the new file passed every
check) is offered only for a proven profile version: on the Conversions
page you prove a version by hand after three trials of it on three
different films passed every check, and the playback verdict must say
the profile is safe for the film's library folder. Until then
originals are never replaced.

## 7. Choose where Axis checks converted files

Before Axis places a converted file, it runs ten checks on it. Four of them
read the whole file: a full decode, the picture's packet count, the copied
tracks' data, and a picture comparison at five points. On a small server,
these measurements are the heaviest work Axis does: a full decode of a
1080p film keeps every core busy for minutes. You can send them to the
worker.

To choose where they run, follow these steps:

1. In Axis, open **Settings** and find **Where converted files are
   checked** (`convert.checks_where`).
2. Pick one of the following values:

   | Value | Name on the pages | What happens |
   | --- | --- | --- |
   | `server` (default) | On this server | The server makes every measurement. Axis uses no figure from a worker |
   | `worker` | On a worker when one is connected, else on this server | A connected worker that has proved the film's disk makes the heavy measurements. If no such worker is connected, or the worker cannot finish them in three attempts, the server makes them |
   | `worker_only` | Only on a worker: this server does no heavy work | A worker always makes the heavy measurements. If no such worker is connected, the conversion waits and its page says why. If workers cannot finish them in three attempts, the conversion fails and the original stays |

3. Save, and open **Conversion workers**. The page says what workers do in
   the mode you chose. The Conversions page shows the mode as "Converted
   files are checked: ...".

In every mode, the server runs the light checks itself: the file's
identity, the original, the streams, the picture's properties, the saving,
and the gates. The server also applies every threshold to the worker's
figures. The worker sends figures, never a verdict, so the same conditions
hold for a file to be accepted in every mode.

On the worker, the checks decode on the CPU, not the GPU, at the lowest CPU
and I/O priority, within the container's `--cpus` limit. The worker writes
nothing while it checks.

On the server, **Threads for checks run on this server**
(`convert.check_threads`, default 2) bounds how many threads each decoder
and filter of the server's checks uses. Fewer threads take longer and load
the server less. The server runs every check tool at the lowest CPU and I/O
priority.

### How Axis knows the figures are of its files

Before Axis uses a worker's figures, it confirms that they describe the
files on its own disk:

- The converted file's inode, size, and times are the ones that Axis
  recorded when it handed out the checks.
- Samples of the converted file and of the original (eight blocks of 1 MiB
  each) have the SHA-256 that Axis reads itself.
- The whole converted file has the SHA-256 that the worker read, while
  **Confirm a worker's checks on the whole file**
  (`convert.confirm_whole_file`) is on (the default). For this, the server
  reads the whole file once, at the lowest CPU and I/O priority: about a
  minute per 10 GB on a spinning disk.

If any of these differ, Axis places nothing, removes the converted file,
leaves the original as it is, and fails the conversion with "The
measurements made on worker desk are not of this file".

### What you trust a worker with

When you choose `worker` or `worker_only`, you trust your workers with the
figures they measure. A worker that lies about a decode could get a damaged
file accepted, and Axis cannot catch that without decoding the file itself.
A worker that could do this already writes to your disks as Axis's user
through its mount (step 3), so it could damage the library without the
checks. With `server`, Axis uses no figure from a worker.

The worker that converted a film may also check it. If another suitable
worker is connected, Axis offers the checks to that worker first, for two
minutes. A conversion's page shows **Converted on** and **Checked on**.

## Resume conversion work after Axis restarts

When Axis starts, it holds all conversion work until you resume it, so that
a server that stopped under the load of a conversion does not start that
load again by itself. While the work is held, Axis does not encode, check,
or measure samples for conversions, on the server or on a worker. Downloads,
imports, scans, and the placing of a converted file go on. A job that a
worker was already running when Axis restarted keeps running.

To resume conversion work, follow these steps:

1. Open **Conversions**. The page says "Conversion work is paused since the
   daemon started".
2. Check that the server is healthy: its log, its temperatures, and what
   it was doing when it stopped.
3. Click **Resume conversion work**.

The Conversion workers page and the page of each waiting conversion say the same and
have the same button. If you don't want the hold, turn off **Conversion
work waits after a restart** (`convert.wait_after_start`) in **Settings**.

## When something goes wrong

The conversion's page and the worker's log say what happened. The ones you
are likely to meet:

| Message | What it means, what to do |
| --- | --- |
| `source changed: the file is not the one the conversion was requested for (its modification time differs below a second, its size and time to the second are the same; ... list /media/disk1 in AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY)` | The disk is mounted with sshfs or SMB, which lose inode numbers and sub-second times, and is not listed. Add it to `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY` (by its path in the container), start the worker again, and request the conversion again. With NFS this means the file really changed: scan its library and request again |
| `source changed: ... (its size differs)` or `(its modification time differs)` | The film changed after the conversion was requested (replaced by an upgrade, say). Scan the library and request again |
| `source changed: its content is not what the daemon read (samples of it differ)` | The worker sees another file at that path than Axis does: a mount of another copy of the disk, or a file being written. Check the mount |
| `Refused when a worker took it: the file on disk is not the one the conversion was requested for` | Axis found the film changed on its own disk before handing out the work. Scan and request again |
| `the volume ... is not one this worker has proved`, `the directory has no volume marker (is the disk mounted?)` | The mount is not there (an empty directory), or at another path than the worker is told. `ls -a` the mount; check `run`'s `disks` |
| `the volume is mounted read-only on this worker` | Mount it read-write |
| `the volume's work area cannot be opened: No such file or directory` | Axis makes `.axis-work/` on the server when a conversion is requested; the mount must show the disk's top, hidden names included |
| `the output cannot be made in the work area: Permission denied` | The mount does not reach the server as Axis's user (step 3) |
| Check 1 fails: the file is not owned by Axis's user | The same: files created through the mount must be Axis's on the server |
| `the encoder crashed (signal 11, SIGSEGV): ...; this worker takes no more work until it is resumed on the Workers page or restarted` | ffmpeg died of a signal, not of a bad input. The conversion is failed and the worker is **paused**: it takes no more work, so that a GPU or driver in a bad state is not given the next film (a crash on the first real run was followed by the machine locking up on the next job). Other workers carry on. Look at the desktop (its log, `dmesg`, `nvidia-smi`), then press **Resume** on the Conversion workers page, or restart the worker |
| `ffmpeg crashed while it measured the decode (signal 11, SIGSEGV)...; this worker takes no more work until it is resumed on the Workers page or restarted` | The same as an encoder's crash, while the worker checked a converted file. The check job goes back to the queue for another try (three in all) and the conversion keeps waiting. Look at the desktop, then press **Resume** on the Conversion workers page |
| `job 12: its thread does not end; giving the job up`, then `volume ... at /media/disk1: the thread of job 12 does not end there; it is offered no more work here until it is proved again` | A call on the mount does not return: the server or the network went away during the job. The worker gives the job up (it is tried again later), takes no more work on that disk until the disk answers the worker's next check, and carries on with other work. When the blocked call returns, the worker removes what the job left in the work area; if it cannot, the Conversions page lists it as a leftover. Check the mount: `ls -a /mnt/server/disk1` on the desktop must answer at once. With sshfs, `ServerAliveInterval` and `ServerAliveCountMax` (step 3) limit how long sshfs waits for a server that does not answer |
| `the volume path /media/disk1 has not answered in 2000 ms: registering without it (it is not proven); it is proved when it answers` | A call on the mount does not return when the worker starts (a server that is down, for example). The worker registers after 2 seconds with the disks that answered and offers no work on this one. Check the mount: `ls -a /mnt/server/disk1` on the desktop must answer at once. When it answers, the worker proves the disk by itself and registers it, with no restart |
| `the volume path /media/disk1 did not answer within 5000 ms: it is not proven, and no work is offered on it until it answers and is proved`, later `the volume path /media/disk1 answers again` | The same while the worker runs. The other disks are checked as usual. A job that reads the disk is stopped and tried again later. The worker keeps one blocked thread for the disk, however long it hangs |
| `the threads of 2 jobs (12, 15) do not end, more than this worker runs at once (max_jobs = 1): ...; stopping` | More jobs are blocked on mounts than the worker runs at once. The worker gives its other work back and exits with status 1, and its restart policy starts it again. Fix the mount first: mount it again on the desktop, then restart the worker's container, which sees the mounts as they were when it started. A call blocked in the kernel can keep the old process from exiting until the mount answers or is unmounted (`fusermount -uz /mnt/server/disk1` for sshfs) |
| `Waiting for a worker to check it: no connected worker has proved its volume, and this server does no heavy work (Settings, "Where converted files are checked" is "Only on a worker")` | No suitable worker is connected. Start the worker, check that it proved the film's disk, or choose another value for **Where converted files are checked** (step 7) |
| `The measurements made on worker desk are not of this file: ...` | The converted file changed after Axis handed out its checks, or the worker read another file than Axis sees (a mount of another copy of the disk). Nothing was placed and the original is unchanged. Check the mount, then request the conversion again |
| `The checks could not be measured on a worker: ...` | In `worker_only` mode, three attempts on workers ended without figures. The conversion fails and the original stays. The worker's log says why |
| `Waiting: conversion work is paused since the daemon started` | Axis restarted and holds conversion work ("Resume conversion work after Axis restarts") |
| `the daemon speaks worker protocol 3 and this axis-worker 2: ...` (worker log), or the Conversion workers page saying the worker speaks another protocol | The worker and Axis are of different versions. Run the worker's image of Axis's version |
| `the daemon refused this worker's token (was it revoked?)` | The token is wrong or the worker was revoked; add it again |
| A leftover named `.fuse_hidden...` or `.nfs...` on the Conversions page | A mount's own temporary name for a file removed while it was open, left when the mount went away at that moment. Axis lists it and never removes a name that is not its own: remove it on the server once no worker runs |

## What has been run for real

On a desktop with rootless Podman and an NVIDIA GPU, the server's disk
mounted with sshfs: the worker registered over https, measured `hevc_nvenc`
as usable, and proved the volume by its marker through the mount. An encode
through a network mount had not succeeded before the
`add-remote-worker-mounts` change (every source was refused as changed);
after it, it is not yet verified until it is run again for real. The tests
stand a mount in by a copy of the disk with other inode numbers and whole
seconds, and never touch a GPU.

Checks on a worker (step 7) have not run through a real network mount. The
tests run them with a real `axis-worker` on the test's own disk, and play
the worker through the API to send figures of other files, tampered
figures, and a worker that goes away.
