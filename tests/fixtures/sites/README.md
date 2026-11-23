# YTS, EZTV and The Pirate Bay fixtures: written by hand, nothing recorded

These answers were written for `add-indexer-sites` from the shapes these three
public sites are documented or publicly known to answer in (YTS's API v2
documentation, EZTV's public API as Sonarr's and Jackett's definitions use it,
and apibay's `q.php` as the sites that read it use it). **No site was asked**
when these files were written.
Every detail below is believed, not verified. The design says these fixtures
are recordings made once, by agreement, of answers
that need no account and change nothing; that recording has not been made. The
stand-in (`tests/axis-test-sites.c`) holds no knowledge of what a fixture says
(it serves a file by the site's path, and a test can set a body for a request),
so recorded files can be put in these files' places without a change to it or
to the kinds. The tests that assert what a fixture holds (names, hashes, sizes,
counts in `tests/test-indexer-sites.c`) read these hand-written ones and would
need their expectations moved to the recorded values: the recorded answers
should go beside these (in `tests/fixtures/sites/recorded`), scrubbed, keeping
real lengths and types, as `tests/fixtures/prowlarr/recorded` was.

The seeds of `tests/fuzz/fuzz-indexer-sites.c` (`tests/fuzz/corpus/fuzz-indexer-sites`)
are made from them by writing each with its selector byte.

| file | what it stands for |
| --- | --- |
| `yts-list.json` | `list_movies.json` of a search: two movies (Hearth, 1995, with three torrents in 720p, 1080p and 2160p; another Hearth, 2011, with one), every field YTS is believed to give, hashes in upper case |
| `yts-empty.json` | a search that found nothing: `movie_count` 0 and no `movies` |
| `yts-error.json` | an answer whose `status` is `error` |
| `yts-hostile.json` | wrong types (a count, a year and a size as text, a quality that is not a word, a type that is a list), hashes that are short, not hex or all zeros, items that are not objects, a movie with no `torrents`, one whose `torrents` is text, a movie with `title_long` only, a date out of range |
| `eztv-list.json` | `get-torrents` of a series: four torrents with numbers as text (`season`, `episode`, `size_bytes`), one with an empty `title` (named by its file), one with a `filename` and a title only |
| `eztv-empty.json` | a series with nothing: `torrents_count` 0, `torrents` empty |
| `eztv-hostile.json` | numbers that are fractions, objects, out of range or text, an IMDb identifier that is 0, a name with control characters and spaces, the same hash twice, a short hash, no hash, no name, items that are not objects |
| `tpb-list.json` | apibay's `q.php` answer: three items with every number as text, hashes in upper and lower case, a category, an IMDb identifier (one of them wrong) |
| `tpb-none.json` | apibay's "nothing found": one item named "No results returned" with a hash of zeros |
| `tpb-hostile.json` | numbers as text and as numbers, a negative one, a fraction, a date out of range, the same hash in two cases, a short hash, no name, a hash that is not hex, a hash that is a number, items that are not objects |
| `tpb-categories.json` | (`add-builtin-torrent-indexers`) made-up titles in UHD movies (211) and UHD TV (212), music (104), a game (401), an application (301), an e-book (601), adult (501) and a category that is none (`x9`): the four that are not video are left out |

## What a release from these sites is

An info-hash, a name and sizes, and nothing else is believed. Axis builds the
magnet link itself (`magnet:?xt=urn:btih:<hash>&dn=<name, percent-encoded>`
and one `&tr=` for each of the trackers below), has the downloads layer's
validator accept it, and fetches nothing from the site at grab time. Hashes
are 40 hexadecimal digits, lower-cased; a hash of zeros, a short one or one
that is not hex drops the item. The site's own magnet link, torrent address
and screenshots are never read.

**YTS names are constructed**: its answer holds no release name, so one is
built from the fields, `Title (Year) [Quality] [Type] [Codec] [YTS]` (for
example `Hearth (1995) [1080p] [BluRay] [x264] [YTS]`; the type `bluray` is
written `BluRay` and `web` `WEB`; a quality, type or codec that is not a plain
word is left out), in the form the release parser reads (the test reads the
names back through it). That the real YTS files are named so is not claimed.

The trackers (the one place: `src/indexer/axis-indexer-sites-parse.c`,
`axis_sites_trackers()`):

    udp://tracker.opentrackr.org:1337/announce
    udp://open.tracker.cl:1337/announce
    udp://open.stealth.si:80/announce
    udp://exodus.desync.com:6969/announce
    udp://tracker.torrent.eu.org:451/announce
    udp://tracker.openbittorrent.com:6969/announce

## Believed, not verified

The addresses:

1. YTS is at `https://yts.gg` (seen on 2026-11-16: `yts.mx` no longer resolves), EZTV at `https://eztvx.to`, The Pirate Bay's API
   (apibay) at `https://apibay.org`. These are the defaults in
   `src/indexer/axis-indexer-sites.h`; the sites move between domains, and any
   of them may not answer, or may answer another business's page, where this
   says. Axis never looks for a new address: it is a setting of each indexer.
   All three are asked over https, at the address's host and port only, with no
   base path.

YTS:

2. `GET /api/v2/list_movies.json?query_term=<tt…>&limit=50&page=1`: `query_term`
   takes an IMDb identifier with its `tt` or a title; `limit` is at most 50;
   `page` starts at 1. A request for TV is not made. The title is searched
   without the year (YTS is believed to match titles, actors and identifiers).
   Only one page is read.
3. The answer is `{"status":"ok","status_message":"…","data":{"movie_count":N,
   "limit":…,"page_number":…,"movies":[…]}}`; with nothing found `movies` is
   missing. `status` other than `ok` is an error. A movie has `title`,
   `title_long` ("Hearth (1995)"), `year` (a number), `imdb_code` and `torrents`;
   a torrent has `hash` (upper case), `quality` ("1080p"), `type` ("bluray",
   "web"), `video_codec` ("x264", "x265"), `size_bytes` (a number), `seeds`,
   `peers`, `date_uploaded_unix` (numbers). YTS's numbers are believed to be
   numbers (not text), so a count that arrives as text is not taken as one.
4. `peers` is leechers (seeders are `seeds`).
5. A fresh test asks `list_movies.json?limit=1&page=1`, which is believed to
   give the newest.

EZTV:

6. `GET /api/get-torrents?imdb_id=<digits>&limit=100&page=N`: the identifier is
   its digits without `tt` and without leading zeros (Jackett's
   `IMDBIDShort`); `limit` up to 100; pages start at 1. A request without an
   IMDb identifier is not made (EZTV is believed to list the newest then): it
   fails with the reason. Up to five pages are read; a later page that fails
   ends the list there.
7. The answer is `{"imdb_id":…,"torrents_count":N,"limit":100,"page":N,
   "torrents":[…]}`; a torrent has `hash`, `title`, `filename`, `imdb_id`,
   `season`, `episode`, `seeds`, `peers`, `date_released_unix`, `size_bytes`.
   Its numbers are believed to arrive as text in places (`season`, `episode`,
   `size_bytes`, `imdb_id` have been seen as text) and as numbers in others:
   both are taken.
8. `title` ("Show S01E02 720p HDTV x264-GRP EZTV") is the name; when it is
   empty the `filename` without its video extension is. The season and episode
   fields are not read: the release parser reads the name.
9. `imdb_id` in an answer is digits without `tt` (and "0" for none).
10. `get-torrents?limit=1&page=1` (a test) is believed to give the newest.

The Pirate Bay (apibay):

11. `GET /q.php?q=<words>&cat=<ids>`; the categories are 201, 202, 207, 209
    and 211 (movies, DVD, HD, 3D, UHD) for a movie, 205, 208 and 212 (TV, HD,
    UHD) for TV, 200 (all of Video) for words as typed, sent as one
    comma-separated value (percent-encoded). A movie with an IMDb identifier is
    asked `q=tt...` first, and by its title and year ("Hearth 1995") when that
    finds nothing; an episode by "Title S01E02", a season by "Title S01".
    (`add-builtin-torrent-indexers`, seen on 2026-11-18: apibay answers
    `q=tt` and a film's IMDb number with the uploads whose `imdb` is that
    identifier.)
12. The answer is a list of `{"id","name","info_hash","leechers","seeders",
    "num_files","size","username","added","status","category","imdb"}` with
    every number as text and the hash in upper case. "Nothing found" is a list
    of one item named "No results returned" with a hash of 40 zeros, which is
    taken as empty. The name, hash, size, seeders, leechers, date, category
    (mapped to Newznab's numbers; one outside video is left out) and the
    uploader's `status` (`vip` and `trusted` become the flags "VIP" and
    "Trusted") are read; the `imdb` an uploader typed is not taken as the
    release's.
13. A test asks `q.php?q=1080p&cat=200` (a word with results, in video).

The traffic:

14. Every request is a GET with no credential, cookie or key, to the
    configured address only, one every two seconds by default per site (the
    setting "Requests a second, per public site", `indexers.site_rate`), one
    at a time, and a site that fails is left alone for a while by the limiter's
    back-off. Whether these sites throttle sooner than that, or answer a
    program with a challenge page (Cloudflare and the like: it would be read as
    "not its list" and the site shown as failing), is not known.
15. A site that has moved shows as one of: no answer (unreachable), an error
    status (404, 403, 5xx: failing), a redirect to another address (not
    followed: failing, saying so), or a page that is not its list (failing).

What the tests prove, against these answers, is the code's handling of them
(`tests/test-indexer-sites.c`, `tests/test-web-indexers.c`), not that the real
sites answer so.
