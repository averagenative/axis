/*
 * axis-indexer-sites-parse.c - YTS, EZTV and The Pirate Bay: their answers,
 * their requests, and the magnet links built from what they list
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-sites-parse.h"

#include <string.h>

#include "probe/axis-probe-json.h"

/* The largest size a release may state (a petabyte). */
#define SIZE_MAX_BYTES ((gint64)1 << 50)

/* The largest count (seeders, peers) believed. */
#define COUNT_MAX 1000000000

/* Dates before this (1990-01-01) and after the year 2100 are not believed. */
#define DATE_MIN_S G_GINT64_CONSTANT(631152000)
#define DATE_MAX_S G_GINT64_CONSTANT(4102444800)

/* A publication date this far ahead of now is not believed. */
#define DATE_FUTURE_SLACK_S G_GINT64_CONSTANT(86400)

/* The most torrents one YTS movie is read for. */
#define YTS_MAX_TORRENTS 32

/* The most characters of a title in a name built from it: with " (1995)" and
 * three words of at most sixteen characters in brackets and " [YTS]", the
 * name stays within AXIS_INDEXER_NAME_MAX_CHARS. */
#define YTS_TITLE_MAX_CHARS 230

/* The Pirate Bay's "nothing found" item: its name and its hash. */
#define TPB_NO_RESULTS_HASH "0000000000000000000000000000000000000000"

/* ── Trackers ───────────────────────────────────────────────────── */

/*
 * The public trackers named by every magnet link built here: the one place
 * that lists them.  They are believed to be alive and open to anyone, as of
 * when this was written (tests/fixtures/sites/README.md); a torrent also finds
 * its peers through the DHT, so a tracker that has gone costs little.
 */
static const gchar *const trackers[] = {
	"udp://tracker.opentrackr.org:1337/announce",
	"udp://open.tracker.cl:1337/announce",
	"udp://open.stealth.si:80/announce",
	"udp://exodus.desync.com:6969/announce",
	"udp://tracker.torrent.eu.org:451/announce",
	"udp://tracker.openbittorrent.com:6969/announce",
	NULL
};

const gchar *const *
axis_sites_trackers(void)
{
	return trackers;
}

/* ── Hashes and magnet links ────────────────────────────────────── */

gboolean
axis_sites_hash_read(
	const gchar  *text,
	gchar       **hash
){
	gboolean zeros = TRUE;
	guint i;

	if (hash != NULL)
		*hash = NULL;
	if (text == NULL || strlen(text) != 40)
		return FALSE;
	for (i = 0; i < 40; i++) {
		if (!g_ascii_isxdigit(text[i]))
			return FALSE;
		zeros = zeros && text[i] == '0';
	}
	if (zeros)
		return FALSE;
	if (hash != NULL)
		*hash = g_ascii_strdown(text, 40);
	return TRUE;
}

/* The link with the name cut to @max_chars (0: no name). */
static gchar *
magnet_with(
	const gchar *hash,
	const gchar *name,
	guint        max_chars
){
	GString *text = g_string_new("magnet:?xt=urn:btih:");
	g_autofree gchar *clean = max_chars > 0 ? axis_indexer_text_clean(name, max_chars) : NULL;
	guint i;

	g_string_append(text, hash);
	if (clean != NULL) {
		g_autofree gchar *escaped = g_uri_escape_string(clean, NULL, FALSE);

		g_string_append_printf(text, "&dn=%s", escaped);
	}
	for (i = 0; trackers[i] != NULL; i++) {
		g_autofree gchar *escaped = g_uri_escape_string(trackers[i], NULL, FALSE);

		g_string_append_printf(text, "&tr=%s", escaped);
	}
	return g_string_free(text, FALSE);
}

gchar *
axis_sites_magnet(
	const gchar *hash,
	const gchar *name
){
	static const guint cuts[] = { AXIS_SITES_DN_MAX_CHARS, 60, 0 };
	guint i;

	if (!axis_download_hash_valid(hash))
		return NULL;
	for (i = 0; i < G_N_ELEMENTS(cuts); i++) {
		gchar *link = magnet_with(hash, name, cuts[i]);

		if (axis_download_magnet_parse(link, NULL, NULL))
			return link;
		g_free(link);
	}
	return NULL;
}

/* ── Requests ───────────────────────────────────────────────────── */

static void
append_param(
	GString     *path,
	const gchar *name,
	const gchar *value
){
	g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

	g_string_append_printf(path, "%c%s=%s", strchr(path->str, '?') != NULL ? '&' : '?', name, escaped);
}

static void
append_number(
	GString     *path,
	const gchar *name,
	guint64      value
){
	g_string_append_printf(path, "%c%s=%" G_GUINT64_FORMAT, strchr(path->str, '?') != NULL ? '&' : '?', name, value);
}

gchar *
axis_sites_yts_path(
	const AxisIndexerRequest *request,
	guint                     page
){
	g_autofree gchar *title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	GString *path;

	if (request->kind != AXIS_INDEXER_QUERY_FILM && request->kind != AXIS_INDEXER_QUERY_TEXT)
		return NULL;
	if (request->imdb_id == NULL && title == NULL)
		return NULL;
	path = g_string_new("/api/v2/list_movies.json");
	append_param(path, "query_term", request->imdb_id != NULL ? request->imdb_id : title);
	append_number(path, "limit", AXIS_SITES_YTS_LIMIT);
	append_number(path, "page", MAX(page, 1u));
	return g_string_free(path, FALSE);
}

gchar *
axis_sites_eztv_path(
	const AxisIndexerRequest *request,
	guint                     page
){
	GString *path;
	guint64 digits;

	if (request->kind == AXIS_INDEXER_QUERY_FILM || request->imdb_id == NULL)
		return NULL;
	/* "tt0700001" is 700001: EZTV's own number has no leading zeros. */
	digits = g_ascii_strtoull(request->imdb_id + 2, NULL, 10);
	if (digits == 0)
		return NULL;
	path = g_string_new("/api/get-torrents");
	append_number(path, "imdb_id", digits);
	append_number(path, "limit", AXIS_SITES_EZTV_LIMIT);
	append_number(path, "page", MAX(page, 1u));
	return g_string_free(path, FALSE);
}

gchar *
axis_sites_tpb_path(const AxisIndexerRequest *request)
{
	g_autofree gchar *title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	g_autofree gchar *words = NULL;
	const gchar *categories;
	GString *path;

	if (title == NULL)
		return NULL;
	switch (request->kind) {
	case AXIS_INDEXER_QUERY_FILM:
		words = request->year > 0 ? g_strdup_printf("%s %d", title, request->year) : g_strdup(title);
		categories = AXIS_SITES_TPB_MOVIE_CATEGORIES;
		break;
	case AXIS_INDEXER_QUERY_EPISODE:
		words = g_strdup_printf("%s S%02dE%02d", title, request->season, request->episode);
		categories = AXIS_SITES_TPB_TV_CATEGORIES;
		break;
	case AXIS_INDEXER_QUERY_SEASON:
		words = g_strdup_printf("%s S%02d", title, request->season);
		categories = AXIS_SITES_TPB_TV_CATEGORIES;
		break;
	default:
		words = g_strdup(title);
		categories = AXIS_SITES_TPB_VIDEO_CATEGORY;
		break;
	}
	path = g_string_new("/q.php");
	append_param(path, "q", words);
	append_param(path, "cat", categories);
	return g_string_free(path, FALSE);
}

gchar *
axis_sites_tpb_imdb_path(const AxisIndexerRequest *request)
{
	GString *path;

	if (request->kind != AXIS_INDEXER_QUERY_FILM || request->imdb_id == NULL ||
	    !axis_indexer_imdb_valid(request->imdb_id))
		return NULL;
	path = g_string_new("/q.php");
	append_param(path, "q", request->imdb_id);
	append_param(path, "cat", AXIS_SITES_TPB_MOVIE_CATEGORIES);
	return g_string_free(path, FALSE);
}

gint
axis_sites_tpb_category(const gchar *text)
{
	gint64 value;

	if (text == NULL || text[0] == '\0' || strlen(text) > 3 || strspn(text, "0123456789") != strlen(text))
		return 0;
	value = g_ascii_strtoll(text, NULL, 10);
	switch (value) {
	case 200:
	case 201:
		return 2000;
	case 202:
		return 2070;
	case 203:
		/* Music videos: music. */
		return 3020;
	case 204:
	case 299:
		return 2999;
	case 205:
		return 5000;
	case 206:
		return 5999;
	case 207:
		return 2040;
	case 208:
		return 5040;
	case 209:
		return 2060;
	case 210:
		return 2030;
	case 211:
		return 2045;
	case 212:
		return 5045;
	case 601:
		return 7020;
	case 602:
		return 7030;
	default:
		break;
	}
	switch (value / 100) {
	case 1:
		return 3000;
	case 3:
		return 4000;
	case 4:
		return 1000;
	case 5:
		return 6000;
	case 6:
		return 8000;
	default:
		return 0;
	}
}

gboolean
axis_sites_category_not_video(gint category)
{
	switch (category / 1000) {
	case 1:
	case 3:
	case 4:
	case 7:
		return TRUE;
	default:
		return FALSE;
	}
}

gchar *
axis_sites_yts_latest_path(void)
{
	return g_strdup("/api/v2/list_movies.json?limit=1&page=1");
}

gchar *
axis_sites_eztv_latest_path(void)
{
	return g_strdup("/api/get-torrents?limit=1&page=1");
}

gchar *
axis_sites_tpb_latest_path(void)
{
	return g_strdup("/q.php?q=1080p&cat=200");
}

/* ── Fields ─────────────────────────────────────────────────────── */

static gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_indexer_text_clean(axis_json_string(axis_json_member(object, name)), max_chars);
}

/* A whole number in range; @text_ok: a string holding one is a number too
 * (EZTV's and The Pirate Bay's are; YTS's are not). */
static gint64
int_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              minimum,
	gint64              maximum,
	gint64              fallback,
	gboolean            text_ok
){
	const AxisJsonNode *node = axis_json_member(object, name);
	gint64 value = 0;

	if (axis_json_type(node) != AXIS_JSON_NUMBER && (!text_ok || axis_json_type(node) != AXIS_JSON_STRING))
		return fallback;
	if (!axis_json_get_integer(node, minimum, maximum, &value))
		return fallback;
	return value;
}

/* A date as seconds since 1970, in microseconds; 0 when not usable. */
static gint64
date_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              now_us,
	gboolean            text_ok
){
	gint64 seconds = int_member(object, name, DATE_MIN_S, DATE_MAX_S, 0, text_ok);

	if (seconds == 0)
		return 0;
	if (now_us > 0 && seconds > now_us / G_USEC_PER_SEC + DATE_FUTURE_SLACK_S)
		return 0;
	return seconds * G_USEC_PER_SEC;
}

/* The IMDb identifier an answer gives as a number or digits (EZTV's: 700001
 * for tt0700001), or as "tt..." text. */
static gchar *
imdb_of(const AxisJsonNode *node)
{
	const gchar *text = axis_json_string(node);
	gint64 value = 0;

	if (text != NULL && axis_indexer_imdb_valid(text))
		return g_strdup(text);
	if ((axis_json_type(node) == AXIS_JSON_NUMBER || axis_json_type(node) == AXIS_JSON_STRING) &&
	    axis_json_get_integer(node, 1, 9999999999, &value))
		return g_strdup_printf("tt%07" G_GINT64_FORMAT, value);
	return NULL;
}

/* Whether @text is one plain word of a release's name: letters, digits, '.'
 * and '-', at most 16 of them. */
static gboolean
plain_word(const gchar *text)
{
	guint i;

	if (text == NULL || text[0] == '\0' || strlen(text) > 16)
		return FALSE;
	for (i = 0; text[i] != '\0'; i++) {
		if (!g_ascii_isalnum(text[i]) && text[i] != '.' && text[i] != '-')
			return FALSE;
	}
	return TRUE;
}

/* ── Releases ───────────────────────────────────────────────────── */

/* Where one answer's releases go. */
typedef struct
{
	AxisIndexerOutcome *outcome;
	GHashTable         *seen;	/* nullable: hash -> TRUE */
	GHashTable         *own;	/* this answer's, when @seen is not given */
	guint               scanned;
} Sink;

static void
sink_init(
	Sink               *sink,
	AxisIndexerOutcome *outcome,
	GHashTable         *seen
){
	sink->outcome = outcome;
	sink->seen = seen;
	sink->own = seen == NULL ? g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL) : NULL;
	sink->scanned = 0;
	if (outcome->items == NULL)
		outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
}

static void
sink_clear(Sink *sink)
{
	g_clear_pointer(&sink->own, g_hash_table_unref);
}

/* Whether no more is read: the scan limit, or the results kept are the most
 * kept. */
static gboolean
sink_full(Sink *sink)
{
	if (sink->scanned >= AXIS_INDEXER_MAX_SCAN || sink->outcome->items->len >= AXIS_INDEXER_MAX_RESULTS) {
		sink->outcome->truncated = TRUE;
		return TRUE;
	}
	return FALSE;
}

/*
 * Keeps @release (taken), which has its name, its info-hash and what else the
 * site said, with the magnet link built for it.  A hash seen before is
 * dropped; a magnet link the downloads layer does not accept skips the item.
 */
static void
sink_add(
	Sink               *sink,
	AxisIndexerRelease *release
){
	GHashTable *seen = sink->seen != NULL ? sink->seen : sink->own;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *key = NULL;
	AxisIndexerFound *found;

	sink->scanned++;
	if (g_hash_table_contains(seen, release->info_hash)) {
		axis_indexer_release_free(release);
		return;
	}
	magnet = axis_sites_magnet(release->info_hash, release->name);
	if (magnet == NULL) {
		sink->outcome->n_skipped++;
		axis_indexer_release_free(release);
		return;
	}
	g_hash_table_add(seen, g_strdup(release->info_hash));
	release->protocol = AXIS_DOWNLOAD_TORRENT;
	release->has_magnet = TRUE;
	key = g_strdup_printf("site|%s", release->info_hash);
	found = g_new0(AxisIndexerFound, 1);
	found->merge_key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
	found->magnet = g_steal_pointer(&magnet);
	found->release = release;
	g_ptr_array_add(sink->outcome->items, found);
}

static void
skip(Sink *sink)
{
	sink->scanned++;
	sink->outcome->n_skipped++;
}

static const gchar *
set_problem(
	gchar      **problem,
	const gchar *text
){
	if (problem != NULL)
		*problem = g_strdup(text);
	return text;
}

/* Reads @data as JSON into @doc; the root is not looked at. */
static AxisJsonDoc *
read_json(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	g_autofree gchar *why = NULL;
	AxisJsonDoc *doc;

	if (data == NULL || length == 0) {
		set_problem(problem, "it is empty");
		return NULL;
	}
	doc = axis_json_parse(data, length, &why);
	if (doc == NULL)
		set_problem(problem, why != NULL ? why : "it is not JSON");
	return doc;
}

/* ── YTS ────────────────────────────────────────────────────────── */

gchar *
axis_sites_yts_name(
	const gchar *title,
	gint         year,
	const gchar *quality,
	const gchar *type,
	const gchar *codec
){
	GString *name = g_string_new(title);
	const gchar *kind = NULL;

	if (year >= 1870 && year <= 2200)
		g_string_append_printf(name, " (%d)", year);
	if (plain_word(quality))
		g_string_append_printf(name, " [%s]", quality);
	if (type != NULL && g_ascii_strcasecmp(type, "bluray") == 0)
		kind = "BluRay";
	else if (type != NULL && g_ascii_strcasecmp(type, "web") == 0)
		kind = "WEB";
	else if (plain_word(type))
		kind = type;
	if (kind != NULL)
		g_string_append_printf(name, " [%s]", kind);
	if (plain_word(codec))
		g_string_append_printf(name, " [%s]", codec);
	g_string_append(name, " [YTS]");
	return g_string_free(name, FALSE);
}

/* A year from "Hearth (1995)": its last four digits in parentheses. */
static gint
year_in(const gchar *title_long)
{
	gsize n = title_long != NULL ? strlen(title_long) : 0;
	gint year;

	if (n < 7 || title_long[n - 1] != ')' || title_long[n - 6] != '(' ||
	    !g_ascii_isdigit(title_long[n - 5]) || !g_ascii_isdigit(title_long[n - 4]) ||
	    !g_ascii_isdigit(title_long[n - 3]) || !g_ascii_isdigit(title_long[n - 2]))
		return 0;
	year = (gint)g_ascii_strtoll(title_long + n - 5, NULL, 10);
	return year >= 1870 && year <= 2200 ? year : 0;
}

static void
yts_movie(
	Sink               *sink,
	const AxisJsonNode *movie,
	gint64              now_us
){
	const AxisJsonNode *torrents = axis_json_member(movie, "torrents");
	g_autofree gchar *title = text_member(movie, "title", AXIS_INDEXER_NAME_MAX_CHARS);
	g_autofree gchar *long_title = text_member(movie, "title_long", AXIS_INDEXER_NAME_MAX_CHARS);
	g_autofree gchar *imdb = NULL;
	const gchar *stated;
	gint year;
	guint i;

	if (axis_json_type(movie) != AXIS_JSON_OBJECT) {
		skip(sink);
		return;
	}
	year = (gint)int_member(movie, "year", 1870, 2200, 0, FALSE);
	if (year == 0)
		year = year_in(long_title);
	/* The title without its year: "Hearth (1995)" is "Hearth" and 1995. */
	if (title == NULL && long_title != NULL) {
		gsize n = strlen(long_title);

		title = year_in(long_title) != 0 ? g_strndup(long_title, n - 7) : g_strdup(long_title);
		g_strstrip(title);
		if (title[0] == '\0')
			g_clear_pointer(&title, g_free);
	}
	/* The name built from it stays within a name's length. */
	if (title != NULL) {
		gchar *cut = axis_indexer_text_clean(title, YTS_TITLE_MAX_CHARS);

		g_free(title);
		title = cut;
	}
	stated = axis_json_string(axis_json_member(movie, "imdb_code"));
	if (stated != NULL && axis_indexer_imdb_valid(stated))
		imdb = g_strdup(stated);
	if (axis_json_type(torrents) != AXIS_JSON_ARRAY)
		return;
	for (i = 0; i < axis_json_length(torrents) && i < YTS_MAX_TORRENTS; i++) {
		const AxisJsonNode *entry = axis_json_element(torrents, i);
		g_autofree gchar *hash = NULL;
		g_autofree gchar *quality = text_member(entry, "quality", 16);
		g_autofree gchar *type = text_member(entry, "type", 16);
		g_autofree gchar *codec = text_member(entry, "video_codec", 16);
		AxisIndexerRelease *release;

		if (sink_full(sink))
			return;
		if (axis_json_type(entry) != AXIS_JSON_OBJECT || title == NULL ||
		    !axis_sites_hash_read(axis_json_string(axis_json_member(entry, "hash")), &hash)) {
			skip(sink);
			continue;
		}
		release = axis_indexer_release_new();
		release->name = axis_sites_yts_name(title, year, quality, type, codec);
		release->info_hash = g_steal_pointer(&hash);
		release->size_bytes = int_member(entry, "size_bytes", 0, SIZE_MAX_BYTES, -1, FALSE);
		release->seeders = (gint)int_member(entry, "seeds", 0, COUNT_MAX, -1, FALSE);
		release->leechers = (gint)int_member(entry, "peers", 0, COUNT_MAX, -1, FALSE);
		release->published_us = date_member(entry, "date_uploaded_unix", now_us, FALSE);
		release->imdb_id = g_strdup(imdb);
		/* Movies. */
		release->n_categories = 1;
		release->categories[0] = 2000;
		sink_add(sink, release);
	}
}

gboolean
axis_sites_parse_yts(
	const gchar         *data,
	gsize                length,
	gint64               now_us,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *body;
	const AxisJsonNode *movies;
	const gchar *status;
	Sink sink;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (read != NULL) {
		read->n_entries = 0;
		read->total = -1;
		read->n_not_video = 0;
	}
	doc = read_json(data, length, problem);
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		set_problem(problem, "it is not an object");
		return FALSE;
	}
	status = axis_json_string(axis_json_member(root, "status"));
	body = axis_json_member(root, "data");
	if (status == NULL && axis_json_type(body) != AXIS_JSON_OBJECT) {
		set_problem(problem, "it is not an answer of YTS's list of movies");
		return FALSE;
	}
	if (status != NULL && g_ascii_strcasecmp(status, "ok") != 0) {
		set_problem(problem, "the site says it could not answer");
		return FALSE;
	}
	if (read != NULL)
		read->total = int_member(body, "movie_count", 0, COUNT_MAX, -1, FALSE);
	sink_init(&sink, outcome, seen);
	movies = axis_json_member(body, "movies");
	if (axis_json_type(movies) == AXIS_JSON_ARRAY) {
		if (read != NULL)
			read->n_entries = axis_json_length(movies);
		for (i = 0; i < axis_json_length(movies) && !sink_full(&sink); i++)
			yts_movie(&sink, axis_json_element(movies, i), now_us);
	}
	sink_clear(&sink);
	return TRUE;
}

/* ── EZTV ───────────────────────────────────────────────────────── */

/* A file name without the extension of a video file ("... .mkv"). */
static gchar *
without_extension(const gchar *name)
{
	static const gchar *const extensions[] = { ".mkv", ".mp4", ".avi", ".m4v", ".ts", ".wmv", NULL };
	gsize n = strlen(name);
	guint i;

	for (i = 0; extensions[i] != NULL; i++) {
		gsize e = strlen(extensions[i]);

		if (n > e && g_ascii_strcasecmp(name + n - e, extensions[i]) == 0)
			return g_strndup(name, n - e);
	}
	return g_strdup(name);
}

gboolean
axis_sites_parse_eztv(
	const gchar         *data,
	gsize                length,
	gint64               now_us,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	const AxisJsonNode *list;
	Sink sink;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (read != NULL) {
		read->n_entries = 0;
		read->total = -1;
		read->n_not_video = 0;
	}
	doc = read_json(data, length, problem);
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT) {
		set_problem(problem, "it is not an object");
		return FALSE;
	}
	list = axis_json_member(root, "torrents");
	if (axis_json_type(list) != AXIS_JSON_ARRAY && axis_json_member(root, "torrents_count") == NULL) {
		set_problem(problem, "it is not an answer of EZTV's list of torrents");
		return FALSE;
	}
	if (read != NULL)
		read->total = int_member(root, "torrents_count", 0, COUNT_MAX, -1, TRUE);
	sink_init(&sink, outcome, seen);
	if (axis_json_type(list) == AXIS_JSON_ARRAY) {
		if (read != NULL)
			read->n_entries = axis_json_length(list);
		for (i = 0; i < axis_json_length(list) && !sink_full(&sink); i++) {
			const AxisJsonNode *entry = axis_json_element(list, i);
			g_autofree gchar *hash = NULL;
			g_autofree gchar *name = NULL;
			g_autofree gchar *filename = NULL;
			AxisIndexerRelease *release;

			if (axis_json_type(entry) != AXIS_JSON_OBJECT ||
			    !axis_sites_hash_read(axis_json_string(axis_json_member(entry, "hash")), &hash)) {
				skip(&sink);
				continue;
			}
			name = text_member(entry, "title", AXIS_INDEXER_NAME_MAX_CHARS);
			if (name == NULL) {
				g_autofree gchar *file = text_member(entry, "filename", AXIS_INDEXER_NAME_MAX_CHARS);

				if (file != NULL)
					filename = without_extension(file);
				name = axis_indexer_text_clean(filename, AXIS_INDEXER_NAME_MAX_CHARS);
			}
			if (name == NULL) {
				skip(&sink);
				continue;
			}
			release = axis_indexer_release_new();
			release->name = g_steal_pointer(&name);
			release->info_hash = g_steal_pointer(&hash);
			release->size_bytes = int_member(entry, "size_bytes", 0, SIZE_MAX_BYTES, -1, TRUE);
			release->seeders = (gint)int_member(entry, "seeds", 0, COUNT_MAX, -1, TRUE);
			release->leechers = (gint)int_member(entry, "peers", 0, COUNT_MAX, -1, TRUE);
			release->published_us = date_member(entry, "date_released_unix", now_us, TRUE);
			release->imdb_id = imdb_of(axis_json_member(entry, "imdb_id"));
			/* TV. */
			release->n_categories = 1;
			release->categories[0] = 5000;
			sink_add(&sink, release);
		}
	}
	sink_clear(&sink);
	return TRUE;
}

/* ── The Pirate Bay ─────────────────────────────────────────────── */

gboolean
axis_sites_parse_tpb(
	const gchar         *data,
	gsize                length,
	gint64               now_us,
	GHashTable          *seen,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	Sink sink;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (read != NULL) {
		read->n_entries = 0;
		read->total = -1;
		read->n_not_video = 0;
	}
	doc = read_json(data, length, problem);
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_ARRAY) {
		set_problem(problem, "it is not a list");
		return FALSE;
	}
	sink_init(&sink, outcome, seen);
	if (read != NULL)
		read->n_entries = axis_json_length(root);
	for (i = 0; i < axis_json_length(root) && !sink_full(&sink); i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		const gchar *stated;
		const gchar *status;
		g_autofree gchar *hash = NULL;
		g_autofree gchar *name = NULL;
		AxisIndexerRelease *release;
		gint category;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT) {
			skip(&sink);
			continue;
		}
		stated = axis_json_string(axis_json_member(entry, "info_hash"));
		/* "No results returned", with a hash of zeros: nothing was found. */
		if (stated != NULL && strcmp(stated, TPB_NO_RESULTS_HASH) == 0) {
			sink.scanned++;
			continue;
		}
		name = text_member(entry, "name", AXIS_INDEXER_NAME_MAX_CHARS);
		if (name == NULL || !axis_sites_hash_read(stated, &hash)) {
			skip(&sink);
			continue;
		}
		category = axis_sites_tpb_category(axis_json_string(axis_json_member(entry, "category")));
		/* Software, music, books and games are no release of a film or an
		 * episode. */
		if (axis_sites_category_not_video(category)) {
			sink.scanned++;
			if (read != NULL)
				read->n_not_video++;
			continue;
		}
		release = axis_indexer_release_new();
		release->name = g_steal_pointer(&name);
		release->info_hash = g_steal_pointer(&hash);
		release->size_bytes = int_member(entry, "size", 0, SIZE_MAX_BYTES, -1, TRUE);
		release->seeders = (gint)int_member(entry, "seeders", 0, COUNT_MAX, -1, TRUE);
		release->leechers = (gint)int_member(entry, "leechers", 0, COUNT_MAX, -1, TRUE);
		release->published_us = date_member(entry, "added", now_us, TRUE);
		if (category != 0) {
			release->n_categories = 1;
			release->categories[0] = category;
		}
		/* The site's word for its uploader: VIP and trusted are shown. */
		status = axis_json_string(axis_json_member(entry, "status"));
		if (g_strcmp0(status, "vip") == 0 || g_strcmp0(status, "trusted") == 0) {
			release->flags = g_new0(gchar *, 2);
			release->flags[0] = g_strdup(strcmp(status, "vip") == 0 ? "VIP" : "Trusted");
		}
		/* Its IMDb identifier is what the uploader typed: not read. */
		sink_add(&sink, release);
	}
	sink_clear(&sink);
	return TRUE;
}
