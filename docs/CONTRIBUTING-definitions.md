# How to repair an indexer definition

This page shows you how to repair a site definition when a torrent site
changes its pages, and how to send the repair to the repository with a test.
Allow about an hour for a first repair.

Axis reads sites that have no API, such as TorrentProject and LimeTorrents,
with site definitions: YAML files in the format that Prowlarr and Jackett
use. The definitions that Axis ships are in `data/definitions/`. A site can
change its pages at any time, so a definition needs a repair from time to
time. You do not need a new build of Axis to use a repair: an administrator
can paste it on the definition's page.

## Before you begin

You need:

- A built `axisd`. In a container, run the commands on this page with
  `podman exec axis axisd ...`. In a source tree, run `make` first.
- For the repository part: a source tree, `python3`, and the build
  prerequisites in the [README](../README.md).

## How Axis tells a changed page from an empty one

Each definition that Axis ships says two things that community definitions
do not:

- `version:` at the top: the definition's version, a whole number. Raise it
  with every change.
- `expect:` under `search.rows`: a selector (or, for a JSON answer, a path)
  for the part that every answer of the site has, with results or without.
  For example, `#similarfiles` for TorrentProject and `rss > channel` for
  LimeTorrents.

When the part that `expect` names is missing, when the answer is not the
HTML, XML or JSON that the definition reads, or when more than half of the
results lack a field they must have, the indexer shows "its pages no longer
look as expected". Searches then pass the indexer over for an hour, and the
dashboard lists it under **Needs attention**.

## Find what changed

1. On the indexer's page, select **Check this indexer**. Enter a title that
   the site has, select **Show an excerpt of each row read**, and select
   **Check it**.
2. Read the account from the top:

   | You see | It means |
   | --- | --- |
   | **What every answer has** says **NOT FOUND** | The page has a new layout, or the address now belongs to someone else. Read the page excerpt. |
   | **Rows** says **matched 0**, and the part is found | The site found nothing, or the row selector no longer fits. Try a title that the site surely has. |
   | A field says **matched nothing** in most rows | That field's selector no longer fits. The row excerpt shows what the row holds now. |
   | **Browser challenge** | The site puts a challenge in front of programs. Axis cannot pass it, and no definition change helps. Try another address of the site. |

## Save the page and repair the definition

1. Save the site's answer to a folder:

   ```
   axisd --check-definition axis-limetorrents --words "a title" --save /tmp/axis-pages
   ```

   This asks the live site once, through the same HTTP client and outbound
   binding as the daemon, and writes each answer as `answer-1.html` (or
   `.xml`, `.json`). It needs no database and no running daemon.

2. Copy the definition. Use `data/definitions/axis-limetorrents.yml` from the
   source tree, or open the definition's page in Axis and copy **The file in
   use, to start a repair from**.

3. Change what no longer fits: the row selector, `expect`, or a field's
   selector or filters. Then raise `version` by one.

4. Check the copy against the saved page, as often as you need. This asks no
   site:

   ```
   axisd --check-definition my-limetorrents.yml --page /tmp/axis-pages/answer-1.xml --words "a title" --excerpts
   ```

   The repair works when the verdict says "read" and the rows you expect say
   **kept**. To check how a release's own page gives its download, add
   `--release-page` with a saved release page.

## Use the repair now

Use one of the following:

- Open the definition's page (**System**, **Indexers**, the definition's
  name under **Other torrent sites, through site definitions**). In **Repairing
  it: your own version**, paste the file and select **Use this version**.
- Put the file in the data directory as `definitions/<id>.yml`, for example
  `definitions/axis-limetorrents.yml`, and restart Axis. Axis takes the file
  only when its version is also later than the one you pasted on the page,
  if you pasted one. The log says why when it does not take the file.

Axis checks the file before it uses it: it must read as a definition, keep
the built-in id, say a later version than the built-in one, and build its
search requests. The indexers that run the definition use your version from
their next search, and a hold for pages that no longer looked as expected
ends. A site's own request to wait does not end. The definition's page says
which version is in use and where it came from. When a later Axis ships a
version that is as new as yours or newer, Axis uses its own again, and the
definition's page says that your version is no longer used. To go back to
the built-in version, select **Remove your version: use the built-in one**.

## Send the repair to the repository

The repository is public and holds made-up titles only. Never commit a page
that a site answered as it came.

1. Scrub the saved page:

   ```
   python3 -I tests/tools/scrub-fixture.py /tmp/axis-pages/answer-1.xml /tmp/limetorrents-search.xml
   ```

   The script replaces every info-hash whole with a made-up pattern, such
   as `1111...` and `1212...`. Even most of a real hash identifies the
   torrent, and so the film. It also replaces IMDb identifiers, sizes,
   dates, counts and the ids in addresses with made-up values of the same
   shape. It removes scripts, styles, comments, frames and inline handlers,
   and replaces uploader names in JSON. Then it lists what you must still
   rewrite by hand: the release names and the hosts of other sites. It exits
   with an error if any hash in its output is not a made-up pattern.

2. Cut the scrubbed page to a few rows. Keep the elements, attributes and
   members that the definition reads, exactly as the site has them.

3. Rewrite every release name to a made-up one, such as `Made Up Film 2001
   1080p WEB x264-NOGRP`, in every place it appears (text, link addresses,
   titles). Make the hosts of other sites end in `.example.invalid`. If you
   type a hash by hand, use a pattern such as `a7a7a7...` (a short unit
   repeated). Never change only a few digits of a real one.

4. Replace the fixture in `tests/fixtures/builtin/`, and describe what it
   stands for in that folder's `README.md`. The test
   `/builtin/fixtures-made-up` fails on any hash in the fixtures that is
   not a made-up pattern.

5. Put your repaired definition in `data/definitions/`, with its raised
   version.

6. Update the site's test in `tests/test-indexer-builtin.c` so that it
   checks what the new page gives, then rebuild the fuzz seeds:

   ```
   sh tests/fuzz/make-def-trace-corpus.sh
   ```

7. Run `make check`. It builds the definitions into the daemon, runs the
   tests, and compiles the fuzz targets.

## What a definition may not do

Axis holds every definition to the following rules, and refuses one that
breaks them:

- It asks only its site's own address. Axis never follows an address that a
  page names to another host. It reads an info-hash and builds the magnet
  link itself.
- It needs no login and no key.
- Everything it reads is bounded: the size of an answer, the number of rows,
  the time a regular expression may take.
