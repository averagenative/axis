<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Podman Quadlet: Axis as systemd services

Five files that Podman's Quadlet (Podman 5) turns into systemd units:
`axis.network`, `axis-db.volume`, `axis-data.volume`,
`axis-postgres.container` and `axis.container`. systemd starts them at boot
and restarts them if they stop.

## What to edit

In `axis.container`, the lines marked `EDIT`: the image, the disk lines,
and `User=`/`Group=` (numeric ids that can read your media). On an SELinux
host (Fedora), also uncomment `SecurityLabelDisable=true`, or the container
cannot read the disks.

## Steps (rootless, as your own user)

1. Make the Podman secrets. The password appears twice: on its own for
   PostgreSQL, and inside the URL Axis reads.

   ```sh
   pw=$(openssl rand -hex 24)
   printf '%s' "$pw" | podman secret create axis_db_password -
   printf 'postgresql://axis:%s@postgres:5432/axis' "$pw" | podman secret create axis_database_url -
   unset pw
   # Optional: the first administrator without reading the journal; then
   # uncomment the two lines in axis.container.
   printf '%s' 'CHOOSE-A-PASSWORD' | podman secret create axis_admin_password -
   ```

2. Install the units and start:

   ```sh
   mkdir -p ~/.config/containers/systemd
   cp axis.network axis-db.volume axis-data.volume axis-postgres.container axis.container \
       ~/.config/containers/systemd/
   systemctl --user daemon-reload
   systemctl --user start axis.service
   loginctl enable-linger "$USER"      # keep them running after you log out
   ```

   For a system-wide install, copy them to `/etc/containers/systemd/` and
   drop `--user`.

3. The first administrator: either the secret above, or the setup token:
   `journalctl --user -u axis.service | grep "setup token"`, then
   `http://127.0.0.1:7800/setup`.

4. Add your disks on the Disks page, as `/media/disk1`. A disk mounted
   read-only (`:ro` on its `Volume=` line) is added without a marker and
   recognised by its filesystem's identifier; Axis writes its marker the
   first time the disk is mounted read-write ([Your
   disks](../../README.md#your-disks)).

## Check that it worked

```sh
systemctl --user status axis.service axis-postgres.service
curl -s http://127.0.0.1:7800/readyz        # {"status":"ready"}
```

`/usr/libexec/podman/quadlet -dryrun -user` prints the units Quadlet makes
from the files, and says which line it does not understand.

## Upgrading

Change the tag on the `Image=` line when you move to another version, run
`systemctl --user daemon-reload`, pull the image (`podman pull
ghcr.io/averagenative/axis:0.1.0-rc1` for the tag the example names), and
run `systemctl --user restart axis.service`. Take a database backup first
(../../README.md, "Backing up the database"). To go back, restore it into a
newly created empty database, never with `pg_restore --clean` over the old
one. The database is created with data checksums, but only when its volume is
first created; an existing install keeps what it has (see "Data checksums"
there).
