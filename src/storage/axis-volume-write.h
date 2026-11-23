/*
 * axis-volume-write.h - the only changes Axis makes to files on a volume
 * besides its marker, all through a write guard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is the whole write surface of the daemon on a volume (the import
 * pipeline's, and the conversions' at the end of this file), in one place
 * so that it can be read in one place.  Every operation:
 *
 *  - takes a guard acquired with %AXIS_VOLUME_INTENT_WRITE, proves the
 *    volume again when it starts (axis_volume_guard_check(): the volume's
 *    path still leads to the directory the guard holds, the marker is the
 *    volume's, it is mounted read-write; a failure marks the guard failed)
 *    and asks axis_volume_guard_writable() before each change: nothing is
 *    written through a read guard, a guard whose check failed, a guard
 *    whose disk was swapped or unmounted after it was taken, or while
 *    "storage.read_only" is on (the default), and a filesystem that became
 *    read-only refuses by itself (EROFS);
 *  - works on directory descriptors that the guard handed out or that were
 *    opened beneath them here (each one checked to be on the guard's
 *    filesystem and mount), by name, with O_NOFOLLOW / AT_SYMLINK_NOFOLLOW:
 *    no path string is given to the system, no symbolic link is followed and
 *    no other filesystem is reached;
 *  - names only what it may: a temporary name is AXIS_VOLUME_TEMP_PREFIX and
 *    digits, created with O_EXCL (never an existing file); a final name is
 *    given by a rename that does not replace (RENAME_NOREPLACE, or a link
 *    and the removal of the temporary name where the filesystem has no
 *    such rename), except the one swap below, which exchanges two names
 *    atomically after both were checked;
 *  - checks, before and after, that each name leads to the file (inode) the
 *    caller expects, and undoes what it did when it does not.
 *
 * What can be removed: a temporary name (when it still names the file
 * expected and another name of that file exists, unless the caller says the
 * file is a copy whose source is still there); an empty directory this run
 * created; a file in the volume's recycle folder whose inode and size are
 * the ones recorded; a file of the work area by its name of Axis's and its
 * inode; and the leftover folder of a finished Usenet download after its
 * import, with what is in it (axis_volume_write_remove_folder(): the caller
 * decides that it is one, by the rules of import/axis-import-folder.c), or
 * its one file when the client put it straight into the category's folder
 * (axis_volume_write_remove_file()); and, after a move across volumes, the
 * download's file whose verified copy is in the library
 * (axis_volume_write_remove_moved()).  A download's file is moved into the
 * library by a rename that never replaces a name
 * (axis_volume_write_move_temp(), and back with
 * axis_volume_write_move_back()).  A
 * file of the library is never removed: it can only be renamed (or, for an
 * upgrade or a conversion that keeps its name, hardlinked) into the recycle
 * folder of its own volume.
 *
 * Everything here blocks on the filesystem: call it from a job's thread.
 * Errors: %AXIS_STORAGE_ERROR_READ_ONLY and _UNAVAILABLE from the guard,
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED for a link, another filesystem or
 * something that is not what was expected, %AXIS_STORAGE_ERROR_INVALID for a
 * name that is not allowed, %G_IO_ERROR_EXISTS for a final name that is
 * taken, %G_IO_ERROR_NOT_SUPPORTED for a swap the filesystem cannot make,
 * and other #GIOError codes from the system.
 */

#ifndef AXIS_VOLUME_WRITE_H
#define AXIS_VOLUME_WRITE_H

#include <gio/gio.h>
#include <glib.h>
#include <sys/stat.h>

#include "storage/axis-volume-guard.h"

G_BEGIN_DECLS

#define AXIS_VOLUME_TEMP_PREFIX  ".axis-partial-"
#define AXIS_VOLUME_RECYCLE_DIR  ".axis-recycle"
/* The conversions' work area, at the top of a volume (made with
 * axis_volume_write_mkdir()). */
#define AXIS_VOLUME_WORK_DIR     ".axis-work"

/* Whether @name is AXIS_VOLUME_TEMP_PREFIX followed by 1 to 20 digits. */
gboolean
axis_volume_write_temp_name_valid(const gchar *name);

/* Whether @name is a name of Axis's in a work area: an attempt's output
 * ("<1-19 digits>-<1-3 digits>.tmp") or a kept trial
 * ("trial-<1-19 digits>.mkv" or ".mp4"), numbers without a leading zero. */
gboolean
axis_volume_write_work_name_valid(const gchar *name);

/**
 * axis_volume_write_mkdir:
 * @guard: a write guard
 * @parent: a directory on the guard's filesystem (not consumed)
 * @name: one component (bytes; not "." or "..", no '/')
 * @mode: the mode a directory made here gets (set exactly, umask aside)
 * @created: (out) (optional): whether it was made now (else it existed)
 * @ino: (out) (optional): its inode
 *
 * Makes the directory @name in @parent unless it exists, and opens it.  An
 * existing entry that is not a directory, or a link, is refused.  Only a
 * directory made now gets @mode; an existing one is not touched.
 *
 * Returns: the directory (a descriptor the caller closes), or -1
 */
gint
axis_volume_write_mkdir(
	AxisVolumeGuard  *guard,
	gint              parent,
	const gchar      *name,
	guint             mode,
	gboolean         *created,
	guint64          *ino,
	GError          **error
);

/* axis_volume_write_mkdir(), saying in *@synced (optional) whether @parent
 * was flushed to the disk after a folder was made in it (%TRUE when none was
 * made): a caller that would delete something once a file is on the disk
 * below the new folder (a move across volumes) keeps it when not. */
gint
axis_volume_write_mkdir_synced(
	AxisVolumeGuard  *guard,
	gint              parent,
	const gchar      *name,
	guint             mode,
	gboolean         *created,
	guint64          *ino,
	gboolean         *synced,
	GError          **error
);

/**
 * axis_volume_write_rmdir_created:
 * @ino: the inode axis_volume_write_mkdir() gave when it made it
 *
 * Removes a directory this run made, when it is still that directory and
 * empty (the system refuses to remove one that is not).  A directory that
 * is not empty, or not that one, is left, and that is not an error.
 *
 * Returns: %FALSE only when the guard refuses
 */
gboolean
axis_volume_write_rmdir_created(
	AxisVolumeGuard  *guard,
	gint              parent,
	const gchar      *name,
	guint64           ino,
	GError          **error
);

/**
 * axis_volume_write_link_temp:
 * @src_dir: the source's directory, on the guard's filesystem
 * @src_name: the source's name in it
 * @src_ino: the inode the caller checked the source has
 * @dst_dir: the destination directory, on the guard's filesystem
 * @temp_name: a temporary name (axis_volume_write_temp_name_valid())
 *
 * Gives the source a second name, @temp_name in @dst_dir (linkat, which
 * never follows a link and never replaces a name).  The source's data,
 * name, mode and owner are not touched.  When the new name turns out not to
 * lead to @src_ino (the source's name was replaced meanwhile) it is removed
 * again and this fails with %AXIS_STORAGE_ERROR_NOT_CONTAINED.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_link_temp(
	AxisVolumeGuard  *guard,
	gint              src_dir,
	const gchar      *src_name,
	guint64           src_ino,
	gint              dst_dir,
	const gchar      *temp_name,
	GError          **error
);

/**
 * axis_volume_write_move_temp:
 * @src_dir: the download file's directory, on the guard's filesystem
 * @src_name: its name in it (not a temporary name)
 * @src_ino: the inode the caller checked it has
 * @dst_dir: the destination directory, on the guard's filesystem
 * @temp_name: a temporary name (axis_volume_write_temp_name_valid())
 *
 * Moves a download's file into the library under @temp_name: one rename
 * that never replaces a name (RENAME_NOREPLACE; where the filesystem has
 * none, a link, checked, and then the removal of the download's name), so
 * that the file has one name or the other (for that moment, both) at every
 * moment.  Its data, mode, owner and times are not touched.  A name that is
 * taken fails with %G_IO_ERROR_EXISTS, another filesystem with
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED; when @temp_name turns out not to lead
 * to @src_ino, the name is put back and this fails with
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED.  Both directories are fsynced after.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_move_temp(
	AxisVolumeGuard  *guard,
	gint              src_dir,
	const gchar      *src_name,
	guint64           src_ino,
	gint              dst_dir,
	const gchar      *temp_name,
	GError          **error
);

/**
 * axis_volume_write_move_back:
 * @dir: the directory the temporary name is in
 * @temp_name: the temporary name a move gave the file
 * @ino: the inode it must lead to
 * @src_dir: the download's directory the file came from
 * @src_name: its name there
 *
 * The undo of axis_volume_write_move_temp(), the same way: the file gets
 * its download name back, never replacing one (%G_IO_ERROR_EXISTS when that
 * name is taken now: the temporary name is left as it is).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_move_back(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           ino,
	gint              src_dir,
	const gchar      *src_name,
	GError          **error
);

/**
 * AxisVolumeCopyStep:
 * @data: (closure)
 * @written: bytes written so far
 * @error: set it to stop the copy
 *
 * Called after the temporary file is made (with @written 0 and @ino its
 * inode, before any data: the caller records it), then about every second
 * while data is written (with @written short of the whole size), and once
 * with the whole size, when the copy is written, the source found unchanged
 * and the copy flushed, before it is read back.
 *
 * Returns: %FALSE (with @error) to stop: the temporary file is removed
 */
typedef gboolean (*AxisVolumeCopyStep)(
	gpointer   data,
	guint64    ino,
	guint64    written,
	GError   **error
);

/**
 * AxisVolumeSourceId:
 * @ino: the file's inode
 * @size: its size
 * @mtime_ns: its modification time, in nanoseconds since the epoch
 * @ctime_ns: its change time, the same way
 *
 * What a file copied was: taken on the descriptor it was read through
 * before its first byte was read, and found the same (its device too) once
 * its last byte was.  It is what the removal of a download's file after a
 * move across volumes requires (axis_volume_write_remove_moved()), never a
 * look at the file taken later: a change made while the copy is flushed and
 * read back would be taken for the state copied.
 */
typedef struct
{
	guint64 ino;
	guint64 size;
	gint64  mtime_ns;
	gint64  ctime_ns;
} AxisVolumeSourceId;

/**
 * axis_volume_write_copy_temp:
 * @source: the file to copy, open for reading (any volume; not consumed)
 * @src_dir: the directory @source was opened in, or -1
 * @src_name: (nullable): its name there, which is looked at when @source has
 *   no name left after it was read
 * @size: its size, as the caller checked it
 * @dst_dir: the destination directory, on the guard's filesystem
 * @temp_name: a temporary name
 * @step: (nullable): called as AxisVolumeCopyStep says
 * @step_data: (closure step)
 * @ino: (out): the temporary file's inode
 * @copied: (out) (optional): what @source was when it was read, on success
 * @checksum: (out) (transfer full): its SHA-256, in hexadecimal
 *
 * Writes a copy of @source to @temp_name (created with O_EXCL), computing
 * its SHA-256 as it goes; fsyncs it, drops its pages from the cache and
 * reads it back from the start: the size and the SHA-256 of what was read
 * back must be those of what was read from @source.  @source is looked at
 * on its descriptor before its first byte is read and again after its last:
 * the same device, inode, size, modification time and change time, to the
 * nanosecond, nothing beyond @size, or the copy is refused.  A file deleted
 * meanwhile, its data unwritten, is the one change of its change time that
 * stands, and only when nothing is at @src_name in @src_dir afterwards
 * (nothing can remove it then): a file renamed over its name (another file
 * there now, the one read unlinked) is a change, and without @src_name it
 * cannot be told from one.  The guard is
 * checked again about every five seconds.  On any failure the temporary
 * file is removed and nothing else exists.
 *
 * Returns: %TRUE when the copy is complete and verified
 */
gboolean
axis_volume_write_copy_temp(
	AxisVolumeGuard     *guard,
	gint                 source,
	gint                 src_dir,
	const gchar         *src_name,
	guint64              size,
	gint                 dst_dir,
	const gchar         *temp_name,
	AxisVolumeCopyStep   step,
	gpointer             step_data,
	guint64             *ino,
	AxisVolumeSourceId  *copied,
	gchar              **checksum,
	GError             **error
);

/**
 * axis_volume_write_publish:
 * @dir: the directory both names are in
 * @temp_name: the temporary name
 * @temp_ino: the inode it must lead to
 * @final_name: the name the file gets (one component, not a temporary one)
 *
 * Gives the file its final name without ever replacing one: fails with
 * %G_IO_ERROR_EXISTS when @final_name is taken (the temporary name is left
 * for the caller to remove).  The directory is fsynced after.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_publish(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	GError          **error
);

/**
 * axis_volume_write_publish_synced:
 * @synced: (out) (optional): whether the directory was flushed to the disk
 *   after the file got its name
 *
 * axis_volume_write_publish(), saying whether the directory's fsync after it
 * succeeded.  It succeeds whether or not it did (the file has its name); a
 * caller that would delete something because the name is on the disk (the
 * download's file of a move across volumes) keeps it when *@synced is
 * %FALSE.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_publish_synced(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	gboolean         *synced,
	GError          **error
);

/* Tests: while @fail, the directory fsync after every publish and swap fails
 * (as with EIO). */
void
axis_volume_write_test_fail_publish_sync(gboolean fail);

/* Tests: while @fail, the fsync of the folder that holds a folder just made
 * fails (as with EIO), and so does axis_volume_write_flush_dir(). */
void
axis_volume_write_test_fail_folder_sync(gboolean fail);

/* Flushes the directory @dir (on the guard's filesystem) to the disk, as a
 * publish does after it: for a run taken up again that finds a file given its
 * name by an earlier one.  Changes nothing.  %FALSE when it could not be
 * flushed. */
gboolean
axis_volume_write_flush_dir(
	AxisVolumeGuard *guard,
	gint             dir
);

/**
 * axis_volume_write_swap:
 * @temp_ino: the new file, at @temp_name
 * @final_name: the name to give it, which the old file has
 * @old_ino: the old file, which must be at @final_name and must have another
 *   name besides (its name in the recycle folder: the caller made it first)
 *
 * For an upgrade whose new file has the old one's name: exchanges the two
 * names atomically (RENAME_EXCHANGE), so that @final_name always names a
 * whole file.  Afterwards @temp_name names the old file; the caller removes
 * that name with axis_volume_write_unlink_temp().  When what came back is
 * not @old_ino, the names are exchanged back and it fails with
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED; a filesystem that cannot exchange
 * fails with %G_IO_ERROR_NOT_SUPPORTED, changing nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_swap(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	guint64           old_ino,
	GError          **error
);

/* axis_volume_write_swap(), saying in *@synced (optional) whether the
 * directory's fsync after it succeeded, as axis_volume_write_publish_synced()
 * does. */
gboolean
axis_volume_write_swap_synced(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	guint64           old_ino,
	gboolean         *synced,
	GError          **error
);

/**
 * axis_volume_write_unlink_temp:
 * @expected_ino: the inode it must lead to, 0 when not known (a name made
 *   with O_EXCL whose inode the caller could not record)
 * @last_link_ok: the file may have no other name: a copy whose source is
 *   still there, so nothing is lost
 * @removed: (out) (optional): whether a name was removed (%FALSE: it was
 *   not there)
 *
 * Removes a temporary name.  Refuses (changing nothing, with
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED) a name that is not a temporary one,
 * that is not a regular file, that leads to another inode than expected, or
 * whose file has no other name unless @last_link_ok.
 *
 * Returns: %TRUE when the name is gone (or was not there)
 */
gboolean
axis_volume_write_unlink_temp(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           expected_ino,
	gboolean          last_link_ok,
	gboolean         *removed,
	GError          **error
);

/**
 * axis_volume_write_unlink_temp_beside:
 * @expected_ino: the inode it must lead to (not 0)
 * @other_dir: the directory of the file's other name, on the guard's
 *   filesystem
 * @other_name: that name (one component, not a temporary name): the file's
 *   final name in the library, or its name in the download
 * @removed: (out) (optional): whether a name was removed
 *
 * Removes the temporary name of a file that was moved out of a download
 * only when @other_name, looked at just before, leads to the very file: its
 * link count is not taken for another name (a hardlink outside, of a
 * cross-seed or a backup, would count too, and the temporary name would be
 * its one name in the library and the download).  Refuses (changing
 * nothing, with %AXIS_STORAGE_ERROR_NOT_CONTAINED) otherwise.
 *
 * Returns: %TRUE when the name is gone (or was not there)
 */
gboolean
axis_volume_write_unlink_temp_beside(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           expected_ino,
	gint              other_dir,
	const gchar      *other_name,
	gboolean         *removed,
	GError          **error
);

typedef enum
{
	AXIS_VOLUME_RETIRE_MOVE,
	AXIS_VOLUME_RETIRE_LINK
} AxisVolumeRetireHow;

/**
 * axis_volume_write_retire:
 * @src_dir: the file's directory, on the guard's filesystem
 * @src_name: its name
 * @src_ino: the inode it must lead to
 * @where: the place in the recycle folder: components (bytes, each one as
 *   axis_volume_write_mkdir() takes), ended by %NULL; made as needed under
 *   AXIS_VOLUME_RECYCLE_DIR at the top of the guard's volume
 * @dir_mode: the mode directories made here get
 * @how: rename it there, or give it a second name there (an upgrade that
 *   keeps the name)
 * @recycle_name: (out) (transfer full): the name it has there: @src_name,
 *   or @src_name and ".1", ".2" ... when that is taken
 *
 * Retires a file of the library into the recycle folder of its own volume:
 * a rename (or a link) that never replaces a name and never crosses a
 * filesystem.  Both directories are fsynced after.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_retire(
	AxisVolumeGuard      *guard,
	gint                  src_dir,
	const gchar          *src_name,
	guint64               src_ino,
	const gchar *const   *where,
	guint                 dir_mode,
	AxisVolumeRetireHow   how,
	gchar               **recycle_name,
	GError              **error
);

/**
 * AxisVolumeRecycledOutcome:
 * @AXIS_VOLUME_RECYCLED_REMOVED: removed
 * @AXIS_VOLUME_RECYCLED_GONE: nothing was at the place
 * @AXIS_VOLUME_RECYCLED_CHANGED: something else is there (another inode or
 *   size, not a regular file, a link on the way): left alone
 */
typedef enum
{
	AXIS_VOLUME_RECYCLED_REMOVED,
	AXIS_VOLUME_RECYCLED_GONE,
	AXIS_VOLUME_RECYCLED_CHANGED
} AxisVolumeRecycledOutcome;

/**
 * axis_volume_write_remove_recycled:
 * @parts: the file's path from the top of the volume, as components (bytes),
 *   ended by %NULL; the first must be AXIS_VOLUME_RECYCLE_DIR
 * @ino: the inode recorded for it
 * @size: the size recorded for it
 * @outcome: (out): what happened
 *
 * The recycle cleanup's one removal: the file at @parts, when it is the
 * regular file recorded (inode and size), reached without a link or another
 * filesystem.  Then the directories above it, up to the recycle folder
 * itself (not included), are removed while they are empty.
 *
 * Returns: %FALSE only for a refusal of the guard or an error of the system
 */
gboolean
axis_volume_write_remove_recycled(
	AxisVolumeGuard            *guard,
	const gchar *const         *parts,
	guint64                     ino,
	guint64                     size,
	AxisVolumeRecycledOutcome  *outcome,
	GError                    **error
);

/* A download's folder is removed only within these bounds: folders inside
 * it at most this deep, and at most this many names in all. */
#define AXIS_VOLUME_FOLDER_DEPTH    16
#define AXIS_VOLUME_FOLDER_ENTRIES  10000

/**
 * AxisVolumeFolderOutcome:
 * @AXIS_VOLUME_FOLDER_REMOVED: the folder and everything in it are gone
 * @AXIS_VOLUME_FOLDER_GONE: nothing was at its name
 * @AXIS_VOLUME_FOLDER_NOT_FOLDER: something that is not a folder (a
 *   symbolic link, a file) is at its name: left as it is
 * @AXIS_VOLUME_FOLDER_CHANGED: the folder at its name is not the one the
 *   caller looked at (another inode): left as it is
 * @AXIS_VOLUME_FOLDER_OTHER_FS: the folder, or a folder inside it, is
 *   another filesystem or mount: nothing was removed
 * @AXIS_VOLUME_FOLDER_TOO_BIG: it is deeper than
 *   %AXIS_VOLUME_FOLDER_DEPTH or holds more than
 *   %AXIS_VOLUME_FOLDER_ENTRIES names: nothing was removed
 */
typedef enum
{
	AXIS_VOLUME_FOLDER_REMOVED,
	AXIS_VOLUME_FOLDER_GONE,
	AXIS_VOLUME_FOLDER_NOT_FOLDER,
	AXIS_VOLUME_FOLDER_CHANGED,
	AXIS_VOLUME_FOLDER_OTHER_FS,
	AXIS_VOLUME_FOLDER_TOO_BIG
} AxisVolumeFolderOutcome;

/**
 * axis_volume_write_remove_folder:
 * @parent: the directory the folder is in, on the guard's filesystem
 * @name: the folder's name in it (one component)
 * @ino: the inode the folder must have (the caller looked at it); 0 for
 *   any
 * @outcome: (out): what happened
 * @removed: (out) (optional): how many names inside the folder were removed
 *
 * The one removal of a whole folder: a finished Usenet download's own job
 * folder, after its import, with whatever its client left in it (the caller
 * has decided that it is that, by its rules).  First the whole tree is
 * looked at, changing nothing: every folder in it must be on the guard's
 * filesystem and mount, and the tree within the bounds above; otherwise
 * nothing is removed.  Then every name in it is removed, deepest first: a
 * symbolic link as a name (never followed), a folder once it is empty, each
 * folder opened again without following a link and checked to be on the
 * guard's filesystem before anything in it is removed.  Only these names go:
 * a file that has another name elsewhere (the library's hardlink of it)
 * keeps that name and its data.  The folder itself goes last, when its name
 * still leads to it.  The guard is asked before every change and the volume
 * proven again now and then.
 *
 * Returns: %FALSE only for a refusal of the guard or an error of the system
 *   (what was removed before it stays removed, and the rest is left for the
 *   next time)
 */
gboolean
axis_volume_write_remove_folder(
	AxisVolumeGuard           *guard,
	gint                       parent,
	const gchar               *name,
	guint64                    ino,
	AxisVolumeFolderOutcome   *outcome,
	guint                     *removed,
	GError                   **error
);

/**
 * axis_volume_write_remove_file:
 * @dir: the directory the file is in, on the guard's filesystem
 * @name: its name (one component)
 * @ino: the inode it must have
 * @outcome: (out): removed; gone (nothing at the name); changed (not a
 *   regular file, or another inode: left as it is)
 *
 * The other removal of a download's leftovers: the one file of a finished
 * Usenet job that its client put straight into the folder of Axis's
 * category, with no job folder of its own (the caller has decided that it
 * is that).  Only that name goes: a file with another name (the library's)
 * keeps it and its data.
 *
 * Returns: %FALSE only for a refusal of the guard or an error of the system
 */
gboolean
axis_volume_write_remove_file(
	AxisVolumeGuard            *guard,
	gint                        dir,
	const gchar                *name,
	guint64                     ino,
	AxisVolumeRecycledOutcome  *outcome,
	GError                    **error
);

/**
 * axis_volume_write_remove_moved:
 * @dir: the download's directory, on the guard's filesystem
 * @name: the download's file in it (one component, not a temporary name)
 * @ino: the inode it had when it was copied (not 0)
 * @size: the size it had then
 * @mtime_ns: its modification time then, in nanoseconds since the epoch
 *   (not 0)
 * @ctime_ns: its change time then, the same way (not 0)
 *
 * The four are the AxisVolumeSourceId axis_volume_write_copy_temp() gave:
 * the file as it was read, not as it was found afterwards.
 * @outcome: (out): removed; gone (nothing at the name); changed (not a
 *   regular file, or another inode, size or time: left as it is)
 *
 * The end of a move across volumes: the download's file, of which a verified
 * copy is in the library (the caller has made sure of that), when it is still
 * the regular file copied, untouched since.  It is opened (without following
 * a link, and only once it is seen to be a regular file) and checked on the
 * descriptor: the inode and size can be another file's (one deleted by
 * something else and made again at its name), its times not.  Its name is
 * looked at again just before it goes, and only that name goes.
 *
 * Returns: %FALSE only for a refusal of the guard or an error of the system
 */
gboolean
axis_volume_write_remove_moved(
	AxisVolumeGuard            *guard,
	gint                        dir,
	const gchar                *name,
	guint64                     ino,
	guint64                     size,
	gint64                      mtime_ns,
	gint64                      ctime_ns,
	AxisVolumeRecycledOutcome  *outcome,
	GError                    **error
);

/*
 * The work area (conversions, add-conversion-execution).  Each of these
 * takes @work, a descriptor of the volume's work area: it must be the
 * directory AXIS_VOLUME_WORK_DIR at the top of the guard's volume (checked
 * by inode, each time), and every name in it must be one of
 * axis_volume_write_work_name_valid()'s.  A worker writes its output there;
 * the daemon settles it, renames a trial there, moves a verified output out
 * of it into the library under a temporary name, and removes its own files
 * from it.
 */

/**
 * axis_volume_write_settle:
 * @work: the work area
 * @name: the output's name in it
 * @ino: the inode it must lead to (a regular file with one link)
 * @mode: the permission bits to give it (masked with 0777)
 * @uid: the owner to give it
 * @gid: the group to give it
 * @mtime_sec, @mtime_nsec: the modification time to give it
 * @owner_must_match: a change of owner or group the system refuses is an
 *   error (%G_IO_ERROR_PERMISSION_DENIED); otherwise it is left as it is
 * @st: (out caller-allocates): what it is afterwards
 *
 * Gives a verified output its original's owner, group, permission bits and
 * modification time, and flushes it to the disk.  Its data is not touched.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_settle(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	guint64           ino,
	guint             mode,
	guint             uid,
	guint             gid,
	gint64            mtime_sec,
	gint              mtime_nsec,
	gboolean          owner_must_match,
	struct stat      *st,
	GError          **error
);

/**
 * axis_volume_write_adopt:
 * @work: the work area
 * @name: the output's name in it
 * @ino: the inode it must lead to (a regular file with one link)
 * @dir: a directory of the library, on the guard's filesystem
 * @temp_name: a temporary name (axis_volume_write_temp_name_valid())
 *
 * Moves a verified output out of the work area into @dir under
 * @temp_name: a rename that never replaces a name (or a link and the
 * removal of the work-area name where the filesystem has no
 * RENAME_NOREPLACE).  When @temp_name does not lead to @ino afterwards the
 * name is put back and this fails with %AXIS_STORAGE_ERROR_NOT_CONTAINED.
 * Both directories are fsynced after.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_adopt(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	guint64           ino,
	gint              dir,
	const gchar      *temp_name,
	GError          **error
);

/**
 * axis_volume_write_rename_work:
 * @from: a name in the work area, leading to @ino
 * @to: the name it gets there, never replacing one (%G_IO_ERROR_EXISTS)
 *
 * A trial's output gets its kept name.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_write_rename_work(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *from,
	guint64           ino,
	const gchar      *to,
	GError          **error
);

/**
 * axis_volume_write_unlink_work:
 * @name: a name of Axis's in the work area
 * @ino: the inode it must lead to; 0 when not known (a leftover is
 *   listed with its inode, so the tidy job always knows it)
 * @removed: (out) (optional): whether a name was removed (%FALSE: it was
 *   not there)
 *
 * Removes a file of the work area: an output that failed, was rejected or
 * was left behind, or a trial whose time has come.  Refuses (changing
 * nothing, with %AXIS_STORAGE_ERROR_NOT_CONTAINED) something that is not a
 * regular file or not the inode expected.
 *
 * Returns: %TRUE when the name is gone (or was not there)
 */
gboolean
axis_volume_write_unlink_work(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	guint64           ino,
	gboolean         *removed,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_VOLUME_WRITE_H */
