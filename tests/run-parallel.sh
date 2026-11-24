#!/bin/sh
# run-parallel.sh - run programs several at a time, each with a log of its own
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/run-parallel.sh JOBS LOGDIR LABEL [--with SCRIPT] ITEM...
#                              [--serial ITEM...]
#
# Runs every ITEM (a program; with --with, `sh SCRIPT ITEM`), JOBS at a
# time, then the ITEMs after --serial one at a time, on their own.  What an
# ITEM writes goes to LOGDIR/<its file name>.log, and the terminal gets one
# line when it ends:
#
#   <LABEL>  <ITEM> (<seconds> s)          it passed
#   FAIL     <ITEM> (<seconds> s, status <N>)
#
# Every ITEM is run, whatever the others did.  At the end come the whole
# log of each one that failed, the ten that took longest, and the names of
# those that failed.  Exits 0 when all passed, else 1.
#
# An ITEM that has not ended yet has a .log in LOGDIR and no .status: that
# is where to look when the run stands still.
#
# `make test` and `make fuzz` call this (TEST_JOBS, FUZZ_JOBS).  It needs
# an xargs with -P, which GNU's and the BSDs' have; nothing else beyond
# POSIX sh.

set -u

usage() {
	echo "usage: $0 JOBS LOGDIR LABEL [--with SCRIPT] ITEM... [--serial ITEM...]" >&2
	exit 2
}

# One ITEM: called by xargs below, never by hand.  Always exits 0, so that
# xargs goes on to the rest; the status is in the .status file.
if [ "${1:-}" = --one ]; then
	[ $# -eq 5 ] || usage
	logdir=$2
	label=$3
	with=$4
	item=$5
	name=${item##*/}
	start=$(date +%s)
	if [ "$with" = - ]; then
		"$item" >"$logdir/$name.log" 2>&1
	else
		sh "$with" "$item" >"$logdir/$name.log" 2>&1
	fi
	status=$?
	seconds=$(($(date +%s) - start))
	echo "$status $seconds $item" >"$logdir/$name.status"
	if [ $status -eq 0 ]; then
		printf '  %-7s %s (%s s)\n' "$label" "$item" "$seconds"
	else
		printf '  %-7s %s (%s s, status %s)\n' FAIL "$item" "$seconds" "$status"
	fi
	exit 0
fi

[ $# -ge 3 ] || usage
jobs=$1
logdir=$2
label=$3
shift 3

case $jobs in
	''|*[!0-9]*|0) echo "$0: JOBS must be a number from 1: '$jobs'" >&2; exit 2 ;;
esac

with=-
if [ "${1:-}" = --with ]; then
	[ $# -ge 2 ] || usage
	with=$2
	shift 2
fi

mkdir -p "$logdir" || exit 1
rm -f "$logdir"/*.log "$logdir"/*.status "$logdir"/parallel.list "$logdir"/serial.list

: >"$logdir/parallel.list"
: >"$logdir/serial.list"
list=$logdir/parallel.list
for item in "$@"; do
	if [ "$item" = --serial ]; then
		list=$logdir/serial.list
		continue
	fi
	echo "$item" >>"$list"
done

began=$(date +%s)

# (An xargs given nothing still runs its command once: hence the tests.)
if [ -s "$logdir/parallel.list" ]; then
	xargs -n 1 -P "$jobs" sh "$0" --one "$logdir" "$label" "$with" <"$logdir/parallel.list"
fi
if [ -s "$logdir/serial.list" ]; then
	echo "  The rest run one at a time:"
	xargs -n 1 -P 1 sh "$0" --one "$logdir" "$label" "$with" <"$logdir/serial.list"
fi

took=$(($(date +%s) - began))

# What each one did.  One that left no .status never ended (xargs was
# stopped, or could not start it): it counts as failed.
total=0
failed=
for item in $(cat "$logdir/parallel.list" "$logdir/serial.list"); do
	name=${item##*/}
	total=$((total + 1))
	status=
	if [ -f "$logdir/$name.status" ]; then
		read -r status seconds path <"$logdir/$name.status"
	fi
	if [ "$status" != 0 ]; then
		failed="$failed $item"
		echo
		echo "==== $item failed (status ${status:-none: it did not end}); its log, $logdir/$name.log:"
		if [ -f "$logdir/$name.log" ]; then
			cat "$logdir/$name.log"
		fi
		echo "==== end of $item"
	fi
done

echo
echo "The ten that took longest (seconds):"
cat "$logdir"/*.status 2>/dev/null | sort -k 2,2nr | head -n 10 | while read -r status seconds path; do
	printf '  %5s  %s\n' "$seconds" "$path"
done

if [ -n "$failed" ]; then
	echo
	echo "Failed:"
	for item in $failed; do
		echo "  $item"
	done
	echo "$total run, $jobs at a time, in $took s; the logs are in $logdir"
	exit 1
fi

echo "$total run, $jobs at a time, in $took s; the logs are in $logdir"
exit 0
