-- 0024_import.sql - imports: a completed download (or a folder an
-- administrator named) taken into the library for one title; every file
-- Axis found in it with its decision and every reason; where each placed
-- file went and how (hardlink or verified copy); the files an upgrade
-- retired into a volume's recycle folder; and what people did about an
-- import that was blocked
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The import pipeline (src/import/) is the first code that writes into a
-- library.  What these tables let it promise:
--
--  - An import is restartable.  Each placed file has its temporary name
--    (".axis-partial-<import_file.id>", in the destination folder) and the
--    inode it expects there recorded before anything is written, and the
--    step it reached after; a job that dies is run again and carries on from
--    what is on disk and here, and records once.
--  - A file is never removed from the library except by a rename into the
--    recycle folder of its own volume (".axis-recycle/" at the volume's top),
--    after its replacement is in place and verified, and only what a row of
--    import_recycle records is ever removed from there: by the cleanup job,
--    after the retention time counted from the import's completion
--    (remove_after), when the file there is still the one recorded (inode
--    and size).
--  - One import at a time per download and per title (the partial unique
--    indexes below), so two jobs never place files for one film at once.
--
-- A title is the owner module's (module + owner_id: a film of the movies
-- module); the pipeline's core knows nothing more about it.

CREATE TABLE import (
	id                  bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	-- What it is for: the module that owns the title and its own record of it
	-- (movie.id), the grab it came from (NULL for a manual import), the
	-- metadata title and a label for pages that stays when the title goes.
	module              text        NOT NULL,
	owner_id            bigint,
	grab_id             bigint,
	title_id            bigint      REFERENCES meta_title (id) ON DELETE SET NULL,
	title_label         text        NOT NULL,
	-- Where from: a recorded download (its location then), or a folder an
	-- administrator named beneath a downloads root.  source_path is the
	-- storage layer's normalised form, relative to the volume's top.
	download_id         bigint      REFERENCES download (id) ON DELETE SET NULL,
	source_volume_id    bigint      REFERENCES volume (id) ON DELETE SET NULL,
	source_path         text        NOT NULL,
	release_name        text,
	protocol            text,
	kind                text        NOT NULL DEFAULT 'auto',
	-- waiting: it cannot go on now for a reason that passes by itself (storage
	-- read-only, a volume offline or read-only, no room, a probe to retry);
	-- running: a job is placing it; blocked: a person must decide (the
	-- reasons are on its files); imported; dismissed (by a person).
	state               text        NOT NULL DEFAULT 'waiting',
	reason              text,
	reason_text         text,
	-- What blocks it, every reason: [{"c": code, "t": text, "o": true when
	-- "import anyway" can override it}].
	blockers            jsonb       NOT NULL DEFAULT '[]',
	-- Where to: the title's root then, and the folder in it (stored form of
	-- src/library/axis-library-name.h, relative to the root).
	root_id             bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	dest_volume_id      bigint      REFERENCES volume (id) ON DELETE SET NULL,
	dest_dir            text        COLLATE "C",
	-- How the film's file was placed and why, its quality and where that
	-- came from, and whether it replaced a file.
	mode                text,
	mode_text           text,
	quality             text,
	quality_text        text,
	upgrade             boolean     NOT NULL DEFAULT false,
	-- What a person allowed: the file chosen, the objections overridden
	-- (codes), by whom and when.
	chosen_position     integer,
	override_codes      text[]      NOT NULL DEFAULT '{}',
	override_by         text,
	override_at         timestamptz,
	-- After the import: a Usenet download is removed from its client with
	-- its data (pending, done, failed and tried again); a torrent is kept.
	client_removal      text,
	client_removal_text text,
	-- Automatic attempts so far (probe retries, failed placements), and the
	-- earliest time of the next.
	attempts            integer     NOT NULL DEFAULT 0,
	retry_after         timestamptz,
	requested_by        text,
	created_at          timestamptz NOT NULL DEFAULT now(),
	updated_at          timestamptz NOT NULL DEFAULT now(),
	finished_at         timestamptz,

	CONSTRAINT import_module_form CHECK (module ~ '^[a-z][a-z0-9-]{0,31}$'),
	CONSTRAINT import_kind CHECK (kind IN ('auto', 'manual')),
	CONSTRAINT import_state CHECK (state IN ('waiting', 'running', 'blocked', 'imported', 'dismissed')),
	CONSTRAINT import_protocol CHECK (protocol IS NULL OR protocol IN ('usenet', 'torrent')),
	CONSTRAINT import_mode CHECK (mode IS NULL OR mode IN ('hardlink', 'copy')),
	CONSTRAINT import_removal CHECK (client_removal IS NULL OR client_removal IN ('pending', 'done', 'failed', 'kept')),
	CONSTRAINT import_finished CHECK ((state IN ('imported', 'dismissed')) = (finished_at IS NOT NULL)),
	CONSTRAINT import_texts CHECK (
		char_length(title_label) BETWEEN 1 AND 600
		AND char_length(source_path) <= 1024 AND source_path !~ '^/' AND source_path !~ '[\x01-\x1f\x7f]'
		AND (release_name IS NULL OR char_length(release_name) BETWEEN 1 AND 300)
		AND (reason IS NULL OR reason ~ '^[a-z_]{1,40}$')
		AND (reason_text IS NULL OR char_length(reason_text) <= 2000)
		AND (dest_dir IS NULL OR (char_length(dest_dir) <= 4096 AND dest_dir !~ '(^/|/$|//)'))
		AND (mode_text IS NULL OR char_length(mode_text) <= 1000)
		AND (quality IS NULL OR quality ~ '^[a-z0-9_]{1,32}$')
		AND (quality_text IS NULL OR char_length(quality_text) <= 1000)
		AND (override_by IS NULL OR char_length(override_by) BETWEEN 1 AND 200)
		AND (requested_by IS NULL OR char_length(requested_by) BETWEEN 1 AND 200)
		AND (client_removal_text IS NULL OR char_length(client_removal_text) <= 1000)
		AND cardinality(override_codes) <= 16
		AND jsonb_typeof(blockers) = 'array'),
	CONSTRAINT import_attempts CHECK (attempts >= 0)
);

-- One open import per download and per title.
CREATE UNIQUE INDEX import_open_download ON import (download_id)
	WHERE download_id IS NOT NULL AND state IN ('waiting', 'running', 'blocked');
CREATE UNIQUE INDEX import_open_owner ON import (module, owner_id)
	WHERE owner_id IS NOT NULL AND state IN ('waiting', 'running', 'blocked');
CREATE INDEX import_recent ON import (updated_at DESC);
CREATE INDEX import_download ON import (download_id, id DESC) WHERE download_id IS NOT NULL;
CREATE INDEX import_grab ON import (module, grab_id) WHERE grab_id IS NOT NULL;
CREATE INDEX import_owner ON import (module, owner_id, id DESC);

-- Every file the walk of an import's source found (bounded), with its
-- decision and every reason, and, for the files placed, where each went and
-- the step it reached:
--   NULL       not placed (yet)
--   temp       its temporary name may exist in the destination folder
--   published  it has its final name
--   swapped    (an upgrade to the same name) the new file has the final
--              name; the old one may still have the temporary name besides
--              its name in the recycle folder
CREATE TABLE import_file (
	id              bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	import_id       bigint      NOT NULL REFERENCES import (id) ON DELETE CASCADE,
	position        integer     NOT NULL,
	-- Relative to the source (the download's folder; "" when the download
	-- is the one file), in the stored form.
	path            text        COLLATE "C" NOT NULL,
	kind            text        NOT NULL,
	size            bigint,
	inode           bigint,
	-- What it is taken as: the film, a subtitle, an extra; NULL: not taken.
	role            text,
	accepted        boolean     NOT NULL DEFAULT false,
	-- [{"c": code, "t": text, "o": true when import anyway can override it}]
	reasons         jsonb       NOT NULL DEFAULT '[]',
	duration_ms     bigint,
	width           integer,
	height          integer,
	video_codec     text,
	probe_text      text,
	-- Placing.
	dest_dir        text        COLLATE "C",
	dest_name       text        COLLATE "C",
	temp_name       text,
	temp_inode      bigint,
	step            text,
	mode            text,
	dest_inode      bigint,
	checksum        text,
	-- The file the same final name had before (an upgrade to the same name).
	replaced_inode  bigint,
	library_file_id bigint      REFERENCES library_file (id) ON DELETE SET NULL,
	note            text,

	CONSTRAINT import_file_position UNIQUE (import_id, position),
	CONSTRAINT import_file_kind CHECK (kind IN ('video', 'subtitle', 'archive', 'other', 'link', 'special')),
	CONSTRAINT import_file_role CHECK (role IS NULL OR role IN ('film', 'subtitle', 'extra')),
	CONSTRAINT import_file_step CHECK (step IS NULL OR step IN ('temp', 'published', 'swapped')),
	CONSTRAINT import_file_mode CHECK (mode IS NULL OR mode IN ('hardlink', 'copy')),
	CONSTRAINT import_file_form CHECK (
		char_length(path) <= 4096 AND path !~ '(^/|/$|//)'
		AND jsonb_typeof(reasons) = 'array'
		AND (temp_name IS NULL OR temp_name ~ '^\.axis-partial-[0-9]{1,20}$')
		AND (dest_name IS NULL OR (dest_name <> '' AND dest_name !~ '/'))
		AND (checksum IS NULL OR checksum ~ '^[0-9a-f]{64}$')
		AND (probe_text IS NULL OR char_length(probe_text) <= 1000)
		AND (note IS NULL OR char_length(note) <= 1000)
		AND (size IS NULL OR size >= 0))
);

CREATE INDEX import_file_temp ON import_file (import_id) WHERE temp_name IS NOT NULL AND step IS NOT NULL;

-- A file an upgrade retired: moved (or, for an upgrade to the same name,
-- hardlinked) from the library into ".axis-recycle/" at the top of its own
-- volume, under its library path.  Paths are stored forms relative to the
-- volume's top.  remove_after is set when the import that retired it is
-- recorded complete (its completion plus import.recycle_retention); until
-- then nothing removes it.
CREATE TABLE import_recycle (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	import_id     bigint      REFERENCES import (id) ON DELETE SET NULL,
	volume_id     bigint      NOT NULL REFERENCES volume (id) ON DELETE CASCADE,
	original_path text        COLLATE "C" NOT NULL,
	recycle_path  text        COLLATE "C",
	inode         bigint      NOT NULL,
	size          bigint      NOT NULL,
	how           text        NOT NULL,
	-- moving: being retired (it is at original_path or at recycle_path);
	-- kept: in the recycle folder; removed: removed by the cleanup; gone:
	-- not where it was recorded when the cleanup looked; changed: something
	-- else is at recycle_path (left alone).
	state         text        NOT NULL DEFAULT 'moving',
	retired_at    timestamptz NOT NULL DEFAULT now(),
	remove_after  timestamptz,
	removed_at    timestamptz,
	note          text,

	CONSTRAINT import_recycle_how CHECK (how IN ('moved', 'linked')),
	CONSTRAINT import_recycle_state CHECK (state IN ('moving', 'kept', 'removed', 'gone', 'changed')),
	CONSTRAINT import_recycle_form CHECK (
		original_path <> '' AND original_path !~ '(^/|/$|//)' AND original_path !~ '^\.axis-recycle(/|$)'
		AND (recycle_path IS NULL OR (recycle_path LIKE '.axis-recycle/%' AND recycle_path !~ '(/|^)\.\.?(/|$)'
			AND recycle_path !~ '(/$|//)'))
		AND size >= 0 AND (note IS NULL OR char_length(note) <= 1000))
);

CREATE INDEX import_recycle_due ON import_recycle (remove_after) WHERE state = 'kept';
CREATE INDEX import_recycle_import ON import_recycle (import_id);

-- What people did about an import, with who.
CREATE TABLE import_action (
	id         bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	import_id  bigint      NOT NULL REFERENCES import (id) ON DELETE CASCADE,
	action     text        NOT NULL,
	by_user    text        NOT NULL,
	text       text,
	created_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT import_action_kind CHECK (action IN ('import_anyway', 'choose_file', 'assign', 'dismiss', 'retry',
		'manual')),
	CONSTRAINT import_action_texts CHECK (char_length(by_user) BETWEEN 1 AND 200
		AND (text IS NULL OR char_length(text) <= 2000))
);

CREATE INDEX import_action_import ON import_action (import_id, id);

-- The film's link to its file learns where it came from: the import, the
-- name it came in with (read again for later upgrade decisions), and in
-- words how its quality was decided.
ALTER TABLE movie_file
	ADD COLUMN import_id    bigint REFERENCES import (id) ON DELETE SET NULL,
	ADD COLUMN release_name text,
	ADD COLUMN quality_text text,
	ADD CONSTRAINT movie_file_import_texts CHECK ((release_name IS NULL OR char_length(release_name) BETWEEN 1 AND 300)
		AND (quality_text IS NULL OR char_length(quality_text) <= 1000));

-- A grab the import pipeline took is closed as imported; one a person
-- dismissed at its import, as dismissed.
ALTER TABLE movie_grab DROP CONSTRAINT movie_grab_close_reason;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed'));

ALTER TABLE movie_event DROP CONSTRAINT movie_event_kind;
ALTER TABLE movie_event ADD CONSTRAINT movie_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'searched',
	'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal', 'completed', 'available',
	'abandoned', 'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded'));
