#!/bin/sh
# make-probe-corpus.sh - builds the seed corpus of fuzz-probe-map from the
# probe fixtures: each seed is a report from the container-and-streams call
# and, where there is one, "@@FRAMES@@" and the frame call's report.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   sh tests/fuzz/make-probe-corpus.sh
#
# Run from the source directory; rewrites tests/fuzz/corpus/fuzz-probe-map/.

set -eu

out=tests/fuzz/corpus/fuzz-probe-map
mkdir -p "$out"

for dir in tests/fixtures/probe/recorded tests/fixtures/probe/constructed; do
	for streams in "$dir"/*.json; do
		case "$streams" in
		*.frames.json) continue ;;
		esac
		base=$(basename "$streams" .json)
		frames="$dir/$base.frames.json"
		if [ -f "$frames" ]; then
			{ cat "$streams"; printf '\n@@FRAMES@@\n'; cat "$frames"; } > "$out/$base"
		else
			cat "$streams" > "$out/$base"
		fi
	done
done

echo "seeds in $out: $(ls "$out" | wc -l)"
