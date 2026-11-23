-- 0062_force_grab.sql - what a grab by force records, and a link a person
-- made or keeps by hand (add-force-grab-and-assign, slice F1)
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A fixed rule about a release's name (not the title asked for, more than
-- one title fits, another year, another title's identifier at the indexer,
-- no readable title) says "Axis cannot tell that this is the title".  An
-- administrator who can tell may grab such a release by force from a manual
-- search (src/acquire/axis-acquire-hand.c, axis_acquire_force_allowed()).
--
-- movie_grab.forced, series_grab.forced: the fixed rules a person set aside
-- for this grab, as its confirmation showed them ([{"k": "safety", "c": code,
-- "t": text}], the form a stored release's reasons have).
-- Not empty only for a grab by hand (set_aside = 'hand').
--
-- movie_grab.bound, series_grab.bound: the grab is bound to what the person
-- named (the movie; for a series, the episodes it claimed): its import does
-- not judge the title in a file's name again, and replaces only the library
-- files the person ticked (replaces_files, which for a bound grab holds
-- every ticked file, the ones the release is better than too).
--
-- series_grab.bound_season: the season a person named for a whole pack (its
-- files are placed by the numbers in their own names within it).  Written
-- by slice F2; here so that both modules' columns arrive in one migration.
--
-- movie_grab.close_reason: "not_imported", a grab by hand let go because
-- its import could take no file of the download (a series' grab has it
-- since 0055).
--
-- movie_file, series_file:
--   hand_how  how a person made the link: "forced" (the import of a grab by
--             force), "assigned" (files assigned by hand at import, slice
--             B), "linked" (the links page, slice C); NULL: not by a person
--   hand_by   who, hand_at when
--   kept      kept by hand: no automatic search and no Search now grabs a
--             replacement for it, until a person clears it.  Set for a
--             forced or assigned file; a person sets and clears it.
--
-- movie_event.kind, series_event.kind: "hand_link", a person's change of a
-- file's link or of its being kept.

ALTER TABLE movie_grab ADD COLUMN forced jsonb NOT NULL DEFAULT '[]';
ALTER TABLE movie_grab ADD COLUMN bound boolean NOT NULL DEFAULT false;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_forced CHECK (jsonb_typeof(forced) = 'array'
	AND (jsonb_array_length(forced) = 0 OR (set_aside = 'hand' AND bound)));

ALTER TABLE series_grab ADD COLUMN forced jsonb NOT NULL DEFAULT '[]';
ALTER TABLE series_grab ADD COLUMN bound boolean NOT NULL DEFAULT false;
ALTER TABLE series_grab ADD COLUMN bound_season integer;
ALTER TABLE series_grab ADD CONSTRAINT series_grab_forced CHECK (jsonb_typeof(forced) = 'array'
	AND (jsonb_array_length(forced) = 0 OR (set_aside = 'hand' AND bound))
	AND (bound_season IS NULL OR (bound AND bound_season BETWEEN 0 AND 10000)));

ALTER TABLE movie_grab DROP CONSTRAINT movie_grab_close_reason;
ALTER TABLE movie_grab ADD CONSTRAINT movie_grab_close_reason CHECK (close_reason IS NULL OR close_reason IN
	('not_sent', 'failed', 'stalled', 'gone', 'not_taken', 'removed', 'abandoned', 'given_up', 'imported',
	'dismissed', 'files_gone', 'not_imported'));

ALTER TABLE movie_file ADD COLUMN hand_how text;
ALTER TABLE movie_file ADD COLUMN hand_by text;
ALTER TABLE movie_file ADD COLUMN hand_at timestamptz;
ALTER TABLE movie_file ADD COLUMN kept boolean NOT NULL DEFAULT false;
ALTER TABLE movie_file ADD CONSTRAINT movie_file_hand CHECK ((hand_how IS NULL OR hand_how IN ('forced', 'assigned',
	'linked')) AND (hand_how IS NULL) = (hand_at IS NULL)
	AND (hand_by IS NULL OR char_length(hand_by) BETWEEN 1 AND 200));

ALTER TABLE series_file ADD COLUMN hand_how text;
ALTER TABLE series_file ADD COLUMN hand_by text;
ALTER TABLE series_file ADD COLUMN hand_at timestamptz;
ALTER TABLE series_file ADD COLUMN kept boolean NOT NULL DEFAULT false;
ALTER TABLE series_file ADD CONSTRAINT series_file_hand CHECK ((hand_how IS NULL OR hand_how IN ('forced', 'assigned',
	'linked')) AND (hand_how IS NULL) = (hand_at IS NULL)
	AND (hand_by IS NULL OR char_length(hand_by) BETWEEN 1 AND 200));

ALTER TABLE movie_event DROP CONSTRAINT movie_event_kind;
ALTER TABLE movie_event ADD CONSTRAINT movie_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'searched',
	'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal', 'completed', 'available',
	'abandoned', 'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded', 'identified',
	'unidentified', 'hand_link'));

ALTER TABLE series_event DROP CONSTRAINT series_event_kind;
ALTER TABLE series_event ADD CONSTRAINT series_event_kind CHECK (kind IN ('added', 'removed', 'changed', 'monitored',
	'episodes', 'searched', 'skipped', 'grabbed', 'not_sent', 'failed', 'blocklisted', 'unblocked', 'removal',
	'completed', 'abandoned', 'imported', 'import_blocked', 'import_waiting', 'import_action', 'upgraded', 'identified',
	'unidentified', 'numbering', 'hand_link'));
