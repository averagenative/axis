-- 0058_empty_seasons.sql - a season a provider answers empty for, after it
-- listed episodes of it: refused while it may be the provider's fault, and
-- taken as a real removal once the same empty answer comes on separate
-- refreshes spread over a day (add-metadata-tvdb, review of V1 and V2)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- empty_answers counts the empty answers refused in a row, empty_since is
-- when the first of them came; an answer with episodes clears both.  A
-- refusal also moves the season's fetch times, so that it waits its turn
-- like any season instead of heading every refresh's batch.

ALTER TABLE meta_season
	ADD COLUMN empty_answers integer NOT NULL DEFAULT 0,
	ADD COLUMN empty_since   timestamptz,
	ADD CONSTRAINT meta_season_empty CHECK (empty_answers >= 0 AND (empty_answers = 0) = (empty_since IS NULL));
