#!/bin/sh
# check-hwenc.sh - the real-encode conversion tests with the hardware
# encoders (NVENC, VA-API) too
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: sh tests/check-hwenc.sh PROGRAM      (make check-hwenc)
#
# Runs PROGRAM (test-convert-real) with AXIS_TEST_HWENC set: 1 (the
# default) for both families, or what the caller set (nvenc, vaapi, or both
# comma-separated).  The test program runs one encode at a time, on
# one-second test clips in a private scratch directory.
#
# The NVIDIA card may be shared with other work, so before anything runs
# its free memory is read once, where nvidia-smi is installed: with less
# than AXIS_HWENC_MIN_FREE_MIB (default 500) free on any NVIDIA GPU, or no
# reading, the NVENC runs are left out, and this says so.  Without
# nvidia-smi the memory cannot be read, and NVENC runs if the capability
# check finds it.

set -u

if [ $# -ne 1 ]; then
	echo "usage: sh tests/check-hwenc.sh PROGRAM" >&2
	exit 2
fi
program=$1
want=${AXIS_TEST_HWENC:-1}
minimum=${AXIS_HWENC_MIN_FREE_MIB:-500}

case ",$want," in
*,1,*|*,all,*|*,nvenc,*) nvenc=yes ;;
*) nvenc=no ;;
esac
case ",$want," in
*,1,*|*,all,*|*,vaapi,*) vaapi=yes ;;
*) vaapi=no ;;
esac

if [ "$nvenc" = yes ]; then
	if command -v nvidia-smi >/dev/null 2>&1; then
		free=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits 2>/dev/null | sort -n | head -n 1 | tr -d ' ')
		case $free in
		''|*[!0-9]*)
			echo "check-hwenc: nvidia-smi gave no reading of the free memory: the NVENC runs are left out"
			nvenc=no
			;;
		*)
			if [ "$free" -lt "$minimum" ]; then
				echo "check-hwenc: $free MiB free on the NVIDIA GPU, less than $minimum MiB: the NVENC runs are left out"
				nvenc=no
			else
				echo "check-hwenc: $free MiB free on the NVIDIA GPU: the NVENC runs go ahead"
			fi
			;;
		esac
	else
		echo "check-hwenc: nvidia-smi is not installed, so the NVIDIA GPU's free memory cannot be read"
	fi
fi

if [ "$nvenc" = yes ] && [ "$vaapi" = yes ]; then
	families=nvenc,vaapi
elif [ "$nvenc" = yes ]; then
	families=nvenc
elif [ "$vaapi" = yes ]; then
	families=vaapi
else
	families=0
	echo "check-hwenc: no hardware family left to run: the software encoders only"
fi

echo "  HWENC   $program (AXIS_TEST_HWENC=$families)"
AXIS_TEST_HWENC=$families "$program" --verbose
