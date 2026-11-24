# Makefile - axis
# SPDX-License-Identifier: AGPL-3.0-or-later
#
#   make                 build axisd (release)
#   make DEBUG=1         debug build
#   make check           build and run all tests under ASAN + UBSAN, check
#                        that the tree holds made-up values only, and
#                        compile the fuzz targets when clang is installed
#   make test            build and run all tests with the current flags
#   make test-build      build them, and run nothing
#   make check-made-up  only the made-up values check (python3, no network;
#                        tests/tools/check-made-up.py)
#   make check-scale     the Library pages' queries on a generated index of
#                        two million files (minutes; not part of make check)
#   make check-jellyfin  start a real Jellyfin in a container (pinned by
#                        digest, no route out), make clients play on it, run
#                        the daemon's collection against it and assert what
#                        was recorded (minutes; needs podman or docker and
#                        ffmpeg; not part of make check)
#   make fuzz            run every fuzz target for FUZZ_SECONDS (default 20)
#   make release        hardened build in build/<cc>-release-hardened:
#                        axisd (stripped) and axisd.debug (its symbols)
#   make install         install the release build under DESTDIR and PREFIX
#                        (default /usr/local): bin/axisd, the example
#                        configuration, and the symbols in the layout gdb
#                        looks for under PREFIX/lib/debug
#   make container       build the container image (deploy/Containerfile)
#                        with podman, else docker, for this machine's
#                        architecture; CONTAINER_IMAGE names it.  Needs a
#                        container tool and git only, not the C packages;
#                        VERSION_SUFFIX=-rc1 builds a pre-release
#   make container-arm64 ARM_BUILDER=host
#                        build the arm64 image natively on an arm64 machine
#                        over SSH (docker or podman there, no sudo) and bring
#                        it back as localhost/axis:<version>-arm64
#   make container-multiarch ARM_BUILDER=host [PUSH=1 REGISTRY=...]
#                        amd64 here, arm64 there, and a manifest list of both
#                        (podman); pushed only with PUSH=1, under the image
#                        workflow's tag policy (deploy/README.md)
#   make container-debug extract the image build's axisd.debug into
#                        build/container-debug
#   make check-container build the image and start deploy/examples/compose's
#                        settings in a throwaway project (takes minutes;
#                        needs podman; not part of make check)
#   make check-convert-container  run the real-encode conversion tests
#                        inside the image, on its ffmpeg (takes minutes:
#                        builds the image's build stage; needs podman and an
#                        image, AXIS_CONVERT_IMAGE, default localhost/axis:latest;
#                        not part of make check)
#   make check-hwenc     the real-encode conversion tests with the hardware
#                        encoders (NVENC, VA-API) too, on one-second test
#                        clips; make check leaves the GPU alone
#                        (AXIS_TEST_HWENC=nvenc or =vaapi: one family only)
#   make clean           remove build/ (and the htmx-glib build with distclean)
#   make V=1 ...         show full command lines
#
# The tests run inside tests/with-postgres.sh, which starts a throwaway
# PostgreSQL container (podman, else docker) and removes it afterwards.
# Set AXIS_TEST_DATABASE_URL to use a server of your own instead, or
# AXIS_SKIP_DB_TESTS=1 to skip the tests that need one.
#
# In parallel.  Three things run side by side, each with its own number:
#
#   make -j16 check      -j is make's own: that many compilers at once.  Give
#                        it; without it the 700 objects compile one by one.
#   TEST_JOBS=N          that many test programs at once (default: the
#                        number of processors, at most 16).  Each one's
#                        output goes to build/<cc>-<type>/test-logs/<name>.log;
#                        the terminal gets a line as each ends, with its
#                        time, then the whole log of each that failed, the
#                        ten slowest, and the names of those that failed.
#                        All of them run, whatever the others did.
#                        TEST_JOBS=1 is the run of old: one after another,
#                        output on the terminal, stopping at the first
#                        failure.
#   FUZZ_JOBS=N          the same for `make fuzz` (logs in build/fuzz/logs).
#                        A fuzz target uses one processor for FUZZ_SECONDS:
#                        more of them at once than there are processors
#                        gives each less than its time.
#
#   make -j16 check && make -j16 fuzz
#
# On 16 processors the two take about ten minutes from an empty build
# directory (nine, then one and a half with FUZZ_SECONDS=20), and six when
# the fuzz targets are compiled already.  The longest test program sets
# the time of the tests: the list of the ten slowest says which.

include config.mk

.DEFAULT_GOAL := all

# Migrations, compiled into the binary as a GResource.  The resource list
# is generated from the directory listing on every build, and generating
# it is where the naming rules are enforced: NNNN_name.sql, numbered from
# 0001 with no gaps.  axis_db_migration_parse_filename() applies the same
# rules at run time.
MIGRATION_DIR    := data/migrations
MIGRATION_FILES  := $(sort $(wildcard $(MIGRATION_DIR)/*))
MIGRATION_XML    := $(OUTDIR)/axis-migrations.gresource.xml
MIGRATION_C      := $(OUTDIR)/axis-migrations-resource.c
MIGRATION_H      := $(OUTDIR)/axis-migrations-resource.h
MIGRATION_OBJECT := $(OBJDIR)/gen/axis-migrations-resource.o

# Web assets, compiled into the binary as a second GResource.  Every file
# under data/web is bundled except documentation (README.md, LICENSE*) and
# the icon generator script (icons/generate.sh).
# The resource list is generated here; generating it is where the file
# rules are enforced, as for migrations.  WEB_VERSION_H holds a hash over
# every bundled path and content: it is the version segment of the asset
# URLs, so any change to any asset changes every asset URL.
WEB_DIR       := data/web
WEB_FILES     := $(shell find $(WEB_DIR) -type f ! -name README.md ! -name 'LICENSE*' ! -name generate.sh | sort)
WEB_XML       := $(OUTDIR)/axis-web.gresource.xml
WEB_C         := $(OUTDIR)/axis-web-resource.c
WEB_H         := $(OUTDIR)/axis-web-resource.h
WEB_VERSION_H := $(OUTDIR)/axis-web-assets-version.h
WEB_OBJECT    := $(OBJDIR)/gen/axis-web-resource.o

# The site definitions Axis ships (add-builtin-torrent-indexers), compiled
# into the binary as a third GResource: data/definitions/axis-*.yml, each a
# definition in the format the engine runs (src/indexer/def/).  Generating
# the list is where their names are checked.
DEFINITION_DIR    := data/definitions
DEFINITION_FILES  := $(sort $(wildcard $(DEFINITION_DIR)/*.yml))
DEFINITION_XML    := $(OUTDIR)/axis-definitions.gresource.xml
DEFINITION_C      := $(OUTDIR)/axis-definitions-resource.c
DEFINITION_H      := $(OUTDIR)/axis-definitions-resource.h
DEFINITION_OBJECT := $(OBJDIR)/gen/axis-definitions-resource.o

# libaxis-core.a is what axisd and axis-worker share: code that is pure,
# works through descriptors or runs ffmpeg, and has nothing of the
# database, the job queue or the web in it.  It is compiled with GLib's
# include paths only (CORE_CFLAGS), and axis-worker links nothing else of
# Axis, without libpq: a core file that reached for the database would not
# link into the worker.  A file joins it here, by name.
CORE_SOURCES := \
	$(SRCDIR)/api/axis-worker-proto.c \
	$(SRCDIR)/core/axis-log.c \
	$(SRCDIR)/library/axis-library-name.c \
	$(SRCDIR)/probe/axis-probe-describe.c \
	$(SRCDIR)/probe/axis-probe-hdr.c \
	$(SRCDIR)/probe/axis-probe-json.c \
	$(SRCDIR)/probe/axis-probe-map.c \
	$(SRCDIR)/probe/axis-probe-model.c \
	$(SRCDIR)/probe/axis-probe-run.c \
	$(SRCDIR)/storage/axis-file-check.c \
	$(SRCDIR)/storage/axis-storage-fs.c \
	$(SRCDIR)/storage/axis-storage-open.c \
	$(SRCDIR)/storage/axis-storage-path.c \
	$(SRCDIR)/storage/axis-storage-probe.c \
	$(SRCDIR)/storage/axis-volume-marker-parse.c \
	$(SRCDIR)/storage/axis-volume-marker.c \
	$(SRCDIR)/storage/axis-volume-proof.c \
	$(SRCDIR)/transcode/axis-convert-caps.c \
	$(SRCDIR)/transcode/axis-convert-check.c \
	$(SRCDIR)/transcode/axis-convert-encoder.c \
	$(SRCDIR)/transcode/axis-convert-plan.c \
	$(SRCDIR)/transcode/axis-convert-progress.c \
	$(SRCDIR)/transcode/axis-estimate-target.c \
	$(SRCDIR)/transcode/axis-profile-builtin.c \
	$(SRCDIR)/transcode/axis-profile-definition.c \
	$(SRCDIR)/transcode/axis-profile-lock.c \
	$(SRCDIR)/transcode/axis-sample-args.c \
	$(SRCDIR)/transcode/axis-verify-judge.c \
	$(SRCDIR)/transcode/axis-verify-measure.c
CORE_OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CORE_SOURCES))
CORE_STATIC  := $(OUTDIR)/libaxis-core.a

# libaxis.a holds the daemon's own code, everything else except each
# program's main.c, so the tests link exactly the objects the daemon does.
LIB_SOURCES  := $(filter-out $(SRCDIR)/axisd/% $(CORE_SOURCES),$(shell find $(SRCDIR) -name '*.c' | sort))
LIB_OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_SOURCES))
LIB_STATIC   := $(OUTDIR)/libaxis.a

AXISD_SOURCES := $(shell find $(SRCDIR)/axisd -name '*.c' | sort)
AXISD_OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(AXISD_SOURCES))
AXISD         := $(OUTDIR)/axisd

# axis-worker: clients/worker/, compiled with GLib's and libsoup's include
# paths only (WORKER_CFLAGS) and linked with libaxis-core.a alone.
WORKER_DIR     := clients/worker
WORKER_SOURCES := $(sort $(wildcard $(WORKER_DIR)/*.c))
WORKER_OBJECTS := $(patsubst %.c,$(OBJDIR)/%.o,$(WORKER_SOURCES))
AXIS_WORKER    := $(OUTDIR)/axis-worker

# axis-portal, the request portal (add-request-portal): clients/portal/, and
# the few files of src/ it shares with the daemon.  Those are objects of
# libaxis.a too (the daemon uses them), but they are compiled without
# libpq's include paths (PORTAL_SHARED_CFLAGS), and axis-portal is linked
# with them by name and with nothing else of the daemon, without libpq: a
# file here that reached for the database would not link into the portal.
# A file joins the list here, by name.  Everything of clients/portal/ but
# main.c is libaxis-portal.a, so that the tests run the portal's own code.
PORTAL_SHARED_SOURCES := \
	$(SRCDIR)/core/axis-proxy.c \
	$(SRCDIR)/core/axis-proxy-resolver.c \
	$(SRCDIR)/web/axis-login-throttle.c \
	$(SRCDIR)/web/axis-secret.c \
	$(SRCDIR)/web/axis-web-auth-input.c \
	$(SRCDIR)/web/axis-web-headers.c \
	$(SRCDIR)/web/axis-web-limits.c
PORTAL_SHARED_OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(PORTAL_SHARED_SOURCES))
PORTAL_DIR         := clients/portal
PORTAL_SOURCES     := $(sort $(wildcard $(PORTAL_DIR)/*.c))
PORTAL_OBJECTS     := $(patsubst %.c,$(OBJDIR)/%.o,$(PORTAL_SOURCES))
PORTAL_MAIN_OBJECT := $(OBJDIR)/$(PORTAL_DIR)/main.o
PORTAL_LIB_OBJECTS := $(filter-out $(PORTAL_MAIN_OBJECT),$(PORTAL_OBJECTS))
PORTAL_STATIC      := $(OUTDIR)/libaxis-portal.a
AXIS_PORTAL        := $(OUTDIR)/axis-portal

# tests/test-*.c are programs; any other tests/*.c is support code linked
# into every one of them.
TEST_SOURCES         := $(sort $(wildcard $(TESTDIR)/test-*.c))
TEST_OBJECTS         := $(patsubst $(TESTDIR)/%.c,$(OBJDIR)/tests/%.o,$(TEST_SOURCES))
TEST_BINS            := $(patsubst $(TESTDIR)/%.c,$(OUTDIR)/tests/%,$(TEST_SOURCES))
TEST_SUPPORT_SOURCES := $(filter-out $(TEST_SOURCES),$(sort $(wildcard $(TESTDIR)/*.c)))
TEST_SUPPORT_OBJECTS := $(patsubst $(TESTDIR)/%.c,$(OBJDIR)/tests/%.o,$(TEST_SUPPORT_SOURCES))

# tests/scale/test-*.c are programs too, but not part of `make test` or
# `make check`: they build a large generated index and take minutes
# (`make check-scale`).
SCALE_SOURCES        := $(sort $(wildcard $(TESTDIR)/scale/test-*.c))
SCALE_BINS           := $(patsubst $(TESTDIR)/%.c,$(OUTDIR)/tests/%,$(SCALE_SOURCES))

# tests/jellyfin/test-*.c are programs that need a real Jellyfin server
# (`make check-jellyfin`); they are not part of `make test`.
LIVE_SOURCES         := $(sort $(wildcard $(TESTDIR)/jellyfin/test-*.c))
LIVE_BINS            := $(patsubst $(TESTDIR)/%.c,$(OUTDIR)/tests/%,$(LIVE_SOURCES))

# Fuzz targets, and the parsers of outside input they are built with.
FUZZ_SOURCES     := $(sort $(wildcard $(TESTDIR)/fuzz/fuzz-*.c))
FUZZ_BINS        := $(patsubst $(TESTDIR)/fuzz/%.c,$(FUZZ_OUTDIR)/%,$(FUZZ_SOURCES))
FUZZ_LIB_SOURCES := $(SRCDIR)/db/axis-db-migrate-parse.c $(SRCDIR)/web/axis-web-path.c \
                    $(SRCDIR)/core/axis-proxy.c $(SRCDIR)/web/axis-web-auth-input.c \
                    $(SRCDIR)/core/axis-outbound-parse.c $(SRCDIR)/core/axis-outbound.c \
                    $(SRCDIR)/web/axis-web-hub-topic.c $(SRCDIR)/core/axis-setting-units.c \
                    $(SRCDIR)/storage/axis-volume-marker-parse.c $(SRCDIR)/storage/axis-storage-path.c \
                    $(SRCDIR)/library/axis-library-name.c $(SRCDIR)/library/axis-library-kind.c \
                    $(SRCDIR)/library/axis-library-filter.c $(SRCDIR)/library/axis-analysis-name.c \
                    $(SRCDIR)/library/axis-analysis-rules.c $(SRCDIR)/library/axis-analysis-rank.c \
                    $(SRCDIR)/core/axis-size.c \
                    $(SRCDIR)/probe/axis-probe-json.c $(SRCDIR)/probe/axis-probe-map.c \
                    $(SRCDIR)/probe/axis-probe-hdr.c $(SRCDIR)/probe/axis-probe-model.c \
                    $(SRCDIR)/transcode/axis-estimate-target.c $(SRCDIR)/transcode/axis-estimate-eligibility.c \
                    $(SRCDIR)/transcode/axis-estimate-model.c $(SRCDIR)/transcode/axis-estimate-total.c \
                    $(SRCDIR)/transcode/axis-profile-definition.c $(SRCDIR)/transcode/axis-profile-lock.c \
                    $(SRCDIR)/transcode/axis-profile-builtin.c \
                    $(SRCDIR)/transcode/axis-convert-encoder.c $(SRCDIR)/transcode/axis-convert-plan.c \
                    $(SRCDIR)/transcode/axis-convert-check.c $(SRCDIR)/transcode/axis-convert-progress.c \
                    $(SRCDIR)/compat/axis-compat-model.c $(SRCDIR)/compat/axis-compat-plex.c \
                    $(SRCDIR)/compat/axis-compat-jellyfin.c $(SRCDIR)/compat/axis-compat-reasons.c \
                    $(SRCDIR)/compat/axis-compat-evidence.c $(SRCDIR)/compat/axis-compat-verdict.c \
                    $(SRCDIR)/compat/axis-compat-path.c $(SRCDIR)/core/axis-http-origin.c \
                    $(SRCDIR)/api/axis-worker-proto.c $(SRCDIR)/api/axis-portal-proto.c \
                    $(SRCDIR)/storage/axis-storage-open.c \
                    $(SRCDIR)/storage/axis-storage-fs.c $(SRCDIR)/transcode/axis-sample-args.c \
                    $(SRCDIR)/transcode/axis-verify-judge.c $(SRCDIR)/probe/axis-probe-run.c \
                    $(SRCDIR)/meta/axis-meta-model.c $(SRCDIR)/meta/axis-meta-image.c \
                    $(SRCDIR)/meta/axis-meta-tmdb-parse.c $(TESTDIR)/axis-test-meta-fixture-parse.c \
                    $(SRCDIR)/meta/axis-meta-json.c $(SRCDIR)/meta/axis-meta-tvdb-parse.c \
                    $(SRCDIR)/download/axis-download-bencode.c $(SRCDIR)/download/axis-download-model.c \
                    $(SRCDIR)/download/axis-download-path.c $(SRCDIR)/core/axis-http-cookie.c \
                    $(SRCDIR)/download/axis-download-sabnzbd-parse.c \
                    $(SRCDIR)/download/axis-download-deluge-parse.c \
                    $(SRCDIR)/download/axis-download-qbittorrent-parse.c \
                    $(SRCDIR)/indexer/axis-indexer-model.c $(SRCDIR)/indexer/axis-indexer-prowlarr-parse.c \
                    $(SRCDIR)/indexer/axis-indexer-xml.c $(SRCDIR)/indexer/axis-indexer-newznab-parse.c \
                    $(SRCDIR)/indexer/axis-indexer-sites-parse.c $(SRCDIR)/indexer/axis-indexer-jackett-parse.c \
                    $(SRCDIR)/indexer/def/axis-def.c $(SRCDIR)/indexer/def/axis-def-engine.c \
                    $(SRCDIR)/indexer/def/axis-def-filter.c $(SRCDIR)/indexer/def/axis-def-html.c \
                    $(SRCDIR)/indexer/def/axis-def-regex.c $(SRCDIR)/indexer/def/axis-def-select.c \
                    $(SRCDIR)/indexer/def/axis-def-template.c $(SRCDIR)/indexer/def/axis-def-yaml.c \
                    $(SRCDIR)/indexer/def/axis-def-trace.c \
                    $(SRCDIR)/release/axis-release.c $(SRCDIR)/release/axis-release-tables.c \
                    $(SRCDIR)/release/axis-release-parse.c $(SRCDIR)/release/axis-release-match.c \
                    $(SRCDIR)/release/axis-release-episodes.c $(SRCDIR)/release/axis-release-after.c \
                    $(SRCDIR)/quality/axis-quality-ladder.c $(SRCDIR)/quality/axis-quality-rule.c \
                    $(SRCDIR)/quality/axis-quality-profile.c $(SRCDIR)/quality/axis-quality-builtin.c \
                    $(SRCDIR)/quality/axis-quality-decide.c $(SRCDIR)/import/axis-import-name.c \
                    $(SRCDIR)/identify/axis-identify-ids.c $(SRCDIR)/identify/axis-identify-rules.c \
                    $(SRCDIR)/arr/axis-arr-parse.c \
                    $(SRCDIR)/youtube/axis-youtube-address.c $(SRCDIR)/youtube/axis-youtube-output.c \
                    $(PORTAL_DIR)/axis-portal-wire.c $(PORTAL_DIR)/axis-portal-form.c \
                    $(PORTAL_DIR)/axis-portal-view.c
FUZZ_HEADERS    := $(shell find $(SRCDIR) -name '*.h' | sort) $(TESTDIR)/axis-test-meta-fixture-parse.h \
                   $(sort $(wildcard $(PORTAL_DIR)/*.h))

# Developer tools: tests/tools/*.c, one program each, built with the tests
# (so they always compile) but neither run by them nor installed.
#   build/<cc>-<type>/tests/tools/axis-release-audit FILE
TOOL_SOURCES     := $(sort $(wildcard $(TESTDIR)/tools/*.c))
TOOL_BINS        := $(patsubst $(TESTDIR)/%.c,$(OUTDIR)/tests/%,$(TOOL_SOURCES))

GENERATED    := $(OUTDIR)/axis-version.h $(MIGRATION_H) $(WEB_H) $(WEB_VERSION_H) $(DEFINITION_H)

include rules.mk

.PHONY: all
all: $(AXISD) $(AXIS_WORKER) $(AXIS_PORTAL)

# htmx-glib: recursive make, static library only.  The submodule must be
# checked out; say so plainly instead of failing inside it.
$(HTMX_LIB):
	@test -f $(HTMX_DIR)/Makefile || { \
		echo "deps/htmx-glib is empty: run 'git submodule update --init'"; exit 1; }
	$(ECHO) "  MAKE    $(HTMX_DIR)"
	$(Q)$(MAKE) --no-print-directory -C $(HTMX_DIR) DEBUG=0 ASAN=0 UBSAN=0 static

$(LIB_OBJECTS) $(AXISD_OBJECTS) $(MIGRATION_OBJECT) $(WEB_OBJECT) $(DEFINITION_OBJECT): $(GENERATED) | $(HTMX_LIB)
$(CORE_OBJECTS) $(WORKER_OBJECTS): $(GENERATED)
$(PORTAL_OBJECTS): $(GENERATED) | $(HTMX_LIB)

$(LIB_STATIC): $(LIB_OBJECTS) $(MIGRATION_OBJECT) $(WEB_OBJECT) $(DEFINITION_OBJECT)
	$(ECHO) "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(LIB_OBJECTS) $(MIGRATION_OBJECT) $(WEB_OBJECT) $(DEFINITION_OBJECT)

$(CORE_STATIC): $(CORE_OBJECTS)
	$(ECHO) "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(CORE_OBJECTS)

$(AXISD): $(AXISD_OBJECTS) $(LIB_STATIC) $(CORE_STATIC) $(HTMX_LIB)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) -o $@ $(AXISD_OBJECTS) $(LIB_STATIC) $(CORE_STATIC) $(HTMX_LIB) $(LDFLAGS)

$(AXIS_WORKER): $(WORKER_OBJECTS) $(CORE_STATIC)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) -o $@ $(WORKER_OBJECTS) $(CORE_STATIC) $(WORKER_LDFLAGS)

$(PORTAL_STATIC): $(PORTAL_LIB_OBJECTS)
	$(ECHO) "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(PORTAL_LIB_OBJECTS)

$(AXIS_PORTAL): $(PORTAL_MAIN_OBJECT) $(PORTAL_STATIC) $(PORTAL_SHARED_OBJECTS) $(CORE_STATIC) $(HTMX_LIB)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) -o $@ $(PORTAL_MAIN_OBJECT) $(PORTAL_STATIC) $(PORTAL_SHARED_OBJECTS) $(CORE_STATIC) $(HTMX_LIB) \
		$(PORTAL_LDFLAGS)

# Release: the hardened build, with its symbols split off.  Each program
# stays the unstripped link output (the tests run it); <program>-stripped
# is what gets installed, with a link to <program>.debug so that gdb and
# crash tools find the symbols.
AXISD_DEBUG          := $(OUTDIR)/axisd.debug
AXISD_STRIPPED       := $(OUTDIR)/axisd-stripped
AXIS_WORKER_DEBUG    := $(OUTDIR)/axis-worker.debug
AXIS_WORKER_STRIPPED := $(OUTDIR)/axis-worker-stripped
AXIS_PORTAL_DEBUG    := $(OUTDIR)/axis-portal.debug
AXIS_PORTAL_STRIPPED := $(OUTDIR)/axis-portal-stripped

$(OUTDIR)/%.debug: $(OUTDIR)/%
	$(ECHO) "  DEBUG   $@"
	$(Q)$(OBJCOPY) --only-keep-debug $< $@
	$(Q)chmod 0644 $@

$(OUTDIR)/%-stripped: $(OUTDIR)/% $(OUTDIR)/%.debug
	$(ECHO) "  STRIP   $@"
	$(Q)cd $(OUTDIR) && $(OBJCOPY) --strip-all --add-gnu-debuglink=$*.debug $* $*-stripped

.PHONY: release release-files
release:
	$(Q)$(MAKE) --no-print-directory RELEASE=1 release-files

release-files: $(AXISD_STRIPPED) $(AXISD_DEBUG) $(AXIS_WORKER_STRIPPED) $(AXIS_WORKER_DEBUG) \
	$(AXIS_PORTAL_STRIPPED) $(AXIS_PORTAL_DEBUG)

.PHONY: install install-files
install:
	$(Q)$(MAKE) --no-print-directory RELEASE=1 install-files

install-files: release-files
	install -D -m 0755 $(AXISD_STRIPPED) $(DESTDIR)$(PREFIX)/bin/axisd
	install -D -m 0644 $(AXISD_DEBUG) $(DESTDIR)$(PREFIX)/lib/debug$(PREFIX)/bin/axisd.debug
	install -D -m 0755 $(AXIS_WORKER_STRIPPED) $(DESTDIR)$(PREFIX)/bin/axis-worker
	install -D -m 0644 $(AXIS_WORKER_DEBUG) $(DESTDIR)$(PREFIX)/lib/debug$(PREFIX)/bin/axis-worker.debug
	install -D -m 0644 data/axis.conf.example $(DESTDIR)$(PREFIX)/share/axis/axis.conf.example
	install -D -m 0755 $(AXIS_PORTAL_STRIPPED) $(DESTDIR)$(PREFIX)/bin/axis-portal
	install -D -m 0644 $(AXIS_PORTAL_DEBUG) $(DESTDIR)$(PREFIX)/lib/debug$(PREFIX)/bin/axis-portal.debug
	install -D -m 0644 data/axis-worker.conf.example $(DESTDIR)$(PREFIX)/share/axis/axis-worker.conf.example
	install -D -m 0644 data/axis-portal.conf.example $(DESTDIR)$(PREFIX)/share/axis/axis-portal.conf.example

# The container image.  The build context is this directory minus
# .containerignore, so the submodule must be checked out; the commit is
# passed in because .git is not part of the context.
# yt-dlp is the newest release of the day of the build: today's date as a
# build argument makes the stage that fetches it run again on a new day,
# whatever layers the builder kept (deploy/Containerfile, the ytdlp stage).
YTDLP_REFRESH ?= $(shell date -u +%F)
# One yt-dlp release for every image built (a tag, such as 2025.01.15);
# empty: the newest on the day of each build.
YTDLP_VERSION ?=
export YTDLP_VERSION

.PHONY: container container-debug check-container check-convert-container
container:
	@test -n "$(CONTAINER_RUNTIME)" || { \
		echo "make container: neither podman nor docker is installed"; exit 1; }
	@test -f $(HTMX_DIR)/Makefile || { \
		echo "deps/htmx-glib is empty: run 'git submodule update --init'"; exit 1; }
	$(CONTAINER_RUNTIME) build -f deploy/Containerfile \
		--build-arg GIT_SHA=$(GIT_SHA) --build-arg AXIS_VERSION=$(VERSION) \
		--build-arg SOURCE_URL=$(SOURCE_URL) \
		--build-arg YTDLP_REFRESH=$(YTDLP_REFRESH) \
		$(if $(YTDLP_VERSION),--build-arg YTDLP_VERSION=$(YTDLP_VERSION)) \
		-t $(CONTAINER_IMAGE) -t $(CONTAINER_LATEST) .

container-debug:
	@test -n "$(CONTAINER_RUNTIME)" || { \
		echo "make container-debug: neither podman nor docker is installed"; exit 1; }
	$(CONTAINER_RUNTIME) build -f deploy/Containerfile --target debug \
		--build-arg GIT_SHA=$(GIT_SHA) --build-arg AXIS_VERSION=$(VERSION) \
		--output type=local,dest=$(BUILDDIR)/container-debug .

# Other architectures, with no CI: deploy/images.sh sends the build context
# above to the arm64 builder, builds there, and copies the image back; the
# manifest list joins it with this machine's.  ARM_BUILDER_DIR,
# ARM_BUILDER_IMAGE, ARM_BUILDER_KEEP and ARM_EMULATE are read from the
# environment (the script's header says what each does).
IMAGES_ENV = AXIS_CONTAINER_RUNTIME="$(CONTAINER_RUNTIME)" AXIS_VERSION="$(VERSION)" \
	AXIS_SOURCE_URL="$(SOURCE_URL)" \
	AXIS_IMAGE="$(CONTAINER_NAME)" ARM_BUILDER="$(ARM_BUILDER)" PUSH="$(PUSH)" REGISTRY="$(REGISTRY)"

.PHONY: container-arm64 container-multiarch
container-arm64:
	$(Q)$(IMAGES_ENV) bash deploy/images.sh arm64

container-multiarch:
	$(Q)$(IMAGES_ENV) bash deploy/images.sh multiarch

check-container:
	$(Q)AXIS_CONTAINER_RUNTIME="$(CONTAINER_RUNTIME)" AXIS_CONTAINER_IMAGE="$(CONTAINER_IMAGE)" \
		MAKE="$(MAKE)" $(TESTDIR)/check-container.sh

check-convert-container:
	$(Q)AXIS_CONTAINER_RUNTIME="$(CONTAINER_RUNTIME)" bash $(TESTDIR)/check-convert-container.sh

# The deployment examples, run for real (Compose, the full stack, Traefik,
# Quadlet).  Not part of check or check-container: it pulls public images and
# needs a Compose front end and a user systemd; each part says why it skips.
.PHONY: check-examples
check-examples:
	$(Q)AXIS_XTEST_IMAGE="$(CONTAINER_LATEST)" bash $(TESTDIR)/check-examples.sh

# Whether a release binary has the protections config.mk asks for.
.PHONY: check-release
check-release: release
	$(Q)$(TESTDIR)/check-release.sh $(BUILDDIR)/$(CC_NAME)-release-hardened

# Every object of tests/ needs the generated headers and htmx-glib's own
# (htmx-version.h is made by its build).  Named targets, not a pattern: a
# pattern rule without a recipe adds nothing to the one in rules.mk, and a
# parallel build then compiled them before htmx-glib was built.
TEST_ALL_OBJECTS := $(TEST_OBJECTS) $(TEST_SUPPORT_OBJECTS) \
	$(patsubst $(TESTDIR)/%.c,$(OBJDIR)/tests/%.o,$(SCALE_SOURCES) $(LIVE_SOURCES) $(TOOL_SOURCES))
$(TEST_ALL_OBJECTS): $(GENERATED) | $(HTMX_LIB)

# (libaxis-portal.a: the portal's own code, for the tests of it; a test that
# names nothing of it links nothing of it.)
$(OUTDIR)/tests/%: $(OBJDIR)/tests/%.o $(TEST_SUPPORT_OBJECTS) $(PORTAL_STATIC) $(LIB_STATIC) $(CORE_STATIC) $(HTMX_LIB)
	@mkdir -p $(dir $@)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) -o $@ $< $(TEST_SUPPORT_OBJECTS) $(PORTAL_STATIC) $(LIB_STATIC) $(CORE_STATIC) $(HTMX_LIB) $(LDFLAGS)

.PRECIOUS: $(OBJDIR)/tests/%.o

# Everything `make test` runs, built and not run.
.PHONY: test-build
test-build: $(AXISD) $(AXIS_WORKER) $(AXIS_PORTAL) $(TEST_BINS) $(TOOL_BINS)

# Test programs that run alone, one at a time, after all the others: a
# program belongs here only when it cannot be made to hold on a machine
# busy with the others.  One per line, by name (test-...), with the reason.
TEST_SERIAL :=
TEST_SERIAL_BINS := $(filter $(addprefix $(OUTDIR)/tests/,$(TEST_SERIAL)),$(TEST_BINS))
TEST_LOGDIR      := $(OUTDIR)/test-logs

# Run every test binary against one throwaway PostgreSQL, TEST_JOBS at a
# time (tests/run-parallel.sh: a log per program in $(TEST_LOGDIR), all of
# them run, every failure reported at the end).  Each test makes and drops
# its own database in it, its own scratch directories, and listens on ports
# it finds free.  TEST_JOBS=1: one after another on the terminal, stopping
# at the first failure.  The
# hardware encoders are never run here, whatever the environment says (the
# GPU may be shared with other work): `make check-hwenc` runs them, and
# every axis-worker a test starts is told to use software encoders only.
# First, that axis-worker links nothing of the database, the job queue or
# the web (tests/check-worker-link.sh), and axis-portal nothing of the
# database or of the daemon's own code (tests/check-portal-link.sh).
.PHONY: test
test: test-build
	$(Q)sh $(TESTDIR)/check-worker-link.sh $(AXIS_WORKER)
	$(Q)sh $(TESTDIR)/check-portal-link.sh $(AXIS_PORTAL)
ifeq ($(TEST_JOBS),1)
	$(Q)AXIS_TEST_HWENC= $(TESTDIR)/with-postgres.sh sh -c \
		'for t in "$$@"; do echo "  TEST    $$t"; "$$t" || exit 1; done' \
		axis-tests $(TEST_BINS)
else
	$(Q)AXIS_TEST_HWENC= $(TESTDIR)/with-postgres.sh sh $(TESTDIR)/run-parallel.sh \
		$(TEST_JOBS) $(TEST_LOGDIR) TEST \
		$(filter-out $(TEST_SERIAL_BINS),$(TEST_BINS)) --serial $(TEST_SERIAL_BINS)
endif
	@echo "All tests passed."

# The scale tests: the Library pages' queries on a generated index of
# millions of rows (AXIS_SCALE_ROWS, default 2000000), with their plans and
# timings printed.  Run under the same sanitizers as `make check`, where
# only the client is slower; the database does the same work either way.
# Its data stays on disk (AXIS_TEST_PG_ON_DISK): millions of rows are too
# many to keep in memory, and what is measured is a database on a disk.
#   make check-scale
#   AXIS_SCALE_ROWS=300000 make check-scale
.PHONY: check-scale
check-scale:
	$(Q)$(MAKE) --no-print-directory DEBUG=1 ASAN=1 UBSAN=1 run-scale

.PHONY: run-scale
run-scale: $(AXISD) $(SCALE_BINS)
	$(Q)AXIS_TEST_PG_ON_DISK=1 $(TESTDIR)/with-postgres.sh sh -c \
		'for t in "$$@"; do echo "  SCALE   $$t"; "$$t" || exit 1; done' \
		axis-scale-tests $(SCALE_BINS)

# The real-encode tests with the hardware encoders too: NVENC and VA-API
# run for real on one-second test clips (in a private scratch directory),
# one encode at a time, wherever the capability check finds them.  The
# NVIDIA GPU's free memory is read first (tests/check-hwenc.sh): with less
# than AXIS_HWENC_MIN_FREE_MIB (500) free, NVENC is left out.  Not part of
# `make check`, which leaves the GPU alone.
#   make check-hwenc                        both families
#   AXIS_TEST_HWENC=vaapi make check-hwenc  one family (nvenc or vaapi)
.PHONY: check-hwenc
check-hwenc:
	$(Q)$(MAKE) --no-print-directory DEBUG=1 ASAN=1 UBSAN=1 run-hwenc

.PHONY: run-hwenc
run-hwenc: $(AXISD) $(OUTDIR)/tests/test-convert-real
	$(Q)sh $(TESTDIR)/check-hwenc.sh $(OUTDIR)/tests/test-convert-real
	@echo "The hardware encoder check passed."

# The daemon's collection against a real Jellyfin: the container (tests/jellyfin)
# comes up with a generated media tree, clients play on it, the program
# reads it as the daemon does and asserts what was recorded, and everything
# is removed again, whatever happens.
.PHONY: check-jellyfin
check-jellyfin:
	$(Q)$(MAKE) --no-print-directory DEBUG=1 ASAN=1 UBSAN=1 run-jellyfin

.PHONY: run-jellyfin
run-jellyfin: $(AXISD) $(LIVE_BINS)
	$(Q)$(TESTDIR)/with-postgres.sh sh $(TESTDIR)/jellyfin/with-jellyfin.sh sh $(TESTDIR)/jellyfin/check.sh \
		$(LIVE_BINS)
	@echo "The Jellyfin check passed."

# The repository is public, so its files hold made-up titles, identifiers,
# hosts and addresses only: every tracked file is read for values that are
# not (tests/tools/made_up.py says what is).  No network, no build needed.
.PHONY: check-made-up
check-made-up:
	$(ECHO) "  CHECK   made-up values"
	$(Q)python3 -I $(TESTDIR)/tools/check-made-up.py --self-test
	$(Q)python3 -I $(TESTDIR)/tools/check-made-up.py

# The documents and the program agree on whether the request portal has been
# reviewed for the internet (tests/tools/check-portal-review.py says where
# and how).  No network, no build needed.
.PHONY: check-portal-review
check-portal-review:
	$(ECHO) "  CHECK   the request portal's review statement"
	$(Q)python3 -I $(TESTDIR)/tools/check-portal-review.py --self-test
	$(Q)python3 -I $(TESTDIR)/tools/check-portal-review.py

# The gate: everything, under both sanitizers.  The fuzz targets are only
# compiled here; `make fuzz` runs them.
.PHONY: check
check: check-made-up check-portal-review
	$(Q)$(MAKE) --no-print-directory DEBUG=1 ASAN=1 UBSAN=1 test
	$(Q)if command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		$(MAKE) --no-print-directory fuzz-build; \
	else \
		echo "  SKIP    fuzz targets: $(FUZZ_CC) is not installed"; \
	fi

.PHONY: fuzz-build
fuzz-build: $(FUZZ_BINS)

# `make -jN check fuzz` in one command: check compiles the fuzz targets
# itself, in a make of its own, so the two would write the same files at
# once.  The fuzz targets wait for check then.
ifneq ($(filter check,$(MAKECMDGOALS)),)
$(FUZZ_BINS): | check
endif

# Each target runs for FUZZ_SECONDS, FUZZ_JOBS of them at a time
# (tests/fuzz/run-target.sh runs one, tests/run-parallel.sh all of them: a
# log per target in build/fuzz/logs).  New inputs go to build/fuzz/corpus,
# never into the seed corpus under tests/fuzz/corpus; a crash is saved
# under build/fuzz/artifacts and fails the run, which names the target.
# FUZZ_JOBS=1: one after another on the terminal, stopping at the first
# crash.
.PHONY: fuzz
fuzz: fuzz-build
ifeq ($(FUZZ_JOBS),1)
	$(Q)for bin in $(FUZZ_BINS); do \
		echo "  FUZZ    $${bin##*/} ($(FUZZ_SECONDS) s)"; \
		FUZZ_SECONDS=$(FUZZ_SECONDS) FUZZ_OUTDIR=$(FUZZ_OUTDIR) \
			sh $(TESTDIR)/fuzz/run-target.sh $$bin || exit 1; \
	done
else
	$(Q)FUZZ_SECONDS=$(FUZZ_SECONDS) FUZZ_OUTDIR=$(FUZZ_OUTDIR) sh $(TESTDIR)/run-parallel.sh \
		$(FUZZ_JOBS) $(FUZZ_OUTDIR)/logs FUZZ --with $(TESTDIR)/fuzz/run-target.sh $(FUZZ_BINS)
endif
	@echo "Fuzzing found nothing."

.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

.PHONY: distclean
distclean: clean
	-$(MAKE) --no-print-directory -C $(HTMX_DIR) clean

-include $(LIB_OBJECTS:.o=.d) $(AXISD_OBJECTS:.o=.d) $(MIGRATION_OBJECT:.o=.d) $(WEB_OBJECT:.o=.d)
-include $(DEFINITION_OBJECT:.o=.d)
-include $(CORE_OBJECTS:.o=.d) $(WORKER_OBJECTS:.o=.d) $(PORTAL_OBJECTS:.o=.d)
-include $(TEST_OBJECTS:.o=.d) $(TEST_SUPPORT_OBJECTS:.o=.d)
-include $(patsubst $(TESTDIR)/%.c,$(OBJDIR)/tests/%.d,$(TOOL_SOURCES))
