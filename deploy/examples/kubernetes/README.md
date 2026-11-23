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
