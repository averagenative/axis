/*
 * axis-youtube.h - the YouTube downloader: one video, fetched by yt-dlp
 * through a built-in download client, saved into a YouTube library folder
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The YouTube downloader (openspec/specs/youtube/spec.md).  What this
 * layer promises:
 *
 *  - Off until an administrator switches it on: before that no yt-dlp is
 *    run for a video and nothing is asked of YouTube.  Who switched it on,
 *    and when, is kept.
 *  - yt-dlp is a separate program, run through probe/axis-probe-run.h and
 *    nothing else: an argument vector (never a shell), a fixed set of options
 *    (axis_youtube_build_argv()), a minimal environment, the lowest
 *    priorities, a time limit, a limit on what it may print and on the size
 *    of a file it may write, its whole process group killed when it is
 *    stopped.  At most one fetch runs at a time (the "youtube" job queue) and
 *    one look beside it.  It reads no configuration file and no cookies; its
 *    home and its settings folder are folders of Axis's own that hold
 *    nothing (it loads plugins from under both), and it is not run while
 *    either holds something.  A proxy reaches it through its environment,
 *    never its command, which the process list shows.  Its version is asked
 *    only for the administrator's page and the client's test: a page
 *    everyone signed in can open starts no program.
 *  - A look and a fetch end at once when the daemon stops
 *    (axis_youtube_stop()), and a fetch when YouTube is switched off or
 *    paused.  A video whose fetch was cut short (by that, or by a kill) says
 *    so and can be asked for again; none stays "downloading", and what
 *    yt-dlp had written is removed (axis_youtube_recover()).
 *  - What an administrator types is read by youtube/axis-youtube-address.h
 *    and never reaches yt-dlp: yt-dlp gets the address Axis writes from the
 *    video's id, as one argument after "--".
 *  - yt-dlp writes into a folder of its own inside the YouTube library root
 *    (".axis-youtube/<video id>-<job>", made through the volume guard, which
 *    both media servers and Axis's scans ignore).  The finished file is moved
 *    from there into the library by axis-volume-write.h: under a temporary
 *    name first, then to its final name by a rename that never replaces one.
 *    Nothing is written while "storage.read_only" is on or the disk is
 *    read-only or offline, or when the disk would be left with less than
 *    "import.free_space_floor": asked before a download is queued, before
 *    yt-dlp starts, and about once a minute while it runs.  Whatever ends a
 *    fetch, its folder is removed.
 *  - A video is fetched once: one record per video id, and a video that is
 *    being fetched or is in the library is not fetched again.
 *  - Nothing is re-encoded or cut: separate video and audio streams are
 *    merged into a Matroska file by yt-dlp's ffmpeg, as a stream copy; a
 *    video YouTube serves as one file is kept as it comes (mp4 or webm).
 *  - While "outbound.bind" is set, yt-dlp (which makes its own connections,
 *    outside the binding) is not run unless "youtube.proxy" is set or an
 *    administrator has confirmed, with "youtube.network_bound", that Axis's
 *    whole network goes through the bound interface.
 *  - When YouTube refuses this server (too many requests, a bot check),
 *    YouTube work pauses, for an hour and then twice as long each time, up to
 *    a day.  Nothing is tried again by itself.
 *
 * It is not a sandbox: yt-dlp can read what the daemon's user can and
 * connect anywhere (axis-probe-run.h says the same of ffmpeg).
 *
 * The layout in the library (scope section 5.1, "show by year"):
 *
 *   <Channel> [<channel id>]/Season <year>/<Channel> - <date> - <Title> [<video id>].mkv
 *
 * named with the import's character rule and byte budget
 * (import/axis-import-name.h).
 *
 * Threads: everything here blocks (the database, a subprocess, a disk): call
 * it from a worker thread.  Create and free on the main thread.
 */

#ifndef AXIS_YOUTUBE_H
#define AXIS_YOUTUBE_H

#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "download/axis-download.h"
#include "jobs/axis-job-context.h"
#include "library/axis-library.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_YOUTUBE_MODULE_ID          "youtube"
#define AXIS_YOUTUBE_KIND_NAME          "ytdlp"
#define AXIS_YOUTUBE_KIND_LABEL         "YouTube (yt-dlp)"
#define AXIS_YOUTUBE_QUEUE              "youtube"
#define AXIS_YOUTUBE_FETCH_KIND         "youtube.fetch"

#define AXIS_YOUTUBE_SETTING_PROGRAM    "youtube.ytdlp_path"
#define AXIS_YOUTUBE_SETTING_HEIGHT     "youtube.max_height"
#define AXIS_YOUTUBE_SETTING_SIZE       "youtube.max_size"
#define AXIS_YOUTUBE_SETTING_PROXY      "youtube.proxy"
#define AXIS_YOUTUBE_SETTING_BOUND      "youtube.network_bound"
#define AXIS_YOUTUBE_SETTING_RUNTIME    "youtube.js_runtime"

/* Where the image's build wrote which yt-dlp it took (deploy/Containerfile). */
#define AXIS_YOUTUBE_VERSION_FILE       "/usr/share/axis/yt-dlp.version"

/* The folder yt-dlp works in, inside a YouTube library root. */
#define AXIS_YOUTUBE_STAGING_DIR        ".axis-youtube"
/* What yt-dlp is told to call its output there; the extension is its own:
 * "mkv" for streams it merged, "mp4" or "webm" for a video YouTube serves as
 * one file. */
#define AXIS_YOUTUBE_STAGING_STEM       "video"

/* A look at one video: how long yt-dlp may take, and how much it may print
 * (the whole record of a video with every format YouTube offers). */
#define AXIS_YOUTUBE_LOOK_TIMEOUT_MS    120000
#define AXIS_YOUTUBE_LOOK_OUTPUT_MAX    (16 * 1024 * 1024)
/* A fetch: at least this long, and three times the video's length besides. */
#define AXIS_YOUTUBE_FETCH_MIN_S        1800
#define AXIS_YOUTUBE_FETCH_MAX_S        (12 * 3600)
/* The most a fetch may print (progress lines, once a second). */
#define AXIS_YOUTUBE_FETCH_OUTPUT_MAX   (8 * 1024 * 1024)
#define AXIS_YOUTUBE_ERROR_MAX          65536

#define AXIS_YOUTUBE_ERROR (axis_youtube_error_quark())

/**
 * AxisYoutubeError:
 * @AXIS_YOUTUBE_ERROR_OFF: YouTube is switched off, paused, or held back by
 *   the outbound binding; the message says which and what resolves it
 * @AXIS_YOUTUBE_ERROR_INVALID: the address is not one video's
 * @AXIS_YOUTUBE_ERROR_REFUSED: the video is not one Axis fetches (a live
 *   stream, one that needs an account)
 * @AXIS_YOUTUBE_ERROR_BUSY: another address is being looked at now
 * @AXIS_YOUTUBE_ERROR_EXISTS: the video is being fetched or is in the library
 * @AXIS_YOUTUBE_ERROR_NOT_FOUND: no such video was looked at
 * @AXIS_YOUTUBE_ERROR_STORAGE: there is no YouTube library folder, or it
 *   cannot be written now
 * @AXIS_YOUTUBE_ERROR_FAILED: yt-dlp failed; the message is for a person
 */
typedef enum
{
	AXIS_YOUTUBE_ERROR_OFF,
	AXIS_YOUTUBE_ERROR_INVALID,
	AXIS_YOUTUBE_ERROR_REFUSED,
	AXIS_YOUTUBE_ERROR_BUSY,
	AXIS_YOUTUBE_ERROR_EXISTS,
	AXIS_YOUTUBE_ERROR_NOT_FOUND,
	AXIS_YOUTUBE_ERROR_STORAGE,
	AXIS_YOUTUBE_ERROR_FAILED
} AxisYoutubeError;

GQuark
axis_youtube_error_quark(void);

typedef struct _AxisYoutube AxisYoutube;

/**
 * axis_youtube_new:
 * @pool: the database, which must outlive the layer
 * @settings: (nullable): the settings store (%NULL: the defaults)
 * @storage: the volumes
 * @downloads: the downloads layer; the built-in kind is added to it here,
 *   so before it is used
 * @library: (nullable): asked to scan the root after a video arrived
 * @state_dir: (nullable): a directory of Axis's own (in its data directory)
 *   for what yt-dlp is given as its surroundings, made when first needed,
 *   for the daemon's user alone: "home" (its HOME) and "config" (its
 *   XDG_CONFIG_HOME), which Axis keeps empty and looks at before every run
 *   (yt-dlp loads plugins from under both, whatever its options say: one
 *   that is not empty is not used, and yt-dlp is not run), and "cache" (its
 *   XDG_CACHE_HOME and --cache-dir).  %NULL: all three name a place that
 *   does not exist, and yt-dlp keeps no cache
 *
 * Returns: (transfer full) (nullable): the layer, or %NULL when its kind
 *   could not be added
 */
AxisYoutube *
axis_youtube_new(
	AxisDbPool     *pool,
	AxisSettings   *settings,
	AxisStorage    *storage,
	AxisDownloads  *downloads,
	AxisLibrary    *library,
	const gchar    *state_dir,
	GError        **error
);

/* The daemon is stopping: a look in flight and a fetch that is running are
 * cancelled (yt-dlp is killed), and none starts after.  The video of a fetch
 * ended this way fails with a plain reason; nothing takes it up again by
 * itself.  Any thread; more than once is fine. */
void
axis_youtube_stop(AxisYoutube *self);

/**
 * axis_youtube_recover:
 *
 * What a daemon that was killed during a fetch left behind, put right; for
 * the start of the daemon, once the job queue has taken back the jobs of the
 * run before.  A video recorded as queued or downloading whose job is
 * neither queued nor running fails, with a plain reason (the downloads
 * layer's tracking does the same each time it looks).  And in every YouTube
 * library folder whose disk can be written now, the folders in
 * ".axis-youtube" that belong to no queued or running fetch are removed,
 * through the volume write guard, following no link.  Blocks (the database,
 * the disks).
 */
void
axis_youtube_recover(AxisYoutube *self);

void
axis_youtube_free(AxisYoutube *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisYoutube, axis_youtube_free)

/* The module's settings, ended by an entry whose key is %NULL. */
const AxisSettingInfo *
axis_youtube_get_setting_table(void);

/**
 * AxisYoutubeTuning:
 * @look_timeout_ms: a look's time limit (0: the default)
 * @fetch_timeout_ms: a fetch's time limit (0: from the video's length)
 * @fetch_output_max: the most a fetch may print (0: the default)
 * @space_check_ms: how often a running fetch looks at the disk's free space
 *   (0: once a minute)
 *
 * For tests.
 */
typedef struct
{
	guint look_timeout_ms;
	guint fetch_timeout_ms;
	gsize fetch_output_max;
	guint space_check_ms;
} AxisYoutubeTuning;

void
axis_youtube_set_tuning(
	AxisYoutube             *self,
	const AxisYoutubeTuning *tuning
);

/* How many times yt-dlp was started by this layer (tests). */
guint
axis_youtube_get_runs(AxisYoutube *self);

/* ── The switch, and what holds YouTube work back ───────────────── */

/**
 * AxisYoutubeStatus:
 * @enabled: switched on
 * @enabled_by: (nullable): who switched it on
 * @enabled_us: when (microseconds since 1970), 0 for never
 * @paused_until_us: YouTube work is paused until then; 0 when it is not
 * @blocked: (nullable): why no YouTube work runs now, a whole sentence that
 *   says what resolves it (switched off, paused, the outbound binding,
 *   yt-dlp not found); %NULL when it may run
 * @program: (nullable): the yt-dlp that would run, as a path
 * @version: (nullable): its version: its own answer when YouTube is
 *   switched on (asked now only when the caller says so, else what an
 *   earlier answer left), otherwise what the image's build recorded
 * @version_from_build: @version is the build's record, not the program's
 *   own answer
 * @bound: "outbound.bind" is set
 */
typedef struct
{
	gboolean  enabled;
	gchar    *enabled_by;
	gint64    enabled_us;
	gint64    paused_until_us;
	gchar    *blocked;
	gchar    *program;
	gchar    *version;
	gboolean  version_from_build;
	gboolean  bound;
} AxisYoutubeStatus;

void
axis_youtube_status_clear(AxisYoutubeStatus *status);

/* @ask_version: yt-dlp may be run now for its version (the administrator's
 * page, the client's test).  Without it no program is started: a page anyone
 * signed in can open starts none. */
gboolean
axis_youtube_get_status(
	AxisYoutube        *self,
	gboolean            ask_version,
	AxisYoutubeStatus  *status,
	GError            **error
);

/* Switches YouTube on (recording @by and the time) or off.  Switching it
 * off stops the download that is running and those that wait (each says so,
 * and can be asked for again), and nothing new starts; *@stopped (optional)
 * is how many were stopped. */
gboolean
axis_youtube_set_enabled(
	AxisYoutube  *self,
	gboolean      enabled,
	const gchar  *by,
	guint        *stopped,
	GError      **error
);

/* ── Videos ─────────────────────────────────────────────────────── */

typedef enum
{
	AXIS_YOUTUBE_LISTED,
	AXIS_YOUTUBE_WANTED,
	AXIS_YOUTUBE_FETCHING,
	AXIS_YOUTUBE_FETCHED,
	AXIS_YOUTUBE_FAILED,
	AXIS_YOUTUBE_CANCELLED
} AxisYoutubeVideoState;

/* "Looked at", "Queued", "Downloading", "In the library", "Failed",
 * "Stopped" */
const gchar *
axis_youtube_video_state_label(AxisYoutubeVideoState state);

/**
 * AxisYoutubeVideo:
 *
 * One video as recorded (youtube_video).  Texts are cleaned; @upload_date is
 * "2021-03-15" or %NULL; figures are -1 when not known; @volume_name and
 * @rel_path say where its file is once fetched.
 */
typedef struct
{
	gchar                 *video_id;
	AxisYoutubeVideoState  state;
	gchar                 *title;
	gchar                 *channel;
	gchar                 *channel_id;
	gchar                 *upload_date;
	gint64                 duration_s;
	gchar                 *format_note;
	gint64                 expected_bytes;
	gint64                 root_id;
	gint64                 volume_id;
	gchar                 *volume_name;
	gchar                 *rel_path;
	gint64                 size_bytes;
	gint64                 done_bytes;
	gint64                 rate_bps;
	gint64                 eta_s;
	gchar                 *message;
	gint64                 job_id;
	gint64                 updated_us;
} AxisYoutubeVideo;

void
axis_youtube_video_free(AxisYoutubeVideo *video);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisYoutubeVideo, axis_youtube_video_free)

/**
 * axis_youtube_look:
 * @address: what was typed
 * @by: (nullable): who asked
 * @video: (out) (transfer full): the video's record
 *
 * Reads @address strictly, and, unless the video is already recorded as
 * being fetched or in the library, asks yt-dlp what it is (downloading
 * nothing) and records it.  Fails with %AXIS_YOUTUBE_ERROR_INVALID,
 * %AXIS_YOUTUBE_ERROR_OFF, %AXIS_YOUTUBE_ERROR_BUSY (one look at a time),
 * %AXIS_YOUTUBE_ERROR_REFUSED and %AXIS_YOUTUBE_ERROR_FAILED.
 */
gboolean
axis_youtube_look(
	AxisYoutube       *self,
	const gchar       *address,
	const gchar       *by,
	AxisYoutubeVideo **video,
	GError           **error
);

/**
 * axis_youtube_download:
 * @video_id: a video that was looked at
 * @root_id: the YouTube library folder to save it in, or 0 for the only one
 * @by: (nullable): who asked
 * @download_id: (out) (optional): its record in the downloads layer
 *
 * Queues the fetch through the downloads layer (axis_downloads_add(), the
 * built-in client), so that it is listed, followed and removed like any
 * download.  Fails with %AXIS_YOUTUBE_ERROR_NOT_FOUND,
 * %AXIS_YOUTUBE_ERROR_OFF, %AXIS_YOUTUBE_ERROR_EXISTS (being fetched, or in
 * the library with its file still there), %AXIS_YOUTUBE_ERROR_STORAGE (no
 * YouTube library folder, more than one and none chosen, storage read-only,
 * the disk offline, a file of its name there already), and with the
 * downloads layer's error.
 */
gboolean
axis_youtube_download(
	AxisYoutube  *self,
	const gchar  *video_id,
	gint64        root_id,
	const gchar  *by,
	gint64       *download_id,
	GError      **error
);

/* Fails with %AXIS_YOUTUBE_ERROR_NOT_FOUND. */
gboolean
axis_youtube_get_video(
	AxisYoutube       *self,
	const gchar       *video_id,
	AxisYoutubeVideo **video,
	GError           **error
);

/* The videos, most recently changed first. */
gboolean
axis_youtube_list_videos(
	AxisYoutube  *self,
	guint         limit,
	GPtrArray   **videos,
	GError      **error
);

/* The handler of %AXIS_YOUTUBE_FETCH_KIND: fetches the video its payload
 * names ("video").  A fetch that fails is recorded on the video with a plain
 * reason, and fails the job with the same. */
gboolean
axis_youtube_run_fetch(
	AxisYoutube     *self,
	AxisJobContext  *context,
	GError         **error
);

/* ── Pure pieces (tests) ────────────────────────────────────────── */

/**
 * axis_youtube_build_argv:
 * @program: yt-dlp's path
 * @video_id: a valid video id
 * @look: %TRUE for the look (prints the record, downloads nothing), %FALSE
 *   for the fetch
 * @max_height: 480, 720, 1080 or 2160
 * @max_bytes: the largest file to take, 0 for no option
 * @cache_dir: (nullable): its cache directory
 * @js_runtime: (nullable): "node" or "deno"; %NULL: yt-dlp's own choice
 *
 * The whole command, and the only place it is made.  The video's address is
 * the last argument, after "--".  The proxy is not in it: a command is in
 * the process list for every user of the machine to read, and a proxy's
 * address can hold a password (axis_youtube_build_environment()).
 *
 * Returns: (transfer full): the argument vector, ended by %NULL
 */
gchar **
axis_youtube_build_argv(
	const gchar *program,
	const gchar *video_id,
	gboolean     look,
	guint        max_height,
	guint64      max_bytes,
	const gchar *cache_dir,
	const gchar *js_runtime
);

/**
 * axis_youtube_build_environment:
 * @home: (nullable): yt-dlp's HOME, an empty directory of Axis's
 * @config: (nullable): its XDG_CONFIG_HOME, likewise
 * @cache: (nullable): its XDG_CACHE_HOME
 * @proxy: (nullable): a proxy's address
 *
 * What yt-dlp's environment holds besides the runner's minimal one
 * (probe/axis-probe-run.h): HOME, XDG_CONFIG_HOME and XDG_CACHE_HOME, each
 * the directory given or, for %NULL, a place that does not exist (never the
 * daemon user's own home, which yt-dlp would find by itself when HOME is not
 * set); and, with @proxy, ALL_PROXY, HTTPS_PROXY and HTTP_PROXY in both
 * spellings, which yt-dlp reads when it has no --proxy.
 *
 * Returns: (transfer full): "NAME=value" entries, ended by %NULL
 */
gchar **
axis_youtube_build_environment(
	const gchar *home,
	const gchar *config,
	const gchar *cache,
	const gchar *proxy
);

/**
 * axis_youtube_names:
 * @video: a video's record
 * @channel_dir: (out) (transfer full): "<Channel> [<channel id>]"
 * @season_dir: (out) (transfer full): "Season <year>" ("Season 0000" for a
 *   video with no upload date)
 * @file_name: (out) (transfer full): "<Channel> - <date> - <Title> [<video
 *   id>].mkv"
 *
 * The names of a video in the library, each one component of at most
 * %AXIS_IMPORT_NAME_BUDGET bytes, by the import's character rule.
 */
void
axis_youtube_names(
	const AxisYoutubeVideo  *video,
	gchar                  **channel_dir,
	gchar                  **season_dir,
	gchar                  **file_name
);

/* The extension a video file of yt-dlp's may have, as Axis writes it ("mkv",
 * "mp4", "webm"), for @extension in any case; %NULL for any other. */
const gchar *
axis_youtube_extension(const gchar *extension);

/* axis_youtube_names() for a file whose extension is @extension (one of
 * axis_youtube_extension()'s; anything else is taken for "mkv"): a video
 * YouTube serves as one file keeps the container it came in. */
void
axis_youtube_names_as(
	const AxisYoutubeVideo  *video,
	const gchar             *extension,
	gchar                  **channel_dir,
	gchar                  **season_dir,
	gchar                  **file_name
);

/* Whether @text can be the "youtube.proxy" setting: an http, https, socks4,
 * socks4a, socks5 or socks5h address of printable ASCII. */
gboolean
axis_youtube_proxy_valid(const gchar *text);

G_END_DECLS

#endif /* AXIS_YOUTUBE_H */
