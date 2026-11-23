/*
 * axis-indexer-search.c - testing a source, searching it, fetching a
 * release, and forgetting old searches
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What was searched for in words is never logged here (and the request's
 * words are not stored: the search is keyed by a hash of the normalised
 * request).  Neither is a fetch reference or a key: errors that come from
 * a request already have both blanked out by the HTTP client, and what this
 * file writes itself is fixed text.
 */

#include "indexer/axis-indexer-private.h"

#include <string.h>

/* ── What a contact found ───────────────────────────────────────── */

gboolean
axis_indexer_is_contact_failure(const GError *error)
{
	if (error == NULL || error->domain != AXIS_INDEXER_ERROR)
		return FALSE;
	switch ((AxisIndexerError)error->code) {
	case AXIS_INDEXER_ERROR_AUTH:
	case AXIS_INDEXER_ERROR_UNREACHABLE:
	case AXIS_INDEXER_ERROR_RATE_LIMITED:
	case AXIS_INDEXER_ERROR_UNREADABLE:
	case AXIS_INDEXER_ERROR_FAILED:
		return TRUE;
	default:
		return FALSE;
	}
}

void
axis_indexer_record_contact(
	AxisIndexers *self,
	gint64        source_id,
	const GError *failure,
	const gchar  *version
){
	AxisIndexerSourceState state = AXIS_INDEXER_SOURCE_OK;
	g_autofree gchar *message = NULL;
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn;

	if (failure != NULL) {
		/* A source that asked to be left alone is not failing. */
		if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED))
			return;
		message = axis_indexer_contact_problem(failure, &state);
	}
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		g_debug("indexers: cannot record what source %" G_GINT64_FORMAT " answered: %s", source_id, local->message);
		return;
	}
	if (!axis_indexer_store_source_state(conn, source_id, state, message, version, failure == NULL, &local))
		g_debug("indexers: cannot record what source %" G_GINT64_FORMAT " answered: %s", source_id, local->message);
	axis_db_pool_release(self->pool, conn);
}

/* "12:34 UTC" of @us. */
static gchar *
clock_of(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%H:%M UTC") : g_strdup("later");
}

/* A site that asked Axis to wait is not asked by a test or a check either:
 * they say when it may be.  Axis's own holds (its pages, a challenge) are
 * what a test or a check is for, and do not stop them. */
static gboolean
site_waits(
	const AxisIndexerTarget  *target,
	GError                  **error
){
	g_autofree gchar *until = NULL;

	if (axis_indexer_hold_cause(target->info, g_get_real_time()) != AXIS_INDEXER_HOLD_WAIT)
		return FALSE;
	until = clock_of(target->info->held_until_us);
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
		"%s asked Axis to wait, so it is not asked, by a test or a check either, until %s", target->info->name, until);
	return TRUE;
}

/* ── Testing ────────────────────────────────────────────────────── */

typedef struct
{
	gchar     *version;
	GPtrArray *indexers;
	gchar     *note;	/* a native indexer's: its capabilities could not be read */
} TestData;

static gboolean
test_call(
	AxisIndexers               *self,
	const AxisIndexerTarget    *target,
	const AxisIndexerKindInfo  *kind,
	AxisIndexerConn            *conn,
	gpointer                    data,
	GError                    **error
){
	TestData *test = data;

	if (site_waits(target, error))
		return FALSE;
	if (!kind->test(kind, conn, &test->version, error) || !kind->indexers(kind, conn, &test->indexers, error))
		return FALSE;
	if (conn->has_native && conn->learned.caps_learned && conn->learned.caps.problem != NULL)
		test->note = g_strdup_printf("Its capabilities could not be read (%s), so it is searched by words only.",
			conn->learned.caps.problem);
	return TRUE;
}

gboolean
axis_indexers_test_source(
	AxisIndexers  *self,
	gint64         id,
	GCancellable  *cancellable,
	GError       **error
){
	TestData test;
	g_autoptr(GError) failure = NULL;
	gboolean ok;

	memset(&test, 0, sizeof(test));
	ok = axis_indexer_with_source(self, id, FALSE, cancellable, test_call, &test, &failure);
	if (ok) {
		AxisDbConn *conn;
		g_autoptr(GError) local = NULL;

		conn = axis_db_pool_acquire(self->pool, &local);
		if (conn != NULL) {
			/* A test that passes ends a hold for what its answers were. */
			ok = axis_indexer_store_source_state(conn, id, AXIS_INDEXER_SOURCE_OK, test.note, test.version, TRUE,
					&local) &&
				axis_indexer_store_lift_hold(conn, id, 0, &local) &&
				(test.indexers == NULL || axis_indexer_store_replace_indexers(conn, id, test.indexers, &local));
			axis_db_pool_release(self->pool, conn);
		} else {
			ok = FALSE;
		}
		g_free(test.version);
		g_free(test.note);
		g_clear_pointer(&test.indexers, g_ptr_array_unref);
		if (!ok)
			g_propagate_error(error, g_steal_pointer(&local));
		return ok;
	}
	g_free(test.version);
	g_free(test.note);
	g_clear_pointer(&test.indexers, g_ptr_array_unref);
	if (!axis_indexer_is_contact_failure(failure)) {
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	axis_indexer_record_contact(self, id, failure, NULL);
	return TRUE;
}

/* ── Checking, step by step ─────────────────────────────────────── */

typedef struct
{
	const AxisIndexerRequest *request;
	AxisDefTrace             *trace;
	guint                     n_releases;
} CheckData;

static gboolean
check_call(
	AxisIndexers               *self,
	const AxisIndexerTarget    *target,
	const AxisIndexerKindInfo  *kind,
	AxisIndexerConn            *conn,
	gpointer                    data,
	GError                    **error
){
	CheckData *check = data;
	AxisIndexerOutcome outcome;
	gboolean ok;

	if (!kind->definition) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"Only an indexer that runs a site definition can be checked step by step.");
		return FALSE;
	}
	if (site_waits(target, error))
		return FALSE;
	axis_indexer_outcome_init(&outcome);
	conn->trace = check->trace;
	ok = kind->search(kind, conn, check->request, &outcome, error);
	conn->trace = NULL;
	check->n_releases = outcome.items != NULL ? outcome.items->len : 0;
	axis_indexer_outcome_clear(&outcome);
	return ok;
}

gboolean
axis_indexers_check_source(
	AxisIndexers   *self,
	gint64          id,
	const gchar    *words,
	gboolean        excerpts,
	GCancellable   *cancellable,
	AxisDefTrace  **trace,
	GError        **error
){
	g_autofree gchar *clean = axis_indexer_text_clean(words, AXIS_INDEXER_TITLE_MAX_CHARS);
	g_autoptr(GError) failure = NULL;
	AxisIndexerRequest request;
	CheckData check;

	*trace = NULL;
	if (clean == NULL) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "Type the words to search for.");
		return FALSE;
	}
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = clean;
	memset(&check, 0, sizeof(check));
	check.request = &request;
	check.trace = axis_def_trace_new(excerpts);
	if (axis_indexer_with_source(self, id, FALSE, cancellable, check_call, &check, &failure)) {
		g_autofree gchar *outcome = g_strdup_printf("%u release%s found", check.n_releases,
			check.n_releases == 1 ? "" : "s");
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, NULL);

		axis_def_trace_set_outcome(check.trace, outcome);
		/* It answered as its definition reads: working, and not held. */
		if (conn != NULL) {
			axis_indexer_store_source_state(conn, id, AXIS_INDEXER_SOURCE_OK, NULL, NULL, TRUE, NULL);
			axis_indexer_store_lift_hold(conn, id, 0, NULL);
			axis_db_pool_release(self->pool, conn);
		}
		*trace = check.trace;
		return TRUE;
	}
	if (!axis_indexer_is_contact_failure(failure) &&
	    !g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID)) {
		axis_def_trace_free(check.trace);
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	/* What failed is the account's end; the state says so as a search's
	 * would. */
	axis_def_trace_set_outcome(check.trace, failure->message);
	if (axis_indexer_is_contact_failure(failure))
		axis_indexer_record_contact(self, id, failure, NULL);
	*trace = check.trace;
	return TRUE;
}

gboolean
axis_indexers_accept_files_origin(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
){
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisDbConn *conn;
	gboolean accepted = FALSE;
	gboolean ok;

	if (!axis_indexers_get_source(self, id, &source, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_accept_files_origin(conn, id, &accepted, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && !accepted) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"There is no other address of files to accept: test the indexer to see where its files come from.");
		return FALSE;
	}
	return ok;
}

/* ── Capabilities ───────────────────────────────────────────────── */

static gboolean
caps_call(
	AxisIndexers               *self,
	const AxisIndexerTarget    *target,
	const AxisIndexerKindInfo  *kind,
	AxisIndexerConn            *conn,
	gpointer                    data,
	GError                    **error
){
	return kind->caps(kind, conn, error);
}

gboolean
axis_indexers_refresh_caps(
	AxisIndexers  *self,
	GCancellable  *cancellable,
	guint         *n_read,
	GError       **error
){
	g_autoptr(GPtrArray) sources = NULL;
	gint64 now_us = g_get_real_time();
	guint i;

	if (n_read != NULL)
		*n_read = 0;
	if (!axis_indexers_list_sources(self, &sources, error))
		return FALSE;
	for (i = 0; i < sources->len; i++) {
		const AxisIndexerSourceInfo *source = g_ptr_array_index(sources, i);
		const AxisIndexerKindInfo *kind = axis_indexers_find_kind(self, source->kind);
		g_autoptr(GError) failure = NULL;

		if (!source->native || !source->enabled || kind == NULL || kind->caps == NULL ||
		    (source->caps.read_us > 0 && now_us - source->caps.read_us < (gint64)(AXIS_INDEXER_CAPS_STALE_S - 300) *
		     G_USEC_PER_SEC) || axis_indexer_source_spent(source, TRUE, now_us))
			continue;
		if (axis_indexer_with_source(self, source->id, TRUE, cancellable, caps_call, NULL, &failure)) {
			if (n_read != NULL)
				(*n_read)++;
			axis_indexer_record_contact(self, source->id, NULL, NULL);
			continue;
		}
		if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&failure));
			return FALSE;
		}
		if (axis_indexer_is_contact_failure(failure))
			axis_indexer_record_contact(self, source->id, failure, NULL);
	}
	return TRUE;
}

/* ── Searching ──────────────────────────────────────────────────── */

typedef struct
{
	const AxisIndexerRequest  *request;
	gchar                     *key;
	gint64                     life_s;
	AxisIndexerSearchResult   *result;
} SearchData;

/* Keeps of what a kind returned only what is still fit to store: a fetch
 * reference under the source's base path, a magnet link that is one. */
static void
recheck_outcome(
	AxisIndexerOutcome *outcome,
	const gchar        *base_path
){
	GPtrArray *kept = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	guint i;

	for (i = 0; i < outcome->items->len && kept->len < AXIS_INDEXER_MAX_RESULTS; i++) {
		AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		if (found->release == NULL || found->release->name == NULL) {
			outcome->n_skipped++;
			continue;
		}
		if (found->fetch_path != NULL && !axis_indexer_path_ok(found->fetch_path, base_path))
			g_clear_pointer(&found->fetch_path, g_free);
		if (found->magnet != NULL && (found->release->protocol != AXIS_DOWNLOAD_TORRENT ||
		    !axis_download_magnet_parse(found->magnet, NULL, NULL)))
			g_clear_pointer(&found->magnet, g_free);
		found->release->has_magnet = found->magnet != NULL;
		if (found->fetch_path == NULL && found->magnet == NULL) {
			outcome->n_unusable++;
			continue;
		}
		g_ptr_array_add(kept, found);
		outcome->items->pdata[i] = NULL;
	}
	if (i < outcome->items->len)
		outcome->truncated = TRUE;
	/* What was not moved is freed with the old array; what was, is not. */
	g_ptr_array_unref(outcome->items);
	outcome->items = kept;
}

static gboolean
search_call(
	AxisIndexers               *self,
	const AxisIndexerTarget    *target,
	const AxisIndexerKindInfo  *kind,
	AxisIndexerConn            *conn,
	gpointer                    data,
	GError                    **error
){
	SearchData *search = data;
	AxisIndexerOutcome outcome;
	g_autoptr(GPtrArray) indexers = NULL;
	g_autoptr(GPtrArray) failed = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) local = NULL;
	gint n_indexers = -1;
	gint64 now_us = g_get_real_time();
	AxisDbConn *db;
	gboolean ok;
	guint i;

	/* A native indexer that said its limit is reached is not asked before
	 * it has had time to reset, nor one whose allowance is used up. */
	if (kind->native && axis_indexer_source_spent(target->info, FALSE, now_us)) {
		g_autoptr(GDateTime) when = target->info->held_until_us > now_us ?
			g_date_time_new_from_unix_utc(target->info->held_until_us / G_USEC_PER_SEC) : NULL;
		g_autofree gchar *until = when != NULL ? g_date_time_format(when, "%H:%M UTC") : NULL;

		/* One that runs a definition: in the words of its hold's cause. */
		switch (axis_indexer_hold_cause(target->info, now_us)) {
		case AXIS_INDEXER_HOLD_DRIFT:
		case AXIS_INDEXER_HOLD_CHALLENGE:
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
				"%s, so searches pass it over until %s (a test or a check that passes ends that)",
				target->info->hold_message, until);
			return FALSE;
		case AXIS_INDEXER_HOLD_WAIT:
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
				"%s asked Axis to wait, so it is not asked again until %s", target->info->name, until);
			return FALSE;
		default:
			break;
		}
		if (until != NULL)
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
				"%s said its request limit is reached, so it is not asked again until %s", target->info->name, until);
		else
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
				"%s's daily allowance of %d API calls is used up", target->info->name,
				target->info->allowance.api_max);
		return FALSE;
	}

	/* Which indexers there are, which fail now and which identifiers each
	 * takes: read again, before the search, when what is known is old (the
	 * kind asks by the title too the indexers that take no identifier).
	 * Best effort: a search does not fail for it. */
	if (target->info->indexers_us == 0 || now_us - target->info->indexers_us > AXIS_INDEXER_LIST_STALE_S * G_USEC_PER_SEC) {
		if (kind->indexers(kind, conn, &indexers, &local) && indexers != NULL) {
			db = axis_db_pool_acquire(self->pool, NULL);
			if (db != NULL) {
				axis_indexer_store_replace_indexers(db, target->info->id, indexers, NULL);
				axis_db_pool_release(self->pool, db);
			}
		} else if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_clear_error(&local);
	}
	if (indexers == NULL)
		indexers = g_ptr_array_ref(target->info->indexers);
	conn->indexers = indexers;

	axis_indexer_outcome_init(&outcome);
	if (!kind->search(kind, conn, search->request, &outcome, error)) {
		axis_indexer_outcome_clear(&outcome);
		return FALSE;
	}
	if (outcome.not_asked != NULL) {
		/* Nothing here for this kind of search, and nothing was asked of the
		 * indexer: made now, not stored (there is nothing to keep, and a stored
		 * search would read back as an answer), and not counted as contact. */
		AxisIndexerSearchResult *none = g_new0(AxisIndexerSearchResult, 1);

		none->source_id = target->info->id;
		none->mode = outcome.mode;
		none->n_indexers = -1;
		none->created_us = now_us;
		none->expires_us = now_us;
		none->releases = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_release_free);
		none->not_asked = g_steal_pointer(&outcome.not_asked);
		search->result = none;
		axis_indexer_outcome_clear(&outcome);
		return TRUE;
	}
	if (outcome.items == NULL)
		outcome.items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	/* A native indexer's files may be anywhere on its origin (a Torznab
	 * provider serves them outside the feed's path), or on the origin
	 * accepted for them; a source's are under its base path. */
	recheck_outcome(&outcome, kind->native ? "" : target->info->base_path);

	if (indexers->len > 0) {
		n_indexers = 0;
		for (i = 0; i < indexers->len; i++) {
			const AxisIndexerInfo *info = g_ptr_array_index(indexers, i);

			if (!info->enabled)
				continue;
			n_indexers++;
			if (info->failing && failed->len < AXIS_INDEXER_MAX_FAILED)
				g_ptr_array_add(failed, g_strdup(info->name));
		}
	}

	db = axis_db_pool_acquire(self->pool, error);
	if (db == NULL) {
		axis_indexer_outcome_clear(&outcome);
		return FALSE;
	}
	ok = axis_indexer_store_put_search(db, target->info->id, search->key, &outcome, n_indexers, failed, search->life_s,
		&search->result, error);
	axis_db_pool_release(self->pool, db);
	axis_indexer_outcome_clear(&outcome);
	return ok;
}

static gboolean
cached_search(
	AxisIndexers              *self,
	gint64                     source_id,
	const gchar               *key,
	AxisIndexerSearchResult  **result,
	GError                   **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_get_search(conn, source_id, key, result, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_indexers_search(
	AxisIndexers              *self,
	gint64                     source_id,
	const AxisIndexerRequest  *request,
	GCancellable              *cancellable,
	AxisIndexerSearchResult  **result,
	GError                   **error
){
	const gchar *problem = NULL;

	g_return_val_if_fail(self != NULL && request != NULL && result != NULL, FALSE);

	*result = NULL;
	if (!axis_indexer_request_check(request, &problem)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "The search cannot be made: %s.", problem);
		return FALSE;
	}
	if (source_id == 0)
		return axis_indexer_search_all(self, request, cancellable, result, error);
	return axis_indexer_search_source(self, source_id, request, cancellable, result, error);
}

gboolean
axis_indexer_search_source(
	AxisIndexers              *self,
	gint64                     source_id,
	const AxisIndexerRequest  *request,
	GCancellable              *cancellable,
	AxisIndexerSearchResult  **result,
	GError                   **error
){
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *flight = NULL;
	SearchData search;
	gint64 wait_until_us;
	gboolean ok;

	*result = NULL;
	key = axis_indexer_request_key(request);
	if (!axis_indexers_get_source(self, source_id, &source, error))
		return FALSE;
	if (!source->enabled) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE,
			"That indexer is switched off.");
		return FALSE;
	}

	/* One search of a kind at a time: a second one waits for the first
	 * and reads its result. */
	flight = g_strdup_printf("%" G_GINT64_FORMAT ":%s", source_id, key);
	wait_until_us = g_get_monotonic_time() +
		(gint64)(self->tuning.wait_ms != 0 ? self->tuning.wait_ms : AXIS_INDEXER_WAIT_MS) * 1000 * 2;
	while (TRUE) {
		gboolean busy;

		if (!cached_search(self, source_id, key, result, error))
			return FALSE;
		if (*result != NULL)
			return TRUE;
		g_mutex_lock(&self->lock);
		busy = g_hash_table_contains(self->inflight, flight);
		if (!busy) {
			g_hash_table_add(self->inflight, g_strdup(flight));
			g_mutex_unlock(&self->lock);
			break;
		}
		if (g_atomic_int_get(&self->stopped) || g_cancellable_is_cancelled(cancellable)) {
			g_mutex_unlock(&self->lock);
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "cancelled");
			return FALSE;
		}
		if (g_get_monotonic_time() >= wait_until_us) {
			g_mutex_unlock(&self->lock);
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_BUSY,
				"the same search is being made and did not finish in time");
			return FALSE;
		}
		g_cond_wait_until(&self->inflight_done, &self->lock, MIN(wait_until_us, g_get_monotonic_time() + 200 * 1000));
		g_mutex_unlock(&self->lock);
	}

	/* The search just ended may have stored this one after it was looked for
	 * and before this call took its turn. */
	if (!cached_search(self, source_id, key, result, &failure) || *result != NULL) {
		g_mutex_lock(&self->lock);
		g_hash_table_remove(self->inflight, flight);
		g_cond_broadcast(&self->inflight_done);
		g_mutex_unlock(&self->lock);
		if (failure != NULL)
			g_propagate_error(error, g_steal_pointer(&failure));
		return failure == NULL && *result != NULL ? TRUE : FALSE;
	}

	memset(&search, 0, sizeof(search));
	search.request = request;
	search.key = key;
	search.life_s = axis_indexer_setting_duration(self, AXIS_INDEXER_SETTING_CACHE_LIFE, 900);
	ok = axis_indexer_with_source(self, source_id, TRUE, cancellable, search_call, &search, &failure);
	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->inflight, flight);
	g_cond_broadcast(&self->inflight_done);
	g_mutex_unlock(&self->lock);
	if (ok) {
		/* Nothing asked is no contact: it shows nothing of the indexer. */
		if (search.result->not_asked == NULL)
			axis_indexer_record_contact(self, source_id, NULL, NULL);
		*result = search.result;
		g_debug("indexers: a search of source %" G_GINT64_FORMAT " found %u releases", source_id,
			search.result->releases->len);
		return TRUE;
	}
	if (axis_indexer_is_contact_failure(failure))
		axis_indexer_record_contact(self, source_id, failure, NULL);
	g_propagate_error(error, g_steal_pointer(&failure));
	return FALSE;
}

/* ── Releases ───────────────────────────────────────────────────── */

gboolean
axis_indexers_get_release(
	AxisIndexers         *self,
	gint64                release_id,
	AxisIndexerRelease  **release,
	GError              **error
){
	AxisDbConn *conn;
	gboolean ok;

	*release = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_get_release(conn, release_id, release, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && *release == NULL) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND,
			"there is no such release, or its search has expired");
		return FALSE;
	}
	return ok;
}

typedef struct
{
	AxisIndexerFetchTarget *target;
	AxisIndexerFetched     *fetched;
} FetchData;

static gboolean
fetch_call(
	AxisIndexers               *self,
	const AxisIndexerTarget    *target,
	const AxisIndexerKindInfo  *kind,
	AxisIndexerConn            *conn,
	gpointer                    data,
	GError                    **error
){
	FetchData *fetch = data;
	AxisIndexerFetchRequest request;
	gint64 now_us = g_get_real_time();

	/* A native indexer that said its download limit is reached, or whose
	 * grabs are used up, is not asked for a file. */
	if (kind->native && (target->info->grabs_held_until_us > now_us ||
	    axis_indexer_allowance_grabs_spent(&target->info->allowance))) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
			"%s's daily allowance of files is used up, so nothing is fetched from it for now", target->info->name);
		return FALSE;
	}
	memset(&request, 0, sizeof(request));
	request.fetch_path = fetch->target->fetch_ref;
	request.magnet = fetch->target->magnet;
	request.protocol = fetch->target->protocol;
	request.name = fetch->target->name;
	request.info_hash = fetch->target->info_hash;
	request.files_origin = fetch->target->files_origin;
	return kind->fetch(kind, conn, &request, fetch->fetched, error);
}

/* @text with every part of @secret blanked out: the whole of it, and its
 * query string (the part that holds the key). */
static void
scrub_message(
	GError      *error,
	const gchar *secret
){
	const gchar *query;
	gchar **parts;
	gchar *joined;

	if (error == NULL || secret == NULL || secret[0] == '\0')
		return;
	if (strstr(error->message, secret) != NULL) {
		parts = g_strsplit(error->message, secret, -1);
		joined = g_strjoinv("***", parts);
		g_strfreev(parts);
		g_free(error->message);
		error->message = joined;
	}
	query = strchr(secret, '?');
	if (query != NULL && query[1] != '\0' && strstr(error->message, query + 1) != NULL) {
		parts = g_strsplit(error->message, query + 1, -1);
		joined = g_strjoinv("***", parts);
		g_strfreev(parts);
		g_free(error->message);
		error->message = joined;
	}
}

/* One release, from the source that returned it. */
static gboolean
fetch_one(
	AxisIndexers        *self,
	gint64               release_id,
	GCancellable        *cancellable,
	AxisIndexerFetched  *fetched,
	GError             **error
){
	AxisIndexerFetchTarget target;
	g_autoptr(GError) failure = NULL;
	AxisDbConn *conn;
	gboolean found = FALSE;
	FetchData fetch;
	gboolean ok;

	memset(fetched, 0, sizeof(*fetched));
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_load_fetch(conn, release_id, &found, &target, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (!found) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND,
			"there is no such release, or its search has expired");
		return FALSE;
	}

	fetch.target = &target;
	fetch.fetched = fetched;
	ok = axis_indexer_with_source(self, target.source_id, TRUE, cancellable, fetch_call, &fetch, &failure);
	if (ok) {
		/* Whatever the kind did, what leaves here is a file of the right
		 * kind or a magnet link, and not both. */
		if ((fetched->file == NULL) == (fetched->magnet == NULL)) {
			g_set_error_literal(&failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
				"the indexer returned neither a file nor a magnet link");
			ok = FALSE;
		} else if (fetched->file != NULL) {
			gsize length = 0;
			const guint8 *data = g_bytes_get_data(fetched->file, &length);
			AxisIndexerContent content = axis_indexer_content_kind(data, length);

			if (length > AXIS_INDEXER_MAX_FILE_BYTES) {
				g_set_error_literal(&failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE,
					"the file is larger than the limit");
				ok = FALSE;
			} else if (content != (target.protocol == AXIS_DOWNLOAD_USENET ? AXIS_INDEXER_CONTENT_NZB :
			           AXIS_INDEXER_CONTENT_TORRENT)) {
				g_set_error_literal(&failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
					"what the indexer returned is not an NZB or a torrent file of the right kind");
				ok = FALSE;
			}
		} else if (!axis_download_magnet_parse(fetched->magnet, NULL, NULL)) {
			g_set_error_literal(&failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
				"the indexer returned a magnet link that is not valid");
			ok = FALSE;
		}
		if (!ok)
			axis_indexer_fetched_clear(fetched);
	}
	if (ok) {
		fetched->source_id = target.source_id;
		fetched->release_id = release_id;
		axis_indexer_fetch_target_clear(&target);
		return TRUE;
	}
	/* A message holds neither the reference nor its query. */
	scrub_message(failure, target.fetch_ref);
	scrub_message(failure, target.magnet);
	if (axis_indexer_is_contact_failure(failure))
		axis_indexer_record_contact(self, target.source_id, failure, NULL);
	axis_indexer_fetch_target_clear(&target);
	g_propagate_error(error, g_steal_pointer(&failure));
	return FALSE;
}

/* The most other indexers' copies of a release tried when it cannot be
 * fetched. */
#define MAX_ALTERNATIVES 8

gboolean
axis_indexers_fetch(
	AxisIndexers        *self,
	gint64               release_id,
	GCancellable        *cancellable,
	AxisIndexerFetched  *fetched,
	GError             **error
){
	g_autoptr(GError) failure = NULL;
	g_autoptr(GArray) others = NULL;
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn;
	guint tried = 0;
	guint i;

	g_return_val_if_fail(self != NULL && fetched != NULL, FALSE);

	if (fetch_one(self, release_id, cancellable, fetched, &failure))
		return TRUE;
	if (g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED) ||
	    g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&failure));
		return FALSE;
	}
	/* The same release at other indexers, before it is given up on. */
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn != NULL) {
		if (!axis_indexer_store_alternatives(conn, release_id, MAX_ALTERNATIVES, &others, &local))
			g_debug("indexers: cannot look for other copies of a release: %s", local->message);
		axis_db_pool_release(self->pool, conn);
	}
	for (i = 0; others != NULL && i < others->len; i++) {
		g_autoptr(GError) again = NULL;
		gint64 other = g_array_index(others, gint64, i);

		tried++;
		if (fetch_one(self, other, cancellable, fetched, &again)) {
			g_debug("indexers: release %" G_GINT64_FORMAT " was fetched from another indexer's copy, %"
				G_GINT64_FORMAT, release_id, other);
			return TRUE;
		}
		if (g_error_matches(again, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&again));
			return FALSE;
		}
	}
	if (tried > 0) {
		gchar *more = g_strdup_printf("%s (nor could it be fetched from the %u other %s that found it)",
			failure->message, tried, tried == 1 ? "indexer" : "indexers");

		g_free(failure->message);
		failure->message = more;
	}
	g_propagate_error(error, g_steal_pointer(&failure));
	return FALSE;
}

/* ── Expiry ─────────────────────────────────────────────────────── */

gboolean
axis_indexers_expire(
	AxisIndexers  *self,
	guint         *n_searches,
	GError       **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_expire(conn, n_searches, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}
