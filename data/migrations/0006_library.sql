-- 0006_library.sql - the file index: every file under each library root
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The scanner (src/library/) keeps one library_file row per regular file
-- and symbolic link under a library root.  Later work bases decisions on
-- these rows (conversion deletes originals), so the rules are strict:
--
--  - Names are bytes.  rel_path is the path relative to the root in the
--    stored form of src/library/axis-library-name.h: exactly the bytes
--    when they are valid UTF-8 and hold no backslash, otherwise a
--    reversible escape.  The form is canonical, so two rows have the same
--    rel_path only if the files have the same bytes; it is compared
--    byte-wise (COLLATE "C") and never normalised.  Paths can be longer
--    than a b-tree entry may be, so uniqueness and lookups go through
--    path_hash (SHA-256 of rel_path) and prefixes through the first 600
--    characters.
--  - A row is only ever marked absent at the end of a scan that walked the
--    whole root on a volume that was verified online after the walk and
--    again just before the change was committed.  state goes present ->
--    suspected (absent in one such scan) -> missing (absent again in a
--    later one at least an hour on) -> deleted (30 days after that).  A file
--    seen again at any stage is present again, with the same row.
--  - generation numbers the scans of a root.  last_seen_generation is the
--    last scan that saw the file; unexamined_generation the last scan that
--    could not look where the file would be (unreadable, ignored, other
--    filesystem, too deep), which never counts as absence.
--  - changed is set whenever the scanner sees new content (size,
--    modification or change time to the nanosecond, inode, kind or link
--    target differ, or the previous look was too close to a modification
--    to prove anything); whoever consumes it (the probe) clears it.
--  - pinned: more than one hard link, or under a downloads root.
--
-- Nothing here is removed because a volume is offline: rows go with their
-- root (ON DELETE CASCADE), which an administrator removes while the volume
-- is online.

-- path_hash: SHA-256 of the stored path's bytes.  convert_to() is only
-- stable in general; in a UTF-8 database (the only kind Axis uses) it is
-- the identity, so this is immutable.
CREATE FUNCTION library_path_hash(path text) RETURNS bytea
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT sha256(convert_to(path, 'UTF8')) $$;

-- Per root: its scan settings and where its scans are.
CREATE TABLE library_root_state (
	root_id               bigint      PRIMARY KEY REFERENCES library_root (id) ON DELETE CASCADE,
	-- The last generation handed out, and the last one whose scan
	-- completed (0: none yet).
	generation            bigint      NOT NULL DEFAULT 0,
	complete_generation   bigint      NOT NULL DEFAULT 0,
	last_scan_id          bigint,
	last_complete_scan_id bigint,
	scan_interval_seconds integer     NOT NULL DEFAULT 21600,
	-- Names (not paths) to leave out, as globs: '*' any run, '?' one
	-- character, '\' quotes the next.
	ignore_patterns       text[]      NOT NULL DEFAULT '{}',
	-- An administrator asked for a scan; cleared when one starts.
	scan_requested_at     timestamptz,
	updated_at            timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT library_root_state_generations
		CHECK (complete_generation >= 0 AND complete_generation <= generation),
	CONSTRAINT library_root_state_interval CHECK (scan_interval_seconds BETWEEN 300 AND 2592000),
	CONSTRAINT library_root_state_patterns CHECK (cardinality(ignore_patterns) <= 64)
);

-- One row per scan of a root.  A scan that dies leaves its row 'running';
-- the next scan of the volume (which holds the volume's lock, so nothing
-- else is scanning it) marks it incomplete, reason 'interrupted'.
CREATE TABLE library_scan (
	id                  bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	root_id             bigint      NOT NULL REFERENCES library_root (id) ON DELETE CASCADE,
	generation          bigint      NOT NULL,
	job_id              bigint,
	trigger             text        NOT NULL,
	status              text        NOT NULL DEFAULT 'running',
	reason              text,
	message             text,
	-- Database clock throughout; the boot id and CLOCK_BOOTTIME of the
	-- daemon let a confirmation tell elapsed time from a clock change.
	started_at          timestamptz NOT NULL DEFAULT now(),
	walk_finished_at    timestamptz,
	finished_at         timestamptz,
	boot_id             text,
	started_boottime_us bigint,
	previous_files      bigint      NOT NULL DEFAULT 0,

	-- What the walk met.
	dirs                bigint      NOT NULL DEFAULT 0,
	files               bigint      NOT NULL DEFAULT 0,
	bytes               bigint      NOT NULL DEFAULT 0,
	symlinks            bigint      NOT NULL DEFAULT 0,
	special             bigint      NOT NULL DEFAULT 0,
	ignored             bigint      NOT NULL DEFAULT 0,
	partials            bigint      NOT NULL DEFAULT 0,
	other_fs            bigint      NOT NULL DEFAULT 0,
	too_deep            bigint      NOT NULL DEFAULT 0,
	too_long            bigint      NOT NULL DEFAULT 0,
	unreadable          bigint      NOT NULL DEFAULT 0,
	io_errors           bigint      NOT NULL DEFAULT 0,
	vanished            bigint      NOT NULL DEFAULT 0,
	replaced            bigint      NOT NULL DEFAULT 0,
	-- {"video": {"files": n, "bytes": b}, ...} for the files seen.
	kinds               jsonb       NOT NULL DEFAULT '{}',

	-- What it changed in the index.
	new_files           bigint      NOT NULL DEFAULT 0,
	changed_files       bigint      NOT NULL DEFAULT 0,
	reappeared          bigint      NOT NULL DEFAULT 0,
	moved               bigint      NOT NULL DEFAULT 0,
	suspected           bigint      NOT NULL DEFAULT 0,
	confirmed           bigint      NOT NULL DEFAULT 0,
	removed             bigint      NOT NULL DEFAULT 0,

	-- The root's index after a complete scan.
	present_after       bigint,
	suspected_after     bigint,
	missing_after       bigint,
	pinned_after        bigint,

	-- Findings met (at most AXIS_LIBRARY_MAX_FINDINGS are stored).
	findings            integer     NOT NULL DEFAULT 0,

	CONSTRAINT library_scan_generation_unique UNIQUE (root_id, generation),
	CONSTRAINT library_scan_trigger_known CHECK (trigger IN ('schedule', 'request', 'direct')),
	CONSTRAINT library_scan_status_known
		CHECK (status IN ('running', 'complete', 'incomplete', 'unavailable')),
	CONSTRAINT library_scan_finished CHECK ((status = 'running') = (finished_at IS NULL)),
	CONSTRAINT library_scan_reason_iff_not_done
		CHECK ((status IN ('running', 'complete')) = (reason IS NULL)),
	CONSTRAINT library_scan_reason_known CHECK (reason IN ('cancelled', 'shutdown',
		'volume_unavailable', 'io_errors', 'root_missing', 'root_changed', 'root_empty', 'stalled',
		'interrupted', 'failed', 'busy')),
	CONSTRAINT library_scan_message_sane
		CHECK (message IS NULL OR (char_length(message) <= 2000 AND message !~ '[\x01-\x1f\x7f]'))
);

CREATE INDEX library_scan_root_idx ON library_scan (root_id, id DESC);

-- What a scan reports by path: directories it skipped or could not read,
-- entries that changed under it, partial downloads it left out.  rel_path
-- is in the stored form, like library_file.rel_path.
CREATE TABLE library_scan_finding (
	scan_id  bigint  NOT NULL REFERENCES library_scan (id) ON DELETE CASCADE,
	seq      integer NOT NULL,
	kind     text    NOT NULL,
	rel_path text    COLLATE "C" NOT NULL,
	detail   text,

	PRIMARY KEY (scan_id, seq),
	CONSTRAINT library_scan_finding_kind_known CHECK (kind IN ('other_filesystem', 'too_deep',
		'too_long', 'unreadable', 'io_error', 'vanished', 'replaced', 'partial')),
	CONSTRAINT library_scan_finding_detail_sane
		CHECK (detail IS NULL OR (char_length(detail) <= 200 AND detail !~ '[\x01-\x1f\x7f]'))
);

CREATE TABLE library_file (
	id                    bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	root_id               bigint      NOT NULL REFERENCES library_root (id) ON DELETE CASCADE,
	volume_id             bigint      NOT NULL REFERENCES volume (id) ON DELETE CASCADE,
	rel_path              text        COLLATE "C" NOT NULL,
	path_hash             bytea       GENERATED ALWAYS AS (library_path_hash(rel_path)) STORED,
	-- Some segment of the path is not valid UTF-8 (rel_path is escaped).
	name_invalid_utf8     boolean     NOT NULL DEFAULT false,
	kind                  text        NOT NULL,
	size                  bigint      NOT NULL,
	mtime_sec             bigint      NOT NULL,
	mtime_nsec            integer     NOT NULL,
	ctime_sec             bigint      NOT NULL,
	ctime_nsec            integer     NOT NULL,
	-- st_ino as a signed 64-bit number (same bits).
	inode                 bigint      NOT NULL,
	nlink                 bigint      NOT NULL,
	-- A symbolic link's target, stored like rel_path; never followed.
	link_target           text        COLLATE "C",
	pinned                boolean     NOT NULL,
	pinned_links          boolean     NOT NULL,
	pinned_downloads      boolean     NOT NULL,
	-- The last look was within a few seconds of a modification, so the
	-- next one counts as changed even if nothing differs.
	stat_racy             boolean     NOT NULL DEFAULT false,
	state                 text        NOT NULL DEFAULT 'present',
	first_seen_generation bigint      NOT NULL,
	last_seen_generation  bigint      NOT NULL,
	unexamined_generation bigint,
	changed               boolean     NOT NULL DEFAULT true,
	changed_generation    bigint      NOT NULL,
	content_changed_at    timestamptz NOT NULL DEFAULT now(),
	created_at            timestamptz NOT NULL DEFAULT now(),
	moved_at              timestamptz,
	suspected_at          timestamptz,
	suspected_scan_id     bigint,
	suspected_boot_id     text,
	suspected_boottime_us bigint,
	missing_at            timestamptz,

	CONSTRAINT library_file_path_form CHECK (rel_path <> '' AND rel_path !~ '(^/|/$|//)'
		AND rel_path !~ '(^|/)\.\.?(/|$)'),
	CONSTRAINT library_file_kind_known CHECK (kind IN ('video', 'audio', 'subtitle', 'image', 'text',
		'other', 'symlink')),
	CONSTRAINT library_file_target_only_links CHECK (link_target IS NULL OR kind = 'symlink'),
	CONSTRAINT library_file_stat_sane CHECK (size >= 0 AND nlink >= 0
		AND mtime_nsec BETWEEN 0 AND 999999999 AND ctime_nsec BETWEEN 0 AND 999999999),
	CONSTRAINT library_file_pinned_reasons CHECK (pinned = (pinned_links OR pinned_downloads)),
	CONSTRAINT library_file_state_known CHECK (state IN ('present', 'suspected', 'missing')),
	CONSTRAINT library_file_state_times CHECK (
		(state = 'present' AND suspected_at IS NULL AND suspected_scan_id IS NULL AND missing_at IS NULL) OR
		(state = 'suspected' AND suspected_at IS NOT NULL AND suspected_scan_id IS NOT NULL
			AND missing_at IS NULL) OR
		(state = 'missing' AND suspected_at IS NOT NULL AND suspected_scan_id IS NOT NULL
			AND missing_at IS NOT NULL)),
	CONSTRAINT library_file_generations CHECK (first_seen_generation >= 1
		AND first_seen_generation <= last_seen_generation
		AND changed_generation BETWEEN first_seen_generation AND last_seen_generation)
) WITH (fillfactor = 90);

-- Lookup by root and path (and the arbiter of the scanner's upsert).
CREATE UNIQUE INDEX library_file_path_idx ON library_file (root_id, path_hash);

-- Everything under a directory: a range over the first 600 characters
-- (at most 2400 bytes, within a b-tree entry), rechecked in full.
CREATE INDEX library_file_prefix_idx ON library_file (root_id, left(rel_path, 600));

-- Hard links and moves: one inode on one volume is one file.
CREATE INDEX library_file_inode_idx ON library_file (volume_id, inode);

-- Files not seen: few, and what the confirmation steps look at.
CREATE INDEX library_file_absent_idx ON library_file (root_id, state) WHERE state <> 'present';

-- New or changed content, for whoever consumes it.
CREATE INDEX library_file_changed_idx ON library_file (root_id) WHERE changed;

-- Names that are not valid UTF-8: few, and listed on the root's page.
CREATE INDEX library_file_invalid_name_idx ON library_file (root_id) WHERE name_invalid_utf8;
