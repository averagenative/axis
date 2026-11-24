/*
 * axis-movies-list.c - a page of the movies, whatever their number: the
 * films Axis manages and the movies on disk it does not
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A film's state is not stored: axis_movie_derive() works it out from the
 * facts query (axis-movies-store.c), which reads a dozen tables for each
 * film.  A page must not read that for every film, so a page of the list is
 * these statements, whatever the number of entries:
 *
 *  1. the counts by state, for the tabs: one statement, the state of a film
 *     worked out in SQL (state_sql below) from the facts it needs, with the
 *     parts that only C can read (a quality profile's definition, the
 *     ladder, the rules' clock and settings) handed in as parameters, worked
 *     out here by the same functions axis_movie_derive() calls; with it the
 *     number of movies on disk Axis does not manage (always "in library")
 *     and of the units identification holds for a person;
 *  2. the page's entries, keyset-paged in the list's order: the films (the
 *     newest first by its index), the state worked out only when a state is
 *     asked for, and the movies on disk (one pass over the linked units),
 *     each side cut to the page before the two are merged;
 *  3. the facts of those entries alone (one statement for the films, one for
 *     the movies on disk), from which their state and its reason are
 *     derived as everywhere else.
 *
 * An entry is one title: a film's last key is its id (or its title's), a
 * movie on disk's its title's id, negated where a film's is the film's, so
 * no two entries share one.
 *
 * The SQL state and axis_movie_derive() must agree: the tests hold the one
 * against the other for every film of their libraries
 * (axis_movies_list_states()).  A film whose state changes between the
 * statements is shown in its new state.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "release/axis-release.h"

/* ── Sorts and cursors ──────────────────────────────────────────── */

static const gchar *const sort_names[] = { "added", "title", "year", "size" };

const gchar *
axis_movies_sort_name(AxisMoviesSort sort)
{
	return (guint)sort < G_N_ELEMENTS(sort_names) ? sort_names[sort] : "added";
}

gboolean
axis_movies_sort_from_name(
	const gchar    *name,
	AxisMoviesSort *sort
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(sort_names); i++) {
		if (strcmp(sort_names[i], name) == 0) {
			*sort = (AxisMoviesSort)i;
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * Each order: its keys in SQL (the last is the film's id), whether the
 * order is descending, and whether each key is a number (else text); then
 * the same keys of a movie on disk (lib and t of AXIS_MOVIES_LIBRARY_FROM),
 * of the same types.  The keys of the row a page ends on are its cursor;
 * the next page is the rows after them in the same order, compared as one
 * row value.
 */
static const struct
{
	const gchar *k1;
	const gchar *k2;	/* (nullable) */
	const gchar *last;	/* unique: the film's id, or its title's */
	gboolean     descending;
	const gchar *k1_type;
	const gchar *k2_type;
	const gchar *lk1;
	const gchar *lk2;	/* (nullable when k2 is) */
	const gchar *llast;	/* its title's id, negated where last is a film's */
} orders[] = {
	/* movie_added; meta_title_by_name; meta_title_by_year (0029_title_lists.sql). */
	[AXIS_MOVIES_SORT_ADDED] = { "m.added_at", NULL, "m.id", TRUE, "timestamptz", NULL,
		"lib.found_at", NULL, "(-t.id)" },
	[AXIS_MOVIES_SORT_TITLE] = { "lower(t.title)", "coalesce(t.year, 0)", "t.id", FALSE, "text", "integer",
		"lower(t.title)", "coalesce(t.year, 0)", "t.id" },
	[AXIS_MOVIES_SORT_YEAR]  = { "(-coalesce(t.year, 0))", "lower(t.title)", "t.id", FALSE, "integer", "text",
		"(-coalesce(t.year, 0))", "lower(t.title)", "t.id" },
	/* Not indexed: a film's size is the sum of its files'. */
	[AXIS_MOVIES_SORT_SIZE]  = { "(" "SELECT coalesce(sum(lf.size), 0) FROM movie_file mf JOIN library_file lf"
		" ON lf.id = mf.file_id WHERE mf.movie_id = m.id AND lf.state = 'present')", NULL, "m.id", TRUE, "bigint",
		NULL, "lib.size", NULL, "(-t.id)" }
};

/* base64url without padding, and back (NULL when it is not). */
static gchar *
b64_encode(const gchar *text)
{
	gchar *out = g_base64_encode((const guchar *)text, strlen(text));
	gchar *p;

	for (p = out; *p != '\0'; p++) {
		if (*p == '+')
			*p = '-';
		else if (*p == '/')
			*p = '_';
		else if (*p == '=') {
			*p = '\0';
			break;
		}
	}
	return out;
}

static gchar *
b64_decode(const gchar *text)
{
	g_autoptr(GString) padded = g_string_new(NULL);
	g_autofree guchar *raw = NULL;
	const gchar *p;
	gsize length = 0;

	for (p = text; *p != '\0'; p++) {
		if (*p == '-')
			g_string_append_c(padded, '+');
		else if (*p == '_')
			g_string_append_c(padded, '/');
		else if (g_ascii_isalnum(*p))
			g_string_append_c(padded, *p);
		else
			return NULL;
	}
	while (padded->len % 4 != 0)
		g_string_append_c(padded, '=');
	raw = g_base64_decode(padded->str, &length);
	if (raw == NULL || memchr(raw, '\0', length) != NULL || !g_utf8_validate((const gchar *)raw, (gssize)length, NULL))
		return NULL;
	return g_strndup((const gchar *)raw, length);
}

/* "<sort>.<k1>.<k2>.<id>", each key base64url: what a link carries. */
static gchar *
cursor_make(
	AxisMoviesSort  sort,
	const gchar    *k1,
	const gchar    *k2,
	const gchar    *id
){
	g_autofree gchar *a = b64_encode(k1 != NULL ? k1 : "");
	g_autofree gchar *b = b64_encode(k2 != NULL ? k2 : "");

	return g_strdup_printf("%s.%s.%s.%s", axis_movies_sort_name(sort), a, b, id);
}

static gboolean
is_integer(const gchar *text)
{
	const gchar *p = text;

	if (*p == '-')
		p++;
	if (*p == '\0' || strlen(p) > 18)
		return FALSE;
	for (; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return FALSE;
	}
	return TRUE;
}

/* A cursor of @sort, its keys checked against their types; %FALSE when it
 * is anything else. */
static gboolean
cursor_parse(
	const gchar     *cursor,
	AxisMoviesSort   sort,
	gchar          **k1,
	gchar          **k2,
	gchar          **id
){
	g_auto(GStrv) parts = NULL;

	*k1 = *k2 = *id = NULL;
	if (cursor == NULL || strlen(cursor) > 2000)
		return FALSE;
	parts = g_strsplit(cursor, ".", 5);
	/* (The last key of a movie on disk is negative in some orders.) */
	if (g_strv_length(parts) != 4 || strcmp(parts[0], axis_movies_sort_name(sort)) != 0 || !is_integer(parts[3]))
		return FALSE;
	*k1 = b64_decode(parts[1]);
	*k2 = b64_decode(parts[2]);
	*id = g_strdup(parts[3]);
	if (*k1 == NULL || *k2 == NULL || (strcmp(orders[sort].k1_type, "text") != 0 &&
	    strcmp(orders[sort].k1_type, "timestamptz") != 0 && !is_integer(*k1)) ||
	    (orders[sort].k2_type != NULL && strcmp(orders[sort].k2_type, "integer") == 0 && !is_integer(*k2)) ||
	    (strcmp(orders[sort].k1_type, "timestamptz") == 0 && (strlen(*k1) > 40 ||
	     strspn(*k1, "0123456789-:. +") != strlen(*k1)))) {
		g_clear_pointer(k1, g_free);
		g_clear_pointer(k2, g_free);
		g_clear_pointer(id, g_free);
		return FALSE;
	}
	return TRUE;
}

/* ── The state in SQL ───────────────────────────────────────────── */

/*
 * What the state statement is given ($1 to $12), worked out here from the
 * same functions and rules axis_movie_derive() uses:
 *
 *   $1-$6  the quality profiles: id, usable, upgrades, upgrade_unknown,
 *          cutoff (its rung, 0 unknown), cutoff_known_source (it is above a
 *          file of unknown source at some resolution: a quality, not a
 *          recording)
 *   $7     the ladder's names, rung 1 first
 *   $8-$9  the resolutions' names and the rank of an unknown source at each
 *   $10    the outcomes of a last search that need attention
 *   $11    today, $12 movies.released_after in days
 *   $13    the volume states a destination is usable in
 *
 * A file's rank is axis_quality_on_disk_rank(): twice its rung when its
 * quality is known (even), one less than twice the lowest source of its
 * resolution when not (odd), 0 when nothing is known.
 */
#define STATE_PARAMS 13

/* A download of the film is still at a client, and it is not its open
 * grab's (od of STATE_FROM: the film's downloads still at a client). */
#define OTHER_LIVE_SQL \
	"(od.ids IS NOT NULL AND (g.download_id IS NULL OR NOT (od.ids <@ ARRAY[g.download_id])))"

#define STATE_SQL \
	"CASE" \
	" WHEN g.id IS NOT NULL AND g.completed_at IS NULL AND d.state IS DISTINCT FROM 'completed' THEN 'downloading'" \
	" WHEN g.id IS NOT NULL THEN CASE WHEN coalesce(idl.state, iow.state) = 'blocked' THEN 'attention'" \
	"   ELSE 'to_import' END" \
	" WHEN " OTHER_LIVE_SQL " THEN 'attention'" \
	" WHEN coalesce(idl.state, iow.state) = 'blocked' THEN 'attention'" \
	" WHEN coalesce(idl.state, iow.state) IN ('waiting', 'running') THEN 'to_import'" \
	" WHEN sj.dedupe_key IS NOT NULL THEN 'searching'" \
	" WHEN fl.n > 0 THEN CASE" \
	"   WHEN m.monitored AND p.upgrades AND fl.best > 0 AND fl.best % 2 = 0 AND p.cutoff > 0" \
	"     AND fl.best < 2 * p.cutoff THEN 'upgrade_wanted'" \
	"   WHEN fl.best % 2 = 1 AND m.monitored AND p.upgrades AND p.upgrade_unknown AND p.cutoff_source" \
	"     AND 2 * p.cutoff > fl.best THEN 'upgrade_wanted'" \
	"   ELSE 'in_library' END" \
	" WHEN NOT CASE WHEN m.min_availability = 'announced' THEN true" \
	"   WHEN t.release_date IS NULL THEN coalesce(t.status = 'released', false)" \
	"   ELSE t.release_date + CASE WHEN m.min_availability = 'released' THEN $12::integer ELSE 0 END" \
	"     <= $11::date END THEN 'not_available'" \
	" WHEN m.root_id IS NULL THEN 'attention'" \
	" WHEN NOT coalesce(v.state = ANY ($13::text[]), false) THEN 'attention'" \
	" WHEN NOT coalesce(p.usable, false) THEN 'attention'" \
	" WHEN s.outcome = ANY ($10::text[]) THEN 'attention'" \
	" ELSE 'wanted' END"

/*
 * The films with what their state is worked out from; the caller adds a
 * WHERE clause.  t is meta_title.
 *
 * Nothing is looked up once a film (six look-ups a film were 160 ms for
 * 20,000 films, at every view of the list): each kind of fact is read once
 * for every film and joined.  What is under way is little whatever the
 * library's size, and is read from the partial indexes that hold just it:
 * the open grabs (at most one a film: movie_grab_one_open), the imports of
 * their downloads, the imports that wait, run or are blocked (at most one a
 * film: import_open_owner), the downloads still at a client and the searches
 * queued or running.  The files and the last searches are as many as the
 * films, read in one pass each.
 */
#define STATE_FROM \
	" FROM movie m JOIN meta_title t ON t.id = m.title_id" \
	" LEFT JOIN unnest($1::bigint[], $2::boolean[], $3::boolean[], $4::boolean[], $5::integer[], $6::boolean[])" \
	"   AS p (id, usable, upgrades, upgrade_unknown, cutoff, cutoff_source) ON p.id = m.quality_profile_id" \
	" LEFT JOIN library_root lr ON lr.id = m.root_id LEFT JOIN volume v ON v.id = lr.volume_id" \
	" LEFT JOIN movie_grab g ON g.movie_id = m.id AND g.closed_at IS NULL" \
	" LEFT JOIN download d ON d.id = g.download_id" \
	/* The latest import of an open grab's download. */ \
	" LEFT JOIN (SELECT DISTINCT ON (ii.download_id) ii.download_id, ii.state FROM import ii" \
	"   WHERE ii.download_id IN (SELECT gg.download_id FROM movie_grab gg WHERE gg.closed_at IS NULL" \
	"    AND gg.download_id IS NOT NULL) ORDER BY ii.download_id, ii.id DESC) idl" \
	"   ON idl.download_id = g.download_id" \
	/* The film's own latest import, where that is one under way: the states \
	 * read of it are these three, and an import that ended says nothing. */ \
	" LEFT JOIN (SELECT ii.owner_id, ii.state FROM import ii WHERE ii.module = '" AXIS_MOVIES_MODULE_ID "'" \
	"   AND ii.owner_id IS NOT NULL AND ii.state IN ('waiting', 'running', 'blocked')" \
	"   AND NOT EXISTS (SELECT 1 FROM import nx WHERE nx.module = '" AXIS_MOVIES_MODULE_ID "'" \
	"    AND nx.owner_id = ii.owner_id AND nx.id > ii.id)) iow ON iow.owner_id = m.id" \
	" LEFT JOIN (SELECT f.movie_id, count(*) AS n, coalesce(max(CASE" \
	"     WHEN array_position($7::text[], f.quality) IS NOT NULL THEN 2 * array_position($7::text[], f.quality)" \
	"     ELSE coalesce(($9::integer[])[array_position($8::text[], f.resolution)], 0) END), 0) AS best" \
	/* (Each film's file looked up by its id, which OFFSET 0 holds the planner \
	 * to: joined as two tables, it walked the whole file index, every \
	 * series' and every other file's entries among the films'.) */ \
	"   FROM movie_file f JOIN LATERAL (SELECT 1 FROM library_file lf WHERE lf.id = f.file_id" \
	"    AND lf.state = 'present' OFFSET 0) present ON true" \
	"   GROUP BY f.movie_id) fl ON fl.movie_id = m.id" \
	" LEFT JOIN (SELECT DISTINCT ON (ss.movie_id) ss.movie_id, ss.outcome FROM movie_search ss" \
	"   WHERE ss.finished_at IS NOT NULL AND ss.cause <> 'list' ORDER BY ss.movie_id, ss.id DESC) s" \
	"   ON s.movie_id = m.id" \
	" LEFT JOIN (SELECT od.title_id, array_agg(od.id) AS ids FROM download od" \
	"   WHERE od.module = '" AXIS_MOVIES_MODULE_ID "' AND od.title_id IS NOT NULL" \
	"   AND od.state IN ('sending', 'queued', 'downloading', 'paused') GROUP BY od.title_id) od" \
	"   ON od.title_id = m.title_id" \
	" LEFT JOIN (SELECT DISTINCT jj.dedupe_key FROM job jj WHERE jj.dedupe_key IS NOT NULL" \
	"   AND jj.state IN ('queued', 'running') AND jj.dedupe_key LIKE '" AXIS_MOVIES_SEARCH_KEY_PREFIX "%'" \
	"   ) sj ON sj.dedupe_key = '" AXIS_MOVIES_SEARCH_KEY_PREFIX "' || m.id::text"

/* Every outcome a search can end with (0023_movies.sql). */
static const gchar *const outcomes[] = {
	"grabbed", "nothing_found", "none_acceptable", "exhausted", "no_indexer", "indexer_error", "no_client",
	"client_error", "root_offline", "profile_unusable", "metadata_missing", "in_flight", "failed", NULL
};

typedef struct
{
	GPtrArray *owned;	/* gchar *, freed with it */
	const gchar *values[STATE_PARAMS];
} StateParams;

static void
state_params_clear(StateParams *params)
{
	g_clear_pointer(&params->owned, g_ptr_array_unref);
}

static const gchar *
keep(
	StateParams *params,
	gchar       *text
){
	g_ptr_array_add(params->owned, text);
	return text;
}

/* "{a,b}" of plain names (letters, digits, '_'), or of numbers. */
static gchar *
plain_array(GPtrArray *items)
{
	g_autofree gchar *joined = NULL;

	g_ptr_array_add(items, NULL);
	joined = g_strjoinv(",", (gchar **)items->pdata);
	g_ptr_array_remove_index(items, items->len - 1);
	return g_strdup_printf("{%s}", joined);
}

static gboolean
state_params_init(
	AxisMovies   *self,
	AxisDbConn   *conn,
	StateParams  *params,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GPtrArray *cols[6];
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	g_autoptr(GPtrArray) resolutions = g_ptr_array_new();
	g_autoptr(GPtrArray) ranks = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) attention = g_ptr_array_new();
	g_autoptr(GPtrArray) usable = g_ptr_array_new();
	AxisMovieRules rules;
	guint i;
	guint c;

	memset(params, 0, sizeof(*params));
	params->owned = g_ptr_array_new_with_free_func(g_free);

	/* The profiles, each checked as the facts query's reader checks it. */
	if (!axis_db_conn_exec(conn, "SELECT id, format, definition FROM quality_profile ORDER BY id", 0, NULL,
	        &result, error))
		return FALSE;
	for (c = 0; c < G_N_ELEMENTS(cols); c++)
		cols[c] = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisQualityProfile parsed;
		g_autofree gchar *unusable = NULL;
		gint64 format = 0;
		gboolean ok;

		memset(&parsed, 0, sizeof(parsed));
		axis_db_result_get_int64(result, i, 1, &format);
		ok = axis_quality_stored_check((gint)format, axis_db_result_get_text(result, i, 2), &parsed, &unusable);
		g_ptr_array_add(cols[0], g_strdup(axis_db_result_get_text(result, i, 0)));
		g_ptr_array_add(cols[1], g_strdup(ok ? "true" : "false"));
		g_ptr_array_add(cols[2], g_strdup(ok && parsed.upgrades ? "true" : "false"));
		g_ptr_array_add(cols[3], g_strdup(ok && parsed.upgrade_unknown ? "true" : "false"));
		g_ptr_array_add(cols[4], g_strdup_printf("%u", ok ? (guint)parsed.cutoff : 0u));
		g_ptr_array_add(cols[5], g_strdup(ok && axis_quality_above_unknown(parsed.cutoff,
			AXIS_RELEASE_RESOLUTION_NONE) ? "true" : "false"));
	}
	for (c = 0; c < G_N_ELEMENTS(cols); c++) {
		params->values[c] = keep(params, plain_array(cols[c]));
		g_ptr_array_unref(cols[c]);
	}

	/* The ladder, rung 1 first. */
	for (i = 1; i <= AXIS_QUALITY_N_LADDER; i++)
		g_ptr_array_add(names, (gpointer)axis_quality_name((AxisQuality)i));
	params->values[6] = keep(params, plain_array(names));

	/* An unknown source at each resolution. */
	for (i = AXIS_RELEASE_RESOLUTION_480P; i <= AXIS_RELEASE_RESOLUTION_2160P; i++) {
		g_ptr_array_add(resolutions, (gpointer)axis_release_resolution_name((AxisReleaseResolution)i));
		g_ptr_array_add(ranks, g_strdup_printf("%u", axis_quality_on_disk_rank(AXIS_QUALITY_UNKNOWN,
			(AxisReleaseResolution)i)));
	}
	params->values[7] = keep(params, plain_array(resolutions));
	params->values[8] = keep(params, plain_array(ranks));

	for (i = 0; outcomes[i] != NULL; i++) {
		if (axis_movie_outcome_needs_attention(outcomes[i]))
			g_ptr_array_add(attention, (gpointer)outcomes[i]);
	}
	params->values[9] = keep(params, plain_array(attention));

	axis_movies_get_rules(self, &rules);
	params->values[10] = keep(params, g_strdup(rules.today));
	params->values[11] = keep(params, g_strdup_printf("%d", MAX(rules.released_after_days, 0)));

	for (i = AXIS_VOLUME_ONLINE; i <= AXIS_VOLUME_MISMATCH; i++) {
		if (axis_volume_state_is_usable((AxisVolumeState)i))
			g_ptr_array_add(usable, (gpointer)axis_volume_state_to_string((AxisVolumeState)i));
	}
	params->values[12] = keep(params, plain_array(usable));
	return TRUE;
}

/* ── Why the films that need attention do, counted ──────────────── */

/*
 * For a film whose STATE_SQL is 'attention': why, as the branch of STATE_SQL
 * that made it so (the same order, the branches that give another state left
 * out: given 'attention', none of them was taken).  It is what
 * axis_movie_derive_attention() gives as its kind: 'import',
 * 'left_at_client', 'no_root', 'root_offline', 'profile_unusable', or the
 * last search's outcome.  Keep the two in step.
 */
#define ATTENTION_WHY_SQL \
	"CASE" \
	" WHEN g.id IS NOT NULL THEN 'import'" \
	" WHEN " OTHER_LIVE_SQL " THEN 'left_at_client'" \
	" WHEN coalesce(idl.state, iow.state) = 'blocked' THEN 'import'" \
	" WHEN m.root_id IS NULL THEN 'no_root'" \
	" WHEN NOT coalesce(v.state = ANY ($13::text[]), false) THEN 'root_offline'" \
	" WHEN NOT coalesce(p.usable, false) THEN 'profile_unusable'" \
	" ELSE s.outcome END"

static AxisAcquireAttention
attention_of_why(const gchar *why)
{
	if (g_strcmp0(why, "import") == 0)
		return AXIS_ACQUIRE_ATTENTION_IMPORT;
	if (g_strcmp0(why, "left_at_client") == 0)
		return AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT;
	if (g_strcmp0(why, "no_root") == 0)
		return AXIS_ACQUIRE_ATTENTION_NO_ROOT;
	if (g_strcmp0(why, "root_offline") == 0)
		return AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE;
	if (g_strcmp0(why, "profile_unusable") == 0)
		return AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE;
	return axis_acquire_attention_of_outcome(why);
}

gboolean
axis_movies_count_attention(
	AxisMovies  *self,
	guint        timeout_ms,
	guint64     *counts,
	gint64      *newest,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *set = g_strdup_printf("SET LOCAL statement_timeout = '%ums'", MAX(timeout_ms, 1));
	StateParams state;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && counts != NULL && newest != NULL, FALSE);

	memset(counts, 0, sizeof(guint64) * AXIS_MOVIES_ATTENTION_KINDS);
	memset(newest, 0, sizeof(gint64) * AXIS_MOVIES_ATTENTION_KINDS);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The state's parameters (the quality profiles, a handful of rows), then
	 * the one statement over the films, cancelled by the server when it runs
	 * out of its time. */
	if (!state_params_init(self, conn, &state, error)) {
		state_params_clear(&state);
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	ok = axis_db_conn_begin(conn, error);
	if (ok) {
		ok = axis_db_conn_exec(conn, set, 0, NULL, NULL, error) &&
			axis_db_conn_exec(conn, "SELECT why, count(*), max(id) FROM (SELECT m.id, " STATE_SQL " AS st, "
				ATTENTION_WHY_SQL " AS why" STATE_FROM ") x WHERE st = 'attention' GROUP BY why", STATE_PARAMS,
				state.values, &result, error) &&
			axis_db_conn_commit(conn, error);
		if (!ok)
			axis_db_conn_rollback(conn, NULL);
	}
	state_params_clear(&state);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisAcquireAttention kind = attention_of_why(axis_db_result_get_text(result, i, 0));
		gint64 count = 0;
		gint64 id = 0;

		axis_db_result_get_int64(result, i, 1, &count);
		axis_db_result_get_int64(result, i, 2, &id);
		if ((guint)kind >= AXIS_MOVIES_ATTENTION_KINDS)
			kind = AXIS_ACQUIRE_ATTENTION_NONE;
		counts[kind] += (guint64)MAX(count, 0);
		newest[kind] = MAX(newest[kind], id);
	}
	return TRUE;
}

/* ── The page ───────────────────────────────────────────────────── */

void
axis_movies_list_page_clear(AxisMoviesListPage *page)
{
	if (page == NULL)
		return;
	g_clear_pointer(&page->films, g_ptr_array_unref);
	g_clear_pointer(&page->states, g_array_unref);
	g_clear_pointer(&page->next, g_free);
	g_clear_pointer(&page->previous, g_free);
	memset(page, 0, sizeof(*page));
}

/* The states of @mask as a text[] of their names. */
static gchar *
states_array(guint mask)
{
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	guint i;

	for (i = 0; i < AXIS_MOVIE_N_STATES; i++) {
		if ((mask & (1u << i)) != 0)
			g_ptr_array_add(names, (gpointer)axis_movie_state_name((AxisMovieState)i));
	}
	return plain_array(names);
}

/* A statement's parameters as it is put together: each placeholder is
 * numbered as it is added. */
typedef struct
{
	GPtrArray *values;	/* const gchar *, borrowed */
	GPtrArray *texts;	/* the placeholders made, freed with it */
} Params;

static void
params_init(Params *params)
{
	params->values = g_ptr_array_new();
	params->texts = g_ptr_array_new_with_free_func(g_free);
}

static void
params_clear(Params *params)
{
	g_clear_pointer(&params->values, g_ptr_array_unref);
	g_clear_pointer(&params->texts, g_ptr_array_unref);
}

/* "$n::type" for @value (nullable), added. */
static const gchar *
param(
	Params      *params,
	const gchar *value,
	const gchar *type
){
	gchar *text;

	g_ptr_array_add(params->values, (gpointer)value);
	text = g_strdup_printf("$%u::%s", params->values->len, type);
	g_ptr_array_add(params->texts, text);
	return text;
}

/* The key of what was typed, or %NULL for no name filter. */
static gchar *
name_key(const gchar *typed)
{
	gchar *key = typed != NULL ? axis_release_title_key(typed) : NULL;

	if (key != NULL && key[0] == '\0')
		g_clear_pointer(&key, g_free);
	return key;
}

static gboolean
read_counts(
	AxisDbConn          *conn,
	StateParams         *state,
	const gchar         *key,
	AxisMoviesListPage  *page,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[STATE_PARAMS + 1];
	guint i;

	memcpy(params, state->values, sizeof(state->values));
	params[STATE_PARAMS] = key;
	/* The films by state; the movies on disk (all in the library); and the
	 * units identification holds, which are in no entry yet. */
	if (!axis_db_conn_exec(conn, "SELECT st, count(*) FROM (SELECT " STATE_SQL " AS st" STATE_FROM
	        " WHERE ($14::text IS NULL OR t.name_keys LIKE '%' || $14::text || '%')) x GROUP BY st"
	        " UNION ALL SELECT 'library', count(*)" AXIS_MOVIES_LIBRARY_FROM
	        " WHERE ($14::text IS NULL OR t.name_keys LIKE '%' || $14::text || '%')"
	        " UNION ALL SELECT ri.state, count(*) FROM library_identity ri JOIN library_root rr ON rr.id = ri.root_id"
	        " AND rr.media_type = 'movies' WHERE ri.state IN ('review', 'waiting') GROUP BY ri.state",
	        STATE_PARAMS + 1, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *name = axis_db_result_get_text(result, i, 0);
		AxisMovieState found;
		gint64 count = 0;
		guint n;

		axis_db_result_get_int64(result, i, 1, &count);
		n = (guint)CLAMP(count, 0, G_MAXUINT);
		if (g_strcmp0(name, "library") == 0)
			page->library = n;
		else if (g_strcmp0(name, "review") == 0)
			page->review = n;
		else if (g_strcmp0(name, "waiting") == 0)
			page->waiting = n;
		else if (axis_movie_state_from_name(name, &found))
			page->counts[found] = n;
	}
	page->counts[AXIS_MOVIE_IN_LIBRARY] += page->library;
	return TRUE;
}

/*
 * The statement of a page's entries: the film's id (NULL for a movie on
 * disk), the title's, the order's keys and its last key (as text, for the
 * cursor), in the list's order (or the other way round, @backwards, for the
 * page before a cursor), one more than @limit_text says.  The state of a
 * film is worked out only when one is asked for (@wanted), with the state's
 * parameters first ($1 to $13).  With @library, the movies on disk too: each
 * side ordered and cut to the page on its own (the films' on their index),
 * then the two merged.  @params collects the values.
 */
typedef struct
{
	AxisMoviesSort  sort;
	gboolean        backwards;
	gboolean        library;
	const gchar    *key;
	const gchar    *wanted;
	const gchar    *c1;
	const gchar    *c2;
	const gchar    *last;
	const gchar    *limit;
} PageSpec;

/* An entry of a page: a film (its id), or a movie on disk (0, and its
 * title). */
typedef struct
{
	gint64 movie_id;
	gint64 title_id;
} Entry;

/* The cursor's terms, made once and used by both sides. */
typedef struct
{
	const gchar *op;
	const gchar *dir;
	const gchar *name;	/* (nullable) the name filter's placeholder */
	const gchar *a;		/* (nullable) the cursor's placeholders */
	const gchar *b;
	const gchar *c;
	const gchar *limit;
} Terms;

/* " AND (k1, k2, last) op (a, b, c) ORDER BY ... LIMIT n" of one side. */
static void
side_tail(
	GString     *sql,
	const Terms *terms,
	const gchar *k1,
	const gchar *k2,
	const gchar *last
){
	if (terms->name != NULL)
		g_string_append_printf(sql, " AND t.name_keys LIKE '%%' || %s || '%%'", terms->name);
	if (terms->c != NULL && k2 != NULL)
		g_string_append_printf(sql, " AND (%s, %s, %s) %s (%s, %s, %s)", k1, k2, last, terms->op, terms->a, terms->b,
			terms->c);
	else if (terms->c != NULL)
		g_string_append_printf(sql, " AND (%s, %s) %s (%s, %s)", k1, last, terms->op, terms->a, terms->c);
	if (k2 != NULL)
		g_string_append_printf(sql, " ORDER BY %s %s, %s %s, %s %s", k1, terms->dir, k2, terms->dir, last, terms->dir);
	else
		g_string_append_printf(sql, " ORDER BY %s %s, %s %s", k1, terms->dir, last, terms->dir);
	g_string_append_printf(sql, " LIMIT %s", terms->limit);
}

/* The films' side: "SELECT m.id, t.id, <keys> FROM ... WHERE ...", the
 * keys as text (@typed: as they are, named o1, o2, o3). */
static void
films_side(
	GString           *sql,
	const PageSpec    *spec,
	const StateParams *state,
	Params            *params,
	gboolean           typed
){
	const gchar *k1 = orders[spec->sort].k1;
	const gchar *k2 = orders[spec->sort].k2;
	const gchar *last = orders[spec->sort].last;
	guint i;

	if (typed)
		g_string_append_printf(sql, "SELECT m.id AS movie_id, t.id AS title_id, %s AS o1, %s AS o2, %s AS o3", k1,
			k2 != NULL ? k2 : "0", last);
	else
		g_string_append_printf(sql, "SELECT m.id, t.id, %s::text, %s::text, %s::text", k1, k2 != NULL ? k2 : "''",
			last);
	if (spec->wanted != NULL) {
		for (i = 0; i < STATE_PARAMS; i++)
			g_ptr_array_add(params->values, (gpointer)state->values[i]);
		/* (Appended as it is: it holds a '%'.) */
		g_string_append(sql, STATE_FROM " WHERE (" STATE_SQL ") = ANY (");
		g_string_append(sql, param(params, spec->wanted, "text[]"));
		g_string_append_c(sql, ')');
	} else {
		g_string_append(sql, " FROM movie m JOIN meta_title t ON t.id = m.title_id WHERE true");
	}
}

static gchar *
page_statement(
	const PageSpec    *spec,
	const StateParams *state,
	Params            *params
){
	GString *sql = g_string_new(NULL);
	gboolean descending = orders[spec->sort].descending != spec->backwards;
	const gchar *k2 = orders[spec->sort].k2;
	Terms terms;

	memset(&terms, 0, sizeof(terms));
	terms.op = descending ? "<" : ">";
	terms.dir = descending ? "DESC" : "ASC";
	if (!spec->library) {
		/* The films alone: the statement their index serves. */
		films_side(sql, spec, state, params, FALSE);
	} else {
		g_string_append(sql, "SELECT e.movie_id, e.title_id, e.o1::text, e.o2::text, e.o3::text FROM ((");
		films_side(sql, spec, state, params, TRUE);
	}
	if (spec->key != NULL)
		terms.name = param(params, spec->key, "text");
	if (spec->last != NULL) {
		terms.a = param(params, spec->c1, orders[spec->sort].k1_type);
		if (k2 != NULL)
			terms.b = param(params, spec->c2, orders[spec->sort].k2_type);
		terms.c = param(params, spec->last, "bigint");
	}
	terms.limit = param(params, spec->limit, "integer");
	side_tail(sql, &terms, orders[spec->sort].k1, k2, orders[spec->sort].last);
	if (spec->library) {
		/* The movies on disk, in the same keys, then the two merged. */
		g_string_append_printf(sql, ") UNION ALL (SELECT NULL::bigint, t.id, %s, %s, %s", orders[spec->sort].lk1,
			orders[spec->sort].lk2 != NULL ? orders[spec->sort].lk2 : "0", orders[spec->sort].llast);
		g_string_append(sql, AXIS_MOVIES_LIBRARY_FROM " WHERE true");
		side_tail(sql, &terms, orders[spec->sort].lk1, orders[spec->sort].lk2, orders[spec->sort].llast);
		g_string_append_printf(sql, ")) e ORDER BY e.o1 %s, e.o2 %s, e.o3 %s LIMIT %s", terms.dir, terms.dir, terms.dir,
			terms.limit);
	}
	return g_string_free(sql, FALSE);
}

/* What a page is asked for, as a statement's terms (strings owned by
 * @owned).  A cursor that does not parse is no cursor. */
static void
page_spec(
	const AxisMoviesListQuery *query,
	PageSpec                  *spec,
	GPtrArray                 *owned
){
	gchar *c1 = NULL;
	gchar *c2 = NULL;
	gchar *last = NULL;
	guint limit = CLAMP(query->limit, 1, AXIS_MOVIES_PAGE_MAX);

	memset(spec, 0, sizeof(*spec));
	spec->sort = (guint)query->sort < G_N_ELEMENTS(orders) ? query->sort : AXIS_MOVIES_SORT_ADDED;
	/* A movie on disk is in the library, and only in the library. */
	spec->library = !query->managed_only && (query->states == 0 ||
		(query->states & (1u << AXIS_MOVIE_IN_LIBRARY)) != 0);
	spec->key = name_key(query->name);
	if (spec->key != NULL)
		g_ptr_array_add(owned, (gpointer)spec->key);
	if (query->states != 0) {
		spec->wanted = states_array(query->states);
		g_ptr_array_add(owned, (gpointer)spec->wanted);
	}
	if (query->after != NULL && cursor_parse(query->after, spec->sort, &c1, &c2, &last)) {
		spec->backwards = FALSE;
	} else if (query->before != NULL && cursor_parse(query->before, spec->sort, &c1, &c2, &last)) {
		spec->backwards = TRUE;
	}
	if (last != NULL) {
		spec->c1 = c1;
		spec->c2 = c2;
		spec->last = last;
		g_ptr_array_add(owned, c1);
		g_ptr_array_add(owned, c2);
		g_ptr_array_add(owned, last);
	}
	spec->limit = g_strdup_printf("%u", limit + 1);
	g_ptr_array_add(owned, (gpointer)spec->limit);
}

gboolean
axis_movies_list_explain(
	AxisMovies                *self,
	const AxisMoviesListQuery *query,
	gchar                    **plan,
	GError                   **error
){
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *explain = NULL;
	StateParams state;
	PageSpec spec;
	Params params;
	AxisDbConn *conn;
	gboolean ok;

	page_spec(query, &spec, owned);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!state_params_init(self, conn, &state, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	params_init(&params);
	sql = page_statement(&spec, &state, &params);
	explain = g_strconcat("EXPLAIN (ANALYZE, FORMAT JSON) ", sql, NULL);
	ok = axis_db_conn_exec(conn, explain, params.values->len, (const gchar *const *)params.values->pdata, &result,
		error);
	params_clear(&params);
	state_params_clear(&state);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*plan = g_strdup(axis_db_result_get_text(result, 0, 0));
	return TRUE;
}

gboolean
axis_movies_list_page(
	AxisMovies                *self,
	const AxisMoviesListQuery *query,
	AxisMoviesListPage        *page,
	GError                   **error
){
	guint limit = CLAMP(query->limit, 1, AXIS_MOVIES_PAGE_MAX);
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *sql = NULL;
	g_autoptr(AxisDbResult) ids = NULL;
	g_autoptr(GArray) entries = g_array_new(FALSE, FALSE, sizeof(Entry));
	g_autoptr(GPtrArray) film_ids = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) title_ids = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) facts = NULL;
	g_autoptr(GPtrArray) library = NULL;
	g_autoptr(GHashTable) by_id = NULL;
	g_autoptr(GHashTable) by_title = NULL;
	Params params;
	StateParams state;
	PageSpec spec;
	AxisMovieRules rules;
	AxisDbConn *conn;
	gboolean more;
	gboolean ok;
	guint n;
	guint i;

	g_return_val_if_fail(self != NULL && query != NULL && page != NULL, FALSE);

	memset(page, 0, sizeof(*page));
	page_spec(query, &spec, owned);
	/* Titles stored before their keys existed get them now (a no-op once
	 * they all have them). */
	if (spec.key != NULL) {
		guint filled = 0;

		do {
			if (!axis_meta_fill_name_keys(self->meta, 2000, &filled, error))
				return FALSE;
		} while (filled == 2000);
	}

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!state_params_init(self, conn, &state, error) || !read_counts(conn, &state, spec.key, page, error)) {
		state_params_clear(&state);
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}

	/* The page's ids, in order, and one more to know whether there is a
	 * next. */
	params_init(&params);
	sql = page_statement(&spec, &state, &params);
	ok = axis_db_conn_exec(conn, sql, params.values->len, (const gchar *const *)params.values->pdata, &ids, error);
	params_clear(&params);
	state_params_clear(&state);
	if (!ok) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	n = MIN(axis_db_result_get_n_rows(ids), limit);
	more = axis_db_result_get_n_rows(ids) > limit;
	for (i = 0; i < n; i++) {
		Entry entry;

		/* Read backwards: put the rows in the list's order. */
		guint row = spec.backwards ? n - 1 - i : i;

		entry.movie_id = 0;
		entry.title_id = 0;
		if (!axis_db_result_is_null(ids, row, 0)) {
			axis_db_result_get_int64(ids, row, 0, &entry.movie_id);
			g_ptr_array_add(film_ids, g_strdup(axis_db_result_get_text(ids, row, 0)));
		} else {
			g_ptr_array_add(title_ids, g_strdup(axis_db_result_get_text(ids, row, 1)));
		}
		axis_db_result_get_int64(ids, row, 1, &entry.title_id);
		g_array_append_val(entries, entry);
	}
	if (n > 0) {
		guint first = spec.backwards ? n - 1 : 0;
		guint last = spec.backwards ? 0 : n - 1;

		/* Forwards: a next page when there was one more row; a previous
		 * page whenever this is not the first.  Backwards: the other way. */
		if (!spec.backwards ? more : TRUE)
			page->next = cursor_make(spec.sort, axis_db_result_get_text(ids, last, 2),
				axis_db_result_get_text(ids, last, 3), axis_db_result_get_text(ids, last, 4));
		if (!spec.backwards ? spec.last != NULL : more)
			page->previous = cursor_make(spec.sort, axis_db_result_get_text(ids, first, 2),
				axis_db_result_get_text(ids, first, 3), axis_db_result_get_text(ids, first, 4));
	}

	/* The page's facts, and nothing else's: one statement for its films, one
	 * for its movies on disk. */
	page->films = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_facts_free);
	page->states = g_array_new(FALSE, FALSE, sizeof(AxisMovieState));
	ok = TRUE;
	if (film_ids->len > 0) {
		g_autofree gchar *id_array = plain_array(film_ids);

		ok = axis_movies_store_facts(conn, AXIS_MOVIES_FACTS_IDS, id_array, &facts, error);
	}
	if (ok && title_ids->len > 0) {
		g_autofree gchar *id_array = plain_array(title_ids);

		ok = axis_movies_store_library(conn, id_array, &library, error);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_movies_list_page_clear(page);
		return FALSE;
	}
	page->facts_read = (facts != NULL ? facts->len : 0) + (library != NULL ? library->len : 0);
	by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	by_title = g_hash_table_new(g_int64_hash, g_int64_equal);
	for (i = 0; facts != NULL && i < facts->len; i++) {
		AxisMovieFacts *f = g_ptr_array_index(facts, i);

		g_hash_table_insert(by_id, &f->id, f);
	}
	for (i = 0; library != NULL && i < library->len; i++) {
		AxisMovieFacts *f = g_ptr_array_index(library, i);

		g_hash_table_insert(by_title, &f->title_id, f);
	}
	axis_movies_get_rules(self, &rules);
	for (i = 0; i < entries->len; i++) {
		Entry *entry = &g_array_index(entries, Entry, i);
		GHashTable *table = entry->movie_id > 0 ? by_id : by_title;
		gint64 key = entry->movie_id > 0 ? entry->movie_id : entry->title_id;
		AxisMovieFacts *f = g_hash_table_lookup(table, &key);
		AxisMovieState found;

		/* Removed, added or gone from disk between the statements: not
		 * shown. */
		if (f == NULL)
			continue;
		g_hash_table_remove(table, &key);
		found = axis_movie_derive(f, &rules, NULL);
		g_ptr_array_add(page->films, f);
		g_array_append_val(page->states, found);
	}
	/* The facts the page did not take (none, but for a race) are freed with
	 * their arrays; those taken now belong to the page. */
	if (facts != NULL) {
		g_ptr_array_set_free_func(facts, NULL);
		for (i = 0; i < facts->len; i++) {
			AxisMovieFacts *f = g_ptr_array_index(facts, i);

			if (g_hash_table_contains(by_id, &f->id))
				axis_movie_facts_free(f);
		}
	}
	if (library != NULL) {
		g_ptr_array_set_free_func(library, NULL);
		for (i = 0; i < library->len; i++) {
			AxisMovieFacts *f = g_ptr_array_index(library, i);

			if (g_hash_table_contains(by_title, &f->title_id))
				axis_movie_facts_free(f);
		}
	}
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++) {
		if (query->states == 0 || (query->states & (1u << i)) != 0)
			page->matching += page->counts[i];
	}
	/* The films alone: the movies on disk are not matched. */
	if (!spec.library && (query->states == 0 || (query->states & (1u << AXIS_MOVIE_IN_LIBRARY)) != 0))
		page->matching -= MIN(page->library, page->matching);
	return TRUE;
}

gboolean
axis_movies_list_states(
	AxisMovies  *self,
	GHashTable **states,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	StateParams state;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = state_params_init(self, conn, &state, error) && axis_db_conn_exec(conn, "SELECT m.id, " STATE_SQL
		STATE_FROM, STATE_PARAMS, state.values, &result, error);
	state_params_clear(&state);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*states = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 *id = g_new(gint64, 1);
		AxisMovieState found = AXIS_MOVIE_WANTED;

		axis_db_result_get_int64(result, i, 0, id);
		axis_movie_state_from_name(axis_db_result_get_text(result, i, 1), &found);
		g_hash_table_insert(*states, id, GINT_TO_POINTER(found));
	}
	return TRUE;
}
