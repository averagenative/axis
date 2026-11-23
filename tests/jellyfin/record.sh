#!/bin/sh
# record.sh - save a real server's answers, for tests/fixtures/compat/jellyfin/recorded
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/jellyfin/with-jellyfin.sh sh tests/jellyfin/record.sh OUTDIR
#
# Asks the server the questions the daemon asks (GET only), before and
# after the test clients of play.sh have started playing, and writes each
# answer pretty-printed (jq, 2 spaces; the content is otherwise the
# server's, key for key) under OUTDIR, which must not be a project
# directory: copy what you want to keep into the fixtures yourself.
# Also saves the parts of the server's own API description that name the
# values the parsers must know.
#
#   identity.json            GET /System/Info
#   public.json              GET /System/Info/Public (no key)
#   libraries.json           GET /Library/VirtualFolders
#   devices-idle.json        GET /Devices, before any client played
#   sessions-idle.json       GET /Sessions, before any client played
#   items.json               GET /Items?Recursive=true&IncludeItemTypes=Movie,Audio&Fields=Path
#   sessions-playing.json    GET /Sessions, while the clients play
#   devices-playing.json     GET /Devices, after they played
#   api-description.json     the enumerations from /api-docs/openapi.json
#   VERSION                  the server's name and version
#
# Needs AXIS_TEST_JELLYFIN_URL and _KEY (see up.sh).

set -eu

if [ $# -ne 1 ]; then
	echo "usage: $0 OUTDIR" >&2
	exit 2
fi
out=$1
here=$(dirname "$0")
u=$AXIS_TEST_JELLYFIN_URL
adm="Authorization: MediaBrowser Token=\"$AXIS_TEST_JELLYFIN_KEY\""
mkdir -p "$out"

get() {
	# get FILE PATH [noauth]
	if [ $# -ge 3 ]; then
		curl -fsS -m 60 "$u$2" | jq . >"$out/$1"
	else
		curl -fsS -m 60 -H "$adm" "$u$2" | jq . >"$out/$1"
	fi
}

get identity.json /System/Info
get public.json /System/Info/Public noauth
get libraries.json /Library/VirtualFolders
get devices-idle.json /Devices
get sessions-idle.json /Sessions
get items.json "/Items?Recursive=true&IncludeItemTypes=Movie,Audio&Fields=Path"

AXIS_TEST_JELLYFIN_CLIENTS="${AXIS_TEST_JELLYFIN_CLIENTS:-axis-test-tv axis-test-phone axis-test-stick axis-test-tablet axis-test-console axis-test-hotel axis-test-speaker}" \
	sh "$here/play.sh"

get sessions-playing.json /Sessions
get devices-playing.json /Devices

curl -fsS -m 60 "$u/api-docs/openapi.json" |
	jq '{source: "GET /api-docs/openapi.json on the test server",
	     version: .info.version,
	     TranscodeReason: .components.schemas.TranscodeReason.enum,
	     PlayMethod: .components.schemas.PlayMethod.enum,
	     TranscodingInfo: (.components.schemas.TranscodingInfo.properties | keys),
	     SessionInfoDto: (.components.schemas.SessionInfoDto.properties | keys)}' \
	>"$out/api-description.json"

jq -r '"\(.ProductName) \(.Version)"' "$out/identity.json" >"$out/VERSION"
