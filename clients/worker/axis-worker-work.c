/*
 * axis-worker-work.c - what axis-worker does on its job threads: measure
 * its encoders, measure samples; and the files it keeps
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A sample measurement trusts nothing of its description it can check:
 * the description was parsed strictly (api/axis-worker-proto.h), each
 * volume is proved again here before the job (its marker read through a
 * directory opened without following links, and the UUID compared), each
 * file is opened beneath that directory by its paths (no link, no other
 * filesystem: storage/axis-storage-open.h), must have the size and time
 * the description says, and the volume is checked again after each file,
 * so that a result is only kept for bytes read from the volume that was
 * proved.  The encoder is ffmpeg run by the probe runner, with the file as
 * descriptor 3 and its output to a pipe that is counted and dropped, in
 * the job's own scratch directory, and a file-size limit of zero.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-worker-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "probe/axis-probe-run.h"
#include "storage/axis-file-check.h"
#include "storage/axis-storage-open.h"
#include "transcode/axis-convert-caps.h"
#include "transcode/axis-sample-args.h"
#include "transcode/axis-verify.h"

#define ERROR_CAP     (16 * 1024)
#define MEMORY_LIMIT  (G_GUINT64_CONSTANT(8) * 1024 * 1024 * 1024)
#define MAX_DEPTH     16
#define CACHE_MAX     (256 * 1024)

/* ── Jobs ───────────────────────────────────────────────────────── */

AxisWorkerRunJob *
axis_worker_run_job_ref(AxisWorkerRunJob *job)
{
	g_atomic_int_inc(&job->ref_count);
	return job;
}

void
axis_worker_run_job_unref(AxisWorkerRunJob *job)
{
	if (job == NULL || !g_atomic_int_dec_and_test(&job->ref_count))
		return;
	axis_worker_job_free(job->job);
	g_clear_object(&job->cancel);
	g_free(job->scratch);
	if (job->mounts != NULL)
		g_ptr_array_unref(job->mounts);
	g_strfreev(job->ffmpeg);
	g_strfreev(job->ffprobe);
	g_free(job->message);
	if (job->check != NULL)
		g_ptr_array_unref(job->check);
	axis_sample_result_free(job->sample_result);
	axis_encoder_capabilities_free(job->caps_result);
	axis_convert_result_free(job->convert_result);
	axis_verify_result_free(job->verify_result);
	if (job->output_area >= 0)
		close(job->output_area);
	if (job->output_anchor >= 0)
		close(job->output_anchor);
	g_free(job->failure);
	g_mutex_clear(&job->lock);
	g_free(job);
}

void
axis_worker_work_fail(
	AxisWorkerRunJob *job,
	const gchar      *why,
	gboolean          retry,
	gboolean          volume_lost
){
	g_mutex_lock(&job->lock);
	if (job->failure == NULL) {
		job->failure = g_strdup(why);
		job->retry = retry;
	}
	job->volume_lost = job->volume_lost || volume_lost;
	g_mutex_unlock(&job->lock);
}

static void
say(
	AxisWorkerRunJob *job,
	gdouble           progress,
	const gchar      *message
){
	g_mutex_lock(&job->lock);
	job->progress = CLAMP(progress, 0.0, 1.0);
	g_free(job->message);
	job->message = g_strdup(message);
	g_mutex_unlock(&job->lock);
}

static gboolean
cancelled(AxisWorkerRunJob *job)
{
	return g_cancellable_is_cancelled(job->cancel);
}

/* Where the volume @uuid was proved, or NULL. */
static const AxisWorkerMount *
mount_for(
	AxisWorkerRunJob *job,
	const gchar      *uuid
){
	guint i;

	for (i = 0; i < job->mounts->len; i++) {
		const AxisWorkerMount *m = g_ptr_array_index(job->mounts, i);

		if (strcmp(m->uuid, uuid) == 0)
			return m;
	}
	return NULL;
}

/* The encoder died of a signal nobody here sent it: the job says so, and
 * the worker pauses (axis-worker-run.c) once the job is over. */
static gchar *
crashed(
	AxisWorkerRunJob         *job,
	const AxisProbeRunResult *run
){
	g_autofree gchar *raw = run->errors->len > 0 ? g_strndup((const gchar *)run->errors->data, run->errors->len) :
		NULL;
	g_autofree gchar *said = raw != NULL ? axis_library_text_clean(raw, 200) : NULL;

	g_mutex_lock(&job->lock);
	job->crash_signal = run->exit_status;
	g_mutex_unlock(&job->lock);
	return g_strdup_printf("the encoder crashed (signal %d, SIG%s)%s%s; this worker takes no more work until it is "
		"resumed on the Workers page or restarted", run->exit_status, axis_probe_run_signal_name(run->exit_status),
		said != NULL ? ": " : "", said != NULL ? said : "");
}

/* ── worker.capabilities ────────────────────────────────────────── */

static void
run_capabilities(AxisWorkerRunJob *job)
{
	g_autoptr(GError) error = NULL;
	AxisEncoderCapabilities *caps;
	AxisCapabilityTool tool;

	say(job, 0.0, "measuring the encoders");
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = (const gchar *const *)job->ffmpeg;
	tool.ffprobe = (const gchar *const *)job->ffprobe;
	tool.scratch = job->scratch;
	tool.cancellable = job->cancel;
	tool.skip_families = job->skip_families;
	caps = axis_convert_capabilities_measure(&tool, &error);
	if (caps == NULL) {
		if (!cancelled(job))
			axis_worker_work_fail(job, error->message, TRUE, FALSE);
		return;
	}
	g_mutex_lock(&job->lock);
	job->caps_result = caps;
	g_mutex_unlock(&job->lock);
	say(job, 1.0, "measured");
}

/* ── estimate.sample.remote ─────────────────────────────────────── */

typedef struct
{
	gchar           *uuid;
	AxisVolumeProof *proof;
} Proved;

static void
proved_free(gpointer data)
{
	Proved *p = data;

	g_free(p->uuid);
	axis_volume_proof_free(p->proof);
	g_free(p);
}

static AxisVolumeProof *
proof_for(
	GPtrArray   *proved,
	const gchar *uuid
){
	guint i;

	for (i = 0; i < proved->len; i++) {
		Proved *p = g_ptr_array_index(proved, i);

		if (strcmp(p->uuid, uuid) == 0)
			return p->proof;
	}
	return NULL;
}

/* The proof of every volume the description names, made now. */
static GPtrArray *
prove_volumes(AxisWorkerRunJob *job)
{
	const AxisSampleDescription *desc = job->job->sample;
	GPtrArray *proved = g_ptr_array_new_with_free_func(proved_free);
	guint i;
	guint j;

	for (i = 0; i < desc->files->len; i++) {
		const AxisSampleFilePlan *f = g_ptr_array_index(desc->files, i);
		const AxisWorkerMount *mount = NULL;
		AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_MISSING;
		AxisVolumeProof *proof;
		Proved *p;

		if (proof_for(proved, f->volume) != NULL)
			continue;
		for (j = 0; j < job->mounts->len; j++) {
			const AxisWorkerMount *m = g_ptr_array_index(job->mounts, j);

			if (strcmp(m->uuid, f->volume) == 0)
				mount = m;
		}
		if (mount == NULL) {
			g_autofree gchar *why = g_strdup_printf("the volume %s is not one this worker has proved", f->volume);

			axis_worker_work_fail(job, why, TRUE, TRUE);
			g_ptr_array_unref(proved);
			return NULL;
		}
		proof = axis_volume_proof_make(mount->path, &status);
		if (proof == NULL || strcmp(axis_volume_proof_get_uuid(proof), f->volume) != 0) {
			g_autofree gchar *why = g_strdup_printf("volume unavailable: %s at %s: %s", f->volume, mount->path,
				proof == NULL ? axis_volume_proof_status_describe(status) : "another volume's marker is there");

			axis_volume_proof_free(proof);
			axis_worker_work_fail(job, why, TRUE, TRUE);
			g_ptr_array_unref(proved);
			return NULL;
		}
		p = g_new0(Proved, 1);
		p->uuid = g_strdup(f->volume);
		p->proof = proof;
		g_ptr_array_add(proved, p);
		/* A copy the main loop checks while an excerpt runs. */
		{
			AxisVolumeProof *copy = axis_volume_proof_dup(proof);

			if (copy != NULL) {
				g_mutex_lock(&job->lock);
				g_ptr_array_add(job->check, copy);
				g_mutex_unlock(&job->lock);
			}
		}
	}
	return proved;
}

/* The volume of @f is still the one proved; else the job stops. */
static gboolean
still_there(
	AxisWorkerRunJob         *job,
	GPtrArray                *proved,
	const AxisSampleFilePlan *f
){
	AxisVolumeProof *proof = proof_for(proved, f->volume);
	AxisVolumeProofStatus status = proof != NULL ? axis_volume_proof_check(proof) : AXIS_VOLUME_PROOF_MISSING;
	g_autofree gchar *why = NULL;

	if (status == AXIS_VOLUME_PROOF_OK)
		return TRUE;
	why = g_strdup_printf("volume unavailable: %s: %s", f->volume, axis_volume_proof_status_describe(status));
	axis_worker_work_fail(job, why, TRUE, TRUE);
	return FALSE;
}

static const gchar *
skip_word(AxisStorageOpenStatus status)
{
	switch (status) {
	case AXIS_STORAGE_OPEN_MISSING:
		return AXIS_SAMPLE_SKIP_MISSING;
	case AXIS_STORAGE_OPEN_NOT_CONTAINED:
		return AXIS_SAMPLE_SKIP_NOT_CONTAINED;
	case AXIS_STORAGE_OPEN_INVALID:
		return AXIS_SAMPLE_SKIP_PATH_REFUSED;
	default:
		return AXIS_SAMPLE_SKIP_UNREADABLE;
	}
}

static void
run_sample(AxisWorkerRunJob *job)
{
	const AxisSampleDescription *desc = job->job->sample;
	g_autoptr(GPtrArray) proved = NULL;
	g_autoptr(AxisSampleResult) result = axis_sample_result_new();
	guint planned = 0;
	guint done = 0;
	guint i;
	guint k;

	/* Never an encoder this worker was not allowed, or not shown, to run,
	 * whatever the daemon says. */
	if ((job->skip_families & AXIS_CONVERT_FAMILY_BIT(desc->family)) != 0) {
		axis_worker_work_fail(job, "this worker does not run that encoder family", TRUE, FALSE);
		return;
	}
	for (i = 0; i < desc->files->len; i++)
		planned += ((const AxisSampleFilePlan *)g_ptr_array_index(desc->files, i))->n_excerpts;
	proved = prove_volumes(job);
	if (proved == NULL)
		return;

	for (i = 0; i < desc->files->len; i++) {
		const AxisSampleFilePlan *f = g_ptr_array_index(desc->files, i);
		AxisVolumeProof *proof = proof_for(proved, f->volume);
		AxisStorageOpenStatus opened;
		AxisEstimateTarget target;
		AxisMediaStream stream;
		guint64 bytes[AXIS_SAMPLE_MAX_EXCERPTS];
		const gchar *skipped = NULL;
		struct stat st;
		gint saved = 0;
		gint fd = -1;

		if (cancelled(job))
			return;
		if (!still_there(job, proved, f))
			return;
		opened = axis_storage_open_file_beneath(axis_volume_proof_get_fd(proof), f->root, f->path, &fd, &st, &saved);
		if (opened != AXIS_STORAGE_OPEN_OK) {
			g_autofree gchar *shown = axis_library_name_display(f->path);

			g_message("job %" G_GINT64_FORMAT ": file %u (%s): %s", job->job->id, f->index, shown,
				axis_storage_open_status_describe(opened));
			axis_sample_result_add(result, f->index, NULL, 0, skip_word(opened));
			done += f->n_excerpts;
			continue;
		}
		/* The file the daemon planned for, not one written since: its size
		 * and time, to the nanosecond where the mount keeps file identities.
		 * Nothing is written by a measurement, and the daemon checks what it
		 * is told against the plan, so the time to the second is enough
		 * where they are not kept. */
		{
			const AxisWorkerMount *mount = mount_for(job, f->volume);
			AxisFileStamp want;
			AxisFileStampResult same;

			memset(&want, 0, sizeof(want));
			want.size = f->size;
			want.mtime_sec = f->mtime_sec;
			want.mtime_nsec = f->mtime_nsec;
			want.inode = -1;
			same = axis_file_stamp_compare(&st, &want, mount == NULL || mount->keeps_identity);
			if (same != AXIS_FILE_STAMP_SAME) {
				g_message("job %" G_GINT64_FORMAT ": file %u: not measured: %s", job->job->id, f->index,
					axis_file_stamp_describe(same));
				close(fd);
				axis_sample_result_add(result, f->index, NULL, 0, AXIS_SAMPLE_SKIP_CHANGED);
				done += f->n_excerpts;
				continue;
			}
		}
		axis_sample_description_to_target(desc, f, &target, &stream);
		for (k = 0; k < f->n_excerpts && skipped == NULL; k++) {
			g_auto(GStrv) argv = NULL;
			g_autofree gchar *message = g_strdup_printf("file %u of %u, excerpt %u of %u", i + 1, desc->files->len,
				k + 1, f->n_excerpts);
			AxisProbeRunSpec spec;
			AxisProbeRunResult run;

			say(job, planned > 0 ? MIN((gdouble)done / planned, 0.99) : 0.0, message);
			if (lseek(fd, 0, SEEK_SET) != 0) {
				skipped = AXIS_SAMPLE_SKIP_UNREADABLE;
				break;
			}
			argv = axis_estimate_sample_argv((const gchar *const *)job->ffmpeg, &target, &stream, f->quality,
				f->bit_depth, (gdouble)f->start_ms[k] / 1000.0, (gdouble)f->length_ms / 1000.0);
			memset(&spec, 0, sizeof(spec));
			spec.argv = (const gchar *const *)argv;
			spec.fd = fd;
			spec.timeout_ms = desc->excerpt_timeout_ms;
			spec.output_cap = (gsize)MIN(AXIS_SAMPLE_OUTPUT_CAP, (guint64)G_MAXSIZE);
			spec.error_cap = ERROR_CAP;
			spec.cancellable = job->cancel;
			spec.memory_limit = MEMORY_LIMIT;
			spec.discard_output = TRUE;
			spec.working_directory = job->scratch;
			spec.cpu_seconds = desc->excerpt_timeout_ms / 1000 * 16 + 60;
			axis_probe_run(&spec, &run);
			if (run.status == AXIS_PROBE_RUN_CANCELLED || cancelled(job)) {
				axis_probe_run_result_clear(&run);
				close(fd);
				return;
			}
			/* A crash ends the measurement: nothing more is encoded here, and
			 * another worker can measure it all again. */
			if (axis_probe_run_crashed(&run)) {
				g_autofree gchar *why = crashed(job, &run);

				g_warning("job %" G_GINT64_FORMAT ": file %u: %s", job->job->id, f->index, why);
				axis_probe_run_result_clear(&run);
				close(fd);
				axis_worker_work_fail(job, why, TRUE, FALSE);
				return;
			}
			if (run.status != AXIS_PROBE_RUN_OK || run.output_bytes == 0) {
				g_autofree gchar *raw = run.errors->len > 0 ?
					g_strndup((const gchar *)run.errors->data, run.errors->len) : NULL;
				g_autofree gchar *said = raw != NULL ? axis_library_text_clean(raw, 200) : NULL;

				g_message("job %" G_GINT64_FORMAT ": file %u: the encoder %s%s%s", job->job->id, f->index,
					axis_probe_run_status_describe(run.status), said != NULL ? ": " : "", said != NULL ? said : "");
				skipped = AXIS_SAMPLE_SKIP_ENCODER_FAILED;
			} else {
				bytes[k] = run.output_bytes;
			}
			axis_probe_run_result_clear(&run);
			done++;
		}
		close(fd);
		/* What was read counts only if the volume is still the one proved. */
		if (!still_there(job, proved, f))
			return;
		if (skipped != NULL) {
			done += f->n_excerpts - MIN(k, f->n_excerpts);
			axis_sample_result_add(result, f->index, NULL, 0, skipped);
		} else {
			axis_sample_result_add(result, f->index, bytes, f->n_excerpts, NULL);
		}
	}
	g_mutex_lock(&job->lock);
	job->sample_result = g_steal_pointer(&result);
	g_mutex_unlock(&job->lock);
	say(job, 1.0, "measured");
}

/* ── convert.encode ─────────────────────────────────────────────── */

/*
 * A conversion trusts nothing of its description it can check either: the
 * volume is proved again, its mount must be read-write, the argument
 * vector must be one the plan builder makes (input descriptor 3, output
 * descriptor 4, only the fd protocol), the source is opened beneath the
 * proved directory and must have the stamp the daemon recorded (all of it,
 * or, on a volume the configuration says is seen through a mount that does
 * not keep file identities, its size, its time to the second and its links)
 * and the digest of samples of its content the daemon read, and the
 * output is made in the work area under the daemon's name, exclusively.
 * An encoder that crashes (dies of a signal nobody sent it) fails the job
 * and pauses the worker (axis-worker-run.c).
 * ffmpeg may write at most the source's size (a larger output would be
 * rejected anyway).  Whatever ends the job without a result removes the
 * output again; a result is the output's name and size, after an fsync, and
 * a result the daemon does not take (the lease ran out before it was
 * reported, or the daemon refused it) has its output removed too.
 */

typedef struct
{
	AxisWorkerRunJob *job;
	gint              fd;
	guint64           size;
	gint              stop;	/* atomic */
	gboolean          checking;	/* the decode of a check job, not an encode */
	gdouble           share;	/* of the job's progress (checking) */
} Watch;

/* How far ffmpeg has read the source (or, checking, the converted file): it
 * shares the descriptor's offset. */
static gpointer
watch_thread(gpointer data)
{
	Watch *watch = data;
	gint64 started = g_get_monotonic_time();
	gdouble best = 0.0;

	while (!g_atomic_int_get(&watch->stop)) {
		off_t at = lseek(watch->fd, 0, SEEK_CUR);

		/* The first seconds read the container's index, which may be at the
		 * end of the file. */
		if (at > 0 && watch->size > 0 && g_get_monotonic_time() - started > 3 * G_USEC_PER_SEC) {
			gdouble fraction = MIN((gdouble)at / (gdouble)watch->size, 0.99);

			if (fraction > best) {
				g_autofree gchar *message = watch->checking ?
					g_strdup_printf("checking: %.0f%% of the converted file decoded", fraction * 100) :
					g_strdup_printf("converting: %.0f%% of the source read", fraction * 100);

				best = fraction;
				say(watch->job, watch->checking ? fraction * watch->share : fraction, message);
			}
		}
		g_usleep(G_USEC_PER_SEC / 2);
	}
	return NULL;
}

/* A stream table of @n streams, indices 0 to @n - 1, for the vector's
 * check. */
static AxisMediaInfo *
shape_of(guint n)
{
	AxisMediaInfo *info = axis_media_info_new();
	guint i;

	for (i = 0; i < n; i++) {
		AxisMediaStream *s = g_new0(AxisMediaStream, 1);

		s->index = (gint)i;
		s->kind = AXIS_STREAM_UNKNOWN;
		g_ptr_array_add(info->streams, s);
	}
	return info;
}

/* Whether @args runs an encoder of a family this worker may not run. */
static gboolean
family_refused(
	AxisWorkerRunJob    *job,
	const gchar *const  *args
){
	const AxisConvertEncoder *table = axis_convert_encoders();
	guint i;
	guint k;

	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		if ((job->skip_families & AXIS_CONVERT_FAMILY_BIT(table[i].family)) == 0)
			continue;
		for (k = 0; args[k] != NULL; k++) {
			if (strcmp(args[k], table[i].name) == 0)
				return TRUE;
		}
	}
	return FALSE;
}

static void
run_convert(AxisWorkerRunJob *job)
{
	const AxisConvertWork *work = job->job->convert;
	g_autoptr(AxisVolumeProof) proof = NULL;
	g_autoptr(AxisMediaInfo) shape = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_autofree gchar *problem = NULL;
	const AxisWorkerMount *mount = NULL;
	AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_MISSING;
	AxisStorageOpenStatus opened;
	AxisProbeRunSpec spec;
	AxisProbeRunResult run;
	struct statvfs vfs;
	struct stat st;
	GThread *watcher;
	Watch watch;
	gint saved = 0;
	gint err = 0;
	gint src = -1;
	gint area = -1;
	gint out = -1;
	gint anchor = -1;
	guint i;

	say(job, 0.0, "proving the volume");
	mount = mount_for(job, work->volume);
	if (mount == NULL) {
		g_autofree gchar *why = g_strdup_printf("the volume %s is not one this worker has proved", work->volume);

		axis_worker_work_fail(job, why, TRUE, TRUE);
		return;
	}
	proof = axis_volume_proof_make(mount->path, &status);
	if (proof == NULL || strcmp(axis_volume_proof_get_uuid(proof), work->volume) != 0) {
		g_autofree gchar *why = g_strdup_printf("volume unavailable: %s at %s: %s", work->volume, mount->path,
			proof == NULL ? axis_volume_proof_status_describe(status) : "another volume's marker is there");

		axis_worker_work_fail(job, why, TRUE, TRUE);
		return;
	}
	{
		AxisVolumeProof *copy = axis_volume_proof_dup(proof);

		if (copy != NULL) {
			g_mutex_lock(&job->lock);
			g_ptr_array_add(job->check, copy);
			g_mutex_unlock(&job->lock);
		}
	}
	if (fstatvfs(axis_volume_proof_get_fd(proof), &vfs) != 0 || (vfs.f_flag & ST_RDONLY) != 0) {
		axis_worker_work_fail(job, "the volume is mounted read-only on this worker: nothing is written", TRUE, FALSE);
		return;
	}

	/* The vector: one the plan builder makes, for a file of this many
	 * streams, and of an encoder this worker may run. */
	shape = shape_of(work->source_streams);
	if (!axis_convert_args_valid((const gchar *const *)work->args, shape, &problem)) {
		g_autofree gchar *why = g_strdup_printf("the arguments are not ones Axis makes (%s): nothing was run", problem);

		axis_worker_work_fail(job, why, FALSE, FALSE);
		return;
	}
	if (family_refused(job, (const gchar *const *)work->args)) {
		axis_worker_work_fail(job, "this worker is not allowed to run that encoder family", TRUE, FALSE);
		return;
	}

	/* The source, as the daemon recorded it: its stamp (all of it where the
	 * mount keeps file identities; its size, its time to the second and its
	 * links where it was said not to), then the samples of its content the
	 * daemon read. */
	opened = axis_storage_open_file_beneath(axis_volume_proof_get_fd(proof), work->root, work->path, &src, &st, &saved);
	if (opened != AXIS_STORAGE_OPEN_OK) {
		g_autofree gchar *why = g_strdup_printf("the source cannot be opened: %s", axis_storage_open_status_describe(opened));

		axis_worker_work_fail(job, why, FALSE, FALSE);
		return;
	}
	{
		AxisFileStamp want;
		AxisFileStampResult same;

		memset(&want, 0, sizeof(want));
		want.size = work->size;
		want.mtime_sec = work->mtime_sec;
		want.mtime_nsec = work->mtime_nsec;
		want.inode = work->inode;
		want.one_link = TRUE;
		same = axis_file_stamp_compare(&st, &want, mount->keeps_identity);
		if (same != AXIS_FILE_STAMP_SAME) {
			g_autofree gchar *why = NULL;

			g_message("job %" G_GINT64_FORMAT ": the source here: %" G_GINT64_FORMAT " bytes, %" G_GINT64_FORMAT
				".%09ld s, inode %" G_GUINT64_FORMAT ", %u link(s); the daemon recorded %" G_GINT64_FORMAT " bytes, %"
				G_GINT64_FORMAT ".%09d s, inode %" G_GINT64_FORMAT, job->job->id, (gint64)st.st_size,
				(gint64)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec, (guint64)st.st_ino, (guint)st.st_nlink,
				work->size, work->mtime_sec, work->mtime_nsec, work->inode);
			if (same == AXIS_FILE_STAMP_SUBSECOND || same == AXIS_FILE_STAMP_INODE)
				why = g_strdup_printf("source changed: the file is not the one the conversion was requested for (%s, "
					"its size and time to the second are the same; if this worker sees the volume through a "
					"network mount that does not keep file identities, such as sshfs or SMB, list %s in "
					"AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY)", axis_file_stamp_describe(same), mount->path);
			else
				why = g_strdup_printf("source changed: the file is not the one the conversion was requested for (%s)",
					axis_file_stamp_describe(same));
			close(src);
			axis_worker_work_fail(job, why, FALSE, FALSE);
			return;
		}
	}
	{
		g_autofree gchar *digest = NULL;

		say(job, 0.0, "reading samples of the source");
		if (!axis_file_sample_digest(src, work->size, &digest, &err)) {
			g_autofree gchar *why = g_strdup_printf("the source cannot be read: %s", g_strerror(err));

			close(src);
			axis_worker_work_fail(job, why, err != EIO, FALSE);
			return;
		}
		if (g_strcmp0(digest, work->source_sample) != 0) {
			close(src);
			axis_worker_work_fail(job, "source changed: its content is not what the daemon read (samples of it "
				"differ): the file is not the one the conversion was requested for", FALSE, FALSE);
			return;
		}
		err = 0;
	}

	/* The output, in the work area, never over anything. */
	area = axis_worker_output_area(axis_volume_proof_get_fd(proof), axis_volume_proof_get_dev(proof), &err);
	if (area < 0) {
		g_autofree gchar *why = g_strdup_printf("the volume's work area cannot be opened: %s", g_strerror(err));

		close(src);
		axis_worker_work_fail(job, why, TRUE, FALSE);
		return;
	}
	if (fstatvfs(area, &vfs) != 0 || (guint64)vfs.f_bavail * (guint64)vfs.f_frsize < (guint64)work->size) {
		close(src);
		close(area);
		axis_worker_work_fail(job, "there is not room on the volume for an output as large as the source", TRUE,
			FALSE);
		return;
	}
	if (cancelled(job)) {
		close(src);
		close(area);
		return;
	}
	out = axis_worker_output_create(area, work->output, &err);
	if (out < 0) {
		g_autofree gchar *why = err == EEXIST ?
			g_strdup("the output's name exists already in the work area: it is not overwritten") :
			g_strdup_printf("the output cannot be made in the work area: %s", g_strerror(err));

		close(src);
		close(area);
		axis_worker_work_fail(job, why, err != EEXIST, FALSE);
		return;
	}

	argv = g_ptr_array_new();
	for (i = 0; job->ffmpeg[i] != NULL; i++)
		g_ptr_array_add(argv, job->ffmpeg[i]);
	for (i = 0; work->args[i] != NULL; i++)
		g_ptr_array_add(argv, work->args[i]);
	g_ptr_array_add(argv, NULL);
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv->pdata;
	spec.fd = src;
	spec.output_fd = out;
	spec.file_size_limit = (guint64)work->size;
	spec.timeout_ms = work->timeout_ms;
	spec.output_cap = 4 * 1024 * 1024;
	spec.error_cap = ERROR_CAP;
	spec.cancellable = job->cancel;
	spec.memory_limit = MEMORY_LIMIT;
	spec.working_directory = job->scratch;
	spec.cpu_seconds = work->timeout_ms / 1000 * 16 + 60;
	memset(&watch, 0, sizeof(watch));
	watch.job = job;
	watch.fd = src;
	watch.size = (guint64)work->size;
	say(job, 0.0, "converting");
	watcher = g_thread_new("axis-convert-watch", watch_thread, &watch);
	axis_probe_run(&spec, &run);
	g_atomic_int_set(&watch.stop, 1);
	g_thread_join(watcher);

	if (run.status == AXIS_PROBE_RUN_CANCELLED || cancelled(job)) {
		axis_probe_run_result_clear(&run);
		goto drop;
	}
	if (axis_probe_run_crashed(&run)) {
		/* Not the input's doing: the conversion fails as any encoder failure
		 * does, and this worker takes no more work (axis-worker-run.c). */
		g_autofree gchar *why = crashed(job, &run);

		g_warning("job %" G_GINT64_FORMAT ": %s", job->job->id, why);
		axis_probe_run_result_clear(&run);
		axis_worker_work_fail(job, why, FALSE, FALSE);
		goto drop;
	}
	if (run.status != AXIS_PROBE_RUN_OK) {
		g_autofree gchar *raw = run.errors->len > 0 ? g_strndup((const gchar *)run.errors->data, run.errors->len) : NULL;
		g_autofree gchar *said = raw != NULL ? axis_library_text_clean(raw, 300) : NULL;
		g_autofree gchar *why = NULL;

		/* The two limits the runner sets end the encoder by a signal: an
		 * output that would be larger than the source, the CPU time. */
		if (run.status == AXIS_PROBE_RUN_SIGNALLED && run.exit_status == SIGXFSZ)
			why = g_strdup("the encoder was stopped: its output would have been larger than the source");
		else if (run.status == AXIS_PROBE_RUN_SIGNALLED && run.exit_status == SIGXCPU)
			why = g_strdup("the encoder was stopped: it used up its CPU time limit");
		else
			why = g_strdup_printf("the encoder %s%s%s", axis_probe_run_status_describe(run.status),
				said != NULL ? ": " : "", said != NULL ? said : "");
		axis_probe_run_result_clear(&run);
		axis_worker_work_fail(job, why, FALSE, FALSE);
		goto drop;
	}
	axis_probe_run_result_clear(&run);
	if (!axis_worker_output_sync(out, &err) || fstat(out, &st) != 0 || st.st_size <= 0) {
		g_autofree gchar *why = g_strdup_printf("the output cannot be flushed to the disk: %s",
			g_strerror(err != 0 ? err : EIO));

		axis_worker_work_fail(job, why, TRUE, FALSE);
		goto drop;
	}
	/* What was written counts only if the volume is still the one proved. */
	status = axis_volume_proof_check(proof);
	if (status != AXIS_VOLUME_PROOF_OK) {
		g_autofree gchar *why = g_strdup_printf("volume unavailable: %s: %s", work->volume,
			axis_volume_proof_status_describe(status));

		axis_worker_work_fail(job, why, TRUE, TRUE);
		goto drop;
	}
	/* The output is known from here on by a read-only descriptor of it, made
	 * while the one that wrote it is still open: no descriptor that can
	 * write it is left once it is reported. */
	anchor = axis_worker_output_anchor(area, work->output, out, &err);
	if (anchor < 0) {
		g_autofree gchar *why = g_strdup_printf("the output cannot be opened again in the work area: %s",
			g_strerror(err != 0 ? err : EIO));

		axis_worker_work_fail(job, why, TRUE, FALSE);
		goto drop;
	}
	close(out);
	close(src);
	/* The work area stays open: if the daemon does not take the result (a
	 * lease that ran out before it was reported, a refusal), the output is
	 * removed again (axis_worker_work_drop_output()). */
	g_mutex_lock(&job->lock);
	job->convert_result = g_new0(AxisConvertResult, 1);
	job->convert_result->output = g_strdup(work->output);
	job->convert_result->size = (gint64)st.st_size;
	job->output_area = area;
	job->output_anchor = anchor;
	g_mutex_unlock(&job->lock);
	say(job, 1.0, "converted");
	return;

drop:
	close(src);
	/* Compared while the descriptor that wrote it still holds it, then
	 * closed, then removed by name. */
	if (!axis_worker_output_remove_held(area, work->output, &out))
		g_warning("job %" G_GINT64_FORMAT ": its output could not be removed from the work area", job->job->id);
	close(area);
}

/* ── convert.verify ─────────────────────────────────────────────── */

/*
 * A converted file's checks measured here, for the daemon to judge: the same
 * proof and the same rules for the original as an encode (the volume proved
 * again, the original's stamp as the mount shows it and the digest of its
 * samples), the converted file opened read-only in the work area as a plain
 * file of the size the daemon recorded, then the measurements of
 * transcode/axis-verify-measure.c (ffmpeg's own threads), the whole file's
 * SHA-256 read beside the decode, and the volume proved again.  Nothing is
 * written.  A tool that crashes fails the job for another try and pauses the
 * worker, as an encoder's crash does; a tool that could not run fails the
 * job for another try; no verdict is ever sent.
 */

typedef struct
{
	gint     fd;
	gint64   size;
	gint     stop;	/* atomic */
	gchar   *hex;
	gint     err;
	gboolean ok;
} WholeRead;

static gpointer
whole_read_thread(gpointer data)
{
	WholeRead *w = data;

	w->ok = axis_file_whole_digest(w->fd, w->size, &w->stop, &w->hex, &w->err);
	return NULL;
}

static void
whole_read_cancelled(
	GCancellable *cancellable,
	gpointer      data
){
	g_atomic_int_set((gint *)data, 1);
}

/* Whether @run is not a measurement, in which case the job ends here: a
 * cancellation says nothing, a crash pauses the worker, anything else fails
 * the job for another try. */
static gboolean
not_measured(
	AxisWorkerRunJob    *job,
	const AxisVerifyRun *run,
	const gchar         *what
){
	if (axis_verify_run_reportable(run))
		return FALSE;
	if (run->status == AXIS_PROBE_RUN_CANCELLED || cancelled(job))
		return TRUE;
	if (axis_verify_run_crashed(run)) {
		g_autofree gchar *why = g_strdup_printf("ffmpeg crashed while it measured %s (signal %d, SIG%s)%s%s; this worker "
			"takes no more work until it is resumed on the Workers page or restarted", what, run->code,
			axis_probe_run_signal_name(run->code), run->said != NULL ? ": " : "", run->said != NULL ? run->said : "");

		g_mutex_lock(&job->lock);
		job->crash_signal = run->code;
		g_mutex_unlock(&job->lock);
		g_warning("job %" G_GINT64_FORMAT ": %s", job->job->id, why);
		axis_worker_work_fail(job, why, TRUE, FALSE);
		return TRUE;
	}
	{
		g_autofree gchar *why = g_strdup_printf("%s could not be measured: the tool %s%s%s", what,
			axis_probe_run_status_describe(run->status), run->said != NULL ? ": " : "", run->said != NULL ? run->said : "");

		axis_worker_work_fail(job, why, TRUE, FALSE);
	}
	return TRUE;
}

static void
run_verify(AxisWorkerRunJob *job)
{
	const AxisVerifyWork *work = job->job->verify;
	g_autoptr(AxisVolumeProof) proof = NULL;
	g_autoptr(AxisVerifyEvidence) evidence = axis_verify_evidence_new();
	g_autofree gchar *source_digest = NULL;
	const AxisWorkerMount *mount = NULL;
	AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_MISSING;
	AxisStorageOpenStatus opened;
	AxisVerifyTools tools;
	struct stat st;
	gint saved = 0;
	gint err = 0;
	gint src = -1;
	gint area = -1;
	gint out = -1;
	guint k;

	say(job, 0.0, "proving the volume");
	mount = mount_for(job, work->volume);
	if (mount == NULL) {
		g_autofree gchar *why = g_strdup_printf("the volume %s is not one this worker has proved", work->volume);

		axis_worker_work_fail(job, why, TRUE, TRUE);
		return;
	}
	proof = axis_volume_proof_make(mount->path, &status);
	if (proof == NULL || strcmp(axis_volume_proof_get_uuid(proof), work->volume) != 0) {
		g_autofree gchar *why = g_strdup_printf("volume unavailable: %s at %s: %s", work->volume, mount->path,
			proof == NULL ? axis_volume_proof_status_describe(status) : "another volume's marker is there");

		axis_worker_work_fail(job, why, TRUE, TRUE);
		return;
	}
	{
		AxisVolumeProof *copy = axis_volume_proof_dup(proof);

		if (copy != NULL) {
			g_mutex_lock(&job->lock);
			g_ptr_array_add(job->check, copy);
			g_mutex_unlock(&job->lock);
		}
	}

	/* The original, as the daemon recorded it, exactly as for an encode. */
	opened = axis_storage_open_file_beneath(axis_volume_proof_get_fd(proof), work->root, work->path, &src, &st, &saved);
	if (opened != AXIS_STORAGE_OPEN_OK) {
		g_autofree gchar *why = g_strdup_printf("the source cannot be opened: %s", axis_storage_open_status_describe(opened));

		axis_worker_work_fail(job, why, FALSE, FALSE);
		return;
	}
	{
		AxisFileStamp want;
		AxisFileStampResult same;

		memset(&want, 0, sizeof(want));
		want.size = work->size;
		want.mtime_sec = work->mtime_sec;
		want.mtime_nsec = work->mtime_nsec;
		want.inode = work->inode;
		want.one_link = TRUE;
		same = axis_file_stamp_compare(&st, &want, mount->keeps_identity);
		if (same != AXIS_FILE_STAMP_SAME) {
			g_autofree gchar *why = NULL;

			if (same == AXIS_FILE_STAMP_SUBSECOND || same == AXIS_FILE_STAMP_INODE)
				why = g_strdup_printf("source changed: the file is not the one the conversion was requested for (%s, "
					"its size and time to the second are the same; if this worker sees the volume through a "
					"network mount that does not keep file identities, such as sshfs or SMB, list %s in "
					"AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY)", axis_file_stamp_describe(same), mount->path);
			else
				why = g_strdup_printf("source changed: the file is not the one the conversion was requested for (%s)",
					axis_file_stamp_describe(same));
			close(src);
			axis_worker_work_fail(job, why, FALSE, FALSE);
			return;
		}
	}
	say(job, 0.0, "reading samples of the source");
	if (!axis_file_sample_digest(src, work->size, &source_digest, &err)) {
		g_autofree gchar *why = g_strdup_printf("the source cannot be read: %s", g_strerror(err));

		close(src);
		axis_worker_work_fail(job, why, err != EIO, FALSE);
		return;
	}
	if (g_strcmp0(source_digest, work->source_sample) != 0) {
		close(src);
		axis_worker_work_fail(job, "source changed: its content is not what the daemon read (samples of it differ): "
			"the file is not the one the conversion was requested for", FALSE, FALSE);
		return;
	}

	/* The converted file, read only, in the work area, never through a
	 * link. */
	area = axis_worker_output_area(axis_volume_proof_get_fd(proof), axis_volume_proof_get_dev(proof), &err);
	if (area < 0) {
		g_autofree gchar *why = g_strdup_printf("the volume's work area cannot be opened: %s", g_strerror(err));

		close(src);
		axis_worker_work_fail(job, why, TRUE, FALSE);
		return;
	}
	out = openat(area, work->output, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	saved = out < 0 ? errno : 0;
	if (out < 0 || fstat(out, &st) != 0 || !S_ISREG(st.st_mode) || (gint64)st.st_size != work->output_size) {
		/* Gone, or another file: trying again cannot help.  Another reason
		 * to fail to open it (the mount) may pass. */
		gboolean again = out < 0 && saved != ENOENT;
		g_autofree gchar *why = out < 0 ? g_strdup_printf("the converted file cannot be opened in the work area: %s",
			g_strerror(saved)) : g_strdup("the converted file in the work area is not a plain file of the size the "
			"daemon recorded");

		if (out >= 0)
			close(out);
		close(src);
		close(area);
		axis_worker_work_fail(job, why, again, FALSE);
		return;
	}
	{
		gint flags = fcntl(out, F_GETFL);

		if (flags >= 0)
			(void)fcntl(out, F_SETFL, flags & ~O_NONBLOCK);
	}

	memset(&tools, 0, sizeof(tools));
	tools.ffmpeg = (const gchar *const *)job->ffmpeg;
	tools.ffprobe = (const gchar *const *)job->ffprobe;
	tools.scratch = job->scratch;
	tools.cancellable = job->cancel;

	/* 5: the decode, with the whole file's SHA-256 read beside it (pread(),
	 * so ffmpeg's offset is its own; the page cache usually serves one
	 * reading from the other). */
	{
		WholeRead whole;
		Watch watch;
		GThread *reader;
		GThread *watcher;
		gulong handler;

		memset(&whole, 0, sizeof(whole));
		whole.fd = out;
		whole.size = work->output_size;
		handler = g_cancellable_connect(job->cancel, G_CALLBACK(whole_read_cancelled), &whole.stop, NULL);
		memset(&watch, 0, sizeof(watch));
		watch.job = job;
		watch.fd = out;
		watch.size = (guint64)work->output_size;
		watch.checking = TRUE;
		watch.share = 0.6;
		say(job, 0.0, "checking: decoding the converted file");
		reader = g_thread_new("axis-verify-digest", whole_read_thread, &whole);
		watcher = g_thread_new("axis-verify-watch", watch_thread, &watch);
		axis_verify_measure_decode(&tools, work->plan, out, evidence);
		g_atomic_int_set(&watch.stop, 1);
		g_thread_join(watcher);
		(void)g_thread_join(reader);
		g_cancellable_disconnect(job->cancel, handler);
		if (not_measured(job, &evidence->decode, "the decode")) {
			g_free(whole.hex);
			goto done;
		}
		if (!whole.ok) {
			g_autofree gchar *why = g_strdup_printf("the converted file cannot be read whole: %s",
				g_strerror(whole.err != 0 ? whole.err : EIO));

			if (!cancelled(job))
				axis_worker_work_fail(job, why, TRUE, FALSE);
			goto done;
		}
		evidence->output_digest = whole.hex;
	}
	if (!axis_file_sample_digest(out, work->output_size, &evidence->output_sample, &err)) {
		g_autofree gchar *why = g_strdup_printf("the converted file cannot be read: %s", g_strerror(err));

		axis_worker_work_fail(job, why, TRUE, FALSE);
		goto done;
	}
	evidence->source_sample = g_steal_pointer(&source_digest);

	/* 6, 7 and 8. */
	say(job, 0.6, "checking: counting the picture's packets");
	axis_verify_measure_packets(&tools, work->plan, src, out, evidence);
	if (not_measured(job, &evidence->source_packets.run, "the original's packets") ||
	    not_measured(job, &evidence->output_packets.run, "the converted file's packets"))
		goto done;
	say(job, 0.7, "checking: hashing the original's copied tracks");
	axis_verify_measure_tracks(&tools, work->plan, src, evidence);
	if (not_measured(job, &evidence->tracks, "the original's tracks"))
		goto done;
	for (k = 0; k < work->plan->n_points; k++) {
		g_autofree gchar *message = g_strdup_printf("checking: comparing the pictures at point %u of %u", k + 1,
			work->plan->n_points);

		say(job, 0.8 + 0.19 * k / MAX(work->plan->n_points, 1), message);
		axis_verify_measure_point(&tools, work->plan, k, src, out, evidence);
		if (not_measured(job, &evidence->points[k].run, "the pictures"))
			goto done;
	}
	if (cancelled(job))
		goto done;

	/* What was read counts only if the volume is still the one proved. */
	status = axis_volume_proof_check(proof);
	if (status != AXIS_VOLUME_PROOF_OK) {
		g_autofree gchar *why = g_strdup_printf("volume unavailable: %s: %s", work->volume,
			axis_volume_proof_status_describe(status));

		axis_worker_work_fail(job, why, TRUE, TRUE);
		goto done;
	}
	{
		AxisVerifyResult *result = g_new0(AxisVerifyResult, 1);

		result->output = g_strdup(work->output);
		result->output_size = work->output_size;
		result->evidence = g_steal_pointer(&evidence);
		g_mutex_lock(&job->lock);
		job->verify_result = result;
		g_mutex_unlock(&job->lock);
	}
	say(job, 1.0, "measured");

done:
	close(out);
	close(src);
	close(area);
}

void
axis_worker_work_drop_output(AxisWorkerRunJob *job)
{
	g_autofree gchar *name = NULL;
	gint anchor;
	gint area;

	g_mutex_lock(&job->lock);
	area = job->output_area;
	anchor = job->output_anchor;
	job->output_anchor = -1;
	if (job->convert_result != NULL)
		name = g_strdup(job->convert_result->output);
	g_mutex_unlock(&job->lock);
	if (area < 0 || name == NULL) {
		if (anchor >= 0)
			close(anchor);
		return;
	}
	if (axis_worker_output_remove_held(area, name, &anchor))
		g_message("job %" G_GINT64_FORMAT ": its output was removed from the work area: the daemon did not take it",
			job->job->id);
	else
		g_warning("job %" G_GINT64_FORMAT ": its output, which the daemon did not take, could not be removed from the "
			"work area (the daemon lists it as a leftover)", job->job->id);
}

#ifdef AXIS_DEBUG
/*
 * Debug builds only (make check), for the tests of a job whose thread does
 * not end: while the FIFO that AXIS_WORKER_TEST_STALL names exists, a job's
 * thread opens it and reads it to its end before it returns, which blocks in
 * the kernel (as a thread does on a mount that stopped answering) until the
 * test closes the FIFO's other end.  A release build has no such hook.
 */
static void
test_stall(AxisWorkerRunJob *job)
{
	const gchar *path = g_getenv("AXIS_WORKER_TEST_STALL");
	gchar byte;
	gint fd;

	if (path == NULL || path[0] == '\0')
		return;
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return;
	g_message("job %" G_GINT64_FORMAT ": its thread stands still on %s (a test)", job->job->id, path);
	for (;;) {
		gssize n = read(fd, &byte, 1);

		if (n > 0 || (n < 0 && errno == EINTR))
			continue;
		break;
	}
	close(fd);
}
#endif

void
axis_worker_work_run(AxisWorkerRunJob *job)
{
	if (g_strcmp0(job->job->kind, AXIS_WORKER_KIND_CAPABILITIES) == 0)
		run_capabilities(job);
	else if (g_strcmp0(job->job->kind, AXIS_WORKER_KIND_SAMPLE) == 0 && job->job->sample != NULL)
		run_sample(job);
	else if (g_strcmp0(job->job->kind, AXIS_WORKER_KIND_CONVERT) == 0 && job->job->convert != NULL)
		run_convert(job);
	else if (g_strcmp0(job->job->kind, AXIS_WORKER_KIND_VERIFY) == 0 && job->job->verify != NULL)
		run_verify(job);
	else
		axis_worker_work_fail(job, "this worker does not run that kind of job", TRUE, FALSE);
#ifdef AXIS_DEBUG
	test_stall(job);
#endif
}

/* ── Start-up ───────────────────────────────────────────────────── */

static gchar *
cache_path(const AxisWorkerConfig *config)
{
	g_autofree gchar *name = g_strdup_printf("capabilities-%u.json", config->skip_families);

	return g_build_filename(config->state, name, NULL);
}

/* The measurement kept for this ffmpeg build and these families. */
static AxisEncoderCapabilities *
cached_caps(
	const AxisWorkerConfig *config,
	const gchar            *digest
){
	g_autofree gchar *path = cache_path(config);
	g_autofree gchar *problem = NULL;
	gchar buffer[CACHE_MAX + 1];
	AxisEncoderCapabilities *caps;
	struct stat st;
	gsize length = 0;
	gssize n;
	gint fd;

	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size > CACHE_MAX) {
		close(fd);
		return NULL;
	}
	while (length < CACHE_MAX && (n = read(fd, buffer + length, CACHE_MAX - length)) > 0)
		length += (gsize)n;
	close(fd);
	caps = axis_worker_tool_parse(buffer, length, &problem);
	if (caps == NULL) {
		g_message("the kept capability measurement is not usable (%s); measuring again", problem);
		return NULL;
	}
	if (g_strcmp0(caps->tool_digest, digest) != 0) {
		axis_encoder_capabilities_free(caps);
		return NULL;
	}
	return caps;
}

gboolean
axis_worker_work_store_caps(
	const AxisWorkerConfig        *config,
	const AxisEncoderCapabilities *caps
){
	g_autofree gchar *path = cache_path(config);
	g_autofree gchar *temporary = g_strdup_printf("%s.tmp-%u", path, (guint)getpid());
	g_autofree gchar *text = axis_worker_tool_to_json(caps);
	gsize length = strlen(text);
	gsize written = 0;
	gint fd;

	(void)unlink(temporary);
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return FALSE;
	while (written < length) {
		gssize n = write(fd, text + written, length - written);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			(void)unlink(temporary);
			return FALSE;
		}
		written += (gsize)n;
	}
	if (close(fd) != 0 || rename(temporary, path) != 0) {
		(void)unlink(temporary);
		return FALSE;
	}
	return TRUE;
}

static AxisEncoderCapabilities *
measure(
	const AxisWorkerConfig  *config,
	GError                 **error
){
	g_autofree gchar *template = g_build_filename(config->scratch, AXIS_WORKER_CAPS_PREFIX "XXXXXX", NULL);
	AxisEncoderCapabilities *caps;
	AxisCapabilityTool tool;
	gchar *scratch;

	scratch = g_mkdtemp_full(template, 0700);
	if (scratch == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "no directory for the capability measurement can be made "
			"in the scratch directory: %s", g_strerror(errno));
		return NULL;
	}
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = (const gchar *const *)config->ffmpeg;
	tool.ffprobe = (const gchar *const *)config->ffprobe;
	tool.scratch = scratch;
	tool.skip_families = config->skip_families;
	caps = axis_convert_capabilities_measure(&tool, error);
	axis_worker_work_remove_dir(scratch);
	return caps;
}

#ifdef AXIS_DEBUG
/*
 * Debug builds only (make check), for the tests of a volume that hangs: when
 * AXIS_WORKER_TEST_HANG_VOLUME names a volume's path, anything that proves or
 * checks that volume reads the FIFO that AXIS_WORKER_TEST_HANG_FIFO names
 * until the test closes its other end, as a stat does on a mount whose server
 * is gone.  A release build has no such hook.
 */
void
axis_worker_work_test_hang(const gchar *path)
{
	const gchar *volume = g_getenv("AXIS_WORKER_TEST_HANG_VOLUME");
	const gchar *fifo = g_getenv("AXIS_WORKER_TEST_HANG_FIFO");
	gchar byte;
	gint fd;

	if (volume == NULL || fifo == NULL || g_strcmp0(volume, path) != 0)
		return;
	fd = open(fifo, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return;
	g_message("the volume %s does not answer (a test)", path);
	for (;;) {
		gssize n = read(fd, &byte, 1);

		if (n > 0 || (n < 0 && errno == EINTR))
			continue;
		break;
	}
	close(fd);
}
#endif

AxisVolumeProof *
axis_worker_work_prove_path(
	const gchar *path,
	gchar      **why
){
	AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_MISSING;
	AxisVolumeProof *proof;

	proof = axis_volume_proof_make(path, &status);
	if (proof == NULL && why != NULL)
		*why = g_strdup(axis_volume_proof_status_describe(status));
	return proof;
}

gboolean
axis_worker_work_prepare(
	const AxisWorkerConfig   *config,
	AxisEncoderCapabilities **caps_out,
	GError                  **error
){
	g_autofree gchar *version = NULL;
	g_autofree gchar *digest = NULL;
	AxisEncoderCapabilities *caps;
	AxisCapabilityTool tool;

	*caps_out = NULL;
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = (const gchar *const *)config->ffmpeg;
	tool.ffprobe = (const gchar *const *)config->ffprobe;
	tool.scratch = config->scratch;
	if (!axis_convert_tool_identity(&tool, &version, &digest, error))
		return FALSE;
	caps = cached_caps(config, digest);
	if (caps == NULL) {
		g_message("measuring what this machine's ffmpeg %s can encode (once per build)", version);
		caps = measure(config, error);
		if (caps == NULL)
			return FALSE;
		if (!axis_worker_work_store_caps(config, caps))
			g_warning("the capability measurement could not be kept in the state directory");
	}
	*caps_out = caps;
	return TRUE;
}

/* ── Files ──────────────────────────────────────────────────────── */

/* Removes @name in @parent_fd and everything below it, never following a
 * link and never leaving @device. */
static gboolean
remove_tree(
	gint         parent_fd,
	const gchar *name,
	dev_t        device,
	guint        depth
){
	struct stat st;
	gboolean ok = TRUE;
	struct dirent *entry;
	DIR *dir;
	gint fd;

	if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT;
	if (st.st_dev != device || depth > MAX_DEPTH)
		return FALSE;
	fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	dir = fdopendir(fd);
	if (dir == NULL) {
		close(fd);
		return FALSE;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (!remove_tree(dirfd(dir), entry->d_name, device, depth + 1))
			ok = FALSE;
	}
	closedir(dir);
	if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT)
		ok = FALSE;
	return ok;
}

void
axis_worker_work_remove_dir(const gchar *path)
{
	g_autofree gchar *parent = NULL;
	g_autofree gchar *base = NULL;
	struct stat st;
	gint fd;

	if (path == NULL)
		return;
	parent = g_path_get_dirname(path);
	base = g_path_get_basename(path);
	if (!g_str_has_prefix(base, AXIS_WORKER_JOB_PREFIX) && !g_str_has_prefix(base, AXIS_WORKER_CAPS_PREFIX))
		return;
	fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return;
	if (fstat(fd, &st) == 0 && !remove_tree(fd, base, st.st_dev, 0))
		g_warning("could not remove everything in %s", path);
	close(fd);
}

void
axis_worker_work_clean_scratch(const gchar *scratch)
{
	struct dirent *entry;
	struct stat st;
	DIR *dir;
	gint fd;

	fd = open(scratch, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return;
	if (fstat(fd, &st) != 0 || (dir = fdopendir(fd)) == NULL) {
		close(fd);
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (!g_str_has_prefix(entry->d_name, AXIS_WORKER_JOB_PREFIX) &&
		    !g_str_has_prefix(entry->d_name, AXIS_WORKER_CAPS_PREFIX))
			continue;
		if (remove_tree(dirfd(dir), entry->d_name, st.st_dev, 0))
			g_message("removed what an earlier run left in the scratch directory (%s)", entry->d_name);
		else
			g_warning("could not remove what an earlier run left in the scratch directory (%s)", entry->d_name);
	}
	closedir(dir);
}

void
axis_worker_work_touch_health(const gchar *state)
{
	g_autofree gchar *path = g_build_filename(state, AXIS_WORKER_HEALTH_FILE, NULL);
	gint fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);

	if (fd < 0)
		return;
	if (write(fd, "ok\n", 3) < 0)
		g_debug("the health file could not be written");
	close(fd);
}

gint
axis_worker_work_healthcheck(const gchar *state)
{
	g_autofree gchar *path = g_build_filename(state, AXIS_WORKER_HEALTH_FILE, NULL);
	struct stat st;
	gint64 age;

	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
		g_printerr("axis-worker: not healthy: it has not been in touch with the daemon\n");
		return 1;
	}
	age = g_get_real_time() / G_USEC_PER_SEC - (gint64)st.st_mtime;
	if (age > AXIS_WORKER_HEALTH_MAX_S) {
		g_printerr("axis-worker: not healthy: the last answer from the daemon was %" G_GINT64_FORMAT " s ago\n", age);
		return 1;
	}
	return 0;
}
