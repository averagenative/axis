/*
 * axis-youtube-fetch.c - the fetch job: the folder yt-dlp works in, the run,
 * and the move of the finished file into the library
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One fetch, step by step:
 *
 *   1. The video's record becomes 'fetching' (only the job the record names
 *      may take it; a job whose download was removed meanwhile does nothing).
 *   2. A write guard on the root's volume: read-only storage, a read-only
 *      mount and a disk that is offline end the fetch here, with nothing
 *      written.  Then: no file has the video's name, and the disk has room
 *      for what yt-dlp said the video takes, twice (the streams and the
 *      merged file; twice the size limit when it gave no size), and
 *      "import.free_space_floor" besides.
 *   3. yt-dlp's folder is made through the guard: ".axis-youtube/<video
 *      id>-<job>" in the root, after what earlier fetches left there is
 *      removed (every folder of this video, and every folder whose job is
 *      neither queued nor running: a daemon that is killed removes
 *      nothing).  yt-dlp is started in it (its working directory is the
 *      folder's descriptor, not a path), and writes only relative names.
 *   4. While it runs, its progress lines are read and recorded once a
 *      second; the downloads layer's tracking carries them to the Downloads
 *      page.  About once a minute the disk's free space is looked at again:
 *      below the floor, yt-dlp is stopped and the fetch fails.
 *   5. When it exits with status 0, the one file "video.mkv" in the folder
 *      ("video.mp4" or "video.webm" for a video YouTube serves as one file,
 *      which yt-dlp does not merge), a regular file on the volume with no
 *      other name, is moved into the library folder under a temporary name
 *      and then given its final name, with the extension it came with, by a
 *      rename that never replaces one (axis-volume-write.h).
 *   6. The folder is removed, whatever happened.  The record says how it
 *      ended; the downloads layer is asked to look at once, and a fetched
 *      video's download is marked imported (its file is the library's).
 *
 * A fetch cut short because the daemon is stopping (axis_youtube_stop(), the
 * job queue's shutdown, a lost lease) removes its folder and fails, saying
 * that Axis stopped: nothing takes it up again by itself.  One cut short by
 * a kill leaves its record and its folder as they were; the record fails at
 * the next look of the downloads layer or the next start, and the folder
 * goes at the next start or the next fetch into that library folder
 * (axis_youtube_recover(), axis_youtube_sweep_staging()).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "youtube/axis-youtube-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/axis-size.h"
#include "db/axis-db-conn.h"
#include "import/axis-import.h"
#include "import/axis-import-name.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-guard.h"
#include "storage/axis-volume-write.h"

/* ── Progress ───────────────────────────────────────────────────── */

typedef struct
{
	AxisYoutube      *self;
	AxisJobContext   *context;
	const gchar      *video_id;
	const gchar      *job_text;
	AxisYoutubeLines  lines;
	gint64            expected;	/* what yt-dlp said the whole takes, or -1 */
	gint64            base;	/* the streams that are done */
	gint64            last_write_us;
	/* The free space, looked at while it runs. */
	GCancellable     *cancel;	/* stops yt-dlp */
	gint              root_fd;
	const gchar      *disk;	/* the volume's name, for a person */
	gint64            space_every_us;
	gint64            last_space_us;
	gchar            *space_problem;	/* why it was stopped, once it is */
} Progress;

static void
progress_write(
	Progress *p,
	gint64    done,
	gint64    rate,
	gint64    eta
){
	g_autofree gchar *done_text = g_strdup_printf("%" G_GINT64_FORMAT, done);
	g_autofree gchar *size_text = p->expected > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, MAX(p->expected, done)) : NULL;
	g_autofree gchar *rate_text = rate >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, rate) : NULL;
	g_autofree gchar *eta_text = eta >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, eta) : NULL;
	const gchar *params[6];

	params[0] = p->video_id;
	params[1] = p->job_text;
	params[2] = done_text;
	params[3] = size_text;
	params[4] = rate_text;
	params[5] = eta_text;
	axis_db_pool_exec(p->self->pool,
		"UPDATE youtube_video SET done_bytes = $3::bigint, size_bytes = $4::bigint, rate_bps = $5::bigint,"
		" eta_s = $6::bigint, updated_at = now()"
		" WHERE video_id = $1::text AND job_id = $2::bigint AND state = 'fetching'", 6, params, NULL, NULL);
	if (p->expected > 0)
		axis_job_context_set_progress(p->context, CLAMP((gdouble)done / (gdouble)MAX(p->expected, done), 0.0, 1.0),
			"Downloading");
	else
		axis_job_context_set_progress(p->context, 0.0, "Downloading");
}

static void
on_line(
	const gchar *line,
	gsize        length,
	gpointer     user_data
){
	Progress *p = user_data;
	AxisYoutubeProgress progress;
	gint64 now = g_get_monotonic_time();
	gint64 done;

	if (!axis_youtube_progress_parse(line, length, &progress))
		return;
	/* A video and its audio are two streams, one after the other; the sum
	 * stays a size whatever the lines say (axis_youtube_progress_add()). */
	done = axis_youtube_progress_add(&p->base, &progress);
	if (!progress.finished && p->last_write_us != 0 && now - p->last_write_us < G_USEC_PER_SEC)
		return;
	p->last_write_us = now;
	progress_write(p, done, progress.finished ? -1 : progress.rate_bps, progress.finished ? -1 : progress.eta_s);
}

/* The free space imports keep is kept while a video comes in too: a disk
 * that something else fills meanwhile, or a video whose size yt-dlp did not
 * say, is not run down to nothing. */
static void
check_space(Progress *p)
{
	gint64 now = g_get_monotonic_time();
	guint64 available = 0;
	guint64 floor = 0;

	if (p->space_problem != NULL || now - p->last_space_us < p->space_every_us)
		return;
	p->last_space_us = now;
	if (!axis_youtube_free_space(p->self, p->root_fd, &available, &floor) || available >= floor)
		return;
	{
		g_autofree gchar *have = axis_size_format(available);
		g_autofree gchar *keep = axis_size_format(floor);

		p->space_problem = g_strdup_printf("The disk %s has %s free, less than the %s Axis keeps free "
			"(import.free_space_floor), so the download was stopped and what it had written was removed.", p->disk, have,
			keep);
	}
	g_cancellable_cancel(p->cancel);
}

static void
on_output(
	const guint8 *data,
	gsize         length,
	gpointer      user_data
){
	Progress *p = user_data;

	axis_youtube_lines_feed(&p->lines, data, length, on_line, p);
	check_space(p);
}

static void
on_cancelled(
	GCancellable *cancellable,
	gpointer      user_data
){
	g_cancellable_cancel(user_data);
}

/* ── One fetch ──────────────────────────────────────────────────── */

typedef struct
{
	AxisYoutube      *self;
	AxisJobContext   *context;
	const gchar      *video_id;
	gchar            *job_text;
	AxisYoutubeVideo *video;
	AxisRootInfo     *root;
	AxisVolumeGuard  *guard;
	gint              root_fd;
	gint              stage_parent;	/* ".axis-youtube" in the root */
	gint              stage_fd;	/* yt-dlp's folder in it */
	gchar            *stage_name;
	guint64           stage_ino;
	gchar            *channel_dir;
	gchar            *season_dir;
	gchar            *file_name;
	gchar            *rel_path;	/* the file, from the volume's top */
	gchar            *disk;	/* the volume's name, for a person */
	guint             dir_mode;
	gint64            placed_bytes;
} Fetch;

static void
fetch_clear(Fetch *f)
{
	if (f->stage_fd >= 0)
		close(f->stage_fd);
	if (f->stage_parent >= 0)
		close(f->stage_parent);
	if (f->root_fd >= 0)
		close(f->root_fd);
	g_clear_pointer(&f->guard, axis_volume_guard_release);
	g_clear_pointer(&f->root, axis_root_info_free);
	g_clear_pointer(&f->video, axis_youtube_video_free);
	g_free(f->job_text);
	g_free(f->stage_name);
	g_free(f->channel_dir);
	g_free(f->season_dir);
	g_free(f->file_name);
	g_free(f->rel_path);
	g_free(f->disk);
}

/* ── What earlier fetches left ──────────────────────────────────── */

/* Whether @name is "<video id>-<job>", a folder a fetch makes; the job. */
static gboolean
staging_name_read(
	const gchar *name,
	gint64      *job_id
){
	g_autofree gchar *id = NULL;
	const gchar *digits;
	gsize n;

	*job_id = 0;
	if (strlen(name) < 13 || name[11] != '-')
		return FALSE;
	id = g_strndup(name, 11);
	digits = name + 12;
	n = strlen(digits);
	if (!axis_youtube_video_id_valid(id) || n < 1 || n > 18 || strspn(digits, "0123456789") != n || digits[0] == '0')
		return FALSE;
	*job_id = g_ascii_strtoll(digits, NULL, 10);
	return TRUE;
}

guint
axis_youtube_sweep_staging(
	AxisYoutube     *self,
	AxisVolumeGuard *guard,
	gint             stage_parent,
	const gchar     *video_id
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) live = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(AxisDbResult) result = NULL;
	struct dirent *entry;
	gboolean jobs_known;
	guint removed = 0;
	guint i;
	gint fd;
	DIR *dir;

	/* The names first, then the jobs: a folder seen here was made by a job
	 * that ran then, so it is running still or has removed it itself. */
	fd = openat(stage_parent, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	dir = fd >= 0 ? fdopendir(fd) : NULL;
	if (dir == NULL) {
		if (fd >= 0)
			close(fd);
		return 0;
	}
	while (names->len < 10000 && (entry = readdir(dir)) != NULL) {
		gint64 job_id;

		if (staging_name_read(entry->d_name, &job_id))
			g_ptr_array_add(names, g_strdup(entry->d_name));
	}
	closedir(dir);
	if (names->len == 0)
		return 0;

	jobs_known = axis_db_pool_exec(self->pool, "SELECT id FROM job WHERE state IN ('queued', 'running')"
		" AND kind = '" AXIS_YOUTUBE_FETCH_KIND "'", 0, NULL, &result, NULL);
	for (i = 0; jobs_known && i < axis_db_result_get_n_rows(result); i++) {
		gint64 *id = g_new0(gint64, 1);

		axis_db_result_get_int64(result, i, 0, id);
		g_hash_table_add(live, id);
	}
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		g_autoptr(GError) error = NULL;
		AxisVolumeFolderOutcome outcome = AXIS_VOLUME_FOLDER_GONE;
		gboolean own = video_id != NULL && strncmp(name, video_id, 11) == 0;
		gint64 job_id = 0;

		staging_name_read(name, &job_id);
		/* Without the jobs (the database did not answer) only the caller's
		 * own video's folders are known to be nobody's. */
		if (!own && (!jobs_known || g_hash_table_contains(live, &job_id)))
			continue;
		if (!axis_volume_write_remove_folder(guard, stage_parent, name, 0, &outcome, NULL, &error)) {
			g_warning("youtube: a folder an earlier download left (%s/%s in a YouTube library folder) could not be "
				"removed: %s", AXIS_YOUTUBE_STAGING_DIR, name, error->message);
			continue;
		}
		if (outcome == AXIS_VOLUME_FOLDER_REMOVED)
			removed++;
	}
	return removed;
}

/* One pass of axis_youtube_recover(). */
static void
recover_once(AxisYoutube *self)
{
	g_autoptr(GPtrArray) roots = NULL;
	guint removed = 0;
	guint i;

	/* The records first; their downloads' records follow at once. */
	if (axis_youtube_fail_orphans(self) > 0)
		axis_youtube_poll_now(self);
	if (!axis_storage_list_roots(self->storage, 0, &roots, NULL))
		return;
	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		g_autoptr(AxisVolumeGuard) guard = NULL;
		struct stat st;
		gint root_fd;
		gint stage;

		if (root->media_type != AXIS_MEDIA_YOUTUBE)
			continue;
		/* Only a disk that can be written now (not read-only storage, not
		 * one that is offline): the next fetch into the folder looks again. */
		guard = axis_volume_guard_acquire(self->storage, root->volume_id, AXIS_VOLUME_INTENT_WRITE, NULL);
		if (guard == NULL)
			continue;
		root_fd = axis_volume_guard_open_dir(guard, root->rel_path, NULL);
		if (root_fd < 0)
			continue;
		/* Nothing is made here: a folder that is not there holds nothing. */
		stage = openat(root_fd, AXIS_YOUTUBE_STAGING_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(root_fd);
		if (stage < 0)
			continue;
		if (fstat(stage, &st) == 0 && (guint64)st.st_dev == axis_volume_guard_get_dev(guard))
			removed += axis_youtube_sweep_staging(self, guard, stage, NULL);
		close(stage);
	}
	if (removed > 0)
		g_message("youtube: removed %u folder(s) that downloads cut short had left in %s", removed,
			AXIS_YOUTUBE_STAGING_DIR);
}

void
axis_youtube_recover(AxisYoutube *self)
{
	g_autoptr(AxisDbResult) result = NULL;
	gint64 in_hand = 0;
	guint i;

	g_return_if_fail(self != NULL);

	recover_once(self);
	/* A video still in hand has a job that is queued or running: its own,
	 * or one of the run before that the job queue has not taken back yet
	 * (it does so in its first moments, on a thread of its own).  Once more
	 * a little later settles which; a fetch that really runs is not
	 * touched either time. */
	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM youtube_video WHERE state IN ('wanted', 'fetching')", 0,
	        NULL, &result, NULL) || !axis_db_result_get_int64(result, 0, 0, &in_hand) || in_hand == 0)
		return;
	for (i = 0; i < 20 && !g_cancellable_is_cancelled(self->stopping); i++)
		g_usleep(250000);
	if (!g_cancellable_is_cancelled(self->stopping))
		recover_once(self);
}

static gboolean
declared(
	AxisYoutube *self,
	const gchar *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

static gchar *
reason_printf(
	const gchar *format,
	...
) G_GNUC_PRINTF(1, 2);

static gchar *
reason_printf(
	const gchar *format,
	...
){
	va_list args;
	gchar *text;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	return text;
}

/* Step 1: the record becomes 'fetching', for this job only. */
static gboolean
claim(
	Fetch     *f,
	gboolean  *mine,
	GError   **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];

	*mine = FALSE;
	params[0] = f->video_id;
	params[1] = f->job_text;
	if (!axis_db_pool_exec(f->self->pool,
	        "UPDATE youtube_video SET state = 'fetching', started_at = now(), done_bytes = 0, rate_bps = NULL,"
	        " eta_s = NULL, message = NULL, updated_at = now()"
	        " WHERE video_id = $1::text AND job_id = $2::bigint AND state IN ('wanted', 'fetching')"
	        " RETURNING video_id", 2, params, &result, error))
		return FALSE;
	*mine = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

/* Steps 2 and 3: the guard, the checks, and yt-dlp's folder.  %NULL when
 * the fetch may run; else why not, for a person. */
static gchar *
prepare(
	Fetch   *f,
	guint64  max_bytes
){
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *name;
	struct stat st;
	gboolean created = FALSE;
	guint64 need;
	guint64 available = 0;
	guint64 floor = 0;
	gint fd;

	if (f->video->root_id <= 0 || !axis_storage_get_root(f->self->storage, f->video->root_id, &f->root, NULL) ||
	    f->root->media_type != AXIS_MEDIA_YOUTUBE)
		return g_strdup("The YouTube library folder this video was to go to is no longer there.");
	volume = axis_storage_get_volume(f->self->storage, f->root->volume_id);
	f->disk = g_strdup(volume != NULL ? volume->name : "the library's");
	name = f->disk;
	f->guard = axis_volume_guard_acquire(f->self->storage, f->root->volume_id, AXIS_VOLUME_INTENT_WRITE, &error);
	if (f->guard == NULL) {
		if (g_error_matches(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY))
			return axis_storage_is_read_only_mode(f->self->storage) ?
				reason_printf("Storage is read-only (the setting storage.read_only is on), so nothing was written to the "
					"disk %s.", name) :
				reason_printf("The disk %s is mounted read-only, so nothing was written to it.", name);
		return reason_printf("The disk %s is not online, so nothing was written to it.", name);
	}
	f->root_fd = axis_volume_guard_open_dir(f->guard, f->root->rel_path, &error);
	if (f->root_fd < 0)
		return reason_printf("The YouTube library folder on the disk %s cannot be opened.", name);

	axis_youtube_names(f->video, &f->channel_dir, &f->season_dir, &f->file_name);
	f->rel_path = g_strdup_printf("%s%s%s/%s/%s", f->root->rel_path, f->root->rel_path[0] != '\0' ? "/" : "",
		f->channel_dir, f->season_dir, f->file_name);
	if (strlen(f->rel_path) > 1024)
		return g_strdup("The video's place in the library folder would be longer than a path may be.");
	/* Never over a file that is there. */
	fd = openat(f->root_fd, f->channel_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0) {
		gint season = openat(fd, f->season_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

		close(fd);
		if (season >= 0) {
			gint rc = fstatat(season, f->file_name, &st, AT_SYMLINK_NOFOLLOW);

			close(season);
			if (rc == 0)
				return reason_printf("A file with this video's name is already in the library folder (%s). Nothing was "
					"replaced.", f->file_name);
		}
	}
	/* Room: the streams and the merged file (twice the size limit when
	 * yt-dlp gave no size), and the floor imports keep. */
	need = axis_youtube_need_bytes(f->video->expected_bytes, max_bytes);
	if (axis_youtube_free_space(f->self, f->root_fd, &available, &floor) && available < need + floor) {
		g_autofree gchar *have = axis_size_format(available);
		g_autofree gchar *want = axis_size_format(need + floor);

		return reason_printf("The disk %s has %s free, and this download needs %s (the video while it is put "
			"together, and the free space Axis keeps, import.free_space_floor). Nothing was written.", name, have,
			want);
	}
	f->dir_mode = 0755;
	if (declared(f->self, AXIS_IMPORT_SETTING_DIR_MODE)) {
		g_autofree gchar *mode = axis_settings_get_string(f->self->settings, AXIS_IMPORT_SETTING_DIR_MODE);
		guint64 value = mode != NULL ? g_ascii_strtoull(mode, NULL, 8) : 0;

		if (value >= 0700 && value <= 0777)
			f->dir_mode = (guint)value;
	}
	/* yt-dlp's folder: a new one, in a folder both media servers ignore. */
	f->stage_parent = axis_volume_write_mkdir(f->guard, f->root_fd, AXIS_YOUTUBE_STAGING_DIR, f->dir_mode, NULL, NULL,
		&error);
	if (f->stage_parent < 0)
		return reason_printf("The folder yt-dlp works in could not be made on the disk %s: %s", name, error->message);
	/* What runs that were killed left goes first: every folder of this
	 * video (this is its one fetch), and every folder whose job is neither
	 * queued nor running. */
	axis_youtube_sweep_staging(f->self, f->guard, f->stage_parent, f->video_id);
	if (fstatat(f->stage_parent, f->stage_name, &st, AT_SYMLINK_NOFOLLOW) == 0)
		return reason_printf("What an earlier run left in the folder yt-dlp works in could not be removed.");
	f->stage_fd = axis_volume_write_mkdir(f->guard, f->stage_parent, f->stage_name, 0700, &created, &f->stage_ino,
		&error);
	if (f->stage_fd < 0 || !created) {
		if (f->stage_fd >= 0) {
			close(f->stage_fd);
			f->stage_fd = -1;
		}
		return reason_printf("The folder yt-dlp works in could not be made on the disk %s.", name);
	}
	return NULL;
}

/* Step 6: yt-dlp's folder goes, with whatever is in it. */
static void
remove_staging(Fetch *f)
{
	g_autoptr(GError) error = NULL;
	AxisVolumeFolderOutcome outcome = AXIS_VOLUME_FOLDER_GONE;

	if (f->guard == NULL || f->stage_parent < 0 || f->stage_fd < 0)
		return;
	close(f->stage_fd);
	f->stage_fd = -1;
	if (!axis_volume_write_remove_folder(f->guard, f->stage_parent, f->stage_name, f->stage_ino, &outcome, NULL, &error) ||
	    (outcome != AXIS_VOLUME_FOLDER_REMOVED && outcome != AXIS_VOLUME_FOLDER_GONE))
		g_warning("youtube: the folder yt-dlp worked in (%s/%s in a YouTube library folder) could not be removed%s%s",
			AXIS_YOUTUBE_STAGING_DIR, f->stage_name, error != NULL ? ": " : "", error != NULL ? error->message : "");
}

/* Step 5: the one file into the library.  %NULL when it is there. */
static gchar *
place(Fetch *f)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *temp = g_strdup_printf(AXIS_VOLUME_TEMP_PREFIX "%" G_GUINT64_FORMAT,
		((guint64)g_random_int() << 20) ^ (guint64)g_get_real_time());
	gchar *problem = NULL;
	struct stat st;
	struct stat there;
	gboolean made_channel = FALSE;
	gboolean made_season = FALSE;
	guint64 channel_ino = 0;
	guint64 season_ino = 0;
	gint channel_fd = -1;
	gint season_fd = -1;
	/* Streams it merged are Matroska, as asked; a video YouTube serves as
	 * one file is not merged, and comes in the container it has. */
	static const gchar *const extensions[] = { "mkv", "mp4", "webm" };
	g_autofree gchar *staged = NULL;
	const gchar *extension = NULL;
	guint found = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(extensions); i++) {
		g_autofree gchar *candidate = g_strdup_printf(AXIS_YOUTUBE_STAGING_STEM ".%s", extensions[i]);
		struct stat seen;

		if (fstatat(f->stage_fd, candidate, &seen, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (found++ == 0) {
			st = seen;
			extension = extensions[i];
			staged = g_steal_pointer(&candidate);
		}
	}
	if (found == 0)
		return g_strdup("yt-dlp finished without a video file. The video may be larger than the size limit "
			"(youtube.max_size).");
	if (found > 1)
		return g_strdup("yt-dlp left more than one video file, so none was taken.");
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size <= 0 ||
	    (guint64)st.st_dev != axis_volume_guard_get_dev(f->guard))
		return g_strdup("What yt-dlp left is not a plain video file, so it was not taken.");
	/* Its name in the library has the extension it came with. */
	if (strcmp(extension, "mkv") != 0) {
		g_clear_pointer(&f->channel_dir, g_free);
		g_clear_pointer(&f->season_dir, g_free);
		g_clear_pointer(&f->file_name, g_free);
		g_clear_pointer(&f->rel_path, g_free);
		axis_youtube_names_as(f->video, extension, &f->channel_dir, &f->season_dir, &f->file_name);
		f->rel_path = g_strdup_printf("%s%s%s/%s/%s", f->root->rel_path, f->root->rel_path[0] != '\0' ? "/" : "",
			f->channel_dir, f->season_dir, f->file_name);
		if (strlen(f->rel_path) > 1024)
			return g_strdup("The video's place in the library folder would be longer than a path may be.");
	}

	channel_fd = axis_volume_write_mkdir(f->guard, f->root_fd, f->channel_dir, f->dir_mode, &made_channel, &channel_ino,
		&error);
	if (channel_fd >= 0)
		season_fd = axis_volume_write_mkdir(f->guard, channel_fd, f->season_dir, f->dir_mode, &made_season, &season_ino,
			&error);
	if (season_fd < 0) {
		problem = reason_printf("The video's folder in the library could not be made: %s", error->message);
	} else if (fstatat(season_fd, f->file_name, &there, AT_SYMLINK_NOFOLLOW) == 0) {
		problem = reason_printf("A file with this video's name is already in the library folder (%s). Nothing was "
			"replaced.", f->file_name);
	} else if (!axis_volume_write_move_temp(f->guard, f->stage_fd, staged, (guint64)st.st_ino, season_fd, temp,
	           &error)) {
		problem = reason_printf("The video could not be moved into the library folder: %s", error->message);
	} else if (!axis_volume_write_publish(f->guard, season_fd, temp, (guint64)st.st_ino, f->file_name, &error)) {
		/* Back where it came from; its folder is removed with it. */
		if (!axis_volume_write_move_back(f->guard, season_fd, temp, (guint64)st.st_ino, f->stage_fd, staged, NULL))
			g_warning("youtube: a file under a temporary name was left in a YouTube library folder (%s)", temp);
		problem = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS) ?
			reason_printf("A file with this video's name appeared in the library folder meanwhile (%s). Nothing was "
				"replaced.", f->file_name) :
			reason_printf("The video could not be given its name in the library folder: %s", error->message);
	} else {
		f->placed_bytes = (gint64)st.st_size;
	}
	/* Folders made for a file that did not arrive go again (when empty). */
	if (problem != NULL && season_fd >= 0 && made_season)
		axis_volume_write_rmdir_created(f->guard, channel_fd, f->season_dir, season_ino, NULL);
	if (problem != NULL && channel_fd >= 0 && made_channel)
		axis_volume_write_rmdir_created(f->guard, f->root_fd, f->channel_dir, channel_ino, NULL);
	if (season_fd >= 0)
		close(season_fd);
	if (channel_fd >= 0)
		close(channel_fd);
	return problem;
}

/* The downloads layer looks now, not at its next turn. */
static void
tell_downloads(
	Fetch    *f,
	gboolean  fetched
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *client_text = NULL;
	const gchar *params[2];
	gint64 client_id = 0;
	gint64 download_id = 0;
	AxisDbConn *conn;
	gboolean marked = FALSE;

	/* Whether or not it could look now (when not, its next turn does). */
	axis_youtube_poll_now(f->self);
	if (!fetched || !axis_youtube_client(f->self, &client_id, &error))
		return;
	/* The file is the library's now: the download is imported, so nothing
	 * else takes it for one waiting to be.  The downloads layer writes a
	 * built-in kind's finished download as imported itself
	 * (axis-download-track.c), so this finds it done; it stays for a record
	 * that an earlier version left 'completed'. */
	client_text = g_strdup_printf("%" G_GINT64_FORMAT, client_id);
	params[0] = client_text;
	params[1] = f->video_id;
	if (!axis_db_pool_exec(f->self->pool,
	        "SELECT id FROM download WHERE client_id = $1::bigint AND client_ref = $2::text AND state = 'completed'"
	        " ORDER BY id DESC LIMIT 1", 2, params, &result, &error) ||
	    axis_db_result_get_n_rows(result) == 0 || !axis_db_result_get_int64(result, 0, 0, &download_id))
		return;
	conn = axis_db_pool_acquire(f->self->pool, &error);
	if (conn == NULL)
		return;
	if (axis_downloads_mark_imported(conn, download_id, &marked, &error) && marked) {
		axis_db_pool_release(f->self->pool, conn);
		axis_downloads_notify_changed(f->self->downloads, download_id);
		return;
	}
	axis_db_pool_release(f->self->pool, conn);
}

static void
record_failure(
	Fetch       *f,
	const gchar *reason
){
	g_autofree gchar *clean = axis_download_text_clean(reason, 300);
	const gchar *params[3];

	params[0] = f->video_id;
	params[1] = f->job_text;
	params[2] = clean != NULL ? clean : "The download failed.";
	/* One a person stopped keeps saying so. */
	axis_db_pool_exec(f->self->pool,
		"UPDATE youtube_video SET message = CASE WHEN state = 'cancelled' THEN message ELSE $3::text END,"
		" state = CASE WHEN state = 'cancelled' THEN 'cancelled' ELSE 'failed' END,"
		" rate_bps = NULL, eta_s = NULL, finished_at = now(), updated_at = now()"
		" WHERE video_id = $1::text AND job_id = $2::bigint AND state IN ('wanted', 'fetching', 'cancelled')", 3, params,
		NULL, NULL);
}

gboolean
axis_youtube_run_fetch(
	AxisYoutube     *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *video_id = payload != NULL && json_object_has_member(payload, "video") &&
		JSON_NODE_HOLDS_VALUE(json_object_get_member(payload, "video")) &&
		json_node_get_value_type(json_object_get_member(payload, "video")) == G_TYPE_STRING ?
		json_object_get_string_member(payload, "video") : NULL;
	g_autofree gchar *reason = NULL;
	g_autofree gchar *workdir = NULL;
	g_auto(GStrv) argv = NULL;
	AxisProbeRunSpec spec;
	AxisProbeRunResult result;
	AxisYoutubeRun run;
	AxisJobStopReason stop;
	Progress progress;
	Fetch f;
	gboolean mine = FALSE;
	gboolean ran = FALSE;
	gboolean refused = FALSE;
	gboolean halted;
	guint timeout_ms;
	guint space_ms;
	gsize output_max;

	if (!axis_youtube_video_id_valid(video_id)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "The job names no YouTube video.");
		return FALSE;
	}
	memset(&f, 0, sizeof(f));
	f.self = self;
	f.context = context;
	f.video_id = video_id;
	f.job_text = g_strdup_printf("%" G_GINT64_FORMAT, axis_job_context_get_id(context));
	f.root_fd = f.stage_parent = f.stage_fd = -1;
	f.stage_name = g_strdup_printf("%s-%s", video_id, f.job_text);

	if (!claim(&f, &mine, error)) {
		fetch_clear(&f);
		return FALSE;
	}
	/* Its download was removed, or another job has the video: nothing to do. */
	if (!mine || !axis_youtube_get_video(self, video_id, &f.video, NULL)) {
		fetch_clear(&f);
		return TRUE;
	}

	axis_youtube_run_read(self, &run);
	reason = axis_youtube_blocked(self);
	if (reason == NULL && run.program == NULL)
		reason = g_strdup("yt-dlp is not installed where the setting \"yt-dlp program\" says (youtube.ytdlp_path).");
	if (reason == NULL && run.problem != NULL)
		reason = g_strdup(run.problem);
	if (reason == NULL)
		reason = prepare(&f, run.max_bytes);

	g_mutex_lock(&self->lock);
	timeout_ms = self->tuning.fetch_timeout_ms;
	output_max = self->tuning.fetch_output_max > 0 ? self->tuning.fetch_output_max : AXIS_YOUTUBE_FETCH_OUTPUT_MAX;
	space_ms = self->tuning.space_check_ms > 0 ? self->tuning.space_check_ms : 60000;
	g_mutex_unlock(&self->lock);
	if (timeout_ms == 0) {
		gint64 seconds = AXIS_YOUTUBE_FETCH_MIN_S + 3 * MAX(f.video->duration_s, 0);

		timeout_ms = (guint)CLAMP(seconds, AXIS_YOUTUBE_FETCH_MIN_S, AXIS_YOUTUBE_FETCH_MAX_S) * 1000;
	}

	if (reason == NULL) {
		GCancellable *job_cancel = axis_job_context_get_cancellable(context);
		GCancellable *run_cancel = g_cancellable_new();
		gulong job_handler;
		gulong stop_handler;

		/* What stops yt-dlp: the job being cancelled, the daemon stopping,
		 * and the disk running short of the free space Axis keeps. */
		job_handler = job_cancel != NULL ? g_cancellable_connect(job_cancel, G_CALLBACK(on_cancelled), run_cancel,
			NULL) : 0;
		stop_handler = g_cancellable_connect(self->stopping, G_CALLBACK(on_cancelled), run_cancel, NULL);
		memset(&progress, 0, sizeof(progress));
		progress.self = self;
		progress.context = context;
		progress.video_id = video_id;
		progress.job_text = f.job_text;
		progress.expected = f.video->expected_bytes;
		progress.cancel = run_cancel;
		progress.root_fd = f.root_fd;
		progress.disk = f.disk;
		progress.space_every_us = (gint64)space_ms * 1000;
		progress.last_space_us = g_get_monotonic_time();
		argv = axis_youtube_build_argv(run.program, video_id, FALSE, run.max_height, run.max_bytes, run.cache_dir,
			run.js_runtime);
		/* Its working directory is the folder made for it, by the descriptor
		 * the guard's checks were made on: no path leads it elsewhere. */
		workdir = g_strdup_printf("/proc/self/fd/%d", f.stage_fd);
		memset(&spec, 0, sizeof(spec));
		spec.argv = (const gchar *const *)argv;
		spec.fd = -1;
		spec.timeout_ms = timeout_ms;
		spec.output_cap = output_max;
		spec.discard_output = TRUE;
		spec.error_cap = AXIS_YOUTUBE_ERROR_MAX;
		spec.cancellable = run_cancel;
		spec.environment = (const gchar *const *)run.environment;
		spec.working_directory = workdir;
		spec.cpu_seconds = timeout_ms / 1000 + 10;
		/* The streams as they come and the merged file are each within the
		 * largest video taken, and a little for what yt-dlp keeps beside. */
		spec.file_size_limit = run.max_bytes + 64 * 1024 * 1024;
		spec.output_func = on_output;
		spec.output_data = &progress;
		g_atomic_int_inc(&self->runs);
		axis_probe_run(&spec, &result);
		ran = TRUE;
		g_cancellable_disconnect(job_cancel, job_handler);
		g_cancellable_disconnect(self->stopping, stop_handler);
		g_object_unref(run_cancel);

		if (progress.space_problem != NULL) {
			/* Stopped for the disk's sake, whatever yt-dlp did meanwhile. */
			reason = g_steal_pointer(&progress.space_problem);
		} else if (result.status == AXIS_PROBE_RUN_OK) {
			reason = place(&f);
		} else if (result.status == AXIS_PROBE_RUN_EXITED) {
			refused = axis_youtube_failure_read((const gchar *)result.errors->data, result.errors->len, &reason) ==
				AXIS_YOUTUBE_FAILURE_REFUSED;
		} else if (result.status == AXIS_PROBE_RUN_SIGNALLED && !axis_probe_run_crashed(&result)) {
			reason = g_strdup("The video is larger than the size limit (youtube.max_size), or yt-dlp used more "
				"processor time than a download may.");
		} else {
			reason = axis_youtube_run_problem(&result, timeout_ms);
		}
		axis_probe_run_result_clear(&result);
	}
	axis_youtube_run_clear(&run);
	remove_staging(&f);

	stop = axis_job_context_get_stop_reason(context);
	/* The daemon is stopping (or lost the job): the video fails, saying so,
	 * and nothing takes it up again by itself. */
	halted = g_cancellable_is_cancelled(self->stopping) || stop == AXIS_JOB_STOP_SHUTDOWN ||
		stop == AXIS_JOB_STOP_LEASE_LOST;

	if (reason != NULL) {
		if (stop == AXIS_JOB_STOP_CANCELLED) {
			g_autoptr(AxisYoutubeVideo) now = NULL;

			/* Why it was stopped is on its record already (a person, the
			 * switch, a pause). */
			g_free(reason);
			reason = axis_youtube_get_video(self, video_id, &now, NULL) && now->state == AXIS_YOUTUBE_CANCELLED &&
				now->message != NULL ? g_strdup(now->message) : g_strdup("Stopped by a person.");
		} else if (halted && ran) {
			/* (One that did not get as far as yt-dlp keeps its own reason.) */
			g_free(reason);
			reason = g_strdup(AXIS_YOUTUBE_STOPPED_REASON);
		}
		record_failure(&f, reason);
		if (refused)
			axis_youtube_note_refused(self);
		tell_downloads(&f, FALSE);
		/* Handed back by the queue as it stops, its job would run again and
		 * find nothing to do: cancelled, it ends here. */
		if (stop == AXIS_JOB_STOP_SHUTDOWN) {
			AxisDbConn *db = axis_db_pool_acquire(self->pool, NULL);

			if (db != NULL) {
				axis_job_cancel(db, axis_job_context_get_id(context), NULL, NULL);
				axis_db_pool_release(self->pool, db);
			}
		}
		fetch_clear(&f);
		g_set_error_literal(error, AXIS_JOB_ERROR, stop != AXIS_JOB_STOP_NONE ? AXIS_JOB_ERROR_CANCELLED :
			AXIS_JOB_ERROR_PERMANENT, reason);
		return FALSE;
	}

	/* It is in the library. */
	{
		g_autofree gchar *volume_text = g_strdup_printf("%" G_GINT64_FORMAT, f.root->volume_id);
		g_autofree gchar *size_text = g_strdup_printf("%" G_GINT64_FORMAT, f.placed_bytes);
		g_autoptr(GError) local = NULL;
		const gchar *params[5];

		params[0] = video_id;
		params[1] = f.job_text;
		params[2] = volume_text;
		params[3] = f.rel_path;
		params[4] = size_text;
		if (!axis_db_pool_exec(self->pool,
		        "UPDATE youtube_video SET state = 'fetched', volume_id = $3::bigint, rel_path = $4::text,"
		        " size_bytes = $5::bigint, done_bytes = $5::bigint, rate_bps = NULL, eta_s = NULL, message = NULL,"
		        " finished_at = now(), updated_at = now() WHERE video_id = $1::text AND job_id = $2::bigint", 5, params,
		        NULL, &local)) {
			/* The file is in the library and stays; the record could not say
			 * so.  The job fails, and the next look at the folder finds it. */
			g_warning("youtube: a video is in the library but its record could not be written: %s", local->message);
			fetch_clear(&f);
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "The video is in the library folder, "
				"but Axis could not record that. Scan the folder to see it.");
			return FALSE;
		}
	}
	axis_youtube_note_worked(self);
	tell_downloads(&f, TRUE);
	if (self->library != NULL) {
		g_autoptr(GError) local = NULL;

		if (!axis_library_request_scan(self->library, f.root->id, NULL, &local))
			g_debug("youtube: the library folder's scan could not be asked for: %s", local->message);
	}
	axis_job_context_set_progress(context, 1.0, "In the library");
	fetch_clear(&f);
	return TRUE;
}
