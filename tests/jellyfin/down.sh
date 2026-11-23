#!/bin/sh
# down.sh - remove what up.sh made
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/jellyfin/down.sh STATEFILE
#
# Removes the container, the network and the media directory named in
# STATEFILE, then STATEFILE itself.  Safe to run twice.  It touches only
# names that start with axis-jellyfin-test-.

set -u

if [ $# -ne 1 ]; then
	echo "usage: $0 STATEFILE" >&2
	exit 2
fi
state=$1
[ -f "$state" ] || exit 0

AXIS_TEST_JELLYFIN_NAME=
AXIS_TEST_JELLYFIN_NETWORK=
AXIS_TEST_JELLYFIN_MEDIA=
AXIS_TEST_JELLYFIN_RUNTIME=podman
# shellcheck disable=SC1090
. "$state"
runtime=$AXIS_TEST_JELLYFIN_RUNTIME

case $AXIS_TEST_JELLYFIN_NAME in
axis-jellyfin-test-*) $runtime rm -f "$AXIS_TEST_JELLYFIN_NAME" >/dev/null 2>&1 ;;
esac
case $AXIS_TEST_JELLYFIN_NETWORK in
axis-jellyfin-test-net-*) $runtime network rm "$AXIS_TEST_JELLYFIN_NETWORK" >/dev/null 2>&1 ;;
esac
case $AXIS_TEST_JELLYFIN_MEDIA in
*/axis-jellyfin-test-media.*) rm -rf -- "$AXIS_TEST_JELLYFIN_MEDIA" ;;
esac
rm -f -- "$state"
exit 0
