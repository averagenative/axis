#!/bin/sh
# fake-ffprobe.sh - stands in for ffprobe in tests/test-import.c, so that a
# test can say how long a "film" runs and what its picture is without making
# a two-hour video: the test file's first line says it.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# It is run as `/bin/sh fake-ffprobe.sh <the probe's own arguments>` and reads
# the file it is given as descriptor 3, as ffprobe would.  A file whose first
# line is
#
#   AXISFAKE duration=7080 width=1920 height=800 vcodec=h264 acodec=dts aprofile=DTS-HD_MA channels=6
#
# is reported as a Matroska file with one video stream (unless "novideo" is
# among the words) and one audio stream (when acodec is given); underscores in
# aprofile stand for spaces.  "fail" among the words, or any other first line,
# is a file ffprobe cannot read.

for argument in "$@"; do
	if [ "$argument" = "-show_frames" ]; then
		echo '{"frames": []}'
		exit 0
	fi
done

line=$(head -c 400 <&3 | head -n 1)
case "$line" in
AXISFAKE*)
	;;
*)
	echo "fd:: Invalid data found when processing input" >&2
	exit 1
	;;
esac

duration=0
width=0
height=0
vcodec=h264
acodec=
aprofile=
channels=0
video=1
for word in $line; do
	case "$word" in
	duration=*) duration=${word#duration=} ;;
	width=*) width=${word#width=} ;;
	height=*) height=${word#height=} ;;
	vcodec=*) vcodec=${word#vcodec=} ;;
	acodec=*) acodec=${word#acodec=} ;;
	aprofile=*) aprofile=$(echo "${word#aprofile=}" | tr '_' ' ') ;;
	channels=*) channels=${word#channels=} ;;
	novideo) video=0 ;;
	fail)
		echo "fd:: Invalid data found when processing input" >&2
		exit 1
		;;
	esac
done

streams=""
index=0
if [ "$video" = 1 ]; then
	streams="{\"index\": $index, \"codec_name\": \"$vcodec\", \"codec_type\": \"video\", \"width\": $width,"
	streams="$streams \"height\": $height, \"pix_fmt\": \"yuv420p\", \"r_frame_rate\": \"24/1\","
	streams="$streams \"avg_frame_rate\": \"24/1\", \"bits_per_raw_sample\": \"8\", \"color_range\": \"tv\","
	streams="$streams \"color_space\": \"bt709\", \"color_transfer\": \"bt709\", \"color_primaries\": \"bt709\","
	streams="$streams \"disposition\": {\"default\": 1, \"attached_pic\": 0}}"
	index=1
fi
if [ -n "$acodec" ]; then
	if [ -n "$streams" ]; then
		streams="$streams, "
	fi
	streams="$streams{\"index\": $index, \"codec_name\": \"$acodec\", \"codec_type\": \"audio\""
	if [ -n "$aprofile" ]; then
		streams="$streams, \"profile\": \"$aprofile\""
	fi
	streams="$streams, \"channels\": $channels, \"sample_rate\": \"48000\","
	streams="$streams \"disposition\": {\"default\": 1, \"attached_pic\": 0}}"
fi

printf '{"streams": [%s], "chapters": [], "format": {"filename": "fd:", "nb_streams": 2, ' "$streams"
printf '"format_name": "matroska,webm", "duration": "%s.000000", "size": "1000", ' "$duration"
printf '"bit_rate": "8000000", "probe_score": 100}}\n'
exit 0
