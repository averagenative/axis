-- 0082_history_retention.sql - the indexes the deletion of old history reads
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The hourly job history.prune (src/jobs/axis-history.c) deletes the events
-- of movies and series, the grabs, the downloads and the imports that ended
-- longer ago than their settings say (history.keep_*), a bounded batch per
-- statement.  Each statement takes the oldest rows of one table after where
-- the last one stopped, and then asks, for each of them, whether anything
-- still names it.  Both need an index, or each statement would read the
-- whole table:
--
-- 1. When a row ended, with its id (the sweep's order): the events by
--    created_at, the grabs by closed_at, the downloads by finished_at.  The
--    events kept for good whatever their age (a title added or removed, what
--    a person did) are left out, so that no statement looks at them.  The
--    imports have theirs already: import_ended (0080_scale_indexes.sql).
--
-- 2. What names a grab or an import that would go.  An event names its grab
--    by a plain column; a blocklist entry names its grab, and a movie's or
--    an episode's file the import it came by, by a foreign key with ON
--    DELETE SET NULL, which without an index also makes every delete of a
--    grab or an import read the whole blocklist or file table.
--
-- Applying it: CREATE INDEX holds a lock that blocks writes to its table
-- while it builds (reads go on), and the migration is one transaction.  The
-- daemon runs migrations before it starts anything that writes.  On the
-- generated database of tests/test-db-scale.c (3,000,000 events, 257,000
-- grabs, 242,000 downloads, 292,000 imports) the eleven indexes took 1.2 s
-- in all; the largest, series_event_age, 0.8 s and 81 MB for 2,700,000
-- events (/db/scale/retention prints each one's time and size).

-- 1. Where each sweep finds the oldest rows.
CREATE INDEX series_event_age ON series_event (created_at, id)
	WHERE kind NOT IN ('added', 'removed') AND by_user IS NULL;
CREATE INDEX movie_event_age ON movie_event (created_at, id)
	WHERE kind NOT IN ('added', 'removed') AND by_user IS NULL;
CREATE INDEX series_grab_age ON series_grab (closed_at, id) WHERE closed_at IS NOT NULL;
CREATE INDEX movie_grab_age ON movie_grab (closed_at, id) WHERE closed_at IS NOT NULL;
CREATE INDEX download_age ON download (finished_at, id)
	WHERE state IN ('failed', 'gone', 'removed', 'imported') AND finished_at IS NOT NULL;

-- 2. What names a grab or an import.
CREATE INDEX series_event_grab ON series_event (grab_id) WHERE grab_id IS NOT NULL;
CREATE INDEX movie_event_grab ON movie_event (grab_id) WHERE grab_id IS NOT NULL;
CREATE INDEX series_blocklist_grab ON series_blocklist (grab_id) WHERE grab_id IS NOT NULL;
CREATE INDEX movie_blocklist_grab ON movie_blocklist (grab_id) WHERE grab_id IS NOT NULL;
CREATE INDEX series_file_import ON series_file (import_id) WHERE import_id IS NOT NULL;
CREATE INDEX movie_file_import ON movie_file (import_id) WHERE import_id IS NOT NULL;
