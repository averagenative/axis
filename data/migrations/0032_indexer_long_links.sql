-- 0032_indexer_long_links.sql - a release's fetch address may be longer
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A release's fetch address may be longer than first allowed.  A real
-- Prowlarr gives torrents from some indexers as a link on itself that
-- wraps the whole magnet link, trackers and all: 2,100 to 2,350 bytes of
-- path and query where 2,048 was the limit, so every such release was
-- left out as having no way to be fetched.  The limit becomes 4,096, the
-- same as for a magnet link.

ALTER TABLE indexer_release_secret
	DROP CONSTRAINT indexer_release_secret_form;

ALTER TABLE indexer_release_secret
	ADD CONSTRAINT indexer_release_secret_form CHECK (
		(fetch_ref IS NOT NULL OR magnet IS NOT NULL)
		AND (fetch_ref IS NULL OR (char_length(fetch_ref) BETWEEN 2 AND 4096 AND fetch_ref ~ '^/'))
		AND (magnet IS NULL OR char_length(magnet) BETWEEN 20 AND 4096));
