-- 0002_job_queue.sql - the durable job queue and its schedules
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- One row per job.  The code that changes a job (src/jobs/) does so only
-- with single conditional statements that name the expected state and,
-- for a running job, its lease token; the constraints below restate the
-- invariants that follow, so a bug that would break one fails loudly
-- instead of leaving a job in an impossible state.
--
--   queued    waiting for run_at and a worker
--   running   held by the worker with lease_token until lease_expires_at
--   succeeded, failed, cancelled   finished (finished_at is set)
--
-- attempts counts claims: 1 while the first run is going on.  A job never
-- runs more than max_attempts times.

CREATE TYPE job_state AS ENUM ('queued', 'running', 'succeeded', 'failed', 'cancelled');

CREATE TABLE job (
	id               bigint           GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	kind             text             NOT NULL,
	queue            text             NOT NULL,
	state            job_state        NOT NULL DEFAULT 'queued',
	payload          jsonb            NOT NULL DEFAULT '{}',
	priority         integer          NOT NULL DEFAULT 0,
	run_at           timestamptz      NOT NULL DEFAULT now(),
	attempts         integer          NOT NULL DEFAULT 0,
	max_attempts     integer          NOT NULL,
	-- At most one queued or running job per key (job_dedupe_key_idx).
	dedupe_key       text,
	-- How many enqueues were answered with this job instead of a new one.
	duplicates       integer          NOT NULL DEFAULT 0,
	lease_token      uuid,
	lease_expires_at timestamptz,
	worker_id        text,
	cancel_requested boolean          NOT NULL DEFAULT false,
	progress         double precision,
	progress_message text,
	last_error       text,
	created_at       timestamptz      NOT NULL DEFAULT now(),
	started_at       timestamptz,
	heartbeat_at     timestamptz,
	finished_at      timestamptz,

	CONSTRAINT job_payload_is_object CHECK (jsonb_typeof(payload) = 'object'),
	CONSTRAINT job_max_attempts_positive CHECK (max_attempts >= 1),
	CONSTRAINT job_attempts_in_range CHECK (attempts >= 0 AND attempts <= max_attempts),
	CONSTRAINT job_progress_in_range CHECK (progress >= 0 AND progress <= 1),
	CONSTRAINT job_lease_iff_running CHECK ((state = 'running') = (lease_token IS NOT NULL)),
	CONSTRAINT job_lease_complete CHECK ((lease_token IS NULL) = (lease_expires_at IS NULL)),
	CONSTRAINT job_finished_iff_final
		CHECK ((state IN ('succeeded', 'failed', 'cancelled')) = (finished_at IS NOT NULL))
);

-- The claim: WHERE state = 'queued' AND queue = $1 AND run_at <= now()
-- ORDER BY priority DESC, run_at, id LIMIT 1 FOR UPDATE SKIP LOCKED.
-- Only queued jobs are in it, so it stays small however long the history.
CREATE INDEX job_claim_idx ON job (queue, priority DESC, run_at, id)
	WHERE state = 'queued';

-- De-duplication, and the arbiter of INSERT ... ON CONFLICT (dedupe_key)
-- WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running'), whose
-- predicate must match this one.
CREATE UNIQUE INDEX job_dedupe_key_idx ON job (dedupe_key)
	WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running');

-- The lease sweeper.
CREATE INDEX job_lease_expiry_idx ON job (lease_expires_at)
	WHERE state = 'running';

-- Retention.
CREATE INDEX job_finished_idx ON job (finished_at)
	WHERE state IN ('succeeded', 'failed', 'cancelled');

-- Recurring jobs.  Each tick enqueues every enabled schedule whose
-- next_run_at has passed, with dedupe_key 'schedule:' || name (so one never
-- overlaps itself), and moves next_run_at to now() + run_interval.
CREATE TABLE job_schedule (
	name         text        PRIMARY KEY,
	kind         text        NOT NULL,
	payload      jsonb       NOT NULL DEFAULT '{}',
	run_interval interval    NOT NULL,
	enabled      boolean     NOT NULL DEFAULT true,
	next_run_at  timestamptz NOT NULL DEFAULT now(),
	-- The last tick that found it due, and the job that tick enqueued (no
	-- foreign key: finished jobs are pruned).
	last_run_at  timestamptz,
	last_job_id  bigint,
	created_at   timestamptz NOT NULL DEFAULT now(),
	updated_at   timestamptz NOT NULL DEFAULT now(),

	CONSTRAINT job_schedule_payload_is_object CHECK (jsonb_typeof(payload) = 'object'),
	CONSTRAINT job_schedule_interval_positive CHECK (run_interval > interval '0')
);

CREATE INDEX job_schedule_due_idx ON job_schedule (next_run_at)
	WHERE enabled;
