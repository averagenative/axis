-- 0011_user_theme.sql - each account's colour theme
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- The theme is the id of one of the built-in themes (src/web/axis-web-theme.c).
-- NULL means the account has chosen none and uses the default theme
-- (setting web.default_theme).  The database does not know which ids exist:
-- an id a later version no longer has is kept and read as the default.

ALTER TABLE app_user ADD COLUMN theme text;

ALTER TABLE app_user ADD CONSTRAINT app_user_theme_form
	CHECK (theme IS NULL OR theme ~ '^[a-z][a-z0-9_-]{0,31}$');
