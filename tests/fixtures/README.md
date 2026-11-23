# Fixtures hold made-up values only

The repository is public. Its fixtures, fuzz seeds, tests, code and documents
hold made-up titles, identifiers, hosts and addresses only, and nothing of an
owner's or a contributor's own setup: no host or domain name of their
machines, no home directory, no LAN address, no account, and no answer of a
real service copied as it came.

A recorded answer (`prowlarr/recorded/`, `compat/jellyfin/recorded/`, the
pages under `builtin/`) is a **structurally faithful recording with invented
content**: the members, their order and types, the nesting, the counts and
the lengths that matter are what the service sent; every value that would
identify a real release, title, indexer, account or machine is made up. Each
directory's README says what was kept and what was replaced.

## The check

`make check-made-up` (part of `make check`; Python 3, no network) runs
`tests/tools/check-made-up.py` over every tracked file. It fails on:

- a run of 40 hexadecimal digits (an info-hash or a commit id), or a magnet
  link's base-32 hash, that is not a made-up pattern: a unit of at most 16
  characters repeated (`e1e2e3e4e1e2e3e4...`, `5a5a5a...`) or ten runs of four
  of one digit (`aaaa1111bbbb2222...`);
- an IMDb identifier that is not made up: made-up ones hold four zeros in a
  row (`tt0000001`, `tt0600001`, `tt7000005`), one digit repeated, or a run of
  digits (`tt1234567`);
- a TMDb or TheTVDB identifier, where a name or an address says what it is
  (`{tmdb-6001}`, `[tvdbid-82001]`, `"tmdbId": 7101`, `/3/movie/6003`), that is
  not under 100, a run, or one of the made-up numbers `tests/tools/made_up.py`
  lists (6001 and on for films, 7001 and on for series);
- a host name, in an address or bare, that is neither reserved for examples
  (`example.org`, `*.example`, `*.invalid`, `*.test`, `localhost`, and any name
  with an `example` label) nor one of the hosts the code and documents must
  name (`made_up.NAMED_HOSTS`: the services Axis talks to, the sites it
  supports by name, standards, registries);
- an e-mail address outside those names, a private IPv4 address of a host
  that `made_up.PRIVATE_ADDRESSES` does not list (networks and the
  documentation ranges are fine), and a home directory with a user's name;
- a word of a local list, when you keep one. To have the check also refuse
  words of your own, such as the names of your machines, write them one per
  line in a plain text file outside the repository and set the environment
  variable `AXIS_MADE_UP_PRIVATE_WORDS` to its path. With no such file, this
  part of the check is skipped.

It cannot tell a real title from a made-up one. `check-made-up.py --titles
[PATH...]` lists every title-like string ("Name (Year)", "Name.S01E01",
"Name.2019.1080p") once, with its count and first place, for a person to
judge. Nor can it tell a real TMDb identifier written as a bare number in a
fixture (`"id": N`) from a made-up one.

## Adding a fixture from a real answer

1. Save the answer (`axisd --check-definition ... --save DIR` for a site's
   pages) and run `python3 -I tests/tools/scrub-fixture.py SAVED OUT`: it makes
   up the hashes, identifiers, numbers, dates, uploaders and e-mail addresses,
   and lists what it cannot know.
2. Rewrite by hand what it lists: release names and titles to made-up ones of
   the same shape (keep what the test is about: an accent, a numeral, a year
   inside the title, a word that is also a quality or language word), and
   hosts other than the site's own to `.example` or `.invalid` names.
3. Cut the answer to the few entries the test needs, keeping its structure.
4. Run `make check-made-up`, and `--titles` on the new file.

Made-up titles in use, for their shapes: "Made Up Film (2001)", "Hearth
(1995)" (a one-word title with namesakes of other years), "The Mazework"
(with "Reloaded" and "Revolutions", a title that is a prefix of others),
"Cracking Slate" (a series), "Copper Road", "Aurélie" (an accent), "Lantern
Row".

## The one real title

The setup guide's token check asks the real metadata service for a film it
must know, so `AXIS_WEB_SETUP_CHECK_TITLE` in
`src/web/axis-web-setup-private.h` is a real title, on purpose. It is named
there only; the page and the tests use the constant.
