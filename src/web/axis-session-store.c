/*
 * axis-session-store.c - the in-memory accounts and sessions of the web
 * interface, written through to PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-session-store.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"

#include <string.h>

/* A write the database could not be reached for is retried after this,
 * doubling up to WRITE_RETRY_MAX_US. */
#define WRITE_RETRY_FIRST_US (G_GINT64_CONSTANT(250) * 1000)
#define WRITE_RETRY_MAX_US   (G_GINT64_CONSTANT(30) * G_USEC_PER_SEC)

/* While stopping, a failing write gets this many tries in all. */
#define WRITE_TRIES_WHEN_STOPPING 3

#define IDLE_MIN_SECONDS 3600
#define IDLE_MAX_SECONDS (90 * 24 * 3600)

/* The idle limit, in seconds (atomic): a process-wide policy. */
static gint session_idle_seconds = (gint)(AXIS_SESSION_IDLE_US / G_USEC_PER_SEC);

void
axis_session_store_set_idle_seconds(guint seconds)
{
	g_return_if_fail(seconds >= IDLE_MIN_SECONDS && seconds <= IDLE_MAX_SECONDS);

	g_atomic_int_set(&session_idle_seconds, (gint)seconds);
}

guint
axis_session_store_get_idle_seconds(void)
{
	return (guint)g_atomic_int_get(&session_idle_seconds);
}

static gint64
idle_us(void)
{
	return (gint64)g_atomic_int_get(&session_idle_seconds) * G_USEC_PER_SEC;
}

typedef enum
{
	OP_INSERT,
	OP_DELETE,
	OP_SET_EPOCH,
	OP_TOUCH,
	OP_PURGE,
	OP_MARK,
	OP_STOP
} OpKind;

typedef struct
{
	GMutex   lock;
	GCond    cond;
	gboolean reached;
} Mark;

typedef struct
{
	OpKind  kind;
	AxisSessionKind session_kind;	/* OP_INSERT */
	gchar  *token_hash;
	gchar  *public_id;
	gint64  user_id;
	gint    epoch;
	gint64  created_us;
	gint64  last_seen_us;
	gchar  *client_address;
	gchar  *user_agent;
	gchar  *hashes;		/* OP_TOUCH: array literals */
	gchar  *times;
	gint64  now_us;		/* OP_PURGE */
	Mark   *mark;		/* OP_MARK */
} Op;

struct _AxisSessionStore
{
	AxisDbPool  *pool;		/* nullable */
	GHashTable  *users;		/* gint64 id -> AxisUser* (owned) */
	GHashTable  *users_by_name;	/* username -> AxisUser* (borrowed) */
	GHashTable  *sessions;		/* token_hash -> AxisSession* (owned) */
	GHashTable  *dirty;		/* token_hash (owned copy) */

	GAsyncQueue *queue;		/* of Op* */
	GThread     *writer;
	gint         stopping;		/* atomic */
	gboolean     stopped;

	AxisSessionEndFunc end_func;	/* nullable */
	gpointer           end_data;
	AxisSessionRoleFunc role_func;	/* nullable */
	gpointer            role_data;
};

static void
op_free(Op *op)
{
	if (op == NULL)
		return;

	g_free(op->token_hash);
	g_free(op->public_id);
	g_free(op->client_address);
	g_free(op->user_agent);
	g_free(op->hashes);
	g_free(op->times);
	g_free(op);
}

static void
session_free(AxisSession *session)
{
	if (session == NULL)
		return;

	g_free(session->token_hash);
	g_free(session->public_id);
	g_free(session->client_address);
	g_free(session->user_agent);
	g_free(session);
}

static gchar *
int64_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

/* ── Writer thread ────────────────────────────────────────────── */

static gboolean
run_op(
	AxisDbPool  *pool,
	Op          *op,
	GError     **error
){
	switch (op->kind) {
	case OP_INSERT: {
		g_autofree gchar *user_id = int64_text(op->user_id);
		g_autofree gchar *epoch = g_strdup_printf("%d", op->epoch);
		g_autofree gchar *created = int64_text(op->created_us);
		g_autofree gchar *seen = int64_text(op->last_seen_us);
		const gchar *params[9];

		params[0] = op->token_hash;
		params[1] = op->public_id;
		params[2] = user_id;
		params[3] = epoch;
		params[4] = created;
		params[5] = seen;
		params[6] = op->client_address;
		params[7] = op->user_agent;
		params[8] = op->session_kind == AXIS_SESSION_PORTAL ? "portal" : "web";
		return axis_db_pool_exec(pool,
			"INSERT INTO web_session (token_hash, public_id, user_id, user_epoch, "
			"created_at, last_seen_at, client_address, user_agent, kind) "
			"VALUES ($1, $2, $3, $4, to_timestamp($5::double precision / 1000000), "
			"to_timestamp($6::double precision / 1000000), $7, $8, $9) "
			"ON CONFLICT (token_hash) DO NOTHING",
			9, params, NULL, error);
	}
	case OP_DELETE: {
		const gchar *params[1];

		params[0] = op->token_hash;
		return axis_db_pool_exec(pool, "DELETE FROM web_session WHERE token_hash = $1",
			1, params, NULL, error);
	}
	case OP_SET_EPOCH: {
		g_autofree gchar *epoch = g_strdup_printf("%d", op->epoch);
		const gchar *params[2];

		params[0] = op->token_hash;
		params[1] = epoch;
		return axis_db_pool_exec(pool,
			"UPDATE web_session SET user_epoch = $2 WHERE token_hash = $1",
			2, params, NULL, error);
	}
	case OP_TOUCH: {
		const gchar *params[2];

		params[0] = op->hashes;
		params[1] = op->times;
		return axis_db_pool_exec(pool,
			"UPDATE web_session AS s "
			"SET last_seen_at = to_timestamp(t.seen::double precision / 1000000) "
			"FROM unnest($1::text[], $2::bigint[]) AS t(hash, seen) "
			"WHERE s.token_hash = t.hash "
			"AND s.last_seen_at < to_timestamp(t.seen::double precision / 1000000)",
			2, params, NULL, error);
	}
	case OP_PURGE: {
		g_autofree gchar *idle = int64_text(op->now_us - idle_us());
		g_autofree gchar *old = int64_text(op->now_us - AXIS_SESSION_LIFETIME_US);
		const gchar *params[2];

		params[0] = idle;
		params[1] = old;
		return axis_db_pool_exec(pool,
			"DELETE FROM web_session AS s USING app_user AS u "
			"WHERE u.id = s.user_id AND (u.disabled OR s.user_epoch <> u.auth_epoch "
			"OR (s.kind = 'portal') <> (u.role = 'requester') "
			"OR s.last_seen_at < to_timestamp($1::double precision / 1000000) "
			"OR s.created_at < to_timestamp($2::double precision / 1000000))",
			2, params, NULL, error);
	}
	case OP_MARK:
	case OP_STOP:
		break;
	}

	return TRUE;
}

static const gchar *
op_description(OpKind kind)
{
	switch (kind) {
	case OP_INSERT:    return "save a new session";
	case OP_DELETE:    return "delete an ended session (it would be accepted again after a restart)";
	case OP_SET_EPOCH: return "update a session";
	case OP_TOUCH:     return "save when sessions were last used";
	case OP_PURGE:     return "delete expired sessions";
	case OP_MARK:
	case OP_STOP:      break;
	}

	return "write";
}

static gboolean
is_transient(const GError *error)
{
	return error->domain == AXIS_DB_ERROR &&
	       (error->code == AXIS_DB_ERROR_CONNECT || error->code == AXIS_DB_ERROR_TIMEOUT ||
	        error->code == AXIS_DB_ERROR_CONNECTION_LOST);
}

/* Sleeps for @us, waking early when the store starts stopping. */
static void
backoff(
	AxisSessionStore *self,
	gint64            us
){
	gint64 deadline = g_get_monotonic_time() + us;

	while (g_get_monotonic_time() < deadline && !g_atomic_int_get(&self->stopping))
		g_usleep(50 * 1000);
}

static void
run_with_retries(
	AxisSessionStore *self,
	Op               *op
){
	gint64 delay = WRITE_RETRY_FIRST_US;
	guint tries = 0;

	for (;;) {
		g_autoptr(GError) error = NULL;

		if (run_op(self->pool, op, &error))
			return;

		tries++;
		if (!is_transient(error)) {
			g_warning("sessions: the database refused to %s: %s",
				op_description(op->kind), error->message);
			return;
		}
		if (g_atomic_int_get(&self->stopping) && tries >= WRITE_TRIES_WHEN_STOPPING) {
			g_warning("sessions: giving up trying to %s: %s",
				op_description(op->kind), error->message);
			return;
		}
		if (tries == 1)
			g_warning("sessions: could not %s, will retry: %s",
				op_description(op->kind), error->message);

		backoff(self, delay);
		delay = MIN(delay * 2, WRITE_RETRY_MAX_US);
	}
}

static gpointer
writer_main(gpointer data)
{
	AxisSessionStore *self = data;

	for (;;) {
		Op *op = g_async_queue_pop(self->queue);

		if (op->kind == OP_STOP) {
			op_free(op);
			break;
		}
		if (op->kind == OP_MARK) {
			g_mutex_lock(&op->mark->lock);
			op->mark->reached = TRUE;
			g_cond_broadcast(&op->mark->cond);
			g_mutex_unlock(&op->mark->lock);
			op_free(op);
			continue;
		}

		run_with_retries(self, op);
		op_free(op);
	}

	return NULL;
}

static void
push(
	AxisSessionStore *self,
	Op               *op
){
	if (self->writer == NULL || self->stopped) {
		op_free(op);
		return;
	}

	g_async_queue_push(self->queue, op);
}

/* ── Loading ──────────────────────────────────────────────────── */

static gboolean
session_expired(
	const AxisSession *session,
	gint64             now_us
){
	return now_us - session->last_seen_us > idle_us() ||
	       now_us - session->created_us > AXIS_SESSION_LIFETIME_US;
}

static void
add_user(
	AxisSessionStore *self,
	AxisUser         *user
){
	gint64 *key = g_new(gint64, 1);

	*key = user->id;
	g_hash_table_replace(self->users, key, user);
	g_hash_table_replace(self->users_by_name, user->username, user);
}

static gboolean
load(
	AxisSessionStore  *self,
	gint64             now_us,
	GError           **error
){
	g_autoptr(GPtrArray) users = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	users = axis_user_store_list(self->pool, error);
	if (users == NULL)
		return FALSE;
	for (i = 0; i < users->len; i++)
		add_user(self, axis_user_copy(g_ptr_array_index(users, i)));

	if (!axis_db_pool_exec(self->pool,
	                       "SELECT s.token_hash, s.public_id, s.user_id, s.user_epoch, "
	                       "(extract(epoch FROM s.created_at) * 1000000)::bigint, "
	                       "(extract(epoch FROM s.last_seen_at) * 1000000)::bigint, "
	                       "s.client_address, s.user_agent, s.kind "
	                       "FROM web_session AS s JOIN app_user AS u ON u.id = s.user_id "
	                       "WHERE NOT u.disabled AND s.user_epoch = u.auth_epoch",
	                       0, NULL, &result, error))
		return FALSE;

	/* A row whose kind does not fit its account's role is loaded too: the
	 * lookup refuses it, by the same rule as any other, and ends it. */
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisSession *session = g_new0(AxisSession, 1);
		gint64 epoch = 0;

		session->kind = g_strcmp0(axis_db_result_get_text(result, i, 8), "portal") == 0 ? AXIS_SESSION_PORTAL :
			AXIS_SESSION_WEB;
		session->token_hash = g_strdup(axis_db_result_get_text(result, i, 0));
		session->public_id = g_strdup(axis_db_result_get_text(result, i, 1));
		axis_db_result_get_int64(result, i, 2, &session->user_id);
		axis_db_result_get_int64(result, i, 3, &epoch);
		session->epoch = (gint)epoch;
		axis_db_result_get_int64(result, i, 4, &session->created_us);
		axis_db_result_get_int64(result, i, 5, &session->last_seen_us);
		session->client_address = g_strdup(axis_db_result_get_text(result, i, 6));
		session->user_agent = g_strdup(axis_db_result_get_text(result, i, 7));

		if (session_expired(session, now_us)) {
			session_free(session);
			continue;
		}
		g_hash_table_replace(self->sessions, session->token_hash, session);
	}

	return TRUE;
}

AxisSessionStore *
axis_session_store_new(
	AxisDbPool  *pool,
	gint64       now_us,
	GError     **error
){
	AxisSessionStore *self = g_new0(AxisSessionStore, 1);

	self->pool = pool;
	self->users = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)axis_user_free);
	self->users_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	self->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
		(GDestroyNotify)session_free);
	self->dirty = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->queue = g_async_queue_new();

	if (pool != NULL) {
		if (!load(self, now_us, error)) {
			self->stopped = TRUE;
			axis_session_store_free(self);
			return NULL;
		}

		self->writer = g_thread_new("axis-sessions", writer_main, self);

		/* Rows that can no longer be used: expired, or of an account that
		 * was disabled or moved on to a new epoch. */
		{
			Op *op = g_new0(Op, 1);

			op->kind = OP_PURGE;
			op->now_us = now_us;
			push(self, op);
		}
	}

	return self;
}

void
axis_session_store_stop(AxisSessionStore *self)
{
	Op *op;

	g_return_if_fail(self != NULL);

	if (self->stopped)
		return;

	axis_session_store_flush(self);
	if (self->writer != NULL) {
		g_atomic_int_set(&self->stopping, TRUE);
		op = g_new0(Op, 1);
		op->kind = OP_STOP;
		g_async_queue_push(self->queue, op);
		g_thread_join(self->writer);
		self->writer = NULL;
	}
	self->stopped = TRUE;
}

void
axis_session_store_free(AxisSessionStore *self)
{
	Op *op;

	if (self == NULL)
		return;

	axis_session_store_stop(self);

	while ((op = g_async_queue_try_pop(self->queue)) != NULL)
		op_free(op);
	g_async_queue_unref(self->queue);
	g_hash_table_unref(self->dirty);
	g_hash_table_unref(self->sessions);
	g_hash_table_unref(self->users_by_name);
	g_hash_table_unref(self->users);
	g_free(self);
}

void
axis_session_store_wait_idle(AxisSessionStore *self)
{
	Mark mark;
	Op *op;

	g_return_if_fail(self != NULL);

	if (self->writer == NULL || self->stopped)
		return;

	g_mutex_init(&mark.lock);
	g_cond_init(&mark.cond);
	mark.reached = FALSE;

	op = g_new0(Op, 1);
	op->kind = OP_MARK;
	op->mark = &mark;
	g_async_queue_push(self->queue, op);

	g_mutex_lock(&mark.lock);
	while (!mark.reached)
		g_cond_wait(&mark.cond, &mark.lock);
	g_mutex_unlock(&mark.lock);

	g_mutex_clear(&mark.lock);
	g_cond_clear(&mark.cond);
}

/* ── Accounts ─────────────────────────────────────────────────── */

const AxisUser *
axis_session_store_get_user(
	AxisSessionStore *self,
	gint64            id
){
	g_return_val_if_fail(self != NULL, NULL);

	return g_hash_table_lookup(self->users, &id);
}

const AxisUser *
axis_session_store_find_user(
	AxisSessionStore *self,
	const gchar      *username
){
	g_return_val_if_fail(self != NULL, NULL);

	if (username == NULL)
		return NULL;

	return g_hash_table_lookup(self->users_by_name, username);
}

guint
axis_session_store_count_users(AxisSessionStore *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_hash_table_size(self->users);
}

static gint
compare_users(
	gconstpointer a,
	gconstpointer b
){
	const AxisUser *x = *(const AxisUser *const *)a;
	const AxisUser *y = *(const AxisUser *const *)b;

	return strcmp(x->username, y->username);
}

GPtrArray *
axis_session_store_list_users(AxisSessionStore *self)
{
	GPtrArray *list = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, list);

	g_hash_table_iter_init(&iter, self->users);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(list, value);
	g_ptr_array_sort(list, compare_users);

	return list;
}

void
axis_session_store_set_end_func(
	AxisSessionStore   *self,
	AxisSessionEndFunc  func,
	gpointer            user_data
){
	g_return_if_fail(self != NULL);

	self->end_func = func;
	self->end_data = user_data;
}

void
axis_session_store_set_role_func(
	AxisSessionStore    *self,
	AxisSessionRoleFunc  func,
	gpointer             user_data
){
	g_return_if_fail(self != NULL);

	self->role_func = func;
	self->role_data = user_data;
}

/* Removes @session (which must be in the table) and frees it. */
static void
drop_session(
	AxisSessionStore *self,
	AxisSession      *session,
	gboolean          persist
){
	if (persist) {
		Op *op = g_new0(Op, 1);

		op->kind = OP_DELETE;
		op->token_hash = g_strdup(session->token_hash);
		push(self, op);
	}

	/* Told while the session is still whole; the handler must not call
	 * back into the store. */
	if (self->end_func != NULL)
		self->end_func(session->token_hash, self->end_data);

	g_hash_table_remove(self->dirty, session->token_hash);
	g_hash_table_remove(self->sessions, session->token_hash);
}

/* Drops every session of @user_id that @keep (nullable) rejects. */
typedef gboolean (*KeepFunc)(const AxisSession *session, gconstpointer data);

static void
drop_sessions_of(
	AxisSessionStore *self,
	gint64            user_id,
	KeepFunc          keep,
	gconstpointer     data
){
	g_autoptr(GPtrArray) doomed = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisSession *session = value;

		if (session->user_id == user_id && (keep == NULL || !keep(session, data)))
			g_ptr_array_add(doomed, session);
	}

	for (i = 0; i < doomed->len; i++)
		drop_session(self, g_ptr_array_index(doomed, i), TRUE);
}

/*
 * Whether @session may be used by its account @owner as it is now: enabled,
 * under the epoch the session was made in, and of the kind its role may have
 * (a requester has no session of the engine's interface; anyone else has no
 * portal session).
 */
static gboolean
session_fits(
	const AxisSession *session,
	const AxisUser    *owner
){
	if (owner->disabled || session->epoch != owner->epoch)
		return FALSE;
	return (session->kind == AXIS_SESSION_PORTAL) == (owner->role == AXIS_USER_ROLE_REQUESTER);
}

static gboolean
keep_if_valid_for(
	const AxisSession *session,
	gconstpointer      data
){
	return session_fits(session, data);
}

void
axis_session_store_put_user(
	AxisSessionStore *self,
	const AxisUser   *user
){
	AxisUser *copy;
	AxisUser *old;
	gboolean role_changed;

	g_return_if_fail(self != NULL);
	g_return_if_fail(user != NULL);

	old = g_hash_table_lookup(self->users, &user->id);
	role_changed = old != NULL && old->role != user->role;
	if (old != NULL)
		g_hash_table_remove(self->users_by_name, old->username);

	copy = axis_user_copy(user);
	add_user(self, copy);
	drop_sessions_of(self, copy->id, keep_if_valid_for, copy);

	/* Every request reads the role from here; what was opened under the
	 * old one (an event stream) is told. */
	if (role_changed && self->role_func != NULL)
		self->role_func(copy->id, self->role_data);
}

void
axis_session_store_remove_user(
	AxisSessionStore *self,
	gint64            id
){
	AxisUser *old;

	g_return_if_fail(self != NULL);

	drop_sessions_of(self, id, NULL, NULL);

	old = g_hash_table_lookup(self->users, &id);
	if (old != NULL) {
		g_hash_table_remove(self->users_by_name, old->username);
		g_hash_table_remove(self->users, &id);
	}
}

/* ── Sessions ─────────────────────────────────────────────────── */

const AxisSession *
axis_session_store_lookup(
	AxisSessionStore  *self,
	const gchar       *token,
	gint64             now_us,
	const AxisUser   **user
){
	return axis_session_store_lookup_kind(self, AXIS_SESSION_WEB, token, now_us, user);
}

const AxisSession *
axis_session_store_lookup_kind(
	AxisSessionStore  *self,
	AxisSessionKind    kind,
	const gchar       *token,
	gint64             now_us,
	const AxisUser   **user
){
	g_autofree gchar *hash = NULL;
	AxisSession *session;
	const AxisUser *owner;

	g_return_val_if_fail(self != NULL, NULL);

	if (user != NULL)
		*user = NULL;

	if (!axis_auth_token_is_wellformed(token, AXIS_TOKEN_LEN))
		return NULL;

	hash = axis_token_hash(token);
	session = g_hash_table_lookup(self->sessions, hash);
	/* A session of the other kind is none here, whatever else is true of
	 * it; it is left as it is, for where it belongs. */
	if (session == NULL || session->kind != kind)
		return NULL;

	owner = axis_session_store_get_user(self, session->user_id);
	if (owner == NULL || !session_fits(session, owner) || session_expired(session, now_us)) {
		drop_session(self, session, TRUE);
		return NULL;
	}

	if (now_us - session->last_seen_us >= AXIS_SESSION_TOUCH_US) {
		session->last_seen_us = now_us;
		g_hash_table_add(self->dirty, g_strdup(session->token_hash));
	}

	if (user != NULL)
		*user = owner;
	return session;
}

/* The least recently used of @user_id's sessions, and how many it has. */
static AxisSession *
oldest_of(
	AxisSessionStore *self,
	gint64            user_id,
	guint            *count
){
	GHashTableIter iter;
	gpointer value;
	AxisSession *oldest = NULL;

	*count = 0;
	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisSession *session = value;

		if (session->user_id != user_id)
			continue;
		(*count)++;
		if (oldest == NULL || session->last_seen_us < oldest->last_seen_us)
			oldest = session;
	}

	return oldest;
}

const AxisSession *
axis_session_store_create(
	AxisSessionStore  *self,
	gint64             user_id,
	gint               epoch,
	const gchar       *client_address,
	const gchar       *user_agent,
	gint64             now_us,
	gchar            **token
){
	return axis_session_store_create_kind(self, AXIS_SESSION_WEB, user_id, epoch, client_address, user_agent, now_us,
		token);
}

guint
axis_session_store_revoke_kind(
	AxisSessionStore *self,
	gint64            user_id,
	AxisSessionKind   kind
){
	g_autoptr(GPtrArray) doomed = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_return_val_if_fail(self != NULL, 0);

	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisSession *session = value;

		if (session->user_id == user_id && session->kind == kind)
			g_ptr_array_add(doomed, session);
	}
	for (i = 0; i < doomed->len; i++)
		drop_session(self, g_ptr_array_index(doomed, i), TRUE);
	return doomed->len;
}

guint
axis_session_store_count_held(
	AxisSessionStore *self,
	gint64            user_id
){
	GHashTableIter iter;
	gpointer value;
	guint n = 0;

	g_return_val_if_fail(self != NULL, 0);

	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		if (((const AxisSession *)value)->user_id == user_id)
			n++;
	}
	return n;
}

guint
axis_session_store_count_kind(
	AxisSessionStore *self,
	gint64            user_id,
	AxisSessionKind   kind,
	gint64            now_us
){
	const AxisUser *owner;
	GHashTableIter iter;
	gpointer value;
	guint n = 0;

	g_return_val_if_fail(self != NULL, 0);

	owner = axis_session_store_get_user(self, user_id);
	if (owner == NULL)
		return 0;
	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		const AxisSession *session = value;

		if (session->user_id == user_id && session->kind == kind && session_fits(session, owner) &&
		    !session_expired(session, now_us))
			n++;
	}
	return n;
}

const AxisSession *
axis_session_store_create_kind(
	AxisSessionStore  *self,
	AxisSessionKind    kind,
	gint64             user_id,
	gint               epoch,
	const gchar       *client_address,
	const gchar       *user_agent,
	gint64             now_us,
	gchar            **token
){
	AxisSession *session;
	AxisSession *oldest;
	guint count;
	Op *op;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(token != NULL, NULL);

	while ((oldest = oldest_of(self, user_id, &count)) != NULL &&
	       count >= AXIS_SESSION_MAX_PER_USER)
		drop_session(self, oldest, TRUE);

	*token = axis_random_token();

	session = g_new0(AxisSession, 1);
	session->kind = kind;
	session->token_hash = axis_token_hash(*token);
	session->public_id = axis_random_public_id();
	session->user_id = user_id;
	session->epoch = epoch;
	session->created_us = now_us;
	session->last_seen_us = now_us;
	session->client_address = g_strdup(client_address != NULL ? client_address : "");
	session->user_agent = axis_auth_clean_user_agent(user_agent);
	g_hash_table_replace(self->sessions, session->token_hash, session);

	op = g_new0(Op, 1);
	op->kind = OP_INSERT;
	op->session_kind = session->kind;
	op->token_hash = g_strdup(session->token_hash);
	op->public_id = g_strdup(session->public_id);
	op->user_id = session->user_id;
	op->epoch = session->epoch;
	op->created_us = session->created_us;
	op->last_seen_us = session->last_seen_us;
	op->client_address = g_strdup(session->client_address);
	op->user_agent = g_strdup(session->user_agent);
	push(self, op);

	return session;
}

gboolean
axis_session_store_revoke(
	AxisSessionStore *self,
	const gchar      *token_hash
){
	AxisSession *session;

	g_return_val_if_fail(self != NULL, FALSE);

	if (token_hash == NULL)
		return FALSE;

	session = g_hash_table_lookup(self->sessions, token_hash);
	if (session == NULL)
		return FALSE;

	drop_session(self, session, TRUE);
	return TRUE;
}

gboolean
axis_session_store_revoke_public(
	AxisSessionStore *self,
	gint64            user_id,
	const gchar      *public_id
){
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!axis_auth_token_is_wellformed(public_id, AXIS_PUBLIC_ID_LEN))
		return FALSE;

	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisSession *session = value;

		if (session->user_id == user_id && strcmp(session->public_id, public_id) == 0) {
			drop_session(self, session, TRUE);
			return TRUE;
		}
	}

	return FALSE;
}

void
axis_session_store_set_epoch(
	AxisSessionStore *self,
	const gchar      *token_hash,
	gint              epoch
){
	AxisSession *session;
	Op *op;

	g_return_if_fail(self != NULL);

	session = token_hash != NULL ? g_hash_table_lookup(self->sessions, token_hash) : NULL;
	if (session == NULL)
		return;

	session->epoch = epoch;

	/* After the session's own insert in the queue, if that has not been
	 * written yet: the row ends up with this epoch either way. */
	op = g_new0(Op, 1);
	op->kind = OP_SET_EPOCH;
	op->token_hash = g_strdup(token_hash);
	op->epoch = epoch;
	push(self, op);
}

static gint
compare_recent_first(
	gconstpointer a,
	gconstpointer b
){
	const AxisSession *x = *(const AxisSession *const *)a;
	const AxisSession *y = *(const AxisSession *const *)b;

	if (x->last_seen_us != y->last_seen_us)
		return x->last_seen_us > y->last_seen_us ? -1 : 1;
	return strcmp(x->public_id, y->public_id);
}

GPtrArray *
axis_session_store_list_for_user(
	AxisSessionStore *self,
	gint64            user_id,
	gint64            now_us
){
	GPtrArray *list = g_ptr_array_new();
	const AxisUser *owner;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, list);

	owner = axis_session_store_get_user(self, user_id);
	if (owner == NULL)
		return list;

	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisSession *session = value;

		/* The account page's list: the engine's own sessions. */
		if (session->user_id == user_id && session->kind == AXIS_SESSION_WEB && session_fits(session, owner) &&
		    !session_expired(session, now_us))
			g_ptr_array_add(list, session);
	}
	g_ptr_array_sort(list, compare_recent_first);

	return list;
}

guint
axis_session_store_count(AxisSessionStore *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_hash_table_size(self->sessions);
}

void
axis_session_store_flush(AxisSessionStore *self)
{
	GString *hashes;
	GString *times;
	GHashTableIter iter;
	gpointer key;
	guint n = 0;
	Op *op;

	g_return_if_fail(self != NULL);

	if (g_hash_table_size(self->dirty) == 0)
		return;

	/* Array literals: the hashes are hex digits and the times digits, so
	 * neither needs quoting. */
	hashes = g_string_new("{");
	times = g_string_new("{");
	g_hash_table_iter_init(&iter, self->dirty);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		const AxisSession *session = g_hash_table_lookup(self->sessions, key);

		if (session == NULL)
			continue;
		if (n > 0) {
			g_string_append_c(hashes, ',');
			g_string_append_c(times, ',');
		}
		g_string_append(hashes, session->token_hash);
		g_string_append_printf(times, "%" G_GINT64_FORMAT, session->last_seen_us);
		n++;
	}
	g_string_append_c(hashes, '}');
	g_string_append_c(times, '}');
	g_hash_table_remove_all(self->dirty);

	if (n == 0) {
		g_string_free(hashes, TRUE);
		g_string_free(times, TRUE);
		return;
	}

	op = g_new0(Op, 1);
	op->kind = OP_TOUCH;
	op->hashes = g_string_free(hashes, FALSE);
	op->times = g_string_free(times, FALSE);
	push(self, op);
}

void
axis_session_store_sweep(
	AxisSessionStore *self,
	gint64            now_us
){
	g_autoptr(GPtrArray) doomed = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	guint i;
	Op *op;

	g_return_if_fail(self != NULL);

	g_hash_table_iter_init(&iter, self->sessions);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisSession *session = value;

		if (session_expired(session, now_us))
			g_ptr_array_add(doomed, session);
	}
	for (i = 0; i < doomed->len; i++)
		drop_session(self, g_ptr_array_index(doomed, i), FALSE);

	/* One statement for every row, including the ones never loaded. */
	op = g_new0(Op, 1);
	op->kind = OP_PURGE;
	op->now_us = now_us;
	push(self, op);
}
