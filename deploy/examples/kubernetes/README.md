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
| `portal-axisd.yaml` | A patch: the daemon answers the portal API on port 7801 |
| `portal.yaml` | The Service `axisd-portal` for that port, the portal's Deployment, Service and Ingress, and three NetworkPolicies |

The NetworkPolicies allow the portal to reach the daemon's port 7801 and the
cluster's DNS, and nothing else: not the daemon's main port, not the
database, and nothing outside the cluster. Only the ingress controller
reaches the portal, and only the portal reaches port 7801. Your network
plugin must enforce NetworkPolicy, or none of this holds.

The portal's token and the requesters' passwords cross the pod network
between the portal and the daemon over plain HTTP. The policies keep other
pods from both ports, but they do not encrypt. On a cluster that other
people share, use a network plugin that encrypts pod traffic.

What to edit in `portal.yaml` (marked `EDIT`): the image,
`AXIS_PORTAL_TRUSTED_PROXIES` (your ingress controller's pod addresses, as
`AXIS_TRUSTED_PROXIES`), the ingress class, host name and TLS secret, and
the namespaces of the ingress controller and of the cluster's DNS.

1. In `kustomization.yaml`, uncomment the `patches` block. Leave
   `- portal.yaml` commented for now. Apply: `kubectl apply -k .`. The
   daemon restarts with its portal listener.
2. In Axis, open **System**, then **Request portal**. Select **Make a
   token**, and copy the token that Axis shows once.
3. Make the secret from a file:

   ```sh
   printf '%s' 'THE-TOKEN' > secrets-portal-token
   kubectl -n axis create secret generic axis-portal-token --from-file=token=secrets-portal-token
   rm secrets-portal-token
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

Not run: these manifests have been rendered with `kubectl kustomize` and
never applied to a cluster. The NetworkPolicies have not been exercised.

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
