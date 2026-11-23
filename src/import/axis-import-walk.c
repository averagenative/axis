/*
 * axis-import-walk.c - walking a download through its volume's guard,
 * opening again what was found, and probing the videos
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Read only.  The download's place is opened through the guard (one
 * component at a time, no link, no other filesystem); below it every name is
 * looked at with fstatat(AT_SYMLINK_NOFOLLOW) relative to its folder's
 * descriptor and every folder opened with openat(O_NOFOLLOW | O_DIRECTORY),
 * checked to be on the guard's device.  A link is listed as one and never
 * followed; a folder on another filesystem, a folder too deep, and the
 * folders Axis leaves out (samples, proofs, ...) are listed and not looked
 * into.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "import/axis-import-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"

/* ── Found entries ──────────────────────────────────────────────── */

void
axis_import_found_free(AxisImportFound *found)
{
	if (found == NULL)
		return;
	g_free(found->path);
	g_free(found->dir);
	g_free(found->name);
	g_free(found->probe_text);
	g_free(found->video_codec);
	g_free(found->hdr);
	g_free(found->audio_codec);
	g_free(found->audio_profile);
	g_clear_pointer(&found->reasons, g_ptr_array_unref);
	g_free(found->dest_subdir);
	g_free(found->dest_name);
	g_free(found);
}

void
axis_import_found_reason(
	AxisImportFound *found,
	const gchar     *code,
	gboolean         overridable,
	const gchar     *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	if (found->reasons == NULL)
		found->reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_reason_free);
	g_ptr_array_add(found->reasons, axis_import_reason_new(code, text, overridable));
}

static gint
compare_found(
	gconstpointer a,
	gconstpointer b
){
	const AxisImportFound *x = *(const AxisImportFound *const *)a;
	const AxisImportFound *y = *(const AxisImportFound *const *)b;

	return strcmp(x->path, y->path);
}

/* ── The walk ───────────────────────────────────────────────────── */

typedef struct
{
	DIR              *dir;
	gchar            *raw;	/* relative to the source, raw bytes; "" its top */
	gchar            *stored;	/* the same in the stored form */
	guint             depth;
	AxisImportFolder  folder;
	const gchar      *extra;
} Level;

static void
level_free(gpointer data)
{
	Level *level = data;

	if (level->dir != NULL)
		closedir(level->dir);
	g_free(level->raw);
	g_free(level->stored);
	g_free(level);
}

static gchar *
join_raw(
	const gchar *dir,
	const gchar *name
){
	return dir[0] != '\0' ? g_strconcat(dir, "/", name, NULL) : g_strdup(name);
}

static gchar *
join_stored(
	const gchar *dir,
	const gchar *name
){
	GString *out = g_string_new(dir);

	if (out->len > 0)
		g_string_append_c(out, '/');
	axis_library_name_encode(out, name, strlen(name));
	return g_string_free(out, FALSE);
}

static AxisImportFound *
found_new(
	const Level *level,
	const gchar *name
){
	AxisImportFound *found = g_new0(AxisImportFound, 1);

	found->dir = g_strdup(level->raw);
	found->name = g_strdup(name);
	found->path = join_stored(level->stored, name);
	found->folder = level->folder;
	found->extra_folder = level->extra;
	return found;
}

/* The parent of @path and its last component (a normalised storage path). */
static void
split_path(
	const gchar  *path,
	gchar       **parent,
	gchar       **last
){
	const gchar *slash = strrchr(path, '/');

	*parent = slash != NULL ? g_strndup(path, (gsize)(slash - path)) : g_strdup("");
	*last = g_strdup(slash != NULL ? slash + 1 : path);
}

static gboolean
walk_folder(
	AxisVolumeGuard  *guard,
	gint              top,
	GPtrArray        *found,
	GError          **error
){
	g_autoptr(GPtrArray) stack = g_ptr_array_new_with_free_func(level_free);
	guint64 dev = axis_volume_guard_get_dev(guard);
	guint entries = 0;
	Level *first = g_new0(Level, 1);

	first->dir = fdopendir(top);
	if (first->dir == NULL) {
		gint err = errno;

		close(top);
		g_free(first);
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Cannot read the download's folder: %s",
			g_strerror(err));
		return FALSE;
	}
	first->raw = g_strdup("");
	first->stored = g_strdup("");
	first->folder = AXIS_IMPORT_FOLDER_PLAIN;
	g_ptr_array_add(stack, first);

	while (stack->len > 0 && entries < AXIS_IMPORT_WALK_ENTRIES) {
		Level *level = g_ptr_array_index(stack, stack->len - 1);
		struct dirent *entry;
		struct stat st;
		AxisImportFound *item;
		gint dfd;

		errno = 0;
		entry = readdir(level->dir);
		if (entry == NULL) {
			if (errno != 0) {
				gint err = errno;

				g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Cannot read a folder of the download: %s",
					g_strerror(err));
				return FALSE;
			}
			g_ptr_array_remove_index(stack, stack->len - 1);
			continue;
		}
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		entries++;
		dfd = dirfd(level->dir);
		if (fstatat(dfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		/* Hidden names: nothing to import, nothing to list. */
		if (entry->d_name[0] == '.')
			continue;
		if (found->len >= AXIS_IMPORT_MAX_FILES)
			break;
		if (S_ISLNK(st.st_mode)) {
			item = found_new(level, entry->d_name);
			item->type = AXIS_IMPORT_FOUND_LINK;
			item->kind = axis_import_file_kind(entry->d_name, strlen(entry->d_name));
			g_ptr_array_add(found, item);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			const gchar *extra = NULL;
			AxisImportFolder kind = axis_import_folder_kind(entry->d_name, strlen(entry->d_name), &extra);
			Level *next;
			gint fd;

			/* A plain folder inside an extras folder is still extras. */
			if (kind == AXIS_IMPORT_FOLDER_PLAIN && level->folder != AXIS_IMPORT_FOLDER_PLAIN) {
				kind = level->folder;
				extra = level->extra;
			}
			if (kind == AXIS_IMPORT_FOLDER_IGNORED || level->depth + 1 >= AXIS_IMPORT_WALK_DEPTH ||
			    (guint64)st.st_dev != dev) {
				item = found_new(level, entry->d_name);
				item->type = AXIS_IMPORT_FOUND_FOLDER;
				item->folder_note = (guint64)st.st_dev != dev ? "on another filesystem: not looked into" :
					kind == AXIS_IMPORT_FOLDER_IGNORED ? "a folder Axis leaves out (samples, proofs, screens, "
					"generic extras): not looked into" : "too deep: not looked into";
				g_ptr_array_add(found, item);
				continue;
			}
			fd = openat(dfd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
			if (fd < 0)
				continue;
			if (fstat(fd, &st) != 0 || (guint64)st.st_dev != dev || !S_ISDIR(st.st_mode)) {
				close(fd);
				continue;
			}
			next = g_new0(Level, 1);
			next->dir = fdopendir(fd);
			if (next->dir == NULL) {
				close(fd);
				g_free(next);
				continue;
			}
			next->raw = join_raw(level->raw, entry->d_name);
			next->stored = join_stored(level->stored, entry->d_name);
			next->depth = level->depth + 1;
			next->folder = kind;
			next->extra = extra;
			g_ptr_array_add(stack, next);
			continue;
		}
		item = found_new(level, entry->d_name);
		if (!S_ISREG(st.st_mode)) {
			item->type = AXIS_IMPORT_FOUND_SPECIAL;
			item->kind = AXIS_IMPORT_FILE_OTHER;
		} else {
			item->type = AXIS_IMPORT_FOUND_FILE;
			item->kind = axis_import_file_kind(entry->d_name, strlen(entry->d_name));
			item->size = (guint64)st.st_size;
			item->inode = (guint64)st.st_ino;
		}
		if (item->kind == AXIS_IMPORT_FILE_IGNORED) {
			axis_import_found_free(item);
			continue;
		}
		g_ptr_array_add(found, item);
	}
	return TRUE;
}

gboolean
axis_import_walk(
	AxisVolumeGuard  *guard,
	const gchar      *source_path,
	GPtrArray       **found_out,
	gboolean         *is_file,
	gchar           **problem,
	GError          **error
){
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	GError *local = NULL;
	struct stat st;
	gint pfd;
	guint i;

	*found_out = NULL;
	*is_file = FALSE;
	*problem = NULL;
	if (source_path == NULL || source_path[0] == '\0') {
		*problem = g_strdup("The download's place is the top of its volume, which Axis does not import from.");
		return TRUE;
	}
	split_path(source_path, &parent, &last);
	pfd = axis_volume_guard_open_dir(guard, parent, &local);
	if (pfd < 0) {
		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED) ||
		    g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID)) {
			*problem = g_strdup_printf("The download's place cannot be reached: %s", local->message);
			g_error_free(local);
			return TRUE;
		}
		if (local->domain == G_IO_ERROR) {
			*problem = g_strdup("Nothing is at the download's place any more.");
			g_error_free(local);
			return TRUE;
		}
		g_propagate_error(error, local);
		return FALSE;
	}
	if (fstatat(pfd, last, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		*problem = errno == ENOENT ? g_strdup("Nothing is at the download's place any more.") :
			g_strdup_printf("The download's place cannot be read: %s.", g_strerror(errno));
		close(pfd);
		return TRUE;
	}
	if (S_ISLNK(st.st_mode)) {
		*problem = g_strdup("The download's place is a symbolic link, which Axis does not follow.");
		close(pfd);
		return TRUE;
	}
	if ((guint64)st.st_dev != axis_volume_guard_get_dev(guard)) {
		*problem = g_strdup("The download's place is on another filesystem than its volume.");
		close(pfd);
		return TRUE;
	}
	if (S_ISREG(st.st_mode)) {
		AxisImportFound *item = g_new0(AxisImportFound, 1);

		item->type = AXIS_IMPORT_FOUND_FILE;
		item->kind = axis_import_file_kind(last, strlen(last));
		item->path = g_strdup("");
		item->dir = g_strdup("");
		item->name = g_strdup(last);
		item->size = (guint64)st.st_size;
		item->inode = (guint64)st.st_ino;
		item->folder = AXIS_IMPORT_FOLDER_PLAIN;
		g_ptr_array_add(found, item);
		close(pfd);
		*is_file = TRUE;
	} else if (S_ISDIR(st.st_mode)) {
		gint fd = openat(pfd, last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		close(pfd);
		if (fd < 0) {
			*problem = g_strdup_printf("The download's folder cannot be opened: %s.", g_strerror(errno));
			return TRUE;
		}
		if (fstat(fd, &st) != 0 || (guint64)st.st_dev != axis_volume_guard_get_dev(guard)) {
			close(fd);
			*problem = g_strdup("The download's folder is on another filesystem than its volume.");
			return TRUE;
		}
		if (!walk_folder(guard, fd, found, error))
			return FALSE;
		g_ptr_array_sort(found, compare_found);
	} else {
		close(pfd);
		*problem = g_strdup("The download's place is not a file or a folder.");
		return TRUE;
	}
	for (i = 0; i < found->len; i++)
		((AxisImportFound *)g_ptr_array_index(found, i))->position = (gint)i;
	/* What the walk saw is only so if the volume is still the one checked. */
	if (!axis_volume_guard_check(guard, error))
		return FALSE;
	*found_out = g_steal_pointer(&found);
	return TRUE;
}

/* ── Opening what was found ─────────────────────────────────────── */

static gboolean
changed(GError **error)
{
	g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
		"The download's file is no longer the one that was found (it was changed, moved or replaced).");
	return FALSE;
}

gboolean
axis_import_open_found(
	AxisVolumeGuard        *guard,
	const gchar            *source_path,
	const AxisImportFound  *found,
	gint                   *dir_out,
	gint                   *fd_out,
	GError                **error
){
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	g_auto(GStrv) parts = NULL;
	guint64 dev = axis_volume_guard_get_dev(guard);
	struct stat st;
	gint current;
	gint fd;
	guint i;

	*dir_out = -1;
	if (fd_out != NULL)
		*fd_out = -1;
	split_path(source_path, &parent, &last);
	current = axis_volume_guard_open_dir(guard, parent, error);
	if (current < 0)
		return FALSE;
	if (found->path[0] != '\0') {
		gint next = openat(current, last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		close(current);
		if (next < 0)
			return changed(error);
		current = next;
		parts = found->dir[0] != '\0' ? g_strsplit(found->dir, "/", -1) : g_new0(gchar *, 1);
		for (i = 0; parts[i] != NULL; i++) {
			next = openat(current, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
			close(current);
			if (next < 0)
				return changed(error);
			current = next;
		}
		if (fstat(current, &st) != 0 || (guint64)st.st_dev != dev) {
			close(current);
			return changed(error);
		}
	}
	if (fd_out == NULL) {
		if (fstatat(current, found->name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
		    (guint64)st.st_ino != found->inode || (guint64)st.st_dev != dev) {
			close(current);
			return changed(error);
		}
		*dir_out = current;
		return TRUE;
	}
	fd = openat(current, found->name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK | O_NOATIME | O_CLOEXEC);
	if (fd < 0 && errno == EPERM)
		fd = openat(current, found->name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		close(current);
		return changed(error);
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (guint64)st.st_ino != found->inode ||
	    (guint64)st.st_size != found->size || (guint64)st.st_dev != dev) {
		close(fd);
		close(current);
		return changed(error);
	}
	{
		gint flags = fcntl(fd, F_GETFL);

		if (flags >= 0)
			(void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}
	*dir_out = current;
	*fd_out = fd;
	return TRUE;
}

/* ── Probing ────────────────────────────────────────────────────── */

AxisReleaseResolution
axis_import_resolution_of(
	gint width,
	gint height
){
	if (width <= 0 && height <= 0)
		return AXIS_RELEASE_RESOLUTION_NONE;
	/* By the larger of what the width and the height say: a scope film
	 * at 1920x800 is 1080p. */
	if (width >= 3200 || height >= 1800)
		return AXIS_RELEASE_RESOLUTION_2160P;
	if (width >= 1700 || height >= 1000)
		return AXIS_RELEASE_RESOLUTION_1080P;
	if (width >= 1100 || height >= 700)
		return AXIS_RELEASE_RESOLUTION_720P;
	if (height > 480)
		return AXIS_RELEASE_RESOLUTION_576P;
	return AXIS_RELEASE_RESOLUTION_480P;
}

static void
take_info(
	AxisImportFound     *found,
	const AxisMediaInfo *info
){
	guint i;

	found->probed = TRUE;
	found->probe_ok = g_strcmp0(info->status, "ok") == 0;
	if (!found->probe_ok) {
		found->probe_text = g_strdup(info->error != NULL ? info->error : "ffprobe could not read the file.");
		return;
	}
	found->duration_ms = info->duration_ms;
	for (i = 0; info->streams != NULL && i < info->streams->len; i++) {
		const AxisMediaStream *stream = g_ptr_array_index(info->streams, i);

		if (stream->kind == AXIS_STREAM_VIDEO && !stream->attached_pic) {
			found->video_streams++;
			if (found->video_streams == 1) {
				found->width = stream->width;
				found->height = stream->height;
				found->video_codec = g_strdup(stream->codec);
				if (stream->hdr_format != AXIS_HDR_NONE && stream->hdr_format != AXIS_HDR_SDR)
					found->hdr = g_strdup(stream->hdr_format == AXIS_HDR_DOLBY_VISION ? "DV" :
						stream->hdr_format == AXIS_HDR_HDR10PLUS ? "HDR10Plus" :
						stream->hdr_format == AXIS_HDR_HLG ? "HLG" : "HDR10");
			}
		} else if (stream->kind == AXIS_STREAM_AUDIO && found->audio_codec == NULL) {
			found->audio_codec = g_strdup(stream->codec);
			found->audio_profile = g_strdup(stream->profile);
			found->audio_channels = stream->channels;
		}
	}
	if (found->video_streams == 0) {
		found->probe_text = g_strdup("ffprobe found no video stream in it.");
		return;
	}
	{
		gint64 minutes = (found->duration_ms + 30000) / 60000;

		found->probe_text = g_strdup_printf("%" G_GINT64_FORMAT " min, %dx%d %s", minutes, found->width,
			found->height, found->video_codec != NULL ? found->video_codec : "video");
	}
}

gboolean
axis_import_probe_found(
	AxisProbe        *probe,
	AxisVolumeGuard  *guard,
	const gchar      *source_path,
	GPtrArray        *found,
	GError          **error
){
	guint probed = 0;
	guint i;

	for (i = 0; i < found->len && probed < AXIS_IMPORT_MAX_PROBES; i++) {
		AxisImportFound *item = g_ptr_array_index(found, i);
		g_autoptr(AxisMediaInfo) info = NULL;
		GError *local = NULL;
		gint dir = -1;
		gint fd = -1;
		gboolean ok;

		if (item->type != AXIS_IMPORT_FOUND_FILE || item->kind != AXIS_IMPORT_FILE_VIDEO)
			continue;
		probed++;
		if (!axis_import_open_found(guard, source_path, item, &dir, &fd, &local)) {
			if (!g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED)) {
				g_propagate_error(error, local);
				return FALSE;
			}
			item->probed = TRUE;
			item->probe_ok = FALSE;
			item->probe_text = g_strdup(local->message);
			g_error_free(local);
			continue;
		}
		ok = axis_probe_inspect_fd(probe, fd, (gint64)item->size, &info, &local);
		close(fd);
		close(dir);
		if (!ok) {
			g_propagate_error(error, local);
			return FALSE;
		}
		take_info(item, info);
	}
	return TRUE;
}
