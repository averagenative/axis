-- 0020_download_base_path.sql - a download client served under a path
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A client behind a reverse proxy may live at http://host/sabnzbd rather
-- than at the top of its origin.  The address stays an origin (scheme, host
-- and port: the one place requests may go, and where a redirect may lead);
-- the path in front of the client's own paths is a column of its own,
-- typed by the administrator and never taken from anything a client says.
--
-- The empty string is "no prefix", which is every client there is.  A
-- prefix is '/' and segments of ordinary URL characters, each one a name
-- (not '.' or '..'), with no trailing '/'; the code checks the same
-- (download/axis-download-model.h) and this is the last line of defence.

ALTER TABLE download_client ADD COLUMN base_path text NOT NULL DEFAULT '';

ALTER TABLE download_client ADD CONSTRAINT download_client_base_path_form CHECK (
	base_path = ''
	OR (char_length(base_path) <= 128
	    AND base_path ~ '^(/[A-Za-z0-9._~-]+)+$'
	    AND base_path !~ '(^|/)\.\.?(/|$)'));
