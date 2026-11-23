# Prowlarr fixtures: recorded, and constructed

The files in `recorded/` have the shape of the answers of a Prowlarr 3.0.1
(`3.0.1.5752`), read through Axis's own code and by direct API calls. Every
answer is rewritten with invented content: the indexers, releases, hosts and
identifiers in them are made up, and they describe no installation. The
files at the top of this directory are constructed: written by hand, before there was a
recording, to hold what a recording does not (junk, addresses elsewhere,
failing indexers, every field of the wrong type). Both are served by the
stand-in, `tests/axis-test-prowlarr.c`: `axis_test_prowlarr_new_recorded()`
answers with the recordings, `axis_test_prowlarr_new()` with the constructed
answers.

## Recorded (`recorded/`)

These are **structurally faithful recordings with invented content**: the
members, their order, their types, the nesting, the counts and the lengths
that matter are as Prowlarr gave them, and every value that would identify a
real release, indexer, account or machine is made up. Prowlarr's address is
replaced by `http://prowlarr.test:9696`, its key by
`0123456789abcdef0123456789abcdef`, and every `link` value (an opaque token of
Prowlarr's) by a run of `R` of the same length, because the length matters: a
link that wraps a magnet runs past 2,000 bytes, and a first recording that
shortened it hid a limit that dropped every such release on the real service.
The film and the series searched for, and so every release name, are made up
("Hearth", "Cracking Slate"), with the rest of each name (resolution, source,
codec, group) kept; the info-hashes are made-up patterns (`e1e2e3e4...`, kept
equal where two results shared one); the IMDb, TMDb and TheTVDB identifiers,
the indexers' own result ids and guids, and the trackers in the magnet links
are made up with the same number of characters; the two Usenet indexers and
the public sites that Axis does not support by name have made-up names and
hosts under `.example`. The searches are cut to a few results where Prowlarr
found hundreds. The stand-in replaces the scrubbed origin and key by its own.
`make check-made-up` checks the result (`tests/fixtures/README.md`).

| file | what it is |
| --- | --- |
| `system-status.json` | `GET /api/v1/system/status` |
| `indexer.json` | `GET /api/v1/indexer`: eleven indexers, seven on; the two Usenet ones (NewsOwl and DozyNewt here) have `redirect: true` |
| `indexerstatus.json` | `GET /api/v1/indexerstatus`: an empty list (no indexer was failing) |
| `search-movie-imdb.json` | a search for Hearth (1995) by `{ImdbId:tt0600001}`, `type=movie`: six of its results |
| `search-movie-text.json` | the same by the words "Hearth 1995": ten of its results, among them two torrents of The Pirate Bay known by their link address only |
| `search-tv-tvdb.json` | an episode by `{TvdbId:82001}{Season:1}{Episode:1}`, `type=tvsearch`: four of its results, the same names from two indexers |

The seeds of `tests/fuzz/fuzz-indexer-prowlarr.c` include them
(`tests/fuzz/corpus/fuzz-indexer-prowlarr/recorded-*`).

## Verified against the real Prowlarr

1. Every `/api/v1` call takes the key in the `X-Api-Key` header; a wrong or
   missing key is answered `401` with an empty body and no content type.
2. `GET /api/v1/system/status` answers an object with a `version` string.
3. `GET /api/v1/indexer` answers a list of objects with `id`, `name`,
   `protocol`, `enable` and `capabilities` (`movieSearchParams`,
   `tvSearchParams`: `q`, `imdbId`, `tmdbId`, `tvdbId` and others), and, per
   indexer, `redirect` and `supportsRedirect`.
4. `GET /api/v1/indexerstatus` answers a list, empty when no indexer fails.
5. The identifier forms work as written: `type=movie` with a `query` of
   `{ImdbId:tt0600001}`, `type=tvsearch` with `{TvdbId:82001}{Season:1}{Episode:1}`.
6. A search's answer does not say which indexers failed (as believed before).
7. `imdbId`, `tmdbId` and `tvdbId` are integers, and `0` means none (a third
   of the results had `imdbId` 0, most had `tmdbId` 0).
8. `limit` does not cap the total: 377 results came back for `limit=100`.
9. `indexerFlags` is a list of strings (`freeleech`).
10. A result has `downloadUrl` (an address on Prowlarr's own origin, as the
    request's `Host` named it: `/<indexerId>/download?apikey=...&link=...&file=...`),
    or only `magnetUrl` of the same form with `infoHash` beside it, or both
    (YTS gives both, the same address). `infoHash` is upper case. A result's
    `guid` is the indexer's address of it, or, for The Pirate Bay, the whole
    magnet link.
11. A `magnetUrl` address answers `301` with `Location: magnet:?xt=urn:btih:...`
    of about 1.1 kB (the trackers), printable ASCII, whose hash is the
    result's `infoHash`. (99 of 377 results of one search were known only so;
    Axis used to drop them.)
12. A download of an indexer with `redirect: true` answers `301` to the
    indexer's own address: another host, with the indexer's own key in the
    address. With `redirect: false` a torrent download answers the file
    (`application/x-bittorrent`, bencoded).
13. An identifier search found 189 results where a search by words found
    377: indexers that take only words give nothing to an identifier search.
14. A search by words across the public indexers took about 10 s (the
    deadline of a search is 60 s).

## Not verified

- **The bytes of a Usenet release file behind a redirect** (NewsOwl's,
  DozyNewt's): not fetched, to spare the indexers' download quota. The
  redirect itself was seen (12); what the indexer's site answers at that
  address, and whether it redirects again, is believed: an NZB, by its bytes.
  The stand-in serves an NZB there.
- A Usenet download through an indexer with `redirect: false`: there was
  none to try.
- A `429`, or a `503` with `Retry-After`: never provoked. Axis waits the time
  stated (at most a day is believed) and does not count it as failing.
- `disabledTill` in `indexerstatus` (its form and meaning): no indexer was
  failing, so the list was empty. The constructed `indexerstatus.json` holds
  the form believed, an ISO 8601 time.
- A Prowlarr served under a base path (`urlBase` was empty), or behind a TLS
  proxy: the addresses it writes in a download link then are believed to
  carry its URL base and the scheme, host and port of the request.
- `categories` as a repeated parameter, and `indexerIds` restricting a search
  to the indexers named (the search by title of the indexers that take no
  identifier relies on it); the stand-in honours `indexerIds` as Prowlarr is
  believed to.

## Constructed (this directory)

Where an address of the stand-in should appear, the constructed fixtures hold
`@ORIGIN@` (its origin), `@BASE@` (the base path it is served under, `""` or
`/prowlarr`) and `@KEY@` (its key), which the stand-in puts in.

| file | what it stands for |
| --- | --- |
| `system-status.json` | `GET /api/v1/system/status` |
| `indexer.json` | `GET /api/v1/indexer`: four indexers Axis keeps (one switched off, one that takes no IMDb identifier for a movie, one that takes no TVDB identifier for an episode), one of a protocol nobody knows and a string, both left out |
| `indexerstatus.json` | `GET /api/v1/indexerstatus`: one indexer disabled until 2099, one until 2020 (not failing now), one with no time |
| `search-movie.json` | `GET /api/v1/search` for a movie: fourteen entries. Eight are kept (a Usenet release with every field; a torrent with peers, flags, repeated categories, an upper-case hash and a magnet link; a torrent whose address redirects to a magnet link; a torrent file; a name that is markup; every optional field of the wrong type; a bad magnet link beside a good address; a magnet link where the address should be). Two name an address that is not the source's own (another host, another port), one has no way to fetch, three cannot be read (a number for a name, no protocol, not an object) |
| `search-tv.json` | the same for a series' episode and season |
| `search-empty.json` | a search that found nothing |
| `search-not-a-list.json` | what Axis reads as an error: an object where a list should be |

The tests also generate answers (a thousand and more results, names with
control characters and a byte that is not UTF-8, addresses of a file that is
not one) from the helpers in `tests/axis-test-prowlarr.c`.
