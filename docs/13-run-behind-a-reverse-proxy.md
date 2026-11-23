# Run Axis behind a reverse proxy

This page shows you how to put Axis behind a reverse proxy such as Traefik
for TLS and a host name, tell Axis which proxy to believe, and check that it
works. Allow about 20 minutes.

Behind a proxy, every request reaches Axis from the proxy's address over
plain HTTP. Axis reads the visitor's address, the scheme and the public host
from the `X-Forwarded-For`, `X-Forwarded-Proto` and `X-Forwarded-Host`
headers, but only when the request comes straight from a proxy you list.
Anyone can send those headers. If Axis believed them from everywhere, a
visitor could choose their own address, slip past the login throttle and
claim HTTPS.

The deployment steps (Compose labels, certificates, Kubernetes, Podman) are
in [the Traefik example](../deploy/examples/traefik/README.md) and in
[Behind a proxy](../deploy/README.md#behind-a-proxy). This page does not
repeat them.

## Before you begin

You need:

- A working Axis ([Install Axis](01-install-and-look-read-only.md)).
- A reverse proxy, with a host name that points to it. The examples use
  `axis.example.com`.
- The name or the address of the proxy, as Axis sees it.

## Steps

1. Choose how to name the proxy. In Compose, name the proxy's service,
   such as `traefik`: Docker gives a container a new address each time it
   is created, and Axis looks the name up again by itself. Use a fixed
   address or a range when the proxy has a stable address or runs outside
   any DNS you control. Never trust a wide range, such as a whole Docker
   network.
2. Set `AXIS_TRUSTED_PROXIES` in Axis's environment to that name, address
   or range. You can list several, separated by commas, such as
   `traefik, 192.0.2.0/24`. Restart Axis. Axis refuses to start, and says
   which entry, if one is not an address, a range or a name.
3. Make the proxy send `X-Forwarded-For`, `X-Forwarded-Proto` and
   `X-Forwarded-Host`. Traefik does. Set HSTS at the proxy: Axis sends
   none.
4. Open `https://axis.example.com/login` and log in.
5. Check what Axis believes. Open **System**, then **Reverse proxies**.
   The **Trusted proxies** card lists each entry:

   | Column | What it shows |
   | --- | --- |
   | **Proxy** | What you set. |
   | **Type** | **Host name**, **Range** or **Address**. |
   | **State** | **Fixed address** for an address or range. For a name: **Resolved**, **Looking up**, **Lookup failing** or **Not resolved**. |
   | **Trusted addresses** | The addresses the entry stands for now. |
   | **Last looked up** | When Axis last looked a name up. |

   <!-- screenshot: the Reverse proxies page with one resolved host name -->

6. Check the visitor's address. Open your **Your account** page. In **Browsers
   logged in to your account**,
   the **Address** of **This session** must be your own address, and not the
   proxy's.

## What the screen shows

A name is looked up when Axis starts, every 30 seconds, and when a request
arrives from an address that is not trusted. A proxy recreated at a new
address is believed again within seconds. The first request from the new
address is treated as untrusted, so its headers are ignored once.

If a lookup that worked starts failing, Axis keeps the earlier addresses for
30 seconds and shows **Lookup failing**, "kept for N more seconds". After
that, **Not resolved** says "None. The name does not resolve (...), so no
address is trusted for it." A name is trusted for at most 16 addresses.

With no entry, the card says "None. Axis believes no X-Forwarded-For,
X-Forwarded-Proto or X-Forwarded-Host header, so every request is taken to
come from the address that connected."

If you put Axis behind a proxy and forget to list it, every request carries
`X-Forwarded-For` and comes from the proxy's one address. Axis notices: the
**Reverse proxies** page and the **Settings** page show "Every request so
far came through one private address, ADDRESS, with an X-Forwarded-For
header, and no reverse proxy is trusted", and the log says it once. Until you list the proxy,
every visitor shares one login throttle, and no visitor's requests are
limited on their own. The same pages also say when the connection limits
refused a connection, and from which address: if that address is your
proxy's, list it.

## Set the proxy's own limits

Axis closes a connection that sends its request too slowly, and caps the
connections it takes in all and per visitor ([Request
limits](../deploy/README.md#request-limits)). When Axis is reachable from
the internet, set at the proxy what Axis cannot do itself:

- Limit the rate of new connections and requests per visitor (Traefik's
  `rateLimit` and `inFlightReq` middlewares).
- Set a timeout for writing a response, and leave `/events` out of it: a
  live page keeps that request open for hours.

## What can go wrong

| What you see | What to do |
| --- | --- |
| Every session shows the proxy's address | The proxy is not listed, or the request did not come straight from it. Check **Reverse proxies**. If the proxy and Axis are on different networks, list the address Axis sees. |
| Visitors get `429` "Too many requests" | One visitor had more requests in flight than `AXIS_MAX_CONNECTIONS_PER_ADDRESS` allows. Live pages and workers are not counted. Raise it only if one visitor really needs more. |
| Visitors get `503` "Axis has too many connections open right now" | Axis had `AXIS_MAX_CONNECTIONS` open, or one public address had `AXIS_MAX_CONNECTIONS_PER_ADDRESS`. **Settings** and **Reverse proxies** say what was refused last and from which address. If that address is your proxy's, list it in `AXIS_TRUSTED_PROXIES`. |
| **Not resolved** | The name is wrong, or the proxy is not on a network Axis shares. Fix the name. |
| A form is refused as coming from another site | Axis compares a form's `Origin` with the public host from `X-Forwarded-Host`. The proxy must send that header and the other two, and must be listed in `AXIS_TRUSTED_PROXIES`. |
| With rootless Podman, every visitor has the proxy network's address | A published port of rootless Podman hides the client's address. Test from another machine, and read the Traefik example before you rely on the throttles. |
| A worker's token fails behind the proxy | List the proxy too, so that the throttle counts per worker. Route `/api/v1/worker/` to Axis with the `Authorization` header. |

## Verified

- The Compose, full-stack, Traefik and Quadlet examples run for real under
  Podman with a Compose front end and user systemd (`make check-examples`,
  58 example checks), with the proxy named by host name.
- The Kubernetes manifests run on kind.
- The queue records no run behind a public host name with real
  certificates.
- With rootless Podman, the address Axis records for visitors was the proxy
  network's, not the client's. The Traefik example documents it.
