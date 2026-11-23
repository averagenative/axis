-- 0051_jobs_hunt_fixes.sql - what fix-jobs-hunt-findings records
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- job.earlier_runs: a job a person retried from the Jobs page is queued
-- again as itself (its id, payload, de-duplication key and priority), with
-- its attempts counted from none; how each earlier run ended is kept here,
-- oldest first, the latest 20 (AXIS_JOB_EARLIER_RUNS_KEPT): an object with
-- state ("failed" or "cancelled"), attempts, error, worker, started_at,
-- finished_at, retried_at and retried_by (the person, or null).  Every job
-- before this migration has none.
--
-- job_schedule.declared: the schedule is the build's, a module's default
-- schedule.  At every start the queue gives a schedule that is still the
-- build's the kind, payload and interval the build declares, and disables,
-- never deletes, the build's schedules that no module declares any more.  A
-- schedule a person made or changed is false: no build changes or disables
-- it.  Nothing before this migration wrote job_schedule but the build's own
-- start-up, so every existing schedule is the build's.
--
-- job_schedule.retired_at: when a start disabled the schedule because no
-- module of its build declared it; a build that declares it again enables
-- it again and clears this.  A schedule disabled any other way (by hand in
-- the database before this migration, say) has none, and stays disabled.

ALTER TABLE job
	ADD COLUMN earlier_runs jsonb NOT NULL DEFAULT '[]',
	ADD CONSTRAINT job_earlier_runs_is_array CHECK (jsonb_typeof(earlier_runs) = 'array');

ALTER TABLE job_schedule
	ADD COLUMN declared boolean NOT NULL DEFAULT true,
	ADD COLUMN retired_at timestamptz,
	ADD CONSTRAINT job_schedule_retired_is_disabled CHECK (retired_at IS NULL OR NOT enabled);
