#!/bin/sh
# make-media.sh - generate the small media tree the Jellyfin tests use
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/jellyfin/make-media.sh DIR
#
# Writes a minute of synthetic video (long enough that a conversion is
# still running when a test looks) and six seconds of audio under DIR
# with ffmpeg's test sources (no real media, nothing downloaded):
#
#   films/Test Film H264 (2024)/...mkv    H.264 8-bit + AAC stereo
#   films/Test Film HEVC10 (2024)/...mkv  HEVC 10-bit + AAC stereo
#   films/Test Film Multi (2024)/...mkv   H.264 + two audio tracks
#                                         (AAC English, AC-3 German)
#                                         + an SRT subtitle track
#   music/Test Artist/Test Album/01 Test Tone.flac   FLAC audio
#
# The paths follow Jellyfin's naming rules ("Name (Year)/Name (Year).ext")
# so that the server recognises every film without looking anything up.

set -eu

if [ $# -ne 1 ]; then
	echo "usage: $0 DIR" >&2
	exit 2
fi

dir=$1
here=$(dirname "$0")
srt=$here/subtitle.srt
ff="ffmpeg -hide_banner -loglevel error -y"
video="-f lavfi -i testsrc2=size=640x360:rate=24:duration=60"
tone="-f lavfi -i sine=frequency=440:duration=60"
tone2="-f lavfi -i sine=frequency=660:duration=60"

mkdir -p "$dir/films/Test Film H264 (2024)" \
	"$dir/films/Test Film HEVC10 (2024)" \
	"$dir/films/Test Film Multi (2024)" \
	"$dir/music/Test Artist/Test Album"

# shellcheck disable=SC2086
$ff $video $tone -c:v libx264 -pix_fmt yuv420p -preset ultrafast \
	-c:a aac -b:a 96k \
	"$dir/films/Test Film H264 (2024)/Test Film H264 (2024).mkv"

# shellcheck disable=SC2086
$ff $video $tone -c:v libx265 -pix_fmt yuv420p10le -preset ultrafast \
	-x265-params log-level=error -c:a aac -b:a 96k \
	"$dir/films/Test Film HEVC10 (2024)/Test Film HEVC10 (2024).mkv"

# shellcheck disable=SC2086
$ff $video $tone $tone2 -i "$srt" \
	-map 0:v -map 1:a -map 2:a -map 3:s \
	-c:v libx264 -pix_fmt yuv420p -preset ultrafast \
	-c:a:0 aac -b:a:0 96k -c:a:1 ac3 -b:a:1 128k -c:s srt \
	-metadata:s:a:0 language=eng -metadata:s:a:1 language=deu \
	-metadata:s:s:0 language=eng \
	"$dir/films/Test Film Multi (2024)/Test Film Multi (2024).mkv"

# shellcheck disable=SC2086
$ff -f lavfi -i sine=frequency=440:duration=6 -c:a flac \
	-metadata title="Test Tone" -metadata artist="Test Artist" \
	-metadata album="Test Album" -metadata track=1 \
	"$dir/music/Test Artist/Test Album/01 Test Tone.flac"
