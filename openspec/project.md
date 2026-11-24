# Axis: a single C media engine

This document is the project record: what Axis is, the decisions behind it, its architecture, its conventions, what is built, what is planned, and the limits that are known today. Behaviour is specified in `openspec/specs/`, one specification per capability. New work is proposed as a change under `openspec/changes/`.

## What Axis is

Axis is one long-lived daemon, written in C, that looks after a media collection on a server you run yourself. It does the work that is usually spread over several separate managers and glue scripts:

- **Find and download.** You search for a movie or a series, Axis asks your indexers for releases, chooses one by your quality profile (or you choose one by hand), and sends it to a download client you already run.
- **Import.** A finished download is matched, moved or linked into the library, and named the way Plex and Jellyfin read it. The media server is told which folder changed.
- **Know the library.** Axis indexes the disks, reads each file with ffprobe, identifies the movies and series that are already there, and shows usage, duplicates and what does not play directly on your devices.
- **Save space.** Axis estimates what converting would save, proves a conversion profile with trials, and then converts a library on one or more workers, replacing an original only after strict checks.

Finding and downloading is the primary function. Analysis, storage and conversion are administration features for looking after the collection afterwards, and the web interface puts media first and administration second.

Media types are modules that an administrator switches on or off. Movies and TV are built. Music, books, audiobooks and games are planned.

Axis is designed to run in a container, with Compose or Kubernetes, beside the indexers, download clients and media servers you have. It is not a seedbox: by default a torrent is removed from its client after import and the library keeps one copy of a title.

## Decisions

| Topic | Decision |
|---|---|
| Language and libraries | C (`-std=gnu89`) with GLib, GObject, GIO, libsoup 3, json-glib, libpq and libsodium. GNU Make. |
| Database | PostgreSQL only, version 15 or later. No other database is supported and none is planned. |
| Frontend | Server-rendered HTML through `htmx-glib` (`deps/htmx-glib`, a git submodule, linked statically). No Node, no JavaScript build, no hand-written frontend JavaScript. The only scripts served are the vendored `htmx.min.js` and its server-sent events extension. |
| Licence | AGPL-3.0-or-later. Every source file carries the SPDX identifier. |
| arr API compatibility | None. Axis does not serve the Radarr or Sonarr APIs and does not read their database files. To move from them, Axis reads their own HTTP APIs with an address and a key, read-only (see the `arr-migration` capability). |
| Downloads | Driven through existing clients: SABnzbd, Deluge and qBittorrent. Axis has no Usenet or BitTorrent client of its own. The one built-in client runs yt-dlp as a contained subprocess. |
| Outside tools | ffmpeg, ffprobe and yt-dlp run as subprocesses and are never linked. A crash in one cannot take the daemon down, and a worker can use whichever ffmpeg build has the right hardware encoders. |
| Media servers | Plex and Jellyfin through one server-neutral interface. Axis reads devices and sessions for compatibility evidence, and makes one kind of write: a request to rescan the folder an import changed. More than one server can be connected. |
| Details providers | Axis talks to the providers directly, with each administrator's own key. TMDb is required for movies and series. TheTVDB is optional, for series whose releases follow its numbering. No key is shipped. |
| Indexers | Newznab and Torznab indexers are asked by Axis itself. Prowlarr is an optional source. Some public sites are built in, and other sites run through installed definition files in the community format. |
| Modules | Compiled in and registered in a static table (id, migrations, routes, job handlers, navigation, settings). Switched on and off in the database from the web interface. No loadable plugins. |
| Conversions run elsewhere | The daemon never encodes. `axis-worker` is a separate process, on the same machine or another, that talks to the daemon over the worker API only and never to the database. |
| One daemon per database | A second daemon on the same database waits as a standby and takes over when the first stops. |
| Kind of source | The preference between Usenet and torrents is a tie-break between two releases the quality profile ranks equal. Axis never takes a lesser copy, or waits, because of where a release comes from. |
| After import | Imports move a download's files by default. A torrent is removed from its client after import unless `import.torrents_after` is `seed`. A replaced file goes to a recycle folder on its own volume and is kept two days by default. |
| Deployment | Containers first. The image runs as any non-root user, takes secrets as files, and has separate liveness and readiness probes. Examples for run commands, Compose, Traefik, Kubernetes and Quadlet are in `deploy/examples/`. |

## Architecture

### Binaries

- `axisd`: the daemon. It has one GLib main loop, and all shared state lives on that loop. Blocking work (disk, database, subprocesses) goes to tasks, thread pools, or htmx-glib's deferred responses. `axisd --healthcheck` and `axisd --check-definition` are modes of the same binary.
- `axis-worker`: the conversion worker. It claims a job, runs ffmpeg, reports progress and sends heartbeats over the worker API. It reaches media through a local path or a network mount, with a path map for each worker.

There is no command-line client.

### Source layout

```
src/axisd/      the daemon's main program and the single-daemon lock
src/core/       configuration, logging, errors, the HTTP client, outbound binding
src/db/         libpq wrapper: connection pool, queries, migration runner
src/jobs/       PostgreSQL-backed job queue, schedules, retries, leases
src/storage/    volumes, library folders, guards, the volume write layer
src/library/    scanner, file index, browser queries, analysis, links made by hand
src/probe/      ffprobe output read into a stream model
src/identify/   matching movies and series on disk to titles
src/meta/       details providers (TMDb, TheTVDB), the title store, artwork
src/release/    release-name parser
src/quality/    quality profiles and the upgrade decision
src/indexer/    Newznab, Torznab, Prowlarr, built-in sites; def/ is the definition engine
src/download/   download clients (SABnzbd, Deluge, qBittorrent, the built-in kind)
src/acquire/    search, grab, follow-up and blocklist code shared by Movies and TV
src/import/     the import pipeline, leftovers, duplicates
src/modules/    movies/ and tv/
src/requests/   requests by people who are not administrators: quotas, followers, decisions
src/discover/   Discover lists and recommendations
src/arr/        migration from Radarr and Sonarr
src/youtube/    the YouTube downloader
src/compat/     media servers: devices, sessions, verdicts, rescan requests
src/transcode/  conversion profiles, ffmpeg arguments, estimates, checks, runs
src/api/        the worker API: tokens, claim, heartbeat, report
src/web/        pages, the event hub, accounts and sessions, the attention notices
clients/worker/ axis-worker
data/migrations/NNNN_name.sql
data/web/       htmx, the stylesheet, icons
data/definitions/  the built-in site definitions
deps/htmx-glib  git submodule
tests/          GLib test programs, fixtures/, fuzz/, tools/
deploy/         Containerfile, images.sh, examples/
docs/           the user guide
```

### The job queue

Jobs are rows in PostgreSQL, claimed with `FOR UPDATE SKIP LOCKED` and announced with `LISTEN` and `NOTIFY`. A job has a lease that its handler renews. A result reported by a holder whose lease was lost is discarded. Schedules, retries with back-off, de-duplication keys and named queues with their own concurrency are part of the queue.

### Storage model

- A volume is identified by its filesystem and a marker file, never by its path. A disk added while mounted read-only is recognised by its filesystem's own identifier until Axis can write the marker. Any doubt makes the volume offline.
- An offline volume freezes its rows. A missing mount is never read as deleted files, and no job targets it.
- A file row carries the volume, the relative path, the size, the modification time, the inode and the link count. A file with more than one link, or under a download client's folder, is pinned: it is not converted or moved in place.
- Every change to a file goes through the volume write layer under a write guard, which proves the volume before it changes anything. A library file is never removed except by retiring it into the recycle folder of its own volume.
- Downloads and library should be reached through one mount, so that an import is a rename or a hardlink and not a copy. The examples are built that way.
- `storage.read_only` stops every write to media. Axis starts that way in the minimal example, so you can look before anything changes.

### Conversions

A conversion profile is a target (codec, quality rule, audio and subtitle handling, container) with the conditions under which a file is eligible. Built-in families are Space saver, Balanced and Maximum compatibility. A profile revision cannot replace originals until trials of it have passed.

A conversion is safe even if two workers end up on the same file: each attempt writes to its own uniquely named temporary file, a worker that learns its lease is lost stops its encoder, and the final step (checks, swap, retiring the original) runs only on the daemon side, under a lock for the file, after it has confirmed that the reporting attempt still holds the lease and that the source is unchanged. The heavy measurements of the checks can run on a worker; the daemon always judges them.

A conversion run applies one profile revision to a selection (libraries or one series) as a list made by a job: what would be converted, ranked by estimated saving, and every other file with the one reason it is left out. A run can be paused, resumed and cancelled, survives restarts, and pauses itself after repeated failures.

Guard rails that are on by default: files that are already efficient are skipped; Dolby Vision and HDR10+ are skipped; HDR10 keeps 10 bits and its mastering metadata; audio and subtitles are copied unless a profile says otherwise; pinned files are skipped; a result that is not smaller by the minimum saving is discarded and the file is not tried again with that revision.

### Web interface

The interface is built with htmx-glib's router, element builder and forms. Axis adds local accounts with argon2id password hashes, sessions stored in PostgreSQL, CSRF tokens on every post, default-deny routing, request limits, an event hub over server-sent events for live updates, and its own handler for static files and artwork. Artwork is downloaded once and served by Axis, because the content security policy forbids loading anything from a third party.

### Outbound traffic

Every outbound connection is made by `src/core/axis-http.c`. A connection to an indexer or a download client goes only to that service's configured origin, and an off-origin redirect is not followed, with one documented exception for an indexer's download redirect. `AXIS_OUTBOUND_BIND` ties outbound traffic to one network interface, such as a VPN's, and refuses it while that interface is down.

## Conventions

### Code

- C with `-std=gnu89`, GLib and GObject. Tabs for indentation.
- Names are prefixed `Axis`, `axis_` and `AXIS_`.
- Ownership is expressed with `g_autoptr`, `g_autofree` and `g_steal_pointer`.
- The build is GNU Make with three files: `Makefile`, `config.mk` and `rules.mk`. `DEBUG`, `ASAN` and `UBSAN` are switches.
- Input from outside (an indexer, a provider, a download client, a media server, ffprobe, yt-dlp, a definition file, a release name) is untrusted. It is read by bounded readers with size, depth and time limits.
- Secrets are never logged. Keys in addresses are blanked in every message.

### Build prerequisites

gcc, GNU Make, pkg-config, and the development packages for glib2, libsoup3, json-glib, libpq, libsodium and libxml2. clang for the fuzz targets. The sanitizer runtimes (`libasan`, `libubsan`) for `make check`. ffmpeg and ffprobe at run time. Podman for the database tests and the container checks.

### Database migrations

- Migrations are `data/migrations/NNNN_name.sql`, numbered without gaps.
- A migration is never edited once it is released. A correction is a new migration.
- Migrations are applied in order, each in a transaction, and their checksums are recorded. A database that a newer build migrated is refused.
- A migration must be safe on a live database of realistic size.

### Tests and the four gates

Tests use the GLib test framework and run under AddressSanitizer and UndefinedBehaviorSanitizer. Database tests run against a throwaway PostgreSQL container started by `tests/with-postgres.sh`. Every parser of outside input has a fuzz target under `tests/fuzz/`.

A change is ready to merge when the four gates pass on its final code:

| Gate | What it runs |
|---|---|
| `make check` | `make check-made-up` first, then every test program under the sanitizers. |
| `make fuzz` | Every fuzz target for a fixed time. |
| `make check-container` | Builds the image and checks it: secrets from files, an arbitrary user id, readiness, the first administrator. |
| `make check-examples` | Runs the Compose, full-stack, Traefik and Quadlet examples for real, including the documented backup and restore. |

Run the gates one at a time and alone on the machine. Two test runs that share the build directory disturb each other, and the gates can take longer than ten minutes. A longer fuzz run has found defects that a short one missed, so do not shorten the fuzz gate.

`make check` and `make fuzz` run in parallel: `make -j16 check` and `make -j16 fuzz`. The `-j` is for the compilers. The test programs run `TEST_JOBS` at a time and the fuzz targets `FUZZ_JOBS` at a time, by default one for each processor and at most 16, each with a log of its own under `build/`; every program runs whatever the others did, and the failures are reported at the end. A value of 1 runs them one after another on the terminal and stops at the first failure. A fuzz target still gets its whole `FUZZ_SECONDS` on a processor of its own, so do not set `FUZZ_JOBS` above the number of processors. A test program must not depend on running alone: its own database, its own scratch directories, ports from `axis_test_free_port()`, and no deadline that only an idle machine meets. The throwaway PostgreSQL keeps its data in memory (`AXIS_TEST_PG_ON_DISK=1` puts it on disk).

Other checks are outside the gates because they need hardware or more time: `make check-scale` (large generated libraries), `make check-hwenc` (hardware encoders), `make check-jellyfin` (a real Jellyfin in a container), and `make test RELEASE=1 AXIS_SKIP_DB_TESTS=1` (the release build, as the image build runs it).

### Made-up titles only

The repository holds made-up titles, release names, identifiers, hosts and addresses only, in tests, fixtures, fuzz seeds, documents and code. Nothing of any person's own setup or library belongs in it. `make check-made-up` (`tests/tools/check-made-up.py`) fails on an info-hash, a provider identifier, a host name, an e-mail address, a private address or a home directory that is not of a made-up form. It cannot judge titles: review those by eye, and run the check with `--titles` to list them. A recorded answer from a real service is kept as a structurally faithful recording with invented content; `tests/tools/scrub-fixture.py` helps to make one.

### Safety promises

A safety promise is written as a scenario in a specification, and each scenario has a test. Examples: an offline volume never loses rows; an original is retired only after every check of its conversion passes; a pinned file is not touched; deleting a download never touches a library folder. Where it can be done, the test is seen to fail with its check switched off before the change is merged.

### Independent review

A change that writes or deletes media, or that touches security (accounts, sessions, outbound requests, secrets, the worker API), is reviewed by someone who did not write it before it is merged. The reviewer reads the diff and tries to break it: the unhappy paths, a crash at each step, two things at once, outside input, and what the specification promises against what the code does. Findings are fixed, each with a test, before the merge.

Between larger pieces of work, each area is also read as a whole for one class of fault at a time: data loss, the wrong file touched, a state that never recovers, races, leaks, trusted input, and a promise in a specification that has no test. A finding states what happens, the steps that show it, how sure the finder is (shown by a test, read in the code, or suspected), and how bad it is.

### Documentation

The user guide is in `docs/`, organised by what a person wants to do. Deployment detail is in `deploy/README.md` and `deploy/examples/`. Documents follow the Google developer documentation style guide and are plain ASCII. A page says what has been run against a real service and what has only run in tests.

### Planning with OpenSpec

- There is one specification per capability in `openspec/specs/`. A specification describes observable behaviour only.
- A change is one small slice under `openspec/changes/<name>/`, with a proposal, a design when one is needed, spec deltas and tasks. It is archived as soon as it lands, which merges its deltas into the specifications.
- When a change is built in slices and only some are built, only the built requirements go into the specifications.
- This document holds what a specification cannot: decisions, the roadmap and the known limits. When a change adds a limit, it adds it here.

## Capabilities

The table lists each capability in `openspec/specs/` and what is planned beyond them. "Built" means the code and its tests are in the tree. Whether a capability has run against the real service is a separate question; see "Known limits".

| Area | Capability | State |
|---|---|---|
| Foundation | `daemon-core`, `database`, `job-queue`, `job-ui`, `settings`, `module-management` | Built |
| Web | `web-shell`, `web-auth`, `web-live`, `setup` | Built |
| Web | `attention` (the bell and the "Needs attention" page) | Partly built: notices for administrators; no notices outside Axis |
| Storage | `storage-volumes`, `library-scan`, `media-probe`, `library-browser`, `library-analysis` | Built |
| Identification | `library-identification` | Built for movies and series |
| Details | `metadata`, `title-search`, `discover` | Built with TMDb; TheTVDB partly built (see the roadmap) |
| Indexers | `indexers`, `release-parsing`, `quality-profiles` | Built; site definitions that need a login are not |
| Downloads | `downloaders`, `import`, `outbound-binding` | Built |
| Media | `movies`, `tv` | Built; anime support in `tv` is partly built |
| Media | `youtube` | Partly built: one video at a time |
| Migration | `arr-migration` (from Radarr and Sonarr) | Partly built |
| Players | `client-compatibility` | Built for Plex and Jellyfin |
| Conversion | `space-estimate`, `conversion-profiles`, `conversion-arguments`, `conversion-execution`, `conversion-runs`, `worker-protocol` | Built for video; see the roadmap for what a run does not do yet |
| Operations | `deployment`, `user-documentation` | Built; the guide is partly written |
| Planned | Music, books, audiobooks, games, a request portal, placement between disks, audio conversion, file preview | Not built |

## Roadmap

Nothing here is built. Each item becomes one or more changes under `openspec/changes/` when its turn comes. The order within a group may change.

### The remainder of what is partly built

- **Needs attention.** Ten sources of notices are missing: an indexer without a key or with capabilities that cannot be read, a details provider's key missing while titles exist, a grab by hand that failed, YouTube that cannot run, conversion work held, a profile version waiting to be proved, a rejected trial, the outbound interface, a proxy that should be trusted, and a library scan that ended incomplete. Notices for users who are not administrators would show only what they can act on. Telling a person outside Axis (a webhook, a push or an e-mail, chosen by each user) is planned and not designed.
- **Migration from Radarr and Sonarr.** Without a TheTVDB key a Sonarr cannot be migrated. The planned strict check brings a series under TMDb's numbering only when every monitored or file-bearing episode agrees in season, number and air date, and skips the rest with a reason. Also open: links to the Migrate page from the setup guide and the empty lists, and one step to start the held titles of a connection that was forgotten.
- **TheTVDB.** A default source for new series, and TheTVDB's other episode orders with a warning. Identification of folder tags and year-less folders under both sources, recording the source that the files fit. A suggestion to change a series' numbering, shown with its evidence and never applied by itself. Provider keys from the environment or a file. The first step is a session against the real API to confirm what is assumed.
- **Anime.** Confirm the searches against real indexers. Then decide whether to loosen the rule that leaves low numbers under another name to a person, whether to read mixed forms such as `S2 - 05`, and whether a person may correct a number that TheTVDB has wrong. Further out: an anime details provider, and preferences for audio, subtitles and groups.
- **YouTube.** Next is picking a YouTube video for one episode of a series, imported under the series' numbering with a file name and history that say where it came from. After that: a series' YouTube source (a channel or playlist) listed and paired with episodes by title and date, where only a confident or confirmed pairing is fetched; playlists and channels followed into the YouTube library, with retention; subtitles and thumbnails. The principles hold for every slice: off until switched on, nothing re-encoded or cut, no cookies by default, one record and one place for each video, and failing closed under the outbound binding.
- **Site definitions.** Logins (form, cookie and key) with the session kept in memory and a hard back-off after a refusal, so that an account is not locked by retries. Then an optional browser-challenge helper that an administrator configures; Axis never embeds a browser. Also the git trees fallback for the Jackett fetch, for a folder of more than 1,000 files.
- **Conversion runs.** The space projection for each volume, other orderings (the fullest volume first), a read limit for each physical disk, standing runs that pick up newly imported files, and estimates corrected from the results of files already converted.
- **Downloads page.** Give series the stray-download check that movies have.
- **By hand.** A manual import page for series, "Assign by hand" on an import that ended imported with files left, renaming a file to match a link made by hand, and restoring a recycled duplicate.
- **User guide.** The missing pages, and screenshots from the demo instance.

### Database resilience

A review of the database layer led to these planned changes, in order:

1. Timeouts and retries: a `lock_timeout` below the statement timeout, an error code of its own for a deadlock, a serialization failure and a lock wait that ran out, a bounded retry for transactions that change several rows, one lock order (the job's row before the module's own row), TCP keep-alives and a client-side deadline, and the pool's use in `/metrics`.
2. Disk steps that survive a lost commit: a conversion records its settled stamp before it settles; a failed retire transaction is retried; a session lock whose unlock fails closes its connection.
3. A schema check at start: the catalog is compared with what the migrations create, each missing or different object is logged, and the daemon does not report ready.
4. Indexes for the foreign keys that bulk deletes follow, a test that fails on a new unindexed foreign key, and retention for search history.
5. `axisd --check-database`, read-only: orphaned jobs, recycle and partial files that no row records, rows that point at nothing, volumes without a marker. Repairs come later, one at a time, each with a test.
6. A Database page under System: server version, checksums, size, pool use, the longest open transaction, and health messages that tell "refused", "no answer" and "disk full" apart.

Replication, high availability and point-in-time recovery are not goals.

### Conversion

- **Worker load limits.** Decode on the GPU as well, or cap ffmpeg's threads, so that an encode does not saturate a weak CPU. Make the order of encoder families a setting.
- **Canary and watch.** A small sample set for each new target (SDR 1080p, HDR10 4K, a file with several audio tracks, one with image subtitles) that you play on your real devices before the profile is unlocked, and a watch on the media server for a rise in server-side conversions of files Axis made.
- **Audio conversion.** Music and audiobooks through the same profiles, runs and checks. An album converts as a unit. Tags, cover art and gapless information are carried over and compared. Lossy to lossy is never the default. Built-in families: keep lossless, space saver (Opus), maximum compatibility (MP3 or AAC), and audiobooks.
- **File preview.** A file handler with byte ranges, to play a file or a trial in the browser. It needs no script. A playback module could grow from it; replacing a media server is not a goal.
- **Other space levers.** Dropping audio tracks and subtitles in languages you do not want, and converting lossless surround to a lossy codec that your players support. Off by default, because it cannot be undone.
- **Sandboxing.** Landlock, seccomp or a network namespace around ffprobe, ffmpeg and yt-dlp.

### Storage

- **Placement.** A planner that proposes moves between volumes as a list you review: bring a split series together, relieve a disk that is nearly full, keep downloads and their library on one disk. Moves run as copy, verify and rename jobs.
- **Refetch a lost volume.** When a disk is declared gone for good, everything that was on it goes back on the wanted list and is fetched again onto the remaining disks, at the quality it had.

### New modules

- **Music.** Low priority. Albums first, and single songs. MusicBrainz and the Cover Art Archive for details (no key; one request a second and a contact string from a setting). Quality profiles for music that say which formats are accepted and preferred. Sources in an order you set: Usenet, torrents, Soulseek through slskd, then YouTube. Axis may rename, retag and move an existing music library; that is for music only. Playlists may come later as a module of their own. Lidarr's model of artists, albums, releases and tracks is the reference to survey first.
- **Books and audiobooks.** Books are identified mostly from embedded metadata and identifiers, less from file names; one work has many editions and formats. Audiobooks come as one file or dozens, carry chapters, and are matched by author, title and narrator; many come from trackers that require seeding indefinitely, so their imports must never disturb the seeded files. Axis finds, downloads and files the book, and hands it to the library application you already run through a connector for each (an ingest folder, or a library folder and a scan request). First connectors: Calibre-Web-Automated and Audiobookshelf. Providers are chosen when the work starts; each needs its terms and rate limits checked first.
- **Games.** A module that supplies games and never runs them. One track places console ROMs, identified by checksum against catalogues that the administrator installs (No-Intro and Redump publish them; Axis ships none), into the folders that RomM reads, and asks RomM to scan through its API. The other keeps PC releases as downloaded file sets, with IGDB for details. The principles: nothing is installed, run, patched or emulated; no archive is extracted to disk; imported files carry no execute permission; no game site is switched on or given an address by Axis, because imitations of such sites serve malware; and RomM's database is never written.
- **Requests.** A request portal with roles, where a person finds a title in Discover and asks for it. This is the piece most likely to face the internet, so it comes after the account system has had time to harden. The change `add-request-portal` plans it in five slices. The first is built: requests inside Axis's own pages, for ordinary users (see "Requests" under "Known limits"). The requester role, the portal's API and the `axis-portal` program are not built, and nothing of requests faces the internet.

### Smaller items

- Discover: collections, people and similar titles, language and certification filters, exclusions for each person.
- Subtitles.
- When a third kind of source can supply the same title, the one preferred kind of source becomes an ordered list, with the same strength as today: a tie-break.
- A scene-numbering mapping is not planned. Following TheTVDB's order for a series is the supported way.

## Known limits

This section collects what is known to be unverified, weak or missing in what is built. None of it is hidden behind a flag. Read the part for an area before you rely on it or change it. When you remove a limit, remove its entry.

Three phrases are used with a fixed meaning:

- **Not run against the real service:** the code is tested against a stand-in whose answers were written by hand from the service's documentation. Expect corrections on first contact.
- **Believed:** a detail of an outside service that the code depends on and that nobody has confirmed. The README beside each fixture set under `tests/fixtures/` lists these in full.
- **No test:** a scenario in a specification that no test covers yet.

### What has run against real services

| Service or environment | State |
|---|---|
| TMDb | Search, title details, seasons, find by identifier and artwork have been used against the real service through the pages. The fixtures are hand-written, not recorded. Discover's lists have not been asked of the real service. |
| TheTVDB | Not run against the real service. Only the `absoluteNumber` field of an episode was confirmed live. Everything the documentation leaves unsaid is in `src/meta/axis-meta-tvdb-assumed.h`. |
| Prowlarr | Run once, read-only, against Prowlarr 3.0.1. Its answers are kept as scrubbed recordings in `tests/fixtures/prowlarr/recorded`. |
| Newznab and Torznab indexers asked directly | Newznab searches were run against real Usenet indexers. Torznab was not run against a real indexer. The tests use hand-written answers. |
| Built-in sites (YTS, EZTV, The Pirate Bay) | Searches were run against the real sites. The tests use saved and hand-written answers. |
| Built-in site definitions | Each was checked with `axisd --check-definition` against answers its site gave on 2026-11-18. The daemon has not searched the live sites. |
| Installed site definitions | Requests were built for the usable public definitions; none was sent. |
| Jackett's repository on GitHub | The assumptions of the definition fetch were confirmed against GitHub on 2026-11-21. |
| SABnzbd, Deluge | Each has taken a real download through to its import. |
| qBittorrent | Not run against a real qBittorrent (4.x or 5.x). |
| Plex | Not run against a real Plex, for the compatibility report or for the rescan request. |
| Jellyfin | Run against a real Jellyfin 10.11.8 in a container (`make check-jellyfin`), for sessions and for the rescan request. |
| Radarr, Sonarr | The shapes of their answers were confirmed against Radarr 6.4.4 and Sonarr 4.0.20. See "Migration from Radarr and Sonarr". |
| YouTube | Not run against the real service. |
| Imports on a real server | A Usenet download and a torrent have each been moved into a library on the same disk. The copy across disks has not run on real hardware. |
| Conversions on a real server | Trial conversions of real movies have passed all ten checks, encoded by a hardware encoder on a worker on another machine through an sshfs mount. No conversion run has replaced a real library. |
| A VPN | Outbound binding was proven in a network namespace only, not with a real VPN or in a container. |
| Docker | Not run. The examples and checks were run with Podman and podman-compose only. |
| Kubernetes | The manifests were applied once to a kind cluster. |
| Browsers | Pages were checked in headless Chrome. Firefox, Safari and a real phone were not tried. |
| The image workflow | `.github/workflows/images.yml` ran once, for `v0.1.0-rc1`, and failed in the arm64 half, which was then built under emulation. Since then each architecture has a job of its own on a runner of that architecture; that form published `0.1.0-rc2` for both architectures in about eight minutes. |

### Web interface

- At 1024 pixels wide, the Jobs table, a file's stream tables and one Savings table scroll sideways inside their cards. On a phone, wide tables scroll sideways inside their cards; on the episode tables the state column is out of view until you scroll.
- The top row fits on one line from 77rem wide. Below that the tabs take a row of their own. One line at 1024 pixels is not reachable without cutting the search box, the tabs or the user name.
- The touch icon, the pinned-tab icon, the install prompt and the maskable icon were not tried on a device. The contrast test covers colour pairs, not layouts.
- The oxblood, midnight and contrast themes all have near-black page colours and differ in surfaces and accents.
- libsoup answers a path that contains an encoded slash or NUL with a bare 400 before any Axis code runs, so that response carries no security headers. Nothing is served.
- The shell keeps its own copy of htmx-glib's route-matching rules to tell 404 from 405.
- libsoup does not let Axis refuse a connection before it is accepted, time a slow reader, or limit a handler. `deploy/README.md` says what a reverse proxy must do instead. The request limits were not tried behind a real proxy.
- A search page can take up to about 8 seconds when a provider is slow. A manual grab runs inside its web request: up to 60 seconds for the indexer and 15 seconds for the client.
- Live pages (Jobs, activity lines, the library browser, Analysis) were checked by tests that read the markup and the event stream, not in a browser. Events are delivered through one `LISTEN` connection, so events are missed while it reconnects; pages fetch again on reconnect and job state is never lost.
- The dashboard's "Coming in" section and the "Needs attention" card are drawn when the page is loaded and do not follow the event hub.
- The Compatibility entry in the navigation is shown to every user and answers 403 to one who is not an administrator.
- Sizes on the acquisition pages are binary. The conversion Profiles pages and the estimator's sentences write decimal GB.
- Known gaps in htmx-glib that Axis works around: attribute values are escaped only for `"` and `&`; there is no request-size limit (Axis refuses bodies over 1 MiB itself); the cookie parser accepts duplicate names; the router has no hook for unmatched requests; no doctype is emitted.

### Accounts and sessions

- Logout and session revocation take effect in memory at once but are written to the database asynchronously. If the database is unreachable across a shutdown, a logged-out session could be accepted again after the restart. Disabling an account and changing a password are transactional and are not affected.
- The login throttle is bounded in memory, so a large distributed flood degrades to 503 for everyone.
- The cross-site check compares `Origin` with `Host`, or with `X-Forwarded-Host` when the request comes from a listed trusted proxy. Behind a proxy that is not listed, `Host` must be passed through unchanged.
- Over plain HTTP the cookies lack the `__Host-` prefix, so a sibling subdomain could plant a cookie before login. Serve Axis over HTTPS when it is reachable beyond the local network, and set HSTS at the proxy; Axis sends none.
- The setup token is logged at warning level and is not shown when `log.level` is `error`.
- Accounts changed in the database from outside the daemon are not seen until it restarts.
- Request bodies are wiped by htmx-glib when a request finishes. What neither it nor Axis can reach remains: libsoup's socket buffers and the TLS layer.
- A user demoted from administrator has their event streams closed. Every action checks the role again.

### Settings, modules and the setup guide

- Secret settings, download-client secrets, an indexer's fetch address (it holds the indexer's key) and a magnet link (it may hold a passkey) are stored in the database unencrypted. The database credentials sit beside the database in the configuration, so a key on the same host would add nothing. Fetch addresses and magnet links expire with their search.
- A module's `start` hook that blocks stalls the main loop when the module is switched on. A module must be registered after the modules it depends on.
- There is no "reset to default" control for a setting.
- The setup guide's outbound links to other projects' documentation, and to pages of the services you configured, were written from memory and not opened.
- A setup step is done by its layer's own check, which is stricter than the dashboard's checklist in three places, so the dashboard's card can be gone while the guide still asks for a test.
- "Not now" and "I don't use this" in the setup guide belong to the installation, not to an account.

### Database and job queue

- Only PostgreSQL 17 is tested. TLS to the database and multi-host URLs are unverified.
- An ordinary query has no client-side deadline on a silently dead network. PostgreSQL's 60-second statement timeout covers the normal case.
- There is no `lock_timeout`: a statement waits for a lock up to the statement timeout. A deadlock, a serialization failure and a lock wait that ran out are plain query errors that nothing retries, with one exception: cancelling a conversion and a worker's report lock two rows in opposite orders, and the one PostgreSQL fails is retried.
- The start-up and readiness checks read only the migration table. A table dropped or changed outside Axis goes unnoticed.
- Bulk deletes (the identities of a library folder, the folders of a volume) cascade through some foreign keys that no index leads, so they can be slow on a large library.
- Search history (searches, candidates, events) has no retention.
- A full database disk stops PostgreSQL, and Axis can say only that the connection is refused.
- There is no consistency check and no repair command. The runbook in `docs/` says what a person can do by hand, and marks the steps that were not run.
- A commit lost with its connection is reported as "not recorded" although it may have committed.
- A failed commit while a movie's old file is retired can leave that file in the recycle folder with its old index row until the next run settles it.
- A session advisory lock can stay with a pooled connection when its unlock fails.
- The single-daemon lock needs a direct connection or session pooling; it does not work through a transaction pooler. It was tested with real daemons on one database, not under Kubernetes or Compose.
- The daemon refuses to start unless the connection pool is at least the total in-process job concurrency plus two. Raising a queue's concurrency needs a larger pool.
- A job handler that is still running after the shutdown grace period is abandoned and its job is handed back. A module that frees state in its stop hook must tolerate such a late handler.
- Several jobs share queues with a concurrency of one. Searches have a queue of their own, but imports and download tracking share the `downloads` queue, so a long copy across volumes delays tracking. The Analysis refresh (about 40 seconds at 2.6 million files) runs on the `maintenance` queue and delays the volume check, the schedulers and the follow-up of downloads that share it.
- Process-state checks in tests must look once and count the kernel's transitional dead state as gone.

### Volumes and the scanner

- A byte-for-byte copy of a disk, mounted at the volume's path, is accepted as the volume. Changed device and filesystem identifiers are logged, not refused.
- A volume added on a directory that is not a mount point (allowed after a second confirmation) loses the mount-point protection, so a stale copy of the marker there would read as online.
- Only a guard's own calls are safe against a disk that hangs. A job's own `readdir`, `openat` or `read` through a guard's descriptor can still block, and a thread stuck in the kernel cannot be killed and can delay the process's exit. The import's calls into the volume write layer are not on the storage layer's deadline helpers.
- Read intent is a convention; the descriptor itself permits writes. Reads through a guard's descriptors are not proved again.
- A deleted marker shows as a mismatch, and restoring it is refused while `storage.read_only` is true.
- Check timeouts and the 60-second check interval are constants.
- Paths with control characters, invalid UTF-8, or segments that begin or end with a space cannot be volumes.
- A disk added read-only without a marker is recognised by its filesystem's type, its own identifier, its UUID when the kernel gives one, its size and the directory's inode, and only while the directory is not empty. A resize or a new UUID before the marker is written makes the volume offline for good: remove it and add it again. The behaviour of xfs, exFAT, FAT, f2fs, NTFS and FUSE filesystems is taken from the kernel's sources and was not tried on such disks. It was tested with tmpfs disks in a user namespace and in rootless Podman, on one recent kernel only.
- Moves are recognised within one library folder only.
- A new file written at an existing path keeps the old record, marked changed, so later logic must compare inodes.
- A library folder that is genuinely emptied stays "incomplete" until it is removed and added again, because an empty folder with recorded files is refused as suspicious.
- A rescan that finds nothing changed still rewrites every row (about 80 seconds per million files), which adds vacuum load.
- Idle I/O priority has an effect only under the BFQ scheduler. The two-second "just modified" window can miss a change on a network filesystem with more clock skew than that.
- Inode reuse on ext4 and xfs, FUSE filesystems, and an I/O error in the middle of a directory are untested.
- At most 200 findings are kept per scan; the counts stay exact.
- A category folder inside a TV library folder (`TV/Children/<Series>`) is read as one series and is not supported.

### Probe

- Dolby Vision and HDR10+ classification is tested only against constructed fixtures, never real files. Check it against real samples before you let a conversion act on it.
- A Dolby Vision profile 8.2 file whose record ffprobe does not report cannot be told from 10-bit SDR HEVC and is classed SDR. Converting it would keep a valid picture and lose the Dolby Vision layer.
- HDR10+ is read from the first frame only.
- ffprobe is constrained by its arguments (the file passed as a descriptor, the `fd` protocol only, a list of allowed demuxers) and by resource limits, but it is not sandboxed. A memory-safety bug in an allowed demuxer runs with the daemon user's access to files and the network. The same is true of ffmpeg in the sample measurement.
- The fixtures were recorded with ffprobe 8.1. The image ships ffmpeg 7.1, whose JSON output has not been run through the mapper.
- Raw streams that need a file extension to be recognised are recorded as failed.

### Library browser and Analysis

- Folder totals describe the last complete scan, while file lists show the index as it is now. The pages say so.
- Name search needs three characters and the `pg_trgm` extension. Without the extension it scans until its deadline.
- Two list queries exceed the 200 ms target on two million rows: a size range sorted by name, and five filters at once.
- Empty folders are not listed. The file list counts only to 10,000.
- The duplicate matching in Analysis reads names with a deliberately small rule: a movie without a year is never compared, and a remake, an edition or a trailer can still be grouped. It shows evidence and acts on nothing. The Duplicates page under imports works from identified titles and is the one to act on.
- A small movie with "Sample" in its title can be flagged as a sample.

### Details providers and title search

- No test checks TMDb's alternative-title field names, release date types or status words against a live answer.
- Adult filtering applies to search and find results, not to a title fetched by its identifier.
- Image requests share a provider's rate budget with its data requests. Artwork is read whole into memory when served (at most 4 MiB each), and artwork files are created with mode 0600, so a later start under another user id cannot read them.
- After two providers' records merge, the row names the last writer as its provider. Two managed movies whose records later merge make the merge fail, because one title cannot be two movies.
- A title that no module refers to is refreshed only while it was recently viewed.
- A provider's default rate is that of the first instance created in a process.
- A movie shows one release date. "Released" is that date (its first release anywhere, as a rule a cinema's) plus `movies.released_after` (90 days), not a real digital or disc date.
- A key changed after a failure does not reset the provider's back-off.
- TheTVDB publishes no numeric rate limit. Its licence requires attribution, which Axis shows as text in every page's footer without a logo, and it asks each administrator to register a key of their own. Whether per-installation free keys are what TheTVDB intends for open-source tools is not stated in its terms.

### Discover

- Filters choose among the stored snapshot (60 titles a list by default), so a narrow filter can show few titles or none. TMDb's own discover queries are not used.
- "What you have" is every title a module refers to, so a movie on a wanted list counts as had.
- "Not interested" belongs to the installation, not to a person.
- A refresh adds brief title records that nothing refers to, as searches do. Their artwork counts against the artwork cap.
- TMDb has no "coming soon" list for series.
- Recommendations fetched for a title you do not have are dropped by the next refresh, so opening that title's page again asks TMDb again, at most once an hour.
- For a series whose record is TheTVDB's, the recommended titles are TMDb's records. If the library holds one of them under a TheTVDB record of its own, its card does not carry the library's mark.
- The pages were read only by tests, never looked at in a browser.

### Release names

- The parser reads only what a name states. It does not infer a source from a bare resolution, a resolution from an SD source, or a remux from a disc codec. Tools that do infer these file some releases differently, by design.
- SDTV, PDTV and DSR are read as HDTV. There is no HD DVD source. `HDRip` and `1080p+` are not defined and are left as ambiguous.
- A group is read only after a hyphen or in brackets. Not read as a group: a trailing word after a dot or a space with no hyphen, a group with a dot in it, and a site's domain used as a group.
- `Complete` in a movie's name reads as a series pack. A bare episode number (`show.name.611`) is not read.
- A raw Usenet post subject with a part counter and a quoted file name is ruled out. Other decorations of raw subjects are still read into the title.
- One obfuscated form (a long jumble of letters with no digits) escapes the obfuscation test.
- Known weak forms: a title that ends in a plausible year with no release year after it, group names that contain hyphens, two-letter language codes, and episode ranges across seasons.
- The title key keeps articles, so a name without a leading article does not match. Abbreviations, translations and country forms match only through the other names the provider lists.
- The parser follows the numbering written in the name. There is no scene mapping: a release numbered for an episode the series' data does not have is ruled out and counted on the series' page, and such episodes are never found automatically. Following TheTVDB's numbering for the series is the way out.
- Input is limited to 512 bytes of valid UTF-8. The fuzz targets are slow because every word is looked up by scanning the whole table.
- An audit of the parser against a large private set of real names found agreement of about 95 percent for movie titles and 90 percent for episode titles against the recorded title alone, and 97 to 99 percent for resolution and source where stated. Most of the rest used another name of the title. The forms it found misread are fixed and kept as made-up regression lines in `tests/fixtures/release/`.

### Quality profiles

- The built-in profiles and every default size limit are reasoning, never compared with real indexer results. Space saver's limit per minute rejects most H.264 WEB-DL releases on purpose.
- The ladder places DVD, Disc and the recordings (cam to workprint) by their source alone and needs a resolution for everything else. An HDTV name without a resolution is unknown, not SDTV. A DVD disc image that states no resolution is Disc, the top of the ladder. A remux below 1080p is placed as that resolution's Blu-ray.
- A torrent with no seeders is ranked last, not rejected.
- A profile stored by a very early build reads its size limits as mebibytes, so its limits are about 5 percent larger than they were written to be.
- A converted file whose source quality was not recorded is never upgraded. A file of unknown quality is never replaced automatically when its resolution is known.
- "Try a name" matches against no title, says so, and decides as Usenet with no age.
- The players preference considers every device seen playing within the compatibility report's window, with the video combination the name states and each audio codec, never a container.

### Indexers

- Prowlarr: not verified are the bytes of a Usenet release file behind an indexer's redirect, a 429 or 503, `indexerstatus` with a failing indexer, a base path, and `indexerIds` restricting a search. `tests/fixtures/prowlarr/README.md` lists what was verified.
- The address Prowlarr writes in a download link must name exactly the configured scheme, host, port and base path. Behind a TLS proxy every result may be dropped as "not this source's own address" until the configured address matches.
- "3 of 12 indexers did not answer" is built from the indexers Prowlarr reports as disabled (read at most every five minutes), because a search's own answer does not say which indexers failed.
- Following an indexer's download redirect is on by default and can be switched off for each source. It weakens "only the source's own address": a malicious indexer can make Axis fetch from any public https address it names. The fetch is https only, refuses private and reserved addresses at connect time, sends nothing of Axis's, and is never redirected again.
- Newznab and Torznab: written from the specifications. Whether real indexers state `total`, honour `offset` and cap a page below `limit` is not verified. A search reads at most 5 pages and 1,000 results. The API path is always `<base path>/api`; an API whose path does not end in `/api` cannot be reached. At most 48 indexers and sources in all.
- An indexer's key travels in the query, as the protocol requires. The HTTP client blanks it out of its messages.
- Automatic searches pass over an indexer with a tenth of its stated daily calls left; a person's searches use it until none is left. There is no reserve for grabs.
- A Usenet release is merged across indexers by exact name and size; one whose size is not stated is never merged.
- A Prowlarr indexer at the host of an indexer added directly is asked once, directly. This relies on a field of Prowlarr's answer that the recording does not hold.
- YTS, EZTV and The Pirate Bay: believed are the default addresses, the parameter and field names, and the categories. Axis never looks for a new address; a site that moved shows as failing. YTS gives no release names, so Axis builds one from the fields. EZTV is asked by IMDb identifier only. Not verified: that these sites do not answer a program with a challenge page.
- Site definitions run only for sites that need no login. Not supported: logins, a browser helper, POST searches, settings edited per indexer (they take their defaults), a definition's own certificates, and several rarely used keys of the format; a definition that needs one is listed as not usable, with the reason. Of 79 public no-login definitions surveyed in the community's set, 39 were usable.
- The definition format's regular expressions are written for .NET; the engine translates the common differences for PCRE2. Legacy text encodings named by a definition are read.
- Bounds of the definition engine: YAML 256 KiB; pages 8 MiB; three seconds of regular-expression matching an answer. An answer over a bound fails the search.
- The Jackett fetch lists a folder through GitHub's contents API, which is documented to return at most 1,000 files; the folder held 591 entries when confirmed. A list of 1,000 entries is marked on the page as possibly cut. GitHub allows about 60 requests an hour without an account, and a listing costs two. No token can be configured.
- The Jackett fetch runs inside the request that asked for it, for up to 120 seconds, with no progress line. A reverse proxy with a shorter timeout cuts the page off; the files already installed stay installed. The list is not paged, and ticks are not kept from one search of the list to the next. Only one earlier version of a definition is kept.
- Most of Jackett's definitions are of private sites that need a login. Axis installs them and marks them not usable.
- Jackett's definitions are published under GPL-2.0. Axis distributes none of them: an administrator downloads them to their own server, and Axis reads them as data. This is the project's reading and not legal advice. The fetch is withdrawn if the Jackett project objects; definitions already fetched stay.

### Download clients

- `tests/fixtures/sabnzbd/README.md`, `tests/fixtures/deluge/README.md` and `tests/fixtures/qbittorrent/README.md` list every detail believed about each client's API.
- Deluge: a torrent whose answer to the add was lost, or whose daemon stopped before its label was set, is unlabelled and so invisible to Axis. Axis cannot adopt it; the grab fails after the reconciliation wait while the torrent stays in Deluge.
- SABnzbd: a job whose status word Axis does not know keeps the last state Axis saw, which is lost on a restart. Its lists are read 500 at a time; a download beyond them is not seen (and never taken for gone) until earlier ones leave the list.
- qBittorrent: a wrong password is not retried for ten minutes and a ban for fifteen; each attempt counts toward qBittorrent's own ban of Axis's address. No save path is sent with an add. A torrent that is BitTorrent v2 only is listed under a hash Axis does not compute and is never matched. Queue priority is ignored. A torrent that qBittorrent moves to another category after the add is left alone.
- One tracking job polls the clients one after another, so an unreachable client delays the others by up to the 15-second request timeout.
- A Usenet download whose answer was lost is matched again by exact release name, so two identical names in flight at one client could be confused.
- A send whose answer was lost, and that the client turns out not to have, is put on the blocklist as "not taken". This errs towards losing a good release.
- Removal after import by ratio or by seeding time is not implemented. The choices are remove, or leave it to seed.
- A removal the client cannot do is retried by the hourly cleanup without end. A torrent a person already removed at the client shows "tried again every hour" for good.
- A torrent that Axis let go of before a move, whose placement then fails, is no longer at its client. Its files stay in its folder and are listed on the Leftovers page.
- The Leftovers page and a folder's removal ask every other client whose path mappings reach the folder. With one disk mapped whole, every client is every other's, and one that cannot be reached holds back the others' deletions.
- Only download-client connections may POST, besides the media servers' rescan request and the TheTVDB login. That is enforced at run time and by a test that scans the source, not by the compiler.
- A torrent's client downloads into a folder that is inside a library folder, or that is registered with a type other than downloads: every removal leaves the torrent's files there, after an import too, and says why. Register the client's folder as a downloads folder outside the library folders to have them removed.

### Movies

- A search judges each release against the best present file of the movie. The titles a release name is compared with are the movie, with its other names, and the movies of the same title that the provider's search returns. When the provider cannot answer, only the movie itself is compared, which weakens the rule that an ambiguous name is never a match; the search line says how many titles were compared.
- A release whose indexer states another movie's IMDb or TMDb identifier is ruled out by a fixed rule, so an indexer that mislabels costs a good release. A person can still take it with "Grab it by force".
- The blocklist keys a failed release by its info-hash when known, and by source, indexer and folded name. The same name from another indexer is tried again. A person's block matches the name from every indexer.
- The scheduler runs every 15 minutes and queues at most `movies.searches_per_run` searches (5). There is no feed watching.
- Failures, stalls and downloads that are gone are found by a job that runs every minute, so the next search can wait a minute plus whatever else runs on that queue.
- Removing a movie leaves any download of it with its client. Added again, the movie needs attention until that download is gone.
- The upgrade path (a file below the cutoff) is derived and unit-tested but has not been seen to search on a real library.

### Series

- An episode is the provider's own episode id. A provider that gives no ids leaves episodes known by season and number alone; renumbered, those read as other episodes.
- An air date is a date, not a time. An episode is searched from the start of its air date in UTC plus `tv.air_delay` (one hour), which is early for an evening broadcast in the Americas. The calendar's days are UTC days.
- The matcher never guesses. A dated release is matched only when exactly one episode aired that day. An absolute number alone, for a series that is not marked as anime, and an episode range across seasons are ruled out.
- A pack is taken only when every monitored episode it holds is wanted, so a season that is still airing is sought episode by episode. A search of a whole series is by name and takes only packs. A pack is decided against the worst file of the episodes it would replace.
- A standard season pack that lacks a file can be grabbed again for the hole.
- The import reads each file's episodes from its own name only. Files named without a season and episode (`01.mkv`) are left for "Assign by hand", as are a series' extras. A subtitle goes with the video whose name it begins with.
- A partly imported download stays at its client and its grab stays open, so its episodes count as in flight until it is retried and completes, or is dismissed. One import of a series places at a time.
- Specials go to "Season 00".
- A series' search has been seen to mark unaired episodes as searching, which makes the wanted count on a series' page larger than its aired count.
- The series list reads every series' episodes for its counts, order and pages, and sorts in memory. Measured with 1,500 series and 60,000 episode files under the sanitizers: 125 to 260 ms a page. A much larger library would want a stored summary for each series.
- The movies list, ordered by size or filtered by a state, reads every movie's facts, because size on disk and state are derived and not stored. The Wanted pages and the scheduler also read every movie's facts.

### Import

- Tested on scratch directories and on small tmpfs mounts in a private user namespace, with a script standing in for ffprobe. Never run over NFS or SMB.
- Names are checked and then used, in an open directory. Every step checks the inode again afterwards and puts back what it moved by mistake, but a process that swaps a file between the check and the call can still make one step act on the other file of that name.
- A movie split into parts is blocked; there is no stacking. An archive is never unpacked. An `.nfo` is never written. Only extras in the standard folders or with the standard suffixes are imported.
- A copied file is created 0644 less the umask, owned by the daemon's user. A hardlinked file keeps the download's owner and mode and shares its inode with the client's copy: a client that repairs or rewrites the file in place changes the library's.
- The free-space floor is checked before a copy starts, not during it. A copy is never resumed: one stopped by a shutdown is removed and made again from the start. A copy that makes no progress at all (a disk that hangs) is not stopped; its job is abandoned at the end of the grace period.
- A copy is read back after an fsync and a request to drop it from the page cache, which most filesystems honour. Where one does not, the read-back may come from the cache and not the disk.
- An upgrade to the same name links the old file into `.axis-recycle` before the swap. If the import is then dismissed, or a placement fails after that link and is never retried, that recycled name has no removal time and stays until removed by hand. Nothing is lost: it is a second name of the old file.
- Waiting imports, and imports left running for ten minutes by a job that gave up, are queued again by the movies' follow-up job, so only while the Movies module is on.
- That media servers ignore the dot-names `.axis-partial-*` and `.axis-recycle` follows from their documented rules and was not checked against a running Plex or Jellyfin.
- The naming scheme is the one both Plex and Jellyfin read: `Title (Year) {tmdb-N}/Title (Year) {tmdb-N}.ext` for movies and `Show (Year) {tvdb-N}/Season 01/Show (Year) - S01E01 - Title.ext` for series. It cannot express editions, and Jellyfin's `default` subtitle flag is not written.
- The stop during a copy was tested with a real daemon for a movie only. The test needs strace and unprivileged namespaces, or it is skipped.
- Notifications to media servers are recorded after an import is committed, so a crash between the two loses one. The servers' own scans are the backstop. Failures are retried five times over about half an hour and then dropped.

### Identification

- How often real names come out sure, likely or unknown has not been measured after the latest rules. The rules are tested against a stand-in provider with made-up titles.
- TMDb's search is fuzzy and returns 20 results a page. When no answer on a full first page has the name, the second is asked for; a movie beyond it stays unknown, never a wrong link.
- Titles of one name are judged by their full records, at most five read per folder. A year two off is likely, not sure. Three off and more is another title.
- The wider runtime band for the only movie of its name (25 percent, and 60 percent longer with an edition word such as Extended or Director's Cut) is reasoning, not measured.
- No run removes or changes a link. A link that a later look does not confirm stays and is listed as "Linked, now uncertain" until a person keeps, changes or undoes it; until then it counts as linked for upgrades.
- Movies linked under the first version of the rules are not looked at again for a new version, only when something in their folder changes.
- A folder left unknown is not looked at again when the provider comes to know its movie, until something in the folder changes or a person searches for it on its page.
- Parts (CD1, CD2) are linked as copies of one movie; the runtime check takes the longest, so they go to review.
- An identifier from an `.nfo` makes a folder sure only when one of its names reads as that title. No `tvshow.nfo` is read.
- A person's decision belongs to a folder's path. When a folder's grouping changes (a second movie put into a movie's folder), the decision is lost and the new units are looked at afresh.
- A series folder without a year is sure only when exactly one series of its name fits its episodes. A daily show without a year in its folder name waits for a person.
- At most 5,000 videos of one series folder are read.
- Episode files are placed by their own names. A season folder's number is never combined with a bare episode number (`Season 01/05.mkv` is held).
- Every run of a library folder aggregates all of that folder's videos twice before it looks at up to `identify.units_per_run` (25) of them; that cost grows with the folder and was measured only on test trees.
- Whether TMDb's movie search answers a regional release date when asked with a region is settled only by its documentation, which Axis follows by sending no region.

### Players and media servers

- Plex: a fully direct play, a converted video stream and a subtitle conversion were never observed, so their fixtures are constructed. Plex states no reason for a conversion, so the reason is inferred; a user's quality setting that forces a codec change counts against the device, which is the safe direction.
- Past plays carry no playback decision, so every verdict starts as unknown and improves only as sessions are observed.
- Over plain `http` the media server's token crosses the network in clear. The redirect check compares host names as text. A server, or someone spoofing it on the network, could feed false direct-play evidence.
- History import reads at most about 10,000 plays a run.
- Jellyfin: 7 of the 27 reasons it can state for a conversion were seen; the other 20 are mapped from their names. It lists a conversion only while it runs and has no play history for each device, so a device is considered for a library only once it is seen playing it.
- Plex is asked to rescan once for each folder, Jellyfin once for up to 32 folders. The Plex library section is chosen by the stored library locations, read hourly. Plex's automatic detection does not work on most network mounts, which is why the request matters there.

### Estimates and profiles

- Every number in the estimator's table of priors is reasoning about typical encoder behaviour, not a measurement. Estimates are labelled uncalibrated until real samples replace them. Run the sample measurement on real files before you trust a figure.
- Nothing is fed back into the estimator from finished conversions. The ratio of real size to estimate for each revision is kept as a view (`conversion_estimate_seen`) for that.
- The Maximum compatibility target converts almost nothing as configured, because "already efficient" reuses the Analysis threshold for H.264. That threshold needs its own value for this target.
- The compatibility verdict is stored when a profile is assigned and can go stale; a conversion computes it again.
- The test that no profile definition can reach a fixed rule compares the eligibility code with an oracle written by the same author, so a shared misreading of a rule would not be caught.
- In a large generated library the music Space saver profile converted no files. The cause was not confirmed; check on real music before building audio conversion.
- Lists filtered by a target or a profile are slow on very large libraries (seconds at 770,000 files in a sanitizer build), and some miss their deadline at 2.4 million files.

### Conversion arguments and encoders

- The hardware encoders (`hevc_nvenc`, `h264_nvenc`, `hevc_vaapi`, `h264_vaapi`) are run by the tests only on request (`make check-hwenc`). They have produced files with ffmpeg 8.1 on one-second clips, where both HEVC hardware encoders kept 10 bits, colour and exact HDR10 values. They have not run with ffmpeg 7.1 or inside the container image. Each worker's capability check decides what it offers.
- The hardware encoders' quality mappings are priors, never compared for picture quality.
- No encoder option states HDR10 values for the hardware encoders. They pass on what the source frames carry and write it at keyframes but not in the stream header; players read the container's values, which are present.
- Found by experiment and worth remembering: MP4 `+faststart` with output to a descriptor writes a corrupt file and still exits 0 (not used); the `-color_primaries` and `-color_trc` output options carry nothing (a filter on the frames does); x265 told that a file is HDR10 without light levels writes zeros.
- The image is about 560 MB, nearly all of it ffmpeg and its dependencies. Its ffmpeg has the NVENC encoders compiled in; using them needs GPU pass-through to a worker.

### Conversions

- The automated tests convert generated four-second clips on tmpfs with the software encoders, with a worker on the same machine as the same user. The first real replacements should be titles that could be downloaded again.
- The check of copied tracks compares every copied track and attachment bit for bit. Tracks that a plan makes anew (a stereo AAC track, a subtitle converted to another text format) are compared only by their streams, their length and a clean decode, so a silent stereo track or a subtitle whose words changed would pass.
- The checks decode both files in full, and one reads the original once more. For a 40 GiB original that is several minutes of one disk at idle I/O priority. Their cost on a feature-length movie was not measured in the tests.
- The picture comparison pairs frames by the nearest timestamp, which needs ffmpeg 6.1 or later, and ignores each window's first and last frame. Its floor of 0.85 was chosen on test clips. VMAF is not used.
- An AAC stream that the `aac_adtstoasc` filter refuses fails the check, which keeps the original.
- A "later" refusal when a worker takes the work (storage read-only, the volume away, too little space) uses one of the encode job's three attempts.
- That an attempt ended reaches the conversion only at the tidy job's next run (every 15 minutes, at start-up, and when a volume comes online). Until then the conversion reads "Being converted on a worker" while its job waits in the queue.
- A worker stopped by `SIGSTOP` leaves its encoder running in another process group. It goes on writing its own output in the work area until the worker continues and stops it.
- A worker that runs as another user than the daemon cannot write in the work area, which is the daemon's own with mode 0700.
- A conversion's finish job stopped by `SIGTERM` in a real daemon was not tested. Its placement is stopped at every step in-process.
- A file Axis made from an SDR source counts as SDR for the player verdicts only while its inode, size and modification time are those Axis settled. Eligibility and the Library pages still show such a file as HDR uncertain.
- A trial rejected for a reason that is not the profile's can be set aside by an administrator.

### Conversion runs

- A run's list is a snapshot. A file that became eligible later is not added, and a file that is no longer eligible is skipped when reached.
- Not built: the space projection for each volume, orderings other than by saving, a read limit for each disk, standing runs that pick up new files, and estimates corrected from results.
- One run asks for files at a time, at most 8 at once. The connected workers' capacity is the sum of their "jobs at once", whatever else they are doing.
- The step runs every minute on the `maintenance` queue, so the next file can wait a minute plus whatever runs there.
- The unit for TV is the series that identification linked. A TV folder that is no series is one unit. Two copies of one movie are two units.
- Hours are the server's local time. A time budget counts paused time.
- A run pauses itself after three failures in a row, twenty refusals in a row (a constant), an encoder crash, or a verdict that is no longer safe.
- Tested against generated libraries of up to 50,000 files and 1,500 series, with the tests standing in for the worker's results.

### Workers

- Wrong worker tokens are throttled by client address. Behind a reverse proxy that is not listed in `server.trusted_proxies`, every worker shares the proxy's address, and one misconfigured worker can get the others throttled for a time.
- A worker token is a bearer secret. Whoever holds it can claim work and read job descriptions (paths relative to a volume, stream facts), though not media. The daemon speaks plain HTTP, so across a network the token needs TLS at a reverse proxy. The worker refuses unencrypted addresses that are not loopback unless told otherwise.
- A worker reaches media through a mount that is checked by content. A mount that hangs when the worker starts does not block its registration, and a job whose thread does not end is abandoned safely. A worker pauses itself after an encoder crash.
- A worker cannot yet limit ffmpeg's threads or decode on the GPU, so a software decode can saturate a weak CPU. See the roadmap.
- The Compose example's worker service has not been started by a Compose front end.

### Deployment

- Not run anywhere: Docker and Docker Compose, the `docker run` form, a system-wide Quadlet install, a certificate from Let's Encrypt in the Traefik example, and any Compose front end but podman-compose.
- On a host with SELinux the examples need `label=disable`, and rootless Podman needs the data folders given to the container's ids. Both are documented beside the examples.
- Through rootless Podman's port forwarding every client reaches a container from the container network's own address, so the client address that a proxy reports is right only for clients on that network or with rootful networking.
- Kubernetes: the Ingress and the NetworkPolicies were applied but not exercised, the worker was not applied, and the Traefik IngressRoute is written from the documentation. Some provisioners make every volume writable by everyone, which the daemon refuses for its data directory; the example points the data directory at a subdirectory that the daemon makes.
- An arbitrary user id was run only within rootless Podman's subordinate range, not in OpenShift's range.
- The image workflow names its actions by major version, not by commit, and builds each architecture in a job of its own on a runner of that architecture. Images for amd64 and arm64 can also be built and pushed locally with `deploy/images.sh`.
- The first administrator's password, when given as `AXIS_ADMIN_PASSWORD`, stays in the process environment. The `_FILE` form avoids that.
- Logs, JSON or not, go to standard error.
- `/metrics` sees what the shell answers, not requests that libsoup refuses itself, and counts event streams without timing them.
- The documented restore goes into a database created empty. Never restore over a database Axis has used. The examples create the database with data checksums; an existing database needs `pg_checksums --enable` with PostgreSQL stopped. There is no point-in-time recovery in the examples.
- An older backup restored over disks that moved on loses no file, but titles are downloaded again, imports block, and recycle and partial files are orphaned.

### Outbound binding

- Name lookups are not bound; they use the system's resolver. The database, loopback and `axis-worker` are not bound either. Under the default scope only indexers and details providers at public addresses are bound, judged by the address connected to, so a name that resolves to a private address is reached directly.
- Before Linux 5.7 a process without `CAP_NET_RAW` cannot bind to a device, and then every bound connection is refused.
- IPv6 through the interface needs an IPv6 route out of it.
- The VPN container lines of the full-stack example are written from that container's documentation and were not run.

### Needs attention

- Every notice is an administrator's. A user who is not an administrator sees no bell, no page and no "Needs attention" card on the dashboard.
- The waits before a notice appears are constants: a disk 10 minutes, a download client or an indexer 30 minutes (an indexer also needs a second failed contact), a media server 30 minutes and two checks, a silent worker 10 minutes. Only the low-space share (`storage.low_space_percent`) is a setting.
- The counts for movies and series, including the line for releases that wait for "Grab this one", are made only after a search or an import ended, at most every 5 minutes, and every 10 minutes otherwise. They can be up to 10 minutes late.
- The duplicates notice reads a number that the hourly import cleanup leaves, so it can be an hour behind the Duplicates page. The leftovers notice is as old as its hourly count.
- A dismissal lasts while its notice lasts. A count that drops without reaching nothing keeps its dismissal, so 12 dismissed, 5 left and then 12 again (others, with no higher id) stays hidden.
- A notice that is kept because a source could not tell is as old as the last reading that knew. A disk that is away keeps its last "almost full" notice and figures.
- A finished download that cannot be imported has a notice 10 minutes after it last grew. The notice names no cause. While any disk is away or its client is not working, that notice is neither made nor ended.
- A release that only a person can grab reaches its own line only when the last search of that one episode found it; a search of a season or of the whole series does not. The line stays after a person's grab fails until the episode is searched again.
- An indexer that is never contacted keeps its last state, and its notice, until a person tests it.
- A worker switched off on purpose is "not heard from" for a day, and is then taken as switched off.
- The failed-jobs notice counts every failed job that is still kept, however old.
- Files that were not found are a notice although they end by themselves.
- The notice for imports that wait for Read-only storage always has the number 1, so a dismissal holds while more arrive.
- A details provider's refused key is known in memory only, so its notice, once dismissed, is shown again after a restart while the key is still refused.
- A reading has no overall deadline: against a database that does not answer it takes the sum of its sources' deadlines, about 10 seconds.
- No test: a disk not recognised at its path, a disk mounted read-only, a marker that cannot be written, a conversion run that paused itself, and the cleanup step that writes the duplicates count.
- Measured under the sanitizers: the movies count takes 55 to 58 ms on 5,000 movies; the series count takes about 1.2 seconds over three readings on 1,500 series with no files, grabs or searches. A library whose episodes have them was not measured.

### Deleting and replacing downloads

- "Delete this download" exists only for an import that a download of Axis's brought. A folder that a person named for a manual import stays the Leftovers page's.
- A torrent that its client no longer has is deleted by Axis only when the client keeps Axis's label or category in a folder of its own. Without such a folder Axis deletes nothing itself, not even a single file.
- A torrent that its client still has is refused when the place the client reports is not named as the torrent is (a torrent with no folder of its own, or one renamed at the client), or when it was moved in the client after it completed. Remove it at the client.
- A few milliseconds pass between Axis's last look and the client's removal. A torrent moved in its client in that moment is deleted where it is. This applies to "Delete this download", to a replace and to "Give up on this download".
- What a client reports as a torrent's place is believed. What qBittorrent and Deluge report for an unfinished torrent, and for one with no folder of its own, is assumed and not observed.
- The place rule of a replace reads paths as text through the path mappings. A symbolic link on the way is not detected. A torrent client that downloads to a folder that no path mapping reaches gets every replace of a torrent refused until a mapping is added.
- Every removal of a torrent with its data passes one check in the downloads layer: "Give up on this download", "Block and search again", an ordinary failure, Remove on the Downloads page and the removal after an import remove a torrent without its data when its client says it is in a library folder or at the top of a disk. A place that cannot be judged (no mapping reaches it, or the client does not answer) is removed with its data.
- After a deletion, a search can grab the same release again unless the blocklist box was ticked.
- A replaced download that finishes where its client is not asked to delete it is left at the client, and stays there until someone removes it.
- A movie gets no offer to replace while another live download of its title that no grab follows is at a client. Series have no such check.
- The Downloads page reads at most 2,000 blocked or partly imported imports for its links to "Delete this download".
- No test: a disk mounted read-only at the deletion; a file under a temporary name at the moment of the deletion; a download that completes under the lock while a person confirms a replace; a new release that the indexer, and not the client, fails to deliver; two replace posts at once; for series, a replace by force, a download at a torrent client and the "left at the client" outcome.

### Anime

Anime support is for a well-known series that is found, downloaded and numbered by season as the media server numbers it. It is not a collector's workflow.

- A series is anime only when a person marks it, or when Sonarr said so at migration. Axis never marks one by itself. The one-step Add on Discover cards and search results always adds a standard series; use "Add with options" to mark a series as anime.
- Absolute numbers are TheTVDB's own, and are used only when each is on one episode and they run in order. A series must follow TheTVDB's aired order to have them. A series whose files are already on disk keeps its numbering when you mark it as anime; change its numbering afterwards.
- TheTVDB's `absoluteNumber` (on every episode, 0 on specials, unique and rising) was confirmed on two series only.
- Not confirmed against real indexers: whether a text search for `Title 137` finds `[Group] Title - 137`, how the number should be padded, and whether the TV category already returns the anime category's releases. Each anime episode search makes up to three requests at an indexer, which matters on a small daily allowance.
- Under a name that is not the series' title or original title, the low-numbered releases of a series with several seasons are listed but never grabbed by a search. A person grabs them with "Grab this one".
- Some whole-series names are refused as part names: a name that contains a shorter name of the series followed by more words, and a romanised name that starts with a short other name. A season's own name that contains none of the series' names cannot be told from a whole-series name.
- Two ways past the name rules are known and stand. A sequel whose name differs from the series' title only by a hyphen, a colon, a comma, a dot or an underscore counts as the title. A part of the series that TheTVDB files as two seasons can have a high number mapped to the wrong season.
- Refused although some may be fine: a mixed form such as `S2 - 05` or `S01E137`; a name with a year before the number, even the series' own year; `Title 01-36 Complete`; an episode whose own title holds words such as "(Part 2)"; a half episode; specials and OVAs by number.
- A bare number or range after the title with no hyphen is a person's grab only.
- A download grabbed by absolute numbers is held to its claim whole. An extra video (a sample, a creditless opening) in a batch that also lacks an episode blocks the whole import, which then waits for "Assign by hand".
- A batch that lacks a file is imported, and that release, by its exact name, is not grabbed again while the episode has no file. The same release under another name can be.
- The comparison of Sonarr's absolute numbers with TheTVDB's is made only by the migration run that creates the series.
- Not in it: an anime details provider, dual-audio and subtitle preferences, preferred groups, absolute numbers under TMDb's numbering, a person's own correction of a number, and checking the CRC in a file's name.
- No test: a range pack through a real search and import; a word request that fails; a definition-driven site end to end; the scheduler's own run for an anime series; the pages in a browser.

### Migration from Radarr and Sonarr

- The answer shapes were confirmed against Radarr 6.4.4 and Sonarr 4.0.20. Sonarr 3 was never read.
- Still believed: a queue record's `movieId` in Radarr and `seriesId` in Sonarr, and whether a season pack is one record; addresses with a URL base; `tmdbId` and `tvdbId` where the app has none (0, null and absent are all taken as none); the `minimumAvailability` values `tba` and `preDB`; the series type `daily`; the `monitorNewItems` value `none`; quality names other than the ones seen; rate limits over thousands of requests in a row.
- Believed and central: that Sonarr's season and episode numbers and its episode identifier are TheTVDB's aired order and episode id. If that is wrong, a run links nothing and reports every file; it does not link files wrongly.
- A Sonarr cannot be read at all without a TheTVDB key in Axis. See the roadmap.
- When a download queue's answer does not say which titles it is for, "Start searching" holds nothing back and only warns. A run ticked to start its own searches then starts nothing; that path has no test.
- Nothing in the conversion code reads the "kept" mark, so a conversion can replace a kept, migrated file. Whether that is wanted is undecided.
- A reading and a run can be in progress for one connection at once. A reading that completes replaces the plan, and the run then stops, keeping what it migrated.
- After "Forget" on a connection, its held titles can only be started one at a time, the action that lets Axis upgrade its files is gone, and Axis no longer remembers which titles a person removed.
- The TV list has no "Held" mark. The connection's page has no cancel or continue button: cancel is on the Jobs page, and to continue you confirm again.
- The preview does not say before the run how many episodes will not match.
- An app that uses Windows paths gets "Path cannot be mapped".
- A movie that Radarr lists without its file record is reported as having no path or size; the separate request for it is not made.
- Daily shows are migrated as any series; matching them by air date under TheTVDB's order is not proven.
- A reading holds the details worker for up to one hour. Limits of a reading: 250 records a page, 400 pages, 5,000 series, 4 requests a second.
- Not migrated, by design: the blocklist, download clients, indexers, custom formats, tags, lists, history, and anything of Lidarr, Readarr or Prowlarr.
- No test: the links pages and the Duplicates page with thousands of migrated links; handing back a migrated movie file; a daemon restart in the middle of a run; a series that Sonarr does not monitor.

### Grab by force, Assign by hand, links by hand and Duplicates

- A forced grab teaches the matcher nothing. The same release is ruled out again by later searches.
- A forced grab for a series is refused while any import of the series is open. A closed forced grab stays bound to its download.
- Assigning a completed download that has no open import works for Movies only; there is no manual import page for a series.
- A partly placed forced pack ends as imported. A file it left says to assign it by hand, but that import does not offer "Assign by hand".
- On "Assign by hand" you type episodes (`S01E03 S01E04`); you do not pick them from a list. A download that holds several movies cannot be assigned in one step. The review uses the last run's records and does not read the disk again.
- "Link to a movie or episode by hand" for a TV file opens the series' links page, which lists the file only when it is within that series' reach. A TV file outside every series' reach cannot be linked this way.
- The quality of a file that had no link before comes from identification's record only, not from its name. If identification never read it, its quality is unknown.
- The Identify page does not flag a link made by hand that disagrees with what identification reads. A folder can show as linked to one title while its file is linked by hand to another.
- The links pages never rename a file, so a media server may still show the file by its name.
- Duplicates: the subtitles of a removed file stay in place, there is no restore action, and a recycle record whose file is in neither place is retried at every cleanup. At most 20 files are removed by one post.
- A duplicate removal waits for the movie's lock, while the movie links page gives up at once and says to try again.
- No test: the 20-file limit; two sessions racing for the files that stay; a links change racing an identification run; several refusals (a pinned file, an open import or grab, read-only storage) were never seen to fail with their check switched off.

### TheTVDB and a series' numbering

- A series follows one numbering at a time: TMDb's, or TheTVDB's aired order. TheTVDB's other orders are not offered. A new series follows TMDb unless it was migrated from Sonarr or added as anime; there is no default setting.
- TheTVDB is a secondary provider. It never makes a title of its own, so a series that TMDb does not know cannot be added through it. It is not searched by name and not used by identification.
- TheTVDB's artwork is never shown, because its terms grant no right to display images.
- That indexers and release names follow TheTVDB's aired order is inferred, not documented.
- When a series changes numbering, episodes are mapped by remembered identity, then by air date and title. An episode that the two providers date one day apart is not matched by its date, and titles in different languages do not match. Such rows are flagged or left unmapped on the confirmation page.
- Files are never renamed after a change of numbering. A file named in one numbering and linked under the other keeps its link until a person places it by its name. Files that Axis imported are left out of "Place all" and are placed one at a time.
- A change of numbering is refused while a grab, an import or a search of the series is open.
- The migration that keeps episodes for each source is one-way. To go back to a build from before it, restore the backup taken before the upgrade.
- The key and the optional PIN can be set only on the Settings page, not from the environment or a file.
- A series that follows TheTVDB keeps working without the key, and says why it is not refreshed.

### YouTube

- Not run end to end against YouTube. Unchecked: a real fetch with its progress, the name of a merged file, a video served as one file, whether yt-dlp accepts the distribution's Node.js, whether the download that ffmpeg makes goes through a SOCKS proxy, and whether `/proc` is mounted where Axis runs. The options and the shape of a lookup's answer were checked against yt-dlp 2026.10.26 without a fetch.
- Failure reasons are told apart by matching yt-dlp's error text, so a new yt-dlp release can change them.
- When YouTube changes, yt-dlp breaks. There is no update button: the fix is a new image, or your own copy named in `youtube.ytdlp_path`.
- The image takes the newest yt-dlp at build time unless `YTDLP_VERSION` pins a release, so two builds can differ. The build checks yt-dlp against the checksum list of the same release; the release's signature is not checked.
- yt-dlp is not sandboxed. It can read whatever the daemon's user can, and it can connect anywhere when no outbound binding is set. Plugin folders outside its home are not checked.
- CPU, memory, disk and time were not measured, so no memory limit is set on yt-dlp.
- One video at a time: no playlists, channels or following, no quality profile (only a height cap), and a fetch that was killed is not retried.
- A video that needs an account (private, members-only, age-restricted) is not fetched. No cookies are used.
- The media server is not told to rescan after a fetch. The folder layout by channel and year was not confirmed in Plex or Jellyfin.
- A pause after YouTube refuses the server, and a block by the outbound binding, are shown only on the YouTube pages, not under "Needs attention".
- With a `socks5://` proxy, YouTube's host names are looked up locally. Use `socks5h://` to keep lookups at the proxy.
- Whether fetching a video is allowed by the service's terms and by law where you live is the administrator's to answer.

### Requests

- Only the first slice of `add-request-portal` is built: requests inside Axis's own pages. A person who asks is an ordinary user, who still sees the other pages of Axis (file names, release names, downloads). The requester role, the portal API and the portal program are not built.
- Not reviewed by someone who did not write it. No request has been made in a real browser or against a real indexer, download client or library.
- An approval records the decision first and adds the title after. A daemon that stops in between leaves an approved request with no title; it reads as "could not be added" two minutes later, and an administrator selects "Try again". Nothing retries it by itself.
- A title's state costs up to 8 statements for a page of cards, and one of them lists every account (to know whose limit is 0). That is right for a household's accounts and was not measured with hundreds.
- The Request button on a Discover card is drawn by the same code as on a search result's card, but no test opens a Discover page as a user.
- A series on disk that Axis does not manage does not read as available; a movie does.
- "Aired" is by the UTC date, without the TV module's delay after airing.
- A declined request for several seasons stays declined for the seasons that a later request does not list.
- When a request with followers is withdrawn, only the earliest follower is offered it.
- The limits count over a calendar window in the server's local time. The days of 23 and 25 hours are tested for one time zone, and only where the system has its data.
- Changing the four placement defaults is four separate writes of settings, not one.
- The people's switches and limits are on a page of the Requests module, not on the Users page, which links to it.

### User guide

- Not every page has a "Verified" note yet, and there are no screenshots. `docs/SCREENSHOTS.md` lists the images the pages need; `tests/tools/axis-demo.c` and `tests/tools/axis-screenshots.py` make a demo instance with made-up titles and take them.
- Not written: pages for Plex and Jellyfin, a VPN interface, and troubleshooting by symptom.
- In the database runbook, the sections on backup, checksums and restore are tested by `make check-examples`. The sections on finding out what is wrong and on repair mark each step that was not run.

### Wording of the interface

- The interface's labels were renamed in one pass so that each says what it holds. The source was not swept afterwards for the old string of every rename, so a few may be missing. Known: the Files page's folder list still labels its row view "Overview" where the Movies and TV lists say "List", and the Space savings measurement status shows a profile's short name and a raw job state.
- The renamed pages were not checked in a browser, and the longer labels were not checked against the table column widths at phone width.
- Specifications use "volume", "root" and "revision" as concept words where the interface says "disk", "library folder" and "version".

