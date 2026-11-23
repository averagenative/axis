# rules.mk - Common build rules for axis
# SPDX-License-Identifier: AGPL-3.0-or-later

# Objects from src/ -- mkdir -p handles every subdirectory.
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# The core library's objects: the same sources, with GLib's include paths
# only (see config.mk).  A static pattern rule, so it wins over the one
# above for exactly these.
$(CORE_OBJECTS): $(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(ECHO) "  CC      $< (core)"
	$(Q)$(CC) $(CORE_CFLAGS) -MMD -MP -c -o $@ $<

# Objects from clients/ (axis-worker): GLib's and libsoup's include paths
# only.
$(OBJDIR)/clients/%.o: clients/%.c
	@mkdir -p $(dir $@)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(WORKER_CFLAGS) -MMD -MP -c -o $@ $<

# Objects from generated sources in $(OUTDIR)
$(OBJDIR)/gen/%.o: $(OUTDIR)/%.c
	@mkdir -p $(dir $@)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Objects from tests/
$(OBJDIR)/tests/%.o: $(TESTDIR)/%.c
	@mkdir -p $(dir $@)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -I$(TESTDIR) -DAXISD_PATH=\"$(abspath $(AXISD))\" \
		-DAXIS_WORKER_PATH=\"$(abspath $(AXIS_WORKER))\" \
		-DAXIS_MIGRATION_DIR=\"$(abspath $(MIGRATION_DIR))\" \
		-MMD -MP -c -o $@ $<

$(OUTDIR):
	@mkdir -p $(OUTDIR)

# Version header.  Rewritten only when its content changes, so a new git
# revision does not rebuild the world unless something reads it.
$(OUTDIR)/axis-version.h: $(SRCDIR)/axis-version.h.in config.mk FORCE | $(OUTDIR)
	$(Q)sed \
		-e 's/@VERSION_MAJOR@/$(VERSION_MAJOR)/g' \
		-e 's/@VERSION_MINOR@/$(VERSION_MINOR)/g' \
		-e 's/@VERSION_MICRO@/$(VERSION_MICRO)/g' \
		-e 's/@VERSION@/$(VERSION)/g' \
		-e 's/@GIT_SHA@/$(GIT_SHA)/g' \
		-e 's|@SOURCE_URL@|$(SOURCE_URL)|g' \
		$< > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

# Migration resource list, from the directory listing.  Regenerated on
# every build (so an added or removed file is noticed) but rewritten only
# when it changes.  A badly named file, or a number out of sequence,
# stops the build here.
$(MIGRATION_XML): FORCE | $(OUTDIR)
	$(Q)( \
		n=0; \
		prev=; \
		echo '<?xml version="1.0" encoding="UTF-8"?>'; \
		echo '<!-- generated from $(MIGRATION_DIR); do not edit -->'; \
		echo '<gresources>'; \
		echo '  <gresource prefix="/axis/migrations">'; \
		for f in $(MIGRATION_FILES); do \
			base=$${f##*/}; \
			n=$$((n + 1)); \
			want=$$(printf '%04d' $$n); \
			if ! printf '%s\n' "$$base" | grep -Eqx '[0-9]{4}_[a-z][a-z0-9_]{0,62}\.sql'; then \
				echo "$$f: bad migration file name: expected NNNN_name.sql, where NNNN is four digits and name is 1 to 63 lower-case letters, digits or '_', starting with a letter" >&2; \
				exit 1; \
			fi; \
			if [ -n "$$prev" ] && [ "$${base%%_*}" = "$${prev%%_*}" ]; then \
				echo "$$f: migration $${base%%_*} is also $(MIGRATION_DIR)/$$prev: each number is used once" >&2; \
				exit 1; \
			fi; \
			if [ "$${base%%_*}" != "$$want" ]; then \
				echo "$$f: migration numbers must start at 0001 and have no gaps: expected $$want here" >&2; \
				exit 1; \
			fi; \
			prev=$$base; \
			echo "    <file>$$base</file>"; \
		done; \
		if [ $$n -eq 0 ]; then \
			echo "$(MIGRATION_DIR): no migrations found" >&2; \
			exit 1; \
		fi; \
		echo '  </gresource>'; \
		echo '</gresources>'; \
	) > $@.tmp || { rm -f $@.tmp; exit 1; }
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

$(MIGRATION_C): $(MIGRATION_XML) $(MIGRATION_FILES)
	$(ECHO) "  GEN     $@"
	$(Q)$(GLIB_COMPILE_RESOURCES) --sourcedir=$(MIGRATION_DIR) --generate-source \
		--c-name axis_migrations --manual-register --target=$@ $<

# The header does not depend on which files there are; replacing it only
# when it changes keeps a new migration from rebuilding everything.
$(MIGRATION_H): $(MIGRATION_XML)
	$(Q)$(GLIB_COMPILE_RESOURCES) --sourcedir=$(MIGRATION_DIR) --generate-header \
		--c-name axis_migrations --manual-register --target=$@.tmp $<
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

# Web asset resource list, from the directory listing.  Regenerated on
# every build, rewritten only when it changes.  A file the server could not
# serve stops the build here: its extension must have a content type in
# src/web/axis-web-assets.c, and every path segment must be acceptable to
# axis_web_static_path_parse() (letters, digits, '-', '_' and '.', not
# starting with '.'; at most 6 segments and 200 characters).
$(WEB_XML): FORCE | $(OUTDIR)
	$(Q)( \
		n=0; \
		echo '<?xml version="1.0" encoding="UTF-8"?>'; \
		echo '<!-- generated from $(WEB_DIR); do not edit -->'; \
		echo '<gresources>'; \
		echo '  <gresource prefix="/axis/web">'; \
		for f in $(WEB_FILES); do \
			rel=$${f#$(WEB_DIR)/}; \
			n=$$((n + 1)); \
			if ! printf '%s\n' "$$rel" | grep -Eqx '([A-Za-z0-9_-][A-Za-z0-9_.-]*/){0,5}[A-Za-z0-9_-][A-Za-z0-9_.-]*'; then \
				echo "$$f: bad web asset name: path segments use letters, digits, '-', '_' and '.', never start with '.', and there are at most six" >&2; \
				exit 1; \
			fi; \
			if [ $${#rel} -gt 200 ]; then \
				echo "$$f: web asset path is longer than 200 characters" >&2; \
				exit 1; \
			fi; \
			case "$$rel" in \
				*.js|*.css|*.svg|*.png|*.ico|*.woff2|*.webmanifest) ;; \
				*) echo "$$f: no content type for this file type: add one to src/web/axis-web-assets.c and to this rule, or keep the file out of $(WEB_DIR)" >&2; exit 1 ;; \
			esac; \
			echo "    <file>$$rel</file>"; \
		done; \
		if [ $$n -eq 0 ]; then \
			echo "$(WEB_DIR): no web assets found" >&2; \
			exit 1; \
		fi; \
		echo '  </gresource>'; \
		echo '</gresources>'; \
	) > $@.tmp || { rm -f $@.tmp; exit 1; }
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

$(WEB_C): $(WEB_XML) $(WEB_FILES)
	$(ECHO) "  GEN     $@"
	$(Q)$(GLIB_COMPILE_RESOURCES) --sourcedir=$(WEB_DIR) --generate-source \
		--c-name axis_web_assets --manual-register --target=$@ $<

$(WEB_H): $(WEB_XML)
	$(Q)$(GLIB_COMPILE_RESOURCES) --sourcedir=$(WEB_DIR) --generate-header \
		--c-name axis_web_assets --manual-register --target=$@.tmp $<
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

# The definitions Axis ships, from the directory listing, regenerated on
# every build and rewritten only when it changes.  A file whose name is not
# axis-<letters, digits and '-'>.yml stops the build here: its id must be
# the same (a test checks that), and the "axis-" prefix is what tells a
# built-in definition from one an administrator installs.
$(DEFINITION_XML): FORCE | $(OUTDIR)
	$(Q)( \
		n=0; \
		echo '<?xml version="1.0" encoding="UTF-8"?>'; \
		echo '<!-- generated from $(DEFINITION_DIR); do not edit -->'; \
		echo '<gresources>'; \
		echo '  <gresource prefix="/axis/definitions">'; \
		for f in $(DEFINITION_FILES); do \
			base=$${f##*/}; \
			n=$$((n + 1)); \
			if ! printf '%s\n' "$$base" | grep -Eqx 'axis-[a-z0-9][a-z0-9-]{0,40}\.yml'; then \
				echo "$$f: bad definition file name: expected axis-NAME.yml, NAME of lower-case letters, digits and '-'" >&2; \
				exit 1; \
			fi; \
			echo "    <file>$$base</file>"; \
		done; \
		if [ $$n -eq 0 ]; then \
			echo "$(DEFINITION_DIR): no definitions found" >&2; \
			exit 1; \
		fi; \
		echo '  </gresource>'; \
		echo '</gresources>'; \
	) > $@.tmp || { rm -f $@.tmp; exit 1; }
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

$(DEFINITION_C): $(DEFINITION_XML) $(DEFINITION_FILES)
	$(ECHO) "  GEN     $@"
	$(Q)$(GLIB_COMPILE_RESOURCES) --sourcedir=$(DEFINITION_DIR) --generate-source \
		--c-name axis_definitions --manual-register --target=$@ $<

$(DEFINITION_H): $(DEFINITION_XML)
	$(Q)$(GLIB_COMPILE_RESOURCES) --sourcedir=$(DEFINITION_DIR) --generate-header \
		--c-name axis_definitions --manual-register --target=$@.tmp $<
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

# The asset version: the first 12 hex digits of a SHA-256 over the name and
# content hash of every bundled file, so adding, removing, renaming or
# editing any of them changes it.  Rewritten only when it changes.
$(WEB_VERSION_H): $(WEB_XML) $(WEB_FILES) FORCE | $(OUTDIR)
	$(Q)rels=$$(cd $(WEB_DIR) && for f in $(WEB_FILES); do echo "$${f#$(WEB_DIR)/}"; done); \
	version=$$(cd $(WEB_DIR) && sha256sum $$rels | sha256sum | cut -c1-12); \
	{ \
		echo '/* generated from $(WEB_DIR); do not edit */'; \
		echo "#define AXIS_WEB_ASSET_VERSION \"$$version\""; \
	} > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "  GEN     $@"; fi

# Fuzz targets: one program per tests/fuzz/fuzz-*.c, linked with the
# parsers in FUZZ_LIB_SOURCES compiled under the same instrumentation.
$(FUZZ_OUTDIR)/%: $(TESTDIR)/fuzz/%.c $(FUZZ_LIB_SOURCES) $(FUZZ_HEADERS)
	@mkdir -p $(dir $@)
	$(ECHO) "  FUZZCC  $@"
	$(Q)$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_LIB_SOURCES) $(FUZZ_LIBS)

.PHONY: FORCE
FORCE:
