#!/usr/bin/env python3
# scrub-fixture.py - turns a page a site answered (saved with
# axisd --check-definition ... --save DIR) into the start of a test fixture
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   python3 -I tests/tools/scrub-fixture.py SAVED OUT
#
# The repository is public and holds made-up values only, so a saved page is
# never committed as it came: a real info-hash, or even most of one,
# identifies the torrent and so the film, and so can a size, a date or an id
# taken together.  This does the mechanical part and lists the rest for a
# person (docs/CONTRIBUTING-definitions.md, "How to repair an indexer
# definition"):
#
#  - every 40-digit hexadecimal string (an info-hash) is replaced whole by a
#    made-up one, a two-digit unit repeated ("1111...", "1212...", ...) in
#    the order first seen, its case kept: nothing of the real one is left,
#    and a page's two mentions of one hash stay one.  Only such patterns pass
#    the check /builtin/fixtures-made-up (tests/test-indexer-builtin.c);
#  - IMDb identifiers become tt0000001, tt0000002, ...;
#  - numbers: in JSON every whole number (or number written as text) but
#    those of the members that are a page's structure or a category, and in
#    pages the numbers that are an element's whole text, those after a seeds,
#    leechers or completed word, and those of four digits or more in an
#    address, become made-up ones of the same number of digits (the first
#    digit kept, so a size stays a size), Unix times become times in
#    October 2024; one-digit numbers are kept;
#  - sizes written with a unit ("11.8 GB") keep their unit and the number of
#    digits before the point;
#  - dates ("2026-05-13 08:29:51", "13 May 2026 08:29:51") keep their year
#    and month, on the first day at noon;
#  - in HTML: comments, styles, iframes and inline handlers (onclick=...)
#    are removed, and so are scripts, except that one of Cloudflare's
#    detection script (/cdn-cgi/challenge-platform/scripts/jsd/main.js) is
#    kept as a one-line stand-in, since the engine must not take it for a
#    browser challenge;
#  - in JSON: the values of the members that name an uploader ("u",
#    "username", "uploader", "user") become maker1, maker2, ...;
#  - e-mail addresses become someone@example.invalid.
#
# What it cannot know is listed on the standard error, for a person to
# rewrite by hand before the file is a fixture: release names (to made-up
# ones such as "Made Up Film 2001 1080p WEB x264-NOGRP"), the hosts of
# addresses other than the site's own, and a magnet link's hash written in
# base 32.  Then cut the page down to the few rows the test needs, keeping
# the structure the definition reads.
#
# The fixture then has to pass `make check-made-up`
# (tests/tools/check-made-up.py), which applies the same rules (in
# tests/tools/made_up.py) to every file of the repository.
#
# It reads and writes files only, runs nothing it reads, and fetches nothing.

import json
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import made_up  # noqa: E402

HASH = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{40}(?![0-9A-Fa-f])")
BASE32_MAGNET = re.compile(r"btih:[A-Z2-7]{32}(?![A-Z2-7])", re.I)
IMDB = re.compile(r"\btt\d{7,8}\b")
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
COMMENT = re.compile(r"<!--.*?-->", re.S)
STYLE = re.compile(r"<style\b[^>]*>.*?</style\s*>", re.S | re.I)
IFRAME = re.compile(r"<iframe\b[^>]*>(.*?</iframe\s*>)?", re.S | re.I)
SCRIPT = re.compile(r"<script\b[^>]*>.*?</script\s*>", re.S | re.I)
HANDLER = re.compile(r"\s+on[a-z]+\s*=\s*(\"[^\"]*\"|'[^']*'|[^\s>]+)", re.I)
ADDRESS = re.compile(r"https?://([A-Za-z0-9.-]+)")
ADDRESS_VALUE = re.compile(r"(?:https?://|/)[^\s\"'<>]*")
DIGITS = re.compile(r"\d+")
ELEMENT_NUMBER = re.compile(r">(\s*)(\d{1,3}(?:,\d{3})+|\d{2,})(\s*)<")
COUNT_WORD = re.compile(r"((?:seed(?:er)?s?|leech(?:er)?s?|peers|completed|downloads|grabs)\b\s*:?\s*(?:&nbsp;)?\s*)"
                        r"(\d{1,3}(?:,\d{3})+|\d{2,})", re.I)
SIZE = re.compile(r"\b(\d+)(?:[.,](\d+))?(\s*(?:&nbsp;)?\s*)(B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)\b")
ISO_DATE = re.compile(r"\b(\d{4})-(\d{2})-(\d{2})([ T])(\d{2}):(\d{2}):(\d{2})")
TEXT_DATE = re.compile(r"\b(\d{1,2}) (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)([a-z]*) (\d{4}) "
                       r"(\d{2}):(\d{2}):(\d{2})")
NAMES = re.compile(r"<a\b[^>]*>([^<]{12,})</a>|<title>([^<]{12,})</title>", re.I)
USER_KEYS = ("u", "username", "uploader", "user")
NAME_KEYS = ("n", "name", "title")
# Members that are the answer's structure, or a site's category, not a fact
# about one torrent.
KEEP_KEYS = ("page_size", "count", "total", "status", "category", "cat", "c", "type", "num_files", "page",
             "per_page", "code")
TIME_KEYS = ("a", "added", "created", "created_unix", "scraped_date", "date", "time", "timestamp", "uploaded",
             "upload_date", "pubdate")
JSD = ("<script>(function(){var a=document.createElement('script');"
       "a.src='/cdn-cgi/challenge-platform/scripts/jsd/main.js';"
       "document.getElementsByTagName('head')[0].appendChild(a);})();</script>")


class Made:
    """The made-up values, each the next of its kind."""

    def __init__(self):
        self.hashes = {}
        self.imdb = {}
        self.numbers = 0
        self.times = 0
        self.dates = 0

    def hash(self, text):
        key = text.lower()
        if key not in self.hashes:
            n = len(self.hashes)
            unit = "%02x" % (0x11 + n) if n < 0xee else "%04x" % (0x1000 + n)
            self.hashes[key] = (unit * 20)[:40]
        made = self.hashes[key]
        return made.upper() if text.upper() == text else made

    def imdb_id(self, text):
        if text not in self.imdb:
            self.imdb[text] = "tt%0*d" % (len(text) - 2, len(self.imdb) + 1)
        return self.imdb[text]

    def number(self, text):
        """A made-up number with @text's digits and its commas where they
        were; the first digit kept."""
        digits = text.replace(",", "")
        if len(digits) < 2:
            return text
        self.numbers += 1
        tail = str(self.numbers % (10 ** (len(digits) - 1)))
        made = digits[0] + tail.rjust(len(digits) - 1, "0")
        out = []
        i = 0
        for ch in text:
            if ch == ",":
                out.append(",")
            else:
                out.append(made[i])
                i += 1
        return "".join(out)

    def time(self):
        self.times += 1
        return 1730000000 + 100 * self.times

    def second(self):
        self.dates += 1
        return "%02d" % (self.dates % 60)


def scrub_script(match):
    return JSD if "/cdn-cgi/challenge-platform/scripts/jsd/" in match.group(0) else ""


def scrub_json(node, made, users, names):
    if isinstance(node, dict):
        for key in list(node):
            value = node[key]
            folded = key.lower()
            if folded in USER_KEYS and isinstance(value, str):
                node[key] = users.setdefault(value, "maker%d" % (len(users) + 1))
            elif folded in NAME_KEYS and isinstance(value, str):
                names.append(value)
            elif folded in KEEP_KEYS:
                continue
            elif isinstance(value, bool):
                continue
            elif isinstance(value, int):
                if folded in TIME_KEYS or 1000000000 <= value < 2000000000:
                    node[key] = made.time()
                elif value >= 10 or value <= -10:
                    node[key] = int(made.number(str(abs(value)))) * (1 if value > 0 else -1)
            elif isinstance(value, str) and value.isdigit():
                if folded in TIME_KEYS or (len(value) == 10 and value[0] == "1"):
                    node[key] = str(made.time())
                else:
                    node[key] = made.number(value)
            else:
                scrub_json(value, made, users, names)
    elif isinstance(node, list):
        for item in node:
            scrub_json(item, made, users, names)


def scrub_page(text, made):
    text = COMMENT.sub("", text)
    text = STYLE.sub("", text)
    text = IFRAME.sub("", text)
    text = SCRIPT.sub(scrub_script, text)
    text = HANDLER.sub("", text)
    text = ISO_DATE.sub(lambda m: "%s-%s-01%s12:00:%s" % (m.group(1), m.group(2), m.group(4), made.second()), text)
    text = TEXT_DATE.sub(lambda m: "01 %s%s %s 12:00:%s" % (m.group(2), m.group(3), m.group(4), made.second()), text)
    text = SIZE.sub(lambda m: "%s%s%s%s" % (made.number(m.group(1)) if len(m.group(1)) > 1 else m.group(1),
                                            ".25" if m.group(2) else "", m.group(3), m.group(4)), text)
    text = COUNT_WORD.sub(lambda m: m.group(1) + made.number(m.group(2)), text)
    text = ELEMENT_NUMBER.sub(lambda m: ">%s%s%s<" % (m.group(1), made.number(m.group(2)), m.group(3)), text)
    return text


def scrub_addresses(text, made):
    """Ids in addresses: runs of four digits or more, except a hash's (made
    up afterwards, whole)."""
    def address(match):
        value = match.group(0)
        if HASH.search(value):
            return value
        return DIGITS.sub(lambda m: made.number(m.group(0)) if len(m.group(0)) >= 4 else m.group(0), value)
    return ADDRESS_VALUE.sub(address, text)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: scrub-fixture.py SAVED OUT\n")
        return 2
    with open(argv[1], "r", encoding="utf-8", errors="replace") as source:
        text = source.read()
    made = Made()
    names = []
    users = {}
    try:
        document = json.loads(text)
    except ValueError:
        document = None
    if document is not None:
        scrub_json(document, made, users, names)
        text = json.dumps(document, ensure_ascii=False)
    else:
        text = scrub_page(text, made)
    text = scrub_addresses(text, made)
    text = IMDB.sub(lambda m: made.imdb_id(m.group(0)), text)
    text = HASH.sub(lambda m: made.hash(m.group(0)), text)
    text = EMAIL.sub("someone@example.invalid", text)
    if document is None:
        names = [a or b for a, b in NAMES.findall(text) if not HASH.fullmatch((a or b).strip())]
    names = list(dict.fromkeys(name.strip() for name in names))
    with open(argv[2], "w", encoding="utf-8") as out:
        out.write(text)
    hosts = sorted(set(ADDRESS.findall(text)))
    sys.stderr.write("written: %s\n" % argv[2])
    sys.stderr.write("made up: %d info-hashes, %d IMDb identifiers, %d numbers, %d times, %d dates; "
                     "%d uploader names replaced\n" % (len(made.hashes), len(made.imdb), made.numbers, made.times,
                                                       made.dates, len(users)))
    if BASE32_MAGNET.search(text):
        sys.stderr.write("a magnet link holds a hash in base 32: replace it by hand\n")
    left = [run for run in HASH.findall(text) if not made_up.made_up_hash(run)]
    if left:
        sys.stderr.write("NOT MADE UP, replace by hand: %s\n" % ", ".join(left))
        return 1
    sys.stderr.write("rewrite these %d names to made-up ones by hand (every place each appears):\n" % len(names))
    for name in names[:40]:
        sys.stderr.write("  %s\n" % name)
    if len(names) > 40:
        sys.stderr.write("  ... and %d more: cut the page to a few rows first\n" % (len(names) - 40))
    sys.stderr.write("hosts named in it (keep the site's own; make the others .example.invalid; "
                     "* = not reserved or named in tests/tools/made_up.py):\n")
    for host in hosts:
        sys.stderr.write("  %s%s\n" % ("" if made_up.named_host(host) else "* ", host))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
