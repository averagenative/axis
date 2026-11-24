#!/bin/bash
# check-container.sh - build the image and start the compose example's services
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/check-container.sh        (or: make check-container)
#
# Builds the image (make container), then starts PostgreSQL and Axis with the
# settings of deploy/compose.example.yaml and checks, from outside and from
# inside the containers:
#
#   - the image has axisd, axis-worker, axis-portal, ffmpeg and ffprobe, and no compiler
#     and no -dev package; it is configured to run as a non-root user
#   - TLS works in the image: it has GIO's TLS backend and a system trust
#     store, and the image's axis-worker completes a TLS handshake (with
#     certificate verification) with a throwaway TLS listener on 127.0.0.1
#     whose certificate it is given the CA of, and refuses the same
#     listener without that CA
#   - both services become healthy, in the order the example asks for
#   - the daemon runs as a non-root user, with no capabilities, no new
#     privileges and a read-only root filesystem
#   - a write to a media mount fails; a write to the root filesystem fails;
#     the data directory is the example's named volume, the daemon made its
#     artwork directory there, it is the daemon's own (mode 0700), and a
#     write to it works
#   - the web interface answers and asks for first-run setup
#   - the database password is in no log
#   - stopped, the daemon exits 0 well inside its grace period
#   - after both containers are removed and started again on the same
#     volumes, the database contents and the files in the data directory are
#     still there
#   - the worker service, started beside Axis in its network namespace with
#     a token made here (only its hash in the database) and a disk with a
#     volume marker, registers, proves the disk, becomes healthy, runs
#     locked down like Axis with software encoders only, cannot write the
#     disk, never shows its token, and stops with exit status 0
#   - with another user id, the daemon refuses the data volume (still the
#     first id's), naming it; once the volume is given to the new id, as
#     deploy/README.md says, the daemon starts and is healthy
#   - started before its database, as an arbitrary user id with group 0 on a
#     fresh data volume, the daemon is alive (/livez) and not ready
#     (/readyz), serves no page, and keeps trying; once a database is
#     started beside it, it becomes ready by itself, creates the first
#     administrator from a secret file (no setup token, the password in no
#     log and in no container configuration), that password logs in, and it
#     can write its data volume without anything having been chowned
#   - the request portal (axis-portal, the same image with another
#     entrypoint) beside that daemon, which is given a portal listener: as a
#     user id the image does not know, read-only with no tmpfs and no volume,
#     no capability, no new privileges, its token (made by the administrator
#     on the page Request portal) as a secret file and nothing of the
#     daemon's in its environment or its mounts, it starts and is healthy; a
#     requester (made by the administrator) logs in and asks for a movie
#     through it, from a client container that plays the reverse proxy, and
#     the request waits in the daemon's database; from an address that is
#     not the listed proxy the same headers get no login; the token and the
#     password are in no log; it stops with exit status 0
#
# HOW THE EXAMPLE IS STARTED.  No compose front end is needed or used.  The
# script reads the example with yq and turns what it says into `podman run`
# options: user, read_only, cap_drop, security_opt, tmpfs, pids_limit,
# environment, secrets (as podman secrets, mounted at /run/secrets/NAME),
# volumes (named volume as is; each bind mount's host side replaced by a
# directory made here, keeping the container path and the mode), networks
# (internal or not; the service name as alias), the health check, the stop
# grace period, and the order (PostgreSQL healthy before Axis starts).  The
# published port is a random one on 127.0.0.1.  What the script does not
# exercise is Compose itself: a mistake in the file that yq can read but a
# compose implementation rejects would not be found here.
#
# On an SELinux host the media directories get --security-opt label=disable on
# the axis container, so that the bind mounts can be read at all; that is the
# one option the example does not have.
#
# Everything the script creates has the name prefix axis-check-PID-RANDOM
# (AXIS_CHECK_PREFIX replaces axis-check) and is removed by name when it
# ends, however it ends: containers, networks, the volumes, the secrets, and
# a temporary directory.  Nothing else is touched.  The image is kept (that
# is what `make container` is for).
#
# Requires: podman (secrets are not available to plain `docker run`), yq (the
# Go one, v4), curl, openssl (for the TLS listener and its certificates), and
# the repository's submodule.

set -u

cd "$(dirname "$0")/.." || exit 1

runtime=${AXIS_CONTAINER_RUNTIME:-podman}
image=${AXIS_CONTAINER_IMAGE:-localhost/axis:dev}
make_cmd=${MAKE:-make}
compose=deploy/examples/compose/compose.yaml

prefix=${AXIS_CHECK_PREFIX:-axis-check}-$$-$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')
work=
tls_pid=
failed=0

say() {
	printf '  %-8s %s\n' "$1" "$2"
}

pass() {
	say ok "$1"
}

fail() {
	say FAIL "$1" >&2
	failed=1
}

# check DESCRIPTION COMMAND [ARG...]: passes when the command succeeds.
check() {
	local description=$1
	shift
	if "$@" >/dev/null 2>&1; then
		pass "$description"
	else
		fail "$description"
	fi
}

names_containers() {
	echo "$prefix-tls-trusted" "$prefix-tls-untrusted" "$prefix-worker" "$prefix-axis" "$prefix-axis-uid" \
		"$prefix-axis-uid-refused" "$prefix-chown" "$prefix-postgres" "$prefix-axis-any" "$prefix-postgres2" \
		"$prefix-portal" "$prefix-portal-client"
}

dump_logs() {
	local c
	for c in $(names_containers); do
		if "$runtime" container exists "$c" 2>/dev/null; then
			echo "---- $c (last 25 lines) ----" >&2
			"$runtime" logs --tail 25 "$c" >&2 2>&1
		fi
	done
}

cleanup() {
	local c
	trap - EXIT
	for c in $(names_containers); do
		"$runtime" rm -f -t 0 "$c" >/dev/null 2>&1
	done
	"$runtime" network rm -f "$prefix-private" >/dev/null 2>&1
	"$runtime" network rm -f "$prefix-private2" >/dev/null 2>&1
	"$runtime" volume rm -f "$prefix-axis-db" >/dev/null 2>&1
	"$runtime" volume rm -f "$prefix-axis-data" >/dev/null 2>&1
	"$runtime" volume rm -f "$prefix-axis-db2" >/dev/null 2>&1
	"$runtime" volume rm -f "$prefix-axis-data2" >/dev/null 2>&1
	for c in axis_db_password axis_database_url axis_worker_token tls_token axis_admin_password axis_portal_token; do
		"$runtime" secret rm "$prefix-$c" >/dev/null 2>&1
	done
	if [ -n "$tls_pid" ]; then
		kill "$tls_pid" >/dev/null 2>&1
		wait "$tls_pid" 2>/dev/null
	fi
	if [ -n "$work" ] && [ -d "$work" ]; then
		rm -rf -- "$work"
	fi
	say cleanup "removed everything named $prefix-*"
}

# A failure that makes the rest meaningless: show the logs, clean up, stop.
die() {
	say FAIL "$1" >&2
	dump_logs
	exit 1
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# ---------------------------------------------------------------- preconditions

case ${runtime##*/} in
podman) ;;
*) echo "check-container: this needs podman (podman secrets); AXIS_CONTAINER_RUNTIME is '$runtime'" >&2
   exit 1 ;;
esac
for tool in "$runtime" yq curl openssl; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "check-container: $tool is not installed" >&2
		exit 1
	fi
done
if ! yq --version 2>&1 | grep -q mikefarah; then
	echo "check-container: yq must be mikefarah/yq v4 (the Go one)" >&2
	exit 1
fi
if [ ! -f "$compose" ]; then
	echo "check-container: $compose is missing" >&2
	exit 1
fi

# yq QUERY: the query's result, raw; "null" when there is none.
y() {
	yq -r "$1" "$compose"
}

# Expands the ${NAME:-default} forms of the example to their defaults.
defaults() {
	sed -E 's/\$\{[A-Za-z_]+:-([^}]*)\}/\1/g'
}

# -------------------------------------------------------------- the example file

echo "The example file"

if yq '.' "$compose" >/dev/null 2>&1; then
	pass "$compose is valid YAML"
else
	die "$compose is not valid YAML"
fi

for svc in axis postgres worker; do
	[ "$(y ".services.$svc | type")" = '!!map' ] || die "service $svc is missing from $compose"

	[ "$(y ".services.$svc.read_only")" = true ] && pass "$svc: read_only" || fail "$svc: read_only"
	y "(.services.$svc.cap_drop // [])[]" | grep -qx ALL \
		&& pass "$svc: cap_drop ALL" || fail "$svc: cap_drop ALL"
	y "(.services.$svc.security_opt // [])[]" | grep -qx 'no-new-privileges:true' \
		&& pass "$svc: no-new-privileges" || fail "$svc: no-new-privileges"
	user=$(y ".services.$svc.user" | defaults)
	case $user in
	''|null|0|0:*|root|root:*) fail "$svc: runs as a non-root user (user is '$user')" ;;
	*) pass "$svc: runs as a non-root user ($user)" ;;
	esac
	[ "$(y ".services.$svc.healthcheck.test[0]")" = CMD ] \
		&& [ "$(y ".services.$svc.healthcheck.test | length")" -gt 1 ] 2>/dev/null \
		&& pass "$svc: has a health check" || fail "$svc: has a health check (a CMD one)"
done

[ "$(y '.services.postgres.ports')" = null ] \
	&& pass "postgres: no port is published" || fail "postgres: no port is published"
[ "$(y '.services.postgres.expose')" = null ] \
	&& pass "postgres: no port is exposed" || fail "postgres: no port is exposed"
[ "$(y '.services.postgres.networks | length')" = 1 ] \
	&& [ "$(y '.services.postgres.networks[0]')" = private ] \
	&& [ "$(y '.services.axis.networks | length')" = 1 ] \
	&& [ "$(y '.services.axis.networks[0]')" = private ] \
	&& [ "$(y '.networks.private.external')" = null ] \
	&& pass "postgres: only on the project's private network" || fail "postgres: only on the project's private network"
[ "$(y '.services.axis.depends_on.postgres.condition')" = service_healthy ] \
	&& pass "axis: waits for a healthy postgres" || fail "axis: waits for a healthy postgres"
[ "$(y '.volumes | has("axis-db")')" = true ] \
	&& y '.services.postgres.volumes[]' | grep -q '^axis-db:' \
	&& pass "postgres: data in the named volume axis-db" || fail "postgres: data in the named volume axis-db"
[ "$(y '.volumes | has("axis-data")')" = true ] \
	&& y '.services.axis.volumes[]' | grep -qx 'axis-data:/var/lib/axis' \
	&& pass "axis: its data directory is the named volume axis-data" \
	|| fail "axis: its data directory is the named volume axis-data"
y '(.services.postgres.environment // {}) | keys | .[]' | grep -qx POSTGRES_PASSWORD_FILE \
	&& ! y '(.services.postgres.environment // {}) | keys | .[]' | grep -qx POSTGRES_PASSWORD \
	&& pass "postgres: the password comes from a secret" || fail "postgres: the password comes from a secret"
y '(.services.axis.environment // {}) | keys | .[]' | grep -qx AXIS_DATABASE_URL_FILE \
	&& ! y '(.services.axis.environment // {}) | keys | .[]' | grep -qx AXIS_DATABASE_URL \
	&& pass "axis: the database URL comes from a secret" || fail "axis: the database URL comes from a secret"

for svc in axis worker; do
	binds=0
	while IFS= read -r volume; do
		case $volume in
		/*)
			binds=$((binds + 1))
			case $volume in
			*:ro) pass "$svc: media mount is read-only: ${volume#*:}" ;;
			*) fail "$svc: media mount is read-only: $volume" ;;
			esac
			;;
		esac
	done < <(y "(.services.$svc.volumes // [])[]")
	[ "$binds" -gt 0 ] && pass "$svc: $binds media mounts" || fail "$svc: at least one media mount"
done

# The worker: axis-worker from the same image, in axis's network namespace,
# its token from a secret file, software encoders only, started on request.
[ "$(y '.services.worker.entrypoint[0]')" = /usr/bin/axis-worker ] \
	&& pass "worker: runs axis-worker" || fail "worker: runs axis-worker"
[ "$(y '.services.worker.healthcheck.test[1]')" = /usr/bin/axis-worker ] \
	&& [ "$(y '.services.worker.healthcheck.test[2]')" = --healthcheck ] \
	&& pass "worker: axis-worker --healthcheck is its health check" \
	|| fail "worker: axis-worker --healthcheck is its health check"
[ "$(y '.services.worker.network_mode')" = "service:axis" ] && [ "$(y '.services.worker.networks')" = null ] \
	&& [ "$(y '.services.worker.ports')" = null ] \
	&& pass "worker: in axis's network namespace, nothing published" \
	|| fail "worker: in axis's network namespace, nothing published"
case $(y '.services.worker.environment.AXIS_WORKER_URL') in
http://127.0.0.1:*|http://localhost:*|https://*) pass "worker: talks to axisd on loopback (or https)" ;;
*) fail "worker: talks to axisd on loopback (or https)" ;;
esac
y '(.services.worker.environment // {}) | keys | .[]' | grep -qx AXIS_WORKER_TOKEN_FILE \
	&& ! y '(.services.worker.environment // {}) | keys | .[]' | grep -qx AXIS_WORKER_TOKEN \
	&& y '(.services.worker.secrets // [])[]' | grep -qx axis_worker_token \
	&& pass "worker: the token comes from a secret" || fail "worker: the token comes from a secret"
[ "$(y '.services.worker.environment.AXIS_WORKER_ENCODER_FAMILIES')" = software ] \
	&& [ "$(y '.services.worker.devices')" = null ] && [ "$(y '.services.worker.deploy')" = null ] \
	&& pass "worker: software encoders only, no device" || fail "worker: software encoders only, no device"
y '(.services.worker.profiles // [])[]' | grep -qx worker \
	&& pass "worker: starts only in the worker profile" || fail "worker: starts only in the worker profile"
[ "$(y '.services.worker.depends_on.axis.condition')" = service_healthy ] \
	&& pass "worker: waits for a healthy axis" || fail "worker: waits for a healthy axis"

if grep -Eq '/home/|/Users/|/mnt/|/srv/' "$compose"; then
	fail "$compose names no path on a real machine"
else
	pass "$compose names no path on a real machine"
fi

# ------------------------------------------------------------------- the image

echo "The image"

"$make_cmd" --no-print-directory container CONTAINER_IMAGE="$image" >/dev/null 2>&1 \
	|| { "$make_cmd" --no-print-directory container CONTAINER_IMAGE="$image" 2>&1 | tail -n 40 >&2
	     die "make container failed"; }
pass "make container built $image"
echo "  info     image size: $("$runtime" image inspect --format '{{.Size}}' "$image") bytes"

image_run() {
	"$runtime" run --rm --entrypoint "$1" "$image" "${@:2}"
}

[ "$("$runtime" image inspect --format '{{.Config.User}}' "$image")" = 1000:1000 ] \
	&& pass "the image's default user is 1000:1000" || fail "the image's default user is 1000:1000"
check "axisd, axis-worker, axis-portal, ffmpeg and ffprobe run" sh -c \
	"'$runtime' run --rm --entrypoint /usr/bin/axisd '$image' --version &&
	 '$runtime' run --rm --entrypoint /usr/bin/axis-worker '$image' --version &&
	 '$runtime' run --rm --entrypoint /usr/bin/axis-portal '$image' --version &&
	 '$runtime' run --rm --entrypoint /usr/bin/ffmpeg '$image' -version &&
	 '$runtime' run --rm --entrypoint /usr/bin/ffprobe '$image' -version"

packages=$(image_run /usr/bin/dpkg-query -W -f '${Package}\n' 2>/dev/null)
if [ -z "$packages" ]; then
	fail "the image's package list could be read"
else
	pass "the image's package list could be read ($(echo "$packages" | wc -l) packages)"
	# gcc-NN-base is the licence and documentation package every Debian
	# image has (libgcc-s1 depends on it); it holds no compiler.
	offenders=$(echo "$packages" | grep -v -E '^gcc-[0-9]+-base$' \
		| grep -E -- '-dev$|-dev-bin$|^gcc|^g\+\+|^cpp|^clang|^make$|^build-essential$|^libc-dev-bin$|^libc6-dev$' || true)
	if [ -z "$offenders" ]; then
		pass "no compiler and no -dev package is installed"
	else
		fail "no compiler and no -dev package is installed (found: $(echo "$offenders" | tr '\n' ' '))"
	fi
fi
compilers=$(image_run /bin/sh -c 'for c in cc gcc g++ c++ cpp clang tcc make; do command -v $c; done' 2>/dev/null || true)
[ -z "$compilers" ] && pass "no cc, gcc, c++, cpp, clang, tcc or make on the path" \
	|| fail "no compiler on the path (found: $compilers)"

echo "  info     ffmpeg encoders for hardware:"
image_run /usr/bin/ffmpeg -hide_banner -encoders 2>/dev/null \
	| grep -E 'nvenc|vaapi|qsv|amf|v4l2m2m|videotoolbox|vulkan|cuvid' | sed 's/^/             /'
echo "  info     ffmpeg hwaccels:"
image_run /usr/bin/ffmpeg -hide_banner -hwaccels 2>/dev/null | tail -n +2 | sed 's/^/             /'

# ------------------------------------------------------------------- resources

work=$(mktemp -d "${TMPDIR:-/tmp}/axis-check.XXXXXX") || die "cannot make a temporary directory"
chmod 0755 "$work"
umask 077
mkdir "$work/secrets"

password=$(od -An -N24 -tx1 /dev/urandom | tr -d ' \n')
printf '%s' "$password" > "$work/secrets/axis_db_password"
printf 'postgresql://axis:%s@postgres:5432/axis\n' "$password" > "$work/secrets/axis_database_url"
umask 022

"$runtime" secret create "$prefix-axis_db_password" "$work/secrets/axis_db_password" >/dev/null \
	|| die "cannot create a secret"
"$runtime" secret create "$prefix-axis_database_url" "$work/secrets/axis_database_url" >/dev/null \
	|| die "cannot create a secret"

# A worker's token, as the Workers page makes one (43 characters of
# base64url); the database gets only its SHA-256, as the page stores it.
worker_token=$(head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=\n' | cut -c1-43)
worker_hash=$(printf '%s' "$worker_token" | sha256sum | cut -d' ' -f1)
worker_volume=$(cat /proc/sys/kernel/random/uuid)
umask 077
printf '%s' "$worker_token" > "$work/secrets/axis_worker_token"
umask 022
"$runtime" secret create "$prefix-axis_worker_token" "$work/secrets/axis_worker_token" >/dev/null \
	|| die "cannot create a secret"
if [ "$(y '.networks.private.internal')" = true ]; then
	"$runtime" network create --internal "$prefix-private" >/dev/null || die "cannot create the network"
else
	"$runtime" network create "$prefix-private" >/dev/null || die "cannot create the network"
fi
"$runtime" volume create "$prefix-axis-db" >/dev/null || die "cannot create the volume"
"$runtime" volume create "$prefix-axis-data" >/dev/null || die "cannot create the volume"

# ------------------------------------------------------------------ TLS

echo "TLS in the image"

check "the image has GIO's TLS backend (glib-networking)" \
	image_run /bin/sh -c 'ls /usr/lib/*/gio/modules/libgiognutls.so'
check "the image has a system trust store" \
	image_run /bin/sh -c 'test -s /etc/ssl/certs/ca-certificates.crt'

# A CA made here, and a certificate for 127.0.0.1 signed by it, for a TLS
# listener (openssl s_server) on 127.0.0.1 that prints what it is sent.
tls=$work/tls
mkdir -m 0755 "$tls" || die "cannot make $tls"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 -subj '/CN=axis-check CA' \
	-addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign' \
	-keyout "$tls/ca.key" -out "$tls/ca.pem" >/dev/null 2>&1 \
	&& openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj '/CN=127.0.0.1' \
		-keyout "$tls/server.key" -out "$tls/server.csr" >/dev/null 2>&1 \
	&& printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' \
		> "$tls/server.ext" \
	&& openssl x509 -req -in "$tls/server.csr" -CA "$tls/ca.pem" -CAkey "$tls/ca.key" -CAcreateserial -days 1 \
		-extfile "$tls/server.ext" -out "$tls/server.pem" >/dev/null 2>&1 \
	&& chmod 0644 "$tls/ca.pem" \
	|| die "cannot make the TLS listener's certificates"
# Its standard input is a FIFO opened for reading and writing: never an end
# of file, so it keeps its connections open.
mkfifo "$tls/stdin" || die "cannot make a FIFO"
tls_port=
for attempt in 1 2 3 4 5; do
	p=$((20000 + RANDOM % 20000))
	openssl s_server -accept "127.0.0.1:$p" -cert "$tls/server.pem" -key "$tls/server.key" \
		<> "$tls/stdin" > "$tls/server.log" 2>&1 &
	tls_pid=$!
	sleep 1
	if kill -0 "$tls_pid" 2>/dev/null; then
		tls_port=$p
		break
	fi
	wait "$tls_pid" 2>/dev/null
	tls_pid=
done
[ -n "$tls_port" ] || die "cannot start a TLS listener on 127.0.0.1"

# A well-formed token the listener may see: it is no worker's.
printf '%s' "$(head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=\n' | cut -c1-43)" > "$tls/token"
"$runtime" secret create "$prefix-tls_token" "$tls/token" >/dev/null || die "cannot create a secret"

# tls_worker NAME [CA]: the image's axis-worker, locked down as the example
# runs it, on the host's network (to reach the listener on 127.0.0.1),
# pointed at https://127.0.0.1:PORT, trusting CA too when given.
tls_worker() {
	local extra=()
	if [ -n "${2:-}" ]; then
		extra=(--volume "$2:/run/axis-check-ca.pem:ro" --env AXIS_WORKER_CA_FILE=/run/axis-check-ca.pem)
	fi
	"$runtime" run --detach --name "$1" --label "axis-check=$prefix" --network host \
		--user 1000:1000 --read-only --cap-drop ALL --security-opt no-new-privileges:true \
		--security-opt label=disable --tmpfs /tmp:rw,noexec,nosuid,size=256m,mode=1777 \
		--secret "$prefix-tls_token,target=token,uid=1000,mode=0400" \
		--env "AXIS_WORKER_URL=https://127.0.0.1:$tls_port" --env AXIS_WORKER_TOKEN_FILE=/run/secrets/token \
		--env AXIS_WORKER_ENCODER_FAMILIES=software --env AXIS_WORKER_REQUEST_TIMEOUT=5 \
		"${extra[@]}" --entrypoint /usr/bin/axis-worker "$image" >/dev/null
}

# wait_until SECONDS COMMAND [ARG...]: whether COMMAND succeeds within SECONDS.
wait_until() {
	local n=0 limit=$1
	shift
	while [ "$n" -lt "$limit" ]; do
		"$@" >/dev/null 2>&1 && return 0
		sleep 1
		n=$((n + 1))
	done
	return 1
}

# Trusting the CA: the listener receives the request in the clear, which it
# can only do once the handshake (the worker verifying the certificate and
# the name 127.0.0.1) has completed.
request='POST /api/v1/worker/register HTTP/1.1'
tls_worker "$prefix-tls-trusted" "$tls/ca.pem" || die "cannot start a worker for the TLS check"
if wait_until 180 grep -qF "$request" "$tls/server.log"; then
	pass "the image's axis-worker completes a TLS handshake (certificate verified) with libsoup"
else
	fail "the image's axis-worker completes a TLS handshake (certificate verified) with libsoup"
	"$runtime" logs --tail 15 "$prefix-tls-trusted" >&2 2>&1
fi
case $("$runtime" logs "$prefix-tls-trusted" 2>&1) in
*"certificate was not accepted"*) fail "the worker accepted the certificate of a CA it was given" ;;
*) pass "the worker accepted the certificate of a CA it was given" ;;
esac
"$runtime" rm -f -t 0 "$prefix-tls-trusted" >/dev/null 2>&1
sent=$(grep -cF "$request" "$tls/server.log")

# Without the CA: the certificate is refused, and nothing is sent.
tls_worker "$prefix-tls-untrusted" || die "cannot start a worker for the TLS check"
if wait_until 180 sh -c "'$runtime' logs '$prefix-tls-untrusted' 2>&1 | grep -q \"the daemon's certificate was not accepted\""; then
	pass "without the CA, the worker refuses the certificate"
else
	fail "without the CA, the worker refuses the certificate"
	"$runtime" logs --tail 15 "$prefix-tls-untrusted" >&2 2>&1
fi
[ "$(grep -cF "$request" "$tls/server.log")" = "$sent" ] \
	&& pass "without the CA, the worker sends nothing over the connection" \
	|| fail "without the CA, the worker sends nothing over the connection"
"$runtime" rm -f -t 0 "$prefix-tls-untrusted" >/dev/null 2>&1
kill "$tls_pid" >/dev/null 2>&1
wait "$tls_pid" 2>/dev/null
tls_pid=

# service_args SERVICE USER: ARGS gets the `podman run` options the example
# gives SERVICE, run as USER (uid:gid).  MEDIA gets the container paths of its
# read-only bind mounts.
service_args() {
	local svc=$1 user=$2 uid=${2%%:*}
	local v name src dst mode nets='' key value

	ARGS=(--user "$user")
	MEDIA=()

	[ "$(y ".services.$svc.read_only")" = true ] && ARGS+=(--read-only)
	while IFS= read -r v; do [ -n "$v" ] && ARGS+=(--cap-drop "$v"); done \
		< <(y "(.services.$svc.cap_drop // [])[]")
	while IFS= read -r v; do [ -n "$v" ] && ARGS+=(--security-opt "$v"); done \
		< <(y "(.services.$svc.security_opt // [])[]")
	while IFS= read -r v; do [ -n "$v" ] && ARGS+=(--tmpfs "$v"); done \
		< <(y "(.services.$svc.tmpfs // [])[]")
	v=$(y ".services.$svc.pids_limit")
	[ "$v" != null ] && ARGS+=(--pids-limit "$v")

	while IFS='=' read -r key value; do
		[ -n "$key" ] && ARGS+=(--env "$key=$(printf '%s' "$value" | defaults)")
	done < <(y "(.services.$svc.environment // {}) | to_entries | .[] | .key + \"=\" + (.value | tostring)")

	while IFS= read -r name; do
		[ -n "$name" ] && ARGS+=(--secret "$prefix-$name,target=$name,uid=$uid,mode=0400")
	done < <(y "(.services.$svc.secrets // [])[]")

	while IFS=: read -r src dst mode; do
		[ -z "$src" ] && continue
		case $src in
		/*)
			local dir="$work/media/${dst##*/}"
			mkdir -p "$dir"
			echo "marker" > "$dir/marker.txt"
			ARGS+=(--volume "$dir:$dst${mode:+:$mode}")
			[ "$mode" = ro ] && MEDIA+=("$dst")
			;;
		*) ARGS+=(--volume "$prefix-$src:$dst${mode:+:$mode}") ;;
		esac
	done < <(y "(.services.$svc.volumes // [])[]")

	while IFS= read -r name; do
		[ -n "$name" ] && nets="${nets:+$nets,}$prefix-$name:alias=$svc"
	done < <(y "(.services.$svc.networks // [])[]")
	[ -n "$nets" ] && ARGS+=(--network "$nets")

	v=$(y ".services.$svc.healthcheck.test | del(.[0]) | join(\" \")")
	if [ "$v" != null ] && [ -n "$v" ]; then
		# Faster than the example's own timings; the check is the same.
		ARGS+=(--health-cmd "$v" --health-interval 2s --health-start-period 0s
		       --health-retries "$(y ".services.$svc.healthcheck.retries")"
		       --health-timeout "$(y ".services.$svc.healthcheck.timeout")")
	fi
}

# healthy NAME: whether the container's health check passes right now.  What
# the check printed last is kept in health_output (a health check prints
# nothing that is secret).
healthy() {
	health_output=$("$runtime" healthcheck run "$1" 2>&1)
}

# wait_healthy NAME SECONDS
wait_healthy() {
	local n=0
	health_output=
	while [ "$n" -lt "$2" ]; do
		if [ "$("$runtime" inspect --format '{{.State.Running}}' "$1" 2>/dev/null)" != true ]; then
			echo "$1 is not running" >&2
			return 1
		fi
		healthy "$1" && return 0
		sleep 1
		n=$((n + 1))
	done
	echo "the last health check of $1 said: $health_output" >&2
	return 1
}

start_postgres() {
	service_args postgres "$(y '.services.postgres.user' | defaults)"
	"$runtime" run --detach --name "$prefix-postgres" --label "axis-check=$prefix" \
		"${ARGS[@]}" "$(y '.services.postgres.image')" >/dev/null \
		|| die "cannot start postgres"
	wait_healthy "$prefix-postgres" 90 || die "postgres did not become healthy"
}

# start_axis NAME USER
start_axis() {
	service_args axis "$2"
	"$runtime" run --detach --name "$1" --label "axis-check=$prefix" \
		--publish 127.0.0.1::7800 --security-opt label=disable \
		"${ARGS[@]}" "$image" >/dev/null \
		|| die "cannot start axis"
	wait_healthy "$1" 90 || die "axis did not become healthy"
}

# stop_clean NAME: stops it as the runtime would, and checks the way out.
stop_clean() {
	local t0=$SECONDS code elapsed
	"$runtime" stop -t "$(y '.services.axis.stop_grace_period' | tr -d s)" "$1" >/dev/null 2>&1
	elapsed=$((SECONDS - t0))
	code=$("$runtime" inspect --format '{{.State.ExitCode}}' "$1")
	[ "$code" = 0 ] && pass "$1 stopped with exit status 0" || fail "$1 stopped with exit status 0 (it was $code)"
	[ "$elapsed" -lt 15 ] && pass "$1 stopped in ${elapsed} s, inside the grace period" \
		|| fail "$1 stopped in ${elapsed} s, inside the grace period"
}

# status_field NAME FIELD: a field of /proc/1/status in the container.
status_field() {
	"$runtime" exec "$1" cat /proc/1/status | sed -n "s/^$2:[[:space:]]*//p"
}

# ---------------------------------------------------------------- first start

echo "First start"

start_postgres
pass "postgres is healthy"
start_axis "$prefix-axis" "$(y '.services.axis.user' | defaults)"
pass "axis is healthy (it started after postgres)"
axis=$prefix-axis

uid=$(status_field "$axis" Uid | cut -f1)
[ -n "$uid" ] && [ "$uid" != 0 ] && pass "the daemon runs as uid $uid, not root" \
	|| fail "the daemon runs as uid $uid, not root"
[ "$(status_field "$axis" CapEff)" = 0000000000000000 ] && pass "the daemon has no capabilities" \
	|| fail "the daemon has no capabilities"
[ "$(status_field "$axis" NoNewPrivs)" = 1 ] && pass "the daemon cannot gain privileges" \
	|| fail "the daemon cannot gain privileges"
[ "$("$runtime" inspect --format '{{.HostConfig.ReadonlyRootfs}}' "$axis")" = true ] \
	&& pass "the root filesystem is read-only" || fail "the root filesystem is read-only"
if "$runtime" exec "$axis" touch /usr/axis-check 2>/dev/null; then
	fail "a write to the root filesystem fails"
else
	pass "a write to the root filesystem fails"
fi
check "a write to /tmp (a tmpfs) works" "$runtime" exec "$axis" touch /tmp/axis-check
check "the daemon made its artwork directory in the data volume" "$runtime" exec "$axis" test -d /var/lib/axis/artwork
# A new volume takes the image's directory: the default user's, group 0,
# which that group may write (for an arbitrary user id), nobody else.
[ "$("$runtime" exec "$axis" stat -c '%u %g %a' /var/lib/axis 2>/dev/null)" = "$uid 0 770" ] \
	&& pass "the data directory is the daemon's own and group 0's, mode 0770" \
	|| fail "the data directory is the daemon's own and group 0's, mode 0770 (it is $("$runtime" exec "$axis" stat -c '%u %g %a' /var/lib/axis 2>&1))"
check "a write to the data directory works" "$runtime" exec "$axis" sh -c 'echo kept > /var/lib/axis/axis-check'

for dst in "${MEDIA[@]}"; do
	if ! "$runtime" exec "$axis" ls "$dst/marker.txt" >/dev/null 2>&1; then
		fail "$dst can be read"
		continue
	fi
	pass "$dst can be read"
	out=$("$runtime" exec "$axis" touch "$dst/axis-check" 2>&1)
	case $out in
	*"Read-only file system"*) pass "a write to $dst fails: read-only file system" ;;
	*) fail "a write to $dst fails: read-only file system (got: $out)" ;;
	esac
	if "$runtime" exec "$axis" rm "$dst/marker.txt" >/dev/null 2>&1; then
		fail "removing a file from $dst fails"
	else
		pass "removing a file from $dst fails"
	fi
done
[ "${#MEDIA[@]}" -gt 0 ] || fail "there are media mounts to check"

port=$("$runtime" port "$axis" 7800/tcp | sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' | head -n 1)
[ -n "$port" ] || die "the web interface has no published port"
if page=$(curl -fsSL --max-time 15 "http://127.0.0.1:$port/"); then
	case $page in
	*"Set up"*) pass "the web interface answers, and asks for first-run setup" ;;
	*) fail "the web interface answers, and asks for first-run setup" ;;
	esac
else
	fail "the web interface answers"
fi
health=$(curl -sS --max-time 15 "http://127.0.0.1:$port/healthz")
case $health in
*'"status":"ok"'*) pass "/healthz reports ok" ;;
*) fail "/healthz reports ok" ;;
esac
[ "$(curl -sS --max-time 15 "http://127.0.0.1:$port/livez")" = '{"status":"alive"}' ] \
	&& pass "/livez reports alive" || fail "/livez reports alive"
[ "$(curl -sS --max-time 15 "http://127.0.0.1:$port/readyz")" = '{"status":"ready"}' ] \
	&& pass "/readyz reports ready" || fail "/readyz reports ready"

logs=$("$runtime" logs "$axis" 2>&1)
case $logs in
*"setup token"*) pass "the log carries the one-time setup token line" ;;
*) fail "the log carries the one-time setup token line" ;;
esac
case $logs in
*"$password"*) fail "the database password is not in the daemon's log" ;;
*) pass "the database password is not in the daemon's log" ;;
esac
case $("$runtime" logs "$prefix-postgres" 2>&1) in
*"$password"*) fail "the database password is not in postgres's log" ;;
*) pass "the database password is not in postgres's log" ;;
esac
case $("$runtime" inspect "$axis" "$prefix-postgres" 2>&1) in
*"$password"*) fail "the database password is not in the containers' configuration" ;;
*) pass "the database password is not in the containers' configuration" ;;
esac

# ---------------------------------------------------------------- data, stop

echo "Stop, remove, start again"

"$runtime" exec "$prefix-postgres" psql --username axis --dbname axis --quiet --no-psqlrc \
	-c 'CREATE TABLE axis_check_marker (v text)' \
	-c "INSERT INTO axis_check_marker VALUES ('kept')" >/dev/null 2>&1 \
	|| die "cannot write to the database"

# For the worker, below: a worker (its token's hash only) and a volume whose
# marker is on the first media directory.  Axis reads both when it starts
# again.
"$runtime" exec "$prefix-postgres" psql --username axis --dbname axis --quiet --no-psqlrc \
	-c "INSERT INTO worker (name, token_hash, created_by) VALUES ('container-check', '$worker_hash', 'check')" \
	-c "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at)
	    VALUES ('$worker_volume', 'Check disk', '${MEDIA[0]}', false, 'online', now())" >/dev/null 2>&1 \
	|| die "cannot add the worker to the database"
printf 'axis-volume 1\nuuid %s\ncreated 2026-11-12T10:15:30Z\nnote %s\n' "$worker_volume" \
	'Axis recognises this disk by this file. Do not edit, delete or copy it.' \
	> "$work/media/${MEDIA[0]##*/}/.axis-volume" || die "cannot write the volume marker"
migrations_before=$("$runtime" exec "$prefix-postgres" psql --username axis --dbname axis \
	--tuples-only --no-align --no-psqlrc -c 'SELECT count(*) FROM axis_schema_migration' 2>/dev/null)
[ -n "$migrations_before" ] && [ "$migrations_before" -gt 0 ] \
	&& pass "the daemon migrated the database ($migrations_before migrations)" \
	|| fail "the daemon migrated the database"

stop_clean "$axis"
"$runtime" stop -t 20 "$prefix-postgres" >/dev/null 2>&1
"$runtime" rm -f "$axis" "$prefix-postgres" >/dev/null 2>&1

start_postgres
start_axis "$prefix-axis" "$(y '.services.axis.user' | defaults)"
pass "both containers, new, are healthy on the old volume"

[ "$("$runtime" exec "$prefix-postgres" psql --username axis --dbname axis --tuples-only --no-align \
	--no-psqlrc -c 'SELECT v FROM axis_check_marker' 2>/dev/null)" = kept ] \
	&& pass "the database contents survived" || fail "the database contents survived"
[ "$("$runtime" exec "$prefix-postgres" psql --username axis --dbname axis --tuples-only --no-align \
	--no-psqlrc -c 'SELECT count(*) FROM axis_schema_migration' 2>/dev/null)" = "$migrations_before" ] \
	&& pass "the migrations were not applied twice" || fail "the migrations were not applied twice"
[ "$("$runtime" exec "$prefix-axis" cat /var/lib/axis/axis-check 2>/dev/null)" = kept ] \
	&& pass "a file in the data directory survived" || fail "a file in the data directory survived"
check "the artwork directory survived" "$runtime" exec "$prefix-axis" test -d /var/lib/axis/artwork

# -------------------------------------------------------------------- a worker

echo "A worker beside Axis"

worker=$prefix-worker
axis_media=("${MEDIA[@]}")
service_args worker "$(y '.services.worker.user' | defaults)"
[ "$(y '.services.worker.network_mode')" = "service:axis" ] || die "the worker is not in axis's network namespace"
"$runtime" run --detach --name "$worker" --label "axis-check=$prefix" \
	--network "container:$prefix-axis" --security-opt label=disable \
	--entrypoint "$(y '.services.worker.entrypoint[0]')" \
	"${ARGS[@]}" "$image" >/dev/null \
	|| die "cannot start the worker"
wait_healthy "$worker" 240 || die "the worker did not become healthy"
pass "the worker is healthy"

wlogs=$("$runtime" logs "$worker" 2>&1)
case $wlogs in
*"registered as container-check"*) pass "the worker registered" ;;
*) fail "the worker registered" ;;
esac
case $wlogs in
*"volume $worker_volume at ${MEDIA[0]}: proved and accepted"*) pass "the worker proved ${MEDIA[0]} by its marker" ;;
*) fail "the worker proved ${MEDIA[0]} by its marker" ;;
esac
case $wlogs in
*"encoder_families=software "*) pass "the worker runs software encoders only" ;;
*) fail "the worker runs software encoders only" ;;
esac
[ "$("$runtime" exec "$prefix-postgres" psql --username axis --dbname axis --tuples-only --no-align --no-psqlrc \
	-c "SELECT (w.registered_at IS NOT NULL)::text || ' ' || coalesce(string_agg(v.path, ','), '-')
	    FROM worker w LEFT JOIN worker_volume v ON v.worker_id = w.id
	    WHERE w.name = 'container-check' GROUP BY w.registered_at" 2>/dev/null)" = "true ${MEDIA[0]}" ] \
	&& pass "axisd recorded the worker and its disk" || fail "axisd recorded the worker and its disk"

uid=$(status_field "$worker" Uid | cut -f1)
[ -n "$uid" ] && [ "$uid" != 0 ] && pass "the worker runs as uid $uid, not root" \
	|| fail "the worker runs as uid $uid, not root"
[ "$(status_field "$worker" CapEff)" = 0000000000000000 ] && pass "the worker has no capabilities" \
	|| fail "the worker has no capabilities"
[ "$(status_field "$worker" NoNewPrivs)" = 1 ] && pass "the worker cannot gain privileges" \
	|| fail "the worker cannot gain privileges"
[ "$("$runtime" inspect --format '{{.HostConfig.ReadonlyRootfs}}' "$worker")" = true ] \
	&& pass "the worker's root filesystem is read-only" || fail "the worker's root filesystem is read-only"
for dst in "${MEDIA[@]}"; do
	out=$("$runtime" exec "$worker" touch "$dst/axis-check" 2>&1)
	case $out in
	*"Read-only file system"*) pass "a write to $dst from the worker fails: read-only file system" ;;
	*) fail "a write to $dst from the worker fails: read-only file system (got: $out)" ;;
	esac
done

case $wlogs in
*"$worker_token"*) fail "the worker's token is not in the worker's log" ;;
*) pass "the worker's token is not in the worker's log" ;;
esac
case $("$runtime" logs "$prefix-axis" 2>&1) in
*"$worker_token"*|*"$worker_hash"*) fail "the worker's token is not in the daemon's log" ;;
*) pass "the worker's token is not in the daemon's log" ;;
esac
case $("$runtime" inspect "$worker" "$prefix-axis" 2>&1) in
*"$worker_token"*) fail "the worker's token is not in the containers' configuration" ;;
*) pass "the worker's token is not in the containers' configuration" ;;
esac

t0=$SECONDS
"$runtime" stop -t "$(y '.services.worker.stop_grace_period' | tr -d s)" "$worker" >/dev/null 2>&1
elapsed=$((SECONDS - t0))
code=$("$runtime" inspect --format '{{.State.ExitCode}}' "$worker")
[ "$code" = 0 ] && pass "the worker stopped with exit status 0" \
	|| fail "the worker stopped with exit status 0 (it was $code)"
[ "$elapsed" -lt 15 ] && pass "the worker stopped in ${elapsed} s" || fail "the worker stopped in ${elapsed} s"
"$runtime" rm -f "$worker" >/dev/null 2>&1
MEDIA=("${axis_media[@]}")

# ----------------------------------------------------------------- another uid

echo "Another user id"

stop_clean "$prefix-axis"
"$runtime" rm -f "$prefix-axis" >/dev/null 2>&1
other=4242:4343

# The data volume is still the first id's: refused, naming the directory,
# before the database is touched.
service_args axis "$other"
out=$(timeout 120 "$runtime" run --rm --name "$prefix-axis-uid-refused" --label "axis-check=$prefix" \
	--security-opt label=disable "${ARGS[@]}" "$image" 2>&1)
code=$?
case $code:$out in
0:*) fail "as $other, the daemon refuses a data volume that is not its own (it started)" ;;
*"/var/lib/axis belongs to user 1000"*) pass "as $other, the daemon refuses a data volume that is not its own, naming it" ;;
*) fail "as $other, the daemon refuses a data volume that is not its own, naming it (it said: $(echo "$out" | tail -n 2))" ;;
esac
# Given to the new id once, as deploy/README.md says.
"$runtime" run --rm --name "$prefix-chown" --label "axis-check=$prefix" --user 0 \
	--volume "$prefix-axis-data:/var/lib/axis" --entrypoint /bin/chown "$image" -R "$other" /var/lib/axis \
	>/dev/null 2>&1 || die "cannot give the data volume to $other"
start_axis "$prefix-axis-uid" "$other"
pass "axis is healthy as $other"
[ "$(status_field "$prefix-axis-uid" Uid | cut -f1)" = 4242 ] \
	&& pass "the daemon runs as uid 4242" || fail "the daemon runs as uid 4242"
[ "$(status_field "$prefix-axis-uid" CapEff)" = 0000000000000000 ] \
	&& pass "the daemon has no capabilities as uid 4242" || fail "the daemon has no capabilities as uid 4242"
[ "$("$runtime" exec "$prefix-axis-uid" cat /var/lib/axis/axis-check 2>/dev/null)" = kept ] \
	&& pass "as uid 4242, the data directory is the same one" || fail "as uid 4242, the data directory is the same one"
stop_clean "$prefix-axis-uid"
"$runtime" rm -f "$prefix-axis-uid" >/dev/null 2>&1
"$runtime" stop -t 20 "$prefix-postgres" >/dev/null 2>&1

# ------------------------------------------- any uid, before its database

echo "Any user id with group 0, started before its database"

# A project of its own: a second network, fresh volumes, and the first
# administrator's password as a secret file.
any=$prefix-axis-any
# An id the image's passwd does not know, as a platform would pick (within
# rootless Podman's subordinate range: OpenShift's own are larger), with
# group 0.
any_uid=54321
any_user=$any_uid:0
admin_password=$(od -An -N18 -tx1 /dev/urandom | tr -d ' \n')
umask 077
printf '%s\n' "$admin_password" > "$work/secrets/axis_admin_password"
umask 022
"$runtime" secret create "$prefix-axis_admin_password" "$work/secrets/axis_admin_password" >/dev/null \
	|| die "cannot create a secret"
"$runtime" network create "$prefix-private2" >/dev/null || die "cannot create the network"
"$runtime" volume create "$prefix-axis-db2" >/dev/null || die "cannot create the volume"
"$runtime" volume create "$prefix-axis-data2" >/dev/null || die "cannot create the volume"

# retarget: ARGS with the first project's network and volumes replaced by
# the second's.
retarget() {
	local i
	for i in "${!ARGS[@]}"; do
		ARGS[$i]=${ARGS[$i]//$prefix-private:/$prefix-private2:}
		ARGS[$i]=${ARGS[$i]//$prefix-axis-data:/$prefix-axis-data2:}
		ARGS[$i]=${ARGS[$i]//$prefix-axis-db:/$prefix-axis-db2:}
	done
}

service_args axis "$any_user"
retarget
# The portal of this check (further down) has a fixed address on this
# network, and the daemon is told it: the portal API answers that address
# alone, and nobody logs in to Axis itself from it.
portal_subnet=$("$runtime" network inspect --format '{{(index .Subnets 0).Subnet}}' "$prefix-private2")
portal_ip=${portal_subnet%.*}.251
"$runtime" run --detach --name "$any" --label "axis-check=$prefix" \
	--publish 127.0.0.1::7800 --security-opt label=disable \
	--secret "$prefix-axis_admin_password,target=axis_admin_password,uid=$any_uid,mode=0400" \
	--env AXIS_ADMIN_USERNAME=admin --env AXIS_ADMIN_PASSWORD_FILE=/run/secrets/axis_admin_password \
	--env AXIS_PORTAL_LISTEN_PORT=7801 --env "AXIS_PORTAL_PEERS=$portal_ip" \
	"${ARGS[@]}" "$image" >/dev/null || die "cannot start axis as $any_user"
any_port=$("$runtime" port "$any" 7800/tcp | sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' | head -n 1)
[ -n "$any_port" ] || die "the daemon as $any_user has no published port"

if wait_until 60 sh -c "[ \"\$(curl -sS --max-time 5 http://127.0.0.1:$any_port/livez)\" = '{\"status\":\"alive\"}' ]"; then
	pass "with no database yet, the daemon is alive (/livez)"
else
	die "with no database yet, the daemon is alive (/livez)"
fi
[ "$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$any_port/readyz")" = 503 ] \
	&& pass "with no database yet, it is not ready (/readyz 503)" \
	|| fail "with no database yet, it is not ready (/readyz 503)"
out=$(curl -sS --max-time 5 -w ' %{http_code}' "http://127.0.0.1:$any_port/login")
case $out in
"Axis is starting"*" 503") pass "with no database yet, it serves no page (503, starting)" ;;
*) fail "with no database yet, it serves no page (got: $(echo "$out" | tail -c 80))" ;;
esac
if wait_until 30 sh -c "'$runtime' logs '$any' 2>&1 | grep -q 'trying again in'"; then
	pass "it keeps trying to reach the database, with back-off"
else
	fail "it keeps trying to reach the database, with back-off"
fi
[ "$("$runtime" inspect --format '{{.State.Running}}' "$any")" = true ] \
	&& pass "it is still running without a database" || fail "it is still running without a database"

# Now its database.
service_args postgres "$(y '.services.postgres.user' | defaults)"
retarget
"$runtime" run --detach --name "$prefix-postgres2" --label "axis-check=$prefix" \
	"${ARGS[@]}" "$(y '.services.postgres.image')" >/dev/null || die "cannot start the second postgres"
wait_healthy "$prefix-postgres2" 90 || die "the second postgres did not become healthy"
if wait_until 120 sh -c "[ \"\$(curl -sS --max-time 5 http://127.0.0.1:$any_port/readyz)\" = '{\"status\":\"ready\"}' ]"; then
	pass "once its database is up, it becomes ready by itself (/readyz)"
else
	die "once its database is up, it becomes ready by itself (/readyz)"
fi
wait_healthy "$any" 60 && pass "its health check passes" || fail "its health check passes"

[ "$(status_field "$any" Uid | cut -f1)" = "$any_uid" ] && [ "$(status_field "$any" Gid | cut -f1)" = 0 ] \
	&& pass "it runs as uid $any_uid (not in the image's passwd), gid 0" \
	|| fail "it runs as uid $any_uid (not in the image's passwd), gid 0"
[ "$(status_field "$any" CapEff)" = 0000000000000000 ] \
	&& pass "it has no capabilities" || fail "it has no capabilities"
[ "$("$runtime" exec "$any" stat -c '%u %g %a' /var/lib/axis 2>/dev/null)" = "1000 0 770" ] \
	&& pass "the data volume was not chowned (still 1000:0, mode 0770)" \
	|| fail "the data volume was not chowned (still 1000:0, mode 0770)"
check "it writes its data volume through group 0" "$runtime" exec "$any" sh -c 'echo kept > /var/lib/axis/axis-check-any'
[ "$("$runtime" exec "$any" stat -c '%u' /var/lib/axis/artwork 2>/dev/null)" = "$any_uid" ] \
	&& pass "it made its artwork directory there, as its own user" \
	|| fail "it made its artwork directory there, as its own user"

logs=$("$runtime" logs "$any" 2>&1)
case $logs in
*"created administrator user=admin from the configuration"*) pass "it created the first administrator from the secret file" ;;
*) fail "it created the first administrator from the secret file" ;;
esac
case $logs in
*"setup token"*) fail "it printed no setup token" ;;
*) pass "it printed no setup token" ;;
esac
case $logs in
*"$admin_password"*) fail "the administrator's password is not in the daemon's log" ;;
*) pass "the administrator's password is not in the daemon's log" ;;
esac
case $("$runtime" inspect "$any" 2>&1) in
*"$admin_password"*) fail "the administrator's password is not in the container's configuration" ;;
*) pass "the administrator's password is not in the container's configuration" ;;
esac

# The password logs in: the login form's token and pre-session cookie, then
# the form.
jar=$work/cookies
login_page=$(curl -sS --max-time 15 -c "$jar" "http://127.0.0.1:$any_port/login")
csrf=$(printf '%s' "$login_page" | grep -o '<input[^>]*name="_csrf"[^>]*>' | head -n 1 \
	| sed -n 's/.* value="\([^"]*\)".*/\1/p')
login_status=$(curl -sS --max-time 30 -b "$jar" -c "$jar" -o /dev/null -w '%{http_code}' \
	--data-urlencode "_csrf=$csrf" --data-urlencode "username=admin" --data-urlencode "password=$admin_password" \
	"http://127.0.0.1:$any_port/login")
if [ -n "$csrf" ] && [ "$login_status" = 303 ] && grep -q 'axis_session' "$jar"; then
	pass "the administrator logs in with the password from the secret"
else
	fail "the administrator logs in with the password from the secret (status $login_status)"
fi

# ------------------------------------------------------- the request portal

echo "The request portal, from the same image"

# This daemon was started with AXIS_PORTAL_LISTEN_PORT=7801 (the one setting
# here that the compose example does not have): its portal API is on.
case $("$runtime" logs "$any" 2>&1) in
*"portal API: listening on 0.0.0.0:7801"*) pass "the daemon answers the portal API on a port of its own (7801)" ;;
*) fail "the daemon answers the portal API on a port of its own (7801)" ;;
esac

# admin_form PATH FIELD=VALUE...: a form of the engine's interface posted by
# the administrator who logged in above; prints the status, and leaves the
# answer in $work/admin-form.
admin_form() {
	local path=$1 token field
	local -a data
	shift
	token=$(curl -sS --max-time 15 -b "$jar" -c "$jar" "http://127.0.0.1:$any_port/admin/portal" \
		| grep -o '<input[^>]*name="_csrf"[^>]*>' | head -n 1 | sed -n 's/.* value="\([^"]*\)".*/\1/p')
	data=(--data-urlencode "_csrf=$token")
	for field in "$@"; do
		data+=(--data-urlencode "$field")
	done
	curl -sS --max-time 30 -b "$jar" -c "$jar" -o "$work/admin-form" -w '%{http_code}' "${data[@]}" \
		"http://127.0.0.1:$any_port$path"
}

# The token, as an administrator makes it: shown once, on the page that
# answers its making.
portal_token=
[ "$(admin_form /admin/portal/tokens)" = 200 ] \
	&& portal_token=$(grep -o '<pre class="worker-token"><code>[^<]*' "$work/admin-form" | head -n 1 | sed 's/.*<code>//')
rm -f "$work/admin-form"
[ -n "$portal_token" ] && pass "the administrator makes a portal token on the page Request portal" \
	|| die "the administrator makes a portal token on the page Request portal"
umask 077
printf '%s' "$portal_token" > "$work/secrets/axis_portal_token"
umask 022
"$runtime" secret create "$prefix-axis_portal_token" "$work/secrets/axis_portal_token" >/dev/null \
	|| die "cannot create a secret"
requester_password=$(od -An -N18 -tx1 /dev/urandom | tr -d ' \n')
[ "$(admin_form /admin/users "username=robin" "password=$requester_password" "role=requester")" = 303 ] \
	&& pass "the administrator creates a requester account" || fail "the administrator creates a requester account"
rm -f "$work/admin-form"
# A title to ask for: there is no details provider here, so the record a
# search would store is written as the daemon stores one.
portal_title=$("$runtime" exec "$prefix-postgres2" psql --username axis --dbname axis --tuples-only --no-align \
	--quiet --no-psqlrc -c "INSERT INTO meta_title (kind, title, year, overview, provider, provider_id, adult,
	    detail_fetched_at) VALUES ('film', 'Glass Orchard', 2031, 'A made-up film.', 'tmdb', '990001', false, now())
	    RETURNING id" 2>/dev/null | head -n 1 | tr -d '[:space:]')
[ -n "$portal_title" ] || die "cannot write a title to ask for"

# The portal: the same image with another entrypoint, as a user id the image
# does not know (and not group 0), read-only with no tmpfs of the runtime's
# either, no capability, no new privileges, no volume, and one secret, its
# token, as a file.  Its environment: where the daemon's portal listener is,
# and who its reverse proxy is.  There is no proxy here: a client container
# at a fixed address plays it (it says https and names a client).
portal=$prefix-portal
portal_uid=61234
proxy_ip=${portal_subnet%.*}.250
"$runtime" run --detach --name "$portal" --label "axis-check=$prefix" \
	--user "$portal_uid:$portal_uid" --read-only --read-only-tmpfs=false --cap-drop ALL \
	--security-opt no-new-privileges --pids-limit 64 --memory 256m \
	--sysctl net.ipv6.conf.all.disable_ipv6=1 --sysctl net.ipv6.conf.default.disable_ipv6=1 \
	--network "$prefix-private2:alias=portal,ip=$portal_ip" \
	--secret "$prefix-axis_portal_token,target=axis_portal_token,uid=$portal_uid,mode=0400" \
	--env AXIS_PORTAL_URL=http://axis:7801 --env AXIS_PORTAL_ALLOW_INSECURE_HTTP=true \
	--env AXIS_PORTAL_TOKEN_FILE=/run/secrets/axis_portal_token \
	--env AXIS_PORTAL_WEB_LISTEN_HOST=0.0.0.0 --env "AXIS_PORTAL_TRUSTED_PROXIES=$proxy_ip" \
	--health-cmd "/usr/bin/axis-portal --healthcheck" --health-interval 2s --health-start-period 0s \
	--health-retries 3 --health-timeout 5s \
	--entrypoint /usr/bin/axis-portal "$image" >/dev/null || die "cannot start the portal"
if wait_healthy "$portal" 60; then
	pass "the portal starts and its health check passes (axis-portal --healthcheck, which needs no token)"
else
	die "the portal did not become healthy"
fi

[ "$(status_field "$portal" Uid | cut -f1)" = "$portal_uid" ] && [ "$(status_field "$portal" Gid | cut -f1)" = "$portal_uid" ] \
	&& pass "the portal runs as uid and gid $portal_uid (not in the image's passwd, not group 0)" \
	|| fail "the portal runs as uid and gid $portal_uid"
[ "$(status_field "$portal" CapEff)" = 0000000000000000 ] && pass "the portal has no capabilities" \
	|| fail "the portal has no capabilities"
[ "$(status_field "$portal" NoNewPrivs)" = 1 ] && pass "the portal cannot gain privileges" \
	|| fail "the portal cannot gain privileges"
[ "$("$runtime" inspect --format '{{.HostConfig.ReadonlyRootfs}}' "$portal")" = true ] \
	&& pass "the portal's root filesystem is read-only" || fail "the portal's root filesystem is read-only"
for dst in / /tmp /var/tmp /run /usr/share/axis; do
	out=$("$runtime" exec "$portal" touch "$dst/axis-check" 2>&1)
	case $out in
	*"Read-only file system"*) pass "a write to $dst from the portal fails: read-only file system" ;;
	*) fail "a write to $dst from the portal fails: read-only file system (got: $out)" ;;
	esac
done
[ -z "$("$runtime" inspect --format '{{range .Mounts}}{{.Destination}} {{end}}' "$portal" | tr -d ' ')" ] \
	&& pass "the portal has no volume and no mount" || fail "the portal has no volume and no mount"
penv=$("$runtime" inspect --format '{{range .Config.Env}}{{println .}}{{end}}' "$portal")
case $penv in
*DATABASE*|*POSTGRES*|*PASSWORD*|*AXIS_ADMIN*|*AXIS_WORKER*|*AXIS_METRICS*)
	fail "the portal's environment has no database address and no other secret" ;;
*) pass "the portal's environment has no database address and no other secret" ;;
esac
# What is there besides the portal's own settings is what the image sets for
# every program it runs, none of it a secret.
echo "  info     the portal's environment, names only: $(printf '%s\n' "$penv" | cut -d= -f1 | grep -v '^$' | tr '\n' ' ')"
case $("$runtime" logs "$portal" 2>&1) in
*"has not been reviewed for exposure to the internet"*) pass "the portal says at start that it is not reviewed for the internet" ;;
*) fail "the portal says at start that it is not reviewed for the internet" ;;
esac

# A requester logs in and asks, through the portal alone.  The client is a
# container of the same image (python3 is in it), with the script on its
# standard input.
out=$("$runtime" run --rm -i --name "$prefix-portal-client" --label "axis-check=$prefix" \
	--network "$prefix-private2:ip=$proxy_ip" --read-only --cap-drop ALL --security-opt no-new-privileges \
	--entrypoint /usr/bin/python3 "$image" \
	- ask http://portal:7802 "" robin "$requester_password" "$portal_title" 203.0.113.9 \
	< tests/check-portal-client.py 2>&1)
echo "$out" | sed 's/^/  info     client: /'
case $out in
*login=303*) pass "a requester logs in through the portal" ;;
*) fail "a requester logs in through the portal" ;;
esac
case $out in
*"cookie=__Host-axis_portal_session secure"*) pass "the portal's session cookie is Secure and __Host-" ;;
*) fail "the portal's session cookie is Secure and __Host-" ;;
esac
case $out in
*"ask=200 made"*"requests=200 listed"*) pass "the requester asks for a movie and sees it among My requests" ;;
*) fail "the requester asks for a movie and sees it among My requests" ;;
esac
[ "$("$runtime" exec "$prefix-postgres2" psql --username axis --dbname axis --tuples-only --no-align --no-psqlrc \
	-c "SELECT count(*) FROM request WHERE via = 'portal' AND decision = 'waiting' AND title_id = $portal_title" \
	2>/dev/null)" = 1 ] && pass "the request waits in the daemon's database" || fail "the request waits in the daemon's database"
case $("$runtime" logs "$any" 2>&1) in
*"portal login: user=robin reported_client=203.0.113.9"*) pass "the daemon is told the client's address the proxy named" ;;
*) fail "the daemon is told the client's address the proxy named" ;;
esac
# From anywhere else, forwarded headers count for nothing: no login is served.
out=$(printf 'import urllib.request, urllib.error\ntry:\n    print(urllib.request.urlopen(urllib.request.Request("http://portal:7802/login", headers={"X-Forwarded-Proto": "https", "X-Forwarded-For": "203.0.113.9"}), timeout=10).status)\nexcept urllib.error.HTTPError as e:\n    print(e.code)\n' \
	| "$runtime" run --rm -i --name "$prefix-portal-client" --label "axis-check=$prefix" \
		--network "$prefix-private2" --read-only --cap-drop ALL --security-opt no-new-privileges \
		--entrypoint /usr/bin/python3 "$image" - 2>&1)
[ "$out" = 403 ] && pass "from an address that is not listed, the same headers get no login (403)" \
	|| fail "from an address that is not listed, the same headers get no login (got: $out)"
# The portal's address (AXIS_PORTAL_PEERS) is the only one the portal API
# answers: from another container on the same network, with the portal's own
# token, 403 before the token is looked at.
out=$("$runtime" run --rm -i --name "$prefix-portal-client" --label "axis-check=$prefix" \
	--network "$prefix-private2" --read-only --cap-drop ALL --security-opt no-new-privileges --security-opt label=disable \
	--volume "$PWD/tests/check-portal-client.py:/client.py:ro" --entrypoint /usr/bin/python3 "$image" \
	/client.py api-hello http://axis:7801 < "$work/secrets/axis_portal_token" 2>&1)
case $out in
"hello=403 "*not-a-portal*) pass "the portal API answers the portal's address alone: 403 from another container with the right token" ;;
*) fail "the portal API answers the portal's address alone (got: $(echo "$out" | tail -c 120))" ;;
esac
# And nobody logs in to Axis itself from the portal's address: the
# administrator's right password, posted from the portal's container to the
# daemon's main port, opens no session, and the daemon says why.
out=$("$runtime" exec -i "$portal" python3 - engine-login http://axis:7800 "" admin "$admin_password" \
	< tests/check-portal-client.py 2>&1 | tr '\n' ' ')
case $out in
*form=200*login=401*session=no*) pass "from the portal's container, the administrator's right password is refused at the daemon's login as a wrong one is" ;;
*) fail "from the portal's container, the administrator's right password is refused at the daemon's login (got: $out)" ;;
esac
case $("$runtime" logs "$any" 2>&1) in
*"audit: login refused: the connection came from $portal_ip, one of the request portal's addresses (server.portal_peers, AXIS_PORTAL_PEERS)"*)
	pass "and the daemon's log says why, naming the address and the setting" ;;
*) fail "and the daemon's log says why, naming the address and the setting" ;;
esac
# The daemon knows the portal by its IPv4 address.  The portal's container
# has IPv6 switched off, as the examples run it, so it has no link-local
# address to be somebody else by: asked from inside it.
out=$("$runtime" exec -i "$portal" python3 - v6 http 7800 axis admin "$admin_password" \
	< tests/check-portal-client.py 2>&1 | tr '\n' ' ')
case $out in
"inet6=none "*) pass "the portal's container has no IPv6 address but loopback's (IPv6 is switched off in it)" ;;
*) fail "the portal's container has no IPv6 address but loopback's (got: $out)" ;;
esac
case $("$runtime" logs "$portal" "$any" 2>&1) in
*"$portal_token"*|*"$requester_password"*) fail "the portal's token and the requester's password are in no log" ;;
*) pass "the portal's token and the requester's password are in no log" ;;
esac
case $("$runtime" inspect "$portal" "$any" 2>&1) in
*"$portal_token"*) fail "the portal's token is not in the containers' configuration" ;;
*) pass "the portal's token is not in the containers' configuration" ;;
esac

t0=$SECONDS
"$runtime" stop -t 15 "$portal" >/dev/null 2>&1
code=$("$runtime" inspect --format '{{.State.ExitCode}}' "$portal")
[ "$code" = 0 ] && pass "the portal stopped with exit status 0" || fail "the portal stopped with exit status 0 (it was $code)"
[ $((SECONDS - t0)) -lt 10 ] && pass "the portal stopped in $((SECONDS - t0)) s" || fail "the portal stopped in $((SECONDS - t0)) s"
"$runtime" rm -f "$portal" >/dev/null 2>&1

stop_clean "$any"
"$runtime" stop -t 20 "$prefix-postgres2" >/dev/null 2>&1

echo
if [ "$failed" = 0 ]; then
	echo "Container checks passed."
else
	echo "Container checks FAILED." >&2
fi
exit "$failed"
