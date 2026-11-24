-- 0085_portal_api.sql - the request portal's API: the requester role, the
-- portal's tokens, and the kind of a session
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- (add-request-portal, slice P2.)
--
--   app_user      a third role, 'requester', below the ordinary user: an
--                 account that can ask for titles through the portal API and
--                 has no part in the engine's own interface.  The engine's
--                 login refuses it, and no session of the engine's interface
--                 is accepted for it.
--
--   web_session   kind: 'web' for a session of the engine's interface (every
--                 row there was), 'portal' for one made by a login through the
--                 portal API.  token_hash is then the SHA-256 of the session
--                 proof the portal presents; the proof itself is stored
--                 nowhere.  The kinds do not cross: a row is accepted only
--                 where its kind says.
--
--   portal_token  the tokens a request portal authenticates with, by their
--                 SHA-256 only (the token is shown once, when it is made).
--                 A revoked token stays as a row, so that the page can say
--                 when it was revoked and by whom.  last_used_at and
--                 last_address are written at most once a minute.
--
-- Only adds a table and a column and widens a check: safe on a live database.

ALTER TABLE app_user DROP CONSTRAINT app_user_role_known;
ALTER TABLE app_user ADD CONSTRAINT app_user_role_known CHECK (role IN ('admin', 'user', 'requester'));

ALTER TABLE web_session ADD COLUMN kind text NOT NULL DEFAULT 'web';
ALTER TABLE web_session ADD CONSTRAINT web_session_kind_known CHECK (kind IN ('web', 'portal'));

CREATE TABLE portal_token (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	token_hash    text        NOT NULL,
	created_at    timestamptz NOT NULL DEFAULT now(),
	created_by    text        NOT NULL,
	revoked_at    timestamptz,
	revoked_by    text,
	last_used_at  timestamptz,
	last_address  text,
	CONSTRAINT portal_token_hash_unique UNIQUE (token_hash),
	CONSTRAINT portal_token_hash_form CHECK (token_hash ~ '^[0-9a-f]{64}$'),
	CONSTRAINT portal_token_revoked_complete CHECK ((revoked_at IS NULL) = (revoked_by IS NULL)),
	CONSTRAINT portal_token_address_form
		CHECK (last_address IS NULL OR (char_length(last_address) BETWEEN 1 AND 64 AND last_address !~ '[\x01-\x1f\x7f]'))
);
