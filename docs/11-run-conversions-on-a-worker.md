# Run conversions on a worker on another machine

This page shows you how to add a conversion worker, check that it works, and
decide where Axis checks the converted files. The worker runs on a faster
machine, such as a desktop with a GPU. It reaches your disks through a
network mount, and Axis stays on the server. Allow about an hour for the
first setup.

The machine-level steps (the mounts, the container options, the GPU and the
reverse proxy) are in
[the remote worker example](../deploy/examples/remote-worker/README.md).
This page does not repeat them. It tells you what to do in Axis and what
you see.

## Before you begin

You need:

- Axis on the server, behind a reverse proxy with TLS, reachable from the
  desktop. See [Run Axis behind a reverse proxy](13-run-behind-a-reverse-proxy.md).
- The server's disks mounted into Axis's container read-write, and
  **Read-only storage** switched off. Each disk must have its marker
  file, because a worker recognizes a disk only by its marker. A disk that
  was added while it was mounted read-only gets its marker the first time
  Axis finds it mounted read-write.
- A desktop with Podman, an NVIDIA driver and the NVIDIA Container Toolkit
  (or another encoder you give the container), and a mount of the server's
  disks: NFS if you can, otherwise sshfs or SMB.
- The same Axis version on both machines. A worker of another version is
  refused, and the **Conversion workers** page says so.

## Steps

1. In Axis, open **Library**, then **Conversion workers**. In **Add
   a worker**, enter a **Name** (lower-case letters, digits and `-`, such
   as `desk`) and select **Add the worker**.
2. Copy the token the page shows. It is shown once. Axis keeps only a hash.
   Put it in a file only the worker's user can read. Never put it on a
   command line.
3. On the desktop, follow
   [the remote worker example](../deploy/examples/remote-worker/README.md)
   to mount the disks, give the container the GPU, and start the worker.
4. Reload **Conversion workers**. Your worker shows a **Connected** badge.
   The page lists:

   - **Encoders this worker can use**: each **Encoder**, whether it is
     **Usable**, and whether it handles 10-bit and **HDR10**. The worker
     measures this when it starts.
   - **Disks this worker can reach**: each **Disk**, **Where the worker
     sees it**, and **How the worker checks a file**. A disk mounted with sshfs or SMB
     says "By its size, time, inode and samples of its content". This is
     expected. NFS keeps file identities, so the worker checks as strictly
     as the server does.

   <!-- screenshot: the Conversion workers page with a connected worker, its encoders and the disks it can reach -->

   If a disk is missing under **Disks this worker can reach**, the worker is
   offered no work on it. The page says "None: this worker is offered no
   work until it proves a disk."

5. If you did not already, select **Resume conversion work** on the
   **Conversions** page. Axis holds conversion work after every start.
6. Run a trial on one film
   ([Prove a conversion profile with trials](09-prove-a-profile-with-trials.md)).
   The conversion's page shows **Converted on** your worker, and **Checked
   on** where the measurements were made.

## Choose where Axis checks converted files

Before Axis places a converted file, it runs ten checks. Several of them
decode the whole file and compare pictures, which is heavy work for a small
server. In **Settings**, in the **Conversions** card, the setting **Where
converted files are checked** has three choices:

| Choice | What happens |
| --- | --- |
| **On this server** (default) | The server makes every measurement, with the number of threads **Threads for checks run on this server** sets (2 by default), at the lowest CPU and I/O priority. |
| **On a worker when one is connected, else on this server** | A connected worker that has proved the file's disk measures. If none is connected, the server measures. |
| **Only on a worker: this server does no heavy work** | Only a worker measures. If none is connected, the conversion waits and says why. |

In every mode, the server decides every check. A worker only sends
figures. Before Axis uses them, it confirms that they describe the files on
its own disk. By default, the setting **Confirm a worker's checks on the
whole file** makes the server read the whole converted file once. Choosing
a worker means you trust it with the figures it measures. A worker that
lied about a decode could get a damaged file accepted. The
[example](../deploy/examples/remote-worker/README.md#7-choose-where-axis-checks-converted-files)
explains this trust in full.

Once every check of a file has passed, Axis keeps that result. If the file
cannot be put in place at once (a scan holds the disk, or the server
restarts), Axis puts it in place later without measuring or checking it
again, as long as it is still the same file. A file that changed in the
meantime is checked from the start.

## What can go wrong

The worker's log and the conversion's page say what happened. The example
has [a table of messages](../deploy/examples/remote-worker/README.md#when-something-goes-wrong).
The ones you meet most:

| What you see | What to do |
| --- | --- |
| **Paused** badge on the worker | Its encoder crashed. It takes no more work, so that a bad GPU or driver is not given the next film. Look at the desktop (its log, `dmesg`, `nvidia-smi`), then select **Resume this worker**. A resume covers that one crash: if the same job crashes again on the worker, the worker is paused again. |
| **Not heard from** badge | The worker has not been heard from recently. Check the machine, the network and the proxy. |
| The worker's log says "the report was answered 503" (or `429`), "trying again until its lease runs out" | A proxy, or a busy Axis, did not take the result. The worker keeps the result and tries again, more slowly each time, until the job's lease runs out; it drops the job only when Axis's worker API itself refuses it. If it happens often, raise the proxy's limits for `/api/v1/worker/`. |
| **Never seen** | The worker has not connected. Check the token file, the address and the proxy. |
| "source changed: ..." on a conversion | The disk is mounted with sshfs or SMB and is not listed as one without file identities. List it in `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`, restart the worker, and request the conversion again. |
| The worker does not list the disk under **Volumes it has proved** | The mount is empty or at another path. `ls -a` the mount on the desktop: it must show `.axis-volume`. |
| "Waiting for a worker to check it: no connected worker has proved its volume" | You chose **Only on a worker: this server does no heavy work** and no worker is ready. Start the worker, or choose another value. |
| A worker speaks another protocol | Run the worker image of the same version as Axis. |
| The worker stops after "the threads of 2 jobs ... do not end" | A mount hangs. Mount it again on the desktop and restart the worker. The jobs it gave up are tried again under new file names, so the files they left in the work area do not block them. Axis removes those leftovers after the time **Remove work-area leftovers after** sets. |

## Verified

- Run for real, through sshfs, on a desktop with rootless Podman and an
  NVIDIA GPU: the worker registered over https, measured `hevc_nvenc` as
  usable, and proved a disk by its marker through the mount.
- An encode through a network mount had not succeeded before the change
  `add-remote-worker-mounts`. After it, trial conversions of real movies,
  encoded by the desktop's GPU through the sshfs mount, passed all ten
  checks.
- Checks on a worker (the two worker choices) are built and tested with
  a real `axis-worker` on generated clips. They have also run on a worker
  through the real sshfs mount, in a trial. Their cost on a small server
  has not been measured.
- The worker survives its server or mount going away. A real crash found
  this, and a stand-in for a dead mount reproduces it in the tests.
- The queue records no real run with NFS or SMB. Only sshfs ran.
