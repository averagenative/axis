-- 0053_search_cause_profile.sql - what fix-attention-actionable records
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- movie_search.cause, series_search.cause: "profile" is a search Axis made
-- because a person changed the title's quality profile (from any form):
-- every wanted item of a monitored title is searched for again at once with
-- the new profile, and the history and the releases say why.  The other
-- causes are as before; no row changes.

ALTER TABLE movie_search DROP CONSTRAINT movie_search_cause;
ALTER TABLE movie_search ADD CONSTRAINT movie_search_cause
	CHECK (cause IN ('added', 'manual', 'scheduled', 'retry', 'profile'));

ALTER TABLE series_search DROP CONSTRAINT series_search_cause;
ALTER TABLE series_search ADD CONSTRAINT series_search_cause
	CHECK (cause IN ('added', 'manual', 'scheduled', 'retry', 'profile'));
