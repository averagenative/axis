/*
 * axis-indexer-jackett.c - indexer definitions an administrator fetches from
 * Jackett's repository: the list of its files, the files chosen, putting an
 * earlier file back
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing here runs by itself (add-definition-fetch): every request is the
 * consequence of a form an administrator posted.  Two fixed hosts are asked,
 * both over https and with GET only (core/axis-http.h, which follows no
 * redirect to another host):
 *
 *   api.github.com             the branch's commit, then the folder's files
 *                              at that commit (name, size, git blob id)
 *   raw.githubusercontent.com  one file, at that commit
 *
 * What is listed is what is fetched: a file is asked for by the commit of
 * the list kept, and its bytes must have the git blob id the list gave, or
 * it is refused.  A file is then judged by the same loader as one installed
 * by hand (indexer/def/axis-def.h) and stored in the same table, with where
 * it came from.  The file it replaces is kept, once, to be put back.
 */

#include "indexer/axis-indexer-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "indexer/axis-indexer-jackett-parse.h"
#include "indexer/axis-indexer-shipped.h"
#include "indexer/def/axis-def.h"

#define API_BASE "https://api.github.com"
#define RAW_BASE "https://raw.githubusercontent.com"

/* "Jackett/Jackett": the repository's path at both hosts. */
#define REPOSITORY "Jackett/Jackett"

#define COMMIT_PATH  "/repos/" REPOSITORY "/commits/" AXIS_INDEXER_JACKETT_BRANCH
#define LISTING_PATH "/repos/" REPOSITORY "/contents/" AXIS_INDEXER_JACKETT_FOLDER "?ref="
#define RAW_PATH     "/" REPOSITORY "/"

#define PEER "GitHub"

/* The list of a folder of several hundred files, with the addresses GitHub
 * writes beside each. */
#define LISTING_MAX_BYTES  (8 * 1024 * 1024)
#define REQUEST_TIMEOUT_MS 15000

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

static GMutex testing_lock;
static gchar *testing_api;	/* (testing_lock) */
static gchar *testing_raw;	/* (testing_lock) */

/* One asking or fetch at a time, whoever asks. */
static GMutex busy_lock;

void
axis_indexers_jackett_set_testing(
	const gchar *api_base,
	const gchar *raw_base
){
	AxisHttpOrigin origin;
	gboolean loopback = TRUE;

	/* Only this machine, whatever a caller passes. */
	memset(&origin, 0, sizeof(origin));
	if (api_base != NULL) {
		loopback = axis_http_origin_parse(api_base, &origin, NULL) && strcmp(origin.host, "127.0.0.1") == 0;
		axis_http_origin_clear(&origin);
	}
	if (loopback && raw_base != NULL) {
		loopback = axis_http_origin_parse(raw_base, &origin, NULL) && strcmp(origin.host, "127.0.0.1") == 0;
		axis_http_origin_clear(&origin);
	}
	if (!loopback)
		return;
	g_mutex_lock(&testing_lock);
	g_clear_pointer(&testing_api, g_free);
	g_clear_pointer(&testing_raw, g_free);
	testing_api = g_strdup(api_base);
	testing_raw = g_strdup(raw_base);
	g_mutex_unlock(&testing_lock);
}

void
axis_indexer_jackett_listing_free(AxisIndexerJackettListing *listing)
{
	if (listing == NULL)
		return;
	g_free(listing->commit);
	g_clear_pointer(&listing->entries, g_ptr_array_unref);
	g_free(listing);
}

static void
entry_free(gpointer data)
{
	AxisIndexerJackettEntry *entry = data;

	g_free(entry->name);
	g_free(entry->definition_name);
	g_free(entry);
}

void
axis_indexer_jackett_outcome_free(AxisIndexerJackettOutcome *outcome)
{
	if (outcome == NULL)
		return;
	g_free(outcome->name);
	g_free(outcome->message);
	g_free(outcome);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return 0;
	return value;
}

/* ── Requests ───────────────────────────────────────────────────── */

/* One GET to GitHub's API (@raw %FALSE) or its file host. */
static gboolean
ask(
	gboolean           raw,
	const gchar       *path,
	const gchar       *accept,
	gsize              max_bytes,
	guint              timeout_ms,
	GCancellable      *cancellable,
	GBytes           **body,
	AxisHttpResponse  *response,
	GError           **error
){
	static const gchar *const api_headers[] = { "X-GitHub-Api-Version", "2022-11-28", NULL };
	g_autofree gchar *base = NULL;
	g_autofree gchar *problem = NULL;
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	gboolean ok;

	g_mutex_lock(&testing_lock);
	base = g_strdup(raw ? (testing_raw != NULL ? testing_raw : RAW_BASE) : (testing_api != NULL ? testing_api :
		API_BASE));
	g_mutex_unlock(&testing_lock);
	memset(&origin, 0, sizeof(origin));
	memset(response, 0, sizeof(*response));
	if (!axis_http_origin_parse(base, &origin, &problem)) {
		g_set_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG, "the address of %s is not one: %s", PEER, problem);
		return FALSE;
	}
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	config.timeout_ms = timeout_ms;
	config.max_bytes = max_bytes;
	config.cancellable = cancellable;
	config.peer = PEER;
	config.accept = accept;
	config.traffic = AXIS_OUTBOUND_TRAFFIC_DEFINITIONS;
	ok = axis_http_get_full(&config, path, raw ? NULL : api_headers, body, response, error);
	axis_http_origin_clear(&origin);
	return ok;
}

/* "in about 5 minutes", from what the answer asked for. */
static gchar *
wait_words(gint64 retry_after_s)
{
	if (retry_after_s < 0)
		return g_strdup("in an hour");
	if (retry_after_s < 90)
		return g_strdup("in a minute or two");
	if (retry_after_s < 5400)
		return g_strdup_printf("in about %" G_GINT64_FORMAT " minutes", (retry_after_s + 59) / 60);
	return g_strdup_printf("in about %" G_GINT64_FORMAT " hours", (retry_after_s + 3599) / 3600);
}

/* What a failed request means, in a sentence for the page. */
static void
explain(
	const GError            *failure,
	const AxisHttpResponse  *response,
	const gchar             *what,
	GError                 **error
){
	if (g_error_matches(failure, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CANCELLED)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "cancelled");
		return;
	}
	if (g_error_matches(failure, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS) &&
	    (response->status == 403 || response->status == 429)) {
		/* Retry-After when GitHub gave one, else when its allowance returns
		 * (X-RateLimit-Reset, bounded by the client). */
		g_autofree gchar *wait = wait_words(response->retry_after_s >= 0 ? response->retry_after_s :
			response->ratelimit_wait_s);

		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED, "GitHub refused to give %s for now "
			"(status %u). Without an account it answers about 60 requests an hour from one address, and other "
			"programs on your network count too. Nothing was changed. Try again %s.", what, response->status, wait);
		return;
	}
	if (g_error_matches(failure, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_STATUS) && response->status == 404) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "GitHub does not have %s where Axis "
			"looks for it (status 404): Jackett may have moved its definitions. Nothing was changed.", what);
		return;
	}
	if (g_error_matches(failure, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_TOO_LARGE)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE, "GitHub's answer for %s is larger than "
			"Axis accepts. Nothing was changed.", what);
		return;
	}
	g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREACHABLE, "Axis could not get %s from GitHub: %s. "
		"Nothing was changed.", what, failure->message);
}

/* ── The list ───────────────────────────────────────────────────── */

/* "{a.yml,b.yml}": an array of text for a statement.  Every value is of a
 * checked form without a comma, a quote, a brace or a backslash. */
static gchar *
array_text(
	GPtrArray *files,
	guint      what
){
	GString *text = g_string_new("{");
	guint i;

	for (i = 0; i < files->len; i++) {
		const AxisJackettFile *file = g_ptr_array_index(files, i);

		if (i > 0)
			g_string_append_c(text, ',');
		if (what == 0)
			g_string_append(text, file->name);
		else if (what == 1)
			g_string_append(text, file->blob);
		else
			g_string_append_printf(text, "%" G_GINT64_FORMAT, file->size_bytes);
	}
	g_string_append_c(text, '}');
	return g_string_free(text, FALSE);
}

static gboolean
store_listing(
	AxisIndexers  *self,
	const gchar   *commit,
	GPtrArray     *files,
	guint          n_left_out,
	gboolean       cut,
	GError       **error
){
	g_autofree gchar *left_out = g_strdup_printf("%u", n_left_out);
	g_autofree gchar *names = array_text(files, 0);
	g_autofree gchar *blobs = array_text(files, 1);
	g_autofree gchar *sizes = array_text(files, 2);
	const gchar *params[3];
	GError *local = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, &local) &&
		axis_db_conn_exec(conn, "DELETE FROM definition_listing WHERE origin = '" AXIS_INDEXER_ORIGIN_JACKETT "'", 0,
			NULL, NULL, &local);
	if (ok) {
		params[0] = commit;
		params[1] = left_out;
		params[2] = cut ? "true" : "false";
		ok = axis_db_conn_exec(conn, "INSERT INTO definition_listing (origin, commit_id, n_left_out, cut) VALUES ('"
			AXIS_INDEXER_ORIGIN_JACKETT "', $1::text, $2::integer, $3::boolean)", 3, params, NULL, &local);
	}
	if (ok && files->len > 0) {
		params[0] = names;
		params[1] = blobs;
		params[2] = sizes;
		ok = axis_db_conn_exec(conn, "INSERT INTO definition_listing_file (origin, name, blob, size_bytes)"
			" SELECT '" AXIS_INDEXER_ORIGIN_JACKETT "', n, b, s"
			" FROM unnest($1::text[], $2::text[], $3::bigint[]) AS t(n, b, s)", 3, params, NULL, &local);
	}
	if (ok)
		ok = axis_db_conn_commit(conn, &local);
	if (!ok) {
		axis_db_conn_rollback(conn, NULL);
		g_propagate_error(error, local);
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_indexers_jackett_refresh(
	AxisIndexers  *self,
	GCancellable  *cancellable,
	guint         *n_files,
	GError       **error
){
	g_autoptr(GBytes) commit_body = NULL;
	g_autoptr(GBytes) listing_body = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *commit = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *problem = NULL;
	AxisHttpResponse response;
	guint n_left_out = 0;
	gboolean cut = FALSE;
	gboolean ok = FALSE;
	gsize length = 0;
	const gchar *data;

	g_return_val_if_fail(self != NULL, FALSE);

	if (n_files != NULL)
		*n_files = 0;
	if (!g_mutex_trylock(&busy_lock)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_BUSY,
			"Axis is asking Jackett's repository already. Try again when that has ended.");
		return FALSE;
	}
	if (cancellable == NULL)
		cancellable = self->cancel;
	/* The branch's commit, as text: one small answer. */
	if (!ask(FALSE, COMMIT_PATH, "application/vnd.github.sha", 4096, REQUEST_TIMEOUT_MS, cancellable, &commit_body,
	        &response, &failure)) {
		explain(failure, &response, "the list of Jackett's definitions", error);
		goto out;
	}
	axis_http_response_clear(&response);
	data = g_bytes_get_data(commit_body, &length);
	commit = axis_jackett_parse_commit(data, length);
	if (commit == NULL) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE, "GitHub's answer does not "
			"name a commit of Jackett's repository. Nothing was changed.");
		goto out;
	}
	/* The folder at that commit, so that a file fetched later is the one
	 * listed now. */
	path = g_strconcat(LISTING_PATH, commit, NULL);
	if (!ask(FALSE, path, "application/vnd.github+json", LISTING_MAX_BYTES, REQUEST_TIMEOUT_MS * 2, cancellable,
	        &listing_body, &response, &failure)) {
		explain(failure, &response, "the list of Jackett's definitions", error);
		goto out;
	}
	axis_http_response_clear(&response);
	data = g_bytes_get_data(listing_body, &length);
	if (!axis_jackett_parse_listing(data, length, &files, &n_left_out, &cut, &problem)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE, "%s Nothing was changed.", problem);
		goto out;
	}
	if (files->len == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE, "GitHub's list of that folder "
			"holds no definition file Axis takes: Jackett may have moved its definitions. Nothing was changed.");
		goto out;
	}
	ok = store_listing(self, commit, files, n_left_out, cut, error);
	if (ok && n_files != NULL)
		*n_files = files->len;
out:
	axis_http_response_clear(&response);
	g_mutex_unlock(&busy_lock);
	return ok;
}

gboolean
axis_indexers_jackett_get_listing(
	AxisIndexers               *self,
	const gchar                *words,
	AxisIndexerJackettListing **listing,
	GError                    **error
){
	g_autoptr(AxisDbResult) head = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *wanted = words != NULL ? g_ascii_strdown(words, -1) : g_strdup("");
	AxisIndexerJackettListing *found;
	const gchar *params[1];
	guint i;

	g_return_val_if_fail(self != NULL && listing != NULL, FALSE);

	*listing = NULL;
	g_strstrip(wanted);
	if (!axis_db_pool_exec(self->pool, "SELECT l.commit_id, " US("l.listed_at") ", l.n_left_out, l.cut::int,"
	        " (SELECT count(*) FROM definition_listing_file f WHERE f.origin = l.origin)"
	        " FROM definition_listing l WHERE l.origin = '" AXIS_INDEXER_ORIGIN_JACKETT "'", 0, NULL, &head, error))
		return FALSE;
	if (axis_db_result_get_n_rows(head) == 0)
		return TRUE;
	params[0] = wanted;
	/* The installed definition of a file is the one whose id is the file's
	 * name without ".yml".  A fetched one that is still as fetched is compared
	 * by the git blob id kept with it; only a by-hand one has its body read
	 * and hashed. */
	if (!axis_db_pool_exec(self->pool, "SELECT f.name, f.size_bytes, f.blob, d.id, d.name,"
	        " CASE WHEN d.origin IS NULL OR d.sha256 IS DISTINCT FROM d.origin_sha256 THEN d.body END, d.origin,"
	        " (d.origin IS NULL OR d.sha256 IS DISTINCT FROM d.origin_sha256)::int, d.origin_blob,"
	        " (SELECT count(*) FROM indexer_source s WHERE s.definition_id = d.id)"
	        " FROM definition_listing_file f"
	        " LEFT JOIN indexer_definition d ON d.def_id = left(f.name, -4)"
	        " WHERE f.origin = '" AXIS_INDEXER_ORIGIN_JACKETT "' AND ($1::text = '' OR strpos(f.name, $1::text) > 0)"
	        " ORDER BY f.name", 1, params, &rows, error))
		return FALSE;
	found = g_new0(AxisIndexerJackettListing, 1);
	found->commit = g_strdup(axis_db_result_get_text(head, 0, 0));
	found->listed_us = int_at(head, 0, 1);
	found->n_left_out = (guint)int_at(head, 0, 2);
	found->cut = int_at(head, 0, 3) != 0;
	found->n_files = (guint)int_at(head, 0, 4);
	found->entries = g_ptr_array_new_with_free_func(entry_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisIndexerJackettEntry *entry = g_new0(AxisIndexerJackettEntry, 1);
		const gchar *body = axis_db_result_get_text(rows, i, 5);

		entry->name = g_strdup(axis_db_result_get_text(rows, i, 0));
		entry->size_bytes = int_at(rows, i, 1);
		entry->definition_id = int_at(rows, i, 3);
		if (entry->definition_id != 0) {
			g_autofree gchar *hashed = body != NULL ? axis_jackett_blob_id(body, strlen(body)) : NULL;
			const gchar *blob = hashed != NULL ? hashed : axis_db_result_get_text(rows, i, 8);

			entry->definition_name = g_strdup(axis_db_result_get_text(rows, i, 4));
			entry->differs = g_strcmp0(blob, axis_db_result_get_text(rows, i, 2)) != 0;
			entry->from_jackett = axis_db_result_get_text(rows, i, 6) != NULL;
			entry->by_hand = int_at(rows, i, 7) != 0;
			entry->n_indexers = (guint)int_at(rows, i, 9);
		}
		g_ptr_array_add(found->entries, entry);
	}
	*listing = found;
	return TRUE;
}

/* ── One file ───────────────────────────────────────────────────── */

static void
refuse(
	AxisIndexerJackettOutcome *outcome,
	const gchar               *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
refuse(
	AxisIndexerJackettOutcome *outcome,
	const gchar               *format,
	...
){
	va_list args;

	va_start(args, format);
	g_free(outcome->message);
	outcome->message = g_strdup_vprintf(format, args);
	va_end(args);
	outcome->result = AXIS_INDEXER_JACKETT_REFUSED;
	outcome->definition_id = 0;
	outcome->cannot_run = FALSE;
}

/*
 * Stores @body (a definition already judged: @def) as the definition of its
 * id, in one transaction: inserted, or in place of the one installed, which
 * is then kept to be put back.  One installed by hand, or changed by hand
 * since it was fetched, is replaced only with @replace_by_hand.
 */
static gboolean
store_file(
	AxisDbConn                 *conn,
	const AxisDef              *def,
	const gchar                *body,
	const gchar                *file,
	const gchar                *commit,
	const gchar                *blob,
	gboolean                    replace_by_hand,
	AxisIndexerJackettOutcome  *outcome,
	GError                    **error
){
	g_autoptr(AxisDbResult) existing = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, body, -1);
	g_autofree gchar *id_text = NULL;
	const gchar *params[7];
	GError *local = NULL;
	gboolean ok;

	params[0] = def->id;
	ok = axis_db_conn_begin(conn, &local) &&
		axis_db_conn_exec(conn, "SELECT id, sha256, (origin IS NULL OR sha256 IS DISTINCT FROM origin_sha256)::int,"
			" (origin IS NULL)::int, (SELECT count(*) FROM indexer_source s WHERE s.definition_id = indexer_definition.id)"
			" FROM indexer_definition WHERE def_id = $1::text FOR UPDATE", 1, params, &existing, &local);
	if (ok && axis_db_result_get_n_rows(existing) > 0) {
		gboolean same = g_strcmp0(axis_db_result_get_text(existing, 0, 1), sha) == 0;
		gboolean by_hand = int_at(existing, 0, 2) != 0;
		gboolean was_by_hand_file = int_at(existing, 0, 3) != 0;
		gboolean cannot_run = !axis_def_usable(def);

		outcome->n_indexers = (guint)int_at(existing, 0, 4);
		outcome->definition_id = int_at(existing, 0, 0);
		id_text = axis_indexer_int_text(outcome->definition_id);
		params[0] = id_text;
		params[1] = file;
		params[2] = commit;
		params[3] = blob;
		params[4] = sha;
		params[5] = def->name;
		params[6] = body;
		if (same) {
			/* The file installed is this one: only where it is from is
			 * written (when it was fetched stays, if it was). */
			outcome->result = AXIS_INDEXER_JACKETT_UNCHANGED;
			/* Adopted without the tick: nothing is replaced, so nothing is lost. */
			if (was_by_hand_file) {
				g_free(outcome->message);
				outcome->message = g_strdup("The same as the one you installed by hand; now tracked as fetched from "
					"Jackett.");
			}
			ok = axis_db_conn_exec(conn, "UPDATE indexer_definition SET origin = '" AXIS_INDEXER_ORIGIN_JACKETT "',"
				" origin_file = $2::text, origin_commit = $3::text, origin_blob = $4::text, origin_sha256 = $5::text,"
				" origin_at = CASE WHEN origin IS NULL OR origin_sha256 IS DISTINCT FROM $5::text THEN now()"
				" ELSE origin_at END"
				" WHERE id = $1::bigint", 5, params, NULL, &local);
		} else if (by_hand && !replace_by_hand) {
			refuse(outcome, "The definition installed was installed or changed by hand on this Axis, and was left "
				"as it is. To replace it, tick \"Also replace definitions installed or changed by hand\".");
		} else if (cannot_run && outcome->n_indexers > 0 && !replace_by_hand) {
			g_autofree gchar *why = axis_def_problem(def);

			refuse(outcome, "%u indexer%s use%s this definition, and Axis cannot run Jackett's file for it (%s) so "
				"they would not search until the earlier version is put back. It was left as it is. To replace it "
				"anyway, tick \"Also replace definitions installed or changed by hand\".", outcome->n_indexers,
				outcome->n_indexers == 1 ? "" : "s", outcome->n_indexers == 1 ? "s" : "", why != NULL ? why : "it is not "
				"usable");
		} else {
			outcome->result = AXIS_INDEXER_JACKETT_UPDATED;
			outcome->cannot_run = cannot_run;
			/* The right-hand sides read the row as it was. */
			ok = axis_db_conn_exec(conn, "UPDATE indexer_definition SET"
				" previous_body = body, previous_name = name, previous_origin = origin,"
				" previous_commit = origin_commit, previous_blob = origin_blob, previous_sha256 = origin_sha256,"
				" previous_at = origin_at,"
				" name = $6::text, body = $7::text, sha256 = $5::text, updated_at = now(),"
				" origin = '" AXIS_INDEXER_ORIGIN_JACKETT "', origin_file = $2::text, origin_commit = $3::text,"
				" origin_blob = $4::text, origin_sha256 = $5::text, origin_at = now()"
				" WHERE id = $1::bigint", 7, params, NULL, &local) &&
				axis_indexer_definition_changed(conn, id_text, &local);
		}
	} else if (ok) {
		g_autoptr(AxisDbResult) count = NULL;

		ok = axis_db_conn_exec(conn, "SELECT count(*) FROM indexer_definition WHERE def_id NOT LIKE '"
			AXIS_INDEXER_SHIPPED_PREFIX "%'", 0, NULL, &count, &local);
		if (ok && int_at(count, 0, 0) >= AXIS_INDEXER_MAX_DEFINITIONS) {
			refuse(outcome, "At most %d definitions can be installed.", AXIS_INDEXER_MAX_DEFINITIONS);
		} else if (ok) {
			params[0] = def->id;
			params[1] = file;
			params[2] = commit;
			params[3] = blob;
			params[4] = sha;
			params[5] = def->name;
			params[6] = body;
			ok = axis_db_conn_exec(conn, "INSERT INTO indexer_definition (def_id, name, body, sha256, origin,"
				" origin_file, origin_commit, origin_blob, origin_sha256, origin_at)"
				" VALUES ($1::text, $6::text, $7::text, $5::text, '" AXIS_INDEXER_ORIGIN_JACKETT "', $2::text,"
				" $3::text, $4::text, $5::text, now()) RETURNING id", 7, params, &result, &local);
			if (ok) {
				outcome->result = AXIS_INDEXER_JACKETT_INSTALLED;
				outcome->definition_id = int_at(result, 0, 0);
			}
		}
	}
	if (ok)
		ok = axis_db_conn_commit(conn, &local);
	if (!ok) {
		axis_db_conn_rollback(conn, NULL);
		g_propagate_error(error, local);
		return FALSE;
	}
	return TRUE;
}

/*
 * Fetches and installs the file @name, listed with @blob and @size.  A file
 * that is not taken is a refusal in @outcome, not a failure.  @stop (out):
 * GitHub refuses for now, so the files after this one are not asked for.
 */
static gboolean
fetch_one(
	AxisIndexers               *self,
	const gchar                *name,
	const gchar                *commit,
	const gchar                *blob,
	gint64                      size,
	gboolean                    replace_by_hand,
	guint                       timeout_ms,
	GCancellable               *cancellable,
	AxisIndexerJackettOutcome  *outcome,
	gboolean                   *stop,
	GError                    **error
){
	g_autofree gchar *path = g_strconcat(RAW_PATH, commit, "/" AXIS_INDEXER_JACKETT_FOLDER "/", name, NULL);
	g_autofree gchar *stem = g_strndup(name, strlen(name) - 4);
	g_autofree gchar *got = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisDef) def = NULL;
	AxisHttpResponse response;
	AxisDbConn *conn;
	const gchar *data;
	gsize length = 0;
	gboolean ok;

	if (size > AXIS_INDEXER_DEFINITION_MAX_BYTES) {
		refuse(outcome, "It is larger than a definition file may be (%d KiB), and was not asked for.",
			AXIS_INDEXER_DEFINITION_MAX_BYTES / 1024);
		return TRUE;
	}
	if (!ask(TRUE, path, "*/*", AXIS_INDEXER_DEFINITION_MAX_BYTES, timeout_ms, cancellable, &body, &response,
	        &failure)) {
		g_autoptr(GError) said = NULL;

		explain(failure, &response, "this file", &said);
		*stop = g_error_matches(said, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED) ||
			g_error_matches(said, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED);
		refuse(outcome, "%s", said->message);
		axis_http_response_clear(&response);
		return TRUE;
	}
	axis_http_response_clear(&response);
	data = g_bytes_get_data(body, &length);
	if (data == NULL || length == 0) {
		refuse(outcome, "GitHub sent an empty file. Nothing was installed.");
		return TRUE;
	}
	/* What was listed is what is installed, or nothing is. */
	got = axis_jackett_blob_id(data, length);
	if (strcmp(got, blob) != 0) {
		refuse(outcome, "What GitHub sent is not the file that was listed (its content has another git id). "
			"Nothing was installed. See what Jackett has again, then fetch it.");
		return TRUE;
	}
	/* From here it is a file like one installed by hand. */
	def = axis_def_load(data, length, &problem);
	if (def == NULL) {
		refuse(outcome, "That file was not installed. %s", problem);
		return TRUE;
	}
	if (strcmp(def->id, stem) != 0) {
		refuse(outcome, "That file was not installed: the definition in it has the id %.64s, not its file's name.",
			def->id);
		return TRUE;
	}
	if (axis_indexer_shipped_find(def->id) != NULL || axis_indexer_shipped_reserved(def->id)) {
		refuse(outcome, "That file was not installed: ids that begin with \"%s\" are Axis's own built-in "
			"definitions.", AXIS_INDEXER_SHIPPED_PREFIX);
		return TRUE;
	}
	/* The reader took it whole: valid UTF-8 without NUL. */
	text = g_strndup(data, length);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = store_file(conn, def, text, name, commit, blob, replace_by_hand, outcome, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_indexers_jackett_fetch(
	AxisIndexers        *self,
	const gchar *const  *names,
	gboolean             replace_by_hand,
	GCancellable        *cancellable,
	GPtrArray          **outcomes,
	GError             **error
){
	g_autoptr(AxisDbResult) head = NULL;
	g_autoptr(GPtrArray) done = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autofree gchar *commit = NULL;
	gint64 deadline_us;
	GError *failure = NULL;
	gboolean stop = FALSE;
	gboolean database_failed = FALSE;
	gboolean ok = TRUE;
	guint n = 0;
	guint i;

	g_return_val_if_fail(self != NULL && outcomes != NULL, FALSE);

	*outcomes = NULL;
	while (names != NULL && names[n] != NULL)
		n++;
	if (n == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"Tick at least one definition to fetch.");
		return FALSE;
	}
	if (n > AXIS_INDEXER_JACKETT_MAX_PER_FETCH) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "At most %d definitions are fetched at "
			"once, and more were ticked. Nothing was fetched: tick fewer, then fetch the rest.",
			AXIS_INDEXER_JACKETT_MAX_PER_FETCH);
		return FALSE;
	}
	if (!g_mutex_trylock(&busy_lock)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_BUSY,
			"Axis is asking Jackett's repository already. Try again when that has ended.");
		return FALSE;
	}
	if (cancellable == NULL)
		cancellable = self->cancel;
	if (!axis_db_pool_exec(self->pool, "SELECT commit_id FROM definition_listing WHERE origin = '"
	        AXIS_INDEXER_ORIGIN_JACKETT "'", 0, NULL, &head, error)) {
		g_mutex_unlock(&busy_lock);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(head) == 0 || !axis_jackett_id_valid(axis_db_result_get_text(head, 0, 0))) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"Axis has no list of Jackett's definitions yet: select See what Jackett has first.");
		g_mutex_unlock(&busy_lock);
		return FALSE;
	}
	commit = g_strdup(axis_db_result_get_text(head, 0, 0));
	done = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_jackett_outcome_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	deadline_us = g_get_monotonic_time() + (gint64)AXIS_INDEXER_JACKETT_FETCH_SECONDS * G_USEC_PER_SEC;
	for (i = 0; i < n; i++) {
		AxisIndexerJackettOutcome *outcome = g_new0(AxisIndexerJackettOutcome, 1);
		g_autoptr(AxisDbResult) row = NULL;
		const gchar *params[1];
		gint64 left_ms = (deadline_us - g_get_monotonic_time()) / 1000;

		g_ptr_array_add(done, outcome);
		/* A name is outside input: one form only, shown only when it is. */
		if (!axis_jackett_name_valid(names[i])) {
			outcome->name = g_strdup("(a name Axis does not take)");
			refuse(outcome, "A definition's file name is lower-case letters, digits and hyphens, then \".yml\".");
			continue;
		}
		outcome->name = g_strdup(names[i]);
		if (!g_hash_table_add(seen, outcome->name)) {
			refuse(outcome, "It was ticked twice; it was fetched once.");
			continue;
		}
		if (database_failed) {
			refuse(outcome, "It was not asked for, because the database gave an error on the file before it. Fetch it "
				"later.");
			continue;
		}
		if (stop) {
			refuse(outcome, "It was not asked for, because GitHub refused the file before it. Fetch it later.");
			continue;
		}
		if (left_ms < 1000) {
			refuse(outcome, "It was not asked for: the fetch had taken %d seconds. Fetch it again.",
				AXIS_INDEXER_JACKETT_FETCH_SECONDS);
			continue;
		}
		params[0] = names[i];
		ok = axis_db_pool_exec(self->pool, "SELECT blob, size_bytes FROM definition_listing_file WHERE origin = '"
			AXIS_INDEXER_ORIGIN_JACKETT "' AND name = $1::text", 1, params, &row, &failure);
		if (ok && axis_db_result_get_n_rows(row) == 0) {
			refuse(outcome, "It is not in the list read from Jackett. See what Jackett has again.");
			continue;
		}
		if (ok)
			ok = fetch_one(self, names[i], commit, axis_db_result_get_text(row, 0, 0), int_at(row, 0, 1),
				replace_by_hand, (guint)MIN(left_ms, REQUEST_TIMEOUT_MS), cancellable, outcome, &stop, &failure);
		if (!ok) {
			/* The files before this one are installed already: what became of
			 * each is returned, so that the page and the audit lines say so. */
			g_message("indexers: a definition fetched from Jackett could not be stored: %s", failure != NULL ?
				failure->message : "unknown error");
			g_clear_error(&failure);
			refuse(outcome, "Axis could not store it: the database gave an error. Nothing of this file was installed. "
				"Fetch it again later.");
			database_failed = TRUE;
		}
	}
	g_mutex_unlock(&busy_lock);
	*outcomes = g_steal_pointer(&done);
	return TRUE;
}

gboolean
axis_indexers_jackett_put_back(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];
	GError *local = NULL;
	AxisDbConn *conn;
	gboolean found = FALSE;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id_text;
	/* The right-hand sides read the row as it was. */
	ok = axis_db_conn_begin(conn, &local) &&
		axis_db_conn_exec(conn, "UPDATE indexer_definition SET"
			" body = previous_body, name = previous_name,"
			" sha256 = encode(sha256(convert_to(previous_body, 'UTF8')), 'hex'), updated_at = now(),"
			" origin = previous_origin,"
			" origin_file = CASE WHEN previous_origin IS NULL THEN NULL ELSE origin_file END,"
			" origin_commit = previous_commit, origin_blob = previous_blob, origin_sha256 = previous_sha256,"
			" origin_at = previous_at,"
			" previous_body = NULL, previous_name = NULL, previous_origin = NULL, previous_commit = NULL,"
			" previous_blob = NULL, previous_sha256 = NULL, previous_at = NULL"
			" WHERE id = $1::bigint AND previous_body IS NOT NULL AND def_id NOT LIKE '" AXIS_INDEXER_SHIPPED_PREFIX
			"%'", 1, params, &result, &local);
	if (ok) {
		found = axis_db_result_get_affected_rows(result) > 0;
		if (found)
			ok = axis_indexer_definition_changed(conn, id_text, &local);
	}
	if (ok)
		ok = axis_db_conn_commit(conn, &local);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		g_propagate_error(error, local);
		return FALSE;
	}
	if (!found) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"No earlier version of that definition is kept.");
		return FALSE;
	}
	return TRUE;
}
