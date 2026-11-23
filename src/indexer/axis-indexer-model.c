/*
 * axis-indexer-model.c - requests, releases, states and the checks on what
 * a source returns
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-model.h"

#include <stdlib.h>
#include <string.h>

#include "download/axis-download-bencode.h"

/* The most a path asked of a source, or a fetch address, may be as text. */
#define URL_MAX_BYTES 4096

/* ── Names ──────────────────────────────────────────────────────── */

typedef struct
{
	gint         value;
	const gchar *name;
	const gchar *label;
} Word;

static const Word state_words[] = {
	{ AXIS_INDEXER_SOURCE_UNCHECKED, "unchecked", "Not checked yet" },
	{ AXIS_INDEXER_SOURCE_OK, "ok", "Working" },
	{ AXIS_INDEXER_SOURCE_UNREACHABLE, "unreachable", "Not answering" },
	{ AXIS_INDEXER_SOURCE_REFUSED, "refused", "Access refused" },
	{ AXIS_INDEXER_SOURCE_FAILING, "failing", "Failing" },
	{ -1, NULL, NULL }
};

static const Word query_words[] = {
	{ AXIS_INDEXER_QUERY_FILM, "film", "Movie" },
	{ AXIS_INDEXER_QUERY_EPISODE, "episode", "Episode" },
	{ AXIS_INDEXER_QUERY_SEASON, "season", "Season" },
	{ AXIS_INDEXER_QUERY_TEXT, "text", "Words" },
	{ -1, NULL, NULL }
};

static const Word mode_words[] = {
	{ AXIS_INDEXER_BY_TEXT, "text", "the title and year" },
	{ AXIS_INDEXER_BY_IMDB, "imdb", "the IMDb identifier" },
	{ AXIS_INDEXER_BY_TMDB, "tmdb", "the TMDb identifier" },
	{ AXIS_INDEXER_BY_TVDB, "tvdb", "the TVDB identifier" },
	{ -1, NULL, NULL }
};

static const Word *
word_by_value(
	const Word *words,
	gint        value
){
	guint i;

	for (i = 0; words[i].name != NULL; i++) {
		if (words[i].value == value)
			return &words[i];
	}
	return NULL;
}

static const Word *
word_by_name(
	const Word  *words,
	const gchar *name
){
	guint i;

	for (i = 0; name != NULL && words[i].name != NULL; i++) {
		if (strcmp(words[i].name, name) == 0)
			return &words[i];
	}
	return NULL;
}

gboolean
axis_indexer_kind_name_valid(const gchar *text)
{
	return axis_download_kind_name_valid(text);
}

const gchar *
axis_indexer_source_state_to_string(AxisIndexerSourceState state)
{
	const Word *word = word_by_value(state_words, (gint)state);

	return word != NULL ? word->name : "unchecked";
}

gboolean
axis_indexer_source_state_from_string(
	const gchar            *text,
	AxisIndexerSourceState *state
){
	const Word *word = word_by_name(state_words, text);

	if (word == NULL)
		return FALSE;
	*state = (AxisIndexerSourceState)word->value;
	return TRUE;
}

const gchar *
axis_indexer_source_state_label(AxisIndexerSourceState state)
{
	const Word *word = word_by_value(state_words, (gint)state);

	return word != NULL ? word->label : "Not checked yet";
}

const gchar *
axis_indexer_query_kind_to_string(AxisIndexerQueryKind kind)
{
	const Word *word = word_by_value(query_words, (gint)kind);

	return word != NULL ? word->name : "text";
}

gboolean
axis_indexer_query_kind_from_string(
	const gchar          *text,
	AxisIndexerQueryKind *kind
){
	const Word *word = word_by_name(query_words, text);

	if (word == NULL)
		return FALSE;
	*kind = (AxisIndexerQueryKind)word->value;
	return TRUE;
}

const gchar *
axis_indexer_mode_to_string(AxisIndexerMode mode)
{
	const Word *word = word_by_value(mode_words, (gint)mode);

	return word != NULL ? word->name : "text";
}

gboolean
axis_indexer_mode_from_string(
	const gchar     *text,
	AxisIndexerMode *mode
){
	const Word *word = word_by_name(mode_words, text);

	if (word == NULL)
		return FALSE;
	*mode = (AxisIndexerMode)word->value;
	return TRUE;
}

const gchar *
axis_indexer_mode_label(AxisIndexerMode mode)
{
	const Word *word = word_by_value(mode_words, (gint)mode);

	return word != NULL ? word->label : "the title and year";
}

/* ── Requests ───────────────────────────────────────────────────── */

void
axis_indexer_request_init(
	AxisIndexerRequest   *request,
	AxisIndexerQueryKind  kind
){
	memset(request, 0, sizeof(*request));
	request->kind = kind;
	request->season = -1;
	request->episode = -1;
}

gboolean
axis_indexer_imdb_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n < 9 || n > 12 || text[0] != 't' || text[1] != 't')
		return FALSE;
	for (i = 2; i < n; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

gchar *
axis_indexer_text_clean(
	const gchar *text,
	guint        max_chars
){
	return axis_download_text_clean(text, max_chars);
}

gboolean
axis_indexer_request_check(
	const AxisIndexerRequest *request,
	const gchar             **problem
){
	g_autofree gchar *title = NULL;
	const gchar *why = NULL;
	gboolean has_id;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (request == NULL) {
		why = "there is no request";
	} else if ((guint)request->kind > AXIS_INDEXER_QUERY_TEXT) {
		why = "the kind of search is not one of movie, episode, season or words";
	} else if (request->title != NULL && (strlen(request->title) > AXIS_INDEXER_TITLE_MAX_BYTES ||
	           !g_utf8_validate(request->title, -1, NULL) ||
	           g_utf8_strlen(request->title, -1) > AXIS_INDEXER_TITLE_MAX_CHARS)) {
		why = "the title is longer than 200 characters or 256 bytes, or is not text";
	} else if (request->year != 0 && (request->year < 1870 || request->year > 2200)) {
		why = "the year is out of range";
	} else if (request->imdb_id != NULL && !axis_indexer_imdb_valid(request->imdb_id)) {
		why = "the IMDb identifier is not tt and 7 to 10 digits";
	} else if (request->tmdb_id < 0 || request->tmdb_id > G_MAXINT32 || request->tvdb_id < 0 ||
	           request->tvdb_id > G_MAXINT32) {
		why = "an identifier is out of range";
	} else if (request->season < -1 || request->season > 9999 || request->episode < -1 || request->episode > 99999) {
		why = "the season or episode number is out of range";
	} else if (request->n_categories > AXIS_INDEXER_MAX_CATEGORIES || request->n_indexers > AXIS_INDEXER_MAX_INDEXER_IDS) {
		why = "there are too many categories or indexers";
	}
	if (why == NULL && request != NULL) {
		for (i = 0; i < request->n_categories; i++) {
			if (request->categories[i] < 1 || request->categories[i] > 999999)
				why = "a category is out of range";
		}
		for (i = 0; i < request->n_indexers; i++) {
			if (request->indexers[i] < 1 || request->indexers[i] > 1000000000)
				why = "an indexer identifier is out of range";
		}
	}
	if (why != NULL) {
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	has_id = request->imdb_id != NULL || request->tmdb_id > 0 || request->tvdb_id > 0;
	switch (request->kind) {
	case AXIS_INDEXER_QUERY_TEXT:
		if (title == NULL)
			why = "a search by words needs words";
		break;
	case AXIS_INDEXER_QUERY_FILM:
		if (title == NULL && !has_id)
			why = "a movie needs a title or an identifier";
		break;
	case AXIS_INDEXER_QUERY_EPISODE:
		if (request->season < 0 || request->episode < 1)
			why = "an episode needs a season and an episode number";
		else if (title == NULL && !has_id)
			why = "an episode needs a series title or identifier";
		break;
	default:
		if (request->season < 0)
			why = "a season needs its number";
		else if (title == NULL && !has_id)
			why = "a season needs a series title or identifier";
		break;
	}
	if (why != NULL) {
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	return TRUE;
}

static gint
compare_int(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : x > y;
}

gchar *
axis_indexer_request_key(const AxisIndexerRequest *request)
{
	g_autofree gchar *clean = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	g_autofree gchar *title = clean != NULL ? g_utf8_strdown(clean, -1) : g_strdup("");
	gint64 sorted[AXIS_INDEXER_MAX_CATEGORIES + AXIS_INDEXER_MAX_INDEXER_IDS];
	GString *text = g_string_new(NULL);
	gchar *key;
	guint n;
	guint i;

	g_string_append_printf(text, "v1|%d|%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%s|%d|%d|%d|",
		(gint)request->kind, request->imdb_id != NULL ? request->imdb_id : "", request->tmdb_id, request->tvdb_id,
		title, request->year, request->season, request->episode);
	n = MIN(request->n_categories, (guint)AXIS_INDEXER_MAX_CATEGORIES);
	for (i = 0; i < n; i++)
		sorted[i] = request->categories[i];
	qsort(sorted, n, sizeof(gint64), compare_int);
	for (i = 0; i < n; i++) {
		if (i == 0 || sorted[i] != sorted[i - 1])
			g_string_append_printf(text, "%" G_GINT64_FORMAT ",", sorted[i]);
	}
	g_string_append_c(text, '|');
	n = MIN(request->n_indexers, (guint)AXIS_INDEXER_MAX_INDEXER_IDS);
	for (i = 0; i < n; i++)
		sorted[i] = request->indexers[i];
	qsort(sorted, n, sizeof(gint64), compare_int);
	for (i = 0; i < n; i++) {
		if (i == 0 || sorted[i] != sorted[i - 1])
			g_string_append_printf(text, "%" G_GINT64_FORMAT ",", sorted[i]);
	}
	/* Only when set: every other request keeps the key it had. */
	if (request->anime)
		g_string_append(text, "|anime");
	key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text->str, (gssize)text->len);
	g_string_free(text, TRUE);
	return key;
}

guint
axis_indexer_request_tv_categories(
	const gint *own,
	guint       n_own,
	gint       *out
){
	gboolean has = FALSE;
	guint n = 0;
	guint i;

	for (i = 0; own != NULL && i < n_own && n < AXIS_INDEXER_MAX_CATEGORIES; i++) {
		out[n++] = own[i];
		has = has || own[i] == AXIS_INDEXER_CATEGORY_ANIME;
	}
	if (n == 0)
		out[n++] = 5000;
	if (!has && n < AXIS_INDEXER_MAX_CATEGORIES)
		out[n++] = AXIS_INDEXER_CATEGORY_ANIME;
	return n;
}

/* ── Releases ───────────────────────────────────────────────────── */

AxisIndexerRelease *
axis_indexer_release_new(void)
{
	AxisIndexerRelease *release = g_new0(AxisIndexerRelease, 1);

	release->size_bytes = -1;
	release->seeders = -1;
	release->leechers = -1;
	release->grabs = -1;
	return release;
}

void
axis_indexer_release_free(AxisIndexerRelease *release)
{
	if (release == NULL)
		return;
	g_free(release->name);
	g_free(release->indexer_name);
	g_strfreev(release->flags);
	g_free(release->imdb_id);
	g_free(release->info_hash);
	g_strfreev(release->also_at);
	g_free(release);
}

AxisIndexerRelease *
axis_indexer_release_copy(const AxisIndexerRelease *release)
{
	AxisIndexerRelease *copy = axis_indexer_release_new();

	*copy = *release;
	copy->name = g_strdup(release->name);
	copy->indexer_name = g_strdup(release->indexer_name);
	copy->flags = g_strdupv(release->flags);
	copy->imdb_id = g_strdup(release->imdb_id);
	copy->info_hash = g_strdup(release->info_hash);
	copy->also_at = g_strdupv(release->also_at);
	return copy;
}

gchar *
axis_indexer_release_group_key(const AxisIndexerRelease *release)
{
	g_autofree gchar *text = NULL;

	if (release == NULL || release->name == NULL)
		return NULL;
	if (release->protocol == AXIS_DOWNLOAD_TORRENT) {
		if (!axis_download_hash_valid(release->info_hash))
			return NULL;
		text = g_strdup_printf("v1|torrent|%s", release->info_hash);
	} else {
		if (release->size_bytes < 0)
			return NULL;
		text = g_strdup_printf("v1|usenet|%" G_GINT64_FORMAT "|%s", release->size_bytes, release->name);
	}
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}

static void
wipe_free(gchar *text)
{
	if (text == NULL)
		return;
	memset(text, 0, strlen(text));
	g_free(text);
}

void
axis_indexer_found_free(AxisIndexerFound *found)
{
	if (found == NULL)
		return;
	axis_indexer_release_free(found->release);
	wipe_free(found->fetch_path);
	wipe_free(found->magnet);
	g_free(found->merge_key);
	g_free(found);
}

void
axis_indexer_outcome_init(AxisIndexerOutcome *outcome)
{
	memset(outcome, 0, sizeof(*outcome));
}

void
axis_indexer_outcome_clear(AxisIndexerOutcome *outcome)
{
	g_clear_pointer(&outcome->items, g_ptr_array_unref);
	g_free(outcome->other_origin);
	g_free(outcome->not_asked);
	memset(outcome, 0, sizeof(*outcome));
}

void
axis_indexer_outcome_nothing_here(
	AxisIndexerOutcome       *outcome,
	const AxisIndexerRequest *request
){
	g_free(outcome->not_asked);
	outcome->not_asked = g_strdup(request->kind == AXIS_INDEXER_QUERY_FILM ? "it has no movies" : "it has no TV");
}

AxisIndexerInfo *
axis_indexer_info_new(void)
{
	return g_new0(AxisIndexerInfo, 1);
}

void
axis_indexer_info_free(AxisIndexerInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->name);
	g_free(info->host);
	g_free(info);
}

gchar *
axis_indexer_host_key(const gchar *host)
{
	g_autofree gchar *lower = NULL;
	const gchar *p;
	const gchar *start;

	if (host == NULL || host[0] == '\0' || strlen(host) > 253)
		return NULL;
	for (p = host; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-' && *p != ':' && *p != '[' && *p != ']')
			return NULL;
	}
	lower = g_ascii_strdown(host, -1);
	start = lower;
	/* "www." or "api." before a name that still has a dot. */
	if (strlen(start) > 4 && (strncmp(start, "www.", 4) == 0 || strncmp(start, "api.", 4) == 0) &&
	    strchr(start + 4, '.') != NULL)
		start += 4;
	return g_strdup(start);
}

gchar *
axis_indexer_url_host_key(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;

	if (url == NULL || strlen(url) > 1024)
		return NULL;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_uri_get_host(uri) == NULL)
		return NULL;
	return axis_indexer_host_key(g_uri_get_host(uri));
}

gchar *
axis_indexer_plain_name(const gchar *text)
{
	GString *out = g_string_new(NULL);

	for (; text != NULL && *text != '\0'; text++) {
		if (g_ascii_isalnum(*text))
			g_string_append_c(out, g_ascii_tolower(*text));
	}
	return g_string_free(out, FALSE);
}

gchar *
axis_indexer_site_name(const gchar *host)
{
	g_auto(GStrv) labels = NULL;
	g_autofree gchar *bare = NULL;
	const gchar *colon;
	gchar *name;
	guint n;

	if (host == NULL || host[0] == '[')
		return NULL;
	colon = strchr(host, ':');
	bare = colon != NULL ? g_strndup(host, (gsize)(colon - host)) : g_strdup(host);
	labels = g_strsplit(bare, ".", -1);
	n = g_strv_length(labels);
	if (n < 2 || labels[n - 1][0] == '\0' || g_ascii_isdigit(labels[n - 1][0]))
		return NULL;
	name = axis_indexer_plain_name(labels[n - 2]);
	if (strlen(name) < 3) {
		g_free(name);
		return NULL;
	}
	return name;
}

/* ── Capabilities and allowances ────────────────────────────────── */

void
axis_indexer_caps_clear(AxisIndexerCaps *caps)
{
	if (caps == NULL)
		return;
	g_free(caps->problem);
	g_strfreev(caps->categories);
	memset(caps, 0, sizeof(*caps));
}

void
axis_indexer_caps_copy(
	const AxisIndexerCaps *from,
	AxisIndexerCaps       *to
){
	*to = *from;
	to->problem = g_strdup(from->problem);
	to->categories = g_strdupv(from->categories);
}

void
axis_indexer_allowance_init(AxisIndexerAllowance *allowance)
{
	allowance->api_used = -1;
	allowance->api_max = -1;
	allowance->grab_used = -1;
	allowance->grab_max = -1;
	allowance->reported_us = 0;
}

gint
axis_indexer_allowance_reserve(gint max)
{
	if (max <= 0)
		return 0;
	return MAX(5, max / 10);
}

gboolean
axis_indexer_allowance_spent(
	const AxisIndexerAllowance *allowance,
	gboolean                    automatic
){
	gint left;

	if (allowance == NULL || allowance->api_max <= 0 || allowance->api_used < 0)
		return FALSE;
	left = allowance->api_max - allowance->api_used;
	if (!automatic)
		return left <= 0;
	return left <= axis_indexer_allowance_reserve(allowance->api_max);
}

gboolean
axis_indexer_allowance_grabs_spent(const AxisIndexerAllowance *allowance)
{
	return allowance != NULL && allowance->grab_max > 0 && allowance->grab_used >= allowance->grab_max;
}

gchar *
axis_indexer_ids_label(guint ids)
{
	GPtrArray *parts = g_ptr_array_new();
	gchar *text;

	if ((ids & AXIS_INDEXER_IDS_IMDB) != 0)
		g_ptr_array_add(parts, (gpointer)"IMDb");
	if ((ids & AXIS_INDEXER_IDS_TMDB) != 0)
		g_ptr_array_add(parts, (gpointer)"TMDb");
	if ((ids & AXIS_INDEXER_IDS_TVDB) != 0)
		g_ptr_array_add(parts, (gpointer)"TVDB");
	g_ptr_array_add(parts, NULL);
	text = parts->len > 1 ? g_strjoinv(", ", (gchar **)parts->pdata) : g_strdup("none");
	g_ptr_array_free(parts, TRUE);
	return text;
}

/* ── Paths and fetch references ─────────────────────────────────── */

static gboolean
has_encoded(
	const gchar *text,
	gsize        length
){
	/* %00 %2e %2f %5c in either case: a dot, a slash, a backslash or a NUL
	 * written so that a server may read them as the real thing. */
	static const gchar *const bad[] = { "%00", "%2e", "%2f", "%5c", NULL };
	gsize i;
	guint j;

	for (i = 0; i + 3 <= length; i++) {
		if (text[i] != '%')
			continue;
		for (j = 0; bad[j] != NULL; j++) {
			if (g_ascii_strncasecmp(text + i, bad[j], 3) == 0)
				return TRUE;
		}
	}
	return FALSE;
}

gboolean
axis_indexer_path_ok(
	const gchar *path,
	const gchar *base_path
){
	const gchar *query;
	gsize path_length;
	gsize base_length;
	const gchar *p;
	const gchar *segment;

	if (path == NULL || path[0] != '/' || path[1] == '/' || strlen(path) > AXIS_INDEXER_FETCH_PATH_MAX)
		return FALSE;
	for (p = path; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '\\' || *p == '#')
			return FALSE;
	}
	query = strchr(path, '?');
	path_length = query != NULL ? (gsize)(query - path) : strlen(path);
	if (has_encoded(path, path_length))
		return FALSE;
	base_length = base_path != NULL ? strlen(base_path) : 0;
	if (base_length > 0) {
		if (path_length < base_length || strncmp(path, base_path, base_length) != 0)
			return FALSE;
		if (path_length > base_length && path[base_length] != '/')
			return FALSE;
	}
	/* No empty, "." or ".." part. */
	segment = path + 1;
	while (segment <= path + path_length) {
		const gchar *end = segment;
		gsize length;

		while (end < path + path_length && *end != '/')
			end++;
		length = (gsize)(end - segment);
		if (length == 0 && end < path + path_length)
			return FALSE;
		if ((length == 1 && segment[0] == '.') || (length == 2 && segment[0] == '.' && segment[1] == '.'))
			return FALSE;
		segment = end + 1;
	}
	return TRUE;
}

AxisIndexerRefResult
axis_indexer_fetch_ref(
	const gchar          *url,
	const AxisHttpOrigin *origin,
	const gchar          *base_path,
	gchar               **path
){
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *full = NULL;
	const gchar *scheme;
	const gchar *host;
	const gchar *uri_path;
	const gchar *query;
	gint port;

	*path = NULL;
	if (url == NULL || url[0] == '\0' || strlen(url) > URL_MAX_BYTES)
		return AXIS_INDEXER_REF_INVALID;
	uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
	if (uri == NULL)
		return AXIS_INDEXER_REF_INVALID;
	scheme = g_uri_get_scheme(uri);
	host = g_uri_get_host(uri);
	if (scheme == NULL || host == NULL || host[0] == '\0')
		return AXIS_INDEXER_REF_INVALID;
	if (g_uri_get_user(uri) != NULL || g_uri_get_password(uri) != NULL || g_uri_get_auth_params(uri) != NULL ||
	    g_uri_get_fragment(uri) != NULL)
		return AXIS_INDEXER_REF_INVALID;
	/* The scheme, host and port must be the source's own. */
	port = g_uri_get_port(uri);
	if (port < 0) {
		if (g_ascii_strcasecmp(scheme, "https") == 0)
			port = 443;
		else if (g_ascii_strcasecmp(scheme, "http") == 0)
			port = 80;
	}
	if (g_ascii_strcasecmp(scheme, origin->https ? "https" : "http") != 0 ||
	    g_ascii_strcasecmp(host, origin->host) != 0 || port < 0 || (guint)port != origin->port)
		return AXIS_INDEXER_REF_OFF_ORIGIN;
	/* GLib resolves dot parts and encoded dots itself, so what it gives as
	 * the path is already "clean": the address as it was written is
	 * checked as well, and one that needed resolving is not one the source
	 * wrote for a download. */
	{
		const gchar *authority = strstr(url, "://");
		const gchar *raw = authority != NULL ? authority + 3 + strcspn(authority + 3, "/?#") : NULL;

		if (raw == NULL || raw[0] != '/' || !axis_indexer_path_ok(raw, ""))
			return AXIS_INDEXER_REF_INVALID;
	}
	uri_path = g_uri_get_path(uri);
	query = g_uri_get_query(uri);
	full = query != NULL ? g_strconcat(uri_path, "?", query, NULL) : g_strdup(uri_path);
	if (!axis_indexer_path_ok(full, base_path))
		return AXIS_INDEXER_REF_INVALID;
	*path = g_steal_pointer(&full);
	return AXIS_INDEXER_REF_OK;
}

/* ── What a fetched file is ─────────────────────────────────────── */

static gboolean
is_space(guint8 c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The first position at or after @from where @needle begins within @limit
 * bytes, or -1. */
static gssize
find_within(
	const guint8 *data,
	gsize         length,
	gsize         from,
	gsize         limit,
	const gchar  *needle
){
	gsize n = strlen(needle);
	gsize i;

	for (i = from; i + n <= length && i < from + limit; i++) {
		if (data[i] == (guint8)needle[0] && memcmp(data + i, needle, n) == 0)
			return (gssize)i;
	}
	return -1;
}

static gboolean
begins(
	const guint8 *data,
	gsize         length,
	gsize         at,
	const gchar  *text
){
	gsize n = strlen(text);

	return at + n <= length && memcmp(data + at, text, n) == 0;
}

static gboolean
looks_like_nzb(
	const guint8 *data,
	gsize         length
){
	gsize pos = 0;
	guint steps;

	if (begins(data, length, 0, "\xef\xbb\xbf"))
		pos = 3;
	for (steps = 0; steps < 32; steps++) {
		gssize end;

		while (pos < length && is_space(data[pos]))
			pos++;
		if (begins(data, length, pos, "<?xml")) {
			end = find_within(data, length, pos, 2048, "?>");
			if (end < 0)
				return FALSE;
			pos = (gsize)end + 2;
		} else if (begins(data, length, pos, "<!--")) {
			end = find_within(data, length, pos + 4, 8192, "-->");
			if (end < 0)
				return FALSE;
			pos = (gsize)end + 3;
		} else if (begins(data, length, pos, "<!DOCTYPE")) {
			gsize i;

			/* An internal subset (entities) is not an NZB's. */
			for (i = pos; i < length && i < pos + 1024 && data[i] != '>'; i++) {
				if (data[i] == '[')
					return FALSE;
			}
			if (i >= length || data[i] != '>')
				return FALSE;
			pos = i + 1;
		} else if (begins(data, length, pos, "<nzb")) {
			return pos + 4 < length && (is_space(data[pos + 4]) || data[pos + 4] == '>' || data[pos + 4] == '/');
		} else {
			return FALSE;
		}
	}
	return FALSE;
}

AxisIndexerContent
axis_indexer_content_kind(
	const guint8 *data,
	gsize         length
){
	if (data == NULL || length < 8)
		return AXIS_INDEXER_CONTENT_NONE;
	if (looks_like_nzb(data, length))
		return AXIS_INDEXER_CONTENT_NZB;
	if (data[0] == 'd' && axis_download_torrent_read(data, length, NULL, NULL, NULL))
		return AXIS_INDEXER_CONTENT_TORRENT;
	return AXIS_INDEXER_CONTENT_NONE;
}

void
axis_indexer_fetched_clear(AxisIndexerFetched *fetched)
{
	g_clear_pointer(&fetched->file, g_bytes_unref);
	wipe_free(fetched->magnet);
	fetched->magnet = NULL;
	g_clear_pointer(&fetched->file_name, g_free);
	g_clear_pointer(&fetched->info_hash, g_free);
}

gchar *
axis_indexer_file_name(
	const gchar *name,
	const gchar *extension
){
	GString *out = g_string_new(NULL);
	const gchar *p;

	if (name == NULL || !g_utf8_validate(name, -1, NULL))
		name = "";
	for (p = name; *p != '\0' && out->len < 120; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x80 && (g_ascii_isalnum((gchar)c) || c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' ||
		    c == ')' || c == '[' || c == ']'))
			g_string_append_c(out, (gchar)c);
		else
			g_string_append_c(out, '_');
	}
	g_strstrip(out->str);
	out->len = strlen(out->str);
	/* No leading dots: never a hidden file. */
	while (out->len > 0 && out->str[0] == '.')
		g_string_erase(out, 0, 1);
	if (out->len == 0)
		g_string_append(out, "release");
	g_string_append_printf(out, ".%s", extension);
	return g_string_free(out, FALSE);
}
