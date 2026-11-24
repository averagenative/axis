-- 0084_title_adult.sql - a title's adult mark, kept with its record
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- (add-request-portal, before slice P2.)  A search drops the titles its
-- provider marks as adult while metadata.tmdb_adult is off, so a search never
-- stores one.  A title stored another way (a lookup by identifier, an
-- identification of a file, a list brought over) was stored without the mark,
-- and so could be asked for by its number by a person who is not an
-- administrator.  The mark is now part of the record: set whenever a
-- provider's answer about the title says so, and never cleared by an answer
-- that does not (a brief record need not carry it).
--
-- A title stored before this migration has no mark until its record is fetched
-- again; the refresh job does that for every title something refers to.
--
-- Only adds a column with a constant default: safe on a live database.

ALTER TABLE meta_title ADD COLUMN adult boolean NOT NULL DEFAULT false;
