/*
 * fuzz-indexer-sites.c - libFuzzer target for the reading of the answers of
 * YTS, EZTV and The Pirate Bay (indexer/axis-indexer-sites-parse.h), the
 * magnet links built from what they list, and the requests made to them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' a YTS answer; '1' an EZTV one;
 * '2' The Pirate Bay's; '3' a request, from lines of text, and the paths made
 * from it; '4' a hash and a name, from two lines, and the link built from
 * them); the rest is the input.  Whatever it is, the code returns without
 * crashing or allocating without bound, and what it returns is of the forms
 * the layer relies on:
 *
 *  - an answer is read or refused with a reason; a refusal gives no results;
 *  - at most AXIS_INDEXER_MAX_RESULTS results are kept and no more than
 *    AXIS_INDEXER_MAX_SCAN looked at, each with a name of clean text, the
 *    torrent protocol, a 40-digit lower-case info-hash that is not zeros, a
 *    magnet link the downloads layer accepts and that holds that hash, no
 *    address to fetch from, figures that are -1 or not negative, a date that
 *    is 0 or in the years believed, at most one category, which is video
 *    (movies, TV, adult) or other, at most The Pirate Bay's one flag, and a
 *    merge key that is a hash;
 *  - a hash seen in an earlier answer is not listed again;
 *  - every path made from a request is accepted by axis_indexer_path_ok().
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-sites-parse.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef gboolean (*ParseFunc)(const gchar *, gsize, gint64, GHashTable *, AxisIndexerOutcome *, AxisSitesRead *,
	gchar **);

#define NOW_US (G_GINT64_CONSTANT(1700000000) * G_USEC_PER_SEC)

/* A property that does not hold: which, by its line. */
static void
fail(int line)
{
	fprintf(stderr, "fuzz-indexer-sites: the check at line %d failed\n", line);
	abort();
}

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
check_release(const AxisIndexerFound *found)
{
	const AxisIndexerRelease *r = found->release;
	gchar *hash = NULL;
	guint i;

	if (r == NULL || !clean_text(r->name, AXIS_INDEXER_NAME_MAX_CHARS))
		fail(__LINE__);
	if (r->protocol != AXIS_DOWNLOAD_TORRENT || found->fetch_path != NULL || found->magnet == NULL || !r->has_magnet)
		fail(__LINE__);
	if (!axis_download_hash_valid(r->info_hash) || strspn(r->info_hash, "0") == 40)
		fail(__LINE__);
	if (!axis_download_magnet_parse(found->magnet, &hash, NULL) || strcmp(hash, r->info_hash) != 0)
		fail(__LINE__);
	g_free(hash);
	if (r->size_bytes < -1 || r->seeders < -1 || r->leechers < -1 || r->grabs != -1 || r->published_us < 0 ||
	    r->tmdb_id != 0 || r->tvdb_id != 0 || r->indexer_id != 0 || r->indexer_name != NULL)
		fail(__LINE__);
	if (r->published_us != 0 && (r->published_us < G_GINT64_CONSTANT(631152000) * G_USEC_PER_SEC ||
	    r->published_us > NOW_US + G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC))
		fail(__LINE__);
	if (r->n_categories > 1)
		fail(__LINE__);
	/* Movies, TV, adult or other: never a category that is not video. */
	for (i = 0; i < r->n_categories; i++) {
		gint class_ = r->categories[i] / 1000;

		if ((class_ != 2 && class_ != 5 && class_ != 6 && class_ != 8) || axis_sites_category_not_video(r->categories[i]))
			fail(__LINE__);
	}
	if (r->imdb_id != NULL && !axis_indexer_imdb_valid(r->imdb_id))
		fail(__LINE__);
	/* None, or the one flag of a VIP or trusted uploader. */
	if (r->flags != NULL && (r->flags[0] == NULL || r->flags[1] != NULL ||
	    (strcmp(r->flags[0], "VIP") != 0 && strcmp(r->flags[0], "Trusted") != 0)))
		fail(__LINE__);
	if (found->merge_key == NULL || strlen(found->merge_key) != 64 ||
	    strspn(found->merge_key, "0123456789abcdef") != 64)
		fail(__LINE__);
}

static void
check_answer(
	ParseFunc    parse,
	const gchar *text,
	gsize        length
){
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	gchar *problem = NULL;
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;

	axis_indexer_outcome_init(&outcome);
	if (parse(text, length, NOW_US, seen, &outcome, &read, &problem)) {
		AxisIndexerOutcome again;

		if (problem != NULL || outcome.items == NULL)
			fail(__LINE__);
		if (outcome.items->len > AXIS_INDEXER_MAX_RESULTS)
			fail(__LINE__);
		if (outcome.items->len + outcome.n_skipped > AXIS_INDEXER_MAX_SCAN)
			fail(__LINE__);
		if (outcome.n_off_origin != 0 || outcome.n_unusable != 0)
			fail(__LINE__);
		for (i = 0; i < outcome.items->len; i++)
			check_release(g_ptr_array_index(outcome.items, i));
		/* The same answer again adds nothing, unless it was cut. */
		axis_indexer_outcome_init(&again);
		if (!parse(text, length, NOW_US, seen, &again, NULL, NULL))
			fail(__LINE__);
		if (again.items->len > AXIS_INDEXER_MAX_RESULTS || (!outcome.truncated && again.items->len != 0))
			fail(__LINE__);
		axis_indexer_outcome_clear(&again);
		axis_indexer_outcome_clear(&outcome);
	} else {
		if (problem == NULL || outcome.items != NULL)
			fail(__LINE__);
		g_free(problem);
	}
	g_hash_table_unref(seen);
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
		gchar *paths[4];
		static const gchar *const prefixes[] = {
			"/api/v2/list_movies.json?query_term=", "/api/get-torrents?imdb_id=", "/q.php?q=", "/q.php?q=tt"
		};

		paths[0] = axis_sites_yts_path(&request, 1 + (guint)(numbers[5] & 7));
		paths[1] = axis_sites_eztv_path(&request, 1 + (guint)(numbers[5] & 7));
		paths[2] = axis_sites_tpb_path(&request);
		paths[3] = axis_sites_tpb_imdb_path(&request);
		/* A movie's identifier only. */
		if (paths[3] != NULL && (request.kind != AXIS_INDEXER_QUERY_FILM || request.imdb_id == NULL))
			fail(__LINE__);
		for (i = 0; i < 4; i++) {
			if (paths[i] != NULL && (!axis_indexer_path_ok(paths[i], "") || !g_str_has_prefix(paths[i], prefixes[i])))
				fail(__LINE__);
			g_free(paths[i]);
		}
	}
	g_strfreev(lines);
	g_free(copy);
}

/* A hash and a name, one on each line. */
static void
check_magnet(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar **lines = g_strsplit(copy, "\n", 3);
	guint count = g_strv_length(lines);
	const gchar *hash = count > 0 ? lines[0] : "";
	const gchar *name = count > 1 ? lines[1] : NULL;
	gchar *read_hash = NULL;
	gchar *magnet = axis_sites_magnet(hash, name);
	gchar *parsed = NULL;

	if (magnet != NULL) {
		if (!axis_download_hash_valid(hash) || !axis_download_magnet_parse(magnet, &parsed, NULL) ||
		    strcmp(parsed, hash) != 0 || strlen(magnet) > AXIS_DOWNLOAD_MAGNET_MAX)
			fail(__LINE__);
		g_free(parsed);
	} else if (axis_download_hash_valid(hash)) {
		/* A valid hash always has a link: without a name at the worst. */
		fail(__LINE__);
	}
	if (axis_sites_hash_read(hash, &read_hash)) {
		if (!axis_download_hash_valid(read_hash) || g_ascii_strcasecmp(read_hash, hash) != 0)
			fail(__LINE__);
		g_free(read_hash);
	} else if (read_hash != NULL) {
		fail(__LINE__);
	}
	g_free(magnet);
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
		check_answer(axis_sites_parse_yts, text, length);
		break;
	case '1':
		check_answer(axis_sites_parse_eztv, text, length);
		break;
	case '2':
		check_answer(axis_sites_parse_tpb, text, length);
		break;
	case '3':
		check_request(text, length);
		break;
	case '4':
		check_magnet(text, length);
		break;
	default:
		break;
	}
	return 0;
}
