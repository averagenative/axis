#!/bin/sh
# check-worker-link.sh - axis-worker links nothing of the database, the job
# queue or the web
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/check-worker-link.sh build/<config>/axis-worker
#
# axis-worker is linked with libaxis-core.a alone (see CORE_SOURCES in the
# Makefile) and without libpq, so code that reached for the database would
# not link at all.  This checks the result as well:
#
#   - no libpq, libsodium or json-glib among its dynamic dependencies;
#   - no symbol of libpq (PQ*), of Axis's database, job queue, storage
#     guard, web or worker-API code, or of htmx-glib, defined or wanted;
#   - none of the functions that write a volume marker.

set -u

bin=${1:?usage: check-worker-link.sh AXIS-WORKER}
failed=0

fail() {
	echo "  FAIL    $bin: $1" >&2
	failed=1
}

if [ ! -x "$bin" ]; then
	echo "check-worker-link: $bin is not an executable" >&2
	exit 1
fi

needed=$(readelf -d "$bin" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
if [ -z "$needed" ]; then
	fail "its dynamic dependencies could not be read"
fi
for lib in $needed; do
	case $lib in
	libpq.so*|libsodium.so*|libjson-glib*) fail "it needs $lib" ;;
	esac
done

symbols=$(nm "$bin" 2>/dev/null)
if [ -z "$symbols" ]; then
	fail "its symbols could not be read"
fi
# Global (defined) and undefined symbols only: a file's own static helpers
# are nobody's API.
bad=$(printf '%s\n' "$symbols" | awk '$(NF-1) ~ /^[TDBRUVWi]$/ { print $NF }' | grep -E \
	'^(PQ[a-zA-Z]|axis_db_|axis_job_|axis_volume_guard_|axis_storage_(new|load|list|add|remove)|axis_web_|axis_worker_api_|axis_worker_tokens_|axis_worker_store_|axis_estimator_|axis_volume_marker_write|axis_volume_marker_new|htmx_|json_(node|object|array|parser|builder|generator|reader|path|gobject|boxed|serializ|from|to)_|crypto_|sodium_)' \
	| sort -u)
if [ -n "$bad" ]; then
	fail "it has symbols it must not have: $(echo $bad | tr '\n' ' ')"
fi

if [ "$failed" = 0 ]; then
	echo "  LINK    $bin: no database, job queue, web or marker-writing code ($(echo $needed | wc -w) libraries)"
fi
exit "$failed"
