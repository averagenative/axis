/*
 * axis-indexer-prowlarr.c - the Prowlarr kind of indexer source
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-prowlarr.h"

#include <string.h>

#include "download/axis-download-bencode.h"
#include "indexer/axis-indexer-prowlarr-parse.h"

/* The most a version, status or indexer list may be. */
#define SMALL_ANSWER_BYTES (4 * 1024 * 1024)

/* Prowlarr's own words for what a file fetch accepts. */
#define FETCH_ACCEPT "application/x-nzb, application/x-bittorrent, application/octet-stream;q=0.9, */*;q=0.5"

static gboolean
unreadable(
	GError      **error,
	const gchar  *what,
	const gchar  *problem
){
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE, "Prowlarr's %s cannot be read: %s.", what,
		problem != NULL ? problem : "it is not what was expected");
	return FALSE;
}

/* A small GET of the API: its body as text (not NUL-ended; with its length). */
static gboolean
get_small(
	AxisIndexerConn  *conn,
	const gchar      *path,
	GBytes          **body,
	GError          **error
){
	AxisIndexerGetOptions options;

	memset(&options, 0, sizeof(options));
	options.max_bytes = SMALL_ANSWER_BYTES;
	return axis_indexer_conn_get(conn, path, &options, body, NULL, error);
}

/* ── test and indexers ──────────────────────────────────────────── */

static gboolean
prowlarr_test(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	gchar                     **version,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	gsize length = 0;
	const gchar *data;

	*version = NULL;
	if (!get_small(conn, "/api/v1/system/status", &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	*version = axis_prowlarr_parse_version(data, length);
	if (*version == NULL)
		return unreadable(error, "status", "it names no version, so it is not Prowlarr's");
	return TRUE;
}

static gboolean
prowlarr_indexers(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	GPtrArray                 **indexers,
	GError                    **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) status = NULL;
	g_autoptr(GArray) failing = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *problem = NULL;
	GPtrArray *list = NULL;
	gsize length = 0;
	const gchar *data;
	guint i;
	guint j;

	*indexers = NULL;
	if (!get_small(conn, "/api/v1/indexer", &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	if (!axis_prowlarr_parse_indexers(data, length, &list, &problem))
		return unreadable(error, "list of indexers", problem);

	/* Which are failing now: a second question whose failure does not fail
	 * the first (unless the source is stopping or refuses the key). */
	if (get_small(conn, "/api/v1/indexerstatus", &status, &local)) {
		data = g_bytes_get_data(status, &length);
		if (axis_prowlarr_parse_failing(data, length, g_get_real_time(), &failing, NULL)) {
			for (i = 0; i < list->len; i++) {
				AxisIndexerInfo *info = g_ptr_array_index(list, i);

				for (j = 0; j < failing->len; j++)
					info->failing = info->failing || g_array_index(failing, gint64, j) == info->id;
			}
		}
	} else if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED) ||
	           g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_AUTH)) {
		g_ptr_array_unref(list);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	*indexers = list;
	return TRUE;
}

/* ── search ─────────────────────────────────────────────────────── */

/* One search, in @mode: what Prowlarr found, read. */
static gboolean
search_once(
	AxisIndexerConn          *conn,
	const AxisIndexerRequest *request,
	AxisIndexerMode           mode,
	AxisIndexerOutcome       *outcome,
	GError                  **error
){
	g_autofree gchar *path = axis_prowlarr_search_path(request, mode);
	g_autofree gchar *problem = NULL;
	g_autoptr(GBytes) body = NULL;
	AxisIndexerGetOptions options;
	gsize length = 0;
	const gchar *data;

	if (path == NULL) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"the search cannot be asked that way");
		return FALSE;
	}
	memset(&options, 0, sizeof(options));
	options.slow = TRUE;
	if (!axis_indexer_conn_get(conn, path, &options, &body, NULL, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	if (!axis_prowlarr_parse_search(data, length, axis_indexer_conn_get_origin(conn),
	        axis_indexer_conn_get_base_path(conn), g_get_real_time(), outcome, &problem))
		return unreadable(error, "search answer", problem);
	outcome->mode = mode;
	return TRUE;
}

/* Whether the answer had anything at all in it, accepted or not. */
static gboolean
has_anything(const AxisIndexerOutcome *outcome)
{
	return (outcome->items != NULL && outcome->items->len > 0) || outcome->n_off_origin > 0 ||
		outcome->n_unusable > 0 || outcome->n_skipped > 0;
}

/*
 * The indexers @request asks (every one, when it names none) that are on and
 * do not take the identifier of @mode for its kind: in @ids, at most
 * %AXIS_INDEXER_MAX_INDEXER_IDS.  *@too_many is set when there are more.
 */
static guint
title_only_indexers(
	const GPtrArray          *indexers,
	const AxisIndexerRequest *request,
	AxisIndexerMode           mode,
	gint64                   *ids,
	gboolean                 *too_many
){
	guint bit = mode == AXIS_INDEXER_BY_IMDB ? AXIS_INDEXER_IDS_IMDB : mode == AXIS_INDEXER_BY_TMDB ?
		AXIS_INDEXER_IDS_TMDB : AXIS_INDEXER_IDS_TVDB;
	guint n = 0;
	guint i;
	guint j;

	*too_many = FALSE;
	for (i = 0; i < indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(indexers, i);
		guint takes = request->kind == AXIS_INDEXER_QUERY_FILM ? info->movie_ids : info->tv_ids;
		gboolean asked = request->n_indexers == 0;

		if (!info->enabled || info->id < 1 || (takes & bit) != 0)
			continue;
		for (j = 0; j < request->n_indexers; j++)
			asked = asked || request->indexers[j] == info->id;
		if (!asked)
			continue;
		if (n == AXIS_INDEXER_MAX_INDEXER_IDS) {
			*too_many = TRUE;
			continue;
		}
		ids[n++] = info->id;
	}
	return n;
}

/*
 * @extra's releases after @outcome's, each once: one whose merge key is in
 * @outcome already (the same result of the same indexer) is left out.  What
 * either left out is counted in @outcome.  @extra is emptied.
 */
static void
merge_outcomes(
	AxisIndexerOutcome *outcome,
	AxisIndexerOutcome *extra
){
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;

	if (outcome->items == NULL)
		outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	for (i = 0; i < outcome->items->len; i++) {
		const AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		if (found->merge_key != NULL)
			g_hash_table_add(seen, found->merge_key);
	}
	for (i = 0; extra->items != NULL && i < extra->items->len; i++) {
		AxisIndexerFound *found = g_ptr_array_index(extra->items, i);

		if (found->merge_key != NULL && g_hash_table_contains(seen, found->merge_key))
			continue;
		if (outcome->items->len >= AXIS_INDEXER_MAX_RESULTS) {
			outcome->truncated = TRUE;
			break;
		}
		if (found->merge_key != NULL)
			g_hash_table_add(seen, found->merge_key);
		g_ptr_array_add(outcome->items, found);
		extra->items->pdata[i] = NULL;
	}
	outcome->n_off_origin += extra->n_off_origin;
	outcome->n_unusable += extra->n_unusable;
	outcome->n_skipped += extra->n_skipped;
	outcome->truncated = outcome->truncated || extra->truncated;
	axis_indexer_outcome_clear(extra);
}

static gboolean
prowlarr_search(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	const AxisIndexerRequest   *request,
	AxisIndexerOutcome         *outcome,
	GError                    **error
){
	g_autofree gchar *title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	const GPtrArray *indexers = axis_indexer_conn_get_indexers(conn);
	AxisIndexerMode modes[3];
	guint n = axis_prowlarr_identifiers(request, modes);

	axis_indexer_outcome_init(outcome);
	if (n == 0) {
		/* No identifier: the words. */
		return search_once(conn, request, AXIS_INDEXER_BY_TEXT, outcome, error);
	}
	/* The first identifier. */
	if (!search_once(conn, request, modes[0], outcome, error))
		return FALSE;
	if (title != NULL && indexers->len > 0) {
		/* An indexer that takes no identifier gives nothing to a search by
		 * one: those are asked by the title as well, and only they. */
		AxisIndexerRequest words = *request;
		AxisIndexerOutcome extra;
		gint64 ids[AXIS_INDEXER_MAX_INDEXER_IDS];
		gboolean too_many = FALSE;
		guint k = title_only_indexers(indexers, request, modes[0], ids, &too_many);

		if (k == 0)
			return TRUE;
		axis_indexer_outcome_init(&extra);
		/* More than a request can name: every indexer asked is asked by the
		 * title too, and the merge drops what both found. */
		if (!too_many) {
			memcpy(words.indexers, ids, sizeof(gint64) * k);
			words.n_indexers = k;
		}
		if (!search_once(conn, &words, AXIS_INDEXER_BY_TEXT, &extra, error)) {
			axis_indexer_outcome_clear(&extra);
			axis_indexer_outcome_clear(outcome);
			return FALSE;
		}
		merge_outcomes(outcome, &extra);
		outcome->mode = modes[0];
		outcome->also_text = TRUE;
		return TRUE;
	}
	/* Which indexers take what is not known yet: the words when the
	 * identifier found nothing at all (an indexer that does not take
	 * identifiers answers an empty list). */
	if (has_anything(outcome) || title == NULL)
		return TRUE;
	axis_indexer_outcome_clear(outcome);
	if (!search_once(conn, request, AXIS_INDEXER_BY_TEXT, outcome, error))
		return FALSE;
	outcome->fell_back = TRUE;
	return TRUE;
}

/* ── fetch ──────────────────────────────────────────────────────── */

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
	const gchar *why = NULL;

	if (!axis_download_magnet_parse(magnet, &hash, &why))
		return refused(error, "Prowlarr gave a magnet link that is not valid");
	if (stated != NULL && g_ascii_strcasecmp(stated, hash) != 0)
		return refused(error, "Prowlarr gave a magnet link for another torrent than the search said");
	fetched->magnet = g_strdup(magnet);
	fetched->info_hash = g_steal_pointer(&hash);
	return TRUE;
}

static gboolean
prowlarr_fetch(
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
	gsize length = 0;
	const guint8 *data;
	gboolean ok;

	fetched->protocol = request->protocol;
	/* A magnet link needs no request: it is all there is to hand over. */
	if (request->magnet != NULL && request->protocol == AXIS_DOWNLOAD_TORRENT)
		return take_magnet(request->magnet, request->info_hash, fetched, error);
	if (request->fetch_path == NULL)
		return refused(error, "there is no way to fetch this release");

	memset(&options, 0, sizeof(options));
	options.accept = FETCH_ACCEPT;
	options.max_bytes = AXIS_INDEXER_MAX_FILE_BYTES;
	options.slow = TRUE;
	memset(&reply, 0, sizeof(reply));
	ok = axis_indexer_conn_fetch_release(conn, request->fetch_path, &options, &body, &reply, &local);
	if (!ok) {
		/* Prowlarr's link address (a result's magnetUrl) answers with a
		 * redirect to the magnet link: read, checked against the hash the
		 * search stated, never followed.  A redirect to the indexer's own
		 * site was followed by the connection when the source allows it, and
		 * any other redirect off the origin refused, with nothing sent
		 * there. */
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT) && reply.magnet != NULL) {
			gboolean taken = request->protocol == AXIS_DOWNLOAD_TORRENT ?
				take_magnet(reply.magnet, request->info_hash, fetched, error) :
				refused(error, "Prowlarr redirected a Usenet release to a magnet link");

			axis_indexer_reply_clear(&reply);
			return taken;
		}
		axis_indexer_reply_clear(&reply);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	axis_indexer_reply_clear(&reply);

	/* What it is is by its bytes, not by what the server says it is. */
	data = g_bytes_get_data(body, &length);
	content = axis_indexer_content_kind(data, length);
	if (content != wanted)
		return refused(error, request->protocol == AXIS_DOWNLOAD_USENET ?
			"what Prowlarr returned is not an NZB file" : "what Prowlarr returned is not a torrent file");
	if (content == AXIS_INDEXER_CONTENT_TORRENT)
		axis_download_torrent_read(data, length, &fetched->info_hash, NULL, NULL);
	fetched->file = g_steal_pointer(&body);
	fetched->file_name = axis_indexer_file_name(request->name, content == AXIS_INDEXER_CONTENT_NZB ? "nzb" : "torrent");
	return TRUE;
}

/* ── The kind ───────────────────────────────────────────────────── */

static const AxisIndexerKindInfo kind = {
	AXIS_INDEXER_PROWLARR_NAME, "Prowlarr", "API key",
	"Prowlarr's own address, such as http://prowlarr:9696, with nothing after the port: if it is served under a "
	"path (a reverse proxy at /prowlarr), the path goes in the base path.",
	"X-Api-Key",
	prowlarr_test, prowlarr_indexers, prowlarr_search, prowlarr_fetch, NULL,
	/* A source of many indexers, its key in a header. */
	FALSE, AXIS_DOWNLOAD_USENET, NULL, NULL,
	FALSE, NULL, NULL,
	FALSE
};

const AxisIndexerKindInfo *
axis_indexer_prowlarr_kind(void)
{
	return &kind;
}
