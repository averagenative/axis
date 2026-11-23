/*
 * axis-discover-read.c - what the Discover pages read: a page of a list with
 * its filters, the Recommended ranking, the genre names, and the titles
 * marked "not interested"
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every read is of the store alone.  A page is one statement for the
 * snapshot's facts and one for its entries with their count (the count comes
 * with an empty page too), whatever the filters; the titles are then read
 * from the metadata store in one more.
 */

#include "discover/axis-discover-private.h"

#include <string.h>

/* The most entries a page asks for. */
#define MAX_LIMIT 100

static void
item_free(AxisDiscoverItem *item)
{
	if (item == NULL)
		return;
	axis_meta_title_free(item->title);
	if (item->because != NULL)
		g_ptr_array_unref(item->because);
	g_free(item);
}

void
axis_discover_page_clear(AxisDiscoverPage *page)
{
	if (page == NULL)
		return;
	g_clear_pointer(&page->items, g_ptr_array_unref);
	g_clear_pointer(&page->problem, g_free);
	memset(page, 0, sizeof(*page));
}

void
axis_discover_dismissed_free(AxisDiscoverDismissed *item)
{
	if (item == NULL)
		return;
	axis_meta_title_free(item->title);
	g_free(item->by);
	g_free(item);
}

static gboolean
invalid(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_INVALID, message);
	return FALSE;
}

/* One entry as the statements give it, before its title is read. */
typedef struct
{
	gint64  title_id;
	gint    rating;
	gint    votes;
	guint   because_n;
	GArray *because;	/* gint64 */
} Row;

static void
row_free(Row *row)
{
	if (row->because != NULL)
		g_array_unref(row->because);
	g_free(row);
}

/* "12,34" into gint64s. */
static GArray *
ids_from_text(const gchar *text)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_auto(GStrv) parts = NULL;
	guint i;

	if (text == NULL || text[0] == '\0')
		return ids;
	parts = g_strsplit(text, ",", -1);
	for (i = 0; parts[i] != NULL; i++) {
		gint64 id = g_ascii_strtoll(parts[i], NULL, 10);

		if (id > 0)
			g_array_append_val(ids, id);
	}
	return ids;
}

/* The snapshot's facts of a list that is not a title's recommendations. */
static gboolean
read_list_facts(
	AxisDbConn         *conn,
	const gchar *const *key,
	AxisDiscoverPage   *page,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;

	if (!axis_db_conn_exec(conn, "SELECT l.fetched_at IS NOT NULL, coalesce(" AXIS_DISCOVER_EPOCH_US("l.fetched_at")
	        ", 0), " AXIS_DISCOVER_EPOCH_US("l.tried_at") ", l.problem,"
	        " (SELECT count(*) FROM discover_entry e WHERE e.list_id = l.id)"
	        " FROM discover_list l WHERE l.provider = $1 AND l.kind = $2 AND l.list = $3 AND l.seed_title_id IS NULL",
	        3, key, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	{
		gint64 size = 0;

		axis_db_result_get_boolean(result, 0, 0, &page->stored);
		axis_db_result_get_int64(result, 0, 1, &page->fetched_us);
		axis_db_result_get_int64(result, 0, 2, &page->tried_us);
		if (!axis_db_result_is_null(result, 0, 3))
			page->problem = g_strdup(axis_db_result_get_text(result, 0, 3));
		axis_db_result_get_int64(result, 0, 4, &size);
		page->size = (guint)size;
	}
	return TRUE;
}

/* What the Recommended list is made from: your titles of the kind, those
 * whose recommendations are kept, and when the latest arrived. */
static gboolean
read_recommended_facts(
	AxisDbConn         *conn,
	const gchar        *have,
	const gchar *const *key,
	AxisDiscoverPage   *page,
	GError            **error
){
	g_autofree gchar *sql = g_strdup_printf("SELECT"
		" (SELECT count(*) FROM meta_title t WHERE t.provider = $1 AND t.kind = $2 AND t.id IN (%s)),"
		" count(*) FILTER (WHERE l.fetched_at IS NOT NULL),"
		" coalesce(max(" AXIS_DISCOVER_EPOCH_US("l.fetched_at") "), 0), coalesce(max(" AXIS_DISCOVER_EPOCH_US("l.tried_at")
		"), 0)"
		" FROM discover_list l WHERE l.provider = $1 AND l.kind = $2 AND l.list = 'recommended'"
		" AND l.seed_title_id IN (%s)", have, have);
	g_autoptr(AxisDbResult) result = NULL;
	gint64 n = 0;

	if (!axis_db_conn_exec(conn, sql, 2, key, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &n);
	page->have = (guint)n;
	axis_db_result_get_int64(result, 0, 1, &n);
	page->seeds = (guint)n;
	page->stored = n > 0;
	axis_db_result_get_int64(result, 0, 2, &page->fetched_us);
	axis_db_result_get_int64(result, 0, 3, &page->tried_us);
	return TRUE;
}

/*
 * "Hide unreleased" as a condition on meta_title t, from three parameters:
 * @on (boolean), @days (integer) and @today (date).  A film counts as
 * available as the Movies module counts it (axis-movies-list.c, the
 * "not_available" state): its release date plus @days is not after today, or
 * with no date its status is released.  A series has started when its first
 * air date is not after today, or with no date it is continuing or ended.
 */
#define UNRELEASED_OK(on, days, today) \
	" AND (NOT " on "::boolean OR CASE WHEN t.kind = 'film' THEN" \
	"  CASE WHEN t.release_date IS NULL THEN coalesce(t.status = 'released', false)" \
	"   ELSE t.release_date + " days "::integer <= " today "::date END" \
	" ELSE CASE WHEN t.first_air_date IS NULL THEN coalesce(t.status IN ('continuing', 'ended'), false)" \
	"   ELSE t.first_air_date <= " today "::date END END)"

/* The filters' parameters, $3 to $9 of both statements below. */
typedef struct
{
	gchar *genre;
	gchar *year_from;
	gchar *year_to;
	gchar *rating;
	gchar *limit;
	gchar *offset;
	gchar *hide_unreleased;	/* "true" or "false" */
	gchar *film_days;	/* movies.released_after, when films wait for it */
	gchar *today;		/* "YYYY-MM-DD", UTC, as the Movies module has it */
} Filters;

static void
filters_clear(Filters *f)
{
	g_free(f->hide_unreleased);
	g_free(f->film_days);
	g_free(f->today);
	g_free(f->genre);
	g_free(f->year_from);
	g_free(f->year_to);
	g_free(f->rating);
	g_free(f->limit);
	g_free(f->offset);
}

static gboolean
read_list_rows(
	AxisDbConn               *conn,
	const gchar              *have,
	const AxisDiscoverQuery  *query,
	const gchar *const       *key,
	const Filters            *f,
	GPtrArray                *rows,
	guint                    *total,
	GError                  **error
){
	g_autofree gchar *sql = g_strdup_printf("WITH m AS (SELECT e.title_id, e.position, e.rating, e.votes"
		" FROM discover_list l JOIN discover_entry e ON e.list_id = l.id JOIN meta_title t ON t.id = e.title_id"
		" WHERE l.provider = $1 AND l.kind = $2 AND l.list = $3 AND l.seed_title_id IS NULL"
		" AND ($4::integer = 0 OR $4::integer = ANY (e.genre_ids))"
		" AND ($5::integer = 0 OR t.year >= $5::integer) AND ($6::integer = 0 OR t.year <= $6::integer)"
		" AND ($7::integer = 0 OR e.rating >= $7::integer)"
		" AND NOT EXISTS (SELECT 1 FROM discover_dismissed d WHERE d.title_id = e.title_id)"
		" AND (NOT $8::boolean OR NOT EXISTS (SELECT 1 FROM (%s) AS h WHERE h.id = e.title_id))"
		UNRELEASED_OK("$11", "$12", "$13") ")"
		" SELECT c.n, p.title_id, coalesce(p.rating, -1), p.votes FROM (SELECT count(*) AS n FROM m) AS c"
		" LEFT JOIN LATERAL (SELECT * FROM m ORDER BY m.position LIMIT $9::integer OFFSET $10::integer) AS p ON true"
		" ORDER BY p.position", have);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[13];
	guint i;

	params[0] = key[0];
	params[1] = key[1];
	params[2] = key[2];
	params[10] = f->hide_unreleased;
	params[11] = f->film_days;
	params[12] = f->today;
	params[3] = f->genre;
	params[4] = f->year_from;
	params[5] = f->year_to;
	params[6] = f->rating;
	params[7] = query->hide_have ? "true" : "false";
	params[8] = f->limit;
	params[9] = f->offset;
	if (!axis_db_conn_exec(conn, sql, 13, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 n = 0;
		gint64 value = 0;
		Row *row;

		axis_db_result_get_int64(result, i, 0, &n);
		*total = (guint)n;
		if (axis_db_result_is_null(result, i, 1))
			continue;
		row = g_new0(Row, 1);
		axis_db_result_get_int64(result, i, 1, &row->title_id);
		axis_db_result_get_int64(result, i, 2, &value);
		row->rating = (gint)value;
		axis_db_result_get_int64(result, i, 3, &value);
		row->votes = (gint)value;
		g_ptr_array_add(rows, row);
	}
	return TRUE;
}

static gboolean
read_recommended_rows(
	AxisDbConn         *conn,
	const gchar        *have,
	const gchar *const *key,
	const Filters      *f,
	GPtrArray          *rows,
	guint              *total,
	guint              *size,
	GError            **error
){
	g_autofree gchar *sql = g_strdup_printf("WITH have AS (%s),"
		" rec AS (SELECT e.title_id, count(DISTINCT l.seed_title_id) AS n, min(e.position) AS best,"
		"  max(e.rating) AS rating, max(e.votes) AS votes,"
		"  bool_or($3::integer = 0 OR $3::integer = ANY (e.genre_ids)) AS genre_ok,"
		"  (array_agg(l.seed_title_id ORDER BY e.position, l.seed_title_id))[1:%d] AS because"
		"  FROM discover_list l JOIN discover_entry e ON e.list_id = l.id"
		"  WHERE l.provider = $1 AND l.kind = $2 AND l.list = 'recommended'"
		"  AND l.seed_title_id IN (SELECT id FROM have) AND e.title_id NOT IN (SELECT id FROM have)"
		"  AND NOT EXISTS (SELECT 1 FROM discover_dismissed d WHERE d.title_id = e.title_id)"
		"  GROUP BY e.title_id),"
		" m AS (SELECT r.* FROM rec r JOIN meta_title t ON t.id = r.title_id WHERE r.genre_ok"
		"  AND ($4::integer = 0 OR t.year >= $4::integer) AND ($5::integer = 0 OR t.year <= $5::integer)"
		"  AND ($6::integer = 0 OR r.rating >= $6::integer)" UNRELEASED_OK("$9", "$10", "$11") ")"
		" SELECT c.n_all, c.n, p.title_id, coalesce(p.rating, -1), p.votes, p.n, array_to_string(p.because, ',')"
		" FROM (SELECT (SELECT count(*) FROM rec) AS n_all, (SELECT count(*) FROM m) AS n) AS c"
		" LEFT JOIN LATERAL (SELECT * FROM m ORDER BY m.n DESC, m.best, m.rating DESC NULLS LAST, m.title_id"
		"  LIMIT $7::integer OFFSET $8::integer) AS p ON true"
		" ORDER BY p.n DESC, p.best, p.rating DESC NULLS LAST, p.title_id", have, AXIS_DISCOVER_BECAUSE_NAMES);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[11];
	guint i;

	params[8] = f->hide_unreleased;
	params[9] = f->film_days;
	params[10] = f->today;
	params[0] = key[0];
	params[1] = key[1];
	params[2] = f->genre;
	params[3] = f->year_from;
	params[4] = f->year_to;
	params[5] = f->rating;
	params[6] = f->limit;
	params[7] = f->offset;
	if (!axis_db_conn_exec(conn, sql, 11, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 n = 0;
		gint64 value = 0;
		Row *row;

		axis_db_result_get_int64(result, i, 0, &n);
		*size = (guint)n;
		axis_db_result_get_int64(result, i, 1, &n);
		*total = (guint)n;
		if (axis_db_result_is_null(result, i, 2))
			continue;
		row = g_new0(Row, 1);
		axis_db_result_get_int64(result, i, 2, &row->title_id);
		axis_db_result_get_int64(result, i, 3, &value);
		row->rating = (gint)value;
		axis_db_result_get_int64(result, i, 4, &value);
		row->votes = (gint)value;
		axis_db_result_get_int64(result, i, 5, &value);
		row->because_n = (guint)value;
		row->because = ids_from_text(axis_db_result_get_text(result, i, 6));
		g_ptr_array_add(rows, row);
	}
	return TRUE;
}

/* The titles of @rows from the metadata store, and the names of the titles
 * they are recommended for, into @page's items. */
static gboolean
attach_titles(
	AxisDiscover      *self,
	GPtrArray         *rows,
	AxisDiscoverPage  *page,
	GError           **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) because = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GPtrArray) seeds = NULL;
	g_autoptr(GHashTable) by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GHashTable) names = g_hash_table_new(g_int64_hash, g_int64_equal);
	guint i;
	guint j;

	for (i = 0; i < rows->len; i++) {
		const Row *row = g_ptr_array_index(rows, i);

		g_array_append_val(ids, row->title_id);
		for (j = 0; row->because != NULL && j < row->because->len; j++)
			g_array_append_val(because, g_array_index(row->because, gint64, j));
	}
	if (!axis_meta_load_titles(self->meta, ids, &titles, error) ||
	    !axis_meta_load_titles(self->meta, because, &seeds, error))
		return FALSE;
	for (i = 0; i < titles->len; i++) {
		AxisMetaTitle *title = g_ptr_array_index(titles, i);

		g_hash_table_insert(by_id, &title->id, title);
	}
	for (i = 0; i < seeds->len; i++) {
		AxisMetaTitle *title = g_ptr_array_index(seeds, i);

		g_hash_table_insert(names, &title->id, title->title);
	}
	for (i = 0; i < rows->len; i++) {
		const Row *row = g_ptr_array_index(rows, i);
		AxisMetaTitle *title = g_hash_table_lookup(by_id, &row->title_id);
		AxisDiscoverItem *item;
		guint k;

		/* Gone from the store between the two statements (a merge). */
		if (title == NULL)
			continue;
		item = g_new0(AxisDiscoverItem, 1);
		item->rating = row->rating;
		item->votes = row->votes;
		item->because_n = row->because_n;
		item->because = g_ptr_array_new_with_free_func(g_free);
		for (j = 0; row->because != NULL && j < row->because->len; j++) {
			const gchar *name = g_hash_table_lookup(names, &g_array_index(row->because, gint64, j));

			if (name != NULL)
				g_ptr_array_add(item->because, g_strdup(name));
		}
		/* The record moves to the item: steal it from @titles. */
		for (k = 0; k < titles->len; k++) {
			if (g_ptr_array_index(titles, k) == title) {
				item->title = g_ptr_array_steal_index(titles, k);
				break;
			}
		}
		g_hash_table_remove(by_id, &row->title_id);
		g_ptr_array_add(page->items, item);
	}
	return TRUE;
}

/* The days after its release date a film counts as available, as the Movies
 * module's settings have them (the settings are read by name: this layer
 * does not include the module).  Only the default "released" waits; "in
 * cinemas" and "announced" count a film from its date. */
static gint64
film_wait_days(AxisDiscover *self)
{
	static const gchar availability[] = "movies.default_min_availability";
	static const gchar after[] = "movies.released_after";
	g_autofree gchar *mode = NULL;

	if (self->settings != NULL && axis_settings_lookup(self->settings, availability, NULL) != NULL)
		mode = axis_settings_get_string(self->settings, availability);
	if (mode != NULL && strcmp(mode, "released") != 0)
		return 0;
	if (self->settings == NULL || axis_settings_lookup(self->settings, after, NULL) == NULL)
		return 90;
	return CLAMP(axis_settings_get_duration(self->settings, after) / 86400, 0, 3650);
}

/* Today, UTC, "YYYY-MM-DD", the day the Movies module goes by. */
static gchar *
today_text(void)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	gchar *text = now != NULL ? g_date_time_format(now, "%Y-%m-%d") : NULL;

	return text != NULL ? text : g_strdup("1970-01-01");
}

gboolean
axis_discover_read(
	AxisDiscover             *self,
	const AxisDiscoverQuery  *query,
	AxisDiscoverPage         *page,
	GError                  **error
){
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	g_autofree gchar *have = NULL;
	g_autoptr(AxisDbResult) tried = NULL;
	const gchar *key[3];
	Filters filters;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && query != NULL && page != NULL, FALSE);

	memset(page, 0, sizeof(*page));
	page->items = g_ptr_array_new_with_free_func((GDestroyNotify)item_free);
	if (query->limit == 0 || query->limit > MAX_LIMIT || query->offset > 100000)
		return invalid(error, "that is not a page of a list");
	if ((query->year_from != 0 && !axis_meta_year_valid(query->year_from)) ||
	    (query->year_to != 0 && !axis_meta_year_valid(query->year_to)))
		return invalid(error, "that is not a year");
	if (query->min_rating < 0 || query->min_rating > 100 || query->genre < 0)
		return invalid(error, "that is not a filter of a list");
	page->provider = axis_discover_provider(self, query->kind, &page->ready);
	if (page->provider == NULL)
		return TRUE;
	if ((axis_meta_provider_lists(page->provider, query->kind) & AXIS_META_LIST_BIT(query->list)) == 0)
		return invalid(error, "there is no such list");

	key[0] = page->provider->name;
	key[1] = axis_meta_kind_to_string(query->kind);
	key[2] = axis_meta_list_to_string(query->list);
	have = axis_meta_referenced_sql(self->meta);
	memset(&filters, 0, sizeof(filters));
	filters.genre = axis_discover_int_text(query->genre);
	filters.year_from = axis_discover_int_text(query->year_from);
	filters.year_to = axis_discover_int_text(query->year_to);
	filters.rating = axis_discover_int_text(query->min_rating);
	filters.limit = axis_discover_int_text(query->limit);
	filters.offset = axis_discover_int_text(query->offset);
	filters.hide_unreleased = g_strdup(query->hide_unreleased ? "true" : "false");
	filters.film_days = axis_discover_int_text((gint)film_wait_days(self));
	filters.today = today_text();

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		filters_clear(&filters);
		return FALSE;
	}
	ok = axis_db_conn_exec(conn, "SELECT EXISTS (SELECT 1 FROM discover_list WHERE provider = $1 AND kind = $2)", 2,
		key, &tried, error);
	if (ok)
		axis_db_result_get_boolean(tried, 0, 0, &page->tried_ever);
	if (ok && query->list == AXIS_META_LIST_RECOMMENDED)
		ok = read_recommended_facts(conn, have, key, page, error) &&
			read_recommended_rows(conn, have, key, &filters, rows, &page->total, &page->size, error);
	else if (ok)
		ok = read_list_facts(conn, key, page, error) &&
			read_list_rows(conn, have, query, key, &filters, rows, &page->total, error);
	axis_db_pool_release(self->pool, conn);
	filters_clear(&filters);
	return ok && attach_titles(self, rows, page, error);
}

/* ── More like this ─────────────────────────────────────────────── */

void
axis_discover_like_clear(AxisDiscoverLike *like)
{
	if (like == NULL)
		return;
	g_clear_pointer(&like->items, g_ptr_array_unref);
	g_clear_pointer(&like->problem, g_free);
	memset(like, 0, sizeof(*like));
}

gboolean
axis_discover_read_like(
	AxisDiscover         *self,
	const AxisMetaTitle  *seed,
	guint                 limit,
	AxisDiscoverLike     *like,
	GError              **error
){
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	g_autoptr(AxisDbResult) facts = NULL;
	g_autoptr(AxisDbResult) entries = NULL;
	g_autofree gchar *seed_id = NULL;
	g_autofree gchar *keep = axis_discover_int_text(AXIS_DISCOVER_RECOMMEND_KEEP_S);
	g_autofree gchar *retry = axis_discover_int_text(AXIS_DISCOVER_LIKE_RETRY_S);
	g_autofree gchar *limit_text = NULL;
	const gchar *params[5];
	AxisDiscoverPage page;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && seed != NULL && like != NULL, FALSE);

	memset(like, 0, sizeof(*like));
	like->items = g_ptr_array_new_with_free_func((GDestroyNotify)item_free);
	if (limit == 0 || limit > MAX_LIMIT)
		return invalid(error, "that is not a page of a list");
	like->provider = axis_discover_like_provider(self, seed, &like->ready, NULL);
	if (like->provider == NULL)
		return TRUE;

	seed_id = axis_discover_int_text(seed->id);
	limit_text = axis_discover_int_text(limit);
	params[0] = like->provider->name;
	params[1] = axis_meta_kind_to_string(seed->kind);
	params[2] = seed_id;
	params[3] = keep;
	params[4] = retry;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* Fresh: fetched within its week, or an attempt failed within the hour. */
	ok = axis_db_conn_exec(conn, "SELECT l.fetched_at IS NOT NULL, coalesce(" AXIS_DISCOVER_EPOCH_US("l.fetched_at")
		", 0), coalesce(" AXIS_DISCOVER_EPOCH_US("l.tried_at") ", 0), l.problem,"
		" coalesce(l.fetched_at > now() - make_interval(secs => $4::double precision)"
		"  OR (l.problem IS NOT NULL AND l.tried_at > now() - make_interval(secs => $5::double precision)), false),"
		" (SELECT count(*) FROM discover_entry e WHERE e.list_id = l.id)"
		" FROM discover_list l WHERE l.provider = $1 AND l.kind = $2 AND l.list = 'recommended'"
		" AND l.seed_title_id = $3::bigint", 5, params, &facts, error);
	if (ok) {
		params[3] = limit_text;
		ok = axis_db_conn_exec(conn, "SELECT e.title_id, coalesce(e.rating, -1), e.votes"
			" FROM discover_list l JOIN discover_entry e ON e.list_id = l.id"
			" WHERE l.provider = $1 AND l.kind = $2 AND l.list = 'recommended' AND l.seed_title_id = $3::bigint"
			" AND NOT EXISTS (SELECT 1 FROM discover_dismissed d WHERE d.title_id = e.title_id)"
			" ORDER BY e.position LIMIT $4::integer", 4, params, &entries, error);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (axis_db_result_get_n_rows(facts) > 0) {
		gint64 size = 0;

		axis_db_result_get_boolean(facts, 0, 0, &like->stored);
		axis_db_result_get_int64(facts, 0, 1, &like->fetched_us);
		axis_db_result_get_int64(facts, 0, 2, &like->tried_us);
		if (!axis_db_result_is_null(facts, 0, 3))
			like->problem = g_strdup(axis_db_result_get_text(facts, 0, 3));
		axis_db_result_get_boolean(facts, 0, 4, &like->fresh);
		axis_db_result_get_int64(facts, 0, 5, &size);
		like->size = (guint)size;
	}
	for (i = 0; i < axis_db_result_get_n_rows(entries); i++) {
		Row *row = g_new0(Row, 1);
		gint64 value = 0;

		axis_db_result_get_int64(entries, i, 0, &row->title_id);
		axis_db_result_get_int64(entries, i, 1, &value);
		row->rating = (gint)value;
		axis_db_result_get_int64(entries, i, 2, &value);
		row->votes = (gint)value;
		g_ptr_array_add(rows, row);
	}
	/* (The titles are read as a page's are: only its items are used.) */
	memset(&page, 0, sizeof(page));
	page.items = like->items;
	return attach_titles(self, rows, &page, error);
}

gboolean
axis_discover_genres(
	AxisDiscover  *self,
	AxisMetaKind   kind,
	GPtrArray    **genres,
	GError       **error
){
	const AxisMetaProviderInfo *provider = axis_discover_provider(self, kind, NULL);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	guint i;

	*genres = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_genre_free);
	if (provider == NULL)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = provider->name;
	params[1] = axis_meta_kind_to_string(kind);
	if (!axis_db_conn_exec(conn, "SELECT genre_id, name FROM discover_genre WHERE provider = $1 AND kind = $2"
	        " ORDER BY name, genre_id", 2, params, &result, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisMetaGenre *genre = g_new0(AxisMetaGenre, 1);
		gint64 id = 0;

		axis_db_result_get_int64(result, i, 0, &id);
		genre->id = (gint)id;
		genre->name = g_strdup(axis_db_result_get_text(result, i, 1));
		g_ptr_array_add(*genres, genre);
	}
	return TRUE;
}

/* ── Not interested ─────────────────────────────────────────────── */

gboolean
axis_discover_dismiss(
	AxisDiscover  *self,
	gint64         title_id,
	const gchar   *user,
	GError       **error
){
	g_autofree gchar *id = axis_discover_int_text(title_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gint64 found = 0;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	params[1] = user != NULL && user[0] != '\0' && strlen(user) <= 64 ? user : NULL;
	ok = axis_db_conn_exec(conn, "WITH t AS (SELECT id FROM meta_title WHERE id = $1::bigint),"
		" i AS (INSERT INTO discover_dismissed (title_id, dismissed_by) SELECT id, $2 FROM t"
		"  ON CONFLICT (title_id) DO NOTHING RETURNING title_id)"
		" SELECT count(*) FROM t", 2, params, &result, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &found);
	if (found == 0) {
		g_set_error_literal(error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_NOT_FOUND, "there is no such title");
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_discover_undismiss(
	AxisDiscover  *self,
	gint64         title_id,
	gboolean      *removed,
	GError       **error
){
	g_autofree gchar *id = axis_discover_int_text(title_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	ok = axis_db_conn_exec(conn, "DELETE FROM discover_dismissed WHERE title_id = $1::bigint", 1, params, &result,
		error);
	axis_db_pool_release(self->pool, conn);
	if (ok && removed != NULL)
		*removed = axis_db_result_get_affected_rows(result) > 0;
	return ok;
}

gboolean
axis_discover_list_dismissed(
	AxisDiscover  *self,
	guint          offset,
	guint          limit,
	GPtrArray    **items,
	guint         *total,
	GError       **error
){
	g_autofree gchar *limit_text = axis_discover_int_text(MIN(MAX(limit, 1u), (guint)MAX_LIMIT));
	g_autofree gchar *offset_text = axis_discover_int_text(MIN(offset, 100000u));
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) counted = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) titles = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gint64 n = 0;
	gboolean ok;
	guint i;
	guint j;

	*items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_discover_dismissed_free);
	*total = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = limit_text;
	params[1] = offset_text;
	ok = axis_db_conn_exec(conn, "SELECT count(*) FROM discover_dismissed", 0, NULL, &counted, error) &&
		axis_db_conn_exec(conn, "SELECT title_id, dismissed_by, " AXIS_DISCOVER_EPOCH_US("dismissed_at")
			" FROM discover_dismissed ORDER BY dismissed_at DESC, title_id LIMIT $1::integer OFFSET $2::integer", 2,
			params, &result, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	axis_db_result_get_int64(counted, 0, 0, &n);
	*total = (guint)n;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = 0;

		axis_db_result_get_int64(result, i, 0, &id);
		g_array_append_val(ids, id);
	}
	if (!axis_meta_load_titles(self->meta, ids, &titles, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = g_array_index(ids, gint64, i);

		for (j = 0; j < titles->len; j++) {
			AxisMetaTitle *title = g_ptr_array_index(titles, j);
			AxisDiscoverDismissed *item;

			if (title == NULL || title->id != id)
				continue;
			item = g_new0(AxisDiscoverDismissed, 1);
			item->title = g_ptr_array_steal_index(titles, j);
			if (!axis_db_result_is_null(result, i, 1))
				item->by = g_strdup(axis_db_result_get_text(result, i, 1));
			axis_db_result_get_int64(result, i, 2, &item->when_us);
			g_ptr_array_add(*items, item);
			break;
		}
	}
	return TRUE;
}
