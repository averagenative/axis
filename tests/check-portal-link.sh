#!/bin/sh
# check-portal-link.sh - axis-portal links nothing of the database or of the
# daemon's own code
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/check-portal-link.sh build/<config>/axis-portal
#
# axis-portal is linked with its own files (clients/portal/), the few files
# of src/ it shares with the daemon by name (PORTAL_SHARED_SOURCES in the
# Makefile), libaxis-core.a and htmx-glib, and without libpq, so code that
# reached for the database would not link at all.  This checks the result as
# well:
#
#   - no libpq among its dynamic dependencies;
#   - no symbol of libpq (PQ*), defined or wanted;
#   - no symbol of Axis's database, job queue, storage, library, probe,
#     conversion, metadata, indexer, download, import, movies, TV, requests,
#     settings or accounts code, of the engine's web interface (but the
#     request limits and the security headers, which it shares), of the
#     worker API or of the daemon's side of the portal API (its reader of
#     bodies and its writer of answers: the portal has a writer of bodies and
#     readers of answers of its own).  The text helpers of
#     library/axis-library-name.c (UTF-8 and JSON strings, no file) come
#     with the log writer and are let through by name;
#   - nothing that hashes or checks a password: the portal passes a password
#     on and never judges one.

set -u

bin=${1:?usage: check-portal-link.sh AXIS-PORTAL}
failed=0

fail() {
	echo "  FAIL    $bin: $1" >&2
	failed=1
}

if [ ! -x "$bin" ]; then
	echo "check-portal-link: $bin is not an executable" >&2
	exit 1
fi

needed=$(readelf -d "$bin" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
if [ -z "$needed" ]; then
	fail "its dynamic dependencies could not be read"
fi
for lib in $needed; do
	case $lib in
	libpq.so*) fail "it needs $lib" ;;
	esac
done

symbols=$(nm "$bin" 2>/dev/null)
if [ -z "$symbols" ]; then
	fail "its symbols could not be read"
fi
# Global (defined) and undefined symbols only: a file's own static helpers
# are nobody's API.
names=$(printf '%s\n' "$symbols" | awk '$(NF-1) ~ /^[TDBRUVWi]$/ { print $NF }')
bad=$(printf '%s\n' "$names" | grep -E \
	'^(PQ[a-zA-Z]|axis_db_|axis_job_|axis_jobs_|axis_volume_|axis_storage_|axis_library_|axis_probe_|axis_convert_|axis_estimat|axis_profile_|axis_verify_|axis_meta_|axis_discover_|axis_indexer_|axis_download_|axis_import_|axis_identify_|axis_movies_|axis_tv_|axis_requests_|axis_request_|axis_settings_|axis_config_|axis_module_|axis_outbound_|axis_youtube_|axis_arr_|axis_compat_|axis_session_store_|axis_user_|axis_password_|crypto_pwhash|axis_worker_|axis_portal_api_|axis_portal_tokens_|axis_portal_writer_|axis_portal_write_|axis_portal_op_|axis_portal_error_|axis_portal_request_parse$|axis_portal_request_free$|axis_web_)' \
	| grep -Ev '^(axis_web_limits_|axis_web_add_security_headers$|axis_web_replace_security_headers$|axis_web_security_header_names$|axis_library_(name|text|utf8|json)_)' \
	| sort -u)
if [ -n "$bad" ]; then
	fail "it has symbols it must not have: $(echo $bad | tr '\n' ' ')"
fi

# What it must have: the checks above mean something only for the real
# program.
for want in axis_portal_app_new axis_portal_client_call axis_portal_wire_read_title; do
	if ! printf '%s\n' "$names" | grep -qx "$want"; then
		fail "it lacks $want: is this axis-portal?"
	fi
done

if [ "$failed" = 0 ]; then
	echo "  LINK    $bin: no database, no daemon code, no password hashing ($(echo $needed | wc -w) libraries)"
fi
exit "$failed"
