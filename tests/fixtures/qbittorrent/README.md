# qBittorrent fixtures: hand-written, not recorded

No qBittorrent was available to record these.  They were written by hand from
qBittorrent's published Web API (v2, under `<base>/api/v2/`) and from memory of
qBittorrent 4.6 and 5.0.  The first run of Axis against a real instance is
expected to find corrections: see the list of details that are believed but not
verified below, in the openspec change `add-downloader-qbittorrent`, in
`tests/test-download-qbittorrent.c` and in the stand-in it runs against,
`tests/axis-test-qbittorrent.c` (also hand-written: it follows the 4.x or the
5.x way, as chosen).

Only the answers that are JSON are files here; a login's, an add's and a
version's answers are plain text, written in the tests.

| file | what it stands for |
| --- | --- |
| `torrents.json` | `GET /torrents/info`: every documented state word (downloading, forcedDL, stalledDL, metaDL, forcedMetaDL, queuedDL, allocating, checkingDL, checkingResumeData, pausedDL, stoppedDL, moving, uploading, forcedUP, stalledUP, queuedUP, checkingUP, pausedUP, stoppedUP, error, missingFiles, unknown), a category of a person's, none, a sub-category of Axis's, an upper-case hash, no `content_path` (before 4.3.2), a `content_path` that is not a place, a trailing slash, a name with two spaces |
| `torrents-wrong-types.json` | fields of the wrong types, missing, out of range, bad hashes, one hash twice, entries that are not objects |
| `torrents-unknown-state.json` | a state word nobody knows, and one in another case |
| `torrents-empty.json`, `torrents-not-a-list.json` | an empty list; an object where a list should be |
| `categories.json`, `categories-no-axis.json` | `GET /torrents/categories`: an object keyed by category name, with and without Axis's (and one that differs only in case); the stand-in also answers each category's `savePath` and `GET /app/defaultSavePath` |
| `categories-not-an-object.json` | a list where an object should be |

## Believed, not verified

Each of these is what the kind or the stand-in assumes; a run against a real
qBittorrent (4.x and 5.x) should confirm or correct it.

1. `POST /api/v2/auth/login` with a form body (`username=...&password=...`,
   `application/x-www-form-urlencoded`) answers `200` with the body `Ok.` in
   4.x and `204` with no body in 5.x on success, `200` with `Fails.` in 4.x
   and `401` in 5.x for a wrong password.  (Told to this project from earlier
   work with a real 5.x; not seen by the author of this change.)
2. After too many failed logins (default five, "Ban client after consecutive
   failures") the address is banned for a time (default an hour) and every
   request, the login included, is answered `403`.  A `403` to the login is
   therefore read as a ban, and not as a wrong password.  Whether a ban also
   answers `403` to a request that carries a valid cookie is not known; the
   kind treats a `403` to any call as a lost session, logs in once, and reads
   the login's `403` as the ban.
3. The session cookie is `SID` in 4.x and `QBT_SID_<port>` in 5.x (the kind
   keeps whatever cookie is set, by name), with `HttpOnly`, `SameSite=Strict`
   and a path; the login's own request carries no cookie.
4. Every API call but the login needs the cookie and is answered `403`
   without a valid one.  The Web UI's CSRF protection compares the host of the
   `Referer` and the `Origin` of a request with the `Host` it was sent to; the
   kind sends both, equal to the client's own address (scheme, host and port,
   never the base path), on every request.  What a mismatch is answered with
   (`401` is assumed), and whether it is checked for a GET, is not known.
   "Bypass authentication for clients on localhost" and "for clients in
   whitelisted IP subnets" are not handled: Axis still wants a password.
5. `GET /app/version` answers the version as plain text (`v4.6.2`) and
   `GET /app/webapiVersion` the Web API's (`2.9.3` in 4.6, `2.11.x` in 5.0).
   Both are read as `[A-Za-z0-9._+-]` of at most 32 characters; the Web API
   version may be missing without failing the test.
6. `GET /torrents/categories` answers a JSON object keyed by category name
   (the value is an object with `name` and `savePath`).  Names are case
   sensitive.  `POST /torrents/createCategory` with `category` and `savePath`
   (empty) makes one and answers `200`; a category that exists, and a name it
   does not take, are both answered `409` (the kind reads the list again, and
   does not take a `409` to mean the category exists).  Whether `savePath` may
   be empty in 4.1 to 4.x is not known.
7. `POST /torrents/add` is a `multipart/form-data` form: `urls` (a magnet
   link; several would be separated by a newline) or `torrents` (the file,
   with a file name and `application/x-bittorrent`), `category`, and `paused`
   (4.x) / `stopped` (5.x), which the kind sets to `false` both, each version
   ignoring the other's word.  It answers `200` with `Ok.` or `Fails.` and no
   hash.  What it answers for a torrent it has already (`Fails.` is assumed in
   4.x; 5.x may merge the trackers and answer `Ok.`) is not known: the kind
   asks for the hash first and refuses a torrent that is there.  A `415` for a
   file that is not a torrent is assumed to be read as a refusal by the layer.
8. A torrent just added may not be listed by `GET /torrents/info?hashes=<hash>`
   at once; the kind asks up to six times, 0.3 s apart.  The `hashes`
   parameter takes hashes separated by `|`, case-insensitively.
9. `GET /torrents/info` answers a JSON array of objects; `?category=<name>`
   filters by category (an empty value means none, a missing parameter any;
   whether sub-categories (`axis/sub`) are included depends on the version and
   a setting, so the kind keeps only those of exactly Axis's).  Each has,
   among many more fields, `hash` (the v1 info-hash; for a hybrid torrent that
   too, for a pure v2 torrent a truncated v2 hash, which Axis does not match),
   `name`, `category` (empty for none), `state`, `size` (the selected files),
   `total_size`, `progress` (0 to 1), `dlspeed` (bytes a second), `eta`
   (seconds; `8640000`, a hundred days, for "never"), `save_path`,
   `content_path` (from 4.3.2), `ratio`, `completion_on` (`-1` or `0` while
   unfinished).  `ratio` and `completion_on` are not read: Axis has no use for
   them.
10. The state words are the ones the Web API documentation lists, case
    sensitive, with 5.x's `stoppedDL` and `stoppedUP` for 4.x's `pausedDL`
    and `pausedUP`.  `checkingUP` is taken as finished and `checkingDL` as not;
    `moving` is taken as not yet where it will be (as Deluge's); `unknown` is
    taken as a word the kind does not know.  `forcedMetaDL` (4.4 and later)
    is assumed to exist.
11. A finished torrent's files are at `content_path` (a file for a torrent of
    one file, a folder for several), which is `save_path` joined with the
    torrent's name for a torrent with a root folder.  Before 4.3.2 there is no
    `content_path` and the kind joins `save_path` and `name` itself.  With
    "Keep incomplete torrents in" or "Move completed" set, whether `save_path`
    and `content_path` are where the files are while the torrent is `moving`
    is not known (the kind never takes a moving torrent as finished).
12. `POST /torrents/delete` with `hashes` and `deleteFiles=true|false` answers
    `200` whether the torrent existed or not, and no more; the kind asks the
    list afterwards and calls it removed when the hash is not listed (up to
    six times, 0.3 s apart).  Whether the files are deleted before the call
    answers, or after, is not known.  Because the delete cannot tell, the
    kind asks the list (`/torrents/info?hashes=<hash>`) first: a torrent it
    does not list is gone already (`AXIS_DOWNLOAD_ERROR_GONE`: something else
    removed it) and no delete is sent.  That a torrent qBittorrent has, long
    finished, is always listed by its hash is believed (item 8).
13. A torrent that qBittorrent adds, and then moves to another category by a
    rule of the person's (Tools, Options, "Automatic Torrent Management" or a
    watched folder) is not Axis's to follow: the add is refused and the
    torrent left.
14. A category's `savePath` (item 6) is the folder its torrents are saved
    in, when the torrent is under automatic management; an empty one means
    the default save path and the category's name (believed), and a
    relative one is not taken (the kind refuses it).  `GET
    /app/defaultSavePath` answers the default save path as plain text.  The
    kind takes a category's save path as Axis's folder only when it is a
    full path and neither the default save path nor another category's
    folder is it or lies inside it, and asks only for a torrent qBittorrent
    no longer has after its import; a torrent saved elsewhere (manual
    management ignores the category's path) is then outside the folder and
    its files are left.
