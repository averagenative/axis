-- 0017_workers.sql - remote workers: who they are, their tokens' hashes,
-- where they see each volume
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A worker is created by an administrator, who is shown its token once.
-- Only the token's SHA-256 is kept (token_hash); the token itself is in no
-- table, no log and no page.  A revoked worker keeps its row (its name
-- stays taken and its history readable); its token is refused from the
-- moment it is revoked.
--
-- What a worker reports when it registers is kept beside it: its version,
-- its ffmpeg's version, how many jobs it runs at once, and, in
-- worker_volume, each volume whose marker it read where it is configured
-- to see it, with that path.  Its encoder capabilities go where the
-- daemon's own are (encoder_tool, encoder_capability: 0016) under the
-- machine 'worker-' || name.
--
-- The remote jobs themselves are ordinary rows of the job table, on the
-- "remote" queue, held under a lease by worker_id 'worker:' || name.

CREATE TABLE worker (
	id             bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	name           text        NOT NULL,
	token_hash     text        NOT NULL,
	created_at     timestamptz NOT NULL DEFAULT now(),
	created_by     text,
	revoked_at     timestamptz,
	revoked_by     text,
	-- The last request with its token, and from where.
	last_seen_at   timestamptz,
	last_address   text,
	-- What its last register said.
	registered_at  timestamptz,
	worker_version text,
	ffmpeg_version text,
	max_jobs       integer,

	CONSTRAINT worker_name_unique UNIQUE (name),
	CONSTRAINT worker_token_hash_unique UNIQUE (token_hash),
	CONSTRAINT worker_name_form CHECK (name ~ '^[a-z][a-z0-9-]{0,47}$'),
	CONSTRAINT worker_token_hash_form CHECK (token_hash ~ '^[0-9a-f]{64}$'),
	CONSTRAINT worker_revoked_complete CHECK ((revoked_at IS NULL) = (revoked_by IS NULL)),
	CONSTRAINT worker_address_sane
		CHECK (last_address IS NULL OR (char_length(last_address) BETWEEN 1 AND 64 AND last_address !~ '[\x01-\x1f\x7f]')),
	CONSTRAINT worker_version_form
		CHECK (worker_version IS NULL OR worker_version ~ '^[0-9A-Za-z.+~_-]{1,32}$'),
	CONSTRAINT worker_ffmpeg_version_form
		CHECK (ffmpeg_version IS NULL OR ffmpeg_version ~ '^[0-9A-Za-z.+~_-]{1,63}$'),
	CONSTRAINT worker_max_jobs_range CHECK (max_jobs IS NULL OR max_jobs BETWEEN 1 AND 16)
);

CREATE TABLE worker_volume (
	worker_id  bigint      NOT NULL REFERENCES worker (id) ON DELETE CASCADE,
	volume_id  bigint      NOT NULL REFERENCES volume (id) ON DELETE CASCADE,
	path       text        NOT NULL,
	proved_at  timestamptz NOT NULL DEFAULT now(),

	PRIMARY KEY (worker_id, volume_id),
	CONSTRAINT worker_volume_path_sane
		CHECK (char_length(path) BETWEEN 2 AND 1024 AND path ~ '^/' AND path !~ '[\x01-\x1f\x7f]')
);
