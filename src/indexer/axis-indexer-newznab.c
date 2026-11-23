/*
 * axis-indexer-newznab.c - the Newznab and Torznab kinds
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One file for both: Torznab is Newznab for torrents.  The kind never sees
 * the key (the connection puts it in the query) and writes its messages from
 * fixed text and the indexer's cleaned, scrubbed words.
 */

#include "indexer/axis-indexer-newznab.h"

#include <string.h>

#include "download/axis-download-bencode.h"
#include "indexer/axis-indexer-newznab-parse.h"

/* The most a capabilities answer may be. */
#define CAPS_ANSWER_BYTES (1024 * 1024)

/* What is asked for: XML, the protocol's one format. */
#define XML_ACCEPT "application/rss+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.5"

/* What a file's fetch accepts. */
#define FETCH_ACCEPT "application/x-nzb, application/x-bittorrent, application/octet-stream;q=0.9, */*;q=0.5"

/* ── Errors ─────────────────────────────────────────────────────── */

static gboolean
unreadable(
	AxisIndexerConn  *conn,
	GError          **error,
	const gchar      *what,
	const gchar      *problem
){
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE, "%s's %s cannot be read: %s.",
		axis_indexer_conn_get_label(conn), what, problem != NULL ? problem : "it is not what was expected");
	return FALSE;
}

/* An error document, as an error of the layer; a limit reached holds the
 * indexer. */
static gboolean
error_document(
	AxisIndexerConn        *conn,
	const AxisNewznabError *document,
	GError                **error
){
	const gchar *label = axis_indexer_conn_get_label(conn);
	g_autofree gchar *said = axis_indexer_conn_scrub(conn, document->description, 200);
	g_autofree gchar *code = g_strdup_printf("error %d%s%s", document->code, said != NULL ? ": " : "",
		said != NULL ? said : "");
	AxisIndexerError kind = axis_newznab_error_kind(document->code);

	switch (kind) {
	case AXIS_INDEXER_ERROR_AUTH:
		g_set_error(error, AXIS_INDEXER_ERROR, kind, "%s refused the key or the account (%s)", label, code);
		break;
	case AXIS_INDEXER_ERROR_RATE_LIMITED: {
		g_autofree gchar *message = g_strdup_printf("%s says its %s limit is reached (%s)", label,
			document->code == 501 ? "download" : "request", code);

		axis_indexer_conn_hold(conn, AXIS_INDEXER_NEWZNAB_HOLD_S, document->code == 501, message);
		g_set_error(error, AXIS_INDEXER_ERROR, kind, "%s, so it is not asked again for an hour", message);
		break;
	}
	case AXIS_INDEXER_ERROR_REFUSED:
		g_set_error(error, AXIS_INDEXER_ERROR, kind, "%s refused the request (%s)", label, code);
		break;
	default:
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED, "%s answered with an error (%s)", label,
			code);
		break;
	}
	return FALSE;
}

static gboolean
get_xml(
	AxisIndexerConn  *conn,
	const gchar      *path,
	gsize             max_bytes,
	GBytes          **body,
	GError          **error
){
	AxisIndexerGetOptions options;

	memset(&options, 0, sizeof(options));
	options.accept = XML_ACCEPT;
	options.max_bytes = max_bytes;
	options.timeout_ms = AXIS_INDEXER_NATIVE_TIMEOUT_MS;
	return axis_indexer_conn_get(conn, path, &options, body, NULL, error);
}

/* ── Capabilities ───────────────────────────────────────────────── */

/*
 * Reads t=caps and tells the connection.  An answer that cannot be read, or
 * an indexer that has no capabilities function, is a capabilities problem
 * (*@problem), not a failure; the key refused, the limit reached, no answer
 * at all are failures.
 */
static gboolean
read_caps(
	AxisIndexerConn  *conn,
	gchar           **version,
	gchar           **problem,
	GError          **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *why = NULL;
	AxisIndexerCaps caps;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	gsize length = 0;
	const gchar *data;

	if (version != NULL)
		*version = NULL;
	*problem = NULL;
	if (!get_xml(conn, AXIS_NEWZNAB_CAPS_PATH, CAPS_ANSWER_BYTES, &body, &local)) {
		/* An indexer that answers its capabilities with a refusal of its
		 * own (a 404 or such) can still be searched by words. */
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED) ||
		    g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE)) {
			*problem = g_strdup("it did not answer with its capabilities");
			return TRUE;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	data = g_bytes_get_data(body, &length);
	switch (axis_newznab_parse_caps(data, length, &caps, &allowance, version, &document, &why)) {
	case AXIS_NEWZNAB_READ_OK:
		axis_indexer_conn_learn_caps(conn, &caps);
		axis_indexer_conn_learn_allowance(conn, &allowance);
		axis_indexer_caps_clear(&caps);
		return TRUE;
	case AXIS_NEWZNAB_READ_ERROR: {
		gboolean ok = TRUE;

		/* No such function, or not available: words only. */
		if (document.code == 202 || document.code == 203)
			*problem = g_strdup("it says it has no capabilities function");
		else
			ok = error_document(conn, &document, error);
		axis_newznab_error_clear(&document);
		return ok;
	}
	default:
		*problem = g_strdup_printf("its answer cannot be read: %s", why != NULL ? why : "it is not XML");
		return TRUE;
	}
}

/* Records a capabilities problem: the indexer is searched by words. */
static void
learn_problem(
	AxisIndexerConn *conn,
	const gchar     *problem
){
	AxisIndexerCaps none;

	memset(&none, 0, sizeof(none));
	none.problem = (gchar *)problem;
	axis_indexer_conn_learn_caps(conn, &none);
}

static gboolean
native_caps(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	GError                    **error
){
	g_autofree gchar *problem = NULL;

	if (!read_caps(conn, NULL, &problem, error))
		return FALSE;
	if (problem != NULL) {
		learn_problem(conn, problem);
		return unreadable(conn, error, "capabilities", problem);
	}
	return TRUE;
}

/* ── A search ───────────────────────────────────────────────────── */

/* One request of a search: its answer read into @outcome. */
static gboolean
search_once(
	AxisIndexerConn     *conn,
	const gchar         *path,
	AxisIndexerOutcome  *outcome,
	GError             **error
){
	const AxisIndexerNative *native = axis_indexer_conn_get_native(conn);
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *problem = NULL;
	AxisNewznabContext context;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	gsize length = 0;
	const gchar *data;
	guint i;

	if (!get_xml(conn, path, AXIS_INDEXER_MAX_ANSWER_BYTES, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	memset(&context, 0, sizeof(context));
	context.origin = axis_indexer_conn_get_origin(conn);
	context.files_origin = native->files_origin;
	context.protocol = native->protocol;
	context.now_us = g_get_real_time();
	switch (axis_newznab_parse_search(data, length, &context, outcome, &allowance, &document, &problem)) {
	case AXIS_NEWZNAB_READ_OK:
		break;
	case AXIS_NEWZNAB_READ_ERROR: {
		gboolean ok = error_document(conn, &document, error);

		axis_newznab_error_clear(&document);
		return ok;
	}
	default:
		return unreadable(conn, error, "search answer", problem);
	}
	axis_indexer_conn_learn_allowance(conn, &allowance);
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

/* Adds what @page found to @outcome: a result already seen (by guid or by
 * info-hash) is dropped, and the results stop at the cap.  What moves leaves
 * a %NULL in @page, which its clearing skips.  Returns how many were new. */
static guint
merge_page(
	AxisIndexerOutcome *outcome,
	AxisIndexerOutcome *page,
	GHashTable         *seen
){
	guint added = 0;
	guint i;

	for (i = 0; page->items != NULL && i < page->items->len; i++) {
		AxisIndexerFound *found = g_ptr_array_index(page->items, i);
		g_autofree gchar *hash_key = found->release->info_hash != NULL ?
			g_strconcat("hash:", found->release->info_hash, NULL) : NULL;

		if (g_hash_table_contains(seen, found->merge_key) ||
		    (hash_key != NULL && g_hash_table_contains(seen, hash_key)))
			continue;
		if (outcome->items->len >= AXIS_INDEXER_MAX_RESULTS) {
			outcome->truncated = TRUE;
			continue;
		}
		g_hash_table_add(seen, g_strdup(found->merge_key));
		if (hash_key != NULL)
			g_hash_table_add(seen, g_strdup(hash_key));
		g_ptr_array_add(outcome->items, found);
		page->items->pdata[i] = NULL;
		added++;
	}
	outcome->n_off_origin += page->n_off_origin;
	outcome->n_unusable += page->n_unusable;
	outcome->n_skipped += page->n_skipped;
	outcome->truncated = outcome->truncated || page->truncated;
	if (outcome->other_origin == NULL)
		outcome->other_origin = g_steal_pointer(&page->other_origin);
	return added;
}

/*
 * A search, a page at a time: while the indexer says there are more (its
 * stated total, else until a page comes back short), up to
 * %AXIS_NEWZNAB_MAX_PAGES pages and the cap on results.  Each page is a call
 * of its own, through the indexer's limiter.  A later page that cannot be had
 * ends the list there (what was read is kept, and the result says it is cut);
 * the first is the search.
 */
static gboolean
search_pages(
	AxisIndexerConn     *conn,
	const gchar         *first_path,
	guint                limit,
	AxisIndexerOutcome  *outcome,
	GError             **error
){
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint offset = 0;
	guint page;

	outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	for (page = 1; ; page++) {
		g_autofree gchar *path = page == 1 ? g_strdup(first_path) : axis_newznab_path_at_offset(first_path, offset);
		g_autoptr(GError) local = NULL;
		AxisIndexerOutcome got;
		gboolean more;
		guint added;

		if (path == NULL) {
			outcome->truncated = TRUE;
			return TRUE;
		}
		axis_indexer_outcome_init(&got);
		if (!search_once(conn, path, &got, page == 1 ? error : &local)) {
			axis_indexer_outcome_clear(&got);
			if (page == 1)
				return FALSE;
			if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			outcome->truncated = TRUE;
			return TRUE;
		}
		added = merge_page(outcome, &got, seen);
		offset += got.n_listed;
		more = got.n_listed > 0 && (got.total >= 0 ? (gint64)offset < got.total : got.n_listed >= limit);
		/* A page of nothing new: an indexer that ignores the offset. */
		if (page > 1 && added == 0)
			more = FALSE;
		if (got.truncated || outcome->truncated) {
			axis_indexer_outcome_clear(&got);
			outcome->truncated = TRUE;
			return TRUE;
		}
		axis_indexer_outcome_clear(&got);
		if (!more)
			return TRUE;
		if (outcome->items->len >= AXIS_INDEXER_MAX_RESULTS || page >= AXIS_NEWZNAB_MAX_PAGES) {
			/* Stopped by a cap: there is more. */
			outcome->truncated = TRUE;
			return TRUE;
		}
	}
}

static gboolean
native_search(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	const AxisIndexerRequest   *request,
	AxisIndexerOutcome         *outcome,
	GError                    **error
){
	const AxisIndexerNative *native = axis_indexer_conn_get_native(conn);
	g_autofree gchar *path = NULL;
	AxisIndexerMode modes[3];
	const gint *categories = request->categories;
	guint n_categories = request->n_categories;
	gint with_anime[AXIS_INDEXER_MAX_CATEGORIES];
	guint n;

	axis_indexer_outcome_init(outcome);
	/* Never read: read now, so that an identifier it takes is used. */
	if (native->caps->read_us == 0) {
		g_autofree gchar *problem = NULL;

		if (!read_caps(conn, NULL, &problem, error))
			return FALSE;
		if (problem != NULL)
			learn_problem(conn, problem);
		native = axis_indexer_conn_get_native(conn);
	}
	if (n_categories == 0) {
		if (request->kind == AXIS_INDEXER_QUERY_FILM) {
			categories = native->movie_categories;
			n_categories = native->n_movie_categories;
		} else if (request->anime) {
			/* An anime series (add-anime-support, A3): its TV categories and
			 * TV/Anime, for an episode, a season and words alike. */
			n_categories = axis_indexer_request_tv_categories(native->tv_categories, native->n_tv_categories,
				with_anime);
			categories = with_anime;
		} else if (request->kind != AXIS_INDEXER_QUERY_TEXT) {
			categories = native->tv_categories;
			n_categories = native->n_tv_categories;
		}
	}
	n = axis_newznab_identifiers(request, native->caps, modes);
	path = axis_newznab_search_path(request, n > 0 ? modes[0] : AXIS_INDEXER_BY_TEXT, native->caps, categories,
		n_categories);
	if (path == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"%s takes none of the identifiers given, and there is no title to search it by",
			axis_indexer_conn_get_label(conn));
		return FALSE;
	}
	if (!search_pages(conn, path, axis_newznab_page_limit(native->caps), outcome, error)) {
		axis_indexer_outcome_clear(outcome);
		return FALSE;
	}
	outcome->mode = n > 0 ? modes[0] : AXIS_INDEXER_BY_TEXT;
	/* The origin of files a search saw is not offered: only a test's. */
	g_clear_pointer(&outcome->other_origin, g_free);
	return TRUE;
}

/* ── A test ─────────────────────────────────────────────────────── */

static gboolean
native_test(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	gchar                     **version,
	GError                    **error
){
	const AxisIndexerNative *native = axis_indexer_conn_get_native(conn);
	g_autofree gchar *problem = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *said = NULL;
	gint categories[AXIS_INDEXER_MAX_CATEGORIES * 2];
	AxisIndexerOutcome outcome;
	guint n = 0;
	guint i;

	*version = NULL;
	if (!read_caps(conn, &said, &problem, error))
		return FALSE;
	if (problem != NULL)
		learn_problem(conn, problem);
	/* What is newest, in its movie and TV categories: proves the key (an
	 * indexer may answer its capabilities without one) and shows where its
	 * files are. */
	for (i = 0; i < native->n_movie_categories && n < G_N_ELEMENTS(categories); i++)
		categories[n++] = native->movie_categories[i];
	for (i = 0; i < native->n_tv_categories && n < G_N_ELEMENTS(categories); i++)
		categories[n++] = native->tv_categories[i];
	path = axis_newznab_latest_path(categories, MIN(n, (guint)AXIS_INDEXER_MAX_CATEGORIES),
		AXIS_INDEXER_NEWZNAB_TEST_LIMIT);
	axis_indexer_outcome_init(&outcome);
	if (!search_once(conn, path, &outcome, error)) {
		axis_indexer_outcome_clear(&outcome);
		return FALSE;
	}
	if (outcome.other_origin != NULL)
		axis_indexer_conn_learn_other_origin(conn, outcome.other_origin);
	axis_indexer_outcome_clear(&outcome);
	*version = said != NULL ? g_steal_pointer(&said) : g_strdup(self->label);
	return TRUE;
}

static gboolean
native_indexers(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	GPtrArray                 **indexers,
	GError                    **error
){
	const AxisIndexerNative *native = axis_indexer_conn_get_native(conn);
	AxisIndexerInfo *info = axis_indexer_info_new();

	/* The indexer itself, with the identifiers its capabilities name. */
	info->id = 0;
	info->name = axis_indexer_text_clean(axis_indexer_conn_get_label(conn), AXIS_INDEXER_INDEXER_MAX_CHARS);
	info->protocol = native->protocol;
	info->enabled = TRUE;
	if (native->caps->read_us > 0 && native->caps->problem == NULL) {
		info->movie_ids = (native->caps->modes & AXIS_INDEXER_CAN_MOVIE) != 0 ? native->caps->movie_ids : 0;
		info->tv_ids = (native->caps->modes & AXIS_INDEXER_CAN_TV) != 0 && native->caps->tv_numbers ?
			native->caps->tv_ids : 0;
	}
	info->host = axis_indexer_host_key(axis_indexer_conn_get_origin(conn)->host);
	*indexers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_info_free);
	g_ptr_array_add(*indexers, info);
	return TRUE;
}

/* ── A fetch ────────────────────────────────────────────────────── */

static gboolean
refused(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED, message);
	return FALSE;
}

/* A magnet link as the fetch's result, when it is one, and of the torrent
 * the search said (@stated, its info-hash, when it gave one). */
static gboolean
take_magnet(
	const gchar         *magnet,
	const gchar         *stated,
	AxisIndexerFetched  *fetched,
	GError             **error
){
	g_autofree gchar *hash = NULL;

	if (!axis_download_magnet_parse(magnet, &hash, NULL))
		return refused(error, "the indexer gave a magnet link that is not valid");
	if (stated != NULL && g_ascii_strcasecmp(stated, hash) != 0)
		return refused(error, "the indexer gave a magnet link for another torrent than the search said");
	fetched->magnet = g_strdup(magnet);
	fetched->info_hash = g_steal_pointer(&hash);
	return TRUE;
}

static gboolean
native_fetch(
	const AxisIndexerKindInfo     *self,
	AxisIndexerConn               *conn,
	const AxisIndexerFetchRequest *request,
	AxisIndexerFetched            *fetched,
	GError                       **error
){
	AxisIndexerGetOptions options;
	AxisIndexerReply reply;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) local = NULL;
	AxisIndexerContent content;
	AxisIndexerContent wanted = request->protocol == AXIS_DOWNLOAD_USENET ? AXIS_INDEXER_CONTENT_NZB :
		AXIS_INDEXER_CONTENT_TORRENT;
	AxisNewznabError document;
	gsize length = 0;
	const guint8 *data;

	fetched->protocol = request->protocol;
	/* A Torznab result's magnet link needs no request. */
	if (request->magnet != NULL && request->protocol == AXIS_DOWNLOAD_TORRENT)
		return take_magnet(request->magnet, request->info_hash, fetched, error);
	if (request->fetch_path == NULL)
		return refused(error, "there is no way to fetch this release");

	memset(&options, 0, sizeof(options));
	options.accept = FETCH_ACCEPT;
	options.max_bytes = AXIS_INDEXER_MAX_FILE_BYTES;
	options.slow = TRUE;
	options.files_origin = request->files_origin;
	memset(&reply, 0, sizeof(reply));
	if (!axis_indexer_conn_fetch_release(conn, request->fetch_path, &options, &body, &reply, &local)) {
		/* A torrent's download that answers with its magnet link: read,
		 * checked, never followed.  Any other redirect off the origin was
		 * refused, with nothing sent there. */
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT) && reply.magnet != NULL &&
		    request->protocol == AXIS_DOWNLOAD_TORRENT) {
			gboolean taken = take_magnet(reply.magnet, request->info_hash, fetched, error);

			axis_indexer_reply_clear(&reply);
			return taken;
		}
		axis_indexer_reply_clear(&reply);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	axis_indexer_reply_clear(&reply);

	/* What it is is by its bytes; an error document is what it says. */
	data = g_bytes_get_data(body, &length);
	content = axis_indexer_content_kind(data, length);
	if (content != wanted) {
		gboolean said;

		memset(&document, 0, sizeof(document));
		said = axis_newznab_parse_error_only((const gchar *)data, length, &document);
		if (said) {
			error_document(conn, &document, error);
			axis_newznab_error_clear(&document);
			return FALSE;
		}
		return refused(error, request->protocol == AXIS_DOWNLOAD_USENET ?
			"what the indexer returned is not an NZB file" : "what the indexer returned is not a torrent file");
	}
	if (content == AXIS_INDEXER_CONTENT_TORRENT)
		axis_download_torrent_read(data, length, &fetched->info_hash, NULL, NULL);
	fetched->file = g_steal_pointer(&body);
	fetched->file_name = axis_indexer_file_name(request->name, content == AXIS_INDEXER_CONTENT_NZB ? "nzb" : "torrent");
	return TRUE;
}

/* ── The kinds ──────────────────────────────────────────────────── */

static const AxisIndexerKindInfo newznab = {
	AXIS_INDEXER_NEWZNAB_NAME, "Newznab", "API key",
	"The indexer's own address, such as https://api.indexer.example, with nothing after the host and port; Axis asks "
	"its /api there. If its API is at another path, such as /api/v1/api, the path in front of /api (here /api/v1) "
	"goes in the base path.",
	NULL,
	native_test, native_indexers, native_search, native_fetch, NULL,
	TRUE, AXIS_DOWNLOAD_USENET, "apikey", native_caps,
	FALSE, NULL, NULL,
	FALSE
};

static const AxisIndexerKindInfo torznab = {
	AXIS_INDEXER_TORZNAB_NAME, "Torznab", "API key",
	"The feed's own address, such as http://jackett:9117, with nothing after the host and port; the feed's path "
	"goes in the base path (for Jackett /api/v2.0/indexers/NAME/results/torznab, for one of Prowlarr's indexers "
	"/NUMBER). Axis asks its /api there.",
	NULL,
	native_test, native_indexers, native_search, native_fetch, NULL,
	TRUE, AXIS_DOWNLOAD_TORRENT, "apikey", native_caps,
	FALSE, NULL, NULL,
	FALSE
};

const AxisIndexerKindInfo *
axis_indexer_newznab_kind(void)
{
	return &newznab;
}

const AxisIndexerKindInfo *
axis_indexer_torznab_kind(void)
{
	return &torznab;
}
