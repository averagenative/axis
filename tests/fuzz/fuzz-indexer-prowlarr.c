/*
 * fuzz-indexer-prowlarr.c - libFuzzer target for the Prowlarr kind's reading
 * of Prowlarr's answers (indexer/axis-indexer-prowlarr-parse.h) and for the
 * checks of what a source returns (indexer/axis-indexer-model.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' a search answer for a source at
 * the top of its origin; '1' for one served under /prowlarr; '2' the list of
 * indexers; '3' their status; '4' the version; '5' a fetched file, by its
 * bytes; '6' a fetch address, against both bases; '7' a request, from lines
 * of text: its checks, its key and the search paths made from it); the rest
 * is the input.  Whatever it is, the code returns without crashing or
 * allocating without bound, and what it returns is of the forms the layer
 * applies its own checks to:
 *
 *  - a search answer is a list (and then every result counted: kept, off
 *    the origin, unusable or skipped, at most the scan limit in all) or
 *    refused with a reason;
 *  - at most AXIS_INDEXER_MAX_RESULTS results are kept, each with a name
 *    that is clean text of at most 300 characters, a protocol, a way to
 *    fetch it (a path that axis_indexer_path_ok() accepts and that is under
 *    the base path, or a magnet link the downloads layer accepts, for a
 *    torrent only), counts that are -1 or not negative, at most 16
 *    categories in range without repeats, at most 8 flags of 32 characters,
 *    and an IMDb identifier and an info-hash of their forms;
 *  - the indexers are at most AXIS_INDEXER_MAX_INDEXERS, with a name, one of
 *    two protocols and an identifier once;
 *  - a file is an NZB or a torrent or neither, and a torrent is one the
 *    bencode reader accepts;
 *  - a fetch address that is accepted gives a path that is under the base
 *    path and that has no dot part, and nothing else is ever a path;
 *  - a request that passes its check can be keyed, and every search path
 *    made from it is accepted by axis_indexer_path_ok().
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "download/axis-download-bencode.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-prowlarr-parse.h"

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
	return text[0] != ' ' && text[strlen(text) - 1] != ' ' && strstr(text, "  ") == NULL;
}

static void
check_release(
	const AxisIndexerFound *found,
	const gchar            *base
){
	const AxisIndexerRelease *r = found->release;
	guint i;
	guint j;

	if (r == NULL || !clean_text(r->name, AXIS_INDEXER_NAME_MAX_CHARS))
		abort();
	if (r->protocol != AXIS_DOWNLOAD_USENET && r->protocol != AXIS_DOWNLOAD_TORRENT)
		abort();
	if (found->fetch_path == NULL && found->magnet == NULL)
		abort();
	if (found->fetch_path != NULL && (!axis_indexer_path_ok(found->fetch_path, base) ||
	    (base[0] != '\0' && strncmp(found->fetch_path, base, strlen(base)) != 0)))
		abort();
	if (found->magnet != NULL && (r->protocol != AXIS_DOWNLOAD_TORRENT || !axis_download_magnet_parse(found->magnet, NULL, NULL)))
		abort();
	if (r->has_magnet != (found->magnet != NULL))
		abort();
	if (r->indexer_name != NULL && !clean_text(r->indexer_name, AXIS_INDEXER_INDEXER_MAX_CHARS))
		abort();
	if (r->size_bytes < -1 || r->seeders < -1 || r->leechers < -1 || r->grabs < -1 || r->published_us < 0 ||
	    r->indexer_id < 0 || r->tmdb_id < 0 || r->tvdb_id < 0)
		abort();
	if (r->protocol == AXIS_DOWNLOAD_USENET && (r->seeders != -1 || r->leechers != -1 || r->info_hash != NULL))
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
	/* The merge key is a hash, never the guid itself (which can hold a
	 * passkey or be a whole magnet link). */
	if (found->merge_key == NULL || strlen(found->merge_key) != 64 ||
	    strspn(found->merge_key, "0123456789abcdef") != 64)
		abort();
}

static void
check_search(
	const gchar *text,
	gsize        length,
	const gchar *base
){
	AxisHttpOrigin origin;
	AxisIndexerOutcome outcome;
	gchar *problem = NULL;
	guint i;

	if (!axis_http_origin_parse("http://prowlarr.test:9696", &origin, NULL))
		abort();
	if (axis_prowlarr_parse_search(text, length, &origin, base, 1700000000 * G_GINT64_CONSTANT(1000000), &outcome,
	        &problem)) {
		if (problem != NULL || outcome.items == NULL)
			abort();
		if (outcome.items->len > AXIS_INDEXER_MAX_RESULTS)
			abort();
		if (outcome.items->len + outcome.n_off_origin + outcome.n_unusable + outcome.n_skipped > AXIS_INDEXER_MAX_SCAN)
			abort();
		for (i = 0; i < outcome.items->len; i++)
			check_release(g_ptr_array_index(outcome.items, i), base);
		axis_indexer_outcome_clear(&outcome);
	} else {
		if (problem == NULL || outcome.items != NULL)
			abort();
		g_free(problem);
	}
	axis_http_origin_clear(&origin);
}

static void
check_indexers(
	const gchar *text,
	gsize        length
){
	GPtrArray *indexers = NULL;
	gchar *problem = NULL;
	guint i;
	guint j;

	if (axis_prowlarr_parse_indexers(text, length, &indexers, &problem)) {
		if (indexers == NULL || problem != NULL || indexers->len > AXIS_INDEXER_MAX_INDEXERS)
			abort();
		for (i = 0; i < indexers->len; i++) {
			const AxisIndexerInfo *info = g_ptr_array_index(indexers, i);

			if (info->id < 0 || !clean_text(info->name, AXIS_INDEXER_INDEXER_MAX_CHARS) ||
			    (info->protocol != AXIS_DOWNLOAD_USENET && info->protocol != AXIS_DOWNLOAD_TORRENT) ||
			    info->movie_ids > 7 || info->tv_ids > 7)
				abort();
			for (j = 0; j < i; j++) {
				if (((const AxisIndexerInfo *)g_ptr_array_index(indexers, j))->id == info->id)
					abort();
			}
		}
		g_ptr_array_unref(indexers);
	} else {
		if (problem == NULL || indexers != NULL)
			abort();
		g_free(problem);
	}
}

static void
check_failing(
	const gchar *text,
	gsize        length
){
	GArray *ids = NULL;
	gchar *problem = NULL;
	guint i;

	if (axis_prowlarr_parse_failing(text, length, 1700000000 * G_GINT64_CONSTANT(1000000), &ids, &problem)) {
		if (ids == NULL || problem != NULL)
			abort();
		for (i = 0; i < ids->len; i++) {
			if (g_array_index(ids, gint64, i) < 0)
				abort();
		}
		g_array_unref(ids);
	} else {
		if (problem == NULL || ids != NULL)
			abort();
		g_free(problem);
	}
}

static void
check_version(
	const gchar *text,
	gsize        length
){
	gchar *version = axis_prowlarr_parse_version(text, length);

	if (version != NULL && !clean_text(version, 64))
		abort();
	g_free(version);
}

static void
check_content(
	const guint8 *data,
	gsize         length
){
	AxisIndexerContent kind = axis_indexer_content_kind(data, length);

	if (kind == AXIS_INDEXER_CONTENT_TORRENT && !axis_download_torrent_read(data, length, NULL, NULL, NULL))
		abort();
	if (kind == AXIS_INDEXER_CONTENT_NZB && (length < 8 || memchr(data, '<', length) == NULL))
		abort();
}

static void
check_ref(
	const gchar *text,
	gsize        length
){
	static const gchar *const bases[] = { "", "/prowlarr", "/a/b" };
	static const gchar *const origins[] = { "http://prowlarr.test:9696", "https://Indexers.test" };
	gchar *url = g_strndup(text, length);
	guint b;
	guint o;

	for (o = 0; o < G_N_ELEMENTS(origins); o++) {
		AxisHttpOrigin origin;

		if (!axis_http_origin_parse(origins[o], &origin, NULL))
			abort();
		for (b = 0; b < G_N_ELEMENTS(bases); b++) {
			gchar *path = NULL;
			AxisIndexerRefResult result = axis_indexer_fetch_ref(url, &origin, bases[b], &path);

			if (result == AXIS_INDEXER_REF_OK) {
				/* No dot part in the path (the query is the source's own text). */
				gsize path_length = strcspn(path != NULL ? path : "", "?");
				gchar *only_path = g_strndup(path != NULL ? path : "", path_length);

				if (path == NULL || !axis_indexer_path_ok(path, bases[b]) || strstr(only_path, "/../") != NULL ||
				    strstr(only_path, "/./") != NULL || g_str_has_suffix(only_path, "/..") ||
				    g_str_has_suffix(only_path, "/."))
					abort();
				g_free(only_path);
			} else if (path != NULL) {
				abort();
			}
			g_free(path);
		}
		axis_http_origin_clear(&origin);
	}
	/* A path as it would be asked of the source, whatever it is. */
	(void)axis_indexer_path_ok(url, "");
	(void)axis_indexer_path_ok(url, "/prowlarr");
	g_free(url);
}

/* A request from lines: title, IMDb identifier, then numbers as text. */
static void
check_request(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar **lines = g_strsplit(copy, "\n", 8);
	AxisIndexerRequest request;
	guint count = g_strv_length(lines);
	gint64 numbers[6] = { 0, 0, 0, -1, -1, 0 };
	guint i;

	axis_indexer_request_init(&request, (AxisIndexerQueryKind)(count > 0 && lines[0][0] != '\0' ?
		(guint)(lines[0][0] - '0') % 5 : 0));
	request.title = count > 1 ? lines[1] : NULL;
	request.imdb_id = count > 2 && lines[2][0] != '\0' ? lines[2] : NULL;
	for (i = 3; i < count && i < 8; i++)
		numbers[i - 3] = g_ascii_strtoll(lines[i], NULL, 10);
	request.year = (gint)numbers[0];
	request.tmdb_id = numbers[1];
	request.tvdb_id = numbers[2];
	request.season = (gint)numbers[3];
	request.episode = (gint)numbers[4];
	if (axis_indexer_request_check(&request, NULL)) {
		AxisIndexerMode modes[3];
		guint n = axis_prowlarr_identifiers(&request, modes);
		gchar *key = axis_indexer_request_key(&request);
		gchar *path;

		if (strlen(key) != 64)
			abort();
		g_free(key);
		for (i = 0; i < n; i++) {
			path = axis_prowlarr_search_path(&request, modes[i]);
			if (path == NULL || !axis_indexer_path_ok(path, "") || !g_str_has_prefix(path, "/api/v1/search?query="))
				abort();
			g_free(path);
		}
		path = axis_prowlarr_search_path(&request, AXIS_INDEXER_BY_TEXT);
		if (path != NULL && (!axis_indexer_path_ok(path, "") || !g_str_has_prefix(path, "/api/v1/search?query=")))
			abort();
		/* A request that is not an anime series' never names the anime
		 * category by itself (the words are escaped: no title can). */
		if (path != NULL && strstr(path, "&categories=5070") != NULL)
			abort();
		/* The same request for an anime series (add-anime-support, A3): a
		 * path as safe, in TV and Anime for everything but a film, which is
		 * asked for as it was; and a key of its own. */
		{
			AxisIndexerRequest anime = request;
			gchar *plain_key = axis_indexer_request_key(&request);
			gchar *anime_key;
			gchar *anime_path;

			anime.anime = TRUE;
			if (!axis_indexer_request_check(&anime, NULL))
				abort();
			anime_key = axis_indexer_request_key(&anime);
			if (strlen(anime_key) != 64 || strcmp(anime_key, plain_key) == 0)
				abort();
			anime_path = axis_prowlarr_search_path(&anime, AXIS_INDEXER_BY_TEXT);
			if ((anime_path == NULL) != (path == NULL))
				abort();
			if (anime_path != NULL) {
				gboolean both = strstr(anime_path, "&categories=5000&categories=5070") != NULL;

				if (!axis_indexer_path_ok(anime_path, "") || !g_str_has_prefix(anime_path, "/api/v1/search?query="))
					abort();
				if (request.kind == AXIS_INDEXER_QUERY_FILM ? strcmp(anime_path, path) != 0 : !both)
					abort();
			}
			for (i = 0; i < n; i++) {
				gchar *by_id = axis_prowlarr_search_path(&anime, modes[i]);

				if (by_id == NULL || !axis_indexer_path_ok(by_id, ""))
					abort();
				g_free(by_id);
			}
			g_free(anime_path);
			g_free(anime_key);
			g_free(plain_key);
		}
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
		check_search(text, length, "");
		break;
	case '1':
		check_search(text, length, "/prowlarr");
		break;
	case '2':
		check_indexers(text, length);
		break;
	case '3':
		check_failing(text, length);
		break;
	case '4':
		check_version(text, length);
		break;
	case '5':
		check_content(data + 1, length);
		break;
	case '6':
		check_ref(text, length);
		break;
	case '7':
		check_request(text, length);
		break;
	default:
		break;
	}
	return 0;
}
