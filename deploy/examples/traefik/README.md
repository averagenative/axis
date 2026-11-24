<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Traefik in front of Axis

Two files: `compose.yaml` (Traefik, Axis and PostgreSQL, with Traefik's
labels on Axis) and `ingressroute.yaml` (the same route for Traefik in
Kubernetes, beside [the Kubernetes manifests](../kubernetes/README.md)).

## What Axis needs from the proxy, and why only from it

Behind a proxy, every request reaches Axis from the proxy's address over
plain HTTP. Axis needs three facts the proxy knows:

| Header | What Axis uses it for |
| --- | --- |
| `X-Forwarded-For` | The client's address in audit lines, on the Account page, and for the login and worker-token throttles |
| `X-Forwarded-Proto` | Whether cookies are `Secure` (and named `__Host-`), and which scheme the cross-site check expects |
| `X-Forwarded-Host` | The public host the cross-site check compares a form's `Origin` with |

Anyone can send those headers. If Axis believed them from everywhere, a
client could pick its own address (and dodge the login throttle or forge
audit lines) and claim HTTPS. So Axis believes them only when the request
comes straight from a proxy in `AXIS_TRUSTED_PROXIES` (addresses, CIDR
ranges and host names, comma-separated, empty by default), and ignores them
from any other address. Here the setting is `traefik`, the Compose service's
name: Axis looks it up (Docker's DNS gives Traefik's current address) at
start, every 30 seconds and when a request comes from an address it does not
trust, so Traefik can be recreated at a new address without any edit.
Behind several proxies, list them all: the client is the rightmost address
in `X-Forwarded-For` that is not one of them. Values that are not well
formed are ignored, not guessed at. [Behind a
proxy](../../README.md#behind-a-proxy) says when an address or a range is
the better choice.

## Compose

What to edit (marked `EDIT`): the host name in the router rule, your e-mail
address for Let's Encrypt, the image lines, the disks, and the subnet if
`172.30.0.0/24` is taken. `AXIS_TRUSTED_PROXIES` names the service
(`traefik`), so it needs no change when Traefik gets a new address. The fixed
address (`172.30.0.2`) is optional.

1. Make `secrets/` as in [the minimal example](../compose/README.md).
2. Point the host name's DNS at this machine, with ports 80 and 443 open.
3. `docker compose up -d`.
4. Create the first administrator with the setup token (the last word of the
   log line, `docker compose logs axis | grep "setup token"`) at
   `https://axis.example.com/setup`, using your host name.
5. Add your disks on the Disks page, as `/media/disk1`. A disk mounted
   read-only is added without a marker
   ([Your disks](../../README.md#your-disks)).

### Podman

Traefik's Docker provider reads Podman's API socket, and three things are
different from Docker:

- **The socket.** Start it with `systemctl --user start podman.socket` (or
  `enable --now`), and replace the first `volumes` line of Traefik with
  `${XDG_RUNTIME_DIR}/podman/podman.sock:/var/run/docker.sock:ro`. Stopping
  the unit leaves the socket file behind, so a file that exists does not mean
  the socket listens: `systemctl --user is-active podman.socket` does.
- **SELinux enforcing** (Fedora): uncomment the `security_opt` lines in
  Traefik's service, or it is refused the socket (`permission denied` in its
  log, and no route).
- **Rootless**: ports below 1024 are refused unless the host allows them
  (`sysctl net.ipv4.ip_unprivileged_port_start`). Put `TRAEFIK_HTTP_PORT=8080`
  and `TRAEFIK_HTTPS_PORT=8443` in `.env` and forward 80 and 443 to them.
  Through a published port of rootless Podman, a connection reaches Traefik
  from the network's own address, not the client's, so Traefik, and then
  Axis, record that address for every client (observed with Podman 5.8: the
  audit line of a login from this very machine names Traefik's address on
  the proxy network). A client that reaches Traefik on
  the network itself, or any client with rootful Podman or Docker, is
  recorded correctly. Check the Account page after a login from another
  machine before you rely on the throttles or the audit lines.

## Kubernetes

Apply `ingressroute.yaml` in Axis's namespace instead of `ingress.yaml`,
and set `AXIS_TRUSTED_PROXIES` in `axisd.yaml` to the range Traefik's pods
are in (comments in both files).

## Check that it worked

```sh
curl -sI https://axis.example.com/login | grep -i set-cookie
```

The cookie is `__Host-axis_presession` and has `Secure`. Log in, then open
**Account**: the session's address is your own, not Traefik's. And a request
that reaches Axis some other way (from another container on its network,
say) with `-H 'X-Forwarded-Proto: https'` gets a cookie without `Secure`:
the header was ignored.

## The request portal

`compose.portal.yaml` adds the request portal (`axis-portal`) to the Compose
example: the pages that people who are not administrators use to ask for a
title. It is optional. `compose.yaml` alone starts no portal, and the daemon
then has no portal listener.

**The request portal has not been reviewed for exposure to the internet.**
Until [the deployment guide](../../README.md#the-request-portal) says that
the review has passed, let the portal be reached only from a network you
trust. Do not open its host name to the internet.

The portal needs an image that contains `axis-portal`: a release after
0.1.0-rc2, or an image that you build (`make container`, then
`AXIS_IMAGE=localhost/axis:latest` in `.env`).

What the file adds:

| Piece | What it is |
| --- | --- |
| `portal` | `axis-portal` from the same image, read-only, with no capability, no volume and no secret but its token |
| `axis-portal` (network) | The daemon and the portal, and nobody else |
| `axis-portal-proxy` (network) | Traefik and the portal, and nobody else |
| On `axis` | The portal API on port 7801, on the daemon's address on `axis-portal` alone; the main port on the daemon's address on `axis-proxy` alone |

The portal is not on `axis-proxy` or on `private`, so it can reach neither
PostgreSQL nor the pages of Axis. It reaches the daemon's port 7801 and
nothing else. The portal's token and the requesters' passwords cross
`axis-portal` over plain HTTP, which is why that network holds only the
daemon and the portal.

What to edit in `compose.portal.yaml` (marked `EDIT`): the host name of the
portal in the router rule, the image line, and the subnet if `172.30.1.0/24`
is taken.

1. Bring up `compose.yaml` first and create the first administrator, as in
   [Compose](#compose).
2. Start Axis with the portal's file, so that the daemon gets its portal
   listener. The portal itself cannot start yet, because its token does not
   exist:

   ```sh
   : > secrets/axis_portal_token
   docker compose -f compose.yaml -f compose.portal.yaml up -d axis
   ```

3. In Axis, open **System**, then **Request portal**. The page says "On: the
   portal API is answered on port 7801". Select **Make a token**, and copy
   the token that Axis shows once.
4. Put the token in the secret file. Let the portal's user read the file,
   and nobody else:

   ```sh
   printf '%s' 'THE-TOKEN' > secrets/axis_portal_token
   chmod 600 secrets/axis_portal_token
   ```

   The file must belong to the user that the portal runs as (`PUID`). With
   rootless Podman, that user is counted inside your user namespace: run
   `podman unshare chown 1000:1000 secrets/axis_portal_token` for the
   default `PUID`.

5. Create a requester account: open **System**, then **Users**, and set
   **Role** to **Requester (the request portal only)**.
6. Point the DNS name of the portal at this machine, and start everything:

   ```sh
   docker compose -f compose.yaml -f compose.portal.yaml up -d
   ```

7. Open `https://requests.example.com/login`, with your host name, and log
   in as the requester.

From then on, name both files in every `docker compose` command for this
directory. A command that names only `compose.yaml` removes the portal and
the daemon's portal listener at the next `up -d`.

Check that it worked:

```sh
docker compose -f compose.yaml -f compose.portal.yaml ps portal     # healthy
curl -sI https://requests.example.com/login | grep -i -e '^HTTP' -e strict-transport
curl -s -o /dev/null -w '%{http_code}\n' https://requests.example.com/admin/users   # 404
```

The first `curl` shows `200` and a `Strict-Transport-Security` header. The
second shows `404`: no page of Axis is reachable through the portal.

If the login page says "This portal answers over HTTPS only", the portal
does not believe Traefik. Read `docker compose ... logs portal`: one line
names the address that forwarded headers came from. `AXIS_PORTAL_TRUSTED_PROXIES`
must resolve to that address.

Traefik gives the portal what it cannot do itself: the redirect from HTTP,
HSTS, a limit of 64 KiB on a request's body, a limit on the requests one
address has open, and a rate limit. Traefik v3 allows 60 seconds for a
request's head and body by default
(`--entrypoints.websecure.transport.respondingTimeouts.readTimeout`); do
not set it to 0.

To replace the token: make a second token in Axis, write it to
`secrets/axis_portal_token`, run `docker compose -f compose.yaml -f
compose.portal.yaml up -d --force-recreate portal`, and then revoke the
first token. Everyone who is logged in to the portal logs in again.

With Podman, the notes under [Podman](#podman) apply to the portal too.
Through a published port of rootless Podman, Traefik sees the network's own
address for every client, so the portal counts every visitor as one address
for its login throttle.

Not run for this example: Docker, a certificate from a real certificate
authority, and the Kubernetes `ingressroute.yaml` with the portal.

## Back up and restore

Follow [Backing up the database](../../README.md#backing-up-the-database):
restore only into a newly created empty database, never with `pg_restore
--clean`. The `postgres` service creates its database with data checksums,
but only the first time its volume is created.
