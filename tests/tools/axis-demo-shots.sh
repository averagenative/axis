#!/bin/sh
# axis-demo-shots.sh - the demo instance and its screenshots, in one go
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/tools/axis-demo-shots.sh OUTDIR [SCENE] [SCREENSHOT-OPTION...]
#
# Run from the top of the repository after building the tests' tools
# (`make build/gcc-release/tests/tools/axis-demo`, or any `make test`).
# Starts a throwaway PostgreSQL (tests/with-postgres.sh), the demo instance
# of tests/tools/axis-demo.c with SCENE (full, the default; empty; setup) on
# 127.0.0.1 at a random port above 20000, and headless Chrome through
# tests/tools/axis-screenshots.py, which logs in and captures every page the
# demo lists at 390x844 and 1440x900 into OUTDIR (keep it outside the
# repository), with report.txt saying which pages scroll sideways on a
# phone.  Everything it started is stopped at the end.  Further options go to
# axis-screenshots.py (for example `--only title-lantern,series-harbour` or
# `--widths phone`).
#
#   AXIS_DEMO   the demo program (default build/gcc-release/tests/tools/axis-demo)
#   AXIS_DEMO_ATTENTION   made-up notices for the bell in the header and the
#               page "Needs attention" (the demo's --attention; default 0).
#               With `--widths phone360,phone,tablet768,wide1024,wide1280 --only
#               dashboard` the report says where each part of the header is at
#               each width, for 0, 3 and 140 (a bell that reads "99+").

set -u

if [ "${1:-}" = "--inside" ]; then
	# Under tests/with-postgres.sh: the demo, then the screenshots.
	shift
	out=$1
	scene=$2
	shift 2
	demo=${AXIS_DEMO:-build/gcc-release/tests/tools/axis-demo}
	port=$((20001 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 40000))
	ready="$out/demo-ready.txt"
	pages="$out/demo-pages.txt"
	rm -f "$ready"
	"$demo" --port "$port" --scene "$scene" --attention "${AXIS_DEMO_ATTENTION:-0}" --ready-file "$ready" \
		--pages-file "$pages" >"$out/demo.log" 2>&1 &
	pid=$!
	trap 'kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null' EXIT
	i=0
	while [ ! -s "$ready" ]; do
		if ! kill -0 "$pid" 2>/dev/null; then
			echo "axis-demo-shots: the demo stopped; see $out/demo.log" >&2
			exit 1
		fi
		i=$((i + 1))
		if [ $i -gt 600 ]; then
			echo "axis-demo-shots: the demo did not serve within 120 s; see $out/demo.log" >&2
			exit 1
		fi
		sleep 0.2
	done
	url=$(sed -n 's/^url=//p' "$ready")
	if [ "$scene" = setup ]; then
		python3 -I tests/tools/axis-screenshots.py --base "$url" --pages "$pages" --out "$out" "$@"
	else
		python3 -I tests/tools/axis-screenshots.py --base "$url" --user admin --password demo-admin-password \
			--pages "$pages" --out "$out" "$@"
	fi
	status=$?
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	trap - EXIT
	exit $status
fi

if [ $# -lt 1 ]; then
	echo "usage: $0 OUTDIR [full|empty|setup] [SCREENSHOT-OPTION...]" >&2
	exit 2
fi
out=$1
shift
scene=full
case "${1:-}" in
full|empty|setup)
	scene=$1
	shift
	;;
esac
mkdir -p "$out" || exit 1
exec tests/with-postgres.sh sh "$0" --inside "$out" "$scene" "$@"
