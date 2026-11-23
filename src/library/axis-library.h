/*
 * axis-library.h - the file index: scanning library roots and what later
 * work reads from it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An AxisLibrary scans library roots (axis-storage.h) into the tables of
 * data/migrations/0006_library.sql: library_file (one row per regular file
 * and symbolic link), library_scan (one row per scan) and
 * library_root_state (per root: scan interval, ignore patterns, request,
 * generations).
 *
 * The promises a scan keeps:
 *
 *  - It reads only: through a read guard of the root's volume, from the
 *    root's directory descriptor, never following a symbolic link and
 *    never leaving the root's filesystem (axis-library-walk.h).  It writes
 *    nothing to any volume.
 *  - What it sees is recorded in batches, each one transaction, so killing
 *    it at any moment leaves an index that is right about everything it
 *    says; the next scan simply carries on.
 *  - It concludes nothing from absence unless it walked the whole root
 *    without an I/O error, was not stopped, the root's directory is still
 *    the one it walked, and the volume's guard passed after the walk and
 *    again just before the conclusion is committed.  Anything it could not
 *    look at (unreadable, ignored, on another filesystem, too deep or too
 *    long) is "not examined", never absent.
 *  - A file absent from one such scan is "suspected"; absent again from a
 *    later one that started at least AXIS_LIBRARY_CONFIRM_SECONDS after it
 *    was suspected (by the database's clock, and by the daemon's boot
 *    clock when both scans ran in the same boot, so a clock change cannot
 *    confirm early), it is "missing"; AXIS_LIBRARY_RETENTION_DAYS later
 *    its row is deleted.  Seen again at any stage, it is present again,
 *    with the same row.
 *  - A file that disappears from one path while one with the same inode,
 *    size and modification time appears at another (or, when the inode
 *    differs, the only file with that size and modification time on both
 *    sides) keeps its row: a move.
 *  - The folder totals the Library pages show (library_dir, with the
 *    files' direct and recursive counts and sizes) are brought up to date
 *    by the transaction that ends a complete scan, from the index, and by
 *    nothing else: a scan that does not complete, or whose last check
 *    fails, leaves the previous ones untouched (axis-library-dirs.c).
 *  - One scan per volume at a time: a scan holds a PostgreSQL advisory
 *    lock on its volume (AXIS_LIBRARY_VOLUME_LOCK_SQL) for as long as it
 *    runs, on the connection it holds throughout.
 *
 * Every function here blocks on the database or a filesystem: call it from
 * a worker thread.
 */

#ifndef AXIS_LIBRARY_H
#define AXIS_LIBRARY_H

#include <gio/gio.h>
#include <glib.h>

#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "library/axis-library-walk.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_LIBRARY_MODULE_ID               "library"

/* The scan job: one per volume (de-duplicated on the volume), scanning
 * that volume's roots that are due or requested, one after another. */
#define AXIS_LIBRARY_SCAN_KIND               "library.scan"
#define AXIS_LIBRARY_SCAN_QUEUE              "scan"
#define AXIS_LIBRARY_SCAN_ATTEMPTS           3
#define AXIS_LIBRARY_REQUEST_PRIORITY        10

/* Every minute: enqueues a scan job for each volume with a root that is
 * due (and the volume online) or requested. */
#define AXIS_LIBRARY_DUE_KIND                "library.scan-due"
#define AXIS_LIBRARY_DUE_SCHEDULE            "library.scan-due"
#define AXIS_LIBRARY_DUE_INTERVAL_SECONDS    60

/* How often a root is scanned, by default and at most and least. */
#define AXIS_LIBRARY_DEFAULT_INTERVAL_SECONDS (6 * 3600)
#define AXIS_LIBRARY_MIN_INTERVAL_SECONDS     3600
#define AXIS_LIBRARY_MAX_INTERVAL_SECONDS     (30 * 24 * 3600)

/* A scan that did not complete is tried again after this long (or the
 * interval, if shorter); one that found the volume unavailable, after this
 * long, once the volume is back. */
#define AXIS_LIBRARY_RETRY_SECONDS            3600
#define AXIS_LIBRARY_UNAVAILABLE_RETRY_SECONDS 300

#define AXIS_LIBRARY_CONFIRM_SECONDS          3600
#define AXIS_LIBRARY_RETENTION_DAYS           30

/* Scans kept per root (the last complete one is always kept too). */
#define AXIS_LIBRARY_KEEP_SCANS               50

/* While the guard passes, it is checked again this often during a walk;
 * a walk that makes no progress for the stall time is given up. */
#define AXIS_LIBRARY_GUARD_INTERVAL_MS        5000
#define AXIS_LIBRARY_STALL_MS                 120000

/* The lock a scan holds on its volume, for as long as it runs; $1 is the
 * volume's id.  Other work that must not overlap a scan of a volume can
 * take it (or its shared form) with the same key. */
#define AXIS_LIBRARY_VOLUME_LOCK_KEY          "hashtext('axis.library.volume'), $1::integer"

#define AXIS_LIBRARY_ERROR (axis_library_error_quark())

/**
 * AxisLibraryError:
 * @AXIS_LIBRARY_ERROR_BUSY: another scan of the volume is running
 * @AXIS_LIBRARY_ERROR_NOT_FOUND: no such root
 * @AXIS_LIBRARY_ERROR_INVALID: a setting is malformed (the message says
 *   how)
 * @AXIS_LIBRARY_ERROR_FAILED: something else
 */
typedef enum
{
	AXIS_LIBRARY_ERROR_BUSY,
	AXIS_LIBRARY_ERROR_NOT_FOUND,
	AXIS_LIBRARY_ERROR_INVALID,
	AXIS_LIBRARY_ERROR_FAILED
} AxisLibraryError;

GQuark
axis_library_error_quark(void);

typedef struct _AxisLibrary AxisLibrary;

/* ── Scans ──────────────────────────────────────────────────────── */

typedef enum
{
	AXIS_SCAN_RUNNING,
	AXIS_SCAN_COMPLETE,
	AXIS_SCAN_INCOMPLETE,
	AXIS_SCAN_UNAVAILABLE
} AxisScanStatus;

/* "running", "complete", "incomplete", "unavailable" (as stored) */
const gchar *
axis_scan_status_to_string(AxisScanStatus status);

gboolean
axis_scan_status_from_string(
	const gchar    *text,
	AxisScanStatus *status
);

typedef enum
{
	AXIS_SCAN_REASON_NONE,
	AXIS_SCAN_REASON_CANCELLED,
	AXIS_SCAN_REASON_SHUTDOWN,
	AXIS_SCAN_REASON_VOLUME_UNAVAILABLE,
	AXIS_SCAN_REASON_IO_ERRORS,
	AXIS_SCAN_REASON_ROOT_MISSING,
	AXIS_SCAN_REASON_ROOT_CHANGED,
	AXIS_SCAN_REASON_ROOT_EMPTY,
	AXIS_SCAN_REASON_STALLED,
	AXIS_SCAN_REASON_INTERRUPTED,
	AXIS_SCAN_REASON_FAILED,
	AXIS_SCAN_REASON_BUSY
} AxisScanReason;

/* "cancelled", ... (as stored; NULL for none) */
const gchar *
axis_scan_reason_to_string(AxisScanReason reason);

gboolean
axis_scan_reason_from_string(
	const gchar    *text,
	AxisScanReason *reason
);

/* One sentence for a page. */
const gchar *
axis_scan_reason_describe(AxisScanReason reason);

/**
 * AxisScanResult:
 *
 * What one scan did.  @walk is what the walk met; the other counts are
 * what changed in the index.  The "after" counts are the root's index
 * after a complete scan (0 otherwise).
 */
typedef struct
{
	gint64          scan_id;
	gint64          root_id;
	gint64          generation;
	AxisScanStatus  status;
	AxisScanReason  reason;
	gchar          *message;	/* nullable; sanitised */
	AxisWalkTotals  walk;
	guint64         new_files;
	guint64         changed_files;
	guint64         reappeared;
	guint64         moved;
	guint64         suspected;
	guint64         confirmed;
	guint64         removed;
	guint64         present_after;
	guint64         suspected_after;
	guint64         missing_after;
	guint64         pinned_after;
} AxisScanResult;

void
axis_scan_result_free(AxisScanResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisScanResult, axis_scan_result_free)

/**
 * AxisScanOptions:
 * @trigger: "schedule", "request" or "direct" (%NULL)
 * @job_id: the job running the scan, or 0
 * @cancellable: (nullable): stops the scan (recorded as cancelled, or as a
 *   shutdown when @is_shutdown says so)
 * @is_shutdown: (nullable): whether a stop is a shutdown
 * @progress: (nullable): called after each batch, on the calling thread
 * @data: for the callbacks
 */
typedef struct
{
	const gchar   *trigger;
	gint64         job_id;
	GCancellable  *cancellable;
	gboolean     (*is_shutdown)(gpointer data);
	void         (*progress)(gdouble fraction, const gchar *message, gpointer data);
	gpointer       data;
} AxisScanOptions;

/**
 * AxisLibraryTuning:
 *
 * For tests: smaller batches, other intervals, and hooks.  Zero fields
 * keep the defaults (a negative @racy_window_ns too).
 * @after_batch is called after batch @n (from 1) was committed;
 * @before_commit inside a transaction, just before it is committed, with
 * @stage "batch" (and @n) or "finish".
 */
typedef struct
{
	guint         batch_entries;
	gint64        racy_window_ns;
	guint         guard_interval_ms;
	guint         stall_ms;
	AxisWalkHook  before_directory;
	AxisWalkHook  after_last;
	void        (*after_batch)(guint n, gpointer data);
	void        (*before_commit)(const gchar *stage, guint n, gpointer data);
	gpointer      hook_data;
} AxisLibraryTuning;

/**
 * axis_library_new:
 * @pool: the database; must outlive the result
 * @storage: the volumes; referenced
 *
 * Returns: (transfer full): the library, reference counted
 */
AxisLibrary *
axis_library_new(
	AxisDbPool  *pool,
	AxisStorage *storage
);

AxisLibrary *
axis_library_ref(AxisLibrary *self);

void
axis_library_unref(AxisLibrary *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisLibrary, axis_library_unref)

/* Tests only, before any scan. */
void
axis_library_set_tuning(
	AxisLibrary             *self,
	const AxisLibraryTuning *tuning
);

/**
 * axis_library_scan_root:
 * @self: the library
 * @root_id: the root
 * @options: (nullable): %NULL for a direct scan without cancellation
 * @result: (out) (transfer full) (optional): what the scan did
 * @error: (nullable): return location for an error
 *
 * Scans one root now, on the calling thread (the walk on a thread of its
 * own), holding one pooled connection and the volume's lock throughout.
 * A scan that ran, whatever its outcome (complete, incomplete, the volume
 * unavailable), returns %TRUE with its result.  Fails, recording nothing,
 * with %AXIS_LIBRARY_ERROR_BUSY while another scan of the volume holds its
 * lock and %AXIS_LIBRARY_ERROR_NOT_FOUND for an unknown root; and with a
 * database error (then a scan row may have been left running, which the
 * next scan of the volume marks interrupted).
 *
 * Returns: %TRUE if the scan ran
 */
gboolean
axis_library_scan_root(
	AxisLibrary            *self,
	gint64                  root_id,
	const AxisScanOptions  *options,
	AxisScanResult        **result,
	GError                **error
);

/* ── Jobs ───────────────────────────────────────────────────────── */

/**
 * axis_library_request_scan:
 * @self: the library
 * @root_id: the root
 * @job_id: (out) (optional): the volume's scan job (a new one, or the one
 *   queued or running)
 * @error: (nullable): return location for an error
 *
 * Marks the root as requested and enqueues its volume's scan job (at a
 * higher priority), in one transaction.  A request made while the root is
 * being scanned is kept for the next scan.  Fails with
 * %AXIS_LIBRARY_ERROR_NOT_FOUND for an unknown root.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_library_request_scan(
	AxisLibrary  *self,
	gint64        root_id,
	gint64       *job_id,
	GError      **error
);

/**
 * axis_library_enqueue_due:
 * @self: the library
 * @n_enqueued: (out) (optional): how many scan jobs were new
 * @error: (nullable): return location for an error
 *
 * What the "library.scan-due" job does: a scan job for every volume that
 * has a requested root, or a root that is due while the volume is online.
 * A root is due when it was never scanned, its last scan started an
 * interval ago, or its last scan did not complete and started
 * AXIS_LIBRARY_RETRY_SECONDS ago (AXIS_LIBRARY_UNAVAILABLE_RETRY_SECONDS
 * when the volume was unavailable).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_library_enqueue_due(
	AxisLibrary  *self,
	guint        *n_enqueued,
	GError      **error
);

/* The handlers of AXIS_LIBRARY_SCAN_KIND and AXIS_LIBRARY_DUE_KIND. */
gboolean
axis_library_run_scan_job(
	AxisLibrary     *self,
	AxisJobContext  *context,
	GError         **error
);

gboolean
axis_library_run_due_job(
	AxisLibrary     *self,
	AxisJobContext  *context,
	GError         **error
);

/* ── Roots ──────────────────────────────────────────────────────── */

/**
 * axis_library_set_root_config:
 * @self: the library
 * @root_id: the root
 * @interval_seconds: AXIS_LIBRARY_MIN_INTERVAL_SECONDS to
 *   AXIS_LIBRARY_MAX_INTERVAL_SECONDS
 * @patterns: (nullable) (array zero-terminated=1): ignore patterns (see
 *   axis_library_pattern_check()); duplicates are dropped
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_LIBRARY_ERROR_INVALID (the message says why) and
 * %AXIS_LIBRARY_ERROR_NOT_FOUND.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_library_set_root_config(
	AxisLibrary         *self,
	gint64               root_id,
	guint                interval_seconds,
	const gchar *const  *patterns,
	GError             **error
);

/**
 * AxisScanRecord:
 *
 * A library_scan row.  Times are microseconds since 1970 (0 for none);
 * the "after" counts are -1 when not recorded.
 */
typedef struct
{
	gint64          id;
	gint64          generation;
	gchar          *trigger;
	AxisScanStatus  status;
	AxisScanReason  reason;
	gchar          *message;
	gint64          started_us;
	gint64          walk_finished_us;
	gint64          finished_us;
	AxisWalkTotals  walk;
	guint64         new_files;
	guint64         changed_files;
	guint64         reappeared;
	guint64         moved;
	guint64         suspected;
	guint64         confirmed;
	guint64         removed;
	gint64          present_after;
	gint64          suspected_after;
	gint64          missing_after;
	gint64          pinned_after;
} AxisScanRecord;

void
axis_scan_record_free(AxisScanRecord *record);

typedef struct
{
	gchar *kind;	/* as stored: "other_filesystem", ... */
	gchar *path;	/* stored form */
	gchar *detail;	/* nullable */
} AxisScanFinding;

/**
 * AxisRootScanView:
 * @root_id: the root
 * @interval_seconds: its scan interval
 * @patterns: its ignore patterns
 * @requested_us: when a scan was requested and not started yet, or 0
 * @last: (nullable): its latest scan
 * @last_complete: (nullable): its latest complete scan
 * @recent: (element-type AxisScanRecord): its latest scans, newest first
 * @findings: (element-type AxisScanFinding): what @last found by path
 * @invalid_names: (element-type utf8): stored paths of indexed files whose
 *   names are not valid UTF-8, the first AXIS_LIBRARY_LISTED_NAMES by path
 * @n_invalid_names: how many there are in all
 */
typedef struct
{
	gint64          root_id;
	guint           interval_seconds;
	gchar         **patterns;
	gint64          requested_us;
	AxisScanRecord *last;
	AxisScanRecord *last_complete;
	GPtrArray      *recent;
	GPtrArray      *findings;
	GPtrArray      *invalid_names;
	guint64         n_invalid_names;
} AxisRootScanView;

#define AXIS_LIBRARY_LISTED_NAMES 20

void
axis_root_scan_view_free(AxisRootScanView *view);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisRootScanView, axis_root_scan_view_free)

/* What a root's page shows: its settings, latest scans and findings. */
gboolean
axis_library_get_root_view(
	AxisLibrary       *self,
	gint64             root_id,
	guint              n_recent,
	AxisRootScanView **view,
	GError           **error
);

/* Walks given up on (a hung filesystem) whose thread has not ended. */
guint
axis_library_get_abandoned_walks(AxisLibrary *self);

G_END_DECLS

#endif /* AXIS_LIBRARY_H */
