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

## The request portal

`axis-portal.network` and `axis-portal.container` add the request portal
(`axis-portal`): the pages that people who are not administrators use to ask
for a title. They are optional and not among the five files above. Without
them no portal runs, and the daemon has no portal listener.

**The request portal has not been reviewed for exposure to the internet.**
Until [the deployment guide](../../README.md#the-request-portal) says that
the review has passed, let the portal be reached only from a network you
trust. Do not forward a port from the internet to it.

The portal needs an image that contains `axis-portal`: a release after
0.1.0-rc2, or `localhost/axis:latest` from `make container`.

The network `axis-portal` holds the daemon and the portal, and nobody else.
It is an internal network: it has no way out, so the portal's container
reaches the daemon and nothing else. It cannot reach PostgreSQL on the
network `axis`, by name or by address, and it cannot reach the internet. The
portal's token and the requesters' passwords cross `axis-portal` over plain
HTTP, which is why it holds only those two.

The portal API listens on the daemon's address on `axis-portal` alone. The
daemon's main port listens on every address of the container, so the
portal's container can also open the login page of Axis. A login from the
portal's address is refused, and a requester cannot log in there at all
([If the portal is taken over](../../README.md#if-the-portal-is-taken-over)).

This example has no reverse proxy. The portal publishes its port on
`127.0.0.1` only, for a proxy that you run on this machine. That proxy must
terminate TLS and do what
[the deployment guide](../../README.md#the-proxy-in-front-of-the-portal)
lists. The portal serves no login without it.

1. In `axis.container`, uncomment the three lines under "The request
   portal". Install the network and start the daemon again:

   ```sh
   cp axis-portal.network axis.container ~/.config/containers/systemd/
   systemctl --user daemon-reload
   systemctl --user restart axis.service
   ```

2. In Axis, open **System**, then **Request portal**. The page says "On: the
   portal API is answered on port 7801". Select **Make a token**, and copy
   the token that Axis shows once.
3. Make the Podman secret:

   ```sh
   printf '%s' 'THE-TOKEN' | podman secret create axis_portal_token -
   ```

4. Create a requester account: open **System**, then **Users**, and set
   **Role** to **Requester (the request portal only)**.
5. Install the portal and start it:

   ```sh
   cp axis-portal.container ~/.config/containers/systemd/
   systemctl --user daemon-reload
   systemctl --user start axis-portal.service
   ```

6. Point your reverse proxy at `http://127.0.0.1:7802`, with a host name of
   its own for the portal. Open `https://requests.example.com/login`, with
   your host name, and log in as the requester.

Check that it worked:

```sh
systemctl --user status axis-portal.service
podman healthcheck run axis-portal && echo healthy
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:7802/login     # 503, or 403
```

The `503` is right: without your proxy the portal serves no login. A plain
request through the published port says nothing of who the client is, and
the portal refuses it. From an address that is not the listed proxy's, the
answer is `403` and "This portal answers over HTTPS only". Through the
proxy, the login page answers `200`.

If the login page says "This portal answers over HTTPS only" through your
proxy too, the portal does not believe the proxy. Run
`journalctl --user -u axis-portal.service`: one line names the address that
forwarded headers came from. Put that address in
`AXIS_PORTAL_TRUSTED_PROXIES` in `axis-portal.container`. The example names
the portal's own address on its network, `172.31.78.3`, which is where a
request through the published port of rootless Podman arrives from. A
system-wide install can see another address.

Every program on this machine can reach `127.0.0.1:7802` and can claim to be
the proxy. Run the portal this way only on a machine whose users you trust.

To replace the token: make a second token in Axis, run `podman secret rm
axis_portal_token`, make the secret again (step 3), run `systemctl --user
restart axis-portal.service`, and then revoke the first token.

Not run for this example: a reverse proxy in front of the portal, and a
system-wide (rootful) install.

## Upgrading

Change the tag on the `Image=` line when you move to another version, run
`systemctl --user daemon-reload`, pull the image (`podman pull
ghcr.io/averagenative/axis:0.1.0-rc2` for the tag the example names), and
run `systemctl --user restart axis.service`. Take a database backup first
(../../README.md, "Backing up the database"). To go back, restore it into a
newly created empty database, never with `pg_restore --clean` over the old
one. The database is created with data checksums, but only when its volume is
first created; an existing install keeps what it has (see "Data checksums"
there).
