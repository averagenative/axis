/*
 * axis-discover.c - the Discover layer: its settings, the provider it uses,
 * and the job that keeps the lists
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "discover/axis-discover-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"

G_DEFINE_QUARK(axis-discover-error-quark, axis_discover_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo setting_table[] = {
	{ AXIS_DISCOVER_SETTING_PAGES, AXIS_SETTING_INTEGER, "Pages kept of each Discover list",
	  "How many pages of 20 titles of each Discover list Axis keeps. More pages give the filters more to choose "
	  "from, and cost one request to TMDb each time a list is fetched.", 0, "3", 1, AXIS_DISCOVER_MAX_PAGES, 0, 0,
	  NULL },
	{ AXIS_DISCOVER_SETTING_REFRESH_AFTER, AXIS_SETTING_DURATION, "Fetch the lists again after",
	  "How old a Discover list may get before Axis asks TMDb for it again. The Discover pages show the lists Axis "
	  "keeps and never ask TMDb themselves.", 0, "12h", 3600, 7 * 24 * 3600, 0, 0, NULL },
	{ AXIS_DISCOVER_SETTING_RECOMMEND, AXIS_SETTING_INTEGER, "Titles to fetch recommendations for, each hour",
	  "For how many of the titles you have Axis asks TMDb for recommendations each hour; each title's are kept a "
	  "week. 0 turns Recommended for you off.", 0, "20", 0, 200, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_discover_get_setting_table(void)
{
	return setting_table;
}

static gint64
setting_integer(
	AxisDiscover *self,
	const gchar  *key,
	gint64        fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_integer(self->settings, key);
}

static gint64
setting_duration(
	AxisDiscover *self,
	const gchar  *key,
	gint64        fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	return axis_settings_get_duration(self->settings, key);
}

guint
axis_discover_recommend_per_run(AxisDiscover *self)
{
	return (guint)CLAMP(setting_integer(self, AXIS_DISCOVER_SETTING_RECOMMEND, AXIS_DISCOVER_DEFAULT_RECOMMEND), 0,
		200);
}

guint
axis_discover_pages(AxisDiscover *self)
{
	return (guint)CLAMP(setting_integer(self, AXIS_DISCOVER_SETTING_PAGES, AXIS_DISCOVER_DEFAULT_PAGES), 1,
		AXIS_DISCOVER_MAX_PAGES);
}

gint64
axis_discover_refresh_after(AxisDiscover *self)
{
	return CLAMP(setting_duration(self, AXIS_DISCOVER_SETTING_REFRESH_AFTER, AXIS_DISCOVER_DEFAULT_REFRESH_AFTER),
		3600, 7 * 24 * 3600);
}

/* ── The object ─────────────────────────────────────────────────── */

AxisDiscover *
axis_discover_new(
	AxisDbPool   *pool,
	AxisSettings *settings,
	AxisMeta     *meta
){
	AxisDiscover *self;

	g_return_val_if_fail(pool != NULL && meta != NULL, NULL);

	self = g_new0(AxisDiscover, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->meta = axis_meta_ref(meta);
	return self;
}

AxisDiscover *
axis_discover_ref(AxisDiscover *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_discover_unref(AxisDiscover *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	axis_meta_unref(self->meta);
	g_free(self);
}

AxisMeta *
axis_discover_get_meta(AxisDiscover *self)
{
	return self->meta;
}

const AxisMetaProviderInfo *
axis_discover_provider(
	AxisDiscover *self,
	AxisMetaKind  kind,
	gboolean     *ready
){
	const AxisMetaProviderInfo *first = NULL;
	const AxisMetaProviderInfo *found = NULL;
	guint i;

	for (i = 0; found == NULL && i < axis_meta_n_providers(self->meta); i++) {
		AxisMetaProviderStatus status;

		axis_meta_provider_status(self->meta, i, &status);
		if (status.info != NULL && axis_meta_provider_lists(status.info, kind) != 0) {
			if (first == NULL)
				first = status.info;
			if (!status.needs_key || status.key_set)
				found = status.info;
		}
		axis_meta_provider_status_clear(&status);
	}
	if (ready != NULL)
		*ready = found != NULL;
	return found != NULL ? found : first;
}

guint
axis_discover_lists(
	const AxisMetaProviderInfo *provider,
	AxisMetaKind                kind,
	AxisMetaList               *lists
){
	guint offered = axis_meta_provider_lists(provider, kind);
	guint n = 0;
	guint i;

	for (i = 0; i < AXIS_META_N_LISTS; i++) {
		if ((offered & AXIS_META_LIST_BIT(i)) != 0)
			lists[n++] = (AxisMetaList)i;
	}
	return n;
}

const AxisMetaProviderInfo *
axis_discover_like_provider(
	AxisDiscover         *self,
	const AxisMetaTitle  *seed,
	gboolean             *ready,
	const gchar         **ref
){
	const AxisMetaProviderInfo *provider;
	const gchar *id = NULL;

	if (ready != NULL)
		*ready = FALSE;
	if (ref != NULL)
		*ref = NULL;
	if (seed == NULL || seed->id <= 0 || seed->provider == NULL)
		return NULL;
	provider = axis_discover_provider(self, seed->kind, ready);
	/* The provider's own record is asked about by its own id; another
	 * provider's (a series known by TheTVDB) by the id it keeps under this
	 * provider's name, when it keeps one. */
	if (provider != NULL)
		id = strcmp(provider->name, seed->provider) == 0 ? seed->provider_id :
			axis_meta_title_get_id(seed, provider->name);
	if (provider == NULL || id == NULL ||
	    (axis_meta_provider_lists(provider, seed->kind) & AXIS_META_LIST_BIT(AXIS_META_LIST_RECOMMENDED)) == 0) {
		if (ready != NULL)
			*ready = FALSE;
		return NULL;
	}
	if (ref != NULL)
		*ref = id;
	return provider;
}

/* ── Statements' values ─────────────────────────────────────────── */

gchar *
axis_discover_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

gchar *
axis_discover_sql_int64_array(GArray *values)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; values != NULL && i < values->len; i++)
		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(values, gint64, i));
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gchar *
axis_discover_sql_int_array(
	GArray   *values,
	gboolean  with_nulls,
	gint      null_below
){
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; values != NULL && i < values->len; i++) {
		gint value = g_array_index(values, gint, i);

		if (i > 0)
			g_string_append_c(out, ',');
		if (with_nulls && value < null_below)
			g_string_append(out, "NULL");
		else
			g_string_append_printf(out, "%d", value);
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gchar *
axis_discover_sql_text_array(GPtrArray *values)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; values != NULL && i < values->len; i++) {
		const gchar *p;

		g_string_append(out, i > 0 ? ",\"" : "\"");
		for (p = g_ptr_array_index(values, i); *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(out, '\\');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/* ── The job ────────────────────────────────────────────────────── */

/* One title kept for a snapshot. */
typedef struct
{
	gint64  title_id;
	gint    rating;
	gint    votes;
	gchar  *genres;		/* "{28,12}" */
} Kept;

static void
kept_free(Kept *kept)
{
	g_free(kept->genres);
	g_free(kept);
}

/* Whether a failure means the provider is not to be asked again this run. */
static gboolean
provider_gone(const GError *error)
{
	return g_error_matches(error, AXIS_META_ERROR, AXIS_META_ERROR_UNAVAILABLE) ||
		g_error_matches(error, AXIS_META_ERROR, AXIS_META_ERROR_RATE_LIMITED) ||
		g_error_matches(error, AXIS_META_ERROR, AXIS_META_ERROR_NO_KEY) ||
		g_error_matches(error, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED);
}

/* A failure of the store, not of the provider: the run itself fails. */
static gboolean
store_failed(const GError *error)
{
	return error == NULL || error->domain != AXIS_META_ERROR ||
		g_error_matches(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED);
}

/* What the page says of a failed attempt: the layer's words (they never hold
 * the key), cut to the column's 300 characters. */
static gchar *
problem_of(const GError *error)
{
	gchar *text = error != NULL && error->domain == AXIS_META_ERROR ? axis_meta_clean_text(error->message, 300) : NULL;

	return text != NULL ? text : g_strdup("the provider did not answer");
}

static const gchar *
kind_name(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? "series" : "movies";
}

/* The seed's id as a parameter (NULL for a list that is not of a title). */
static gchar *
seed_text(gint64 seed_id)
{
	return seed_id > 0 ? axis_discover_int_text(seed_id) : NULL;
}

#define LIST_KEY "(provider, kind, list, (coalesce(seed_title_id, 0)))"

static gboolean
record_failure(
	AxisDiscover  *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	AxisMetaList   list,
	gint64         seed_id,
	const gchar   *problem,
	GError       **error
){
	g_autofree gchar *seed = seed_text(seed_id);
	const gchar *params[5];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = axis_meta_list_to_string(list);
	params[3] = seed;
	params[4] = problem;
	ok = axis_db_conn_exec(conn, "INSERT INTO discover_list (provider, kind, list, seed_title_id, tried_at, problem)"
		" VALUES ($1, $2, $3, $4::bigint, now(), $5) ON CONFLICT " LIST_KEY
		" DO UPDATE SET tried_at = now(), problem = EXCLUDED.problem", 5, params, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* Replaces the snapshot of the list with @kept, in one transaction. */
static gboolean
store_snapshot(
	AxisDiscover  *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	AxisMetaList   list,
	gint64         seed_id,
	guint          pages,
	GPtrArray     *kept,
	GError       **error
){
	g_autofree gchar *seed = seed_text(seed_id);
	g_autofree gchar *pages_text = axis_discover_int_text(pages);
	g_autofree gchar *list_id = NULL;
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(GArray) titles = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) ratings = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autoptr(GArray) votes = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autoptr(GPtrArray) genres = g_ptr_array_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	const gchar *params[5];
	AxisDbConn *conn;
	gint64 id = 0;
	gboolean ok;
	guint i;

	for (i = 0; i < kept->len; i++) {
		const Kept *k = g_ptr_array_index(kept, i);

		g_array_append_val(titles, k->title_id);
		g_array_append_val(ratings, k->rating);
		g_array_append_val(votes, k->votes);
		g_ptr_array_add(genres, k->genres);
	}
	a = axis_discover_sql_int64_array(titles);
	b = axis_discover_sql_int_array(ratings, TRUE, 0);
	c = axis_discover_sql_int_array(votes, FALSE, 0);
	d = axis_discover_sql_text_array(genres);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = axis_meta_list_to_string(list);
	params[3] = seed;
	params[4] = pages_text;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "INSERT INTO discover_list (provider, kind, list, seed_title_id, pages, fetched_at,"
			" tried_at, problem) VALUES ($1, $2, $3, $4::bigint, $5::integer, now(), now(), NULL) ON CONFLICT " LIST_KEY
			" DO UPDATE SET pages = EXCLUDED.pages, fetched_at = now(), tried_at = now(), problem = NULL RETURNING id",
			5, params, &row, error) &&
		axis_db_result_get_int64(row, 0, 0, &id);
	if (ok) {
		list_id = axis_discover_int_text(id);
		params[0] = list_id;
		ok = axis_db_conn_exec(conn, "DELETE FROM discover_entry WHERE list_id = $1::bigint", 1, params, NULL, error);
	}
	if (ok && kept->len > 0) {
		params[1] = a;
		params[2] = b;
		params[3] = c;
		params[4] = d;
		ok = axis_db_conn_exec(conn, "INSERT INTO discover_entry (list_id, position, title_id, rating, votes, genre_ids)"
			" SELECT $1::bigint, u.n::integer, u.t, u.r, u.v, u.g::integer[]"
			" FROM unnest($2::bigint[], $3::integer[], $4::integer[], $5::text[]) WITH ORDINALITY AS u (t, r, v, g, n)",
			5, params, NULL, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/*
 * Asks for @pages pages of @list (the recommendations of @seed_ref, the
 * provider's id of the stored title @seed_id) and replaces its snapshot when
 * every one arrived; otherwise records why.  *@gone: the provider is not to be
 * asked again this run.  Returns %FALSE for a failure of the store.
 */
static gboolean
refresh_list(
	AxisDiscover               *self,
	const AxisMetaProviderInfo *provider,
	AxisMetaKind                kind,
	AxisMetaList                list,
	gint64                      seed_id,
	const gchar                *seed_ref,
	guint                       pages,
	GCancellable               *cancellable,
	AxisDiscoverReport         *report,
	gboolean                   *gone,
	GError                    **error
){
	g_autoptr(GPtrArray) kept = g_ptr_array_new_with_free_func((GDestroyNotify)kept_free);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint fetched = 0;
	guint page;
	guint i;

	*gone = FALSE;
	for (page = 1; page <= pages; page++) {
		g_autoptr(AxisMetaListPage) answer = NULL;
		g_autoptr(GError) local = NULL;

		report->pages++;
		if (!axis_meta_fetch_list(self->meta, provider->name, kind, list, seed_ref, page, cancellable, &answer,
		        &local)) {
			g_autofree gchar *problem = NULL;

			if (store_failed(local)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			*gone = provider_gone(local);
			/* A daemon stopping is no problem of the list's. */
			if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED))
				return TRUE;
			report->failed++;
			/* The layer's words for a 404 are about a title. */
			if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND))
				problem = list == AXIS_META_LIST_RECOMMENDED ?
					g_strdup_printf("%s has no recommendations for this title", provider->label) :
					g_strdup_printf("%s did not have page %u of this list", provider->label, page);
			else
				problem = problem_of(local);
			g_info("discover: the list %s of %s is left as it was: %s", axis_meta_list_to_string(list), kind_name(kind),
				problem);
			return record_failure(self, provider->name, kind, list, seed_id, problem, error);
		}
		fetched = page;
		for (i = 0; i < answer->entries->len; i++) {
			const AxisMetaListEntry *entry = g_ptr_array_index(answer->entries, i);
			gint64 *key;
			Kept *k;

			/* A title on two pages (the list moved while it was read) is
			 * kept at its first place. */
			if (entry->title->id <= 0 || g_hash_table_contains(seen, &entry->title->id))
				continue;
			key = g_new(gint64, 1);
			*key = entry->title->id;
			g_hash_table_add(seen, key);
			k = g_new0(Kept, 1);
			k->title_id = entry->title->id;
			k->rating = entry->rating;
			k->votes = entry->votes;
			k->genres = axis_discover_sql_int_array(entry->genre_ids, FALSE, 0);
			g_ptr_array_add(kept, k);
		}
		if (answer->entries->len == 0 || (answer->total_pages > 0 && page >= answer->total_pages))
			break;
	}
	if (!store_snapshot(self, provider->name, kind, list, seed_id, fetched, kept, error))
		return FALSE;
	if (list == AXIS_META_LIST_RECOMMENDED)
		report->recommended++;
	else
		report->lists++;
	return TRUE;
}

static gboolean
genres_fresh(
	AxisDiscover  *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	gboolean      *fresh,
	GError       **error
){
	g_autofree gchar *keep = axis_discover_int_text(AXIS_DISCOVER_GENRES_KEEP_S);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gint64 stale = 0;
	gint64 all = 0;
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = keep;
	ok = axis_db_conn_exec(conn, "SELECT count(*), count(*) FILTER (WHERE fetched_at < now() - make_interval(secs =>"
		" $3::double precision)) FROM discover_genre WHERE provider = $1 AND kind = $2", 3, params, &result, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &all);
	axis_db_result_get_int64(result, 0, 1, &stale);
	*fresh = all > 0 && stale == 0;
	return TRUE;
}

static gboolean
store_genres(
	AxisDiscover  *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	GPtrArray     *genres,
	GError       **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	const gchar *params[4];
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	for (i = 0; i < genres->len; i++) {
		const AxisMetaGenre *genre = g_ptr_array_index(genres, i);

		g_array_append_val(ids, genre->id);
		g_ptr_array_add(names, genre->name);
	}
	a = axis_discover_sql_int_array(ids, FALSE, 0);
	b = axis_discover_sql_text_array(names);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = a;
	params[3] = b;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "DELETE FROM discover_genre WHERE provider = $1 AND kind = $2", 2, params, NULL,
			error) &&
		axis_db_conn_exec(conn, "INSERT INTO discover_genre (provider, kind, genre_id, name)"
			" SELECT $1, $2, u.i, u.n FROM unnest($3::integer[], $4::text[]) AS u (i, n)", 4, params, NULL, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* *@problem: (out) when the provider is not to be asked again this run, why. */
static gboolean
refresh_genres(
	AxisDiscover               *self,
	const AxisMetaProviderInfo *provider,
	AxisMetaKind                kind,
	GCancellable               *cancellable,
	AxisDiscoverReport         *report,
	gboolean                   *gone,
	gchar                     **problem_out,
	GError                    **error
){
	g_autoptr(GPtrArray) genres = NULL;
	g_autoptr(GError) local = NULL;
	gboolean fresh = FALSE;

	*gone = FALSE;
	*problem_out = NULL;
	if (provider->genres == NULL)
		return TRUE;
	if (!genres_fresh(self, provider->name, kind, &fresh, error))
		return FALSE;
	if (fresh)
		return TRUE;
	if (!axis_meta_fetch_genres(self->meta, provider->name, kind, cancellable, &genres, &local)) {
		g_autofree gchar *problem = NULL;

		if (store_failed(local)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		*gone = provider_gone(local);
		problem = problem_of(local);
		g_info("discover: the genres of %s are left as they were: %s", kind_name(kind), problem);
		/* A daemon stopping is no problem of the provider's. */
		if (*gone && !g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_CANCELLED))
			*problem_out = g_steal_pointer(&problem);
		return TRUE;
	}
	if (!store_genres(self, provider->name, kind, genres, error))
		return FALSE;
	report->genres++;
	return TRUE;
}

/* The lists (bits) of @kind whose snapshot is still fresh. */
static gboolean
fresh_lists(
	AxisDiscover  *self,
	const gchar   *provider,
	AxisMetaKind   kind,
	guint         *fresh,
	GError       **error
){
	g_autofree gchar *after = axis_discover_int_text(axis_discover_refresh_after(self));
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;
	guint i;

	*fresh = 0;
	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = after;
	ok = axis_db_conn_exec(conn, "SELECT list FROM discover_list WHERE provider = $1 AND kind = $2"
		" AND seed_title_id IS NULL AND fetched_at > now() - make_interval(secs => $3::double precision)", 3, params,
		&result, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisMetaList list;

		if (axis_meta_list_from_string(axis_db_result_get_text(result, i, 0), &list))
			*fresh |= AXIS_META_LIST_BIT(list);
	}
	return TRUE;
}

typedef struct
{
	gint64  id;
	gchar  *provider_id;
} Seed;

static void
seed_free(Seed *seed)
{
	g_free(seed->provider_id);
	g_free(seed);
}

/* Drops the recommendations of titles no longer had, and picks the titles
 * whose recommendations are fetched now: never fetched first, then the
 * oldest. */
static gboolean
pick_seeds(
	AxisDiscover        *self,
	const gchar         *provider,
	AxisMetaKind         kind,
	guint                n,
	GPtrArray          **seeds,
	AxisDiscoverReport  *report,
	GError             **error
){
	g_autofree gchar *have = axis_meta_referenced_sql(self->meta);
	g_autofree gchar *drop = g_strdup_printf("DELETE FROM discover_list l WHERE l.provider = $1 AND l.kind = $2"
		" AND l.list = 'recommended' AND NOT EXISTS (SELECT 1 FROM (%s) AS h WHERE h.id = l.seed_title_id)", have);
	g_autofree gchar *pick = g_strdup_printf("SELECT t.id, t.provider_id FROM meta_title t"
		" LEFT JOIN discover_list l ON l.provider = $1 AND l.kind = $2 AND l.list = 'recommended'"
		"  AND l.seed_title_id = t.id"
		" WHERE t.provider = $1 AND t.kind = $2 AND t.id IN (%s)"
		"  AND (l.id IS NULL OR l.tried_at < now() - make_interval(secs => $3::double precision))"
		" ORDER BY (l.id IS NOT NULL), l.tried_at, t.id DESC LIMIT $4::integer", have);
	g_autofree gchar *keep = axis_discover_int_text(AXIS_DISCOVER_RECOMMEND_KEEP_S);
	g_autofree gchar *limit = axis_discover_int_text(n);
	g_autoptr(AxisDbResult) dropped = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[4];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;
	guint i;

	*seeds = NULL;
	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = keep;
	params[3] = limit;
	ok = axis_db_conn_exec(conn, drop, 2, params, &dropped, error) &&
		(n == 0 || axis_db_conn_exec(conn, pick, 4, params, &result, error));
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	report->dropped += (guint)axis_db_result_get_affected_rows(dropped);
	*seeds = g_ptr_array_new_with_free_func((GDestroyNotify)seed_free);
	for (i = 0; result != NULL && i < axis_db_result_get_n_rows(result); i++) {
		Seed *seed = g_new0(Seed, 1);

		axis_db_result_get_int64(result, i, 0, &seed->id);
		seed->provider_id = g_strdup(axis_db_result_get_text(result, i, 1));
		g_ptr_array_add(*seeds, seed);
	}
	return TRUE;
}

static gboolean
cancelled(AxisJobContext *context)
{
	return context != NULL && axis_job_context_is_cancelled(context);
}

/* Everything of one kind (@all: every list, whatever its age).  Returns
 * %FALSE for a failure of the store. */
static gboolean
refresh_kind(
	AxisDiscover               *self,
	const AxisMetaProviderInfo *provider,
	AxisMetaKind                kind,
	AxisJobContext             *context,
	gboolean                    all,
	AxisDiscoverReport         *report,
	GError                    **error
){
	GCancellable *cancellable = context != NULL ? axis_job_context_get_cancellable(context) : NULL;
	AxisMetaList lists[AXIS_META_N_LISTS];
	g_autoptr(GPtrArray) seeds = NULL;
	g_autofree gchar *problem = NULL;
	guint pages = axis_discover_pages(self);
	guint fresh = 0;
	gboolean gone = FALSE;
	gboolean recommends = FALSE;
	guint n;
	guint i;

	n = axis_discover_lists(provider, kind, lists);
	if (!refresh_genres(self, provider, kind, cancellable, report, &gone, &problem, error))
		return FALSE;
	if (gone) {
		/* Said on the first list's page, and the attempt recorded: a page
		 * with nothing stored does not ask again by itself. */
		if (problem != NULL && n > 0 && lists[0] != AXIS_META_LIST_RECOMMENDED)
			return record_failure(self, provider->name, kind, lists[0], 0, problem, error);
		return TRUE;
	}
	if (!all && !fresh_lists(self, provider->name, kind, &fresh, error))
		return FALSE;
	for (i = 0; i < n; i++) {
		if (lists[i] == AXIS_META_LIST_RECOMMENDED) {
			recommends = TRUE;
			continue;
		}
		if ((fresh & AXIS_META_LIST_BIT(lists[i])) != 0)
			continue;
		if (cancelled(context))
			return TRUE;
		if (!refresh_list(self, provider, kind, lists[i], 0, NULL, pages, cancellable, report, &gone, error))
			return FALSE;
		if (gone)
			return TRUE;
	}
	if (!recommends)
		return TRUE;
	if (!pick_seeds(self, provider->name, kind, axis_discover_recommend_per_run(self), &seeds, report, error))
		return FALSE;
	for (i = 0; i < seeds->len; i++) {
		const Seed *seed = g_ptr_array_index(seeds, i);

		if (cancelled(context))
			return TRUE;
		if (!refresh_list(self, provider, kind, AXIS_META_LIST_RECOMMENDED, seed->id, seed->provider_id, 1,
		        cancellable, report, &gone, error))
			return FALSE;
		if (gone)
			return TRUE;
	}
	return TRUE;
}

gboolean
axis_discover_run_refresh(
	AxisDiscover        *self,
	AxisJobContext      *context,
	gboolean             all,
	AxisDiscoverReport  *report_out,
	GError             **error
){
	static const AxisMetaKind kinds[] = { AXIS_META_KIND_FILM, AXIS_META_KIND_SERIES };
	AxisDiscoverReport report;
	guint k;

	g_return_val_if_fail(self != NULL, FALSE);

	memset(&report, 0, sizeof(report));
	if (report_out != NULL)
		memset(report_out, 0, sizeof(*report_out));
	for (k = 0; k < G_N_ELEMENTS(kinds) && !cancelled(context); k++) {
		gboolean ready = FALSE;
		const AxisMetaProviderInfo *provider = axis_discover_provider(self, kinds[k], &ready);

		if (provider == NULL || !ready) {
			report.skipped++;
			continue;
		}
		if (!refresh_kind(self, provider, kinds[k], context, all, &report, error))
			return FALSE;
		if (context != NULL)
			axis_job_context_set_progress(context, (gdouble)(k + 1) / G_N_ELEMENTS(kinds), kind_name(kinds[k]));
	}
	if (cancelled(context)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
		return FALSE;
	}
	if (report.pages > 0 || report.dropped > 0)
		g_info("discover: %u lists and %u titles' recommendations fetched (%u pages), %u failed, %u dropped",
			report.lists, report.recommended, report.pages, report.failed, report.dropped);
	if (report_out != NULL)
		*report_out = report;
	return TRUE;
}

gboolean
axis_discover_fetch_like(
	AxisDiscover         *self,
	const AxisMetaTitle  *seed,
	GCancellable         *cancellable,
	gboolean             *asked,
	GError              **error
){
	const AxisMetaProviderInfo *provider;
	AxisDiscoverReport report;
	AxisDiscoverLike like;
	gboolean ready = FALSE;
	gboolean gone = FALSE;
	const gchar *ref = NULL;
	gboolean fresh;

	g_return_val_if_fail(self != NULL && seed != NULL, FALSE);

	if (asked != NULL)
		*asked = FALSE;
	provider = axis_discover_like_provider(self, seed, &ready, &ref);
	if (provider == NULL || !ready)
		return TRUE;
	/* Another view of the page may have asked since this one looked. */
	if (!axis_discover_read_like(self, seed, 1, &like, error)) {
		axis_discover_like_clear(&like);
		return FALSE;
	}
	fresh = like.fresh;
	axis_discover_like_clear(&like);
	if (fresh)
		return TRUE;
	memset(&report, 0, sizeof(report));
	if (asked != NULL)
		*asked = TRUE;
	return refresh_list(self, provider, seed->kind, AXIS_META_LIST_RECOMMENDED, seed->id, ref, 1, cancellable, &report,
		&gone, error);
}

gboolean
axis_discover_request_refresh(
	AxisDiscover  *self,
	gboolean       all,
	gint64        *job_id,
	gboolean      *created,
	GError       **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (all)
		json_object_set_boolean_member(payload, AXIS_DISCOVER_PAYLOAD_ALL, TRUE);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_DISCOVER_REFRESH_KIND;
	spec.queue = AXIS_META_QUEUE;
	spec.payload = payload;
	spec.priority = 10;
	spec.dedupe_key = AXIS_DISCOVER_REFRESH_KIND ":request";
	spec.max_attempts = 1;
	ok = axis_job_enqueue(conn, &spec, job_id, created, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}
