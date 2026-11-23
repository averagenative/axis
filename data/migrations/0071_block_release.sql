-- 0071_block_release.sql - a release a person put on a blocklist
-- (add-block-release)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- movie_blocklist.reason, series_blocklist.reason: "blocked", a release an
-- administrator put on the title's blocklist (Block this release on a manual
-- search, Block and search again on a download).  Who is created_by and when
-- is created_at, as for every entry; no new column is needed.  The other
-- reasons are a failure's ("failed", "stalled", "gone", "not_taken",
-- "unfetchable", "refused") or a download a person gave up on ("given_up").
--
-- The key of an entry is unchanged: the info-hash when it is known, else the
-- source, the indexer and the folded release name.  An entry with the reason
-- "blocked" is matched by its folded release name whatever the indexer
-- (src/acquire/axis-acquire.c, axis_acquire_blocked_by()): the name is the
-- part of name_key after its second colon, so nothing is stored for it.
--
-- Safe on a live database: each constraint is replaced in the migration's
-- transaction, and the new one allows every row the old one allowed.

ALTER TABLE movie_blocklist DROP CONSTRAINT movie_blocklist_form;
ALTER TABLE movie_blocklist ADD CONSTRAINT movie_blocklist_form CHECK (
	(key ~ '^hash:[0-9a-f]{40}$' OR (key LIKE 'name:%' AND char_length(key) <= 410))
	AND char_length(name_key) BETWEEN 1 AND 400
	AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
	AND char_length(release_name) BETWEEN 1 AND 300
	AND reason IN ('failed', 'stalled', 'gone', 'not_taken', 'unfetchable', 'refused', 'given_up', 'blocked')
	AND (reason_text IS NULL OR char_length(reason_text) <= 1000)
	AND (created_by IS NULL OR char_length(created_by) BETWEEN 1 AND 200));

ALTER TABLE series_blocklist DROP CONSTRAINT series_blocklist_form;
ALTER TABLE series_blocklist ADD CONSTRAINT series_blocklist_form CHECK (
	(key ~ '^hash:[0-9a-f]{40}$' OR (key LIKE 'name:%' AND char_length(key) <= 410))
	AND char_length(name_key) BETWEEN 1 AND 400
	AND (info_hash IS NULL OR info_hash ~ '^[0-9a-f]{40}$')
	AND char_length(release_name) BETWEEN 1 AND 300
	AND reason IN ('failed', 'stalled', 'gone', 'not_taken', 'unfetchable', 'refused', 'given_up', 'blocked')
	AND (reason_text IS NULL OR char_length(reason_text) <= 1000)
	AND (created_by IS NULL OR char_length(created_by) BETWEEN 1 AND 200));
