/*
 * axis-requests-ask.c - a person asks for a title
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One transaction decides everything a request depends on, under the title's
 * lock and then the person's: whether the title is closed, which seasons the
 * title's record has, what is requested already (followed, never refused),
 * what is left of the person's quota and under the daily ceiling, and whether
 * the request is approved at once.  The counts and the insert they guard are
 * in that one transaction with the person locked, so two requests posted at
 * the same moment cannot both take the last place; the database's unique
 * indexes hold the rule of one open request besides.
 *
 * What the engine's records say of the title (in the library, managed
 * already) is read before the transaction, on connections of the modules'
 * own: no connection is held while another module is called.
 */

#include "requests/axis-requests-private.h"

#include <string.h>

#include "web/axis-password.h"

void
axis_request_made_clear(AxisRequestMade *made)
{
	g_clear_pointer(&made->public_id, g_free);
	g_clear_pointer(&made->approved_how, g_free);
	g_clear_pointer(&made->wait_reason, g_free);
	g_clear_pointer(&made->seasons, g_array_unref);
	g_clear_pointer(&made->left_available, g_array_unref);
	g_clear_pointer(&made->left_requested, g_array_unref);
}

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
has_int(
	const GArray *list,
	gint          value
){
	guint i;

	for (i = 0; i < list->len; i++) {
		if (g_array_index(list, gint, i) == value)
			return TRUE;
	}
	return FALSE;
}

/* "{1,2,3}" of gint values. */
static gchar *
int_array_text(const GArray *list)
{
	GString *text = g_string_new("{");
	guint i;

	for (i = 0; i < list->len; i++)
		g_string_append_printf(text, "%s%d", i > 0 ? "," : "", g_array_index(list, gint, i));
	g_string_append_c(text, '}');
	return g_string_free(text, FALSE);
}

/*
 * The person's choice as a list of season numbers, from the seasons the
 * title's record has now.  The specials are in it only when picked.
 */
static gboolean
expand_seasons(
	AxisDbConn            *conn,
	const AxisRequestAsk  *ask,
	GArray                *listed,
	GError               **error
){
	g_autofree gchar *title = axis_requests_int_text(ask->title_id);
	g_autoptr(GArray) record = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { title };
	gint first = -1;
	gint latest = -1;
	guint i;

	if (!axis_db_conn_exec(conn, "SELECT number FROM meta_season_followed WHERE title_id = $1::bigint ORDER BY number", 1,
	        params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 number = 0;
		gint season;

		axis_db_result_get_int64(result, i, 0, &number);
		season = (gint)number;
		g_array_append_val(record, season);
		if (season > 0 && first < 0)
			first = season;
		if (season > 0)
			latest = season;
	}
	switch (ask->seasons) {
	case AXIS_REQUEST_SEASONS_ALL:
		for (i = 0; i < record->len; i++) {
			if (g_array_index(record, gint, i) > 0)
				g_array_append_val(listed, g_array_index(record, gint, i));
		}
		break;
	case AXIS_REQUEST_SEASONS_FIRST:
		if (first > 0)
			g_array_append_val(listed, first);
		break;
	case AXIS_REQUEST_SEASONS_LATEST:
		if (latest > 0)
			g_array_append_val(listed, latest);
		break;
	default:
		for (i = 0; i < ask->n_picked; i++) {
			if (!has_int(record, ask->picked[i]))
				return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the title's record has no such season");
			g_array_append_val(listed, ask->picked[i]);
		}
		break;
	}
	if (listed->len == 0)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the title's record has no season to ask for yet");
	if (listed->len > AXIS_REQUESTS_MAX_SEASONS)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the request lists too many seasons");
	return TRUE;
}

static gboolean
follow(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gint64         request_id,
	gint64         user_id,
	GError       **error
){
	g_autofree gchar *request = axis_requests_int_text(request_id);
	g_autofree gchar *user = axis_requests_int_text(user_id);
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	const gchar *params[] = { request, user, now };

	return axis_db_conn_exec(conn, "INSERT INTO request_follower (request_id, user_id, followed_at)"
		" VALUES ($1::bigint, $2::bigint, " AXIS_REQUESTS_TS(3) ") ON CONFLICT DO NOTHING", 3, params, NULL, error);
}

/* What stands in the way of an approval at once, or %NULL (static). */
static const gchar *
auto_wait_reason(
	AxisRequests              *self,
	const AxisRequestDefaults *defaults,
	AxisRequestKind            kind,
	guint                      n_seasons,
	gint64                    *root_id,
	gint64                    *profile_id
){
	*root_id = kind == AXIS_REQUEST_SERIES ? defaults->series_root : defaults->movies_root;
	*profile_id = kind == AXIS_REQUEST_SERIES ? defaults->series_profile : defaults->movies_profile;
	if (*root_id == 0 || *profile_id == 0)
		return "no_defaults";
	if (!(kind == AXIS_REQUEST_SERIES ? defaults->series_ok : defaults->movies_ok))
		return "defaults_gone";
	if (kind == AXIS_REQUEST_SERIES &&
	    n_seasons > (guint)axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_AUTO_SEASONS, 3))
		return "too_many_seasons";
	return NULL;
}

/* The transaction; @made is filled as far as it got. */
static gboolean
ask_locked(
	AxisRequests             *self,
	AxisDbConn               *conn,
	const AxisRequestAsk     *ask,
	const AxisRequestsFacts  *facts,
	AxisRequestMade          *made,
	GError                  **error
){
	g_autofree gchar *title = axis_requests_int_text(ask->title_id);
	g_autofree gchar *user = axis_requests_int_text(ask->user_id);
	g_autofree gchar *now = axis_requests_int_text(axis_requests_now(self));
	g_autofree gchar *username = NULL;
	g_autoptr(GArray) listed = g_array_new(FALSE, FALSE, sizeof(gint));
	const AxisRequestsMovieFact *movie = g_hash_table_lookup(facts->movies, &ask->title_id);
	const AxisRequestsSeriesFact *series = g_hash_table_lookup(facts->series, &ask->title_id);
	const AxisRequestAllowance *allowance;
	const gchar *wait_reason = NULL;
	const gchar *approved_how = NULL;
	AxisRequestDefaults defaults;
	AxisRequestQuota quota;
	gboolean found = FALSE;
	gboolean auto_on = FALSE;
	gboolean adult = FALSE;
	gboolean admin = FALSE;
	gint64 root_id = 0;
	gint64 profile_id = 0;
	gint64 managed_id = 0;
	guint need;
	guint i;

	if (!axis_requests_lock_title(conn, ask->title_id, error))
		return FALSE;
	{
		g_autoptr(AxisDbResult) result = NULL;
		const gchar *params[] = { title, user };
		const gchar *kind;
		gboolean closed = FALSE;
		gboolean asker_admin = FALSE;

		/* A title its provider marks as adult, or whose mark is not known
		 * yet (stored before migration 0084, or from no provider's answer),
		 * is not one a person who is not an administrator can ask for,
		 * however it came to be stored and whatever the search's setting:
		 * the answer of a title that does not exist, before anything else
		 * of the title is told (its kind, whether it is closed), so that its
		 * number says nothing.  The account is read again, locked, below. */
		if (!axis_db_conn_exec(conn, "SELECT t.kind, EXISTS (SELECT 1 FROM request_closed_title c"
		        " WHERE c.title_id = t.id), t.adult IS NOT FALSE, coalesce((SELECT u.role = 'admin' FROM app_user u"
		        " WHERE u.id = $2::bigint), false) FROM meta_title t WHERE t.id = $1::bigint", 2, params, &result, error))
			return FALSE;
		if (axis_db_result_get_n_rows(result) == 0)
			return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such title");
		axis_db_result_get_boolean(result, 0, 2, &adult);
		axis_db_result_get_boolean(result, 0, 3, &asker_admin);
		if (adult && !asker_admin)
			return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such title");
		kind = axis_db_result_get_text(result, 0, 0);
		if (g_strcmp0(kind, ask->kind == AXIS_REQUEST_SERIES ? "series" : "film") != 0)
			return refuse(error, AXIS_REQUESTS_ERROR_INVALID, ask->kind == AXIS_REQUEST_SERIES ?
				"the title is not a series" : "the title is not a movie");
		axis_db_result_get_boolean(result, 0, 1, &closed);
		if (closed)
			return refuse(error, AXIS_REQUESTS_ERROR_CLOSED, "the title is closed to requests");
	}
	{
		g_autoptr(AxisDbResult) result = NULL;
		const gchar *params[] = { user };
		guint column = ask->kind == AXIS_REQUEST_SERIES ? 4 : 3;
		gint64 limit;

		/* FOR SHARE: a disable of the account waits until this request is
		 * made, so that what follows the disable (its waiting requests
		 * withdrawn) finds it. */
		if (!axis_db_conn_exec(conn, "SELECT username, request_auto_movies, request_auto_series, request_movies_limit,"
		        " request_seasons_limit, role = 'admin' FROM app_user WHERE id = $1::bigint AND NOT disabled FOR SHARE",
		        1, params, &result, error))
			return FALSE;
		if (axis_db_result_get_n_rows(result) == 0)
			return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such account");
		username = g_strdup(axis_db_result_get_text(result, 0, 0));
		axis_db_result_get_boolean(result, 0, ask->kind == AXIS_REQUEST_SERIES ? 2 : 1, &auto_on);
		axis_db_result_get_boolean(result, 0, 5, &admin);
		/* The same rule with the account locked: an account that stopped
		 * being an administrator a moment ago is not one now. */
		if (adult && !admin)
			return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such title");
		limit = axis_requests_setting_int(self, ask->kind == AXIS_REQUEST_SERIES ? AXIS_REQUESTS_SETTING_SEASONS :
			AXIS_REQUESTS_SETTING_MOVIES, 5);
		if (!axis_db_result_is_null(result, 0, column))
			axis_db_result_get_int64(result, 0, column, &limit);
		/* A limit of none: the person asks for nothing of the kind, and
		 * follows nothing either. */
		if (!admin && limit == 0) {
			g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA, "you cannot ask for %s",
				ask->kind == AXIS_REQUEST_SERIES ? "series" : "movies");
			return FALSE;
		}
	}
	/* What the engine's records said before the transaction is managed still
	 * is, and stays so until this commits (a removal waits): a title removed
	 * in between is refused as it is, not with a broken reference. */
	{
		gint64 known = ask->kind == AXIS_REQUEST_MOVIE ? (movie != NULL ? movie->movie_id : 0) :
			(series != NULL ? series->series_id : 0);

		if (known != 0) {
			g_autofree gchar *managed = axis_requests_int_text(known);
			g_autoptr(AxisDbResult) result = NULL;
			const gchar *params[] = { managed };

			if (!axis_db_conn_exec(conn, ask->kind == AXIS_REQUEST_MOVIE ?
			        "SELECT 1 FROM movie WHERE id = $1::bigint FOR KEY SHARE" :
			        "SELECT 1 FROM series WHERE id = $1::bigint FOR KEY SHARE", 1, params, &result, error))
				return FALSE;
			if (axis_db_result_get_n_rows(result) == 0)
				return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the title changed at this moment; ask again");
		}
	}

	if (ask->kind == AXIS_REQUEST_MOVIE) {
		g_autoptr(AxisDbResult) open = NULL;
		const gchar *params[] = { title, user };

		if (movie != NULL && movie->on_disk) {
			made->outcome = AXIS_REQUEST_ALREADY_AVAILABLE;
			return TRUE;
		}
		if (!axis_db_conn_exec(conn, "SELECT r.id, coalesce(r.requested_by, 0), EXISTS (SELECT 1 FROM request_follower f"
		        " WHERE f.request_id = r.id AND f.user_id = $2::bigint) FROM request r WHERE r.title_id = $1::bigint"
		        " AND r.kind = 'movie' AND r.decision IN ('waiting', 'approved') FOR UPDATE OF r", 2, params, &open, error))
			return FALSE;
		if (axis_db_result_get_n_rows(open) > 0) {
			gint64 request_id = 0;
			gint64 by = 0;
			gboolean follows = FALSE;

			axis_db_result_get_int64(open, 0, 0, &request_id);
			axis_db_result_get_int64(open, 0, 1, &by);
			axis_db_result_get_boolean(open, 0, 2, &follows);
			if (by == ask->user_id || follows) {
				made->outcome = AXIS_REQUEST_UNCHANGED;
				return TRUE;
			}
			if (!follow(self, conn, request_id, ask->user_id, error))
				return FALSE;
			made->outcome = AXIS_REQUEST_FOLLOWED;
			made->n_followed = 1;
			return TRUE;
		}
		if (movie != NULL && movie->movie_id != 0) {
			approved_how = "existing";
			managed_id = movie->movie_id;
		}
	} else {
		g_autoptr(GArray) wanted = g_array_new(FALSE, FALSE, sizeof(gint));
		g_autoptr(GArray) followed = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autoptr(AxisDbResult) open = NULL;
		g_autofree gchar *numbers = NULL;
		const gchar *params[3];
		gboolean all_monitored = series != NULL;
		guint own = 0;

		if (!expand_seasons(conn, ask, wanted, error))
			return FALSE;
		numbers = int_array_text(wanted);
		params[0] = title;
		params[1] = user;
		params[2] = numbers;
		/* The open requests that list any of them, their rows locked. */
		if (!axis_db_conn_exec(conn, "SELECT s.season, r.id, coalesce(r.requested_by, 0), EXISTS (SELECT 1"
		        " FROM request_follower f WHERE f.request_id = r.id AND f.user_id = $2::bigint)"
		        " FROM request_season s JOIN request r ON r.id = s.request_id"
		        " WHERE s.title_id = $1::bigint AND s.open AND s.season = ANY ($3::integer[]) FOR UPDATE OF r", 3, params,
		        &open, error))
			return FALSE;
		for (i = 0; i < wanted->len; i++) {
			gint season = g_array_index(wanted, gint, i);
			gboolean requested = FALSE;
			guint j;

			if (axis_requests_season_available(axis_requests_season_fact(series, season))) {
				g_array_append_val(made->left_available, season);
				continue;
			}
			for (j = 0; j < axis_db_result_get_n_rows(open); j++) {
				gint64 number = 0;
				gint64 request_id = 0;
				gint64 by = 0;
				gboolean follows = FALSE;
				gboolean known = FALSE;
				guint k;

				axis_db_result_get_int64(open, j, 0, &number);
				if ((gint)number != season)
					continue;
				axis_db_result_get_int64(open, j, 1, &request_id);
				axis_db_result_get_int64(open, j, 2, &by);
				axis_db_result_get_boolean(open, j, 3, &follows);
				requested = TRUE;
				if (by == ask->user_id || follows) {
					own++;
					continue;
				}
				for (k = 0; k < followed->len; k++)
					known = known || g_array_index(followed, gint64, k) == request_id;
				if (!known)
					g_array_append_val(followed, request_id);
			}
			if (requested)
				g_array_append_val(made->left_requested, season);
			else
				g_array_append_val(listed, season);
		}
		for (i = 0; i < followed->len; i++) {
			if (!follow(self, conn, g_array_index(followed, gint64, i), ask->user_id, error))
				return FALSE;
		}
		made->n_followed = followed->len;
		if (listed->len == 0) {
			made->outcome = followed->len > 0 ? AXIS_REQUEST_FOLLOWED : own > 0 ? AXIS_REQUEST_UNCHANGED :
				AXIS_REQUEST_ALREADY_AVAILABLE;
			return TRUE;
		}
		/* A series Axis has, with every season asked for monitored already:
		 * approving it would fetch nothing more. */
		for (i = 0; i < listed->len; i++) {
			const AxisRequestsSeasonFact *fact = axis_requests_season_fact(series, g_array_index(listed, gint, i));

			all_monitored = all_monitored && fact != NULL && fact->monitored;
		}
		if (all_monitored) {
			approved_how = "existing";
			managed_id = series->series_id;
		}
	}

	/* A new request: the person's quota, counted with the person locked. */
	need = ask->kind == AXIS_REQUEST_SERIES ? listed->len : 1;
	if (!axis_requests_lock_person(conn, ask->user_id, error) ||
	    !axis_requests_quota_on(self, conn, ask->user_id, &quota, &found, error))
		return FALSE;
	if (!found)
		return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such account");
	allowance = ask->kind == AXIS_REQUEST_SERIES ? &quota.seasons : &quota.movies;
	if (!allowance->unlimited) {
		if (allowance->limit == 0) {
			g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA, "you cannot ask for %s",
				ask->kind == AXIS_REQUEST_SERIES ? "series" : "movies");
			return FALSE;
		}
		if (need > allowance->left) {
			const gchar *until = axis_request_window_until(quota.window);

			if (ask->kind == AXIS_REQUEST_MOVIE)
				g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA,
					"no movie request is left until %s", until);
			else if (allowance->left == 0)
				g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA,
					"no season request is left until %s", until);
			else
				g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA,
					"the request lists %u seasons and %u %s left until %s", need, allowance->left,
					allowance->left == 1 ? "season is" : "seasons are", until);
			return FALSE;
		}
	}
	/* The ceiling a withdrawal does not give back: what the person made in
	 * the last 24 hours, whatever became of it. */
	if (!allowance->unlimited) {
		g_autofree gchar *since = axis_requests_int_text(axis_requests_now(self) - G_TIME_SPAN_DAY);
		g_autoptr(AxisDbResult) result = NULL;
		const gchar *params[] = { user, since };
		gint64 lately = 0;

		if (!axis_db_conn_exec(conn, "SELECT count(*) FROM request WHERE made_by = $1::bigint AND created_at > "
		        AXIS_REQUESTS_TS(2), 2, params, &result, error))
			return FALSE;
		axis_db_result_get_int64(result, 0, 0, &lately);
		if (lately >= AXIS_REQUESTS_MAX_A_DAY) {
			g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_TOO_MANY, "you made %d requests in the last 24 "
				"hours, withdrawn ones included, and that is the most anyone may; ask again later",
				AXIS_REQUESTS_MAX_A_DAY);
			return FALSE;
		}
	}

	/* Through the portal API: what all such requests count on this calendar
	 * day, whoever made them and whatever became of them (a movie one, a
	 * season one each), and what the person's own count of it, withdrawn
	 * ones included, so that no one person can use the day up for everyone
	 * by asking and withdrawing.  One lock for all of them, taken last, so
	 * that two people at the same moment cannot both take the last place. */
	if (ask->portal) {
		g_autoptr(GTimeZone) local = self->zone != NULL ? g_time_zone_ref(self->zone) : g_time_zone_new_local();
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *from = NULL;
		const gchar *params[2];
		gint64 ceiling = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_PORTAL_DAY, AXIS_REQUESTS_PORTAL_A_DAY);
		gint64 share = AXIS_REQUESTS_PORTAL_SHARE_OF(ceiling);
		gint64 start = 0;
		gint64 end = 0;
		gint64 counted = 0;
		gint64 own = 0;

		axis_request_window_bounds(local, axis_requests_now(self), AXIS_REQUEST_WINDOW_DAY, &start, &end);
		from = axis_requests_int_text(start);
		params[0] = from;
		params[1] = user;
		if (!axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(hashtext('axis.request.portal'), 0)", 0, NULL, NULL,
		        error) ||
		    !axis_db_conn_exec(conn, "SELECT coalesce(sum(n), 0), coalesce(sum(n) FILTER (WHERE made_by = $2::bigint), 0)"
		        " FROM (SELECT r.made_by, CASE WHEN r.kind = 'movie' THEN 1 ELSE (SELECT count(*) FROM request_season s"
		        " WHERE s.request_id = r.id) END AS n FROM request r WHERE r.via = 'portal' AND r.created_at >= "
		        AXIS_REQUESTS_TS(1) ") counted", 2, params, &result, error))
			return FALSE;
		axis_db_result_get_int64(result, 0, 0, &counted);
		axis_db_result_get_int64(result, 0, 1, &own);
		if (counted + need > ceiling) {
			g_set_error_literal(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PORTAL_FULL,
				"the portal's limit for today is reached; ask again tomorrow");
			return FALSE;
		}
		if (own + need > share) {
			g_set_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PORTAL_SHARE,
				"your requests through the portal today count %" G_GINT64_FORMAT ", withdrawn ones included,"
				" and one person's share of the portal's day is %" G_GINT64_FORMAT "; ask again tomorrow", own, share);
			return FALSE;
		}
	}

	/* Approved at once: Axis looks for it already, or the person's switch. */
	if (approved_how == NULL && auto_on) {
		if (!axis_requests_defaults_read(self, conn, &defaults, error))
			return FALSE;
		wait_reason = auto_wait_reason(self, &defaults, ask->kind, listed->len, &root_id, &profile_id);
		if (wait_reason == NULL)
			approved_how = "auto";
	}
	if (approved_how == NULL || g_strcmp0(approved_how, "existing") == 0) {
		root_id = 0;
		profile_id = 0;
	}

	/* A declined request of the same thing is replaced by this one. */
	{
		g_autofree gchar *numbers = int_array_text(listed);
		const gchar *params[] = { title, numbers };

		if (ask->kind == AXIS_REQUEST_MOVIE) {
			if (!axis_db_conn_exec(conn, "UPDATE request SET decision = 'replaced' WHERE title_id = $1::bigint"
			        " AND kind = 'movie' AND decision = 'declined'", 1, params, NULL, error))
				return FALSE;
		} else if (!axis_db_conn_exec(conn, "UPDATE request r SET decision = 'replaced' WHERE r.title_id = $1::bigint"
		               " AND r.kind = 'series' AND r.decision = 'declined' AND NOT EXISTS (SELECT 1 FROM request_season s"
		               " WHERE s.request_id = r.id AND s.season <> ALL ($2::integer[]))", 2, params, NULL, error)) {
			return FALSE;
		}
	}
	{
		g_autofree gchar *public_id = axis_random_public_id();
		g_autofree gchar *root = root_id != 0 ? axis_requests_int_text(root_id) : NULL;
		g_autofree gchar *profile = profile_id != 0 ? axis_requests_int_text(profile_id) : NULL;
		g_autofree gchar *managed = managed_id != 0 ? axis_requests_int_text(managed_id) : NULL;
		g_autoptr(AxisDbResult) inserted = NULL;
		gboolean existing = g_strcmp0(approved_how, "existing") == 0;
		const gchar *params[] = {
			public_id, title, axis_request_kind_name(ask->kind), user, username, ask->portal ? "portal" : "web", now,
			approved_how != NULL ? "approved" : "waiting", approved_how, wait_reason, root, profile,
			ask->kind == AXIS_REQUEST_MOVIE ? managed : NULL, ask->kind == AXIS_REQUEST_SERIES ? managed : NULL,
			existing ? "t" : "f"
		};

		if (!axis_db_conn_exec(conn, "INSERT INTO request (public_id, title_id, kind, requested_by, requested_name,"
		        " made_by, via, created_at, counted_at, decision, approved_how, wait_reason, decided_at, root_id,"
		        " quality_profile_id, movie_id, series_id, added_at)"
		        " VALUES ($1, $2::bigint, $3, $4::bigint, $5, $4::bigint, $6, " AXIS_REQUESTS_TS(7) ", "
		        AXIS_REQUESTS_TS(7) ", $8, $9, $10, CASE WHEN $8 = 'approved' THEN " AXIS_REQUESTS_TS(7) " END,"
		        " $11::bigint, $12::bigint, $13::bigint, $14::bigint, CASE WHEN $15::boolean THEN " AXIS_REQUESTS_TS(7)
		        " END) RETURNING id", G_N_ELEMENTS(params), params, &inserted, error))
			return FALSE;
		axis_db_result_get_int64(inserted, 0, 0, &made->request_id);
		made->public_id = g_steal_pointer(&public_id);
	}
	if (ask->kind == AXIS_REQUEST_SERIES) {
		g_autofree gchar *request = axis_requests_int_text(made->request_id);
		g_autofree gchar *numbers = int_array_text(listed);
		const gchar *params[] = { request, title, numbers };

		if (!axis_db_conn_exec(conn, "INSERT INTO request_season (request_id, title_id, season)"
		        " SELECT $1::bigint, $2::bigint, unnest($3::integer[])", 3, params, NULL, error))
			return FALSE;
		g_array_append_vals(made->seasons, listed->data, listed->len);
	}
	made->outcome = AXIS_REQUEST_MADE;
	made->decision = approved_how != NULL ? AXIS_REQUEST_APPROVED : AXIS_REQUEST_WAITING;
	made->approved_how = g_strdup(approved_how);
	made->wait_reason = g_strdup(wait_reason);
	return TRUE;
}

gboolean
axis_requests_ask(
	AxisRequests         *self,
	const AxisRequestAsk *ask,
	AxisRequestMade      *made,
	GError              **error
){
	g_autoptr(GArray) films = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) series = g_array_new(FALSE, FALSE, sizeof(gint64));
	AxisRequestsFacts facts = { NULL, NULL };
	AxisDbConn *conn;
	gboolean ok;
	guint i;
	guint j;

	memset(made, 0, sizeof(*made));
	made->seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	made->left_available = g_array_new(FALSE, FALSE, sizeof(gint));
	made->left_requested = g_array_new(FALSE, FALSE, sizeof(gint));

	if (ask->kind != AXIS_REQUEST_MOVIE && ask->kind != AXIS_REQUEST_SERIES)
		return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "a request is for a movie or a series");
	if (!axis_requests_kind_on(self, ask->kind))
		return refuse(error, AXIS_REQUESTS_ERROR_KIND_OFF, ask->kind == AXIS_REQUEST_SERIES ?
			"series cannot be requested here" : "movies cannot be requested here");
	if (ask->title_id <= 0 || ask->user_id <= 0)
		return refuse(error, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such title");
	if (ask->kind == AXIS_REQUEST_SERIES) {
		if ((guint)ask->seasons > AXIS_REQUEST_SEASONS_PICKED)
			return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "choose the seasons to ask for");
		if (ask->seasons == AXIS_REQUEST_SEASONS_PICKED) {
			if (ask->n_picked == 0 || ask->picked == NULL)
				return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "pick at least one season");
			if (ask->n_picked > AXIS_REQUESTS_MAX_SEASONS)
				return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the request lists too many seasons");
			for (i = 0; i < ask->n_picked; i++) {
				if (ask->picked[i] < 0 || ask->picked[i] > 10000)
					return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "the title's record has no such season");
				for (j = 0; j < i; j++) {
					if (ask->picked[j] == ask->picked[i])
						return refuse(error, AXIS_REQUESTS_ERROR_INVALID, "a season is picked twice");
				}
			}
		}
	}

	g_array_append_val(ask->kind == AXIS_REQUEST_SERIES ? series : films, ask->title_id);
	if (!axis_requests_facts_load(self, films, series, &facts, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_requests_facts_clear(&facts);
		return FALSE;
	}
	ok = axis_db_conn_begin(conn, error) && ask_locked(self, conn, ask, &facts, made, error);
	/* Refused: nothing of it stays, not even what it began to follow. */
	if (ok)
		ok = axis_db_conn_commit(conn, error);
	else
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	axis_requests_facts_clear(&facts);
	if (!ok) {
		/* Two requests for the same thing cannot both be made: the title's
		 * lock orders them, and the unique index is the last word. */
		if (error != NULL && g_error_matches(*error, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_clear_error(error);
			refuse(error, AXIS_REQUESTS_ERROR_INVALID, "it was asked for at the same moment; ask again");
		}
		return FALSE;
	}
	if (made->outcome == AXIS_REQUEST_MADE && made->decision == AXIS_REQUEST_APPROVED) {
		if (g_strcmp0(made->approved_how, "auto") == 0) {
			g_autoptr(GError) local = NULL;

			/* The same placing as an administrator's approval.  The request
			 * is approved whatever comes of it. */
			if (!axis_requests_place(self, made->request_id, &made->placed, &local))
				g_message("requests: an approved request could not be placed: %s", local->message);
		} else {
			made->placed = TRUE;
		}
	}
	{
		g_autoptr(GError) local = NULL;

		if (!axis_requests_get_quota(self, ask->user_id, &made->quota, &local))
			g_debug("requests: the quota after a request could not be read: %s", local->message);
	}
	return TRUE;
}
