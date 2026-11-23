-- 0076_attention.sql - what a person dismissed on the page "Needs attention",
-- the index its count of blocked imports reads, and since when a download
-- client, an indexer or a disk has not been working
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The notices of the page "Needs attention" (add-attention-notices) are
-- worked out from the state Axis already keeps; none is stored.  These
-- are stored for them:
--
--   attention_dismissal  what a user dismissed: one row for each user and
--                        thing (kind, subject), with the level, the number
--                        and the "newest" the page showed when they did.  The
--                        notice stays out of that user's count while none of
--                        the three is higher than the row's.  It holds no
--                        notice, no name and no path: subject is an id as
--                        text, or '' for a count of many.
--
--   import_attention     a partial index over the imports that are blocked
--                        or partly done: the few a person must decide, counted
--                        with their highest id at every reading.
--
--   not_working_since    on indexer_source and download_client: since when the
--                        row has not been working (its state unreachable,
--                        refused or failing), NULL while it works or was not
--                        checked.  The rows held only the state and the last
--                        contact that worked, so "failing for 30 minutes"
--                        could not be told from "found failing just now by a
--                        contact days after the last".  Set by the contact
--                        that first finds it not working and kept while it
--                        stays so, whatever the reason becomes (not answering,
--                        then failing: one outage); cleared by a contact that
--                        works; begun again when the row is switched on.
--
--   unusable_since       on volume: since when the disk has been offline or
--                        not recognised, NULL while it is usable.  Kept while
--                        it stays unusable, whatever the reason becomes;
--                        state_changed_at, which the Disks pages show, still
--                        says when the state or its reason last changed.
--
-- Safe on a live database: the table is new, the columns are added without a
-- default (no rewrite) and set for a handful of rows, and the index is built
-- over a table of one row for each import, under a lock held for that long.
-- An older daemon reads none of this.

CREATE TABLE attention_dismissal (
	user_id      bigint      NOT NULL REFERENCES app_user (id) ON DELETE CASCADE,
	kind         text        NOT NULL,
	subject      text        NOT NULL DEFAULT '',
	severity     smallint    NOT NULL,
	count        bigint      NOT NULL,
	newest       bigint      NOT NULL,
	dismissed_at timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (user_id, kind, subject),
	CONSTRAINT attention_dismissal_form CHECK (
		kind ~ '^[a-z][a-z0-9_.]{0,62}$'
		AND char_length(subject) <= 64
		AND severity BETWEEN 1 AND 3
		AND count >= 0
		AND newest >= 0)
);

CREATE INDEX import_attention ON import (id) WHERE state IN ('blocked', 'partial');

ALTER TABLE indexer_source ADD COLUMN not_working_since timestamptz;
ALTER TABLE download_client ADD COLUMN not_working_since timestamptz;
ALTER TABLE volume ADD COLUMN unusable_since timestamptz;

-- A row that is not working now has been so since its last contact at the
-- latest, as far as its row can say; one never contacted, since now.
UPDATE indexer_source SET not_working_since = coalesce(last_checked_at, now())
	WHERE state IN ('unreachable', 'refused', 'failing');
UPDATE download_client SET not_working_since = coalesce(last_checked_at, now())
	WHERE state IN ('unreachable', 'refused', 'failing');
-- A disk that is away now: since its state or its reason last changed, which
-- is the earliest its row can say.
UPDATE volume SET unusable_since = state_changed_at WHERE state IN ('offline', 'mismatch');
