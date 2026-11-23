/*
 * axis-probe-private.h - shared by the files of src/probe
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_PROBE_PRIVATE_H
#define AXIS_PROBE_PRIVATE_H

#include <sys/stat.h>

#include "probe/axis-probe.h"
#include "probe/axis-probe-file.h"
#include "probe/axis-probe-run.h"
#include "storage/axis-volume-guard.h"

G_BEGIN_DECLS

struct _AxisProbe
{
	gint             ref_count;	/* atomic */
	AxisDbPool      *pool;
	AxisStorage     *storage;	/* owned reference */
	AxisSettings    *settings;	/* owned reference, nullable */
	gint             runs;		/* atomic: tool processes started */

	GMutex           lock;
	AxisProbeTuning  tuning;	/* (lock) */
	gchar          **tool_command;	/* (lock) a copy of tuning.tool_command */
	gchar           *version_program;	/* (lock) the program the version is of */
	gchar           *version_text;	/* (lock) */
	gboolean         tool_missing_logged;	/* (lock) */
};

/* A snapshot of the library_file row a probe is for, with what the
 * decision to probe needs. */
typedef struct
{
	gint64    file_id;
	gint64    root_id;
	gint64    volume_id;
	gchar    *rel_path;	/* stored form */
	gchar    *root_rel_path;
	gchar    *kind;
	gchar    *state;
	gint64    size;
	gint64    mtime_sec;
	gint      mtime_nsec;
	gint64    ctime_sec;
	gint      ctime_nsec;
	gint64    inode;
	gboolean  changed;
	gint64    changed_generation;
	gint64    complete_generation;
	gboolean  has_info;
	gint      info_version;
} AxisProbeTarget;

void
axis_probe_target_clear(AxisProbeTarget *target);

/* Whether the file needs a probe: a video or audio file that is present
 * and either changed in a complete scan or recorded with an older
 * version. */
gboolean
axis_probe_target_pending(
	const AxisProbeTarget *target,
	gint                   current_version
);

/* ── Settings, the tool and its arguments (axis-probe.c) ────────── */

/* The version rows are stored with (and compared against). */
gint
axis_probe_current_version(AxisProbe *self);

guint
axis_probe_get_timeout_ms(AxisProbe *self);

/* The program to run, as an absolute path, or %NULL when there is none. */
gchar *
axis_probe_resolve_tool(AxisProbe *self);

/* "8.1.3" for the program; %NULL when not known or under a test's tool. */
gchar *
axis_probe_tool_version(
	AxisProbe   *self,
	const gchar *program
);

/**
 * axis_probe_build_argv:
 * @frames: the call that reads one frame of stream @stream_index, rather
 *   than the one that lists the container and streams
 *
 * Returns: (transfer full): the command, ended by %NULL.  The file is not
 *   in it: the child reads descriptor 3.
 */
gchar **
axis_probe_build_argv(
	AxisProbe   *self,
	const gchar *program,
	gboolean     frames,
	gint         stream_index
);

/* Runs the tool with @fd as its descriptor 3 and counts the run. */
void
axis_probe_run_tool(
	AxisProbe          *self,
	gchar             **argv,
	gint                fd,
	GCancellable       *cancellable,
	AxisProbeRunResult *result
);

/* ── Opening a file through a guard (axis-probe-file.c) ─────────── */

/* Whether a stat is the stamp the index recorded. */
gboolean
axis_probe_stamp_matches(
	const struct stat     *st,
	const AxisProbeTarget *target
);

/* Whether the open file and the name it was opened by still agree with
 * the index: same stamp, and the name still leads to the same file. */
gboolean
axis_probe_file_unchanged(
	const AxisProbeFile   *file,
	const AxisProbeTarget *target
);

/* ── The database (axis-probe-store.c) ──────────────────────────── */

/* Loads the row; *found is FALSE when there is none. */
gboolean
axis_probe_store_load_target(
	AxisDbConn       *conn,
	gint64            file_id,
	AxisProbeTarget  *target,
	gboolean         *found,
	GError          **error
);

/**
 * axis_probe_store_result:
 * @info: a media_info row to store; its @status says which, and for a
 *   failure only the error and the probe's facts matter
 * @stored: (out): whether it was stored (not if the row's stamp or change
 *   generation is no longer the one @target was read with)
 *
 * In one transaction: clears the file's changed flag where it still
 * matches @target, replaces the file's media_info and media_stream rows,
 * and forgets any delay.  Stores nothing (and says so) if the flag could not
 * be cleared against @target.
 */
gboolean
axis_probe_store_result(
	AxisDbConn            *conn,
	const AxisProbeTarget *target,
	const AxisMediaInfo   *info,
	gboolean              *stored,
	GError               **error
);

/* Puts the file's next probe off, for longer each time. */
gboolean
axis_probe_store_defer(
	AxisDbConn  *conn,
	gint64       file_id,
	const gchar *reason,
	GError     **error
);

/* The media_info row of a file and its streams; *found is FALSE for none. */
gboolean
axis_probe_store_load_info(
	AxisDbConn      *conn,
	gint64           file_id,
	AxisMediaInfo  **info,
	GError         **error
);

G_END_DECLS

#endif /* AXIS_PROBE_PRIVATE_H */
