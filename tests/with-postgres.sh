#!/bin/sh
# with-postgres.sh - run a command against a throwaway PostgreSQL
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/with-postgres.sh COMMAND [ARG...]
#
# Starts a PostgreSQL container (podman, else docker) published on
# 127.0.0.1 at a port the runtime picks, waits until it accepts
# connections, runs COMMAND with AXIS_TEST_DATABASE_URL pointing at it,
# and removes the container when COMMAND ends, fails or is interrupted.
# Exits with COMMAND's status.
#
#   AXIS_SKIP_DB_TESTS=1     start nothing; the database tests skip
#   AXIS_TEST_DATABASE_URL   already set: use that server, start nothing
#   AXIS_TEST_PG_IMAGE       the image (default postgres:17-alpine)
#
# The password is random per run.  The server runs with fsync off: its
# data is thrown away anyway.

set -u

image=${AXIS_TEST_PG_IMAGE:-docker.io/library/postgres:17-alpine}

if [ $# -eq 0 ]; then
	echo "usage: $0 COMMAND [ARG...]" >&2
	exit 2
fi

if [ "${AXIS_SKIP_DB_TESTS:-}" = 1 ]; then
	echo "with-postgres: AXIS_SKIP_DB_TESTS=1, the database tests will skip" >&2
	exec "$@"
fi

if [ -n "${AXIS_TEST_DATABASE_URL:-}" ]; then
	exec "$@"
fi

if command -v podman >/dev/null 2>&1; then
	runtime=podman
elif command -v docker >/dev/null 2>&1; then
	runtime=docker
else
	cat >&2 <<EOF
with-postgres: the database tests need PostgreSQL, and neither podman nor
docker is installed to start a throwaway one.  Either:
  - install podman (Fedora: dnf install podman) or docker, or
  - point the tests at a server you control (the tests create and drop
    their own databases on it):
      AXIS_TEST_DATABASE_URL=postgresql://user:password@host/postgres make check
  - or skip the database tests explicitly:
      AXIS_SKIP_DB_TESTS=1 make check
EOF
	exit 1
fi

name=axis-test-pg-$$-$(od -An -N4 -tx4 /dev/urandom | tr -d ' ')
password=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
started=0

cleanup() {
	if [ "$started" = 1 ]; then
		$runtime rm -f -v "$name" >/dev/null 2>&1
		# -v: the image declares an anonymous volume for its data, and
		# removing a running --rm container by force can leave it behind.
		started=0
	fi
}
# EXIT covers a normal end and `exit` from the signal traps; a signal
# exits with the conventional 128+N status.
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# A backstop for a SIGKILLed script: podman stops the container itself
# after two hours (docker has no such option).
extra=
if [ "$runtime" = podman ]; then
	extra="--timeout=7200"
fi

started=1
# shellcheck disable=SC2086
if ! $runtime run --detach --rm $extra --name "$name" \
	--label axis-test=1 \
	--publish 127.0.0.1::5432 \
	--env POSTGRES_PASSWORD="$password" \
	"$image" \
	-c fsync=off -c synchronous_commit=off -c full_page_writes=off \
	-c max_connections=200 >/dev/null; then
	started=0
	echo "with-postgres: could not start $image with $runtime" >&2
	exit 1
fi

port=
i=0
while [ $i -lt 600 ]; do
	# The image's entrypoint first runs a server for initialisation that
	# listens on the Unix socket only; TCP answers once the real one is
	# up.  Ask from inside the container, where the host forwarder
	# cannot answer for it.
	if $runtime exec "$name" pg_isready -q -h 127.0.0.1 -U postgres >/dev/null 2>&1; then
		port=$($runtime port "$name" 5432/tcp 2>/dev/null | sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' | head -n 1)
		[ -n "$port" ] && break
	fi
	if [ "$($runtime inspect -f '{{.State.Running}}' "$name" 2>/dev/null)" != true ]; then
		echo "with-postgres: the PostgreSQL container stopped while starting:" >&2
		$runtime logs "$name" >&2 2>&1
		exit 1
	fi
	sleep 0.1
	i=$((i + 1))
done

if [ -z "$port" ]; then
	echo "with-postgres: PostgreSQL did not accept connections within 60 s" >&2
	$runtime logs "$name" >&2 2>&1
	exit 1
fi

AXIS_TEST_DATABASE_URL="postgresql://postgres:$password@127.0.0.1:$port/postgres"
export AXIS_TEST_DATABASE_URL

"$@"
status=$?
exit $status
