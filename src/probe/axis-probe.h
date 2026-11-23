/*
 * axis-probe.h - probing indexed media files with ffprobe
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An AxisProbe looks inside the video and audio files the scanner indexed
 * and records what it finds in media_info and media_stream
 * (data/migrations/0007_media_probe.sql; the model is axis-probe-model.h).
 * Later work decides from those rows which files may be converted and
 * their originals deleted, so the promises are strict:
 *
 *  - It reads only.  It opens each file through a read guard of its volume
 *    (axis-volume-guard.h), from the root's directory descriptor, one name
 *    at a time with O_NOFOLLOW and without leaving the root's filesystem,
 *    and hands ffprobe that descriptor.  ffprobe is never given a path: it
 *    runs with `-protocol_whitelist fd -fd 3 -i fd:`, so it can read the
 *    one file it was given and nothing else, whatever the file says (a
 *    playlist or a concat script naming another file or a URL cannot be
 *    followed: no other protocol is allowed, and the formats that follow
 *    references are not on the format whitelist either).
 *  - A probe stores a result only for the file it was started for.  The
 *    file's size, times and inode are compared with the index before the
 *    tool runs and with the open file (and its name) after; the result is
 *    stored by a statement that names the stamp it was read against, in the
 *    transaction that clears the file's `changed` flag.  Anything that
 *    does not match stores nothing and leaves the file to be probed again
 *    (after a delay that grows with each attempt, for a file still being
 *    written).
 *  - It re-checks the volume's guard after the tool has run and before it
 *    stores anything.  A volume that is, or went, offline is never a probe
 *    failure: the previous record stays as it was and the job fails with
 *    %AXIS_PROBE_ERROR_UNAVAILABLE for the queue to retry.
 *  - A file the tool cannot read is recorded as failed once (or timed out),
 *    with a cleaned message, and is not probed again until it changes,
 *    an administrator asks, or the probe version is raised.
 *  - Probes of one volume run one at a time, and no probe starts while the
 *    volume is being scanned (the scan's lock is only looked at, never
 *    held, so probing cannot make a scan fail).  A scan that starts during
 *    a probe is not held up; the stamp checks make that harmless.
 *
 * What it does not do is make ffprobe safe against a bug in ffprobe: see
 * axis-probe-run.h for the limits it is run under.
 *
 * Every function that touches the database or a filesystem blocks: call it
 * from a worker thread.
 */

#ifndef AXIS_PROBE_H
#define AXIS_PROBE_H

#include <gio/gio.h>
#include <glib.h>

#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "probe/axis-probe-model.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_PROBE_MODULE_ID           "probe"

/* One job per file, de-duplicated on the file. */
#define AXIS_PROBE_FILE_KIND           "probe.file"
#define AXIS_PROBE_QUEUE               "probe"
#define AXIS_PROBE_FILE_ATTEMPTS       8
#define AXIS_PROBE_REQUEST_PRIORITY    10
#define AXIS_PROBE_STALE_PRIORITY      (-10)

/* Every minute: enqueues probes for new and changed files, up to a number
 * of unfinished probe jobs, a volume at a time in turn, skipping volumes
 * that are offline or being scanned. */
#define AXIS_PROBE_DUE_KIND            "probe.due"
#define AXIS_PROBE_DUE_SCHEDULE        "probe.due"
#define AXIS_PROBE_DUE_INTERVAL_SECONDS 60
#define AXIS_PROBE_QUEUE_TARGET        500

/* Every ten minutes: the same for files recorded with an older probe
 * version, at low priority. */
#define AXIS_PROBE_STALE_KIND          "probe.stale"
#define AXIS_PROBE_STALE_SCHEDULE      "probe.stale"
#define AXIS_PROBE_STALE_INTERVAL_SECONDS 600

#define AXIS_PROBE_SETTING_PATH        "probe.ffprobe_path"
#define AXIS_PROBE_SETTING_TIMEOUT     "probe.timeout"
#define AXIS_PROBE_DEFAULT_TIMEOUT_SECONDS 60

/* What ffprobe may write (per call) and use: AXIS_PROBE_OUTPUT_CAP,
 * AXIS_PROBE_STDERR_CAP and AXIS_PROBE_MEMORY_LIMIT, in axis-probe-model.h
 * (axis-worker's capability check reads its outputs with the same
 * limits). */

/* How long a job waits for another probe of its volume to end. */
#define AXIS_PROBE_LOCK_WAIT_MS        120000

/* Files listed on a root's probe page. */
#define AXIS_PROBE_LISTED_FILES        50

/* An administrator's request for a whole root's failed probes. */
#define AXIS_PROBE_REQUEST_CAP         10000

/* The lock a probe holds on its volume while it runs (a different one
 * from the scan's AXIS_LIBRARY_VOLUME_LOCK_KEY); $1 is the volume's id. */
#define AXIS_PROBE_VOLUME_LOCK_KEY     "hashtext('axis.probe.volume'), $1::integer"

#define AXIS_PROBE_ERROR (axis_probe_error_quark())

/**
 * AxisProbeError:
 * @AXIS_PROBE_ERROR_BUSY: the volume is being scanned, or another probe of
 *   it did not finish in time; try again later
 * @AXIS_PROBE_ERROR_UNAVAILABLE: the volume is offline or did not answer;
 *   try again later (nothing was recorded)
 * @AXIS_PROBE_ERROR_TOOL: ffprobe is not there or cannot be started (a
 *   problem of the installation, not of the file)
 * @AXIS_PROBE_ERROR_NOT_FOUND: no such media file
 * @AXIS_PROBE_ERROR_FAILED: something else
 */
typedef enum
{
	AXIS_PROBE_ERROR_BUSY,
	AXIS_PROBE_ERROR_UNAVAILABLE,
	AXIS_PROBE_ERROR_TOOL,
	AXIS_PROBE_ERROR_NOT_FOUND,
	AXIS_PROBE_ERROR_FAILED
} AxisProbeError;

GQuark
axis_probe_error_quark(void);

typedef struct _AxisProbe AxisProbe;

/**
 * AxisProbeOutcome:
 * @AXIS_PROBE_OUTCOME_STORED: probed and recorded
 * @AXIS_PROBE_OUTCOME_FAILED: the tool could not read the file; the failure
 *   was recorded
 * @AXIS_PROBE_OUTCOME_TIMEOUT: the tool did not finish; recorded as timed out
 * @AXIS_PROBE_OUTCOME_NOTHING: nothing to do (the file is gone, is not
 *   video or audio, or has no change to probe)
 * @AXIS_PROBE_OUTCOME_DEFERRED: the file changed or vanished meanwhile;
 *   nothing was recorded and it will be tried again later
 */
typedef enum
{
	AXIS_PROBE_OUTCOME_STORED,
	AXIS_PROBE_OUTCOME_FAILED,
	AXIS_PROBE_OUTCOME_TIMEOUT,
	AXIS_PROBE_OUTCOME_NOTHING,
	AXIS_PROBE_OUTCOME_DEFERRED
} AxisProbeOutcome;

/**
 * AxisProbeTuning:
 *
 * For tests: other limits and hooks.  Zero fields keep the defaults.
 * @tool_command is the program and the arguments that come before the
 * probe's own (a test points it at `/bin/sh script`).  The hooks are
 * called on the probing thread: @after_run once the tool has ended and
 * before the file and the volume are checked again, @before_store just
 * before the result is stored.
 */
typedef struct
{
	const gchar *const *tool_command;
	guint               timeout_ms;
	gsize               output_cap;
	guint64             memory_limit;
	guint               lock_wait_ms;
	gint                version;
	void              (*after_run)(gint64 file_id, gpointer data);
	void              (*before_store)(gint64 file_id, gpointer data);
	gpointer            hook_data;
} AxisProbeTuning;

/**
 * axis_probe_new:
 * @pool: the database; must outlive the result
 * @storage: the volumes; referenced
 * @settings: (nullable): where "probe.ffprobe_path" and "probe.timeout" are
 *   read; %NULL (tests) is the defaults
 *
 * Returns: (transfer full): the probe, reference counted
 */
AxisProbe *
axis_probe_new(
	AxisDbPool   *pool,
	AxisStorage  *storage,
	AxisSettings *settings
);

AxisProbe *
axis_probe_ref(AxisProbe *self);

void
axis_probe_unref(AxisProbe *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisProbe, axis_probe_unref)

/* The module's settings, to declare in its AxisModuleInfo or add to a
 * test's store. */
const AxisSettingInfo *
axis_probe_get_setting_table(void);

/* Tests only, before any probe. */
void
axis_probe_set_tuning(
	AxisProbe             *self,
	const AxisProbeTuning *tuning
);

/**
 * axis_probe_file:
 * @self: the probe
 * @file_id: a library_file row
 * @context: (nullable): the job, for cancellation and for waiting
 * @outcome: (out): what happened
 * @error: (nullable): return location for an error
 *
 * Probes one file if it needs it (it is new or changed, an administrator
 * asked, or it was recorded with an older probe version), as the header
 * comment promises.  Fails, having recorded nothing, with
 * %AXIS_PROBE_ERROR_UNAVAILABLE, %AXIS_PROBE_ERROR_BUSY,
 * %AXIS_PROBE_ERROR_TOOL, a database error, or %G_IO_ERROR_CANCELLED.
 *
 * Returns: %TRUE if the file was dealt with (see @outcome)
 */
gboolean
axis_probe_file(
	AxisProbe         *self,
	gint64             file_id,
	AxisJobContext    *context,
	AxisProbeOutcome  *outcome,
	GError           **error
);

/**
 * axis_probe_inspect_fd:
 * @self: the probe
 * @fd: a regular file open for reading, read from its start (not consumed)
 * @size: its size
 * @info: (out) (transfer full): what the tool read: status "ok", or
 *   "failed" or "timeout" with the error (a file the tool cannot read is a
 *   result, not a failure)
 * @error: (nullable): return location for an error
 *
 * Looks inside a file that is not in the index (a download's, before it is
 * imported), with the tool run under the same limits as a probe, and
 * records nothing.  The caller opened @fd through a guard.  Fails with
 * %AXIS_PROBE_ERROR_TOOL when ffprobe is not there or cannot be started, and
 * %AXIS_PROBE_ERROR_UNAVAILABLE when it could not be stopped.
 *
 * Returns: %TRUE with @info
 */
gboolean
axis_probe_inspect_fd(
	AxisProbe       *self,
	gint             fd,
	gint64           size,
	AxisMediaInfo  **info,
	GError         **error
);

/* The handlers of AXIS_PROBE_FILE_KIND, AXIS_PROBE_DUE_KIND and
 * AXIS_PROBE_STALE_KIND. */
gboolean
axis_probe_run_file_job(
	AxisProbe       *self,
	AxisJobContext  *context,
	GError         **error
);

gboolean
axis_probe_run_due_job(
	AxisProbe       *self,
	AxisJobContext  *context,
	GError         **error
);

gboolean
axis_probe_run_stale_job(
	AxisProbe       *self,
	AxisJobContext  *context,
	GError         **error
);

/**
 * axis_probe_enqueue_due:
 * @self: the probe
 * @stale: files recorded with an older probe version (at low priority)
 *   rather than new and changed ones
 * @n_enqueued: (out) (optional): how many jobs were new
 * @error: (nullable): return location for an error
 *
 * Tops the queue of probe jobs up to %AXIS_PROBE_QUEUE_TARGET unfinished
 * ones with files that need probing, taking them a volume at a time in
 * turn.  Files of volumes that are offline or being scanned, files waiting
 * out a delay, and everything when ffprobe is not installed are left for
 * a later turn.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_probe_enqueue_due(
	AxisProbe  *self,
	gboolean    stale,
	guint      *n_enqueued,
	GError    **error
);

/**
 * axis_probe_request_file:
 * @self: the probe
 * @file_id: a file of kind video or audio
 * @job_id: (out) (optional): the probe job
 * @error: (nullable): return location for an error
 *
 * What an administrator's "probe again" does: marks the file changed,
 * forgets any delay, and enqueues its probe at a higher priority, in one
 * transaction.  Fails with %AXIS_PROBE_ERROR_NOT_FOUND for no such file or
 * one that is not present, video or audio.
 */
gboolean
axis_probe_request_file(
	AxisProbe  *self,
	gint64      file_id,
	gint64     *job_id,
	GError    **error
);

/**
 * axis_probe_request_failed:
 * @self: the probe
 * @root_id: a root
 * @n_requested: (out) (optional): how many files
 * @capped: (out) (optional): whether there were more than
 *   %AXIS_PROBE_REQUEST_CAP
 * @error: (nullable): return location for an error
 *
 * As axis_probe_request_file() for every file of the root whose probe
 * failed or timed out (up to the cap).
 */
gboolean
axis_probe_request_failed(
	AxisProbe  *self,
	gint64      root_id,
	guint      *n_requested,
	gboolean   *capped,
	GError    **error
);

/**
 * axis_probe_request_folder:
 * @self: the probe
 * @dir_id: a folder's row (library_dir)
 * @n_requested: (out) (optional): how many files
 * @capped: (out) (optional): whether there were more than
 *   %AXIS_PROBE_REQUEST_CAP
 *
 * As axis_probe_request_file() for the present video and audio files of the
 * folder and everything below it, up to the cap, in one transaction.  Only
 * the index and the job queue are written; no file is opened.  A file that
 * is already queued is not queued again (the jobs de-duplicate).  Fails
 * with %AXIS_PROBE_ERROR_NOT_FOUND for no such folder.
 */
gboolean
axis_probe_request_folder(
	AxisProbe  *self,
	gint64      dir_id,
	guint      *n_requested,
	gboolean   *capped,
	GError    **error
);

/* ── What the pages read ────────────────────────────────────────── */

/**
 * AxisFileView:
 * @file_id: the file
 * @root_id: its root
 * @volume_id: its volume
 * @rel_path: its path in the stored form of axis-library-name.h
 * @kind: "video" or "audio" (or another, for a file the probe ignores)
 * @state: "present", "suspected" or "missing"
 * @size: its size
 * @mtime_us: its modification time, microseconds since 1970
 * @pinned: whether it is pinned
 * @pinned_links: because it has more than one hard link
 * @pinned_downloads: because it is under a downloads root
 * @changed: the index says its content changed since it was last probed
 * @waiting: it is waiting to be probed
 * @delayed_reason: (nullable): why a probe was put off
 * @delayed_until_us: until when, or 0
 * @nlink: its link count
 * @name_invalid: some segment of its path is not valid UTF-8
 * @first_seen_us: when it was first indexed
 * @suspected_us: since when the last complete scan has not found it (0 if
 *   it was found)
 * @missing_us: since when it is missing (0 if it is not)
 * @link_target: (nullable): where a symbolic link points, stored form
 * @dir_id: the row of the folder it is in (library_dir), 0 if there is none
 * @info: (nullable): the media_info row with its streams, if there is one
 */
typedef struct
{
	gint64         file_id;
	gint64         root_id;
	gint64         volume_id;
	gchar         *rel_path;
	gchar         *kind;
	gchar         *state;
	gint64         size;
	gint64         mtime_us;
	gboolean       pinned;
	gboolean       pinned_links;
	gboolean       pinned_downloads;
	gboolean       changed;
	gboolean       waiting;
	gchar         *delayed_reason;
	gint64         delayed_until_us;
	gint64         nlink;
	gboolean       name_invalid;
	gint64         first_seen_us;
	gint64         suspected_us;
	gint64         missing_us;
	gchar         *link_target;
	gint64         dir_id;
	AxisMediaInfo *info;
} AxisFileView;

void
axis_file_view_free(AxisFileView *view);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisFileView, axis_file_view_free)

/* Fails with %AXIS_PROBE_ERROR_NOT_FOUND for no such file. */
gboolean
axis_probe_get_file_view(
	AxisProbe     *self,
	gint64         file_id,
	AxisFileView **view,
	GError       **error
);

/* The whole stored record of a file (its media_info row and every stream,
 * as the file page shows it), read on @conn: what a conversion plan is made
 * from.  *@info is %NULL (and TRUE is returned) when there is none. */
gboolean
axis_probe_load_info(
	AxisDbConn     *conn,
	gint64          file_id,
	AxisMediaInfo **info,
	GError        **error
);

/** One line of a root's probe page. */
typedef struct
{
	gint64   file_id;
	gchar   *rel_path;	/* stored form */
	gchar   *status;	/* "ok", "failed", "timeout" */
	gchar   *text;		/* the error, or the container */
	gint64   probed_at_us;
} AxisProbeListing;

/**
 * AxisRootProbeView:
 *
 * Counts are of the root's present files of kind video and audio.
 * @waiting are those that need probing (including delayed ones, which are
 * @delayed too); @stale are probed with an older version.
 */
typedef struct
{
	gint64     root_id;
	guint64    media_files;
	guint64    probed;
	guint64    failed;
	guint64    timed_out;
	guint64    never_probed;
	guint64    waiting;
	guint64    delayed;
	guint64    stale;
	guint64    hdr_uncertain;
	GPtrArray *failures;	/* (element-type AxisProbeListing) */
	GPtrArray *recent;	/* (element-type AxisProbeListing) */
} AxisRootProbeView;

void
axis_root_probe_view_free(AxisRootProbeView *view);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisRootProbeView, axis_root_probe_view_free)

gboolean
axis_probe_get_root_view(
	AxisProbe          *self,
	gint64              root_id,
	AxisRootProbeView **view,
	GError            **error
);

/* axis_probe_format_whitelist() is in axis-probe-model.h. */

/* How many tool processes this probe has started (tests). */
guint64
axis_probe_get_runs(AxisProbe *self);

G_END_DECLS

#endif /* AXIS_PROBE_H */
