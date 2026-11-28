/*
 * axis-requests-decide.c - deciding a request: approving and placing its
 * title, declining, withdrawing, closing a title, and a person's switches
 * and limits
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A decision is one transaction under the title's lock with the request's
 * row locked: it checks that the request still waits, so of two decisions at
 * the same moment (two administrators, or an approval and a withdrawal)
 * exactly one takes effect and the other is told the request was decided.
 *
 * Placing the title comes after the commit: it adds the title or finds it,
 * monitors the request's seasons (and changes no other), and queues the
 * search.  Two placings of one title wait for each other (a lock of the
 * placing's own, held on one connection for the whole placing); the movies and
 * the TV module work on connections of their own.  It can be run again
 * (axis_requests_try_again()): every step finds what an earlier attempt did.
 * A placing never removes what it added: a series whose season could not be
 * monitored stays, with the seasons monitored so far, the request says why,
 * and "Try again" completes it.
 */

#include "requests/axis-requests-private.h"

#include <string.h>

static gboolean
refuse(
	GError            **error,
	AxisRequestsError   code,
	const gchar        *message
){
	g_set_error_literal(error, AXIS_REQUESTS_ERROR, code, message);
	return FALSE;
}

static gboolean
no_such_request(GError **error)
{
	return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such request");
}

typedef struct
{
	gint64          id;
	gint64          title_id;
	AxisRequestKind kind;
} Ref;

/* The request a public identifier names (a random value of 22 characters:
 * anything else names none). */
static gboolean
find_ref(
	AxisDbConn   *conn,
	const gchar  *public_id,
	Ref          *ref,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { public_id };

	memset(ref, 0, sizeof(*ref));
	if (public_id == NULL || strlen(public_id) != 22 || !g_utf8_validate(public_id, -1, NULL))
		return no_such_request(error);
	if (!axis_db_conn_exec(conn, "SELECT id, title_id, kind FROM request WHERE public_id = $1", 1, params, &result,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return no_such_request(error);
	axis_db_result_get_int64(result, 0, 0, &ref->id);
	axis_db_result_get_int64(result, 0, 1, &ref->title_id);
	axis_request_kind_from_name(axis_db_result_get_text(result, 0, 2), &ref->kind);
	return TRUE;
}

/* The request's row locked, inside a transaction with its title locked:
 * its decision, who asked, and whether its title was added and is still
 * there (@placed, optional). */
static gboolean
lock_request(
	AxisDbConn  *conn,
	const Ref   *ref,
	gchar      **decision,
	gint64      *requested_by,
	gboolean    *placed,
	GError     **error
){
	g_autofree gchar *id = axis_requests_int_text(ref->id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { id };

	if (!axis_db_conn_exec(conn, "SELECT decision, coalesce(requested_by, 0),"
	        " added_at IS NOT NULL AND coalesce(movie_id, series_id) IS NOT NULL FROM request WHERE id = $1::bigint"
	        " FOR UPDATE", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return no_such_request(error);
	*decision = g_strdup(axis_db_result_get_text(result, 0, 0));
	if (requested_by != NULL)
		axis_db_result_get_int64(result, 0, 1, requested_by);
	if (placed != NULL)
		axis_db_result_get_boolean(result, 0, 2, placed);
	return TRUE;
}

/* Runs @body in a transaction on a connection of the pool. */
typedef gboolean (*Body)(AxisRequests *self, AxisDbConn *conn, gpointer data, GError **error);

static gboolean
in_transaction(
	AxisRequests  *self,
	Body           body,
	gpointer       data,
	GError       **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && body(self, conn, data, error);
	if (ok)
		ok = axis_db_conn_commit(conn, error);
	else
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── Placing ────────────────────────────────────────────────────── */

static gchar *
sentence(const gchar *message)
{
	g_autofree gchar *clean = NULL;
	glong length;

	if (message == NULL || message[0] == '\0')
		return g_strdup("It could not be added.");
	clean = g_utf8_make_valid(message, -1);
	length = g_utf8_strlen(clean, -1);
	if (length > 400) {
		gchar *end = g_utf8_offset_to_pointer(clean, 400);

		*end = '\0';
	}
	return g_strdup_printf("%c%s%s", g_ascii_toupper(clean[0]), clean + 1, g_str_has_suffix(clean, ".") ? "" : ".");
}

static gboolean
record_placed(
	AxisRequests  *self,
	gint64         request_id,
	AxisRequestKind kind,
	gint64         managed_id,
	const gchar   *problem,
	GError       **error
){
	g_autofree gchar *id = axis_requests_int_text(request_id);
	g_autofree gchar *managed = axis_requests_int_text(managed_id);
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	const gchar *params[] = { id, managed, now };
	const gchar *failed[] = { id, problem };

	if (problem != NULL)
		return axis_db_pool_exec(self->pool, "UPDATE request SET add_error = $2 WHERE id = $1::bigint"
			" AND decision = 'approved'", 2, failed, NULL, error);
	/* (The title's row may be gone again by now: then nothing is recorded,
	 * and the request reads as not added until it is tried again.) */
	return axis_db_pool_exec(self->pool, kind == AXIS_REQUEST_SERIES ?
		"UPDATE request SET series_id = (SELECT id FROM series WHERE id = $2::bigint), added_at = "
		AXIS_REQUESTS_TS(3) ", add_error = NULL WHERE id = $1::bigint AND decision = 'approved'"
		" AND EXISTS (SELECT 1 FROM series WHERE id = $2::bigint)" :
		"UPDATE request SET movie_id = (SELECT id FROM movie WHERE id = $2::bigint), added_at = "
		AXIS_REQUESTS_TS(3) ", add_error = NULL WHERE id = $1::bigint AND decision = 'approved'"
		" AND EXISTS (SELECT 1 FROM movie WHERE id = $2::bigint)", 3, params, NULL, error);
}

/* The managed movie or series of a title, 0 for none. */
static gboolean
find_managed(
	AxisRequests    *self,
	AxisRequestKind  kind,
	gint64           title_id,
	gint64          *managed_id,
	GError         **error
){
	g_autofree gchar *title = axis_requests_int_text(title_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { title };

	*managed_id = 0;
	if (!axis_db_pool_exec(self->pool, kind == AXIS_REQUEST_SERIES ? "SELECT id FROM series WHERE title_id = $1::bigint" :
	        "SELECT id FROM movie WHERE title_id = $1::bigint", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		axis_db_result_get_int64(result, 0, 0, managed_id);
	return TRUE;
}

/* Whether @error is the database's (a failure of the attempt itself), not a
 * module's answer about the title. */
static gboolean
is_database_error(const GError *error)
{
	return error != NULL && error->domain == AXIS_DB_ERROR;
}

/* Adds or finds the movie.  *@problem: (transfer full) why it could not be
 * added. */
static gboolean
place_movie(
	AxisRequests  *self,
	gint64         title_id,
	gint64         root_id,
	gint64         profile_id,
	const gchar   *user,
	gint64        *movie_id,
	gchar        **problem,
	GError       **error
){
	g_autoptr(GError) local = NULL;
	AxisMovieSpec spec;

	if (!find_managed(self, AXIS_REQUEST_MOVIE, title_id, movie_id, error))
		return FALSE;
	if (*movie_id != 0)
		return TRUE;
	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_id;
	spec.root_id = root_id;
	spec.profile_id = profile_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	if (self->settings != NULL && axis_settings_lookup(self->settings, AXIS_MOVIES_SETTING_AVAILABILITY, NULL) != NULL) {
		g_autofree gchar *name = axis_settings_get_string(self->settings, AXIS_MOVIES_SETTING_AVAILABILITY);

		axis_movie_availability_from_name(name, &spec.availability);
	}
	spec.search_now = TRUE;
	spec.user = user;
	if (axis_movies_add(self->movies, &spec, movie_id, &local))
		return TRUE;
	/* Added by someone else in this moment: that one is it. */
	if (g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_EXISTS))
		return find_managed(self, AXIS_REQUEST_MOVIE, title_id, movie_id, error);
	if (is_database_error(local)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	*problem = sentence(local->message);
	return TRUE;
}

static gboolean
place_series(
	AxisRequests  *self,
	gint64         title_id,
	gint64         root_id,
	gint64         profile_id,
	const GArray  *seasons,
	const gchar   *user,
	gint64        *series_id,
	gchar        **problem,
	GError       **error
){
	g_autoptr(GError) local = NULL;
	guint i;

	if (!find_managed(self, AXIS_REQUEST_SERIES, title_id, series_id, error))
		return FALSE;
	if (*series_id == 0) {
		AxisTvSpec spec;

		/* With nothing monitored: the request's seasons are monitored next,
		 * and no other. */
		memset(&spec, 0, sizeof(spec));
		spec.title_id = title_id;
		spec.root_id = root_id;
		spec.profile_id = profile_id;
		spec.monitor = AXIS_TV_MONITOR_NONE;
		spec.user = user;
		if (axis_tv_add(self->tv, &spec, series_id, &local)) {
			/* Added: its seasons next. */
		} else if (g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_EXISTS)) {
			g_clear_error(&local);
			if (!find_managed(self, AXIS_REQUEST_SERIES, title_id, series_id, error))
				return FALSE;
		} else if (is_database_error(local)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		} else {
			*problem = sentence(local->message);
			return TRUE;
		}
	}
	if (*series_id == 0) {
		*problem = g_strdup("The series could not be added.");
		return TRUE;
	}
	/* Approval widens what is monitored and never narrows it: a season is
	 * only ever switched on here, and the series' folder and profile are its
	 * own.  What fails part-way is left as it is, never removed: the series
	 * may hold another request's seasons, or an administrator's, by now, and
	 * "Try again" completes what is left. */
	for (i = 0; i < seasons->len; i++) {
		if (axis_tv_set_season_monitored(self->tv, *series_id, g_array_index(seasons, gint, i), TRUE, user, &local))
			continue;
		if (is_database_error(local)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		*problem = sentence(local->message);
		*series_id = 0;
		return TRUE;
	}
	for (i = 0; i < seasons->len; i++) {
		g_autoptr(GError) search = NULL;

		/* The scheduler takes what a search not queued here leaves. */
		if (!axis_tv_request_search(self->tv, *series_id, AXIS_TV_SCOPE_SEASON, g_array_index(seasons, gint, i), "added",
		        user, NULL, &search))
			g_debug("requests: a season's search was not queued: %s", search->message);
	}
	return TRUE;
}

/* The placing itself, with the placing's lock of the title held. */
static gboolean
place_locked(
	AxisRequests  *self,
	gint64         request_id,
	gboolean      *added,
	GError       **error
){
	g_autofree gchar *id = axis_requests_int_text(request_id);
	g_autoptr(GArray) seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *user = NULL;
	g_autofree gchar *problem = NULL;
	const gchar *params[] = { id };
	AxisRequestKind kind = AXIS_REQUEST_MOVIE;
	gint64 title_id = 0;
	gint64 root_id = 0;
	gint64 profile_id = 0;
	gint64 managed_id = 0;
	gboolean placed;

	if (!axis_db_pool_exec(self->pool, "SELECT r.title_id, r.kind, coalesce(r.root_id, 0),"
	        " coalesce(r.quality_profile_id, 0), coalesce(r.decided_by, r.requested_name),"
	        " coalesce((SELECT string_agg(s.season::text, ',' ORDER BY s.season) FROM request_season s"
	        "  WHERE s.request_id = r.id), '') FROM request r WHERE r.id = $1::bigint AND r.decision = 'approved'", 1,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	axis_db_result_get_int64(result, 0, 0, &title_id);
	axis_request_kind_from_name(axis_db_result_get_text(result, 0, 1), &kind);
	axis_db_result_get_int64(result, 0, 2, &root_id);
	axis_db_result_get_int64(result, 0, 3, &profile_id);
	user = g_strdup(axis_db_result_get_text(result, 0, 4));
	axis_requests_parse_list(axis_db_result_get_text(result, 0, 5), FALSE, seasons);
	g_clear_pointer(&result, axis_db_result_free);

	if (!axis_requests_kind_on(self, kind)) {
		problem = g_strdup(kind == AXIS_REQUEST_SERIES ? "The TV module is switched off." :
			"The Movies module is switched off.");
	} else {
		/* A title Axis has already needs no folder: it keeps its own. */
		if (!find_managed(self, kind, title_id, &managed_id, error))
			return FALSE;
		if (managed_id == 0 && root_id == 0) {
			problem = g_strdup("The library folder chosen for it no longer exists.");
		} else if (managed_id == 0 && profile_id == 0) {
			problem = g_strdup("The quality profile chosen for it no longer exists.");
		} else {
			placed = kind == AXIS_REQUEST_SERIES ?
				place_series(self, title_id, root_id, profile_id, seasons, user, &managed_id, &problem, error) :
				place_movie(self, title_id, root_id, profile_id, user, &managed_id, &problem, error);
			if (!placed) {
				/* The database failed in the middle: said at once, for "Try
				 * again", when it can still be written. */
				record_placed(self, request_id, kind, 0, "The database failed while it was being added.", NULL);
				return FALSE;
			}
		}
	}
	if (problem == NULL && managed_id == 0)
		problem = g_strdup("It could not be added.");
	if (!record_placed(self, request_id, kind, managed_id, problem, error))
		return FALSE;
	*added = problem == NULL;
	return TRUE;
}

gboolean
axis_requests_place(
	AxisRequests  *self,
	gint64         request_id,
	gboolean      *added,
	GError       **error
){
	g_autofree gchar *id = axis_requests_int_text(request_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { id };
	AxisDbConn *lock;
	gboolean dummy;
	gboolean ok;
	gint64 title_id = 0;

	if (added == NULL)
		added = &dummy;
	*added = FALSE;
	if (!axis_db_pool_exec(self->pool, "SELECT title_id FROM request WHERE id = $1::bigint AND decision = 'approved'", 1,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	axis_db_result_get_int64(result, 0, 0, &title_id);
	/* Held until the placing is over, whichever way it ends; the request is
	 * read again under it. */
	lock = axis_requests_lock_placing(self, title_id, error);
	if (lock == NULL)
		return FALSE;
	ok = place_locked(self, request_id, added, error);
	axis_requests_unlock_placing(self, lock, title_id);
	return ok;
}

/* ── Approving ──────────────────────────────────────────────────── */

/* A folder given for an approval must be one of the request's kind.  One that
 * does not exist (any more) passes: it is recorded as none, and the request
 * then says that its folder is gone. */
static gboolean
folder_of_kind(
	AxisDbConn       *conn,
	gint64            root_id,
	AxisRequestKind   kind,
	GError          **error
){
	g_autofree gchar *given = axis_requests_int_text(root_id);
	g_autoptr(AxisDbResult) found = NULL;
	const gchar *params[] = { given };

	if (root_id <= 0)
		return TRUE;
	if (!axis_db_conn_exec(conn, "SELECT media_type FROM library_root WHERE id = $1::bigint", 1, params, &found, error))
		return FALSE;
	if (axis_db_result_get_n_rows(found) > 0 &&
	    g_strcmp0(axis_db_result_get_text(found, 0, 0), kind == AXIS_REQUEST_SERIES ? "tv" : "movies") != 0)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, kind == AXIS_REQUEST_SERIES ?
			"that library folder is not one for series" : "that library folder is not one for movies");
	return TRUE;
}

typedef struct
{
	const gchar *public_id;
	const gchar *admin;
	gint64       root_id;
	gint64       profile_id;
	gboolean     again;
	Ref          ref;
} Approval;

static gboolean
approve_body(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gpointer       data,
	GError       **error
){
	Approval *approval = data;
	g_autofree gchar *decision = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *profile = NULL;
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	const gchar *media = NULL;
	AxisRequestDefaults defaults;
	gint64 root_id = approval->root_id;
	gint64 profile_id = approval->profile_id;
	gboolean placed = FALSE;

	if (!find_ref(conn, approval->public_id, &approval->ref, error) ||
	    !axis_requests_lock_title(conn, approval->ref.title_id, error) ||
	    !lock_request(conn, &approval->ref, &decision, NULL, &placed, error))
		return FALSE;
	id = axis_requests_int_text(approval->ref.id);
	media = approval->ref.kind == AXIS_REQUEST_SERIES ? "tv" : "movies";
	if (approval->again) {
		const gchar *params[4];

		if (g_strcmp0(decision, "approved") != 0)
			return refuse(error, AXIS_REQUESTS_ERROR_DECIDED, "the request is not an approved one");
		/* Only a title that was never added, or is gone: what an
		 * administrator changed since was changed on purpose. */
		if (placed)
			return refuse(error, AXIS_REQUESTS_ERROR_DECIDED, "its title was added and is still in Axis");
		if (!folder_of_kind(conn, root_id, approval->ref.kind, error) ||
		    !axis_requests_defaults_read(self, conn, &defaults, error))
			return FALSE;
		/* Another folder or profile when given; what the approval had
		 * otherwise, and the defaults where that is gone. */
		params[0] = id;
		params[1] = root = axis_requests_int_text(root_id);
		params[2] = profile = axis_requests_int_text(profile_id);
		params[3] = media;
		if (!axis_db_conn_exec(conn, "UPDATE request SET"
		        " root_id = coalesce((SELECT id FROM library_root WHERE id = $2::bigint AND media_type = $4), root_id),"
		        " quality_profile_id = coalesce((SELECT id FROM quality_profile WHERE id = $3::bigint),"
		        "  quality_profile_id) WHERE id = $1::bigint", 4, params, NULL, error))
			return FALSE;
		g_clear_pointer(&root, g_free);
		g_clear_pointer(&profile, g_free);
		params[1] = root = axis_requests_int_text(approval->ref.kind == AXIS_REQUEST_SERIES ? defaults.series_root :
			defaults.movies_root);
		params[2] = profile = axis_requests_int_text(approval->ref.kind == AXIS_REQUEST_SERIES ? defaults.series_profile :
			defaults.movies_profile);
		return axis_db_conn_exec(conn, "UPDATE request SET"
			" root_id = coalesce(root_id, (SELECT id FROM library_root WHERE id = $2::bigint AND media_type = $4)),"
			" quality_profile_id = coalesce(quality_profile_id, (SELECT id FROM quality_profile WHERE id = $3::bigint))"
			" WHERE id = $1::bigint", 4, params, NULL, error);
	}
	if (g_strcmp0(decision, "waiting") != 0)
		return refuse(error, AXIS_REQUESTS_ERROR_DECIDED, "the request was already decided");
	if (!folder_of_kind(conn, root_id, approval->ref.kind, error))
		return FALSE;
	if (root_id == 0 || profile_id == 0) {
		if (!axis_requests_defaults_read(self, conn, &defaults, error))
			return FALSE;
		if (root_id == 0)
			root_id = approval->ref.kind == AXIS_REQUEST_SERIES ? defaults.series_root : defaults.movies_root;
		if (profile_id == 0)
			profile_id = approval->ref.kind == AXIS_REQUEST_SERIES ? defaults.series_profile : defaults.movies_profile;
	}
	/* Nothing is approved until both are chosen. */
	if (root_id == 0 || profile_id == 0)
		return refuse(error, AXIS_REQUESTS_ERROR_PLACEMENT, "choose a library folder and a quality profile for it");
	{
		const gchar *params[] = {
			id, approval->admin, now, root = axis_requests_int_text(root_id),
			profile = axis_requests_int_text(profile_id), media
		};

		/* A folder or a profile that is gone at this moment is recorded as
		 * none: the title is then not added, and the page says why.  (A
		 * default is checked for its kind here, as a folder given above.) */
		return axis_db_conn_exec(conn, "UPDATE request SET decision = 'approved', approved_how = 'admin',"
			" decided_by = $2, decided_at = " AXIS_REQUESTS_TS(3) ", wait_reason = NULL,"
			" root_id = (SELECT id FROM library_root WHERE id = $4::bigint AND media_type = $6),"
			" quality_profile_id = (SELECT id FROM quality_profile WHERE id = $5::bigint)"
			" WHERE id = $1::bigint", 6, params, NULL, error);
	}
}

static gboolean
approve(
	AxisRequests  *self,
	Approval      *approval,
	gboolean      *added,
	GError       **error
){
	if (approval->admin == NULL || approval->admin[0] == '\0')
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "an approval is an administrator's");
	if (approval->root_id < 0 || approval->profile_id < 0)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "there is no such folder or profile");
	if (!in_transaction(self, approve_body, approval, error))
		return FALSE;
	return axis_requests_place(self, approval->ref.id, added, error);
}

gboolean
axis_requests_approve(
	AxisRequests  *self,
	const gchar   *public_id,
	const gchar   *admin,
	gint64         root_id,
	gint64         profile_id,
	gboolean      *added,
	GError       **error
){
	Approval approval = { public_id, admin, root_id, profile_id, FALSE, { 0, 0, AXIS_REQUEST_MOVIE } };

	return approve(self, &approval, added, error);
}

gboolean
axis_requests_try_again(
	AxisRequests  *self,
	const gchar   *public_id,
	const gchar   *admin,
	gint64         root_id,
	gint64         profile_id,
	gboolean      *added,
	GError       **error
){
	Approval approval = { public_id, admin, root_id, profile_id, TRUE, { 0, 0, AXIS_REQUEST_MOVIE } };

	return approve(self, &approval, added, error);
}

/* ── Declining and closing ──────────────────────────────────────── */

typedef struct
{
	const gchar *public_id;
	const gchar *admin;
	const gchar *reason;
	gboolean     close_title;
} Decline;

static gboolean
close_on(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gint64         title_id,
	const gchar   *admin,
	gboolean      *known,
	GError       **error
){
	g_autofree gchar *title = axis_requests_int_text(title_id);
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { title, admin, now };

	if (!axis_db_conn_exec(conn, "WITH t AS (SELECT id, CASE kind WHEN 'series' THEN 'series' ELSE 'movie' END AS kind"
	        " FROM meta_title WHERE id = $1::bigint AND kind IN ('film', 'series')),"
	        " i AS (INSERT INTO request_closed_title (title_id, kind, closed_by, closed_at)"
	        "  SELECT t.id, t.kind, $2, " AXIS_REQUESTS_TS(3) " FROM t ON CONFLICT (title_id) DO NOTHING)"
	        " SELECT count(*) FROM t", 3, params, &result, error))
		return FALSE;
	if (known != NULL) {
		gint64 count = 0;

		axis_db_result_get_int64(result, 0, 0, &count);
		*known = count > 0;
	}
	return TRUE;
}

static gboolean
decline_body(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gpointer       data,
	GError       **error
){
	Decline *decline = data;
	g_autofree gchar *decision = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	const gchar *params[4];
	Ref ref;

	if (!find_ref(conn, decline->public_id, &ref, error) || !axis_requests_lock_title(conn, ref.title_id, error) ||
	    !lock_request(conn, &ref, &decision, NULL, NULL, error))
		return FALSE;
	if (g_strcmp0(decision, "waiting") != 0)
		return refuse(error, AXIS_REQUESTS_ERROR_DECIDED, "the request was already decided");
	params[0] = id = axis_requests_int_text(ref.id);
	params[1] = decline->admin;
	params[2] = now;
	params[3] = decline->reason;
	if (!axis_db_conn_exec(conn, "UPDATE request SET decision = 'declined', decided_by = $2, decided_at = "
	        AXIS_REQUESTS_TS(3) ", decline_reason = $4, wait_reason = NULL WHERE id = $1::bigint", 4, params, NULL, error))
		return FALSE;
	return !decline->close_title || close_on(self, conn, ref.title_id, decline->admin, NULL, error);
}

gboolean
axis_requests_decline(
	AxisRequests  *self,
	const gchar   *public_id,
	const gchar   *admin,
	const gchar   *reason,
	gboolean       close_title,
	GError       **error
){
	g_autofree gchar *clean = reason != NULL && g_utf8_validate(reason, -1, NULL) ? g_strstrip(g_strdup(reason)) : NULL;
	Decline decline = { public_id, admin, clean, close_title };
	const gchar *p;

	if (admin == NULL || admin[0] == '\0')
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "a decline is an administrator's");
	if (clean == NULL || clean[0] == '\0')
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "a decline needs a reason");
	if (g_utf8_strlen(clean, -1) > AXIS_REQUESTS_MAX_REASON)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the reason is longer than 300 characters");
	for (p = clean; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the reason must be one line of plain text");
	}
	return in_transaction(self, decline_body, &decline, error);
}

gboolean
axis_requests_set_title_closed(
	AxisRequests  *self,
	gint64         title_id,
	gboolean       closed,
	const gchar   *admin,
	GError       **error
){
	g_autofree gchar *title = axis_requests_int_text(title_id);
	const gchar *params[] = { title };
	AxisDbConn *conn;
	gboolean known = FALSE;
	gboolean ok;

	if (!closed)
		return axis_db_pool_exec(self->pool, "DELETE FROM request_closed_title WHERE title_id = $1::bigint", 1, params,
			NULL, error);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = close_on(self, conn, title_id, admin, &known, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && !known)
		return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such title");
	return ok;
}

/* ── Withdrawing ────────────────────────────────────────────────── */

gboolean
axis_requests_withdraw_locked(
	AxisRequests    *self,
	AxisDbConn      *conn,
	gint64           request_id,
	AxisRequestKind  kind,
	gboolean        *handed_over,
	GError         **error
){
	g_autofree gchar *id = axis_requests_int_text(request_id);
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	g_autoptr(AxisDbResult) follower = NULL;
	const gchar *params[] = { id };

	if (handed_over != NULL)
		*handed_over = FALSE;
	/* The earliest follower, if they may still log in. */
	if (!axis_db_conn_exec(conn, "SELECT f.user_id, u.username, u.disabled,"
	        " (SELECT count(*) FROM request_season s WHERE s.request_id = f.request_id)"
	        " FROM request_follower f JOIN app_user u ON u.id = f.user_id WHERE f.request_id = $1::bigint"
	        " ORDER BY f.followed_at, f.user_id LIMIT 1", 1, params, &follower, error))
		return FALSE;
	if (axis_db_result_get_n_rows(follower) > 0) {
		const AxisRequestAllowance *allowance;
		AxisRequestQuota quota;
		gint64 follower_id = 0;
		gint64 n_seasons = 0;
		gboolean disabled = TRUE;
		gboolean found = FALSE;
		guint need;

		axis_db_result_get_int64(follower, 0, 0, &follower_id);
		axis_db_result_get_boolean(follower, 0, 2, &disabled);
		axis_db_result_get_int64(follower, 0, 3, &n_seasons);
		need = kind == AXIS_REQUEST_SERIES ? (guint)n_seasons : 1;
		/* Theirs only if they have it left: counted with the person locked,
		 * as a request of their own would be. */
		if (!disabled) {
			if (!axis_requests_lock_person(conn, follower_id, error) ||
			    !axis_requests_quota_on(self, conn, follower_id, &quota, &found, error))
				return FALSE;
			allowance = kind == AXIS_REQUEST_SERIES ? &quota.seasons : &quota.movies;
			if (found && (allowance->unlimited || need <= allowance->left)) {
				g_autofree gchar *user = axis_requests_int_text(follower_id);
				const gchar *take[] = { id, user, axis_db_result_get_text(follower, 0, 1), now };
				const gchar *drop[] = { id, user };

				if (!axis_db_conn_exec(conn, "UPDATE request SET requested_by = $2::bigint, requested_name = $3,"
				        " counted_at = " AXIS_REQUESTS_TS(4) " WHERE id = $1::bigint", 4, take, NULL, error) ||
				    !axis_db_conn_exec(conn, "DELETE FROM request_follower WHERE request_id = $1::bigint"
				        " AND user_id = $2::bigint", 2, drop, NULL, error))
					return FALSE;
				if (handed_over != NULL)
					*handed_over = TRUE;
				return TRUE;
			}
		}
	}
	{
		const gchar *withdraw[] = { id, now };

		return axis_db_conn_exec(conn, "UPDATE request SET decision = 'withdrawn', decided_at = " AXIS_REQUESTS_TS(2)
			", wait_reason = NULL WHERE id = $1::bigint", 2, withdraw, NULL, error);
	}
}

typedef struct
{
	const gchar *public_id;
	gint64       user_id;
	gboolean     handed_over;
} Withdrawal;

static gboolean
withdraw_body(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gpointer       data,
	GError       **error
){
	Withdrawal *withdrawal = data;
	g_autofree gchar *decision = NULL;
	gint64 by = 0;
	Ref ref;

	if (!find_ref(conn, withdrawal->public_id, &ref, error) || !axis_requests_lock_title(conn, ref.title_id, error) ||
	    !lock_request(conn, &ref, &decision, &by, NULL, error))
		return FALSE;
	/* Another person's request: the answer of one that does not exist. */
	if (by != withdrawal->user_id || withdrawal->user_id == 0)
		return no_such_request(error);
	if (g_strcmp0(decision, "waiting") != 0)
		return refuse(error, AXIS_REQUESTS_ERROR_DECIDED, "the request is no longer waiting");
	return axis_requests_withdraw_locked(self, conn, ref.id, ref.kind, &withdrawal->handed_over, error);
}

gboolean
axis_requests_withdraw(
	AxisRequests  *self,
	const gchar   *public_id,
	gint64         user_id,
	gboolean      *handed_over,
	GError       **error
){
	Withdrawal withdrawal = { public_id, user_id, FALSE };

	if (!in_transaction(self, withdraw_body, &withdrawal, error))
		return FALSE;
	if (handed_over != NULL)
		*handed_over = withdrawal.handed_over;
	return TRUE;
}

gboolean
axis_requests_unfollow(
	AxisRequests  *self,
	const gchar   *public_id,
	gint64         user_id,
	GError       **error
){
	g_autofree gchar *user = axis_requests_int_text(user_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { public_id, user };

	if (public_id == NULL || strlen(public_id) != 22 || !g_utf8_validate(public_id, -1, NULL))
		return no_such_request(error);
	if (!axis_db_pool_exec(self->pool, "DELETE FROM request_follower f USING request r WHERE r.id = f.request_id"
	        " AND r.public_id = $1 AND f.user_id = $2::bigint", 2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0)
		return no_such_request(error);
	return TRUE;
}

typedef struct
{
	Ref    ref;
	gint64 user_id;
} Gone;

static gboolean
gone_body(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gpointer       data,
	GError       **error
){
	Gone *gone = data;
	g_autofree gchar *decision = NULL;
	gint64 by = 0;

	if (!axis_requests_lock_title(conn, gone->ref.title_id, error) ||
	    !lock_request(conn, &gone->ref, &decision, &by, NULL, error))
		return FALSE;
	/* Decided, or taken over, since it was listed: nothing to do.  (An
	 * account that was deleted left its requests with no one.) */
	if ((by != gone->user_id && by != 0) || g_strcmp0(decision, "waiting") != 0)
		return TRUE;
	return axis_requests_withdraw_locked(self, conn, gone->ref.id, gone->ref.kind, NULL, error);
}

gboolean
axis_requests_account_gone(
	AxisRequests  *self,
	gint64         user_id,
	GError       **error
){
	g_autofree gchar *user = axis_requests_int_text(user_id);
	g_autoptr(AxisDbResult) waiting = NULL;
	const gchar *params[] = { user };
	guint i;

	/* Its own, and those of any account deleted before (the row then names
	 * no one): a deleted account's waiting requests are found here whichever
	 * call comes after the deletion. */
	if (!axis_db_pool_exec(self->pool, "SELECT id, title_id, kind FROM request"
	        " WHERE (requested_by = $1::bigint OR requested_by IS NULL) AND decision = 'waiting' ORDER BY id", 1, params,
	        &waiting, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(waiting); i++) {
		Gone gone;

		memset(&gone, 0, sizeof(gone));
		gone.user_id = user_id;
		axis_db_result_get_int64(waiting, i, 0, &gone.ref.id);
		axis_db_result_get_int64(waiting, i, 1, &gone.ref.title_id);
		axis_request_kind_from_name(axis_db_result_get_text(waiting, i, 2), &gone.ref.kind);
		if (!in_transaction(self, gone_body, &gone, error))
			return FALSE;
	}
	/* Its part in what it follows ends. */
	return axis_db_pool_exec(self->pool, "DELETE FROM request_follower WHERE user_id = $1::bigint", 1, params, NULL,
		error);
}

/* ── A person's switches and limits, and the defaults ───────────── */

gboolean
axis_requests_set_person(
	AxisRequests            *self,
	const AxisRequestPerson *person,
	GError                 **error
){
	g_autofree gchar *id = axis_requests_int_text(person->id);
	g_autofree gchar *movies = person->movies_limit >= 0 ? axis_requests_int_text(person->movies_limit) : NULL;
	g_autofree gchar *seasons = person->seasons_limit >= 0 ? axis_requests_int_text(person->seasons_limit) : NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = {
		id, person->auto_movies ? "t" : "f", person->auto_series ? "t" : "f", movies, seasons
	};

	if (person->movies_limit > AXIS_REQUESTS_MAX_LIMIT || person->seasons_limit > AXIS_REQUESTS_MAX_LIMIT ||
	    person->movies_limit < -1 || person->seasons_limit < -1)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "a limit is a number from 0 to 1000");
	if (!axis_db_pool_exec(self->pool, "UPDATE app_user SET request_auto_movies = $2::boolean,"
	        " request_auto_series = $3::boolean, request_movies_limit = $4::integer,"
	        " request_seasons_limit = $5::integer, updated_at = now() WHERE id = $1::bigint", 5, params, &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0)
		return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such account");
	return TRUE;
}

gboolean
axis_requests_set_new_account_switches(
	AxisRequests  *self,
	gboolean       movies,
	gboolean       series,
	GError       **error
){
	if (self->settings == NULL ||
	    axis_settings_lookup(self->settings, AXIS_REQUESTS_SETTING_NEW_AUTO_MOVIES, NULL) == NULL)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the settings are not available");
	return axis_settings_set(self->settings, AXIS_REQUESTS_SETTING_NEW_AUTO_MOVIES, movies ? "true" : "false", error) &&
		axis_settings_set(self->settings, AXIS_REQUESTS_SETTING_NEW_AUTO_SERIES, series ? "true" : "false", error);
}

gboolean
axis_requests_set_defaults(
	AxisRequests               *self,
	const AxisRequestDefaults  *defaults,
	GError                    **error
){
	static const gchar *const keys[] = {
		AXIS_REQUESTS_SETTING_MOVIES_ROOT, AXIS_REQUESTS_SETTING_MOVIES_PROFILE, AXIS_REQUESTS_SETTING_SERIES_ROOT,
		AXIS_REQUESTS_SETTING_SERIES_PROFILE
	};
	const gint64 values[] = {
		defaults->movies_root, defaults->movies_profile, defaults->series_root, defaults->series_profile
	};
	g_autofree gchar *movies_root = axis_requests_int_text(defaults->movies_root);
	g_autofree gchar *movies_profile = axis_requests_int_text(defaults->movies_profile);
	g_autofree gchar *series_root = axis_requests_int_text(defaults->series_root);
	g_autofree gchar *series_profile = axis_requests_int_text(defaults->series_profile);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { movies_root, movies_profile, series_root, series_profile };
	gboolean ok = FALSE;
	guint i;

	if (self->settings == NULL || axis_settings_lookup(self->settings, keys[0], NULL) == NULL)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the settings are not available");
	for (i = 0; i < G_N_ELEMENTS(values); i++) {
		if (values[i] < 0)
			return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "there is no such folder or profile");
	}
	/* Each one given must be a folder of its kind, or a profile, that exists. */
	if (!axis_db_pool_exec(self->pool, "SELECT"
	        " ($1::bigint = 0 OR EXISTS (SELECT 1 FROM library_root WHERE id = $1::bigint AND media_type = 'movies'))"
	        " AND ($2::bigint = 0 OR EXISTS (SELECT 1 FROM quality_profile WHERE id = $2::bigint))"
	        " AND ($3::bigint = 0 OR EXISTS (SELECT 1 FROM library_root WHERE id = $3::bigint AND media_type = 'tv'))"
	        " AND ($4::bigint = 0 OR EXISTS (SELECT 1 FROM quality_profile WHERE id = $4::bigint))", 4, params, &result,
	        error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, &ok);
	if (!ok)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "there is no such folder or profile");
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		if (!axis_settings_set(self->settings, keys[i], params[i], error))
			return FALSE;
	}
	return TRUE;
}
