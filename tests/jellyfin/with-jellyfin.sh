#!/bin/sh
# with-jellyfin.sh - run a command against a throwaway Jellyfin
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/jellyfin/with-jellyfin.sh COMMAND [ARG...]
#
# Starts the server (up.sh), runs COMMAND with AXIS_TEST_JELLYFIN_URL,
# AXIS_TEST_JELLYFIN_KEY and the other variables up.sh documents set, and
# removes everything (down.sh) when COMMAND ends, fails or is interrupted.
# Exits with COMMAND's status.

set -u

if [ $# -eq 0 ]; then
	echo "usage: $0 COMMAND [ARG...]" >&2
	exit 2
fi

here=$(dirname "$0")
state=$(mktemp "${TMPDIR:-/tmp}/axis-jellyfin-test-state.XXXXXX") || exit 1

cleanup() {
	sh "$here/down.sh" "$state"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# up.sh writes the state file only when it has finished; it removes what
# it made itself if it fails.  An empty state file means it failed.
if ! sh "$here/up.sh" "$state" || [ ! -s "$state" ]; then
	echo "with-jellyfin: could not start Jellyfin" >&2
	exit 1
fi

# shellcheck disable=SC1090
. "$state"
export AXIS_TEST_JELLYFIN_NAME AXIS_TEST_JELLYFIN_NETWORK \
	AXIS_TEST_JELLYFIN_MEDIA AXIS_TEST_JELLYFIN_URL AXIS_TEST_JELLYFIN_KEY \
	AXIS_TEST_JELLYFIN_USER AXIS_TEST_JELLYFIN_PASSWORD AXIS_TEST_JELLYFIN_RUNTIME

"$@"
exit $?
