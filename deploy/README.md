<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Running Axis in containers

Containers are the primary way to run Axis (`axisd`), its PostgreSQL
database and its workers (`axis-worker`). Pick the example that matches how
you run containers, copy its directory, and change the lines marked `EDIT`.

| Example | For | Files |
| --- | --- | --- |
| [Run commands](examples/run/README.md) | `docker run` / `podman run`, no Compose | commands in the README |
| [Compose, minimal](examples/compose/README.md) | Axis and PostgreSQL (and a worker), media read-only | `compose.yaml` |
| [Compose, full stack](examples/compose-full/README.md) | Axis, PostgreSQL, Prowlarr, SABnzbd and Deluge on one data mount, so imports are renames, not copies | `compose.yaml` |
| [Traefik](examples/traefik/README.md) | TLS and a host name in front of Axis, Compose or Kubernetes; optionally the request portal | `compose.yaml`, `ingressroute.yaml`, `compose.portal.yaml` |
| [Kubernetes](examples/kubernetes/README.md) | Deployment, claims, Service, Ingress, workers with optional GPU, NetworkPolicy; optionally the request portal | `kustomization.yaml` and nine manifests |
| [Podman Quadlet](examples/quadlet/README.md) | Containers as systemd services; optionally the request portal | five unit files, and two for the portal |
| [A worker on another machine](examples/remote-worker/README.md) | Axis on a server, the conversions on another machine (an NVIDIA GPU, an Intel or AMD GPU, or the CPU only): the worker over https, the disks through NFS, sshfs or SMB | `run`, `axis-worker.container`, `compose.yaml` |

Every example keeps the same rules: Axis never runs as root, its root
filesystem is read-only, it has no Linux capabilities, secrets come from
files, and the database is reachable from Axis only.

The rest of this page is what the examples share.

## The image

`ghcr.io/averagenative/axis` holds `axisd` (the entry point),
`axis-worker`, `axis-portal`, `ffmpeg` and `ffprobe` on Debian stable, for
`linux/amd64` and `linux/arm64`. `axis-portal` is in images built after
0.1.0-rc2; see [The request portal](#the-request-portal).

### Published images

Images are built and pushed by the GitHub Actions workflow
`.github/workflows/images.yml`, when a version tag is pushed and once a
night. A release
candidate (a tag with a suffix, such as `v0.1.0-rc2`) and a final release
(such as `v0.1.2`) get different tags:

| Tag | Published by | Moves? | Use it when |
| --- | --- | --- | --- |
| `0.1.0-rc2` | a release candidate | never | you want exactly this release candidate |
| `rc` | a release candidate | to each new release candidate | you want to try what the next release will be |
| `0.1.2` | a final release | never | you want exactly this build (recommended for a server) |
| `0.1` | a final release | to each new `0.1.x` | you want fixes without changes in behaviour |
| `latest` | a final release | to every final release | trying things out |
| `nightly` | the nightly build of the main branch | every night that main has a new commit | trying a change before it is released; never for a server you rely on |

A release candidate never moves `0.1` or `latest`. A workflow run that
someone starts by hand at a commit without a version tag publishes
`sha-<commit>` only.

The first published image is from the tag `v0.1.0-rc2` (`v0.1.0-rc1` has no
image: its build failed). It exists as `0.1.0-rc2` and `rc`, and the examples
name `0.1.0-rc2`. The tags `0.1.0`, `0.1` and `latest` don't exist until the first
final release. Before the run finishes, or to run your own build, build the
image where you run it, or for both architectures on machines of your own
([below](#building-the-images-yourself)).

Each image's labels name the commit it was built from
(`org.opencontainers.image.revision`), its version and its source; `axisd
--version` in the image prints the version and commit too, and for a release
candidate the version includes the suffix (`axisd 0.1.0-rc2`). Images are
signed with cosign, keyless, by the workflow's identity:

```sh
cosign verify ghcr.io/averagenative/axis:0.1.0-rc2 \
    --certificate-identity-regexp '^https://github.com/averagenative/axis/' \
    --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

### Building the images yourself

You need Podman or Docker, `git`, GNU Make and the `deps/htmx-glib`
submodule (`git submodule update --init`). You don't need a compiler or the
C development packages on the host: the image is compiled inside a
container. Everything below runs on your own machines; none of it needs CI.

**This machine's architecture.** From the repository root:

```sh
make container
```

This builds `deploy/Containerfile` and tags the result
`localhost/axis:<version>` and `localhost/axis:latest`. Point an example at it
with `AXIS_IMAGE=localhost/axis:latest` (Compose) or by changing its `image`
line. To build a release candidate, add its suffix: `make container
VERSION_SUFFIX=-rc2` builds `localhost/axis:0.1.0-rc2`, and the programs in
it report `0.1.0-rc2`.

The footer of every page in the web interface has a **Source** link, and the
image has an `org.opencontainers.image.source` label. Both name the address
in `SOURCE_URL` (`config.mk`), which is this repository by default. If you
change Axis and let other people use your build over a network, the licence
(AGPL, section 13) asks you to offer them your changed source: publish it,
and build with its address, for example
`make container SOURCE_URL=https://git.example.org/you/axis`.

**arm64, on an arm64 machine.** Give the SSH destination of an arm64 Linux
machine (a `Host` in `~/.ssh/config`, or `user@host`), such as a Raspberry Pi
with a 64-bit system, where `ssh` works without a password prompt and your
user runs `docker` or `podman` without `sudo`:

```sh
make container-arm64 ARM_BUILDER=mypi
```

`deploy/images.sh` writes the build context `make container` uses (this
directory less `.containerignore`, the submodule included; the runtime
itself applies the ignore file) as a tar, sends it and the Containerfile to
`~/axis-build` on the builder, and builds there natively: the build stage
compiles and runs the unit tests on arm64. It then copies the image back
(`save` there, `load` here) as `localhost/axis:<version>-arm64`, and removes
`~/axis-build` and the image from the builder (`ARM_BUILDER_KEEP=1` leaves
the image there, to run it on that machine). The builder's own build cache
keeps the base image and the package layers, so a second build of the same
sources is quick; `docker builder prune` there clears it. Nothing is
installed or configured on the builder. On a Raspberry Pi with 4 cores and
8 GB, a first full build took 18 minutes (nearly all of it compiling, one
file at a time; the unit tests took under half a minute), and copying the
552 MB image back under a minute. `ARM_BUILDER_DIR`
and `ARM_BUILDER_IMAGE` change the work directory and the image's name
there. Without an arm64 machine, `ARM_EMULATE=1` builds it here under QEMU
instead, when qemu-user-static is registered for aarch64: expect it to take
many times longer, since the whole compile and test stage is emulated (this
way has not been run to the end).

**Both, as one multi-architecture image.** On an amd64 machine with Podman:

```sh
make container-multiarch ARM_BUILDER=mypi
```

builds `localhost/axis:<version>-amd64` here and the arm64 image as above,
and joins them in the manifest list `localhost/axis-multiarch:<version>`.
Nothing is pushed unless you ask:

```sh
podman login ghcr.io
make container-multiarch ARM_BUILDER=mypi PUSH=1 REGISTRY=ghcr.io/you/axis
```

The login is yours, done beforehand; nothing in the repository holds a
secret. A push needs a committed working tree, and uses the workflow's tag
policy: at a commit tagged `vX.Y.Z` (which must be `config.mk`'s version)
`X.Y.Z`, `X.Y` and `latest`; at a commit tagged as a release candidate, such
as `v0.1.0-rc2` (build it with `VERSION_SUFFIX=-rc2`), `0.1.0-rc2` and `rc`;
at any other commit `sha-<commit>` only (`bash deploy/images.sh tags` prints
them). Images pushed this way are not signed; the workflow's are.

**The GitHub workflow, if you want it.** `.github/workflows/images.yml`
builds both architectures with Buildx, pushes them to ghcr.io and signs them
with cosign. It runs only when a version tag is pushed or when someone starts
it by hand from the Actions tab, never on a branch push or a pull request.
On a public repository GitHub's runners cost nothing; on a private one each
run uses the account's included Actions minutes, and is billed beyond them.
Each architecture is built and tested in a job of its own, on a runner of
that architecture, so nothing is emulated. The two do not wait for each
other: when one fails, the image is published with the other alone and the
run is marked failed, and **Re-run failed jobs** on that run adds the missing
one. Started by hand, the workflow can build one architecture only.

Without `make`:

```sh
podman build -f deploy/Containerfile --build-arg GIT_SHA=$(git rev-parse --short HEAD) \
    -t localhost/axis:latest .
```

The build has two stages. The first installs a compiler and the `-dev`
packages, builds the hardened release (`make release`), runs the unit tests
that need no database, and installs into a staging directory. The second
installs only run-time libraries, `ffmpeg`, GIO's TLS backend
(`glib-networking`) and the system trust store (`ca-certificates`), and
copies the binaries in: no compiler and no `-dev` package. It also holds
`yt-dlp`, the program the YouTube downloader runs, with `python3` and
`nodejs` from Debian for it. A stage of its own fetches the newest yt-dlp
release when the image is built, over https from yt-dlp's release location on
GitHub, checks the file against the checksum list published with it, and
records its version in `/usr/share/axis/yt-dlp.version`; the build prints the
version. So a build needs that location as well as the base image and
Debian's archive, and two builds of one commit can hold two yt-dlp versions.
A builder keeps a layer for as long as its command and arguments stay the
same, so `make container`, `deploy/images.sh` and the workflow pass today's
date (UTC) as `--build-arg YTDLP_REFRESH=<date>`: the stage runs again on a
new day, and uses its cached layer within one day. A build by hand without
that argument can reuse an old layer; pass it, or `--no-cache`. The two
architectures of one release can differ by a yt-dlp release when they are
built on different days, or on one day that yt-dlp publishes a release (the
arm64 image on the Raspberry Pi and the amd64 image here are two builds). To
build with one release everywhere, pin it: `make container
YTDLP_VERSION=<tag>` or `make container-multiarch YTDLP_VERSION=<tag>` (a
tag is a date, as yt-dlp names its releases, such as `2025.01.15`), or
`--build-arg YTDLP_VERSION=<tag>` by hand; the workflow has no such input and
always takes the newest. `podman run --rm --entrypoint cat <image>
/usr/share/axis/yt-dlp.version` prints what an image holds. Axis never
updates yt-dlp by itself: a newer one comes with a newer image, or from the
setting "yt-dlp program" pointing at a copy you mount. `make
container-debug` writes the split-off symbols (`axisd.debug`,
`axis-worker.debug`, `axis-portal.debug`) to `build/container-debug/` for
reading crash reports (see "When a program crashes").

`Containerfile` names Debian stable (slim) by digest, with the date the
digest was read: the digest of the image index, which holds every
architecture of one Debian build, so each build takes its own from it. To
move to a newer base on purpose, pull the tag, read its digest with `podman
image inspect --format '{{.Digest}}' docker.io/library/debian:stable-slim`,
check that `podman manifest inspect docker.io/library/debian@sha256:<it>`
lists both `linux/amd64` and `linux/arm64` (one architecture's image cannot
build the other), paste it in, change the date, and run `make
check-container`.

The image's `ffmpeg` (Debian's) has the NVENC, VA-API, Quick Sync, Vulkan and
V4L2 encoders compiled in; using one needs the device given to the
container (the worker examples show how, commented out). `make
check-container` prints the list.

## Configuration

Axis reads its bootstrap settings from the environment (or a file given with
`--config`, see `data/axis.conf.example`); everything else is set in the web
interface and kept in the database.

| Variable | Default | What it is |
| --- | --- | --- |
| `AXIS_DATABASE_URL` / `_FILE` | none (required) | The PostgreSQL URL, password included. Use the `_FILE` form. |
| `AXIS_DATABASE_WAIT` | `forever` | How long start-up keeps trying to reach the database (seconds; `0` tries once) |
| `AXIS_ADMIN_USERNAME` | none | The first administrator, created while no account exists |
| `AXIS_ADMIN_PASSWORD` / `_FILE` | none | Its password (at least 10 characters). Use the `_FILE` form. |
| `AXIS_TRUSTED_PROXIES` | empty | Reverse proxies (addresses, CIDR ranges, host names) whose forwarded headers are believed ([Behind a proxy](#behind-a-proxy)) |
| `AXIS_SECURE_COOKIES` | `auto` | `always` marks cookies Secure whatever the request says |
| `AXIS_METRICS_TOKEN` / `_FILE` | none | Turns on `/metrics` ([Metrics](#metrics)) |
| `AXIS_LOG_LEVEL` | `info` | `debug`, `info`, `warning` or `error` |
| `AXIS_LOG_FORMAT` | `text` | `json`: one JSON object per line ([Logs](#logs)) |
| `AXIS_LISTEN_HOST`, `AXIS_LISTEN_PORT` | `0.0.0.0`, `7800` in the image | Where it listens |
| `AXIS_DATA_DIR` | `/var/lib/axis` | Its data directory ([below](#the-data-directory)) |
| `AXIS_OUTBOUND_BIND` | none | The interface (`tun0`, `wg0`) or local address Axis's outbound connections are tied to ([VPN](#sending-axiss-traffic-through-a-vpn)) |
| `AXIS_OUTBOUND_BIND_SCOPE` | `internet` | `all` ties the download clients' and media servers' connections too |
| `AXIS_HEAD_TIMEOUT` | `30` | Seconds a client has to send a request's whole head ([Request limits](#request-limits)) |
| `AXIS_BODY_TIMEOUT` | `60` | Seconds a client then has to send the request's body |
| `AXIS_IDLE_TIMEOUT` | `60` | Seconds a kept-alive connection may wait for its next request |
| `AXIS_MAX_CONNECTIONS` | `512` | Connections open at once, in all |
| `AXIS_MAX_CONNECTIONS_PER_ADDRESS` | `64` | Connections from one address (behind a trusted proxy: requests in flight from one client) |
| `AXIS_DATABASE_POOL_SIZE`, `AXIS_JOBS_CONCURRENCY`, `AXIS_JOBS_GRACE_PERIOD`, `AXIS_WORKER_LEASE` | | See `data/axis.conf.example` |

### Secrets from files

Each secret (`AXIS_DATABASE_URL`, `AXIS_ADMIN_PASSWORD`,
`AXIS_METRICS_TOKEN`) can be given as a file instead: set the variable with
`_FILE` added to the file's path. That is what Docker and Podman secrets and
Kubernetes Secrets mounted as files are for: the value is then not in the
environment, so not in `docker inspect`, `podman inspect` or `kubectl
describe`. The file holds the value; white space around it (the newline an
editor adds) is removed. Setting both forms of one secret stops the daemon
at start, naming both. No message ever contains a secret or what a file
holds.

### The first administrator

With no account in the database, Axis needs a first administrator. Two ways:

- **The setup token.** Axis prints a one-time token in its log at warning
  level; open `/setup`, enter it, and create the account. Keep
  `AXIS_LOG_LEVEL` at `warning` or below until then.
- **From the configuration.** Set `AXIS_ADMIN_USERNAME` and
  `AXIS_ADMIN_PASSWORD_FILE` before the first start. Axis creates that
  administrator (same rules as the setup page: a username of letters,
  digits, `.`, `_` and `-`; a password of at least 10 characters) and prints
  no token. A username or password that breaks the rules stops the start,
  saying which rule; the password is never logged. Once any account exists,
  the two settings are not used (Axis says so in its log), so you can remove
  them.

## Start-up and probes

Axis listens as soon as it starts, then connects to its database, retrying
with back-off (1, 2, 4 ... up to 30 seconds between tries, logging each
failure) for as long as `AXIS_DATABASE_WAIT` allows: forever by default, so
the start order does not matter. Until the database is connected, held by
this daemon ([One daemon per database](#one-daemon-per-database)) and
migrated, and every module has started, it answers only the probes and `503`
for everything else.

| Path | Answers 200 when | Use it for |
| --- | --- | --- |
| `/livez` | the process is serving (always, once listening) | Kubernetes liveness and startup probes |
| `/readyz` | this daemon holds the database lock, the database answers, and its schema is the version this build expects | Kubernetes readiness probe |
| `/healthz` | the database answers | `axisd --healthcheck`, the Compose and Quadlet health checks |

None of them needs a session, and their answers say only `alive`, `ready`,
`not ready`, `ok`, `degraded`, `starting`, `held by another daemon` or
`database lock lost`. `/healthz` also says the version and the database's
state (`"database":"ok"` or `"unreachable"`) to a request that has a session
or carries the metrics token as a bearer token; a wrong token counts against
the address as it does at `/metrics`. To a client on the same machine (a
loopback address), a probe's answer also names the process that answered,
in an `X-Axis-Process` header, so that a health check or a test can tell
its own daemon from another one on a reused port. A URL that cannot be read at all, or a
PostgreSQL older than 15, stops the start at once: no amount of waiting fixes
those.

The probes cannot be used to tire the database. Each database question
(the ping of `/healthz`, the schema check of `/readyz`) is asked by one
thread at a time; every probe that arrives meanwhile waits for that answer,
and the answer is reused for one second. However many probes arrive, they
hold at most two of the pool's connections.

### Request limits

These limits hold from the moment Axis listens, in every state: while it
waits for its database, while it stands by for another daemon's lock, while
it serves, and while it takes its lock back.

- **Request bodies.** At most 1 MiB (a worker's request, 256 KiB). A body
  that is too long, or of unknown length, is answered `413` and the
  connection is closed. Nothing of it is kept: libsoup reads it to its end
  and drops it as it arrives, within the body deadline. Until Axis serves,
  and while it has lost its lock, nothing takes a body: a request with one
  is answered at once with what the start-up page says (`503`,
  `Retry-After`), and no byte of the body is read.
- **Deadlines.** A new connection has `AXIS_HEAD_TIMEOUT` (30 s) to send its
  request's whole head. A request with a body then has `AXIS_BODY_TIMEOUT`
  (60 s) for the body. After a response, a kept-alive connection has
  `AXIS_IDLE_TIMEOUT` (60 s) until its next request starts, and that request
  has the head deadline again. A connection that misses a deadline is
  closed. An event stream (a page that updates live) is not cut by these
  once its head has arrived; the hub's own limits hold it (16 streams per
  account, 8 per session, 256 in all of which 64 are kept for
  administrators, and a client that stops reading is dropped). A remote
  worker's connection is not closed for being idle.
- **Connections.** At most `AXIS_MAX_CONNECTIONS` (512) at once, and at most
  `AXIS_MAX_CONNECTIONS_PER_ADDRESS` (64) from one public address. An IPv6
  address counts by its `/64`, which is what one subscriber is usually
  given. A loopback, private (`10.0.0.0/8`, `172.16.0.0/12`,
  `192.168.0.0/16`, `fc00::/7`) or link-local address is held to the total
  only, because that is where reverse proxies and container gateways
  connect from.
- **Over a cap.** A connection over a cap is let in, up to 32 at once, with
  two seconds to send its head. It is then answered `503` with
  `Retry-After`, except a probe (`/livez`, `/readyz`, `/healthz`) from a
  loopback or private address, which is served however full Axis is, so
  that the platform's health checks do not restart a busy daemon. Beyond
  those 32, a connection is closed as soon as Axis sees it. The log says
  what was refused (once a minute at most), and so do the Settings and
  Reverse proxies pages, with what to do.
- **Behind a trusted proxy.** The proxy's address is not held to the
  per-address cap, since every client comes through it. Instead each client
  it forwards (by `X-Forwarded-For`, under the same rule as the login
  throttle) may have at most `AXIS_MAX_CONNECTIONS_PER_ADDRESS` requests in
  flight at once; more are answered `429` with `Retry-After`. Event streams
  are not counted there (the hub's limits hold them), and neither is a
  request that carries a worker's valid token.

Configure the trusted proxies even though a private proxy is not held to
the per-address cap. Without them every client shares the proxy's address:
the login throttle counts every visitor's wrong passwords together, the
cap on requests in flight per visitor does not apply, and one visitor can
use connections up to the total.

What Axis cannot do from inside, and what to have the reverse proxy do
instead when Axis is reachable from the internet:

- Axis sees a connection only once it has accepted it, so a connection over
  a cap still costs an accept and a close, and the kernel's queue of
  connections not yet accepted is not Axis's to limit. Limit the rate of new
  connections per client at the proxy (Traefik's `inFlightReq` and
  `rateLimit` middlewares, nginx's `limit_conn` and `limit_req`).
- Nothing times how fast a client reads a response, so a client that reads
  slowly holds its connection until the response is written. Set the
  proxy's response timeouts (nginx's `send_timeout`, Traefik's
  `respondingTimeouts.writeTimeout`), leaving event streams out of them or
  giving them hours.
- How long a page takes to answer is not limited (a page waiting for the
  database waits). Set the proxy's upstream read timeout above the slowest
  page you expect.
- Behind a proxy that is not in `AXIS_TRUSTED_PROXIES`, every client has the
  proxy's address, so they share one login throttle. When every request
  with `X-Forwarded-For` comes from one private address and no proxy is
  trusted, Axis says so once in its log and on the Settings and Reverse
  proxies pages: add the proxy to `AXIS_TRUSTED_PROXIES`.

`SIGTERM` stops Axis cleanly whenever it comes: while it waits for the
database, at once; while it serves, after giving running jobs
`AXIS_JOBS_GRACE_PERIOD` (30 s) to finish and handing the rest back. Give the
container a stop timeout above that (the examples use 45 s).

## One daemon per database

Only one Axis daemon uses a database at a time. After `axisd` connects, and before it applies any migration, it takes a lock in PostgreSQL (a session advisory lock on a connection of its own) and holds it until it stops. Another `axisd` that you point at the same database finds the lock taken and waits as a standby.

A standby does the following:

- It answers `/livez` with `{"status":"alive"}`, and `/readyz` and `/healthz` with `503` and `{"status":"held by another daemon"}`.
- It answers every other path with `503` and "Axis is waiting: another Axis daemon is using this database."
- It runs no job, applies no migration, and changes nothing.
- It logs a warning that names the daemon that holds the database, again when that changes, and every 10 minutes. The name gives that daemon's instance (new at each start), version, process ID, and host:

  ```
  the database is in use by another Axis daemon: session 4242 (axisd 1f0c9a3b5d7e2468 0.1.0 pid 1 on media-box) from 10.0.2.15, connected since 2026-11-17 09:12:44 UTC. This daemon serves only the probes and runs no job until that one stops, then takes over; ...
  ```

- It tries again every 2 seconds and takes over when the other daemon stops, within `AXIS_DATABASE_WAIT`. With the default, `forever`, it waits as long as it takes. With `0`, it exits at once with status 1 and names the daemon that holds the database.

The following list says how the lock meets the usual ways of running Axis:

- **Compose.** `docker compose up -d` with a new image and `docker compose restart` stop the old container before they start the new one, so the two never overlap. If you start a second container by mistake (`docker compose up --scale axis=2`, or the quick start's `docker run` beside the stack), it waits and says why in its log. Stop it.
- **Kubernetes.** The example runs one replica with `strategy: Recreate`: Kubernetes deletes the old pod before it creates the new one, and the new pod takes the lock when the old process has ended. Keep that strategy. With `RollingUpdate`, the new pod waits for the old one and never becomes ready, so the rollout never finishes, while the old pod goes on serving. More replicas give you standbys, not more capacity.
- **A host that vanishes.** If the host of the daemon that holds the lock loses power or its network, PostgreSQL ends that daemon's session after about a minute without an answer, and a waiting daemon then takes over. Axis asks the server for TCP keep-alives and a TCP user timeout on the lock's connection, so the server notices whether the connection was idle or had data in flight. By then, the cut-off daemon has already stopped its jobs.

### If the daemon loses its lock

The lock lasts as long as its connection. Axis checks the lock every 5 seconds. A slow answer alone doesn't count as a loss: when the lock's connection doesn't answer within 5 seconds, Axis asks the server on another connection, and goes on working if the server confirms the lock. Axis counts the lock as lost in any of the following cases:

- The connection ends, for example because PostgreSQL restarts or someone ends the session.
- The server says that the lock is no longer held.
- The server hasn't confirmed the lock for 20 seconds, on any connection.

Another daemon may then take the database, so Axis stops working at once:

1. Every path except the probes and `/metrics` answers `503` with "Axis has lost its hold on its database and is taking it back." `/readyz` answers `{"status":"database lock lost"}`.
2. Axis claims no job, renews no lease, fires no schedule, and tells every running job to stop. A job that stops is handed back, and the stop does not count as one of its attempts.
3. If a job has not stopped after 25 seconds, Axis ends its own process with status 3, so that the job can never run beside a daemon that takes over. Your container runtime starts Axis again.
4. Axis takes the lock again, trying every few seconds. When it holds the lock again, it goes on without a restart. If another daemon took the lock meanwhile, Axis stops with status 1, and your container runtime starts it again as a standby.

After a PostgreSQL restart, Axis is usually back within seconds, and the jobs that were running start again. If you end the lock's session by hand (`pg_terminate_backend()`), the running jobs are interrupted in the same way.

A daemon that takes over waits 50 seconds after it starts before it takes back the jobs that an earlier daemon left running. That is longer than the 45 seconds that a daemon can take to notice a loss and stop its jobs.

A daemon frozen for longer than these bounds (a paused container or a suspended virtual machine) can go on with a step that it had started until it notices the loss. Job leases have the same limit.

### Connect directly to PostgreSQL

The lock needs one server session for the daemon's whole life, and so do the job notifications (`LISTEN`). Point `AXIS_DATABASE_URL` at PostgreSQL itself, or at a pooler in session mode (PgBouncer's `pool_mode = session`). A pooler in transaction or statement mode isn't supported: it hands each statement to whichever server connection is free, so the lock stays on a connection that other clients then use. When the check that Axis makes every 5 seconds finds another server session behind the lock's connection, Axis counts the lock as lost. The check can't detect every pooler in transaction mode, so don't rely on it.

## User and group

The image runs as 1000:1000 by default and works as any numeric user and
group, with no root phase and no chown at start:

- **Compose and run commands:** `user: "${PUID}:${PGID}"` (or `--user`).
  Pick the ids that own your media, so Axis can read them (and, in the full
  stack, write the library as the download clients do).
- **An arbitrary user id with group 0** (OpenShift): the image's
  `/var/lib/axis` belongs to group 0 with mode 0770, and a new named volume
  copies that, so the daemon can use it.
- **Kubernetes `fsGroup`:** the volume is given to that group, and Axis
  accepts it.

The rule Axis applies to its data directory: it must belong to the daemon's
user, or to one of its groups with write permission for that group, and must
not be writable by everyone. A volume made earlier for another user and
group is refused, naming the directory and its owner; give it to the new
ones once:

```sh
docker run --rm --user 0 -v PROJECT_axis-data:/var/lib/axis \
    --entrypoint /bin/chown localhost/axis:latest -R 1500:1500 /var/lib/axis
```

(`podman run` takes the same options.) The secret files must be readable by
the user too. With rootless Podman the ids are inside a user namespace, so a
host file shows up as owned by `root` unless you use `--userns keep-id`.

### The data directory

Axis keeps the artwork of movies and TV series (which it serves to your
browser itself) in its data directory, `/var/lib/axis`, and the key that
signs remembered browsers with the record of them (`device-key` and
`devices`, mode 0600). The examples keep it in a named volume, so the root
filesystem stays read-only and the files survive a restart. Everything in
it can be fetched or made again, so it needs no backup: without those two
files, Axis makes a new key and every browser logs in once as a stranger
before it is remembered again. Its size
is capped (Settings, Movie and TV details, "Artwork kept, at most"). Axis creates the
directory if it is missing (its parent must exist).

## Your disks

Mount each disk whole, at a path of its own under `/media`, and read-only
(`:ro`) to start with. Then, in Axis, open **Disks and library folders**, enter the disk's path
inside the container (for example `/media/disk1`), and select **Look at this
directory**. Axis shows what is there and what adding it does. Nothing is
written until you confirm.

Axis recognises each disk by a small marker file, `.axis-volume`, at its
top, not by the path it is mounted at. So a disk that is not mounted shows
as offline, never as empty, and another disk mounted at the same path is
never taken for it.

A disk that is mounted read-only has no marker yet, and Axis cannot write
one. Axis adds it anyway and recognises it by what it can read without
writing: the filesystem's identifier, its UUID where the kernel gives one,
its type and size, and the directory itself. If any of these is different at
a check, or the directory is empty, Axis shows the disk as offline instead
of guessing. You can scan and browse the disk, but Axis writes nothing to it,
and no conversion worker is given work on it, because a worker recognises a
disk only by its marker.

The first time Axis finds the disk mounted read-write (you take `:ro` off and
start the container again), it writes the marker, even while
**Read-only storage** is on. That marker is the only file Axis writes while
**Read-only storage** is on. From then on Axis recognises the disk by the
marker, and you can make the disk read-only again.

Some disks cannot be recognised without a marker, and the page that shows
the disk says so. For these, mount the disk read-write once, add it so that
Axis writes the marker, and mount it read-only again:

- An empty directory. It looks like a disk that is not mounted.
- A network, FUSE or layered filesystem: NFS, SMB, sshfs, mergerfs, ntfs-3g,
  and the file sharing of Docker Desktop on macOS and Windows.
- A filesystem that reports only the number of the device it is on: xfs,
  exFAT, FAT and f2fs, unless the host's kernel gives the filesystem's UUID
  (Linux 6.10 and later do for some filesystems). A device number changes
  when disks are found in another order.

ext4, btrfs, NTFS (the in-kernel `ntfs3` driver), ZFS and JFS report an
identifier of their own and can be added read-only. Two disks that are
byte-for-byte copies of each other look the same to Axis, with or without a
marker. If a disk without a marker changes (it is resized or reformatted, or
its UUID is changed), Axis no longer recognises it: remove the disk and
add it again.

## The one layout rule

For imports to be renames (or, for a torrent left seeding, hardlinks) rather
than copies, downloads and library must be on one disk, reached through one
mount, at the same path in every container that touches them
(`/data/downloads` and `/data/media` under one `/data`). Separate mounts of
the same disk make every import a full copy (with the default, the
download's copy is deleted afterwards, so the space is used twice only while
it is copied).
[The full-stack example](examples/compose-full/README.md#the-one-layout-rule)
shows the layout and the common wrong one; the Kubernetes example uses one
claim the same way.

## Download folders and a cleanup job of your own

By default an import moves the files it takes out of the download folder
(Settings, Imports, "Files taken from a download": "Move into the library"; "Always hardlink" keeps
the old way). A torrent's client is asked to let go of the torrent first,
without its files; a torrent left seeding is hardlinked. Then Axis deletes
what the import did not take with the download's folder, but only inside the
client's folder for Axis (SABnzbd's category folder, Deluge's label "Move
completed to" folder, qBittorrent's category save path), and only by strict
rules. Whatever is still in such a folder was not imported; Downloads,
Leftovers in the download folders lists it, and deletes an entry only when you confirm.

So a folder of Axis's own is safe to clean by age with a job of your own (for
example a cron job that deletes every folder in the client's completed folder
older than a day). Axis expects it: a download deleted before its import ends
as "files gone" and its movie or episode is searched for again; a folder
deleted after its import is recorded as gone; a seeding torrent's files
deleted are said to be gone on the import's page, and the library's hardlink
is unaffected. Give Axis's category or label a folder of its own at the
client, so that its downloads are not mixed with other managers'.

Axis writes nothing to your media until you allow it: the minimal examples
mount media read-only, and the **Read-only storage** setting in Axis
(Settings, Storage) is a second lock. Both must allow a write for one to
happen. The one exception is the marker file at the top of a disk that is
mounted read-write ([Your disks](#your-disks)). Before making a disk
writable: anything running as the container's user can then change every
file on it that user may change, so do one disk at a time and have a backup.

## Behind a proxy

Behind a reverse proxy every request comes from the proxy's address over
plain HTTP, so Axis reads the client's address, the scheme and the public
host from `X-Forwarded-For`, `X-Forwarded-Proto` and `X-Forwarded-Host`,
but only when the request comes straight from a proxy listed in
`AXIS_TRUSTED_PROXIES` (comma-separated addresses, CIDR ranges and host
names; empty by default, so no proxy is believed). From any other address
the headers are ignored. The client is the rightmost `X-Forwarded-For`
address that is not itself a listed proxy, so several proxies in a row work;
malformed values are ignored. That address goes in the audit lines, on the
Your account page and into the login and worker-token throttles; the scheme
decides whether cookies are `Secure` (and take the `__Host-` prefix); the
host is what the cross-site check compares a form's `Origin` with.

Believing these headers from everywhere would let anyone choose their own
address (and slip past the login throttle) and claim HTTPS. List exactly the
proxy. Set HSTS at the proxy; Axis sends none.

Not listing a proxy that is there has a cost too: every client then has the
proxy's address, so they share one login throttle ([Request
limits](#request-limits)), and one stranger's wrong passwords slow down
everyone. A browser that has logged in to an account before is remembered
(a signed cookie, recorded with the account: at most 20 browsers per
account, the oldest forgotten) and held to a count of failures of its own,
so its owner is not locked out by others. All of an account's remembered
browsers together are held to a second count (ten free failures), so that
browsers cannot multiply guesses. A password change, or disabling the
account, ends every remembered browser of it. When every request with
`X-Forwarded-For` comes from one private address and no proxy is listed,
Axis says so once in its log and on the Settings and Reverse proxies pages.

### Naming the proxy

In Compose, Docker gives a container a new address each time it is created,
so a fixed list of addresses breaks whenever the proxy is recreated. Name the
proxy's service instead:

```yaml
AXIS_TRUSTED_PROXIES: traefik
```

Axis looks the name up through the system's resolver (in a container, Docker's
or Kubernetes' DNS) and trusts every address it gets, IPv4 and IPv6. A name
can sit beside addresses and ranges: `traefik, 192.0.2.0/24`. Names use
letters, digits, `-`, `_` and `.`. Axis refuses to start, and says which
entry, if one is not an address, a range or a name.

How Axis keeps the addresses current:

- **When it looks.** At start, every 30 seconds, and at once (but at most
  every 5 seconds) when a request arrives from an address that is not
  trusted while the list has a name. A proxy that is recreated at a new
  address is believed again within seconds, after the first request from
  it. That first request is treated as from an untrusted address: its
  headers are ignored, so the visitor's address in it is the proxy's.
- **It never waits.** Lookups run in the background. No request waits for
  one.
- **A name that does not resolve trusts nothing.** Until the first lookup
  works, and after the grace below, no address is trusted for the name. Axis
  logs each change once (at warning level for a failure).
- **A short grace for a DNS blip.** When a lookup that worked starts
  failing, Axis keeps the earlier addresses for 30 seconds, one lookup
  period, so a blip does not make every visitor look like the proxy. Then it
  drops them.
- **A cap.** A name is trusted for at most 16 addresses (the lowest, IPv4
  first). If a name has more, Axis logs and shows it.

The **Reverse proxies** page (System, `/admin/proxies`, administrators)
lists each entry with the addresses it stands for now, when they were last
looked up, or why a name does not resolve.

#### When to use a name, an address or a range

A name is only as trustworthy as the resolver that answers for it. In
Docker, a container on the same network can take over a name only if it is
started with that name or network alias, which needs control of the Docker
host or its API; whoever has that can already read the secrets anyway. On
Kubernetes, a Service name resolves to a cluster address that is not the
address the proxy's pods connect from, so use a headless Service (which
resolves to the pods) or a range.

Use a fixed address or a range when:

- the proxy has a stable address (a fixed `ipv4_address`, a host address, or
  a load balancer), because there is then no resolver to trust;
- the name comes from a DNS server you do not control, or a shared network
  where other people can register names;
- the proxy runs on the host or outside any DNS you control.

Never trust a wide range, such as the whole Docker network, to avoid naming
the proxy: every container in it could then claim any visitor's address.
[The Traefik example](examples/traefik/README.md) names the `traefik`
service.

## Sending Axis's traffic through a VPN

Axis downloads nothing itself: the download clients do, and their traffic is
theirs to route. Axis does make connections of its own: searches and release
files to indexers (and to an indexer's own site after a redirect), details
and artwork to TMDb, and API calls to the download clients and media servers
(Plex, Jellyfin). There are two ways to send them through a VPN. The first
covers more; use the second where the first does not fit, or with it.

**1. Run Axis in the VPN container's network.** With Compose, give Axis
`network_mode: "service:gluetun"` (or whatever your VPN container is called)
and no `networks:` of its own. Axis then has no network but the VPN
container's: every connection it makes, name lookups (DNS) included, goes
where that container sends it, and its kill switch (gluetun's firewall)
decides what happens when the tunnel is down. Nothing in Axis is involved,
so nothing in Axis can leak. What follows from it:

- Axis's published port moves to the VPN container (`ports:
  ["127.0.0.1:7800:7800"]` under it), and Axis's health check still works
  (it asks `127.0.0.1` in the shared network).
- The database, Prowlarr and the download clients are reached through the VPN
  container's networks: attach it to them (`private` and `apps` in the
  full-stack example), and allow their subnets through its firewall
  (gluetun: `FIREWALL_OUTBOUND_SUBNETS`, such as `172.16.0.0/12`). The VPN
  container's DNS may not know other containers' names; if `postgres` does not
  resolve, give the database URL an address, or see your VPN container's
  documentation on local names.
- When the VPN container is recreated, Axis loses its network and must be
  restarted too.

**2. Tie Axis's connections to the VPN's interface.** Set
`AXIS_OUTBOUND_BIND` to the interface (`tun0`, `wg0`) or to an address it
holds (`10.8.0.2`). Axis only sees interfaces in its own network namespace:
on the host (Axis installed directly, or `network_mode: host`), in a
Kubernetes pod with a VPN sidecar, or inside the VPN container's network as
above. A container on a bridge network does not see the host's `wg0`.

As each connection is about to be made, Axis reads the interface's state and
ties the socket to the interface itself (`SO_BINDTODEVICE`) and to its
address. The kernel then sends that connection out of that interface only,
whatever the routing table says, and not at all once the interface goes
down or away. When the interface is missing, down (or without carrier) or
has no address, the connection is not made: the request fails at once with
the reason ("the outbound interface tun0 is down"), and nothing is sent by
another route. A VPN that reconnects with a new address is picked up at the
next connection; no connection outlives the request it was made for.

| Value | Each bound connection is tied to | Guarantee |
| --- | --- | --- |
| `tun0` (an interface) | that interface, and its IPv4 address or its IPv6 address (not link-local), whichever the connection's family | Leaves by `tun0` only, from its address; refused while `tun0` is missing, down or without an address |
| `10.8.0.2` (an address) | the interface that holds that address at that moment, and that address | The same, for that address's family only: a server's addresses of the other family are not used |

Why the interface and not only the address: Linux routes a packet by its
destination, so a socket bound only to the VPN's address can still leave by
the default route, from that address (`tests/test-outbound.c` shows it
happening, `/outbound/namespace/kernel`). Tying the socket to the device is
what keeps it in the tunnel. That needs Linux 5.7 or later for a process
without privileges (all capabilities dropped, as in the examples); on an
older kernel the kernel refuses, and so every bound connection is refused,
which the page says. IPv6 connections need an IPv6 route through the VPN
(the VPN adds one when it carries IPv6); without one they fail, and nothing
leaves by another interface.

What is tied depends on `AXIS_OUTBOUND_BIND_SCOPE`:

| Traffic | `internet` (default) | `all` |
| --- | --- | --- |
| Indexer searches, release files, an indexer's site after a redirect | tied, to public addresses | tied |
| Details and artwork (TMDb) | tied, to public addresses | tied |
| Download clients, media servers, a Radarr read by **Migrate** | not tied | tied |
| Anything at a local address (a Prowlarr beside Axis, `10.x`, `192.168.x`) | not tied | tied |
| Anything on this machine (`127.0.0.1`, `::1`) | never | never |
| The database | never | never |
| Name lookups (DNS) | never | never |

Name lookups are made by the system's resolver, through the DNS server the
host or container is given, so whoever runs that server sees which names
Axis looks up (an indexer's, TMDb's). The setting cannot change that; the
first way can. Neither way covers the download clients' own traffic, or a
Prowlarr's: put those behind the VPN where they run.

The state is on **System, Outbound network and VPN**, read when the page is opened,
with the table above for the scope in effect. The log has one line at start
and one at each change (checked every ten seconds), never one per request.
Start-up does not wait for the interface and `/readyz` does not look at it:
a VPN that comes up later is used from then on.

## A worker

`axis-worker` does encoding work for Axis, and can measure the checks of a
converted file. It reads media, writes only its own output in a disk's work
area (`.axis-work/`, never the library), and needs no database.

For a worker on another machine, or one you start with `docker run`,
`podman run`, a `compose.yaml` of its own or a Quadlet unit, select
**Attach a worker** on the **Conversion workers** page: the guide asks what
the machine is and which disks it reaches, gives you the text that starts
the worker there, and shows when the worker has registered
([Run conversions on a worker](../docs/11-run-conversions-on-a-worker.md)).
For the worker of the Compose or Kubernetes example, do it by hand:

1. In Axis, open **Conversion workers**. In **Add a worker without the
   guide**, add one, and copy the token it shows once (Axis keeps only its
   hash).
2. Give it to the worker as a file: the `axis_worker_token` secret in the
   Compose example, `axis-worker-token` in Kubernetes.
3. Start it (`--profile worker` in Compose; `worker.yaml` in Kubernetes). It
   measures its encoders, registers, and proves each disk by the volume
   marker on it. A disk that has no marker yet (it was added while mounted
   read-only) is not offered to any worker until Axis writes its marker
   ([Your disks](#your-disks)); the Conversion workers page names such disks.

Beside Axis it shares Axis's network namespace and talks to
`http://127.0.0.1:7800`. Elsewhere it talks to Axis through a reverse proxy
over `https` (it refuses to send its token over plain HTTP to anything but
loopback, unless `AXIS_WORKER_ALLOW_INSECURE_HTTP` is set); list the proxy
in `AXIS_TRUSTED_PROXIES` so that the throttle on wrong tokens counts per
worker. A GPU: add `nvenc` or `vaapi` to `AXIS_WORKER_ENCODER_FAMILIES` and
give the container the device (commented lines in the examples).

On another machine it reaches the disks through a mount of them, at paths
of its own (it finds each disk by its marker). NFS keeps file identities
(the server's inode numbers and times to the nanosecond), so the worker
checks every file as strictly as Axis does. sshfs and SMB/CIFS do not: list
those disks in `AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY`, and the worker
checks a file there by its size, its time to the second and samples of its
content instead; Axis's own checks on the server do not change. [A worker on
another machine](examples/remote-worker/README.md) is a worked example with
a desktop GPU and rootless Podman: mounts, the token, the proxy, what working
looks like and the messages you may meet.

An encoder that crashes (dies of a signal, rather than giving up on its
input) fails its conversion and pauses its worker: it takes no more work
until you select Resume this worker on the Conversion workers page or restart it. The worker and
Axis must be the same version; a worker of another one is refused and says
so in its log and on the Conversion workers page.

Axis checks every converted file before it places it. By default, the
server makes every measurement of those checks, including a full decode and
a picture comparison, which is heavy work for a small server. To have a
worker make the heavy measurements, set **Where converted files are
checked** (`convert.checks_where`) in **Settings**:

- "On this server" (`server`, default): the server measures, with at most
  `convert.check_threads` threads per decoder and filter (default 2), at the
  lowest CPU and I/O priority.
- "On a worker when one is connected, else on this server" (`worker`): a
  connected worker that has proved the file's disk measures.
  If none is connected, the server measures.
- "Only on a worker: this server does no heavy work" (`worker_only`): only a
  worker measures. If none is connected, the
  conversion waits.

In every mode, the server runs the light checks and decides every check
from the figures. Before it uses a worker's figures, it confirms that they
describe the files on its own disk, by default by reading the whole
converted file once (`convert.confirm_whole_file`). Choosing a worker
trusts it with the figures it measures. [A worker on another
machine](examples/remote-worker/README.md) explains each mode and the trust
it takes.

After every start, Axis holds conversion work (encodes, checks, and sample
measurements, on the server and on workers) until an administrator clicks
**Resume conversion work** on the Conversions or Conversion workers page. This keeps a
server that stopped under the load of a conversion from starting that load
again by itself. Downloads, imports, and scans go on. To turn the hold off,
set `convert.wait_after_start` to off.

## The request portal

The request portal (`axis-portal`) is a separate program for people who are
not administrators. They log in to it, look for a title, ask for it, and see
what became of the request. It is optional. An example that you start as it
is published runs no portal, and the daemon has no portal listener.

**The request portal has not been reviewed for exposure to the internet.**
The portal, the portal API and the requester role have not had their
independent security review. Until this page says that the review has
passed, let the portal be reached only from a network you trust, such as
your home network or a VPN. Do not open its host name to the internet. The
portal logs the same warning each time it starts.

[Let other people ask for titles](../docs/20-let-other-people-ask-for-titles.md#set-up-the-request-portal-administrators)
has the steps in order, from the requester account to the login page.

### What runs where

| Piece | Where it runs | What it holds |
| --- | --- | --- |
| `axisd` | The daemon's container, as before | Everything: the accounts, the requests, the library |
| The portal API | A second listener of `axisd`, off by default | Eleven operations for a portal, and nothing else: no page, no login form, no other API |
| `axis-portal` | A container of its own, from the same image with another entry point | Its token. No database address, no media, no volume |
| The reverse proxy | In front of the portal | The TLS certificate |

To switch the portal API on, set these three settings of the daemon and
restart Axis:

| Setting | What it is |
| --- | --- |
| `AXIS_PORTAL_LISTEN_PORT` | The port of the portal API. The examples use 7801. The main port never answers the portal API. |
| `AXIS_PORTAL_LISTEN_HOST` | The address that Axis has on the network it shares with the portal, so that nothing else reaches that port. |
| `AXIS_PORTAL_PEERS` | The portal's addresses: IP addresses and CIDR ranges, separated by commas. Host names are not accepted. |

`AXIS_PORTAL_PEERS` (`portal_peers` in the `[server]` section of the
configuration file) means two things at the same time:

- The portal API answers a connection from one of these addresses, and from
  no other. From any other address the answer is 403, before Axis looks at
  the token. While the list is empty, the portal API answers 503 to
  everyone. Axis says so in its log when it starts, and the **Request
  portal** page says so too.
- Nobody logs in to Axis itself from one of these addresses. Axis refuses
  the login when the connection comes from a listed address, when a trusted
  proxy names a listed address as the client, and when `X-Forwarded-For`
  names a listed address anywhere, whoever sent the header. The answer is
  the one for a wrong password. See
  [If the portal is taken over](#if-the-portal-is-taken-over).

Axis reads every address in `X-Forwarded-For` for this rule, not only the
client that its trusted proxies arrive at. A sender can add addresses in
front of the one that its proxy writes, and can remove none. So a login
that the portal's container sends through a proxy carries the portal's
address whatever else it carries, also through a proxy that adds to the
header instead of replacing it. The only login that an invented address can
stop is the sender's own.

The rule reads `X-Forwarded-For` and no other header, and in it only an
entry that is an address by itself. An entry with a port
(`172.30.2.3:51234`, `[2001:db8::7]:443`) says nothing to the rule, and the
`Forwarded` header (RFC 7239) is not read at all. The proxies of the
examples write bare addresses in `X-Forwarded-For`. If your proxy writes
ports, or sends `Forwarded` only, the rule sees the address of the
connection and nothing else: set the proxy to write `X-Forwarded-For` with
bare addresses.

Two more refusals belong to the same rule. Both apply only while
`AXIS_PORTAL_PEERS` is set:

- A login that came from, or by way of, an IPv6 link-local address
  (`fe80::/10`) is refused. An interface gives itself such an address, the
  portal's container has one unless IPv6 is switched off in it, and no list
  can name it. A link-local address is never the browser of an
  administrator who reaches Axis through a proxy from elsewhere. If you
  reach Axis directly over a link-local address, use another address.
- While a host name in `AXIS_TRUSTED_PROXIES` has no address (Axis has not
  looked it up yet, or the lookup fails), a login that arrives with
  `X-Forwarded-For` from an address that is not a trusted proxy is refused.
  That address can be the proxy, and Axis cannot check whom it forwards
  for. After a start this lasts until the first lookup is done, usually
  less than a second. A login without the header, directly to Axis, is not
  affected. If the name never gets an address, every login through the
  proxy is refused for as long as that lasts: write the proxy's address in
  `AXIS_TRUSTED_PROXIES` in place of its name, and restart Axis.

Write the portal's own addresses, on every network that the portal has, and
nothing wider:

- A range is as wide as you write it. One IPv6 address is one address, not
  its /64. An IPv4-mapped IPv6 address counts as its IPv4 address. If the
  portal has an IPv4 and an IPv6 address, list both.
- An IPv6 link-local address cannot be listed in a useful way, because the
  portal gets a new one with each new interface. Switch IPv6 off in the
  portal's container instead, as the Compose and Quadlet examples do
  (`net.ipv6.conf.all.disable_ipv6=1` and
  `net.ipv6.conf.default.disable_ipv6=1`), and give the portal's networks no
  IPv6 subnet. On a dual-stack Kubernetes cluster, the portal's pods need a
  range of their own in each family, and both ranges belong in the list.
- Never list an address that is also in `AXIS_TRUSTED_PROXIES`. A login that
  arrives from an address in both lists is refused, whoever it is forwarded
  for. Axis warns about the overlap in its log when it starts, if the
  trusted proxies are written as addresses. If they are written as host
  names, Axis cannot know at the start, and says it under **Needs
  attention** once the names have their addresses.
- Nothing is learned and nothing expires. The list holds until you change
  the setting and restart Axis.

The portal's own settings all begin with `AXIS_PORTAL_`:

| Setting | What it is |
| --- | --- |
| `AXIS_PORTAL_URL` | The daemon's portal listener, such as `http://axis:7801` |
| `AXIS_PORTAL_TOKEN_FILE` | The file that holds the portal's token |
| `AXIS_PORTAL_TRUSTED_PROXIES` | The reverse proxy in front of the portal |
| `AXIS_PORTAL_WEB_LISTEN_HOST`, `AXIS_PORTAL_WEB_LISTEN_PORT` | Where the portal serves its pages (`127.0.0.1` and 7802; `0.0.0.0` in a container) |
| `AXIS_PORTAL_ALLOW_INSECURE_HTTP` | Allows plain HTTP to a daemon that is not on the same machine |
| `AXIS_PORTAL_CA_FILE` | Certificate authorities for an `https` daemon address |

The portal refuses to start with a setting it does not know, and names it.
`/usr/share/axis/axis-portal.conf.example` in the image lists every setting.
`AXIS_PORTAL_LISTEN_PORT` and `AXIS_PORTAL_LISTEN_HOST` are the daemon's
settings: the portal refuses to start when it finds them in its own
environment. `AXIS_PORTAL_PEERS` is the daemon's setting too: give it to the
daemon only.

### The networks

Each example that has the portal builds the same layout:

- The reverse proxy reaches the portal and, on another network, Axis. The
  portal is the only Axis service that the proxy for the portal's host name
  reaches.
- The portal and the daemon share a network that holds those two alone.
- The portal is not on PostgreSQL's network or on any download client's
  network, and it has no media mount.
- In the Compose and Quadlet examples, the portal's networks are internal:
  they have no way out. Leaving a container off a network is not enough. A
  container on an ordinary network can reach the other networks of the same
  host by address, even when no name resolves. With Podman, a portal on
  ordinary networks can open PostgreSQL's port that way. Do not remove
  `internal` from the portal's networks.
- The daemon's main port stays where it was. It is not published through the
  portal's host name.

The portal sends its token and every requester's password to the daemon over
plain HTTP on that shared network. That is why the network holds only those
two. If the portal and the daemon are on different machines, give
`AXIS_PORTAL_URL` an `https` address. The portal verifies the certificate,
and refuses plain HTTP to another machine unless you set
`AXIS_PORTAL_ALLOW_INSECURE_HTTP`.

What the portal's container can reach differs by example:

| Example | The portal reaches | How |
| --- | --- | --- |
| [Traefik, Compose](examples/traefik/README.md#the-request-portal) | The daemon's portal port, and Traefik's ports, where every request from the portal's address gets 403 | The portal's two networks are internal, and the portal has a fixed address on each. The portal API listens on the daemon's address on the shared network. The main port listens on the daemon's address on Traefik's network, so nothing answers it on a network that the portal is on. The portal shares a network with Traefik, and Traefik routes to Axis: a router refuses the portal's address whatever it asks for, and `AXIS_PORTAL_PEERS` holds the portal's address on both networks, so Axis refuses a login that Traefik forwards for the portal. |
| [Kubernetes](examples/kubernetes/README.md#the-request-portal) | The daemon's portal port and the cluster's DNS only, if your network plugin enforces NetworkPolicy | A NetworkPolicy allows that port and nothing else. `AXIS_PORTAL_PEERS` is the range that the portal's pods have addresses in, which your network plugin must give them. Without enforced policies, the portal's pod reaches every pod: Axis still refuses a login from the portal's range, directly or through the ingress controller. |
| [Podman Quadlet](examples/quadlet/README.md#the-request-portal) | The daemon's portal port and its main port | The shared network is internal, so the portal reaches the daemon and nothing else. The portal API listens on the daemon's address on that network. The main port listens on every address, so the portal's container can open the login page, the static files and the probes of Axis. `AXIS_PORTAL_PEERS` holds the portal's address, so a login from it is refused. This example cannot close the main port to the portal: see its README. |

The minimal and full-stack Compose examples and the run commands have no
reverse proxy with TLS, so they have no portal. Use the Traefik example.

In every layout, check one more path: **a reverse proxy that reaches both the
portal and Axis.** A proxy routes by host name, not by where a request comes
from. If the portal's container can open a connection to the proxy that
serves Axis, it can ask that proxy for the host name of Axis. Close that path
at the proxy, as the Traefik example does, and list in `AXIS_PORTAL_PEERS`
the address that the proxy sees the portal at. Or use a second proxy for the
portal that has no route to Axis.

### The token

1. In Axis, open **System**, then **Request portal**, and select **Make a
   token**. Axis shows the token once.
2. Put the token in a file that only the portal's user can read, and give
   the file to the portal as a container secret. Each example says where.
   Never put the token in a command line, a compose file or an address.

To replace a token, make a second one, restart the portal with it, and then
select **Revoke this token** on the first. Up to four tokens can be active.
A revoked token is refused from the portal's next request. When the portal
starts with a new token, everyone who is logged in to the portal logs in
again.

### The proxy in front of the portal

Publish the portal only through a reverse proxy that terminates TLS. The
portal serves a login only when it knows that the request came over HTTPS.
The proxy must do what the portal cannot:

- Redirect plain HTTP to HTTPS.
- Send HSTS (`Strict-Transport-Security`). The portal sends it too, but only
  on answers that it gives itself.
- Limit the size of a request before it is read. The portal takes at most
  64 KiB.
- Limit the time for a request's head and body. The portal cannot time a
  slow sender.
- Limit the connections and the requests of one address.
- Send `X-Forwarded-For`, `X-Forwarded-Proto` and `X-Forwarded-Host`, and
  discard the ones a client sent.

Give the portal a host name of its own. Do not publish it as a path under
the host name of Axis, and do not let the proxy decide by the path.

List the proxy in `AXIS_PORTAL_TRUSTED_PROXIES`. The rules are those of
`AXIS_TRUSTED_PROXIES` ([Behind a proxy](#behind-a-proxy)): addresses,
ranges and host names, and never a whole network. If the proxy is not
listed:

- Nobody can log in. The portal does not know that a request came over
  HTTPS, so it serves a page that says "This portal answers over HTTPS only"
  and no login form.
- Every client has the proxy's address, so all of them share one login
  throttle.
- The portal's log says once that forwarded headers came from an address
  that is not trusted, and names the address. If that address is your proxy,
  list it.

### Hardening

Run the portal as each example does:

- A read-only root filesystem, no volume and no writable directory. The
  portal writes no file.
- No Linux capability, no new privileges, a user that is not root (any user
  id works), and a limit on processes and memory.
- One secret, the token, as a file.

At start, the portal also sets no-new-privileges for itself and makes itself
not dumpable, so it leaves no core file and another process of the same user
cannot read its memory.

The image is the same one that runs `axisd`. The portal's container
therefore also holds `ffmpeg`, Python, Node.js and `yt-dlp`, which the
portal never runs. There is no smaller image for the portal.

### If the portal is taken over

Someone who controls the portal's container holds its token and sees every
password and session that passes through it from then on.

That person can:

- Act as each requester who logs in or uses the portal while it is held,
  within that requester's limits.
- Try passwords of requester accounts, at the daemon's pace: five free
  failures for a username, and 30 failures an hour over all names.
- Ask for titles for those requesters, up to each person's quota and up to
  the daily limit for the portal (30 by default). Each request waits for an
  administrator unless that person is approved automatically.
- Search for titles, up to 30 searches a minute.
- Show requesters false pages, and stop the portal.

That person cannot:

- Read the database, the media, the configuration or any key of an indexer,
  a download client or a provider. None of them is in the container.
- Act as an administrator or an ordinary user, or open any page of Axis
  through the portal API.
- Approve a request, choose a folder or a quality profile, or change or
  delete a file, a title, a download, an account or a setting.
- Act for a requester who does not use the portal while it is held. The
  token by itself acts as no one.

Two rules follow for you:

- **Never type an administrator's password into the portal.** The portal's
  login accepts requesters only.
- **Nobody logs in to Axis from the portal's addresses.** Axis refuses a
  login at its own login page, and the first setup, when the connection
  comes from an address in `AXIS_PORTAL_PEERS`, or `X-Forwarded-For` names
  such an address anywhere. It answers as it answers a wrong password,
  checks no password, and counts no failure. A session that exists is not
  affected. So an administrator's password that reached a portal in the
  wrong hands cannot be used from where the portal is. The rule does not
  depend on what the portal does, and a restart of Axis does not end it.

The answer is that of a wrong password, but the portal's holder can still
tell that the address is refused: a refused login is answered sooner than a
checked one, because no password is checked, and a refused setup never turns
into "Too many failed attempts" as wrong setup tokens do. Neither says
anything about a password or a token.

#### If Axis refuses your own login after you set the portal up

If your browser reaches Axis from an address in `AXIS_PORTAL_PEERS`, Axis
refuses your login too, with the message for a wrong password. This happens
when the list is wider than the portal, or when the portal and your browser
reach Axis from the same address, for example through one gateway. The same
answer has two more causes, both described under
[What runs where](#what-runs-where): your login reached Axis from or by way
of an IPv6 link-local address, or it arrived through a reverse proxy whose
host name in `AXIS_TRUSTED_PROXIES` has no address at the moment.

To find out, look at the log of Axis for a line that begins with
`audit: login refused:`. The line says which of these it was, and names the
address and the setting. Axis writes one such line, and then one a minute at
most. The **Request portal** page shows when Axis last refused a login for
one of these reasons, if you still have a session.

To get back in, do one of these:

- Reach Axis from an address that is not in the list, and not over a
  link-local address.
- If the log names the proxy's host name, wait a few seconds and try again.
  If the name still has no address (the service was renamed, the runtime's
  name server fails, a headless Service has no endpoints), write the
  proxy's address in `AXIS_TRUSTED_PROXIES` in place of its name, and
  restart Axis. That works in every example. Where Axis publishes a port of
  its own, you can also log in there, without the proxy. In the Traefik
  example Axis publishes no port, so there is no such way.
- Correct `AXIS_PORTAL_PEERS` (or `portal_peers` in the configuration file)
  so that it holds the portal's own addresses only, and restart Axis. The
  setting is in the configuration, so you never need to change the database.

If you think the portal was taken over: revoke its token on the **Request
portal** page, set new passwords for the requesters, and look at the
**Requests** page for requests that nobody made.

Under **Needs attention**, Axis shows a notice when the portal reached its
limit of failed logins in an hour or its limit of requests in a day. The
**Request portal** page shows where both limits stand.

### What was run, and what was not

- `make check-container` starts the portal from the image, read-only,
  without a capability, a volume or a tmpfs, as a user id that the image
  does not know, and logs in and asks for a title through it.
- `make check-examples` starts the Traefik example with the portal, logs in
  through Traefik over HTTPS, asks for a title, and tries the database and
  the daemon's main port from the portal's container, by name and by
  address. From the portal's container it also posts the administrator's
  correct password to Traefik with the host name of Axis: Traefik answers
  403 and no session is made. It tries the same by way of the host's
  address on each of the portal's networks, at four starts of Traefik: no
  connection is made. A login that Traefik does forward, for an
  address that the check adds to `AXIS_PORTAL_PEERS`, is refused by Axis.
  The portal's container has no IPv6 address but loopback's, and cannot
  make an IPv6 connection to Traefik. With the portal's container stopped,
  Traefik still refuses the portal's address.
- Before IPv6 was switched off in the portal's container, the same check
  showed the reason: over an IPv6 link-local address, from the portal's
  container, the administrator's password opened a session through Traefik,
  and Axis recorded Traefik's own address as the client. With IPv6 left on
  and the other two barriers in place, Traefik answered that connection 403.
  That the daemon refuses such a login is shown by its tests only.
- It starts the Quadlet example with the portal too, without a reverse
  proxy: it sends one request as the proxy would, and tries the database
  from the portal's container. From the portal's container it posts the
  administrator's correct password to the daemon's main port, which is open
  there: Axis answers as for a wrong password, makes no session, and writes
  the reason to its log. The portal's container has no IPv6 address there
  either.
- These checks ran with rootless Podman. With rootless Podman, a request
  through a published port arrives in the container from one of the
  container's own addresses, and to that same address. For a container on
  two networks, Podman picks the address each time the container starts
  (measured with Podman 5.8: either address, in two and three starts of
  five), and no setting in a Compose file or a Quadlet unit chooses it. Two
  things follow. The Quadlet example cannot bind the daemon's main port to
  one address, because connections through the published port are then
  reset when Podman picks the other. And in the Traefik example, the portal
  answers 503 to every visit through the published port when Podman picks
  Traefik's address on the portal's network, until the Traefik container
  starts again: the portal is then told that the visitor is its own proxy.
  So do not run the Traefik example's portal for other people with rootless
  Podman and published ports. A system-wide Podman was not tried. Docker 29
  was tried once, by hand, for the portal's ways to Traefik only (the
  Traefik example's README, "What the address rule covers").
- The Kubernetes manifests of the portal are only rendered
  (`kubectl kustomize`). They have not been applied to a cluster, their
  NetworkPolicy has not been exercised, and the annotations that give the
  portal's pods a range of their own have not been tried with any network
  plugin.
- Docker has not been run for any example. A certificate from a real
  certificate authority has not been used: the checks use Traefik's own
  default certificate.
- Nobody who did not write the portal has tried to break it. That review is
  what this page waits for.

## Metrics

Off unless `AXIS_METRICS_TOKEN_FILE` (or `AXIS_METRICS_TOKEN`) names a token
of at least 24 printable characters (`openssl rand -hex 32`). Then
`/metrics` answers in the Prometheus text format, only to a request with
`Authorization: Bearer <token>`, and `401` to every other; without a token
it does not exist (`404`). Each wrong token (a bearer token of the token's
form that is not the token) counts against the client's address, an IPv6
address by its `/64`: after ten, the address waits (2 seconds, doubling up
to 15 minutes) and its wrong tokens are answered `429` with `Retry-After`.
The right token always works, so a scanner beside Prometheus cannot lock it
out, and other addresses are not affected. Any other `Authorization`
header (another scheme, or a forward-auth proxy's own bearer token) is not
counted. Wrong tokens are logged at most once a minute. It reports requests by
status class and their durations, jobs waiting and running by queue,
downloads and imports by state (counts only: no titles, paths or names),
whether the database answered, and the version. The token is the only
protection: keep `/metrics` behind the same TLS as the pages if it crosses
a network, and give Prometheus the token from a file:

```yaml
scrape_configs:
  - job_name: axis
    authorization:
      credentials_file: /etc/prometheus/axis-metrics-token
    static_configs:
      - targets: ["axis:7800"]
```

## Logs

Axis logs to standard error, one line per event, which every container
runtime collects. `AXIS_LOG_FORMAT=json` writes each line as a JSON object
with exactly four members, `time`, `level`, `domain` and `message`; the
message is the same text as in the plain format, so the same redaction
applies (database URLs with their password replaced by `****`, tokens and
passwords never written at all).

### When a program crashes

When `axisd`, `axis-worker` or `axis-portal` dies of a fault (a segmentation
fault, a bus error, an illegal instruction, an arithmetic fault or an abort),
it first writes a report to its log, in the log's format: one line with the
signal, the address, the thread and the build, then one line for each frame
of that thread's stack, as a file and an offset in it:

    ERROR axis: crashed: signal 11 (SIGSEGV) at address 0x10 (nothing is mapped there), in thread pool-7 (id 41); build axisd 0.1.0 (abc1234). ...
    ERROR axis: crash: frame 0: /usr/bin/axisd+0x1a2b3c (the instruction that faulted)
    ERROR axis: crash: frame 1: /usr/lib/x86_64-linux-gnu/libglib-2.0.so.0+0x7dd52

The program then ends as it would have, and a restart policy starts it again.
To turn the offsets in an Axis program into functions and lines, take the
debug files of the same build (`make container-debug` at that commit writes
`axisd.debug`, `axis-worker.debug` and `axis-portal.debug` to
`build/container-debug/`) and run `eu-addr2line -f -i -e axisd.debug
0x1a2b3c`. A thread whose own stack overflowed leaves no report.

The report shows where the program died, not what was in its memory. For
that you need a core dump, and the kernel writes those, not Axis: where they
go is one setting of the host (`kernel.core_pattern`), shared by every
container. To keep core dumps of Axis:

1. On the host, set a pattern that is a path, for example `sysctl -w
   kernel.core_pattern=/var/crash/core.%e.%p.%t`. The kernel opens that path
   inside the container that crashed. A pattern that starts with `|` (apport,
   systemd-coredump) hands the dump to a program on the host instead, and
   what that program does with a container's dump is its own business;
   Ubuntu's apport drops it.
2. Mount a writable directory of the host at that path in the container (the
   examples run with a read-only root, so nothing can be written elsewhere),
   owned by the user Axis runs as.
3. Leave the core size limit unlimited (`ulimits: core: -1` in Compose;
   Docker's default already is, Podman's may not be).

A core dump holds everything the program had in memory: session secrets, API
keys, the database password. Keep that directory as private as the secrets
directory, and remove dumps when you are done with them. One dump of `axisd`
is some hundreds of megabytes. `gdb /usr/bin/axisd core.axisd.1.1764266045`
with `axisd.debug` beside it reads one.

## Upgrading

Take a [backup](#backing-up-the-database) first, and keep it until the
upgrade has proved itself. Axis applies database migrations when it starts,
and an older Axis refuses to start on a database a newer one has migrated
(its `/readyz` would say not ready), so going back means restoring the backup
into a newly created empty database, as above, and starting the older image. Migration 0057 (seasons and episodes kept per source of numbering) is one-way: it changes the keys of the metadata tables, so an image from before it can't store a season on a database it has migrated. To go back past 0057, restore the backup you took before the upgrade. With a published image: change the tag (or
pull the moving one) and recreate the container. With your own build: `git
pull --recurse-submodules`, `make container`, then recreate. Watch the log
for `applied migration` lines and `listening on`. A new PostgreSQL major
version needs a dump and restore, not a restart.

Recreate the container rather than start the new one beside the old one. A new Axis that starts while the old one runs waits for it, and migrates nothing until it holds the database ([One daemon per database](#one-daemon-per-database)). A daemon built before the single-daemon lock doesn't take the lock, so a newer one does not wait for it: stop it first.

At each start Axis brings its background schedules to what the new version declares (how often each runs, and what it is given), disables a schedule the new version no longer has (it is kept, not deleted, and the log says so), and moves jobs queued by the older version to the queue the new version runs them from. A version can add a job queue: searches for movies and episodes have a queue of their own since `fix-jobs-hunt-findings`, which needs one more database connection. If you don't set `pool_size` or `AXIS_DATABASE_POOL_SIZE`, Axis sizes the pool itself: the queues' concurrency added up, plus 2 for the queue itself, plus 4 for the pages, the health checks and the worker API (15 with the defaults). If you set it, it must be at least the concurrency added up plus 2, and 4 more is recommended. A pool set for an earlier version can be too small: Axis then stops at start, before it applies any migration, and says how many connections it needs ("the database pool has 12 connections, but the job queue needs 13"), so the previous image still starts on the same database. Raise the setting, or remove it.

## Backing up the database

Everything Axis keeps is in PostgreSQL (the data volume holds only artwork
that can be fetched again). A dump while the stack runs is consistent:

```sh
docker compose exec -T postgres pg_dump --username axis --format custom axis > axis-$(date +%F).dump
```

Restore only into a database you have just created empty, with Axis stopped:

```sh
docker compose stop axis
docker compose exec postgres dropdb --username axis --force axis
docker compose exec postgres createdb --username axis --owner axis axis
docker compose exec -T postgres pg_restore --username axis --dbname axis --exit-on-error < axis-2026-11-12.dump
docker compose start axis
```

Do not restore with `pg_restore --clean` into the database Axis has used. If
a newer Axis migrated that database after the backup, `--clean` cannot drop
the tables that newer tables refer to, ignores those errors, and leaves the
later data in them. The older Axis then starts on a mixture of the backup and
the later data, without any warning. A database created empty has nothing to
mix with, and `--exit-on-error` stops at the first error instead of going
on.

Copy the dump off the machine. A named volume is not a backup: `down -v`
deletes it. [Back up, restore, and repair the
database](../docs/14-back-up-restore-and-repair-the-database.md) has the full
procedure and what to check afterwards.

### Data checksums

The examples create the database with data checksums
(`POSTGRES_INITDB_ARGS=--data-checksums`), so PostgreSQL notices a page that
the disk damaged instead of reading it. The setting applies only when the
database is first created: an existing install keeps what it has. Check with:

```sh
docker compose exec postgres psql --username axis --dbname axis --command "SHOW data_checksums"
```

If the answer is `off`, you have two options. Dump the database, create a new
one with the new settings, and restore into it (the procedure above). Or stop
PostgreSQL and run `pg_checksums --enable` on its data directory; this has
not been tested for Axis. Checksums catch a damaged page when it is read;
they do not replace a backup. Point-in-time recovery needs WAL archiving,
which the examples do not set up; PostgreSQL's documentation describes it.

## SELinux

On a host with SELinux enforcing (Fedora, for instance), a bind mount from
your disks is not readable by the container unless it is labelled. Do not
use `:z` on whole disks (it relabels every file on them); add
`label=disable` to the service's `security_opt` instead.

## The checks

- `make check` runs the unit and daemon tests under the sanitizers.
- `make check-container` (a few minutes, needs Podman and `yq`) builds the
  image, reads `examples/compose/compose.yaml` with `yq` and starts its
  settings as `podman run` options, and checks the hardening, the probes,
  secrets from files, a restart on the same volumes, the worker, another
  user id, and an arbitrary user id with group 0 started before its
  database that becomes ready by itself and creates the first
  administrator from a secret file, and the request portal beside that
  daemon, hardened, with a requester who logs in and asks through it. It
  names everything it creates
  `axis-check-*` and removes it at the end. It does not run a Compose front
  end, so a mistake Compose rejects but `yq` reads would pass it: start an
  example with your own front end once before you rely on it.
- `make check-examples` (`tests/check-examples.sh`; not part of the two above)
  runs the Compose, full-stack, Traefik and Quadlet examples for real with
  Podman, a Compose front end and user systemd, using the local image. In
  each one it adds the first disk and a library folder through the web
  interface, as the examples mount the disk, and scans it. In the Compose
  example it then takes `:ro` off and checks that the marker is written.
  In the Traefik and Quadlet examples it then adds the request portal as
  their READMEs say ([The request portal](#the-request-portal)).
- It pulls public images, names everything `axis-xtest-*`, removes it at the
  end, and skips with a stated reason any part whose prerequisite is missing.
  It does not run the worker of
  [the remote-worker example](examples/remote-worker/README.md), which needs
  two machines and, for two of its three encoders, a GPU. Its `worker` part
  checks what one machine without a GPU can: the `run` script is valid
  shell, `compose config` accepts `compose.yaml` for each encoder, and
  Quadlet's generator accepts `axis-worker.container` in a dry run.
- Every push to `main` runs these checks and `make fuzz` on GitHub
  (`.github/workflows/checks.yml`), each in a job of its own. There, a part
  of `make check-examples` that is skipped fails the job, because the
  workflow installs every prerequisite.
