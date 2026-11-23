/*
 * axis-meta-refresh.c - keeping what is referred to current: the refresh
 * job, its request from the page, and the page's figures
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-private.h"

#include <string.h>

#include "db/axis-db-result.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"

#define EPOCH_US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* " OR t.id IN (...)" for every reference a module added. */
static gchar *
referenced_clause(AxisMeta *self)
{
	GString *out = g_string_new("t.viewed_at > now() - make_interval(secs => $2::double precision)");
	guint i;

	for (i = 0; i < self->references->len; i++) {
		const AxisMetaReference *reference = g_ptr_array_index(self->references, i);

		g_string_append_printf(out, " OR t.id IN (%s)", reference->sql);
	}
	return g_string_free(out, FALSE);
}

gchar *
axis_meta_referenced_sql(AxisMeta *self)
{
	GString *out = g_string_new("SELECT r.id FROM ((SELECT NULL::bigint WHERE false)");
	guint i;

	g_return_val_if_fail(self != NULL, g_string_free(out, FALSE));

	for (i = 0; i < self->references->len; i++) {
		const AxisMetaReference *reference = g_ptr_array_index(self->references, i);

		g_string_append_printf(out, " UNION ALL (%s)", reference->sql);
	}
	g_string_append(out, ") AS r (id) WHERE r.id IS NOT NULL");
	return g_string_free(out, FALSE);
}

typedef struct
{
	gint64        id;
	AxisMetaKind  kind;
	gchar        *provider_id;
	gint          number;	/* a season's */
	gchar        *source;	/* a season's source of numbering */
} Stale;

static void
stale_free(Stale *stale)
{
	g_free(stale->source);
	g_free(stale->provider_id);
	g_free(stale);
}

static gboolean
stale_titles(
	AxisMeta              *self,
	AxisDbConn            *conn,
	AxisMetaProviderEntry *entry,
	GPtrArray            **out,
	GError               **error
){
	AxisMetaFreshness freshness;
	g_autofree gchar *referenced = referenced_clause(self);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *viewed = NULL;
	g_autofree gchar *ended = NULL;
	g_autofree gchar *running = NULL;
	g_autofree gchar *released = NULL;
	g_autofree gchar *batch = g_strdup_printf("%d", AXIS_META_REFRESH_BATCH);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[6];
	guint i;

	axis_meta_get_freshness(self, &freshness);
	viewed = g_strdup_printf("%" G_GINT64_FORMAT, freshness.viewed);
	ended = g_strdup_printf("%" G_GINT64_FORMAT, freshness.ended);
	running = g_strdup_printf("%" G_GINT64_FORMAT, freshness.unreleased);
	released = g_strdup_printf("%" G_GINT64_FORMAT, freshness.released);
	/* The same periods as axis_meta_title_period(). */
	sql = g_strdup_printf("SELECT t.id, t.kind, t.provider_id FROM meta_title t WHERE t.provider = $1 AND (%s)"
		" AND (t.detail_fetched_at IS NULL OR t.detail_fetched_at < now() - make_interval(secs => (CASE"
		"   WHEN t.kind = 'series' AND t.status = 'ended' THEN $3"
		"   WHEN t.kind = 'series' THEN $4"
		"   WHEN t.status = 'released' OR (t.status IS NULL AND t.release_date <= current_date) THEN $5"
		"   ELSE $4 END)::double precision))"
		" ORDER BY coalesce(t.detail_fetched_at, '-infinity'::timestamptz), t.id LIMIT $6::integer", referenced);
	params[0] = entry->info->name;
	params[1] = viewed;
	params[2] = ended;
	params[3] = running;
	params[4] = released;
	params[5] = batch;
	if (!axis_db_conn_exec(conn, sql, 6, params, &result, error))
		return FALSE;
	*out = g_ptr_array_new_with_free_func((GDestroyNotify)stale_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Stale *stale = g_new0(Stale, 1);

		axis_db_result_get_int64(result, i, 0, &stale->id);
		axis_meta_kind_from_string(axis_db_result_get_text(result, i, 1), &stale->kind);
		stale->provider_id = g_strdup(axis_db_result_get_text(result, i, 2));
		g_ptr_array_add(*out, stale);
	}
	return TRUE;
}

static gboolean
stale_seasons(
	AxisMeta              *self,
	AxisDbConn            *conn,
	AxisMetaProviderEntry *entry,
	GPtrArray            **out,
	GError               **error
){
	AxisMetaFreshness freshness;
	g_autofree gchar *referenced = referenced_clause(self);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *viewed = NULL;
	g_autofree gchar *current = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *batch = g_strdup_printf("%d", AXIS_META_REFRESH_BATCH);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[5];
	guint i;

	axis_meta_get_freshness(self, &freshness);
	viewed = g_strdup_printf("%" G_GINT64_FORMAT, freshness.viewed);
	current = g_strdup_printf("%" G_GINT64_FORMAT, freshness.current_season);
	other = g_strdup_printf("%" G_GINT64_FORMAT, freshness.released);
	/* Each source's seasons apart, asked of the source's own provider with
	 * that provider's identifier of the series (add-metadata-tvdb). */
	/* One identifier a series and source, always the same one: the record's
	 * own when this provider made it, else the provider's identifier the
	 * title holds (one a scheme; the least, should there be more). */
	sql = g_strdup_printf("SELECT q.title_id, q.number, q.pid, q.source FROM (SELECT s.title_id, s.number, s.source,"
		" s.episodes_fetched_at, coalesce(CASE WHEN t.provider = $1 THEN t.provider_id END,"
		"  (SELECT min(x.value) FROM meta_external_id x WHERE x.title_id = s.title_id AND x.kind = 'series'"
		"   AND x.scheme = $1)) AS pid"
		" FROM meta_season s JOIN meta_title t ON t.id = s.title_id"
		" WHERE (s.source = $1 OR s.source LIKE $1 || '-%%') AND (%s)"
		" AND s.episodes_fetched_at IS NOT NULL AND s.episodes_fetched_at < now() - make_interval(secs => (CASE"
		"   WHEN t.status IS DISTINCT FROM 'ended'"
		"    AND s.number = (SELECT max(m.number) FROM meta_season m WHERE m.title_id = s.title_id"
		"     AND m.source = s.source) THEN $3"
		"   ELSE $4 END)::double precision)) q WHERE q.pid IS NOT NULL"
		" ORDER BY q.episodes_fetched_at, q.title_id, q.number LIMIT $5::integer", referenced);
	params[0] = entry->info->name;
	params[1] = viewed;
	params[2] = current;
	params[3] = other;
	params[4] = batch;
	if (!axis_db_conn_exec(conn, sql, 5, params, &result, error))
		return FALSE;
	*out = g_ptr_array_new_with_free_func((GDestroyNotify)stale_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Stale *stale = g_new0(Stale, 1);
		gint64 number = 0;

		axis_db_result_get_int64(result, i, 0, &stale->id);
		axis_db_result_get_int64(result, i, 1, &number);
		stale->number = (gint)number;
		stale->kind = AXIS_META_KIND_SERIES;
		stale->provider_id = g_strdup(axis_db_result_get_text(result, i, 2));
		stale->source = g_strdup(axis_db_result_get_text(result, i, 3));
		/* An order is fetched whole: once per series and source. */
		if (entry->info->order != NULL) {
			gboolean twice = FALSE;
			guint j;

			for (j = 0; j < (*out)->len && !twice; j++) {
				const Stale *before = g_ptr_array_index(*out, j);

				twice = before->id == stale->id && g_strcmp0(before->source, stale->source) == 0;
			}
			if (twice) {
				stale_free(stale);
				continue;
			}
		}
		g_ptr_array_add(*out, stale);
	}
	return TRUE;
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

/* A title its provider no longer has keeps its record, and is not asked
 * about again until it is stale once more. */
static void
mark_checked(
	AxisMeta *self,
	gint64    id
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, NULL);

	if (conn == NULL)
		return;
	params[0] = text;
	(void)axis_db_conn_exec(conn, "UPDATE meta_title SET detail_fetched_at = now() WHERE id = $1::bigint", 1,
		params, NULL, NULL);
	axis_db_pool_release(self->pool, conn);
}

static gboolean
refresh_provider(
	AxisMeta               *self,
	AxisMetaProviderEntry  *entry,
	AxisJobContext         *context,
	AxisMetaRefreshReport  *report,
	gdouble                 base,
	gdouble                 share,
	GError                **error
){
	GCancellable *cancellable = context != NULL ? axis_job_context_get_cancellable(context) : self->cancel;
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GPtrArray) seasons = NULL;
	AxisDbConn *conn;
	gboolean ok;
	guint total;
	guint done = 0;
	guint i;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = stale_titles(self, conn, entry, &titles, error) && stale_seasons(self, conn, entry, &seasons, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	total = titles->len + seasons->len;
	for (i = 0; i < titles->len; i++) {
		const Stale *stale = g_ptr_array_index(titles, i);
		AxisMetaPending *pending = axis_meta_pending_new(AXIS_META_ASK_TITLE, entry);
		g_autoptr(GError) local = NULL;

		if (context != NULL && axis_job_context_is_cancelled(context))
			break;
		pending->kind = stale->kind;
		pending->text = g_strdup(stale->provider_id);
		if (axis_meta_ask_now(self, pending, cancellable, &local)) {
			report->titles++;
		} else if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND)) {
			mark_checked(self, stale->id);
		} else {
			report->failed++;
			if (provider_gone(local)) {
				g_info("metadata refresh: %s is left for the next run: %s", entry->info->label, local->message);
				return TRUE;
			}
		}
		done++;
		if (context != NULL)
			axis_job_context_set_progress(context, base + share * done / MAX(total, 1u), entry->info->label);
	}
	for (i = 0; i < seasons->len; i++) {
		const Stale *stale = g_ptr_array_index(seasons, i);
		gboolean whole = entry->info->order != NULL;
		AxisMetaPending *pending;
		g_autoptr(GError) local = NULL;

		if (context != NULL && axis_job_context_is_cancelled(context))
			break;
		/* A season of the provider's own numbering, or a whole order. */
		if (!whole && g_strcmp0(stale->source, entry->info->name) != 0)
			continue;
		pending = axis_meta_pending_new(whole ? AXIS_META_ASK_ORDER : AXIS_META_ASK_SEASON, entry);
		pending->kind = AXIS_META_KIND_SERIES;
		pending->text = g_strdup(stale->provider_id);
		pending->number = stale->number;
		pending->query = g_strdup(stale->source);
		if (axis_meta_ask_now(self, pending, cancellable, &local)) {
			report->seasons++;
		} else {
			report->failed++;
			if (provider_gone(local)) {
				g_info("metadata refresh: %s is left for the next run: %s", entry->info->label, local->message);
				return TRUE;
			}
		}
		done++;
		if (context != NULL)
			axis_job_context_set_progress(context, base + share * done / MAX(total, 1u), entry->info->label);
	}
	return TRUE;
}

gboolean
axis_meta_run_refresh(
	AxisMeta               *self,
	AxisJobContext         *context,
	AxisMetaRefreshReport  *report_out,
	GError                **error
){
	AxisMetaRefreshReport report;
	AxisMetaFreshness freshness;
	g_autoptr(AxisDbResult) pruned = NULL;
	AxisDbConn *conn;
	guint n = self->providers->len;
	guint i;

	memset(&report, 0, sizeof(report));
	if (report_out != NULL)
		memset(report_out, 0, sizeof(*report_out));
	axis_meta_get_freshness(self, &freshness);
	for (i = 0; i < n; i++) {
		AxisMetaProviderEntry *entry = g_ptr_array_index(self->providers, i);
		AxisMetaLimitStatus status;
		gchar *key;
		gboolean held;

		if (context != NULL && axis_job_context_is_cancelled(context))
			break;
		memset(&status, 0, sizeof(status));
		axis_meta_limiter_get_status(entry->limiter, &status);
		held = status.paused_until_us != 0 || status.backoff_until_us != 0;
		axis_meta_limit_status_clear(&status);
		key = entry->key_setting != NULL ? axis_meta_provider_key(self, entry) : NULL;
		if (held || (entry->key_setting != NULL && key == NULL)) {
			report.providers_skipped++;
			axis_meta_secret_free(key);
			continue;
		}
		axis_meta_secret_free(key);
		if (!refresh_provider(self, entry, context, &report, (gdouble)i / n, 1.0 / n, error))
			return FALSE;
	}
	if (context != NULL && axis_job_context_is_cancelled(context)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
		return FALSE;
	}

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	{
		g_autofree gchar *keep = g_strdup_printf("%d", AXIS_META_SEARCH_KEEP_S);
		const gchar *params[1];

		params[0] = keep;
		if (!axis_db_conn_exec(conn, "DELETE FROM meta_search WHERE fetched_at < now() - make_interval(secs =>"
		        " $1::double precision)", 1, params, &pruned, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
		report.searches_pruned = (guint)axis_db_result_get_affected_rows(pruned);
	}
	axis_db_pool_release(self->pool, conn);
	if (report.titles + report.seasons + report.failed > 0)
		g_info("metadata refresh: %u titles and %u seasons fetched again, %u failed, %u providers left alone",
			report.titles, report.seasons, report.failed, report.providers_skipped);
	if (report_out != NULL)
		*report_out = report;
	return TRUE;
}

gboolean
axis_meta_request_refresh(
	AxisMeta  *self,
	gint64    *job_id,
	gboolean  *created,
	GError   **error
){
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_META_REFRESH_KIND;
	spec.queue = AXIS_META_QUEUE;
	spec.priority = 10;
	spec.dedupe_key = AXIS_META_REFRESH_KIND ":request";
	spec.max_attempts = 1;
	ok = axis_job_enqueue(conn, &spec, job_id, created, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── The page's figures ─────────────────────────────────────────── */

void
axis_meta_figures_clear(AxisMetaFigures *figures)
{
	if (figures == NULL)
		return;
	g_clear_pointer(&figures->refresh_state, g_free);
	g_clear_pointer(&figures->refresh_error, g_free);
}

gboolean
axis_meta_load_figures(
	AxisMeta         *self,
	AxisMetaFigures  *figures,
	GError          **error
){
	g_autoptr(AxisDbResult) counts = NULL;
	g_autoptr(AxisDbResult) job = NULL;
	gint64 *fields[11];
	AxisDbConn *conn;
	guint i;

	memset(figures, 0, sizeof(*figures));
	figures->artwork_cap = self->settings != NULL &&
		axis_settings_lookup(self->settings, AXIS_META_SETTING_ARTWORK_CAP, NULL) != NULL ?
		axis_settings_get_byte_size(self->settings, AXIS_META_SETTING_ARTWORK_CAP) :
		G_GINT64_CONSTANT(1024) * 1024 * 1024;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_exec(conn, "SELECT"
	        " (SELECT count(*) FROM meta_title WHERE kind = 'film'),"
	        " (SELECT count(*) FROM meta_title WHERE kind = 'series'),"
	        " (SELECT count(*) FROM meta_external_id),"
	        " (SELECT count(*) FROM meta_alt_title),"
	        " (SELECT count(*) FROM meta_season),"
	        " (SELECT count(*) FROM meta_episode),"
	        " (SELECT count(*) FROM meta_search),"
	        " (SELECT count(DISTINCT sha256) FROM meta_artwork WHERE sha256 IS NOT NULL),"
	        " (SELECT coalesce(sum(bytes), 0) FROM (SELECT DISTINCT sha256, bytes FROM meta_artwork"
	        "   WHERE sha256 IS NOT NULL) AS d),"
	        " (SELECT count(*) FROM meta_artwork WHERE sha256 IS NULL),"
	        " (SELECT coalesce(sum(pg_total_relation_size(c)), 0) FROM unnest(ARRAY['meta_title', 'meta_external_id',"
	        "   'meta_alt_title', 'meta_season', 'meta_episode', 'meta_search', 'meta_artwork']::regclass[]) AS c)",
	        0, NULL, &counts, error) ||
	    /* The latest: one queued or running first, then the one that ended
	     * last (a job a person retried keeps its older id). */
	    !axis_db_conn_exec(conn, "SELECT state, " EPOCH_US("finished_at") ", last_error FROM job"
	        " WHERE kind = '" AXIS_META_REFRESH_KIND "' ORDER BY finished_at DESC NULLS FIRST, id DESC LIMIT 1", 0,
	        NULL, &job, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	fields[0] = &figures->films;
	fields[1] = &figures->series;
	fields[2] = &figures->ids;
	fields[3] = &figures->alt_titles;
	fields[4] = &figures->seasons;
	fields[5] = &figures->episodes;
	fields[6] = &figures->searches;
	fields[7] = &figures->artwork;
	fields[8] = &figures->artwork_bytes;
	fields[9] = &figures->artwork_refused;
	fields[10] = &figures->store_bytes;
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
		axis_db_result_get_int64(counts, 0, i, fields[i]);
	if (axis_db_result_get_n_rows(job) > 0) {
		figures->refresh_state = g_strdup(axis_db_result_get_text(job, 0, 0));
		axis_db_result_get_int64(job, 0, 1, &figures->refresh_finished_us);
		if (!axis_db_result_is_null(job, 0, 2))
			figures->refresh_error = g_strdup(axis_db_result_get_text(job, 0, 2));
	}
	return TRUE;
}
