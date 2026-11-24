# Back up, restore, and repair the database

> **Note.** The backup, checksum, and restore sections are tested (see [Verified](#verified)). The sections on finding out what is wrong, repairing, and running one Axis per database come from a review of how Axis behaves when its database fails. Where a step says **Not run**, treat it as untested.

Axis keeps everything that it cannot fetch again in its PostgreSQL database: accounts, settings, the passwords and keys of your indexers and download clients, your movie and series lists, the reviews and trials you confirmed, and the history of your downloads, imports, and conversions. This page shows you how to back up that database, check a backup, restore it, choose how long Axis keeps its history, start again from an empty database, and find out what is wrong when the database is the problem. Allow about 15 minutes for a backup and a restore of a small library.

The commands use the names of [the Compose example](../deploy/examples/compose/README.md): the services `postgres` and `axis`, and the database user and database `axis`. If you run PostgreSQL yourself, run the same programs (`pg_dump`, `pg_restore`, `dropdb`, `createdb`, and `psql`) against your server.

## Before you begin

You need:

- A running Axis and its PostgreSQL ([Install Axis](01-install-and-look-read-only.md)).
- A place for the backup on another machine or disk.
- Access to the logs of both containers: `docker compose logs axis` and `docker compose logs postgres`.

> **Caution:** A backup holds every secret Axis knows in readable form: indexer keys, download client passwords, and the hashes of account passwords. Axis stores them that way on purpose, because the database password sits beside them anyway. Store and send the backup file the way you store those secrets.

## What a backup protects

The following table says what you lose without a backup, and what Axis can rebuild by itself:

| What | Rebuilt without a backup? |
| --- | --- |
| Accounts, sessions, and settings | No. You create them again. |
| Indexers, download clients, path maps, and workers | No. You add them again, with their keys and passwords. |
| Movies and series you added, quality profiles, and blocklists | No. |
| Identification reviews, conversion profiles, and the trials that proved them | No. |
| The history of downloads, imports, and conversions, and the files marked as not worth converting | No. |
| Your disks | Partly. A disk that carries an Axis marker can be added again; you add its library folders again. |
| The file index, what is inside each file, and the analysis | Yes. Axis scans and probes again. |
| Titles, posters, and media server evidence | Yes. Axis fetches them again. |

The data directory, `/var/lib/axis`, holds only artwork that Axis can fetch again, so it needs no backup.

## Back up the database

1. Run `pg_dump` while Axis runs. A dump reads one consistent snapshot, so you don't need to stop Axis:

   ```sh
   docker compose exec -T postgres pg_dump --username axis --format custom axis > axis-2026-11-17.dump
   ```

2. Check that the file is a whole backup. `pg_restore --list` reads the file without a database:

   ```sh
   docker run --rm -i docker.io/library/postgres:17 pg_restore --list < axis-2026-11-17.dump | grep -c "TABLE DATA"
   ```

   The count is the number of tables in the backup, 98 at schema version 0047. The listing starts with `; Archive created at` and the time of the backup.

3. Copy the file off the machine. A named volume is not a backup: `docker compose down -v` deletes it.
4. Take a backup before every upgrade, and keep it until the upgrade has proved itself. An older Axis refuses a database that a newer one has migrated, so going back means restoring that backup with the older image. Migration 0057, which keeps seasons and episodes per source of numbering, is one-way: to go back past it, restore the backup you took before the upgrade.

**Run:** steps 1 and 2 run in `make check-examples` against the Compose example, through `podman compose`, with Axis running.

## Check for data checksums

PostgreSQL can keep a checksum of every page it writes, so that it notices a page that the disk damaged. A dump does not notice a damaged page, so checksums protect your backups too. The examples create new databases with checksums on (`POSTGRES_INITDB_ARGS=--data-checksums`). The setting applies only when the database is first created. A database that you created with an older version of the examples keeps what it has.

1. Ask the database:

   ```sh
   docker compose exec postgres psql --username axis --dbname axis --command "SHOW data_checksums"
   ```

   A database created by the current examples answers `on`. An older one answers `off`.

2. If the answer is `off`, choose one of two ways to turn checksums on:

   - Dump and restore into a new database. Back up as in [Back up the database](#back-up-the-database). Then stop Axis and PostgreSQL, remove the database volume, add `POSTGRES_INITDB_ARGS: --data-checksums` to the `postgres` service, start PostgreSQL, and restore as in [Restore a backup](#restore-a-backup), skipping the drop and create. The new volume is initialized with checksums.
   - Stop PostgreSQL and run `pg_checksums --enable` on its data directory, for example with `docker run` and the `postgres:17` image on the stopped volume. It rewrites every page, so allow time in proportion to the database size.

3. Checksums find a damaged page when PostgreSQL reads it. They do not replace a backup. PostgreSQL can also archive its write-ahead log for point-in-time recovery; the examples do not set that up, and PostgreSQL's documentation describes it.

**Run:** step 1 runs in `make check-examples` and expects `on`. **Not run:** step 2, both ways, and step 3.

## Restore a backup

> **Warning:** Restore only into a database that you have just created empty. Don't restore with `pg_restore --clean` into the database Axis has used. If a newer Axis migrated that database after the backup, `pg_restore` cannot drop the tables that newer tables refer to. It ignores those errors and leaves the later data in them, and the older Axis then starts on a mixture of the backup and the later data. The review showed this. [The deployment notes](../deploy/README.md#backing-up-the-database) give the same restore as this page.

1. Back up the database as it is now, as in [Back up the database](#back-up-the-database). If the backup you restore is not what you expect, this one lets you go back.
2. Stop Axis:

   ```sh
   docker compose stop axis
   ```

3. Drop the database and create it again, empty:

   ```sh
   docker compose exec postgres dropdb --username axis --force axis
   docker compose exec postgres createdb --username axis --owner axis axis
   ```

4. Restore the backup. `--exit-on-error` makes `pg_restore` stop at the first error instead of going on:

   ```sh
   docker compose exec -T postgres pg_restore --username axis --dbname axis --exit-on-error < axis-2026-11-17.dump
   ```

   No output means it worked.

5. If the backup is older than your disks, switch on read-only storage before Axis starts, so that nothing is imported or converted until you have looked. Axis reads its settings when it starts:

   ```sh
   docker compose exec postgres psql --username axis --dbname axis --command "INSERT INTO setting (key, value) VALUES ('storage.read_only', 'true') ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value"
   ```

6. Start Axis with the image whose version made the backup, or a newer one:

   ```sh
   docker compose start axis
   ```

7. Watch the log for `database schema is up to date at version` (or `database schema migrated from version`), and then `listening on`:

   ```sh
   docker compose logs --follow axis
   ```

8. Check that Axis is ready. The answer is `{"status":"ready"}`:

   ```sh
   curl http://127.0.0.1:7800/readyz
   ```

**Run:** steps 2, 3, 4, 6, and 8 run in `make check-examples` against the Compose example: after the restore, a table made after the backup is gone, Axis is ready, and the administrator of the backup logs in. The review also ran steps 2 to 4 and 6 to 8 with `podman exec` against `postgres:17`. **Not run:** step 5 with `true` (the same statement with `false` ran, and Axis read the value without a warning), and step 7.

### After restoring an older backup

If your disks changed after the backup, expect the following, and scan every library folder before you switch read-only storage off:

- Axis never deletes or converts a file because of the older records: it compares each file on disk with what it recorded before it changes anything.
- A download that waited to be imported when the backup was taken is gone, so Axis wants its title again and downloads it again. The new import then stops with a name that is taken, because the file is already in the library and the backup does not know it.
- Files that Axis moved to `.axis-recycle` or left as `.axis-partial-` files after the backup are not removed: Axis removes only what it recorded.
- A disk that you removed from Axis after the backup is back, because its marker stays on the disk.
- Everything you decided after the backup is gone: accounts, settings, reviews, trials, and conversion runs.

These come from reading the code; the review did not run them.

## Choose how long Axis keeps history

Axis records what happens to each movie and series, each release it grabs, each download, and each import. By default, Axis keeps these records for a year. The job **Deleting old history of movies, series, downloads and imports** runs every hour and deletes the records that ended longer ago than the time you set. To keep a kind of record forever, set its time to `0`.

To choose how long Axis keeps history:

1. Open **System**, then **Settings**, and find the **History settings** card.
2. Change the durations, then select **Save History settings**.

The following table describes the settings:

| Setting | Default | What it keeps |
| --- | --- | --- |
| **Keep the history of what happened to movies and series for** | `365d` (a year) | The events on a movie's or a series' page and on the **History** pages, counted from when each happened. |
| **Keep the record of each release grabbed for** | `365d` (a year) | The record of each grab, counted from when the grab ended. |
| **Keep finished downloads on the Downloads page for** | `365d` (a year) | The downloads that finished, failed, or were removed, counted from when each ended. |
| **Keep finished imports on the Imports page for** | `365d` (a year) | The imports that were imported or dismissed, with their files and decisions, counted from when each ended. |

Each setting takes a duration from `30d` to `3650d`. Enter `0` to keep that kind of record forever. A change applies at the next hourly run, and you don't need to restart Axis.

Axis never deletes a record that something still needs, however old the record is:

- Anything that is still in progress: a grab that is open, a download that is still at its client, and an import that waits, runs, is blocked, or is partly imported.
- The 30 latest events of each movie and series, so that a title's page shows what it showed before. Axis also keeps the events that say when a title was added or removed, and every event of something that a person did.
- The 5 latest grabs of each movie and series, a grab whose release is on a blocklist, and a series' grab whose import left episodes without a file. Axis reads that last kind of grab so that it does not grab the same release again.
- The import that each file in your library came by, an import that moved a file to the recycle folder, the latest import of each movie and series, and an import with work left at the download client, such as a torrent that you left seeding.
- A grab, a download, or an import that another kept record still names. Axis deletes a download only after the grabs and the imports that name it are gone.

Each run deletes a limited number of records at a time, in short steps, so that pages and imports do not wait for it. After an upgrade, a large backlog of old history goes over several runs: each hourly run deletes at most 50,000 events of series, 50,000 events of movies, 50,000 grabs of series, 50,000 grabs of movies, 12,500 imports, and 50,000 downloads, and the rest waits for the next run.

**Run:** each rule above runs in `tests/test-history-retention.c`, and the runs on a generated database of 3,000,000 events run in `tests/test-db-scale.c`. **Not run:** the hourly job on a real server.

## Start again from an empty database

If you lost the database and have no backup, Axis starts on an empty one and builds its schema.

1. Stop Axis, then drop and create the database as in steps 2 and 3 of [Restore a backup](#restore-a-backup). If PostgreSQL itself is gone, remove its volume instead and start it again.
2. Start Axis. The log says `database schema migrated from version 0000 to` the current version, and then `no account exists yet` with a one-time setup token.
3. Open `/setup`, enter the token, and create the first administrator.
4. Add each disk again on **Disks and library folders** ([Add disks and library folders](02-add-disks-and-library-folders.md)). A disk that already carries an Axis marker shows its identifier in **Marker**, and **Add the volume** takes that marker over.
5. Add the library folders, and scan them.
6. Add your indexers, download clients, workers, and accounts again, and add the movies and series you want.

**Run:** steps 1 and 2 ran. **Not run:** steps 3 to 6 for this page. Step 4 comes from reading the code.

## Find out what is wrong

The following table lists what you see when the database is the problem, what it means, and what to do:

| What you see | What it means | What to do |
| --- | --- | --- |
| Every page answers `503` with "Axis is starting: it is waiting for its database." | Axis has not reached its database since it started. It tries again, forever by default. | Read `docker compose logs postgres`. A PostgreSQL older than 15 stops Axis at once. |
| `/healthz` answers `503` and `{"status":"degraded"}` (with a session or the metrics token, also `"database":"unreachable"`) and the log has "job notifications: lost the connection" and "cannot claim jobs: ... Connection refused" | PostgreSQL stopped or restarted. | Start PostgreSQL. Axis reconnects by itself; in the review it was back within about 3 seconds of the server. |
| As in the preceding row, and PostgreSQL's log has "No space left on device" | The database's disk is full. PostgreSQL cannot start again until it has room. | Give the database volume more room, then start PostgreSQL. Axis's own log does not say that the disk is full. |
| Warnings such as "database connection 4 is no longer usable (closed by the server?); reconnecting" and "no answer within 10010 ms" | PostgreSQL stopped answering without closing its connections: a paused container, a host that froze, or a lost network. | Find out why the server does not answer. Axis recovers by itself when it answers again. |
| Axis does not start, and its log says "the database records migration ..., which this build of Axis does not have" | The database was migrated by a newer Axis. | Run that newer image, or restore a backup taken before it. |
| Axis does not start, and its log says a migration "has been changed since it was applied" | The image was built from edited migrations. | Run a released image. |
| A page fails with "relation ... does not exist" or names a missing column | The database was changed outside Axis. Axis does not check the schema when it starts. | Restore a backup. |
| "the settings table holds ..., which no module declares" or "the stored value of setting ... is not valid; using the default" | A setting row was changed outside Axis. | Set the setting again on **Settings**, or remove the row. |
| Pages and jobs wait a long time, then fail with "no database connection became free" | Every database connection is in use. | Look at the database's sessions (see the query after this table). |
| `/readyz` answers `{"status":"held by another daemon"}`, and the log says "the database is in use by another Axis daemon" | Another Axis daemon holds this database, and this one waits as a standby. | See [Run one Axis per database](#run-one-axis-per-database). |
| The log says "the lock's connection did not answer within 5 s" and then "the server confirms on another connection that this daemon still holds the lock" | PostgreSQL is slow to answer, for example under heavy disk load. Axis still holds its lock and goes on working. | Nothing, unless it happens often. Then find out why the server is slow. |
| `/readyz` answers `{"status":"database lock lost"}`, and the log says "lost the database lock" | The connection that holds the daemon's lock ended (PostgreSQL restarted, or someone ended the session), or the server hasn't confirmed the lock for 20 seconds. Axis runs no job until it holds the lock again. | Wait. Axis takes the lock again by itself. If the log then says "another session took the database lock", the daemon stops and your container runtime starts it again as a standby. |
| Axis exits with status 3, and the log says "a job handler did not stop within 25 s after the database lock was lost" | A job could not stop after the lock was lost, so Axis ended its own process to keep the job from running beside another daemon. | Find out why the job could not stop, for example a network mount that stopped responding. Your container runtime starts Axis again. |
| A job stays **Running** after Axis restarted | Axis takes back the jobs of its previous run about 50 seconds after it starts, and a job whose lease ran out at the next sweep. | Wait a minute. |
| Axis does not start, and its log says "the database pool has ... connections, but the job queue needs ..." | You set `pool_size` or `AXIS_DATABASE_POOL_SIZE`, and it is smaller than the job queue needs: the queues' concurrency added up, plus 2. A pool set for an earlier version can be too small after an upgrade that added a queue. Axis stopped before it changed anything in the database. | Set it to at least the number the message names; 4 more is recommended, for the pages and the health checks. Or remove the setting, and Axis sizes the pool itself. |
| The log warns "schedule ... was next due in ... s, more than its interval of ... s ahead (the database's clock went back?)" | The database server's clock was set back, for example when the host corrected it after a start with the wrong time. Axis brought that schedule back to one interval from now, so it runs again. | Nothing. Check that the clock of the database's host is kept right. |
| The log says "schedule ... is disabled: no module of this build declares it any more" | The version you upgraded to no longer runs that task on a schedule. Axis keeps the schedule, disabled, and enables it again if a later version runs it again. | Nothing. |
| The log says "schedule ... stays disabled: it was switched off, not by a build" | Someone switched that schedule off in the database. Axis leaves it off at every start. | Nothing, if that was meant. Axis has no page to switch schedules on or off yet. |
| A job is **Failed** with "this build of Axis has no handler for its kind" | It was queued by an earlier version for a kind of work this version no longer has. Axis failed it when it started, so that it holds up nothing, and kept it. | Nothing. A version that has the kind can retry it. |

To see what each database session is doing, and since when:

```sh
docker compose exec postgres psql --username axis --dbname axis --command "SELECT pid, state, wait_event_type, now() - xact_start AS in_transaction, left(query, 60) AS query FROM pg_stat_activity WHERE datname = 'axis' ORDER BY xact_start NULLS LAST"
```

To see which daemons ran jobs (each start of a daemon has a name of its own, `axisd:HOST:PID:INSTANCE`):

```sh
docker compose exec postgres psql --username axis --dbname axis --command "SELECT split_part(worker_id, '/', 1) AS daemon, count(*) FROM job WHERE worker_id IS NOT NULL GROUP BY 1"
```

**Run:** the messages in the table are quoted from the review's runs (a restart, a full disk, a paused server, and a dropped table), except the two "does not start" rows, which come from the code, and "no database connection became free", which the review did not cause. The three rows about the database lock are quoted from the runs of `tests/test-daemon-lock.c`. The rows about the clock, the schedules and a job without a handler are quoted from the runs of `tests/test-job-queue.c`; the pool message is quoted from `tests/test-daemon.c`. The query of the daemons ran. **Not run:** the query of the sessions.

## Repair the database

Axis has no repair command and no consistency check. When the database is damaged or was changed outside Axis, restore a backup into an empty database. Don't change tables by hand, apart from the read-only storage step of [Restore a backup](#restore-a-backup).

## Run one Axis per database

Only one Axis daemon uses a database at a time. The first `axisd` to start takes a lock in the database and holds it until it stops. Another `axisd` that you point at the same database waits as a standby: it serves only the probes, runs no job, changes nothing, and logs which daemon holds the database. When the first one stops, the standby takes over within a few seconds. For the details, including what happens when a daemon loses its lock, see [One daemon per database](../deploy/README.md#one-daemon-per-database) in the deployment notes.

A second daemon starts by mistake in these cases:

- You run `docker compose up --scale axis=2`, or start the quick start's container while the Compose stack runs.
- A test instance on another machine points at the same database.
- A Kubernetes deployment has more than one replica, or uses the `RollingUpdate` strategy. The example uses one replica and the `Recreate` strategy.

To find the daemon that holds the database, ask PostgreSQL which session holds the lock. The name tells you the daemon's instance, version, process ID, and host:

```sh
docker compose exec postgres psql --username axis --dbname axis --command "SELECT a.pid, a.application_name, a.client_addr, a.backend_start FROM pg_locks l JOIN pg_stat_activity a ON a.pid = l.pid WHERE l.locktype = 'advisory' AND l.granted AND l.classid = 1635281267 AND l.objid = 1600417134 AND l.objsubid = 1"
```

Stop the daemon that should not run. If that is the one that holds the database, the standby takes over.

Axis needs a direct connection to PostgreSQL, or a pooler in session mode. A pooler in transaction mode, such as PgBouncer with `pool_mode = transaction`, isn't supported.

**Run:** two daemons on one database, the takeover when the first stops, a standby that does not wait (`AXIS_DATABASE_WAIT=0`), a lost lock taken again, a lock taken by another session, and a job that does not stop, in `tests/test-daemon-lock.c` against `postgres:17-alpine`. The lock condition of the query is the one those tests use. **Not run:** the `docker compose exec` form of the query, and Kubernetes.

## Verified

- `make check-examples` runs the Compose example for real, with Podman and `podman compose` only, and in it: the data checksums check, the backup, the listing of the backup, a change after the backup, the restore into a newly created empty database, and the start of Axis on it. It checks that the change is gone, that `/readyz` is ready, and that the administrator logs in. Docker was not used.
- The review also showed that a `pg_restore --clean` over a database that a newer migration had changed leaves later data in it, and ran a restart of PostgreSQL, a full database disk, and a paused PostgreSQL under a running Axis, and two daemons on one database.
- The deletion of old history ran in the tests only: each rule of what it keeps in `tests/test-history-retention.c`, and its runs on a generated database of 3,000,000 events, 257,000 grabs, 242,000 downloads, and 292,000 imports in `tests/test-db-scale.c`. It has not run on a real server.
- Nothing ran the Quadlet, Kubernetes, or `docker run` forms of the commands, a restore of a large library, a restore after a newer Axis migrated the database in the same check, `pg_checksums`, or the adoption of a disk into an empty database.
