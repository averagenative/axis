/*
 * axis-test-worker.h - test support for the worker API: the daemon's side
 * served on the test's own main loop, requests to it, and real axis-worker
 * processes pointed at it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every axis-worker a test starts is told to run software encoders only
 * (encoder_families = software): nothing here touches a GPU.  The
 * capability measurement is kept in one state directory for the whole test
 * program, so ffmpeg's encoders are measured once per program.
 */

#ifndef AXIS_TEST_WORKER_H
#define AXIS_TEST_WORKER_H

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>

#include "axis-test-web.h"
#include "api/axis-worker-api.h"
#include "api/axis-worker-tokens.h"
#include "core/axis-module.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-workers.h"

G_BEGIN_DECLS

#ifndef AXIS_WORKER_PATH
#error "AXIS_WORKER_PATH must be defined by the build"
#endif

typedef struct
{
	AxisDbPool        *pool;		/* borrowed */
	AxisJobRegistry   *registry;
	AxisWorkerTokens  *tokens;
	AxisModuleRegistry *modules;
	AxisWebWorkers    *workers_ui;
	AxisWebAuth       *auth;
	HtmxServer        *server;
	AxisWeb           *web;
	AxisWorkerApi     *api;
	AxisWebHub        *hub;		/* NULL unless made with a hub */
	SoupSession       *session;
	guint16            port;
	gchar             *url;		/* http://127.0.0.1:PORT */
} AxisTestDaemon;

/**
 * axis_test_daemon_new:
 * @pool: the database, migrated
 * @estimator: (nullable): for sample measurements
 * @timing: (nullable): the API's timing (default: a 2 s lease)
 * @trusted_proxies: (nullable): for the client address
 *
 * The worker API and the Workers page on 127.0.0.1, on the default main
 * context.  The auth has no database: sessions are test sessions.
 */
AxisTestDaemon *
axis_test_daemon_new(
	AxisDbPool                *pool,
	AxisEstimator             *estimator,
	const AxisWorkerApiTiming *timing,
	const gchar               *trusted_proxies
);

/* The same, with the hub of the pages' live updates (/events) installed. */
AxisTestDaemon *
axis_test_daemon_new_with_hub(
	AxisDbPool                *pool,
	AxisEstimator             *estimator,
	const AxisWorkerApiTiming *timing,
	const gchar               *trusted_proxies
);

void
axis_test_daemon_free(AxisTestDaemon *self);

/* Adds a worker; returns its token. */
gchar *
axis_test_daemon_add_worker(
	AxisTestDaemon *self,
	const gchar    *name,
	gint64         *id
);

/**
 * axis_test_daemon_post:
 * @request: a request's name ("claim") or a path starting with '/'
 * @token: (nullable): sent as a bearer token
 * @content_type: (nullable): default application/json
 * @body: (nullable): the body
 * @extra: (nullable): header name and value pairs, ended by %NULL
 */
AxisTestReply
axis_test_daemon_post(
	AxisTestDaemon     *self,
	const gchar        *method,
	const gchar        *request,
	const gchar        *token,
	const gchar        *content_type,
	const gchar        *body,
	const gchar *const *extra
);

/* A register request for a worker with software encoders (libx265 and
 * libx264 usable unless @no_x265) and @volumes (UUID, path pairs, ended by
 * %NULL). */
gchar *
axis_test_register_json(
	gboolean            no_x265,
	const gchar *const *volumes
);

/* A claim request for both remote kinds and @volumes (UUIDs). */
gchar *
axis_test_claim_json(const gchar *const *volumes);

/* ── axis-worker processes ──────────────────────────────────────── */

typedef struct _AxisTestWorker AxisTestWorker;

/**
 * AxisTestWorkerOptions:
 * @url: the daemon
 * @token: the token (written to a file the worker reads)
 * @volumes: (nullable): its volume paths, ended by %NULL
 * @ffmpeg: (nullable): ffmpeg's command as the configuration file takes
 *   it (';'-separated), default the real one
 * @env: (nullable): name and value pairs for its environment
 * @volume_check: seconds between proofs (0: 0.2)
 * @extra_config: (nullable): lines added to [worker]
 * @scratch: (nullable): its scratch directory (kept); default a new one,
 *   removed with it
 * @args: (nullable): arguments after `--config FILE`, ended by %NULL
 */
typedef struct
{
	const gchar        *url;
	const gchar        *token;
	const gchar *const *volumes;
	const gchar        *ffmpeg;
	const gchar *const *env;
	gdouble             volume_check;
	const gchar        *extra_config;
	const gchar        *scratch;
	const gchar *const *args;
} AxisTestWorkerOptions;

AxisTestWorker *
axis_test_worker_start(const AxisTestWorkerOptions *options);

/* Any program (axisd), handled as a worker is: no AXIS_* variable of the
 * test's environment, then @env's (name and value pairs, ended by %NULL);
 * its standard error is its log. */
AxisTestWorker *
axis_test_process_spawn(
	const gchar *const *argv,
	const gchar *const *env
);

/* Pumps the default main context until @needle is in its log. */
gboolean
axis_test_worker_wait_log(
	AxisTestWorker *self,
	const gchar    *needle,
	guint           timeout_ms
);

/* Pumps until it has exited; FALSE if it did not in time. */
gboolean
axis_test_worker_wait_exit(
	AxisTestWorker *self,
	guint           timeout_ms
);

void
axis_test_worker_signal(
	AxisTestWorker *self,
	gint            signal_number
);

GPid
axis_test_worker_get_pid(AxisTestWorker *self);

/* Its exit status (after it exited normally), or -1. */
gint
axis_test_worker_get_status(AxisTestWorker *self);

/* What it wrote on standard error so far. */
const gchar *
axis_test_worker_get_log(AxisTestWorker *self);

const gchar *
axis_test_worker_get_scratch(AxisTestWorker *self);

/* Its configuration file. */
const gchar *
axis_test_worker_get_config(AxisTestWorker *self);

/* Whether its scratch directory holds nothing. */
gboolean
axis_test_worker_scratch_empty(AxisTestWorker *self);

/* Kills it if it still runs, then frees. */
void
axis_test_worker_free(AxisTestWorker *self);

/* The state directory every worker of this program shares; removed by
 * axis_test_worker_cleanup(). */
const gchar *
axis_test_worker_state_dir(void);

void
axis_test_worker_cleanup(void);

/* Whether the process @pid is alive (a zombie counts as gone). */
gboolean
axis_test_process_alive(GPid pid);

/* The pid written in @path, or 0. */
GPid
axis_test_read_pid(const gchar *path);

/* A child of @parent whose command is @comm, alive, or 0. */
GPid
axis_test_find_child(
	GPid         parent,
	const gchar *comm
);

/**
 * axis_test_worker_seed_caps:
 * @no_x265: libx265 is not usable
 *
 * Writes, in the shared state directory, the capability measurement a
 * software-only worker keeps for this machine's ffmpeg (identified by
 * `ffmpeg -version`), as if it had measured: libx264 usable (8 bits) and
 * libx265 usable with 10 bits (unless @no_x265).  A worker started
 * afterwards measures nothing.
 * Returns FALSE when ffmpeg is not installed.
 */
gboolean
axis_test_worker_seed_caps(gboolean no_x265);

/**
 * axis_test_slow_ffmpeg:
 * @dir: where to write it
 * @flag: a path: while a file is there, the fake stands still
 *
 * An ffmpeg for a worker's configuration (';'-separated, as
 * AxisTestWorkerOptions.ffmpeg takes it): `-version` is the real one's;
 * anything else is the real one, unless @flag exists, when it becomes
 * `sleep 600` (a process called "sleep", a child of the worker) and makes
 * nothing.
 */
gchar *
axis_test_slow_ffmpeg(
	const gchar *dir,
	const gchar *flag
);

/**
 * axis_test_failing_ffmpeg:
 * @dir: where to write it
 * @flag: a path: while a file is there, the fake fails as the file says:
 *   "fail" exits 1 with an error (input it cannot read), a signal's name
 *   ("SEGV", "ABRT", "XFSZ") makes it die of that signal
 *
 * An ffmpeg for a worker's configuration, as axis_test_slow_ffmpeg() gives
 * one; `-version` is the real one's, and without the file it is the real
 * one.
 */
gchar *
axis_test_failing_ffmpeg(
	const gchar *dir,
	const gchar *flag
);

/**
 * axis_test_paced_ffmpeg:
 * @dir: where to write it
 * @slow: a path: while a file is there, the real ffmpeg is run reading its
 *   input at a fiftieth of its rate (`-readrate 0.02`): it converts, and
 *   writes its output a little at a time, for minutes
 * @hold: a path: while a file is there, the fake waits (a shell, a child of
 *   the worker called "sh") before it runs ffmpeg as above
 *
 * An ffmpeg for a worker's configuration, as axis_test_slow_ffmpeg() gives
 * one; `-version` is the real one's.  Once ffmpeg runs it is the worker's
 * child "ffmpeg" (the shell execs it).
 */
gchar *
axis_test_paced_ffmpeg(
	const gchar *dir,
	const gchar *slow,
	const gchar *hold
);

G_END_DECLS

#endif /* AXIS_TEST_WORKER_H */
