-- 0008_library_browser.sql - what the Library pages read: folders, and the
-- indexes their lists need
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The Library pages (src/web/axis-web-browser*.c) list folders and files
-- of a library that may hold millions of files, so no page does work
-- proportional to the library: every list is a range of an index cut short
-- by a limit, and every count stops at a cap.  The rules:
--
--  - library_dir has one row per folder of a root that holds, directly or
--    below, a present file, with its direct and recursive file counts and
--    byte totals.  It is written only inside the transaction that ends a
--    complete scan (axis_library_index_complete), derived by the database
--    from the index, so a scan that does not complete leaves the totals
--    of the last complete one untouched, and a folder's totals always
--    describe one scan.  A folder with no present file below it has no
--    row (it holds nothing to count), except a root's top, which always
--    has one.  Rows keep their id from scan to scan, so a folder's page
--    can be bookmarked; a folder that disappears takes its id with it.
--  - A folder is addressed by its row id, never by its path.  Its path is
--    kept in the stored form of src/library/axis-library-name.h and is
--    what its files are found by (dir_hash, below), not something a page
--    puts in a URL.
--  - library_file gets two generated columns, so that the files directly in
--    a folder, and a file's own name, are indexable: dir_hash is the hash
--    of the file's directory path ('' for a file at the root's top) and
--    base_name its last path segment (byte-wise order, like the path).
--  - Keyset paging: each supported sort is an index whose last column is
--    the id (the tie-breaker), so that "after this row" is a range of that
--    index.  Sorts: name, size, modification time (library_file) and
--    duration, bit rate (media_info).  There are no others.
--  - The filters of the file list that the sorts cannot serve are indexed
--    on media_info: container, video codec, resolution class, HDR format,
--    audio codec.

-- The directory part of a stored path ('' for a name with no '/') and its
-- last segment.  reverse() and strpos() work on characters, as left() and
-- right() do.
CREATE FUNCTION library_dir_of(path text) RETURNS text
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT CASE WHEN strpos(path, '/') = 0 THEN ''::text
	                  ELSE left(path, length(path) - strpos(reverse(path), '/')) END $$;

CREATE FUNCTION library_base_name_of(path text) RETURNS text
	LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
	AS $$ SELECT CASE WHEN strpos(path, '/') = 0 THEN path
	                  ELSE right(path, strpos(reverse(path), '/') - 1) END $$;

ALTER TABLE library_file
	ADD COLUMN dir_hash  bytea GENERATED ALWAYS AS (library_path_hash(library_dir_of(rel_path))) STORED,
	ADD COLUMN base_name text COLLATE "C" GENERATED ALWAYS AS (library_base_name_of(rel_path)) STORED;

CREATE TABLE library_dir (
	id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	root_id      bigint NOT NULL REFERENCES library_root (id) ON DELETE CASCADE,
	-- NULL for the root's top; set once the parent's row exists.  Not a
	-- foreign key: the statements that maintain the table delete a folder
	-- together with everything below it (a folder with no file below it has
	-- no row), and checking each of a few hundred thousand rows costs more
	-- than the rest of the work.
	parent_id    bigint,
	-- The path relative to the root, stored form ('' for the top), and
	-- its last segment ('' for the top).
	path         text   COLLATE "C" NOT NULL,
	path_hash    bytea  GENERATED ALWAYS AS (library_path_hash(path)) STORED,
	name         text   COLLATE "C" NOT NULL,
	-- Present files directly in the folder, and in it and below.
	direct_files bigint NOT NULL,
	direct_bytes bigint NOT NULL,
	total_files  bigint NOT NULL,
	total_bytes  bigint NOT NULL,
	-- Direct subfolders (that have a row).
	subdirs      bigint NOT NULL,

	CONSTRAINT library_dir_counts CHECK (direct_files >= 0 AND direct_bytes >= 0
		AND total_files >= direct_files AND total_bytes >= direct_bytes AND subdirs >= 0),
	CONSTRAINT library_dir_top CHECK ((path = '') = (name = ''))
);

CREATE UNIQUE INDEX library_dir_path_idx ON library_dir (root_id, path_hash);

-- A folder's subfolders in name order, and the keyset after one of them.
CREATE INDEX library_dir_parent_idx ON library_dir (parent_id, name, id);

-- The files directly in a folder, in name order.
CREATE INDEX library_file_dir_idx ON library_file (root_id, dir_hash, base_name, id);

-- The sorts of the flat file list.
CREATE INDEX library_file_name_idx ON library_file (base_name, id);
CREATE INDEX library_file_size_idx ON library_file (size, id);
CREATE INDEX library_file_mtime_idx ON library_file (mtime_sec, id);
CREATE INDEX media_info_duration_idx ON media_info (duration_ms, file_id) WHERE duration_ms IS NOT NULL;
CREATE INDEX media_info_bitrate_idx ON media_info (bitrate, file_id) WHERE bitrate IS NOT NULL;

-- "Name contains": a trigram index on the file names, so that a search for
-- text of three characters or more reads the files that have those
-- trigrams and not every file (the filter parser refuses shorter text).
-- It needs the pg_trgm extension, which PostgreSQL ships in contrib; where
-- it cannot be created (not installed, or not allowed to this role) the
-- migration goes on without, and a name search reads the files in the
-- order of the list until its deadline (the page then says it was too slow).
DO $$
BEGIN
	CREATE EXTENSION IF NOT EXISTS pg_trgm;
	CREATE INDEX library_file_name_trgm_idx ON library_file USING gin (base_name gin_trgm_ops);
EXCEPTION WHEN OTHERS THEN
	RAISE NOTICE 'pg_trgm is not available, name searches will not be indexed: %', SQLERRM;
END
$$;

-- The filters of the flat file list.
CREATE INDEX media_info_container_idx ON media_info (container, file_id);
CREATE INDEX media_info_video_codec_idx ON media_info (video_codec, file_id) WHERE video_codec IS NOT NULL;
CREATE INDEX media_info_resolution_idx ON media_info (resolution_class, file_id) WHERE resolution_class IS NOT NULL;
CREATE INDEX media_info_hdr_idx ON media_info (hdr_format, file_id) WHERE hdr_format IS NOT NULL;
CREATE INDEX media_info_audio_codec_idx ON media_info (audio_codec, file_id) WHERE audio_codec IS NOT NULL;
