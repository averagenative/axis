#!/bin/sh
# make-def-trace-corpus.sh - builds the seed corpus of fuzz-def-trace from the
# definitions Axis ships (data/definitions) and their made-up fixtures
# (tests/fixtures/builtin): each seed is a mode byte, a definition, a NUL byte
# and an answer, so the fuzzer starts from real definitions read against pages
# shaped like their sites'.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   sh tests/fuzz/make-def-trace-corpus.sh
#
# Run from the source directory; rewrites tests/fuzz/corpus/fuzz-def-trace/.
# The mode byte: its low two bits choose the search (0 a movie, 1 an episode,
# 2 words), the next bit excerpts ('0' 0x30: a movie; '6' 0x36: words, with
# excerpts; '5' 0x35: an episode, with excerpts).

set -eu

out=tests/fuzz/corpus/fuzz-def-trace
defs=data/definitions
pages=tests/fixtures/builtin
mkdir -p "$out"

# seed NAME MODE DEFINITION PAGE
seed() {
	{ printf '%s' "$2"; cat "$defs/$3"; printf '\000'; cat "$pages/$4"; } > "$out/$1"
}

seed limetorrents-movie 0 axis-limetorrents.yml limetorrents-search.xml
seed limetorrents-empty 6 axis-limetorrents.yml limetorrents-empty.xml
seed torrentproject-movie 4 axis-torrentproject.yml torrentproject-search.html
seed torrentproject-empty 2 axis-torrentproject.yml torrentproject-empty.html
seed torrentproject-wrong-page 6 axis-torrentproject.yml torrentproject-details.html
seed therarbg-movie 0 axis-therarbg.yml therarbg-search.json
seed therarbg-episode 5 axis-therarbg.yml therarbg-empty.json
seed torrentscsv-words 6 axis-torrentscsv.yml torrentscsv-search.json
seed torrentscsv-empty 2 axis-torrentscsv.yml torrentscsv-empty.json
# A JSON site that answers a page, and a page site that answers JSON.
seed torrentscsv-drift 6 axis-torrentscsv.yml torrentproject-empty.html
seed limetorrents-drift 4 axis-limetorrents.yml therarbg-search.json

echo "seeds in $out: $(ls "$out" | wc -l)"
