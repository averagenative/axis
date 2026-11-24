#!/bin/sh
# run-target.sh - run one fuzz target for FUZZ_SECONDS
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: FUZZ_SECONDS=20 FUZZ_OUTDIR=build/fuzz tests/fuzz/run-target.sh build/fuzz/fuzz-NAME
#
# The target reads the seed corpus next to this script (corpus/fuzz-NAME)
# and its dictionary (fuzz-NAME.dict) when there is one.  New inputs go to
# FUZZ_OUTDIR/corpus/fuzz-NAME, never into the seed corpus; a crash is
# saved under FUZZ_OUTDIR/artifacts as fuzz-NAME-<kind>-<hash> and the
# target exits non-zero.  `make fuzz` calls this for every target.

set -u

bin=${1:?usage: $0 FUZZ-BINARY}
name=${bin##*/}
src=$(dirname "$0")
out=${FUZZ_OUTDIR:?FUZZ_OUTDIR is not set}
seconds=${FUZZ_SECONDS:?FUZZ_SECONDS is not set}

mkdir -p "$out/corpus/$name" "$out/artifacts" || exit 1

dict=
if [ -f "$src/$name.dict" ]; then
	dict=-dict=$src/$name.dict
fi

# shellcheck disable=SC2086
exec "$bin" -max_total_time="$seconds" -print_final_stats=1 $dict \
	-artifact_prefix="$out/artifacts/$name-" \
	"$out/corpus/$name" "$src/corpus/$name"
