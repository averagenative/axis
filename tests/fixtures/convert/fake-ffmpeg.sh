#!/bin/sh
# fake-ffmpeg.sh - stands in for ffmpeg in the capability check's tests
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   /bin/sh fake-ffmpeg.sh MODE FFMPEG ARGUMENTS...
#
# Runs the real FFMPEG with ARGUMENTS, except where MODE says to lie:
#
#   no-device    the hardware encoders are listed and fail as a machine
#                without the device makes them fail (also in every mode
#                below: the tests do not depend on this machine's GPU)
#   eight-bit    a test encode asked for 10 bits gets 8 (the picture format
#                of the planned output stream is rewritten)
#   hdr-altered  the HDR10 test encode is given other mastering display
#                values than the plan's (the minimum luminance changed)
#   hdr-dropped  the HDR10 test encode loses its values: they are deleted
#                from the frames (the sidedata filter, so that ffmpeg does
#                not pass them on) and left out of x265's parameters, as an
#                encoder that ignores them would do (removing the SEI with
#                filter_units is not enough: x265 also puts it in the
#                stream's extradata, which a raw remux still shows)
#   version-a,   `-version` says another version; every encode fails at
#   version-b    once (the tests of re-measuring want speed, not results)
#   bad-version  `-version` prints something that is not ffmpeg's

mode=$1
real=$2
shift 2

case " $* " in
*" -version "*)
	case $mode in
	version-a) echo "ffmpeg version 99.1-fake-a Copyright (c) the fake"; exit 0 ;;
	version-b) echo "ffmpeg version 99.2-fake-b Copyright (c) the fake"; exit 0 ;;
	bad-version) echo "this is not the program you are looking for"; exit 0 ;;
	esac
	exec "$real" "$@"
	;;
*" -encoders "*)
	"$real" "$@" || exit $?
	echo " V....D hevc_nvenc           (fake) NVIDIA NVENC hevc encoder (codec hevc)"
	echo " V....D h264_nvenc           (fake) NVIDIA NVENC H.264 encoder (codec h264)"
	echo " V....D hevc_vaapi           (fake) H.265/HEVC (VAAPI) (codec hevc)"
	echo " V....D h264_vaapi           (fake) H.264/AVC (VAAPI) (codec h264)"
	exit 0
	;;
esac

case $mode in
version-a|version-b)
	echo "the fake encodes nothing" >&2
	exit 1
	;;
esac

for a in "$@"; do
	case $a in
	hevc_nvenc|h264_nvenc)
		echo "[$a @ 0x0] No NVENC capable devices found" >&2
		exit 1
		;;
	hevc_vaapi|h264_vaapi)
		echo "[AVHWDeviceContext @ 0x0] No VA display found for device /dev/dri/renderD128." >&2
		exit 1
		;;
	esac
done

# Rewrites, argument by argument: the loop reads the arguments as they
# were, and each (possibly changed) one is appended; the originals are
# shifted away at the end.
n=$#
prev=
for a in "$@"; do
	out=$a
	case $mode:$prev in
	eight-bit:-pix_fmt:0)
		[ "$a" = yuv420p10le ] && out=yuv420p
		;;
	hdr-altered:-x265-params:0)
		case $a in
		*master-display=*)
			out="colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,1):max-cll=1000,400:log-level=error"
			;;
		esac
		;;
	hdr-dropped:-x265-params:0)
		case $a in
		*master-display=*)
			out="colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:log-level=error"
			;;
		esac
		;;
	hdr-dropped:-filter:0)
		case $a in
		*smpte2084*)
			out="$a,sidedata=mode=delete:type=MASTERING_DISPLAY_METADATA,sidedata=mode=delete:type=CONTENT_LIGHT_LEVEL"
			;;
		esac
		;;
	esac
	set -- "$@" "$out"
	prev=$a
done
shift $n
exec "$real" "$@"
