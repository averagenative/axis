/*
 * axis-worker-private.h - shared by the files of axis-worker
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The worker is one main loop that talks to the daemon (axis-worker-run.c)
 * and one thread per job that does the work (axis-worker-work.c).  A job
 * thread reads its job's description and writes its results and progress
 * under the job's lock; everything else of a job, and the worker itself,
 * belongs to the main loop.  The main loop never waits for a job thread:
 * it cancels the job's cancellable, which kills the encoder's process
 * group (probe/axis-probe-run.h), and goes on.  A thread that does not
 * return even then (blocked in the kernel on a volume that stopped
 * answering) has its job given up, but what the main loop keeps of the job
 * stays until the thread returns, and nothing the thread does after that is
 * acted upon (axis-worker-run.c).
 */

#ifndef AXIS_WORKER_PRIVATE_H
#define AXIS_WORKER_PRIVATE_H

#include <gio/gio.h>

#include "api/axis-worker-proto.h"
#include "axis-worker-client.h"
#include "axis-worker-config.h"
#include "storage/axis-volume-proof.h"
#include "transcode/axis-convert.h"

G_BEGIN_DECLS

/* What the job directories in the scratch directory are called. */
#define AXIS_WORKER_JOB_PREFIX  "job-"
#define AXIS_WORKER_CAPS_PREFIX "caps-"

/* The file in the state directory that --healthcheck reads, and how old it
 * may be. */
#define AXIS_WORKER_HEALTH_FILE   "healthy"
#define AXIS_WORKER_HEALTH_MAX_S  90

/* A volume's work area: the daemon's AXIS_VOLUME_WORK_DIR
 * (storage/axis-volume-write.h, which the worker does not link; a test
 * checks that the two agree). */
#define AXIS_WORKER_WORK_DIR      ".axis-work"

typedef struct
{
	gchar    *uuid;
	gchar    *path;
	gboolean  keeps_identity;	/* FALSE: listed in volumes_without_file_identity */
} AxisWorkerMount;

typedef struct _AxisWorkerRunJob AxisWorkerRunJob;

struct _AxisWorkerRunJob
{
	gint             ref_count;	/* atomic */
	gpointer         worker;	/* the main loop's (Worker *) */
	AxisWorkerJob   *job;		/* as claimed: read-only from then on */
	GCancellable    *cancel;	/* kills the encoder */
	gchar           *scratch;	/* its own directory, made private */
	GPtrArray       *mounts;	/* AxisWorkerMount: where its volumes were proved */
	gchar          **ffmpeg;
	gchar          **ffprobe;
	guint            skip_families;

	GMutex           lock;
	gdouble          progress;		/* (lock) */
	gchar           *message;		/* (lock) */
	GPtrArray       *check;			/* (lock) AxisVolumeProof copies for the main loop's checks */
	AxisSampleResult *sample_result;	/* (lock) */
	AxisEncoderCapabilities *caps_result;	/* (lock) */
	AxisConvertResult *convert_result;	/* (lock) */
	AxisVerifyResult *verify_result;	/* (lock) a converted file's checks measured */
	gint             output_area;		/* (lock) the work area of a kept output, or -1 */
	gint             output_anchor;		/* (lock) that output, open read-only (its identity), or -1 */
	gchar           *failure;		/* (lock) why it failed */
	gboolean         retry;			/* (lock) */
	gboolean         volume_lost;		/* (lock) a volume was not the one proved */
	gint             crash_signal;		/* (lock) the encoder died of this signal (a crash), or 0 */
};

AxisWorkerRunJob *
axis_worker_run_job_ref(AxisWorkerRunJob *job);

void
axis_worker_run_job_unref(AxisWorkerRunJob *job);

/* Runs the job on the calling thread (a job thread): fills its results. */
void
axis_worker_work_run(AxisWorkerRunJob *job);

/* On a thread: removes a conversion's kept output that the daemon did not
 * take (its result was never reported, or the daemon refused it), by its
 * name while it is the file the kept read-only descriptor holds, from the
 * work area it was made in. */
void
axis_worker_work_drop_output(AxisWorkerRunJob *job);

/* Sets the job's failure (the first one stays). */
void
axis_worker_work_fail(
	AxisWorkerRunJob *job,
	const gchar      *why,
	gboolean          retry,
	gboolean          volume_lost
);

/**
 * axis_worker_work_prepare:
 *
 * At start-up, on a thread: ffmpeg's identity, then the capabilities from
 * the state directory's measurement if it is of the same ffmpeg build and
 * families, else a new measurement (kept there).  The volumes are not
 * touched: a mount that hangs must not hold back the registration (they are
 * proved path by path, each on its own thread, by the main loop).
 */
gboolean
axis_worker_work_prepare(
	const AxisWorkerConfig   *config,
	AxisEncoderCapabilities **caps,
	GError                  **error
);

/* Proves the volume at @path; NULL, with the reason in @why (nullable), when
 * it cannot be.  Blocks on the filesystem. */
AxisVolumeProof *
axis_worker_work_prove_path(
	const gchar *path,
	gchar      **why
);

#ifdef AXIS_DEBUG
/* The test hook of a volume that hangs (axis-worker-work.c). */
void
axis_worker_work_test_hang(const gchar *path);
#endif

/* Measures again (a worker.capabilities job does it in its own scratch),
 * and keeps the result in the state directory. */
gboolean
axis_worker_work_store_caps(
	const AxisWorkerConfig        *config,
	const AxisEncoderCapabilities *caps
);

/* Removes, in @scratch, every job or capability directory an earlier run
 * left behind (never following a link, never leaving its filesystem). */
void
axis_worker_work_clean_scratch(const gchar *scratch);

/* Removes one job's directory. */
void
axis_worker_work_remove_dir(const gchar *path);

/* Writes the health file now. */
void
axis_worker_work_touch_health(const gchar *state);

/* --healthcheck: 0 when the health file is recent. */
gint
axis_worker_work_healthcheck(const gchar *state);

/* ── axis-worker-output.c: the only changes on a volume ─────────── */

/* The work area beneath a volume's proved directory @top, on the device
 * @dev (opened, never made), or -1 with *@err. */
gint
axis_worker_output_area(
	gint     top,
	guint64  dev,
	gint    *err
);

/* Makes the output @name (axis_convert_output_name_valid()) in @area,
 * exclusively, mode 0600; its descriptor (open for writing), or -1 with
 * *@err (EEXIST: the name is taken, and nothing was touched). */
gint
axis_worker_output_create(
	gint         area,
	const gchar *name,
	gint        *err
);

/* fsync()s the output. */
gboolean
axis_worker_output_sync(
	gint  fd,
	gint *err
);

/* The output @name of @area opened again, read-only, while @out (its
 * descriptor for writing) is still open: the same file, or -1.  It is kept
 * instead of @out once the output is written, so that the output can be
 * recognised later without its inode number, which a network mount may
 * make up anew once the file is closed (FUSE without use_ino, SMB without
 * serverino); and no descriptor that can write it is left. */
gint
axis_worker_output_anchor(
	gint         area,
	const gchar *name,
	gint         out,
	gint        *err
);

/* Removes the output @name of @area while it is the file *@held is open on
 * (compared through the same mount, while that descriptor holds it), after
 * closing *@held (which is set to -1 in every case): a network mount renames
 * an open file it is asked to remove (.nfs..., .fuse_hidden...) instead of
 * removing it.  TRUE when it is gone. */
gboolean
axis_worker_output_remove_held(
	gint         area,
	const gchar *name,
	gint        *held
);

/* ── axis-worker-run.c ──────────────────────────────────────────── */

/* Runs the worker until it is stopped; the exit status. */
gint
axis_worker_run(AxisWorkerConfig *config);

G_END_DECLS

#endif /* AXIS_WORKER_PRIVATE_H */
