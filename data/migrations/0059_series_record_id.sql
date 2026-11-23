-- 0059_series_record_id.sql - the record a series' episode rows were last
-- synced against, so that a change of its title's record (two records of
-- one provider merged, the newer one's answer kept) keys the rows anew by
-- season and number instead of marking them gone and adding new ones
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- record_id is the title's provider_id when the rows last followed it.
-- While the title's provider_id differs, the sync takes a row whose
-- episode the provider no longer lists for the episode now listed at its
-- place (keeping its id, files, monitoring and history), until every season
-- of the series' source was fetched after the change was seen
-- (record_changed_at).

ALTER TABLE series
	ADD COLUMN record_id         text,
	ADD COLUMN record_changed_at timestamptz;

UPDATE series s SET record_id = t.provider_id FROM meta_title t WHERE t.id = s.title_id;
