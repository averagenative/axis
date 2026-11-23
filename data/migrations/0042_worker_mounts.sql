-- 0042_worker_mounts.sql - a worker's mounts that do not keep file
-- identities; a worker paused after its encoder crashed; a worker of another
-- protocol version
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- worker_volume.keeps_identity: what the worker said at its last register
-- about its mount of the volume.  false: it sees the volume through a network
-- mount that does not keep file identities (sshfs, SMB): the inode numbers
-- and the times below a second it sees are not the server's, so it checks a
-- source by its size, its time to the second and samples of its content.
-- Shown on the Workers page; the daemon's own checks do not change with it.
--
-- worker.paused_*: the worker's encoder died of a signal while it converted
-- (job paused_job, what the worker said in paused_reason): the worker takes
-- no work until a person resumes it on the Workers page, or it is restarted.
-- A resume keeps the job's number in resumed_job, so that the worker's next
-- report of that same crash does not pause it again; a worker that registers
-- again has its pause cleared (a restarted worker has forgotten its crash; a
-- worker that only registered again says so at its next claim).
--
-- worker.refused_protocol: the version of the last request the daemon
-- refused for being of another protocol version, cleared by a register of
-- the daemon's own version: the page says that the worker's axis-worker does
-- not match the daemon.

ALTER TABLE worker_volume
	ADD COLUMN keeps_identity boolean NOT NULL DEFAULT true;

ALTER TABLE worker
	ADD COLUMN paused_at           timestamptz,
	ADD COLUMN paused_job          bigint,
	ADD COLUMN paused_reason       text,
	ADD COLUMN resumed_job         bigint,
	ADD COLUMN refused_protocol    integer,
	ADD COLUMN refused_protocol_at timestamptz,
	ADD CONSTRAINT worker_paused_complete
		CHECK ((paused_at IS NULL) = (paused_job IS NULL) AND (paused_job IS NULL) = (paused_reason IS NULL)),
	ADD CONSTRAINT worker_paused_job_range CHECK (paused_job IS NULL OR paused_job > 0),
	ADD CONSTRAINT worker_resumed_job_range CHECK (resumed_job IS NULL OR resumed_job > 0),
	ADD CONSTRAINT worker_paused_reason_sane
		CHECK (paused_reason IS NULL OR (char_length(paused_reason) BETWEEN 1 AND 200
			AND paused_reason !~ '[\x01-\x1f\x7f]')),
	ADD CONSTRAINT worker_refused_protocol_complete
		CHECK ((refused_protocol IS NULL) = (refused_protocol_at IS NULL)),
	ADD CONSTRAINT worker_refused_protocol_range
		CHECK (refused_protocol IS NULL OR refused_protocol BETWEEN 0 AND 1000);
