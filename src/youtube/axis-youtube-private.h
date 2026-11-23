/*
 * axis-youtube-private.h - shared by the files of the YouTube layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-youtube-address.c  which addresses are taken (pure)
 *   axis-youtube-output.c   reading what yt-dlp writes (pure)
 *   axis-youtube.c          lifetime, settings, the switch and what holds
 *                           work back, the command, a look, the records, the
 *                           built-in download kind
 *   axis-youtube-fetch.c    the fetch job: the folder yt-dlp works in, the
 *                           run, the move into the library
 */

#ifndef AXIS_YOUTUBE_PRIVATE_H
#define AXIS_YOUTUBE_PRIVATE_H

#include "probe/axis-probe-run.h"
#include "storage/axis-volume-guard.h"
#include "youtube/axis-youtube.h"
#include "youtube/axis-youtube-address.h"
#include "youtube/axis-youtube-output.h"

G_BEGIN_DECLS

struct _AxisYoutube
{
	AxisDbPool           *pool;
	AxisSettings         *settings;	/* owned reference, nullable */
	AxisStorage          *storage;	/* owned reference */
	AxisDownloads        *downloads;	/* owned reference */
	AxisLibrary          *library;	/* owned reference, nullable */
	gchar                *state_dir;	/* nullable: yt-dlp's home, settings and cache folders are in it */
	GCancellable         *stopping;	/* cancelled by axis_youtube_stop(): ends a look and a fetch */
	AxisDownloadKindInfo  kind;	/* registered with @downloads; its data is this layer */
	GMutex                lock;
	AxisYoutubeTuning     tuning;	/* (lock) */
	gchar                *version_program;	/* (lock) the program @version_text was asked of */
	gchar                *version_text;	/* (lock) */
	GMutex                look_lock;	/* held by the one look at a time */
	gint                  runs;	/* atomic */
};

/* What a run of yt-dlp needs from the settings, read once for it. */
typedef struct
{
	gchar   *program;	/* resolved; NULL when it is not found */
	guint    max_height;
	guint64  max_bytes;
	gchar   *js_runtime;	/* NULL for yt-dlp's own choice */
	gchar   *cache_dir;	/* NULL for none (made, when it is said) */
	gchar  **environment;	/* its home, settings and cache folders, and the proxy */
	gchar   *problem;	/* why yt-dlp is not run with this (its home is not as Axis keeps it), or NULL */
} AxisYoutubeRun;

void
axis_youtube_run_read(
	AxisYoutube    *self,
	AxisYoutubeRun *run
);

void
axis_youtube_run_clear(AxisYoutubeRun *run);

/* Why no YouTube work runs now (transfer full), or %NULL when it may. */
gchar *
axis_youtube_blocked(AxisYoutube *self);

/* YouTube refused this server: work pauses (an hour, then twice as long each
 * time, up to a day). */
void
axis_youtube_note_refused(AxisYoutube *self);

/* A fetch worked: the next pause starts at an hour again. */
void
axis_youtube_note_worked(AxisYoutube *self);

/* One sentence for a run that did not end well (not for an exit with a
 * status: axis_youtube_failure_read() reads that one's words). */
gchar *
axis_youtube_run_problem(
	const AxisProbeRunResult *result,
	guint                     timeout_ms
);

/* "<Channel> - <Title> [<video id>]": what the downloads layer calls it. */
gchar *
axis_youtube_release_name(const AxisYoutubeVideo *video);

/* What a video that Axis stopped downloading says (it was killed, or it is
 * shutting down): nothing takes it up again by itself. */
#define AXIS_YOUTUBE_STOPPED_REASON "Axis stopped while it was downloading; download it again."

/* Videos recorded as queued or downloading whose job is neither queued nor
 * running (the daemon was killed during a fetch, or lost the job's lease)
 * become failed, with %AXIS_YOUTUBE_STOPPED_REASON.  Returns how many. */
guint
axis_youtube_fail_orphans(AxisYoutube *self);

/* Stops every fetch queued or running: its record says @message and its job
 * is cancelled, which kills yt-dlp.  Returns how many. */
guint
axis_youtube_stop_fetches(
	AxisYoutube *self,
	const gchar *message
);

/* How much room a download of a video said to take @expected_bytes (-1: not
 * said) needs while it is fetched: the streams and the merged file, each at
 * most @max_bytes; twice the size limit when yt-dlp gave no size. */
guint64
axis_youtube_need_bytes(
	gint64  expected_bytes,
	guint64 max_bytes
);

/* The free space of the filesystem @fd is on, and the free space imports
 * keep ("import.free_space_floor").  %FALSE when the system does not say. */
gboolean
axis_youtube_free_space(
	AxisYoutube *self,
	gint         fd,
	guint64     *available,
	guint64     *floor
);

/* The client of the built-in kind (axis_downloads_builtin_client()). */
gboolean
axis_youtube_client(
	AxisYoutube  *self,
	gint64       *client_id,
	GError      **error
);

/* The downloads layer looks at the built-in client now, not at its next
 * turn.  %FALSE when it could not (its next turn does). */
gboolean
axis_youtube_poll_now(AxisYoutube *self);

/* Removes from @stage_parent (".axis-youtube" of a YouTube library folder,
 * a directory on @guard's volume) the folders of fetches that are not in
 * hand: every "<video id>-<job>" whose job is neither queued nor running,
 * and, with @video_id, every folder of that video (the caller is its one
 * fetch, and has made none yet).  A name of another form is left alone, and
 * no link is followed.  Returns how many folders went. */
guint
axis_youtube_sweep_staging(
	AxisYoutube     *self,
	AxisVolumeGuard *guard,
	gint             stage_parent,
	const gchar     *video_id
);

G_END_DECLS

#endif /* AXIS_YOUTUBE_PRIVATE_H */
