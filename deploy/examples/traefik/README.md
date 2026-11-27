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
| `portal` | `axis-portal` from the same image, read-only, with no capability, no volume, no secret but its token, and no IPv6 |
| `axis-portal` (network) | The daemon (`172.30.1.2`) and the portal (`172.30.1.3`), and nobody else. Internal: it has no way out |
| `axis-portal-proxy` (network) | Traefik (`172.30.2.2`) and the portal (`172.30.2.3`), and nobody else. Internal too |
| On `axis` | The portal API on port 7801, on the daemon's address on `axis-portal` alone; the main port on the daemon's address on `axis-proxy` alone; `AXIS_PORTAL_PEERS` with the portal's two addresses and the host's address on each of the portal's networks |
| On Traefik | The router `axis-from-portal`, among the labels of the `traefik` service itself, which answers the portal's address, the host's address on `axis-portal-proxy`, all of `axis-portal`, and every IPv6 link-local address, with 403 whatever it asks for |

The portal is not on `axis-proxy` or on `private`, and both of its networks
are internal, so its container has no route to PostgreSQL, to the daemon's
main port or to the internet. Keep `internal: true` on both networks.
Without it, a container can reach the other networks of the same host by
address even when no name resolves: with Podman, the portal's container
could then open PostgreSQL's port.

The portal's container can open a connection to two things: the daemon's
port 7801, and Traefik. Traefik routes to Axis by host name, so the file
closes that path twice:

- Traefik refuses the portal's own address (`172.30.2.3`) with 403, before
  any other router is tried. It refuses three more sources the same way: the
  host's address on `axis-portal-proxy` (`172.30.2.1`, the network's
  gateway), all of `axis-portal` (`172.30.1.0/24`, where Traefik has no
  address, so nobody but the portal's container by way of the host can come
  from it), and every IPv6 link-local address. The rule is a `ClientIP` matcher of Traefik 3
  at a high priority, with an `ipAllowList` middleware that admits nobody.
  Traefik has no middleware that refuses an address, which is why the
  example is built this way. The portal never needs to ask Traefik for
  anything. The router's labels are on the `traefik` service, not on the
  `portal` service: Traefik drops the routers of a container that is stopped
  or not healthy, and whoever holds the portal's container decides whether
  it is healthy.
- `AXIS_PORTAL_PEERS` on `axis` holds the portal's address on both
  networks, and the host's address on each of them (`172.30.1.1`,
  `172.30.2.1`). Axis answers the portal API to those addresses alone, and
  only with the token, and refuses a login to Axis itself when the
  connection comes from one of them or when `X-Forwarded-For` names one of
  them anywhere. See
  [The request portal](../../README.md#the-request-portal) in the
  deployment guide.

Both name the portal by its IPv4 addresses, so the portal's container must
have no other address. A container's interface also gives itself an IPv6
link-local address (`fe80::...`). Before these lines were added, the portal's
container was seen to reach Traefik over that address: Traefik did not
recognize it, Axis recorded Traefik's own address as the client, and the
administrator's password opened a session. The file closes that in three
places:

- The `portal` service has IPv6 switched off (`sysctls`,
  `net.ipv6.conf.all.disable_ipv6` and `default`), so its container has no
  IPv6 address but loopback's. The container cannot switch it on again,
  because that needs a capability it does not have. Do not remove the two
  lines, and do not give the portal's networks an IPv6 subnet.
- The router `axis-from-portal` also refuses every IPv6 link-local client
  (`fe80::/10`).
- While `AXIS_PORTAL_PEERS` is set, Axis takes no login that came from, or
  by way of, an IPv6 link-local address.

Every container on the portal's two networks has a fixed address, and so has
Axis on `axis-proxy`. A container without one could be given another's
address when the host starts them in another order, and the other would
then not start.

The portal's token and the requesters' passwords cross `axis-portal` over
plain HTTP, which is why that network holds only the daemon and the portal.

What to edit in `compose.portal.yaml` (marked `EDIT`): the host name of the
portal in the router rule, and the image line. If `172.30.1.0/24` or
`172.30.2.0/24` is taken on your host, change the subnet and every address
in it: the addresses and the `gateway` under `networks`,
`AXIS_PORTAL_LISTEN_HOST`, `AXIS_PORTAL_PEERS`, and the `ClientIP` rule.

1. Bring up `compose.yaml` first and create the first administrator, as in
   [Compose](#compose).
2. Start Axis with the portal's file, so that the daemon gets its portal
   listener. The portal itself cannot start yet, because its token does not
   exist. Create the token's file now, empty and readable by you alone:

   ```sh
   install -m 600 /dev/null secrets/axis_portal_token
   docker compose -f compose.yaml -f compose.portal.yaml up -d axis
   ```

3. In Axis, open **System**, then **Request portal**. The page says "On: the
   portal API is answered on port 7801", and lists the portal's addresses,
   `172.30.1.3, 172.30.2.3, 172.30.1.1, 172.30.2.1`. Select **Make a token**, and copy the token
   that Axis shows once.
4. Put the token in the secret file. The command reads the token from your
   keyboard, so that it is in no command line and not in your shell's
   history: paste the token, press Enter, and then press Ctrl-D.

   ```sh
   ( umask 077; cat > secrets/axis_portal_token )
   ```

   The file has its mode before the token is written into it. Then give the
   file to the user that the portal runs as (`PUID`, 1000 by default), so
   that the portal can read it. Do not make the file readable by everyone
   instead.

   ```sh
   sudo chown 1000:1000 secrets/axis_portal_token           # Docker
   podman unshare chown 1000:1000 secrets/axis_portal_token # rootless Podman
   ```

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
second shows `404`: no page of Axis is reachable through the portal's host
name.

`tests/check-examples.sh traefik` checks the path through Traefik: from
inside the portal's container it asks Traefik (`https://172.30.2.2`) for the
host name of Axis, and expects 403 for the login page, the setup page, the
worker API and a probe. It also expects that the portal's container has no
IPv6 address and can make no IPv6 connection to Traefik, and that Traefik
still refuses the portal's address after the portal's container is stopped.
If you change the addresses, the router `axis-from-portal` must name the
portal's address on `axis-portal-proxy`, or it applies to nobody.

Right after a start, Axis has not yet looked up the name `traefik`
(`AXIS_TRUSTED_PROXIES`). Until it has, usually for less than a second, a
login through Traefik is refused as a wrong password is: with the portal
configured, Axis takes no login that arrives with `X-Forwarded-For` from an
address it cannot yet tell from its proxy. The log of Axis says "trusted
proxy name traefik resolves to ..." when the wait is over. If the name
never resolves (the service was renamed, or the runtime's name server
fails), the log says so, and every login through Traefik is refused for as
long as that lasts. In this example Axis publishes no port of its own, so
you cannot log in without the proxy. The way out is to write Traefik's
address in place of its name: set `AXIS_TRUSTED_PROXIES: 172.30.0.2` on
`axis` in `compose.yaml`, and run `docker compose ... up -d axis`.

If nobody can log in at the portal, read the log of Axis. A line "a
connection from ... was refused before its token was looked at" names the
address that the portal calls from: that address is missing from
`AXIS_PORTAL_PEERS`.

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

**Do not run the portal for other people with rootless Podman and published
ports. Use Docker or rootful Podman for it.** Neither was run with the whole
of this example: Docker was tried once, for the paths in
[What the address rule covers](#what-the-address-rule-covers) only, and
rootful Podman not at all. With rootless Podman, a
connection through a published port reaches Traefik from one of Traefik's
own addresses, and to that same address. With the portal's file, Traefik has
two addresses, and Podman picks one each time the Traefik container starts
(measured with Podman 5.8: the address on the internal network in two starts
of five). No setting in a Compose file chooses it.

- When Podman picks Traefik's address on `axis-proxy`, the portal works,
  and counts every visitor as one address for its login throttle.
- When Podman picks Traefik's address on `axis-portal-proxy`, the portal is
  told that the visitor is its own proxy, and answers 503 to everyone who
  comes through the published port. Its log says "the trusted proxy ... did
  not say who the client is". The portal stays that way until the Traefik
  container starts again.

Axis itself is reached in both cases. The checks therefore reach the portal
from a container on Traefik's network, and print what the published port
answered without judging it. Binding Traefik's entry points to its address
on `axis-proxy` does not help with rootless Podman: when Podman picks the
other address, every connection through a published port is reset (measured
with a listener bound to one of two addresses: three starts of five).

Not run for this example: rootful Podman, the whole of
`tests/check-examples.sh traefik` under Docker (it drives Podman; under
Docker only the paths in the next section were tried), a certificate from a
real certificate authority, and the Kubernetes `ingressroute.yaml` with the
portal.

### What the address rule covers

The address rule is the second barrier, not the first. The first needs no
address: the portal takes a requester's password and no administrator's,
and the portal API accepts no administrator. An administrator who never
types their password at the portal gives the portal's container nothing to
try. Never log in at the portal with an administrator's account. The
portal's login page says so too.

The address rule (the router `axis-from-portal` and `AXIS_PORTAL_PEERS`) is
there for an administrator's password that reaches the portal's container
anyway. It holds for the addresses it names. It cannot cover a path on which
the portal's container reaches the login of Axis looking like someone else.
These are the paths that were tried from the portal's container, with the
administrator's right password:

| Path from the portal's container | Rootless Podman 5.8 | Docker 29 |
| --- | --- | --- |
| Traefik's address on `axis-portal-proxy`, port 443 | 403 from Traefik, no session | 403 from Traefik, no session |
| Traefik over an IPv6 link-local address | No IPv6 address, no connection | No IPv6 address |
| The host's address on either network (`.1`), Traefik's published port | No connection | No connection |
| The host's address on either network, port 443 | Refused | Refused |
| Other addresses of the host, Traefik's published port | `host.containers.internal`: unreachable | The host's address on `axis-proxy` and on its LAN: unreachable |
| A container at an address in `AXIS_PORTAL_PEERS`, through Traefik | Axis refuses the login | Not tried |
| The portal's address, with the portal's container stopped | 403 from Traefik | Not tried |

Rootless Podman: `tests/check-examples.sh traefik`, four runs in a row, each
trying the host's addresses at four starts of Traefik. Docker: one run on an
arm64 machine (Docker 29.6, Traefik 3.6.25), by hand, with the example's
files and a container at the portal's two addresses in place of the portal
(no capability, IPv6 off, not root), at four starts of Traefik; the same
login from a container on `axis-proxy` opened a session, so the paths were
open to a login that Traefik takes. Rootful Podman: not run.

The host's addresses are in the rule even though no run reached Traefik
through them. Whether a runtime forwards such a connection, and from which
address, depends on its version and its firewall rules, which this example
does not control. Listing them costs two things. A login from the host
itself that reaches Traefik from one of those addresses is refused. And the
portal API takes a connection from those addresses too, still only with the
token.

## Back up and restore

Follow [Backing up the database](../../README.md#backing-up-the-database):
restore only into a newly created empty database, never with `pg_restore
--clean`. The `postgres` service creates its database with data checksums,
but only the first time its volume is created.
