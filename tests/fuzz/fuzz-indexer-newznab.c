/*
 * fuzz-indexer-newznab.c - libFuzzer target for the reading of Newznab and
 * Torznab answers (indexer/axis-indexer-newznab-parse.h) and the searches
 * made from a request
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' a capabilities answer; '1' a
 * Newznab search answer; '2' a Torznab one; '3' a Newznab one with an other
 * origin accepted for its files; '4' a fetched answer read as an error
 * document; '5' a date; '6' a request, from lines of text, and the paths
 * made from it against the capabilities the next lines describe); the rest
 * is the input.  Whatever it is, the code returns without crashing or
 * allocating without bound, and what it returns is of the forms the layer
 * relies on:
 *
 *  - an answer is read, or an error document, or refused with a reason; an
 *    error document or a refusal gives no results and no allowances;
 *  - at most AXIS_INDEXER_MAX_RESULTS results are kept, each with a name of
 *    clean text, the protocol asked for, a way to fetch it (a path that
 *    axis_indexer_path_ok() accepts, on the accepted other origin only when
 *    one is, or a valid magnet link for a torrent), figures that are -1 or
 *    not negative, categories in range without repeats, flags of clean text,
 *    an IMDb identifier and an info-hash of their forms, and a merge key
 *    that is a hash;
 *  - an origin a result named elsewhere is an origin as one is configured;
 *  - capabilities are bits within their range and limits within theirs;
 *  - a date is 0 or within the years believed;
 *  - every search path made is accepted by axis_indexer_path_ok() and holds
 *    no key.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-newznab-parse.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static gboolean
clean_text(
	const gchar *text,
	guint        max_chars
){
	const gchar *p;

	if (text == NULL || text[0] == '\0' || !g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > (glong)max_chars)
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

static void
check_release(
	const AxisIndexerFound *found,
	AxisDownloadProtocol    protocol,
	gboolean                files
){
	const AxisIndexerRelease *r = found->release;
	guint i;
	guint j;

	if (r == NULL || !clean_text(r->name, AXIS_INDEXER_NAME_MAX_CHARS) || r->protocol != protocol)
		abort();
	if (found->fetch_path == NULL && found->magnet == NULL)
		abort();
	if (found->fetch_path != NULL && !axis_indexer_path_ok(found->fetch_path, ""))
		abort();
	if (found->files_origin && (!files || found->fetch_path == NULL))
		abort();
	if (found->magnet != NULL && (protocol != AXIS_DOWNLOAD_TORRENT ||
	    !axis_download_magnet_parse(found->magnet, NULL, NULL)))
		abort();
	if (r->has_magnet != (found->magnet != NULL))
		abort();
	if (r->size_bytes < -1 || r->seeders < -1 || r->leechers < -1 || r->grabs < -1 || r->published_us < 0 ||
	    r->tmdb_id < 0 || r->tvdb_id < 0)
		abort();
	if (protocol == AXIS_DOWNLOAD_USENET && (r->seeders != -1 || r->leechers != -1 || r->info_hash != NULL))
		abort();
	if (r->n_categories > AXIS_INDEXER_MAX_CATEGORIES)
		abort();
	for (i = 0; i < r->n_categories; i++) {
		if (r->categories[i] < 1 || r->categories[i] > 999999)
			abort();
		for (j = 0; j < i; j++) {
			if (r->categories[j] == r->categories[i])
				abort();
		}
	}
	if (r->flags != NULL) {
		if (g_strv_length(r->flags) > AXIS_INDEXER_MAX_FLAGS || r->flags[0] == NULL)
			abort();
		for (i = 0; r->flags[i] != NULL; i++) {
			if (!clean_text(r->flags[i], AXIS_INDEXER_FLAG_MAX_CHARS))
				abort();
		}
	}
	if (r->imdb_id != NULL && !axis_indexer_imdb_valid(r->imdb_id))
		abort();
	if (r->info_hash != NULL && !axis_download_hash_valid(r->info_hash))
		abort();
	if (found->merge_key == NULL || strlen(found->merge_key) != 64 ||
	    strspn(found->merge_key, "0123456789abcdef") != 64)
		abort();
}

static void
check_allowance(
	const AxisIndexerAllowance *a,
	gboolean                    none
){
	if (a->api_used < -1 || a->api_max < -1 || a->grab_used < -1 || a->grab_max < -1)
		abort();
	if (none && (a->api_used != -1 || a->api_max != -1 || a->grab_used != -1 || a->grab_max != -1 ||
	    a->reported_us != 0))
		abort();
}

static void
check_search(
	const gchar          *text,
	gsize                 length,
	AxisDownloadProtocol  protocol,
	gboolean              files
){
	AxisHttpOrigin origin;
	AxisHttpOrigin other;
	AxisNewznabContext context;
	AxisIndexerOutcome outcome;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	gchar *problem = NULL;
	guint i;

	if (!axis_http_origin_parse("https://indexer.test", &origin, NULL) ||
	    !axis_http_origin_parse("https://files.indexer.test", &other, NULL))
		abort();
	memset(&context, 0, sizeof(context));
	context.origin = &origin;
	context.files_origin = files ? &other : NULL;
	context.protocol = protocol;
	context.now_us = 1700000000 * G_GINT64_CONSTANT(1000000);
	switch (axis_newznab_parse_search(text, length, &context, &outcome, &allowance, &document, &problem)) {
	case AXIS_NEWZNAB_READ_OK:
		if (outcome.items == NULL || problem != NULL || outcome.items->len > AXIS_INDEXER_MAX_RESULTS)
			abort();
		if (outcome.items->len + outcome.n_off_origin + outcome.n_unusable + outcome.n_skipped > AXIS_INDEXER_MAX_SCAN)
			abort();
		for (i = 0; i < outcome.items->len; i++)
			check_release(g_ptr_array_index(outcome.items, i), protocol, files);
		if (outcome.other_origin != NULL) {
			AxisHttpOrigin seen;

			if (!axis_http_origin_parse(outcome.other_origin, &seen, NULL) ||
			    strcmp(seen.base, outcome.other_origin) != 0)
				abort();
			axis_http_origin_clear(&seen);
		}
		check_allowance(&allowance, FALSE);
		axis_indexer_outcome_clear(&outcome);
		break;
	case AXIS_NEWZNAB_READ_ERROR:
		if (outcome.items != NULL || problem != NULL)
			abort();
		if (document.description != NULL && !clean_text(document.description, 200))
			abort();
		check_allowance(&allowance, TRUE);
		axis_newznab_error_clear(&document);
		break;
	default:
		if (outcome.items != NULL || problem == NULL || document.description != NULL)
			abort();
		check_allowance(&allowance, TRUE);
		g_free(problem);
		break;
	}
	axis_http_origin_clear(&origin);
	axis_http_origin_clear(&other);
}

static void
check_caps(
	const gchar *text,
	gsize        length
){
	AxisIndexerCaps caps;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	gchar *version = NULL;
	gchar *problem = NULL;
	guint i;

	switch (axis_newznab_parse_caps(text, length, &caps, &allowance, &version, &document, &problem)) {
	case AXIS_NEWZNAB_READ_OK:
		if (problem != NULL || caps.modes > 7 || caps.movie_ids > 7 || caps.tv_ids > 7 || caps.limit_max < 0 ||
		    caps.limit_max > 100000 || caps.limit_default < 0 || caps.limit_default > 100000)
			abort();
		if (version != NULL && !clean_text(version, 64))
			abort();
		if (caps.categories != NULL) {
			if (g_strv_length(caps.categories) > AXIS_INDEXER_MAX_CAPS_CATEGORIES || caps.categories[0] == NULL)
				abort();
			for (i = 0; caps.categories[i] != NULL; i++) {
				if (!g_utf8_validate(caps.categories[i], -1, NULL) || !g_ascii_isdigit(caps.categories[i][0]))
					abort();
			}
		}
		check_allowance(&allowance, FALSE);
		axis_indexer_caps_clear(&caps);
		g_free(version);
		break;
	case AXIS_NEWZNAB_READ_ERROR:
		if (version != NULL || problem != NULL || caps.categories != NULL)
			abort();
		check_allowance(&allowance, TRUE);
		axis_newznab_error_clear(&document);
		break;
	default:
		if (version != NULL || problem == NULL || caps.categories != NULL)
			abort();
		check_allowance(&allowance, TRUE);
		g_free(problem);
		break;
	}
}

static void
check_error_only(
	const gchar *text,
	gsize        length
){
	AxisNewznabError document;

	if (axis_newznab_parse_error_only(text, length, &document)) {
		if (document.description != NULL && !clean_text(document.description, 200))
			abort();
		(void)axis_newznab_error_kind(document.code);
		axis_newznab_error_clear(&document);
	} else if (document.description != NULL) {
		abort();
	}
}

static void
check_date(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gint64 us = axis_newznab_parse_date(copy);

	/* 1990 to 2100, give or take a zone. */
	if (us != 0 && (us < G_GINT64_CONSTANT(631000000) * G_USEC_PER_SEC ||
	    us > G_GINT64_CONSTANT(4103000000) * G_USEC_PER_SEC))
		abort();
	g_free(copy);
}

/* A request from lines: kind, title, IMDb identifier, numbers; then the
 * capabilities' bits. */
static void
check_request(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar **lines = g_strsplit(copy, "\n", 10);
	AxisIndexerRequest request;
	AxisIndexerCaps caps;
	guint count = g_strv_length(lines);
	gint64 numbers[7] = { 0, 0, 0, -1, -1, 0, 0 };
	const gint categories[] = { 2000, 5000 };
	guint i;

	axis_indexer_request_init(&request, (AxisIndexerQueryKind)(count > 0 && lines[0][0] != '\0' ?
		(guint)(lines[0][0] - '0') % 5 : 0));
	request.title = count > 1 ? lines[1] : NULL;
	request.imdb_id = count > 2 && lines[2][0] != '\0' ? lines[2] : NULL;
	for (i = 3; i < count && i < 10; i++)
		numbers[i - 3] = g_ascii_strtoll(lines[i], NULL, 10);
	request.year = (gint)numbers[0];
	request.tmdb_id = numbers[1];
	request.tvdb_id = numbers[2];
	request.season = (gint)numbers[3];
	request.episode = (gint)numbers[4];
	memset(&caps, 0, sizeof(caps));
	caps.read_us = numbers[5] != 0 ? 1 : 0;
	caps.modes = (guint)(numbers[6] & 7);
	caps.movie_ids = (guint)((numbers[6] >> 3) & 7);
	caps.tv_ids = (guint)((numbers[6] >> 6) & 7);
	caps.movie_text = (numbers[6] & 512) != 0;
	caps.tv_text = (numbers[6] & 1024) != 0;
	caps.tv_numbers = (numbers[6] & 2048) != 0;
	caps.limit_max = (gint)((numbers[6] >> 12) & 0xfff);
	if (axis_indexer_request_check(&request, NULL)) {
		AxisIndexerMode modes[3];
		guint n = axis_newznab_identifiers(&request, &caps, modes);
		gchar *path;

		if (n > 3)
			abort();
		for (i = 0; i <= n; i++) {
			path = axis_newznab_search_path(&request, i < n ? modes[i] : AXIS_INDEXER_BY_TEXT, &caps, categories, 2);
			if (i < n && path == NULL)
				abort();
			/* No key parameter (the words are escaped: a title that says
			 * "apikey=" cannot make one). */
			if (path != NULL && (!axis_indexer_path_ok(path, "") || !g_str_has_prefix(path, "/api?t=") ||
			    strstr(path, "&apikey=") != NULL || strstr(path, "?apikey=") != NULL))
				abort();
			g_free(path);
		}
		path = axis_newznab_latest_path(categories, 2, (guint)numbers[0]);
		if (!axis_indexer_path_ok(path, ""))
			abort();
		g_free(path);
	}
	g_strfreev(lines);
	g_free(copy);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text;
	gsize length;

	if (size < 1 || size > 1 << 20)
		return 0;
	text = (const gchar *)data + 1;
	length = size - 1;
	switch (data[0]) {
	case '0':
		check_caps(text, length);
		break;
	case '1':
		check_search(text, length, AXIS_DOWNLOAD_USENET, FALSE);
		break;
	case '2':
		check_search(text, length, AXIS_DOWNLOAD_TORRENT, FALSE);
		break;
	case '3':
		check_search(text, length, AXIS_DOWNLOAD_USENET, TRUE);
		break;
	case '4':
		check_error_only(text, length);
		break;
	case '5':
		check_date(text, length);
		break;
	case '6':
		check_request(text, length);
		break;
	default:
		break;
	}
	return 0;
}
