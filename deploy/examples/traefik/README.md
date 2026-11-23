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

## Back up and restore

Follow [Backing up the database](../../README.md#backing-up-the-database):
restore only into a newly created empty database, never with `pg_restore
--clean`. The `postgres` service creates its database with data checksums,
but only the first time its volume is created.
