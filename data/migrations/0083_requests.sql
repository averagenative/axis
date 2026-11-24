-- 0083_requests.sql - requests: a person asks for a movie or for seasons of a
-- series, and an administrator decides
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- (add-request-portal, slice P1.)  What is stored is what people decided: who
-- asked for what, and whether it was approved, declined or withdrawn.  What
-- became of an approved request (downloading, in the library, failed) is never
-- stored: it is read from the records the engine already keeps of the title.
--
--   request               one row for each request: a person, a title, a kind
--                         ('movie' or 'series') and a decision.  public_id is
--                         the name it has outside the daemon: random, so that
--                         requests cannot be guessed or counted.  requested_name
--                         is the account's name when it asked, kept so that the
--                         request still says who asked when the account is gone.
--                         counted_at is when it was counted against the quota of
--                         requested_by: when it was made, or when a follower took
--                         it over.  A request carries no text a person typed:
--                         decline_reason is the administrator's, add_error is
--                         the engine's own reason, shown to administrators only.
--
--   request_season        the seasons a series request lists.  open follows the
--                         request's decision (a trigger), so that one index can
--                         allow a season of a title in one open request only.
--
--   request_follower      the people who asked for the same thing after the
--                         first: they are not refused and not counted.
--
--   request_closed_title  a title an administrator closed to requests.
--
--   app_user              the two switches (requests of a kind are approved
--                         automatically) and the two limits of a person (NULL:
--                         the installation's; 0: the person cannot ask).
--
-- One open request (waiting or approved) for a movie, and one for a season of a
-- series, is the database's rule (two partial unique indexes): two people who
-- ask at the same moment cannot both make one, whatever the code does.
--
-- Safe on a live database: the tables are new, the columns on app_user are
-- added with a constant default (no rewrite) or none, and nothing an older
-- daemon reads changes.

CREATE TABLE request (
	id                 bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	public_id          text        NOT NULL,
	title_id           bigint      NOT NULL REFERENCES meta_title (id) ON DELETE RESTRICT,
	kind               text        NOT NULL,
	requested_by       bigint      REFERENCES app_user (id) ON DELETE SET NULL,
	requested_name     text        NOT NULL,
	-- How it came: the engine's own interface, or (a later slice) the portal API.
	via                text        NOT NULL DEFAULT 'web',
	created_at         timestamptz NOT NULL,
	counted_at         timestamptz NOT NULL,
	decision           text        NOT NULL DEFAULT 'waiting',
	-- How it was approved: by an administrator, by the person's switch, or
	-- without waiting because Axis was looking for the title already.
	approved_how       text,
	-- Why it waits although the person's switch is on.
	wait_reason        text,
	decided_by         text,
	decided_at         timestamptz,
	decline_reason     text,
	-- Where an approval put the title, and the title it added or found.
	root_id            bigint      REFERENCES library_root (id) ON DELETE SET NULL,
	quality_profile_id bigint      REFERENCES quality_profile (id) ON DELETE SET NULL,
	movie_id           bigint      REFERENCES movie (id) ON DELETE SET NULL,
	series_id          bigint      REFERENCES series (id) ON DELETE SET NULL,
	added_at           timestamptz,
	add_error          text,

	CONSTRAINT request_public_id_unique UNIQUE (public_id),
	CONSTRAINT request_public_id_form CHECK (public_id ~ '^[A-Za-z0-9_-]{22}$'),
	CONSTRAINT request_kind CHECK (kind IN ('movie', 'series')),
	CONSTRAINT request_via CHECK (via IN ('web', 'portal')),
	CONSTRAINT request_decision CHECK (decision IN ('waiting', 'approved', 'declined', 'withdrawn', 'replaced')),
	CONSTRAINT request_approved_how CHECK (approved_how IS NULL OR approved_how IN ('admin', 'auto', 'existing')),
	CONSTRAINT request_wait_reason CHECK (wait_reason IS NULL
		OR wait_reason IN ('no_defaults', 'defaults_gone', 'too_many_seasons')),
	CONSTRAINT request_form CHECK (char_length(requested_name) BETWEEN 1 AND 64
		AND (decided_by IS NULL OR char_length(decided_by) BETWEEN 1 AND 200)
		AND (decline_reason IS NULL OR char_length(decline_reason) <= 300)
		AND (add_error IS NULL OR char_length(add_error) <= 500)),
	-- A request that waits has no outcome, and one that was approved says how.
	CONSTRAINT request_waiting CHECK (decision <> 'waiting'
		OR (decided_at IS NULL AND approved_how IS NULL AND added_at IS NULL AND movie_id IS NULL AND series_id IS NULL)),
	CONSTRAINT request_approved CHECK (decision <> 'approved' OR approved_how IS NOT NULL)
);

-- One open request for a movie.
CREATE UNIQUE INDEX request_one_open_movie ON request (title_id)
	WHERE kind = 'movie' AND decision IN ('waiting', 'approved');
CREATE INDEX request_title ON request (title_id);
-- The quota: a person's requests of a window.
CREATE INDEX request_person ON request (requested_by, counted_at);
-- The waiting list and its notice: the few that wait, the oldest first.
CREATE INDEX request_waiting_list ON request (created_at, id) WHERE decision = 'waiting';
-- The notice of approved requests whose title was not added: the engine gave
-- a reason, or the daemon stopped between the decision and the adding.
CREATE INDEX request_not_added ON request (id) WHERE decision = 'approved' AND added_at IS NULL;
CREATE INDEX request_movie ON request (movie_id) WHERE movie_id IS NOT NULL;
CREATE INDEX request_series ON request (series_id) WHERE series_id IS NOT NULL;

CREATE TABLE request_season (
	request_id bigint  NOT NULL REFERENCES request (id) ON DELETE CASCADE,
	title_id   bigint  NOT NULL,
	season     integer NOT NULL,
	open       boolean NOT NULL DEFAULT true,

	PRIMARY KEY (request_id, season),
	CONSTRAINT request_season_number CHECK (season BETWEEN 0 AND 10000)
);

-- One open request for a season of a series.
CREATE UNIQUE INDEX request_season_one_open ON request_season (title_id, season) WHERE open;
CREATE INDEX request_season_title ON request_season (title_id, season);

CREATE FUNCTION request_decided() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	UPDATE request_season SET open = NEW.decision IN ('waiting', 'approved')
		WHERE request_id = NEW.id AND open <> (NEW.decision IN ('waiting', 'approved'));
	RETURN NULL;
END $$;

CREATE TRIGGER request_decision_closes_seasons AFTER UPDATE OF decision ON request
	FOR EACH ROW WHEN (NEW.decision IS DISTINCT FROM OLD.decision)
	EXECUTE FUNCTION request_decided();

CREATE TABLE request_follower (
	request_id  bigint      NOT NULL REFERENCES request (id) ON DELETE CASCADE,
	user_id     bigint      NOT NULL REFERENCES app_user (id) ON DELETE CASCADE,
	followed_at timestamptz NOT NULL,

	PRIMARY KEY (request_id, user_id)
);

CREATE INDEX request_follower_user ON request_follower (user_id);

CREATE TABLE request_closed_title (
	title_id  bigint      PRIMARY KEY REFERENCES meta_title (id) ON DELETE RESTRICT,
	kind      text        NOT NULL,
	closed_by text,
	closed_at timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT request_closed_title_kind CHECK (kind IN ('movie', 'series')),
	CONSTRAINT request_closed_title_by CHECK (closed_by IS NULL OR char_length(closed_by) BETWEEN 1 AND 200)
);

ALTER TABLE app_user
	ADD COLUMN request_auto_movies   boolean NOT NULL DEFAULT false,
	ADD COLUMN request_auto_series   boolean NOT NULL DEFAULT false,
	ADD COLUMN request_movies_limit  integer,
	ADD COLUMN request_seasons_limit integer,
	ADD CONSTRAINT app_user_request_limits CHECK (
		(request_movies_limit IS NULL OR request_movies_limit BETWEEN 0 AND 1000)
		AND (request_seasons_limit IS NULL OR request_seasons_limit BETWEEN 0 AND 1000));

-- When two records of a title merge (0023_movies.sql says how), the requests
-- of the record that goes follow its identifiers to the one kept, as its movie
-- or its series does.  An open request of the record that goes, for a thing
-- the record kept has an open request for already, is replaced by that one
-- first, so that the rule of one open request holds; a title closed on either
-- record is closed on the one kept.
CREATE FUNCTION request_follow_title_merge() RETURNS trigger
	LANGUAGE plpgsql AS $$
BEGIN
	IF OLD.title_id <> NEW.title_id
	   AND NOT EXISTS (SELECT 1 FROM meta_external_id WHERE title_id = OLD.title_id) THEN
		UPDATE request r SET decision = 'replaced'
			WHERE r.title_id = OLD.title_id AND r.kind = 'movie' AND r.decision IN ('waiting', 'approved')
			  AND EXISTS (SELECT 1 FROM request k WHERE k.title_id = NEW.title_id AND k.kind = 'movie'
				AND k.decision IN ('waiting', 'approved'));
		UPDATE request r SET decision = 'replaced'
			WHERE r.title_id = OLD.title_id AND r.kind = 'series' AND r.decision IN ('waiting', 'approved')
			  AND EXISTS (SELECT 1 FROM request_season s
				JOIN request_season k ON k.title_id = NEW.title_id AND k.season = s.season AND k.open
				WHERE s.request_id = r.id);
		UPDATE request_season SET title_id = NEW.title_id WHERE title_id = OLD.title_id;
		UPDATE request SET title_id = NEW.title_id WHERE title_id = OLD.title_id;
		DELETE FROM request_closed_title c WHERE c.title_id = OLD.title_id
			AND EXISTS (SELECT 1 FROM request_closed_title k WHERE k.title_id = NEW.title_id);
		UPDATE request_closed_title SET title_id = NEW.title_id WHERE title_id = OLD.title_id;
	END IF;
	RETURN NULL;
END $$;

CREATE TRIGGER request_follows_title_merge AFTER UPDATE OF title_id ON meta_external_id
	FOR EACH ROW EXECUTE FUNCTION request_follow_title_merge();
