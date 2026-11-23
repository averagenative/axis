-- 0033_library_notify.sql - telling the media servers which folder changed
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- After an import (or an upgrade, or a removal) the daemon asks each
-- configured media server to look at the one folder that changed.  An import
-- records what happened here, per server kind, and goes on: nothing about
-- the media server is waited for.  The notify job (queue "compat") sends the
-- pending rows once a folder has been quiet for a while, merges what
-- happened to one folder into one row, retries a failure a few times within
-- the hour and then marks the row dropped.  The row is kept, so that the
-- Player compatibility page can show what was sent and what was not.
--
-- folder is Axis's own absolute path (a title's folder under a root); the
-- server's name for it is made from the path map when the row is sent, so a
-- mapping fixed meanwhile is the one used.
--
-- state: pending (waiting its quiet period, or its next try), sent,
-- dropped (the tries ran out), unmapped (no path map entry or, for Plex, no
-- library holds it: nothing was sent), skipped (notifications to that server
-- were switched off, or it was no longer set up, when the row's turn came).

CREATE TABLE compat_notify (
	id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	server_kind text        NOT NULL,
	folder      text        NOT NULL,
	change      text        NOT NULL,
	state       text        NOT NULL DEFAULT 'pending',
	-- How many events were merged into the row, and how many sends failed.
	events      integer     NOT NULL DEFAULT 1,
	attempts    integer     NOT NULL DEFAULT 0,
	first_at    timestamptz NOT NULL DEFAULT now(),
	last_at     timestamptz NOT NULL DEFAULT now(),
	due_at      timestamptz NOT NULL DEFAULT now(),
	done_at     timestamptz,
	-- What the server was told the folder was, and why a row ended as it did
	-- (cleaned text; never the secret).
	sent_folder text,
	note        text,

	CONSTRAINT compat_notify_kind CHECK (server_kind ~ '^[a-z][a-z0-9]{0,15}$'),
	CONSTRAINT compat_notify_change CHECK (change IN ('created', 'modified', 'deleted')),
	CONSTRAINT compat_notify_state CHECK (state IN ('pending', 'sent', 'dropped', 'unmapped', 'skipped')),
	CONSTRAINT compat_notify_form CHECK (
		folder ~ '^/' AND char_length(folder) <= 4096 AND folder !~ '[\x01-\x1f\x7f]'
		AND (sent_folder IS NULL OR (sent_folder ~ '^/' AND char_length(sent_folder) <= 4096))
		AND (note IS NULL OR char_length(note) <= 1000)
		AND events >= 1 AND attempts >= 0),
	CONSTRAINT compat_notify_done CHECK ((state = 'pending') = (done_at IS NULL))
);

-- One waiting row per server and folder: later events merge into it.
CREATE UNIQUE INDEX compat_notify_pending ON compat_notify (server_kind, folder) WHERE state = 'pending';
CREATE INDEX compat_notify_due ON compat_notify (due_at) WHERE state = 'pending';
CREATE INDEX compat_notify_recent ON compat_notify (id DESC);
