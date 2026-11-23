-- 0030_setup_guide.sql - the setup guide's record of the steps an
-- administrator put off ("Not now") or does not use ("I don't use this")
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- One row per step that has such a choice; a step without a row has none.
-- The record is the installation's, not a browser's or an account's: every
-- administrator sees the same choices wherever they log in.  Whether a step
-- is done is never stored: the guide asks the layer behind each step every
-- time (src/web/axis-web-setup.c), so a choice here only decides where the
-- guide resumes and whether the dashboard's Getting started card still asks.
-- The step names are those of src/web/axis-web-setup-path.c.

CREATE TABLE setup_step (
	step       text        PRIMARY KEY,
	choice     text        NOT NULL,
	changed_by text        NOT NULL,
	changed_at timestamptz NOT NULL DEFAULT now(),
	CONSTRAINT setup_step_known CHECK (step IN ('titles', 'library', 'indexers', 'clients', 'quality', 'players',
		'writing', 'existing')),
	CONSTRAINT setup_step_choice CHECK (choice IN ('skipped', 'dismissed')),
	CONSTRAINT setup_step_by CHECK (char_length(changed_by) BETWEEN 1 AND 64)
);
