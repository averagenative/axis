-- 0056_add_defaults.sql - where each person's Adds from a list go
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- Beside the theme (0011): what the bar above Discover's lists and the
-- search results chose for Add, per media type, for this person only:
--   {"movies": {"root": 3, "profile": 2},
--    "series": {"root": 4, "profile": 2, "monitor": "all"}}
-- An empty object means none chosen: the installation's defaults.  The
-- ids are checked when the bar is drawn and when an Add is made (a root
-- or profile that went is passed over), not here.

ALTER TABLE app_user ADD COLUMN add_defaults jsonb NOT NULL DEFAULT '{}'::jsonb;

ALTER TABLE app_user ADD CONSTRAINT app_user_add_defaults_object
	CHECK (jsonb_typeof(add_defaults) = 'object');
