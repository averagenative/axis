-- 0004_module_state.sql - which modules an administrator has switched on or off
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- One row per module an administrator has changed; a module without a row
-- is in the state its description declares.  Switching a module off removes
-- nothing: its tables, settings and queued jobs stay as they are.  A row
-- for a module this build does not have is kept and ignored.

CREATE TABLE module_state (
	module_id  text        PRIMARY KEY,
	enabled    boolean     NOT NULL,
	updated_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT module_state_id_form CHECK (module_id ~ '^[a-z][a-z0-9-]{0,31}$')
);
