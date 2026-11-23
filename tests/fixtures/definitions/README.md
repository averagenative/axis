<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Indexer definition fixtures

Every file here is **hand-written** for Axis's tests, in the style of the
community's indexer definition format (the "Cardigann" YAML that Prowlarr and
Jackett use). None is copied from the community's repository, and none
describes a real site: the addresses end in `.invalid`, and the pages are made
up. The definitions' own licence does not apply to these files; they are part
of Axis (AGPL-3.0-or-later).

| File | What it exercises |
|---|---|
| `example-html.yml`, `example-html-search.html`, `example-html-details.html` | An HTML site: a templated path with the words in it, inputs (`join .Categories`, an empty one left out), `keywordsfilters` (`re_replace` with `$1`, `tolower`), a `Referer` header, rows by `>` and `:has()` and `:not()` through the `tbody` a browser adds, `andmatch`, fields by `:nth-child()` and `:nth-last-child()`, attributes, `optional` with `default`, `case`, `remove`, `querystring`, `replace`, `re_replace`, `append` and `dateparse`, sizes with a decimal point and units, counts with a thousands comma, a row left open (`<td>56` then `<tr>`), a script whose text looks like rows, a row lost for a missing field, and the download read from the release's page (`download.selectors`: the first selector names another site, the second a magnet link) |
| `example-json.yml`, `example-json.json` | A JSON API: `response: json`, `categories` as a map, `eq` and `.Query.IMDBID` in inputs, dotted paths and `[0]`, an info-hash made into a magnet link, a number given as text, a row without a hash (left out as unusable), a row that is not an object, odd text, a negative count and a date far in the future (both dropped) |
| `example-rss.yml`, `example-rss.xml` | An RSS feed: `response: xml`, a path's `categories`, `$raw` with `range .Categories`, `rss > channel > item`, CDATA, `<link>` (not void in XML), `torznab:attr` read by `[name=...]`, `case` by `:contains()`, a download on another site (counted, not kept), a relative one, and `dateparse` with `zzz` |
| `example-after.yml`, `example-after.html` | A site in windows-1251 (the tests convert the page, written here in UTF-8) whose results take two rows (`rows.after: 1`), words encoded in that encoding, `timeago`, `fuzzytime`, `or` in a template, and sizes in Russian units with a decimal comma |
| `hostile-path.yml` | Reads as usable (the address is inside a template, which the judging does not render), but its path leads to another address (`@OTHER@`, put in by the test): Axis must ask nothing, anywhere |
| `hostile-links.yml`, `hostile-links-search.html`, `hostile-links-release.html` | Pages that point elsewhere: download addresses on another origin, scheme-relative, `javascript:`, the site's own pages by absolute and relative address, and a release page whose only file is at another address (`@OTHER@` and `@SELF@` are put in by the test) |

The definitions that are not usable (a login, a POST search, a search on
another host, an unknown filter, a selector or template feature the engine
lacks, date headers, a key typed in, a private site, no movie or TV category)
and those that are not definitions at all (YAML anchors, aliases, tags, block
scalars, two documents, a key twice, no id) are written inside
`tests/test-indexer-definitions.c`, each a few lines.

## What is believed about the format, not verified

Nothing here was run against a real site, and the engine was written from the
public definitions' text (read as data, never run) and from memory of how
Prowlarr's and Jackett's engine reads them. Believed:

- Field values are an element's text (its descendants' text, as a browser's
  `textContent`) or an attribute, trimmed; a field's selector picks the row
  itself when the row matches it, else the row's first matching descendant.
- `rows.after: N` merges each row of the selection with the N rows of the
  selection after it.
- A non-optional field that is not found loses its row; an optional one takes
  its `default` (a template) or empty text.
- `.Keywords` in a path is form-encoded (`+` for a space), as are inputs; an
  input whose value is empty is not sent unless `allowEmptyInputs` is true.
- A checkbox setting is "True" when checked and empty when not; `.True` is
  "True" and `.False` empty, so `eq .Config.x .False` is true when unchecked.
- `regexp` gives the first match's group 1 (empty without one); `re_replace`
  uses .NET's replacement syntax (`$1`).
- `dateparse` takes .NET layouts, with English month and day names; a time
  without a zone is taken as UTC (Prowlarr takes the machine's local time).
- `timeago` reads "5 hours ago" and the like; `fuzzytime` reads "Today
  12:25am", "Yesterday 10:30" and falls back to the general date reader.
- Sizes with units KB, MB, GB, TB are binary (1024), as Prowlarr reads them.
- A category's value is the site's identifier, mapped to Newznab numbers by
  `categorymappings` (or the `categories` map); one identifier may map to
  several.
- JSON rows are the elements of the array the rows selector names (a lone
  object is one row); a field's selector is a dotted path from the row.
- The download block's selectors are tried in order on the page at the row's
  download address; the first that gives something is used.
- A browser challenge's page carries one of a few markers (Cloudflare's
  `cf_chl_opt`, `/cdn-cgi/challenge-platform/h/`, "Just a moment...",
  DDoS-Guard's), and a site that challenges a program answers 403 or 503;
  the markers were written from memory. `add-builtin-torrent-indexers` read a
  real one (2026-11-18: a 403 with `cf-mitigated: challenge`, "Just a
  moment...", `cf_chl_opt` and `/cdn-cgi/challenge-platform/h/b/orchestrate/`)
  and found that `/cdn-cgi/challenge-platform/` alone also matches the
  detection script Cloudflare puts into the ordinary pages of the sites it
  fronts (`.../scripts/jsd/main.js`), so the marker is now the challenge's
  own path.
