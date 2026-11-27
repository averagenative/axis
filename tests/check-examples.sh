#!/bin/bash
# check-examples.sh - run the deployment examples for real
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage: tests/check-examples.sh [PART...]       (or: make check-examples)
#
# PART is compose, full, traefik, quadlet or worker; with none, all five run.  Needs
# the image `make container` builds (AXIS_XTEST_IMAGE, default
# localhost/axis:latest).  It is NOT part of `make check` or `make
# check-container`: it pulls public images (postgres, traefik, and for the
# full stack prowlarr, sabnzbd and deluge from lscr.io, anonymously), needs a
# Compose front end and, for Quadlet, a user systemd.  Each part says why it
# is skipped when its prerequisite is missing; a skipped part is not a failure.
#
#   compose  deploy/examples/compose, copied, edited and started as its README
#            says (secret files, the first administrator from a secret file,
#            media paths): ready, logs in, media read-only inside; the quick
#            start's last step through the web interface (the disk, mounted
#            read-only, added as a volume without a marker, a library root on
#            it, a scan that finds the film, nothing written to the disk);
#            then ":ro" taken off and the stack started again: the marker is
#            written and the volume is recognised by it; then the documented
#            backup and restore (data checksums on, a backup taken while Axis
#            runs, a change made, Axis stopped, the database dropped and
#            created empty, pg_restore --exit-on-error, Axis started: the
#            change is gone, Axis is ready, the login works); then `down -v`.
#   full     deploy/examples/compose-full: Axis, PostgreSQL, Prowlarr, SABnzbd
#            and Deluge on one data mount: Axis ready; a file made by Deluge's
#            user is seen by Axis at the same path; a hardlink from downloads
#            to the library succeeds from inside Axis; the writable data mount
#            added as a volume (its marker written at once), a root, a scan.
#            If the images cannot be pulled in time, only `compose config`
#            runs.
#   traefik  deploy/examples/traefik with a real Traefik reading the example's
#            own labels through the Podman API socket: setup and login through
#            it, cookies Secure and __Host-, the audit line names the client's
#            address and not Traefik's, and the same forwarded headers sent
#            straight to Axis from an unlisted container are ignored; the
#            read-only disk added through Traefik, a root, a scan.  Traefik
#            is told its certificate authority is a local dead end, so nothing
#            reaches Let's Encrypt.  Then the request portal, added with
#            compose.portal.yaml as the README says: before it, no portal
#            runs and the daemon has no portal listener; with it, the
#            administrator makes a token and a requester, the portal starts
#            hardened, a requester logs in at the portal's own host name
#            through Traefik over https and asks for a movie, which waits on
#            the daemon's Requests page; the engine's paths are 404 at the
#            portal's host name; from the portal's container neither
#            PostgreSQL nor the daemon's main listener can be reached, and
#            Traefik answers it 403 for the engine's host name, so that the
#            administrator's right password posted that way opens no
#            session; the portal's container has no IPv6 address but
#            loopback's and can make no IPv6 connection to Traefik; by the
#            host's own address on each of the portal's networks (the
#            gateway) and Traefik's published port, the same password opens
#            no session from the portal's container, as the stack stands and
#            after each of three restarts of Traefik; a login
#            that Traefik does forward, for an address the check lists among
#            the portal's, is refused by the daemon; and with the portal's
#            container stopped, Traefik still refuses the portal's address.
#            (Traefik logs at debug level here, for the one step that prints
#            what its provider did when it fails.)
#   quadlet  deploy/examples/quadlet installed for the current user under
#            ~/.config/containers/systemd/ (every file prefixed axis-xtest-),
#            started with systemctl --user, ready, the read-only disk added, a
#            root, a scan, stopped, removed.  Without a usable user systemd,
#            Quadlet's generator dry run only.  Before the stop, the request
#            portal's two files as the README says: the daemon on the
#            portal's network with its portal listener, a token, a requester,
#            the portal started with nothing writable; no login without a
#            proxy, the login form when the check plays the proxy; the
#            database not reachable from the portal; the daemon's main port
#            open from there (this example cannot close it), and the
#            administrator's right password, posted to it from the portal's
#            container, refused by the daemon as a wrong one is; the portal's
#            container has no IPv6 address but loopback's and can make no
#            IPv6 connection to the daemon.
#
#   worker   deploy/examples/remote-worker, as far as it can be checked on one
#            machine without a GPU: `run` is valid shell; `compose config`
#            accepts compose.yaml as written (an NVIDIA GPU), with its VA-API
#            lines switched on, and with no GPU at all; Quadlet's generator
#            accepts axis-worker.container in a dry run.  Nothing is started,
#            installed or pulled.
#
# Not run: the worker of deploy/examples/remote-worker itself (Axis on a
# server, the worker on another machine), which needs two machines, a mount
# of one's disks on the other and, for two of its three encoders, a GPU.
# Its worker is the one the compose part's image carries, and its checks
# are the tests' (tests/test-conversion.c, test-worker-*.c).
#
# Everything made is named axis-xtest-* (Quadlet's own default names add the
# prefix systemd-): containers, pods, networks, volumes, secrets, units and a
# temporary directory.  At the start and at the end (however it ends) the
# script lists what carries the prefix and removes exactly that, by name.
# Nothing else is touched.  The image and the pulled public images are kept.
# Run one copy at a time.  The examples' ports are replaced by unused ones
# (the examples read AXIS_PORT, TRAEFIK_HTTPS_PORT and so on from .env).
#
# Needs: podman, curl, sed, ss, openssl, and for each part what it says.

set -u

cd "$(dirname "$0")/.." || exit 1
repo=$PWD

image=${AXIS_XTEST_IMAGE:-localhost/axis:latest}
# AXIS_XTEST_PREFIX: another prefix for what the Compose parts make (compose,
# full, traefik), so that one of them can be run by hand beside nothing of
# the usual name: axis-xtest and up to twelve letters or digits more
# (axis-xtesthand, say; anything else is refused, below).  The quadlet part's
# names are axis-xtest-* whatever it says, so that part runs with the usual
# prefix only and is skipped with another.
prefix=${AXIS_XTEST_PREFIX:-axis-xtest}
# The prefix decides what this script removes: every container, pod, network,
# volume and secret named <prefix>-*, and Podman's name server files of such
# networks.  A prefix like "axis" would be the real installation's names
# (axis-portal, axis-proxy, axis-db).  So it is a test's prefix or the script
# does nothing: axis-xtest, with at most twelve lower-case letters or digits
# after it and nothing else.  Refused here, before the cleanup is armed and
# before anything is listed.
if [[ ! $prefix =~ ^axis-xtest[a-z0-9]{0,12}$ ]]; then
	echo "check-examples: AXIS_XTEST_PREFIX must be axis-xtest, with at most twelve lower-case letters or digits" \
		"after it; \"$prefix\" could be the name of something real, and this script removes everything named" \
		"after its prefix. Nothing was listed and nothing was removed." >&2
	exit 2
fi
pull_seconds=${AXIS_XTEST_PULL_SECONDS:-420}
client_image=docker.io/library/python:3.12-slim
# The image the Traefik example names (a check below holds the two together).
traefik_image=docker.io/library/traefik:v3.6
puid=$(id -u)
pgid=$(id -g)
# The image's own user; what the examples run Axis as unless told otherwise.
axis_uid=1000

work=
passed=0
failed=0
skipped=0
used_ports=
started_socket=
quadlet_dir=$HOME/.config/containers/systemd
quadlet_dir_made=
quadlet_files=
compose=

export PODMAN_COMPOSE_WARNING_LOGS=false

say() {
	printf '  %-8s %s\n' "$1" "$2"
}

pass() {
	say ok "$1"
	passed=$((passed + 1))
}

fail() {
	say FAIL "$1" >&2
	failed=$((failed + 1))
}

skip() {
	say skipped "$1"
	skipped=$((skipped + 1))
}

# check DESCRIPTION COMMAND [ARG...]
check() {
	local description=$1
	shift
	if "$@" >/dev/null 2>&1; then
		pass "$description"
	else
		fail "$description"
	fi
}

# ---------------------------------------------------------------------------
# What this script made, and its removal.

is_mine() {
	case $1 in
	"$prefix"-* | systemd-"$prefix"-* | pod_"$prefix"-*) return 0 ;;
	esac
	return 1
}

# remove_mine: list everything that carries the prefix, then remove it by name.
remove_mine() {
	local kind name found=0 listing
	local -a names

	for kind in container pod network volume secret; do
		case $kind in
		container) listing=$(podman ps -a --format '{{.Names}}' 2>/dev/null) ;;
		pod) listing=$(podman pod ls --format '{{.Name}}' 2>/dev/null) ;;
		network) listing=$(podman network ls --format '{{.Name}}' 2>/dev/null) ;;
		volume) listing=$(podman volume ls --format '{{.Name}}' 2>/dev/null) ;;
		secret) listing=$(podman secret ls --format '{{.Name}}' 2>/dev/null) ;;
		esac
		names=()
		for name in $listing; do
			is_mine "$name" && names+=("$name")
		done
		[ "${#names[@]}" -gt 0 ] || continue
		found=1
		say remove "$kind: ${names[*]}"
		for name in "${names[@]}"; do
			is_mine "$name" || continue
			case $kind in
			container) podman rm -f -t 0 "$name" >/dev/null 2>&1 ;;
			pod) podman pod rm -f -t 0 "$name" >/dev/null 2>&1 ;;
			network) podman network rm -f "$name" >/dev/null 2>&1 ;;
			volume) podman volume rm -f "$name" >/dev/null 2>&1 ;;
			secret) podman secret rm "$name" >/dev/null 2>&1 ;;
			esac
		done
	done
	[ "$found" = 1 ] || say remove "nothing named $prefix-* is left over"
	remove_stale_names
}

# remove_stale_names: Podman's name server (aardvark-dns) keeps a file for
# each network, by the network's name, with the names and addresses of its
# containers.  A container that went without being taken out of it (a run
# that was killed) stays in the file, the file outlives its network, and the
# next network of that name answers for the dead container too: "postgres"
# then has two addresses, one of which nothing answers at, and a connection
# that tries it first waits three seconds.  The files of networks with this
# script's prefix that no longer exist are removed, by name.
remove_stale_names() {
	local dir=${XDG_RUNTIME_DIR:-}/containers/networks/aardvark-dns f name
	[ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$dir" ] || return 0
	for f in "$dir"/*; do
		[ -f "$f" ] || continue
		name=${f##*/}
		is_mine "$name" || continue
		podman network exists "$name" 2>/dev/null && continue
		rm -f "$f" && say remove "a name server file left by an earlier run: $name"
	done
}

# nothing_left: after the removal, the same listing must be empty.
nothing_left() {
	local name left=
	for name in $(podman ps -a --format '{{.Names}}' 2>/dev/null) \
		$(podman pod ls --format '{{.Name}}' 2>/dev/null) \
		$(podman network ls --format '{{.Name}}' 2>/dev/null) \
		$(podman volume ls --format '{{.Name}}' 2>/dev/null) \
		$(podman secret ls --format '{{.Name}}' 2>/dev/null); do
		is_mine "$name" && left="$left $name"
	done
	if [ -z "$left" ]; then
		pass "nothing named $prefix-* is left in Podman"
	else
		fail "left in Podman:$left"
	fi
}

remove_quadlet_files() {
	local f
	# A unit left running outlives its file: the network's unit stays "active"
	# after its network is removed, and the next run's start then never makes
	# the network again.  So the units are stopped before their files go, on
	# every path, and systemd forgets them afterwards.
	if [ -n "$quadlet_files" ]; then
		systemctl --user stop 'axis-xtest-*' >/dev/null 2>&1
	fi
	for f in $quadlet_files; do
		case $f in
		"$quadlet_dir"/axis-xtest-*) rm -f "$f" ;;
		esac
	done
	if [ -n "$quadlet_files" ]; then
		systemctl --user daemon-reload >/dev/null 2>&1
		systemctl --user reset-failed 'axis-xtest-*' >/dev/null 2>&1
	fi
	quadlet_files=
	if [ -n "$quadlet_dir_made" ]; then
		rmdir "$quadlet_dir" 2>/dev/null
		quadlet_dir_made=
	fi
}

cleanup() {
	trap - EXIT
	cd "$repo" || true
	if [ -n "$quadlet_files" ]; then
		systemctl --user stop 'axis-xtest-*' >/dev/null 2>&1
		remove_quadlet_files
		systemctl --user daemon-reload >/dev/null 2>&1
		systemctl --user reset-failed 'axis-xtest-*' >/dev/null 2>&1
	fi
	remove_mine
	if [ -n "$started_socket" ]; then
		systemctl --user stop podman.socket podman.service >/dev/null 2>&1
		# Stopping the unit leaves the socket file behind, and a leftover file
		# looks like a listening socket to the next reader.
		rm -f "$XDG_RUNTIME_DIR/podman/podman.sock"
		rmdir "$XDG_RUNTIME_DIR/podman" 2>/dev/null
		say remove "podman.socket (started by this script) stopped again"
	fi
	if [ -n "$work" ] && [ -d "$work" ]; then
		# Files chowned inside the user namespace belong to a subordinate id.
		podman unshare rm -rf "$work" >/dev/null 2>&1 || rm -rf "$work"
		say remove "scratch directory"
	fi
}

trap cleanup EXIT
trap 'exit 130' INT TERM

# ---------------------------------------------------------------------------
# Helpers.

free_port() {
	local p n=0
	while [ "$n" -lt 100 ]; do
		p=$((20000 + RANDOM % 20000))
		case " $used_ports " in
		*" $p "*) n=$((n + 1)); continue ;;
		esac
		if ! ss -ltnH 2>/dev/null | grep -q "[:.]$p "; then
			used_ports="$used_ports $p"
			echo "$p"
			return 0
		fi
		n=$((n + 1))
	done
	return 1
}

# wait_for SECONDS COMMAND...: true once the command succeeds.
wait_for() {
	local n=0 limit=$1
	shift
	while [ "$n" -lt "$limit" ]; do
		"$@" >/dev/null 2>&1 && return 0
		sleep 1
		n=$((n + 1))
	done
	return 1
}

# steady TIMES LIMIT COMMAND...: true once the command has succeeded TIMES
# times on end, a second apart, within LIMIT tries.
steady() {
	local need=$1 limit=$2 run=0 n=0
	shift 2
	while [ "$n" -lt "$limit" ]; do
		if "$@" >/dev/null 2>&1; then
			run=$((run + 1))
			[ "$run" -ge "$need" ] && return 0
		else
			run=0
		fi
		sleep 1
		n=$((n + 1))
	done
	return 1
}

body_is() { # URL EXPECTED [curl option...]
	local url=$1 expected=$2
	shift 2
	[ "$(curl -sk --max-time 5 "$@" "$url")" = "$expected" ]
}

health_is() { # CONTAINER STATUS
	[ "$(podman inspect --format '{{.State.Health.Status}}' "$1" 2>/dev/null)" = "$2" ]
}

# container_of PROJECT SERVICE: the name podman-compose gave the container.
container_of() {
	podman ps -a --filter "label=io.podman.compose.project=$1" \
		--filter "label=com.docker.compose.service=$2" --format '{{.Names}}' | head -n 1
}

# login BASE ORIGIN USER PASSWORD JAR [curl option...]: the login form, as a
# browser does it; true on the 303 that follows a good login.
login() {
	local base=$1 origin=$2 user=$3 password=$4 jar=$5 page csrf code
	shift 5
	page=$(curl -sk --max-time 10 "$@" -c "$jar" -D "$jar.headers" "$base/login") || return 1
	csrf=$(printf '%s' "$page" | grep -o '<input[^>]*name="_csrf"[^>]*>' | head -n 1 |
		sed -n 's/.* value="\([^"]*\)".*/\1/p')
	[ -n "$csrf" ] || return 1
	code=$(curl -sk --max-time 10 "$@" -b "$jar" -c "$jar" -o /dev/null -w '%{http_code}' \
		-H "Origin: $origin" --data-urlencode "_csrf=$csrf" \
		--data-urlencode "username=$user" --data-urlencode "password=$password" "$base/login")
	[ "$code" = 303 ]
}

random_hex() {
	od -An -N"$1" -tx1 /dev/urandom | tr -d ' \n'
}

# The curl options of the part being run (Traefik's --resolve), for the web
# helpers below.
curl_extra=()

# field_value PAGE NAME: the value of the form field NAME in PAGE.
field_value() {
	printf '%s' "$1" | grep -o "<input[^>]*name=\"$2\"[^>]*>" | head -n 1 |
		sed -n 's/.* value="\([^"]*\)".*/\1/p'
}

# web_get BASE JAR PATH: a page, as the logged-in browser gets it.
web_get() {
	curl -sk --max-time 20 "${curl_extra[@]}" -b "$2" -c "$2" "$1$3"
}

# web_post BASE ORIGIN JAR PATH OUT FIELD=VALUE...: a form posted as the
# browser posts it, with the session's CSRF token.  Prints the status; the
# headers go to OUT and the body to OUT.body.
web_post() {
	local base=$1 origin=$2 jar=$3 path=$4 out=$5 csrf field
	local -a data
	shift 5
	csrf=$(web_get "$base" "$jar" /storage | grep -o '<input[^>]*name="_csrf"[^>]*>' | head -n 1 |
		sed -n 's/.* value="\([^"]*\)".*/\1/p')
	data=(--data-urlencode "_csrf=$csrf")
	for field in "$@"; do
		data+=(--data-urlencode "$field")
	done
	curl -sk --max-time 30 "${curl_extra[@]}" -b "$jar" -c "$jar" -D "$out" -o "$out.body" -w '%{http_code}' \
		-H "Origin: $origin" "${data[@]}" "$base$path"
}

# page_has BASE JAR PATH TEXT: true when the page holds TEXT.
page_has() {
	web_get "$1" "$2" "$3" | grep -qF -- "$4"
}

# A made-up film for the scans to find.
film_dir='Glass Orchard (2031)'

# put_film DIR: the film, in a folder of its own under DIR.
put_film() {
	mkdir -p "$1/$film_dir"
	echo frames > "$1/$film_dir/$film_dir.mkv"
}

# add_first_disk PART BASE ORIGIN JAR DISK PLAN ROOT: the quick start's last
# step, through the web interface as a person does it: Storage, Add a volume,
# the disk mounted at DISK in the container, look, confirm; a library root of
# movies at ROOT on it; Scan now; the scan finds the film.  PLAN is the plan
# the confirmation page must show: identify for a disk mounted read-only
# without a marker, write for a writable one.  Sets volume to the volume's
# number.
add_first_disk() {
	local part=$1 base=$2 origin=$3 jar=$4 disk=$5 want=$6 root_path=$7 out=$work/$1-web page plan code root
	volume=
	code=$(web_post "$base" "$origin" "$jar" /storage/volumes/inspect "$out.inspect" "path=$disk" "name=Disk 1")
	page=$(cat "$out.inspect.body")
	plan=$(field_value "$page" plan)
	if [ "$code" = 200 ] && [ "$plan" = "$want" ]; then
		pass "$part: Add a volume, $disk: the page shows the plan '$plan'"
	else
		fail "$part: Add a volume, $disk: the page shows the plan '$want' (status $code, plan '${plan:-none}')"
		printf '%s' "$page" | grep -o 'role="alert">[^<]*' | head -n 2 | cut -c 1-400 >&2
		return 1
	fi
	code=$(web_post "$base" "$origin" "$jar" /storage/volumes "$out.add" \
		"path=$(field_value "$page" path)" "name=Disk 1" "plan=$plan" "dev=$(field_value "$page" dev)" \
		"ino=$(field_value "$page" ino)" "fsid=$(field_value "$page" fsid)" "size=$(field_value "$page" size)" \
		"marker=$(field_value "$page" marker)" "confirm=yes")
	volume=$(sed -n 's/^[Ll]ocation: *//p' "$out.add" | tr -d '\r' | head -n 1)
	volume=${volume#/storage/volumes/}
	volume=${volume%%\?*}
	case $code:$volume in
	303:[0-9]*) pass "$part: confirmed: volume $volume is added" ;;
	*)
		fail "$part: confirmed: the volume is added (status $code)"
		grep -o 'role="alert">[^<]*' "$out.add.body" | head -n 2 | cut -c 1-400 >&2
		return 1
		;;
	esac
	code=$(web_post "$base" "$origin" "$jar" /storage/roots "$out.root" "volume=$volume" "path=$root_path" \
		"type=movies" "name=" "return=volume")
	root=$(web_get "$base" "$jar" /storage/roots | grep -o 'href="/storage/roots/[0-9]*"' | head -n 1 | tr -dc '0-9')
	if [ "$code" = 303 ] && [ -n "$root" ]; then
		pass "$part: $root_path on it is added as a library root of movies"
	else
		fail "$part: $root_path on it is added as a library root of movies (status $code)"
		return 1
	fi
	code=$(web_post "$base" "$origin" "$jar" "/storage/roots/$root/scan" "$out.scan")
	[ "$code" = 303 ] && pass "$part: Scan now is accepted" || fail "$part: Scan now is accepted (status $code)"
	if wait_for 180 page_has "$base" "$jar" "/library/root/$root" "last complete scan"; then
		pass "$part: the scan completes"
	else
		fail "$part: the scan completes"
	fi
	page_has "$base" "$jar" "/library/root/$root" "$film_dir" && pass "$part: the scan finds the film" ||
		fail "$part: the scan finds the film"
}

# have_image IMAGE: present, or pulled anonymously within the time allowed.
have_image() {
	podman image exists "$1" 2>/dev/null && return 0
	timeout "$pull_seconds" podman pull -q "$1" >/dev/null 2>&1
}

compose_in() { # DIR PROJECT ARGS...: the compose front end, run in DIR
	local dir=$1 project=$2
	shift 2
	(cd "$dir" && $compose -p "$project" "$@")
}

# edit FILE SED-ARG...: what a reader does with an editor, in place.
edit() {
	local file=$1
	shift
	sed "$@" "$file" > "$file.new" && mv "$file.new" "$file"
}

# On an SELinux host a bind mount is unreadable to the container unless it is
# labelled; deploy/README.md says to add label=disable to security_opt.
selinux_edit() {
	if [ "$(getenforce 2>/dev/null)" = Enforcing ]; then
		edit "$1" -e 's/^\( *\)- no-new-privileges:true$/&\n\1- label=disable/'
	fi
}

# The README's first step for the minimal and Traefik examples, verbatim.
make_secrets() {
	mkdir -m 700 secrets
	pw=$(openssl rand -hex 24)
	printf '%s' "$pw" > secrets/axis_db_password
	printf 'postgresql://axis:%s@postgres:5432/axis\n' "$pw" > secrets/axis_database_url
	: > secrets/axis_worker_token
	chmod 644 secrets/*
	unset pw
}

# backup_and_restore DIR PROJECT PORT ADMIN-PASSWORD
# The commands of docs/14-back-up-restore-and-repair-the-database.md and
# deploy/README.md, run against the running Compose example: data checksums
# are on; a backup is taken while Axis runs and is whole; a table is made
# after it; Axis is stopped, the database dropped and created empty, the
# backup restored with --exit-on-error, Axis started; the table is gone, the
# data of the backup is back (the administrator logs in) and Axis is ready.
backup_and_restore() {
	local dir=$1 proj=$2 port=$3 admin=$4 dump=$work/axis-xtest.dump jar=$work/restore.jar tables

	say info "compose: backup and restore"
	if [ "$(compose_in "$dir" "$proj" exec -T postgres psql --username axis --dbname axis --tuples-only --no-align --command 'SHOW data_checksums' 2>/dev/null | tr -d '[:space:]')" = on ]; then
		pass "compose: the database has data checksums on"
	else
		fail "compose: the database has data checksums on"
	fi

	if compose_in "$dir" "$proj" exec -T postgres pg_dump --username axis --format custom axis > "$dump" 2> "$work/dump.err" &&
		[ -s "$dump" ]; then
		pass "compose: pg_dump takes a backup while Axis runs"
	else
		fail "compose: pg_dump takes a backup while Axis runs"
		return
	fi
	tables=$(podman run --rm -i --network none docker.io/library/postgres:17 pg_restore --list < "$dump" 2>/dev/null | grep -c 'TABLE DATA')
	[ "${tables:-0}" -gt 0 ] && pass "compose: pg_restore --list reads the backup ($tables tables)" ||
		fail "compose: pg_restore --list reads the backup"

	# Something that changes after the backup: a table the backup does not have.
	if compose_in "$dir" "$proj" exec -T postgres psql --username axis --dbname axis --command 'CREATE TABLE backup_probe (n integer)' > /dev/null 2>&1; then
		pass "compose: a change made after the backup"
	else
		fail "compose: a change made after the backup"
		return
	fi

	check "compose: restore: axis stopped" compose_in "$dir" "$proj" stop axis
	check "compose: restore: the database dropped" \
		compose_in "$dir" "$proj" exec -T postgres dropdb --username axis --force axis
	check "compose: restore: the database created empty" \
		compose_in "$dir" "$proj" exec -T postgres createdb --username axis --owner axis axis
	if compose_in "$dir" "$proj" exec -T postgres pg_restore --username axis --dbname axis --exit-on-error < "$dump" > "$work/restore.out" 2>&1; then
		pass "compose: pg_restore --exit-on-error restores the backup"
	else
		fail "compose: pg_restore --exit-on-error restores the backup"
		tail -n 10 "$work/restore.out" >&2
	fi
	check "compose: restore: axis started" compose_in "$dir" "$proj" start axis

	if wait_for 240 body_is "http://127.0.0.1:$port/readyz" '{"status":"ready"}'; then
		pass "compose: after the restore /readyz is ready"
	else
		fail "compose: after the restore /readyz is ready"
		podman logs --tail 20 "$(container_of "$proj" axis)" >&2 2>&1
	fi
	if [ "$(compose_in "$dir" "$proj" exec -T postgres psql --username axis --dbname axis --tuples-only --no-align --command "SELECT to_regclass('backup_probe') IS NULL" 2>/dev/null | tr -d '[:space:]')" = t ]; then
		pass "compose: the change made after the backup is gone"
	else
		fail "compose: the change made after the backup is gone"
	fi
	check "compose: the administrator of the backup logs in after the restore" \
		login "http://127.0.0.1:$port" "http://127.0.0.1:$port" admin "$admin" "$jar"
}

# ---------------------------------------------------------------------------
# Part: compose

part_compose() {
	local proj=$prefix-c dir=$work/compose port admin axis jar line volume=

	echo "compose: deploy/examples/compose"
	if [ -z "$compose" ]; then
		skip "compose: no Compose front end (podman compose or docker compose)"
		return
	fi
	port=$(free_port) || { fail "compose: no free port"; return; }
	cp -R "$repo/deploy/examples/compose" "$dir" || { fail "compose: copy"; return; }
	cd "$dir" || return
	make_secrets
	admin=$(random_hex 12)
	printf '%s' "$admin" > secrets/axis_admin_password
	chmod 644 secrets/axis_admin_password
	mkdir -p media/disk1 media/disk2
	echo marker > media/disk1/marker.txt
	put_film media/disk1/Movies
	chmod -R a+rX media
	# The README's lines to edit: the disks, and the first administrator from
	# a secret file (the three axis_admin_password lines and the two
	# AXIS_ADMIN_* lines uncommented).
	edit compose.yaml \
		-e "s|/path/to/disk1|$dir/media/disk1|" -e "s|/path/to/disk2|$dir/media/disk2|" \
		-e 's/^\( *\)# \(- axis_admin_password\)$/\1\2/' \
		-e 's/^\( *\)# \(AXIS_ADMIN_[A-Z_]*:.*\)$/\1\2/' \
		-e 's/^\( *\)# \(axis_admin_password:\)$/\1\2/' \
		-e 's|^\( *\)#   \(file: ./secrets/axis_admin_password\)$|\1  \2|'
	selinux_edit compose.yaml
	printf 'AXIS_IMAGE=%s\nAXIS_PORT=%s\nPUID=%s\nPGID=%s\n' "$image" "$port" "$axis_uid" "$axis_uid" > .env

	if compose_in "$dir" "$proj" up -d > "$work/compose.log" 2>&1; then
		pass "compose: up -d"
	else
		fail "compose: up -d (see below)"
		tail -n 20 "$work/compose.log" >&2
		compose_in "$dir" "$proj" down -v >/dev/null 2>&1
		return
	fi
	axis=$(container_of "$proj" axis)
	if wait_for 240 body_is "http://127.0.0.1:$port/readyz" '{"status":"ready"}'; then
		pass "compose: /readyz is ready"
	else
		fail "compose: /readyz is ready"
		podman logs --tail 20 "$axis" >&2 2>&1
	fi
	body_is "http://127.0.0.1:$port/livez" '{"status":"alive"}' && pass "compose: /livez is alive" ||
		fail "compose: /livez is alive"
	if wait_for 120 health_is "$axis" healthy; then
		pass "compose: axis is healthy"
	else
		fail "compose: axis is healthy"
	fi
	[ "$(podman inspect --format '{{.State.Health.Status}}' "$(container_of "$proj" postgres)")" = healthy ] &&
		pass "compose: postgres is healthy" || fail "compose: postgres is healthy"
	if podman logs "$axis" 2>&1 | grep -q 'setup token'; then
		fail "compose: no setup token in the log when the administrator comes from a secret"
	else
		pass "compose: no setup token in the log when the administrator comes from a secret"
	fi

	jar=$work/compose.jar
	if login "http://127.0.0.1:$port" "http://127.0.0.1:$port" admin "$admin" "$jar"; then
		pass "compose: the first administrator logs in with the password from the secret file"
	else
		fail "compose: the first administrator logs in with the password from the secret file"
	fi
	line=$(podman logs "$axis" 2>&1 | grep 'audit: login: user=admin' | tail -n 1)
	[ -n "$line" ] && say info "$line"
	no_portal compose "$(container_of "$proj" portal)" "$axis"

	if podman exec "$axis" sh -c 'touch /media/disk1/x 2>&1' | grep -qi 'read-only'; then
		pass "compose: the media mount is read-only inside the container"
	else
		fail "compose: the media mount is read-only inside the container"
	fi
	check "compose: the media are readable inside the container" \
		podman exec "$axis" cat /media/disk1/marker.txt
	check "compose: the root filesystem is read-only" \
		sh -c "! podman exec '$axis' touch /x 2>/dev/null"
	check "compose: no port but axis's is published on the host" \
		sh -c "[ \"\$(podman ps --filter 'label=io.podman.compose.project=$proj' --format '{{.Ports}}' | tr ',' '\n' | grep -c '0.0.0.0')\" = 0 ]"

	# The quick start's last step, on the disk as the example mounts it
	# (read-only, no marker): added, recognised by its filesystem, scanned,
	# and nothing written to it.
	if add_first_disk compose "http://127.0.0.1:$port" "http://127.0.0.1:$port" "$jar" /media/disk1 identify Movies; then
		page_has "http://127.0.0.1:$port" "$jar" "/storage/volumes/$volume" "state-read_only" &&
			pass "compose: the volume is online, mounted read-only" ||
			fail "compose: the volume is online, mounted read-only"
		page_has "http://127.0.0.1:$port" "$jar" "/storage/volumes/$volume" "It has no marker yet." &&
			pass "compose: its page says it is recognised by its filesystem identifier and has no marker yet" ||
			fail "compose: its page says it is recognised by its filesystem identifier and has no marker yet"
	fi
	[ ! -e media/disk1/.axis-volume ] && pass "compose: nothing is written to the read-only disk, the marker included" ||
		fail "compose: nothing is written to the read-only disk, the marker included"

	# Later the disk is made writable (":ro" taken off, the quick start's
	# note): Axis writes the marker then, and recognises the disk by it.  In
	# rootless Podman the folder is given to the container's user first.
	podman unshare chown "$axis_uid:$axis_uid" media/disk1
	edit compose.yaml -e "s|^\( *- $dir/media/disk1:/media/disk1\):ro\$|\1|"
	if compose_in "$dir" "$proj" down > "$work/compose-down-1.log" 2>&1 &&
		compose_in "$dir" "$proj" up -d > "$work/compose-up-2.log" 2>&1; then
		pass "compose: down, then up -d with the disk mounted read-write"
	else
		fail "compose: down, then up -d with the disk mounted read-write"
	fi
	axis=$(container_of "$proj" axis)
	wait_for 240 body_is "http://127.0.0.1:$port/readyz" '{"status":"ready"}'
	jar=$work/compose-2.jar
	check "compose: the administrator logs in again" \
		login "http://127.0.0.1:$port" "http://127.0.0.1:$port" admin "$admin" "$jar"
	if wait_for 120 test -f "$dir/media/disk1/.axis-volume"; then
		pass "compose: the marker is written the first time Axis finds the disk mounted read-write"
	else
		fail "compose: the marker is written the first time Axis finds the disk mounted read-write"
	fi
	if [ -n "$volume" ]; then
		page_has "http://127.0.0.1:$port" "$jar" "/storage/volumes/$volume" "Its marker, .axis-volume" &&
			pass "compose: from then on the volume is recognised by its marker" ||
			fail "compose: from then on the volume is recognised by its marker"
		page_has "http://127.0.0.1:$port" "$jar" "/storage/volumes/$volume" "state-online" &&
			pass "compose: the volume is online, mounted read-write" ||
			fail "compose: the volume is online, mounted read-write"
	fi

	backup_and_restore "$dir" "$proj" "$port" "$admin"
	cd "$dir" || return

	if compose_in "$dir" "$proj" down -v > "$work/compose-down.log" 2>&1; then
		pass "compose: down -v"
	else
		fail "compose: down -v"
	fi
	check "compose: its volumes are gone after down -v" \
		sh -c "[ -z \"\$(podman volume ls --format '{{.Name}}' | grep '^$proj')\" ]"
	cd "$repo" || true
}

# ---------------------------------------------------------------------------
# Part: full

part_full() {
	local proj=$prefix-f dir=$work/full ports axis sab deluge prowlarr admin img pulled=1 volume=
	local -a ports_env

	echo "full: deploy/examples/compose-full"
	if [ -z "$compose" ]; then
		skip "full: no Compose front end (podman compose or docker compose)"
		return
	fi
	cp -R "$repo/deploy/examples/compose-full" "$dir" || { fail "full: copy"; return; }
	cd "$dir" || return
	for img in lscr.io/linuxserver/prowlarr:latest lscr.io/linuxserver/sabnzbd:latest \
		lscr.io/linuxserver/deluge:latest; do
		have_image "$img" || pulled=0
	done
	have_image docker.io/library/postgres:17 || pulled=0

	# README step 1.
	mkdir -m 700 secrets
	pw=$(openssl rand -hex 24)
	admin=$(random_hex 12)
	printf '%s' "$pw" > secrets/axis_db_password
	printf 'postgresql://axis:%s@postgres:5432/axis\n' "$pw" > secrets/axis_database_url
	printf '%s' "$admin" > secrets/axis_admin_password
	chmod 644 secrets/*
	unset pw
	# README: the folders first, owned by PUID:PGID.  Rootless Podman counts
	# the ids inside a user namespace, so the folders are given to them there.
	mkdir -p data/downloads/usenet/incomplete data/downloads/usenet/complete data/downloads/torrents \
		data/media/movies data/media/tv
	put_film data/media/movies
	podman unshare chown -R "$axis_uid:$axis_uid" data
	edit compose.yaml -e "s|/path/to/data|$dir/data|"
	selinux_edit compose.yaml
	# linuxserver.io's services have no no-new-privileges line to hang it on.
	if [ "$(getenforce 2>/dev/null)" = Enforcing ]; then
		edit compose.yaml -e 's/^\(  \(prowlarr\|sabnzbd\|deluge\):\)$/\1\n    security_opt:\n      - label=disable/'
	fi
	ports_env=("AXIS_PORT=$(free_port)" "PROWLARR_PORT=$(free_port)" "SABNZBD_PORT=$(free_port)"
		"DELUGE_PORT=$(free_port)" "TORRENT_PORT=$(free_port)")
	{
		printf 'AXIS_IMAGE=%s\nPUID=%s\nPGID=%s\nTZ=Etc/UTC\n' "$image" "$axis_uid" "$axis_uid"
		printf '%s\n' "${ports_env[@]}"
	} > .env
	ports=$(sed -n 's/^AXIS_PORT=//p' .env)

	if [ "$pulled" = 0 ]; then
		skip "full: the images could not be pulled in ${pull_seconds}s; only compose config runs"
		if compose_in "$dir" "$proj" config > "$work/full-config.out" 2>&1; then
			pass "full: compose config accepts the file"
		else
			fail "full: compose config accepts the file"
		fi
		cd "$repo" || true
		return
	fi

	if compose_in "$dir" "$proj" up -d > "$work/full.log" 2>&1; then
		pass "full: up -d"
	else
		fail "full: up -d (see below)"
		tail -n 20 "$work/full.log" >&2
		compose_in "$dir" "$proj" down -v >/dev/null 2>&1
		cd "$repo" || true
		return
	fi
	axis=$(container_of "$proj" axis)
	sab=$(container_of "$proj" sabnzbd)
	deluge=$(container_of "$proj" deluge)
	prowlarr=$(container_of "$proj" prowlarr)
	if wait_for 300 body_is "http://127.0.0.1:$ports/readyz" '{"status":"ready"}'; then
		pass "full: Axis is ready"
	else
		fail "full: Axis is ready"
		podman logs --tail 20 "$axis" >&2 2>&1
	fi
	check "full: the first administrator (secret file) logs in" \
		login "http://127.0.0.1:$ports" "http://127.0.0.1:$ports" admin "$admin" "$work/full.jar"
	# The download clients start as root and drop to PUID themselves; give them
	# time to be up before making files as that user.
	wait_for 120 sh -c "[ \"\$(podman inspect --format '{{.State.Running}}' '$deluge')\" = true ] && podman exec '$deluge' true"
	for c in "$sab" "$deluge" "$prowlarr"; do
		[ "$(podman inspect --format '{{.State.Running}}' "$c" 2>/dev/null)" = true ] &&
			pass "full: $c is running" || fail "full: $c is running"
	done

	# README: the same device number for both folders in Axis.
	if [ "$(podman exec "$axis" stat -c '%d' /data/downloads)" = "$(podman exec "$axis" stat -c '%d' /data/media)" ]; then
		pass "full: /data/downloads and /data/media are on one mount in Axis"
	else
		fail "full: /data/downloads and /data/media are on one mount in Axis"
	fi
	# A file made in the downloads directory by one container, as its PUID user.
	if podman exec --user "$axis_uid:$axis_uid" "$deluge" sh -c 'echo film > /data/downloads/torrents/xtest.mkv'; then
		pass "full: Deluge's user makes a file in /data/downloads/torrents"
	else
		fail "full: Deluge's user makes a file in /data/downloads/torrents"
	fi
	check "full: Axis sees that file at /data/downloads/torrents/xtest.mkv" \
		podman exec "$axis" test -f /data/downloads/torrents/xtest.mkv
	check "full: SABnzbd sees it at the same path" \
		podman exec "$sab" test -f /data/downloads/torrents/xtest.mkv
	# The one-mount layout: a hardlink from downloads into the library, from
	# inside the Axis container, as Axis's own user.
	if podman exec "$axis" ln /data/downloads/torrents/xtest.mkv /data/media/movies/xtest.mkv; then
		pass "full: a hardlink from downloads into the library succeeds inside Axis"
	else
		fail "full: a hardlink from downloads into the library succeeds inside Axis"
	fi
	[ "$(podman exec "$axis" stat -c '%h' /data/media/movies/xtest.mkv 2>/dev/null)" = 2 ] &&
		pass "full: the imported file has a link count of 2" || fail "full: the imported file has a link count of 2"
	[ "$(podman exec "$axis" stat -c '%i' /data/media/movies/xtest.mkv 2>/dev/null)" = \
		"$(podman exec "$axis" stat -c '%i' /data/downloads/torrents/xtest.mkv 2>/dev/null)" ] &&
		pass "full: both names are the same inode" || fail "full: both names are the same inode"
	check "full: Deluge sees the library name too" \
		podman exec "$deluge" test -f /data/media/movies/xtest.mkv
	# The command the README gives, through the compose front end.
	out=$(compose_in "$dir" "$proj" exec -T axis sh -c 'echo x > /data/downloads/torrents/t.mkv && ln /data/downloads/torrents/t.mkv /data/media/movies/t.mkv && stat -c "%h %n" /data/media/movies/t.mkv' 2>&1)
	case $out in
	"2 /data/media/movies/t.mkv"*) pass "full: the README's hardlink command prints a link count of 2" ;;
	*) fail "full: the README's hardlink command prints a link count of 2 (got: $out)" ;;
	esac

	# The one data mount is writable as the example is written: adding it
	# writes its marker at once, as before.
	if add_first_disk full "http://127.0.0.1:$ports" "http://127.0.0.1:$ports" "$work/full.jar" /data write media/movies; then
		[ -f "$dir/data/.axis-volume" ] && pass "full: the marker is written when the writable disk is added" ||
			fail "full: the marker is written when the writable disk is added"
		page_has "http://127.0.0.1:$ports" "$work/full.jar" "/storage/volumes/$volume" "Its marker, .axis-volume" &&
			pass "full: the volume is recognised by its marker" || fail "full: the volume is recognised by its marker"
	fi

	if compose_in "$dir" "$proj" down -v > "$work/full-down.log" 2>&1; then
		pass "full: down -v"
	else
		fail "full: down -v"
	fi
	cd "$repo" || true
}

# ---------------------------------------------------------------------------
# Part: traefik

part_traefik() {
	local proj=$prefix-t dir=$work/traefik http_port https_port net client_ip forged_ip admin
	local axis proxy token jar page csrf code line out axis_ip network=$prefix-proxy volume=

	echo "traefik: deploy/examples/traefik"
	if [ -z "$compose" ]; then
		skip "traefik: no Compose front end (podman compose or docker compose)"
		return
	fi
	have_image "$traefik_image" || { skip "traefik: $traefik_image cannot be pulled"; return; }
	have_image "$client_image" || { skip "traefik: $client_image cannot be pulled"; return; }
	if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
		skip "traefik: no XDG_RUNTIME_DIR, so no Podman API socket"
		return
	fi
	# A socket file can outlive its unit, so ask systemd, not the file system.
	if ! systemctl --user is-active --quiet podman.socket 2>/dev/null; then
		if systemctl --user start podman.socket >/dev/null 2>&1 && [ -S "$XDG_RUNTIME_DIR/podman/podman.sock" ]; then
			started_socket=1
			say info "podman.socket started for this run; it is stopped again at the end"
		else
			skip "traefik: the Podman API socket is not listening and cannot be started (systemctl --user start podman.socket)"
			return
		fi
	fi
	http_port=$(free_port) && https_port=$(free_port) || { fail "traefik: no free port"; return; }
	net=10.89.$((100 + RANDOM % 100))
	client_ip=$net.77
	forged_ip=$net.78
	cp -R "$repo/deploy/examples/traefik" "$dir" || { fail "traefik: copy"; return; }
	cd "$dir" || return
	grep -q "^    image: $traefik_image\$" compose.yaml &&
		pass "traefik: the image this check pulled is the one the example names ($traefik_image)" ||
		fail "traefik: the image this check pulled is the one the example names ($traefik_image)"
	make_secrets
	mkdir -p media/disk1
	put_film media/disk1/Movies
	chmod -R a+rX media
	# What a reader edits: the socket (the comment in the file), the disk; and
	# for this run only: another subnet and network name, and a certificate
	# authority that is a local dead end, so that nothing leaves this machine.
	edit compose.yaml \
		-e 's|/var/run/docker.sock:/var/run/docker.sock:ro|${XDG_RUNTIME_DIR}/podman/podman.sock:/var/run/docker.sock:ro|' \
		-e "s|/path/to/disk1|$dir/media/disk1|" \
		-e "s|172\.30\.0\.|$net.|g" \
		-e "s|name: axis-proxy|name: $network|" \
		-e "s|--providers.docker.network=axis-proxy|--providers.docker.network=$network|" \
		-e 's|^\( *\)- --certificatesresolvers.letsencrypt.acme.email=.*$|&\n\1- --certificatesresolvers.letsencrypt.acme.caserver=https://127.0.0.1:9/dir\n\1- --log.level=DEBUG\n\1- --accesslog=true|'
	selinux_edit compose.yaml
	if [ "$(getenforce 2>/dev/null)" = Enforcing ]; then
		# The two commented lines the file offers for Traefik and the socket.
		edit compose.yaml -e 's/^    # security_opt:$/    security_opt:/' \
			-e 's/^    #   - label=disable$/      - label=disable/'
	fi
	printf 'AXIS_IMAGE=%s\nPUID=%s\nPGID=%s\nTRAEFIK_HTTP_PORT=%s\nTRAEFIK_HTTPS_PORT=%s\n' \
		"$image" "$axis_uid" "$axis_uid" "$http_port" "$https_port" > .env

	if compose_in "$dir" "$proj" up -d > "$work/traefik.log" 2>&1; then
		pass "traefik: up -d"
	else
		fail "traefik: up -d (see below)"
		tail -n 20 "$work/traefik.log" >&2
		compose_in "$dir" "$proj" down -v >/dev/null 2>&1
		cd "$repo" || true
		return
	fi
	axis=$(container_of "$proj" axis)
	proxy=$(container_of "$proj" traefik)
	if [ "$(podman inspect --format "{{(index .NetworkSettings.Networks \"$network\").IPAddress}}" "$proxy" 2>/dev/null)" = "$net.2" ]; then
		pass "traefik: Traefik has the fixed address $net.2 (AXIS_TRUSTED_PROXIES names the service, not this address)"
	else
		fail "traefik: Traefik has the fixed address $net.2 (AXIS_TRUSTED_PROXIES names the service, not this address)"
	fi
	local base=https://axis.example.com:$https_port
	local resolve=(--resolve "axis.example.com:$https_port:127.0.0.1")
	# The Compose labels have been read by Traefik when it routes the host name.
	if wait_for 240 body_is "$base/livez" '{"status":"alive"}' "${resolve[@]}"; then
		pass "traefik: a request for the router's host name reaches Axis through Traefik (the example's own labels)"
	else
		fail "traefik: a request for the router's host name reaches Axis through Traefik"
		podman logs --tail 4 "$proxy" 2>&1 | cut -c 1-300 >&2
	fi
	code=$(curl -sk --max-time 5 -o /dev/null -w '%{http_code}' "https://127.0.0.1:$https_port/livez")
	[ "$code" = 404 ] && pass "traefik: another host name is not routed (404)" ||
		fail "traefik: another host name is not routed (got $code)"
	curl -sk --max-time 5 -D - -o /dev/null "${resolve[@]}" "$base/livez" | grep -qi '^strict-transport-security: max-age=31536000' &&
		pass "traefik: the headers middleware from the labels sends HSTS" ||
		fail "traefik: the headers middleware from the labels sends HSTS"
	wait_for 120 health_is "$axis" healthy

	# First administrator by the setup token, as the README of the minimal
	# example says, through Traefik.
	# The README: `logs axis | grep "setup token"`; the token ends the line.
	token=$(podman logs "$axis" 2>&1 | grep 'setup token' | tail -n 1 | awk '{ print $NF }')
	admin=$(random_hex 12)
	jar=$work/traefik.jar
	if [ -z "$token" ]; then
		fail "traefik: the setup token is in the log"
		podman logs --tail 10 "$axis" >&2 2>&1
	else
		pass "traefik: the setup token is in the log"
		page=$(curl -sk --max-time 10 "${resolve[@]}" -c "$jar" -D "$jar.headers" "$base/setup")
		grep -qi '^set-cookie: __Host-axis_presession=.*; Secure' "$jar.headers" &&
			pass "traefik: through Traefik the pre-session cookie is __Host- and Secure" ||
			fail "traefik: through Traefik the pre-session cookie is __Host- and Secure"
		csrf=$(printf '%s' "$page" | grep -o '<input[^>]*name="_csrf"[^>]*>' | head -n 1 | sed -n 's/.* value="\([^"]*\)".*/\1/p')
		code=$(curl -sk --max-time 10 "${resolve[@]}" -b "$jar" -c "$jar" -o /dev/null -w '%{http_code}' \
			-H "Origin: $base" --data-urlencode "_csrf=$csrf" --data-urlencode "setup_token=$token" \
			--data-urlencode "username=admin" --data-urlencode "password=$admin" \
			--data-urlencode "password_confirm=$admin" "$base/setup")
		[ "$code" = 303 ] && pass "traefik: the first administrator is created through Traefik (setup token)" ||
			fail "traefik: the first administrator is created through Traefik (status $code)"
	fi
	if login "$base" "$base" admin "$admin" "$work/traefik-login.jar" "${resolve[@]}"; then
		pass "traefik: a request through Traefik logs in"
	else
		fail "traefik: a request through Traefik logs in"
	fi
	grep -qi '^set-cookie: __Host-axis_presession=.*; Secure' "$work/traefik-login.jar.headers" &&
		pass "traefik: the login page's cookie is Secure and __Host- prefixed" ||
		fail "traefik: the login page's cookie is Secure and __Host- prefixed"
	if grep -i '__Host-axis_' "$work/traefik-login.jar" | grep -q .; then
		# curl's jar has "#HttpOnly_" in front of the host for HttpOnly cookies;
		# every cookie in it must be __Host- prefixed.
		if grep -v '^$' "$work/traefik-login.jar" | grep -v '^# ' | awk -F'\t' 'NF >= 7 && $6 !~ /^__Host-/ { bad = 1 } END { exit bad }'; then
			pass "traefik: every cookie after login is __Host- prefixed"
		else
			fail "traefik: every cookie after login is __Host- prefixed"
		fi
	else
		fail "traefik: the session cookie after login is __Host- prefixed"
	fi
	line=$(podman logs "$axis" 2>&1 | grep 'audit: login: user=admin' | tail -n 1)
	say info "through the published port: $line"

	# The first disk, read-only as the example mounts it, added through
	# Traefik.
	curl_extra=("${resolve[@]}")
	add_first_disk traefik "$base" "$base" "$work/traefik-login.jar" /media/disk1 identify Movies
	curl_extra=()
	[ ! -e media/disk1/.axis-volume ] && pass "traefik: nothing is written to the read-only disk" ||
		fail "traefik: nothing is written to the read-only disk"

	# A client that is a container of its own on Traefik's network.
	out=$(podman run --rm --network "$network" --ip "$client_ip" --security-opt label=disable \
		-v "$repo/tests/check-examples-client.py:/client.py:ro" "$client_image" \
		python3 /client.py login "https://$net.2" axis.example.com https://axis.example.com admin "$admin" 2>&1)
	echo "$out" | sed 's/^/           client: /'
	case $out in
	*status=303*) pass "traefik: a client container logs in through Traefik" ;;
	*) fail "traefik: a client container logs in through Traefik" ;;
	esac
	case $out in
	*"cookie=__Host-"*" plain"* | *"cookie=axis_"*) fail "traefik: every cookie the client gets is Secure and __Host-" ;;
	*"cookie=__Host-"*) pass "traefik: every cookie the client gets is Secure and __Host-" ;;
	*) fail "traefik: every cookie the client gets is Secure and __Host-" ;;
	esac
	line=$(podman logs "$axis" 2>&1 | grep 'audit: login: user=admin' | tail -n 1)
	say info "$line"
	case $line in
	*"client=$client_ip"*) pass "traefik: the audit line shows the client's address ($client_ip), not Traefik's ($net.2)" ;;
	*) fail "traefik: the audit line shows the client's address ($client_ip), not Traefik's ($net.2)" ;;
	esac

	# The same kind of headers, straight to Axis from a container that is not
	# the proxy.
	axis_ip=$(podman inspect --format "{{(index .NetworkSettings.Networks \"$network\").IPAddress}}" "$axis")
	out=$(podman run --rm --network "$network" --ip "$forged_ip" --security-opt label=disable \
		-v "$repo/tests/check-examples-client.py:/client.py:ro" "$client_image" \
		python3 /client.py forged "http://$axis_ip:7800" 2>&1)
	echo "$out" | sed 's/^/           client: /'
	case $out in
	*"cookie=axis_presession plain"*) pass "traefik: forged X-Forwarded-Proto sent straight to Axis is ignored (cookie not Secure, no __Host-)" ;;
	*) fail "traefik: forged X-Forwarded-Proto sent straight to Axis is ignored" ;;
	esac
	line=$(podman logs "$axis" 2>&1 | grep 'worker API: a request from' | tail -n 1)
	say info "$line"
	case $line in
	*"from $forged_ip "*) pass "traefik: forged X-Forwarded-For sent straight to Axis is ignored (logged $forged_ip, not 203.0.113.9)" ;;
	*) fail "traefik: forged X-Forwarded-For sent straight to Axis is ignored" ;;
	esac

	# As published, the example has no portal (compose.portal.yaml is not
	# named): no portal runs, and the daemon has no portal listener.
	no_portal traefik "$(container_of "$proj" portal)" "$axis"

	# The request portal, added as the README's part "The request portal"
	# says, with the administrator's session from above.
	traefik_portal "$dir" "$proj" "$net" "$network" "$https_port" "$work/traefik-login.jar" "$base" "$admin" "${resolve[@]}"

	if compose_in "$dir" "$proj" -f compose.yaml -f compose.portal.yaml down -v > "$work/traefik-down.log" 2>&1; then
		pass "traefik: down -v"
	else
		fail "traefik: down -v"
	fi
	cd "$repo" || true
}

# no_portal PART PORTAL-CONTAINER AXIS-CONTAINER: the example as published
# runs no portal, and its daemon has no portal listener.
no_portal() {
	if [ -z "$2" ] && ! podman ps -a --format '{{.Names}}' | grep -q -e "^$prefix-.*portal" -e '^axis-xtest-.*portal' -e '^systemd-axis-xtest-.*portal'; then
		pass "$1: as published, no portal runs"
	else
		fail "$1: as published, no portal runs"
	fi
	if podman logs "$3" 2>&1 | grep -q 'portal API: listening'; then
		fail "$1: as published, the daemon has no portal listener"
	else
		pass "$1: as published, the daemon has no portal listener"
	fi
}

# reach_from CONTAINER HOST PORT: what a TCP connection to HOST:PORT from
# inside CONTAINER comes to (open, refused, unresolved or timeout), asked by
# the image's own python3 with the client script on its standard input: the
# portal's container has no mount to put a script in.
reach_from() {
	podman exec -i "$1" python3 - reach "$2" "$3" < "$repo/tests/check-portal-client.py" 2>/dev/null |
		sed -n 's/^reach=//p'
}

# inet6_of CONTAINER: the IPv6 addresses CONTAINER has that are not
# loopback's (the link-local ones among them), from the kernel's table, one a
# word.  Nothing when it has none.
inet6_of() {
	podman exec "$1" cat /proc/net/if_inet6 2>/dev/null |
		awk '$6 != "lo" { a = $1; gsub(/..../, "&:", a); sub(/:$/, "", a); print a }' | tr '\n' ' '
}

# portal_no_ipv6 PART PORTAL SCHEME PORT HOST ADMIN-PASSWORD TARGET AXIS WHOM:
# the portal's container is named by its IPv4 addresses (AXIS_PORTAL_PEERS,
# Traefik's router), so it must have no other kind.  An interface on a bridge
# has an IPv6 link-local address (fe80::/10) of the kernel's own making unless
# IPv6 is switched off in the container, which the examples do by sysctl.
# Asked from inside the portal's container: it has no IPv6 address but
# loopback's, and the administrator's right password, posted over IPv6 to
# each IPv6 address the container TARGET (WHOM, for the sentence) has, through
# each of the portal's interfaces, comes to no connection.
portal_no_ipv6() {
	local part=$1 portal=$2 scheme=$3 port=$4 host=$5 admin=$6 target=$7 axis=$8 whom=$9 targets out
	targets=$(inet6_of "$target")
	# shellcheck disable=SC2086
	out=$(podman exec -i "$portal" python3 - v6 "$scheme" "$port" "$host" admin "$admin" $targets \
		< "$repo/tests/check-portal-client.py" 2>&1)
	echo "$out" | sed 's/^/           portal: /'
	case $out in
	inet6=none*) pass "$part: the portal's container has no IPv6 address but loopback's (IPv6 is switched off in it)" ;;
	*) fail "$part: the portal's container has no IPv6 address but loopback's (IPv6 is switched off in it)" ;;
	esac
	if [ -z "$targets" ]; then
		say info "$part: $whom has no IPv6 address on this host, so there is none to try from the portal's container"
	elif echo "$out" | grep -q '^try=.*connect=open'; then
		fail "$part: from the portal's container, no IPv6 connection to $whom can be made ($targets)"
		podman logs "$axis" 2>&1 | grep 'audit: login' | tail -n 4 | cut -c 1-400 >&2
	elif echo "$out" | grep -q '^try=.*connect=failed'; then
		pass "$part: from the portal's container, no IPv6 connection to $whom can be made ($targets)"
	else
		fail "$part: from the portal's container, no IPv6 connection to $whom can be made (nothing was tried: $targets)"
	fi
}

# proxy_name_resolved CONTAINER: the daemon in CONTAINER has said that the
# name of its trusted proxy has an address (its own line, written when a
# lookup first finds one and whenever the addresses change).  Only the log
# since the container last started is read: after a restart in place, the
# line of the start before would answer for this one.
proxy_name_resolved() {
	local since
	since=$(podman inspect --format '{{.State.StartedAt.Format "2006-01-02T15:04:05Z07:00"}}' "$1" 2>/dev/null)
	podman logs ${since:+--since "$since"} "$1" 2>&1 | grep -q 'trusted proxy name traefik resolves to '
}

# portal_page NETWORK IP BASE: the portal's login page as a container of its
# own at IP on NETWORK gets it from Traefik at BASE (check-portal-client.py,
# "page"); portal_page_is ... WORD: whether that answer has WORD in it.
# (Each client container has a name of its own: one removed a moment ago can
# still hold its name when the next is made.)
portal_page() {
	podman run --rm --name "$prefix-client-$RANDOM$RANDOM" --network "$1" --ip "$2" --security-opt label=disable \
		-v "$repo/tests/check-portal-client.py:/client.py:ro" "$client_image" \
		python3 /client.py page "$3" requests.example.com /login 2>&1
}

portal_page_is() {
	portal_page "$1" "$2" "$3" | grep -qF -- "$4"
}

# gateway_tries PORTAL AXIS TRAEFIK ADMIN-PASSWORD NETWORK NET HOST:PORT...:
# the engine's login with the administrator's right password, from inside the
# portal's container, at https://HOST:PORT for each (the gateway addresses of
# the portal's networks with Traefik's published port, and the like), as the
# stack stands and after each of three restarts of Traefik.  A place that
# takes no connection is said and not asked further.  Prints what each came
# to, what Traefik's access log has as the client of each login it was asked
# for, and the daemon's lines about logins; asserts that no attempt opened a
# session and that the daemon took no login of the administrator's meanwhile.
gateway_tries() {
	local portal=$1 axis=$2 proxy=$3 admin=$4 network=$5 net=$6 round place out reach before after opened= answered=0
	shift 6
	before=$(podman logs "$axis" 2>&1 | grep -c 'audit: login: user=admin')
	for round in 0 1 2 3; do
		if [ "$round" -gt 0 ]; then
			podman restart -t 5 "$proxy" >/dev/null 2>&1
			if ! wait_for 120 portal_page_is "$network" "$net.82" "https://$net.2" status=200; then
				fail "traefik: Traefik routes again after its restart number $round"
				return
			fi
		fi
		for place in "$@"; do
			reach=$(reach_from "$portal" "${place%:*}" "${place##*:}")
			if [ "$reach" != open ]; then
				say info "start $round: from the portal's container, $place takes no connection (${reach:-no answer})"
				continue
			fi
			out=$(podman exec -i "$portal" python3 - engine-login "https://$place" axis.example.com admin "$admin" \
				< "$repo/tests/check-portal-client.py" 2>&1 | tr '\n' ' ')
			say info "start $round: from the portal's container, the engine's login at $place: $out"
			answered=$((answered + 1))
			case $out in
			*login=303* | *session=yes*) opened="$opened $place(start $round)" ;;
			esac
		done
	done
	say info "Traefik's access log, logins by client, status and router: $(podman logs "$proxy" 2>&1 |
		awk '$7 == "/login" { print $1, $6 "\"", $9, $(NF - 2) }' | sort | uniq -c | tr -s ' ' | tr '\n' ';')"
	podman logs "$axis" 2>&1 | grep 'audit: login' | tail -n 6 | cut -c 1-260 | sed 's/^/           axis: /'
	after=$(podman logs "$axis" 2>&1 | grep -c 'audit: login: user=admin')
	if [ "$answered" = 0 ]; then
		answered="none of the places took a connection"
	else
		answered="$answered tries were answered, each with a refusal; the others took no connection"
	fi
	if [ -z "$opened" ] && [ "$after" = "$before" ]; then
		pass "traefik: from the portal's container, by the gateway addresses of its networks and Traefik's published port, the administrator's right password opens no session, over four starts of Traefik ($answered)"
	else
		fail "traefik: from the portal's container, by the gateway addresses of its networks and Traefik's published port, the administrator's right password opens no session (opened at:${opened:- none}; logins of the administrator's taken: $before before, $after after)"
	fi
}

# portal_hardened PART CONTAINER: the portal's container as the examples run
# it: not root, no capability, no new privileges, a read-only root, no
# volume, and nothing of the daemon's in its environment.
portal_hardened() {
	local part=$1 c=$2 uid mounts env
	uid=$(podman exec "$c" cat /proc/1/status | sed -n 's/^Uid:[[:space:]]*\([0-9]*\).*/\1/p')
	[ -n "$uid" ] && [ "$uid" != 0 ] && pass "$part: the portal runs as uid $uid, not root" ||
		fail "$part: the portal runs as uid ${uid:-?}, not root"
	[ "$(podman exec "$c" cat /proc/1/status | sed -n 's/^CapEff:[[:space:]]*//p')" = 0000000000000000 ] &&
		pass "$part: the portal has no capabilities" || fail "$part: the portal has no capabilities"
	[ "$(podman exec "$c" cat /proc/1/status | sed -n 's/^NoNewPrivs:[[:space:]]*//p')" = 1 ] &&
		pass "$part: the portal cannot gain privileges" || fail "$part: the portal cannot gain privileges"
	[ "$(podman inspect --format '{{.HostConfig.ReadonlyRootfs}}' "$c")" = true ] &&
		pass "$part: the portal's root filesystem is read-only" || fail "$part: the portal's root filesystem is read-only"
	check "$part: a write to the portal's root filesystem fails" sh -c "! podman exec '$c' touch /x 2>/dev/null"
	# Its mounts: the token, and nothing else that is a volume or a bind
	# mount of the example's (the runtime's own files for names and hosts,
	# and Podman's tmpfs of a read-only container, are not the example's).
	mounts=$(podman inspect --format '{{range .Mounts}}{{.Type}}:{{.Destination}} {{end}}' "$c" |
		tr ' ' '\n' | grep -v -e '^$' -e 'axis_portal_token$' -e 'portal-token$' | tr '\n' ' ')
	[ -z "$mounts" ] && pass "$part: the portal has no volume and no mount but its token" ||
		fail "$part: the portal has no volume and no mount but its token (found: $mounts)"
	env=$(podman inspect --format '{{range .Config.Env}}{{println .}}{{end}}' "$c" |
		grep -i -E 'DATABASE|POSTGRES|PASSWORD|AXIS_ADMIN|AXIS_WORKER|AXIS_METRICS' | cut -d= -f1 | tr '\n' ' ')
	[ -z "$env" ] && pass "$part: the portal's environment has no database address and no other secret" ||
		fail "$part: the portal's environment has no database address and no other secret (found: $env)"
}

# make_portal_token BASE ORIGIN JAR: the administrator makes a portal token
# on the page "Request portal"; prints it (the page shows it once).
make_portal_token() {
	local out=$work/portal-token-page code
	code=$(web_post "$1" "$2" "$3" /admin/portal/tokens "$out")
	[ "$code" = 200 ] || return 1
	grep -o '<pre class="worker-token"><code>[^<]*' "$out.body" | head -n 1 | sed 's/.*<code>//'
}

# traefik_portal DIR PROJECT NET NETWORK HTTPS-PORT JAR BASE ADMIN-PASSWORD RESOLVE...
# deploy/examples/traefik with compose.portal.yaml: the daemon gets its
# portal listener, the administrator makes a token and a requester, the
# portal starts hardened behind Traefik; a requester logs in over HTTPS
# through Traefik and asks for a movie, which then waits on the daemon's
# Requests page; the engine's pages are not behind the portal's host name;
# and from the portal's container neither the database nor the daemon's main
# listener can be reached.
traefik_portal() {
	local dir=$1 proj=$2 net=$3 network=$4 https_port=$5 jar=$6 base=$7 admin=$8
	shift 8
	local -a resolve=("$@")
	# (Subnets of this part's own: the Quadlet part's is 10.89.60 to 10.89.89.)
	local pnet=10.89.$((200 + RANDOM % 50)) xnet=10.89.$((20 + RANDOM % 30))
	local portal axis token requester title out code line pg_ip audits
	local pbase=https://requests.example.com:$https_port
	local -a presolve=(--resolve "requests.example.com:$https_port:127.0.0.1")
	local -a both=(-f compose.yaml -f compose.portal.yaml)

	say info "traefik: the request portal (compose.portal.yaml)"
	cd "$dir" || return
	# For this run only: the test's subnets and network names; and one more
	# address among the portal's, that of a container the check runs on
	# Traefik's first network, to see the daemon's own rule on a login that
	# Traefik does carry.
	edit compose.portal.yaml \
		-e "s|172\.30\.0\.|$net.|g" \
		-e "s|172\.30\.1\.|$pnet.|g" \
		-e "s|172\.30\.2\.|$xnet.|g" \
		-e "s|^\( *AXIS_PORTAL_PEERS: .*\)\$|\1, $net.81|" \
		-e "s|name: axis-portal-proxy|name: $prefix-portal-proxy|" \
		-e "s|name: axis-portal\$|name: $prefix-portal|" \
		-e "s|traefik.docker.network=axis-portal-proxy|traefik.docker.network=$prefix-portal-proxy|"
	selinux_edit compose.portal.yaml

	# README, step 2: the daemon with its portal listener; the portal waits
	# for its token.  The file is made with its mode, before anything is in it.
	install -m 600 /dev/null secrets/axis_portal_token
	if compose_in "$dir" "$proj" "${both[@]}" up -d axis > "$work/traefik-portal-axis.log" 2>&1; then
		pass "traefik: up -d axis with compose.portal.yaml"
	else
		fail "traefik: up -d axis with compose.portal.yaml (see below)"
		tail -n 20 "$work/traefik-portal-axis.log" >&2
		return
	fi
	axis=$(container_of "$proj" axis)
	if wait_for 240 body_is "$base/readyz" '{"status":"ready"}' "${resolve[@]}"; then
		pass "traefik: Axis is ready again, through Traefik, with its main listener on its address on Traefik's network"
	else
		fail "traefik: Axis is ready again, through Traefik"
		podman logs --tail 20 "$axis" >&2 2>&1
		return
	fi
	podman logs "$axis" 2>&1 | grep -q "portal API: listening on $pnet.2:7801" &&
		pass "traefik: the daemon answers the portal API on $pnet.2:7801, its address on the portal's network" ||
		fail "traefik: the daemon answers the portal API on $pnet.2:7801"
	wait_for 120 health_is "$axis" healthy && pass "traefik: axis is healthy (its health check asks the main listener's own address)" ||
		fail "traefik: axis is healthy with the main listener on one address"

	# Steps 3 to 5: the token, its file, a requester.  The administrator's
	# session is the one from before: sessions are in the database.
	curl_extra=("${resolve[@]}")
	# The daemon was started again, and believes Traefik once it has looked
	# its name up: until then a request through Traefik is plain http to the
	# daemon, the session's Secure cookie is not read, and (with a portal
	# configured) a login that Traefik forwards is refused.  The daemon says
	# when it has the name's address; that line is waited for, and then the
	# session must be read: both are asserted, so that a failure here says
	# which of the two it was.
	#
	# The session must be read six times on end, a second apart.  Two runs by
	# hand failed at the token's making, in the second after the daemon had
	# started, with Traefik's own 404 for a host name it had routed a moment
	# before (the page before it had answered).  Traefik's debug log of a
	# later run shows why: around the restart its provider makes
	# configurations without the router of Axis (the old container's end,
	# then the new one "unhealthy or starting"), and one with it once the
	# first health check has passed, all within two seconds; Traefik applies
	# what its provider reports at most once in two seconds, the newest only.
	# Meanwhile the new container, at the old one's address, answers through
	# the old route.  When an interval ends between those reports, the route
	# is gone until the next one.  Six seconds without a break is three such
	# intervals.  (Traefik logs at debug level in this check, so that a run
	# that fails here prints what its provider did.)
	if wait_for 60 proxy_name_resolved "$axis"; then
		pass "traefik: the daemon, started again, has looked up the name of Traefik ($(podman logs "$axis" 2>&1 |
			sed -n 's/.*trusted proxy name traefik resolves to \([0-9a-f.:, ]*\).*/\1/p' | tail -n 1))"
	else
		fail "traefik: the daemon, started again, has looked up the name of Traefik"
		podman logs "$axis" 2>&1 | grep -i 'trusted prox' | tail -n 6 | cut -c 1-300 >&2
	fi
	if steady 6 60 page_has "$base" "$jar" /storage 'name="_csrf"'; then
		pass "traefik: the administrator's session from before is read again through Traefik, six times on end"
	else
		fail "traefik: the administrator's session from before is read again through Traefik, six times on end (see below)"
		echo "           /storage: status $(curl -sk --max-time 10 "${curl_extra[@]}" -b "$jar" -o /dev/null -w '%{http_code} -> %{redirect_url}' "$base/storage")" >&2
		echo "           cookies in the jar: $(grep -v '^# ' "$jar" | awk -F'\t' 'NF >= 7 { printf "%s ", $6 }')" >&2
		podman logs "$axis" 2>&1 | grep -i -e 'trusted prox' -e 'forward' | tail -n 6 | cut -c 1-300 >&2
		podman logs --tail 6 "$(container_of "$proj" traefik)" 2>&1 | cut -c 1-300 >&2
	fi
	token=$(make_portal_token "$base" "$base" "$jar")
	if [ -n "$token" ]; then
		pass "traefik: the administrator makes a portal token on the page Request portal"
	else
		fail "traefik: the administrator makes a portal token on the page Request portal"
		head -n 12 "$work/portal-token-page" 2>/dev/null | cut -c 1-200 >&2
		podman logs --tail 12 "$axis" 2>&1 | cut -c 1-400 >&2
		podman logs "$(container_of "$proj" traefik)" 2>&1 | grep -i -e 'filter' -e 'configuration received' -e 'skipping' |
			tail -n 30 | cut -c 1-400 >&2
		curl_extra=()
		return
	fi
	(umask 077 && printf '%s' "$token" > secrets/axis_portal_token)
	[ "$(stat -c %a secrets/axis_portal_token)" = 600 ] &&
		pass "traefik: the token's file is readable by its owner alone, as the README makes it" ||
		fail "traefik: the token's file is readable by its owner alone (mode $(stat -c %a secrets/axis_portal_token))"
	podman unshare chown "$axis_uid:$axis_uid" secrets/axis_portal_token
	podman logs "$axis" 2>&1 | grep -q "portal's addresses: $pnet.3, $xnet.3, $pnet.1, $xnet.1" &&
		pass "traefik: the daemon was told the portal's addresses on both of the portal's networks, and the host's on each ($pnet.3, $xnet.3, $pnet.1, $xnet.1)" ||
		fail "traefik: the daemon was told the portal's addresses on both of the portal's networks, and the host's on each"
	requester=$(random_hex 12)
	code=$(web_post "$base" "$base" "$jar" /admin/users "$work/traefik-requester" \
		"username=robin" "password=$requester" "role=requester")
	[ "$code" = 303 ] && pass "traefik: the administrator creates a requester account" ||
		fail "traefik: the administrator creates a requester account (status $code)"
	curl_extra=()
	# A title to ask for: this stack has no details provider, so the record a
	# search would store is written as the daemon stores one.
	title=$(compose_in "$dir" "$proj" "${both[@]}" exec -T postgres psql --username axis --dbname axis --tuples-only \
		--no-align --quiet --command "INSERT INTO meta_title (kind, title, year, overview, provider, provider_id, adult, detail_fetched_at) VALUES ('film', 'Glass Orchard', 2031, 'A made-up film.', 'tmdb', '990001', false, now()) RETURNING id" 2>/dev/null |
		head -n 1 | tr -d '[:space:]')
	case $title in
	[0-9]*) pass "traefik: a title to ask for is in the daemon's store (number $title)" ;;
	*) fail "traefik: a title to ask for is in the daemon's store"; return ;;
	esac

	# Step 6: everything.
	if compose_in "$dir" "$proj" "${both[@]}" up -d > "$work/traefik-portal-up.log" 2>&1; then
		pass "traefik: up -d with compose.portal.yaml"
	else
		fail "traefik: up -d with compose.portal.yaml (see below)"
		tail -n 20 "$work/traefik-portal-up.log" >&2
		return
	fi
	portal=$(container_of "$proj" portal)
	axis=$(container_of "$proj" axis)
	if [ -n "$portal" ] && wait_for 120 health_is "$portal" healthy; then
		pass "traefik: the portal is healthy (axis-portal --healthcheck)"
	else
		fail "traefik: the portal is healthy"
		podman logs --tail 20 "$portal" >&2 2>&1
		return
	fi
	podman logs "$portal" 2>&1 | grep -q 'has not been reviewed for exposure to the internet' &&
		pass "traefik: the portal says at start that it is not reviewed for the internet" ||
		fail "traefik: the portal says at start that it is not reviewed for the internet"
	portal_hardened traefik "$portal"

	# Step 7: the login page at the portal's own host name, over HTTPS, as a
	# visitor's browser gets it: from a container of its own on Traefik's
	# first network.  (Not through the port this machine publishes: rootless
	# Podman delivers a published port's traffic to Traefik from one of
	# Traefik's own addresses, picked at each start.  When that is its address
	# on the portal's network, the portal is told of a visitor who is its own
	# proxy and rightly answers 503.  What the published port gave is printed
	# below, and not judged.)
	if wait_for 120 portal_page_is "$network" "$net.82" "https://$net.2" status=200; then
		pass "traefik: the portal's login page answers at its own host name through Traefik, over https"
	else
		fail "traefik: the portal's login page answers at its own host name through Traefik, over https"
		podman logs --tail 10 "$portal" >&2 2>&1
	fi
	out=$(portal_page "$network" "$net.82" "https://$net.2")
	case $out in
	*"hsts=max-age=31536000"*) pass "traefik: the portal's page has HSTS" ;;
	*) fail "traefik: the portal's page has HSTS" ;;
	esac
	case $out in
	*"cookie=__Host-axis_portal_form secure"*"password-field=yes"*)
		pass "traefik: it is the portal's own login form, with a Secure, __Host- cookie (Traefik is believed)" ;;
	*) fail "traefik: it is the portal's own login form, with a Secure, __Host- cookie ($(echo "$out" | tr '\n' ' '))" ;;
	esac
	code=$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' "${presolve[@]}" "$pbase/login")
	say info "through the port this machine publishes, the portal's login page is answered $code (200, or 503 under rootless Podman: see above)"
	code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${http_port:-0}/login" \
		-H 'Host: requests.example.com' 2>/dev/null)
	say info "plain http to the portal's host name is answered $code by Traefik (a redirect)"

	# The engine is not behind the portal's host name.
	for line in /setup /admin/users /api/v1/portal/hello /api/v1/worker/claim /readyz /storage; do
		code=$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' "${presolve[@]}" "$pbase$line")
		[ "$code" = 404 ] && pass "traefik: $line at the portal's host name is 404" ||
			fail "traefik: $line at the portal's host name is 404 (got $code)"
	done

	# A requester, from a container of its own on Traefik's network: logs
	# in, asks for the movie, sees it among My requests.
	out=$(podman run --rm --network "$network" --ip "$net.79" --security-opt label=disable \
		-v "$repo/tests/check-portal-client.py:/client.py:ro" "$client_image" \
		python3 /client.py ask "https://$net.2" requests.example.com robin "$requester" "$title" 2>&1)
	echo "$out" | sed 's/^/           client: /'
	case $out in
	*login=303*) pass "traefik: a requester logs in at the portal through Traefik, over https" ;;
	*) fail "traefik: a requester logs in at the portal through Traefik, over https" ;;
	esac
	case $out in
	*"cookie=__Host-axis_portal_session secure"*) pass "traefik: the portal's session cookie is Secure and __Host-" ;;
	*) fail "traefik: the portal's session cookie is Secure and __Host-" ;;
	esac
	case $out in
	*"ask=200 made"*"requests=200 listed"*) pass "traefik: the requester asks for a movie and sees it among My requests" ;;
	*) fail "traefik: the requester asks for a movie and sees it among My requests" ;;
	esac
	line=$(podman logs "$axis" 2>&1 | grep 'audit: portal login: user=robin' | tail -n 1)
	say info "$line"
	case $line in
	*"reported_client=$net.79"*) pass "traefik: the daemon is told the client's address ($net.79), which the portal took from Traefik" ;;
	*) fail "traefik: the daemon is told the client's address ($net.79)" ;;
	esac
	[ "$(compose_in "$dir" "$proj" "${both[@]}" exec -T postgres psql --username axis --dbname axis --tuples-only --no-align \
		--command "SELECT count(*) FROM request WHERE via = 'portal' AND decision = 'waiting' AND title_id = $title" 2>/dev/null |
		tr -d '[:space:]')" = 1 ] && pass "traefik: the request is in the database, waiting" ||
		fail "traefik: the request is in the database, waiting"
	curl_extra=("${resolve[@]}")
	page_has "$base" "$jar" /requests "Glass Orchard" &&
		pass "traefik: the request waits on the daemon's Requests page" ||
		fail "traefik: the request waits on the daemon's Requests page"
	curl_extra=()

	# What the portal's container can reach: the daemon's portal port, and
	# nothing else of the stack.
	[ "$(reach_from "$portal" axis 7801)" = open ] && pass "traefik: from the portal, the daemon's portal port is open" ||
		fail "traefik: from the portal, the daemon's portal port is open"
	out=$(reach_from "$portal" axis 7800)
	[ -n "$out" ] && [ "$out" != open ] && pass "traefik: from the portal, the daemon's main port is not reachable by its name ($out)" ||
		fail "traefik: from the portal, the daemon's main port is not reachable by its name (${out:-no answer})"
	out=$(reach_from "$portal" "$net.3" 7800)
	[ -n "$out" ] && [ "$out" != open ] && pass "traefik: nor at the address the main listener has on Traefik's network ($out)" ||
		fail "traefik: nor at the address the main listener has on Traefik's network (${out:-no answer})"
	# The portal's fixed addresses, which the daemon and Traefik were told.
	[ "$(podman inspect --format "{{(index .NetworkSettings.Networks \"$prefix-portal\").IPAddress}} {{(index .NetworkSettings.Networks \"$prefix-portal-proxy\").IPAddress}}" "$portal" 2>/dev/null)" = "$pnet.3 $xnet.3" ] &&
		pass "traefik: the portal has its fixed address on each of its two networks ($pnet.3, $xnet.3)" ||
		fail "traefik: the portal has its fixed address on each of its two networks ($pnet.3, $xnet.3)"
	# And the host's own address on each of them, which the file names too.
	out="$(podman network inspect --format '{{range .Subnets}}{{.Gateway}}{{end}}' "$prefix-portal" 2>/dev/null) $(podman network inspect --format '{{range .Subnets}}{{.Gateway}}{{end}}' "$prefix-portal-proxy" 2>/dev/null)"
	[ "$out" = "$pnet.1 $xnet.1" ] &&
		pass "traefik: the host's address on each of the portal's two networks is the one the file names ($pnet.1, $xnet.1)" ||
		fail "traefik: the host's address on each of the portal's two networks is the one the file names ($pnet.1, $xnet.1; found: $out)"
	# The way round: Traefik shares a network with the portal, and routes to
	# Axis by host name.  First that the question is a real one: the same
	# client, from a container on Traefik's other network, does log in with
	# the administrator's password.  (The daemon believes Traefik by now: if
	# the last `up -d` started it again, its log has the line again.)
	wait_for 60 proxy_name_resolved "$axis" && pass "traefik: (the daemon has the address of Traefik's name)" ||
		fail "traefik: (the daemon has the address of Traefik's name)"
	out=$(podman run --rm --name "$prefix-client-$RANDOM$RANDOM" --network "$network" --ip "$net.80" --security-opt label=disable \
		-v "$repo/tests/check-portal-client.py:/client.py:ro" "$client_image" \
		python3 /client.py engine-login "https://$net.2" axis.example.com admin "$admin" 2>&1)
	case $out in
	*form=200*login=303*session=yes*) pass "traefik: (from another container, the administrator's password opens a session through Traefik)" ;;
	*) fail "traefik: (from another container, the administrator's password opens a session through Traefik): $(echo "$out" | tr '\n' ' ')" ;;
	esac
	# From the portal's own container, the same request to Traefik, with the
	# engine's host name and the administrator's right password.  The daemon
	# writes an audit line for every login it takes and for every wrong
	# password, under whatever address, and for the first login in a minute
	# that it refuses by the portal's addresses (later ones in that minute
	# are counted in the next such line, not written).  Its lines about
	# logins are counted before and after: a login that was taken shows.
	audits=$(podman logs "$axis" 2>&1 | grep -c 'audit: login')
	[ "$(reach_from "$portal" "$xnet.2" 443)" = open ] &&
		pass "traefik: (the portal's container can open a connection to Traefik)" ||
		fail "traefik: (the portal's container can open a connection to Traefik)"
	out=$(podman exec -i "$portal" python3 - engine-login "https://$xnet.2" axis.example.com admin "$admin" \
		< "$repo/tests/check-portal-client.py" 2>&1)
	echo "$out" | sed 's/^/           portal: /'
	case $out in
	*form=403*) pass "traefik: from the portal's container, Traefik refuses the engine's host name (403)" ;;
	*) fail "traefik: from the portal's container, Traefik refuses the engine's host name" ;;
	esac
	case $out in
	*login=303* | *session=yes*) fail "traefik: from the portal's container, the administrator's right password opens no session through Traefik" ;;
	*login=*session=no*) pass "traefik: from the portal's container, the administrator's right password opens no session through Traefik" ;;
	*) fail "traefik: from the portal's container, the administrator's right password opens no session through Traefik (no answer)" ;;
	esac
	out=$(podman exec -i "$portal" python3 - paths "https://$xnet.2" axis.example.com /setup /api/v1/worker/claim /livez \
		< "$repo/tests/check-portal-client.py" 2>&1 | tr '\n' ' ')
	case $out in
	"/setup 403 /api/v1/worker/claim 403 /livez 403 ") pass "traefik: nor does it reach the setup page, the worker API or a probe that way (403)" ;;
	*) fail "traefik: nor does it reach the setup page, the worker API or a probe that way (got: $out)" ;;
	esac
	# The same over IPv6, by which the portal's container would be neither
	# $xnet.3 to Traefik nor a listed address to Axis.
	portal_no_ipv6 traefik "$portal" https 443 axis.example.com "$admin" "$(container_of "$proj" traefik)" "$axis" Traefik
	# What this proves: no login the portal's container sent, over IPv4 or
	# IPv6, was taken by the daemon under any address, and none was the first
	# it refused.  (Not that no request of another kind reached it: the daemon
	# does not log those; the 403 answers above are Traefik's.)
	line=$(podman logs "$axis" 2>&1 | grep -c 'audit: login')
	[ "$line" = "$audits" ] &&
		pass "traefik: the daemon took no login, and first refused none, while the portal's container tried (its lines about logins: $audits before, $line after)" ||
		fail "traefik: the daemon took no login, and first refused none, while the portal's container tried (its lines about logins: $audits before, $line after)"
	# The daemon's own rule, should a login from one of the portal's addresses
	# reach it through Traefik after all: a container at an address this check
	# listed among the portal's ($net.81), through Traefik, with the right
	# password.  Traefik carries it; Axis refuses it, and says why.
	out=$(podman run --rm --name "$prefix-client-$RANDOM$RANDOM" --network "$network" --ip "$net.81" --security-opt label=disable \
		-v "$repo/tests/check-portal-client.py:/client.py:ro" "$client_image" \
		python3 /client.py engine-login "https://$net.2" axis.example.com admin "$admin" 2>&1 | tr '\n' ' ')
	case $out in
	*form=200*login=401*session=no*) pass "traefik: a login that Traefik forwards for an address listed as a portal's is refused by Axis as a wrong password is ($out)" ;;
	*) fail "traefik: a login that Traefik forwards for an address listed as a portal's is refused by Axis ($out)" ;;
	esac
	podman logs "$axis" 2>&1 | grep -q "audit: login refused: the client $net.81, as a trusted proxy names it, is one of the request portal's addresses" &&
		pass "traefik: and the daemon's log says why, naming the address and the setting" ||
		fail "traefik: and the daemon's log says why, naming the address and the setting"
	# The way round by the host.  An internal network has no way out, but the
	# host's own end of its bridge (the gateway address, .1) is on it, and
	# Traefik's published port answers on every address of the host.  A
	# connection from the portal's container to a gateway address and the
	# published port is sent on to Traefik by the runtime, which may rewrite
	# where it comes from to that gateway address: Traefik then sees a client
	# that is not the portal's address.  So the gateway addresses of the
	# portal's two networks are among those Traefik refuses and the daemon
	# takes no login from (compose.portal.yaml).  Tried here at each gateway,
	# on the published port and on 443, as the stack stands and after each of
	# three restarts of Traefik (the runtime writes its forwarding rules again
	# at each start, and their order decides which network's rule is taken).
	# Asserted: no session, and no login of the administrator's taken.
	gateway_tries "$portal" "$axis" "$(container_of "$proj" traefik)" "$admin" "$network" "$net" \
		"$xnet.1:$https_port" "$pnet.1:$https_port" "$xnet.1:443" "$pnet.1:443" "host.containers.internal:$https_port"
	out=$(reach_from "$portal" postgres 5432)
	[ "$out" = unresolved ] && pass "traefik: from the portal, the name postgres does not resolve" ||
		fail "traefik: from the portal, the name postgres does not resolve (${out:-no answer})"
	# Both of its networks are internal: it has no way out at all, so the
	# addresses of the host's other networks are as far as the internet is.
	if podman exec "$portal" cat /proc/net/route 2>/dev/null | awk 'NR > 1 && $2 == "00000000" { found = 1 } END { exit !found }'; then
		fail "traefik: the portal's container has no default route (its networks are internal)"
	else
		pass "traefik: the portal's container has no default route (its networks are internal)"
	fi
	pg_ip=$(podman inspect --format '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}' "$(container_of "$proj" postgres)" |
		tr ' ' '\n' | grep -v '^$' | head -n 1)
	out=$(reach_from "$portal" "$pg_ip" 5432)
	[ -n "$out" ] && [ "$out" != open ] && pass "traefik: nor is PostgreSQL reachable by its address ($pg_ip: $out)" ||
		fail "traefik: nor is PostgreSQL reachable by its address ($pg_ip: ${out:-no answer})"
	case $(podman logs "$portal" "$axis" 2>&1) in
	*"$token"*) fail "traefik: the portal's token is in no log" ;;
	*) pass "traefik: the portal's token is in no log" ;;
	esac
	case $(podman inspect "$portal" "$axis" 2>&1) in
	*"$token"*) fail "traefik: the portal's token is not in the containers' configuration" ;;
	*) pass "traefik: the portal's token is not in the containers' configuration" ;;
	esac

	# Last: Traefik's refusal of the portal's address does not depend on the
	# portal's container.  Traefik drops the routers of a container that is
	# stopped or not healthy, and whoever holds the portal's container decides
	# that.  The portal is stopped; once Traefik has dropped the portal's own
	# router (its host name is 404), a container at the portal's address on
	# the network the portal shares with Traefik asks for the engine's host
	# name with the administrator's right password, and is still refused.
	podman stop -t 5 "$portal" >/dev/null 2>&1
	if wait_for 90 portal_page_is "$network" "$net.82" "https://$net.2" status=404; then
		pass "traefik: with the portal's container stopped, Traefik has dropped the portal's own router (404 at its host name)"
	else
		fail "traefik: with the portal's container stopped, Traefik has dropped the portal's own router ($(portal_page "$network" "$net.82" "https://$net.2" | head -n 1))"
	fi
	out=$(podman run --rm --name "$prefix-client-$RANDOM$RANDOM" --network "$prefix-portal-proxy" --ip "$xnet.3" \
		--security-opt label=disable -v "$repo/tests/check-portal-client.py:/client.py:ro" "$client_image" \
		python3 /client.py engine-login "https://$xnet.2" axis.example.com admin "$admin" 2>&1 | tr '\n' ' ')
	case $out in
	*form=403*login=403*session=no*) pass "traefik: and still refuses the portal's address the engine's host name and the administrator's password ($out)" ;;
	*) fail "traefik: and still refuses the portal's address the engine's host name and the administrator's password ($out)" ;;
	esac
}

# ---------------------------------------------------------------------------
# Part: quadlet

part_quadlet() {
	local src=$repo/deploy/examples/quadlet dir=$work/quadlet port admin f name state pw volume=
	local quadlet=/usr/libexec/podman/quadlet axis_unit=axis-xtest-axis.service

	echo "quadlet: deploy/examples/quadlet"
	# This part's names are axis-xtest-* whatever the prefix is, and the
	# removal at the end goes by the prefix: with another prefix its network,
	# volumes and secrets would be left behind.
	if [ "$prefix" != axis-xtest ]; then
		skip "quadlet: its names are axis-xtest-*, which the prefix \"$prefix\" would not remove; run it without AXIS_XTEST_PREFIX"
		return
	fi
	[ -x "$quadlet" ] || { skip "quadlet: $quadlet is not installed (Podman 5 with Quadlet)"; return; }
	port=$(free_port) || { fail "quadlet: no free port"; return; }
	mkdir -p "$dir"
	for f in axis.network axis-db.volume axis-data.volume axis-postgres.container axis.container; do
		cp "$src/$f" "$dir/$f"
	done
	admin=$(random_hex 12)
	mkdir -p "$work/quadlet-media"
	echo marker > "$work/quadlet-media/marker.txt"
	put_film "$work/quadlet-media/Movies"
	chmod -R a+rX "$work/quadlet-media"
	# What a reader edits, and the prefix on every name so that nothing here
	# can be mistaken for a real installation.
	edit "$dir/axis.container" \
		-e 's|^Image=ghcr.io/averagenative/axis:0.1.0-rc2$|Image='"$image"'|' \
		-e 's|^PublishPort=127.0.0.1:7800:7800$|PublishPort=127.0.0.1:'"$port"':7800|' \
		-e 's|/path/to/disk1|'"$work"'/quadlet-media|' \
		-e 's|^# \(SecurityLabelDisable=true\)$|\1|' \
		-e 's|^# \(Secret=axis_admin_password.*\)$|\1|' \
		-e 's|^# \(Environment=AXIS_ADMIN_USERNAME=.*\)$|\1|'
	for f in axis.container axis-postgres.container; do
		edit "$dir/$f" \
			-e 's|axis-postgres\.service|axis-xtest-axis-postgres.service|g' \
			-e 's|axis-db\.volume|axis-xtest-axis-db.volume|g' \
			-e 's|axis-data\.volume|axis-xtest-axis-data.volume|g' \
			-e 's|axis\.network|axis-xtest-axis.network|g' \
			-e 's|^ContainerName=axis-postgres$|ContainerName=axis-xtest-postgres|' \
			-e 's|^ContainerName=axis$|ContainerName=axis-xtest-axis|' \
			-e 's|^Secret=axis_\([a-z_]*\),|Secret=axis-xtest-\1,|'
	done
	edit "$dir/axis.network" -e 's|^NetworkName=axis$|NetworkName=axis-xtest-net|'
	edit "$dir/axis-db.volume" -e 's|^VolumeName=axis-db$|VolumeName=axis-xtest-db|'
	edit "$dir/axis-data.volume" -e 's|^VolumeName=axis-data$|VolumeName=axis-xtest-data|'
	for f in axis.network axis-db.volume axis-data.volume axis-postgres.container axis.container; do
		mv "$dir/$f" "$dir/axis-xtest-$f"
	done
	# The Secret= lines name axis-xtest-db_password, and so on: make them one
	# kind of name.
	for f in "$dir"/axis-xtest-*.container; do
		edit "$f" -e 's|^Secret=axis-xtest-\([a-z]*\)_\([a-z_]*\),|Secret=axis-xtest-\1-\2,|'
	done

	state=$(systemctl --user is-system-running 2>/dev/null)
	if [ "$state" != running ] && [ "$state" != degraded ]; then
		skip "quadlet: no usable user systemd (systemctl --user is-system-running); the generator's dry run only"
		if QUADLET_UNIT_DIRS="$dir" "$quadlet" -dryrun -user > "$work/quadlet-dryrun.out" 2>&1 &&
			grep -q '^\[Service\]' "$work/quadlet-dryrun.out"; then
			pass "quadlet: the generator's dry run accepts all five files"
		else
			fail "quadlet: the generator's dry run accepts all five files"
		fi
		return
	fi

	# Units an earlier run that was killed left behind would keep this run's
	# network from being made.
	systemctl --user stop 'axis-xtest-*' >/dev/null 2>&1
	systemctl --user reset-failed 'axis-xtest-*' >/dev/null 2>&1

	# README step 1 (the names carry the prefix).
	pw=$(openssl rand -hex 24)
	printf '%s' "$pw" | podman secret create axis-xtest-db-password - >/dev/null
	printf 'postgresql://axis:%s@postgres:5432/axis' "$pw" | podman secret create axis-xtest-database-url - >/dev/null
	unset pw
	printf '%s' "$admin" | podman secret create axis-xtest-admin-password - >/dev/null

	# README step 2.
	if [ ! -d "$quadlet_dir" ]; then
		mkdir -p "$quadlet_dir"
		quadlet_dir_made=1
	fi
	for f in "$dir"/axis-xtest-*; do
		name=$(basename "$f")
		cp "$f" "$quadlet_dir/$name"
		quadlet_files="$quadlet_files $quadlet_dir/$name"
	done
	if QUADLET_UNIT_DIRS="$quadlet_dir" "$quadlet" -dryrun -user 2>&1 | grep -q "^---axis-xtest-axis.service---"; then
		pass "quadlet: the generator turns the five files into units (dry run)"
	else
		fail "quadlet: the generator turns the five files into units (dry run)"
	fi
	if systemctl --user daemon-reload; then
		pass "quadlet: systemctl --user daemon-reload"
	else
		fail "quadlet: systemctl --user daemon-reload"
		return
	fi
	if timeout 420 systemctl --user start "$axis_unit"; then
		pass "quadlet: systemctl --user start $axis_unit (postgres first, both healthy)"
	else
		fail "quadlet: systemctl --user start $axis_unit"
		systemctl --user status --no-pager axis-xtest-axis-postgres.service "$axis_unit" 2>&1 | tail -n 25 >&2
		journalctl --user --no-pager -n 25 -u "$axis_unit" >&2 2>&1
		return
	fi
	if wait_for 120 body_is "http://127.0.0.1:$port/readyz" '{"status":"ready"}'; then
		pass "quadlet: /readyz is ready"
	else
		fail "quadlet: /readyz is ready"
	fi
	check "quadlet: both services are active" \
		sh -c "systemctl --user is-active axis-xtest-axis-postgres.service $axis_unit | grep -vq '^active\$' && exit 1 || exit 0"
	check "quadlet: axis is healthy" health_is axis-xtest-axis healthy
	check "quadlet: the first administrator (Podman secret) logs in" \
		login "http://127.0.0.1:$port" "http://127.0.0.1:$port" admin "$admin" "$work/quadlet.jar"
	if podman exec axis-xtest-axis sh -c 'touch /media/disk1/x 2>&1' | grep -qi 'read-only'; then
		pass "quadlet: the media mount is read-only inside the container"
	else
		fail "quadlet: the media mount is read-only inside the container"
	fi
	add_first_disk quadlet "http://127.0.0.1:$port" "http://127.0.0.1:$port" "$work/quadlet.jar" /media/disk1 \
		identify Movies
	[ ! -e "$work/quadlet-media/.axis-volume" ] && pass "quadlet: nothing is written to the read-only disk" ||
		fail "quadlet: nothing is written to the read-only disk"
	no_portal quadlet "" axis-xtest-axis
	quadlet_portal "$src" "$dir" "$port" "$work/quadlet.jar" "$admin"
	if systemctl --user stop "$axis_unit" axis-xtest-axis-postgres.service; then
		pass "quadlet: systemctl --user stop"
	else
		fail "quadlet: systemctl --user stop"
	fi
	# The network and volume units stay "active (exited)" until stopped too.
	systemctl --user stop 'axis-xtest-*' >/dev/null 2>&1
	remove_quadlet_files
	systemctl --user daemon-reload
	systemctl --user reset-failed 'axis-xtest-*' >/dev/null 2>&1
	check "quadlet: its units are gone after the files are removed and daemon-reload" \
		sh -c "! systemctl --user cat $axis_unit >/dev/null 2>&1"
	check "quadlet: no axis-xtest unit is loaded any more" \
		sh -c "[ -z \"\$(systemctl --user list-units --all --plain --no-legend 'axis-xtest-*')\" ]"
}

# quadlet_portal SRC DIR PORT JAR: deploy/examples/quadlet with its two
# portal files, as the README's part "The request portal" says: the three
# lines of axis.container uncommented, the network installed, the daemon
# started again with its portal listener; a token and a requester made by the
# administrator; the portal installed and started.  This example has no
# reverse proxy, so the check plays the proxy on this machine for one
# request: the portal serves no login to a plain request, and its login form
# to one that the published port's own address reports as https.  From the
# portal's container the database does not resolve; the daemon's main port
# is open there, as the README says, and the administrator's right password
# posted to it from the portal's container is refused as a wrong one is,
# with the reason in the daemon's log.
quadlet_portal() {
	local src=$1 dir=$2 port=$3 jar=$4 admin=$5 pnet=10.89.$((60 + RANDOM % 30)) pport token requester f out code
	local portal=axis-xtest-portal unit=axis-xtest-axis-portal.service base=http://127.0.0.1:$3

	say info "quadlet: the request portal (axis-portal.network, axis-portal.container)"
	pport=$(free_port) || { fail "quadlet: no free port for the portal"; return; }
	cp "$src/axis-portal.network" "$dir/axis-portal.network"
	cp "$src/axis-portal.container" "$dir/axis-portal.container"
	# README step 1: the four lines; and for this run the test's subnet.
	edit "$dir/axis-xtest-axis.container" \
		-e 's|^# \(Network=axis-portal.network:ip=.*\)$|\1|' \
		-e 's|^# \(Environment=AXIS_PORTAL_[A-Z_]*=.*\)$|\1|' \
		-e "s|172\.31\.78\.|$pnet.|g" \
		-e 's|axis-portal\.network|axis-xtest-axis-portal.network|g'
	check "quadlet: the four lines of axis.container are there to uncomment" \
		sh -c "[ \"\$(grep -c -e '^Network=axis-xtest-axis-portal.network:ip=$pnet.2\$' -e '^Environment=AXIS_PORTAL_LISTEN_PORT=7801\$' -e '^Environment=AXIS_PORTAL_LISTEN_HOST=$pnet.2\$' -e '^Environment=AXIS_PORTAL_PEERS=$pnet.3\$' '$dir/axis-xtest-axis.container')\" = 4 ]"
	edit "$dir/axis-portal.network" \
		-e 's|^NetworkName=axis-portal$|NetworkName=axis-xtest-portal-net|' \
		-e "s|172\.31\.78\.|$pnet.|g"
	edit "$dir/axis-portal.container" \
		-e 's|^Image=ghcr.io/averagenative/axis:0.1.0-rc2$|Image='"$image"'|' \
		-e 's|^PublishPort=127.0.0.1:7802:7802$|PublishPort=127.0.0.1:'"$pport"':7802|' \
		-e 's|^ContainerName=axis-portal$|ContainerName='"$portal"'|' \
		-e 's|^Secret=axis_portal_token,|Secret=axis-xtest-portal-token,|' \
		-e 's|axis-portal\.network|axis-xtest-axis-portal.network|g' \
		-e 's|axis\.service|axis-xtest-axis.service|g' \
		-e 's|^\(Environment=AXIS_PORTAL_URL=http://\)axis:|\1axis-xtest-axis:|' \
		-e "s|172\.31\.78\.|$pnet.|g"
	mv "$dir/axis-portal.network" "$dir/axis-xtest-axis-portal.network"
	mv "$dir/axis-portal.container" "$dir/axis-xtest-axis-portal.container"
	for f in axis-xtest-axis-portal.network axis-xtest-axis.container; do
		cp "$dir/$f" "$quadlet_dir/$f"
		case " $quadlet_files " in
		*" $quadlet_dir/$f "*) ;;
		*) quadlet_files="$quadlet_files $quadlet_dir/$f" ;;
		esac
	done
	systemctl --user daemon-reload
	if timeout 300 systemctl --user restart axis-xtest-axis.service &&
		wait_for 120 body_is "$base/readyz" '{"status":"ready"}'; then
		pass "quadlet: the daemon starts again on the portal's network too, and is ready"
	else
		fail "quadlet: the daemon starts again on the portal's network too, and is ready"
		curl -s --max-time 5 -o /dev/null -w 'the published port: status %{http_code} %{errormsg}\n' "$base/readyz" >&2
		podman port axis-xtest-axis >&2 2>&1
		journalctl --user --no-pager -n 8 -u axis-xtest-axis.service 2>&1 | cut -c 1-300 >&2
		podman inspect --format '{{.Name}}: {{range $k, $v := .NetworkSettings.Networks}}{{$k}}={{$v.IPAddress}} {{end}}' \
			axis-xtest-axis axis-xtest-postgres >&2 2>&1
		podman exec -i axis-xtest-axis python3 - probe postgres 5432 < "$repo/tests/check-portal-client.py" >&2 2>&1
		return
	fi
	podman logs axis-xtest-axis 2>&1 | grep -q "portal API: listening on $pnet.2:7801" &&
		pass "quadlet: the daemon answers the portal API on $pnet.2:7801, its address on the portal's network" ||
		fail "quadlet: the daemon answers the portal API on $pnet.2:7801"
	podman logs axis-xtest-axis 2>&1 | grep -q "portal's addresses: $pnet.3 " &&
		pass "quadlet: the daemon was told the portal's address ($pnet.3)" ||
		fail "quadlet: the daemon was told the portal's address ($pnet.3)"

	# Steps 2 to 4.  (The daemon was started again: the session is in the
	# database and still good.)
	token=$(make_portal_token "$base" "$base" "$jar")
	if [ -z "$token" ]; then
		fail "quadlet: the administrator makes a portal token"
		return
	fi
	pass "quadlet: the administrator makes a portal token"
	printf '%s' "$token" | podman secret create axis-xtest-portal-token - >/dev/null
	requester=$(random_hex 12)
	code=$(web_post "$base" "$base" "$jar" /admin/users "$work/quadlet-requester" \
		"username=robin" "password=$requester" "role=requester")
	[ "$code" = 303 ] && pass "quadlet: the administrator creates a requester account" ||
		fail "quadlet: the administrator creates a requester account (status $code)"

	# Step 5.
	cp "$dir/axis-xtest-axis-portal.container" "$quadlet_dir/axis-xtest-axis-portal.container"
	quadlet_files="$quadlet_files $quadlet_dir/axis-xtest-axis-portal.container"
	systemctl --user daemon-reload
	if timeout 300 systemctl --user start "$unit"; then
		pass "quadlet: systemctl --user start $unit (healthy: axis-portal --healthcheck)"
	else
		fail "quadlet: systemctl --user start $unit"
		journalctl --user --no-pager -n 25 -u "$unit" >&2 2>&1
		return
	fi
	portal_hardened quadlet "$portal"
	check "quadlet: nothing is writable in the portal's container, /tmp included" \
		sh -c "! podman exec '$portal' touch /tmp/x 2>/dev/null && ! podman exec '$portal' touch /run/x 2>/dev/null"

	# Without a reverse proxy the portal serves no login.  With rootless
	# Podman a plain request through the published port arrives from the
	# address the example lists as the proxy's, and says nothing of who the
	# client is: 503.  From any other address it is 403, "HTTPS only".
	out=$(curl -s --max-time 10 -w ' %{http_code}' "http://127.0.0.1:$pport/login")
	case $out in
	*'name="password"'*) fail "quadlet: to a plain request the portal serves no login (it served the form)" ;;
	*" 503" | *"HTTPS only"*" 403") pass "quadlet: to a plain request the portal serves no login (${out##* })" ;;
	*) fail "quadlet: to a plain request the portal serves no login (got: $(echo "$out" | tail -c 60))" ;;
	esac
	# The check as the proxy on this machine: what arrives through the
	# published port comes from the address the example lists (with rootless
	# Podman, the container's own).
	out=$(curl -s --max-time 10 -w ' %{http_code}' -H 'X-Forwarded-Proto: https' -H 'X-Forwarded-For: 203.0.113.9' \
		-H 'X-Forwarded-Host: requests.example.com' "http://127.0.0.1:$pport/login")
	case $out in
	*'name="password"'*" 200") pass "quadlet: to a request the listed address reports as https, the portal serves its login form" ;;
	*)
		fail "quadlet: to a request the listed address reports as https, the portal serves its login form"
		podman logs "$portal" 2>&1 | grep 'not trusted' | tail -n 1 >&2
		;;
	esac

	[ "$(reach_from "$portal" axis-xtest-axis 7801)" = open ] && pass "quadlet: from the portal, the daemon's portal port is open" ||
		fail "quadlet: from the portal, the daemon's portal port is open"
	out=$(reach_from "$portal" postgres 5432)
	[ "$out" = unresolved ] && pass "quadlet: from the portal, the name postgres does not resolve" ||
		fail "quadlet: from the portal, the name postgres does not resolve (${out:-no answer})"
	out=$(reach_from "$portal" axis-xtest-postgres 5432)
	[ "$out" = unresolved ] && pass "quadlet: nor does the database container's own name" ||
		fail "quadlet: nor does the database container's own name (${out:-no answer})"
	if podman exec "$portal" cat /proc/net/route 2>/dev/null | awk 'NR > 1 && $2 == "00000000" { found = 1 } END { exit !found }'; then
		fail "quadlet: the portal's container has no default route (its network is internal)"
	else
		pass "quadlet: the portal's container has no default route (its network is internal)"
	fi
	out=$(podman inspect --format '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}' axis-xtest-postgres | tr ' ' '\n' | grep -v '^$' | head -n 1)
	code=$(reach_from "$portal" "$out" 5432)
	[ -n "$code" ] && [ "$code" != open ] && pass "quadlet: nor is PostgreSQL reachable by its address ($out: $code)" ||
		fail "quadlet: nor is PostgreSQL reachable by its address ($out: ${code:-no answer})"
	# The daemon's main port is open to the portal's container in this example
	# (the README says so, and why it cannot be closed here).  What answers
	# that: nobody logs in to Axis from the portal's address.  First that the
	# question is a real one: the same password, from this machine through the
	# published port, opens a session.
	check "quadlet: (through the published port, the administrator's password opens a session)" \
		login "$base" "$base" admin "$admin" "$work/quadlet-again.jar"
	out=$(reach_from "$portal" axis-xtest-axis 7800)
	[ "$out" = open ] && pass "quadlet: from the portal's container, the daemon's main port is open (as the README says)" ||
		fail "quadlet: from the portal's container, the daemon's main port is open (${out:-no answer}): the checks below need it so"
	# The administrator's right password, posted from the portal's own
	# container to the daemon's main port: the login page is served, the
	# answer is that of a wrong password, and no session is made.
	out=$(podman exec -i "$portal" python3 - engine-login "http://axis-xtest-axis:7800" "" admin "$admin" \
		< "$repo/tests/check-portal-client.py" 2>&1 | tr '\n' ' ')
	case $out in
	*form=200*login=401*session=no*) pass "quadlet: from the portal's container, the administrator's right password is refused as a wrong one is ($out)" ;;
	*) fail "quadlet: from the portal's container, the administrator's right password is refused as a wrong one is ($out)" ;;
	esac
	out=$(podman exec -i "$portal" python3 - engine-login "http://$pnet.2:7800" "" admin "$admin" \
		< "$repo/tests/check-portal-client.py" 2>&1 | tr '\n' ' ')
	case $out in
	*login=303* | *session=yes*) fail "quadlet: nor does it open a session at the daemon's address ($out)" ;;
	*login=*session=no*) pass "quadlet: nor does it open a session at the daemon's address ($out)" ;;
	*) fail "quadlet: nor does it open a session at the daemon's address (no answer: $out)" ;;
	esac
	podman logs axis-xtest-axis 2>&1 | grep -q "audit: login refused: the connection came from $pnet.3, one of the request portal's addresses (server.portal_peers, AXIS_PORTAL_PEERS)" &&
		pass "quadlet: and the daemon's log says why, naming the portal's address ($pnet.3) and the setting" ||
		fail "quadlet: and the daemon's log says why, naming the portal's address ($pnet.3) and the setting"
	# The same over IPv6, by which the portal's container would not be
	# $pnet.3 to the daemon.
	portal_no_ipv6 quadlet "$portal" http 7800 axis "$admin" axis-xtest-axis axis-xtest-axis "the daemon"
	# And still, afterwards, from where the administrator is: no count was
	# touched by the refusals.
	check "quadlet: (and the administrator still logs in through the published port afterwards)" \
		login "$base" "$base" admin "$admin" "$work/quadlet-after.jar"

	if systemctl --user stop "$unit"; then
		pass "quadlet: systemctl --user stop $unit"
	else
		fail "quadlet: systemctl --user stop $unit"
	fi
}

# ---------------------------------------------------------------------------
# Part: worker

# worker_config DIR PROJECT OUT DESCRIPTION: `compose config` on the copy.
worker_config() {
	local dir=$1 proj=$2 out=$3 description=$4

	if compose_in "$dir" "$proj" config > "$out" 2>&1; then
		pass "$description"
	else
		fail "$description"
		tail -n 10 "$out" >&2
	fi
}

part_worker() {
	local src=$repo/deploy/examples/remote-worker dir=$work/worker proj=$prefix-w
	local quadlet=/usr/libexec/podman/quadlet units=$work/worker-quadlet

	echo "worker: deploy/examples/remote-worker (without a second machine or a GPU)"
	check "worker: run is valid shell" sh -n "$src/run"

	if [ -z "$compose" ]; then
		skip "worker: no Compose front end (podman compose or docker compose): compose.yaml is not checked"
	else
		cp -R "$src" "$dir" || { fail "worker: copy"; return; }
		# README: the token's file beside compose.yaml, the user in .env.
		mkdir -m 700 "$dir/secrets"
		: > "$dir/secrets/axis_worker_token"
		chmod 600 "$dir/secrets/axis_worker_token"
		printf 'AXIS_IMAGE=%s\nPUID=%s\nPGID=%s\n' "$image" "$puid" "$pgid" > "$dir/.env"
		worker_config "$dir" "$proj" "$work/worker-nvenc.out" \
			"worker: compose config accepts compose.yaml as written (an NVIDIA GPU)"
		check "worker: as written it asks for the nvidia driver" grep -q 'driver: nvidia' "$work/worker-nvenc.out"
		check "worker: read-only, no capabilities, no new privileges, a process limit" \
			sh -c "grep -q 'read_only: true' '$work/worker-nvenc.out' && grep -q 'no-new-privileges:true' '$work/worker-nvenc.out' && grep -q 'pids_limit: 512' '$work/worker-nvenc.out' && grep -A1 'cap_drop:' '$work/worker-nvenc.out' | grep -q 'ALL'"

		# ENCODER, for an Intel or AMD GPU: "deploy" removed, the device and
		# the group's lines uncommented, NVIDIA's variable removed, the family
		# named, RENDER_GID in .env.
		edit "$dir/compose.yaml" \
			-e '/^    deploy:$/,/^              capabilities: \[gpu\]$/d' \
			-e 's/^    # \(devices:\)$/    \1/' \
			-e 's|^    #   \(- /dev/dri/renderD128:/dev/dri/renderD128\)$|      \1|' \
			-e 's/^    # \(group_add:\)$/    \1/' \
			-e 's/^    #   \(- "[$]{RENDER_GID}"\)$/      \1/' \
			-e '/^      NVIDIA_DRIVER_CAPABILITIES: /d' \
			-e 's/^\(      AXIS_WORKER_ENCODER_FAMILIES: \)"nvenc"$/\1"vaapi"/'
		printf 'RENDER_GID=%s\n' 105 >> "$dir/.env"
		worker_config "$dir" "$proj" "$work/worker-vaapi.out" \
			"worker: compose config accepts compose.yaml with the VA-API lines"
		check "worker: with them it passes the render node and the group, and no NVIDIA GPU" \
			sh -c "grep -q '/dev/dri/renderD128:/dev/dri/renderD128' '$work/worker-vaapi.out' && grep -A2 'group_add' '$work/worker-vaapi.out' | grep -q '105' && ! grep -qi 'nvidia' '$work/worker-vaapi.out'"

		# ENCODER, for the CPU only: those lines commented out again.
		edit "$dir/compose.yaml" \
			-e '/^    devices:$/,/^      - "[$]{RENDER_GID}"$/d' \
			-e 's/^\(      AXIS_WORKER_ENCODER_FAMILIES: \)"vaapi"$/\1"software"/'
		worker_config "$dir" "$proj" "$work/worker-cpu.out" \
			"worker: compose config accepts compose.yaml for the CPU only"
		check "worker: for the CPU only it passes no device" \
			sh -c "! grep -q 'devices:' '$work/worker-cpu.out' && grep -q 'AXIS_WORKER_ENCODER_FAMILIES: software' '$work/worker-cpu.out'"
	fi

	if [ ! -x "$quadlet" ]; then
		skip "worker: $quadlet is not installed (Podman 5 with Quadlet): axis-worker.container is not checked"
		return
	fi
	mkdir -p "$units"
	cp "$src/axis-worker.container" "$units/axis-xtest-worker.container"
	if QUADLET_UNIT_DIRS="$units" "$quadlet" -dryrun -user > "$work/worker-quadlet.out" 2>&1 &&
		grep -q '^---axis-xtest-worker.service---' "$work/worker-quadlet.out"; then
		pass "worker: Quadlet's generator accepts axis-worker.container (dry run)"
	else
		fail "worker: Quadlet's generator accepts axis-worker.container (dry run)"
		tail -n 10 "$work/worker-quadlet.out" >&2
	fi
	check "worker: the unit it makes runs the container read-only, without capabilities or new privileges" \
		sh -c "grep '^ExecStart=' '$work/worker-quadlet.out' | grep -e '--read-only' | grep -e '--cap-drop all' | grep -q -e 'no-new-privileges'"
}

# ---------------------------------------------------------------------------
# Main.

echo "check-examples: image $image"
if ! command -v podman >/dev/null 2>&1; then
	echo "  skipped  podman is not installed"
	trap - EXIT
	exit 0
fi
if ! podman image exists "$image" 2>/dev/null; then
	echo "  skipped  $image is not built (make container)"
	trap - EXIT
	exit 0
fi
command -v curl >/dev/null 2>&1 && command -v ss >/dev/null 2>&1 || { echo "  skipped  curl and ss are needed"; trap - EXIT; exit 0; }

if podman compose version >/dev/null 2>&1; then
	compose="podman compose"
fi

parts=${*:-compose full traefik quadlet worker}
work=$(mktemp -d "${TMPDIR:-/tmp}/$prefix.XXXXXX") || exit 1
chmod 755 "$work"

remove_mine
for part in $parts; do
	case $part in
	compose | full | traefik | quadlet | worker) "part_$part" ;;
	*) echo "unknown part: $part" >&2; failed=$((failed + 1)) ;;
	esac
done
cd "$repo" || true
remove_quadlet_files
cleanup
trap - EXIT
nothing_left
if [ -d "$quadlet_dir" ]; then
	check "no $prefix file is left in $quadlet_dir" sh -c "! ls '$quadlet_dir' | grep -q '^$prefix-'"
fi

echo "$passed passed, $failed failed, $skipped skipped"
[ "$failed" = 0 ]
