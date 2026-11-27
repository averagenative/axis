<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Kubernetes

Plain manifests with comments, applied with Kustomize (`kubectl apply -k`).
A Helm chart follows once these have settled.

| File | What it is |
| --- | --- |
| `kustomization.yaml` | The list, and the namespace `axis` everything goes in |
| `namespace.yaml` | The namespace, with the restricted Pod Security Standard |
| `storage.yaml` | `axis-data` (the daemon's own files) and `axis-media` (downloads and library, one volume) |
| `postgres.yaml` | A PostgreSQL 17 StatefulSet and its Service, for convenience |
| `axisd.yaml` | The daemon: one replica, `Recreate`, probes, and its Service |
| `ingress.yaml` | An Ingress with TLS (or use [Traefik's IngressRoute](../traefik/ingressroute.yaml)) |
| `networkpolicy.yaml` | Deny by default; the database only from the daemon, the daemon only from the ingress |
| `worker.yaml` | Encoding workers (not applied until one has a token), GPU lines commented |
| `portal.yaml`, `portal-axisd.yaml` | The request portal (not applied until you switch it on; see [The request portal](#the-request-portal)) |

## What to edit

Lines marked `EDIT`: the image, the storage classes and the media size,
`AXIS_TRUSTED_PROXIES` (your ingress controller's pod addresses), the
ingress class, host name and TLS secret, and the ingress controller's
namespace in `networkpolicy.yaml`.

## Steps

1. Make the secret, from files (nothing secret goes in a manifest):

   ```sh
   kubectl apply -f namespace.yaml
   mkdir -m 700 secrets
   pw=$(openssl rand -hex 24)
   printf '%s' "$pw" > secrets/db-password
   printf 'postgresql://axis:%s@postgres:5432/axis' "$pw" > secrets/database-url
   printf '%s' 'CHOOSE-A-PASSWORD' > secrets/admin-password
   unset pw
   kubectl -n axis create secret generic axis-secrets --from-file=secrets/
   ```

2. Apply: `kubectl apply -k .`
3. Watch: `kubectl -n axis get pods -w`. The daemon may start before its
   database: it is then live (`/livez`) and not ready (`/readyz`), retries
   with back-off, and becomes ready by itself. It creates the administrator
   `admin` with the password from the secret when no account exists.

## Why one replica and `Recreate`

Axis is one long-lived process per database: its job queue, sessions and
in-flight guards assume a single daemon. A rolling update would run two for
a moment; `Recreate` stops the old pod first. Workers are the part that
scales: raise their replicas.

Axis enforces this with a lock in the database. A second daemon pod waits as a standby and never becomes ready while the first one runs, so a `RollingUpdate` never finishes, and extra replicas are standbys, not capacity. See [One daemon per database](../../README.md#one-daemon-per-database).

## The media volume

`axis-media` holds `downloads/` and `media/` and is mounted once, at `/data`,
in every pod that touches them, so that imports are renames (or hardlinks,
for torrents left seeding) and not copies; see
[the layout rule](../compose-full/README.md#the-one-layout-rule). If the
download clients run in the cluster, give them the same claim at the same
path. Workers on other nodes need it `ReadWriteMany` (NFS, CephFS); on a
single node, the `hostPath` lines in `axisd.yaml` are an alternative.

## User and group

The pods run as 1000:1000 with `fsGroup: 1000`; the volumes are given to
that group, and Axis accepts a data directory of one of its groups that the
group may write. On OpenShift, which assigns an arbitrary user id with group
0, remove `runAsUser`, `runAsGroup` and `fsGroup`: the image's own
`/var/lib/axis` belongs to group 0 and is group-writable, and no chown is
ever done at start.

## A worker

Add the worker on the Conversion workers page, then:

```sh
printf '%s' 'THE-TOKEN' > secrets-worker-token
kubectl -n axis create secret generic axis-worker-token --from-file=worker-token=secrets-worker-token
rm secrets-worker-token
```

and uncomment `worker.yaml` in `kustomization.yaml`. The worker reaches the
daemon through the ingress over https; with `networkpolicy.yaml` applied,
a direct `http://axisd:7800` is refused (and needs
`AXIS_WORKER_ALLOW_INSECURE_HTTP`, sound only on an encrypted pod network).
For a GPU, uncomment the device plugin's resource in `worker.yaml` and add
`nvenc` or `vaapi` to `AXIS_WORKER_ENCODER_FAMILIES`.

## The request portal

`portal.yaml` and `portal-axisd.yaml` add the request portal
(`axis-portal`): the pages that people who are not administrators use to ask
for a title. They are optional and left out of `kustomization.yaml`. Without
them no portal runs, and the daemon has no portal listener.

**The request portal has not been reviewed for exposure to the internet.**
Until [the deployment guide](../../README.md#the-request-portal) says that
the review has passed, let the portal be reached only from a network you
trust. Do not point a public host name at it.

The portal needs an image that contains `axis-portal`: a release after
0.1.0-rc2, or an image that you build and push to your own registry.

| File | What it is |
| --- | --- |
| `portal-axisd.yaml` | A patch: the daemon answers the portal API on port 7801, to the addresses in `AXIS_PORTAL_PEERS` |
| `portal.yaml` | The Service `axisd-portal` for that port, the portal's Deployment, Service and Ingress, and three NetworkPolicies |

The NetworkPolicies allow the portal to reach the daemon's port 7801 and the
cluster's DNS, and nothing else: not the daemon's main port, not the ingress
controller, not the database, and nothing outside the cluster. Only the
ingress controller reaches the portal, and only the portal reaches port
7801. Your network plugin must enforce NetworkPolicy for this to hold.

### The portal's addresses

`AXIS_PORTAL_PEERS` in `portal-axisd.yaml` tells the daemon which addresses
are the portal's. The daemon answers the portal API to those addresses
alone, and refuses a login to Axis itself from them. Kubernetes gives a pod
any free address of the pod network, so you must give the portal's pods a
range of their own:

- **An address pool for the portal's pods.** With Calico, make an `IPPool`
  for the range and name it in the annotation
  `cni.projectcalico.org/ipv4pools` on the portal's pod template. With
  Cilium in multi-pool IPAM mode, make a pool and name it in
  `ipam.cilium.io/ip-pool`. `portal.yaml` has the lines, commented. Set
  `AXIS_PORTAL_PEERS` to the pool's range.
- **One fixed address**, where the plugin can pin one (Calico:
  `cni.projectcalico.org/ipAddrs`), with `replicas: 1`. Set
  `AXIS_PORTAL_PEERS` to that address.

These annotations come from the plugins' documentation. They were not tried:
check them against the version that you run.

The range must hold the portal's pods and no other pod. Above all, it must
not hold your ingress controller's pods: Axis refuses every login that
arrives from an address in the range, so nobody could log in through the
controller. Axis warns in its log, and under **Needs attention**, when
`AXIS_PORTAL_PEERS` and `AXIS_TRUSTED_PROXIES` share an address. As shipped
they do not: the portal's range is `10.44.250.0/28`, a pool of its own, and
the trusted range is the pod network `10.42.0.0/16`. Keep the portal's
range outside the trusted range when you change either.

IPv6. On a dual-stack cluster, the portal's pods need a range of their own
in each family, and `AXIS_PORTAL_PEERS` must hold both: the pool annotations
in `portal.yaml` name an IPv4 pool only. A pod's interface also gives itself
an IPv6 link-local address (`fe80::...`), which no range can hold. The
Compose and Quadlet examples switch IPv6 off in the portal's container.
Kubernetes counts the two settings for that among its unsafe sysctls, so
`portal.yaml` has them commented out: a pod that asks for them starts only
if the kubelet allows them. Without them, one rule of Axis stands in: while
`AXIS_PORTAL_PEERS` is set, Axis takes no login that came from, or by way
of, an IPv6 link-local address. That rule is about logins only. The portal's
pod keeps its link-local address, and whatever answers on the node's side
of the pod's interface over IPv6 can be reached from it under an address
that no policy and no range names: an ingress controller on the host
network, or the kubelet's ports. Requests to Axis that are not logins are
not refused on that path.

If your network plugin can do neither, the portal has no address that you
can name ahead of time, and this example does not fit your cluster as it
is. You can write the address that the portal's pod has now, and change it
each time the pod is replaced: until you do, the daemon answers the portal
403 and writes the pod's new address to its log, and the portal serves
nobody. Do not write the whole pod network instead.

What holds on a cluster that does not enforce NetworkPolicy:

- The portal's pod can open a connection to every pod, among them the
  daemon's main port, the ingress controller and the database. The database
  still needs its password, which the portal does not have.
- Axis still refuses a login from the portal's range. It compares the
  address of the connection, the client that the ingress controller names,
  and every address in `X-Forwarded-For`. So a login is refused directly.
  Through the controller it is refused only where the controller sees the
  portal pod's own address, also when the portal's pod sends forwarding
  headers of its own and the controller adds to them instead of replacing
  them.
- Where source NAT stands between the portal's pod and the controller, the
  controller sees a node's address, writes that in `X-Forwarded-For`, and
  the rule does not apply: the login is checked as anyone's. Three cases
  are known, and no cluster was run to find more. kube-proxy rewrites the
  source of traffic to a Service address when the source lies outside the
  cluster CIDR it was given, and the portal's pool as shipped
  (`10.44.250.0/28`) lies outside `10.42.0.0/16`. A pool with outgoing NAT
  switched on rewrites the source toward a controller on the host network.
  A NodePort or a load balancer with `externalTrafficPolicy: Cluster`
  rewrites it too. Before you rely on the rule, log in to Axis from a pod in
  the portal's range through the controller, and read the address in the
  audit line of Axis: it must be the pod's.
- Every pod can reach the portal's port and the daemon's port 7801. The
  daemon answers port 7801 only to the portal's range.

Do not run the portal on such a cluster for people outside your network.

The portal's token and the requesters' passwords cross the pod network
between the portal and the daemon over plain HTTP. The policies keep other
pods from both ports, but they do not encrypt. On a cluster that other
people share, use a network plugin that encrypts pod traffic.

What to edit (marked `EDIT`):

- In `portal-axisd.yaml`: `AXIS_PORTAL_PEERS`, the range of the portal's
  pods.
- In `portal.yaml`: the image, the annotation that gives the portal's pods
  their range, `AXIS_PORTAL_TRUSTED_PROXIES`, the ingress class, host name
  and TLS secret, and the namespaces of the ingress controller and of the
  cluster's DNS.
- `AXIS_PORTAL_TRUSTED_PROXIES`, and `AXIS_TRUSTED_PROXIES` in
  `axisd.yaml`: your ingress controller's pods and nothing else. Use their
  addresses, a range that holds only them, or the name of a headless
  Service that resolves to them. The files ship with a whole pod network,
  which lets every pod in it name any client. The portal's pods must not be
  inside `AXIS_TRUSTED_PROXIES`; as shipped, they are not.
- The DNS rule of the policy `portal-to-axisd-portal-api` admits every pod
  of `kube-system` on port 53. On a cluster with a node-local DNS cache,
  the pods ask a link-local address that the rule does not match: add an
  `ipBlock` for it, or the portal cannot find the daemon and nobody can log
  in.

1. In `kustomization.yaml`, uncomment the `patches` block. Leave
   `- portal.yaml` commented for now. Apply: `kubectl apply -k .`. The
   daemon restarts with its portal listener.
2. In Axis, open **System**, then **Request portal**. The page lists the
   portal's addresses. Select **Make a token**, and copy the token that
   Axis shows once.
3. Make the secret from a file that only you can read. The second command
   reads the token from your keyboard, so that it is in no command line and
   not in your shell's history: paste the token, press Enter, and then press
   Ctrl-D.

   ```sh
   install -m 600 /dev/null portal-token
   ( umask 077; cat > portal-token )
   ```

   ```sh
   kubectl -n axis create secret generic axis-portal-token --from-file=token=portal-token
   rm portal-token
   ```

4. Create a requester account: open **System**, then **Users**, and set
   **Role** to **Requester (the request portal only)**.
5. In `kustomization.yaml`, uncomment `- portal.yaml`, and apply again:
   `kubectl apply -k .`
6. Open `https://requests.example.com/login`, with your host name, and log
   in as the requester.

Check that it worked:

```sh
kubectl -n axis get deploy axis-portal                    # READY 1/1
kubectl -n axis logs deploy/axis-portal | head            # "listening on 0.0.0.0:7802"
```

If the login page says "This portal answers over HTTPS only", the portal
does not believe your ingress controller. One line of the portal's log names
the address that forwarded headers came from. `AXIS_PORTAL_TRUSTED_PROXIES`
must cover that address.

The ingress controller must do what the portal cannot: terminate TLS,
redirect plain HTTP, send HSTS, and limit the size of a request, the time
for its head and body, and the connections of one address. How you set
those depends on the controller. The deployment guide lists them under
[The proxy in front of the portal](../../README.md#the-proxy-in-front-of-the-portal).

To replace the token: make a second token in Axis, replace the secret
(`kubectl -n axis delete secret axis-portal-token`, then step 3), run
`kubectl -n axis rollout restart deploy/axis-portal`, and then revoke the
first token.

If nobody can log in at the portal, read the daemon's log
(`kubectl -n axis logs deploy/axisd`). A line "a connection from ... was
refused before its token was looked at" names the address of the portal's
pod: `AXIS_PORTAL_PEERS` does not hold it.

If Axis refuses your own login after you set the portal up, see
[the deployment guide](../../README.md#if-axis-refuses-your-own-login-after-you-set-the-portal-up).

Not run: these manifests have been rendered with `kubectl kustomize` and
never applied to a cluster. The NetworkPolicies have not been exercised, and
the annotations for the portal's address range have not been tried with any
network plugin.

## Check that it worked

```sh
kubectl -n axis get deploy axisd                    # READY 1/1
kubectl -n axis port-forward deploy/axisd 7800:7800 &
curl -s http://127.0.0.1:7800/readyz                # {"status":"ready"}
```

## Back up and restore

The commands of [Backing up the database](../../README.md#backing-up-the-database)
work through `kubectl -n axis exec` in place of `docker compose exec`. To
restore, scale `axisd` to zero, drop and create the database empty, run
`pg_restore --exit-on-error`, and scale back to one; never use `pg_restore
--clean` over the database Axis has used. `postgres.yaml` sets
`POSTGRES_INITDB_ARGS=--data-checksums`, which applies only when the volume
is first initialized. The Kubernetes form of the commands is untested.
