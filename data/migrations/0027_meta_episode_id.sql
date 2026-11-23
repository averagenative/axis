-- 0027_meta_episode_id.sql - a provider's own identifier of each episode,
-- so that what refers to an episode (a managed series' episodes, their
-- grabs and files) follows it when the provider numbers it anew
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- An episode's place (season, number) is the provider's numbering, which
-- changes: specials move, seasons are split, double episodes are joined.
-- The provider's own id does not (TMDb's episode id).  A season's episodes
-- are written anew on every fetch (src/meta/axis-meta-store.c) and the id
-- goes with each; a provider that gives none leaves it NULL, and then a
-- place is all there is to know an episode by.  Ids are not unique here: for
-- the time between the fetches of two seasons an episode that moved between
-- them can be listed in both; whoever follows ids takes the season fetched
-- last.

ALTER TABLE meta_episode
	ADD COLUMN provider_id text,
	ADD CONSTRAINT meta_episode_provider_id CHECK (provider_id IS NULL OR provider_id ~ '^[A-Za-z0-9]{1,24}$');

CREATE INDEX meta_episode_provider_id ON meta_episode (title_id, provider_id) WHERE provider_id IS NOT NULL;
