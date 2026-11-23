-- 0052_identify_hunt_fixes.sql - what fix-identify-hunt-findings records
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- library_identity.rules: the version of the rules that made a unit's
-- verdict (AXIS_IDENTIFY_RULES_VERSION in src/identify/axis-identify-rules.h).
-- Every unit stored before this migration was decided by the rules of
-- version 1.  A unit decided by older rules than the build's, that no person
-- decided (held is false), is looked at again once as if its folder had
-- changed, in the runs' batches: one held for review, and a series' folder
-- linked by itself by its episodes (rule 5, whose comparison with the other
-- series of its name was wrong before version 2).  A person's decision is
-- never looked at again.
--
-- library_identity.retry_at, tries: a unit whose verdict waits for the
-- metadata provider (a full record it needs was not answered, so nothing
-- final is stored: a unit not linked is in state 'waiting', a linked one
-- keeps its link) is looked at again from retry_at, each try waiting twice
-- as long as the one before, up to a day.
--
-- library_identity.doubt: a linked unit (not by a person) that was looked
-- at again and that the evidence no longer makes sure, or points to another
-- title: its link stands (only a person changes a link), and this says why
-- it is listed for a person.  NULL for every other unit.
--
-- meta_title.regional_year: a film's full record's year of its theatrical
-- release in the region, when that is not its first release's year and is
-- within two years of it (a disc or a stream decades later is not); NULL
-- for none and for every title stored before (the next fetch of its full
-- record fills it).
--
-- meta_search.lookup 'search2': the second page of a search's answer, kept
-- apart from the first (identification asks for it only when the first was
-- full and nothing on it had the name looked for).
--
-- The comparison key of a title (axis_release_title_key()) now writes the
-- letters that do not decompose as the plain letters they stand for (o for
-- the o with a stroke, d, l, ae, oe, th, h, i, t).  Keys stored with such
-- letters are made again: a title's name_keys is emptied here, and the
-- daemon's start makes them again after the migrations, before anything
-- reads them (axis_meta_fill_every_name_key()); a scope whose episode
-- files' own title keys hold one is looked at again (its fingerprint no
-- longer matches).  The letters are written with chr() so that this file
-- stays ASCII.

ALTER TABLE library_identity
	ADD COLUMN rules    smallint    NOT NULL DEFAULT 1,
	ADD COLUMN retry_at timestamptz,
	ADD COLUMN tries    smallint    NOT NULL DEFAULT 0,
	ADD COLUMN doubt    text,
	ADD CONSTRAINT library_identity_rules CHECK (rules >= 1 AND tries BETWEEN 0 AND 1000),
	ADD CONSTRAINT library_identity_doubt CHECK (doubt IS NULL OR (state = 'linked' AND char_length(doubt) <= 1000));

ALTER TABLE meta_title
	ADD COLUMN regional_year integer,
	ADD CONSTRAINT meta_title_regional_year CHECK (regional_year IS NULL OR regional_year BETWEEN 1870 AND 2200);

-- The scheduler's and the runs' look for units decided by older rules, and
-- for units whose wait for the provider is over: index ranges per root.
CREATE INDEX library_identity_again ON library_identity (root_id, rules) WHERE NOT held;
CREATE INDEX library_identity_waiting ON library_identity (root_id, retry_at) WHERE retry_at IS NOT NULL;

ALTER TABLE meta_search DROP CONSTRAINT meta_search_lookup;
ALTER TABLE meta_search ADD CONSTRAINT meta_search_lookup CHECK (lookup IN ('search', 'search2', 'find', 'title'));

UPDATE meta_title SET name_keys = NULL
	WHERE name_keys ~ ('[' || chr(248) || chr(273) || chr(240) || chr(322) || chr(230) || chr(339) || chr(254) ||
		chr(295) || chr(305) || chr(359) || chr(223) || ']');

UPDATE library_identity_scope s SET fingerprint = repeat('0', 32)
	WHERE EXISTS (SELECT 1 FROM library_identity i JOIN library_identity_file f ON f.identity_id = i.id
		WHERE i.root_id = s.root_id AND i.scope_hash = s.path_hash
		AND f.title_key ~ ('[' || chr(248) || chr(273) || chr(240) || chr(322) || chr(230) || chr(339) || chr(254) ||
			chr(295) || chr(305) || chr(359) || chr(223) || ']'));
