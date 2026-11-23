<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Run commands: Axis without Compose

The same two containers as [the minimal Compose example](../compose/README.md),
started with `podman run` or `docker run`. Change the lines marked `# EDIT`.

## Podman

Podman has secrets of its own, so nothing secret is in a file Axis reads
from the host or in `podman inspect`.

```sh
pw=$(openssl rand -hex 24)
printf '%s' "$pw" | podman secret create axis_db_password -
printf 'postgresql://axis:%s@postgres:5432/axis' "$pw" | podman secret create axis_database_url -
unset pw

podman network create axis
podman volume create axis-db
podman volume create axis-data

podman run -d --name axis-postgres --network axis --network-alias postgres \
    --user 999:999 --read-only --cap-drop ALL --security-opt no-new-privileges \
    --pids-limit 512 \
    --tmpfs /tmp:rw,noexec,nosuid,size=64m,mode=1777 \
    --tmpfs /var/run/postgresql:rw,noexec,nosuid,size=1m,mode=1777 \
    --secret axis_db_password \
    -e POSTGRES_USER=axis -e POSTGRES_DB=axis \
    -e POSTGRES_PASSWORD_FILE=/run/secrets/axis_db_password \
    -e POSTGRES_INITDB_ARGS=--data-checksums \
    -v axis-db:/var/lib/postgresql/data \
    --health-cmd 'pg_isready --username=axis --dbname=axis' --health-interval 10s \
    --restart unless-stopped \
    docker.io/library/postgres:17

podman run -d --name axis --network axis \
    -p 127.0.0.1:7800:7800 \
    --user 1000:1000 --read-only --cap-drop ALL --security-opt no-new-privileges \
    --pids-limit 512 --tmpfs /tmp:rw,noexec,nosuid,size=64m,mode=1777 \
    --secret axis_database_url \
    -e AXIS_DATABASE_URL_FILE=/run/secrets/axis_database_url \
    -v axis-data:/var/lib/axis \
    -v /path/to/disk1:/media/disk1:ro \
    --health-cmd '/usr/bin/axisd --healthcheck' --health-interval 15s \
    --stop-timeout 45 --restart unless-stopped \
    ghcr.io/averagenative/axis:0.1.0-rc1
```

Lines to edit in the last command: `-v /path/to/disk1:/media/disk1:ro` (one
per disk, each whole, read-only), `--user 1000:1000` (ids that can read the
media), and the image (`localhost/axis:latest` after `make container`; the
tags that are published are listed under
[Published images](../../README.md#published-images)).

Then add each disk on the Disks page as `/media/disk1`. A disk mounted
read-only is added without a marker and recognised by its filesystem's
identifier; Axis writes its marker the first time the disk is mounted
read-write ([Your disks](../../README.md#your-disks)).

## Docker

Plain `docker run` has no secrets outside Swarm, so the secret files are
bind-mounted read-only instead:

```sh
mkdir -m 700 secrets
pw=$(openssl rand -hex 24)
printf '%s' "$pw" > secrets/axis_db_password
printf 'postgresql://axis:%s@postgres:5432/axis' "$pw" > secrets/axis_database_url
chmod 644 secrets/*
unset pw

docker network create axis
docker volume create axis-db
docker volume create axis-data

docker run -d --name axis-postgres --network axis --network-alias postgres \
    --user 999:999 --read-only --cap-drop ALL --security-opt no-new-privileges \
    --pids-limit 512 \
    --tmpfs /tmp:rw,noexec,nosuid,size=64m,mode=1777 \
    --tmpfs /var/run/postgresql:rw,noexec,nosuid,size=1m,mode=1777 \
    -v "$PWD/secrets/axis_db_password:/run/secrets/axis_db_password:ro" \
    -e POSTGRES_USER=axis -e POSTGRES_DB=axis \
    -e POSTGRES_PASSWORD_FILE=/run/secrets/axis_db_password \
    -e POSTGRES_INITDB_ARGS=--data-checksums \
    -v axis-db:/var/lib/postgresql/data \
    --restart unless-stopped \
    docker.io/library/postgres:17

docker run -d --name axis --network axis \
    -p 127.0.0.1:7800:7800 \
    --user 1000:1000 --read-only --cap-drop ALL --security-opt no-new-privileges \
    --pids-limit 512 --tmpfs /tmp:rw,noexec,nosuid,size=64m,mode=1777 \
    -v "$PWD/secrets/axis_database_url:/run/secrets/axis_database_url:ro" \
    -e AXIS_DATABASE_URL_FILE=/run/secrets/axis_database_url \
    -v axis-data:/var/lib/axis \
    -v /path/to/disk1:/media/disk1:ro \
    --health-cmd '/usr/bin/axisd --healthcheck' --health-interval 15s \
    --stop-timeout 45 --restart unless-stopped \
    ghcr.io/averagenative/axis:0.1.0-rc1
```

## The first administrator

Either read the setup token from the log (`podman logs axis | grep "setup
token"`, then open `http://127.0.0.1:7800/setup`), or, before the first
start, give it from a secret: add
`--secret axis_admin_password -e AXIS_ADMIN_USERNAME=admin -e AXIS_ADMIN_PASSWORD_FILE=/run/secrets/axis_admin_password`
to the `axis` command (with Docker, a bind-mounted file as above).

## Check that it worked

```sh
curl -s http://127.0.0.1:7800/livez     # {"status":"alive"}
curl -s http://127.0.0.1:7800/readyz    # {"status":"ready"} once the database is up
podman healthcheck run axis && echo healthy
```

Axis does not need the database to be up first: it waits, retrying with
back-off, and is ready once the database is.

The `axis-postgres` container creates its database with data checksums
(`POSTGRES_INITDB_ARGS`), but only the first time `axis-db` is used. To back
up and restore, run the commands of
[Backing up the database](../../README.md#backing-up-the-database) with
`podman exec` in place of `docker compose exec`; restore only into a newly
created empty database, never with `pg_restore --clean`.
