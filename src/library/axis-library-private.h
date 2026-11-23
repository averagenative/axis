/*
 * axis-library-private.h - shared by the files of src/library
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_LIBRARY_PRIVATE_H
#define AXIS_LIBRARY_PRIVATE_H

#include "library/axis-library.h"
#include "library/axis-library-name.h"
#include "library/axis-library-walk.h"

G_BEGIN_DECLS

/* Messages kept with a scan, and details kept with a finding. */
#define AXIS_LIBRARY_MESSAGE_CHARS 2000
#define AXIS_LIBRARY_DETAIL_CHARS  200

/* The memory the database may use for each sort and hash of the folder
 * totals (they hold a few tens of bytes per folder and per level of it;
 * more than this and they spill to temporary files, which is slower, not
 * wrong). */
#define AXIS_LIBRARY_DIRS_WORK_MEM "128MB"

/* Statements of a scan's last transaction may take longer than the usual
 * 60 seconds on a root with millions of files. */
#define AXIS_LIBRARY_FINISH_TIMEOUT "600s"

struct _AxisLibrary
{
	gint               ref_count;	/* atomic */
	AxisDbPool        *pool;
	AxisStorage       *storage;	/* owned reference */
	gchar             *boot_id;	/* nullable: this boot of the machine */

	GMutex             lock;
	AxisLibraryTuning  tuning;	/* (lock) */
	GHashTable        *abandoned;	/* (lock) gint64* volume -> AxisLibraryWalk* */
};

/* One scan of one root, as the index code sees it. */
typedef struct
{
	gint64    root_id;
	gint64    volume_id;
	gboolean  downloads;	/* the root's media type is downloads */
	gint64    scan_id;
	gint64    generation;
	gint64    complete_generation;	/* before this scan */
	gchar    *started_at;	/* the database's text for the scan's start */
	gchar    *boot_id;	/* nullable */
	gint64    boottime_us;	/* at the start */
	gchar   **patterns;
	gint64    previous_files;
	guint     batches;
	guint     findings;	/* stored so far (the next one's sequence) */
	guint64   new_files;
	guint64   changed_files;
	guint64   reappeared;
} AxisScanState;

void
axis_scan_state_clear(AxisScanState *state);

typedef void (*AxisLibraryCommitHook)(
	const gchar *stage,
	guint        n,
	gpointer     data
);

/* A last check made inside the final transaction, just before COMMIT. */
typedef gboolean (*AxisLibraryCommitCheck)(
	gpointer   data,
	GError   **error
);

/* ── The index (axis-library-index.c) ───────────────────────────── */

/* Takes the volume's lock on @conn if nobody holds it; *@got says. */
gboolean
axis_library_index_lock(
	AxisDbConn  *conn,
	gint64       volume_id,
	gboolean    *got,
	GError     **error
);

void
axis_library_index_unlock(
	AxisDbConn *conn,
	gint64      volume_id
);

/*
 * Starts a scan of @state->root_id (root and volume ids, downloads set by
 * the caller): marks scans of the volume left running as interrupted,
 * hands out a generation, clears a request made before now, records the
 * scan as running and fills the rest of @state.
 */
gboolean
axis_library_index_begin(
	AxisDbConn     *conn,
	AxisScanState  *state,
	const gchar    *trigger,
	gint64          job_id,
	GError        **error
);

/* Records one batch in one transaction, with the counters so far. */
gboolean
axis_library_index_write(
	AxisDbConn             *conn,
	AxisScanState          *state,
	AxisWalkBatch          *batch,
	const AxisWalkTotals   *totals,
	AxisLibraryCommitHook   hook,
	gpointer                hook_data,
	GError                **error
);

/* Whether the root has rows marked present. */
gboolean
axis_library_index_has_present(
	AxisDbConn  *conn,
	gint64       root_id,
	gboolean    *present,
	GError     **error
);

/* Ends the scan as incomplete or unavailable: no file changes state. */
gboolean
axis_library_index_end(
	AxisDbConn            *conn,
	AxisScanState         *state,
	AxisScanStatus         status,
	AxisScanReason         reason,
	const gchar           *message,
	const AxisWalkTotals  *totals,
	GError               **error
);

/*
 * Ends the scan as complete, in one transaction: moves, then suspected,
 * confirmed and removed files, the root's counts, the scan's row.  @check
 * runs last, inside the transaction: if it fails the transaction is rolled
 * back and its error returned (nothing changed).  Fills @result's index
 * counts.
 */
gboolean
axis_library_index_complete(
	AxisDbConn              *conn,
	AxisScanState           *state,
	const AxisWalkTotals    *totals,
	AxisScanResult          *result,
	AxisLibraryCommitCheck   check,
	gpointer                 check_data,
	AxisLibraryCommitHook    hook,
	gpointer                 hook_data,
	GError                 **error
);

/*
 * Brings the root's library_dir rows (folder totals, the browser's) up to
 * date from its present files.  Only for the transaction that ends a
 * complete scan: it writes nothing outside the open transaction, so
 * whatever fails leaves the last complete scan's totals as they were.
 * axis-library-dirs.c.
 */
gboolean
axis_library_index_update_dirs(
	AxisDbConn  *conn,
	gint64       root_id,
	GError     **error
);

/* The statements of axis_library_index_update_dirs(), in order ($1 the
 * root): so that a test can ask for their plans. */
extern const gchar axis_library_sql_dirs_build[];
extern const gchar axis_library_sql_dirs_write[];
extern const gchar axis_library_sql_dirs_delete[];
extern const gchar axis_library_sql_dirs_parents[];

/* Keeps a root's scans to AXIS_LIBRARY_KEEP_SCANS (and its last complete
 * one). */
gboolean
axis_library_index_prune_scans(
	AxisDbConn  *conn,
	gint64       root_id,
	GError     **error
);

/* "123" for @value; for parameters. */
gchar *
axis_library_int_text(gint64 value);

/* CLOCK_BOOTTIME in microseconds: time since this boot, suspend included,
 * unmoved by changes of the wall clock. */
gint64
axis_library_boottime_us(void);

G_END_DECLS

#endif /* AXIS_LIBRARY_PRIVATE_H */
