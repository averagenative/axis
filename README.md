# Axis

Axis is one media engine, written in C, that looks after a media collection on
a server you run yourself. It does the work that is usually spread over
several separate managers (Radarr, Sonarr and the scripts around them): it
finds movies and series, has them downloaded, imports them under names that
Plex and Jellyfin read, and looks after the library afterwards.

What Axis is not:

- **Not a download client.** Axis downloads nothing itself. It drives the
  programs you already run: SABnzbd, Deluge and qBittorrent, and yt-dlp for
  YouTube. It searches your Usenet indexers and torrent sites itself, or
  through Prowlarr if you run it.
- **Not a media server.** Plex and Jellyfin stay your players. Axis tells
  them which folder changed.
- **Not finished.** Axis is early software. Its first public version is a
  release candidate, 0.1.0-rc2. Do not point it at a library you care about
  yet. See [Status](#status).

Axis is a single daemon, `axisd`, with a PostgreSQL database and a
server-rendered web interface built with
[htmx-glib](https://git.podbielniak.com/zachpodbielniak/htmx-glib) and htmx.
There is no Node and no JavaScript build. Axis is designed to run in a
container, with Compose or Kubernetes.

## What you can do today

Each item links to its page in the [user guide](docs/README.md).

- **Find and download movies and series.** You add a title. Axis asks your
  indexers, picks the best release that your quality profile accepts, sends
  it to your download client, and imports the result. It keeps every release
  it considered, with the reasons.
  ([Movies](docs/04-find-and-download-a-movie.md),
  [series](docs/07-add-a-series.md))
- **Take over when you know better.** Search by hand and grab a release
  yourself, grab one by force, block a release, replace a download in
  flight, or delete a download whose import is blocked.
  ([By hand](docs/04-find-and-download-a-movie.md#search-by-hand-and-choose-a-release),
  [delete a download](docs/05-imports-leftovers-and-cleanup-jobs.md#delete-a-download-you-do-not-want))
- **Follow anime.** Mark a series as anime, and Axis matches releases that
  are numbered 1, 2, 3 across seasons to seasons and episodes. This needs a
  TheTVDB key and is partly built.
  ([Anime series](docs/07-add-a-series.md#anime-series))
- **Discover something to watch.** The Discover pages list what is popular,
  trending, out now, coming soon and top rated on TMDb, and recommendations
  from what you have. You add a title in one step, and a title's page has a
  **More like this** card.
  ([Add from a list](docs/04-find-and-download-a-movie.md#add-several-movies-from-a-list))
- **Migrate from Radarr and Sonarr.** Axis reads their APIs, read-only, and
  takes over your titles and the files you have, where they are. It moves
  and renames nothing, and searches for nothing until you start it. Partly
  built: for example, Sonarr needs a TheTVDB key.
  ([Migrate](docs/16-migrate-from-radarr-and-sonarr.md))
- **Download a video from YouTube.** One video at a time, through yt-dlp,
  into a library folder of its own. Playlists and channels are planned.
  ([YouTube](docs/17-download-from-youtube.md))
- **Add more torrent sites.** Axis runs site definitions in the community
  format for public sites that need no login, and can fetch them from
  Jackett's repository to your server. Sites that need a login are planned.
  ([Indexers and clients](docs/03-connect-indexers-and-download-clients.md),
  [fetch definitions](docs/18-fetch-site-definitions-from-jackett.md))
- **Know the library and what is on its disks.** Axis indexes many
  independent disks, recognizes a disk by a marker file and never by its
  path, and shows where the space goes, with duplicates and leftovers. It
  matches the movies and series already there to their titles, and holds
  its doubts for you to review.
  ([Disks](docs/02-add-disks-and-library-folders.md),
  [identify](docs/06-identify-movies-and-series-on-disk.md),
  [leftovers](docs/05-imports-leftovers-and-cleanup-jobs.md))
- **Save space with checked conversions.** Axis estimates what re-encoding
  would save, has you prove a profile with trials, and then converts a
  library one file at a time. A separate worker encodes, on the same machine
  or on another one with a GPU. An original is replaced only after every
  check of the new file passes.
  ([Estimate](docs/08-see-what-converting-would-save.md),
  [trials](docs/09-prove-a-profile-with-trials.md),
  [whole library](docs/10-convert-a-whole-library.md),
  [worker](docs/11-run-conversions-on-a-worker.md))
- **See what needs attention.** A bell on every page gathers what waits for
  a person, such as a blocked import or a disk that is away. Today it is for
  administrators only.
  ([Needs attention](docs/19-see-what-needs-your-attention.md))

Planned and not built: music, books, audiobooks, games, a request portal,
moving files between disks, audio conversion and subtitles. The roadmap is in
[`openspec/project.md`](openspec/project.md).

## Status

Axis is early software and is not ready for a library you care about. The
code and its tests are in the tree, but many parts have run only against test
doubles (hand-written stand-ins for a real service) or on one installation,
so expect corrections when a part first meets the real thing. Build progress
is documented in `openspec/project.md`:
[What has run against real services](openspec/project.md#what-has-run-against-real-services)
says what has been run for real, and
[Known limits](openspec/project.md#known-limits) lists everything known to be
unverified, weak or missing. Each guide page also ends with a **Verified**
note. Axis writes nothing to a library while **Read-only storage** is on.

## Quick start

This starts Axis and its database from the Compose example in
`deploy/examples/compose`, with your media mounted read-only, so Axis can
look and cannot change anything.

You need Linux, `git`, and Docker with the Compose plugin or Podman with
`podman compose`.

The image is `ghcr.io/averagenative/axis`. The release candidate is published
under the tag `0.1.0-rc2` and under the moving tag `rc`, by a workflow that
runs when the tag `v0.1.0-rc2` is pushed to this repository. The image exists
only after that workflow run has finished. If the pull in step 2 says that
the image is not found, build the image yourself instead. The tags `0.1` and
`latest` appear with the first final release.

1. Get the source, which holds the example files:

   ```sh
   git clone --recurse-submodules https://github.com/averagenative/axis.git
   cd axis
   ```

2. Get the image, in one of two ways:

   - **Pull the release candidate.** The example already names this tag, so
     there is nothing to change:

     ```sh
     docker pull ghcr.io/averagenative/axis:0.1.0-rc2    # or: podman pull
     ```

   - **Build the image yourself.** This also needs GNU Make. It doesn't need
     a compiler or the C development packages on your machine, because the
     image is compiled inside a container. The image is tagged
     `localhost/axis:latest`:

     ```sh
     make container
     ```

     Then create a file named `.env` in `deploy/examples/compose` that holds
     the line `AXIS_IMAGE=localhost/axis:latest`.

3. Go to the example, and edit the lines marked `EDIT` in `compose.yaml`:
   one line for each disk, mounted whole, with `:ro` kept at the end.

   ```sh
   cd deploy/examples/compose
   ```

4. Make the three secret files with the commands in step 1 of the
   [example's README](deploy/examples/compose/README.md#steps). That README
   also has the notes for Podman and for a host with SELinux.
5. Start Axis, and find the one-time setup token in its log:

   ```sh
   docker compose up -d        # or: podman compose up -d
   docker compose logs axis | grep "setup token"
   ```

6. Open `http://127.0.0.1:7800/setup` and enter the token, which is the last
   word of that line. This creates the first administrator.

Where to go next:

- [Install and look read-only](docs/01-install-and-look-read-only.md) and
  [Add disks and library folders](docs/02-add-disks-and-library-folders.md)
  take you through the first run.
- To let Axis download and import, use the
  [full-stack example](deploy/examples/compose-full/README.md).
- For Kubernetes, Podman Quadlet, plain run commands, Traefik, a worker on
  another machine, every setting and backups, see
  [`deploy/README.md`](deploy/README.md).

## Build from source

You need Linux, GCC, GNU Make, pkg-config, and the development packages for
GLib, libsoup 3, json-glib, libpq and libsodium. The checks also need Clang
(for the fuzz targets), the sanitizer runtimes (`libasan`, `libubsan`) and
Python 3. Axis runs `ffmpeg`, `ffprobe` and `yt-dlp` as separate programs at
run time. The database is PostgreSQL 15 or later.

Clone with `git clone --recurse-submodules`, as in the quick start, and then:

```sh
make                              # builds axisd and axis-worker
make DEBUG=1 ASAN=1 UBSAN=1       # a debug build under both sanitizers
make release                      # the hardened build
```

A change is ready when four gates pass. Run them one at a time, and alone on
the machine. A gate can take longer than ten minutes.

The first two gates run in parallel. Give `make` a `-j` for the compilers:

```sh
make -j16 check
make -j16 fuzz
```

The test programs and the fuzz targets run several at a time by themselves:
one for each processor, and at most 16. To choose the number, set
`TEST_JOBS` or `FUZZ_JOBS`. Each program writes to a log of its own under
`build/`. The terminal shows one line for each program, with the time it
took. At the end, the terminal shows the whole log of each program that
failed, the ten slowest programs, and the names of the programs that failed.
With `TEST_JOBS=1` or `FUZZ_JOBS=1`, the programs run one after another,
write to the terminal, and stop at the first failure.

| Gate | What it runs | What it needs |
| --- | --- | --- |
| `make check` | `make check-made-up`, then every test under AddressSanitizer and UndefinedBehaviorSanitizer | Podman or Docker, for a throwaway PostgreSQL container |
| `make fuzz` | Every fuzz target for a fixed time | Clang |
| `make check-container` | Builds the image and checks its hardening, probes and secrets | Podman and `yq` |
| `make check-examples` | Runs the Compose, full-stack, Traefik and Quadlet examples for real | Podman, a Compose front end and user systemd |

The throwaway PostgreSQL keeps its data in memory, about 1 GB while 16 test
programs run. To keep the data on disk, set `AXIS_TEST_PG_ON_DISK=1`, and
expect the tests to wait for the disk. To test against a PostgreSQL server
of your own, set `AXIS_TEST_DATABASE_URL`. `make check` never touches a GPU: `make
check-hwenc` runs the hardware encoder tests when you ask for them.

## How the project is organized

- `src/`: the daemon, `axisd`, one directory for each layer (the source
  layout is listed in `openspec/project.md`). `clients/worker/`:
  `axis-worker`. `tests/`: tests, fixtures and fuzz targets. `deploy/`: the
  image and the examples. `docs/`: the user guide.
- [`openspec/specs/`](openspec/specs/): what Axis does, one requirement
  specification for each capability.
- [`openspec/project.md`](openspec/project.md): decisions, architecture,
  conventions, the roadmap and the known limits.
- [`openspec/changes/`](openspec/changes/README.md): where new work is
  proposed, as a proposal, spec deltas and tasks.

Two rules matter most:

- **Made-up titles only.** Tests, fixtures and documents hold invented
  titles, release names, identifiers and hosts, and nothing from anyone's
  own library. `make check-made-up` checks what it can, and titles are
  reviewed by eye.
- **Safety for anything that deletes or moves media.** Every change to a
  file goes through one write layer that proves the disk first. Each safety
  promise is a scenario in a specification, with a test, and such a change
  is reviewed by someone who did not write it.

## Contributing

Axis is a small project. Issues and pull requests are welcome. A change
needs tests, the four gates, and a spec delta under `openspec/changes/` when
it changes behavior. To repair a site definition, see
[How to repair an indexer definition](docs/CONTRIBUTING-definitions.md).

## License and credits

Axis is Copyright (C) 2026 The Axis authors, and is licensed under the GNU
Affero General Public License, version 3 or later (AGPL-3.0-or-later). See
[`COPYRIGHT`](COPYRIGHT) and [`LICENSE`](LICENSE). The footer of every page
in the web interface has a **Source** link to where the source of that build
is offered. If you run a changed Axis for other people, build it with
`SOURCE_URL` set to where your source is.

- The web interface is built with
  [htmx-glib](https://git.podbielniak.com/zachpodbielniak/htmx-glib) and
  [htmx](https://htmx.org). They keep their own notices under `deps/` and
  `data/web/vendor/`.
- Movie and series details come from TMDb. This product uses the TMDb API
  but is not endorsed or certified by TMDb.
- For series that follow TheTVDB's numbering: Metadata provided by TheTVDB.
  Please consider adding missing information or subscribing.

The web interface shows both notices in its footer. Axis ships no key for
either service: you use your own.

## History

Development before the first public commit happened in a private repository.
That history is not published: this repository starts from one commit that
holds the tree as it stood at that point.
