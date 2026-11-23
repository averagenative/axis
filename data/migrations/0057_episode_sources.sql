-- 0057_episode_sources.sql - a series' seasons and episodes kept per source
-- of numbering, and a managed series following one source at a time
-- (add-metadata-tvdb, V2)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A source of numbering is a provider's own seasons ("tmdb"; "tvdb",
-- TheTVDB's aired order) or another of its episode orders ("tvdb-dvd"):
-- src/meta/axis-meta-model.h, axis_meta_source_valid().  One title is one
-- record with every provider's identifiers; its seasons and episodes are
-- kept per source, side by side, each refreshed apart.  Every season stored
-- before this migration is its record provider's own, and is kept as that
-- source.
--
-- A managed series follows one source (series.episode_source): its episode
-- rows are that source's episodes, and everything downstream reads only
-- them.  Every series existing before this migration follows its record
-- provider (TMDb), and shows exactly what it did.  A series added without
-- naming one follows its record provider too (the trigger below).
--
-- What is read where nothing says which source: meta_season_followed and
-- meta_episode_followed, the seasons and episodes of the source a title is
-- followed in (its managed series' source, else its record provider's own).
-- The episode files of the series on disk (library_episode_file) are placed
-- by them.
--
-- An episode row remembers every identity it has held, per source
-- (series_episode_identity), so that changing a series' numbering and
-- changing it back maps each episode to the one it was.
--
-- A provider's claim about another scheme's identifier never merges two
-- titles that each hold their own provider's record; a claim that names an
-- identifier another title holds is kept in meta_id_claim, and shown.

-- ── Seasons and episodes per source ─────────────────────────────────

ALTER TABLE meta_season ADD COLUMN source text;
UPDATE meta_season s SET source = t.provider FROM meta_title t WHERE t.id = s.title_id;
ALTER TABLE meta_season ALTER COLUMN source SET NOT NULL;

ALTER TABLE meta_episode ADD COLUMN source text;
UPDATE meta_episode e SET source = s.source FROM meta_season s
	WHERE s.title_id = e.title_id AND s.number = e.season_number;
ALTER TABLE meta_episode ALTER COLUMN source SET NOT NULL;

ALTER TABLE meta_episode DROP CONSTRAINT meta_episode_title_id_season_number_fkey;
ALTER TABLE meta_episode DROP CONSTRAINT meta_episode_pkey;
ALTER TABLE meta_season DROP CONSTRAINT meta_season_pkey;

-- The source is the last column of each key, so that what looks an episode
-- up by its title and place (the episode files' view, the pages) keeps the
-- index it had.
ALTER TABLE meta_season
	ADD PRIMARY KEY (title_id, number, source),
	ADD CONSTRAINT meta_season_source CHECK (source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$');
ALTER TABLE meta_episode
	ADD PRIMARY KEY (title_id, season_number, number, source),
	-- A season moves with its episodes when two records of a title merge.
	ADD CONSTRAINT meta_episode_season FOREIGN KEY (title_id, season_number, source)
		REFERENCES meta_season (title_id, number, source) ON DELETE CASCADE ON UPDATE CASCADE;

-- A season or an episode stored without naming its source is its record
-- provider's own (what every season was before).
CREATE FUNCTION meta_default_source() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.source IS NULL THEN
		SELECT provider INTO NEW.source FROM meta_title WHERE id = NEW.title_id;
	END IF;
	RETURN NEW;
END $$;

CREATE TRIGGER meta_season_takes_default_source BEFORE INSERT ON meta_season
	FOR EACH ROW EXECUTE FUNCTION meta_default_source();
CREATE TRIGGER meta_episode_takes_default_source BEFORE INSERT ON meta_episode
	FOR EACH ROW EXECUTE FUNCTION meta_default_source();

DROP INDEX meta_episode_provider_id;
CREATE INDEX meta_episode_provider_id ON meta_episode (title_id, provider_id, source) WHERE provider_id IS NOT NULL;
DROP INDEX meta_episode_air_date;
CREATE INDEX meta_episode_air_date ON meta_episode (title_id, air_date, source) WHERE air_date IS NOT NULL;

-- ── A managed series follows one source ─────────────────────────────

ALTER TABLE series ADD COLUMN episode_source text;
UPDATE series s SET episode_source = t.provider FROM meta_title t WHERE t.id = s.title_id;
ALTER TABLE series ALTER COLUMN episode_source SET NOT NULL;
ALTER TABLE series ADD CONSTRAINT series_episode_source
	CHECK (episode_source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$');

-- A series added without naming its source follows its record provider,
-- and never TheTVDB by itself: a series follows TheTVDB only when that is
-- chosen for it (add-metadata-tvdb, V3), so one whose record is TheTVDB's
-- alone is refused here.
CREATE FUNCTION series_default_source() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.episode_source IS NULL THEN
		SELECT provider INTO NEW.episode_source FROM meta_title WHERE id = NEW.title_id;
		IF NEW.episode_source = 'tvdb' OR NEW.episode_source LIKE 'tvdb-%' THEN
			RAISE EXCEPTION 'a series follows TheTVDB only when it is chosen for it';
		END IF;
	END IF;
	RETURN NEW;
END $$;

CREATE TRIGGER series_takes_default_source BEFORE INSERT ON series
	FOR EACH ROW EXECUTE FUNCTION series_default_source();

-- An episode row is known by the source its identity (provider_id, place)
-- is of.  Rows of the source the series follows are its live episodes; rows
-- of another are kept, marked gone, with their links.
ALTER TABLE series_episode ADD COLUMN source text;
UPDATE series_episode e SET source = s.episode_source FROM series s WHERE s.id = e.series_id;
ALTER TABLE series_episode ALTER COLUMN source SET NOT NULL;
ALTER TABLE series_episode ADD CONSTRAINT series_episode_source
	CHECK (source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$');

DROP INDEX series_episode_provider;
CREATE UNIQUE INDEX series_episode_provider ON series_episode (series_id, source, provider_id)
	WHERE provider_id IS NOT NULL;

-- A row added without naming its source is of the source its series follows.
CREATE FUNCTION series_episode_default_source() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.source IS NULL THEN
		SELECT episode_source INTO NEW.source FROM series WHERE id = NEW.series_id;
	END IF;
	RETURN NEW;
END $$;

CREATE TRIGGER series_episode_takes_default_source BEFORE INSERT ON series_episode
	FOR EACH ROW EXECUTE FUNCTION series_episode_default_source();

-- Every identity a row has held, one per source: the provider's own id of
-- the episode (NULL: it was known by its place) and its place there.
CREATE TABLE series_episode_identity (
	episode_id    bigint      NOT NULL REFERENCES series_episode (id) ON DELETE CASCADE,
	source        text        NOT NULL,
	provider_id   text,
	season_number integer     NOT NULL,
	number        integer     NOT NULL,
	held_at       timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (episode_id, source),
	CONSTRAINT series_episode_identity_form CHECK (source ~ '^[a-z][a-z0-9]{0,15}(-[a-z]{1,16})?$'
		AND (provider_id IS NULL OR provider_id ~ '^[A-Za-z0-9]{1,24}$')
		AND season_number BETWEEN 0 AND 10000 AND number BETWEEN 0 AND 100000)
);

CREATE INDEX series_episode_identity_provider ON series_episode_identity (source, provider_id)
	WHERE provider_id IS NOT NULL;

INSERT INTO series_episode_identity (episode_id, source, provider_id, season_number, number)
	SELECT id, source, provider_id, season_number, number FROM series_episode;

-- Whatever writes a live row, the identity it holds now is remembered.
CREATE FUNCTION series_episode_remember() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.gone_at IS NULL THEN
		INSERT INTO series_episode_identity (episode_id, source, provider_id, season_number, number)
			VALUES (NEW.id, NEW.source, NEW.provider_id, NEW.season_number, NEW.number)
			ON CONFLICT (episode_id, source) DO UPDATE SET provider_id = EXCLUDED.provider_id,
				season_number = EXCLUDED.season_number, number = EXCLUDED.number, held_at = now();
	END IF;
	RETURN NULL;
END $$;

CREATE TRIGGER series_episode_remembers AFTER INSERT OR UPDATE OF source, provider_id, season_number, number, gone_at
	ON series_episode FOR EACH ROW EXECUTE FUNCTION series_episode_remember();

-- ── What is read where nothing says which source ────────────────────

-- Each a plain filter of its table (two lookups by key a row), so that a
-- query joining it keeps the plan it had with the table itself.
CREATE VIEW meta_season_followed AS
	SELECT ms.* FROM meta_season ms
	WHERE ms.source = coalesce((SELECT sr.episode_source FROM series sr WHERE sr.title_id = ms.title_id),
		(SELECT mt.provider FROM meta_title mt WHERE mt.id = ms.title_id));

CREATE VIEW meta_episode_followed AS
	SELECT me.* FROM meta_episode me
	WHERE me.source = coalesce((SELECT sr.episode_source FROM series sr WHERE sr.title_id = me.title_id),
		(SELECT mt.provider FROM meta_title mt WHERE mt.id = me.title_id));

-- The episode files of the series on disk (0045_identify_series.sql), placed
-- by the source each series is followed in.
CREATE OR REPLACE VIEW library_episode_file AS
	SELECT li.title_id, li.id AS identity_id, li.root_id, lif.file_id, me.season_number, me.number, false AS by_date
	FROM library_identity li
	JOIN library_root lr ON lr.id = li.root_id AND lr.media_type = 'tv'
	JOIN meta_title lt ON lt.id = li.title_id AND lt.kind = 'series'
	JOIN library_identity_file lif ON lif.identity_id = li.id AND lif.episode_code IS NULL
	JOIN library_file lf ON lf.id = lif.file_id AND lf.state = 'present'
	JOIN library_identity_episode lie ON lie.file_id = lif.file_id AND lie.air_date IS NULL
	JOIN meta_episode_followed me ON me.title_id = li.title_id AND me.season_number = lie.season_number
		AND me.number = lie.number
	WHERE li.state = 'linked'
	  AND (lif.title_key IS NULL OR position('|' || lif.title_key || '|' IN '|' || coalesce(lt.name_keys, '') || '|') > 0)
	UNION ALL
	SELECT li.title_id, li.id, li.root_id, lif.file_id, me.season_number, me.number, true
	FROM library_identity li
	JOIN library_root lr ON lr.id = li.root_id AND lr.media_type = 'tv'
	JOIN meta_title lt ON lt.id = li.title_id AND lt.kind = 'series'
	JOIN library_identity_file lif ON lif.identity_id = li.id AND lif.episode_code IS NULL
	JOIN library_file lf ON lf.id = lif.file_id AND lf.state = 'present'
	JOIN library_identity_episode lie ON lie.file_id = lif.file_id AND lie.air_date IS NOT NULL
	JOIN meta_episode_followed me ON me.title_id = li.title_id AND me.air_date = lie.air_date
	WHERE li.state = 'linked'
	  AND (lif.title_key IS NULL OR position('|' || lif.title_key || '|' IN '|' || coalesce(lt.name_keys, '') || '|') > 0)
	  AND NOT EXISTS (SELECT 1 FROM meta_episode m2 WHERE m2.title_id = me.title_id AND m2.source = me.source
		AND m2.air_date = me.air_date AND (m2.season_number <> me.season_number OR m2.number <> me.number));

-- ── Claims about other schemes ──────────────────────────────────────

-- What @provider's record of a title says about another scheme's
-- identifier, when that identifier is held by another title that has a
-- record of its own (so the two were not merged).  Replaced each time that
-- provider's record is stored.
CREATE TABLE meta_id_claim (
	title_id  bigint      NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	provider  text        NOT NULL,
	scheme    text        NOT NULL,
	value     text        NOT NULL,
	held_by   bigint      NOT NULL REFERENCES meta_title (id) ON DELETE CASCADE,
	noted_at  timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (title_id, provider, scheme),
	CONSTRAINT meta_id_claim_form CHECK (provider ~ '^[a-z][a-z0-9]{0,15}$' AND scheme ~ '^[a-z][a-z0-9]{1,15}$'
		AND value ~ '^[A-Za-z0-9]{1,24}$' AND held_by <> title_id)
);

CREATE INDEX meta_id_claim_held_by ON meta_id_claim (held_by);
