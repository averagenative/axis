-- 0037_indexer_sites.sql - three public torrent sites as native indexers: YTS,
-- EZTV and The Pirate Bay
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A site with a JSON API of its own is a row of indexer_source of kind 'yts',
-- 'eztv' or 'piratebay' (torrents), at the address the administrator set (the
-- sites move between domains).  It takes no key, so it has no row in
-- indexer_source_secret; everything else is a native indexer's.  The check of
-- 0035 names the kinds that have a protocol: it now names these too.

ALTER TABLE indexer_source DROP CONSTRAINT indexer_source_native_protocol;

ALTER TABLE indexer_source
	ADD CONSTRAINT indexer_source_native_protocol CHECK (
		(kind = 'newznab' AND protocol = 'usenet')
		OR (kind IN ('torznab', 'yts', 'eztv', 'piratebay') AND protocol = 'torrent')
		OR (kind NOT IN ('newznab', 'torznab', 'yts', 'eztv', 'piratebay') AND protocol IS NULL));
