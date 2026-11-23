/*
 * axis-indexer-conn.c - a connection to one configured source
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The only place a source's key is read (through the store) and handed to
 * anything: the request made to the source's own origin, which sends it
 * there only (in the header the kind names, or for a native indexer in the
 * query parameter its kind names, on the requests made to its own origin
 * and never on a fetch reference) and blanks it out of every message.  Every
 * request takes a token of the source's limiter first, so a search, a list
 * of indexers and a fetch share one budget.
 */

#include "indexer/axis-indexer-private.h"

#include <string.h>

#include "axis-version.h"

#define USER_AGENT "Axis/" AXIS_VERSION

/* How often a call waiting for a place looks at whether it was cancelled. */
#define WAIT_STEP_US (200 * 1000)

void
axis_indexer_reply_clear(AxisIndexerReply *reply)
{
	if (reply == NULL)
		return;
	g_clear_pointer(&reply->magnet, g_free);
	g_clear_pointer(&reply->refusal, g_free);
	reply->status = 0;
	reply->challenged = FALSE;
}

/* ── Calls ──────────────────────────────────────────────────────── */

static void
apply_rate(AxisIndexerConn *conn)
{
	gdouble rate = axis_indexer_setting_decimal(conn->owner, AXIS_INDEXER_SETTING_RATE, 1.0);
	guint burst = (guint)axis_indexer_setting_integer(conn->owner, AXIS_INDEXER_SETTING_BURST, 5);

	/* A public site has a rate of its own, and no burst: one request, then
	 * the interval. */
	if (conn->kind->site) {
		rate = axis_indexer_setting_decimal(conn->owner, AXIS_INDEXER_SETTING_SITE_RATE, 0.5);
		burst = 1;
	}
	axis_meta_limiter_set_rate(conn->slot->limiter, rate, burst);
}

/* Why the source is not asked before the real time @until. */
static void
set_held_error(
	AxisIndexerConn *conn,
	gint64           until,
	gboolean         paused,
	GError         **error
){
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(until / G_USEC_PER_SEC);
	g_autofree gchar *text = when != NULL ? g_date_time_format(when, "%H:%M:%S UTC") : g_strdup("later");

	if (paused)
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
			"%s asked Axis to wait, so it is not asked again until %s", conn->label, text);
	else
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREACHABLE,
			"%s did not answer the last times it was asked, so it is not asked again until %s", conn->label, text);
}

/* Waits for the source's budget, as long as the call may. */
static gboolean
take_token(
	AxisIndexerConn  *conn,
	GError          **error
){
	AxisMetaLimitStatus status;
	gint64 until = 0;
	gint64 held;

	apply_rate(conn);
	switch (axis_meta_limiter_acquire(conn->slot->limiter, conn->deadline_us, conn->cancel, &until)) {
	case AXIS_META_LIMIT_OK:
		return TRUE;
	case AXIS_META_LIMIT_CANCELLED:
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "cancelled");
		return FALSE;
	case AXIS_META_LIMIT_PAUSED:
		memset(&status, 0, sizeof(status));
		axis_meta_limiter_get_status(conn->slot->limiter, &status);
		held = MAX(status.paused_until_us, status.backoff_until_us);
		set_held_error(conn, held > 0 ? held : until, status.paused_until_us >= status.backoff_until_us, error);
		axis_meta_limit_status_clear(&status);
		return FALSE;
	default:
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
			"%s's request limit leaves no room for this in time", conn->label);
		return FALSE;
	}
}

/* What a failed request means for the source and the caller. */
static void
report_failure(
	AxisIndexerConn        *conn,
	const AxisHttpResponse *response,
	const GError           *local,
	AxisIndexerReply       *reply,
	GError                **error
){
	AxisMetaLimiter *limiter = conn->slot->limiter;
	guint status = response->status;

	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CANCELLED)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "cancelled");
		return;
	}
	if (response->offsite) {
		/* The source answered, with the way to the indexer's site: what went
		 * wrong there is the indexer's, not the source's (its 401 is not the
		 * source refusing its key), so the source is not held to it. */
		axis_meta_limiter_answered(limiter);
		if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_TOO_LARGE)) {
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE, local->message);
		} else if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_REDIRECT)) {
			if (reply != NULL && response->location != NULL &&
			    g_ascii_strncasecmp(response->location, "magnet:", 7) == 0)
				reply->magnet = g_strdup(response->location);
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT, local->message);
		} else {
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
				"the release could not be fetched from the indexer's site: %s", local->message);
		}
		return;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_TOO_LARGE)) {
		/* It answered; the answer is not one that is taken. */
		axis_meta_limiter_answered(limiter);
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE, local->message);
		return;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_REDIRECT)) {
		axis_meta_limiter_answered(limiter);
		if (reply != NULL && response->location != NULL && g_ascii_strncasecmp(response->location, "magnet:", 7) == 0)
			reply->magnet = g_strdup(response->location);
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT, local->message);
		return;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS)) {
		if (status == 401 || status == 403) {
			axis_meta_limiter_answered(limiter);
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_AUTH, local->message);
			return;
		}
		if (status == 429 || (status == 503 && response->retry_after_s >= 0)) {
			gint64 seconds = response->retry_after_s;
			g_autofree gchar *message = seconds >= 0 ?
				g_strdup_printf("%s answered %u and asked Axis to wait %" G_GINT64_FORMAT " s", conn->label, status,
					seconds) :
				g_strdup_printf("%s answered %u (too many requests)", conn->label, status);

			axis_meta_limiter_pause(limiter, seconds, message);
			/* One that runs a definition keeps the site's wait as its hold, in
			 * words that say whose it is: searches, tests and checks honour it,
			 * and nothing of Axis's lifts it (axis_indexer_hold_cause()). */
			if (conn->has_native && conn->definition != NULL) {
				AxisMetaLimitStatus held;
				gint64 now = g_get_real_time();

				memset(&held, 0, sizeof(held));
				axis_meta_limiter_get_status(limiter, &held);
				conn->learned.held_until_us = held.paused_until_us > now ? held.paused_until_us :
					now + 60 * G_USEC_PER_SEC;
				axis_meta_limit_status_clear(&held);
				g_free(conn->learned.hold_message);
				conn->learned.hold_message = g_strconcat(conn->label, AXIS_INDEXER_HOLD_WAIT_WORDS, NULL);
			}
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED, message);
			return;
		}
		if (status >= 400 && status < 500) {
			/* It answered; what was asked it does not take. */
			axis_meta_limiter_answered(limiter);
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED, local->message);
			return;
		}
		axis_meta_limiter_failed(limiter, local->message);
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED, local->message);
		return;
	}
	if (g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, local->message);
		return;
	}
	axis_meta_limiter_failed(limiter, local->message);
	g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREACHABLE, local->message);
}

/* A redirect off the origin that following would have taken: an address on
 * the web, not a magnet link. */
static gboolean
is_web_address(const gchar *location)
{
	return location != NULL && (g_ascii_strncasecmp(location, "https://", 8) == 0 ||
		g_ascii_strncasecmp(location, "http://", 7) == 0);
}

/* @release: the fetch of a release's file, which may follow the one
 * redirect to the indexer's site when the source allows it. */
static gboolean
do_get(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	gboolean                     release,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
){
	AxisHttpConfig config;
	AxisHttpResponse response;
	g_autoptr(GError) local = NULL;
	const AxisHttpOrigin *origin = &conn->origin;
	guint timeout_ms;
	gboolean ok;

	*body = NULL;
	axis_indexer_reply_clear(reply);
	if (options != NULL && options->files_origin) {
		/* The other origin accepted for a native indexer's files: only a
		 * release's fetch, and only while it is accepted. */
		if (!release || conn->files_origin.base == NULL) {
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
				"%s's files were found at another address, which is no longer accepted for it; nothing was sent",
				conn->label);
			return FALSE;
		}
		origin = &conn->files_origin;
	}
	if (!take_token(conn, error))
		return FALSE;
	timeout_ms = conn->owner->tuning.request_timeout_ms != 0 ? conn->owner->tuning.request_timeout_ms :
		options != NULL && options->timeout_ms != 0 ? options->timeout_ms :
		options != NULL && options->slow ? AXIS_INDEXER_SLOW_TIMEOUT_MS : AXIS_INDEXER_REQUEST_TIMEOUT_MS;
	if (conn->has_native) {
		if (release)
			conn->learned.n_grabs++;
		else
			conn->learned.n_calls++;
	}

	memset(&config, 0, sizeof(config));
	config.origin = origin;
	/* A native indexer's key is in the query (axis_indexer_conn_get());
	 * nothing is sent in a header, and nothing at all to its files' origin. */
	config.auth_header = conn->kind->key_header;
	config.auth_value = conn->kind->key_header != NULL ? conn->secret : NULL;
	config.token = conn->secret;
	config.accept_untrusted_certificate = conn->accept_untrusted;
	config.timeout_ms = timeout_ms;
	config.max_bytes = options != NULL && options->max_bytes != 0 ? options->max_bytes : AXIS_INDEXER_MAX_ANSWER_BYTES;
	config.cancellable = conn->cancel;
	config.peer = conn->has_native ? "the indexer" : "Prowlarr";
	config.secret_name = "key";
	config.user_agent = USER_AGENT;
	config.accept = options != NULL && options->accept != NULL ? options->accept : "application/json";
	/* Searches and release files, and the indexer's site after a redirect:
	 * outbound.bind applies when the address is public (core/axis-outbound.h). */
	config.traffic = AXIS_OUTBOUND_TRAFFIC_INDEXER;
	if (release && conn->follow_redirects)
		ok = axis_http_fetch_release(&config, path, options != NULL ? options->headers : NULL, body, &response, &local);
	else
		ok = axis_http_get_full(&config, path, options != NULL ? options->headers : NULL, body, &response, &local);
	if (reply != NULL) {
		reply->status = response.status;
		if (response.refusal != NULL && !response.offsite)
			reply->refusal = g_strdup(response.refusal);
		reply->challenged = response.challenged && !response.offsite;
	}
	if (ok) {
		axis_meta_limiter_answered(conn->slot->limiter);
		axis_http_response_clear(&response);
		return TRUE;
	}
	if (release && conn->has_native && g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_REDIRECT) &&
	    is_web_address(response.location)) {
		/* A native indexer's download that leads elsewhere is not followed:
		 * its files come from its own address or the one accepted for it. */
		axis_meta_limiter_answered(conn->slot->limiter);
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT,
			"%s redirected this release's download to another address, which Axis does not follow, so nothing "
			"was sent there", conn->label);
		axis_http_response_clear(&response);
		return FALSE;
	}
	if (release && !conn->follow_redirects && g_error_matches(local, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_REDIRECT) &&
	    is_web_address(response.location)) {
		/* The indexer redirects its downloads and following is off: say so,
		 * and how to resolve it.  Nothing was sent there. */
		axis_meta_limiter_answered(conn->slot->limiter);
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT,
			"%s redirects this release's download to the indexer's own site, and following those redirects is "
			"switched off for this indexer, so nothing was sent there. Either turn off Redirect for that indexer in "
			"%s, or allow following download redirects in this indexer's settings in Axis", conn->label,
			conn->kind->label);
		axis_http_response_clear(&response);
		return FALSE;
	}
	report_failure(conn, &response, local, reply, error);
	axis_http_response_clear(&response);
	return FALSE;
}

gboolean
axis_indexer_conn_get(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
){
	g_autofree gchar *full = NULL;

	g_return_val_if_fail(conn != NULL && path != NULL && body != NULL, FALSE);

	*body = NULL;
	full = conn->base_path != NULL && conn->base_path[0] != '\0' ? g_strconcat(conn->base_path, path, NULL) :
		g_strdup(path);
	if (!axis_indexer_path_ok(full, conn->base_path) || (options != NULL && options->files_origin)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"the request to the indexer is not a valid path; nothing was sent");
		return FALSE;
	}
	if (conn->kind->key_param != NULL && conn->secret != NULL && conn->secret[0] != '\0') {
		/* The protocol's way: the key in the query, to the origin only. */
		g_autofree gchar *escaped = g_uri_escape_string(conn->secret, NULL, FALSE);
		gchar *keyed = g_strdup_printf("%s%c%s=%s", full, strchr(full, '?') != NULL ? '&' : '?',
			conn->kind->key_param, escaped);
		gboolean ok;

		memset(escaped, 0, strlen(escaped));
		ok = do_get(conn, keyed, options, FALSE, body, reply, error);
		memset(keyed, 0, strlen(keyed));
		g_free(keyed);
		return ok;
	}
	return do_get(conn, full, options, FALSE, body, reply, error);
}

gboolean
axis_indexer_conn_get_ref(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
){
	g_return_val_if_fail(conn != NULL && path != NULL && body != NULL, FALSE);

	*body = NULL;
	/* A native indexer's files may be anywhere on its origin (a Torznab
	 * provider serves them outside the feed's path). */
	if (!axis_indexer_path_ok(path, conn->has_native ? "" : conn->base_path)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"the release's address is not one under the indexer's own; nothing was sent");
		return FALSE;
	}
	return do_get(conn, path, options, FALSE, body, reply, error);
}

gboolean
axis_indexer_conn_fetch_release(
	AxisIndexerConn             *conn,
	const gchar                 *path,
	const AxisIndexerGetOptions *options,
	GBytes                     **body,
	AxisIndexerReply            *reply,
	GError                     **error
){
	g_return_val_if_fail(conn != NULL && path != NULL && body != NULL, FALSE);

	*body = NULL;
	/* A native indexer's files may be anywhere on its origin (a Torznab
	 * provider serves them outside the feed's path). */
	if (!axis_indexer_path_ok(path, conn->has_native ? "" : conn->base_path)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"the release's address is not one under the indexer's own; nothing was sent");
		return FALSE;
	}
	return do_get(conn, path, options, TRUE, body, reply, error);
}

gboolean
axis_indexer_conn_get_once(
	const AxisHttpOrigin  *origin,
	const gchar           *path,
	const gchar *const    *headers,
	const gchar           *accept,
	gsize                  max_bytes,
	GBytes               **body,
	guint                 *status,
	gchar                **refusal,
	gboolean              *challenged,
	GError               **error
){
	AxisHttpConfig config;
	AxisHttpResponse response;
	gboolean ok;

	g_return_val_if_fail(origin != NULL && body != NULL && status != NULL && refusal != NULL, FALSE);

	*body = NULL;
	*status = 0;
	*refusal = NULL;
	if (challenged != NULL)
		*challenged = FALSE;
	if (path == NULL || !axis_indexer_path_ok(path, "")) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"the request to the site is not a valid path; nothing was sent");
		return FALSE;
	}
	memset(&config, 0, sizeof(config));
	config.origin = origin;
	config.timeout_ms = AXIS_INDEXER_NATIVE_TIMEOUT_MS;
	config.max_bytes = max_bytes != 0 ? max_bytes : AXIS_INDEXER_MAX_ANSWER_BYTES;
	config.peer = "the site";
	config.secret_name = "key";
	config.user_agent = USER_AGENT;
	config.accept = accept != NULL ? accept : "application/json";
	config.traffic = AXIS_OUTBOUND_TRAFFIC_INDEXER;
	ok = axis_http_get_full(&config, path, headers, body, &response, error);
	*status = response.status;
	if (response.refusal != NULL)
		*refusal = g_strdup(response.refusal);
	if (challenged != NULL)
		*challenged = response.challenged;
	axis_http_response_clear(&response);
	return ok;
}

const GPtrArray *
axis_indexer_conn_get_indexers(AxisIndexerConn *conn)
{
	static GPtrArray *none = NULL;

	if (conn->indexers != NULL)
		return conn->indexers;
	if (g_once_init_enter(&none))
		g_once_init_leave(&none, g_ptr_array_new());
	return none;
}

const AxisHttpOrigin *
axis_indexer_conn_get_origin(AxisIndexerConn *conn)
{
	return &conn->origin;
}

const gchar *
axis_indexer_conn_get_base_path(AxisIndexerConn *conn)
{
	return conn->base_path != NULL ? conn->base_path : "";
}

const gchar *
axis_indexer_conn_get_label(AxisIndexerConn *conn)
{
	return conn->label;
}

const gchar *
axis_indexer_conn_get_definition(AxisIndexerConn *conn)
{
	return conn->kind->definition ? conn->definition : NULL;
}

const AxisIndexerNative *
axis_indexer_conn_get_native(AxisIndexerConn *conn)
{
	return conn->has_native ? &conn->native : NULL;
}

void
axis_indexer_conn_learn_caps(
	AxisIndexerConn       *conn,
	const AxisIndexerCaps *caps
){
	if (!conn->has_native || caps == NULL)
		return;
	axis_indexer_caps_clear(&conn->learned.caps);
	axis_indexer_caps_copy(caps, &conn->learned.caps);
	conn->learned.caps.read_us = g_get_real_time();
	conn->learned.caps_learned = TRUE;
	/* What the kind knows of the indexer from now on, in this call too. */
	conn->native.caps = &conn->learned.caps;
}

void
axis_indexer_conn_learn_allowance(
	AxisIndexerConn            *conn,
	const AxisIndexerAllowance *allowance
){
	if (!conn->has_native || allowance == NULL || allowance->reported_us == 0)
		return;
	conn->learned.allowance = *allowance;
	conn->learned.allowance.reported_us = g_get_real_time();
	conn->learned.allowance_learned = TRUE;
	/* Counted from this report on. */
	conn->learned.n_calls = 0;
	conn->learned.n_grabs = 0;
}

void
axis_indexer_conn_learn_other_origin(
	AxisIndexerConn *conn,
	const gchar     *origin
){
	AxisHttpOrigin parsed;
	g_autofree gchar *problem = NULL;

	if (!conn->has_native || origin == NULL || conn->learned.other_origin != NULL)
		return;
	/* Only an origin as one is configured, so that it can be accepted as
	 * typed. */
	if (!axis_http_origin_parse(origin, &parsed, &problem))
		return;
	conn->learned.other_origin = g_strdup(parsed.base);
	axis_http_origin_clear(&parsed);
}

void
axis_indexer_conn_hold(
	AxisIndexerConn *conn,
	gint64           seconds,
	gboolean         grabs,
	const gchar     *message
){
	gint64 until = g_get_real_time() + CLAMP(seconds, 60, 86400) * G_USEC_PER_SEC;

	if (!conn->has_native)
		return;
	g_free(conn->learned.hold_message);
	conn->learned.hold_message = axis_indexer_text_clean(message, 300);
	if (grabs) {
		conn->learned.grabs_held_until_us = until;
	} else {
		conn->learned.held_until_us = until;
		axis_meta_limiter_pause(conn->slot->limiter, CLAMP(seconds, 60, 86400), message);
	}
}

void
axis_indexer_conn_hold_searches(
	AxisIndexerConn *conn,
	gint64           seconds,
	const gchar     *message
){
	if (!conn->has_native)
		return;
	g_free(conn->learned.hold_message);
	conn->learned.hold_message = axis_indexer_text_clean(message, 300);
	conn->learned.held_until_us = g_get_real_time() + CLAMP(seconds, 60, 86400) * G_USEC_PER_SEC;
}

struct _AxisDefTrace *
axis_indexer_conn_get_trace(AxisIndexerConn *conn)
{
	return conn->trace;
}

gchar *
axis_indexer_conn_scrub(
	AxisIndexerConn *conn,
	const gchar     *text,
	guint            max_chars
){
	g_autofree gchar *queries = axis_http_scrub_queries(text);
	g_autofree gchar *blanked = axis_http_scrub(queries, conn->secret, max_chars);

	return axis_indexer_text_clean(blanked, max_chars);
}

/* ── With a source ──────────────────────────────────────────────── */

static gboolean
load_target(
	AxisIndexers       *self,
	gint64              id,
	AxisIndexerTarget  *target,
	GError            **error
){
	g_autoptr(GPtrArray) sources = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_sources(conn, id, &sources, error) &&
		(sources->len == 0 || axis_indexer_store_secret(conn, id, &target->secret, error));
	/* The definition it runs, read with it. */
	if (ok && sources->len > 0 && ((AxisIndexerSourceInfo *)g_ptr_array_index(sources, 0))->definition_id != 0)
		ok = axis_indexer_store_definition_body(conn,
			((AxisIndexerSourceInfo *)g_ptr_array_index(sources, 0))->definition_id, &target->definition, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (sources->len == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such indexer");
		return FALSE;
	}
	target->info = g_ptr_array_steal_index(sources, 0);
	return TRUE;
}

static void
on_cancel(
	GCancellable *source,
	gpointer      user_data
){
	g_cancellable_cancel(G_CANCELLABLE(user_data));
}

/* A place among the calls running at the source, waiting for one. */
static gboolean
take_place(
	AxisIndexers     *self,
	AxisIndexerSlot  *slot,
	GCancellable     *cancellable,
	gint64            deadline_us,
	GError          **error
){
	guint limit = (guint)axis_indexer_setting_integer(self, AXIS_INDEXER_SETTING_MAX_SIMULTANEOUS, 2);
	gboolean ok = FALSE;

	g_mutex_lock(&slot->lock);
	while (TRUE) {
		gint64 now;

		if (g_atomic_int_get(&self->stopped) || g_cancellable_is_cancelled(cancellable)) {
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "cancelled");
			break;
		}
		if (slot->active < limit) {
			slot->active++;
			ok = TRUE;
			break;
		}
		now = g_get_monotonic_time();
		if (now >= deadline_us) {
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_BUSY,
				"the indexer is busy with other searches; try again in a moment");
			break;
		}
		g_cond_wait_until(&slot->cond, &slot->lock, MIN(deadline_us, now + WAIT_STEP_US));
	}
	g_mutex_unlock(&slot->lock);
	return ok;
}

gboolean
axis_indexer_with_source(
	AxisIndexers         *self,
	gint64                id,
	gboolean              only_enabled,
	GCancellable         *cancellable,
	AxisIndexerCallFunc   func,
	gpointer              data,
	GError              **error
){
	AxisIndexerTarget target;
	const AxisIndexerKindInfo *kind;
	AxisIndexerSlot *slot;
	AxisIndexerConn conn;
	g_autoptr(GCancellable) call_cancel = NULL;
	g_autofree gchar *problem = NULL;
	gulong own_handler = 0;
	gulong caller_handler = 0;
	gint64 deadline_us;
	gboolean ok;

	if (g_atomic_int_get(&self->stopped)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "the daemon is stopping");
		return FALSE;
	}
	memset(&target, 0, sizeof(target));
	if (!load_target(self, id, &target, error))
		return FALSE;
	if (only_enabled && !target.info->enabled) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE,
			"that indexer is switched off");
		axis_indexer_target_clear(&target);
		return FALSE;
	}
	kind = axis_indexers_find_kind(self, target.info->kind);
	if (kind == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"this version of Axis has no kind of indexer called %s", target.info->kind);
		axis_indexer_target_clear(&target);
		return FALSE;
	}

	memset(&conn, 0, sizeof(conn));
	if (!axis_http_origin_parse(target.info->address, &conn.origin, &problem)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "the indexer's address %s",
			problem != NULL ? problem : "cannot be used");
		axis_indexer_target_clear(&target);
		return FALSE;
	}

	slot = axis_indexer_slot_get(self, id);
	deadline_us = g_get_monotonic_time() +
		(gint64)(self->tuning.wait_ms != 0 ? self->tuning.wait_ms : AXIS_INDEXER_WAIT_MS) * 1000;
	call_cancel = g_cancellable_new();
	own_handler = g_cancellable_connect(self->cancel, G_CALLBACK(on_cancel), call_cancel, NULL);
	if (cancellable != NULL)
		caller_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancel), call_cancel, NULL);
	if (!take_place(self, slot, call_cancel, deadline_us, error)) {
		g_cancellable_disconnect(self->cancel, own_handler);
		if (cancellable != NULL)
			g_cancellable_disconnect(cancellable, caller_handler);
		axis_indexer_slot_unref(slot);
		axis_http_origin_clear(&conn.origin);
		axis_indexer_target_clear(&target);
		return FALSE;
	}

	conn.owner = self;
	conn.kind = kind;
	conn.slot = slot;
	conn.base_path = target.info->base_path;
	conn.secret = target.secret;
	conn.label = target.info->name;
	conn.accept_untrusted = target.info->accept_untrusted;
	/* A native indexer's download is never followed off its origin. */
	conn.follow_redirects = target.info->follow_redirects && !kind->native;
	conn.indexers = target.info->indexers;
	conn.cancel = call_cancel;
	conn.deadline_us = deadline_us;
	conn.definition = target.definition;
	if (kind->native) {
		conn.has_native = TRUE;
		conn.native.protocol = kind->protocol;
		conn.native.caps = &target.info->caps;
		conn.native.movie_categories = target.info->movie_categories;
		conn.native.n_movie_categories = target.info->n_movie_categories;
		conn.native.tv_categories = target.info->tv_categories;
		conn.native.n_tv_categories = target.info->n_tv_categories;
		conn.native.priority = target.info->priority;
		if (target.info->files_origin != NULL &&
		    axis_http_origin_parse(target.info->files_origin, &conn.files_origin, NULL))
			conn.native.files_origin = &conn.files_origin;
	}
	ok = func(self, &target, kind, &conn, data, error);

	g_mutex_lock(&slot->lock);
	slot->active--;
	g_cond_broadcast(&slot->cond);
	g_mutex_unlock(&slot->lock);
	g_cancellable_disconnect(self->cancel, own_handler);
	if (cancellable != NULL)
		g_cancellable_disconnect(cancellable, caller_handler);
	if (conn.has_native) {
		/* What the indexer told on the way, whatever came of the call. */
		g_autoptr(GError) local = NULL;
		AxisDbConn *db = axis_db_pool_acquire(self->pool, &local);

		if (db != NULL) {
			if (!axis_indexer_store_learned(db, id, &conn.learned, &local))
				g_debug("indexers: cannot record what indexer %" G_GINT64_FORMAT " reported: %s", id, local->message);
			axis_db_pool_release(self->pool, db);
		}
		axis_indexer_learned_clear(&conn.learned);
		axis_http_origin_clear(&conn.files_origin);
	}
	axis_indexer_slot_unref(slot);
	axis_http_origin_clear(&conn.origin);
	axis_indexer_target_clear(&target);
	return ok;
}
