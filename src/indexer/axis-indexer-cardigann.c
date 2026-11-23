/*
 * axis-indexer-cardigann.c - the kind that runs an installed definition
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The engine (indexer/def/axis-def-engine.h) says what to ask and reads
 * what comes back; this file asks, through the connection, and writes the
 * messages from fixed text and the site's cleaned, scrubbed words.
 */

#include "indexer/axis-indexer-cardigann.h"

#include <string.h>

#include "download/axis-download-bencode.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"

/* A wait is cut into steps this long, to see a cancellation. */
#define WAIT_STEP_US (100 * 1000)

/* ── The definition ─────────────────────────────────────────────── */

typedef struct
{
	AxisDef        *def;
	AxisDefSite     site;
} Running;

static void
running_clear(Running *running)
{
	g_clear_pointer(&running->def, axis_def_free);
}

/* The indexer's definition, judged now: it runs only when usable. */
static gboolean
running_open(
	AxisIndexerConn  *conn,
	Running          *running,
	GError          **error
){
	const gchar *text = axis_indexer_conn_get_definition(conn);
	g_autofree gchar *why = NULL;

	memset(running, 0, sizeof(*running));
	if (text == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s has no installed definition to run", axis_indexer_conn_get_label(conn));
		return FALSE;
	}
	running->def = axis_def_load(text, strlen(text), &why);
	if (running->def == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s's definition cannot be read by this version of Axis: %s", axis_indexer_conn_get_label(conn),
			why != NULL ? why : "it is not one");
		return FALSE;
	}
	if (!axis_def_usable(running->def)) {
		g_autofree gchar *problem = axis_def_problem(running->def);

		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s's definition is not usable yet: %s", axis_indexer_conn_get_label(conn), problem);
		running_clear(running);
		return FALSE;
	}
	running->site.def = running->def;
	running->site.origin = axis_indexer_conn_get_origin(conn);
	running->site.base_path = axis_indexer_conn_get_base_path(conn);
	running->site.now_us = g_get_real_time();
	return TRUE;
}

/* What the definition can be searched for, as capabilities. */
static void
tell_caps(
	AxisIndexerConn *conn,
	const AxisDef   *def
){
	AxisIndexerCaps caps;

	memset(&caps, 0, sizeof(caps));
	if (def->modes & AXIS_DEF_MODE_SEARCH)
		caps.modes |= AXIS_INDEXER_CAN_SEARCH;
	if (def->covers_movies || def->general)
		caps.modes |= AXIS_INDEXER_CAN_MOVIE;
	if (def->covers_tv || def->general)
		caps.modes |= AXIS_INDEXER_CAN_TV;
	if (def->movie_params & AXIS_DEF_PARAM_IMDB)
		caps.movie_ids |= AXIS_INDEXER_IDS_IMDB;
	if (def->tv_params & AXIS_DEF_PARAM_IMDB)
		caps.tv_ids |= AXIS_INDEXER_IDS_IMDB;
	caps.movie_text = TRUE;
	caps.tv_text = TRUE;
	caps.tv_numbers = (def->tv_params & (AXIS_DEF_PARAM_SEASON | AXIS_DEF_PARAM_EP)) != 0;
	axis_indexer_conn_learn_caps(conn, &caps);
}

/* ── Asking ─────────────────────────────────────────────────────── */

static const gchar *
accept_of(AxisDefResponse response)
{
	switch (response) {
	case AXIS_DEF_JSON:
		return "application/json";
	case AXIS_DEF_XML:
		return "application/rss+xml, application/xml, text/xml";
	default:
		return "text/html, application/xhtml+xml";
	}
}

/*
 * One GET of @path on the indexer's origin, @file for a release's torrent
 * file (its type and size).  Every request of a definition, its file's too,
 * is the connection's ordinary one (axis_indexer_conn_get_ref()), never the
 * release fetch that may follow a redirect off the origin: a redirect off it
 * is refused, nothing sent there, and one to a magnet link is reported in
 * @reply.  What a public site says no to, or where it has moved, is the site
 * failing (as for the native sites): the connection's errors for those are
 * made into failures that say what to do.
 */
static gboolean
ask(
	AxisIndexerConn     *conn,
	const AxisDef       *def,
	const gchar         *path,
	AxisDefResponse      response,
	const gchar *const  *headers,
	gboolean             file,
	GBytes             **body,
	AxisIndexerReply    *reply,
	gboolean            *challenge,
	GError             **error
){
	AxisIndexerGetOptions options;
	AxisIndexerReply own;
	g_autoptr(GError) local = NULL;
	const gchar *label = axis_indexer_conn_get_label(conn);
	gboolean ok;

	memset(&own, 0, sizeof(own));
	if (reply == NULL)
		reply = &own;
	if (challenge != NULL)
		*challenge = FALSE;

	memset(&options, 0, sizeof(options));
	options.accept = file ? "application/x-bittorrent, */*" : accept_of(response);
	options.max_bytes = file ? AXIS_INDEXER_MAX_FILE_BYTES : AXIS_CARDIGANN_ANSWER_BYTES;
	options.timeout_ms = AXIS_INDEXER_NATIVE_TIMEOUT_MS;
	options.slow = file;
	options.headers = headers;
	ok = axis_indexer_conn_get_ref(conn, path, &options, body, reply, &local);
	if (ok) {
		axis_indexer_reply_clear(&own);
		return TRUE;
	}
	if (local->domain != AXIS_INDEXER_ERROR) {
		axis_indexer_reply_clear(&own);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	/* What a site known to challenge says when it does (its definition
	 * carries the setting for a browser helper), an answer that says it is
	 * a challenge ("cf-mitigated: challenge", with a 403 or a 503), and a
	 * refusal that is a challenge's page (Cloudflare's managed challenge
	 * answers 403). */
	if (reply->challenged || (reply->status == 403 && axis_def_is_challenge(reply->refusal)) ||
	    (def != NULL && def->challenge_hint && (reply->status == 403 || reply->status == 503))) {
		if (reply->challenged || (reply->status == 403 && axis_def_is_challenge(reply->refusal)))
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
				"%s answered %u with a browser challenge in front of its pages, which Axis cannot pass: it is not "
				"usable from this address, it needs a browser helper (a later stage)", label, reply->status);
		else
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
				"%s answered %u, as it does when it puts a browser challenge in front of a program: it is not "
				"usable yet from here, it needs a browser helper (a later stage)", label, reply->status);
		if (challenge != NULL)
			*challenge = TRUE;
		axis_indexer_reply_clear(&own);
		return FALSE;
	}
	axis_indexer_reply_clear(&own);
	switch ((AxisIndexerError)local->code) {
	case AXIS_INDEXER_ERROR_REDIRECT:
		/* A file's redirect to a magnet link is the caller's to read. */
		if (file && reply != NULL && reply->magnet != NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			break;
		}
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s redirects to another address, which Axis does not follow, so nothing was sent there: if the site "
			"has moved, change its address here", label);
		break;
	case AXIS_INDEXER_ERROR_AUTH:
	case AXIS_INDEXER_ERROR_REFUSED: {
		g_autofree gchar *said = axis_indexer_conn_scrub(conn, local->message, 200);

		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED,
			"%s did not answer as its definition says: %s (the site may have changed, or moved: then change its "
			"address here)", label, said != NULL ? said : "it refused the request");
		break;
	}
	case AXIS_INDEXER_ERROR_TOO_LARGE:
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE,
			"%s's answer is larger than any of its pages should be", label);
		break;
	default:
		g_propagate_error(error, g_steal_pointer(&local));
		break;
	}
	return FALSE;
}

/* Waits until @seconds after @last (monotonic), at most the cap: a
 * definition's requestDelay between its own requests (the site rate's
 * limiter keeps the time between searches). */
static void
pause_after(
	gint64  last,
	gdouble seconds
){
	gint64 until = last + (gint64)(MIN(seconds, (gdouble)AXIS_CARDIGANN_MAX_DELAY_S) * G_USEC_PER_SEC);
	gint64 now;

	while ((now = g_get_monotonic_time()) < until)
		g_usleep((gulong)MIN((gint64)WAIT_STEP_US, until - now));
}

static gboolean
unreadable(
	AxisIndexerConn  *conn,
	GError          **error,
	const gchar      *problem
){
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE,
		"%s's answer cannot be read as its definition says (the site may have changed its pages): %s",
		axis_indexer_conn_get_label(conn), problem != NULL ? problem : "it is not what was expected");
	return FALSE;
}

/*
 * A site whose answer is not what its definition reads (drift) or a browser
 * challenge: searches pass it over for a while, saying so, unless this is a
 * test or a check (which a person asked for, to see).
 */
static void
hold_for(
	AxisIndexerConn *conn,
	gboolean         hold,
	gboolean         drift
){
	g_autofree gchar *message = NULL;

	if (!hold || axis_indexer_conn_get_trace(conn) != NULL)
		return;
	message = g_strconcat(axis_indexer_conn_get_label(conn), drift ? AXIS_INDEXER_HOLD_DRIFT_WORDS :
		AXIS_INDEXER_HOLD_CHALLENGE_WORDS, NULL);
	axis_indexer_conn_hold_searches(conn, AXIS_CARDIGANN_HOLD_S, message);
}

/* The failure of drift, in words that say what to do. */
static gboolean
drifted(
	AxisIndexerConn  *conn,
	const gchar      *problem,
	GError          **error
){
	const gchar *detail = problem != NULL && g_str_has_prefix(problem, AXIS_DEF_DRIFT_PROBLEM ": ") ?
		problem + strlen(AXIS_DEF_DRIFT_PROBLEM ": ") : problem;

	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE,
		"%s" AXIS_CARDIGANN_DRIFT_WORDS " (%s): the site may have changed them, or its address may belong to "
		"someone else now. Searches pass it over for an hour; Check this indexer on its page shows what was read",
		axis_indexer_conn_get_label(conn), detail != NULL ? detail : "they are not what its definition reads");
	return FALSE;
}

/* Asks every request of @search, reading the answers into @outcome.  An
 * answer after the first that fails ends there, the outcome cut.  @hold: a
 * first answer that is drift or a challenge holds the indexer's searches. */
static gboolean
run_search(
	AxisIndexerConn     *conn,
	const Running       *running,
	AxisDefSearch       *search,
	AxisIndexerOutcome  *outcome,
	gboolean             hold,
	GError             **error
){
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	AxisDefTrace *trace = axis_indexer_conn_get_trace(conn);
	gint64 last = 0;
	guint i;

	axis_def_search_set_trace(search, trace);
	for (i = 0; i < axis_def_search_n_requests(search); i++) {
		const AxisDefRequest *request = axis_def_search_request(search, i);
		g_autoptr(GBytes) body = NULL;
		g_autoptr(GError) local = NULL;
		g_autofree gchar *problem = NULL;
		AxisIndexerReply reply;
		AxisDefAnswerInfo info;
		gboolean challenge = FALSE;
		gboolean read;
		gsize length = 0;
		const gchar *data;

		if (i > 0 && running->def->request_delay > 0)
			pause_after(last, running->def->request_delay);
		last = g_get_monotonic_time();
		memset(&reply, 0, sizeof(reply));
		if (!ask(conn, running->def, request->path, request->response, (const gchar *const *)request->headers, FALSE,
		         &body, &reply, &challenge, &local)) {
			axis_def_trace_add_failure(trace, request->path, reply.status, local->message);
			axis_indexer_reply_clear(&reply);
			if (challenge && i == 0)
				hold_for(conn, hold, FALSE);
			if (i == 0 || g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			outcome->truncated = TRUE;
			return TRUE;
		}
		data = g_bytes_get_data(body, &length);
		read = axis_def_search_read_full(search, i, data, length, seen, outcome, &info, &problem);
		/* On this answer's record only: none when the trace had no room. */
		if (info.answer != NULL)
			info.answer->status = reply.status;
		axis_indexer_reply_clear(&reply);
		if (!read) {
			if (i > 0) {
				outcome->truncated = TRUE;
				return TRUE;
			}
			if (info.verdict == AXIS_DEF_ANSWER_CHALLENGE) {
				hold_for(conn, hold, FALSE);
				g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED, "%s: %s",
					axis_indexer_conn_get_label(conn), problem);
				return FALSE;
			}
			if (info.verdict == AXIS_DEF_ANSWER_DRIFT) {
				hold_for(conn, hold, TRUE);
				return drifted(conn, problem, error);
			}
			return unreadable(conn, error, problem);
		}
	}
	return TRUE;
}

/* ── The kind's functions ───────────────────────────────────────── */

static gboolean
cardigann_search(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	const AxisIndexerRequest   *request,
	AxisIndexerOutcome         *outcome,
	GError                    **error
){
	const AxisIndexerNative *native = axis_indexer_conn_get_native(conn);
	g_autoptr(AxisDefSearch) search = NULL;
	g_autofree gchar *problem = NULL;
	Running running;
	gboolean film;
	guint i;

	axis_indexer_outcome_init(outcome);
	outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	outcome->mode = AXIS_INDEXER_BY_TEXT;
	if (!running_open(conn, &running, error)) {
		axis_indexer_outcome_clear(outcome);
		return FALSE;
	}
	if (native->caps->read_us == 0)
		tell_caps(conn, running.def);
	/* What its categories do not cover is not asked. */
	film = request->kind == AXIS_INDEXER_QUERY_FILM;
	if ((film && !running.def->covers_movies && !running.def->general) ||
	    ((request->kind == AXIS_INDEXER_QUERY_EPISODE || request->kind == AXIS_INDEXER_QUERY_SEASON ||
	      (request->anime && !film)) && !running.def->covers_tv && !running.def->general)) {
		running_clear(&running);
		axis_indexer_outcome_nothing_here(outcome, request);
		return TRUE;
	}
	search = axis_def_search_new(&running.site, request, &problem);
	if (search == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "%s cannot be searched for this: %s",
			axis_indexer_conn_get_label(conn), problem);
		running_clear(&running);
		axis_indexer_outcome_clear(outcome);
		return FALSE;
	}
	/* A movie known only by an identifier this site is not searched by
	 * gives it no words: its paths would list its newest releases (what a
	 * test asks for), which are not this title's.  Nothing is asked. */
	if (!axis_def_search_has_words(search)) {
		running_clear(&running);
		g_free(outcome->not_asked);
		outcome->not_asked = g_strdup("it is searched by words, and this search has none (only an identifier it "
			"cannot be searched by)");
		return TRUE;
	}
	if (!run_search(conn, &running, search, outcome, TRUE, error)) {
		running_clear(&running);
		axis_indexer_outcome_clear(outcome);
		return FALSE;
	}
	outcome->mode = axis_def_search_mode(search);
	for (i = 0; i < outcome->items->len; i++) {
		AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		g_free(found->release->indexer_name);
		found->release->indexer_name = axis_indexer_text_clean(axis_indexer_conn_get_label(conn),
			AXIS_INDEXER_INDEXER_MAX_CHARS);
		found->release->indexer_id = 0;
	}
	running_clear(&running);
	return TRUE;
}

/*
 * A test asks for what the definition lists without words (its newest),
 * and reads the answer as its definition says: what a site at an address
 * that is no longer its own answers is not that, and is shown as failing.
 */
static gboolean
cardigann_test(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	gchar                     **version,
	GError                    **error
){
	g_autoptr(AxisDefSearch) search = NULL;
	g_autofree gchar *problem = NULL;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Running running;
	gboolean ok;

	*version = NULL;
	if (!running_open(conn, &running, error))
		return FALSE;
	tell_caps(conn, running.def);
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	search = axis_def_search_new(&running.site, &request, &problem);
	if (search == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "%s cannot be asked for its newest: %s",
			axis_indexer_conn_get_label(conn), problem);
		running_clear(&running);
		return FALSE;
	}
	axis_indexer_outcome_init(&outcome);
	ok = run_search(conn, &running, search, &outcome, FALSE, error);
	if (ok)
		*version = g_strdup_printf("%s, by its definition: %u releases on its newest page", running.def->name,
			outcome.items != NULL ? outcome.items->len : 0);
	axis_indexer_outcome_clear(&outcome);
	running_clear(&running);
	return ok;
}

static gboolean
cardigann_indexers(
	const AxisIndexerKindInfo  *self,
	AxisIndexerConn            *conn,
	GPtrArray                 **indexers,
	GError                    **error
){
	AxisIndexerInfo *info = axis_indexer_info_new();
	const gchar *text = axis_indexer_conn_get_definition(conn);
	g_autoptr(AxisDef) def = text != NULL ? axis_def_load(text, strlen(text), NULL) : NULL;

	info->id = 0;
	info->name = axis_indexer_text_clean(axis_indexer_conn_get_label(conn), AXIS_INDEXER_INDEXER_MAX_CHARS);
	info->protocol = AXIS_DOWNLOAD_TORRENT;
	info->enabled = TRUE;
	info->movie_ids = def != NULL && (def->movie_params & AXIS_DEF_PARAM_IMDB) ? AXIS_INDEXER_IDS_IMDB : 0;
	info->tv_ids = def != NULL && (def->tv_params & AXIS_DEF_PARAM_IMDB) ? AXIS_INDEXER_IDS_IMDB : 0;
	info->host = axis_indexer_host_key(axis_indexer_conn_get_origin(conn)->host);
	*indexers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_info_free);
	g_ptr_array_add(*indexers, info);
	return TRUE;
}

static gboolean
refused(
	GError      **error,
	const gchar  *text
){
	g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED, text);
	return FALSE;
}

/* A magnet link the downloads layer accepts, of the hash the search said
 * when it said one. */
static gboolean
take_magnet(
	const gchar         *magnet,
	const gchar         *info_hash,
	AxisIndexerFetched  *fetched,
	GError             **error
){
	g_autofree gchar *hash = NULL;

	if (magnet == NULL || strlen(magnet) > AXIS_DOWNLOAD_MAGNET_MAX || !axis_download_magnet_parse(magnet, &hash, NULL))
		return refused(error, "the release's magnet link is not valid");
	if (info_hash != NULL && g_ascii_strcasecmp(info_hash, hash) != 0)
		return refused(error, "the release's magnet link is for another torrent than the search said");
	fetched->magnet = g_strdup(magnet);
	fetched->info_hash = g_ascii_strdown(hash, -1);
	return TRUE;
}

/* The torrent file at @path on the origin. */
static gboolean
fetch_file(
	AxisIndexerConn               *conn,
	const AxisDef                 *def,
	const AxisIndexerFetchRequest *request,
	const gchar                   *path,
	AxisIndexerFetched            *fetched,
	GError                       **error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) local = NULL;
	AxisIndexerReply reply;
	const guint8 *data;
	gsize length = 0;

	memset(&reply, 0, sizeof(reply));
	if (!ask(conn, def, path, AXIS_DEF_HTML, NULL, TRUE, &body, &reply, NULL, &local)) {
		/* A download answered with its magnet link: read, never followed. */
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT) && reply.magnet != NULL) {
			gboolean taken = take_magnet(reply.magnet, request->info_hash, fetched, error);

			axis_indexer_reply_clear(&reply);
			return taken;
		}
		axis_indexer_reply_clear(&reply);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	axis_indexer_reply_clear(&reply);
	data = g_bytes_get_data(body, &length);
	if (axis_indexer_content_kind(data, length) != AXIS_INDEXER_CONTENT_TORRENT)
		return refused(error, "what the site returned is not a torrent file");
	axis_download_torrent_read(data, length, &fetched->info_hash, NULL, NULL);
	if (request->info_hash != NULL && fetched->info_hash != NULL &&
	    g_ascii_strcasecmp(request->info_hash, fetched->info_hash) != 0) {
		g_clear_pointer(&fetched->info_hash, g_free);
		return refused(error, "the torrent the site returned is another than the search said");
	}
	fetched->file = g_steal_pointer(&body);
	fetched->file_name = axis_indexer_file_name(request->name, "torrent");
	return TRUE;
}

static gboolean
cardigann_fetch(
	const AxisIndexerKindInfo     *self,
	AxisIndexerConn               *conn,
	const AxisIndexerFetchRequest *request,
	AxisIndexerFetched            *fetched,
	GError                       **error
){
	g_autoptr(GBytes) page = NULL;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *file_path = NULL;
	g_autofree gchar *problem = NULL;
	Running running;
	const gchar *data;
	gsize length = 0;
	gboolean ok;

	fetched->protocol = AXIS_DOWNLOAD_TORRENT;
	/* A release with its magnet link needs no request. */
	if (request->magnet != NULL)
		return take_magnet(request->magnet, request->info_hash, fetched, error);
	if (request->fetch_path == NULL)
		return refused(error, "there is no way to fetch this release");
	if (!running_open(conn, &running, error))
		return FALSE;
	if (!axis_def_download_is_page(running.def)) {
		ok = fetch_file(conn, running.def, request, request->fetch_path, fetched, error);
		running_clear(&running);
		return ok;
	}
	/* The release's page, where the definition reads its download. */
	if (!ask(conn, running.def, request->fetch_path, AXIS_DEF_HTML, NULL, FALSE, &page, NULL, NULL, error)) {
		running_clear(&running);
		return FALSE;
	}
	data = g_bytes_get_data(page, &length);
	running.site.now_us = g_get_real_time();
	if (!axis_def_read_download(&running.site, request->fetch_path, data, length, &magnet, &file_path, &problem)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED, "%s: %s", axis_indexer_conn_get_label(conn),
			problem);
		running_clear(&running);
		return FALSE;
	}
	ok = magnet != NULL ? take_magnet(magnet, request->info_hash, fetched, error) :
		fetch_file(conn, running.def, request, file_path, fetched, error);
	running_clear(&running);
	return ok;
}

static const AxisIndexerKindInfo cardigann = {
	AXIS_INDEXER_CARDIGANN_NAME, "Site definition", "",
	"One of the site's addresses its definition lists, or the one it is at now if it has moved: http or https, "
	"its host and port, and a path when the site is under one. Axis asks only this address.",
	NULL,
	cardigann_test, cardigann_indexers, cardigann_search, cardigann_fetch, NULL,
	TRUE, AXIS_DOWNLOAD_TORRENT, NULL, NULL,
	TRUE, NULL, NULL,
	TRUE
};

const AxisIndexerKindInfo *
axis_indexer_cardigann_kind(void)
{
	return &cardigann;
}
