<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Fixtures of the definitions Axis ships

The answers of the four sites whose definitions are in `data/definitions`
(`add-builtin-torrent-indexers`), for `tests/test-indexer-builtin.c` and the
seeds of `tests/fuzz/fuzz-def-trace.c`.

Each site was asked once or twice on 2026-11-18 with a public domain film's
title. **No fetched page is here.** Each file was written by hand from the
structure of what the site answered: the elements, attributes and members the
definition reads are as the site had them, and everything else is cut. Every
value in them is made up: titles (`Made Up Film`, `NOGRP`), uploaders
(`maker1`), addresses (`.example.invalid`), sizes, dates, counts and ids
(`2000000001`, `1730000101`), and info-hashes, which are a short unit
repeated (`A7A7A7...`, `d6d6d6...`) so that no one can be a real torrent's.
`/builtin/fixtures-made-up` in `tests/test-indexer-builtin.c` fails on any
40-digit hash here, in the seeds or in the shipped definitions that is not
one of those patterns. To make a new fixture from a saved page, follow
`docs/CONTRIBUTING-definitions.md` ("How to repair an indexer definition"),
which uses `tests/tools/scrub-fixture.py`.

Both HTML-served answers end with the script Cloudflare puts into the pages
of the sites it fronts (`/cdn-cgi/challenge-platform/scripts/jsd/main.js`;
after `</rss>` in the feed): it is not a browser challenge, and the engine
must not take it for one (it did, before this change).

| File | Stands for |
|---|---|
| `limetorrents-search.xml` | LimeTorrents' search feed (`/searchrss/WORDS/`): two movies (one name escaped twice, `&amp;amp;`), an episode, an anime (`Anime.`), an e-book (`Other - E-books`) and music (`Music - Music`), both left out, and `Other - Other` with a count that has a thousands comma |
| `limetorrents-empty.xml` | the feed of a search that found nothing: the channel, no item |
| `torrentproject-search.html` | TorrentProject's result page (`/?t=WORDS&orderby=seeders`): the `#similarfiles` list with its header, five rows (a verified one, a name cut with "..." whose address has it whole, ages as "6 months ago" and as a date, sizes in GB and MB), and a script in the head whose text looks like a row |
| `torrentproject-details.html` | a release's own page: its name in `h1`, the hash in `.torrenthash h4` and in the magnet span's title, and the magnet link wrapped in another site's redirector, which is never followed |
| `torrentproject-empty.html` | a result page with the list and no row |
| `therarbg-search.json` | TheRARBG's JSON listing: two movies, a sequel without one of the words (left out by andmatch), music and a game (not video), and one in Other |
| `therarbg-empty.json` | the listing with no result |
| `torrentscsv-search.json` | Torrents-CSV's search: three results, one with no seeders or leechers (`null`), and one whose hash is not one (lost) |
| `torrentscsv-empty.json` | a search with nothing found |
