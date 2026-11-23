/*
 * axis-estimate-sample.c - the optional sample measurement: encode short
 * excerpts of a few files to measure, rather than guess, a target's output
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An administrator asks for it; it runs as one job (estimate.sample) on a
 * queue of its own, at the lowest priority, one at a time.  For one target
 * it picks up to estimate.sample_files eligible files of each resolution
 * class (spread across their bits per pixel), encodes three excerpts of
 * each (at a quarter, half and three quarters of the file) with the
 * target's encoder and quality, and records the measured output bitrate of
 * each file as a calibration sample.  What it promises:
 *
 *  - It reads media through a read guard (storage.read_only does not stop
 *    it), opening each file as the probe does (axis-probe-file.h); ffmpeg
 *    gets the open file as descriptor 3 with the fd protocol and the
 *    probe's format whitelist, never a path.
 *  - It writes nothing to any disk: the encoder writes its raw stream to a
 *    pipe, which is counted and dropped, and it runs with a file-size limit
 *    of zero, so a file write fails.  Its working directory is a scratch
 *    directory made for the run under the daemon's temporary directory
 *    (refused when that is on or under a volume), so that a file it might
 *    create could only land there; the directory is removed on success,
 *    failure and cancellation, and what a killed daemon left is removed at
 *    start-up (axis_estimator_clean_scratch()).
 *  - Cancelling the job kills the encoder (its whole process group) within
 *    a moment, removes the scratch directory and records nothing more.
 *  - A slow machine is slow: an excerpt has a time limit
 *    (estimate.sample_timeout), the job's progress says which file and
 *    excerpt it is at and how long the rest will take at the speed so far.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-estimator-load.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "api/axis-worker-proto.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library-name.h"
#include "transcode/axis-convert.h"
#include "probe/axis-probe.h"
#include "probe/axis-probe-file.h"
#include "probe/axis-probe-run.h"
#include "storage/axis-storage-open.h"
#include "storage/axis-volume-guard.h"
#include "transcode/axis-estimate-private.h"

#define EXCERPTS          3
#define CANDIDATES        "200"
#define OUTPUT_CAP        AXIS_SAMPLE_OUTPUT_CAP
#define ERROR_CAP         (16 * 1024)
#define MEMORY_LIMIT      (G_GUINT64_CONSTANT(8) * 1024 * 1024 * 1024)
#define SCRATCH_PREFIX    "sample-"
#define MAX_DEPTH         16

/* ── Tuning ─────────────────────────────────────────────────────── */

void
axis_estimator_set_sample_tuning(
	AxisEstimator          *self,
	const AxisSampleTuning *tuning
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	g_strfreev(self->tool_command);
	self->tool_command = NULL;
	g_clear_pointer(&self->scratch_parent, g_free);
	memset(&self->tuning, 0, sizeof(self->tuning));
	if (tuning != NULL) {
		self->tuning = *tuning;
		if (tuning->tool_command != NULL) {
			self->tool_command = g_strdupv((gchar **)tuning->tool_command);
			self->tuning.tool_command = (const gchar *const *)self->tool_command;
		}
		self->scratch_parent = g_strdup(tuning->scratch_parent);
		self->tuning.scratch_parent = self->scratch_parent;
	}
	g_mutex_unlock(&self->lock);
}

/* ── Scratch ────────────────────────────────────────────────────── */

/* Whether @path is a volume's directory or under one, or on the same
 * filesystem as a volume that is a mount point of its own (by the device
 * it was last seen on: a bind mount of a media disk elsewhere).  A volume
 * added on a directory that is not a mount point shares its filesystem
 * with whatever is around it, so only the path counts for it. */
static gboolean
on_a_volume(
	AxisEstimator *self,
	const gchar   *path,
	dev_t          device
){
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	g_autofree gchar *major_minor = g_strdup_printf("%u:%u", major(device), minor(device));
	guint i;

	for (i = 0; volumes != NULL && i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);
		gsize length = strlen(volume->path);

		if (strcmp(path, volume->path) == 0 ||
		    (strncmp(path, volume->path, length) == 0 && path[length] == '/'))
			return TRUE;
		if (volume->mount_root && volume->device != NULL && volume->device[0] != '\0' &&
		    strcmp(volume->device, major_minor) == 0)
			return TRUE;
	}
	return FALSE;
}

gchar *
axis_estimator_scratch_parent(
	AxisEstimator  *self,
	GError        **error
){
	g_autofree gchar *base = NULL;
	g_autofree gchar *name = g_strdup_printf("axis-scratch-%u", (guint)getuid());
	g_autofree gchar *path = NULL;
	g_autofree gchar *real = NULL;
	struct stat st;

	g_mutex_lock(&self->lock);
	base = g_strdup(self->scratch_parent != NULL ? self->scratch_parent : g_get_tmp_dir());
	g_mutex_unlock(&self->lock);
	if (base == NULL || !g_path_is_absolute(base)) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH,
			"the temporary directory is not an absolute path");
		return NULL;
	}
	/* Nothing is made on a volume, not even the directory. */
	{
		g_autofree gchar *base_real = realpath(base, NULL);
		struct stat base_st;

		if (base_real == NULL || stat(base_real, &base_st) != 0 || on_a_volume(self, base_real, base_st.st_dev)) {
			g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH,
				"the temporary directory %s is missing or on a media volume; set TMPDIR to a directory elsewhere",
				base);
			return NULL;
		}
	}
	path = g_build_filename(base, name, NULL);
	if (mkdir(path, 0700) != 0 && errno != EEXIST) {
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH, "cannot make %s: %s", path,
			g_strerror(errno));
		return NULL;
	}
	/* Not a link, ours, closed to others: a directory another user made
	 * in a shared temporary directory is never written to or emptied. */
	if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH,
			"%s is not a private directory of this user", path);
		return NULL;
	}
	real = realpath(path, NULL);
	if (real == NULL || on_a_volume(self, real, st.st_dev)) {
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH,
			"%s is on a media volume; set TMPDIR to a directory elsewhere", path);
		return NULL;
	}
	return g_steal_pointer(&real);
}

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
	gint fd;
	DIR *dir;
	struct dirent *entry;

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

/* Removes the scratch directory @path (a child of the parent). */
static void
remove_scratch(const gchar *path)
{
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autofree gchar *base = g_path_get_basename(path);
	struct stat st;
	gint fd;

	if (!g_str_has_prefix(base, SCRATCH_PREFIX))
		return;
	fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return;
	if (fstat(fd, &st) == 0 && !remove_tree(fd, base, st.st_dev, 0))
		g_warning("estimate: could not remove everything in the scratch directory %s", path);
	close(fd);
}

void
axis_estimator_clean_scratch(AxisEstimator *self)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *parent = axis_estimator_scratch_parent(self, &error);
	struct stat st;
	DIR *dir;
	struct dirent *entry;
	gint fd;

	if (parent == NULL) {
		g_message("estimate: no scratch directory for the sample measurement: %s", error->message);
		return;
	}
	fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return;
	if (fstat(fd, &st) != 0 || (dir = fdopendir(fd)) == NULL) {
		close(fd);
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (!g_str_has_prefix(entry->d_name, SCRATCH_PREFIX))
			continue;
		if (remove_tree(dirfd(dir), entry->d_name, st.st_dev, 0))
			g_message("estimate: removed what an earlier measurement left in the scratch directory");
		else
			g_warning("estimate: could not remove an earlier measurement's scratch directory");
	}
	closedir(dir);
}

/* ── Requests ───────────────────────────────────────────────────── */

gboolean
axis_estimator_request_sample(
	AxisEstimator  *self,
	const gchar    *target,
	const gchar    *requested_by,
	gint64         *job_id,
	gboolean       *created,
	GError        **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(AxisProfileRevision) revision = NULL;
	AxisEstimateTarget resolved;
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
	/* The profile's current revision, which must be usable: the samples
	 * name it. */
	if (!axis_estimator_profile_target(self, target, 0, &resolved, &revision, error))
		return FALSE;
	json_object_set_string_member(payload, "target", target);
	json_object_set_int_member(payload, "revision", revision->id);
	if (requested_by != NULL)
		json_object_set_string_member(payload, "requested_by", requested_by);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_ESTIMATE_SAMPLE_KIND;
	spec.queue = AXIS_ESTIMATE_SAMPLE_QUEUE;
	spec.payload = payload;
	spec.priority = AXIS_ESTIMATE_SAMPLE_PRIORITY;
	/* One measurement at a time, queued or running. */
	spec.dedupe_key = AXIS_ESTIMATE_SAMPLE_KIND ":run";
	spec.max_attempts = 1;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_enqueue(conn, &spec, job_id, created, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_estimator_cancel_sample(
	AxisEstimator  *self,
	gboolean       *cancelled,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	if (cancelled != NULL)
		*cancelled = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The daemon's own measurement or a worker's: at most one exists. */
	if (!axis_db_conn_exec(conn, "SELECT id FROM job WHERE kind IN ('" AXIS_ESTIMATE_SAMPLE_KIND "', '"
	        AXIS_WORKER_KIND_SAMPLE "') AND state IN ('queued', 'running')", 0, NULL, &result, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	for (i = 0; ok && i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = 0;
		AxisJobCancelOutcome outcome = AXIS_JOB_CANCEL_TOO_LATE;

		axis_db_result_get_int64(result, i, 0, &id);
		ok = axis_job_cancel(conn, id, &outcome, error);
		if (ok && outcome != AXIS_JOB_CANCEL_TOO_LATE && cancelled != NULL)
			*cancelled = TRUE;
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}

void
axis_sample_status_clear(AxisSampleStatus *status)
{
	g_free(status->state);
	g_free(status->target);
	g_free(status->message);
	g_free(status->error);
	memset(status, 0, sizeof(*status));
}

gboolean
axis_estimator_get_sample_status(
	AxisEstimator     *self,
	AxisSampleStatus  *status,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;

	memset(status, 0, sizeof(*status));
	status->progress = -1;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT j.id, j.state::text, j.payload->>'target', coalesce(j.progress, -1)::text, j.progress_message,"
	        " j.last_error, coalesce((extract(epoch FROM j.started_at) * 1000000)::bigint, 0),"
	        " coalesce((extract(epoch FROM j.finished_at) * 1000000)::bigint, 0),"
	        " (SELECT count(*) FROM estimate_sample s WHERE s.job_id = j.id)"
	        " FROM job j WHERE j.kind IN ('" AXIS_ESTIMATE_SAMPLE_KIND "', '" AXIS_WORKER_KIND_SAMPLE "')"
	        /* One queued or running first, then the one that ended last (a
	         * job a person retried keeps its older id). */
	        " ORDER BY j.finished_at DESC NULLS FIRST, j.id DESC LIMIT 1",
	        0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1) {
		axis_db_result_get_int64(result, 0, 0, &status->job_id);
		status->state = g_strdup(axis_db_result_get_text(result, 0, 1));
		status->target = g_strdup(axis_db_result_get_text(result, 0, 2));
		status->progress = g_ascii_strtod(axis_db_result_get_text(result, 0, 3), NULL);
		status->message = g_strdup(axis_db_result_get_text(result, 0, 4));
		status->error = g_strdup(axis_db_result_get_text(result, 0, 5));
		axis_db_result_get_int64(result, 0, 6, &status->started_us);
		axis_db_result_get_int64(result, 0, 7, &status->finished_us);
		axis_db_result_get_int64(result, 0, 8, &status->samples);
	}
	return TRUE;
}

/* ── The job ────────────────────────────────────────────────────── */

typedef struct
{
	gint64   id;
	gint64   volume_id;
	gchar   *root_path;	/* the root's directory on its volume */
	gchar   *rel_path;	/* stored form */
	gchar   *klass;		/* resolution class, or "audio" */
	gdouble  bpp;
} Candidate;

static void
candidate_free(gpointer data)
{
	Candidate *c = data;

	if (c == NULL)
		return;
	g_free(c->root_path);
	g_free(c->rel_path);
	g_free(c->klass);
	g_free(c);
}

typedef struct
{
	AxisEstimator      *self;
	AxisJobContext     *context;
	AxisEstimateTarget  target;
	AxisEstimateModel  *model;
	gchar             **prefix;
	gchar              *scratch;
	guint               excerpt_seconds;
	guint               timeout_ms;
	guint               planned;	/* excerpts */
	guint               done;
	gint64              started_us;
	guint               recorded;
} Measure;

static gchar **
tool_prefix(
	AxisEstimator  *self,
	GError        **error
){
	g_autofree gchar *name = NULL;
	gchar **prefix = NULL;
	gchar *program;

	g_mutex_lock(&self->lock);
	if (self->tool_command != NULL)
		prefix = g_strdupv(self->tool_command);
	g_mutex_unlock(&self->lock);
	if (prefix != NULL)
		return prefix;

	name = axis_estimator_get_string(self, AXIS_ESTIMATE_SETTING_FFMPEG, "ffmpeg");
	if (strchr(name, '/') != NULL)
		program = g_path_is_absolute(name) && g_file_test(name, G_FILE_TEST_IS_REGULAR | G_FILE_TEST_IS_EXECUTABLE) ?
			g_strdup(name) : NULL;
	else
		program = g_find_program_in_path(name);
	if (program == NULL) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_TOOL,
			"ffmpeg is not installed where the setting \"ffmpeg program\" says (estimate.ffmpeg_path).");
		return NULL;
	}
	prefix = g_new0(gchar *, 2);
	prefix[0] = program;
	return prefix;
}

/* The files to measure: of each class, up to @per_class eligible files at
 * evenly spread places of their bits per pixel. */
static gboolean
choose(
	Measure    *m,
	guint       per_class,
	GPtrArray **chosen,
	GError    **error
){
	g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(candidate_free);
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(candidate_free);
	g_autoptr(GHashTable) classes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisEstimateRows) rows = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *scope = NULL;
	g_autofree gchar *sql = NULL;
	GHashTableIter iter;
	gpointer value;
	AxisDbConn *conn;
	guint i;

	*chosen = NULL;
	/* The roots the profile is for (fixed names from the target's bits). */
	{
		GString *types = g_string_new(NULL);
		guint type;

		for (type = 0; type < AXIS_MEDIA_TYPE_COUNT; type++) {
			if ((m->target.root_types & AXIS_TARGET_ROOT(type)) != 0)
				g_string_append_printf(types, "%s'%s'", types->len > 0 ? ", " : "",
					axis_media_type_to_string((AxisMediaType)type));
		}
		scope = g_strdup_printf("%s AND r.media_type IN (%s)", m->target.media == AXIS_TARGET_VIDEO ?
			"f.kind = 'video'" : "f.kind = 'audio'", types->len > 0 ? types->str : "NULL");
		g_string_free(types, TRUE);
	}
	sql = g_strdup_printf(
		"SELECT id, volume_id, root_path, rel_path, klass, bpp FROM ("
		" SELECT f.id, f.volume_id, r.rel_path AS root_path, f.rel_path,"
		"  CASE WHEN f.kind = 'audio' THEN 'audio' ELSE coalesce(m.resolution_class, 'other') END AS klass,"
		"  coalesce(m.bits_per_pixel, -1) AS bpp,"
		"  row_number() OVER (PARTITION BY CASE WHEN f.kind = 'audio' THEN 'audio'"
		"   ELSE coalesce(m.resolution_class, 'other') END ORDER BY m.bits_per_pixel NULLS LAST, f.id) AS n"
		" FROM library_file f JOIN library_root r ON r.id = f.root_id JOIN volume v ON v.id = f.volume_id"
		" JOIN media_info m ON m.file_id = f.id"
		" WHERE %s AND f.state = 'present' AND NOT f.pinned AND f.nlink = 1 AND m.status = 'ok' AND NOT f.changed"
		"  AND v.state IN ('online', 'read_only')) c"
		" WHERE n <= " CANDIDATES " ORDER BY klass, bpp, id", scope);

	conn = axis_db_pool_acquire(m->self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_estimator_timed_exec(conn, "60s", sql, 0, NULL, &result, error))
		goto fail;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Candidate *c = g_new0(Candidate, 1);

		axis_db_result_get_int64(result, i, 0, &c->id);
		axis_db_result_get_int64(result, i, 1, &c->volume_id);
		c->root_path = g_strdup(axis_db_result_get_text(result, i, 2));
		c->rel_path = g_strdup(axis_db_result_get_text(result, i, 3));
		c->klass = g_strdup(axis_db_result_get_text(result, i, 4));
		c->bpp = g_ascii_strtod(axis_db_result_get_text(result, i, 5), NULL);
		g_array_append_val(ids, c->id);
		g_ptr_array_add(candidates, c);
	}
	if (ids->len > 0 && !axis_estimator_load_files(conn, (const gint64 *)ids->data, ids->len, &rows, error))
		goto fail;
	axis_db_pool_release(m->self->pool, conn);

	/* Only files the target would convert (an album's other files do not
	 * matter for what an encoder makes of one track). */
	for (i = 0; rows != NULL && i < rows->files->len; i++) {
		AxisEstimateFile file = *(const AxisEstimateFile *)g_ptr_array_index(rows->files, i);
		AxisEstimateResult result_one;
		guint j;

		file.index.unit = file.index.unit != NULL ? file.index.unit : "";
		file.index.unit_files = 1;
		axis_estimate_eligibility(&file, &m->target, m->model, &result_one);
		if (!axis_estimate_result_converts(&result_one))
			continue;
		for (j = 0; j < candidates->len; j++) {
			Candidate *c = g_ptr_array_index(candidates, j);

			if (c != NULL && c->id == file.index.file_id) {
				GPtrArray *list = g_hash_table_lookup(classes, c->klass);

				if (list == NULL) {
					list = g_ptr_array_new_with_free_func(candidate_free);
					g_hash_table_insert(classes, g_strdup(c->klass), list);
				}
				g_ptr_array_add(list, g_ptr_array_steal_index(candidates, j));
				break;
			}
		}
	}

	g_hash_table_iter_init(&iter, classes);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		GPtrArray *list = value;
		guint n = MIN(per_class, list->len);
		guint k;

		for (k = 0; k < n; k++) {
			guint at = (guint)(((gdouble)k + 0.5) * list->len / n);
			Candidate *c = g_ptr_array_index(list, MIN(at, list->len - 1));

			if (c == NULL)
				continue;
			g_ptr_array_index(list, MIN(at, list->len - 1)) = NULL;
			g_ptr_array_add(out, c);
		}
	}
	*chosen = g_steal_pointer(&out);
	return TRUE;

fail:
	axis_db_pool_release(m->self->pool, conn);
	return FALSE;
}

static void
say(
	Measure     *m,
	guint        file,
	guint        files,
	guint        excerpt
){
	g_autofree gchar *message = NULL;
	gdouble fraction = m->planned > 0 ? (gdouble)m->done / m->planned : 0;

	if (m->done > 0 && m->planned > m->done) {
		gdouble per = (gdouble)(g_get_monotonic_time() - m->started_us) / G_USEC_PER_SEC / m->done;
		gdouble left = per * (m->planned - m->done);

		message = g_strdup_printf("file %u of %u, excerpt %u of %u; about %.0f min left at the speed so far "
			"(%.0f s an excerpt)", file, files, excerpt, EXCERPTS, left / 60.0 + 0.5, per);
	} else {
		message = g_strdup_printf("file %u of %u, excerpt %u of %u", file, files, excerpt, EXCERPTS);
	}
	axis_job_context_set_progress(m->context, MIN(fraction, 0.99), message);
}

/* What measuring one file means: the stream, quality, depth and excerpts
 * the target gives it.  The daemon's own measurement and a worker's plan
 * (and the check of a worker's result) work it out the same way. */
typedef struct
{
	AxisEstimateShape      shape;
	const AxisMediaStream *stream;	/* in the file's facts */
	gint                   quality;
	gint                   depth;
	gdouble                length;
	gdouble                starts[EXCERPTS];
} FilePlan;

/* FALSE when @file is not one to measure for @target. */
static gboolean
plan_file(
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	guint                     excerpt_seconds,
	const AxisEstimateFile   *file,
	FilePlan                 *plan
){
	const AxisMediaInfo *info = file->info;
	AxisEstimateResult result;
	guint k;

	memset(plan, 0, sizeof(*plan));
	{
		AxisEstimateFile one = *file;

		one.index.unit = one.index.unit != NULL ? one.index.unit : "";
		one.index.unit_files = 1;
		axis_estimate_eligibility(&one, target, model, &result);
	}
	if (!axis_estimate_result_converts(&result) || info == NULL)
		return FALSE;
	axis_estimate_shape_init(&plan->shape, info);
	plan->stream = target->media == AXIS_TARGET_VIDEO ? plan->shape.video : plan->shape.audio;
	if (plan->stream == NULL || plan->shape.seconds <= 0)
		return FALSE;
	plan->quality = result.quality;
	plan->depth = info->hdr_format == AXIS_HDR_HDR10 || info->hdr_format == AXIS_HDR_HLG ? target->hdr_bit_depth :
		target->bit_depth;
	plan->length = MIN((gdouble)excerpt_seconds, plan->shape.seconds / (EXCERPTS + 1));
	if (plan->length < 0.5)
		return FALSE;
	for (k = 0; k < EXCERPTS; k++)
		plan->starts[k] = plan->shape.seconds * (k + 1) / (EXCERPTS + 1) - plan->length / 2.0;
	return TRUE;
}

/* The calibration sample of @file measured as @plan says: @bytes over
 * @seconds of excerpts. */
static void
fill_sample(
	const AxisEstimateTarget *target,
	const AxisEstimateFile   *file,
	const FilePlan           *plan,
	guint64                   bytes,
	gdouble                   seconds,
	AxisEstimateSample       *sample
){
	const AxisMediaStream *stream = plan->stream;

	memset(sample, 0, sizeof(*sample));
	sample->target = target->id;
	sample->revision_id = target->revision_id;
	sample->codec = target->codec;
	sample->family = target->encoder;
	sample->quality = plan->quality;
	sample->part = target->media == AXIS_TARGET_VIDEO ? "video" : "audio";
	sample->source_codec = stream->codec;
	sample->seconds = seconds;
	sample->output_bitrate = (gint64)((gdouble)bytes * 8.0 / seconds);
	/* What it was encoded to, so that it applies to every revision whose
	 * output is the same for its cell. */
	sample->has_output_facts = TRUE;
	if (target->media == AXIS_TARGET_VIDEO) {
		sample->width = stream->width;
		sample->height = stream->height;
		sample->frame_rate = plan->shape.fps;
		sample->source_bitrate = plan->shape.video_bitrate;
		sample->out_bit_depth = plan->depth;
	} else {
		sample->source_bitrate = stream->bitrate > 0 ? stream->bitrate :
			(gint64)((gdouble)file->index.size * 8.0 / plan->shape.seconds);
		sample->sample_rate = stream->sample_rate;
		sample->channels = stream->channels;
		sample->bit_depth = stream->bit_depth_audio;
		sample->nominal_bitrate = target->audio_bitrate_mono > 0 && stream->channels == 1 ?
			target->audio_bitrate_mono : target->audio_bitrate;
		sample->reduce_to_cd = target->codec == AXIS_TCODEC_FLAC && target->reduce_to_cd &&
			(stream->bit_depth_audio > 16 || stream->sample_rate > 48000);
	}
}

/* Measures one file; FALSE (with an error) only for a cancellation or a
 * database failure: a file that cannot be measured is skipped. */
static gboolean
measure_file(
	Measure          *m,
	const Candidate  *c,
	guint             position,
	guint             files,
	GError          **error
){
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(AxisEstimateRows) rows = NULL;
	const AxisEstimateFile *file;
	AxisEstimateSample sample;
	AxisProbeFile pf;
	const AxisMediaStream *stream;
	FilePlan plan;
	guint64 bytes = 0;
	gdouble seconds = 0;
	gdouble length;
	gint quality;
	gint depth;
	AxisDbConn *conn;
	guint k;

	conn = axis_db_pool_acquire(m->self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_estimator_load_files(conn, &c->id, 1, &rows, error)) {
		axis_db_pool_release(m->self->pool, conn);
		return FALSE;
	}
	axis_db_pool_release(m->self->pool, conn);
	if (rows->files->len != 1)
		return TRUE;
	file = g_ptr_array_index(rows->files, 0);
	if (!plan_file(&m->target, m->model, m->excerpt_seconds, file, &plan))
		return TRUE;
	stream = plan.stream;
	quality = plan.quality;
	depth = plan.depth;
	length = plan.length;

	guard = axis_volume_guard_acquire(m->self->storage, c->volume_id, AXIS_VOLUME_INTENT_READ, &local);
	if (guard == NULL) {
		g_message("estimate: measurement skips file %" G_GINT64_FORMAT ": %s", c->id, local->message);
		return TRUE;
	}
	if (!axis_probe_file_open(guard, c->root_path, c->rel_path, &pf, &local)) {
		g_message("estimate: measurement skips file %" G_GINT64_FORMAT ": %s", c->id, local->message);
		return TRUE;
	}
	/* The file the index and the probe describe, not one written since. */
	if ((gint64)pf.st.st_size != file->index.size || (gint64)pf.st.st_mtim.tv_sec != file->index.mtime_sec ||
	    (gint)pf.st.st_mtim.tv_nsec != file->index.mtime_nsec || (gint64)pf.st.st_ino != file->index.inode) {
		axis_probe_file_close(&pf);
		return TRUE;
	}

	for (k = 0; k < EXCERPTS; k++) {
		g_auto(GStrv) argv = NULL;
		AxisProbeRunSpec spec;
		AxisProbeRunResult run;
		gdouble start = plan.starts[k];
		void (*hook)(const gchar *, gpointer);
		gpointer hook_data;

		say(m, position, files, k + 1);
		if (lseek(pf.fd, 0, SEEK_SET) != 0) {
			axis_probe_file_close(&pf);
			return TRUE;
		}
		argv = axis_estimate_sample_argv((const gchar *const *)m->prefix, &m->target, stream, quality, depth,
			MAX(start, 0.0), length);
		memset(&spec, 0, sizeof(spec));
		spec.argv = (const gchar *const *)argv;
		spec.fd = pf.fd;
		spec.timeout_ms = m->timeout_ms;
		spec.output_cap = (gsize)MIN(OUTPUT_CAP, (guint64)G_MAXSIZE);
		spec.error_cap = ERROR_CAP;
		spec.cancellable = axis_job_context_get_cancellable(m->context);
		spec.memory_limit = MEMORY_LIMIT;
		spec.discard_output = TRUE;
		spec.working_directory = m->scratch;
		/* Several encoder threads use CPU time faster than the clock. */
		spec.cpu_seconds = m->timeout_ms / 1000 * 16 + 60;
		axis_probe_run(&spec, &run);

		g_mutex_lock(&m->self->lock);
		hook = m->self->tuning.excerpt_hook;
		hook_data = m->self->tuning.hook_data;
		g_mutex_unlock(&m->self->lock);
		if (hook != NULL)
			hook(m->scratch, hook_data);

		if (run.status == AXIS_PROBE_RUN_CANCELLED || axis_job_context_is_cancelled(m->context)) {
			axis_probe_run_result_clear(&run);
			axis_probe_file_close(&pf);
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "the measurement was stopped");
			return FALSE;
		}
		if (run.status != AXIS_PROBE_RUN_OK || run.output_bytes == 0) {
			/* Standard error is bytes, not a string. */
			g_autofree gchar *raw = run.errors->len > 0 ?
				g_strndup((const gchar *)run.errors->data, run.errors->len) : NULL;
			g_autofree gchar *said = raw != NULL ? axis_library_text_clean(raw, 200) : NULL;

			g_message("estimate: measuring file %" G_GINT64_FORMAT " %s%s%s", c->id,
				axis_probe_run_status_describe(run.status), said != NULL ? ": " : "", said != NULL ? said : "");
			axis_probe_run_result_clear(&run);
			axis_probe_file_close(&pf);
			m->done += EXCERPTS - k;
			return TRUE;
		}
		bytes += run.output_bytes;
		seconds += length;
		m->done++;
		axis_probe_run_result_clear(&run);
	}
	axis_probe_file_close(&pf);

	/* What was read is only trusted if the volume is still the one the
	 * guard checked. */
	if (!axis_volume_guard_check(guard, &local)) {
		g_message("estimate: measurement drops file %" G_GINT64_FORMAT ": %s", c->id, local->message);
		return TRUE;
	}

	fill_sample(&m->target, file, &plan, bytes, seconds, &sample);
	if (!axis_estimator_record_sample(m->self, NULL, &sample, "measured", axis_job_context_get_id(m->context),
	        c->id, &local)) {
		if (local->domain == AXIS_ESTIMATE_ERROR) {
			g_message("estimate: measurement of file %" G_GINT64_FORMAT " not recorded: %s", c->id, local->message);
			return TRUE;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	m->recorded++;
	return TRUE;
}

gboolean
axis_estimator_run_sample_job(
	AxisEstimator   *self,
	AxisJobContext  *context,
	GError         **error
){
	g_autoptr(GPtrArray) chosen = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *template = NULL;
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *target_id = payload != NULL ? json_object_get_string_member_with_default(payload, "target", NULL) :
		NULL;
	gint64 revision_id = payload != NULL ? json_object_get_int_member_with_default(payload, "revision", 0) : 0;
	Measure m;
	gboolean ok = TRUE;
	guint i;

	memset(&m, 0, sizeof(m));
	m.self = self;
	m.context = context;
	/* The revision the request named (a job queued before profiles names
	 * a built-in by its slug only: its current revision). */
	{
		g_autoptr(AxisProfileRevision) revision = NULL;
		g_autoptr(GError) local = NULL;
		AxisEstimateTarget built;
		AxisEstimateConfig config;
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);

		if (conn == NULL)
			return FALSE;
		if (revision_id > 0)
			ok = axis_profile_store_revision_by_id(conn, revision_id, &revision, &local);
		else
			ok = axis_profile_store_current(conn, target_id, &revision, &local);
		axis_db_pool_release(self->pool, conn);
		if (!ok) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (revision == NULL || !axis_profile_revision_target(revision, &built)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "the measurement's profile "
				"revision is not one this daemon can use");
			return FALSE;
		}
		axis_estimator_get_config(self, &config);
		axis_estimate_target_configure(&built, &config, &m.target);
	}
	m.prefix = tool_prefix(self, error);
	if (m.prefix == NULL)
		return FALSE;
	parent = axis_estimator_scratch_parent(self, error);
	if (parent == NULL) {
		g_strfreev(m.prefix);
		return FALSE;
	}
	template = g_strdup_printf("%s/" SCRATCH_PREFIX "%" G_GINT64_FORMAT "-XXXXXX", parent,
		axis_job_context_get_id(context));
	m.scratch = g_mkdtemp_full(template, 0700);
	if (m.scratch == NULL) {
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH, "cannot make a scratch directory: %s",
			g_strerror(errno));
		g_strfreev(m.prefix);
		return FALSE;
	}
	m.scratch = g_strdup(m.scratch);
	m.excerpt_seconds = (guint)CLAMP(axis_estimator_get_integer(self, AXIS_ESTIMATE_SETTING_SAMPLE_SECONDS, 20), 2,
		120);
	g_mutex_lock(&self->lock);
	m.timeout_ms = self->tuning.timeout_ms;
	g_mutex_unlock(&self->lock);
	if (m.timeout_ms == 0)
		m.timeout_ms = (guint)CLAMP(axis_estimator_get_integer(self, AXIS_ESTIMATE_SETTING_SAMPLE_TIMEOUT, 1800), 30,
			6 * 3600) * 1000;

	m.model = axis_estimator_get_model(self, error);
	if (m.model == NULL)
		ok = FALSE;
	if (ok)
		ok = choose(&m, (guint)CLAMP(axis_estimator_get_integer(self, AXIS_ESTIMATE_SETTING_SAMPLE_FILES, 3), 1, 10),
			&chosen, error);
	if (ok) {
		m.planned = chosen->len * EXCERPTS;
		m.started_us = g_get_monotonic_time();
		g_message("estimate: measuring %u files for %s (%u excerpts of up to %u s)", chosen->len, m.target.id,
			m.planned, m.excerpt_seconds);
		for (i = 0; ok && i < chosen->len; i++)
			ok = measure_file(&m, g_ptr_array_index(chosen, i), i + 1, chosen->len, error);
	}

	/* Whatever happened: nothing is left behind. */
	remove_scratch(m.scratch);
	if (ok) {
		g_message("estimate: the measurement for %s recorded %u samples in %.0f s", m.target.id, m.recorded,
			(gdouble)(g_get_monotonic_time() - m.started_us) / G_USEC_PER_SEC);
		axis_job_context_set_progress(context, 1.0, "done");
	}
	axis_estimator_model_unref(m.model);
	g_strfreev(m.prefix);
	g_free(m.scratch);
	return ok;
}

/* ── The measurement on a worker ────────────────────────────────── */

/*
 * A worker gets a plan made here, when the measurement is asked for: the
 * same files the daemon would choose, each with the stream, quality, depth
 * and excerpts plan_file() gives it, named by its volume's UUID and its
 * paths relative to the volume.  The plan is kept in the job's payload,
 * with each file's id and inode, so that what is checked when the worker
 * reports is what it was asked: the worker reports only how many bytes
 * each excerpt's encoder wrote, and everything else (the seconds measured,
 * the stream, the facts the sample keeps) comes from here, after the file
 * is found unchanged and planned the same way again.
 */

#define REMOTE_ATTEMPTS        3
#define REMOTE_PLAUSIBLE_RATIO 20.0

static void
measure_settings(
	AxisEstimator *self,
	guint         *excerpt_seconds,
	guint         *timeout_ms,
	guint         *per_class
){
	*excerpt_seconds = (guint)CLAMP(axis_estimator_get_integer(self, AXIS_ESTIMATE_SETTING_SAMPLE_SECONDS, 20), 2, 120);
	g_mutex_lock(&self->lock);
	*timeout_ms = self->tuning.timeout_ms;
	g_mutex_unlock(&self->lock);
	if (*timeout_ms == 0)
		*timeout_ms = (guint)CLAMP(axis_estimator_get_integer(self, AXIS_ESTIMATE_SETTING_SAMPLE_TIMEOUT, 1800), 30,
			6 * 3600) * 1000;
	*per_class = (guint)CLAMP(axis_estimator_get_integer(self, AXIS_ESTIMATE_SETTING_SAMPLE_FILES, 3), 1, 10);
}

static const gchar *
volume_uuid(
	GPtrArray *volumes,
	gint64     id
){
	guint i;

	for (i = 0; volumes != NULL && i < volumes->len; i++) {
		const AxisVolumeInfo *v = g_ptr_array_index(volumes, i);

		if (v->id == id)
			return v->uuid;
	}
	return NULL;
}

gboolean
axis_estimator_request_remote_sample(
	AxisEstimator  *self,
	const gchar    *target_slug,
	const gchar    *requested_by,
	gint64         *job_id,
	gboolean       *created,
	GError        **error
){
	g_autoptr(AxisProfileRevision) revision = NULL;
	g_autoptr(GPtrArray) chosen = NULL;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(AxisSampleDescription) description = NULL;
	g_autoptr(JsonObject) payload = NULL;
	g_autofree gchar *plan_text = NULL;
	g_autofree gchar *problem = NULL;
	JsonObject *requires;
	JsonArray *need_volumes;
	JsonArray *files;
	gboolean ten_bit = FALSE;
	AxisJobSpec spec;
	AxisDbConn *conn;
	Measure m;
	guint per_class = 3;
	guint i;
	gboolean ok = TRUE;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
	if (created != NULL)
		*created = FALSE;
	memset(&m, 0, sizeof(m));
	m.self = self;
	if (!axis_estimator_profile_target(self, target_slug, 0, &m.target, &revision, error))
		return FALSE;
	measure_settings(self, &m.excerpt_seconds, &m.timeout_ms, &per_class);
	m.model = axis_estimator_get_model(self, error);
	if (m.model == NULL)
		return FALSE;
	if (!choose(&m, per_class, &chosen, error)) {
		axis_estimator_model_unref(m.model);
		return FALSE;
	}
	volumes = axis_storage_list_volumes(self->storage);

	description = axis_sample_description_new();
	description->media = m.target.media;
	description->codec = m.target.codec;
	description->family = m.target.encoder;
	description->target_quality = m.target.quality;
	description->audio_bitrate = CLAMP(m.target.audio_bitrate, 0, AXIS_SAMPLE_MAX_AUDIO_BITRATE);
	description->audio_bitrate_mono = CLAMP(m.target.audio_bitrate_mono, 0, AXIS_SAMPLE_MAX_AUDIO_BITRATE);
	description->audio_quality = CLAMP(m.target.audio_quality, -1, 9);
	description->reduce_to_cd = m.target.reduce_to_cd;
	description->excerpt_timeout_ms = CLAMP(m.timeout_ms, AXIS_SAMPLE_MIN_TIMEOUT_MS, AXIS_SAMPLE_MAX_TIMEOUT_MS);

	payload = json_object_new();
	requires = json_object_new();
	need_volumes = json_array_new();
	files = json_array_new();

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		ok = FALSE;
		goto out;
	}
	for (i = 0; chosen != NULL && i < chosen->len && description->files->len < AXIS_SAMPLE_MAX_FILES; i++) {
		const Candidate *c = g_ptr_array_index(chosen, i);
		g_autoptr(AxisEstimateRows) rows = NULL;
		const gchar *uuid = volume_uuid(volumes, c->volume_id);
		const AxisEstimateFile *file;
		AxisSampleFilePlan entry;
		JsonObject *meta;
		FilePlan plan;
		guint k;
		guint n;
		gboolean listed = FALSE;

		if (uuid == NULL || !axis_storage_file_path_valid(c->root_path, c->rel_path, NULL))
			continue;
		if (!axis_estimator_load_files(conn, &c->id, 1, &rows, error)) {
			ok = FALSE;
			break;
		}
		if (rows->files->len != 1)
			continue;
		file = g_ptr_array_index(rows->files, 0);
		if (!plan_file(&m.target, m.model, m.excerpt_seconds, file, &plan))
			continue;
		memset(&entry, 0, sizeof(entry));
		entry.volume = (gchar *)uuid;
		entry.root = c->root_path;
		entry.path = c->rel_path;
		entry.size = file->index.size;
		entry.mtime_sec = file->index.mtime_sec;
		entry.mtime_nsec = file->index.mtime_nsec;
		entry.stream = plan.stream->index;
		entry.quality = plan.quality;
		if (m.target.media == AXIS_TARGET_VIDEO) {
			entry.bit_depth = plan.depth >= 10 && m.target.codec == AXIS_TCODEC_HEVC ? 10 : 8;
			ten_bit = ten_bit || entry.bit_depth == 10;
		} else {
			entry.channels = CLAMP(plan.stream->channels, 0, AXIS_SAMPLE_MAX_CHANNELS);
			entry.sample_rate = CLAMP(plan.stream->sample_rate, 0, AXIS_SAMPLE_MAX_SAMPLE_RATE);
			entry.sample_bits = CLAMP(plan.stream->bit_depth_audio, 0, AXIS_SAMPLE_MAX_SAMPLE_BITS);
		}
		entry.n_excerpts = EXCERPTS;
		entry.length_ms = (gint64)(plan.length * 1000.0);
		for (k = 0; k < EXCERPTS; k++)
			entry.start_ms[k] = (gint64)(MAX(plan.starts[k], 0.0) * 1000.0);
		axis_sample_description_add_file(description, &entry);

		meta = json_object_new();
		json_object_set_int_member(meta, "file", c->id);
		json_object_set_int_member(meta, "inode", file->index.inode);
		json_object_set_int_member(meta, "volume_id", c->volume_id);
		json_array_add_object_element(files, meta);
		n = json_array_get_length(need_volumes);
		for (k = 0; k < n; k++)
			listed = listed || g_strcmp0(json_array_get_string_element(need_volumes, k), uuid) == 0;
		if (!listed)
			json_array_add_string_element(need_volumes, uuid);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		goto out;
	if (description->files->len == 0) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND,
			"there is no file that this profile would convert to measure on a worker");
		ok = FALSE;
		goto out;
	}
	if (!axis_sample_description_check(description, &problem)) {
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "the plan for a worker could not be "
			"made: %s", problem);
		ok = FALSE;
		goto out;
	}
	plan_text = axis_sample_description_to_json(description);

	/* What a worker must have shown before it is offered the job. */
	if (m.target.media == AXIS_TARGET_VIDEO) {
		const AxisConvertEncoder *encoder = axis_convert_encoder_for(m.target.codec, m.target.encoder);

		if (encoder == NULL) {
			g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
				"the profile's encoder is not one a worker can show it has");
			ok = FALSE;
			goto out;
		}
		json_object_set_string_member(requires, "encoder", encoder->name);
		json_object_set_boolean_member(requires, "ten_bit", ten_bit);
	}
	json_object_set_array_member(requires, "volumes", json_array_ref(need_volumes));
	json_object_set_string_member(payload, "target", target_slug);
	json_object_set_int_member(payload, "revision", revision->id);
	if (requested_by != NULL)
		json_object_set_string_member(payload, "requested_by", requested_by);
	json_object_set_int_member(payload, "excerpt_seconds", m.excerpt_seconds);
	json_object_set_object_member(payload, "requires", json_object_ref(requires));
	json_object_set_string_member(payload, "plan", plan_text);
	json_object_set_array_member(payload, "files", json_array_ref(files));

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_WORKER_KIND_SAMPLE;
	spec.queue = AXIS_WORKER_QUEUE;
	spec.payload = payload;
	spec.priority = AXIS_ESTIMATE_SAMPLE_PRIORITY;
	/* One measurement at a time, the daemon's own or a worker's. */
	spec.dedupe_key = AXIS_ESTIMATE_SAMPLE_KIND ":run";
	spec.max_attempts = REMOTE_ATTEMPTS;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		ok = FALSE;
		goto out;
	}
	ok = axis_job_enqueue(conn, &spec, job_id, created, error);
	axis_db_pool_release(self->pool, conn);

out:
	json_object_unref(requires);
	json_array_unref(need_volumes);
	json_array_unref(files);
	axis_estimator_model_unref(m.model);
	return ok;
}

/* The payload a request above made, read back; NULL (and @error) for one
 * that is not. */
static JsonObject *
remote_payload(
	const gchar  *text,
	GError      **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
			"the job's payload is not one this daemon made");
		return NULL;
	}
	return json_object_ref(json_node_get_object(root));
}

static AxisSampleDescription *
remote_plan(
	JsonObject  *payload,
	GError     **error
){
	const gchar *text = json_object_get_string_member_with_default(payload, "plan", NULL);
	g_autofree gchar *problem = NULL;
	AxisSampleDescription *description;

	description = text != NULL ? axis_sample_description_parse(text, strlen(text), &problem) : NULL;
	if (description == NULL)
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "the job's plan is not one this daemon "
			"made: %s", problem != NULL ? problem : "it is missing");
	return description;
}

gboolean
axis_estimator_describe_remote_sample(
	const gchar            *payload_text,
	AxisSampleDescription **description,
	GError                **error
){
	g_autoptr(JsonObject) payload = remote_payload(payload_text, error);

	*description = NULL;
	if (payload == NULL)
		return FALSE;
	*description = remote_plan(payload, error);
	return *description != NULL;
}

/* Whether a measured bitrate could be what the encoder made: within a
 * factor of REMOTE_PLAUSIBLE_RATIO of what the model predicts for a
 * picture; for audio, at most twice the source or four times the target's
 * bitrate. */
static gboolean
plausible(
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	const AxisEstimateFile   *file,
	const FilePlan           *plan,
	const AxisEstimateSample *sample,
	const gchar             **why
){
	gdouble bitrate = (gdouble)sample->output_bitrate;

	if (bitrate < 1000.0 || !axis_estimate_sample_is_valid(sample)) {
		*why = "the bitrate is outside what a sample may have";
		return FALSE;
	}
	if (target->media == AXIS_TARGET_VIDEO) {
		AxisEstimatePart part;
		gboolean hdr = file->info->hdr_format == AXIS_HDR_HDR10 || file->info->hdr_format == AXIS_HDR_HLG;
		gdouble predicted;

		memset(&part, 0, sizeof(part));
		if (!axis_estimate_model_video(model, target, &plan->shape, plan->stream->codec, hdr, plan->quality, &part) ||
		    plan->shape.seconds <= 0 || part.mid <= 0) {
			*why = "there is no prediction to compare it with";
			return FALSE;
		}
		predicted = part.mid * 8.0 / plan->shape.seconds;
		if (bitrate > predicted * REMOTE_PLAUSIBLE_RATIO || bitrate < predicted / REMOTE_PLAUSIBLE_RATIO) {
			*why = "the bitrate is far from anything the encoder would make";
			return FALSE;
		}
		return TRUE;
	}
	{
		gdouble nominal = (gdouble)MAX(sample->nominal_bitrate, 0);
		gdouble ceiling = MAX(MAX((gdouble)sample->source_bitrate * 2.0, nominal * 4.0), 64000.0);

		if (bitrate > ceiling) {
			*why = "the bitrate is far above the source's and the target's";
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
axis_estimator_record_remote_sample(
	AxisEstimator           *self,
	AxisDbConn              *conn,
	gint64                   job_id,
	const gchar             *payload_text,
	const AxisSampleResult  *result,
	const gchar             *worker,
	guint                   *recorded,
	guint                   *refused,
	GError                 **error
){
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(AxisSampleDescription) description = NULL;
	g_autoptr(AxisProfileRevision) revision = NULL;
	AxisEstimateModel *model = NULL;
	AxisEstimateTarget built;
	AxisEstimateTarget target;
	AxisEstimateConfig config;
	JsonArray *files;
	gint64 revision_id;
	guint excerpt_seconds;
	gboolean ok = TRUE;
	guint i;

	g_return_val_if_fail(self != NULL && conn != NULL && result != NULL, FALSE);

	*recorded = 0;
	*refused = 0;
	payload = remote_payload(payload_text, error);
	if (payload == NULL)
		return FALSE;
	description = remote_plan(payload, error);
	if (description == NULL)
		return FALSE;
	files = json_object_has_member(payload, "files") ? json_object_get_array_member(payload, "files") : NULL;
	if (files == NULL || json_array_get_length(files) != description->files->len) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
			"the job's payload is not one this daemon made");
		return FALSE;
	}

	/* The result as a whole fits the plan, or nothing of it is kept. */
	for (i = 0; i < result->files->len; i++) {
		const AxisSampleFileResult *r = g_ptr_array_index(result->files, i);
		const AxisSampleFilePlan *f;

		if (r->index >= description->files->len) {
			g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_INVALID,
				"the result names a file the plan does not have");
			return FALSE;
		}
		f = g_ptr_array_index(description->files, r->index);
		if (r->measured && r->n_bytes != f->n_excerpts) {
			g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_INVALID,
				"the result has another number of excerpts than the plan");
			return FALSE;
		}
	}

	/* The target as the plan was made for it. */
	revision_id = json_object_get_int_member_with_default(payload, "revision", 0);
	excerpt_seconds = (guint)json_object_get_int_member_with_default(payload, "excerpt_seconds", 0);
	if (revision_id <= 0 || excerpt_seconds < 2 || excerpt_seconds > 120) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
			"the job's payload is not one this daemon made");
		return FALSE;
	}
	if (!axis_profile_store_revision_by_id(conn, revision_id, &revision, error))
		return FALSE;
	if (revision == NULL || !axis_profile_revision_target(revision, &built)) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "the measurement's profile "
			"revision is not one this daemon can use");
		return FALSE;
	}
	axis_estimator_get_config(self, &config);
	axis_estimate_target_configure(&built, &config, &target);
	if (target.media != description->media || target.codec != description->codec ||
	    target.encoder != description->family) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
			"the profile is encoded otherwise now than when the measurement was planned");
		return FALSE;
	}
	model = axis_estimator_get_model(self, error);
	if (model == NULL)
		return FALSE;

	for (i = 0; ok && i < result->files->len; i++) {
		const AxisSampleFileResult *r = g_ptr_array_index(result->files, i);
		const AxisSampleFilePlan *f = g_ptr_array_index(description->files, r->index);
		JsonObject *meta = json_array_get_object_element(files, r->index);
		g_autoptr(AxisEstimateRows) rows = NULL;
		g_autoptr(GError) local = NULL;
		const AxisEstimateFile *file;
		const gchar *why = NULL;
		AxisEstimateSample sample;
		FilePlan plan;
		gint64 file_id = meta != NULL ? json_object_get_int_member_with_default(meta, "file", 0) : 0;
		gint64 inode = meta != NULL ? json_object_get_int_member_with_default(meta, "inode", -1) : -1;
		gint64 volume_id = meta != NULL ? json_object_get_int_member_with_default(meta, "volume_id", 0) : 0;
		guint64 bytes = 0;
		gdouble seconds;
		guint k;

		if (!r->measured)
			continue;
		if (file_id <= 0) {
			(*refused)++;
			continue;
		}
		if (!axis_estimator_load_files(conn, &file_id, 1, &rows, error)) {
			ok = FALSE;
			break;
		}
		file = rows->files->len == 1 ? g_ptr_array_index(rows->files, 0) : NULL;
		/* The file the plan was made of, unchanged since. */
		if (file == NULL || g_strcmp0(file->index.state, "present") != 0 || file->index.changed ||
		    file->index.pinned || file->index.nlink != 1 || file->index.volume_id != volume_id ||
		    file->index.size != f->size || file->index.mtime_sec != f->mtime_sec ||
		    file->index.mtime_nsec != f->mtime_nsec || file->index.inode != inode) {
			why = "the file is not the one the measurement was planned for";
		} else if (!plan_file(&target, model, excerpt_seconds, file, &plan) || plan.stream->index != f->stream ||
		           plan.quality != f->quality || (gint64)(plan.length * 1000.0) != f->length_ms ||
		           (target.media == AXIS_TARGET_VIDEO &&
		            (plan.depth >= 10 && target.codec == AXIS_TCODEC_HEVC ? 10 : 8) != f->bit_depth)) {
			why = "the file would be measured otherwise now than when the measurement was planned";
		}
		if (why == NULL) {
			for (k = 0; k < r->n_bytes; k++) {
				if (r->bytes[k] == 0)
					why = "an excerpt's encoder wrote nothing";
				bytes += r->bytes[k];
			}
		}
		if (why == NULL) {
			/* The seconds are the plan's, never the worker's. */
			seconds = (gdouble)f->length_ms / 1000.0 * f->n_excerpts;
			fill_sample(&target, file, &plan, bytes, seconds, &sample);
			if (!plausible(&target, model, file, &plan, &sample, &why))
				why = why != NULL ? why : "the result is not plausible";
		}
		if (why != NULL) {
			g_message("estimate: worker %s: the result for file %" G_GINT64_FORMAT " is not recorded: %s",
				worker != NULL ? worker : "?", file_id, why);
			(*refused)++;
			continue;
		}
		if (!axis_estimator_record_sample(self, conn, &sample, "measured", job_id, file_id, &local)) {
			if (local->domain == AXIS_ESTIMATE_ERROR) {
				g_message("estimate: worker %s: the result for file %" G_GINT64_FORMAT " is not recorded: %s",
					worker != NULL ? worker : "?", file_id, local->message);
				(*refused)++;
				continue;
			}
			g_propagate_error(error, g_steal_pointer(&local));
			ok = FALSE;
			break;
		}
		(*recorded)++;
	}
	axis_estimator_model_unref(model);
	return ok;
}
