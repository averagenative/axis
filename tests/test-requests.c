/*
 * test-requests.c - the request model: asking, seasons, quotas, followers,
 * decisions, automatic approval, the state worked out from the engine's own
 * records, and the races between two requests and between two decisions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P1; the `requests` specification.)  Each test
 * has its own database and scratch directory; the clock of the requests and
 * of the TV module stands still (AXIS_TEST_REQUESTS_NOW_US) and the calendar
 * is read in UTC, so no test depends on the day it runs.  The tests of races
 * hold in every interleaving: they assert the outcome the rule allows, never
 * an order.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-requests.h"
#include "requests/axis-requests-private.h"
#include "web/axis-user-store.h"

typedef struct
{
	AxisTestRequests *r;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->r = axis_test_requests_new();
	if (fx->r == NULL)
		return;
	axis_test_requests_declare(fx->r);
	axis_test_requests_ready(fx->r);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_requests_free(fx->r);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->r != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

#define Q(...) axis_test_requests_q(fx->r, __VA_ARGS__)
#define N(...) axis_test_requests_n(fx->r, __VA_ARGS__)

static gint64
film(
	Fixture     *fx,
	const gchar *id
){
	return axis_test_requests_title(fx->r, AXIS_META_KIND_FILM, id);
}

static gint64
series(
	Fixture     *fx,
	const gchar *id
){
	return axis_test_requests_title(fx->r, AXIS_META_KIND_SERIES, id);
}

static gboolean
ask_movie(
	Fixture          *fx,
	gint64            user,
	gint64            title,
	AxisRequestMade  *made,
	GError          **error
){
	AxisRequestAsk ask;

	memset(&ask, 0, sizeof(ask));
	ask.user_id = user;
	ask.title_id = title;
	ask.kind = AXIS_REQUEST_MOVIE;
	return axis_requests_ask(fx->r->requests, &ask, made, error);
}

static gboolean
ask_series(
	Fixture             *fx,
	gint64               user,
	gint64               title,
	AxisRequestSeasons   seasons,
	const gint          *picked,
	guint                n_picked,
	AxisRequestMade     *made,
	GError             **error
){
	AxisRequestAsk ask;

	memset(&ask, 0, sizeof(ask));
	ask.user_id = user;
	ask.title_id = title;
	ask.kind = AXIS_REQUEST_SERIES;
	ask.seasons = seasons;
	ask.picked = picked;
	ask.n_picked = n_picked;
	return axis_requests_ask(fx->r->requests, &ask, made, error);
}

/* A movie request that must be made; its public identifier. */
static gchar *
must_ask_movie(
	Fixture *fx,
	gint64   user,
	gint64   title
){
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	gchar *id;

	g_assert_true(ask_movie(fx, user, title, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	id = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	return id;
}

static gchar *
seasons_of(
	Fixture     *fx,
	const gchar *public_id
){
	return Q("SELECT coalesce(string_agg(s.season::text, ',' ORDER BY s.season), '') FROM request r"
		" LEFT JOIN request_season s ON s.request_id = r.id WHERE r.public_id = '%s'", public_id);
}

#define assert_decision(fx, id, expected) G_STMT_START { \
	g_autofree gchar *decision_ = Q("SELECT decision FROM request WHERE public_id = '%s'", id); \
	g_assert_cmpstr(decision_, ==, expected); \
} G_STMT_END

static AxisRequestInfo *
info_of(
	Fixture     *fx,
	const gchar *public_id
){
	g_autoptr(GError) error = NULL;
	AxisRequestInfo *info = NULL;

	g_assert_true(axis_requests_get(fx->r->requests, public_id, 0, TRUE, &info, &error));
	g_assert_no_error(error);
	return info;
}

static AxisRequestQuota
quota_of(
	Fixture *fx,
	gint64   user
){
	g_autoptr(GError) error = NULL;
	AxisRequestQuota quota;

	g_assert_true(axis_requests_get_quota(fx->r->requests, user, &quota, &error));
	g_assert_no_error(error);
	return quota;
}

/* The state of one title. */
static AxisRequestTitleState *
title_state(
	Fixture     *fx,
	gint64       title,
	GHashTable **keep
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) error = NULL;
	AxisRequestTitleState *state;

	g_array_append_val(ids, title);
	g_assert_true(axis_requests_title_states(fx->r->requests, ids, keep, &error));
	g_assert_no_error(error);
	state = g_hash_table_lookup(*keep, &title);
	g_assert_nonnull(state);
	return state;
}

static AxisRequestState
state_of_title(
	Fixture *fx,
	gint64   title
){
	g_autoptr(GHashTable) keep = NULL;

	return title_state(fx, title, &keep)->state;
}

static void
set_person(
	Fixture  *fx,
	gint64    user,
	gboolean  auto_movies,
	gboolean  auto_series,
	gint      movies_limit,
	gint      seasons_limit
){
	g_autoptr(GError) error = NULL;
	AxisRequestPerson person;

	memset(&person, 0, sizeof(person));
	person.id = user;
	person.auto_movies = auto_movies;
	person.auto_series = auto_series;
	person.movies_limit = movies_limit;
	person.seasons_limit = seasons_limit;
	g_assert_true(axis_requests_set_person(fx->r->requests, &person, &error));
	g_assert_no_error(error);
}

static void
approve(
	Fixture     *fx,
	const gchar *public_id
){
	g_autoptr(GError) error = NULL;
	gboolean added = FALSE;

	g_assert_true(axis_requests_approve(fx->r->requests, public_id, "root", 0, 0, &added, &error));
	g_assert_no_error(error);
	g_assert_true(added);
}

/* ── The model ──────────────────────────────────────────────────── */

/* Scenario: A movie. */
static void
test_movie(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autoptr(AxisRequestInfo) info = info_of(fx, id);

	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 1);
	g_assert_cmpint(info->title_id, ==, orchard);
	g_assert_cmpint(info->kind, ==, AXIS_REQUEST_MOVIE);
	g_assert_cmpint(info->requested_by, ==, fx->r->robin);
	g_assert_cmpstr(info->requested_name, ==, "robin");
	g_assert_cmpuint(info->seasons->len, ==, 0);
	g_assert_cmpint(info->decision, ==, AXIS_REQUEST_WAITING);
	g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_REQUESTED);
	/* Named outside the daemon by a random value, never by its row. */
	g_assert_cmpuint(strlen(id), ==, 22);
	g_assert_cmpstr(info->title, ==, "The Lantern Orchard");
	g_assert_cmpint(info->year, ==, 2019);
}

/* Scenarios: The whole series becomes a season list; The first and the
 * latest season. */
static void
test_seasons(Fixture *fx, gconstpointer data)
{
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 clockwork = series(fx, AXIS_TEST_REQUESTS_CLOCKWORK);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *whole = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *latest = NULL;
	AxisRequestMade made;

	/* The record has seasons 0, 1, 2 and 3: the whole series is 1, 2 and 3. */
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	whole = seasons_of(fx, made.public_id);
	g_assert_cmpstr(whole, ==, "1,2,3");
	axis_request_made_clear(&made);

	/* On another series: one person the first, another the latest. */
	g_assert_true(ask_series(fx, fx->r->robin, clockwork, AXIS_REQUEST_SEASONS_FIRST, NULL, 0, &made, &error));
	g_assert_no_error(error);
	first = seasons_of(fx, made.public_id);
	g_assert_cmpstr(first, ==, "1");
	axis_request_made_clear(&made);
	g_assert_true(ask_series(fx, fx->r->sam, clockwork, AXIS_REQUEST_SEASONS_LATEST, NULL, 0, &made, &error));
	g_assert_no_error(error);
	latest = seasons_of(fx, made.public_id);
	g_assert_cmpstr(latest, ==, "2");
	axis_request_made_clear(&made);
}

/* The latest season of the series with seasons 0 to 3 is season 3, and the
 * specials are in a request only when picked by number. */
static void
test_latest_and_specials(Fixture *fx, gconstpointer data)
{
	static const gint specials[] = { 0 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *latest = NULL;
	g_autofree gchar *picked = NULL;
	AxisRequestMade made;

	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_LATEST, NULL, 0, &made, &error));
	g_assert_no_error(error);
	latest = seasons_of(fx, made.public_id);
	g_assert_cmpstr(latest, ==, "3");
	axis_request_made_clear(&made);
	g_assert_true(ask_series(fx, fx->r->sam, harbour, AXIS_REQUEST_SEASONS_PICKED, specials, 1, &made, &error));
	g_assert_no_error(error);
	picked = seasons_of(fx, made.public_id);
	g_assert_cmpstr(picked, ==, "0");
	axis_request_made_clear(&made);
}

/* Scenario: No episodes and nothing typed (the model's part: a season the
 * title's record does not have, a kind that is not the title's, nothing
 * picked; the form's part is /web/requests/forged-fields). */
static void
test_refused_whole(Fixture *fx, gconstpointer data)
{
	static const gint missing[] = { 1, 7 };
	static const gint twice[] = { 1, 1 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	g_assert_false(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, missing, 2, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_false(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, twice, 2, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_false(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, NULL, 0, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	/* A movie asked for as a series, and a series as a movie. */
	g_assert_false(ask_series(fx, fx->r->robin, orchard, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_false(ask_movie(fx, fx->r->robin, harbour, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	/* No such title, no such account. */
	g_assert_false(ask_movie(fx, fx->r->robin, 987654321, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_false(ask_movie(fx, 987654321, orchard, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	g_clear_error(&error);
	axis_request_made_clear(&made);

	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request_season"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);
}

/* Scenario: A module that is off (a layer without the TV module; with the
 * module registered and switched off: /web/requests/module-off). */
static void
test_module_off(Fixture *fx, gconstpointer data)
{
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autoptr(AxisRequests) without = axis_requests_new(fx->r->t->pool, fx->r->settings, fx->r->meta, fx->r->movies, NULL,
		NULL, NULL);
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	AxisRequestAsk ask;

	memset(&ask, 0, sizeof(ask));
	ask.user_id = fx->r->robin;
	ask.title_id = harbour;
	ask.kind = AXIS_REQUEST_SERIES;
	g_assert_false(axis_requests_ask(without, &ask, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_KIND_OFF);
	g_assert_nonnull(strstr(error->message, "series cannot be requested"));
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
}

/* ── Deciding ───────────────────────────────────────────────────── */

/* Scenarios: Waiting fetches nothing; Off by default. */
static void
test_waiting_fetches_nothing(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autofree gchar *id = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	/* Even with the defaults set: a new account's switches are off. */
	axis_test_requests_set_defaults(fx->r);
	id = must_ask_movie(fx, fx->r->robin, orchard);
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	g_assert_null(made.approved_how);
	axis_request_made_clear(&made);
	assert_decision(fx, id, "waiting");
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM series"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM job"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM movie_search"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM download"), ==, 0);
}

/* Scenarios: Approval adds and searches; Defaults are used. */
static void
test_approval_adds_and_searches(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autoptr(AxisRequestInfo) info = NULL;

	axis_test_requests_set_defaults(fx->r);
	approve(fx, id);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT " AND root_id = %" G_GINT64_FORMAT
		" AND quality_profile_id = %" G_GINT64_FORMAT, orchard, fx->r->films_root, fx->r->profile_id), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 1);
	info = info_of(fx, id);
	g_assert_cmpint(info->decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpstr(info->approved_how, ==, "admin");
	g_assert_cmpstr(info->decided_by, ==, "root");
	g_assert_cmpint(info->movie_id, >, 0);
	g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_APPROVED);
}

/* Scenario: Approval of seasons. */
static void
test_approval_of_seasons(Fixture *fx, gconstpointer data)
{
	static const gint picked[] = { 2, 3 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *monitored = NULL;
	AxisRequestMade made;

	axis_test_requests_set_defaults(fx->r);
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, picked, 2, &made, &error));
	g_assert_no_error(error);
	approve(fx, made.public_id);
	axis_request_made_clear(&made);
	monitored = Q("SELECT string_agg(ss.number || ':' || ss.monitored, ',' ORDER BY ss.number) FROM series s"
		" JOIN series_season ss ON ss.series_id = s.id WHERE s.title_id = %" G_GINT64_FORMAT, harbour);
	g_assert_cmpstr(monitored, ==, "0:false,1:false,2:true,3:true");
	/* Its episodes follow their season. */
	g_assert_cmpint(N("SELECT count(*) FROM series_episode WHERE monitored AND season_number NOT IN (2, 3)"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM series_episode WHERE monitored AND season_number IN (2, 3)"), ==, 4);
	g_assert_cmpint(N("SELECT count(*) FROM job WHERE kind = 'tv.search'"), >=, 1);
}

/* Scenario: Approval widens and never narrows. */
static void
test_widens_never_narrows(Fixture *fx, gconstpointer data)
{
	static const gint picked[] = { 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 other_root = axis_test_library_add_root(fx->r->t, fx->r->volume_id, fx->r->disk, "series-two", AXIS_MEDIA_TV);
	gint64 series_id = axis_test_requests_add_series(fx->r, harbour);
	gint64 its_profile = N("SELECT quality_profile_id FROM series WHERE id = %" G_GINT64_FORMAT, series_id);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *monitored = NULL;
	AxisRequestDefaults defaults;
	AxisRequestMade made;

	g_assert_cmpint(its_profile, !=, fx->r->profile_id);
	g_assert_true(axis_tv_set_season_monitored(fx->r->tv, series_id, 1, TRUE, "root", &error));
	g_assert_no_error(error);
	/* The defaults name another folder: a series Axis has keeps its own. */
	memset(&defaults, 0, sizeof(defaults));
	defaults.series_root = other_root;
	defaults.series_profile = fx->r->profile_id;
	g_assert_true(axis_requests_set_defaults(fx->r->requests, &defaults, &error));
	g_assert_no_error(error);
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, picked, 1, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	approve(fx, made.public_id);
	axis_request_made_clear(&made);
	monitored = Q("SELECT string_agg(number || ':' || monitored, ',' ORDER BY number) FROM series_season"
		" WHERE series_id = %" G_GINT64_FORMAT, series_id);
	g_assert_cmpstr(monitored, ==, "0:false,1:true,2:true,3:false");
	g_assert_cmpint(N("SELECT count(*) FROM series"), ==, 1);
	g_assert_cmpint(N("SELECT root_id FROM series WHERE id = %" G_GINT64_FORMAT, series_id), ==, fx->r->series_root);
	g_assert_cmpint(N("SELECT quality_profile_id FROM series WHERE id = %" G_GINT64_FORMAT, series_id), ==,
		its_profile);
}

/* Scenario: A decline has a reason. */
static void
test_decline_has_reason(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *long_reason = g_strnfill(301, 'x');
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(AxisRequestInfo) own = NULL;

	/* No reason, one too long, one of two lines: nothing is declined. */
	g_assert_false(axis_requests_decline(fx->r->requests, id, "root", "  ", FALSE, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_requests_decline(fx->r->requests, id, "root", long_reason, FALSE, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_requests_decline(fx->r->requests, id, "root", "one\ntwo", FALSE, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_clear_error(&error);
	assert_decision(fx, id, "waiting");

	g_assert_true(axis_requests_decline(fx->r->requests, id, "root", "Not for this library", FALSE, &error));
	g_assert_no_error(error);
	info = info_of(fx, id);
	g_assert_cmpint(info->decision, ==, AXIS_REQUEST_DECLINED);
	g_assert_cmpstr(info->decline_reason, ==, "Not for this library");
	/* The person who asked reads it. */
	g_assert_true(axis_requests_get(fx->r->requests, id, fx->r->robin, FALSE, &own, &error));
	g_assert_cmpstr(own->decline_reason, ==, "Not for this library");
	g_assert_cmpint(own->state, ==, AXIS_REQUEST_STATE_DECLINED);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM job"), ==, 0);
	/* Decided once. */
	g_assert_false(axis_requests_approve(fx->r->requests, id, "root", fx->r->films_root, fx->r->profile_id, NULL,
		&error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
}

/* Two threads let go at the same moment. */
typedef struct
{
	Fixture      *fx;
	const gchar  *public_id;
	gint64        request_id;
	gint64        user;
	gint64        title;
	gint         *go;
	gint          what;	/* 0 approve, 1 decline, 2 withdraw, 3 ask for a movie, 4 place */
	gboolean      ok;
	gboolean      added;
	GError       *error;
	AxisRequestOutcome outcome;
} Racer;

static gpointer
racer_main(gpointer data)
{
	Racer *racer = data;
	Fixture *fx = racer->fx;

	while (g_atomic_int_get(racer->go) == 0)
		g_thread_yield();
	switch (racer->what) {
	case 0:
		racer->ok = axis_requests_approve(fx->r->requests, racer->public_id, "root", fx->r->films_root,
			fx->r->profile_id, NULL, &racer->error);
		break;
	case 1:
		racer->ok = axis_requests_decline(fx->r->requests, racer->public_id, "other", "Not now", FALSE, &racer->error);
		break;
	case 2:
		racer->ok = axis_requests_withdraw(fx->r->requests, racer->public_id, racer->user, NULL, &racer->error);
		break;
	case 4:
		racer->ok = axis_requests_place(fx->r->requests, racer->request_id, &racer->added, &racer->error);
		break;
	default: {
		AxisRequestMade made;

		racer->ok = ask_movie(fx, racer->user, racer->title, &made, &racer->error);
		racer->outcome = made.outcome;
		axis_request_made_clear(&made);
		break;
	}
	}
	return NULL;
}

/*
 * Lets the racers go, and makes sure they really meet: the test's own
 * connection first holds a lock on the request table that lets every racer
 * read and lock rows but lets none write.  Each racer then runs until it
 * waits, either for that lock (it has made its checks and is about to write)
 * or for a lock of the layer's own, held by a racer that is.  Only when all
 * of them wait is the table let go, so every racer has started before any
 * writes.  The count of waiting racers proves only that: it is the same with
 * or without the layer's locks, since a racer that took none would wait for
 * the table lock all the same, past its checks.  What a lock of the layer's
 * prevents is shown by the outcome each test asserts (code without it lets
 * every racer pass its checks before any writes, and all of them write).  No
 * timing decides the outcome.
 */
static void
race(
	Fixture *fx,
	Racer   *racers,
	guint    n
){
	g_autofree GThread **threads = g_new0(GThread *, n);
	gint64 waiting = 0;
	gint go = 0;
	guint tries;
	guint i;

	axis_test_db_exec(fx->r->t->db, "BEGIN");
	axis_test_db_exec(fx->r->t->db, "LOCK TABLE request IN SHARE MODE");
	for (i = 0; i < n; i++) {
		racers[i].go = &go;
		threads[i] = g_thread_new("racer", racer_main, &racers[i]);
	}
	g_atomic_int_set(&go, 1);
	for (tries = 0; tries < 6000 && waiting < n; tries++) {
		waiting = N("SELECT count(*) FROM pg_locks WHERE NOT granted AND database = (SELECT oid FROM pg_database"
			" WHERE datname = current_database())");
		if (waiting < n)
			g_usleep(10 * 1000);
	}
	axis_test_db_exec(fx->r->t->db, "COMMIT");
	for (i = 0; i < n; i++)
		g_thread_join(threads[i]);
	/* Every racer had started, and was held at a lock, before any wrote. */
	g_assert_cmpint(waiting, ==, n);
}

/* Scenario: Two decisions at once. */
static void
test_two_at_once(Fixture *fx, gconstpointer data)
{
	static const gchar *const films[] = {
		AXIS_TEST_REQUESTS_ORCHARD, AXIS_TEST_REQUESTS_LEDGER, AXIS_TEST_REQUESTS_KILN, AXIS_TEST_REQUESTS_BELLS
	};
	guint i;

	set_person(fx, fx->r->robin, FALSE, FALSE, 50, 50);
	for (i = 0; i < G_N_ELEMENTS(films); i++) {
		gint64 title = film(fx, films[i]);
		g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, title);
		g_autofree gchar *decision = NULL;
		const GError *lost;
		Racer racers[2];

		memset(racers, 0, sizeof(racers));
		racers[0].fx = racers[1].fx = fx;
		racers[0].public_id = racers[1].public_id = id;
		racers[0].what = 0;
		racers[1].what = 1;
		race(fx, racers, 2);
		/* Exactly one took effect; the other was told it was decided. */
		g_assert_cmpint(racers[0].ok + racers[1].ok, ==, 1);
		lost = racers[0].ok ? racers[1].error : racers[0].error;
		g_assert_error(lost, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
		g_assert_nonnull(strstr(lost->message, "already decided"));
		decision = Q("SELECT decision FROM request WHERE public_id = '%s'", id);
		g_assert_cmpstr(decision, ==, racers[0].ok ? "approved" : "declined");
		g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, title), ==,
			racers[0].ok ? 1 : 0);
		g_clear_error(&racers[0].error);
		g_clear_error(&racers[1].error);
	}
}

/*
 * Two placings of one series at the same moment, for two people's requests:
 * one lists a season the series' record has lost since it was made, the other
 * a season it has.  Neither undoes the other: the series is added once and
 * stays, the season that exists is monitored, the request that could not be
 * completed says why, and "Try again" completes it on the same series once
 * the season is there.
 */
static void
test_two_placings(Fixture *fx, gconstpointer data)
{
	static const gint one[] = { 1 };
	static const gint two[] = { 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autofree gchar *failing = NULL;
	g_autofree gchar *working = NULL;
	g_autofree gchar *monitored = NULL;
	g_autofree gchar *completed = NULL;
	g_autoptr(AxisRequestInfo) stuck = NULL;
	g_autoptr(AxisRequestInfo) fine = NULL;
	g_autoptr(AxisRequestInfo) done = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	Racer racers[2];
	gboolean added = FALSE;
	gint64 series_id;
	guint i;

	axis_test_requests_set_defaults(fx->r);
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, one, 1, &made, &error));
	g_assert_no_error(error);
	failing = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	g_assert_true(ask_series(fx, fx->r->sam, harbour, AXIS_REQUEST_SEASONS_PICKED, two, 1, &made, &error));
	g_assert_no_error(error);
	working = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	/* robin's season is one the record no longer has (renumbered since). */
	N("UPDATE request_season SET season = 7 WHERE request_id = (SELECT id FROM request WHERE public_id = '%s')"
		" RETURNING season", failing);
	/* Both approved, as the approval's transaction leaves them, and placed
	 * at the same moment. */
	N("UPDATE request SET decision = 'approved', approved_how = 'admin', decided_by = 'root',"
		" decided_at = 'epoch'::timestamptz + %" G_GINT64_FORMAT " * interval '1 microsecond', root_id = %"
		G_GINT64_FORMAT ", quality_profile_id = %" G_GINT64_FORMAT " WHERE title_id = %" G_GINT64_FORMAT
		" RETURNING id", AXIS_TEST_REQUESTS_NOW_US, fx->r->series_root, fx->r->profile_id, harbour);
	memset(racers, 0, sizeof(racers));
	for (i = 0; i < 2; i++) {
		racers[i].fx = fx;
		racers[i].what = 4;
		racers[i].request_id = N("SELECT id FROM request WHERE public_id = '%s'", i == 0 ? failing : working);
	}
	race(fx, racers, 2);
	for (i = 0; i < 2; i++) {
		g_assert_no_error(racers[i].error);
		g_assert_true(racers[i].ok);
	}
	g_assert_false(racers[0].added);
	g_assert_true(racers[1].added);

	g_assert_cmpint(N("SELECT count(*) FROM series WHERE title_id = %" G_GINT64_FORMAT, harbour), ==, 1);
	series_id = N("SELECT id FROM series WHERE title_id = %" G_GINT64_FORMAT, harbour);
	monitored = Q("SELECT string_agg(number || ':' || monitored, ',' ORDER BY number) FROM series_season"
		" WHERE series_id = %" G_GINT64_FORMAT, series_id);
	g_assert_cmpstr(monitored, ==, "0:false,1:false,2:true,3:false");
	fine = info_of(fx, working);
	g_assert_cmpint(fine->decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpint(fine->series_id, ==, series_id);
	g_assert_cmpint(fine->state, ==, AXIS_REQUEST_STATE_APPROVED);
	stuck = info_of(fx, failing);
	g_assert_cmpint(stuck->decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpint(stuck->state, ==, AXIS_REQUEST_STATE_FAILED);
	g_assert_cmpint(stuck->cause, ==, AXIS_REQUEST_CAUSE_NOT_ADDED);
	g_assert_nonnull(stuck->add_error);

	/* The season is there again: tried again, on the series that stayed. */
	N("UPDATE request_season SET season = 1 WHERE request_id = (SELECT id FROM request WHERE public_id = '%s')"
		" RETURNING season", failing);
	g_assert_true(axis_requests_try_again(fx->r->requests, failing, "root", 0, 0, &added, &error));
	g_assert_no_error(error);
	g_assert_true(added);
	g_assert_cmpint(N("SELECT count(*) FROM series"), ==, 1);
	completed = Q("SELECT string_agg(number || ':' || monitored, ',' ORDER BY number) FROM series_season"
		" WHERE series_id = %" G_GINT64_FORMAT, series_id);
	g_assert_cmpstr(completed, ==, "0:false,1:true,2:true,3:false");
	done = info_of(fx, failing);
	g_assert_cmpint(done->series_id, ==, series_id);
	g_assert_null(done->add_error);
	g_assert_cmpint(done->state, ==, AXIS_REQUEST_STATE_APPROVED);

	/* Alone too: a placing that added a series and could not monitor all of
	 * its seasons leaves it, with the seasons monitored so far. */
	{
		static const gint both[] = { 1, 2 };
		gint64 clockwork = series(fx, AXIS_TEST_REQUESTS_CLOCKWORK);
		g_autofree gchar *partial = NULL;
		g_autofree gchar *left = NULL;

		g_assert_true(ask_series(fx, fx->r->robin, clockwork, AXIS_REQUEST_SEASONS_PICKED, both, 2, &made, &error));
		g_assert_no_error(error);
		partial = g_strdup(made.public_id);
		axis_request_made_clear(&made);
		N("UPDATE request_season SET season = 9 WHERE season = 2 AND request_id = (SELECT id FROM request"
			" WHERE public_id = '%s') RETURNING season", partial);
		added = TRUE;
		g_assert_true(axis_requests_approve(fx->r->requests, partial, "root", 0, 0, &added, &error));
		g_assert_no_error(error);
		g_assert_false(added);
		left = Q("SELECT string_agg(ss.number || ':' || ss.monitored, ',' ORDER BY ss.number) FROM series s"
			" JOIN series_season ss ON ss.series_id = s.id WHERE s.title_id = %" G_GINT64_FORMAT, clockwork);
		g_assert_cmpstr(left, ==, "1:true,2:false");
	}
}

/* ── Automatic approval ─────────────────────────────────────────── */

/* Scenarios: A movie approved automatically; One switch does not open the
 * other. */
static void
test_auto(Fixture *fx, gconstpointer data)
{
	static const gint one[] = { 1 };
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisRequestInfo) info = NULL;
	AxisRequestMade made;

	axis_test_requests_set_defaults(fx->r);
	set_person(fx, fx->r->robin, TRUE, FALSE, -1, -1);
	g_assert_true(ask_movie(fx, fx->r->robin, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpstr(made.approved_how, ==, "auto");
	g_assert_true(made.placed);
	info = info_of(fx, made.public_id);
	axis_request_made_clear(&made);
	g_assert_cmpstr(info->approved_how, ==, "auto");
	g_assert_null(info->decided_by);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT " AND root_id = %" G_GINT64_FORMAT
		" AND quality_profile_id = %" G_GINT64_FORMAT, orchard, fx->r->films_root, fx->r->profile_id), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 1);

	/* Only the movies switch is on: a season of a series waits. */
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, one, 1, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	g_assert_null(made.wait_reason);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM series"), ==, 0);

	/* Another person's switch is their own. */
	g_assert_true(ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_LEDGER), &made, &error));
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	axis_request_made_clear(&made);
}

/* Scenario: New accounts start with the switches the settings say.  Both
 * settings on: an account made now has both switches on and its request is
 * approved when it is made; an account made before keeps its switches; the
 * settings off (the default) give an account with both off. */
static void
test_new_account_switches(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	static const gint one[] = { 1 };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisUser) early = NULL;
	g_autoptr(AxisUser) late = NULL;
	g_autoptr(AxisUser) off = NULL;
	AxisRequestMade made;

	axis_test_requests_set_defaults(fx->r);
	g_assert_false(axis_settings_get_boolean(fx->r->settings, "requests.new_auto_movies"));
	g_assert_false(axis_settings_get_boolean(fx->r->settings, "requests.new_auto_series"));
	early = axis_user_store_create(fx->r->t->pool, "early", "$argon2id$not-a-password", AXIS_USER_ROLE_USER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(early);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'early' AND NOT request_auto_movies"
		" AND NOT request_auto_series"), ==, 1);

	g_assert_true(axis_requests_set_new_account_switches(fx->r->requests, TRUE, TRUE, &error));
	g_assert_no_error(error);
	late = axis_user_store_create(fx->r->t->pool, "late", "$argon2id$not-a-password", AXIS_USER_ROLE_USER, &error);
	g_assert_no_error(error);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'late' AND request_auto_movies"
		" AND request_auto_series"), ==, 1);
	/* The account made before keeps off. */
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'early' AND NOT request_auto_movies"
		" AND NOT request_auto_series"), ==, 1);

	g_assert_true(ask_movie(fx, late->id, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpstr(made.approved_how, ==, "auto");
	axis_request_made_clear(&made);
	g_assert_true(ask_series(fx, late->id, harbour, AXIS_REQUEST_SEASONS_PICKED, one, 1, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_APPROVED);
	axis_request_made_clear(&made);
	g_assert_true(ask_movie(fx, early->id, film(fx, AXIS_TEST_REQUESTS_LEDGER), &made, &error));
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	axis_request_made_clear(&made);

	/* Switched off again: the next account starts off; "late" keeps on. */
	g_assert_true(axis_requests_set_new_account_switches(fx->r->requests, FALSE, FALSE, &error));
	off = axis_user_store_create(fx->r->t->pool, "off", "$argon2id$not-a-password", AXIS_USER_ROLE_USER, &error);
	g_assert_no_error(error);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'off' AND NOT request_auto_movies"
		" AND NOT request_auto_series"), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'late' AND request_auto_movies"
		" AND request_auto_series"), ==, 1);
}

/* Scenario: Too many seasons for automatic approval; and a series within
 * the cap is approved automatically, with its seasons monitored. */
static void
test_auto_seasons(Fixture *fx, gconstpointer data)
{
	gint64 winters = series(fx, AXIS_TEST_REQUESTS_WINTERS);
	gint64 clockwork = series(fx, AXIS_TEST_REQUESTS_CLOCKWORK);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisRequestInfo) info = NULL;
	AxisRequestMade made;

	axis_test_requests_set_defaults(fx->r);
	set_person(fx, fx->r->robin, FALSE, TRUE, -1, 20);
	g_assert_true(ask_series(fx, fx->r->robin, winters, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(made.seasons->len, ==, 9);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	g_assert_cmpstr(made.wait_reason, ==, "too_many_seasons");
	info = info_of(fx, made.public_id);
	g_assert_cmpstr(info->wait_reason, ==, "too_many_seasons");
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM series"), ==, 0);

	g_assert_true(ask_series(fx, fx->r->robin, clockwork, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpstr(made.approved_how, ==, "auto");
	g_assert_true(made.placed);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM series_season ss JOIN series s ON s.id = ss.series_id"
		" WHERE s.title_id = %" G_GINT64_FORMAT " AND ss.monitored", clockwork), ==, 2);
}

/* Scenario: The defaults are gone; and no defaults at all. */
static void
test_auto_defaults_gone(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 other_root = axis_test_library_add_root(fx->r->t, fx->r->volume_id, fx->r->disk, "films-two",
		AXIS_MEDIA_MOVIES);
	g_autoptr(GError) error = NULL;
	AxisRequestDefaults defaults;
	AxisRequestMade made;

	set_person(fx, fx->r->robin, TRUE, TRUE, -1, -1);
	/* None set yet. */
	g_assert_true(ask_movie(fx, fx->r->robin, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	g_assert_cmpstr(made.wait_reason, ==, "no_defaults");
	axis_request_made_clear(&made);

	/* Set, and its folder removed since. */
	memset(&defaults, 0, sizeof(defaults));
	defaults.movies_root = other_root;
	defaults.movies_profile = fx->r->profile_id;
	g_assert_true(axis_requests_set_defaults(fx->r->requests, &defaults, &error));
	g_assert_no_error(error);
	N("DELETE FROM library_root WHERE id = %" G_GINT64_FORMAT " RETURNING id", other_root);
	g_assert_true(ask_movie(fx, fx->r->robin, ledger, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_WAITING);
	g_assert_cmpstr(made.wait_reason, ==, "defaults_gone");
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	g_assert_true(axis_requests_get_defaults(fx->r->requests, &defaults, &error));
	g_assert_false(defaults.movies_ok);
}

/* ── The quota ──────────────────────────────────────────────────── */

/* Scenarios: The limit is reached; Withdrawing gives the count back; A
 * decline keeps the count; A new window. */
static void
test_quota_movies(Fixture *fx, gconstpointer data)
{
	static const gchar *const films[] = {
		AXIS_TEST_REQUESTS_ORCHARD, AXIS_TEST_REQUESTS_LEDGER, AXIS_TEST_REQUESTS_KILN, AXIS_TEST_REQUESTS_BELLS,
		AXIS_TEST_REQUESTS_COMPASS
	};
	gint64 almanac = film(fx, AXIS_TEST_REQUESTS_ALMANAC);
	gchar *ids[G_N_ELEMENTS(films)] = { NULL };
	g_autofree gchar *words = NULL;
	g_autofree gchar *again = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestQuota quota;
	AxisRequestMade made;
	guint i;

	/* The installation's limit: 5 movies a week. */
	quota = quota_of(fx, fx->r->robin);
	g_assert_cmpint(quota.window, ==, AXIS_REQUEST_WINDOW_WEEK);
	g_assert_cmpuint(quota.movies.limit, ==, 5);
	g_assert_cmpuint(quota.movies.left, ==, 5);
	for (i = 0; i < 3; i++)
		ids[i] = must_ask_movie(fx, fx->r->robin, film(fx, films[i]));
	quota = quota_of(fx, fx->r->robin);
	words = axis_request_quota_words(&quota, AXIS_REQUEST_MOVIE);
	g_assert_cmpstr(words, ==, "2 of 5 left this week");
	for (i = 3; i < 5; i++)
		ids[i] = must_ask_movie(fx, fx->r->robin, film(fx, films[i]));

	/* The sixth is refused whole. */
	g_assert_false(ask_movie(fx, fx->r->robin, almanac, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
	g_assert_nonnull(strstr(error->message, "no movie request is left until Monday"));
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, almanac), ==, 0);
	/* Another person's count is their own; seasons are counted apart. */
	g_assert_cmpuint(quota_of(fx, fx->r->sam).movies.left, ==, 5);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).seasons.left, ==, 5);

	/* A decline keeps the count. */
	g_assert_true(axis_requests_decline(fx->r->requests, ids[0], "root", "Not for this library", FALSE, &error));
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, ==, 0);
	/* Withdrawing gives it back. */
	g_assert_true(axis_requests_withdraw(fx->r->requests, ids[1], fx->r->robin, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, ==, 1);
	again = must_ask_movie(fx, fx->r->robin, almanac);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, ==, 0);

	/* The next window: Monday. */
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 6 * G_TIME_SPAN_DAY);
	quota = quota_of(fx, fx->r->robin);
	g_assert_cmpuint(quota.movies.used, ==, 0);
	g_assert_cmpuint(quota.movies.left, ==, 5);
	/* An administrator is not limited. */
	g_assert_true(quota_of(fx, fx->r->admin).movies.unlimited);
	for (i = 0; i < G_N_ELEMENTS(ids); i++)
		g_free(ids[i]);
}

/* Scenarios: Seasons count one each; A limit of none. */
static void
test_quota_seasons(Fixture *fx, gconstpointer data)
{
	static const gint two[] = { 1, 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 winters = series(fx, AXIS_TEST_REQUESTS_WINTERS);
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	/* 2 of 5 used: 3 are left, and a request for 4 is refused whole. */
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, two, 2, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(made.quota.seasons.left, ==, 3);
	axis_request_made_clear(&made);
	set_person(fx, fx->r->robin, FALSE, FALSE, -1, 5);
	{
		static const gint four[] = { 1, 2, 3, 4 };

		g_assert_false(ask_series(fx, fx->r->robin, winters, AXIS_REQUEST_SEASONS_PICKED, four, 4, &made, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
		g_assert_nonnull(strstr(error->message, "lists 4 seasons and 3 seasons are left"));
		g_clear_error(&error);
		axis_request_made_clear(&made);
	}
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, winters), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request_season WHERE title_id = %" G_GINT64_FORMAT, winters), ==, 0);

	/* A limit of none: refused, whatever the installation allows. */
	set_person(fx, fx->r->sam, FALSE, FALSE, -1, 0);
	g_assert_false(ask_series(fx, fx->r->sam, winters, AXIS_REQUEST_SEASONS_FIRST, NULL, 0, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
	g_assert_nonnull(strstr(error->message, "cannot ask for series"));
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE requested_by = %" G_GINT64_FORMAT, fx->r->sam), ==, 0);
	/* Their movies are not touched by it. */
	g_assert_true(ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_ORCHARD), &made, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);
}

/* The window of the setting: a day and a month, each with a new one. */
static void
test_quota_windows(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *words = NULL;
	AxisRequestQuota quota;
	AxisRequestMade made;

	set_person(fx, fx->r->robin, FALSE, FALSE, 1, -1);
	g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_WINDOW, "day", &error));
	g_assert_no_error(error);
	first = must_ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_ORCHARD));
	g_assert_false(ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_LEDGER), &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
	g_assert_nonnull(strstr(error->message, "until tomorrow"));
	g_clear_error(&error);
	axis_request_made_clear(&made);
	quota = quota_of(fx, fx->r->robin);
	words = axis_request_quota_words(&quota, AXIS_REQUEST_MOVIE);
	g_assert_cmpstr(words, ==, "0 of 1 left today");
	/* Thirteen hours on: the next day. */
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 13 * G_TIME_SPAN_HOUR);
	second = must_ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_LEDGER));

	/* By the month both are in November: none is left until the 1st. */
	g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_WINDOW, "month", &error));
	g_assert_false(ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_KILN), &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
	g_assert_nonnull(strstr(error->message, "until the 1st of next month"));
	g_clear_error(&error);
	axis_request_made_clear(&made);
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 8 * G_TIME_SPAN_DAY);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, ==, 1);
}

/* The calendar window itself: a day, a week from Monday and a month, and the
 * two days of a year that are 23 and 25 hours long.  Pure. */
static void
test_window_bounds(void)
{
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(GTimeZone) berlin = g_time_zone_new_identifier("Europe/Berlin");
	gint64 start = 0;
	gint64 end = 0;

	/* Tuesday 2026-11-24 12:00 UTC. */
	axis_request_window_bounds(utc, AXIS_TEST_REQUESTS_NOW_US, AXIS_REQUEST_WINDOW_DAY, &start, &end);
	g_assert_cmpint(start, ==, AXIS_TEST_REQUESTS_NOW_US - 12 * G_TIME_SPAN_HOUR);
	g_assert_cmpint(end - start, ==, G_TIME_SPAN_DAY);
	axis_request_window_bounds(utc, AXIS_TEST_REQUESTS_NOW_US, AXIS_REQUEST_WINDOW_WEEK, &start, &end);
	/* From Monday the 23rd to Monday the 30th. */
	g_assert_cmpint(start, ==, AXIS_TEST_REQUESTS_NOW_US - 12 * G_TIME_SPAN_HOUR - G_TIME_SPAN_DAY);
	g_assert_cmpint(end - start, ==, 7 * G_TIME_SPAN_DAY);
	axis_request_window_bounds(utc, AXIS_TEST_REQUESTS_NOW_US, AXIS_REQUEST_WINDOW_MONTH, &start, &end);
	g_assert_cmpint(start, ==, AXIS_TEST_REQUESTS_NOW_US - 12 * G_TIME_SPAN_HOUR - 23 * G_TIME_SPAN_DAY);
	g_assert_cmpint(end - start, ==, 30 * G_TIME_SPAN_DAY);
	/* A Monday's first moment is in its own week, and the moment before is
	 * in the week before. */
	axis_request_window_bounds(utc, end, AXIS_REQUEST_WINDOW_MONTH, &start, &end);
	g_assert_cmpint(end - start, ==, 31 * G_TIME_SPAN_DAY);
	{
		gint64 monday = AXIS_TEST_REQUESTS_NOW_US - 12 * G_TIME_SPAN_HOUR - G_TIME_SPAN_DAY;
		gint64 before_start = 0;
		gint64 before_end = 0;

		axis_request_window_bounds(utc, monday, AXIS_REQUEST_WINDOW_WEEK, &start, &end);
		g_assert_cmpint(start, ==, monday);
		axis_request_window_bounds(utc, monday - 1, AXIS_REQUEST_WINDOW_WEEK, &before_start, &before_end);
		g_assert_cmpint(before_end, ==, monday);
		g_assert_cmpint(before_start, ==, monday - 7 * G_TIME_SPAN_DAY);
	}

	if (berlin == NULL) {
		g_test_message("no time zone data for Europe/Berlin here: the days of 23 and 25 hours are not checked");
		return;
	}
	/* 2026-03-29 12:00 UTC: the clocks go forward that day (23 hours). */
	axis_request_window_bounds(berlin, G_GINT64_CONSTANT(1774785600000000), AXIS_REQUEST_WINDOW_DAY, &start, &end);
	g_assert_cmpint(end - start, ==, 23 * G_TIME_SPAN_HOUR);
	axis_request_window_bounds(berlin, G_GINT64_CONSTANT(1774785600000000), AXIS_REQUEST_WINDOW_WEEK, &start, &end);
	g_assert_cmpint(end - start, ==, 7 * G_TIME_SPAN_DAY - G_TIME_SPAN_HOUR);
	axis_request_window_bounds(berlin, G_GINT64_CONSTANT(1774785600000000), AXIS_REQUEST_WINDOW_MONTH, &start, &end);
	g_assert_cmpint(end - start, ==, 31 * G_TIME_SPAN_DAY - G_TIME_SPAN_HOUR);
	/* 2026-10-25 12:00 UTC: they go back (25 hours). */
	axis_request_window_bounds(berlin, G_GINT64_CONSTANT(1792929600000000), AXIS_REQUEST_WINDOW_DAY, &start, &end);
	g_assert_cmpint(end - start, ==, 25 * G_TIME_SPAN_HOUR);
	axis_request_window_bounds(berlin, G_GINT64_CONSTANT(1792929600000000), AXIS_REQUEST_WINDOW_WEEK, &start, &end);
	g_assert_cmpint(end - start, ==, 7 * G_TIME_SPAN_DAY + G_TIME_SPAN_HOUR);
}

/* The quota cannot be passed by racing: with room for one, of many requests
 * posted at the same moment exactly one is made. */
static void
test_quota_race(Fixture *fx, gconstpointer data)
{
	static const gchar *const films[] = {
		AXIS_TEST_REQUESTS_ORCHARD, AXIS_TEST_REQUESTS_LEDGER, AXIS_TEST_REQUESTS_KILN, AXIS_TEST_REQUESTS_BELLS,
		AXIS_TEST_REQUESTS_COMPASS, AXIS_TEST_REQUESTS_ALMANAC
	};
	Racer racers[G_N_ELEMENTS(films)];
	guint made = 0;
	guint i;

	set_person(fx, fx->r->robin, FALSE, FALSE, 1, -1);
	memset(racers, 0, sizeof(racers));
	for (i = 0; i < G_N_ELEMENTS(films); i++) {
		racers[i].fx = fx;
		racers[i].what = 3;
		racers[i].user = fx->r->robin;
		racers[i].title = film(fx, films[i]);
	}
	race(fx, racers, G_N_ELEMENTS(racers));
	for (i = 0; i < G_N_ELEMENTS(racers); i++) {
		if (racers[i].ok) {
			made++;
		} else {
			g_assert_error(racers[i].error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
			g_clear_error(&racers[i].error);
		}
	}
	g_assert_cmpuint(made, ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 1);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, ==, 0);
}

/* Rows made by asking and withdrawing again and again are bounded: whatever
 * became of them, a person who is not an administrator makes at most
 * AXIS_REQUESTS_MAX_A_DAY requests in any 24 hours.  A request withdrawn, or
 * taken over by a follower, stays counted for the person who made it. */
static void
test_daily_ceiling(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 compass = film(fx, AXIS_TEST_REQUESTS_COMPASS);
	g_autofree gchar *withdrawn = NULL;
	g_autofree gchar *handed = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	gboolean handed_over = FALSE;

	set_person(fx, fx->r->robin, FALSE, FALSE, 1000, 1000);
	/* 98 asked and withdrawn in the last hour, as such a loop leaves them,
	 * the oldest 49 minutes ago. */
	N("INSERT INTO request (public_id, title_id, kind, requested_by, requested_name, made_by, created_at, counted_at,"
		" decision, decided_at) SELECT 'ceil' || lpad(i::text, 18, '0'), %" G_GINT64_FORMAT ", 'movie', %"
		G_GINT64_FORMAT ", 'robin', %" G_GINT64_FORMAT ", t, t, 'withdrawn', t FROM generate_series(1, %d) i,"
		" LATERAL (SELECT 'epoch'::timestamptz + (%" G_GINT64_FORMAT " - i::bigint * 30000000) * interval '1 microsecond'"
		" AS t) w"
		" RETURNING 1", orchard, fx->r->robin, fx->r->robin, AXIS_REQUESTS_MAX_A_DAY - 2, AXIS_TEST_REQUESTS_NOW_US);
	/* The 99th, withdrawn: its quota comes back, its place under the
	 * ceiling does not. */
	withdrawn = must_ask_movie(fx, fx->r->robin, orchard);
	g_assert_true(axis_requests_withdraw(fx->r->requests, withdrawn, fx->r->robin, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, ==, 1000);
	/* The 100th, taken over by a follower: still robin's under the ceiling. */
	handed = must_ask_movie(fx, fx->r->robin, compass);
	g_assert_true(ask_movie(fx, fx->r->sam, compass, &made, &error));
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
	axis_request_made_clear(&made);
	g_assert_true(axis_requests_withdraw(fx->r->requests, handed, fx->r->robin, &handed_over, &error));
	g_assert_no_error(error);
	g_assert_true(handed_over);
	g_assert_false(ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_ALMANAC), &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_TOO_MANY);
	g_assert_nonnull(strstr(error->message, "100 requests in the last 24 hours"));
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE made_by = %" G_GINT64_FORMAT, fx->r->robin), ==,
		AXIS_REQUESTS_MAX_A_DAY);
	/* Another person is not touched by it, nor is an administrator who made
	 * as many. */
	g_assert_true(ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_LEDGER), &made, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);
	N("INSERT INTO request (public_id, title_id, kind, requested_by, requested_name, made_by, created_at, counted_at,"
		" decision, decided_at) SELECT 'ceiladmin' || lpad(i::text, 13, '0'), %" G_GINT64_FORMAT ", 'movie', %"
		G_GINT64_FORMAT ", 'root', %" G_GINT64_FORMAT ", now(), now(), 'withdrawn', now() FROM generate_series(1, %d) i"
		" RETURNING 1", orchard, fx->r->admin, fx->r->admin, AXIS_REQUESTS_MAX_A_DAY);
	g_assert_true(ask_movie(fx, fx->r->admin, film(fx, AXIS_TEST_REQUESTS_KILN), &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);

	/* A day after the oldest of robin's, one place is free again. */
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + G_TIME_SPAN_DAY -
		(AXIS_REQUESTS_MAX_A_DAY - 2) * 30 * G_TIME_SPAN_SECOND + G_TIME_SPAN_SECOND);
	g_assert_true(ask_movie(fx, fx->r->robin, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);
	g_assert_false(ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_BELLS), &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_TOO_MANY);
	g_clear_error(&error);
	axis_request_made_clear(&made);
}

/* ── Followers ──────────────────────────────────────────────────── */

/* Scenarios: Following a movie request; Asked twice. */
static void
test_follow_movie(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autoptr(GPtrArray) mine = NULL;
	g_autoptr(GError) error = NULL;
	const AxisRequestInfo *followed;
	AxisRequestMade made;

	/* Asked twice by the same person: nothing changes. */
	g_assert_true(ask_movie(fx, fx->r->robin, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_UNCHANGED);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 1);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.used, ==, 1);

	/* A second person follows it, and is counted nothing. */
	g_assert_true(ask_movie(fx, fx->r->sam, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
	g_assert_cmpuint(made.n_followed, ==, 1);
	g_assert_null(made.public_id);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower WHERE user_id = %" G_GINT64_FORMAT, fx->r->sam), ==, 1);
	g_assert_cmpuint(quota_of(fx, fx->r->sam).movies.used, ==, 0);
	g_assert_true(axis_requests_list_mine(fx->r->requests, fx->r->sam, 50, &mine, &error));
	g_assert_cmpuint(mine->len, ==, 1);
	followed = g_ptr_array_index(mine, 0);
	g_assert_cmpstr(followed->public_id, ==, id);
	g_assert_true(followed->follows);
	g_assert_cmpint(followed->state, ==, AXIS_REQUEST_STATE_REQUESTED);
	/* Asking again as a follower changes nothing either. */
	g_assert_true(ask_movie(fx, fx->r->sam, orchard, &made, &error));
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_UNCHANGED);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 1);
	/* A person whose limit for movies is none follows nothing either. */
	{
		gint64 kim = axis_test_requests_person(fx->r, "kim", FALSE);

		set_person(fx, kim, FALSE, FALSE, 0, -1);
		g_assert_false(ask_movie(fx, kim, orchard, &made, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_QUOTA);
		g_assert_nonnull(strstr(error->message, "cannot ask for movies"));
		g_clear_error(&error);
		axis_request_made_clear(&made);
		g_assert_cmpint(N("SELECT count(*) FROM request_follower WHERE user_id = %" G_GINT64_FORMAT, kim), ==, 0);
	}
}

/* Asked twice, and by two people, at the same moment: one request. */
static void
test_follow_race(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	Racer racers[4];
	guint i;

	memset(racers, 0, sizeof(racers));
	for (i = 0; i < 4; i++) {
		racers[i].fx = fx;
		racers[i].what = 3;
	}
	/* The same person twice for one film; two people for another. */
	racers[0].user = racers[1].user = fx->r->robin;
	racers[0].title = racers[1].title = orchard;
	racers[2].user = fx->r->robin;
	racers[3].user = fx->r->sam;
	racers[2].title = racers[3].title = ledger;
	race(fx, racers, 4);
	for (i = 0; i < 4; i++) {
		g_assert_no_error(racers[i].error);
		g_assert_true(racers[i].ok);
	}
	g_assert_cmpint((racers[0].outcome == AXIS_REQUEST_MADE) + (racers[1].outcome == AXIS_REQUEST_MADE), ==, 1);
	g_assert_cmpint((racers[0].outcome == AXIS_REQUEST_UNCHANGED) + (racers[1].outcome == AXIS_REQUEST_UNCHANGED), ==,
		1);
	g_assert_cmpint((racers[2].outcome == AXIS_REQUEST_MADE) + (racers[3].outcome == AXIS_REQUEST_MADE), ==, 1);
	g_assert_cmpint((racers[2].outcome == AXIS_REQUEST_FOLLOWED) + (racers[3].outcome == AXIS_REQUEST_FOLLOWED), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, orchard), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 1);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.used + quota_of(fx, fx->r->sam).movies.used, ==, 2);
}

/* Scenarios: Seasons are left out, not refused; Nothing remains; Seasons of
 * a series. */
static void
test_follow_seasons(Fixture *fx, gconstpointer data)
{
	static const gint two[] = { 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 series_id = axis_test_requests_add_series(fx->r, harbour);
	g_autoptr(GHashTable) keep = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *listed = NULL;
	const AxisRequestTitleState *state;
	AxisRequestMade made;
	guint i;

	/* Season 1 is in the library (both aired episodes), and robin has an
	 * open request for season 2. */
	axis_test_requests_episode_files(fx->r, series_id, 1, 2);
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, two, 1, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);

	/* sam asks for the whole series of three seasons. */
	g_assert_true(ask_series(fx, fx->r->sam, harbour, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	listed = seasons_of(fx, made.public_id);
	g_assert_cmpstr(listed, ==, "3");
	g_assert_cmpuint(made.left_available->len, ==, 1);
	g_assert_cmpint(g_array_index(made.left_available, gint, 0), ==, 1);
	g_assert_cmpuint(made.left_requested->len, ==, 1);
	g_assert_cmpint(g_array_index(made.left_requested, gint, 0), ==, 2);
	g_assert_cmpuint(made.n_followed, ==, 1);
	g_assert_cmpuint(made.quota.seasons.used, ==, 1);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower WHERE user_id = %" G_GINT64_FORMAT, fx->r->sam), ==, 1);

	/* Nothing remains for a third person: no request, and they follow both. */
	{
		gint64 kim = axis_test_requests_person(fx->r, "kim", FALSE);

		g_assert_true(ask_series(fx, kim, harbour, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
		g_assert_no_error(error);
		g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
		g_assert_null(made.public_id);
		g_assert_cmpuint(made.n_followed, ==, 2);
		g_assert_cmpuint(made.left_available->len, ==, 1);
		g_assert_cmpuint(made.left_requested->len, ==, 2);
		axis_request_made_clear(&made);
		g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 2);
		g_assert_cmpuint(quota_of(fx, kim).seasons.used, ==, 0);
	}

	/* The series' seasons, each with its state: with half of season 2 on
	 * disk it is partly available, and so is the series. */
	axis_test_requests_episode_files(fx->r, series_id, 2, 1);
	state = title_state(fx, harbour, &keep);
	g_assert_cmpint(state->state, ==, AXIS_REQUEST_STATE_PARTLY);
	g_assert_cmpuint(state->seasons->len, ==, 4);
	for (i = 0; i < state->seasons->len; i++) {
		static const AxisRequestState expected[] = {
			AXIS_REQUEST_STATE_CAN, AXIS_REQUEST_STATE_AVAILABLE, AXIS_REQUEST_STATE_PARTLY,
			AXIS_REQUEST_STATE_REQUESTED
		};
		const AxisRequestSeasonState *season = &g_array_index(state->seasons, AxisRequestSeasonState, i);

		g_assert_cmpint(season->season, ==, (gint)i);
		g_assert_cmpint(season->state, ==, expected[i]);
	}
}

/* Scenarios: Already in the library; A title Axis is already looking for. */
static void
test_follow_engine(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 have = axis_test_requests_add_movie(fx->r, orchard);
	gint64 looking = axis_test_requests_add_movie(fx->r, ledger);
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	axis_test_requests_movie_file(fx->r, have);
	g_assert_true(ask_movie(fx, fx->r->robin, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_ALREADY_AVAILABLE);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_AVAILABLE);

	/* An administrator added it, and it has no file yet: approved without
	 * waiting, counted once, and nothing more is fetched. */
	g_assert_true(ask_movie(fx, fx->r->robin, ledger, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	g_assert_cmpint(made.decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpstr(made.approved_how, ==, "existing");
	g_assert_cmpuint(made.quota.movies.used, ==, 1);
	info = info_of(fx, made.public_id);
	axis_request_made_clear(&made);
	g_assert_cmpint(info->movie_id, ==, looking);
	g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_APPROVED);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 2);
	g_assert_cmpint(N("SELECT count(*) FROM job"), ==, 0);
}

/* ── Declined, closed, withdrawn ────────────────────────────────── */

/* Scenarios: Asked again; Closed to requests; Opened again. */
static void
test_declined(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *first = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *second = NULL;
	g_autofree gchar *third = NULL;
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	g_assert_true(axis_requests_decline(fx->r->requests, first, "root", "Not for this library", FALSE, &error));
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_DECLINED);
	/* Asked again: a new waiting request in the declined one's place. */
	second = must_ask_movie(fx, fx->r->robin, orchard);
	g_assert_cmpstr(second, !=, first);
	assert_decision(fx, first, "replaced");
	assert_decision(fx, second, "waiting");
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_REQUESTED);
	info = info_of(fx, second);
	g_assert_cmpuint(info->declined_before, ==, 1);
	/* It counts like any request: two of this window. */
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.used, ==, 2);

	/* Declined with "Do not ask again": closed to everyone. */
	g_assert_true(axis_requests_decline(fx->r->requests, second, "root", "Not for this library", TRUE, &error));
	g_assert_no_error(error);
	g_assert_false(ask_movie(fx, fx->r->sam, orchard, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_CLOSED);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_CLOSED);
	g_assert_cmpuint(quota_of(fx, fx->r->sam).movies.used, ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE requested_by = %" G_GINT64_FORMAT, fx->r->sam), ==, 0);

	/* Opened again. */
	g_assert_true(axis_requests_set_title_closed(fx->r->requests, orchard, FALSE, "root", &error));
	g_assert_no_error(error);
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_DECLINED);
	third = must_ask_movie(fx, fx->r->sam, orchard);
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_REQUESTED);
	/* A title can be closed without a request, and one that is none cannot. */
	g_assert_true(axis_requests_set_title_closed(fx->r->requests, film(fx, AXIS_TEST_REQUESTS_KILN), TRUE, "root",
		&error));
	g_assert_no_error(error);
	g_assert_false(axis_requests_set_title_closed(fx->r->requests, 987654321, TRUE, "root", &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
}

/* Scenarios: Withdrawn while waiting; Not after approval; Not another
 * person's. */
static void
test_withdraw(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *approved = must_ask_movie(fx, fx->r->robin, ledger);
	g_autofree gchar *again = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *unknown_message = NULL;
	g_autoptr(GError) error = NULL;

	axis_test_requests_set_defaults(fx->r);
	/* Another person's: the answer of a request that does not exist. */
	g_assert_false(axis_requests_withdraw(fx->r->requests, "AAAAAAAAAAAAAAAAAAAAAA", fx->r->sam, NULL, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	unknown_message = g_strdup(error->message);
	g_clear_error(&error);
	before = Q("SELECT md5(r::text) FROM request r WHERE public_id = '%s'", id);
	g_assert_false(axis_requests_withdraw(fx->r->requests, id, fx->r->sam, NULL, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	g_assert_cmpstr(error->message, ==, unknown_message);
	g_clear_error(&error);
	after = Q("SELECT md5(r::text) FROM request r WHERE public_id = '%s'", id);
	g_assert_cmpstr(after, ==, before);
	/* Nor may they read it. */
	{
		AxisRequestInfo *info = NULL;

		g_assert_false(axis_requests_get(fx->r->requests, id, fx->r->sam, FALSE, &info, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
		g_assert_cmpstr(error->message, ==, unknown_message);
		g_clear_error(&error);
		g_assert_null(info);
	}

	/* Withdrawn while waiting. */
	g_assert_true(axis_requests_withdraw(fx->r->requests, id, fx->r->robin, NULL, &error));
	g_assert_no_error(error);
	assert_decision(fx, id, "withdrawn");
	g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_CAN);
	g_assert_false(axis_requests_approve(fx->r->requests, id, "root", 0, 0, NULL, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
	g_clear_error(&error);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	again = must_ask_movie(fx, fx->r->robin, orchard);

	/* Not after approval. */
	approve(fx, approved);
	g_clear_pointer(&before, g_free);
	g_clear_pointer(&after, g_free);
	before = Q("SELECT md5(r::text) FROM request r WHERE public_id = '%s'", approved);
	g_assert_false(axis_requests_withdraw(fx->r->requests, approved, fx->r->robin, NULL, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
	after = Q("SELECT md5(r::text) FROM request r WHERE public_id = '%s'", approved);
	g_assert_cmpstr(after, ==, before);
}

/* A withdrawal that races an approval: one of the two, never both. */
static void
test_withdraw_race(Fixture *fx, gconstpointer data)
{
	static const gchar *const films[] = {
		AXIS_TEST_REQUESTS_ORCHARD, AXIS_TEST_REQUESTS_LEDGER, AXIS_TEST_REQUESTS_KILN, AXIS_TEST_REQUESTS_BELLS
	};
	guint i;

	set_person(fx, fx->r->robin, FALSE, FALSE, 50, 50);
	for (i = 0; i < G_N_ELEMENTS(films); i++) {
		gint64 title = film(fx, films[i]);
		g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, title);
		g_autofree gchar *decision = NULL;
		const GError *lost;
		Racer racers[2];

		memset(racers, 0, sizeof(racers));
		racers[0].fx = racers[1].fx = fx;
		racers[0].public_id = racers[1].public_id = id;
		racers[0].what = 0;
		racers[1].what = 2;
		racers[1].user = fx->r->robin;
		race(fx, racers, 2);
		g_assert_cmpint(racers[0].ok + racers[1].ok, ==, 1);
		lost = racers[0].ok ? racers[1].error : racers[0].error;
		g_assert_error(lost, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
		decision = Q("SELECT decision FROM request WHERE public_id = '%s'", id);
		g_assert_cmpstr(decision, ==, racers[0].ok ? "approved" : "withdrawn");
		/* A withdrawn request fetched nothing. */
		g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, title), ==,
			racers[0].ok ? 1 : 0);
		g_clear_error(&racers[0].error);
		g_clear_error(&racers[1].error);
	}
}

/* Scenario: A follower takes it over; and one without quota does not. */
static void
test_follower_takes_over(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *other = must_ask_movie(fx, fx->r->robin, ledger);
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	gboolean handed = FALSE;

	g_assert_true(ask_movie(fx, fx->r->sam, orchard, &made, &error));
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
	axis_request_made_clear(&made);
	g_assert_true(ask_movie(fx, fx->r->sam, ledger, &made, &error));
	axis_request_made_clear(&made);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.used, ==, 2);

	g_assert_true(axis_requests_withdraw(fx->r->requests, id, fx->r->robin, &handed, &error));
	g_assert_no_error(error);
	g_assert_true(handed);
	info = info_of(fx, id);
	g_assert_cmpint(info->decision, ==, AXIS_REQUEST_WAITING);
	g_assert_cmpint(info->requested_by, ==, fx->r->sam);
	g_assert_cmpstr(info->requested_name, ==, "sam");
	g_assert_cmpuint(info->n_followers, ==, 0);
	g_assert_cmpuint(quota_of(fx, fx->r->sam).movies.used, ==, 1);
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.used, ==, 1);

	/* With nothing left, the follower does not take it: withdrawn for all. */
	set_person(fx, fx->r->sam, FALSE, FALSE, 1, -1);
	handed = TRUE;
	g_assert_true(axis_requests_withdraw(fx->r->requests, other, fx->r->robin, &handed, &error));
	g_assert_no_error(error);
	g_assert_false(handed);
	assert_decision(fx, other, "withdrawn");
	g_assert_cmpuint(quota_of(fx, fx->r->sam).movies.used, ==, 1);
	/* A follower may stop following; a stranger follows nothing to stop. */
	g_assert_false(axis_requests_unfollow(fx->r->requests, id, fx->r->robin, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_true(axis_requests_unfollow(fx->r->requests, other, fx->r->sam, &error));
	g_assert_no_error(error);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);
}

/* ── The state from the engine's records ────────────────────────── */

/* Scenarios: Downloading; A movie is available; A failure in plain words; An
 * import that needs a person; Removed. */
static void
test_movie_states(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(GError) error = NULL;
	gint64 movie;
	gint64 grab;

	axis_test_requests_set_defaults(fx->r);
	approve(fx, id);
	movie = N("SELECT id FROM movie WHERE title_id = %" G_GINT64_FORMAT, orchard);
	before = Q("SELECT md5(r::text) FROM request r WHERE public_id = '%s'", id);

	/* A grab in flight. */
	grab = axis_test_requests_movie_grab(fx->r, movie, NULL);
	{
		g_autoptr(AxisRequestInfo) info = info_of(fx, id);

		g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_DOWNLOADING);
		g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_DOWNLOADING);
	}
	/* It arrived, and its import is blocked: it needs the administrator. */
	axis_test_requests_import_blocked(fx->r, movie, grab);
	{
		g_autoptr(AxisRequestInfo) info = info_of(fx, id);
		g_autofree gchar *words = axis_request_info_words(info);

		g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_FAILED);
		g_assert_cmpint(info->cause, ==, AXIS_REQUEST_CAUSE_NEEDS_ADMIN);
		g_assert_cmpstr(words, ==, "Failed: A download finished and needs the administrator.");
	}
	/* That download failed, and nothing else is in flight. */
	N("DELETE FROM import RETURNING id");
	N("UPDATE movie_grab SET closed_at = now(), close_reason = 'failed' WHERE id = %" G_GINT64_FORMAT " RETURNING id",
		grab);
	{
		g_autoptr(AxisRequestInfo) info = info_of(fx, id);
		g_autofree gchar *words = axis_request_info_words(info);

		g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_FAILED);
		g_assert_cmpint(info->cause, ==, AXIS_REQUEST_CAUSE_DOWNLOAD_FAILED);
		g_assert_cmpstr(words, ==, "Failed: A download failed. Axis is looking again.");
		/* Nothing names the release. */
		g_assert_null(strstr(words, "Made.Up.Release"));
		g_assert_null(info->add_error);
	}
	/* The next one arrived and was imported: in the library. */
	axis_test_requests_movie_grab(fx->r, movie, "imported");
	axis_test_requests_movie_file(fx->r, movie);
	{
		g_autoptr(AxisRequestInfo) info = info_of(fx, id);
		g_autofree gchar *words = axis_request_info_words(info);

		g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_AVAILABLE);
		g_assert_cmpstr(words, ==, "Available: in the library");
		g_assert_cmpint(state_of_title(fx, orchard), ==, AXIS_REQUEST_STATE_AVAILABLE);
	}
	/* All of it was read: the request itself was never written. */
	after = Q("SELECT md5(r::text) FROM request r WHERE public_id = '%s'", id);
	g_assert_cmpstr(after, ==, before);

	/* Removed before it had a file: closed, and the title can be asked for
	 * again; what it used of the quota stays used. */
	{
		gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
		g_autofree gchar *other = must_ask_movie(fx, fx->r->robin, ledger);
		g_autofree gchar *again = NULL;
		g_autofree gchar *words = NULL;
		g_autoptr(AxisRequestInfo) info = NULL;
		g_autoptr(GPtrArray) mine = NULL;
		guint used;
		guint i;

		approve(fx, other);
		used = quota_of(fx, fx->r->robin).movies.used;
		g_assert_true(axis_movies_remove(fx->r->movies, N("SELECT id FROM movie WHERE title_id = %" G_GINT64_FORMAT,
			ledger), "root", &error));
		g_assert_no_error(error);
		assert_decision(fx, other, "removed");
		info = info_of(fx, other);
		words = axis_request_info_words(info);
		g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_REMOVED);
		g_assert_cmpstr(words, ==, "Removed from Axis");
		g_assert_cmpint(state_of_title(fx, ledger), ==, AXIS_REQUEST_STATE_CAN);
		g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.used, ==, used);
		/* The person who asked sees it so among their requests. */
		g_assert_true(axis_requests_list_mine(fx->r->requests, fx->r->robin, 50, &mine, &error));
		g_assert_no_error(error);
		for (i = 0; i < mine->len; i++) {
			const AxisRequestInfo *one = g_ptr_array_index(mine, i);

			if (g_strcmp0(one->public_id, other) == 0)
				g_assert_cmpint(one->state, ==, AXIS_REQUEST_STATE_REMOVED);
		}
		/* Nothing to try again: it is closed. */
		g_assert_false(axis_requests_try_again(fx->r->requests, other, "root", 0, 0, NULL, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
		g_clear_error(&error);
		/* Asked for again: a new request, which waits. */
		again = must_ask_movie(fx, fx->r->robin, ledger);
		assert_decision(fx, again, "waiting");
		assert_decision(fx, other, "removed");
	}
}

/* A series removed from Axis after its request was approved: the request is
 * closed, its seasons are open to a new request, and the series can be asked
 * for again. */
static void
test_series_removed(Fixture *fx, gconstpointer data)
{
	static const gint two[] = { 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autofree gchar *id = NULL;
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	axis_test_requests_set_defaults(fx->r);
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, two, 1, &made, &error));
	g_assert_no_error(error);
	id = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	approve(fx, id);
	g_assert_true(axis_tv_remove(fx->r->tv, N("SELECT id FROM series WHERE title_id = %" G_GINT64_FORMAT, harbour),
		"root", &error));
	g_assert_no_error(error);
	assert_decision(fx, id, "removed");
	g_assert_cmpint(N("SELECT count(*) FROM request_season WHERE open"), ==, 0);
	info = info_of(fx, id);
	g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_REMOVED);
	/* Season 2 can be asked for again, by anyone. */
	g_assert_true(ask_series(fx, fx->r->sam, harbour, AXIS_REQUEST_SEASONS_PICKED, two, 1, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	g_assert_cmpint(made.n_followed, ==, 0);
	axis_request_made_clear(&made);
}

/* Scenario: A series is partly available. */
static void
test_series_partly(Fixture *fx, gconstpointer data)
{
	gint64 clockwork = series(fx, AXIS_TEST_REQUESTS_CLOCKWORK);
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(AxisRequestInfo) full = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *words = NULL;
	g_autofree gchar *id = NULL;
	AxisRequestMade made;
	gint64 series_id;

	axis_test_requests_set_defaults(fx->r);
	g_assert_true(ask_series(fx, fx->r->robin, clockwork, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
	g_assert_no_error(error);
	id = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	approve(fx, id);
	series_id = N("SELECT id FROM series WHERE title_id = %" G_GINT64_FORMAT, clockwork);
	/* Seasons 1 and 2, 16 aired episodes, 7 of them with files. */
	axis_test_requests_episode_files(fx->r, series_id, 1, 7);
	info = info_of(fx, id);
	words = axis_request_info_words(info);
	g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_PARTLY);
	g_assert_cmpuint(info->have, ==, 7);
	g_assert_cmpuint(info->aired, ==, 16);
	g_assert_cmpstr(words, ==, "Partly available, 7 of 16 episodes");
	/* Every aired episode: available. */
	N("DELETE FROM series_file RETURNING file_id");
	axis_test_requests_episode_files(fx->r, series_id, 1, 8);
	axis_test_requests_episode_files(fx->r, series_id, 2, 8);
	full = info_of(fx, id);
	g_assert_cmpint(full->state, ==, AXIS_REQUEST_STATE_AVAILABLE);
	g_assert_cmpint(state_of_title(fx, clockwork), ==, AXIS_REQUEST_STATE_AVAILABLE);
}

/* Scenarios: The folder is gone; Tried again. */
static void
test_could_not_be_added(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(AxisRequestInfo) own = NULL;
	g_autoptr(AxisRequestInfo) done = NULL;
	g_autoptr(GError) error = NULL;
	gboolean added = TRUE;

	/* No defaults and nothing chosen: nothing is approved. */
	g_assert_false(axis_requests_approve(fx->r->requests, id, "root", 0, 0, &added, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PLACEMENT);
	g_clear_error(&error);
	g_assert_false(axis_requests_approve(fx->r->requests, id, "root", fx->r->films_root, 0, &added, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PLACEMENT);
	g_clear_error(&error);
	/* A folder of the other kind is refused, and nothing is approved. */
	g_assert_false(axis_requests_approve(fx->r->requests, id, "root", fx->r->series_root, fx->r->profile_id, &added,
		&error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "not one for movies"));
	g_clear_error(&error);
	assert_decision(fx, id, "waiting");

	/* The folder chosen no longer exists at that moment. */
	g_assert_true(axis_requests_approve(fx->r->requests, id, "root", 987654, fx->r->profile_id, &added, &error));
	g_assert_no_error(error);
	g_assert_false(added);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM job"), ==, 0);
	info = info_of(fx, id);
	g_assert_cmpint(info->decision, ==, AXIS_REQUEST_APPROVED);
	g_assert_cmpint(info->state, ==, AXIS_REQUEST_STATE_FAILED);
	g_assert_cmpint(info->cause, ==, AXIS_REQUEST_CAUSE_NOT_ADDED);
	g_assert_nonnull(info->add_error);
	g_assert_nonnull(strstr(info->add_error, "library folder"));
	/* The person who asked sees that it could not be added. */
	g_assert_true(axis_requests_get(fx->r->requests, id, fx->r->robin, FALSE, &own, &error));
	g_assert_cmpstr(axis_request_cause_words(own->cause), ==, "The title could not be added.");

	/* Another folder, and again. */
	g_assert_true(axis_requests_try_again(fx->r->requests, id, "root", fx->r->films_root, 0, &added, &error));
	g_assert_no_error(error);
	g_assert_true(added);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT " AND root_id = %" G_GINT64_FORMAT,
		orchard, fx->r->films_root), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 1);
	done = info_of(fx, id);
	g_assert_null(done->add_error);
	g_assert_cmpint(done->state, ==, AXIS_REQUEST_STATE_APPROVED);
	/* Not again once its title is added and in Axis: nothing is changed. */
	g_assert_false(axis_requests_try_again(fx->r->requests, id, "root", fx->r->films_root, 0, &added, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
	g_assert_nonnull(strstr(error->message, "still in Axis"));
	g_clear_error(&error);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 1);
	/* Only an approved request can be tried again. */
	{
		g_autofree gchar *waiting = must_ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_LEDGER));

		g_assert_false(axis_requests_try_again(fx->r->requests, waiting, "root", fx->r->films_root, 0, &added, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_DECIDED);
	}
}

/* A daemon that stopped between a decision and the placing of its title
 * leaves an approved request with no title and no reason: it reads as not
 * added once the placing has had its time, and "Try again" completes it. */
static void
test_stopped_before_placing(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = must_ask_movie(fx, fx->r->robin, orchard);
	g_autoptr(AxisRequestInfo) fresh = NULL;
	g_autoptr(AxisRequestInfo) stuck = NULL;
	g_autoptr(AxisRequestInfo) done = NULL;
	g_autoptr(GError) error = NULL;
	gboolean added = FALSE;

	axis_test_requests_set_defaults(fx->r);
	/* The decision as the approval's transaction commits it, and no placing. */
	N("UPDATE request SET decision = 'approved', approved_how = 'admin', decided_by = 'root',"
		" decided_at = 'epoch'::timestamptz + %" G_GINT64_FORMAT " * interval '1 microsecond', root_id = %"
		G_GINT64_FORMAT ", quality_profile_id = %" G_GINT64_FORMAT " WHERE public_id = '%s' RETURNING id",
		AXIS_TEST_REQUESTS_NOW_US, fx->r->films_root, fx->r->profile_id, id);
	fresh = info_of(fx, id);
	g_assert_cmpint(fresh->state, ==, AXIS_REQUEST_STATE_APPROVED);
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 3 * G_TIME_SPAN_MINUTE);
	stuck = info_of(fx, id);
	g_assert_cmpint(stuck->state, ==, AXIS_REQUEST_STATE_FAILED);
	g_assert_cmpint(stuck->cause, ==, AXIS_REQUEST_CAUSE_NOT_ADDED);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	g_assert_true(axis_requests_try_again(fx->r->requests, id, "root", 0, 0, &added, &error));
	g_assert_no_error(error);
	g_assert_true(added);
	done = info_of(fx, id);
	g_assert_cmpint(done->state, ==, AXIS_REQUEST_STATE_APPROVED);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, orchard), ==, 1);
}

/* ── Lists, who may read, accounts ──────────────────────────────── */

/* Scenarios: My requests; The waiting list (its order); Who asked and a
 * reason, as the model gives them to a viewer. */
static void
test_lists(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	g_autofree gchar *r1 = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *r2 = NULL;
	g_autofree gchar *s1 = NULL;
	g_autofree gchar *s2 = NULL;
	g_autoptr(GPtrArray) mine = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GHashTable) keep = NULL;
	g_autoptr(GError) error = NULL;
	const AxisRequestTitleState *state;
	AxisRequestTitleView view;
	AxisRequestMade made;
	guint n_waiting = 0;
	guint i;

	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + G_TIME_SPAN_MINUTE);
	r2 = must_ask_movie(fx, fx->r->robin, ledger);
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 2 * G_TIME_SPAN_MINUTE);
	s1 = must_ask_movie(fx, fx->r->sam, kiln);
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 3 * G_TIME_SPAN_MINUTE);
	s2 = must_ask_movie(fx, fx->r->sam, bells);
	/* robin follows one of sam's. */
	g_assert_true(ask_movie(fx, fx->r->robin, kiln, &made, &error));
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
	axis_request_made_clear(&made);

	/* Those three and none of sam's others, the newest first. */
	g_assert_true(axis_requests_list_mine(fx->r->requests, fx->r->robin, 50, &mine, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(mine->len, ==, 3);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(mine, 0))->public_id, ==, s1);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(mine, 1))->public_id, ==, r2);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(mine, 2))->public_id, ==, r1);
	for (i = 0; i < mine->len; i++)
		g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(mine, i))->public_id, !=, s2);

	/* Every request: the waiting ones first, the oldest at the top. */
	g_assert_true(axis_requests_decline(fx->r->requests, r2, "root", "Not for this library", FALSE, &error));
	g_assert_true(axis_requests_list_all(fx->r->requests, 50, &all, &n_waiting, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(n_waiting, ==, 3);
	g_assert_cmpuint(all->len, ==, 4);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(all, 0))->public_id, ==, r1);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(all, 1))->public_id, ==, s1);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(all, 2))->public_id, ==, s2);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(all, 3))->public_id, ==, r2);
	g_assert_cmpuint(((AxisRequestInfo *)g_ptr_array_index(all, 1))->n_followers, ==, 1);
	g_assert_cmpuint(((AxisRequestInfo *)g_ptr_array_index(all, 0))->would_look_for, ==, 1);
	g_assert_true(((AxisRequestInfo *)g_ptr_array_index(all, 0))->look_known);

	/* Who asked: to sam only while the setting allows; to an administrator
	 * and to robin always. */
	state = title_state(fx, orchard, &keep);
	g_assert_cmpstr(state->asked_by, ==, "robin");
	axis_request_title_view(state, fx->r->sam, FALSE, TRUE, &view);
	g_assert_cmpstr(view.asked_by, ==, "robin");
	g_assert_false(view.mine);
	axis_request_title_view(state, fx->r->sam, FALSE, FALSE, &view);
	g_assert_null(view.asked_by);
	axis_request_title_view(state, fx->r->admin, TRUE, FALSE, &view);
	g_assert_cmpstr(view.asked_by, ==, "robin");
	axis_request_title_view(state, fx->r->robin, FALSE, FALSE, &view);
	g_assert_cmpstr(view.asked_by, ==, "robin");
	g_assert_true(view.mine);
	g_clear_pointer(&keep, g_hash_table_unref);

	/* A reason: to who asked, to a follower and to an administrator. */
	g_assert_true(axis_requests_decline(fx->r->requests, s1, "root", "Too long for the disks", FALSE, &error));
	state = title_state(fx, kiln, &keep);
	g_assert_cmpint(state->state, ==, AXIS_REQUEST_STATE_DECLINED);
	axis_request_title_view(state, fx->r->sam, FALSE, TRUE, &view);
	g_assert_cmpstr(view.decline_reason, ==, "Too long for the disks");
	axis_request_title_view(state, fx->r->robin, FALSE, TRUE, &view);
	g_assert_cmpstr(view.decline_reason, ==, "Too long for the disks");
	g_assert_true(view.follows);
	axis_request_title_view(state, fx->r->admin, TRUE, TRUE, &view);
	g_assert_cmpstr(view.decline_reason, ==, "Too long for the disks");
	{
		gint64 kim = axis_test_requests_person(fx->r, "kim", FALSE);

		axis_request_title_view(state, kim, FALSE, TRUE, &view);
		g_assert_null(view.decline_reason);
		g_assert_cmpstr(view.asked_by, ==, "sam");
		axis_request_title_view(state, kim, FALSE, FALSE, &view);
		g_assert_null(view.decline_reason);
		g_assert_null(view.asked_by);
	}
}

/* Who asked, hidden, for a series: a person with a request for another
 * season of it is not the person whose name the title's state carries. */
static void
test_who_asked_series(Fixture *fx, gconstpointer data)
{
	static const gint one[] = { 1 };
	static const gint two[] = { 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	g_autoptr(GHashTable) keep = NULL;
	g_autoptr(GError) error = NULL;
	const AxisRequestTitleState *state;
	AxisRequestTitleView view;
	AxisRequestMade made;

	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, one, 1, &made, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);
	g_assert_true(ask_series(fx, fx->r->sam, harbour, AXIS_REQUEST_SEASONS_PICKED, two, 1, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);
	state = title_state(fx, harbour, &keep);
	g_assert_cmpstr(state->asked_by, ==, "robin");
	g_assert_cmpint(state->asked_by_id, ==, fx->r->robin);
	/* Off: sam made a request of the title, and is not told robin's name. */
	axis_request_title_view(state, fx->r->sam, FALSE, FALSE, &view);
	g_assert_true(view.mine);
	g_assert_null(view.asked_by);
	axis_request_title_view(state, fx->r->robin, FALSE, FALSE, &view);
	g_assert_cmpstr(view.asked_by, ==, "robin");
	axis_request_title_view(state, fx->r->admin, TRUE, FALSE, &view);
	g_assert_cmpstr(view.asked_by, ==, "robin");
	/* On: everyone who can ask is told it. */
	axis_request_title_view(state, fx->r->sam, FALSE, TRUE, &view);
	g_assert_cmpstr(view.asked_by, ==, "robin");
}

/* Scenario: A disabled requester (an ordinary user in this slice); and an
 * account that is deleted. */
static void
test_account_gone(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	g_autofree gchar *waiting = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *approved = must_ask_movie(fx, fx->r->robin, ledger);
	g_autofree gchar *followed = must_ask_movie(fx, fx->r->sam, kiln);
	g_autofree gchar *sams = NULL;
	g_autoptr(AxisUser) disabled = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	axis_test_requests_set_defaults(fx->r);
	approve(fx, approved);
	g_assert_true(ask_movie(fx, fx->r->robin, kiln, &made, &error));
	axis_request_made_clear(&made);

	disabled = axis_user_store_set_disabled(fx->r->t->pool, fx->r->robin, TRUE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(disabled);
	g_assert_true(axis_requests_account_gone(fx->r->requests, fx->r->robin, &error));
	g_assert_no_error(error);
	assert_decision(fx, waiting, "withdrawn");
	assert_decision(fx, approved, "approved");
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 1);
	/* Its part in what it follows ended, and sam's request stands. */
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);
	assert_decision(fx, followed, "waiting");
	/* A disabled account asks for nothing. */
	g_assert_false(ask_movie(fx, fx->r->robin, film(fx, AXIS_TEST_REQUESTS_BELLS), &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	g_clear_error(&error);
	axis_request_made_clear(&made);

	/* Deleted: the waiting request names no one, and is withdrawn. */
	sams = must_ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_COMPASS));
	g_assert_true(axis_user_store_delete(fx->r->t->pool, fx->r->sam, &error));
	g_assert_no_error(error);
	g_assert_true(axis_requests_account_gone(fx->r->requests, fx->r->sam, &error));
	g_assert_no_error(error);
	assert_decision(fx, sams, "withdrawn");
	assert_decision(fx, followed, "withdrawn");
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE decision = 'waiting'"), ==, 0);
	/* Who asked is still said. */
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE requested_by IS NULL AND requested_name = 'sam'"), ==, 2);
}

/* As the metadata store merges two records of a title: every identifier of
 * the record that goes moves to the one kept, then the record is deleted. */
static void
merge_titles(
	Fixture     *fx,
	gint64       going,
	gint64       kept,
	const gchar *kind
){
	N("INSERT INTO meta_external_id (title_id, kind, scheme, value) VALUES (%" G_GINT64_FORMAT ", '%s', 'madeup',"
		" 'merge%" G_GINT64_FORMAT "') ON CONFLICT DO NOTHING RETURNING title_id", going, kind, going);
	N("UPDATE meta_external_id SET title_id = %" G_GINT64_FORMAT " WHERE title_id = %" G_GINT64_FORMAT
		" RETURNING title_id", kept, going);
	N("DELETE FROM meta_title WHERE id = %" G_GINT64_FORMAT " RETURNING id", going);
	g_assert_cmpint(N("SELECT count(*) FROM meta_title WHERE id = %" G_GINT64_FORMAT, going), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, going), ==, 0);
}

/* Approved, as the approval's transaction leaves a request, and not placed. */
static void
mark_approved(
	Fixture     *fx,
	const gchar *public_id
){
	N("UPDATE request SET decision = 'approved', approved_how = 'admin', decided_by = 'root', decided_at = now()"
		" WHERE public_id = '%s' RETURNING id", public_id);
}

static gchar *
followers_of(
	Fixture     *fx,
	const gchar *public_id
){
	return Q("SELECT coalesce(string_agg(u.username, ',' ORDER BY u.username), '') FROM request_follower f"
		" JOIN request r ON r.id = f.request_id JOIN app_user u ON u.id = f.user_id WHERE r.public_id = '%s'",
		public_id);
}

/* Two records of a title merge: the requests of the one that goes follow.
 * Of two waiting requests for the movie, the kept record's stays, and the
 * other's maker and followers follow it. */
static void
test_title_merge(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kim = axis_test_requests_person(fx->r, "kim", FALSE);
	g_autofree gchar *going = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *kept = must_ask_movie(fx, fx->r->sam, ledger);
	g_autofree gchar *followers = NULL;
	g_autofree gchar *words = NULL;
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(GPtrArray) mine = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	g_assert_true(ask_movie(fx, kim, orchard, &made, &error));
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
	axis_request_made_clear(&made);
	g_assert_true(axis_requests_set_title_closed(fx->r->requests, orchard, TRUE, "root", &error));
	g_assert_no_error(error);
	merge_titles(fx, orchard, ledger, "film");
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 2);
	/* One open request for the title: the one of the record that went was
	 * replaced, and its people follow the one that stayed. */
	assert_decision(fx, going, "replaced");
	assert_decision(fx, kept, "waiting");
	followers = followers_of(fx, kept);
	g_assert_cmpstr(followers, ==, "kim,robin");
	g_assert_true(axis_requests_list_mine(fx->r->requests, fx->r->robin, 50, &mine, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(mine->len, ==, 1);
	g_assert_cmpstr(((AxisRequestInfo *)g_ptr_array_index(mine, 0))->public_id, ==, kept);
	g_assert_true(((AxisRequestInfo *)g_ptr_array_index(mine, 0))->follows);
	/* An administrator reads what became of it, not "declined". */
	info = info_of(fx, going);
	words = axis_request_info_words(info);
	g_assert_cmpstr(words, ==, "Joined to another request for the same title");
	g_assert_cmpint(N("SELECT count(*) FROM request_closed_title WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 1);
}

/* A merge keeps the approved request over the waiting one, whichever record
 * it is on. */
static void
test_merge_approved_wins(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	g_autofree gchar *going_approved = must_ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *kept_waiting = must_ask_movie(fx, fx->r->sam, ledger);
	g_autofree gchar *going_waiting = must_ask_movie(fx, fx->r->robin, kiln);
	g_autofree gchar *kept_approved = must_ask_movie(fx, fx->r->sam, bells);
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	/* On the record that goes. */
	mark_approved(fx, going_approved);
	merge_titles(fx, orchard, ledger, "film");
	assert_decision(fx, going_approved, "approved");
	assert_decision(fx, kept_waiting, "replaced");
	g_assert_cmpint(N("SELECT title_id FROM request WHERE public_id = '%s'", going_approved), ==, ledger);
	first = followers_of(fx, going_approved);
	g_assert_cmpstr(first, ==, "sam");
	/* On the record kept. */
	mark_approved(fx, kept_approved);
	merge_titles(fx, kiln, bells, "film");
	assert_decision(fx, kept_approved, "approved");
	assert_decision(fx, going_waiting, "replaced");
	second = followers_of(fx, kept_approved);
	g_assert_cmpstr(second, ==, "robin");
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE decision IN ('waiting', 'approved')"), ==, 2);
}

/* A merge of two records of a series: each season two requests share goes
 * to one of them (the approved one, else the kept record's); a request keeps
 * the seasons it does not share and stays open with them, and its people
 * follow the requests that took the others. */
static void
test_merge_seasons(Fixture *fx, gconstpointer data)
{
	static const gint specials[] = { 0 };
	static const gint three[] = { 1, 2, 3 };
	static const gint one[] = { 1 };
	static const gint two[] = { 2 };
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 clockwork = series(fx, AXIS_TEST_REQUESTS_CLOCKWORK);
	gint64 kim = axis_test_requests_person(fx->r, "kim", FALSE);
	gint64 jo = axis_test_requests_person(fx->r, "jo", FALSE);
	g_autofree gchar *robins = NULL;
	g_autofree gchar *jos = NULL;
	g_autofree gchar *kims = NULL;
	g_autofree gchar *sams = NULL;
	g_autofree gchar *left = NULL;
	g_autofree gchar *kept_two = NULL;
	g_autofree gchar *open = NULL;
	g_autofree gchar *of_jo = NULL;
	g_autofree gchar *of_kim = NULL;
	g_autofree gchar *of_sam = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	/* The record that goes: robin waits for 1, 2 and 3; jo's specials are
	 * approved. */
	g_assert_true(ask_series(fx, fx->r->robin, harbour, AXIS_REQUEST_SEASONS_PICKED, three, 3, &made, &error));
	g_assert_no_error(error);
	robins = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	g_assert_true(ask_series(fx, jo, harbour, AXIS_REQUEST_SEASONS_PICKED, specials, 1, &made, &error));
	g_assert_no_error(error);
	jos = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	mark_approved(fx, jos);
	/* The record kept: kim's season 1 is approved; sam waits for 2, and for
	 * the specials (as the record held them once). */
	g_assert_true(ask_series(fx, kim, clockwork, AXIS_REQUEST_SEASONS_PICKED, one, 1, &made, &error));
	g_assert_no_error(error);
	kims = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	mark_approved(fx, kims);
	g_assert_true(ask_series(fx, fx->r->sam, clockwork, AXIS_REQUEST_SEASONS_PICKED, two, 1, &made, &error));
	g_assert_no_error(error);
	sams = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	N("INSERT INTO request_season (request_id, title_id, season) SELECT id, title_id, 0 FROM request"
		" WHERE public_id = '%s' RETURNING season", sams);

	merge_titles(fx, harbour, clockwork, "series");
	/* No request was left with nothing: all four stay open. */
	assert_decision(fx, robins, "waiting");
	assert_decision(fx, jos, "approved");
	assert_decision(fx, kims, "approved");
	assert_decision(fx, sams, "waiting");
	/* robin keeps season 3 alone; jo's approval took the specials from sam;
	 * kim's approval and sam's (the kept record's) took 1 and 2 from robin. */
	left = seasons_of(fx, robins);
	g_assert_cmpstr(left, ==, "3");
	kept_two = seasons_of(fx, sams);
	g_assert_cmpstr(kept_two, ==, "2");
	g_assert_cmpint(N("SELECT title_id FROM request WHERE public_id = '%s'", robins), ==, clockwork);
	open = Q("SELECT string_agg(season::text, ',' ORDER BY season) FROM request_season WHERE title_id = %"
		G_GINT64_FORMAT " AND open", clockwork);
	g_assert_cmpstr(open, ==, "0,1,2,3");
	of_jo = followers_of(fx, jos);
	of_kim = followers_of(fx, kims);
	of_sam = followers_of(fx, sams);
	g_assert_cmpstr(of_jo, ==, "sam");
	g_assert_cmpstr(of_kim, ==, "robin");
	g_assert_cmpstr(of_sam, ==, "robin");
}

/* ── At scale ───────────────────────────────────────────────────── */

typedef struct
{
	guint n;
} Counted;

static void
count_statement(
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	gpointer            data
){
	Counted *counted = data;

	g_atomic_int_inc((gint *)&counted->n);
}

/*
 * The state of 20 titles and of 50 requests is read in a bounded number of
 * statements, on a library of 5,000 movies and 1,500 series: the same number
 * as on a library of a handful.
 */
static void
test_one_query_a_page(Fixture *fx, gconstpointer data)
{
	g_autoptr(GArray) titles = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GHashTable) states = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GPtrArray) mine = NULL;
	g_autoptr(GError) error = NULL;
	Counted counted = { 0 };
	guint for_titles;
	guint for_all;
	guint i;

	N("WITH t AS (INSERT INTO meta_title (kind, title, year, status, release_date, provider, provider_id)"
		" SELECT 'film', 'Generated Film ' || i, 1950 + i %% 70, 'released', DATE '2000-01-01' + i %% 7000, 'fixture',"
		" 'genf' || i FROM generate_series(1, 5000) i RETURNING id)"
		" INSERT INTO movie (title_id, quality_profile_id, root_id) SELECT t.id, %" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT " FROM t RETURNING 1", fx->r->profile_id, fx->r->films_root);
	N("WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 1990 + i %% 30, 'ended', 'fixture', 'gens' || i"
		" FROM generate_series(1, 1500) i RETURNING id),"
		" s AS (INSERT INTO series (title_id, quality_profile_id, root_id) SELECT t.id, %" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT " FROM t RETURNING id),"
		" ss AS (INSERT INTO series_season (series_id, number, monitored) SELECT s.id, n, true FROM s,"
		"  generate_series(1, 2) n RETURNING series_id, number)"
		" INSERT INTO series_episode (series_id, season_number, number, monitored, air_date)"
		" SELECT ss.series_id, ss.number, e, true, DATE '2015-01-01' + e FROM ss, generate_series(1, 5) e RETURNING 1",
		fx->r->profile_id, fx->r->series_root);
	/* 50 requests, of movies and of series, by the two people. */
	N("INSERT INTO request (public_id, title_id, kind, requested_by, requested_name, created_at, counted_at)"
		" SELECT 'gen' || lpad(row_number() OVER ()::text, 19, '0'), t.id, CASE t.kind WHEN 'film' THEN 'movie'"
		" ELSE 'series' END, %" G_GINT64_FORMAT ", 'robin', now(), now() FROM (SELECT id, kind FROM meta_title"
		" WHERE provider_id IN (SELECT 'genf' || i FROM generate_series(1, 25) i UNION ALL SELECT 'gens' || i"
		" FROM generate_series(1, 25) i)) t RETURNING 1", fx->r->robin);
	N("INSERT INTO request_season (request_id, title_id, season) SELECT id, title_id, 1 FROM request"
		" WHERE kind = 'series' RETURNING 1");
	axis_test_db_exec(fx->r->t->db, "ANALYZE");
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 50);

	/* 20 titles: ten films and ten series. */
	for (i = 1; i <= 10; i++) {
		gint64 a = N("SELECT id FROM meta_title WHERE provider_id = 'genf%u'", i * 3);
		gint64 b = N("SELECT id FROM meta_title WHERE provider_id = 'gens%u'", i * 3);

		g_array_append_val(titles, a);
		g_array_append_val(titles, b);
	}
	axis_db_pool_set_statement_observer(fx->r->t->pool, count_statement, &counted);
	g_assert_true(axis_requests_title_states(fx->r->requests, titles, &states, &error));
	g_assert_no_error(error);
	for_titles = counted.n;
	g_assert_cmpuint(g_hash_table_size(states), ==, 20);
	g_assert_cmpuint(for_titles, <=, 10);

	counted.n = 0;
	g_assert_true(axis_requests_list_all(fx->r->requests, 50, &all, NULL, &error));
	g_assert_no_error(error);
	for_all = counted.n;
	g_assert_cmpuint(all->len, ==, 50);
	g_assert_cmpuint(for_all, <=, 10);
	counted.n = 0;
	g_assert_true(axis_requests_list_mine(fx->r->requests, fx->r->robin, 50, &mine, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(mine->len, ==, 50);
	g_assert_cmpuint(counted.n, <=, 10);
	axis_db_pool_set_statement_observer(fx->r->t->pool, NULL, NULL);
	g_test_message("statements: %u for 20 titles, %u for 50 requests", for_titles, for_all);
	/* A managed series' request says what an approval would look for. */
	for (i = 0; i < all->len; i++) {
		const AxisRequestInfo *info = g_ptr_array_index(all, i);

		g_assert_true(info->look_known);
		g_assert_cmpuint(info->would_look_for, ==, info->kind == AXIS_REQUEST_SERIES ? 5 : 1);
	}
}

/*
 * A title its provider marks as adult (add-request-portal, before slice P2):
 * the mark is stored with the record however the title came to be stored (here
 * a lookup by its identifier, which no search's filter sees), and a person who
 * is not an administrator cannot ask for it by its number: the answer is that
 * of a title that does not exist.
 */
static void
test_adult_title(Fixture *fx, gconstpointer data)
{
	gint64 annex = film(fx, AXIS_TEST_REQUESTS_ANNEX);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autoptr(GError) marked = NULL;
	g_autoptr(GError) unknown = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;

	g_assert_cmpint(N("SELECT adult::integer FROM meta_title WHERE id = %" G_GINT64_FORMAT, annex), ==, 1);
	g_assert_cmpint(N("SELECT adult::integer FROM meta_title WHERE id = %" G_GINT64_FORMAT, orchard), ==, 0);

	g_assert_false(ask_movie(fx, fx->r->robin, annex, &made, &marked));
	g_assert_error(marked, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	axis_request_made_clear(&made);
	g_assert_false(ask_movie(fx, fx->r->robin, 987654, &made, &unknown));
	g_assert_error(unknown, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	axis_request_made_clear(&made);
	g_assert_cmpstr(marked->message, ==, unknown->message);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);

	/* Nor does the person follow an administrator's request for it. */
	g_assert_true(ask_movie(fx, fx->r->admin, annex, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);
	g_clear_error(&marked);
	g_assert_false(ask_movie(fx, fx->r->sam, annex, &made, &marked));
	g_assert_error(marked, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);

	/* An answer that does not carry the mark does not clear it. */
	{
		g_autoptr(GBytes) body = g_bytes_new_static("{\"id\":\"" AXIS_TEST_REQUESTS_ANNEX "\",\"kind\":\"film\","
			"\"title\":\"Velvet Annex\",\"year\":2014}", strlen("{\"id\":\"" AXIS_TEST_REQUESTS_ANNEX "\",\"kind\":\"film\","
			"\"title\":\"Velvet Annex\",\"year\":2014}"));

		axis_test_plex_route_full(fx->r->fa->api, "/title/film/" AXIS_TEST_REQUESTS_ANNEX, 200, "application/json", body,
			NULL);
		N("UPDATE meta_title SET detail_fetched_at = now() - interval '400 days', fetched_at = now() - interval '400 days'"
			" WHERE id = %" G_GINT64_FORMAT " RETURNING id", annex);
		g_assert_cmpint(film(fx, AXIS_TEST_REQUESTS_ANNEX), ==, annex);
		g_assert_cmpint(N("SELECT (adult AND detail_fetched_at > now() - interval '1 day')::integer FROM meta_title"
			" WHERE id = %" G_GINT64_FORMAT, annex), ==, 1);
	}
}

/*
 * A title whose adult mark is not known (review of P2, S2): stored before
 * migration 0084, or from no provider's answer.  A person who is not an
 * administrator is refused it as a title that does not exist, whatever kind
 * it is asked for as and whether it is closed (M2), and follows no request
 * for it; an administrator asks for it; and once a provider's answer about
 * it is stored, it can be asked for.
 */
static void
test_adult_unknown(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	g_autoptr(GError) unknown = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	guint i;

	g_assert_false(ask_movie(fx, fx->r->robin, 987654, &made, &unknown));
	g_assert_error(unknown, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	axis_request_made_clear(&made);
	N("UPDATE meta_title SET adult = NULL WHERE id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ") RETURNING id", orchard,
		ledger);

	/* As a movie, as a series, and while it is closed. */
	for (i = 0; i < 3; i++) {
		if (i == 2)
			N("INSERT INTO request_closed_title (title_id, kind, closed_by) VALUES (%" G_GINT64_FORMAT ", 'movie',"
				" 'root') RETURNING title_id", orchard);
		if (i == 1)
			g_assert_false(ask_series(fx, fx->r->robin, orchard, AXIS_REQUEST_SEASONS_ALL, NULL, 0, &made, &error));
		else
			g_assert_false(ask_movie(fx, fx->r->robin, orchard, &made, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
		g_assert_cmpstr(error->message, ==, unknown->message);
		g_clear_error(&error);
		axis_request_made_clear(&made);
	}
	N("DELETE FROM request_closed_title WHERE title_id = %" G_GINT64_FORMAT " RETURNING title_id", orchard);

	/* An administrator asks; the ordinary user follows nothing of it. */
	g_assert_true(ask_movie(fx, fx->r->admin, ledger, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);
	g_assert_false(ask_movie(fx, fx->r->sam, ledger, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, orchard), ==, 0);

	/* A provider's answer about it: the mark is known, and it can be asked
	 * for. */
	N("UPDATE meta_title SET detail_fetched_at = now() - interval '400 days', fetched_at = now() - interval '400 days'"
		" WHERE id = %" G_GINT64_FORMAT " RETURNING id", orchard);
	g_assert_cmpint(film(fx, AXIS_TEST_REQUESTS_ORCHARD), ==, orchard);
	g_assert_cmpint(N("SELECT count(*) FROM meta_title WHERE id = %" G_GINT64_FORMAT " AND adult = false", orchard), ==,
		1);
	g_assert_true(ask_movie(fx, fx->r->robin, orchard, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);
}

/*
 * Scenario: The portal's daily ceiling (the engine's half; the API's answer is
 * in tests/test-portal-api.c).  What all requests made through the portal API
 * count on one calendar day, whoever made them and whatever became of them: a
 * movie one, a season one each; and of it, what one person's count, withdrawn
 * ones included, at most a third (review of P2, S4).  A request made in the
 * engine's interface is not counted and not refused.
 */
static void
test_portal_ceiling(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	gint64 compass = film(fx, AXIS_TEST_REQUESTS_COMPASS);
	gint64 almanac = film(fx, AXIS_TEST_REQUESTS_ALMANAC);
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 tess = axis_test_requests_person(fx->r, "tess", FALSE);
	gint64 vic = axis_test_requests_person(fx->r, "vic", FALSE);
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	AxisRequestAsk ask;
	gboolean handed = FALSE;

	/* Six in the day, two for each person. */
	g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_PORTAL_DAY, "6", &error));
	g_assert_no_error(error);
	g_assert_cmpint(AXIS_REQUESTS_PORTAL_SHARE_OF(6), ==, 2);
	g_assert_cmpint(AXIS_REQUESTS_PORTAL_SHARE_OF(2), ==, 1);
	memset(&ask, 0, sizeof(ask));
	ask.portal = TRUE;
	ask.kind = AXIS_REQUEST_MOVIE;

	ask.user_id = fx->r->robin;
	ask.title_id = orchard;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'portal'"), ==, 1);
	/* Withdrawn: the person's quota has it back, the portal's day and the
	 * person's share of it do not. */
	g_assert_true(axis_requests_withdraw(fx->r->requests, made.public_id, fx->r->robin, &handed, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);

	/* Three seasons are more than sam's share; two are made. */
	ask.user_id = fx->r->sam;
	ask.title_id = harbour;
	ask.kind = AXIS_REQUEST_SERIES;
	ask.seasons = AXIS_REQUEST_SEASONS_ALL;
	g_assert_false(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PORTAL_SHARE);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE kind = 'series'"), ==, 0);
	{
		static const gint two[] = { 1, 2 };

		ask.seasons = AXIS_REQUEST_SEASONS_PICKED;
		ask.picked = two;
		ask.n_picked = 2;
		g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
		g_assert_no_error(error);
		axis_request_made_clear(&made);
	}
	ask.picked = NULL;
	ask.n_picked = 0;
	ask.kind = AXIS_REQUEST_MOVIE;

	/* robin's second place: made; a third is refused, though the day has
	 * room and robin's quota too. */
	ask.user_id = fx->r->robin;
	ask.title_id = ledger;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);
	ask.title_id = kiln;
	g_assert_cmpuint(quota_of(fx, fx->r->robin).movies.left, >, 0);
	g_assert_false(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PORTAL_SHARE);
	g_assert_nonnull(strstr(error->message, "withdrawn ones included"));
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, kiln), ==, 0);
	/* tess has a share of her own: one place of the day is left after it. */
	ask.user_id = tess;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);

	/* The day is full: another person, with quota and share left, is
	 * refused (with the day's refusal, which comes first). */
	ask.user_id = vic;
	ask.title_id = bells;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'portal'"), ==, 5);
	ask.title_id = compass;
	g_assert_false(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_PORTAL_FULL);
	g_clear_error(&error);
	axis_request_made_clear(&made);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, compass), ==, 0);

	/* Following what is asked for already makes no row and is not refused. */
	ask.user_id = fx->r->robin;
	ask.title_id = harbour;
	ask.kind = AXIS_REQUEST_SERIES;
	ask.seasons = AXIS_REQUEST_SEASONS_FIRST;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_FOLLOWED);
	axis_request_made_clear(&made);

	/* In the engine's interface the same person asks as before. */
	ask.portal = FALSE;
	ask.title_id = almanac;
	ask.kind = AXIS_REQUEST_MOVIE;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'web'"), ==, 1);
	axis_request_made_clear(&made);

	/* The next day the portal's count starts again, and each person's share
	 * of it. */
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + G_TIME_SPAN_DAY);
	ask.portal = TRUE;
	ask.title_id = compass;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	axis_request_made_clear(&made);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/requests/quota/window-bounds", test_window_bounds);
	AXIS_TEST_ADD_DB_CASE("/requests/model/adult-title", Fixture, NULL, test_adult_title);
	AXIS_TEST_ADD_DB_CASE("/requests/model/adult-mark-not-known", Fixture, NULL, test_adult_unknown);
	AXIS_TEST_ADD_DB_CASE("/requests/quota/portal-ceiling", Fixture, NULL, test_portal_ceiling);
	AXIS_TEST_ADD_DB_CASE("/requests/model/movie", Fixture, NULL, test_movie);
	AXIS_TEST_ADD_DB_CASE("/requests/model/whole-series-is-a-list", Fixture, NULL, test_seasons);
	AXIS_TEST_ADD_DB_CASE("/requests/model/first-and-latest", Fixture, NULL, test_latest_and_specials);
	AXIS_TEST_ADD_DB_CASE("/requests/model/no-episodes-no-text", Fixture, NULL, test_refused_whole);
	AXIS_TEST_ADD_DB_CASE("/requests/model/module-off", Fixture, NULL, test_module_off);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/waiting-fetches-nothing", Fixture, NULL, test_waiting_fetches_nothing);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/approval-adds-and-searches", Fixture, NULL,
		test_approval_adds_and_searches);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/approval-of-seasons", Fixture, NULL, test_approval_of_seasons);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/widens-never-narrows", Fixture, NULL, test_widens_never_narrows);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/decline-has-reason", Fixture, NULL, test_decline_has_reason);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/two-at-once", Fixture, NULL, test_two_at_once);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/two-placings", Fixture, NULL, test_two_placings);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/folder-gone", Fixture, NULL, test_could_not_be_added);
	AXIS_TEST_ADD_DB_CASE("/requests/decide/stopped-before-placing", Fixture, NULL, test_stopped_before_placing);
	AXIS_TEST_ADD_DB_CASE("/requests/auto/movie", Fixture, NULL, test_auto);
	AXIS_TEST_ADD_DB_CASE("/requests/auto/new-account-switches", Fixture, NULL, test_new_account_switches);
	AXIS_TEST_ADD_DB_CASE("/requests/auto/too-many-seasons", Fixture, NULL, test_auto_seasons);
	AXIS_TEST_ADD_DB_CASE("/requests/auto/defaults-gone", Fixture, NULL, test_auto_defaults_gone);
	AXIS_TEST_ADD_DB_CASE("/requests/quota/limit-reached", Fixture, NULL, test_quota_movies);
	AXIS_TEST_ADD_DB_CASE("/requests/quota/seasons-count-each", Fixture, NULL, test_quota_seasons);
	AXIS_TEST_ADD_DB_CASE("/requests/quota/new-window", Fixture, NULL, test_quota_windows);
	AXIS_TEST_ADD_DB_CASE("/requests/quota/race", Fixture, NULL, test_quota_race);
	AXIS_TEST_ADD_DB_CASE("/requests/quota/daily-ceiling", Fixture, NULL, test_daily_ceiling);
	AXIS_TEST_ADD_DB_CASE("/requests/follow/movie", Fixture, NULL, test_follow_movie);
	AXIS_TEST_ADD_DB_CASE("/requests/follow/asked-twice", Fixture, NULL, test_follow_race);
	AXIS_TEST_ADD_DB_CASE("/requests/follow/seasons-left-out", Fixture, NULL, test_follow_seasons);
	AXIS_TEST_ADD_DB_CASE("/requests/follow/already-in-library", Fixture, NULL, test_follow_engine);
	AXIS_TEST_ADD_DB_CASE("/requests/declined/asked-again", Fixture, NULL, test_declined);
	AXIS_TEST_ADD_DB_CASE("/requests/withdraw/while-waiting", Fixture, NULL, test_withdraw);
	AXIS_TEST_ADD_DB_CASE("/requests/withdraw/not-after-approval", Fixture, NULL, test_withdraw_race);
	AXIS_TEST_ADD_DB_CASE("/requests/withdraw/follower-takes-over", Fixture, NULL, test_follower_takes_over);
	AXIS_TEST_ADD_DB_CASE("/requests/state/movie", Fixture, NULL, test_movie_states);
	AXIS_TEST_ADD_DB_CASE("/requests/state/series-partly", Fixture, NULL, test_series_partly);
	AXIS_TEST_ADD_DB_CASE("/requests/state/series-removed", Fixture, NULL, test_series_removed);
	AXIS_TEST_ADD_DB_CASE("/requests/read/lists", Fixture, NULL, test_lists);
	AXIS_TEST_ADD_DB_CASE("/requests/read/who-asked-series", Fixture, NULL, test_who_asked_series);
	AXIS_TEST_ADD_DB_CASE("/requests/account/disabled", Fixture, NULL, test_account_gone);
	AXIS_TEST_ADD_DB_CASE("/requests/model/title-merge", Fixture, NULL, test_title_merge);
	AXIS_TEST_ADD_DB_CASE("/requests/model/merge-approved-wins", Fixture, NULL, test_merge_approved_wins);
	AXIS_TEST_ADD_DB_CASE("/requests/model/merge-seasons", Fixture, NULL, test_merge_seasons);
	AXIS_TEST_ADD_DB_CASE("/requests/state/one-query-a-page", Fixture, NULL, test_one_query_a_page);

	return g_test_run();
}
