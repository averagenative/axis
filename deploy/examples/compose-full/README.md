<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Compose: the full stack on one data mount

Axis, its PostgreSQL, Prowlarr (indexers), SABnzbd (Usenet) and Deluge
(torrents), set up so that a completed download is moved into your library
by a rename (or, for a torrent left seeding, hardlinked), never copied.

## The one layout rule

Downloads and library must be on **one disk**, reached through **one
mount**, at **the same path** in every container that touches them.

```
/path/to/data            one disk, mounted at /data in axis, sabnzbd, deluge
├── downloads
│   ├── usenet
│   │   ├── incomplete
│   │   └── complete
│   └── torrents
└── media
    ├── movies
    └── tv
```

By default an import moves the files it takes: a rename gives the file its
name in the library in no time and no space, and nothing of it is left in
the download folder (a torrent's client is asked to let go of the torrent
first). A torrent left seeding is hardlinked instead: a second name for the
same file, so that the client still has it. The kernel makes a rename or a
hardlink only within one mount. Two bind mounts of the same disk are two
mounts, so both fail between them (`EXDEV`) and Axis falls back to a full
copy: minutes of disk work for every movie, and twice the space while it
runs (for a seeding torrent, for as long as it seeds).

The download clients' folders for Axis (SABnzbd's `axis` category folder,
Deluge's label folder) are safe to clean by age with a job of your own; see
[Download folders and a cleanup job of your own](../../README.md#download-folders-and-a-cleanup-job-of-your-own).

The common wrong layout, which looks harmless:

```yaml
  sabnzbd:
    volumes:
      - /disk1/downloads:/downloads      # one mount
  axis:
    volumes:
      - /disk1/downloads:/downloads      # a second mount of the same disk
      - /disk1/movies:/movies            # and a third: every import is a copy
```

It also gives the same folder different names in different containers, so
every path Axis reads from a download client needs a mapping. With one
`/data` mount everywhere, a path means the same thing in every container
and no mapping is needed.

## What to edit

Every line to change is marked `EDIT`:

- `/path/to/data` (three times, the same path): the disk with `downloads/`
  and `media/`. Make the folders above first, owned by `PUID:PGID`.
- `image:` lines: the Axis image, and a version you have tested for each
  linuxserver.io image.
- `PUID`, `PGID` and `TZ` in a `.env` file beside `compose.yaml`: the user
  and group that own `/path/to/data`. Every service runs as them, so each
  can read, link and rename what the others wrote.
- `AXIS_PORT`, `PROWLARR_PORT`, `SABNZBD_PORT`, `DELUGE_PORT` and
  `TORRENT_PORT` (in `.env`), for any of the published ports (7800, 9696,
  8080, 8112, 6881) that is taken on this machine.
- Optionally, a VPN for Axis's own traffic: the commented `vpn` service and
  the lines under Axis's `networks:`, as
  [Sending Axis's traffic through a VPN](../../README.md#sending-axiss-traffic-through-a-vpn)
  explains. It does not cover the download clients' own traffic.

### Podman

- **Rootless Podman** counts the ids inside a user namespace: give the
  folders to `PUID:PGID` there, not as your own user on the host:
  `podman unshare chown -R 1000:1000 /path/to/data` (with the ids you chose).
  Without that, Axis and the download clients cannot write the folders. To
  remove them later, use `podman unshare rm -r`.
- **SELinux enforcing** (Fedora): add `label=disable` to the `security_opt`
  of every service that mounts `/path/to/data` (Axis, SABnzbd, Deluge; the
  last two have no `security_opt` yet, so add the key), as in
  [SELinux](../../README.md#selinux).

## Steps

1. Secret files (the administrator's password: at least 10 characters):

   ```sh
   mkdir -m 700 secrets
   pw=$(openssl rand -hex 24)
   printf '%s' "$pw" > secrets/axis_db_password
   printf 'postgresql://axis:%s@postgres:5432/axis\n' "$pw" > secrets/axis_database_url
   printf '%s' 'CHOOSE-A-PASSWORD' > secrets/axis_admin_password
   chmod 644 secrets/*
   unset pw
   ```

2. `docker compose up -d` (or `podman compose up -d`).
3. Set up the download clients' folders as the comments in `compose.yaml`
   say, and Prowlarr's indexers.
4. Log in to Axis at `http://127.0.0.1:7800` as `admin`, then follow the
   setup guide: Prowlarr at `http://prowlarr:9696`, SABnzbd at
   `http://sabnzbd:8080`, Deluge at `http://deluge:8112`; in Disks, add the
   disk `/data` and the library folders `/data/media/movies` and `/data/media/tv`,
   and allow writing when you are ready for imports.

## Check that it worked

```sh
docker compose ps                         # axis and postgres "healthy"
curl -s http://127.0.0.1:7800/readyz      # {"status":"ready"}
docker compose exec axis stat -c '%d %n' /data/downloads /data/media
```

The last command prints the same device number for both folders when they
are on one mount. To see a hardlink work before Axis makes one, make a file
in `downloads/` and link it from inside Axis (the link and the original
then show a link count of 2):

```sh
docker compose exec axis sh -c 'echo x > /data/downloads/torrents/t.mkv && ln /data/downloads/torrents/t.mkv /data/media/movies/t.mkv && stat -c "%h %n" /data/media/movies/t.mkv'
```

Remove both files afterwards. After the first import, `stat -c %h` on the
imported file in `media/` shows a link count of 1 (it was moved) or, for a
torrent left seeding, 2.

## Hardening

Axis and PostgreSQL run as in the minimal example: non-root, read-only root
filesystem, no capabilities. The linuxserver.io images start as root and drop
to `PUID`/`PGID` themselves, so they keep the capabilities that needs; their
web interfaces are published on `127.0.0.1` only. Put a reverse proxy with
TLS in front (see [Traefik](../traefik/README.md)) before offering any of
them to a network.

To back up and restore the database, follow
[Backing up the database](../../README.md#backing-up-the-database): restore
only into a newly created empty database, never with `pg_restore --clean`.
The `postgres` service creates its database with data checksums, but only
the first time its volume is created.
