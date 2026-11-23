/*
 * axis-indexer-newznab-parse.c - Newznab and Torznab answers, read, and the
 * search paths
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-newznab-parse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/axis-indexer-xml.h"

/* The largest size a release may state (a petabyte). */
#define SIZE_MAX_BYTES ((gint64)1 << 50)

/* The largest count (seeders, grabs) believed. */
#define COUNT_MAX 1000000000

/* Dates before this and after the far future are not believed. */
#define DATE_MIN_YEAR 1990
#define DATE_MAX_YEAR 2100

/* A publication date this far ahead of now is not believed. */
#define DATE_FUTURE_SLACK_US (G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC)

/* The most text kept of one field of an item: a fetch address is at most
 * 4096 bytes, and a name is cut far shorter. */
#define FIELD_MAX_BYTES 8192

/* The most attributes of one item looked at. */
#define ITEM_MAX_ATTRS 64

void
axis_newznab_error_clear(AxisNewznabError *error)
{
	if (error == NULL)
		return;
	g_clear_pointer(&error->description, g_free);
	error->code = 0;
}

AxisIndexerError
axis_newznab_error_kind(gint code)
{
	if ((code >= 100 && code <= 199) || code == 910)
		return AXIS_INDEXER_ERROR_AUTH;
	if (code == 500 || code == 501)
		return AXIS_INDEXER_ERROR_RATE_LIMITED;
	if ((code >= 200 && code <= 299) || code == 300)
		return AXIS_INDEXER_ERROR_REFUSED;
	return AXIS_INDEXER_ERROR_FAILED;
}

/* ── Numbers and words ──────────────────────────────────────────── */

/* A whole number within [@minimum, @maximum], as text of digits (a sign
 * only for a negative minimum); %FALSE otherwise. */
static gboolean
int_of(
	const gchar *text,
	gint64       minimum,
	gint64       maximum,
	gint64      *value
){
	const gchar *p;
	gboolean negative = FALSE;
	gint64 n = 0;

	if (text == NULL)
		return FALSE;
	while (*text == ' ')
		text++;
	p = text;
	if (*p == '-' && minimum < 0) {
		negative = TRUE;
		p++;
	}
	if (!g_ascii_isdigit(*p))
		return FALSE;
	for (; g_ascii_isdigit(*p); p++) {
		if (n > (G_MAXINT64 - 9) / 10)
			return FALSE;
		n = n * 10 + (*p - '0');
	}
	while (*p == ' ')
		p++;
	if (*p != '\0')
		return FALSE;
	if (negative)
		n = -n;
	if (n < minimum || n > maximum)
		return FALSE;
	*value = n;
	return TRUE;
}

/* A decimal number within [0, 100], as text; -1 otherwise. */
static gdouble
factor_of(const gchar *text)
{
	gchar *end = NULL;
	gdouble value;

	if (text == NULL || text[0] == '\0' || strlen(text) > 32)
		return -1;
	value = g_ascii_strtod(text, &end);
	if (end == NULL || *end != '\0' || !isfinite(value) || value < 0 || value > 100)
		return -1;
	return value;
}

static gboolean
is_yes(const gchar *text)
{
	return text != NULL && (g_ascii_strcasecmp(text, "yes") == 0 || g_ascii_strcasecmp(text, "true") == 0 ||
		strcmp(text, "1") == 0);
}

static gboolean
is_magnet_text(const gchar *text)
{
	return text != NULL && g_ascii_strncasecmp(text, "magnet:", 7) == 0;
}

static gboolean
is_web_text(const gchar *text)
{
	return text != NULL && (g_ascii_strncasecmp(text, "https://", 8) == 0 ||
		g_ascii_strncasecmp(text, "http://", 7) == 0);
}

/* ── Dates ──────────────────────────────────────────────────────── */

static gint
month_of(const gchar *text)
{
	static const gchar *const months[] = {
		"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
	};
	guint i;

	if (text == NULL || strlen(text) < 3)
		return 0;
	for (i = 0; i < G_N_ELEMENTS(months); i++) {
		if (g_ascii_strncasecmp(text, months[i], 3) == 0)
			return (gint)i + 1;
	}
	return 0;
}

/* A zone's offset from UTC in seconds, or %FALSE. */
static gboolean
zone_of(
	const gchar *text,
	gint        *offset
){
	static const struct
	{
		const gchar *name;
		gint         hours;
	} named[] = {
		{ "UT", 0 }, { "UTC", 0 }, { "GMT", 0 }, { "Z", 0 }, { "EST", -5 }, { "EDT", -4 }, { "CST", -6 },
		{ "CDT", -5 }, { "MST", -7 }, { "MDT", -6 }, { "PST", -8 }, { "PDT", -7 }
	};
	guint i;

	*offset = 0;
	if (text == NULL || text[0] == '\0')
		return TRUE;
	if ((text[0] == '+' || text[0] == '-') && strlen(text) == 5) {
		for (i = 1; i < 5; i++) {
			if (!g_ascii_isdigit(text[i]))
				return FALSE;
		}
		*offset = ((text[1] - '0') * 10 + (text[2] - '0')) * 3600 + ((text[3] - '0') * 10 + (text[4] - '0')) * 60;
		if (text[0] == '-')
			*offset = -*offset;
		return TRUE;
	}
	for (i = 0; i < G_N_ELEMENTS(named); i++) {
		if (g_ascii_strcasecmp(text, named[i].name) == 0) {
			*offset = named[i].hours * 3600;
			return TRUE;
		}
	}
	return FALSE;
}

gint64
axis_newznab_parse_date(const gchar *text)
{
	g_auto(GStrv) words = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 day = 0;
	gint64 year = 0;
	gint hour = 0;
	gint minute = 0;
	gint second = 0;
	gint offset = 0;
	gint month;
	guint n;
	guint i = 0;

	if (text == NULL || text[0] == '\0' || strlen(text) > 64)
		return 0;
	/* ISO 8601 ("2024-03-02T10:11:12Z"), which a few feeds write. */
	if (g_ascii_isdigit(text[0]) && strlen(text) >= 10 && text[4] == '-') {
		when = g_date_time_new_from_iso8601(text, NULL);
		if (when == NULL)
			return 0;
		year = g_date_time_get_year(when);
		if (year < DATE_MIN_YEAR || year > DATE_MAX_YEAR)
			return 0;
		return g_date_time_to_unix(when) * G_USEC_PER_SEC;
	}
	words = g_strsplit_set(text, " \t", -1);
	/* Words between runs of spaces are empty: drop them. */
	{
		GPtrArray *kept = g_ptr_array_new();
		gchar **old = words;

		for (n = 0; old[n] != NULL; n++) {
			if (old[n][0] != '\0')
				g_ptr_array_add(kept, g_strdup(old[n]));
		}
		g_ptr_array_add(kept, NULL);
		g_strfreev(old);
		words = (gchar **)g_ptr_array_free(kept, FALSE);
	}
	n = g_strv_length(words);
	/* An optional day of the week, "Sat," */
	if (i < n && g_ascii_isalpha(words[i][0]))
		i++;
	if (i + 4 > n)
		return 0;
	if (!int_of(words[i], 1, 31, &day))
		return 0;
	month = month_of(words[i + 1]);
	if (month == 0 || strlen(words[i + 1]) > 9)
		return 0;
	if (!int_of(words[i + 2], 0, 9999, &year))
		return 0;
	if (year < 100)
		year += year < 70 ? 2000 : 1900;
	{
		gint parts[3] = { 0, 0, 0 };
		guint k = 0;
		const gchar *p = words[i + 3];

		while (*p != '\0' && k < 3) {
			if (!g_ascii_isdigit(p[0]) || !g_ascii_isdigit(p[1]))
				return 0;
			parts[k++] = (p[0] - '0') * 10 + (p[1] - '0');
			p += 2;
			if (*p == ':')
				p++;
			else if (*p != '\0')
				return 0;
		}
		if (k < 2 || *p != '\0')
			return 0;
		hour = parts[0];
		minute = parts[1];
		second = parts[2];
	}
	if (!zone_of(i + 4 < n ? words[i + 4] : NULL, &offset))
		return 0;
	if (i + 5 < n)
		return 0;
	if (year < DATE_MIN_YEAR || year > DATE_MAX_YEAR || hour > 23 || minute > 59 || second > 60)
		return 0;
	when = g_date_time_new_utc((gint)year, month, (gint)day, hour, minute, second == 60 ? 59 : second);
	if (when == NULL)
		return 0;
	return (g_date_time_to_unix(when) - offset) * G_USEC_PER_SEC;
}

gchar *
axis_newznab_origin_of(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme;
	const gchar *host;
	gint port;

	if (!is_web_text(url) || strlen(url) > 4096)
		return NULL;
	uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
	if (uri == NULL)
		return NULL;
	scheme = g_uri_get_scheme(uri);
	host = g_uri_get_host(uri);
	if (scheme == NULL || host == NULL || host[0] == '\0' || g_uri_get_user(uri) != NULL)
		return NULL;
	{
		const gchar *p;

		for (p = host; *p != '\0'; p++) {
			if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-' && *p != ':' && *p != '[' && *p != ']')
				return NULL;
		}
	}
	port = g_uri_get_port(uri);
	if (port < 0)
		port = g_ascii_strcasecmp(scheme, "https") == 0 ? 443 : 80;
	{
		g_autofree gchar *candidate = strchr(host, ':') != NULL ?
			g_strdup_printf("%s://[%s]:%d", g_ascii_strcasecmp(scheme, "https") == 0 ? "https" : "http", host, port) :
			g_strdup_printf("%s://%s:%d", g_ascii_strcasecmp(scheme, "https") == 0 ? "https" : "http", host, port);
		AxisHttpOrigin origin;
		gchar *base;

		/* As an origin is written when it is configured: what an
		 * administrator accepts is what would be typed. */
		if (!axis_http_origin_parse(candidate, &origin, NULL))
			return NULL;
		base = g_strdup(origin.base);
		axis_http_origin_clear(&origin);
		return base;
	}
}

/* ── Documents ──────────────────────────────────────────────────── */

/* The root element's start: an <error> document's code and description. */
static void
read_error(
	AxisXmlReader    *reader,
	AxisNewznabError *error
){
	gint64 code = 0;

	axis_newznab_error_clear(error);
	if (int_of(axis_xml_reader_attribute(reader, "code"), 0, 99999, &code))
		error->code = (gint)code;
	else
		error->code = 900;
	error->description = axis_indexer_text_clean(axis_xml_reader_attribute(reader, "description"), 200);
}

/* <newznab:apilimits apicurrent apimax grabcurrent grabmax>, wherever it is. */
static void
read_allowance(
	AxisXmlReader        *reader,
	AxisIndexerAllowance *allowance
){
	static const gchar *const names[] = { "apicurrent", "apimax", "grabcurrent", "grabmax" };
	gint *fields[4];
	guint i;

	if (allowance == NULL)
		return;
	fields[0] = &allowance->api_used;
	fields[1] = &allowance->api_max;
	fields[2] = &allowance->grab_used;
	fields[3] = &allowance->grab_max;
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		gint64 value = 0;

		if (int_of(axis_xml_reader_attribute(reader, names[i]), 0, COUNT_MAX, &value))
			*fields[i] = (gint)value;
	}
	allowance->reported_us = 1;
}

/* The words of a supportedParams list, as bits and flags. */
static void
read_params(
	const gchar *list,
	const gchar *fallback,
	guint       *ids,
	gboolean    *text,
	gboolean    *season,
	gboolean    *episode
){
	g_auto(GStrv) words = NULL;
	guint i;

	if (list == NULL || strlen(list) > 1024)
		list = fallback;
	words = g_strsplit(list, ",", 64);
	for (i = 0; words[i] != NULL; i++) {
		g_autofree gchar *word = g_ascii_strdown(g_strstrip(words[i]), -1);

		if (strcmp(word, "q") == 0)
			*text = TRUE;
		else if (strcmp(word, "imdbid") == 0)
			*ids |= AXIS_INDEXER_IDS_IMDB;
		else if (strcmp(word, "tmdbid") == 0)
			*ids |= AXIS_INDEXER_IDS_TMDB;
		else if (strcmp(word, "tvdbid") == 0)
			*ids |= AXIS_INDEXER_IDS_TVDB;
		else if (strcmp(word, "season") == 0 && season != NULL)
			*season = TRUE;
		else if (strcmp(word, "ep") == 0 && episode != NULL)
			*episode = TRUE;
	}
}

AxisNewznabRead
axis_newznab_parse_caps(
	const gchar          *data,
	gsize                 length,
	AxisIndexerCaps      *caps,
	AxisIndexerAllowance *allowance,
	gchar               **version,
	AxisNewznabError     *error,
	gchar               **problem
){
	g_autoptr(AxisXmlReader) reader = NULL;
	g_autoptr(GPtrArray) categories = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *server = NULL;
	AxisIndexerCaps read;
	gboolean is_error = FALSE;
	gboolean root = TRUE;
	gboolean in_searching = FALSE;
	gboolean in_categories = FALSE;
	gboolean tv_season = FALSE;
	gboolean tv_episode = FALSE;
	AxisXmlEvent event;

	memset(caps, 0, sizeof(*caps));
	memset(&read, 0, sizeof(read));
	memset(error, 0, sizeof(*error));
	if (allowance != NULL)
		axis_indexer_allowance_init(allowance);
	if (version != NULL)
		*version = NULL;
	if (problem != NULL)
		*problem = NULL;
	reader = axis_xml_reader_new(data, length, NULL);
	while ((event = axis_xml_reader_next(reader)) != AXIS_XML_DONE && event != AXIS_XML_ERROR) {
		const gchar *local;
		guint depth = axis_xml_reader_depth(reader);

		if (event != AXIS_XML_START && event != AXIS_XML_END)
			continue;
		local = axis_xml_local_name(axis_xml_reader_name(reader));
		if (event == AXIS_XML_END) {
			if (strcmp(local, "searching") == 0)
				in_searching = FALSE;
			else if (strcmp(local, "categories") == 0)
				in_categories = FALSE;
			continue;
		}
		if (root) {
			root = FALSE;
			if (strcmp(local, "error") == 0) {
				is_error = TRUE;
				read_error(reader, error);
			} else if (strcmp(local, "caps") != 0) {
				if (problem != NULL)
					*problem = g_strdup("it is not a capabilities document");
				axis_newznab_error_clear(error);
				return AXIS_NEWZNAB_READ_INVALID;
			}
			continue;
		}
		if (is_error)
			continue;
		if (strcmp(local, "apilimits") == 0) {
			read_allowance(reader, allowance);
		} else if (depth == 2 && strcmp(local, "server") == 0 && server == NULL) {
			g_autofree gchar *title = axis_indexer_text_clean(axis_xml_reader_attribute(reader, "title"), 40);
			g_autofree gchar *number = axis_indexer_text_clean(axis_xml_reader_attribute(reader, "version"), 20);

			if (title != NULL && number != NULL)
				server = g_strdup_printf("%s %s", title, number);
			else if (title != NULL || number != NULL)
				server = g_strdup(title != NULL ? title : number);
		} else if (depth == 2 && strcmp(local, "limits") == 0) {
			gint64 value = 0;

			if (int_of(axis_xml_reader_attribute(reader, "max"), 1, 100000, &value))
				read.limit_max = (gint)value;
			if (int_of(axis_xml_reader_attribute(reader, "default"), 1, 100000, &value))
				read.limit_default = (gint)value;
		} else if (depth == 2 && strcmp(local, "searching") == 0) {
			in_searching = TRUE;
		} else if (depth == 2 && strcmp(local, "categories") == 0) {
			in_categories = TRUE;
		} else if (in_searching && depth == 3) {
			gboolean available = is_yes(axis_xml_reader_attribute(reader, "available"));
			const gchar *params = axis_xml_reader_attribute(reader, "supportedParams");

			if (!available)
				continue;
			if (strcmp(local, "search") == 0) {
				gboolean text = FALSE;
				guint ids = 0;

				read_params(params, "q", &ids, &text, NULL, NULL);
				read.modes |= AXIS_INDEXER_CAN_SEARCH;
			} else if (strcmp(local, "tv-search") == 0) {
				read.modes |= AXIS_INDEXER_CAN_TV;
				read_params(params, "q,season,ep", &read.tv_ids, &read.tv_text, &tv_season, &tv_episode);
			} else if (strcmp(local, "movie-search") == 0) {
				read.modes |= AXIS_INDEXER_CAN_MOVIE;
				read_params(params, "q,imdbid", &read.movie_ids, &read.movie_text, NULL, NULL);
			}
		} else if (in_categories && (depth == 3 || depth == 4) &&
		           (strcmp(local, "category") == 0 || strcmp(local, "subcat") == 0)) {
			gint64 id = 0;
			g_autofree gchar *name = axis_indexer_text_clean(axis_xml_reader_attribute(reader, "name"), 60);

			if (categories->len < AXIS_INDEXER_MAX_CAPS_CATEGORIES &&
			    int_of(axis_xml_reader_attribute(reader, "id"), 1, 999999, &id))
				g_ptr_array_add(categories, g_strdup_printf("%" G_GINT64_FORMAT "%s%s", id, name != NULL ? " " : "",
					name != NULL ? name : ""));
		}
	}
	if (event == AXIS_XML_ERROR) {
		if (problem != NULL)
			*problem = g_strdup(axis_xml_reader_error(reader));
		axis_newznab_error_clear(error);
		if (allowance != NULL)
			axis_indexer_allowance_init(allowance);
		return AXIS_NEWZNAB_READ_INVALID;
	}
	if (is_error) {
		if (allowance != NULL)
			axis_indexer_allowance_init(allowance);
		return AXIS_NEWZNAB_READ_ERROR;
	}
	read.tv_numbers = tv_season && tv_episode;
	if (categories->len > 0) {
		g_ptr_array_add(categories, NULL);
		read.categories = (gchar **)g_ptr_array_steal(categories, NULL);
	}
	*caps = read;
	if (version != NULL)
		*version = g_steal_pointer(&server);
	return AXIS_NEWZNAB_READ_OK;
}

/* ── A feed ─────────────────────────────────────────────────────── */

/* What one <item> held, as text. */
typedef struct
{
	GString   *title;
	GString   *guid;
	GString   *link;
	GString   *date;
	GString   *size;
	gchar     *enclosure;
	gchar     *enclosure_length;
	GPtrArray *names;	/* attr names, lower case */
	GPtrArray *values;
	guint      depth;	/* the item's own */
	GString   *field;	/* the child whose text is read now, or NULL */
} Item;

static void
item_init(
	Item  *item,
	guint  depth
){
	memset(item, 0, sizeof(*item));
	item->title = g_string_new(NULL);
	item->guid = g_string_new(NULL);
	item->link = g_string_new(NULL);
	item->date = g_string_new(NULL);
	item->size = g_string_new(NULL);
	item->names = g_ptr_array_new_with_free_func(g_free);
	item->values = g_ptr_array_new_with_free_func(g_free);
	item->depth = depth;
}

static void
wipe_string(GString *text)
{
	if (text == NULL)
		return;
	memset(text->str, 0, text->len);
	g_string_free(text, TRUE);
}

static void
wipe_text(gchar *text)
{
	if (text == NULL)
		return;
	memset(text, 0, strlen(text));
	g_free(text);
}

static void
item_clear(Item *item)
{
	if (item->title == NULL)
		return;
	g_string_free(item->title, TRUE);
	wipe_string(item->guid);
	wipe_string(item->link);
	g_string_free(item->date, TRUE);
	g_string_free(item->size, TRUE);
	wipe_text(item->enclosure);
	g_free(item->enclosure_length);
	g_ptr_array_unref(item->names);
	g_ptr_array_set_free_func(item->values, (GDestroyNotify)wipe_text);
	g_ptr_array_unref(item->values);
	memset(item, 0, sizeof(*item));
}

static const gchar *
item_attr(
	const Item  *item,
	const gchar *name
){
	guint i;

	for (i = 0; i < item->names->len; i++) {
		if (strcmp(g_ptr_array_index(item->names, i), name) == 0)
			return g_ptr_array_index(item->values, i);
	}
	return NULL;
}

/* The values of every attr of @name, as numbers (categories). */
static void
item_categories(
	const Item         *item,
	AxisIndexerRelease *release
){
	guint i;
	guint j;

	for (i = 0; i < item->names->len && release->n_categories < AXIS_INDEXER_MAX_CATEGORIES; i++) {
		gint64 value = 0;
		gboolean seen = FALSE;

		if (strcmp(g_ptr_array_index(item->names, i), "category") != 0 ||
		    !int_of(g_ptr_array_index(item->values, i), 1, 999999, &value))
			continue;
		for (j = 0; j < release->n_categories; j++)
			seen = seen || release->categories[j] == (gint)value;
		if (!seen)
			release->categories[release->n_categories++] = (gint)value;
	}
}

static gint64
attr_int(
	const Item  *item,
	const gchar *name,
	gint64       minimum,
	gint64       maximum,
	gint64       fallback
){
	gint64 value = 0;

	return int_of(item_attr(item, name), minimum, maximum, &value) ? value : fallback;
}

/* "tt0600001" from "0600001", "600001" or "tt0600001". */
static gchar *
imdb_of(const gchar *text)
{
	gint64 value = 0;

	if (text == NULL)
		return NULL;
	if (axis_indexer_imdb_valid(text))
		return g_strdup(text);
	if (g_ascii_strncasecmp(text, "tt", 2) == 0)
		text += 2;
	if (strlen(text) > 10 || !int_of(text, 1, 9999999999, &value))
		return NULL;
	return g_strdup_printf("tt%07" G_GINT64_FORMAT, value);
}

static void
add_flag(
	GPtrArray   *flags,
	const gchar *flag
){
	guint i;

	for (i = 0; i < flags->len; i++) {
		if (strcmp(g_ptr_array_index(flags, i), flag) == 0)
			return;
	}
	if (flags->len < AXIS_INDEXER_MAX_FLAGS)
		g_ptr_array_add(flags, g_strdup(flag));
}

typedef enum
{
	ITEM_KEPT,
	ITEM_SKIPPED,
	ITEM_OFF_ORIGIN,
	ITEM_UNUSABLE
} ItemResult;

/*
 * One of an item's web addresses for its file: %FALSE when it is an address
 * that is neither the indexer's own nor the accepted one (*@other: its
 * origin); otherwise *@path is the path and query on the origin it is on,
 * *@files whether that is the accepted one, or %NULL when it is no usable
 * address.
 */
static gboolean
read_address(
	const gchar               *url,
	const AxisNewznabContext  *context,
	gchar                    **path,
	gboolean                  *files,
	gchar                    **other
){
	*path = NULL;
	*files = FALSE;
	if (!is_web_text(url))
		return TRUE;
	switch (axis_indexer_fetch_ref(url, context->origin, "", path)) {
	case AXIS_INDEXER_REF_OK:
		return TRUE;
	case AXIS_INDEXER_REF_OFF_ORIGIN:
		if (context->files_origin != NULL &&
		    axis_indexer_fetch_ref(url, context->files_origin, "", path) == AXIS_INDEXER_REF_OK) {
			*files = TRUE;
			return TRUE;
		}
		g_clear_pointer(path, g_free);
		if (*other == NULL)
			*other = axis_newznab_origin_of(url);
		return FALSE;
	default:
		g_clear_pointer(path, g_free);
		return TRUE;
	}
}

static ItemResult
item_found(
	const Item                *item,
	const AxisNewznabContext  *context,
	gchar                    **other,
	AxisIndexerFound         **found
){
	g_autoptr(AxisIndexerRelease) release = NULL;
	g_autofree gchar *name = axis_indexer_text_clean(item->title->str, AXIS_INDEXER_NAME_MAX_CHARS);
	g_autofree gchar *fetch_path = NULL;
	g_autofree gchar *link_path = NULL;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *hash = NULL;
	gboolean torrent = context->protocol == AXIS_DOWNLOAD_TORRENT;
	gboolean files = FALSE;
	gboolean link_files = FALSE;
	const gchar *link = item->link->len > 0 ? item->link->str : NULL;
	GPtrArray *flags;
	AxisIndexerFound *kept;
	gint64 size;
	gdouble factor;

	*found = NULL;
	if (name == NULL)
		return ITEM_SKIPPED;

	/* How to fetch it: the enclosure's address, else the link; for a
	 * torrent also a magnet link (the magneturl attribute, the enclosure,
	 * the link).  Either address elsewhere drops the item. */
	if (!read_address(item->enclosure, context, &fetch_path, &files, other) ||
	    !read_address(link, context, &link_path, &link_files, other))
		return ITEM_OFF_ORIGIN;
	if (fetch_path == NULL) {
		fetch_path = g_steal_pointer(&link_path);
		files = link_files;
	}
	if (torrent) {
		const gchar *candidates[3];
		guint i;

		candidates[0] = item_attr(item, "magneturl");
		candidates[1] = item->enclosure;
		candidates[2] = link;
		for (i = 0; i < G_N_ELEMENTS(candidates) && magnet == NULL; i++) {
			if (is_magnet_text(candidates[i]) && strlen(candidates[i]) <= 4096 &&
			    axis_download_magnet_parse(candidates[i], &hash, NULL))
				magnet = g_strdup(candidates[i]);
		}
	}
	if (fetch_path == NULL && magnet == NULL)
		return ITEM_UNUSABLE;

	release = axis_indexer_release_new();
	release->name = g_steal_pointer(&name);
	release->protocol = context->protocol;
	size = attr_int(item, "size", 0, SIZE_MAX_BYTES, -1);
	if (size < 0 && item->enclosure_length != NULL) {
		gint64 value = 0;

		if (int_of(item->enclosure_length, 0, SIZE_MAX_BYTES, &value) && value > 0)
			size = value;
	}
	if (size < 0 && item->size->len > 0) {
		gint64 value = 0;

		if (int_of(item->size->str, 0, SIZE_MAX_BYTES, &value))
			size = value;
	}
	release->size_bytes = size;
	release->published_us = axis_newznab_parse_date(item->date->str);
	if (context->now_us > 0 && release->published_us > context->now_us + DATE_FUTURE_SLACK_US)
		release->published_us = 0;
	release->grabs = (gint)attr_int(item, "grabs", 0, COUNT_MAX, -1);
	item_categories(item, release);
	release->imdb_id = imdb_of(item_attr(item, "imdb"));
	if (release->imdb_id == NULL)
		release->imdb_id = imdb_of(item_attr(item, "imdbid"));
	release->tmdb_id = attr_int(item, "tmdbid", 1, G_MAXINT32, 0);
	if (release->tmdb_id == 0)
		release->tmdb_id = attr_int(item, "tmdb", 1, G_MAXINT32, 0);
	release->tvdb_id = attr_int(item, "tvdbid", 1, G_MAXINT32, 0);
	if (release->tvdb_id == 0)
		release->tvdb_id = attr_int(item, "tvdb", 1, G_MAXINT32, 0);

	flags = g_ptr_array_new_with_free_func(g_free);
	if (torrent) {
		gint64 seeders = attr_int(item, "seeders", 0, COUNT_MAX, -1);
		gint64 peers = attr_int(item, "peers", 0, COUNT_MAX, -1);
		gint64 leechers = attr_int(item, "leechers", 0, COUNT_MAX, -1);
		const gchar *stated = item_attr(item, "infohash");
		g_autofree gchar *lower = stated != NULL && strlen(stated) == 40 ? g_ascii_strdown(stated, -1) : NULL;

		release->seeders = (gint)seeders;
		/* Torznab's peers are the seeders and the leechers together. */
		if (leechers >= 0)
			release->leechers = (gint)leechers;
		else if (peers >= 0 && seeders >= 0 && peers >= seeders)
			release->leechers = (gint)(peers - seeders);
		if (axis_download_hash_valid(lower))
			release->info_hash = g_steal_pointer(&lower);
		else if (hash != NULL)
			release->info_hash = g_steal_pointer(&hash);
		factor = factor_of(item_attr(item, "downloadvolumefactor"));
		if (factor == 0)
			add_flag(flags, "freeleech");
		else if (factor > 0 && factor < 1)
			add_flag(flags, "partial freeleech");
		factor = factor_of(item_attr(item, "uploadvolumefactor"));
		if (factor >= 2)
			add_flag(flags, "double upload");
	} else {
		const gchar *password = item_attr(item, "password");

		if (g_strcmp0(password, "1") == 0 || g_strcmp0(password, "2") == 0)
			add_flag(flags, "passworded");
	}
	if (flags->len > 0) {
		g_ptr_array_add(flags, NULL);
		release->flags = (gchar **)g_ptr_array_free(flags, FALSE);
	} else {
		g_ptr_array_unref(flags);
	}
	release->has_magnet = magnet != NULL;

	kept = g_new0(AxisIndexerFound, 1);
	if (item->guid->len > 0) {
		kept->merge_key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, item->guid->str, (gssize)item->guid->len);
	} else {
		g_autofree gchar *fallback = g_strdup_printf("no guid|%s|%" G_GINT64_FORMAT, release->name,
			release->size_bytes);

		kept->merge_key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, fallback, -1);
	}
	kept->release = g_steal_pointer(&release);
	kept->fetch_path = g_steal_pointer(&fetch_path);
	kept->files_origin = kept->fetch_path != NULL && files;
	kept->magnet = g_steal_pointer(&magnet);
	*found = kept;
	return ITEM_KEPT;
}

/* Appends @text to @field, up to its cap. */
static void
add_text(
	GString     *field,
	const gchar *text,
	gsize        length
){
	if (field->len >= FIELD_MAX_BYTES)
		return;
	g_string_append_len(field, text, (gssize)MIN(length, FIELD_MAX_BYTES - field->len));
}

AxisNewznabRead
axis_newznab_parse_search(
	const gchar               *data,
	gsize                      length,
	const AxisNewznabContext  *context,
	AxisIndexerOutcome        *outcome,
	AxisIndexerAllowance      *allowance,
	AxisNewznabError          *error,
	gchar                    **problem
){
	g_autoptr(AxisXmlReader) reader = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *other = NULL;
	AxisIndexerOutcome read;
	gboolean is_error = FALSE;
	gboolean root = TRUE;
	gboolean in_item = FALSE;
	guint scanned = 0;
	Item item;
	AxisXmlEvent event;

	axis_indexer_outcome_init(outcome);
	axis_indexer_outcome_init(&read);
	memset(error, 0, sizeof(*error));
	memset(&item, 0, sizeof(item));
	if (allowance != NULL)
		axis_indexer_allowance_init(allowance);
	if (problem != NULL)
		*problem = NULL;
	read.items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_found_free);
	read.total = -1;
	reader = axis_xml_reader_new(data, length, NULL);
	while ((event = axis_xml_reader_next(reader)) != AXIS_XML_DONE && event != AXIS_XML_ERROR) {
		guint depth = axis_xml_reader_depth(reader);
		const gchar *local;

		if (event == AXIS_XML_TEXT) {
			if (in_item && item.field != NULL && depth == item.depth + 1) {
				gsize n = 0;
				const gchar *text = axis_xml_reader_text(reader, &n);

				add_text(item.field, text, n);
			}
			continue;
		}
		local = axis_xml_local_name(axis_xml_reader_name(reader));
		if (event == AXIS_XML_END) {
			if (in_item && depth == item.depth + 1) {
				item.field = NULL;
			} else if (in_item && depth == item.depth) {
				AxisIndexerFound *found = NULL;

				in_item = FALSE;
				read.n_listed++;
				if (read.items->len >= AXIS_INDEXER_MAX_RESULTS || scanned >= AXIS_INDEXER_MAX_SCAN) {
					read.truncated = TRUE;
					item_clear(&item);
					continue;
				}
				scanned++;
				switch (item_found(&item, context, &other, &found)) {
				case ITEM_KEPT:
					if (g_hash_table_contains(seen, found->merge_key)) {
						axis_indexer_found_free(found);
						break;
					}
					g_hash_table_add(seen, g_strdup(found->merge_key));
					g_ptr_array_add(read.items, found);
					break;
				case ITEM_OFF_ORIGIN:
					read.n_off_origin++;
					break;
				case ITEM_UNUSABLE:
					read.n_unusable++;
					break;
				default:
					read.n_skipped++;
					break;
				}
				item_clear(&item);
			}
			continue;
		}
		/* A start. */
		if (root) {
			root = FALSE;
			if (strcmp(local, "error") == 0) {
				is_error = TRUE;
				read_error(reader, error);
			} else if (strcmp(local, "rss") != 0) {
				axis_indexer_outcome_clear(&read);
				if (problem != NULL)
					*problem = g_strdup("it is not an RSS feed");
				return AXIS_NEWZNAB_READ_INVALID;
			}
			continue;
		}
		if (is_error)
			continue;
		if (strcmp(local, "apilimits") == 0) {
			read_allowance(reader, allowance);
			continue;
		}
		if (!in_item) {
			if (strcmp(local, "item") == 0) {
				in_item = TRUE;
				item_clear(&item);
				item_init(&item, depth);
			} else if (strcmp(local, "response") == 0) {
				/* <newznab:response offset="0" total="214"/>: how many
				 * there are, where the indexer says. */
				const gchar *total = axis_xml_reader_attribute(reader, "total");
				gint64 value = 0;
				const gchar *p;

				for (p = total; p != NULL && *p != '\0' && g_ascii_isdigit(*p) && value <= G_MAXINT32; p++)
					value = value * 10 + (*p - '0');
				if (total != NULL && total[0] != '\0' && *p == '\0' && value <= G_MAXINT32)
					read.total = value;
			}
			continue;
		}
		if (depth != item.depth + 1)
			continue;
		item.field = NULL;
		if (strcmp(local, "title") == 0)
			item.field = item.title;
		else if (strcmp(local, "guid") == 0)
			item.field = item.guid;
		else if (strcmp(local, "link") == 0)
			item.field = item.link;
		else if (strcmp(local, "pubDate") == 0)
			item.field = item.date;
		else if (strcmp(local, "size") == 0)
			item.field = item.size;
		else if (strcmp(local, "enclosure") == 0 && item.enclosure == NULL) {
			const gchar *url = axis_xml_reader_attribute(reader, "url");
			const gchar *len = axis_xml_reader_attribute(reader, "length");

			/* The first enclosure with an address is the item's. */
			item.enclosure = url != NULL && strlen(url) <= FIELD_MAX_BYTES ? g_strdup(url) : NULL;
			g_free(item.enclosure_length);
			item.enclosure_length = len != NULL && strlen(len) <= 32 ? g_strdup(len) : NULL;
		} else if (strcmp(local, "attr") == 0 && item.names->len < ITEM_MAX_ATTRS) {
			const gchar *name = axis_xml_reader_attribute(reader, "name");
			const gchar *value = axis_xml_reader_attribute(reader, "value");

			if (name != NULL && value != NULL && strlen(name) <= 64 && strlen(value) <= FIELD_MAX_BYTES) {
				g_ptr_array_add(item.names, g_ascii_strdown(name, -1));
				g_ptr_array_add(item.values, g_strdup(value));
			}
		}
	}
	item_clear(&item);
	if (event == AXIS_XML_ERROR) {
		/* Nothing of an answer that is refused anywhere is taken. */
		axis_indexer_outcome_clear(&read);
		axis_newznab_error_clear(error);
		if (allowance != NULL)
			axis_indexer_allowance_init(allowance);
		if (problem != NULL)
			*problem = g_strdup(axis_xml_reader_error(reader));
		return AXIS_NEWZNAB_READ_INVALID;
	}
	if (is_error) {
		axis_indexer_outcome_clear(&read);
		if (allowance != NULL)
			axis_indexer_allowance_init(allowance);
		return AXIS_NEWZNAB_READ_ERROR;
	}
	read.other_origin = g_steal_pointer(&other);
	*outcome = read;
	return AXIS_NEWZNAB_READ_OK;
}

gboolean
axis_newznab_parse_error_only(
	const gchar      *data,
	gsize             length,
	AxisNewznabError *error
){
	g_autoptr(AxisXmlReader) reader = NULL;
	gboolean root = TRUE;
	gboolean is_error = FALSE;
	AxisXmlEvent event;
	AxisXmlLimits limits;

	memset(error, 0, sizeof(*error));
	memset(&limits, 0, sizeof(limits));
	limits.max_bytes = 64 * 1024;
	reader = axis_xml_reader_new(data, length, &limits);
	while ((event = axis_xml_reader_next(reader)) != AXIS_XML_DONE && event != AXIS_XML_ERROR) {
		if (event == AXIS_XML_START && root) {
			root = FALSE;
			if (strcmp(axis_xml_local_name(axis_xml_reader_name(reader)), "error") == 0) {
				is_error = TRUE;
				read_error(reader, error);
			}
		}
	}
	if (event == AXIS_XML_ERROR || !is_error) {
		axis_newznab_error_clear(error);
		return FALSE;
	}
	return TRUE;
}

/* ── Asking ─────────────────────────────────────────────────────── */

static gboolean
caps_known(const AxisIndexerCaps *caps)
{
	return caps != NULL && caps->read_us > 0 && caps->problem == NULL;
}

guint
axis_newznab_identifiers(
	const AxisIndexerRequest *request,
	const AxisIndexerCaps    *caps,
	AxisIndexerMode          *modes
){
	guint n = 0;

	if (!caps_known(caps))
		return 0;
	switch (request->kind) {
	case AXIS_INDEXER_QUERY_FILM:
		if ((caps->modes & AXIS_INDEXER_CAN_MOVIE) == 0)
			break;
		if (request->imdb_id != NULL && (caps->movie_ids & AXIS_INDEXER_IDS_IMDB) != 0)
			modes[n++] = AXIS_INDEXER_BY_IMDB;
		if (request->tmdb_id > 0 && (caps->movie_ids & AXIS_INDEXER_IDS_TMDB) != 0)
			modes[n++] = AXIS_INDEXER_BY_TMDB;
		break;
	case AXIS_INDEXER_QUERY_EPISODE:
	case AXIS_INDEXER_QUERY_SEASON:
		if ((caps->modes & AXIS_INDEXER_CAN_TV) == 0)
			break;
		/* Without a season (and an episode) it takes, an identifier would
		 * ask for the whole series. */
		if (!caps->tv_numbers)
			break;
		if (request->tvdb_id > 0 && (caps->tv_ids & AXIS_INDEXER_IDS_TVDB) != 0)
			modes[n++] = AXIS_INDEXER_BY_TVDB;
		if (request->imdb_id != NULL && (caps->tv_ids & AXIS_INDEXER_IDS_IMDB) != 0)
			modes[n++] = AXIS_INDEXER_BY_IMDB;
		if (request->tmdb_id > 0 && (caps->tv_ids & AXIS_INDEXER_IDS_TMDB) != 0)
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

static void
append_number(
	GString     *path,
	const gchar *name,
	gint64       value
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);

	append_param(path, name, text);
}

static void
append_tail(
	GString               *path,
	const AxisIndexerCaps *caps,
	const gint            *categories,
	guint                  n_categories
){
	gint limit = (gint)axis_newznab_page_limit(caps);
	GString *list = g_string_new(NULL);
	guint i;

	for (i = 0; i < n_categories && i < AXIS_INDEXER_MAX_CATEGORIES; i++)
		g_string_append_printf(list, "%s%d", i > 0 ? "," : "", categories[i]);
	if (list->len > 0)
		append_param(path, "cat", list->str);
	g_string_free(list, TRUE);
	/* Every attribute of a result, not only a few. */
	append_param(path, "extended", "1");
	append_number(path, "limit", limit);
	append_number(path, "offset", 0);
}

gchar *
axis_newznab_search_path(
	const AxisIndexerRequest *request,
	AxisIndexerMode           mode,
	const AxisIndexerCaps    *caps,
	const gint               *categories,
	guint                     n_categories
){
	g_autofree gchar *title = axis_indexer_text_clean(request->title, AXIS_INDEXER_TITLE_MAX_CHARS);
	gboolean known = caps_known(caps);
	gboolean tv = request->kind == AXIS_INDEXER_QUERY_EPISODE || request->kind == AXIS_INDEXER_QUERY_SEASON;
	GString *path = g_string_new("/api");

	if (mode == AXIS_INDEXER_BY_TEXT) {
		if (title == NULL) {
			g_string_free(path, TRUE);
			return NULL;
		}
		if (request->kind == AXIS_INDEXER_QUERY_FILM && known && (caps->modes & AXIS_INDEXER_CAN_MOVIE) != 0 &&
		    caps->movie_text) {
			g_autofree gchar *query = request->year > 0 ? g_strdup_printf("%s %d", title, request->year) :
				g_strdup(title);

			append_param(path, "t", "movie");
			append_param(path, "q", query);
		} else if (tv && known && (caps->modes & AXIS_INDEXER_CAN_TV) != 0 && caps->tv_text && caps->tv_numbers) {
			append_param(path, "t", "tvsearch");
			append_param(path, "q", title);
			append_number(path, "season", request->season);
			if (request->kind == AXIS_INDEXER_QUERY_EPISODE)
				append_number(path, "ep", request->episode);
		} else {
			g_autofree gchar *query = NULL;

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
			append_param(path, "t", "search");
			append_param(path, "q", query);
		}
	} else if (request->kind == AXIS_INDEXER_QUERY_FILM) {
		append_param(path, "t", "movie");
		if (mode == AXIS_INDEXER_BY_IMDB && request->imdb_id != NULL) {
			append_param(path, "imdbid", request->imdb_id + 2);
		} else if (mode == AXIS_INDEXER_BY_TMDB && request->tmdb_id > 0) {
			append_number(path, "tmdbid", request->tmdb_id);
		} else {
			g_string_free(path, TRUE);
			return NULL;
		}
	} else if (tv) {
		append_param(path, "t", "tvsearch");
		if (mode == AXIS_INDEXER_BY_TVDB && request->tvdb_id > 0) {
			append_number(path, "tvdbid", request->tvdb_id);
		} else if (mode == AXIS_INDEXER_BY_IMDB && request->imdb_id != NULL) {
			append_param(path, "imdbid", request->imdb_id + 2);
		} else if (mode == AXIS_INDEXER_BY_TMDB && request->tmdb_id > 0) {
			append_number(path, "tmdbid", request->tmdb_id);
		} else {
			g_string_free(path, TRUE);
			return NULL;
		}
		append_number(path, "season", request->season);
		if (request->kind == AXIS_INDEXER_QUERY_EPISODE)
			append_number(path, "ep", request->episode);
	} else {
		g_string_free(path, TRUE);
		return NULL;
	}
	append_tail(path, caps, categories, n_categories);
	return g_string_free(path, FALSE);
}

gchar *
axis_newznab_latest_path(
	const gint *categories,
	guint       n_categories,
	guint       limit
){
	GString *path = g_string_new("/api");
	GString *list = g_string_new(NULL);
	guint i;

	append_param(path, "t", "search");
	for (i = 0; i < n_categories && i < AXIS_INDEXER_MAX_CATEGORIES; i++)
		g_string_append_printf(list, "%s%d", i > 0 ? "," : "", categories[i]);
	if (list->len > 0)
		append_param(path, "cat", list->str);
	g_string_free(list, TRUE);
	append_param(path, "extended", "1");
	append_number(path, "limit", CLAMP(limit, 1, 100));
	return g_string_free(path, FALSE);
}

guint
axis_newznab_page_limit(const AxisIndexerCaps *caps)
{
	return caps_known(caps) && caps->limit_max > 0 ? (guint)MIN(caps->limit_max, AXIS_INDEXER_MAX_RESULTS) :
		AXIS_NEWZNAB_DEFAULT_LIMIT;
}

gchar *
axis_newznab_path_at_offset(
	const gchar *path,
	guint        offset
){
	static const gchar tail[] = "&offset=0";
	gsize length = strlen(path);

	if (length < sizeof(tail) - 1 || strcmp(path + length - (sizeof(tail) - 1), tail) != 0)
		return NULL;
	return g_strdup_printf("%.*s&offset=%u", (gint)(length - (sizeof(tail) - 1)), path, offset);
}
