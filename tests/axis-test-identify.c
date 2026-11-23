/*
 * axis-test-identify.c - test support for identification (see the header)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-identify.h"

#include <stdio.h>
#include <string.h>

static const AxisTestFilm films[] = {
	{ "6001", "Hearth", 1995, 170, "tt0600001" },
	{ "6002", "Rokan", 1998, 122, "tt0600002" },
	{ "6003", "The Mazework", 1999, 136, "tt0600003" },
	{ "6004", "Colliery", 2004, 120, "tt0600004" },
	{ "6005", "Thistle", 1981, 123, "tt0600005" },
	{ "6006", "Dunmore", 2021, 155, "tt0600006" },
	{ "6007", "Dunmore", 1984, 137, "tt0600007" },
	{ "6008", "Solvane", 1972, 167, "tt0600008" },
	{ "6009", "Solvane", 2002, 99, "tt0600009" },
	{ "6010", "Manhollow", 1986, 120, "tt0610000" },
	{ "6011", "Insmere", 2002, 118, "tt0620000" },
	{ "6012", "Blackwick", 2015, 133, "tt0630000" },
	{ "6013", "Public Rivals", 2009, 140, "tt0640000" },
	{ "6014", "Aro", 2001, 157, "tt0650000" },
	{ "6015", "The Inlander", 1999, 157, "tt0660000" },
	{ "6016", "The Prestwick", 2006, 130, "tt0670000" },
	{ NULL, NULL, 0, 0, NULL }
};

const AxisTestFilm *
axis_test_identify_films(void)
{
	return films;
}

/* Made-up series (none is a real one). */
static const AxisTestSeries series_table[] = {
	{ "7001", "Lantern Row", 2015, 45, "81001", "tt7000001", "2015-01-05", { 3, 3, 0, 0 }, 1, FALSE, FALSE, NULL },
	{ "7002", "Copper Valley", 2019, 30, "81002", "tt7000002", "2019-03-04", { 4, 0, 0, 0 }, 0, FALSE, FALSE, NULL },
	{ "7003", "Copper Valley", 2004, 50, "81003", "tt7000003", "2004-09-06", { 2, 0, 0, 0 }, 0, FALSE, FALSE, NULL },
	{ "7004", "The Night Ledger", 2024, 40, "81004", "tt7000004", "2024-03-11", { 7, 0, 0, 0 }, 0, TRUE, TRUE, NULL },
	{ "7005", "Harrow Fields", 2010, 60, "81005", "tt7000005", "2010-10-04", { 2, 2, 0, 0 }, 0, FALSE, FALSE, NULL },
	{ "7006", "Glass Harbor", 2018, 24, "81006", "tt7000006", "2018-04-02", { 12, 0, 0, 0 }, 0, FALSE, FALSE, NULL },
	{ "7007", "Saltmarsh Signals", 2022, 50, "81007", "tt7000007", "2022-02-07", { 3, 3, 0, 0 }, 0, FALSE, FALSE,
	  NULL },
	/* Still airing on 2026-11-14 (the tests' clock): weekly from 2026-10-05. */
	{ "7008", "Ember Station", 2026, 42, "81008", "tt7000008", "2026-10-05", { 8, 0, 0, 0 }, 0, FALSE, FALSE, NULL },
	/* Two of one name whose first seasons both have five episodes or more. */
	{ "7009", "Brass Meridian", 2008, 45, "81009", "tt7000009", "2008-01-07", { 10, 10, 0, 0 }, 0, FALSE, FALSE, NULL },
	{ "7010", "Brass Meridian", 2021, 45, "81010", "tt7000010", "2021-05-03", { 8, 0, 0, 0 }, 0, FALSE, FALSE, NULL },
	/* Known under another name too. */
	{ "7011", "Kestrel Point", 2016, 50, "81011", "tt7000011", "2016-02-01", { 6, 0, 0, 0 }, 0, FALSE, FALSE,
	  "Kestrel Bay" },
	/* No runtime given. */
	{ "7012", "Juniper Hollow", 2012, 0, "81012", "tt7000012", "2012-09-03", { 6, 0, 0, 0 }, 0, FALSE, FALSE, NULL },
	{ NULL, NULL, 0, 0, NULL, NULL, NULL, { 0, 0, 0, 0 }, 0, FALSE, FALSE, NULL }
};

/* "Shelf Series <n>" (see the header): @number, or 0 when @text is not one
 * ("shelf<n>" as an id with @by_id, else the title). */
static guint
shelf_number(
	const gchar *text,
	gboolean     by_id
){
	const gchar *digits;
	guint64 n = 0;

	if (text == NULL)
		return 0;
	if (by_id && g_str_has_prefix(text, "shelf"))
		digits = text + strlen("shelf");
	else if (!by_id && g_ascii_strncasecmp(text, "shelf series ", strlen("shelf series ")) == 0)
		digits = text + strlen("shelf series ");
	else
		return 0;
	if (digits[0] == '0' || !g_ascii_string_to_unsigned(digits, 10, 1, 99999, &n, NULL))
		return 0;
	return (guint)n;
}

/* The made-up library's series @n, its strings in @owned. */
static void
shelf_series(
	guint           n,
	AxisTestSeries *out,
	GPtrArray      *owned
){
	memset(out, 0, sizeof(*out));
	out->id = g_strdup_printf("shelf%u", n);
	g_ptr_array_add(owned, (gpointer)out->id);
	out->title = g_strdup_printf("Shelf Series %u", n);
	g_ptr_array_add(owned, (gpointer)out->title);
	out->tvdb = g_strdup_printf("9%u", n);
	g_ptr_array_add(owned, (gpointer)out->tvdb);
	out->imdb = g_strdup_printf("tt9%06u", n);
	g_ptr_array_add(owned, (gpointer)out->imdb);
	out->year = 1960 + (gint)(n % 60);
	out->first_aired = "2001-01-01";
	out->seasons[0] = out->seasons[1] = out->seasons[2] = out->seasons[3] = 10;
}

static guint season_requests;

const AxisTestSeries *
axis_test_identify_series(void)
{
	return series_table;
}

guint
axis_test_identify_season_requests(void)
{
	return g_atomic_int_get(&season_requests);
}

/* The air date of episode @at (counted from 0 across the series). */
static gchar *
aired(
	const AxisTestSeries *s,
	guint                 at
){
	gint year = 0;
	gint month = 0;
	gint day = 0;
	GDate *date;
	gchar text[11];
	guint step;
	guint n_regular = 0;
	guint i;

	if (sscanf(s->first_aired, "%4d-%2d-%2d", &year, &month, &day) != 3)
		return g_strdup("2000-01-01");
	for (i = 0; i < G_N_ELEMENTS(s->seasons); i++)
		n_regular += (guint)s->seasons[i];
	step = at;
	/* Two on one day: the last one airs the day the one before it does. */
	if (s->two_on_day && n_regular >= 2 && at == n_regular - 1)
		step = at - 1;
	date = g_date_new_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year);
	g_date_add_days(date, s->daily ? step : step * 7);
	g_snprintf(text, sizeof(text), "%04d-%02d-%02d", g_date_get_year(date), g_date_get_month(date),
		g_date_get_day(date));
	g_date_free(date);
	return g_strdup(text);
}

static void
append_series(
	GString              *json,
	const AxisTestSeries *s,
	gboolean              full
){
	guint i;

	g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"series\",\"title\":\"%s\",\"year\":%d,"
		"\"poster\":\"/p%s.png\",\"ids\":{\"imdb\":\"%s\",\"tvdb\":\"%s\",\"tmdb\":\"%s\"}", s->id, s->title, s->year,
		s->id, s->imdb, s->tvdb, s->id);
	if (full) {
		g_autofree gchar *first = aired(s, 0);
		gboolean any = FALSE;

		if (s->runtime > 0)
			g_string_append_printf(json, ",\"runtime\":%d", s->runtime);
		if (s->other_name != NULL)
			g_string_append_printf(json, ",\"alt_titles\":[{\"title\":\"%s\"}]", s->other_name);
		g_string_append_printf(json, ",\"status\":\"ended\",\"first_air_date\":\"%s\","
			"\"overview\":\"A series called %s.\",\"seasons\":[", first, s->title);
		if (s->specials > 0) {
			g_string_append_printf(json, "{\"number\":0,\"name\":\"Specials\",\"episodes\":%d}", s->specials);
			any = TRUE;
		}
		for (i = 0; i < G_N_ELEMENTS(s->seasons); i++) {
			if (s->seasons[i] <= 0)
				continue;
			g_string_append_printf(json, "%s{\"number\":%u,\"name\":\"Season %u\",\"episodes\":%d}", any ? "," : "",
				i + 1, i + 1, s->seasons[i]);
			any = TRUE;
		}
		g_string_append_c(json, ']');
	}
	g_string_append_c(json, '}');
}

static void
append_season(
	GString              *json,
	const AxisTestSeries *s,
	guint                 number
){
	guint before = 0;
	guint count;
	guint e;
	guint i;

	if (number == 0) {
		count = (guint)s->specials;
	} else {
		for (i = 0; i + 1 < number; i++)
			before += (guint)s->seasons[i];
		count = (guint)s->seasons[number - 1];
	}
	g_string_append_printf(json, "{\"number\":%u,\"name\":\"%s %u\",\"episodes\":[", number,
		number == 0 ? "Specials" : "Season", number);
	for (e = 1; e <= count; e++) {
		g_autofree gchar *date = NULL;

		if (number == 0) {
			date = g_strdup("2016-12-24");
		} else if (shelf_number(s->id, TRUE) > 0) {
			GDate *day = g_date_new_dmy(1, G_DATE_JANUARY, 2001);
			gchar text[11];

			g_date_add_days(day, number * 100 + e * 7);
			g_snprintf(text, sizeof(text), "%04d-%02d-%02d", g_date_get_year(day), g_date_get_month(day),
				g_date_get_day(day));
			g_date_free(day);
			date = g_strdup(text);
		} else {
			date = aired(s, before + e - 1);
		}
		g_string_append_printf(json, "%s{\"number\":%u,\"id\":\"%s%02u%03u\",\"name\":\"Chapter %u\","
			"\"air_date\":\"%s\"", e > 1 ? "," : "", e, s->id, number, e, e, date);
		if (s->runtime > 0)
			g_string_append_printf(json, ",\"runtime\":%d", s->runtime);
		g_string_append_c(json, '}');
	}
	g_string_append(json, "]}");
}

/* What the provider says about series; %FALSE when the request is not one. */
static gboolean
series_answer(
	const AxisTestPlexRequest *request,
	GHashTable                *query,
	GString                   *json
){
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
	AxisTestSeries shelf;
	guint i;
	guint n = 0;

	if (g_str_has_prefix(request->path, "/search/series")) {
		const gchar *q = query != NULL ? g_hash_table_lookup(query, "q") : NULL;
		g_autofree gchar *want = q != NULL ? g_utf8_strdown(q, -1) : g_strdup("");
		guint number = shelf_number(q, FALSE);

		g_string_append(json, "{\"results\":[");
		if (number > 0) {
			shelf_series(number, &shelf, owned);
			append_series(json, &shelf, FALSE);
			n++;
		}
		for (i = 0; series_table[i].id != NULL; i++) {
			g_autofree gchar *title = g_utf8_strdown(series_table[i].title, -1);
			g_autofree gchar *other = series_table[i].other_name != NULL ?
				g_utf8_strdown(series_table[i].other_name, -1) : NULL;

			if (want[0] == '\0' || (strstr(title, want) == NULL && (other == NULL || strstr(other, want) == NULL)))
				continue;
			if (n++ > 0)
				g_string_append_c(json, ',');
			append_series(json, &series_table[i], FALSE);
		}
		g_string_append(json, "]}");
		return TRUE;
	}
	if (g_str_has_prefix(request->path, "/find/series")) {
		const gchar *scheme = query != NULL ? g_hash_table_lookup(query, "scheme") : NULL;
		const gchar *value = query != NULL ? g_hash_table_lookup(query, "value") : NULL;

		g_string_append(json, "{\"results\":[");
		for (i = 0; series_table[i].id != NULL && scheme != NULL && value != NULL; i++) {
			if ((strcmp(scheme, "tmdb") == 0 && strcmp(series_table[i].id, value) == 0) ||
			    (strcmp(scheme, "tvdb") == 0 && strcmp(series_table[i].tvdb, value) == 0) ||
			    (strcmp(scheme, "imdb") == 0 && strcmp(series_table[i].imdb, value) == 0)) {
				append_series(json, &series_table[i], FALSE);
				break;
			}
		}
		g_string_append(json, "]}");
		return TRUE;
	}
	if (g_str_has_prefix(request->path, "/title/series/")) {
		const gchar *rest = request->path + strlen("/title/series/");
		const gchar *slash = strchr(rest, '/');
		g_autofree gchar *id = slash != NULL ? g_strndup(rest, (gsize)(slash - rest)) : g_strdup(rest);
		const AxisTestSeries *s = NULL;
		guint64 number = 0;

		if (shelf_number(id, TRUE) > 0) {
			shelf_series(shelf_number(id, TRUE), &shelf, owned);
			s = &shelf;
		}
		for (i = 0; s == NULL && series_table[i].id != NULL; i++) {
			if (strcmp(series_table[i].id, id) == 0)
				s = &series_table[i];
		}
		if (s == NULL)
			return FALSE;
		if (slash == NULL) {
			append_series(json, s, TRUE);
			return TRUE;
		}
		if (!g_str_has_prefix(slash, "/season/") ||
		    !g_ascii_string_to_unsigned(slash + strlen("/season/"), 10, 0, 4, &number, NULL) ||
		    (number == 0 && s->specials == 0) || (number > 0 && s->seasons[number - 1] == 0))
			return FALSE;
		g_atomic_int_inc(&season_requests);
		append_season(json, s, (guint)number);
		return TRUE;
	}
	return FALSE;
}

static void
append_film(
	GString            *json,
	const AxisTestFilm *f,
	gboolean            full
){
	g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"%s\",\"year\":%d,"
		"\"poster\":\"/p%s.png\",\"ids\":{\"imdb\":\"%s\",\"tmdb\":\"%s\"}", f->id, f->title, f->year, f->id, f->imdb,
		f->id);
	if (full)
		g_string_append_printf(json, ",\"runtime\":%d,\"status\":\"released\",\"release_date\":\"%d-06-01\","
			"\"overview\":\"A film called %s.\"", f->runtime, f->year, f->title);
	g_string_append_c(json, '}');
}

static void
json_answer(
	AxisTestPlexAnswer *answer,
	GString            *json
){
	answer->status = 200;
	answer->content_type = g_strdup("application/json");
	answer->body = g_bytes_new(json->str, json->len);
}

void
axis_test_identify_provider(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	g_autoptr(GHashTable) query = request->query != NULL ?
		g_uri_parse_params(request->query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
	g_autoptr(GString) json = g_string_new(NULL);
	guint i;
	guint n = 0;

	if (series_answer(request, query, json)) {
		json_answer(answer, json);
		return;
	}
	g_string_truncate(json, 0);
	if (g_str_has_prefix(request->path, "/search/film")) {
		const gchar *q = query != NULL ? g_hash_table_lookup(query, "q") : NULL;
		g_autofree gchar *want = q != NULL ? g_utf8_strdown(q, -1) : g_strdup("");

		g_string_append(json, "{\"results\":[");
		for (i = 0; films[i].id != NULL; i++) {
			g_autofree gchar *title = g_utf8_strdown(films[i].title, -1);

			if (want[0] == '\0' || strstr(title, want) == NULL)
				continue;
			if (n++ > 0)
				g_string_append_c(json, ',');
			append_film(json, &films[i], FALSE);
		}
		g_string_append(json, "]}");
		json_answer(answer, json);
		return;
	}
	if (g_str_has_prefix(request->path, "/find/film")) {
		const gchar *scheme = query != NULL ? g_hash_table_lookup(query, "scheme") : NULL;
		const gchar *value = query != NULL ? g_hash_table_lookup(query, "value") : NULL;

		g_string_append(json, "{\"results\":[");
		for (i = 0; films[i].id != NULL && scheme != NULL && value != NULL; i++) {
			if ((strcmp(scheme, "tmdb") == 0 && strcmp(films[i].id, value) == 0) ||
			    (strcmp(scheme, "imdb") == 0 && strcmp(films[i].imdb, value) == 0)) {
				append_film(json, &films[i], FALSE);
				break;
			}
		}
		g_string_append(json, "]}");
		json_answer(answer, json);
		return;
	}
	if (g_str_has_prefix(request->path, "/title/film/")) {
		const gchar *id = request->path + strlen("/title/film/");

		for (i = 0; films[i].id != NULL; i++) {
			if (strcmp(films[i].id, id) == 0) {
				append_film(json, &films[i], TRUE);
				json_answer(answer, json);
				return;
			}
		}
	}
	answer->status = 404;
}

void
axis_test_identify_posters(AxisTestMetaFixture *fixture)
{
	g_autoptr(GBytes) png = axis_test_meta_png();
	guint i;

	for (i = 0; films[i].id != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/img/w185/p%s.png", films[i].id);

		axis_test_plex_route_full(fixture->images, path, 200, "image/png", png, NULL);
	}
	for (i = 0; series_table[i].id != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/img/w185/p%s.png", series_table[i].id);

		axis_test_plex_route_full(fixture->images, path, 200, "image/png", png, NULL);
	}
}
