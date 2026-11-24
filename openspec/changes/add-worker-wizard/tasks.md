# Tasks

## 1. The text

- [x] 1.1 `src/web/axis-worker-attach.c`: a plan (name, tool, encoder, processor, address, disks with their mounts and paths) and its check; the address and a path in one plain form each, or refused with a sentence that quotes nothing typed.
- [x] 1.2 The text for `docker run`, `podman run`, a `compose.yaml` and a Quadlet `.container` file, from the one list of settings; the image's tag is the version given.
- [x] 1.3 The commands that put the token in a file, that start the worker, and that mount a disk (with placeholders for the server, the path or share on it, and the user).
- [x] 1.4 No function of the file is given the token.

## 2. The pages

- [x] 2.1 `GET /workers/attach` and `POST /workers/attach`: the machine. The browser's address is offered when it has the plain form.
- [x] 2.2 `POST /workers/attach/disks`: the disks Axis knows; a disk without a marker cannot be ticked.
- [x] 2.3 `POST /workers/attach/mounts`: the mount commands, and the summary before the worker is created.
- [x] 2.4 `POST /workers/attach/create`: the worker and its token, the text, the commands. A name taken in the meantime shows step 1 again and no token.
- [x] 2.5 `GET /workers/attach/waiting` and `GET /workers/attach/status`: what the worker reported, each ticked disk as proved or not, what to do; asked for again on the workers' event and every 5 seconds until all is good.
- [x] 2.6 Every step carries the choices in hidden fields and has a form that goes back; every route checks the steps before its own again.
- [x] 2.7 `axis_worker_api_set_registered()`: an accepted registration is told on the main loop; `axis_web_workers_note_registered()` publishes `workers-changed` on `workers:admin`.
- [x] 2.8 The Conversion workers page: the card "Attach a worker", and "Add a worker without the guide".

## 3. Tests

- [x] 3.1 `tests/test-worker-attach.c`: the exact text for four plans (one per tool) and for the four kinds of mount; for every tool, encoder and kind of mount (60 texts) no token's shape, the token's file, the hardening options, the image's tag once, the disks and exactly the sshfs and SMB disks without file identities, a GPU only for a GPU; addresses, paths, names and plans that are refused.
- [x] 3.2 `tests/test-web-worker-attach.c`: administrators only and the CSRF check on every POST; the five pages in order, going back, nothing created before step 4, the token once and only its hash kept; each tool's text on the page; what is refused at each step and creates nothing; the last page before, during and after a registration, with the event on the stream.
- [x] 3.3 `tests/test-worker-api.c` still passes with the registration told.

## 4. Examples and documents

- [x] 4.1 `deploy/examples/remote-worker/compose.yaml`; the encoder switch in `run`, `axis-worker.container` and `compose.yaml`.
- [x] 4.2 The example's README: the guide first, the three files, VA-API and the CPU only, and a table of what ran on real hardware.
- [x] 4.3 `tests/check-examples.sh`: the `worker` part (the script's syntax, `compose config` for each encoder, Quadlet's dry run). Written and checked by hand step by step; the part itself was not run here.
- [x] 4.4 `docs/11-run-conversions-on-a-worker.md` around the guide, with "A small ARM board as a worker" and a marked sentence to replace with a measurement.
- [x] 4.5 `deploy/README.md`: the example's row, the worker section, the checks.

## 5. Not done here

- [ ] 5.1 Start a worker on a second machine from the generated text, for each tool.
- [ ] 5.2 Run the VA-API text on a machine with an Intel or AMD GPU, and the Docker texts on a Docker host with an NVIDIA GPU.
- [ ] 5.3 Time a conversion on an ARM board and put the figure in the guide.
- [ ] 5.4 Run `make check-examples worker` (and the whole gate) on the merged tree.
- [ ] 5.5 A screenshot of the guide for `docs/SCREENSHOTS.md`.
- [ ] 5.6 Keep the plan of a worker (its tool, its ticked disks) so that the last page can be opened again from the worker's card without its address.
