-- 0060_numbering_event.sql - a change of a series' episode numbering is an
-- event of its own in the series' history (add-metadata-tvdb, V3)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An administrator changes the source a series follows (series.episode_source,
-- 0057_episode_sources.sql) through a confirmation page; the one event of the
-- change names who made it, the old and new sources and how many episodes
-- were matched, kept without a counterpart and new.  Nothing else of the
-- schema changes: the change keys the series' episode rows anew in place.

ALTER TABLE series_event DROP CONSTRAINT series_event_kind;
ALTER TABLE series_event ADD CONSTRAINT series_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'monitored',
	'episodes', 'searched', 'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal',
	'completed', 'abandoned', 'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded', 'identified',
	'unidentified', 'numbering'));
