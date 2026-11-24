# Attach a worker with a guide

## Why

Adding a conversion worker asks a person for a name and shows a token. Everything else is in a guide page and one example script for one kind of machine, a desktop with an NVIDIA GPU and rootless Podman: which image to run, which options, which mounts, how to tell the worker about a disk mounted with sshfs, and how to tell that it worked. A person with Docker, with an Intel GPU, or with no GPU has to translate the script, and a mistake shows up later as a worker that never registers or a disk it is given no work on, with nothing on the pages that says why.

Axis knows most of what that text needs: its own version, the disks, and the address the browser used. It can write the text and then say what the worker reported.

## What changes

- The Conversion workers page has "Attach a worker", a guide of five plain pages, for administrators:
  1. The machine: the worker's name, its container tool (Docker, Docker Compose, Podman, Podman Quadlet), its encoder (an NVIDIA GPU, an Intel or AMD GPU through VA-API, the CPU only), its processor (x86-64 or ARM64, as information: the published image has both), and the address the worker reaches Axis at. The address the browser used is offered as the default. Plain http to another machine is taken only when the person says the network is trusted.
  2. The disks: the disks Axis knows, each to tick, with how it is mounted on that machine (a disk of that machine, NFS, sshfs, SMB) and its path there. A disk without a marker is named and cannot be ticked.
  3. The mount commands for each ticked disk, as text to adapt. The server's address, the path or share on it and the user to log in as are placeholders. Axis prints nothing of its own host.
  4. The worker is created. The page shows its token once, the commands that put it in a file only that user can read, the text for the chosen tool with the name, address, disks, the disks without file identities and the encoder filled in, and the commands that start it.
  5. Waiting for the worker: what it reported (its versions, the encoders it measured as usable, each ticked disk as proved or not), with a plain reason and what to do when something is missing. The page is told when a worker registers and asks again by itself until everything is good. When all is good it links to "Measure samples on a worker" and to planning a run.
- Every step can be gone back to with what was chosen kept. Nothing is kept on the server between the steps: each step posts every choice so far, and each route checks every earlier step again.
- The token is created at step 4 and at no step before. No form field and no address of an earlier step holds it.
- The text comes from one place in C (`src/web/axis-worker-attach.c`), which is never given the token and gives no text for a plan whose name, address or paths do not have their plain form. The text keeps the hardening options of the example: read-only, no capabilities, no new privileges, a process limit, a tmpfs for `/tmp`.
- The worker API tells the pages when a worker's registration is accepted. A topic that only administrators may follow carries an event without content.
- The one-step form stays, as "Add a worker without the guide".
- `deploy/examples/remote-worker/` gains a `compose.yaml` for Docker Compose, and all three files gain a switch for an Intel or AMD GPU and for the CPU only. The README says which were run on real hardware and which were not. `make check-examples` gains a `worker` part for what one machine without a GPU can check.
- `docs/11-run-conversions-on-a-worker.md` is rewritten around the guide, with a section on a small ARM board as a worker.
- No migration. No change to the worker protocol or to `axis-worker`.

## Impact

- Capabilities: `worker-protocol`, `deployment`.
- Code: `src/web/axis-worker-attach.c` (the text), `src/web/axis-web-workers-attach.c` (the pages), `src/web/axis-web-workers.c` and `src/web/axis-web-workers-private.h` (the link, the event), `src/api/axis-worker-api.c` (the registration is told), `src/axisd/main.c` (the two are joined), `data/web/axis.css` (two rules).
- Tests: `tests/test-worker-attach.c`, `tests/test-web-worker-attach.c`, `tests/axis-test-worker.c` (a test daemon with the hub).
- Documents: `docs/11-run-conversions-on-a-worker.md`, `deploy/examples/remote-worker/README.md`, `deploy/README.md`.
- Behavior that differs: the card "Add a worker" is named "Add a worker without the guide" and comes after the new card "Attach a worker". An accepted registration publishes one event to administrators' open pages.
- Not verified: the generated text has not started a worker on a second machine. Docker, Docker Compose, VA-API and a CPU-only worker in a container have not run on real hardware. The Podman text for an NVIDIA GPU follows the example's script, which did.
