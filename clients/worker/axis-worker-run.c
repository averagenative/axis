/*
 * axis-worker-run.c - axis-worker's main loop: register, claim, heartbeat,
 * report; the lease deadline and fencing; the volume checks; a clean stop
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The lease deadline.  Each request that grants or renews a lease (a claim
 * that returns a job, a heartbeat answered "continue") is timed from the
 * moment before it was sent, on this machine's monotonic clock: if the
 * daemon grants L milliseconds, the worker's deadline is
 *
 *     sent + L - margin,   margin = max(L / 4, 250 ms)
 *
 * The daemon counts its lease from when it handled the request, which is
 * after it was sent, so by its clocks the lease ends no earlier than
 * sent + L: the worker's deadline is always the earlier by at least the
 * margin, which covers the two clocks running at slightly different rates
 * and the time it takes here to kill the encoder.  A heartbeat goes every
 * heartbeat_ms (at most L / 3, so two can be lost before the deadline).
 * When the deadline passes before a heartbeat was answered "continue", the
 * job is fenced: its encoder's process group is killed (the job's
 * cancellable; the probe runner reaps the group), what it made is dropped
 * with its scratch directory (and a conversion's output in the work area,
 * even one that was finished but not reported yet), and nothing more about
 * the job is ever said to the daemon.  The same happens when the daemon answers "stop" or 409,
 * and when the worker finds, after being stopped (SIGSTOP) or blocked, that
 * the deadline has passed: every report is preceded by a look at the clock.
 *
 * A job whose thread does not end.  Killing the encoder ends a job's thread
 * at once, unless the thread is blocked in the kernel on a volume that
 * stopped answering (a network mount whose server went down), which nothing
 * here can interrupt.  STUCK_MS after its encoder was told to stop, such a
 * job is given up: its lease too (a failure is reported, unless it is fenced
 * already, so that it is tried again), it holds no place among the jobs any
 * more, and the volumes it reads are offered no more work here until a check
 * proves them again.  Its memory stays until the thread really returns
 * (on_thread_done()), since the thread and its completion still point at it,
 * and nothing the thread does after the give-up is acted upon: no result is
 * reported, no measurement kept, no pause taken, and a conversion's output
 * it left in the work area is removed then (or, if the volume does not let
 * it be, left to the daemon's tidy job, which lists it as a leftover).  At
 * most max_jobs threads may be stuck at once: one more, and the worker stops
 * as on SIGTERM (giving its other work back) and exits 1, saying why, so that
 * it is started again rather than gather threads that may never end.
 *
 * A volume that hangs.  A mount whose server is gone can block stat() for
 * good, so no volume is touched on the main loop or on the start-up thread.
 * Each configured volume (and each volume a running job reads) is proved or
 * checked by a thread of its own (a Probe), so one that hangs holds back only
 * itself.  The first registration waits for the first proofs at most
 * START_WAIT_MS and then goes without the volumes that have not answered
 * (they are not proven; they are registered when proved).  A check that
 * takes longer than the check limit drops the volume's proof, stops the
 * sample and verify jobs that read it and is left to end: no second check
 * starts for that path until its thread returns, so threads never pile up.
 *
 * A clean stop (SIGTERM, SIGINT): no more claims; each running job's
 * encoder is killed, and the job is given back (fail with "stopping"), then
 * the worker exits 0 with its scratch directory empty.
 *
 * A crashed encoder (it died of a signal nobody here sent it, as opposed to
 * an exit with a bad status, which is its verdict on its input) pauses the
 * worker: the job is reported failed as any other, and from the moment the
 * job's thread is done this process takes no work.  It keeps asking (at the
 * poll interval) with the crash in its claim, so that the daemon holds it
 * paused and the Workers page says why; a job a claim brings anyway is given
 * back at once.  It takes work again when a claim's answer says the daemon
 * no longer holds it paused (a person resumed it), or when it is restarted:
 * the pause lives in this process.
 */

#include "axis-worker-private.h"

#include <glib-unix.h>
#include <signal.h>
#include <string.h>

#include "axis-version.h"
#include "probe/axis-probe-run.h"
#include "transcode/axis-convert-caps.h"

#define MIN_MARGIN_US       (G_GINT64_CONSTANT(250) * 1000)
#define RETRY_MIN_MS        500
#define RETRY_MAX_MS        30000
#define CHECK_TIMEOUT_MIN_MS 5000
#define START_WAIT_MS       2000
#define STUCK_MS            15000
#define STOP_MAX_MS         (60 * 1000)
#define DIGEST_INTERVAL_US  (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

static const gchar *const kinds[] = {
	AXIS_WORKER_KIND_CAPABILITIES, AXIS_WORKER_KIND_SAMPLE, AXIS_WORKER_KIND_CONVERT, AXIS_WORKER_KIND_VERIFY, NULL
};

typedef struct
{
	AxisWorkerConfig        *config;
	AxisWorkerClient        *client;
	GMainLoop               *loop;
	AxisEncoderCapabilities *caps;
	GPtrArray               *proofs;	/* AxisVolumeProof, accepted by the daemon */
	GPtrArray               *pending_proofs;	/* proved, not yet registered */
	GHashTable              *refused;	/* path -> UUID the daemon did not accept there */
	AxisWorkerRegistered    *registered;
	GPtrArray               *jobs;		/* Job */
	GPtrArray               *abandoned;	/* Job: given up, their threads not returned yet */
	guint                    retry_ms;
	guint                    claim_id;
	guint                    check_id;
	guint                    stop_id;
	gboolean                 claiming;
	gboolean                 registering;
	GHashTable              *probes;	/* path -> Probe: its check or proof is running */
	GPtrArray               *later;		/* proved while a registration was in flight */
	guint                    start_id;	/* the end of the wait for the first proofs */
	gboolean                 registered_once;	/* the first registration was begun */
	gboolean                 register_again;	/* proofs changed while one was in flight */
	gint64                   digest_checked_us;
	gboolean                 stopping;
	gboolean                 fatal;
	guint                    in_flight;	/* requests not answered yet */
	gint                     status;
	gint64                   paused_job;	/* the job whose encoder crashed here, or 0 */
	gchar                   *paused_reason;
	gchar                   *held_paused;	/* the daemon's last word on it, or NULL */
} Worker;

/* What the main loop keeps of a job: in w->jobs while it holds a place, in
 * w->abandoned once it was given up and until its thread returns.  It is
 * freed only once its thread has returned and no request about it is in
 * flight. */
typedef struct
{
	Worker           *worker;
	AxisWorkerRunJob *run;
	gint64            deadline_us;
	guint             beat_id;
	guint             deadline_id;
	gboolean          fenced;
	gboolean          thread_done;	/* its thread returned */
	gboolean          abandoned;	/* given up: what its thread does is not acted upon */
	gboolean          beating;
	gboolean          reporting;
	gboolean          stop_sent;
	gboolean          delivered;	/* its report was taken, or may have been */
	gint64            cancelled_us;	/* when its encoder was told to stop */
	gint64            abandoned_us;	/* when it was given up */
	guint             report_retry_id;	/* its report is sent again then */
	guint             report_tries;	/* times it was sent */
	gboolean          report_lost;	/* an answer to its report was lost */
	guint             beat_retry_id;	/* a heartbeat sent again sooner than the next */
	guint             beat_tries;	/* heartbeats not renewed in a row */
} Job;

/* A report that was not answered, or answered "try again" (429, a 5xx, or
 * anything that is not the worker API's own refusal: a proxy's, or the
 * daemon's connection limits'), is sent again after this, doubling up to
 * the cap, until the lease's deadline. */
#define REPORT_RETRY_FIRST_MS 250
#define REPORT_RETRY_MAX_MS   5000

static void maybe_claim(Worker *w);
static void start_register(Worker *w);
static void maybe_quit(Worker *w);
static void stop_running(Worker *w);

/* ── Small things ───────────────────────────────────────────────── */

static void
mount_free(gpointer data)
{
	AxisWorkerMount *mount = data;

	g_free(mount->uuid);
	g_free(mount->path);
	g_free(mount);
}

static gint64
margin_us(guint lease_ms)
{
	return MAX((gint64)lease_ms * 1000 / 4, MIN_MARGIN_US);
}

/* How long a request may take: its own limit, and never past @deadline. */
static guint
request_timeout(
	Worker *w,
	gint64  deadline_us
){
	gint64 left;

	if (deadline_us <= 0)
		return w->config->request_timeout_ms;
	left = (deadline_us - g_get_monotonic_time()) / 1000;
	return (guint)CLAMP(left, 100, (gint64)w->config->request_timeout_ms);
}

static void
next_retry(Worker *w)
{
	w->retry_ms = w->retry_ms == 0 ? RETRY_MIN_MS : MIN(w->retry_ms * 2, (guint)RETRY_MAX_MS);
}

/* The error answer's word, or NULL.  An answer of an older protocol (a
 * daemon older than this worker, which refuses everything it says) makes the
 * message say so. */
static gchar *
error_word(
	GBytes *body,
	gchar **message
){
	gchar *word = NULL;
	gsize length = 0;
	guint version = 0;
	const gchar *data = body != NULL ? g_bytes_get_data(body, &length) : NULL;

	*message = NULL;
	if (data == NULL || !axis_worker_error_parse(data, length, &word, message, &version, NULL))
		return NULL;
	if (version != AXIS_WORKER_PROTOCOL) {
		gchar *said = *message;

		*message = g_strdup_printf("the daemon speaks worker protocol %u and this axis-worker %u: it gives this "
			"worker no work; update the daemon, or run the axis-worker of its version (it said: %s)", version,
			AXIS_WORKER_PROTOCOL, said != NULL ? said : "");
		g_free(said);
	}
	return word;
}

static void
became_fatal(
	Worker      *w,
	const gchar *why
){
	if (w->fatal)
		return;
	w->fatal = TRUE;
	w->status = 1;
	g_critical("%s; stopping", why);
	w->stopping = TRUE;
}

/* ── Jobs ───────────────────────────────────────────────────────── */

static void
job_remove_timers(Job *job)
{
	if (job->beat_id != 0) {
		g_source_remove(job->beat_id);
		job->beat_id = 0;
	}
	if (job->deadline_id != 0) {
		g_source_remove(job->deadline_id);
		job->deadline_id = 0;
	}
	if (job->report_retry_id != 0) {
		g_source_remove(job->report_retry_id);
		job->report_retry_id = 0;
	}
	if (job->beat_retry_id != 0) {
		g_source_remove(job->beat_retry_id);
		job->beat_retry_id = 0;
	}
}

static void
drop_output_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	axis_worker_work_drop_output(task_data);
	g_task_return_boolean(task, TRUE);
}

static void
on_output_dropped(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Worker *w = data;

	w->in_flight--;
	maybe_quit(w);
}

/* Whether the main loop waits for the job's thread no more: it returned,
 * or the job was given up. */
static gboolean
thread_over(const Job *job)
{
	return job->thread_done || job->abandoned;
}

/* The job's thread has returned and the job is over: its directory goes,
 * and it is forgotten.  A conversion's output the daemon did not take (its
 * result was never reported, because the lease ran out first or the job was
 * given up, or the daemon refused it) is removed, on a thread (the work area
 * is on the volume). */
static void
job_free(Job *job)
{
	Worker *w = job->worker;
	gboolean kept;

	axis_worker_work_remove_dir(job->run->scratch);
	g_mutex_lock(&job->run->lock);
	kept = job->run->convert_result != NULL && job->run->output_area >= 0;
	g_mutex_unlock(&job->run->lock);
	if (kept && (job->abandoned || !job->delivered)) {
		GTask *task = g_task_new(NULL, NULL, on_output_dropped, w);

		g_task_set_task_data(task, axis_worker_run_job_ref(job->run), (GDestroyNotify)axis_worker_run_job_unref);
		w->in_flight++;
		g_task_run_in_thread(task, drop_output_thread);
		g_object_unref(task);
	}
	axis_worker_run_job_unref(job->run);
	g_free(job);
}

/* At most as many job threads may be stuck as the worker runs jobs at once:
 * each one is blocked in the kernel, ends only when its volume answers, and
 * may never.  One more, and the worker stops and exits 1, saying why, so
 * that it is started again (a container's restart policy does it). */
static void
too_many_stuck(Worker *w)
{
	g_autoptr(GString) ids = NULL;
	g_autofree gchar *why = NULL;
	guint i;

	if (w->abandoned->len <= w->config->max_jobs || w->fatal)
		return;
	ids = g_string_new(NULL);
	for (i = 0; i < w->abandoned->len; i++) {
		Job *job = g_ptr_array_index(w->abandoned, i);

		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? ", " : "", job->run->job->id);
	}
	why = g_strdup_printf("the threads of %u jobs (%s) do not end, more than this worker runs at once (max_jobs = %u): "
		"a thread blocked on a volume that does not answer ends only when the volume answers again, so check the "
		"mounts; this worker gives its other work back and exits, to be started again", w->abandoned->len, ids->str,
		w->config->max_jobs);
	became_fatal(w, why);
	stop_running(w);
}

/* The job is over for the daemon (fenced, or its end reported) and no
 * request about it is in flight: it gives its place up.  A job whose thread
 * has not returned (it was given up) is kept, out of the running jobs, until
 * the thread returns (on_thread_done()): the thread and its completion still
 * point at it. */
static void
job_drop(Job *job)
{
	Worker *w = job->worker;

	job_remove_timers(job);
	g_cancellable_cancel(job->run->cancel);
	g_ptr_array_remove(w->jobs, job);
	if (job->thread_done) {
		job_free(job);
	} else {
		job->abandoned = TRUE;
		g_ptr_array_add(w->abandoned, job);
		too_many_stuck(w);
	}
	maybe_quit(w);
	maybe_claim(w);
}

/* The lease is lost, or the daemon said stop: kill the encoder, drop what
 * it made, say nothing more about the job. */
static void
fence(
	Job         *job,
	const gchar *why
){
	if (job->fenced)
		return;
	job->fenced = TRUE;
	g_warning("job %" G_GINT64_FORMAT ": %s: its encoder is stopped and what it made is dropped", job->run->job->id,
		why);
	job_remove_timers(job);
	g_cancellable_cancel(job->run->cancel);
	if (job->cancelled_us == 0)
		job->cancelled_us = g_get_monotonic_time();
	if (thread_over(job) && !job->reporting && !job->beating)
		job_drop(job);
}

static gboolean
past_deadline(Job *job)
{
	return g_get_monotonic_time() >= job->deadline_us;
}

static gboolean on_deadline(gpointer data);

static void
schedule_deadline(Job *job)
{
	gint64 left = (job->deadline_us - g_get_monotonic_time()) / 1000;

	if (job->deadline_id != 0)
		g_source_remove(job->deadline_id);
	job->deadline_id = g_timeout_add((guint)CLAMP(left, 0, (gint64)G_MAXUINT), on_deadline, job);
}

static gboolean
on_deadline(gpointer data)
{
	Job *job = data;

	job->deadline_id = 0;
	if (!job->fenced && !thread_over(job))
		fence(job, "its lease could not be renewed before its deadline");
	else if (!job->fenced && past_deadline(job))
		fence(job, "its lease ran out before its result could be reported");
	return G_SOURCE_REMOVE;
}

/* ── Reporting a job's end ──────────────────────────────────────── */

static void report(Job *job);

static gboolean
on_report_retry(gpointer data)
{
	Job *job = data;

	job->report_retry_id = 0;
	report(job);
	return G_SOURCE_REMOVE;
}

/*
 * Whether an answer to a report means "try again": 429 and every 5xx
 * (the daemon busy, its connection limits, its lock being taken back), and
 * any answer that is not the worker API's own refusal (a proxy's).  Only
 * the worker API's own refusal (a 4xx it wrote: the lease is not this
 * worker's, the token is not a worker's, the report is not acceptable)
 * ends the job here.
 */
static gboolean
report_again(
	guint        status,
	const gchar *word
){
	return status == 429 || status >= 500 || word == NULL;
}

/* Sends the report again after a while, unless that would be past the
 * lease's deadline: FALSE then. */
static gboolean
retry_report(
	Job   *job,
	guint  tries
){
	guint delay = MIN((guint)REPORT_RETRY_FIRST_MS << MIN(tries, 8u), (guint)REPORT_RETRY_MAX_MS);

	if (job->fenced || job->worker->fatal || g_get_monotonic_time() + (gint64)delay * 1000 >= job->deadline_us)
		return FALSE;
	/* Not "reporting" while it waits: a fence (the deadline passing) drops
	 * it, and takes this timer with it. */
	job->report_retry_id = g_timeout_add(delay, on_report_retry, job);
	return TRUE;
}

static void
on_reported(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Job *job = data;
	Worker *w = job->worker;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *word = NULL;
	guint status = 0;

	w->in_flight--;
	job->reporting = FALSE;
	job->report_tries++;
	if (!axis_worker_client_post_finish(w->client, result, &status, &body, &error)) {
		/* The daemon may have taken it before the answer was lost: sent
		 * again, the daemon says whether it has it (the lease is no longer
		 * this worker's). */
		job->report_lost = TRUE;
		g_warning("job %" G_GINT64_FORMAT ": its end could not be reported: %s; trying again until its lease runs "
			"out", job->run->job->id, error->message);
		if (retry_report(job, job->report_tries))
			return;
		job->delivered = TRUE;
		g_warning("job %" G_GINT64_FORMAT ": its end could not be reported before its lease ran out", job->run->job->id);
	} else if (status == 200) {
		job->delivered = TRUE;
		axis_worker_work_touch_health(w->config->state);
		g_message("job %" G_GINT64_FORMAT ": reported", job->run->job->id);
	} else {
		word = error_word(body, &message);
		if (report_again(status, word)) {
			g_message("job %" G_GINT64_FORMAT ": the report was answered %u (%s); trying again until its lease runs "
				"out", job->run->job->id, status, word != NULL ? word : "not the worker API's own answer");
			if (retry_report(job, job->report_tries))
				return;
			g_warning("job %" G_GINT64_FORMAT ": its end could not be reported before its lease ran out",
				job->run->job->id);
		} else if (status == 409 && job->report_lost) {
			/* Sent again after an answer was lost: the daemon may have
			 * taken the first one (or let the lease go meanwhile). */
			job->delivered = TRUE;
			g_message("job %" G_GINT64_FORMAT ": the daemon no longer holds its lease for this worker: it took the "
				"report whose answer was lost, or let the lease go", job->run->job->id);
		} else {
			g_warning("job %" G_GINT64_FORMAT ": the daemon refused the report (%u %s: %s)", job->run->job->id,
				status, word, message != NULL ? message : "");
			if (status == 401)
				became_fatal(w, "the daemon refused this worker's token (was it revoked?)");
		}
	}
	job->fenced = TRUE;
	job_drop(job);
}

static void
report(Job *job)
{
	Worker *w = job->worker;
	AxisWorkerRunJob *run = job->run;
	g_autofree gchar *json = NULL;
	const gchar *request;

	if (job->fenced) {
		job_drop(job);
		return;
	}
	/* Never past the lease: a worker that was stopped or blocked finds out
	 * here. */
	if (past_deadline(job)) {
		fence(job, "its lease ran out before its result could be reported");
		return;
	}
	g_mutex_lock(&run->lock);
	/* A result that is there is reported, even when stopping; a job the
	 * stop interrupted is given back.  A job given up is reported failed
	 * whatever its thread has made: that thread is still running. */
	if (job->abandoned || run->failure != NULL || (run->sample_result == NULL && run->caps_result == NULL &&
	                                               run->convert_result == NULL && run->verify_result == NULL)) {
		AxisWorkerFail fail;

		memset(&fail, 0, sizeof(fail));
		fail.job = run->job->id;
		fail.lease_token = run->job->lease_token;
		/* A job given up is a failure, never a job given back, even while
		 * stopping: its thread may still hold what it made in the work area,
		 * and a give-back uses no attempt of the job.  When its thread said
		 * why it failed before it stood still (a volume lost, a name taken),
		 * that is reported, and a failure for good stays one; otherwise it is
		 * a failure for another try. */
		if (job->abandoned && run->failure != NULL) {
			fail.error = run->failure;
			fail.retry = run->retry;
		} else if (job->abandoned) {
			fail.error = (gchar *)"the worker gave the job up: its thread did not end after it was told to stop (a "
				"volume that does not answer?)";
			fail.retry = TRUE;
		} else if (w->stopping && !run->volume_lost && run->failure == NULL) {
			fail.error = (gchar *)"the worker is stopping";
			fail.stopping = TRUE;
			fail.retry = TRUE;
		} else if (run->failure != NULL) {
			fail.error = run->failure;
			fail.retry = run->retry;
		} else {
			fail.error = (gchar *)"the job ended without a result";
			fail.retry = TRUE;
		}
		json = axis_worker_fail_to_json(&fail);
		request = "fail";
		g_message("job %" G_GINT64_FORMAT ": %s: %s", run->job->id, fail.stopping ? "giving it back" : "failed",
			fail.error);
	} else {
		AxisWorkerComplete complete;

		memset(&complete, 0, sizeof(complete));
		complete.job = run->job->id;
		complete.lease_token = run->job->lease_token;
		complete.kind = run->job->kind;
		complete.sample = run->sample_result;
		complete.caps = run->caps_result;
		complete.convert = run->convert_result;
		complete.verify = run->verify_result;
		json = axis_worker_complete_to_json(&complete);
		request = "complete";
		if (json == NULL) {
			/* Measurements this protocol cannot carry (not one of a
			 * measurement's endings): never sent as a result. */
			AxisWorkerFail fail;

			memset(&fail, 0, sizeof(fail));
			fail.job = run->job->id;
			fail.lease_token = run->job->lease_token;
			fail.error = (gchar *)"the measurements cannot be reported";
			fail.retry = TRUE;
			json = axis_worker_fail_to_json(&fail);
			request = "fail";
		}
	}
	g_mutex_unlock(&run->lock);

	/* What a new measurement found is this worker's from now on. */
	g_mutex_lock(&run->lock);
	if (run->caps_result != NULL && run->failure == NULL && !job->abandoned) {
		axis_encoder_capabilities_free(w->caps);
		w->caps = axis_encoder_capabilities_copy(run->caps_result);
		if (!axis_worker_work_store_caps(w->config, w->caps))
			g_warning("the capability measurement could not be kept in the state directory");
	}
	g_mutex_unlock(&run->lock);

	job_remove_timers(job);
	schedule_deadline(job);
	job->reporting = TRUE;
	w->in_flight++;
	axis_worker_client_post(w->client, request, json, request_timeout(w, job->deadline_us), on_reported, job);
}

/* The job's encoder crashed: from now on this worker takes no work. */
static void
pause_after(Job *job)
{
	Worker *w = job->worker;
	gint signal_number;

	g_mutex_lock(&job->run->lock);
	signal_number = job->run->crash_signal;
	g_mutex_unlock(&job->run->lock);
	if (signal_number == 0 || w->paused_job != 0)
		return;
	w->paused_job = job->run->job->id;
	g_free(w->paused_reason);
	w->paused_reason = g_strdup_printf("signal %d, SIG%s", signal_number, axis_probe_run_signal_name(signal_number));
	g_warning("job %" G_GINT64_FORMAT ": its encoder crashed (%s): this worker takes no more work until it is resumed "
		"on the Workers page or restarted", w->paused_job, w->paused_reason);
}

static void
on_thread_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Job *job = data;
	Worker *w = job->worker;

	job->thread_done = TRUE;
	if (job->abandoned) {
		/* Given up already: nothing the thread did is acted upon (no
		 * report, no measurement kept, no pause); what it left in the work
		 * area goes now. */
		g_message("job %" G_GINT64_FORMAT ": its thread ended, %" G_GINT64_FORMAT " s after the job was given up; what "
			"it did is not reported", job->run->job->id, (g_get_monotonic_time() - job->abandoned_us) / G_USEC_PER_SEC);
		/* Out of the running jobs: freed now.  Otherwise its failure is
		 * being reported, and on_reported() drops it. */
		if (g_ptr_array_remove(w->abandoned, job)) {
			job_free(job);
			maybe_quit(w);
			maybe_claim(w);
		}
		return;
	}
	pause_after(job);
	if (job->fenced) {
		if (!job->beating && !job->reporting)
			job_drop(job);
		return;
	}
	if (job->beating)
		return;	/* reported when the heartbeat in flight is answered */
	report(job);
}

static void
job_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	AxisWorkerRunJob *run = task_data;

	axis_worker_work_run(run);
	g_task_return_boolean(task, TRUE);
}

/* ── Heartbeats ─────────────────────────────────────────────────── */

typedef struct
{
	Job    *job;
	gint64  sent_us;
} Beat;

static gboolean on_beat(gpointer data);

/* A heartbeat sent again sooner than the next, after one that did not
 * renew the lease. */
static gboolean
on_beat_retry(gpointer data)
{
	Job *job = data;

	job->beat_retry_id = 0;
	on_beat(job);
	return G_SOURCE_REMOVE;
}

static void
on_beat_answered(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Beat *beat = data;
	Job *job = beat->job;
	Worker *w = job->worker;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *word = NULL;
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisWorkerBeat) answer = NULL;
	gint64 sent = beat->sent_us;
	gboolean beat_again = FALSE;
	guint status = 0;
	gsize length = 0;
	const gchar *text;

	g_free(beat);
	w->in_flight--;
	/* job->beating stays set until "out": fence() drops a job whose
	 * thread is done unless it is beating, and the job is used below. */
	if (!axis_worker_client_post_finish(w->client, result, &status, &body, &error)) {
		g_message("job %" G_GINT64_FORMAT ": a heartbeat was not answered: %s; trying again", job->run->job->id,
			error->message);
		beat_again = TRUE;
		goto out;
	}
	if (status == 200) {
		text = g_bytes_get_data(body, &length);
		answer = axis_worker_beat_parse(text, length, &problem);
		if (answer == NULL) {
			g_warning("job %" G_GINT64_FORMAT ": the daemon's heartbeat answer is not acceptable: %s",
				job->run->job->id, problem);
			goto out;
		}
		axis_worker_work_touch_health(w->config->state);
		if (answer->stop) {
			fence(job, g_strcmp0(answer->reason, "cancelled") == 0 ? "the daemon said stop: it was cancelled" :
				"the daemon said stop: the lease is not this worker's");
			goto out;
		}
		/* Renewed: from when this heartbeat was sent, never more. */
		if (!job->fenced && sent < job->deadline_us) {
			job->deadline_us = sent + (gint64)answer->lease_ms * 1000 - margin_us(answer->lease_ms);
			job->beat_tries = 0;
			if (!job->reporting)
				schedule_deadline(job);
		}
		goto out;
	}
	word = error_word(body, &message);
	if (status == 409 && word != NULL) {
		fence(job, "the daemon says the lease is not this worker's any more");
	} else if (status == 401 && word != NULL) {
		fence(job, "the daemon refused this worker's token");
		became_fatal(w, "the daemon refused this worker's token (was it revoked?)");
	} else if (report_again(status, word)) {
		/* Busy, or not the worker API's own answer: the lease is renewed
		 * by the next one, sooner than the next beat. */
		g_message("job %" G_GINT64_FORMAT ": a heartbeat was answered %u (%s); trying again", job->run->job->id,
			status, word != NULL ? word : "not the worker API's own answer");
		beat_again = TRUE;
	} else {
		g_message("job %" G_GINT64_FORMAT ": a heartbeat was refused (%u %s)", job->run->job->id, status, word);
	}

out:
	if (beat_again && !job->fenced && !thread_over(job) && job->beat_retry_id == 0) {
		guint delay = MIN((guint)REPORT_RETRY_FIRST_MS << MIN(job->beat_tries, 8u), (guint)REPORT_RETRY_MAX_MS);

		job->beat_tries++;
		if (g_get_monotonic_time() + (gint64)delay * 1000 < job->deadline_us)
			job->beat_retry_id = g_timeout_add(delay, on_beat_retry, job);
	}
	job->beating = FALSE;
	if (job->fenced) {
		if (thread_over(job) && !job->reporting)
			job_drop(job);
		return;
	}
	if (thread_over(job) && !job->reporting)
		report(job);
}

static gboolean
on_beat(gpointer data)
{
	Job *job = data;
	Worker *w = job->worker;
	AxisWorkerHeartbeat beat;
	g_autofree gchar *json = NULL;
	Beat *pending;

	if (job->fenced || thread_over(job) || job->beating || job->reporting)
		return G_SOURCE_CONTINUE;
	if (past_deadline(job)) {
		fence(job, "its lease could not be renewed before its deadline");
		return G_SOURCE_REMOVE;
	}
	memset(&beat, 0, sizeof(beat));
	beat.job = job->run->job->id;
	beat.lease_token = job->run->job->lease_token;
	g_mutex_lock(&job->run->lock);
	beat.has_progress = TRUE;
	beat.progress = job->run->progress;
	beat.message = g_strdup(job->run->message);
	g_mutex_unlock(&job->run->lock);
	json = axis_worker_heartbeat_to_json(&beat);
	g_free(beat.message);

	pending = g_new0(Beat, 1);
	pending->job = job;
	pending->sent_us = g_get_monotonic_time();
	job->beating = TRUE;
	w->in_flight++;
	axis_worker_client_post(w->client, "heartbeat", json, request_timeout(w, job->deadline_us), on_beat_answered,
		pending);
	return G_SOURCE_CONTINUE;
}

/* ── Starting a job ─────────────────────────────────────────────── */

/* Whether this worker can run @job as described: kinds it knows, an
 * encoder family it may run and was shown to run, volumes it proved. */
static const gchar *
cannot_run(
	Worker              *w,
	const AxisWorkerJob *job
){
	guint i;
	guint j;

	if (strcmp(job->kind, AXIS_WORKER_KIND_CAPABILITIES) == 0)
		return NULL;
	if (strcmp(job->kind, AXIS_WORKER_KIND_CONVERT) == 0 && job->convert != NULL) {
		for (j = 0; j < w->proofs->len; j++) {
			if (strcmp(axis_volume_proof_get_uuid(g_ptr_array_index(w->proofs, j)), job->convert->volume) == 0)
				return NULL;
		}
		return "the job needs a volume this worker has not proved";
	}
	if (strcmp(job->kind, AXIS_WORKER_KIND_VERIFY) == 0 && job->verify != NULL) {
		for (j = 0; j < w->proofs->len; j++) {
			if (strcmp(axis_volume_proof_get_uuid(g_ptr_array_index(w->proofs, j)), job->verify->volume) == 0)
				return NULL;
		}
		return "the job needs a volume this worker has not proved";
	}
	if (strcmp(job->kind, AXIS_WORKER_KIND_SAMPLE) != 0 || job->sample == NULL)
		return "this worker does not run that kind of job";
	if ((w->config->skip_families & AXIS_CONVERT_FAMILY_BIT(job->sample->family)) != 0)
		return "this worker is not allowed to run that encoder family";
	if (job->sample->media == AXIS_TARGET_VIDEO) {
		const AxisConvertEncoder *encoder = axis_convert_encoder_for(job->sample->codec, job->sample->family);
		const AxisEncoderCapability *cap = encoder != NULL ? axis_encoder_capabilities_get(w->caps, encoder) : NULL;

		if (cap == NULL || !cap->usable)
			return "this worker was not shown able to run that encoder";
	}
	for (i = 0; i < job->sample->files->len; i++) {
		const AxisSampleFilePlan *f = g_ptr_array_index(job->sample->files, i);
		gboolean proved = FALSE;

		for (j = 0; j < w->proofs->len; j++)
			proved = proved || strcmp(axis_volume_proof_get_uuid(g_ptr_array_index(w->proofs, j)), f->volume) == 0;
		if (!proved)
			return "the job needs a volume this worker has not proved";
	}
	return NULL;
}

static void
give_back_now(
	Worker        *w,
	AxisWorkerJob *claimed,
	gint64         sent_us,
	const gchar   *why,
	gboolean       stopping
);

static void
start_job(
	Worker        *w,
	AxisWorkerJob *claimed,
	gint64         sent_us
){
	g_autofree gchar *template = NULL;
	AxisWorkerRunJob *run;
	const gchar *why;
	GTask *task;
	Job *job;
	guint i;

	why = cannot_run(w, claimed);
	if (why != NULL) {
		g_warning("job %" G_GINT64_FORMAT " (%s): %s; giving it back", claimed->id, claimed->kind, why);
		give_back_now(w, claimed, sent_us, why, FALSE);
		return;
	}
	run = g_new0(AxisWorkerRunJob, 1);
	run->ref_count = 1;
	run->worker = w;
	run->job = claimed;
	run->cancel = g_cancellable_new();
	run->ffmpeg = g_strdupv(w->config->ffmpeg);
	run->ffprobe = g_strdupv(w->config->ffprobe);
	run->skip_families = w->config->skip_families;
	run->retry = TRUE;
	run->output_area = -1;
	run->output_anchor = -1;
	g_mutex_init(&run->lock);
	run->check = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_proof_free);
	/* Where each volume was proved: the job proves them again itself. */
	run->mounts = g_ptr_array_new_with_free_func(mount_free);
	for (i = 0; i < w->proofs->len; i++) {
		AxisVolumeProof *proof = g_ptr_array_index(w->proofs, i);
		AxisWorkerMount *mount = g_new0(AxisWorkerMount, 1);

		mount->uuid = g_strdup(axis_volume_proof_get_uuid(proof));
		mount->path = g_strdup(axis_volume_proof_get_path(proof));
		mount->keeps_identity = axis_worker_config_keeps_identity(w->config, mount->path);
		g_ptr_array_add(run->mounts, mount);
	}
	template =g_strdup_printf("%s/" AXIS_WORKER_JOB_PREFIX "%" G_GINT64_FORMAT "-XXXXXX", w->config->scratch,
		claimed->id);
	run->scratch = g_mkdtemp_full(template, 0700) != NULL ? g_strdup(template) : NULL;

	job = g_new0(Job, 1);
	job->worker = w;
	job->run = run;
	job->deadline_us = sent_us + (gint64)claimed->lease_ms * 1000 - margin_us(claimed->lease_ms);
	g_ptr_array_add(w->jobs, job);
	if (run->scratch == NULL) {
		axis_worker_work_fail(run, "no directory for the job can be made in the scratch directory", TRUE, FALSE);
		job->thread_done = TRUE;
		report(job);
		return;
	}
	g_message("job %" G_GINT64_FORMAT " (%s), attempt %u: started", claimed->id, claimed->kind, claimed->attempt);
	job->beat_id = g_timeout_add(claimed->heartbeat_ms, on_beat, job);
	schedule_deadline(job);
	task = g_task_new(NULL, NULL, on_thread_done, job);
	g_task_set_task_data(task, axis_worker_run_job_ref(run), (GDestroyNotify)axis_worker_run_job_unref);
	g_task_run_in_thread(task, job_thread);
	g_object_unref(task);
}

typedef struct
{
	Worker *w;
	gint64  job;
} GiveBack;

static void
on_given_back(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	GiveBack *back = data;
	Worker *w = back->w;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	guint status = 0;

	w->in_flight--;
	if (!axis_worker_client_post_finish(w->client, result, &status, &body, &error))
		g_message("job %" G_GINT64_FORMAT ": giving it back was not answered: %s", back->job, error->message);
	g_free(back);
	maybe_quit(w);
	maybe_claim(w);
}

/* A job that is not to run at all: given back at once. */
static void
give_back_now(
	Worker        *w,
	AxisWorkerJob *claimed,
	gint64         sent_us,
	const gchar   *why,
	gboolean       stopping
){
	g_autofree gchar *json = NULL;
	AxisWorkerFail fail;
	GiveBack *back;
	gint64 deadline = sent_us + (gint64)claimed->lease_ms * 1000 - margin_us(claimed->lease_ms);

	if (g_get_monotonic_time() >= deadline) {
		/* Its lease is over already: nothing is said about it. */
		axis_worker_job_free(claimed);
		maybe_quit(w);
		maybe_claim(w);
		return;
	}
	memset(&fail, 0, sizeof(fail));
	fail.job = claimed->id;
	fail.lease_token = claimed->lease_token;
	fail.error = (gchar *)why;
	fail.retry = TRUE;
	fail.stopping = stopping;
	json = axis_worker_fail_to_json(&fail);
	back = g_new0(GiveBack, 1);
	back->w = w;
	back->job = claimed->id;
	w->in_flight++;
	axis_worker_client_post(w->client, "fail", json, request_timeout(w, deadline), on_given_back, back);
	axis_worker_job_free(claimed);
}

/* ── Claiming ───────────────────────────────────────────────────── */

typedef struct
{
	Worker   *w;
	gint64    sent_us;
	gboolean  reported_pause;	/* it said this worker's encoder crashed */
} Claim;

static gboolean
on_claim_timer(gpointer data)
{
	Worker *w = data;

	w->claim_id = 0;
	maybe_claim(w);
	return G_SOURCE_REMOVE;
}

static void
claim_later(
	Worker *w,
	guint   ms
){
	if (w->claim_id != 0)
		g_source_remove(w->claim_id);
	w->claim_id = g_timeout_add(ms, on_claim_timer, w);
}

static void
on_claimed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Claim *claim = data;
	Worker *w = claim->w;
	gint64 sent = claim->sent_us;
	gboolean reported_pause = claim->reported_pause;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *word = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *paused = NULL;
	AxisWorkerJob *job = NULL;
	guint status = 0;
	gsize length = 0;
	const gchar *text;

	g_free(claim);
	w->in_flight--;
	w->claiming = FALSE;
	if (!axis_worker_client_post_finish(w->client, result, &status, &body, &error)) {
		g_message("asking for work: %s", error->message);
		next_retry(w);
		claim_later(w, w->retry_ms);
		maybe_quit(w);
		return;
	}
	if (status != 200) {
		word = error_word(body, &message);
		if (status == 401) {
			became_fatal(w, "the daemon refused this worker's token (was it revoked?)");
		} else if (status == 409 && g_strcmp0(word, "unregistered") == 0) {
			g_clear_pointer(&w->registered, axis_worker_registered_free);
			start_register(w);
		} else {
			g_message("asking for work was refused (%u %s: %s)", status, word != NULL ? word : "?",
				message != NULL ? message : "");
			next_retry(w);
			claim_later(w, w->retry_ms);
		}
		maybe_quit(w);
		return;
	}
	text = g_bytes_get_data(body, &length);
	if (!axis_worker_claimed_parse(text, length, &job, &paused, &problem)) {
		g_warning("the daemon's answer to a claim is not acceptable: %s", problem);
		next_retry(w);
		claim_later(w, w->retry_ms);
		maybe_quit(w);
		return;
	}
	w->retry_ms = 0;
	axis_worker_work_touch_health(w->config->state);
	/* What the daemon says of the pause.  "Not held" after this worker
	 * reported its crash in the claim is a person's resume: the daemon holds
	 * a reported crash until then. */
	if (paused != NULL && g_strcmp0(paused, w->held_paused) != 0)
		g_warning("the daemon holds this worker paused: %s", paused);
	if (paused == NULL && w->paused_job != 0 && reported_pause) {
		g_message("resumed on the Workers page after the crash on job %" G_GINT64_FORMAT ": taking work again",
			w->paused_job);
		w->paused_job = 0;
		g_clear_pointer(&w->paused_reason, g_free);
	}
	g_free(w->held_paused);
	w->held_paused = g_steal_pointer(&paused);
	if (job == NULL) {
		if (w->registered != NULL)
			claim_later(w, w->registered->poll_ms);
		maybe_quit(w);
		return;
	}
	if (w->stopping) {
		give_back_now(w, job, sent, "the worker is stopping", TRUE);
		return;
	}
	if (w->paused_job != 0) {
		/* A claim sent before the crash was seen: given back at once, as a
		 * worker that stops gives its work back (no attempt is used). */
		g_warning("job %" G_GINT64_FORMAT " (%s): this worker is paused after its encoder crashed; giving it back",
			job->id, job->kind);
		give_back_now(w, job, sent, "the worker is paused: its encoder crashed", TRUE);
		return;
	}
	start_job(w, job, sent);
	maybe_claim(w);
}

static void
maybe_claim(Worker *w)
{
	g_autofree gchar *json = NULL;
	GPtrArray *volumes;
	AxisWorkerClaim request;
	Claim *claim;
	guint i;

	if (w->stopping || w->registered == NULL || w->claiming || w->registering || w->jobs->len >= w->config->max_jobs ||
	    w->claim_id != 0)
		return;
	volumes = g_ptr_array_new();
	for (i = 0; i < w->proofs->len; i++)
		g_ptr_array_add(volumes, (gpointer)axis_volume_proof_get_uuid(g_ptr_array_index(w->proofs, i)));
	g_ptr_array_add(volumes, NULL);
	request.kinds = (gchar **)kinds;
	request.volumes = (gchar **)volumes->pdata;
	/* Paused: it still asks, saying why, to hear when it is resumed. */
	request.paused_job = w->paused_job;
	request.paused_reason = w->paused_reason;
	json = axis_worker_claim_to_json(&request);
	g_ptr_array_unref(volumes);

	claim = g_new0(Claim, 1);
	claim->w = w;
	claim->sent_us = g_get_monotonic_time();
	claim->reported_pause = w->paused_job != 0;
	w->claiming = TRUE;
	w->in_flight++;
	axis_worker_client_post(w->client, "claim", json, w->config->request_timeout_ms, on_claimed, claim);
}

/* ── Registering ────────────────────────────────────────────────── */

static gboolean
on_register_timer(gpointer data)
{
	Worker *w = data;

	w->claim_id = 0;
	start_register(w);
	return G_SOURCE_REMOVE;
}

static void
on_registered(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Worker *w = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *word = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerRegistered *answer;
	guint status = 0;
	gsize length = 0;
	const gchar *text;
	guint i;

	w->in_flight--;
	w->registering = FALSE;
	if (w->stopping) {
		maybe_quit(w);
		return;
	}
	if (!axis_worker_client_post_finish(w->client, result, &status, &body, &error)) {
		g_message("registering with the daemon: %s; trying again", error->message);
		next_retry(w);
		w->claim_id = g_timeout_add(w->retry_ms, on_register_timer, w);
		return;
	}
	if (status != 200) {
		word = error_word(body, &message);
		if (status == 401) {
			became_fatal(w, "the daemon refused this worker's token (was it revoked?)");
			maybe_quit(w);
			return;
		}
		g_warning("registering was refused (%u %s: %s); trying again", status, word != NULL ? word : "?",
			message != NULL ? message : "");
		next_retry(w);
		w->claim_id = g_timeout_add(w->retry_ms, on_register_timer, w);
		return;
	}
	text = g_bytes_get_data(body, &length);
	answer = axis_worker_registered_parse(text, length, &problem);
	if (answer == NULL) {
		g_warning("the daemon's answer to register is not acceptable: %s; trying again", problem);
		next_retry(w);
		w->claim_id = g_timeout_add(w->retry_ms, on_register_timer, w);
		return;
	}
	w->retry_ms = 0;
	axis_worker_work_touch_health(w->config->state);
	/* Only the volumes the daemon accepted are offered work. */
	for (i = 0; i < w->pending_proofs->len; ) {
		AxisVolumeProof *proof = g_ptr_array_index(w->pending_proofs, i);
		const AxisWorkerVolumeAnswer *verdict = NULL;
		guint j;

		for (j = 0; j < answer->volumes->len; j++) {
			const AxisWorkerVolumeAnswer *v = g_ptr_array_index(answer->volumes, j);

			if (strcmp(v->uuid, axis_volume_proof_get_uuid(proof)) == 0)
				verdict = v;
		}
		if (verdict != NULL && verdict->accepted) {
			g_message("volume %s at %s: proved and accepted%s", axis_volume_proof_get_uuid(proof),
				axis_volume_proof_get_path(proof), axis_worker_config_keeps_identity(w->config,
				axis_volume_proof_get_path(proof)) ? "" : " (a mount that does not keep file identities: a source is "
				"checked by its size, its time to the second and samples of its content)");
			g_ptr_array_add(w->proofs, g_ptr_array_steal_index(w->pending_proofs, i));
		} else {
			g_warning("volume %s at %s: the daemon did not accept it (%s)", axis_volume_proof_get_uuid(proof),
				axis_volume_proof_get_path(proof), verdict != NULL && verdict->reason != NULL ? verdict->reason :
				"no answer for it");
			/* Not reported again until another disk is found there. */
			g_hash_table_replace(w->refused, g_strdup(axis_volume_proof_get_path(proof)),
				g_strdup(axis_volume_proof_get_uuid(proof)));
			i++;
		}
	}
	/* Proofs refused stay out until they are proved again. */
	g_ptr_array_set_size(w->pending_proofs, 0);
	axis_worker_registered_free(w->registered);
	w->registered = answer;
	g_message("registered as %s: lease %u ms, heartbeat %u ms, %u volume(s) accepted", answer->worker,
		answer->lease_ms, answer->heartbeat_ms, w->proofs->len);
	if ((w->later->len > 0 || w->register_again) && !w->stopping) {
		start_register(w);
		return;
	}
	maybe_claim(w);
}

static void
start_register(Worker *w)
{
	g_autoptr(AxisWorkerRegister) request = axis_worker_register_new();
	g_autofree gchar *json = NULL;
	guint i;

	if (w->registering || w->stopping)
		return;
	w->registered_once = TRUE;
	w->register_again = FALSE;
	if (w->start_id != 0) {
		g_source_remove(w->start_id);
		w->start_id = 0;
	}
	/* What was proved while the last registration was in flight. */
	while (w->later->len > 0)
		g_ptr_array_add(w->pending_proofs, g_ptr_array_steal_index(w->later, 0));
	/* Every proof is reported again; what the daemon accepts is kept. */
	for (i = 0; i < w->proofs->len; i++)
		g_ptr_array_add(w->pending_proofs, g_ptr_array_index(w->proofs, i));
	g_ptr_array_set_free_func(w->proofs, NULL);
	g_ptr_array_set_size(w->proofs, 0);
	g_ptr_array_set_free_func(w->proofs, (GDestroyNotify)axis_volume_proof_free);
	for (i = 0; i < w->pending_proofs->len; i++) {
		AxisVolumeProof *proof = g_ptr_array_index(w->pending_proofs, i);
		AxisWorkerVolumeSeen *seen = g_new0(AxisWorkerVolumeSeen, 1);
		guint j;
		gboolean twice = FALSE;

		for (j = 0; j < request->volumes->len; j++)
			twice = twice || strcmp(((AxisWorkerVolumeSeen *)g_ptr_array_index(request->volumes, j))->uuid,
				axis_volume_proof_get_uuid(proof)) == 0;
		if (twice) {
			g_free(seen);
			continue;
		}
		seen->uuid = g_strdup(axis_volume_proof_get_uuid(proof));
		seen->path = g_strdup(axis_volume_proof_get_path(proof));
		seen->keeps_identity = axis_worker_config_keeps_identity(w->config, seen->path);
		g_ptr_array_add(request->volumes, seen);
	}
	request->worker_version = g_strdup(AXIS_VERSION);
	request->max_jobs = w->config->max_jobs;
	request->caps = axis_encoder_capabilities_copy(w->caps);
	json = axis_worker_register_to_json(request);
	w->registering = TRUE;
	w->in_flight++;
	axis_worker_client_post(w->client, "register", json, w->config->request_timeout_ms, on_registered, w);
}

/* ── Checking the volumes ───────────────────────────────────────── */

/* The check or proof of one volume path, on a thread of its own: a volume
 * that hangs holds back only itself.  While the thread of a path has not
 * returned, no other is started for that path (a thread that never returns
 * is never followed by more of them).  The thread touches this record only
 * (its inputs are copies); the main loop reads the results when it returns,
 * and frees it then. */
typedef struct
{
	Worker          *w;
	gchar           *path;
	gint64           started_us;
	gboolean         timed_out;	/* the main loop waited long enough: it is treated as hung */
	AxisVolumeProof *idle;		/* a copy of the worker's proof of this path, or NULL */
	gboolean         idle_ok;
	gboolean         prove;		/* nothing is proved here: prove it */
	AxisVolumeProof *fresh;		/* the new proof */
	gchar           *fresh_why;	/* why there is none */
	GPtrArray       *jobs;		/* AxisWorkerRunJob refs: jobs that read this path */
	GPtrArray       *job_checks;	/* per job: GPtrArray of AxisVolumeProof copies */
	GArray          *job_ok;	/* gboolean */
} Probe;

static void
probe_free(Probe *probe)
{
	g_free(probe->path);
	axis_volume_proof_free(probe->idle);
	axis_volume_proof_free(probe->fresh);
	g_free(probe->fresh_why);
	g_ptr_array_unref(probe->jobs);
	g_ptr_array_unref(probe->job_checks);
	g_array_unref(probe->job_ok);
	g_free(probe);
}

static void
probe_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Probe *probe = task_data;
	guint i;
	guint j;

#ifdef AXIS_DEBUG
	axis_worker_work_test_hang(probe->path);
#endif
	if (probe->idle != NULL)
		probe->idle_ok = axis_volume_proof_check(probe->idle) == AXIS_VOLUME_PROOF_OK;
	for (i = 0; i < probe->job_checks->len; i++) {
		GPtrArray *proofs = g_ptr_array_index(probe->job_checks, i);
		gboolean ok = TRUE;

		for (j = 0; j < proofs->len; j++)
			ok = ok && axis_volume_proof_check(g_ptr_array_index(proofs, j)) == AXIS_VOLUME_PROOF_OK;
		g_array_append_val(probe->job_ok, ok);
	}
	if (probe->prove)
		probe->fresh = axis_worker_work_prove_path(probe->path, &probe->fresh_why);
	g_task_return_boolean(task, TRUE);
}

static Job *
job_of_run(
	Worker           *w,
	AxisWorkerRunJob *run
){
	guint i;

	for (i = 0; i < w->jobs->len; i++) {
		Job *job = g_ptr_array_index(w->jobs, i);

		if (job->run == run)
			return job;
	}
	return NULL;
}

/* The registration after proofs changed: the first one waits for the first
 * proofs (but not for a volume that hangs: on_start_wait()), and one that
 * is in flight is followed by another when it is answered. */
static void
proofs_changed(Worker *w)
{
	if (w->stopping)
		return;
	if (!w->registered_once && (g_hash_table_size(w->probes) > 0 || w->caps == NULL))
		return;
	if (w->registering)
		w->register_again = TRUE;
	start_register(w);
}

/* A job that reads a volume that is not there any more stops now. */
static void
stop_for_volume(
	Worker           *w,
	AxisWorkerRunJob *run,
	const gchar      *why
){
	Job *job = job_of_run(w, run);

	if (job == NULL || job->fenced || thread_over(job))
		return;
	g_warning("job %" G_GINT64_FORMAT ": a volume it reads %s; stopping it", run->job->id, why);
	axis_worker_work_fail(run, "volume unavailable: the volume's directory is not the one proved, or its marker "
		"is not the volume's any more", TRUE, TRUE);
	g_cancellable_cancel(run->cancel);
	if (job->cancelled_us == 0)
		job->cancelled_us = g_get_monotonic_time();
}

/* Drops the worker's proof of the volume at @path, if it has one. */
static gboolean
drop_proof(
	Worker      *w,
	const gchar *path,
	const gchar *why
){
	gboolean changed = FALSE;
	guint i;

	for (i = w->proofs->len; i > 0; i--) {
		AxisVolumeProof *proof = g_ptr_array_index(w->proofs, i - 1);

		if (strcmp(axis_volume_proof_get_path(proof), path) != 0)
			continue;
		g_warning("volume %s at %s %s; it is offered no more work here", axis_volume_proof_get_uuid(proof), path, why);
		g_ptr_array_remove_index(w->proofs, i - 1);
		changed = TRUE;
	}
	for (i = w->pending_proofs->len; i > 0; i--) {
		AxisVolumeProof *proof = g_ptr_array_index(w->pending_proofs, i - 1);

		if (strcmp(axis_volume_proof_get_path(proof), path) == 0)
			g_ptr_array_remove_index(w->pending_proofs, i - 1);
	}
	return changed;
}

static void
on_probed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Probe *probe = data;
	Worker *w = probe->w;
	gboolean changed = FALSE;
	guint i;

	g_hash_table_remove(w->probes, probe->path);
	if (probe->timed_out)
		g_message("the volume path %s answers again", probe->path);
	/* A job whose volume is not the one proved any more stops now. */
	for (i = 0; i < probe->jobs->len; i++) {
		if (!g_array_index(probe->job_ok, gboolean, i))
			stop_for_volume(w, g_ptr_array_index(probe->jobs, i), "is not the one proved any more");
	}
	/* The worker's own: a volume that went drops out; one that came is
	 * reported. */
	if (probe->idle != NULL && !probe->idle_ok)
		changed = drop_proof(w, probe->path, "is not the one proved any more");
	if (probe->prove && probe->fresh == NULL)
		g_message("the volume path %s cannot be proved: %s", probe->path,
			probe->fresh_why != NULL ? probe->fresh_why : "unknown");
	if (probe->fresh != NULL) {
		AxisVolumeProof *proof = g_steal_pointer(&probe->fresh);
		const gchar *refused = g_hash_table_lookup(w->refused, axis_volume_proof_get_path(proof));

		if (refused != NULL && strcmp(refused, axis_volume_proof_get_uuid(proof)) == 0) {
			axis_volume_proof_free(proof);
		} else {
			/* A registration in flight reports what it was sent, no more. */
			g_ptr_array_add(w->registering ? w->later : w->pending_proofs, proof);
			changed = TRUE;
		}
	}
	probe_free(probe);
	if (changed || (!w->registered_once && g_hash_table_size(w->probes) == 0))
		proofs_changed(w);
	maybe_quit(w);
}

/* Whether @job reads the volume @uuid. */
static gboolean
job_reads(
	const AxisWorkerJob *job,
	const gchar         *uuid
){
	guint i;

	if (job->convert != NULL)
		return strcmp(job->convert->volume, uuid) == 0;
	if (job->verify != NULL)
		return strcmp(job->verify->volume, uuid) == 0;
	for (i = 0; job->sample != NULL && i < job->sample->files->len; i++) {
		const AxisSampleFilePlan *f = g_ptr_array_index(job->sample->files, i);

		if (strcmp(f->volume, uuid) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Drops the volumes @job reads from those offered work (and from those
 * about to be registered): the next check proves them again, and only then
 * are they offered again. */
static void
forget_volumes(
	Worker *w,
	Job    *job
){
	const AxisWorkerJob *claimed = job->run->job;
	gboolean changed = FALSE;
	guint i;

	for (i = w->proofs->len; i > 0; i--) {
		AxisVolumeProof *proof = g_ptr_array_index(w->proofs, i - 1);

		if (!job_reads(claimed, axis_volume_proof_get_uuid(proof)))
			continue;
		g_warning("volume %s at %s: the thread of job %" G_GINT64_FORMAT " does not end there; it is offered no more "
			"work here until it is proved again", axis_volume_proof_get_uuid(proof), axis_volume_proof_get_path(proof),
			claimed->id);
		g_ptr_array_remove_index(w->proofs, i - 1);
		changed = TRUE;
	}
	for (i = w->pending_proofs->len; i > 0; i--) {
		AxisVolumeProof *proof = g_ptr_array_index(w->pending_proofs, i - 1);

		if (!job_reads(claimed, axis_volume_proof_get_uuid(proof)))
			continue;
		g_warning("volume %s at %s: the thread of job %" G_GINT64_FORMAT " does not end there; it is offered no more "
			"work here until it is proved again", axis_volume_proof_get_uuid(proof), axis_volume_proof_get_path(proof),
			claimed->id);
		g_ptr_array_remove_index(w->pending_proofs, i - 1);
	}
	if (changed && !w->stopping)
		start_register(w);
}

/* A job whose thread does not return after its encoder was told to stop (a
 * volume that does not answer): the job is given up and its thread left to
 * end (see the top of this file). */
static void
give_up(Job *job)
{
	g_warning("job %" G_GINT64_FORMAT ": its thread does not end; giving the job up", job->run->job->id);
	job->abandoned = TRUE;
	job->abandoned_us = g_get_monotonic_time();
	forget_volumes(job->worker, job);
	/* A failure, so that it is tried again, unless it is fenced: then
	 * nothing, and it is dropped at once.  Either way it ends in
	 * job_drop(), which keeps it until its thread returns. */
	report(job);
}

/* Whether @proofs has a proof of the volume at @path. */
static gboolean
proofs_have(
	GPtrArray   *proofs,
	const gchar *path
){
	guint i;

	for (i = 0; i < proofs->len; i++)
		if (strcmp(axis_volume_proof_get_path(g_ptr_array_index(proofs, i)), path) == 0)
			return TRUE;
	return FALSE;
}

/* Begins the check or proof of the volume at @path, unless the thread of
 * the last one has not returned: never a second thread for a path. */
static void
start_probe(
	Worker      *w,
	const gchar *path,
	gboolean     configured
){
	Probe *probe;
	GTask *task;
	guint i;
	guint j;

	if (g_hash_table_contains(w->probes, path))
		return;
	probe = g_new0(Probe, 1);
	probe->w = w;
	probe->path = g_strdup(path);
	probe->started_us = g_get_monotonic_time();
	probe->jobs = g_ptr_array_new_with_free_func((GDestroyNotify)axis_worker_run_job_unref);
	probe->job_checks = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	probe->job_ok = g_array_new(FALSE, FALSE, sizeof(gboolean));
	for (i = 0; i < w->proofs->len; i++) {
		AxisVolumeProof *proof = g_ptr_array_index(w->proofs, i);

		if (strcmp(axis_volume_proof_get_path(proof), path) == 0)
			probe->idle = axis_volume_proof_dup(proof);
	}
	/* A refused path is proved again only to see whether another disk is
	 * there now (on_probed() drops the proof otherwise). */
	probe->prove = configured && !proofs_have(w->proofs, path) && !proofs_have(w->pending_proofs, path) &&
		!proofs_have(w->later, path);
	for (i = 0; i < w->jobs->len; i++) {
		Job *job = g_ptr_array_index(w->jobs, i);
		GPtrArray *copies;

		if (job->fenced || thread_over(job))
			continue;
		copies = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_proof_free);
		g_mutex_lock(&job->run->lock);
		for (j = 0; j < job->run->check->len; j++) {
			AxisVolumeProof *proof = g_ptr_array_index(job->run->check, j);
			AxisVolumeProof *copy;

			if (strcmp(axis_volume_proof_get_path(proof), path) != 0)
				continue;
			copy = axis_volume_proof_dup(proof);
			if (copy != NULL)
				g_ptr_array_add(copies, copy);
		}
		g_mutex_unlock(&job->run->lock);
		if (copies->len == 0) {
			g_ptr_array_unref(copies);
			continue;
		}
		g_ptr_array_add(probe->jobs, axis_worker_run_job_ref(job->run));
		g_ptr_array_add(probe->job_checks, copies);
	}
	g_hash_table_insert(w->probes, probe->path, probe);
	task = g_task_new(NULL, NULL, on_probed, probe);
	g_task_set_task_data(task, probe, NULL);
	g_task_run_in_thread(task, probe_thread);
	g_object_unref(task);
}

/* Each configured volume, and each volume a running job reads, is checked
 * or proved on a thread of its own. */
static void
start_probes(Worker *w)
{
	guint i;
	guint j;

	for (i = 0; w->config->volumes != NULL && w->config->volumes[i] != NULL; i++)
		start_probe(w, w->config->volumes[i], TRUE);
	for (i = 0; i < w->jobs->len; i++) {
		Job *job = g_ptr_array_index(w->jobs, i);
		GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

		if (job->fenced || thread_over(job)) {
			g_ptr_array_unref(paths);
			continue;
		}
		g_mutex_lock(&job->run->lock);
		for (j = 0; j < job->run->check->len; j++)
			g_ptr_array_add(paths, g_strdup(axis_volume_proof_get_path(g_ptr_array_index(job->run->check, j))));
		g_mutex_unlock(&job->run->lock);
		for (j = 0; j < paths->len; j++)
			start_probe(w, g_ptr_array_index(paths, j), FALSE);
		g_ptr_array_unref(paths);
	}
}

/* The longest a check of one volume may take before the volume is treated
 * as hung. */
static gint64
probe_limit_us(const Worker *w)
{
	return MAX((gint64)CHECK_TIMEOUT_MIN_MS, 3 * (gint64)w->config->volume_check_ms) * 1000;
}

/* A check that does not come back is a volume that hangs: it is offered no
 * more work, the jobs that read it stop, and the others go on.  Its thread
 * is left (nothing can end it); no other is started for the path until it
 * returns. */
static void
time_out_probes(Worker *w)
{
	GHashTableIter iter;
	gpointer value;
	gboolean changed = FALSE;
	GPtrArray *late = g_ptr_array_new();
	guint i;

	g_hash_table_iter_init(&iter, w->probes);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Probe *probe = value;

		if (!probe->timed_out && g_get_monotonic_time() - probe->started_us > probe_limit_us(w))
			g_ptr_array_add(late, probe);
	}
	for (i = 0; i < late->len; i++) {
		Probe *probe = g_ptr_array_index(late, i);
		guint j;

		probe->timed_out = TRUE;
		g_warning("the volume path %s did not answer within %u ms: it is not proven, and no work is offered on it "
			"until it answers and is proved", probe->path, (guint)(probe_limit_us(w) / 1000));
		changed = drop_proof(w, probe->path, "does not answer") || changed;
		for (j = 0; j < probe->jobs->len; j++) {
			AxisWorkerRunJob *run = g_ptr_array_index(probe->jobs, j);

			if (g_strcmp0(run->job->kind, AXIS_WORKER_KIND_SAMPLE) == 0 || g_strcmp0(run->job->kind,
			    AXIS_WORKER_KIND_VERIFY) == 0)
				stop_for_volume(w, run, "does not answer");
		}
	}
	g_ptr_array_unref(late);
	if (changed)
		proofs_changed(w);
}

/* The first registration does not wait for a volume that hangs: after
 * START_WAIT_MS it goes with the volumes proved so far, and the others are
 * registered when they are proved. */
static gboolean
on_start_wait(gpointer data)
{
	Worker *w = data;
	GHashTableIter iter;
	gpointer value;

	w->start_id = 0;
	g_hash_table_iter_init(&iter, w->probes);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const Probe *probe = value;

		if (probe->prove)
			g_warning("the volume path %s has not answered in %u ms: registering without it (it is not proven); it "
				"is proved when it answers", probe->path, START_WAIT_MS);
	}
	start_register(w);
	return G_SOURCE_REMOVE;
}

static gboolean
on_check(gpointer data)
{
	Worker *w = data;
	guint i;

	/* First, while stopping too and whether or not a check hangs: a job
	 * whose thread has not returned STUCK_MS after its encoder was told to
	 * stop is given up.  Never while a request about it is in flight, so
	 * that nothing is in flight about a job once it has left w->jobs. */
	for (i = 0; i < w->jobs->len; i++) {
		Job *job = g_ptr_array_index(w->jobs, i);

		if (!thread_over(job) && job->cancelled_us > 0 && !job->reporting && !job->beating &&
		    g_get_monotonic_time() - job->cancelled_us > (gint64)STUCK_MS * 1000) {
			give_up(job);
			break;	/* the list may have changed */
		}
	}
	if (w->stopping)
		return G_SOURCE_CONTINUE;
	time_out_probes(w);
	start_probes(w);
	return G_SOURCE_CONTINUE;
}

/* ── Stopping ───────────────────────────────────────────────────── */

static void
maybe_quit(Worker *w)
{
	if (!w->stopping)
		return;
	if (w->jobs->len > 0 || w->in_flight > 0)
		return;
	g_main_loop_quit(w->loop);
}

static gboolean
on_stop_timeout(gpointer data)
{
	Worker *w = data;

	w->stop_id = 0;
	g_warning("stopping took too long; exiting with requests unanswered (their leases will run out)");
	g_main_loop_quit(w->loop);
	return G_SOURCE_REMOVE;
}

/* No more claims; each running job's encoder is told to stop, and the job
 * is given back as it ends; then the worker exits (at the latest after
 * STOP_MAX_MS). */
static void
stop_running(Worker *w)
{
	guint i;

	w->stopping = TRUE;
	if (w->stop_id != 0)
		return;
	if (w->claim_id != 0) {
		g_source_remove(w->claim_id);
		w->claim_id = 0;
	}
	for (i = 0; i < w->jobs->len; i++) {
		Job *job = g_ptr_array_index(w->jobs, i);

		g_cancellable_cancel(job->run->cancel);
		if (job->cancelled_us == 0)
			job->cancelled_us = g_get_monotonic_time();
	}
	w->stop_id = g_timeout_add(STOP_MAX_MS, on_stop_timeout, w);
	maybe_quit(w);
}

static gboolean
on_signal(gpointer data)
{
	Worker *w = data;

	if (w->stopping && w->stop_id != 0)
		return G_SOURCE_CONTINUE;
	g_message("stopping: no more work is taken, and the work running is given back");
	stop_running(w);
	return G_SOURCE_CONTINUE;
}

/* ── Start ──────────────────────────────────────────────────────── */

typedef struct
{
	const AxisWorkerConfig  *config;
	AxisEncoderCapabilities *caps;
	GError                  *error;
} Prepare;

static void
prepare_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Prepare *prepare = task_data;

	axis_worker_work_prepare(prepare->config, &prepare->caps, &prepare->error);
	g_task_return_boolean(task, TRUE);
}

static void
on_prepared(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Prepare *prepare = g_task_get_task_data(G_TASK(result));
	Worker *w = data;

	w->in_flight--;
	if (prepare->error != NULL) {
		became_fatal(w, prepare->error->message);
		maybe_quit(w);
		return;
	}
	w->caps = g_steal_pointer(&prepare->caps);
	if (w->stopping) {
		maybe_quit(w);
		return;
	}
	w->check_id = g_timeout_add(w->config->volume_check_ms, on_check, w);
	/* Every volume is proved on a thread of its own, and the registration
	 * waits for them only a short time: a mount that hangs does not hold it
	 * back. */
	start_probes(w);
	if (g_hash_table_size(w->probes) == 0) {
		start_register(w);
		return;
	}
	w->start_id = g_timeout_add(START_WAIT_MS, on_start_wait, w);
}

static void
prepare_free(gpointer data)
{
	Prepare *prepare = data;

	axis_encoder_capabilities_free(prepare->caps);
	g_clear_error(&prepare->error);
	g_free(prepare);
}

gint
axis_worker_run(AxisWorkerConfig *config)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *description = axis_worker_config_describe(config);
	Prepare *prepare;
	Worker w;
	GTask *task;
	guint sigterm;
	guint sigint;
	guint i;

	memset(&w, 0, sizeof(w));
	w.config = config;
	w.client = axis_worker_client_new(config, &error);
	if (w.client == NULL) {
		g_critical("%s", error->message);
		return 1;
	}
	g_message("axis-worker %s (%s) starting: %s", AXIS_VERSION, AXIS_GIT_SHA, description);
	w.loop = g_main_loop_new(NULL, FALSE);
	w.proofs = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_proof_free);
	w.pending_proofs = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_proof_free);
	w.probes = g_hash_table_new(g_str_hash, g_str_equal);
	w.later = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_proof_free);
	w.refused = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	w.jobs = g_ptr_array_new();
	w.abandoned = g_ptr_array_new();

	/* What a killed run left behind goes first. */
	axis_worker_work_clean_scratch(config->scratch);

	sigterm = g_unix_signal_add(SIGTERM, on_signal, &w);
	sigint = g_unix_signal_add(SIGINT, on_signal, &w);

	prepare = g_new0(Prepare, 1);
	prepare->config = config;
	w.in_flight++;
	task = g_task_new(NULL, NULL, on_prepared, &w);
	g_task_set_task_data(task, prepare, prepare_free);
	g_task_run_in_thread(task, prepare_thread);
	g_object_unref(task);

	g_main_loop_run(w.loop);

	g_source_remove(sigterm);
	g_source_remove(sigint);
	if (w.claim_id != 0)
		g_source_remove(w.claim_id);
	if (w.check_id != 0)
		g_source_remove(w.check_id);
	if (w.stop_id != 0)
		g_source_remove(w.stop_id);
	if (w.start_id != 0)
		g_source_remove(w.start_id);
	/* Jobs still here (a stop that took too long): their encoders are
	 * killed and their directories removed.  A job whose thread has not
	 * returned, as each one given up, is not freed: its thread and that
	 * thread's completion still point at it, and the process ends now (the
	 * next start removes what they leave in the scratch directory). */
	for (i = 0; i < w.jobs->len; i++) {
		Job *job = g_ptr_array_index(w.jobs, i);

		job->fenced = TRUE;
		job_remove_timers(job);
		g_cancellable_cancel(job->run->cancel);
		axis_worker_work_remove_dir(job->run->scratch);
		if (job->thread_done) {
			axis_worker_run_job_unref(job->run);
			g_free(job);
		}
	}
	if (w.abandoned->len > 0)
		g_message("exiting with the threads of %u jobs given up still blocked", w.abandoned->len);
	axis_worker_client_free(w.client);
	g_ptr_array_unref(w.jobs);
	g_ptr_array_unref(w.abandoned);
	g_ptr_array_unref(w.proofs);
	g_ptr_array_unref(w.pending_proofs);
	g_ptr_array_unref(w.later);
	/* The records of checks still running stay (their threads use them);
	 * the process ends now. */
	if (g_hash_table_size(w.probes) > 0)
		g_message("exiting with the checks of %u volume(s) that do not answer still running",
			g_hash_table_size(w.probes));
	g_hash_table_unref(w.probes);
	g_hash_table_unref(w.refused);
	axis_encoder_capabilities_free(w.caps);
	axis_worker_registered_free(w.registered);
	g_free(w.paused_reason);
	g_free(w.held_paused);
	g_main_loop_unref(w.loop);
	if (w.status == 0)
		g_message("stopped");
	return w.status;
}
