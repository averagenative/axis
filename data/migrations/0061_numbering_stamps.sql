-- 0061_numbering_stamps.sql - what was found, grabbed and placed knows the
-- numbering it was read under (add-metadata-tvdb, review of V3)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A release's name and a file's name say SxxEyy in one numbering.  After a
-- series changes the source it follows (series.episode_source), what was
-- read under the old one must not be read again under the new one:
--
--   series_search.source  the numbering a search's releases were matched
--                         under: a stored release is grabbed only while the
--                         series follows it (axis-tv-search.c);
--   series_grab.source    the numbering a grab was claimed under: its
--                         download is imported only while the series follows
--                         it, else held for a person (axis-tv-import.c);
--   series_file.read_source  the numbering a file's name was read under when
--                         it was linked: identification never moves or
--                         removes a link read under another numbering than
--                         the series follows now (axis-tv-identify.c).
--
-- Each takes the series' source when it is written without one.  Rows from
-- before are stamped with the source their series follows now (every series
-- followed TMDb until V3).

ALTER TABLE series_search ADD COLUMN source text;
ALTER TABLE series_grab ADD COLUMN source text;
ALTER TABLE series_file ADD COLUMN read_source text;

UPDATE series_search x SET source = s.episode_source FROM series s WHERE s.id = x.series_id;
UPDATE series_grab x SET source = s.episode_source FROM series s WHERE s.id = x.series_id;
UPDATE series_file x SET read_source = s.episode_source FROM series s WHERE s.id = x.series_id;

ALTER TABLE series_search ADD CONSTRAINT series_search_source
	CHECK (source IS NULL OR source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$');
ALTER TABLE series_grab ADD CONSTRAINT series_grab_source
	CHECK (source IS NULL OR source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$');
ALTER TABLE series_file ADD CONSTRAINT series_file_read_source
	CHECK (read_source IS NULL OR read_source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$');

CREATE FUNCTION series_stamp_source() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.source IS NULL THEN
		SELECT episode_source INTO NEW.source FROM series WHERE id = NEW.series_id;
	END IF;
	RETURN NEW;
END $$;

CREATE TRIGGER series_search_stamps_source BEFORE INSERT ON series_search
	FOR EACH ROW EXECUTE FUNCTION series_stamp_source();
CREATE TRIGGER series_grab_stamps_source BEFORE INSERT ON series_grab
	FOR EACH ROW EXECUTE FUNCTION series_stamp_source();

-- A file's link takes the numbering its series follows when the link is
-- made; a link moved to another series is read anew.
CREATE FUNCTION series_file_stamp_source() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.read_source IS NULL OR (TG_OP = 'UPDATE' AND NEW.series_id <> OLD.series_id) THEN
		SELECT episode_source INTO NEW.read_source FROM series WHERE id = NEW.series_id;
	END IF;
	RETURN NEW;
END $$;

CREATE TRIGGER series_file_stamps_source BEFORE INSERT OR UPDATE OF series_id ON series_file
	FOR EACH ROW EXECUTE FUNCTION series_file_stamp_source();
