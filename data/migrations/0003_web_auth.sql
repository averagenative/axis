-- 0003_web_auth.sql - local accounts and persistent web sessions
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- app_user holds one row per account.  Usernames are stored lower-case
-- and compared as stored, so "Alice" and "alice" are one account.  The
-- password is only ever an argon2id hash string from libsodium.
--
-- auth_epoch goes up whenever every session of the account must end
-- (password change or reset, disabling).  A session row records the epoch
-- it was created under and is only valid while the two are equal, so a
-- session written by a login that raced such a change can never be used,
-- even if its row reaches the table afterwards.
--
-- setup_admin is true only on the account first-run setup created.  The
-- unique index lets at most one row have it, which is what stops two
-- racing setup requests from both creating an administrator: each inserts
-- only when the table is empty, and the second insert to commit violates
-- the index.

CREATE TABLE app_user (
	id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	username      text        NOT NULL,
	password_hash text        NOT NULL,
	role          text        NOT NULL,
	disabled      boolean     NOT NULL DEFAULT false,
	auth_epoch    integer     NOT NULL DEFAULT 0,
	setup_admin   boolean     NOT NULL DEFAULT false,
	created_at    timestamptz NOT NULL DEFAULT now(),
	updated_at    timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT app_user_username_unique UNIQUE (username),
	CONSTRAINT app_user_username_form CHECK (username ~ '^[a-z0-9][a-z0-9._-]{0,63}$'),
	CONSTRAINT app_user_password_is_argon2id CHECK (password_hash LIKE '$argon2id$%'),
	CONSTRAINT app_user_role_known CHECK (role IN ('admin', 'user'))
);

CREATE UNIQUE INDEX app_user_one_setup_admin_idx ON app_user (setup_admin) WHERE setup_admin;

-- One row per session.  The cookie holds a random token; this table holds
-- only its SHA-256 (token_hash), from which the token cannot be
-- recovered, so nothing here can be used as a cookie.  public_id names a
-- session on the account page (to revoke it) and is not a credential
-- either.  The CSRF token is derived from the cookie's token and is not
-- stored at all.
CREATE TABLE web_session (
	token_hash     text        PRIMARY KEY,
	public_id      text        NOT NULL,
	user_id        bigint      NOT NULL REFERENCES app_user (id) ON DELETE CASCADE,
	user_epoch     integer     NOT NULL,
	created_at     timestamptz NOT NULL,
	last_seen_at   timestamptz NOT NULL,
	client_address text        NOT NULL DEFAULT '',
	user_agent     text        NOT NULL DEFAULT '',

	CONSTRAINT web_session_public_id_unique UNIQUE (public_id),
	CONSTRAINT web_session_token_hash_form CHECK (token_hash ~ '^[0-9a-f]{64}$'),
	CONSTRAINT web_session_public_id_form CHECK (public_id ~ '^[A-Za-z0-9_-]{22}$')
);

CREATE INDEX web_session_user_idx ON web_session (user_id);
