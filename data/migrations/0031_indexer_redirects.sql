-- 0031_indexer_redirects.sql - what the first run against a real Prowlarr
-- showed: indexers that redirect their downloads to their own site, and
-- searches made by identifier and by title together
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An indexer configured in Prowlarr with its "Redirect" option on answers a
-- download with a redirect to the indexer's own address, which holds the
-- indexer's key.  Axis follows that one redirect for a release's fetch, under
-- the rules of src/core/axis-http.h, unless an administrator switches it off
-- for the source (follow_redirects).  The redirect's address is never stored:
-- only whether each indexer redirects, as the source reports it.
--
-- A search by identifier also asks by title the indexers that take no
-- identifier, and merges the two answers; also_text records that it did
-- (mode stays the identifier's).

ALTER TABLE indexer_source
	ADD COLUMN follow_redirects boolean NOT NULL DEFAULT true;

ALTER TABLE indexer_source_indexer
	ADD COLUMN redirect boolean NOT NULL DEFAULT false;

ALTER TABLE indexer_search
	ADD COLUMN also_text boolean NOT NULL DEFAULT false;
