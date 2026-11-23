#!/bin/sh
# play.sh - act as a handful of Jellyfin clients, for the test server only
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/jellyfin/play.sh
#
# Needs AXIS_TEST_JELLYFIN_URL, _KEY, _USER and _PASSWORD (see up.sh).
# AXIS_TEST_JELLYFIN_CLIENTS, a space-separated list of device ids, limits
# which of the clients below play (default: all).
#
# The daemon never does this: it only reads.  A session exists on a
# Jellyfin server only while a client reports playback, so the tests make
# nine small clients do what real ones do:
#
#   1. sign in with the client's own device id and name,
#   2. ask for the item's playback info with a device profile (what the
#      device says it can play),
#   3. when the server answers with a conversion, request the stream so
#      the server starts it (as a real player does),
#   4. report playback started and one progress report.
#
# The device profiles differ so that the server states different reasons
# for converting.  The clients are left playing; the caller reads the
# server's session list (record.sh) and tears the server down.

set -eu

u=$AXIS_TEST_JELLYFIN_URL
adm="Authorization: MediaBrowser Token=\"$AXIS_TEST_JELLYFIN_KEY\""
json='Content-Type: application/json'

# Device profiles: what each device says it can take.
dp_all='[{"Container":"mkv,mp4","Type":"Video","VideoCodec":"h264,hevc","AudioCodec":"aac,ac3"},{"Container":"flac","Type":"Audio"}]'
dp_mp4='[{"Container":"mp4","Type":"Video","VideoCodec":"h264","AudioCodec":"aac"},{"Container":"flac","Type":"Audio"}]'
dp_aac='[{"Container":"mkv,mp4","Type":"Video","VideoCodec":"h264,hevc","AudioCodec":"aac"},{"Container":"flac","Type":"Audio"}]'
cp_none='[]'
cp_bitdepth8='[{"Type":"Video","Codec":"hevc","Conditions":[{"Condition":"LessThanEqual","Property":"VideoBitDepth","Value":"8","IsRequired":true}]}]'
cp_level60='[{"Type":"Video","Codec":"hevc","Conditions":[{"Condition":"LessThanEqual","Property":"VideoLevel","Value":"60","IsRequired":true}]}]'
trans='[{"Container":"ts","Type":"Video","Protocol":"hls","VideoCodec":"h264","AudioCodec":"aac","Context":"Streaming","MaxAudioChannels":"2","MinSegments":1,"BreakOnNonKeyFrames":true},{"Container":"mp3","Type":"Audio","Protocol":"http","AudioCodec":"mp3","Context":"Streaming"}]'

item_id() {
	curl -fsS -m 30 -H "$adm" "$u/Items?Recursive=true&IncludeItemTypes=Movie,Audio" |
		jq -r --arg n "$1" '.Items[] | select(.Name | contains($n)) | .Id' | head -n 1
}

# client DEVICE_ID DEVICE_NAME APP ITEM DIRECTPLAY CODECPROFILES MAXBITRATE EXTRA_JSON
client() {
	dev=$1 devname=$2 app=$3 item=$4 dp=$5 cp=$6 maxbr=$7 extra=$8
	case " ${AXIS_TEST_JELLYFIN_CLIENTS:-all} " in
	*" all "* | *" $dev "*) ;;
	*) return 0 ;;
	esac
	head="Authorization: MediaBrowser Client=\"$app\", Device=\"$devname\", DeviceId=\"$dev\", Version=\"1.0\""
	auth=$(curl -fsS -m 30 -X POST -H "$json" -H "$head" \
		-d "{\"Username\":\"$AXIS_TEST_JELLYFIN_USER\",\"Pw\":\"$AXIS_TEST_JELLYFIN_PASSWORD\"}" \
		"$u/Users/AuthenticateByName")
	tok=$(echo "$auth" | jq -r .AccessToken)
	uid=$(echo "$auth" | jq -r .User.Id)
	a="Authorization: MediaBrowser Token=\"$tok\""
	id=$(item_id "$item")
	[ -n "$id" ] || { echo "play: no item $item" >&2; exit 1; }
	# The stream indexes in EXTRA are honoured only with a media source id
	# (a file's single source has the item's id).
	body=$(jq -cn --arg uid "$uid" --arg id "$id" --argjson dp "$dp" --argjson cp "$cp" \
		--argjson tr "$trans" --argjson maxbr "$maxbr" --argjson extra "$extra" \
		'{UserId: $uid, MediaSourceId: $id,
		  DeviceProfile: {Name: "axis-test", MaxStreamingBitrate: $maxbr,
		    DirectPlayProfiles: $dp, TranscodingProfiles: $tr,
		    CodecProfiles: $cp, SubtitleProfiles: []}} + $extra')
	info=$(curl -fsS -m 60 -X POST -H "$json" -H "$a" -d "$body" "$u/Items/$id/PlaybackInfo")
	psid=$(echo "$info" | jq -r .PlaySessionId)
	msid=$(echo "$info" | jq -r '.MediaSources[0].Id')
	turl=$(echo "$info" | jq -r '.MediaSources[0].TranscodingUrl // empty')
	method=DirectPlay
	if [ -n "$turl" ]; then
		method=Transcode
		base=${turl%%/master.m3u8*}
		main=$(curl -fsS -m 60 -H "$a" "$u$turl" | grep -v '^#' | head -n 1)
		seg=$(curl -fsS -m 120 -H "$a" "$u$base/$main" | grep -v '^#' | head -n 1)
		curl -fsS -m 120 -o /dev/null -H "$a" "$u$base/$seg"
	elif [ "$(echo "$info" | jq -r '.MediaSources[0].SupportsDirectPlay')" != true ]; then
		method=DirectStream
	fi
	# A real player reports the audio track and the subtitle track it chose.
	report="{\"ItemId\":\"$id\",\"MediaSourceId\":\"$msid\",\"PlaySessionId\":\"$psid\",\"PlayMethod\":\"$method\",\"CanSeek\":true,\"IsPaused\":false"
	chosen=$(echo "$extra" | jq -r '.AudioStreamIndex // empty')
	[ -z "$chosen" ] || report="$report,\"AudioStreamIndex\":$chosen"
	chosen=$(echo "$extra" | jq -r '.SubtitleStreamIndex // empty')
	[ -z "$chosen" ] || report="$report,\"SubtitleStreamIndex\":$chosen"
	curl -fsS -m 30 -X POST -H "$json" -H "$a" -d "$report,\"PositionTicks\":0}" \
		"$u/Sessions/Playing" >/dev/null
	curl -fsS -m 30 -X POST -H "$json" -H "$a" -d "$report,\"PositionTicks\":20000000}" \
		"$u/Sessions/Playing/Progress" >/dev/null
	echo "play: $devname: $item as $method" >&2
}

client axis-test-tv        "Living Room TV"  AxisTestPlayer  "Test Film H264"   "$dp_all" "$cp_none"      120000000 '{}'
client axis-test-phone     "Pixel Phone"     AxisTestPhone   "Test Film HEVC10" "$dp_mp4" "$cp_none"      120000000 '{}'
client axis-test-stick     "Bedroom Stick"   AxisTestStick   "Test Film H264"   "$dp_mp4" "$cp_none"      120000000 '{}'
client axis-test-tablet    "Tablet"          AxisTestTablet  "Test Film HEVC10" "$dp_all" "$cp_bitdepth8" 120000000 '{}'
client axis-test-projector "Projector"       AxisTestBeam    "Test Film HEVC10" "$dp_all" "$cp_level60"   120000000 '{}'
client axis-test-console   "Games Console"   AxisTestConsole "Test Film Multi"  "$dp_aac" "$cp_none"      120000000 '{"AudioStreamIndex":2}'
client axis-test-browser   "Web Browser"     AxisTestWeb     "Test Film Multi"  "$dp_all" "$cp_none"      120000000 '{"SubtitleStreamIndex":3}'
client axis-test-hotel     "Hotel TV"        AxisTestHotel   "Test Film H264"   "$dp_all" "$cp_none"      500000    '{}'
client axis-test-speaker   "Kitchen Speaker" AxisTestSpeaker "Test Tone"        "$dp_all" "$cp_none"      120000000 '{}'
