#!/bin/bash
# images.sh - build the Axis images for amd64 and arm64 without CI
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Called by the Makefile (deploy/README.md, "Building the images yourself"):
#
#   make container-arm64        the arm64 image, built natively on an arm64
#                               machine over SSH (ARM_BUILDER), brought back
#                               here as localhost/axis:<version>-arm64
#   make container-multiarch    that, the amd64 image built here, and a
#                               manifest list of the two; pushed only with
#                               PUSH=1 and REGISTRY set
#
#   images.sh context FILE      write the build context `make container` uses
#                               (this directory less .containerignore, the
#                               submodule included) to FILE as a tar
#   images.sh arm64
#   images.sh multiarch
#
# Settings (the Makefile passes them; all have defaults here):
#   AXIS_CONTAINER_RUNTIME  podman or docker here (default: podman, else docker)
#   AXIS_VERSION            the version, as config.mk has it (with its
#                           VERSION_SUFFIX for a pre-release: 0.1.0-rc1)
#   AXIS_IMAGE              the local image name (localhost/axis)
#   CONTAINERFILE           the file built (deploy/Containerfile)
#   ARM_BUILDER             an SSH destination (a host name from ~/.ssh/config,
#                           or user@host) of an arm64 Linux machine where your
#                           user can run docker or podman without sudo
#   ARM_BUILDER_DIR         the work directory there, under its home
#                           (axis-build); made for each build, removed after
#   ARM_BUILDER_IMAGE       the image's name there (localhost/axis); it is
#                           removed from the builder once brought back, unless
#                           ARM_BUILDER_KEEP=1
#   ARM_EMULATE=1           instead of ARM_BUILDER: build arm64 here under
#                           QEMU (needs qemu-user-static registered; slow)
#   PUSH=1, REGISTRY        multiarch only: push the manifest list to
#                           REGISTRY (such as ghcr.io/averagenative/axis)
#                           under the workflow's tag policy; you log in first
#                           (podman login), nothing here holds a secret
#   YTDLP_VERSION           a yt-dlp release tag to build every architecture
#                           with (default: the newest on the day of each build,
#                           so two architectures built on different days can
#                           hold two yt-dlp releases)
#
# Nothing is pushed unless PUSH=1, and nothing is installed or configured on
# the builder: the script sends the context, runs one build there, copies the
# image back with `save`/`load`, and removes its work directory.

set -euo pipefail

cd "$(dirname "$0")/.."

runtime=${AXIS_CONTAINER_RUNTIME:-}
if [ -z "$runtime" ]; then
	runtime=$(command -v podman || command -v docker || true)
fi
version=${AXIS_VERSION:-$(sed -n 's/^VERSION_\(MAJOR\|MINOR\|MICRO\) *= *//p' config.mk | paste -sd.)}
image=${AXIS_IMAGE:-localhost/axis}
containerfile=${CONTAINERFILE:-deploy/Containerfile}
builder=${ARM_BUILDER:-}
builder_dir=${ARM_BUILDER_DIR:-axis-build}
builder_image=${ARM_BUILDER_IMAGE:-localhost/axis}
work=build/images

# The work directory is removed on the builder before and after each build:
# a plain relative path under its home, nothing else.
case "$builder_dir" in
''|/*|.|..|./*|../*|*/..|*/../*|*/.|*/./*|*[!A-Za-z0-9._/-]*)
	printf 'images: ARM_BUILDER_DIR must be a plain directory under the builder'"'"'s home, such as axis-build\n' >&2
	exit 1
	;;
esac

say() {
	printf 'images: %s\n' "$*" >&2
}

die() {
	printf 'images: %s\n' "$*" >&2
	exit 1
}

git_sha() {
	git rev-parse --short=7 HEAD 2>/dev/null || echo unknown
}

need_runtime() {
	[ -n "$runtime" ] || die "neither podman nor docker is installed here"
}

need_submodule() {
	[ -f deps/htmx-glib/Makefile ] || die "deps/htmx-glib is empty: run 'git submodule update --init'"
}

# The build context exactly as the runtime sends it to `make container`: a
# build of nothing but `COPY . /` onto an empty image, written out as a tar.
# The runtime itself applies .containerignore, so what is left out is what
# `make container` leaves out.
make_context() {
	local out=$1
	local file=$work/context.Containerfile
	local id

	need_runtime
	need_submodule
	mkdir -p "$work"
	printf 'FROM scratch\nCOPY . /\n' > "$file"
	rm -f "$out"
	id=$("$runtime" build --quiet -f "$file" --output "type=tar,dest=$out" . | tail -n 1)
	# Podman also keeps what it built as an image: not wanted.
	if [ -n "$id" ]; then
		"$runtime" rmi "$id" >/dev/null 2>&1 || true
	fi
	[ -s "$out" ] || die "the build context could not be written to $out"
}

# Runs a command on the builder.  The command is one string, run by the
# builder's shell in its home directory.
remote() {
	ssh -o BatchMode=yes "$builder" "$@"
}

# The builder's container runtime, checked: it must be arm64 and its docker
# or podman must work for this user without sudo.
builder_runtime() {
	local arch
	local rt

	arch=$(remote uname -m) || die "cannot reach $builder with ssh (it must work without a password prompt)"
	case "$arch" in
	aarch64|arm64) ;;
	*) die "$builder is $arch, not arm64" ;;
	esac
	rt=$(remote 'command -v docker || command -v podman' || true)
	[ -n "$rt" ] || die "$builder has neither docker nor podman"
	rt=${rt##*/}
	remote "$rt info >/dev/null 2>&1" ||
		die "$rt on $builder cannot be used by your user there (it may need sudo, or membership of the docker group); this script never uses sudo"
	printf '%s\n' "$rt"
}

build_args() {
	# Today's date makes the stage that fetches yt-dlp run again on a new day
	# (deploy/Containerfile); YTDLP_VERSION, when set, names one release for
	# every architecture built.
	printf '%s\n' --build-arg "GIT_SHA=$(git_sha)" --build-arg "AXIS_VERSION=$version" \
		--build-arg "YTDLP_REFRESH=${YTDLP_REFRESH:-$(date -u +%F)}"
	# Where the source is offered (config.mk's SOURCE_URL, through the
	# Makefile); without it the Containerfile's own address stands.
	if [ -n "${AXIS_SOURCE_URL:-}" ]; then
		printf '%s\n' --build-arg "SOURCE_URL=$AXIS_SOURCE_URL"
	fi
	if [ -n "${YTDLP_VERSION:-}" ]; then
		case "$YTDLP_VERSION" in
		*[!A-Za-z0-9._-]*) die "YTDLP_VERSION is a yt-dlp release tag, such as 2025.01.15: letters, digits, '.', '_' and '-'" ;;
		esac
		printf '%s\n' --build-arg "YTDLP_VERSION=$YTDLP_VERSION"
	fi
}

# arm64, natively on ARM_BUILDER: the context and the Containerfile are sent
# to ARM_BUILDER_DIR there, built, and the image is copied back here as
# $image:$version-arm64.
build_arm64_remote() {
	local rt
	local tag="$builder_image:$version-arm64"
	local loaded
	local started
	local args

	need_runtime
	rt=$(builder_runtime)
	say "building $tag on $builder with $rt"
	make_context "$work/context.tar"
	say "sending the build context ($(du -h "$work/context.tar" | cut -f1)) to $builder:$builder_dir"
	remote "rm -rf '$builder_dir' && mkdir -p '$builder_dir/context'"
	remote "tar -x -C '$builder_dir/context' -f -" < "$work/context.tar"
	remote "cat > '$builder_dir/Containerfile'" < "$containerfile"
	args=$(build_args | tr '\n' ' ')
	started=$(date +%s)
	if [ "$rt" = docker ]; then
		args="--progress=plain $args"
	fi
	if ! remote "cd '$builder_dir' && $rt build -f Containerfile $args -t '$tag' context"; then
		remote "rm -rf '$builder_dir'" || true
		die "the build on $builder failed (its output is above)"
	fi
	say "built on $builder in $(( $(date +%s) - started )) s"
	remote "rm -rf '$builder_dir'"

	# By name, not by id: Docker's containerd store calls an image by its
	# index's digest, podman by its configuration's.
	say "copying $tag back"
	loaded=$(remote "$rt save '$tag'" | "$runtime" load --quiet | sed -n 's/^Loaded image[^:]*: *//p' | tail -n 1)
	[ -n "$loaded" ] || die "the image from $builder could not be loaded here"
	"$runtime" tag "$loaded" "$image:$version-arm64"
	if [ "$loaded" != "$image:$version-arm64" ]; then
		# The name it arrived under here: only the local name is kept.
		"$runtime" rmi "$loaded" >/dev/null 2>&1 || true
	fi
	if [ "${ARM_BUILDER_KEEP:-0}" != 1 ]; then
		remote "$rt rmi '$tag' >/dev/null"
		say "removed $tag from $builder (ARM_BUILDER_KEEP=1 keeps it there)"
	fi
	check_arch "$image:$version-arm64" arm64
}

# arm64 here, under QEMU.
build_arm64_emulated() {
	need_runtime
	need_submodule
	[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] ||
		die "ARM_EMULATE=1 needs qemu-user-static registered for aarch64 (/proc/sys/fs/binfmt_misc/qemu-aarch64)"
	say "building $image:$version-arm64 here under emulation: this is slow (the build stage compiles and runs the unit tests)"
	# shellcheck disable=SC2046
	"$runtime" build --platform linux/arm64 -f "$containerfile" $(build_args) -t "$image:$version-arm64" .
	check_arch "$image:$version-arm64" arm64
}

build_arm64() {
	if [ -n "$builder" ]; then
		build_arm64_remote
	elif [ "${ARM_EMULATE:-0}" = 1 ]; then
		build_arm64_emulated
	else
		die "set ARM_BUILDER to an arm64 machine you reach with ssh, where your user can run docker or podman without sudo (for example: make container-arm64 ARM_BUILDER=mypi); or ARM_EMULATE=1 to build here under QEMU (slow)"
	fi
}

# amd64, here.
build_amd64() {
	need_runtime
	need_submodule
	case "$(uname -m)" in
	x86_64|amd64) ;;
	*) die "the amd64 image is built on this machine, which is $(uname -m)" ;;
	esac
	# shellcheck disable=SC2046
	"$runtime" build -f "$containerfile" $(build_args) -t "$image:$version-amd64" .
	check_arch "$image:$version-amd64" amd64
}

check_arch() {
	local got

	got=$("$runtime" image inspect --format '{{.Architecture}}' "$1")
	[ "$got" = "$2" ] || die "$1 is $got, not $2"
	say "$1 is ready ($2, $("$runtime" image inspect --format '{{.Size}}' "$1") bytes)"
}

# The workflow's tag policy (.github/workflows/images.yml): at a pre-release
# tag vX.Y.Z-NAME (such as v0.1.0-rc1), X.Y.Z-NAME and rc; at a release tag
# vX.Y.Z, X.Y.Z, X.Y and latest; anywhere else sha-<commit> only.  The tag
# must be the version this build has: config.mk's, with VERSION_SUFFIX for a
# pre-release.
push_tags() {
	local exact

	exact=$(git describe --exact-match --tags --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD 2>/dev/null || true)
	if [ -z "$exact" ]; then
		printf '%s\n' "sha-$(git_sha)"
		return
	fi
	[ "$exact" = "v$version" ] ||
		die "HEAD is tagged $exact but this build is version $version (config.mk; a pre-release needs its suffix too, such as VERSION_SUFFIX=-rc1)"
	case "$version" in
	*-*) printf '%s\n' "$version" rc ;;
	*) printf '%s\n' "$version" "${version%.*}" latest ;;
	esac
}

multiarch() {
	local list="$image-multiarch:$version"
	local tag

	[ "${runtime##*/}" = podman ] || die "the manifest list is made with podman (podman manifest); with docker, use docker buildx or the GitHub workflow"
	if [ "${PUSH:-0}" = 1 ]; then
		[ -n "${REGISTRY:-}" ] || die "PUSH=1 needs REGISTRY, such as REGISTRY=ghcr.io/averagenative/axis"
		[ -z "$(git status --porcelain --ignore-submodules=none 2>/dev/null)" ] ||
			die "the working tree has changes that are not committed: an image that is pushed must be the commit it names"
	fi
	build_amd64
	build_arm64
	"$runtime" manifest rm "$list" >/dev/null 2>&1 || true
	"$runtime" manifest create "$list" >/dev/null
	"$runtime" manifest add "$list" "containers-storage:$image:$version-amd64" >/dev/null
	"$runtime" manifest add "$list" "containers-storage:$image:$version-arm64" >/dev/null
	say "manifest list $list: linux/amd64 and linux/arm64"
	if [ "${PUSH:-0}" != 1 ]; then
		say "not pushed; PUSH=1 REGISTRY=<registry/name> would push it as: $(push_tags | tr '\n' ' ')"
		return
	fi
	for tag in $(push_tags); do
		say "pushing $REGISTRY:$tag"
		"$runtime" manifest push --all "$list" "docker://$REGISTRY:$tag"
	done
}

case "${1:-}" in
context)
	[ -n "${2:-}" ] || die "usage: images.sh context FILE"
	make_context "$2"
	say "wrote the build context to $2"
	;;
arm64)
	build_arm64
	;;
multiarch)
	multiarch
	;;
tags)
	push_tags
	;;
*)
	die "usage: images.sh context FILE | arm64 | multiarch | tags"
	;;
esac
