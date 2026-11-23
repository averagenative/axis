#!/usr/bin/env python3
# check-made-up.py - the repository's check that it holds made-up values only
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   python3 -I tests/tools/check-made-up.py [PATH...]   the check (make check-made-up)
#   python3 -I tests/tools/check-made-up.py --titles [PATH...]
#   python3 -I tests/tools/check-made-up.py --self-test   the rules on samples
#
# The repository is public, and its tests, fixtures, documents and code hold
# made-up values only: nothing a real service answered, and nothing of an
# owner's or contributor's own setup.  This reads every file git tracks (or
# each PATH given; the tree under the current directory, without build/,
# deps/ and .git, when there is no git) and fails, naming each file and
# line, on:
#
#  - a run of 40 hexadecimal digits (an info-hash, or a commit id), or a
#    magnet link's base-32 hash, that is not one of the made-up patterns
#    made_up.made_up_hash() accepts; tests/tools/scrub-fixture.py writes
#    those;
#  - an IMDb identifier (tt and seven or eight digits), or a TMDb or TheTVDB
#    identifier where a name or an address says what it is ({tmdb-N},
#    [tvdbid-N], "tmdbId": N, /3/movie/N, ...), that is not made up
#    (made_up.made_up_imdb(), made_up.made_up_provider_id());
#  - a host name, in an address or bare, that is neither reserved for
#    examples (example.org, .example, .invalid, .test, ...) nor one of the
#    hosts the code and the documents must name (made_up.NAMED_HOSTS);
#  - an e-mail address outside the reserved names, a private IPv4 address
#    of a host that made_up.PRIVATE_ADDRESSES does not list, and a home
#    directory with a user's name in it (/home/NAME, /Users/NAME);
#  - a word of a local list, when there is one: a plain text file outside
#    the repository, one word per line, named by the environment variable
#    AXIS_MADE_UP_PRIVATE_WORDS (tests/tools/made_up.py).  With no such
#    file this part is skipped.
#
# Text is also read with its percent-encoding undone ("btih%3A...",
# "https%3A%2F%2F..."), and file names are read as well as contents.  In C,
# header and JavaScript files only string literals and comments are read
# for bare host names (a member access such as st.st_mtim.tv_sec looks like
# one).
#
# What it cannot tell is whether a title is real: "Made Up Film (2001)" and
# a real film's name look alike.  --titles lists every title-like string
# ("Name (Year)", "Name.S01E01", "Name.2019.1080p") once, with how often it
# appears and where first, for a person to judge (it never fails); see
# tests/fixtures/README.md.  Nor can it tell a real TMDb identifier written
# as a bare number in a fixture ("id": N) from a made-up one.
#
# It reads files only, runs nothing but `git ls-files`, and fetches nothing.

import collections
import os
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import made_up  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CODE_SUFFIXES = (".c", ".h", ".js")
PERCENT = re.compile(r"%([0-9A-Fa-f]{2})")
TITLE_YEAR = re.compile(r"(?<![\w'])([A-Z][^\s()\"\[\]{}<>/\\|]*(?: [^\s()\"\[\]{}<>/\\|]+){0,8}) \(((?:19|20)\d\d)\)")
TITLE_DOTTED = re.compile(r"(?<![\w'.])([A-Z][A-Za-z0-9'&!+]*(?:[._ -][A-Za-z0-9'&!+]+){0,9}?)[._ ]"
                          r"(?:(?:19|20)\d\d(?![\dpi])|S\d{1,2}E\d{1,3}|S\d{1,2}(?![\dE]))")


def tracked_files(paths):
    if paths:
        out = []
        for path in paths:
            if os.path.isdir(path):
                for top, dirs, names in os.walk(path):
                    dirs.sort()
                    out.extend(os.path.join(top, name) for name in sorted(names))
            else:
                out.append(path)
        return out
    try:
        listing = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
        names = [name.decode("utf-8", "replace") for name in listing.split(b"\0") if name]
    except (OSError, subprocess.CalledProcessError):
        names = []
        for top, dirs, files in os.walk(ROOT):
            dirs[:] = sorted(d for d in dirs if d not in (".git", "build", "deps"))
            names.extend(os.path.relpath(os.path.join(top, name), ROOT) for name in sorted(files))
    return [os.path.join(ROOT, name) for name in names if os.path.isfile(os.path.join(ROOT, name))]


def code_text(text):
    """@text with everything but string literals and comments blanked
    (lines kept)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(text[i:end])
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(text[i:end])
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c if c == "\n" else " ")
            i += 1
    return "".join(out)


def decoded(line):
    return PERCENT.sub(lambda m: chr(int(m.group(1), 16)), line) if "%" in line else line


def check_line(line, bare_line, found, private=frozenset()):
    """Adds to @found (kind, value) for each value of @line that is not
    made up; @bare_line is the text read for bare host names, @private the
    local word list (made_up.private_words())."""
    for text in {line, decoded(line)}:
        for m in made_up.HASH.finditer(text):
            if not made_up.made_up_hash(m.group(0)):
                found.add(("info-hash or commit id not made up", m.group(0)))
        for m in made_up.BASE32_MAGNET.finditer(text):
            if not made_up.made_up_hash(m.group(1)):
                found.add(("base-32 info-hash not made up", m.group(1)))
        for m in made_up.IMDB.finditer(text):
            if not made_up.made_up_imdb(m.group(1)):
                found.add(("IMDb identifier not made up", m.group(0)))
        for m in made_up.PROVIDER_ID.finditer(text):
            number = next(g for g in m.groups() if g is not None)
            if not made_up.made_up_provider_id(number):
                found.add(("TMDb or TheTVDB identifier not made up", m.group(0)))
        for m in made_up.URL_HOST.finditer(text):
            if not made_up.named_host(m.group(1)):
                found.add(("host name not reserved or named", m.group(1).lower()))
        for m in made_up.EMAIL.finditer(text):
            if not made_up.made_up_email(m.group(0), m.group(1)):
                found.add(("e-mail address not made up", m.group(0)))
        for m in made_up.IPV4.finditer(text):
            if not made_up.made_up_ipv4(m.groups()[:4], m.group(5)):
                found.add(("private address of a host not listed", m.group(0)))
        for m in made_up.HOME.finditer(text):
            if m.group(1) not in made_up.HOME_NAMES:
                found.add(("home directory with a user's name", m.group(0)))
    if bare_line:
        for m in made_up.BARE_HOST.finditer(decoded(bare_line)):
            if not made_up.named_host(m.group(1)):
                found.add(("host name not reserved or named", m.group(1)))
    if private and not private.isdisjoint(made_up.WORD.findall(decoded(line).lower())):
        found.add(("a word on the local list", ""))


def read(path):
    with open(path, "rb") as source:
        return source.read().decode("utf-8", "replace")


def check(paths):
    failures = 0
    private = made_up.private_words()
    for path in tracked_files(paths):
        name = os.path.relpath(path, ROOT)
        found = set()
        check_line(name, name, found, private)
        for kind, value in sorted(found):
            print("%s: (file name) %s%s" % (name, kind, ": " + value if value else ""))
            failures += 1
        text = read(path)
        bare = code_text(text) if path.endswith(CODE_SUFFIXES) else text
        for number, (line, bare_line) in enumerate(zip(text.split("\n"), bare.split("\n")), 1):
            found = set()
            check_line(line, bare_line, found, private)
            for kind, value in sorted(found):
                print("%s:%d: %s%s" % (name, number, kind, ": " + value if value else ""))
                failures += 1
    if failures:
        print("%d value%s not made up (tests/tools/made_up.py says what is; "
              "tests/fixtures/README.md how to replace one)" % (failures, "" if failures == 1 else "s"))
        return 1
    return 0


def titles(paths):
    seen = collections.OrderedDict()
    for path in tracked_files(paths):
        name = os.path.relpath(path, ROOT)
        for number, line in enumerate(read(path).split("\n"), 1):
            for pattern in (TITLE_YEAR, TITLE_DOTTED):
                for m in pattern.finditer(line):
                    title = re.sub(r"[._]", " ", m.group(1)).strip()
                    entry = seen.setdefault(title.lower(), [title, 0, "%s:%d" % (name, number)])
                    entry[1] += 1
    for key in sorted(seen):
        title, count, where = seen[key]
        print("%5d  %-50s  %s" % (count, title, where))
    return 0


def self_test():
    """The rules on values of each kind, made up and not (the ones that are
    not are written in pieces, so that this file holds none whole)."""
    def dotted(*labels):
        return ".".join(labels)

    def refused(line, bare=True):
        found = set()
        check_line(line, line if bare else "", found)
        return {kind for kind, _ in found}

    real_hash = "d41c8a9b7e2f6013" + "5a4c9e8b7d6f5a4c3b2e1d0f"
    cases = [
        ("magnet:?xt=urn:btih:" + "e1e2e3e4" * 5, False),
        ("magnet:?xt=urn:btih:" + real_hash, True),
        ("magnet:?xt=urn:btih%3A" + real_hash.upper(), True),
        ("magnet:?xt=urn:btih:" + "MFRGGZDF" * 4, False),
        ("magnet:?xt=urn:btih:" + "AEBAGBAF" + "AYDQQCIK" + "BMGA2DQP" + "CAIREEYU", True),
        ("https://www.imdb.com/title/tt0600001/", False),
        ("https://www.imdb.com/title/tt" + "0118" + "276/", True),
        ("Hearth (1995) {tmdb-6001}", False),
        ("Something (1995) {tmdb-" + "94" + "9}", True),
        ("https://api.newsowl.example/api", False),
        ("https://" + dotted("api", "someindexer", "ws") + "/api", True),
        ("see tracker.opentrackr.org and indexer.test", False),
        ("udp%3A%2F%2F" + dotted("open", "sometracker", "com") + "%3A1337", True),
        ("trusted_proxies = 10.0.0.0/8, 192.0.2.7, 198.51.100.4", False),
        ("the server at 192.168." + "1.77", True),
        ("someone@example.invalid", False),
        ("someone@" + dotted("somemail", "com"), True),
        ("/home/you/axis", False),
        ("/home/" + "jdoe/axis", True),
    ]
    failures = 0
    for line, expect_refused in cases:
        if bool(refused(line)) != expect_refused:
            print("self-test: %s for %r" % ("not refused" if expect_refused else "refused", line))
            failures += 1
    # A member access in C is no host name; in a string it is checked.
    if refused("st.st_mtim.tv_sec = 0;", bare=False) or not refused("\"a host " + dotted("www", "somewhere", "com") + "\""):
        print("self-test: bare host names in code")
        failures += 1
    # The local word list: no file, no words and nothing refused; a file's
    # words are refused as words of their own, in any case, and not inside
    # another word.
    with tempfile.TemporaryDirectory() as scratch:
        listed = os.path.join(scratch, "words")
        with open(listed, "w", encoding="utf-8") as out:
            out.write("# a comment\n\nSomeBox\nlan-name\n")
        none = made_up.load_private_words(os.path.join(scratch, "absent"))
        words = made_up.load_private_words(listed)

        def on_list(line, private):
            found = set()
            check_line(line, line, found, private)
            return ("a word on the local list", "") in found

        if (none or made_up.load_private_words("") or words != frozenset(("somebox", "lan", "name"))
                or on_list("ssh somebox", none) or not on_list("ssh SOMEBOX:/srv", words)
                or on_list("somebox1 and asomebox", words)):
            print("self-test: the local word list")
            failures += 1
    return 1 if failures else 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) >= 2 and argv[1] == "--titles":
        return titles(argv[2:])
    if len(argv) >= 2 and argv[1].startswith("-"):
        sys.stderr.write("usage: check-made-up.py [--titles] [PATH...] | --self-test\n")
        return 2
    return check(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
