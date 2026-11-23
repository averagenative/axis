#!/bin/sh
# up.sh - start a throwaway Jellyfin server for the compatibility tests
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/jellyfin/up.sh STATEFILE
#
# Starts a Jellyfin container on a podman (else docker) network that has
# no route out, published on 127.0.0.1 only, with a small generated media
# tree (make-media.sh) mounted read-only.  Completes the first-run wizard
# through the server's own API, adds a Films and a Music library, waits
# for the scan to finish, and creates an API key.  Then writes STATEFILE,
# a shell fragment that sets:
#
#   AXIS_TEST_JELLYFIN_NAME      container name (axis-jellyfin-test-...)
#   AXIS_TEST_JELLYFIN_NETWORK   network name
#   AXIS_TEST_JELLYFIN_MEDIA     host directory holding the media
#   AXIS_TEST_JELLYFIN_URL       http://127.0.0.1:PORT
#   AXIS_TEST_JELLYFIN_KEY       the API key
#   AXIS_TEST_JELLYFIN_USER      an administrator's name and password,
#   AXIS_TEST_JELLYFIN_PASSWORD  for the test harness's own requests
#
# Everything it creates carries the axis-jellyfin-test- prefix.  Remove
# it with `sh tests/jellyfin/down.sh STATEFILE`.  If this script fails
# half way it removes what it made before it exits.
#
# The server has no route to the internet by design: nothing is fetched,
# no metadata provider is consulted, no plugin repository is asked.
#
#   AXIS_TEST_JELLYFIN_IMAGE   override the image

set -u

# Jellyfin 10.11.8, pulled 2026-11-13 (image built 2026-04-05).  Pinned by
# digest so that the recorded fixtures stay true: to move to a newer
# server, pull the tag, read the digest with
# `podman image inspect --format '{{.Digest}}' docker.io/jellyfin/jellyfin:TAG`,
# re-run `make check-jellyfin`, and re-record tests/fixtures/compat/jellyfin/recorded.
image=${AXIS_TEST_JELLYFIN_IMAGE:-docker.io/jellyfin/jellyfin:10.11.8@sha256:1ebca12ee68116ddc6bd38ea01af2db50d6e4cee64a5076981ef75c35709d5e8}

if [ $# -ne 1 ]; then
	echo "usage: $0 STATEFILE" >&2
	exit 2
fi
state=$1
here=$(dirname "$0")

if command -v podman >/dev/null 2>&1; then
	runtime=podman
elif command -v docker >/dev/null 2>&1; then
	runtime=docker
else
	echo "jellyfin: neither podman nor docker is installed" >&2
	exit 1
fi
for tool in curl jq ffmpeg; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "jellyfin: $tool is needed" >&2
		exit 1
	fi
done

suffix=$$-$(od -An -N4 -tx4 /dev/urandom | tr -d ' ')
name=axis-jellyfin-test-$suffix
network=axis-jellyfin-test-net-$suffix
password=$(od -An -N12 -tx1 /dev/urandom | tr -d ' \n')
media=
made_network=0
made_container=0

cleanup_failed() {
	if [ "$made_container" = 1 ]; then
		$runtime rm -f "$name" >/dev/null 2>&1
	fi
	if [ "$made_network" = 1 ]; then
		$runtime network rm "$network" >/dev/null 2>&1
	fi
	case $media in
	*/axis-jellyfin-test-media.*) rm -rf -- "$media" ;;
	esac
}
fail() {
	echo "jellyfin: $*" >&2
	cleanup_failed
	exit 1
}
trap 'cleanup_failed; exit 129' HUP
trap 'cleanup_failed; exit 130' INT
trap 'cleanup_failed; exit 143' TERM

media=$(mktemp -d "${TMPDIR:-/tmp}/axis-jellyfin-test-media.XXXXXX") ||
	fail "cannot make a directory for the media"
sh "$here/make-media.sh" "$media" || fail "cannot generate the media tree"

# --internal: the network has no route out.  Ports can still be
# published to 127.0.0.1.
$runtime network create --internal "$network" >/dev/null ||
	fail "cannot create the network"
made_network=1

extra=
if [ "$runtime" = podman ]; then
	# A backstop for a SIGKILLed script.
	extra="--timeout=7200"
fi
made_container=1
# shellcheck disable=SC2086
$runtime run --detach --rm $extra --name "$name" \
	--label axis-test=1 \
	--network "$network" \
	--publish 127.0.0.1::8096 \
	--volume "$media:/media:ro,Z" \
	"$image" >/dev/null || {
	made_container=0
	fail "cannot start $image"
}

port=
i=0
while [ $i -lt 1800 ]; do
	port=$($runtime port "$name" 8096/tcp 2>/dev/null | sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' | head -n 1)
	if [ -n "$port" ] &&
		curl -fsS -m 2 -o /dev/null "http://127.0.0.1:$port/System/Info/Public" 2>/dev/null; then
		break
	fi
	port=
	if [ "$($runtime inspect -f '{{.State.Running}}' "$name" 2>/dev/null)" != true ]; then
		$runtime logs "$name" >&2 2>&1
		fail "the Jellyfin container stopped while starting"
	fi
	sleep 0.1
	i=$((i + 1))
done
[ -n "$port" ] || fail "Jellyfin did not answer within 180 s"
url=http://127.0.0.1:$port

json='Content-Type: application/json'
auth_head='Authorization: MediaBrowser Client="axis-test-setup", Device="axis-test-setup", DeviceId="axis-test-setup", Version="1.0"'

# The public endpoint answers before the server has finished starting
# (the wizard endpoints say 503 until then).
i=0
while [ $i -lt 600 ]; do
	if curl -fsS -m 5 -o /dev/null "$url/Startup/Configuration" 2>/dev/null; then
		break
	fi
	sleep 0.5
	i=$((i + 1))
done

# Wizard.  The sequence is the one the server's own web client uses.
curl -fsS -m 30 -X POST -H "$json" \
	-d '{"UICulture":"en-US","MetadataCountryCode":"US","PreferredMetadataLanguage":"en"}' \
	"$url/Startup/Configuration" >/dev/null || fail "wizard: configuration"
# The first read creates the default user the next call renames.
curl -fsS -m 30 "$url/Startup/User" >/dev/null || fail "wizard: read user"
curl -fsS -m 30 -X POST -H "$json" \
	-d "{\"Name\":\"axis-test\",\"Password\":\"$password\"}" \
	"$url/Startup/User" >/dev/null || fail "wizard: user"
curl -fsS -m 30 -X POST -H "$json" \
	-d '{"EnableRemoteAccess":true,"EnableAutomaticPortMapping":false}' \
	"$url/Startup/RemoteAccess" >/dev/null || fail "wizard: remote access"
curl -fsS -m 30 -X POST "$url/Startup/Complete" >/dev/null ||
	fail "wizard: complete"

token=$(curl -fsS -m 30 -X POST -H "$json" -H "$auth_head" \
	-d "{\"Username\":\"axis-test\",\"Pw\":\"$password\"}" \
	"$url/Users/AuthenticateByName" | jq -r '.AccessToken')
[ -n "$token" ] && [ "$token" != null ] || fail "cannot sign in"
auth="Authorization: MediaBrowser Token=\"$token\""

# Libraries, with every lookup on the internet switched off.
add_library() {
	curl -fsS -m 60 -X POST -H "$json" -H "$auth" \
		-d "{\"LibraryOptions\":{\"EnableInternetProviders\":false,\"EnableRealtimeMonitor\":false,\"PathInfos\":[{\"Path\":\"$3\"}]}}" \
		"$url/Library/VirtualFolders?name=$1&collectionType=$2&refreshLibrary=false" >/dev/null
}
add_library Films movies /media/films || fail "cannot add the Films library"
add_library Music music /media/music || fail "cannot add the Music library"
curl -fsS -m 30 -X POST -H "$auth" "$url/Library/Refresh" >/dev/null ||
	fail "cannot start a library scan"

# The scan is done when all four items are there, each with its media
# sources probed (the film items carry a path and streams).
i=0
done_scan=0
while [ $i -lt 600 ]; do
	n=$(curl -fsS -m 10 -H "$auth" \
		"$url/Items?Recursive=true&IncludeItemTypes=Movie,Audio&Fields=MediaSources" |
		jq '[.Items[] | select((.MediaSources | length) > 0 and (.MediaSources[0].MediaStreams | length) > 0)] | length' 2>/dev/null)
	if [ "${n:-0}" = 4 ]; then
		done_scan=1
		break
	fi
	sleep 0.5
	i=$((i + 1))
done
[ "$done_scan" = 1 ] || fail "the library scan did not finish within 5 minutes"

curl -fsS -m 30 -X POST -H "$auth" "$url/Auth/Keys?app=axis-test" >/dev/null ||
	fail "cannot create an API key"
key=$(curl -fsS -m 30 -H "$auth" "$url/Auth/Keys" |
	jq -r '[.Items[] | select(.AppName == "axis-test")][0].AccessToken')
[ -n "$key" ] && [ "$key" != null ] || fail "cannot read the API key back"

umask 077
{
	echo "AXIS_TEST_JELLYFIN_NAME='$name'"
	echo "AXIS_TEST_JELLYFIN_NETWORK='$network'"
	echo "AXIS_TEST_JELLYFIN_MEDIA='$media'"
	echo "AXIS_TEST_JELLYFIN_URL='$url'"
	echo "AXIS_TEST_JELLYFIN_KEY='$key'"
	echo "AXIS_TEST_JELLYFIN_USER='axis-test'"
	echo "AXIS_TEST_JELLYFIN_PASSWORD='$password'"
	echo "AXIS_TEST_JELLYFIN_RUNTIME='$runtime'"
} >"$state" || fail "cannot write $state"
trap - HUP INT TERM
