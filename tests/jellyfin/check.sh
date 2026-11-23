#!/bin/sh
# check.sh - make the test clients play, then run the live check
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/jellyfin/with-jellyfin.sh sh tests/jellyfin/check.sh PROGRAM
#
# Runs inside with-jellyfin.sh (AXIS_TEST_JELLYFIN_URL and the rest are set):
# play.sh reports playback to the throwaway server the way nine clients do,
# then PROGRAM, the live check (tests/jellyfin/test-jellyfin-live.c), reads
# the server as the daemon does and asserts what it recorded.

set -eu

if [ $# -ne 1 ]; then
	echo "usage: $0 PROGRAM" >&2
	exit 2
fi
here=$(dirname "$0")

sh "$here/play.sh"
exec "$1"
