#!/bin/bash
# check-convert-container.sh - the real-encode tests on the container image's
# ffmpeg
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: bash tests/check-convert-container.sh      (or: make check-convert-container)
#
# The image's ffmpeg is another version than this machine's (Debian's 7.1
# against Fedora's 8.1 today), and a conversion's arguments must hold for
# both.  The test program (tests/test-convert-real.c) has to run where that
# ffmpeg is, so:
#
#   1. the test program is compiled on the image's own Debian
#      (tests/convert/Containerfile.check: deploy/Containerfile's build
#      stage, whose cached layers it reuses, building only this program, so
#      that the check does not depend on every other test passing there);
#   2. the program is copied out to a private directory here;
#   3. it is run inside the runtime image (AXIS_CONVERT_IMAGE, default
#      localhost/axis:latest, which only needs to have ffmpeg, ffprobe and
#      the libraries: it may be older than the code), with no network,
#      writing nothing outside its own temporary directory in the container.
#
# Everything the run needs is generated inside the container: one-second
# clips of ffmpeg's test sources.  Its output says which ffmpeg it ran with
# and what the capability check found there.  The hardware families are
# not run (AXIS_TEST_HWENC is not set, and no GPU is passed in); run once
# with them, the image's encoders were found not usable, as on a machine
# without one (`Cannot load libcuda.so.1`, `No VA display found`).
#
# Requires: podman, and the repository's submodule.  Takes minutes (the
# build stage compiles everything).

set -u

cd "$(dirname "$0")/.." || exit 1

runtime=${AXIS_CONTAINER_RUNTIME:-podman}
image=${AXIS_CONVERT_IMAGE:-localhost/axis:latest}
builder=${AXIS_CONVERT_BUILDER:-localhost/axis-build:convert-check}
work=$(mktemp -d "${TMPDIR:-/tmp}/axis-convert-check.XXXXXX") || exit 1

cleanup() {
	rm -rf -- "$work"
}
trap cleanup EXIT

if ! command -v "$runtime" >/dev/null 2>&1; then
	echo "check-convert-container: $runtime is not installed" >&2
	exit 1
fi
if ! "$runtime" image exists "$image"; then
	echo "check-convert-container: no image $image (make container builds one)" >&2
	exit 1
fi

echo "  BUILD   $builder (test-convert-real on the image's Debian)"
if ! "$runtime" build -f tests/convert/Containerfile.check -t "$builder" . > "$work/build.log" 2>&1; then
	tail -n 40 "$work/build.log" >&2
	echo "check-convert-container: the build failed" >&2
	exit 1
fi

echo "  COPY    test-convert-real"
if ! "$runtime" run --rm --network=none --user 0:0 -v "$work":/out:Z --entrypoint /bin/cp "$builder" \
	/src/build/gcc-release-hardened/tests/test-convert-real /out/test-convert-real; then
	echo "check-convert-container: the test program was not built" >&2
	exit 1
fi

echo "  RUN     test-convert-real in $image"
"$runtime" run --rm --network=none --user 0:0 -e TMPDIR=/tmp -v "$work":/check:ro,Z \
	--entrypoint /check/test-convert-real "$image" --verbose
status=$?
if [ $status -ne 0 ]; then
	echo "check-convert-container: FAILED (status $status)" >&2
	exit 1
fi
echo "The real-encode tests passed on the image's ffmpeg."
