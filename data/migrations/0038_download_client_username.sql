-- 0038_download_client_username.sql - a download client's user name
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- qBittorrent logs in with a user name and a password; SABnzbd and Deluge
-- take a key or a password alone.  The user name is not a secret (it is on
-- the client's page, and in the form that sets it), so it is a column of the
-- client; the password stays in download_client_secret.  NULL is "none",
-- which is every client there is.  The kinds are not named by any check
-- constraint (the kind column's form is), so nothing else changes.

ALTER TABLE download_client ADD COLUMN username text;

ALTER TABLE download_client ADD CONSTRAINT download_client_username_form CHECK (
	username IS NULL
	OR (char_length(username) BETWEEN 1 AND 128 AND username !~ '[\x01-\x1f\x7f]'));
