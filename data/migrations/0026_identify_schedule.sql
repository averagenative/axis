-- 0026_identify_schedule.sql - what identification's scheduler compares, so
-- that a root where nothing could have changed is not run again
-- SPDX-License-Identifier: AGPL-3.0-or-later
--
-- A films root is run again only when, since its last run began, a scan of
-- it completed (library_root_state.complete_generation passed scan_generation),
-- a video of it was probed (media_info.probed_at after run_started_at), the
-- wait after a provider that did not answer is over, or the last run left
-- units to look at (pending).  Every check is cheap: the probe check reads
-- only the probes made since the run began, through the index below.

ALTER TABLE library_identity_root
	ADD COLUMN scan_generation bigint      NOT NULL DEFAULT 0,
	ADD COLUMN run_started_at  timestamptz,
	ADD CONSTRAINT library_identity_root_generation CHECK (scan_generation >= 0);

CREATE INDEX media_info_probed_idx ON media_info (probed_at);
