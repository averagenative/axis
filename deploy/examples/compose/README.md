<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Compose: Axis and PostgreSQL

The smallest stack: Axis, its PostgreSQL, and (when you add one) a worker
beside Axis. Your media are mounted read-only, so Axis can find, analyse
and plan, but not import. To import downloads into the library, use
[the full-stack example](../compose-full/README.md) instead.

Works with Docker Compose and Podman Compose.

## What to edit

In `compose.yaml`, every line to change is marked `EDIT`:

- `image:` (both services): the image is published as
  `ghcr.io/averagenative/axis` from the tag `v0.1.0-rc2` on. The example
  names the release candidate, `0.1.0-rc2`. The minor line (`0.1`) and
  `latest` exist from the first final release on (see
  [Published images](../../README.md#published-images)). To use an image
  that you built with `make container`, set
  `AXIS_IMAGE=localhost/axis:latest` in a `.env` file beside `compose.yaml`.
- `volumes:` of `axis` (and of `worker`): one line per disk, each disk
  mounted whole, ending in `:ro`. You can add a disk that is mounted
  read-only (step 4).
- `AXIS_WORKER_VOLUMES`: the same disks' paths inside the container.
- `PUID` and `PGID` (in `.env`), if the media's owner is not 1000:1000.
- `AXIS_PORT` (in `.env`), if port 7800 is already taken on this machine.

### Podman

- **Rootless Podman** counts user ids inside a user namespace, so your own
  files appear to the container as owned by `root`. Axis (user 1000) can read
  media that other users may read (mode `r-x` for others); for media only
  your own user can read, see [User and
  group](../../README.md#user-and-group).
- **SELinux enforcing** (Fedora): a bind mount of your disks is refused to
  the container unless `label=disable` is added to the service's
  `security_opt` ([SELinux](../../README.md#selinux)).
- `podman compose` may print `Could not find mount at destination
  "/var/run/postgresql"` for the database. It is harmless: the directory is
  a tmpfs the container makes itself.

## Steps

1. Make the secret files. The database password is random and appears in
   two of them: on its own for PostgreSQL, and inside the URL Axis reads.

   ```sh
   mkdir -m 700 secrets
   pw=$(openssl rand -hex 24)
   printf '%s' "$pw" > secrets/axis_db_password
   printf 'postgresql://axis:%s@postgres:5432/axis\n' "$pw" > secrets/axis_database_url
   : > secrets/axis_worker_token
   chmod 644 secrets/*
   unset pw
   ```

   Compose mounts a secret file with the owner and mode it has on the host,
   and the container user (not root) must be able to read it. The directory
   is `700`, so other users on the host cannot get to the files.

2. Start:

   ```sh
   docker compose up -d        # or: podman compose up -d
   ```

3. Create the first administrator, one of two ways:

   - **From the log.** Axis prints a one-time setup token at warning level.
     Run `docker compose logs axis | grep "setup token"`, open
     `http://127.0.0.1:7800/setup`, and enter the token, which is the last
     word of that line.
   - **From a secret file** (no log reading). Before the first start, put the
     password (at least 10 characters) in `secrets/axis_admin_password`, and
     uncomment the three `axis_admin_password` lines and the two
     `AXIS_ADMIN_*` lines. Axis creates that administrator when no account
     exists and prints no token; later starts leave the accounts alone, so
     you can then remove the lines and the file. Log in at
     `http://127.0.0.1:7800/login` as `admin` with that password.

4. Add your disks. In Axis, open **Disks**, enter `/media/disk1`, select
   **Look at this directory**, and confirm. Then add the disk's movie and TV
   folders as library folders. The disk is mounted read-only and has no marker
   yet, so Axis writes nothing to it and recognises it by its filesystem's
   identifier. When you later take `:ro` off and run `docker compose up -d`,
   Axis writes its marker file, `.axis-volume`, at the top of the disk, and
   recognises the disk by it from then on. If Axis cannot recognise a disk
   without a marker, the page says why; see [Your
   disks](../../README.md#your-disks).

## Check that it worked

```sh
docker compose ps                     # axis and postgres "healthy"
curl -s http://127.0.0.1:7800/livez   # {"status":"alive"}
curl -s http://127.0.0.1:7800/readyz  # {"status":"ready"}
```

`axis` is healthy when `axisd --healthcheck` (its `/healthz`) answers 200,
which needs the database. If PostgreSQL is not up yet, Axis waits for it,
trying again with back-off, and answers `/livez` meanwhile; it does not exit.

## A worker

See [A worker](../../README.md#a-worker) in the main deployment notes: add a
worker on the Conversion workers page, put its token in `secrets/axis_worker_token`,
and run `docker compose --profile worker up -d`.

## Back up and restore

Back up, and restore into a newly created empty database, as
[Backing up the database](../../README.md#backing-up-the-database) says;
never restore with `pg_restore --clean` over the database Axis has used. The
`postgres` service creates its database with data checksums
(`POSTGRES_INITDB_ARGS`), but only the first time its volume is created.
`tests/check-examples.sh` runs these commands against this example.
