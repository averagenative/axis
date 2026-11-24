/*
 * main.c - axisd, the Axis media engine daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "axis-version.h"
#include "axisd/axis-daemon-lock.h"
#include "acquire/axis-acquire.h"
#include "api/axis-portal-api.h"
#include "api/axis-portal-tokens.h"
#include "api/axis-worker-api.h"
#include "api/axis-worker-tokens.h"
#include "arr/axis-arr.h"
#include "core/axis-config.h"
#include "core/axis-data-dir.h"
#include "core/axis-error.h"
#include "core/axis-log.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "core/axis-outbound.h"
#include "core/axis-settings.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"
#include "download/axis-download.h"
#include "download/axis-download-deluge.h"
#include "download/axis-download-qbittorrent.h"
#include "download/axis-download-sabnzbd.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-cardigann.h"
#include "indexer/axis-indexer-def-check.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-sites.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "jobs/axis-history.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-analysis.h"
#include "library/axis-library.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"
#include "meta/axis-meta-tvdb.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"
#include "storage/axis-storage.h"
#include "compat/axis-compat.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-estimator.h"
#include "web/axis-web.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-arr.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-compat.h"
#include "requests/axis-requests.h"
#include "web/axis-web-discover.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-health.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-limits.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-library.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-metrics.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-profiles.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-ready.h"
#include "web/axis-web-savings.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-setup.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-workers.h"
#include "web/axis-web-youtube.h"
#include "youtube/axis-youtube.h"
#include "web/axis-web-conversions.h"

/* A health check gives the database this long to answer. */
#define HEALTH_PING_TIMEOUT_MS 2000

/* --healthcheck gives the daemon this long to answer. */
#define HEALTHCHECK_TIMEOUT_SECONDS 4

/* Waiting for the database at start: the first retry after this long,
 * doubling up to the cap. */
#define START_RETRY_FIRST_S 1
#define START_RETRY_MAX_S   30

/* A pool that job threads still running at exit may use: deliberately not
 * freed, and kept reachable so that a leak checker does not report it. */
static AxisDbPool *pool_left_open;

/* The single-daemon lock, likewise: held until the process ends while those
 * threads may still run, so that no other daemon takes over beside them. */
static AxisDaemonLock *lock_left_open;

/* The same for what a still-running deletion of old history uses. */
static AxisHistory    *history_left_open;

/* The same for what a still-running storage check or scan uses. */
static AxisStorage    *storage_left_open;
static AxisWebStorage *storage_ui_left_open;
static AxisLibrary    *library_left_open;
static AxisWebLibrary *library_ui_left_open;
static AxisProbe      *probe_left_open;
static AxisWebProbe   *probe_ui_left_open;
static AxisAnalysis    *analysis_left_open;
static AxisWebAnalysis *analysis_ui_left_open;
static AxisEstimator   *estimator_left_open;
static AxisWebSavings  *savings_ui_left_open;
static AxisCompat      *compat_left_open;
static AxisWebCompat   *compat_ui_left_open;
static AxisWebProfiles *profiles_ui_left_open;
static AxisWebWorkers  *workers_ui_left_open;
static AxisMeta        *meta_left_open;
static AxisWebMeta     *meta_ui_left_open;
static AxisDownloads    *downloads_left_open;
static AxisWebDownloads *downloads_ui_left_open;
static AxisIndexers     *indexers_left_open;
static AxisWebIndexers  *indexers_ui_left_open;
static AxisQualityStore *quality_left_open;
static AxisWebQuality   *quality_ui_left_open;
static AxisMovies       *movies_left_open;
static AxisTv           *tv_left_open;
static AxisImport       *import_left_open;
static AxisWebImports   *imports_ui_left_open;
static AxisIdentify     *identify_left_open;
static AxisWebIdentify  *identify_ui_left_open;
static AxisConversions    *conversions_left_open;
static AxisWebConversions *conversions_ui_left_open;
static AxisDiscover       *discover_left_open;
static AxisWebDiscover    *discover_ui_left_open;
static AxisArr            *arr_left_open;
static AxisWebArr         *arr_ui_left_open;
static AxisYoutube        *youtube_left_open;
static AxisWebYoutube     *youtube_ui_left_open;

G_STATIC_ASSERT(AXIS_CONFIG_MIN_POOL_SIZE >= AXIS_DB_POOL_MIN_SIZE);
G_STATIC_ASSERT(AXIS_CONFIG_MAX_POOL_SIZE <= AXIS_DB_POOL_MAX_SIZE);

/*
 * What the probes ask (web/axis-web-health.h, which shares each question
 * among the probes that arrive while it is asked): the pool, and the lock
 * that readiness needs held.
 */
typedef struct
{
	AxisDbPool     *pool;
	AxisDaemonLock *daemon_lock;
} Probes;

static gboolean
on_signal(gpointer user_data)
{
	GMainLoop *loop = user_data;

	g_message("shutting down");
	g_main_loop_quit(loop);

	return G_SOURCE_REMOVE;
}

/* On a thread of its own: the database answers. */
static gboolean
probe_ping(
	gpointer  data,
	GError  **error
){
	Probes *probes = data;

	return axis_db_pool_ping(probes->pool, HEALTH_PING_TIMEOUT_MS, error);
}

/* On a thread of its own: the database answers at this build's schema. */
static gboolean
probe_current(
	gpointer  data,
	GError  **error
){
	Probes *probes = data;

	return axis_db_migrate_check_current(probes->pool, HEALTH_PING_TIMEOUT_MS, error);
}

static gboolean
probe_held(gpointer data)
{
	Probes *probes = data;

	return probes->daemon_lock == NULL || axis_daemon_lock_is_held(probes->daemon_lock);
}

/*
 * Start-up.  The daemon listens at once, before it has a database, so that
 * a container platform sees it alive; until everything is ready, the gate
 * (the outermost middleware) answers every request itself: /livez alive,
 * /readyz and /healthz not ready ({"status":"starting"}, or, while another
 * daemon holds the database, {"status":"held by another daemon"}), and
 * anything else 503 with Retry-After.  No page, no API and no static file
 * is served before the database is connected, held and migrated and every
 * module has started.
 *
 * While serving, a daemon that loses its database lock suspends: the gate
 * passes only the probes and the metrics to their own handlers (/readyz
 * then answers {"status":"database lock lost"}) and answers everything else
 * 503 itself, until the lock is held again (axis-daemon-lock.h).
 *
 * The phase is atomic: the start thread sets the standby, and the lock's
 * watcher the suspension.
 */
typedef enum
{
	START_WAITING,	/* for the database, or for the migrations */
	START_STANDBY,	/* another daemon holds the database */
	START_SERVING,
	START_SUSPENDED	/* serving, but the database lock was lost */
} StartPhase;

typedef struct
{
	gint phase;	/* atomic: a StartPhase */
} StartGate;

static HtmxResponse *
gate_refusal(const gchar *text)
{
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, 503);
	htmx_response_set_content(response, text);
	htmx_response_set_content_type(response, "text/plain; charset=utf-8");
	htmx_response_add_header(response, "Retry-After", "5");
	htmx_response_add_header(response, "Cache-Control", "no-store");

	return response;
}

/* What the gate says to a request it does not serve in @phase (and what a
 * request with a body is told then, by the request limits). */
static const gchar *
gate_words(gint phase)
{
	if (phase == START_SUSPENDED)
		return "Axis has lost its hold on its database and is taking it back; it serves nothing meanwhile. "
			"Try again in a few seconds.\n";
	if (phase == START_STANDBY)
		return "Axis is waiting: another Axis daemon is using this database. This one takes over when that "
			"one stops.\n";
	return "Axis is starting: it is waiting for its database. Try again in a few seconds.\n";
}

/* The request limits' words for a body refused (axis-web-limits.h). */
static const gchar *
gate_refusal_words(gpointer data)
{
	StartGate *gate = data;

	return gate_words(g_atomic_int_get(&gate->phase));
}

static void
start_gate(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	StartGate *gate = user_data;
	HtmxRequest *request;
	const gchar *path;
	HtmxResponse *response;
	gint phase = g_atomic_int_get(&gate->phase);

	if (phase == START_SERVING) {
		next(context, next_data);
		return;
	}

	request = htmx_context_get_request(context);
	path = htmx_request_get_path(request);
	if (phase == START_SUSPENDED) {
		/* The probes and the metrics answer from their own handlers:
		 * they change nothing, and /readyz says why it is not ready. */
		if (g_strcmp0(path, "/livez") == 0 || g_strcmp0(path, "/readyz") == 0 ||
		    g_strcmp0(path, "/healthz") == 0 || g_strcmp0(path, "/metrics") == 0) {
			next(context, next_data);
			return;
		}
		response = gate_refusal(gate_words(phase));
	} else if (g_strcmp0(path, "/livez") == 0) {
		response = axis_web_health_probe_response(200, "alive");
		axis_web_health_mark(request, response);
	} else if (g_strcmp0(path, "/readyz") == 0 || g_strcmp0(path, "/healthz") == 0) {
		response = axis_web_health_probe_response(503, phase == START_STANDBY ? "held by another daemon" :
			"starting");
		axis_web_health_mark(request, response);
	} else {
		response = gate_refusal(gate_words(phase));
	}
	/* The shell's headers are not installed yet: the strictest ones. */
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	htmx_context_set_response(context, response);	/* takes it */
}

/*
 * The start thread: connects to the database (retrying with back-off for
 * as long as database.wait allows), takes the single-daemon lock (waiting,
 * within the same database.wait, while another daemon holds it), applies the
 * migrations and brings the built-in profiles up to date.  It touches
 * nothing of the main loop's but the gate's phase (atomic); it hands its
 * result back through start_finished().
 */
typedef struct
{
	gchar          *url;		/* a copy; wiped when freed */
	guint           pool_size;
	gint            wait_s;	/* or AXIS_CONFIG_WAIT_FOREVER */
	gint           *phase;	/* the gate's, atomic */
	GMutex          lock;
	GCond           wake;
	gboolean        cancelled;	/* (lock) */
	GMainLoop      *loop;
	AxisDbPool     *pool;		/* the result, or NULL */
	AxisDaemonLock *daemon_lock;	/* with it, held */
	GError         *error;	/* why not, unless cancelled */
} StartWork;

static gboolean
start_finished(gpointer data)
{
	StartWork *work = data;

	g_main_loop_quit(work->loop);
	return G_SOURCE_REMOVE;
}

/* Sleeps @ms unless cancelled first; returns FALSE when cancelled. */
static gboolean
start_sleep_ms(
	StartWork *work,
	guint      ms
){
	gint64 until = g_get_monotonic_time() + (gint64)ms * 1000;
	gboolean go_on;

	g_mutex_lock(&work->lock);
	while (!work->cancelled && g_cond_wait_until(&work->wake, &work->lock, until))
		;
	go_on = !work->cancelled;
	g_mutex_unlock(&work->lock);

	return go_on;
}

static gboolean
start_sleep(
	StartWork *work,
	guint      seconds
){
	return start_sleep_ms(work, seconds * 1000);
}

static gboolean
start_cancelled(StartWork *work)
{
	gboolean cancelled;

	g_mutex_lock(&work->lock);
	cancelled = work->cancelled;
	g_mutex_unlock(&work->lock);

	return cancelled;
}

/* While another daemon holds the database, a reminder this often. */
#define STANDBY_NOTE_US (G_GINT64_CONSTANT(600) * G_USEC_PER_SEC)

/*
 * Takes the single-daemon lock, trying again every
 * AXIS_DAEMON_LOCK_RETRY_MS while another daemon holds it (the gate then
 * says so) and with the connection's back-off while the database cannot be
 * asked, all within database.wait counted from @begin.  FALSE: cancelled,
 * or gave up (work->error says why).
 */
static gboolean
take_daemon_lock(
	StartWork      *work,
	AxisDaemonLock *daemon_lock,
	gint64          begin
){
	g_autofree gchar *noted = NULL;	/* the holder last logged */
	gint64 waiting_since = 0;
	gint64 noted_at = 0;
	guint delay = START_RETRY_FIRST_S;

	for (;;) {
		g_autofree gchar *holder = NULL;
		g_autoptr(GError) local = NULL;
		AxisDbLockResult result = axis_daemon_lock_try(daemon_lock, &holder, &local);
		gint64 now = g_get_monotonic_time();
		gint64 elapsed_s = (now - begin) / G_USEC_PER_SEC;
		guint sleep_ms;

		if (result == AXIS_DB_LOCK_TAKEN) {
			g_atomic_int_set(work->phase, START_WAITING);
			if (waiting_since != 0)
				g_message("took the database lock after waiting %" G_GINT64_FORMAT " s: the daemon that "
					"held it has stopped", (now - waiting_since) / G_USEC_PER_SEC);
			g_info("holds the database lock (database session %d, named \"%s\")",
				axis_daemon_lock_get_backend_pid(daemon_lock), axis_daemon_lock_get_name(daemon_lock));
			return TRUE;
		}

		if (result == AXIS_DB_LOCK_BUSY) {
			g_atomic_int_set(work->phase, START_STANDBY);
			if (work->wait_s == 0) {
				g_set_error(&work->error, AXIS_ERROR, AXIS_ERROR_DATABASE_IN_USE,
					"the database is in use by another Axis daemon (%s); database.wait is 0, so this one "
					"does not wait for it", holder);
				return FALSE;
			}
			if (work->wait_s != AXIS_CONFIG_WAIT_FOREVER && elapsed_s >= work->wait_s) {
				g_set_error(&work->error, AXIS_ERROR, AXIS_ERROR_DATABASE_IN_USE,
					"gave up waiting after %" G_GINT64_FORMAT " s (database.wait is %d): the database is "
					"still in use by another Axis daemon (%s)", elapsed_s, work->wait_s, holder);
				return FALSE;
			}
			if (g_strcmp0(holder, noted) != 0 || now - noted_at >= STANDBY_NOTE_US) {
				g_autofree gchar *budget = work->wait_s == AXIS_CONFIG_WAIT_FOREVER ?
					g_strdup("for as long as it takes (database.wait is forever)") :
					g_strdup_printf("for %d s at most since it started (database.wait)", work->wait_s);

				g_warning("the database is in use by another Axis daemon: %s. This daemon serves only the "
					"probes and runs no job until that one stops, then takes over; it checks every %d s, "
					"%s", holder, AXIS_DAEMON_LOCK_RETRY_MS / 1000, budget);
				g_free(noted);
				noted = g_strdup(holder);
				noted_at = now;
			}
			if (waiting_since == 0)
				waiting_since = now;
			sleep_ms = AXIS_DAEMON_LOCK_RETRY_MS;
			delay = START_RETRY_FIRST_S;
		} else {
			/* The database went away after the connection was made: as
			 * when it cannot be reached at all.  (An earlier session of
			 * this same lock cannot hold it at start, its name being new;
			 * it is waited for the same way.) */
			if (local == NULL)
				g_set_error_literal(&local, AXIS_ERROR, AXIS_ERROR_DATABASE_IN_USE,
					"an earlier connection of this daemon still holds the database lock");
			if (work->wait_s != AXIS_CONFIG_WAIT_FOREVER && elapsed_s + (gint64)delay > work->wait_s) {
				if (work->wait_s == 0)
					g_propagate_error(&work->error, g_steal_pointer(&local));
				else
					g_set_error(&work->error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
						"gave up waiting for the database after %" G_GINT64_FORMAT " s (database.wait is "
						"%d): %s", elapsed_s, work->wait_s, local->message);
				return FALSE;
			}
			g_warning("cannot take the database lock: %s; trying again in %u s", local->message, delay);
			sleep_ms = delay * 1000;
			delay = MIN(delay * 2, START_RETRY_MAX_S);
		}

		if (!start_sleep_ms(work, sleep_ms))
			return FALSE;
	}
}

static gpointer
start_main(gpointer data)
{
	StartWork *work = data;
	gint64 begin = g_get_monotonic_time();
	guint delay = START_RETRY_FIRST_S;
	AxisDbPool *pool = NULL;
	AxisDaemonLock *daemon_lock = NULL;

	/* A URL libpq cannot read never starts working: no waiting for it. */
	if (!axis_db_url_check(work->url, &work->error))
		goto done;

	for (;;) {
		g_autoptr(GError) local = NULL;
		gint64 elapsed_s;

		pool = axis_db_pool_new(work->url, work->pool_size, &local);
		if (pool != NULL)
			break;
		/* A server that is too old does not get younger. */
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_VERSION)) {
			g_propagate_error(&work->error, g_steal_pointer(&local));
			goto done;
		}
		elapsed_s = (g_get_monotonic_time() - begin) / G_USEC_PER_SEC;
		if (work->wait_s != AXIS_CONFIG_WAIT_FOREVER && elapsed_s + (gint64)delay > work->wait_s) {
			if (work->wait_s == 0)
				g_propagate_error(&work->error, g_steal_pointer(&local));
			else
				g_set_error(&work->error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
					"gave up waiting for the database after %" G_GINT64_FORMAT " s (database.wait is %d): %s",
					elapsed_s, work->wait_s, local->message);
			goto done;
		}
		g_warning("%s; trying again in %u s", local->message, delay);
		if (!start_sleep(work, delay))
			goto done;
		delay = MIN(delay * 2, START_RETRY_MAX_S);
	}
	g_info("connected to the database (pool of %u connections)", axis_db_pool_get_size(pool));

	if (start_cancelled(work))
		goto done;
	/* One daemon per database, held before anything is written: a daemon
	 * that waits for another (a newer one during an update, say) never
	 * migrates the schema under it. */
	daemon_lock = axis_daemon_lock_new(pool);
	if (!take_daemon_lock(work, daemon_lock, begin))
		goto done;
	if (start_cancelled(work))
		goto done;
	/* The schema, then the built-in conversion profiles as this version's
	 * code has them (a built-in whose definition changed gets a new
	 * revision: the old one is kept, and the new one is locked), then the
	 * built-in quality profiles, brought up to date in place (a person
	 * never edits one), then every title's name keys that a migration left
	 * to be made again (nothing reads them before this). */
	if (!axis_db_migrate(pool, &work->error) ||
	    !axis_profile_store_sync_builtins(pool, NULL, &work->error) ||
	    !axis_quality_store_sync_builtins(pool, NULL, &work->error) ||
	    !axis_meta_fill_every_name_key(pool, &work->error))
		goto done;

	work->pool = g_steal_pointer(&pool);
	work->daemon_lock = g_steal_pointer(&daemon_lock);

done:
	/* The lock uses the pool's settings: it goes first. */
	if (daemon_lock != NULL)
		axis_daemon_lock_free(daemon_lock);
	if (pool != NULL)
		axis_db_pool_free(pool);
	g_idle_add(start_finished, work);
	return NULL;
}

static gboolean
on_start_signal(gpointer user_data)
{
	StartWork *work = user_data;

	g_message("shutting down before the database was ready");
	g_mutex_lock(&work->lock);
	work->cancelled = TRUE;
	g_cond_broadcast(&work->wake);
	g_mutex_unlock(&work->lock);

	return G_SOURCE_REMOVE;
}

/*
 * The in-process job queues of this build's modules (the queues their
 * handler tables name; the worker API's queue has no thread here).  serve()
 * checks that the job queue it builds has exactly these, so a module that
 * names another one fails every start of the daemon, its tests included.
 */
static const gchar *const job_queues[] = {
	AXIS_JOB_MAINTENANCE_QUEUE, AXIS_LIBRARY_SCAN_QUEUE, AXIS_PROBE_QUEUE, AXIS_ESTIMATE_SAMPLE_QUEUE,
	AXIS_COMPAT_QUEUE, AXIS_META_QUEUE, AXIS_DOWNLOAD_QUEUE, AXIS_ACQUIRE_SEARCH_QUEUE, AXIS_CONVERSION_QUEUE,
	AXIS_YOUTUBE_QUEUE,
	NULL
};

/* The worker threads the job queue will have, at the configured concurrency
 * (1 for a queue not set), and the connections it needs of the pool. */
static guint
jobs_pool_need(
	AxisConfig *config,
	guint      *threads
){
	guint total = 0;
	guint i;

	for (i = 0; job_queues[i] != NULL; i++) {
		guint n = AXIS_JOB_QUEUE_DEFAULT_CONCURRENCY;

		axis_config_get_jobs_concurrency(config, job_queues[i], &n);
		total += n;
	}
	if (threads != NULL)
		*threads = total;
	return total + AXIS_JOB_QUEUE_RESERVED_CONNECTIONS;
}

/*
 * The pool's size: as set, when it is set, refused (before the database is
 * asked anything, so that no migration of this build is applied and the
 * image before it still starts) when it is less than the job queue needs;
 * otherwise what the queue needs and AXIS_CONFIG_POOL_HEADROOM for the pages,
 * the health checks and the worker API, never less than
 * AXIS_CONFIG_DEFAULT_POOL_SIZE, so that a build with another queue never
 * stops a setup that set nothing.
 */
static gboolean
pool_size_for(
	AxisConfig  *config,
	guint       *size,
	GError     **error
){
	guint threads = 0;
	guint need = jobs_pool_need(config, &threads);

	if (axis_config_has_database_pool_size(config)) {
		*size = axis_config_get_database_pool_size(config);
		if (*size >= need)
			return TRUE;
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"the database pool has %u connections, but the job queue needs %u: one for each of its %u worker "
			"threads (the queues' concurrency added up) and %d for itself; set database.pool_size (or "
			"AXIS_DATABASE_POOL_SIZE) to at least %u (%u leaves room for the pages), or remove it and Axis "
			"chooses; nothing was changed in the database", *size, need, threads,
			AXIS_JOB_QUEUE_RESERVED_CONNECTIONS, need, need + AXIS_CONFIG_POOL_HEADROOM);
		return FALSE;
	}

	*size = MAX(need + AXIS_CONFIG_POOL_HEADROOM, AXIS_CONFIG_DEFAULT_POOL_SIZE);
	if (*size > AXIS_CONFIG_MAX_POOL_SIZE)
		*size = MAX(need, AXIS_CONFIG_MAX_POOL_SIZE);
	if (*size <= AXIS_CONFIG_MAX_POOL_SIZE)
		return TRUE;
	g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
		"the job queue needs %u database connections (%u worker threads and %d for itself), more than the pool "
		"can have (%d); lower jobs.concurrency", need, threads, AXIS_JOB_QUEUE_RESERVED_CONNECTIONS,
		AXIS_CONFIG_MAX_POOL_SIZE);
	return FALSE;
}

/*
 * Runs the main loop (the gate answering the probes) while the start thread
 * works.  SIGINT and SIGTERM cancel the wait between attempts; a connection
 * attempt or migration in progress is finished first (each is bounded, and a
 * migration is a transaction).  Returns the migrated pool and, in
 * *@daemon_lock, the single-daemon lock, held; or %NULL: with *@stopped set
 * when a signal ended the wait, else with @error set.
 */
static AxisDbPool *
wait_for_database(
	AxisConfig      *config,
	StartGate       *gate,
	AxisDaemonLock **daemon_lock,
	gboolean        *stopped,
	GError         **error
){
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	StartWork work;
	GThread *thread;
	guint sigint_id;
	guint sigterm_id;

	*stopped = FALSE;
	*daemon_lock = NULL;
	memset(&work, 0, sizeof(work));
	if (!pool_size_for(config, &work.pool_size, error))
		return NULL;
	work.url = g_strdup(axis_config_get_database_url(config));
	work.wait_s = axis_config_get_database_wait(config);
	work.phase = &gate->phase;
	work.loop = loop;
	g_mutex_init(&work.lock);
	g_cond_init(&work.wake);

	sigint_id = g_unix_signal_add(SIGINT, on_start_signal, &work);
	sigterm_id = g_unix_signal_add(SIGTERM, on_start_signal, &work);
	thread = g_thread_new("axis-start", start_main, &work);
	g_main_loop_run(loop);
	g_thread_join(thread);
	/* start_finished() has run: nothing of @work is left on the loop. */

	if (g_main_context_find_source_by_id(NULL, sigint_id) != NULL)
		g_source_remove(sigint_id);
	if (g_main_context_find_source_by_id(NULL, sigterm_id) != NULL)
		g_source_remove(sigterm_id);

	if (work.pool == NULL) {
		if (work.cancelled)
			*stopped = TRUE;
		else if (work.error != NULL)
			g_propagate_error(error, g_steal_pointer(&work.error));
		else
			g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_LISTEN, "start-up failed");
	}
	g_clear_error(&work.error);
	memset(work.url, 0, strlen(work.url));
	g_free(work.url);
	g_mutex_clear(&work.lock);
	g_cond_clear(&work.wake);

	*daemon_lock = work.daemon_lock;
	return work.pool;
}

/*
 * The job queue: every module's job kinds and schedules (the queue's own
 * retention job among them), with the concurrency the configuration sets.
 */
static AxisJobQueue *
jobs_new(
	AxisConfig         *config,
	AxisDbPool         *pool,
	AxisModuleRegistry *modules,
	AxisJobRegistry   **registry_out,
	GError            **error
){
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(AxisJobQueue) queue = NULL;
	g_auto(GStrv) queues = NULL;
	guint i;

	if (!axis_job_registry_add_modules(registry, modules, error))
		return NULL;
	/* The kinds only remote workers run, through the worker API. */
	if (!axis_worker_api_register_kinds(registry, modules, error))
		return NULL;
	/* Every job is named in plain words and is work or upkeep wherever it
	 * is shown: a kind that says neither is a mistake in its module. */
	{
		g_auto(GStrv) undescribed = axis_job_registry_list_undescribed(registry);

		if (undescribed[0] != NULL) {
			g_autofree gchar *list = g_strjoinv(", ", undescribed);

			g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
				"job kinds without a label or a class (work or upkeep): %s", list);
			return NULL;
		}
	}

	queue = axis_job_queue_new(pool, registry);
	queues = axis_config_list_jobs_concurrency(config);
	for (i = 0; queues[i] != NULL; i++) {
		guint n = 0;

		axis_config_get_jobs_concurrency(config, queues[i], &n);
		axis_job_queue_set_concurrency(queue, queues[i], n);
	}

	*registry_out = axis_job_registry_ref(registry);
	return g_steal_pointer(&queue);
}

/* Jobs held for a module start as soon as it is switched on. */
static void
on_module_switched(
	AxisModuleRegistry *registry,
	const gchar        *id,
	gboolean            enabled,
	gpointer            user_data
){
	if (enabled)
		axis_job_queue_wake(user_data);
}

/* A worker registered: the pages that wait for one look again. */
static void
on_worker_registered(
	const gchar *worker,
	gpointer     user_data
){
	axis_web_workers_note_registered(user_data, worker);
}

static gboolean
wake_jobs(gpointer user_data)
{
	axis_job_queue_wake(user_data);
	return G_SOURCE_REMOVE;
}

/* Conversion work held since the start is resumed (on a page's thread): the
 * jobs held start at once, from the main loop. */
static void
on_conversions_resumed(gpointer user_data)
{
	g_idle_add(wake_jobs, user_data);
}

/*
 * What the single-daemon lock's watcher stops and starts again
 * (axis-daemon-lock.h).  Its hooks run on the watcher's thread; what must
 * happen on the main loop is passed there.
 */
typedef struct
{
	AxisDaemonLock *daemon_lock;
	AxisJobQueue   *jobs;
	AxisWorkerApi  *worker_api;
	StartGate      *gate;
	AxisWebLimits  *limits;	/* bodies follow the gate */
	GMainLoop      *loop;
	gboolean        taken;	/* main loop: another session took the lock */
} Serving;

/* Main loop: the worker API hands back no lease while the lock is not held
 * (its workers' requests are refused meanwhile). */
static gboolean
serving_pause_workers(gpointer data)
{
	Serving *serving = data;

	axis_worker_api_pause(serving->worker_api, !axis_daemon_lock_is_held(serving->daemon_lock));
	return G_SOURCE_REMOVE;
}

/* Main loop: the daemon stops (status 1), to start afresh as a standby. */
static gboolean
serving_stop_taken(gpointer data)
{
	Serving *serving = data;

	serving->taken = TRUE;
	g_main_loop_quit(serving->loop);
	return G_SOURCE_REMOVE;
}

static void
on_lock_lost(gpointer data)
{
	Serving *serving = data;

	/* No new request (nor any body: what still answers needs none), then
	 * no new job, and every running one told to stop. */
	g_atomic_int_set(&serving->gate->phase, START_SUSPENDED);
	axis_web_limits_set_bodies(serving->limits, FALSE);
	axis_job_queue_suspend(serving->jobs);
	g_idle_add(serving_pause_workers, serving);
}

static gboolean
on_lock_wait_handlers(
	gpointer data,
	guint    timeout_ms
){
	Serving *serving = data;

	return axis_job_queue_wait_handlers(serving->jobs, timeout_ms);
}

static void
on_lock_regained(
	gpointer     data,
	gint         backend_pid,
	const gchar *backend_start
){
	Serving *serving = data;

	/* The new session is the one the claims and renewals depend on. */
	axis_job_queue_set_lock(serving->jobs, axis_daemon_lock_get_key(serving->daemon_lock), backend_pid,
		backend_start);
	axis_job_queue_resume(serving->jobs);
	g_idle_add(serving_pause_workers, serving);
	axis_web_limits_set_bodies(serving->limits, TRUE);
	g_atomic_int_set(&serving->gate->phase, START_SERVING);
}

static void
on_lock_taken(gpointer data)
{
	g_idle_add(serving_stop_taken, data);
}

/* Whether the daemon serves: what the portal listener asks before it answers
 * anything (api/axis-portal-api.h).  Any thread. */
static gboolean
portal_serving(gpointer data)
{
	StartGate *gate = data;

	return g_atomic_int_get(&gate->phase) == START_SERVING;
}

static const AxisDaemonLockHooks lock_hooks = {
	on_lock_lost,
	on_lock_wait_handlers,
	on_lock_regained,
	on_lock_taken
};

/*
 * Serves until SIGINT or SIGTERM, or until another session takes the
 * database lock this daemon lost (then status 1).  The database is ready
 * (connected, held with @daemon_lock and migrated) before this is called,
 * and @server is already listening behind @gate, which answers only the
 * probes until this opens it, once every route is registered and every
 * module has started.  Sets *@pool_in_use when job threads that may still
 * use the pool were left running at shutdown: the pool (and the lock) must
 * then not be freed.
 */
static int
serve(
	AxisConfig     *config,
	AxisDbPool     *pool,
	AxisDaemonLock *daemon_lock,
	AxisDataDir    *data,
	HtmxServer     *server,
	StartGate      *gate,
	AxisWebMetrics *metrics,
	gboolean       *pool_in_use,
	GError        **error
){
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(AxisSettings) settings = axis_settings_new();	/* before @web_settings goes */
	g_autoptr(AxisWebSettings) web_settings = axis_web_settings_new(settings, pool);
	g_autoptr(AxisHistory) history = axis_history_new(settings);	/* after @jobs */
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(AxisWorkerTokens) worker_tokens = axis_worker_tokens_new();	/* after @auth, @worker_api, @workers_ui */
	g_autoptr(AxisJobRegistry) job_registry = NULL;
	g_autoptr(AxisJobQueue) jobs = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisPortalTokens) portal_tokens = axis_portal_tokens_new();	/* after @auth, @portal_api, the pages */
	g_autoptr(AxisWebAuth) auth = NULL;	/* after @hub and @web are freed */
	g_autoptr(AxisPortalApi) portal_api = NULL;	/* before @auth: it asks it who a request is from */
	g_autoptr(HtmxServer) portal_server = NULL;	/* the portal listener, when one is configured */
	g_autoptr(AxisWorkerApi) worker_api = NULL;	/* after @web */
	g_autoptr(AxisWebWorkers) workers_ui = NULL;	/* after @web */
	g_autoptr(AxisWebHub) hub = NULL;	/* after @jobs_ui, before @auth */
	g_autoptr(AxisWebJobs) jobs_ui = NULL;	/* after @web, before @pool goes */
	g_autoptr(AxisWebStorage) storage_ui = NULL;	/* after @web, before @hub */
	g_autoptr(AxisWebLibrary) library_ui = NULL;	/* after @web */
	g_autoptr(AxisWebProbe) probe_ui = NULL;	/* after @web */
	g_autoptr(AxisWebBrowser) browser_ui = NULL;	/* after @web, before @hub */
	g_autoptr(AxisWebAnalysis) analysis_ui = NULL;	/* after @web */
	g_autoptr(AxisWebSavings) savings_ui = NULL;	/* after @web */
	g_autoptr(AxisWebCompat) compat_ui = NULL;	/* after @web */
	g_autoptr(AxisWebProfiles) profiles_ui = NULL;	/* after @web */
	g_autoptr(AxisWebConversions) conversions_ui = NULL;	/* after @web, @probe_ui and @worker_api stop */
	g_autoptr(AxisConversions) conversions = NULL;
	g_autoptr(AxisWebMeta) meta_ui = NULL;	/* after @web */
	g_autoptr(AxisWebDownloads) downloads_ui = NULL;	/* after @web, before @hub */
	g_autoptr(AxisDownloads) downloads = NULL;
	g_autoptr(AxisWebIndexers) indexers_ui = NULL;	/* after @web */
	g_autoptr(AxisIndexers) indexers = NULL;
	g_autoptr(AxisWebImports) imports_ui = NULL;	/* after @web */
	g_autoptr(AxisImport) import = NULL;	/* after @movies and @tv, which borrow it */
	g_autoptr(AxisWebDiscover) discover_ui = NULL;	/* after @web */
	g_autoptr(AxisDiscover) discover = NULL;
	g_autoptr(AxisWebArr) arr_ui = NULL;	/* after @web */
	g_autoptr(AxisArr) arr = NULL;
	g_autoptr(AxisYoutube) youtube = NULL;	/* before @downloads, whose built-in kind it is */
	g_autoptr(AxisWebYoutube) youtube_ui = NULL;	/* after @web; declared after @youtube, which its
							 * stop calls, so that it is freed first */
	g_autoptr(AxisRequests) requests = NULL;	/* after @web; it keeps its own references */
	g_autoptr(AxisMovies) movies = NULL;	/* after @web */
	g_autoptr(AxisTv) tv = NULL;	/* after @web */
	g_autoptr(AxisWebReady) ready_ui = NULL;	/* after @web */
	g_autoptr(AxisWebSetup) setup_ui = NULL;	/* after @web */
	g_autoptr(AxisWebIdentify) identify_ui = NULL;	/* after @web */
	g_autoptr(AxisIdentify) identify = NULL;	/* before @movies, which it borrows as its owner */
	g_autoptr(AxisWebQuality) quality_ui = NULL;	/* after @web */
	g_autoptr(AxisQualityStore) quality = NULL;
	g_autoptr(AxisMeta) meta = NULL;
	g_autoptr(AxisCompat) compat = NULL;
	g_autoptr(AxisWeb) web = NULL;	/* before @server is freed */
	g_autoptr(AxisEstimator) estimator = NULL;
	g_autoptr(AxisAnalysis) analysis = NULL;
	g_autoptr(AxisProbe) probe = NULL;
	g_autoptr(AxisLibrary) library = NULL;
	g_autoptr(AxisStorage) storage = NULL;
	AxisWebHealth *health = NULL;
	Probes probes;
	Serving serving;
	guint sigint_id;
	guint sigterm_id;

	*pool_in_use = FALSE;
	memset(&serving, 0, sizeof(serving));

	if (!axis_module_registry_register(registry, axis_job_builtin_get_module(), settings, error))
		return 1;
	/* How long the history of movies, series, downloads and imports is kept. */
	if (!axis_module_registry_register(registry, axis_history_get_module(), history, error))
		return 1;
	/* The Settings and Modules pages. */
	if (!axis_module_registry_register(registry, axis_web_settings_get_module(), web_settings,
	                                   error))
		return 1;
	/* The Jobs pages and the hub their live updates go through. */
	hub = axis_web_hub_new(NULL);
	jobs_ui = axis_web_jobs_new(pool, hub);
	if (!axis_module_registry_register(registry, axis_web_jobs_get_module(), jobs_ui, error))
		return 1;
	/* Volumes and roots, their check and their pages. */
	storage = axis_storage_new(pool, settings);
	storage_ui = axis_web_storage_new(storage, hub);
	if (!axis_module_registry_register(registry, axis_web_storage_get_module(), storage_ui, error))
		return 1;
	/* The file index: scans of the roots, and each root's page. */
	library = axis_library_new(pool, storage);
	library_ui = axis_web_library_new(library, storage);
	if (!axis_module_registry_register(registry, axis_web_library_get_module(), library_ui, error))
		return 1;
	/* What is inside the indexed video and audio files (ffprobe). */
	probe = axis_probe_new(pool, storage, settings);
	probe_ui = axis_web_probe_new(probe, storage);
	if (!axis_module_registry_register(registry, axis_web_probe_get_module(), probe_ui, error))
		return 1;
	/* The Library pages: volumes, folders and files, read from the index. */
	browser_ui = axis_web_browser_new(library, probe, storage, hub);
	if (!axis_module_registry_register(registry, axis_web_browser_get_module(), browser_ui, error))
		return 1;
	/* The Analysis pages: reports computed from the index into snapshots. */
	analysis = axis_analysis_new(pool, settings);
	analysis_ui = axis_web_analysis_new(analysis);
	if (!axis_module_registry_register(registry, axis_web_analysis_get_module(), analysis_ui, error))
		return 1;
	/* The Savings pages: what each conversion target would save, the file
	 * page's "If converted" card and the file lists by estimate. */
	estimator = axis_estimator_new(pool, storage, settings);
	savings_ui = axis_web_savings_new(estimator);
	if (!axis_module_registry_register(registry, axis_web_savings_get_module(), savings_ui, error))
		return 1;
	axis_web_probe_set_estimator(probe_ui, estimator);
	axis_web_browser_set_estimator(browser_ui, estimator);
	/* The Compatibility pages: what the household's players can play, read
	 * from the media server; the Savings pages show its verdicts. */
	compat = axis_compat_new(pool, storage, settings, estimator);
	compat_ui = axis_web_compat_new(compat);
	if (!axis_module_registry_register(registry, axis_web_compat_get_module(), compat_ui, error))
		return 1;
	axis_web_savings_set_compat(savings_ui, compat);
	/* The conversion profiles: their pages and the assignment to roots. */
	profiles_ui = axis_web_profiles_new(estimator);
	axis_web_profiles_set_compat(profiles_ui, compat);
	if (!axis_module_registry_register(registry, axis_web_profiles_get_module(), profiles_ui, error))
		return 1;
	/* Remote workers: their tokens (by hash), their page, and (below) the
	 * worker API they call. */
	{
		AxisDbConn *conn = axis_db_pool_acquire(pool, error);
		gboolean loaded;

		if (conn == NULL)
			return 1;
		/* And the request portal's, a class of their own. */
		loaded = axis_worker_tokens_load(worker_tokens, conn, error) &&
			axis_portal_tokens_load(portal_tokens, conn, error);
		axis_db_pool_release(pool, conn);
		if (!loaded)
			return 1;
	}
	workers_ui = axis_web_workers_new(pool, worker_tokens, estimator,
		axis_config_get_worker_lease(config) * 1000 / 4);
	if (!axis_module_registry_register(registry, axis_web_workers_get_module(), workers_ui, error))
		return 1;
	/* Conversions: one file at a time on request, encoded on a worker, checked
	 * by the daemon and put in the original's place; a "Convert this file"
	 * card on the file page. */
	conversions = axis_conversions_new(pool, settings, storage, probe, estimator);
	axis_conversions_set_verdict(conversions, axis_web_conversions_compat_verdict, compat);
	conversions_ui = axis_web_conversions_new(conversions);
	if (!axis_module_registry_register(registry, axis_web_conversions_get_module(), conversions_ui, error))
		return 1;
	axis_web_probe_set_file_card(probe_ui, axis_web_conversions_file_collect, axis_web_conversions_file_render,
		axis_web_conversions_file_free, conversions_ui);
	/* The Workers page says what workers do where the checks run. */
	axis_web_workers_set_conversions(workers_ui, conversions);
	/* Metadata: titles from the providers, kept in the database, and their
	 * artwork in the data directory.  TMDb is the provider of films and
	 * series; TheTVDB, after it, a second source of series' details, asked
	 * only where it is named and offered only with its key. */
	meta = axis_meta_new(pool, settings, data, error);
	if (meta == NULL || !axis_meta_add_provider(meta, axis_meta_tmdb_get_info(), error) ||
	    !axis_meta_add_provider(meta, axis_meta_tvdb_get_info(), error))
		return 1;
	meta_ui = axis_web_meta_new(meta);
	if (!axis_module_registry_register(registry, axis_web_meta_get_module(meta_ui), meta_ui, error))
		return 1;
	/* Downloads: the download clients Axis drives, the record of every
	 * release it sent them, and the job that follows them.  SABnzbd (Usenet),
	 * Deluge and qBittorrent (torrents) are the kinds built in so far; the
	 * pages say so when none is. */
	downloads = axis_downloads_new(pool, storage, settings);
	if (!axis_downloads_add_kind(downloads, axis_download_sabnzbd_kind(), error) ||
	    !axis_downloads_add_kind(downloads, axis_download_deluge_kind(), error) ||
	    !axis_downloads_add_kind(downloads, axis_download_qbittorrent_kind(), error))
		return 1;
	downloads_ui = axis_web_downloads_new(downloads, storage, hub);
	if (!axis_module_registry_register(registry, axis_web_downloads_get_module(), downloads_ui, error))
		return 1;
	/* Imports: a completed download taken into the library, through the
	 * volume guard, by hardlink or verified copy; the first code that writes
	 * into a library (and nothing while storage.read_only is on). */
	import = axis_import_new(pool, settings, storage, downloads, probe);
	imports_ui = axis_web_imports_new(import, settings);
	if (!axis_module_registry_register(registry, axis_web_imports_get_module(), imports_ui, error))
		return 1;
	/* Indexers: where releases are found.  Usenet indexers (Newznab) and
	 * torrent feeds (Torznab) asked directly with the user's own keys, and
	 * Prowlarr as a source of many, and three public torrent sites with a JSON
	 * API of their own (YTS, EZTV, The Pirate Bay) that need no key, and other
	 * public sites through definitions: those Axis ships (TorrentProject,
	 * LimeTorrents, TheRARBG, Torrents-CSV; written to the database when the
	 * module starts, with an administrator's overrides from the data
	 * directory's "definitions") and those an administrator installs; every
	 * enabled one is searched at once, and a release is fetched only from one
	 * that found it. */
	indexers = axis_indexers_new(pool, settings);
	if (!axis_indexers_add_kind(indexers, axis_indexer_newznab_kind(), error) ||
	    !axis_indexers_add_kind(indexers, axis_indexer_torznab_kind(), error) ||
	    !axis_indexers_add_kind(indexers, axis_indexer_yts_kind(), error) ||
	    !axis_indexers_add_kind(indexers, axis_indexer_eztv_kind(), error) ||
	    !axis_indexers_add_kind(indexers, axis_indexer_piratebay_kind(), error) ||
	    !axis_indexers_add_kind(indexers, axis_indexer_cardigann_kind(), error) ||
	    !axis_indexers_add_kind(indexers, axis_indexer_prowlarr_kind(), error))
		return 1;
	indexers_ui = axis_web_indexers_new(indexers);
	{
		g_autofree gchar *overrides = g_build_filename(axis_data_dir_get_path(data), "definitions", NULL);

		axis_web_indexers_set_definitions_dir(indexers_ui, overrides);
	}
	if (!axis_module_registry_register(registry, axis_web_indexers_get_module(), indexers_ui, error))
		return 1;
	/* Quality profiles: which releases of a title are acceptable and which is
	 * best, decided with reasons; the Quality page and its Try a name box.
	 * The modules that will use profiles register what refers to them with
	 * the store, so that a profile in use is not deleted. */
	quality = axis_quality_store_new(pool, settings);
	quality_ui = axis_web_quality_new(quality);
	axis_web_quality_set_compat(quality_ui, compat);
	if (!axis_module_registry_register(registry, axis_web_quality_get_module(), quality_ui, error))
		return 1;
	/* Films: the first media module.  It joins the layers above: a title
	 * from the metadata, releases from the indexers, read by the release
	 * parser and decided by the film's quality profile, the best sent to a
	 * download client once and followed until it has arrived. */
	/* Identification: the films and series already in the library matched
	 * to titles, with evidence and a review by a person.  It only reads the
	 * library; the films and the series (below) are its owners, linking what
	 * it identified. */
	identify = axis_identify_new(pool, settings, storage, meta);
	identify_ui = axis_web_identify_new(identify, quality);
	if (!axis_module_registry_register(registry, axis_web_identify_get_module(), identify_ui, error))
		return 1;
	movies = axis_movies_new(pool, settings, meta, indexers, downloads, storage, quality);
	axis_movies_set_compat(movies, compat);
	axis_movies_set_import(movies, import);
	axis_movies_set_identify(movies, identify);
	if (!axis_module_registry_register(registry, axis_movies_get_module(), movies, error))
		return 1;
	/* Series: the second media module, on the same acquisition layer as the
	 * films, with episodes imported file by file; the owner of what
	 * identification finds in the roots for TV. */
	tv = axis_tv_new(pool, settings, meta, indexers, downloads, storage, quality);
	axis_tv_set_compat(tv, compat);
	axis_tv_set_import(tv, import);
	axis_tv_set_identify(tv, identify);
	if (!axis_module_registry_register(registry, axis_tv_get_module(), tv, error))
		return 1;
	/* The Library's folders of a films or series root show the title each
	 * holds; the pages' work is awaited (axis_web_browser_stop()) before
	 * either goes. */
	axis_web_browser_set_folder_titles(browser_ui, AXIS_MEDIA_MOVIES, AXIS_IDENTIFY_MODULE_ID,
		axis_web_identify_folder_titles, identify);
	axis_web_browser_set_folder_titles(browser_ui, AXIS_MEDIA_TV, AXIS_TV_MODULE_ID, axis_tv_folder_titles, tv);
	/* Requests: a person who is not an administrator asks for a movie or for
	 * seasons of a series, and an administrator decides.  An approval adds
	 * the title through the two modules above, unchanged. */
	requests = axis_requests_new(pool, settings, meta, movies, tv, storage, quality);
	/* Its page "Request portal" makes and revokes the portal's tokens. */
	axis_requests_set_portal(requests, portal_tokens, axis_config_get_portal_port(config));
	if (!axis_module_registry_register(registry, axis_requests_get_module(), requests, error))
		return 1;
	/* Discover: TMDb's lists of movies and series, kept and refreshed by a
	 * job, and recommendations for what the modules above refer to; its cards
	 * show the films' and the series' own state or one-step add. */
	discover = axis_discover_new(pool, settings, meta);
	discover_ui = axis_web_discover_new(discover);
	if (!axis_module_registry_register(registry, axis_web_discover_get_module(), discover_ui, error))
		return 1;
	/* Bring over: a Radarr the household already runs, read through its own
	 * API with GET requests only, into a preview of what Axis would take
	 * over.  It writes nothing to the library. */
	arr = axis_arr_new(pool, meta);
	/* What a run adds its movies through: the movies module's own Add. */
	axis_arr_set_movies(arr, movies);
	axis_arr_set_tv(arr, tv);
	arr_ui = axis_web_arr_new(arr);
	if (!axis_module_registry_register(registry, axis_web_arr_get_module(), arr_ui, error))
		return 1;
	/* YouTube: one video at a time, fetched by yt-dlp (a separate program)
	 * through a built-in download client, into a YouTube library folder.
	 * Off until an administrator switches it on; what yt-dlp is given as
	 * its home, its settings folder (both kept empty) and its cache are in
	 * the data directory, made when first needed. */
	{
		g_autofree gchar *state = g_build_filename(axis_data_dir_get_path(data), "youtube", NULL);

		youtube = axis_youtube_new(pool, settings, storage, downloads, library, state, error);
		if (youtube == NULL)
			return 1;
	}
	youtube_ui = axis_web_youtube_new(youtube, storage);
	if (!axis_module_registry_register(registry, axis_web_youtube_get_module(), youtube_ui, error))
		return 1;
	/* Getting started: what is still needed before anything can be found
	 * and downloaded, on the dashboard until it is all there. */
	ready_ui = axis_web_ready_new(meta, indexers, downloads, storage);
	if (!axis_module_registry_register(registry, axis_web_ready_get_module(), ready_ui, error))
		return 1;
	/* The setup guide: a path through the pages above, step by step, each
	 * showing that page's own form and what its layer's check says. */
	{
		AxisWebSetupLayers layers;

		memset(&layers, 0, sizeof(layers));
		layers.settings = settings;
		layers.meta = meta;
		layers.indexers = indexers;
		layers.downloads = downloads;
		layers.storage = storage;
		layers.quality = quality;
		layers.compat = compat;
		layers.identify = identify;
		setup_ui = axis_web_setup_new(&layers);
		if (!axis_module_registry_register(registry, axis_web_setup_get_module(), setup_ui, error))
			return 1;
	}

	/* What administrators have set and switched, before anything uses it.
	 * Declarations are checked first: a duplicate key stops the daemon. */
	if (!axis_settings_add_modules(settings, registry, error) ||
	    !axis_settings_load(settings, pool, error) ||
	    !axis_module_state_load(registry, pool, error) ||
	    !axis_storage_load(storage, error))
		return 1;
	axis_web_settings_sync(web_settings);

	jobs = jobs_new(config, pool, registry, &job_registry, error);
	if (jobs == NULL)
		return 1;
	/* The pool was sized for job_queues before the database was asked
	 * anything; a module whose queue is not among them is this build's
	 * mistake. */
	{
		guint threads = 0;

		jobs_pool_need(config, &threads);
		if (axis_job_queue_get_total_concurrency(jobs) != threads) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_MODULE,
				"the job queue has %u worker threads, but the pool was sized for %u: the modules' job queues "
				"are not those axisd lists (job_queues in axisd/main.c)",
				axis_job_queue_get_total_concurrency(jobs), threads);
			return 1;
		}
	}
	/* One daemon per database (axis-daemon-lock.h): the workers are named
	 * after this run, each claim and lease renewal needs the lock, and what
	 * earlier runs left running is taken back once whoever held the
	 * database before must have stopped. */
	axis_job_queue_set_instance(jobs, axis_daemon_lock_get_instance(daemon_lock));
	axis_job_queue_set_lease(jobs, AXIS_DAEMON_LOCK_JOB_LEASE_MS);
	axis_job_queue_set_lock(jobs, axis_daemon_lock_get_key(daemon_lock),
		axis_daemon_lock_get_backend_pid(daemon_lock), axis_daemon_lock_get_backend_start(daemon_lock));
	axis_job_queue_set_take_over(jobs, AXIS_DAEMON_LOCK_TAKE_OVER_MS);
	/* Conversion work (encodes, the checks' measurements, sample
	 * measurements) follows one hold, which is on from now until a person
	 * resumes it while convert.wait_after_start is on: nothing heavy starts
	 * by itself after a restart (design.md of verify-on-worker). */
	{
		const gchar *const *held = axis_conversion_held_kinds();
		guint i;

		for (i = 0; held[i] != NULL; i++) {
			if (!axis_job_registry_set_hold(job_registry, held[i], axis_conversions_get_hold(conversions), error))
				return 1;
		}
		axis_conversions_set_on_resume(conversions, on_conversions_resumed, jobs);
		axis_conversions_hold_after_start(conversions);
	}
	/* Jobs are named in plain words, and only work counts as running. */
	axis_web_jobs_set_kinds(jobs_ui, job_registry);
	axis_web_workers_set_kinds(workers_ui, job_registry);
	axis_module_registry_add_listener(registry, on_module_switched, jobs);
	worker_api = axis_worker_api_new(pool, job_registry, worker_tokens, estimator);
	axis_worker_api_set_convert(worker_api, axis_web_conversions_worker_hooks(), conversions);
	axis_worker_api_set_registered(worker_api, on_worker_registered, workers_ui);
	/* Its claims and expiries need the daemon's lock, as the queue's do. */
	axis_worker_api_set_fence(worker_api, jobs);
	{
		AxisWorkerApiTiming timing;

		memset(&timing, 0, sizeof(timing));
		timing.lease_ms = axis_config_get_worker_lease(config) * 1000;
		axis_worker_api_set_timing(worker_api, &timing);
	}

	/* Accounts and sessions are loaded before anything is served.  The
	 * auth's threads hold a pooled connection only while they use it,
	 * one at a time, so they wait for (never deadlock on) connections
	 * the job queue's threads hold. */
	/* With no account yet, the first administrator from the configuration
	 * (if one is given) is created here instead of a setup token; its
	 * password is wiped from memory straight after. */
	auth = axis_web_auth_new_with_admin(pool, axis_config_get_trusted_proxies(config),
		axis_config_get_secure_cookies(config), axis_config_get_admin_username(config),
		axis_config_get_admin_password(config), error);
	axis_config_forget_admin_password(config);
	if (auth == NULL)
		return 1;
	axis_web_auth_set_worker_tokens(auth, worker_tokens);
	/* A browser that logged in stays remembered across restarts (its own
	 * count of failed logins); without the key file, until this one stops. */
	if (!axis_web_auth_load_device_key(auth, axis_data_dir_get_fd(data), &local)) {
		g_warning("remembered browsers are forgotten when the daemon stops: the device key in %s cannot be "
			"kept: %s", axis_data_dir_get_path(data), local->message);
		g_clear_error(&local);
	}

	/* The portal API (add-request-portal): a listener of its own, only when
	 * a port is configured for it (none by default), with a router that holds
	 * its eleven operations and nothing else.  Like the main listener it
	 * answers 503 until the daemon serves, and while it has lost its database
	 * lock.  The main listener answers none of it (web/axis-web-auth.c). */
	if (axis_config_get_portal_port(config) != 0) {
		guint16 portal_port = axis_config_get_portal_port(config);

		if (portal_port == axis_config_get_port(config)) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_LISTEN, "the portal listener (server.portal_listen_port, "
				"AXIS_PORTAL_LISTEN_PORT) needs a port of its own: %u is the main listener's", (guint)portal_port);
			return 1;
		}
		portal_api = axis_portal_api_new(pool, auth, portal_tokens, requests, meta, discover);
		axis_portal_api_set_serving(portal_api, portal_serving, gate);
		portal_server = htmx_server_new();
		axis_portal_api_install(portal_api, portal_server, axis_config_get_max_connections(config));
		/* On an address of its own when one is set (server.portal_listen_host),
		 * so that the portal's network alone reaches it. */
		if (!htmx_server_listen_on(portal_server, axis_config_get_portal_host(config), portal_port, &local) ||
		    !htmx_server_start(portal_server, &local)) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_LISTEN, "cannot listen on %s:%u for the portal API: %s",
				axis_config_get_portal_host(config), (guint)portal_port, local->message);
			return 1;
		}
		g_message("portal API: listening on %s:%u (the portal API and nothing else)",
			axis_config_get_portal_host(config), (guint)portal_port);
	}

	/* The probes share each database question among the requests that
	 * arrive while it is asked (axis-web-health.h). */
	{
		AxisWebHealthHooks hooks;

		memset(&probes, 0, sizeof(probes));
		probes.pool = pool;
		probes.daemon_lock = daemon_lock;
		memset(&hooks, 0, sizeof(hooks));
		hooks.ping = probe_ping;
		hooks.current = probe_current;
		hooks.held = probe_held;
		hooks.data = &probes;
		hooks.metrics = metrics;
		health = axis_web_health_new(&hooks);
	}
	/* The shell: security headers, error pages, assets, login, the
	 * dashboard at "/" and every module's pages.  The probes go through it
	 * too, so a wrong method gets a 405 and its response the security
	 * headers; they are among the few routes that need no session.
	 * /healthz (the database answers) is what `axisd --healthcheck` and the
	 * compose example ask; /livez (the process serves) and /readyz (the
	 * database answers and is at this build's schema) are for Kubernetes. */
	web = axis_web_new(server, registry, auth);
	axis_web_health_register(health, web);
	/* /metrics, only when a metrics token is configured. */
	axis_web_metrics_register(metrics, web, pool);
	axis_web_hub_install(hub, web, auth);
	/* The third class of route: a worker's token, never a session. */
	axis_worker_api_register_routes(worker_api, web);

	if (!axis_module_registry_start_all(registry, error)) {
		htmx_server_stop(server);
		if (portal_server != NULL)
			htmx_server_stop(portal_server);
		axis_web_health_free(health);
		return 1;
	}

	/* After the modules, whose state the handlers may need. */
	if (!axis_job_queue_start(jobs, error)) {
		axis_module_registry_stop_all(registry);
		htmx_server_stop(server);
		if (portal_server != NULL)
			htmx_server_stop(portal_server);
		axis_web_health_drain(health);
		axis_web_auth_stop(auth);
		while (g_main_context_iteration(NULL, FALSE))
			;
		axis_web_health_free(health);
		return 1;
	}

	/* What a run that was killed during a YouTube download left (a video
	 * recorded as downloading, the folder yt-dlp worked in) is put right,
	 * now that the queue has taken that run's jobs back. */
	axis_web_youtube_recover(youtube_ui);

	/* Leases of remote workers that fell silent are handed back. */
	axis_worker_api_start(worker_api);

	/* Job events reach the pages, and cancel and retry reach the queue.
	 * Without it the pages still show jobs; they just do not move. */
	if (!axis_web_jobs_attach(jobs_ui, jobs, &local))
		g_warning("the Jobs pages will not update live: %s", local->message);
	g_clear_error(&local);

	/* What needs a person (the bell in the header, the page "Needs
	 * attention") is read from here on, in the background: a timer on the
	 * main loop and a worker thread, no job kind and no queue.
	 * axis_web_stop() ends it before any module is stopped. */
	axis_web_attention_start(web, pool);
	/* The dashboard's figures likewise: read in the background from here on,
	 * and a page view draws what was last read. */
	axis_web_dashboard_start(web);

	sigint_id = g_unix_signal_add(SIGINT, on_signal, loop);
	sigterm_id = g_unix_signal_add(SIGTERM, on_signal, loop);

	/* Everything is in place: the gate lets requests through from here,
	 * bodies too, and the lock is watched (its watcher may close the gate
	 * again). */
	serving.limits = axis_web_limits_get(server);
	axis_web_limits_set_bodies(serving.limits, TRUE);
	g_atomic_int_set(&gate->phase, START_SERVING);
	serving.daemon_lock = daemon_lock;
	serving.jobs = jobs;
	serving.worker_api = worker_api;
	serving.gate = gate;
	serving.loop = loop;
	axis_daemon_lock_watch(daemon_lock, &lock_hooks, &serving);
	g_message("listening on %s:%u",
		axis_config_get_host(config), (guint)axis_config_get_port(config));
	g_main_loop_run(loop);

	/* Stop taking requests first.  A health check still running holds the
	 * pool: wait for it, then deliver what the finished ones left on the
	 * loop so nothing is leaked.  The lock's watcher stops next (one that
	 * lost the lock first sees its handlers stop, or ends the process);
	 * the lock itself stays held until the job queue has stopped.  Then
	 * the job queue, which gives running jobs their grace period, then the
	 * modules their handlers belong to (the registry stops them in reverse
	 * registration order). */
	htmx_server_stop(server);
	if (portal_server != NULL)
		htmx_server_stop(portal_server);
	axis_daemon_lock_stop_watching(daemon_lock);
	axis_web_health_drain(health);
	axis_web_metrics_drain(metrics);
	/* A dashboard being built reads through the modules' parts. */
	axis_web_stop(web);
	while (g_main_context_iteration(NULL, FALSE))
		;
	/* Worker requests in flight are answered; leases are left to the
	 * database (a worker still running is adopted after a restart). */
	axis_worker_api_stop(worker_api);
	axis_worker_api_set_registered(worker_api, NULL, NULL);
	axis_web_workers_stop(workers_ui);
	/* Portal operations in flight are answered; a login among them ends with
	 * the password work below, so this comes first. */
	if (portal_api != NULL)
		axis_portal_api_stop(portal_api);
	/* Password work in flight is answered (or skipped), and the session
	 * changes still queued are written, while the pool is certainly
	 * there. */
	axis_web_auth_stop(auth);
	axis_web_settings_stop(web_settings);
	axis_web_jobs_stop(jobs_ui);
	/* Storage work in flight is bounded by the storage deadlines. */
	axis_web_storage_stop(storage_ui);
	axis_web_library_stop(library_ui);
	axis_web_probe_stop(probe_ui);
	axis_web_browser_stop(browser_ui);
	axis_web_analysis_stop(analysis_ui);
	axis_web_savings_stop(savings_ui);
	axis_web_compat_stop(compat_ui);
	axis_web_profiles_stop(profiles_ui);
	axis_web_conversions_stop(conversions_ui);
	axis_web_meta_stop(meta_ui);
	axis_web_downloads_stop(downloads_ui);
	axis_web_imports_stop(imports_ui);
	axis_web_indexers_stop(indexers_ui);
	axis_web_quality_stop(quality_ui);
	axis_web_identify_stop(identify_ui);
	axis_web_setup_stop(setup_ui);
	axis_web_discover_stop(discover_ui);
	axis_web_arr_stop(arr_ui);
	/* A look at a YouTube video in flight, and a download that runs, are
	 * cancelled (yt-dlp is killed); the download's video says that Axis
	 * stopped. */
	axis_web_youtube_stop(youtube_ui);
	/* A request to a Radarr in flight is cancelled. */
	axis_arr_stop(arr);
	/* A decision in flight is awaited: it ends with the database's answer. */
	axis_requests_stop(requests);
	axis_movies_stop(movies);
	axis_tv_stop(tv);
	*pool_in_use =!axis_job_queue_stop(jobs, axis_config_get_jobs_grace_period(config) * 1000);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_module_registry_stop_all(registry);
	axis_storage_stop(storage);
	/* What is being fetched is cancelled; its threads are awaited (their
	 * requests have a deadline). */
	axis_meta_stop(meta);
	/* Requests to download clients in flight are cancelled. */
	axis_downloads_stop(downloads);
	/* Requests to indexer sources in flight are cancelled. */
	axis_indexers_stop(indexers);
	if (*pool_in_use) {
		/* A deletion of old history still running uses this. */
		history_left_open = g_steal_pointer(&history);
		/* A conversion's check or placement still running uses these. */
		conversions_ui_left_open = g_steal_pointer(&conversions_ui);
		conversions_left_open = g_steal_pointer(&conversions);
		identify_ui_left_open = g_steal_pointer(&identify_ui);
		identify_left_open = g_steal_pointer(&identify);
		/* A refresh of the Discover lists still running uses these. */
		discover_ui_left_open = g_steal_pointer(&discover_ui);
		discover_left_open = g_steal_pointer(&discover);
		/* A reading of a Radarr still running uses these. */
		arr_ui_left_open = g_steal_pointer(&arr_ui);
		arr_left_open = g_steal_pointer(&arr);
		/* A YouTube fetch still running uses these. */
		youtube_ui_left_open = g_steal_pointer(&youtube_ui);
		youtube_left_open = g_steal_pointer(&youtube);
		movies_left_open = g_steal_pointer(&movies);
		tv_left_open = g_steal_pointer(&tv);
		imports_ui_left_open = g_steal_pointer(&imports_ui);
		import_left_open = g_steal_pointer(&import);
		quality_ui_left_open = g_steal_pointer(&quality_ui);
		quality_left_open = g_steal_pointer(&quality);
		indexers_ui_left_open = g_steal_pointer(&indexers_ui);
		indexers_left_open = g_steal_pointer(&indexers);
		downloads_ui_left_open = g_steal_pointer(&downloads_ui);
		downloads_left_open = g_steal_pointer(&downloads);
		meta_ui_left_open = g_steal_pointer(&meta_ui);
		meta_left_open = g_steal_pointer(&meta);
		/* A storage check, a scan, a probe, a refresh or a measurement still
		 * running uses these. */
		profiles_ui_left_open = g_steal_pointer(&profiles_ui);
		workers_ui_left_open = g_steal_pointer(&workers_ui);
		compat_ui_left_open = g_steal_pointer(&compat_ui);
		compat_left_open = g_steal_pointer(&compat);
		savings_ui_left_open = g_steal_pointer(&savings_ui);
		estimator_left_open = g_steal_pointer(&estimator);
		analysis_ui_left_open = g_steal_pointer(&analysis_ui);
		analysis_left_open = g_steal_pointer(&analysis);
		probe_ui_left_open = g_steal_pointer(&probe_ui);
		probe_left_open = g_steal_pointer(&probe);
		library_ui_left_open = g_steal_pointer(&library_ui);
		library_left_open = g_steal_pointer(&library);
		storage_ui_left_open = g_steal_pointer(&storage_ui);
		storage_left_open = g_steal_pointer(&storage);
	}
	axis_web_health_free(health);

	/* A source that fired has already removed itself. */
	if (g_main_context_find_source_by_id(NULL, sigint_id) != NULL)
		g_source_remove(sigint_id);
	if (g_main_context_find_source_by_id(NULL, sigterm_id) != NULL)
		g_source_remove(sigterm_id);
	/* What the lock's watcher left on the loop refers to @serving. */
	while (g_main_context_iteration(NULL, FALSE))
		;

	if (serving.taken) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_DATABASE_IN_USE,
			"stopped: another session took the database lock while this daemon had lost it (%s); start "
			"this daemon again to wait for that one", axis_daemon_lock_get_taken_by(daemon_lock) != NULL ?
			axis_daemon_lock_get_taken_by(daemon_lock) : "unknown");
		return 1;
	}
	return 0;
}

/*
 * Listen (behind the start-up gate), wait for the database and migrate it,
 * then serve; the server and the pool outlive everything that uses them.
 */
static int
run(
	AxisConfig  *config,
	AxisDataDir *data,
	GError     **error
){
	g_autoptr(HtmxServer) server = htmx_server_new();
	g_autoptr(AxisDbPool) pool = NULL;
	g_autoptr(AxisWebMetrics) metrics = NULL;	/* before @server goes */
	g_autoptr(GError) local = NULL;
	AxisDaemonLock *daemon_lock = NULL;
	StartGate gate;
	gboolean pool_in_use = FALSE;
	gboolean stopped = FALSE;
	int status;

	/* The gate is the outermost middleware: everything else the shell
	 * installs later runs inside it.  Then the request timing for the
	 * metrics, when they are on, so that it sees every answer the shell
	 * and the login checks give. */
	g_atomic_int_set(&gate.phase, START_WAITING);
	htmx_server_use(server, start_gate, &gate, NULL);
	metrics = axis_web_metrics_new(axis_config_get_metrics_token(config));
	axis_web_metrics_install_timing(metrics, server);
	/* What a request and a client may cost holds from the first
	 * connection, whatever the daemon is waiting for; no body is taken
	 * until it serves (axis-web-limits.h). */
	{
		AxisWebLimitsOptions options;
		AxisWebLimits *limits;

		memset(&options, 0, sizeof(options));
		options.head_timeout_ms = axis_config_get_head_timeout(config) * 1000;
		options.body_timeout_ms = axis_config_get_body_timeout(config) * 1000;
		options.idle_timeout_ms = axis_config_get_idle_timeout(config) * 1000;
		options.max_connections = axis_config_get_max_connections(config);
		options.max_per_address = axis_config_get_max_connections_per_address(config);
		options.trusted_proxies = axis_config_get_trusted_proxies(config);
		limits = axis_web_limits_install(server, &options);
		axis_web_limits_set_bodies(limits, FALSE);
		axis_web_limits_set_refusal(limits, gate_refusal_words, &gate);
	}
	if (!htmx_server_listen_on(server, axis_config_get_host(config),
	                           axis_config_get_port(config), &local)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_LISTEN,
			"cannot listen on %s:%u: %s",
			axis_config_get_host(config), (guint)axis_config_get_port(config),
			local->message);
		return 1;
	}
	if (!htmx_server_start(server, &local)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_LISTEN,
			"cannot start server on %s:%u: %s",
			axis_config_get_host(config), (guint)axis_config_get_port(config),
			local->message);
		return 1;
	}
	g_message("starting: until the database is ready, %s:%u answers only /livez (alive), "
		"/readyz and /healthz (not ready)",
		axis_config_get_host(config), (guint)axis_config_get_port(config));

	pool = wait_for_database(config, &gate, &daemon_lock, &stopped, error);
	if (pool == NULL) {
		htmx_server_stop(server);
		return stopped ? 0 : 1;
	}

	status = serve(config, pool, daemon_lock, data, server, &gate, metrics, &pool_in_use, error);
	/* Whatever serve() left: stopped already on its way out. */
	if (status != 0)
		htmx_server_stop(server);

	/* Job threads still running may use it until the process exits, so it
	 * is deliberately not freed; nor is the lock, which the end of the
	 * process releases, so that no daemon takes over beside them.
	 * Otherwise the lock goes (first: it uses the pool's settings). */
	if (pool_in_use) {
		g_warning("exiting with job threads still running; the database pool and lock are left open");
		lock_left_open = daemon_lock;
		pool_left_open = g_steal_pointer(&pool);
	} else {
		axis_daemon_lock_free(daemon_lock);
	}

	return status;
}

/*
 * The address a health check connects to for a daemon listening on @host:
 * the wildcard addresses mean "every interface", so the loopback one does.
 */
static const gchar *
healthcheck_host(const gchar *host)
{
	if (strcmp(host, "0.0.0.0") == 0)
		return "127.0.0.1";
	if (strcmp(host, "::") == 0)
		return "::1";

	return host;
}

/*
 * --healthcheck: asks the daemon the configuration describes for /healthz
 * and reports the answer as the exit status (0 only for a 200).  It opens
 * no database connection and starts no job; the daemon it asks does the
 * database ping.  Gives up after HEALTHCHECK_TIMEOUT_SECONDS, whatever
 * the network does.
 */
static int
healthcheck(AxisConfig *config)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GUri) uri = NULL;
	guint status;

	/* Backstop: SIGALRM ends the process, with a non-zero status. */
	alarm(HEALTHCHECK_TIMEOUT_SECONDS + 1);

	uri = g_uri_build(G_URI_FLAGS_NONE, "http", NULL,
		healthcheck_host(axis_config_get_host(config)),
		(gint)axis_config_get_port(config), "/healthz", NULL, NULL);
	message = soup_message_new_from_uri(SOUP_METHOD_GET, uri);
	/* No proxy: this asks the daemon on this machine. */
	session = soup_session_new_with_options("timeout", HEALTHCHECK_TIMEOUT_SECONDS,
		"proxy-resolver", (GProxyResolver *)NULL, NULL);

	body = soup_session_send_and_read(session, message, NULL, &error);
	if (body == NULL) {
		g_printerr("axisd: health check failed: %s\n", error->message);
		return 1;
	}

	status = soup_message_get_status(message);
	if (status != SOUP_STATUS_OK) {
		g_printerr("axisd: health check failed: the daemon answered %u\n", status);
		return 1;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	g_autoptr(GOptionContext) context = g_option_context_new(NULL);
	g_autoptr(AxisConfig) config = axis_config_new();
	g_autoptr(AxisDataDir) data = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) envp = NULL;
	g_autofree gchar *config_path = NULL;
	g_autofree gchar *description = NULL;
	gboolean show_version = FALSE;
	gboolean check_health = FALSE;
	g_autofree gchar *check_definition = NULL;
	g_autofree gchar *check_page = NULL;
	g_autofree gchar *check_release_page = NULL;
	g_autofree gchar *check_words = NULL;
	g_autofree gchar *check_address = NULL;
	g_autofree gchar *check_save = NULL;
	gboolean check_excerpts = FALSE;
	int status;
	GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path,
		  "Read settings from FILE", "FILE" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &show_version,
		  "Print the version and exit", NULL },
		{ "healthcheck", 0, 0, G_OPTION_ARG_NONE, &check_health,
		  "Ask the running daemon whether it is healthy; exit 0 if it is", NULL },
		{ "check-definition", 0, 0, G_OPTION_ARG_STRING, &check_definition,
		  "Run an indexer definition (a FILE, or a built-in one's id) step by step and print what it read; no "
		  "daemon or database is needed", "FILE" },
		{ "page", 0, 0, G_OPTION_ARG_FILENAME, &check_page,
		  "With --check-definition: read this saved answer instead of asking the site", "FILE" },
		{ "release-page", 0, 0, G_OPTION_ARG_FILENAME, &check_release_page,
		  "With --check-definition: read a release's download from this saved page", "FILE" },
		{ "words", 0, 0, G_OPTION_ARG_STRING, &check_words,
		  "With --check-definition: the words to search for (none: the definition's test)", "WORDS" },
		{ "address", 0, 0, G_OPTION_ARG_STRING, &check_address,
		  "With --check-definition: the site's address (none: the definition's first)", "URL" },
		{ "excerpts", 0, 0, G_OPTION_ARG_NONE, &check_excerpts,
		  "With --check-definition: print an excerpt of each row read", NULL },
		{ "save", 0, 0, G_OPTION_ARG_FILENAME, &check_save,
		  "With --check-definition: write the site's answers into DIR, to make a fixture of", "DIR" },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
	};

	g_option_context_set_summary(context, "Axis media engine daemon");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("axisd: %s\n", error->message);
		return 1;
	}

	if (show_version) {
		g_print("axisd %s (%s)\n", AXIS_VERSION, AXIS_GIT_SHA);
		return 0;
	}

	/* defaults -> file -> environment */
	envp = g_get_environ();
	if (check_health) {
		/* Only the address matters: no secret (the database URL, the first
		 * administrator's password, the metrics token, or the files that
		 * may hold them) is read or required. */
		g_auto(GStrv) public_env = axis_config_environ_without_secrets(envp);

		if ((config_path != NULL && !axis_config_load_file(config, config_path, &error)) ||
		    !axis_config_apply_environment(config, public_env, &error)) {
			g_printerr("axisd: %s\n", error->message);
			return 1;
		}
		return healthcheck(config);
	}
	if (check_definition != NULL) {
		/* A developer's or an administrator's check, not the daemon: only
		 * the outbound binding is taken from the settings (no secret is read
		 * or needed), so a live check goes out the way the daemon's searches
		 * do. */
		g_auto(GStrv) public_env = axis_config_environ_without_secrets(envp);
		g_autoptr(GString) out = g_string_new(NULL);
		AxisDefCheckOptions check;
		AxisOutboundTarget target;

		if ((config_path != NULL && !axis_config_load_file(config, config_path, &error)) ||
		    !axis_config_apply_environment(config, public_env, &error)) {
			g_printerr("axisd: %s\n", error->message);
			return 2;
		}
		if (check_page == NULL && axis_outbound_target_parse(axis_config_get_outbound_bind(config), &target, NULL))
			axis_outbound_configure(&target, axis_config_get_outbound_scope(config));
		memset(&check, 0, sizeof(check));
		check.definition = check_definition;
		check.page = check_page;
		check.release_page = check_release_page;
		check.words = check_words;
		check.address = check_address;
		check.excerpts = check_excerpts;
		check.save_dir = check_save;
		status = axis_indexer_def_check_run(&check, out);
		g_print("%s", out->str);
		return status;
	}

	if ((config_path != NULL && !axis_config_load_file(config, config_path, &error)) ||
	    !axis_config_apply_environment(config, envp, &error) ||
	    !axis_config_check_required(config, &error)) {
		g_printerr("axisd: %s\n", error->message);
		return 1;
	}

	axis_log_init(axis_config_get_log_level(config));
	axis_log_set_format(axis_config_get_log_format(config));
	/* The line shows the pool size in use; a size that cannot be used is refused later, with its reason. */
	{
		guint used = 0;

		if (pool_size_for(config, &used, NULL))
			axis_config_set_database_pool_size_used(config, used);
	}
	description = axis_config_describe(config);
	g_info("axisd %s (%s) starting: %s", AXIS_VERSION, AXIS_GIT_SHA, description);

	/* The interface outbound connections are tied to: its state is logged
	 * now and whenever it changes.  Start-up does not wait for it (a VPN
	 * may come up later); until it is up, connections it must carry are
	 * refused. */
	{
		AxisOutboundTarget target;

		/* Validated when the configuration was read. */
		if (axis_outbound_target_parse(axis_config_get_outbound_bind(config), &target, NULL))
			axis_outbound_configure(&target, axis_config_get_outbound_scope(config));
		axis_outbound_watch_start();
	}

	/* The daemon's own files: it does not start without a place for them,
	 * and the database is not touched until it has one. */
	data = axis_data_dir_open(axis_config_get_data_dir(config), &error);
	if (data == NULL) {
		g_critical("%s", error->message);
		return 1;
	}

	status = run(config, data, &error);
	axis_outbound_watch_stop();
	if (status != 0) {
		g_critical("%s", error->message);
		return status;
	}

	g_info("stopped");
	return 0;
}
