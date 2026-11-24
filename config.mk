# config.mk - Build configuration for axis
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Same three-file layout (Makefile, config.mk, rules.mk) and the same
# variable names as deps/htmx-glib, so anyone who knows that build knows
# this one.

PROJECT_NAME    = axis
VERSION_MAJOR   = 0
VERSION_MINOR   = 1
VERSION_MICRO   = 0

# A pre-release's suffix, such as VERSION_SUFFIX=-rc1 for the release
# candidate 0.1.0-rc1: empty for a release and for an ordinary build.  The
# image build passes it for a pre-release tag (deploy/Containerfile), so that
# `axisd --version`, the footer and the image's name say the same.  A hyphen,
# then letters and digits in parts joined by '.', at most 20 characters: the
# whole version is sent as a worker's version, which holds 32.
VERSION_SUFFIX  ?=
ifneq ($(VERSION_SUFFIX),)
    ifneq ($(findstring ',$(VERSION_SUFFIX)),)
        $(error VERSION_SUFFIX must be a hyphen and a pre-release name, such as -rc1)
    endif
    ifneq ($(shell printf '%s\n' '$(VERSION_SUFFIX)' | grep -Ex -e '-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*' | grep -Ev -e '.{21}'),$(VERSION_SUFFIX))
        $(error VERSION_SUFFIX must be a hyphen and a pre-release name, such as -rc1)
    endif
endif
VERSION         = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)$(VERSION_SUFFIX)

# Compiler settings
CC              = gcc
AR              = ar
CSTD            = -std=gnu89
WARNINGS        = -Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow
CFLAGS_BASE     = $(CSTD) $(WARNINGS)

# pkg-config dependencies.  One missing package blanks the whole list and
# shows up as "glib.h: No such file or directory", so check for it first.
PKG_DEPS        = glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0 libsoup-3.0 json-glib-1.0 libpq libsodium

# The targets that compile nothing on this machine: the image is built, and
# its checks run, inside containers, so a machine with a container tool and
# git needs none of the packages above for them.  The check below is left
# out only when every target asked for is one of these; plain `make`, and a
# command that names any other target too, still stops at a missing package.
NO_HOST_BUILD_GOALS = container container-debug container-arm64 container-multiarch \
                      check-container check-convert-container check-examples \
                      check-made-up clean
ifeq ($(MAKECMDGOALS),)
    NEED_PKG_DEPS := yes
else ifneq ($(filter-out $(NO_HOST_BUILD_GOALS),$(MAKECMDGOALS)),)
    NEED_PKG_DEPS := yes
else
    NEED_PKG_DEPS := no
endif

ifeq ($(NEED_PKG_DEPS),yes)
    PKG_OK      := $(shell pkg-config --exists $(PKG_DEPS) && echo yes)
    ifneq ($(PKG_OK),yes)
        $(error Missing build dependency: $(shell pkg-config --print-errors --exists $(PKG_DEPS) 2>&1 | head -n 1))
    endif

    PKG_CFLAGS  := $(shell pkg-config --cflags $(PKG_DEPS))
    PKG_LIBS    := $(shell pkg-config --libs $(PKG_DEPS))

    # Ships with GLib; compiles data/migrations into the binary.
    GLIB_COMPILE_RESOURCES := $(shell pkg-config --variable=glib_compile_resources gio-2.0)
endif

# htmx-glib (bundled under deps/htmx-glib).  Always built DEBUG=0: deps use
# release output whatever our own DEBUG is.
HTMX_DIR        = deps/htmx-glib
HTMX_OUTDIR     = $(HTMX_DIR)/build/release
HTMX_LIB        = $(HTMX_OUTDIR)/libhtmx-glib-1.0.a
HTMX_CFLAGS     = -I$(HTMX_DIR)/src -I$(HTMX_OUTDIR)

# Directories
SRCDIR          = src
TESTDIR         = tests
BUILDDIR        := build

# Build options (0 or 1).  RELEASE=1 is the hardened build that `make
# release` and `make install` use; it cannot be combined with the others.
DEBUG           ?= 0
ASAN            ?= 0
UBSAN           ?= 0
RELEASE         ?= 0

# Every compiler and flag combination gets its own directory, so objects
# from one never end up linked into another (make cannot tell that a .o
# was built by clang when gcc asks for it).
CC_NAME         := $(notdir $(lastword $(CC)))
ifeq ($(DEBUG),1)
    BUILD_TYPE  := debug
    CFLAGS_OPT   = -O0 -g3 -DAXIS_DEBUG
else
    BUILD_TYPE  := release
    CFLAGS_OPT   = -O2 -g -DNDEBUG
endif

# Hardening: fortified libc calls (level 3, or 2 where the compiler or C
# library rejects 3), stack protectors, position-independent code and a
# position-independent executable, immediate binding with the relocation
# table read-only (full RELRO), and a non-executable stack.
ifeq ($(RELEASE),1)
    ifneq ($(DEBUG)$(ASAN)$(UBSAN),000)
        $(error RELEASE=1 cannot be combined with DEBUG=1, ASAN=1 or UBSAN=1)
    endif
    BUILD_TYPE      := release-hardened
    FORTIFY_LEVEL   := $(shell printf 'int main(void) { return 0; }\n' | \
                         $(CC) -O2 -D_FORTIFY_SOURCE=3 -Werror -x c -c -o /dev/null - \
                         2>/dev/null && echo 3 || echo 2)
    CFLAGS_HARDEN   = -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=$(FORTIFY_LEVEL) \
                      -fstack-protector-strong -fstack-clash-protection -fPIE
    LDFLAGS_HARDEN  = -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
endif

ifeq ($(ASAN),1)
    BUILD_TYPE  := $(BUILD_TYPE)-asan
    CFLAGS_SAN  += -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS_SAN += -fsanitize=address
endif

ifeq ($(UBSAN),1)
    BUILD_TYPE  := $(BUILD_TYPE)-ubsan
    CFLAGS_SAN  += -fsanitize=undefined -fno-sanitize-recover=undefined
    LDFLAGS_SAN += -fsanitize=undefined
endif

OUTDIR          := $(BUILDDIR)/$(CC_NAME)-$(BUILD_TYPE)
OBJDIR          := $(OUTDIR)/obj

GIT_SHA         := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

# Where the source of this build is offered: the web interface's footer
# links to it (AXIS_SOURCE_URL), and the image's label names it.  If you
# run a changed Axis for other people, build it with SOURCE_URL set to
# where your changed source is (the licence's section 13).  An http or
# https address, with no quote, space, '|', '&' or '\' in it.
SOURCE_URL      ?= https://github.com/averagenative/axis
# An empty value counts as none given: an image build that passes no
# SOURCE_URL hands make an empty one in the environment.
ifeq ($(strip $(SOURCE_URL)),)
    override SOURCE_URL := https://github.com/averagenative/axis
endif
ifneq ($(filter-out http://% https://%,$(SOURCE_URL))$(word 2,$(SOURCE_URL)),)
    $(error SOURCE_URL must be one http or https address)
endif
ifneq ($(findstring ",$(SOURCE_URL))$(findstring |,$(SOURCE_URL))$(findstring &,$(SOURCE_URL))$(findstring \,$(SOURCE_URL))$(findstring ',$(SOURCE_URL)),)
    $(error SOURCE_URL must hold no quote, '|', '&' or backslash)
endif

# Verbose mode
V               ?= 0
ifeq ($(V),0)
    Q            = @
    ECHO         = @echo
else
    Q            =
    ECHO         = @true
endif

CFLAGS          = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(CFLAGS_HARDEN) $(PKG_CFLAGS) \
                  $(HTMX_CFLAGS) -DG_LOG_DOMAIN=\"axis\" \
                  -I$(SRCDIR) -I$(OUTDIR)

# libm: the estimator's size model (src/transcode) uses log, exp and sqrt.
LDFLAGS         = $(LDFLAGS_SAN) $(LDFLAGS_HARDEN) $(PKG_LIBS) -lm

# The core library (libaxis-core.a, CORE_SOURCES in the Makefile), which
# axisd and axis-worker both link, is compiled with GLib's include paths
# only: a core file that included htmx-glib or json-glib would not compile,
# and one that called libpq would not link into axis-worker, which is
# linked with nothing but these and libsoup (tests/check-worker-link.sh
# checks the result).
CORE_PKG_DEPS   = glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0
WORKER_PKG_DEPS = $(CORE_PKG_DEPS) libsoup-3.0
CORE_CFLAGS     = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(CFLAGS_HARDEN) \
                  $(shell pkg-config --cflags $(CORE_PKG_DEPS)) -DG_LOG_DOMAIN=\"axis\" \
                  -I$(SRCDIR) -I$(OUTDIR)
WORKER_CFLAGS   = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(CFLAGS_HARDEN) \
                  $(shell pkg-config --cflags $(WORKER_PKG_DEPS)) -DG_LOG_DOMAIN=\"axis-worker\" \
                  -I$(SRCDIR) -I$(OUTDIR)
WORKER_LDFLAGS  = $(LDFLAGS_SAN) $(LDFLAGS_HARDEN) $(shell pkg-config --libs $(WORKER_PKG_DEPS)) -lm

# Installation (make install DESTDIR=... PREFIX=...) and the container image.
PREFIX          ?= /usr/local
DESTDIR         ?=
OBJCOPY         ?= objcopy
CONTAINER_RUNTIME ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null)
CONTAINER_NAME  ?= localhost/axis
CONTAINER_IMAGE ?= $(CONTAINER_NAME):$(VERSION)
CONTAINER_LATEST = $(CONTAINER_NAME):latest

# The arm64 image and the two-architecture one (deploy/images.sh): the SSH
# destination of an arm64 machine to build on, and what to push where.
# Nothing is pushed unless PUSH=1.
ARM_BUILDER     ?=
PUSH            ?=
REGISTRY        ?=

# Fuzz targets (libFuzzer) are always built with clang, whatever CC is,
# and with the parsers they exercise compiled in under the same
# instrumentation.
FUZZ_CC         ?= clang
FUZZ_SECONDS    ?= 20
FUZZ_OUTDIR     := $(BUILDDIR)/fuzz
FUZZ_CFLAGS     = $(CSTD) $(WARNINGS) -O1 -g -fno-omit-frame-pointer \
                  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
                  $(shell pkg-config --cflags gio-2.0) -I$(SRCDIR)
FUZZ_LIBS       = $(shell pkg-config --libs gio-2.0) -lm
