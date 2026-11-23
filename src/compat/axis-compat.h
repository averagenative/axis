/*
 * axis-compat.h - the client compatibility report: the media server
 * connection, the collection of what its players do, and the settings
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Originals are deleted after conversion, so the engine must know what the
 * household's players can really play before it recommends or allows a
 * target.  This module reads that from the media servers (Plex and Jellyfin,
 * one connection of each kind, either or both), read only: it sends nothing
 * but GET requests to the configured address of each, with that server's
 * secret as a request header (core/axis-http.h), and parses what comes
 * back as untrusted (axis-compat-plex.h, axis-compat-jellyfin.h).  The one
 * exception is telling a server which folder changed after an import
 * (axis-compat-notify.h): Plex's is a GET too, Jellyfin's is the single
 * pinned POST of core/axis-http.h.  What
 * differs between the kinds is in one table (AxisCompatKind,
 * axis-compat-private.h); everything stored says which kind it came from.
 *
 *  - compat.poll (queue "compat") looks at the sessions in progress, at an
 *    adaptive interval (compat.poll_interval while anything plays, backing
 *    off by doubling to compat.idle_interval when nothing does; failures
 *    back off too).  Each session is recorded once, whatever the number of
 *    polls or restarts, with the evidence per stream
 *    (axis-compat-evidence.h), matched to an Axis file through the path
 *    map when it can be.
 *  - compat.history (queue "compat") imports the server's play history at
 *    first connection and daily.  The history records no playback decision:
 *    it only says which devices played which library and when, and so who
 *    a verdict has to consider.
 *  - Retention removes sessions and plays older than compat.retention.
 *
 * The "compat" queue is its own so that a slow or dead media server never
 * holds up the maintenance queue's refreshes or anything else: a poll
 * waits at most the request deadline (15 s) per request.  It needs one
 * pool connection more (the daemon requires the pool to be at least the
 * jobs' total concurrency plus two).
 *
 * Every function that touches the database or the network blocks: call it
 * from a worker thread.
 */

#ifndef AXIS_COMPAT_H
#define AXIS_COMPAT_H

#include <glib.h>

#include "compat/axis-compat-path.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "storage/axis-storage.h"
#include "transcode/axis-estimator.h"

G_BEGIN_DECLS

#define AXIS_COMPAT_MODULE_ID               "compat"
#define AXIS_COMPAT_QUEUE                   "compat"
#define AXIS_COMPAT_POLL_KIND               "compat.poll"
#define AXIS_COMPAT_POLL_SCHEDULE           "compat.poll"
#define AXIS_COMPAT_POLL_INTERVAL_SECONDS   30
#define AXIS_COMPAT_NOTIFY_KIND             "compat.notify"
#define AXIS_COMPAT_NOTIFY_SCHEDULE         "compat.notify"
#define AXIS_COMPAT_NOTIFY_INTERVAL_SECONDS 15
#define AXIS_COMPAT_HISTORY_KIND            "compat.history"
#define AXIS_COMPAT_HISTORY_SCHEDULE        "compat.history"
#define AXIS_COMPAT_HISTORY_INTERVAL_SECONDS 3600
#define AXIS_COMPAT_REQUEST_PRIORITY        10

/* The kinds of media server, as stored in server_kind. */
#define AXIS_COMPAT_KIND_PLEX               "plex"
#define AXIS_COMPAT_KIND_JELLYFIN           "jellyfin"

/* The connection settings are per server kind ("compat.plex_*",
 * "compat.jellyfin_*"), each group optional. */
#define AXIS_COMPAT_SETTING_ADDRESS         "compat.plex_address"
#define AXIS_COMPAT_SETTING_TOKEN           "compat.plex_token"
#define AXIS_COMPAT_SETTING_UNTRUSTED_TLS   "compat.plex_accept_untrusted_certificate"
#define AXIS_COMPAT_JELLYFIN_ADDRESS        "compat.jellyfin_address"
#define AXIS_COMPAT_JELLYFIN_TOKEN          "compat.jellyfin_token"
#define AXIS_COMPAT_JELLYFIN_UNTRUSTED_TLS  "compat.jellyfin_accept_untrusted_certificate"
/* Telling a server which folder changed (axis-compat-notify.h): on by
 * default once the server is set up, per kind of server, and how long a folder
 * must be quiet before it is sent. */
#define AXIS_COMPAT_SETTING_NOTIFY_PLEX     "compat.notify_plex"
#define AXIS_COMPAT_SETTING_NOTIFY_JELLYFIN "compat.notify_jellyfin"
#define AXIS_COMPAT_SETTING_NOTIFY_QUIET    "compat.notify_quiet"
#define AXIS_COMPAT_SETTING_LOOKBACK        "compat.lookback"
#define AXIS_COMPAT_SETTING_RETENTION       "compat.retention"
#define AXIS_COMPAT_SETTING_ACCOUNT_NAMES   "compat.store_account_names"
#define AXIS_COMPAT_SETTING_POLL_INTERVAL   "compat.poll_interval"
#define AXIS_COMPAT_SETTING_IDLE_INTERVAL   "compat.idle_interval"

/* The history is imported at most this often by the schedule, and a run
 * reads at most this many pages of AXIS_COMPAT_HISTORY_PAGE plays. */
#define AXIS_COMPAT_HISTORY_REFRESH_SECONDS (23 * 3600)
#define AXIS_COMPAT_HISTORY_PAGE            200
#define AXIS_COMPAT_HISTORY_MAX_PAGES       50
#define AXIS_COMPAT_HISTORY_DEADLINE_SECONDS 150

/* Failures back off by doubling up to this. */
#define AXIS_COMPAT_FAILURE_BACKOFF_MAX_SECONDS (15 * 60)

/* The libraries are fetched again when older than this. */
#define AXIS_COMPAT_LIBRARIES_REFRESH_SECONDS 3600

#define AXIS_COMPAT_ERROR (axis_compat_error_quark())

typedef enum
{
	AXIS_COMPAT_ERROR_NOT_CONFIGURED,
	AXIS_COMPAT_ERROR_SERVER,	/* the server's answer could not be used */
	AXIS_COMPAT_ERROR_DEADLINE,
	AXIS_COMPAT_ERROR_NOT_FOUND,
	AXIS_COMPAT_ERROR_INVALID,
	AXIS_COMPAT_ERROR_FAILED
} AxisCompatError;

GQuark
axis_compat_error_quark(void);

typedef struct _AxisCompat AxisCompat;

/**
 * AxisCompatTuning:
 *
 * For tests: the request deadline and size cap, and the settings-independent
 * clock offset of the next-poll arithmetic (not used by the daemon).
 */
typedef struct
{
	guint  request_timeout_ms;
	gsize  max_body_bytes;
	gint64 max_session_rows;
} AxisCompatTuning;

/* A media server that lies (or an attack on the link) could invent a new
 * session at every look; once this many are stored, new sessions are no
 * longer recorded (known ones still are updated) until the retention has
 * made room. */
#define AXIS_COMPAT_MAX_SESSION_ROWS 500000

/**
 * axis_compat_new:
 * @pool: the database; must outlive the result
 * @storage: referenced (kept for the roots' sake)
 * @settings: the store the module's settings are read from; referenced
 * @estimator: (nullable): where the estimated savings come from; referenced
 */
AxisCompat *
axis_compat_new(
	AxisDbPool    *pool,
	AxisStorage   *storage,
	AxisSettings  *settings,
	AxisEstimator *estimator
);

AxisCompat *
axis_compat_ref(AxisCompat *self);

void
axis_compat_unref(AxisCompat *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompat, axis_compat_unref)

const AxisSettingInfo *
axis_compat_get_setting_table(void);

void
axis_compat_set_tuning(
	AxisCompat             *self,
	const AxisCompatTuning *tuning
);

/* Whether the "compat" module is on: other pages show verdicts only while
 * it is.  Any thread. */
void
axis_compat_set_enabled(
	AxisCompat *self,
	gboolean    enabled
);

gboolean
axis_compat_is_enabled(AxisCompat *self);

AxisDbPool *
axis_compat_get_pool(AxisCompat *self);

AxisEstimator *
axis_compat_get_estimator(AxisCompat *self);

/* ── The connection ─────────────────────────────────────────────── */

/* The kinds of media server, in the order they are shown. */
guint
axis_compat_n_kinds(void);

const gchar *
axis_compat_kind_name(guint index);

/* "Plex", "Jellyfin"; %NULL for a name that is not a kind. */
const gchar *
axis_compat_kind_label(const gchar *kind);

/* Whether a kind keeps a play history that can be imported. */
gboolean
axis_compat_kind_has_history(const gchar *kind);

/* "Plex Media Server", "Jellyfin Server": what to call a server of this kind
 * that has not said its own name; %NULL for a name that is not a kind. */
const gchar *
axis_compat_kind_product(const gchar *kind);

/* The name a person sees for a device, library or setting handle: for Plex
 * the id as it is, for another kind "<kind>~<id>" ("~" is in no id), so that
 * an address of the pages names one thing, and the addresses of Plex's
 * remain what they were. */
gchar *
axis_compat_handle(
	const gchar *kind,
	const gchar *id
);

/* Splits a handle; %FALSE if the kind is unknown or the id is not valid. */
gboolean
axis_compat_handle_split(
	const gchar  *handle,
	gchar       **kind,
	gchar       **id
);

/**
 * axis_compat_is_configured:
 *
 * Whether an address and a token are set for any kind of server.  (The
 * token itself is never given out except to the request helper.)
 */
gboolean
axis_compat_is_configured(AxisCompat *self);

gboolean
axis_compat_is_configured_kind(
	AxisCompat  *self,
	const gchar *kind
);

/* The configured Plex server as scheme://host:port for showing (never the
 * token), or %NULL when the connection is not set up. */
gchar *
axis_compat_describe_address(AxisCompat *self);

gchar *
axis_compat_describe_address_kind(
	AxisCompat  *self,
	const gchar *kind
);

/**
 * AxisCompatTestResult:
 * @ok: the server answered and identified itself
 * @name: its name, when it says
 * @version: its version
 * @message: on failure, why, in a cleaned sentence without the token
 */
typedef struct
{
	gboolean  ok;
	gchar    *name;
	gchar    *version;
	gchar    *message;
} AxisCompatTestResult;

void
axis_compat_test_result_clear(AxisCompatTestResult *result);

/**
 * axis_compat_test_connection:
 * @cancellable: (nullable)
 *
 * Asks the server who it is (the connection test); changes nothing.  Fills
 * @result either way; always returns %TRUE unless @error is set for
 * something that is not the connection's fault.
 */
void
axis_compat_test_connection(
	AxisCompat           *self,
	GCancellable         *cancellable,
	AxisCompatTestResult *result
);

/* As above for the server of @kind (axis_compat_test_connection() tests
 * Plex's). */
void
axis_compat_test_connection_kind(
	AxisCompat           *self,
	const gchar          *kind,
	GCancellable         *cancellable,
	AxisCompatTestResult *result
);

/* ── Collection ─────────────────────────────────────────────────── */

/**
 * AxisCompatPollSummary:
 * @playing: sessions the server listed
 * @recorded: new sessions recorded
 * @updated: known sessions seen again
 * @skipped: sessions that could not be recorded (no session id, no device,
 *   no item)
 * @matched: new sessions matched to an Axis file
 * @skipped_poll: the poll was not due and nothing was asked
 */
typedef struct
{
	guint    playing;
	guint    recorded;
	guint    updated;
	guint    skipped;
	guint    matched;
	gboolean skipped_poll;
} AxisCompatPollSummary;

/**
 * axis_compat_poll:
 * @force: poll even if the next poll is not due (tests, the "poll now" button)
 * @cancellable: (nullable)
 * @summary: (out) (optional)
 *
 * One look at the sessions in progress, recorded, at every configured
 * server (each in a thread of its own when more than one is configured, so
 * that one that does not answer holds up none of the others).  Does nothing
 * and succeeds when no server is configured or no next poll is yet due.
 * On failure the state of the server that failed records the (cleaned)
 * error and its back-off, the others are looked at all the same, and the
 * first error is returned.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_compat_poll(
	AxisCompat            *self,
	gboolean               force,
	GCancellable          *cancellable,
	AxisCompatPollSummary *summary,
	GError               **error
);

typedef struct
{
	guint    pages;
	guint    plays;		/* plays stored (new) */
	guint    seen;		/* plays read */
	gboolean complete;	/* reached the retention horizon or the end */
	gboolean skipped;
} AxisCompatHistorySummary;

/**
 * axis_compat_import_history:
 * @force: import even if one was done in the last day
 *
 * Reads the server's play history back to the retention horizon (at most
 * %AXIS_COMPAT_HISTORY_MAX_PAGES pages) and stores who played which library
 * when.  Adds no evidence of any playback decision.  Applies the retention.
 */
gboolean
axis_compat_import_history(
	AxisCompat               *self,
	gboolean                  force,
	GCancellable             *cancellable,
	AxisCompatHistorySummary *summary,
	GError                  **error
);

/* Removes sessions, plays and devices older than the retention period. */
gboolean
axis_compat_apply_retention(
	AxisCompat  *self,
	guint       *sessions,
	guint       *plays,
	GError     **error
);

/* The job handlers. */
gboolean
axis_compat_run_poll_job(
	AxisCompat      *self,
	AxisJobContext  *context,
	GError         **error
);

gboolean
axis_compat_run_history_job(
	AxisCompat      *self,
	AxisJobContext  *context,
	GError         **error
);

/* An administrator's "import now" / "look now": queue a job. */
gboolean
axis_compat_request_history(
	AxisCompat  *self,
	gint64      *job_id,
	GError     **error
);

gboolean
axis_compat_request_poll(
	AxisCompat  *self,
	gint64      *job_id,
	GError     **error
);

G_END_DECLS

#endif /* AXIS_COMPAT_H */
