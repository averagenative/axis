/*
 * axis-requests.c - the requests layer: its names and words, the calendar
 * window, its settings and its module
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "requests/axis-requests-private.h"

#include <string.h>

G_DEFINE_QUARK(axis-requests-error-quark, axis_requests_error)

/* ── Names and words ────────────────────────────────────────────── */

const gchar *
axis_request_kind_name(AxisRequestKind kind)
{
	return kind == AXIS_REQUEST_SERIES ? "series" : "movie";
}

gboolean
axis_request_kind_from_name(
	const gchar     *name,
	AxisRequestKind *kind
){
	if (g_strcmp0(name, "movie") == 0)
		*kind = AXIS_REQUEST_MOVIE;
	else if (g_strcmp0(name, "series") == 0)
		*kind = AXIS_REQUEST_SERIES;
	else
		return FALSE;
	return TRUE;
}

static const gchar *const decision_names[] = { "waiting", "approved", "declined", "withdrawn", "replaced", "removed" };

const gchar *
axis_request_decision_name(AxisRequestDecision decision)
{
	return (guint)decision < G_N_ELEMENTS(decision_names) ? decision_names[decision] : "waiting";
}

gboolean
axis_request_seasons_from_name(
	const gchar        *name,
	AxisRequestSeasons *seasons
){
	static const gchar *const names[] = { "all", "first", "latest", "picked" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		if (g_strcmp0(name, names[i]) == 0) {
			*seasons = (AxisRequestSeasons)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_request_state_name(AxisRequestState state)
{
	static const gchar *const names[] = {
		"can", "closed", "withdrawn", "removed", "declined", "requested", "approved", "failed", "downloading",
		"partly", "available"
	};

	return (guint)state < G_N_ELEMENTS(names) ? names[state] : "can";
}

const gchar *
axis_request_state_label(AxisRequestState state)
{
	static const gchar *const labels[] = {
		"Can be requested", "Closed to requests", "Withdrawn", "Removed from Axis", "Declined", "Requested",
		"Approved", "Failed", "Downloading", "Partly available", "Available"
	};

	return (guint)state < G_N_ELEMENTS(labels) ? labels[state] : "Can be requested";
}

const gchar *
axis_request_cause_words(AxisRequestCause cause)
{
	switch (cause) {
	case AXIS_REQUEST_CAUSE_NOT_ADDED:
		return "The title could not be added.";
	case AXIS_REQUEST_CAUSE_DOWNLOAD_FAILED:
		return "A download failed. Axis is looking again.";
	case AXIS_REQUEST_CAUSE_NEEDS_ADMIN:
		return "A download finished and needs the administrator.";
	default:
		return "";
	}
}

gchar *
axis_request_seasons_words(const GArray *seasons)
{
	GString *words;
	guint i;

	if (seasons == NULL || seasons->len == 0)
		return NULL;
	if (seasons->len == 1) {
		gint only = g_array_index(seasons, gint, 0);

		return only == 0 ? g_strdup("Specials") : g_strdup_printf("Season %d", only);
	}
	words = g_string_new("Seasons ");
	for (i = 0; i < seasons->len; i++) {
		gint number = g_array_index(seasons, gint, i);

		if (i > 0)
			g_string_append(words, i + 1 == seasons->len ? " and " : ", ");
		if (number == 0)
			g_string_append(words, "0 (specials)");
		else
			g_string_append_printf(words, "%d", number);
	}
	return g_string_free(words, FALSE);
}

gchar *
axis_request_info_words(const AxisRequestInfo *info)
{
	switch (info->state) {
	case AXIS_REQUEST_STATE_FAILED:
		return g_strdup_printf("Failed: %s", axis_request_cause_words(info->cause));
	case AXIS_REQUEST_STATE_AVAILABLE:
		/* The file is in the library folder; whether a media server has
		 * scanned it is not Axis's to know. */
		return g_strdup("Available: in the library");
	case AXIS_REQUEST_STATE_PARTLY:
		return g_strdup_printf("Partly available, %u of %u episodes", info->have, info->aired);
	case AXIS_REQUEST_STATE_REQUESTED:
		return g_strdup("Requested: waits for approval");
	case AXIS_REQUEST_STATE_APPROVED:
		return g_strdup(g_strcmp0(info->approved_how, "auto") == 0 ? "Approved automatically: Axis is looking for it" :
			"Approved: Axis is looking for it");
	case AXIS_REQUEST_STATE_DECLINED:
		/* Replaced with no reason of a decline: two records of its title
		 * merged, and another request for the same thing stayed. */
		if (info->decision == AXIS_REQUEST_REPLACED && info->decline_reason == NULL)
			return g_strdup("Joined to another request for the same title");
		return g_strdup(axis_request_state_label(info->state));
	default:
		return g_strdup(axis_request_state_label(info->state));
	}
}

/* ── The calendar window ────────────────────────────────────────── */

gboolean
axis_request_window_from_name(
	const gchar       *name,
	AxisRequestWindow *window
){
	if (g_strcmp0(name, "day") == 0)
		*window = AXIS_REQUEST_WINDOW_DAY;
	else if (g_strcmp0(name, "week") == 0)
		*window = AXIS_REQUEST_WINDOW_WEEK;
	else if (g_strcmp0(name, "month") == 0)
		*window = AXIS_REQUEST_WINDOW_MONTH;
	else
		return FALSE;
	return TRUE;
}

static gint64
to_us(GDateTime *when)
{
	return g_date_time_to_unix(when) * G_USEC_PER_SEC;
}

void
axis_request_window_bounds(
	GTimeZone         *zone,
	gint64             now_us,
	AxisRequestWindow  window,
	gint64            *start_us,
	gint64            *end_us
){
	/* (Floor, for a moment before 1970.) */
	gint64 seconds = now_us >= 0 ? now_us / G_USEC_PER_SEC : -((-now_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);
	g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(seconds);
	g_autoptr(GDateTime) local = utc != NULL ? g_date_time_to_timezone(utc, zone) : NULL;
	g_autoptr(GDateTime) day = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;

	*start_us = now_us;
	*end_us = now_us;
	if (local == NULL)
		return;
	/* Midnight as the calendar has it that day: adding days or months keeps
	 * the time of day, so a day of 23 or 25 hours is still one day. */
	day = g_date_time_new(zone, g_date_time_get_year(local), g_date_time_get_month(local),
		g_date_time_get_day_of_month(local), 0, 0, 0);
	if (day == NULL)
		return;
	switch (window) {
	case AXIS_REQUEST_WINDOW_DAY:
		start = g_date_time_ref(day);
		end = g_date_time_add_days(start, 1);
		break;
	case AXIS_REQUEST_WINDOW_MONTH:
		start = g_date_time_new(zone, g_date_time_get_year(local), g_date_time_get_month(local), 1, 0, 0, 0);
		end = start != NULL ? g_date_time_add_months(start, 1) : NULL;
		break;
	default:
		/* Monday is day 1. */
		start = g_date_time_add_days(day, -(g_date_time_get_day_of_week(local) - 1));
		end = start != NULL ? g_date_time_add_days(start, 7) : NULL;
		break;
	}
	if (start == NULL || end == NULL)
		return;
	*start_us = to_us(start);
	*end_us = to_us(end);
}

const gchar *
axis_request_window_until(AxisRequestWindow window)
{
	switch (window) {
	case AXIS_REQUEST_WINDOW_DAY:
		return "tomorrow";
	case AXIS_REQUEST_WINDOW_MONTH:
		return "the 1st of next month";
	default:
		return "Monday";
	}
}

static const gchar *
window_this(AxisRequestWindow window)
{
	switch (window) {
	case AXIS_REQUEST_WINDOW_DAY:
		return "today";
	case AXIS_REQUEST_WINDOW_MONTH:
		return "this month";
	default:
		return "this week";
	}
}

gchar *
axis_request_quota_words(
	const AxisRequestQuota *quota,
	AxisRequestKind         kind
){
	const AxisRequestAllowance *allowance = kind == AXIS_REQUEST_SERIES ? &quota->seasons : &quota->movies;

	if (allowance->unlimited)
		return g_strdup("No limit");
	if (allowance->limit == 0)
		return g_strdup_printf("None: you cannot ask for %s", kind == AXIS_REQUEST_SERIES ? "series" : "movies");
	return g_strdup_printf("%u of %u left %s", allowance->left, allowance->limit, window_this(quota->window));
}

/* ── Small helpers ──────────────────────────────────────────────── */

gchar *
axis_requests_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

gchar *
axis_requests_array_text(const GArray *ids)
{
	GString *text = g_string_new("{");
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++)
		g_string_append_printf(text, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(text, '}');
	return g_string_free(text, FALSE);
}

void
axis_requests_parse_list(
	const gchar *text,
	gboolean     wide,
	GArray      *out
){
	g_auto(GStrv) parts = NULL;
	guint i;

	if (text == NULL || text[0] == '\0')
		return;
	parts = g_strsplit(text, ",", -1);
	for (i = 0; parts[i] != NULL; i++) {
		gint64 value = g_ascii_strtoll(parts[i], NULL, 10);

		if (wide) {
			g_array_append_val(out, value);
		} else {
			gint narrow = (gint)value;

			g_array_append_val(out, narrow);
		}
	}
}

/*
 * The locks are PostgreSQL's advisory locks of two keys: a class for what is
 * locked (a title, a person, a title's placing) and the id.  Ids of one class
 * that are equal modulo 2^31 - 1 share a lock, which only makes unrelated work
 * wait.  The title's key is also taken by the trigger that moves requests when
 * two records of a title merge (0083_requests.sql): the two must stay alike.
 */
#define LOCK_KEY(class) "hashtext('" class "'), ($1::bigint % 2147483647)::integer"

gboolean
axis_requests_lock_title(
	AxisDbConn  *conn,
	gint64       title_id,
	GError     **error
){
	g_autofree gchar *id = axis_requests_int_text(title_id);
	const gchar *params[] = { id };

	return axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(" LOCK_KEY("axis.request.title") ")", 1, params, NULL,
		error);
}

gboolean
axis_requests_lock_person(
	AxisDbConn  *conn,
	gint64       user_id,
	GError     **error
){
	g_autofree gchar *id = axis_requests_int_text(user_id);
	const gchar *params[] = { id };

	return axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(" LOCK_KEY("axis.request.person") ")", 1, params,
		NULL, error);
}

AxisDbConn *
axis_requests_lock_placing(
	AxisRequests  *self,
	gint64         title_id,
	GError       **error
){
	g_autofree gchar *id = axis_requests_int_text(title_id);
	const gchar *params[] = { id };
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);

	if (conn == NULL)
		return NULL;
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_lock(" LOCK_KEY("axis.request.placing") ")", 1, params, NULL,
	        error)) {
		axis_db_pool_release(self->pool, conn);
		return NULL;
	}
	return conn;
}

void
axis_requests_unlock_placing(
	AxisRequests *self,
	AxisDbConn   *conn,
	gint64        title_id
){
	g_autofree gchar *id = axis_requests_int_text(title_id);
	g_autoptr(GError) local = NULL;
	const gchar *params[] = { id };

	if (conn == NULL)
		return;
	/* A session's lock outlives a statement: let go of it before the
	 * connection goes back to the pool.  (A connection that failed to is a
	 * bad one, which the pool closes, and the lock goes with it.) */
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_unlock(" LOCK_KEY("axis.request.placing") ")", 1, params, NULL,
	        &local))
		g_warning("requests: the lock on placing title %" G_GINT64_FORMAT " could not be let go: %s", title_id,
			local->message);
	axis_db_pool_release(self->pool, conn);
}

/* ── The settings ───────────────────────────────────────────────── */

static const gchar *const window_choices[] = { "day", "week", "month", NULL };
static const gchar *const window_labels[] = { "A day", "A week, from Monday", "A month", NULL };

static const AxisSettingInfo settings_table[] = {
	{ AXIS_REQUESTS_SETTING_WINDOW, AXIS_SETTING_CHOICE, "A person's request limits count over",
	  "The calendar window, in the server's local time, that each person's limits below are counted over. A new "
	  "window starts everyone again.", 0, "week", 0, 0, 0, 0, window_choices },
	{ AXIS_REQUESTS_SETTING_MOVIES, AXIS_SETTING_INTEGER, "Movie requests for each person in that time",
	  "How many movies a person may ask for in one window. 0: nobody can ask for movies unless they have a limit of "
	  "their own. A person's own limit is set on the Requests page, under People; administrators are not limited.",
	  0, "5", 0, AXIS_REQUESTS_MAX_LIMIT, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_SEASONS, AXIS_SETTING_INTEGER, "Season requests for each person in that time",
	  "How many seasons of series a person may ask for in one window: a request for three seasons counts three. 0: "
	  "nobody can ask for series unless they have a limit of their own.", 0, "5", 0, AXIS_REQUESTS_MAX_LIMIT, 0, 0,
	  NULL },
	{ AXIS_REQUESTS_SETTING_AUTO_SEASONS, AXIS_SETTING_INTEGER, "Most seasons approved automatically in one request",
	  "For a person whose series requests are approved automatically: a request that lists more seasons than this "
	  "waits for an administrator.", 0, "3", 1, AXIS_REQUESTS_MAX_SEASONS, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_SHOW_WHO, AXIS_SETTING_BOOLEAN, "Show people who asked for a title",
	  "On: beside a requested title, everyone who can ask sees the name of the person who asked. Off: only "
	  "administrators and that person do.", 0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_MOVIES_ROOT, AXIS_SETTING_INTEGER, "Library folder for requested movies (its number)",
	  "Set on the Requests page, under Defaults, where the folders are listed by name. 0: none, so every movie "
	  "request asks the approving administrator.", 0, "0", 0, 0, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_MOVIES_PROFILE, AXIS_SETTING_INTEGER, "Quality profile for requested movies (its number)",
	  "Set on the Requests page, under Defaults. 0: none.", 0, "0", 0, 0, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_SERIES_ROOT, AXIS_SETTING_INTEGER, "Library folder for requested series (its number)",
	  "Set on the Requests page, under Defaults. 0: none, so every series request asks the approving "
	  "administrator.", 0, "0", 0, 0, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_SERIES_PROFILE, AXIS_SETTING_INTEGER, "Quality profile for requested series (its number)",
	  "Set on the Requests page, under Defaults. 0: none.", 0, "0", 0, 0, 0, 0, NULL },
	{ AXIS_REQUESTS_SETTING_PORTAL_DAY, AXIS_SETTING_INTEGER, "Requests through the request portal in one day",
	  "What all requests made through the request portal may count in one calendar day, whoever made them: a movie "
	  "counts one and each season one. Beyond it the portal's requests are refused until the next day. It bounds what "
	  "a portal in the wrong hands can ask for. One person's requests may count a third of it at most, withdrawn ones "
	  "included. 0: no request can be made through the portal.", 0, "30", 0, 10000, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

static gboolean
declared(
	AxisRequests *self,
	const gchar  *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

gint64
axis_requests_setting_int(
	AxisRequests *self,
	const gchar  *key,
	gint64        fallback
){
	return declared(self, key) ? axis_settings_get_integer(self->settings, key) : fallback;
}

AxisRequestWindow
axis_requests_window(AxisRequests *self)
{
	AxisRequestWindow window = AXIS_REQUEST_WINDOW_WEEK;

	if (declared(self, AXIS_REQUESTS_SETTING_WINDOW)) {
		g_autofree gchar *name = axis_settings_get_string(self->settings, AXIS_REQUESTS_SETTING_WINDOW);

		axis_request_window_from_name(name, &window);
	}
	return window;
}

gboolean
axis_requests_show_who(AxisRequests *self)
{
	return declared(self, AXIS_REQUESTS_SETTING_SHOW_WHO) ?
		axis_settings_get_boolean(self->settings, AXIS_REQUESTS_SETTING_SHOW_WHO) : TRUE;
}

/* ── The layer ──────────────────────────────────────────────────── */

AxisRequests *
axis_requests_new(
	AxisDbPool       *pool,
	AxisSettings     *settings,
	AxisMeta         *meta,
	AxisMovies       *movies,
	AxisTv           *tv,
	AxisStorage      *storage,
	AxisQualityStore *quality
){
	AxisRequests *self;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(meta != NULL, NULL);

	self = g_new0(AxisRequests, 1);
	g_atomic_ref_count_init(&self->ref);
	g_mutex_init(&self->portal_limit_lock);
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->meta = axis_meta_ref(meta);
	self->movies = movies != NULL ? axis_movies_ref(movies) : NULL;
	self->tv = tv != NULL ? axis_tv_ref(tv) : NULL;
	self->storage = storage != NULL ? axis_storage_ref(storage) : NULL;
	self->quality = quality != NULL ? axis_quality_store_ref(quality) : NULL;
	/* A requested title, and one closed to requests, is kept and kept fresh. */
	axis_meta_add_reference(meta, "requests", "SELECT title_id FROM request");
	axis_meta_add_reference(meta, "titles closed to requests", "SELECT title_id FROM request_closed_title");
	return self;
}

AxisRequests *
axis_requests_ref(AxisRequests *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_ref_count_inc(&self->ref);
	return self;
}

void
axis_requests_unref(AxisRequests *self)
{
	if (self == NULL || !g_atomic_ref_count_dec(&self->ref))
		return;
	g_clear_pointer(&self->settings, axis_settings_unref);
	g_clear_pointer(&self->meta, axis_meta_unref);
	g_clear_pointer(&self->movies, axis_movies_unref);
	g_clear_pointer(&self->tv, axis_tv_unref);
	g_clear_pointer(&self->storage, axis_storage_unref);
	g_clear_pointer(&self->quality, axis_quality_store_unref);
	g_clear_pointer(&self->zone, g_time_zone_unref);
	g_mutex_clear(&self->portal_limit_lock);
	g_free(self);
}

void
axis_requests_set_registry(
	AxisRequests       *self,
	AxisModuleRegistry *registry
){
	self->registry = registry;
}

gboolean
axis_requests_is_on(AxisRequests *self)
{
	return self->registry == NULL || axis_module_registry_is_enabled(self->registry, AXIS_REQUESTS_MODULE_ID);
}

void
axis_requests_set_portal(
	AxisRequests     *self,
	AxisPortalTokens *tokens,
	guint             port
){
	self->portal_tokens = tokens;
	self->portal_port = port;
}

/* ── The portal's ceilings, as they were reached ────────────────── */

void
axis_requests_portal_limit_reached(
	AxisRequests            *self,
	AxisRequestsPortalLimit  which,
	gint64                   not_again_since_us
){
	gint64 now = axis_requests_now(self);
	gint64 *at;
	guint n;

	g_return_if_fail(self != NULL);
	g_return_if_fail((guint)which < AXIS_REQUESTS_PORTAL_LIMITS);

	g_mutex_lock(&self->portal_limit_lock);
	at = self->portal_limit_at[which];
	n = self->portal_limit_n[which];
	if (n > 0 && not_again_since_us != 0 && at[n - 1] >= not_again_since_us) {
		g_mutex_unlock(&self->portal_limit_lock);
		return;
	}
	if (n == AXIS_REQUESTS_PORTAL_LIMIT_KEPT) {
		memmove(at, at + 1, (n - 1) * sizeof(*at));
		n--;
	}
	at[n++] = now;
	self->portal_limit_n[which] = n;
	g_mutex_unlock(&self->portal_limit_lock);
	/* Never who: the notice and the page name no account, and neither does
	 * this line. */
	g_message("requests: the portal reached its limit of %s", which == AXIS_REQUESTS_PORTAL_LIMIT_DAY ?
		"requests for a day (requests.portal_per_day); requests through it are refused until tomorrow" :
		"failed logins for an hour; logins through it are admitted one a minute for now");
}

guint
axis_requests_portal_limit_count(
	AxisRequests            *self,
	AxisRequestsPortalLimit  which,
	gint64                  *last_us
){
	gint64 since = axis_requests_now(self) - G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC;
	guint count = 0;
	guint i;

	g_return_val_if_fail(self != NULL, 0);
	g_return_val_if_fail((guint)which < AXIS_REQUESTS_PORTAL_LIMITS, 0);

	if (last_us != NULL)
		*last_us = 0;
	g_mutex_lock(&self->portal_limit_lock);
	for (i = 0; i < self->portal_limit_n[which]; i++) {
		if (self->portal_limit_at[which][i] <= since)
			continue;
		count++;
		if (last_us != NULL)
			*last_us = self->portal_limit_at[which][i];
	}
	g_mutex_unlock(&self->portal_limit_lock);
	return count;
}

gboolean
axis_requests_portal_today(
	AxisRequests  *self,
	gint64        *counted,
	gint64        *ceiling,
	GError       **error
){
	g_autoptr(GTimeZone) local = self->zone != NULL ? g_time_zone_ref(self->zone) : g_time_zone_new_local();
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *from = NULL;
	const gchar *params[1];
	gint64 start = 0;
	gint64 end = 0;

	*counted = 0;
	*ceiling = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_PORTAL_DAY, AXIS_REQUESTS_PORTAL_A_DAY);
	axis_request_window_bounds(local, axis_requests_now(self), AXIS_REQUEST_WINDOW_DAY, &start, &end);
	from = axis_requests_int_text(start);
	params[0] = from;
	/* As the ask's own count (axis-requests-ask.c): a movie one, a season one
	 * each, whoever made them and whatever became of them. */
	if (!axis_db_pool_exec(self->pool, "SELECT coalesce(sum(CASE WHEN r.kind = 'movie' THEN 1 ELSE (SELECT count(*) FROM"
	        " request_season s WHERE s.request_id = r.id) END), 0) FROM request r WHERE r.via = 'portal'"
	        " AND r.created_at >= " AXIS_REQUESTS_TS(1), 1, params, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, counted);
	return TRUE;
}

gboolean
axis_requests_kind_on(
	AxisRequests    *self,
	AxisRequestKind  kind
){
	const gchar *id = kind == AXIS_REQUEST_SERIES ? AXIS_TV_MODULE_ID : AXIS_MOVIES_MODULE_ID;

	if (kind == AXIS_REQUEST_SERIES ? self->tv == NULL : self->movies == NULL)
		return FALSE;
	return self->registry == NULL || axis_module_registry_is_enabled(self->registry, id);
}

void
axis_requests_set_clock(
	AxisRequests *self,
	gint64        now_us
){
	self->clock_us = now_us;
}

void
axis_requests_set_zone(
	AxisRequests *self,
	GTimeZone    *zone
){
	g_clear_pointer(&self->zone, g_time_zone_unref);
	self->zone = zone != NULL ? g_time_zone_ref(zone) : NULL;
}

gint64
axis_requests_now(AxisRequests *self)
{
	return self->clock_us != 0 ? self->clock_us : g_get_real_time();
}

void
axis_requests_today(
	AxisRequests *self,
	gchar         today[11]
){
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_utc(axis_requests_now(self) / G_USEC_PER_SEC);

	if (now == NULL) {
		g_strlcpy(today, "1970-01-01", 11);
		return;
	}
	g_snprintf(today, 11, "%04d-%02d-%02d", g_date_time_get_year(now), g_date_time_get_month(now),
		g_date_time_get_day_of_month(now));
}

void
axis_requests_stop(AxisRequests *self)
{
	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_requests_register(user_data, web);
}

/* Media: a page of its own beside the media types. */
static const AxisNavEntry nav_entries[] = {
	{ "Requests", "/requests", 90, AXIS_NAV_GROUP_MEDIA, FALSE, NULL, 0 },
	/* System: the tokens of the request portal, and its listener. */
	{ "Request portal", "/admin/portal", 22, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends[] = { AXIS_META_MODULE_ID, NULL };

const AxisModuleInfo *
axis_requests_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		axis_setting_set_choice_labels(AXIS_REQUESTS_SETTING_WINDOW, window_labels);
		info.id = AXIS_REQUESTS_MODULE_ID;
		info.name = "Requests";
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "People who are not administrators ask for a movie or for seasons of a series, and an "
			"administrator approves or declines on the Requests page. An approved request adds the title with "
			"the folder and the quality profile the administrator set, and starts its search. Each person has a "
			"limit for each kind over a day, a week or a month.";
		info.settings = settings_table;
		info.depends_on = depends;
	}
	return &info;
}
