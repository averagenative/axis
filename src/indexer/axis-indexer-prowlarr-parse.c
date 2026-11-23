/*
 * axis-indexer-prowlarr-parse.c - Prowlarr's JSON answers, read into the
 * indexer layer's words, and the search path
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-prowlarr-parse.h"

#include <string.h>

/* The largest size a release may state (a petabyte). */
#define SIZE_MAX_BYTES ((gint64)1 << 50)

/* The largest count (seeders, grabs) believed. */
#define COUNT_MAX 1000000000

/* Dates before this and after the far future are not believed. */
#define DATE_MIN_YEAR 1990
#define DATE_MAX_YEAR 2100

/* A publication date this far ahead of now is not believed. */
#define DATE_FUTURE_SLACK_US (G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC)

/* ── Fields ─────────────────────────────────────────────────────── */

static gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_indexer_text_clean(axis_json_string(axis_json_member(object, name)), max_chars);
}

static gint64
int_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              minimum,
	gint64              maximum,
	gint64              fallback
){
	const AxisJsonNode *node = axis_json_member(object, name);
	gint64 value = 0;

	if (axis_json_type(node) != AXIS_JSON_NUMBER && axis_json_type(node) != AXIS_JSON_STRING)
		return fallback;
	if (!axis_json_get_integer(node, minimum, maximum, &value))
		return fallback;
	return value;
}

/* A date as ISO 8601 text, in microseconds since 1970; 0 when not usable. */
static gint64
date_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              now_us
){
	const gchar *text = axis_json_string(axis_json_member(object, name));
	GDateTime *when;
	gint64 us;
	gint year;

	if (text == NULL || text[0] == '\0' || strlen(text) > 64)
		return 0;
	when = g_date_time_new_from_iso8601(text, NULL);
	if (when == NULL)
		return 0;
	year = g_date_time_get_year(when);
	us = g_date_time_to_unix(when) * G_USEC_PER_SEC;
	g_date_time_unref(when);
	if (year < DATE_MIN_YEAR || year > DATE_MAX_YEAR)
		return 0;
	if (now_us > 0 && us > now_us + DATE_FUTURE_SLACK_US)
		return 0;
	return us;
}

static gboolean
protocol_member(
	const AxisJsonNode   *object,
	AxisDownloadProtocol *protocol
){
	const gchar *text = axis_json_string(axis_json_member(object, "protocol"));
	g_autofree gchar *lower = text != NULL && strlen(text) <= 16 ? g_ascii_strdown(text, -1) : NULL;

	/* An indexer has Usenet or torrent releases, whatever it says. */
	return lower != NULL && axis_download_protocol_from_string(lower, protocol) &&
		(*protocol == AXIS_DOWNLOAD_USENET || *protocol == AXIS_DOWNLOAD_TORRENT);
}

/* The categories of a result: ids (and their names, which are not kept). */
static void
read_categories(
	const AxisJsonNode *object,
	AxisIndexerRelease *release
){
	const AxisJsonNode *list = axis_json_member(object, "categories");
	guint i;
	guint j;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return;
	for (i = 0; i < axis_json_length(list) && release->n_categories < AXIS_INDEXER_MAX_CATEGORIES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const AxisJsonNode *id = axis_json_type(entry) == AXIS_JSON_OBJECT ? axis_json_member(entry, "id") : entry;
		gint64 value = 0;
		gboolean seen = FALSE;

		if ((axis_json_type(id) != AXIS_JSON_NUMBER && axis_json_type(id) != AXIS_JSON_STRING) ||
		    !axis_json_get_integer(id, 1, 999999, &value))
			continue;
		for (j = 0; j < release->n_categories; j++)
			seen = seen || release->categories[j] == (gint)value;
		if (!seen)
			release->categories[release->n_categories++] = (gint)value;
	}
}

/* The flags of a result ("freeleech"): strings only. */
static void
read_flags(
	const AxisJsonNode *object,
	AxisIndexerRelease *release
){
	const AxisJsonNode *list = axis_json_member(object, "indexerFlags");
	GPtrArray *flags;
	guint i;
	guint j;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return;
	flags = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(list) && flags->len < AXIS_INDEXER_MAX_FLAGS; i++) {
		gchar *flag = axis_indexer_text_clean(axis_json_string(axis_json_element(list, i)),
			AXIS_INDEXER_FLAG_MAX_CHARS);
		gboolean seen = FALSE;

		if (flag == NULL)
			continue;
		for (j = 0; j < flags->len; j++)
			seen = seen || strcmp(g_ptr_array_index(flags, j), flag) == 0;
		if (seen)
			g_free(flag);
		else
			g_ptr_array_add(flags, flag);
	}
	if (flags->len == 0) {
		g_ptr_array_unref(flags);
		return;
	}
	g_ptr_array_add(flags, NULL);
	release->flags = (gchar **)g_ptr_array_free(flags, FALSE);
}

/* The IMDb identifier of a result: Prowlarr gives a number (600003 for
 * tt0600003); a string "tt..." is taken too. */
static gchar *
imdb_member(const AxisJsonNode *object)
{
	const AxisJsonNode *node = axis_json_member(object, "imdbId");
	const gchar *text;
	gint64 value = 0;

	if (axis_json_type(node) == AXIS_JSON_STRING) {
		text = axis_json_string(node);
		if (axis_indexer_imdb_valid(text))
			return g_strdup(text);
	}
	if ((axis_json_type(node) == AXIS_JSON_NUMBER || axis_json_type(node) == AXIS_JSON_STRING) &&
	    axis_json_get_integer(node, 1, 9999999999, &value))
		return g_strdup_printf("tt%07" G_GINT64_FORMAT, value);
	return NULL;
}

static gboolean
is_magnet_text(const gchar *text)
{
	return text != NULL && g_ascii_strncasecmp(text, "magnet:", 7) == 0;
}

/*
 * One of a result's two addresses (downloadUrl, magnetUrl).  %FALSE when it
 * is an address that is not the source's own: the result is dropped.
 * Otherwise *@path is its path and query when it is the source's own address
 * under its base path, and %NULL when it is nothing, a magnet link (read
 * apart) or not an address that could be used.
 */
static gboolean
read_address(
	const gchar          *text,
	const AxisHttpOrigin *origin,
	const gchar          *base_path,
	gchar               **path
){
	*path = NULL;
	if (text == NULL || text[0] == '\0' || is_magnet_text(text))
		return TRUE;
	switch (axis_indexer_fetch_ref(text, origin, base_path, path)) {
	case AXIS_INDEXER_REF_OFF_ORIGIN:
		return FALSE;
	case AXIS_INDEXER_REF_OK:
		return TRUE;
	default:
		g_clear_pointer(path, g_free);
		return TRUE;
	}
}

/* What tells a result apart, for merging two answers: a hash of its "guid"
 * (an address that can hold a passkey, or a whole magnet link: never kept as
 * it is), else of its indexer, name and size. */
static gchar *
merge_key_of(
	const AxisJsonNode       *entry,
	const AxisIndexerRelease *release
){
	const gchar *guid = axis_json_string(axis_json_member(entry, "guid"));
	g_autofree gchar *fallback = NULL;

	if (guid != NULL && guid[0] != '\0')
		return g_compute_checksum_for_string(G_CHECKSUM_SHA256, guid, -1);
	fallback = g_strdup_printf("no guid|%" G_GINT64_FORMAT "|%s|%" G_GINT64_FORMAT, release->indexer_id, release->name,
		release->size_bytes);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, fallback, -1);
}

/* ── A search ───────────────────────────────────────────────────── */

typedef enum
{
	ITEM_KEPT,
	ITEM_SKIPPED,
	ITEM_OFF_ORIGIN,
	ITEM_UNUSABLE
} ItemResult;

static ItemResult
read_item(
	const AxisJsonNode   *entry,
	const AxisHttpOrigin *origin,
	const gchar          *base_path,
	gint64                now_us,
	AxisIndexerFound    **found
){
	g_autoptr(AxisIndexerRelease) release = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *fetch_path = NULL;
	g_autofree gchar *link_path = NULL;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *hash = NULL;
	const gchar *download = NULL;
	const gchar *link = NULL;
	AxisDownloadProtocol protocol = AXIS_DOWNLOAD_USENET;
	AxisIndexerFound *item;

	*found = NULL;
	if (axis_json_type(entry) != AXIS_JSON_OBJECT)
		return ITEM_SKIPPED;
	name = text_member(entry, "title", AXIS_INDEXER_NAME_MAX_CHARS);
	if (name == NULL || !protocol_member(entry, &protocol))
		return ITEM_SKIPPED;

	/* How to fetch it.  Prowlarr gives two fields: downloadUrl, the file's
	 * address, and magnetUrl, which for a torrent is an address on Prowlarr
	 * that answers with a redirect to the magnet link (or, from some
	 * indexers, the magnet link itself).  Either is a fetch address when it
	 * is on the source's own origin and under its base path; an address
	 * anywhere else drops the result, whatever else it has.  The file's
	 * address is kept when there is one, else the link's. */
	download = axis_json_string(axis_json_member(entry, "downloadUrl"));
	link = axis_json_string(axis_json_member(entry, "magnetUrl"));
	if (!read_address(download, origin, base_path, &fetch_path) ||
	    !read_address(link, origin, base_path, &link_path))
		return ITEM_OFF_ORIGIN;
	if (fetch_path == NULL)
		fetch_path = g_steal_pointer(&link_path);
	/* A magnet link is for a torrent: a Usenet result's is not believed. */
	if (protocol == AXIS_DOWNLOAD_TORRENT) {
		if (is_magnet_text(link) && axis_download_magnet_parse(link, &hash, NULL))
			magnet = g_strdup(link);
		else if (is_magnet_text(download) && axis_download_magnet_parse(download, &hash, NULL))
			magnet = g_strdup(download);
	}
	if (fetch_path == NULL && magnet == NULL)
		return ITEM_UNUSABLE;

	release = axis_indexer_release_new();
	release->name = g_steal_pointer(&name);
	release->protocol = protocol;
	release->size_bytes = int_member(entry, "size", 0, SIZE_MAX_BYTES, -1);
	release->indexer_id = int_member(entry, "indexerId", 0, 1000000000, 0);
	release->indexer_name = text_member(entry, "indexer", AXIS_INDEXER_INDEXER_MAX_CHARS);
	release->published_us = date_member(entry, "publishDate", now_us);
	if (protocol == AXIS_DOWNLOAD_TORRENT) {
		release->seeders = (gint)int_member(entry, "seeders", 0, COUNT_MAX, -1);
		release->leechers = (gint)int_member(entry, "leechers", 0, COUNT_MAX, -1);
	}
	release->grabs = (gint)int_member(entry, "grabs", 0, COUNT_MAX, -1);
	read_categories(entry, release);
	read_flags(entry, release);
	release->imdb_id = imdb_member(entry);
	release->tmdb_id = int_member(entry, "tmdbId", 1, G_MAXINT32, 0);
	release->tvdb_id = int_member(entry, "tvdbId", 1, G_MAXINT32, 0);
	if (protocol == AXIS_DOWNLOAD_TORRENT) {
		const gchar *stated = axis_json_string(axis_json_member(entry, "infoHash"));
		g_autofree gchar *lower = stated != NULL && strlen(stated) == 40 ? g_ascii_strdown(stated, -1) : NULL;

		if (axis_download_hash_valid(lower))
			release->info_hash = g_steal_pointer(&lower);
		else
			release->info_hash = g_steal_pointer(&hash);
	}
	release->has_magnet = magnet != NULL;

	item = g_new0(AxisIndexerFound, 1);
	item->merge_key = merge_key_of(entry, release);
	item->release = g_steal_pointer(&release);
	item->fetch_path = g_steal_pointer(&fetch_path);
	item->magnet = g_steal_pointer(&magnet);
	*found = item;
	return ITEM_KEPT;
}

gboolean
axis_prowlarr_parse_search(
	const gchar          *data,
	gsize                 length,
	const AxisHttpOrigin *origin,
	const gchar          *base_path,
	gint64                now_us,
	AxisIndexerOutcome   *outcome,
	gchar               **problem
){
	g_autofree gchar *why = NULL;
	AxisJsonDoc *doc;
	const AxisJsonNode *root;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	axis_indexer_outcome_init(outcome);
	if (data == NULL || length == 0) {
		if (problem != NULL)
			*problem = g_strdup("it is empty");
		return FALSE;
	}
	doc = axis_json_parse(data, length, &why);
	if (doc == NULL) {
		if (problem != NULL)
			*problem = g_strdup(why != NULL ? why : "it is not JSON");
		return FALSE;
	}
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_ARRAY) {
		axis_json_doc_free(doc);
		if (problem != NULL)
			*problem = g_strdup("it is not a list of results");
		return FALSE;
	}
	outcome->items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	for (i = 0; i < axis_json_length(root); i++) {
		AxisIndexerFound *found = NULL;

		if (outcome->items->len >= AXIS_INDEXER_MAX_RESULTS || i >= AXIS_INDEXER_MAX_SCAN) {
			outcome->truncated = TRUE;
			break;
		}
		switch (read_item(axis_json_element(root, i), origin, base_path, now_us, &found)) {
		case ITEM_KEPT:
			g_ptr_array_add(outcome->items, found);
			break;
		case ITEM_OFF_ORIGIN:
			outcome->n_off_origin++;
			break;
		case ITEM_UNUSABLE:
			outcome->n_unusable++;
			break;
		default:
			outcome->n_skipped++;
			break;
		}
	}
	axis_json_doc_free(doc);
	return TRUE;
}

/* ── Indexers ───────────────────────────────────────────────────── */

/* The identifiers named in a list of search parameters. */
static guint
ids_of(const AxisJsonNode *list)
{
	guint ids = 0;
	guint i;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return 0;
	for (i = 0; i < axis_json_length(list) && i < 64; i++) {
		const gchar *text = axis_json_string(axis_json_element(list, i));

		if (text == NULL)
			continue;
		if (g_ascii_strcasecmp(text, "imdbId") == 0)
			ids |= AXIS_INDEXER_IDS_IMDB;
		else if (g_ascii_strcasecmp(text, "tmdbId") == 0)
			ids |= AXIS_INDEXER_IDS_TMDB;
		else if (g_ascii_strcasecmp(text, "tvdbId") == 0)
			ids |= AXIS_INDEXER_IDS_TVDB;
	}
	return ids;
}

/* The host an indexer of Prowlarr's is at: its "baseUrl" field (the address
 * chosen of its definition's), else the first of its "indexerUrls"; only the
 * host is kept (an address there can hold nothing secret, but a field's
 * value could: no other field is read). */
static gchar *
indexer_host(const AxisJsonNode *entry)
{
	const AxisJsonNode *fields = axis_json_member(entry, "fields");
	const AxisJsonNode *urls = axis_json_member(entry, "indexerUrls");
	guint i;

	if (axis_json_type(fields) == AXIS_JSON_ARRAY) {
		for (i = 0; i < axis_json_length(fields) && i < 64; i++) {
			const AxisJsonNode *field = axis_json_element(fields, i);

			if (axis_json_type(field) == AXIS_JSON_OBJECT &&
			    g_strcmp0(axis_json_string(axis_json_member(field, "name")), "baseUrl") == 0) {
				gchar *host = axis_indexer_url_host_key(axis_json_string(axis_json_member(field, "value")));

				if (host != NULL)
					return host;
			}
		}
	}
	if (axis_json_type(urls) == AXIS_JSON_ARRAY && axis_json_length(urls) > 0)
		return axis_indexer_url_host_key(axis_json_string(axis_json_element(urls, 0)));
	return NULL;
}

gboolean
axis_prowlarr_parse_indexers(
	const gchar  *data,
	gsize         length,
	GPtrArray   **indexers,
	gchar       **problem
){
	g_autofree gchar *why = NULL;
	AxisJsonDoc *doc;
	const AxisJsonNode *root;
	guint i;

	*indexers = NULL;
	if (problem != NULL)
		*problem = NULL;
	doc = data != NULL && length > 0 ? axis_json_parse(data, length, &why) : NULL;
	if (doc == NULL || axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_ARRAY) {
		if (problem != NULL)
			*problem = g_strdup(doc != NULL ? "it is not a list of indexers" : why != NULL ? why : "it is empty");
		axis_json_doc_free(doc);
		return FALSE;
	}
	root = axis_json_doc_get_root(doc);
	*indexers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_info_free);
	for (i = 0; i < axis_json_length(root) && (*indexers)->len < AXIS_INDEXER_MAX_INDEXERS; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		const AxisJsonNode *capabilities;
		AxisIndexerInfo *info;
		gboolean enabled = FALSE;
		gboolean redirect = FALSE;
		guint j;
		gboolean duplicate = FALSE;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		info = axis_indexer_info_new();
		info->id = int_member(entry, "id", 0, 1000000000, -1);
		info->name = text_member(entry, "name", AXIS_INDEXER_INDEXER_MAX_CHARS);
		if (info->id < 0 || info->name == NULL || !protocol_member(entry, &info->protocol)) {
			axis_indexer_info_free(info);
			continue;
		}
		for (j = 0; j < (*indexers)->len; j++)
			duplicate = duplicate || ((AxisIndexerInfo *)g_ptr_array_index(*indexers, j))->id == info->id;
		if (duplicate) {
			axis_indexer_info_free(info);
			continue;
		}
		if (axis_json_get_boolean(axis_json_member(entry, "enable"), &enabled))
			info->enabled = enabled;
		/* Prowlarr's "Redirect" option for the indexer: a download is answered
		 * with a redirect to the indexer's own site. */
		if (axis_json_get_boolean(axis_json_member(entry, "redirect"), &redirect))
			info->redirect = redirect;
		capabilities = axis_json_member(entry, "capabilities");
		info->movie_ids = ids_of(axis_json_member(capabilities, "movieSearchParams"));
		info->tv_ids = ids_of(axis_json_member(capabilities, "tvSearchParams"));
		info->host = indexer_host(entry);
		g_ptr_array_add(*indexers, info);
	}
	axis_json_doc_free(doc);
	return TRUE;
}

gboolean
axis_prowlarr_parse_failing(
	const gchar  *data,
	gsize         length,
	gint64        now_us,
	GArray      **ids,
	gchar       **problem
){
	g_autofree gchar *why = NULL;
	AxisJsonDoc *doc;
	const AxisJsonNode *root;
	guint i;

	*ids = NULL;
	if (problem != NULL)
		*problem = NULL;
	doc = data != NULL && length > 0 ? axis_json_parse(data, length, &why) : NULL;
	if (doc == NULL || axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_ARRAY) {
		if (problem != NULL)
			*problem = g_strdup(doc != NULL ? "it is not a list of statuses" : why != NULL ? why : "it is empty");
		axis_json_doc_free(doc);
		return FALSE;
	}
	root = axis_json_doc_get_root(doc);
	*ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	for (i = 0; i < axis_json_length(root) && i < AXIS_INDEXER_MAX_INDEXERS * 4; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		gint64 id;
		gint64 until;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		id = int_member(entry, "indexerId", 0, 1000000000, -1);
		until = date_member(entry, "disabledTill", 0);
		if (id >= 0 && until > now_us)
			g_array_append_val(*ids, id);
	}
	axis_json_doc_free(doc);
	return TRUE;
}

gchar *
axis_prowlarr_parse_version(
	const gchar *data,
	gsize        length
){
	AxisJsonDoc *doc = data != NULL && length > 0 ? axis_json_parse(data, length, NULL) : NULL;
	gchar *version = NULL;

	if (doc == NULL)
		return NULL;
	if (axis_json_type(axis_json_doc_get_root(doc)) == AXIS_JSON_OBJECT)
		version = text_member(axis_json_doc_get_root(doc), "version", 64);
	axis_json_doc_free(doc);
	return version;
}

/* ── Asking ─────────────────────────────────────────────────────── */

guint
axis_prowlarr_identifiers(
	const AxisIndexerRequest *request,
	AxisIndexerMode          *modes
){
	guint n = 0;

	switch (request->kind) {
	case AXIS_INDEXER_QUERY_FILM:
		if (request->imdb_id != NULL)
			modes[n++] = AXIS_INDEXER_BY_IMDB;
		if (request->tmdb_id > 0)
			modes[n++] = AXIS_INDEXER_BY_TMDB;
		break;
	case AXIS_INDEXER_QUERY_EPISODE:
	case AXIS_INDEXER_QUERY_SEASON:
		if (request->tvdb_id > 0)
			modes[n++] = AXIS_INDEXER_BY_TVDB;
		if (request->imdb_id != NULL)
			modes[n++] = AXIS_INDEXER_BY_IMDB;
		if (request->tmdb_id > 0)
			modes[n++] = AXIS_INDEXER_BY_TMDB;
		break;
	default:
		break;
	}
	return n;
}

static void
append_param(
	GString     *path,
	const gchar *name,
	const gchar *value
){
	g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

	g_string_append_printf(path, "%c%s=%s", strchr(path->str, '?') != NULL ? '&' : '?', name, escaped);
}

gchar *
axis_prowlarr_search_path(
	const AxisIndexerRequest *request,
	AxisIndexerMode           mode
){
	g_autofree gchar *title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	g_autofree gchar *query = NULL;
	g_autofree gchar *numbers = NULL;
	const gchar *type;
	GString *path;
	guint i;

	if (request->kind == AXIS_INDEXER_QUERY_EPISODE)
		numbers = g_strdup_printf("{Season:%d}{Episode:%d}", request->season, request->episode);
	else if (request->kind == AXIS_INDEXER_QUERY_SEASON)
		numbers = g_strdup_printf("{Season:%d}", request->season);
	else
		numbers = g_strdup("");

	if (mode == AXIS_INDEXER_BY_TEXT) {
		if (title == NULL)
			return NULL;
		type = "search";
		switch (request->kind) {
		case AXIS_INDEXER_QUERY_FILM:
			query = request->year > 0 ? g_strdup_printf("%s %d", title, request->year) : g_strdup(title);
			break;
		case AXIS_INDEXER_QUERY_EPISODE:
			query = g_strdup_printf("%s S%02dE%02d", title, request->season, request->episode);
			break;
		case AXIS_INDEXER_QUERY_SEASON:
			query = g_strdup_printf("%s S%02d", title, request->season);
			break;
		default:
			query = g_strdup(title);
			break;
		}
	} else {
		const gchar *form;
		g_autofree gchar *value = NULL;

		if (request->kind != AXIS_INDEXER_QUERY_FILM && request->kind != AXIS_INDEXER_QUERY_EPISODE &&
		    request->kind != AXIS_INDEXER_QUERY_SEASON)
			return NULL;
		switch (mode) {
		case AXIS_INDEXER_BY_IMDB:
			if (request->imdb_id == NULL)
				return NULL;
			form = "ImdbId";
			value = g_strdup(request->imdb_id);
			break;
		case AXIS_INDEXER_BY_TMDB:
			if (request->tmdb_id <= 0)
				return NULL;
			form = "TmdbId";
			value = g_strdup_printf("%" G_GINT64_FORMAT, request->tmdb_id);
			break;
		default:
			if (request->tvdb_id <= 0)
				return NULL;
			form = "TvdbId";
			value = g_strdup_printf("%" G_GINT64_FORMAT, request->tvdb_id);
			break;
		}
		type = request->kind == AXIS_INDEXER_QUERY_FILM ? "movie" : "tvsearch";
		query = g_strdup_printf("{%s:%s}%s", form, value, numbers);
	}

	path = g_string_new("/api/v1/search");
	append_param(path, "query", query);
	append_param(path, "type", type);
	if (request->n_categories > 0) {
		for (i = 0; i < request->n_categories && i < AXIS_INDEXER_MAX_CATEGORIES; i++) {
			g_autofree gchar *category = g_strdup_printf("%d", request->categories[i]);

			append_param(path, "categories", category);
		}
	} else if (request->kind == AXIS_INDEXER_QUERY_FILM) {
		append_param(path, "categories", "2000");
	} else if (request->anime) {
		/* An anime series (add-anime-support, A3): TV and TV/Anime, for an
		 * episode, a season and words alike. */
		gint with_anime[AXIS_INDEXER_MAX_CATEGORIES];
		guint n = axis_indexer_request_tv_categories(NULL, 0, with_anime);

		for (i = 0; i < n; i++) {
			g_autofree gchar *category = g_strdup_printf("%d", with_anime[i]);

			append_param(path, "categories", category);
		}
	} else if (request->kind == AXIS_INDEXER_QUERY_EPISODE || request->kind == AXIS_INDEXER_QUERY_SEASON) {
		append_param(path, "categories", "5000");
	}
	for (i = 0; i < request->n_indexers && i < AXIS_INDEXER_MAX_INDEXER_IDS; i++) {
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, request->indexers[i]);

		append_param(path, "indexerIds", id);
	}
	{
		g_autofree gchar *limit = g_strdup_printf("%d", AXIS_INDEXER_MAX_RESULTS);

		append_param(path, "limit", limit);
	}
	return g_string_free(path, FALSE);
}
