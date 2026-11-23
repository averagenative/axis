# Newznab and Torznab fixtures: written by hand, nothing recorded

These answers were written for `add-indexer-newznab` from the Newznab API
specification (newznab.readthedocs.io, the `t=caps`, `t=search`, `t=movie`,
`t=tvsearch` and `t=get` functions, its RSS answer, its `newznab:attr`
attributes and its error codes), the Torznab specification (torznab.github.io:
the same with `torznab:attr` and the torrent attributes), and what Sonarr,
Radarr, Prowlarr and Jackett are documented or known to send and read. **No
real indexer was asked**: the implementer had no key and was told not to look
for one or contact any indexer. Every detail below is believed, not verified.
The tests name two made-up Newznab indexers, NewsOwl (`api.newsowl.example`)
and DozyNewt (`dozynewt.example`). The first run against real indexers is
expected to find corrections, and its answers should be recorded beside these
with invented content that keeps the real structure and lengths, as
`tests/fixtures/prowlarr/recorded` was (see `tests/fixtures/README.md`).

The stand-in (`tests/axis-test-newznab.c`) serves them; `@ORIGIN@` (its
origin), `@BASE@` (its prefix), `@KEY@` (its key) and `@FILES@` (where its
results say its files are: its own origin unless a test set another) are put
in. The seeds of `tests/fuzz/fuzz-indexer-xml.c` and
`tests/fuzz/fuzz-indexer-newznab.c` are made from them.

| file | what it stands for |
| --- | --- |
| `caps.xml` | `t=caps` of a Newznab indexer: search by `q`; tv-search by `q,rid,tvdbid,tvmazeid,season,ep`; movie-search by `q,imdbid,genre`; limits 100 (50 unless asked); categories 2000 and 5000 with subcategories, and 7000 |
| `caps-defaults.xml` | an older indexer's `t=caps`: the searches available with no `supportedParams` |
| `search-movie.xml` | a search's RSS answer: ten items (every field; the `/getnzb/` address form, another zone, the size from the enclosure only, `imdbid` with `tt`; the same guid twice; a file on another address; no title; no way to fetch; a name with markup as entities and a CDATA description; passworded; every optional field wrong; numeric character references), and `newznab:apilimits` (12 of 5000 calls, 3 of 100 grabs) |
| `search-tv.xml` | an episode's: two items, `tvdbid`, season and episode attributes |
| `search-empty.xml` | a search that found nothing |
| `torznab-caps.xml` | Jackett's `t=caps` for one tracker |
| `torznab-search.xml` | Jackett's answer: a torrent file at `/dl/...` (outside the feed's path, on its origin) with seeders, peers, an info-hash and download volume factor 0; a magnet link only (in the link, the enclosure and `magneturl`), factor 0.5 and upload factor 2; an address that answers with a redirect to a magnet link |
| `error-100.xml`, `error-500.xml` | error documents: credentials, request limit |
| `hostile-entities.xml` | an answer with an internal subset defining nested entities ("billion laughs"): refused whole |
| `hostile-external.xml` | an answer with an external DTD and an external entity: refused whole |

## Believed, not verified

The protocol and the answers:

1. The endpoint is `<address><base path>/api`; NewsOwl's is believed to be
   `https://api.newsowl.example/api`, DozyNewt's `https://dozynewt.example/api`
   (or an `api.` host). An indexer whose API is elsewhere than `/api` cannot be
   used (there is no setting for it).
2. `apikey` in the query is accepted by every call, `t=caps` included (the
   specification says `caps` needs none; Axis sends it anyway).
3. `t=caps` is answered as `caps.xml` is: `<searching>` with `search`,
   `tv-search`, `movie-search` and `available="yes"`, and `supportedParams` a
   comma-separated list. When `supportedParams` is missing the defaults are
   taken as `q` for search, `q,season,ep` for tv-search and `q,imdbid` for
   movie-search (from the specification's examples; Sonarr's and Radarr's
   defaults may differ).
4. `available` may be `yes`, `true` or `1`.
5. `imdbid` is sent as its digits without `tt`, keeping the leading zeros
   (`0600001`); `tvdbid`, `tmdbid`, `season` and `ep` as plain numbers.
   `tmdbid` for a movie and `imdbid`/`tmdbid` for TV are used only when the
   indexer's capabilities name them.
6. `extended=1` makes an indexer give every attribute of a result (`size`,
   `grabs`, `imdb`, `tvdbid` and the rest); without it some give only a few.
7. `o=xml` is not sent: XML is the default in the specification, and the
   clients indexers are tested against do not send it.
8. `limit` up to the capabilities' `max` (100 when not known) and `offset=0`
   are honoured; only one page is asked for (paging would spend calls).
9. A result is an `<item>` with `title`, `guid`, `link`, `pubDate` (RFC 822),
   an `<enclosure url length type>`, and `newznab:attr` / `torznab:attr` with
   `name` and `value`. The prefix bound to the attributes' namespace is
   whatever the indexer chose; attributes are matched by local name.
10. The size is the `size` attribute, else the enclosure's `length`, else a
    `<size>` element (Jackett's); a `length` of 0 is not a size.
11. `imdb` holds the digits without `tt` (newznab), `imdbid` may hold either;
    `tmdbid`/`tmdb` and `tvdbid`/`tvdb` are accepted under both names.
12. Torznab's `peers` is seeders and leechers together; leechers are
    `peers - seeders`. `downloadvolumefactor` 0 is freeleech, between 0 and
    1 a partial freeleech; `uploadvolumefactor` 2 or more is shown as "double
    upload".
13. `password` 1 or 2 means passworded (shown as a flag; the quality layer
    does not act on it).
14. A result's file address (enclosure, else link) is on the indexer's own
    origin; when it is on another, an administrator must accept that origin.
    NewsOwl's is believed to be `https://api.newsowl.example/api?t=get&id=...&apikey=...`;
    nZEDb-based indexers give `/getnzb/<guid>.nzb&i=<user>&r=<key>`.
    Whether DozyNewt serves files from the same host as its API is not
    known: its first test will show it.
15. A file's download answers with the NZB itself (status 200), or with an
    error document (status 200) such as `<error code="300">` (no such item) or
    `501` (download limit reached). A download that redirects is not followed
    off the indexer's origin (a redirect to a magnet link is read).
16. Error documents come with status 200: 100 to 199 are the credentials and
    the account (`AUTH`), 200 to 299 a parameter or function (`REFUSED`), 300
    no such item, 500 the request limit and 501 the download limit
    (`RATE_LIMITED`; the indexer is held for an hour), 910 the API disabled.
    The description is cleaned, cut to 200 characters and has the key
    blanked out before it is shown.
17. `<newznab:apilimits apicurrent apimax grabcurrent grabmax>` may be in any
    answer (believed of NewsOwl's searches and caps); `apicurrent` is taken to
    count the call that answered. Its window is taken to be a day: a report
    more than a day old counts as nothing used. `apioldesttime` and
    `graboldesttime` are not read.
18. Jackett's feed path is `/api/v2.0/indexers/<id>/results/torznab` and its
    files are at `/dl/<id>/?jackett_apikey=...&path=...&file=...` on the same
    origin. Prowlarr's per-indexer feed is believed to be `/<number>/api`.
19. Prowlarr's `/api/v1/indexer` is believed to give each indexer's
    `indexerUrls` and a `baseUrl` field (the recording in
    `tests/fixtures/prowlarr/recorded` holds neither, so it may have been cut
    from it); the host from them is how an indexer configured both natively
    and in Prowlarr is recognised. Without them, such an indexer is asked
    twice (and its releases listed once).

What the tests prove, against these answers, is the code's handling of them
(`tests/test-indexer-newznab.c`, `tests/test-web-indexers.c`,
`tests/test-movies.c`, `tests/test-tv.c`), not that real indexers answer so.
