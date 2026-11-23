/*
 * axis-compat-report.h - what the pages show: coverage, devices and their
 * capability tables, what fails today, libraries with their mapping, and
 * the verdicts and the recommendation
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything is computed from the stored sessions, plays, libraries and
 * path map when asked for (nothing derived is stored, so nothing can be
 * stale), with the rules of axis-compat-verdict.h.  Every function that
 * touches the database blocks: call it from a worker thread.
 */

#ifndef AXIS_COMPAT_REPORT_H
#define AXIS_COMPAT_REPORT_H

#include <glib.h>

#include "compat/axis-compat.h"
#include "compat/axis-compat-verdict.h"
#include "transcode/axis-estimate-target.h"

G_BEGIN_DECLS

/* ── Coverage ───────────────────────────────────────────────────── */

/**
 * AxisCompatCoverage:
 * @started_us: when collection started (0: it has not)
 * @sessions: sessions observed; @local and @remote of them by where the
 *   player was; @matched have an Axis file
 * @first_session_us, @last_session_us: the period they cover (0: none)
 * @plays: imported plays; @first_play_us and @last_play_us their period
 * @devices: devices that played within the look-back window
 * @observed_devices: devices with at least one observed session
 */
typedef struct
{
	gchar    *kind;		/* "plex", "jellyfin" */
	gchar    *label;	/* "Plex" */
	gboolean  configured;
	gboolean  has_history;	/* the server keeps a play history to import */
	gint64    started_us;
	gint64    last_poll_us;
	gint64    last_ok_us;
	gint64    next_poll_us;
	gint64    history_us;
	gint      failures;
	gint      playing_now;
	gchar    *last_error;
	gchar    *history_error;
	gchar    *server_name;
	gchar    *server_version;
	gint64    sessions;
	gint64    plays;
	gint64    devices;	/* known by the server's kind: seen or listed */
	gint64    observed_devices;
} AxisCompatServerStatus;

void
axis_compat_server_status_free(AxisCompatServerStatus *status);

/* The totals are those of every server; the figures about the connection
 * (@last_poll_us and the like, @server_name) are those of the Plex
 * connection, as they always were, and @servers has one entry for each kind
 * of server with its own.  @started_us is the earliest start of any. */
typedef struct
{
	gboolean  configured;	/* any kind */
	gint64    started_us;
	gint64    sessions;
	gint64    local_sessions;
	gint64    remote_sessions;
	gint64    matched_sessions;
	gint64    first_session_us;
	gint64    last_session_us;
	gint64    plays;
	gint64    first_play_us;
	gint64    last_play_us;
	gint64    devices;
	gint64    observed_devices;
	gint64    last_poll_us;
	gint64    last_ok_us;
	gint64    next_poll_us;
	gint64    history_us;
	gint      failures;
	gint      playing_now;
	gchar    *last_error;
	gchar    *history_error;
	gchar    *server_name;
	gchar    *server_version;
	GPtrArray *servers;	/* AxisCompatServerStatus, one per kind, in order */
} AxisCompatCoverage;

/* The status of one kind, or %NULL. */
const AxisCompatServerStatus *
axis_compat_coverage_server(
	const AxisCompatCoverage *coverage,
	const gchar              *kind
);

void
axis_compat_coverage_clear(AxisCompatCoverage *coverage);

gboolean
axis_compat_load_coverage(
	AxisCompat          *self,
	AxisCompatCoverage  *coverage,
	GError             **error
);

/* ── Devices ────────────────────────────────────────────────────── */

/* One row of a device's capability table. */
typedef struct
{
	AxisCompatObservation obs;	/* its combo points into the strings below */
	gchar                *codec;
	gchar                *profile;
	gchar                *res_class;
	gchar                *container;
	gchar                *out_codec;
	gchar                *stated;	/* the reasons the server stated, or NULL */
	gint                  channels;
	gint64                last_us;
} AxisCompatObsRow;

void
axis_compat_obs_row_free(AxisCompatObsRow *row);

/* "hevc main 10 10-bit 2160p HDR in mkv": the combination, as text. */
gchar *
axis_compat_describe_row(const AxisCompatObsRow *row);

typedef struct
{
	gchar     *server_kind;	/* "plex", "jellyfin" */
	gchar     *client_id;
	gchar     *name;
	gchar     *platform;
	gchar     *product;
	gboolean   pinned;
	gint64     first_seen_us;
	gint64     last_seen_us;
	guint      sessions;
	guint      remote_sessions;
	guint      plays_in_window;
	gint64     last_play_us;
	GPtrArray *rows;	/* AxisCompatObsRow, ordered; filled for one device */
} AxisCompatDeviceInfo;

void
axis_compat_device_info_free(AxisCompatDeviceInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatDeviceInfo, axis_compat_device_info_free)

/* Every device with how much evidence it has, most recently active first. */
gboolean
axis_compat_list_devices(
	AxisCompat  *self,
	GPtrArray  **devices,
	GError     **error
);

/* One device with its capability table; %AXIS_COMPAT_ERROR_NOT_FOUND.
 * @handle is axis_compat_handle() of the device: a Plex device's id as it
 * is, another kind's "<kind>~<id>". */
gboolean
axis_compat_get_device(
	AxisCompat            *self,
	const gchar           *handle,
	AxisCompatDeviceInfo **info,
	GError               **error
);

gboolean
axis_compat_set_pinned(
	AxisCompat   *self,
	const gchar  *handle,
	gboolean      pinned,
	GError      **error
);

/* ── What fails today ───────────────────────────────────────────── */

typedef struct
{
	gchar            *server_kind;
	gchar            *client_id;
	gchar            *device_name;
	gint              kind;
	gchar            *codec;
	gchar            *out_codec;
	AxisCompatEffect  effect;
	AxisCompatProperty property;	/* what the server blamed, or none */
	gchar            *stated;	/* the reasons it stated, or NULL */
	guint             plays;
	guint             remote_plays;
	guint             files;
	gint64            bytes;
} AxisCompatFailure;

void
axis_compat_failure_free(AxisCompatFailure *failure);

typedef struct
{
	gint64   file_id;
	gchar   *rel_path;	/* stored form */
	gint64   size;
	guint    plays;
	guint    devices;
} AxisCompatFailedFile;

void
axis_compat_failed_file_free(AxisCompatFailedFile *file);

/* Combinations the server converts when played, by device and reason, and
 * the files most often converted.  Unmatched sessions count in the first,
 * not in the second. */
gboolean
axis_compat_report_failures(
	AxisCompat  *self,
	GPtrArray  **failures,
	GPtrArray  **files,
	GError     **error
);

/* ── The path map ───────────────────────────────────────────────── */

/**
 * axis_compat_path_map_add:
 * @problem: (out) (optional) (transfer full): why a path was refused, for
 *   the form
 *
 * Fails with %AXIS_COMPAT_ERROR_INVALID (and a problem) for a path that is
 * not absolute and clean, or one that is already mapped.
 */
gboolean
axis_compat_path_map_add(
	AxisCompat   *self,
	const gchar  *server_path,
	const gchar  *axis_path,
	gchar       **problem,
	GError      **error
);

/* As above for the paths of the server of @kind (the one above maps Plex's). */
gboolean
axis_compat_path_map_add_kind(
	AxisCompat   *self,
	const gchar  *kind,
	const gchar  *server_path,
	const gchar  *axis_path,
	gchar       **problem,
	GError      **error
);

/* The entries (AxisCompatPathEntry), oldest first. */
gboolean
axis_compat_list_path_map(
	AxisCompat  *self,
	GPtrArray  **entries,
	GError     **error
);

gboolean
axis_compat_path_map_remove(
	AxisCompat  *self,
	gint64       id,
	GError     **error
);

/* ── Libraries, verdicts, recommendation ────────────────────────── */

typedef struct
{
	gchar            *server_kind;
	gchar            *client_id;
	gchar            *name;
	gboolean          pinned;
	AxisCompatVerdict verdict;
	AxisCompatWhy     why;
	gchar            *detail;	/* what decided it, for administrators */
	guint             plays;	/* plays in the window */
	gint64            last_play_us;
	guint             local_evidence;
	guint             remote_evidence;
} AxisCompatDeviceVerdict;

void
axis_compat_device_verdict_free(AxisCompatDeviceVerdict *verdict);

typedef struct
{
	AxisEstimateTarget  target;
	AxisCompatVerdict   verdict;
	GPtrArray          *devices;	/* AxisCompatDeviceVerdict, risky first */
	gint64              saving;	/* bytes, or -1 when there is no estimate */
	guint               n_safe;
	guint               n_risky;
	guint               n_unknown;
} AxisCompatTargetVerdict;

typedef struct
{
	gchar               *server_kind;	/* which server it is of: "plex", "jellyfin" */
	gchar               *key;
	gchar               *title;
	gchar               *kind;
	AxisCompatMapState   state;
	GArray              *root_ids;
	GPtrArray           *root_names;
	GPtrArray           *folders;		/* utf8: all of the library's folders */
	GPtrArray           *unmapped_folders;	/* utf8: those that reach no root */
	GPtrArray           *suggestions;	/* AxisCompatSuggestion */
	const gchar         *res_class;		/* the highest in the library, "2160p" when not known */
	gboolean             probed;		/* the library's files have all been probed */
	gboolean             has_sdr;
	gboolean             has_hdr;
	guint                n_devices;		/* considered */
	GPtrArray           *targets;		/* AxisCompatTargetVerdict, those that apply */
	gint                 recommended;	/* index into targets, or -1 */
	gint                 best;		/* the largest saving, or -1 */
	gint                 next;		/* the next largest, or -1 */
} AxisCompatLibraryReport;

typedef struct
{
	AxisCompatCoverage coverage;
	GPtrArray         *libraries;		/* AxisCompatLibraryReport */
	gboolean           have_estimates;
	gint64             estimates_us;
	gint64             lookback_seconds;
} AxisCompatReport;

void
axis_compat_report_free(AxisCompatReport *report);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatReport, axis_compat_report_free)

/**
 * axis_compat_compute_report:
 *
 * Reads the stored evidence once and works out, for every library the
 * server has: how it maps to Axis roots, which devices count (those that
 * played it within the look-back window, and pinned devices), the verdict
 * for each target that applies to it, and the recommendation.  Only
 * libraries that map completely get verdicts; the others are listed as
 * they are.
 */
gboolean
axis_compat_compute_report(
	AxisCompat         *self,
	AxisCompatReport  **report,
	GError            **error
);

/* How many libraries have each verdict for @target_id, and how many the
 * target applies to. */
void
axis_compat_report_target_counts(
	const AxisCompatReport *report,
	const gchar            *target_id,
	guint                  *safe,
	guint                  *risky,
	guint                  *unknown
);

/**
 * axis_compat_report_root_verdict:
 * @root_id: an Axis root
 * @target_id: a profile's slug
 * @libraries: (out) (optional): how many of the server's libraries that
 *   map to the root gave a verdict
 *
 * The verdict for a profile on a root: the worst of the libraries of every
 * server that map to the root; unknown when none does (or the report has
 * no verdict for the profile).
 */
AxisCompatVerdict
axis_compat_report_root_verdict(
	const AxisCompatReport *report,
	gint64                  root_id,
	const gchar            *target_id,
	guint                  *libraries
);

/* The library report with this key (a Plex library's key as it is, another
 * kind's "<kind>~<key>": axis_compat_handle()), or NULL. */
const AxisCompatLibraryReport *
axis_compat_report_find_library(
	const AxisCompatReport *report,
	const gchar            *key
);

G_END_DECLS

#endif /* AXIS_COMPAT_REPORT_H */
