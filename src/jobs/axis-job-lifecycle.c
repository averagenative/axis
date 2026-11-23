/*
 * axis-job-lifecycle.c - the job lifecycle, one conditional statement each
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every statement that changes a job names the state it expects (and, for
 * a running job, the lease token) in its WHERE clause.  Under READ
 * COMMITTED a row that another transaction changed meanwhile is re-read
 * and the WHERE clause re-checked against its latest version before it is
 * updated, so two of these can never both act on one version of a job:
 * whichever commits second finds the job no longer as it expects and
 * changes nothing.  The SET expressions read the version being updated,
 * never an older snapshot.
 *
 * The NOTIFY is part of the same statement (pg_notify() in the outer
 * SELECT over the RETURNING rows), so an event exists exactly when the
 * change does.
 */

#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-private.h"
#include "db/axis-db-lock.h"

#include <math.h>
#include <string.h>

/* The NOTIFY payload for the row @r (see axis-job-lifecycle.h).  Names are
 * at most 63 characters and the message at most 500 (so at most about 3
 * KB of JSON with every character escaped), well inside NOTIFY's 8000
 * bytes: a payload that is too long would fail the whole statement. */
#define EVENT_JSON(type, r, message) \
	"json_build_object('type', '" type "', 'id', " r ".id, 'kind', " r ".kind," \
	" 'queue', " r ".queue, 'state', " r ".state, 'attempts', " r ".attempts," \
	" 'max_attempts', " r ".max_attempts, 'runnable', " r ".run_at <= now()," \
	" 'progress', " r ".progress, 'message', left(" message ", 500))::text"

#define NOTIFY(type, r, message) \
	"pg_notify('" AXIS_JOB_CHANNEL "', " EVENT_JSON(type, r, message) ")"

/* What RETURNING must give for EVENT_JSON. */
#define EVENT_COLUMNS "id, kind, queue, state, attempts, max_attempts, run_at, progress"

/* The partial unique index job_dedupe_key_idx, as an ON CONFLICT target. */
#define DEDUPE_TARGET \
	"ON CONFLICT (dedupe_key) WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running')"

/*
 * A new job, or (DO UPDATE) the queued or running job holding the key.  DO
 * UPDATE rather than DO NOTHING because it returns the existing row even
 * when that row was inserted by a transaction that committed after this
 * statement's snapshot was taken (two enqueues of one key at once): a
 * follow-up SELECT in the same statement could not see it.  The update
 * only counts the duplicate, which also tells the two cases apart.
 */
static const gchar enqueue_sql[] =
	"WITH ins AS ("
	" INSERT INTO job (kind, queue, payload, priority, run_at, dedupe_key, max_attempts)"
	" VALUES ($1, $2, $3::jsonb, $4::integer,"
	"  now() + $5::bigint * interval '1 millisecond', $6, $7::integer) "
	DEDUPE_TARGET
	" DO UPDATE SET duplicates = job.duplicates + 1"
	" RETURNING " EVENT_COLUMNS ", duplicates = 0 AS created"
	")"
	" SELECT id, created, CASE WHEN created THEN " NOTIFY("state", "ins", "NULL::text") " END"
	" FROM ins";

/*
 * The fence (AxisJobFence): true when no fence is given (its pid NULL), or
 * when the session of that server process and that start holds that
 * advisory lock in this database now.  pg_locks shows a lock taken with one
 * bigint key as classid (the high 32 bits) and objid (the low 32), with
 * objsubid 1; the start comes from pg_stat_activity, because process ids
 * repeat after a server restart.  The subquery does not depend on the row,
 * so it is evaluated once per statement.
 */
#define FENCE(pid, start, classid, objid) \
	"(" pid "::integer IS NULL OR EXISTS (SELECT 1 FROM pg_locks fl JOIN pg_stat_activity fa ON fa.pid = fl.pid" \
	" WHERE fl.locktype = 'advisory' AND fl.granted AND fl.objsubid = 1 AND fl.pid = " pid "::integer" \
	"  AND " AXIS_DB_BACKEND_START_SQL("fa") " = " start "::text" \
	"  AND fl.classid = " classid "::bigint::oid AND fl.objid = " objid "::bigint::oid" \
	"  AND fl.database = (SELECT oid FROM pg_database WHERE datname = current_database())))"

/*
 * The claim.  The inner SELECT walks job_claim_idx in claim order and
 * locks the first row nobody else has locked whose latest version still
 * matches (LockRows sits below the LIMIT, so a skipped or changed row
 * makes it fetch the next); the UPDATE then changes that locked row.
 */
static const gchar claim_sql[] =
	"WITH next AS ("
	" SELECT id FROM job"
	" WHERE state = 'queued' AND queue = $1 AND run_at <= now() AND kind = ANY ($2::text[])"
	"  AND " FENCE("$5", "$6", "$7", "$8")
	" ORDER BY priority DESC, run_at, id"
	" LIMIT 1"
	" FOR UPDATE SKIP LOCKED"
	"), claimed AS ("
	" UPDATE job SET state = 'running', lease_token = gen_random_uuid(),"
	"  lease_expires_at = now() + $4::bigint * interval '1 millisecond',"
	"  attempts = job.attempts + 1, started_at = now(), heartbeat_at = now(),"
	"  worker_id = $3, progress = NULL, progress_message = NULL"
	" FROM next WHERE job.id = next.id AND job.state = 'queued'"
	" RETURNING job.id, job.kind, job.queue, job.state, job.payload, job.priority,"
	"  job.attempts, job.max_attempts, job.run_at, job.progress, job.lease_token,"
	"  job.cancel_requested"
	")"
	" SELECT id, kind, queue, payload::text, priority, attempts, max_attempts,"
	"  lease_token::text, cancel_requested, " NOTIFY("state", "claimed", "NULL::text")
	" FROM claimed";

#define HELD "id = $1::bigint AND lease_token = $2::uuid AND state = 'running'"

/* Always one row: whether the fence held, then the renewal's, if any; so a
 * renewal that the fence refused is told from one whose lease is gone. */
static const gchar heartbeat_sql[] =
	"WITH f AS (SELECT " FENCE("$7", "$8", "$9", "$10") " AS ok),"
	" hb AS ("
	" UPDATE job SET lease_expires_at = now() + $3::bigint * interval '1 millisecond',"
	"  heartbeat_at = now(),"
	"  progress = CASE WHEN $4::boolean THEN $5::double precision ELSE progress END,"
	"  progress_message = CASE WHEN $4::boolean THEN $6::text ELSE progress_message END"
	" WHERE " HELD " AND (SELECT ok FROM f)"
	" RETURNING " EVENT_COLUMNS ", progress_message, cancel_requested"
	")"
	" SELECT f.ok, hb.cancel_requested,"
	"  CASE WHEN $4::boolean AND hb.id IS NOT NULL THEN " NOTIFY("progress", "hb", "hb.progress_message") " END"
	" FROM f LEFT JOIN hb ON true";

static const gchar succeed_sql[] =
	"WITH done AS ("
	" UPDATE job SET state = 'succeeded', finished_at = now(),"
	"  lease_token = NULL, lease_expires_at = NULL"
	" WHERE " HELD
	" RETURNING " EVENT_COLUMNS
	")"
	" SELECT " NOTIFY("state", "done", "NULL::text") " FROM done";

/* Ends the job when cancelled, permanent, or out of attempts; all three
 * CASEs read the row as it was before this update. */
#define FAIL_ENDS "(cancel_requested OR $4::boolean OR attempts >= max_attempts)"

static const gchar fail_sql[] =
	"WITH f AS ("
	" UPDATE job SET"
	"  state = CASE WHEN cancel_requested THEN 'cancelled'::job_state"
	"   WHEN $4::boolean OR attempts >= max_attempts THEN 'failed'::job_state"
	"   ELSE 'queued'::job_state END,"
	"  run_at = CASE WHEN " FAIL_ENDS " THEN run_at"
	"   ELSE now() + least($6::double precision,"
	"    $5::double precision * power(2::double precision, least(attempts - 1, 40)))"
	"    * (0.5 + random() * 0.5) * interval '1 millisecond' END,"
	"  finished_at = CASE WHEN " FAIL_ENDS " THEN now() END,"
	"  last_error = $3::text, lease_token = NULL, lease_expires_at = NULL"
	" WHERE " HELD
	" RETURNING " EVENT_COLUMNS ", last_error"
	")"
	" SELECT state::text, " NOTIFY("state", "f", "f.last_error") " FROM f";

static const gchar hand_back_sql[] =
	"WITH hb AS ("
	" UPDATE job SET"
	"  state = CASE WHEN cancel_requested THEN 'cancelled'::job_state"
	"   ELSE 'queued'::job_state END,"
	"  attempts = CASE WHEN cancel_requested THEN attempts ELSE attempts - 1 END,"
	"  finished_at = CASE WHEN cancel_requested THEN now() END,"
	"  lease_token = NULL, lease_expires_at = NULL"
	" WHERE " HELD
	" RETURNING " EVENT_COLUMNS
	")"
	" SELECT state::text, " NOTIFY("state", "hb", "NULL::text") " FROM hb";

/*
 * Cancel.  One UPDATE over both cases, so that a job claimed between this
 * statement's snapshot and its update is re-read as running and gets a
 * cancel request rather than being missed.  When nothing was updated, the
 * snapshot tells "finished" from "no such job": a job that changed
 * concurrently out of queued or running has finished.
 */
static const gchar cancel_sql[] =
	"WITH c AS ("
	" UPDATE job SET"
	"  state = CASE WHEN state = 'queued' THEN 'cancelled'::job_state ELSE state END,"
	"  cancel_requested = true,"
	"  finished_at = CASE WHEN state = 'queued' THEN now() ELSE finished_at END"
	" WHERE id = $1::bigint AND state IN ('queued', 'running')"
	" RETURNING " EVENT_COLUMNS
	")"
	" SELECT"
	"  (SELECT state::text FROM c),"
	"  (SELECT CASE WHEN c.state = 'cancelled'"
	"    THEN " NOTIFY("state", "c", "NULL::text")
	"    ELSE " NOTIFY("cancel-requested", "c", "NULL::text") " END FROM c),"
	"  EXISTS (SELECT 1 FROM job WHERE id = $1::bigint)";

#define SWEEP_ENDS "(cancel_requested OR attempts >= max_attempts)"

static const gchar sweep_sql[] =
	"WITH s AS ("
	" UPDATE job SET"
	"  state = CASE WHEN cancel_requested THEN 'cancelled'::job_state"
	"   WHEN attempts >= max_attempts THEN 'failed'::job_state"
	"   ELSE 'queued'::job_state END,"
	"  run_at = CASE WHEN " SWEEP_ENDS " THEN run_at ELSE now() END,"
	"  finished_at = CASE WHEN " SWEEP_ENDS " THEN now() END,"
	"  last_error = CASE WHEN cancel_requested THEN last_error"
	"   ELSE 'the lease of worker ' || coalesce(worker_id, '(unknown)')"
	"    || ' expired: it stopped renewing it' END,"
	"  lease_token = NULL, lease_expires_at = NULL"
	" WHERE state = 'running' AND lease_expires_at < now() AND kind = ANY ($1::text[])"
	"  AND coalesce(worker_id, '') <> ALL ($2::text[])"
	"  AND " FENCE("$3", "$4", "$5", "$6")
	" RETURNING " EVENT_COLUMNS ", last_error, worker_id"
	")"
	" SELECT id, kind, state::text, attempts, max_attempts, coalesce(worker_id, '(unknown)'),"
	"  " NOTIFY("state", "s", "s.last_error")
	" FROM s ORDER BY id";

/* The jobs another run of an in-process daemon left running, whatever
 * their leases: as the sweep, for another reason.  At least one row, the
 * first column whether the fence held (the rest NULL when nothing was
 * taken back). */
static const gchar take_back_sql[] =
	"WITH f AS (SELECT " FENCE("$3", "$4", "$5", "$6") " AS ok),"
	" s AS ("
	" UPDATE job SET"
	"  state = CASE WHEN cancel_requested THEN 'cancelled'::job_state"
	"   WHEN attempts >= max_attempts THEN 'failed'::job_state"
	"   ELSE 'queued'::job_state END,"
	"  run_at = CASE WHEN " SWEEP_ENDS " THEN run_at ELSE now() END,"
	"  finished_at = CASE WHEN " SWEEP_ENDS " THEN now() END,"
	"  last_error = CASE WHEN cancel_requested THEN last_error"
	"   ELSE 'taken back from ' || split_part(worker_id, '/', 1)"
	"    || ', which no longer runs: the daemon that holds the database took it back' END,"
	"  lease_token = NULL, lease_expires_at = NULL"
	" WHERE state = 'running' AND kind = ANY ($1::text[])"
	"  AND split_part(worker_id, '/', 1) ~ '" AXIS_JOB_DAEMON_ID_PATTERN "'"
	"  AND split_part(worker_id, '/', 1) <> $2::text"
	"  AND (SELECT ok FROM f)"
	" RETURNING " EVENT_COLUMNS ", last_error, worker_id"
	")"
	" SELECT f.ok, s.id, s.kind, s.state::text, s.attempts, s.max_attempts, split_part(s.worker_id, '/', 1),"
	"  CASE WHEN s.id IS NOT NULL THEN " NOTIFY("state", "s", "s.last_error") " END"
	" FROM f LEFT JOIN s ON true ORDER BY s.id";

/*
 * Remote workers.  The claim is claim_sql with what the worker has shown
 * it can do as conditions on the payload's "requires"; the rest hold a job
 * only for its worker, and only while its lease has not run out.
 */
static const gchar claim_remote_sql[] =
	"WITH next AS ("
	" SELECT j.id FROM job j"
	" WHERE j.state = 'queued' AND j.queue = $1 AND j.run_at <= now() AND j.kind = ANY ($2::text[])"
	"  AND (j.payload->'requires'->>'worker' IS NULL OR j.payload->'requires'->>'worker' = $5)"
	"  AND (j.payload->'requires'->>'prefer_not' IS NULL OR j.payload->'requires'->>'prefer_not' <> $5"
	"   OR CASE WHEN (j.payload->'requires'->>'prefer_not_until') ~ '^[0-9]{1,12}$'"
	"    THEN (j.payload->'requires'->>'prefer_not_until')::bigint ELSE 0 END <= extract(epoch FROM now()))"
	"  AND (j.payload->'requires'->>'encoder' IS NULL OR EXISTS ("
	"   SELECT 1 FROM encoder_capability c"
	"   WHERE c.machine = $6 AND c.encoder = j.payload->'requires'->>'encoder' AND c.usable"
	"    AND (coalesce(j.payload->'requires'->>'ten_bit', 'false') <> 'true' OR c.ten_bit)))"
	"  AND coalesce(j.payload->'requires'->'volumes', '[]'::jsonb) <@ $7::jsonb"
	"  AND " FENCE("$8", "$9", "$10", "$11")
	" ORDER BY j.priority DESC, j.run_at, j.id"
	" LIMIT 1"
	" FOR UPDATE OF j SKIP LOCKED"
	"), claimed AS ("
	" UPDATE job SET state = 'running', lease_token = gen_random_uuid(),"
	"  lease_expires_at = now() + $4::bigint * interval '1 millisecond',"
	"  attempts = job.attempts + 1, started_at = now(), heartbeat_at = now(),"
	"  worker_id = $3, progress = NULL, progress_message = NULL"
	" FROM next WHERE job.id = next.id AND job.state = 'queued'"
	" RETURNING job.id, job.kind, job.queue, job.state, job.payload, job.priority,"
	"  job.attempts, job.max_attempts, job.run_at, job.progress, job.lease_token,"
	"  job.cancel_requested"
	")"
	" SELECT id, kind, queue, payload::text, priority, attempts, max_attempts,"
	"  lease_token::text, cancel_requested, " NOTIFY("state", "claimed", "NULL::text")
	" FROM claimed";

#define HELD_REMOTE HELD " AND worker_id = $7::text AND lease_expires_at > now()"

static const gchar heartbeat_remote_sql[] =
	"WITH hb AS ("
	" UPDATE job SET lease_expires_at = now() + $3::bigint * interval '1 millisecond',"
	"  heartbeat_at = now(),"
	"  progress = CASE WHEN $4::boolean THEN $5::double precision ELSE progress END,"
	"  progress_message = CASE WHEN $4::boolean THEN $6::text ELSE progress_message END"
	" WHERE " HELD_REMOTE
	" RETURNING " EVENT_COLUMNS ", progress_message, cancel_requested"
	")"
	" SELECT cancel_requested,"
	"  CASE WHEN $4::boolean THEN " NOTIFY("progress", "hb", "hb.progress_message") " END"
	" FROM hb";

static const gchar lock_remote_sql[] =
	"SELECT kind, payload::text FROM job"
	" WHERE id = $1::bigint AND lease_token = $2::uuid AND state = 'running'"
	"  AND worker_id = $3::text AND lease_expires_at > now()"
	" FOR UPDATE";

/* As the heartbeat: always one row, whether the fence held first. */
static const gchar expire_remote_sql[] =
	"WITH f AS (SELECT " FENCE("$3", "$4", "$5", "$6") " AS ok),"
	" s AS ("
	" UPDATE job SET"
	"  state = CASE WHEN cancel_requested THEN 'cancelled'::job_state"
	"   WHEN attempts >= max_attempts THEN 'failed'::job_state"
	"   ELSE 'queued'::job_state END,"
	"  run_at = CASE WHEN " SWEEP_ENDS " THEN run_at ELSE now() END,"
	"  finished_at = CASE WHEN " SWEEP_ENDS " THEN now() END,"
	"  last_error = CASE WHEN cancel_requested THEN last_error"
	"   ELSE 'the lease of worker ' || coalesce(worker_id, '(unknown)')"
	"    || ' expired: it stopped renewing it' END,"
	"  lease_token = NULL, lease_expires_at = NULL"
	" WHERE " HELD " AND (SELECT ok FROM f)"
	" RETURNING " EVENT_COLUMNS ", last_error, worker_id"
	")"
	" SELECT s.state::text, s.attempts, s.max_attempts, coalesce(s.worker_id, '(unknown)'),"
	"  CASE WHEN s.id IS NOT NULL THEN " NOTIFY("state", "s", "s.last_error") " END, f.ok"
	" FROM f LEFT JOIN s ON true";

/*
 * Retry: a failed or cancelled job queued again as itself (its id, payload,
 * key, priority and attempt limit), its attempts counted from none, on the
 * queue its kind is claimed from now ($2, $3: the known kinds and their
 * queues); how its last run ended goes to earlier_runs, the oldest dropped
 * beyond AXIS_JOB_EARLIER_RUNS_KEPT.  Not while another job of its key is
 * queued or running (one that commits after this statement's snapshot makes
 * the update break job_dedupe_key_idx instead).  Always one row: the job as
 * it was (NULL state: no such job), whether its kind is known, the live job
 * of its key, then the retried job's id if it was retried.
 */
static const gchar retry_sql[] =
	"WITH known AS ("
	" SELECT * FROM unnest($2::text[], $3::text[]) AS k (kind, queue)"
	"), j AS ("
	" SELECT job.id, job.state, job.dedupe_key, known.queue FROM job LEFT JOIN known ON known.kind = job.kind"
	" WHERE job.id = $1::bigint"
	"), live AS ("
	" SELECT o.id FROM job o, j WHERE o.dedupe_key = j.dedupe_key AND o.id <> j.id"
	"  AND o.state IN ('queued', 'running') ORDER BY o.id LIMIT 1"
	"), r AS ("
	" UPDATE job SET state = 'queued', queue = j.queue, attempts = 0, run_at = now(), finished_at = NULL,"
	"  cancel_requested = false, progress = NULL, progress_message = NULL, last_error = NULL,"
	"  started_at = NULL, heartbeat_at = NULL, worker_id = NULL,"
	"  earlier_runs = (CASE WHEN jsonb_array_length(job.earlier_runs) >= " G_STRINGIFY(AXIS_JOB_EARLIER_RUNS_KEPT)
	"   THEN job.earlier_runs - 0 ELSE job.earlier_runs END)"
	"   || jsonb_build_array(jsonb_build_object('state', job.state, 'attempts', job.attempts,"
	"    'error', left(job.last_error, " G_STRINGIFY(AXIS_JOB_EARLIER_ERROR_CHARS) "),"
	"    'worker', job.worker_id, 'started_at', job.started_at,"
	"    'finished_at', job.finished_at, 'retried_at', now(), 'retried_by', $4::text))"
	" FROM j WHERE job.id = j.id AND j.queue IS NOT NULL AND job.state IN ('failed', 'cancelled')"
	"  AND NOT EXISTS (SELECT 1 FROM live)"
	" RETURNING job.id, job.kind, job.queue, job.state, job.attempts, job.max_attempts, job.run_at, job.progress"
	")"
	" SELECT (SELECT state::text FROM j), (SELECT queue IS NOT NULL FROM j), (SELECT id FROM live), r.id,"
	"  CASE WHEN r.id IS NOT NULL THEN " NOTIFY("state", "r", "NULL::text") " END"
	" FROM (SELECT 1) AS one LEFT JOIN r ON true";

/* The queued or running job of a key, after a retry broke its index. */
static const gchar live_of_sql[] =
	"SELECT o.id FROM job o JOIN job j ON o.dedupe_key = j.dedupe_key AND o.id <> j.id"
	" WHERE j.id = $1::bigint AND o.state IN ('queued', 'running') ORDER BY o.id LIMIT 1";

/* Unfinished jobs of the known kinds ($1, $2: kinds and their queues) on
 * another queue than their kind's, moved to it; one row per kind and queue
 * left. */
static const gchar move_to_queues_sql[] =
	"WITH known AS ("
	" SELECT * FROM unnest($1::text[], $2::text[]) AS k (kind, queue)"
	"), m AS ("
	" SELECT job.id, job.queue AS was, known.queue FROM job JOIN known ON known.kind = job.kind"
	" WHERE job.state IN ('queued', 'running') AND job.queue <> known.queue"
	" FOR UPDATE OF job"
	"), u AS ("
	" UPDATE job SET queue = m.queue FROM m WHERE job.id = m.id AND job.state IN ('queued', 'running')"
	" RETURNING job.kind, m.was, job.queue, job.state"
	")"
	" SELECT kind, was, queue, count(*) FILTER (WHERE state = 'queued'),"
	"  count(*) FILTER (WHERE state = 'running')"
	" FROM u GROUP BY kind, was, queue ORDER BY kind, was";

/* Queued jobs of kinds this daemon has no handler for ($1: the kinds it
 * has), failed for good so that their keys are free; one row per kind. */
static const gchar fail_unknown_sql[] =
	"WITH f AS ("
	" UPDATE job SET state = 'failed', finished_at = now(),"
	"  last_error = 'this build of Axis has no handler for its kind, ' || kind || ': failed when it started,"
	" so that its de-duplication key (' || coalesce(dedupe_key, 'none') || ') is free; a build that has the"
	" kind can retry it'"
	" WHERE state = 'queued' AND kind <> ALL ($1::text[])"
	" RETURNING kind"
	")"
	" SELECT kind, count(*) FROM f GROUP BY kind ORDER BY kind";

static const gchar prune_sql[] =
	"DELETE FROM job"
	" WHERE (state IN ('succeeded', 'cancelled')"
	"   AND finished_at < now() - $1::bigint * interval '1 millisecond')"
	"  OR (state = 'failed' AND finished_at < now() - $2::bigint * interval '1 millisecond')";

/* The earlier runs of the jobs left that ended longer ago than a failed job
 * is kept ($1), dropped; the rest in their order. */
#define EARLIER_ENDED "coalesce((e->>'finished_at')::timestamptz, (e->>'retried_at')::timestamptz)"

static const gchar prune_earlier_sql[] =
	"UPDATE job SET earlier_runs = coalesce((SELECT jsonb_agg(e ORDER BY n)"
	"  FROM jsonb_array_elements(job.earlier_runs) WITH ORDINALITY AS x (e, n)"
	"  WHERE " EARLIER_ENDED " >= now() - $1::bigint * interval '1 millisecond'), '[]'::jsonb)"
	" WHERE earlier_runs <> '[]'::jsonb AND EXISTS (SELECT 1 FROM jsonb_array_elements(job.earlier_runs) AS x (e)"
	"  WHERE " EARLIER_ENDED " IS NULL OR " EARLIER_ENDED " < now() - $1::bigint * interval '1 millisecond')";

static const gchar *const state_names[] = {
	"queued", "running", "succeeded", "failed", "cancelled"
};

const gchar *
axis_job_state_to_string(AxisJobState state)
{
	if ((guint)state >= G_N_ELEMENTS(state_names))
		return "unknown";

	return state_names[state];
}

gboolean
axis_job_state_from_string(
	const gchar  *text,
	AxisJobState *state
){
	guint i;

	if (text == NULL)
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(state_names); i++) {
		if (strcmp(text, state_names[i]) == 0) {
			*state = (AxisJobState)i;
			return TRUE;
		}
	}

	return FALSE;
}

gboolean
axis_job_name_is_valid(const gchar *name)
{
	const gchar *p;

	if (name == NULL || name[0] < 'a' || name[0] > 'z')
		return FALSE;
	if (strlen(name) > AXIS_JOB_NAME_MAX_LEN)
		return FALSE;

	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      *p == '.' || *p == '_' || *p == '-'))
			return FALSE;
	}

	return TRUE;
}

/*
 * Shared helpers (axis-job-private.h)
 */

gchar *
axis_job_text_array(const gchar *const *items)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; items != NULL && items[i] != NULL; i++) {
		const gchar *p;

		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, '"');
		for (p = items[i]; *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(out, '\\');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '}');

	return g_string_free(out, FALSE);
}

gchar *
axis_job_clean_text(
	const gchar *text,
	guint        max_chars
){
	g_autofree gchar *valid = NULL;

	if (text == NULL)
		return NULL;

	valid = g_utf8_make_valid(text, -1);
	if (g_utf8_strlen(valid, -1) <= (glong)max_chars)
		return g_steal_pointer(&valid);

	return g_utf8_substring(valid, 0, (glong)max_chars);
}

gchar *
axis_job_payload_to_text(
	JsonObject  *payload,
	GError     **error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	gchar *text;
	gsize length = 0;

	if (payload == NULL)
		return g_strdup("{}");

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, payload);
	generator = json_generator_new();
	json_generator_set_root(generator, node);
	text = json_generator_to_data(generator, &length);

	if (length > AXIS_JOB_MAX_PAYLOAD_BYTES) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"the payload is %" G_GSIZE_FORMAT " bytes of JSON; at most %d are allowed",
			length, AXIS_JOB_MAX_PAYLOAD_BYTES);
		g_free(text);
		return NULL;
	}

	return text;
}

/*
 * Arguments
 */

static gboolean
check_name(
	const gchar  *what,
	const gchar  *name,
	GError      **error
){
	if (axis_job_name_is_valid(name))
		return TRUE;

	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
		"invalid %s name '%s' (use 1 to %d lower-case letters, digits, '.', '_' and '-', "
		"starting with a letter)", what, name != NULL ? name : "(null)", AXIS_JOB_NAME_MAX_LEN);
	return FALSE;
}

static gboolean
check_kinds(
	const gchar *const  *kinds,
	GError             **error
){
	guint i;

	if (kinds == NULL) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"no list of kinds was given");
		return FALSE;
	}
	for (i = 0; kinds[i] != NULL; i++) {
		if (!check_name("kind", kinds[i], error))
			return FALSE;
	}

	return TRUE;
}

/* A token that is not a UUID cannot be anyone's lease. */
static gboolean
check_token(
	gint64        id,
	const gchar  *lease_token,
	GError      **error
){
	if (lease_token != NULL && g_uuid_string_is_valid(lease_token))
		return TRUE;

	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST,
		"job %" G_GINT64_FORMAT " is not held under lease token '%s'",
		id, lease_token != NULL ? lease_token : "(null)");
	return FALSE;
}

static void
set_lease_lost(
	GError **error,
	gint64   id
){
	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST,
		"job %" G_GINT64_FORMAT " is no longer held under this lease (it expired and the job "
		"was returned to the queue, it was handed back, or the job has finished)", id);
}

/* A fence's four parameters (pid, start, classid, objid); all NULL for
 * none.  The strings are written to @buffers (or point into @fence), which
 * must outlive the call. */
static void
fence_params(
	const AxisJobFence  *fence,
	gchar                buffers[3][24],
	const gchar        **params
){
	guint64 bits;

	if (fence == NULL) {
		params[0] = NULL;
		params[1] = NULL;
		params[2] = NULL;
		params[3] = NULL;
		return;
	}

	bits = (guint64)fence->lock_key;
	g_snprintf(buffers[0], 24, "%d", fence->holder_pid);
	g_snprintf(buffers[1], 24, "%u", (guint)(bits >> 32));
	g_snprintf(buffers[2], 24, "%u", (guint)(bits & G_GUINT64_CONSTANT(0xffffffff)));
	params[0] = buffers[0];
	params[1] = fence->holder_start;
	params[2] = buffers[1];
	params[3] = buffers[2];
}

static gboolean
parse_uint(
	AxisDbResult *result,
	guint         row,
	guint         column,
	guint        *value
){
	gint64 v;

	if (!axis_db_result_get_int64(result, row, column, &v) || v < 0 || v > G_MAXUINT)
		return FALSE;

	*value = (guint)v;
	return TRUE;
}

static void
set_bad_result(
	GError      **error,
	const gchar  *what
){
	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
		"unexpected result from the database for %s", what);
}

/*
 * Enqueue
 */

gboolean
axis_job_enqueue(
	AxisDbConn         *conn,
	const AxisJobSpec  *spec,
	gint64             *id,
	gboolean           *created,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *payload = NULL;
	g_autofree gchar *priority = NULL;
	g_autofree gchar *delay = NULL;
	g_autofree gchar *max_attempts = NULL;
	const gchar *params[7];
	gint64 new_id = 0;
	gboolean was_created = FALSE;

	g_return_val_if_fail(conn != NULL, FALSE);
	g_return_val_if_fail(spec != NULL, FALSE);

	if (!check_name("kind", spec->kind, error) || !check_name("queue", spec->queue, error))
		return FALSE;
	if (spec->max_attempts < 1 || spec->max_attempts > AXIS_JOB_MAX_ATTEMPTS_LIMIT) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"max_attempts must be from 1 to %d, not %u",
			AXIS_JOB_MAX_ATTEMPTS_LIMIT, spec->max_attempts);
		return FALSE;
	}
	if (spec->dedupe_key != NULL &&
	    (*spec->dedupe_key == '\0' || strlen(spec->dedupe_key) > AXIS_JOB_MAX_DEDUPE_KEY_LEN ||
	     !g_utf8_validate(spec->dedupe_key, -1, NULL))) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"a de-duplication key must be 1 to %d bytes of UTF-8",
			AXIS_JOB_MAX_DEDUPE_KEY_LEN);
		return FALSE;
	}

	payload = axis_job_payload_to_text(spec->payload, error);
	if (payload == NULL)
		return FALSE;
	priority = g_strdup_printf("%d", spec->priority);
	delay = g_strdup_printf("%" G_GINT64_FORMAT, MAX(spec->delay_ms, 0));
	max_attempts = g_strdup_printf("%u", spec->max_attempts);

	params[0] = spec->kind;
	params[1] = spec->queue;
	params[2] = payload;
	params[3] = priority;
	params[4] = delay;
	params[5] = spec->dedupe_key;
	params[6] = max_attempts;

	if (!axis_db_conn_exec(conn, enqueue_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) != 1 ||
	    !axis_db_result_get_int64(result, 0, 0, &new_id) ||
	    !axis_db_result_get_boolean(result, 0, 1, &was_created)) {
		set_bad_result(error, "an enqueue");
		return FALSE;
	}

	if (id != NULL)
		*id = new_id;
	if (created != NULL)
		*created = was_created;
	return TRUE;
}

/*
 * Claim
 */

void
axis_job_claim_free(AxisJobClaim *claim)
{
	if (claim == NULL)
		return;

	g_free(claim->kind);
	g_free(claim->queue);
	g_free(claim->payload);
	g_free(claim->lease_token);
	g_free(claim);
}

gboolean
axis_job_claim(
	AxisDbConn          *conn,
	const gchar         *queue,
	const gchar *const  *kinds,
	const gchar         *worker_id,
	guint                lease_ms,
	AxisJobClaim       **claim,
	GError             **error
){
	return axis_job_claim_fenced(conn, queue, kinds, worker_id, lease_ms, NULL, claim, error);
}

gboolean
axis_job_claim_fenced(
	AxisDbConn          *conn,
	const gchar         *queue,
	const gchar *const  *kinds,
	const gchar         *worker_id,
	guint                lease_ms,
	const AxisJobFence  *fence,
	AxisJobClaim       **claim,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisJobClaim) out = NULL;
	g_autofree gchar *kinds_array = NULL;
	g_autofree gchar *lease = NULL;
	gchar fence_text[3][24];
	const gchar *params[8];
	gint64 priority = 0;

	g_return_val_if_fail(conn != NULL, FALSE);
	g_return_val_if_fail(claim != NULL, FALSE);

	*claim = NULL;

	if (!check_name("queue", queue, error) || !check_kinds(kinds, error))
		return FALSE;
	if (worker_id == NULL || *worker_id == '\0' || strlen(worker_id) > AXIS_JOB_MAX_WORKER_ID_LEN ||
	    !g_utf8_validate(worker_id, -1, NULL)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"a worker id must be 1 to %d bytes of UTF-8", AXIS_JOB_MAX_WORKER_ID_LEN);
		return FALSE;
	}
	if (lease_ms == 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"a lease must be longer than 0 ms");
		return FALSE;
	}
	if (kinds[0] == NULL)
		return TRUE;	/* nothing it could run */

	kinds_array = axis_job_text_array(kinds);
	lease = g_strdup_printf("%u", lease_ms);
	params[0] = queue;
	params[1] = kinds_array;
	params[2] = worker_id;
	params[3] = lease;
	fence_params(fence, fence_text, &params[4]);

	if (!axis_db_conn_exec(conn, claim_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;

	out = g_new0(AxisJobClaim, 1);
	if (axis_db_result_get_n_rows(result) != 1 ||
	    !axis_db_result_get_int64(result, 0, 0, &out->id) ||
	    !axis_db_result_get_int64(result, 0, 4, &priority) ||
	    !parse_uint(result, 0, 5, &out->attempt) ||
	    !parse_uint(result, 0, 6, &out->max_attempts) ||
	    !axis_db_result_get_boolean(result, 0, 8, &out->cancel_requested) ||
	    axis_db_result_is_null(result, 0, 7)) {
		set_bad_result(error, "a claim");
		return FALSE;
	}
	out->kind = g_strdup(axis_db_result_get_text(result, 0, 1));
	out->queue = g_strdup(axis_db_result_get_text(result, 0, 2));
	out->payload = g_strdup(axis_db_result_get_text(result, 0, 3));
	out->priority = (gint)priority;
	out->lease_token = g_strdup(axis_db_result_get_text(result, 0, 7));

	*claim = g_steal_pointer(&out);
	return TRUE;
}

gboolean
axis_job_claim_any(
	AxisDbConn          *conn,
	const gchar *const  *queues,
	const gchar *const  *kinds,
	const gchar         *worker_id,
	guint                lease_ms,
	AxisJobClaim       **claim,
	GError             **error
){
	guint i;

	g_return_val_if_fail(claim != NULL, FALSE);

	*claim = NULL;
	if (queues == NULL) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"no list of queues was given");
		return FALSE;
	}

	for (i = 0; queues[i] != NULL; i++) {
		if (!axis_job_claim(conn, queues[i], kinds, worker_id, lease_ms, claim, error))
			return FALSE;
		if (*claim != NULL)
			break;
	}

	return TRUE;
}

/*
 * Running jobs
 */

gboolean
axis_job_heartbeat(
	AxisDbConn             *conn,
	gint64                  id,
	const gchar            *lease_token,
	guint                   lease_ms,
	const AxisJobProgress  *progress,
	gboolean               *cancel_requested,
	GError                **error
){
	return axis_job_heartbeat_fenced(conn, id, lease_token, lease_ms, progress, NULL, cancel_requested, error);
}

gboolean
axis_job_heartbeat_fenced(
	AxisDbConn             *conn,
	gint64                  id,
	const gchar            *lease_token,
	guint                   lease_ms,
	const AxisJobProgress  *progress,
	const AxisJobFence     *fence,
	gboolean               *cancel_requested,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *lease = NULL;
	g_autofree gchar *fraction = NULL;
	g_autofree gchar *message = NULL;
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	gchar fence_text[3][24];
	const gchar *params[10];
	gboolean requested = FALSE;
	gboolean fenced_ok = FALSE;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_token(id, lease_token, error))
		return FALSE;
	if (lease_ms == 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"a lease must be longer than 0 ms");
		return FALSE;
	}

	id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	lease = g_strdup_printf("%u", lease_ms);
	if (progress != NULL) {
		gdouble f = progress->fraction;

		if (isnan(f) || f < 0.0)
			f = 0.0;
		else if (f > 1.0)
			f = 1.0;
		fraction = g_strdup(g_ascii_dtostr(buffer, sizeof(buffer), f));
		message = axis_job_clean_text(progress->message, AXIS_JOB_MAX_PROGRESS_CHARS);
	}

	params[0] = id_text;
	params[1] = lease_token;
	params[2] = lease;
	params[3] = progress != NULL ? "true" : "false";
	params[4] = fraction;
	params[5] = message;
	fence_params(fence, fence_text, &params[6]);

	if (!axis_db_conn_exec(conn, heartbeat_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1 || !axis_db_result_get_boolean(result, 0, 0, &fenced_ok)) {
		set_bad_result(error, "a heartbeat");
		return FALSE;
	}
	if (!fenced_ok) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_FENCED,
			"the lease of job %" G_GINT64_FORMAT " was not renewed: this daemon does not hold the database "
			"lock that its leases depend on", id);
		return FALSE;
	}
	if (axis_db_result_is_null(result, 0, 1)) {
		set_lease_lost(error, id);
		return FALSE;
	}
	if (!axis_db_result_get_boolean(result, 0, 1, &requested)) {
		set_bad_result(error, "a heartbeat");
		return FALSE;
	}

	if (cancel_requested != NULL)
		*cancel_requested = requested;
	return TRUE;
}

/* Runs one of the statements that take (id, token, ...) and end or hand
 * back a running job.  @state_column is the result column holding the new
 * state, or -1. */
static gboolean
run_held(
	AxisDbConn    *conn,
	const gchar   *sql,
	guint          n_params,
	const gchar  **params,
	gint64         id,
	gint           state_column,
	AxisJobState  *state,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	AxisJobState new_state = AXIS_JOB_STATE_QUEUED;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, sql, n_params, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		set_lease_lost(error, id);
		return FALSE;
	}
	if (state_column >= 0 &&
	    !axis_job_state_from_string(axis_db_result_get_text(result, 0, (guint)state_column),
	                                &new_state)) {
		set_bad_result(error, "a job update");
		return FALSE;
	}

	if (state != NULL)
		*state = new_state;
	return TRUE;
}

gboolean
axis_job_succeed(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *lease_token,
	GError      **error
){
	const gchar *params[2];

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_token(id, lease_token, error))
		return FALSE;

	params[1] = lease_token;
	return run_held(conn, succeed_sql, G_N_ELEMENTS(params), params, id, -1, NULL, error);
}

gboolean
axis_job_fail(
	AxisDbConn    *conn,
	gint64         id,
	const gchar   *lease_token,
	const gchar   *message,
	gboolean       permanent,
	guint          backoff_base_ms,
	guint          backoff_cap_ms,
	AxisJobState  *state,
	GError       **error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *cap = NULL;
	const gchar *params[6];

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_token(id, lease_token, error))
		return FALSE;

	text = axis_job_clean_text(message != NULL && *message != '\0' ? message : "failed",
		AXIS_JOB_MAX_ERROR_CHARS);
	base = g_strdup_printf("%u", backoff_base_ms);
	cap = g_strdup_printf("%u", MAX(backoff_cap_ms, backoff_base_ms));

	params[1] = lease_token;
	params[2] = text;
	params[3] = permanent ? "true" : "false";
	params[4] = base;
	params[5] = cap;
	return run_held(conn, fail_sql, G_N_ELEMENTS(params), params, id, 0, state, error);
}

gboolean
axis_job_hand_back(
	AxisDbConn    *conn,
	gint64         id,
	const gchar   *lease_token,
	AxisJobState  *state,
	GError       **error
){
	const gchar *params[2];

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_token(id, lease_token, error))
		return FALSE;

	params[1] = lease_token;
	return run_held(conn, hand_back_sql, G_N_ELEMENTS(params), params, id, 0, state, error);
}

/*
 * Cancel, sweep, prune
 */

gboolean
axis_job_cancel(
	AxisDbConn            *conn,
	gint64                 id,
	AxisJobCancelOutcome  *outcome,
	GError               **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];
	const gchar *state;
	gboolean exists = FALSE;
	AxisJobCancelOutcome out;

	g_return_val_if_fail(conn != NULL, FALSE);

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, cancel_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1 ||
	    !axis_db_result_get_boolean(result, 0, 2, &exists)) {
		set_bad_result(error, "a cancel");
		return FALSE;
	}

	state = axis_db_result_get_text(result, 0, 0);
	if (state == NULL) {
		if (!exists) {
			g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND,
				"there is no job %" G_GINT64_FORMAT, id);
			return FALSE;
		}
		out = AXIS_JOB_CANCEL_TOO_LATE;
	} else if (strcmp(state, "cancelled") == 0) {
		out = AXIS_JOB_CANCEL_DONE;
	} else {
		out = AXIS_JOB_CANCEL_REQUESTED;
	}

	if (outcome != NULL)
		*outcome = out;
	return TRUE;
}

/* The live job of @id's key, or 0, after a retry broke the key's index. */
static gboolean
live_of(
	AxisDbConn  *conn,
	const gchar *id_text,
	gint64      *live,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	*live = 0;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, live_of_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		axis_db_result_get_int64(result, 0, 0, live);
	return TRUE;
}

/* The job's state now, or NULL when there is no job @id. */
static gboolean
state_of(
	AxisDbConn   *conn,
	const gchar  *id_text,
	gchar       **state,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	*state = NULL;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "SELECT state::text FROM job WHERE id = $1::bigint", G_N_ELEMENTS(params), params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*state = g_strdup(axis_db_result_get_text(result, 0, 0));
	return TRUE;
}

gboolean
axis_job_retry(
	AxisDbConn           *conn,
	gint64                id,
	const gchar *const   *kinds,
	const gchar *const   *queues,
	const gchar          *by,
	AxisJobRetryOutcome  *outcome,
	gint64               *live_id,
	GError              **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_autofree gchar *kinds_array = NULL;
	g_autofree gchar *queues_array = NULL;
	g_autofree gchar *who = NULL;
	const gchar *params[4];
	const gchar *state;
	gboolean known = FALSE;
	gint64 live = 0;
	guint attempt;
	AxisJobRetryOutcome out;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (live_id != NULL)
		*live_id = 0;
	if (!check_kinds(kinds, error) || !check_kinds(queues, error))
		return FALSE;
	if (g_strv_length((gchar **)kinds) != g_strv_length((gchar **)queues)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"the lists of kinds and of their queues differ in length");
		return FALSE;
	}

	kinds_array = axis_job_text_array(kinds);
	queues_array = axis_job_text_array(queues);
	who = axis_job_clean_text(by, AXIS_JOB_MAX_PROGRESS_CHARS);
	params[0] = id_text;
	params[1] = kinds_array;
	params[2] = queues_array;
	params[3] = who;
	for (attempt = 0;; attempt++) {
		if (axis_db_conn_exec(conn, retry_sql, G_N_ELEMENTS(params), params, &result, &local))
			break;
		/* Another job of its key became queued after the statement's
		 * snapshot: that one is the live job.  Gone already (it ended
		 * before it could be looked up), the retry is tried once more;
		 * after that, another job did this work just now (live 0). */
		if (!g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_clear_error(&local);
		if (!live_of(conn, id_text, &live, error))
			return FALSE;
		if (live > 0 || attempt >= 1) {
			if (outcome != NULL)
				*outcome = AXIS_JOB_RETRY_LIVE;
			if (live_id != NULL)
				*live_id = live;
			return TRUE;
		}
	}
	if (axis_db_result_get_n_rows(result) != 1) {
		set_bad_result(error, "a retry");
		return FALSE;
	}

	state = axis_db_result_get_text(result, 0, 0);
	if (state == NULL) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND,
			"there is no job %" G_GINT64_FORMAT, id);
		return FALSE;
	}
	axis_db_result_get_boolean(result, 0, 1, &known);
	if (!axis_db_result_is_null(result, 0, 3)) {
		out = AXIS_JOB_RETRY_DONE;
	} else if (strcmp(state, "failed") != 0 && strcmp(state, "cancelled") != 0) {
		out = AXIS_JOB_RETRY_NOT_ENDED;
	} else if (!known) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND,
			"job %" G_GINT64_FORMAT " is of a kind that has no handler here", id);
		return FALSE;
	} else if (!axis_db_result_is_null(result, 0, 2)) {
		out = AXIS_JOB_RETRY_LIVE;
		axis_db_result_get_int64(result, 0, 2, &live);
	} else {
		/* It changed between the snapshot and the update: what it is now
		 * says what happened (pruned, or retried by someone else at the
		 * same moment and now doing the work itself, or done). */
		g_autofree gchar *now = NULL;

		if (!state_of(conn, id_text, &now, error))
			return FALSE;
		if (now == NULL) {
			g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND,
				"there is no job %" G_GINT64_FORMAT, id);
			return FALSE;
		}
		if (strcmp(now, "queued") == 0 || strcmp(now, "running") == 0) {
			out = AXIS_JOB_RETRY_LIVE;
			live = id;
		} else {
			out = AXIS_JOB_RETRY_NOT_ENDED;
		}
	}

	if (outcome != NULL)
		*outcome = out;
	if (live_id != NULL)
		*live_id = live;
	return TRUE;
}

gboolean
axis_job_move_to_kind_queues(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar *const  *queues,
	guint               *n_moved,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kinds_array = NULL;
	g_autofree gchar *queues_array = NULL;
	const gchar *params[2];
	guint total = 0;
	guint i;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (n_moved != NULL)
		*n_moved = 0;
	if (!check_kinds(kinds, error) || !check_kinds(queues, error))
		return FALSE;
	if (g_strv_length((gchar **)kinds) != g_strv_length((gchar **)queues)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"the lists of kinds and of their queues differ in length");
		return FALSE;
	}
	if (kinds[0] == NULL)
		return TRUE;

	kinds_array = axis_job_text_array(kinds);
	queues_array = axis_job_text_array(queues);
	params[0] = kinds_array;
	params[1] = queues_array;
	if (!axis_db_conn_exec(conn, move_to_queues_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		guint queued = 0;
		guint running = 0;

		parse_uint(result, i, 3, &queued);
		parse_uint(result, i, 4, &running);
		total += queued + running;
		g_message("moved %u queued and %u running job(s) of kind '%s' from queue '%s' to '%s', the queue "
			"this build claims that kind from", queued, running, axis_db_result_get_text(result, i, 0),
			axis_db_result_get_text(result, i, 1), axis_db_result_get_text(result, i, 2));
	}

	if (n_moved != NULL)
		*n_moved = total;
	return TRUE;
}

gboolean
axis_job_fail_unknown_kinds(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	guint               *n_failed,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kinds_array = NULL;
	const gchar *params[1];
	guint total = 0;
	guint i;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (n_failed != NULL)
		*n_failed = 0;
	if (!check_kinds(kinds, error))
		return FALSE;

	kinds_array = axis_job_text_array(kinds);
	params[0] = kinds_array;
	if (!axis_db_conn_exec(conn, fail_unknown_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		guint n = 0;

		parse_uint(result, i, 1, &n);
		total += n;
		g_warning("job kind '%s' has no handler in this daemon: its %u queued job(s) are failed, not deleted, "
			"so that the de-duplication keys they held (a schedule's among them) are free; a build that has "
			"the kind can retry them", axis_db_result_get_text(result, i, 0), n);
	}

	if (n_failed != NULL)
		*n_failed = total;
	return TRUE;
}

gboolean
axis_job_sweep_expired_leases(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar *const  *except_workers,
	guint               *n_swept,
	GError             **error
){
	return axis_job_sweep_expired_leases_fenced(conn, kinds, except_workers, NULL, n_swept, error);
}

gboolean
axis_job_sweep_expired_leases_fenced(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar *const  *except_workers,
	const AxisJobFence  *fence,
	guint               *n_swept,
	GError             **error
){
	static const gchar *const none[] = { NULL };
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kinds_array = NULL;
	g_autofree gchar *except_array = NULL;
	gchar fence_text[3][24];
	const gchar *params[6];
	guint n;
	guint i;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (n_swept != NULL)
		*n_swept = 0;
	if (!check_kinds(kinds, error))
		return FALSE;
	if (kinds[0] == NULL)
		return TRUE;

	kinds_array = axis_job_text_array(kinds);
	except_array = axis_job_text_array(except_workers != NULL ? except_workers : none);
	params[0] = kinds_array;
	params[1] = except_array;
	fence_params(fence, fence_text, &params[2]);
	if (!axis_db_conn_exec(conn, sweep_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	n = axis_db_result_get_n_rows(result);
	for (i = 0; i < n; i++) {
		const gchar *state = axis_db_result_get_text(result, i, 2);

		g_warning("job %s (%s): the lease of worker %s expired; %s (attempt %s of %s)",
			axis_db_result_get_text(result, i, 0), axis_db_result_get_text(result, i, 1),
			axis_db_result_get_text(result, i, 5),
			g_strcmp0(state, "queued") == 0 ? "returned to the queue" :
			g_strcmp0(state, "failed") == 0 ? "no attempts are left, so it failed" :
			"it was being cancelled, so it is cancelled",
			axis_db_result_get_text(result, i, 3), axis_db_result_get_text(result, i, 4));
	}

	if (n_swept != NULL)
		*n_swept = n;
	return TRUE;
}

gboolean
axis_job_take_back_from_daemons(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar         *own_daemon,
	const AxisJobFence  *fence,
	guint               *n_taken,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kinds_array = NULL;
	gchar fence_text[3][24];
	const gchar *params[6];
	gboolean fenced_ok = FALSE;
	guint n;
	guint i;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (n_taken != NULL)
		*n_taken = 0;
	if (!check_kinds(kinds, error))
		return FALSE;
	if (own_daemon == NULL || !g_regex_match_simple(AXIS_JOB_DAEMON_ID_PATTERN, own_daemon, 0, 0)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"'%s' is not a daemon id of the form axisd:HOST:PID:INSTANCE",
			own_daemon != NULL ? own_daemon : "(null)");
		return FALSE;
	}
	if (kinds[0] == NULL)
		return TRUE;

	kinds_array = axis_job_text_array(kinds);
	params[0] = kinds_array;
	params[1] = own_daemon;
	fence_params(fence, fence_text, &params[2]);
	if (!axis_db_conn_exec(conn, take_back_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0 || !axis_db_result_get_boolean(result, 0, 0, &fenced_ok)) {
		set_bad_result(error, "a take-over");
		return FALSE;
	}
	if (!fenced_ok) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_FENCED,
			"nothing was taken back: this daemon does not hold the database lock");
		return FALSE;
	}

	n = 0;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *state = axis_db_result_get_text(result, i, 3);

		if (axis_db_result_is_null(result, i, 1))
			continue;
		n++;
		g_warning("job %s (%s): its daemon %s no longer runs; %s (attempt %s of %s)",
			axis_db_result_get_text(result, i, 1), axis_db_result_get_text(result, i, 2),
			axis_db_result_get_text(result, i, 6),
			g_strcmp0(state, "queued") == 0 ? "returned to the queue" :
			g_strcmp0(state, "failed") == 0 ? "no attempts are left, so it failed" :
			"it was being cancelled, so it is cancelled",
			axis_db_result_get_text(result, i, 4), axis_db_result_get_text(result, i, 5));
	}

	if (n_taken != NULL)
		*n_taken = n;
	return TRUE;
}

gboolean
axis_job_prune(
	AxisDbConn   *conn,
	gint64        keep_finished_ms,
	gint64        keep_failed_ms,
	guint64      *n_deleted,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *finished = g_strdup_printf("%" G_GINT64_FORMAT, MAX(keep_finished_ms, 0));
	g_autofree gchar *failed = g_strdup_printf("%" G_GINT64_FORMAT, MAX(keep_failed_ms, 0));
	const gchar *params[2];

	g_return_val_if_fail(conn != NULL, FALSE);

	params[0] = finished;
	params[1] = failed;
	if (!axis_db_conn_exec(conn, prune_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (n_deleted != NULL)
		*n_deleted = axis_db_result_get_affected_rows(result);

	/* What a job a person retried keeps of its earlier runs lasts no
	 * longer than a failed job does. */
	params[0] = failed;
	return axis_db_conn_exec(conn, prune_earlier_sql, 1, params, NULL, error);
}

/*
 * Remote workers
 */

static gboolean
check_worker_id(
	const gchar  *worker_id,
	GError      **error
){
	if (worker_id != NULL && *worker_id != '\0' && strlen(worker_id) <= AXIS_JOB_MAX_WORKER_ID_LEN &&
	    g_utf8_validate(worker_id, -1, NULL))
		return TRUE;
	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
		"a worker id must be 1 to %d bytes of UTF-8", AXIS_JOB_MAX_WORKER_ID_LEN);
	return FALSE;
}

gboolean
axis_job_claim_remote(
	AxisDbConn                 *conn,
	const gchar                *queue,
	const gchar *const         *kinds,
	const gchar                *worker_id,
	const AxisJobRemoteFilter  *filter,
	guint                       lease_ms,
	AxisJobClaim              **claim,
	GError                    **error
){
	return axis_job_claim_remote_fenced(conn, queue, kinds, worker_id, filter, lease_ms, NULL, claim, error);
}

gboolean
axis_job_claim_remote_fenced(
	AxisDbConn                 *conn,
	const gchar                *queue,
	const gchar *const         *kinds,
	const gchar                *worker_id,
	const AxisJobRemoteFilter  *filter,
	guint                       lease_ms,
	const AxisJobFence         *fence,
	AxisJobClaim              **claim,
	GError                    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisJobClaim) out = NULL;
	g_autofree gchar *kinds_array = NULL;
	g_autofree gchar *lease = NULL;
	gchar fence_text[3][24];
	const gchar *params[11];
	gint64 priority = 0;

	g_return_val_if_fail(conn != NULL, FALSE);
	g_return_val_if_fail(claim != NULL, FALSE);
	g_return_val_if_fail(filter != NULL, FALSE);

	*claim = NULL;
	if (!check_name("queue", queue, error) || !check_kinds(kinds, error) || !check_worker_id(worker_id, error))
		return FALSE;
	if (lease_ms == 0 || filter->worker == NULL || filter->machine == NULL || filter->volumes_json == NULL) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"a remote claim needs a lease, the worker, its machine and its volumes");
		return FALSE;
	}
	if (kinds[0] == NULL)
		return TRUE;

	kinds_array = axis_job_text_array(kinds);
	lease = g_strdup_printf("%u", lease_ms);
	params[0] = queue;
	params[1] = kinds_array;
	params[2] = worker_id;
	params[3] = lease;
	params[4] = filter->worker;
	params[5] = filter->machine;
	params[6] = filter->volumes_json;
	fence_params(fence, fence_text, &params[7]);

	if (!axis_db_conn_exec(conn, claim_remote_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;

	out = g_new0(AxisJobClaim, 1);
	if (axis_db_result_get_n_rows(result) != 1 ||
	    !axis_db_result_get_int64(result, 0, 0, &out->id) ||
	    !axis_db_result_get_int64(result, 0, 4, &priority) ||
	    !parse_uint(result, 0, 5, &out->attempt) ||
	    !parse_uint(result, 0, 6, &out->max_attempts) ||
	    !axis_db_result_get_boolean(result, 0, 8, &out->cancel_requested) ||
	    axis_db_result_is_null(result, 0, 7)) {
		set_bad_result(error, "a remote claim");
		return FALSE;
	}
	out->kind = g_strdup(axis_db_result_get_text(result, 0, 1));
	out->queue = g_strdup(axis_db_result_get_text(result, 0, 2));
	out->payload = g_strdup(axis_db_result_get_text(result, 0, 3));
	out->priority = (gint)priority;
	out->lease_token = g_strdup(axis_db_result_get_text(result, 0, 7));

	*claim = g_steal_pointer(&out);
	return TRUE;
}

gboolean
axis_job_heartbeat_remote(
	AxisDbConn             *conn,
	gint64                  id,
	const gchar            *lease_token,
	const gchar            *worker_id,
	guint                   lease_ms,
	const AxisJobProgress  *progress,
	gboolean               *cancel_requested,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *lease = NULL;
	g_autofree gchar *fraction = NULL;
	g_autofree gchar *message = NULL;
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	const gchar *params[7];
	gboolean requested = FALSE;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_token(id, lease_token, error) || !check_worker_id(worker_id, error))
		return FALSE;
	if (lease_ms == 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"a lease must be longer than 0 ms");
		return FALSE;
	}

	id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	lease = g_strdup_printf("%u", lease_ms);
	if (progress != NULL) {
		gdouble f = progress->fraction;

		if (isnan(f) || f < 0.0)
			f = 0.0;
		else if (f > 1.0)
			f = 1.0;
		fraction = g_strdup(g_ascii_dtostr(buffer, sizeof(buffer), f));
		message = axis_job_clean_text(progress->message, AXIS_JOB_MAX_PROGRESS_CHARS);
	}

	params[0] = id_text;
	params[1] = lease_token;
	params[2] = lease;
	params[3] = progress != NULL ? "true" : "false";
	params[4] = fraction;
	params[5] = message;
	params[6] = worker_id;

	if (!axis_db_conn_exec(conn, heartbeat_remote_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		set_lease_lost(error, id);
		return FALSE;
	}
	if (!axis_db_result_get_boolean(result, 0, 0, &requested)) {
		set_bad_result(error, "a remote heartbeat");
		return FALSE;
	}

	if (cancel_requested != NULL)
		*cancel_requested = requested;
	return TRUE;
}

gboolean
axis_job_lock_remote(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *lease_token,
	const gchar  *worker_id,
	gchar       **kind,
	gchar       **payload,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = NULL;
	const gchar *params[3];

	g_return_val_if_fail(conn != NULL, FALSE);

	if (kind != NULL)
		*kind = NULL;
	if (payload != NULL)
		*payload = NULL;
	if (!check_token(id, lease_token, error) || !check_worker_id(worker_id, error))
		return FALSE;

	id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	params[0] = id_text;
	params[1] = lease_token;
	params[2] = worker_id;
	if (!axis_db_conn_exec(conn, lock_remote_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		set_lease_lost(error, id);
		return FALSE;
	}
	if (kind != NULL)
		*kind = g_strdup(axis_db_result_get_text(result, 0, 0));
	if (payload != NULL)
		*payload = g_strdup(axis_db_result_get_text(result, 0, 1));
	return TRUE;
}

gboolean
axis_job_expire_remote(
	AxisDbConn    *conn,
	gint64         id,
	const gchar   *lease_token,
	AxisJobState  *state,
	GError       **error
){
	return axis_job_expire_remote_fenced(conn, id, lease_token, NULL, state, error);
}

gboolean
axis_job_expire_remote_fenced(
	AxisDbConn          *conn,
	gint64               id,
	const gchar         *lease_token,
	const AxisJobFence  *fence,
	AxisJobState        *state,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = NULL;
	gchar fence_text[3][24];
	const gchar *params[6];
	AxisJobState new_state = AXIS_JOB_STATE_QUEUED;
	gboolean fenced_ok = FALSE;
	const gchar *text;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_token(id, lease_token, error))
		return FALSE;
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	params[0] = id_text;
	params[1] = lease_token;
	fence_params(fence, fence_text, &params[2]);
	if (!axis_db_conn_exec(conn, expire_remote_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1 || !axis_db_result_get_boolean(result, 0, 5, &fenced_ok)) {
		set_bad_result(error, "a remote expiry");
		return FALSE;
	}
	if (!fenced_ok) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_FENCED,
			"job %" G_GINT64_FORMAT " was not taken back: this daemon does not hold the database lock", id);
		return FALSE;
	}
	if (axis_db_result_is_null(result, 0, 0)) {
		set_lease_lost(error, id);
		return FALSE;
	}
	text = axis_db_result_get_text(result, 0, 0);
	if (!axis_job_state_from_string(text, &new_state)) {
		set_bad_result(error, "a remote expiry");
		return FALSE;
	}
	g_warning("job %" G_GINT64_FORMAT ": the lease of worker %s expired; %s (attempt %s of %s)", id,
		axis_db_result_get_text(result, 0, 3),
		new_state == AXIS_JOB_STATE_QUEUED ? "returned to the queue" :
		new_state == AXIS_JOB_STATE_FAILED ? "no attempts are left, so it failed" :
		"it was being cancelled, so it is cancelled",
		axis_db_result_get_text(result, 0, 1), axis_db_result_get_text(result, 0, 2));
	if (state != NULL)
		*state = new_state;
	return TRUE;
}
