-- 0001_schema_baseline.sql - the first schema
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Engine-wide settings that live in the database rather than in axis.conf:
-- everything the daemon does not need before it can reach the database.
-- One row per setting; the value is JSON so a setting can be a number, a
-- string, a list or an object without a column per type.

CREATE TABLE setting (
	key        text        PRIMARY KEY,
	value      jsonb       NOT NULL,
	updated_at timestamptz NOT NULL DEFAULT now()
);
