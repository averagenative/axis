# Run conversions on a worker on another machine

This page shows you how to attach a conversion worker with the guide in
Axis, check that it works, and decide where Axis checks the converted
files. The worker runs on another machine, such as a desktop with a GPU.
It reaches your disks through a network mount, and Axis stays on the
server. Allow about an hour for the first setup. Most of that time goes
into the mounts.

The guide is five pages on the **Conversion workers** page. You say what
the machine is, and Axis gives you the text that starts the worker there:
a `docker run` or `podman run` command, a `compose.yaml` file, or a Quadlet
`.container` file. The last page shows you when the worker has registered
and what it can do.

[The remote worker example](../deploy/examples/remote-worker/README.md)
explains the same setup line by line, for when you want to do it by hand
or understand what the text does.

## Before you begin

You need:

- Axis on the server, reachable from the worker's machine. Use a reverse
  proxy with TLS: a worker refuses to send its token over plain http to
  another machine unless you tell it that the network is trusted. See
  [Run Axis behind a reverse proxy](13-run-behind-a-reverse-proxy.md).
- The server's disks mounted into Axis's container read-write, and
  **Read-only storage** switched off. Each disk must have its marker
  file, because a worker recognizes a disk only by its marker. A disk that
  was added while it was mounted read-only gets its marker the first time
  Axis finds it mounted read-write. The guide lists such a disk and does
  not let you select it.
- On the worker's machine: Docker (with the Compose plugin if you want a
  `compose.yaml`) or Podman (Podman 5 for Quadlet), and a way to mount the
  server's disks: NFS if you can, otherwise sshfs or SMB.
- For an NVIDIA GPU: the NVIDIA driver and the NVIDIA Container Toolkit on
  that machine. For an Intel or AMD GPU: the GPU's VA-API driver. For the
  CPU only: nothing more.
- An administrator account in Axis. Only administrators see the guide.

You don't choose a version. The text names the image of the Axis version
that generates it, and a worker of another version is refused.

## Attach the worker

1. In Axis, open **Library**, then **Conversion workers**, and select
   **Attach a worker**.

2. On **Step 1 of 5: the machine**, enter the following, and then select
   **Next: the disks**:

   - **Name of the worker**: lower-case letters, digits and `-`, such as
     `desk`. The container on that machine is named after it.
   - **Container tool on that machine**: Docker, Docker Compose, Podman,
     or Podman Quadlet.
   - **Encoder**: an NVIDIA GPU, an Intel or AMD GPU, or the CPU only. The
     worker is told to use that family of encoders only.
   - **Processor**: x86-64 or ARM64. This is for your information. The
     published image has both, and the container tool takes the right one.
   - **Address the worker reaches Axis at**: Axis offers the address your
     browser uses. Change it if the worker reaches Axis another way. If the
     address is plain http to another machine, select the checkbox that
     says the network is trusted, or the worker refuses to start.

   Nothing is created yet.

3. On **Step 2 of 5: the disks**, select **The worker reaches this disk**
   for each disk the worker will reach. For each selected disk, choose
   **How it is mounted on that machine** and enter **Its path on that
   machine**. Then select **Next: mount the disks**.

   | Choice | When to use it | What the worker does |
   | --- | --- | --- |
   | **A disk of that machine (no network mount)** | The worker runs on the machine that holds the disk | Checks every file as strictly as the server |
   | **NFS** | The disk is on the server and you can export it | Checks every file as strictly as the server |
   | **sshfs** or **SMB/CIFS** | NFS is not possible | Checks a file by its size, its time to the second and samples of its content, because these mounts do not keep file identities |

   The path may hold letters, digits and `/ . _ -`. If your path has other
   characters, enter a plain one and change the text by hand afterward.

4. On **Step 3 of 5: mount the disks**, run the commands for each disk on
   the worker's machine. Replace the words in capitals first: `SERVER` is
   the address of the machine that holds the disk, `/PATH/ON/SERVER` is the
   disk's top directory there (`SHARE` for SMB), and `AXIS_USER` is the
   user Axis runs as there. Axis does not know these values and does not
   guess them.

   A disk is ready when the last command shows `.axis-volume`. The mount
   must be writable, and it must reach the server as the user Axis runs
   as, because Axis accepts only converted files that its own user owns.

5. Select **Create the worker and show its token**. Have the worker's
   machine at hand: the next page shows the token once.

   To change something first, select **Back: the disks**. Every page has a
   button that takes you back with your choices kept.

6. On **Step 4 of 5: start the worker**, do the three parts on the worker's
   machine, in order:

   1. Copy the token, and put it in a file that only your user can read.
      The page gives you the commands. Never put the token on a command
      line or in an environment variable. Axis keeps only a hash of the
      token, so it cannot show it again.
   2. Run the command, or save the file under the name the page says. The
      text names the token's file and never the token.
   3. Run the commands that start the worker and show its log.

   Select a block once to mark all of it for copying.

7. Select **Next: wait for the worker**. **Step 5 of 5** updates by itself.
   When the worker registers, the page shows:

   - Its state, its `axis-worker` version and its ffmpeg version.
   - **Encoders it measured as usable**: the worker runs a short test
     encode for each encoder when it starts.
   - **The disks you ticked**: each one **Proved** or **Not proved**, with
     what to do for a disk that is not proved.

   When the worker is connected, can encode, and has proved every disk you
   selected, the page says "The worker is attached" and links to **Measure
   samples on a worker** and **Plan a conversion run**.

   <!-- screenshot: the Conversion workers page with a connected worker, its encoders and the disks it can reach -->

8. If you did not already, select **Resume conversion work** on the
   **Conversions** page. Axis holds conversion work after every start.

9. Run a trial on one film
   ([Prove a conversion profile with trials](09-prove-a-profile-with-trials.md)).
   The conversion's page shows **Converted on** your worker, and **Checked
   on** where the measurements were made.

To add a worker for a setup the guide does not cover, such as Kubernetes or
a configuration file of your own, use **Add a worker without the guide** on
the same page. It asks for a name and shows the token once.

## What the generated text does

Every form of the text starts the same container:

- It runs read-only, without capabilities and without new privileges, with
  a limit on its processes. It writes only to `/tmp` (a tmpfs) and to the
  work area at the top of each disk.
- It runs as your own user, so that it can read the token file and write
  through the mounts as you do.
- It gets the GPU only when you chose one.
- It lists the disks at fixed paths inside the container (`/media/disk-1`
  and so on), and names the disks you mounted with sshfs or SMB in
  `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`.

The Podman command for an NVIDIA GPU matches a setup that ran on real
hardware. The other forms are written from the tools' documentation and
have not run on real hardware yet. See
[What has been run for real](../deploy/examples/remote-worker/README.md#what-has-been-run-for-real).

## A small ARM board as a worker

You can attach a small ARM board, such as a Raspberry Pi 4 or 5, as a
worker. The published image has an ARM64 build, and the guide generates the
same text for it. Choose **CPU only**: Axis has no hardware encoder for
these boards.

Be realistic about what you get:

- Software encoding on such a board is slow. Expect one film to take many
  times longer than on a desktop, and longer again for 4K.
- The board runs at full load for the whole time. Give it cooling and a
  power supply that can hold that load.
- Reading and writing through a network mount on the board's network port
  adds to the time.
- The checks of a converted file decode the whole film, so they are slow on
  the board too. Leave **Where converted files are checked** on **On this
  server** unless your server is weaker than the board.

<!-- MEASUREMENT NEEDED: replace the next sentence with a measured figure. -->
No conversion has been timed on an ARM board for this page yet, so it gives
you no figure.

A board is a reasonable worker for a few small files, or when you don't
mind waiting. For a whole library, use a machine with a GPU.

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

The last page of the guide, the worker's log and the conversion's page say
what happened. The example has
[a table of messages](../deploy/examples/remote-worker/README.md#when-something-goes-wrong).
The ones you meet most:

| What you see | What to do |
| --- | --- |
| Step 5 says "The worker has not registered" | The worker did not reach Axis, or Axis refused its token. Read the worker's log. Check that the address works from the worker's machine, that the token file holds exactly the token step 4 showed, and that a reverse proxy passes `/api/v1/worker/` on without a login page. If the token is lost, revoke the worker and attach another. |
| Step 5 shows a disk as **Not proved** | The worker did not find `.axis-volume` at that disk's path in its container. Run `ls -a` on the directory you mounted the disk at: it must show `.axis-volume`. Then restart the worker. |
| Step 5 says no encoder of the family you chose is usable | The GPU did not reach the container, or its driver is missing. The page says what that GPU needs on the machine and why each encoder failed. Restart the worker afterward: it measures again when it starts. |
| Step 1 says "There is a worker of that name already" | Choose another name. A revoked worker keeps its name. |
| **Paused** badge on the worker | Its encoder crashed. It takes no more work, so that a bad GPU or driver is not given the next film. Look at the worker's machine (its log, `dmesg`, `nvidia-smi`), then select **Resume this worker**. A resume covers that one crash: if the same job crashes again on the worker, the worker is paused again. |
| **Not heard from** badge | The worker has not been heard from recently. Check the machine, the network and the proxy. |
| The worker's log says "the report was answered 503" (or `429`), "trying again until its lease runs out" | A proxy, or a busy Axis, did not take the result. The worker keeps the result and tries again, more slowly each time, until the job's lease runs out; it drops the job only when Axis's worker API itself refuses it. If it happens often, raise the proxy's limits for `/api/v1/worker/`. |
| "source changed: ..." on a conversion | The disk is mounted with sshfs or SMB and is not listed as one without file identities. List it in `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`, restart the worker, and request the conversion again. The guide lists it for you when you choose sshfs or SMB for the disk. |
| "Waiting for a worker to check it: no connected worker has proved its volume" | You chose **Only on a worker: this server does no heavy work** and no worker is ready. Start the worker, or choose another value. |
| A worker speaks another protocol | Run the worker image of the same version as Axis. The guide's text names that image. |
| The worker stops after "the threads of 2 jobs ... do not end" | A mount hangs. Mount it again on the worker's machine and restart the worker. The jobs it gave up are tried again under new file names, so the files they left in the work area do not block them. Axis removes those leftovers after the time **Remove work-area leftovers after** sets. |

## Verified

- The guide's pages are tested with a worker played through the worker
  API: nothing is created before step 4, the token is on that page only,
  and the last page shows a disk as proved only after the worker reports
  it.
- The generated text is tested for every container tool, encoder and kind
  of mount: it never holds a token, it always has the hardening options,
  and it lists exactly the sshfs and SMB disks as the ones without file
  identities. Checked by hand: a Compose front end accepts a generated
  Compose file, and Quadlet's generator accepts a generated Quadlet file
  in a dry run.
- Run for real, through sshfs, on a desktop with rootless Podman and an
  NVIDIA GPU, with the example's script that the Podman text follows: the
  worker registered over https, measured `hevc_nvenc` as usable, and proved
  a disk by its marker through the mount.
- Not run for real: the generated text itself on a second machine, Docker
  and Docker Compose in any form, an Intel or AMD GPU, a CPU-only worker in
  a container, and an ARM board.
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
