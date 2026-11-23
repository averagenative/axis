#!/bin/sh
# check-release.sh - does a release build have the protections it should
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/check-release.sh BUILD_DIR
#
# BUILD_DIR is build/<cc>-release-hardened, after `make release`.  Reads
# the ELF headers with readelf; runs nothing but the stripped daemon's
# --version.  Exits 0 when every check passes.

set -u

dir=${1:-}
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
	echo "usage: $0 BUILD_DIR" >&2
	exit 2
fi
if ! command -v readelf >/dev/null 2>&1; then
	echo "check-release: readelf is not installed" >&2
	exit 1
fi

full=$dir/axisd
stripped=$dir/axisd-stripped
debug=$dir/axisd.debug
failed=0

pass() {
	echo "  ok      $1"
}

fail() {
	echo "  FAIL    $1" >&2
	failed=1
}

# check DESCRIPTION COMMAND [ARG...]: passes when the command succeeds.
check() {
	description=$1
	shift
	if "$@" >/dev/null 2>&1; then
		pass "$description"
	else
		fail "$description"
	fi
}

has_header() {
	readelf -h "$1" | grep -Eq 'Type:[[:space:]]+DYN'
}

has_relro() {
	readelf -lW "$1" | grep -q 'GNU_RELRO'
}

binds_now() {
	readelf -dW "$1" | grep -Eq 'BIND_NOW|FLAGS_1.*NOW'
}

stack_not_executable() {
	readelf -lW "$1" | grep 'GNU_STACK' | grep -Eq 'RW[[:space:]]+0x'
}

uses_stack_protector() {
	readelf -sW "$1" | grep -q '__stack_chk_fail'
}

has_section() {
	readelf -SW "$1" | grep -q " $2 "
}

has_no_section() {
	! has_section "$1" "$2"
}

debuglink_names_debug_file() {
	readelf -p .gnu_debuglink "$1" | grep -q 'axisd.debug'
}

for f in "$full" "$stripped" "$debug"; do
	if [ ! -f "$f" ]; then
		echo "check-release: $f is missing: run 'make release' first" >&2
		exit 1
	fi
done

for binary in "$full" "$stripped"; do
	name=${binary##*/}
	check "$name is a position-independent executable" has_header "$binary"
	check "$name has full RELRO (RELRO segment and immediate binding)" has_relro "$binary"
	check "$name binds symbols at start-up" binds_now "$binary"
	check "$name has a non-executable stack" stack_not_executable "$binary"
	check "$name is built with stack protectors" uses_stack_protector "$binary"
done

check "axisd.debug holds the debug information" has_section "$debug" .debug_info
check "axisd-stripped holds none" has_no_section "$stripped" .debug_info
check "axisd-stripped names axisd.debug as its symbols" debuglink_names_debug_file "$stripped"
check "axisd-stripped runs and reports its version" "$stripped" --version

# A pre-release has a suffix after the three numbers (config.mk's
# VERSION_SUFFIX): "axisd 0.1.0-rc1 (1a2b3c4)".
if "$stripped" --version | grep -Eq '^axisd [0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)? \([0-9a-z]+\)$'; then
	pass "the version line carries a version and a commit"
else
	fail "the version line carries a version and a commit"
fi

if [ "$failed" = 0 ]; then
	echo "Release build checks passed."
fi
exit "$failed"
