/*
 * axis-indexer-sites.c - the YTS, EZTV and The Pirate Bay kinds
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three small kinds sharing their helpers: which site one is is its kind's
 * data.  A kind never sees an address (the connection gives it GETs of paths
 * on the site's own origin) and writes its messages from fixed text and the
 * site's cleaned, scrubbed words.
 */

#include "indexer/axis-indexer-sites.h"

#include <string.h>

#include "indexer/axis-indexer-sites-parse.h"

/* The most an answer may be: a hundred results are some hundred kilobytes. */
#define SITE_ANSWER_BYTES (4 * 1024 * 1024)

typedef enum
{
	SITE_YTS,
	SITE_EZTV,
	SITE_TPB
} Site;

static Site
site_of(const AxisIndexerKindInfo *self)
{
	return (Site)GPOINTER_TO_INT(self->data);
}

/* ── Errors ─────────────────────────────────────────────────────── */

static gboolean
unreadable(
	AxisIndexerConn  *conn,
	GError          **error,
	const gchar      *problem
){
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE, "%s's answer cannot be read: %s.",
		axis_indexer_conn_get_label(conn), problem != NULL ? problem : "it is not what was expected");
	return FALSE;
}

/*
 * One GET of @path, the body in @body.  What a site says no to, or where it
 * has moved to, is the site failing (it has no key to be refused, and a
 * redirect to another address is how a moved domain shows): the connection's
 * errors for those are made into failures that say what to do.
 */
static gboolean
get_json(
	AxisIndexerConn  *conn,
	const gchar      *path,
	GBytes          **body,
	GError          **error
){
	AxisIndexerGetOptions options;
	g_autoptr(GError) local = NULL;
	const gchar *label = axis_indexer_conn_get_label(conn);

	memset(&options, 0, sizeof(options));
	options.accept = "application/json";
	options.max_bytes = SITE_ANSWER_BYTES;
	options.timeout_ms = AXIS_INDEXER_NATIVE_TIMEOUT_MS;
	if (axis_indexer_conn_get(conn, path, &options, body, NULL, &local))
		return TRUE;
	if (local->domain != AXIS_INDEXER_ERROR) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	switch ((AxisIndexerError)local->code) {
	case AXIS_INDEXER_ERROR_REDIRECT:
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s redirects to another address, which Axis does not follow: if the site has moved, change its address "
			"here", label);
		break;
	case AXIS_INDEXER_ERROR_AUTH:
	case AXIS_INDEXER_ERROR_REFUSED: {
		g_autofree gchar *said = axis_indexer_conn_scrub(conn, local->message, 200);

		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s did not give its list: %s (if the site has moved, change its address here)", label,
			said != NULL ? said : "it refused the request");
		break;
	}
	case AXIS_INDEXER_ERROR_TOO_LARGE:
		unreadable(conn, error, "it is larger than any list of its should be");
		break;
	default:
		g_propagate_error(error, g_steal_pointer(&local));
		break;
	}
	return FALSE;
}

/* ── What each site takes ───────────────────────────────────────── */

/* What a site can be asked, as capabilities: nothing to read, so it is told
 * to the connection from what is known (a test, and the first search). */
static void
tell_caps(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn
){
	AxisIndexerCaps caps;

	memset(&caps, 0, sizeof(caps));
	switch (site_of(self)) {
	case SITE_YTS:
		caps.modes = AXIS_INDEXER_CAN_MOVIE;
		caps.movie_ids = AXIS_INDEXER_IDS_IMDB;
		caps.movie_text = TRUE;
		caps.limit_max = AXIS_SITES_YTS_LIMIT;
		caps.limit_default = AXIS_SITES_YTS_LIMIT;
		break;
	case SITE_EZTV:
		caps.modes = AXIS_INDEXER_CAN_TV;
		caps.tv_ids = AXIS_INDEXER_IDS_IMDB;
		caps.limit_max = AXIS_SITES_EZTV_LIMIT;
		caps.limit_default = AXIS_SITES_EZTV_LIMIT;
		break;
	default:
		caps.modes = AXIS_INDEXER_CAN_SEARCH | AXIS_INDEXER_CAN_MOVIE | AXIS_INDEXER_CAN_TV;
		caps.movie_ids = AXIS_INDEXER_IDS_IMDB;
		caps.movie_text = TRUE;
		caps.tv_text = TRUE;
		break;
	}
	axis_indexer_conn_learn_caps(conn, &caps);
}

/* What a site's answer to the test is called on its page ("It answers as"). */
static const gchar *
answers_as(const AxisIndexerKindInfo *self)
{
	switch (site_of(self)) {
	case SITE_YTS:
		return "YTS's list of movies";
	case SITE_EZTV:
		return "EZTV's list of torrents";
	default:
		return "The Pirate Bay's list of torrents";
	}
}

/* Reads @body as the site's answer into @outcome. */
static gboolean
read_answer(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	GBytes                    *body,
	GHashTable                *seen,
	AxisIndexerOutcome        *outcome,
	AxisSitesRead             *read,
	GError                   **error
){
	g_autofree gchar *problem = NULL;
	gsize length = 0;
	const gchar *data = g_bytes_get_data(body, &length);
	gint64 now_us = g_get_real_time();
	gboolean ok;

	switch (site_of(self)) {
	case SITE_YTS:
		ok = axis_sites_parse_yts(data, length, now_us, seen, outcome, read, &problem);
		break;
	case SITE_EZTV:
		ok = axis_sites_parse_eztv(data, length, now_us, seen, outcome, read, &problem);
		break;
	default:
		ok = axis_sites_parse_tpb(data, length, now_us, seen, outcome, read, &problem);
		break;
	}
	if (!ok)
		return unreadable(conn, error, problem);
	return TRUE;
}

/* ── A search ───────────────────────────────────────────────────── */

static gboolean
invalid(
	AxisIndexerConn  *conn,
	GError          **error,
	const gchar      *text
){
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "%s %s", axis_indexer_conn_get_label(conn),
		text);
	return FALSE;
}

/* One request and its answer, read into @outcome. */
static gboolean
ask(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	const gchar               *path,
	GHashTable                *seen,
	AxisIndexerOutcome        *outcome,
	AxisSitesRead             *read,
	GError                   **error
){
	g_autoptr(GBytes) body = NULL;

	return get_json(conn, path, &body, error) && read_answer(self, conn, body, seen, outcome, read, error);
}

/*
 * The Pirate Bay: a movie by its IMDb identifier when the request has one,
 * which the site matches against what its uploads name; many uploads name
 * none, so when that finds nothing the title and year are asked instead and
 * the outcome says it fell back.  An episode, a season and words are asked
 * by words: an identifier would list every episode of a series.
 */
static gboolean
search_tpb(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	const AxisIndexerRequest  *request,
	AxisIndexerOutcome        *outcome,
	GError                   **error
){
	g_autofree gchar *by_id = axis_sites_tpb_imdb_path(request);
	g_autofree gchar *by_words = axis_sites_tpb_path(request);
	AxisSitesRead read;

	if (by_id == NULL && by_words == NULL)
		return invalid(conn, error, "is searched by words or a movie's IMDb identifier, and this search has neither");
	if (by_id != NULL) {
		if (!ask(self, conn, by_id, NULL, outcome, &read, error))
			return FALSE;
		outcome->mode = AXIS_INDEXER_BY_IMDB;
		if (outcome->items->len > 0 || by_words == NULL)
			return TRUE;
		outcome->mode = AXIS_INDEXER_BY_TEXT;
		outcome->fell_back = TRUE;
	}
	return ask(self, conn, by_words, NULL, outcome, &read, error);
}

/* EZTV's list, a page at a time up to the cap: a later page that cannot be
 * had ends the list there (the search keeps what it has, and says it is
 * cut); the first is the search. */
static gboolean
search_eztv(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	const AxisIndexerRequest  *request,
	AxisIndexerOutcome        *outcome,
	GError                   **error
){
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint page;

	for (page = 1; page <= AXIS_SITES_EZTV_MAX_PAGES; page++) {
		g_autofree gchar *path = axis_sites_eztv_path(request, page);
		g_autoptr(GError) local = NULL;
		AxisSitesRead read;

		if (!ask(self, conn, path, seen, outcome, &read, page == 1 ? error : &local)) {
			if (page == 1)
				return FALSE;
			if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			outcome->truncated = TRUE;
			return TRUE;
		}
		/* The last page: fewer than a page, or all the site says there are. */
		if (read.n_entries < AXIS_SITES_EZTV_LIMIT || outcome->truncated ||
		    (read.total >= 0 && (gint64)page * AXIS_SITES_EZTV_LIMIT >= read.total))
			return TRUE;
	}
	/* Stopped by the cap: there may be more. */
	outcome->truncated = TRUE;
	return TRUE;
}

static gboolean
site_search(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	const AxisIndexerRequest   *request,
	AxisIndexerOutcome         *outcome,
	GError                    **error
){
	const AxisIndexerNative *native = axis_indexer_conn_get_native(conn);
	g_autofree gchar *path = NULL;
	AxisSitesRead read;
	gboolean ok = TRUE;
	guint i;

	axis_indexer_outcome_init(outcome);
	outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	outcome->mode = AXIS_INDEXER_BY_TEXT;
	if (native->caps->read_us == 0)
		tell_caps(self, conn);
	switch (site_of(self)) {
	case SITE_YTS:
		/* Movies only: a search for TV finds nothing here. */
		/* (Words asked for an anime series are a series' too.) */
		if (request->kind == AXIS_INDEXER_QUERY_EPISODE || request->kind == AXIS_INDEXER_QUERY_SEASON ||
		    (request->anime && request->kind != AXIS_INDEXER_QUERY_FILM)) {
			axis_indexer_outcome_nothing_here(outcome, request);
			return TRUE;
		}
		path = axis_sites_yts_path(request, 1);
		if (path == NULL) {
			axis_indexer_outcome_clear(outcome);
			return invalid(conn, error, "takes an IMDb identifier or a title, and this search has neither");
		}
		ok = ask(self, conn, path, NULL, outcome, &read, error);
		if (request->imdb_id != NULL)
			outcome->mode = AXIS_INDEXER_BY_IMDB;
		break;
	case SITE_EZTV:
		/* TV only, and only by IMDb identifier. */
		if (request->kind == AXIS_INDEXER_QUERY_FILM) {
			axis_indexer_outcome_nothing_here(outcome, request);
			return TRUE;
		}
		path = axis_sites_eztv_path(request, 1);
		if (path == NULL) {
			axis_indexer_outcome_clear(outcome);
			return invalid(conn, error, "is searched by IMDb identifier only, and this search has none, so nothing "
				"was asked");
		}
		ok = search_eztv(self, conn, request, outcome, error);
		outcome->mode = AXIS_INDEXER_BY_IMDB;
		break;
	default:
		ok = search_tpb(self, conn, request, outcome, error);
		break;
	}
	if (!ok) {
		axis_indexer_outcome_clear(outcome);
		return FALSE;
	}
	/* Each release is this indexer's. */
	for (i = 0; i < outcome->items->len; i++) {
		AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		g_free(found->release->indexer_name);
		found->release->indexer_name = axis_indexer_text_clean(axis_indexer_conn_get_label(conn),
			AXIS_INDEXER_INDEXER_MAX_CHARS);
		found->release->indexer_id = 0;
	}
	return TRUE;
}

/* ── A test ─────────────────────────────────────────────────────── */

/*
 * Asks for the newest of the site and reads the answer as its list: that is
 * all there is to prove (no key to try).  A site at an address that is no
 * longer its own answers with something else (a page, an error, another
 * list), and is shown as failing.
 */
static gboolean
site_test(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	gchar                     **version,
	GError                    **error
){
	g_autofree gchar *path = NULL;
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	gboolean ok;

	*version = NULL;
	tell_caps(self, conn);
	switch (site_of(self)) {
	case SITE_YTS:
		path = axis_sites_yts_latest_path();
		break;
	case SITE_EZTV:
		path = axis_sites_eztv_latest_path();
		break;
	default:
		path = axis_sites_tpb_latest_path();
		break;
	}
	axis_indexer_outcome_init(&outcome);
	ok = ask(self, conn, path, NULL, &outcome, &read, error);
	axis_indexer_outcome_clear(&outcome);
	if (!ok)
		return FALSE;
	*version = g_strdup(answers_as(self));
	return TRUE;
}

static gboolean
site_indexers(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	GPtrArray                 **indexers,
	GError                    **error
){
	AxisIndexerInfo *info = axis_indexer_info_new();

	/* The site itself, with the identifiers it takes. */
	info->id = 0;
	info->name = axis_indexer_text_clean(axis_indexer_conn_get_label(conn), AXIS_INDEXER_INDEXER_MAX_CHARS);
	info->protocol = AXIS_DOWNLOAD_TORRENT;
	info->enabled = TRUE;
	info->movie_ids = site_of(self) != SITE_EZTV ? AXIS_INDEXER_IDS_IMDB : 0;
	info->tv_ids = site_of(self) == SITE_EZTV ? AXIS_INDEXER_IDS_IMDB : 0;
	info->host = axis_indexer_host_key(axis_indexer_conn_get_origin(conn)->host);
	*indexers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_info_free);
	g_ptr_array_add(*indexers, info);
	return TRUE;
}

/* ── A fetch ────────────────────────────────────────────────────── */

/* Nothing is asked of the site: the magnet link built when the release was
 * found is the release. */
static gboolean
site_fetch(
	const AxisIndexerKindInfo     *self,
	AxisIndexerConn               *conn,
	const AxisIndexerFetchRequest *request,
	AxisIndexerFetched            *fetched,
	GError                       **error
){
	g_autofree gchar *hash = NULL;

	fetched->protocol = AXIS_DOWNLOAD_TORRENT;
	if (request->magnet == NULL) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
			"there is no way to fetch this release");
		return FALSE;
	}
	if (!axis_download_magnet_parse(request->magnet, &hash, NULL)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
			"the magnet link kept for this release is not valid");
		return FALSE;
	}
	if (request->info_hash != NULL && g_ascii_strcasecmp(request->info_hash, hash) != 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED,
			"the magnet link kept for this release is for another torrent than the search said");
		return FALSE;
	}
	fetched->magnet = g_strdup(request->magnet);
	fetched->info_hash = g_steal_pointer(&hash);
	return TRUE;
}

/* ── The kinds ──────────────────────────────────────────────────── */

static const AxisIndexerKindInfo yts = {
	AXIS_INDEXER_YTS_NAME, "YTS", "",
	"The address YTS is at now. Sites like this move between domains from time to time: when it stops answering, "
	"change the address here. Nothing after the host and port; Axis asks its /api/v2 there.",
	NULL,
	site_test, site_indexers, site_search, site_fetch, GINT_TO_POINTER(SITE_YTS),
	TRUE, AXIS_DOWNLOAD_TORRENT, NULL, NULL,
	TRUE, AXIS_INDEXER_YTS_ADDRESS, "Movies, by IMDb identifier or by title",
	FALSE
};

static const AxisIndexerKindInfo eztv = {
	AXIS_INDEXER_EZTV_NAME, "EZTV", "",
	"The address EZTV is at now. Sites like this move between domains from time to time: when it stops answering, "
	"change the address here. Nothing after the host and port; Axis asks its /api there.",
	NULL,
	site_test, site_indexers, site_search, site_fetch, GINT_TO_POINTER(SITE_EZTV),
	TRUE, AXIS_DOWNLOAD_TORRENT, NULL, NULL,
	TRUE, AXIS_INDEXER_EZTV_ADDRESS, "TV, by IMDb identifier only",
	FALSE
};

static const AxisIndexerKindInfo piratebay = {
	AXIS_INDEXER_PIRATEBAY_NAME, "The Pirate Bay", "",
	"The address of The Pirate Bay's API now (apibay). Sites like this move between domains from time to time: when it "
	"stops answering, change the address here. Nothing after the host and port; Axis asks its /q.php there.",
	NULL,
	site_test, site_indexers, site_search, site_fetch, GINT_TO_POINTER(SITE_TPB),
	TRUE, AXIS_DOWNLOAD_TORRENT, NULL, NULL,
	TRUE, AXIS_INDEXER_PIRATEBAY_ADDRESS, "Movies by IMDb identifier or by title, TV by words",
	FALSE
};

const AxisIndexerKindInfo *
axis_indexer_yts_kind(void)
{
	return &yts;
}

const AxisIndexerKindInfo *
axis_indexer_eztv_kind(void)
{
	return &eztv;
}

const AxisIndexerKindInfo *
axis_indexer_piratebay_kind(void)
{
	return &piratebay;
}
